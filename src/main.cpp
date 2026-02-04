#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <ESPAsyncWebServer.h>
#include "AudioTools.h"
#include "config.h"

// Global objects
WiFiManager wifiManager;
AsyncWebServer server(80);
I2SStream i2sStream;
FormatConverterStream convStream(i2sStream);  // Converts 32-bit to 16-bit

// Audio streaming buffer
const size_t AUDIO_BUFFER_SIZE = 4096;
uint8_t audioBuffer[AUDIO_BUFFER_SIZE];
AsyncWebServerRequest* activeStreamRequest = nullptr;
bool isStreaming = false;

// Volume monitoring
float currentVolume = 0.0;
unsigned long lastVolumeUpdate = 0;
const unsigned long VOLUME_UPDATE_INTERVAL = 100; // Update every 100ms

// WiFi status
bool wifiConnected = false;
unsigned long lastWiFiRetry = 0;
const unsigned long WIFI_RETRY_INTERVAL = 10000; // 10 seconds

// Function declarations
void setupWiFi();
void setupOTA();
void setupI2S();
void setupWebServer();
void handleStreamRequest(AsyncWebServerRequest *request);
void handleRootRequest(AsyncWebServerRequest *request);
void handleResetWiFi(AsyncWebServerRequest *request);
void checkWiFiConnection();
void updateLED();

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n=== Turntable Audio Streamer ===");
  
  // Setup LED
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);
  
  // Setup WiFi
  setupWiFi();
  
  // Setup OTA
  setupOTA();
  
  // Setup I2S audio capture
  setupI2S();
  
  // Wait for network stack to be fully ready
  delay(1000);
  
  // Setup web server
  setupWebServer();
  
  Serial.println("Setup complete!");
  Serial.print("Stream URL: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/stream.wav");
}

void loop() {
  static unsigned long lastHeartbeat = 0;
  unsigned long now = millis();
  
  // Heartbeat every 5 seconds
  if (now - lastHeartbeat > 5000) {
    Serial.printf("[LOOP] Heartbeat: uptime=%ds, free_heap=%d\n", now/1000, ESP.getFreeHeap());
    lastHeartbeat = now;
  }
  
  // Handle OTA updates
  ArduinoOTA.handle();
  
  // Check WiFi connection and auto-reconnect
  checkWiFiConnection();
  
  // Update LED status
  updateLED();
  
  delay(100);
}

void setupWiFi() {
  Serial.println("Setting up WiFi...");
  
  // Set WiFiManager timeout to 180 seconds
  wifiManager.setConfigPortalTimeout(180);
  
  // Try to connect with saved credentials
  wifiManager.setAPStaticIPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  
  // Start WiFiManager
  if (wifiManager.autoConnect(AP_SSID, AP_PASSWORD)) {
    Serial.println("WiFi connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    wifiConnected = true;
  } else {
    Serial.println("Failed to connect - restarting...");
    delay(3000);
    ESP.restart();
  }
}

void checkWiFiConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    wifiConnected = false;
    
    // Try to reconnect every 10 seconds
    if (millis() - lastWiFiRetry > WIFI_RETRY_INTERVAL) {
      Serial.println("WiFi disconnected. Attempting to reconnect...");
      WiFi.reconnect();
      lastWiFiRetry = millis();
    }
  } else {
    if (!wifiConnected) {
      Serial.println("WiFi reconnected!");
      Serial.print("IP Address: ");
      Serial.println(WiFi.localIP());
      wifiConnected = true;
    }
  }
}

void setupOTA() {
  Serial.println("Setting up OTA...");
  
  // Set hostname
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  
  // Set password
  ArduinoOTA.setPassword(OTA_PASSWORD);
  
  // OTA callbacks
  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    Serial.println("Start updating " + type);
  });
  
  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA Update Complete!");
  });
  
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  
  ArduinoOTA.begin();
  
  // Start mDNS
  if (MDNS.begin(OTA_HOSTNAME)) {
    Serial.printf("mDNS responder started: %s.local\n", OTA_HOSTNAME);
    MDNS.addService("http", "tcp", 80);
  }
  
  Serial.println("OTA ready");
}

void setupI2S() {
  Serial.println("Setting up I2S...");
  
  // Configure I2S for master mode (ESP32 generates clocks for PCM1808)
  auto config = i2sStream.defaultConfig(RX_MODE);
  config.sample_rate = SAMPLE_RATE;
  config.bits_per_sample = BITS_PER_SAMPLE; // PCM1808 outputs 24-bit
  config.channels = CHANNELS;
  config.i2s_format = I2S_STD_FORMAT; // Standard I2S format
  config.is_master = true; // Master mode - ESP32 generates BCK and WS clocks
  config.use_apll = true;
  config.buffer_count = 8;
  config.buffer_size = 1024;

  config.pin_bck = I2S_BCK_PIN;
  config.pin_ws = I2S_LRC_PIN;
  config.pin_data = I2S_OUT_PIN;
  config.pin_mck = I2S_MCLK_PIN;

  
  Serial.printf("I2S Pins: BCK=%d, WS=%d, DATA=%d, MCLK=%d\n", 
    I2S_BCK_PIN, I2S_LRC_PIN, I2S_OUT_PIN, I2S_MCLK_PIN);
  
  // Start I2S stream
  i2sStream.begin(config);
  
  // Test if I2S is actually receiving data
  Serial.println("Testing I2S data reception...");
  delay(1000); // Wait for PCM to stabilize
  
  int available = i2sStream.available();
  Serial.printf("I2S available bytes: %d\n", available);
  
  if (available > 0) {
    Serial.println("Attempting to read test data (with timeout)...");
    uint8_t testBuffer[32];
    size_t bytesRead = 0;
    unsigned long startTime = millis();
    
    // Try to read with timeout
    while (bytesRead < 32 && (millis() - startTime) < 2000) {
      if (i2sStream.available() > 0) {
        int byte = i2sStream.read();
        if (byte >= 0) {
          testBuffer[bytesRead++] = byte;
        }
      }
      delay(1);
    }
    
    Serial.printf("Read %d bytes in %dms\n", bytesRead, millis() - startTime);
    
    if (bytesRead > 0) {
      Serial.print("Sample data: ");
      bool allZeros = true;
      for (size_t i = 0; i < min(bytesRead, (size_t)16); i++) {
        Serial.printf("%02X ", testBuffer[i]);
        if (testBuffer[i] != 0) allZeros = false;
      }
      Serial.println();
      
      if (allZeros) {
        Serial.println("WARNING: All zeros detected!");
        Serial.println("Possible causes:");
        Serial.println("  1. No audio input connected to PCM1808");
        Serial.println("  2. PCM1808 FMT pins incorrectly configured");
        Serial.println("  3. PCM1808 MCLK/SCK not receiving clock (check GPIO0)");
        Serial.println("  4. PCM1808 in power-down or reset mode");
        Serial.println("Connect an audio source and check the PCM1808 wiring.");
      }
    } else {
      Serial.println("WARNING: I2S reports data available but cannot read it!");
      Serial.println("Check: 1) PCM1808 power  2) Clock signals  3) Pin connections");
    }
  } else {
    Serial.println("WARNING: No I2S data available after 1 second");
    Serial.println("Possible issues:");
    Serial.println("  - PCM1808 not powered");
    Serial.println("  - No clock signals from PCM1808");
    Serial.println("  - BCK/WS pins not connected");
    Serial.println("  - PCM1808 not in master mode");
  }
  
  // Configure format converter: 32-bit I2S input -> 16-bit output
  AudioInfo fromInfo(SAMPLE_RATE, CHANNELS, BITS_PER_SAMPLE);  // 32-bit from I2S
  AudioInfo toInfo(SAMPLE_RATE, CHANNELS, 16);                  // 16-bit output
  convStream.begin(fromInfo, toInfo);
  
  Serial.println("I2S configured in master mode");
  Serial.println("Format converter: 32-bit -> 16-bit");
  Serial.printf("Sample Rate: %d Hz\n", SAMPLE_RATE);
  Serial.printf("Bit Depth: %d-bit (capturing 24-bit, outputting 16-bit)\n", BITS_PER_SAMPLE);
  Serial.printf("Channels: %d\n", CHANNELS);
}

void setupWebServer() {
  Serial.println("Setting up web server...");
  
  // Root endpoint - web interface
  server.on("/", HTTP_GET, handleRootRequest);
  
  // Stream endpoint
  server.on("/stream.wav", HTTP_GET, handleStreamRequest);
  
  // Info endpoint
  server.on("/info", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{";
    json += "\"sample_rate\":" + String(SAMPLE_RATE) + ",";
    json += "\"bit_depth\":" + String(BITS_PER_SAMPLE) + ",";
    json += "\"channels\":" + String(CHANNELS) + ",";
    json += "\"format\":\"WAV (PCM)\"";
    json += "}";
    request->send(200, "application/json", json);
  });
  
  // Volume endpoint
  server.on("/volume", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{\"volume\":" + String(currentVolume, 1) + "}";
    request->send(200, "application/json", json);
  });
  
  // WiFi reset endpoint
  server.on("/reset", HTTP_GET, handleResetWiFi);
  
  server.begin();
  Serial.println("Web server started");
}

void handleStreamRequest(AsyncWebServerRequest *request) {
  Serial.println("[STREAM] Request received");
  
  // WAV header for 16-bit stereo at 48kHz
  static const uint8_t wavHeader[44] = {
    'R','I','F','F',
    0xFF,0xFF,0xFF,0x7F,  // File size (max for streaming)
    'W','A','V','E',
    'f','m','t',' ',
    16,0,0,0,             // Chunk size (16 for PCM)
    1,0,                  // Audio format (1 = PCM)
    2,0,                  // Channels (2 = stereo)
    0x80,0xBB,0x00,0x00,  // Sample rate (48000)
    0x00,0xEE,0x02,0x00,  // Byte rate (48000 * 2 * 2 = 192000)
    4,0,                  // Block align (2 channels * 2 bytes)
    16,0,                 // Bits per sample (16)
    'd','a','t','a',
    0xFF,0xFF,0xFF,0x7F   // Data size (max for streaming)
  };
  
  // Send WAV stream with header
  AsyncWebServerResponse *response = request->beginResponse("audio/wav", 0, [](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
    static unsigned long lastLog = 0;
    unsigned long now = millis();
    
    // Log every 2 seconds
    if (now - lastLog > 2000) {
      Serial.printf("[STREAM] index=%zu, maxLen=%zu\n", index, maxLen);
      lastLog = now;
    }
    
    // Send WAV header at the start
    if (index == 0) {
      if (maxLen >= 44) {
        memcpy(buffer, wavHeader, 44);
        Serial.println("[STREAM] Sent WAV header (44 bytes)");
        return 44;
      }
    }
    
    // Read already-converted 16-bit data from FormatConverterStream
    int available = convStream.available();
    if (available <= 0) {
      yield();
      return 0;
    }
    
    size_t toRead = min((size_t)available, maxLen);
    size_t bytesRead = convStream.readBytes(buffer, toRead);
    
    // Calculate volume from 16-bit samples
    if (bytesRead >= 4 && now - lastVolumeUpdate > VOLUME_UPDATE_INTERVAL) {
      int16_t* samples = (int16_t*)buffer;
      size_t numSamples = bytesRead / 2;
      long sum = 0;
      for (size_t i = 0; i < numSamples; i++) {
        sum += abs(samples[i]);
      }
      if (numSamples > 0) {
        currentVolume = (sum / (float)numSamples) / 32768.0 * 100.0;
        lastVolumeUpdate = now;
      }
    }
    
    yield();
    return bytesRead;
  });
  
  response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  response->addHeader("Pragma", "no-cache");
  response->addHeader("Connection", "keep-alive");
  response->addHeader("Accept-Ranges", "none");
  request->send(response);
}

void handleRootRequest(AsyncWebServerRequest *request) {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Turntable Streamer</title>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; max-width: 600px; margin: 50px auto; padding: 20px; background: #f0f0f0; }";
  html += ".container { background: white; padding: 30px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
  html += "h1 { color: #333; margin-top: 0; }";
  html += ".info { background: #e8f4f8; padding: 15px; border-radius: 5px; margin: 15px 0; }";
  html += ".info p { margin: 5px 0; }";
  html += ".stream-url { background: #f5f5f5; padding: 10px; border-radius: 5px; word-break: break-all; font-family: monospace; }";
  html += "button { background: #4CAF50; color: white; padding: 12px 24px; border: none; border-radius: 5px; cursor: pointer; font-size: 16px; margin: 5px; }";
  html += "button:hover { background: #45a049; }";
  html += ".danger { background: #f44336; }";
  html += ".danger:hover { background: #da190b; }";
  html += ".copy-btn { background: #2196F3; }";
  html += ".copy-btn:hover { background: #0b7dda; }";
  html += ".meter{background:#eee;height:20px;border-radius:10px;overflow:hidden;margin:10px 0}";
  html += ".meter-bar{background:linear-gradient(90deg,#4CAF50,#FFC107,#f44336);height:100%;transition:width 0.3s}";
  html += "</style></head><body>";
  html += "<div class='container'>";
  html += "<h1>🎵 Turntable Audio Streamer</h1>";
  html += "<div class='info'>";
  html += "<p><strong>Status:</strong> " + String(wifiConnected ? "✓ Connected" : "✗ Disconnected") + "</p>";
  html += "<p><strong>IP Address:</strong> " + WiFi.localIP().toString() + "</p>";
  html += "<p><strong>WiFi Network:</strong> " + WiFi.SSID() + "</p>";
  html += "<p><strong>Hostname:</strong> " + String(OTA_HOSTNAME) + ".local</p>";
  html += "<p><strong>Volume:</strong> <span id='vol'>--</span>%</p>";
  html += "<div class='meter'><div class='meter-bar' id='bar' style='width:0%'></div></div>";
  html += "</div>";
  html += "<h2>Audio Stream</h2>";
  html += "<p>Use this URL in your Sonos app or other DLNA player:</p>";
  html += "<div class='stream-url' id='streamUrl'>http://" + WiFi.localIP().toString() + "/stream.wav</div>";
  html += "<button class='copy-btn' onclick='copyStream()'>📋 Copy Stream URL</button>";
  html += "<h2>Configuration</h2>";
  html += "<p><strong>Sample Rate:</strong> " + String(SAMPLE_RATE) + " Hz</p>";
  html += "<p><strong>Bit Depth:</strong> " + String(BITS_PER_SAMPLE) + "-bit</p>";
  html += "<p><strong>Channels:</strong> Stereo</p>";
  html += "<h2>Actions</h2>";
  html += "<button onclick='location.href=\"/info\"'>ℹ️ View Audio Info</button>";
  html += "<button class='danger' onclick='resetWiFi()'>🔄 Reset WiFi</button>";
  html += "</div>";
  html += "<script>";
  html += "function copyStream() {";
  html += "  const url = document.getElementById('streamUrl').textContent;";
  html += "  navigator.clipboard.writeText(url).then(() => alert('Stream URL copied to clipboard!'));";
  html += "}";
  html += "function resetWiFi() {";
  html += "  if(confirm('This will clear WiFi credentials and restart in AP mode. Continue?')) {";
  html += "    location.href='/reset';";
  html += "  }";
  html += "}";
  html += "setInterval(()=>fetch('/volume').then(r=>r.json()).then(d=>{document.getElementById('vol').textContent=d.volume.toFixed(1);document.getElementById('bar').style.width=Math.min(d.volume,100)+'%'}),500)";
  html += "</script>";
  html += "</body></html>";
  
  request->send(200, "text/html", html);
}

void handleResetWiFi(AsyncWebServerRequest *request) {
  request->send(200, "text/html", 
    "<html><body><h1>Resetting WiFi...</h1><p>Device will restart in AP mode.</p></body></html>");
  
  delay(1000);
  wifiManager.resetSettings();
  delay(1000);
  ESP.restart();
}

void updateLED() {
  static unsigned long lastBlink = 0;
  static bool ledState = false;
  
  if (!wifiConnected) {
    // Fast blink when disconnected
    if (millis() - lastBlink > 250) {
      ledState = !ledState;
      digitalWrite(STATUS_LED_PIN, ledState);
      lastBlink = millis();
    }
  } else {
    // Solid when connected
    digitalWrite(STATUS_LED_PIN, HIGH);
  }
}
