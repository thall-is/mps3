#include "bt_link.h"
#include "uart_ctrl.h"
#include "menu.h"
#include "oled_display.h"
#include "i2s_output.h"
#include "audio_player.h"
#include "pinos.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/uart.h"

#include <string.h>
#include <stdio.h>
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "bt_link";

#define BT_LINK_UART_NUM UART_NUM_1
#define BT_LINK_BAUD     115200
#define BT_MAX_DEVICES   8

typedef struct {
    uint8_t bda[6];
    int8_t  rssi;
    char    name[32];
} bt_dev_entry_t;

static SemaphoreHandle_t s_status_mutex = NULL;
static uart_status_payload_t s_status = {0};
static bool s_have_status = false;

static bt_dev_entry_t s_devs[BT_MAX_DEVICES];
static int s_dev_count = 0;
static int s_cursor = 0; // 0 = [ Buscar Fones ], 1 = [ Codec: ... ], 2..s_dev_count+1 = fones encontrados
static bool s_scanning = false;
static char s_connected_name[32] = "";
static int8_t s_connected_rssi = 0;
static codec_preference_t s_codec_pref = CODEC_PREF_AUTO;
static uint16_t s_active_bitrate_kbps = 0;

static bool s_screen_active = false;
static bool s_exit_requested = false;

static const char *codec_pref_name(codec_preference_t pref)
{
    switch (pref) {
        case CODEC_PREF_AUTO:    return "Auto";
        case CODEC_PREF_LDAC:    return "LDAC";
        case CODEC_PREF_APTX_HD: return "aptX HD";
        case CODEC_PREF_APTX:    return "aptX";
        case CODEC_PREF_SBC:     return "SBC";
        default:                 return "Auto";
    }
}

static void send_get_status(void)
{
    uart_ctrl_send(UART_CMD_GET_STATUS, NULL, 0);
}

static void on_i2s_rate_change(uint32_t rate)
{
    uint8_t payload[4];
    memcpy(payload, &rate, 4);
    uart_ctrl_send(UART_CMD_SET_SAMPLE_RATE, payload, sizeof(payload));
}

static void on_uart_frame(uart_ctrl_cmd_t cmd, const uint8_t *payload, size_t len, void *ctx)
{
    (void)ctx;
    switch (cmd) {
        case UART_EVT_STATUS:
            if (len >= sizeof(uart_status_payload_t)) {
                xSemaphoreTake(s_status_mutex, portMAX_DELAY);
                bool was_connected = ((uart_link_state_t)s_status.state >= UART_LINK_CONNECTED_SBC);
                memcpy(&s_status, payload, sizeof(uart_status_payload_t));
                s_have_status = true;
                bool is_connected = ((uart_link_state_t)s_status.state >= UART_LINK_CONNECTED_SBC);
                if (is_connected) {
                    s_scanning = false;
                    for (int i = 0; i < s_dev_count; i++) {
                        if (memcmp(s_devs[i].bda, s_status.bt_addr, 6) == 0) {
                            strncpy(s_connected_name, s_devs[i].name, sizeof(s_connected_name) - 1);
                            s_connected_rssi = s_devs[i].rssi;
                            break;
                        }
                    }
                } else if ((uart_link_state_t)s_status.state == UART_LINK_IDLE) {
                    s_connected_name[0] = '\0';
                }
                xSemaphoreGive(s_status_mutex);

                if (is_connected && !was_connected) {
                    bt_link_send_volume((uint8_t)audio_player_get_volume());
                    on_i2s_rate_change(i2s_output_get_rate());
                }
            }
            break;

        case UART_EVT_SCAN_RESULT:
            if (len >= sizeof(uart_scan_result_t)) {
                uart_scan_result_t r;
                memcpy(&r, payload, sizeof(uart_scan_result_t));
                xSemaphoreTake(s_status_mutex, portMAX_DELAY);
                int found_idx = -1;
                for (int i = 0; i < s_dev_count; i++) {
                    if (memcmp(s_devs[i].bda, r.bda, 6) == 0) {
                        found_idx = i;
                        break;
                    }
                }
                if (found_idx >= 0) {
                    s_devs[found_idx].rssi = r.rssi;
                    if (r.name[0] != '\0' && strncmp(s_devs[found_idx].name, "BT-", 3) == 0) {
                        strncpy(s_devs[found_idx].name, r.name, sizeof(s_devs[found_idx].name) - 1);
                    }
                } else if (s_dev_count < BT_MAX_DEVICES) {
                    memcpy(s_devs[s_dev_count].bda, r.bda, 6);
                    s_devs[s_dev_count].rssi = r.rssi;
                    strncpy(s_devs[s_dev_count].name, r.name, sizeof(s_devs[s_dev_count].name) - 1);
                    s_devs[s_dev_count].name[sizeof(s_devs[s_dev_count].name) - 1] = '\0';
                    s_dev_count++;
                }
                xSemaphoreGive(s_status_mutex);
            }
            break;

        case UART_EVT_SCAN_COMPLETE:
            xSemaphoreTake(s_status_mutex, portMAX_DELAY);
            s_scanning = false;
            xSemaphoreGive(s_status_mutex);
            ESP_LOGI(TAG, "Busca concluida. Fones encontrados: %d", s_dev_count);
            break;

        case UART_EVT_VOLUME_CHANGED:
            if (len >= 1) {
                ESP_LOGI(TAG, "Fone alterou volume para: %d%%", payload[0]);
                audio_player_set_volume((int)payload[0]);
            }
            break;

        case UART_EVT_CODEC_INFO:
            if (len >= sizeof(uart_codec_info_t)) {
                uart_codec_info_t info;
                memcpy(&info, payload, sizeof(info));
                xSemaphoreTake(s_status_mutex, portMAX_DELAY);
                s_active_bitrate_kbps = info.bitrate_kbps;
                xSemaphoreGive(s_status_mutex);
                ESP_LOGI(TAG, "Codec ativo: %s (%u kbps)", codec_pref_name((codec_preference_t)info.codec), (unsigned)info.bitrate_kbps);
            }
            break;

        case UART_CMD_PONG:
            ESP_LOGI(TAG, "Companheiro respondeu PING");
            break;

        default:
            break;
    }
}

static void on_player_volume_changed(int volume)
{
    bt_link_send_volume((uint8_t)volume);
}

void bt_link_init(void)
{
    s_status_mutex = xSemaphoreCreateMutex();

    // Carrega preferencia de codec salva na NVS
    nvs_handle_t h;
    if (nvs_open("mps3_settings", NVS_READONLY, &h) == ESP_OK) {
        uint8_t val = 0;
        if (nvs_get_u8(h, "bt_codec_pref", &val) == ESP_OK && val < CODEC_PREF_COUNT) {
            s_codec_pref = (codec_preference_t)val;
            ESP_LOGI(TAG, "Preferencia de codec carregada da NVS: %s", codec_pref_name(s_codec_pref));
        }
        nvs_close(h);
    }

#if defined(PIN_BT_LINK_EN) && (PIN_BT_LINK_EN >= 0)
    gpio_config_t en_cfg = {
        .pin_bit_mask = 1ULL << PIN_BT_LINK_EN,
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&en_cfg);
    gpio_set_level((gpio_num_t)PIN_BT_LINK_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level((gpio_num_t)PIN_BT_LINK_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(150));
#endif

    uart_ctrl_config_t cfg = {
        .uart_num  = BT_LINK_UART_NUM,
        .tx_gpio   = PIN_BT_LINK_UART_TX,
        .rx_gpio   = PIN_BT_LINK_UART_RX,
        .baud_rate = BT_LINK_BAUD,
        .rx_cb     = on_uart_frame,
        .user_ctx  = NULL,
    };
    if (uart_ctrl_init(&cfg) != 0) {
        ESP_LOGE(TAG, "Falha ao iniciar UART do companheiro de Bluetooth");
        return;
    }

    i2s_output_set_rate_change_cb(on_i2s_rate_change);
    audio_player_set_volume_change_cb(on_player_volume_changed);
    send_get_status();

    // Envia a preferencia de codec para o companheiro
    uint8_t pref_byte = (uint8_t)s_codec_pref;
    uart_ctrl_send(UART_CMD_SET_CODEC, &pref_byte, 1);

    ESP_LOGI(TAG, "bt_link pronto (TX=%d RX=%d | Codec Pref: %s)",
             PIN_BT_LINK_UART_TX, PIN_BT_LINK_UART_RX, codec_pref_name(s_codec_pref));
}

static const char *codec_label(uint8_t state)
{
    switch ((uart_link_state_t)state) {
        case UART_LINK_CONNECTED_LDAC:   return "LDAC";
        case UART_LINK_CONNECTED_SBC:    return "SBC";
        case UART_LINK_CONNECTED_APTX:   return "aptX";
        case UART_LINK_CONNECTED_APTX_HD:return "aptX HD";
        case UART_LINK_CONNECTING:       return "Conectando...";
        case UART_LINK_SCANNING:         return "Buscando...";
        default:                         return "Desconectado";
    }
}

static void bt_link_on_select(void)
{
    s_screen_active = true;
    s_exit_requested = false;

    send_get_status();

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    uart_link_state_t state = s_have_status ? (uart_link_state_t)s_status.state : UART_LINK_IDLE;
    xSemaphoreGive(s_status_mutex);

    if (state == UART_LINK_IDLE) {
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_dev_count = 0;
        s_cursor = 0;
        s_scanning = true;
        xSemaphoreGive(s_status_mutex);
        uart_ctrl_send(UART_CMD_SCAN_START, NULL, 0);
    }
}

static bool bt_link_is_active(void)
{
    return s_screen_active;
}

static bool bt_link_poll(void)
{
    if (s_exit_requested) {
        s_screen_active = false;
        s_exit_requested = false;
        return true;
    }
    return false;
}

static void bt_link_draw_status(void)
{
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    uart_status_payload_t st = s_status;
    bool have = s_have_status;
    int count = s_dev_count;
    int cur = s_cursor;
    bool scanning = s_scanning;
    char conn_name[32];
    strncpy(conn_name, s_connected_name, sizeof(conn_name));
    int8_t conn_rssi = s_connected_rssi;
    uint16_t cur_bitrate = s_active_bitrate_kbps;
    codec_preference_t cur_pref = s_codec_pref;

    char item_labels[BT_MAX_DEVICES + 2][36];
    const char *display_ptrs[BT_MAX_DEVICES + 2];

    if (scanning) {
        snprintf(item_labels[0], sizeof(item_labels[0]), "[ Parar Busca ]");
    } else {
        snprintf(item_labels[0], sizeof(item_labels[0]), "[ Buscar Fones ]");
    }
    display_ptrs[0] = item_labels[0];

    snprintf(item_labels[1], sizeof(item_labels[1]), "[ Codec: %s ]", codec_pref_name(cur_pref));
    display_ptrs[1] = item_labels[1];

    for (int i = 0; i < count; i++) {
        snprintf(item_labels[i + 2], sizeof(item_labels[i + 2]), "%d. %s",
                 i + 1, s_devs[i].name);
        display_ptrs[i + 2] = item_labels[i + 2];
    }
    int total_items = 2 + count;
    xSemaphoreGive(s_status_mutex);

    if (!have) {
        oled_display_show_message("Bluetooth", "Iniciando...");
        return;
    }

    if ((uart_link_state_t)st.state == UART_LINK_CONNECTING) {
        oled_display_show_message("Conectando...", conn_name[0] ? conn_name : "Fone");
        return;
    }

    if ((uart_link_state_t)st.state >= UART_LINK_CONNECTED_SBC) {
        char full_codec[32];
        if (cur_bitrate > 0) {
            snprintf(full_codec, sizeof(full_codec), "%s %uk", codec_label(st.state), (unsigned)cur_bitrate);
        } else {
            snprintf(full_codec, sizeof(full_codec), "%s", codec_label(st.state));
        }
        oled_display_show_bt_connected(conn_name, full_codec, st.sample_rate, conn_rssi);
        return;
    }

    oled_display_show_bt_devices(display_ptrs, total_items, cur, scanning);
}

static void bt_link_request_exit(void)
{
    s_exit_requested = true;
}

static void bt_link_nav_up(void)
{
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    if (s_cursor > 0) {
        s_cursor--;
    }
    xSemaphoreGive(s_status_mutex);
}

static void bt_link_nav_down(void)
{
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    int max_idx = 1 + s_dev_count; // total items: 2 + s_dev_count, indices 0..(1+s_dev_count)
    if (s_cursor < max_idx) {
        s_cursor++;
    }
    xSemaphoreGive(s_status_mutex);
}

static void bt_link_nav_select(void)
{
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    uart_link_state_t state = s_have_status ? (uart_link_state_t)s_status.state : UART_LINK_IDLE;
    int cur = s_cursor;
    int count = s_dev_count;
    bool scanning = s_scanning;
    uint8_t target_bda[6];
    bool do_connect = false;
    bool do_rescan = false;
    bool do_stop_scan = false;
    bool do_cycle_codec = false;

    if (state < UART_LINK_CONNECTED_SBC) {
        if (cur == 0) {
            if (scanning) {
                do_stop_scan = true;
            } else {
                do_rescan = true;
            }
        } else if (cur == 1) {
            do_cycle_codec = true;
        } else if (cur - 2 < count) {
            memcpy(target_bda, s_devs[cur - 2].bda, 6);
            strncpy(s_connected_name, s_devs[cur - 2].name, sizeof(s_connected_name) - 1);
            s_connected_rssi = s_devs[cur - 2].rssi;
            do_connect = true;
        }
    } else {
        ESP_LOGI(TAG, "Fone conectado! Disparando tom senoidal de teste (1 kHz, 3s)...");
        i2s_output_play_test_tone(1000, 3000);
    }
    xSemaphoreGive(s_status_mutex);

    if (do_cycle_codec) {
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_codec_pref = (codec_preference_t)((s_codec_pref + 1) % CODEC_PREF_COUNT);
        codec_preference_t new_pref = s_codec_pref;
        xSemaphoreGive(s_status_mutex);

        nvs_handle_t h;
        if (nvs_open("mps3_settings", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, "bt_codec_pref", (uint8_t)new_pref);
            nvs_commit(h);
            nvs_close(h);
        }
        uint8_t pref_byte = (uint8_t)new_pref;
        uart_ctrl_send(UART_CMD_SET_CODEC, &pref_byte, 1);
        ESP_LOGI(TAG, "Preferencia de codec ciclada para: %s", codec_pref_name(new_pref));
    } else if (do_stop_scan) {
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_scanning = false;
        xSemaphoreGive(s_status_mutex);
        uart_ctrl_send(UART_CMD_SCAN_STOP, NULL, 0);
    } else if (do_rescan) {
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_dev_count = 0;
        s_cursor = 0;
        s_scanning = true;
        xSemaphoreGive(s_status_mutex);
        uart_ctrl_send(UART_CMD_SCAN_START, NULL, 0);
    } else if (do_connect) {
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_scanning = false;
        xSemaphoreGive(s_status_mutex);
        ESP_LOGI(TAG, "Conectando ao fone selecionado");
        uart_ctrl_send(UART_CMD_CONNECT_ADDR, target_bda, 6);
    }
}

static void bt_link_request_action(void)
{
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    uart_link_state_t state = s_have_status ? (uart_link_state_t)s_status.state : UART_LINK_IDLE;
    xSemaphoreGive(s_status_mutex);

    if (state >= UART_LINK_CONNECTED_SBC) {
        ESP_LOGI(TAG, "Acao: Desconectar");
        uart_ctrl_send(UART_CMD_DISCONNECT, NULL, 0);
    } else {
        bt_link_nav_select();
    }
}

void bt_link_register_menu_entry(void)
{
    static const menu_item_t item = {
        .name = "Bluetooth",
        .on_select = bt_link_on_select,
        .is_active = bt_link_is_active,
        .poll = bt_link_poll,
        .draw_status = bt_link_draw_status,
        .request_exit = bt_link_request_exit,
        .request_action = bt_link_request_action,
        .nav_up = bt_link_nav_up,
        .nav_down = bt_link_nav_down,
        .nav_select = bt_link_nav_select,
    };
    menu_register(&item);
}

void bt_link_reset_companion(void)
{
#if defined(PIN_BT_LINK_EN) && (PIN_BT_LINK_EN >= 0)
    gpio_set_level((gpio_num_t)PIN_BT_LINK_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level((gpio_num_t)PIN_BT_LINK_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(150));
#endif
}

void bt_link_send_volume(uint8_t volume_percent)
{
    if (volume_percent > 100) volume_percent = 100;
    uart_ctrl_send(UART_CMD_SET_VOLUME, &volume_percent, 1);
}
