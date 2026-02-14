#ifndef CONFIG_PORTAL_H
#define CONFIG_PORTAL_H

#include "esp_http_server.h"

class ConfigPortal {
public:
    // Initialize config portal (register routes)
    static bool init(httpd_handle_t server);
    
    // DNS redirect server for captive portal detection (T028)
    static bool start_dns_server();
    static void stop_dns_server();
    
private:
    // HTTP handlers (T029-T033)
    static esp_err_t wifi_scan_handler(httpd_req_t *req);      // GET /wifi/scan
    static esp_err_t get_config_handler(httpd_req_t *req);     // GET /config
    static esp_err_t post_config_handler(httpd_req_t *req);    // POST /config
    static esp_err_t root_handler(httpd_req_t *req);           // GET /
    
    // HTML page generator (T030)
    static const char* get_config_page_html();
};

#endif // CONFIG_PORTAL_H
