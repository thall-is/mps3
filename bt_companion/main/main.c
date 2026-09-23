// Companheiro de Bluetooth do mps3 - recebe PCM via I2S (slave) da placa
// principal (ESP32-S3), codifica no codec negociado com o peer (LDAC,
// aptX HD, aptX ou SBC, nessa ordem de preferencia) e transmite via
// A2DP usando o caminho de "codec externo" do ESP-IDF. Controle
// (parear, conectar, status, qualidade) chega via UART da placa
// principal - ver components/uart_ctrl.
//
// PIPELINE:
//   I2S RX (32-bit slots) -> audio_pipeline (acumula + extrai pra
//   int16) -> ldac_enc_process() / sbc_enc_process() /
//   aptx_enc_process() / aptx_hd_enc_process(), conforme o codec
//   negociado (ver bt_source_codec_cb_t) -> monta o cabecalho
//   apropriado (varia por codec - ver on_chunk_ready()) ->
//   bt_source_send_media_packet()
//
// IMPORTANTE: o SEP LDAC (e, por reaproveitar o mesmo mecanismo, aptX e
// aptX HD tambem) depende dos patches em esp-idf-patches/ estarem
// aplicados no seu checkout do ESP-IDF (ver README, secao "ESP-IDF
// travado + patches") - sem eles, bt_source_init() ainda sobe a pilha
// BT normalmente e o SEP SBC registra OK (nao depende de patch), mas
// os outros tres falham (ESP_A2D_SEP_REG_UNSUPPORTED no log) e so'
// sobra o fallback SBC.

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "driver/uart.h"

#include "uart_ctrl.h"
#include "i2s_input.h"
#include "bt_source.h"
#include "ldac_enc.h"
#include "sbc_enc.h"
#include "aptx_enc.h"
#include "aptx_hd_enc.h"
#include "a2dp_media_payload.h"
#include "audio_pipeline.h"
#include "bitrate_abr.h"

static const char *TAG = "bt_companion";

// --- Pinagem do companheiro -------------------------------------------
#define PIN_I2S_BCLK   26
#define PIN_I2S_WS     25
#define PIN_I2S_DIN    22
#define PIN_UART_TX    17
#define PIN_UART_RX    16
#define UART_PORT      UART_NUM_1

static uint32_t s_sample_rate = 44100;
static bool s_sample_rate_locked_by_uart = false;
static uart_link_state_t s_link_state = UART_LINK_IDLE;
static codec_preference_t s_codec_pref = CODEC_PREF_AUTO;

#define FRAMES_PER_CHUNK_MAX 256
static volatile size_t s_chunk_frames = 128; // valor pra 44.1/48kHz (o caso comum)

static SemaphoreHandle_t s_enc_mutex;
static ldac_enc_handle_t s_enc = NULL;
static sbc_enc_handle_t s_sbc_enc = NULL;
static aptx_enc_handle_t s_aptx_enc = NULL;
static aptx_hd_enc_handle_t s_aptx_hd_enc = NULL;
static volatile bt_source_codec_t s_negotiated_codec = BT_SOURCE_CODEC_NONE;

// Adaptacao automatica de qualidade do LDAC - comeca no nivel mais alto
// e cai pro mais baixo se a conexao Bluetooth nao aguentar (ver
// components/bitrate_abr). So' se aplica ao LDAC - os outros 3 codecs
// deste projeto sao taxa fixa (ver bitrate_abr.h).
static bitrate_abr_state_t s_ldac_abr;

static bool reopen_encoders(uint32_t sample_rate)
{
    xSemaphoreTake(s_enc_mutex, portMAX_DELAY);

    ESP_LOGI(TAG, "Reabrindo encoders para %u Hz (heap livre antes: %u bytes)",
             (unsigned)sample_rate, (unsigned)esp_get_free_heap_size());

    if (s_enc) ldac_enc_close(s_enc);
    // Comeca sempre no nivel mais alto (HIGH) - o ABR (ver
    // bitrate_abr_init logo abaixo) cuida de cair se o link nao
    // aguentar. Reabrir o encoder (troca de taxa de amostragem) reseta
    // o ABR de volta pro topo tambem - e' um recomeco razoavel, ja que
    // as condicoes do link nao tem relacao com a taxa de amostragem do
    // audio.
    ldac_enc_quality_t init_q = LDAC_ENC_QUALITY_STANDARD;
    s_enc = ldac_enc_open(679, sample_rate, init_q);
    bool ldac_ok = (s_enc != NULL);
    if (ldac_ok) {
        bitrate_abr_init(&s_ldac_abr, (bitrate_abr_tier_t)init_q);
        ESP_LOGI(TAG, "  LDAC encoder aberto OK (qualidade: STANDARD ~660kbps, heap livre: %u bytes)", (unsigned)esp_get_free_heap_size());
    }

    bool rate_44_48 = (sample_rate == 44100 || sample_rate == 48000);

    if (s_sbc_enc) sbc_enc_close(s_sbc_enc);
    s_sbc_enc = rate_44_48 ? sbc_enc_open(sample_rate, SBC_ENC_BITPOOL_HIGH) : NULL;

    if (s_aptx_enc) aptx_enc_close(s_aptx_enc);
    s_aptx_enc = rate_44_48 ? aptx_enc_open(sample_rate) : NULL;

    if (s_aptx_hd_enc) aptx_hd_enc_close(s_aptx_hd_enc);
    s_aptx_hd_enc = rate_44_48 ? aptx_hd_enc_open(sample_rate) : NULL;

    ESP_LOGI(TAG, "  Todos encoders abertos (heap livre final: %u bytes)", (unsigned)esp_get_free_heap_size());

    xSemaphoreGive(s_enc_mutex);

    if (!ldac_ok) ESP_LOGE(TAG, "Falha ao abrir encoder LDAC em %u Hz", (unsigned)sample_rate);
    if (rate_44_48 && !s_sbc_enc) ESP_LOGE(TAG, "Falha ao abrir encoder SBC em %u Hz", (unsigned)sample_rate);
    if (rate_44_48 && !s_aptx_enc) ESP_LOGE(TAG, "Falha ao abrir encoder aptX em %u Hz", (unsigned)sample_rate);
    if (rate_44_48 && !s_aptx_hd_enc) ESP_LOGE(TAG, "Falha ao abrir encoder aptX HD em %u Hz", (unsigned)sample_rate);
    return ldac_ok;
}

static const char *codec_name(bt_source_codec_t codec)
{
    switch (codec) {
        case BT_SOURCE_CODEC_LDAC:    return "LDAC";
        case BT_SOURCE_CODEC_APTX_HD: return "aptX HD";
        case BT_SOURCE_CODEC_APTX:    return "aptX";
        case BT_SOURCE_CODEC_SBC:     return "SBC";
        default:                      return "nenhum";
    }
}

static uart_link_state_t link_state_for_codec(bt_source_codec_t codec)
{
    switch (codec) {
        case BT_SOURCE_CODEC_LDAC:    return UART_LINK_CONNECTED_LDAC;
        case BT_SOURCE_CODEC_APTX_HD: return UART_LINK_CONNECTED_APTX_HD;
        case BT_SOURCE_CODEC_APTX:    return UART_LINK_CONNECTED_APTX;
        case BT_SOURCE_CODEC_SBC:     return UART_LINK_CONNECTED_SBC;
        default:                      return UART_LINK_IDLE;
    }
}

static uint8_t s_connected_bda[6] = {0};

static uint16_t codec_bitrate_kbps(bt_source_codec_t codec)
{
    switch (codec) {
        case BT_SOURCE_CODEC_LDAC:
            switch (s_ldac_abr.tier) {
                case BITRATE_ABR_TIER_HIGH:     return 990;
                case BITRATE_ABR_TIER_STANDARD: return 660;
                case BITRATE_ABR_TIER_MOBILE:   return 330;
                default: return 990;
            }
        case BT_SOURCE_CODEC_APTX_HD: return 576;
        case BT_SOURCE_CODEC_APTX:    return 352;
        case BT_SOURCE_CODEC_SBC:     return 328;
        default:                      return 0;
    }
}

static uint8_t codec_to_pref(bt_source_codec_t codec)
{
    switch (codec) {
        case BT_SOURCE_CODEC_LDAC:    return CODEC_PREF_LDAC;
        case BT_SOURCE_CODEC_APTX_HD: return CODEC_PREF_APTX_HD;
        case BT_SOURCE_CODEC_APTX:    return CODEC_PREF_APTX;
        case BT_SOURCE_CODEC_SBC:     return CODEC_PREF_SBC;
        default:                      return CODEC_PREF_AUTO;
    }
}

static void send_codec_info(bt_source_codec_t codec)
{
    uart_codec_info_t info = {
        .codec = codec_to_pref(codec),
        .bitrate_kbps = codec_bitrate_kbps(codec),
    };
    uart_ctrl_send(UART_EVT_CODEC_INFO, (const uint8_t *)&info, sizeof(info));
}

static void bt_codec_changed(bt_source_codec_t codec)
{
    s_negotiated_codec = codec;
    s_link_state = link_state_for_codec(codec);
    uint32_t codec_sr = bt_source_get_codec_sample_rate();
    if (codec_sr == 0) codec_sr = 44100;
    ESP_LOGI(TAG, "Codec ativo: %s (%u Hz, ~%u kbps)", codec_name(codec),
             (unsigned)codec_sr, (unsigned)codec_bitrate_kbps(codec));
    reopen_encoders(codec_sr);
    if (codec == BT_SOURCE_CODEC_SBC) {
        s_chunk_frames = 128;
    }
    if (codec == BT_SOURCE_CODEC_LDAC) {
        s_chunk_frames = 128;
        // Nova conexao (ou reconexao) negociou LDAC - comeca o ABR do
        // zero, no nivel mais alto, independente de onde ele tinha
        // ficado na conexao anterior (as condicoes do link de um peer
        // novo nao tem relacao com o anterior).
        xSemaphoreTake(s_enc_mutex, portMAX_DELAY);
        if (s_enc) {
            ldac_enc_set_quality(s_enc, LDAC_ENC_QUALITY_STANDARD);
            bitrate_abr_init(&s_ldac_abr, BITRATE_ABR_TIER_STANDARD);
        }
        xSemaphoreGive(s_enc_mutex);
    }
    uart_ctrl_send_status(s_link_state, s_connected_bda, s_sample_rate);
    send_codec_info(codec);
}

static void bt_volume_changed(uint8_t volume_percent)
{
    ESP_LOGI(TAG, "Fone alterou volume para %d%%. Notificando placa principal via UART...", volume_percent);
    uart_ctrl_send(UART_EVT_VOLUME_CHANGED, &volume_percent, 1);
}

static void bt_state_changed(bt_source_state_t state, const uint8_t bda[6])
{
    switch (state) {
        case BT_SOURCE_DISCONNECTED:
            s_link_state = UART_LINK_IDLE;
            memset(s_connected_bda, 0, sizeof(s_connected_bda));
            break;
        case BT_SOURCE_PAIRING:      s_link_state = UART_LINK_PAIRING; break;
        case BT_SOURCE_CONNECTING:   s_link_state = UART_LINK_CONNECTING; break;
        case BT_SOURCE_CONNECTED:
            s_link_state = link_state_for_codec(s_negotiated_codec == BT_SOURCE_CODEC_NONE
                                                     ? BT_SOURCE_CODEC_LDAC : s_negotiated_codec);
            if (bda) {
                memcpy(s_connected_bda, bda, 6);
                nvs_handle_t h;
                if (nvs_open("bt_cfg", NVS_READWRITE, &h) == ESP_OK) {
                    nvs_set_blob(h, "last_bda", bda, 6);
                    nvs_commit(h);
                    nvs_close(h);
                    ESP_LOGI(TAG, "Endereco do fone salvo na NVS");
                }
            }
            break;
    }
    uart_ctrl_send_status(s_link_state, bda ? bda : s_connected_bda, s_sample_rate);
}

static void on_bt_scan_result(const uint8_t bda[6], int8_t rssi, const char *name)
{
    uart_scan_result_t res = {0};
    memcpy(res.bda, bda, 6);
    res.rssi = rssi;
    strncpy(res.name, name, sizeof(res.name) - 1);
    uart_ctrl_send(UART_EVT_SCAN_RESULT, (const uint8_t *)&res, sizeof(res));
}

static void on_bt_scan_done(void)
{
    if (s_link_state == UART_LINK_SCANNING) {
        s_link_state = UART_LINK_IDLE;
    }
    uart_ctrl_send(UART_EVT_SCAN_COMPLETE, NULL, 0);
    uart_ctrl_send_status(s_link_state, NULL, s_sample_rate);
}

static void uart_rx_handler(uart_ctrl_cmd_t cmd, const uint8_t *payload, size_t len, void *ctx)
{
    (void)ctx;
    switch (cmd) {
        case UART_CMD_PING:
            uart_ctrl_send(UART_CMD_PONG, NULL, 0);
            break;

        case UART_CMD_SCAN_START:
            ESP_LOGI(TAG, "Comando UART: Iniciar Busca (Scan)");
            s_link_state = UART_LINK_SCANNING;
            uart_ctrl_send_status(s_link_state, NULL, s_sample_rate);
            bt_source_start_scan(on_bt_scan_result, on_bt_scan_done);
            break;

        case UART_CMD_SCAN_STOP:
            ESP_LOGI(TAG, "Comando UART: Parar Busca (Scan)");
            bt_source_stop_scan();
            s_link_state = UART_LINK_IDLE;
            uart_ctrl_send_status(s_link_state, NULL, s_sample_rate);
            break;

        case UART_CMD_CONNECT_ADDR:
            if (len >= 6) {
                ESP_LOGI(TAG, "Comando UART: Conectar a %02x:%02x:%02x:%02x:%02x:%02x",
                         payload[0], payload[1], payload[2], payload[3], payload[4], payload[5]);
                s_link_state = UART_LINK_CONNECTING;
                uart_ctrl_send_status(s_link_state, payload, s_sample_rate);
                bt_source_connect(payload);
            }
            break;

        case UART_CMD_PAIR_START:
            bt_source_start_pairing();
            break;

        case UART_CMD_PAIR_STOP:
            bt_source_stop_pairing();
            break;

        case UART_CMD_DISCONNECT:
            bt_source_disconnect();
            break;

        case UART_CMD_CONNECT_KNOWN: {
            uint8_t last_bda[6];
            size_t sz = 6;
            nvs_handle_t h;
            if (nvs_open("bt_cfg", NVS_READONLY, &h) == ESP_OK) {
                if (nvs_get_blob(h, "last_bda", last_bda, &sz) == ESP_OK && sz == 6) {
                    ESP_LOGI(TAG, "Reconectando ao ultimo fone salvo: %02x:%02x:%02x:%02x:%02x:%02x",
                             last_bda[0], last_bda[1], last_bda[2], last_bda[3], last_bda[4], last_bda[5]);
                    s_link_state = UART_LINK_CONNECTING;
                    uart_ctrl_send_status(s_link_state, last_bda, s_sample_rate);
                    bt_source_connect(last_bda);
                } else {
                    ESP_LOGW(TAG, "Nenhum fone salvo na NVS");
                }
                nvs_close(h);
            }
            break;
        }

        case UART_CMD_FORGET_ALL: {
            nvs_handle_t h;
            if (nvs_open("bt_cfg", NVS_READWRITE, &h) == ESP_OK) {
                nvs_erase_all(h);
                nvs_commit(h);
                nvs_close(h);
                ESP_LOGI(TAG, "Pareamentos apagados da NVS");
            }
            break;
        }

        case UART_CMD_SET_QUALITY:
            if (len >= 1 && s_enc && payload[0] < BITRATE_ABR_TIER_COUNT) {
                xSemaphoreTake(s_enc_mutex, portMAX_DELAY);
                ldac_enc_set_quality(s_enc, (ldac_enc_quality_t)payload[0]);
                bitrate_abr_init(&s_ldac_abr, (bitrate_abr_tier_t)payload[0]);
                xSemaphoreGive(s_enc_mutex);
                ESP_LOGI(TAG, "Qualidade LDAC ajustada manualmente: %d (ABR sincronizado)", payload[0]);
            }
            break;

        case UART_CMD_SET_SAMPLE_RATE:
            if (len >= 4) {
                uint32_t rate;
                memcpy(&rate, payload, 4);
                s_sample_rate = rate;
                s_sample_rate_locked_by_uart = true;
                ESP_LOGI(TAG, "Taxa de amostragem de entrada (S3) atualizada via UART: %u Hz (Travada)", (unsigned)rate);
            }
            break;

        case UART_CMD_GET_STATUS:
            uart_ctrl_send_status(s_link_state, NULL, s_sample_rate);
            break;

        case UART_CMD_SET_VOLUME:
            if (len >= 1) {
                ESP_LOGI(TAG, "Comando UART: Definir Volume: %d%%", payload[0]);
                bt_source_set_volume(payload[0]);
            }
            break;

        case UART_CMD_SET_CODEC:
            if (len >= 1 && payload[0] < CODEC_PREF_COUNT) {
                s_codec_pref = (codec_preference_t)payload[0];
                bt_source_set_codec_preference((uint8_t)s_codec_pref);
                // Salva na NVS para persistir entre reinicializacoes
                nvs_handle_t h;
                if (nvs_open("bt_cfg", NVS_READWRITE, &h) == ESP_OK) {
                    nvs_set_u8(h, "codec_pref", payload[0]);
                    nvs_commit(h);
                    nvs_close(h);
                }
                static const char *pref_names[] = {"Auto", "LDAC", "aptX HD", "aptX", "SBC"};
                ESP_LOGI(TAG, "Preferencia de codec alterada para: %s", pref_names[payload[0]]);
                // Se ja estiver conectado, desconecta para permitir renegociacao com a nova preferencia
                if (s_negotiated_codec != BT_SOURCE_CODEC_NONE) {
                    ESP_LOGI(TAG, "Desconectando para renegociar com nova preferencia...");
                    bt_source_disconnect();
                }
            }
            break;

        default:
            ESP_LOGW(TAG, "Comando UART desconhecido: 0x%02x", cmd);
            break;
    }
}

#define CODEC_OUT_CAP 679

typedef struct {
    uint8_t codec_out[CODEC_OUT_CAP];
    uint8_t packet[A2DP_MEDIA_HEADER_LEN + CODEC_OUT_CAP];
    int16_t pcm16[FRAMES_PER_CHUNK_MAX * 2];
    a2dp_media_stream_state_t stream_state;
} audio_task_ctx_t;

static volatile uint32_t s_pkts_ok_cnt = 0;
static volatile uint32_t s_pkts_fail_cnt = 0;
static volatile esp_err_t s_last_send_err = ESP_OK;

static void send_a2dp_chunk(const uint8_t *pkt, size_t len)
{
    esp_err_t ret = bt_source_send_media_packet(pkt, len);
    if (ret == ESP_OK) {
        s_pkts_ok_cnt++;
    } else {
        s_pkts_fail_cnt++;
        s_last_send_err = ret;
    }
}

static void on_chunk_ready(void *ctx_v, const int32_t *frames, size_t frame_count)
{
    audio_task_ctx_t *ctx = (audio_task_ctx_t *)ctx_v;
    bt_source_codec_t codec = s_negotiated_codec;

    if (codec == BT_SOURCE_CODEC_NONE) return;

    xSemaphoreTake(s_enc_mutex, portMAX_DELAY);

    switch (codec) {
        case BT_SOURCE_CODEC_LDAC: {
            if (!s_enc) break;
            int frame_num = 0;
            // Alimenta diretamente com o sinal de 32/24 bits (True 24-bit Hi-Res!)
            int n = ldac_enc_process_s32(s_enc, frames, frame_count, ctx->codec_out, sizeof(ctx->codec_out), &frame_num);
            if (n > 0 && frame_num > 0 && frame_num <= 0x0F) {
                int32_t peak = 0;
                for (size_t i = 0; i < frame_count * 2; i++) {
                    int32_t v = frames[i] < 0 ? -frames[i] : frames[i];
                    if (v > peak) peak = v;
                }

                uint32_t samples_in_packet = (uint32_t)frame_num * (uint32_t)ldac_enc_frame_samples(s_enc);

                if (a2dp_media_write_header(ctx->packet, sizeof(ctx->packet), &ctx->stream_state,
                                             (uint8_t)frame_num, samples_in_packet)) {
                    memcpy(ctx->packet + A2DP_MEDIA_HEADER_LEN, ctx->codec_out, (size_t)n);
                    esp_err_t send_ret = bt_source_send_media_packet(ctx->packet, A2DP_MEDIA_HEADER_LEN + (size_t)n);
                    s_last_send_err = send_ret;
                    if (send_ret == ESP_OK) {
                        s_pkts_ok_cnt++;
                    } else {
                        s_pkts_fail_cnt++;
                    }

                    static uint32_t s_last_diag_tick = 0;
                    uint32_t now_diag = (uint32_t)xTaskGetTickCount();
                    if (now_diag - s_last_diag_tick >= pdMS_TO_TICKS(2000)) {
                        s_last_diag_tick = now_diag;
                        ESP_LOGI(TAG, "[DIAG LDAC 24-bit] peak_pcm=%d, enc_len=%d, fn=%d, pkt_len=%u | sr=%u",
                                 (int)(peak >> 16), n, frame_num,
                                 (unsigned)(A2DP_MEDIA_HEADER_LEN + n),
                                 (unsigned)bt_source_get_codec_sample_rate());
                    }

                    // Reporta ao ABR
                    if (bitrate_abr_report(&s_ldac_abr, send_ret == ESP_OK)) {
                        ldac_enc_set_quality(s_enc, (ldac_enc_quality_t)s_ldac_abr.tier);
                        ESP_LOGW(TAG, "ABR do LDAC mudou de nivel: tier=%d (%s)",
                                 s_ldac_abr.tier, send_ret == ESP_OK ? "recuperando" : "degradando");
                        send_codec_info(codec);
                    }
                }
            }
            break;
        }

        case BT_SOURCE_CODEC_SBC: {
            if (!s_sbc_enc) break;
            #define SBC_FRAMES_PER_PACKET 5
            static uint8_t s_sbc_pack_buf[SBC_FRAMES_PER_PACKET * 128];
            static uint8_t s_sbc_frame_cnt = 0;
            static size_t s_sbc_pack_len = 0;

            audio_pipeline_extract16(frames, frame_count * 2, ctx->pcm16);
            int n = sbc_enc_process(s_sbc_enc, ctx->pcm16, ctx->codec_out, sizeof(ctx->codec_out));
            if (n > 0) {
                if (s_sbc_pack_len + (size_t)n <= sizeof(s_sbc_pack_buf)) {
                    memcpy(s_sbc_pack_buf + s_sbc_pack_len, ctx->codec_out, (size_t)n);
                    s_sbc_pack_len += (size_t)n;
                    s_sbc_frame_cnt++;
                }
                if (s_sbc_frame_cnt >= SBC_FRAMES_PER_PACKET) {
                    esp_err_t send_ret = bt_source_send_sbc_frames(s_sbc_pack_buf, s_sbc_pack_len,
                                                                   s_sbc_frame_cnt,
                                                                   (uint32_t)(s_sbc_frame_cnt * 128));
                    if (send_ret == ESP_OK) {
                        s_pkts_ok_cnt++;
                    } else {
                        s_pkts_fail_cnt++;
                        s_last_send_err = send_ret;
                    }
                    s_sbc_frame_cnt = 0;
                    s_sbc_pack_len = 0;
                }
            }
            break;
        }

        case BT_SOURCE_CODEC_APTX_HD: {
            if (!s_aptx_hd_enc) break;
            int n = aptx_hd_enc_process_s32(s_aptx_hd_enc, frames, frame_count, ctx->codec_out, sizeof(ctx->codec_out));
            if (n > 0) {
                if (a2dp_media_write_rtp_header_only(ctx->packet, sizeof(ctx->packet), &ctx->stream_state,
                                                      (uint32_t)frame_count)) {
                    memcpy(ctx->packet + A2DP_RTP_HEADER_LEN, ctx->codec_out, (size_t)n);
                    send_a2dp_chunk(ctx->packet, A2DP_RTP_HEADER_LEN + (size_t)n);
                }
            }
            break;
        }

        case BT_SOURCE_CODEC_APTX: {
            if (!s_aptx_enc) break;
            audio_pipeline_extract16(frames, frame_count * 2, ctx->pcm16);
            int n = aptx_enc_process(s_aptx_enc, ctx->pcm16, frame_count, ctx->codec_out, sizeof(ctx->codec_out));
            if (n > 0) {
                send_a2dp_chunk(ctx->codec_out, (size_t)n);
            }
            break;
        }

        default:
            break;
    }

    xSemaphoreGive(s_enc_mutex);
}

static void audio_task(void *arg)
{
    (void)arg;

    static int32_t raw[256 * 2];
    static int32_t chunk_backing[FRAMES_PER_CHUNK_MAX * 2];
    static audio_task_ctx_t ctx;
    static audio_pipeline_resampler_t s_resampler;
    static uint32_t s_active_in_rate = 0;
    static uint32_t s_active_out_rate = 0;
    static int32_t resampled[512 * 2];

    audio_pipeline_acc_t acc;
    size_t current_chunk_frames = s_chunk_frames;
    audio_pipeline_acc_init(&acc, chunk_backing, current_chunk_frames);
    memset(&ctx, 0, sizeof(ctx));

    TickType_t last_telemetry_tick = xTaskGetTickCount();
    uint32_t frames_in_window = 0;
    uint32_t timeouts_in_window = 0;

    while (1) {
        size_t wanted = s_chunk_frames;
        if (wanted != current_chunk_frames && wanted > 0 && wanted <= FRAMES_PER_CHUNK_MAX) {
            current_chunk_frames = wanted;
            audio_pipeline_acc_init(&acc, chunk_backing, current_chunk_frames);
        }

        int frames = i2s_input_read_frames(raw, 128, 1000);
        if (frames <= 0) {
            timeouts_in_window++;
        } else {
            frames_in_window += (uint32_t)frames;

            uint32_t out_rate = bt_source_get_codec_sample_rate();
            if (out_rate == 0) out_rate = 44100;

            uint32_t in_rate = s_sample_rate;
            if (in_rate == 0) in_rate = out_rate;

            if (in_rate != s_active_in_rate || out_rate != s_active_out_rate) {
                s_active_in_rate = in_rate;
                s_active_out_rate = out_rate;
                i2s_input_set_rate(in_rate);
                audio_pipeline_resampler_init(&s_resampler, in_rate, out_rate);
                ESP_LOGI(TAG, "Pipeline de audio: in=%u Hz -> out=%u Hz (%s)",
                         (unsigned)in_rate, (unsigned)out_rate,
                         in_rate == out_rate ? "Passthrough direto" : "Resampler ativo");
            }

            if (in_rate == out_rate) {
                audio_pipeline_acc_feed(&acc, raw, (size_t)frames, on_chunk_ready, &ctx);
            } else {
                size_t n_out = audio_pipeline_resample(&s_resampler, raw, (size_t)frames, resampled, 512);
                if (n_out > 0) {
                    audio_pipeline_acc_feed(&acc, resampled, n_out, on_chunk_ready, &ctx);
                }
            }
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - last_telemetry_tick) >= pdMS_TO_TICKS(2000)) {
            uint32_t elapsed_ms = (now - last_telemetry_tick) * portTICK_PERIOD_MS;
            last_telemetry_tick = now;
            uint32_t fps = (frames_in_window * 1000) / (elapsed_ms ? elapsed_ms : 1);
            uint32_t ok = s_pkts_ok_cnt;
            uint32_t fail = s_pkts_fail_cnt;
            s_pkts_ok_cnt = 0;
            s_pkts_fail_cnt = 0;
            frames_in_window = 0;

            // Auto-sincronizacao de seguranca do clock fisico I2S RX (so' se nao travado via UART)
            if (!s_sample_rate_locked_by_uart) {
                if (fps >= 46500 && fps <= 50000 && s_sample_rate != 48000) {
                    ESP_LOGI(TAG, "Clock I2S detectado fisicamente como 48000 Hz (fps=%u)", (unsigned)fps);
                    s_sample_rate = 48000;
                } else if (fps >= 42000 && fps < 46500 && s_sample_rate != 44100) {
                    ESP_LOGI(TAG, "Clock I2S detectado fisicamente como 44100 Hz (fps=%u)", (unsigned)fps);
                    s_sample_rate = 44100;
                } else if (fps >= 84000 && fps < 92000 && s_sample_rate != 88200) {
                    ESP_LOGI(TAG, "Clock I2S detectado fisicamente como 88200 Hz (fps=%u)", (unsigned)fps);
                    s_sample_rate = 88200;
                } else if (fps >= 92000 && fps <= 102000 && s_sample_rate != 96000) {
                    ESP_LOGI(TAG, "Clock I2S detectado fisicamente como 96000 Hz (fps=%u)", (unsigned)fps);
                    s_sample_rate = 96000;
                } else if (fps >= 180000 && fps <= 200000 && s_sample_rate != 192000) {
                    ESP_LOGI(TAG, "Clock I2S detectado fisicamente como 192000 Hz (fps=%u)", (unsigned)fps);
                    s_sample_rate = 192000;
                }
            }

            if (fps == 0) {
                ESP_LOGW(TAG, "[TELEMETRIA] I2S RX: 0 frames/s (Timeouts=%u) -> ALERTA: Verifique pinos BCLK(26), WS(25), DIN(22) e GND!",
                         (unsigned)timeouts_in_window);
            } else {
                uint32_t cur_out = bt_source_get_codec_sample_rate();
                ESP_LOGI(TAG, "[TELEMETRIA] I2S RX: %u fps | Codec: %s (%u Hz) | In: %u Hz%s | Mode: %s | Pkts: %u OK, %u Falhas (err: %s)",
                         (unsigned)fps,
                         codec_name(s_negotiated_codec),
                         (unsigned)(cur_out ? cur_out : 44100),
                         (unsigned)s_sample_rate,
                         s_sample_rate_locked_by_uart ? " (UART)" : "",
                         (s_sample_rate == cur_out) ? "PASSTHROUGH" : "RESAMPLING",
                         (unsigned)ok, (unsigned)fail,
                         esp_err_to_name(s_last_send_err));
            }
            timeouts_in_window = 0;
        }
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_enc_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_enc_mutex ? ESP_OK : ESP_ERR_NO_MEM);
    reopen_encoders(s_sample_rate);

    uart_ctrl_config_t uart_cfg = {
        .uart_num  = UART_PORT,
        .tx_gpio   = PIN_UART_TX,
        .rx_gpio   = PIN_UART_RX,
        .baud_rate = 115200,
        .rx_cb     = uart_rx_handler,
        .user_ctx  = NULL,
    };
    ESP_ERROR_CHECK(uart_ctrl_init(&uart_cfg) == 0 ? ESP_OK : ESP_FAIL);

    // Carrega preferencia de codec da NVS
    nvs_handle_t h_pref;
    if (nvs_open("bt_cfg", NVS_READONLY, &h_pref) == ESP_OK) {
        uint8_t pref_val = 0;
        if (nvs_get_u8(h_pref, "codec_pref", &pref_val) == ESP_OK && pref_val < CODEC_PREF_COUNT) {
            s_codec_pref = (codec_preference_t)pref_val;
            static const char *pref_names[] = {"Auto", "LDAC", "aptX HD", "aptX", "SBC"};
            ESP_LOGI(TAG, "Preferencia de codec carregada da NVS: %s", pref_names[pref_val]);
        }
        nvs_close(h_pref);
    }
    bt_source_set_codec_preference((uint8_t)s_codec_pref);

    ESP_ERROR_CHECK(bt_source_init("mps3-bt", bt_state_changed, bt_codec_changed));
    bt_source_set_volume_cb(bt_volume_changed);

    i2s_input_config_t i2s_cfg = {
        .bclk_gpio = PIN_I2S_BCLK,
        .ws_gpio   = PIN_I2S_WS,
        .din_gpio  = PIN_I2S_DIN,
    };
    ESP_ERROR_CHECK(i2s_input_init(&i2s_cfg));

    xTaskCreatePinnedToCore(audio_task, "audio_task", 8192, NULL, 18, NULL, 1);

    uint8_t last_bda[6];
    size_t sz = 6;
    nvs_handle_t h;
    if (nvs_open("bt_cfg", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "last_bda", last_bda, &sz) == ESP_OK && sz == 6) {
            ESP_LOGI(TAG, "Auto-reconectando ao ultimo fone: %02x:%02x:%02x:%02x:%02x:%02x",
                     last_bda[0], last_bda[1], last_bda[2], last_bda[3], last_bda[4], last_bda[5]);
            s_link_state = UART_LINK_CONNECTING;
            bt_source_connect(last_bda);
        }
        nvs_close(h);
    }

    ESP_LOGI(TAG, "Companheiro BT pronto (LDAC, aptX HD, aptX, SBC) - aguardando comandos UART da placa principal");
}
