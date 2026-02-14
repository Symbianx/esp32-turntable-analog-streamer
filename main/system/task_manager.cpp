#include "task_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "task_manager";

// Task priority definitions (higher number = higher priority)
// FreeRTOS priorities: 0 (lowest) to configMAX_PRIORITIES-1 (highest, typically 25)
constexpr UBaseType_t AUDIO_CAPTURE_PRIORITY = 24;  // Highest (Core 0)
constexpr UBaseType_t HTTP_STREAM_PRIORITY = 10;    // Medium (Core 1)
constexpr UBaseType_t WIFI_MANAGER_PRIORITY = 8;    // Medium-low (Core 1)
constexpr UBaseType_t METRICS_PRIORITY = 5;         // Low (Core 1)

// Stack sizes in bytes
constexpr uint32_t AUDIO_CAPTURE_STACK = 4096;
constexpr uint32_t HTTP_STREAM_STACK = 8192;
constexpr uint32_t WIFI_MANAGER_STACK = 4096;
constexpr uint32_t METRICS_STACK = 2048;

// Core affinity
constexpr BaseType_t AUDIO_CORE = 0;   // Core 0: Audio pipeline
constexpr BaseType_t NETWORK_CORE = 1; // Core 1: Networking

bool TaskManager::create_audio_capture_task(TaskFunction_t task_func, void *params)
{
    BaseType_t result = xTaskCreatePinnedToCore(
        task_func,
        "audio_capture",
        AUDIO_CAPTURE_STACK,
        params,
        AUDIO_CAPTURE_PRIORITY,
        nullptr,
        AUDIO_CORE
    );
    
    if (result == pdPASS) {
        ESP_LOGI(TAG, "Audio capture task created on Core %d (priority %d)", 
                 AUDIO_CORE, AUDIO_CAPTURE_PRIORITY);
        return true;
    } else {
        ESP_LOGE(TAG, "Failed to create audio capture task");
        return false;
    }
}

bool TaskManager::create_http_stream_task(TaskFunction_t task_func, void *params)
{
    BaseType_t result = xTaskCreatePinnedToCore(
        task_func,
        "http_stream",
        HTTP_STREAM_STACK,
        params,
        HTTP_STREAM_PRIORITY,
        nullptr,
        NETWORK_CORE
    );
    
    if (result == pdPASS) {
        ESP_LOGI(TAG, "HTTP stream task created on Core %d (priority %d)", 
                 NETWORK_CORE, HTTP_STREAM_PRIORITY);
        return true;
    } else {
        ESP_LOGE(TAG, "Failed to create HTTP stream task");
        return false;
    }
}

bool TaskManager::create_wifi_manager_task(TaskFunction_t task_func, void *params)
{
    BaseType_t result = xTaskCreatePinnedToCore(
        task_func,
        "wifi_manager",
        WIFI_MANAGER_STACK,
        params,
        WIFI_MANAGER_PRIORITY,
        nullptr,
        NETWORK_CORE
    );
    
    if (result == pdPASS) {
        ESP_LOGI(TAG, "WiFi manager task created on Core %d (priority %d)", 
                 NETWORK_CORE, WIFI_MANAGER_PRIORITY);
        return true;
    } else {
        ESP_LOGE(TAG, "Failed to create WiFi manager task");
        return false;
    }
}

bool TaskManager::create_metrics_task(TaskFunction_t task_func, void *params)
{
    BaseType_t result = xTaskCreatePinnedToCore(
        task_func,
        "metrics",
        METRICS_STACK,
        params,
        METRICS_PRIORITY,
        nullptr,
        NETWORK_CORE
    );
    
    if (result == pdPASS) {
        ESP_LOGI(TAG, "Metrics task created on Core %d (priority %d)", 
                 NETWORK_CORE, METRICS_PRIORITY);
        return true;
    } else {
        ESP_LOGE(TAG, "Failed to create metrics task");
        return false;
    }
}

uint32_t TaskManager::get_cpu_usage_core0()
{
    // Get idle task handle for Core 0
    TaskHandle_t idle_task = xTaskGetIdleTaskHandleForCore(AUDIO_CORE);
    if (idle_task == nullptr) {
        return 0;
    }
    
    // Get runtime stats (requires CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS)
    TaskStatus_t task_status;
    vTaskGetInfo(idle_task, &task_status, pdFALSE, eInvalid);
    
    // CPU usage = 100 - (idle_time_percentage)
    // This is a simplified calculation; full implementation would track
    // total runtime across all tasks and compute percentages
    return 0; // Placeholder - requires full runtime stats implementation
}

uint32_t TaskManager::get_cpu_usage_core1()
{
    // Get idle task handle for Core 1
    TaskHandle_t idle_task = xTaskGetIdleTaskHandleForCore(NETWORK_CORE);
    if (idle_task == nullptr) {
        return 0;
    }
    
    // Same approach as Core 0
    return 0; // Placeholder - requires full runtime stats implementation
}
