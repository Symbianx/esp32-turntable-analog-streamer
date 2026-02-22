# ESP32 PCM1808 Audio Streamer

High-quality 16-bit audio streaming from turntables (or any analog source) over WiFi using ESP32-S3 and PCM1808 ADC.

[![Platform](https://img.shields.io/badge/platform-ESP32--S3-blue)]()
[![Framework](https://img.shields.io/badge/framework-ESP--IDF%205.3-orange)]()

## Features
- 24-bit stereo audio capture (48kHz, configurable)
- Downsampling to 16-bit WAV streaming over HTTP
- Lock-free ring buffer for smooth playback
- Up to 3 concurrent clients
- WiFi STA mode with auto-reconnect
- Captive portal for WiFi and sample rate setup
- Real-time status and diagnostics page (HTML/JSON)
- Clipping detection and recovery
- Comprehensive serial logging

## Hardware Requirements

### ESP32 Module
- **Board**: ESP32-S3
- **PSRAM**: 8MB (required for audio buffer)
- **Flash**: 4MB minimum
- **Cores**: Dual-core (Core 0: audio, Core 1: networking)

### Audio ADC
- **Chip**: Texas Instruments PCM1808
- **Resolution**: 24-bit
- **Sample rates**: 8kHz - 96kHz
- **Mode**: Slave (ESP32 provides all clocks)
- **Format**: I²S Philips, 24-bit

### Connections

| PCM1808 Pin | Function | ESP32 GPIO | Signal |
|-------------|----------|-----------|--------|
| SCKI (15) | System clock | GPIO9 | MCLK (12.288 MHz @ 48kHz) |
| BCK (14) | Bit clock | GPIO3 | BCK |
| LRCK (13) | Word select | GPIO85 | LRCK |
| DOUT (12) | Audio data | GPIO46 | I²S RX |
| MD0 (5) | Mode 0 | GND | Hardwired LOW |
| MD1 (6) | Mode 1 | GND | Hardwired LOW |
| FMT0 (7) | Format 0 | GND | Hardwired LOW |
| FMT1 (8) | Format 1 | GND | Hardwired LOW |
| VA (1,20) | Analog 5V | 5V | Power |
| VD (10) | Digital 3.3V | 3.3V | Power |
| AGND (2,19) | Analog GND | GND | Ground |
| DGND (9) | Digital GND | GND | Ground |

## Software Setup

### Prerequisites

- [PlatformIO](https://platformio.org/) (VSCode extension or CLI)
- Python 3.7+ (for PlatformIO)
- USB drivers for ESP32 (CP210x or CH340)

### Build & Flash

1. **Clone repository**:
```bash
git clone <repo-url>
cd esp32-turntable-analog-streamer
```

2. **Build**:
```bash
platformio run
```

3. **Flash to ESP32**:
```bash
platformio run --target upload
```

4. **Monitor serial output** (115200 baud):
```bash
platformio device monitor
```

## Usage

### Debugging Initialization with RGB LED

If you have no access to the serial port, you can follow the progress of device initialization using the onboard RGB LED (Neopixel):

| Step                | LED Color         | RGB Value      |
|---------------------|------------------|---------------|
| NVS Init            | Blue             | (0, 0, 32)    |
| WiFi Init           | Cyan             | (0, 32, 32)   |
| Config Portal       | Yellow           | (32, 32, 0)   |
| HTTP Server         | Magenta          | (32, 0, 32)   |
| Audio Buffer        | Orange           | (32, 16, 0)   |
| I²S Init            | Purple           | (16, 0, 32)   |
| Audio Capture       | White            | (32, 32, 32)  |
| Success             | Green            | (0, 32, 0)    |
| Error               | Flashing Red     | (32, 0, 0)    |

**How it works:**
- The LED changes color at the start of each initialization step.
- If a step fails, the LED flashes red and stays red.
- If all steps succeed, the LED turns green.

This allows you to visually debug where the device gets stuck during boot.

### First-Time Setup

1. **Flash firmware** and power on the ESP32
2. Connect to WiFi network **ESP32-Audio-Streamer** from your phone/laptop
3. Navigate to **http://192.168.4.1** (captive portal should redirect automatically)
4. Select your WiFi network, enter password, choose sample rate
5. Click Save — device restarts and connects to your network
6. Check serial output for the assigned IP address

### Stream Audio

Open the stream URL in any media player:

**Browser** (Chrome/Firefox):
```
http://<esp32-ip>/stream.wav
```

**VLC**:
```
Media → Open Network Stream → http://<esp32-ip>/stream.wav
```

**foobar2000**:
```
File → Open Location → http://<esp32-ip>/stream.wav
```

**FFmpeg**:
```bash
ffplay http://<esp32-ip>/stream.wav
```

### Status Page

Navigate to `http://<esp32-ip>/status` to view real-time diagnostics:
- Audio pipeline: sample rate, buffer fill, underruns, clipping status
- System health: CPU usage per core, free heap, uptime
- Network: WiFi RSSI, active clients, stream URL

Also available as JSON: `curl -H "Accept: application/json" http://<esp32-ip>/status`

### Audio Format

- **Container**: WAV (RIFF)
- **Codec**: PCM (uncompressed)
- **Bit depth**: 16-bit
- **Sample rate**: 48000 Hz (default)
- **Channels**: 2 (stereo)
- **Byte rate**: 288 kB/s
- **Latency**: <100ms

### Concurrent Clients

- **Max clients**: 3 simultaneous streams
- **4th client**: Returns HTTP 503 with `Retry-After: 5` header

## Architecture

### Memory Layout

| Region | Size | Usage |
|--------|------|-------|
| Internal SRAM | 327 KB | Code, stack, DMA buffers (8.6 KB) |
| PSRAM | 8 MB | Audio ring buffer (1.1 MB) |
| Flash | 1.5 MB | Firmware |

### Task Distribution

| Core | Task | Priority | Stack | Function |
|------|------|----------|-------|----------|
| 0 | Audio Capture | 24 (highest) | 4 KB | I²S DMA → PSRAM ring buffer |
| 1 | HTTP Server | 6 | 16 KB | Stream serving, status page |
| 1 | WiFi Manager | 8 | 4 KB | Connection, auto-reconnect |
| 1 | Main Loop | 1 | 4 KB | Monitoring, logging |

### Data Flow

```
PCM1808 ADC → I²S (GPIO46) → DMA (Internal SRAM) → Ring Buffer (PSRAM)
                                                           ↓
                                                  HTTP Client 1 ← TCP/IP Stack
                                                  HTTP Client 2 ← (Core 1)
                                                  HTTP Client 3 ←
```

## Roadmap

- [ ] mDNS as `esp32-audio-stream.local`
- [ ] OTA firmware updates
- [ ] Equalization

## License

See [LICENSE](LICENSE) file.
