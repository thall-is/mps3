#ifndef BT_SOURCE_H
#define BT_SOURCE_H

// Fonte A2DP (transmissor) via Bluetooth classico (Bluedroid), usando o
// caminho de "codec externo" do ESP-IDF (CONFIG_BT_A2DP_USE_EXTERNAL_CODEC)
// pra registrar QUATRO Stream Endpoints, por ordem de prioridade
// (SEID menor = preferido primeiro):
//   SEID 0: LDAC     (melhor qualidade quando o peer suporta)
//   SEID 1: aptX HD  (24 bits, ~529kbps)
//   SEID 2: aptX     (16 bits, ~352kbps, taxa fixa)
//   SEID 3: SBC      (fallback obrigatorio do A2DP - todo receptor suporta)
// e enviar frames JA CODIFICADOS (o app faz o encode, nao o Bluedroid).
// Qual dos quatro acaba sendo usado depende do que o peer aceitar
// durante a negociacao AVDTP - ver bt_source_codec_cb_t.
//
// STATUS: O registro do SEP LDAC utiliza os patches oficiais do projeto
// aplicados à pilha Bluedroid (pasta esp-idf-patches/). Totalmente validado
// e operacional em hardware com fones comerciais em Sony LDAC (24-bit / 96 kHz
// e 44.1/48 kHz) com ABR dinâmico, além de SBC com Bitpool 53 de alta qualidade.
// (Nota técnica: os fluxos de handshake aptX/aptX HD não sincronizam de forma
// confiável com fones atuais e permanecem inoperantes nesta versão).

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BT_SOURCE_DISCONNECTED = 0,
    BT_SOURCE_PAIRING,
    BT_SOURCE_CONNECTING,
    BT_SOURCE_CONNECTED,
} bt_source_state_t;

typedef enum {
    BT_SOURCE_CODEC_NONE = 0,
    BT_SOURCE_CODEC_LDAC,
    BT_SOURCE_CODEC_APTX_HD,
    BT_SOURCE_CODEC_APTX,
    BT_SOURCE_CODEC_SBC,
} bt_source_codec_t;

typedef void (*bt_source_state_cb_t)(bt_source_state_t state, const uint8_t bda[6]);
// Chamado quando a negociacao do codec fecha (ESP_A2D_AUDIO_CFG_EVT) -
// diz qual dos SEPs registrados o peer conectado realmente aceitou.
// main.c usa isso pra escolher qual encoder alimentar em
// on_chunk_ready().
typedef void (*bt_source_codec_cb_t)(bt_source_codec_t codec);

typedef void (*bt_source_scan_res_cb_t)(const uint8_t bda[6], int8_t rssi, const char *name);
typedef void (*bt_source_scan_done_cb_t)(void);

esp_err_t bt_source_init(const char *device_name, bt_source_state_cb_t state_cb,
                          bt_source_codec_cb_t codec_cb);

esp_err_t bt_source_start_scan(bt_source_scan_res_cb_t res_cb, bt_source_scan_done_cb_t done_cb);
esp_err_t bt_source_stop_scan(void);
esp_err_t bt_source_start_pairing(void);
esp_err_t bt_source_stop_pairing(void);
esp_err_t bt_source_connect(const uint8_t bda[6]);
esp_err_t bt_source_disconnect(void);

// Envia um pacote de midia A2DP JA MONTADO pro peer conectado - o
// formato exato do pacote (com ou sem cabecalho RTP, com ou sem byte
// de frame_count) depende do codec negociado, ver
// components/bt_source/a2dp_media_payload.h. Chamado pela audio_task em
// main.c depois de codificar (ldac_enc_process() / sbc_enc_process() /
// aptx_enc_process() / aptx_hd_enc_process(), conforme
// bt_source_codec_cb_t) e montar o cabecalho apropriado. Nao bloqueia
// por muito tempo (repassa direto pra esp_a2d_source_audio_data_send());
// se nao houver conexao ativa, descarta e retorna
// ESP_ERR_INVALID_STATE (silencioso de proposito - audio_task nao para
// de rodar so' porque o BT ainda nao conectou).
esp_err_t bt_source_send_media_packet(const uint8_t *packet, size_t len);
esp_err_t bt_source_send_sbc_frames(const uint8_t *sbc_data, size_t len, uint8_t frame_count, uint32_t samples_in_packet);
bool bt_source_is_media_started(void);
uint32_t bt_source_get_codec_sample_rate(void);
void bt_source_set_codec_preference(uint8_t pref);

// Controle de Volume Absoluto (AVRCP Controller)
typedef void (*bt_source_volume_cb_t)(uint8_t volume_percent);
void bt_source_set_volume_cb(bt_source_volume_cb_t cb);
esp_err_t bt_source_set_volume(uint8_t volume_percent);
bool bt_source_is_avrc_connected(void);

#ifdef __cplusplus
}
#endif

#endif // BT_SOURCE_H
