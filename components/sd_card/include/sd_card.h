#ifndef SD_CARD_H
#define SD_CARD_H

/**
 * @file sd_card.h
 * @brief Driver de Inicialização e Montagem FatFS do Cartão MicroSD (SDMMC 4-Bit)
 *
 * Configura o periférico host SDMMC nativo do ESP32-S3 em modo de 4 vias a 40 MHz
 * (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7), aloca buffers DMA
 * e monta o sistema de arquivos FAT32 em `/sdcard`.
 */

#include "esp_err.h"
#include "sdmmc_cmd.h"

#define SD_MOUNT_POINT "/sdcard" /**< Ponto de montagem no VFS do ESP-IDF. */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa o barramento SDMMC de 4 vias a 40 MHz e monta a partição FatFS.
 *
 * @return 
 *   - ESP_OK: Cartão detectado, inicializado e montado em `/sdcard`.
 *   - ESP_ERR_TIMEOUT: Timeout na comunicação com o cartão.
 *   - ESP_FAIL: Falha ao montar o sistema de arquivos FatFS.
 */
esp_err_t sd_card_init(void);

/**
 * @brief Desmonta o sistema de arquivos e desativa o host SDMMC.
 *
 * Libera os pinos e o slot do cartão para permitir manipulação externa ou modo USB MSC.
 */
void sd_card_deinit(void);

/**
 * @brief Retorna o ponteiro para a estrutura de dados e capacidade do cartão.
 *
 * @return Ponteiro para `sdmmc_card_t` contendo velocidade, capacidade e setor do cartão.
 */
sdmmc_card_t *sd_card_get_handle(void);

#ifdef __cplusplus
}
#endif

#endif // SD_CARD_H