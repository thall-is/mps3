#include "i2s_output.h"
#include "pinos.h"

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

static const char *TAG = "i2s_output";
#include <string.h>

void (*g_radio_pcm_hook)(const int16_t *data, size_t len) = NULL;

static i2s_chan_handle_t s_tx_handle = NULL;
static uint32_t s_current_rate = 0;
static bool s_channel_enabled = true;
static void (*s_rate_change_cb)(uint32_t sample_rate) = NULL;

void i2s_output_set_rate_change_cb(void (*cb)(uint32_t sample_rate))
{
    s_rate_change_cb = cb;
    if (s_rate_change_cb && s_current_rate > 0) {
        s_rate_change_cb(s_current_rate);
    }
}

esp_err_t i2s_output_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    // Buffer DMA maior do que o padrao (6 descritores x 240 frames) para
    // absorver picos de latencia da leitura do SD / decodificacao sem
    // estalar o audio. 8 x 1024 frames x 4 bytes x 2 canais = 64KB de
    // buffer total, dando bastante margem.
    chan_cfg.dma_desc_num = 16;
    chan_cfg.dma_frame_num = 480;
    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao criar canal I2S: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                         I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            // PCM5102: sem MCLK externo (pino SCK do modulo aterrado,
            // ele usa o PLL interno a partir de BCK/LRCK).
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)PIN_I2S_BCLK,
            .ws   = (gpio_num_t)PIN_I2S_LRCK,
            .dout = (gpio_num_t)PIN_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(s_tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao configurar modo std I2S: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_channel_enable(s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao habilitar canal I2S: %s", esp_err_to_name(ret));
        return ret;
    }

    s_current_rate = 44100;
    s_channel_enabled = true;
    return ESP_OK;
}

esp_err_t i2s_output_set_rate(uint32_t sample_rate)
{
    if (sample_rate == s_current_rate) {
        return ESP_OK;
    }

    esp_err_t ret = i2s_channel_disable(s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao desabilitar canal I2S: %s", esp_err_to_name(ret));
        return ret;
    }
    // A partir daqui o canal esta' de fato desabilitado no hardware -
    // s_channel_enabled so' pode voltar a "true" se a gente realmente
    // conseguir reabilita-lo mais abaixo (senao audio_player fica
    // escrevendo silenciosamente num canal mudo, sem erro visivel).
    s_channel_enabled = false;

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    ret = i2s_channel_reconfig_std_clock(s_tx_handle, &clk_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao reconfigurar clock I2S: %s", esp_err_to_name(ret));
        // Tenta religar o canal na taxa antiga em vez de deixar o audio
        // mudo ate' a proxima troca de taxa - best effort, se isso
        // tambem falhar s_channel_enabled fica false (estado real) e o
        // erro original e' o que retorna pro chamador.
        esp_err_t re_ret = i2s_channel_enable(s_tx_handle);
        if (re_ret == ESP_OK) s_channel_enabled = true;
        return ret;
    }

    ret = i2s_channel_enable(s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao reabilitar canal I2S: %s", esp_err_to_name(ret));
        return ret;
    }

    s_current_rate = sample_rate;
    s_channel_enabled = true;
    ESP_LOGI(TAG, "Taxa de amostragem I2S ajustada para %u Hz", (unsigned)sample_rate);
    if (s_rate_change_cb) {
        s_rate_change_cb(sample_rate);
    }
    return ESP_OK;
}

uint32_t i2s_output_get_rate(void)
{
    return s_current_rate;
}

esp_err_t i2s_output_write(const int32_t *samples, size_t sample_count, size_t *samples_written)
{
    size_t bytes_to_write = sample_count * sizeof(int32_t);
    size_t bytes_written = 0;

    esp_err_t ret = i2s_channel_write(s_tx_handle, samples, bytes_to_write,
                                       &bytes_written, portMAX_DELAY);

    if (g_radio_pcm_hook && sample_count > 0) {
        // Converter int32_t para int16_t para streaming WAV/PCM
        // i2s_output_write recebe estéreo, então sample_count é o total de amostras (L+R combinados)
        // O áudio no ESP32 S3 em 32-bit I2S geralmente usa os 24 bits ou 16 bits superiores ou inferiores.
        // O codec esp_audio usa 16 bits que são expandidos para 32-bit. No ESP-IDF, normalmente
        // o áudio de 16-bit é deslocado para a esquerda (<< 16) ao enviar para 32-bit.
        // Vamos extrair os 16 bits mais significativos.
        int16_t *buf16 = (int16_t *)malloc(sample_count * sizeof(int16_t));
        if (buf16) {
            for (size_t i = 0; i < sample_count; i++) {
                buf16[i] = (int16_t)(samples[i] >> 16);
            }
            g_radio_pcm_hook(buf16, sample_count * sizeof(int16_t));
            free(buf16);
        }
    }

    if (samples_written) {
        *samples_written = bytes_written / sizeof(int32_t);
    }

    static uint32_t s_diag_last_tick = 0;
    static uint32_t s_diag_frames = 0;
    s_diag_frames += (uint32_t)(bytes_written / (2 * sizeof(int32_t)));
    uint32_t now_tick = (uint32_t)xTaskGetTickCount();
    if ((now_tick - s_diag_last_tick) >= pdMS_TO_TICKS(3000)) {
        uint32_t elapsed_ms = (now_tick - s_diag_last_tick) * portTICK_PERIOD_MS;
        s_diag_last_tick = now_tick;
        uint32_t fps = (s_diag_frames * 1000) / (elapsed_ms ? elapsed_ms : 1);
        ESP_LOGI(TAG, "[TELEMETRIA S3] I2S TX: %u frames/s transmitidos (Rate=%u Hz, DOUT=47, BCLK=48, LRCK=21)",
                 (unsigned)fps, (unsigned)s_current_rate);
        s_diag_frames = 0;
    }

    return ret;
}

esp_err_t i2s_output_disable(void)
{
    if (!s_channel_enabled) return ESP_OK;
    esp_err_t ret = i2s_channel_disable(s_tx_handle);
    if (ret == ESP_OK) s_channel_enabled = false;
    return ret;
}

esp_err_t i2s_output_enable(void)
{
    if (s_channel_enabled) return ESP_OK;
    esp_err_t ret = i2s_channel_enable(s_tx_handle);
    if (ret == ESP_OK) s_channel_enabled = true;
    return ret;
}

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void i2s_output_play_test_tone(uint32_t freq_hz, uint32_t duration_ms)
{
    ESP_LOGI(TAG, "Gerando tom senoidal de teste: %u Hz por %u ms...", (unsigned)freq_hz, (unsigned)duration_ms);
    uint32_t rate = s_current_rate ? s_current_rate : 44100;
    size_t chunk_frames = 256;
    int32_t *buf = (int32_t *)malloc(chunk_frames * 2 * sizeof(int32_t));
    if (!buf) return;

    size_t total_frames = (rate * duration_ms) / 1000;
    size_t frames_sent = 0;
    double phase = 0.0;
    double phase_inc = (2.0 * M_PI * (double)freq_hz) / (double)rate;

    while (frames_sent < total_frames) {
        size_t count = (total_frames - frames_sent < chunk_frames) ? (total_frames - frames_sent) : chunk_frames;
        for (size_t i = 0; i < count; i++) {
            int16_t val = (int16_t)(sin(phase) * 16000.0);
            int32_t s32 = ((int32_t)val) << 16;
            buf[2 * i]     = s32;
            buf[2 * i + 1] = s32;
            phase += phase_inc;
            if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
        }
        size_t written = 0;
        i2s_output_write(buf, count * 2, &written);
        frames_sent += count;
    }
    free(buf);
    ESP_LOGI(TAG, "Tom de teste finalizado.");
}
