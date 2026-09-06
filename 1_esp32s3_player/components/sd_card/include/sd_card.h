#ifndef SD_CARD_H
#define SD_CARD_H

#include "esp_err.h"

#define SD_MOUNT_POINT "/sdcard"

#ifdef __cplusplus
extern "C" {
#endif

#include "sdmmc_cmd.h"

// Inicializa o barramento SDMMC (4-bit) e monta o cartao SD em
// SD_MOUNT_POINT.
esp_err_t sd_card_init(void);

// Desmonta o cartao (libera o slot SDMMC).
void sd_card_deinit(void);

// Retorna o ponteiro pro cartao
sdmmc_card_t* sd_card_get_handle(void);

#ifdef __cplusplus
}
#endif

#endif // SD_CARD_H