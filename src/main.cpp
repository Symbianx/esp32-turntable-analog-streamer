#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include "AudioTools.h"

// Audio Tools I2S
I2SStream i2sStream;

// Ring buffer for WAV data
#define WAV_BUFFER_SIZE (64 * 1024) // 64KB ring buffer
RingBufferStream *wavBuffer = nullptr;
volatile bool wavClientConnected = false;

// Audio processing task
TaskHandle_t audioTaskHandle = nullptr;

// WiFi and Web Server
WiFiManager wifiManager;
AsyncWebServer server(80);

// Audio configuration
#define SAMPLE_RATE 48000
#define BITS_PER_SAMPLE 32 // I2S captures 32-bit
#define OUTPUT_BITS 16     // WAV output 16-bit
#define CHANNELS 2
#define AUDIO_BUFF_SIZE 4096 // Audio buffer size

/* ESP32 I2S Pins */
#define I2S_MCLK_PIN 0 // I2S master clock
#define I2S_BCK_PIN 26 // I2S bit clock
#define I2S_WS_PIN 25  // I2S word select
#define I2S_DIN_PIN 27 // I2S data in

// WAV header for 16-bit stereo 48kHz streaming
static const uint8_t wavHeader[44] = {
    'R', 'I', 'F', 'F',
    0xFF, 0xFF, 0xFF, 0x7F, // File size (max for streaming)
    'W', 'A', 'V', 'E',
    'f', 'm', 't', ' ',
    16, 0, 0, 0,            // Chunk size (16 for PCM)
    1, 0,                   // Audio format (1 = PCM)
    2, 0,                   // Channels (2 = stereo)
    0x80, 0xBB, 0x00, 0x00, // Sample rate (48000)
    0x00, 0xEE, 0x02, 0x00, // Byte rate (48000 * 2 * 2 = 192000)
    4, 0,                   // Block align (2 channels * 2 bytes)
    16, 0,                  // Bits per sample (16)
    'd', 'a', 't', 'a',
    0xFF, 0xFF, 0xFF, 0x7F // Data size (max for streaming)
};

// Function declarations
void i2s_setup();
void audio_setup();
void wifi_setup();
void webserver_setup();
void handleWavStreamRequest(AsyncWebServerRequest *request);
void handleRootRequest(AsyncWebServerRequest *request);

// Audio processing task - reads I2S and buffers WAV data
void audioTask(void *param)
{
  Serial.println("[AUDIO] Task started on core 0");

  uint8_t buffer[1024];

  while (true)
  {
    bool didWork = false;

    // Read from I2S (32-bit)
    size_t bytesRead = i2sStream.readBytes(buffer, sizeof(buffer));

    if (bytesRead > 0)
    {
      // Convert 32-bit to 16-bit from MSB-aligned 24-bit data
      int32_t *samples32 = (int32_t *)buffer;
      int16_t samples16[bytesRead / 4];
      size_t numSamples = bytesRead / 4;

      for (size_t i = 0; i < numSamples; i++)
      {
        // // PCM1808 with Philips I2S Standard: 24-bit left-aligned in 32-bit frame
        // // Try treating as signed 24-bit and properly converting to 16-bit
        // // First mask to 24 bits, then shift down and sign-extend
        // int32_t sample24 = samples32[i] & 0xFFFFFF00;  // Keep bits [31:8]
        // samples16[i] = (int16_t)(sample24 >> 16);      // Get top 16 bits

        int32_t sample32 = ((int32_t *)buffer)[i];
        samples16[i] = (int16_t)(sample32 >> 16);
        // If this sounds quiet/garbage, try: samples16[i] = (int16_t)sample32; 
      }

      // Write to WAV buffer if client connected and space available
      if (wavClientConnected && wavBuffer != nullptr)
      {
        int wavSpace = wavBuffer->availableForWrite();
        if (wavSpace >= (int)(numSamples * 2))
        {
          wavBuffer->write((uint8_t *)samples16, numSamples * 2);
          didWork = true;
        }
      }
    }

    // TODO: Is this causing audio issues?
    if (!didWork)
    {
      delay(5);
    }

    taskYIELD();
  }
}

void i2s_setup()
{
  Serial.println("Setting up I2S with AudioTools...");

  auto config = i2sStream.defaultConfig(RX_MODE);
  config.sample_rate = SAMPLE_RATE;
  config.bits_per_sample = BITS_PER_SAMPLE;
  config.channels = CHANNELS;
  config.i2s_format = I2S_STD_FORMAT;
  config.is_master = true;
  config.use_apll = true;
  config.buffer_count = 16;
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
}

void audio_setup()
{
  Serial.println("Setting up audio buffer...");

  // Allocate ring buffer for WAV output
  wavBuffer = new RingBufferStream(WAV_BUFFER_SIZE);
  Serial.println("WAV buffer ready (64KB)");
}

void wifi_setup()
{
  Serial.println("Setting up WiFi...");

  wifiManager.setConfigPortalTimeout(180);

  if (wifiManager.autoConnect("TurntableStreamer"))
  {
    Serial.println("WiFi connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  }
  else
  {
    Serial.println("WiFi failed - restarting...");
    delay(3000);
    ESP.restart();
  }

  if (MDNS.begin("turntable-streamer"))
  {
    Serial.println("mDNS: turntable-streamer.local");
    MDNS.addService("http", "tcp", 80);
  }
}

void handleRootRequest(AsyncWebServerRequest *request)
{
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'><title>Turntable Streamer</title>";
  html += "<style>body{font-family:Arial;max-width:600px;margin:50px auto;padding:20px;background:#f0f0f0;}";
  html += ".container{background:white;padding:30px;border-radius:10px;}";
  html += ".note{font-size:12px;color:#666;margin-top:10px;}</style></head><body>";
  html += "<div class='container'><h1>🎵 Turntable Streamer</h1>";
  html += "<h3>Audio Player:</h3>";
  html += "<audio controls src='/stream.wav' style='width:100%;'></audio>";
  html += "<h3>Stream URL:</h3>";
  html += "<p>WAV Stream: <a href='/stream.wav'>/stream.wav</a></p>";
  html += "<p class='note'>48kHz / 16-bit / Stereo</p>";
  html += "</div></body></html>";
  request->send(200, "text/html", html);
}

void handleWavStreamRequest(AsyncWebServerRequest *request)
{
  Serial.println("[WAV STREAM] New connection");

  // Clear old data
  wavClientConnected = false;
  delay(20);
  while (wavBuffer->available() > 0)
  {
    uint8_t dummy[256];
    wavBuffer->readBytes(dummy, min(256, wavBuffer->available()));
  }
  wavClientConnected = true;

  // Wait for some initial data
  int waitCount = 0;
  while (wavBuffer->available() < 2048 && waitCount < 100)
  {
    delay(10);
    waitCount++;
  }
  Serial.printf("[WAV STREAM] Buffer ready: %d bytes\n", wavBuffer->available());

  AsyncWebServerResponse *response = request->beginResponse("audio/wav", 0,
                                                            [](uint8_t *buffer, size_t maxLen, size_t index) -> size_t
                                                            {
                                                              // Send WAV header first
                                                              if (index == 0)
                                                              {
                                                                if (maxLen >= 44)
                                                                {
                                                                  memcpy(buffer, wavHeader, 44);
                                                                  return 44;
                                                                }
                                                                return 0;
                                                              }

                                                              // Read from WAV ring buffer
                                                              if (wavBuffer == nullptr)
                                                                return 0;

                                                              int available = wavBuffer->available();
                                                              if (available <= 0)
                                                              {
                                                                // Wait a bit for data
                                                                int retries = 0;
                                                                while (available <= 0 && retries < 20)
                                                                {
                                                                  delay(5);
                                                                  available = wavBuffer->available();
                                                                  retries++;
                                                                }
                                                                if (available <= 0)
                                                                  return 0;
                                                              }

                                                              size_t toRead = min((size_t)available, maxLen);
                                                              return wavBuffer->readBytes(buffer, toRead);
                                                            });

  response->addHeader("Cache-Control", "no-cache, no-store");
  response->addHeader("Connection", "keep-alive");
  response->addHeader("Accept-Ranges", "none");
  request->send(response);
}

void webserver_setup()
{
  Serial.println("Setting up web server...");

  server.on("/", HTTP_GET, handleRootRequest);
  server.on("/stream.wav", HTTP_GET, handleWavStreamRequest);

  server.begin();
  Serial.println("Web server started");
  Serial.print("WAV Stream: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/stream.wav");
}

void setup()
{
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== Turntable Audio Streamer ===");

  // Check PSRAM
  if (psramFound())
  {
    Serial.printf("PSRAM: %d bytes available\n", ESP.getPsramSize());
  }
  else
  {
    Serial.println("Warning: No PSRAM found");
  }

  i2s_setup();
  audio_setup();
  wifi_setup();
  delay(500);
  webserver_setup();

  // Start audio processing task on core 0
  xTaskCreatePinnedToCore(
      audioTask,
      "AudioTask",
      8192,
      nullptr,
      3,
      &audioTaskHandle,
      1);

  Serial.println("Setup complete!");
}

void loop()
{
  delay(1000);
}