#include "audio_meter.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "audio_meter";

#define NVS_NAMESPACE_METER "audio_meter"
#define NVS_KEY_MODE        "meter_mode"

#define METER_FLOOR_DB      (-40.0f) // Escala de -40 dBFS a 0 dBFS
#define DECAY_RATE_PER_SEC  (1.0f / 0.300f) // Queda total de 1.0 a 0.0 em 300 ms
#define HOLD_TIME_US        (700000)        // 700 ms de pico retido
#define HOLD_FALL_RATE      (1.0f / 0.250f) // Queda do pico apos hold em 250 ms

// Variaveis atomicas alimentadas pelo audio thread
static volatile int32_t s_raw_peak_l = 0;
static volatile int32_t s_raw_peak_r = 0;
static volatile int64_t s_last_feed_time_us = 0;

// Buffer de captura rapida para espectro RTA (64 amostras mono a cada >=20ms)
static int16_t s_spec_samples[64] = {0};
static volatile int64_t s_last_spec_snapshot_us = 0;

// Estado da balistica consumido pela display_task
static float s_disp_peak_l = 0.0f;
static float s_disp_peak_r = 0.0f;
static float s_disp_hold_l = 0.0f;
static float s_disp_hold_r = 0.0f;
static int64_t s_hold_expire_l_us = 0;
static int64_t s_hold_expire_r_us = 0;
static int64_t s_last_update_us = 0;

// Estado da balistica do espectro (11 bandas)
static float s_disp_spec_bands[AUDIO_METER_SPECTRUM_BANDS] = {0};
static float s_disp_spec_peaks[AUDIO_METER_SPECTRUM_BANDS] = {0};
static int64_t s_spec_hold_expires[AUDIO_METER_SPECTRUM_BANDS] = {0};
static int64_t s_last_spec_update_us = 0;

// Tabela senoidal pré-calculada de 64 pontos (int8_t, amplitude 127)
static const int8_t s_sin64[64] = {
      0,  12,  25,  37,  49,  60,  71,  80,  89,  98, 105, 111, 117, 121, 125, 126,
    127, 126, 125, 121, 117, 111, 105,  98,  89,  80,  71,  60,  49,  37,  25,  12,
      0, -12, -25, -37, -49, -60, -71, -80, -89, -98,-105,-111,-117,-121,-125,-126,
   -127,-126,-125,-121,-117,-111,-105, -98, -89, -80, -71, -60, -49, -37, -25, -12
};

static audio_meter_mode_t s_meter_mode = AUDIO_METER_MODE_PRE_VOLUME;
static bool s_initialized = false;

esp_err_t audio_meter_init(void)
{
    if (s_initialized) return ESP_OK;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE_METER, NVS_READONLY, &h) == ESP_OK) {
        uint8_t m = 0;
        if (nvs_get_u8(h, NVS_KEY_MODE, &m) == ESP_OK) {
            s_meter_mode = (m == 1) ? AUDIO_METER_MODE_POST_VOLUME : AUDIO_METER_MODE_PRE_VOLUME;
        }
        nvs_close(h);
    }

    s_last_update_us = esp_timer_get_time();
    s_initialized = true;
    ESP_LOGI(TAG, "Audio Meter inicializado (Modo: %s, Escala: %.0f a 0 dBFS)",
             (s_meter_mode == AUDIO_METER_MODE_PRE_VOLUME) ? "Pre-Volume" : "Pos-Volume",
             METER_FLOOR_DB);
    return ESP_OK;
}

void IRAM_ATTR audio_meter_feed(const int32_t *samples, size_t sample_count, uint32_t sample_rate)
{
    if (!samples || sample_count < 2) return;

    int32_t max_l = 0;
    int32_t max_r = 0;

    size_t pairs = sample_count >> 1;
    // Em faixas Hi-Res (96 kHz / 192 kHz / 384 kHz), decimamos a leitura de pico
    // mantendo ~48 kHz efetivos para o VU meter, reduzindo drasticamente o trafego de PSRAM e carga de CPU no Core 0.
    size_t stride = (sample_rate > 192000) ? 8 : ((sample_rate > 96000) ? 4 : ((sample_rate > 48000) ? 2 : 1));
    for (size_t i = 0; i < pairs; i += stride) {
        int32_t sl = samples[i * 2];
        int32_t sr = samples[i * 2 + 1];

        // Valor absoluto seguro para int32
        if (sl < 0) sl = (sl == INT32_MIN) ? INT32_MAX : -sl;
        if (sr < 0) sr = (sr == INT32_MIN) ? INT32_MAX : -sr;

        if (sl > max_l) max_l = sl;
        if (sr > max_r) max_r = sr;
    }

    // Atualiza picos brutos de forma atomica/leve
    if (max_l > s_raw_peak_l) s_raw_peak_l = max_l;
    if (max_r > s_raw_peak_r) s_raw_peak_r = max_r;

    // Atualiza snapshot de 64 amostras mono para o espectro RTA (max 50 Hz, a cada >= 20 ms)
    int64_t now_us = esp_timer_get_time();
    if ((now_us - s_last_spec_snapshot_us) >= 20000 && pairs >= 64) {
        s_last_spec_snapshot_us = now_us;
        size_t step = pairs / 64;
        if (step < 1) step = 1;
        for (size_t k = 0; k < 64; k++) {
            size_t idx = k * step;
            if (idx >= pairs) idx = pairs - 1;
            int32_t mono = (samples[idx * 2] / 2) + (samples[idx * 2 + 1] / 2);
            s_spec_samples[k] = (int16_t)(mono >> 16);
        }
    }

    s_last_feed_time_us = now_us;
}

static inline float mag_to_db(int32_t mag)
{
    if (mag <= 0) return -100.0f;
    // Escala normalizada para int32 full-scale (2147483647.0f)
    float ratio = (float)mag / 2147483647.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    return 20.0f * log10f(ratio);
}

static inline float db_to_normalized(float db)
{
    if (db <= METER_FLOOR_DB) return 0.0f;
    if (db >= 0.0f) return 1.0f;
    return (db - METER_FLOOR_DB) / (-METER_FLOOR_DB); // Mapeia [-40, 0] para [0, 1]
}

void audio_meter_get_levels(float *out_peak_l, float *out_peak_r,
                           float *out_hold_l, float *out_hold_r,
                           float *out_db_l, float *out_db_r)
{
    int64_t now = esp_timer_get_time();
    float dt = (float)(now - s_last_update_us) / 1000000.0f;
    if (dt < 0.0f || dt > 1.0f) dt = 0.033f; // Default ~30 FPS se houver salto
    s_last_update_us = now;

    // Se nao houver alimentacao ha mais de 150 ms, considera silencio
    bool active = (now - s_last_feed_time_us) <= 150000;

    int32_t raw_l = active ? s_raw_peak_l : 0;
    int32_t raw_r = active ? s_raw_peak_r : 0;

    // Reseta picos brutos para nova janela de amostragem
    s_raw_peak_l = 0;
    s_raw_peak_r = 0;

    float db_l = mag_to_db(raw_l);
    float db_r = mag_to_db(raw_r);

    float target_l = db_to_normalized(db_l);
    float target_r = db_to_normalized(db_r);

    // --- BALISTICA CANAL L ---
    if (target_l >= s_disp_peak_l) {
        s_disp_peak_l = target_l; // Ataque instantaneo (0 ms)
    } else {
        s_disp_peak_l -= DECAY_RATE_PER_SEC * dt; // Decaimento suave
        if (s_disp_peak_l < target_l) s_disp_peak_l = target_l;
        if (s_disp_peak_l < 0.0f) s_disp_peak_l = 0.0f;
    }

    if (s_disp_peak_l >= s_disp_hold_l) {
        s_disp_hold_l = s_disp_peak_l;
        s_hold_expire_l_us = now + HOLD_TIME_US; // Renova retencao de 700 ms
    } else if (now > s_hold_expire_l_us) {
        s_disp_hold_l -= HOLD_FALL_RATE * dt; // Queda do pico apos o tempo
        if (s_disp_hold_l < s_disp_peak_l) s_disp_hold_l = s_disp_peak_l;
        if (s_disp_hold_l < 0.0f) s_disp_hold_l = 0.0f;
    }

    // --- BALISTICA CANAL R ---
    if (target_r >= s_disp_peak_r) {
        s_disp_peak_r = target_r; // Ataque instantaneo (0 ms)
    } else {
        s_disp_peak_r -= DECAY_RATE_PER_SEC * dt;
        if (s_disp_peak_r < target_r) s_disp_peak_r = target_r;
        if (s_disp_peak_r < 0.0f) s_disp_peak_r = 0.0f;
    }

    if (s_disp_peak_r >= s_disp_hold_r) {
        s_disp_hold_r = s_disp_peak_r;
        s_hold_expire_r_us = now + HOLD_TIME_US;
    } else if (now > s_hold_expire_r_us) {
        s_disp_hold_r -= HOLD_FALL_RATE * dt;
        if (s_disp_hold_r < s_disp_peak_r) s_disp_hold_r = s_disp_peak_r;
        if (s_disp_hold_r < 0.0f) s_disp_hold_r = 0.0f;
    }

    if (out_peak_l) *out_peak_l = s_disp_peak_l;
    if (out_peak_r) *out_peak_r = s_disp_peak_r;
    if (out_hold_l) *out_hold_l = s_disp_hold_l;
    if (out_hold_r) *out_hold_r = s_disp_hold_r;

    if (out_db_l) *out_db_l = (db_l < METER_FLOOR_DB) ? METER_FLOOR_DB : db_l;
    if (out_db_r) *out_db_r = (db_r < METER_FLOOR_DB) ? METER_FLOOR_DB : db_r;
}

void audio_meter_set_mode(audio_meter_mode_t mode, bool save_nvs)
{
    s_meter_mode = mode;
    if (save_nvs) {
        nvs_handle_t h;
        if (nvs_open(NVS_NAMESPACE_METER, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, NVS_KEY_MODE, (uint8_t)mode);
            nvs_commit(h);
            nvs_close(h);
        }
    }
}

audio_meter_mode_t audio_meter_get_mode(void)
{
    return s_meter_mode;
}

bool audio_meter_is_active(void)
{
    if (s_last_feed_time_us == 0) return false;
    int64_t diff = esp_timer_get_time() - s_last_feed_time_us;
    return (diff >= 0 && diff <= 150000); // Ativo se recebeu dados nos ultimos 150 ms
}

void audio_meter_get_spectrum_bands(float out_bands[AUDIO_METER_SPECTRUM_BANDS], float out_peaks[AUDIO_METER_SPECTRUM_BANDS])
{
    int64_t now = esp_timer_get_time();
    float dt = (float)(now - s_last_spec_update_us) / 1000000.0f;
    if (dt < 0.0f || dt > 1.0f) dt = 0.040f; // Default 25 FPS
    s_last_spec_update_us = now;

    bool active = (now - s_last_feed_time_us) <= 150000;

    // Frequencias representativas para as 11 bandas (k in [1..31])
    static const uint8_t s_k_bins[AUDIO_METER_SPECTRUM_BANDS] = {
        1,  // ~60 Hz (Sub-Bass)
        2,  // ~125 Hz (Bass)
        3,  // ~250 Hz (Low Mid)
        4,  // ~400 Hz
        6,  // ~600 Hz (Mid)
        8,  // ~1 kHz
        11, // ~1.5 kHz
        15, // ~2.5 kHz (Upper Mid)
        20, // ~4 kHz (Presence)
        25, // ~8 kHz (Brilliance)
        31  // ~16 kHz (Air)
    };

    // Compensacao de ganho perceptual (curva adaptativa para visual harmonico balanceado)
    static const float s_band_gains[AUDIO_METER_SPECTRUM_BANDS] = {
        1.30f, 1.20f, 1.10f, 1.00f, 0.95f, 0.90f, 0.85f, 0.90f, 1.05f, 1.20f, 1.45f
    };

    for (int b = 0; b < AUDIO_METER_SPECTRUM_BANDS; b++) {
        float target = 0.0f;
        if (active) {
            uint8_t k = s_k_bins[b];
            int32_t sum_i = 0;
            int32_t sum_q = 0;
            for (int n = 0; n < 64; n++) {
                uint8_t phase = (n * k) & 63;
                int8_t s = s_sin64[phase];
                int8_t c = s_sin64[(phase + 16) & 63];
                int32_t val = (int32_t)s_spec_samples[n];
                sum_i += (val * c) >> 7;
                sum_q += (val * s) >> 7;
            }
            float mag = sqrtf((float)sum_i * sum_i + (float)sum_q * sum_q) / 32.0f;
            float db = 20.0f * log10f(mag > 1.0f ? mag : 1.0f);
            target = (db - 18.0f) / 52.0f;
            target *= s_band_gains[b];
            if (target < 0.0f) target = 0.0f;
            if (target > 1.0f) target = 1.0f;
        }

        // Balistica da banda: ataque instantaneo, decaimento suave
        if (target >= s_disp_spec_bands[b]) {
            s_disp_spec_bands[b] = target;
        } else {
            s_disp_spec_bands[b] -= (DECAY_RATE_PER_SEC * 1.15f) * dt;
            if (s_disp_spec_bands[b] < target) s_disp_spec_bands[b] = target;
            if (s_disp_spec_bands[b] < 0.0f) s_disp_spec_bands[b] = 0.0f;
        }

        // Peak-hold da banda: retencao e queda
        if (s_disp_spec_bands[b] >= s_disp_spec_peaks[b]) {
            s_disp_spec_peaks[b] = s_disp_spec_bands[b];
            s_spec_hold_expires[b] = now + (HOLD_TIME_US - 200000); // 500 ms de retencao
        } else if (now > s_spec_hold_expires[b]) {
            s_disp_spec_peaks[b] -= (HOLD_FALL_RATE * 1.25f) * dt;
            if (s_disp_spec_peaks[b] < s_disp_spec_bands[b]) s_disp_spec_peaks[b] = s_disp_spec_bands[b];
            if (s_disp_spec_peaks[b] < 0.0f) s_disp_spec_peaks[b] = 0.0f;
        }

        if (out_bands) out_bands[b] = s_disp_spec_bands[b];
        if (out_peaks) out_peaks[b] = s_disp_spec_peaks[b];
    }
}


