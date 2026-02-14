#include "task_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdlib>
#include <cstring>

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

// CPU usage tracking
static uint32_t cpu_usage_val[2] = {0, 0};

static void update_cpu_usage()
{
    static uint32_t prev_idle[2] = {0, 0};
    static uint32_t prev_total = 0;
    static int64_t last_update_us = 0;

    int64_t now = esp_timer_get_time();
    if (now - last_update_us < 2000000) return; // Update at most every 2s
    last_update_us = now;

    TaskHandle_t idle0 = xTaskGetIdleTaskHandleForCore(0);
    TaskHandle_t idle1 = xTaskGetIdleTaskHandleForCore(1);
    if (!idle0 || !idle1) return;

    UBaseType_t num = uxTaskGetNumberOfTasks();
    TaskStatus_t *tasks = (TaskStatus_t *)malloc(num * sizeof(TaskStatus_t));
    if (!tasks) return;

    uint32_t total;
    num = uxTaskGetSystemState(tasks, num, &total);

    uint32_t idle_time[2] = {0, 0};
    for (UBaseType_t i = 0; i < num; i++) {
        if (tasks[i].xHandle == idle0) idle_time[0] = tasks[i].ulRunTimeCounter;
        if (tasks[i].xHandle == idle1) idle_time[1] = tasks[i].ulRunTimeCounter;
    }
    free(tasks);

    uint32_t dt = total - prev_total;
    if (dt > 0) {
        for (int c = 0; c < 2; c++) {
            uint32_t di = idle_time[c] - prev_idle[c];
            uint32_t usage = 100 - (di * 100 / dt);
            cpu_usage_val[c] = (usage > 100) ? 0 : usage;
            prev_idle[c] = idle_time[c];
        }
    }
    prev_total = total;
}

uint32_t TaskManager::get_cpu_usage_core0()
{
    update_cpu_usage();
    return cpu_usage_val[0];
}

uint32_t TaskManager::get_cpu_usage_core1()
{
    update_cpu_usage();
    return cpu_usage_val[1];
}
