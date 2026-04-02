# Data Model: ESP-DSP Parametric Equalization

**Feature**: 003-esp-dsp-equalization  
**Date**: 2026-04-02

## Overview

This document defines the data entities, state, and relationships for the parametric equalizer feature. The model extends `DeviceConfig` (existing `config_schema.h`) with EQ configuration and introduces `EQProcessor` as the audio-path runtime component.

---

## Entities

### 1. EQFilterType (enum)

Filter type for an EQ band.

```cpp
enum class EQFilterType : uint8_t {
    PEAKING   = 0,   // Parametric peak/dip bell curve (most common mid-band)
    LOW_SHELF = 1,   // Boost/cut all frequencies below corner frequency
    HIGH_SHELF = 2,  // Boost/cut all frequencies above corner frequency
    LOW_PASS  = 3,   // Cut above corner frequency (no gain parameter)
    HIGH_PASS = 4,   // Cut below corner frequency (no gain parameter)
};
```

---

### 2. EQBandConfig

Configuration for a single EQ band. Stored in NVS; used to generate biquad coefficients at init and on update.

**Fields**:

| Field | Type | Range | Default | Description |
|---|---|---|---|---|
| `enabled` | `bool` | — | `false` | Whether this band is active in the signal chain |
| `filter_type` | `EQFilterType` | 0–4 | `PEAKING` | Filter shape |
| `frequency_hz` | `float` | 20.0–20000.0 | band-specific† | Center/corner frequency |
| `gain_db` | `float` | -24.0–+24.0 | `0.0` | Boost/cut in dB (ignored for LPF/HPF) |
| `q_factor` | `float` | 0.1–10.0 | `0.707` | Bandwidth/resonance (higher Q = narrower band) |

† Default frequencies for a 10-band graphic-style layout: 32, 64, 125, 250, 500, 1k, 2k, 4k, 8k, 16k Hz

**Validation Rules**:
- `frequency_hz` clamped to [20.0, `sample_rate / 2.0 - 1.0`] at load time
- `gain_db` clamped to [-24.0, +24.0] at load time
- `q_factor` clamped to [0.1, 10.0] at load time
- For `LOW_PASS` / `HIGH_PASS`: `gain_db` is ignored (forced to 0 internally)

**NVS Storage** (per band N = 0..9, 15-char key limit):

| NVS Key | Type | Notes |
|---|---|---|
| `eq_bN_en` | `uint8_t` | 0=disabled, 1=enabled |
| `eq_bN_type` | `uint8_t` | `EQFilterType` value |
| `eq_bN_freq` | `uint32_t` | `float` bitcast via `memcpy` |
| `eq_bN_gain` | `uint32_t` | `float` bitcast via `memcpy` |
| `eq_bN_q` | `uint32_t` | `float` bitcast via `memcpy` |

---

### 3. EQConfig

Top-level EQ configuration block. Added to `DeviceConfig` in `config_schema.h`.

**Fields**:

| Field | Type | Default | Description |
|---|---|---|---|
| `eq_enabled` | `bool` | `false` | Master EQ bypass switch (false = audio passes through unmodified) |
| `eq_bands[10]` | `EQBandConfig[10]` | flat (all 0 dB) | Per-band configuration |

**NVS Storage**:
- `eq_enabled` → `uint8_t`
- Per-band keys as described in `EQBandConfig` above (50 keys total)

**Default State** (factory default — flat, all bands 0 dB gain):

| Band | Frequency | Type | Gain | Q |
|---|---|---|---|---|
| 0 | 32 Hz | LOW_SHELF | 0.0 dB | 0.707 |
| 1 | 64 Hz | PEAKING | 0.0 dB | 1.4 |
| 2 | 125 Hz | PEAKING | 0.0 dB | 1.4 |
| 3 | 250 Hz | PEAKING | 0.0 dB | 1.4 |
| 4 | 500 Hz | PEAKING | 0.0 dB | 1.4 |
| 5 | 1000 Hz | PEAKING | 0.0 dB | 1.4 |
| 6 | 2000 Hz | PEAKING | 0.0 dB | 1.4 |
| 7 | 4000 Hz | PEAKING | 0.0 dB | 1.4 |
| 8 | 8000 Hz | PEAKING | 0.0 dB | 1.4 |
| 9 | 16000 Hz | HIGH_SHELF | 0.0 dB | 0.707 |

---

### 4. EQProcessor (runtime, audio path)

The in-process audio filter chain. Not persisted — reconstructed from `EQConfig` at startup and on config update.

**State** (all `DRAM_ATTR` static in `eq_processor.cpp`):

| Field | C Type | Size | Description |
|---|---|---|---|
| `coef[10][5]` | `float` | 200 B | Biquad coefficients per band {b0,b1,b2,a1,a2} |
| `w[10][4]` | `float` | 160 B | Stereo delay lines per band {wL0,wL1,wR0,wR1} |
| `float_buf[480]` | `float` | 1920 B | Working buffer for float32 LRLR interleaved frames |
| `enabled` | `bool` | — | Mirrors `eq_enabled` from config |
| `num_active_bands` | `uint8_t` | — | Count of `enabled` bands for loop optimization |

**Total static DRAM**: ≈ 2.3 KB

---

## Audio Data Flow

```
I²S DMA read (Core 0)
    │
    ▼
dma_buffer[1920]         ← uint8_t, 240 × 2ch × 4-byte I²S slots
    │
    ▼ EQProcessor::convert_i2s_to_float()
float_buf[480]           ← float32, normalized [-1.0, +1.0], LRLR interleaved
    │                       (240 L/R frame pairs)
    ▼ EQProcessor::process()   [if eq_enabled && active bands > 0]
float_buf[480]           ← float32, EQ-processed, in-place
    │
    ▼ EQProcessor::convert_float_to_24bit()
converted_buffer[1440]   ← uint8_t, 240 × 2ch × 3-byte packed 24-bit
    │
    ▼
AudioBuffer::write()     ← ring buffer → HTTP streaming clients
```

**If EQ is disabled** or no bands are enabled: `float_buf` step is skipped; the existing dma_buffer → 24-bit packing path runs as before (no performance penalty).

---

## State Transitions

### EQProcessor Initialization

```
UNINITIALIZED
    │ EQProcessor::init(config, sample_rate)
    ▼
INITIALIZED
    ├─► [process() called per DMA block in audio task]
    │       └─► runs biquad chain or bypass
    │
    └─► [update_band() called from HTTP handler]
            └─► recomputes coef[b], does NOT reset w[b]
```

### EQ Enable/Disable (Master Switch)

```
ENABLED ──── POST /eq { "eq_enabled": false } ──→ DISABLED (bypass, no processing)
DISABLED ─── POST /eq { "eq_enabled": true }  ──→ ENABLED  (recompute all active bands)
```

When transitioning from DISABLED → ENABLED: `memset(w, 0)` to clear all delay line state, then recompute all active band coefficients from stored config. This avoids transient artifacts from stale delay state.

---

## HTTP API Response Shapes

### GET /eq → EQStatusResponse

```json
{
  "eq_enabled": true,
  "sample_rate": 48000,
  "bands": [
    {
      "index": 0,
      "enabled": true,
      "filter_type": "LOW_SHELF",
      "frequency_hz": 80.0,
      "gain_db": 3.0,
      "q_factor": 0.707
    }
  ]
}
```

### POST /eq (full update)

```json
{
  "eq_enabled": true,
  "bands": [
    {
      "index": 0,
      "enabled": true,
      "filter_type": "PEAKING",
      "frequency_hz": 1000.0,
      "gain_db": -3.0,
      "q_factor": 1.4
    }
  ]
}
```

All fields except `index` are optional — omitted fields are unchanged.

### POST /eq/reset

No request body. Resets all bands to factory defaults (0 dB, disabled), persists to NVS.

---

## NVS Schema Extension

New keys added to the existing `"config"` NVS namespace (alongside existing `wifi_ssid`, `mqtt_*`, etc.):

```cpp
namespace NVSKeys {
    // EQ Configuration (new in feature 003)
    constexpr const char* EQ_ENABLED  = "eq_enabled";   // uint8
    // Per-band: N = "0".."9" appended at runtime
    constexpr const char* EQ_BAND_EN   = "eq_bN_en";    // template: replace N
    constexpr const char* EQ_BAND_TYPE = "eq_bN_type";
    constexpr const char* EQ_BAND_FREQ = "eq_bN_freq";
    constexpr const char* EQ_BAND_GAIN = "eq_bN_gain";
    constexpr const char* EQ_BAND_Q    = "eq_bN_q";
}
```

**CRC32**: The existing `DeviceConfig::crc32` field covers the full struct. Adding `EQBandConfig eq_bands[10]` to `DeviceConfig` requires a schema version bump (`version` field) so stale NVS configs are detected and defaulted correctly.
