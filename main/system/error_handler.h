#ifndef ERROR_HANDLER_H
#define ERROR_HANDLER_H

#include <cstdint>

enum class ErrorType {
    I2S_ERROR,
    WIFI_ERROR,
    HTTP_ERROR,
    NVS_ERROR,
    SYSTEM_ERROR
};

class ErrorHandler {
public:
    // Log error with automatic counter increment
    static void log_error(ErrorType type, const char *message);
    
    // Log warning (no counter)
    static void log_warning(const char *subsystem, const char *message);
    
    // Log info (no counter)
    static void log_info(const char *subsystem, const char *message);
    
    // Get error counts (for SystemMetrics)
    static uint32_t get_i2s_error_count();
    static uint32_t get_wifi_error_count();
    static uint32_t get_http_error_count();
    static uint32_t get_nvs_error_count();
    
    // Reset all error counters
    static void reset_error_counts();
};

#endif // ERROR_HANDLER_H
