#include "i2s_input.h"

#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

static const char *TAG = "i2s_input";
static i2s_chan_handle_t s_rx_handle = NULL;

esp_err_t i2s_input_init(const i2s_input_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
    // Buffer generoso: como slave, a gente nao controla quando os dados
    // chegam (segue o clock da placa principal), entao vale a pena um
    // colchao de DMA maior pra' absorver qualquer atraso da tarefa que
    // consome (codificar LDAC + escrever na pilha BT) sem perder frames.
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 512;

    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &s_rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao criar canal I2S RX: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_cfg = {
        // Como slave, esse clk_cfg nao gera clock nenhum (quem manda e' a
        // placa principal) - o valor aqui so' influencia detalhes
        // internos do driver. 44100 e' o valor inicial; se a taxa real
        // mudar (arquivo com sample rate diferente), quem precisa saber
        // disso de verdade e' o encoder LDAC (main.c re-abre o handle
        // dele quando chega UART_CMD_SET_SAMPLE_RATE) - o I2S em si
        // continua so' seguindo o clock que chega fisicamente.
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                         I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)cfg->bclk_gpio,
            .ws   = (gpio_num_t)cfg->ws_gpio,
            .dout = I2S_GPIO_UNUSED,
            .din  = (gpio_num_t)cfg->din_gpio,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(s_rx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao configurar modo std I2S RX: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_channel_enable(s_rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao habilitar canal I2S RX: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "I2S RX (slave) pronto - bclk=%d ws=%d din=%d",
             cfg->bclk_gpio, cfg->ws_gpio, cfg->din_gpio);
    return ESP_OK;
}

int i2s_input_read_frames(int32_t *out, size_t frame_count, uint32_t timeout_ms)
{
    if (!s_rx_handle || !out) return -1;

    size_t bytes_to_read = frame_count * 2 /* L+R */ * sizeof(int32_t);
    size_t bytes_read = 0;

    esp_err_t ret = i2s_channel_read(s_rx_handle, out, bytes_to_read, &bytes_read,
                                      pdMS_TO_TICKS(timeout_ms));
    if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "Erro na leitura I2S: %s", esp_err_to_name(ret));
        return -1;
    }

    return (int)(bytes_read / (2 * sizeof(int32_t)));
}

static uint32_t s_rx_sample_rate = 44100;

esp_err_t i2s_input_set_rate(uint32_t sample_rate)
{
    if (!s_rx_handle || sample_rate == 0) return ESP_ERR_INVALID_ARG;
    if (sample_rate == s_rx_sample_rate) return ESP_OK;

    i2s_channel_disable(s_rx_handle);
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    esp_err_t ret = i2s_channel_reconfig_std_clock(s_rx_handle, &clk_cfg);
    i2s_channel_enable(s_rx_handle);
    if (ret == ESP_OK) {
        s_rx_sample_rate = sample_rate;
        ESP_LOGI(TAG, "I2S RX slave clock reconfigurado para %u Hz", (unsigned)sample_rate);
    } else {
        ESP_LOGE(TAG, "Falha ao reconfigurar clock I2S RX slave para %u Hz: %s",
                 (unsigned)sample_rate, esp_err_to_name(ret));
    }
    return ret;
}
