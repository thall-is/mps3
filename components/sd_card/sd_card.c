#include "sd_card.h"
#include "pinos.h"

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"

static const char *TAG = "sd_card";
static sdmmc_card_t *s_card = NULL;

esp_err_t sd_card_init(void)
{
    esp_err_t ret;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 64 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    // Barramento SDMMC nativo de 4 vias a 40 MHz (High-Speed):
    // Proporciona taxas de leitura superiores a 15 MB/s, garantindo
    // vazão contínua para reprodução suave de arquivos FLAC 24-bit/96kHz.
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = PIN_SD_CLK;
    slot_config.cmd = PIN_SD_CMD;
    slot_config.d0  = PIN_SD_D0;
    slot_config.d1  = PIN_SD_D1;
    slot_config.d2  = PIN_SD_D2;
    slot_config.d3  = PIN_SD_D3;
    slot_config.width = 4;
    // O circuito possui resistores pull-up físicos externos de 10 kΩ em
    // todas as linhas de sinal (CMD, D0-D3), assegurando integridade e
    // tempos de subida rápidos para o clock de 40 MHz.

    ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Falha ao montar o sistema de arquivos no cartao SD.");
        } else {
            ESP_LOGE(TAG, "Falha ao inicializar o cartao SD (%s). Verifique a fiacao/pull-ups.",
                     esp_err_to_name(ret));
        }
        return ret;
    }

    sdmmc_card_print_info(stdout, s_card);

    uint64_t total_bytes = 0, free_bytes = 0;
    if (esp_vfs_fat_info(SD_MOUNT_POINT, &total_bytes, &free_bytes) == ESP_OK) {
        ESP_LOGI(TAG, "Cartao montado com sucesso: Total=%.2f GB | Livre=%.2f MB",
                 (double)total_bytes / (1024.0 * 1024.0 * 1024.0),
                 (double)free_bytes / (1024.0 * 1024.0));
    }
    return ESP_OK;
}

sdmmc_card_t* sd_card_get_handle(void)
{
    return s_card;
}

void sd_card_deinit(void)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        s_card = NULL;
    }
}