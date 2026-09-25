#ifndef BT_LINK_H
#define BT_LINK_H

/**
 * @file bt_link.h
 * @brief Ponte de Controle e Comunicação com o Co-Processador Bluetooth (ESP32)
 *
 * Gerencia a troca de comandos via UART (115200 bps, 8N1) com o co-processador
 * dedicado `bt_companion` (transmissor Sony LDAC 24-bit / 96 kHz e SBC),
 * sincronização de volume absoluto via AVRCP e controle de reset de hardware (GPIO 12).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa a UART de controle inter-MCU e registra callbacks de amostragem.
 *
 * Configura o canal UART1 com o co-processador e monitora mudanças de taxa de amostragem
 * no driver I2S para informar o reamostrador do co-processador.
 */
void bt_link_init(void);

/**
 * @brief Registra a entrada "Bluetooth" no menu carrossel principal do sistema.
 */
void bt_link_register_menu_entry(void);

/**
 * @brief Gera um pulso de reset em nível baixo no pino EN (GPIO 12) do co-processador.
 */
void bt_link_reset_companion(void);

/**
 * @brief Envia comando de volume absoluto ao co-processador para sincronização AVRCP.
 *
 * @param[in] volume_percent Nível de volume em porcentagem (0 a 100%).
 */
void bt_link_send_volume(uint8_t volume_percent);

#ifdef __cplusplus
}
#endif

#endif // BT_LINK_H
