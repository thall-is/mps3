#include "touch_input.h"
#include "pinos.h"
#include "audio_player.h"
#include "menu.h"
#include "oled_display.h"
#include "rgb_led.h"
#include "usb_manager.h"
#include "i2s_output.h"
#include "podcast_sync.h"
#include "wifi_transfer.h"
#include "pwr_governor.h"
#include "debug_nav.h"

#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "touch_input";

// Banda selecionada no modo EQ (0=baixo, 1=mid, 2=mid-high, 3=treble, 4=geral)
// Total de 5 itens seleccionÃ¡veis (4 bandas + 1 ganho geral)
#define EQ_NUM_SELECTABLE 5
static int s_eq_band = 0;
static int s_top_cursor = 0;
static int s_top_eq_focus = 0;
static int s_tela_cursor = 0;
static int s_timeout_idx = 0;
#define FALLBACK_OPTIONS_COUNT 6
static const int FALLBACK_SECS[FALLBACK_OPTIONS_COUNT] = {0, 5, 10, 15, 30, 60};
static int s_fallback_idx = 1; // Padrao: 5 segundos (0 = desativado)
static bool s_is_sleeping = false;
static int s_led_channel = 0;
static uint8_t s_oled_brightness = 255;
static const uint8_t BRIGHTNESS_CURVE[] = {1, 3, 7, 15, 30, 50, 80, 120, 180, 255};

#define DEEPSLEEP_OPTIONS_COUNT 7
static const int DEEPSLEEP_MINS[DEEPSLEEP_OPTIONS_COUNT] = {0, 1, 3, 5, 10, 15, 30};
static int s_deepsleep_idx = 3; // Padrao: 5 minutos
static uint32_t s_last_interaction_ms = 0;

#define LOCK_CLOCK_OPTIONS_COUNT 6
static const int LOCK_CLOCK_SECS[LOCK_CLOCK_OPTIONS_COUNT] = {0, 3, 5, 8, 10, 15};
static int s_lock_clock_timeout_idx = 2; // Padrao: 5 segundos (0 = desativado)
static uint32_t s_lock_clock_until_ms = 0;

// Estado do Cronometro (Stopwatch)
static bool s_chrono_running = false;
static int64_t s_chrono_start_us = 0;
static int64_t s_chrono_accum_us = 0;
static int s_chrono_lap_count = 0;
static uint32_t s_chrono_laps[8] = {0};

bool touch_input_chrono_is_running(void) { return s_chrono_running; }

uint32_t touch_input_chrono_get_elapsed_ms(void)
{
    int64_t total = s_chrono_accum_us;
    if (s_chrono_running) {
        total += (esp_timer_get_time() - s_chrono_start_us);
    }
    return (uint32_t)(total / 1000);
}

int touch_input_chrono_get_laps(uint32_t *out_laps, int max_laps)
{
    if (!out_laps || max_laps <= 0) return 0;
    int cnt = (s_chrono_lap_count > max_laps) ? max_laps : s_chrono_lap_count;
    for (int i = 0; i < cnt; i++) out_laps[i] = s_chrono_laps[i];
    return s_chrono_lap_count;
}

void touch_input_chrono_toggle(void)
{
    int64_t now_us = esp_timer_get_time();
    if (s_chrono_running) {
        s_chrono_accum_us += (now_us - s_chrono_start_us);
        s_chrono_running = false;
    } else {
        s_chrono_start_us = now_us;
        s_chrono_running = true;
    }
}

void touch_input_chrono_lap_or_reset(void)
{
    if (s_chrono_running) {
        if (s_chrono_lap_count < 8) {
            uint32_t cur_ms = touch_input_chrono_get_elapsed_ms();
            s_chrono_laps[s_chrono_lap_count++] = cur_ms;
        }
    } else {
        s_chrono_accum_us = 0;
        s_chrono_lap_count = 0;
    }
}

int touch_input_get_lock_clock_timeout_idx(void) { return s_lock_clock_timeout_idx; }

uint32_t touch_input_get_lock_clock_secs(void)
{
    if (s_lock_clock_timeout_idx <= 0 || s_lock_clock_timeout_idx >= LOCK_CLOCK_OPTIONS_COUNT) return 0;
    return (uint32_t)LOCK_CLOCK_SECS[s_lock_clock_timeout_idx];
}

bool touch_input_is_lock_clock_visible(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t lk_secs = touch_input_get_lock_clock_secs();
    if (lk_secs == 0) return false;

    // 1. Temporizador ativo (ao acordar do repouso ou bloquear manualmente)
    if (s_lock_clock_until_ms > now) {
        return true;
    }

    // 2. Pre-bloqueio por inatividade
    int t_idx = s_timeout_idx;
    if (t_idx > 0 && t_idx <= 5) {
        static const int TIMEOUT_SECS[] = {0, 15, 30, 60, 120, 300};
        uint32_t total_timeout_ms = (uint32_t)TIMEOUT_SECS[t_idx] * 1000UL;
        uint32_t pre_lock_ms = lk_secs * 1000UL;
        if (total_timeout_ms > pre_lock_ms) {
            uint32_t pre_trigger_ms = total_timeout_ms - pre_lock_ms;
            if (!s_is_sleeping && (now - s_last_interaction_ms) >= pre_trigger_ms &&
                (now - s_last_interaction_ms) < total_timeout_ms) {
                return true;
            }
        }
    }
    return false;
}

static void save_deepsleep_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open("mps3_settings", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "deepsleep", (uint8_t)s_deepsleep_idx);
        nvs_commit(h);
        nvs_close(h);
    }
}
int touch_input_get_led_channel(void) { return s_led_channel; }
uint8_t touch_input_get_oled_brightness(void) { return s_oled_brightness; } // banda atualmente selecionada
// Passo de ajuste por pressÃ£o de JOY_UP/DOWN no modo EQ (em dB)
#define EQ_STEP_DB 1.0f

int touch_input_get_eq_band(void) { return s_eq_band; }

static int s_wifi_net_cursor = 0;
int touch_input_get_wifi_net_cursor(void) { return s_wifi_net_cursor; }

static bool s_in_player_browser = false; // true = navegando pastas a partir do Player
bool touch_input_is_in_player_browser(void) { return s_in_player_browser; }

#define VOLUME_STEP_PERCENT  5

// Clique longo do botao central alterna o bloqueio. E' bem distinto de um
// clique curto de play/pause, sem deixar o bloqueio lento de usar.
#define CENTER_LOCK_HOLD_MS 700

// Amostragem do joystick a ~28.5 Hz (35 ms): resposta tátil instantânea ao operador,
// reduzindo as interrupções de CPU no Core 0 em 28%.
#define POLL_INTERVAL_MS 35

// Segurar JOY_LEFT por esse tempo, com um modo de tela cheia (USB/WiFi)
// ativo, pede pra sair dele. Antes era "qualquer toque ou botao" - trocado
// pra' um gesto deliberado (segurar ESQUERDA) pra nao sair sem querer.
#define EXIT_ACTIVE_MODE_HOLD_MS 1000

// --- Joystick: 5 botoes que aterram a linha quando pressionados --------
#define NUM_JOY_BTNS 5
enum { JOY_UP = 0, JOY_LEFT, JOY_DOWN, JOY_RIGHT, JOY_CENTER };

// Cima/baixo repetem enquanto segurados (rolagem rapida em listas
// longas): primeiro passo no toque, repete no intervalo abaixo depois do
// atraso inicial. Esquerda/direita/centro sao so' de "borda" - 1 acao
// por aperto, sem repeticao.
#define JOY_REPEAT_INITIAL_MS 400
#define JOY_REPEAT_MS         120

// --- Avanco/retrocesso continuo (segurar) na tela de reproducao ----------
// Segurar JOY_RIGHT/JOY_LEFT avanca/retrocede em pulsos que ACELERAM
// exponencialmente quanto mais tempo o botao fica segurado: comeca
// suave (SEEK_HOLD_BASE_SEC por pulso) e DOBRA a cada SEEK_HOLD_DOUBLE_MS
// segurado, ate' um teto (SEEK_HOLD_MAX_SEC) - sem teto, um podcast de
// horas faria o avanco crescer tanto que passaria muito do ponto
// desejado antes de dar tempo de soltar o botao. Cada pulso individual
// usa o seek O(1) (fseek direto pro offset estimado - ver
// audio_player_seek_forward/backward), entao mesmo segurando por muito
// tempo cada pulso continua instantaneo, sem acumular lentidao.
// 2 toques RAPIDOS (sem segurar - dentro de DOUBLE_TAP_MS) na MESMA
// direcao pulam pra' proxima/anterior faixa em vez de avancar/voltar -
// so' verificado no PRIMEIRO pulso de cada aperto (nao faz sentido
// re-checar duplo-toque no meio de um hold ja' em andamento).
#define SEEK_HOLD_PULSE_MS   150   // intervalo entre pulsos enquanto segura
#define SEEK_HOLD_BASE_SEC   5     // avanco do 1o pulso (toque rapido = isso)
#define SEEK_HOLD_DOUBLE_MS  1200  // tempo segurado pra' o pulso dobrar de tamanho
#define SEEK_HOLD_MAX_SEC    120   // teto por pulso
#define DOUBLE_TAP_MS        350

static const gpio_num_t s_joy_pins[NUM_JOY_BTNS] = {
    (gpio_num_t)PIN_JOY_UP,
    (gpio_num_t)PIN_JOY_LEFT,
    (gpio_num_t)PIN_JOY_DOWN,
    (gpio_num_t)PIN_JOY_RIGHT,
    (gpio_num_t)PIN_JOY_CENTER,
};

static volatile uint32_t s_last_activity_ms = 0;
static ui_mode_t s_mode = UI_MODE_LIST;
static int s_list_cursor = 0;
static int s_conf_cursor = 0;
static int s_conf_audio_cursor = 0;
static int s_conf_display_cursor = 0;
static int s_conf_system_cursor = 0;
static int s_games_cursor = 0;

#define NAV_STACK_MAX_DEPTH 16

typedef struct {
    char dir_name[64];
    int cursor_index;
} nav_stack_entry_t;

static nav_stack_entry_t s_nav_stack[NAV_STACK_MAX_DEPTH];
static int s_nav_stack_depth = 0;

static void nav_history_push(const char *name, int cursor)
{
    if (s_nav_stack_depth < NAV_STACK_MAX_DEPTH) {
        nav_stack_entry_t *entry = &s_nav_stack[s_nav_stack_depth++];
        strlcpy(entry->dir_name, name ? name : "", sizeof(entry->dir_name));
        entry->cursor_index = cursor;
        ESP_LOGI(TAG, "Nav stack push [%d]: '%s' cursor=%d", s_nav_stack_depth - 1, entry->dir_name, cursor);
    }
}

static bool nav_history_pop(nav_stack_entry_t *out)
{
    if (s_nav_stack_depth > 0) {
        s_nav_stack_depth--;
        if (out) {
            *out = s_nav_stack[s_nav_stack_depth];
        }
        ESP_LOGI(TAG, "Nav stack pop [%d]: '%s' cursor=%d", s_nav_stack_depth, out ? out->dir_name : "", out ? out->cursor_index : -1);
        return true;
    }
    return false;
}

static void nav_history_clear(void)
{
    s_nav_stack_depth = 0;
}

static volatile bool s_locked = false;
static volatile bool s_game_active = false;
static TaskHandle_t s_display_task_handle = NULL;

void touch_input_set_display_task_handle(TaskHandle_t handle)
{
    s_display_task_handle = handle;
}

void touch_input_wake_display(void)
{
    if (s_display_task_handle != NULL) {
        xTaskNotifyGive(s_display_task_handle);
    }
}

uint32_t touch_input_get_last_activity_ms(void) { return s_last_activity_ms; }
ui_mode_t touch_input_get_mode(void)            { return s_mode; }
#ifndef MAX_PATH_LEN
#define MAX_PATH_LEN 512
#endif
#ifndef NAME_BUF_LEN
#define NAME_BUF_LEN 160
#endif

static int s_action_cursor = 0;
static ui_mode_t s_prev_mode_before_actions = UI_MODE_LIST;
static char s_clipboard_path[MAX_PATH_LEN] = {0};
static bool s_clipboard_is_cut = false;
static char s_action_target_path[MAX_PATH_LEN] = {0};
static char s_action_target_name[NAME_BUF_LEN] = {0};
static bool s_action_target_is_dir = false;
static bool s_action_target_is_playlist = false;

int touch_input_get_action_cursor(void) { return s_action_cursor; }

int touch_input_get_action_items(const char *items[], int max_items) {
    if (!items || max_items <= 0) return 0;
    static char s_target_pl_label[36];
    snprintf(s_target_pl_label, sizeof(s_target_pl_label), "Alvo: %s", audio_player_get_target_playlist());

    if (s_action_target_is_playlist) {
        static const char *pl_actions[] = {
            "Reproduzir Lista",
            "Definir como Alvo",
            "+ Adic. Faixa Atual",
            "+ Adic. Pasta Atual",
            "+ Nova Playlist",
            "Renomear Lista",
            "Apagar Playlist",
            "Informacoes"
        };
        int count = sizeof(pl_actions) / sizeof(pl_actions[0]);
        if (count > max_items) count = max_items;
        for (int i = 0; i < count; i++) items[i] = pl_actions[i];
        return count;
    } else {
        const char *norm_actions[] = {
            "Reproduzir Agora",
            "Tocar a Seguir",
            "Adicionar a Fila",
            "+ Adic. Lista Alvo",
            "Pasta -> Lista Alvo",
            s_target_pl_label,
            "+ Nova Playlist",
            "Renomear Item",
            "Copiar",
            "Recortar",
            "Colar",
            "Informacoes",
            "Mover p/ Lixeira"
        };
        int count = sizeof(norm_actions) / sizeof(norm_actions[0]);
        if (count > max_items) count = max_items;
        for (int i = 0; i < count; i++) items[i] = norm_actions[i];
        return count;
    }
}
const char *touch_input_get_clipboard_item(void) {
    if (s_clipboard_path[0] == '\0') return "";
    const char *p = strrchr(s_clipboard_path, '/');
    return p ? (p + 1) : s_clipboard_path;
}

int touch_input_get_list_cursor(void)
{
    if (s_mode == UI_MODE_ACTIONS_MENU) {
        return s_action_cursor;
    }
    if (s_mode == UI_MODE_CONF_MENU) {
        return s_conf_cursor;
    } else if (s_mode == UI_MODE_CONF_AUDIO) {
        return s_conf_audio_cursor;
    } else if (s_mode == UI_MODE_CONF_DISPLAY) {
        return s_conf_display_cursor;
    } else if (s_mode == UI_MODE_CONF_SYSTEM) {
        return s_conf_system_cursor;
    }
    return s_list_cursor;
}
bool touch_input_is_locked(void)                { return s_locked; }
bool touch_input_is_powered_off(void)           { return false; }

static int s_eq_preset_cursor = 0;
static ui_mode_t s_prev_mode_before_eq = UI_MODE_TOP_SCREEN;

// Estado do Teclado Virtual Desacoplado
static char s_kbd_text[17] = {0};
static int s_kbd_cursor = 0;
static int s_kbd_grid_x = 0;
static int s_kbd_grid_y = 0;
static int s_kbd_page = 0;
static bool s_kbd_confirming = false;
static keyboard_callback_t s_kbd_callback = NULL;

static const char kbd_pages[3][3][10] = {
    {
        {'q','w','e','r','t','y','u','i','o','p'},
        {'a','s','d','f','g','h','j','k','l','\0'},
        {'z','x','c','v','b','n','m','_','<','\r'}
    },
    {
        {'Q','W','E','R','T','Y','U','I','O','P'},
        {'A','S','D','F','G','H','J','K','L','\0'},
        {'Z','X','C','V','B','N','M','_','<','\r'}
    },
    {
        {'1','2','3','4','5','6','7','8','9','0'},
        {'!','@','#','$','%','&','*','-','+','\0'},
        {'(',')','[',']','{','}','?','_','<','\r'}
    }
};

int touch_input_get_eq_preset_cursor(void) {
    return s_eq_preset_cursor;
}

static ui_mode_t s_prev_mode_before_kbd = UI_MODE_LIST;

void keyboard_start(const char *initial_text, keyboard_callback_t cb) {
    if (initial_text) {
        strncpy(s_kbd_text, initial_text, sizeof(s_kbd_text) - 1);
    } else {
        s_kbd_text[0] = '\0';
    }
    s_kbd_text[sizeof(s_kbd_text) - 1] = '\0';
    s_kbd_cursor = strlen(s_kbd_text);
    s_kbd_grid_x = 0;
    s_kbd_grid_y = 0;
    s_kbd_page = 0;
    s_kbd_confirming = false;
    s_kbd_callback = cb;
    s_prev_mode_before_kbd = s_mode;
    s_mode = UI_MODE_KEYBOARD;
    ESP_LOGI(TAG, "Teclado iniciado com texto: '%s'", s_kbd_text);
}

static char s_kbd_undo_text[24];
static int s_kbd_undo_cursor = 0;

void touch_input_get_keyboard_state(char *text, int *cursor, int *grid_x, int *grid_y, int *page, bool *is_confirming) {
    if (text) {
        strncpy(text, s_kbd_text, 23);
        text[23] = '\0';
    }
    if (cursor) *cursor = s_kbd_cursor;
    if (grid_x) *grid_x = s_kbd_grid_x;
    if (grid_y) *grid_y = s_kbd_grid_y;
    if (page) *page = s_kbd_page;
    if (is_confirming) *is_confirming = s_kbd_confirming;
}

static void on_preset_renamed(const char *new_name, bool confirmed) {
    if (confirmed && s_eq_preset_cursor >= 0 && s_eq_preset_cursor < 10) {
        player_eq_config_t *cfg = (player_eq_config_t *)malloc(sizeof(player_eq_config_t));
        if (cfg) {
            audio_player_get_eq_config(cfg);
            strncpy(cfg->presets[s_eq_preset_cursor].name, new_name, PLAYER_EQ_PRESET_NAME_LEN);
            cfg->presets[s_eq_preset_cursor].name[PLAYER_EQ_PRESET_NAME_LEN - 1] = '\0';
            audio_player_set_eq_config(cfg);
            ESP_LOGI(TAG, "Preset %d renomeado para: '%s'", s_eq_preset_cursor, cfg->presets[s_eq_preset_cursor].name);
            free(cfg);
        }
    }
    s_mode = UI_MODE_EQ_PRESETS;
}

static void on_new_playlist_named(const char *new_name, bool confirmed) {
    if (confirmed && new_name && new_name[0] != '\0') {
        char created[64] = {0};
        if (audio_player_create_playlist(new_name, created, sizeof(created))) {
            oled_display_show_message("PLAYLIST CRIADA", created);
            vTaskDelay(pdMS_TO_TICKS(900));
        } else {
            oled_display_show_message("ERRO AO CRIAR", "Verifique o SD");
            vTaskDelay(pdMS_TO_TICKS(900));
        }
    }
    if (s_prev_mode_before_kbd == UI_MODE_ACTIONS_MENU) {
        s_mode = s_prev_mode_before_actions;
    } else if (s_prev_mode_before_kbd == UI_MODE_CONF_AUDIO) {
        s_mode = UI_MODE_CONF_MENU;
    } else {
        s_mode = s_prev_mode_before_kbd;
    }
}

static void on_new_eq_preset_named(const char *new_name, bool confirmed) {
    if (confirmed && new_name && new_name[0] != '\0') {
        player_eq_config_t cfg;
        audio_player_get_eq_config(&cfg);
        int new_slot = -1;
        for (int i = 1; i < PLAYER_EQ_MAX_PRESETS; i++) {
            if (cfg.presets[i].name[0] == '\0') {
                new_slot = i;
                break;
            }
        }
        if (new_slot != -1) {
            strlcpy(cfg.presets[new_slot].name, new_name, PLAYER_EQ_PRESET_NAME_LEN);
            int cur_act = cfg.active_preset_idx;
            if (cur_act < 0 || cur_act >= PLAYER_EQ_MAX_PRESETS) cur_act = 0;
            for (int b = 0; b < PLAYER_EQ_BANDS; b++) {
                cfg.presets[new_slot].band_gains[b] = cfg.presets[cur_act].band_gains[b];
            }
            cfg.presets[new_slot].overall_gain = cfg.presets[cur_act].overall_gain;
            cfg.active_preset_idx = new_slot;
            cfg.enabled = true;
            audio_player_set_eq_config(&cfg);
            oled_display_show_message("PRESET CRIADO", new_name);
            vTaskDelay(pdMS_TO_TICKS(900));
        } else {
            oled_display_show_message("LIMITE DE PRESETS", "Max 10 atingido");
            vTaskDelay(pdMS_TO_TICKS(900));
        }
    }
    s_mode = s_prev_mode_before_kbd;
}

static esp_err_t configure_joystick(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < NUM_JOY_BTNS; i++) {
        mask |= (1ULL << s_joy_pins[i]);
    }

    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        // Botoes aterram a linha quando pressionados (ativo em nivel
        // baixo) - precisa do pull-up interno pra' ficar em nivel alto
        // quando solto.
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

// --- Navegacao pelo joystick ---------------------------------------------

// Quantas entradas a lista tem AGORA (contando os itens do menu principal
// - o que estiver registrado em menu.h, ver a secao "Registro do menu
// principal" em main.c -, e escondendo a entrada ".." dentro de pastas -
// voltar de pasta e' pelo botao esquerdo dedicado, nao aparece mais na
// lista em si).
static int current_list_count(void)
{
    bool at_root = audio_player_browse_is_root();
    if (at_root && !s_in_player_browser) {
        // Menu principal: um item por entrada registrada em menu.h
        return menu_count();
    } else {
        int count = audio_player_get_browse_entry_count();
        if (!at_root) {
            // Subpasta: esconde o ".." (Ã­ndice 0)
            count--;
        }
        // Raiz do Player: nÃ£o subtrai, pois jÃ¡ nÃ£o tem ".."
        if (count < 0) count = 0;
        return count;
    }
}

static void joy_move_cursor(int delta, bool is_edge)
{
    if (s_mode != UI_MODE_LIST) return;

    bool at_root = audio_player_browse_is_root();
    bool in_player = s_in_player_browser;

    if (at_root && !in_player) {
        // Menu Principal: carrossel circular infinito usando quantidade real de itens registrados
        int m_cnt = menu_count();
        if (m_cnt <= 0) m_cnt = 7;
        s_list_cursor = (s_list_cursor + delta + m_cnt) % m_cnt;
        return;
    }

    if (delta < 0 && s_list_cursor == 0 && is_edge) {
        // Ja' no topo da lista e apertou "subir" NESSE aperto (nao durante
        // repeticao automatica ao segurar) - pula pra' tela de reproducao,
        // se houver algo tocando/carregado. Sem isso nao havia como voltar
        // pra' tela "Tocando agora" a partir da lista (JOY_DOWN agora sai
        // de la', ver o case correspondente em touch_task()).
        playback_state_t st;
        audio_player_get_state(&st);
        if (st.playing || st.track_loaded) {
            s_mode = UI_MODE_PLAYING;
            ESP_LOGI(TAG, "Ja' no topo da lista - indo pra' tela de reproducao");
            return;
        }
    }

    int count = current_list_count();
    if (count <= 0) {
        s_list_cursor = 0;
        return;
    }
    s_list_cursor += delta;
    if (s_list_cursor < 0) s_list_cursor = 0;
    if (s_list_cursor > count - 1) s_list_cursor = count - 1;
}

// --- Avanco/retrocesso continuo (so' faz sentido em UI_MODE_PLAYING) -----
static int s_seek_last_dir = 0;      // +1 = direita, -1 = esquerda, 0 = nenhum ainda
static uint32_t s_seek_last_press_ms = 0;

static void reset_seek_progression(void)
{
    s_seek_last_dir = 0;
    s_seek_last_press_ms = 0;
}

// Quanto avancar/retroceder no PROXIMO pulso, dado ha' quanto tempo
// (em ms) o botao ja' esta' sendo segurado sem soltar. Dobra a cada
// SEEK_HOLD_DOUBLE_MS, com teto em SEEK_HOLD_MAX_SEC.
static uint32_t seek_hold_amount_sec(uint32_t held_for_ms)
{
    uint32_t doublings = held_for_ms / SEEK_HOLD_DOUBLE_MS;
    if (doublings > 8) doublings = 8; // seguranca contra overflow - o teto ja' e' atingido bem antes disso
    uint32_t amount = SEEK_HOLD_BASE_SEC << doublings;
    return (amount > SEEK_HOLD_MAX_SEC) ? SEEK_HOLD_MAX_SEC : amount;
}

// dir: +1 = JOY_RIGHT (avancar/proxima), -1 = JOY_LEFT (retroceder/anterior).
// held_for_ms: ha' quanto tempo o botao esta' sendo segurado (0 no exato
// instante do toque). is_first_pulse: true so' na borda do aperto (usado
// pra' checar duplo-toque = pular faixa; pulsos seguintes do mesmo hold
// nao re-checam isso).
static void joy_seek_hold_pulse(int dir, uint32_t now, uint32_t held_for_ms, bool is_first_pulse)
{
    if (s_mode != UI_MODE_PLAYING) return;
    (void)now;
    (void)is_first_pulse;

    uint32_t effective_hold = (held_for_ms >= 350) ? (held_for_ms - 350) : 0;
    uint32_t amount = seek_hold_amount_sec(effective_hold);
    if (dir > 0) {
        ESP_LOGI(TAG, "Seek Avanco %u s (segurado ha %u ms)", (unsigned)amount, (unsigned)held_for_ms);
        audio_player_seek_forward(amount);
    } else {
        ESP_LOGI(TAG, "Seek Retrocesso %u s (segurado ha %u ms)", (unsigned)amount, (unsigned)held_for_ms);
        audio_player_seek_backward(amount);
    }
}

static void joy_select(void)
{
    if (s_mode != UI_MODE_LIST) return;

    bool at_root = audio_player_browse_is_root();

    // Tela principal? Despacha genericamente pro item selecionado no menu
    // (ver menu.h) - nenhum item novo precisa de mudanca aqui, so' de um
    // "*_register_menu_entry()" chamado no boot (main.c).
    if (at_root && !s_in_player_browser) {
        menu_select(s_list_cursor);
        return;
    }

    // Modo navegador de pastas (dentro do Player)
    int real_index;
    if (audio_player_browse_is_root()) {
        real_index = s_list_cursor;
    } else {
        real_index = s_list_cursor + 1;
    }

    char entry_name[64];
    audio_player_get_browse_entry_name(real_index, entry_name, sizeof(entry_name));
    bool is_dir = audio_player_browse_entry_is_dir(real_index);
    ESP_LOGI(TAG, "select: idx=%d, name='%s', is_dir=%d", real_index, entry_name, is_dir);

    if (is_dir) {
        // Salva na pilha a posicao do cursor atual e o nome da pasta em que estamos entrando
        nav_history_push(entry_name, s_list_cursor);
        audio_player_select_entry(real_index);
        s_in_player_browser = true;
        s_list_cursor = 0;
    } else {
        audio_player_select_entry(real_index);
        s_in_player_browser = true;
        s_mode = UI_MODE_PLAYING;
    }
}

static void joy_back(void)
{
    if (s_mode == UI_MODE_PLAYING) {
        s_mode = UI_MODE_LIST;
        int cur_track = audio_player_get_current_track_browse_index();
        if (cur_track >= 0) {
            int max_c = current_list_count();
            if (cur_track < max_c) {
                s_list_cursor = cur_track;
            }
        }
        ESP_LOGI(TAG, "Voltando para a lista (cursor=%d)", s_list_cursor);
        return;
    }
    if (s_mode == UI_MODE_LIST) {
        bool at_root = audio_player_browse_is_root();

        if (!at_root) {
            // Sobe uma pasta
            audio_player_select_entry(0);
            s_in_player_browser = true;

            nav_stack_entry_t entry;
            if (nav_history_pop(&entry)) {
                int total_items = current_list_count();
                int restored_cursor = entry.cursor_index;

                // Tenta validar se a pasta que entramos ainda existe no mesmo indice ou em outro
                if (strlen(entry.dir_name) > 0) {
                    bool now_at_root = audio_player_browse_is_root();
                    char check_name[64];
                    int check_idx = now_at_root ? restored_cursor : (restored_cursor + 1);
                    audio_player_get_browse_entry_name(check_idx, check_name, sizeof(check_name));
                    if (strcmp(check_name, entry.dir_name) != 0) {
                        for (int k = 0; k < total_items; k++) {
                            int k_idx = now_at_root ? k : (k + 1);
                            audio_player_get_browse_entry_name(k_idx, check_name, sizeof(check_name));
                            if (strcmp(check_name, entry.dir_name) == 0) {
                                restored_cursor = k;
                                break;
                            }
                        }
                    }
                }

                if (restored_cursor >= total_items) {
                    restored_cursor = total_items > 0 ? total_items - 1 : 0;
                }
                if (restored_cursor < 0) restored_cursor = 0;
                s_list_cursor = restored_cursor;
            } else {
                s_list_cursor = 0;
            }
            ESP_LOGI(TAG, "Voltando uma pasta -> cursor restaurado para %d", s_list_cursor);
        } else {
            // Ja na raiz
            if (s_in_player_browser) {
                s_in_player_browser = false;
                nav_history_clear();
                s_list_cursor = 0; // Retorna para "Player" (indice 0) no menu principal
                ESP_LOGI(TAG, "Voltando ao menu principal");
            }
            // Se ja esta no menu principal (in_player==false), JOY_LEFT nao faz nada
        }
    }
}

static bool fs_copy_file(const char *src_path, const char *dst_path) {
    if (!src_path || !dst_path || strcmp(src_path, dst_path) == 0) return false;
    FILE *in = fopen(src_path, "rb");
    if (!in) return false;
    FILE *out = fopen(dst_path, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    uint8_t *buf = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t chunk_sz = buf ? 4096 : 256;
    static uint8_t min_buf[256];
    if (!buf) buf = min_buf;

    size_t bytes_read = 0;
    bool ok = true;
    while ((bytes_read = fread(buf, 1, chunk_sz, in)) > 0) {
        if (fwrite(buf, 1, bytes_read, out) != bytes_read) {
            ok = false;
            break;
        }
    }
    fclose(in);
    fclose(out);
    if (buf != min_buf) {
        heap_caps_free(buf);
    }
    return ok;
}

static bool fs_move_to_trash(const char *src_path) {
    if (!src_path || src_path[0] == '\0') return false;
    mkdir("/sdcard/.trash", 0777);
    const char *fname = strrchr(src_path, '/');
    fname = fname ? (fname + 1) : src_path;
    char dst_path[MAX_PATH_LEN];
    snprintf(dst_path, sizeof(dst_path), "/sdcard/.trash/%.240s", fname);
    if (rename(src_path, dst_path) == 0) {
        return true;
    }
    if (fs_copy_file(src_path, dst_path)) {
        unlink(src_path);
        return true;
    }
    return false;
}

static void on_action_item_renamed(const char *new_name, bool confirmed) {
    if (confirmed && new_name && new_name[0] != '\0' && s_action_target_path[0] != '\0') {
        char parent[MAX_PATH_LEN];
        strlcpy(parent, s_action_target_path, sizeof(parent));
        char *slash = strrchr(parent, '/');
        if (slash) {
            *slash = '\0';
            char new_full_path[MAX_PATH_LEN];
            snprintf(new_full_path, sizeof(new_full_path), "%.240s/%.240s", parent, new_name);
            if (rename(s_action_target_path, new_full_path) == 0) {
                oled_display_trigger_heart_anim(true, "RENOMEADO!");
                audio_player_rescan_browser();
            } else {
                oled_display_show_message("ERRO RENOMEAR", "Falha de IO");
                vTaskDelay(pdMS_TO_TICKS(800));
            }
        }
    }
    s_mode = s_prev_mode_before_actions;
}

static void open_context_actions_menu(void) {
    s_action_target_path[0] = '\0';
    s_action_target_name[0] = '\0';
    s_action_target_is_dir = false;
    s_action_target_is_playlist = false;

    if (s_mode == UI_MODE_PLAYING) {
        playback_state_t st;
        audio_player_get_state(&st);
        if (st.filename[0] != '\0') {
            strlcpy(s_action_target_name, st.filename, sizeof(s_action_target_name));
            char cur_dir[MAX_PATH_LEN];
            audio_player_get_browse_dir(cur_dir, sizeof(cur_dir));
            snprintf(s_action_target_path, sizeof(s_action_target_path), "%.240s/%.240s", cur_dir, st.filename);
        }
    } else if (s_mode == UI_MODE_LIST) {
        bool at_root = audio_player_browse_is_root();
        bool in_player = s_in_player_browser;
        if (!at_root || in_player) {
            int src_idx = at_root ? s_list_cursor : (s_list_cursor + 1);
            char raw_name[NAME_BUF_LEN];
            audio_player_get_browse_entry_name(src_idx, raw_name, sizeof(raw_name));
            s_action_target_is_dir = audio_player_browse_entry_is_dir(src_idx);

            const char *clean_ptr = raw_name;
            if (strncmp(clean_ptr, "[PL] ", 5) == 0) {
                clean_ptr += 5;
                s_action_target_is_playlist = true;
            }
            char clean_name[NAME_BUF_LEN];
            strlcpy(clean_name, clean_ptr, sizeof(clean_name));
            size_t len = strlen(clean_name);
            if (len > 0 && clean_name[len - 1] == '/') clean_name[len - 1] = '\0';

            if (strlen(clean_name) > 4) {
                const char *ext = clean_name + strlen(clean_name) - 4;
                if (strcasecmp(ext, ".m3u") == 0) s_action_target_is_playlist = true;
            }
            if (strlen(clean_name) > 5) {
                const char *ext = clean_name + strlen(clean_name) - 5;
                if (strcasecmp(ext, ".m3u8") == 0) s_action_target_is_playlist = true;
            }

            strlcpy(s_action_target_name, clean_name, sizeof(s_action_target_name));

            char cur_dir[MAX_PATH_LEN];
            audio_player_get_browse_dir(cur_dir, sizeof(cur_dir));
            snprintf(s_action_target_path, sizeof(s_action_target_path), "%.240s/%.240s", cur_dir, clean_name);
        }
    }

    if (s_action_target_path[0] != '\0') {
        s_prev_mode_before_actions = s_mode;
        s_mode = UI_MODE_ACTIONS_MENU;
        s_action_cursor = 0;
        ESP_LOGI(TAG, "Menu de Acoes aberto para: %s (is_pl=%d)", s_action_target_path, s_action_target_is_playlist);
    } else {
        oled_display_trigger_heart_anim(false, "NENHUM ITEM");
    }
}

static void execute_context_action(void) {
    char cur_dir[MAX_PATH_LEN];
    audio_player_get_browse_dir(cur_dir, sizeof(cur_dir));

    if (s_action_target_is_playlist) {
        switch (s_action_cursor) {
            case 0: // Reproduzir Lista
                if (s_prev_mode_before_actions == UI_MODE_LIST) {
                    bool at_root = audio_player_browse_is_root();
                    int src_idx = at_root ? s_list_cursor : (s_list_cursor + 1);
                    audio_player_select_entry(src_idx);
                }
                s_mode = UI_MODE_PLAYING;
                return;
            case 1: { // Definir como Alvo
                audio_player_create_playlist(s_action_target_name, NULL, 0);
                oled_display_show_message(s_action_target_name, "Lista Alvo Ativa!");
                vTaskDelay(pdMS_TO_TICKS(800));
                s_mode = s_prev_mode_before_actions;
                return;
            }
            case 2: { // + Adic. Faixa Atual
                bool added = false;
                char prev_target[64];
                strlcpy(prev_target, audio_player_get_target_playlist(), sizeof(prev_target));
                audio_player_create_playlist(s_action_target_name, NULL, 0);
                if (audio_player_toggle_favorite_current(&added)) {
                    oled_display_show_message(s_action_target_name, added ? "+ Faixa adicionada!" : "- Faixa removida!");
                } else {
                    oled_display_show_message(s_action_target_name, "Nenhuma faixa ativa");
                }
                audio_player_create_playlist(prev_target, NULL, 0);
                vTaskDelay(pdMS_TO_TICKS(900));
                s_mode = s_prev_mode_before_actions;
                return;
            }
            case 3: { // + Adic. Pasta Atual
                int added_cnt = 0;
                char prev_target[64];
                strlcpy(prev_target, audio_player_get_target_playlist(), sizeof(prev_target));
                audio_player_create_playlist(s_action_target_name, NULL, 0);
                if (audio_player_add_current_folder_to_target_playlist(&added_cnt)) {
                    char msg[32];
                    snprintf(msg, sizeof(msg), "+ %d faixas adic.", added_cnt);
                    oled_display_show_message(s_action_target_name, msg);
                } else {
                    oled_display_show_message(s_action_target_name, "Erro ao adicionar");
                }
                audio_player_create_playlist(prev_target, NULL, 0);
                vTaskDelay(pdMS_TO_TICKS(900));
                s_mode = s_prev_mode_before_actions;
                return;
            }
            case 4: // + Nova Playlist
                keyboard_start("MinhaLista", on_new_playlist_named);
                return;
            case 5: // Renomear Lista
                keyboard_start(s_action_target_name, on_action_item_renamed);
                return;
            case 6: { // Apagar Playlist
                if (strncasecmp(s_action_target_name, "Favoritos", 9) == 0) {
                    oled_display_show_message("FAVORITOS", "Lista protegida!");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                } else {
                    if (audio_player_delete_playlist(s_action_target_name)) {
                        oled_display_show_message(s_action_target_name, "Lista Apagada!");
                        audio_player_rescan_browser();
                    } else {
                        oled_display_show_message("ERRO", "Falha ao apagar");
                    }
                    vTaskDelay(pdMS_TO_TICKS(900));
                }
                s_mode = s_prev_mode_before_actions;
                return;
            }
            case 7: { // Informacoes
                struct stat st;
                if (stat(s_action_target_path, &st) == 0) {
                    char size_str[32];
                    if (st.st_size >= 1048576) {
                        snprintf(size_str, sizeof(size_str), "Tam: %.1f MB", (float)st.st_size / 1048576.0f);
                    } else {
                        snprintf(size_str, sizeof(size_str), "Tam: %ld B", (long)st.st_size);
                    }
                    oled_display_show_message("INFO LISTA", size_str);
                    vTaskDelay(pdMS_TO_TICKS(1400));
                } else {
                    oled_display_trigger_heart_anim(false, "SEM INFO");
                }
                s_mode = s_prev_mode_before_actions;
                return;
            }
        }
    } else {
        switch (s_action_cursor) {
            case 0: // Reproduzir Agora
                if (!s_action_target_is_dir) {
                    if (s_prev_mode_before_actions == UI_MODE_LIST) {
                        bool at_root = audio_player_browse_is_root();
                        int src_idx = at_root ? s_list_cursor : (s_list_cursor + 1);
                        audio_player_select_entry(src_idx);
                    }
                    s_mode = UI_MODE_PLAYING;
                } else {
                    oled_display_trigger_heart_anim(false, "E DIRETORIO");
                    s_mode = s_prev_mode_before_actions;
                }
                break;
            case 1: // Tocar a Seguir
                oled_display_trigger_heart_anim(true, "PROXIMA FAIXA");
                s_mode = s_prev_mode_before_actions;
                break;
            case 2: // Adicionar a Fila
                oled_display_trigger_heart_anim(true, "NA FILA");
                s_mode = s_prev_mode_before_actions;
                break;
            case 3: { // + Adic. Lista Alvo
                const char *pl = audio_player_get_target_playlist();
                bool added = false;
                if (s_prev_mode_before_actions == UI_MODE_LIST) {
                    bool at_root = audio_player_browse_is_root();
                    int src_idx = at_root ? s_list_cursor : (s_list_cursor + 1);
                    int cnt = 0;
                    if (audio_player_toggle_browse_item_in_target_playlist(src_idx, &added, &cnt)) {
                        oled_display_show_message(pl, added ? "+ Adicionado!" : "- Removido!");
                    } else {
                        oled_display_show_message(pl, "Erro ao adicionar");
                    }
                } else {
                    if (audio_player_toggle_favorite_current(&added)) {
                        oled_display_show_message(pl, added ? "+ Faixa adicionada!" : "- Faixa removida!");
                    } else {
                        oled_display_show_message(pl, "Nenhuma faixa ativa");
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(900));
                s_mode = s_prev_mode_before_actions;
                break;
            }
            case 4: { // Pasta -> Lista Alvo
                int added_cnt = 0;
                const char *pl = audio_player_get_target_playlist();
                if (audio_player_add_current_folder_to_target_playlist(&added_cnt)) {
                    char msg[32];
                    snprintf(msg, sizeof(msg), "+ %d faixas adic.", added_cnt);
                    oled_display_show_message(pl, msg);
                } else {
                    oled_display_show_message(pl, "Erro ao adicionar");
                }
                vTaskDelay(pdMS_TO_TICKS(900));
                s_mode = s_prev_mode_before_actions;
                break;
            }
            case 5: { // Mudar Lista Alvo
                audio_player_cycle_target_playlist(+1);
                oled_display_show_message("LISTA ALVO", audio_player_get_target_playlist());
                vTaskDelay(pdMS_TO_TICKS(800));
                break;
            }
            case 6: // + Nova Playlist
                keyboard_start("MinhaLista", on_new_playlist_named);
                break;
            case 7: // Renomear Item
                keyboard_start(s_action_target_name, on_action_item_renamed);
                break;
            case 8: // Copiar
                strlcpy(s_clipboard_path, s_action_target_path, sizeof(s_clipboard_path));
                s_clipboard_is_cut = false;
                oled_display_trigger_heart_anim(true, "COPIADO");
                s_mode = s_prev_mode_before_actions;
                break;
            case 9: // Recortar
                strlcpy(s_clipboard_path, s_action_target_path, sizeof(s_clipboard_path));
                s_clipboard_is_cut = true;
                oled_display_trigger_heart_anim(true, "RECORTADO");
                s_mode = s_prev_mode_before_actions;
                break;
            case 10: // Colar
                if (s_clipboard_path[0] == '\0') {
                    oled_display_trigger_heart_anim(false, "CLIP VAZIO");
                } else {
                    const char *fname = strrchr(s_clipboard_path, '/');
                    fname = fname ? (fname + 1) : s_clipboard_path;
                    char dst_path[MAX_PATH_LEN];
                    snprintf(dst_path, sizeof(dst_path), "%.240s/%.240s", cur_dir, fname);
                    bool ok = false;
                    if (s_clipboard_is_cut) {
                        ok = (rename(s_clipboard_path, dst_path) == 0);
                        if (!ok) {
                            ok = fs_copy_file(s_clipboard_path, dst_path);
                            if (ok) unlink(s_clipboard_path);
                        }
                        if (ok) s_clipboard_path[0] = '\0';
                    } else {
                        ok = fs_copy_file(s_clipboard_path, dst_path);
                    }
                    if (ok) {
                        oled_display_trigger_heart_anim(true, "COLADO!");
                        audio_player_rescan_browser();
                    } else {
                        oled_display_show_message("ERRO AO COLAR", "Falha de IO");
                        vTaskDelay(pdMS_TO_TICKS(800));
                    }
                }
                s_mode = s_prev_mode_before_actions;
                break;
            case 11: { // Informacoes
                struct stat st;
                if (stat(s_action_target_path, &st) == 0) {
                    char size_str[32];
                    if (st.st_size >= 1048576) {
                        snprintf(size_str, sizeof(size_str), "Tam: %.1f MB", (float)st.st_size / 1048576.0f);
                    } else {
                        snprintf(size_str, sizeof(size_str), "Tam: %ld KB", (long)(st.st_size / 1024));
                    }
                    oled_display_show_message("INFO ARQUIVO", size_str);
                    vTaskDelay(pdMS_TO_TICKS(1400));
                } else {
                    oled_display_trigger_heart_anim(false, "SEM INFO");
                }
                s_mode = s_prev_mode_before_actions;
                break;
            }
            case 12: // Mover p/ Lixeira
                if (fs_move_to_trash(s_action_target_path)) {
                    oled_display_trigger_heart_anim(true, "LIXEIRA OK");
                    audio_player_rescan_browser();
                } else {
                    oled_display_show_message("ERRO LIXEIRA", "Falha ao mover");
                    vTaskDelay(pdMS_TO_TICKS(800));
                }
                s_mode = s_prev_mode_before_actions;
                break;
        }
    }
}

static bool s_chord_is_liked_cached = false;
static void handle_chord_like(bool like) {
    bool ok = false;
    const char *pl = audio_player_get_target_playlist();

    if (s_mode == UI_MODE_PLAYING) {
        if (audio_player_like_current(like, &ok)) {
            s_chord_is_liked_cached = like;
            char toast[48];
            snprintf(toast, sizeof(toast), "%s %s", like ? "+" : "-", pl);
            oled_display_trigger_heart_anim(like, toast);
        }
    } else if (s_mode == UI_MODE_LIST) {
        bool at_root = audio_player_browse_is_root();
        bool in_player = s_in_player_browser;
        if (!at_root || in_player) {
            if (audio_player_like_browse_item(s_list_cursor, like, &ok)) {
                s_chord_is_liked_cached = like;
                char toast[48];
                snprintf(toast, sizeof(toast), "%s %s", like ? "+" : "-", pl);
                oled_display_trigger_heart_anim(like, toast);
            }
        }
    }
}

// Controle da Semi Aba (Drawers de Playlists e Equalizador dentro das Bolhas)
static inline bool is_chord_menu_allowed(void)
{
    if (s_mode == UI_MODE_PLAYING) {
        return true;
    }
    if (s_mode == UI_MODE_LIST) {
        bool at_root = audio_player_browse_is_root();
        bool in_player = s_in_player_browser;
        return (!at_root || in_player);
    }
    return false;
}

static chord_drawer_mode_t s_active_chord_drawer = CHORD_DRAWER_NONE;
static uint32_t s_dir_hold_start_ms = 0;
static uint32_t s_rehold_start_ms = 0;
static int s_drawer_pl_cursor = 0;
static int s_drawer_eq_cursor = 0;
static bool s_deep_hold_triggered = false;
static bool s_drawer_latched_open = false;
static char s_cached_drawer_pl_names[10][32];
static const char *s_cached_drawer_pl_ptrs[10];
static int s_cached_drawer_pl_count = 0;
static int s_cached_drawer_active_pl_idx = 0;
static player_eq_config_t s_cached_eq_cfg;
static char s_cached_drawer_eq_names[10][32];
static const char *s_cached_drawer_eq_ptrs[10];
static int s_cached_drawer_eq_count = 0;
static uint32_t s_last_chord_action_ms = 0;
static uint8_t s_chord_suppressed_mask = 0;
static bool s_chord_hud_latched = false;


// --- Gaveta Inferior de Modos de Reproducao (Loop e Shuffle) ---
static int s_drawer_modes_cursor = 0; // 0 = Loop, 1 = Shuffle
static char s_cached_modes_strings[2][32];
static const char *s_cached_modes_ptrs[2];

static void update_modes_drawer_strings(void)
{
    audio_player_loop_mode_t lm = audio_player_get_loop_mode();
    const char *lm_str = (lm == AUDIO_LOOP_TRACK) ? "< 1x >" : ((lm == AUDIO_LOOP_ALL) ? "< Tudo >" : "< Off >");
    snprintf(s_cached_modes_strings[0], sizeof(s_cached_modes_strings[0]), "%s", lm_str);
    s_cached_modes_ptrs[0] = s_cached_modes_strings[0];

    audio_player_shuffle_mode_t sm = audio_player_get_shuffle_mode();
    const char *sm_str = (sm == AUDIO_SHUFFLE_FOLDER) ? "< Pasta >" : ((sm == AUDIO_SHUFFLE_ALL) ? "< Tudo >" : "< Off >");
    snprintf(s_cached_modes_strings[1], sizeof(s_cached_modes_strings[1]), "%s", sm_str);
    s_cached_modes_ptrs[1] = s_cached_modes_strings[1];
}

static void open_modes_drawer(uint32_t now)
{
    s_active_chord_drawer = CHORD_DRAWER_MODES;
    s_dir_hold_start_ms = now;
    s_rehold_start_ms = 0;
    s_deep_hold_triggered = false;
    s_drawer_latched_open = true;

    update_modes_drawer_strings();
    oled_display_set_chord_drawer(CHORD_DRAWER_MODES, 50, s_cached_modes_ptrs, 2, s_drawer_modes_cursor, 0);
}

static void open_playlists_drawer(uint32_t now)
{
    s_active_chord_drawer = CHORD_DRAWER_PLAYLISTS;
    s_dir_hold_start_ms = now;
    s_rehold_start_ms = 0;
    s_deep_hold_triggered = false;
    s_drawer_latched_open = false;

    int real_pls = audio_player_get_playlists_count();
    if (real_pls > 8) real_pls = 8;
    s_cached_drawer_active_pl_idx = 0;
    for (int i = 0; i < real_pls; i++) {
        bool is_act = false;
        audio_player_get_playlist_by_index(i, s_cached_drawer_pl_names[i], sizeof(s_cached_drawer_pl_names[i]), &is_act);
        s_cached_drawer_pl_ptrs[i] = s_cached_drawer_pl_names[i];
        if (is_act) s_cached_drawer_active_pl_idx = i;
    }
    strlcpy(s_cached_drawer_pl_names[real_pls], "+ Nova Lista", sizeof(s_cached_drawer_pl_names[real_pls]));
    s_cached_drawer_pl_ptrs[real_pls] = s_cached_drawer_pl_names[real_pls];
    s_cached_drawer_pl_count = real_pls + 1;

    s_drawer_pl_cursor = s_cached_drawer_active_pl_idx;
    oled_display_set_chord_drawer(CHORD_DRAWER_PLAYLISTS, 0, s_cached_drawer_pl_ptrs, s_cached_drawer_pl_count, s_drawer_pl_cursor, s_cached_drawer_active_pl_idx);
}

static void open_equalizer_drawer(uint32_t now)
{
    s_active_chord_drawer = CHORD_DRAWER_EQUALIZER;
    s_dir_hold_start_ms = now;
    s_rehold_start_ms = 0;
    s_deep_hold_triggered = false;
    s_drawer_latched_open = false;

    audio_player_get_eq_config(&s_cached_eq_cfg);
    int real_eq = 0;
    for (int i = 0; i < PLAYER_EQ_MAX_PRESETS; i++) {
        if (s_cached_eq_cfg.presets[i].name[0] != '\0') {
            strlcpy(s_cached_drawer_eq_names[real_eq], s_cached_eq_cfg.presets[i].name, sizeof(s_cached_drawer_eq_names[real_eq]));
            s_cached_drawer_eq_ptrs[real_eq] = s_cached_drawer_eq_names[real_eq];
            real_eq++;
        }
    }
    if (real_eq == 0) {
        strlcpy(s_cached_drawer_eq_names[0], "Padrao", sizeof(s_cached_drawer_eq_names[0]));
        s_cached_drawer_eq_ptrs[0] = s_cached_drawer_eq_names[0];
        real_eq = 1;
    }

    if (real_eq < PLAYER_EQ_MAX_PRESETS) {
        strlcpy(s_cached_drawer_eq_names[real_eq], "+ Novo Preset", sizeof(s_cached_drawer_eq_names[real_eq]));
        s_cached_drawer_eq_ptrs[real_eq] = s_cached_drawer_eq_names[real_eq];
        s_cached_drawer_eq_count = real_eq + 1;
    } else {
        s_cached_drawer_eq_count = real_eq;
    }

    s_drawer_eq_cursor = (s_cached_eq_cfg.active_preset_idx < real_eq) ? s_cached_eq_cfg.active_preset_idx : 0;
    oled_display_set_chord_drawer(CHORD_DRAWER_EQUALIZER, 0, s_cached_drawer_eq_ptrs, s_cached_drawer_eq_count, s_drawer_eq_cursor, s_cached_eq_cfg.active_preset_idx);
}

static void close_active_drawer(void)
{
    s_active_chord_drawer = CHORD_DRAWER_NONE;
    s_deep_hold_triggered = false;
    s_drawer_latched_open = false;
    s_dir_hold_start_ms = 0;
    s_rehold_start_ms = 0;
    oled_display_set_chord_drawer(CHORD_DRAWER_NONE, 0, NULL, 0, 0, -1);
}

static void transition_to_playlists_browser(uint32_t now)
{
    oled_display_trigger_bubble_expand_toast(0, 31, "PLAYLISTS", "Explorador de Arquivos", NULL);
    audio_player_browse_to_playlists();
    s_in_player_browser = true;
    s_mode = UI_MODE_LIST;
    s_list_cursor = 0;
    s_chord_hud_latched = false;
    close_active_drawer();
    oled_display_set_chord_hud(false, 0, -1, false);
    s_chord_suppressed_mask |= (1 << JOY_LEFT);
    s_last_chord_action_ms = now;
    ESP_LOGI(TAG, "Navegando para /Playlists com anuncio expansivo!");
}

static void transition_to_equalizer_presets(uint32_t now)
{
    oled_display_trigger_bubble_expand_toast(127, 31, "EQUALIZADOR", "Painel de Presets", NULL);
    s_prev_mode_before_eq = UI_MODE_PLAYING;
    s_mode = UI_MODE_EQ_PRESETS;
    s_eq_preset_cursor = s_drawer_eq_cursor;
    s_chord_hud_latched = false;
    close_active_drawer();
    oled_display_set_chord_hud(false, 0, -1, false);
    s_chord_suppressed_mask |= (1 << JOY_RIGHT);
    s_last_chord_action_ms = now;
    ESP_LOGI(TAG, "Navegando para Equalizador (Presets) com anuncio expansivo!");
}


static void touch_task(void *arg)
{
    (void)arg;

    bool joy_prev[NUM_JOY_BTNS] = {false};
    uint32_t joy_press_ms[NUM_JOY_BTNS] = {0};
    uint32_t joy_last_repeat_ms[NUM_JOY_BTNS] = {0};

    // JOY_CENTER: curto = play/pause; longo = bloqueia/desbloqueia.
    uint32_t center_press_start = 0;
    bool center_long_fired = false;
    bool center_modifier_used = false;
    uint32_t s_last_center_release_ms = 0;
    bool center_is_lock_candidate = false;

    // Segurar JOY_LEFT com um modo de tela cheia ativo (USB/WiFi) = sair.
    uint32_t exit_active_hold_start = 0;
    bool exit_active_fired = false;

    // Blindagem de acordes e prevencao de vazamento de comandos de 1a ordem:
    uint32_t joy_pending_edge_ms[NUM_JOY_BTNS] = {0};
    int8_t s_chord_focused_dir = -1;  // Direcao focada (0=UP, 1=DOWN, 2=LEFT, 3=RIGHT) para XOR no hold e exec no release

    while (true) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if (s_last_interaction_ms == 0) s_last_interaction_ms = now;

        uint8_t vmask = debug_nav_get_vmask(now);
        bool joy_cur[NUM_JOY_BTNS];
        for (int i = 0; i < NUM_JOY_BTNS; i++) {
            bool phys = (gpio_get_level(s_joy_pins[i]) == 0); // ativo em nivel baixo
            bool virt = (vmask & (1u << i)) != 0;
            joy_cur[i] = phys || virt;
        }

        bool any_joy = false;
        for (int i = 0; i < NUM_JOY_BTNS; i++) {
            if (joy_cur[i] && !joy_prev[i]) any_joy = true;
        }
        if (any_joy) {
            s_last_activity_ms = now;
            s_last_interaction_ms = now;
            uint32_t lk_secs = touch_input_get_lock_clock_secs();
            if (s_is_sleeping) {
                s_is_sleeping = false;
                if (lk_secs > 0) {
                    s_lock_clock_until_ms = now + (lk_secs * 1000UL);
                }
                touch_input_wake_display();
                for (int i = 0; i < NUM_JOY_BTNS; i++) joy_prev[i] = joy_cur[i];
                vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
                continue;
            } else if (s_locked && !joy_cur[JOY_CENTER]) {
                if (lk_secs > 0) {
                    s_lock_clock_until_ms = now + (lk_secs * 1000UL);
                }
                touch_input_wake_display();
                for (int i = 0; i < NUM_JOY_BTNS; i++) joy_prev[i] = joy_cur[i];
                vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
                continue;
            } else if (s_locked && joy_cur[JOY_CENTER]) {
                if (lk_secs > 0) {
                    s_lock_clock_until_ms = now + (lk_secs * 1000UL);
                }
                touch_input_wake_display();
            }
        }

        // --- Botao central: curto = play/pause; longo = bloqueio -----------
        // Fica antes do retorno de s_locked: assim, um toque longo no mesmo
        // botao sempre funciona para desbloquear.
        bool center_pressed_edge = joy_cur[JOY_CENTER] && !joy_prev[JOY_CENTER];
        bool center_released_edge = !joy_cur[JOY_CENTER] && joy_prev[JOY_CENTER];
        if (center_pressed_edge) {
            if (s_active_chord_drawer != CHORD_DRAWER_NONE) {
                if (is_chord_menu_allowed()) {
                    // Clique no centro confirma o item ativo na Semi Aba!
                    if (s_active_chord_drawer == CHORD_DRAWER_PLAYLISTS) {
                        int real_pls = s_cached_drawer_pl_count - 1;
                        if (s_drawer_pl_cursor >= real_pls) {
                            close_active_drawer();
                            s_chord_hud_latched = false;
                            oled_display_set_chord_hud(false, 0, -1, false);
                            center_modifier_used = true;
                            s_last_chord_action_ms = now;
                            s_chord_suppressed_mask |= (1 << JOY_CENTER);
                            center_press_start = 0;
                            s_chord_focused_dir = -1;
                            keyboard_start("MinhaLista", on_new_playlist_named);
                            continue;
                        } else {
                            audio_player_set_target_playlist_by_index(s_drawer_pl_cursor);
                            const char *pl = audio_player_get_target_playlist();
                            char clean_pl[32];
                            strlcpy(clean_pl, pl, sizeof(clean_pl));
                            char *dot = strrchr(clean_pl, '.');
                            if (dot) *dot = '\0';
                            oled_display_trigger_bubble_expand_toast(0, 31, "PLAYLIST ALVO", clean_pl, NULL);
                        }
                    } else if (s_active_chord_drawer == CHORD_DRAWER_EQUALIZER) {
                        int real_eq = s_cached_drawer_eq_count - 1;
                        if (s_cached_drawer_eq_count > 1 && s_drawer_eq_cursor == real_eq && strcmp(s_cached_drawer_eq_ptrs[real_eq], "+ Novo Preset") == 0) {
                            close_active_drawer();
                            s_chord_hud_latched = false;
                            oled_display_set_chord_hud(false, 0, -1, false);
                            center_modifier_used = true;
                            s_last_chord_action_ms = now;
                            s_chord_suppressed_mask |= (1 << JOY_CENTER);
                            center_press_start = 0;
                            s_chord_focused_dir = -1;
                            keyboard_start("MeuPreset", on_new_eq_preset_named);
                            continue;
                        } else {
                            s_cached_eq_cfg.active_preset_idx = s_drawer_eq_cursor;
                            s_cached_eq_cfg.enabled = true;
                            audio_player_set_eq_config(&s_cached_eq_cfg);
                            oled_display_trigger_bubble_expand_toast(127, 31, "EQUALIZADOR", s_cached_drawer_eq_ptrs[s_drawer_eq_cursor], NULL);
                        }
                    } else if (s_active_chord_drawer == CHORD_DRAWER_MODES) {
                        // Confirma a selecao atual na gaveta sem ciclar novamente
                        update_modes_drawer_strings();
                        if (s_drawer_modes_cursor == 0) {
                            audio_player_loop_mode_t lm = audio_player_get_loop_mode();
                            const char *lm_toast = (lm == AUDIO_LOOP_TRACK) ? "Repetir: 1x" : ((lm == AUDIO_LOOP_ALL) ? "Repetir: Tudo" : "Repetir: Off");
                            oled_display_trigger_bubble_expand_toast(64, 63, "REPRODUCAO", lm_toast, NULL);
                        } else {
                            audio_player_shuffle_mode_t sm = audio_player_get_shuffle_mode();
                            const char *sm_toast = (sm == AUDIO_SHUFFLE_FOLDER) ? "Aleatorio: Pasta" : ((sm == AUDIO_SHUFFLE_ALL) ? "Aleatorio: Tudo" : "Aleatorio: Off");
                            oled_display_trigger_bubble_expand_toast(64, 63, "REPRODUCAO", sm_toast, NULL);
                        }
                    }
                }
                close_active_drawer();
                s_chord_hud_latched = false;
                oled_display_set_chord_hud(false, 0, -1, false);
                center_modifier_used = true;
                s_last_chord_action_ms = now;
                s_chord_suppressed_mask |= (1 << JOY_CENTER);
                center_press_start = 0;
                s_chord_focused_dir = -1;
            } else if (s_chord_hud_latched && is_chord_menu_allowed()) {
                // "um outro clique no meio estouraria as bolhas"
                s_chord_hud_latched = false;
                oled_display_set_chord_hud(false, 0, -1, false);
                s_last_chord_action_ms = now;
                s_chord_suppressed_mask |= (1 << JOY_CENTER);
                center_modifier_used = true; // Impede play/pause ou tap-lock na soltura
                center_press_start = 0;
                s_chord_focused_dir = -1;
                ESP_LOGI(TAG, "Bolhas estouradas pelo clique central");
            } else {
                center_press_start = now;
                center_long_fired = false;
                center_modifier_used = false;

                // Cache do status de favorito no momento do aperto inicial apenas em telas permitidas (ZERO I/O no loop de hold!)
                if (is_chord_menu_allowed()) {
                    if (s_mode == UI_MODE_LIST) {
                        s_chord_is_liked_cached = audio_player_is_browse_item_favorite(s_list_cursor);
                    } else {
                        s_chord_is_liked_cached = audio_player_is_current_favorite();
                    }
                }

                // Bloqueio deliberado: exige clique rapido anterior (< 400 ms) + este 2o clique segurado
                if (!s_locked && s_last_center_release_ms > 0 && (now - s_last_center_release_ms) < 400) {
                    center_is_lock_candidate = true;
                } else {
                    center_is_lock_candidate = false;
                }
            }
        }

        // Se o modo atual nao for permitido para acordes, assegura que qualquer gaveta ou HUD aberto seja imediatamente fechado
        if (!is_chord_menu_allowed()) {
            if (s_active_chord_drawer != CHORD_DRAWER_NONE) {
                close_active_drawer();
            }
            if (s_chord_hud_latched) {
                s_chord_hud_latched = false;
                oled_display_set_chord_hud(false, 0, -1, false);
            }
        }

        // --- Gerenciamento da Gaveta Lateral Ativa (Playlists / Equalizador) ---
        if (s_active_chord_drawer != CHORD_DRAWER_NONE && !s_locked && is_chord_menu_allowed()) {
            if (s_active_chord_drawer == CHORD_DRAWER_PLAYLISTS) {
                if (joy_cur[JOY_LEFT]) {
                    if (!s_drawer_latched_open) {
                        // 1o acionamento:
                        // 0..150 ms: infla suavemente ate 50%
                        // 150..550 ms: segura em 50% exibindo a lista
                        // >= 550 ms (hold continuo): dispara a transicao com o anuncio da bolha!
                        uint32_t elapsed = now - s_dir_hold_start_ms;
                        if (elapsed < 150) {
                            uint8_t pct = (uint8_t)((elapsed * 50) / 150);
                            oled_display_set_chord_drawer(CHORD_DRAWER_PLAYLISTS, pct, s_cached_drawer_pl_ptrs, s_cached_drawer_pl_count, s_drawer_pl_cursor, s_cached_drawer_active_pl_idx);
                        } else if (elapsed < 550) {
                            oled_display_set_chord_drawer(CHORD_DRAWER_PLAYLISTS, 50, s_cached_drawer_pl_ptrs, s_cached_drawer_pl_count, s_drawer_pl_cursor, s_cached_drawer_active_pl_idx);
                        } else {
                            if (!s_deep_hold_triggered) {
                                s_deep_hold_triggered = true;
                                transition_to_playlists_browser(now);
                            }
                        }
                    }
                } else if (!joy_cur[JOY_LEFT] && joy_prev[JOY_LEFT]) {
                    // Soltura da direcao Esquerda:
                    if (!s_drawer_latched_open) {
                        // Soltura do 1o aperto (toque rapido ou segurado < 550 ms): gaveta trava aberta a 50%!
                        s_drawer_latched_open = true;
                        oled_display_set_chord_drawer(CHORD_DRAWER_PLAYLISTS, 50, s_cached_drawer_pl_ptrs, s_cached_drawer_pl_count, s_drawer_pl_cursor, s_cached_drawer_active_pl_idx);
                    }
                }

                // 2º clique na MESMA direcao com a gaveta ja aberta a 50%:
                // Tanto um toque rapido quanto segurar navega de fato para o local com anuncio expansivo
                if (!s_deep_hold_triggered && s_drawer_latched_open && joy_cur[JOY_LEFT] && !joy_prev[JOY_LEFT]) {
                    s_deep_hold_triggered = true;
                    transition_to_playlists_browser(now);
                }

                // Direcao oposta (Direita): fecha a gaveta de Playlists e retorna ao HUD de bolhas
                if (!s_deep_hold_triggered && joy_cur[JOY_RIGHT] && !joy_prev[JOY_RIGHT]) {
                    s_chord_suppressed_mask |= (1 << JOY_RIGHT);
                    close_active_drawer();
                    s_chord_hud_latched = false;
                    oled_display_set_chord_hud(false, 0, -1, false);
                }
            } else if (s_active_chord_drawer == CHORD_DRAWER_EQUALIZER) {
                if (joy_cur[JOY_RIGHT]) {
                    if (!s_drawer_latched_open) {
                        // 1o acionamento:
                        // 0..150 ms: infla suavemente ate 50%
                        // 150..550 ms: segura em 50% exibindo presets
                        // >= 550 ms (hold continuo): dispara a transicao com o anuncio da bolha!
                        uint32_t elapsed = now - s_dir_hold_start_ms;
                        if (elapsed < 150) {
                            uint8_t pct = (uint8_t)((elapsed * 50) / 150);
                            oled_display_set_chord_drawer(CHORD_DRAWER_EQUALIZER, pct, s_cached_drawer_eq_ptrs, s_cached_drawer_eq_count, s_drawer_eq_cursor, s_cached_eq_cfg.active_preset_idx);
                        } else if (elapsed < 550) {
                            oled_display_set_chord_drawer(CHORD_DRAWER_EQUALIZER, 50, s_cached_drawer_eq_ptrs, s_cached_drawer_eq_count, s_drawer_eq_cursor, s_cached_eq_cfg.active_preset_idx);
                        } else {
                            if (!s_deep_hold_triggered) {
                                s_deep_hold_triggered = true;
                                transition_to_equalizer_presets(now);
                            }
                        }
                    }
                } else if (!joy_cur[JOY_RIGHT] && joy_prev[JOY_RIGHT]) {
                    // Soltura da direcao Direita:
                    if (!s_drawer_latched_open) {
                        // Soltura do 1o aperto (toque rapido ou segurado < 550 ms): gaveta trava aberta a 50%!
                        s_drawer_latched_open = true;
                        oled_display_set_chord_drawer(CHORD_DRAWER_EQUALIZER, 50, s_cached_drawer_eq_ptrs, s_cached_drawer_eq_count, s_drawer_eq_cursor, s_cached_eq_cfg.active_preset_idx);
                    }
                }

                // 2º clique na MESMA direcao com a gaveta ja aberta a 50%:
                // Tanto um toque rapido quanto segurar navega de fato para o local com anuncio expansivo
                if (!s_deep_hold_triggered && s_drawer_latched_open && joy_cur[JOY_RIGHT] && !joy_prev[JOY_RIGHT]) {
                    s_deep_hold_triggered = true;
                    transition_to_equalizer_presets(now);
                }

                // Direcao oposta (Esquerda): fecha a gaveta do Equalizador e retorna ao HUD de bolhas
                if (!s_deep_hold_triggered && joy_cur[JOY_LEFT] && !joy_prev[JOY_LEFT]) {
                    s_chord_suppressed_mask |= (1 << JOY_LEFT);
                    close_active_drawer();
                    s_chord_hud_latched = false;
                    oled_display_set_chord_hud(false, 0, -1, false);
                }
            }

            if (s_active_chord_drawer == CHORD_DRAWER_MODES) {
                if (joy_cur[JOY_RIGHT] && !joy_prev[JOY_RIGHT]) {
                    s_chord_suppressed_mask |= (1 << JOY_RIGHT);
                    if (s_drawer_modes_cursor == 0) {
                        audio_player_cycle_loop_mode();
                    } else {
                        audio_player_cycle_shuffle_mode();
                    }
                    update_modes_drawer_strings();
                    oled_display_set_chord_drawer(CHORD_DRAWER_MODES, 50, s_cached_modes_ptrs, 2, s_drawer_modes_cursor, 0);
                } else if (joy_cur[JOY_LEFT] && !joy_prev[JOY_LEFT]) {
                    s_chord_suppressed_mask |= (1 << JOY_LEFT);
                    if (s_drawer_modes_cursor == 0) {
                        audio_player_prev_loop_mode();
                    } else {
                        audio_player_prev_shuffle_mode();
                    }
                    update_modes_drawer_strings();
                    oled_display_set_chord_drawer(CHORD_DRAWER_MODES, 50, s_cached_modes_ptrs, 2, s_drawer_modes_cursor, 0);
                } else if (joy_cur[JOY_CENTER] && !joy_prev[JOY_CENTER]) {
                    // Botao Central na gaveta de Modos: CONFIRMA E FECHA com suavidade
                    s_chord_suppressed_mask |= (1 << JOY_CENTER);
                    center_modifier_used = true;
                    s_last_center_release_ms = 0;
                    close_active_drawer();
                    s_chord_hud_latched = false;
                    oled_display_set_chord_hud(false, 0, -1, false);
                }
            }

            // 2. Navegacao com a Gaveta aberta (Cima / Baixo)
            if (!s_deep_hold_triggered && s_active_chord_drawer != CHORD_DRAWER_NONE) {
                if (joy_cur[JOY_UP] && !joy_prev[JOY_UP]) {
                    s_chord_suppressed_mask |= (1 << JOY_UP);
                    if (s_active_chord_drawer == CHORD_DRAWER_PLAYLISTS && s_cached_drawer_pl_count > 0) {
                        s_drawer_pl_cursor = (s_drawer_pl_cursor - 1 + s_cached_drawer_pl_count) % s_cached_drawer_pl_count;
                        oled_display_set_chord_drawer(CHORD_DRAWER_PLAYLISTS, 50, s_cached_drawer_pl_ptrs, s_cached_drawer_pl_count, s_drawer_pl_cursor, s_cached_drawer_active_pl_idx);
                    } else if (s_active_chord_drawer == CHORD_DRAWER_EQUALIZER && s_cached_drawer_eq_count > 0) {
                        s_drawer_eq_cursor = (s_drawer_eq_cursor - 1 + s_cached_drawer_eq_count) % s_cached_drawer_eq_count;
                        oled_display_set_chord_drawer(CHORD_DRAWER_EQUALIZER, 50, s_cached_drawer_eq_ptrs, s_cached_drawer_eq_count, s_drawer_eq_cursor, s_cached_eq_cfg.active_preset_idx);
                    } else if (s_active_chord_drawer == CHORD_DRAWER_MODES) {
                        s_drawer_modes_cursor = 1 - s_drawer_modes_cursor;
                        update_modes_drawer_strings();
                        oled_display_set_chord_drawer(CHORD_DRAWER_MODES, 50, s_cached_modes_ptrs, 2, s_drawer_modes_cursor, 0);
                    }
                } else if (joy_cur[JOY_DOWN] && !joy_prev[JOY_DOWN]) {
                    s_chord_suppressed_mask |= (1 << JOY_DOWN);
                    if (s_active_chord_drawer == CHORD_DRAWER_PLAYLISTS && s_cached_drawer_pl_count > 0) {
                        s_drawer_pl_cursor = (s_drawer_pl_cursor + 1) % s_cached_drawer_pl_count;
                        oled_display_set_chord_drawer(CHORD_DRAWER_PLAYLISTS, 50, s_cached_drawer_pl_ptrs, s_cached_drawer_pl_count, s_drawer_pl_cursor, s_cached_drawer_active_pl_idx);
                    } else if (s_active_chord_drawer == CHORD_DRAWER_EQUALIZER && s_cached_drawer_eq_count > 0) {
                        s_drawer_eq_cursor = (s_drawer_eq_cursor + 1) % s_cached_drawer_eq_count;
                        oled_display_set_chord_drawer(CHORD_DRAWER_EQUALIZER, 50, s_cached_drawer_eq_ptrs, s_cached_drawer_eq_count, s_drawer_eq_cursor, s_cached_eq_cfg.active_preset_idx);
                    } else if (s_active_chord_drawer == CHORD_DRAWER_MODES) {
                        s_drawer_modes_cursor = 1 - s_drawer_modes_cursor;
                        update_modes_drawer_strings();
                        oled_display_set_chord_drawer(CHORD_DRAWER_MODES, 50, s_cached_modes_ptrs, 2, s_drawer_modes_cursor, 0);
                    }
                }
            }
        }
        // --- Modo Acordes Convencional (HUD de Bolhas) quando nenhuma Gaveta esta aberta ---
        // Exige centro previamente pressionado por tempo suficiente (>= 200 ms) ou HUD latched, e tela permitida
        else if (!s_locked && is_chord_menu_allowed() && (s_chord_hud_latched || (joy_cur[JOY_CENTER] && center_press_start != 0 && (now - center_press_start >= 200)))) {
            if (joy_cur[JOY_LEFT] && !joy_prev[JOY_LEFT]) {
                open_playlists_drawer(now);
                center_modifier_used = true;
                s_chord_suppressed_mask |= (1 << JOY_LEFT);
            } else if (joy_cur[JOY_RIGHT] && !joy_prev[JOY_RIGHT]) {
                open_equalizer_drawer(now);
                center_modifier_used = true;
                s_chord_suppressed_mask |= (1 << JOY_RIGHT);
            } else if (joy_cur[JOY_UP] && !joy_prev[JOY_UP]) {
                s_chord_focused_dir = 0;
                center_modifier_used = true;
                s_chord_suppressed_mask |= (1 << JOY_UP);
            } else if (!joy_cur[JOY_UP] && joy_prev[JOY_UP] && (s_chord_focused_dir == 0)) {
                bool already_liked = false;
                if (s_mode == UI_MODE_LIST) {
                    already_liked = audio_player_is_browse_item_favorite(s_list_cursor);
                } else {
                    already_liked = audio_player_is_current_favorite();
                }
                handle_chord_like(!already_liked);
                s_chord_hud_latched = false;
                s_chord_focused_dir = -1;
                oled_display_set_chord_hud(false, 0, -1, false);
            } else if (joy_cur[JOY_DOWN] && !joy_prev[JOY_DOWN]) {
                s_chord_focused_dir = 1;
                center_modifier_used = true;
                s_chord_suppressed_mask |= (1 << JOY_DOWN);
            } else if (!joy_cur[JOY_DOWN] && joy_prev[JOY_DOWN] && (s_chord_focused_dir == 1)) {
                if (s_mode == UI_MODE_PLAYING) {
                    open_modes_drawer(now);
                    s_chord_hud_latched = false;
                    s_chord_focused_dir = -1;
                } else {
                    oled_display_trigger_bubble_expand_toast(64, 63, "OPCOES", "Menu de Acoes", NULL);
                    open_context_actions_menu();
                    s_chord_hud_latched = false;
                    s_chord_focused_dir = -1;
                    oled_display_set_chord_hud(false, 0, -1, false);
                }
            }
        }

        // Expande o HUD de Bolhas apenas se NAO estiver bloqueado, NAO for candidato a bloqueio e ESTIVER em tela permitida
        bool can_show_chord_hud = !s_locked && !center_is_lock_candidate && is_chord_menu_allowed();
        if (can_show_chord_hud && joy_cur[JOY_CENTER] && !center_long_fired && !center_modifier_used && center_press_start != 0) {
            uint32_t hold_time = now - center_press_start;
            if (hold_time >= 200) {
                // Cresce de 0 a 100% em 350 ms
                uint8_t pct = (uint8_t)(((hold_time - 200) * 100) / 350);
                if (pct >= 100) {
                    pct = 100;
                    s_chord_hud_latched = true; // Bolha inflada a 100%: destrava dedo, fica latched!
                }
                oled_display_set_chord_hud(true, pct, s_chord_focused_dir, s_chord_is_liked_cached);
            }
        } else if (s_chord_hud_latched && !joy_cur[JOY_CENTER] && is_chord_menu_allowed()) {
            // Bolhas permanecem abertas e estaveis a 100% aguardando selecao de direcao ou estouro
            oled_display_set_chord_hud(true, 100, s_chord_focused_dir, s_chord_is_liked_cached);
        } else if (s_chord_hud_latched && !is_chord_menu_allowed()) {
            s_chord_hud_latched = false;
            oled_display_set_chord_hud(false, 0, -1, false);
        }

        // Bloqueio de tela vs Acoes Especiais de Tela Segurando Centro:
        // Telas de configuracao e EQ possuem acao longa nativa direta (nao exigem Tap-Hold previo):
        bool is_screen_special_long = (s_mode == UI_MODE_LED || s_mode == UI_MODE_TELA ||
                                       s_mode == UI_MODE_DEEP_SLEEP || s_mode == UI_MODE_RTC_CLOCK ||
                                       s_mode == UI_MODE_EQ || s_mode == UI_MODE_EQ_PRESETS ||
                                       s_mode == UI_MODE_KEYBOARD || s_mode == UI_MODE_LOCK_CLOCK_CFG ||
                                       s_mode == UI_MODE_CHRONO || (s_mode == UI_MODE_TOP_SCREEN && s_top_cursor == 1));

        bool lock_time_reached = (now - center_press_start) >= CENTER_LOCK_HOLD_MS;
        bool can_trigger_long = (is_screen_special_long || s_locked || center_is_lock_candidate) && (s_active_chord_drawer == CHORD_DRAWER_NONE);

        if (joy_cur[JOY_CENTER] && !center_long_fired && !center_modifier_used && center_press_start != 0 &&
            lock_time_reached && can_trigger_long) {
            center_long_fired = true;
            oled_display_set_chord_hud(false, 0, -1, false);
            if (s_mode == UI_MODE_LED) {
                s_mode = UI_MODE_CONF_DISPLAY;
                s_conf_display_cursor = 1;
                rgb_led_save_nvs();
                ESP_LOGI(TAG, "Saindo da configuracao de LED");
            } else if (s_mode == UI_MODE_TELA) {
                s_mode = UI_MODE_CONF_DISPLAY;
                s_conf_display_cursor = 0;
                nvs_handle_t h;
                if (nvs_open("mps3_settings", NVS_READWRITE, &h) == ESP_OK) {
                    nvs_set_u8(h, "brightness", s_oled_brightness);
                    nvs_set_u8(h, "timeout", s_timeout_idx);
                    nvs_set_u8(h, "fallback", (uint8_t)s_fallback_idx);
                    nvs_commit(h);
                    nvs_close(h);
                }
                ESP_LOGI(TAG, "Saindo da tela TELA via pressao longa");
            } else if (s_mode == UI_MODE_DEEP_SLEEP) {
                s_mode = UI_MODE_CONF_SYSTEM;
                s_conf_system_cursor = 1;
                save_deepsleep_nvs();
                ESP_LOGI(TAG, "Saindo da tela DEEP SLEEP via pressao longa");
            } else if (s_mode == UI_MODE_RTC_CLOCK) {
                s_mode = UI_MODE_CONF_SYSTEM;
                s_conf_system_cursor = 0;
                ESP_LOGI(TAG, "Saindo da tela RELOGIO RTC via pressao longa");
            } else if (s_mode == UI_MODE_EQ) {
                s_mode = UI_MODE_EQ_PRESETS;
                ESP_LOGI(TAG, "EQ bandas salvas - voltando para a lista de presets");
            } else if (s_mode == UI_MODE_EQ_PRESETS) {
                player_eq_config_t cfg;
                audio_player_get_eq_config(&cfg);
                keyboard_start(cfg.presets[s_eq_preset_cursor].name, on_preset_renamed);
            } else if (s_mode == UI_MODE_KEYBOARD) {
                s_kbd_confirming = true;
                ESP_LOGI(TAG, "Teclado: solicitando confirmacao de salvar");
            } else if (s_mode == UI_MODE_TOP_SCREEN && s_top_cursor == 1) {
                s_prev_mode_before_eq = UI_MODE_TOP_SCREEN;
                s_eq_preset_cursor = s_top_eq_focus;
                s_mode = UI_MODE_EQ_PRESETS;
                ESP_LOGI(TAG, "Entrando na lista de presets a partir da Top Screen");
            } else if (s_mode == UI_MODE_LOCK_CLOCK_CFG) {
                s_mode = UI_MODE_CONF_DISPLAY;
                s_conf_display_cursor = 1;
                nvs_handle_t h;
                if (nvs_open("mps3_settings", NVS_READWRITE, &h) == ESP_OK) {
                    nvs_set_u8(h, "lock_clock", (uint8_t)s_lock_clock_timeout_idx);
                    nvs_commit(h);
                    nvs_close(h);
                }
                ESP_LOGI(TAG, "Saindo da tela LOCK CLOCK CFG via pressao longa");
            } else if (s_mode == UI_MODE_CHRONO) {
                s_mode = UI_MODE_LIST;
                s_list_cursor = 6;
                ESP_LOGI(TAG, "Saindo da tela CHRONO via pressao longa");
            } else {
                s_locked = !s_locked;
                center_is_lock_candidate = false;
                s_last_center_release_ms = 0;
                reset_seek_progression();
                ESP_LOGI(TAG, "Controles %s pelo botao central (Tap-Hold)", s_locked ? "bloqueados" : "desbloqueados");
                uint32_t lk_secs = touch_input_get_lock_clock_secs();
                if (s_locked) {
                    // Bloquear: circulo cresce do centro (0 -> 74 px) tampando a tela
                    oled_display_trigger_lock_anim(true);
                    if (lk_secs == 0) lk_secs = 10; // Exibe o relogio por 10s apos a animacao
                    s_lock_clock_until_ms = now + (lk_secs * 1000UL) + 400UL;
                    touch_input_wake_display();
                } else {
                    // Desbloquear: o inverso! circulo encolhe de 74 -> 0 px revelando a tela!
                    oled_display_trigger_lock_anim(false);
                    s_lock_clock_until_ms = 0; // Sai do relogio imediatamente
                    touch_input_wake_display();
                }
            }
        }
        if (center_released_edge) {
            if (s_chord_hud_latched && is_chord_menu_allowed()) {
                // Bolhas estao latched a 100%: o usuario soltou o meio para escolher com calma.
                // Mantem HUD aberto e anula clique curto de play/pause!
                center_modifier_used = false;
                s_last_center_release_ms = 0;
            } else {
                if (s_chord_hud_latched) {
                    s_chord_hud_latched = false;
                }
                oled_display_set_chord_hud(false, 0, -1, false);
                s_chord_focused_dir = -1;
                bool was_growing = is_chord_menu_allowed() && (center_press_start != 0) && ((now - center_press_start) >= 200);
                if (center_modifier_used || (s_chord_suppressed_mask & (1 << JOY_CENTER))) {
                    center_modifier_used = false;
                    s_chord_suppressed_mask &= ~(1 << JOY_CENTER);
                    s_last_center_release_ms = 0; // Usou modificador, anula tap
                } else if (was_growing) {
                    // Soltou antes de 100%: aborta expansao das bolhas sem play/pause
                    ESP_LOGI(TAG, "Expansao abortada antes de 100% - play/pause suprimido");
                    s_last_center_release_ms = 0;
                } else if (!center_long_fired && !s_locked) {
                s_last_center_release_ms = now; // Registra soltura do 1o clique curto!
                static uint32_t s_last_short_center_ms = 0;
                bool is_double_click = (now - s_last_short_center_ms) < 350;
                s_last_short_center_ms = now;

                if (s_mode == UI_MODE_ACTIONS_MENU) {
                    execute_context_action();
                } else if (menu_any_active()) {
                    menu_request_action_active();
                } else if (s_mode == UI_MODE_GAMES_MENU) {
                    if (s_games_cursor == 0) {
                        // Game of Life
                        oled_display_reset_life();
                        s_game_active = true;
                    } else if (s_games_cursor == 1) {
                        // Tetris (placeholder / aviso)
                        oled_display_show_message("TETRIS", "EM BREVE!");
                        vTaskDelay(pdMS_TO_TICKS(1000));
                    }
                } else if (s_mode == UI_MODE_CONF_MENU) {
                    if (s_conf_cursor == 0) {
                        s_mode = UI_MODE_CONF_AUDIO;
                        s_conf_audio_cursor = 0;
                    } else if (s_conf_cursor == 1) {
                        s_mode = UI_MODE_CONF_DISPLAY;
                        s_conf_display_cursor = 0;
                    } else if (s_conf_cursor == 2) {
                        s_mode = UI_MODE_CONF_SYSTEM;
                        s_conf_system_cursor = 0;
                    }
                } else if (s_mode == UI_MODE_CONF_AUDIO) {
                    if (s_conf_audio_cursor == 0) {
                        s_mode = UI_MODE_VOLUME;
                    } else if (s_conf_audio_cursor == 1) {
                        s_mode = UI_MODE_BALANCE;
                    } else if (s_conf_audio_cursor == 2) {
                        s_prev_mode_before_eq = UI_MODE_CONF_AUDIO;
                        s_mode = UI_MODE_EQ_PRESETS;
                        s_eq_preset_cursor = 0;
                    } else if (s_conf_audio_cursor == 3) {
                        s_mode = UI_MODE_SORT;
                    } else if (s_conf_audio_cursor == 4) {
                        audio_player_set_gapless_enabled(!audio_player_get_gapless_enabled());
                    } else if (s_conf_audio_cursor == 5) {
                        audio_player_set_soft_fade_enabled(!audio_player_get_soft_fade_enabled());
                    } else if (s_conf_audio_cursor == 6) {
                        replaygain_mode_t cur_rg = audio_player_get_replaygain_mode();
                        audio_player_set_replaygain_mode((replaygain_mode_t)(((int)cur_rg + 1) % 3));
                    } else if (s_conf_audio_cursor == 7) {
                        bookmark_mode_t cur_bm = audio_player_get_bookmark_mode();
                        audio_player_set_bookmark_mode((bookmark_mode_t)(((int)cur_bm + 1) % 3));
                    } else if (s_conf_audio_cursor == 8) {
                        audio_player_set_az_jump_enabled(!audio_player_get_az_jump_enabled());
                    }
                } else if (s_mode == UI_MODE_CONF_DISPLAY) {
                    if (s_conf_display_cursor == 0) {
                        s_mode = UI_MODE_TELA;
                        s_tela_cursor = 0;
                    } else if (s_conf_display_cursor == 1) {
                        s_mode = UI_MODE_LOCK_CLOCK_CFG;
                    } else if (s_conf_display_cursor == 2) {
                        s_mode = UI_MODE_LED;
                        s_led_channel = 0;
                    }
                } else if (s_mode == UI_MODE_LOCK_CLOCK_CFG) {
                    s_mode = UI_MODE_CONF_DISPLAY;
                    s_conf_display_cursor = 1;
                    nvs_handle_t h;
                    if (nvs_open("mps3_settings", NVS_READWRITE, &h) == ESP_OK) {
                        nvs_set_u8(h, "lock_clock", (uint8_t)s_lock_clock_timeout_idx);
                        nvs_commit(h);
                        nvs_close(h);
                    }
                    ESP_LOGI(TAG, "Saindo da tela LOCK CLOCK CFG (botao central)");
                } else if (s_mode == UI_MODE_CHRONO) {
                    touch_input_chrono_toggle();
                    ESP_LOGI(TAG, "Cronometro alternado: %s", s_chrono_running ? "Iniciado" : "Pausado");
                } else if (s_mode == UI_MODE_CONF_SYSTEM) {
                    if (s_conf_system_cursor == 0) {
                        s_mode = UI_MODE_RTC_CLOCK;
                    } else if (s_conf_system_cursor == 1) {
                        s_mode = UI_MODE_DEEP_SLEEP;
                    } else if (s_conf_system_cursor == 2) {
                        s_wifi_net_cursor = 0;
                        s_mode = UI_MODE_WIFI_NETS;
                    } else if (s_conf_system_cursor == 3) {
                        podcast_sync_start_manual();
                    }
                } else if (s_mode == UI_MODE_VOLUME || s_mode == UI_MODE_BALANCE) {
                    s_mode = UI_MODE_CONF_AUDIO;
                    s_conf_audio_cursor = (s_mode == UI_MODE_VOLUME) ? 0 : 1;
                    ESP_LOGI(TAG, "Saindo da tela de volume/balanco (botao central)");
                } else if (s_mode == UI_MODE_TELA) {
                    s_mode = UI_MODE_CONF_DISPLAY;
                    s_conf_display_cursor = 0;
                    nvs_handle_t h;
                    if (nvs_open("mps3_settings", NVS_READWRITE, &h) == ESP_OK) {
                        nvs_set_u8(h, "brightness", s_oled_brightness);
                        nvs_set_u8(h, "timeout", s_timeout_idx);
                        nvs_set_u8(h, "fallback", (uint8_t)s_fallback_idx);
                        nvs_commit(h);
                        nvs_close(h);
                    }
                    ESP_LOGI(TAG, "Saindo da tela TELA (botao central)");
                } else if (s_mode == UI_MODE_DEEP_SLEEP) {
                    s_mode = UI_MODE_CONF_SYSTEM;
                    s_conf_system_cursor = 1;
                    save_deepsleep_nvs();
                    ESP_LOGI(TAG, "Saindo da tela DEEP SLEEP (botao central)");
                } else if (s_mode == UI_MODE_RTC_CLOCK) {
                    s_mode = UI_MODE_CONF_SYSTEM;
                    s_conf_system_cursor = 0;
                    ESP_LOGI(TAG, "Saindo da tela RELOGIO RTC (botao central)");
                } else if (s_mode == UI_MODE_LED) {
                    rgb_led_toggle();
                    ESP_LOGI(TAG, "LED RGB alternado via botao central");
                } else if (s_mode == UI_MODE_EQ) {
                    player_eq_config_t cfg;
                    audio_player_get_eq_config(&cfg);
                    cfg.enabled = !cfg.enabled;
                    audio_player_set_eq_config(&cfg);
                    ESP_LOGI(TAG, "EQ %s via botao central", cfg.enabled ? "ligado" : "desligado");
                } else if (s_mode == UI_MODE_EQ_PRESETS) {
                    player_eq_config_t cfg;
                    audio_player_get_eq_config(&cfg);
                    if (cfg.enabled && cfg.active_preset_idx == s_eq_preset_cursor) {
                        cfg.enabled = false;
                    } else {
                        cfg.enabled = true;
                        cfg.active_preset_idx = s_eq_preset_cursor;
                    }
                    audio_player_set_eq_config(&cfg);
                    ESP_LOGI(TAG, "Preset %d ativo=%d", s_eq_preset_cursor, cfg.enabled);
                } else if (s_mode == UI_MODE_KEYBOARD) {
                    if (!s_kbd_confirming) {
                        if (is_double_click) {
                            // Reverte a alteracao feita no primeiro clique do duplo clique
                            strncpy(s_kbd_text, s_kbd_undo_text, sizeof(s_kbd_text) - 1);
                            s_kbd_text[sizeof(s_kbd_text) - 1] = '\0';
                            s_kbd_cursor = s_kbd_undo_cursor;
                            s_kbd_confirming = false;
                            s_kbd_page = (s_kbd_page + 1) % 3;
                            ESP_LOGI(TAG, "Teclado: pagina %d", s_kbd_page);
                        } else {
                            // Salva estado anterior para possibilitar rollback se for duplo clique
                            strncpy(s_kbd_undo_text, s_kbd_text, sizeof(s_kbd_undo_text) - 1);
                            s_kbd_undo_text[sizeof(s_kbd_undo_text) - 1] = '\0';
                            s_kbd_undo_cursor = s_kbd_cursor;

                            char ch = kbd_pages[s_kbd_page][s_kbd_grid_y][s_kbd_grid_x];
                            if (ch == '\r') {
                                s_kbd_confirming = true;
                            } else if (ch == '<') {
                                if (s_kbd_cursor > 0) {
                                    s_kbd_cursor--;
                                    s_kbd_text[s_kbd_cursor] = '\0';
                                }
                            } else if (ch == '_') {
                                if (s_kbd_cursor < 15) {
                                    s_kbd_text[s_kbd_cursor] = ' ';
                                    s_kbd_text[s_kbd_cursor + 1] = '\0';
                                    s_kbd_cursor++;
                                }
                            } else if (ch != '\0') {
                                if (s_kbd_cursor < 15) {
                                    s_kbd_text[s_kbd_cursor] = ch;
                                    s_kbd_text[s_kbd_cursor + 1] = '\0';
                                    s_kbd_cursor++;
                                }
                            }
                        }
                    }
                } else if (s_mode == UI_MODE_USB_PROMPT) {
                    if (s_list_cursor == 1) { // CIMA: DAC
                        usb_manager_set_mode(USB_MODE_DAC);
                        s_mode = UI_MODE_USB_DAC;
                    } else if (s_list_cursor == 2) { // DIREITA: MSC
                        usb_manager_set_mode(USB_MODE_MSC);
                        s_mode = UI_MODE_USB_MSC;
                    } else if (s_list_cursor == 3) { // ESQUERDA: COM (Flash)
                        oled_display_show_message("Entrando em modo", "FLASH / GRAVACAO");
                        vTaskDelay(pdMS_TO_TICKS(400));
                        usb_manager_enter_bootloader();
                    } else if (s_list_cursor == 4) { // BAIXO: SAIR
                        touch_input_cancel_usb();
                    } else { // 0 (Centro): Default para DAC
                        usb_manager_set_mode(USB_MODE_DAC);
                        s_mode = UI_MODE_USB_DAC;
                    }
                } else if (s_mode == UI_MODE_SORT) {
                    track_sort_mode_t cur = audio_player_get_sort_mode();
                    audio_player_set_sort_mode(cur == SORT_MODE_NAME ? SORT_MODE_DATE : SORT_MODE_NAME);

                } else if (s_mode == UI_MODE_WIFI_NETS) {
                    if (s_wifi_net_cursor == 0) {
                        s_mode = UI_MODE_LIST;
                        wifi_transfer_enter(WIFI_TRANSFER_MODE_AP);
                    } else {
                        char target_ssid[33] = {0};
                        char target_pass[65] = {0};
                        if (wifi_transfer_get_known_network(s_wifi_net_cursor - 1, target_ssid, sizeof(target_ssid), target_pass, sizeof(target_pass))) {
                            wifi_transfer_promote_network(target_ssid);
                            s_mode = UI_MODE_LIST;
                            wifi_transfer_enter(WIFI_TRANSFER_MODE_STA);
                        } else {
                            s_mode = UI_MODE_CONF_SYSTEM;
                            s_conf_system_cursor = 2;
                        }
                    }

                } else if (s_mode == UI_MODE_TOP_SCREEN) {
                    if (s_top_cursor == 1) {
                        player_eq_config_t *cfg = (player_eq_config_t *)malloc(sizeof(player_eq_config_t));
                        if (cfg) {
                            audio_player_get_eq_config(cfg);
                            if (cfg->enabled && cfg->active_preset_idx == s_top_eq_focus) {
                                cfg->enabled = false;
                            } else {
                                cfg->enabled = true;
                                cfg->active_preset_idx = s_top_eq_focus;
                            }
                            audio_player_set_eq_config(cfg);
                            free(cfg);
                        }
                    }
                } else if (s_mode == UI_MODE_USB_DAC) {
                    usb_manager_send_hid(0); // 0 = Play/Pause
                } else {
                    if (s_mode == UI_MODE_PLAYING && is_double_click) {
                        // Reverte o toggle play/pause disparado no primeiro clique e cicla o modo de visualizacao
                        audio_player_toggle_play_pause();
                        oled_display_cycle_viz_mode();
                        s_last_short_center_ms = 0; // Evita encadear 3º clique como duplo
                        ESP_LOGI(TAG, "Duplo-clique no centro: alternando modo de visualizacao -> %d", (int)oled_display_get_viz_mode());
                    } else if (s_mode == UI_MODE_LIST && s_in_player_browser && is_double_click) {
                        // Duplo-clique no navegador: reverte o play/pause do 1o clique e adiciona/remove o item na playlist alvo!
                        audio_player_toggle_play_pause();
                        s_last_short_center_ms = 0;
                        bool at_root = audio_player_browse_is_root();
                        int src_idx = at_root ? s_list_cursor : (s_list_cursor + 1);
                        bool added = false;
                        int cnt = 0;
                        const char *pl = audio_player_get_target_playlist();
                        if (audio_player_toggle_browse_item_in_target_playlist(src_idx, &added, &cnt)) {
                            if (cnt > 1) {
                                char msg[32];
                                snprintf(msg, sizeof(msg), "+ %d faixas adic.", cnt);
                                oled_display_show_message(pl, msg);
                            } else {
                                oled_display_show_message(pl, added ? "+ Faixa adicionada!" : "- Faixa removida!");
                            }
                            vTaskDelay(pdMS_TO_TICKS(800));
                        }
                    } else {
                        audio_player_toggle_play_pause();
                        reset_seek_progression();
                        if (s_mode == UI_MODE_PLAYING) {
                            oled_display_trigger_viz_overlay();
                        }
                    }
                }
            }
            }
            center_press_start = 0;
            center_modifier_used = false; // Reset pro proximo click
        }

        if (s_locked) {
            // O centro ja' foi avaliado acima. Todo o restante fica inativo
            // enquanto a tela permanece apagada pelo display_task.
            vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
            for (int i = 0; i < NUM_JOY_BTNS; i++) joy_prev[i] = joy_cur[i];
            continue;
        }

        bool is_active_mode = menu_any_active() || podcast_sync_is_busy() || (s_mode == UI_MODE_USB_MSC) || (s_mode == UI_MODE_USB_DAC);
        if (is_active_mode) {
            if (menu_any_active()) {
                static uint32_t s_menu_up_press_ms = 0;
                static uint32_t s_menu_up_repeat_ms = 0;
                static uint32_t s_menu_down_press_ms = 0;
                static uint32_t s_menu_down_repeat_ms = 0;

                bool up_fire = false;
                if (joy_cur[JOY_UP] && !joy_prev[JOY_UP]) {
                    s_menu_up_press_ms = now;
                    s_menu_up_repeat_ms = now;
                    up_fire = true;
                } else if (joy_cur[JOY_UP]) {
                    if ((now - s_menu_up_press_ms) >= JOY_REPEAT_INITIAL_MS &&
                        (now - s_menu_up_repeat_ms) >= JOY_REPEAT_MS) {
                        s_menu_up_repeat_ms = now;
                        up_fire = true;
                    }
                }

                bool down_fire = false;
                if (joy_cur[JOY_DOWN] && !joy_prev[JOY_DOWN]) {
                    s_menu_down_press_ms = now;
                    s_menu_down_repeat_ms = now;
                    down_fire = true;
                } else if (joy_cur[JOY_DOWN]) {
                    if ((now - s_menu_down_press_ms) >= JOY_REPEAT_INITIAL_MS &&
                        (now - s_menu_down_repeat_ms) >= JOY_REPEAT_MS) {
                        s_menu_down_repeat_ms = now;
                        down_fire = true;
                    }
                }

                if (up_fire) {
                    menu_nav_up_active();
                } else if (down_fire) {
                    menu_nav_down_active();
                } else if (joy_cur[JOY_RIGHT] && !joy_prev[JOY_RIGHT]) {
                    menu_nav_select_active();
                }
            }

            if (joy_cur[JOY_LEFT]) {
                if (podcast_sync_is_busy() && !joy_prev[JOY_LEFT]) {
                    ESP_LOGI(TAG, "JOY_LEFT pressionado - cancelando sincronizacao de podcasts");
                    podcast_sync_request_cancel();
                }
                if (exit_active_hold_start == 0) exit_active_hold_start = now;
                if (!exit_active_fired && (now - exit_active_hold_start) >= EXIT_ACTIVE_MODE_HOLD_MS) {
                    exit_active_fired = true;
                    ESP_LOGI(TAG, "JOY_LEFT segurado - saindo do modo ativo");
                    if (podcast_sync_is_busy()) {
                        ESP_LOGI(TAG, "Cancelando sincronizacao de podcasts a pedido do usuario");
                        podcast_sync_request_cancel();
                    } else if (menu_any_active()) {
                        menu_request_exit_active();
                    } else {
                        // Sair do USB
                        s_mode = UI_MODE_PLAYING;
                        usb_manager_set_mode(USB_MODE_NONE);
                    }
                }
            } else {
                if (joy_prev[JOY_LEFT] && !exit_active_fired) {
                    if (menu_any_active()) {
                        menu_nav_left_active();
                    }
                }
                exit_active_hold_start = 0;
                exit_active_fired = false;
            }

            // No modo DAC, ainda permitimos botoes de borda para HID
            if (s_mode != UI_MODE_USB_DAC) {
                vTaskDelay(pdMS_TO_TICKS(wifi_transfer_is_boost_active() ? 500 : POLL_INTERVAL_MS));
                for (int i = 0; i < NUM_JOY_BTNS; i++) joy_prev[i] = joy_cur[i];
                continue;
            }
        }

        
        // Auto-idle to PLAYING configurável (0 = desativado)
        uint32_t fallback_sec = (s_fallback_idx >= 0 && s_fallback_idx < FALLBACK_OPTIONS_COUNT) ? (uint32_t)FALLBACK_SECS[s_fallback_idx] : 0;
        if (fallback_sec > 0 && s_mode == UI_MODE_LIST && !menu_any_active() && (now - s_last_activity_ms > (fallback_sec * 1000UL))) {
            playback_state_t st;
            audio_player_get_state(&st);
            if (st.playing || st.track_loaded) {
                s_mode = UI_MODE_PLAYING;
                ESP_LOGI(TAG, "Inatividade (%u s): retornando a tela de reproducao", (unsigned)fallback_sec);
            }
        }
        
        // --- Joystick: navegacao / selecao / voltar / avanco-retrocesso ----
        // JOY_CENTER fica fora: ele foi tratado acima.
        for (int i = 0; i < NUM_JOY_BTNS; i++) {
            if (i == JOY_CENTER) continue;

            bool pressed_edge = joy_cur[i] && !joy_prev[i];

            // Limpa mascara de supressao se o botao foi solto e o tempo de cooldown de acorde expirou
            if (!joy_cur[i] && (now - s_last_chord_action_ms >= 350)) {
                s_chord_suppressed_mask &= ~(1 << i);
            }

            // Se as bolhas estao abertas (latched), ou centro pressionado por tempo suficiente (>= 200 ms) em tela permitida, ou cooldown ativo:
            // suprime TOTALMENTE comandos de 1a ordem direcionais!
            bool chord_suppression_active = (s_active_chord_drawer != CHORD_DRAWER_NONE) ||
                                            (s_chord_hud_latched && is_chord_menu_allowed()) ||
                                            (is_chord_menu_allowed() && joy_cur[JOY_CENTER] && center_press_start != 0 && (now - center_press_start >= 200)) ||
                                            (s_chord_suppressed_mask & (1 << i)) ||
                                            (now - s_last_chord_action_ms < 350);
            if (chord_suppression_active) {
                joy_pending_edge_ms[i] = 0;
                continue;
            }

            // Em modos onde ha acordes (PLAYING e LIST permitidos), se uma direcao subiu sem o Centro,
            // aguarda janela de 50 ms para detectar se o Centro foi acionado simultaneamente
            bool is_chord_eligible_mode = is_chord_menu_allowed();
            if (pressed_edge && is_chord_eligible_mode) {
                joy_pending_edge_ms[i] = now;
                joy_press_ms[i] = now;
                continue; // Aguarda janela de 50 ms
            }

            bool resolved_edge = false;
            if (joy_pending_edge_ms[i] > 0) {
                if ((now - joy_pending_edge_ms[i]) >= 50) {
                    joy_pending_edge_ms[i] = 0;
                    resolved_edge = true;
                } else {
                    continue; // Continua aguardando janela
                }
            }

            bool is_updown_repeatable = (i == JOY_UP || i == JOY_DOWN);
            bool is_seek_repeatable = (i == JOY_LEFT || i == JOY_RIGHT) && s_mode == UI_MODE_PLAYING;
            bool is_lr_repeatable = (i == JOY_LEFT || i == JOY_RIGHT) && (s_mode == UI_MODE_VOLUME || s_mode == UI_MODE_BALANCE || s_mode == UI_MODE_EQ || s_mode == UI_MODE_LED || (s_mode == UI_MODE_TOP_SCREEN && s_top_cursor == 0));

            bool released_edge = !joy_cur[i] && joy_prev[i];
            if (released_edge) {
                if (s_mode == UI_MODE_PLAYING && (i == JOY_RIGHT || i == JOY_LEFT)) {
                    uint32_t held_for = now - joy_press_ms[i];
                    if (held_for < 350) {
                        // Toque curto: pular ou retroceder faixa
                        if (i == JOY_RIGHT) {
                            ESP_LOGI(TAG, "Clique curto JOY_RIGHT: avancar proxima faixa");
                            audio_player_next();
                        } else {
                            playback_state_t cur_st;
                            audio_player_get_state(&cur_st);
                            if (cur_st.elapsed_sec > 3) {
                                ESP_LOGI(TAG, "Clique curto JOY_LEFT (>3s): reiniciar faixa");
                                audio_player_restart();
                            } else {
                                ESP_LOGI(TAG, "Clique curto JOY_LEFT (<=3s): faixa anterior");
                                audio_player_previous();
                            }
                        }
                    } else {
                        // Soltou apos segurar (seek continuo)
                        ESP_LOGI(TAG, "Soltura de seek apos %u ms", (unsigned)held_for);
                        reset_seek_progression();
                    }
                }
            }

            bool effective_edge = pressed_edge || resolved_edge;
            bool fire = effective_edge;
            if (effective_edge && is_seek_repeatable) {
                // Na borda de subida em modo Now Playing, nao da seek imediato (aguarda hold ou clique curto na soltura)
                fire = false;
            } else if (!fire && joy_cur[i]) {
                if (is_updown_repeatable || is_lr_repeatable) {
                    uint32_t held_for = now - joy_press_ms[i];
                    if (held_for >= JOY_REPEAT_INITIAL_MS &&
                        (now - joy_last_repeat_ms[i]) >= JOY_REPEAT_MS) {
                        fire = true;
                    }
                } else if (is_seek_repeatable) {
                    uint32_t held_for = now - joy_press_ms[i];
                    if (held_for >= 350 && (now - joy_last_repeat_ms[i]) >= SEEK_HOLD_PULSE_MS) {
                        fire = true;
                    }
                }
            }

            if (pressed_edge) {
                joy_press_ms[i] = now;
            }
            pressed_edge = effective_edge; // Propaga borda efetiva para o switch interno!
            if (fire) {
                joy_last_repeat_ms[i] = now;
                s_last_interaction_ms = now;
                switch (i) {
                    case JOY_UP:
                        if (s_mode == UI_MODE_ACTIONS_MENU && pressed_edge) {
                            int max_act = s_action_target_is_playlist ? 8 : 13;
                            s_action_cursor = (s_action_cursor == 0) ? (max_act - 1) : (s_action_cursor - 1);
                        } else if (s_mode == UI_MODE_USB_PROMPT) {
                            if (s_list_cursor == 1 && pressed_edge) {
                                usb_manager_set_mode(USB_MODE_DAC);
                                s_mode = UI_MODE_USB_DAC;
                            } else {
                                s_list_cursor = 1; // Seleciona CIMA: DAC
                            }
                        } else if (s_mode == UI_MODE_GAMES_MENU) {
                            s_games_cursor = (s_games_cursor == 0) ? 1 : 0;
                        } else if (s_mode == UI_MODE_CONF_MENU) {
                            s_conf_cursor = (s_conf_cursor == 0) ? 2 : (s_conf_cursor - 1);
                        } else if (s_mode == UI_MODE_CONF_AUDIO) {
                            s_conf_audio_cursor = (s_conf_audio_cursor == 0) ? 8 : (s_conf_audio_cursor - 1);
                        } else if (s_mode == UI_MODE_CONF_DISPLAY) {
                            s_conf_display_cursor = (s_conf_display_cursor == 0) ? 2 : (s_conf_display_cursor - 1);
                        } else if (s_mode == UI_MODE_LOCK_CLOCK_CFG && pressed_edge) {
                            if (s_lock_clock_timeout_idx > 0) s_lock_clock_timeout_idx--;
                        } else if (s_mode == UI_MODE_CHRONO && pressed_edge) {
                            touch_input_chrono_toggle();
                        } else if (s_mode == UI_MODE_CONF_SYSTEM) {
                            s_conf_system_cursor = (s_conf_system_cursor == 0) ? 3 : (s_conf_system_cursor - 1);
                        } else if (s_mode == UI_MODE_DEEP_SLEEP) {
                            if (s_deepsleep_idx > 0) s_deepsleep_idx--;
                            else s_deepsleep_idx = DEEPSLEEP_OPTIONS_COUNT - 1;
                        } else if (s_mode == UI_MODE_WIFI_NETS) {
                            int total_nets = 1 + wifi_transfer_get_known_count();
                            s_wifi_net_cursor = (s_wifi_net_cursor == 0) ? (total_nets - 1) : (s_wifi_net_cursor - 1);
                        } else if (s_mode == UI_MODE_SORT && pressed_edge) {
                            track_sort_mode_t cur = audio_player_get_sort_mode();
                            audio_player_set_sort_mode(cur == SORT_MODE_NAME ? SORT_MODE_DATE : SORT_MODE_NAME);
                        } else if (s_mode == UI_MODE_BALANCE && pressed_edge) {
                            s_mode = UI_MODE_CONF_AUDIO;
                            s_conf_audio_cursor = 1;
                        } else if (s_mode == UI_MODE_LED && pressed_edge) {
                            s_led_channel = (s_led_channel == 0) ? 2 : (s_led_channel - 1);
                        } else if (s_mode == UI_MODE_PLAYING && pressed_edge) {
                            s_mode = UI_MODE_TOP_SCREEN;
                            s_top_cursor = 0; // Entra na Top Screen selecionando Volume
                            ESP_LOGI(TAG, "Entrando na Top Screen (Volume)");
                        } else if (s_mode == UI_MODE_TOP_SCREEN && pressed_edge) {
                            if (s_top_cursor == 0) s_top_cursor = 1;      // Volume -> EQ
                            else if (s_top_cursor == 1) s_top_cursor = 2; // EQ -> Bateria
                        } else if (s_mode == UI_MODE_TELA && pressed_edge) {
                            s_tela_cursor = (s_tela_cursor == 0) ? 2 : (s_tela_cursor - 1);
                        } else if (s_mode == UI_MODE_VOLUME) {
                            audio_player_adjust_volume(VOLUME_STEP_PERCENT);
                        } else if (s_mode == UI_MODE_EQ_PRESETS) {
                            if (s_eq_preset_cursor > 0) s_eq_preset_cursor--;
                            else s_eq_preset_cursor = 9;
                        } else if (s_mode == UI_MODE_KEYBOARD) {
                            if (!s_kbd_confirming && s_kbd_grid_y > 0) {
                                s_kbd_grid_y--;
                                int max_x = (s_kbd_grid_y == 1) ? 8 : 9;
                                if (s_kbd_grid_x > max_x) s_kbd_grid_x = max_x;
                            }
                        } else if (s_mode == UI_MODE_EQ) {
                            player_eq_config_t cfg;
                            audio_player_get_eq_config(&cfg);
                            float *gain_ptr = (s_eq_band < 10) ? &cfg.presets[cfg.active_preset_idx].band_gains[s_eq_band] : &cfg.presets[cfg.active_preset_idx].overall_gain;
                            *gain_ptr += EQ_STEP_DB;
                            float max_db = (s_eq_band < 10) ? 15.0f : 30.0f;
                            if (*gain_ptr > max_db) *gain_ptr = max_db;
                            audio_player_set_eq_config(&cfg);
                        } else if (s_mode == UI_MODE_USB_DAC) {
                            audio_player_adjust_volume(VOLUME_STEP_PERCENT);
                            usb_manager_send_hid(3);
                        } else if (s_mode == UI_MODE_LIST) {
                            uint32_t held_ms = now - joy_press_ms[i];
                            if (!pressed_edge && s_in_player_browser && audio_player_get_az_jump_enabled() &&
                                held_ms >= 950 && current_list_count() > 10) {
                                bool at_root = audio_player_browse_is_root();
                                int offset = at_root ? 0 : 1;
                                char let = 0;
                                int new_real = audio_player_jump_browse_letter(s_list_cursor + offset, -1, &let);
                                int new_cur = new_real - offset;
                                if (new_cur < 0) new_cur = 0;
                                s_list_cursor = new_cur;
                                if (let != 0) oled_display_trigger_az_hud(let);
                            } else {
                                joy_move_cursor(-1, pressed_edge);
                            }
                        }
                        break;
                    case JOY_DOWN:
                        if (s_mode == UI_MODE_ACTIONS_MENU && pressed_edge) {
                            int max_act = s_action_target_is_playlist ? 8 : 13;
                            s_action_cursor = (s_action_cursor + 1) % max_act;
                        } else if (s_mode == UI_MODE_USB_PROMPT) {
                            if (s_list_cursor == 4 && pressed_edge) {
                                touch_input_cancel_usb();
                            } else {
                                s_list_cursor = 4; // Seleciona BAIXO: SAIR
                            }
                        } else if (s_mode == UI_MODE_GAMES_MENU) {
                            s_games_cursor = (s_games_cursor + 1) % 2;
                        } else if (s_mode == UI_MODE_CONF_MENU) {
                            s_conf_cursor = (s_conf_cursor + 1) % 3;
                        } else if (s_mode == UI_MODE_CONF_AUDIO) {
                            s_conf_audio_cursor = (s_conf_audio_cursor + 1) % 9;
                        } else if (s_mode == UI_MODE_CONF_DISPLAY) {
                            s_conf_display_cursor = (s_conf_display_cursor + 1) % 3;
                        } else if (s_mode == UI_MODE_LOCK_CLOCK_CFG && pressed_edge) {
                            if (s_lock_clock_timeout_idx < (LOCK_CLOCK_OPTIONS_COUNT - 1)) s_lock_clock_timeout_idx++;
                        } else if (s_mode == UI_MODE_CHRONO && pressed_edge) {
                            touch_input_chrono_lap_or_reset();
                        } else if (s_mode == UI_MODE_CONF_SYSTEM) {
                            s_conf_system_cursor = (s_conf_system_cursor + 1) % 4;
                        } else if (s_mode == UI_MODE_DEEP_SLEEP) {
                            s_deepsleep_idx = (s_deepsleep_idx + 1) % DEEPSLEEP_OPTIONS_COUNT;
                        } else if (s_mode == UI_MODE_WIFI_NETS) {
                            int total_nets = 1 + wifi_transfer_get_known_count();
                            s_wifi_net_cursor = (s_wifi_net_cursor + 1) % total_nets;
                        } else if (s_mode == UI_MODE_SORT && pressed_edge) {
                            track_sort_mode_t cur = audio_player_get_sort_mode();
                            audio_player_set_sort_mode(cur == SORT_MODE_NAME ? SORT_MODE_DATE : SORT_MODE_NAME);
                        } else if (s_mode == UI_MODE_BALANCE && pressed_edge) {
                            s_mode = UI_MODE_CONF_AUDIO;
                            s_conf_audio_cursor = 1;
                        } else if (s_mode == UI_MODE_LED && pressed_edge) {
                            s_led_channel = (s_led_channel + 1) % 3;
                        } else if (s_mode == UI_MODE_TOP_SCREEN && pressed_edge) {
                            if (s_top_cursor == 2) {
                                s_top_cursor = 1;      // Bateria -> EQ
                            } else if (s_top_cursor == 1) {
                                s_top_cursor = 0;      // EQ -> Volume
                            } else if (s_top_cursor == 0) {
                                s_mode = UI_MODE_PLAYING; // Volume -> Tela de reproducao!
                                ESP_LOGI(TAG, "Saindo da Top Screen -> Reproducao");
                            }
                        } else if (s_mode == UI_MODE_TELA && pressed_edge) {
                            s_tela_cursor = (s_tela_cursor + 1) % 3;
                        } else if (s_mode == UI_MODE_VOLUME) {
                            audio_player_adjust_volume(-VOLUME_STEP_PERCENT);
                        } else if (s_mode == UI_MODE_EQ_PRESETS) {
                            if (s_eq_preset_cursor < 9) s_eq_preset_cursor++;
                            else s_eq_preset_cursor = 0;
                        } else if (s_mode == UI_MODE_KEYBOARD) {
                            if (!s_kbd_confirming && s_kbd_grid_y < 2) {
                                s_kbd_grid_y++;
                                int max_x = (s_kbd_grid_y == 1) ? 8 : 9;
                                if (s_kbd_grid_x > max_x) s_kbd_grid_x = max_x;
                            }
                        } else if (s_mode == UI_MODE_EQ) {
                            player_eq_config_t cfg;
                            audio_player_get_eq_config(&cfg);
                            float *gain_ptr = (s_eq_band < 10) ? &cfg.presets[cfg.active_preset_idx].band_gains[s_eq_band] : &cfg.presets[cfg.active_preset_idx].overall_gain;
                            *gain_ptr -= EQ_STEP_DB;
                            float min_db = (s_eq_band < 10) ? -15.0f : -30.0f;
                            if (*gain_ptr < min_db) *gain_ptr = min_db;
                            audio_player_set_eq_config(&cfg);
                        } else if (s_mode == UI_MODE_PLAYING) {
                            if (pressed_edge) {
                                s_mode = UI_MODE_LIST;
                                int cur_track = audio_player_get_current_track_browse_index();
                                if (cur_track >= 0) {
                                    int max_c = current_list_count();
                                    if (cur_track < max_c) {
                                        s_list_cursor = cur_track;
                                    }
                                }
                                ESP_LOGI(TAG, "Voltando para a lista (JOY_DOWN, cursor=%d)", s_list_cursor);
                            }
                        } else if (s_mode == UI_MODE_USB_DAC) {
                            audio_player_adjust_volume(-VOLUME_STEP_PERCENT);
                            usb_manager_send_hid(4);
                        } else if (s_mode == UI_MODE_LIST) {
                            uint32_t held_ms = now - joy_press_ms[i];
                            if (!pressed_edge && s_in_player_browser && audio_player_get_az_jump_enabled() &&
                                held_ms >= 950 && current_list_count() > 10) {
                                bool at_root = audio_player_browse_is_root();
                                int offset = at_root ? 0 : 1;
                                char let = 0;
                                int new_real = audio_player_jump_browse_letter(s_list_cursor + offset, +1, &let);
                                int new_cur = new_real - offset;
                                int max_c = current_list_count();
                                if (new_cur < 0) new_cur = 0;
                                if (max_c > 0 && new_cur >= max_c) new_cur = max_c - 1;
                                s_list_cursor = new_cur;
                                if (let != 0) oled_display_trigger_az_hud(let);
                            } else {
                                joy_move_cursor(+1, pressed_edge);
                            }
                        }
                        break;
                    case JOY_LEFT:
                        if (pressed_edge) {
                            static uint32_t s_last_left_tap_ms = 0;
                            static int s_left_taps = 0;
                            if (now - s_last_left_tap_ms < 350) {
                                s_left_taps++;
                            } else {
                                s_left_taps = 1;
                            }
                            s_last_left_tap_ms = now;

                            if (s_left_taps >= 3) {
                                s_left_taps = 0;
                                s_mode = UI_MODE_LIST;
                                s_list_cursor = 0;
                                s_in_player_browser = false;
                                audio_player_browse_to_root();
                                oled_display_trigger_heart_anim(true, "MENU PRINCIPAL");
                                break;
                            }
                        }
                        if (s_mode == UI_MODE_ACTIONS_MENU && pressed_edge) {
                            s_mode = s_prev_mode_before_actions;
                        } else if (s_mode == UI_MODE_USB_PROMPT && pressed_edge) {
                            if (s_list_cursor == 3) {
                                oled_display_show_message("Entrando em modo", "FLASH / GRAVACAO");
                                vTaskDelay(pdMS_TO_TICKS(400));
                                usb_manager_enter_bootloader();
                            } else {
                                s_list_cursor = 3; // Seleciona ESQUERDA: COM (Flash)
                            }
                        } else if (s_mode == UI_MODE_GAMES_MENU && pressed_edge) {
                            s_mode = UI_MODE_LIST;
                            s_list_cursor = 5; // Retorna para "Games" (indice 5) no menu principal
                        } else if (s_mode == UI_MODE_CONF_MENU && pressed_edge) {
                            s_mode = UI_MODE_LIST;
                            s_list_cursor = 3; // Retorna para "Conf" (indice 3) no menu principal
                        } else if (s_mode == UI_MODE_CONF_AUDIO && pressed_edge) {
                            s_mode = UI_MODE_CONF_MENU;
                            s_conf_cursor = 0;
                        } else if (s_mode == UI_MODE_CONF_DISPLAY && pressed_edge) {
                            s_mode = UI_MODE_CONF_MENU;
                            s_conf_cursor = 1;
                        } else if (s_mode == UI_MODE_LOCK_CLOCK_CFG && pressed_edge) {
                            s_mode = UI_MODE_CONF_DISPLAY;
                            s_conf_display_cursor = 1;
                            nvs_handle_t h;
                            if (nvs_open("mps3_settings", NVS_READWRITE, &h) == ESP_OK) {
                                nvs_set_u8(h, "lock_clock", (uint8_t)s_lock_clock_timeout_idx);
                                nvs_commit(h);
                                nvs_close(h);
                            }
                        } else if (s_mode == UI_MODE_CHRONO && pressed_edge) {
                            s_mode = UI_MODE_LIST;
                            s_list_cursor = 6; // Retorna para "Chrono" no menu principal
                        } else if (s_mode == UI_MODE_CONF_SYSTEM && pressed_edge) {
                            s_mode = UI_MODE_CONF_MENU;
                            s_conf_cursor = 2;
                        } else if (s_mode == UI_MODE_DEEP_SLEEP && pressed_edge) {
                            s_mode = UI_MODE_CONF_SYSTEM;
                            s_conf_system_cursor = 1;
                            save_deepsleep_nvs();
                        } else if (s_mode == UI_MODE_RTC_CLOCK && pressed_edge) {
                            s_mode = UI_MODE_CONF_SYSTEM;
                            s_conf_system_cursor = 0;
                        } else if (s_mode == UI_MODE_WIFI_NETS && pressed_edge) {
                            s_mode = UI_MODE_CONF_SYSTEM;
                            s_conf_system_cursor = 2; // Retorna para "Redes Wi-Fi" no submenu Sistema
                        } else if (s_mode == UI_MODE_SORT && pressed_edge) {
                            s_mode = UI_MODE_CONF_AUDIO;
                            s_conf_audio_cursor = 3;
                        } else if (s_mode == UI_MODE_TELA && pressed_edge) {
                            if (s_tela_cursor == 0) {
                                int next_idx = 0;
                                for (int k = 9; k >= 0; k--) {
                                    if (BRIGHTNESS_CURVE[k] < s_oled_brightness) {
                                        next_idx = k; break;
                                    }
                                }
                                s_oled_brightness = BRIGHTNESS_CURVE[next_idx];
                                oled_display_set_brightness(s_oled_brightness);
                            } else if (s_tela_cursor == 1) {
                                if (s_timeout_idx > 0) s_timeout_idx--;
                            } else if (s_tela_cursor == 2) {
                                if (s_fallback_idx > 0) s_fallback_idx--;
                            }
                        } else if (s_mode == UI_MODE_LED) {
                            rgb_led_config_t cfg;
                            rgb_led_get_config(&cfg);
                            uint8_t *val = (s_led_channel == 0) ? &cfg.r : (s_led_channel == 1 ? &cfg.g : &cfg.b);
                            if (*val >= 15) *val -= 15; else *val = 0;
                            rgb_led_set_config(&cfg);
                        } else if (s_mode == UI_MODE_TOP_SCREEN) {
                            if (s_top_cursor == 0) {
                                audio_player_adjust_volume(-VOLUME_STEP_PERCENT);
                            } else if (s_top_cursor == 1 && pressed_edge) {
                                if (s_top_eq_focus > 0) s_top_eq_focus--;
                                else s_top_eq_focus = 9;
                            }
                        } else if (s_mode == UI_MODE_VOLUME) {
                            audio_player_adjust_volume(-VOLUME_STEP_PERCENT);
                        } else if (s_mode == UI_MODE_BALANCE) {
                            audio_player_adjust_balance(-1);
                        } else if (s_mode == UI_MODE_EQ_PRESETS && pressed_edge) {
                            s_mode = s_prev_mode_before_eq;
                        } else if (s_mode == UI_MODE_KEYBOARD && pressed_edge) {
                            if (s_kbd_confirming) {
                                s_kbd_confirming = false; // esquerda volta para o teclado
                            } else {
                                if (s_kbd_grid_x > 0) s_kbd_grid_x--;
                            }
                        } else if (s_mode == UI_MODE_EQ) {
                            if (pressed_edge) {
                                s_eq_band--;
                                if (s_eq_band < 0) s_eq_band = 10; // 10 bandas + 1 geral = 11 posicoes (0 a 10)
                            }
                        } else if (s_mode == UI_MODE_PLAYING) {
                            oled_display_trigger_viz_overlay();
                            joy_seek_hold_pulse(-1, now, now - joy_press_ms[i], pressed_edge);
                        } else if (s_mode == UI_MODE_USB_DAC && pressed_edge) {
                            usb_manager_send_hid(2); // Prev
                        } else if (s_mode == UI_MODE_LIST && pressed_edge) {
                            bool at_root = audio_player_browse_is_root();
                            bool in_player = s_in_player_browser;
                            if (at_root && !in_player) {
                                // No Menu Principal, JOY_LEFT abre Now Playing
                                s_mode = UI_MODE_PLAYING;
                                ESP_LOGI(TAG, "Menu principal: JOY_LEFT -> UI_MODE_PLAYING");
                            } else {
                                ESP_LOGI(TAG, "JOY_LEFT pressionado");
                                joy_back();
                            }
                        }
                        break;
                    case JOY_RIGHT:
                        if (s_mode == UI_MODE_ACTIONS_MENU && pressed_edge) {
                            execute_context_action();
                        } else if (s_mode == UI_MODE_USB_PROMPT && pressed_edge) {
                            if (s_list_cursor == 2) {
                                usb_manager_set_mode(USB_MODE_MSC);
                                s_mode = UI_MODE_USB_MSC;
                            } else {
                                s_list_cursor = 2; // Seleciona DIREITA: MSC (Pendrive)
                            }
                        } else if (s_mode == UI_MODE_GAMES_MENU && pressed_edge) {
                            if (s_games_cursor == 0) {
                                // Game of Life
                                oled_display_reset_life();
                                s_game_active = true;
                            } else if (s_games_cursor == 1) {
                                // Tetris (placeholder / aviso)
                                oled_display_show_message("TETRIS", "EM BREVE!");
                                vTaskDelay(pdMS_TO_TICKS(1000));
                            }
                        } else if (s_mode == UI_MODE_CONF_MENU && pressed_edge) {
                            if (s_conf_cursor == 0) {
                                s_mode = UI_MODE_CONF_AUDIO;
                                s_conf_audio_cursor = 0;
                            } else if (s_conf_cursor == 1) {
                                s_mode = UI_MODE_CONF_DISPLAY;
                                s_conf_display_cursor = 0;
                            } else if (s_conf_cursor == 2) {
                                s_mode = UI_MODE_CONF_SYSTEM;
                                s_conf_system_cursor = 0;
                            }
                        } else if (s_mode == UI_MODE_CONF_AUDIO && pressed_edge) {
                            if (s_conf_audio_cursor == 0) {
                                s_mode = UI_MODE_VOLUME;
                            } else if (s_conf_audio_cursor == 1) {
                                s_mode = UI_MODE_BALANCE;
                            } else if (s_conf_audio_cursor == 2) {
                                s_prev_mode_before_eq = UI_MODE_CONF_AUDIO;
                                s_mode = UI_MODE_EQ_PRESETS;
                                s_eq_preset_cursor = 0;
                            } else if (s_conf_audio_cursor == 3) {
                                s_mode = UI_MODE_SORT;
                            } else if (s_conf_audio_cursor == 4) {
                                audio_player_set_gapless_enabled(!audio_player_get_gapless_enabled());
                            } else if (s_conf_audio_cursor == 5) {
                                audio_player_set_soft_fade_enabled(!audio_player_get_soft_fade_enabled());
                            } else if (s_conf_audio_cursor == 6) {
                                replaygain_mode_t cur_rg = audio_player_get_replaygain_mode();
                                audio_player_set_replaygain_mode((replaygain_mode_t)(((int)cur_rg + 1) % 3));
                            } else if (s_conf_audio_cursor == 7) {
                                bookmark_mode_t cur_bm = audio_player_get_bookmark_mode();
                                audio_player_set_bookmark_mode((bookmark_mode_t)(((int)cur_bm + 1) % 3));
                            } else if (s_conf_audio_cursor == 8) {
                                audio_player_set_az_jump_enabled(!audio_player_get_az_jump_enabled());
                            }
                        } else if (s_mode == UI_MODE_CONF_DISPLAY && pressed_edge) {
                            if (s_conf_display_cursor == 0) {
                                s_mode = UI_MODE_TELA;
                                s_tela_cursor = 0;
                            } else if (s_conf_display_cursor == 1) {
                                s_mode = UI_MODE_LOCK_CLOCK_CFG;
                            } else if (s_conf_display_cursor == 2) {
                                s_mode = UI_MODE_LED;
                                s_led_channel = 0;
                            }
                        } else if (s_mode == UI_MODE_LOCK_CLOCK_CFG && pressed_edge) {
                            if (s_lock_clock_timeout_idx < (LOCK_CLOCK_OPTIONS_COUNT - 1)) s_lock_clock_timeout_idx++;
                        } else if (s_mode == UI_MODE_CHRONO && pressed_edge) {
                            touch_input_chrono_lap_or_reset();
                        } else if (s_mode == UI_MODE_CONF_SYSTEM && pressed_edge) {
                            if (s_conf_system_cursor == 0) {
                                s_mode = UI_MODE_RTC_CLOCK;
                            } else if (s_conf_system_cursor == 1) {
                                s_mode = UI_MODE_DEEP_SLEEP;
                            } else if (s_conf_system_cursor == 2) {
                                s_wifi_net_cursor = 0;
                                s_mode = UI_MODE_WIFI_NETS;
                            } else if (s_conf_system_cursor == 3) {
                                podcast_sync_start_manual();
                            }
                        } else if (s_mode == UI_MODE_DEEP_SLEEP && pressed_edge) {
                            ESP_LOGI(TAG, "JOY_RIGHT: Forcando Deep Sleep imediato como teste de bancada...");
                            save_deepsleep_nvs();
                            pwr_governor_enter_deep_sleep();
                        } else if (s_mode == UI_MODE_WIFI_NETS && pressed_edge) {
                            if (s_wifi_net_cursor == 0) {
                                s_mode = UI_MODE_LIST;
                                wifi_transfer_enter(WIFI_TRANSFER_MODE_AP);
                            } else {
                                char target_ssid[33] = {0};
                                char target_pass[65] = {0};
                                if (wifi_transfer_get_known_network(s_wifi_net_cursor - 1, target_ssid, sizeof(target_ssid), target_pass, sizeof(target_pass))) {
                                    wifi_transfer_promote_network(target_ssid);
                                    s_mode = UI_MODE_LIST;
                                    wifi_transfer_enter(WIFI_TRANSFER_MODE_STA);
                                }
                            }
                        } else if (s_mode == UI_MODE_SORT && pressed_edge) {
                            track_sort_mode_t cur = audio_player_get_sort_mode();
                            audio_player_set_sort_mode(cur == SORT_MODE_NAME ? SORT_MODE_DATE : SORT_MODE_NAME);
                        } else if (s_mode == UI_MODE_BALANCE) {
                            audio_player_adjust_balance(+1);
                        } else if (s_mode == UI_MODE_TELA && pressed_edge) {
                            if (s_tela_cursor == 0) {
                                int next_idx = 9;
                                for (int k = 0; k <= 9; k++) {
                                    if (BRIGHTNESS_CURVE[k] > s_oled_brightness) {
                                        next_idx = k; break;
                                    }
                                }
                                s_oled_brightness = BRIGHTNESS_CURVE[next_idx];
                                oled_display_set_brightness(s_oled_brightness);
                            } else if (s_tela_cursor == 1) {
                                if (s_timeout_idx < 5) s_timeout_idx++;
                            } else if (s_tela_cursor == 2) {
                                if (s_fallback_idx < (FALLBACK_OPTIONS_COUNT - 1)) s_fallback_idx++;
                            }
                        } else if (s_mode == UI_MODE_LED) {
                            rgb_led_config_t cfg;
                            rgb_led_get_config(&cfg);
                            uint8_t *val = (s_led_channel == 0) ? &cfg.r : (s_led_channel == 1 ? &cfg.g : &cfg.b);
                            if (*val <= 240) *val += 15; else *val = 255;
                            rgb_led_set_config(&cfg);
                        } else if (s_mode == UI_MODE_TOP_SCREEN) {
                            if (s_top_cursor == 0) {
                                audio_player_adjust_volume(VOLUME_STEP_PERCENT);
                            } else if (s_top_cursor == 1 && pressed_edge) {
                                if (s_top_eq_focus < 9) s_top_eq_focus++;
                                else s_top_eq_focus = 0;
                            }
                        } else if (s_mode == UI_MODE_VOLUME) {
                            audio_player_adjust_volume(VOLUME_STEP_PERCENT);
                        } else if (s_mode == UI_MODE_EQ_PRESETS && pressed_edge) {
                            player_eq_config_t cfg;
                            audio_player_get_eq_config(&cfg);
                            cfg.active_preset_idx = s_eq_preset_cursor;
                            audio_player_set_eq_config(&cfg);
                            s_mode = UI_MODE_EQ;
                            s_eq_band = 0;
                            ESP_LOGI(TAG, "Entrando na edicao de bandas do preset %d", s_eq_preset_cursor);
                        } else if (s_mode == UI_MODE_KEYBOARD && pressed_edge) {
                            if (s_kbd_confirming) {
                                if (s_kbd_callback) s_kbd_callback(s_kbd_text, true);
                            } else {
                                int max_x = (s_kbd_grid_y == 1) ? 8 : 9;
                                if (s_kbd_grid_x < max_x) s_kbd_grid_x++;
                            }
                        } else if (s_mode == UI_MODE_EQ) {
                            if (pressed_edge) {
                                s_eq_band++;
                                if (s_eq_band > 10) s_eq_band = 0;
                            }
                        } else if (s_mode == UI_MODE_PLAYING) {
                            oled_display_trigger_viz_overlay();
                            joy_seek_hold_pulse(+1, now, now - joy_press_ms[i], pressed_edge);
                        } else if (s_mode == UI_MODE_USB_DAC && pressed_edge) {
                            s_prev_mode_before_eq = UI_MODE_USB_DAC;
                            player_eq_config_t cfg;
                            audio_player_get_eq_config(&cfg);
                            s_eq_preset_cursor = cfg.active_preset_idx;
                            s_mode = UI_MODE_EQ_PRESETS;
                            ESP_LOGI(TAG, "Entrando no EQ a partir do USB DAC (JOY_RIGHT)");
                        } else if (s_mode == UI_MODE_LIST && pressed_edge) {
                            ESP_LOGI(TAG, "JOY_RIGHT pressionado");
                            joy_select();
                        }
                        break;
                }
            }
        }

        for (int i = 0; i < NUM_JOY_BTNS; i++) joy_prev[i] = joy_cur[i];

        // Sleep check
        static const int TIMEOUT_SECS[] = {0, 15, 30, 60, 120, 300};
        int t_idx = s_timeout_idx;
        if (t_idx < 0) t_idx = 0;
        if (t_idx > 5) t_idx = 5;
        int timeout = TIMEOUT_SECS[t_idx];
        // Se estiver em modo USB ou WiFi ativo, mantem o display ativo (reseta timer de inatividade)
        if (s_mode == UI_MODE_USB_MSC || s_mode == UI_MODE_USB_DAC || s_mode == UI_MODE_USB_PROMPT ||
            menu_any_active() || podcast_sync_is_busy()) {
            s_last_interaction_ms = now;
        }
        if (!s_is_sleeping && timeout > 0 && (now - s_last_interaction_ms) > (uint32_t)(timeout * 1000)) {
            s_is_sleeping = true;
        }
        vTaskDelay(pdMS_TO_TICKS(wifi_transfer_is_boost_active() ? 500 : POLL_INTERVAL_MS));
    }
}

// --- Registro dos itens "Player" e "Conf" no menu principal --------------
// Diferente de USB/WiFi (que sao modos de tela cheia com is_active/poll
// proprios), Player e Conf sao "internos" ao touch_input: Player so' troca
// a navegacao pro modo pasta, Conf ainda nao faz nada. Continuam vivendo
// aqui porque mexem direto em s_in_player_browser/s_list_cursor.

static void player_menu_on_select(void)
{
    ESP_LOGI(TAG, "Selecionado Player");
    s_in_player_browser = true;
    s_list_cursor = 0;
}

static void conf_menu_on_select(void) { s_mode = UI_MODE_CONF_MENU; s_conf_cursor = 0; }

int touch_input_get_games_cursor(void)
{
    return s_games_cursor;
}

// Menu "Games": selecionado no carrossel principal, abre a lista de jogos (UI_MODE_GAMES_MENU)
static void game_menu_on_select(void)
{
    s_mode = UI_MODE_GAMES_MENU;
    s_games_cursor = 0;
}

static bool game_menu_is_active(void) { return s_game_active; }
static bool game_menu_poll(void) { return false; }
static void game_menu_draw_status(void) { oled_display_show_locked(); }
static void game_menu_request_exit(void) { s_game_active = false; }

void touch_input_register_player_entry(void)
{
    static const menu_item_t item = { .name = "Player", .on_select = player_menu_on_select };
    menu_register(&item);
}

void touch_input_register_conf_entry(void)
{
    static const menu_item_t item = { .name = "Conf", .on_select = conf_menu_on_select };
    menu_register(&item);
}

void touch_input_register_usb_entry(void)
{
    static const menu_item_t item = { .name = "USB", .on_select = touch_input_set_usb_prompt };
    menu_register(&item);
}

void touch_input_register_game_entry(void)
{
    static const menu_item_t item = {
        .name = "Games",
        .on_select = game_menu_on_select,
        .is_active = game_menu_is_active,
        .poll = game_menu_poll,
        .draw_status = game_menu_draw_status,
        .request_exit = game_menu_request_exit,
    };
    menu_register(&item);
}

static void chrono_menu_on_select(void)
{
    s_mode = UI_MODE_CHRONO;
    s_is_sleeping = false;
    touch_input_wake_display();
}

void touch_input_register_chrono_entry(void)
{
    static const menu_item_t item = {
        .name = "Chrono",
        .on_select = chrono_menu_on_select,
    };
    menu_register(&item);
}

#include "nvs_flash.h"
#include "nvs.h"

esp_err_t touch_input_start(void)
{
    nvs_handle_t h;
    if (nvs_open("mps3_settings", NVS_READONLY, &h) == ESP_OK) {
        uint8_t val;
        if (nvs_get_u8(h, "brightness", &val) == ESP_OK) {
            s_oled_brightness = val;
            oled_display_set_brightness(s_oled_brightness);
        }
        if (nvs_get_u8(h, "timeout", &val) == ESP_OK) {
            s_timeout_idx = val;
        }
        if (nvs_get_u8(h, "fallback", &val) == ESP_OK) {
            if (val < FALLBACK_OPTIONS_COUNT) s_fallback_idx = val;
        }
        if (nvs_get_u8(h, "deepsleep", &val) == ESP_OK) {
            if (val < DEEPSLEEP_OPTIONS_COUNT) s_deepsleep_idx = val;
        }
        if (nvs_get_u8(h, "lock_clock", &val) == ESP_OK) {
            if (val < LOCK_CLOCK_OPTIONS_COUNT) s_lock_clock_timeout_idx = val;
        }
        nvs_close(h);
    }

    esp_err_t ret = configure_joystick();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao configurar GPIOs do joystick: %s", esp_err_to_name(ret));
        return ret;
    }

    // Se havia uma sessao anterior valida (NVS), comeca direto na tela de
    // reproducao em vez do menu.
    s_mode = audio_player_should_start_in_playing_mode() ? UI_MODE_PLAYING : UI_MODE_LIST;
    s_in_player_browser = (s_mode == UI_MODE_PLAYING); // se já está tocando, considera que veio do Player
    s_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    // Prioridade 4 no Core 0: cede passagem imediata para a audio_dsp_task (prioridade 5),
    // garantindo que eventos de UI não causem preempção sobre o envio de blocos DMA ao I2S.
    BaseType_t ok = xTaskCreatePinnedToCore(touch_task, "touch_task", 12288, NULL, 4, NULL, 0);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

void touch_input_set_usb_prompt(void) {
    if (wifi_transfer_is_active()) {
        ESP_LOGI(TAG, "Cabo USB detectado mas WiFi esta ativo - ignorando prompt USB para nao interromper transferencia");
        return;
    }
    if (s_mode != UI_MODE_USB_PROMPT && s_mode != UI_MODE_USB_DAC && s_mode != UI_MODE_USB_MSC) {
        s_mode = UI_MODE_USB_PROMPT;
        s_list_cursor = 0; // default pra pendrive
        s_is_sleeping = false;
        touch_input_wake_display();
    }
}

void touch_input_cancel_usb(void) {
    if (s_mode == UI_MODE_USB_PROMPT || s_mode == UI_MODE_USB_DAC || s_mode == UI_MODE_USB_MSC) {
        s_mode = UI_MODE_PLAYING;
        usb_manager_set_mode(USB_MODE_NONE);
    }
}

void touch_input_enter_usb_msc(void) {
    usb_manager_set_mode(USB_MODE_MSC);
    s_mode = UI_MODE_USB_MSC;
}

void touch_input_enter_usb_dac(void) {
    usb_manager_set_mode(USB_MODE_DAC);
    s_mode = UI_MODE_USB_DAC;
}

void touch_input_reset_to_main_menu(void) {
    nav_history_clear();
    while (!audio_player_browse_is_root()) {
        audio_player_select_entry(0);
    }
    s_mode = UI_MODE_LIST;
    s_in_player_browser = false;
    s_list_cursor = 0;
    s_is_sleeping = false;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    s_last_activity_ms = now;
    s_last_interaction_ms = now;
    touch_input_wake_display();
}

void touch_input_set_menu_cursor(int c) {
    nav_history_clear();
    while (!audio_player_browse_is_root()) {
        audio_player_select_entry(0);
    }
    s_mode = UI_MODE_LIST;
    s_in_player_browser = false;
    if (c >= 0 && c <= 6) {
        s_list_cursor = c;
    }
    s_is_sleeping = false;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    s_last_activity_ms = now;
    s_last_interaction_ms = now;
    touch_input_wake_display();
}

int touch_input_get_top_cursor(void) { return s_top_cursor; }
int touch_input_get_top_eq_focus(void) { return s_top_eq_focus; }

int touch_input_get_tela_cursor(void) { return s_tela_cursor; }
int touch_input_get_timeout_idx(void) { return s_timeout_idx; }
int touch_input_get_fallback_idx(void) { return s_fallback_idx; }
bool touch_input_is_sleeping(void) { return s_is_sleeping; }

int touch_input_get_deepsleep_idx(void)
{
    return s_deepsleep_idx;
}

uint32_t touch_input_get_deepsleep_ms(void)
{
    if (s_deepsleep_idx <= 0 || s_deepsleep_idx >= DEEPSLEEP_OPTIONS_COUNT) {
        return 0; // Desativado
    }
    return (uint32_t)DEEPSLEEP_MINS[s_deepsleep_idx] * 60UL * 1000UL;
}
