#ifndef DRIVER_GPIO_H_STUB
#define DRIVER_GPIO_H_STUB
#include "esp_err.h"
#include <stdint.h>

typedef int gpio_num_t;
typedef enum { GPIO_MODE_DISABLE = 0, GPIO_MODE_INPUT, GPIO_MODE_OUTPUT, GPIO_MODE_OUTPUT_OD } gpio_mode_t;
typedef enum { GPIO_PULLUP_DISABLE = 0, GPIO_PULLUP_ENABLE } gpio_pullup_t;
typedef enum { GPIO_PULLDOWN_DISABLE = 0, GPIO_PULLDOWN_ENABLE } gpio_pulldown_t;
typedef enum { GPIO_INTR_DISABLE = 0, GPIO_INTR_POSEDGE, GPIO_INTR_NEGEDGE, GPIO_INTR_ANYEDGE } gpio_int_type_t;

typedef struct {
    uint64_t pin_bit_mask;
    gpio_mode_t mode;
    gpio_pullup_t pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;

static inline esp_err_t gpio_config(const gpio_config_t *cfg) { (void)cfg; return ESP_OK; }
static inline esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level) { (void)pin; (void)level; return ESP_OK; }
static inline int gpio_get_level(gpio_num_t pin) { (void)pin; return 0; }
#endif
