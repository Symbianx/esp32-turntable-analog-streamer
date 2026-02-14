#ifndef PCM1808_DRIVER_H
#define PCM1808_DRIVER_H

#include <cstdint>

class PCM1808Driver {
public:
    // Initialize PCM1808 ADC (power-up sequence, 500ms settling time)
    // Assumes VA/VD supplies are always-on and FMT/MODE pins hardwired
    static bool init();
    
    // Validate that SCKI clock frequency matches sample rate requirements
    // PCM1808 requires 256fs, 384fs, or 512fs (we use 256fs)
    static bool validate_clock(uint32_t sample_rate);
    
    // Deinitialize PCM1808 (assert RST LOW if GPIO-controlled)
    static void deinit();
};

#endif // PCM1808_DRIVER_H
