# Implementation Plan: PCM1808 ADC to HTTP Audio Streaming

**Branch**: `001-pcm1808-http-streaming` | **Date**: 2026-02-13 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/001-pcm1808-http-streaming/spec.md`

**Note**: This template is filled in by the `/speckit.plan` command. See `.specify/templates/commands/plan.md` for the execution workflow.

## Summary

Build an ESP32 C++ application that captures stereo audio from a PCM1808 24-bit ADC via I²S (ESP32 as master) and streams it over HTTP in uncompressed 24-bit PCM/WAV format. The system provides WiFi configuration portal, real-time status monitoring, and maintains <100ms latency with >95dB SNR. Target platform: ESP32-DevKitC-WROVER (8MB PSRAM) running ESP-IDF.

## Technical Context

**Language/Version**: C++17, ESP-IDF v5.x (Espressif IoT Development Framework)  
**Primary Dependencies**: ESP-IDF (WiFi, I²S, HTTP server, NVS), FreeRTOS (task management, queues), lwIP (TCP/IP stack)  
**Storage**: NVS (Non-Volatile Storage) for WiFi credentials and configuration; no external database  
**Testing**: ESP-IDF Unity test framework, hardware-in-loop tests with audio analyzer, VLC/foobar2000 client compatibility tests  
**Target Platform**: ESP32-DevKitC-WROVER module (8MB PSRAM, dual-core Xtensa LX6 @ 240MHz)  
**Project Type**: Single embedded firmware project  
**Hardware**: PCM1808 24-bit ADC (I²S slave), GPIO pinout: SCK=GPIO0, BCK=GPIO26, LRCK=GPIO25, DOUT=GPIO27  
**Performance Goals**: 
- <100ms end-to-end latency (analog input → HTTP output)
- THD+N <-90dB, SNR ≥95dB
- CPU ≤60% (Core 0 audio), ≤70% (Core 1 network)
- Zero buffer underruns during 4-hour stress test

**Constraints**: 
- Heap usage ≤416KB (80% of 520KB internal SRAM)
- 8MB PSRAM available for audio buffers (≥2 seconds network buffer)
- WiFi: WPA2-PSK only
- No authentication (trusted local network)
- 24-bit PCM output (preserves full ADC resolution, ~2.3 Mbps at 48kHz)

**Scale/Scope**: 
- 3 concurrent HTTP clients maximum
- Sample rates: 44.1kHz, 48kHz, 96kHz (user-selectable)
- Single device, no cloud integration
- mDNS discovery as "esp32-audio-stream.local"

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

*Post-Phase 1 re-check: ✅ All gates pass. Data model uses PSRAM ring buffer (resource efficient), contracts expose no auth (trusted local network per spec), quickstart confirms correct GPIO pinout.*

Review compliance with [Constitution](../../.specify/memory/constitution.md):

- [x] **Real-Time Performance**: I²S DMA with 6×240 frame buffers ensures deterministic capture. Audio task pinned to Core 0 at highest priority (24). Network operations on Core 1 cannot block audio.
- [x] **Audio Quality**: 24-bit capture and 24-bit output preserves full PCM1808 resolution. Sample rates 44.1/48/96 kHz supported. APLL clock source for accurate 44.1kHz family.
- [x] **Resource Efficiency**: DMA buffers in internal SRAM (8,640 bytes). PSRAM ring buffer (~1.1MB of 8MB). No heap allocation in audio path. Zero-copy DMA → ring buffer pipeline.
- [x] **Deterministic Timing**: Lock-free SPMC ring buffer with atomic pointers. No mutexes in audio path. FreeRTOS task notifications for consumer wake-up (no polling).
- [x] **Fail-Safe Operation**: Watchdog on audio task. WiFi disconnect → continue capture to ring buffer. NVS config with CRC32 validation. ADC clipping detection via signal monitoring.
- [x] **Hardware Constraints**: ESP32-DevKitC-WROVER (8MB PSRAM). I²S master with PCM1808 slave. Core 0: audio (≤60% CPU). Core 1: networking (≤70% CPU). GPIO0/26/25/27 pinout confirmed.
- [x] **Testing Standards**: THD+N target -85dB (PCM1808 typical), SNR ≥95dB, latency <100ms. Unity test framework. Hardware-in-loop validation required.

*Document any violations with explicit justification in Complexity Tracking section below.*

## Project Structure

### Documentation (this feature)

```text
specs/001-pcm1808-http-streaming/
├── plan.md              # This file (/speckit.plan command output)
├── research.md          # Phase 0 output (/speckit.plan command)
├── data-model.md        # Phase 1 output (/speckit.plan command)
├── quickstart.md        # Phase 1 output (/speckit.plan command)
├── contracts/           # Phase 1 output (/speckit.plan command)
└── tasks.md             # Phase 2 output (/speckit.tasks command - NOT created by /speckit.plan)
```

### Source Code (repository root)

```text
main/
├── main.cpp                    # ESP-IDF application entry point
├── audio/
│   ├── i2s_master.cpp         # I²S master clock generation (SCK, BCK, LRCK)
│   ├── pcm1808_driver.cpp     # PCM1808 ADC initialization and control
│   ├── audio_capture.cpp      # DMA buffer management and audio capture loop
│   └── audio_buffer.cpp       # Lock-free ring buffer (PSRAM-backed)
├── network/
│   ├── wifi_manager.cpp       # WiFi STA/AP mode, WPA2-PSK, reconnection logic
│   ├── http_server.cpp        # HTTP server (/stream, /status endpoints)
│   ├── stream_handler.cpp     # Chunked HTTP streaming with WAV headers
│   └── config_portal.cpp      # Configuration web UI (AP mode)
├── storage/
│   ├── nvs_config.cpp         # NVS read/write with CRC32 validation
│   └── config_schema.h        # Configuration data structures
├── system/
│   ├── task_manager.cpp       # FreeRTOS task creation, priorities, core affinity
│   ├── watchdog.cpp           # Watchdog timer for audio and network tasks
│   └── error_handler.cpp      # Error logging and recovery logic
└── CMakeLists.txt             # ESP-IDF build configuration

components/
└── (custom components if needed for modularity)

test/
├── unit/                      # ESP-IDF Unity tests
│   ├── test_audio_buffer.cpp
│   ├── test_i2s_master.cpp
│   └── test_nvs_config.cpp
└── integration/               # Hardware-in-loop tests
    ├── test_audio_quality.cpp  # THD, SNR, frequency response
    └── test_streaming.cpp      # End-to-end latency, client compatibility

sdkconfig                       # ESP-IDF Kconfig configuration
sdkconfig.defaults              # Project-specific defaults
```

**Structure Decision**: Single ESP-IDF project structure following Espressif conventions. Main application code in `main/` organized by subsystem (audio, network, storage, system). Hardware abstraction limited to PCM1808 driver; all other I/O via ESP-IDF APIs. Testing via ESP-IDF Unity framework with hardware-in-loop validation on target device.

## Complexity Tracking

> **Fill ONLY if Constitution Check has violations that must be justified**

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| [e.g., 4th project] | [current need] | [why 3 projects insufficient] |
| [e.g., Repository pattern] | [specific problem] | [why direct DB access insufficient] |
