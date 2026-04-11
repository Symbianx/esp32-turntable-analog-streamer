#ifndef EQ_PROCESSOR_H
#define EQ_PROCESSOR_H

#include "../config_schema.h"
#include <cstdint>
#include <cstddef>

// EQProcessor: Real-time parametric equalizer for the audio capture pipeline.
//
// Architecture:
//   - All state is static DRAM_ATTR (no heap; Constitution §III)
//   - process() is called from Core 0 (audio_capture_task) per DMA block
//   - update_band() / set_enabled() are called from Core 1 (HTTP handler)
//   - Coefficient updates are lock-free: Xtensa 32-bit float writes are atomic
//     (Constitution §IV — no mutex in audio path)
//
// Data flow (per DMA block):
//   dma_buffer (uint8_t, 32-bit I²S slots) →
//   float32 LRLR interleaved →
//   N-stage dsps_biquad_sf32 chain →
//   24-bit packed stereo (uint8_t)

static constexpr uint8_t  EQ_MAX_BANDS         = 10;
static constexpr size_t   EQ_FRAMES_PER_BLOCK   = 240;  // DMA block = 240 stereo frames

class EQProcessor {
public:
    // Initialize EQ from config and current sample rate.
    // Computes biquad coefficients for all enabled bands.
    // Zeroes all delay lines.
    // Call once at startup, after NVSConfig::load().
    static bool init(const DeviceConfig& config, uint32_t sample_rate);

    // Process one DMA block through the EQ filter chain (Core 0 only).
    //   input_i2s : uint8_t[frames * 8]  — raw I²S DMA data (32-bit slots, MSB-aligned 24-bit)
    //   output_24 : uint8_t[frames * 6]  — output 24-bit packed stereo (little-endian)
    //   frames    : stereo frame count (typically EQ_FRAMES_PER_BLOCK = 240)
    // Returns true if EQ was applied; false if bypassed (caller uses legacy packing path).
    static bool process(const uint8_t* input_i2s, uint8_t* output_24, size_t frames);

    // Update a single band's parameters and recompute its coefficients.
    // Delay lines are NOT reset — avoids clicks on live parameter change.
    // Safe to call from Core 1 (HTTP handler).
    static void update_band(uint8_t band_index, const EQBandConfig& band, uint32_t sample_rate);

    // Enable or disable master EQ switch.
    // On false→true: zeroes all delay lines, recomputes all active-band coefficients.
    static void set_enabled(bool enabled);

    static bool     is_enabled();
    static uint8_t  active_band_count();
    static uint32_t get_sample_rate();
};

#endif // EQ_PROCESSOR_H
