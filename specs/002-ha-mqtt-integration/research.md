# Research: Home Assistant MQTT Integration

**Feature**: 002-ha-mqtt-integration  
**Date**: 2026-02-24

## Technology Decisions

### 1. MQTT Client Library

**Decision**: Use ESP-MQTT (official ESP-IDF component `espressif/mqtt`)

**Rationale**:
- Native ESP-IDF integration with zero external dependencies
- Officially maintained by Espressif with active support
- Memory efficient (~30-40KB footprint) optimized for ESP32 constraints
- Built-in reconnection with configurable exponential backoff
- Full QoS support (0, 1, 2) and retained message handling
- TLS/SSL support for optional secure connections
- Extensively documented with production-ready examples

**Alternatives Considered**:
- **PubSubClient**: Arduino-focused library requiring compatibility layer; not suitable for pure ESP-IDF/C++ projects
- **Paho MQTT C**: Generic embedded MQTT client; requires more manual integration and lacks ESP-IDF optimizations

**Implementation Notes**:
- Add `espressif/mqtt: "^1.0.0"` to `main/idf_component.yml`
- Use `esp_mqtt_client_*` API for all MQTT operations
- Configure reconnect timeout starting at 5 seconds with automatic exponential backoff
- Typical heap usage during operation: 40-50KB

---

### 2. Home Assistant MQTT Discovery Protocol

**Decision**: Implement standard Home Assistant MQTT Discovery v2024+ specification

**Topic Structure**:
```
homeassistant/<component>/<node_id>/<object_id>/config
```

**Binary Sensor Configuration** (for playback status):
- Component: `binary_sensor`
- Node ID: `esp32_<mac_address>` (e.g., `esp32_aabbccddeeff`)
- Object ID: `playback_status`
- Example topic: `homeassistant/binary_sensor/esp32_aabbccddeeff/playback_status/config`

**Required Payload Fields**:
```json
{
  "name": "Turntable Playback",
  "unique_id": "esp32_aabbccddeeff_playback",
  "device_class": "sound",
  "state_topic": "turntable/esp32_aabbccddeeff/state",
  "payload_on": "playing",
  "payload_off": "idle",
  "json_attributes_topic": "turntable/esp32_aabbccddeeff/attributes",
  "availability_topic": "turntable/esp32_aabbccddeeff/availability",
  "device": {
    "identifiers": ["esp32_aabbccddeeff"],
    "name": "ESP32 Turntable Streamer",
    "model": "PCM1808 HTTP Streamer",
    "manufacturer": "Custom",
    "sw_version": "2.0.0"
  }
}
```

**Message Patterns**:
- **Discovery config**: Published once at startup with `retain=true`
- **State updates**: `"playing"` or `"idle"` published to state topic (QoS 1)
- **Attributes**: JSON object with `stream_url` field published to attributes topic
- **Availability**: `"online"` on connect, `"offline"` as Last Will and Testament (LWT)

**Rationale**:
- Standard protocol ensures compatibility with all Home Assistant versions
- Retained discovery messages allow Home Assistant to rediscover devices after restart
- Availability topic provides reliable online/offline status tracking
- Device grouping via identifiers enables logical organization in Home Assistant UI

---

### 3. Audio Detection Threshold Implementation

**Decision**: RMS (Root Mean Square) calculation with Exponential Moving Average (EMA)

**Algorithm**:
1. Calculate RMS of audio samples in each I2S buffer chunk
2. Apply exponential moving average to smooth/debounce transitions
3. Compare smoothed RMS against linear amplitude threshold
4. Update atomic boolean flag for MQTT publishing

**dB to Linear Conversion** (24-bit signed PCM):
```cpp
// For 24-bit: max amplitude = 2^23 = 8,388,608
// Linear amplitude = max_value × 10^(dB/20)
// Example: -40dB → 8,388,608 × 10^(-40/20) = ~838
```

**Implementation Pattern**:
```cpp
// Configuration
constexpr int32_t MAX_24BIT = 8388608;
constexpr float THRESHOLD_DB = -40.0f;  // User-configurable
constexpr int32_t THRESHOLD_LINEAR = (int32_t)(MAX_24BIT * pow(10.0f, THRESHOLD_DB / 20.0f));
constexpr float RMS_ALPHA = 0.05f;  // Smoothing factor (0.01-0.1)

// Per-buffer processing (in audio_capture task)
int64_t sum_squares = 0;
for (size_t i = 0; i < frames; i += 8) {  // Sample every 8th frame
    int32_t sample = extract_24bit_signed(buffer, i);
    sum_squares += (int64_t)sample * sample;
}
float chunk_rms = sqrtf((float)sum_squares / (frames / 8));

// Exponential moving average
rms_accumulator = (RMS_ALPHA * chunk_rms) + ((1.0f - RMS_ALPHA) * rms_accumulator);

// Threshold check
bool is_playing = (rms_accumulator > THRESHOLD_LINEAR);
```

**Rationale**:
- **RMS over peak detection**: Matches human perception of loudness; resistant to transient spikes/clicks
- **RMS over simple average**: Works correctly for signed AC signals (audio)
- **Exponential moving average**: Provides natural debouncing for rapid play/pause transitions
- **Frame sampling (every 8th)**: Reduces CPU overhead by ~87% with negligible accuracy loss

**Performance Characteristics**:
- CPU overhead: <10% increase on real-time audio path
- Detection latency: ~0.5-2 seconds depending on alpha value (meets <2s requirement)
- Memory impact: Single float accumulator (~4 bytes)

**Alternatives Considered**:
- **Peak detection**: Too sensitive to transients; false positives on vinyl pops/clicks
- **Zero-crossing rate**: Poor indicator for music/complex signals
- **FFT-based energy**: Excessive CPU overhead for simple presence detection

**Configurable Parameters**:
- **Threshold range**: -60dB to -20dB (stored in NVS, adjustable via web UI)
- **Default threshold**: -40dB (suitable for typical turntable output after preamp)
- **Alpha value**: Fixed at 0.05 for balance between responsiveness and stability

---

### 4. Stream URL Publication

**Decision**: Reuse existing HTTP stream endpoint from Feature 001, publish current IP:port via MQTT attributes

**Implementation**:
- Stream URL format: `http://<device_ip>:8080/stream` (existing endpoint)
- Publish in JSON attributes topic: `{"stream_url": "http://192.168.1.100:8080/stream"}`
- Update attributes whenever IP address changes (WiFi reconnect event)
- No mDNS URL (maintain IP-based for maximum client compatibility)

**Rationale**:
- Feature 001 already implements HTTP server on port 8080 with `/stream` endpoint
- No code duplication or refactoring needed
- IP-based URLs work with all HTTP clients (mDNS requires client support)
- Automatic updates via WiFi event handlers ensure URL stays current

**Alternatives Considered**:
- **mDNS URLs** (e.g., `http://turntable.local:8080/stream`): Requires mDNS client support; not universally compatible
- **Static configuration**: Fails when DHCP assigns new IP address
- **Separate streaming protocol**: Unnecessary complexity; HTTP streaming already proven

---

### 5. Web Interface Extension for MQTT Configuration

**Decision**: Add new `/mqtt-settings` page to existing HTTP server, reuse config portal styling

**Implementation**:
- New HTTP handler in `network/http_server.cpp`: `GET /mqtt-settings`, `POST /mqtt-settings`
- Reuse existing HTML templating pattern from `config_portal.cpp`
- Store credentials in NVS using existing `storage/nvs_config.cpp` infrastructure
- Add navigation link from existing `/status` page

**Rationale**:
- Feature 001 establishes HTTP server pattern and NVS storage pattern
- Consistent UI/UX with existing configuration portal
- No additional web framework dependencies
- Users already familiar with web-based configuration workflow

**Alternatives Considered**:
- **Extend config portal AP mode**: Would require MQTT config before WiFi connect; cart-before-horse ordering
- **Separate MQTT config app**: Code duplication and inconsistent UX
- **Serial console configuration**: Not user-friendly; violates usability requirements

---

### 6. Infrastructure Reuse from Feature 001

**Decision**: Direct reuse of web server, NVS storage, WiFi manager, and audio capture components

**Reused Components**:
- ✅ `network/http_server.cpp`: Add MQTT settings endpoint
- ✅ `storage/nvs_config.cpp`: Extend DeviceConfig struct with MQTT fields
- ✅ `network/wifi_manager.cpp`: Use existing WiFi events for IP change detection
- ✅ `audio/audio_capture.cpp`: Add RMS calculation inline in capture task
- ✅ `config_schema.h`: Add MQTT NVS key definitions

**New Components**:
- ➕ `network/mqtt_client.cpp`: New MQTT client service
- ➕ `audio/audio_detector.cpp`: Optional separate module for threshold logic (or inline in audio_capture)

**Rationale**:
- Feature 001 infrastructure is production-ready and proven
- No refactoring needed; clean extension points already exist
- Maintains single responsibility: each module extends its domain (network, storage, audio)
- Minimal code churn reduces regression risk

**Alternatives Considered**:
- **Refactor with abstraction layers**: Premature; only 2 features don't justify abstraction overhead
- **Duplicate infrastructure**: Would violate DRY principle and increase maintenance burden
- **Wrapper pattern**: Adds complexity without tangible benefit at this scale

---

## Integration Strategy

### System Architecture

```
┌─────────────────────────────────────────────────┐
│  Audio Capture Task (Core 1)                    │
│  ├─ I2S DMA receive                             │
│  ├─ 24-bit → WAV conversion                     │
│  ├─ RMS calculation + threshold check (NEW)     │
│  └─ Update atomic<bool> playback_status (NEW)   │
└─────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────┐
│  MQTT Task (Core 0 - NEW)                       │
│  ├─ Connect to broker (esp_mqtt_client)         │
│  ├─ Publish discovery config (retained)         │
│  ├─ Monitor playback_status atomic flag         │
│  ├─ Publish state updates (debounced)           │
│  ├─ Publish attributes (stream URL)             │
│  ├─ Handle reconnection with exponential backoff│
│  └─ LWT: availability = "offline"               │
└─────────────────────────────────────────────────┘
                    ↓
┌─────────────────────────────────────────────────┐
│  HTTP Server (Core 0 - existing)                │
│  ├─ /status (existing)                          │
│  ├─ /stream (existing)                          │
│  └─ /mqtt-settings (NEW)                        │
│     ├─ GET: Render config form                  │
│     ├─ POST: Save broker credentials to NVS     │
│     ├─ POST: Test connection                    │
│     └─ POST: Reset all config + reboot to AP    │
└─────────────────────────────────────────────────┘
```

**Core Affinity**:
- **Core 1**: Real-time audio capture (existing) + RMS detection (new, minimal overhead)
- **Core 0**: MQTT publishing, HTTP server, WiFi (no impact on audio determinism)

**Memory Budget**:
- MQTT client: ~40-50KB heap
- RMS state: ~4 bytes static
- Configuration storage: ~256 bytes NVS (broker credentials + threshold)
- **Total increase**: <60KB (well within ESP32-WROVER 8MB PSRAM budget)

---

## Configuration Schema Extension

**New NVS Keys** (add to `config_schema.h`):

```cpp
// MQTT Configuration
constexpr char NVS_MQTT_ENABLED[] = "mqtt_enabled";       // bool
constexpr char NVS_MQTT_BROKER[] = "mqtt_broker";         // string (max 128 chars)
constexpr char NVS_MQTT_PORT[] = "mqtt_port";             // uint16_t (default: 1883)
constexpr char NVS_MQTT_USERNAME[] = "mqtt_user";         // string (max 64 chars)
constexpr char NVS_MQTT_PASSWORD[] = "mqtt_pass";         // string (max 64 chars)
constexpr char NVS_MQTT_TLS[] = "mqtt_tls";               // bool (optional, default: false)

// Audio Detection
constexpr char NVS_AUDIO_THRESHOLD_DB[] = "audio_thresh"; // float (default: -40.0)
```

**DeviceConfig Struct Extension**:
```cpp
struct MQTTConfig {
    bool enabled;
    char broker[128];
    uint16_t port;
    char username[64];
    char password[64];
    bool use_tls;
};

struct DeviceConfig {
    WiFiCredentials wifi;     // Existing
    AudioSettings audio;       // Existing
    MQTTConfig mqtt;           // NEW
    float audio_threshold_db;  // NEW
};
```

---

## Testing Strategy

### Manual Testing (no automated tests requested in spec)

**Unit Test Scenarios** (manual verification):
1. MQTT connection with valid/invalid credentials
2. Auto-discovery message formatting and retention
3. Playback status detection with test tones (-60dB, -40dB, -20dB)
4. Threshold adjustment and immediate effect verification
5. Reconnection after broker restart (exponential backoff timing)
6. Factory reset: NVS erase + AP mode reboot

**Integration Test Scenarios**:
1. End-to-end: Device boot → discovery → state updates → Home Assistant dashboard
2. Network resilience: WiFi disconnect/reconnect during playback
3. Concurrent operation: HTTP streaming + MQTT publishing (no audio artifacts)
4. Memory stability: 7-day continuous operation (SC-004)

### Success Criteria Validation

Reference spec.md Success Criteria (SC-001 through SC-010):
- SC-001: Discovery within 30 seconds → Test with stopwatch
- SC-002: Status latency <2 seconds → Measure with oscilloscope/audio sync
- SC-Performance: <5% CPU, <50KB heap → FreeRTOS task stats
- SC-Resource: <1 msg/sec publish rate → MQTT broker logs

---

## Open Questions for Phase 1 Design

1. **MQTT Topic Prefix**: Should the device name be user-configurable or auto-generated from MAC?
   - Proposal: Auto-generate from MAC, allow override in advanced settings
2. **TLS Certificate Storage**: If TLS enabled, where to store broker CA certificate?
   - Proposal: Store in NVS partition (requires partition size validation)
3. **Reconnection Backoff Maximum**: Spec says "max 5 minutes" - should this be configurable?
   - Proposal: Fixed at 5 minutes (matches Home Assistant recommendations)
4. **Multiple Threshold Modes**: Should we support both dBFS (full scale) and dBu (voltage)?
   - Proposal: dBFS only (simpler, sufficient for digital threshold)
5. **Clipping Indicator in MQTT**: Should audio clipping status be published as separate sensor?
   - Proposal: Defer to future feature; include in attributes JSON for now

---

## Dependencies and Risks

### External Dependencies
- **ESP-IDF Version**: Requires ESP-IDF v5.0+ (project uses v5.x, compatible)
- **Home Assistant Version**: Discovery protocol compatible with HA 2020.12+ (widely supported)
- **Network Requirements**: MQTT broker accessible on local network (user responsibility)

### Risk Mitigation
- **Risk**: MQTT publishing blocks audio path
  - **Mitigation**: MQTT runs on Core 0, audio on Core 1; use non-blocking publish with queue
- **Risk**: Memory exhaustion with multiple connections
  - **Mitigation**: Limit MQTT queue depth, implement heap monitoring with alerts
- **Risk**: Threshold calibration difficulty for users
  - **Mitigation**: Real-time visual feedback (dB meter) in web UI during adjustment
- **Risk**: Factory reset accidental activation
  - **Mitigation**: Explicit confirmation dialog (FR-013a requirement)

### Constitution Compliance
- ✅ **Real-Time Performance**: MQTT on Core 0, no audio path impact
- ✅ **Audio Quality**: RMS calculation uses subsampling, <10% overhead
- ✅ **Resource Efficiency**: 60KB total increase within ESP32-WROVER budget
- ✅ **Deterministic Timing**: No locks in audio path; atomic flag for status
- ✅ **Fail-Safe**: Device continues streaming if MQTT unavailable (FR-010)

---

## Next Steps (Phase 1)

1. Generate `data-model.md`: Define MQTT message schemas, state machine, entity relationships
2. Generate `contracts/`: Document MQTT topics, payload formats, HTTP endpoints
3. Generate `quickstart.md`: Step-by-step setup and testing procedures
4. Update Constitution Check: Validate no real-time or resource violations
5. Proceed to Phase 2: Task decomposition for implementation
