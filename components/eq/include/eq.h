#ifndef EQ_H
#define EQ_H

/**
 * @file eq.h
 * @brief Equalizador Paramétrico de 10 Bandas IIR com Auto Pre-cut
 *
 * Implementa 10 filtros biquad Direct Form I com ganhos ajustáveis de -12 dB a +12 dB
 * por banda, cálculo automático de atenuação de entrada (Auto Pre-cut) para prevenção
 * de ceifamento digital (*hard clipping*), alocação de coeficientes em SRAM interna rápida
 * e bypass Bit-Perfect automático para faixas Hi-Res acima de 48 kHz.
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
 * @brief Recalcula os coeficientes dos filtros biquad para a nova taxa de amostragem.
 *
 * Deve ser invocada antes de iniciar o processamento de novas faixas quando a taxa
 * de amostragem mudar (ex: de 44.1 kHz para 48 kHz).
 *
 * @param[in] sample_rate Frequência de amostragem da faixa em Hz.
 */
void eq_set_sample_rate(uint32_t sample_rate);

/**
 * @brief Atualiza os ganhos das bandas, recalcula o Auto Pre-cut e persiste na NVS.
 *
 * @param[in] config Ponteiro para a nova configuração a ser aplicada.
 */
void eq_update_config(const eq_config_t *config);

/**
 * @brief Processa um bloco de amostras PCM de áudio estéreo em 32 bits (*in-place*).
 *
 * Aplica os 10 filtros biquad em cascata nos canais esquerdo e direito.
 * Para taxas acima de 48 kHz, o processamento entra automaticamente em modo Bit-Perfect
 * direto sem alteração de amostras para economizar ciclos de CPU.
 *
 * @param[in,out] buffer  Ponteiro para as amostras estéreo intercaladas (L, R).
 * @param[in]     samples Número total de amostras escalares (frames * 2).
 *
 * @note Executa no Core 0 dentro de `audio_dsp_task`.
 */
void eq_process(int32_t *buffer, size_t samples);

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
 * @brief Zera os registradores de atraso histórico (z1, z2) dos filtros biquad.
 *
 * Evita transientes e ruídos residuais (*pops*) ao alternar de faixa ou após saltos de busca (seek).
 */
void eq_reset_state(void);

#ifdef __cplusplus
}
#endif

#endif // EQ_H