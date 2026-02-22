#include "wifi_manager.h"
#include "../system/error_handler.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "freertos/event_groups.h"
#include "mdns.h"
#include "esp_mac.h"
#include <cstring>

static const char *TAG = "wifi_manager";

// Event group bits
constexpr int WIFI_CONNECTED_BIT = BIT0;
constexpr int WIFI_FAIL_BIT = BIT1;
constexpr int WIFI_AP_STARTED_BIT = BIT2;

static EventGroupHandle_t wifi_event_group;
static int retry_count = 0;
constexpr int MAX_RETRY = 5;

static bool wifi_initialized = false;
static wifi_mode_t current_mode = WIFI_MODE_NULL;
static esp_netif_t* sta_netif = nullptr;
static esp_netif_t* ap_netif = nullptr;
static bool mdns_initialized = false;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA started, connecting...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (retry_count < MAX_RETRY) {
            esp_wifi_connect();
            retry_count++;
            ESP_LOGI(TAG, "Retry connecting to AP (attempt %d/%d)", retry_count, MAX_RETRY);
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "Failed to connect to AP after %d attempts", MAX_RETRY);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "Station " MACSTR " joined AP", MAC2STR(event->mac));
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "Station " MACSTR " left AP", MAC2STR(event->mac));
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "WiFi AP started");
        xEventGroupSetBits(wifi_event_group, WIFI_AP_STARTED_BIT);
    }
}

bool WiFiManager::init()
{
    if (wifi_initialized) {
        return true;
    }

    ESP_LOGI(TAG, "Initializing WiFi");
    
    // Initialize TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());
    
    // Create default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Create WiFi event group
    wifi_event_group = xEventGroupCreate();
    
    // Initialize WiFi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, 
                                                &wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, 
                                                &wifi_event_handler, nullptr));
    
    wifi_initialized = true;
    ESP_LOGI(TAG, "WiFi initialized");
    
    return true;
}

bool WiFiManager::connect_sta(const char* ssid, const char* password)
{
    if (!wifi_initialized) {
        ErrorHandler::log_error(ErrorType::WIFI_ERROR, "WiFi not initialized");
        return false;
    }
    
    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", ssid);
    
    // Create default STA interface if needed
    if (sta_netif == nullptr) {
        sta_netif = esp_netif_create_default_wifi_sta();
    }
    
    // Set WiFi mode
    wifi_mode_t mode = (current_mode == WIFI_MODE_AP) ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    ESP_ERROR_CHECK(esp_wifi_set_mode(mode));
    current_mode = mode;
    
    // Configure WiFi
    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    
    // Start WiFi
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Wait for connection or failure
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdTRUE,
                                            pdFALSE,
                                            pdMS_TO_TICKS(30000));  // 30 second timeout
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP: %s", ssid);
        return true;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to AP: %s", ssid);
        ErrorHandler::log_error(ErrorType::WIFI_ERROR, "STA connection failed");
        return false;
    } else {
        ESP_LOGE(TAG, "Connection timeout");
        ErrorHandler::log_error(ErrorType::WIFI_ERROR, "STA connection timeout");
        return false;
    }
}

bool WiFiManager::disconnect()
{
    if (!wifi_initialized) {
        return true;  // Already disconnected
    }
    
    ESP_LOGI(TAG, "Disconnecting WiFi");
    
    esp_wifi_disconnect();
    esp_wifi_stop();
    
    return true;
}

bool WiFiManager::is_connected()
{
    if (!wifi_initialized) {
        return false;
    }
    
    wifi_ap_record_t ap_info;
    return (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
}

int8_t WiFiManager::get_rssi()
{
    if (!is_connected()) {
        return -100;  // No connection
    }
    
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    
    return -100;
}

bool WiFiManager::get_ip_address(char* ip_str, size_t max_len)
{
    if (!is_connected()) {
        return false;
    }
    
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == nullptr) {
        return false;
    }
    
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(ip_str, max_len, IPSTR, IP2STR(&ip_info.ip));
        return true;
    }
    
    return false;
}

void WiFiManager::deinit()
{
    if (wifi_initialized) {
        stop_mdns();
        disconnect();
        stop_ap();
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler);
        esp_wifi_deinit();
        vEventGroupDelete(wifi_event_group);
        wifi_initialized = false;
        ESP_LOGI(TAG, "WiFi deinitialized");
    }
}

// Phase 4: SoftAP mode implementation (T026)
bool WiFiManager::start_ap(const char* ssid, const char* password)
{
    if (!wifi_initialized) {
        ErrorHandler::log_error(ErrorType::WIFI_ERROR, "WiFi not initialized");
        return false;
    }
    
    ESP_LOGI(TAG, "Starting SoftAP: %s", ssid);
    
    // Create default AP interface if needed
    if (ap_netif == nullptr) {
        ap_netif = esp_netif_create_default_wifi_ap();
    }
    
    // Set WiFi mode
    wifi_mode_t mode = (current_mode == WIFI_MODE_STA) ? WIFI_MODE_APSTA : WIFI_MODE_AP;
    ESP_ERROR_CHECK(esp_wifi_set_mode(mode));
    current_mode = mode;
    
    // Configure AP
    wifi_config_t ap_config = {};
    strncpy((char*)ap_config.ap.ssid, ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.beacon_interval = 100;
    
    if (password != nullptr && strlen(password) >= 8) {
        strncpy((char*)ap_config.ap.password, password, sizeof(ap_config.ap.password) - 1);
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }
    
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Wait for AP to start
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                            WIFI_AP_STARTED_BIT,
                                            pdTRUE,
                                            pdFALSE,
                                            pdMS_TO_TICKS(5000));
    
    if (bits & WIFI_AP_STARTED_BIT) {
        ESP_LOGI(TAG, "SoftAP started: %s (IP: 192.168.4.1)", ssid);
        return true;
    }
    
    ESP_LOGE(TAG, "Failed to start SoftAP");
    return false;
}

bool WiFiManager::stop_ap()
{
    if (current_mode == WIFI_MODE_AP || current_mode == WIFI_MODE_APSTA) {
        ESP_LOGI(TAG, "Stopping SoftAP");
        
        if (current_mode == WIFI_MODE_APSTA) {
            // Switch to STA only
            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
            current_mode = WIFI_MODE_STA;
        } else {
            esp_wifi_stop();
            current_mode = WIFI_MODE_NULL;
        }
        return true;
    }
    return false;
}

bool WiFiManager::is_ap_running()
{
    return (current_mode == WIFI_MODE_AP || current_mode == WIFI_MODE_APSTA);
}

// Phase 4: AP/STA combined mode (T027)
bool WiFiManager::start_ap_sta_mode(const char* ap_ssid, const char* sta_ssid, const char* sta_password)
{
    if (!wifi_initialized) {
        return false;
    }
    
    ESP_LOGI(TAG, "Starting AP/STA mode: AP=%s, STA=%s", ap_ssid, sta_ssid);
    
    // Start AP first
    if (!start_ap(ap_ssid, nullptr)) {
        return false;
    }
    
    // Then try to connect STA (non-blocking on failure)
    connect_sta(sta_ssid, sta_password);
    
    return true;
}

// Phase 4: WiFi scanning (T029)
int WiFiManager::scan_networks(WiFiScanResult* results, int max_results)
{
    if (!wifi_initialized) {
        return 0;
    }
    
    ESP_LOGI(TAG, "Scanning WiFi networks...");
    
    // If in AP-only mode, temporarily switch to APSTA to enable scanning
    wifi_mode_t original_mode = current_mode;
    bool mode_changed = false;
    
    if (current_mode == WIFI_MODE_AP) {
        ESP_LOGI(TAG, "Switching to APSTA mode for scanning");
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to switch to APSTA mode: %d", err);
            return 0;
        }
        current_mode = WIFI_MODE_APSTA;
        mode_changed = true;
        vTaskDelay(pdMS_TO_TICKS(100));  // Give WiFi time to switch modes
    }
    
    // Start scan
    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = false;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);  // Block until done
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan failed: %d", err);
        
        // Restore original mode if we changed it
        if (mode_changed) {
            esp_wifi_set_mode(original_mode);
            current_mode = original_mode;
        }
        return 0;
    }
    
    // Get scan results
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    
    if (ap_count == 0) {
        ESP_LOGW(TAG, "No WiFi networks found");
        
        // Restore original mode if we changed it
        if (mode_changed) {
            esp_wifi_set_mode(original_mode);
            current_mode = original_mode;
        }
        return 0;
    }
    
    wifi_ap_record_t* ap_records = (wifi_ap_record_t*)malloc(ap_count * sizeof(wifi_ap_record_t));
    if (ap_records == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate scan results buffer");
        
        // Restore original mode if we changed it
        if (mode_changed) {
            esp_wifi_set_mode(original_mode);
            current_mode = original_mode;
        }
        return 0;
    }
    
    esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    
    // Convert to our format
    int result_count = (ap_count < max_results) ? ap_count : max_results;
    for (int i = 0; i < result_count; i++) {
        strncpy(results[i].ssid, (char*)ap_records[i].ssid, sizeof(results[i].ssid) - 1);
        results[i].ssid[sizeof(results[i].ssid) - 1] = '\0';
        results[i].rssi = ap_records[i].rssi;
        results[i].auth_mode = ap_records[i].authmode;
    }
    
    free(ap_records);
    ESP_LOGI(TAG, "Found %d WiFi networks", result_count);
    
    // Restore original mode if we changed it
    if (mode_changed) {
        ESP_LOGI(TAG, "Restoring AP-only mode");
        esp_wifi_set_mode(original_mode);
        current_mode = original_mode;
    }
    
    return result_count;
}

// Phase 4: mDNS service (T034)
bool WiFiManager::start_mdns(const char* hostname, uint16_t http_port)
{
    if (mdns_initialized) {
        ESP_LOGW(TAG, "mDNS already initialized");
        return true;
    }
    
    // Initialize mDNS
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %d", err);
        return false;
    }
    
    // Set hostname
    err = mdns_hostname_set(hostname);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set mDNS hostname: %d", err);
        mdns_free();
        return false;
    }
    
    // Set instance name
    err = mdns_instance_name_set("ESP32 Audio Streamer");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set mDNS instance name: %d", err);
        mdns_free();
        return false;
    }
    
    // Add HTTP service
    err = mdns_service_add(nullptr, "_http", "_tcp", http_port, nullptr, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add mDNS HTTP service: %d", err);
        mdns_free();
        return false;
    }
    
    mdns_initialized = true;
    ESP_LOGI(TAG, "mDNS initialized: %s.local:%d", hostname, http_port);
    return true;
}

void WiFiManager::stop_mdns()
{
    if (mdns_initialized) {
        mdns_free();
        mdns_initialized = false;
        ESP_LOGI(TAG, "mDNS stopped");
    }
}
