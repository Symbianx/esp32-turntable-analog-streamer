#include "http_server.h"
#include "stream_handler.h"
#include "mqtt_service.h"
#include "../config_schema.h"
#include "../storage/nvs_config.h"
#include "../audio/audio_buffer.h"
#include "../audio/audio_capture.h"
#include "../audio/i2s_master.h"
#include "../system/error_handler.h"
#include "../system/task_manager.h"
#include "../network/wifi_manager.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "freertos/event_groups.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <errno.h>

static const char *TAG = "http_server";

static httpd_handle_t server = nullptr;
static ClientConnection clients[ClientConnection::MAX_CLIENTS];
static uint32_t current_sample_rate = 48000;
static uint16_t current_http_port = DeviceConfig::DEFAULT_HTTP_PORT;

constexpr EventBits_t MQTT_TEST_SUCCESS_BIT = BIT0;
constexpr EventBits_t MQTT_TEST_FAILURE_BIT = BIT1;
static EventGroupHandle_t mqtt_test_event_group = nullptr;
static char mqtt_test_message[128] = {0};

static void add_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

// Context for async streaming task
struct StreamTaskContext {
    httpd_req_t *req;
    int client_id;
    bool is_24bit;
};

// Initialize client slots
static void init_client_slots()
{
    for (int i = 0; i < ClientConnection::MAX_CLIENTS; i++)
    {
        clients[i].is_active = false;
        clients[i].client_id = i;
        clients[i].socket_fd = -1;
        clients[i].bytes_sent = 0;
        clients[i].underrun_count = 0;
    }
}

static void url_decode_inplace(char *value)
{
    char *read_ptr = value;
    char *write_ptr = value;

    while (*read_ptr != '\0') {
        if (*read_ptr == '+') {
            *write_ptr++ = ' ';
            read_ptr++;
        } else if (*read_ptr == '%' && read_ptr[1] != '\0' && read_ptr[2] != '\0') {
            char hex[3] = {read_ptr[1], read_ptr[2], '\0'};
            *write_ptr++ = static_cast<char>(strtol(hex, nullptr, 16));
            read_ptr += 3;
        } else {
            *write_ptr++ = *read_ptr++;
        }
    }

    *write_ptr = '\0';
}

static bool get_form_value(const char *body, const char *key, char *value, size_t value_len)
{
    if (httpd_query_key_value(body, key, value, value_len) == ESP_OK) {
        url_decode_inplace(value);
        return true;
    }

    if (value_len > 0) {
        value[0] = '\0';
    }
    return false;
}

static bool get_checkbox_value(const char *body, const char *key)
{
    char tmp[8];
    return httpd_query_key_value(body, key, tmp, sizeof(tmp)) == ESP_OK;
}

static esp_err_t read_form_body(httpd_req_t *req, char *buffer, size_t buffer_len)
{
    if (req->content_len <= 0 || req->content_len >= static_cast<int>(buffer_len)) {
        return ESP_ERR_INVALID_SIZE;
    }

    int remaining = req->content_len;
    int offset = 0;
    while (remaining > 0) {
        int ret = httpd_req_recv(req, buffer + offset, remaining);
        if (ret <= 0) {
            return ESP_FAIL;
        }
        remaining -= ret;
        offset += ret;
    }

    buffer[offset] = '\0';
    return ESP_OK;
}

static float clamp_threshold_db(float threshold_db)
{
    if (threshold_db < -60.0f) {
        return -60.0f;
    }
    if (threshold_db > -20.0f) {
        return -20.0f;
    }
    return threshold_db;
}

static void build_stream_url(const char *ip, char *buffer, size_t len)
{
    snprintf(buffer, len, "http://%s:%u/stream", ip, static_cast<unsigned>(current_http_port));
}

static bool load_config_or_defaults(DeviceConfig *config)
{
    return NVSConfig::load(config) || NVSConfig::load_factory_defaults(config);
}

static void mqtt_test_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = static_cast<esp_mqtt_event_handle_t>(event_data);

    if (mqtt_test_event_group == nullptr) {
        return;
    }

    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            strncpy(mqtt_test_message, "Connected to broker successfully", sizeof(mqtt_test_message) - 1);
            xEventGroupSetBits(mqtt_test_event_group, MQTT_TEST_SUCCESS_BIT);
            break;
        case MQTT_EVENT_ERROR:
            if (event != nullptr && event->error_handle != nullptr) {
                if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED &&
                    (event->error_handle->connect_return_code == MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
                     event->error_handle->connect_return_code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED)) {
                    strncpy(mqtt_test_message, "Authentication failed", sizeof(mqtt_test_message) - 1);
                } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                    strncpy(mqtt_test_message, "Connection refused", sizeof(mqtt_test_message) - 1);
                } else {
                    strncpy(mqtt_test_message, "Network unreachable", sizeof(mqtt_test_message) - 1);
                }
            } else {
                strncpy(mqtt_test_message, "Connection error", sizeof(mqtt_test_message) - 1);
            }
            xEventGroupSetBits(mqtt_test_event_group, MQTT_TEST_FAILURE_BIT);
            break;
        case MQTT_EVENT_DISCONNECTED:
            if ((xEventGroupGetBits(mqtt_test_event_group) & MQTT_TEST_SUCCESS_BIT) == 0) {
                strncpy(mqtt_test_message, "Connection closed", sizeof(mqtt_test_message) - 1);
                xEventGroupSetBits(mqtt_test_event_group, MQTT_TEST_FAILURE_BIT);
            }
            break;
        default:
            break;
    }
}

static bool run_mqtt_connection_test(const DeviceConfig &config, char *message, size_t message_len)
{
    mqtt_test_event_group = xEventGroupCreate();
    if (mqtt_test_event_group == nullptr) {
        snprintf(message, message_len, "Unable to allocate test context");
        return false;
    }

    mqtt_test_message[0] = '\0';
    char broker_uri[192];
    snprintf(broker_uri, sizeof(broker_uri), "%s://%s:%u",
             config.mqtt_use_tls ? "mqtts" : "mqtt",
             config.mqtt_broker,
             static_cast<unsigned>(config.mqtt_port));

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = broker_uri;
    mqtt_cfg.network.timeout_ms = 5000;
    mqtt_cfg.network.disable_auto_reconnect = true;
    mqtt_cfg.session.keepalive = 10;
    mqtt_cfg.buffer.size = 1024;
    mqtt_cfg.buffer.out_size = 1024;
    if (strlen(config.mqtt_username) > 0) {
        mqtt_cfg.credentials.username = config.mqtt_username;
        mqtt_cfg.credentials.authentication.password = config.mqtt_password;
    }

    esp_mqtt_client_handle_t test_client = esp_mqtt_client_init(&mqtt_cfg);
    if (test_client == nullptr) {
        vEventGroupDelete(mqtt_test_event_group);
        mqtt_test_event_group = nullptr;
        snprintf(message, message_len, "Failed to initialize MQTT client");
        return false;
    }

    esp_mqtt_client_register_event(test_client, MQTT_EVENT_ANY, mqtt_test_event_handler, nullptr);
    if (esp_mqtt_client_start(test_client) != ESP_OK) {
        esp_mqtt_client_destroy(test_client);
        vEventGroupDelete(mqtt_test_event_group);
        mqtt_test_event_group = nullptr;
        snprintf(message, message_len, "Failed to start MQTT client");
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(
        mqtt_test_event_group,
        MQTT_TEST_SUCCESS_BIT | MQTT_TEST_FAILURE_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(5000));

    bool success = (bits & MQTT_TEST_SUCCESS_BIT) != 0;
    if (bits == 0) {
        snprintf(message, message_len, "Connection timeout");
    } else {
        snprintf(message, message_len, "%s", mqtt_test_message[0] != '\0' ? mqtt_test_message : (success ? "Connected to broker successfully" : "Connection failed"));
    }

    esp_mqtt_client_stop(test_client);
    esp_mqtt_client_destroy(test_client);
    vEventGroupDelete(mqtt_test_event_group);
    mqtt_test_event_group = nullptr;
    return success;
}

static void reboot_task(void *params)
{
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

// Find free client slot
static int find_free_slot()
{
    for (int i = 0; i < ClientConnection::MAX_CLIENTS; i++)
    {
        if (!clients[i].is_active)
        {
            return i;
        }
    }
    return -1; // No free slot
}

static esp_err_t mqtt_settings_get_handler(httpd_req_t *req)
{
    DeviceConfig config;
    load_config_or_defaults(&config);
    config.audio_threshold_db = clamp_threshold_db(config.audio_threshold_db);

    char page[8192];
    int len = snprintf(
        page,
        sizeof(page),
        "<!DOCTYPE html><html><head>"
        "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>MQTT Settings</title>"
        "<style>body{font-family:system-ui,sans-serif;max-width:760px;margin:0 auto;padding:16px;background:#f3f4f6;color:#111827}"
        ".card{background:#fff;border:1px solid #d1d5db;border-radius:12px;padding:18px;margin-bottom:16px}"
        "h1,h2{margin:0 0 12px}.row{display:grid;gap:10px;margin-bottom:12px}.two{grid-template-columns:1fr 1fr}"
        "label{display:block;font-size:14px;font-weight:600;margin-bottom:6px}"
        "input{width:100%%;padding:10px;border:1px solid #9ca3af;border-radius:8px;font-size:14px}"
        "input[type=checkbox]{width:auto;margin-right:8px}input[type=range]{width:100%%}"
        "button,a.btn{display:inline-block;padding:10px 14px;border-radius:8px;border:none;background:#111827;color:#fff;text-decoration:none;cursor:pointer;margin-right:8px}"
        "button.alt{background:#4b5563}.danger{background:#b91c1c}.status{padding:10px 12px;border-radius:8px;background:#e5e7eb;margin-top:12px}"
        ".metric{font-family:monospace;font-size:15px}.small{font-size:13px;color:#4b5563}</style></head><body>"
        "<div class='card'><h1>MQTT Settings</h1><p class='small'>Configure Home Assistant discovery, playback state publishing, and the stream URL attribute.</p>"
        "<form id='mqttForm' method='POST' action='/mqtt-settings'>"
        "<div class='row'><label><input type='checkbox' name='enabled' %s>Enable MQTT integration</label></div>"
        "<div class='row two'><div><label>Broker</label><input type='text' name='broker' maxlength='127' value='%s' placeholder='192.168.1.100'></div>"
        "<div><label>Port</label><input type='number' name='port' min='1' max='65535' value='%u'></div></div>"
        "<div class='row two'><div><label>Username</label><input type='text' name='username' maxlength='63' value='%s'></div>"
        "<div><label>Password</label><input type='password' name='password' maxlength='63' value='%s'></div></div>"
        "<div class='row'><label><input type='checkbox' name='use_tls' %s>Use TLS</label></div>"
        "<div class='row'><div><label>Audio Threshold: <span id='thresholdValue'>%.1f dB</span></label>"
        "<input id='threshold' type='range' name='threshold_db' min='-60' max='-20' step='0.5' value='%.1f'></div></div>"
        "<div class='row'><div><label>Current Audio Level</label><div id='audioLevel' class='status metric'>Loading...</div></div></div>"
        "<div class='row'><div><button type='submit'>Save</button><button type='button' class='alt' onclick='testConnection()'>Test Connection</button><button type='button' class='danger' onclick='resetConfig()'>Reset All Configuration</button><a class='btn alt' href='/status'>Back to Status</a></div></div>"
        "</form><div id='result' class='status'>MQTT is %s.</div></div>"
        "<script>"
        "const threshold=document.getElementById('threshold');const thresholdValue=document.getElementById('thresholdValue');"
        "let audioLevelRequestInFlight=false;"
        "threshold.addEventListener('input',()=>{thresholdValue.textContent=threshold.value+' dB';});"
        "async function refreshAudioLevel(){if(audioLevelRequestInFlight)return;audioLevelRequestInFlight=true;try{const response=await fetch('/api/audio-level',{cache:'no-store'});const data=await response.json();document.getElementById('audioLevel').textContent=data.rms_db.toFixed(1)+' dB | threshold '+data.threshold_db.toFixed(1)+' dB | '+(data.playing?'playing':'idle');}catch(e){document.getElementById('audioLevel').textContent='Unavailable';}finally{audioLevelRequestInFlight=false;}}"
        "async function testConnection(){const form=new FormData(document.getElementById('mqttForm'));const body=new URLSearchParams(form).toString();const response=await fetch('/mqtt-settings/test',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});const data=await response.json();document.getElementById('result').textContent=data.message;document.getElementById('result').style.background=(data.status==='success')?'#dcfce7':'#fee2e2';}"
        "async function resetConfig(){if(!confirm('Erase all configuration and reboot to setup mode?'))return;const response=await fetch('/mqtt-settings/reset',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'confirm=yes'});document.open();document.write(await response.text());document.close();}"
        "refreshAudioLevel();setInterval(refreshAudioLevel,5000);"
        "</script></body></html>",
        config.mqtt_enabled ? "checked" : "",
        config.mqtt_broker,
        static_cast<unsigned>(config.mqtt_port),
        config.mqtt_username,
        config.mqtt_password,
        config.mqtt_use_tls ? "checked" : "",
        config.audio_threshold_db,
        config.audio_threshold_db,
        MQTTClient::is_connected() ? "connected" : "disconnected");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page, len);
}

static esp_err_t mqtt_settings_post_handler(httpd_req_t *req)
{
    char body[1024];
    if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form body");
        return ESP_FAIL;
    }

    DeviceConfig current_config;
    load_config_or_defaults(&current_config);
    DeviceConfig new_config = current_config;

    char buffer[160];
    new_config.mqtt_enabled = get_checkbox_value(body, "enabled");
    get_form_value(body, "broker", new_config.mqtt_broker, sizeof(new_config.mqtt_broker));

    if (get_form_value(body, "port", buffer, sizeof(buffer)) && strlen(buffer) > 0) {
        int port = atoi(buffer);
        if (port < 1 || port > 65535) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "MQTT port must be between 1 and 65535");
            return ESP_FAIL;
        }
        new_config.mqtt_port = static_cast<uint16_t>(port);
    }

    get_form_value(body, "username", new_config.mqtt_username, sizeof(new_config.mqtt_username));
    get_form_value(body, "password", new_config.mqtt_password, sizeof(new_config.mqtt_password));
    new_config.mqtt_use_tls = get_checkbox_value(body, "use_tls");
    new_config.audio_threshold_db = current_config.audio_threshold_db;
    if (get_form_value(body, "threshold_db", buffer, sizeof(buffer)) && strlen(buffer) > 0) {
        new_config.audio_threshold_db = clamp_threshold_db(strtof(buffer, nullptr));
    }

    if (new_config.mqtt_enabled && strlen(new_config.mqtt_broker) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Broker is required when MQTT is enabled");
        return ESP_FAIL;
    }

    if ((strlen(new_config.mqtt_username) == 0) != (strlen(new_config.mqtt_password) == 0)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Username and password must both be set or both be empty");
        return ESP_FAIL;
    }

    if (!NVSConfig::save(&new_config)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save configuration");
        return ESP_FAIL;
    }

    AudioCapture::set_threshold_db(new_config.audio_threshold_db);

    bool mqtt_changed =
        current_config.mqtt_enabled != new_config.mqtt_enabled ||
        current_config.mqtt_port != new_config.mqtt_port ||
        current_config.mqtt_use_tls != new_config.mqtt_use_tls ||
        strcmp(current_config.mqtt_broker, new_config.mqtt_broker) != 0 ||
        strcmp(current_config.mqtt_username, new_config.mqtt_username) != 0 ||
        strcmp(current_config.mqtt_password, new_config.mqtt_password) != 0;

    if (new_config.mqtt_enabled) {
        if (mqtt_changed || !MQTTClient::is_connected()) {
            MQTTClient::reconnect();
        }
    } else {
        MQTTClient::stop();
    }

    char response[1024];
    int len = snprintf(
        response,
        sizeof(response),
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>MQTT Saved</title>"
        "<style>body{font-family:system-ui,sans-serif;max-width:640px;margin:0 auto;padding:24px;background:#f3f4f6;color:#111827}.card{background:#fff;border:1px solid #d1d5db;border-radius:12px;padding:20px}a{display:inline-block;margin-top:12px}</style></head><body>"
        "<div class='card'><h1>Settings saved</h1><p>MQTT is %s.</p><p>Connection status: %s</p><p>Audio threshold: %.1f dB</p><a href='/mqtt-settings'>Back to MQTT settings</a></div></body></html>",
        new_config.mqtt_enabled ? "enabled" : "disabled",
        MQTTClient::is_connected() ? "Connected" : (new_config.mqtt_enabled ? MQTTClient::get_last_error() : "Disabled"),
        new_config.audio_threshold_db);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, response, len);
}

static esp_err_t mqtt_settings_test_handler(httpd_req_t *req)
{
    char body[1024];
    if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Invalid request\"}");
    }

    DeviceConfig config;
    load_config_or_defaults(&config);
    config.mqtt_enabled = get_checkbox_value(body, "enabled");
    get_form_value(body, "broker", config.mqtt_broker, sizeof(config.mqtt_broker));
    char buffer[160];
    if (get_form_value(body, "port", buffer, sizeof(buffer)) && strlen(buffer) > 0) {
        int port = atoi(buffer);
        if (port > 0 && port <= 65535) {
            config.mqtt_port = static_cast<uint16_t>(port);
        }
    }
    get_form_value(body, "username", config.mqtt_username, sizeof(config.mqtt_username));
    get_form_value(body, "password", config.mqtt_password, sizeof(config.mqtt_password));
    config.mqtt_use_tls = get_checkbox_value(body, "use_tls");

    httpd_resp_set_type(req, "application/json");
    if (strlen(config.mqtt_broker) == 0) {
        return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Broker is required\"}");
    }

    if ((strlen(config.mqtt_username) == 0) != (strlen(config.mqtt_password) == 0)) {
        return httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Username and password must both be set or both be empty\"}");
    }

    char message[128];
    bool success = run_mqtt_connection_test(config, message, sizeof(message));
    char response[256];
    int len = snprintf(response, sizeof(response), "{\"status\":\"%s\",\"message\":\"%s\"}", success ? "success" : "error", message);
    return httpd_resp_send(req, response, len);
}

static esp_err_t mqtt_settings_reset_handler(httpd_req_t *req)
{
    char body[64];
    if (read_form_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing confirmation");
        return ESP_FAIL;
    }

    char confirm[8];
    if (!get_form_value(body, "confirm", confirm, sizeof(confirm)) || strcmp(confirm, "yes") != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "confirm=yes is required");
        return ESP_FAIL;
    }

    if (!NVSConfig::erase_all()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to erase configuration");
        return ESP_FAIL;
    }

    MQTTClient::stop();
    xTaskCreatePinnedToCore(reboot_task, "reboot", 2048, nullptr, 2, nullptr, 1);

    const char *response =
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>Resetting</title></head><body>"
        "<h1>Resetting device</h1><p>All configuration has been erased. The device will reboot to setup mode in about 2 seconds.</p>"
        "<p>Reconnect to the ESP32 access point after reboot.</p></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
} 

static esp_err_t audio_level_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    add_cors_headers(req);

    char response[160];
    int len = snprintf(response, sizeof(response),
        "{\"rms_db\":%.2f,\"threshold_db\":%.2f,\"playing\":%s}",
        AudioCapture::get_current_rms_db(),
        AudioCapture::get_threshold_db(),
        AudioCapture::is_playing() ? "true" : "false");
    return httpd_resp_send(req, response, len);
}

// Async streaming task - runs independently from HTTP worker thread
static void stream_task(void *arg)
{
    StreamTaskContext *ctx = (StreamTaskContext *)arg;
    httpd_req_t *req = ctx->req;
    int client_id = ctx->client_id;
    
    bool is_24bit = ctx->is_24bit;
    ESP_LOGI(TAG, "Stream task started for client %d (%d-bit)", client_id, is_24bit ? 24 : 16);

    // Chunk aligned to DMA production unit (240 frames × 6 bytes = 5ms at 48kHz)
    uint8_t audio_chunk_24bit[1440];  // 24-bit input from ring buffer
    uint8_t audio_chunk_16bit[960];   // 16-bit output for HTTP stream (1440 * 2/3)
    size_t bytes_read;
    uint32_t last_log_time = esp_timer_get_time() / 1000000;
    uint32_t period_bytes = 0;
    uint32_t empty_waits = 0;

    // Pacing: match send rate to audio production rate
    // 16-bit: sample_rate × 2ch × 2bytes, 24-bit: sample_rate × 2ch × 3bytes
    uint32_t byte_rate = is_24bit ? (current_sample_rate * 6) : (current_sample_rate * 4);

    while (clients[client_id].is_active)
    {
        TickType_t iter_start = xTaskGetTickCount();

        // Read from ring buffer (24-bit data)
        if (!AudioBuffer::read(client_id, audio_chunk_24bit, sizeof(audio_chunk_24bit), &bytes_read))
        {
            ESP_LOGE(TAG, "Ring buffer read error for client %d", client_id);
            break;
        }

        if (bytes_read == 0)
        {
            // No data - wait for capture to produce more (do NOT send silence)
            empty_waits++;
            if (empty_waits == 500)
            {
                ESP_LOGW(TAG, "Client %d: buffer starved for %u waits", client_id, empty_waits);
            }
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        empty_waits = 0;

        size_t send_bytes;
        const uint8_t *send_buf;

        if (is_24bit) {
            // 24-bit mode: send ring buffer data directly (already 24-bit packed)
            send_buf = audio_chunk_24bit;
            send_bytes = bytes_read;
        } else {
            // 16-bit mode: downsample with TPDF dithering
            send_bytes = StreamHandler::downsample_24to16(audio_chunk_24bit, audio_chunk_16bit, bytes_read);
            if (send_bytes == 0)
            {
                ESP_LOGE(TAG, "Downsampling failed for client %d", client_id);
                break;
            }
            send_buf = audio_chunk_16bit;
        }

        // Send audio chunk
        if (httpd_resp_send_chunk(req, (const char *)send_buf, send_bytes) != ESP_OK)
        {
            ESP_LOGI(TAG, "Client %d disconnected", client_id);
            break;
        }

        clients[client_id].bytes_sent += send_bytes;
        period_bytes += send_bytes;

        // Log throughput every 10 seconds
        uint32_t now = esp_timer_get_time() / 1000000;
        uint32_t elapsed = now - last_log_time;
        if (elapsed >= 10)
        {
            uint32_t kbps = (period_bytes * 8) / (elapsed * 1000);
            uint32_t target_kbps = (byte_rate * 8) / 1000;
            ESP_LOGI(TAG, "Client %d (%d-bit): %u kbps (target: %u), total %llu bytes",
                     client_id, is_24bit ? 24 : 16, kbps, target_kbps, clients[client_id].bytes_sent);
            period_bytes = 0;
            last_log_time = now;
        }

        // Check if audio capture is still running
        if (!AudioCapture::is_running())
        {
            ESP_LOGW(TAG, "Audio capture stopped, ending stream");
            break;
        }

        // Pace sends to match audio production rate.
        // Calculate how long this chunk represents in real-time, then wait
        // the remaining duration after subtracting time already spent on
        // the read+send. This prevents burst-drain-starve buffer oscillation.
        // Use send_bytes / byte_rate since both reflect the same bit-depth.
        TickType_t target_ticks = pdMS_TO_TICKS((send_bytes * 1000) / byte_rate);
        if (target_ticks < 1) target_ticks = 1;
        TickType_t elapsed_ticks = xTaskGetTickCount() - iter_start;
        if (elapsed_ticks < target_ticks)
        {
            vTaskDelay(target_ticks - elapsed_ticks);
        }
    }

    // Clean up client connection
    ESP_LOGI(TAG, "Client %d disconnecting (sent %llu bytes)",
             client_id, clients[client_id].bytes_sent);

    AudioBuffer::unregister_client(client_id);
    clients[client_id].is_active = false;
    clients[client_id].socket_fd = -1;

    // Send empty chunk to signal end of stream
    httpd_resp_send_chunk(req, nullptr, 0);
    
    // Mark async request complete and free resources
    httpd_req_async_handler_complete(req);
    
    free(ctx);
    vTaskDelete(NULL);
}

// Stream handler for /stream endpoint - returns immediately using async pattern
static esp_err_t stream_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "New stream request from client");

    // Check for available slot
    int client_id = find_free_slot();
    if (client_id < 0)
    {
        ESP_LOGW(TAG, "Max clients reached, rejecting connection");
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_hdr(req, "Retry-After", "5");
        httpd_resp_sendstr(req, "Maximum clients reached. Please try again later.");
        return ESP_OK;
    }

    // Register client with audio buffer
    if (!AudioBuffer::register_client(client_id))
    {
        ESP_LOGE(TAG, "Failed to register client %d with audio buffer", client_id);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Mark slot as active
    clients[client_id].is_active = true;
    clients[client_id].socket_fd = httpd_req_to_sockfd(req);
    clients[client_id].bytes_sent = 0;
    clients[client_id].underrun_count = 0;
    clients[client_id].connected_at = esp_timer_get_time();

    ESP_LOGI(TAG, "Client %d connected (socket fd: %d)", client_id, clients[client_id].socket_fd);

    // Set TCP_NODELAY on streaming socket for lower latency
    int fd = httpd_req_to_sockfd(req);
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    // Build and send WAV header
    WavHeader wav_header;
    StreamHandler::build_wav_header(&wav_header, current_sample_rate);

    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (httpd_resp_send_chunk(req, (const char *)&wav_header, sizeof(WavHeader)) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to send WAV header to client %d", client_id);
        AudioBuffer::unregister_client(client_id);
        clients[client_id].is_active = false;
        clients[client_id].socket_fd = -1;
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    clients[client_id].bytes_sent += sizeof(WavHeader);

    // Create async request copy for background streaming
    httpd_req_t *async_req = nullptr;
    esp_err_t err = httpd_req_async_handler_begin(req, &async_req);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create async handler for client %d: %d", client_id, err);
        AudioBuffer::unregister_client(client_id);
        clients[client_id].is_active = false;
        clients[client_id].socket_fd = -1;
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Allocate context for streaming task
    StreamTaskContext *ctx = (StreamTaskContext *)malloc(sizeof(StreamTaskContext));
    if (ctx == nullptr)
    {
        ESP_LOGE(TAG, "Failed to allocate context for client %d", client_id);
        httpd_req_async_handler_complete(async_req);
        AudioBuffer::unregister_client(client_id);
        clients[client_id].is_active = false;
        clients[client_id].socket_fd = -1;
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ctx->req = async_req;
    ctx->client_id = client_id;
    ctx->is_24bit = false;

    // Spawn streaming task on Core 1 (network core)
    char task_name[16];
    snprintf(task_name, sizeof(task_name), "stream_%d", client_id);
    BaseType_t result = xTaskCreatePinnedToCore(
        stream_task,
        task_name,
        16384,          // Stack size (16KB for audio chunks)
        ctx,
        6,              // Priority (same as HTTP server)
        nullptr,
        1               // Core 1 (network core)
    );

    if (result != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create streaming task for client %d", client_id);
        httpd_req_async_handler_complete(async_req);
        free(ctx);
        AudioBuffer::unregister_client(client_id);
        clients[client_id].is_active = false;
        clients[client_id].socket_fd = -1;
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Return immediately - worker thread is now free for other requests
    ESP_LOGI(TAG, "Stream handler returning, client %d now served by async task", client_id);
    return ESP_OK;
}

// Stream handler for /stream24.wav endpoint - 24-bit native resolution
static esp_err_t stream_24bit_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "New 24-bit stream request from client");

    int client_id = find_free_slot();
    if (client_id < 0)
    {
        ESP_LOGW(TAG, "Max clients reached, rejecting connection");
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_hdr(req, "Retry-After", "5");
        httpd_resp_sendstr(req, "Maximum clients reached. Please try again later.");
        return ESP_OK;
    }

    if (!AudioBuffer::register_client(client_id))
    {
        ESP_LOGE(TAG, "Failed to register client %d with audio buffer", client_id);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    clients[client_id].is_active = true;
    clients[client_id].socket_fd = httpd_req_to_sockfd(req);
    clients[client_id].bytes_sent = 0;
    clients[client_id].underrun_count = 0;
    clients[client_id].connected_at = esp_timer_get_time();

    ESP_LOGI(TAG, "Client %d connected for 24-bit stream (socket fd: %d)", client_id, clients[client_id].socket_fd);

    int fd = httpd_req_to_sockfd(req);
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    // Build and send 24-bit WAV header
    WavHeader wav_header;
    StreamHandler::build_wav_header_24bit(&wav_header, current_sample_rate);

    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (httpd_resp_send_chunk(req, (const char *)&wav_header, sizeof(WavHeader)) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to send WAV header to client %d", client_id);
        AudioBuffer::unregister_client(client_id);
        clients[client_id].is_active = false;
        clients[client_id].socket_fd = -1;
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    clients[client_id].bytes_sent += sizeof(WavHeader);

    httpd_req_t *async_req = nullptr;
    esp_err_t err = httpd_req_async_handler_begin(req, &async_req);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create async handler for client %d: %d", client_id, err);
        AudioBuffer::unregister_client(client_id);
        clients[client_id].is_active = false;
        clients[client_id].socket_fd = -1;
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    StreamTaskContext *ctx = (StreamTaskContext *)malloc(sizeof(StreamTaskContext));
    if (ctx == nullptr)
    {
        ESP_LOGE(TAG, "Failed to allocate context for client %d", client_id);
        httpd_req_async_handler_complete(async_req);
        AudioBuffer::unregister_client(client_id);
        clients[client_id].is_active = false;
        clients[client_id].socket_fd = -1;
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ctx->req = async_req;
    ctx->client_id = client_id;
    ctx->is_24bit = true;

    char task_name[16];
    snprintf(task_name, sizeof(task_name), "stream24_%d", client_id);
    BaseType_t result = xTaskCreatePinnedToCore(
        stream_task,
        task_name,
        16384,
        ctx,
        6,
        nullptr,
        1
    );

    if (result != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create streaming task for client %d", client_id);
        httpd_req_async_handler_complete(async_req);
        free(ctx);
        AudioBuffer::unregister_client(client_id);
        clients[client_id].is_active = false;
        clients[client_id].socket_fd = -1;
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "24-bit stream handler returning, client %d now served by async task", client_id);
    return ESP_OK;
}

// --- Status page handler ---

static void format_uptime(uint32_t seconds, char *buf, size_t len)
{
    uint32_t d = seconds / 86400;
    uint32_t h = (seconds % 86400) / 3600;
    uint32_t m = (seconds % 3600) / 60;
    uint32_t s = seconds % 60;
    if (d > 0)
        snprintf(buf, len, "%ud %uh %um", d, h, m);
    else if (h > 0)
        snprintf(buf, len, "%uh %um %us", h, m, s);
    else
        snprintf(buf, len, "%um %us", m, s);
}

static esp_err_t send_status_json(httpd_req_t *req, const char *ip, uint32_t sr,
    float buf_fill, uint64_t frames, uint32_t underruns, uint32_t overruns,
    bool clipping, bool streaming, uint32_t cpu0, uint32_t cpu1,
    uint32_t heap_free, uint32_t heap_min, int8_t rssi, bool wifi,
    uint8_t num_clients, uint32_t uptime, bool mqtt_enabled, bool mqtt_connected,
    const char *mqtt_broker, const char *mqtt_state)
{
    httpd_resp_set_type(req, "application/json");
    add_cors_headers(req);

    char stream_url[96];
    build_stream_url(ip, stream_url, sizeof(stream_url));

    char json[768];
    int len = snprintf(json, sizeof(json),
        "{\"audio\":{\"sample_rate\":%u,\"bit_depth\":24,\"channels\":2,"
        "\"buffer_fill_pct\":%.1f,\"total_frames\":%llu,"
        "\"underrun_count\":%u,\"overrun_count\":%u,"
        "\"clipping\":%s,\"streaming\":%s},"
        "\"system\":{\"uptime_seconds\":%u,"
        "\"cpu_core0_pct\":%u,\"cpu_core1_pct\":%u,"
        "\"heap_free_bytes\":%u,\"heap_min_free_bytes\":%u},"
        "\"mqtt\":{\"enabled\":%s,\"connected\":%s,\"broker\":\"%s\",\"last_state\":\"%s\"},"
        "\"network\":{\"wifi_connected\":%s,\"rssi_dbm\":%d,"
        "\"ip_address\":\"%s\",\"active_clients\":%u,"
        "\"stream_url\":\"%s\"}}",
        sr, buf_fill, frames, underruns, overruns,
        clipping ? "true" : "false", streaming ? "true" : "false",
        uptime, cpu0, cpu1, heap_free, heap_min,
        mqtt_enabled ? "true" : "false", mqtt_connected ? "true" : "false", mqtt_broker, mqtt_state,
        wifi ? "true" : "false", rssi, ip, num_clients, stream_url);

    httpd_resp_send(req, json, len);
    return ESP_OK;
}

static esp_err_t send_status_html(httpd_req_t *req, const char *ip, uint32_t sr,
    float buf_fill, uint64_t frames, uint32_t underruns, uint32_t overruns,
    bool clipping, bool streaming, uint32_t cpu0, uint32_t cpu1,
    uint32_t heap_free, uint32_t heap_min, int8_t rssi, bool wifi,
    uint8_t num_clients, uint32_t uptime, bool mqtt_enabled, bool mqtt_connected,
    const char *mqtt_broker, const char *mqtt_state)
{
    httpd_resp_set_type(req, "text/html");
    add_cors_headers(req);

    char stream_url[96];
    build_stream_url(ip, stream_url, sizeof(stream_url));

    // Static HTML head + CSS
    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html><html><head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta http-equiv='refresh' content='5'>"
        "<title>ESP32 Audio Streamer</title>"
        "<style>"
        "*{box-sizing:border-box;margin:0;padding:0}"
        "body{font-family:system-ui,sans-serif;max-width:480px;margin:0 auto;padding:12px;background:#1a1a2e;color:#e0e0e0}"
        "h1{text-align:center;font-size:18px;margin-bottom:12px;color:#fff}"
        ".c{background:#16213e;border-radius:8px;padding:12px;margin-bottom:10px}"
        ".c h2{font-size:14px;color:#0f969c;margin-bottom:8px;border-bottom:1px solid #1a1a3e;padding-bottom:4px}"
        ".r{display:flex;justify-content:space-between;padding:3px 0;font-size:13px}"
        ".l{color:#888}.v{font-weight:600}"
        ".ok{color:#4caf50}.warn{color:#ff9800}.err{color:#f44336}"
        ".url{background:#0d1b2a;padding:8px;border-radius:4px;font-family:monospace;font-size:12px;word-break:break-all;margin-top:4px}"
        ".ft{text-align:center;font-size:11px;color:#555;margin-top:8px}.nav{display:flex;gap:8px;margin-bottom:12px}.btn{display:inline-block;padding:8px 10px;border-radius:6px;background:#0f969c;color:#fff;text-decoration:none;font-size:12px}"
        "</style></head><body>"
        "<h1>&#127925; ESP32 Audio Streamer</h1><div class='nav'><a class='btn' href='/mqtt-settings'>MQTT Settings</a><a class='btn' href='/stream'>Open Stream</a></div>");

    char buf[384];

    // Audio section
    const char *buf_class = (buf_fill > 50) ? "ok" : (buf_fill > 10) ? "warn" : "err";
    snprintf(buf, sizeof(buf),
        "<div class='c'><h2>&#127911; Audio Pipeline</h2>"
        "<div class='r'><span class='l'>Sample Rate</span><span class='v'>%u Hz</span></div>"
        "<div class='r'><span class='l'>Format</span><span class='v'>24-bit Stereo</span></div>"
        "<div class='r'><span class='l'>Buffer Fill</span><span class='v %s'>%.1f%%</span></div>"
        "<div class='r'><span class='l'>Frames Captured</span><span class='v'>%llu</span></div>"
        "<div class='r'><span class='l'>Underruns</span><span class='v'>%u</span></div>"
        "<div class='r'><span class='l'>Overruns</span><span class='v'>%u</span></div>"
        "<div class='r'><span class='l'>Clipping</span><span class='v %s'>%s</span></div>"
        "<div class='r'><span class='l'>Status</span><span class='v %s'>%s</span></div>"
        "</div>",
        sr, buf_class, buf_fill, frames, underruns, overruns,
        clipping ? "err" : "ok", clipping ? "CLIPPING" : "OK",
        streaming ? "ok" : "err", streaming ? "Streaming" : "Stopped");
    httpd_resp_sendstr_chunk(req, buf);

    // Network section
    const char *rssi_class = (rssi > -60) ? "ok" : (rssi > -75) ? "warn" : "err";
    snprintf(buf, sizeof(buf),
        "<div class='c'><h2>&#128225; Network</h2>"
        "<div class='r'><span class='l'>WiFi</span><span class='v %s'>%s</span></div>"
        "<div class='r'><span class='l'>RSSI</span><span class='v %s'>%d dBm</span></div>"
        "<div class='r'><span class='l'>IP Address</span><span class='v'>%s</span></div>"
        "<div class='r'><span class='l'>Clients</span><span class='v'>%u / 3</span></div>"
        "<div class='url'>%s</div>"
        "</div>",
        wifi ? "ok" : "err", wifi ? "Connected" : "Disconnected",
        rssi_class, rssi, ip, num_clients, stream_url);
    httpd_resp_sendstr_chunk(req, buf);

    snprintf(buf, sizeof(buf),
        "<div class='c'><h2>MQTT</h2>"
        "<div class='r'><span class='l'>Enabled</span><span class='v %s'>%s</span></div>"
        "<div class='r'><span class='l'>Connected</span><span class='v %s'>%s</span></div>"
        "<div class='r'><span class='l'>Broker</span><span class='v'>%s</span></div>"
        "<div class='r'><span class='l'>Last State</span><span class='v'>%s</span></div>"
        "</div>",
        mqtt_enabled ? "ok" : "warn", mqtt_enabled ? "Yes" : "No",
        mqtt_connected ? "ok" : "warn", mqtt_connected ? "Connected" : "Disconnected",
        mqtt_broker, mqtt_state);
    httpd_resp_sendstr_chunk(req, buf);

    // System section
    char uptime_str[32];
    format_uptime(uptime, uptime_str, sizeof(uptime_str));
    const char *cpu0_class = (cpu0 < 50) ? "ok" : (cpu0 < 80) ? "warn" : "err";
    const char *cpu1_class = (cpu1 < 60) ? "ok" : (cpu1 < 85) ? "warn" : "err";
    const char *heap_class = (heap_min > 30000) ? "ok" : (heap_min > 10000) ? "warn" : "err";
    snprintf(buf, sizeof(buf),
        "<div class='c'><h2>&#128187; System</h2>"
        "<div class='r'><span class='l'>Uptime</span><span class='v'>%s</span></div>"
        "<div class='r'><span class='l'>CPU Core 0 (Audio)</span><span class='v %s'>%u%%</span></div>"
        "<div class='r'><span class='l'>CPU Core 1 (Network)</span><span class='v %s'>%u%%</span></div>"
        "<div class='r'><span class='l'>Free Heap</span><span class='v'>%u KB</span></div>"
        "<div class='r'><span class='l'>Min Free Heap</span><span class='v %s'>%u KB</span></div>"
        "</div>",
        uptime_str, cpu0_class, cpu0, cpu1_class, cpu1,
        heap_free / 1024, heap_class, heap_min / 1024);
    httpd_resp_sendstr_chunk(req, buf);

    // Footer
    httpd_resp_sendstr_chunk(req,
        "<p class='ft'>Auto-refreshes every 5 seconds</p>"
        "</body></html>");

    httpd_resp_sendstr_chunk(req, nullptr);
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    // Gather all metrics
    char ip[16] = "0.0.0.0";
    WiFiManager::get_ip_address(ip, sizeof(ip));
    uint32_t sr = I2SMaster::get_sample_rate();
    float buf_fill = AudioBuffer::get_fill_percentage();
    uint64_t frames = AudioCapture::get_total_frames();
    uint32_t underruns = AudioCapture::get_underrun_count();
    uint32_t overruns = AudioBuffer::get_overrun_count();
    bool clipping = AudioCapture::is_clipping();
    bool streaming = AudioCapture::is_running();
    uint32_t cpu0 = TaskManager::get_cpu_usage_core0();
    uint32_t cpu1 = TaskManager::get_cpu_usage_core1();
    uint32_t heap_free = esp_get_free_heap_size();
    uint32_t heap_min = esp_get_minimum_free_heap_size();
    int8_t rssi = WiFiManager::get_rssi();
    bool wifi = WiFiManager::is_connected();
    uint8_t num_clients = HTTPServer::get_active_client_count();
    uint32_t uptime = (uint32_t)(esp_timer_get_time() / 1000000);
    DeviceConfig config;
    load_config_or_defaults(&config);
    bool mqtt_enabled = config.mqtt_enabled;
    const char *mqtt_broker = strlen(config.mqtt_broker) > 0 ? config.mqtt_broker : "-";
    bool mqtt_connected = MQTTClient::is_connected();
    const char *mqtt_state = MQTTClient::get_last_state();

    // Content negotiation
    char accept[128] = {0};
    httpd_req_get_hdr_value_str(req, "Accept", accept, sizeof(accept));

    if (strstr(accept, "application/json") != nullptr) {
        return send_status_json(req, ip, sr, buf_fill, frames, underruns, overruns,
            clipping, streaming, cpu0, cpu1, heap_free, heap_min, rssi, wifi,
            num_clients, uptime, mqtt_enabled, mqtt_connected, mqtt_broker, mqtt_state);
    }
    return send_status_html(req, ip, sr, buf_fill, frames, underruns, overruns,
        clipping, streaming, cpu0, cpu1, heap_free, heap_min, rssi, wifi,
        num_clients, uptime, mqtt_enabled, mqtt_connected, mqtt_broker, mqtt_state);
}

bool HTTPServer::init(uint16_t port, uint32_t sample_rate)
{
    if (server != nullptr)
    {
        ESP_LOGW(TAG, "HTTP server already running");
        return true;
    }

    ESP_LOGI(TAG, "Starting HTTP server on port %d", port);

    current_sample_rate = sample_rate;
    current_http_port = port;
    init_client_slots();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_open_sockets = 4; // 3 streaming + 1 for status/config
    config.max_uri_handlers = 16;
    config.lru_purge_enable = true;
    config.stack_size = 16384;  // Increased for larger audio chunks
    config.send_wait_timeout = 5;  // Allow time for WiFi congestion
    config.recv_wait_timeout = 5;
    config.task_priority = 6;  // Higher priority for streaming
    config.core_id = 1;  // Network core

    if (httpd_start(&server, &config) != ESP_OK)
    {
        ErrorHandler::log_error(ErrorType::HTTP_ERROR, "Failed to start HTTP server");
        return false;
    }

    // Register stream URI handler
    httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_handler,
        .user_ctx = nullptr};
    if (httpd_register_uri_handler(server, &stream_uri) != ESP_OK)
    {
        ErrorHandler::log_error(ErrorType::HTTP_ERROR, "Failed to register /stream URI");
        httpd_stop(server);
        server = nullptr;
        return false;
    }

    httpd_uri_t stream_wav_uri = {
        .uri = "/stream.wav",
        .method = HTTP_GET,
        .handler = stream_handler,
        .user_ctx = nullptr};
    if (httpd_register_uri_handler(server, &stream_wav_uri) != ESP_OK)
    {
        ErrorHandler::log_error(ErrorType::HTTP_ERROR, "Failed to register /stream.wav URI");
        httpd_stop(server);
        server = nullptr;
        return false;
    }

    // Register 24-bit stream URI handler
    httpd_uri_t stream_24bit_uri = {
        .uri = "/stream24.wav",
        .method = HTTP_GET,
        .handler = stream_24bit_handler,
        .user_ctx = nullptr};
    if (httpd_register_uri_handler(server, &stream_24bit_uri) != ESP_OK)
    {
        ErrorHandler::log_error(ErrorType::HTTP_ERROR, "Failed to register /stream24.wav URI");
        httpd_stop(server);
        server = nullptr;
        return false;
    }

    // Register status URI handler
    httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = nullptr};
    if (httpd_register_uri_handler(server, &status_uri) != ESP_OK)
    {
        ErrorHandler::log_error(ErrorType::HTTP_ERROR, "Failed to register /status URI");
        httpd_stop(server);
        server = nullptr;
        return false;
    }

    httpd_uri_t mqtt_settings_get_uri = {
        .uri = "/mqtt-settings",
        .method = HTTP_GET,
        .handler = mqtt_settings_get_handler,
        .user_ctx = nullptr};
    if (httpd_register_uri_handler(server, &mqtt_settings_get_uri) != ESP_OK)
    {
        ErrorHandler::log_error(ErrorType::HTTP_ERROR, "Failed to register /mqtt-settings GET URI");
        httpd_stop(server);
        server = nullptr;
        return false;
    }

    httpd_uri_t mqtt_settings_post_uri = {
        .uri = "/mqtt-settings",
        .method = HTTP_POST,
        .handler = mqtt_settings_post_handler,
        .user_ctx = nullptr};
    if (httpd_register_uri_handler(server, &mqtt_settings_post_uri) != ESP_OK)
    {
        ErrorHandler::log_error(ErrorType::HTTP_ERROR, "Failed to register /mqtt-settings POST URI");
        httpd_stop(server);
        server = nullptr;
        return false;
    }

    httpd_uri_t mqtt_settings_test_uri = {
        .uri = "/mqtt-settings/test",
        .method = HTTP_POST,
        .handler = mqtt_settings_test_handler,
        .user_ctx = nullptr};
    if (httpd_register_uri_handler(server, &mqtt_settings_test_uri) != ESP_OK)
    {
        ErrorHandler::log_error(ErrorType::HTTP_ERROR, "Failed to register /mqtt-settings/test URI");
        httpd_stop(server);
        server = nullptr;
        return false;
    }

    httpd_uri_t mqtt_settings_reset_uri = {
        .uri = "/mqtt-settings/reset",
        .method = HTTP_POST,
        .handler = mqtt_settings_reset_handler,
        .user_ctx = nullptr};
    if (httpd_register_uri_handler(server, &mqtt_settings_reset_uri) != ESP_OK)
    {
        ErrorHandler::log_error(ErrorType::HTTP_ERROR, "Failed to register /mqtt-settings/reset URI");
        httpd_stop(server);
        server = nullptr;
        return false;
    }

    httpd_uri_t audio_level_uri = {
        .uri = "/api/audio-level",
        .method = HTTP_GET,
        .handler = audio_level_handler,
        .user_ctx = nullptr};
    if (httpd_register_uri_handler(server, &audio_level_uri) != ESP_OK)
    {
        ErrorHandler::log_error(ErrorType::HTTP_ERROR, "Failed to register /api/audio-level URI");
        httpd_stop(server);
        server = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "HTTP server started successfully");
    ESP_LOGI(TAG, "Stream endpoint: http://[ip]:%d/stream", port);

    return true;
}

bool HTTPServer::stop()
{
    if (server == nullptr)
    {
        return true;
    }

    ESP_LOGI(TAG, "Stopping HTTP server");

    // Disconnect all active clients
    for (int i = 0; i < ClientConnection::MAX_CLIENTS; i++)
    {
        if (clients[i].is_active)
        {
            clients[i].is_active = false;
            AudioBuffer::unregister_client(i);
        }
    }

    httpd_stop(server);
    server = nullptr;

    ESP_LOGI(TAG, "HTTP server stopped");
    return true;
}

uint8_t HTTPServer::get_active_client_count()
{
    uint8_t count = 0;
    for (int i = 0; i < ClientConnection::MAX_CLIENTS; i++)
    {
        if (clients[i].is_active)
        {
            count++;
        }
    }
    return count;
}

httpd_handle_t HTTPServer::get_server_handle()
{
    return server;
}
