# MQTT Protocol Contracts

**Feature**: 002-ha-mqtt-integration  
**Date**: 2026-02-24

## Overview

This document specifies all MQTT topics, message formats, and protocol behaviors for the Home Assistant MQTT integration.

---

## Topic Hierarchy

**Prefix**: `turntable/<device_unique_id>/`  
**Discovery Prefix**: `homeassistant/binary_sensor/<device_unique_id>/`

where `<device_unique_id>` = `esp32_<mac_address>` (e.g., `esp32_aabbccddeeff`)

---

## MQTT Topics

### 1. Discovery Configuration

**Topic**: `homeassistant/binary_sensor/<device_unique_id>/playback_status/config`

**Direction**: Device → Broker (publish once on connect)

**QoS**: 1 (at least once)

**Retained**: `true`

**Payload** (JSON):
```json
{
  "name": "Turntable Playback",
  "unique_id": "<device_unique_id>_playback",
  "device_class": "sound",
  "state_topic": "turntable/<device_unique_id>/state",
  "payload_on": "playing",
  "payload_off": "idle",
  "json_attributes_topic": "turntable/<device_unique_id>/attributes",
  "availability_topic": "turntable/<device_unique_id>/availability",
  "device": {
    "identifiers": ["<device_unique_id>"],
    "name": "ESP32 Turntable Streamer",
    "model": "PCM1808 HTTP Streamer",
    "manufacturer": "Custom",
    "sw_version": "2.0.0"
  }
}
```

**Size**: ~450 bytes

**Publish Frequency**: Once on MQTT connect, republish on reconnect

---

### 2. Playback State

**Topic**: `turntable/<device_unique_id>/state`

**Direction**: Device → Broker (publish on state change)

**QoS**: 1 (at least once)

**Retained**: `false`

**Payload** (plain text):
- `"playing"` - Audio signal detected above threshold
- `"idle"` - Audio signal below threshold

**Publish Frequency**: On state change only (debounced), typically <1 msg/sec

**Example**:
```
Topic: turntable/esp32_aabbccddeeff/state
Payload: playing
```

---

### 3. Device Attributes

**Topic**: `turntable/<device_unique_id>/attributes`

**Direction**: Device → Broker (publish on IP change or connect)

**QoS**: 1 (at least once)

**Retained**: `false`

**Payload** (JSON):
```json
{
  "stream_url": "http://192.168.1.100:8080/stream"
}
```

**Size**: ~60 bytes

**Publish Frequency**: On WiFi connect, IP address change, or manual trigger

---

### 4. Availability

**Topic**: `turntable/<device_unique_id>/availability`

**Direction**: Device → Broker (publish on connect, LWT on disconnect)

**QoS**: 1 (at least once)

**Retained**: `true`

**Payload** (plain text):
- `"online"` - Device connected and operational
- `"offline"` - Device disconnected (Last Will and Testament)

**Publish Frequency**: 
- `"online"` published immediately after MQTT connect
- `"offline"` published by broker when device disconnects (LWT)

**Example**:
```
Topic: turntable/esp32_aabbccddeeff/availability
Payload: online
```

---

## Connection Behavior

### Initial Connection Sequence

1. **CONNECT** to broker with credentials from NVS
   - Client ID: `esp32_<mac_address>`
   - Clean Session: `false` (persistent session)
   - Keep Alive: 60 seconds
   - LWT Topic: `turntable/<device_unique_id>/availability`
   - LWT Payload: `"offline"`
   - LWT QoS: 1
   - LWT Retained: `true`

2. **PUBLISH** discovery config (retained, QoS 1)
   - Topic: `homeassistant/binary_sensor/<device_unique_id>/playback_status/config`

3. **PUBLISH** availability `"online"` (retained, QoS 1)
   - Topic: `turntable/<device_unique_id>/availability`

4. **PUBLISH** current state (QoS 1)
   - Topic: `turntable/<device_unique_id>/state`
   - Payload: `"idle"` or `"playing"` (current state)

5. **PUBLISH** device attributes (QoS 1)
   - Topic: `turntable/<device_unique_id>/attributes`
   - Payload: Stream URL JSON

6. **Begin monitoring** playback status for changes

---

### Reconnection Strategy

**Trigger**: Connection lost, network error, or broker unavailable

**Behavior**:
- Exponential backoff: 5s, 10s, 20s, 40s, 80s, ..., max 300s (5 minutes)
- Retry indefinitely until connection restored
- On successful reconnect, repeat Initial Connection Sequence

**Buffering**: No state buffering during disconnect; publish current state on reconnect

---

## Message Rate Limits

**Maximum Publish Rate**: 1 message per second (sustained average)

**Burst Allowance**: 
- Discovery + availability + state + attributes on connect: 4 messages in <1 second (acceptable)
- State changes: Debounced to prevent rapid toggling

**Rationale**: Prevents broker overload; meets Home Assistant recommended practices

---

## Error Handling

### Connection Failures

- **Invalid credentials**: Log error, do not retry until config changed
- **Network unreachable**: Retry with exponential backoff
- **Broker timeout**: Retry with exponential backoff
- **DNS failure**: Retry with exponential backoff

### Publish Failures

- **QoS 1 timeout**: ESP-MQTT library handles retries automatically
- **Queue full**: Drop oldest non-discovery message, log warning
- **Disconnected**: Queue for next connection (max 10 messages)

---

## Data Types

### device_unique_id
- **Format**: `esp32_[0-9a-f]{12}` (lowercase hex MAC address)
- **Example**: `esp32_aabbccddeeff`
- **Derivation**: `sprintf(buf, "esp32_%02x%02x%02x%02x%02x%02x", mac[0], mac[1], ...)`

### Playback State
- **Type**: String enum
- **Values**: `"playing"` | `"idle"`
- **Encoding**: UTF-8 plain text

### Availability Status
- **Type**: String enum
- **Values**: `"online"` | `"offline"`
- **Encoding**: UTF-8 plain text

### Stream URL
- **Format**: `http://<ipv4_address>:<port>/stream`
- **Example**: `http://192.168.1.100:8080/stream`
- **Max Length**: 127 characters

---

## Compliance

**Home Assistant MQTT Discovery**: v2024.1+ compatible

**MQTT Protocol Version**: 3.1.1

**Character Encoding**: UTF-8

**Topic Naming**: Lowercase, alphanumeric + underscore, no wildcards

---

## Testing Checklist

- [ ] Discovery message published on connect with correct format
- [ ] State updates published within 2 seconds of actual change
- [ ] Attributes updated when IP address changes
- [ ] Availability `"online"` published on connect
- [ ] LWT `"offline"` published by broker on disconnect
- [ ] Reconnection with exponential backoff observed
- [ ] Message rate <1/sec sustained during normal operation
- [ ] All topics use correct device_unique_id derived from MAC
