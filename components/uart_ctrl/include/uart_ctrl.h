#ifndef UART_CTRL_H
#define UART_CTRL_H

// Protocolo de controle entre a placa principal (ESP32-S3) e o
// companheiro de Bluetooth. Formato do quadro, do primeiro ao ultimo
// byte:
//
//   [0]     0xA5              byte de sincronismo
//   [1]     LEN               tamanho do payload (0-250)
//   [2]     CMD               ver uart_ctrl_cmd_t
//   [3..]   PAYLOAD           LEN bytes
//   [3+LEN] CRC8              CRC8/Maxim (poly 0x8C refletido) de
//                              CMD+PAYLOAD (nao inclui SYNC nem LEN)
//
// Multibyte no payload = little-endian. Sem escaping de byte 0xA5 dentro
// do payload - a integridade e' garantida pelo CRC8 + resync automatico.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    // Placa principal -> companheiro
    UART_CMD_PING             = 0x01, // payload: vazio
    UART_CMD_PAIR_START       = 0x10, // payload: vazio - entra em modo descobrivel
    UART_CMD_PAIR_STOP        = 0x11, // payload: vazio
    UART_CMD_CONNECT_KNOWN    = 0x12, // payload: vazio - conecta ao ultimo dispositivo pareado
    UART_CMD_DISCONNECT       = 0x13, // payload: vazio
    UART_CMD_FORGET_ALL       = 0x14, // payload: vazio - apaga pareamentos salvos
    UART_CMD_SET_QUALITY      = 0x15, // payload: 1 byte (0=HQ,1=SQ,2=MQ)
    UART_CMD_SET_SAMPLE_RATE  = 0x16, // payload: 4 bytes (uint32 LE) - Hz, bate com I2S
    UART_CMD_GET_STATUS       = 0x17, // payload: vazio - companheiro responde com EVT_STATUS
    UART_CMD_SCAN_START       = 0x18, // payload: vazio - inicia busca por fones
    UART_CMD_SCAN_STOP        = 0x19, // payload: vazio - interrompe busca
    UART_CMD_CONNECT_ADDR     = 0x1A, // payload: 6 bytes BDA - conecta ao fone escolhido
    UART_CMD_SET_VOLUME       = 0x1B, // payload: 1 byte (0..100) - define volume absoluto no fone
    UART_CMD_SET_CODEC        = 0x1C, // payload: 1 byte (codec_preference_t) - preferencia de codec

    // Companheiro -> placa principal
    UART_CMD_PONG             = 0x81, // resposta a PING
    UART_EVT_STATUS           = 0x90, // ver uart_status_payload_t
    UART_EVT_SCAN_RESULT      = 0x91, // ver uart_scan_result_t
    UART_EVT_SCAN_COMPLETE    = 0x92, // payload: vazio - busca concluida
    UART_EVT_VOLUME_CHANGED   = 0x93, // payload: 1 byte (0..100) - volume alterado pelo fone
    UART_EVT_CODEC_INFO       = 0x94, // ver uart_codec_info_t - codec ativo + bitrate
} uart_ctrl_cmd_t;

// Preferencia de codec selecionada pelo usuario no menu do S3
typedef enum {
    CODEC_PREF_AUTO     = 0,  // Negocia o melhor disponivel (LDAC > aptX HD > aptX > SBC)
    CODEC_PREF_LDAC     = 1,  // Registra apenas LDAC + SBC (fallback)
    CODEC_PREF_APTX_HD  = 2,  // Registra apenas aptX HD + SBC
    CODEC_PREF_APTX     = 3,  // Registra apenas aptX + SBC
    CODEC_PREF_SBC      = 4,  // Registra apenas SBC
    CODEC_PREF_COUNT
} codec_preference_t;

typedef enum {
    UART_LINK_IDLE = 0,          // sem radio ativo, nao pareando nem conectado
    UART_LINK_PAIRING,           // discoverable legado
    UART_LINK_SCANNING,          // buscando fones disponiveis
    UART_LINK_CONNECTING,        // conectando ao fone selecionado
    UART_LINK_CONNECTED_SBC,     // conectado, transmitindo em SBC
    UART_LINK_CONNECTED_LDAC,    // conectado, transmitindo em LDAC
    UART_LINK_CONNECTED_APTX,    // conectado, transmitindo em aptX
    UART_LINK_CONNECTED_APTX_HD, // conectado, transmitindo em aptX HD
} uart_link_state_t;

#pragma pack(push, 1)
typedef struct {
    uint8_t  state;       // uart_link_state_t
    uint8_t  bt_addr[6];  // endereco do peer conectado (zerado se nao conectado)
    uint32_t sample_rate; // taxa atual configurada
} uart_status_payload_t;

typedef struct {
    uint8_t bda[6];      // MAC do dispositivo
    int8_t  rssi;        // Sinal (-128 a 0)
    char    name[32];    // Nome do fone (null-terminated)
} uart_scan_result_t;

typedef struct {
    uint8_t  codec;       // codec_preference_t (1=LDAC, 2=aptX_HD, 3=aptX, 4=SBC, 0=auto/nenhum)
    uint16_t bitrate_kbps;// taxa negociada em kbps (ex: 990, 660, 330, 352, 328)
} uart_codec_info_t;
#pragma pack(pop)

// Chamado pela tarefa interna do uart_ctrl quando um quadro completo e valido chega.
typedef void (*uart_ctrl_rx_cb_t)(uart_ctrl_cmd_t cmd, const uint8_t *payload, size_t len, void *user_ctx);

typedef struct {
    int uart_num;
    int tx_gpio;
    int rx_gpio;
    int baud_rate;
    uart_ctrl_rx_cb_t rx_cb;
    void *user_ctx;
} uart_ctrl_config_t;

// Inicializa o driver UART e sobe a tarefa de recepcao. Retorna 0 em sucesso.
int uart_ctrl_init(const uart_ctrl_config_t *cfg);

// Monta e envia um quadro. Thread-safe (protegido por mutex).
bool uart_ctrl_send(uart_ctrl_cmd_t cmd, const uint8_t *payload, size_t len);

// Atalho para envio de status
bool uart_ctrl_send_status(uart_link_state_t state, const uint8_t bt_addr[6], uint32_t sample_rate);

#ifdef __cplusplus
}
#endif

#endif // UART_CTRL_H

