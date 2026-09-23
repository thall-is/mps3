#ifndef I2S_OUTPUT_H
#define I2S_OUTPUT_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Inicializa o canal I2S em modo STD, 32 bits por slot, estereo,
// usando os pinos definidos em pinos.h. O PCM5102A opera em modo 4 fios
// com Master Clock dedicado no GPIO 8 (pino SCK do DAC), desligando a PLL
// analógica interna do DAC e garantindo reprodução bit-perfect até 192 kHz / 24-bit.
// Clock source configurado para PLL_240M com multiplicador inteligente
// (128 fs para >= 176.4 kHz; 256 fs para <= 96 kHz, mantendo MCLK <= 24.576 MHz),
// e buffer DMA ampliado de 24x512 frames (98 KB / 64 ms a 192 kHz).
esp_err_t i2s_output_init(void);

// Reconfigura a taxa de amostragem do canal (suporta de 8 kHz ate 192 kHz).
// E' uma operacao "cara" (desabilita e reabilita o canal), por isso so'
// faz algo se a taxa mudou. Usa clock source PLL_240M.
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
