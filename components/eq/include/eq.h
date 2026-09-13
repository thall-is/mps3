#ifndef EQ_H
#define EQ_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Configuração do equalizador de 10 bandas
#define EQ_BANDS 10

#define EQ_MAX_PRESETS 10
#define EQ_PRESET_NAME_LEN 16

typedef struct {
    char name[EQ_PRESET_NAME_LEN];
    float band_gains[EQ_BANDS];
    float overall_gain;
} eq_preset_t;

typedef struct {
    bool enabled;
    int active_preset_idx;
    eq_preset_t presets[EQ_MAX_PRESETS];
} eq_config_t;

// Inicializa o equalizador (carrega config da NVS se disponível)
void eq_init(void);

// Notifica o EQ da taxa de amostragem real do stream atual — deve ser
// chamado em HeaderReady, antes de qualquer eq_process(). Recalcula os
// coeficientes biquad para a frequência correta.
void eq_set_sample_rate(uint32_t sample_rate);

// Atualiza a configuração do equalizador (ganhos e enable) e salva na NVS
void eq_update_config(const eq_config_t *config);

// Processa um buffer de áudio estéreo (int32_t, left-justified 32 bits)
// Entrada/Saída: mesmo buffer, mesmo tamanho em samples (pares L,R intercalados)
void eq_process(int32_t *buffer, size_t samples);

// Obtém a configuração atual do equalizador
void eq_get_config(eq_config_t *config);

// Reseta para configurações padrão (flat, sem efeito) e salva na NVS
void eq_reset_to_defaults(void);

// Reseta os buffers de atraso (z1, z2) dos biquads para evitar estalos entre faixas e seeks
void eq_reset_state(void);

#ifdef __cplusplus
}
#endif

#endif // EQ_H