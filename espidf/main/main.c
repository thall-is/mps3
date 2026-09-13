#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "pinos.h"
#include "sd_card.h"
#include "i2s_output.h"
#include "battery.h"
#include "oled_display.h"
#include "rgb_led.h"
#include "bitmaps.h"
#include "audio_player.h"
#include "touch_input.h"
#include "eq.h"
#include "usb_manager.h"
#include "wifi_transfer.h"
#include "bt_link.h"
#include "menu.h"

static const char *TAG = "main";
static bool s_sd_ok = false;

#define MUSIC_DIR SD_MOUNT_POINT  // navegacao comeca na raiz do cartao

// Tempo sem nenhum toque na barra apos o qual a tela apaga.
#define SCREEN_IDLE_TIMEOUT_MS 60000
// Quanto tempo a barra de volume fica visivel depois de um ajuste.
#define VOLUME_DISPLAY_MS      1500

// Mesmo teto usado internamente em audio_player.cpp (MAX_ENTRIES) - usado
// so' pra dimensionar o array local de nomes formatados pro menu.
#define MAX_FILES_UI 256
#define NAME_BUF_LEN 160

static void display_task(void *arg)
{
    (void)arg;
    playback_state_t state;

    int last_volume_seen = audio_player_get_volume();
    uint32_t volume_show_until_ms = 0;

    // A API de navegacao ESCREVE cada nome num buffer (em vez de devolver
    // um ponteiro pra dado ja' existente) - pastas exigem montar uma
    // string nova ("nome/") a cada chamada, e um unico buffer estatico
    // compartilhado faria todo item de pasta na lista mostrar o mesmo
    // nome (o da ultima chamada). Alocamos os MAX_FILES_UI buffers em
    // PSRAM pra nao pesar nos 320KB de RAM interna do S3 (256*160 = 40KB).
    static char (*names_storage)[NAME_BUF_LEN] = NULL;
    static const char *names[MAX_FILES_UI];

    if (!names_storage) {
        names_storage = (char (*)[NAME_BUF_LEN])heap_caps_malloc(
            (size_t)MAX_FILES_UI * NAME_BUF_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    static bool s_was_sleeping = false;
    static ui_mode_t s_last_rendered_ui_mode = (ui_mode_t)-1;

    while (true) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);

        oled_display_update_animations();

        bool should_sleep = touch_input_is_sleeping() || touch_input_is_powered_off() || touch_input_is_locked();
        if (should_sleep) {
            if (!s_was_sleeping) {
                oled_display_blank();
                oled_display_set_power_save(true);
                s_was_sleeping = true;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        } else if (s_was_sleeping) {
            oled_display_set_power_save(false);
            s_was_sleeping = false;
        }

        ui_mode_t cur_ui_mode = touch_input_get_mode();
        bool ui_mode_changed = (cur_ui_mode != s_last_rendered_ui_mode);
        s_last_rendered_ui_mode = cur_ui_mode;

        audio_player_get_state(&state);

        int vol = audio_player_get_volume();
        if (vol != last_volume_seen) {
            last_volume_seen = vol;
            volume_show_until_ms = now + VOLUME_DISPLAY_MS;
        }

        if (menu_any_active()) {
            // Algum item do menu esta' num modo de tela cheia (USB, WiFi,
            // ou um futuro). menu_poll_active() pode levar uma fracao de
            // segundo quando a saida acontece (ex.: USB remonta o FATFS e
            // rescaneia) - aceitavel aqui, display_task nao concorre por
            // CPU com o player_task (core 1) e o player ja' esta' pausado
            // nesse momento de qualquer forma.
            menu_poll_active();
            if (menu_any_active()) {
                menu_draw_active_status();
            }
            // Se menu_poll_active() acabou de sair do modo, a proxima
            // iteracao do loop (daqui 250ms) ja' cai na lista normal.
        } else if (now < volume_show_until_ms) {
            oled_display_show_volume(vol);
        }else if (touch_input_get_mode() == UI_MODE_LIST) {

            bool at_root = audio_player_browse_is_root();
            bool in_player = touch_input_is_in_player_browser();

            if (at_root && !in_player) {
                // Menu principal com Ã­cones (Player, WiFi, Conf, USB)
                oled_display_show_main_menu(touch_input_get_list_cursor());
            } else {
                // Dentro de uma pasta: comporta-se como antes, porÃ©m sem o item ".."
                int real_count = audio_player_get_browse_entry_count();
                int count;
                if (at_root) {
                    // Raiz do Player: nÃ£o tem "..", count = real_count
                    count = real_count;
                } else {
                    // Subpasta: esconde ".."
                    count = real_count - 1;
                }
                if (count < 0) count = 0;
                if (count > MAX_FILES_UI) count = MAX_FILES_UI;
                    for (int i = 0; i < count; i++) {
                        if (names_storage) {
                            int src_idx = at_root ? i : (i + 1);  // se subpasta, pula ".."
                            audio_player_get_browse_entry_name(src_idx, names_storage[i], NAME_BUF_LEN);
                            names[i] = names_storage[i];
                        } else {
                            names[i] = "";
                        }
                    }
                oled_display_show_list(names, count, touch_input_get_list_cursor(), &state);
            }
                } else if (touch_input_get_mode() == UI_MODE_CONF_MENU) {
            const char* conf_items[] = {"Volume", "Balanco L/R", "Equalizador", "LED RGB", "Tela"};
            oled_display_show_list(conf_items, 5, touch_input_get_list_cursor(), &state);
        } else if (touch_input_get_mode() == UI_MODE_BALANCE) {
            oled_display_show_balance(audio_player_get_balance());
        } else if (touch_input_get_mode() == UI_MODE_TELA) {
            oled_display_show_tela(touch_input_get_tela_cursor(), touch_input_get_oled_brightness(), touch_input_get_timeout_idx());
        } else if (touch_input_get_mode() == UI_MODE_VOLUME) {
            oled_display_show_volume(audio_player_get_volume());
        } else if (touch_input_get_mode() == UI_MODE_EQ_PRESETS) {
            player_eq_config_t cfg;
            audio_player_get_eq_config(&cfg);
            oled_display_show_eq_preset_list(touch_input_get_eq_preset_cursor(), cfg.active_preset_idx, cfg.enabled, &cfg);
        } else if (touch_input_get_mode() == UI_MODE_KEYBOARD) {
            char text[24];
            int cursor, grid_x, grid_y, page;
            bool is_confirming = false;
            touch_input_get_keyboard_state(text, &cursor, &grid_x, &grid_y, &page, &is_confirming);
            oled_display_show_keyboard(text, cursor, grid_x, grid_y, page, is_confirming);
        } else if (touch_input_get_mode() == UI_MODE_EQ) {
            player_eq_config_t cfg;
            audio_player_get_eq_config(&cfg);
            float gains[11];
            for (int i = 0; i < 10; i++) gains[i] = cfg.presets[cfg.active_preset_idx].band_gains[i];
            gains[10] = cfg.presets[cfg.active_preset_idx].overall_gain;
            oled_display_show_eq(gains, touch_input_get_eq_band(), cfg.enabled);
        } else if (touch_input_get_mode() == UI_MODE_LED) {
            rgb_led_config_t lcfg;
            rgb_led_get_config(&lcfg);
            oled_display_show_led(lcfg.r, lcfg.g, lcfg.b, touch_input_get_led_channel(), lcfg.enabled);
        } else if (touch_input_get_mode() == UI_MODE_TOP_SCREEN) {
            int mv = 0, pct = -1, time_left = -1;
            battery_get_info(&mv, &pct, &time_left);
            int volume = audio_player_get_volume();
            player_eq_config_t cfg;
            audio_player_get_eq_config(&cfg);
            oled_display_show_top_screen(volume, &cfg, touch_input_get_top_eq_focus(), mv / 1000.0f, pct, time_left, touch_input_get_top_cursor());
        } else if (touch_input_get_mode() == UI_MODE_USB_PROMPT) {
            oled_display_show_usb_prompt(touch_input_get_list_cursor());
        } else if (touch_input_get_mode() == UI_MODE_USB_MSC) {
            static int s_msc_state = -1;
            if (ui_mode_changed) s_msc_state = -1;
            if (s_msc_state != 1) {
                oled_display_show_usb_msc();
                s_msc_state = 1;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        } else if (touch_input_get_mode() == UI_MODE_USB_DAC) {
            static int s_last_dac_vol = -1;
            static int s_last_dac_bal = -999;
            static uint32_t s_last_dac_rate = 0;
            static uint32_t s_last_dac_pkts = 999999;
            static int s_last_dac_eq_preset = -1;
            static bool s_last_dac_eq_en = false;
            static int s_dac_state = -1;
            if (ui_mode_changed) s_dac_state = -1;

            int cur_vol = audio_player_get_volume();
            int cur_bal = audio_player_get_balance();
            uint32_t cur_rate = usb_manager_get_sample_rate();
            uint32_t cur_pkts = usb_manager_get_pkt_count();

            player_eq_config_t eq_cfg;
            audio_player_get_eq_config(&eq_cfg);

            if (s_dac_state != 1 || cur_vol != s_last_dac_vol || cur_bal != s_last_dac_bal || 
                cur_rate != s_last_dac_rate || eq_cfg.active_preset_idx != s_last_dac_eq_preset ||
                eq_cfg.enabled != s_last_dac_eq_en ||
                (cur_pkts != s_last_dac_pkts && (cur_pkts % 50 == 0 || s_last_dac_pkts == 999999))) {
                s_last_dac_vol = cur_vol;
                s_last_dac_bal = cur_bal;
                s_last_dac_rate = cur_rate;
                s_last_dac_pkts = cur_pkts;
                s_last_dac_eq_preset = eq_cfg.active_preset_idx;
                s_last_dac_eq_en = eq_cfg.enabled;
                s_dac_state = 1;
                oled_display_show_usb_dac();
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        } else {
            if (!s_sd_ok) {
                oled_display_show_sd_error();
            } else if (state.no_files_found) {
                oled_display_show_message("Nenhum audio", "encontrado no SD");
            } else if (state.last_error[0] != '\0') {
                if (strncmp(state.last_error, "Formato", 7) == 0) {
                    oled_display_show_unsupported(state.filename);
                } else {
                    oled_display_show_message(state.last_error, state.filename);
                }
            } else if (!state.track_loaded) {
                oled_display_show_loading();
            } else {
                oled_display_show_now_playing(&state);
            }
        }

        // Intervalo de 250ms para a rolagem do nome ficar fluida. Roda
        // isolada no core 0, nao disputa CPU com o player_task (core 1),
        // entao nao tem custo real em audio.
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Iniciando mps3...");

    // Boilerplate padrao do ESP-IDF: a particao NVS pode precisar ser
    // reformatada apos mudanca de tamanho de flash/versao - usada aqui
    // pra lembrar a ultima faixa/posicao tocada entre religadas.
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    esp_err_t ret;

    ret = oled_display_init();
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar o display OLED (codigo %d). Continuando sem display.", ret);
    }

    bitmap_animations_init();

    rgb_led_init();
    ret = battery_init(); 
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Bateria nao configurada (codigo %d).", ret);
    }

    s_sd_ok = (sd_card_init() == ESP_OK);
    if (!s_sd_ok) {
        ESP_LOGE(TAG, "Falha ao montar o cartao SD. Continuando sem SD para permitir USB/Controles.");
    }

    ret = i2s_output_init();
    if (ret != ESP_OK) {
        oled_display_show_message("Erro no I2S", "verifique a fiacao");
        ESP_LOGE(TAG, "Falha ao iniciar o I2S. Abortando.");
        return;
    }

    bt_link_init();

    if (s_sd_ok) {
        ret = audio_player_start(MUSIC_DIR);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao iniciar a task de reproducao.");
        }
    }

    // Splash de boot animado
    for (int i=0; i<20; i++) {
        oled_display_show_loading();
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // --- Registro do menu principal --------------------------------------
    // A ORDEM das chamadas abaixo e' a ordem visual do carrossel do menu
    // (0=Player, 1=WiFi, 2=Bluetooth, 3=Conf, 4=USB, 5=Game - ver os "case" em
    // oled_display_show_main_menu).
    touch_input_register_player_entry();  // indice 0
    wifi_transfer_register_menu_entry();  // indice 1
    bt_link_register_menu_entry();        // indice 2
    touch_input_register_conf_entry();    // indice 3
    touch_input_register_usb_entry();     // indice 4
    touch_input_register_game_entry();    // indice 5

    ret = touch_input_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar os controles do joystick (codigo %d). Continuando sem controles.", ret);
    }

    ret = usb_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar o USB Manager");
    }

    // 4096 -> 8192: a tela de reproducao/lista cresceu bastante desde que
    // esse numero foi escolhido (metadados, bateria, cracha de formato,
    // 3 marquees separados, barra de rolagem) sem o stack acompanhar. Ja
    // vimos nesse projeto (main_task) que 4096 e' marginal mesmo pra
    // codigo bem mais simples que isso - e um estouro de pilha aqui tem
    // exatamente a cara de "tela apaga do nada, precisa reiniciar" (a
    // task reinicia o sistema com panic, mas por fora so' se ve a tela
    // preta).
    xTaskCreatePinnedToCore(display_task, "display_task", 8192, NULL, 2, NULL, 0);

    ESP_LOGI(TAG, "mps3 rodando.");
}

