#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include <cstdint>

class AudioCapture {
public:
    // Start audio capture task (I²S DMA → ring buffer)
    // Task runs on Core 0 at highest priority
    static bool start();
    
    // Stop audio capture task
    static bool stop();
    
    // Get total frames captured since start
    static uint64_t get_total_frames();
    
    // Get I²S read underrun count
    static uint32_t get_underrun_count();
    
    // Check if sustained clipping is detected (>1s)
    static bool is_clipping();
    
    // Check if capture is running
    static bool is_running();
};

#endif // AUDIO_CAPTURE_H
