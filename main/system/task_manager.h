#ifndef TASK_MANAGER_H
#define TASK_MANAGER_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdint>

class TaskManager {
public:
    // Create core-pinned tasks with predefined priorities and stack sizes
    static bool create_audio_capture_task(TaskFunction_t task_func, void *params);
    static bool create_http_stream_task(TaskFunction_t task_func, void *params);
    static bool create_wifi_manager_task(TaskFunction_t task_func, void *params);
    static bool create_metrics_task(TaskFunction_t task_func, void *params);
    
    // CPU usage monitoring (for SystemMetrics)
    static uint32_t get_cpu_usage_core0();
    static uint32_t get_cpu_usage_core1();
};

#endif // TASK_MANAGER_H
