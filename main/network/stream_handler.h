#ifndef STREAM_HANDLER_H
#define STREAM_HANDLER_H

#include "../config_schema.h"

class StreamHandler {
public:
    // Build WAV header for HTTP streaming
    static void build_wav_header(WavHeader* header, uint32_t sample_rate);
    
    // TODO: Add client management, stream serving, etc. in later tasks
};

#endif // STREAM_HANDLER_H
