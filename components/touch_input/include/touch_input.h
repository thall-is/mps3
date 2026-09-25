#ifndef TOUCH_INPUT_H
#define TOUCH_INPUT_H

/**
 * @file touch_input.h
 * @brief Gerenciador de Entradas do Usuário (Joystick de 5 Vias e Navegação da UI)
 *
 * Configura os GPIOs de navegação (UP GPIO 2, DOWN GPIO 41, LEFT GPIO 39, RIGHT GPIO 42,
 * CENTER GPIO 40) com resistores internos de pull-up, gerencia detecção de cliques
 * rápidos, repetição contínua (*auto-repeat*), toque longo (*long press* para bloqueio),
 * atalhos da tela superior e estados da interface gráfica.
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Registra o handle da tarefa de renderização do display (`display_task`).
 *
 * Permite que a tarefa de entrada notifique diretamente a tarefa gráfica
 * quando ocorrer qualquer evento de toque para despertar imediato do repouso.
 *
 * @param[in] handle Handle da tarefa FreeRTOS do display.
 */
void touch_input_set_display_task_handle(TaskHandle_t handle);

/**
 * @brief Dispara notificação FreeRTOS para acordar a `display_task` imediatamente.
 */
void touch_input_wake_display(void);

/**
 * @brief Modos de tela e estados da interface gráfica de usuário (UI).
 */
typedef enum {
    UI_MODE_LIST        = 0,  /**< Navegação na lista de pastas e músicas. */
    UI_MODE_PLAYING     = 1,  /**< Tela principal de reprodução (Now Playing). */
    UI_MODE_EQ          = 2,  /**< Ajuste fino das 10 bandas do equalizador. */
    UI_MODE_USB_PROMPT  = 3,  /**< Menu estilo Android de seleção ao plugar cabo USB. */
    UI_MODE_USB_MSC     = 4,  /**< Cartão montado como drive USB Mass Storage. */
    UI_MODE_USB_DAC     = 5,  /**< Placa de som USB UAC2 24-bit operando no computador. */
    UI_MODE_CONF_MENU   = 6,  /**< Menu de configurações gerais do sistema. */
    UI_MODE_VOLUME      = 7,  /**< Popup temporário de ajuste de volume. */
    UI_MODE_LED         = 8,  /**< Menu de controle de cores do LED RGB WS2812. */
    UI_MODE_TOP_SCREEN  = 10, /**< Tela superior de atalhos rápidos (Bateria/EQ/Volume). */
    UI_MODE_TELA        = 11, /**< Menu de configuração de brilho e repouso da tela. */
    UI_MODE_EQ_PRESETS  = 12, /**< Carrossel de seleção rápida de presets de EQ. */
    UI_MODE_KEYBOARD    = 13, /**< Teclado virtual em tela para digitação de senhas. */
    UI_MODE_BALANCE     = 14, /**< Popup temporário de ajuste de balanço estéreo L/R. */
    UI_MODE_SORT        = 15, /**< Menu de seleção de ordenação (Nome vs Data). */
    UI_MODE_WIFI_NETS   = 16, /**< Visualizador de redes Wi-Fi e QR Code. */
    UI_MODE_DEEP_SLEEP  = 17, /**< Menu de configuração de suspensão profunda (Deep Sleep). */
} ui_mode_t;

/**
 * @brief Inicializa os pinos de GPIO do joystick e inicia a tarefa de amostragem (`touch_task`).
 *
 * Configura os resistores de pull-up internos e cria a tarefa no Core 0 com prioridade 5.
 * Se houver sessão anterior válida na NVS, inicia diretamente no modo `UI_MODE_PLAYING`.
 *
 * @return ESP_OK se a tarefa foi criada com sucesso.
 */
esp_err_t touch_input_start(void);

/**
 * @brief Exibe o diálogo de seleção de modo USB (Flash CDC, DAC, Armazenamento ou Cancelar).
 *
 * Invocada pelo gerenciador USB (`usb_manager`) ao detectar inserção de cabo VBUS.
 */
void touch_input_set_usb_prompt(void);

/**
 * @brief Cancela o prompt USB e retorna o usuário à tela de reprodução.
 */
void touch_input_cancel_usb(void);

/**
 * @brief Retorna o timestamp em milissegundos da última ação detectada no joystick.
 *
 * Utilizado pelos algoritmos de inatividade para temporização de suspensão da tela.
 *
 * @return Milissegundos decorridos desde o boot baseados em `xTaskGetTickCount()`.
 */
uint32_t touch_input_get_last_activity_ms(void);

/**
 * @brief Retorna o modo de interface gráfica atualmente ativo.
 *
 * @return Enum `ui_mode_t`.
 */
ui_mode_t touch_input_get_mode(void);

/**
 * @brief Retorna a posição do cursor na lista de navegação de arquivos.
 *
 * @return Índice selecionado.
 */
int touch_input_get_list_cursor(void);

/**
 * @brief Informa se as entradas do joystick e a tela estão bloqueadas.
 *
 * Pressionar e segurar o botão central por mais de 700 ms alterna o bloqueio.
 *
 * @return true se o teclado físico estiver bloqueado; false caso contrário.
 */
bool touch_input_is_locked(void);

/**
 * @brief Informa se o sistema recebeu comando de desligamento via interface.
 *
 * @return true se o sistema estiver em desligamento; false caso contrário.
 */
bool touch_input_is_powered_off(void);

/**
 * @brief Informa se o usuário está navegando na lista de arquivos do player de áudio.
 *
 * @return true se o modo ativo for `UI_MODE_LIST`.
 */
bool touch_input_is_in_player_browser(void);

/**
 * @brief Registra a entrada "Player" no menu carrossel principal.
 */
void touch_input_register_player_entry(void);

/**
 * @brief Registra a entrada "Conf" (Configurações) no menu carrossel principal.
 */
void touch_input_register_conf_entry(void);

/**
 * @brief Registra a entrada "USB" no menu carrossel principal.
 */
void touch_input_register_usb_entry(void);

/**
 * @brief Registra o jogo "Game of Life" no menu carrossel principal.
 */
void touch_input_register_game_entry(void);

/**
 * @brief Retorna o índice da banda do equalizador selecionada no momento.
 *
 * @return Índice de 0 a 9 (bandas de frequência) ou 10 (ganho geral).
 */
int touch_input_get_eq_band(void);

/**
 * @brief Retorna o canal RGB ativo no menu de configuração do LED (0=R, 1=G, 2=B).
 *
 * @return Canal selecionado.
 */
int touch_input_get_led_channel(void);

/**
 * @brief Retorna o nível de brilho do display OLED selecionado no menu de tela.
 *
 * @return Nível de contraste (0 a 255).
 */
uint8_t touch_input_get_oled_brightness(void);

/**
 * @brief Retorna a posição do cursor vertical na tela superior (Top Screen).
 *
 * @return 0 = Volume (base), 1 = Equalizador (meio), 2 = Bateria (topo).
 */
int touch_input_get_top_cursor(void);

/**
 * @brief Retorna o preset de equalizador em foco na tela superior.
 *
 * @return Índice do preset.
 */
int touch_input_get_top_eq_focus(void);

/**
 * @brief Retorna a opção selecionada no menu de ajustes de tela.
 *
 * @return Índice do parâmetro em foco.
 */
int touch_input_get_tela_cursor(void);

/**
 * @brief Retorna o índice do tempo de desligamento automático configurado.
 *
 * @return Índice de timeout (0=Nunca, 1=15s, 2=30s, 3=1m, 4=2m).
 */
int touch_input_get_timeout_idx(void);

/**
 * @brief Informa se o sistema encontra-se em modo de suspensão de tela (Display Sleep).
 *
 * @return true se a tela estiver apagada por economia de energia; false se ativa.
 */
bool touch_input_is_sleeping(void);

/**
 * @brief Retorna a posição do cursor no menu de seleção de presets do equalizador.
 *
 * @return Índice selecionado.
 */
int touch_input_get_eq_preset_cursor(void);

/**
 * @brief Retorna a posição do cursor na lista de redes Wi-Fi salvas.
 *
 * @return Índice da rede selecionada.
 */
int touch_input_get_wifi_net_cursor(void);

/**
 * @brief Retorna o índice selecionado no menu de configuração de Deep Sleep.
 *
 * @return Índice da opção (Desligado, 1 min, 5 min, 15 min, atalho [>]).
 */
int touch_input_get_deepsleep_idx(void);

/**
 * @brief Converte o índice configurado para o tempo de inatividade em milissegundos.
 *
 * @return Tempo limite em ms (0 se desativado).
 */
uint32_t touch_input_get_deepsleep_ms(void);

/**
 * @brief Callback de conclusão da digitação no teclado virtual em tela.
 *
 * @param[in] text      Texto final digitado pelo usuário.
 * @param[in] confirmed true se o usuário clicou em OK/Confirmar; false se cancelou.
 */
typedef void (*keyboard_callback_t)(const char *text, bool confirmed);

/**
 * @brief Abre o teclado virtual alfanumérico em tela para entrada de texto.
 *
 * @param[in] initial_text Texto inicial ou string vazia.
 * @param[in] cb           Função de callback a ser chamada ao concluir ou cancelar.
 */
void keyboard_start(const char *initial_text, keyboard_callback_t cb);

/**
 * @brief Obtém o estado atual dos cursores e texto do teclado virtual.
 *
 * @param[out] text          Buffer que recebe a string em edição.
 * @param[out] cursor        Posição atual do cursor no texto.
 * @param[out] grid_x        Posição horizontal na grade de caracteres.
 * @param[out] grid_y        Posição vertical na grade de caracteres.
 * @param[out] page          Página de símbolos ativa.
 * @param[out] is_confirming true se o botão OK estiver selecionado.
 */
void touch_input_get_keyboard_state(char *text, int *cursor, int *grid_x, int *grid_y, int *page, bool *is_confirming);

#ifdef __cplusplus
}
#endif

#endif // TOUCH_INPUT_H
