#include "http_server.h"
#include "stream_handler.h"
#include "../audio/audio_buffer.h"
#include "../audio/audio_capture.h"
#include "../audio/i2s_master.h"
#include "../system/error_handler.h"
#include "../system/task_manager.h"
#include "../network/wifi_manager.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include <cstring>
#include <cstdio>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <errno.h>

static const char *TAG = "http_server";

static httpd_handle_t server = nullptr;
static ClientConnection clients[ClientConnection::MAX_CLIENTS];
static uint32_t current_sample_rate = 48000;

// Context for async streaming task
struct StreamTaskContext {
    httpd_req_t *req;
    int client_id;
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

// Async streaming task - runs independently from HTTP worker thread
static void stream_task(void *arg)
{
    StreamTaskContext *ctx = (StreamTaskContext *)arg;
    httpd_req_t *req = ctx->req;
    int client_id = ctx->client_id;
    
    ESP_LOGI(TAG, "Stream task started for client %d", client_id);

    // Chunk aligned to DMA production unit (240 frames × 6 bytes = 5ms at 48kHz)
    uint8_t audio_chunk[1440];
    size_t bytes_read;
    uint32_t last_log_time = esp_timer_get_time() / 1000000;
    uint32_t period_bytes = 0;
    uint32_t empty_waits = 0;

    // Pacing: match send rate to audio production rate
    uint32_t byte_rate = current_sample_rate * 6; // sample_rate × 2ch × 3bytes

    while (clients[client_id].is_active)
    {
        TickType_t iter_start = xTaskGetTickCount();

        // Read from ring buffer
        if (!AudioBuffer::read(client_id, audio_chunk, sizeof(audio_chunk), &bytes_read))
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

        // Send audio chunk
        if (httpd_resp_send_chunk(req, (const char *)audio_chunk, bytes_read) != ESP_OK)
        {
            ESP_LOGI(TAG, "Client %d disconnected", client_id);
            break;
        }

        clients[client_id].bytes_sent += bytes_read;
        period_bytes += bytes_read;

        // Log throughput every 10 seconds
        uint32_t now = esp_timer_get_time() / 1000000;
        uint32_t elapsed = now - last_log_time;
        if (elapsed >= 10)
        {
            uint32_t kbps = (period_bytes * 8) / (elapsed * 1000);
            ESP_LOGI(TAG, "Client %d: %u kbps (target: 2304), total %llu bytes",
                     client_id, kbps, clients[client_id].bytes_sent);
            period_bytes = 0;
            last_log_time = now;
        }

        // Check if audio capture is still running
        if (!AudioCapture::is_running())
        {
            ESP_LOGW(TAG, "Audio capture stopped, ending stream");
            break;
        }

        // // Pace sends to match audio production rate.
        // // Calculate how long this chunk represents in real-time, then wait
        // // the remaining duration after subtracting time already spent on
        // // the read+send. This prevents burst-drain-starve buffer oscillation.
        // TickType_t target_ticks = pdMS_TO_TICKS((bytes_read * 1000) / byte_rate);
        // if (target_ticks < 1) target_ticks = 1;
        // TickType_t elapsed_ticks = xTaskGetTickCount() - iter_start;
        // if (elapsed_ticks < target_ticks)
        // {
        //     vTaskDelay(target_ticks - elapsed_ticks);
        // }
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

// --- Status page handler ---

static void add_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

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
    uint8_t num_clients, uint32_t uptime)
{
    httpd_resp_set_type(req, "application/json");
    add_cors_headers(req);

    char json[768];
    int len = snprintf(json, sizeof(json),
        "{\"audio\":{\"sample_rate\":%u,\"bit_depth\":24,\"channels\":2,"
        "\"buffer_fill_pct\":%.1f,\"total_frames\":%llu,"
        "\"underrun_count\":%u,\"overrun_count\":%u,"
        "\"clipping\":%s,\"streaming\":%s},"
        "\"system\":{\"uptime_seconds\":%u,"
        "\"cpu_core0_pct\":%u,\"cpu_core1_pct\":%u,"
        "\"heap_free_bytes\":%u,\"heap_min_free_bytes\":%u},"
        "\"network\":{\"wifi_connected\":%s,\"rssi_dbm\":%d,"
        "\"ip_address\":\"%s\",\"active_clients\":%u,"
        "\"stream_url\":\"http://%s/stream.wav\"}}",
        sr, buf_fill, frames, underruns, overruns,
        clipping ? "true" : "false", streaming ? "true" : "false",
        uptime, cpu0, cpu1, heap_free, heap_min,
        wifi ? "true" : "false", rssi, ip, num_clients, ip);

    httpd_resp_send(req, json, len);
    return ESP_OK;
}

static esp_err_t send_status_html(httpd_req_t *req, const char *ip, uint32_t sr,
    float buf_fill, uint64_t frames, uint32_t underruns, uint32_t overruns,
    bool clipping, bool streaming, uint32_t cpu0, uint32_t cpu1,
    uint32_t heap_free, uint32_t heap_min, int8_t rssi, bool wifi,
    uint8_t num_clients, uint32_t uptime)
{
    httpd_resp_set_type(req, "text/html");
    add_cors_headers(req);

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
        ".ft{text-align:center;font-size:11px;color:#555;margin-top:8px}"
        "</style></head><body>"
        "<h1>&#127925; ESP32 Audio Streamer</h1>");

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
        "<div class='url'>http://%s/stream.wav</div>"
        "</div>",
        wifi ? "ok" : "err", wifi ? "Connected" : "Disconnected",
        rssi_class, rssi, ip, num_clients, ip);
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

    // Content negotiation
    char accept[128] = {0};
    httpd_req_get_hdr_value_str(req, "Accept", accept, sizeof(accept));

    if (strstr(accept, "application/json") != nullptr) {
        return send_status_json(req, ip, sr, buf_fill, frames, underruns, overruns,
            clipping, streaming, cpu0, cpu1, heap_free, heap_min, rssi, wifi,
            num_clients, uptime);
    }
    return send_status_html(req, ip, sr, buf_fill, frames, underruns, overruns,
        clipping, streaming, cpu0, cpu1, heap_free, heap_min, rssi, wifi,
        num_clients, uptime);
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
    init_client_slots();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_open_sockets = 4; // 3 streaming + 1 for status/config
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
        .uri = "/stream.wav",
        .method = HTTP_GET,
        .handler = stream_handler,
        .user_ctx = nullptr};
    httpd_register_uri_handler(server, &stream_uri);

    // Register status URI handler
    httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = nullptr};
    httpd_register_uri_handler(server, &status_uri);

    ESP_LOGI(TAG, "HTTP server started successfully");
    ESP_LOGI(TAG, "Stream endpoint: http://[ip]:%d/stream.wav", port);

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
