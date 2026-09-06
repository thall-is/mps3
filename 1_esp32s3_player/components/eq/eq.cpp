// Equalizador de 5 bandas usando filtros biquad IIR
// Implementação puramente em software, sem dependência de biblioteca
// externa. Cada banda usa um filtro "peaking EQ" (RBJ Audio EQ Cookbook,
// seção "Peaking EQ filter"), que aplica ganho/atenuação apenas na
// vizinhança da frequência central sem distorcer as outras bandas.
//
// Formato do buffer: int32_t, left-justified (32 bits por amostra),
// intercalado L,R,L,R,... — mesmo formato usado em todo o pipeline.
// O EQ é aplicado in-place (mesmo buffer de entrada e saída).
//
// Thread safety: eq_update_config() copia os coeficientes de forma
// atomica antes de substituir o ponteiro interno. A task de áudio
// (player_task, core 1) só lê o ponteiro/coeficientes depois de qualquer
// escrita da touch_task — em arquitetura ARM/Xtensa, a escrita de um
// ponteiro de 32 bits é atômica, então não usamos mutex aqui para não
// atrasar o caminho crítico de áudio.

#include "eq.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <math.h>

static const char *TAG = "eq";

// NVS — namespace e chaves dos ganhos do EQ
#define EQ_NVS_NAMESPACE  "mps3"
// NVS — namespace e chaves dos ganhos do EQ
#define EQ_NVS_NAMESPACE  "mps3"
#define EQ_NVS_KEY_ENABLED "eq_enabled"
#define EQ_NVS_KEY_OVERALL "eq_overall"
#define EQ_NVS_KEY_PRESET "eq_preset"
// Usaremos um prefixo pra gerar as chaves das 10 bandas dinamicamente: "eq_b0" a "eq_b9"

// Coeficientes de um filtro biquad de 2ª ordem (forma direta II transposta)
// y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
typedef struct {
    float b0, b1, b2; // numerador
    float a1, a2;     // denominador (a0 normalizado para 1.0)
} BiquadCoeffs;

// Estado de delay de um filtro biquad (por canal)
typedef struct {
    float z1, z2; // w[n-1], w[n-2] na forma direta II transposta
} BiquadState;

// Frequências centrais das 10 bandas (Hz)
static const float BAND_FREQ[EQ_BANDS] = {
    31.0f, 62.0f, 125.0f, 250.0f, 500.0f,
    1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f
};

// Q factor fixo para as bandas (1.41 = meia oitava, bom para EQ grafico de 10 bandas)
static const float BAND_Q = 1.41f;

// Filtro biquad Peaking EQ (RBJ Cookbook):
//   H(s) = (s^2 + s*(dBgain/V)/Q*w0 + w0^2) / (s^2 + s*w0/(Q*V) + w0^2)
// onde V = 10^(|dBgain|/20), w0 = 2*PI*f0/Fs
static void compute_peaking_biquad(float freq_hz, float q, float gain_db,
                                    float sample_rate, BiquadCoeffs *c)
{
    if (sample_rate <= 0.0f) sample_rate = 44100.0f;

    // Se freq_hz >= Nyquist * 0.9 (freq_hz >= sample_rate * 0.45f), o filtro ultrapassa o limite de Nyquist.
    // Nesse caso, o biquad deve ser configurado como pass-through wire puro para evitar polos instaveis (|a2/a0| > 1).
    if (freq_hz >= sample_rate * 0.45f) {
        c->b0 = 1.0f;
        c->b1 = 0.0f;
        c->b2 = 0.0f;
        c->a1 = 0.0f;
        c->a2 = 0.0f;
        return;
    }

    float A  = powf(10.0f, gain_db / 40.0f); // sqrt(10^(gain/20))
    float w0 = 2.0f * (float)M_PI * freq_hz / sample_rate;
    float sinw = sinf(w0);
    float cosw = cosf(w0);
    float alpha = sinw / (2.0f * q);

    float b0 =  1.0f + alpha * A;
    float b1 = -2.0f * cosw;
    float b2 =  1.0f - alpha * A;
    float a0 =  1.0f + alpha / A;
    float a1 = -2.0f * cosw;
    float a2 =  1.0f - alpha / A;

    c->b0 = b0 / a0;
    c->b1 = b1 / a0;
    c->b2 = b2 / a0;
    c->a1 = a1 / a0;
    c->a2 = a2 / a0;
}

// Bloco de filtros para TODOS os canais (máximo 2 = estéreo)
typedef struct {
    BiquadCoeffs coeffs[EQ_BANDS];
    BiquadState  states[EQ_BANDS][2]; // [banda][canal 0=L 1=R]
    float        overall_gain;            // multiplicador linear final (10^(overall_db/20))
    bool         enabled;
    bool         is_flat;
} EqState;

// Ping-pong double-buffering para troca 100% atomica de coeficientes sem travar o Core 1
static EqState *s_eq_pool[2] = {NULL, NULL};
static volatile int s_active_idx = 0;
static EqState * volatile s_active_eq = NULL;

// Configuração corrente (protegida por escrita atômica de ponteiro)
static eq_config_t s_config; // zero-initialized by BSS

// Sample rate descoberto ao abrir a primeira faixa — precisamos para
// recalcular os coeficientes quando o EQ é configurado ANTES de tocar.
// Inicializado com o valor mais comum; será atualizado por eq_set_sample_rate().
static float s_sample_rate = 44100.0f;

// Calcula todos os coeficientes biquad a partir da config atual
static void recalculate_coeffs(EqState *eq, const eq_config_t *cfg, float sr)
{
    int p = cfg->active_preset_idx;
    if (p < 0 || p >= EQ_MAX_PRESETS) p = 0;
    bool flat = true;
    for (int b = 0; b < EQ_BANDS; b++) {
        compute_peaking_biquad(BAND_FREQ[b], BAND_Q, cfg->presets[p].band_gains[b], sr, &eq->coeffs[b]);
        if (cfg->presets[p].band_gains[b] != 0.0f) flat = false;
    }
    float o = cfg->presets[p].overall_gain;
    if (o != 0.0f) flat = false;
    eq->overall_gain = (o == 0.0f) ? 1.0f : powf(10.0f, o / 20.0f);
    eq->enabled = cfg->enabled;
    eq->is_flat = flat;
}

// Aplica UM filtro biquad a UMA amostra (estado por canal embutido)
static inline float biquad_process(const BiquadCoeffs *c, BiquadState *s, float x)
{
    // Forma direta II transposta: numericamente mais estável que DF-I
    float y = c->b0 * x + s->z1;
    s->z1   = c->b1 * x - c->a1 * y + s->z2;
    s->z2   = c->b2 * x - c->a2 * y;
    return y;
}

// -------------------------------------------------------------------------
// API pública
// -------------------------------------------------------------------------

void eq_reset_state(void)
{
    for (int i = 0; i < 2; i++) {
        if (s_eq_pool[i]) {
            memset(s_eq_pool[i]->states, 0, sizeof(s_eq_pool[i]->states));
        }
    }
}

void eq_init(void)
{
    if (s_active_eq) return;

    s_eq_pool[0] = (EqState *)heap_caps_calloc(1, sizeof(EqState),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_eq_pool[1] = (EqState *)heap_caps_calloc(1, sizeof(EqState),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_eq_pool[0] || !s_eq_pool[1]) {
        ESP_LOGE(TAG, "Sem memoria para o equalizador");
        return;
    }

    eq_config_t cfg = {};
    for(int p=0; p<EQ_MAX_PRESETS; p++) {
        snprintf(cfg.presets[p].name, EQ_PRESET_NAME_LEN, "Preset %d", p+1);
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
            nvs_get_str(h, key_name, cfg.presets[p].name, &len);
            
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
    recalculate_coeffs(s_eq_pool[0], &s_config, s_sample_rate);
    recalculate_coeffs(s_eq_pool[1], &s_config, s_sample_rate);
    s_active_idx = 0;
    s_active_eq = s_eq_pool[0];
    ESP_LOGI(TAG, "Equalizador inicializado com double-buffer");
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
        EqState *cur_eq = s_eq_pool[s_active_idx];
        if (cur_eq) {
            memcpy(next_eq->states, cur_eq->states, sizeof(next_eq->states));
        }
        recalculate_coeffs(next_eq, &s_config, s_sample_rate);
        s_active_eq = next_eq;
        s_active_idx = next_idx;
        ESP_LOGI(TAG, "EQ: coeficientes recalculados para %u Hz", (unsigned)sample_rate);
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
        EqState *cur_eq = s_eq_pool[s_active_idx];
        if (cur_eq) {
            memcpy(next_eq->states, cur_eq->states, sizeof(next_eq->states));
        }
        recalculate_coeffs(next_eq, &s_config, s_sample_rate);
        s_active_eq = next_eq;
        s_active_idx = next_idx;
    }

    nvs_handle_t h;
    if (nvs_open(EQ_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        for (int p = 0; p < EQ_MAX_PRESETS; p++) {
            char key_name[16];
            snprintf(key_name, sizeof(key_name), "p%d_name", p);
            nvs_set_str(h, key_name, cfg.presets[p].name);
            
            char key_ovr[16];
            snprintf(key_ovr, sizeof(key_ovr), "p%d_ovr", p);
            nvs_set_i32(h, key_ovr, (int32_t)(cfg.presets[p].overall_gain * 10.0f));
            
            for (int b = 0; b < EQ_BANDS; b++) {
                char key[16];
                snprintf(key, sizeof(key), "p%d_b%d", p, b);
                nvs_set_i32(h, key, (int32_t)(cfg.presets[p].band_gains[b] * 10.0f));
            }
        }
        nvs_set_i32(h, EQ_NVS_KEY_PRESET, cfg.active_preset_idx);
        nvs_set_i32(h, EQ_NVS_KEY_ENABLED, cfg.enabled ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

void eq_get_config(eq_config_t *config)
{
    if (config) *config = s_config;
}

void eq_reset_to_defaults(void)
{
    eq_config_t defaults = {};
    for (int p = 0; p < EQ_MAX_PRESETS; p++) {
        snprintf(defaults.presets[p].name, EQ_PRESET_NAME_LEN, "Preset %d", p+1);
    }
    defaults.enabled = false;
    eq_update_config(&defaults);
}

void eq_process(int32_t *buffer, size_t samples)
{
    if (s_sample_rate > 48000) return;

    EqState *eq = s_active_eq;
    if (!eq || !eq->enabled || eq->is_flat || !buffer || samples == 0) return;

    // O pipeline de áudio entrega samples intercaladas L,R,L,R,...
    // com 2 canais (o mono já foi expandido para stereo antes desta
    // chamada em audio_player.cpp). Processamos par a par.
    size_t num_pairs = samples / 2; // pares L+R

    for (size_t i = 0; i < num_pairs; i++) {
        for (int ch = 0; ch < 2; ch++) {
            // Normaliza int32 left-justified para float [-1.0, 1.0]
            float x = (float)buffer[i * 2 + ch] / 2147483648.0f; // 2^31

            // Aplica as 5 bandas em cascata
            for (int b = 0; b < EQ_BANDS; b++) {
                x = biquad_process(&eq->coeffs[b],
                                   (BiquadState *)&eq->states[b][ch], x);
            }

            // Aplica ganho geral
            x *= eq->overall_gain;

            // Clamp para evitar overflow ao converter de volta
            if (x >  1.0f) x =  1.0f;
            if (x < -1.0f) x = -1.0f;

            // Devolve como int32 left-justified
            buffer[i * 2 + ch] = (int32_t)(x * 2147483647.0f); // 2^31 - 1
        }
    }
}
