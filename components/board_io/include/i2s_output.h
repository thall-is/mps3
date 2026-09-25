#ifndef I2S_OUTPUT_H
#define I2S_OUTPUT_H

/**
 * @file i2s_output.h
 * @brief Driver de Saída de Áudio Digital I2S Master para o DAC PCM5102A
 *
 * Configura e gerencia o barramento I2S em modo 4-fios com Master Clock (MCLK)
 * dedicado no GPIO 8, DOUT no GPIO 47, BCLK no GPIO 48 e LRCK no GPIO 21.
 * Utiliza o clock base PLL_240M com multiplicadores inteligentes (128fs / 256fs)
 * e descritores DMA estéreo de 32 bits para transmissão Bit-Perfect até 24-bit / 192 kHz.
 */

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa o canal transmissor I2S Master e configura a DMA.
 *
 * Inicializa a interface I2S padrão Philips, slots de 32 bits estéreo, aloca
 * descritores DMA (24 descritores de 512 frames) e ativa o sinal MCLK no GPIO 8.
 *
 * @return 
 *   - ESP_OK: Driver I2S inicializado com sucesso.
 *   - ESP_FAIL: Falha ao alocar canal ou descritores DMA.
 */
esp_err_t i2s_output_init(void);

/**
 * @brief Reconfigura a taxa de amostragem do canal I2S (de 8 kHz a 192 kHz).
 *
 * Ajusta dinamicamente os divisores de frequência do clock I2S (`PLL_240M`),
 * selecionando multiplicador de 128fs para taxas >= 176.4 kHz e 256fs para taxas <= 96 kHz.
 *
 * @param[in] sample_rate Frequência alvo em Hz (ex.: 44100, 48000, 96000, 192000).
 *
 * @return 
 *   - ESP_OK: Taxa reconfigurada com sucesso.
 *   - ESP_ERR_INVALID_ARG: Taxa de amostragem não suportada.
 */
esp_err_t i2s_output_set_rate(uint32_t sample_rate);

/**
 * @brief Retorna a taxa de amostragem atualmente configurada no canal I2S.
 *
 * @return Frequência em Hz.
 */
uint32_t i2s_output_get_rate(void);

/**
 * @brief Registra callback notificado sempre que a taxa de amostragem do I2S for alterada.
 *
 * @param[in] cb Ponteiro para a função de notificação `void cb(uint32_t sample_rate)`.
 */
void i2s_output_set_rate_change_cb(void (*cb)(uint32_t sample_rate));

/**
 * @brief Escreve amostras de áudio PCM de 32 bits diretamente nos descritores DMA do I2S.
 *
 * Transmite blocos de amostras estéreo intercaladas (L, R). Bloqueia de forma segura
 * até que haja espaço nos descritores DMA ou ocorra timeout de segurança de 1000 ms.
 *
 * @param[in]  samples         Ponteiro para o buffer de amostras PCM estéreo de 32 bits.
 * @param[in]  sample_count    Número total de amostras a escrever (frames * 2).
 * @param[out] samples_written Ponteiro onde será registrado o número de amostras aceitas.
 *
 * @return 
 *   - ESP_OK: Amostras escritas no buffer DMA.
 *   - ESP_ERR_INVALID_STATE: Canal I2S desabilitado ou nulo.
 *   - ESP_ERR_TIMEOUT: Timeout na fila de DMA.
 */
esp_err_t i2s_output_write(const int32_t *samples, size_t sample_count, size_t *samples_written);

/**
 * @brief Desabilita fisicamente o canal de transmissão I2S.
 *
 * Utilizado na operação de Pausa e modos de suspensão para silenciar a saída analógica.
 *
 * @return ESP_OK em caso de sucesso.
 */
esp_err_t i2s_output_disable(void);

/**
 * @brief Reabilita o canal de transmissão I2S após uma pausa.
 *
 * @return ESP_OK em caso de sucesso.
 */
esp_err_t i2s_output_enable(void);

/**
 * @brief Injeta um tom senoidal de teste diretamente nos buffers do I2S.
 *
 * Ferramenta de bancada para diagnóstico de integridade elétrica do DAC PCM5102A.
 *
 * @param[in] freq_hz     Frequência do tom em Hz (ex.: 1000 Hz).
 * @param[in] duration_ms Duração da emissão do tom em milissegundos.
 */
void i2s_output_play_test_tone(uint32_t freq_hz, uint32_t duration_ms);

#ifdef __cplusplus
}
#endif

#endif // I2S_OUTPUT_H
