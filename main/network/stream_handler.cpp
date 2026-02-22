#include "stream_handler.h"
#include "../system/error_handler.h"
#include "esp_log.h"
#include <cstring>

static const char *TAG = "stream_handler";

void StreamHandler::build_wav_header(WavHeader* header, uint32_t sample_rate)
{
    if (header == nullptr) {
        return;
    }
    
    // RIFF chunk
    memcpy(header->riff_tag, "RIFF", 4);
    header->riff_size = 0xFFFFFFFF;  // Indeterminate (streaming)
    memcpy(header->wave_tag, "WAVE", 4);
    
    // fmt chunk
    memcpy(header->fmt_tag, "fmt ", 4);
    header->fmt_size = 16;  // PCM format chunk size
    header->audio_format = 1;  // PCM (uncompressed)
    header->num_channels = 2;  // Stereo
    header->sample_rate = sample_rate;
    header->byte_rate = sample_rate * 2 * 2;  // sample_rate × channels × bytes_per_sample (16-bit = 2 bytes)
    header->block_align = 4;  // channels × bytes_per_sample (2 × 2)
    header->bits_per_sample = 16;
    
    // data chunk
    memcpy(header->data_tag, "data", 4);
    header->data_size = 0xFFFFFFFF;  // Indeterminate (streaming)
    
    ESP_LOGI(TAG, "WAV header built: %d Hz, 16-bit stereo, byte_rate=%d",
             sample_rate, header->byte_rate);
}
size_t StreamHandler::downsample_24to16(const uint8_t* input_24bit, uint8_t* output_16bit, size_t input_bytes)
{
    // Input: 24-bit PCM stereo (6 bytes per frame: 3 bytes L, 3 bytes R)
    // Output: 16-bit PCM stereo (4 bytes per frame: 2 bytes L, 2 bytes R)
    // Method: Truncation - take upper 16 bits (bytes 1 and 2) from each 24-bit sample
    
    if (input_24bit == nullptr || output_16bit == nullptr) {
        return 0;
    }
    
    size_t num_frames = input_bytes / 6;  // Each frame is 6 bytes in 24-bit stereo
    size_t output_bytes = 0;
    
    for (size_t frame = 0; frame < num_frames; frame++)
    {
        size_t in_offset = frame * 6;
        size_t out_offset = frame * 4;
        
        // Left channel: Take bytes 1 and 2 from input (upper 16 bits)
        output_16bit[out_offset + 0] = input_24bit[in_offset + 1];  // Mid byte (bits 8-15)
        output_16bit[out_offset + 1] = input_24bit[in_offset + 2];  // MSB (bits 16-23)
        
        // Right channel: Take bytes 1 and 2 from input (upper 16 bits)
        output_16bit[out_offset + 2] = input_24bit[in_offset + 4];  // Mid byte (bits 8-15)
        output_16bit[out_offset + 3] = input_24bit[in_offset + 5];  // MSB (bits 16-23)
        
        output_bytes += 4;
    }
    
    return output_bytes;
}
