#include "i2s_master.h"
#include "../system/error_handler.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "i2s_master";

// GPIO pinout (from spec clarifications)
constexpr gpio_num_t MCLK_GPIO = GPIO_NUM_0;   // SCK → PCM1808 SCKI
constexpr gpio_num_t BCK_GPIO = GPIO_NUM_26;   // BCK → PCM1808 BCK
constexpr gpio_num_t WS_GPIO = GPIO_NUM_25;    // LRCK → PCM1808 LRCK
constexpr gpio_num_t DIN_GPIO = GPIO_NUM_27;   // DOUT ← PCM1808 DOUT

// DMA configuration (must be multiples of 3 for 24-bit)
constexpr uint32_t DMA_FRAME_NUM = 240;  // 240 frames per descriptor
constexpr uint32_t DMA_DESC_NUM = 6;     // 6 descriptors

static i2s_chan_handle_t rx_handle = nullptr;
static uint32_t current_sample_rate = 48000;

bool I2SMaster::init(uint32_t sample_rate)
{
    ESP_LOGI(TAG, "Initializing I²S master at %d Hz", sample_rate);
    
    current_sample_rate = sample_rate;
    
    // Create I²S RX channel
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_frame_num = DMA_FRAME_NUM;
    chan_cfg.dma_desc_num = DMA_DESC_NUM;
    chan_cfg.auto_clear = false;  // Don't clear DMA buffer on underflow
    
    esp_err_t err = i2s_new_channel(&chan_cfg, nullptr, &rx_handle);
    if (err != ESP_OK) {
        ErrorHandler::log_error(ErrorType::I2S_ERROR, "Failed to create I²S channel");
        return false;
    }
    
    // Configure I²S standard mode (Philips format, 24-bit)
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = MCLK_GPIO,
            .bclk = BCK_GPIO,
            .ws = WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    
    // Set MCLK multiple to 256 for 32-bit slot width
    // PCM1808 supports 256fs, 384fs, or 512fs
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    
    // Use APLL for better clock accuracy (critical for 44.1kHz family)
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
    
    err = i2s_channel_init_std_mode(rx_handle, &std_cfg);
    if (err != ESP_OK) {
        ErrorHandler::log_error(ErrorType::I2S_ERROR, "Failed to initialize I²S standard mode");
        i2s_del_channel(rx_handle);
        rx_handle = nullptr;
        return false;
    }
    
    ESP_LOGI(TAG, "I²S master initialized: %d Hz, 24-bit stereo, MCLK=%d MHz", 
             sample_rate, (sample_rate * 256) / 1000000);
    return true;
}

bool I2SMaster::start()
{
    if (rx_handle == nullptr) {
        ErrorHandler::log_error(ErrorType::I2S_ERROR, "Cannot start - I²S not initialized");
        return false;
    }
    
    esp_err_t err = i2s_channel_enable(rx_handle);
    if (err != ESP_OK) {
        ErrorHandler::log_error(ErrorType::I2S_ERROR, "Failed to enable I²S channel");
        return false;
    }
    
    ESP_LOGI(TAG, "I²S master started");
    return true;
}

bool I2SMaster::stop()
{
    if (rx_handle == nullptr) {
        return true;  // Already stopped
    }
    
    esp_err_t err = i2s_channel_disable(rx_handle);
    if (err != ESP_OK) {
        ErrorHandler::log_error(ErrorType::I2S_ERROR, "Failed to disable I²S channel");
        return false;
    }
    
    ESP_LOGI(TAG, "I²S master stopped");
    return true;
}

bool I2SMaster::read(uint8_t *buffer, size_t size, size_t *bytes_read, uint32_t timeout_ms)
{
    if (rx_handle == nullptr) {
        ErrorHandler::log_error(ErrorType::I2S_ERROR, "Cannot read - I²S not initialized");
        return false;
    }
    
    esp_err_t err = i2s_channel_read(rx_handle, buffer, size, bytes_read, 
                                      pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK) {
        if (err != ESP_ERR_TIMEOUT) {
            ErrorHandler::log_error(ErrorType::I2S_ERROR, "I²S read failed");
        }
        return false;
    }
    
    return true;
}

bool I2SMaster::change_sample_rate(uint32_t new_sample_rate)
{
    ESP_LOGI(TAG, "Changing sample rate from %d Hz to %d Hz", 
             current_sample_rate, new_sample_rate);
    
    // Stop I²S channel
    if (!stop()) {
        return false;
    }
    
    // Reconfigure clock
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(new_sample_rate);
    clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;  // Must be multiple of 3 for 24-bit
    clk_cfg.clk_src = I2S_CLK_SRC_APLL;
    
    esp_err_t err = i2s_channel_reconfig_std_clock(rx_handle, &clk_cfg);
    if (err != ESP_OK) {
        ErrorHandler::log_error(ErrorType::I2S_ERROR, "Failed to reconfigure I²S clock");
        return false;
    }
    
    current_sample_rate = new_sample_rate;
    
    // Restart I²S channel
    if (!start()) {
        return false;
    }
    
    ESP_LOGI(TAG, "Sample rate changed to %d Hz", new_sample_rate);
    return true;
}

uint32_t I2SMaster::get_sample_rate()
{
    return current_sample_rate;
}

void I2SMaster::deinit()
{
    if (rx_handle != nullptr) {
        stop();
        i2s_del_channel(rx_handle);
        rx_handle = nullptr;
        ESP_LOGI(TAG, "I²S master deinitialized");
    }
}
