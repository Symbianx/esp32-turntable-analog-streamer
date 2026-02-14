# Quickstart: PCM1808 ADC to HTTP Audio Streaming

**Feature**: 001-pcm1808-http-streaming

## Prerequisites

### Hardware

- ESP32-DevKitC-WROVER (8MB PSRAM, dual-core Xtensa LX6 @ 240MHz)
- PCM1808 ADC breakout board (already wired to turntable RCA inputs)
- USB cable for programming and serial monitoring
- Turntable or audio source with RCA output (for testing)

### Software

- [PlatformIO CLI](https://docs.platformio.org/en/latest/core/installation.html) or [PlatformIO IDE](https://platformio.org/install/ide) (VS Code extension)
- Python 3.8+ (required by PlatformIO)
- Serial terminal (PlatformIO monitor, minicom, or similar)

### Wiring (PCM1808 ↔ ESP32)

| PCM1808 Pin | ESP32 GPIO | Function | Direction |
|-------------|-----------|----------|-----------|
| SCKI (Pin 15) | GPIO 0 | MCLK (256×fs) | ESP32 → PCM1808 |
| BCK (Pin 14) | GPIO 26 | I²S BCK | ESP32 → PCM1808 |
| LRCK (Pin 13) | GPIO 25 | I²S WS/LRCK | ESP32 → PCM1808 |
| DOUT (Pin 12) | GPIO 27 | I²S Data In | PCM1808 → ESP32 |
| FMT0 (Pin 7) | GND | Format select (I²S) | Hardwired LOW |
| FMT1 (Pin 8) | GND | Format select (24-bit) | Hardwired LOW |
| MD0 (Pin 5) | GND | Mode select (slave) | Hardwired LOW |
| MD1 (Pin 6) | GND | Mode select (slave) | Hardwired LOW |
| VDD (Pin 1, 20) | 5V | Analog power | Power supply |
| VD (Pin 10) | 3.3V | Digital power | ESP32 3.3V |
| AGND (Pin 2, 19) | GND | Analog ground | Common ground |
| DGND (Pin 9) | GND | Digital ground | Common ground |

> **Note**: GPIO 0 is a strapping pin (must be HIGH during boot); the PCM1808 SCKI input
> is high-impedance and does not affect boot. GPIO 0 supports MCLK output via the I²S
> peripheral's clock output function. GPIO 27 is used for I²S data input from the PCM1808.

## Build & Flash

### 1. Clone and Enter Project

```bash
cd turntable-esp32-upnp-streamer
```

### 2. Build Firmware

```bash
pio run
```

This downloads the ESP-IDF toolchain (first run only), compiles the firmware, and links it.

### 3. Flash to ESP32

```bash
pio run --target upload
```

Hold the **BOOT** button on the ESP32 if it doesn't enter flash mode automatically.

### 4. Monitor Serial Output

```bash
pio device monitor
```

You should see boot messages, I²S initialization, and WiFi status.

## First-Time Setup

### 1. Connect to Configuration Portal

On first boot (no stored WiFi credentials), the ESP32 creates a WiFi access point:

- **SSID**: `ESP32-Audio-Stream-XXXX` (last 4 hex digits of MAC address)
- **Password**: None (open network)

Connect to this network from your phone or laptop.

### 2. Configure WiFi and Audio

A captive portal should open automatically. If not, navigate to:

```
http://192.168.4.1
```

On the configuration page:
1. Select your home WiFi network from the dropdown (click "Scan" to refresh)
2. Enter your WiFi password
3. Choose sample rate: **48kHz** (recommended), 44.1kHz, or 96kHz
4. Click **Save**

The device will connect to your WiFi network and display the stream URL.

### 3. Find the Device

After WiFi connection, the device is discoverable via:

- **mDNS**: `http://esp32-audio-stream.local:8080`
- **IP Address**: Shown in serial output and on the status page
- **Status page**: `http://[device-ip]:8080/status`

## Stream Audio

### Open the Stream

Open the stream URL in any media player that supports HTTP audio:

**VLC Media Player**:
```
Media → Open Network Stream → http://esp32-audio-stream.local:8080/stream
```

**foobar2000**:
```
File → Open URL → http://esp32-audio-stream.local:8080/stream
```

**Web Browser** (Chrome/Firefox):
```
Navigate to http://esp32-audio-stream.local:8080/stream
```

**Command Line (ffplay)**:
```bash
ffplay http://esp32-audio-stream.local:8080/stream
```

### Verify Audio

1. Play a record on your turntable
2. Audio should begin playing through your media player within 2-3 seconds
3. Check the status page for diagnostics: `http://esp32-audio-stream.local:8080/status`

## Diagnostics

Navigate to `http://[device-ip]:8080/status` to view:

| Metric | Expected Value |
|--------|---------------|
| Sample Rate | 48000 Hz (or configured) |
| Bit Depth | 24-bit |
| Buffer Fill | 50-80% (healthy range) |
| CPU Core 0 | ≤60% |
| CPU Core 1 | ≤70% |
| WiFi RSSI | > -70 dBm (good) |
| Underrun Count | 0 |
| Clipping | No |

## Troubleshooting

| Symptom | Possible Cause | Fix |
|---------|---------------|-----|
| No audio in player | PCM1808 not connected or wrong GPIO | Check wiring table above |
| Clicks/pops in audio | WiFi interference or weak signal | Move closer to router, check RSSI on status page |
| Stream won't connect | Max clients reached | Close other stream connections (max 3) |
| Device not found on network | mDNS not supported on client | Use IP address from serial output instead |
| "503 Service Unavailable" | Too many streaming clients | Wait for a client to disconnect, then retry |
| Buffer underruns reported | Network congestion | Reduce sample rate to 48kHz or 44.1kHz |
| High CPU usage (>60% Core 0) | 96kHz sample rate | Reduce to 48kHz if not needed |
| Captive portal doesn't open | DNS redirect not working | Manually navigate to http://192.168.4.1 |

## Development

### Run Unit Tests

```bash
pio test
```

### Clean Build

```bash
pio run --target clean && pio run
```

### OTA Update (future)

OTA update support is planned via the dual-partition layout. The partition table reserves space for two application images for safe rollback.

### Configuration via Serial (Advanced)

For debugging, WiFi credentials can also be set via `idf.py` menuconfig:

```bash
pio run --target menuconfig
```

Navigate to: **Project Configuration → WiFi Settings**
