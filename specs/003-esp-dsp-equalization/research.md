# Research: ESP-DSP Parametric Equalization

**Feature**: 003-esp-dsp-equalization  
**Date**: 2026-04-02

## Technology Decisions

### 1. ESP-DSP Biquad IIR Library

**Decision**: Use `espressif/esp-dsp` component — specifically `dsps_biquad_sf32` (stereo interleaved) and `dsps_biquad_gen_*` coefficient generators.

**Rationale**:
- Official Espressif library, maintained alongside ESP-IDF
- Hardware-accelerated: auto-selects Xtensa ae32 assembly (ESP32 LX6 FPU + zero-overhead loop) or aes3 (ESP32-S3 LX7). ~17 CPU cycles/sample/stage at 240 MHz
- Stereo interleaved variant (`dsps_biquad_sf32`) processes L/R together — no de-interleaving needed, matches the existing LRLR I²S output layout
- Coefficient generators produce normalized `coef[5]` = {b0, b1, b2, a1, a2} ready for direct use
- Already part of the Espressif component ecosystem; adds via `main/idf_component.yml`

**Alternatives Considered**:
- **Manual ARM CMSIS DSP port**: Would require porting to Xtensa architecture. No benefit over native esp-dsp.
- **Fixed-point Q15/Q31 biquad**: No IIR biquad in fixed-point exists in esp-dsp. ESP32 LX6 has a single-cycle FPU (`madd.s`) making float32 equally fast. Fixed-point rejected.
- **Custom DSP from scratch**: Unnecessary given esp-dsp quality and acceleration.

**Component registry**: `espressif/esp-dsp`, version `>=1.0.0`  
**Headers**: `#include "esp_dsp.h"` (umbrella) or individually `dsps_biquad.h` + `dsps_biquad_gen.h`

---

### 2. Filter Type Selection

**Decision**: Support 5 filter types: Low Shelf, Peaking EQ, High Shelf, Low-Pass, High-Pass.

**Available in esp-dsp**:

| Type | Generator | Gain Param |
|---|---|---|
| Low-pass | `dsps_biquad_gen_lpf_f32(coef, f, Q)` | No |
| High-pass | `dsps_biquad_gen_hpf_f32(coef, f, Q)` | No |
| Low shelf | `dsps_biquad_gen_lowShelf_f32(coef, f, gain_dB, Q)` | ✅ Yes |
| High shelf | `dsps_biquad_gen_highShelf_f32(coef, f, gain_dB, Q)` | ✅ Yes |
| Peaking EQ | `dsps_biquad_gen_peakingEQ_f32(coef, f, Q)` | ⚠️ **No gain** |

**Critical finding — Peaking EQ**: The built-in `dsps_biquad_gen_peakingEQ_f32` does **not** accept a gain parameter and is effectively a BPF with 0 dB peak — unsuitable for parametric EQ. A custom coefficient generator using the Audio EQ Cookbook (R. Bristow-Johnson) must be implemented:

```c
// Custom peaking EQ with dB gain control:
static void biquad_gen_peak(float *c, float freq_hz, float gain_db, float Q, float Fs) {
    float A     = powf(10.0f, gain_db / 40.0f);
    float w0    = 2.0f * M_PI * (freq_hz / Fs);
    float alpha = sinf(w0) / (2.0f * Q);
    float cosw0 = cosf(w0);
    float a0_inv = 1.0f / (1.0f + alpha / A);
    c[0] = (1.0f + alpha * A) * a0_inv;   // b0
    c[1] = (-2.0f * cosw0)   * a0_inv;   // b1
    c[2] = (1.0f - alpha * A) * a0_inv;  // b2
    c[3] = (-2.0f * cosw0)   * a0_inv;   // a1
    c[4] = (1.0f - alpha / A) * a0_inv;  // a2
}
```

The shelf and LPF/HPF generators from esp-dsp are correct as-is.

---

### 3. Audio Path Integration Point

**Decision**: Insert EQ processing in `audio_capture.cpp` after I²S DMA read, before writing to ring buffer.

**Data flow (current)**:
```
I²S DMA read → dma_buffer[1920] (uint8_t, 32-bit slots, 240 frames)
    → bit-pack to 24-bit: converted_buffer[1440]
    → AudioBuffer::write(converted_buffer, 1440)
```

**Data flow (with EQ)**:
```
I²S DMA read → dma_buffer[1920] (uint8_t, 32-bit slots)
    → Convert 32-bit slots to float32 normalized LRLR: float_buf[480]  ← new
    → EQProcessor::process(float_buf, 240 frames)                       ← new
    → Convert float32 LRLR → 24-bit packed: converted_buffer[1440]      ← new (replaces old pack)
    → AudioBuffer::write(converted_buffer, 1440)
```

**Rationale**: 
- The 32-bit I²S slot format (24-bit audio in MSBs of a 32-bit word) converts naturally to float32 without losing precision — avoids the existing two-step 32→24-bit truncation then 24→float chain
- Processing at this point keeps EQ fully transparent to the ring buffer and all consumers
- Float32 working buffer (480 samples × 4 bytes = 1920 bytes) fits within existing SRAM budget

**Alternatives Considered**:
- **Process on read path (in stream_handler)**: EQ would run per-client on Core 1, duplicating work for multiple clients. Rejected.
- **Separate EQ task**: Adds task overhead and ring buffer latency. Rejected — the existing audio task has headroom.

---

### 4. Memory Strategy

**Decision**: All EQ state in static `DRAM_ATTR` arrays; no heap allocation.

**Memory footprint**:
- Float working buffer: `DRAM_ATTR float float_buf[480]` = 1920 bytes
- EQ coefficients: `DRAM_ATTR float coef[10][5]` = 200 bytes
- Stereo delay lines: `DRAM_ATTR float w[10][4]` = 160 bytes
- Total EQ static DRAM: **≈ 2.3 KB** (within budget, no PSRAM needed)

**`DRAM_ATTR` is required** to prevent Xtensa ae32 assembly from incurring instruction/data cache misses during the multiply-accumulate loop. The delay line `w[]` is accessed every sample — any cache miss would break real-time timing guarantees.

---

### 5. Thread Safety for Coefficient Updates

**Decision**: Lock-free coefficient update via in-place float array overwrite; no mutex.

**Rationale**:
- Coefficient generators run on Core 1 (HTTP handler), audio loop runs on Core 0
- 32-bit float writes are naturally aligned and atomic on Xtensa LX6 (single instruction `s32i`)
- The biquad filter reads each `coef[b][i]` once per invocation; a mid-update read gets a partial-old/partial-new coefficient set at worst — this causes a single block of mild artifacts (inaudible at typical gains)
- Delay lines `w[]` are exclusively owned by Core 0 — never written by Core 1
- A mutex in the audio path would violate Constitution §IV (mutex in audio tasks FORBIDDEN)

**Alternative**: Double-buffered coefficient swap with `std::atomic<int>` index. More complex for negligible benefit given the infrequency of EQ parameter changes. Rejected.

---

### 6. NVS Persistence Strategy

**Decision**: Store EQ configuration as individual NVS keys per band (10 bands × 5 values = 50 keys), plus 1 master enable key. Use the existing `NVSConfig` load/save pattern.

**NVS key naming** (15-char max NVS constraint):
- `eq_enabled` — master switch (uint8)
- `eq_bN_type` — filter type enum (uint8), N=0..9
- `eq_bN_en` — band enabled (uint8)
- `eq_bN_freq` — frequency in Hz (float, stored as uint32 via `memcpy`)
- `eq_bN_gain` — gain in dB (float, stored as uint32 via `memcpy`)
- `eq_bN_q` — Q factor (float, stored as uint32 via `memcpy`)

**Alternative Considered**: Store all EQ bands as a single NVS blob. Simpler but risks corruption of all bands on partial write; individual keys allow partial recovery.

---

### 7. HTTP API Design

**Decision**: Two new REST endpoints — `GET /eq` and `POST /eq`. `POST /eq/reset` for flat reset.

**Format**: JSON (matching existing `/status` endpoint style)  
**Integration**: Added to `http_server.cpp` alongside existing config and stream handlers  
**Config portal**: EQ section added to the existing web UI in `config_portal_html.h`

See `contracts/eq-api.yaml` for full OpenAPI specification.

---

## Performance Estimate

At 48 kHz, 240 frames per DMA block, 10 bands stereo:
- `dsps_biquad_sf32` ae32 assembly: ~17 cycles/sample/stage
- Stereo 240 frames = 480 samples per call
- 10 bands × 480 samples × 17 cycles = **81,600 cycles**
- At 240 MHz: **0.34 ms** per DMA block
- DMA block period at 48 kHz / 240 frames: **5 ms**
- EQ CPU overhead: **≈ 6.8%** of audio task budget — well under the 60% Core 0 ceiling
