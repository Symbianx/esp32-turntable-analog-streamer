#include "nvs_config.h"
#include "../system/error_handler.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <cstring>

static const char *TAG = "nvs_config";

// NVS namespace and key
constexpr const char *NVS_NAMESPACE = "device_cfg";
constexpr const char *NVS_KEY = "config";

// CRC32 calculation (simple implementation)
static uint32_t calculate_crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return ~crc;
}

bool NVSConfig::init()
{
    ESP_LOGI(TAG, "Initializing NVS");
    
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated or format changed, erase and reinit
        ESP_LOGW(TAG, "NVS partition needs erase, reinitializing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    
    if (err != ESP_OK) {
        ErrorHandler::log_error(ErrorType::NVS_ERROR, "NVS initialization failed");
        return false;
    }
    
    ESP_LOGI(TAG, "NVS initialized successfully");
    return true;
}

bool NVSConfig::load(DeviceConfig *config)
{
    if (config == nullptr) {
        return false;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace not found (err: %d), using factory defaults", err);
        return load_factory_defaults(config);
    }
    
    // Read config blob
    size_t required_size = 0;
    err = nvs_get_blob(nvs_handle, NVS_KEY, nullptr, &required_size);
    if (err != ESP_OK || required_size != sizeof(DeviceConfig)) {
        ESP_LOGW(TAG, "NVS config blob invalid (err: %d, size: %d vs %d), using factory defaults",
                 err, required_size, sizeof(DeviceConfig));
        nvs_close(nvs_handle);
        return load_factory_defaults(config);
    }
    
    uint8_t buffer[sizeof(DeviceConfig)];
    required_size = sizeof(DeviceConfig);
    err = nvs_get_blob(nvs_handle, NVS_KEY, buffer, &required_size);
    nvs_close(nvs_handle);
    
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read NVS config (err: %d), using factory defaults", err);
        return load_factory_defaults(config);
    }
    
    // Copy to config struct
    memcpy(config, buffer, sizeof(DeviceConfig));
    
    // Validate CRC32
    uint32_t expected_crc = config->crc32;
    config->crc32 = 0;  // Zero out for calculation
    uint32_t calculated_crc = calculate_crc32((uint8_t *)config, 
                                               sizeof(DeviceConfig) - sizeof(uint32_t));
    config->crc32 = expected_crc;  // Restore
    
    if (calculated_crc != expected_crc) {
        ESP_LOGE(TAG, "NVS config CRC mismatch (expected: 0x%08X, got: 0x%08X)", 
                 expected_crc, calculated_crc);
        ESP_LOGE(TAG, "Config data: v=%d, ssid='%s', rate=%lu", 
                 config->version, config->wifi_ssid, config->sample_rate);
        ErrorHandler::log_error(ErrorType::NVS_ERROR, "Config corruption detected");
        return load_factory_defaults(config);
    }
    
    ESP_LOGI(TAG, "Config loaded from NVS (version %d, SSID: '%s', rate: %lu Hz)", 
             config->version, config->wifi_ssid, config->sample_rate);
    return true;
}

bool NVSConfig::save(const DeviceConfig *config)
{
    if (config == nullptr) {
        return false;
    }
    
    // Create a copy and calculate CRC32
    DeviceConfig config_copy = *config;
    config_copy.crc32 = 0;
    config_copy.crc32 = calculate_crc32((uint8_t *)&config_copy, 
                                         sizeof(DeviceConfig) - sizeof(uint32_t));
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ErrorHandler::log_error(ErrorType::NVS_ERROR, "Failed to open NVS for writing");
        return false;
    }
    
    err = nvs_set_blob(nvs_handle, NVS_KEY, &config_copy, sizeof(DeviceConfig));
    if (err != ESP_OK) {
        ErrorHandler::log_error(ErrorType::NVS_ERROR, "Failed to write config to NVS");
        nvs_close(nvs_handle);
        return false;
    }
    
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    if (err != ESP_OK) {
        ErrorHandler::log_error(ErrorType::NVS_ERROR, "Failed to commit NVS changes");
        return false;
    }
    
    ESP_LOGI(TAG, "Config saved to NVS (CRC: 0x%08X)", config_copy.crc32);
    return true;
}

bool NVSConfig::load_factory_defaults(DeviceConfig *config)
{
    if (config == nullptr) {
        return false;
    }
    
    // Initialize with factory defaults
    config->version = 1;
    strncpy(config->wifi_ssid, "", sizeof(config->wifi_ssid));
    strncpy(config->wifi_password, "", sizeof(config->wifi_password));
    config->sample_rate = DeviceConfig::DEFAULT_SAMPLE_RATE;
    strncpy(config->device_name, DeviceConfig::DEFAULT_DEVICE_NAME, 
            sizeof(config->device_name));
    config->http_port = DeviceConfig::DEFAULT_HTTP_PORT;
    config->max_clients = DeviceConfig::DEFAULT_MAX_CLIENTS;
    config->crc32 = 0;
    
    ESP_LOGI(TAG, "Factory defaults loaded");
    return true;
}

bool NVSConfig::erase()
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return true;  // Nothing to erase
    }
    
    err = nvs_erase_all(nvs_handle);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    
    nvs_close(nvs_handle);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NVS config erased");
        return true;
    } else {
        ErrorHandler::log_error(ErrorType::NVS_ERROR, "Failed to erase NVS config");
        return false;
    }
}
