#ifndef ESP_BT_H_STUB
#define ESP_BT_H_STUB
#include <stdint.h>
#include "esp_err.h"
typedef enum { ESP_BT_MODE_IDLE = 0, ESP_BT_MODE_BLE, ESP_BT_MODE_CLASSIC_BT, ESP_BT_MODE_BTDM } esp_bt_mode_t;
typedef struct { int dummy; uint8_t mode; } esp_bt_controller_config_t;
#define BT_CONTROLLER_INIT_CONFIG_DEFAULT() { .dummy = 0, .mode = ESP_BT_MODE_BTDM }
static inline esp_err_t esp_bt_controller_init(esp_bt_controller_config_t *cfg) { (void)cfg; return ESP_OK; }
static inline esp_err_t esp_bt_controller_enable(esp_bt_mode_t mode) { (void)mode; return ESP_OK; }
#endif
