#include "stream_handler.h"
#include "../system/error_handler.h"
#include "esp_log.h"
#include "esp_random.h"
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

void StreamHandler::build_wav_header_24bit(WavHeader* header, uint32_t sample_rate)
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
    header->byte_rate = sample_rate * 2 * 3;  // sample_rate × channels × bytes_per_sample (24-bit = 3 bytes)
    header->block_align = 6;  // channels × bytes_per_sample (2 × 3)
    header->bits_per_sample = 24;
    
    // data chunk
    memcpy(header->data_tag, "data", 4);
    header->data_size = 0xFFFFFFFF;  // Indeterminate (streaming)
    
    ESP_LOGI(TAG, "WAV header built: %d Hz, 24-bit stereo, byte_rate=%d",
             sample_rate, header->byte_rate);
}

size_t StreamHandler::downsample_24to16(const uint8_t* input_24bit, uint8_t* output_16bit, size_t input_bytes)
{
    // Input: 24-bit PCM stereo (6 bytes per frame: 3 bytes L, 3 bytes R)
    // Output: 16-bit PCM stereo (4 bytes per frame: 2 bytes L, 2 bytes R)
    // Method: TPDF dithering - add triangular probability density function noise
    //         before truncating to eliminate correlated quantization distortion
    
    if (input_24bit == nullptr || output_16bit == nullptr) {
        return 0;
    }
    
    size_t num_frames = input_bytes / 6;  // Each frame is 6 bytes in 24-bit stereo
    size_t output_bytes = 0;
    
    for (size_t frame = 0; frame < num_frames; frame++)
    {
        size_t in_offset = frame * 6;
        size_t out_offset = frame * 4;
        
        // Left channel: reconstruct signed 24-bit sample (little-endian in WAV)
        int32_t left = input_24bit[in_offset + 0]
                     | (input_24bit[in_offset + 1] << 8)
                     | (input_24bit[in_offset + 2] << 16);
        if (left & 0x800000) left |= 0xFF000000;  // Sign-extend to 32-bit
        
        // Right channel: reconstruct signed 24-bit sample
        int32_t right = input_24bit[in_offset + 3]
                      | (input_24bit[in_offset + 4] << 8)
                      | (input_24bit[in_offset + 5] << 16);
        if (right & 0x800000) right |= 0xFF000000;
        
        // TPDF dither: sum of two independent uniform random values in [-128, 127]
        // produces a triangular distribution in [-255, 255] (1-LSB of 16-bit peak)
        uint32_t rnd = esp_random();
        int32_t dither_l = (int8_t)(rnd & 0xFF) + (int8_t)((rnd >> 8) & 0xFF);
        int32_t dither_r = (int8_t)((rnd >> 16) & 0xFF) + (int8_t)((rnd >> 24) & 0xFF);
        
        // Add dither and truncate: shift right by 8 to go from 24-bit to 16-bit
        int32_t left_16 = (left + dither_l) >> 8;
        int32_t right_16 = (right + dither_r) >> 8;
        
        // Clamp to 16-bit range
        if (left_16 > 32767) left_16 = 32767;
        else if (left_16 < -32768) left_16 = -32768;
        if (right_16 > 32767) right_16 = 32767;
        else if (right_16 < -32768) right_16 = -32768;
        
        // Write 16-bit little-endian
        output_16bit[out_offset + 0] = (uint8_t)(left_16 & 0xFF);
        output_16bit[out_offset + 1] = (uint8_t)((left_16 >> 8) & 0xFF);
        output_16bit[out_offset + 2] = (uint8_t)(right_16 & 0xFF);
        output_16bit[out_offset + 3] = (uint8_t)((right_16 >> 8) & 0xFF);
        
        output_bytes += 4;
    }
    
    return output_bytes;
}
