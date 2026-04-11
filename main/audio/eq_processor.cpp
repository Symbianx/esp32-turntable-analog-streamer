#include "eq_processor.h"
#include "esp_dsp.h"
#include "esp_log.h"
#include <cstring>
#include <cmath>

static const char *TAG = "eq_processor";

// ─── Static DRAM_ATTR storage (Constitution §III: no heap in audio path) ─────
// DRAM_ATTR ensures arrays are in internal SRAM, preventing cache miss latency
// in the Xtensa ae32 biquad multiply-accumulate loop.

static DRAM_ATTR float  s_coef[EQ_MAX_BANDS][5];       // Biquad coefficients {b0,b1,b2,a1,a2}
static DRAM_ATTR float  s_w[EQ_MAX_BANDS][4];           // Stereo delay lines {wL0,wL1,wR0,wR1}
static DRAM_ATTR float  s_float_buf[EQ_FRAMES_PER_BLOCK * 2]; // Float32 LRLR interleaved workspace
static DRAM_ATTR EQBandConfig s_bands[EQ_MAX_BANDS];   // Band configs (read each DMA block)

static DRAM_ATTR bool     s_enabled      = false;
static DRAM_ATTR uint8_t  s_active_bands = 0;
static uint32_t s_sample_rate  = 48000;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static inline float clamp_f(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Custom peaking EQ coefficient generator using the Audio EQ Cookbook formula.
// NOTE: dsps_biquad_gen_peakingEQ_f32 does NOT support a gain parameter —
//       it generates a 0 dB bandpass, unsuitable for parametric EQ.
static void biquad_gen_peak(float *c, float freq_hz, float gain_db, float Q, float Fs) {
    float A     = powf(10.0f, gain_db / 40.0f);
    float w0    = 2.0f * static_cast<float>(M_PI) * (freq_hz / Fs);
    float sn    = sinf(w0);
    float cs    = cosf(w0);
    float alpha = sn / (2.0f * Q);
    float a0_inv = 1.0f / (1.0f + alpha / A);

    c[0] = (1.0f + alpha * A) * a0_inv;  // b0
    c[1] = (-2.0f * cs)       * a0_inv;  // b1
    c[2] = (1.0f - alpha * A) * a0_inv;  // b2
    c[3] = (-2.0f * cs)       * a0_inv;  // a1  (same as b1 for peaking)
    c[4] = (1.0f - alpha / A) * a0_inv;  // a2
}

// Recompute biquad coefficients for one band from s_bands[b] and s_sample_rate.
static void recompute_band_coef(uint8_t b) {
    const EQBandConfig& band = s_bands[b];

    if (!band.enabled) {
        // Identity filter: pass through without processing
        s_coef[b][0] = 1.0f; s_coef[b][1] = 0.0f; s_coef[b][2] = 0.0f;
        s_coef[b][3] = 0.0f; s_coef[b][4] = 0.0f;
        return;
    }

    float freq  = clamp_f(band.frequency_hz, 20.0f, (float)s_sample_rate * 0.5f - 1.0f);
    float gain  = clamp_f(band.gain_db,  -24.0f,  24.0f);
    float Q     = clamp_f(band.q_factor,   0.1f,  10.0f);
    float f_norm = freq / (float)s_sample_rate;  // Normalized freq for esp-dsp generators

    switch (band.filter_type) {
        case EQFilterType::LOW_SHELF:
            dsps_biquad_gen_lowShelf_f32(s_coef[b], f_norm, gain, Q);
            break;
        case EQFilterType::HIGH_SHELF:
            dsps_biquad_gen_highShelf_f32(s_coef[b], f_norm, gain, Q);
            break;
        case EQFilterType::LOW_PASS:
            dsps_biquad_gen_lpf_f32(s_coef[b], f_norm, Q);
            break;
        case EQFilterType::HIGH_PASS:
            dsps_biquad_gen_hpf_f32(s_coef[b], f_norm, Q);
            break;
        case EQFilterType::PEAKING:
        default:
            biquad_gen_peak(s_coef[b], freq, gain, Q, (float)s_sample_rate);
            break;
    }

    ESP_LOGD(TAG, "Band %u: %s %.0fHz %.1fdB Q=%.2f -> coef=[%.4f %.4f %.4f %.4f %.4f]",
             b, eq_filter_type_to_str(band.filter_type), freq, gain, Q,
             s_coef[b][0], s_coef[b][1], s_coef[b][2], s_coef[b][3], s_coef[b][4]);
}

static uint8_t count_active_bands() {
    uint8_t count = 0;
    for (uint8_t b = 0; b < EQ_MAX_BANDS; b++) {
        if (s_bands[b].enabled) count++;
    }
    return count;
}

// ─── Public API ──────────────────────────────────────────────────────────────

bool EQProcessor::init(const DeviceConfig& config, uint32_t sample_rate) {
    s_sample_rate = sample_rate;
    s_enabled     = config.eq_enabled;

    memcpy(s_bands, config.eq_bands, sizeof(s_bands));
    memset(s_w, 0, sizeof(s_w));

    for (uint8_t b = 0; b < EQ_MAX_BANDS; b++) {
        recompute_band_coef(b);
    }
    s_active_bands = count_active_bands();

    ESP_LOGI(TAG, "EQ initialized: %u/%u bands active, sample_rate=%lu, %s",
             s_active_bands, EQ_MAX_BANDS, (unsigned long)s_sample_rate,
             s_enabled ? "ENABLED" : "bypassed");

    for (uint8_t b = 0; b < EQ_MAX_BANDS; b++) {
        ESP_LOGI(TAG, "  Band %u: %s %.0fHz %.1fdB Q=%.2f [%s]",
                 b, eq_filter_type_to_str(s_bands[b].filter_type),
                 s_bands[b].frequency_hz, s_bands[b].gain_db, s_bands[b].q_factor,
                 s_bands[b].enabled ? "on" : "off");
    }
    return true;
}

// process() is called from Core 0 audio_capture_task per DMA block.
// Constitution §IV: no mutex; float writes from Core 1 are atomic on Xtensa.
bool EQProcessor::process(const uint8_t* input_i2s, uint8_t* output_24, size_t frames) {
    if (!s_enabled || s_active_bands == 0) {
        return false;  // Caller uses legacy bit-packing path — zero overhead
    }

    // Step 1: Convert 32-bit MSB-aligned I²S slots → float32 LRLR normalized [-1, +1]
    // I²S layout per stereo frame (8 bytes):
    //   [L_byte0=0, L_byte1=LSB, L_byte2=mid, L_byte3=MSB]
    //   [R_byte0=0, R_byte1=LSB, R_byte2=mid, R_byte3=MSB]
    for (size_t i = 0; i < frames; i++) {
        size_t src = i * 8;

        int32_t lv = (int32_t)input_i2s[src + 1]
                   | ((int32_t)input_i2s[src + 2] << 8)
                   | ((int32_t)input_i2s[src + 3] << 16);
        if (lv & 0x800000) lv |= 0xFF000000;  // sign-extend 24→32 bit

        int32_t rv = (int32_t)input_i2s[src + 5]
                   | ((int32_t)input_i2s[src + 6] << 8)
                   | ((int32_t)input_i2s[src + 7] << 16);
        if (rv & 0x800000) rv |= 0xFF000000;

        s_float_buf[i * 2 + 0] = (float)lv / 8388608.0f;  // L — normalize to [-1, +1]
        s_float_buf[i * 2 + 1] = (float)rv / 8388608.0f;  // R
    }

    // Step 2: Apply biquad filter chain in-place (stereo interleaved LRLR)
    // dsps_biquad_sf32 is a macro → resolves to ae32/aes3 FPU assembly on ESP32/S3
    for (uint8_t b = 0; b < EQ_MAX_BANDS; b++) {
        if (!s_bands[b].enabled) continue;
        dsps_biquad_sf32(s_float_buf, s_float_buf, (int)frames, s_coef[b], s_w[b]);
    }

    // Step 3: Convert float32 LRLR → 24-bit packed little-endian stereo
    for (size_t i = 0; i < frames; i++) {
        size_t dst = i * 6;

        float lf = s_float_buf[i * 2 + 0];
        float rf = s_float_buf[i * 2 + 1];

        // Hard-clip to [-1, +1] before quantization
        if (lf >  1.0f) lf =  1.0f;
        if (lf < -1.0f) lf = -1.0f;
        if (rf >  1.0f) rf =  1.0f;
        if (rf < -1.0f) rf = -1.0f;

        int32_t lo = (int32_t)(lf * 8388607.0f);
        int32_t ro = (int32_t)(rf * 8388607.0f);

        output_24[dst + 0] = (uint8_t)( lo        & 0xFF);  // L0 LSB
        output_24[dst + 1] = (uint8_t)((lo >>  8) & 0xFF);  // L1
        output_24[dst + 2] = (uint8_t)((lo >> 16) & 0xFF);  // L2 MSB
        output_24[dst + 3] = (uint8_t)( ro        & 0xFF);  // R0 LSB
        output_24[dst + 4] = (uint8_t)((ro >>  8) & 0xFF);  // R1
        output_24[dst + 5] = (uint8_t)((ro >> 16) & 0xFF);  // R2 MSB
    }

    return true;
}

void EQProcessor::update_band(uint8_t band_index, const EQBandConfig& band, uint32_t sample_rate) {
    if (band_index >= EQ_MAX_BANDS) return;

    s_sample_rate       = sample_rate;
    s_bands[band_index] = band;

    // Recompute coefficients — delay lines (s_w[b]) are intentionally NOT zeroed
    // to avoid audible clicks during live parameter changes.
    recompute_band_coef(band_index);
    s_active_bands = count_active_bands();

    ESP_LOGI(TAG, "Band %u updated: %s %.0fHz %.1fdB Q=%.2f [%s]",
             band_index, eq_filter_type_to_str(band.filter_type),
             band.frequency_hz, band.gain_db, band.q_factor,
             band.enabled ? "on" : "off");
}

void EQProcessor::set_enabled(bool enabled) {
    bool was_enabled = s_enabled;
    s_enabled = enabled;

    if (enabled && !was_enabled) {
        // Transitioning off→on: zero delay lines to avoid artifacts from stale state
        memset(s_w, 0, sizeof(s_w));
        s_active_bands = count_active_bands();
        ESP_LOGI(TAG, "EQ enabled (%u active bands)", s_active_bands);
    } else if (!enabled && was_enabled) {
        ESP_LOGI(TAG, "EQ disabled (bypass)");
    }
}

bool EQProcessor::is_enabled() {
    return s_enabled;
}

uint8_t EQProcessor::active_band_count() {
    return s_active_bands;
}

uint32_t EQProcessor::get_sample_rate() {
    return s_sample_rate;
}
