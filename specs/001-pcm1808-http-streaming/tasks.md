# Tasks: PCM1808 ADC to HTTP Audio Streaming

**Input**: Design documents from `/specs/001-pcm1808-http-streaming/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/http-api.yaml, quickstart.md

**Tests**: Not explicitly requested in feature spec. Test tasks omitted. Audio quality validation is part of hardware-in-loop testing in the Polish phase.

**Organization**: Tasks grouped by user story for independent implementation and testing.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Path Conventions

- **ESP-IDF project**: `main/` for source, `test/` for tests, config at root
- Header files co-located with source (`.h` alongside `.cpp`)

---

## Phase 1: Setup (Project Initialization)

**Purpose**: PlatformIO ESP-IDF project scaffolding and build configuration

- [X] T001 Create PlatformIO project config with esp-wrover-kit board and ESP-IDF framework in platformio.ini
- [X] T002 Create sdkconfig.defaults with PSRAM enabled, WiFi, I²S, NVS, mDNS, and HTTP server Kconfig options
- [X] T003 Create custom partition table with NVS (24KB), OTA data (8KB), app0 (1.5MB), app1 (1.5MB) in partitions.csv
- [X] T004 [P] Create main/CMakeLists.txt registering all source files and include dirs via idf_component_register
- [X] T005 [P] Create main/config_schema.h with DeviceConfig struct, AudioStream state struct, ClientConnection struct, SystemMetrics struct, and WavHeader struct per data-model.md
- [X] T006 Verify project builds cleanly with `pio run` (empty app_main, PSRAM init, no warnings)

**Checkpoint**: Project compiles and flashes to ESP32-DevKitC-WROVER. PSRAM detected in boot log.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core infrastructure required by ALL user stories — audio pipeline, task management, error handling

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [X] T007 Implement FreeRTOS task manager with core-pinned task creation, priority constants, and stack sizes in main/system/task_manager.cpp
- [X] T008 [P] Implement error handler with ESP_LOG macros, error counters, and serial output in main/system/error_handler.cpp
- [X] T009 [P] Implement watchdog timer setup (10s timeout) for audio and network tasks in main/system/watchdog.cpp
- [X] T010 Implement I²S master driver: RX channel init, STD Philips mode, APLL clock, GPIO0/26/25/27 pinout, 6×240 DMA buffers, 24-bit 48kHz default in main/audio/i2s_master.cpp
- [X] T011 Implement PCM1808 driver: power-up sequence (500ms settling), RST pin control, clock validation in main/audio/pcm1808_driver.cpp
- [X] T012 Implement SPMC lock-free ring buffer in PSRAM: atomic write pointer, per-client read pointers, overrun policy, FreeRTOS task notification on write in main/audio/audio_buffer.cpp
- [X] T013 Implement audio capture task: i2s_channel_read loop on Core 0, DMA → ring buffer write, clipping detection, underrun/overrun counters in main/audio/audio_capture.cpp
- [X] T014 Implement NVS config manager: load/save DeviceConfig blob with CRC32 validation, factory defaults fallback, schema versioning in main/storage/nvs_config.cpp
- [X] T015 Implement app_main entry point: init NVS, init PSRAM ring buffer, start task manager, start audio pipeline in main/main.cpp
- [X] T016 Verify foundational build: flash to device, confirm I²S master clocks on GPIO0/26/25, serial log shows DMA capture running, ring buffer filling

**Checkpoint**: Audio capture pipeline running on Core 0. DMA reads from PCM1808 into PSRAM ring buffer. Serial log shows frame counts incrementing. No user-facing features yet.

---

## Phase 3: User Story 1 — Capture and Stream Turntable Audio (Priority: P1) 🎯 MVP

**Goal**: Stream live audio from PCM1808 ADC over HTTP as uncompressed 24-bit WAV. Users open http://[ip]:8080/stream in VLC and hear their turntable.

**Independent Test**: Connect turntable, power on ESP32 (with hardcoded WiFi creds in sdkconfig), open http://[esp32-ip]:8080/stream in VLC. Audio plays within 2 seconds with no audible artifacts during a 10-minute listening test.

### Implementation for User Story 1

- [X] T017 [US1] Implement WiFi STA connection with hardcoded credentials from NVS config, WPA2-PSK mode, auto-reconnect logic in main/network/wifi_manager.cpp
- [X] T018 [US1] Implement WAV header generator: build 44-byte header from current sample rate, 24-bit, stereo, indefinite length (0xFFFFFFFF) in main/network/stream_handler.cpp
- [X] T019 [US1] Implement HTTP server: init esp_http_server on port 8080, register /stream route, configure max_open_sockets=4 in main/network/http_server.cpp
- [X] T020 [US1] Implement stream handler: client connection tracking (max 3), per-client read pointer allocation, WAV header send, chunked audio data loop reading from ring buffer, disconnect detection in main/network/stream_handler.cpp
- [X] T021 [US1] Implement HTTP 503 rejection when max clients exceeded with Retry-After header in main/network/stream_handler.cpp
- [X] T022 [US1] Implement WiFi disconnect resilience: audio capture continues to ring buffer during WiFi loss, auto-reconnect within 30s, resume streaming in main/network/wifi_manager.cpp
- [X] T023 [US1] Implement sample rate switching: stop I²S → reconfigure clock/slot → disconnect clients → reset ring buffer → restart in main/audio/i2s_master.cpp
- [X] T024 [US1] Wire up networking tasks on Core 1 in main/main.cpp: start WiFi manager, HTTP server, stream handler tasks with correct priorities
- [ ] T025 [US1] End-to-end validation: flash firmware, connect to WiFi, open stream in VLC/foobar2000/Chrome, verify 24-bit stereo WAV playback, test 3 concurrent clients, verify 4th gets 503

**Checkpoint**: Core streaming works. User can open http://[esp32-ip]:8080/stream in VLC and hear turntable audio. WiFi creds must be pre-configured via serial/NVS. This is the MVP.

---

## Phase 4: User Story 2 — Device Discovery and Configuration (Priority: P2)

**Goal**: First-time setup via WiFi captive portal. User connects to ESP32 AP, configures WiFi + sample rate, device joins home network. mDNS discovery as esp32-audio-stream.local.

**Independent Test**: Factory reset device (erase NVS), power on, connect to AP "ESP32-Audio-Stream-XXXX", navigate to http://192.168.4.1, configure WiFi and sample rate, verify device connects to home network and stream URL is accessible.

### Implementation for User Story 2

- [X] T026 [US2] Implement SoftAP mode: create AP "ESP32-Audio-Stream-[MAC]", DHCP server, 192.168.4.1 gateway in main/network/wifi_manager.cpp
- [X] T027 [US2] Implement AP/STA mode switching: start AP if no stored credentials or STA connection fails after 30s in main/network/wifi_manager.cpp
- [X] T028 [P] [US2] Implement DNS redirect server: respond to all DNS queries with 192.168.4.1 for captive portal detection in main/network/config_portal.cpp
- [X] T029 [P] [US2] Implement WiFi scan endpoint: GET /wifi/scan returns JSON array of discovered networks with SSID, RSSI, auth type in main/network/config_portal.cpp
- [X] T030 [US2] Implement configuration portal HTML page: mobile-responsive (320px min), SSID dropdown from scan, password field, sample rate selector (44.1/48/96 kHz), device name, save button in main/network/config_portal.cpp
- [X] T031 [US2] Implement GET /config endpoint returning current config (password masked) as JSON in main/network/config_portal.cpp
- [X] T032 [US2] Implement POST /config endpoint: validate input, save to NVS, trigger WiFi reconnect with new credentials, return success/error JSON in main/network/config_portal.cpp
- [X] T033 [US2] Implement captive portal root handler: serve config page in AP mode, redirect to /status in STA mode for GET / in main/network/config_portal.cpp
- [ ] T034 [P] [US2] Implement mDNS service advertisement: hostname "esp32-audio-stream", HTTP service on port 8080 in main/network/wifi_manager.cpp *(stub only — deferred due to ESP-IDF v5.3.4 mdns component incompatibility)*
- [X] T035 [US2] Register config portal routes in HTTP server and wire AP mode startup in main/main.cpp
- [ ] T036 [US2] End-to-end validation: erase NVS, power on, connect to AP, configure WiFi via portal, verify STA connection, verify mDNS resolution, verify stream accessible via esp32-audio-stream.local:8080/stream

**Checkpoint**: Full first-time setup experience works. Device creates AP, user configures WiFi via browser, device joins network, discoverable via mDNS.

---

## Phase 5: User Story 3 — Audio Quality Monitoring and Diagnostics (Priority: P3)

**Goal**: Real-time status page at /status showing audio metrics, system health, and network info. Auto-refreshes every 5 seconds.

**Independent Test**: Navigate to http://[esp32-ip]:8080/status, verify all metrics displayed (sample rate, bit depth, buffer fill, CPU, heap, WiFi RSSI, uptime), confirm values update on refresh.

### Implementation for User Story 3

- [X] T037 [US3] Implement system metrics collector: CPU usage per core (using FreeRTOS idle hook), heap stats, uptime, WiFi RSSI polling in main/system/task_manager.cpp
- [X] T038 [US3] Implement audio metrics aggregation: buffer fill %, underrun/overrun counts, clipping status, total frames captured in main/audio/audio_capture.cpp
- [X] T039 [US3] Implement GET /status JSON endpoint: assemble DeviceStatus response from SystemMetrics + AudioStream + network state per contracts/http-api.yaml schema in main/network/http_server.cpp
- [X] T040 [P] [US3] Implement GET /status HTML endpoint: mobile-responsive status page with auto-refresh meta tag (5s), color-coded health indicators, stream URL with copy button in main/network/http_server.cpp
- [X] T041 [US3] Implement content negotiation for /status: return JSON when Accept: application/json, HTML otherwise in main/network/http_server.cpp
- [X] T042 [US3] Implement clipping indicator: detect sustained near-max samples (>1s), set flag in AudioStream state, display warning on status page in main/audio/audio_capture.cpp
- [X] T043 [US3] Implement CPU usage warning: highlight >50% Core 0 usage with visual warning on HTML status page in main/network/http_server.cpp
- [ ] T044 [US3] End-to-end validation: navigate to /status in browser, verify all metrics present and updating, check JSON response format matches contracts/http-api.yaml DeviceStatus schema

**Checkpoint**: Full diagnostics dashboard operational. All metrics visible, auto-refreshing, mobile-friendly.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Hardening, edge cases, documentation, and cross-story integration

- [X] T045 [P] Implement edge case: stream silence (zero samples) when no audio source connected without crash in main/audio/audio_capture.cpp
- [X] T046 [P] Implement edge case: I²S failure detection (no data within 5s), log error, display on status page, attempt reset/recovery in main/audio/pcm1808_driver.cpp
- [X] T047 [P] Implement edge case: graceful sample rate change while streaming — disconnect clients, reconfigure I²S, new stream within 3s in main/audio/i2s_master.cpp
- [X] T048 Implement serial logging for all state transitions: boot, WiFi connect/disconnect, client connect/disconnect, errors, sample rate changes in main/system/error_handler.cpp
- [X] T049 [P] Add CORS headers (Access-Control-Allow-Origin: *) to all HTTP responses for browser compatibility in main/network/http_server.cpp
- [X] T050 Review and validate all GPIO assignments against ESP32-WROVER datasheet for pin conflicts (GPIO0 boot strapping, PSRAM pins) — document in README.md
- [ ] T051 Run quickstart.md validation: follow quickstart.md end-to-end on fresh device, verify all steps work, update any discrepancies
- [ ] T052 Final stress test: 4-hour continuous streaming at 48kHz, monitor for underruns, memory leaks, WiFi stability via /status endpoint

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — start immediately
- **Foundational (Phase 2)**: Depends on Phase 1 completion — BLOCKS all user stories
- **US1 (Phase 3)**: Depends on Phase 2 — core audio streaming MVP
- **US2 (Phase 4)**: Depends on Phase 2 + T017 (WiFi manager base from US1)
- **US3 (Phase 5)**: Depends on Phase 2 + T019 (HTTP server base from US1)
- **Polish (Phase 6)**: Depends on all user stories being complete

### User Story Dependencies

- **User Story 1 (P1)**: Can start after Foundational (Phase 2). No dependencies on other stories. This is the MVP.
- **User Story 2 (P2)**: Extends wifi_manager.cpp (T017) and http_server.cpp (T019) from US1. Best done after US1 but independently testable with hardcoded audio pipeline.
- **User Story 3 (P3)**: Extends http_server.cpp (T019) from US1. Reads metrics from audio pipeline (Phase 2). Can start after US1 HTTP server is in place.

### Within Each User Story

- Models/structs before services
- Core logic before HTTP handlers
- HTTP handlers before integration wiring
- Integration wiring before end-to-end validation
- Story complete before moving to next priority

### Parallel Opportunities

Within Phase 1:
- T004 and T005 can run in parallel (CMakeLists.txt + config_schema.h)

Within Phase 2:
- T008 and T009 can run in parallel (error_handler + watchdog)

Within Phase 4 (US2):
- T028, T029, and T034 can run in parallel (DNS server, scan endpoint, mDNS)

Within Phase 5 (US3):
- T040 can run in parallel with T039 (HTML and JSON status pages)

Within Phase 6:
- T045, T046, T047, and T049 can all run in parallel (independent edge cases)

---

## Parallel Example: Phase 2 (Foundational)

```bash
# These can run in parallel (different files, no dependencies):
Task: "T008 Implement error handler in main/system/error_handler.cpp"
Task: "T009 Implement watchdog timer in main/system/watchdog.cpp"

# Then sequentially (T010 depends on task_manager from T007):
Task: "T010 Implement I²S master driver in main/audio/i2s_master.cpp"
Task: "T011 Implement PCM1808 driver in main/audio/pcm1808_driver.cpp"
Task: "T012 Implement ring buffer in main/audio/audio_buffer.cpp"
Task: "T013 Implement audio capture task in main/audio/audio_capture.cpp"
```

## Parallel Example: User Story 2

```bash
# These can run in parallel (different concerns in same/different files):
Task: "T028 DNS redirect server in main/network/config_portal.cpp"
Task: "T029 WiFi scan endpoint in main/network/config_portal.cpp"
Task: "T034 mDNS advertisement in main/network/wifi_manager.cpp"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup → Project builds and flashes
2. Complete Phase 2: Foundational → Audio capture pipeline running
3. Complete Phase 3: User Story 1 → Stream accessible in VLC
4. **STOP and VALIDATE**: Play vinyl record, open in VLC, listen for 10 minutes
5. Ship MVP if quality is acceptable

### Incremental Delivery

1. Setup + Foundational → Audio pipeline running (no networking yet)
2. Add User Story 1 → HTTP streaming works → **MVP deployed**
3. Add User Story 2 → WiFi portal works → Consumer-friendly setup
4. Add User Story 3 → Status page works → Diagnostics available
5. Polish → Edge cases, stress testing, documentation

---

## Notes

- [P] tasks = different files, no dependencies on incomplete tasks
- [Story] label maps task to specific user story for traceability
- ESP-IDF build system: all source files must be listed in main/CMakeLists.txt
- GPIO0 is a boot strapping pin — do not add external pull-down
- 24-bit DMA: frame_num and buffer sizes must be multiples of 3
- PSRAM: use heap_caps_malloc(size, MALLOC_CAP_SPIRAM) for ring buffer
- Ring buffer atomic operations: __atomic_store_n / __atomic_load_n with RELEASE/ACQUIRE ordering
- Commit after each task or logical group
- Stop at any checkpoint to validate independently
