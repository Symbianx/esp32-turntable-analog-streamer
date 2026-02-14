#include "http_server.h"
#include "stream_handler.h"
#include "../audio/audio_buffer.h"
#include "../audio/audio_capture.h"
#include "../system/error_handler.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include <cstring>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <errno.h>

static const char *TAG = "http_server";

static httpd_handle_t server = nullptr;
static ClientConnection clients[ClientConnection::MAX_CLIENTS];
static uint32_t current_sample_rate = 48000;

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

// Stream handler for /stream endpoint
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
    {
        int fd = httpd_req_to_sockfd(req);
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    }

    // Build and send WAV header
    WavHeader wav_header;
    StreamHandler::build_wav_header(&wav_header, current_sample_rate);

    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Connection", "close");

    if (httpd_resp_send_chunk(req, (const char *)&wav_header, sizeof(WavHeader)) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to send WAV header to client %d", client_id);
        goto cleanup;
    }

    clients[client_id].bytes_sent += sizeof(WavHeader);

    // Stream audio data in chunks
    {
        uint8_t audio_chunk[4320]; // 720 frames × 6 bytes (15ms of audio)
        size_t bytes_read;
        uint32_t last_log_time = esp_timer_get_time() / 1000000;
        uint32_t period_bytes = 0;
        uint32_t empty_waits = 0;

        while (clients[client_id].is_active)
        {
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
        }
    } // End of streaming scope

cleanup:
    // Clean up client connection
    ESP_LOGI(TAG, "Client %d disconnecting (sent %llu bytes)",
             client_id, clients[client_id].bytes_sent);

    AudioBuffer::unregister_client(client_id);
    clients[client_id].is_active = false;
    clients[client_id].socket_fd = -1;

    // Send empty chunk to signal end of stream
    httpd_resp_send_chunk(req, nullptr, 0);

    return ESP_OK;
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

    // Register stream URI handler (root handler is registered by config_portal in Phase 4)
    httpd_uri_t stream_uri = {
        .uri = "/stream.wav",
        .method = HTTP_GET,
        .handler = stream_handler,
        .user_ctx = nullptr};
    httpd_register_uri_handler(server, &stream_uri);

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
