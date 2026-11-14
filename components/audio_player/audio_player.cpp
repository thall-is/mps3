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
#include "audio_meter.h"
#include "user_metrics.h"
#include "sd_card.h"
#include "esp_timer.h"

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
// precisar caber no buffer. 256KB alocado em PSRAM garante margem folgada
// para leituras em bloco no SD Card a 40 MHz e previne starvation de decodificador.
#define INBUF_SIZE (256 * 1024)

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
#define NVS_KEY_GAPLESS "gapless"
#define NVS_KEY_SOFT_FADE "soft_fade"
#define NVS_KEY_RG_MODE "rg_mode"
#define NVS_KEY_BM_MODE "bm_mode"
#define NVS_KEY_AZ_JUMP "az_jump"
#define NVS_KEY_LOOP_MODE "loop_mode"
#define NVS_KEY_SHUF_MODE "shuf_mode" 

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
static void scan_lock(void)   { if (s_scan_mutex) xSemaphoreTake(s_scan_mutex, portMAX_DELAY); }
static void scan_unlock(void) { if (s_scan_mutex) xSemaphoreGive(s_scan_mutex); }

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

void state_lock(void)   { if (s_state_mutex) xSemaphoreTake(s_state_mutex, portMAX_DELAY); }
void state_unlock(void) { if (s_state_mutex) xSemaphoreGive(s_state_mutex); }

// Forward declarations - definidas mais abaixo, usadas antes por
// audio_player_toggle_play_pause().
static void save_resume_state(const char *relative_path, uint32_t elapsed_sec);
static void save_volume(int volume);
static void load_volume(void);
static void save_balance(int balance);
static void load_balance(void);
static void build_relative_path(const char *abs_dir, const char *filename, char *out, size_t out_len);
static void resolve_track_full_path(const char *dir, const char *entry_name, char *out, size_t out_len);
static const char *extract_track_basename(const char *entry_name);
static void invalidate_global_shuffle_deck(void);

static volatile bool s_gapless_enabled = true;
static volatile bool s_soft_fade_enabled = true;
static volatile replaygain_mode_t s_replaygain_mode = RG_MODE_OFF;
static volatile bookmark_mode_t s_bookmark_mode = BOOKMARK_MODE_AUTO;
static volatile bool s_az_jump_enabled = true;
static char s_target_playlist[64] = "Favoritos.m3u8";

static volatile bool s_gapless_transition_active = false;
static volatile bool s_dsp_pause_fade_out = false;
static volatile float s_current_rg_gain_linear = 1.0f;
static int s_rg_generation = 0;
static QueueHandle_t s_dsp_free_queue = NULL;
static QueueHandle_t s_dsp_ready_queue = NULL;
static volatile uint32_t s_dsp_current_rate = 44100;
static track_metadata_t s_current_track_meta = {};

static void update_active_replaygain_multiplier(const track_metadata_t *meta)
{
    float db = 0.0f;
    replaygain_mode_t mode = s_replaygain_mode;
    if (meta && mode != RG_MODE_OFF) {
        if (mode == RG_MODE_TRACK) {
            if (meta->has_replaygain_track) db = meta->replaygain_track_db;
            else if (meta->has_replaygain_album) db = meta->replaygain_album_db;
        } else if (mode == RG_MODE_ALBUM) {
            if (meta->has_replaygain_album) db = meta->replaygain_album_db;
            else if (meta->has_replaygain_track) db = meta->replaygain_track_db;
        }
    }
    if (db == 0.0f) {
        s_current_rg_gain_linear = 1.0f;
    } else {
        s_current_rg_gain_linear = powf(10.0f, db / 20.0f);
    }
    s_rg_generation++;
}

// --- Sistema de Multi-Bookmarks em PSRAM (/sdcard/.mps3_bookmarks.dat) ---
#define MAX_BOOKMARKS 64
#define BOOKMARKS_FILE_PATH "/sdcard/.mps3_bookmarks.dat"
#define BOOKMARKS_MAGIC 0x4D50424DUL // "MPBM"

struct BookmarkEntry {
    char rel_path[240];
    uint32_t elapsed_sec;
    uint32_t total_sec;
    uint32_t seq;
};

static BookmarkEntry *s_bookmarks = nullptr;
static uint32_t s_bookmark_seq = 1;

static void bookmarks_init(void)
{
    if (!s_bookmarks) {
        s_bookmarks = (BookmarkEntry *)heap_caps_calloc(MAX_BOOKMARKS, sizeof(BookmarkEntry),
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_bookmarks) {
            s_bookmarks = (BookmarkEntry *)calloc(MAX_BOOKMARKS, sizeof(BookmarkEntry));
        }
    }
    if (!s_bookmarks) return;

    FILE *fp = fopen(BOOKMARKS_FILE_PATH, "rb");
    if (!fp) return;
    uint32_t magic = 0, count = 0;
    if (fread(&magic, sizeof(uint32_t), 1, fp) == 1 && magic == BOOKMARKS_MAGIC &&
        fread(&count, sizeof(uint32_t), 1, fp) == 1) {
        if (count > MAX_BOOKMARKS) count = MAX_BOOKMARKS;
        size_t n = fread(s_bookmarks, sizeof(BookmarkEntry), count, fp);
        for (size_t i = 0; i < n; i++) {
            if (s_bookmarks[i].seq >= s_bookmark_seq) {
                s_bookmark_seq = s_bookmarks[i].seq + 1;
            }
        }
    }
    fclose(fp);
}

static void bookmarks_flush_to_sd(void)
{
    if (!s_bookmarks || s_usb_takeover_requested) return;
    FILE *fp = fopen(BOOKMARKS_FILE_PATH, "wb");
    if (!fp) return;
    uint32_t magic = BOOKMARKS_MAGIC;
    uint32_t count = MAX_BOOKMARKS;
    fwrite(&magic, sizeof(uint32_t), 1, fp);
    fwrite(&count, sizeof(uint32_t), 1, fp);
    fwrite(s_bookmarks, sizeof(BookmarkEntry), MAX_BOOKMARKS, fp);
    fclose(fp);
}

static bool should_bookmark_track(const char *rel_path, uint32_t total_sec)
{
    bookmark_mode_t mode = s_bookmark_mode;
    if (mode == BOOKMARK_MODE_OFF || !rel_path || rel_path[0] == '\0') return false;
    if (mode == BOOKMARK_MODE_ALL) return true;

    // BOOKMARK_MODE_AUTO: faixas >= 15 min (900s), extensao .m4b ou pastas de Podcast/Audiobook
    if (total_sec >= 900) return true;
    const char *ext = strrchr(rel_path, '.');
    if (ext && strcasecmp(ext, ".m4b") == 0) return true;
    if (strcasestr(rel_path, "podcast") != NULL ||
        strcasestr(rel_path, "audiobook") != NULL ||
        strcasestr(rel_path, "audiolivro") != NULL ||
        strcasestr(rel_path, "livro") != NULL ||
        strcasestr(rel_path, "aula") != NULL) {
        return true;
    }
    return false;
}

static uint32_t bookmarks_get(const char *rel_path)
{
    if (!s_bookmarks || !rel_path || rel_path[0] == '\0') return 0;
    for (int i = 0; i < MAX_BOOKMARKS; i++) {
        if (s_bookmarks[i].rel_path[0] != '\0' &&
            strcasecmp(s_bookmarks[i].rel_path, rel_path) == 0) {
            return s_bookmarks[i].elapsed_sec;
        }
    }
    return 0;
}

static void bookmarks_set(const char *rel_path, uint32_t elapsed_sec, uint32_t total_sec)
{
    if (!s_bookmarks || !rel_path || rel_path[0] == '\0') return;
    if (elapsed_sec < 5) return; // Ignora primeiros segundos

    int target_slot = -1;
    uint32_t oldest_seq = 0xFFFFFFFFUL;
    int oldest_slot = 0;

    for (int i = 0; i < MAX_BOOKMARKS; i++) {
        if (s_bookmarks[i].rel_path[0] != '\0' &&
            strcasecmp(s_bookmarks[i].rel_path, rel_path) == 0) {
            target_slot = i;
            break;
        }
        if (s_bookmarks[i].rel_path[0] == '\0' && target_slot < 0) {
            target_slot = i;
        }
        if (s_bookmarks[i].seq < oldest_seq) {
            oldest_seq = s_bookmarks[i].seq;
            oldest_slot = i;
        }
    }
    if (target_slot < 0) target_slot = oldest_slot;

    strlcpy(s_bookmarks[target_slot].rel_path, rel_path, sizeof(s_bookmarks[target_slot].rel_path));
    s_bookmarks[target_slot].elapsed_sec = elapsed_sec;
    s_bookmarks[target_slot].total_sec = total_sec;
    s_bookmarks[target_slot].seq = s_bookmark_seq++;
    bookmarks_flush_to_sd();
}

static void bookmarks_remove(const char *rel_path)
{
    if (!s_bookmarks || !rel_path || rel_path[0] == '\0') return;
    for (int i = 0; i < MAX_BOOKMARKS; i++) {
        if (s_bookmarks[i].rel_path[0] != '\0' &&
            strcasecmp(s_bookmarks[i].rel_path, rel_path) == 0) {
            memset(&s_bookmarks[i], 0, sizeof(BookmarkEntry));
            bookmarks_flush_to_sd();
            return;
        }
    }
}

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
        if (s_soft_fade_enabled && s_dsp_ready_queue && uxQueueMessagesWaiting(s_dsp_ready_queue) > 0) {
            s_dsp_pause_fade_out = true;
            for (int w = 0; w < 4 && s_dsp_pause_fade_out; w++) {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        }
        s_dsp_pause_fade_out = false;
        s_paused = true;
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
        if (sd_card_get_frequency() != SDMMC_FREQ_HIGHSPEED) {
            sd_card_set_frequency(SDMMC_FREQ_HIGHSPEED);
        }
        if (s_soft_fade_enabled) {
            audio_dsp_trigger_fade_in(s_dsp_current_rate);
        }
        s_paused = false;
    }
    ESP_LOGI(TAG, "%s", s_paused ? "Pausado" : "Tocando");
}

static audio_volume_change_cb_t s_volume_change_cb = NULL;
static audio_track_completed_cb_t s_track_completed_cb = NULL;

void audio_player_set_volume_change_cb(audio_volume_change_cb_t cb)
{
    s_volume_change_cb = cb;
}

void audio_player_register_track_completed_cb(audio_track_completed_cb_t cb)
{
    s_track_completed_cb = cb;
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
#define DSP_BLOCK_FRAMES  1024 // 1024 quadros stereo = 2048 amostras int32_t (8192 bytes)
#define DSP_BLOCK_SAMPLES (DSP_BLOCK_FRAMES * 2)
#define DSP_NUM_BLOCKS    96   // 96 blocos x 1024 quadros (~512 ms a 192 kHz e ~2.2 s a 44.1 kHz) previnem underruns de I2S DMA
#define DSP_PRIME_WATERMARK 24 // 24 blocos (~128 ms a 192 kHz) de pre-buffer minimo antes de iniciar DMA

typedef struct {
    int32_t samples[DSP_BLOCK_SAMPLES];
    size_t  sample_count; // Amostras int32_t validas (sempre par)
    uint32_t sample_rate; // Taxa de amostragem deste bloco
    bool    is_flush;     // Solicitacao de reset/limpeza de filtros biquad
} dsp_block_t;

static TaskHandle_t s_audio_dsp_task_handle = NULL;
static dsp_block_t *s_dsp_blocks[DSP_NUM_BLOCKS] = {NULL};
static size_t s_dsp_fade_in_total = 0;
static size_t s_dsp_fade_in_remaining = 0;
static volatile bool s_dsp_primed = false;
static volatile bool s_dsp_stream_ended = false;

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
    s_dsp_primed = false;
    s_dsp_stream_ended = false;
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
    static int s_cached_rg_gen = -1;
    static int32_t s_cached_final_L = 32768;
    static int32_t s_cached_final_R = 32768;

    while (true) {
        if (s_usb_takeover_requested) {
            s_dsp_primed = false;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (s_paused) {
            s_dsp_primed = false;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (!s_dsp_primed) {
            if (uxQueueMessagesWaiting(s_dsp_ready_queue) >= DSP_PRIME_WATERMARK || s_dsp_stream_ended) {
                s_dsp_primed = true;
            } else {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
        }

        dsp_block_t *blk = NULL;
        if (xQueueReceive(s_dsp_ready_queue, &blk, pdMS_TO_TICKS(10)) != pdTRUE) {
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

            // Volume, Balanço e ReplayGain
            int vol = s_volume_percent;
            int bal = s_balance;
            int rg_gen = s_rg_generation;
            if (vol != s_cached_vol || bal != s_cached_bal || rg_gen != s_cached_rg_gen) {
                s_cached_vol = vol;
                s_cached_bal = bal;
                s_cached_rg_gen = rg_gen;

                float vol_gain = volume_to_gain(vol) * s_current_rg_gain_linear;
                int32_t vol_mult = (int32_t)(vol_gain * 32768.0f + 0.5f);
                if (vol_mult > 65536) vol_mult = 65536;
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

            const bool meter_pre_vol = (audio_meter_get_mode() == AUDIO_METER_MODE_PRE_VOLUME);
            if (meter_pre_vol) {
                audio_meter_feed(to_write, samples_to_write, blk->sample_rate);
            }

            // Fade-out suave anti-click (Pause) ou Fade-in suave de transição
            if (s_dsp_pause_fade_out) {
                size_t total_pairs = samples_to_write / 2;
                if (total_pairs == 0) total_pairs = 1;
                for (size_t i = 0; i < samples_to_write; i += 2) {
                    size_t pair_idx = i / 2;
                    float fade = 1.0f - ((float)pair_idx / (float)total_pairs);
                    int32_t fade_q15 = (int32_t)(fade * 32768.0f + 0.5f);
                    int32_t f_L = (int32_t)(((int64_t)final_L * fade_q15) >> 15);
                    int32_t f_R = (int32_t)(((int64_t)final_R * fade_q15) >> 15);
                    to_write[i]     = (int32_t)(((int64_t)to_write[i]     * f_L) >> 15);
                    if (i + 1 < samples_to_write) {
                        to_write[i + 1] = (int32_t)(((int64_t)to_write[i + 1] * f_R) >> 15);
                    }
                }
                eq_process(to_write, samples_to_write);
                s_dsp_pause_fade_out = false;
            } else if (s_dsp_fade_in_remaining > 0) {
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
                eq_process(to_write, samples_to_write);
            } else {
                const float gain_l = (float)final_L * (1.0f / 32768.0f);
                const float gain_r = (float)final_R * (1.0f / 32768.0f);
                // Se o EQ estiver ativo, eq_process_with_gain ja aplica gain_l/gain_r sem custo extra
                if (!eq_process_with_gain(to_write, samples_to_write, gain_l, gain_r)) {
                    if (final_L != 32768 || final_R != 32768) {
                        if (final_L <= 32768 && final_R <= 32768) {
                            for (size_t i = 0; i < samples_to_write; i += 2) {
                                to_write[i]     = (int32_t)(((int64_t)to_write[i]     * final_L) >> 15);
                                if (i + 1 < samples_to_write) {
                                    to_write[i + 1] = (int32_t)(((int64_t)to_write[i + 1] * final_R) >> 15);
                                }
                            }
                        } else {
                            for (size_t i = 0; i < samples_to_write; i += 2) {
                                int64_t vL = ((int64_t)to_write[i] * final_L) >> 15;
                                if (vL > 2147483647LL) vL = 2147483647LL;
                                else if (vL < -2147483648LL) vL = -2147483648LL;
                                to_write[i] = (int32_t)vL;
                                if (i + 1 < samples_to_write) {
                                    int64_t vR = ((int64_t)to_write[i + 1] * final_R) >> 15;
                                    if (vR > 2147483647LL) vR = 2147483647LL;
                                    else if (vR < -2147483648LL) vR = -2147483648LL;
                                    to_write[i + 1] = (int32_t)vR;
                                }
                            }
                        }
                    }
                }
            }

            // Alimentação do medidor de intensidade sonora (VU Meter pós-volume/EQ)
            if (!meter_pre_vol) {
                audio_meter_feed(to_write, samples_to_write, blk->sample_rate);
            }

            // Transmissão direta aos descritores DMA do I2S
            size_t written = 0;
            i2s_output_write(to_write, samples_to_write, &written);
        }

        xQueueSend(s_dsp_free_queue, &blk, portMAX_DELAY);
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
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_dsp_blocks[i]) {
            s_dsp_blocks[i] = (dsp_block_t *)heap_caps_calloc(1, sizeof(dsp_block_t),
                                                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (s_dsp_blocks[i]) internal_count++;
        }
        if (!s_dsp_blocks[i]) {
            s_dsp_blocks[i] = (dsp_block_t *)calloc(1, sizeof(dsp_block_t));
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

// --- Pipeline Assincrono de I/O / Double-Buffering (Core 0 -> Core 1) ---
// Desacopla as chamadas de I/O do SD Card da CPU de decodificacao (Core 1).
// Core 0 executa o pre-fetch via SDMMC GDMA em segundo plano (prioridade 4),
// enquanto o Core 1 decodifica FLAC/WAV/etc. continuamente em tempo real.
#define ASYNC_IO_RING_SIZE (1024 * 1024) // 1024 KB no Octal PSRAM (~333 ms a 384k/32b, ~1 s a 192k/24b)
#define ASYNC_IO_READ_CHUNK (64 * 1024)  // 64 KB por rajada SDMMC, alinhado a clusters

struct AsyncIoReader {
    uint8_t *buf = nullptr;
    size_t capacity = 0;
    volatile size_t head = 0;
    volatile size_t tail = 0;
    volatile size_t count = 0;
    volatile bool active = false;
    volatile bool eof = false;
    volatile bool in_read = false;
    volatile bool abort_req = false;
    FILE *fp = nullptr;
    SemaphoreHandle_t mutex = nullptr;
    SemaphoreHandle_t sem_space = nullptr;
    SemaphoreHandle_t sem_data = nullptr;
    TaskHandle_t task_handle = nullptr;
};

static AsyncIoReader s_async_reader;

static void audio_reader_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "audio_reader_task INICIADA no Core %d (prio %d)",
             xPortGetCoreID(), (int)uxTaskPriorityGet(NULL));

    while (true) {
        // Se inativo, sem arquivo, abortando ou ja em EOF, aguarda notificacao
        if (!s_async_reader.active || !s_async_reader.fp || s_async_reader.abort_req || s_async_reader.eof) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            continue;
        }

        // Verifica espaco no ring buffer
        xSemaphoreTake(s_async_reader.mutex, portMAX_DELAY);
        size_t free_space = s_async_reader.capacity - s_async_reader.count;
        size_t head = s_async_reader.head;
        FILE *fp = s_async_reader.fp;
        bool abort = s_async_reader.abort_req;
        bool active = s_async_reader.active;
        xSemaphoreGive(s_async_reader.mutex);

        if (abort || !active || !fp) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (free_space < ASYNC_IO_READ_CHUNK) {
            // Ring buffer cheio - dorme ate o player_task consumir dados
            xSemaphoreTake(s_async_reader.sem_space, pdMS_TO_TICKS(50));
            continue;
        }

        // Calcula leitura contigua ate o fim do anel circular
        size_t contiguous = s_async_reader.capacity - head;
        size_t to_read = (contiguous < ASYNC_IO_READ_CHUNK) ? contiguous : ASYNC_IO_READ_CHUNK;
        if (to_read > free_space) to_read = free_space;

        s_async_reader.in_read = true;
        size_t n = fread(s_async_reader.buf + head, 1, to_read, fp);
        s_async_reader.in_read = false;

        xSemaphoreTake(s_async_reader.mutex, portMAX_DELAY);
        if (s_async_reader.abort_req || !s_async_reader.active || s_async_reader.fp != fp) {
            xSemaphoreGive(s_async_reader.mutex);
            continue;
        }

        if (n > 0) {
            s_async_reader.head = (head + n) % s_async_reader.capacity;
            s_async_reader.count += n;
            size_t current_count = s_async_reader.count;
            xSemaphoreGive(s_async_reader.mutex);
            xSemaphoreGive(s_async_reader.sem_data);

            // Cede a CPU brevemente se ja tivermos margem abundante no buffer (> 256 KB),
            // garantindo que a display_task (prio 3) e o pipeline de UI no Core 0 mantenham
            // seu framerate fluido (~60 FPS) sem congelamentos ou starvation.
            if (current_count >= (256 * 1024)) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        } else {
            // EOF ou erro de leitura
            s_async_reader.eof = true;
            xSemaphoreGive(s_async_reader.mutex);
            xSemaphoreGive(s_async_reader.sem_data);
        }
    }
}

static esp_err_t async_reader_init(void)
{
    if (s_async_reader.buf != nullptr) return ESP_OK;

    s_async_reader.capacity = ASYNC_IO_RING_SIZE;
    s_async_reader.buf = (uint8_t *)heap_caps_malloc(s_async_reader.capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_async_reader.buf) {
        s_async_reader.capacity = 256 * 1024;
        s_async_reader.buf = (uint8_t *)heap_caps_malloc(s_async_reader.capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_async_reader.buf) {
        ESP_LOGE(TAG, "Falha ao alocar ring buffer assincrono em PSRAM");
        return ESP_ERR_NO_MEM;
    }

    s_async_reader.mutex = xSemaphoreCreateMutex();
    s_async_reader.sem_space = xSemaphoreCreateBinary();
    s_async_reader.sem_data = xSemaphoreCreateBinary();

    BaseType_t ret = xTaskCreatePinnedToCore(audio_reader_task, "audio_reader",
                                             6144, NULL, 4, &s_async_reader.task_handle, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar audio_reader_task no Core 0");
        heap_caps_free(s_async_reader.buf);
        s_async_reader.buf = nullptr;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Pipeline assincrono de I/O ativado: Ring=%u KB em PSRAM (Core 0, prio 4)",
             (unsigned)(s_async_reader.capacity / 1024U));
    return ESP_OK;
}

static void async_reader_start(FILE *fp)
{
    if (!s_async_reader.buf || !fp) return;

    xSemaphoreTake(s_async_reader.mutex, portMAX_DELAY);
    // Drena tokens residuais de semaforos/notificacoes de faixas/seeks anteriores
    while (xSemaphoreTake(s_async_reader.sem_data, 0) == pdTRUE) {}
    while (xSemaphoreTake(s_async_reader.sem_space, 0) == pdTRUE) {}
    if (s_async_reader.task_handle) {
        ulTaskNotifyTake(pdTRUE, 0);
    }
    s_async_reader.fp = fp;
    s_async_reader.head = 0;
    s_async_reader.tail = 0;
    s_async_reader.count = 0;
    s_async_reader.eof = false;
    s_async_reader.abort_req = false;
    s_async_reader.active = true;
    xSemaphoreGive(s_async_reader.mutex);

    if (s_async_reader.task_handle) {
        xTaskNotifyGive(s_async_reader.task_handle);
    }
}

static void async_reader_stop(void)
{
    if (!s_async_reader.buf) return;

    xSemaphoreTake(s_async_reader.mutex, portMAX_DELAY);
    s_async_reader.abort_req = true;
    s_async_reader.active = false;
    xSemaphoreGive(s_async_reader.mutex);

    xSemaphoreGive(s_async_reader.sem_space);
    if (s_async_reader.task_handle) {
        xTaskNotifyGive(s_async_reader.task_handle);
    }

    // Aguarda com seguranca (ate 2 s) qualquer leitura fread() ativa terminar
    for (int i = 0; i < 200 && s_async_reader.in_read; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    xSemaphoreTake(s_async_reader.mutex, portMAX_DELAY);
    s_async_reader.fp = nullptr;
    s_async_reader.head = 0;
    s_async_reader.tail = 0;
    s_async_reader.count = 0;
    s_async_reader.eof = false;
    s_async_reader.abort_req = false;
    while (xSemaphoreTake(s_async_reader.sem_data, 0) == pdTRUE) {}
    while (xSemaphoreTake(s_async_reader.sem_space, 0) == pdTRUE) {}
    xSemaphoreGive(s_async_reader.mutex);
}

static size_t async_reader_read(uint8_t *dst, size_t max_bytes, uint32_t timeout_ms)
{
    if (!dst || max_bytes == 0 || !s_async_reader.buf) return 0;

    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000ULL;
    size_t total_copied = 0;

    while (total_copied < max_bytes) {
        if (s_usb_takeover_requested || s_pending_cmd != PLAYER_CMD_NONE) {
            break;
        }

        xSemaphoreTake(s_async_reader.mutex, portMAX_DELAY);
        size_t avail = s_async_reader.count;
        size_t tail = s_async_reader.tail;
        bool is_eof = s_async_reader.eof;
        bool is_active = s_async_reader.active;

        if (!is_active || (avail == 0 && is_eof)) {
            xSemaphoreGive(s_async_reader.mutex);
            break;
        }

        if (avail == 0) {
            xSemaphoreGive(s_async_reader.mutex);
            if (total_copied > 0) {
                // Ja transferimos bytes validos: retorna imediatamente para decodificacao paralela
                break;
            }
            int64_t now = esp_timer_get_time();
            if (now >= deadline) break;
            TickType_t wait_ticks = pdMS_TO_TICKS((deadline - now) / 1000ULL);
            if (wait_ticks == 0) wait_ticks = 1;
            xSemaphoreTake(s_async_reader.sem_data, wait_ticks);
            continue;
        }

        size_t want = max_bytes - total_copied;
        size_t to_copy = (want < avail) ? want : avail;
        size_t contiguous = s_async_reader.capacity - tail;
        size_t chunk1 = (to_copy < contiguous) ? to_copy : contiguous;
        size_t chunk2 = to_copy - chunk1;

        // Copia dados sob protecao do mutex para eliminar condicao de corrida com fread() no Core 0
        memcpy(dst + total_copied, s_async_reader.buf + tail, chunk1);
        if (chunk2 > 0) {
            memcpy(dst + total_copied + chunk1, s_async_reader.buf, chunk2);
        }
        total_copied += to_copy;

        // Atualiza ponteiros do anel circular somente apos a copia concluida
        s_async_reader.tail = (tail + to_copy) % s_async_reader.capacity;
        s_async_reader.count -= to_copy;
        size_t free_after = s_async_reader.capacity - s_async_reader.count;
        xSemaphoreGive(s_async_reader.mutex);

        // Desperta a thread leitora apenas se o espaco livre acomodar pelo menos uma rajada SDMMC
        if (free_after >= ASYNC_IO_READ_CHUNK) {
            xSemaphoreGive(s_async_reader.sem_space);
            if (s_async_reader.task_handle) {
                xTaskNotifyGive(s_async_reader.task_handle);
            }
        }
    }

    return total_copied;
}

static bool async_reader_is_eof(void)
{
    if (!s_async_reader.buf) return true;
    xSemaphoreTake(s_async_reader.mutex, portMAX_DELAY);
    bool is_eof = s_async_reader.eof && (s_async_reader.count == 0);
    xSemaphoreGive(s_async_reader.mutex);
    return is_eof;
}

static size_t async_reader_available(void)
{
    if (!s_async_reader.buf) return 0;
    xSemaphoreTake(s_async_reader.mutex, portMAX_DELAY);
    size_t cnt = s_async_reader.count;
    xSemaphoreGive(s_async_reader.mutex);
    return cnt;
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

bool audio_player_is_seeking(void)
{
    return s_forced_restart_pending;
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
        snprintf(out, out_len, "%s", extract_track_basename(s_playback_scan.audio_files[index]));
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

bool audio_player_browse_is_playback_dir(void)
{
    scan_lock();
    bool match = (strcmp(s_browse_dir, s_playback_dir) == 0);
    scan_unlock();
    return match;
}

int audio_player_get_current_track_browse_index(void)
{
    scan_lock();
    int idx = -1;
    if (strcmp(s_browse_dir, s_playback_dir) == 0 && s_playback_scan.audio_count > 0) {
        if (s_current_index >= 0 && s_current_index < s_playback_scan.audio_count) {
            idx = s_browse_scan.subdir_count + s_current_index;
        }
    }
    scan_unlock();
    return idx;
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
        if (is_playlist_extension(s_browse_scan.subdirs[i])) {
            snprintf(out, out_len, "[PL] %s", s_browse_scan.subdirs[i]);
        } else {
            snprintf(out, out_len, "%s/", s_browse_scan.subdirs[i]);
        }
        scan_unlock();
        return;
    }
    i -= s_browse_scan.subdir_count;
    if (i >= 0 && i < s_browse_scan.audio_count) {
        snprintf(out, out_len, "%s", extract_track_basename(s_browse_scan.audio_files[i]));
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
    invalidate_global_shuffle_deck();
    scan_lock();
    scan_dir(s_browse_dir, s_browse_scan);
    scan_dir(s_playback_dir, s_playback_scan);
    if (s_current_index >= s_playback_scan.audio_count) s_current_index = 0;
    scan_unlock();
    i2s_output_enable();
    s_dsp_current_rate = 0; // Força reconfiguração de taxa no próximo bloco de áudio
    s_usb_takeover_requested = false; // libera o player_task pra continuar
}

void audio_player_suspend(void)
{
    if (s_async_reader.task_handle != NULL) {
        ESP_LOGI("audio_player", "Suspendendo audio_reader_task para modo exclusivo USB");
        vTaskSuspend(s_async_reader.task_handle);
    }
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
    if (s_async_reader.task_handle != NULL) {
        ESP_LOGI("audio_player", "Retomando audio_reader_task apos modo exclusivo USB");
        vTaskResume(s_async_reader.task_handle);
    }
}

// --- Persistencia da ultima faixa/posicao tocada (NVS) -------------------
// Guardamos o caminho RELATIVO a' raiz (ex: "Rock/Album1/faixa.mp3", ou so'
// "faixa.mp3" se estiver na raiz) - assim, com pastas, ainda achamos a
// musica certa mesmo que o conteudo do cartao mude entre uma ligada e
// outra. A posicao (elapsed_sec) e' salva periodicamente durante a
// reproducao e ao pausar.
static bool s_has_resume = false;

static void resolve_track_full_path(const char *dir, const char *entry_name, char *out, size_t out_len)
{
    if (!entry_name || !out || out_len == 0) return;
    if (entry_name[0] == '/') {
        strlcpy(out, entry_name, out_len);
    } else {
        snprintf(out, out_len, "%s/%s", dir, entry_name);
    }
}

static const char *extract_track_basename(const char *entry_name)
{
    if (!entry_name) return "";
    const char *slash = strrchr(entry_name, '/');
    return slash ? (slash + 1) : entry_name;
}

static void build_relative_path(const char *abs_dir, const char *filename, char *out, size_t out_len)
{
    size_t root_len = strlen(s_root_dir);
    if (filename && filename[0] == '/') {
        if (strncmp(filename, s_root_dir, root_len) == 0) {
            const char *rel_f = filename + root_len;
            if (rel_f[0] == '/') rel_f++;
            snprintf(out, out_len, "%s", rel_f);
        } else {
            snprintf(out, out_len, "%s", filename + 1);
        }
        return;
    }
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
        strlcpy(file_out, relative, file_out_len);
    } else {
        size_t dir_len = (size_t)(last_slash - relative);
        if (dir_len >= dir_out_len) dir_len = dir_out_len - 1;
        memcpy(dir_out, relative, dir_len);
        dir_out[dir_len] = '\0';
        strlcpy(file_out, last_slash + 1, file_out_len);
    }
}

static void save_resume_state(const char *relative_path, uint32_t elapsed_sec)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, NVS_KEY_LAST_PATH, relative_path);
        nvs_set_u32(h, NVS_KEY_LAST_ELAPSED, elapsed_sec);
        nvs_commit(h);
        nvs_close(h);
    }
    if (should_bookmark_track(relative_path, s_state.total_sec)) {
        bookmarks_set(relative_path, elapsed_sec, s_state.total_sec);
    }
}

void audio_player_clear_resume_state(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY_LAST_PATH);
        nvs_erase_key(h, NVS_KEY_LAST_ELAPSED);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Registro de resume NVS limpo manualmente.");
    }
}

void audio_player_set_last_error(const char *filename, const char *error_msg)
{
    state_lock();
    if (filename != nullptr) {
        strncpy(s_state.filename, filename, sizeof(s_state.filename) - 1);
        s_state.filename[sizeof(s_state.filename) - 1] = '\0';
    }
    if (error_msg != nullptr && error_msg[0] != '\0') {
        strncpy(s_state.last_error, error_msg, sizeof(s_state.last_error) - 1);
        s_state.last_error[sizeof(s_state.last_error) - 1] = '\0';
        s_state.playing = false;
        s_state.track_loaded = false;
    } else {
        s_state.last_error[0] = '\0';
    }
    state_unlock();
}

static void clear_resume_state_if_matches(const char *relative_path)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        char saved[PATH_LEN] = {0};
        size_t len = sizeof(saved);
        if (nvs_get_str(h, NVS_KEY_LAST_PATH, saved, &len) == ESP_OK) {
            if (relative_path == nullptr || strcmp(saved, relative_path) == 0) {
                nvs_erase_key(h, NVS_KEY_LAST_PATH);
                nvs_erase_key(h, NVS_KEY_LAST_ELAPSED);
                nvs_commit(h);
                ESP_LOGI(TAG, "Registro de resume NVS removido para faixa problematica: %s", saved);
            }
        }
        nvs_close(h);
    }
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

// --- Modos de Reproducao: Loop e Shuffle (Fisher-Yates em PSRAM) ---
static audio_player_loop_mode_t s_loop_mode = AUDIO_LOOP_ALL; // Padrao de fabrica: Repete Tudo
static uint8_t s_loop_track_remaining = 0; // 1 = primeira execucao (icone com 1), 0 = repetindo (icone simples sem 1)
static audio_player_shuffle_mode_t s_shuffle_mode = AUDIO_SHUFFLE_OFF;

static uint16_t *s_shuffle_indices = NULL;
static int s_shuffle_count = 0;
static int s_shuffle_pos = 0;

#define SHUFFLE_HIST_MAX 32
typedef struct {
    char dir[192];
    int index;
} shuffle_history_entry_t;

static shuffle_history_entry_t *s_shuffle_history = NULL;
static int s_shuffle_hist_count = 0;
static int s_shuffle_hist_idx = 0;

static void ensure_shuffle_history_alloc(void)
{
    if (!s_shuffle_history) {
        s_shuffle_history = (shuffle_history_entry_t *)heap_caps_calloc(
            SHUFFLE_HIST_MAX, sizeof(shuffle_history_entry_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_shuffle_history) {
            s_shuffle_history = (shuffle_history_entry_t *)calloc(
                SHUFFLE_HIST_MAX, sizeof(shuffle_history_entry_t));
        }
    }
}

static void push_shuffle_history(const char *dir, int index)
{
    if (!dir || index < 0) return;
    ensure_shuffle_history_alloc();
    if (!s_shuffle_history) return;
    // Evita empilhar duplicatas consecutivas (ex.: durante seek/restart ou ao voltar com PREV)
    if (s_shuffle_hist_count > 0 &&
        s_shuffle_history[s_shuffle_hist_idx].index == index &&
        strcmp(s_shuffle_history[s_shuffle_hist_idx].dir, dir) == 0) {
        return;
    }
    int next_idx = (s_shuffle_hist_idx + 1) % SHUFFLE_HIST_MAX;
    strlcpy(s_shuffle_history[next_idx].dir, dir, sizeof(s_shuffle_history[next_idx].dir));
    s_shuffle_history[next_idx].index = index;
    s_shuffle_hist_idx = next_idx;
    if (s_shuffle_hist_count < SHUFFLE_HIST_MAX) s_shuffle_hist_count++;
}

static bool pop_shuffle_history(char *out_dir, size_t dir_len, int *out_index)
{
    if (!s_shuffle_history || s_shuffle_hist_count <= 1) return false;
    s_shuffle_hist_count--;
    s_shuffle_hist_idx = (s_shuffle_hist_idx - 1 + SHUFFLE_HIST_MAX) % SHUFFLE_HIST_MAX;
    if (out_dir) strlcpy(out_dir, s_shuffle_history[s_shuffle_hist_idx].dir, dir_len);
    if (out_index) *out_index = s_shuffle_history[s_shuffle_hist_idx].index;
    return true;
}

static void update_folder_shuffle(int count, int current_idx)
{
    if (s_shuffle_indices) {
        heap_caps_free(s_shuffle_indices);
        s_shuffle_indices = NULL;
    }
    s_shuffle_count = 0;
    s_shuffle_pos = 0;
    if (count <= 0) return;

    // Aloca vetor de indices na PSRAM Octal para economia absoluta de DRAM interna
    s_shuffle_indices = (uint16_t *)heap_caps_malloc(count * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_shuffle_indices) {
        s_shuffle_indices = (uint16_t *)malloc(count * sizeof(uint16_t));
        if (!s_shuffle_indices) return;
    }
    s_shuffle_count = count;

    for (int i = 0; i < count; i++) {
        s_shuffle_indices[i] = (uint16_t)i;
    }

    // Algoritmo Fisher-Yates de permutacao uniforme
    for (int i = count - 1; i > 0; i--) {
        int j = esp_random() % (i + 1);
        uint16_t temp = s_shuffle_indices[i];
        s_shuffle_indices[i] = s_shuffle_indices[j];
        s_shuffle_indices[j] = temp;
    }

    // Se houver faixa em reproducao, coloca ela no inicio do vetor para continuidade
    if (current_idx >= 0 && current_idx < count) {
        for (int i = 0; i < count; i++) {
            if (s_shuffle_indices[i] == (uint16_t)current_idx) {
                s_shuffle_indices[i] = s_shuffle_indices[0];
                s_shuffle_indices[0] = (uint16_t)current_idx;
                break;
            }
        }
    }
    s_shuffle_pos = 0;
}

static bool is_excluded_shuffle_dir(const char *name)
{
    if (!name || name[0] == '\0') return true;
    if (name[0] == '.') return true; // .mps3_analytics, etc.
    if (strcasecmp(name, "System Volume Information") == 0) return true;
    if (strcasecmp(name, "Podcasts") == 0) return true;
    if (strcasecmp(name, "playlists") == 0) return true;
    if (strcasecmp(name, "Android") == 0) return true;
    if (strcasecmp(name, "LOST.DIR") == 0) return true;
    return false;
}

// --- Deck Global de Shuffle Uniforme em PSRAM Octal (AUDIO_SHUFFLE_ALL) ---
// Corrige o vies estrutural do antigo passeio aleatorio em arvore (onde uma
// pasta com 1 unica faixa tinha a mesma chance 1/N de uma discografia com
// 60 faixas, alem do vies de 60% de parada precoce e repeticao sem memoria).
// Agora cada faixa individual da biblioteca tem probabilidade rigorosamente
// uniforme (1 / Total_Faixas) num baralho Fisher-Yates sem reposicao +
// desagrupamento de pastas consecutivas (anti-cluster).
#define MAX_GLOBAL_SHUF_DIRS   512
#define MAX_GLOBAL_SHUF_TRACKS 4096

struct GlobalShuffleDir {
    char path[256];
    uint16_t count;
};

struct GlobalShuffleTrack {
    uint16_t dir_idx;
    uint16_t track_idx;
};

static GlobalShuffleDir *s_global_shuf_dirs = nullptr;
static GlobalShuffleTrack *s_global_shuf_deck = nullptr;
static int s_global_shuf_dir_count = 0;
static int s_global_shuf_track_count = 0;
static int s_global_shuf_pos = 0;
static bool s_global_shuf_ready = false;

static void invalidate_global_shuffle_deck(void)
{
    s_global_shuf_ready = false;
    s_global_shuf_pos = 0;
}

static void permute_global_shuffle_deck(void)
{
    if (!s_global_shuf_deck || s_global_shuf_track_count <= 1) {
        s_global_shuf_pos = 0;
        return;
    }

    int n = s_global_shuf_track_count;
    // 1. Permutacao uniforme de Fisher-Yates usando RNG de hardware (esp_random)
    for (int i = n - 1; i > 0; i--) {
        int j = esp_random() % (i + 1);
        GlobalShuffleTrack tmp = s_global_shuf_deck[i];
        s_global_shuf_deck[i] = s_global_shuf_deck[j];
        s_global_shuf_deck[j] = tmp;
    }

    // 2. Passe de desagrupamento (Anti-Clustering): evita tocar duas faixas
    // seguidas da mesma pasta/album quando houver outras pastas disponiveis,
    // preservando 100% das faixas no baralho sem alterar a equiprobabilidade global.
    if (s_global_shuf_dir_count > 1) {
        for (int i = 1; i < n - 1; i++) {
            if (s_global_shuf_deck[i].dir_idx == s_global_shuf_deck[i - 1].dir_idx) {
                int remaining = n - (i + 1);
                int tries = (remaining < 12) ? remaining : 12;
                for (int t = 0; t < tries; t++) {
                    int k = (i + 1) + (esp_random() % remaining);
                    if (s_global_shuf_deck[k].dir_idx != s_global_shuf_deck[i - 1].dir_idx) {
                        GlobalShuffleTrack tmp = s_global_shuf_deck[i];
                        s_global_shuf_deck[i] = s_global_shuf_deck[k];
                        s_global_shuf_deck[k] = tmp;
                        break;
                    }
                }
            }
        }
    }

    s_global_shuf_pos = 0;
}

static bool build_global_shuffle_deck(void)
{
    if (!s_global_shuf_dirs) {
        s_global_shuf_dirs = (GlobalShuffleDir *)heap_caps_malloc(
            MAX_GLOBAL_SHUF_DIRS * sizeof(GlobalShuffleDir), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_global_shuf_deck) {
        s_global_shuf_deck = (GlobalShuffleTrack *)heap_caps_malloc(
            MAX_GLOBAL_SHUF_TRACKS * sizeof(GlobalShuffleTrack), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_global_shuf_dirs || !s_global_shuf_deck) {
        return false;
    }

    char root_path[256];
    struct stat st;
    if (stat("/sdcard/Musicas", &st) == 0 && S_ISDIR(st.st_mode)) {
        strlcpy(root_path, "/sdcard/Musicas", sizeof(root_path));
    } else if (stat("/sdcard/musicas", &st) == 0 && S_ISDIR(st.st_mode)) {
        strlcpy(root_path, "/sdcard/musicas", sizeof(root_path));
    } else if (stat("/sdcard/Music", &st) == 0 && S_ISDIR(st.st_mode)) {
        strlcpy(root_path, "/sdcard/Music", sizeof(root_path));
    } else {
        strlcpy(root_path, "/sdcard", sizeof(root_path));
    }

    // Fila BFS em PSRAM para varrer a arvore sem consumir stack da player_task
    const int QUEUE_CAP = 256;
    struct QueueItem {
        char path[256];
        uint8_t depth;
    };
    QueueItem *queue = (QueueItem *)heap_caps_malloc(QUEUE_CAP * sizeof(QueueItem), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!queue) return false;

    int q_head = 0;
    int q_tail = 0;
    strlcpy(queue[q_tail].path, root_path, sizeof(queue[q_tail].path));
    queue[q_tail].depth = 0;
    q_tail++;

    s_global_shuf_dir_count = 0;
    s_global_shuf_track_count = 0;

    while (q_head != q_tail &&
           s_global_shuf_dir_count < MAX_GLOBAL_SHUF_DIRS &&
           s_global_shuf_track_count < MAX_GLOBAL_SHUF_TRACKS) {
        QueueItem cur = queue[q_head];
        q_head = (q_head + 1) % QUEUE_CAP;

        DIR *d = opendir(cur.path);
        if (!d) continue;

        int audio_count = 0;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (is_junk_entry(ent->d_name)) continue;
            bool is_dir = (ent->d_type == DT_DIR);
            bool is_reg = (ent->d_type == DT_REG);
            if (ent->d_type == DT_UNKNOWN || (!is_dir && !is_reg)) {
                char full_p[520];
                snprintf(full_p, sizeof(full_p), "%.255s/%.255s", cur.path, ent->d_name);
                struct stat ent_st;
                if (stat(full_p, &ent_st) == 0) {
                    is_dir = S_ISDIR(ent_st.st_mode);
                    is_reg = S_ISREG(ent_st.st_mode);
                }
            }
            if (is_dir) {
                if (cur.depth < 6 && !is_excluded_shuffle_dir(ent->d_name)) {
                    int next_tail = (q_tail + 1) % QUEUE_CAP;
                    if (next_tail != q_head) {
                        snprintf(queue[q_tail].path, sizeof(queue[q_tail].path), "%.190s/%.60s", cur.path, ent->d_name);
                        queue[q_tail].depth = cur.depth + 1;
                        q_tail = next_tail;
                    }
                }
            } else if (has_supported_extension(ent->d_name)) {
                if (audio_count < MAX_ENTRIES) {
                    audio_count++;
                }
            }
        }
        closedir(d);

        if (audio_count > 0 && s_global_shuf_dir_count < MAX_GLOBAL_SHUF_DIRS) {
            uint16_t d_idx = (uint16_t)s_global_shuf_dir_count++;
            strlcpy(s_global_shuf_dirs[d_idx].path, cur.path, sizeof(s_global_shuf_dirs[d_idx].path));
            s_global_shuf_dirs[d_idx].count = (uint16_t)audio_count;

            for (int t = 0; t < audio_count && s_global_shuf_track_count < MAX_GLOBAL_SHUF_TRACKS; t++) {
                s_global_shuf_deck[s_global_shuf_track_count].dir_idx = d_idx;
                s_global_shuf_deck[s_global_shuf_track_count].track_idx = (uint16_t)t;
                s_global_shuf_track_count++;
            }
        }
    }

    heap_caps_free(queue);

    if (s_global_shuf_track_count <= 0) {
        s_global_shuf_ready = false;
        return false;
    }

    permute_global_shuffle_deck();
    s_global_shuf_ready = true;
    ESP_LOGI(TAG, "Deck Shuffle Global construido em PSRAM: %d faixas em %d pastas (distribuicao 1/N uniforme)",
             s_global_shuf_track_count, s_global_shuf_dir_count);
    return true;
}

static bool is_in_recent_shuffle_history(const char *dir, int index)
{
    if (!s_shuffle_history || s_shuffle_hist_count <= 0) return false;
    int check_max = (s_shuffle_hist_count < 16) ? s_shuffle_hist_count : 16;
    for (int i = 0; i < check_max; i++) {
        int pos = (s_shuffle_hist_idx - i + SHUFFLE_HIST_MAX) % SHUFFLE_HIST_MAX;
        if (s_shuffle_history[pos].index == index &&
            strcmp(s_shuffle_history[pos].dir, dir) == 0) {
            return true;
        }
    }
    return false;
}

// Sorteia uma pasta e faixa pelo cartao SD contido em Musicas (Shuffle Global Uniforme)
static bool pick_random_track_sd(char *out_dir, size_t dir_len, int *out_index)
{
    if (!s_global_shuf_ready || s_global_shuf_track_count <= 0) {
        if (!build_global_shuffle_deck()) {
            return false;
        }
    }

    for (int tries = 0; tries < s_global_shuf_track_count + 2; tries++) {
        if (s_global_shuf_pos >= s_global_shuf_track_count) {
            permute_global_shuffle_deck();
        }
        GlobalShuffleTrack pick = s_global_shuf_deck[s_global_shuf_pos++];
        if (pick.dir_idx >= (uint16_t)s_global_shuf_dir_count) continue;

        const char *cand_dir = s_global_shuf_dirs[pick.dir_idx].path;
        int cand_idx = (int)pick.track_idx;

        // Evita repetir a faixa atual ou faixas recentes do historico se a biblioteca for maior
        if (s_global_shuf_track_count > 1 &&
            cand_idx == s_current_index &&
            strcmp(cand_dir, s_playback_dir) == 0) {
            continue;
        }
        if (s_global_shuf_track_count > 20 &&
            tries < 8 &&
            is_in_recent_shuffle_history(cand_dir, cand_idx)) {
            continue;
        }

        strlcpy(out_dir, cand_dir, dir_len);
        *out_index = cand_idx;
        return true;
    }

    // Fallback garantido
    GlobalShuffleTrack pick = s_global_shuf_deck[0];
    strlcpy(out_dir, s_global_shuf_dirs[pick.dir_idx].path, dir_len);
    *out_index = (int)pick.track_idx;
    return true;
}


static void save_audio_prefs_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_GAPLESS, s_gapless_enabled ? 1 : 0);
    nvs_set_u8(h, NVS_KEY_SOFT_FADE, s_soft_fade_enabled ? 1 : 0);
    nvs_set_u8(h, NVS_KEY_RG_MODE, (uint8_t)s_replaygain_mode);
    nvs_set_u8(h, NVS_KEY_BM_MODE, (uint8_t)s_bookmark_mode);
    nvs_set_u8(h, NVS_KEY_AZ_JUMP, s_az_jump_enabled ? 1 : 0);
    nvs_set_u8(h, NVS_KEY_LOOP_MODE, (uint8_t)s_loop_mode);
    nvs_set_u8(h, NVS_KEY_SHUF_MODE, (uint8_t)s_shuffle_mode);
    nvs_set_str(h, "target_pl", s_target_playlist);
    nvs_commit(h);
    nvs_close(h);
}


static void load_audio_prefs_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v = 1;
    if (nvs_get_u8(h, NVS_KEY_GAPLESS, &v) == ESP_OK) s_gapless_enabled = (v != 0);
    if (nvs_get_u8(h, NVS_KEY_SOFT_FADE, &v) == ESP_OK) s_soft_fade_enabled = (v != 0);
    if (nvs_get_u8(h, NVS_KEY_RG_MODE, &v) == ESP_OK && v <= (uint8_t)RG_MODE_ALBUM) {
        s_replaygain_mode = (replaygain_mode_t)v;
    }
    if (nvs_get_u8(h, NVS_KEY_BM_MODE, &v) == ESP_OK && v <= (uint8_t)BOOKMARK_MODE_ALL) {
        s_bookmark_mode = (bookmark_mode_t)v;
    }
    if (nvs_get_u8(h, NVS_KEY_AZ_JUMP, &v) == ESP_OK) s_az_jump_enabled = (v != 0);
    uint8_t lm = 0;
    if (nvs_get_u8(h, NVS_KEY_LOOP_MODE, &lm) == ESP_OK && lm <= (uint8_t)AUDIO_LOOP_TRACK) {
        s_loop_mode = (audio_player_loop_mode_t)lm;
    }
    uint8_t sm = 0;
    if (nvs_get_u8(h, NVS_KEY_SHUF_MODE, &sm) == ESP_OK && sm <= (uint8_t)AUDIO_SHUFFLE_ALL) {
        s_shuffle_mode = (audio_player_shuffle_mode_t)sm;
    }
    s_state.loop_mode = (uint8_t)s_loop_mode;
    s_state.shuffle_mode = (uint8_t)s_shuffle_mode;
    size_t pl_len = sizeof(s_target_playlist);
    if (nvs_get_str(h, "target_pl", s_target_playlist, &pl_len) != ESP_OK || s_target_playlist[0] == '\0') {
        strlcpy(s_target_playlist, "Favoritos.m3u8", sizeof(s_target_playlist));
    }
    nvs_close(h);
}

void audio_player_set_gapless_enabled(bool enabled)
{
    s_gapless_enabled = enabled;
    save_audio_prefs_nvs();
}

bool audio_player_get_gapless_enabled(void)
{
    return s_gapless_enabled;
}

void audio_player_set_soft_fade_enabled(bool enabled)
{
    s_soft_fade_enabled = enabled;
    save_audio_prefs_nvs();
}

bool audio_player_get_soft_fade_enabled(void)
{
    return s_soft_fade_enabled;
}

void audio_player_set_replaygain_mode(replaygain_mode_t mode)
{
    if (mode > RG_MODE_ALBUM) mode = RG_MODE_OFF;
    s_replaygain_mode = mode;
    update_active_replaygain_multiplier(&s_current_track_meta);
    save_audio_prefs_nvs();
}

replaygain_mode_t audio_player_get_replaygain_mode(void)
{
    return s_replaygain_mode;
}

void audio_player_set_bookmark_mode(bookmark_mode_t mode)
{
    if (mode > BOOKMARK_MODE_ALL) mode = BOOKMARK_MODE_AUTO;
    s_bookmark_mode = mode;
    save_audio_prefs_nvs();
}

bookmark_mode_t audio_player_get_bookmark_mode(void)
{
    return s_bookmark_mode;
}

void audio_player_set_az_jump_enabled(bool enabled)
{
    s_az_jump_enabled = enabled;
    save_audio_prefs_nvs();
}

bool audio_player_get_az_jump_enabled(void)
{
    return s_az_jump_enabled;
}

audio_player_loop_mode_t audio_player_get_loop_mode(void)
{
    return s_loop_mode;
}

void audio_player_set_loop_mode(audio_player_loop_mode_t mode)
{
    if (mode > AUDIO_LOOP_TRACK) mode = AUDIO_LOOP_NONE;
    s_loop_mode = mode;
    s_loop_track_remaining = (s_loop_mode == AUDIO_LOOP_TRACK) ? 1 : 0;
    state_lock();
    s_state.loop_mode = (uint8_t)s_loop_mode;
    s_state.loop_track_repeat_count = s_loop_track_remaining;
    state_unlock();
    save_audio_prefs_nvs();
}

audio_player_loop_mode_t audio_player_prev_loop_mode(void)
{
    audio_player_loop_mode_t next;
    switch (s_loop_mode) {
        case AUDIO_LOOP_NONE:  next = AUDIO_LOOP_TRACK; break;
        case AUDIO_LOOP_TRACK: next = AUDIO_LOOP_ALL; break;
        case AUDIO_LOOP_ALL:   next = AUDIO_LOOP_NONE; break;
        default:               next = AUDIO_LOOP_NONE; break;
    }
    audio_player_set_loop_mode(next);
    return next;
}

audio_player_loop_mode_t audio_player_cycle_loop_mode(void)
{
    audio_player_loop_mode_t next;
    switch (s_loop_mode) {
        case AUDIO_LOOP_NONE:  next = AUDIO_LOOP_ALL; break;
        case AUDIO_LOOP_ALL:   next = AUDIO_LOOP_TRACK; break;
        case AUDIO_LOOP_TRACK: next = AUDIO_LOOP_NONE; break;
        default:               next = AUDIO_LOOP_NONE; break;
    }
    audio_player_set_loop_mode(next);
    return next;
}

audio_player_shuffle_mode_t audio_player_get_shuffle_mode(void)
{
    return s_shuffle_mode;
}

void audio_player_set_shuffle_mode(audio_player_shuffle_mode_t mode)
{
    if (mode > AUDIO_SHUFFLE_ALL) mode = AUDIO_SHUFFLE_OFF;
    s_shuffle_mode = mode;
    state_lock();
    s_state.shuffle_mode = (uint8_t)s_shuffle_mode;
    state_unlock();
    if (s_shuffle_mode == AUDIO_SHUFFLE_FOLDER) {
        scan_lock();
        update_folder_shuffle(s_playback_scan.audio_count, s_current_index);
        scan_unlock();
    } else if (s_shuffle_mode == AUDIO_SHUFFLE_ALL) {
        if (s_global_shuf_ready && s_global_shuf_track_count > 1) {
            permute_global_shuffle_deck();
        }
    }
    save_audio_prefs_nvs();
}

audio_player_shuffle_mode_t audio_player_prev_shuffle_mode(void)
{
    audio_player_shuffle_mode_t next;
    switch (s_shuffle_mode) {
        case AUDIO_SHUFFLE_OFF:    next = AUDIO_SHUFFLE_ALL; break;
        case AUDIO_SHUFFLE_ALL:    next = AUDIO_SHUFFLE_FOLDER; break;
        case AUDIO_SHUFFLE_FOLDER: next = AUDIO_SHUFFLE_OFF; break;
        default:                   next = AUDIO_SHUFFLE_OFF; break;
    }
    audio_player_set_shuffle_mode(next);
    return next;
}

audio_player_shuffle_mode_t audio_player_cycle_shuffle_mode(void)
{
    audio_player_shuffle_mode_t next;
    switch (s_shuffle_mode) {
        case AUDIO_SHUFFLE_OFF:    next = AUDIO_SHUFFLE_FOLDER; break;
        case AUDIO_SHUFFLE_FOLDER: next = AUDIO_SHUFFLE_ALL; break;
        case AUDIO_SHUFFLE_ALL:    next = AUDIO_SHUFFLE_OFF; break;
        default:                   next = AUDIO_SHUFFLE_OFF; break;
    }
    audio_player_set_shuffle_mode(next);
    return next;
}


static char extract_entry_initial_letter(const char *raw_name)
{
    if (!raw_name) return '#';
    const char *p = raw_name;
    if (strncmp(p, "[PL] ", 5) == 0) p += 5;
    const char *slash = strrchr(p, '/');
    if (slash && slash[1] != '\0') p = slash + 1;
    while (*p != '\0') {
        unsigned char c = (unsigned char)*p;
        if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
        if (c >= 'A' && c <= 'Z') return (char)c;
        if (c >= '0' && c <= '9') return '#';
        p++;
    }
    return '#';
}

int audio_player_jump_browse_letter(int cur_cursor, int dir_step, char *out_letter)
{
    if (out_letter) *out_letter = 0;
    scan_lock();
    bool has_parent = browse_has_parent();
    int offset = has_parent ? 1 : 0;
    int total = offset + s_browse_scan.subdir_count + s_browse_scan.audio_count;
    if (total <= offset + 1) {
        scan_unlock();
        return cur_cursor;
    }

    if (cur_cursor < offset) cur_cursor = offset;
    if (cur_cursor >= total) cur_cursor = total - 1;

    auto get_raw_at = [&](int idx) -> const char * {
        int i = idx - offset;
        if (i >= 0 && i < s_browse_scan.subdir_count) return s_browse_scan.subdirs[i];
        i -= s_browse_scan.subdir_count;
        if (i >= 0 && i < s_browse_scan.audio_count) return extract_track_basename(s_browse_scan.audio_files[i]);
        return "";
    };

    char cur_let = extract_entry_initial_letter(get_raw_at(cur_cursor));
    int count_items = total - offset;
    int step = (dir_step >= 0) ? 1 : -1;

    int chosen = cur_cursor;
    char chosen_let = cur_let;
    for (int s = 1; s < count_items; s++) {
        int cand_rel = ((cur_cursor - offset) + s * step + count_items) % count_items;
        int cand_idx = cand_rel + offset;
        char cand_let = extract_entry_initial_letter(get_raw_at(cand_idx));
        if (cand_let != cur_let) {
            if (step < 0) {
                chosen = cand_idx;
                chosen_let = cand_let;
                for (int k = 1; k < count_items; k++) {
                    int prev_rel = (cand_rel - k + count_items) % count_items;
                    int prev_idx = prev_rel + offset;
                    if (extract_entry_initial_letter(get_raw_at(prev_idx)) == chosen_let) {
                        chosen = prev_idx;
                    } else {
                        break;
                    }
                }
            } else {
                chosen = cand_idx;
                chosen_let = cand_let;
            }
            break;
        }
    }

    scan_unlock();
    if (out_letter) *out_letter = chosen_let;
    return chosen;
}

static void ensure_playlists_dir_and_migration(void)
{
    static bool s_migrated = false;
    if (s_migrated) return;
    s_migrated = true;

    // Aloca buffers temporarios no heap para evitar estourar a pilha de qualquer tarefa chamadora
    char *buf = (char *)malloc(MAX_PATH_LEN * 3);
    if (!buf) return;
    char *pldir = buf;
    char *p1 = buf + MAX_PATH_LEN;
    char *p2 = buf + MAX_PATH_LEN * 2;

    snprintf(pldir, MAX_PATH_LEN, "%s/Playlists", s_root_dir);
    struct stat st;
    if (stat(pldir, &st) != 0) {
        mkdir(pldir, 0777);
    }
    // 1. Migra .m3u e .m3u8 soltos na raiz /sdcard/
    DIR *d = opendir(s_root_dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            if (!is_playlist_extension(ent->d_name)) continue;
            snprintf(p1, MAX_PATH_LEN, "%s/%s", s_root_dir, ent->d_name);
            snprintf(p2, MAX_PATH_LEN, "%s/Playlists/%s", s_root_dir, ent->d_name);
            rename(p1, p2);
        }
        closedir(d);
    }
    // 2. Migra de /sdcard/listas caso exista
    snprintf(p1, MAX_PATH_LEN, "%s/listas", s_root_dir);
    d = opendir(p1);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            if (!is_playlist_extension(ent->d_name)) continue;
            snprintf(p1, MAX_PATH_LEN, "%s/listas/%s", s_root_dir, ent->d_name);
            snprintf(p2, MAX_PATH_LEN, "%s/Playlists/%s", s_root_dir, ent->d_name);
            rename(p1, p2);
        }
        closedir(d);
    }
    // 3. Garante Favoritos.m3u8 (estilo Spotify: lista padrao inicial)
    snprintf(p1, MAX_PATH_LEN, "%s/Playlists/Favoritos.m3u8", s_root_dir);
    snprintf(p2, MAX_PATH_LEN, "%s/Playlists/Favoritos.m3u", s_root_dir);
    if (stat(p1, &st) != 0 && stat(p2, &st) != 0) {
        FILE *fp = fopen(p1, "w");
        if (fp) {
            fprintf(fp, "#EXTM3U\n");
            fclose(fp);
        }
    }
    free(buf);
}

void audio_player_ensure_playlists_migrated(void)
{
    ensure_playlists_dir_and_migration();
}

void audio_player_browse_to_playlists(void)
{
    ensure_playlists_dir_and_migration();
    char pl_dir[DIR_LEN];
    snprintf(pl_dir, sizeof(pl_dir), "%s/Playlists", s_root_dir);
    scan_lock();
    strlcpy(s_browse_dir, pl_dir, sizeof(s_browse_dir));
    scan_dir(s_browse_dir, s_browse_scan);
    scan_unlock();
}

const char *audio_player_get_target_playlist(void)
{
    if (s_target_playlist[0] == '\0') return "Favoritos.m3u8";
    return s_target_playlist;
}

static bool get_target_playlist_abs_path(char *out_path, size_t out_len)
{
    ensure_playlists_dir_and_migration();
    const char *pl = audio_player_get_target_playlist();
    int n = snprintf(out_path, out_len, "%s/Playlists/%s", s_root_dir, pl);
    return (n > 0 && (size_t)n < out_len);
}

int audio_player_get_playlists_count(void)
{
    ensure_playlists_dir_and_migration();
    char pldir[MAX_PATH_LEN];
    snprintf(pldir, sizeof(pldir), "%s/Playlists", s_root_dir);
    DIR *d = opendir(pldir);
    if (!d) return 1; // Pelo menos Favoritos existe

    int count = 0;
    bool has_fav = false;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (!is_playlist_extension(ent->d_name)) continue;
        if (strncasecmp(ent->d_name, "Favoritos", 9) == 0) {
            has_fav = true;
        } else {
            count++;
        }
    }
    closedir(d);
    return count + (has_fav ? 1 : 1);
}

bool audio_player_get_playlist_by_index(int index, char *out_name, size_t out_len, bool *out_is_active)
{
    if (!out_name || out_len == 0 || index < 0) return false;
    ensure_playlists_dir_and_migration();

    const int MAX_PLS = 64;
    char (*pl_list)[64] = (char (*)[64])heap_caps_calloc(MAX_PLS, 64, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pl_list) return false;

    // 0 = Sempre Favoritos (Spotify style)
    int count = 0;
    strlcpy(pl_list[count++], "Favoritos.m3u8", 64);

    char pldir[MAX_PATH_LEN];
    snprintf(pldir, sizeof(pldir), "%s/Playlists", s_root_dir);
    DIR *d = opendir(pldir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && count < MAX_PLS) {
            if (ent->d_name[0] == '.') continue;
            if (!is_playlist_extension(ent->d_name)) continue;
            if (strncasecmp(ent->d_name, "Favoritos", 9) == 0) {
                // Atualiza com a extensao exata encontrada (.m3u ou .m3u8)
                strlcpy(pl_list[0], ent->d_name, 64);
                continue;
            }
            strlcpy(pl_list[count++], ent->d_name, 64);
        }
        closedir(d);
    }

    if (index >= count) {
        heap_caps_free(pl_list);
        return false;
    }

    strlcpy(out_name, pl_list[index], out_len);
    if (out_is_active) {
        *out_is_active = (strcasecmp(pl_list[index], s_target_playlist) == 0);
    }

    heap_caps_free(pl_list);
    return true;
}

bool audio_player_set_target_playlist_by_index(int index)
{
    char pl_name[64];
    if (audio_player_get_playlist_by_index(index, pl_name, sizeof(pl_name), NULL)) {
        strlcpy(s_target_playlist, pl_name, sizeof(s_target_playlist));
        save_audio_prefs_nvs();
        return true;
    }
    return false;
}

void audio_player_cycle_target_playlist(int delta)
{
    if (s_usb_takeover_requested) return;
    ensure_playlists_dir_and_migration();

    const int MAX_PLS = 64;
    char (*pl_list)[64] = (char (*)[64])heap_caps_calloc(MAX_PLS, 64, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pl_list) return;

    int count = 0;
    strlcpy(pl_list[count++], "Favoritos.m3u8", 64);

    char pldir[MAX_PATH_LEN];
    snprintf(pldir, sizeof(pldir), "%s/Playlists", s_root_dir);
    DIR *d = opendir(pldir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && count < MAX_PLS) {
            if (ent->d_name[0] == '.') continue;
            if (!is_playlist_extension(ent->d_name)) continue;
            if (strncasecmp(ent->d_name, "Favoritos", 9) == 0) {
                strlcpy(pl_list[0], ent->d_name, 64);
                continue;
            }
            bool exists = false;
            for (int i = 0; i < count; i++) {
                if (strcasecmp(pl_list[i], ent->d_name) == 0) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                strlcpy(pl_list[count++], ent->d_name, 64);
            }
        }
        closedir(d);
    }

    int cur_idx = 0;
    for (int i = 0; i < count; i++) {
        if (strcasecmp(pl_list[i], s_target_playlist) == 0) {
            cur_idx = i;
            break;
        }
    }

    int step = (delta >= 0) ? 1 : -1;
    int next_idx = (cur_idx + step + count) % count;
    strlcpy(s_target_playlist, pl_list[next_idx], sizeof(s_target_playlist));
    heap_caps_free(pl_list);

    save_audio_prefs_nvs();
}

bool audio_player_create_playlist(const char *name, char *out_filename, size_t out_len)
{
    if (s_usb_takeover_requested) return false;

    char clean[48] = {0};
    if (name) {
        size_t j = 0;
        for (size_t i = 0; name[i] != '\0' && j + 1 < sizeof(clean); i++) {
            char c = name[i];
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                c == '"' || c == '<' || c == '>' || c == '|') {
                continue;
            }
            if (j == 0 && c == ' ') continue;
            clean[j++] = c;
        }
        while (j > 0 && clean[j - 1] == ' ') clean[--j] = '\0';
    }
    if (clean[0] == '\0') {
        strlcpy(clean, "Playlist", sizeof(clean));
    }

    char final_name[64] = {0};
    if (is_playlist_extension(clean)) {
        strlcpy(final_name, clean, sizeof(final_name));
    } else {
        snprintf(final_name, sizeof(final_name), "%s.m3u8", clean);
    }

    char abs_path[MAX_PATH_LEN] = {0};
    ensure_playlists_dir_and_migration();
    snprintf(abs_path, sizeof(abs_path), "%s/Playlists/%s", s_root_dir, final_name);

    struct stat st;
    if (stat(abs_path, &st) != 0) {
        FILE *fp = fopen(abs_path, "w");
        if (!fp) return false;
        fprintf(fp, "#EXTM3U\n");
        fclose(fp);
    }

    strlcpy(s_target_playlist, final_name, sizeof(s_target_playlist));
    save_audio_prefs_nvs();

    if (out_filename && out_len > 0) {
        strlcpy(out_filename, final_name, out_len);
    }

    scan_lock();
    if (strcmp(s_browse_dir, s_root_dir) == 0) {
        scan_dir(s_browse_dir, s_browse_scan);
    }
    scan_unlock();
    return true;
}

bool audio_player_delete_playlist(const char *name_or_filename)
{
    if (s_usb_takeover_requested || !name_or_filename || name_or_filename[0] == '\0') {
        return false;
    }

    // Se vier com prefixo "[PL] ", remove
    const char *p = name_or_filename;
    if (strncmp(p, "[PL] ", 5) == 0) {
        p += 5;
    }

    // Isola o basename se vier caminho completo (/sdcard/Playlists/nome.m3u8)
    const char *slash = strrchr(p, '/');
    if (slash) p = slash + 1;
    slash = strrchr(p, '\\');
    if (slash) p = slash + 1;

    char clean[64] = {0};
    strlcpy(clean, p, sizeof(clean));

    // Protecao inegociavel do sistema: Favoritos e lista padrao protegida!
    if (strncasecmp(clean, "Favoritos", 9) == 0) {
        ESP_LOGW(TAG, "Tentativa de apagar lista protegida: %s", clean);
        return false;
    }

    ensure_playlists_dir_and_migration();

    char abs_path[MAX_PATH_LEN] = {0};
    snprintf(abs_path, sizeof(abs_path), "%s/Playlists/%s", s_root_dir, clean);

    struct stat st;
    bool found = (stat(abs_path, &st) == 0);

    if (!found && !is_playlist_extension(clean)) {
        snprintf(abs_path, sizeof(abs_path), "%s/Playlists/%s.m3u8", s_root_dir, clean);
        if (stat(abs_path, &st) == 0) {
            found = true;
            snprintf(clean + strlen(clean), sizeof(clean) - strlen(clean), ".m3u8");
        } else {
            snprintf(abs_path, sizeof(abs_path), "%s/Playlists/%s.m3u", s_root_dir, clean);
            if (stat(abs_path, &st) == 0) {
                found = true;
                snprintf(clean + strlen(clean), sizeof(clean) - strlen(clean), ".m3u");
            }
        }
    }

    if (!found) {
        ESP_LOGE(TAG, "Playlist nao encontrada para exclusao: %s", abs_path);
        return false;
    }

    if (unlink(abs_path) != 0) {
        ESP_LOGE(TAG, "Erro ao deletar playlist: %s", abs_path);
        return false;
    }

    ESP_LOGI(TAG, "Playlist excluida com sucesso: %s", abs_path);

    // Se era a playlist alvo atual, reseta para Favoritos.m3u8
    if (strcasecmp(s_target_playlist, clean) == 0 ||
        strcasecmp(s_target_playlist, p) == 0) {
        strlcpy(s_target_playlist, "Favoritos.m3u8", sizeof(s_target_playlist));
        save_audio_prefs_nvs();
    }

    scan_lock();
    scan_dir(s_browse_dir, s_browse_scan);
    scan_unlock();
    return true;
}

static bool modify_single_track_in_playlist_file(const char *pl_abs_path, const char *full_track_path, int mode, bool *out_status)
{
    // mode: 0 = toggle, 1 = force add (like), -1 = force remove (unlike)
    if (out_status) *out_status = false;
    if (!pl_abs_path || !full_track_path || full_track_path[0] == '\0') return false;

    char **lines = (char **)heap_caps_calloc(MAX_ENTRIES, sizeof(char *), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!lines) return false;

    int count = 0;
    bool found = false;

    FILE *fp = fopen(pl_abs_path, "r");
    if (fp) {
        char buf[512];
        while (fgets(buf, sizeof(buf), fp) != NULL && count < MAX_ENTRIES) {
            size_t l = strlen(buf);
            while (l > 0 && (buf[l - 1] == '\r' || buf[l - 1] == '\n')) buf[--l] = '\0';
            if (l == 0 || buf[0] == '#') continue;
            if (strcasecmp(buf, full_track_path) == 0) {
                found = true;
                if (mode == 1) { // manter na lista se for forcar Like
                    lines[count] = strdup(buf);
                    if (lines[count]) count++;
                }
                continue;
            }
            lines[count] = strdup(buf);
            if (lines[count]) count++;
        }
        fclose(fp);
    }

    bool changed = false;
    if (mode == 0) { // toggle
        if (!found && count < MAX_ENTRIES) {
            lines[count] = strdup(full_track_path);
            if (lines[count]) count++;
            if (out_status) *out_status = true;
            changed = true;
        } else if (found) {
            if (out_status) *out_status = false;
            changed = true;
        }
    } else if (mode == 1) { // force add
        if (!found && count < MAX_ENTRIES) {
            lines[count] = strdup(full_track_path);
            if (lines[count]) count++;
            changed = true;
        }
        if (out_status) *out_status = true;
    } else if (mode == -1) { // force remove
        if (found) {
            changed = true;
        }
        if (out_status) *out_status = false;
    }

    if (!changed) {
        for (int i = 0; i < count; i++) free(lines[i]);
        heap_caps_free(lines);
        return true;
    }

    FILE *out_fp = fopen(pl_abs_path, "w");
    if (!out_fp) {
        for (int i = 0; i < count; i++) free(lines[i]);
        heap_caps_free(lines);
        return false;
    }
    fprintf(out_fp, "#EXTM3U\n");
    for (int i = 0; i < count; i++) {
        fprintf(out_fp, "%s\n", lines[i]);
        free(lines[i]);
    }
    fclose(out_fp);
    heap_caps_free(lines);

    scan_lock();
    if (strcmp(s_browse_dir, s_root_dir) == 0 || strcmp(s_browse_dir, pl_abs_path) == 0) {
        scan_dir(s_browse_dir, s_browse_scan);
    }
    scan_unlock();
    return true;
}

static inline bool toggle_single_track_in_playlist_file(const char *pl_abs_path, const char *full_track_path, bool *out_added)
{
    return modify_single_track_in_playlist_file(pl_abs_path, full_track_path, 0, out_added);
}

static bool add_folder_tracks_to_playlist_file(const char *pl_abs_path, const char *folder_abs_path, int *out_added_count)
{
    if (out_added_count) *out_added_count = 0;
    if (!pl_abs_path || !folder_abs_path || is_playlist_extension(folder_abs_path)) return false;

    char **lines = (char **)heap_caps_calloc(MAX_ENTRIES, sizeof(char *), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!lines) return false;

    int count = 0;
    FILE *fp = fopen(pl_abs_path, "r");
    if (fp) {
        char buf[512];
        while (fgets(buf, sizeof(buf), fp) != NULL && count < MAX_ENTRIES) {
            size_t l = strlen(buf);
            while (l > 0 && (buf[l - 1] == '\r' || buf[l - 1] == '\n')) buf[--l] = '\0';
            if (l == 0 || buf[0] == '#') continue;
            lines[count] = strdup(buf);
            if (lines[count]) count++;
        }
        fclose(fp);
    }

    DirScan *tmp_scan = (DirScan *)heap_caps_calloc(1, sizeof(DirScan), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tmp_scan) {
        for (int i = 0; i < count; i++) free(lines[i]);
        heap_caps_free(lines);
        return false;
    }
    scan_dir(folder_abs_path, *tmp_scan);

    int added = 0;
    char full_track[MAX_PATH_LEN];
    for (int t = 0; t < tmp_scan->audio_count && count < MAX_ENTRIES; t++) {
        resolve_track_full_path(folder_abs_path, tmp_scan->audio_files[t], full_track, sizeof(full_track));
        bool exists = false;
        for (int i = 0; i < count; i++) {
            if (strcasecmp(lines[i], full_track) == 0) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            lines[count] = strdup(full_track);
            if (lines[count]) {
                count++;
                added++;
            }
        }
    }
    free_dir_scan(*tmp_scan);
    heap_caps_free(tmp_scan);

    FILE *out_fp = fopen(pl_abs_path, "w");
    if (!out_fp) {
        for (int i = 0; i < count; i++) free(lines[i]);
        heap_caps_free(lines);
        return false;
    }
    fprintf(out_fp, "#EXTM3U\n");
    for (int i = 0; i < count; i++) {
        fprintf(out_fp, "%s\n", lines[i]);
        free(lines[i]);
    }
    fclose(out_fp);
    heap_caps_free(lines);

    if (out_added_count) *out_added_count = added;

    scan_lock();
    if (strcmp(s_browse_dir, s_root_dir) == 0 || strcmp(s_browse_dir, pl_abs_path) == 0) {
        scan_dir(s_browse_dir, s_browse_scan);
    }
    scan_unlock();
    return true;
}

bool audio_player_toggle_favorite_current(bool *out_added)
{
    if (out_added) *out_added = false;
    if (s_usb_takeover_requested) return false;

    char full_track_path[MAX_PATH_LEN] = {0};
    scan_lock();
    if (s_current_index >= 0 && s_current_index < s_playback_scan.audio_count) {
        resolve_track_full_path(s_playback_dir, s_playback_scan.audio_files[s_current_index],
                                full_track_path, sizeof(full_track_path));
    }
    scan_unlock();

    if (full_track_path[0] == '\0') return false;

    char pl_path[MAX_PATH_LEN] = {0};
    if (!get_target_playlist_abs_path(pl_path, sizeof(pl_path))) return false;
    bool res = toggle_single_track_in_playlist_file(pl_path, full_track_path, out_added);
    if (res && out_added) {
        state_lock();
        s_state.is_favorite = *out_added;
        state_unlock();
    }
    return res;
}

bool audio_player_add_current_folder_to_target_playlist(int *out_added_count)
{
    if (out_added_count) *out_added_count = 0;
    if (s_usb_takeover_requested) return false;

    char folder_path[MAX_PATH_LEN] = {0};
    scan_lock();
    strlcpy(folder_path, s_playback_dir[0] ? s_playback_dir : s_browse_dir, sizeof(folder_path));
    scan_unlock();

    char pl_path[MAX_PATH_LEN] = {0};
    if (!get_target_playlist_abs_path(pl_path, sizeof(pl_path))) return false;
    return add_folder_tracks_to_playlist_file(pl_path, folder_path, out_added_count);
}

bool audio_player_toggle_browse_item_in_target_playlist(int browse_idx, bool *out_added, int *out_count)
{
    if (out_added) *out_added = false;
    if (out_count) *out_count = 0;
    if (s_usb_takeover_requested) return false;

    char pl_path[MAX_PATH_LEN] = {0};
    if (!get_target_playlist_abs_path(pl_path, sizeof(pl_path))) return false;

    char item_abs[MAX_PATH_LEN] = {0};
    bool is_folder = false;

    scan_lock();
    int offset = browse_has_parent() ? 1 : 0;
    int idx = browse_idx - offset;
    if (idx >= 0 && idx < s_browse_scan.subdir_count) {
        const char *sub = s_browse_scan.subdirs[idx];
        const char *clean_sub = (strncmp(sub, "[PL] ", 5) == 0) ? (sub + 5) : sub;
        snprintf(item_abs, sizeof(item_abs), "%s/%s", s_browse_dir, clean_sub);
        is_folder = !is_playlist_extension(clean_sub);
    } else {
        int a_idx = idx - s_browse_scan.subdir_count;
        if (a_idx >= 0 && a_idx < s_browse_scan.audio_count) {
            resolve_track_full_path(s_browse_dir, s_browse_scan.audio_files[a_idx], item_abs, sizeof(item_abs));
            is_folder = false;
        }
    }
    scan_unlock();

    if (item_abs[0] == '\0') return false;

    if (is_folder) {
        int added = 0;
        bool ok = add_folder_tracks_to_playlist_file(pl_path, item_abs, &added);
        if (out_added) *out_added = (added > 0);
        if (out_count) *out_count = added;
        return ok;
    } else if (!is_playlist_extension(item_abs)) {
        bool added = false;
        bool ok = toggle_single_track_in_playlist_file(pl_path, item_abs, &added);
        if (out_added) *out_added = added;
        if (out_count) *out_count = 1;
        return ok;
    }
    return false;
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
    track_metadata_t meta = {};
    bool has_meta = false;
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
    resolve_track_full_path(s_playback_dir, s_playback_scan.audio_files[next_index], path, sizeof(path));
    strlcpy(dir_snapshot, s_playback_dir, sizeof(dir_snapshot));
    scan_unlock();

    metadata_extract(path, &s_prefetch.meta);
    s_prefetch.has_meta = true;

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
    // Garante SDMMC a 40 MHz High-Speed para maxima vazao e margem de tempo no Core 1
    if (sd_card_get_frequency() != SDMMC_FREQ_HIGHSPEED) {
        sd_card_set_frequency(SDMMC_FREQ_HIGHSPEED);
    }

    const char *clean_base = extract_track_basename(display_name);
    track_metadata_t meta;
    char meta_path[MAX_PATH_LEN];
    resolve_track_full_path(s_playback_dir, display_name, meta_path, sizeof(meta_path));

    state_lock();
    bool is_same_loaded_track = s_state.track_loaded && (strcmp(s_state.filename, clean_base) == 0);
    state_unlock();

    scan_lock();
    bool prefetch_meta_ready = s_prefetch.fp && s_prefetch.index == index &&
                               strcmp(s_prefetch.dir, s_playback_dir) == 0 && s_prefetch.has_meta;
    if (prefetch_meta_ready) {
        meta = s_prefetch.meta;
    }
    scan_unlock();

    if (is_same_loaded_track && s_current_track_meta.total_sec > 0) {
        meta = s_current_track_meta;
    } else if (!prefetch_meta_ready) {
        metadata_extract(meta_path, &meta); // rapido - so' le' os primeiros KB, nao decodifica audio
    }
    s_current_track_meta = meta;
    update_active_replaygain_multiplier(&s_current_track_meta);

    char metrics_rel_path[PATH_LEN];
    build_relative_path(s_playback_dir, display_name, metrics_rel_path, sizeof(metrics_rel_path));

    // Validacao seletiva por formato dos limites de decodificacao e hardware:
    // - FLAC: estritamente <= 24 bits e <= 192 kHz (libflac nao suporta 32 bits e entra em loop/TWDT).
    // - WAV: PCM linear descomprimido, aceita nativamente 16, 24 e 32 bits ate 384 kHz no DAC PCM5102A.
    // - Outros formatos comprimidos: limite seguro de 192 kHz / 24 bits.
    mps3::AudioFormat fmt_check = mps3::detect_audio_format(display_name);
    bool unsupported_format_limits = false;
    char error_msg[32] = {0};

    if (fmt_check == mps3::AudioFormat::Flac) {
        if (meta.bits_per_sample > 24 || meta.sample_rate > 192000) {
            unsupported_format_limits = true;
            snprintf(error_msg, sizeof(error_msg), "FLAC max 24b/192k");
            ESP_LOGE(TAG, "FLAC fora dos limites suportados: %s (%u Hz / %u bits - max 192kHz/24b)",
                     clean_base, (unsigned)meta.sample_rate, (unsigned)meta.bits_per_sample);
        }
    } else if (fmt_check == mps3::AudioFormat::Wav) {
        if (meta.bits_per_sample > 32 || meta.sample_rate > 384000) {
            unsupported_format_limits = true;
            snprintf(error_msg, sizeof(error_msg), "WAV max 32b/384k");
            ESP_LOGE(TAG, "WAV fora dos limites do DAC: %s (%u Hz / %u bits - max 384kHz/32b)",
                     clean_base, (unsigned)meta.sample_rate, (unsigned)meta.bits_per_sample);
        }
    } else {
        if (meta.sample_rate > 192000 || meta.bits_per_sample > 24) {
            unsupported_format_limits = true;
            snprintf(error_msg, sizeof(error_msg), "Max 192kHz/24b");
            ESP_LOGE(TAG, "Formato comprimido excede limites: %s (%u Hz / %u bits - max 192kHz/24b)",
                     clean_base, (unsigned)meta.sample_rate, (unsigned)meta.bits_per_sample);
        }
    }

    if (unsupported_format_limits) {
        state_lock();
        strncpy(s_state.filename, clean_base, sizeof(s_state.filename) - 1);
        s_state.filename[sizeof(s_state.filename) - 1] = '\0';
        strncpy(s_state.last_error, error_msg, sizeof(s_state.last_error) - 1);
        s_state.last_error[sizeof(s_state.last_error) - 1] = '\0';
        s_state.playing = false;
        s_state.track_loaded = false;
        state_unlock();

        scan_lock();
        if (s_prefetch.fp && s_prefetch.index == index && strcmp(s_prefetch.dir, s_playback_dir) == 0) {
            fclose(s_prefetch.fp);
            s_prefetch.fp = nullptr;
            s_prefetch.index = -1;
            if (s_prefetch.buf) { heap_caps_free(s_prefetch.buf); s_prefetch.buf = nullptr; }
        }
        scan_unlock();

        // Limpa o registro de resume da NVS para evitar boot-loops ("brick" no reinicio)
        clear_resume_state_if_matches(metrics_rel_path);

        vTaskDelay(pdMS_TO_TICKS(1800)); // tempo para a UI exibir o erro
        return;
    }

    if (!is_same_loaded_track && resume_elapsed_sec == 0 && should_bookmark_track(metrics_rel_path, meta.total_sec)) {
        uint32_t bm_sec = bookmarks_get(metrics_rel_path);
        if (bm_sec >= 5 && (meta.total_sec == 0 || bm_sec + 10 < meta.total_sec)) {
            resume_elapsed_sec = bm_sec;
            ESP_LOGI(TAG, "Bookmark individual restaurado para %s: %u s", clean_base, (unsigned)resume_elapsed_sec);
        }
    }

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
        resolve_track_full_path(s_playback_dir, display_name, full_path, sizeof(full_path));
        if (sd_card_get_frequency() != SDMMC_FREQ_HIGHSPEED) {
            sd_card_set_frequency(SDMMC_FREQ_HIGHSPEED);
        }
        fp = fopen(full_path, "rb");
        if (!fp) {
            ESP_LOGE(TAG, "Nao foi possivel abrir %s", full_path);
            state_lock();
            strncpy(s_state.filename, clean_base, sizeof(s_state.filename) - 1);
            s_state.filename[sizeof(s_state.filename) - 1] = '\0';
            snprintf(s_state.last_error, sizeof(s_state.last_error), "Erro ao abrir arquivo");
            state_unlock();
            vTaskDelay(pdMS_TO_TICKS(1800));
            return;
        }
        ESP_LOGI(TAG, "Faixa aberta no SD (FatFs FastSeek/CLMT ativo): %s", full_path);
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
                size_t space = INBUF_SIZE - prefix_len;
                long cur_pos = ftell(fp);
                size_t to_read = space;
                if (cur_pos >= 0) {
                    size_t misalign = (size_t)(cur_pos % 512);
                    if (misalign != 0) {
                        size_t align_bytes = 512 - misalign;
                        if (space >= align_bytes) {
                            to_read = align_bytes + ((space - align_bytes) & ~511UL);
                        }
                    } else {
                        to_read = space & ~511UL;
                    }
                }
                valid_end = prefix_len + fread(inbuf + prefix_len, 1, to_read, fp);
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
        ESP_LOGE(TAG, "Formato nao suportado: %s", clean_base);
        state_lock();
        strncpy(s_state.filename, clean_base, sizeof(s_state.filename) - 1);
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

    bool is_fav = audio_player_is_current_favorite();
    state_lock();
    s_state.is_favorite = is_fav;
    strncpy(s_state.filename, clean_base, sizeof(s_state.filename) - 1);
    s_state.filename[sizeof(s_state.filename) - 1] = '\0';

    if (meta.title[0]) {
        strncpy(s_state.title, meta.title, sizeof(s_state.title) - 1);
        s_state.title[sizeof(s_state.title) - 1] = '\0';
    } else {
        // Sem tag de titulo - usa o nome do arquivo sem a extensao.
        strncpy(s_state.title, clean_base, sizeof(s_state.title) - 1);
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
    uint32_t display_elapsed = s_forced_restart_pending ? s_forced_restart_target_sec : resume_elapsed_sec;
    s_state.elapsed_sec = display_elapsed;
    s_state.total_sec = meta.total_sec;
    s_state.duration_is_estimate = meta.duration_is_estimate;
    s_state.playing = true;
    s_state.track_loaded = is_same_loaded_track || (resume_elapsed_sec > 0);
    state_unlock();
    s_last_elapsed_sec = display_elapsed;
    if (!s_gapless_transition_active) {
        eq_reset_state();
    }

    user_metrics_on_track_begin(metrics_rel_path);

    uint64_t session_decoded_samples = 0;
    bool prebuffer_done = false;
    size_t prebuffered_frames = 0;
    size_t prebuffer_target_frames = 0;

    s_seek_pending = false;
    if (resume_elapsed_sec > 0) {
        s_seek_target_sec = resume_elapsed_sec;
        s_seek_pending = true;
    }

    bool marked_completed = false;
    bool reached_eof = false;

    int stuck_at_eof_count = 0; // ver checagem "at_true_eof" mais abaixo

    bool done = false;
    bool need_refill = (valid_end == 0);

    int64_t prof_t_read_us = 0;
    int64_t prof_t_decode_us = 0;
    int64_t prof_t_dsp_us = 0;
    int64_t prof_t_memmove_us = 0;
    uint32_t prof_read_bytes = 0;
    uint32_t prof_read_count = 0;
    int64_t prof_last_log_us = esp_timer_get_time();

    bool use_async = (s_async_reader.buf != nullptr && !use_frag_demux && !use_webm_demux);
    if (use_async) {
        async_reader_start(fp);
        ESP_LOGI(TAG, "Double-buffering assincrono ativo: Ring=%u KB (Core 0 GDMA pre-fetch)",
                 (unsigned)(s_async_reader.capacity / 1024U));
    }

    int zero_samples_loop_count = 0;

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
        // So compacta (memmove em PSRAM) quando os bytes validos restantes cairem
        // abaixo de 64 KB E nao houver espaco no final do inbuf para ler pelo menos 64 KB.
        if (!use_webm_demux && data_start > 0 && (INBUF_SIZE - valid_end) < 65536 && (valid_end - data_start) < 65536) {
            size_t remaining = valid_end - data_start;
            int64_t t_m0 = esp_timer_get_time();
            memmove(inbuf, inbuf + data_start, remaining);
            prof_t_memmove_us += (esp_timer_get_time() - t_m0);
            data_start = 0;
            valid_end = remaining;
            need_refill = true;
        }

        // Se ainda sobrou prefixo M4A "fora de ordem" (moov reordenado
        // virtualmente - ver prepare_m4a_prefix()), drena ele ANTES de
        // ler mais do arquivo de verdade - pode precisar de varias voltas
        // do loop se o moov for maior que INBUF_SIZE inteiro.
        size_t from_m4a_prefix = 0;
        if (pending_m4a_prefix && valid_end < INBUF_SIZE && (need_refill || (INBUF_SIZE - valid_end) >= 65536)) {
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

        size_t fresh_bytes = 0;
        bool at_true_eof = false;
        if (use_webm_demux) {
            if (data_start >= valid_end && valid_end < INBUF_SIZE) {
                data_start = 0;
                valid_end = 0;
                fresh_bytes = webm_opus_read_one_packet(fp, &webm_demux, inbuf, INBUF_SIZE);
                valid_end = fresh_bytes;
                at_true_eof = (fresh_bytes == 0);
            }
        } else if (valid_end < INBUF_SIZE && (need_refill || (INBUF_SIZE - valid_end) >= 65536 || (valid_end - data_start) == 0)) {
            need_refill = false;
            if (!use_async && sd_card_get_frequency() != SDMMC_FREQ_HIGHSPEED) {
                sd_card_set_frequency(SDMMC_FREQ_HIGHSPEED);
            }
            size_t space = INBUF_SIZE - valid_end;
            size_t to_read = space;
            if (!use_frag_demux && !use_async) {
                long cur_pos = ftell(fp);
                if (cur_pos >= 0) {
                    size_t misalign = (size_t)(cur_pos % 512);
                    if (misalign != 0) {
                        size_t align_bytes = 512 - misalign;
                        if (space >= align_bytes) {
                            to_read = align_bytes + ((space - align_bytes) & ~511UL);
                        }
                    } else {
                        to_read = space & ~511UL;
                    }
                }
            }
            if (to_read > 0) {
                int64_t t_r0 = esp_timer_get_time();
                if (use_async) {
                    fresh_bytes = async_reader_read(inbuf + valid_end, to_read, 1500);
                    at_true_eof = (fresh_bytes == 0) && async_reader_is_eof() && (from_m4a_prefix == 0);
                } else {
                    fresh_bytes = use_frag_demux ? m4a_frag_read(fp, &frag_demux, inbuf + valid_end, to_read)
                                                  : fread(inbuf + valid_end, 1, to_read, fp);
                    at_true_eof = (fresh_bytes == 0) && (from_m4a_prefix == 0);
                }
                prof_t_read_us += (esp_timer_get_time() - t_r0);
                prof_read_bytes += fresh_bytes;
                prof_read_count++;
                valid_end += fresh_bytes;
            }
        }

        size_t valid = valid_end - data_start;
        if (valid == 0) {
            if (at_true_eof) {
                reached_eof = true;
            } else {
                ESP_LOGW(TAG, "Buffer esgotado mas at_true_eof falso (timeout de leitura SDMMC); abortando faixa");
            }
            break;
        }

        size_t bytes_consumed = 0;
        size_t samples_decoded = 0;

        int64_t t_d0 = esp_timer_get_time();
        mps3::DecodeStatus result = decoder->decode(inbuf + data_start, valid,
                                                      output_buf, output_capacity_samples,
                                                      bytes_consumed, samples_decoded);
        prof_t_decode_us += (esp_timer_get_time() - t_d0);

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
                reached_eof = true;
                break;
            }
        } else {
            stuck_at_eof_count = 0;
        }

        switch (result) {
            case mps3::DecodeStatus::HeaderReady: {
                sample_rate = decoder->sample_rate();
                num_channels = decoder->channels();
                uint32_t dec_bps = decoder->bits_per_sample();
                bool hdr_unsupported = false;
                char hdr_err[32] = {0};
                if (fmt_detected == mps3::AudioFormat::Flac) {
                    if (dec_bps > 24 || sample_rate > 192000) {
                        hdr_unsupported = true;
                        snprintf(hdr_err, sizeof(hdr_err), "FLAC max 24b/192k");
                    }
                } else if (fmt_detected == mps3::AudioFormat::Wav) {
                    if (dec_bps > 32 || sample_rate > 384000) {
                        hdr_unsupported = true;
                        snprintf(hdr_err, sizeof(hdr_err), "WAV max 32b/384k");
                    }
                } else {
                    if (dec_bps > 24 || sample_rate > 192000) {
                        hdr_unsupported = true;
                        snprintf(hdr_err, sizeof(hdr_err), "Max 192kHz/24b");
                    }
                }
                if (hdr_unsupported) {
                    ESP_LOGE(TAG, "Decoder detectou taxa/bits (%s) acima do limite: %u Hz / %u bits (%s)",
                             display_name, (unsigned)sample_rate, (unsigned)dec_bps, hdr_err);
                    state_lock();
                    snprintf(s_state.last_error, sizeof(s_state.last_error), "%s", hdr_err);
                    s_state.playing = false;
                    state_unlock();
                    clear_resume_state_if_matches(metrics_rel_path);
                    done = true;
                    break;
                }
                output_capacity_samples = decoder->recommended_output_capacity_samples();
                if (output_buf) heap_caps_free(output_buf);
                output_buf = (int32_t *)heap_caps_malloc(output_capacity_samples * sizeof(int32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!output_buf) {
                    output_buf = (int32_t *)heap_caps_malloc(output_capacity_samples * sizeof(int32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
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
                bool gapless_keep_stream = s_gapless_enabled && s_gapless_transition_active &&
                                           (i2s_rate == s_dsp_current_rate) && (resume_elapsed_sec == 0);
                s_gapless_transition_active = false;
                if (gapless_keep_stream) {
                    ESP_LOGI(TAG, "Transicao Gapless Real ativa em %u Hz (fila DSP preservada sem drain/flush)", (unsigned)i2s_rate);
                } else {
                    ESP_LOGI(TAG, "Saida I2S configurada para taxa nativa: %u Hz (Dual-Core: Core 1 decodifica, Core 0 processa DSP/I2S)", (unsigned)i2s_rate);
                    audio_dsp_flush();
                    audio_dsp_set_rate(i2s_rate);
                    if (s_soft_fade_enabled) {
                        audio_dsp_trigger_fade_in(i2s_rate);
                    }
                }

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
                static bool s_first_track_loaded = false;
                if (!s_first_track_loaded) {
                    s_first_track_loaded = true;
                    printf("\n@METRIC: FAIXA CARREGADA NO PLAYER: '%s' (%u ms)\n", s_state.filename, (unsigned)(esp_timer_get_time() / 1000ULL)); fflush(stdout);
                }
                state_unlock();

                const char *display_format = (s_state.format_name[0] != '\0') ? s_state.format_name : label;
                ESP_LOGI(TAG, "%s: %u Hz, %u canal(is), %u bits, %s%u s",
                         display_format, (unsigned)sample_rate, (unsigned)num_channels,
                         (unsigned)s_state.bits_per_sample,
                         meta.duration_is_estimate ? "~" : "", (unsigned)total_sec);
                header_ready = true;
                save_resume_state(metrics_rel_path, resume_elapsed_sec);
                break;
            }

            case mps3::DecodeStatus::Success: {
                if (!header_ready || samples_decoded == 0) break;

                elapsed_samples += samples_decoded / num_channels;
                uint32_t elapsed_sec = (sample_rate > 0)
                    ? (uint32_t)(elapsed_samples / sample_rate) : 0;

                bool seeking = s_seek_pending && elapsed_sec < s_seek_target_sec;
                if (seeking) {
                    break;
                }
                if (s_seek_pending) {
                    s_seek_pending = false;
                    audio_dsp_flush();
                    if (s_soft_fade_enabled) {
                        audio_dsp_trigger_fade_in(sample_rate);
                    }
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
                    session_decoded_samples += (samples_to_write / 2);
                    int64_t t_s0 = esp_timer_get_time();
                    audio_dsp_send_pcm(to_write, samples_to_write, sample_rate);
                    prof_t_dsp_us += (esp_timer_get_time() - t_s0);
                    user_metrics_add_listened_frames((uint32_t)(samples_to_write / 2), sample_rate, s_volume_percent);
                }

                if (!prebuffer_done) {
                    prebuffered_frames += (samples_to_write / 2);
                    if (prebuffered_frames >= prebuffer_target_frames) {
                        prebuffer_done = true;
                    }
                }

                int64_t now_prof_us = esp_timer_get_time();
                if ((now_prof_us - prof_last_log_us) >= 3000000ULL) {
                    prof_last_log_us = now_prof_us;
                    uint32_t free_blk = s_dsp_free_queue ? (uint32_t)uxQueueMessagesWaiting(s_dsp_free_queue) : 0;
                    uint32_t rdy_blk = s_dsp_ready_queue ? (uint32_t)uxQueueMessagesWaiting(s_dsp_ready_queue) : 0;
                    size_t ring_avail = use_async ? async_reader_available() : 0;
                    ESP_LOGI("PROF_CORE1", "read: %u ms (%u KB, %u calls) | dec: %u ms | dsp_send: %u ms | memmove: %u ms | queues(rdy=%u/%u, free=%u/%u) | ring: %u/%u KB",
                             (unsigned)(prof_t_read_us / 1000ULL),
                             (unsigned)(prof_read_bytes / 1024U),
                             (unsigned)prof_read_count,
                             (unsigned)(prof_t_decode_us / 1000ULL),
                             (unsigned)(prof_t_dsp_us / 1000ULL),
                             (unsigned)(prof_t_memmove_us / 1000ULL),
                             (unsigned)rdy_blk, DSP_NUM_BLOCKS, (unsigned)free_blk, DSP_NUM_BLOCKS,
                             (unsigned)(ring_avail / 1024U), (unsigned)(s_async_reader.capacity / 1024U));
                    prof_t_read_us = 0;
                    prof_t_decode_us = 0;
                    prof_t_dsp_us = 0;
                    prof_t_memmove_us = 0;
                    prof_read_bytes = 0;
                    prof_read_count = 0;
                }

                // O prefetch da proxima faixa so e iniciado nos ultimos 4 segundos da faixa atual,
                // quando a decodificacao ja preencheu a fila DSP com folga, evitando qualquer
                // concorrencia de barramento SDMMC e CPU no Core 1 durante a reproducao ativa.
                if (prebuffer_done) {
                    bool safe_to_prefetch = (meta.total_sec > 0 && (meta.total_sec - elapsed_sec) <= 4 &&
                                             uxQueueMessagesWaiting(s_dsp_ready_queue) >= (DSP_NUM_BLOCKS / 2)) ||
                                            (meta.total_sec > 0 && meta.total_sec <= 5 &&
                                             session_decoded_samples >= (uint64_t)sample_rate * 2);

                    if (!prefetched_next && safe_to_prefetch && s_shuffle_mode != AUDIO_SHUFFLE_ALL) {
                        prefetched_next = true;
                        scan_lock();
                        int count_for_prefetch = s_playback_scan.audio_count;
                        scan_unlock();
                        if (count_for_prefetch > 1) {
                            int next_index = (index + 1) % count_for_prefetch;
                            if (s_loop_mode == AUDIO_LOOP_TRACK) {
                                if (s_loop_track_remaining > 0) {
                                    next_index = index; // Prefetch da propria faixa para repeticao gapless
                                } else {
                                    next_index = (index + 1) % count_for_prefetch;
                                }
                            } else if (s_shuffle_mode == AUDIO_SHUFFLE_FOLDER && s_shuffle_indices && s_shuffle_count == count_for_prefetch) {
                                int n_pos = (s_shuffle_pos + 1) % s_shuffle_count;
                                next_index = s_shuffle_indices[n_pos];
                            }
                            prefetch_next_file(next_index);
                        }
                    }

                    state_lock();
                    if (!s_forced_restart_pending) {
                        s_state.elapsed_sec = elapsed_sec;
                    }
                    state_unlock();
                    if (!s_forced_restart_pending) {
                        s_last_elapsed_sec = elapsed_sec;
                    }

                    if (!marked_completed && meta.total_sec > 0 && elapsed_sec >= (meta.total_sec * 90) / 100) {
                        marked_completed = true;
                        if (s_track_completed_cb) {
                            char rel[PATH_LEN];
                            build_relative_path(s_playback_dir, display_name, rel, sizeof(rel));
                            s_track_completed_cb(rel, elapsed_sec, meta.total_sec);
                        }
                    }

                    // O tempo decorrido ja e atualizado em RAM (s_last_elapsed_sec) a cada segundo.
                    // Nao executamos save_resume_state() sincronamente a cada 30 s para evitar
                    // travamentos de cache flash causados por nvs_commit() no meio de reproducoes Hi-Res.
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
                    need_refill = true;
                } else if (valid_end < INBUF_SIZE) {
                    need_refill = true;
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
                reached_eof = true;
                done = true;
                break;

            default:
                ESP_LOGE(TAG, "Erro de decodificacao (codigo %d)", (int)result);
                done = true;
                break;
        }

        // Protecao contra travamento de CPU e Task Watchdog:
        // Se a volta do loop nao gerou saida de audio (ex: pedindo mais dados, decodificando
        // metadados ou frames vazios), cede brevemente a CPU para que IDLE1 e outras tarefas rodem.
        if (result == mps3::DecodeStatus::Success && samples_decoded > 0) {
            zero_samples_loop_count = 0;
        } else {
            zero_samples_loop_count++;
            if (zero_samples_loop_count >= 5) {
                vTaskDelay(pdMS_TO_TICKS(1)); // Cede a CPU para IDLE1 alimentar o Task Watchdog
            }
            if (zero_samples_loop_count >= 100) {
                ESP_LOGW(TAG, "Decoder sem progresso por 100 ciclos consecutivos; abortando faixa.");
                clear_resume_state_if_matches(metrics_rel_path);
                done = true;
                break;
            }
        }
    }

    s_dsp_stream_ended = true;

    bool threshold_reached = (meta.total_sec > 0 && s_last_elapsed_sec >= (meta.total_sec * 90) / 100);
    if (!marked_completed && (reached_eof || threshold_reached) && s_last_elapsed_sec > 0) {
        marked_completed = true;
        if (s_track_completed_cb) {
            char rel[PATH_LEN];
            build_relative_path(s_playback_dir, display_name, rel, sizeof(rel));
            s_track_completed_cb(rel, s_last_elapsed_sec, meta.total_sec);
        }
    }

    if (reached_eof || threshold_reached) {
        bookmarks_remove(metrics_rel_path);
    } else if (s_last_elapsed_sec >= 5 && should_bookmark_track(metrics_rel_path, meta.total_sec)) {
        bookmarks_set(metrics_rel_path, s_last_elapsed_sec, meta.total_sec);
    }

    scan_lock();
    int cur_audio_cnt = s_playback_scan.audio_count;
    scan_unlock();
    bool is_seek_restart = (s_pending_cmd == PLAYER_CMD_RESTART && s_forced_restart_pending);
    bool can_gapless_next = s_gapless_enabled && reached_eof &&
                            (s_pending_cmd == PLAYER_CMD_NONE) &&
                            !s_usb_takeover_requested && (cur_audio_cnt > 1) &&
                            (s_shuffle_mode != AUDIO_SHUFFLE_ALL);
    if (can_gapless_next) {
        s_gapless_transition_active = true;
    } else {
        s_gapless_transition_active = false;
        if (is_seek_restart) {
            audio_dsp_flush();
        } else {
            audio_dsp_drain();
        }
    }

    {
        const char *end_reason = "stop";
        if (reached_eof) {
            end_reason = "eof";
        } else if (s_pending_cmd == PLAYER_CMD_NEXT) {
            end_reason = "skip_next";
        } else if (s_pending_cmd == PLAYER_CMD_PREV) {
            end_reason = "skip_prev";
        } else if (s_pending_cmd == PLAYER_CMD_PLAY_INDEX) {
            end_reason = "select";
        } else if (s_pending_cmd == PLAYER_CMD_RESTART) {
            end_reason = "repeat";
        }

        user_metrics_end_info_t end_info = {};
        end_info.rel_path          = metrics_rel_path;
        end_info.title             = s_state.title;
        end_info.artist            = s_state.artist;
        end_info.album             = s_state.album;
        end_info.genre             = meta.genre;
        end_info.release_year      = meta.release_year;
        end_info.codec             = s_state.format_name;
        end_info.sample_rate       = s_state.sample_rate;
        end_info.bits_per_sample   = s_state.bits_per_sample;
        end_info.bitrate_bps       = s_state.bitrate;
        end_info.duration_sec      = meta.total_sec;
        end_info.final_elapsed_sec = s_last_elapsed_sec;
        end_info.end_reason        = end_reason;
        user_metrics_on_track_end(&end_info, is_seek_restart);
    }

    if (use_async) {
        async_reader_stop();
    }

    if (output_buf) heap_caps_free(output_buf);
    if (pending_m4a_prefix) free(pending_m4a_prefix);
    if (use_frag_demux) m4a_frag_demuxer_free(&frag_demux);
    heap_caps_free(inbuf);
    fclose(fp);

    state_lock();
    if (!is_seek_restart) {
        s_state.playing = false;
    }
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
                vTaskDelay(pdMS_TO_TICKS(500));
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
        push_shuffle_history(s_playback_dir, current_index);

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

        // Se shuffle de pasta ativo e contagem mudou, atualiza vetor
        if (s_shuffle_mode == AUDIO_SHUFFLE_FOLDER && (s_shuffle_indices == NULL || s_shuffle_count != count_for_switch)) {
            update_folder_shuffle(count_for_switch, current_index);
        }

        switch (cmd) {
            case PLAYER_CMD_NEXT:
                state_lock();
                s_forced_restart_pending = false;
                state_unlock();
                if (s_loop_mode == AUDIO_LOOP_TRACK) {
                    s_loop_mode = AUDIO_LOOP_NONE;
                    s_loop_track_remaining = 0;
                    state_lock();
                    s_state.loop_mode = AUDIO_LOOP_NONE;
                    s_state.loop_track_repeat_count = 0;
                    state_unlock();
                    save_audio_prefs_nvs();
                }
                if (s_shuffle_mode == AUDIO_SHUFFLE_FOLDER && s_shuffle_indices && s_shuffle_count == count_for_switch) {
                    if (s_shuffle_pos + 1 >= s_shuffle_count) {
                        update_folder_shuffle(count_for_switch, current_index);
                        s_shuffle_pos = (s_shuffle_count > 1) ? 1 : 0;
                    } else {
                        s_shuffle_pos++;
                    }
                    current_index = s_shuffle_indices[s_shuffle_pos];
                } else if (s_shuffle_mode == AUDIO_SHUFFLE_ALL) {
                    char rand_dir[DIR_LEN];
                    int rand_idx = 0;
                    if (pick_random_track_sd(rand_dir, sizeof(rand_dir), &rand_idx)) {
                        strlcpy(s_playback_dir, rand_dir, sizeof(s_playback_dir));
                        scan_dir(s_playback_dir, s_playback_scan);
                        count_for_switch = s_playback_scan.audio_count;
                        current_index = (rand_idx < count_for_switch) ? rand_idx : 0;
                    } else {
                        current_index = (current_index + 1) % count_for_switch;
                    }
                } else {
                    current_index = (current_index + 1) % count_for_switch;
                }
                break;

            case PLAYER_CMD_PREV:
                state_lock();
                s_forced_restart_pending = false;
                state_unlock();
                if (s_loop_mode == AUDIO_LOOP_TRACK) {
                    s_loop_mode = AUDIO_LOOP_NONE;
                    s_loop_track_remaining = 0;
                    state_lock();
                    s_state.loop_mode = AUDIO_LOOP_NONE;
                    s_state.loop_track_repeat_count = 0;
                    state_unlock();
                    save_audio_prefs_nvs();
                }
                if (s_shuffle_mode == AUDIO_SHUFFLE_FOLDER && s_shuffle_indices && s_shuffle_count == count_for_switch) {
                    s_shuffle_pos = (s_shuffle_pos - 1 + s_shuffle_count) % s_shuffle_count;
                    current_index = s_shuffle_indices[s_shuffle_pos];
                } else if (s_shuffle_mode == AUDIO_SHUFFLE_ALL) {
                    char hist_dir[DIR_LEN];
                    int hist_idx = 0;
                    if (pop_shuffle_history(hist_dir, sizeof(hist_dir), &hist_idx)) {
                        if (strcmp(s_playback_dir, hist_dir) != 0) {
                            strlcpy(s_playback_dir, hist_dir, sizeof(s_playback_dir));
                            scan_dir(s_playback_dir, s_playback_scan);
                            count_for_switch = s_playback_scan.audio_count;
                        }
                        current_index = (hist_idx < count_for_switch) ? hist_idx : 0;
                    } else {
                        current_index = (current_index - 1 + count_for_switch) % count_for_switch;
                    }
                } else {
                    current_index = (current_index - 1 + count_for_switch) % count_for_switch;
                }
                break;

            case PLAYER_CMD_RESTART:
                break; // mesmo indice: play_track vai tocar do zero

            case PLAYER_CMD_PLAY_INDEX:
                if (pending_idx >= 0 && pending_idx < count_for_switch) {
                    reselect_resume_pending = (pending_idx == current_index) &&
                                               (strcmp(s_playback_dir, prev_playback_dir) == 0);
                    current_index = pending_idx;
                    if (s_shuffle_mode == AUDIO_SHUFFLE_FOLDER && s_shuffle_indices && s_shuffle_count == count_for_switch) {
                        for (int i = 0; i < s_shuffle_count; i++) {
                            if (s_shuffle_indices[i] == (uint16_t)pending_idx) {
                                s_shuffle_pos = i;
                                break;
                            }
                        }
                    }
                }
                break;

            case PLAYER_CMD_PLAY_URL:
                s_pending_cmd = PLAYER_CMD_PLAY_URL;
                break;

            case PLAYER_CMD_STOP_URL:
                s_pending_cmd = PLAYER_CMD_STOP_URL;
                break;

            case PLAYER_CMD_NONE:
            default:
                // Fim natural da faixa
                if (s_loop_mode == AUDIO_LOOP_TRACK) {
                    if (s_loop_track_remaining > 0) {
                        // Tocou 1x: agora entra na repeticao unica! O "1" some no display e vira repeat simples
                        s_loop_track_remaining = 0;
                        state_lock();
                        s_state.loop_track_repeat_count = 0;
                        state_unlock();
                        // Mantem exatamente o mesmo current_index
                    } else {
                        // Ja tocou a repeticao: desliga o loop e avanca normalmente!
                        s_loop_mode = AUDIO_LOOP_NONE;
                        s_loop_track_remaining = 0;
                        state_lock();
                        s_state.loop_mode = AUDIO_LOOP_NONE;
                        s_state.loop_track_repeat_count = 0;
                        state_unlock();
                        save_audio_prefs_nvs();

                        if (s_shuffle_mode == AUDIO_SHUFFLE_FOLDER && s_shuffle_indices && s_shuffle_count == count_for_switch) {
                            s_shuffle_pos = (s_shuffle_pos + 1) % s_shuffle_count;
                            current_index = s_shuffle_indices[s_shuffle_pos];
                        } else if (s_shuffle_mode == AUDIO_SHUFFLE_ALL) {
                            char rand_dir[DIR_LEN];
                            int rand_idx = 0;
                            if (pick_random_track_sd(rand_dir, sizeof(rand_dir), &rand_idx)) {
                                strlcpy(s_playback_dir, rand_dir, sizeof(s_playback_dir));
                                scan_dir(s_playback_dir, s_playback_scan);
                                count_for_switch = s_playback_scan.audio_count;
                                current_index = (rand_idx < count_for_switch) ? rand_idx : 0;
                            } else {
                                current_index = (current_index + 1) % count_for_switch;
                            }
                        } else {
                            if (current_index + 1 >= count_for_switch) {
                                s_paused = true;
                                state_lock();
                                s_state.playing = false;
                                s_state.paused = true;
                                state_unlock();
                                i2s_output_disable();
                            } else {
                                current_index = current_index + 1;
                            }
                        }
                    }
                } else if (s_shuffle_mode == AUDIO_SHUFFLE_FOLDER && s_shuffle_indices && s_shuffle_count == count_for_switch) {
                    if (s_shuffle_pos + 1 >= s_shuffle_count && s_loop_mode == AUDIO_LOOP_NONE) {
                        // Fim de todas as faixas da pasta sem repeticao -> Pausa
                        s_paused = true;
                        state_lock();
                        s_state.playing = false;
                        s_state.paused = true;
                        state_unlock();
                        i2s_output_disable();
                    } else {
                        if (s_shuffle_pos + 1 >= s_shuffle_count) {
                            update_folder_shuffle(count_for_switch, current_index);
                            s_shuffle_pos = (s_shuffle_count > 1) ? 1 : 0;
                        } else {
                            s_shuffle_pos++;
                        }
                        current_index = s_shuffle_indices[s_shuffle_pos];
                    }
                } else if (s_shuffle_mode == AUDIO_SHUFFLE_ALL) {
                    char rand_dir[DIR_LEN];
                    int rand_idx = 0;
                    if (pick_random_track_sd(rand_dir, sizeof(rand_dir), &rand_idx)) {
                        strlcpy(s_playback_dir, rand_dir, sizeof(s_playback_dir));
                        scan_dir(s_playback_dir, s_playback_scan);
                        count_for_switch = s_playback_scan.audio_count;
                        current_index = (rand_idx < count_for_switch) ? rand_idx : 0;
                    } else {
                        current_index = (current_index + 1) % count_for_switch;
                    }
                } else {
                    // Ordem Linear Convencional
                    if (current_index + 1 >= count_for_switch && s_loop_mode == AUDIO_LOOP_NONE) {
                        // Atingiu ultima faixa e Loop esta desativado: para reproducao
                        s_paused = true;
                        state_lock();
                        s_state.playing = false;
                        s_state.paused = true;
                        state_unlock();
                        i2s_output_disable();
                    } else {
                        current_index = (current_index + 1) % count_for_switch;
                    }
                }
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
    load_audio_prefs_nvs();
    bookmarks_init();

    strncpy(s_root_dir, music_dir, sizeof(s_root_dir) - 1);
    s_root_dir[sizeof(s_root_dir) - 1] = '\0';

    strncpy(s_browse_dir, s_root_dir, sizeof(s_browse_dir) - 1);
    s_browse_dir[sizeof(s_browse_dir) - 1] = '\0';
    strncpy(s_playback_dir, s_root_dir, sizeof(s_playback_dir) - 1);
    s_playback_dir[sizeof(s_playback_dir) - 1] = '\0';

    ensure_playlists_dir_and_migration();

    s_state_mutex = xSemaphoreCreateMutex();
    if (!s_state_mutex) {
        return ESP_ERR_NO_MEM;
    }
    s_scan_mutex = xSemaphoreCreateMutex();
    if (!s_scan_mutex) {
        return ESP_ERR_NO_MEM;
    }

    if (sd_card_get_frequency() != SDMMC_FREQ_HIGHSPEED) {
        sd_card_set_frequency(SDMMC_FREQ_HIGHSPEED);
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

    BaseType_t volume_task_ok = xTaskCreatePinnedToCore(volume_persistence_task, "volume_nvs",
                                             4096, NULL, 2, NULL, 0);
    if (volume_task_ok != pdPASS) return ESP_ERR_NO_MEM;

    esp_err_t dsp_err = audio_dsp_init();
    if (dsp_err != ESP_OK) return dsp_err;

    async_reader_init();

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

bool audio_player_like_current(bool like, bool *out_status)
{
    if (out_status) *out_status = false;
    if (s_usb_takeover_requested) return false;

    char full_track_path[MAX_PATH_LEN] = {0};
    scan_lock();
    if (s_current_index >= 0 && s_current_index < s_playback_scan.audio_count) {
        resolve_track_full_path(s_playback_dir, s_playback_scan.audio_files[s_current_index],
                                full_track_path, sizeof(full_track_path));
    }
    scan_unlock();

    if (full_track_path[0] == '\0') return false;

    char pl_path[MAX_PATH_LEN] = {0};
    if (!get_target_playlist_abs_path(pl_path, sizeof(pl_path))) return false;
    bool res = modify_single_track_in_playlist_file(pl_path, full_track_path, like ? 1 : -1, out_status);
    if (res) {
        state_lock();
        s_state.is_favorite = (out_status != NULL) ? *out_status : like;
        state_unlock();
    }
    return res;
}

bool audio_player_like_browse_item(int browse_idx, bool like, bool *out_status)
{
    if (out_status) *out_status = false;
    if (s_usb_takeover_requested) return false;

    char pl_path[MAX_PATH_LEN] = {0};
    if (!get_target_playlist_abs_path(pl_path, sizeof(pl_path))) return false;

    char item_abs[MAX_PATH_LEN] = {0};
    bool is_folder = false;

    scan_lock();
    int offset = browse_has_parent() ? 1 : 0;
    int idx = browse_idx - offset;
    if (idx >= 0 && idx < s_browse_scan.subdir_count) {
        const char *sub = s_browse_scan.subdirs[idx];
        const char *clean_sub = (strncmp(sub, "[PL] ", 5) == 0) ? (sub + 5) : sub;
        snprintf(item_abs, sizeof(item_abs), "%s/%s", s_browse_dir, clean_sub);
        is_folder = !is_playlist_extension(clean_sub);
    } else {
        int a_idx = idx - s_browse_scan.subdir_count;
        if (a_idx >= 0 && a_idx < s_browse_scan.audio_count) {
            resolve_track_full_path(s_browse_dir, s_browse_scan.audio_files[a_idx], item_abs, sizeof(item_abs));
            is_folder = false;
        }
    }
    scan_unlock();

    if (item_abs[0] == '\0') return false;

    if (is_folder) {
        if (like) {
            int added = 0;
            bool ok = add_folder_tracks_to_playlist_file(pl_path, item_abs, &added);
            if (out_status) *out_status = (added > 0);
            return ok;
        }
        return false;
    }

    return modify_single_track_in_playlist_file(pl_path, item_abs, like ? 1 : -1, out_status);
}

void audio_player_get_browse_dir(char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    scan_lock();
    strlcpy(out, s_browse_dir, out_len);
    scan_unlock();
}

void audio_player_rescan_browser(void)
{
    scan_lock();
    scan_dir(s_browse_dir, s_browse_scan);
    scan_unlock();
}

void audio_player_browse_to_root(void)
{
    scan_lock();
    strlcpy(s_browse_dir, s_root_dir, sizeof(s_browse_dir));
    scan_dir(s_browse_dir, s_browse_scan);
    scan_unlock();
}

static bool is_track_in_playlist_file(const char *pl_abs_path, const char *full_track_path)
{
    if (!pl_abs_path || !full_track_path) return false;
    FILE *fp = fopen(pl_abs_path, "r");
    if (!fp) return false;
    char buf[512];
    bool found = false;
    while (fgets(buf, sizeof(buf), fp) != NULL) {
        size_t l = strlen(buf);
        while (l > 0 && (buf[l - 1] == '\r' || buf[l - 1] == '\n')) buf[--l] = '\0';
        if (l == 0 || buf[0] == '#') continue;
        if (strcasecmp(buf, full_track_path) == 0) {
            found = true;
            break;
        }
    }
    fclose(fp);
    return found;
}

bool audio_player_is_current_favorite(void)
{
    if (s_usb_takeover_requested) return false;
    char full_track_path[MAX_PATH_LEN] = {0};
    scan_lock();
    if (s_current_index >= 0 && s_current_index < s_playback_scan.audio_count) {
        resolve_track_full_path(s_playback_dir, s_playback_scan.audio_files[s_current_index],
                                full_track_path, sizeof(full_track_path));
    }
    scan_unlock();
    if (full_track_path[0] == '\0') return false;

    char pl_path[MAX_PATH_LEN] = {0};
    if (!get_target_playlist_abs_path(pl_path, sizeof(pl_path))) return false;
    return is_track_in_playlist_file(pl_path, full_track_path);
}

bool audio_player_is_browse_item_favorite(int browse_idx)
{
    if (s_usb_takeover_requested) return false;
    char pl_path[MAX_PATH_LEN] = {0};
    if (!get_target_playlist_abs_path(pl_path, sizeof(pl_path))) return false;

    char item_abs[MAX_PATH_LEN] = {0};
    scan_lock();
    int offset = browse_has_parent() ? 1 : 0;
    int idx = browse_idx - offset;
    if (idx >= 0 && idx < s_browse_scan.subdir_count) {
        scan_unlock();
        return false;
    } else {
        int a_idx = idx - s_browse_scan.subdir_count;
        if (a_idx >= 0 && a_idx < s_browse_scan.audio_count) {
            resolve_track_full_path(s_browse_dir, s_browse_scan.audio_files[a_idx], item_abs, sizeof(item_abs));
        }
    }
    scan_unlock();

    if (item_abs[0] == '\0') return false;
    return is_track_in_playlist_file(pl_path, item_abs);
}
