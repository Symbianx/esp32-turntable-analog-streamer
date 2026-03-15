#ifndef MQTT_SERVICE_H
#define MQTT_SERVICE_H

#include <cstddef>
#include <cstdint>

#include "esp_event.h"
#include <mqtt_client.h>

class MQTTClient {
public:
    static bool init();
    static bool start();
    static bool stop();
    static bool publish_discovery();
    static bool publish_state(bool is_playing);
    static bool publish_attributes(const char* stream_url);
    static bool reconnect();
    static bool is_enabled();
    static bool is_connected();
    static const char* get_broker();
    static const char* get_last_state();
    static const char* get_last_error();

private:
    static void monitor_task(void* params);
    static void event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data);
    static void ip_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data);
    static void generate_device_id(char* buffer, size_t len);
    static void get_stream_url(char* buffer, size_t len);
};

#endif // MQTT_SERVICE_H