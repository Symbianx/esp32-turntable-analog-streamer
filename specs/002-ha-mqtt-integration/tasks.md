# Tasks: Home Assistant MQTT Integration

**Input**: Design documents from `/specs/002-ha-mqtt-integration/`  
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/

**Tests**: No automated tests requested in specification. Manual integration testing only.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing of each story.

## Format: `- [ ] [ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

## Path Conventions

ESP32 embedded firmware project structure:
- `main/` - Source code root
- `main/audio/` - Audio capture and processing
- `main/network/` - WiFi, HTTP, MQTT networking
- `main/storage/` - NVS configuration persistence
- `main/system/` - System services (LED, watchdog)

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Add ESP-MQTT dependency and extend configuration schema

- [X] T001 Add ESP-MQTT component dependency to main/idf_component.yml
- [X] T002 [P] Extend DeviceConfig struct with MQTTConfig and audio_threshold_db in config_schema.h
- [X] T003 [P] Add NVS key constants for MQTT settings in config_schema.h

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core infrastructure that MUST be complete before ANY user story can be implemented

**⚠️ CRITICAL**: No user story work can begin until this phase is complete

- [x] T004 Extend NVSConfig::load() to read MQTT config fields in storage/nvs_config.cpp
- [x] T005 Extend NVSConfig::save() to write MQTT config fields in storage/nvs_config.cpp
- [x] T006 [P] Implement NVSConfig::erase_all() for factory reset in storage/nvs_config.cpp
- [x] T007 [P] Create atomic<bool> playback_status global variable in audio/audio_capture.cpp
- [x] T008 [P] Implement RMS calculation with subsampling (every 8th frame) in audio/audio_capture.cpp
- [x] T009 [P] Implement threshold comparison and atomic flag update in audio/audio_capture.cpp
- [x] T010 [P] Add debounce logic for state transitions (200ms/500ms) in audio/audio_capture.cpp

**Checkpoint**: Foundation ready - user story implementation can now begin in parallel

---

## Phase 3: User Story 1 - Automatic Device Discovery (Priority: P1) 🎯 MVP

**Goal**: Device announces itself to Home Assistant via MQTT auto-discovery protocol, appearing automatically in the integrations page within 30 seconds of connection.

**Independent Test**: Power on device with valid broker credentials, check Home Assistant integrations page for "ESP32 Turntable Streamer" device within 30 seconds.

### Implementation for User Story 1

- [X] T011 [P] [US1] Create MQTTClient class header in main/network/mqtt_service.h
- [X] T012 [US1] Implement MQTTClient::init() with esp_mqtt_client_config_t setup in main/network/mqtt_client.cpp
- [X] T013 [US1] Implement MQTTClient::start() to begin connection loop in main/network/mqtt_client.cpp
- [X] T014 [US1] Implement MQTT event handler for CONNECTED, DISCONNECTED events in main/network/mqtt_client.cpp
- [X] T015 [US1] Generate DeviceIdentity (unique_id from MAC address) in main/network/mqtt_client.cpp
- [X] T016 [US1] Implement MQTTClient::publish_discovery() with JSON payload construction in main/network/mqtt_client.cpp
- [X] T017 [US1] Set Last Will Testament (LWT) for availability topic in main/network/mqtt_client.cpp
- [X] T018 [US1] Publish availability "online" message after successful connect in main/network/mqtt_client.cpp
- [X] T019 [US1] Implement exponential backoff reconnection (5s to 300s max) in main/network/mqtt_client.cpp
- [X] T020 [US1] Initialize MQTTClient in main.cpp after WiFi connection established

**US1 Test Criteria**:
- Device discovered in Home Assistant within 30 seconds ✓
- Device metadata (name, model, manufacturer) displayed correctly ✓
- Availability status shows "online" after connect ✓
- Device rediscoverable after power cycle ✓

---

## Phase 4: User Story 2 - Audio Playback Status Monitoring (Priority: P1)

**Goal**: Device publishes binary sensor state ("playing"/"idle") based on audio signal detection, updating within 2 seconds of actual state change.

**Independent Test**: Play audio on turntable, verify Home Assistant binary sensor updates to "playing" within 2 seconds. Stop audio, verify updates to "idle" within 2 seconds.

### Implementation for User Story 2

- [X] T021 [P] [US2] Implement MQTTClient::publish_state(bool is_playing) in main/network/mqtt_client.cpp
- [X] T022 [P] [US2] Add FreeRTOS task MQTTClient::monitor_task() polling playback_status atomic flag in main/network/mqtt_client.cpp
- [X] T023 [US2] Implement state change detection with debounce (prevent rapid toggle) in main/network/mqtt_client.cpp
- [X] T024 [US2] Publish "playing" or "idle" to state topic with QoS 1 in main/network/mqtt_client.cpp
- [X] T025 [US2] Start monitor task after discovery published in main/network/mqtt_client.cpp
- [X] T026 [US2] Load audio_threshold_db from NVS and convert to linear amplitude in audio/audio_capture.cpp
- [X] T027 [US2] Validate threshold range [-60.0, -20.0] and clamp if needed in audio/audio_capture.cpp

**US2 Test Criteria**:
- Playback status updates within 2 seconds of audio start ✓
- Playback status updates within 2 seconds of audio stop ✓
- No false positives on silence or transient noise ✓
- Historical timestamps accurate in Home Assistant ✓

---

## Phase 5: User Story 3 - Stream URL Access (Priority: P2)

**Goal**: Device publishes stream URL as JSON attribute, automatically updating when IP address changes.

**Independent Test**: Retrieve stream_url attribute from Home Assistant, open in VLC, verify audio plays. Change device IP (DHCP renewal), verify attribute updates within 60 seconds.

### Implementation for User Story 3

- [X] T028 [P] [US3] Implement MQTTClient::publish_attributes(const char* stream_url) in main/network/mqtt_client.cpp
- [X] T029 [P] [US3] Construct JSON payload with stream_url field in main/network/mqtt_client.cpp
- [X] T030 [US3] Publish attributes to json_attributes_topic with QoS 1 in main/network/mqtt_client.cpp
- [X] T031 [US3] Register WiFi IP change event handler in main/network/mqtt_client.cpp
- [X] T032 [US3] Construct stream URL from current IP address and port 8080 in main/network/mqtt_client.cpp
- [X] T033 [US3] Publish initial stream URL after discovery in main/network/mqtt_client.cpp
- [X] T034 [US3] Republish stream URL on WiFi IP change event in main/network/mqtt_client.cpp

**US3 Test Criteria**:
- Stream URL attribute present in Home Assistant device ✓
- URL format correct: http://<ip>:8080/stream ✓
- Stream accessible via URL in media player ✓
- Attribute updates within 60 seconds of IP change ✓

---

## Phase 6: User Story 4 - MQTT Configuration via Web Interface (Priority: P2)

**Goal**: User-friendly web page for configuring MQTT broker credentials, testing connection, adjusting audio threshold with real-time feedback, and factory reset with confirmation.

**Independent Test**: Navigate to /mqtt-settings, enter broker credentials, click Test Connection (success), adjust threshold slider (observe current level), click Save (status shows Connected within 10s). Click Reset All Configuration, confirm, verify AP mode reboot.

### Implementation for User Story 4

- [X] T035 [P] [US4] Create HTML template for MQTT settings form in main/network/http_server.cpp
- [X] T036 [P] [US4] Implement GET /mqtt-settings handler rendering form with current config in main/network/http_server.cpp
- [X] T037 [P] [US4] Add threshold slider with JavaScript for real-time value display in main/network/http_server.cpp
- [X] T038 [P] [US4] Add current audio level indicator div with JavaScript auto-update in main/network/http_server.cpp
- [X] T039 [US4] Implement POST /mqtt-settings handler parsing form data in main/network/http_server.cpp
- [X] T040 [US4] Validate broker field (non-empty if enabled, max 127 chars) in main/network/http_server.cpp
- [X] T041 [US4] Validate port field (1-65535 range) in main/network/http_server.cpp
- [X] T042 [US4] Validate username/password (both set or both empty, max 63 chars) in main/network/http_server.cpp
- [X] T043 [US4] Validate and clamp threshold_db to [-60.0, -20.0] range in main/network/http_server.cpp
- [X] T044 [US4] Save valid config to NVS via NVSConfig::save() in main/network/http_server.cpp
- [X] T045 [US4] Trigger MQTTClient reconnect if broker settings changed in main/network/http_server.cpp
- [X] T046 [US4] Update audio threshold immediately (no reboot required) in main/network/http_server.cpp
- [X] T047 [US4] Return success page with current connection status in main/network/http_server.cpp
- [X] T048 [P] [US4] Implement POST /mqtt-settings/test handler for connection test in main/network/http_server.cpp
- [X] T049 [US4] Test connection with provided credentials (5s timeout) in main/network/http_server.cpp
- [X] T050 [US4] Return JSON response {"status": "success/error", "message": "..."} in main/network/http_server.cpp
- [X] T051 [P] [US4] Implement POST /mqtt-settings/reset handler with confirm check in main/network/http_server.cpp
- [X] T052 [US4] Validate confirm=yes parameter (400 Bad Request if missing) in main/network/http_server.cpp
- [X] T053 [US4] Call NVSConfig::erase_all() to wipe all settings in main/network/http_server.cpp
- [X] T054 [US4] Schedule reboot in 2 seconds and return confirmation page in main/network/http_server.cpp
- [X] T055 [US4] Add navigation link to /mqtt-settings from existing /status page in main/network/http_server.cpp
- [X] T056 [P] [US4] Implement GET /api/audio-level endpoint for real-time level polling in main/network/http_server.cpp

**US4 Test Criteria**:
- MQTT Settings link accessible from status page ✓
- Form renders with current configuration ✓
- Test Connection validates and shows success/error ✓
- Save persists config and connects within 10 seconds ✓
- Threshold slider updates immediately affect detection ✓
- Current level indicator shows real-time audio level ✓
- Reset requires confirmation and reboots to AP mode ✓

---

## Phase 7: Polish & Cross-Cutting Concerns

**Purpose**: Finalization, documentation, and verification

- [X] T057 [P] Add MQTT connection status indicator to existing /status page in main/network/http_server.cpp
- [X] T058 [P] Update status page JSON response with MQTT fields (enabled, connected, broker, last_state) in main/network/http_server.cpp
- [X] T059 [P] Add ESP_LOGI logging for MQTT connection events in main/network/mqtt_client.cpp
- [X] T060 [P] Add ESP_LOGW logging for MQTT errors and reconnection attempts in main/network/mqtt_client.cpp
- [X] T061 [P] Add ESP_LOGI logging for audio threshold changes in audio/audio_capture.cpp
- [ ] T062 [P] Verify memory budget: MQTT client heap usage <50KB via heap tracing in main.cpp
- [ ] T063 [P] Verify CPU overhead: Audio task <10% increase via FreeRTOS stats in main.cpp
- [ ] T064 Verify message rate: <1 msg/sec average during normal operation via MQTT broker logs
- [ ] T065 Run 7-day continuous operation test (SC-004 requirement)
- [X] T066 [P] Update README.md with MQTT setup instructions and troubleshooting
- [ ] T067 [P] Test factory reset erases all config and reboots to AP mode (10 trials for SC-009)

---

## Dependencies

### User Story Dependencies

```
Phase 1 (Setup) → Phase 2 (Foundational)
    ↓
    ├─→ Phase 3: US1 (Auto Discovery) [P1] ← MVP Scope
    ├─→ Phase 4: US2 (Playback Status) [P1] ← MVP Scope
    ├─→ Phase 5: US3 (Stream URL) [P2]
    └─→ Phase 6: US4 (Web Config) [P2]
    ↓
Phase 7 (Polish)
```

**MVP Delivery**: Complete Phases 1-4 (US1 + US2) for minimum viable product.

**Full Feature**: Complete all phases (US1-US4) for complete functionality.

### Story-Level Dependencies

- **US1 (Discovery)**: No dependencies (can implement first)
- **US2 (Playback Status)**: Depends on US1 (discovery must publish state_topic)
- **US3 (Stream URL)**: Depends on US1 (discovery must publish attributes_topic)
- **US4 (Web Config)**: Independent (web UI can be implemented in parallel)

**Parallel Execution**: US2, US3, US4 can be implemented in parallel after US1 complete.

---

## Parallel Execution Opportunities

### Phase 1 (Setup)
- T002 and T003 can run in parallel (different sections of config_schema.h)

### Phase 2 (Foundational)
- T006 (factory reset) can run in parallel with T004-T005 (different function)
- T007-T010 (audio detection) can run in parallel with T004-T006 (different file)

### Phase 3 (US1 - Discovery)
- T011 (header) must complete before T012-T020 (implementation)
- Most US1 tasks are sequential (dependencies within mqtt_client.cpp)

### Phase 4 (US2 - Playback Status)
- T021-T022 (MQTT publishing) can run in parallel (different functions)
- T026-T027 (threshold loading) can run in parallel with T021-T025

### Phase 5 (US3 - Stream URL)
- T028-T029 (attributes publishing) can run in parallel (different functions)

### Phase 6 (US4 - Web Config)
- T035-T038 (HTML/JS UI) can run in parallel (different UI elements)
- T048-T050 (test endpoint) can run in parallel with T039-T047 (save endpoint)
- T051-T054 (reset endpoint) can run in parallel with T048-T050
- T056 (audio level API) can run in parallel with other endpoints

### Phase 7 (Polish)
- All tasks in Phase 7 can run in parallel (different files/concerns)

**Maximum Parallelism**: 3-4 developers can work simultaneously on different user stories after Phase 2 complete.

---

## Testing Strategy

### Manual Integration Tests (No Automated Tests)

**Test Scenario 1: End-to-End Discovery**
1. Configure broker credentials via web UI
2. Save and verify device connects within 10 seconds
3. Check Home Assistant integrations page
4. Verify device appears within 30 seconds
5. Verify device metadata displayed correctly

**Test Scenario 2: Playback Detection Accuracy**
1. Inject 1kHz test tone at -60dB, gradually increase to -20dB
2. Observe Home Assistant binary sensor state changes
3. Verify threshold accuracy: idle below, playing above
4. Test debouncing: rapid on/off should not flood broker

**Test Scenario 3: Network Resilience**
1. Start device with MQTT connected and publishing
2. Stop MQTT broker service
3. Observe device reconnection attempts (check serial logs)
4. Restart broker
5. Verify device reconnects within 2 minutes
6. Verify discovery and state republished

**Test Scenario 4: IP Address Change**
1. Note current stream URL attribute in Home Assistant
2. Force DHCP lease renewal or change to static IP
3. Wait for device to obtain new IP
4. Verify stream URL attribute updates within 60 seconds
5. Verify stream still accessible at new URL

**Test Scenario 5: Factory Reset**
1. Navigate to /mqtt-settings page
2. Click "Reset All Configuration" button
3. Verify confirmation dialog appears
4. Confirm action
5. Verify device reboots within 15 seconds
6. Verify WiFi AP mode: ESP32-Audio-Stream-<MAC> appears
7. Verify all config erased (connect to AP, check empty config)

**Test Scenario 6: Long-Term Stability (SC-004)**
1. Configure device with valid broker credentials
2. Start audio playback
3. Monitor for 7 days continuously
4. Verify zero crashes related to MQTT features
5. Check heap usage remains stable (<50KB for MQTT)
6. Verify message rate <1/sec average

---

## Success Criteria Validation

All success criteria from [spec.md](./spec.md) must be validated:

- ✅ **SC-001**: Device appears in HA within 30 seconds → Manual test
- ✅ **SC-002**: Status updates <2s latency → Stopwatch + oscilloscope
- ✅ **SC-003**: Stream URL connection <3s → VLC connection timer
- ✅ **SC-004**: 7-day uptime, zero crashes → Long-term test
- ✅ **SC-005**: Reconnect within 2 minutes → Network disruption test
- ✅ **SC-006**: Zero message loss in 1-hour stable playback → MQTT broker logs
- ✅ **SC-007**: Setup without docs → Usability test with beta users
- ✅ **SC-008**: Threshold calibration <2 minutes → Timed user test
- ✅ **SC-009**: Factory reset within 15s, 100% success → 10 trial test
- ✅ **SC-010**: Confirmation prevents accidental reset → Usability test
- ✅ **SC-Performance**: <5% CPU, <50KB heap → FreeRTOS stats + heap tracing
- ✅ **SC-Resource**: <1 msg/sec publish rate → MQTT broker monitoring

---

## Implementation Notes

### Core Affinity
- **Core 1**: Audio capture, RMS calculation, threshold check (real-time, no changes to existing affinity)
- **Core 0**: MQTT client, HTTP server, WiFi manager (non-real-time)

### Memory Constraints
- MQTT client library: ~40-50KB heap (ESP-MQTT)
- Discovery message: ~450 bytes (JSON payload)
- NVS storage: ~256 bytes (MQTT config + threshold)
- Total increase: <60KB (<1% of 8MB PSRAM)

### Performance Targets
- RMS calculation overhead: <10% CPU increase on Core 1
- Audio detection latency: <2 seconds (200ms/500ms debounce)
- MQTT publish rate: <1 message/second sustained average
- Reconnection backoff: 5s, 10s, 20s, 40s, ..., max 300s (5 min)

### Error Handling
- Invalid broker credentials: Log error, do not retry until config changed
- Network unreachable: Retry with exponential backoff
- MQTT publish failure: ESP-MQTT handles QoS 1 retries automatically
- Audio threshold out of range: Clamp to [-60.0, -20.0] and proceed

---

## Total Task Count: 67 tasks

**By Phase**:
- Phase 1 (Setup): 3 tasks
- Phase 2 (Foundational): 7 tasks
- Phase 3 (US1 - Discovery): 10 tasks
- Phase 4 (US2 - Playback Status): 7 tasks
- Phase 5 (US3 - Stream URL): 7 tasks
- Phase 6 (US4 - Web Config): 22 tasks
- Phase 7 (Polish): 11 tasks

**By User Story**:
- US1 (Auto Discovery): 10 tasks
- US2 (Playback Status): 7 tasks
- US3 (Stream URL): 7 tasks
- US4 (Web Config): 22 tasks

**MVP Scope** (Phases 1-4): 27 tasks → Delivers auto-discovery + playback monitoring
**Full Feature** (All phases): 67 tasks → Complete MQTT integration with web config

**Parallel Tasks**: 38 tasks marked [P] can run in parallel with others

**Format Validation**: ✅ All tasks follow `- [ ] [TID] [P?] [Story?] Description with file path` format
