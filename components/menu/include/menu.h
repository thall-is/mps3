#ifndef MENU_H
#define MENU_H

/**
 * @file menu.h
 * @brief Registro Central e Despachante Polimórfico de Menus do Sistema
 *
 * Fornece um barramento desacoplado para registro dos itens navegáveis no
 * carrossel principal (Player, WiFi, Conf, USB, Game). Cada componente registra
 * seus callbacks de ciclo de vida (`on_select`, `is_active`, `draw_status`,
 * `request_exit`, etc.), permitindo que a `display_task` e a `touch_task` despachem
 * eventos genericamente.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Estrutura que define um item registrável no menu do sistema.
 */
typedef struct {
    const char *name; /**< Nome descritivo do item (para logs e auditoria). */

    /** Callback invocado quando o usuário confirma o item (joystick Direita). */
    void (*on_select)(void);

    // Callbacks de ciclo de vida para modos de tela cheia (USB, WiFi, etc.)
    bool (*is_active)(void);      /**< true enquanto o modo exclusivo estiver em execução. */
    bool (*poll)(void);           /**< Chamado a cada ciclo da display_task; retorna true quando o modo finaliza. */
    void (*draw_status)(void);    /**< Renderiza a tela gráfica personalizada do modo ativo. */
    void (*request_exit)(void);   /**< Solicita encerramento limpo do modo ativo (seguro entre tasks). */
    void (*request_action)(void); /**< Dispara ação contextual durante o modo (ex: clique central). */
    void (*nav_up)(void);         /**< Navegação contextual para cima. */
    void (*nav_down)(void);       /**< Navegação contextual para baixo. */
    void (*nav_select)(void);     /**< Seleção contextual dentro do modo ativo. */
} menu_item_t;

#define MENU_MAX_ITEMS 8 /**< Limite máximo de itens suportados no menu principal. */

/**
 * @brief Registra um item no menu principal na próxima posição livre do carrossel.
 *
 * A ordem de registro no boot define a sequência visual dos itens (0, 1, 2...).
 *
 * @param[in] item Ponteiro para a estrutura com os dados e callbacks do menu.
 */
void menu_register(const menu_item_t *item);

/**
 * @brief Retorna a quantidade de itens atualmente registrados no menu.
 *
 * @return Total de itens cadastrados.
 */
int menu_count(void);

/**
 * @brief Obtém o ponteiro para o item na posição especificada.
 *
 * @param[in] index Índice do item (0 a menu_count()-1).
 *
 * @return Ponteiro constante para `menu_item_t` ou NULL se índice for inválido.
 */
const menu_item_t *menu_get(int index);

/**
 * @brief Dispara o callback `on_select()` do item correspondente ao índice fornecido.
 *
 * @param[in] index Posição do item no carrossel.
 */
void menu_select(int index);

/**
 * @brief Verifica se algum dos itens registrados está operando em modo de tela cheia exclusivo.
 *
 * @return true se algum item possui `is_active() == true`; false caso contrário.
 */
bool menu_any_active(void);

/**
 * @brief Executa o ciclo periódico de polling do item que está ativo no momento.
 *
 * @return true no ciclo exato em que o modo terminou; false caso contrário.
 */
bool menu_poll_active(void);

/**
 * @brief Invoca a rotina de desenho de status do modo atualmente em foco.
 */
void menu_draw_active_status(void);

/**
 * @brief Solicita a saída e fechamento do modo ativo no momento.
 */
void menu_request_exit_active(void);

/**
 * @brief Dispara a ação de clique contextual do modo em execução.
 */
void menu_request_action_active(void);

/**
 * @brief Envia evento de navegação para cima ao modo ativo.
 */
void menu_nav_up_active(void);

/**
 * @brief Envia evento de navegação para baixo ao modo ativo.
 */
void menu_nav_down_active(void);

/**
 * @brief Envia evento de seleção/avanço ao modo ativo.
 */
void menu_nav_select_active(void);

#ifdef __cplusplus
}
#endif

#endif // MENU_H
