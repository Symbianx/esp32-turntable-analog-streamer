#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <cstdint>
#include <atomic>
#include "mqtt_client.h"

class MQTTClient {
public:
    // Initialize MQTT client with configuration from NVS
    static bool init();
    
    // Start MQTT connection and discovery publishing
    static bool start();
    
    // Stop MQTT client and cleanup
    static bool stop();
    
    // Publish Home Assistant auto-discovery configuration
    static bool publish_discovery();
    
    // Publish playback state ("playing" or "idle")
    static bool publish_state(bool is_playing);
    
    // Publish stream URL as JSON attributes
    static bool publish_attributes(const char* stream_url);
    
    // Reconnect to broker (used after config change)
    static bool reconnect();
    
    // Check if connected to broker
    static bool is_connected();
    
    // Get last error message
    static const char* get_last_error();
    
private:
    // Background task monitoring playback status
    static void monitor_task(void* params);
    
    // MQTT event handler
    static void event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data);
    
    // Generate unique device ID from MAC address
    static void generate_device_id(char* buffer, size_t len);
    
    // Construct stream URL from current IP
    static void get_stream_url(char* buffer, size_t len);
};

#endif // MQTT_CLIENT_H
