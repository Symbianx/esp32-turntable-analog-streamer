#ifndef RGB_LED_H
#define RGB_LED_H

#include <stdint.h>


class RGBLed {
public:
    static void init();
    static void indicate_success();
    static void indicate_progress();
    static void indicate_error();

    // Step indicators
    static void step_nvs();
    static void step_wifi();
    static void step_config_portal();
    static void step_http_server();
    static void step_audio_buffer();
    static void step_i2s();
    static void step_audio_capture();
};

#endif // RGB_LED_H
