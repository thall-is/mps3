#ifndef USB_MANAGER_H
#define USB_MANAGER_H

/**
 * @file usb_manager.h
 * @brief Gerenciador da Pilha USB Nativa TinyUSB (CDC, UAC2 DAC e MSC)
 *
 * Controla os perfis USB do periférico OTG nativo do ESP32-S3 (GPIO 19 D-, GPIO 20 D+).
 * Permite chaveamento dinâmico exclusivo entre gravação serial CDC (1200 bps touch / bootloader),
 * placa de som estéreo USB UAC2 24-bit de baixa latência e unidade de armazenamento
 * em massa USB Mass Storage (MSC) montando o cartão MicroSD.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Perfis operacionais USB selecionáveis pelo usuário.
 */
typedef enum {
    USB_MODE_NONE = 0, /**< Modo ocioso ou desconectado. */
    USB_MODE_MSC  = 1, /**< Unidade de disco externo USB Mass Storage. */
    USB_MODE_DAC  = 2, /**< Placa de som USB UAC2 estéreo 24-bit. */
} usb_mode_t;

/**
 * @brief Inicializa a pilha TinyUSB Device e configura os descritores USB.
 *
 * @return ESP_OK se o periférico OTG foi configurado com sucesso.
 */
esp_err_t usb_manager_init(void);

/**
 * @brief Aplica e comuta para o perfil USB selecionado pelo usuário.
 *
 * @param[in] mode Perfil operacional desejado (`USB_MODE_MSC` ou `USB_MODE_DAC`).
 */
void usb_manager_set_mode(usb_mode_t mode);

/**
 * @brief Informa se o cabo USB de dados está conectado e enumerado por um Host.
 *
 * @return true se conectado ao computador; false caso contrário.
 */
bool usb_manager_is_connected(void);

/**
 * @brief Transmite comandos de controle de mídia HID Consumer ao computador.
 *
 * @param[in] command Identificador do comando:
 *                    - 0: Play / Pause
 *                    - 1: Próxima Faixa (Next Track)
 *                    - 2: Faixa Anterior (Previous Track)
 *                    - 3: Aumentar Volume (Volume Up)
 *                    - 4: Diminuir Volume (Volume Down)
 */
void usb_manager_send_hid(int command);

/**
 * @brief Reinicia a CPU no modo de gravação USB Serial/JTAG (Download Bootloader da ROM).
 */
void usb_manager_enter_bootloader(void);

/**
 * @brief Retorna a taxa de amostragem negociada com o computador no modo DAC UAC2.
 *
 * @return Frequência em Hz (normalmente 44100 ou 48000 Hz).
 */
uint32_t usb_manager_get_sample_rate(void);

/**
 * @brief Retorna o contador total de pacotes de áudio isócronos recebidos do PC.
 *
 * @return Quantidade de pacotes UAC2 processados.
 */
uint32_t usb_manager_get_pkt_count(void);

/**
 * @brief Retorna a última amostra escalar de áudio recebida para cálculo de VU-meter.
 *
 * @return Amostra PCM em 32 bits.
 */
int32_t usb_manager_get_last_sample(void);

/**
 * @brief Informa se o computador está transmitindo áudio ativamente no momento.
 *
 * @return true se houve pacotes de áudio recebidos nos últimos 250 ms; false se o streaming estiver parado.
 */
bool usb_manager_is_streaming(void);

#ifdef __cplusplus
}
#endif

#endif // USB_MANAGER_H
