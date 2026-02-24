# Feature Specification: Home Assistant MQTT Integration

**Feature Branch**: `002-ha-mqtt-integration`  
**Created**: 2026-02-24  
**Status**: Draft  
**Input**: User description: "Connect the current device to home assistant via MQTT. The device should be discoverable via MQTT auto discovery. It should send whether it has detected something is playing (or not) and it should also expose its stream url via mqtt."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Automatic Device Discovery (Priority: P1)

A home automation user adds the audio streaming device to their network and wants it to automatically appear in Home Assistant without manual configuration. The device announces itself and its capabilities, allowing the user to immediately see it in their Home Assistant dashboard and start monitoring audio activity.

**Why this priority**: Core integration functionality - without auto-discovery, users must manually configure sensors and entities, significantly increasing setup complexity and reducing adoption.

**Independent Test**: Can be fully tested by powering on the device with valid network and broker credentials, then checking Home Assistant for the new device entity. Delivers immediate value by eliminating manual configuration steps.

**Acceptance Scenarios**:

1. **Given** the device is connected to the network and broker credentials are configured, **When** the device powers on, **Then** Home Assistant automatically discovers the device within 30 seconds
2. **Given** the device has been discovered, **When** a user views the Home Assistant integrations page, **Then** the device appears with its name, model, and available sensors
3. **Given** the device is already discovered, **When** the device reconnects after power cycle, **Then** Home Assistant retains the device configuration and updates its availability status

---

### User Story 2 - Audio Playback Status Monitoring (Priority: P1)

A user wants to know when audio is actively being played through the turntable system so they can trigger automations (e.g., adjust lighting, notify others, log listening sessions). The system detects when audio signal is present and reports this status in real-time.

**Why this priority**: Primary business value of the integration - enables users to create meaningful automations based on actual listening activity rather than just device power state.

**Independent Test**: Can be fully tested by playing audio through the turntable and verifying the playback status sensor updates in Home Assistant. Delivers standalone value for monitoring and automation triggers.

**Acceptance Scenarios**:

1. **Given** the device is idle with no audio input, **When** audio playback begins on the turntable, **Then** the playback status sensor updates to "playing" within 2 seconds
2. **Given** audio is currently playing, **When** playback stops (signal drops below threshold), **Then** the playback status sensor updates to "idle" within 2 seconds
3. **Given** the playback status is being monitored, **When** a user checks historical data, **Then** they can see accurate timestamps of when playback started and stopped

---

### User Story 3 - Stream URL Access (Priority: P2)

A user wants to access the audio stream from other devices on their network (e.g., stream to smart speakers, record to NAS, or monitor from mobile app). The device publishes its stream URL through Home Assistant, making it discoverable and accessible to other services.

**Why this priority**: Extends utility beyond simple monitoring - enables advanced use cases like multi-room audio, recording, and remote listening. Lower priority than basic monitoring since it requires additional client software.

**Independent Test**: Can be fully tested by retrieving the stream URL from Home Assistant and successfully connecting to it with a standard audio player. Delivers independent value for audio streaming scenarios.

**Acceptance Scenarios**:

1. **Given** the device is streaming audio, **When** a user reads the stream URL attribute from Home Assistant, **Then** they receive a valid URL in the format "http://[device-ip]:[port]/stream"
2. **Given** a valid stream URL is available, **When** a user connects to the URL with an audio client, **Then** they receive the audio stream with less than 3 seconds of latency
3. **Given** the device IP address changes, **When** the device reconnects to the network, **Then** the stream URL attribute updates automatically within 60 seconds

---

### User Story 4 - MQTT Configuration via Web Interface (Priority: P2)

A user navigates to the existing device web interface and accesses a new "MQTT Settings" page. They enter their Home Assistant broker details (host, port, username, password, optional TLS settings), adjust the audio detection threshold using a slider with real-time feedback, test the connection, and save the configuration. The device immediately connects to the broker and begins publishing discovery messages and status updates using the configured threshold. If needed, the user can reset all configuration (MQTT and WiFi) using a clearly labeled button with confirmation dialog, useful for troubleshooting or transferring the device to a new network/broker.

**Why this priority**: Essential for enabling the integration, but slightly lower priority than auto-discovery itself since users can configure once and forget. Leverages existing web UI infrastructure for consistency. The threshold control allows users to fine-tune detection for their specific setup (varying preamp gains, turntable output levels, and noise floors). Factory reset capability is critical for device handoff and troubleshooting scenarios.

**Independent Test**: Can be fully tested by navigating to the MQTT settings page, entering valid broker credentials, adjusting the threshold while observing current audio level indicator, clicking "Test Connection" to verify, then clicking "Save". Device should connect to broker and show connection status as "Connected" within 10 seconds. Reset functionality can be tested by clicking "Reset All Configuration", confirming the action, and verifying the device reboots into AP mode.

**Acceptance Scenarios**:

1. **Given** the user is on the device status page, **When** they click the "MQTT Settings" navigation link, **Then** they are taken to the MQTT configuration page within 1 second
2. **Given** the user enters broker credentials, **When** they click "Test Connection", **Then** the device attempts connection and displays success or error message within 5 seconds
3. **Given** the user has entered valid credentials, **When** they click "Save", **Then** the settings are persisted and the device connects to the broker within 10 seconds
4. **Given** MQTT is configured and connected, **When** the device reboots, **Then** it automatically reconnects to the broker using saved credentials
5. **Given** the user adjusts the audio detection threshold slider, **When** they save the settings, **Then** the new threshold takes effect immediately and playback detection updates accordingly
6. **Given** the user is on the MQTT settings page, **When** they click "Reset All Configuration" and confirm the action, **Then** all MQTT and WiFi settings are erased and the device reboots into configuration AP mode within 10 seconds

---

### Edge Cases

- What happens when no audio source is connected (silence)? Device should report "idle" status; playback status remains "idle" until signal exceeds threshold.
- What happens when the network connection drops during active streaming? Device should buffer status updates and republish when reconnected.
- What happens when the broker becomes unavailable? Device should attempt reconnection with exponential backoff and maintain last known state.
- How does the system handle rapid audio transitions (quick play/pause/play sequences)? Status updates should debounce to prevent flooding the broker.
- What happens when multiple devices have the same name? Device identifiers should use unique hardware identifiers (MAC address) to prevent conflicts.
- How does the device behave before broker credentials are configured? Device should operate normally for audio streaming but skip integration attempts.
- What happens when Home Assistant removes the device? Device should continue operating independently and be rediscoverable when needed.
- What happens when the audio threshold is set too low? False positives may occur (noise detected as playback); user can adjust threshold upward through web interface.
- What happens when the audio threshold is set too high? Quiet passages in music may not be detected as playback; user can adjust threshold downward through web interface.
- What happens if user accidentally clicks "Reset All Configuration"? Confirmation dialog prevents accidental reset; once confirmed, all settings are erased and device reboots to AP mode for fresh setup.
- What happens when user resets configuration while device is streaming? Device completes the reset, reboots immediately, and stream clients are disconnected gracefully; audio capture resumes after reconfiguration.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Device MUST announce itself to Home Assistant using the standard auto-discovery mechanism, including device information (name, model, manufacturer, firmware version)
- **FR-002**: Device MUST create a binary sensor entity representing audio playback status with two states: "playing" (audio signal detected) and "idle" (no audio signal)
- **FR-003**: Device MUST detect audio playback by monitoring the input signal level and applying a configurable threshold to distinguish signal from noise
- **FR-003a**: The audio detection threshold MUST be configurable through the MQTT settings page with a default value (e.g., -40dB) and range from -60dB to -20dB
- **FR-003b**: Changes to the audio detection threshold MUST take effect immediately without requiring device reboot
- **FR-004**: Device MUST update the playback status sensor within 2 seconds of detecting a change in audio signal presence
- **FR-005**: Device MUST publish a device attribute containing the current stream URL (reusing the existing HTTP stream endpoint at "http://[ip-address]:8080/stream" from feature 001)
- **FR-006**: Device MUST update the stream URL attribute whenever the network configuration changes (IP address change, port reconfiguration)
- **FR-007**: Device MUST publish availability status indicating whether it is online and operational
- **FR-008**: Device MUST retain connection to the broker and automatically reconnect if the connection is lost, with exponential backoff (starting at 5 seconds, max 5 minutes)
- **FR-009**: Device MUST add an MQTT configuration page to the existing web interface (accessible alongside the existing status page) where users can configure broker connection parameters (host, port, username, password, optional TLS/SSL settings) and audio detection threshold
- **FR-009a**: The MQTT configuration page MUST provide a "Test Connection" button that validates broker connectivity before saving settings
- **FR-009b**: The MQTT configuration page MUST display current connection status (Connected/Disconnected/Error) with descriptive error messages on connection failure
- **FR-009c**: The MQTT configuration page MUST include a threshold control (slider or numeric input) with visual feedback showing current audio level relative to threshold
- **FR-009d**: MQTT configuration settings (broker credentials and audio threshold) MUST be stored in non-volatile storage (NVS) alongside existing WiFi and audio configuration settings
- **FR-010**: Device MUST continue normal audio streaming operation even when the broker is unavailable or not configured
- **FR-011**: Device MUST use a unique device identifier based on hardware MAC address to prevent conflicts with other devices
- **FR-012**: Device MUST publish discovery messages with the "retain" flag so Home Assistant can discover it even if it restarts after the device
- **FR-013**: Device MUST provide a "Reset All Configuration" button on the MQTT settings page that erases all stored settings (MQTT credentials, WiFi credentials, audio threshold) from NVS
- **FR-013a**: The reset button MUST require explicit user confirmation (e.g., "Are you sure? This will erase all settings and reboot the device") before executing
- **FR-013b**: After reset confirmation, the device MUST erase NVS configuration, reboot, and start in WiFi AP mode for initial configuration (same as first-time setup from feature 001)

### Key Entities

- **Audio Playback Status**: A binary state (playing/idle) representing whether the device is currently detecting audio signal above the threshold. Updated in real-time based on signal monitoring. Exposed as a binary sensor in Home Assistant.
- **Device Identity**: Unique identifier, friendly name, model, manufacturer, and firmware version. Used for device registration and identification in Home Assistant.
- **Stream Access Information**: The network address and port where the audio stream can be accessed by clients. Published as a device attribute and updated when network configuration changes.
- **Device Availability**: Online/offline status indicating whether the device is connected to the network and broker. Used by Home Assistant to show device health.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Device appears in Home Assistant within 30 seconds of power-on when broker credentials are configured
- **SC-002**: Playback status updates reflect actual audio signal state with less than 2 seconds latency
- **SC-003**: Stream URL allows successful connection from network clients with less than 3 seconds of connection establishment time
- **SC-004**: Device maintains continuous operation for 7 days with zero crashes related to integration features
- **SC-005**: Device successfully reconnects to broker within 2 minutes after network disruption (100% success rate in 10 test cycles)
- **SC-006**: Zero message loss during 1-hour continuous playback monitoring when network is stable
- **SC-007**: Users can set up the integration without consulting technical documentation (measured by zero support requests for basic setup in beta testing)
- **SC-008**: Users can calibrate the audio detection threshold to their specific setup within 2 minutes by adjusting the slider while monitoring real-time feedback
- **SC-009**: Factory reset via "Reset All Configuration" button completes successfully and device enters AP mode within 15 seconds in 100% of test cases (10 trials)
- **SC-010**: Confirmation dialog prevents accidental resets (measured by zero accidental resets during usability testing with 20 participants)
- **SC-Performance**: Integration features consume less than 5% additional CPU cycles and less than 50KB additional heap memory compared to non-integrated operation
- **SC-Resource**: Message publish rate does not exceed 1 message per second during normal operation to avoid broker overload

## Assumptions

- Users have an existing Home Assistant installation with working broker access
- Users have basic networking knowledge to obtain their broker IP address and credentials
- The device has an existing web server and HTTP streaming endpoint from feature 001-pcm1808-http-streaming that will be reused for stream URL publishing
- The device has an existing web interface with status page that can be extended with an MQTT configuration page
- The device has existing NVS (non-volatile storage) infrastructure for persisting WiFi/audio settings that can store MQTT configuration
- Audio signal detection uses RMS or peak measurement of the audio stream to compare against threshold
- Default audio detection threshold of -40dB is reasonable for typical turntable playback, but users may need to adjust based on their specific setup (preamp gain, turntable output level, noise floor)
- Discovery messages follow Home Assistant's published auto-discovery specification format
- Standard broker port is 1883 (unencrypted) or 8883 (encrypted)
- Device has sufficient memory and processing capacity to handle message publishing without impacting audio quality

## Dependencies

- **Feature 001-pcm1808-http-streaming**: This feature depends on the HTTP streaming infrastructure, web server (port 8080), web interface, and NVS configuration system implemented in feature 001. The MQTT integration will:
  - Extend the existing web interface with a new MQTT settings page
  - Reuse the existing HTTP stream URL (http://[ip]:8080/stream) for publishing to Home Assistant
  - Store MQTT configuration in the existing NVS system alongside WiFi/audio settings
  - Operate without impacting audio streaming quality or performance
