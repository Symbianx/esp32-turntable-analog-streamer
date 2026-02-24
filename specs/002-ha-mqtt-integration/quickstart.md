# Quickstart Guide: Home Assistant MQTT Integration

**Feature**: 002-ha-mqtt-integration  
**Date**: 2026-02-24

## Prerequisites

- ESP32 Turntable Streamer running Feature 001
- Device connected to WiFi network
- Home Assistant with MQTT broker
- Broker accessible from ESP32 device

## Setup Steps

### 1. Configure MQTT Broker

1. Navigate to `http://<device-ip>:8080/mqtt-settings`
2. Fill in configuration:
   - ☑ Enable MQTT Integration
   - Broker: `192.168.1.100` (your broker IP)
   - Port: `1883` (default)
   - Username/Password (if required)
3. Click "Test Connection" → verify success
4. Click "Save"

### 2. Adjust Audio Threshold

1. On MQTT Settings page, adjust "Audio Threshold" slider
2. Default: -40 dB (suitable for most turntables)
3. Observe "Current Level" indicator while playing audio
4. Set threshold just below normal playback level
5. Click "Save"

### 3. Verify Home Assistant Discovery

1. Open Home Assistant → Settings → Devices & Services → MQTT
2. Device "ESP32 Turntable Streamer" should appear within 30 seconds
3. Binary sensor "Turntable Playback" shows current state

### 4. Access Stream URL

1. Click device in Home Assistant
2. View "stream_url" attribute
3. Open URL in media player to verify stream

## Testing

- **Playback Detection**: Play audio, verify status changes to "Playing" within 2s
- **Network Recovery**: Disconnect WiFi, reconnect, verify device auto-reconnects
- **Factory Reset**: Click "Reset All Configuration", confirm, verify device reboots to AP mode

## Automation Example

```yaml
automation:
  - alias: "Amplifier Auto-On"
    trigger:
      - platform: state
        entity_id: binary_sensor.turntable_playback
        to: "playing"
    action:
      - service: switch.turn_on
        target:
          entity_id: switch.amplifier_power
```

## Troubleshooting

- **Not discovered**: Check broker credentials, verify MQTT integration in HA
- **Status not updating**: Adjust threshold, verify MQTT connected
- **Old stream URL**: Restart device to force IP redetection
