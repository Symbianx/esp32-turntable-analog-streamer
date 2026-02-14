#include "audio_buffer.h"
#include "../system/error_handler.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <cstring>

static const char *TAG = "audio_buffer";

// Ring buffer size: 2 seconds at 96kHz stereo 24-bit
// 96000 Hz × 2 channels × 3 bytes × 2 seconds = 1,152,000 bytes
constexpr size_t RING_BUFFER_SIZE = 1152000;

// Ring buffer and pointers
static uint8_t *ring_buffer = nullptr;
static std::atomic<uint32_t> write_pos{0};
static std::atomic<uint32_t> read_pos[ClientConnection::MAX_CLIENTS] = {0, 0, 0};
static std::atomic<bool> client_active[ClientConnection::MAX_CLIENTS] = {false, false, false};

// Overrun counters
static std::atomic<uint32_t> overrun_count{0};

bool AudioBuffer::init()
{
    ESP_LOGI(TAG, "Initializing audio ring buffer in PSRAM");
    
    // Allocate ring buffer in PSRAM
    ring_buffer = (uint8_t *)heap_caps_malloc(RING_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (ring_buffer == nullptr) {
        ErrorHandler::log_error(ErrorType::SYSTEM_ERROR, 
                                "Failed to allocate ring buffer in PSRAM");
        return false;
    }
    
    // Zero-initialize buffer
    memset(ring_buffer, 0, RING_BUFFER_SIZE);
    
    // Reset all pointers
    write_pos.store(0, std::memory_order_release);
    for (int i = 0; i < ClientConnection::MAX_CLIENTS; i++) {
        read_pos[i].store(0, std::memory_order_release);
        client_active[i].store(false, std::memory_order_release);
    }
    overrun_count.store(0, std::memory_order_release);
    
    ESP_LOGI(TAG, "Ring buffer initialized: %d bytes (%.2f MB) in PSRAM", 
             RING_BUFFER_SIZE, RING_BUFFER_SIZE / (1024.0 * 1024.0));
    
    return true;
}

bool AudioBuffer::write(const uint8_t *data, size_t size)
{
    if (ring_buffer == nullptr) {
        return false;
    }
    
    uint32_t wp = write_pos.load(std::memory_order_acquire);
    
    // Write data to ring buffer using memcpy (handles wrap-around)
    uint32_t space_to_end = RING_BUFFER_SIZE - wp;
    if (size <= space_to_end) {
        // No wrap needed
        memcpy(&ring_buffer[wp], data, size);
        wp = (wp + size) % RING_BUFFER_SIZE;
    } else {
        // Wrap around
        memcpy(&ring_buffer[wp], data, space_to_end);
        memcpy(&ring_buffer[0], data + space_to_end, size - space_to_end);
        wp = size - space_to_end;
    }
    
    // Check if we're about to overrun any active client
    static uint32_t log_throttle[ClientConnection::MAX_CLIENTS] = {0};
    for (int client_id = 0; client_id < ClientConnection::MAX_CLIENTS; client_id++) {
        if (client_active[client_id].load(std::memory_order_acquire)) {
            uint32_t rp = read_pos[client_id].load(std::memory_order_acquire);
            
            // Calculate distance between write and read pointers
            uint32_t distance = (wp >= rp) ? (wp - rp) : (RING_BUFFER_SIZE - rp + wp);
            
            // Warn when writer is about to lap reader (< 5% buffer remaining)
            uint32_t pct = (distance * 100) / RING_BUFFER_SIZE;
            if (pct < 5) {
                if (log_throttle[client_id]++ % 5000 == 0) {
                    ESP_LOGW(TAG, "Client %d buffer low (%u%%)", client_id, pct);
                }
                overrun_count++;
            } else {
                log_throttle[client_id] = 0;
            }
        }
    }
    
    // Update write pointer atomically
    write_pos.store(wp, std::memory_order_release);
    
    return true;
}

bool AudioBuffer::read(uint8_t client_id, uint8_t *data, size_t size, size_t *bytes_read)
{
    if (ring_buffer == nullptr || client_id >= ClientConnection::MAX_CLIENTS) {
        *bytes_read = 0;
        return false;
    }
    
    if (!client_active[client_id].load(std::memory_order_acquire)) {
        *bytes_read = 0;
        return false;
    }
    
    uint32_t wp = write_pos.load(std::memory_order_acquire);
    uint32_t rp = read_pos[client_id].load(std::memory_order_acquire);
    
    // Calculate available data
    uint32_t available = (wp >= rp) ? (wp - rp) : (RING_BUFFER_SIZE - rp + wp);
    
    if (available == 0) {
        *bytes_read = 0;
        return true;  // No error, just no data available
    }
    
    // Read up to requested size
    size_t to_read = (size < available) ? size : available;
    *bytes_read = to_read;
    
    // Read data from ring buffer using memcpy (handles wrap-around)
    uint32_t space_to_end = RING_BUFFER_SIZE - rp;
    if (to_read <= space_to_end) {
        // No wrap needed
        memcpy(data, &ring_buffer[rp], to_read);
        rp = (rp + to_read) % RING_BUFFER_SIZE;
    } else {
        // Wrap around
        memcpy(data, &ring_buffer[rp], space_to_end);
        memcpy(data + space_to_end, &ring_buffer[0], to_read - space_to_end);
        rp = to_read - space_to_end;
    }
    
    // Update read pointer atomically
    read_pos[client_id].store(rp, std::memory_order_release);
    
    return true;
}

bool AudioBuffer::register_client(uint8_t client_id)
{
    if (client_id >= ClientConnection::MAX_CLIENTS) {
        return false;
    }
    
    if (client_active[client_id].load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "Client %d already registered", client_id);
        return false;
    }
    
    // Set client read position BEHIND write position to allow buffering
    // Start 500ms behind (48000 Hz * 2 ch * 3 bytes * 0.5s = 144000 bytes)
    constexpr uint32_t START_BUFFER = 144000;
    uint32_t wp = write_pos.load(std::memory_order_acquire);
    uint32_t rp = (wp >= START_BUFFER) ? (wp - START_BUFFER) : (RING_BUFFER_SIZE - START_BUFFER + wp);
    
    read_pos[client_id].store(rp, std::memory_order_release);
    client_active[client_id].store(true, std::memory_order_release);
    
    ESP_LOGI(TAG, "Client %d registered (read pos: %u, write pos: %u, buffer: %u bytes)", 
             client_id, rp, wp, (wp >= rp) ? (wp - rp) : (RING_BUFFER_SIZE - rp + wp));
    return true;
}

bool AudioBuffer::unregister_client(uint8_t client_id)
{
    if (client_id >= ClientConnection::MAX_CLIENTS) {
        return false;
    }
    
    client_active[client_id].store(false, std::memory_order_release);
    read_pos[client_id].store(0, std::memory_order_release);
    
    ESP_LOGI(TAG, "Client %d unregistered", client_id);
    return true;
}

uint32_t AudioBuffer::get_fill_bytes()
{
    if (ring_buffer == nullptr) {
        return 0;
    }
    
    uint32_t wp = write_pos.load(std::memory_order_acquire);
    
    // Calculate minimum fill across all active clients
    uint32_t min_fill = RING_BUFFER_SIZE;
    bool any_active = false;
    
    for (int client_id = 0; client_id < ClientConnection::MAX_CLIENTS; client_id++) {
        if (client_active[client_id].load(std::memory_order_acquire)) {
            any_active = true;
            uint32_t rp = read_pos[client_id].load(std::memory_order_acquire);
            uint32_t fill = (wp >= rp) ? (wp - rp) : (RING_BUFFER_SIZE - rp + wp);
            if (fill < min_fill) {
                min_fill = fill;
            }
        }
    }
    
    return any_active ? min_fill : 0;
}

float AudioBuffer::get_fill_percentage()
{
    return (get_fill_bytes() * 100.0f) / RING_BUFFER_SIZE;
}

uint32_t AudioBuffer::get_overrun_count()
{
    return overrun_count.load(std::memory_order_acquire);
}

void AudioBuffer::deinit()
{
    if (ring_buffer != nullptr) {
        heap_caps_free(ring_buffer);
        ring_buffer = nullptr;
        ESP_LOGI(TAG, "Ring buffer freed");
    }
}
