#include "bt_source.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "bt_source";

// SEID menor = maior prioridade de negociacao no modo Auto:
// LDAC (0) > aptX HD (1) > aptX (2) > SBC (3)
#define LDAC_SEID    0
#define APTX_HD_SEID 1
#define APTX_SEID    2
#define SBC_SEID     3

// Vendor ID / Codec ID de cada codec vendor (LDAC, aptX, aptX HD) no
// AVDTP - confirmados contra 3 fontes independentes: captura real de
// pacote Bluetooth (BlueZ AVDTP dissector), codigo de producao do
// Android (a2dp_vendor_*_encoder.cc) e a lista de constantes do
// proprio Android (tA2DP_CODEC_ID em packages/modules/Bluetooth).
#define LDAC_VENDOR_ID    0x0000012DUL // Sony Corporation
#define LDAC_CODEC_ID     0x00AA
#define APTX_VENDOR_ID    0x0000004FUL // APT Licensing Ltd. (Qualcomm)
#define APTX_CODEC_ID     0x0001
#define APTX_HD_VENDOR_ID 0x000000D7UL // Qualcomm
#define APTX_HD_CODEC_ID  0x0024

// Bitmask de taxa de amostragem (mesma convencao pros 3 codecs vendor)
#define SAMP_44100  0x20
#define SAMP_48000  0x10
#define SAMP_88200  0x08
#define SAMP_96000  0x04
// Bitmask/valor de modo de canal - LDAC e aptX/aptX-HD usam
// codificacoes DIFERENTES pro mesmo conceito "estereo" (confirmado:
// LDAC stereo=0x01, aptX/aptX-HD stereo=0x02 - ver captura real citada
// acima).
#define LDAC_CHMODE_STEREO 0x01
#define APTX_CHMODE_STEREO 0x02

// Bitpool maximo que anunciamos no SEP SBC - precisa bater com o que
// sbc_enc de fato usa (SBC_ENC_BITPOOL_HIGH em sbc_enc.h) pra' nao
// anunciar uma capacidade que o encoder nao entrega. Redefinido aqui
// (em vez de incluir sbc_enc.h) pra' este componente nao depender de
// sbc_enc so' por causa dessa constante.
#define SBC_ENC_BITPOOL_HIGH_VALUE 53

static bt_source_state_cb_t s_state_cb;
static bt_source_codec_cb_t s_codec_cb;
static bt_source_scan_res_cb_t s_scan_res_cb = NULL;
static bt_source_scan_done_cb_t s_scan_done_cb = NULL;
static bool s_scanning = false;
static bt_source_state_t s_state = BT_SOURCE_DISCONNECTED;
static uint8_t s_peer_bda[6];
static esp_a2d_conn_hdl_t s_conn_hdl;
static bool s_connected = false;
static volatile bool s_media_started = false;
static volatile uint32_t s_codec_sample_rate = 44100;

bool bt_source_is_media_started(void)
{
    return s_media_started;
}

uint32_t bt_source_get_codec_sample_rate(void)
{
    return s_codec_sample_rate;
}

extern void bta_av_co_set_codec_preference(uint8_t pref);

void bt_source_set_codec_preference(uint8_t pref)
{
    bta_av_co_set_codec_preference(pref);
}

static bt_source_volume_cb_t s_volume_cb = NULL;
static uint8_t s_avrc_tl = 0;
static volatile bool s_avrc_connected = false;
static uint8_t s_current_volume_val = 127; // 100% por padrao (0-127)

void bt_source_set_volume_cb(bt_source_volume_cb_t cb)
{
    s_volume_cb = cb;
}

bool bt_source_is_avrc_connected(void)
{
    return s_avrc_connected;
}

esp_err_t bt_source_set_volume(uint8_t volume_percent)
{
    if (volume_percent > 100) volume_percent = 100;
    uint8_t vol = (uint8_t)(((uint32_t)volume_percent * 127) / 100);
    s_current_volume_val = vol;
    if (!s_avrc_connected) return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "Definindo volume absoluto no fone: %d%% (AVRC=%d/127)", volume_percent, vol);
    return esp_avrc_ct_send_set_absolute_volume_cmd(s_avrc_tl++, vol);
}

static void avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch (event) {
        case ESP_AVRC_CT_CONNECTION_STATE_EVT:
            ESP_LOGI(TAG, "AVRCP CT conexao: %s", param->conn_stat.connected ? "CONECTADO" : "DESCONECTADO");
            s_avrc_connected = param->conn_stat.connected;
            if (s_avrc_connected) {
                ESP_LOGI(TAG, "Fone conectado ao AVRCP! Enviando comando para desmutar (volume %d/127)...", s_current_volume_val);
                esp_avrc_ct_send_set_absolute_volume_cmd(s_avrc_tl++, s_current_volume_val);
                esp_avrc_ct_send_register_notification_cmd(s_avrc_tl++, ESP_AVRC_RN_VOLUME_CHANGE, 0);
            }
            break;

        case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
            if (param->change_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
                uint8_t vol = param->change_ntf.event_parameter.volume; // 0-127
                s_current_volume_val = vol;
                uint8_t pct = (uint8_t)(((uint32_t)vol * 100 + 63) / 127);
                ESP_LOGI(TAG, "Notificacao de volume do fone: %d/127 (%d%%)", vol, pct);
                esp_avrc_ct_send_register_notification_cmd(s_avrc_tl++, ESP_AVRC_RN_VOLUME_CHANGE, 0);
                if (s_volume_cb) {
                    s_volume_cb(pct);
                }
            }
            break;

        case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT:
            ESP_LOGI(TAG, "Set Absolute Volume aceito pelo fone: %d/127", param->set_volume_rsp.volume);
            break;

        default:
            break;
    }
}

static void notify_state(bt_source_state_t state)
{
    s_state = state;
    if (s_state_cb) s_state_cb(state, s_peer_bda);
}

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
        case ESP_BT_GAP_DISC_RES_EVT: {
            uint8_t *eir = NULL;
            char dev_name[32] = {0};
            int8_t rssi = -128;
            uint32_t cod = 0;

            for (int i = 0; i < param->disc_res.num_prop; i++) {
                esp_bt_gap_dev_prop_t *p = param->disc_res.prop + i;
                switch (p->type) {
                    case ESP_BT_GAP_DEV_PROP_COD:
                        cod = *(uint32_t *)(p->val);
                        break;
                    case ESP_BT_GAP_DEV_PROP_RSSI:
                        rssi = *(int8_t *)(p->val);
                        break;
                    case ESP_BT_GAP_DEV_PROP_EIR:
                        eir = (uint8_t *)(p->val);
                        break;
                    case ESP_BT_GAP_DEV_PROP_BDNAME:
                        if (p->len > 0) {
                            int l = p->len < 31 ? p->len : 31;
                            memcpy(dev_name, p->val, l);
                            dev_name[l] = '\0';
                        }
                        break;
                    default:
                        break;
                }
            }

            if (eir && dev_name[0] == '\0') {
                uint8_t rmt_name_len = 0;
                uint8_t *rmt_name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &rmt_name_len);
                if (!rmt_name) {
                    rmt_name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &rmt_name_len);
                }
                if (rmt_name && rmt_name_len > 0) {
                    int l = rmt_name_len < 31 ? rmt_name_len : 31;
                    memcpy(dev_name, rmt_name, l);
                    dev_name[l] = '\0';
                }
            }

            if (dev_name[0] == '\0') {
                snprintf(dev_name, sizeof(dev_name), "BT-%02X:%02X:%02X",
                         param->disc_res.bda[3], param->disc_res.bda[4], param->disc_res.bda[5]);
            }

            ESP_LOGI(TAG, "Fone encontrado: %s [%02x:%02x:%02x:%02x:%02x:%02x] (RSSI %d, COD 0x%lx)",
                     dev_name,
                     param->disc_res.bda[0], param->disc_res.bda[1], param->disc_res.bda[2],
                     param->disc_res.bda[3], param->disc_res.bda[4], param->disc_res.bda[5],
                     (int)rssi, (unsigned long)cod);

            if (s_scan_res_cb) {
                s_scan_res_cb(param->disc_res.bda, rssi, dev_name);
            }
            break;
        }

        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: {
            if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
                ESP_LOGI(TAG, "Busca de dispositivos concluida");
                s_scanning = false;
                if (s_scan_done_cb) {
                    s_scan_done_cb();
                }
            } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
                ESP_LOGI(TAG, "Busca de dispositivos iniciada");
                s_scanning = true;
            }
            break;
        }

        case ESP_BT_GAP_AUTH_CMPL_EVT:
            if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "Pareado com %s", param->auth_cmpl.device_name);
            } else {
                ESP_LOGW(TAG, "Falha na autenticacao, status=%d", param->auth_cmpl.stat);
            }
            break;
        case ESP_BT_GAP_PIN_REQ_EVT: {
            esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
            break;
        }
        case ESP_BT_GAP_CFM_REQ_EVT:
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
            break;
        default:
            break;
    }
}

// Le' os 4 bytes de vendor_id (little-endian) do inicio de um CIE
// vendor de 8 bytes (o mesmo campo mcc.cie.ldac_info e' reaproveitado
// pra' LDAC/aptX/aptX HD - ver register_vendor_sep()).
static uint32_t read_vendor_id(const uint8_t *cie)
{
    return (uint32_t)cie[0] | ((uint32_t)cie[1] << 8) |
           ((uint32_t)cie[2] << 16) | ((uint32_t)cie[3] << 24);
}

static uint16_t read_codec_id(const uint8_t *cie)
{
    return (uint16_t)cie[4] | ((uint16_t)cie[5] << 8);
}

static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT:
            if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                memcpy(s_peer_bda, param->conn_stat.remote_bda, 6);
                s_conn_hdl = param->conn_stat.conn_hdl;
                s_connected = true;
                ESP_LOGI(TAG, "A2DP conectado (mtu=%d)", param->conn_stat.audio_mtu);
                notify_state(BT_SOURCE_CONNECTED);
                ESP_LOGI(TAG, "Verificando se A2DP media channel esta pronto...");
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
            } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                s_connected = false;
                s_media_started = false;
                ESP_LOGI(TAG, "A2DP desconectado");
                notify_state(BT_SOURCE_DISCONNECTED);
                if (s_codec_cb) s_codec_cb(BT_SOURCE_CODEC_NONE);
            }
            break;

        case ESP_A2D_MEDIA_CTRL_ACK_EVT: {
            uint8_t cmd = param->media_ctrl_stat.cmd;
            uint8_t stat = param->media_ctrl_stat.status;
            ESP_LOGI(TAG, "ESP_A2D_MEDIA_CTRL_ACK_EVT: cmd=%d stat=%d", cmd, stat);
            if (cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY) {
                if (stat == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                    ESP_LOGI(TAG, "Media channel pronto! Disparando ESP_A2D_MEDIA_CTRL_START...");
                    esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
                } else {
                    ESP_LOGW(TAG, "CHECK_SRC_RDY status=%d. Tentando START diretamente...", stat);
                    esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
                }
            } else if (cmd == ESP_A2D_MEDIA_CTRL_START) {
                if (stat == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                    s_media_started = true;
                    ESP_LOGI(TAG, ">>> A2DP MEDIA STREAM INICIADO! Pronto para transmitir audio <<<");
                } else {
                    s_media_started = false;
                    ESP_LOGE(TAG, "Falha ao iniciar media stream A2DP (status=%d)", stat);
                }
            }
            break;
        }

        case ESP_A2D_AUDIO_STATE_EVT:
            ESP_LOGI(TAG, "Estado de audio A2DP: %d (%s)", param->audio_stat.state,
                     param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED ? "STARTED" : "SUSPEND");
            if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
                s_media_started = true;
            } else {
                s_media_started = false;
            }
            break;

        case ESP_A2D_AUDIO_CFG_EVT: {
            // E' aqui que a gente descobre qual dos SEPs registrados o
            // peer de fato aceitou - so' depois disso audio_task() em
            // main.c sabe qual encoder usar. Pros 3 codecs vendor
            // (mcc.type == NON_A2DP), precisa olhar os bytes do CIE
            // pra' distinguir qual e' qual (todos compartilham o mesmo
            // "type" no AVDTP).
            bt_source_codec_t codec = BT_SOURCE_CODEC_NONE;
            const esp_a2d_mcc_t *mcc = &param->audio_cfg.mcc;

            if (mcc->type == ESP_A2D_MCT_NON_A2DP) {
                uint32_t vendor_id = read_vendor_id(mcc->cie.ldac_info);
                uint16_t codec_id = read_codec_id(mcc->cie.ldac_info);
                if (vendor_id == LDAC_VENDOR_ID && codec_id == LDAC_CODEC_ID) {
                    codec = BT_SOURCE_CODEC_LDAC;
                    uint8_t sr_mask = mcc->cie.ldac_info[6];
                    uint32_t sr = 44100;
                    if (sr_mask & 0x04) sr = 96000;
                    else if (sr_mask & 0x08) sr = 88200;
                    else if (sr_mask & 0x10) sr = 48000;
                    else if (sr_mask & 0x20) sr = 44100;
                    s_codec_sample_rate = sr;
                    ESP_LOGI(TAG, "LDAC negociado: freq=%u Hz (mask=0x%02x), ch=0x%02x",
                             (unsigned)sr, sr_mask, mcc->cie.ldac_info[7]);
                } else if (vendor_id == APTX_HD_VENDOR_ID && codec_id == APTX_HD_CODEC_ID) {
                    codec = BT_SOURCE_CODEC_APTX_HD;
                    uint8_t sr_mask = mcc->cie.ldac_info[6];
                    uint32_t sr = (sr_mask & 0x10) ? 48000 : 44100;
                    s_codec_sample_rate = sr;
                    ESP_LOGI(TAG, "aptX HD negociado: freq=%u Hz", (unsigned)sr);
                } else if (vendor_id == APTX_VENDOR_ID && codec_id == APTX_CODEC_ID) {
                    codec = BT_SOURCE_CODEC_APTX;
                    uint8_t sr_mask = mcc->cie.ldac_info[6];
                    uint32_t sr = (sr_mask & 0x10) ? 48000 : 44100;
                    s_codec_sample_rate = sr;
                    ESP_LOGI(TAG, "aptX negociado: freq=%u Hz", (unsigned)sr);
                } else {
                    ESP_LOGW(TAG, "Codec vendor desconhecido: vendor_id=0x%08x codec_id=0x%04x",
                              (unsigned)vendor_id, codec_id);
                }
            } else if (mcc->type == ESP_A2D_MCT_SBC) {
                codec = BT_SOURCE_CODEC_SBC;
                uint32_t sbc_sr = (mcc->cie.sbc_info.samp_freq & ESP_A2D_SBC_CIE_SF_44K) ? 44100 :
                                  (mcc->cie.sbc_info.samp_freq & ESP_A2D_SBC_CIE_SF_48K) ? 48000 : 44100;
                s_codec_sample_rate = sbc_sr;
                ESP_LOGI(TAG, "SBC negociado: freq=%u Hz (sf_mask=0x%02x), ch_mode=0x%02x, bitpool=[%d..%d]",
                         (unsigned)sbc_sr, mcc->cie.sbc_info.samp_freq,
                         mcc->cie.sbc_info.ch_mode,
                         mcc->cie.sbc_info.min_bitpool, mcc->cie.sbc_info.max_bitpool);
            } else {
                ESP_LOGW(TAG, "Codec negociado desconhecido (type=%d)", mcc->type);
            }

            static const char *names[] = {"nenhum", "LDAC", "aptX HD", "aptX", "SBC"};
            ESP_LOGI(TAG, "Codec negociado: %s", names[codec]);
            if (s_codec_cb) s_codec_cb(codec);
            if (!s_media_started) {
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
            }
            break;
        }

        case ESP_A2D_SEP_REG_STATE_EVT:
            // reg_state == ESP_A2D_SEP_REG_UNSUPPORTED pro SEID do LDAC
            // (0) significa que os patches do ESP-IDF (ver
            // esp-idf-patches/) nao foram aplicados ou nao pegaram - os
            // outros SEIDs (aptX HD, aptX, SBC) reaproveitam o mesmo
            // "case" ja' patcheado (todos sao NON_A2DP de 8 bytes) e
            // deveriam registrar OK junto com o LDAC.
            ESP_LOGI(TAG, "Registro de SEP: seid=%d estado=%d",
                     param->a2d_sep_reg_stat.seid, param->a2d_sep_reg_stat.reg_state);
            break;

        default:
            break;
    }
}

// Monta um CIE vendor generico de 8 bytes (vendor_id LE + codec_id LE +
// freq + chmode) - o mesmo layout serve pra' LDAC, aptX e aptX HD, so'
// muda os valores. Reaproveita o campo mcc.cie.ldac_info (adicionado no
// patch esp-idf-patches/0001) mesmo pra' aptX/aptX HD - o nome do campo
// e' so' historico (foi adicionado pensando em LDAC primeiro), o layout
// de bytes e' identico pros 3.
static void build_vendor_cie(esp_a2d_mcc_t *mcc, uint32_t vendor_id, uint16_t codec_id,
                              uint8_t samp_mask, uint8_t chmode)
{
    memset(mcc, 0, sizeof(*mcc));
    mcc->type = ESP_A2D_MCT_NON_A2DP;

    uint8_t *info = mcc->cie.ldac_info;
    info[0] = (uint8_t)(vendor_id & 0xFF);
    info[1] = (uint8_t)((vendor_id >> 8) & 0xFF);
    info[2] = (uint8_t)((vendor_id >> 16) & 0xFF);
    info[3] = (uint8_t)((vendor_id >> 24) & 0xFF);
    info[4] = (uint8_t)(codec_id & 0xFF);
    info[5] = (uint8_t)((codec_id >> 8) & 0xFF);
    info[6] = samp_mask;
    info[7] = chmode;
}

static esp_err_t register_ldac_sep(void)
{
    esp_a2d_mcc_t mcc;
    build_vendor_cie(&mcc, LDAC_VENDOR_ID, LDAC_CODEC_ID,
                      SAMP_96000 | SAMP_88200 | SAMP_48000 | SAMP_44100, LDAC_CHMODE_STEREO);
    return esp_a2d_source_register_stream_endpoint(LDAC_SEID, &mcc);
}

static esp_err_t register_aptx_hd_sep(void)
{
    esp_a2d_mcc_t mcc;
    build_vendor_cie(&mcc, APTX_HD_VENDOR_ID, APTX_HD_CODEC_ID,
                      SAMP_44100 | SAMP_48000, APTX_CHMODE_STEREO);
    return esp_a2d_source_register_stream_endpoint(APTX_HD_SEID, &mcc);
}

static esp_err_t register_aptx_sep(void)
{
    esp_a2d_mcc_t mcc;
    build_vendor_cie(&mcc, APTX_VENDOR_ID, APTX_CODEC_ID,
                      SAMP_44100 | SAMP_48000, APTX_CHMODE_STEREO);
    return esp_a2d_source_register_stream_endpoint(APTX_SEID, &mcc);
}

static esp_err_t register_sbc_sep(void)
{
    // SBC e' um "case" ja' nativo do switch em btc_av_reg_sep() (ver
    // esp-idf-patches/0003) - registrar isso NAO depende de nenhum
    // patch, funciona em qualquer ESP-IDF com
    // CONFIG_BT_A2DP_USE_EXTERNAL_CODEC=y.
    esp_a2d_mcc_t mcc = {0};
    mcc.type = ESP_A2D_MCT_SBC;

    mcc.cie.sbc_info.samp_freq  = ESP_A2D_SBC_CIE_SF_44K;
    mcc.cie.sbc_info.ch_mode    = ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO;
    mcc.cie.sbc_info.block_len  = ESP_A2D_SBC_CIE_BLOCK_LEN_16;
    mcc.cie.sbc_info.num_subbands = ESP_A2D_SBC_CIE_NUM_SUBBANDS_8;
    mcc.cie.sbc_info.alloc_mthd = ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS;
    mcc.cie.sbc_info.min_bitpool = 2;
    mcc.cie.sbc_info.max_bitpool = SBC_ENC_BITPOOL_HIGH_VALUE;

    return esp_a2d_source_register_stream_endpoint(SBC_SEID, &mcc);
}

esp_err_t bt_source_init(const char *device_name, bt_source_state_cb_t state_cb,
                          bt_source_codec_cb_t codec_cb)
{
    s_state_cb = state_cb;
    s_codec_cb = codec_cb;

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    bt_cfg.mode = ESP_BT_MODE_CLASSIC_BT;

    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "controller_init: %s", esp_err_to_name(ret)); return ret; }

    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "controller_enable: %s", esp_err_to_name(ret)); return ret; }

    ret = esp_bluedroid_init();
    if (ret != ESP_OK) { ESP_LOGE(TAG, "bluedroid_init: %s", esp_err_to_name(ret)); return ret; }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) { ESP_LOGE(TAG, "bluedroid_enable: %s", esp_err_to_name(ret)); return ret; }

    esp_bt_gap_set_device_name(device_name);
    esp_bt_gap_register_callback(gap_cb);

    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));

    esp_avrc_ct_init();
    esp_avrc_ct_register_callback(avrc_ct_cb);

    esp_a2d_register_callback(a2d_cb);
    esp_a2d_source_init();

    // Registro dos SEPs tem que acontecer com a maquina de estados do
    // A2DP em BTC_AV_STATE_IDLE (antes de qualquer tentativa de
    // conexao). Registramos os 4 codecs em ordem de prioridade
    // (SEID menor = preferido primeiro pelo Bluedroid na negociacao
    // AVDTP). O peer (fone/caixa) aceita o primeiro SEP que ele
    // suportar - se suportar LDAC, usa LDAC; senao tenta aptX HD;
    // senao aptX; senao SBC (fallback universal obrigatorio).
    // Depende de CONFIG_BT_A2DP_SEP_NUM_MAX=4 no sdkconfig e dos
    // 3 patches em esp-idf-patches/ estarem aplicados no ESP-IDF.
    struct { const char *name; uint8_t seid; esp_err_t (*fn)(void); } seps[] = {
        {"LDAC",    LDAC_SEID,    register_ldac_sep},
        {"aptX HD", APTX_HD_SEID, register_aptx_hd_sep},
        {"aptX",    APTX_SEID,    register_aptx_sep},
        {"SBC",     SBC_SEID,     register_sbc_sep},
    };
    for (size_t i = 0; i < sizeof(seps) / sizeof(seps[0]); i++) {
        esp_err_t sep_ret = seps[i].fn();
        if (sep_ret != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao registrar SEP %s: %s", seps[i].name, esp_err_to_name(sep_ret));
        } else {
            ESP_LOGI(TAG, "SEP %s registrado com sucesso (SEID %d)", seps[i].name, (int)seps[i].seid);
        }
    }

    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

    ESP_LOGI(TAG, "Bluetooth A2DP source inicializado como \"%s\" (LDAC > aptX HD > aptX > SBC)", device_name);
    return ESP_OK;
}

esp_err_t bt_source_start_scan(bt_source_scan_res_cb_t res_cb, bt_source_scan_done_cb_t done_cb)
{
    s_scan_res_cb = res_cb;
    s_scan_done_cb = done_cb;
    if (s_scanning) {
        esp_bt_gap_cancel_discovery();
    }
    // 10 * 1.28s = ~12.8s de busca, maximo de respostas ilimitado (0)
    return esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
}

esp_err_t bt_source_stop_scan(void)
{
    if (!s_scanning) return ESP_OK;
    return esp_bt_gap_cancel_discovery();
}

esp_err_t bt_source_start_pairing(void)
{
    notify_state(BT_SOURCE_PAIRING);
    return esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
}

esp_err_t bt_source_stop_pairing(void)
{
    if (s_state == BT_SOURCE_PAIRING) notify_state(BT_SOURCE_DISCONNECTED);
    return esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
}

esp_err_t bt_source_connect(const uint8_t bda[6])
{
    if (!bda) return ESP_ERR_INVALID_ARG;
    if (s_scanning) {
        esp_bt_gap_cancel_discovery();
        s_scanning = false;
    }
    memcpy(s_peer_bda, bda, 6);
    notify_state(BT_SOURCE_CONNECTING);
    return esp_a2d_source_connect((uint8_t *)bda);
}

esp_err_t bt_source_disconnect(void)
{
    return esp_a2d_source_disconnect(s_peer_bda);
}

esp_err_t bt_source_send_media_packet(const uint8_t *packet, size_t len)
{
    if (!s_connected) return ESP_ERR_INVALID_STATE;
    if (!packet || len == 0) return ESP_ERR_INVALID_ARG;

    if (!s_media_started) {
        static uint32_t s_last_retry_tick = 0;
        uint32_t now = (uint32_t)xTaskGetTickCount();
        if (now - s_last_retry_tick > pdMS_TO_TICKS(1000)) {
            s_last_retry_tick = now;
            ESP_LOGW(TAG, "Audio pronto mas A2DP media stream nao iniciado. Disparando START...");
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
        }
        return ESP_ERR_INVALID_STATE;
    }

    esp_a2d_audio_buff_t *buf = esp_a2d_audio_buff_alloc((uint16_t)len);
    if (!buf) {
        static uint32_t s_last_alloc_fail = 0;
        uint32_t now = (uint32_t)xTaskGetTickCount();
        if (now - s_last_alloc_fail > pdMS_TO_TICKS(2000)) {
            s_last_alloc_fail = now;
            ESP_LOGE(TAG, "esp_a2d_audio_buff_alloc falhou para tamanho %u", (unsigned)len);
        }
        return ESP_ERR_NO_MEM;
    }

    memcpy(buf->data, packet, len);
    buf->data_len = (uint16_t)len;
    buf->number_frame = 0;
    buf->timestamp = 0;

    esp_err_t ret = esp_a2d_source_audio_data_send(s_conn_hdl, buf);
    if (ret != ESP_OK) {
        esp_a2d_audio_buff_free(buf);
        static uint32_t s_last_fail_tick = 0;
        uint32_t now = (uint32_t)xTaskGetTickCount();
        if (now - s_last_fail_tick > pdMS_TO_TICKS(2000)) {
            s_last_fail_tick = now;
            ESP_LOGW(TAG, "audio_data_send falhou (%s) - pacote descartado", esp_err_to_name(ret));
        }
    }
    return ret;
}

static uint32_t s_sbc_rtp_timestamp = 0;

esp_err_t bt_source_send_sbc_frames(const uint8_t *sbc_data, size_t len, uint8_t frame_count, uint32_t samples_in_packet)
{
    if (!s_connected) return ESP_ERR_INVALID_STATE;
    if (!sbc_data || len == 0) return ESP_ERR_INVALID_ARG;

    if (!s_media_started) {
        static uint32_t s_last_retry_tick = 0;
        uint32_t now = (uint32_t)xTaskGetTickCount();
        if (now - s_last_retry_tick > pdMS_TO_TICKS(1000)) {
            s_last_retry_tick = now;
            ESP_LOGW(TAG, "Audio pronto mas A2DP media stream nao iniciado. Disparando START...");
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
        }
        return ESP_ERR_INVALID_STATE;
    }

    esp_a2d_audio_buff_t *buf = esp_a2d_audio_buff_alloc((uint16_t)len);
    if (!buf) return ESP_ERR_NO_MEM;

    memcpy(buf->data, sbc_data, len);
    buf->data_len = (uint16_t)len;
    buf->number_frame = frame_count;
    buf->timestamp = s_sbc_rtp_timestamp;
    s_sbc_rtp_timestamp += samples_in_packet;

    esp_err_t ret = esp_a2d_source_audio_data_send(s_conn_hdl, buf);
    if (ret != ESP_OK) {
        esp_a2d_audio_buff_free(buf);
        static uint32_t s_last_fail_tick = 0;
        uint32_t now = (uint32_t)xTaskGetTickCount();
        if (now - s_last_fail_tick > pdMS_TO_TICKS(2000)) {
            s_last_fail_tick = now;
            ESP_LOGW(TAG, "audio_data_send (SBC) falhou (%s) - pacote descartado", esp_err_to_name(ret));
        }
    }
    return ret;
}
