#include "touch_input.h"
#include "pinos.h"
#include "audio_player.h"
#include "menu.h"
#include "oled_display.h"
#include "rgb_led.h"
#include "usb_manager.h"
#include "i2s_output.h"

#include <stdbool.h>
#include <string.h>

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
static bool s_is_sleeping = false;
static int s_led_channel = 0;
static uint8_t s_oled_brightness = 255;
static const uint8_t BRIGHTNESS_CURVE[] = {1, 3, 7, 15, 30, 50, 80, 120, 180, 255};
int touch_input_get_led_channel(void) { return s_led_channel; }
uint8_t touch_input_get_oled_brightness(void) { return s_oled_brightness; } // banda atualmente selecionada
// Passo de ajuste por pressÃ£o de JOY_UP/DOWN no modo EQ (em dB)
#define EQ_STEP_DB 1.0f

int touch_input_get_eq_band(void) { return s_eq_band; }

static bool s_in_player_browser = false; // true = navegando pastas a partir do Player
bool touch_input_is_in_player_browser(void) { return s_in_player_browser; }

#define VOLUME_STEP_PERCENT  5

// Clique longo do botao central alterna o bloqueio. E' bem distinto de um
// clique curto de play/pause, sem deixar o bloqueio lento de usar.
#define CENTER_LOCK_HOLD_MS 700

#define POLL_INTERVAL_MS 25

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
static volatile bool s_locked = false;
static volatile bool s_game_active = false;

uint32_t touch_input_get_last_activity_ms(void) { return s_last_activity_ms; }
ui_mode_t touch_input_get_mode(void)            { return s_mode; }
int touch_input_get_list_cursor(void)           { return s_list_cursor; }
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

    if (is_first_pulse) {
        bool same_dir_recent = (s_seek_last_dir == dir) && (s_seek_last_press_ms != 0) &&
                                (now - s_seek_last_press_ms) <= DOUBLE_TAP_MS;
        s_seek_last_dir = dir;
        s_seek_last_press_ms = now;
        if (same_dir_recent) {
            // 2 apertos rapidos na mesma direcao = pular faixa, nao segurar-avancar.
            if (dir > 0) {
                ESP_LOGI(TAG, "Duplo toque direita - proxima faixa");
                audio_player_next();
            } else {
                ESP_LOGI(TAG, "Duplo toque esquerda - faixa anterior");
                audio_player_previous();
            }
            reset_seek_progression();
            return; // nao inicia avanco/retrocesso nesse toque
        }
    }

    uint32_t amount = seek_hold_amount_sec(held_for_ms);
    if (dir > 0) {
        ESP_LOGI(TAG, "Avancando %u s (segurado ha' %u ms)", (unsigned)amount, (unsigned)held_for_ms);
        audio_player_seek_forward(amount);
    } else {
        ESP_LOGI(TAG, "Retrocedendo %u s (segurado ha' %u ms)", (unsigned)amount, (unsigned)held_for_ms);
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

    // Log para diagnÃ³stico
    char entry_name[64];
    audio_player_get_browse_entry_name(real_index, entry_name, sizeof(entry_name));
    bool is_dir = audio_player_browse_entry_is_dir(real_index);
    ESP_LOGI(TAG, "select: idx=%d, name='%s', is_dir=%d", real_index, entry_name, is_dir);

    audio_player_select_entry(real_index);
    s_in_player_browser = true;

    if (is_dir) {
        s_list_cursor = 0;
    } else {
        s_mode = UI_MODE_PLAYING;
    }
}

static void joy_back(void)
{
    if (s_mode == UI_MODE_PLAYING) {
        s_mode = UI_MODE_LIST;
        ESP_LOGI(TAG, "Voltando para a lista");
        return;
    }
    if (s_mode == UI_MODE_LIST) {
        bool at_root = audio_player_browse_is_root();

        if (!at_root) {
            // Sempre sobe uma pasta, independente de s_in_player_browser
            audio_player_select_entry(0);
            s_list_cursor = 0;
            // Se nÃ£o estava no Player Browser, agora passa a estar
            s_in_player_browser = true;
            ESP_LOGI(TAG, "Voltando uma pasta");
        } else {
            // JÃ¡ na raiz
            if (s_in_player_browser) {
                s_in_player_browser = false;
                s_list_cursor = 0;
                ESP_LOGI(TAG, "Voltando ao menu principal");
            }
            // Se jÃ¡ estÃ¡ no menu principal (in_player==false), JOY_LEFT nÃ£o faz nada
        }
    }
}

static void touch_task(void *arg)
{
    (void)arg;

    bool joy_prev[NUM_JOY_BTNS] = {false};
    uint32_t joy_press_ms[NUM_JOY_BTNS] = {0};
    static uint32_t last_interaction_ms = 0;
    uint32_t joy_last_repeat_ms[NUM_JOY_BTNS] = {0};

    // JOY_CENTER: curto = play/pause; longo = bloqueia/desbloqueia.
    uint32_t center_press_start = 0;
    bool center_long_fired = false;
    bool center_modifier_used = false;

    // Segurar JOY_LEFT com um modo de tela cheia ativo (USB/WiFi) = sair.
    uint32_t exit_active_hold_start = 0;
    bool exit_active_fired = false;

    while (true) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if (last_interaction_ms == 0) last_interaction_ms = now;

        bool joy_cur[NUM_JOY_BTNS];
        for (int i = 0; i < NUM_JOY_BTNS; i++) {
            joy_cur[i] = gpio_get_level(s_joy_pins[i]) == 0; // ativo em nivel baixo
        }

        bool any_joy = false;
        for (int i = 0; i < NUM_JOY_BTNS; i++) {
            if (joy_cur[i] && !joy_prev[i]) any_joy = true;
        }
        if (any_joy) {
            s_last_activity_ms = now;
            last_interaction_ms = now;
            if (s_is_sleeping) {
                s_is_sleeping = false;
                for (int i = 0; i < NUM_JOY_BTNS; i++) joy_prev[i] = joy_cur[i];
                vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
                continue;
            }
        }

        // --- Botao central: curto = play/pause; longo = bloqueio -----------
        // Fica antes do retorno de s_locked: assim, um toque longo no mesmo
        // botao sempre funciona para desbloquear.
        bool center_pressed_edge = joy_cur[JOY_CENTER] && !joy_prev[JOY_CENTER];
        bool center_released_edge = !joy_cur[JOY_CENTER] && joy_prev[JOY_CENTER];
        if (center_pressed_edge) {
            center_press_start = now;
            center_long_fired = false;
        }
        if (joy_cur[JOY_CENTER] && !center_long_fired && center_press_start != 0 &&
            (now - center_press_start) >= CENTER_LOCK_HOLD_MS) {
            center_long_fired = true;
            if (s_mode == UI_MODE_LED) {
                s_mode = UI_MODE_CONF_MENU;
                rgb_led_save_nvs();
                ESP_LOGI(TAG, "Saindo da configuracao de LED");
            } else if (s_mode == UI_MODE_TELA) {
                s_mode = UI_MODE_CONF_MENU;
                nvs_handle_t h;
                if (nvs_open("mps3_settings", NVS_READWRITE, &h) == ESP_OK) {
                    nvs_set_u8(h, "brightness", s_oled_brightness);
                    nvs_set_u8(h, "timeout", s_timeout_idx);
                    nvs_commit(h);
                    nvs_close(h);
                }
                ESP_LOGI(TAG, "Saindo da tela TELA via pressao longa");
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
            } else {
                s_locked = !s_locked;
                reset_seek_progression();
                ESP_LOGI(TAG, "Controles %s pelo botao central", s_locked ? "bloqueados" : "desbloqueados");
            }
        }
        if (center_released_edge) {
            // Com a tela bloqueada, clique curto nao tem efeito.
            if (!center_long_fired && !s_locked) {
                static uint32_t s_last_short_center_ms = 0;
                bool is_double_click = (now - s_last_short_center_ms) < 350;
                s_last_short_center_ms = now;

                if (center_modifier_used) {
                    // Botao central foi usado como modifier (ex: CENTER + DOWN)
                    // Nao fazemos a acao de clique curto
                } else if (menu_any_active()) {
                    menu_request_action_active();
                } else if (s_mode == UI_MODE_VOLUME || s_mode == UI_MODE_BALANCE) {
                    s_mode = UI_MODE_CONF_MENU;
                    ESP_LOGI(TAG, "Saindo da tela de volume/balanco (botao central)");
                } else if (s_mode == UI_MODE_TELA) {
                    s_mode = UI_MODE_CONF_MENU;
                    nvs_handle_t h;
                    if (nvs_open("mps3_settings", NVS_READWRITE, &h) == ESP_OK) {
                        nvs_set_u8(h, "brightness", s_oled_brightness);
                        nvs_set_u8(h, "timeout", s_timeout_idx);
                        nvs_commit(h);
                        nvs_close(h);
                    }
                    ESP_LOGI(TAG, "Saindo da tela TELA (botao central)");
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
                    if (s_list_cursor == 0) {
                        usb_manager_set_mode(USB_MODE_MSC);
                        s_mode = UI_MODE_USB_MSC;
                    } else if (s_list_cursor == 1) {
                        usb_manager_set_mode(USB_MODE_DAC);
                        s_mode = UI_MODE_USB_DAC;
                    } else if (s_list_cursor == 2) {
                        oled_display_show_message("Entrando em modo", "FLASH / GRAVACAO");
                        vTaskDelay(pdMS_TO_TICKS(400));
                        usb_manager_enter_bootloader();
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
                    audio_player_toggle_play_pause();
                    reset_seek_progression();
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

        bool is_active_mode = menu_any_active() || (s_mode == UI_MODE_USB_MSC) || (s_mode == UI_MODE_USB_DAC);
        if (is_active_mode) {
            if (menu_any_active()) {
                if (joy_cur[JOY_UP] && !joy_prev[JOY_UP]) {
                    menu_nav_up_active();
                } else if (joy_cur[JOY_DOWN] && !joy_prev[JOY_DOWN]) {
                    menu_nav_down_active();
                } else if (joy_cur[JOY_RIGHT] && !joy_prev[JOY_RIGHT]) {
                    menu_nav_select_active();
                }
            }

            if (joy_cur[JOY_LEFT]) {
                if (exit_active_hold_start == 0) exit_active_hold_start = now;
                if (!exit_active_fired && (now - exit_active_hold_start) >= EXIT_ACTIVE_MODE_HOLD_MS) {
                    exit_active_fired = true;
                    ESP_LOGI(TAG, "JOY_LEFT segurado - saindo do modo ativo");
                    if (menu_any_active()) {
                        menu_request_exit_active();
                    } else {
                        // Sair do USB
                        s_mode = UI_MODE_PLAYING;
                        usb_manager_set_mode(USB_MODE_NONE);
                    }
                }
            } else {
                exit_active_hold_start = 0;
                exit_active_fired = false;
            }

            // No modo DAC, ainda permitimos botoes de borda para HID
            if (s_mode != UI_MODE_USB_DAC) {
                vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
                for (int i = 0; i < NUM_JOY_BTNS; i++) joy_prev[i] = joy_cur[i];
                continue;
            }
        }

        
        // Auto-idle to PLAYING
        if (s_mode == UI_MODE_LIST && !menu_any_active() && (now - s_last_activity_ms > 5000)) {
            playback_state_t st;
            audio_player_get_state(&st);
            if (st.playing || st.track_loaded) {
                s_mode = UI_MODE_PLAYING;
                ESP_LOGI(TAG, "Inatividade: entrando na tela de reproducao");
            }
        }
        
        // --- Joystick: navegacao / selecao / voltar / avanco-retrocesso ----
        // JOY_CENTER fica fora: ele foi tratado acima.
        for (int i = 0; i < NUM_JOY_BTNS; i++) {
            if (i == JOY_CENTER) continue;

            bool pressed_edge = joy_cur[i] && !joy_prev[i];
            bool is_updown_repeatable = (i == JOY_UP || i == JOY_DOWN);
            bool is_seek_repeatable = (i == JOY_LEFT || i == JOY_RIGHT) && s_mode == UI_MODE_PLAYING;
            bool is_lr_repeatable = (i == JOY_LEFT || i == JOY_RIGHT) && (s_mode == UI_MODE_VOLUME || s_mode == UI_MODE_BALANCE || s_mode == UI_MODE_EQ || s_mode == UI_MODE_LED || (s_mode == UI_MODE_TOP_SCREEN && s_top_cursor == 0));

            bool fire = pressed_edge;
            if (!fire && joy_cur[i]) {
                if (is_updown_repeatable || is_lr_repeatable) {
                    uint32_t held_for = now - joy_press_ms[i];
                    if (held_for >= JOY_REPEAT_INITIAL_MS &&
                        (now - joy_last_repeat_ms[i]) >= JOY_REPEAT_MS) {
                        fire = true;
                    }
                } else if (is_seek_repeatable) {
                    // Sem atraso inicial aqui (diferente do cima/baixo) -
                    // o 1o pulso ja' sai na borda do aperto, os seguintes
                    // continuam nesse ritmo desde logo, acelerando por
                    // conta do calculo em seek_hold_amount_sec().
                    if ((now - joy_last_repeat_ms[i]) >= SEEK_HOLD_PULSE_MS) {
                        fire = true;
                    }
                }
            }

            if (pressed_edge) {
                joy_press_ms[i] = now;
            }
            if (fire) {
                joy_last_repeat_ms[i] = now;
                last_interaction_ms = now;
                switch (i) {
                                        case JOY_UP:
                        if (s_mode == UI_MODE_USB_PROMPT && pressed_edge) {
                            s_list_cursor = (s_list_cursor == 0) ? 2 : (s_list_cursor - 1);
                        } else if (s_mode == UI_MODE_CONF_MENU && pressed_edge) {
                            s_list_cursor = (s_list_cursor == 0) ? 4 : (s_list_cursor - 1);
                        } else if (s_mode == UI_MODE_BALANCE && pressed_edge) {
                            s_mode = UI_MODE_CONF_MENU;
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
                            s_tela_cursor = (s_tela_cursor == 0) ? 1 : 0;
                        } else if (s_mode == UI_MODE_VOLUME) {
                            audio_player_adjust_volume(VOLUME_STEP_PERCENT);
                        } else if (s_mode == UI_MODE_EQ_PRESETS && pressed_edge) {
                            if (s_eq_preset_cursor > 0) s_eq_preset_cursor--;
                            else s_eq_preset_cursor = 9;
                        } else if (s_mode == UI_MODE_KEYBOARD && pressed_edge) {
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
                        } else if (s_mode == UI_MODE_LIST) {
                            joy_move_cursor(-1, pressed_edge);
                        }
                        break;
                    case JOY_DOWN:
                        if (s_mode == UI_MODE_USB_PROMPT && pressed_edge) {
                            s_list_cursor = (s_list_cursor + 1) % 3;
                        } else if (s_mode == UI_MODE_CONF_MENU && pressed_edge) {
                            s_list_cursor = (s_list_cursor + 1) % 5;
                        } else if (s_mode == UI_MODE_BALANCE && pressed_edge) {
                            s_mode = UI_MODE_CONF_MENU;
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
                            s_tela_cursor = (s_tela_cursor == 0) ? 1 : 0;
                        } else if (s_mode == UI_MODE_VOLUME) {
                            audio_player_adjust_volume(-VOLUME_STEP_PERCENT);
                        } else if (s_mode == UI_MODE_EQ_PRESETS && pressed_edge) {
                            if (s_eq_preset_cursor < 9) s_eq_preset_cursor++;
                            else s_eq_preset_cursor = 0;
                        } else if (s_mode == UI_MODE_KEYBOARD && pressed_edge) {
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
                                ESP_LOGI(TAG, "Voltando para a lista (JOY_DOWN)");
                                s_mode = UI_MODE_LIST;
                            }
                        } else if (s_mode == UI_MODE_USB_DAC) {
                            audio_player_adjust_volume(-VOLUME_STEP_PERCENT);
                        } else if (s_mode == UI_MODE_LIST) {
                            joy_move_cursor(+1, pressed_edge);
                        }
                        break;
                    case JOY_LEFT:
                        if (s_mode == UI_MODE_USB_PROMPT && pressed_edge) {
                            touch_input_cancel_usb();
                        } else if (s_mode == UI_MODE_CONF_MENU && pressed_edge) {
                            s_mode = UI_MODE_LIST;
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
                            } else {
                                if (s_timeout_idx > 0) s_timeout_idx--;
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
                            joy_seek_hold_pulse(-1, now, now - joy_press_ms[i], pressed_edge);
                        } else if (s_mode == UI_MODE_USB_DAC && pressed_edge) {
                            usb_manager_send_hid(2); // Prev
                        } else if (s_mode == UI_MODE_LIST && pressed_edge) {
                            ESP_LOGI(TAG, "JOY_LEFT pressionado");
                            joy_back();
                        }
                        break;
                    case JOY_RIGHT:
                        if (s_mode == UI_MODE_CONF_MENU && pressed_edge) {
                            if (s_list_cursor == 0) {
                                s_mode = UI_MODE_VOLUME;
                            } else if (s_list_cursor == 1) {
                                s_mode = UI_MODE_BALANCE;
                            } else if (s_list_cursor == 2) {
                                s_prev_mode_before_eq = UI_MODE_CONF_MENU;
                                s_mode = UI_MODE_EQ_PRESETS;
                                s_eq_preset_cursor = 0;
                            } else if (s_list_cursor == 3) {
                                s_mode = UI_MODE_LED;
                                s_led_channel = 0;
                            } else if (s_list_cursor == 4) {
                                s_mode = UI_MODE_TELA;
                                s_tela_cursor = 0;
                            }
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
                            } else {
                                if (s_timeout_idx < 5) s_timeout_idx++;
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
            menu_any_active()) {
            last_interaction_ms = now;
        }
        if (!s_is_sleeping && timeout > 0 && (now - last_interaction_ms) > (uint32_t)(timeout * 1000)) {
            s_is_sleeping = true;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
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

static void conf_menu_on_select(void) { s_mode = UI_MODE_CONF_MENU; s_list_cursor = 0; }

// O Game of Life deixa de ser a tela de bloqueio e passa a ser uma atividade
// independente do menu. LEFT segurado por 1s usa o fluxo generico de saida
// de modos ativos e volta ao menu principal.
static void game_menu_on_select(void)
{
    oled_display_reset_life();
    s_game_active = true;
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
        .name = "Game",
        .on_select = game_menu_on_select,
        .is_active = game_menu_is_active,
        .poll = game_menu_poll,
        .draw_status = game_menu_draw_status,
        .request_exit = game_menu_request_exit,
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
    s_in_player_browser = (s_mode == UI_MODE_PLAYING); // se jÃ¡ estÃ¡ tocando, considera que veio do Player

    BaseType_t ok = xTaskCreatePinnedToCore(touch_task, "touch_task", 12288, NULL, 4, NULL, 0);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

void touch_input_set_usb_prompt(void) {
    if (s_mode != UI_MODE_USB_PROMPT && s_mode != UI_MODE_USB_DAC && s_mode != UI_MODE_USB_MSC) {
        // Libera imediatamente todos os arquivos abertos no SD e coloca player_task em espera passiva
        audio_player_release_sd_for_usb();
        i2s_output_disable();
        s_mode = UI_MODE_USB_PROMPT;
        s_list_cursor = 0; // default pra pendrive
    }
}

void touch_input_cancel_usb(void) {
    if (s_mode == UI_MODE_USB_PROMPT || s_mode == UI_MODE_USB_DAC || s_mode == UI_MODE_USB_MSC) {
        s_mode = UI_MODE_PLAYING;
        usb_manager_set_mode(USB_MODE_NONE);
    }
}

int touch_input_get_top_cursor(void) { return s_top_cursor; }
int touch_input_get_top_eq_focus(void) { return s_top_eq_focus; }

int touch_input_get_tela_cursor(void) { return s_tela_cursor; }
int touch_input_get_timeout_idx(void) { return s_timeout_idx; }
bool touch_input_is_sleeping(void) { return s_is_sleeping; }
