# Data Model: Home Assistant MQTT Integration

**Feature**: 002-ha-mqtt-integration  
**Date**: 2026-02-24

## Overview

This document defines the data entities, state machines, and relationships for the Home Assistant MQTT integration feature. The data model extends the existing configuration schema from Feature 001 and introduces MQTT-specific entities for device discovery, playback status, and stream URL publication.

---

## Entities

### 1. MQTTConfig

Broker connection configuration stored in NVS.

**Fields**:
- `enabled`: `bool` - Whether MQTT integration is enabled (default: `false`)
- `broker`: `char[128]` - MQTT broker hostname or IP address
- `port`: `uint16_t` - MQTT broker port (default: `1883`, TLS: `8883`)
- `username`: `char[64]` - MQTT authentication username (optional)
- `password`: `char[64]` - MQTT authentication password (optional)
- `use_tls`: `bool` - Enable TLS/SSL encryption (default: `false`)

**Validation Rules**:
- `broker`: Must be non-empty if `enabled` is `true`; max 127 characters
- `port`: Valid range 1-65535
- `username`/`password`: Max 63 characters; both empty or both filled

**Storage**: NVS namespace `"config"` with keys: `mqtt_enabled`, `mqtt_broker`, `mqtt_port`, `mqtt_user`, `mqtt_pass`, `mqtt_tls`

---

### 2. AudioThreshold

Configurable threshold for audio playback detection.

**Fields**:
- `threshold_db`: `float` - Threshold in dBFS, range: -60.0 to -20.0 (default: -40.0)

**Validation**: Clamped to [-60.0, -20.0] on load

**Storage**: NVS key `audio_thresh`

---

### 3. PlaybackStatus

Real-time audio playback state (volatile).

**Fields**:
- `is_playing`: `std::atomic<bool>` - Current state: `true` = playing, `false` = idle

**State Transitions**: Idle ↔ Playing based on RMS threshold with debouncing (200ms/500ms)

---

### 4. DeviceIdentity

Immutable device metadata for Home Assistant discovery.

**Fields**:
- `unique_id`: `char[32]` - Format: `"esp32_<mac_address>"`
- `name`: `char[64]` - `"ESP32 Turntable Streamer"`
- `model`: `char[32]` - `"PCM1808 HTTP Streamer"`
- `manufacturer`: `char[32]` - `"Custom"`
- `sw_version`: `char[16]` - Firmware version

---

### 5. MQTTDiscoveryMessage

Home Assistant MQTT discovery payload (JSON, ~450 bytes).

**Topic**: `homeassistant/binary_sensor/<device_unique_id>/playback_status/config`  
**QoS**: 1, **Retained**: true

---

## Memory Budget

| Entity | Storage | Location |
|--------|---------|----------|
| MQTTConfig | ~300 bytes | NVS |
| AudioThreshold | 4 bytes | NVS |
| MQTT Client | ~40-50KB | Heap |
| **Total Increase** | **~51KB** | **<1% of 8MB PSRAM** |
