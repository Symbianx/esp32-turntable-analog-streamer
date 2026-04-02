# Implementation Plan: ESP-DSP Parametric Equalization

**Branch**: `003-esp-dsp-equalization` | **Date**: 2026-04-02 | **Spec**: N/A (spec to be created)  
**Input**: User request — add parametric EQ to the turntable audio streamer using esp-dsp

## Summary

Add a configurable multi-band parametric equalizer to the ESP32 audio capture pipeline. The EQ runs on Core 0 (audio task) immediately after I²S DMA read, processing float32 stereo audio using the `espressif/esp-dsp` biquad IIR filter library before packing to 24-bit and writing to the ring buffer. Up to 10 configurable bands support peaking EQ, low shelf, and high shelf filter types. Band parameters (frequency, gain, Q) are persisted in NVS and exposed via HTTP REST API + config portal web UI.

## Technical Context

**Language/Version**: C++17 / ESP-IDF 5.3.4  
**Primary Dependencies**: `espressif/esp-dsp` (biquad IIR filters, SIMD-accelerated via Xtensa ae32/aes3 FPU), existing ESP-IDF stack  
**Storage**: NVS (Non-Volatile Storage) — extends existing `DeviceConfig` schema  
**Testing**: ESP-IDF Unity test framework — sine wave input/output THD/gain measurement per band  
**Target Platform**: ESP32 / ESP32-S3 @ 240 MHz, FreeRTOS, Core 0 audio task  
**Project Type**: Single embedded C++ project  
**Performance Goals**: EQ processing must complete within audio task budget (<60% Core 0 CPU). 10-band stereo EQ at 48kHz/240-frame blocks ≈ 0.7 ms per block @ 240 MHz — well within budget  
**Constraints**: No heap allocation in audio path; all EQ buffers static (`DRAM_ATTR`); coefficient generation only at init/config time (not in ISR); delay lines in DRAM to avoid cache misses  
**Scale/Scope**: Single device; 10 EQ bands; 5 filter types; NVS-persisted; HTTP configurable

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- [x] **Real-Time Performance**: EQ processing is inserted synchronously in the existing audio task loop (Core 0). No new tasks, no blocking. Processing time ≈ 0.7 ms for 10-band stereo @ 240 MHz — deterministic and within budget.
- [x] **Audio Quality**: Processing stays in float32 throughout the EQ chain. Single int↔float conversion pair (32-bit I²S slot → float32 → 24-bit packed) — no intermediate quantization. Sample rate unchanged (44.1/48/96 kHz). THD impact negligible (<0.001% for shelf/peak filters at moderate gain).
- [x] **Resource Efficiency**: All EQ state is static `DRAM_ATTR` (no heap). Float working buffers: 2×240×4 = 1920 bytes. Coefficient+delay state: 10 bands × 5 floats + 4 floats stereo delay = 90 floats = 360 bytes. Total EQ static footprint ≈ 2.3 KB DRAM. Flash usage impact minimal.
- [x] **Deterministic Timing**: Coefficient updates (from HTTP handler on Core 1) write to `DRAM_ATTR float coef[5]` arrays; 32-bit float writes are atomic on Xtensa. No mutex needed between config writer and audio reader for coefficient updates. Delay lines are exclusive to Core 0 audio task — no sharing.
- [x] **Fail-Safe Operation**: If EQ is disabled (master switch) or initialization fails, pipeline passes audio through unmodified. Invalid band parameters are clamped to safe ranges at config-load time.
- [x] **Hardware Constraints**: Runs on Core 0 audio task with existing pinned affinity. Uses `dsps_biquad_sf32` (Xtensa ae32/aes3 FPU assembly) for SIMD acceleration. Float buffers placed in DRAM (not PSRAM) to avoid cache miss latency in the audio loop.
- [x] **Testing Standards**: Unit tests verify per-band gain accuracy (sine wave in → measure output level vs. expected gain), flat response at 0 dB, and bypass behavior.

## Project Structure

### Documentation (this feature)

```text
specs/003-esp-dsp-equalization/
├── plan.md              # This file
├── research.md          # Phase 0: esp-dsp API research
├── data-model.md        # Phase 1: EQ data entities and state
├── quickstart.md        # Phase 1: Developer integration guide
├── contracts/
│   └── eq-api.yaml      # OpenAPI 3.0 — EQ REST endpoints
└── tasks.md             # Phase 2 output (/speckit.tasks — not yet created)
```

### Source Code (repository root)

```text
main/
├── audio/
│   ├── eq_processor.h          # NEW — EQProcessor class (init, process, update_band)
│   ├── eq_processor.cpp        # NEW — biquad chain, float conversion, DRAM buffers
│   ├── audio_capture.cpp       # MODIFIED — insert EQ processing between DMA read and ring buffer write
│   ├── audio_buffer.h          # unchanged
│   ├── i2s_master.h            # unchanged
│   └── pcm1808_driver.h        # unchanged
├── network/
│   ├── http_server.cpp         # MODIFIED — add GET /eq, POST /eq, POST /eq/reset handlers
│   └── config_portal_html.h    # MODIFIED — add EQ band editor UI section
├── storage/
│   └── nvs_config.cpp          # MODIFIED — load/save EQ bands in NVS
├── config_schema.h             # MODIFIED — add EQBandConfig[10] + eq_enabled to DeviceConfig
├── CMakeLists.txt              # MODIFIED — add eq_processor.cpp to SRCS
└── idf_component.yml           # MODIFIED — add espressif/esp-dsp dependency
```

**Structure Decision**: Single embedded project. EQ is a pure audio-path addition — new files confined to `main/audio/`, config extended in-place, HTTP handled in existing `http_server.cpp`.

## Complexity Tracking

> No Constitution violations. No additional complexity required.
