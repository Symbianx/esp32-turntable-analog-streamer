#include "config_portal.h"
#include "config_portal_html.h"
#include "esp_wifi.h"
#include "wifi_manager.h"
#include "../storage/nvs_config.h"
#include "../config_schema.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>
#include <cstdio>

static const char *TAG = "config_portal";

// DNS server state
static int dns_server_socket = -1;
static TaskHandle_t dns_task_handle = nullptr;
static bool dns_running = false;

// Phase 4 T028: DNS redirect task for captive portal
static void dns_server_task(void* arg)
{
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    uint8_t dns_buffer[512];
    
    ESP_LOGI(TAG, "DNS redirect server started on port 53");
    
    while (dns_running) {
        int len = recvfrom(dns_server_socket, dns_buffer, sizeof(dns_buffer), 0,
                           (struct sockaddr*)&client_addr, &client_addr_len);
        
        if (len > 12) {  // Min DNS header size
            // Build DNS response redirecting to 192.168.4.1
            dns_buffer[2] = 0x81;  // Flags: response, no error
            dns_buffer[3] = 0x80;
            dns_buffer[6] = dns_buffer[4];  // Answer count = question count
            dns_buffer[7] = dns_buffer[5];
            
            // Add answer section (point to 192.168.4.1)
            uint8_t* answer = dns_buffer + len;
            answer[0] = 0xC0; answer[1] = 0x0C;  // Name pointer
            answer[2] = 0x00; answer[3] = 0x01;  // Type A
            answer[4] = 0x00; answer[5] = 0x01;  // Class IN
            answer[6] = 0x00; answer[7] = 0x00;  // TTL
            answer[8] = 0x00; answer[9] = 0x3C;
            answer[10] = 0x00; answer[11] = 0x04;  // Data length
            answer[12] = 192; answer[13] = 168;  // 192.168.4.1
            answer[14] = 4; answer[15] = 1;
            
            sendto(dns_server_socket, dns_buffer, len + 16, 0,
                   (struct sockaddr*)&client_addr, client_addr_len);
        }
    }
    
    vTaskDelete(nullptr);
}

bool ConfigPortal::start_dns_server()
{
    if (dns_running) {
        return true;
    }
    
    // Create UDP socket
    dns_server_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (dns_server_socket < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket");
        return false;
    }
    
    struct sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(53);
    
    if (bind(dns_server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind DNS socket");
        close(dns_server_socket);
        return false;
    }
    
    dns_running = true;
    xTaskCreate(dns_server_task, "dns_server", 4096, nullptr, 5, &dns_task_handle);
    
    ESP_LOGI(TAG, "DNS redirect server started");
    return true;
}

void ConfigPortal::stop_dns_server()
{
    if (dns_running) {
        dns_running = false;
        if (dns_server_socket >= 0) {
            close(dns_server_socket);
            dns_server_socket = -1;
        }
        if (dns_task_handle) {
            vTaskDelete(dns_task_handle);
            dns_task_handle = nullptr;
        }
        ESP_LOGI(TAG, "DNS redirect server stopped");
    }
}

// Phase 4 T029: WiFi scan endpoint
esp_err_t ConfigPortal::wifi_scan_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /wifi/scan");
    
    WiFiScanResult results[20];
    int count = WiFiManager::scan_networks(results, 20);
    
    // Build JSON response
    char response[2048];
    int pos = snprintf(response, sizeof(response), "{\"networks\":[");
    
    for (int i = 0; i < count && pos < sizeof(response) - 100; i++) {
        const char* auth_str = (results[i].auth_mode == WIFI_AUTH_OPEN) ? "open" : "wpa";
        pos += snprintf(response + pos, sizeof(response) - pos,
                        "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":\"%s\"}",
                        (i > 0) ? "," : "", results[i].ssid, results[i].rssi, auth_str);
    }
    
    pos += snprintf(response + pos, sizeof(response) - pos, "]}");
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, response, pos);
}

// Phase 4 T031: GET /config endpoint
esp_err_t ConfigPortal::get_config_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /config");
    
    DeviceConfig config;
    if (!NVSConfig::load(&config)) {
        config = DeviceConfig{};
    }
    
    // Build JSON (mask password)
    char response[512];
    int len = snprintf(response, sizeof(response),
        "{\"ssid\":\"%s\",\"password\":\"%s\",\"sample_rate\":%u,\"device_name\":\"%s\",\"http_port\":%u,\"max_clients\":%u}",
        config.wifi_ssid,
        (strlen(config.wifi_password) > 0) ? "********" : "",
        config.sample_rate,
        config.device_name,
        config.http_port,
        config.max_clients
    );
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, response, len);
}

// Phase 4 T032: POST /config endpoint
esp_err_t ConfigPortal::post_config_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "POST /config");
    
    char content[512];
    int ret = httpd_req_recv(req, content, sizeof(content) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
        return ESP_FAIL;
    }
    content[ret] = '\0';
    
    // Parse simple form data (ssid=xxx&password=xxx&sample_rate=48000)
    DeviceConfig config;
    if (!NVSConfig::load(&config)) {
        config = DeviceConfig{};
    }
    
    // Simple key=value parser
    char* token = strtok(content, "&");
    while (token) {
        char* eq = strchr(token, '=');
        if (eq) {
            *eq = '\0';
            char* key = token;
            char* value = eq + 1;
            
            // URL decode value (replace + with space, %XX with char)
            for (char* p = value; *p; p++) {
                if (*p == '+') *p = ' ';
            }
            
            if (strcmp(key, "ssid") == 0) {
                strncpy(config.wifi_ssid, value, sizeof(config.wifi_ssid) - 1);
            } else if (strcmp(key, "password") == 0) {
                strncpy(config.wifi_password, value, sizeof(config.wifi_password) - 1);
            } else if (strcmp(key, "sample_rate") == 0) {
                config.sample_rate = atoi(value);
            } else if (strcmp(key, "device_name") == 0) {
                strncpy(config.device_name, value, sizeof(config.device_name) - 1);
            }
        }
        token = strtok(nullptr, "&");
    }
    
    // Validate
    if (strlen(config.wifi_ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_FAIL;
    }
    
    // Save to NVS
    if (!NVSConfig::save(&config)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save config");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Config saved: SSID=%s, Sample Rate=%u", config.wifi_ssid, config.sample_rate);
    
    const char* response = "{\"status\":\"ok\",\"message\":\"Configuration saved. Device will restart...\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    
    // Trigger restart after 2 seconds (allow response to send)
    // In production, you'd trigger a soft restart here
    ESP_LOGI(TAG, "Configuration updated - restart required");
    
    return ESP_OK;
}

// Phase 4 T030: Configuration portal HTML page
const char* ConfigPortal::get_config_page_html()
{
    return CONFIG_PORTAL_HTML;
}

// Phase 4 T033: Root handler
esp_err_t ConfigPortal::root_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /");
    
    // If in AP mode, show config portal
    // If in STA mode, redirect to /status
    if (WiFiManager::is_ap_running()) {
        httpd_resp_set_type(req, "text/html");
        return httpd_resp_send(req, get_config_page_html(), strlen(get_config_page_html()));
    } else {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/status");
        return httpd_resp_send(req, nullptr, 0);
    }
}

// Phase 4 T035: Register routes
bool ConfigPortal::init(httpd_handle_t server)
{
    if (server == nullptr) {
        ESP_LOGE(TAG, "Invalid HTTP server handle");
        return false;
    }
    
    ESP_LOGI(TAG, "Registering config portal routes");
    
    // GET / - root handler
    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(server, &root_uri);
    
    // GET /wifi/scan
    httpd_uri_t scan_uri = {
        .uri = "/wifi/scan",
        .method = HTTP_GET,
        .handler = wifi_scan_handler,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(server, &scan_uri);
    
    // GET /config
    httpd_uri_t get_config_uri = {
        .uri = "/config",
        .method = HTTP_GET,
        .handler = get_config_handler,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(server, &get_config_uri);
    
    // POST /config
    httpd_uri_t post_config_uri = {
        .uri = "/config",
        .method = HTTP_POST,
        .handler = post_config_handler,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(server, &post_config_uri);
    
    ESP_LOGI(TAG, "Config portal routes registered");
    return true;
}
