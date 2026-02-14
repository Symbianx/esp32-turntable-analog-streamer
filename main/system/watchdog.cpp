#include "watchdog.h"
#include "error_handler.h"
#include "esp_task_wdt.h"
#include "esp_log.h"

static const char *TAG = "watchdog";

// Watchdog timeout in seconds (from sdkconfig.defaults: 10s)
constexpr uint32_t WATCHDOG_TIMEOUT_S = 10;

bool Watchdog::init()
{
    // ESP-IDF task watchdog is already initialized by default
    // We just need to subscribe our critical tasks to it
    ESP_LOGI(TAG, "Watchdog initialized (timeout: %d seconds)", WATCHDOG_TIMEOUT_S);
    return true;
}

bool Watchdog::subscribe_task(TaskHandle_t task_handle)
{
    esp_err_t err = esp_task_wdt_add(task_handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Task subscribed to watchdog");
        return true;
    } else {
        ErrorHandler::log_error(ErrorType::SYSTEM_ERROR, "Failed to subscribe task to watchdog");
        return false;
    }
}

bool Watchdog::unsubscribe_task(TaskHandle_t task_handle)
{
    esp_err_t err = esp_task_wdt_delete(task_handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Task unsubscribed from watchdog");
        return true;
    } else {
        ErrorHandler::log_error(ErrorType::SYSTEM_ERROR, "Failed to unsubscribe task from watchdog");
        return false;
    }
}

void Watchdog::reset()
{
    // Reset watchdog timer for current task
    esp_task_wdt_reset();
}
