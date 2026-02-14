# Feature Specification: PCM1808 ADC to HTTP Audio Streaming

**Feature Branch**: `001-pcm1808-http-streaming`  
**Created**: 2026-02-13  
**Status**: Draft  
**Input**: User description: "Build an ESP32 c++ application that takes audio from an external ADC (pcm1808) and streams it via HTTP in uncompressed format. The audio will be fed via RCA to the pcm1808 (already wired). The esp32 should act as the I2S master."

## Clarifications

### Session 2026-02-13

- Q: What is the GPIO pinout configuration for PCM1808 connections (SCK, BCK, LRCK, DOUT, MODE, FMT)? → A: SCK: GPIO0, BCK: GPIO26, LRCK: GPIO25, DOUT: GPIO27, MODE/FMT0/FMT1 hardwired LOW
- Q: What is the exact ESP32 hardware specification (model, PSRAM, Flash)? → A: ESP32-DevKitC-WROVER with 8MB PSRAM
- Q: Which WiFi security modes must be supported? → A: WPA2-PSK only (password-based)
- Q: What is the output bit depth for the HTTP audio stream? → A: 24-bit PCM (preserve full ADC resolution)
- Q: Is HTTP authentication required for stream and status endpoints? → A: No authentication (open access on local network)

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Capture and Stream Turntable Audio (Priority: P1)

A user connects their turntable to the ESP32 device via RCA cables. The ESP32 continuously captures audio from the PCM1808 ADC and makes it available as an HTTP stream. The user opens the HTTP stream URL in their media player (VLC, foobar2000, etc.) and hears their turntable playing in real-time with high fidelity.

**Why this priority**: This is the core value proposition—digitizing analog audio and making it network-accessible. Without this, the device has no purpose.

**Independent Test**: Connect a turntable, power on the ESP32, open http://[esp32-ip]:8080/stream in VLC. Audio plays with <50ms latency and no audible artifacts during a 10-minute listening test.

**Acceptance Scenarios**:

1. **Given** the ESP32 is powered on and connected to WiFi, **When** a user navigates to the HTTP stream URL, **Then** the audio stream begins playing within 2 seconds
2. **Given** audio is streaming, **When** the turntable plays a 1kHz test tone, **Then** the output measures 1kHz ±1Hz with THD+N <-90dB
3. **Given** audio is streaming, **When** the user plays a full vinyl album (45 minutes), **Then** no buffer underruns or dropouts occur
4. **Given** audio is streaming, **When** WiFi signal fluctuates, **Then** the stream buffers gracefully without stopping audio capture

---

### User Story 2 - Device Discovery and Configuration (Priority: P2)

A user powers on the ESP32 for the first time. The device creates a WiFi access point with configuration portal. The user connects to the AP, configures their home WiFi credentials and audio settings (sample rate: 44.1kHz/48kHz/96kHz), then the device connects to their network and displays its stream URL.

**Why this priority**: Essential for user-friendly setup. Without this, users must hard-code WiFi credentials or use serial console configuration, which is not consumer-friendly.

**Independent Test**: Factory reset the device, power on, connect to the AP "ESP32-Audio-Stream", configure WiFi and sample rate via web interface at http://192.168.4.1, verify device connects to home network and displays stream URL.

**Acceptance Scenarios**:

1. **Given** the device has no stored WiFi credentials, **When** powered on, **Then** it creates an access point named "ESP32-Audio-Stream-[MAC]"
2. **Given** the user is connected to the configuration AP, **When** they navigate to http://192.168.4.1, **Then** a configuration portal loads within 3 seconds
3. **Given** the user enters valid WiFi credentials, **When** they submit the form, **Then** the device connects to the network within 15 seconds and displays the stream URL
4. **Given** audio settings are saved, **When** the device reboots, **Then** it automatically reconnects to WiFi and resumes streaming

---

### User Story 3 - Audio Quality Monitoring and Diagnostics (Priority: P3)

A user wants to verify audio quality and system health. They navigate to a diagnostics page at http://[esp32-ip]:8080/status that displays real-time metrics: sample rate, bit depth, buffer fill level, CPU usage, heap usage, WiFi signal strength, THD+N estimate, and uptime. This helps diagnose issues like poor WiFi or suboptimal sample rate selection.

**Why this priority**: Useful for troubleshooting and quality assurance, but not essential for core functionality. Power users and installers will appreciate this.

**Independent Test**: Navigate to http://[esp32-ip]:8080/status, verify all metrics are displayed and update in real-time (refresh every 5 seconds), inject a 1kHz test tone and confirm reported THD+N matches expected values.

**Acceptance Scenarios**:

1. **Given** the device is streaming audio, **When** a user navigates to the status page, **Then** they see current sample rate, bit depth, buffer fill %, CPU usage %, heap usage %, and uptime
2. **Given** WiFi signal degrades, **When** the status page refreshes, **Then** the WiFi RSSI indicator updates to reflect poor signal
3. **Given** audio processing has high CPU load, **When** the status page displays CPU usage, **Then** it shows >50% and warns the user
4. **Given** the user injects a 1kHz test tone, **When** the status page updates, **Then** it displays measured THD+N within 3dB of actual value

---

### Edge Cases

- What happens when no audio source is connected (silence)? System should stream silence (zero samples) without crashing or stalling.
- What happens when WiFi disconnects mid-stream? Device continues capturing audio to circular buffer, attempts reconnection, resumes streaming when WiFi returns.
- What happens when multiple clients connect to the HTTP stream simultaneously? System supports up to 3 concurrent clients; 4th client receives HTTP 503 "Service Unavailable".
- What happens when user changes sample rate while streaming? Stream disconnects gracefully, PCM1808 and I²S are reconfigured, new stream begins within 3 seconds.
- What happens when the PCM1808 is not responding (bad I²S connection)? Device detects missing I²S data within 5 seconds, logs error, displays error on status page, attempts reset/recovery.
- What happens during power-on when PCM1808 is not fully initialized? ESP32 waits up to 500ms for PCM1808 power-up sequence (per datasheet), then begins I²S communication.
- What happens when analog input exceeds 3Vp-p (ADC clipping)? PCM1808 clips gracefully (hard limiting); status page displays clipping indicator if sustained for >1 second.

## Requirements *(mandatory)*

### Functional Requirements

#### Audio Capture Requirements

- **FR-001**: System MUST capture stereo audio from PCM1808 ADC via I²S interface with ESP32 as I²S master
- **FR-002**: System MUST support three sample rates: 44.1kHz, 48kHz, and 96kHz (user-selectable, default 48kHz)
- **FR-003**: System MUST capture audio at 24-bit resolution (PCM1808 native bit depth)
- **FR-004**: System MUST configure PCM1808 in slave mode with ESP32 providing BCK (bit clock) and LRCK (word clock)
- **FR-005**: System MUST generate system clock (SCK) for PCM1808 at 256×fs (e.g., 12.288MHz for 48kHz sampling)
- **FR-006**: System MUST use I²S data format (PCM1808 supports 24-bit I²S and left-justified; I²S selected for compatibility)
- **FR-007**: System MUST use DMA for I²S data transfer to minimize CPU overhead and ensure real-time performance
- **FR-008**: System MUST implement double-buffering: one DMA buffer filling while the other is being streamed

#### Audio Streaming Requirements

- **FR-009**: System MUST stream audio over HTTP as uncompressed 24-bit PCM data (no lossy compression; preserves full ADC resolution)
- **FR-010**: System MUST serve HTTP stream on port 8080 at path /stream (e.g., http://[ip]:8080/stream)
- **FR-011**: System MUST send proper HTTP headers: Content-Type: audio/wav, Transfer-Encoding: chunked, Cache-Control: no-cache
- **FR-012**: System MUST prepend a valid WAV header to the stream indicating sample rate, 24-bit depth, and stereo format
- **FR-013**: System MUST maintain a network buffer of at least 2 seconds of audio to tolerate WiFi jitter (allocated in 8MB PSRAM)
- **FR-014**: System MUST support up to 3 concurrent HTTP clients streaming simultaneously
- **FR-015**: System MUST reject additional client connections beyond the limit with HTTP 503 status code

#### Network and Configuration Requirements

- **FR-016**: System MUST connect to WPA2-PSK secured WiFi networks using stored credentials (SSID and password)
- **FR-017**: System MUST create a WiFi access point for initial configuration if no credentials are stored or connection fails after 30 seconds
- **FR-018**: System MUST provide a web-based configuration portal accessible at http://192.168.4.1 when in AP mode
- **FR-019**: System MUST store WiFi credentials and audio settings in non-volatile storage (NVS) with checksum validation
- **FR-020**: System MUST display stream URL and IP address via status endpoint at http://[ip]:8080/status
- **FR-021**: System MUST support mDNS/Bonjour for device discovery as "esp32-audio-stream.local"

#### Performance and Quality Requirements

- **FR-022**: System MUST maintain end-to-end latency (analog input to HTTP output) below 100ms under normal network conditions
- **FR-023**: System MUST maintain THD+N below -90dB (inherited from PCM1808's -93dB capability, allowing 3dB system overhead)
- **FR-024**: System MUST maintain SNR of at least 95dB (PCM1808 spec: 99dB, allowing 4dB system noise margin)
- **FR-025**: System MUST operate with CPU utilization ≤60% on audio core (Core 0) during continuous streaming
- **FR-026**: System MUST operate with heap usage ≤80% of available SRAM (416KB of 520KB internal SRAM; 8MB PSRAM available for audio buffers)
- **FR-027**: System MUST detect and report buffer underruns/overruns; zero underruns allowed during 4-hour stress test

#### Error Handling and Recovery Requirements

- **FR-028**: System MUST detect I²S communication failure (no data from PCM1808) within 5 seconds and log error
- **FR-029**: System MUST continue audio capture to circular buffer during WiFi disconnection
- **FR-030**: System MUST automatically reconnect to WiFi within 30 seconds of disconnection and resume streaming
- **FR-031**: System MUST gracefully handle ADC clipping (input >3Vp-p) without crashing; display clipping indicator on status page
- **FR-032**: System MUST implement watchdog timer with 10-second timeout to recover from system hangs
- **FR-033**: System MUST log critical errors to serial output (ESP_LOGE) and maintain error count in status page

#### Security and Access Control Requirements

- **FR-034**: System MUST NOT require authentication for HTTP stream endpoint (/stream) or status endpoint (/status) - open access on local network
- **FR-035**: System MUST NOT require authentication for configuration portal in AP mode - simplified first-time setup experience
- **FR-036**: System operates on assumption of trusted local network environment (no internet exposure expected)

#### Hardware Interface Requirements (PCM1808-Specific)

- **FR-037**: PCM1808 MODE, FMT0, and FMT1 pins are hardwired LOW to select I²S format with 24-bit resolution (per PCM1808 datasheet section 8.1)
- **FR-038**: System MUST configure ESP32 WROVER GPIO pins as follows: SCK on GPIO0, BCK on GPIO26, LRCK on GPIO25, DOUT on GPIO27
- **FR-039**: System MUST provide 5V power supply to PCM1808 analog section (VA pin) within 4.75-5.25V range
- **FR-040**: System MUST provide 3.3V power supply to PCM1808 digital section (VD pin) within 3.0-3.6V range
- **FR-041**: System MUST respect PCM1808 power-up sequence: apply VA first, then VD, then start SCK after 500ms settling time
- **FR-042**: System MUST generate SCK (system clock) on GPIO0 at 256×fs frequency (12.288MHz for 48kHz, 11.2896MHz for 44.1kHz, 24.576MHz for 96kHz)
- **FR-043**: System MUST generate BCK (bit clock) on GPIO26 at 64×fs frequency for 24-bit stereo I²S
- **FR-044**: System MUST generate LRCK (word select clock) on GPIO25 at fs frequency (sample rate)
- **FR-045**: System MUST read I²S data from PCM1808 DOUT pin connected to GPIO27

### Key Entities

- **Audio Stream**: Represents the real-time PCM audio data flowing from ADC → ESP32 → HTTP clients. Attributes: sample rate (44.1/48/96kHz), bit depth (24-bit), format (stereo), buffer size (samples), fill level (%).
- **Device Configuration**: Represents stored settings for WiFi (SSID, password, IP mode), Audio (sample rate, bit depth), Network (HTTP port, max clients), System (device name, mDNS name). Persisted in NVS with CRC32 checksum.
- **Client Connection**: Represents an active HTTP streaming client. Attributes: IP address, connection time, bytes sent, buffer underrun count, disconnect reason. Maximum 3 concurrent connections.
- **System Metrics**: Represents real-time health data. Attributes: CPU usage (%), heap free (bytes), WiFi RSSI (dBm), buffer fill (%), I²S error count, uptime (seconds), THD+N estimate (dB).

### Hardware Specification

- **MCU**: ESP32-DevKitC-WROVER module with 8MB PSRAM
- **ADC**: PCM1808 24-bit stereo audio ADC (externally connected via I²S)
- **Audio Input**: Stereo RCA connectors (pre-wired to PCM1808)
- **Power**: USB-powered (5V supplied to PCM1808 VA, 3.3V to PCM1808 VD)

## Success Criteria *(mandatory)*

### Measurable Outcomes

#### Audio Quality Metrics

- **SC-001**: Measured THD+N is below -90dB when streaming a 1kHz sine wave at -20dBFS input level
- **SC-002**: Measured SNR is at least 95dB with silence (no signal) at the RCA input
- **SC-003**: Frequency response is ±1dB from 20Hz to 20kHz when measured with swept sine wave input
- **SC-004**: Stereo channel separation is at least 85dB (left channel signal does not leak into right channel above -85dB)
- **SC-005**: No audible artifacts (clicks, pops, dropouts) during 45-minute vinyl album playback as verified by listening test

#### Performance Metrics

- **SC-006**: End-to-end latency from analog RCA input to HTTP stream output is below 100ms (measured with oscilloscope + audio analyzer)
- **SC-007**: Zero buffer underruns occur during 4-hour continuous streaming stress test with stable WiFi
- **SC-008**: System recovers from WiFi disconnection within 30 seconds and resumes streaming with <3 seconds of audio loss
- **SC-009**: CPU utilization on audio core (Core 0) stays below 60% during streaming at maximum sample rate (96kHz)
- **SC-010**: Heap memory usage stays below 416KB (80% of 520KB SRAM) throughout 24-hour uptime test

#### Reliability Metrics

- **SC-011**: Device successfully streams audio for 24 hours continuously without crashes, reboots, or manual intervention
- **SC-012**: Configuration portal successfully provisions WiFi credentials on first attempt in 95% of test cases (20 trials)
- **SC-013**: Device automatically reconnects to WiFi after network outage within 30 seconds in 100% of test cases (10 trials)
- **SC-014**: HTTP stream connects successfully from VLC, foobar2000, and web browsers (Chrome, Firefox) in 100% of connection attempts

#### User Experience Metrics

- **SC-015**: Stream begins playing in media player within 3 seconds of opening stream URL
- **SC-016**: Status page loads and displays all metrics within 2 seconds of navigation
- **SC-017**: Configuration portal is usable on mobile devices (320px width minimum) without horizontal scrolling
- **SC-018**: Stream URL is clearly displayed on status page and can be copied with one click/tap
