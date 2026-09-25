#ifndef UART_CTRL_H
#define UART_CTRL_H

/**
 * @file uart_ctrl.h
 * @brief Protocolo Binário de Controle UART Inter-MCU (ESP32-S3 ↔ ESP32 Companion)
 *
 * Implementa o protocolo de mensagens em quadros com byte de sincronismo `0xA5`,
 * tamanho de payload (0-250 bytes), identificador de comando e checksum `CRC8/Maxim`
 * (polinômio 0x8C refletido). Permite pareamento, seleção de codecs Bluetooth,
 * controle de volume absoluto e telemetria de taxa de amostragem.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Identificadores de comandos e eventos do protocolo UART.
 */
typedef enum {
    // Placa principal (S3) -> Companheiro (ESP32)
    UART_CMD_PING             = 0x01, /**< Teste de conectividade (resposta: PONG). */
    UART_CMD_PAIR_START       = 0x10, /**< Entra em modo de pareamento descobridor. */
    UART_CMD_PAIR_STOP        = 0x11, /**< Interrompe modo de pareamento. */
    UART_CMD_CONNECT_KNOWN    = 0x12, /**< Conecta ao último fone de ouvido pareado. */
    UART_CMD_DISCONNECT       = 0x13, /**< Desconecta do fone de ouvido atual. */
    UART_CMD_FORGET_ALL       = 0x14, /**< Apaga lista de dispositivos pareados na NVS. */
    UART_CMD_SET_QUALITY      = 0x15, /**< Qualidade de codificação (0=HQ, 1=SQ, 2=MQ). */
    UART_CMD_SET_SAMPLE_RATE  = 0x16, /**< Frequência de amostragem em Hz (uint32 LE). */
    UART_CMD_GET_STATUS       = 0x17, /**< Solicita status imediato do link Bluetooth. */
    UART_CMD_SCAN_START       = 0x18, /**< Inicia busca por fones Bluetooth disponíveis. */
    UART_CMD_SCAN_STOP        = 0x19, /**< Interrompe busca de fones. */
    UART_CMD_CONNECT_ADDR     = 0x1A, /**< Conecta ao fone com o endereço MAC especificado (6 bytes). */
    UART_CMD_SET_VOLUME       = 0x1B, /**< Ajusta volume absoluto no fone via AVRCP (0..100). */
    UART_CMD_SET_CODEC        = 0x1C, /**< Define a preferência de codec Bluetooth. */

    // Companheiro (ESP32) -> Placa principal (S3)
    UART_CMD_PONG             = 0x81, /**< Resposta ao comando PING. */
    UART_EVT_STATUS           = 0x90, /**< Notificação periódica de status (`uart_status_payload_t`). */
    UART_EVT_SCAN_RESULT      = 0x91, /**< Notificação de fone descoberto na varredura. */
    UART_EVT_SCAN_COMPLETE    = 0x92, /**< Varredura de fones concluída. */
    UART_EVT_VOLUME_CHANGED   = 0x93, /**< Volume alterado pelos botões físicos do fone. */
    UART_EVT_CODEC_INFO       = 0x94, /**< Notificação do codec negociado e taxa de bits. */
} uart_ctrl_cmd_t;

/**
 * @brief Preferências de codec Bluetooth selecionáveis pelo usuário.
 */
typedef enum {
    CODEC_PREF_AUTO     = 0,  /**< Negocia o melhor codec disponível (LDAC > SBC). */
    CODEC_PREF_LDAC     = 1,  /**< Força exclusivamente Sony LDAC 24-bit / 96 kHz. */
    CODEC_PREF_APTX_HD  = 2,  /**< Reservado para Qualcomm aptX HD. */
    CODEC_PREF_APTX     = 3,  /**< Reservado para Qualcomm aptX Standard. */
    CODEC_PREF_SBC      = 4,  /**< Força codec padrão universal SBC de alta qualidade. */
    CODEC_PREF_COUNT
} codec_preference_t;

/**
 * @brief Estados operacionais do enlace Bluetooth.
 */
typedef enum {
    UART_LINK_IDLE = 0,          /**< Sem rádio ativo, desconectado e inativo. */
    UART_LINK_PAIRING,           /**< Modo descobridor aguardando pareamento. */
    UART_LINK_SCANNING,          /**< Realizando varredura ativa de fones. */
    UART_LINK_CONNECTING,        /**< Conectando ao dispositivo selecionado. */
    UART_LINK_CONNECTED_SBC,     /**< Conectado e transmitindo via codec SBC. */
    UART_LINK_CONNECTED_LDAC,    /**< Conectado e transmitindo via Sony LDAC 24/96. */
    UART_LINK_CONNECTED_APTX,    /**< Conectado via aptX. */
    UART_LINK_CONNECTED_APTX_HD, /**< Conectado via aptX HD. */
} uart_link_state_t;

#pragma pack(push, 1)
/**
 * @brief Payload do evento de status (`UART_EVT_STATUS`).
 */
typedef struct {
    uint8_t  state;       /**< Estado atual do link (`uart_link_state_t`). */
    uint8_t  bt_addr[6];  /**< Endereço MAC do fone conectado (zerado se desconectado). */
    uint32_t sample_rate; /**< Frequência de amostragem ativa em Hz. */
} uart_status_payload_t;

/**
 * @brief Payload de resultado de varredura Bluetooth (`UART_EVT_SCAN_RESULT`).
 */
typedef struct {
    uint8_t bda[6];      /**< Endereço MAC do dispositivo descoberto. */
    int8_t  rssi;        /**< Nível de sinal medido em dBm (-128 a 0). */
    char    name[32];    /**< Nome legível do dispositivo (null-terminated). */
} uart_scan_result_t;

/**
 * @brief Payload de informações de codec e bitrate negociado (`UART_EVT_CODEC_INFO`).
 */
typedef struct {
    uint8_t  codec;        /**< Código do codec ativo (`codec_preference_t`). */
    uint16_t bitrate_kbps; /**< Taxa de bits efetiva em kbps (ex: 990, 660, 330, 328). */
} uart_codec_info_t;
#pragma pack(pop)

/**
 * @brief Callback invocado ao receber um quadro de comando ou evento válido com CRC íntegro.
 */
typedef void (*uart_ctrl_rx_cb_t)(uart_ctrl_cmd_t cmd, const uint8_t *payload, size_t len, void *user_ctx);

/**
 * @brief Configuração de inicialização da interface UART de controle.
 */
typedef struct {
    int uart_num;             /**< Número da porta UART do SoC (ex: UART_NUM_1). */
    int tx_gpio;              /**< Pino GPIO de transmissão TX (ex: 14). */
    int rx_gpio;              /**< Pino GPIO de recepção RX (ex: 13). */
    int baud_rate;            /**< Taxa de comunicação em bps (geralmente 115200). */
    uart_ctrl_rx_cb_t rx_cb;  /**< Função callback para despacho de mensagens recebidas. */
    void *user_ctx;           /**< Contexto opcional passado ao callback. */
} uart_ctrl_config_t;

/**
 * @brief Inicializa a porta UART e cria a tarefa receptora com decodificador de quadros.
 *
 * @param[in] cfg Ponteiro para a estrutura `uart_ctrl_config_t`.
 *
 * @return 0 em caso de sucesso; < 0 em caso de falha.
 */
int uart_ctrl_init(const uart_ctrl_config_t *cfg);

/**
 * @brief Empacota e transmite um quadro de comando ou evento com cabeçalho e CRC8.
 *
 * @param[in] cmd     Identificador do comando (`uart_ctrl_cmd_t`).
 * @param[in] payload Dados do pacote (ou NULL se não houver payload).
 * @param[in] len     Tamanho do payload em bytes (0 a 250).
 *
 * @return true se o quadro foi transmitido com sucesso; false caso contrário.
 *
 * @note Thread-safe. Protegido por mutex interno de transmissão.
 */
bool uart_ctrl_send(uart_ctrl_cmd_t cmd, const uint8_t *payload, size_t len);

/**
 * @brief Função utilitária para envio do status do link Bluetooth.
 *
 * @param[in] state       Estado operacional (`uart_link_state_t`).
 * @param[in] bt_addr     Endereço MAC de 6 bytes do fone conectado.
 * @param[in] sample_rate Taxa de amostragem em Hz.
 *
 * @return true se enviado com sucesso.
 */
bool uart_ctrl_send_status(uart_link_state_t state, const uint8_t bt_addr[6], uint32_t sample_rate);

#ifdef __cplusplus
}
#endif

#endif // UART_CTRL_H
