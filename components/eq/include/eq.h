#ifndef EQ_H
#define EQ_H

/**
 * @file eq.h
 * @brief Equalizador Paramétrico SVF Trapezoidal Linear de 10 Bandas
 *
 * Implementa 10 filtros SVF (State Variable Filter Trapezoidal Linear — Andrew Simper / Cytomic
 * Peaking Bell) sintetizados em precisão dupla (`double` 64-bit com `g = tan(pi * f0 / Fs)`)
 * e executados em ponto flutuante IEEE-754 de 32 bits em `IRAM_ATTR`, com ganhos ajustáveis
 * de -15 dB a +15 dB por banda e Pre-Amp geral de -30 dB a +30 dB. Opera continuamente em
 * todas as taxas de amostragem (44.1 kHz a 384 kHz) sem bypass, utilizando double-buffering
 * lock-free (`s_eq_pool[2]`) em SRAM interna e processamento em tiles de 64 pares L/R com ILP.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EQ_BANDS 10           /**< Número de bandas de frequência do equalizador. */
#define EQ_MAX_PRESETS 10     /**< Capacidade máxima de perfis de equalização. */
#define EQ_PRESET_NAME_LEN 16 /**< Limite de caracteres para o nome de cada preset. */

/**
 * @brief Estrutura de perfil predefinido de equalização.
 */
typedef struct {
    char name[EQ_PRESET_NAME_LEN]; /**< Nome do perfil (ex: "Rock", "Bass Boost"). */
    float band_gains[EQ_BANDS];    /**< Ganhos individuais de cada banda em dB. */
    float overall_gain;            /**< Ganho geral compensatório em dB. */
} eq_preset_t;

/**
 * @brief Configuração global do equalizador persistida na NVS.
 */
typedef struct {
    bool enabled;                         /**< Estado do equalizador (ativo ou bypass). */
    int active_preset_idx;                /**< Índice do preset em uso. */
    eq_preset_t presets[EQ_MAX_PRESETS];  /**< Lista de presets disponíveis. */
} eq_config_t;

/**
 * @brief Inicializa o equalizador paramétrico e restaura a configuração salva na NVS.
 *
 * Aloca as estruturas de estado dos filtros em SRAM interna de alta velocidade
 * (`MALLOC_CAP_INTERNAL`) para máxima performance no pipeline de áudio.
 */
void eq_init(void);

/**
 * @brief Recalcula os coeficientes dos filtros SVF trapezoidais lineares para a nova taxa de amostragem (suporta de 8 kHz a 384 kHz).
 *
 * Deve ser invocada antes de iniciar o processamento de novas faixas quando a taxa
 * de amostragem mudar (ex: de 44.1 kHz para 96 kHz ou 384 kHz).
 *
 * @param[in] sample_rate Frequência de amostragem da faixa em Hz.
 */
void eq_set_sample_rate(uint32_t sample_rate);

/**
 * @brief Atualiza os ganhos das bandas (-15 dB a +15 dB) e Pre-Amp (-30 dB a +30 dB) em double-buffer lock-free e agenda gravação debounced (1.5s) na NVS.
 *
 * @param[in] config Ponteiro para a nova configuração a ser aplicada.
 */
void eq_update_config(const eq_config_t *config);

/**
 * @brief Processa um bloco de amostras PCM de áudio estéreo em 32 bits (*in-place*).
 *
 * Aplica os 10 filtros SVF (State Variable Filter trapezoidal linear) em cascata
 * nos canais esquerdo e direito, suportando taxas desde 8 kHz até 192 kHz / 384 kHz
 * sem perda de precisão nos graves.
 *
 * @param[in,out] buffer  Ponteiro para as amostras estéreo intercaladas (L, R).
 * @param[in]     samples Número total de amostras escalares (frames * 2).
 *
 * @note Executa no Core 0 dentro de `audio_dsp_task`.
 */
void eq_process(int32_t *buffer, size_t samples);

/**
 * @brief Processa o equalizador de 10 bandas fundindo o ganho de volume/balanço L/R em um único passe.
 *
 * Se o equalizador estiver ativo e não-flat, aplica `gain_l` e `gain_r` na conversão de entrada
 * Q31 -> float (custo zero extra de CPU e metade do tráfego de barramento PSRAM) e retorna `true`.
 * Caso o equalizador esteja desativado ou totalmente flat (0 dB), retorna `false` sem modificar
 * o buffer para que o chamador aplique apenas o controle de volume inteiro rápido.
 *
 * @param[in,out] buffer  Ponteiro para as amostras estéreo intercaladas (L, R).
 * @param[in]     samples Número total de amostras escalares (frames * 2).
 * @param[in]     gain_l  Multiplicador linear de volume/balanço do canal esquerdo [0.0 .. 1.0].
 * @param[in]     gain_r  Multiplicador linear de volume/balanço do canal direito [0.0 .. 1.0].
 * @return `true` se o buffer foi processado (incluindo ganho L/R), `false` se em bypass.
 */
bool eq_process_with_gain(int32_t *buffer, size_t samples, float gain_l, float gain_r);

/**
 * @brief Obtém uma cópia da configuração atual do equalizador.
 *
 * @param[out] config Ponteiro para a estrutura `eq_config_t` de saída.
 */
void eq_get_config(eq_config_t *config);

/**
 * @brief Restaura todos os ganhos do equalizador para a curva plana (Flat 0 dB).
 */
void eq_reset_to_defaults(void);

/**
 * @brief Zera os registradores de estado integrador (ic1, ic2) dos filtros SVF.
 *
 * Evita transientes e ruídos residuais (*pops*) ao alternar de faixa ou após saltos de busca (seek).
 */
void eq_reset_state(void);

#ifdef __cplusplus
}
#endif

#endif // EQ_H