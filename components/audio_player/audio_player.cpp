#include "audio_player.h"
#include "audio_player_internal.h"
#include "fs_browser.h"
#include "i2s_output.h"
#include "audio_decoder.h"
#include "esp_codec_decoder_adapter.h"
#include "metadata_extract.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec_default.h"
#include "eq.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <memory>
#include <cmath>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_http_client.h"

static const char *TAG = "audio_player";

// Buffer de entrada (dados comprimidos lidos do SD). So' precisa caber
// um frame de audio de cada vez - metadados (ID3v2, capa de album, tags
// FLAC) sao pulados ANTES do loop comecar (skip_leading_metadata()), sem
// precisar caber no buffer. 64KB da' bastante margem sobre o maior
// max_frame ja' visto na pratica (~17KB, FLAC 24 bits/96kHz).
#define INBUF_SIZE (64 * 1024)

// Duracao do fade-in aplicado no inicio de cada faixa e logo apos um
// avanco rapido terminar - suaviza estalos audiveis nessas transicoes.
#define FADE_IN_MS 15

#define MAX_ENTRIES 256   // por pasta (subpastas e arquivos de audio, cada um)

// Tamanhos de buffer de caminho - dimensionados com folga o bastante pra
// o GCC conseguir PROVAR estaticamente que nenhum snprintf/strncpy de
// concatenacao de caminho trunca (-Werror=format-truncation trata isso
// como erro, nao so' aviso). DIR_LEN e' o teto pra' s_browse_dir/
// s_playback_dir (a "posicao atual" persistente); PATH_LEN e' maior,
// usado em buffers TEMPORARIOS que concatenam um DIR_LEN inteiro + mais
// um componente (nome de pasta/arquivo, ate' 255 bytes no FAT LFN) - por
// isso PATH_LEN > DIR_LEN + 255 + folga.
#define DIR_LEN  512
#define PATH_LEN 1024
#define MAX_PATH_LEN PATH_LEN // mantido como alias - usado pros caminhos completos de arquivo abertos com fopen()

#define NVS_NAMESPACE "mps3"
#define NVS_KEY_LAST_PATH "last_path"       // caminho RELATIVO a' raiz (ex: "Rock/Album1/faixa.mp3")
#define NVS_KEY_LAST_ELAPSED "last_elapsed"
#define NVS_KEY_VOLUME "volume"
#define NVS_KEY_BALANCE "balance"
#define NVS_KEY_SORT_MODE "sort_mode"

// Espera o usuario terminar o ajuste de volume antes de escrever na flash.
// Isso evita uma escrita NVS repetitiva a cada alteracao rapida.
#define VOLUME_NVS_SAVE_DELAY_MS 1500

static char s_root_dir[128]; // raiz fixa do cartao SD, ex "/sdcard" - definida uma vez em audio_player_start()





// Nomes de pastas/arquivos "de sistema" que aparecem sozinhos em cartoes
// SD (criados pelo proprio driver FAT ou pelo Windows ao formatar/montar
// o cartao) - nao sao musica nem pastas que o usuario criou, entao nao
// fazem sentido no navegador. Comparacao sem diferenciar maiusculas.


// --- Escaneamento de diretorio --------------------------------------------
// Usado tanto pra' pasta que o MENU esta' mostrando quanto pra' pasta da
// playlist ATIVA (onde a faixa tocando esta') - sao instancias
// independentes, ver s_browse_* / s_playback_* abaixo.
static char s_browse_dir[DIR_LEN];
static DirScan s_browse_scan;

// --- Estado de REPRODUCAO (playlist ativa - pasta da faixa TOCANDO) ------
static char s_playback_dir[DIR_LEN];
static DirScan s_playback_scan;

// s_browse_scan/s_playback_scan guardam ponteiros char* alocados com
// strdup() por scan_dir(), e free_dir_scan() da' free() neles antes de
// realocar. Essas structs sao lidas e escritas de TRES tasks diferentes -
// touch_task (audio_player_select_entry chama scan_dir a qualquer
// momento), display_task (audio_player_get_browse_entry_name /
// audio_player_get_file_name, pra' desenhar a lista/"proximas faixas") e
// player_task (prefetch, avanco de faixa) - sem NENHUMA sincronizacao.
//
// BUG ENCONTRADO: isso e' uma corrida classica. Quando free_dir_scan() da'
// free() num ponteiro (pra' realocar com o conteudo novo), o alocador do
// heap sobrescreve os PRIMEIROS bytes desse bloco com metadado interno da
// free-list. Se display_task estiver lendo essa mesma string bem nesse
// instante (ou logo depois, antes do strdup() novo escrever por cima), os
// primeiros bytes da string somem/viram lixo - exatamente o "primeiro
// caractere nao aparece" visto ao selecionar um item. E se um ponteiro
// fica retido por mais tempo ainda (como acontecia com o array
// `upcoming` em main.c, guardando o char* direto em vez de copiar - ver
// audio_player_get_file_name), a janela de corrida e' bem maior: o bloco
// pode ser sido liberado E realocado pra outra coisa por completo antes
// da leitura acontecer, produzindo lixo binario "aleatorio" (mas
// deterministico pra' uma mesma sequencia de alocacoes) - o padrao de
// "ruido, sempre identico, sem heap poisoning acusar nada" relatado.
static SemaphoreHandle_t s_scan_mutex = NULL;
static void scan_lock(void)   { xSemaphoreTake(s_scan_mutex, portMAX_DELAY); }
static void scan_unlock(void) { xSemaphoreGive(s_scan_mutex); }

SemaphoreHandle_t s_state_mutex = NULL;
playback_state_t s_state = {};

// --- Play/Pause e volume (lidos/escritos pela task de touch e pela task
// de reproducao; tipos simples com leitura/escrita atomica em ESP32/
// FreeRTOS, entao nao usamos mutex aqui pra nao gastar tempo no caminho
// critico de audio). ---
volatile bool s_paused = false;
volatile int s_volume_percent = 100;
static volatile bool s_volume_nvs_dirty = false;
static volatile TickType_t s_volume_last_change_tick = 0;
static portMUX_TYPE s_volume_nvs_mux = portMUX_INITIALIZER_UNLOCKED;

volatile int s_balance = 0; // -100 a +100. 0 = Centro
static volatile bool s_balance_nvs_dirty = false;
static volatile TickType_t s_balance_last_change_tick = 0;
static portMUX_TYPE s_balance_nvs_mux = portMUX_INITIALIZER_UNLOCKED;

static volatile int s_current_index = 0;
static volatile uint32_t s_last_elapsed_sec = 0; // espelho sem lock, usado pelo seek e pelo pause

// Avanco rapido (seek) por decodificacao+descarte - declarado aqui (cedo)
// porque audio_player_toggle_play_pause() precisa cancelar um seek em
// andamento ao pausar/retomar.
static volatile uint32_t s_seek_target_sec = 0;
static volatile bool s_seek_pending = false;

// Hooks pro modo de armazenamento USB (ver audio_player.h) - declarados
// aqui pelo mesmo motivo.
static volatile bool s_usb_takeover_requested = false;
volatile bool s_usb_takeover_active = false;
static TaskHandle_t s_player_task_handle = NULL;

void state_lock(void)   { xSemaphoreTake(s_state_mutex, portMAX_DELAY); }
void state_unlock(void) { xSemaphoreGive(s_state_mutex); }

// Forward declarations - definidas mais abaixo, usadas antes por
// audio_player_toggle_play_pause().
static void save_resume_state(const char *relative_path, uint32_t elapsed_sec);
static void save_volume(int volume);
static void load_volume(void);
static void save_balance(int balance);
static void load_balance(void);
static void build_relative_path(const char *abs_dir, const char *filename, char *out, size_t out_len);

void audio_player_get_state(playback_state_t *out_state)
{
    if (!out_state) return;
    if (!s_state_mutex) {
        memset(out_state, 0, sizeof(*out_state));
        return;
    }
    state_lock();
    *out_state = s_state;
    out_state->paused = s_paused;
    state_unlock();
}

void audio_player_toggle_play_pause(void)
{
    // Cancela qualquer avanco rapido (seek) em andamento. Sem isso, tocar
    // em play/pause durante um "segurar pad[6] pra avancar" so' pausava/
    // retomava o proprio avanco silencioso - dava a impressao de estar
    // "preso" sem conseguir parar.
    s_seek_pending = false;

    if (!s_paused) {
        s_paused = true;
        i2s_output_disable(); // silencio real, nao so' para de escrever amostras
        // Salva a posicao ao pausar - momento provavel de "vou desligar
        // daqui a pouco", entao vale capturar aqui alem do save periodico.
        scan_lock();
        int idx = s_current_index;
        if (idx >= 0 && idx < s_playback_scan.audio_count) {
            char rel[PATH_LEN];
            build_relative_path(s_playback_dir, s_playback_scan.audio_files[idx], rel, sizeof(rel));
            scan_unlock();
            save_resume_state(rel, s_last_elapsed_sec);
        } else {
            scan_unlock();
        }
    } else {
        i2s_output_enable();
        s_paused = false;
    }
    ESP_LOGI(TAG, "%s", s_paused ? "Pausado" : "Tocando");
}

static audio_volume_change_cb_t s_volume_change_cb = NULL;

void audio_player_set_volume_change_cb(audio_volume_change_cb_t cb)
{
    s_volume_change_cb = cb;
}

int audio_player_get_volume(void) { return s_volume_percent; }

void audio_player_adjust_volume(int delta)
{
    int v = s_volume_percent + delta;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (v == s_volume_percent) return;

    s_volume_percent = v;
    portENTER_CRITICAL(&s_volume_nvs_mux);
    s_volume_last_change_tick = xTaskGetTickCount();
    s_volume_nvs_dirty = true;
    portEXIT_CRITICAL(&s_volume_nvs_mux);

    if (s_volume_change_cb) {
        s_volume_change_cb(s_volume_percent);
    }
}

void audio_player_set_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    if (volume == s_volume_percent) return;

    s_volume_percent = volume;
    portENTER_CRITICAL(&s_volume_nvs_mux);
    s_volume_last_change_tick = xTaskGetTickCount();
    s_volume_nvs_dirty = true;
    portEXIT_CRITICAL(&s_volume_nvs_mux);
}

int audio_player_get_balance(void) { return s_balance; }

void audio_player_set_balance(int balance)
{
    if (balance < -100) balance = -100;
    if (balance > 100) balance = 100;
    if (balance == s_balance) return;

    s_balance = balance;
    portENTER_CRITICAL(&s_balance_nvs_mux);
    s_balance_last_change_tick = xTaskGetTickCount();
    s_balance_nvs_dirty = true;
    portEXIT_CRITICAL(&s_balance_nvs_mux);
}

void audio_player_adjust_balance(int delta)
{
    audio_player_set_balance(s_balance + delta);
}

// Curva de volume logaritmica: a percepcao humana de intensidade sonora
// e' aproximadamente logaritmica, entao um controle LINEAR de amplitude
// soa "tudo alto" ja' nos primeiros 20-30% do curso. Mapeamos 0-100% pra
// uma faixa de -40dB a 0dB (ganho 1.0), com curva exponencial entre os dois.
#define VOLUME_MIN_DB (-40.0f)

static float volume_to_gain(int percent)
{
    if (percent <= 0) return 0.0f;
    if (percent >= 100) return 1.0f;
    float db = VOLUME_MIN_DB * (1.0f - (float)percent / 100.0f);
    return powf(10.0f, db / 20.0f);
}

// =============================================================================
// PIPELINE DUAL-CORE / BOOST: Core 0 (DSP & I2S Output)
// =============================================================================
#define DSP_BLOCK_FRAMES  512 // 512 quadros stereo = 1024 amostras int32_t (4096 bytes)
#define DSP_BLOCK_SAMPLES (DSP_BLOCK_FRAMES * 2)
#define DSP_NUM_BLOCKS    16  // 16 blocos individuais (64 KB total) em SRAM interna

typedef struct {
    int32_t samples[DSP_BLOCK_SAMPLES];
    size_t  sample_count; // Amostras int32_t validas (sempre par)
    uint32_t sample_rate; // Taxa de amostragem deste bloco
    bool    is_flush;     // Solicitacao de reset/limpeza de filtros biquad
} dsp_block_t;

static QueueHandle_t s_dsp_free_queue = NULL;
static QueueHandle_t s_dsp_ready_queue = NULL;
static TaskHandle_t s_audio_dsp_task_handle = NULL;
static dsp_block_t *s_dsp_blocks[DSP_NUM_BLOCKS] = {NULL};
static volatile uint32_t s_dsp_current_rate = 44100;
static size_t s_dsp_fade_in_total = 0;
static size_t s_dsp_fade_in_remaining = 0;

void audio_dsp_trigger_fade_in(uint32_t sample_rate)
{
    size_t total = (sample_rate * FADE_IN_MS / 1000) * 2;
    s_dsp_fade_in_total = total;
    s_dsp_fade_in_remaining = total;
}

void audio_dsp_set_rate(uint32_t rate)
{
    if (rate == 0) return;
    if (s_dsp_current_rate != rate) {
        s_dsp_current_rate = 0; // Força reconfiguração sincronizada no Core 0 pela audio_dsp_task
    }
}

void audio_dsp_flush(void)
{
    if (!s_dsp_ready_queue || !s_dsp_free_queue) return;
    dsp_block_t *blk = NULL;
    while (xQueueReceive(s_dsp_ready_queue, &blk, 0) == pdTRUE) {
        if (blk) {
            xQueueSend(s_dsp_free_queue, &blk, 0);
        }
    }
    eq_reset_state();
}

void audio_dsp_drain(void)
{
    if (!s_dsp_ready_queue) return;
    if (s_pending_cmd != PLAYER_CMD_NONE || s_usb_takeover_requested) {
        audio_dsp_flush();
        return;
    }
    for (int i = 0; i < 50 && uxQueueMessagesWaiting(s_dsp_ready_queue) > 0; i++) {
        if (s_pending_cmd != PLAYER_CMD_NONE || s_usb_takeover_requested) {
            audio_dsp_flush();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t audio_dsp_send_pcm(const int32_t *samples, size_t count, uint32_t rate)
{
    if (!samples || count == 0) return ESP_OK;

    while (count > 0) {
        if (s_usb_takeover_requested || s_pending_cmd != PLAYER_CMD_NONE) {
            return ESP_ERR_INVALID_STATE;
        }

        dsp_block_t *blk = NULL;
        if (xQueueReceive(s_dsp_free_queue, &blk, pdMS_TO_TICKS(50)) != pdTRUE) {
            if (s_usb_takeover_requested || s_pending_cmd != PLAYER_CMD_NONE) {
                return ESP_ERR_INVALID_STATE;
            }
            continue;
        }

        size_t to_copy = (count > DSP_BLOCK_SAMPLES) ? DSP_BLOCK_SAMPLES : count;
        to_copy &= ~1UL; // Amostras sempre em pares stereo completos
        if (to_copy == 0) {
            xQueueSend(s_dsp_free_queue, &blk, 0);
            break;
        }

        memcpy(blk->samples, samples, to_copy * sizeof(int32_t));
        blk->sample_count = to_copy;
        blk->sample_rate = rate;
        blk->is_flush = false;

        if (xQueueSend(s_dsp_ready_queue, &blk, pdMS_TO_TICKS(100)) != pdTRUE) {
            xQueueSend(s_dsp_free_queue, &blk, 0);
            return ESP_FAIL;
        }

        samples += to_copy;
        count -= to_copy;
    }
    return ESP_OK;
}

static void audio_dsp_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "audio_dsp_task iniciada no Core %d (prioridade %d)",
             xPortGetCoreID(), (int)uxTaskPriorityGet(NULL));

    static int s_cached_vol = -1;
    static int s_cached_bal = 999;
    static int32_t s_cached_final_L = 32768;
    static int32_t s_cached_final_R = 32768;

    while (true) {
        if (s_usb_takeover_requested) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        dsp_block_t *blk = NULL;
        if (xQueueReceive(s_dsp_ready_queue, &blk, pdMS_TO_TICKS(20)) != pdTRUE) {
            continue;
        }

        if (!blk) continue;

        if (blk->is_flush) {
            eq_reset_state();
            xQueueSend(s_dsp_free_queue, &blk, 0);
            taskYIELD();
            continue;
        }

        size_t samples_to_write = blk->sample_count;
        int32_t *to_write = blk->samples;

        if (samples_to_write > 0) {
            // Sincronizacao dinamica de clock I2S e EQ
            if (blk->sample_rate > 0 && blk->sample_rate != s_dsp_current_rate) {
                s_dsp_current_rate = blk->sample_rate;
                i2s_output_set_rate(s_dsp_current_rate);
                eq_set_sample_rate(s_dsp_current_rate);
            }

            // Volume e Balanço
            int vol = s_volume_percent;
            int bal = s_balance;
            if (vol != s_cached_vol || bal != s_cached_bal) {
                s_cached_vol = vol;
                s_cached_bal = bal;

                float vol_gain = volume_to_gain(vol);
                int32_t vol_mult = (int32_t)(vol_gain * 32768.0f + 0.5f);
                if (vol_mult > 32768) vol_mult = 32768;
                if (vol_mult < 0) vol_mult = 0;

                int pct_L = 100;
                int pct_R = 100;
                if (bal < 0) {
                    pct_R = 100 + bal;
                } else if (bal > 0) {
                    pct_L = 100 - bal;
                }
                if (pct_L < 0) pct_L = 0;
                if (pct_R < 0) pct_R = 0;
                if (pct_L > 100) pct_L = 100;
                if (pct_R > 100) pct_R = 100;

                int32_t bal_mult_L = (pct_L <= 0) ? 0 : ((pct_L >= 100) ? 32768 : (int32_t)(((int64_t)pct_L * pct_L * 32768) / 10000));
                int32_t bal_mult_R = (pct_R <= 0) ? 0 : ((pct_R >= 100) ? 32768 : (int32_t)(((int64_t)pct_R * pct_R * 32768) / 10000));

                s_cached_final_L = (int32_t)(((int64_t)vol_mult * bal_mult_L) >> 15);
                s_cached_final_R = (int32_t)(((int64_t)vol_mult * bal_mult_R) >> 15);
            }

            int32_t final_L = s_cached_final_L;
            int32_t final_R = s_cached_final_R;

            // Fade-in suave de transição
            if (s_dsp_fade_in_remaining > 0) {
                for (size_t i = 0; i < samples_to_write; i += 2) {
                    float fade = (s_dsp_fade_in_total > 0)
                        ? 1.0f - ((float)s_dsp_fade_in_remaining / (float)s_dsp_fade_in_total)
                        : 1.0f;
                    int32_t fade_q15 = (int32_t)(fade * 32768.0f + 0.5f);
                    if (fade_q15 > 32768) fade_q15 = 32768;
                    if (fade_q15 < 0) fade_q15 = 0;
                    int32_t f_L = (int32_t)(((int64_t)final_L * fade_q15) >> 15);
                    int32_t f_R = (int32_t)(((int64_t)final_R * fade_q15) >> 15);
                    to_write[i]     = (int32_t)(((int64_t)to_write[i]     * f_L) >> 15);
                    if (i + 1 < samples_to_write) {
                        to_write[i + 1] = (int32_t)(((int64_t)to_write[i + 1] * f_R) >> 15);
                    }
                    if (s_dsp_fade_in_remaining > 0) s_dsp_fade_in_remaining--;
                }
            } else if (final_L < 32768 || final_R < 32768) {
                for (size_t i = 0; i < samples_to_write; i += 2) {
                    to_write[i]     = (int32_t)(((int64_t)to_write[i]     * final_L) >> 15);
                    if (i + 1 < samples_to_write) {
                        to_write[i + 1] = (int32_t)(((int64_t)to_write[i + 1] * final_R) >> 15);
                    }
                }
            }

            // Equalizador gráfico de 10 bandas executado inteiramente no Core 0
            eq_process(to_write, samples_to_write);

            // Transmissão direta aos descritores DMA do I2S
            size_t written = 0;
            i2s_output_write(to_write, samples_to_write, &written);
        }

        xQueueSend(s_dsp_free_queue, &blk, portMAX_DELAY);
        taskYIELD();
    }
}

static esp_err_t audio_dsp_init(void)
{
    if (s_dsp_free_queue != NULL) return ESP_OK;

    s_dsp_free_queue = xQueueCreate(DSP_NUM_BLOCKS, sizeof(dsp_block_t *));
    s_dsp_ready_queue = xQueueCreate(DSP_NUM_BLOCKS, sizeof(dsp_block_t *));
    if (!s_dsp_free_queue || !s_dsp_ready_queue) {
        ESP_LOGE(TAG, "Falha ao criar filas do audio_dsp");
        return ESP_ERR_NO_MEM;
    }

    int internal_count = 0;
    for (int i = 0; i < DSP_NUM_BLOCKS; i++) {
        s_dsp_blocks[i] = (dsp_block_t *)heap_caps_calloc(1, sizeof(dsp_block_t),
                                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (s_dsp_blocks[i]) {
            internal_count++;
        } else {
            ESP_LOGW(TAG, "SRAM interna esgotada no bloco %d, tentando SPIRAM", i);
            s_dsp_blocks[i] = (dsp_block_t *)heap_caps_calloc(1, sizeof(dsp_block_t),
                                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!s_dsp_blocks[i]) {
                s_dsp_blocks[i] = (dsp_block_t *)calloc(1, sizeof(dsp_block_t));
            }
        }
        if (!s_dsp_blocks[i]) {
            ESP_LOGE(TAG, "Sem memoria para bloco DSP %d", i);
            return ESP_ERR_NO_MEM;
        }
        dsp_block_t *blk = s_dsp_blocks[i];
        xQueueSend(s_dsp_free_queue, &blk, 0);
    }

    BaseType_t ret = xTaskCreatePinnedToCore(audio_dsp_task, "audio_dsp",
                                             4096, NULL, 5, &s_audio_dsp_task_handle, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar audio_dsp_task no Core 0");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Pipeline Dual-Core inicializado: audio_dsp no Core 0, player no Core 1 (%d blocos de %u B; %d em SRAM interna)",
             DSP_NUM_BLOCKS, (unsigned)sizeof(dsp_block_t), internal_count);
    return ESP_OK;
}

// --- Comandos de navegacao pedidos pela UI -----------------------------


volatile player_cmd_t s_pending_cmd = PLAYER_CMD_NONE;
static volatile int s_pending_index = -1;

// Usado so' por audio_player_seek_backward(): como nao ha' seek de
// verdade pra tras, ela reusa PLAYER_CMD_RESTART (mesmo indice, comeca
// do zero) mas precisa avisar o player_task pra' NAO comecar do zero de
// verdade e sim ja' avancar rapido ate' essa posicao (ver o calculo de
// target_elapsed logo no topo do loop de player_task()).
static volatile uint32_t s_forced_restart_target_sec = 0;
static volatile bool s_forced_restart_pending = false;

void audio_player_next(void)     { s_pending_cmd = PLAYER_CMD_NEXT; }
void audio_player_previous(void) { s_pending_cmd = PLAYER_CMD_PREV; }
void audio_player_restart(void)  { s_pending_cmd = PLAYER_CMD_RESTART; }

static char s_pending_url[256];

esp_err_t audio_player_play_url(const char *url) {
    if (!url) return ESP_ERR_INVALID_ARG;
    strncpy(s_pending_url, url, sizeof(s_pending_url) - 1);
    s_pending_url[sizeof(s_pending_url) - 1] = '\0';
    s_pending_cmd = PLAYER_CMD_PLAY_URL;
    return ESP_OK;
}

void audio_player_stop_url(void) {
    s_pending_cmd = PLAYER_CMD_STOP_URL;
}

// --- Seek de verdade (fseek + estimativa por taxa media de bytes) --------
// Ambos os sentidos passam pelo mesmo mecanismo O(1) usado pelo resume:
// PLAYER_CMD_RESTART reabre o arquivo e pula direto pro offset de byte
// estimado (meta.avg_byte_rate, calculado em metadata_extract.c),
// SEM decodificar/descartar nada no meio do caminho. Essencial pra
// arquivos longos (podcasts de horas) - o tempo de um seek passa a
// depender so' da velocidade do cartao SD, nao da distancia pulada.
void audio_player_seek_forward(uint32_t seconds)
{
    state_lock();
    uint32_t current = s_state.elapsed_sec;
    uint32_t total = s_state.total_sec;

    uint32_t base = s_forced_restart_pending ? s_forced_restart_target_sec : current;
    uint32_t target = base + seconds;
    if (total > 0 && target >= total) {
        target = (total > 1) ? total - 1 : 0;
    }
    s_forced_restart_target_sec = target;
    s_forced_restart_pending = true;
    s_state.elapsed_sec = target;
    s_last_elapsed_sec = target;
    s_pending_cmd = PLAYER_CMD_RESTART;
    state_unlock();

    ESP_LOGI(TAG, "Seek Forward: base=%u s + %u s -> target=%u s (total=%u s)",
             (unsigned)base, (unsigned)seconds, (unsigned)target, (unsigned)total);
}

void audio_player_seek_backward(uint32_t seconds)
{
    state_lock();
    uint32_t current = s_state.elapsed_sec;

    uint32_t base = s_forced_restart_pending ? s_forced_restart_target_sec : current;
    uint32_t target = (seconds >= base) ? 0 : (base - seconds);
    s_forced_restart_target_sec = target;
    s_forced_restart_pending = true;
    s_state.elapsed_sec = target;
    s_last_elapsed_sec = target;
    s_pending_cmd = PLAYER_CMD_RESTART;
    state_unlock();

    ESP_LOGI(TAG, "Seek Backward: base=%u s - %u s -> target=%u s",
             (unsigned)base, (unsigned)seconds, (unsigned)target);
}

int audio_player_get_file_count(void)
{
    scan_lock();
    int n = s_playback_scan.audio_count;
    scan_unlock();
    return n;
}

// Copia o nome pro buffer do chamador (em vez de devolver o char* interno
// direto) - esse ponteiro pode ser liberado por scan_dir() a qualquer
// momento, de outra task, entao devolve-lo cru nao e' seguro (era
// exatamente isso, usado em main.c pro array de "proximas faixas", que
// causava a corrupcao intermitente: o ponteiro ficava retido bem depois
// do lock, entao podia apontar pra memoria ja' liberada/realocada na hora
// de desenhar na tela).
void audio_player_get_file_name(int index, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    scan_lock();
    if (index < 0 || index >= s_playback_scan.audio_count) {
        out[0] = '\0';
    } else {
        snprintf(out, out_len, "%s", s_playback_scan.audio_files[index]);
    }
    scan_unlock();
}

int audio_player_get_current_index(void) { return s_current_index; }

// --- Navegacao de pastas (usada pelo menu) --------------------------------
static bool browse_has_parent(void) { return strcmp(s_browse_dir, s_root_dir) != 0; }

// Usado por main.c pra' saber quando injetar a entrada sintetica "Modo USB"
// no topo da lista (so' faz sentido na raiz - dentro de subpastas ela
// ficaria confusa/repetida a cada nivel).
bool audio_player_browse_is_root(void)
{
    scan_lock();
    bool is_root = !browse_has_parent();
    scan_unlock();
    return is_root;
}

int audio_player_get_browse_entry_count(void)
{
    scan_lock();
    int n = (browse_has_parent() ? 1 : 0) + s_browse_scan.subdir_count + s_browse_scan.audio_count;
    scan_unlock();
    return n;
}

void audio_player_get_browse_entry_name(int index, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;

    scan_lock();

    bool has_parent = browse_has_parent();
    int offset = has_parent ? 1 : 0;

    if (has_parent && index == 0) {
        snprintf(out, out_len, "..");
        scan_unlock();
        return;
    }

    int i = index - offset;
    if (i >= 0 && i < s_browse_scan.subdir_count) {
        snprintf(out, out_len, "%s/", s_browse_scan.subdirs[i]);
        scan_unlock();
        return;
    }
    i -= s_browse_scan.subdir_count;
    if (i >= 0 && i < s_browse_scan.audio_count) {
        snprintf(out, out_len, "%s", s_browse_scan.audio_files[i]);
        scan_unlock();
        return;
    }
    out[0] = '\0';
    scan_unlock();
}

bool audio_player_browse_entry_is_dir(int index)
{
    scan_lock();
    bool has_parent = browse_has_parent();
    int offset = has_parent ? 1 : 0;
    bool result;
    if (has_parent && index == 0) {
        result = true; // ".."
    } else {
        int i = index - offset;
        result = (i >= 0 && i < s_browse_scan.subdir_count);
    }
    scan_unlock();
    return result;
}

void audio_player_select_entry(int index)
{
    scan_lock();

    bool has_parent = browse_has_parent();
    int offset = has_parent ? 1 : 0;

    if (has_parent && index == 0) {
        // Volta pra pasta pai: remove o ultimo componente do caminho.
        char *last_slash = strrchr(s_browse_dir, '/');
        if (last_slash && last_slash != s_browse_dir && (size_t)(last_slash - s_browse_dir) >= strlen(s_root_dir)) {
            *last_slash = '\0';
        } else {
            strncpy(s_browse_dir, s_root_dir, sizeof(s_browse_dir) - 1);
            s_browse_dir[sizeof(s_browse_dir) - 1] = '\0';
        }
        scan_dir(s_browse_dir, s_browse_scan);
        scan_unlock();
        return;
    }

    int i = index - offset;
    if (i >= 0 && i < s_browse_scan.subdir_count) {
        // Entra na subpasta.
        char new_dir[PATH_LEN];
        snprintf(new_dir, sizeof(new_dir), "%s/%s", s_browse_dir, s_browse_scan.subdirs[i]);
        strncpy(s_browse_dir, new_dir, sizeof(s_browse_dir) - 1);
        s_browse_dir[sizeof(s_browse_dir) - 1] = '\0';
        scan_dir(s_browse_dir, s_browse_scan);
        scan_unlock();
        return;
    }

    i -= s_browse_scan.subdir_count;
    if (i >= 0 && i < s_browse_scan.audio_count) {
        // Selecionou um arquivo de audio: a playlist ATIVA passa a ser
        // esta pasta.
        char target_name[260];
        strncpy(target_name, s_browse_scan.audio_files[i], sizeof(target_name) - 1);
        target_name[sizeof(target_name) - 1] = '\0';

        strncpy(s_playback_dir, s_browse_dir, sizeof(s_playback_dir) - 1);
        s_playback_dir[sizeof(s_playback_dir) - 1] = '\0';
        scan_dir(s_playback_dir, s_playback_scan);

        int play_idx = 0;
        for (int k = 0; k < s_playback_scan.audio_count; k++) {
            if (strcmp(s_playback_scan.audio_files[k], target_name) == 0) { play_idx = k; break; }
        }

        s_pending_index = play_idx;
        s_pending_cmd = PLAYER_CMD_PLAY_INDEX;
    }
    scan_unlock();
}

// --- Hooks pro modo de armazenamento USB ---------------------------------
void audio_player_release_sd_for_usb(void)
{
    s_usb_takeover_requested = true;
    audio_dsp_flush();
    // Espera o player_task realmente soltar os arquivos (com um teto de
    // seguranca de 5s, pra' nao travar pra sempre se algo der errado).
    for (int i = 0; i < 50 && !s_usb_takeover_active; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void audio_player_reacquire_sd_after_usb(void)
{
    // O conteudo do cartao pode ter mudado enquanto estava exposto via
    // USB (musicas adicionadas/removidas) - rescaneia antes de retomar.
    scan_lock();
    scan_dir(s_browse_dir, s_browse_scan);
    scan_dir(s_playback_dir, s_playback_scan);
    if (s_current_index >= s_playback_scan.audio_count) s_current_index = 0;
    scan_unlock();
    s_usb_takeover_requested = false; // libera o player_task pra continuar
}

void audio_player_suspend(void)
{
    if (s_player_task_handle != NULL) {
        ESP_LOGI("audio_player", "Suspendendo player_task para modo exclusivo USB");
        vTaskSuspend(s_player_task_handle);
    }
    if (s_audio_dsp_task_handle != NULL) {
        ESP_LOGI("audio_player", "Suspendendo audio_dsp_task para modo exclusivo USB");
        vTaskSuspend(s_audio_dsp_task_handle);
    }
}

void audio_player_resume(void)
{
    if (s_audio_dsp_task_handle != NULL) {
        ESP_LOGI("audio_player", "Retomando audio_dsp_task apos modo exclusivo USB");
        vTaskResume(s_audio_dsp_task_handle);
    }
    if (s_player_task_handle != NULL) {
        ESP_LOGI("audio_player", "Retomando player_task apos modo exclusivo USB");
        vTaskResume(s_player_task_handle);
    }
}

// --- Persistencia da ultima faixa/posicao tocada (NVS) -------------------
// Guardamos o caminho RELATIVO a' raiz (ex: "Rock/Album1/faixa.mp3", ou so'
// "faixa.mp3" se estiver na raiz) - assim, com pastas, ainda achamos a
// musica certa mesmo que o conteudo do cartao mude entre uma ligada e
// outra. A posicao (elapsed_sec) e' salva periodicamente durante a
// reproducao e ao pausar.
static bool s_has_resume = false;

static void build_relative_path(const char *abs_dir, const char *filename, char *out, size_t out_len)
{
    size_t root_len = strlen(s_root_dir);
    const char *rel_dir = abs_dir + root_len;
    if (rel_dir[0] == '/') rel_dir++;
    if (rel_dir[0] == '\0') {
        snprintf(out, out_len, "%s", filename);
    } else {
        snprintf(out, out_len, "%s/%s", rel_dir, filename);
    }
}

static void split_relative_path(const char *relative, char *dir_out, size_t dir_out_len,
                                 char *file_out, size_t file_out_len)
{
    const char *last_slash = strrchr(relative, '/');
    if (!last_slash) {
        dir_out[0] = '\0';
        snprintf(file_out, file_out_len, "%s", relative);
    } else {
        size_t dir_len = (size_t)(last_slash - relative);
        if (dir_len >= dir_out_len) dir_len = dir_out_len - 1;
        memcpy(dir_out, relative, dir_len);
        dir_out[dir_len] = '\0';
        snprintf(file_out, file_out_len, "%s", last_slash + 1);
    }
}

static void save_resume_state(const char *relative_path, uint32_t elapsed_sec)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY_LAST_PATH, relative_path);
    nvs_set_u32(h, NVS_KEY_LAST_ELAPSED, elapsed_sec);
    nvs_commit(h);
    nvs_close(h);
}

static void save_volume(int volume)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, NVS_KEY_VOLUME, volume);
    nvs_commit(h);
    nvs_close(h);
}

static void load_volume(void)
{
    int32_t volume = 100;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, NVS_KEY_VOLUME, &volume);
        nvs_close(h);
    }
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    s_volume_percent = volume;
}

static void save_balance(int balance)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, NVS_KEY_BALANCE, balance);
    nvs_commit(h);
    nvs_close(h);
}

static void load_balance(void)
{
    int32_t balance = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, NVS_KEY_BALANCE, &balance);
        nvs_close(h);
    }
    if (balance < -100) balance = -100;
    if (balance > 100) balance = 100;
    s_balance = balance;
}

static track_sort_mode_t s_track_sort_mode = SORT_MODE_NAME;

static void save_sort_mode(track_sort_mode_t mode)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_SORT_MODE, (uint8_t)mode);
    nvs_commit(h);
    nvs_close(h);
}

static void load_sort_mode(void)
{
    uint8_t mode = (uint8_t)SORT_MODE_NAME;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_SORT_MODE, &mode);
        nvs_close(h);
    }
    if (mode > (uint8_t)SORT_MODE_DATE) mode = (uint8_t)SORT_MODE_NAME;
    s_track_sort_mode = (track_sort_mode_t)mode;
    fs_browser_set_sort_mode(s_track_sort_mode);
}

void audio_player_set_sort_mode(track_sort_mode_t mode)
{
    if (mode > SORT_MODE_DATE) mode = SORT_MODE_NAME;
    s_track_sort_mode = mode;
    fs_browser_set_sort_mode(mode);
    save_sort_mode(mode);

    // Re-ordena imediatamente a lista da pasta aberta na navegacao
    scan_lock();
    scan_dir(s_browse_dir, s_browse_scan);
    scan_unlock();
}

track_sort_mode_t audio_player_get_sort_mode(void)
{
    return s_track_sort_mode;
}

static void volume_persistence_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(250));

        int volume_to_save = 0;
        bool should_save_vol = false;
        portENTER_CRITICAL(&s_volume_nvs_mux);
        if (s_volume_nvs_dirty &&
            (xTaskGetTickCount() - s_volume_last_change_tick) >= pdMS_TO_TICKS(VOLUME_NVS_SAVE_DELAY_MS)) {
            volume_to_save = s_volume_percent;
            s_volume_nvs_dirty = false;
            should_save_vol = true;
        }
        portEXIT_CRITICAL(&s_volume_nvs_mux);

        if (should_save_vol) save_volume(volume_to_save);

        int balance_to_save = 0;
        bool should_save_bal = false;
        portENTER_CRITICAL(&s_balance_nvs_mux);
        if (s_balance_nvs_dirty &&
            (xTaskGetTickCount() - s_balance_last_change_tick) >= pdMS_TO_TICKS(VOLUME_NVS_SAVE_DELAY_MS)) {
            balance_to_save = s_balance;
            s_balance_nvs_dirty = false;
            should_save_bal = true;
        }
        portEXIT_CRITICAL(&s_balance_nvs_mux);

        if (should_save_bal) save_balance(balance_to_save);
    }
}

// Buffers de trabalho de load_last_track_index() - deliberadamente NAO
// sao locais de pilha. Essa funcao roda de forma sincrona dentro de
// audio_player_start(), chamada direto por app_main() - ou seja, na
// propria task "main" do FreeRTOS, que so' tem alguns KB de pilha (e
// aumentar esse numero no sdkconfig se mostrou fragil/dificil de garantir
// que realmente foi aplicado, dependendo do sistema de build). Um bloco
// so' de ~2.8KB (rel_path+rel_dir+filename+abs_dir) alocado na pilha
// aqui, somado a' profundidade de chamada ja' grande de app_main ->
// audio_player_start -> aqui -> nvs_get_str/scan_dir (mais o custo por
// chamada do ABI de janela de registradores do Xtensa), estourava a
// pilha - "stack overflow in task main" confirmado no log. Alocando no
// heap, o tamanho da pilha da task main deixa de importar pra essa
// funcao.
struct LoadResumeScratch {
    char rel_path[PATH_LEN];
    char rel_dir[DIR_LEN];
    char filename[260];
    char abs_dir[PATH_LEN];
};

// Le' a ultima faixa/posicao salva, navega ate' a pasta dela (atualizando
// tanto a navegacao quanto a playback) e retorna o indice dela na pasta.
// Retorna 0 (raiz, primeira faixa) se nao houver registro valido.
static int load_last_track_index(uint32_t *out_resume_elapsed)
{
    *out_resume_elapsed = 0;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return 0;

    LoadResumeScratch *s = (LoadResumeScratch *)malloc(sizeof(LoadResumeScratch));
    if (!s) {
        ESP_LOGE(TAG, "Sem memoria para retomar a ultima faixa - comecando da raiz");
        nvs_close(h);
        return 0;
    }

    size_t len = sizeof(s->rel_path);
    esp_err_t err = nvs_get_str(h, NVS_KEY_LAST_PATH, s->rel_path, &len);
    if (err != ESP_OK) {
        nvs_close(h);
        free(s);
        return 0;
    }

    uint32_t elapsed = 0;
    nvs_get_u32(h, NVS_KEY_LAST_ELAPSED, &elapsed); // ok falhar (registro antigo sem essa chave)
    nvs_close(h);

    split_relative_path(s->rel_path, s->rel_dir, sizeof(s->rel_dir), s->filename, sizeof(s->filename));

    if (s->rel_dir[0] == '\0') {
        snprintf(s->abs_dir, sizeof(s->abs_dir), "%s", s_root_dir);
    } else {
        snprintf(s->abs_dir, sizeof(s->abs_dir), "%s/%s", s_root_dir, s->rel_dir);
    }

    strncpy(s_playback_dir, s->abs_dir, sizeof(s_playback_dir) - 1);
    s_playback_dir[sizeof(s_playback_dir) - 1] = '\0';

    scan_lock();
    scan_dir(s_playback_dir, s_playback_scan);

    // O menu tambem abre direto na pasta da faixa retomada.
    strncpy(s_browse_dir, s->abs_dir, sizeof(s_browse_dir) - 1);
    s_browse_dir[sizeof(s_browse_dir) - 1] = '\0';
    scan_dir(s_browse_dir, s_browse_scan);

    for (int i = 0; i < s_playback_scan.audio_count; i++) {
        if (strcmp(s_playback_scan.audio_files[i], s->filename) == 0) {
            *out_resume_elapsed = elapsed;
            s_has_resume = true;
            scan_unlock();
            free(s);
            return i;
        }
    }
    scan_unlock();
    free(s);
    return 0;
}

bool audio_player_should_start_in_playing_mode(void) { return s_has_resume; }

static int s_boot_resume_index = 0;
static uint32_t s_boot_resume_elapsed = 0;

// Expande amostras mono para estereo (duplica L->R) num buffer auxiliar.
static int32_t *s_stereo_scratch = nullptr;
static size_t s_stereo_scratch_capacity = 0;

int32_t *ensure_stereo_scratch(size_t needed_samples)
{
    if (s_stereo_scratch_capacity < needed_samples) {
        if (s_stereo_scratch) heap_caps_free(s_stereo_scratch);
        s_stereo_scratch = (int32_t *)heap_caps_malloc(needed_samples * sizeof(int32_t),
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_stereo_scratch) {
            s_stereo_scratch = (int32_t *)heap_caps_malloc(needed_samples * sizeof(int32_t),
                                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        s_stereo_scratch_capacity = s_stereo_scratch ? needed_samples : 0;
    }
    return s_stereo_scratch;
}

static std::unique_ptr<mps3::AudioDecoderBase> create_decoder(mps3::AudioFormat fmt, uint64_t /*file_size_bytes*/)
{
    esp_audio_simple_dec_type_t type;
    const char *label;
    if (!mps3::audio_format_to_simple_dec(fmt, &type, &label)) return nullptr;
    return std::make_unique<mps3::EspCodecDecoderAdapter>(type, label);
}

// --- Pular metadados no inicio do arquivo -------------------------------
// ID3v2 (MP3, e as vezes .aac) e os blocos de metadados nativos do FLAC
// (STREAMINFO, VORBIS_COMMENT, PICTURE - capa de album embutida) podem
// ser bem maiores que o buffer de entrada (INBUF_SIZE) - uma capa de
// album em alta resolucao facilmente passa de varios MB. Se a gente
// simplesmente jogar os primeiros INBUF_SIZE bytes do arquivo pro
// decoder, ele pode nunca achar o primeiro frame de audio dentro desse
// buffer (nem aumentando o limite de busca do parser resolve, porque o
// limite so' vale pra dentro dos bytes que a gente ja' entregou). Como
// so' avancamos o ponteiro do arquivo (fseek), o custo e' O(1)
// independente do tamanho da capa - nao precisamos de um buffer gigante
// pra "torcer" pra caber tudo de uma vez. Deixa o ponteiro exatamente no
// primeiro byte de audio de verdade (ou no inicio do arquivo, se nao
// reconhecer nada pra pular).
// Pula ID3v2 (se houver) e, se for FLAC, anda pelos blocos de metadados
// nativos (VORBIS_COMMENT, PICTURE - capa de album, PADDING, etc.),
// DESCARTANDO-os - exceto o STREAMINFO, que e' CAPTURADO em out_prefix
// (sempre exatamente 42 bytes: "fLaC" + cabecalho do bloco + corpo de 34
// bytes) em vez de pulado. O ESP_ES_PARSER precisa ver "fLaC"+STREAMINFO
// pra se configurar (sample rate/canais/etc.) antes de reconhecer
// qualquer frame - pular esse bloco tambem (como uma versao anterior
// deste fix fazia) faz TODO FLAC falhar com "Not supported", nao so' os
// de capa grande. Devolve 42 se capturou (chamador deve colocar esses
// bytes no INICIO do buffer de entrada, antes dos frames de audio de
// verdade), ou 0 se nao for FLAC / nao achou STREAMINFO valido (nesse
// caso nada foi capturado, so' o ID3v2 foi pulado se havia).
static size_t skip_leading_metadata(FILE *fp, mps3::AudioFormat fmt, uint8_t *out_prefix /* >= 42 bytes */)
{
    uint8_t hdr[10];
    if (fread(hdr, 1, 10, fp) == 10 && hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3') {
        // ID3v2: tamanho da tag em inteiro "syncsafe" de 28 bits (7 bits
        // uteis por byte, bit alto sempre 0).
        uint32_t tag_size = ((uint32_t)(hdr[6] & 0x7F) << 21) |
                             ((uint32_t)(hdr[7] & 0x7F) << 14) |
                             ((uint32_t)(hdr[8] & 0x7F) << 7)  |
                             ((uint32_t)(hdr[9] & 0x7F));
        bool has_footer = (hdr[5] & 0x10) != 0; // flag "footer present" (+10 bytes)
        fseek(fp, tag_size + (has_footer ? 10 : 0), SEEK_CUR);
    } else {
        fseek(fp, 0, SEEK_SET);
    }

    if (fmt != mps3::AudioFormat::Flac) return 0;

    // FLAC nativo (pode vir logo de cara, ou logo apos um ID3v2 raro em
    // arquivo hibrido).
    long pos_before = ftell(fp);
    char magic[4];
    if (fread(magic, 1, 4, fp) != 4 || memcmp(magic, "fLaC", 4) != 0) {
        fseek(fp, pos_before, SEEK_SET); // nao achou "fLaC" aqui - desfaz e deixa como estava
        return 0;
    }

    bool got_streaminfo = false;
    for (;;) {
        uint8_t bhdr[4];
        if (fread(bhdr, 1, 4, fp) != 4) break; // EOF inesperado - desiste, sem prefixo
        bool is_last = (bhdr[0] & 0x80) != 0;
        uint8_t block_type = bhdr[0] & 0x7F;
        uint32_t block_len = ((uint32_t)bhdr[1] << 16) | ((uint32_t)bhdr[2] << 8) | bhdr[3];

        if (block_type == 0 && block_len == 34 && !got_streaminfo) {
            // STREAMINFO (sempre exatamente 34 bytes de corpo, por spec)
            // - captura em vez de pular.
            uint8_t body[34];
            if (fread(body, 1, 34, fp) == 34) {
                memcpy(out_prefix, "fLaC", 4);
                out_prefix[4] = 0x80; // tipo 0 (STREAMINFO), bit "ultimo bloco" forcado em 1 -
                                       // depois dele so' vem audio de verdade, entao E' o ultimo
                                       // bloco de metadados que o decoder vai ver.
                out_prefix[5] = 0; out_prefix[6] = 0; out_prefix[7] = 34; // tamanho = 34
                memcpy(out_prefix + 8, body, 34);
                got_streaminfo = true;
            }
        } else {
            fseek(fp, block_len, SEEK_CUR); // qualquer outro bloco - descarta
        }

        if (is_last) break;
    }

    return got_streaminfo ? 42 : 0; // ponteiro do arquivo ja' esta' no 1o frame de audio
}

// --- Reordenacao "virtual" de M4A com mdat antes de moov -----------------
// O parser M4A do esp_audio_codec so' entende o arquivo se o bloco moov
// (metadados/tabelas de amostra) vier ANTES do mdat (audio de verdade) -
// e' assim que a maioria dos .m4a e' gravado, mas arquivos extraidos de
// video (savefromnet, yt-dlp, etc., sem "fast start") costumam vir na
// ordem contraria. Quando isso acontece, o parser tenta interpretar
// bytes de audio cru como se fossem estrutura e falha (ex.: "Fail to
// allocate memory for stsc").
//
// Como nosso player NUNCA faz acesso aleatorio de verdade ao arquivo (so'
// decodifica sequencialmente, ate' pra "seek" - ver audio_player_seek_*)
// a gente pode ENTREGAR pro decoder o moov ANTES do mdat mesmo que no
// arquivo fisico esteja na ordem contraria - uma reordenacao "virtual",
// sem reescrever nada no cartao SD. So' ler o moov de onde ele estiver,
// guardar numa memoria a parte, e depois ir lendo o mdat normalmente de
// onde ele estiver.

struct M4aBoxInfo {
    uint64_t ftyp_start = 0, ftyp_size = 0; // ftyp_size == 0 se nao achou
    uint64_t moov_start = 0, moov_size = 0;
    uint64_t mdat_start = 0;                // posicao do cabecalho do mdat, nao so' do payload
    bool found_moov = false;
    bool found_mdat = false;
};

// Anda pelas caixas de NIVEL SUPERIOR do MP4 (ftyp/moov/mdat/free/...) -
// nao precisa entender o que tem DENTRO de moov, so' onde cada caixa
// comeca/termina. Devolve false se nao achou "moov" (arquivo invalido/
// atipico - desiste, deixa pro parser da Espressif tentar do jeito dele).
static bool scan_m4a_top_level_boxes(FILE *fp, uint64_t file_size, M4aBoxInfo *out)
{
    *out = M4aBoxInfo{};
    uint64_t pos = 0;

    while (pos + 8 <= file_size) {
        if (fseek(fp, (long)pos, SEEK_SET) != 0) break;
        uint8_t hdr[8];
        if (fread(hdr, 1, 8, fp) != 8) break;

        uint64_t box_size = ((uint64_t)hdr[0] << 24) | ((uint64_t)hdr[1] << 16) |
                             ((uint64_t)hdr[2] << 8)  | (uint64_t)hdr[3];
        char type[5] = { (char)hdr[4], (char)hdr[5], (char)hdr[6], (char)hdr[7], 0 };
        uint64_t header_len = 8;

        if (box_size == 1) {
            // "largesize": tamanho de 64 bits nos 8 bytes seguintes.
            uint8_t ext[8];
            if (fread(ext, 1, 8, fp) != 8) break;
            box_size = 0;
            for (int i = 0; i < 8; i++) box_size = (box_size << 8) | ext[i];
            header_len = 16;
        } else if (box_size == 0) {
            box_size = file_size - pos; // caixa vai ate' o fim do arquivo
        }

        if (box_size < header_len || pos + box_size > file_size) break; // tamanho invalido - desiste

        if (strcmp(type, "ftyp") == 0 && out->ftyp_size == 0) {
            out->ftyp_start = pos;
            out->ftyp_size = box_size;
        } else if (strcmp(type, "moov") == 0 && !out->found_moov) {
            out->moov_start = pos;
            out->moov_size = box_size;
            out->found_moov = true;
        } else if (strcmp(type, "mdat") == 0 && !out->found_mdat) {
            out->mdat_start = pos;
            out->found_mdat = true;
        }

        pos += box_size;
    }

    return out->found_moov;
}

// Acha a PRIMEIRA caixa filha direta de tipo `want_type` dentro do
// intervalo [start,end) - nao desce em sub-caixas (nivel unico). Usado
// pra navegar a arvore de boxes do MP4 (moov>trak>mdia>..., moof>traf>...)
// um nivel de cada vez.
static bool find_child_box(FILE *fp, uint64_t start, uint64_t end, const char want_type[4],
                            uint64_t *out_start, uint64_t *out_header_len, uint64_t *out_size)
{
    uint64_t pos = start;
    while (pos + 8 <= end) {
        if (fseek(fp, (long)pos, SEEK_SET) != 0) return false;
        uint8_t hdr[8];
        if (fread(hdr, 1, 8, fp) != 8) return false;
        uint64_t box_size = ((uint64_t)hdr[0] << 24) | ((uint64_t)hdr[1] << 16) | ((uint64_t)hdr[2] << 8) | hdr[3];
        uint64_t header_len = 8;
        if (box_size == 1) {
            uint8_t ext[8];
            if (fread(ext, 1, 8, fp) != 8) return false;
            box_size = 0;
            for (int i = 0; i < 8; i++) box_size = (box_size << 8) | ext[i];
            header_len = 16;
        } else if (box_size == 0) {
            box_size = end - pos;
        }
        if (box_size < header_len || pos + box_size > end) return false;
        if (memcmp(hdr + 4, want_type, 4) == 0) {
            *out_start = pos;
            *out_header_len = header_len;
            *out_size = box_size;
            return true;
        }
        pos += box_size;
    }
    return false;
}

// true se o moov tiver uma caixa "mvex" - marca de MP4 FRAGMENTADO
// (moof+mdat repetidos em vez de um mdat classico so'; a tabela de
// amostras em moov>stbl fica vazia/incompleta, os offsets/tamanhos de
// verdade ficam espalhados em cada moof>traf>trun). Tipico de audio
// baixado direto de segmentos DASH (ex: YouTube) sem remux completo -
// ver M4aFragDemuxer mais abaixo, que existe especificamente pra' tocar
// esses arquivos sem depender do parser MP4 "classico" do Simple Decoder
// (que nao entende fragmentacao).
static bool m4a_is_fragmented(FILE *fp, uint64_t moov_start, uint64_t moov_size)
{
    uint64_t s, h, sz;
    return find_child_box(fp, moov_start + 8, moov_start + moov_size, "mvex", &s, &h, &sz);
}

// Desce moov > trak > mdia > minf > stbl > stsd > (1a entrada) > esds, e
// extrai do ES Descriptor o AudioSpecificConfig (audioObjectType,
// samplingFrequencyIndex, channelConfiguration) - precisamos disso pra
// montar cabecalhos ADTS sinteticos (ver M4aFragDemuxer). So' suporta
// a 1a sample entry sendo "mp4a" (AAC) - outros codecs dentro de M4A
// (ex: ALAC) nao tem como virar ADTS, devolve false nesse caso e quem
// chamou cai pro decoder M4A comum (que provavelmente tambem vai falhar
// num arquivo fragmentado, mas pelo menos nao arrisca produzir ruido).
static bool m4a_parse_audio_config(FILE *fp, uint64_t moov_start, uint64_t moov_size,
                                    uint8_t *out_profile, uint8_t *out_sr_idx, uint8_t *out_channels)
{
    uint64_t s, h, sz;
    if (!find_child_box(fp, moov_start + 8, moov_start + moov_size, "trak", &s, &h, &sz)) return false;
    uint64_t trak_s = s, trak_sz = sz;
    if (!find_child_box(fp, trak_s + 8, trak_s + trak_sz, "mdia", &s, &h, &sz)) return false;
    uint64_t mdia_s = s, mdia_h = h, mdia_sz = sz;
    if (!find_child_box(fp, mdia_s + mdia_h, mdia_s + mdia_sz, "minf", &s, &h, &sz)) return false;
    uint64_t minf_s = s, minf_h = h, minf_sz = sz;
    if (!find_child_box(fp, minf_s + minf_h, minf_s + minf_sz, "stbl", &s, &h, &sz)) return false;
    uint64_t stbl_s = s, stbl_h = h, stbl_sz = sz;
    if (!find_child_box(fp, stbl_s + stbl_h, stbl_s + stbl_sz, "stsd", &s, &h, &sz)) return false;
    uint64_t stsd_s = s, stsd_h = h, stsd_sz = sz;

    // stsd: 4 bytes version/flags + 4 bytes entry_count, DEPOIS vem a 1a
    // sample entry (ex: caixa "mp4a" - mesmo layout de uma caixa normal:
    // 4 size + 4 fourcc + corpo especifico do codec).
    uint64_t entry_pos = stsd_s + stsd_h + 8;
    if (entry_pos + 8 > stsd_s + stsd_sz) return false;
    uint8_t entry_hdr[8];
    if (fseek(fp, (long)entry_pos, SEEK_SET) != 0) return false;
    if (fread(entry_hdr, 1, 8, fp) != 8) return false;
    uint64_t entry_size = ((uint64_t)entry_hdr[0] << 24) | ((uint64_t)entry_hdr[1] << 16) |
                           ((uint64_t)entry_hdr[2] << 8) | entry_hdr[3];
    if (memcmp(entry_hdr + 4, "mp4a", 4) != 0) return false; // so' suportamos AAC aqui
    if (entry_size < 8 || entry_pos + entry_size > stsd_s + stsd_sz) return false;

    // Dentro da entrada "mp4a" a AudioSampleEntry ocupa 28 bytes fixos
    // antes de "esds" (reservado+data_ref_index+reservado+canais+bps+
    // reservado+sample_rate) - procuramos em vez de assumir offset fixo
    // exato, mais robusto a variacoes/versoes.
    uint64_t esds_search_start = entry_pos + 8 + 28;
    if (esds_search_start > entry_pos + entry_size) esds_search_start = entry_pos + 8;
    if (!find_child_box(fp, esds_search_start, entry_pos + entry_size, "esds", &s, &h, &sz)) return false;

    if (sz < h + 4 + 5) return false; // pequeno demais pra ter um ES_Descriptor valido
    uint8_t body[64];
    size_t body_len = sz - h - 4; // pula version/flags(4) do proprio esds
    if (body_len > sizeof(body)) body_len = sizeof(body);
    if (fseek(fp, (long)(s + h + 4), SEEK_SET) != 0) return false;
    if (fread(body, 1, body_len, fp) != body_len) return false;

    // Descriptors MPEG-4 "expandable length" (ISO/IEC 14496-1): tag(1) +
    // length(1-4 bytes, bit7=continuacao) + payload. Precisamos descer
    // ES_Descr(0x03) > DecoderConfigDescr(0x04) > DecoderSpecificInfo(0x05).
    size_t p = 0;
    auto read_len = [&](size_t &pos) -> int {
        int val = 0;
        for (int i = 0; i < 4 && pos < body_len; i++) {
            uint8_t b = body[pos++];
            val = (val << 7) | (b & 0x7f);
            if (!(b & 0x80)) break;
        }
        return val;
    };
    if (p >= body_len || body[p++] != 0x03) return false; // ES_DescrTag
    read_len(p);
    p += 3; // ES_ID(2) + flags(1)
    if (p >= body_len || body[p++] != 0x04) return false; // DecoderConfigDescrTag
    read_len(p);
    if (p >= body_len) return false;
    uint8_t object_type_indication = body[p]; p += 1;
    if (object_type_indication != 0x40 && object_type_indication != 0x67) {
        // 0x40 = MPEG-4 AAC, 0x67 = MPEG-2 AAC LC - os unicos que sabemos
        // embrulhar em ADTS do jeito feito aqui.
        return false;
    }
    p += 1 + 3 + 4 + 4; // streamType/flags + bufferSizeDB + maxBitrate + avgBitrate
    if (p >= body_len || body[p++] != 0x05) return false; // DecoderSpecificInfoTag
    int dsi_len = read_len(p);
    if (dsi_len < 2 || p + 2 > body_len) return false;

    uint16_t v = ((uint16_t)body[p] << 8) | body[p + 1];
    uint8_t audio_object_type = (v >> 11) & 0x1f;
    uint8_t sr_idx = (v >> 7) & 0x0f;
    uint8_t channels = (v >> 3) & 0x0f;
    if (audio_object_type < 1 || audio_object_type > 4 || sr_idx >= 13 || channels == 0 || channels > 7) {
        return false; // valores fora do esperado - nao arrisca
    }

    *out_profile = audio_object_type - 1; // ADTS profile = audioObjectType-1
    *out_sr_idx = sr_idx;
    *out_channels = channels;
    return true;
}

static void build_adts_header(uint8_t out[7], uint8_t profile, uint8_t sr_idx, uint8_t channels, uint32_t frame_len_with_header)
{
    // ADTS de 7 bytes, sem CRC (protection_absent=1). Layout validado
    // por round-trip contra um parser de referencia antes de integrar.
    out[0] = 0xFF;
    out[1] = 0xF1;
    out[2] = (uint8_t)(((profile & 0x3) << 6) | ((sr_idx & 0xF) << 2) | ((channels >> 2) & 0x1));
    out[3] = (uint8_t)(((channels & 0x3) << 6) | ((frame_len_with_header >> 11) & 0x3));
    out[4] = (uint8_t)((frame_len_with_header >> 3) & 0xFF);
    out[5] = (uint8_t)(((frame_len_with_header & 0x7) << 5) | 0x1F);
    out[6] = 0xFC;
}

// --- Demuxer de MP4 fragmentado (AAC) -------------------------------------
// Bypassa o parser M4A "container-aware" do ESP Audio Simple Decoder pra'
// arquivos MP4 fragmentados - na pratica esse tipo de parser embarcado so'
// entende a tabela de amostras classica (moov>stbl>stsz/stco), que em
// arquivos fragmentados fica vazia. Em vez disso, extraimos o
// AudioSpecificConfig do esds (uma vez, no inicio) e usamos ele pra'
// sintetizar um cabecalho ADTS na frente de cada frame AAC bruto extraido
// dos blocos mdat (usando os tamanhos exatos de moof>traf>trun) -
// entregando esse fluxo ADTS pro decoder "AAC" comum (ja' sabemos que
// funciona), em vez do decoder "M4A".
//
// Validado offline contra um arquivo real (M4A/AAC baixado do YouTube,
// 20 fragmentos): soma dos tamanhos do trun bate exatamente com o
// payload de cada mdat correspondente, sem nenhuma incompatibilidade.
struct M4aFragDemuxer {
    uint8_t adts_profile = 1;
    uint8_t adts_sr_idx = 4;
    uint8_t adts_channels = 2;

    uint64_t file_size = 0;
    uint64_t scan_pos = 0; // proxima posicao a examinar em busca do proximo moof

    uint32_t *sample_sizes = nullptr; // malloc'd - tamanhos das amostras do FRAGMENTO ATUAL
    uint32_t sample_count = 0;
    uint32_t sample_idx = 0;

    uint32_t frame_remaining = 0; // bytes do FRAME (corpo) da amostra atual ainda nao lidos do arquivo
    uint8_t adts_header[7];
    uint8_t header_remaining = 0; // bytes do cabecalho ADTS sintetico da amostra atual ainda nao entregues
    bool eof = false;             // nao ha' mais fragmentos - fim logico do audio
};

static void m4a_frag_demuxer_free(M4aFragDemuxer *d)
{
    if (d->sample_sizes) { free(d->sample_sizes); d->sample_sizes = nullptr; }
}

// Acha o proximo par moof+mdat a partir de d->scan_pos, parseia o trun
// (com fallback pro default-sample-size do tfhd quando o trun nao traz
// tamanho explicito por amostra) e deixa `d` pronto pra' comecar a
// emitir as amostras desse fragmento. Marca d->eof=true se nao achar
// mais nenhum moof valido antes do fim do arquivo. Fragmentos que
// falham na checagem de consistencia (soma dos tamanhos != payload do
// mdat correspondente) sao pulados em vez de arriscar ler fora dos
// limites ou produzir audio corrompido.
static void m4a_frag_advance_fragment(FILE *fp, M4aFragDemuxer *d)
{
    m4a_frag_demuxer_free(d);
    d->sample_count = 0;
    d->sample_idx = 0;

    for (;;) {
        uint64_t moof_s, moof_h, moof_sz;
        if (!find_child_box(fp, d->scan_pos, d->file_size, "moof", &moof_s, &moof_h, &moof_sz)) {
            d->eof = true;
            return;
        }
        uint64_t after_moof = moof_s + moof_sz;

        uint64_t traf_s, traf_h, traf_sz;
        if (!find_child_box(fp, moof_s + moof_h, after_moof, "traf", &traf_s, &traf_h, &traf_sz)) {
            d->scan_pos = after_moof;
            continue;
        }

        uint32_t default_sample_size = 0;
        uint64_t tfhd_s, tfhd_h, tfhd_sz;
        if (find_child_box(fp, traf_s + traf_h, traf_s + traf_sz, "tfhd", &tfhd_s, &tfhd_h, &tfhd_sz)) {
            uint8_t hdr[4];
            if (fseek(fp, (long)(tfhd_s + tfhd_h), SEEK_SET) == 0 && fread(hdr, 1, 4, fp) == 4) {
                uint32_t tfhd_flags = ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | hdr[3];
                uint64_t p = tfhd_s + tfhd_h + 4 + 4; // pula version/flags(4) + track_ID(4)
                if (tfhd_flags & 0x000001) p += 8; // base-data-offset-present
                if (tfhd_flags & 0x000002) p += 4; // sample-description-index-present
                if (tfhd_flags & 0x000008) p += 4; // default-sample-duration-present
                if (tfhd_flags & 0x000010) {       // default-sample-size-present
                    uint8_t sz4[4];
                    if (fseek(fp, (long)p, SEEK_SET) == 0 && fread(sz4, 1, 4, fp) == 4) {
                        default_sample_size = ((uint32_t)sz4[0] << 24) | ((uint32_t)sz4[1] << 16) |
                                               ((uint32_t)sz4[2] << 8) | sz4[3];
                    }
                }
            }
        }

        uint64_t trun_s, trun_h, trun_sz;
        if (!find_child_box(fp, traf_s + traf_h, traf_s + traf_sz, "trun", &trun_s, &trun_h, &trun_sz)) {
            d->scan_pos = after_moof;
            continue;
        }

        uint8_t trun_hdr[8];
        if (fseek(fp, (long)(trun_s + trun_h), SEEK_SET) != 0 || fread(trun_hdr, 1, 8, fp) != 8) {
            d->scan_pos = after_moof;
            continue;
        }
        uint32_t trun_flags = ((uint32_t)trun_hdr[1] << 16) | ((uint32_t)trun_hdr[2] << 8) | trun_hdr[3];
        uint32_t sample_count = ((uint32_t)trun_hdr[4] << 24) | ((uint32_t)trun_hdr[5] << 16) |
                                 ((uint32_t)trun_hdr[6] << 8) | trun_hdr[7];
        if (sample_count == 0 || sample_count > 100000) { // sanidade - fragmento absurdo, desiste desse
            d->scan_pos = after_moof;
            continue;
        }

        uint64_t p = trun_s + trun_h + 8;
        if (trun_flags & 0x000001) p += 4; // data-offset-present
        if (trun_flags & 0x000004) p += 4; // first-sample-flags-present

        uint32_t *sizes = (uint32_t *)malloc(sample_count * sizeof(uint32_t));
        if (!sizes) { d->eof = true; return; } // sem memoria (raro - sample_count e' pequeno) - desiste de vez

        bool ok = true;
        for (uint32_t i = 0; i < sample_count && ok; i++) {
            if (trun_flags & 0x000100) p += 4; // sample-duration-present
            uint32_t sz = default_sample_size;
            if (trun_flags & 0x000200) { // sample-size-present
                uint8_t b4[4];
                if (fseek(fp, (long)p, SEEK_SET) != 0 || fread(b4, 1, 4, fp) != 4) { ok = false; break; }
                sz = ((uint32_t)b4[0] << 24) | ((uint32_t)b4[1] << 16) | ((uint32_t)b4[2] << 8) | b4[3];
                p += 4;
            }
            if (trun_flags & 0x000400) p += 4; // sample-flags-present
            if (trun_flags & 0x000800) p += 4; // sample-composition-time-offsets-present
            sizes[i] = sz;
        }

        uint64_t mdat_s, mdat_h, mdat_sz;
        if (!ok || !find_child_box(fp, after_moof, d->file_size, "mdat", &mdat_s, &mdat_h, &mdat_sz)) {
            free(sizes);
            d->scan_pos = after_moof;
            continue;
        }

        uint64_t sum = 0;
        for (uint32_t i = 0; i < sample_count; i++) sum += sizes[i];
        if (sum != mdat_sz - mdat_h) {
            free(sizes);
            d->scan_pos = after_moof;
            continue;
        }

        d->sample_sizes = sizes;
        d->sample_count = sample_count;
        d->sample_idx = 0;
        d->scan_pos = after_moof;
        fseek(fp, (long)(mdat_s + mdat_h), SEEK_SET); // posiciona no INICIO do payload, pronto pra 1a amostra
        return;
    }
}

static bool m4a_frag_demuxer_init(FILE *fp, uint64_t file_size, uint64_t moov_start, uint64_t moov_size, M4aFragDemuxer *d)
{
    *d = M4aFragDemuxer{};
    d->file_size = file_size;
    if (!m4a_parse_audio_config(fp, moov_start, moov_size, &d->adts_profile, &d->adts_sr_idx, &d->adts_channels)) {
        return false;
    }
    d->scan_pos = moov_start + moov_size;
    m4a_frag_advance_fragment(fp, d);
    return d->sample_count > 0; // achou pelo menos 1 fragmento valido de verdade?
}

// Le' ate' `want` bytes do fluxo ADTS "virtual" (cabecalhos sinteticos +
// frames AAC brutos extraidos do(s) mdat) pra' dentro de `dst`. Devolve
// quantos bytes conseguiu entregar - < want so' quando acabaram os
// fragmentos (fim de verdade da faixa).
static size_t m4a_frag_read(FILE *fp, M4aFragDemuxer *d, uint8_t *dst, size_t want)
{
    size_t total = 0;
    while (total < want) {
        if (d->header_remaining > 0) {
            size_t n = (size_t)d->header_remaining < (want - total) ? d->header_remaining : (want - total);
            memcpy(dst + total, d->adts_header + (7 - d->header_remaining), n);
            d->header_remaining -= (uint8_t)n;
            total += n;
            continue;
        }
        if (d->frame_remaining > 0) {
            size_t chunk = (size_t)d->frame_remaining < (want - total) ? d->frame_remaining : (want - total);
            size_t got = fread(dst + total, 1, chunk, fp);
            total += got;
            d->frame_remaining -= (uint32_t)got;
            if (got < chunk) return total; // erro de leitura inesperado - desiste, entrega o que deu
            continue;
        }
        if (d->sample_idx >= d->sample_count) {
            if (d->eof) return total;
            m4a_frag_advance_fragment(fp, d);
            if (d->eof && d->sample_count == 0) return total;
            continue;
        }
        uint32_t frame_size = d->sample_sizes[d->sample_idx++];
        if ((uint64_t)frame_size + 7 > 8191) { // nao cabe no campo de 13 bits do ADTS - desiste
            d->eof = true;
            return total;
        }
        build_adts_header(d->adts_header, d->adts_profile, d->adts_sr_idx, d->adts_channels, frame_size + 7);
        d->header_remaining = 7;
        d->frame_remaining = frame_size;
    }
    return total;
}

// --- Demuxer de WebM/Matroska (Opus) ---------------------------------------
// Extrai pacotes Opus brutos de um arquivo WebM (EBML/Matroska) - formato
// tipico de audio baixado direto de segmentos DASH do YouTube (ex: itag
// 251, audio-only) e salvo sem remux pra Ogg (extensao ".weba"/".webm").
// Sem isso esses arquivos nao tocam de jeito nenhum: nem o parser MP4
// nem o parser Ogg do Simple Decoder entendem EBML/Matroska - sao
// formatos de container completamente diferentes por baixo.
//
// Diferente do M4aFragDemuxer, aqui NAO sintetizamos cabecalho nenhum -
// pacotes Opus crus sao auto-suficientes (o proprio decoder Opus extrai
// canais/config do byte TOC de cada pacote). Em compensacao, precisamos
// entregar CADA pacote isolado numa chamada de decode() separada -
// diferente de ADTS/MP3/FLAC (que tem palavras de sincronismo e
// aceitam varios frames concatenados numa chamada so'), Opus cru nao
// tem como saber onde um pacote termina e o outro comeca dentro de um
// blob so'. Por isso o loop principal trata esse caso com uma regra
// extra: nunca acrescenta um novo pacote ao buffer antes do anterior
// ter sido totalmente consumido (ver uso de webm_opus_read_one_packet
// em play_track()).
//
// Validado offline (fora do dispositivo) contra os 2 arquivos WebM reais
// enviados pelo usuario: extracao bate exatamente 9612 pacotes nos dois
// (identico entre as versoes de alta e baixa qualidade do mesmo audio,
// como esperado - mesma duracao, taxas de bits diferentes), cobrindo
// tanto SimpleBlock quanto BlockGroup>Block (o ultimo pacote de um dos
// arquivos usa esse 2o formato).
static bool ebml_read_vint(FILE *fp, uint64_t pos, bool strip_marker,
                            uint64_t *out_value, uint64_t *out_new_pos)
{
    uint8_t first;
    if (fseek(fp, (long)pos, SEEK_SET) != 0) return false;
    if (fread(&first, 1, 1, fp) != 1) return false;
    if (first == 0) return false; // vint invalido (byte 0 nunca tem o bit marcador setado)
    int length = 1;
    uint8_t mask = 0x80;
    while (!(first & mask)) {
        length++;
        mask >>= 1;
        if (length > 8) return false; // vint absurdamente longo - dado invalido
    }
    uint64_t value = strip_marker ? (uint64_t)(first & (mask - 1)) : (uint64_t)first;
    if (length > 1) {
        uint8_t rest[7];
        if (fread(rest, 1, (size_t)(length - 1), fp) != (size_t)(length - 1)) return false;
        for (int i = 0; i < length - 1; i++) value = (value << 8) | rest[i];
    }
    *out_value = value;
    *out_new_pos = pos + (uint64_t)length;
    return true;
}

struct EbmlElement {
    uint64_t id = 0;
    uint64_t payload_start = 0;
    uint64_t payload_end = 0;
    bool unknown_size = false; // "tamanho desconhecido" (streaming) - vai ate' o fim da regiao pai
};

// Le' o cabecalho (ID + tamanho) do elemento EBML comecando em `pos`,
// que deve estar dentro de [pos, region_end). Devolve false em
// erro/dado invalido/fora dos limites.
static bool ebml_read_element(FILE *fp, uint64_t pos, uint64_t region_end, EbmlElement *out)
{
    uint64_t id, pos2;
    if (!ebml_read_vint(fp, pos, false, &id, &pos2)) return false;
    uint64_t size_start = pos2;
    uint64_t size, pos3;
    if (!ebml_read_vint(fp, pos2, true, &size, &pos3)) return false;
    uint64_t vint_len = pos3 - size_start;
    bool unknown = (vint_len >= 8) ? (size == UINT64_MAX) : (size == ((1ULL << (7 * vint_len)) - 1));
    out->id = id;
    out->payload_start = pos3;
    out->payload_end = unknown ? region_end : (pos3 + size);
    out->unknown_size = unknown;
    if (!unknown && out->payload_end > region_end) return false;
    return true;
}

// Acha a PRIMEIRA caixa filha direta de ID `want_id` dentro de
// [start,end) - nao desce em sub-elementos (nivel unico).
static bool ebml_find_child(FILE *fp, uint64_t start, uint64_t end, uint64_t want_id, EbmlElement *out)
{
    uint64_t pos = start;
    while (pos < end) {
        EbmlElement el;
        if (!ebml_read_element(fp, pos, end, &el)) return false;
        if (el.id == want_id) { *out = el; return true; }
        pos = el.unknown_size ? end : el.payload_end;
    }
    return false;
}

struct WebmOpusDemuxer {
    uint64_t file_size = 0;
    uint64_t track_number = 0; // numero (Matroska) da trilha de audio Opus identificada

    uint64_t segment_end = 0;
    uint64_t cluster_scan_pos = 0; // proxima posicao (nivel do Segment) onde procurar o proximo Cluster
    uint64_t block_pos = 0;        // proxima posicao a examinar DENTRO do cluster atual
    uint64_t block_end = 0;        // fim do cluster atual

    bool eof = false;
};

// Acha Segment>Tracks>TrackEntry com TrackType=2 (audio) e
// CodecID="A_OPUS" - preenche `d->track_number` e os limites do Segment
// (onde a varredura de Clusters vai comecar). Devolve false se nao
// achar nenhuma trilha Opus (ex: e' Vorbis, ou video puro) - nesse caso
// o arquivo nao toca via esse caminho, mas tambem nao quebra nada (quem
// chamou cai pro comportamento anterior a esse fix).
static bool webm_find_opus_track(FILE *fp, uint64_t file_size, WebmOpusDemuxer *d)
{
    const uint64_t ID_SEGMENT = 0x18538067;
    const uint64_t ID_TRACKS = 0x1654AE6B;
    const uint64_t ID_TRACKENTRY = 0xAE;
    const uint64_t ID_TRACKNUMBER = 0xD7;
    const uint64_t ID_TRACKTYPE = 0x83;
    const uint64_t ID_CODECID = 0x86;

    EbmlElement seg;
    if (!ebml_find_child(fp, 0, file_size, ID_SEGMENT, &seg)) return false;
    d->segment_end = seg.payload_end;
    d->cluster_scan_pos = seg.payload_start;

    EbmlElement tracks;
    if (!ebml_find_child(fp, seg.payload_start, seg.payload_end, ID_TRACKS, &tracks)) return false;

    uint64_t pos = tracks.payload_start;
    while (pos < tracks.payload_end) {
        EbmlElement entry;
        if (!ebml_read_element(fp, pos, tracks.payload_end, &entry)) return false;
        if (entry.id == ID_TRACKENTRY) {
            EbmlElement type_el, codec_el, num_el;
            bool has_type = ebml_find_child(fp, entry.payload_start, entry.payload_end, ID_TRACKTYPE, &type_el);
            bool has_codec = ebml_find_child(fp, entry.payload_start, entry.payload_end, ID_CODECID, &codec_el);
            bool has_num = ebml_find_child(fp, entry.payload_start, entry.payload_end, ID_TRACKNUMBER, &num_el);
            if (has_type && has_codec && has_num) {
                uint8_t track_type = 0;
                if (fseek(fp, (long)type_el.payload_start, SEEK_SET) == 0) {
                    if (fread(&track_type, 1, 1, fp) != 1) track_type = 0;
                }
                char codec_id[16] = {0};
                size_t codec_len = (size_t)(codec_el.payload_end - codec_el.payload_start);
                if (codec_len < sizeof(codec_id) && fseek(fp, (long)codec_el.payload_start, SEEK_SET) == 0) {
                    if (fread(codec_id, 1, codec_len, fp) != codec_len) codec_id[0] = '\0';
                }
                if (track_type == 2 && strcmp(codec_id, "A_OPUS") == 0) {
                    uint64_t track_num = 0;
                    for (uint64_t p = num_el.payload_start; p < num_el.payload_end; p++) {
                        uint8_t b;
                        if (fseek(fp, (long)p, SEEK_SET) != 0 || fread(&b, 1, 1, fp) != 1) return false;
                        track_num = (track_num << 8) | b;
                    }
                    d->track_number = track_num;
                    return true;
                }
            }
        }
        pos = entry.unknown_size ? tracks.payload_end : entry.payload_end;
    }
    return false;
}

static bool webm_advance_cluster(FILE *fp, WebmOpusDemuxer *d)
{
    const uint64_t ID_CLUSTER = 0x1F43B675;
    EbmlElement cl;
    if (!ebml_find_child(fp, d->cluster_scan_pos, d->segment_end, ID_CLUSTER, &cl)) return false;
    d->block_pos = cl.payload_start;
    d->block_end = cl.payload_end;
    d->cluster_scan_pos = cl.payload_end;
    return true;
}

// Le' o payload de um SimpleBlock/Block (mesmo layout nos dois): numero
// da trilha (vint) + timecode relativo (2 bytes) + flags (1 byte) +
// dados. So' suporta "sem lacing" (flags bits 1-2 == 00) - caso comum
// pra' audio de trilha unica; blocos com lacing sao pulados (devolve
// false), preferindo perder um pacote raro a arriscar decodificar
// varios frames agrupados incorretamente.
static bool webm_parse_block(FILE *fp, uint64_t payload_start, uint64_t payload_end,
                              uint64_t *out_track_num, uint64_t *out_data_start, uint64_t *out_data_end)
{
    uint64_t track_num, pos;
    if (!ebml_read_vint(fp, payload_start, true, &track_num, &pos)) return false;
    if (pos + 3 > payload_end) return false;
    uint8_t hdr[3];
    if (fseek(fp, (long)pos, SEEK_SET) != 0 || fread(hdr, 1, 3, fp) != 3) return false;
    uint8_t lacing = (hdr[2] >> 1) & 0x3;
    *out_track_num = track_num;
    *out_data_start = pos + 3;
    *out_data_end = payload_end;
    return lacing == 0;
}

// Acha o proximo pacote Opus (SimpleBlock OU BlockGroup>Block da trilha
// certa) a partir da posicao atual, avancando pros proximos Clusters
// automaticamente quando os blocos do atual se esgotam. Marca
// d->eof=true quando nao ha' mais Clusters (fim de verdade da faixa).
// Blocos de outras trilhas, com lacing, ou de tipos que nao sao
// blocos de audio (Timecode, PrevSize etc) sao pulados silenciosamente.
static bool webm_next_packet(FILE *fp, WebmOpusDemuxer *d, uint64_t *out_start, uint64_t *out_end)
{
    const uint64_t ID_SIMPLEBLOCK = 0xA3;
    const uint64_t ID_BLOCKGROUP = 0xA0;
    const uint64_t ID_BLOCK = 0xA1;

    for (;;) {
        if (d->block_pos >= d->block_end) {
            if (!webm_advance_cluster(fp, d)) {
                d->eof = true;
                return false;
            }
            continue;
        }
        EbmlElement el;
        if (!ebml_read_element(fp, d->block_pos, d->block_end, &el)) {
            d->eof = true; // dado invalido - desiste (mais seguro que tentar recuperar as cegas)
            return false;
        }
        d->block_pos = el.unknown_size ? d->block_end : el.payload_end;

        if (el.id == ID_SIMPLEBLOCK) {
            uint64_t tn, ds, de;
            if (webm_parse_block(fp, el.payload_start, el.payload_end, &tn, &ds, &de) && tn == d->track_number) {
                *out_start = ds; *out_end = de;
                return true;
            }
        } else if (el.id == ID_BLOCKGROUP) {
            EbmlElement blk;
            if (ebml_find_child(fp, el.payload_start, el.payload_end, ID_BLOCK, &blk)) {
                uint64_t tn, ds, de;
                if (webm_parse_block(fp, blk.payload_start, blk.payload_end, &tn, &ds, &de) && tn == d->track_number) {
                    *out_start = ds; *out_end = de;
                    return true;
                }
            }
        }
        // outro tipo de elemento, trilha errada, ou lacing - continua.
    }
}

static bool webm_opus_demuxer_init(FILE *fp, uint64_t file_size, WebmOpusDemuxer *d)
{
    *d = WebmOpusDemuxer{};
    d->file_size = file_size;
    return webm_find_opus_track(fp, file_size, d);
}

// Le' EXATAMENTE UM pacote Opus pra' dentro de `dst` (capacidade `cap`) -
// NUNCA concatena 2+ pacotes numa unica chamada (ver nota grande no topo
// desta secao - Opus cru nao e' auto-sincronizavel). Devolve o tamanho
// do pacote lido, ou 0 se acabaram os pacotes (fim de verdade da faixa)
// ou se o pacote for grande demais pra' `cap` (nao deveria acontecer na
// pratica - pacotes Opus sao tipicamente algumas centenas de bytes).
static size_t webm_opus_read_one_packet(FILE *fp, WebmOpusDemuxer *d, uint8_t *dst, size_t cap)
{
    uint64_t ps, pe;
    if (!webm_next_packet(fp, d, &ps, &pe)) return 0;
    uint64_t len = pe - ps;
    if (len == 0 || len > cap) return 0;
    if (fseek(fp, (long)ps, SEEK_SET) != 0) return 0;
    if (fread(dst, 1, (size_t)len, fp) != (size_t)len) return 0;
    return (size_t)len;
}

// --- Ogg (.ogg/.opus) -------------------------------------------------------
// CORRECAO DE ROTA (ver AUDIO_FORMATS_PLAN.md): existia aqui um demuxer
// manual proprio (OggPacketDemuxer) que extraia pacotes Vorbis/Opus crus
// pra alimentar os tipos "raw VORBIS"/"raw OPUS" do Simple Decoder. Um bug
// real foi achado e corrigido nele (pacote 1/cabecalho de identificacao
// Vorbis sendo descartado por engano), mas MESMO ASSIM Vorbis/Opus via Ogg
// continuavam sem tocar depois do fix - a causa real era mais simples:
// a doc oficial da esp_audio_codec (conferida na v2.6.1, pagina completa,
// nao so' o resumo da introducao) tem um tipo de container "OGG" dedicado
// ("OGG | Supports VORBIS, OPUS") que aceita o arquivo Ogg genuino, com o
// framing de pagina intacto, e resolve Vorbis-vs-Opus por dentro sozinho -
// exatamente o mesmo padrao simples ja usado com sucesso pra
// M4A/FLAC/MP3/WAV/TS/AMR (so' entregar o arquivo cru, sem pre-processar).
// O demuxer manual foi removido - AudioFormat::Ogg agora mapeia direto pra
// ESP_AUDIO_SIMPLE_DEC_TYPE_OGG (ver audio_decoder.h), sem nenhum
// tratamento especial no loop de leitura (cai no fread() generico, igual
// os outros formatos "simples").

// Se detectar mdat ANTES de moov, devolve um buffer malloc'd (PSRAM) com
// ftyp+moov concatenados na ordem que o decoder precisa, e deixa `fp`
// posicionado exatamente no cabecalho do mdat (pronto pro loop principal
// ler dali em diante, normalmente). Devolve nullptr (out_len=0) se a
// ordem ja' esta' correta, se nao achou moov, ou se algo pareceu invalido
// demais pra mexer com seguranca - nesses casos deixa fp em 0 e nao muda
// nada, o resto do codigo segue como se essa funcao nao existisse.
static uint8_t *prepare_m4a_prefix(FILE *fp, uint64_t file_size, size_t *out_len)
{
    *out_len = 0;

    M4aBoxInfo boxes;
    if (!scan_m4a_top_level_boxes(fp, file_size, &boxes)) {
        fseek(fp, 0, SEEK_SET);
        return nullptr;
    }

    if (!boxes.found_mdat || boxes.mdat_start >= boxes.moov_start) {
        // Ordem ja' e' a esperada (moov antes de mdat, ou nao achou mdat
        // nenhum pra comparar) - nada a fazer.
        fseek(fp, 0, SEEK_SET);
        return nullptr;
    }

    bool include_ftyp = (boxes.ftyp_size > 0 && boxes.ftyp_start < boxes.moov_start);
    uint64_t prefix_total = boxes.moov_size + (include_ftyp ? boxes.ftyp_size : 0);

    // moov de 0 byte ou maior que 32MB (bem alem de qualquer coisa
    // razoavel, mesmo com tabelas gigantes) - desiste em vez de arriscar
    // uma alocacao descontrolada; o arquivo so' vai continuar falhando
    // como antes, sem piorar nada.
    if (prefix_total == 0 || prefix_total > 32UL * 1024 * 1024) {
        fseek(fp, 0, SEEK_SET);
        return nullptr;
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc(prefix_total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        fseek(fp, 0, SEEK_SET);
        return nullptr;
    }

    size_t off = 0;
    if (include_ftyp) {
        fseek(fp, (long)boxes.ftyp_start, SEEK_SET);
        size_t n = fread(buf, 1, boxes.ftyp_size, fp);
        off += n;
        if (n != boxes.ftyp_size) { free(buf); fseek(fp, 0, SEEK_SET); return nullptr; }
    }
    fseek(fp, (long)boxes.moov_start, SEEK_SET);
    size_t n = fread(buf + off, 1, boxes.moov_size, fp);
    off += n;
    if (n != boxes.moov_size) { free(buf); fseek(fp, 0, SEEK_SET); return nullptr; }

    // Pronto: fp fica no cabecalho do mdat, o loop principal continua
    // lendo dali normalmente depois de esgotar esse prefixo.
    fseek(fp, (long)boxes.mdat_start, SEEK_SET);

    ESP_LOGI(TAG, "M4A com mdat antes de moov detectado - reordenando (moov: %llu bytes)",
             (unsigned long long)prefix_total);

    *out_len = off;
    return buf;
}

// --- Prefetch do proximo arquivo --------------------------------------
struct Prefetch {
    FILE *fp = nullptr;
    uint8_t *buf = nullptr;
    size_t valid = 0;
    uint64_t file_size = 0;
    int index = -1;
    // Pasta de onde `index`/`fp` foram de fato lidos (copia de
    // s_playback_dir no momento do prefetch). BUG ENCONTRADO: sem isso,
    // a checagem de "ja' prefetchado" comparava so' o NUMERO do indice -
    // se o usuario trocasse de pasta bem no meio de uma musica (troca e'
    // imediata, feita pela touch_task via audio_player_select_entry(),
    // sem esperar a faixa atual terminar), o prefetch antigo (da pasta
    // ANTERIOR) ficava pendurado sem ser invalidado. Se o "proximo
    // indice" calculado pra' pasta NOVA coincidisse por acaso com esse
    // indice velho (bem comum - gente costuma comecar do inicio do
    // album, e' assim que "proximo" vira 0->1 toda vez), o prefetch era
    // erroneamente considerado valido e a SEGUNDA musica tocava com o
    // audio da pasta anterior (o titulo mostrado na tela, porem, vinha
    // certo da pasta nova - metadados sao lidos a parte, so' os BYTES de
    // audio vinham do arquivo errado). Agora index so' e' considerado
    // valido se `dir` tambem bater com a pasta de reproducao atual.
    char dir[DIR_LEN] = {0};
    // Prefixo M4A "fora de ordem" (moov lido de outra posicao do arquivo -
    // ver prepare_m4a_prefix()) que nao coube inteiro no primeiro
    // carregamento de `buf`. nullptr/0 no caso comum (arquivo normal, ou
    // nem e' M4A). Dono da memoria passa pra' play_track() quando essa
    // faixa comeca a tocar de verdade.
    uint8_t *pending_m4a_prefix = nullptr;
    size_t pending_m4a_prefix_len = 0;
    size_t pending_m4a_prefix_offset = 0;
};
static Prefetch s_prefetch;

static void prefetch_next_file(int next_index)
{
    scan_lock();
    if (next_index < 0 || next_index >= s_playback_scan.audio_count) {
        scan_unlock();
        return;
    }
    if (s_prefetch.index == next_index && s_prefetch.fp &&
        strcmp(s_prefetch.dir, s_playback_dir) == 0) {
        scan_unlock();
        return; // ja' prefetchado (mesmo indice E mesma pasta)
    }

    char path[MAX_PATH_LEN];
    char dir_snapshot[DIR_LEN];
    snprintf(path, sizeof(path), "%s/%s", s_playback_dir, s_playback_scan.audio_files[next_index]);
    strncpy(dir_snapshot, s_playback_dir, sizeof(dir_snapshot) - 1);
    dir_snapshot[sizeof(dir_snapshot) - 1] = '\0';
    scan_unlock();

    if (s_prefetch.fp) {
        fclose(s_prefetch.fp);
        s_prefetch.fp = nullptr;
    }
    if (s_prefetch.pending_m4a_prefix) {
        free(s_prefetch.pending_m4a_prefix);
        s_prefetch.pending_m4a_prefix = nullptr;
        s_prefetch.pending_m4a_prefix_len = 0;
        s_prefetch.pending_m4a_prefix_offset = 0;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        s_prefetch.index = -1;
        return;
    }

    struct stat st;
    s_prefetch.file_size = (fstat(fileno(fp), &st) == 0) ? (uint64_t)st.st_size : 0;

    if (!s_prefetch.buf) {
        s_prefetch.buf = (uint8_t *)heap_caps_malloc(INBUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    mps3::AudioFormat fmt = mps3::detect_audio_format(path);

    // WebM/Opus (ver WebmOpusDemuxer) usa demuxer proprio que so' faz
    // sentido dentro de play_track() - prefetch aqui so' serve pra deixar
    // arquivos NORMAIS prontos com antecedencia. BUG ENCONTRADO: mesmo
    // pulando o calculo do prefixo pra esse caso, o codigo abaixo ainda
    // caia no fallback generico (fread cru pro buffer) e MARCAVA
    // s_prefetch.fp como valido mesmo assim - fazendo play_track()
    // reaproveitar esse prefetch "lixo" (bytes crus de EBML, nunca
    // demuxados de verdade) em vez de passar pelo caminho novo. Agora
    // esse formato simplesmente NAO e' prefetchado - fecha o arquivo e
    // sai, deixando s_prefetch vazio (mesmo estado de quando fopen falha
    // logo acima). Custa, no maximo, um pequeno atraso ao trocar pra essa
    // faixa - bem melhor que arriscar tocar lixo.
    //
    // Ogg (.ogg/.opus) NAO precisa mais dessa exclusao (ver comentario
    // grande "--- Ogg (.ogg/.opus) ---" em play_track()) - agora usa
    // ESP_AUDIO_SIMPLE_DEC_TYPE_OGG, que aceita o arquivo cru igual
    // qualquer formato "normal" - cai direto no fallback generico
    // (fread cru) logo abaixo, sem tratamento especial, e prefetch
    // funciona normalmente.
    if (fmt == mps3::AudioFormat::Webm) {
        fclose(fp);
        s_prefetch.index = -1;
        return;
    }
    bool m4a_fragmented = false;
    if (fmt == mps3::AudioFormat::M4a) {
        M4aBoxInfo box;
        m4a_fragmented = scan_m4a_top_level_boxes(fp, s_prefetch.file_size, &box) &&
                          m4a_is_fragmented(fp, box.moov_start, box.moov_size);
        if (m4a_fragmented) {
            fclose(fp);
            s_prefetch.index = -1;
            return;
        }
    }

    uint8_t flac_prefix[42];
    size_t prefix_len = skip_leading_metadata(fp, fmt, flac_prefix);

    uint8_t *m4a_prefix = nullptr;
    size_t m4a_prefix_len = 0;
    if (fmt == mps3::AudioFormat::M4a) {
        m4a_prefix = prepare_m4a_prefix(fp, s_prefetch.file_size, &m4a_prefix_len);
    }

    size_t n = 0;
    if (s_prefetch.buf) {
        if (prefix_len > 0) {
            // FLAC - sempre cabe de uma vez so' (42 bytes fixos).
            memcpy(s_prefetch.buf, flac_prefix, prefix_len);
            n = prefix_len + fread(s_prefetch.buf + prefix_len, 1, INBUF_SIZE - prefix_len, fp);
        } else if (m4a_prefix_len > 0) {
            // M4A fora de ordem - pode nao caber tudo de uma vez; o que
            // sobrar fica pendente e continua sendo drenado dentro do
            // proprio loop de play_track() (ver pending_m4a_prefix_*).
            size_t from_prefix = m4a_prefix_len < INBUF_SIZE ? m4a_prefix_len : INBUF_SIZE;
            memcpy(s_prefetch.buf, m4a_prefix, from_prefix);
            n = from_prefix;
            if (from_prefix < m4a_prefix_len) {
                s_prefetch.pending_m4a_prefix = m4a_prefix;
                s_prefetch.pending_m4a_prefix_len = m4a_prefix_len;
                s_prefetch.pending_m4a_prefix_offset = from_prefix;
                m4a_prefix = nullptr; // posse passou pro s_prefetch - nao libera abaixo
            } else {
                n += fread(s_prefetch.buf + from_prefix, 1, INBUF_SIZE - from_prefix, fp);
            }
        } else {
            n = fread(s_prefetch.buf, 1, INBUF_SIZE, fp);
        }
    }
    if (m4a_prefix) free(m4a_prefix); // so' sobra algo aqui se coube tudo (nao virou pendente)

    s_prefetch.fp = fp;
    s_prefetch.valid = n;
    s_prefetch.index = next_index;
    strncpy(s_prefetch.dir, dir_snapshot, sizeof(s_prefetch.dir) - 1);
    s_prefetch.dir[sizeof(s_prefetch.dir) - 1] = '\0';
}

// Reposiciona `fp` pro offset estimado do seek rapido (fast_seek_offset)
// e prepara o INICIO de `inbuf` com o que o decoder precisa ver antes de
// qualquer frame de audio. BUG ENCONTRADO: pra' FLAC isso faltava
// completamente - o codigo so' fazia fseek() cru pro meio do arquivo, mas
// o decoder FLAC PRECISA ver "fLaC"+STREAMINFO antes de decodificar
// qualquer frame (mesmo motivo ja documentado em skip_leading_metadata() -
// esse caminho de seek rapido nao passava por ela, entao FLAC falhava
// sempre que o seek rapido entrava em acao, virando um "pulo pra proxima
// faixa" em vez de um avanco). MP3/WAV/AAC nao precisam disso (frames
// autocontidos - qualquer offset serve de ponto de partida), entao pulam
// direto.
// Devolve quantos bytes ja' ficaram validos no inicio de `inbuf` (so' o
// prefixo, se houver - o resto enche naturalmente no loop de decodificacao
// mais abaixo, a partir de onde este fseek deixou o arquivo).
static size_t seek_and_prime_inbuf(FILE *fp, uint8_t *inbuf, uint64_t file_size,
                                    uint64_t fast_seek_offset, mps3::AudioFormat fmt)
{
    size_t prefix_len = 0;
    uint8_t flac_prefix[42];

    if (fmt == mps3::AudioFormat::Flac) {
        fseek(fp, 0, SEEK_SET);
        prefix_len = skip_leading_metadata(fp, fmt, flac_prefix);
        uint64_t audio_start = 0;
        if (prefix_len > 0) {
            audio_start = (uint64_t)ftell(fp); // logo apos o STREAMINFO = 1o frame real
            if (fast_seek_offset < audio_start) fast_seek_offset = audio_start;
        }
        memcpy(inbuf, flac_prefix, prefix_len);

        // Procura o proximo cabecalho de frame FLAC valido (syncword 0xFF 0xF8) a partir do offset estimado
        if (fast_seek_offset > audio_start && fast_seek_offset < file_size) {
            uint64_t scan_pos = fast_seek_offset;
            size_t max_scan = 32768; // varre ate 32KB a frente procurando frame sync
            if (scan_pos + max_scan > file_size) max_scan = (size_t)(file_size - scan_pos);
            fseek(fp, (long)scan_pos, SEEK_SET);
            uint8_t scan[4096];
            bool found = false;
            size_t total_scanned = 0;
            while (total_scanned < max_scan && !found) {
                size_t to_read = sizeof(scan);
                if (total_scanned + to_read > max_scan) to_read = max_scan - total_scanned;
                size_t nscan = fread(scan, 1, to_read, fp);
                if (nscan < 4) break;
                for (size_t i = 0; i + 4 <= nscan; i++) {
                    if (scan[i] == 0xFF && (scan[i+1] & 0xFE) == 0xF8) {
                        uint8_t bs = scan[i+2] >> 4;
                        uint8_t sr = scan[i+2] & 0x0F;
                        uint8_t ch = scan[i+3] >> 4;
                        uint8_t ss = (scan[i+3] >> 1) & 0x07;
                        if (bs != 0 && bs != 0x0F && sr != 0x0F && ch <= 0x0A && ss != 0x03 && (scan[i+3] & 0x01) == 0) {
                            fast_seek_offset = scan_pos + total_scanned + i;
                            found = true;
                            ESP_LOGI(TAG, "FLAC seek alinhado: delta=%zu bytes -> offset %llu",
                                     total_scanned + i, (unsigned long long)fast_seek_offset);
                            break;
                        }
                    }
                }
                if (!found) {
                    if (nscan > 3) {
                        total_scanned += (nscan - 3);
                        fseek(fp, (long)(scan_pos + total_scanned), SEEK_SET);
                    } else {
                        break;
                    }
                }
            }
        }
    } else if (fmt == mps3::AudioFormat::Ogg) {
        // Ogg Vorbis/Opus precisa das paginas de cabecalho (BOS/codebooks)
        fseek(fp, 0, SEEK_SET);
        prefix_len = fread(inbuf, 1, 4096, fp);

        // Procura a proxima pagina "OggS" a partir do offset estimado
        if (fast_seek_offset > file_size) fast_seek_offset = file_size;
        fseek(fp, (long)fast_seek_offset, SEEK_SET);
        uint8_t scan[4096];
        size_t nscan = fread(scan, 1, sizeof(scan), fp);
        size_t ogg_page_offset = 0;
        for (size_t i = 0; i + 4 <= nscan; i++) {
            if (scan[i] == 'O' && scan[i+1] == 'g' && scan[i+2] == 'g' && scan[i+3] == 'S') {
                ogg_page_offset = i;
                break;
            }
        }
        fast_seek_offset += ogg_page_offset;
    }

    if (fast_seek_offset > file_size) fast_seek_offset = file_size;
    fseek(fp, (long)fast_seek_offset, SEEK_SET);

    return prefix_len;
}

static void play_track(int index, const char *display_name, uint32_t resume_elapsed_sec)
{
    FILE *fp = nullptr;
    uint8_t *inbuf = nullptr;
    size_t data_start = 0;
    size_t valid_end = 0;
    uint64_t file_size = 0;

    // M4A "fora de ordem" (moov reordenado virtualmente - ver
    // prepare_m4a_prefix()) que ainda nao foi todo entregue pro decoder.
    // Drenado a cada volta do loop principal mais abaixo, antes de ler
    // mais dados do arquivo de verdade.
    uint8_t *pending_m4a_prefix = nullptr;
    size_t pending_m4a_prefix_len = 0;
    size_t pending_m4a_prefix_offset = 0;

    // Ativo so' quando essa faixa e' um M4A/AAC fragmentado (ver
    // M4aFragDemuxer mais acima) - nesse caso, os dados "de audio" que
    // alimentam o decoder vem de m4a_frag_read() em vez de fread() direto
    // no arquivo (que teria moof/sidx/etc misturados no meio, corrompendo
    // o fluxo).
    bool use_frag_demux = false;
    M4aFragDemuxer frag_demux;

    // Ativo so' quando essa faixa e' WebM/Opus (ver WebmOpusDemuxer mais
    // acima) - igual ao use_frag_demux, mas com uma regra extra: cada
    // pacote Opus tem que ser entregue sozinho numa chamada de decode(),
    // nunca concatenado com outro (ver nota grande no WebmOpusDemuxer).
    bool use_webm_demux = false;
    WebmOpusDemuxer webm_demux;

    // (Ogg/.opus nao usa mais demuxer proprio - ver comentario grande
    // "--- Ogg (.ogg/.opus) ---" mais acima. Cai no caminho generico
    // fread(), igual FLAC/MP3/WAV/M4A nao-fragmentado.)

    // Metadados (duracao, tags, e o offset/taxa de bytes usados pro seek
    // rapido) - extraidos JA' AQUI (antes de decidir como abrir o
    // arquivo) porque o seek rapido precisa saber ONDE pular ANTES de
    // preparar o buffer de entrada, pra' nao perder tempo lendo/pulando
    // metadados do INICIO do arquivo quando o destino e' logo no meio.
    track_metadata_t meta;
    char meta_path[MAX_PATH_LEN];
    snprintf(meta_path, sizeof(meta_path), "%s/%s", s_playback_dir, display_name);
    metadata_extract(meta_path, &meta); // rapido - so' le' os primeiros KB, nao decodifica audio

    // Seek rapido: se sabemos onde o audio comeca e uma taxa media de
    // bytes/segundo (meta.avg_byte_rate > 0 - ver metadata_extract.h),
    // da' pra' estimar direto o offset de bytes do alvo e pular pra' la'
    // com fseek, em vez de decodificar (e descartar) a faixa inteira
    // desde o comeco - essencial pra' podcasts de horas. So' cobre
    // FLAC/MP3/WAV/AAC por enquanto (M4A ainda nao calcula avg_byte_rate -
    // ver nota em metadata_extract.h) - nesse caso cai pro jeito antigo
    // (decodificar desde o inicio) automaticamente, sem checagem extra
    // aqui.
    mps3::AudioFormat fmt_detected = mps3::detect_audio_format(display_name);
    uint64_t fast_seek_offset = 0;
    bool use_fast_seek = (resume_elapsed_sec > 0 && meta.avg_byte_rate > 0 && fmt_detected != mps3::AudioFormat::M4a);
    if (use_fast_seek) {
        fast_seek_offset = meta.audio_data_offset + (uint64_t)meta.avg_byte_rate * resume_elapsed_sec;
    }

    scan_lock();
    bool prefetch_matches = s_prefetch.fp && s_prefetch.index == index &&
                             strcmp(s_prefetch.dir, s_playback_dir) == 0;
    scan_unlock();

    if (prefetch_matches) {
        fp = s_prefetch.fp;
        file_size = s_prefetch.file_size;
        s_prefetch.fp = nullptr;
        s_prefetch.index = -1;

        if (use_fast_seek) {
            // O prefetch ja' tinha carregado o buffer pensando em tocar
            // do INICIO - descarta isso e reposiciona pro alvo. So'
            // deveria acontecer em teoria se um seek for pedido bem no
            // instante em que a proxima faixa acabou de ser prefetchada -
            // raro, mas tratado com seguranca de qualquer jeito.
            inbuf = s_prefetch.buf;
            s_prefetch.buf = nullptr;
            if (s_prefetch.pending_m4a_prefix) free(s_prefetch.pending_m4a_prefix); // nao deveria ter (M4A nao usa fast seek), mas por seguranca
            s_prefetch.pending_m4a_prefix = nullptr;
            s_prefetch.pending_m4a_prefix_len = 0;
            s_prefetch.pending_m4a_prefix_offset = 0;
            mps3::AudioFormat fmt_for_seek = mps3::detect_audio_format(display_name);
            valid_end = seek_and_prime_inbuf(fp, inbuf, file_size, fast_seek_offset, fmt_for_seek);
            data_start = 0;
        } else {
            inbuf = s_prefetch.buf;
            valid_end = s_prefetch.valid;
            s_prefetch.buf = nullptr;
            pending_m4a_prefix = s_prefetch.pending_m4a_prefix;
            pending_m4a_prefix_len = s_prefetch.pending_m4a_prefix_len;
            pending_m4a_prefix_offset = s_prefetch.pending_m4a_prefix_offset;
            s_prefetch.pending_m4a_prefix = nullptr;
            s_prefetch.pending_m4a_prefix_len = 0;
            s_prefetch.pending_m4a_prefix_offset = 0;
        }
    } else {
        // O prefetch existente (se houver) nao serve pra essa faixa -
        // indice diferente ou (o bug corrigido acima) pasta diferente.
        // Descarta ele agora em vez de deixar pendurado ate' a proxima
        // chamada de prefetch_next_file(): se essa faixa terminar rapido
        // demais pra' chegar a prefetchar (ou o usuario trocar de pasta
        // de novo antes disso), o file descriptor antigo ficaria aberto
        // indefinidamente.
        if (s_prefetch.fp) {
            fclose(s_prefetch.fp);
            s_prefetch.fp = nullptr;
            s_prefetch.index = -1;
        }
        if (s_prefetch.pending_m4a_prefix) {
            free(s_prefetch.pending_m4a_prefix);
            s_prefetch.pending_m4a_prefix = nullptr;
            s_prefetch.pending_m4a_prefix_len = 0;
            s_prefetch.pending_m4a_prefix_offset = 0;
        }

        char full_path[MAX_PATH_LEN];
        snprintf(full_path, sizeof(full_path), "%s/%s", s_playback_dir, display_name);
        fp = fopen(full_path, "rb");
        if (!fp) {
            ESP_LOGE(TAG, "Nao foi possivel abrir %s", full_path);
            state_lock();
            strncpy(s_state.filename, display_name, sizeof(s_state.filename) - 1);
            s_state.filename[sizeof(s_state.filename) - 1] = '\0';
            snprintf(s_state.last_error, sizeof(s_state.last_error), "Erro ao abrir arquivo");
            state_unlock();
            vTaskDelay(pdMS_TO_TICKS(1800));
            return;
        }
        struct stat st;
        file_size = (fstat(fileno(fp), &st) == 0) ? (uint64_t)st.st_size : 0;

        inbuf = (uint8_t *)heap_caps_malloc(INBUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!inbuf) {
            ESP_LOGE(TAG, "Sem memoria para buffer de entrada");
            fclose(fp);
            return;
        }

        if (use_fast_seek) {
            // Vai direto pro offset estimado - nao precisa (nem faz
            // sentido) pular metadados do INICIO do arquivo do jeito
            // "normal", ja' vamos pular pra' bem depois deles. EXCECAO:
            // FLAC ainda precisa ver "fLaC"+STREAMINFO antes de qualquer
            // frame - ver seek_and_prime_inbuf().
            mps3::AudioFormat fmt_for_seek = mps3::detect_audio_format(display_name);
            valid_end = seek_and_prime_inbuf(fp, inbuf, file_size, fast_seek_offset, fmt_for_seek);
            data_start = 0;
            ESP_LOGI(TAG, "Seek rapido: %s -> ~%u s (offset estimado %llu de %llu bytes)",
                     display_name, (unsigned)resume_elapsed_sec,
                     (unsigned long long)fast_seek_offset, (unsigned long long)file_size);
        } else {
            mps3::AudioFormat fmt = mps3::detect_audio_format(display_name);
            uint8_t flac_prefix[42];
            size_t prefix_len = skip_leading_metadata(fp, fmt, flac_prefix);
            if (prefix_len > 0) {
                memcpy(inbuf, flac_prefix, prefix_len);
                valid_end = prefix_len + fread(inbuf + prefix_len, 1, INBUF_SIZE - prefix_len, fp);
            } else if (fmt == mps3::AudioFormat::M4a) {
                M4aBoxInfo box;
                bool have_moov = scan_m4a_top_level_boxes(fp, file_size, &box);
                if (have_moov && m4a_is_fragmented(fp, box.moov_start, box.moov_size)) {
                    // MP4 fragmentado (ver M4aFragDemuxer) - bypassa o
                    // decoder M4A "container-aware" (que nao entende
                    // fragmentacao) e alimenta frames AAC brutos
                    // extraidos manualmente, direto no decoder AAC comum.
                    if (m4a_frag_demuxer_init(fp, file_size, box.moov_start, box.moov_size, &frag_demux)) {
                        use_frag_demux = true;
                        valid_end = m4a_frag_read(fp, &frag_demux, inbuf, INBUF_SIZE);
                    }
                    // Se a inicializacao falhar (ex: codec dentro do MP4
                    // fragmentado nao e' AAC - ALAC, por exemplo),
                    // use_frag_demux fica false e valid_end fica 0: cai
                    // pro comportamento antigo (decoder M4A tentando por
                    // conta propria - sabemos que tende a falhar num
                    // arquivo fragmentado, mas pelo menos nao regride
                    // nada em relacao a antes desse fix).
                } else if (have_moov) {
                    size_t m4a_prefix_len = 0;
                    uint8_t *m4a_prefix = prepare_m4a_prefix(fp, file_size, &m4a_prefix_len);
                    if (m4a_prefix_len > 0) {
                        size_t from_prefix = m4a_prefix_len < INBUF_SIZE ? m4a_prefix_len : INBUF_SIZE;
                        memcpy(inbuf, m4a_prefix, from_prefix);
                        valid_end = from_prefix;
                        if (from_prefix < m4a_prefix_len) {
                            pending_m4a_prefix = m4a_prefix;
                            pending_m4a_prefix_len = m4a_prefix_len;
                            pending_m4a_prefix_offset = from_prefix;
                        } else {
                            valid_end += fread(inbuf + from_prefix, 1, INBUF_SIZE - from_prefix, fp);
                            free(m4a_prefix);
                        }
                    }
                }
            } else if (fmt == mps3::AudioFormat::Webm) {
                // WebM/Opus (ver WebmOpusDemuxer) - le' exatamente 1
                // pacote pra' comecar; o loop principal mais abaixo
                // continua chamando webm_opus_read_one_packet() um
                // pacote de cada vez, nunca concatenando 2+ numa unica
                // entrega ao decoder (ver nota grande no WebmOpusDemuxer).
                if (webm_opus_demuxer_init(fp, file_size, &webm_demux)) {
                    use_webm_demux = true;
                    valid_end = webm_opus_read_one_packet(fp, &webm_demux, inbuf, INBUF_SIZE);
                }
                // Se a inicializacao falhar (ex: trilha nao e' Opus - e'
                // Vorbis, mais raro em audio do YouTube), use_webm_demux
                // fica false e valid_end fica 0 - a faixa nao vai tocar,
                // mas tambem nao trava/corrompe nada.
            }
            // Ogg (.ogg/.opus) NAO tem bloco especial aqui de proposito -
            // ver comentario grande "--- Ogg (.ogg/.opus) ---" mais acima:
            // usa o tipo de container ESP_AUDIO_SIMPLE_DEC_TYPE_OGG, que
            // aceita o arquivo cru, entao cai no mesmo caminho generico
            // que MP3/AAC/WAV/TS/AMR ja usam (valid_end fica 0 aqui, o
            // loop principal mais abaixo enche o buffer do zero).
            // Se prefix_len == 0 (nao e' FLAC, ou nao achou STREAMINFO), deixa
            // valid_end em 0 mesmo - a primeira iteracao do loop mais abaixo
            // ja' enche o buffer do zero a partir de onde skip_leading_metadata
            // deixou o ponteiro (so' apos o ID3v2, se havia).
        }
    }

    mps3::AudioFormat fmt = mps3::detect_audio_format(display_name);
    mps3::AudioFormat fmt_for_decoder = fmt;
    if (use_frag_demux) {
        fmt_for_decoder = mps3::AudioFormat::Aac;
    }
    // (Ogg nao precisa mais de reclassificacao - fmt_for_decoder fica
    // AudioFormat::Ogg mesmo, que audio_format_to_simple_dec() mapeia
    // direto pra ESP_AUDIO_SIMPLE_DEC_TYPE_OGG.)
    std::unique_ptr<mps3::AudioDecoderBase> decoder = create_decoder(fmt_for_decoder, file_size);
    if (!decoder) {
        // BUG ENCONTRADO: essa falha era 100% silenciosa pra' quem usa o
        // aparelho - so' aparecia um ESP_LOGE no log serial (que ninguem
        // ve' no dia a dia), e a faixa "desaparecia" quase instantaneamente
        // porque player_task() trata um play_track() que retorna sem
        // nunca ter tocado nada exatamente como "a faixa acabou
        // normalmente" e ja' avanca pra' proxima - do ponto de vista de
        // quem esta' ouvindo, parece que o arquivo foi pulado sem
        // explicacao nenhuma (foi assim que os arquivos WebM apareceram
        // "pulados" quando o tipo de decoder OPUS nao estava disponivel
        // na versao da biblioteca resolvida pelo build). Agora a tela
        // mostra o motivo, e a task espera um pouco antes de seguir pra'
        // proxima faixa - da' tempo de ler o que aconteceu em vez de so'
        // notar que "nao tocou".
        ESP_LOGE(TAG, "Formato nao suportado: %s", display_name);
        state_lock();
        strncpy(s_state.filename, display_name, sizeof(s_state.filename) - 1);
        s_state.filename[sizeof(s_state.filename) - 1] = '\0';
        snprintf(s_state.last_error, sizeof(s_state.last_error), "Formato nao suportado");
        state_unlock();
        if (pending_m4a_prefix) free(pending_m4a_prefix);
        if (use_frag_demux) m4a_frag_demuxer_free(&frag_demux);
        heap_caps_free(inbuf);
        fclose(fp);
        vTaskDelay(pdMS_TO_TICKS(1800)); // da' tempo de ler a mensagem na tela antes do proximo arquivo
        return;
    }

    state_lock();
    s_state.last_error[0] = '\0'; // faixa atual carregou - qualquer erro anterior nao se aplica mais
    state_unlock();

    ESP_LOGI(TAG, "Tocando: %s/%s", s_playback_dir, display_name);

    int32_t *output_buf = nullptr;
    size_t output_capacity_samples = 0;

    bool header_ready = false;
    bool prefetched_next = false;
    uint32_t sample_rate = 44100;
    uint32_t num_channels = 2;
    uint64_t elapsed_samples = 0;

    state_lock();
    strncpy(s_state.filename, display_name, sizeof(s_state.filename) - 1);
    s_state.filename[sizeof(s_state.filename) - 1] = '\0';

    if (meta.title[0]) {
        strncpy(s_state.title, meta.title, sizeof(s_state.title) - 1);
        s_state.title[sizeof(s_state.title) - 1] = '\0';
    } else {
        // Sem tag de titulo - usa o nome do arquivo sem a extensao.
        strncpy(s_state.title, display_name, sizeof(s_state.title) - 1);
        s_state.title[sizeof(s_state.title) - 1] = '\0';
        char *dot = strrchr(s_state.title, '.');
        if (dot) *dot = '\0';
    }
    strncpy(s_state.artist, meta.artist, sizeof(s_state.artist) - 1);
    s_state.artist[sizeof(s_state.artist) - 1] = '\0';
    strncpy(s_state.album, meta.album, sizeof(s_state.album) - 1);
    s_state.album[sizeof(s_state.album) - 1] = '\0';

    if (meta.format_name[0] != '\0') {
        strncpy(s_state.format_name, meta.format_name, sizeof(s_state.format_name) - 1);
        s_state.format_name[sizeof(s_state.format_name) - 1] = '\0';
    } else {
        s_state.format_name[0] = '\0';
    }
    s_state.bits_per_sample = meta.bits_per_sample;
    s_state.sample_rate = meta.sample_rate;
    s_state.bitrate = meta.bitrate;
    s_state.elapsed_sec = resume_elapsed_sec;
    s_state.total_sec = meta.total_sec;
    s_state.duration_is_estimate = meta.duration_is_estimate;
    s_state.playing = true;
    s_state.track_loaded = false;
    state_unlock();
    s_last_elapsed_sec = resume_elapsed_sec;
    eq_reset_state();

    bool prebuffer_done = false;
    size_t prebuffered_frames = 0;
    size_t prebuffer_target_frames = 0;

    s_seek_pending = false;
    if (resume_elapsed_sec > 0) {
        s_seek_target_sec = resume_elapsed_sec;
        s_seek_pending = true;
    }

    uint32_t last_saved_elapsed = 0;

    int stuck_at_eof_count = 0; // ver checagem "at_true_eof" mais abaixo

    bool done = false;
    while (!done) {
        // Transferencia pro modo USB tem prioridade maxima - interrompe
        // mesmo que pausado ou no meio de um seek.
        if (s_usb_takeover_requested) {
            break;
        }
        if (s_pending_cmd != PLAYER_CMD_NONE) {
            break;
        }
        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Otimizacao da janela deslizante do inbuf:
        // Em vez de memmove a cada bloco decodificado, so compacta (memmove em PSRAM)
        // e recarrega do SD quando o consumo atingir pelo menos metade do buffer (INBUF_SIZE / 2)
        // ou quando os bytes restantes no buffer nao forem suficientes para o proximo frame (< 24576).
        // Isso elimina ~70% das copias em PSRAM e chamadas fread(), alcancando ~1.35x tempo real.
        if (!use_webm_demux && data_start > 0 &&
            (data_start >= (INBUF_SIZE / 2) || (valid_end - data_start) < 24576)) {
            size_t remaining = valid_end - data_start;
            memmove(inbuf, inbuf + data_start, remaining);
            data_start = 0;
            valid_end = remaining;
        }

        // Se ainda sobrou prefixo M4A "fora de ordem" (moov reordenado
        // virtualmente - ver prepare_m4a_prefix()), drena ele ANTES de
        // ler mais do arquivo de verdade - pode precisar de varias voltas
        // do loop se o moov for maior que INBUF_SIZE inteiro.
        size_t from_m4a_prefix = 0;
        if (pending_m4a_prefix && valid_end < INBUF_SIZE) {
            size_t avail = pending_m4a_prefix_len - pending_m4a_prefix_offset;
            size_t space = INBUF_SIZE - valid_end;
            from_m4a_prefix = avail < space ? avail : space;
            memcpy(inbuf + valid_end, pending_m4a_prefix + pending_m4a_prefix_offset, from_m4a_prefix);
            pending_m4a_prefix_offset += from_m4a_prefix;
            valid_end += from_m4a_prefix;
            if (pending_m4a_prefix_offset >= pending_m4a_prefix_len) {
                free(pending_m4a_prefix);
                pending_m4a_prefix = nullptr;
            }
        }

        // "fresh_bytes==0" so' significa EOF de verdade se a gente
        // REALMENTE tentou ler (valid_end < INBUF_SIZE); se o buffer ja'
        // estava cheio, a gente nem chega a chamar fread() e fresh_bytes
        // fica 0 por padrao SEM ser EOF - isso e' o caso normal/comum
        // (acontece toda vez que o decoder so' esta' "drenando" PCM ja'
        // decodificado, com bytes_consumed=0 de proposito, sem relacao
        // nenhuma com fim de arquivo). Sem essa distincao, a trava abaixo
        // dispararia toda hora durante reproducao normal. Mesma logica
        // vale se acabamos de drenar algo do prefixo M4A pendente acima -
        // isso tambem NAO e' EOF, mesmo que fread() nao seja chamado
        // nesse ciclo.
        size_t fresh_bytes = 0;
        bool at_true_eof = false;
        if (use_webm_demux) {
            // Regra especial (ver WebmOpusDemuxer): NUNCA acrescenta um
            // pacote novo enquanto sobrar bytes do pacote anterior ainda
            // nao consumidos pelo decoder (data_start < valid_end) - Opus
            // cru nao e' auto-sincronizavel, cada chamada de decode()
            // precisa receber exatamente 1 pacote isolado. Na pratica o
            // decoder deveria sempre consumir o pacote inteiro numa unica
            // chamada (foi isso que alimentamos), entao esse "esperar" e'
            // so' uma rede de seguranca - nunca deveria persistir por mais
            // de uma volta do loop. (Ogg nao usa mais esse ramo - ver
            // comentario grande "--- Ogg (.ogg/.opus) ---" mais acima -
            // agora e' um container "de verdade" pro Simple Decoder,
            // igual FLAC/MP3/WAV/M4A, cai no ramo generico abaixo.)
            if (data_start >= valid_end && valid_end < INBUF_SIZE) {
                data_start = 0;
                valid_end = 0;
                fresh_bytes = webm_opus_read_one_packet(fp, &webm_demux, inbuf, INBUF_SIZE);
                valid_end = fresh_bytes;
                at_true_eof = (fresh_bytes == 0);
            }
        } else if (valid_end < INBUF_SIZE) {
            fresh_bytes = use_frag_demux ? m4a_frag_read(fp, &frag_demux, inbuf + valid_end, INBUF_SIZE - valid_end)
                                          : fread(inbuf + valid_end, 1, INBUF_SIZE - valid_end, fp);
            valid_end += fresh_bytes;
            at_true_eof = (fresh_bytes == 0) && (from_m4a_prefix == 0);
        }

        size_t valid = valid_end - data_start;
        if (valid == 0) {
            break;
        }

        size_t bytes_consumed = 0;
        size_t samples_decoded = 0;

        mps3::DecodeStatus result = decoder->decode(inbuf + data_start, valid,
                                                      output_buf, output_capacity_samples,
                                                      bytes_consumed, samples_decoded);

        if (bytes_consumed > 0 && bytes_consumed <= valid) {
            data_start += bytes_consumed;
        }

        // Trava de seguranca geral: chegamos no fim do arquivo de verdade
        // (tentamos ler mais e fread() nao trouxe NADA) e o decoder nao
        // consumiu nem 1 byte do que sobrou nem decodificou nenhuma
        // amostra - nao ha' como progredir mais nunca. Sem essa checagem,
        // um arquivo menor que INBUF_SIZE com lixo/dados incompletos no
        // final gira pra sempre (a trava de "buffer cheio" mais abaixo so'
        // cobre o caso do buffer ter enchido ate' INBUF_SIZE, o que nunca
        // acontece se o arquivo inteiro for menor que isso - ex.: um FLAC
        // truncado logo apos o STREAMINFO, sem nenhum frame de audio).
        //
        // Exige alguns ciclos SEGUIDOS de zero progresso (nao so' 1) antes
        // de desistir: quando o decoder precisa crescer o buffer de saida
        // internamente (BUFF_NOT_ENOUGH), ele tambem devolve 0 bytes
        // consumidos/0 amostras NESSE ciclo mesmo indo funcionar no
        // proximo - se isso acontecer bem no fim do arquivo, 1 unica
        // ocorrencia nao pode ser tratada como "travado pra sempre".
        if (at_true_eof && bytes_consumed == 0 && samples_decoded == 0) {
            stuck_at_eof_count++;
            if (stuck_at_eof_count >= 4) {
                ESP_LOGW(TAG, "Fim do arquivo alcancado sem conseguir decodificar mais nada; abortando faixa.");
                break;
            }
        } else {
            stuck_at_eof_count = 0;
        }

        switch (result) {
            case mps3::DecodeStatus::HeaderReady: {
                sample_rate = decoder->sample_rate();
                num_channels = decoder->channels();
                output_capacity_samples = decoder->recommended_output_capacity_samples();
                if (output_buf) heap_caps_free(output_buf);
                output_buf = (int32_t *)heap_caps_malloc(output_capacity_samples * sizeof(int32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                if (!output_buf) {
                    output_buf = (int32_t *)heap_caps_malloc(output_capacity_samples * sizeof(int32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                }
                if (!output_buf) {
                    output_buf = (int32_t *)malloc(output_capacity_samples * sizeof(int32_t));
                }

                // Se usamos seek rapido (fseek direto pro offset estimado,
                // ver use_fast_seek mais acima), a decodificacao comecou
                // NO MEIO do arquivo - mas elapsed_samples (declarado la'
                // em cima) sempre comeca em 0, contando so' as amostras
                // decodificadas NESSA sessao. Sem isso, o ajuste fino do
                // seek (s_seek_pending/s_seek_target_sec logo abaixo)
                // pensaria que ainda estamos no instante 0 e tentaria
                // descartar as MESMAS horas que a gente acabou de pular -
                // anulando o ganho todo do seek rapido. Corrige
                // adiantando elapsed_samples pra' bater com a estimativa.
                if (use_fast_seek) {
                    elapsed_samples = (uint64_t)resume_elapsed_sec * sample_rate;
                }

                // Duracao vem de metadata_extract() (chamado mais acima,
                // antes do loop) - o ESP Audio Simple Decoder e' um
                // decodificador de streaming, nao le' o arquivo inteiro
                // nem conta amostras totais (ver nota em audio_decoder.h).
                uint32_t total_sec = meta.total_sec;
                char label[8];
                decoder->format_label(label, sizeof(label));

                uint32_t i2s_rate = sample_rate;
                ESP_LOGI(TAG, "Saida I2S configurada para taxa nativa: %u Hz (Dual-Core: Core 1 decodifica, Core 0 processa DSP/I2S)", (unsigned)i2s_rate);
                audio_dsp_flush();
                audio_dsp_set_rate(i2s_rate);
                audio_dsp_trigger_fade_in(i2s_rate);

                uint32_t target_pb = (i2s_rate > 0) ? ((i2s_rate * 100) / 1000) : 9600;
                // Ring buffer DMA tem 24 descritores x 512 frames = 12.288 frames no maximo
                prebuffer_target_frames = (target_pb > 12288) ? 12288 : target_pb;
                prebuffered_frames = 0;
                prebuffer_done = false;

                state_lock();
                s_state.total_sec = total_sec;
                s_state.duration_is_estimate = meta.duration_is_estimate;
                if (meta.format_name[0] != '\0') {
                    strncpy(s_state.format_name, meta.format_name, sizeof(s_state.format_name) - 1);
                } else {
                    strncpy(s_state.format_name, label, sizeof(s_state.format_name) - 1);
                }
                s_state.format_name[sizeof(s_state.format_name) - 1] = '\0';

                if (meta.bits_per_sample > 0) {
                    s_state.bits_per_sample = meta.bits_per_sample;
                } else {
                    s_state.bits_per_sample = decoder->bits_per_sample();
                }

                s_state.sample_rate = sample_rate;
                if (meta.bitrate > 0) {
                    s_state.bitrate = meta.bitrate;
                } else if (meta.avg_byte_rate > 0) {
                    s_state.bitrate = meta.avg_byte_rate * 8;
                } else if (total_sec > 0 && file_size > 0) {
                    s_state.bitrate = (uint32_t)((file_size * 8) / total_sec);
                } else {
                    s_state.bitrate = 0;
                }
                s_state.track_loaded = true;
                state_unlock();

                const char *display_format = (s_state.format_name[0] != '\0') ? s_state.format_name : label;
                ESP_LOGI(TAG, "%s: %u Hz, %u canal(is), %u bits, %s%u s",
                         display_format, (unsigned)sample_rate, (unsigned)num_channels,
                         (unsigned)s_state.bits_per_sample,
                         meta.duration_is_estimate ? "~" : "", (unsigned)total_sec);
                header_ready = true;
                break;
            }

            case mps3::DecodeStatus::Success: {
                if (!header_ready || samples_decoded == 0) break;

                elapsed_samples += samples_decoded / num_channels;
                uint32_t elapsed_sec = (sample_rate > 0)
                    ? (uint32_t)(elapsed_samples / sample_rate) : 0;

                bool seeking = s_seek_pending && elapsed_sec < s_seek_target_sec;
                if (seeking) {
                    state_lock();
                    s_state.elapsed_sec = elapsed_sec;
                    state_unlock();
                    s_last_elapsed_sec = elapsed_sec;
                    break;
                }
                if (s_seek_pending) {
                    s_seek_pending = false;
                    audio_dsp_flush();
                    audio_dsp_trigger_fade_in(sample_rate);
                    prebuffered_frames = 0;
                    prebuffer_done = false;
                }

                int32_t *to_write = output_buf;
                size_t samples_to_write = samples_decoded;

                if (num_channels == 1) {
                    int32_t *stereo = ensure_stereo_scratch(samples_decoded * 2);
                    if (stereo) {
                        for (size_t i = 0; i < samples_decoded; i++) {
                            stereo[2 * i]     = output_buf[i];
                            stereo[2 * i + 1] = output_buf[i];
                        }
                        to_write = stereo;
                        samples_to_write = samples_decoded * 2;
                    }
                }

                // Envia para o Core 0 (audio_dsp_task) para processar Volume/Balanço/EQ e gravar no I2S DMA
                if (samples_to_write > 0) {
                    audio_dsp_send_pcm(to_write, samples_to_write, sample_rate);
                }

                if (!prebuffer_done) {
                    prebuffered_frames += (samples_to_write / 2);
                    if (prebuffered_frames >= prebuffer_target_frames) {
                        prebuffer_done = true;
                    }
                }

                // So inicia o prefetch da proxima faixa e a contagem/reproducao livre
                // apos o pre-buffering da DMA (~100 ms) estar concluido.
                if (prebuffer_done) {
                    if (!prefetched_next) {
                        prefetched_next = true;
                        scan_lock();
                        int count_for_prefetch = s_playback_scan.audio_count;
                        scan_unlock();
                        if (count_for_prefetch > 1) {
                            int next_index = (index + 1) % count_for_prefetch;
                            prefetch_next_file(next_index);
                        }
                    }

                    state_lock();
                    s_state.elapsed_sec = elapsed_sec;
                    state_unlock();
                    s_last_elapsed_sec = elapsed_sec;

                    if (elapsed_sec >= last_saved_elapsed + 30) {
                        last_saved_elapsed = elapsed_sec;
                        char rel[PATH_LEN];
                        build_relative_path(s_playback_dir, display_name, rel, sizeof(rel));
                        save_resume_state(rel, elapsed_sec);
                    }
                }
                break;
            }

            case mps3::DecodeStatus::NeedMoreData:
                if (data_start > 0) {
                    // O decoder precisa de mais dados, mas o final do buffer ja alcancou INBUF_SIZE
                    // ou nao atingiu o limiar normal de compactacao. Compacta imediatamente os
                    // bytes restantes para a origem a fim de abrir espaco para fread() no proximo ciclo.
                    size_t remaining = valid_end - data_start;
                    memmove(inbuf, inbuf + data_start, remaining);
                    data_start = 0;
                    valid_end = remaining;
                } else if (valid_end == INBUF_SIZE) {
                    // Buffer de entrada cheio e o decoder nao conseguiu
                    // consumir nem 1 byte - com skip_leading_metadata()
                    // ja' pulando ID3v2/blocos FLAC antes do loop
                    // comecar, isso so' deveria acontecer com um arquivo
                    // genuinamente corrompido ou nao suportado. Desiste
                    // dessa faixa (sem tentar "ressincronizar" byte a
                    // byte - isso ja' causou um watchdog reset por ser
                    // absurdamente lento com o buffer grande).
                    ESP_LOGW(TAG, "Buffer de entrada cheio e decoder pede mais dados; abortando faixa.");
                    done = true;
                }
                break;

            case mps3::DecodeStatus::EndOfStream:
                done = true;
                break;

            default:
                ESP_LOGE(TAG, "Erro de decodificacao (codigo %d)", (int)result);
                done = true;
                break;
        }
    }

    audio_dsp_drain();

    if (output_buf) heap_caps_free(output_buf);
    if (pending_m4a_prefix) free(pending_m4a_prefix);
    if (use_frag_demux) m4a_frag_demuxer_free(&frag_demux);
    heap_caps_free(inbuf);
    fclose(fp);

    state_lock();
    s_state.playing = false;
    state_unlock();
}

static void player_task(void *arg)
{
    (void)arg;
    uint32_t resume_elapsed = s_boot_resume_elapsed;
    int current_index = s_boot_resume_index;
    bool first_track = true;
    bool usb_resume_pending = false;
    bool reselect_resume_pending = false;

    while (true) {
        if (s_usb_takeover_requested) {
            if (s_prefetch.fp) {
                fclose(s_prefetch.fp);
                s_prefetch.fp = nullptr;
                s_prefetch.index = -1;
            }
            s_usb_takeover_active = true;
            while (s_usb_takeover_requested) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            s_usb_takeover_active = false;
            usb_resume_pending = true;
            continue;
        }

        scan_lock();
        int audio_count_now = s_playback_scan.audio_count;
        scan_unlock();

        if (audio_count_now == 0) {
            state_lock();
            s_state.no_files_found = true;
            state_unlock();
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        s_current_index = current_index;
        state_lock();
        s_state.no_files_found = false; // havia audio, limpa a flag "travada" (bug corrigido)
        state_unlock();

        char current_name[260];
        char prev_playback_dir[DIR_LEN];
        scan_lock();
        strncpy(current_name, s_playback_scan.audio_files[current_index], sizeof(current_name) - 1);
        current_name[sizeof(current_name) - 1] = '\0';
        // Guarda de qual pasta a faixa que estamos PRESTES a tocar veio -
        // usado mais abaixo pra' detectar se um PLAYER_CMD_PLAY_INDEX
        // seguinte esta' re-selecionando essa MESMA faixa (ver
        // `reselected_same_track` - bug corrigido: reiniciava do zero).
        strncpy(prev_playback_dir, s_playback_dir, sizeof(prev_playback_dir) - 1);
        prev_playback_dir[sizeof(prev_playback_dir) - 1] = '\0';
        scan_unlock();

        char rel[PATH_LEN];
        build_relative_path(s_playback_dir, current_name, rel, sizeof(rel));
        uint32_t target_elapsed;
        if (first_track) {
            target_elapsed = resume_elapsed;
        } else if (usb_resume_pending) {
            target_elapsed = s_last_elapsed_sec;
        } else if (s_forced_restart_pending) {
            // audio_player_seek_backward() pediu pra' reiniciar a faixa E
            // ja' avancar rapido ate' essa posicao, nao comecar do zero.
            state_lock();
            target_elapsed = s_forced_restart_target_sec;
            s_forced_restart_pending = false;
            state_unlock();
        } else if (reselect_resume_pending) {
            // O usuario re-selecionou (na lista, ou voltando pra' tela de
            // reproducao) a MESMA faixa que ja' estava carregada/pausada -
            // ver deteccao logo depois do switch(cmd) abaixo. Sem isso a
            // faixa reiniciava do zero toda vez que voce pausava, saia
            // pra' navegar e voltava/re-clicava nela.
            target_elapsed = s_last_elapsed_sec;
            reselect_resume_pending = false;
        } else {
            target_elapsed = 0;
        }
        if (s_pending_cmd == PLAYER_CMD_PLAY_URL) {
            // Se chegou um comando pra tocar URL, extraimos ele da fila
            s_pending_cmd = PLAYER_CMD_NONE;
            char url[256];
            strncpy(url, s_pending_url, sizeof(url) - 1);
            url[sizeof(url)-1] = '\0';
            
            // Avisa o estado
            state_lock();
            s_state.is_web_radio = true;
            strncpy(s_state.web_radio_url, url, sizeof(s_state.web_radio_url) - 1);
            s_state.web_radio_url[sizeof(s_state.web_radio_url) - 1] = '\0';
            state_unlock();

            play_web_radio(url);

            state_lock();
            s_state.is_web_radio = false;
            s_state.web_radio_url[0] = '\0';
            state_unlock();
        } else if (s_pending_cmd != PLAYER_CMD_STOP_URL) {
            save_resume_state(rel, target_elapsed);
            play_track(current_index, current_name, target_elapsed);
            first_track = false;
            usb_resume_pending = false;
        } else {
            // Era um comando de STOP_URL, mas ja limpamos
            s_pending_cmd = PLAYER_CMD_NONE;
        }

        player_cmd_t cmd = s_pending_cmd;
        int pending_idx = s_pending_index;
        s_pending_cmd = PLAYER_CMD_NONE;

        if (cmd != PLAYER_CMD_NONE && s_paused) {
            audio_player_toggle_play_pause();
        }

        scan_lock();
        int count_for_switch = s_playback_scan.audio_count;
        switch (cmd) {
            case PLAYER_CMD_NEXT:
                current_index = (current_index + 1) % count_for_switch;
                state_lock();
                s_forced_restart_pending = false;
                state_unlock();
                break;
            case PLAYER_CMD_PREV:
                current_index = (current_index - 1 + count_for_switch) % count_for_switch;
                state_lock();
                s_forced_restart_pending = false;
                state_unlock();
                break;
            case PLAYER_CMD_RESTART:
                break; // mesmo indice: play_track vai tocar do zero
            case PLAYER_CMD_PLAY_INDEX:
                if (pending_idx >= 0 && pending_idx < count_for_switch) {
                    reselect_resume_pending = (pending_idx == current_index) &&
                                               (strcmp(s_playback_dir, prev_playback_dir) == 0);
                    current_index = pending_idx;
                }
                break;
            case PLAYER_CMD_PLAY_URL:
                s_pending_cmd = PLAYER_CMD_PLAY_URL; // Preserva para a proxima iteracao principal do laco
                break;
            case PLAYER_CMD_STOP_URL:
                s_pending_cmd = PLAYER_CMD_STOP_URL; // Preserva para a proxima iteracao principal do laco
                break;
            case PLAYER_CMD_NONE:
            default:
                current_index = (current_index + 1) % count_for_switch; // fim natural -> proxima
                break;
        }
        scan_unlock();
    }
}

esp_err_t audio_player_start(const char *music_dir)
{
    // Inicializa o equalizador â€” carrega a config salva na NVS (ganhos e
    // enable). Feito antes de criar as tasks pra que a primeira faixa ja'
    // saia com o EQ correto configurado.
    eq_init();

    // Registra os decodificadores do ESP Audio Codec (FLAC/MP3/WAV/AAC/M4A)
    // uma unica vez, antes de qualquer decode() (ver esp_codec_decoder_adapter.h).
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();

    // NVS ja' foi inicializado em app_main(). Carrega antes de iniciar as
    // tasks para que a primeira faixa ja' saia no volume salvo.
    load_volume();
    load_balance();
    load_sort_mode();

    strncpy(s_root_dir, music_dir, sizeof(s_root_dir) - 1);
    s_root_dir[sizeof(s_root_dir) - 1] = '\0';

    strncpy(s_browse_dir, s_root_dir, sizeof(s_browse_dir) - 1);
    s_browse_dir[sizeof(s_browse_dir) - 1] = '\0';
    strncpy(s_playback_dir, s_root_dir, sizeof(s_playback_dir) - 1);
    s_playback_dir[sizeof(s_playback_dir) - 1] = '\0';

    s_state_mutex = xSemaphoreCreateMutex();
    if (!s_state_mutex) {
        return ESP_ERR_NO_MEM;
    }
    s_scan_mutex = xSemaphoreCreateMutex();
    if (!s_scan_mutex) {
        return ESP_ERR_NO_MEM;
    }

    scan_lock();
    scan_dir(s_browse_dir, s_browse_scan);
    scan_dir(s_playback_dir, s_playback_scan);
    scan_unlock();

    // Carregado aqui (sincrono, antes de criar a task) - pode substituir
    // s_browse_dir/s_playback_dir por uma subpasta se houver uma sessao
    // anterior valida. Sincrono pra' audio_player_should_start_in_playing_mode()
    // ja' refletir o valor correto assim que audio_player_start() retorna.
    // (load_last_track_index() gerencia o scan_lock() por conta propria,
    // por isso ja' liberamos acima antes de chama-la.)
    s_boot_resume_index = load_last_track_index(&s_boot_resume_elapsed);

    BaseType_t volume_task_ok = xTaskCreate(volume_persistence_task, "volume_nvs",
                                             2048, NULL, 2, NULL);
    if (volume_task_ok != pdPASS) return ESP_ERR_NO_MEM;

    esp_err_t dsp_err = audio_dsp_init();
    if (dsp_err != ESP_OK) return dsp_err;

    ESP_LOGI(TAG, "Heap interno livre antes do player_task: %u bytes (maior bloco: %u bytes)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    BaseType_t ok = xTaskCreatePinnedToCore(player_task, "player_task",
                                             24576, NULL, 5, &s_player_task_handle, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar player_task no Core 1 (ok=%d, free_internal=%u, largest_block=%u)",
                 (int)ok,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

// --- Wrappers do equalizador (bridge entre audio_player.h e eq.h) --------
// Evitam que touch_input.c e oled_display.c precisem incluir eq.h
// diretamente (que e' C++ e causaria problemas de linkagem em arquivos .c).

void audio_player_get_eq_config(player_eq_config_t *out)
{
    if (!out) return;
    eq_config_t cfg;
    eq_get_config(&cfg);
    out->enabled = cfg.enabled;
    out->active_preset_idx = cfg.active_preset_idx;
    for (int p = 0; p < PLAYER_EQ_MAX_PRESETS; p++) {
        out->presets[p].overall_gain = cfg.presets[p].overall_gain;
        strncpy(out->presets[p].name, cfg.presets[p].name, PLAYER_EQ_PRESET_NAME_LEN);
        for (int i = 0; i < PLAYER_EQ_BANDS; i++) {
            out->presets[p].band_gains[i] = cfg.presets[p].band_gains[i];
        }
    }
}

void audio_player_set_eq_config(const player_eq_config_t *config)
{
    if (!config) return;
    eq_config_t cfg;
    cfg.enabled = config->enabled;
    cfg.active_preset_idx = config->active_preset_idx;
    for (int p = 0; p < PLAYER_EQ_MAX_PRESETS; p++) {
        cfg.presets[p].overall_gain = config->presets[p].overall_gain;
        strncpy(cfg.presets[p].name, config->presets[p].name, PLAYER_EQ_PRESET_NAME_LEN);
        for (int i = 0; i < PLAYER_EQ_BANDS; i++) {
            cfg.presets[p].band_gains[i] = config->presets[p].band_gains[i];
        }
    }
    eq_update_config(&cfg);
}
