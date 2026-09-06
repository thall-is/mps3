#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    bool enabled;
} rgb_led_config_t;

esp_err_t rgb_led_init(void);
void rgb_led_set_color(uint8_t r, uint8_t g, uint8_t b);
void rgb_led_get_config(rgb_led_config_t *out_cfg);
void rgb_led_set_config(const rgb_led_config_t *cfg);
void rgb_led_toggle(void);
void rgb_led_save_nvs(void);

#ifdef __cplusplus
}
#endif
