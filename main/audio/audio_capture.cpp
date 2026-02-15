#include "audio_capture.h"
#include "i2s_master.h"
#include "audio_buffer.h"
#include "../system/error_handler.h"
#include "../system/watchdog.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>

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
        
        // Convert from 32-bit I²S slots to 24-bit packed WAV format
        // ESP32 I²S reads: [L_byte0 L_byte1 L_byte2 L_byte3] [R_byte0 R_byte1 R_byte2 R_byte3]
        // 24-bit data is in upper 3 bytes (MSB-aligned): [XX L2 L1 L0] [XX R2 R1 R0]
        // WAV needs: [L0 L1 L2] [R0 R1 R2]
        size_t frames = bytes_read / 8;  // 8 bytes per stereo frame
        for (size_t i = 0; i < frames; i++) {
            size_t src_idx = i * 8;
            size_t dst_idx = i * 6;
            
            // Left channel: bytes 1,2,3 of 32-bit word (MSB-aligned 24-bit)
            converted_buffer[dst_idx + 0] = dma_buffer[src_idx + 1];  // L0
            converted_buffer[dst_idx + 1] = dma_buffer[src_idx + 2];  // L1
            converted_buffer[dst_idx + 2] = dma_buffer[src_idx + 3];  // L2
            
            // Right channel: bytes 5,6,7 of 32-bit word (MSB-aligned 24-bit)
            converted_buffer[dst_idx + 3] = dma_buffer[src_idx + 5];  // R0
            converted_buffer[dst_idx + 4] = dma_buffer[src_idx + 6];  // R1
            converted_buffer[dst_idx + 5] = dma_buffer[src_idx + 7];  // R2
        }
        size_t converted_size = frames * 6;
        
        read_count++;
        if (read_count == 1 || read_count % 5000 == 0) {
            // Log less frequently - every 25 seconds
            ESP_LOGI(TAG, "Audio capture: %lu chunks, %llu frames. Sample: %02X %02X %02X",
                     read_count, total_frames_captured.load(),
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
