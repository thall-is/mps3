#ifndef ESP_BT_DEFS_H_STUB
#define ESP_BT_DEFS_H_STUB
#include <stdint.h>
typedef uint8_t esp_bd_addr_t[6];
typedef enum {
    ESP_BT_STATUS_SUCCESS = 0, ESP_BT_STATUS_FAIL, ESP_BT_STATUS_NOT_READY,
    ESP_BT_STATUS_BUSY, ESP_BT_STATUS_UNSUPPORTED,
} esp_bt_status_t;
#endif
