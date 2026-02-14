#include <esp_log.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "audio/i2s_master.h"
#include "audio/audio_buffer.h"
#include "audio/audio_capture.h"
#include "network/wifi_manager.h"
#include "network/config_portal.h"
#include "network/http_server.h"
#include "storage/nvs_config.h"
#include "config_schema.h"

static const char* TAG = "main";

extern "C" void app_main() {
    ESP_LOGI(TAG, "=== ESP32 PCM1808 HTTP Audio Streamer ===");
    
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Initialize WiFi Manager
    if (!WiFiManager::init()) {
        ESP_LOGE(TAG, "WiFi Manager initialization failed");
        return;
    }
    
    // Try to load saved credentials and connect
    DeviceConfig config;
    bool has_config = NVSConfig::load(&config);
    
    if (has_config && strlen(config.wifi_ssid) > 0) {
        ESP_LOGI(TAG, "Found saved WiFi credentials for: %s", config.wifi_ssid);
        ESP_LOGI(TAG, "Attempting to connect...");
        
        if (WiFiManager::connect_sta(config.wifi_ssid, config.wifi_password)) {
            char ip[16];
            WiFiManager::get_ip_address(ip, sizeof(ip));
            ESP_LOGI(TAG, "Connected! IP: %s, RSSI: %d dBm", 
                     ip, WiFiManager::get_rssi());
        } else {
            ESP_LOGW(TAG, "Failed to connect with saved credentials");
        }
    }
    
    // If still not connected, start configuration portal
    if (!WiFiManager::is_connected()) {
        ESP_LOGI(TAG, "Starting configuration portal...");
        
        if (!WiFiManager::start_ap("ESP32-Audio-Streamer")) {
            ESP_LOGE(TAG, "Failed to start AP");
            return;
        }
        
        // Initialize HTTP server for config portal (port 80)
        uint32_t sample_rate = 48000;  // Default sample rate
        if (!HTTPServer::init(80, sample_rate)) {
            ESP_LOGE(TAG, "Failed to initialize HTTP server");
            return;
        }
        
        // Initialize config portal (uses existing HTTP server)
        if (!ConfigPortal::init(HTTPServer::get_server_handle())) {
            ESP_LOGE(TAG, "Config portal initialization failed");
            return;
        }
        
        ESP_LOGI(TAG, "=== Configuration Portal Active ===");
        ESP_LOGI(TAG, "Connect to WiFi: ESP32-Audio-Streamer");
        ESP_LOGI(TAG, "Navigate to: http://192.168.4.1/config");
        
        // Wait for configuration
        while (!WiFiManager::is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        
        char ip[16];
        WiFiManager::get_ip_address(ip, sizeof(ip));
        ESP_LOGI(TAG, "Configuration complete! IP: %s", ip);
        
        // Stop config portal and restart for normal operation
        ESP_LOGI(TAG, "Restarting to apply new configuration...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
    
    // At this point we're connected to WiFi - initialize services
    ESP_LOGI(TAG, "WiFi connected, starting services...");
    
    // Load sample rate from config
    DeviceConfig loaded_config;
    uint32_t sample_rate = 48000;  // Default
    if (NVSConfig::load(&loaded_config)) {
        sample_rate = loaded_config.sample_rate;
    }
    
    // Initialize HTTP server for streaming (port 80)
    if (!HTTPServer::init(80, sample_rate)) {
        ESP_LOGE(TAG, "Failed to initialize HTTP server");
        return;
    }
    ESP_LOGI(TAG, "HTTP server started");
    
    // Register config portal routes (for / redirect to /status in STA mode)
    ConfigPortal::init(HTTPServer::get_server_handle());
    
    // Initialize audio buffer
    if (!AudioBuffer::init()) {
        ESP_LOGE(TAG, "Failed to initialize audio buffer");
        return;
    }
    ESP_LOGI(TAG, "Audio buffer initialized");
    
    // Initialize I²S master
    ESP_LOGI(TAG, "Initializing I²S at %lu Hz", sample_rate);
    
    if (!I2SMaster::init(sample_rate)) {
        ESP_LOGE(TAG, "I²S master initialization failed");
        return;
    }
    ESP_LOGI(TAG, "I²S master initialized");
    
    // Start audio capture task (will internally start I²S reception)
    if (!AudioCapture::start()) {
        ESP_LOGE(TAG, "Failed to start audio capture task");
        return;
    }
    ESP_LOGI(TAG, "Audio capture task started");
    
    char ip[16];
    WiFiManager::get_ip_address(ip, sizeof(ip));
    ESP_LOGI(TAG, "=== System Ready ===");
    ESP_LOGI(TAG, "Stream URL: http://%s/stream.wav", ip);
    
    // Main loop - monitor WiFi connection
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        
        if (!WiFiManager::is_connected()) {
            ESP_LOGW(TAG, "WiFi disconnected!");
        }
    }
}
