// Equalizador Paramétrico de 10 Bandas usando Filtros SVF Trapezoidais Lineares
// (Andrew Simper / Cytomic Peaking Bell), sintetizados em precisão dupla (double 64-bit)
// e executados em ponto flutuante IEEE-754 de 32 bits em IRAM (IRAM_ATTR).
// Opera em todas as taxas de amostragem (44.1 kHz a 384 kHz) sem perda de precisão nos graves,
// com ganhos de -15 dB a +15 dB por banda e Pre-Amp geral de -30 dB a +30 dB.
//
// Formato do buffer: int32_t Q31 (32 bits por amostra), intercalado L,R,L,R,...
// Processamento in-place ou fundido com volume/balanço L/R (eq_process_with_gain)
// em blocos de 64 pares (EQ_TILE_PAIRS) para maximizar localidade de cache D-Cache L1
// e paralelismo de instruções (ILP) na FPU do Xtensa LX7.
//
// Thread safety: eq_update_config() preenche o slot inativo de s_eq_pool[2] e realiza
// troca atômica de ponteiro (s_active_eq) consumido pela audio_dsp_task (Core 0).

#include "eq.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <math.h>

static const char *TAG = "eq";

// NVS — namespace e chaves dos ganhos do EQ
#define EQ_NVS_NAMESPACE   "mps3"
#define EQ_NVS_KEY_ENABLED "eq_enabled"
#define EQ_NVS_KEY_OVERALL "eq_overall"
#define EQ_NVS_KEY_PRESET  "eq_preset"
// Usaremos um prefixo pra gerar as chaves das 10 bandas dinamicamente: "eq_b0" a "eq_b9"

// Coeficientes do filtro SVF Trapezoidal Linear (Andrew Simper / Cytomic Peaking Bell)
// Diferente da Forma Direta II Transposta (DF2T) — onde cos(w0) -> 0.9999995 em 31 Hz @ 192 kHz
// perde 20 bits da mantissa float32 e causa instabilidade/estampidos nos graves — o SVF
// trapezoidal opera diretamente sobre g = tan(pi * f0 / Fs), preservando os 24 bits de
// significando da IEEE-754 em qualquer taxa (44.1 kHz a 384 kHz) e permitindo modulacao
// de ganho ao vivo livre de cliques.
typedef struct {
    float a1; // 1 / (1 + g * (g + k))
    float a2; // g * a1
    float g2; // 2 * g
    float m1; // k * (A^2 - 1)
} SvfCoeffs;

// Estado dos integradores trapezoidais do SVF (por canal)
typedef struct {
    float ic1, ic2;
} SvfState;

// Frequências centrais das 10 bandas (Hz)
static const float BAND_FREQ[EQ_BANDS] = {
    31.0f, 62.0f, 125.0f, 250.0f, 500.0f,
    1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f
};

// Q factor fixo para as bandas (1.41 = meia oitava, bom para EQ grafico de 10 bandas)
static const float BAND_Q = 1.41f;

// Síntese em dupla precisão (64-bit double) para os coeficientes do Peaking Bell SVF:
//   A  = 10^(gain_db / 40)
//   g  = tan(pi * freq_hz / sample_rate)
//   k  = 1 / (Q * A)
//   a1 = 1 / (1 + g * (g + k))
//   a2 = g * a1
//   g2 = 2 * g
//   m1 = k * (A^2 - 1)
static void compute_peaking_svf(float freq_hz, float q, float gain_db,
                                float sample_rate, SvfCoeffs *c)
{
    if (sample_rate <= 0.0f) sample_rate = 44100.0f;

    // Se freq_hz >= Nyquist * 0.9 (freq_hz >= sample_rate * 0.45f) ou ganho nulo,
    // configura pass-through wire puro.
    if (freq_hz >= sample_rate * 0.45f || fabsf(gain_db) <= 0.01f) {
        c->a1 = 0.0f;
        c->a2 = 0.0f;
        c->g2 = 0.0f;
        c->m1 = 0.0f;
        return;
    }

    double A  = pow(10.0, (double)gain_db / 40.0);
    double g  = tan(M_PI * (double)freq_hz / (double)sample_rate);
    double k  = 1.0 / ((double)q * A);
    double a1 = 1.0 / (1.0 + g * (g + k));
    double a2 = g * a1;
    double g2 = 2.0 * g;
    double m1 = k * (A * A - 1.0);

    c->a1 = (float)a1;
    c->a2 = (float)a2;
    c->g2 = (float)g2;
    c->m1 = (float)m1;
}

// Bloco de coeficientes para todas as bandas (double-buffered para troca lock-free)
typedef struct {
    SvfCoeffs    coeffs[EQ_BANDS];
    float        cached_gains[EQ_BANDS]; // cache de ganhos para evitar recalcular bandas inalteradas
    float        cached_sr;              // cache de sample rate
    uint8_t      active_bands[EQ_BANDS]; // indices das bandas com ganho != 0 dB
    int          num_active_bands;       // quantidade de bandas ativas (0..EQ_BANDS)
    float        overall_gain;           // multiplicador linear final (10^(overall_db/20))
    bool         enabled;
    bool         is_flat;
} EqState;

// Estado unico dos integradores em DRAM interna: pertence exclusivamente ao fluxo de audio,
// garantindo continuidade de fase 100% livre de saltos durante trocas de coeficientes ao vivo.
DRAM_ATTR static SvfState s_eq_states[EQ_BANDS][2];

// Ping-pong double-buffering para troca 100% atomica de coeficientes sem travar o Core 0/1
static EqState *s_eq_pool[2] = {NULL, NULL};
static volatile int s_active_idx = 0;
static EqState * volatile s_active_eq = NULL;

// Configuração corrente e espelho do que já está salvo na NVS (para gravação diferencial)
static eq_config_t s_config;           // zero-initialized by BSS
static eq_config_t s_nvs_saved_config; // espelho da NVS para evitar gravar 122 chaves a cada clique
static esp_timer_handle_t s_eq_nvs_timer = NULL;

// Sample rate descoberto ao abrir a primeira faixa — precisamos para
// recalcular os coeficientes quando o EQ é configurado ANTES de tocar.
// Inicializado com o valor mais comum; será atualizado por eq_set_sample_rate().
static float s_sample_rate = 44100.0f;

// Calcula apenas os coeficientes SVF modificados a partir da config atual e compacta bandas ativas
static void recalculate_coeffs(EqState *eq, const eq_config_t *cfg, float sr)
{
    int p = cfg->active_preset_idx;
    if (p < 0 || p >= EQ_MAX_PRESETS) p = 0;

    bool sr_changed = (eq->cached_sr != sr);
    int active_cnt = 0;
    for (int b = 0; b < EQ_BANDS; b++) {
        float gain = cfg->presets[p].band_gains[b];
        if (sr_changed || fabsf(gain - eq->cached_gains[b]) > 0.001f) {
            compute_peaking_svf(BAND_FREQ[b], BAND_Q, gain, sr, &eq->coeffs[b]);
            eq->cached_gains[b] = gain;
        }
        if (fabsf(gain) > 0.01f && BAND_FREQ[b] < sr * 0.45f) {
            eq->active_bands[active_cnt++] = (uint8_t)b;
        } else {
            // Limpa estado dos integradores de bandas em 0 dB (bypass) para evitar residuo DC
            s_eq_states[b][0].ic1 = 0.0f;
            s_eq_states[b][0].ic2 = 0.0f;
            s_eq_states[b][1].ic1 = 0.0f;
            s_eq_states[b][1].ic2 = 0.0f;
        }
    }
    eq->cached_sr = sr;
    eq->num_active_bands = active_cnt;

    float o = cfg->presets[p].overall_gain;
    bool overall_active = (fabsf(o) > 0.01f);

    // Ganho geral direto configurado pelo usuario (sem atenuacao automatica de pre-cut):
    // Permite que o EQ atue com ganho real positivo, com ceifamento seguro em Q31.
    eq->overall_gain = overall_active ? powf(10.0f, o / 20.0f) : 1.0f;
    eq->enabled = cfg->enabled;
    eq->is_flat = (active_cnt == 0) && !overall_active;
}

// Callback de timer debounced (1.5s após o último ajuste no EQ):
// Grava na NVS APENAS as chaves que realmente mudaram, evitando travar o cache da Flash/PSRAM
// durante a reprodução de faixas 24-bit / 192 kHz.
static void eq_nvs_flush_cb(void *arg)
{
    (void)arg;
    eq_config_t cfg = s_config;

    nvs_handle_t h;
    if (nvs_open(EQ_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;

    bool dirty = false;
    for (int p = 0; p < EQ_MAX_PRESETS; p++) {
        if (strncmp(cfg.presets[p].name, s_nvs_saved_config.presets[p].name, EQ_PRESET_NAME_LEN) != 0) {
            char key_name[16];
            snprintf(key_name, sizeof(key_name), "p%d_name", p);
            nvs_set_str(h, key_name, cfg.presets[p].name);
            dirty = true;
        }

        int32_t new_ovr = (int32_t)(cfg.presets[p].overall_gain * 10.0f);
        int32_t old_ovr = (int32_t)(s_nvs_saved_config.presets[p].overall_gain * 10.0f);
        if (new_ovr != old_ovr) {
            char key_ovr[16];
            snprintf(key_ovr, sizeof(key_ovr), "p%d_ovr", p);
            nvs_set_i32(h, key_ovr, new_ovr);
            dirty = true;
        }

        for (int b = 0; b < EQ_BANDS; b++) {
            int32_t new_b = (int32_t)(cfg.presets[p].band_gains[b] * 10.0f);
            int32_t old_b = (int32_t)(s_nvs_saved_config.presets[p].band_gains[b] * 10.0f);
            if (new_b != old_b) {
                char key[16];
                snprintf(key, sizeof(key), "p%d_b%d", p, b);
                nvs_set_i32(h, key, new_b);
                dirty = true;
            }
        }
    }

    if (cfg.active_preset_idx != s_nvs_saved_config.active_preset_idx) {
        nvs_set_i32(h, EQ_NVS_KEY_PRESET, cfg.active_preset_idx);
        dirty = true;
    }
    if (cfg.enabled != s_nvs_saved_config.enabled) {
        nvs_set_i32(h, EQ_NVS_KEY_ENABLED, cfg.enabled ? 1 : 0);
        dirty = true;
    }

    if (dirty) {
        nvs_commit(h);
        s_nvs_saved_config = cfg;
    }
    nvs_close(h);
}

// -------------------------------------------------------------------------
// API pública
// -------------------------------------------------------------------------

void eq_reset_state(void)
{
    memset(s_eq_states, 0, sizeof(s_eq_states));
}

void eq_init(void)
{
    if (s_active_eq) return;

    s_eq_pool[0] = (EqState *)heap_caps_calloc(1, sizeof(EqState),
                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_eq_pool[1] = (EqState *)heap_caps_calloc(1, sizeof(EqState),
                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_eq_pool[0] || !s_eq_pool[1]) {
        ESP_LOGW(TAG, "Falha ao alocar EqState em SRAM interna, tentando alocador padrao...");
        if (!s_eq_pool[0]) s_eq_pool[0] = (EqState *)calloc(1, sizeof(EqState));
        if (!s_eq_pool[1]) s_eq_pool[1] = (EqState *)calloc(1, sizeof(EqState));
    }
    if (!s_eq_pool[0] || !s_eq_pool[1]) {
        ESP_LOGE(TAG, "Sem memoria para o equalizador");
        return;
    }

    memset(s_eq_states, 0, sizeof(s_eq_states));

    eq_config_t cfg = {};
    strlcpy(cfg.presets[0].name, "Padrao", EQ_PRESET_NAME_LEN);
    for(int p = 1; p < EQ_MAX_PRESETS; p++) {
        cfg.presets[p].name[0] = '\0';
    }
    
    nvs_handle_t h;
    if (nvs_open(EQ_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        int32_t active = 0;
        nvs_get_i32(h, EQ_NVS_KEY_PRESET, &active);
        cfg.active_preset_idx = active;
        int32_t enabled = 0;
        nvs_get_i32(h, EQ_NVS_KEY_ENABLED, &enabled);
        cfg.enabled = (enabled != 0);
        
        for (int p = 0; p < EQ_MAX_PRESETS; p++) {
            char key_name[16];
            snprintf(key_name, sizeof(key_name), "p%d_name", p);
            size_t len = EQ_PRESET_NAME_LEN;
            if (nvs_get_str(h, key_name, cfg.presets[p].name, &len) == ESP_OK) {
                if (p == 0 && (strcmp(cfg.presets[0].name, "Preset 1") == 0 || cfg.presets[0].name[0] == '\0')) {
                    strlcpy(cfg.presets[0].name, "Padrao", EQ_PRESET_NAME_LEN);
                } else if (p > 0 && strncmp(cfg.presets[p].name, "Preset ", 7) == 0) {
                    cfg.presets[p].name[0] = '\0';
                }
            } else {
                if (p == 0) strlcpy(cfg.presets[0].name, "Padrao", EQ_PRESET_NAME_LEN);
                else cfg.presets[p].name[0] = '\0';
            }
            
            char key_ovr[16];
            snprintf(key_ovr, sizeof(key_ovr), "p%d_ovr", p);
            int32_t overall = 0;
            nvs_get_i32(h, key_ovr, &overall);
            cfg.presets[p].overall_gain = (float)overall / 10.0f;
            
            for (int b = 0; b < EQ_BANDS; b++) {
                char key[16];
                snprintf(key, sizeof(key), "p%d_b%d", p, b);
                int32_t gain_i32 = 0;
                nvs_get_i32(h, key, &gain_i32);
                cfg.presets[p].band_gains[b] = (float)gain_i32 / 10.0f;
            }
        }
        nvs_close(h);
        ESP_LOGI(TAG, "Config EQ restaurada da NVS (Preset %d)", cfg.active_preset_idx);
    }

    s_config = cfg;
    s_nvs_saved_config = cfg;
    recalculate_coeffs(s_eq_pool[0], &s_config, s_sample_rate);
    recalculate_coeffs(s_eq_pool[1], &s_config, s_sample_rate);
    s_active_idx = 0;
    s_active_eq = s_eq_pool[0];

    if (!s_eq_nvs_timer) {
        const esp_timer_create_args_t t_args = {
            .callback = &eq_nvs_flush_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "eq_nvs_flush",
            .skip_unhandled_events = true
        };
        esp_timer_create(&t_args, &s_eq_nvs_timer);
    }

    ESP_LOGI(TAG, "Equalizador SVF inicializado com double-buffer e NVS debounced");
}

// Chamado pelo audio_player quando HeaderReady revela a sample rate real
void eq_set_sample_rate(uint32_t sample_rate)
{
    if (sample_rate == 0 || sample_rate == (uint32_t)s_sample_rate) return;
    s_sample_rate = (float)sample_rate;

    // Recalcula coeficientes no buffer inativo e faz troca atomica
    int next_idx = 1 - s_active_idx;
    EqState *next_eq = s_eq_pool[next_idx];
    if (next_eq) {
        memset(s_eq_states, 0, sizeof(s_eq_states));
        recalculate_coeffs(next_eq, &s_config, s_sample_rate);
        s_active_eq = next_eq;
        s_active_idx = next_idx;
        ESP_LOGI(TAG, "EQ SVF: coeficientes recalculados para %u Hz (%d bandas ativas)",
                 (unsigned)sample_rate, next_eq->num_active_bands);
    }
}

void eq_update_config(const eq_config_t *config)
{
    if (!config) return;

    eq_config_t cfg = *config;
    for (int p = 0; p < EQ_MAX_PRESETS; p++) {
        for (int b = 0; b < EQ_BANDS; b++) {
            if (cfg.presets[p].band_gains[b] < -15.0f) cfg.presets[p].band_gains[b] = -15.0f;
            if (cfg.presets[p].band_gains[b] >  15.0f) cfg.presets[p].band_gains[b] =  15.0f;
        }
        if (cfg.presets[p].overall_gain < -30.0f) cfg.presets[p].overall_gain = -30.0f;
        if (cfg.presets[p].overall_gain >  30.0f) cfg.presets[p].overall_gain =  30.0f;
    }

    s_config = cfg;

    int next_idx = 1 - s_active_idx;
    EqState *next_eq = s_eq_pool[next_idx];
    if (next_eq) {
        recalculate_coeffs(next_eq, &s_config, s_sample_rate);
        s_active_eq = next_eq;
        s_active_idx = next_idx;
    }

    // Agenda persistência diferencial em NVS 1.5s após o último ajuste para não travar o cache SPI
    if (s_eq_nvs_timer) {
        esp_timer_stop(s_eq_nvs_timer);
        esp_timer_start_once(s_eq_nvs_timer, 1500000ULL);
    }
}

void eq_get_config(eq_config_t *config)
{
    if (config) *config = s_config;
}

void eq_reset_to_defaults(void)
{
    eq_config_t defaults = {};
    strlcpy(defaults.presets[0].name, "Padrao", EQ_PRESET_NAME_LEN);
    for (int p = 1; p < EQ_MAX_PRESETS; p++) {
        defaults.presets[p].name[0] = '\0';
    }
    defaults.enabled = false;
    eq_update_config(&defaults);
}

#define EQ_TILE_PAIRS 64

// Maior float IEEE-754 de 32 bits menor ou igual a INT32_MAX (2147483647):
// 2147483647.0f arredonda para +2147483648.0f (0x4F000000), que ao converter para int32_t
// estoura INT32_MAX e vira INT32_MIN (-2147483648), gerando estampidos violentos em 0 dBFS!
// 2147483520.0f (0x4EFFFFFF = 0x1.fffffep30f) é o maior float exato <= INT32_MAX.
static constexpr float SCALE_Q31_SAFE_MAX = 2147483520.0f;
static constexpr float INV_2_31           = 1.0f / 2147483648.0f;

bool IRAM_ATTR eq_process_with_gain(int32_t *buffer, size_t samples, float gain_l, float gain_r)
{
    EqState *eq = s_active_eq;
    if (!eq || !eq->enabled || eq->is_flat || !buffer || samples < 2) return false;

    size_t num_pairs = samples >> 1; // pares estereo L+R
    const float overall = eq->overall_gain;
    const int num_active = eq->num_active_bands;

    const float in_scale_L = INV_2_31 * gain_l;
    const float in_scale_R = INV_2_31 * gain_r;
    const float out_scale  = overall * SCALE_Q31_SAFE_MAX;

    // Fast-path: se apenas o ganho geral (Pre-Amp) estiver ativo e todas as 10 bandas em 0 dB
    if (num_active == 0) {
        const float tot_L = in_scale_L * out_scale;
        const float tot_R = in_scale_R * out_scale;
        for (size_t i = 0; i < num_pairs; i++) {
            float yL = (float)buffer[i * 2]     * tot_L;
            float yR = (float)buffer[i * 2 + 1] * tot_R;
            if (yL >  SCALE_Q31_SAFE_MAX) yL =  SCALE_Q31_SAFE_MAX;
            if (yL < -SCALE_Q31_SAFE_MAX) yL = -SCALE_Q31_SAFE_MAX;
            if (yR >  SCALE_Q31_SAFE_MAX) yR =  SCALE_Q31_SAFE_MAX;
            if (yR < -SCALE_Q31_SAFE_MAX) yR = -SCALE_Q31_SAFE_MAX;
            buffer[i * 2]     = (int32_t)yL;
            buffer[i * 2 + 1] = (int32_t)yR;
        }
        return true;
    }

    // Processamento em blocos (Tiles de 64 pares L/R na stack em SRAM interna):
    // 1. Mantem coeficientes SVF (a1,a2,g2,m1) e estados (ic1L,ic2L,ic1R,ic2R) em registradores
    //    da FPU durante todo o tile de 64 amostras.
    // 2. Intercala canais L e R na mesma iteracao do loop interno para esconder a latencia de
    //    4 ciclos de pipeline da FPU Xtensa LX7 (zero stalls RAW).
    // 3. Apenas 4 multiplicacoes (madd.s) por amostra por banda ativa!
    float bufL[EQ_TILE_PAIRS];
    float bufR[EQ_TILE_PAIRS];

    size_t pair_idx = 0;
    while (pair_idx < num_pairs) {
        size_t count = num_pairs - pair_idx;
        if (count > EQ_TILE_PAIRS) count = EQ_TILE_PAIRS;

        int32_t *tile_ptr = &buffer[pair_idx * 2];

        // 1. Desentrelaça, aplica volume/balanço L/R e converte int32 Q31 -> float [-1.0, 1.0]
        for (size_t i = 0; i < count; i++) {
            bufL[i] = (float)tile_ptr[i * 2]     * in_scale_L;
            bufR[i] = (float)tile_ptr[i * 2 + 1] * in_scale_R;
        }

        // 2. Loop externo apenas pelas bandas ativas (gain != 0 dB) usando SVF Trapezoidal Linear
        for (int idx = 0; idx < num_active; idx++) {
            const uint8_t b = eq->active_bands[idx];
            const float a1 = eq->coeffs[b].a1;
            const float a2 = eq->coeffs[b].a2;
            const float g2 = eq->coeffs[b].g2;
            const float m1 = eq->coeffs[b].m1;

            float ic1L = s_eq_states[b][0].ic1;
            float ic2L = s_eq_states[b][0].ic2;
            float ic1R = s_eq_states[b][1].ic1;
            float ic2R = s_eq_states[b][1].ic2;

            // Loop interno com canais L e R intercalados (ILP na FPU Xtensa LX7)
            for (size_t i = 0; i < count; i++) {
                float xL = bufL[i];
                float xR = bufR[i];

                float v3L = xL - ic2L;
                float v3R = xR - ic2R;

                float v1L = a1 * ic1L + a2 * v3L;
                float v1R = a1 * ic1R + a2 * v3R;

                ic1L = v1L + v1L - ic1L;
                ic1R = v1R + v1R - ic1R;

                ic2L = ic2L + g2 * v1L;
                ic2R = ic2R + g2 * v1R;

                bufL[i] = xL + m1 * v1L;
                bufR[i] = xR + m1 * v1R;
            }

            s_eq_states[b][0].ic1 = ic1L;
            s_eq_states[b][0].ic2 = ic2L;
            s_eq_states[b][1].ic1 = ic1R;
            s_eq_states[b][1].ic2 = ic2R;
        }

        // 3. Aplica ganho geral, hard-clamp seguro [-SCALE_Q31_SAFE_MAX, +SCALE_Q31_SAFE_MAX] e grava Q31
        for (size_t i = 0; i < count; i++) {
            float yL = bufL[i] * out_scale;
            float yR = bufR[i] * out_scale;

            if (yL >  SCALE_Q31_SAFE_MAX) yL =  SCALE_Q31_SAFE_MAX;
            if (yL < -SCALE_Q31_SAFE_MAX) yL = -SCALE_Q31_SAFE_MAX;
            if (yR >  SCALE_Q31_SAFE_MAX) yR =  SCALE_Q31_SAFE_MAX;
            if (yR < -SCALE_Q31_SAFE_MAX) yR = -SCALE_Q31_SAFE_MAX;

            tile_ptr[i * 2]     = (int32_t)yL;
            tile_ptr[i * 2 + 1] = (int32_t)yR;
        }

        pair_idx += count;
    }

    return true;
}

void IRAM_ATTR eq_process(int32_t *buffer, size_t samples)
{
    (void)eq_process_with_gain(buffer, samples, 1.0f, 1.0f);
}
