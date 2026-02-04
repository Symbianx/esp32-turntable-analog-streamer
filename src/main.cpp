#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include "driver/i2s_std.h"
#include "driver/gpio.h"

// I2S handle
i2s_chan_handle_t rx_handle;

// WiFi and Web Server
WiFiManager wifiManager;
AsyncWebServer server(80);

// Audio configuration
#define SAMPLE_RATE           48000
#define BITS_PER_SAMPLE       32              // I2S captures 32-bit
#define OUTPUT_BITS           16              // WAV output 16-bit
#define CHANNELS              2
#define AUDIO_BUFF_SIZE       4096            // Audio buffer size

/* ESP32 I2S Pins */
#define I2S_STD_MCLK_IO1        GPIO_NUM_0      // I2S master clock
#define I2S_STD_BCLK_IO1        GPIO_NUM_26     // I2S bit clock
#define I2S_STD_WS_IO1          GPIO_NUM_25     // I2S word select
#define I2S_STD_DIN_IO1         GPIO_NUM_27     // I2S data in

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

void i2s_setup() {
  Serial.println("Setting up I2S...");
  
  // Allocate RX channel only
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
  Serial.printf("Init I2S Channel: 0x%x\n", err);
  
  // Configure I2S standard mode
  i2s_std_config_t std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = I2S_STD_MCLK_IO1,
      .bclk = I2S_STD_BCLK_IO1,
      .ws = I2S_STD_WS_IO1,
      .dout = I2S_GPIO_UNUSED,
      .din = I2S_STD_DIN_IO1,
      .invert_flags = {
        .mclk_inv = false,
        .bclk_inv = false,
        .ws_inv = false,
      },
    },
  };
  std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  
  err = i2s_channel_init_std_mode(rx_handle, &std_cfg);
  Serial.printf("Init I2S RX: 0x%x\n", err);
  
  err = i2s_channel_enable(rx_handle);
  Serial.printf("Enable I2S RX: 0x%x\n", err);
  
  Serial.printf("I2S: MCLK=%d, BCK=%d, WS=%d, DIN=%d\n", 
    I2S_STD_MCLK_IO1, I2S_STD_BCLK_IO1, I2S_STD_WS_IO1, I2S_STD_DIN_IO1);
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
      
      // Read 32-bit I2S data
      static uint8_t i2sBuf[AUDIO_BUFF_SIZE];
      size_t bytesRead = 0;
      
      esp_err_t err = i2s_channel_read(rx_handle, i2sBuf, 
        min((size_t)AUDIO_BUFF_SIZE, maxLen * 2), &bytesRead, 100);
      
      if (err != ESP_OK || bytesRead == 0) {
        return 0;
      }
      
      // Convert 32-bit to 16-bit
      int32_t* samples32 = (int32_t*)i2sBuf;
      int16_t* samples16 = (int16_t*)buffer;
      size_t numSamples = bytesRead / 4;
      
      for (size_t i = 0; i < numSamples; i++) {
        samples16[i] = (int16_t)(samples32[i] >> 16);
      }
      
      return numSamples * 2;
    }
  );
  
  response->addHeader("Cache-Control", "no-cache");
  response->addHeader("Connection", "keep-alive");
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
  
  Serial.println("Setup complete!");
}

void loop() {
  delay(1000);
}