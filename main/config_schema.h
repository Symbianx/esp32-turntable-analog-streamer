#ifndef CONFIG_SCHEMA_H
#define CONFIG_SCHEMA_H

#include <cstdint>
#include <cstddef>
#include <cstring>

// ─── EQ Types ────────────────────────────────────────────────────────────────

enum class EQFilterType : uint8_t {
    PEAKING    = 0,  // Bell curve boost/cut — custom AudioEQ Cookbook formula
    LOW_SHELF  = 1,  // Boost/cut all frequencies below corner
    HIGH_SHELF = 2,  // Boost/cut all frequencies above corner
    LOW_PASS   = 3,  // Attenuate above corner (gain_db ignored)
    HIGH_PASS  = 4,  // Attenuate below corner (gain_db ignored)
};

struct EQBandConfig {
    bool        enabled;        // Active in signal chain
    EQFilterType filter_type;   // Filter shape
    float       frequency_hz;   // Center/corner frequency (20–20000 Hz)
    float       gain_db;        // Boost(+) / cut(-) in dB, range [-24, +24]
    float       q_factor;       // Bandwidth/resonance (0.1–10.0)
} __attribute__((packed));

// ─── DeviceConfig ─────────────────────────────────────────────────────────────

// DeviceConfig: Persistent device configuration stored in NVS
struct DeviceConfig {
    uint8_t version;              // Schema version (bump to force NVS re-init on size change)
    char wifi_ssid[33];           // WiFi station SSID (null-terminated)
    char wifi_password[65];       // WiFi station password (null-terminated)
    uint32_t sample_rate;         // PCM1808 sample rate (44100/48000/96000)
    char device_name[33];         // mDNS hostname and AP name
    uint16_t http_port;           // HTTP server listen port (1024-65535)
    uint8_t max_clients;          // Max concurrent streaming clients (1-5)
    
    // MQTT Configuration
    bool mqtt_enabled;            // Enable MQTT integration
    char mqtt_broker[128];        // MQTT broker hostname or IP
    uint16_t mqtt_port;           // MQTT broker port (default: 1883)
    char mqtt_username[64];       // MQTT username (optional)
    char mqtt_password[64];       // MQTT password (optional)
    bool mqtt_use_tls;            // Enable TLS/SSL
    float audio_threshold_db;     // Audio detection threshold in dBFS (-60 to -20)

    // EQ Configuration (feature 003)
    bool eq_enabled;              // Master EQ bypass switch
    EQBandConfig eq_bands[10];    // Up to 10 parametric EQ bands
    
    uint32_t crc32;               // Integrity checksum (covers all fields above)

    static constexpr uint8_t  SCHEMA_VERSION       = 2;  // Bumped for EQ fields
    static constexpr uint32_t DEFAULT_SAMPLE_RATE  = 48000;
    static constexpr uint16_t DEFAULT_HTTP_PORT    = 8080;
    static constexpr uint8_t  DEFAULT_MAX_CLIENTS  = 3;
    static constexpr const char* DEFAULT_DEVICE_NAME = "ESP32-Audio-Stream";
    static constexpr uint16_t DEFAULT_MQTT_PORT    = 1883;
    static constexpr float    DEFAULT_AUDIO_THRESHOLD_DB = -40.0f;
} __attribute__((packed));

// ─── AudioStream ──────────────────────────────────────────────────────────────

// AudioStream: Real-time audio data pipeline state
struct AudioStream {
    uint32_t sample_rate;         // Active sample rate (44100/48000/96000)
    uint8_t bit_depth;            // Always 24 (PCM1808 native)
    uint8_t channels;             // Always 2 (stereo)
    uint8_t bytes_per_sample;     // 3 (24-bit = 3 bytes)
    uint8_t bytes_per_frame;      // 6 (3 bytes × 2 channels)
    uint32_t buffer_size_bytes;   // Ring buffer total size (PSRAM)
    uint32_t buffer_fill_bytes;   // Current bytes in ring buffer (atomic)
    float buffer_fill_pct;        // fill_bytes / size_bytes × 100
    uint64_t total_frames_captured; // Running count since start
    uint32_t underrun_count;      // Buffer underrun events
    uint32_t overrun_count;       // Buffer overrun events
    bool is_streaming;            // Whether I²S capture is active
    bool is_clipping;             // Whether sustained clipping detected (>1s)

    static constexpr uint8_t BIT_DEPTH = 24;
    static constexpr uint8_t CHANNELS = 2;
    static constexpr uint8_t BYTES_PER_SAMPLE = 3;
    static constexpr uint8_t BYTES_PER_FRAME = 6;
};

// ClientConnection: Active HTTP streaming client
struct ClientConnection {
    uint8_t client_id;            // Index 0-2 (max 3 clients)
    uint32_t ip_address;          // Client IPv4 address
    int64_t connected_at;         // Timestamp (microseconds since boot)
    uint64_t bytes_sent;          // Total bytes streamed to this client
    uint32_t buffer_read_pos;     // This client's position in ring buffer
    uint32_t underrun_count;      // Times this client had no data available
    bool is_active;               // Whether slot is occupied
    int socket_fd;                // HTTP socket file descriptor

    static constexpr uint8_t MAX_CLIENTS = 3;
};

// SystemMetrics: Real-time system health data
struct SystemMetrics {
    float cpu_usage_core0;        // Audio core CPU % (target ≤60%)
    float cpu_usage_core1;        // Network core CPU % (target ≤70%)
    uint32_t heap_free_bytes;     // esp_get_free_heap_size()
    uint32_t heap_min_free_bytes; // esp_get_minimum_free_heap_size()
    int8_t wifi_rssi;             // WiFi signal strength in dBm
    bool wifi_connected;          // WiFi STA connection status
    uint32_t uptime_seconds;      // Seconds since boot
    uint32_t i2s_error_count;     // Cumulative I²S errors
    uint8_t active_clients;       // Number of connected streaming clients
    uint32_t sample_rate;         // Currently active sample rate
    uint8_t bit_depth;            // Always 24
    float buffer_fill_pct;        // Audio ring buffer fill level
    bool clipping_detected;       // Whether ADC clipping is occurring
    char stream_url[64];          // e.g., "http://192.168.1.42:8080/stream"
};

// WavHeader: WAV file header for HTTP audio stream
struct WavHeader {
    char riff_tag[4];             // "RIFF"
    uint32_t riff_size;           // 0xFFFFFFFF (indeterminate)
    char wave_tag[4];             // "WAVE"
    char fmt_tag[4];              // "fmt "
    uint32_t fmt_size;            // 16 (PCM format chunk size)
    uint16_t audio_format;        // 1 (PCM uncompressed)
    uint16_t num_channels;        // 2 (stereo)
    uint32_t sample_rate;         // 44100/48000/96000
    uint32_t byte_rate;           // sample_rate × channels × bytes_per_sample
    uint16_t block_align;         // channels × bytes_per_sample (6)
    uint16_t bits_per_sample;     // 24
    char data_tag[4];             // "data"
    uint32_t data_size;           // 0xFFFFFFFF (indeterminate)

    static constexpr size_t SIZE = 44;
} __attribute__((packed));

// NVS Key Constants
namespace NVSKeys {
    // WiFi Configuration
    constexpr const char* WIFI_SSID = "wifi_ssid";
    constexpr const char* WIFI_PASSWORD = "wifi_pass";
    
    // Audio Configuration
    constexpr const char* SAMPLE_RATE = "sample_rate";
    constexpr const char* DEVICE_NAME = "device_name";
    constexpr const char* HTTP_PORT = "http_port";
    constexpr const char* MAX_CLIENTS = "max_clients";
    
    // MQTT Configuration
    constexpr const char* MQTT_ENABLED = "mqtt_enabled";
    constexpr const char* MQTT_BROKER = "mqtt_broker";
    constexpr const char* MQTT_PORT = "mqtt_port";
    constexpr const char* MQTT_USERNAME = "mqtt_user";
    constexpr const char* MQTT_PASSWORD = "mqtt_pass";
    constexpr const char* MQTT_USE_TLS = "mqtt_tls";
    constexpr const char* AUDIO_THRESHOLD_DB = "audio_thresh";

    // EQ Configuration (feature 003) — stored in the DeviceConfig blob, keys unused at runtime
    constexpr const char* EQ_ENABLED = "eq_enabled";  // stored in blob; key listed for reference
}

// EQ filter type string helpers (used by HTTP handlers and logging)
inline const char* eq_filter_type_to_str(EQFilterType t) {
    switch (t) {
        case EQFilterType::PEAKING:    return "PEAKING";
        case EQFilterType::LOW_SHELF:  return "LOW_SHELF";
        case EQFilterType::HIGH_SHELF: return "HIGH_SHELF";
        case EQFilterType::LOW_PASS:   return "LOW_PASS";
        case EQFilterType::HIGH_PASS:  return "HIGH_PASS";
        default:                       return "PEAKING";
    }
}

inline EQFilterType eq_filter_type_from_str(const char* s) {
    if (s == nullptr)                   return EQFilterType::PEAKING;
    if (strcmp(s, "LOW_SHELF")  == 0)   return EQFilterType::LOW_SHELF;
    if (strcmp(s, "HIGH_SHELF") == 0)   return EQFilterType::HIGH_SHELF;
    if (strcmp(s, "LOW_PASS")   == 0)   return EQFilterType::LOW_PASS;
    if (strcmp(s, "HIGH_PASS")  == 0)   return EQFilterType::HIGH_PASS;
    return EQFilterType::PEAKING;
}

#endif // CONFIG_SCHEMA_H
