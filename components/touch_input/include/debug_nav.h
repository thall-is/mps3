#ifndef DEBUG_NAV_H
#define DEBUG_NAV_H

/**
 * @file debug_nav.h
 * @brief Camada de Injeção de Botões Virtuais e Telemetria de UI para Debug
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Botões correspondentes a touch_input (JOY_UP=0, JOY_LEFT=1, JOY_DOWN=2, JOY_RIGHT=3, JOY_CENTER=4)
#define DEBUG_BTN_UP     (1u << 0)
#define DEBUG_BTN_LEFT   (1u << 1)
#define DEBUG_BTN_DOWN   (1u << 2)
#define DEBUG_BTN_RIGHT  (1u << 3)
#define DEBUG_BTN_CENTER (1u << 4)

/**
 * @brief Inicializa o módulo de debug navigation.
 */
void debug_nav_init(void);

/**
 * @brief Liga ou desliga o modo de debug em runtime.
 * Salva preferência na NVS ("mps3_settings" -> "dbg_mode").
 */
void debug_nav_set_enabled(bool enable);

/**
 * @brief Retorna se o modo de debug está ativo.
 */
bool debug_nav_is_enabled(void);

/**
 * @brief Injeta o pressionamento de um botão com duração específica em milissegundos.
 */
esp_err_t debug_nav_press(uint8_t btn_mask, uint32_t duration_ms);

/**
 * @brief Enfileira uma sequência de comandos (ex: "U,U,R,C:800").
 */
esp_err_t debug_nav_enqueue_seq(const char *seq_str);

/**
 * @brief Libera qualquer botão virtual ativo e esvazia a fila.
 */
void debug_nav_release_all(void);

/**
 * @brief Chamado pela touch_task para obter a máscara de botões virtuais no instante atual.
 */
uint8_t debug_nav_get_vmask(uint32_t now_ms);

/**
 * @brief Chamado quando há mudança de estado/modo de UI para notificar (@EVT).
 */
void debug_nav_notify_mode_change(int old_mode, int new_mode);

/**
 * @brief Formata e imprime o snapshot completo de estado no stdout (@UI {...}).
 */
void debug_nav_dump_state(void);

/**
 * @brief Solicita captura do framebuffer OLED (1024 bytes) e imprime @SCR <base64>.
 */
esp_err_t debug_nav_request_screenshot(void);

/**
 * @brief Executa reset de navegação: solta botões, acorda tela, desbloqueia e retorna ao menu.
 */
void debug_nav_reset_home(void);

#ifdef __cplusplus
}
#endif

#endif // DEBUG_NAV_H
