#ifndef ESP_A2DP_API_H_STUB
#define ESP_A2DP_API_H_STUB
// Stub fiel ao esp_a2dp_api.h real do commit travado
// (08e0d30a74ad0bfd5a34933142b80f45619ee410), JA' COM o patch 0001
// aplicado (campo ldac_info[8] na union) - ver esp-idf-patches/ na raiz
// do bt_companion. Extraido lendo o header real, nao "de memoria".
#include "esp_err.h"
#include "esp_bt_defs.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#define ESP_A2D_MAX_SEPS 4 // CONFIG_BT_A2DP_SEP_NUM_MAX deste projeto

typedef uint16_t esp_a2d_conn_hdl_t;

#define ESP_A2D_MCT_SBC      (0)
#define ESP_A2D_MCT_M12      (0x01)
#define ESP_A2D_MCT_M24      (0x02)
#define ESP_A2D_MCT_ATRAC    (0x04)
#define ESP_A2D_MCT_NON_A2DP (0xff)
typedef uint8_t esp_a2d_mct_t;

#define ESP_A2D_SBC_CIE_SF_16K 0x8
#define ESP_A2D_SBC_CIE_SF_32K 0x4
#define ESP_A2D_SBC_CIE_SF_44K 0x2
#define ESP_A2D_SBC_CIE_SF_48K 0x1
#define ESP_A2D_SBC_CIE_CH_MODE_MONO         0x8
#define ESP_A2D_SBC_CIE_CH_MODE_DUAL_CHANNEL 0x4
#define ESP_A2D_SBC_CIE_CH_MODE_STEREO       0x2
#define ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO 0x1
#define ESP_A2D_SBC_CIE_BLOCK_LEN_4  0x8
#define ESP_A2D_SBC_CIE_BLOCK_LEN_8  0x4
#define ESP_A2D_SBC_CIE_BLOCK_LEN_12 0x2
#define ESP_A2D_SBC_CIE_BLOCK_LEN_16 0x1
#define ESP_A2D_SBC_CIE_NUM_SUBBANDS_4 0x2
#define ESP_A2D_SBC_CIE_NUM_SUBBANDS_8 0x1
#define ESP_A2D_SBC_CIE_ALLOC_MTHD_SNR      0x2
#define ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS 0x1

typedef struct {
    uint8_t ch_mode      : 4;
    uint8_t samp_freq    : 4;
    uint8_t alloc_mthd   : 2;
    uint8_t num_subbands : 2;
    uint8_t block_len    : 4;
    uint8_t min_bitpool;
    uint8_t max_bitpool;
} __attribute__((packed)) esp_a2d_cie_sbc_t;

typedef struct { uint8_t dummy[4]; } __attribute__((packed)) esp_a2d_cie_m12_t;
typedef struct { uint8_t dummy[6]; } __attribute__((packed)) esp_a2d_cie_m24_t;
typedef struct { uint8_t dummy[7]; } __attribute__((packed)) esp_a2d_cie_atrac_t;

typedef struct {
    esp_a2d_mct_t type;
#define ESP_A2D_CIE_LEN_SBC   (4)
#define ESP_A2D_CIE_LEN_M12   (4)
#define ESP_A2D_CIE_LEN_M24   (6)
#define ESP_A2D_CIE_LEN_ATRAC (7)
#define ESP_A2D_CIE_LEN_LDAC  (8) // patch 0001
    union {
        esp_a2d_cie_sbc_t   sbc_info;
        esp_a2d_cie_m12_t   m12_info;
        esp_a2d_cie_m24_t   m24_info;
        esp_a2d_cie_atrac_t atrac_info;
        uint8_t             ldac_info[ESP_A2D_CIE_LEN_LDAC]; // patch 0001 - reaproveitado p/ aptX/aptX HD tambem (ver bt_source.c)
    } cie;
} __attribute__((packed)) esp_a2d_mcc_t;

typedef struct { uint8_t seid; esp_a2d_mcc_t mcc; } esp_a2d_sep_mcc_t;

typedef enum {
    ESP_A2D_CONNECTION_STATE_DISCONNECTED = 0, ESP_A2D_CONNECTION_STATE_CONNECTING,
    ESP_A2D_CONNECTION_STATE_CONNECTED, ESP_A2D_CONNECTION_STATE_DISCONNECTING,
} esp_a2d_connection_state_t;

typedef enum { ESP_A2D_DISC_RSN_NORMAL = 0, ESP_A2D_DISC_RSN_ABNORMAL } esp_a2d_disc_rsn_t;
typedef enum { ESP_A2D_AUDIO_STATE_SUSPEND = 0, ESP_A2D_AUDIO_STATE_STARTED } esp_a2d_audio_state_t;
typedef enum { ESP_A2D_MEDIA_CTRL_ACK_SUCCESS = 0, ESP_A2D_MEDIA_CTRL_ACK_FAILURE, ESP_A2D_MEDIA_CTRL_ACK_BUSY } esp_a2d_media_ctrl_ack_t;
typedef enum { ESP_A2D_MEDIA_CTRL_NONE = 0, ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY, ESP_A2D_MEDIA_CTRL_START, ESP_A2D_MEDIA_CTRL_SUSPEND } esp_a2d_media_ctrl_t;
typedef enum { ESP_A2D_DEINIT_SUCCESS = 0, ESP_A2D_INIT_SUCCESS } esp_a2d_init_state_t;
typedef enum {
    ESP_A2D_SEP_REG_SUCCESS = 0, ESP_A2D_SEP_REG_FAIL,
    ESP_A2D_SEP_REG_UNSUPPORTED, ESP_A2D_SEP_REG_INVALID_STATE,
} esp_a2d_sep_reg_state_t;
typedef enum { ESP_A2D_SET_SUCCESS = 0, ESP_A2D_SET_INVALID_PARAMS } esp_a2d_set_delay_value_state_t;

typedef struct { bool a2d_snk_inited; bool a2d_src_inited; uint8_t conn_num; } esp_a2d_profile_status_t;

typedef enum {
    ESP_A2D_CONNECTION_STATE_EVT = 0, ESP_A2D_AUDIO_STATE_EVT, ESP_A2D_AUDIO_CFG_EVT,
    ESP_A2D_MEDIA_CTRL_ACK_EVT, ESP_A2D_PROF_STATE_EVT, ESP_A2D_SEP_REG_STATE_EVT,
    ESP_A2D_SNK_PSC_CFG_EVT, ESP_A2D_SNK_SET_DELAY_VALUE_EVT, ESP_A2D_SNK_GET_DELAY_VALUE_EVT,
    ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT, ESP_A2D_REPORT_SNK_CODEC_CAPS_EVT,
    ESP_A2D_SRC_SET_PREF_MCC_EVT, ESP_A2D_REPORT_SNK_ALL_CODEC_CAPS_EVT,
} esp_a2d_cb_event_t;

typedef struct {
    uint16_t buff_size;
    uint16_t number_frame;
    uint32_t timestamp;
    uint16_t data_len;
    uint8_t  *data;
} esp_a2d_audio_buff_t;

typedef union {
    struct {
        esp_a2d_connection_state_t state;
        esp_bd_addr_t remote_bda;
        esp_a2d_conn_hdl_t conn_hdl;
        uint16_t audio_mtu;
        esp_a2d_disc_rsn_t disc_rsn;
    } conn_stat;
    struct {
        esp_a2d_audio_state_t state;
        esp_bd_addr_t remote_bda;
        esp_a2d_conn_hdl_t conn_hdl;
    } audio_stat;
    struct {
        esp_bd_addr_t remote_bda;
        esp_a2d_conn_hdl_t conn_hdl;
        esp_a2d_mcc_t mcc;
    } audio_cfg;
    struct { esp_a2d_media_ctrl_t cmd; esp_a2d_media_ctrl_ack_t status; } media_ctrl_stat;
    struct { esp_a2d_init_state_t init_state; } a2d_prof_stat;
    struct { uint8_t seid; esp_a2d_sep_reg_state_t reg_state; } a2d_sep_reg_stat;
    struct { uint16_t psc_mask; } a2d_psc_cfg_stat;
    struct { esp_a2d_set_delay_value_state_t set_state; uint16_t delay_value; } a2d_set_delay_value_stat;
    struct { uint16_t delay_value; } a2d_get_delay_value_stat;
    struct { uint16_t delay_value; } a2d_report_delay_value_stat;
    struct { esp_a2d_conn_hdl_t conn_hdl; esp_a2d_mcc_t mcc; } a2d_report_snk_codec_caps_stat;
    struct { esp_bt_status_t set_status; esp_a2d_conn_hdl_t conn_hdl; } a2d_set_pref_mcc_stat;
    struct { esp_a2d_conn_hdl_t conn_hdl; esp_a2d_sep_mcc_t *sep_mcc; uint8_t sep_num; } a2d_report_snk_all_codec_caps_stat;
} esp_a2d_cb_param_t;

typedef void (*esp_a2d_cb_t)(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
typedef void (*esp_a2d_sink_audio_data_cb_t)(esp_a2d_conn_hdl_t conn_hdl, esp_a2d_audio_buff_t *audio_buf);

static inline esp_a2d_audio_buff_t *esp_a2d_audio_buff_alloc(uint16_t size) {
    esp_a2d_audio_buff_t *b = (esp_a2d_audio_buff_t *)malloc(sizeof(*b));
    if (!b) return NULL;
    b->data = (uint8_t *)malloc(size);
    b->buff_size = size; b->data_len = 0; b->number_frame = 0; b->timestamp = 0;
    return b;
}
static inline void esp_a2d_audio_buff_free(esp_a2d_audio_buff_t *b) { if (b) { free(b->data); free(b); } }
static inline esp_err_t esp_a2d_register_callback(esp_a2d_cb_t cb) { (void)cb; return ESP_OK; }
static inline esp_err_t esp_a2d_source_init(void) { return ESP_OK; }
static inline esp_err_t esp_a2d_source_register_stream_endpoint(uint8_t seid, const esp_a2d_mcc_t *mcc) {
    (void)seid; (void)mcc; return ESP_OK;
}
static inline esp_err_t esp_a2d_source_audio_data_send(esp_a2d_conn_hdl_t h, esp_a2d_audio_buff_t *buf) {
    (void)h; (void)buf; return ESP_OK;
}
static inline esp_err_t esp_a2d_source_connect(esp_bd_addr_t bda) { (void)bda; return ESP_OK; }
static inline esp_err_t esp_a2d_source_disconnect(esp_bd_addr_t bda) { (void)bda; return ESP_OK; }
static inline esp_err_t esp_a2d_media_ctrl(esp_a2d_media_ctrl_t ctrl) { (void)ctrl; return ESP_OK; }
#endif
