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
        .max_files = 16,
        .allocation_unit_size = 64 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    // Inicializa no modo High-Speed (40 MHz) para habilitar suporte a frequencias
    // elevadas de 0..50 MHz no cartao. Permite comutacao dinamica entre 20, 26.6, 32 e 40 MHz.
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    // Pre-aloca buffer DMA dedicado (4 KB = 8 setores) para operacoes FATFS/desalinhadas.
    // Isso impede de forma definitiva que o driver sdmmc tente alocar dinamicamente sob escassez de memoria (erro 0x101).
    host.dma_aligned_buffer = heap_caps_malloc(4096, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (host.dma_aligned_buffer) {
        host.unaligned_multi_block_rw_max_chunk_size = 8;
        ESP_LOGI(TAG, "Buffer DMA dedicado SDMMC alocado com sucesso (4 KB)");
    } else {
        ESP_LOGW(TAG, "Nao foi possivel pre-alocar buffer DMA dedicado SDMMC");
    }

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = PIN_SD_CLK;
    slot_config.cmd = PIN_SD_CMD;
    slot_config.d0  = PIN_SD_D0;
    slot_config.d1  = PIN_SD_D1;
    slot_config.d2  = PIN_SD_D2;
    slot_config.d3  = PIN_SD_D3;
    slot_config.width = 4;
    // O circuito possui resistores pull-up físicos externos de 10 kΩ em
    // todas as linhas de sinal (CMD, D0-D3).

    ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        if (host.dma_aligned_buffer) {
            heap_caps_free(host.dma_aligned_buffer);
        }
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Falha ao montar o sistema de arquivos no cartao SD.");
        } else {
            ESP_LOGE(TAG, "Falha ao inicializar o cartao SD (%s). Verifique a fiacao/pull-ups.",
                     esp_err_to_name(ret));
        }
        return ret;
    }

    sdmmc_card_print_info(stdout, s_card);

    // Ajusta frequencia padrao inicial para 20 MHz (livre de interferencia com Wi-Fi)
    sd_card_set_frequency(SDMMC_FREQ_DEFAULT);

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

esp_err_t sd_card_set_frequency(uint32_t freq_khz)
{
    if (!s_card) return ESP_ERR_INVALID_STATE;
    esp_err_t err = sdmmc_host_set_card_clk(s_card->host.slot, freq_khz);
    if (err == ESP_OK) {
        s_card->max_freq_khz = freq_khz;
        ESP_LOGI(TAG, "Clock SDMMC alterado para %u kHz (%.2f MHz)", (unsigned)freq_khz, (double)freq_khz / 1000.0);
    } else {
        ESP_LOGE(TAG, "Falha ao alterar clock SDMMC para %u kHz (err=0x%x)", (unsigned)freq_khz, err);
    }
    return err;
}

uint32_t sd_card_get_frequency(void)
{
    if (!s_card) return 0;
    return s_card->max_freq_khz;
}

void sd_card_deinit(void)
{
    if (s_card) {
        void *dma_buf = s_card->host.dma_aligned_buffer;
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        if (dma_buf) {
            heap_caps_free(dma_buf);
        }
        s_card = NULL;
    }
}