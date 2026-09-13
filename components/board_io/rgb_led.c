#include "rgb_led.h"
#include "pinos.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/rmt_tx.h"
#include <string.h>

static const char *TAG = "rgb_led";
static rmt_channel_handle_t s_led_chan = NULL;
static rmt_encoder_handle_t s_led_encoder = NULL;
static rgb_led_config_t s_config = { .r = 0, .g = 0, .b = 0, .enabled = false };
static bool s_initialized = false;

// WS2812 T0H/T0L/T1H/T1L encoding logic
#define WS2812_T0H_NS 400
#define WS2812_T0L_NS 850
#define WS2812_T1H_NS 800
#define WS2812_T1L_NS 450
#define WS2812_RESET_US 50

esp_err_t rgb_led_init(void) {
    ESP_LOGI(TAG, "Inicializando LED RGB nativo (WS2812 via RMT)");
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = PIN_RGB_LED,
        .mem_block_symbols = 64,
        .resolution_hz = 10000000, // 10MHz = 100ns tick
        .trans_queue_depth = 4,
    };
    esp_err_t err = rmt_new_tx_channel(&tx_chan_config, &s_led_chan);
    if (err != ESP_OK) return err;

    rmt_bytes_encoder_config_t bytes_encoder_config = {
        .bit0 = { .duration0 = WS2812_T0H_NS/100, .level0 = 1, .duration1 = WS2812_T0L_NS/100, .level1 = 0 },
        .bit1 = { .duration0 = WS2812_T1H_NS/100, .level0 = 1, .duration1 = WS2812_T1L_NS/100, .level1 = 0 }
    };
    err = rmt_new_bytes_encoder(&bytes_encoder_config, &s_led_encoder);
    if (err != ESP_OK) return err;

    err = rmt_enable(s_led_chan);
    if (err != ESP_OK) return err;
    
    s_initialized = true;
    // Default config (vermelho ativo)
    s_config.enabled = true;
    s_config.r = 255;
    s_config.g = 0;
    s_config.b = 0;

    // Load config from NVS
    nvs_handle_t h;
    if (nvs_open("mps3_settings", NVS_READONLY, &h) == ESP_OK) {
        uint8_t en = 0, r = 0, g = 0, b = 0;
        if (nvs_get_u8(h, "led_en", &en) == ESP_OK) s_config.enabled = (bool)en;
        if (nvs_get_u8(h, "led_r", &r) == ESP_OK) s_config.r = r;
        if (nvs_get_u8(h, "led_g", &g) == ESP_OK) s_config.g = g;
        if (nvs_get_u8(h, "led_b", &b) == ESP_OK) s_config.b = b;
        nvs_close(h);
        ESP_LOGI(TAG, "LED config carregada da NVS: en=%d r=%d g=%d b=%d", s_config.enabled, s_config.r, s_config.g, s_config.b);
    }
    
    rgb_led_set_color(s_config.r, s_config.g, s_config.b);
    return ESP_OK;
}

void rgb_led_set_color(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_initialized) return;
    s_config.r = r;
    s_config.g = g;
    s_config.b = b;
    
    uint8_t pixels[3] = {0, 0, 0};
    if (s_config.enabled) {
        pixels[0] = g; // WS2812 is GRB
        pixels[1] = r;
        pixels[2] = b;
    }
    
    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    rmt_transmit(s_led_chan, s_led_encoder, pixels, sizeof(pixels), &tx_config);
}

void rgb_led_get_config(rgb_led_config_t *out_cfg) {
    if (out_cfg) *out_cfg = s_config;
}

void rgb_led_set_config(const rgb_led_config_t *cfg) {
    if (cfg) {
        s_config = *cfg;
        rgb_led_set_color(s_config.r, s_config.g, s_config.b);
    }
}

void rgb_led_toggle(void) {
    s_config.enabled = !s_config.enabled;
    rgb_led_set_color(s_config.r, s_config.g, s_config.b);
}

void rgb_led_save_nvs(void) {
    nvs_handle_t h;
    if (nvs_open("mps3_settings", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "led_en", (uint8_t)s_config.enabled);
        nvs_set_u8(h, "led_r", s_config.r);
        nvs_set_u8(h, "led_g", s_config.g);
        nvs_set_u8(h, "led_b", s_config.b);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "LED config salva na NVS");
    }
}
