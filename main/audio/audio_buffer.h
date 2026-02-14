#ifndef AUDIO_BUFFER_H
#define AUDIO_BUFFER_H

#include "config_schema.h"
#include <cstdint>
#include <cstddef>
#include <atomic>

class AudioBuffer {
public:
    // Initialize ring buffer in PSRAM (1.1MB for 2s at 96kHz)
    static bool init();
    
    // Write audio data to ring buffer (called by I²S capture task)
    // Returns true on success, false if buffer not initialized
    static bool write(const uint8_t *data, size_t size);
    
    // Read audio data from ring buffer for specific client
    // Returns true on success, bytes_read=0 if no data available
    static bool read(uint8_t client_id, uint8_t *data, size_t size, size_t *bytes_read);
    
    // Register client for reading (allocates read pointer)
    // Client starts reading from current write position
    static bool register_client(uint8_t client_id);
    
    // Unregister client (frees read pointer)
    static bool unregister_client(uint8_t client_id);
    
    // Get current buffer fill level in bytes (minimum across all active clients)
    static uint32_t get_fill_bytes();
    
    // Get buffer fill percentage (0-100)
    static float get_fill_percentage();
    
    // Get overrun count (writer lapped a reader)
    static uint32_t get_overrun_count();
    
    // Deinitialize and free ring buffer
    static void deinit();
};

#endif // AUDIO_BUFFER_H
