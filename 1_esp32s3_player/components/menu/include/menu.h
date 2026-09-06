#ifndef MENU_H
#define MENU_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Registro central do menu principal (Player, WiFi, Conf, USB, ...).
//
// Cada função do menu (existente ou nova) vive no seu próprio componente e
// só precisa preencher um menu_item_t e chamar menu_register() UMA VEZ, no
// boot (ver a seção "Registro do menu principal" em src/main.c). A partir
// daí, ninguém mais no firmware precisa saber que aquele item existe:
// touch_input.c despacha seleção/saída genericamente, e main.c desenha a
// tela de status ativa genericamente, ambos iterando sobre o que estiver
// registrado aqui.
//
// A ÚNICA parte que continua manual ao adicionar um item novo é o ícone do
// carrossel do menu principal (oled_display_show_main_menu) — é arte pixel
// por pixel (posição do ícone + rótulos vizinhos), não dá pra generalizar
// sem redesenhar os bitmaps. Tudo o mais (contagem de itens, despacho de
// seleção, ciclo de vida de modos de tela cheia como USB/WiFi) é
// automático.
// =============================================================================

typedef struct {
    const char *name; // só para logs/diagnóstico

    // Chamado quando o usuário confirma esse item no menu principal
    // (joystick direita). NULL se o item não faz nada ao ser selecionado.
    void (*on_select)(void);

    // --- Modos de "tela cheia" (USB, WiFi, e futuros modos parecidos) ------
    // Deixe os quatro abaixo como NULL se o item NÃO abre um modo de tela
    // cheia próprio (ex.: "Conf" ainda não faz nada; "Player" só troca de
    // tela dentro do próprio menu de lista, não é um modo separado).
    bool (*is_active)(void);    // true enquanto o modo estiver rodando
    bool (*poll)(void);         // chamado a cada ciclo do display_task
                                 // enquanto is_active(); retorna true no
                                 // ciclo exato em que o modo termina
    void (*draw_status)(void);  // desenha a tela desse modo (chamado no
                                 // lugar da lista/menu enquanto is_active())
    void (*request_exit)(void); // pedido de saída manual (toque/joystick
                                 // durante o modo) - seguro de chamar de
                                 // qualquer task
    void (*request_action)(void); // acao contextual enquanto ativo (ex: botao central)
    void (*nav_up)(void);         // navegacao pra cima no modo ativo
    void (*nav_down)(void);       // navegacao pra baixo no modo ativo
    void (*nav_select)(void);     // selecao (joystick direita) no modo ativo
} menu_item_t;

#define MENU_MAX_ITEMS 8

// Adiciona um item ao menu principal, na próxima posição livre. Chame
// durante o boot, antes de touch_input_start() (ver main.c) - a ORDEM das
// chamadas é a ordem visual do carrossel (índice 0, 1, 2...). "item" é
// copiado internamente, não precisa sobreviver depois da chamada.
void menu_register(const menu_item_t *item);

// Quantos itens estão registrados agora.
int menu_count(void);

// Item na posição "index", ou NULL se fora do intervalo [0, menu_count()).
const menu_item_t *menu_get(int index);

// Chama o on_select() do item em "index", se existir. Não faz nada se
// "index" estiver fora do intervalo ou o item não tiver on_select.
void menu_select(int index);

// true se algum item registrado estiver com is_active() == true agora.
bool menu_any_active(void);

// Faz poll() do item ativo no momento (se houver). Retorna true no exato
// ciclo em que ele terminou (deixou de estar ativo). Não faz nada (e
// retorna false) se nenhum item estiver ativo.
bool menu_poll_active(void);

// Desenha a tela do item ativo no momento (chama draw_status() dele, se
// houver). Não faz nada se nenhum item estiver ativo.
void menu_draw_active_status(void);

// Pede pra sair do modo ativo no momento (toque/joystick durante um modo
// de tela cheia) - delega pro request_exit() do item ativo, se houver.
void menu_request_exit_active(void);

// Dispara a acao contextual do modo ativo no momento (ex: botao central)
// delega pro request_action() do item ativo, se houver.
void menu_request_action_active(void);

// Navegacao contextual dentro do modo ativo
void menu_nav_up_active(void);
void menu_nav_down_active(void);
void menu_nav_select_active(void);

#ifdef __cplusplus
}
#endif

#endif // MENU_H
