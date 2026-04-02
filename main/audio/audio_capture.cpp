#include "audio_capture.h"
#include "i2s_master.h"
#include "audio_buffer.h"
#include "eq_processor.h"
#include "../system/error_handler.h"
#include "../system/watchdog.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <cmath>

static const char *TAG = "audio_capture";

// DMA read buffer (must be in internal SRAM, not PSRAM)
// ESP32 I²S reads 32-bit slots for 24-bit audio (4 bytes per sample)
constexpr size_t DMA_READ_SIZE = 1920;  // 240 frames × 8 bytes (32-bit stereo)
static uint8_t dma_buffer[DMA_READ_SIZE];
static uint8_t converted_buffer[1440];  // 240 frames × 6 bytes (24-bit packed)

// Capture state
static std::atomic<bool> capture_running{false};
static std::atomic<uint64_t> total_frames_captured{0};
static std::atomic<uint32_t> underrun_count{0};
static std::atomic<bool> clipping_detected{false};

// Playback detection state (T007)
static std::atomic<bool> playback_status{false};
static float audio_threshold_db_value = -40.0f;
static float audio_threshold_linear = 838.0f;  // Default: -40dB for 24-bit
static float rms_accumulator = 0.0f;
constexpr float RMS_ALPHA = 0.05f;  // Exponential moving average factor
constexpr float PLAYBACK_ENTER_HYSTERESIS = 1.35f;
constexpr float PLAYBACK_EXIT_HYSTERESIS = 0.75f;
constexpr uint32_t PLAYBACK_ON_DEBOUNCE_CHUNKS = 120;   // ~600ms at 48kHz with 240-frame chunks
constexpr uint32_t PLAYBACK_OFF_DEBOUNCE_CHUNKS = 100;  // ~500ms at 48kHz with 240-frame chunks
constexpr int32_t MAX_24BIT = 8388608;  // 2^23

// Clipping detection parameters
constexpr int32_t CLIP_THRESHOLD = 8388000;  // ~99.9% of 24-bit range (8388608)
constexpr uint32_t CLIP_DURATION_FRAMES = 48000;  // 1 second at 48kHz

static void audio_capture_task(void *params)
{
    ESP_LOGI(TAG, "Audio capture task started on Core %d", xPortGetCoreID());
    
    // Subscribe to watchdog (skip if not initialized)
    // Watchdog::subscribe_task(nullptr);  // Disabled for now
    
    capture_running.store(true, std::memory_order_release);
    
    uint32_t clip_counter = 0;
    uint32_t read_count = 0;
    int64_t last_good_read = esp_timer_get_time();
    
    ESP_LOGI(TAG, "Starting audio capture loop");
    
    while (capture_running.load(std::memory_order_acquire)) {
        // Read from I²S DMA
        size_t bytes_read = 0;
        if (!I2SMaster::read(dma_buffer, DMA_READ_SIZE, &bytes_read, 100)) {
            // Read timeout or error
            if (bytes_read == 0) {
                underrun_count++;
                if (underrun_count % 100 == 1) {
                    ESP_LOGW(TAG, "I²S read underrun (count: %lu)", underrun_count.load());
                }
                // I²S failure detection: no data for 5 seconds → attempt reset
                if (esp_timer_get_time() - last_good_read > 5000000) {
                    ESP_LOGE(TAG, "No I²S data for 5s, attempting reset");
                    I2SMaster::stop();
                    vTaskDelay(pdMS_TO_TICKS(100));
                    I2SMaster::start();
                    last_good_read = esp_timer_get_time();
                }
            }
            continue;
        }
        
        if (bytes_read == 0) {
            continue;
        }
        
        last_good_read = esp_timer_get_time();
        
        // Convert from 32-bit I²S slots to 24-bit packed WAV format.
        // EQProcessor::process() handles both the conversion and biquad filtering.
        // If EQ is disabled or no bands are active it returns false and we fall
        // back to the original bit-packing path — zero cost when bypassed.
        size_t frames = bytes_read / 8;  // 8 bytes per stereo frame

        if (!EQProcessor::process(dma_buffer, converted_buffer, frames)) {
            // EQ bypassed: use legacy 32-bit slot → 24-bit packed conversion
            // ESP32 I²S reads: [L_byte0 L_byte1 L_byte2 L_byte3] [R_byte0 R_byte1 R_byte2 R_byte3]
            // 24-bit data is in upper 3 bytes (MSB-aligned): [XX L2 L1 L0] [XX R2 R1 R0]
            for (size_t i = 0; i < frames; i++) {
                size_t src_idx = i * 8;
                size_t dst_idx = i * 6;
                converted_buffer[dst_idx + 0] = dma_buffer[src_idx + 1];  // L0
                converted_buffer[dst_idx + 1] = dma_buffer[src_idx + 2];  // L1
                converted_buffer[dst_idx + 2] = dma_buffer[src_idx + 3];  // L2
                converted_buffer[dst_idx + 3] = dma_buffer[src_idx + 5];  // R0
                converted_buffer[dst_idx + 4] = dma_buffer[src_idx + 6];  // R1
                converted_buffer[dst_idx + 5] = dma_buffer[src_idx + 7];  // R2
            }
        }
        size_t converted_size = frames * 6;
        
        read_count++;
        if (read_count == 1 || read_count % 5000 == 0) {
            // Log less frequently - every 25 seconds
            // Show both raw DMA buffer and converted buffer for diagnostics
            ESP_LOGI(TAG, "Audio capture: %lu chunks, %llu frames. Raw DMA: %02X %02X %02X %02X %02X %02X %02X %02X, Converted: %02X %02X %02X",
                     read_count, total_frames_captured.load(),
                     dma_buffer[0], dma_buffer[1], dma_buffer[2], dma_buffer[3],
                     dma_buffer[4], dma_buffer[5], dma_buffer[6], dma_buffer[7],
                     converted_buffer[0], converted_buffer[1], converted_buffer[2]);
        }
        
        // Write converted 24-bit data to ring buffer
        if (!AudioBuffer::write(converted_buffer, converted_size)) {
            ErrorHandler::log_error(ErrorType::SYSTEM_ERROR, 
                                    "Failed to write to ring buffer");
        }
        
        // Lightweight clipping check: test first sample pair per chunk
        if (converted_size >= 6) {
            int32_t sample = (int32_t)(converted_buffer[0] |
                                       (converted_buffer[1] << 8) |
                                       (converted_buffer[2] << 16));
            if (sample & 0x800000) sample |= 0xFF000000;
            if (abs(sample) > CLIP_THRESHOLD) {
                clip_counter++;
            } else if (clip_counter > 0) {
                clip_counter--;
            }
            if (clip_counter > CLIP_DURATION_FRAMES / DMA_READ_SIZE) {
                if (!clipping_detected.load(std::memory_order_relaxed)) {
                    ESP_LOGW(TAG, "Sustained clipping detected");
                    clipping_detected.store(true, std::memory_order_release);
                }
            } else if (clip_counter == 0 && clipping_detected.load(std::memory_order_relaxed)) {
                ESP_LOGI(TAG, "Clipping cleared");
                clipping_detected.store(false, std::memory_order_release);
            }
        }
        
        // RMS calculation with subsampling for playback detection (T008)
        int64_t sum_squares = 0;
        for (size_t i = 0; i < frames; i += 8) {  // Sample every 8th frame
            if (i * 6 + 2 >= converted_size) break;
            
            // Extract 24-bit signed sample (left channel)
            int32_t sample = (int32_t)(converted_buffer[i*6 + 0] |
                                       (converted_buffer[i*6 + 1] << 8) |
                                       (converted_buffer[i*6 + 2] << 16));
            if (sample & 0x800000) sample |= 0xFF000000;  // Sign extend
            
            sum_squares += (int64_t)sample * sample;
        }
        
        // Calculate RMS for this chunk
        size_t sampled_count = (frames + 7) / 8;  // Ceiling division
        if (sampled_count > 0) {
            float chunk_rms = sqrtf((float)sum_squares / sampled_count);
            
            // Exponential moving average (debounce)
            rms_accumulator = (RMS_ALPHA * chunk_rms) + ((1.0f - RMS_ALPHA) * rms_accumulator);

            // Threshold comparison with hysteresis (T009/T010)
            bool prev_status = playback_status.load(std::memory_order_relaxed);
            float enter_threshold = audio_threshold_linear * PLAYBACK_ENTER_HYSTERESIS;
            float exit_threshold = audio_threshold_linear * PLAYBACK_EXIT_HYSTERESIS;
            bool candidate_state = prev_status
                ? (rms_accumulator > exit_threshold)
                : (rms_accumulator > enter_threshold);

            static uint32_t state_change_counter = 0;
            static bool pending_state = false;

            if (candidate_state == prev_status) {
                state_change_counter = 0;
                pending_state = prev_status;
            } else {
                if (pending_state != candidate_state) {
                    pending_state = candidate_state;
                    state_change_counter = 1;
                } else {
                    state_change_counter++;
                }

                uint32_t debounce_threshold = candidate_state
                    ? PLAYBACK_ON_DEBOUNCE_CHUNKS
                    : PLAYBACK_OFF_DEBOUNCE_CHUNKS;

                if (state_change_counter >= debounce_threshold) {
                    playback_status.store(candidate_state, std::memory_order_release);
                    state_change_counter = 0;
                    ESP_LOGI(TAG,
                             "Playback status changed: %s (RMS: %.1f, on: %.1f, off: %.1f)",
                             candidate_state ? "PLAYING" : "IDLE",
                             rms_accumulator,
                             enter_threshold,
                             exit_threshold);
                }
            }
        }
        
        // Update frame counter
        total_frames_captured.fetch_add(frames, std::memory_order_release);
    }
    
    // Unsubscribe from watchdog (skip if not initialized)
    // Watchdog::unsubscribe_task(nullptr);  // Disabled for now
    
    ESP_LOGI(TAG, "Audio capture task stopped");
    vTaskDelete(nullptr);
}

bool AudioCapture::start()
{
    if (capture_running.load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "Audio capture already running");
        return true;
    }
    
    // Reset counters
    total_frames_captured.store(0, std::memory_order_release);
    underrun_count.store(0, std::memory_order_release);
    clipping_detected.store(false, std::memory_order_release);
    
    // Start I²S master
    if (!I2SMaster::start()) {
        ErrorHandler::log_error(ErrorType::I2S_ERROR, "Failed to start I²S master");
        return false;
    }
    
    // Create audio capture task (will be pinned to Core 0 by TaskManager)
    // For now, create directly since TaskManager is called from main
    BaseType_t result = xTaskCreatePinnedToCore(
        audio_capture_task,
        "audio_capture",
        4096,
        nullptr,
        24,  // Highest priority
        nullptr,
        0    // Core 0
    );
    
    if (result != pdPASS) {
        ErrorHandler::log_error(ErrorType::SYSTEM_ERROR, 
                                "Failed to create audio capture task");
        I2SMaster::stop();
        return false;
    }
    
    ESP_LOGI(TAG, "Audio capture started");
    return true;
}

bool AudioCapture::stop()
{
    capture_running.store(false, std::memory_order_release);
    
    // Wait for task to exit
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Stop I²S master
    I2SMaster::stop();
    
    ESP_LOGI(TAG, "Audio capture stopped");
    return true;
}

uint64_t AudioCapture::get_total_frames()
{
    return total_frames_captured.load(std::memory_order_acquire);
}

uint32_t AudioCapture::get_underrun_count()
{
    return underrun_count.load(std::memory_order_acquire);
}

bool AudioCapture::is_clipping()
{
    return clipping_detected.load(std::memory_order_acquire);
}

bool AudioCapture::is_running()
{
    return capture_running.load(std::memory_order_acquire);
}

bool AudioCapture::is_playing()
{
    return playback_status.load(std::memory_order_acquire);
}

void AudioCapture::set_threshold_db(float threshold_db)
{
    if (threshold_db < -60.0f) {
        threshold_db = -60.0f;
    } else if (threshold_db > -20.0f) {
        threshold_db = -20.0f;
    }

    audio_threshold_db_value = threshold_db;
    // Convert dB to linear amplitude for 24-bit audio
    // linear = 2^23 * 10^(dB/20)
    audio_threshold_linear = MAX_24BIT * powf(10.0f, threshold_db / 20.0f);
    ESP_LOGI(TAG, "Audio threshold set to %.1f dB (linear: %.1f)", threshold_db, audio_threshold_linear);
}

float AudioCapture::get_threshold_db()
{
    return audio_threshold_db_value;
}

float AudioCapture::get_current_rms_db()
{
    // Convert current RMS to dB
    // dB = 20 * log10(rms / 2^23)
    if (rms_accumulator < 1.0f) {
        return -100.0f;  // Silence
    }
    return 20.0f * log10f(rms_accumulator / MAX_24BIT);
}
