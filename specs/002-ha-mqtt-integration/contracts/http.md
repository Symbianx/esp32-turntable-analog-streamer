# HTTP API Contracts

**Feature**: 002-ha-mqtt-integration  
**Date**: 2026-02-24

## Overview

This document specifies the HTTP endpoints for MQTT configuration management, extending the existing HTTP server from Feature 001.

---

## Endpoints

### GET /mqtt-settings

Display MQTT configuration form.

**Method**: `GET`

**Authentication**: None (local network access only)

**Request**: No parameters

**Response**:
- **Status**: `200 OK`
- **Content-Type**: `text/html; charset=utf-8`
- **Body**: HTML form with current MQTT configuration

**HTML Form Fields**:
- `enabled`: checkbox - Enable MQTT integration
- `broker`: text input - MQTT broker hostname/IP (max 127 chars)
- `port`: number input - MQTT broker port (1-65535, default: 1883)
- `username`: text input - MQTT username (max 63 chars, optional)
- `password`: password input - MQTT password (max 63 chars, optional)
- `use_tls`: checkbox - Enable TLS/SSL
- `threshold_db`: range slider - Audio detection threshold (-60 to -20 dB, default: -40)
- `current_level`: display - Real-time audio level indicator (read-only)

**Actions**:
- Test Connection button (POST to `/mqtt-settings/test`)
- Save button (POST to `/mqtt-settings`)
- Reset All Configuration button (POST to `/mqtt-settings/reset` with confirmation)

**Example Response**:
```html
<!DOCTYPE html>
<html>
<head><title>MQTT Settings</title></head>
<body>
  <h1>MQTT Configuration</h1>
  <form method="POST" action="/mqtt-settings">
    <label><input type="checkbox" name="enabled" checked> Enable MQTT</label><br>
    <label>Broker: <input type="text" name="broker" value="192.168.1.100" maxlength="127"></label><br>
    <label>Port: <input type="number" name="port" value="1883" min="1" max="65535"></label><br>
    <label>Username: <input type="text" name="username" maxlength="63"></label><br>
    <label>Password: <input type="password" name="password" maxlength="63"></label><br>
    <label><input type="checkbox" name="use_tls"> Use TLS/SSL</label><br>
    <label>Audio Threshold: <input type="range" name="threshold_db" min="-60" max="-20" value="-40"> <span id="threshold_value">-40 dB</span></label><br>
    <div>Current Level: <span id="current_level">-50 dB</span></div>
    <button type="submit">Save</button>
    <button type="button" onclick="testConnection()">Test Connection</button>
    <button type="button" onclick="resetAllConfig()">Reset All Configuration</button>
  </form>
  <script>
    // JavaScript for real-time threshold feedback and test connection
  </script>
</body>
</html>
```

---

### POST /mqtt-settings

Save MQTT configuration to NVS.

**Method**: `POST`

**Authentication**: None

**Content-Type**: `application/x-www-form-urlencoded`

**Request Parameters**:
- `enabled`: `"on"` or absent (checkbox)
- `broker`: string (max 127 chars)
- `port`: integer (1-65535)
- `username`: string (max 63 chars, optional)
- `password`: string (max 63 chars, optional)
- `use_tls`: `"on"` or absent (checkbox)
- `threshold_db`: float (-60.0 to -20.0)

**Validation**:
- If `enabled=on` and `broker` is empty → `400 Bad Request`
- If `port` out of range → `400 Bad Request`
- If only one of `username`/`password` provided → `400 Bad Request`
- If `threshold_db` out of range → clamp to [-60.0, -20.0], proceed

**Success Response**:
- **Status**: `200 OK` or `303 See Other` (redirect to `/mqtt-settings`)
- **Content-Type**: `text/html`
- **Body**: Success message + current connection status

**Error Response**:
- **Status**: `400 Bad Request`
- **Content-Type**: `text/html`
- **Body**: Error message describing validation failure

**Side Effects**:
1. Save configuration to NVS
2. If MQTT was enabled and broker changed: Disconnect existing connection
3. If MQTT enabled: Initiate new connection
4. If threshold changed: Update audio detection module immediately

**Example Success**:
```
HTTP/1.1 200 OK
Content-Type: text/html

<html><body>
  <p>Settings saved successfully!</p>
  <p>MQTT Status: Connected</p>
  <a href="/mqtt-settings">Back to settings</a>
</body></html>
```

---

### POST /mqtt-settings/test

Test MQTT broker connection without saving.

**Method**: `POST`

**Authentication**: None

**Content-Type**: `application/x-www-form-urlencoded`

**Request Parameters**: Same as `/mqtt-settings` POST

**Behavior**:
- Validate parameters (same rules as save)
- Attempt connection to broker with provided credentials
- Wait up to 5 seconds for CONNACK
- Return success or error message
- **Do not save** to NVS
- **Do not affect** current MQTT connection

**Success Response**:
- **Status**: `200 OK`
- **Content-Type**: `application/json`
- **Body**: `{"status": "success", "message": "Connected to broker successfully"}`

**Failure Response**:
- **Status**: `200 OK` (not 4xx/5xx, to avoid browser error handling)
- **Content-Type**: `application/json`
- **Body**: `{"status": "error", "message": "<error_description>"}`

**Error Messages**:
- `"Connection timeout"` - No response within 5 seconds
- `"Authentication failed"` - Invalid username/password
- `"Connection refused"` - Broker rejected connection
- `"Network unreachable"` - Cannot reach broker IP
- `"Invalid broker address"` - DNS failure or malformed hostname

**Example**:
```json
{
  "status": "success",
  "message": "Connected to broker successfully"
}
```

---

### POST /mqtt-settings/reset

Factory reset: erase all configuration and reboot to AP mode.

**Method**: `POST`

**Authentication**: None

**Content-Type**: `application/x-www-form-urlencoded`

**Request Parameters**:
- `confirm`: `"yes"` (required, prevents accidental reset)

**Validation**:
- If `confirm != "yes"` → `400 Bad Request` with error message

**Success Response**:
- **Status**: `200 OK`
- **Content-Type**: `text/html`
- **Body**: "Resetting device..." message

**Behavior**:
1. Validate `confirm` parameter
2. Erase entire NVS `"config"` namespace (WiFi, MQTT, audio settings)
3. Schedule reboot in 2 seconds
4. Return response to client
5. Reboot device → enters AP mode (first-time setup)

**Side Effects**:
- All configuration erased (WiFi credentials, MQTT broker, audio threshold)
- Device disconnects from network and MQTT broker
- HTTP clients disconnected
- Device reboots and creates WiFi AP "ESP32-Audio-Stream-<MAC>"

**Example**:
```html
HTTP/1.1 200 OK
Content-Type: text/html

<html><body>
  <p>All configuration erased. Device rebooting to setup mode...</p>
  <p>Connect to WiFi network: ESP32-Audio-Stream-aabbccddeeff</p>
</body></html>
```

---

## Error Handling

### 400 Bad Request
- Invalid parameter values
- Missing required fields when `enabled=on`
- Malformed form data

### 500 Internal Server Error
- NVS write failure
- Memory allocation failure
- Unexpected exception

---

## CORS

**Policy**: No CORS headers (local network access only)

**Rationale**: Device not exposed to internet; no cross-origin requests expected

---

## Security Considerations

**No Authentication**: HTTP endpoints accessible to anyone on local network

**Rationale**: 
- Home network assumed trusted
- Adding auth complicates first-time setup
- Future feature: Optional HTTP Basic Auth

**Mitigation**:
- Device should not be exposed to internet (router firewall)
- Factory reset requires explicit confirmation

---

## Integration with Existing Endpoints

**Existing** (from Feature 001):
- `GET /` - Status page (add MQTT status section)
- `GET /status` - JSON status (add MQTT fields)
- `GET /stream` - Audio stream (no changes)
- `GET /config` - Configuration portal (add MQTT link)

**New** (this feature):
- `GET /mqtt-settings`
- `POST /mqtt-settings`
- `POST /mqtt-settings/test`
- `POST /mqtt-settings/reset`

**Navigation**: Add "MQTT Settings" link to existing status page (`/`)

---

## Testing Checklist

- [ ] GET `/mqtt-settings` renders form with current config
- [ ] POST `/mqtt-settings` saves valid config to NVS
- [ ] POST `/mqtt-settings` rejects invalid broker/port
- [ ] POST `/mqtt-settings/test` succeeds with valid credentials
- [ ] POST `/mqtt-settings/test` fails gracefully with invalid credentials
- [ ] POST `/mqtt-settings/reset` requires `confirm=yes`
- [ ] Factory reset erases all config and reboots to AP mode
- [ ] Threshold slider updates immediately affect audio detection
- [ ] Real-time audio level indicator updates every second
