# Quickstart: ESP-DSP Parametric Equalization

**Feature**: 003-esp-dsp-equalization  
**Date**: 2026-04-02

---

## Overview

This guide covers how the parametric EQ fits into the existing codebase, how to build/test it, and how to extend it.

---

## Architecture at a Glance

The EQ runs entirely on **Core 0** inside the existing `audio_capture_task`. It is inserted as a transparent in-place processing step between I²S DMA read and ring buffer write:

```
                     [Core 0 — audio_capture_task]
                     ┌─────────────────────────────────────────┐
  I²S DMA            │                                         │
  ─────────────────► │ 1. I2SMaster::read() → dma_buffer[]     │
  (32-bit slots)      │ 2. EQProcessor::process(dma_buffer)     │  ← NEW
                     │    • Convert I²S slots → float32 LRLR   │
                     │    • Apply N biquad stages per channel   │
                     │    • Convert float32 → 24-bit packed     │
                     │ 3. AudioBuffer::write(converted_buffer)  │
                     └─────────────────────────────────────────┘
                                                │
                     [Core 1 — http_task]        ▼ ring buffer
                     ┌───────────────┐    ┌─────────────────┐
                     │ GET  /eq      │    │ HTTP streaming   │
                     │ POST /eq      │    │ clients          │
                     │ POST /eq/reset│    └─────────────────┘
                     └───────────────┘
                           │ EQProcessor::update_band()
                           │ (rewrites coef[], lock-free)
```

---

## Key Files

| File | Role |
|---|---|
| `main/audio/eq_processor.h` | `EQProcessor` class declaration |
| `main/audio/eq_processor.cpp` | Float conversion, biquad chain, coefficient generators |
| `main/audio/audio_capture.cpp` | **Integration point** — calls `EQProcessor::process()` |
| `main/config_schema.h` | `EQBandConfig`, `EQFilterType` added to `DeviceConfig` |
| `main/storage/nvs_config.cpp` | Loads/saves EQ config from NVS |
| `main/network/http_server.cpp` | `GET /eq`, `POST /eq`, `POST /eq/reset` handlers |
| `main/network/config_portal_html.h` | Web UI EQ band editor |
| `main/idf_component.yml` | `espressif/esp-dsp` dependency |

---

## Adding the esp-dsp Dependency

Add to `main/idf_component.yml`:

```yaml
dependencies:
  espressif/esp-dsp:
    version: ">=1.0.0"
  # ... existing deps ...
```

Then run:
```bash
idf.py update-dependencies
```

In `main/CMakeLists.txt`, add `eq_processor.cpp` to `SRCS` and `esp-dsp` to `PRIV_REQUIRES`:

```cmake
idf_component_register(
    SRCS
        ...
        "audio/eq_processor.cpp"   # ADD THIS
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp-dsp                    # ADD THIS
)
```

---

## EQProcessor API

```cpp
#include "audio/eq_processor.h"

// Initialize with config and sample rate — call once at startup
// Builds biquad coefficients for all enabled bands
bool EQProcessor::init(const DeviceConfig& config, uint32_t sample_rate);

// Process one DMA block in-place (called from audio_capture_task, Core 0)
// input_i2s:  uint8_t[] — raw I²S DMA data (32-bit slots, 240 frames × 2ch × 4 bytes = 1920 bytes)
// output_24:  uint8_t[] — output 24-bit packed stereo (240 frames × 2ch × 3 bytes = 1440 bytes)
// frames:     number of stereo frames in this block (typically 240)
// Returns false if EQ is disabled (caller should use fallback packing path)
bool EQProcessor::process(const uint8_t* input_i2s, uint8_t* output_24, size_t frames);

// Update a single band's parameters and recompute its coefficients (safe to call from Core 1)
// Band's delay line is NOT reset — avoids audible clicks on parameter change
void EQProcessor::update_band(uint8_t band_index, const EQBandConfig& band, uint32_t sample_rate);

// Enable or disable master EQ switch
// On enable: zeros all delay lines then recomputes coefficients
// On disable: next process() call will bypass the filter chain
void EQProcessor::set_enabled(bool enabled);

bool EQProcessor::is_enabled();
```

---

## Biquad Filter Details

### Coefficient layout: `float coef[5]`
```
coef[0] = b0/a0
coef[1] = b1/a0
coef[2] = b2/a0
coef[3] = a1/a0
coef[4] = a2/a0
```

### Delay line layout: `float w[4]` (stereo)
```
w[0] = left channel state 0
w[1] = left channel state 1
w[2] = right channel state 0
w[3] = right channel state 1
```

### Peaking EQ note
The built-in `dsps_biquad_gen_peakingEQ_f32` **does not support gain**. `EQProcessor` implements the Audio EQ Cookbook formula directly for `PEAKING` type. Shelf filters use the esp-dsp generators directly (they do support gain).

---

## Building and Flashing

```bash
# Configure for your board (first time only)
idf.py set-target esp32s3     # or esp32

# Build
idf.py build

# Flash and monitor
idf.py flash monitor
```

Expected startup log lines after this feature:
```
I (xxx) eq_processor: EQ initialized: 10 bands, sample_rate=48000
I (xxx) eq_processor: Band 0: LOW_SHELF 32Hz, 0.0dB, Q=0.707 [disabled]
...
I (xxx) eq_processor: EQ bypassed (disabled)
```

---

## Using the EQ API

### Enable EQ and set a bass boost:

```bash
curl -X POST http://192.168.1.100:8080/eq \
  -H "Content-Type: application/json" \
  -d '{
    "eq_enabled": true,
    "bands": [
      {"index": 0, "enabled": true, "filter_type": "LOW_SHELF",
       "frequency_hz": 80, "gain_db": 4.0, "q_factor": 0.707}
    ]
  }'
```

### Query current state:

```bash
curl http://192.168.1.100:8080/eq | python3 -m json.tool
```

### Reset to flat:

```bash
curl -X POST http://192.168.1.100:8080/eq/reset
```

---

## Testing EQ Correctness

### Unit test approach (ESP-IDF Unity):

1. Generate a sine wave at a known frequency (e.g., 1 kHz, -6 dBFS)
2. Call `EQProcessor::process()` with the sine wave as input
3. Measure output RMS level
4. Compare to expected: input_level_dB + `gain_db` configured for that band (±0.5 dB tolerance)

Test cases to cover:
- All bands at 0 dB → output == input (flat response)
- Single PEAKING band at +6 dB @ 1 kHz: output at 1 kHz shows +6 dB
- LOW_SHELF at -6 dB @ 100 Hz: output at 50 Hz shows -6 dB, output at 5 kHz shows 0 dB
- HIGH_SHELF at +3 dB @ 10 kHz: output at 15 kHz shows +3 dB
- EQ disabled: `process()` returns false, fallback path used, output identical to input
- Band enabled/disabled toggle: no click (delay lines preserved on param update)

---

## Extending the EQ

### Adding a new filter type

1. Add the enum value to `EQFilterType` in `config_schema.h`
2. Add a case in `EQProcessor::recompute_band_coef()` in `eq_processor.cpp`
3. Add the filter type string to the HTTP JSON serializer/deserializer
4. Update the config portal HTML band editor dropdown

### Increasing band count beyond 10

Update `EQ_MAX_BANDS` in `eq_processor.h` and increase `DeviceConfig` array size. Bump the NVS schema version in `config_schema.h` to force re-initialization on existing devices.

### PHONO RIAA equalization

A future dedicated RIAA curve can be implemented as a preset: 3 fixed biquad stages (a shelf + two peaks) matching the IEC 60268-7 standard. Apply by calling `EQProcessor::load_preset(EQPreset::RIAA)` — this populates all band configs and re-initializes coefficients without changing the runtime architecture.
