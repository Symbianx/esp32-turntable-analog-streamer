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
    header->byte_rate = sample_rate * 2 * 3;  // sample_rate × channels × bytes_per_sample
    header->block_align = 6;  // channels × bytes_per_sample (2 × 3)
    header->bits_per_sample = 24;
    
    // data chunk
    memcpy(header->data_tag, "data", 4);
    header->data_size = 0xFFFFFFFF;  // Indeterminate (streaming)
    
    ESP_LOGI(TAG, "WAV header built: %d Hz, 24-bit stereo, byte_rate=%d",
             sample_rate, header->byte_rate);
}
