#ifndef I2S_OUTPUT_H
#define I2S_OUTPUT_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Inicializa o canal I2S em modo STD, 32 bits por slot, estereo,
// usando os pinos definidos em pinos.h. O PCM5102 nao usa MCLK
// (pino SCK do modulo deve estar aterrado para usar o PLL interno).
esp_err_t i2s_output_init(void);

// Reconfigura a taxa de amostragem do canal (chamado ao trocar de faixa
// se o sample rate for diferente do anterior). E' uma operacao "cara"
// (desabilita e reabilita o canal), por isso so' faz algo se a taxa mudou.
esp_err_t i2s_output_set_rate(uint32_t sample_rate);
uint32_t  i2s_output_get_rate(void);

// Registra callback chamado sempre que a taxa de amostragem mudar
void i2s_output_set_rate_change_cb(void (*cb)(uint32_t sample_rate));

// Escreve amostras PCM de 32 bits, intercaladas (estereo: L,R,L,R,...).
// Bloqueia (portMAX_DELAY) até que o DMA aceite os dados.
esp_err_t i2s_output_write(const int32_t *samples, size_t sample_count, size_t *samples_written);

// Desliga/religa fisicamente o canal I2S (usado pelo play/pause: silencia
// de verdade a saida, em vez de so' parar de escrever amostras).
esp_err_t i2s_output_disable(void);
esp_err_t i2s_output_enable(void);

// Injeta um tom senoidal de teste diretamente no barramento I2S
void i2s_output_play_test_tone(uint32_t freq_hz, uint32_t duration_ms);

#ifdef __cplusplus
}
#endif

#endif // I2S_OUTPUT_H
