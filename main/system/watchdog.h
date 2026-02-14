#ifndef WATCHDOG_H
#define WATCHDOG_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class Watchdog {
public:
    // Initialize watchdog timer (10s timeout from sdkconfig.defaults)
    static bool init();
    
    // Subscribe task to watchdog monitoring
    static bool subscribe_task(TaskHandle_t task_handle);
    
    // Unsubscribe task from watchdog
    static bool unsubscribe_task(TaskHandle_t task_handle);
    
    // Reset watchdog timer (call periodically from monitored tasks)
    static void reset();
};

#endif // WATCHDOG_H
