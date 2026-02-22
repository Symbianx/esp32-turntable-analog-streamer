#ifndef STREAM_HANDLER_H
#define STREAM_HANDLER_H

#include "../config_schema.h"

class StreamHandler {
public:
    // Build WAV header for HTTP streaming
    static void build_wav_header(WavHeader* header, uint32_t sample_rate);
    
    // Downsample 24-bit PCM to 16-bit PCM (truncation method)
    // Returns number of bytes written to output_16bit
    static size_t downsample_24to16(const uint8_t* input_24bit, uint8_t* output_16bit, size_t input_bytes);
    
    // TODO: Add client management, stream serving, etc. in later tasks
};

#endif // STREAM_HANDLER_H
