
#include "rgb_led.h"
#include <led_strip.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Onboard Neopixel (WS2812) on ESP32-S3-DevKitC-1 is GPIO 48
#define NEOPIXEL_GPIO 48

static led_strip_handle_t neopixel = nullptr;

static void set_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (neopixel)
    {
        led_strip_set_pixel(neopixel, 0, r, g, b);
        led_strip_refresh(neopixel);
    }
}

void RGBLed::init()
{
    // Configure for a single WS2812 (Neopixel)
    led_strip_config_t config = {
        .strip_gpio_num = NEOPIXEL_GPIO,
        .max_leds = 1,
        // .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags = {.invert_out = false}};
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
    };
    led_strip_new_rmt_device(&config, &rmt_cfg, &neopixel);
    set_color(0, 0, 0); // off
}

void RGBLed::indicate_error()
{
    const int flash_duration_ms = 3000;
    const int interval_ms = 300;
    int elapsed = 0;
    while (elapsed < flash_duration_ms)
    {
        set_color(32, 0, 0); // red
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
        elapsed += interval_ms;
        if (elapsed < flash_duration_ms)
        {
            set_color(0, 0, 0); // off
            vTaskDelay(pdMS_TO_TICKS(interval_ms));
            elapsed += interval_ms;
        }
    }
    set_color(32, 0, 0); // stay red
}

void RGBLed::indicate_success()
{
    set_color(0, 32, 0); // stay green
}

void RGBLed::indicate_progress()
{
    set_color(0, 0, 32); // stay blue
}

void RGBLed::step_nvs() { set_color(0, 0, 32); }         // Blue
void RGBLed::step_wifi() { set_color(0, 32, 32); }       // Cyan
void RGBLed::step_config_portal() { set_color(32, 32, 0); } // Yellow
void RGBLed::step_http_server() { set_color(32, 0, 32); }   // Magenta
void RGBLed::step_audio_buffer() { set_color(32, 16, 0); }  // Orange
void RGBLed::step_i2s() { set_color(16, 0, 32); }           // Purple
void RGBLed::step_audio_capture() { set_color(32, 32, 32); } // White