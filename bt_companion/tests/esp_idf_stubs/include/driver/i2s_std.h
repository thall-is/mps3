#ifndef DRIVER_I2S_STD_H_STUB
#define DRIVER_I2S_STD_H_STUB
#include "esp_err.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum { I2S_NUM_0, I2S_NUM_1 } i2s_port_t;
typedef enum { I2S_ROLE_MASTER, I2S_ROLE_SLAVE } i2s_role_t;
typedef enum { I2S_DATA_BIT_WIDTH_8BIT = 8, I2S_DATA_BIT_WIDTH_16BIT = 16, I2S_DATA_BIT_WIDTH_24BIT = 24, I2S_DATA_BIT_WIDTH_32BIT = 32 } i2s_data_bit_width_t;
typedef enum { I2S_SLOT_BIT_WIDTH_AUTO = 0 } i2s_slot_bit_width_t;
typedef enum { I2S_SLOT_MODE_MONO = 1, I2S_SLOT_MODE_STEREO = 2 } i2s_slot_mode_t;
typedef enum { I2S_CLK_SRC_DEFAULT = 0 } soc_periph_i2s_clk_src_t;
typedef soc_periph_i2s_clk_src_t i2s_clock_src_t;

#define I2S_GPIO_UNUSED (-1)

typedef struct { void *dummy; } *i2s_chan_handle_t;

typedef struct {
    i2s_port_t id;
    i2s_role_t role;
    uint32_t dma_desc_num;
    uint32_t dma_frame_num;
    bool auto_clear;
} i2s_chan_config_t;

#define I2S_CHANNEL_DEFAULT_CONFIG(cfg_port, cfg_role) \
    { .id = (cfg_port), .role = (cfg_role), .dma_desc_num = 6, .dma_frame_num = 240, .auto_clear = false }

typedef struct {
    i2s_clock_src_t clk_src;
    uint32_t mclk_multiple;
    uint32_t sample_rate_hz;
} i2s_std_clk_config_t;

#define I2S_STD_CLK_DEFAULT_CONFIG(rate) \
    { .clk_src = I2S_CLK_SRC_DEFAULT, .mclk_multiple = 256, .sample_rate_hz = (rate) }

typedef struct {
    i2s_data_bit_width_t data_bit_width;
    i2s_slot_bit_width_t slot_bit_width;
    i2s_slot_mode_t slot_mode;
    uint32_t slot_mask;
    uint32_t ws_width;
    bool ws_pol;
    bool bit_shift;
    bool left_align;
    bool big_endian;
    bool bit_order_lsb;
} i2s_std_slot_config_t;

static inline i2s_std_slot_config_t i2s_std_philips_slot_default(i2s_data_bit_width_t bw, i2s_slot_mode_t mode) {
    i2s_std_slot_config_t c = {0};
    c.data_bit_width = bw; c.slot_mode = mode;
    return c;
}
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bw, mode) i2s_std_philips_slot_default((bw), (mode))

typedef struct { bool mclk_inv, bclk_inv, ws_inv; } i2s_std_gpio_invert_flags_t;
typedef struct {
    gpio_num_t mclk, bclk, ws, dout, din;
    i2s_std_gpio_invert_flags_t invert_flags;
} i2s_std_gpio_config_t;

typedef struct {
    i2s_std_clk_config_t clk_cfg;
    i2s_std_slot_config_t slot_cfg;
    i2s_std_gpio_config_t gpio_cfg;
} i2s_std_config_t;

static inline esp_err_t i2s_new_channel(const i2s_chan_config_t *cfg, i2s_chan_handle_t *tx, i2s_chan_handle_t *rx) {
    (void)cfg;
    if (tx) *tx = (i2s_chan_handle_t)1;
    if (rx) *rx = (i2s_chan_handle_t)1;
    return ESP_OK;
}
static inline esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t h, const i2s_std_config_t *cfg) { (void)h; (void)cfg; return ESP_OK; }
static inline esp_err_t i2s_channel_reconfig_std_clock(i2s_chan_handle_t h, const i2s_std_clk_config_t *cfg) { (void)h; (void)cfg; return ESP_OK; }
static inline esp_err_t i2s_channel_enable(i2s_chan_handle_t h) { (void)h; return ESP_OK; }
static inline esp_err_t i2s_channel_disable(i2s_chan_handle_t h) { (void)h; return ESP_OK; }
static inline esp_err_t i2s_channel_write(i2s_chan_handle_t h, const void *buf, size_t len, size_t *written, uint32_t timeout) {
    (void)h; (void)buf; (void)timeout;
    if (written) *written = len;
    return ESP_OK;
}
static inline esp_err_t i2s_channel_read(i2s_chan_handle_t h, void *buf, size_t len, size_t *read_bytes, uint32_t timeout) {
    (void)h; (void)buf; (void)timeout;
    if (read_bytes) *read_bytes = len;
    return ESP_OK;
}
#endif
