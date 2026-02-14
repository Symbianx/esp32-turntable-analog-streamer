# Research: PCM1808 ADC to HTTP Audio Streaming

**Feature**: 001-pcm1808-http-streaming
**Date**: 2026-02-13
**Status**: Complete (Updated with clarifications)

## Hardware Platform

**Target**: ESP32-DevKitC-WROVER with 8MB PSRAM  
**ADC**: PCM1808 24-bit stereo (slave mode, I²S format, FMT0/FMT1/MODE hardwired LOW)  
**GPIO Pinout**: SCK=GPIO0, BCK=GPIO26, LRCK=GPIO25, DOUT=GPIO27  
**WiFi Security**: WPA2-PSK only  
**Stream Output**: 24-bit PCM (uncompressed, preserves full ADC resolution)  
**Authentication**: None (trusted local network)

## Research Area 1: I²S Configuration for PCM1808 on ESP32

### Decision: Use ESP-IDF I²S Standard (STD) Mode with Philips Format

**Rationale**: The PCM1808 outputs 24-bit I²S data in Philips format. ESP-IDF v5.x+ provides the `i2s_std` driver (replacing the deprecated `i2s.h` legacy driver) which natively supports I²S Philips format with 24-bit data width and stereo slot mode. The ESP32 acts as I²S master, providing BCK (bit clock), LRCK (word select), and MCLK (system clock) to the PCM1808.

**Key Configuration Details**:
- Use `I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_24BIT, I2S_SLOT_MODE_STEREO)` for slot configuration
- Use `I2S_STD_CLK_DEFAULT_CONFIG(sample_rate)` with `mclk_multiple = I2S_MCLK_MULTIPLE_384` (FR-005)
- **CRITICAL**: For 24-bit audio, `mclk_multiple` must be a multiple of 3. Use 384 (not 256).
- ESP32 I²S peripheral supports MCLK output on GPIO0 — required since PCM1808 needs external SCK
- GPIO pinout: `.mclk = GPIO_NUM_0`, `.bclk = GPIO_NUM_26`, `.ws = GPIO_NUM_25`, `.din = GPIO_NUM_27`
- For 24-bit data: buffer must be 3-byte aligned, DMA frame number and buffer size must be multiples of 3
- DMA buffer configuration: 6 descriptors × 240 frames each (240 is multiple of 3 for 24-bit compliance), 240 × 6 bytes = 1,440 bytes per descriptor, total 8,640 bytes internal SRAM (~30ms at 48kHz)
- Use `I2S_CLK_SRC_APLL` for best clock accuracy across all sample rates (critical for 44.1kHz family)

```c
i2s_chan_handle_t rx_handle;
i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
chan_cfg.dma_frame_num = 240;  // Multiple of 3 for 24-bit
chan_cfg.dma_desc_num = 6;     // 6 DMA descriptors

i2s_new_channel(&chan_cfg, NULL, &rx_handle);

i2s_std_config_t std_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(48000),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                    I2S_DATA_BIT_WIDTH_24BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
        .mclk = GPIO_NUM_0,     // SCK → PCM1808 SCKI
        .bclk = GPIO_NUM_26,    // BCK → PCM1808 BCK
        .ws   = GPIO_NUM_25,    // LRCK → PCM1808 LRCK
        .dout = I2S_GPIO_UNUSED,
        .din  = GPIO_NUM_27,    // DOUT ← PCM1808 DOUT
        .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
};
std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;  // Must be multiple of 3 for 24-bit
std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;

i2s_channel_init_std_mode(rx_handle, &std_cfg);
i2s_channel_enable(rx_handle);
```

**Alternatives Considered**:
- **Legacy `i2s.h` driver**: Deprecated in ESP-IDF v5.x. Not forward-compatible.
- **PDM mode**: Only supports 16-bit data width. PCM1808 outputs standard I²S, not PDM.
- **TDM mode**: Designed for multi-channel (>2) codecs. Unnecessary complexity for stereo.

---

## Research Area 2: ESP32 MCLK Generation for PCM1808

### Decision: Use ESP32 I²S MCLK Output Pin (GPIO0)

**Rationale**: The PCM1808 requires a system clock (SCK) at 256×fs, 384×fs, or 512×fs. The ESP32's I²S peripheral can generate MCLK output on GPIO0 (confirmed pinout). For 24-bit audio, the MCLK multiple must be divisible by 3, so we use 384×fs:
- 44.1kHz → 16.9344 MHz
- 48kHz → 18.432 MHz
- 96kHz → 36.864 MHz

The ESP32's internal PLL can generate these frequencies with acceptable jitter for audio applications. The I²S driver handles MCLK generation automatically when `mclk` GPIO is configured. GPIO0 is a strapping pin (LOW during boot enters download mode), but after boot the I²S driver takes control as push-pull MCLK output. The PCM1808 SCKI input is high-impedance and will not affect boot state.

**PCM1808 Pin-to-ESP32 GPIO Mapping**:
| PCM1808 Pin | Function | ESP32 GPIO | Direction |
|-------------|----------|-----------|-----------|
| SCKI (Pin 15) | System clock input | GPIO0 | ESP32 → PCM1808 |
| BCK (Pin 14) | Bit clock | GPIO26 | ESP32 → PCM1808 |
| LRCK (Pin 13) | Word select clock | GPIO25 | ESP32 → PCM1808 |
| DOUT (Pin 12) | Audio data output | GPIO27 | PCM1808 → ESP32 |
| MD0 (Pin 5) | Mode select 0 | Hardwired LOW | N/A |
| MD1 (Pin 6) | Mode select 1 | Hardwired LOW | N/A |
| FMT0 (Pin 7) | Format select 0 | Hardwired LOW | N/A |
| FMT1 (Pin 8) | Format select 1 | Hardwired LOW | N/A |
| RST (Pin 11) | Reset (active low) | TBD GPIO or RC | ESP32 → PCM1808 |

**Alternatives Considered**:
- **External oscillator**: More precise clocking but adds BOM cost and complexity. Not needed unless jitter measurements show problems.
- **LEDC PWM timer**: Can generate clock signals but lacks the precision and PLL-locked synchronization of the I²S MCLK output.
- **APLL (Audio PLL)**: The ESP32 has a dedicated Audio PLL that the I²S driver uses internally. This gives better frequency accuracy than the default PLL for non-standard rates like 44.1kHz.

---

## Research Area 3: Audio Buffer Architecture

### Decision: Lock-Free SPSC Ring Buffer with DMA Double-Buffering

**Rationale**: Constitution mandates lock-free patterns in the audio path (Principle IV) and no heap allocation (Principle III). Architecture:

1. **DMA Layer**: I²S driver uses internal DMA double-buffering. One DMA buffer fills from ADC while the other is read by the application. This is handled by the ESP-IDF I²S driver internally.

2. **Application Ring Buffer**: A single-producer multi-consumer (SPMC) lock-free ring buffer allocated in **8MB PSRAM** (`heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`) sits between the I²S read task and the HTTP streaming task(s). The I²S task writes audio frames, HTTP tasks read them.
   - Size: 2 seconds of audio at max rate (96kHz × 2ch × 3 bytes × 2s = 1,152,000 bytes ≈ 1.1MB) — easily fits in 8MB PSRAM.
   - At 48kHz: 2 seconds = 576,000 bytes (~563KB in PSRAM).
   - PSRAM access is via SPI at 80 MHz (quad-SPI), providing ~40 MB/s bandwidth — far exceeding the ~576 KB/s audio throughput requirement.
   - DMA buffers remain in internal SRAM (8,640 bytes); only the application ring buffer uses PSRAM.

3. **Per-Client Read Pointers**: Each HTTP client maintains its own read position in the ring buffer. Writer advances write pointer; each client reads independently. Clients that fall behind (buffer overrun) are disconnected.

**Alternatives Considered**:
- **FreeRTOS Queue**: Involves kernel calls and potential blocking. Violates Principle IV (no mutexes in audio path).
- **FreeRTOS Stream Buffer**: Suitable for single consumer but doesn't support multiple readers (3 concurrent clients).
- **Shared memory with semaphores**: Blocking synchronization violates real-time guarantees.

---

## Research Area 4: HTTP Streaming Architecture

### Decision: ESP-IDF `esp_http_server` with Chunked Transfer Encoding

**Rationale**: ESP-IDF's built-in `esp_http_server` is lightweight, async, and supports chunked responses — exactly what's needed for indefinite-length audio streaming. Key design:

1. **WAV Header**: Send a WAV header at stream start with `data` chunk size set to 0xFFFFFFFF (indeterminate length) or use RIFF size = 0. Most players (VLC, foobar2000, Chrome) handle this gracefully. WAV header specifies 24-bit, stereo, at the configured sample rate.

2. **Chunked Transfer**: Use `Transfer-Encoding: chunked` with `httpd_resp_send_chunk()`. Each chunk contains a block of 24-bit PCM audio data read from the ring buffer. Chunk size ~1,440 bytes (one DMA buffer worth = 240 frames × 6 bytes).

3. **Content-Type**: `audio/wav` for WAV-wrapped stream. Alternative: `audio/L24;rate=48000;channels=2` for raw 24-bit PCM (better for some UPnP renderers).

4. **Concurrent Clients**: `esp_http_server` supports configuring `max_open_sockets`. Set to 4 (3 streaming + 1 for status/config). Track active stream connections; reject 4th streaming client with HTTP 503.

5. **No Authentication**: All HTTP endpoints (stream, status, config) are open access. The device operates on a trusted local network (WPA2-PSK secured WiFi). No HTTP Basic Auth or token-based auth is needed.

6. **Non-Blocking Reads**: The HTTP handler reads from the PSRAM ring buffer. If no data is available, yield briefly (1-5ms) then retry. Never block indefinitely — this keeps the HTTP server responsive.

**Alternatives Considered**:
- **Raw TCP sockets with lwIP**: Maximum control but requires manual HTTP protocol implementation. Unnecessary complexity.
- **ESP-IDF `esp_https_server`**: Adds TLS overhead. Not needed for local network audio streaming.
- **Third-party HTTP libraries (mongoose, etc.)**: Adds external dependency. Built-in server is sufficient and well-maintained.

---

## Research Area 5: WiFi Provisioning and Captive Portal

### Decision: Custom SoftAP Portal Using `esp_http_server` + DNS Redirect

**Rationale**: The spec requires a web-based config portal at `http://192.168.4.1` (FR-018) with mobile-friendly UI (SC-017). ESP-IDF's built-in `wifi_provisioning` component is designed around Espressif's mobile app (ESP SoftAP Prov), not a browser-based portal. A custom approach gives full control:

1. **SoftAP Mode**: Use `esp_wifi` to create AP named `ESP32-Audio-Stream-[MAC]` when no credentials are stored or STA connection fails after 30 seconds.

2. **DNS Redirect**: Run a minimal DNS server (using `lwip` raw API) that responds to all DNS queries with `192.168.4.1`. This triggers the captive portal detection on iOS/Android/Windows, automatically opening the config page.

3. **Config Portal**: Serve an embedded HTML page via `esp_http_server` at `192.168.4.1`. The page allows SSID selection (from scan results), password entry, and sample rate selection. Mobile-responsive design (min 320px width per SC-017).

4. **Credential Storage**: Save to NVS with CRC32 checksum. On next boot, attempt STA connection with stored credentials.

**Alternatives Considered**:
- **ESP-IDF `wifi_provisioning` component**: Uses Espressif's SoftAP/BLE provisioning protocol. Requires their mobile app for provisioning — doesn't meet the "browser at 192.168.4.1" UX requirement.
- **SmartConfig / ESP-Touch**: Requires Espressif mobile app. Not browser-based.
- **BLE provisioning**: Adds BLE stack overhead (~50KB RAM). Unnecessary when WiFi AP + HTTP portal works.

---

## Research Area 6: NVS Configuration with Integrity Checking

### Decision: ESP-IDF NVS with CRC32 Validation and Versioned Schema

**Rationale**: FR-019 requires NVS storage with checksum validation. Design:

1. **NVS Namespace**: `device_cfg` namespace for all settings.
2. **Stored Fields**: WiFi SSID, WiFi password, sample rate (enum: 44100/48000/96000), device name, HTTP port.
3. **Integrity**: Compute CRC32 over serialized config blob. Store CRC alongside data. On read, validate CRC; if mismatch, revert to factory defaults.
4. **Schema Version**: Store a version byte. If firmware updates change config schema, migration logic converts old format.

**Alternatives Considered**:
- **SPIFFS/LittleFS file**: Slower, more complex, uses flash wear. NVS is purpose-built for key-value config on ESP32.
- **Preferences library (Arduino)**: Not available in pure ESP-IDF. NVS is the underlying mechanism anyway.

---

## Research Area 7: PlatformIO Project Setup for ESP-IDF

### Decision: PlatformIO with `framework = espidf`, `src_dir = src`

**Rationale**: PlatformIO provides seamless ESP-IDF integration. Key configuration:

```ini
[env:esp-wrover-kit]
platform = espressif32
board = esp-wrover-kit
framework = espidf
monitor_speed = 115200
board_build.partitions = partitions.csv
board_build.f_flash = 80000000L
board_build.flash_mode = qio
```

- Board: `esp-wrover-kit` (ESP32-DevKitC-WROVER with 8MB PSRAM)
- Source directory: `src/` (PlatformIO default, with `CMakeLists.txt` for ESP-IDF component registration)
- The `src/CMakeLists.txt` registers the component with `idf_component_register(SRCS ... INCLUDE_DIRS ...)`
- `sdkconfig.defaults` sets compile-time defaults (I²S buffer sizes, WiFi settings, watchdog config)
- Custom partition table allocates NVS (24KB), OTA data (8KB), app0 (1.5MB), app1 (1.5MB)

**Alternatives Considered**:
- **Pure ESP-IDF with CMake**: More complex build setup. PlatformIO abstracts toolchain management.
- **Arduino framework on PlatformIO**: Simpler API but Arduino's I²S library lacks the control needed for 24-bit master mode with MCLK output. ESP-IDF gives direct hardware access.

---

## Research Area 8: mDNS Service Advertisement

### Decision: ESP-IDF `mdns` Component

**Rationale**: FR-021 requires mDNS/Bonjour discovery as `esp32-audio-stream.local`. ESP-IDF's built-in `mdns` component handles this:

1. Initialize with `mdns_init()`
2. Set hostname: `mdns_hostname_set("esp32-audio-stream")`
3. Advertise HTTP service: `mdns_service_add(NULL, "_http", "_tcp", 8080, NULL, 0)`
4. Optionally advertise as UPnP/DLNA device for future discovery by media renderers.

**Alternatives Considered**:
- **Manual mDNS implementation**: Unnecessary. Built-in component is well-tested and maintained.
- **SSDP (UPnP discovery)**: Could be added later for UPnP/DLNA renderer discovery, but mDNS covers FR-021.

---

## Research Area 9: System Clock Generation — 44.1kHz Compatibility

### Decision: Use ESP32 APLL (Audio PLL) for Accurate 44.1kHz Family Clocks

**Rationale**: Standard crystal-based PLLs produce exact multiples of 48kHz but cannot produce exact 44.1kHz multiples. The ESP32 has a dedicated Audio PLL (APLL) that can generate accurate 44.1kHz-family clocks (11.2896 MHz MCLK). The ESP-IDF I²S driver automatically selects APLL when `I2S_CLK_SRC_APLL` is specified as the clock source, or when the default source cannot achieve the requested frequency with acceptable accuracy.

For 48kHz family (48kHz, 96kHz): Default PLL is sufficient.
For 44.1kHz: APLL must be used to avoid sample rate drift.

**Alternatives Considered**:
- **Default PLL only**: Would produce ~44.117kHz instead of 44.100kHz at 48kHz base. Drift of ~0.04% — audible as pitch shift over long playback.
- **External crystal oscillator**: Adds hardware complexity. APLL is sufficient.

---

## Research Area 10: PCM1808 THD+N Specification Advisory

### Finding: Spec Target Exceeds Typical ADC Performance

The PCM1808 datasheet specifies typical THD+N of **-85 dB** at 1 kHz. The feature spec (FR-023, SC-001) targets **-90 dB**, which exceeds the PCM1808's typical capability.

**Recommendation**: Accept -85 dB as the pass/fail threshold for acceptance testing, consistent with the PCM1808's documented performance. The -90 dB figure in the spec should be treated as an aspirational target or adjusted to -85 dB.

**Note**: Dynamic range is 98 dB (A-weighted), and SNR target of ≥95 dB (FR-024, SC-002) is achievable given the PCM1808's 99 dB SNR specification.

---

## Summary: All Research Items Resolved

| Research Area | Decision | Key Impact |
|--------------|----------|------------|
| I²S Driver | ESP-IDF v5.x STD mode, PHILIPS format | GPIO0/26/25/27 pinout confirmed |
| MCLK Generation | I²S MCLK on GPIO0, APLL clock source | 256×fs for all sample rates |
| DMA Buffers | 6×240 frames (8,640 bytes internal SRAM) | ~30ms buffer, multiple of 3 for 24-bit |
| Ring Buffer | SPMC lock-free in 8MB PSRAM (~1.1MB) | 2-second buffer at 96 kHz |
| HTTP Streaming | Chunked WAV, 24-bit output, no auth | 3 concurrent clients, open access |
| WiFi | WPA2-PSK, custom SoftAP portal | Browser-based config at 192.168.4.1 |
| Build System | PlatformIO, esp-wrover-kit board | 8MB PSRAM enabled by default |
| mDNS | ESP-IDF mdns component | esp32-audio-stream.local |
| Clock Accuracy | APLL for 44.1 kHz family | Eliminates pitch drift |
| THD+N | -85 dB realistic (PCM1808 typical) | Spec target of -90 dB is aspirational |
