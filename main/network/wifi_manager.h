#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <cstdint>
#include <cstddef>

// WiFi scan result structure
struct WiFiScanResult {
    char ssid[33];
    int8_t rssi;
    uint8_t auth_mode;  // 0=open, 1=WEP, 2=WPA-PSK, 3=WPA2-PSK, 4=WPA/WPA2-PSK
};

class WiFiManager {
public:
    // Initialize WiFi subsystem
    static bool init();
    
    // STA (Station) mode functions
    static bool connect_sta(const char* ssid, const char* password);
    static bool disconnect();
    static bool is_connected();
    static int8_t get_rssi();
    static bool get_ip_address(char* ip_str, size_t max_len);
    
    // AP (Access Point) mode functions - Phase 4
    static bool start_ap(const char* ssid, const char* password = nullptr);
    static bool stop_ap();
    static bool is_ap_running();
    
    // AP/STA combined mode
    static bool start_ap_sta_mode(const char* ap_ssid, const char* sta_ssid, const char* sta_password);
    
    // WiFi scanning
    static int scan_networks(WiFiScanResult* results, int max_results);
    
    // mDNS service
    static bool start_mdns(const char* hostname, uint16_t http_port);
    static void stop_mdns();
    
    // Deinitialize WiFi
    static void deinit();
};

#endif // WIFI_MANAGER_H
