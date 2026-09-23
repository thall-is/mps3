#ifndef __ESP_AVRC_API_H__
#define __ESP_AVRC_API_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_bt_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_AVRC_RN_PLAY_STATUS_CHANGE = 0x01,
    ESP_AVRC_RN_TRACK_CHANGE = 0x02,
    ESP_AVRC_RN_PLAY_POS_CHANGED = 0x05,
    ESP_AVRC_RN_VOLUME_CHANGE = 0x0d,
    ESP_AVRC_RN_MAX_EVT
} esp_avrc_rn_event_ids_t;

typedef union {
    uint8_t volume;
    uint32_t play_pos;
} esp_avrc_rn_param_t;

typedef enum {
    ESP_AVRC_CT_CONNECTION_STATE_EVT = 0,
    ESP_AVRC_CT_PASSTHROUGH_RSP_EVT = 1,
    ESP_AVRC_CT_METADATA_RSP_EVT = 2,
    ESP_AVRC_CT_PLAY_STATUS_RSP_EVT = 3,
    ESP_AVRC_CT_CHANGE_NOTIFY_EVT = 4,
    ESP_AVRC_CT_REMOTE_FEATURES_EVT = 5,
    ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT = 6,
    ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT = 7,
} esp_avrc_ct_cb_event_t;

typedef union {
    struct avrc_ct_conn_stat_param {
        bool connected;
        esp_bd_addr_t remote_bda;
    } conn_stat;

    struct avrc_ct_change_notify_param {
        uint8_t event_id;
        esp_avrc_rn_param_t event_parameter;
    } change_ntf;

    struct avrc_ct_set_volume_rsp_param {
        uint8_t volume;
    } set_volume_rsp;
} esp_avrc_ct_cb_param_t;

typedef void (* esp_avrc_ct_cb_t)(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);

static inline esp_err_t esp_avrc_ct_init(void) {
    return ESP_OK;
}

static inline esp_err_t esp_avrc_ct_deinit(void) {
    return ESP_OK;
}

static inline esp_err_t esp_avrc_ct_register_callback(esp_avrc_ct_cb_t callback) {
    (void)callback;
    return ESP_OK;
}

static inline esp_err_t esp_avrc_ct_send_set_absolute_volume_cmd(uint8_t tl, uint8_t volume) {
    (void)tl;
    (void)volume;
    return ESP_OK;
}

static inline esp_err_t esp_avrc_ct_send_register_notification_cmd(uint8_t tl, uint8_t event_id, uint32_t event_parameter) {
    (void)tl;
    (void)event_id;
    (void)event_parameter;
    return ESP_OK;
}

#ifdef __cplusplus
}
#endif

#endif // __ESP_AVRC_API_H__
