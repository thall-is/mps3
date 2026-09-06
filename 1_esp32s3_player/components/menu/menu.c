#include "menu.h"
#include <string.h>
#include <stddef.h>

static menu_item_t s_items[MENU_MAX_ITEMS];
static int s_count = 0;

void menu_register(const menu_item_t *item)
{
    if (!item || s_count >= MENU_MAX_ITEMS) return;
    s_items[s_count++] = *item;
}

int menu_count(void)
{
    return s_count;
}

const menu_item_t *menu_get(int index)
{
    if (index < 0 || index >= s_count) return NULL;
    return &s_items[index];
}

void menu_select(int index)
{
    const menu_item_t *item = menu_get(index);
    if (item && item->on_select) item->on_select();
}

// Encontra o item ativo agora, se houver (por construção, no máximo um
// item de tela cheia fica ativo por vez - entrar em outro só é possível a
// partir do menu principal, que fica bloqueado enquanto um já está ativo).
static const menu_item_t *find_active(void)
{
    for (int i = 0; i < s_count; i++) {
        if (s_items[i].is_active && s_items[i].is_active()) {
            return &s_items[i];
        }
    }
    return NULL;
}

bool menu_any_active(void)
{
    return find_active() != NULL;
}

bool menu_poll_active(void)
{
    const menu_item_t *item = find_active();
    if (item && item->poll) return item->poll();
    return false;
}

void menu_draw_active_status(void)
{
    const menu_item_t *item = find_active();
    if (item && item->draw_status) item->draw_status();
}

void menu_request_exit_active(void)
{
    const menu_item_t *item = find_active();
    if (item && item->request_exit) item->request_exit();
}

void menu_request_action_active(void)
{
    const menu_item_t *item = find_active();
    if (item && item->request_action) item->request_action();
}

void menu_nav_up_active(void)
{
    const menu_item_t *item = find_active();
    if (item && item->nav_up) item->nav_up();
}

void menu_nav_down_active(void)
{
    const menu_item_t *item = find_active();
    if (item && item->nav_down) item->nav_down();
}

void menu_nav_select_active(void)
{
    const menu_item_t *item = find_active();
    if (item && item->nav_select) item->nav_select();
}
