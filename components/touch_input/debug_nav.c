#include "debug_nav.h"
#include "touch_input.h"
#include "audio_player.h"
#include "wifi_transfer.h"
#include "menu.h"
#include "oled_display.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static const char *TAG = "debug_nav";

#define QUEUE_MAX_ITEMS 16

typedef struct {
    uint8_t btn_mask;
    uint32_t duration_ms;
} nav_cmd_item_t;

static nav_cmd_item_t s_cmd_queue[QUEUE_MAX_ITEMS];
static int s_q_head = 0;
static int s_q_tail = 0;
static int s_q_count = 0;

static uint8_t s_cur_vmask = 0;
static uint32_t s_press_start_ms = 0;
static uint32_t s_press_duration_ms = 0;
static bool s_inter_pause = false;
static uint32_t s_pause_start_ms = 0;

static bool s_enabled = false;

static const char* ui_mode_to_string(ui_mode_t mode)
{
    switch(mode) {
        case UI_MODE_LIST: return "LIST";
        case UI_MODE_PLAYING: return "PLAYING";
        case UI_MODE_EQ: return "EQ";
        case UI_MODE_USB_PROMPT: return "USB_PROMPT";
        case UI_MODE_USB_MSC: return "USB_MSC";
        case UI_MODE_USB_DAC: return "USB_DAC";
        case UI_MODE_CONF_MENU: return "CONF_MENU";
        case UI_MODE_VOLUME: return "VOLUME";
        case UI_MODE_LED: return "LED";
        case UI_MODE_TOP_SCREEN: return "TOP_SCREEN";
        case UI_MODE_TELA: return "TELA";
        case UI_MODE_EQ_PRESETS: return "EQ_PRESETS";
        case UI_MODE_KEYBOARD: return "KEYBOARD";
        case UI_MODE_BALANCE: return "BALANCE";
        case UI_MODE_SORT: return "SORT";
        case UI_MODE_WIFI_NETS: return "WIFI_NETS";
        case UI_MODE_DEEP_SLEEP: return "DEEP_SLEEP";
        case UI_MODE_RTC_CLOCK: return "RTC_CLOCK";
        case UI_MODE_CONF_AUDIO: return "CONF_AUDIO";
        case UI_MODE_CONF_DISPLAY: return "CONF_DISPLAY";
        case UI_MODE_CONF_SYSTEM: return "CONF_SYSTEM";
        default: return "UNKNOWN";
    }
}

void debug_nav_init(void)
{
    nvs_handle_t h;
    uint8_t val = 0;
    if (nvs_open("mps3_settings", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "dbg_mode", &val) == ESP_OK) {
            s_enabled = (val != 0);
        }
        nvs_close(h);
    }
    
    // Configura nivel de log padrão da UART
    if (!s_enabled) {
        esp_log_level_set("*", ESP_LOG_WARN);
    } else {
        esp_log_level_set("*", ESP_LOG_INFO);
    }
    ESP_LOGI(TAG, "debug_nav inicializado (enabled=%d)", s_enabled);
}

void debug_nav_set_enabled(bool enable)
{
    s_enabled = enable;
    nvs_handle_t h;
    if (nvs_open("mps3_settings", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "dbg_mode", enable ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    if (enable) {
        esp_log_level_set("*", ESP_LOG_INFO);
        printf("@OK dbg on\n");
    } else {
        debug_nav_release_all();
        printf("@OK dbg off\n");
        esp_log_level_set("*", ESP_LOG_WARN);
    }
}

bool debug_nav_is_enabled(void)
{
    return s_enabled;
}

void debug_nav_release_all(void)
{
    s_q_head = 0;
    s_q_tail = 0;
    s_q_count = 0;
    s_cur_vmask = 0;
    s_press_start_ms = 0;
    s_press_duration_ms = 0;
    s_inter_pause = false;
}

esp_err_t debug_nav_press(uint8_t btn_mask, uint32_t duration_ms)
{
    if (s_q_count >= QUEUE_MAX_ITEMS) {
        return ESP_ERR_NO_MEM;
    }
    s_cmd_queue[s_q_tail].btn_mask = btn_mask;
    s_cmd_queue[s_q_tail].duration_ms = duration_ms ? duration_ms : 80;
    s_q_tail = (s_q_tail + 1) % QUEUE_MAX_ITEMS;
    s_q_count++;
    return ESP_OK;
}

static uint8_t parse_btn_char(char c)
{
    switch(toupper((unsigned char)c)) {
        case 'U': return DEBUG_BTN_UP;
        case 'D': return DEBUG_BTN_DOWN;
        case 'L': return DEBUG_BTN_LEFT;
        case 'R': return DEBUG_BTN_RIGHT;
        case 'C': return DEBUG_BTN_CENTER;
        default: return 0;
    }
}

esp_err_t debug_nav_enqueue_seq(const char *seq_str)
{
    if (!seq_str) return ESP_ERR_INVALID_ARG;
    const char *p = seq_str;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        uint8_t mask = parse_btn_char(*p);
        if (!mask) return ESP_ERR_INVALID_ARG;
        p++;
        uint32_t dur = 80;
        if (*p == ':') {
            p++;
            dur = (uint32_t)strtoul(p, (char**)&p, 10);
            if (dur == 0) dur = 80;
        }
        esp_err_t err = debug_nav_press(mask, dur);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

uint8_t debug_nav_get_vmask(uint32_t now_ms)
{
    if (!s_enabled) return 0;

    // Se estiver em pausa entre toques (para garantir liberação e borda de subida/descida)
    if (s_inter_pause) {
        if (now_ms - s_pause_start_ms >= 40) { // 40ms liberado
            s_inter_pause = false;
        } else {
            return 0;
        }
    }

    // Se há um botão sendo pressionado atualmente
    if (s_cur_vmask != 0) {
        if (now_ms - s_press_start_ms >= s_press_duration_ms) {
            // Terminou o aperto atual
            s_cur_vmask = 0;
            s_inter_pause = true;
            s_pause_start_ms = now_ms;
            return 0;
        }
        return s_cur_vmask;
    }

    // Se não há botão ativo, pega o próximo da fila
    if (s_q_count > 0) {
        nav_cmd_item_t item = s_cmd_queue[s_q_head];
        s_q_head = (s_q_head + 1) % QUEUE_MAX_ITEMS;
        s_q_count--;

        s_cur_vmask = item.btn_mask;
        s_press_start_ms = now_ms;
        s_press_duration_ms = item.duration_ms;
        return s_cur_vmask;
    }

    return 0;
}

void debug_nav_notify_mode_change(int old_mode, int new_mode)
{
    if (!s_enabled) return;
    printf("@EVT mode %s->%s (%d->%d)\n",
           ui_mode_to_string((ui_mode_t)old_mode),
           ui_mode_to_string((ui_mode_t)new_mode),
           old_mode, new_mode);
}

void debug_nav_dump_state(void)
{
    ui_mode_t mode = touch_input_get_mode();
    int cursor = touch_input_get_list_cursor();
    int top_cursor = touch_input_get_top_cursor();
    bool locked = touch_input_is_locked();
    bool sleeping = touch_input_is_sleeping();
    bool root = audio_player_browse_is_root();
    bool in_player = touch_input_is_in_player_browser();
    bool menu_active = menu_any_active();
    bool wifi = wifi_transfer_is_active();

    playback_state_t st;
    audio_player_get_state(&st);

    printf("@UI {\"mode\":%d,\"mode_name\":\"%s\",\"cursor\":%d,\"top\":%d,\"locked\":%s,\"sleeping\":%s,\"browse_root\":%s,\"in_player\":%s,\"menu_active\":%s,\"wifi\":%s,\"playing\":%s,\"paused\":%s}\n",
           (int)mode, ui_mode_to_string(mode), cursor, top_cursor,
           locked ? "true" : "false",
           sleeping ? "true" : "false",
           root ? "true" : "false",
           in_player ? "true" : "false",
           menu_active ? "true" : "false",
           wifi ? "true" : "false",
           st.playing ? "true" : "false",
           st.paused ? "true" : "false");
}

esp_err_t debug_nav_request_screenshot(void)
{
    uint8_t *fb = (uint8_t*)malloc(1024);
    if (!fb) {
        printf("@ERR no mem for fb\n");
        return ESP_ERR_NO_MEM;
    }

    if (oled_display_capture_buffer(fb) != ESP_OK) {
        free(fb);
        printf("@ERR screen not ready\n");
        return ESP_FAIL;
    }

    size_t dlen = 0;
    mbedtls_base64_encode(NULL, 0, &dlen, fb, 1024);
    char *b64 = (char*)malloc(dlen + 1);
    if (!b64) {
        free(fb);
        printf("@ERR no mem for b64\n");
        return ESP_ERR_NO_MEM;
    }

    if (mbedtls_base64_encode((unsigned char*)b64, dlen + 1, &dlen, fb, 1024) == 0) {
        b64[dlen] = '\0';
        printf("@SCR %s\n", b64);
    } else {
        printf("@ERR b64 encode fail\n");
    }

    free(b64);
    free(fb);
    return ESP_OK;
}

void debug_nav_reset_home(void)
{
    debug_nav_release_all();
    if (touch_input_is_sleeping()) {
        touch_input_wake_display();
    }
    if (menu_any_active()) {
        menu_request_exit_active();
    }
    touch_input_cancel_usb();
    printf("@OK reset home\n");
    debug_nav_dump_state();
}
