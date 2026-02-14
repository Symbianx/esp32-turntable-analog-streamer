#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <cstdint>
#include "esp_http_server.h"

class HTTPServer {
public:
    // Initialize and start HTTP server
    static bool init(uint16_t port, uint32_t sample_rate);
    
    // Stop HTTP server
    static bool stop();
    
    // Get number of active streaming clients
    static uint8_t get_active_client_count();
    
    // Get server handle for registering additional routes (Phase 4)
    static httpd_handle_t get_server_handle();
};

#endif // HTTP_SERVER_H
