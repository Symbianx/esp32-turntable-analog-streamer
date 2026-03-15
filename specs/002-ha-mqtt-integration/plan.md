# Implementation Plan: Home Assistant MQTT Integration

**Branch**: `002-ha-mqtt-integration` | **Date**: 2026-02-24 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/002-ha-mqtt-integration/spec.md`

## Summary

Integrate the ESP32 turntable audio streamer with Home Assistant via MQTT, enabling automatic device discovery, real-time playback status monitoring, and stream URL publishing. The implementation extends Feature 001's web server, NVS storage, and audio capture infrastructure with a new MQTT client service (ESP-MQTT), RMS-based audio detection, and web-based configuration UI. Users configure broker credentials and audio threshold through a new `/mqtt-settings` page, with optional factory reset capability to erase all configuration and reboot to AP mode.

## Technical Context

**Language/Version**: C++17 (ESP-IDF framework, `std=gnu++17`)  
**Primary Dependencies**: ESP-IDF v5.x, ESP-MQTT component (`espressif/mqtt ^1.0.0`), FreeRTOS, LWIP TCP/IP stack  
**Storage**: NVS (Non-Volatile Storage) for MQTT credentials and audio threshold configuration  
**Testing**: Manual integration testing (no automated test framework specified)  
**Target Platform**: ESP32-DevKitC-WROVER (ESP32 dual-core, 8MB PSRAM, WiFi)  
**Project Type**: Single embedded firmware project (ESP-IDF component structure)  
**Performance Goals**: <2s playback detection latency, <5% CPU overhead for MQTT, <1 msg/sec publish rate  
**Constraints**: <50KB heap for MQTT client, no blocking operations in audio path (Core 1), deterministic I2S timing  
**Scale/Scope**: Single-device deployment, local network MQTT broker, 1-3 concurrent HTTP stream clients (existing)

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

Review compliance with [Constitution](../../.specify/memory/constitution.md):

- [x] **Real-Time Performance**: Does design maintain deterministic audio path timing?
  - ✅ MQTT client runs on Core 0 (non-audio core)
  - ✅ RMS calculation in audio path uses subsampling (every 8th frame) for <10% overhead
  - ✅ Atomic boolean flag for playback status (lock-free communication)
  - ✅ No blocking MQTT calls in audio capture task
  
- [x] **Audio Quality**: Are sample rate/depth requirements (≥44.1kHz, ≥16-bit) met?
  - ✅ No changes to existing I2S configuration (44.1/48/96kHz, 24-bit)
  - ✅ RMS calculation operates on captured samples without modification
  - ✅ Audio streaming path unchanged from Feature 001
  
- [x] **Resource Efficiency**: Does heap/stack/CPU usage stay within ESP32 limits?
  - ✅ MQTT client: ~40-50KB heap (within 8MB PSRAM budget)
  - ✅ RMS state: ~4 bytes static memory
  - ✅ Configuration: ~256 bytes NVS
  - ✅ Total increase: <60KB (<1% of available memory)
  
- [x] **Deterministic Timing**: Are lock-free patterns used in audio path?
  - ✅ `std::atomic<bool>` for playback status (lock-free)
  - ✅ No mutexes or semaphores in audio capture task
  - ✅ MQTT task polls atomic flag (no callbacks into audio path)
  
- [x] **Fail-Safe Operation**: Are error recovery and graceful degradation designed?
  - ✅ Device continues streaming if MQTT unavailable (FR-010)
  - ✅ MQTT reconnection with exponential backoff (5s to 5min)
  - ✅ WiFi disconnect: buffered status updates resume on reconnect
  - ✅ Invalid broker config: device operates normally without MQTT
  
- [x] **Hardware Constraints**: Are I²S codec, DMA, and core affinity properly utilized?
  - ✅ No changes to I2S master mode configuration
  - ✅ No changes to DMA buffer management
  - ✅ Core 0: MQTT, HTTP (non-real-time)
  - ✅ Core 1: Audio capture, RMS detection (real-time)
  
- [x] **Testing Standards**: Are audio quality benchmarks (THD, SNR, latency) defined?
  - ✅ Existing audio benchmarks from Feature 001 remain applicable
  - ✅ No audio processing changes that would affect THD/SNR
  - ✅ Success criteria define <2s detection latency (SC-002)
  - ✅ Performance criteria: <5% CPU, <50KB heap (SC-Performance)

*No constitution violations. All gates passed.*

## Project Structure

### Documentation (this feature)

```text
specs/[###-feature]/
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
├── audio/
│   ├── i2s_master.cpp/.h          # Existing: I2S configuration
│   ├── audio_buffer.cpp/.h        # Existing: Circular buffer
│   └── audio_capture.cpp/.h       # EXTEND: Add RMS calculation + threshold check
├── network/
│   ├── wifi_manager.cpp/.h        # Existing: WiFi connection
│   ├── config_portal.cpp/.h       # Existing: AP mode config
│   ├── http_server.cpp/.h         # EXTEND: Add /mqtt-settings endpoint
│   └── mqtt_client.cpp/.h         # NEW: MQTT client service
├── storage/
│   └── nvs_config.cpp/.h          # EXTEND: Add MQTT config fields
├── system/
│   ├── rgb_led.cpp/.h             # Existing: Status LED
│   └── watchdog.cpp/.h            # Existing: Task monitoring
├── config_schema.h                 # EXTEND: Add MQTT NVS keys
├── main.cpp                        # EXTEND: Initialize MQTT task
└── CMakeLists.txt                  # Existing: Build configuration

managed_components/
├── espressif__mdns/               # Existing: mDNS service
├── espressif__led_strip/          # Existing: RGB LED driver
└── espressif__mqtt/               # NEW: MQTT client library

specs/002-ha-mqtt-integration/
├── spec.md                         # Feature specification
├── plan.md                         # This file
├── research.md                     # Phase 0 output (complete)
├── data-model.md                   # Phase 1 output (pending)
├── quickstart.md                   # Phase 1 output (pending)
└── contracts/                      # Phase 1 output (pending)
```

**Structure Decision**: Single embedded firmware project using ESP-IDF component structure. This is a standard ESP32 project layout with components organized by domain (audio, network, storage, system). Feature 002 extends existing components (`audio_capture`, `http_server`, `nvs_config`) and adds a new MQTT client service (`mqtt_client`). No refactoring or abstraction layers needed; clean extension points already exist from Feature 001.

## Complexity Tracking

> **Fill ONLY if Constitution Check has violations that must be justified**

| Violation | Why Needed | Simpler Alternative Rejected Because |
|-----------|------------|-------------------------------------|
| [e.g., 4th project] | [current need] | [why 3 projects insufficient] |
| [e.g., Repository pattern] | [specific problem] | [why direct DB access insufficient] |
