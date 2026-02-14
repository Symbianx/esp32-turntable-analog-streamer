#include "error_handler.h"
#include "esp_log.h"
#include <atomic>

static const char *TAG = "error_handler";

// Error counters (atomic for thread safety)
static std::atomic<uint32_t> i2s_error_count{0};
static std::atomic<uint32_t> wifi_error_count{0};
static std::atomic<uint32_t> http_error_count{0};
static std::atomic<uint32_t> nvs_error_count{0};

void ErrorHandler::log_error(ErrorType type, const char *message)
{
    // Increment error counter
    switch (type) {
        case ErrorType::I2S_ERROR:
            i2s_error_count++;
            ESP_LOGE(TAG, "[I2S] %s (count: %d)", message, i2s_error_count.load());
            break;
        case ErrorType::WIFI_ERROR:
            wifi_error_count++;
            ESP_LOGE(TAG, "[WiFi] %s (count: %d)", message, wifi_error_count.load());
            break;
        case ErrorType::HTTP_ERROR:
            http_error_count++;
            ESP_LOGE(TAG, "[HTTP] %s (count: %d)", message, http_error_count.load());
            break;
        case ErrorType::NVS_ERROR:
            nvs_error_count++;
            ESP_LOGE(TAG, "[NVS] %s (count: %d)", message, nvs_error_count.load());
            break;
        case ErrorType::SYSTEM_ERROR:
            ESP_LOGE(TAG, "[SYSTEM] %s", message);
            break;
    }
}

void ErrorHandler::log_warning(const char *subsystem, const char *message)
{
    ESP_LOGW(TAG, "[%s] %s", subsystem, message);
}

void ErrorHandler::log_info(const char *subsystem, const char *message)
{
    ESP_LOGI(TAG, "[%s] %s", subsystem, message);
}

uint32_t ErrorHandler::get_i2s_error_count()
{
    return i2s_error_count.load();
}

uint32_t ErrorHandler::get_wifi_error_count()
{
    return wifi_error_count.load();
}

uint32_t ErrorHandler::get_http_error_count()
{
    return http_error_count.load();
}

uint32_t ErrorHandler::get_nvs_error_count()
{
    return nvs_error_count.load();
}

void ErrorHandler::reset_error_counts()
{
    i2s_error_count = 0;
    wifi_error_count = 0;
    http_error_count = 0;
    nvs_error_count = 0;
    ESP_LOGI(TAG, "Error counters reset");
}
