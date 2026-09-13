#ifndef TOUCH_INPUT_H
#define TOUCH_INPUT_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_MODE_LIST = 0,        // menu de selecao/navegacao de pastas
    UI_MODE_PLAYING = 1,     // tela de reproducao normal
    UI_MODE_EQ = 2,          // tela de bandas do equalizador
    UI_MODE_EQ_PRESETS = 12, // tela de lista dos 10 presets do EQ
    UI_MODE_KEYBOARD = 13,   // tela de teclado virtual
    UI_MODE_LED = 8,
    UI_MODE_TELA = 11,
    UI_MODE_TOP_SCREEN = 10,
    UI_MODE_USB_PROMPT = 3,
    UI_MODE_USB_MSC = 4,
    UI_MODE_USB_DAC = 5,
    UI_MODE_CONF_MENU = 6,
    UI_MODE_VOLUME = 7,
    UI_MODE_BALANCE = 14,
} ui_mode_t;

// Configura os 5 GPIOs do joystick de navegação (UP/DOWN/LEFT/RIGHT/CENTER)
// como entradas ativas em nível baixo (com pull-up) e cria a task de leitura
// de eventos e repetição. Se houver uma sessão anterior válida na NVS
// (audio_player_should_start_in_playing_mode()), inicia direto em UI_MODE_PLAYING.
esp_err_t touch_input_start(void);

void touch_input_set_usb_prompt(void);
void touch_input_cancel_usb(void);

// Timestamp (ms, base xTaskGetTickCount) da última ação detectada no joystick.
// Usado para temporização de repouso (sleep/dim) da tela OLED.
uint32_t touch_input_get_last_activity_ms(void);

// Modo de UI atual e posicao do cursor no menu (relevante durante UI_MODE_LIST).
ui_mode_t touch_input_get_mode(void);
int touch_input_get_list_cursor(void);

// true enquanto os controles estiverem bloqueados. Segurar o botão central
// por 700 ms alterna o bloqueio/desbloqueio do teclado.
bool touch_input_is_locked(void);

// Mantido por compatibilidade com a interface gráfica.
bool touch_input_is_powered_off(void);

bool touch_input_is_in_player_browser(void);

// Disparado pelo usb_manager ao conectar o cabo
void touch_input_set_usb_prompt(void);
void touch_input_cancel_usb(void);

// Registram os itens "Player" e "Conf" no menu principal (ver menu.h).
// Chame uma vez no boot, nas posicoes em que devem aparecer no carrossel
// (ver a secao "Registro do menu principal" em main.c) - antes ou depois
// de touch_input_start() tanto faz, so' precisam acontecer antes do
// primeiro desenho do menu.
void touch_input_register_player_entry(void);
void touch_input_register_conf_entry(void);
void touch_input_register_usb_entry(void);
void touch_input_register_game_entry(void);

// Retorna o indice da banda selecionada na tela do EQ (0-5, onde 5 e' o
// ganho geral). So' relevante quando touch_input_get_mode() == UI_MODE_EQ.
int touch_input_get_eq_band(void);
int touch_input_get_led_channel(void);
uint8_t touch_input_get_oled_brightness(void);
int touch_input_get_top_cursor(void);
int touch_input_get_top_eq_focus(void);
int touch_input_get_tela_cursor(void);
int touch_input_get_timeout_idx(void);
bool touch_input_is_sleeping(void);
int touch_input_get_eq_preset_cursor(void);

typedef void (*keyboard_callback_t)(const char *text, bool confirmed);
void keyboard_start(const char *initial_text, keyboard_callback_t cb);
void touch_input_get_keyboard_state(char *text, int *cursor, int *grid_x, int *grid_y, int *page, bool *is_confirming);

#ifdef __cplusplus
}
#endif

#endif // TOUCH_INPUT_H

