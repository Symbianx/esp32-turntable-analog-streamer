#include "mqtt_client.h"
#include "../config_schema.h"
#include "../storage/nvs_config.h"
#include "../audio/audio_capture.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>
#include <cstdio>

static const char *TAG = "mqtt_client";

// MQTT client state
static esp_mqtt_client_handle_t mqtt_client = nullptr;
static std::atomic<bool> connected{false};
static std::atomic<bool> monitor_running{false};
static TaskHandle_t monitor_task_handle = nullptr;
static char last_error[128] = {0};
static char device_id[32] = {0};
static uint32_t reconnect_delay_ms = 5000;  // Start at 5 seconds

// Device metadata
constexpr const char* DEVICE_NAME = "ESP32 Turntable Streamer";
constexpr const char* DEVICE_MODEL = "PCM1808 Audio Streamer";
constexpr const char* DEVICE_MANUFACTURER = "SymbianX";
constexpr const char* SW_VERSION = "1.0.0";

// MQTT topics (will be constructed with device_id)
static char discovery_topic[256];
static char state_topic[128];
static char attributes_topic[128];
static char availability_topic[128];

bool MQTTClient::init()
{
    ESP_LOGI(TAG, "Initializing MQTT client");
    
    // Load configuration from NVS
    DeviceConfig config;
    if (!NVSConfig::load(config)) {
        ESP_LOGE(TAG, "Failed to load configuration");
        strncpy(last_error, "Failed to load config from NVS", sizeof(last_error) - 1);
        return false;
    }
    
    // Check if MQTT is enabled
    if (!config.mqtt_enabled) {
        ESP_LOGI(TAG, "MQTT is disabled in configuration");
        return true;  // Not an error, just disabled
    }
    
    // Validate broker configuration
    if (strlen(config.mqtt_broker) == 0) {
        ESP_LOGW(TAG, "MQTT broker not configured");
        strncpy(last_error, "Broker address not configured", sizeof(last_error) - 1);
        return false;
    }
    
    // Generate unique device ID from MAC address
    generate_device_id(device_id, sizeof(device_id));
    ESP_LOGI(TAG, "Device ID: %s", device_id);
    
    // Construct topic paths
    snprintf(discovery_topic, sizeof(discovery_topic),
             "homeassistant/binary_sensor/%s/config", device_id);
    snprintf(state_topic, sizeof(state_topic),
             "homeassistant/binary_sensor/%s/state", device_id);
    snprintf(attributes_topic, sizeof(attributes_topic),
             "homeassistant/binary_sensor/%s/attributes", device_id);
    snprintf(availability_topic, sizeof(availability_topic),
             "homeassistant/binary_sensor/%s/availability", device_id);
    
    // Configure MQTT client
    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = config.mqtt_broker;
    mqtt_cfg.broker.address.port = config.mqtt_port;
    
    // Set credentials if provided
    if (strlen(config.mqtt_username) > 0) {
        mqtt_cfg.credentials.username = config.mqtt_username;
        mqtt_cfg.credentials.authentication.password = config.mqtt_password;
    }
    
    // Enable TLS if configured
    if (config.mqtt_use_tls) {
        mqtt_cfg.broker.verification.skip_cert_common_name_check = true;
        mqtt_cfg.broker.verification.use_global_ca_store = true;
    }
    
    // Set Last Will Testament (LWT) for availability
    mqtt_cfg.session.last_will.topic = availability_topic;
    mqtt_cfg.session.last_will.msg = "offline";
    mqtt_cfg.session.last_will.msg_len = 7;
    mqtt_cfg.session.last_will.qos = 1;
    mqtt_cfg.session.last_will.retain = 1;
    
    // Create MQTT client
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_client == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        strncpy(last_error, "esp_mqtt_client_init failed", sizeof(last_error) - 1);
        return false;
    }
    
    // Register event handler
    esp_mqtt_client_register_event(mqtt_client, MQTT_EVENT_ANY, event_handler, nullptr);
    
    ESP_LOGI(TAG, "MQTT client initialized (broker: %s:%d)", config.mqtt_broker, config.mqtt_port);
    return true;
}

bool MQTTClient::start()
{
    if (mqtt_client == nullptr) {
        ESP_LOGW(TAG, "MQTT client not initialized");
        return false;
    }
    
    ESP_LOGI(TAG, "Starting MQTT client");
    
    esp_err_t err = esp_mqtt_client_start(mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %d", err);
        snprintf(last_error, sizeof(last_error), "esp_mqtt_client_start failed: %d", err);
        return false;
    }
    
    return true;
}

bool MQTTClient::stop()
{
    // Stop monitor task
    if (monitor_task_handle != nullptr) {
        monitor_running.store(false, std::memory_order_release);
        vTaskDelay(pdMS_TO_TICKS(100));
        monitor_task_handle = nullptr;
    }
    
    if (mqtt_client == nullptr) {
        return true;
    }
    
    ESP_LOGI(TAG, "Stopping MQTT client");
    
    esp_err_t err = esp_mqtt_client_stop(mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to stop MQTT client: %d", err);
    }
    
    esp_mqtt_client_destroy(mqtt_client);
    mqtt_client = nullptr;
    connected.store(false, std::memory_order_release);
    
    return true;
}

bool MQTTClient::publish_discovery()
{
    if (mqtt_client == nullptr || !connected.load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "Cannot publish discovery: not connected");
        return false;
    }
    
    ESP_LOGI(TAG, "Publishing Home Assistant auto-discovery config");
    
    // Get current IP for stream URL
    char stream_url[128];
    get_stream_url(stream_url, sizeof(stream_url));
    
    // Construct JSON discovery payload
    char payload[768];
    snprintf(payload, sizeof(payload),
        "{"
        "\"name\":\"%s\","
        "\"unique_id\":\"%s\","
        "\"state_topic\":\"%s\","
        "\"availability_topic\":\"%s\","
        "\"json_attributes_topic\":\"%s\","
        "\"payload_on\":\"playing\","
        "\"payload_off\":\"idle\","
        "\"device_class\":\"running\","
        "\"device\":{"
            "\"identifiers\":[\"%s\"],"
            "\"name\":\"%s\","
            "\"model\":\"%s\","
            "\"manufacturer\":\"%s\","
            "\"sw_version\":\"%s\""
        "}"
        "}",
        DEVICE_NAME,
        device_id,
        state_topic,
        availability_topic,
        attributes_topic,
        device_id,
        DEVICE_NAME,
        DEVICE_MODEL,
        DEVICE_MANUFACTURER,
        SW_VERSION
    );
    
    // Publish with QoS 1, retained
    int msg_id = esp_mqtt_client_publish(mqtt_client, discovery_topic, payload, 0, 1, 1);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish discovery");
        return false;
    }
    
    ESP_LOGI(TAG, "Discovery config published (msg_id: %d)", msg_id);
    return true;
}

bool MQTTClient::publish_state(bool is_playing)
{
    if (mqtt_client == nullptr || !connected.load(std::memory_order_acquire)) {
        return false;
    }
    
    const char* state = is_playing ? "playing" : "idle";
    int msg_id = esp_mqtt_client_publish(mqtt_client, state_topic, state, 0, 1, 0);
    
    if (msg_id < 0) {
        ESP_LOGW(TAG, "Failed to publish state");
        return false;
    }
    
    ESP_LOGD(TAG, "State published: %s", state);
    return true;
}

bool MQTTClient::publish_attributes(const char* stream_url)
{
    if (mqtt_client == nullptr || !connected.load(std::memory_order_acquire)) {
        return false;
    }
    
    // Construct JSON attributes payload
    char payload[256];
    snprintf(payload, sizeof(payload), "{\"stream_url\":\"%s\"}", stream_url);
    
    int msg_id = esp_mqtt_client_publish(mqtt_client, attributes_topic, payload, 0, 1, 0);
    
    if (msg_id < 0) {
        ESP_LOGW(TAG, "Failed to publish attributes");
        return false;
    }
    
    ESP_LOGI(TAG, "Attributes published: %s", stream_url);
    return true;
}

bool MQTTClient::reconnect()
{
    if (mqtt_client != nullptr) {
        stop();
    }
    
    if (!init()) {
        return false;
    }
    
    return start();
}

bool MQTTClient::is_connected()
{
    return connected.load(std::memory_order_acquire);
}

const char* MQTTClient::get_last_error()
{
    return last_error;
}

void MQTTClient::monitor_task(void* params)
{
    ESP_LOGI(TAG, "Monitor task started on Core %d", xPortGetCoreID());
    
    monitor_running.store(true, std::memory_order_release);
    bool last_state = false;
    
    while (monitor_running.load(std::memory_order_acquire)) {
        // Check if connected
        if (!connected.load(std::memory_order_acquire)) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        
        // Read current playback status from atomic flag
        bool current_state = AudioCapture::is_playing();
        
        // Publish if state changed
        if (current_state != last_state) {
            ESP_LOGI(TAG, "Playback state changed: %s -> %s",
                     last_state ? "playing" : "idle",
                     current_state ? "playing" : "idle");
            
            if (publish_state(current_state)) {
                last_state = current_state;
            }
        }
        
        // Poll every 500ms
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    ESP_LOGI(TAG, "Monitor task stopped");
    vTaskDelete(nullptr);
}

void MQTTClient::event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    
    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected");
            connected.store(true, std::memory_order_release);
            reconnect_delay_ms = 5000;  // Reset backoff
            
            // Publish availability "online"
            esp_mqtt_client_publish(mqtt_client, availability_topic, "online", 0, 1, 1);
            
            // Publish discovery configuration
            publish_discovery();
            
            // Publish initial stream URL
            char stream_url[128];
            get_stream_url(stream_url, sizeof(stream_url));
            publish_attributes(stream_url);
            
            // Start monitor task if not already running
            if (monitor_task_handle == nullptr) {
                BaseType_t result = xTaskCreatePinnedToCore(
                    monitor_task,
                    "mqtt_monitor",
                    4096,
                    nullptr,
                    5,  // Lower priority than audio
                    &monitor_task_handle,
                    0  // Core 0
                );
                
                if (result != pdPASS) {
                    ESP_LOGE(TAG, "Failed to create monitor task");
                }
            }
            break;
            
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected");
            connected.store(false, std::memory_order_release);
            
            // Exponential backoff: 5s -> 10s -> 20s -> ... -> 300s max
            if (reconnect_delay_ms < 300000) {
                reconnect_delay_ms *= 2;
                if (reconnect_delay_ms > 300000) {
                    reconnect_delay_ms = 300000;
                }
            }
            ESP_LOGI(TAG, "Next reconnect attempt in %lu seconds", reconnect_delay_ms / 1000);
            break;
            
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error: %d", event->error_handle->error_type);
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGE(TAG, "Last errno: %d", event->error_handle->esp_transport_sock_errno);
            }
            break;
            
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGD(TAG, "Message published (msg_id: %d)", event->msg_id);
            break;
            
        default:
            break;
    }
}

void MQTTClient::generate_device_id(char* buffer, size_t len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buffer, len, "esp32_turntable_%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void MQTTClient::get_stream_url(char* buffer, size_t len)
{
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == nullptr) {
        snprintf(buffer, len, "http://unknown:8080/stream");
        return;
    }
    
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        snprintf(buffer, len, "http://unknown:8080/stream");
        return;
    }
    
    snprintf(buffer, len, "http://" IPSTR ":8080/stream", IP2STR(&ip_info.ip));
}
