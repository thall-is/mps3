#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_METER_MODE_PRE_VOLUME = 0,  // Nivel da faixa / dinamica total da gravacao
    AUDIO_METER_MODE_POST_VOLUME = 1  // Nivel atenuado pelo volume do fone de ouvido
} audio_meter_mode_t;

/**
 * @brief Inicializa o motor de medicao de intensidade sonora e balistica.
 */
esp_err_t audio_meter_init(void);

/**
 * @brief Alimenta um bloco de amostras PCM estéreo de 32 bits intercaladas (L, R, L, R...).
 *        Thread-safe e lock-free para execucao suave nos cores de audio.
 *
 * @param samples Ponteiro para o buffer de amostras int32_t (L e R)
 * @param sample_count Quantidade total de amostras (canais somados, ex: 512 = 256 L + 256 R)
 * @param sample_rate Taxa de amostragem em Hz (ex: 44100, 48000)
 */
void audio_meter_feed(const int32_t *samples, size_t sample_count, uint32_t sample_rate);

/**
 * @brief Obtem os niveis calculados com balistica suave para desenho no display OLED.
 *        Os valores normalizados variam de 0.0f (silencio <= -40 dBFS) ate 1.0f (0 dBFS / clipping).
 *
 * @param out_peak_l Nivel instantaneo suavizado do canal L (0.0 a 1.0)
 * @param out_peak_r Nivel instantaneo suavizado do canal R (0.0 a 1.0)
 * @param out_hold_l Nivel de Peak-Hold do canal L (0.0 a 1.0)
 * @param out_hold_r Nivel de Peak-Hold do canal R (0.0 a 1.0)
 * @param out_db_l Valor opcional em dBFS do canal L (-40.0 a 0.0)
 * @param out_db_r Valor opcional em dBFS do canal R (-40.0 a 0.0)
 */
void audio_meter_get_levels(float *out_peak_l, float *out_peak_r,
                           float *out_hold_l, float *out_hold_r,
                           float *out_db_l, float *out_db_r);

/**
 * @brief Configura o modo de medicao (Pre-Volume vs Pos-Volume) e persiste na NVS se solicitado.
 */
void audio_meter_set_mode(audio_meter_mode_t mode, bool save_nvs);

/**
 * @brief Retorna o modo atual de medicao.
 */
audio_meter_mode_t audio_meter_get_mode(void);

/**
 * @brief Informa se ha sinal de audio ativo detectado recentemente (< 120 ms).
 */
bool audio_meter_is_active(void);

#define AUDIO_METER_SPECTRUM_BANDS 11

/**
 * @brief Obtem os niveis normalizados (0.0 a 1.0) das bandas de espectro com balistica e peak-hold.
 *
 * @param out_bands Array de AUDIO_METER_SPECTRUM_BANDS floats para as barras de energia
 * @param out_peaks Array de AUDIO_METER_SPECTRUM_BANDS floats para os marcadores de peak-hold flutuantes
 */
void audio_meter_get_spectrum_bands(float out_bands[AUDIO_METER_SPECTRUM_BANDS], float out_peaks[AUDIO_METER_SPECTRUM_BANDS]);

#ifdef __cplusplus
}
#endif

