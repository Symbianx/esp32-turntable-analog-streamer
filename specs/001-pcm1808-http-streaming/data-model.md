# Data Model: PCM1808 ADC to HTTP Audio Streaming

**Feature**: 001-pcm1808-http-streaming
**Date**: 2026-02-13

## Entity: DeviceConfig

Represents the persistent device configuration stored in NVS.

| Field | Type | Size | Default | Validation | Notes |
|-------|------|------|---------|------------|-------|
| `version` | `uint8_t` | 1B | `1` | 1–255 | Schema version for migration |
| `wifi_ssid` | `char[33]` | 33B | `""` | 1–32 chars, null-terminated | WiFi station SSID |
| `wifi_password` | `char[65]` | 65B | `""` | 0–64 chars, null-terminated | WiFi station password (WPA2) |
| `sample_rate` | `uint32_t` | 4B | `48000` | {44100, 48000, 96000} | PCM1808 sample rate in Hz |
| `device_name` | `char[33]` | 33B | `"ESP32-Audio-Stream"` | 1–32 chars, alphanumeric + hyphen | mDNS hostname and AP name |
| `http_port` | `uint16_t` | 2B | `8080` | 1024–65535 | HTTP server listen port |
| `max_clients` | `uint8_t` | 1B | `3` | 1–5 | Max concurrent streaming clients |
| `crc32` | `uint32_t` | 4B | computed | Must match CRC32 of all above fields | Integrity checksum |

**Total size**: ~143 bytes
**NVS namespace**: `device_cfg`
**NVS key**: `config` (single blob)

### State Transitions

```
┌─────────────────┐
│  FACTORY_DEFAULT │ ── Config corrupt / first boot
└────────┬────────┘
         │ User saves config via portal
         ▼
┌─────────────────┐
│   CONFIGURED     │ ── Valid config in NVS
└────────┬────────┘
         │ User changes settings
         ▼
┌─────────────────┐
│   CONFIGURED     │ ── Updated config (version preserved)
└─────────────────┘
```

---

## Entity: AudioStream

Represents the real-time audio data pipeline from ADC to HTTP output.

| Field | Type | Size | Notes |
|-------|------|------|-------|
| `sample_rate` | `uint32_t` | 4B | Active sample rate (44100/48000/96000) |
| `bit_depth` | `uint8_t` | 1B | Always 24 (PCM1808 native) |
| `channels` | `uint8_t` | 1B | Always 2 (stereo) |
| `bytes_per_sample` | `uint8_t` | 1B | 3 (24-bit = 3 bytes) |
| `bytes_per_frame` | `uint8_t` | 1B | 6 (3 bytes × 2 channels) |
| `buffer_size_bytes` | `uint32_t` | 4B | Ring buffer total size (in PSRAM) |
| `buffer_fill_bytes` | `uint32_t` | 4B | Current bytes in ring buffer (atomic read) |
| `buffer_fill_pct` | `float` | 4B | Computed: fill_bytes / size_bytes × 100 |
| `total_frames_captured` | `uint64_t` | 8B | Running count since start |
| `underrun_count` | `uint32_t` | 4B | Buffer underrun events |
| `overrun_count` | `uint32_t` | 4B | Buffer overrun events (writer lapped reader) |
| `is_streaming` | `bool` | 1B | Whether I²S capture is active |
| `is_clipping` | `bool` | 1B | Whether sustained clipping detected (>1s) |

### Ring Buffer Layout (PSRAM Allocation)

```
┌─────────────────────────────────────────────────────┐
│                    Ring Buffer                       │
│  1,152,000 bytes (~2s at 96kHz stereo 24-bit)       │
│  Allocated in 8MB PSRAM via heap_caps_malloc()       │
│                                                      │
│  write_pos ──►  [frame][frame][frame]...             │
│                                                      │
│  read_pos[0] ──► Client 0 read position              │
│  read_pos[1] ──► Client 1 read position              │
│  read_pos[2] ──► Client 2 read position              │
│                                                      │
│  All positions are atomic uint32_t                   │
│  Writer: I²S DMA task (Core 0)                       │
│  Readers: HTTP handler tasks (Core 1)                │
│  DMA buffers: 8,640 bytes in internal SRAM           │
└─────────────────────────────────────────────────────┘
```

---

## Entity: ClientConnection

Represents an active HTTP streaming client.

| Field | Type | Size | Default | Notes |
|-------|------|------|---------|-------|
| `client_id` | `uint8_t` | 1B | auto | Index 0–2 (max 3 clients) |
| `ip_address` | `uint32_t` | 4B | — | Client IPv4 address |
| `connected_at` | `int64_t` | 8B | — | Timestamp (microseconds since boot) |
| `bytes_sent` | `uint64_t` | 8B | `0` | Total bytes streamed to this client |
| `buffer_read_pos` | `uint32_t` | 4B | — | This client's position in ring buffer |
| `underrun_count` | `uint32_t` | 4B | `0` | Times this client had no data available |
| `is_active` | `bool` | 1B | `false` | Whether slot is occupied |
| `socket_fd` | `int` | 4B | `-1` | HTTP socket file descriptor |

### Client Lifecycle

```
┌──────────┐     GET /stream      ┌──────────────┐
│  IDLE    │ ──────────────────►  │  CONNECTING   │
│ (slot    │   Check slot avail   │  Send WAV hdr │
│  free)   │                      └──────┬───────┘
└──────────┘                             │
     ▲                                   ▼
     │ Disconnect              ┌──────────────┐
     │ (client close,          │  STREAMING    │
     │  overrun, error)        │  Send chunks  │
     └─────────────────────────┴──────────────┘
```

**Slot exhaustion**: When all 3 slots are active, new `GET /stream` requests receive HTTP 503 with `Retry-After: 5` header.

---

## Entity: SystemMetrics

Represents real-time system health data exposed on the `/status` endpoint.

| Field | Type | Size | Update Interval | Notes |
|-------|------|------|-----------------|-------|
| `cpu_usage_core0` | `float` | 4B | 1s | Audio core CPU % (target ≤60%) |
| `cpu_usage_core1` | `float` | 4B | 1s | Network core CPU % (target ≤70%) |
| `heap_free_bytes` | `uint32_t` | 4B | 1s | `esp_get_free_heap_size()` |
| `heap_min_free_bytes` | `uint32_t` | 4B | 1s | `esp_get_minimum_free_heap_size()` |
| `wifi_rssi` | `int8_t` | 1B | 5s | WiFi signal strength in dBm |
| `wifi_connected` | `bool` | 1B | event | WiFi STA connection status |
| `uptime_seconds` | `uint32_t` | 4B | 1s | Seconds since boot |
| `i2s_error_count` | `uint32_t` | 4B | event | Cumulative I²S errors |
| `active_clients` | `uint8_t` | 1B | event | Number of connected streaming clients |
| `sample_rate` | `uint32_t` | 4B | event | Currently active sample rate |
| `bit_depth` | `uint8_t` | 1B | — | Always 24 |
| `buffer_fill_pct` | `float` | 4B | 100ms | Audio ring buffer fill level |
| `clipping_detected` | `bool` | 1B | 100ms | Whether ADC clipping is occurring |
| `stream_url` | `char[64]` | 64B | event | e.g., `http://192.168.1.42:8080/stream` |

---

## Entity: WavHeader

Represents the WAV file header prepended to the HTTP audio stream.

| Field | Offset | Size | Value | Notes |
|-------|--------|------|-------|-------|
| `riff_tag` | 0 | 4B | `"RIFF"` | RIFF container marker |
| `riff_size` | 4 | 4B | `0xFFFFFFFF` | Indeterminate (streaming) |
| `wave_tag` | 8 | 4B | `"WAVE"` | WAVE format identifier |
| `fmt_tag` | 12 | 4B | `"fmt "` | Format chunk marker |
| `fmt_size` | 16 | 4B | `16` | PCM format chunk size |
| `audio_format` | 20 | 2B | `1` | PCM (uncompressed) |
| `num_channels` | 22 | 2B | `2` | Stereo |
| `sample_rate` | 24 | 4B | variable | 44100/48000/96000 |
| `byte_rate` | 28 | 4B | computed | sample_rate × channels × bytes_per_sample |
| `block_align` | 32 | 2B | `6` | channels × bytes_per_sample |
| `bits_per_sample` | 34 | 2B | `24` | PCM1808 native depth |
| `data_tag` | 36 | 4B | `"data"` | Data chunk marker |
| `data_size` | 40 | 4B | `0xFFFFFFFF` | Indeterminate (streaming) |

**Total header size**: 44 bytes
**Byte order**: Little-endian (WAV standard)

---

## Relationships

```
DeviceConfig ──1:1──► AudioStream (config determines sample_rate, bit_depth)
AudioStream  ──1:N──► ClientConnection (N ≤ 3, shared ring buffer)
AudioStream  ──1:1──► SystemMetrics (stream stats feed metrics)
AudioStream  ──1:1──► WavHeader (generated from stream parameters)
```
