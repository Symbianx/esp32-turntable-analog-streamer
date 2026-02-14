#ifndef I2S_MASTER_H
#define I2S_MASTER_H

#include <cstdint>
#include <cstddef>

class I2SMaster {
public:
    // Initialize I²S master with specified sample rate
    // Sample rate must be 44100, 48000, or 96000 Hz
    static bool init(uint32_t sample_rate);
    
    // Start I²S reception
    static bool start();
    
    // Stop I²S reception
    static bool stop();
    
    // Read audio data from I²S DMA buffer
    // Returns true if data was read, false on error or timeout
    static bool read(uint8_t *buffer, size_t size, size_t *bytes_read, 
                     uint32_t timeout_ms = 1000);
    
    // Change sample rate (stops I²S, reconfigures, restarts)
    // This will cause a brief audio interruption
    static bool change_sample_rate(uint32_t new_sample_rate);
    
    // Get current sample rate
    static uint32_t get_sample_rate();
    
    // Deinitialize I²S master
    static void deinit();
};

#endif // I2S_MASTER_H
