#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include "AudioTools.h"

// Audio Tools I2S
I2SStream i2sStream;

// WiFi and Web Server
WiFiManager wifiManager;
AsyncWebServer server(80);

// Ring buffer for smooth streaming
#define RING_BUFFER_SIZE      32768           // 32KB ring buffer (~170ms at 48kHz stereo 16-bit)
static uint8_t ringBuffer[RING_BUFFER_SIZE];
static volatile size_t ringWritePos = 0;
static volatile size_t ringReadPos = 0;
static volatile size_t ringAvailable = 0;
static SemaphoreHandle_t ringMutex = NULL;

// Audio configuration
// PCM1808 sends 24-bit audio left-justified in 32-bit I2S slots
// Format: [23:0 = 24-bit audio][7:0 = padding zeros]
#define SAMPLE_RATE           48000
#define BITS_PER_SAMPLE       32              // I2S slot size (PCM1808 uses 32-bit slots for 24-bit data)
#define OUTPUT_BITS           16              // WAV output 16-bit (top 16 bits of 24-bit audio)
#define CHANNELS              2
#define AUDIO_BUFF_SIZE       2048            // I2S read buffer size

/* ESP32 I2S Pins */
#define I2S_MCLK_PIN          0               // I2S master clock
#define I2S_BCK_PIN           26              // I2S bit clock
#define I2S_WS_PIN            25              // I2S word select
#define I2S_DIN_PIN           27              // I2S data in

// WAV header for 16-bit stereo 48kHz streaming
static const uint8_t wavHeader[44] = {
  'R','I','F','F',
  0xFF,0xFF,0xFF,0x7F,          // File size (max for streaming)
  'W','A','V','E',
  'f','m','t',' ',
  16,0,0,0,                     // Chunk size (16 for PCM)
  1,0,                          // Audio format (1 = PCM)
  2,0,                          // Channels (2 = stereo)
  0x80,0xBB,0x00,0x00,          // Sample rate (48000)
  0x00,0xEE,0x02,0x00,          // Byte rate (48000 * 2 * 2 = 192000)
  4,0,                          // Block align (2 channels * 2 bytes)
  16,0,                         // Bits per sample (16)
  'd','a','t','a',
  0xFF,0xFF,0xFF,0x7F           // Data size (max for streaming)
};

// Function declarations
void i2s_setup();
void wifi_setup();
void webserver_setup();
void handleStreamRequest(AsyncWebServerRequest *request);
void handleRootRequest(AsyncWebServerRequest *request);
void audioTask(void* param);

// Write 16-bit converted audio to ring buffer
void ringBufferWrite(const uint8_t* data, size_t len) {
  xSemaphoreTake(ringMutex, portMAX_DELAY);
  for (size_t i = 0; i < len; i++) {
    if (ringAvailable < RING_BUFFER_SIZE) {
      ringBuffer[ringWritePos] = data[i];
      ringWritePos = (ringWritePos + 1) % RING_BUFFER_SIZE;
      ringAvailable++;
    }
  }
  xSemaphoreGive(ringMutex);
}

// Read from ring buffer
size_t ringBufferRead(uint8_t* data, size_t maxLen) {
  xSemaphoreTake(ringMutex, portMAX_DELAY);
  size_t avail = ringAvailable;
  size_t toRead = (maxLen < avail) ? maxLen : avail;
  for (size_t i = 0; i < toRead; i++) {
    data[i] = ringBuffer[ringReadPos];
    ringReadPos = (ringReadPos + 1) % RING_BUFFER_SIZE;
  }
  ringAvailable -= toRead;
  xSemaphoreGive(ringMutex);
  return toRead;
}

// Background task to continuously read I2S and fill ring buffer
void audioTask(void* param) {
  static uint8_t i2sBuf[AUDIO_BUFF_SIZE];
  static uint8_t outBuf[AUDIO_BUFF_SIZE / 2];  // 16-bit output is half size
  
  Serial.println("[AUDIO] Task started");
  
  while (true) {
    // Read 32-bit I2S data
    size_t bytesRead = i2sStream.readBytes(i2sBuf, AUDIO_BUFF_SIZE);
    
    if (bytesRead > 0) {
      // Ensure alignment to stereo frame (8 bytes for 32-bit stereo)
      bytesRead = (bytesRead / 8) * 8;
      
      // Convert 32-bit to 16-bit
      int32_t* samples32 = (int32_t*)i2sBuf;
      int16_t* samples16 = (int16_t*)outBuf;
      size_t numSamples = bytesRead / 4;
      
      for (size_t i = 0; i < numSamples; i++) {
        samples16[i] = (int16_t)(samples32[i] >> 16);
      }
      
      // Write to ring buffer
      ringBufferWrite(outBuf, numSamples * 2);
    }
    
    // Small delay to prevent task starvation
    vTaskDelay(1);
  }
}

void i2s_setup() {
  Serial.println("Setting up I2S with AudioTools...");
  
  // Create mutex for ring buffer
  ringMutex = xSemaphoreCreateMutex();
  
  auto config = i2sStream.defaultConfig(RX_MODE);
  config.sample_rate = SAMPLE_RATE;
  config.bits_per_sample = BITS_PER_SAMPLE;
  config.channels = CHANNELS;
  config.i2s_format = I2S_STD_FORMAT;  // Standard Philips I2S
  config.is_master = true;
  config.use_apll = true;
  config.buffer_count = 8;
  config.buffer_size = 1024;
  
  config.pin_mck = I2S_MCLK_PIN;
  config.pin_bck = I2S_BCK_PIN;
  config.pin_ws = I2S_WS_PIN;
  config.pin_data = I2S_DIN_PIN;
  
  i2sStream.begin(config);
  
  Serial.printf("I2S: MCLK=%d, BCK=%d, WS=%d, DIN=%d\n", 
    I2S_MCLK_PIN, I2S_BCK_PIN, I2S_WS_PIN, I2S_DIN_PIN);
  Serial.printf("Sample Rate: %d Hz, %d-bit -> %d-bit\n", 
    SAMPLE_RATE, BITS_PER_SAMPLE, OUTPUT_BITS);
  
  // Test read and print sample data to verify format
  delay(100);
  uint8_t testBuf[32];
  size_t read = i2sStream.readBytes(testBuf, 32);
  if (read > 0) {
    Serial.print("I2S test data: ");
    for (int i = 0; i < min((int)read, 16); i++) {
      Serial.printf("%02X ", testBuf[i]);
    }
    Serial.println();
    
    // Print as 32-bit samples
    int32_t* samples = (int32_t*)testBuf;
    Serial.printf("As int32: %08X %08X %08X %08X\n", 
      samples[0], samples[1], samples[2], samples[3]);
  }
}

void wifi_setup() {
  Serial.println("Setting up WiFi...");
  
  wifiManager.setConfigPortalTimeout(180);
  
  if (wifiManager.autoConnect("TurntableStreamer")) {
    Serial.println("WiFi connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi failed - restarting...");
    delay(3000);
    ESP.restart();
  }
  
  if (MDNS.begin("turntable-streamer")) {
    Serial.println("mDNS: turntable-streamer.local");
    MDNS.addService("http", "tcp", 80);
  }
}

void handleRootRequest(AsyncWebServerRequest *request) {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'><title>Turntable Streamer</title>";
  html += "<style>body{font-family:Arial;max-width:600px;margin:50px auto;padding:20px;background:#f0f0f0;}";
  html += ".container{background:white;padding:30px;border-radius:10px;}</style></head><body>";
  html += "<div class='container'><h1>Turntable Streamer</h1>";
  html += "<p>Stream: <a href='/stream.wav'>/stream.wav</a></p>";
  html += "<p>48kHz / 16-bit / Stereo</p>";
  html += "<audio controls src='/stream.wav' style='width:100%;'></audio>";
  html += "</div></body></html>";
  request->send(200, "text/html", html);
}

void handleStreamRequest(AsyncWebServerRequest *request) {
  Serial.println("[STREAM] New connection");
  Serial.printf("[STREAM] Buffer level: %d bytes\n", ringAvailable);
  
  AsyncWebServerResponse *response = request->beginResponse("audio/wav", 0, 
    [](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
      // Send WAV header first
      if (index == 0) {
        if (maxLen >= 44) {
          memcpy(buffer, wavHeader, 44);
          Serial.println("[STREAM] Sent WAV header");
          return 44;
        }
        return 0;
      }
      
      // Read pre-converted 16-bit data from ring buffer
      size_t toRead = (maxLen / 4) * 4;  // Align to stereo frame
      
      // Wait for data if buffer is empty (up to 100ms)
      int attempts = 0;
      while (ringAvailable < toRead && attempts < 10) {
        delay(10);
        attempts++;
      }
      
      size_t bytesRead = ringBufferRead(buffer, toRead);
      
      // If still no data, send silence to keep stream alive
      if (bytesRead == 0) {
        size_t silenceSize = (maxLen / 4) * 4;
        if (silenceSize > 1024) silenceSize = 1024;
        memset(buffer, 0, silenceSize);
        return silenceSize;
      }
      
      return bytesRead;
    }
  );
  
  response->addHeader("Cache-Control", "no-cache");
  response->addHeader("Connection", "keep-alive");
  response->addHeader("Transfer-Encoding", "chunked");
  request->send(response);
}

void webserver_setup() {
  Serial.println("Setting up web server...");
  
  server.on("/", HTTP_GET, handleRootRequest);
  server.on("/stream.wav", HTTP_GET, handleStreamRequest);
  
  server.begin();
  Serial.println("Web server started");
  Serial.print("Stream: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/stream.wav");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== Turntable Audio Streamer ===");
  
  i2s_setup();
  wifi_setup();
  delay(500);
  webserver_setup();
  
  // Start background audio task on core 0 (WiFi runs on core 0, but audio is higher priority)
  xTaskCreatePinnedToCore(
    audioTask,
    "AudioTask",
    4096,
    NULL,
    5,              // Higher priority than idle
    NULL,
    1              // Run on core 1 (separate from WiFi)
  );
  
  Serial.println("Setup complete!");
}

void loop() {
  // Print buffer status periodically
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 5000) {
    Serial.printf("[STATUS] Ring buffer: %d / %d bytes\n", ringAvailable, RING_BUFFER_SIZE);
    lastPrint = millis();
  }
  delay(100);
}