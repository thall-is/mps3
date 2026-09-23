#ifndef ESP_GAP_BT_API_H_STUB
#define ESP_GAP_BT_API_H_STUB
#include "esp_err.h"
#include "esp_bt_defs.h"
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    ESP_BT_GAP_DISC_RES_EVT = 0,
    ESP_BT_GAP_DISC_STATE_CHANGED_EVT,
    ESP_BT_GAP_RMT_SRVCS_EVT,
    ESP_BT_GAP_RMT_SRVC_REC_EVT,
    ESP_BT_GAP_AUTH_CMPL_EVT,
    ESP_BT_GAP_PIN_REQ_EVT,
    ESP_BT_GAP_CFM_REQ_EVT,
    ESP_BT_GAP_KEY_NOTIF_EVT,
    ESP_BT_GAP_KEY_REQ_EVT,
} esp_bt_gap_cb_event_t;

typedef enum {
    ESP_BT_NON_DISCOVERABLE,
    ESP_BT_LIMITED_DISCOVERABLE,
    ESP_BT_GENERAL_DISCOVERABLE
} esp_bt_discovery_mode_t;

typedef enum {
    ESP_BT_NON_CONNECTABLE,
    ESP_BT_CONNECTABLE
} esp_bt_connection_mode_t;

typedef enum {
    ESP_BT_INQ_MODE_GENERAL_INQUIRY,
    ESP_BT_INQ_MODE_LIMITED_INQUIRY,
} esp_bt_inq_mode_t;

typedef enum {
    ESP_BT_GAP_DISCOVERY_STOPPED,
    ESP_BT_GAP_DISCOVERY_STARTED,
} esp_bt_gap_discovery_state_t;

typedef enum {
    ESP_BT_GAP_DEV_PROP_BDNAME = 1,
    ESP_BT_GAP_DEV_PROP_COD,
    ESP_BT_GAP_DEV_PROP_RSSI,
    ESP_BT_GAP_DEV_PROP_EIR,
} esp_bt_gap_dev_prop_type_t;

typedef struct {
    esp_bt_gap_dev_prop_type_t type;
    int len;
    void *val;
} esp_bt_gap_dev_prop_t;

#define ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME 0x08
#define ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME  0x09

#define ESP_BT_PIN_CODE_LEN 16
typedef uint8_t esp_bt_pin_code_t[ESP_BT_PIN_CODE_LEN];
typedef enum { ESP_BT_SP_IOCAP_MODE } esp_bt_sp_param_t;
typedef uint8_t esp_bt_io_cap_t;
#define ESP_BT_IO_CAP_NONE 3

typedef union {
    struct {
        esp_bd_addr_t bda;
        int num_prop;
        esp_bt_gap_dev_prop_t *prop;
    } disc_res;
    struct {
        esp_bt_gap_discovery_state_t state;
    } disc_st_chg;
    struct { esp_bt_status_t stat; esp_bd_addr_t bda; uint8_t device_name[32]; } auth_cmpl;
    struct { esp_bd_addr_t bda; bool min_16_digit; } pin_req;
    struct { esp_bd_addr_t bda; uint32_t num_val; } cfm_req;
    struct { esp_bd_addr_t bda; uint32_t passkey; } key_notif;
    struct { esp_bd_addr_t bda; } key_req;
} esp_bt_gap_cb_param_t;

typedef void (*esp_bt_gap_cb_t)(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);

static inline esp_err_t esp_bt_gap_set_device_name(const char *name) { (void)name; return ESP_OK; }
static inline esp_err_t esp_bt_gap_register_callback(esp_bt_gap_cb_t cb) { (void)cb; return ESP_OK; }
static inline esp_err_t esp_bt_gap_set_scan_mode(esp_bt_connection_mode_t c, esp_bt_discovery_mode_t d) { (void)c; (void)d; return ESP_OK; }
static inline esp_err_t esp_bt_gap_start_discovery(esp_bt_inq_mode_t mode, uint8_t inq_len, uint8_t num_rsps) {
    (void)mode; (void)inq_len; (void)num_rsps; return ESP_OK;
}
static inline esp_err_t esp_bt_gap_cancel_discovery(void) { return ESP_OK; }
static inline uint8_t *esp_bt_gap_resolve_eir_data(uint8_t *eir, uint8_t type, uint8_t *length) {
    (void)eir; (void)type; (void)length; return (void*)0;
}
static inline esp_err_t esp_bt_gap_pin_reply(esp_bd_addr_t bda, bool accept, uint8_t len, esp_bt_pin_code_t pin) {
    (void)bda; (void)accept; (void)len; (void)pin; return ESP_OK;
}
static inline esp_err_t esp_bt_gap_ssp_confirm_reply(esp_bd_addr_t bda, bool accept) { (void)bda; (void)accept; return ESP_OK; }
static inline esp_err_t esp_bt_gap_set_security_param(esp_bt_sp_param_t t, void *value, uint8_t len) {
    (void)t; (void)value; (void)len; return ESP_OK;
}
static inline esp_err_t esp_bt_gap_remove_bond_device(esp_bd_addr_t bda) { (void)bda; return ESP_OK; }

#endif
