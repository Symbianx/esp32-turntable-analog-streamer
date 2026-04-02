# esp32-turntable-analog-streamer Development Guidelines

Auto-generated from all feature plans. Last updated: 2026-02-13

## Active Technologies
- C++17, ESP-IDF v5.x (Espressif IoT Development Framework) + ESP-IDF (WiFi, I²S, HTTP server, NVS), FreeRTOS (task management, queues), lwIP (TCP/IP stack) (001-pcm1808-http-streaming)
- NVS (Non-Volatile Storage) for WiFi credentials and configuration; no external database (001-pcm1808-http-streaming)
- C++17 (ESP-IDF framework, `std=gnu++17`) + ESP-IDF v5.x, ESP-MQTT component (`espressif/mqtt ^1.0.0`), FreeRTOS, LWIP TCP/IP stack (002-ha-mqtt-integration)
- NVS (Non-Volatile Storage) for MQTT credentials and audio threshold configuration (002-ha-mqtt-integration)
- C++17 / ESP-IDF 5.3.4 + `espressif/esp-dsp` (biquad IIR filters, SIMD-accelerated via Xtensa ae32/aes3 FPU), existing ESP-IDF stack (003-esp-dsp-equalization)
- NVS (Non-Volatile Storage) — extends existing `DeviceConfig` schema (003-esp-dsp-equalization)

- C++ (C++17), ESP-IDF v5.x (via PlatformIO espressif32 platform) + ESP-IDF built-in components: `driver/i2s_std`, `esp_http_server`, `esp_wifi`, `nvs_flash`, `mdns`, `lwip`, `esp_timer`, `esp_system` (001-pcm1808-http-streaming)

## Project Structure

```text
src/
tests/
```

## Commands

# Add commands for C++ (C++17), ESP-IDF v5.x (via PlatformIO espressif32 platform)

## Code Style

C++ (C++17), ESP-IDF v5.x (via PlatformIO espressif32 platform): Follow standard conventions

## Recent Changes
- 003-esp-dsp-equalization: Added C++17 / ESP-IDF 5.3.4 + `espressif/esp-dsp` (biquad IIR filters, SIMD-accelerated via Xtensa ae32/aes3 FPU), existing ESP-IDF stack
- 002-ha-mqtt-integration: Added C++17 (ESP-IDF framework, `std=gnu++17`) + ESP-IDF v5.x, ESP-MQTT component (`espressif/mqtt ^1.0.0`), FreeRTOS, LWIP TCP/IP stack
- 001-pcm1808-http-streaming: Added C++17, ESP-IDF v5.x (Espressif IoT Development Framework) + ESP-IDF (WiFi, I²S, HTTP server, NVS), FreeRTOS (task management, queues), lwIP (TCP/IP stack)


<!-- MANUAL ADDITIONS START -->

Whenever you need references about libraries, APIs, SDK or frameworks, use context7.

<!-- MANUAL ADDITIONS END -->
