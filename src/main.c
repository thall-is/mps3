#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <time.h>
#include <sys/time.h>

#include "pinos.h"
#include "sd_card.h"
#include "i2s_output.h"
#include "battery.h"
#include "oled_display.h"
#include "u8g2_hal.h"
#include "rtc_ds3231.h"
#include "rgb_led.h"
#include "bitmaps.h"
#include "audio_player.h"
#include "touch_input.h"
#include "eq.h"
#include "usb_manager.h"
#include "wifi_transfer.h"
#include "bt_link.h"
#include "menu.h"
#include "podcast_sync.h"
#include "pwr_governor.h"

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
    ESP_LOGI("display", "display_task INICIADA no Core %d (prio %d)", xPortGetCoreID(), (int)uxTaskPriorityGet(NULL));
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
        uint32_t frame_start_ms = now;

        bool should_sleep = touch_input_is_sleeping() || touch_input_is_powered_off() || touch_input_is_locked();
        if (should_sleep) {
            if (!s_was_sleeping) {
                ESP_LOGI(TAG, "Tela em repouso/bloqueio. Desligando OLED e suspendendo display_task...");
                oled_display_blank();
                oled_display_set_power_save(true);
                s_was_sleeping = true;
            }
            // Avalia estado do player e comuta CPU para perfil de repouso (ex: 80 MHz para MP3/pausado)
            audio_player_get_state(&state);
            bool wifi_active = (wifi_transfer_is_active() && wifi_transfer_is_transferring()) || podcast_sync_is_busy();
            pwr_governor_update(false, wifi_active, audio_player_is_seeking(), state.playing, state.sample_rate);

            // Gerenciamento de inatividade prolongada para Deep Sleep
            uint32_t ds_timeout_ms = touch_input_get_deepsleep_ms();
            uint32_t last_act_ms = touch_input_get_last_activity_ms();
            if (ds_timeout_ms > 0 && !state.playing && !wifi_active && !podcast_sync_is_busy()) {
                if (now >= last_act_ms && (now - last_act_ms) >= ds_timeout_ms) {
                    ESP_LOGI(TAG, "Inatividade atingiu tempo limite (%u s). Entrando em Deep Sleep...", (unsigned)(ds_timeout_ms / 1000));
                    pwr_governor_enter_deep_sleep();
                }
                // Entra em Light Sleep temporizado (1000 ms) para economizar energia na bancada (~2-3 mA)
                pwr_governor_enter_light_sleep(1000);
            } else {
                // Aguarda sinal de acordar (touch_task / eventos) sem gastar CPU nem barramento I2C.
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            }
            continue;
        } else if (s_was_sleeping) {
            ESP_LOGI(TAG, "Tela acordando do repouso/bloqueio. Reativando OLED...");
            oled_display_set_power_save(false);
            s_was_sleeping = false;
            s_last_rendered_ui_mode = (ui_mode_t)-1;
            // Ao acordar, restaura frequência para UI fluida (160 MHz)
            audio_player_get_state(&state);
            bool wifi_active = (wifi_transfer_is_active() && wifi_transfer_is_transferring()) || podcast_sync_is_busy();
            pwr_governor_update(true, wifi_active, audio_player_is_seeking(), state.playing, state.sample_rate);
        }

        oled_display_update_animations();

        ui_mode_t cur_ui_mode = touch_input_get_mode();
        bool ui_mode_changed = (cur_ui_mode != s_last_rendered_ui_mode);
        s_last_rendered_ui_mode = cur_ui_mode;

        audio_player_get_state(&state);

        // Atualizacao dinamica de clock com tela acordada
        bool wifi_active = (wifi_transfer_is_active() && wifi_transfer_is_transferring()) || podcast_sync_is_busy();
        pwr_governor_update(true, wifi_active, audio_player_is_seeking(), state.playing, state.sample_rate);

        // Checagem universal de inatividade para Deep Sleep (pausado e sem uso, mesmo com tela ligada)
        uint32_t ds_timeout_ms = touch_input_get_deepsleep_ms();
        uint32_t last_act_ms = touch_input_get_last_activity_ms();
        if (ds_timeout_ms > 0 && !state.playing && !wifi_active && !podcast_sync_is_busy() &&
            cur_ui_mode != UI_MODE_USB_MSC && cur_ui_mode != UI_MODE_USB_DAC && cur_ui_mode != UI_MODE_USB_PROMPT) {
            if (now >= last_act_ms && (now - last_act_ms) >= ds_timeout_ms) {
                ESP_LOGI(TAG, "Inatividade prolongada atingida (%u s). Entrando em Deep Sleep...", (unsigned)(ds_timeout_ms / 1000));
                pwr_governor_enter_deep_sleep();
            }
        }

        // Auto-sincronizacao de podcasts ao plugar carregador TP4056 (GPIO 11)
        static bool s_last_chrg_active = false;
        bool is_charging = (battery_get_status() == BATTERY_CHARGING);
        if (is_charging && !s_last_chrg_active) {
            s_last_chrg_active = true;
            if (!podcast_sync_is_busy() && !state.playing) {
                ESP_LOGI(TAG, "Carregador plugado! Disparando sincronizacao automatica de podcasts...");
                podcast_sync_start();
            }
        } else if (!is_charging) {
            s_last_chrg_active = false;
        }

        int vol = audio_player_get_volume();
        if (vol != last_volume_seen) {
            last_volume_seen = vol;
            volume_show_until_ms = now + VOLUME_DISPLAY_MS;
        }

        if (podcast_sync_is_busy()) {
            podcast_sync_progress_t p;
            podcast_sync_get_progress(&p);
            oled_display_show_podcast_sync(p.current_program, p.current_title,
                                           p.current_idx, p.total_count,
                                           p.current_pct, p.speed_kbs, p.status_msg);
            vTaskDelay(pdMS_TO_TICKS(250)); // 4 FPS durante sync para dedicar 95% da CPU e rede
            continue;
        } else if (menu_any_active()) {
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
        } else if (now < volume_show_until_ms && cur_ui_mode != UI_MODE_USB_DAC) {
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
            const char* conf_items[] = {"Volume", "Balanco L/R", "Equalizador", "LED RGB", "Tela", "Ordenar", "Redes Wi-Fi", "Deep Sleep"};
            oled_display_show_list(conf_items, 8, touch_input_get_list_cursor(), &state);
        } else if (touch_input_get_mode() == UI_MODE_DEEP_SLEEP) {
            oled_display_show_deepsleep_cfg(touch_input_get_deepsleep_idx());
        } else if (touch_input_get_mode() == UI_MODE_WIFI_NETS) {
            int known = wifi_transfer_get_known_count();
            int total = 1 + known;
            int cur = touch_input_get_wifi_net_cursor();
            if (cur < 0) cur = 0;
            if (cur >= total) cur = total - 1;

            char ssid[64] = {0};
            char pass[64] = {0};
            const char *tag = "HOTSPOT";

            if (cur == 0) {
                wifi_transfer_get_ap_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
                tag = "HOTSPOT";
            } else {
                wifi_transfer_get_known_network(cur - 1, ssid, sizeof(ssid), pass, sizeof(pass));
                tag = "SALVA";
            }
            oled_display_show_wifi_qr(ssid, pass, tag, cur + 1, total);
        } else if (touch_input_get_mode() == UI_MODE_SORT) {
            oled_display_show_sort_mode((int)audio_player_get_sort_mode());
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
            static bool s_last_dac_streaming = false;
            static int s_last_dac_eq_preset = -1;
            static bool s_last_dac_eq_en = false;
            static int s_dac_state = -1;
            if (ui_mode_changed) s_dac_state = -1;

            int cur_vol = audio_player_get_volume();
            int cur_bal = audio_player_get_balance();
            uint32_t cur_rate = usb_manager_get_sample_rate();
            bool cur_streaming = usb_manager_is_streaming();

            player_eq_config_t eq_cfg;
            audio_player_get_eq_config(&eq_cfg);

            if (s_dac_state != 1 || cur_vol != s_last_dac_vol || cur_bal != s_last_dac_bal || 
                cur_rate != s_last_dac_rate || eq_cfg.active_preset_idx != s_last_dac_eq_preset ||
                eq_cfg.enabled != s_last_dac_eq_en ||
                cur_streaming != s_last_dac_streaming) {
                s_last_dac_vol = cur_vol;
                s_last_dac_bal = cur_bal;
                s_last_dac_rate = cur_rate;
                s_last_dac_streaming = cur_streaming;
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

        // Controle de taxa de atualizacao (pacing dinamico para 25 FPS / 40 ms).
        // Roda no Core 0 com prioridade 3: nunca disputa CPU com player_task (Core 1)
        // e cede CPU instantaneamente para audio_dsp_task (Core 0, prioridade 5)
        // e touch_task (Core 0, prioridade 5).
        uint32_t frame_elapsed_ms = (uint32_t)(esp_timer_get_time() / 1000ULL) - frame_start_ms;
        bool is_transferring = (wifi_transfer_is_active() && wifi_transfer_is_transferring()) || podcast_sync_is_busy();
        const uint32_t target_frame_ms = is_transferring ? 250 : 40; // 4 FPS durante transferencias (libera 95% do barramento I2C e Core 0) / 25 FPS normal
        if (frame_elapsed_ms < target_frame_ms) {
            uint32_t sleep_ms = target_frame_ms - frame_elapsed_ms;
            vTaskDelay(pdMS_TO_TICKS(sleep_ms < 2 ? 2 : sleep_ms));
        } else {
            vTaskDelay(pdMS_TO_TICKS(2));
        }

        static uint32_t s_last_fps_log_ms = 0;
        static uint32_t s_fps_frame_count = 0;
        static uint32_t s_fps_elapsed_sum = 0;
        s_fps_frame_count++;
        s_fps_elapsed_sum += frame_elapsed_ms;
        if (now - s_last_fps_log_ms >= 5000) {
            float avg_frame_ms = s_fps_frame_count ? (float)s_fps_elapsed_sum / s_fps_frame_count : 0.0f;
            float fps = (now > s_last_fps_log_ms) ? (float)s_fps_frame_count * 1000.0f / (now - s_last_fps_log_ms) : 0.0f;
            ESP_LOGI("display", "[TELEMETRIA OLED] FPS: %.1f | Frame: %.1f ms (render+I2C) | Target: %u ms (~%u FPS)",
                     fps, avg_frame_ms, (unsigned)target_frame_ms, (unsigned)(1000 / target_frame_ms));
            s_last_fps_log_ms = now;
            s_fps_frame_count = 0;
            s_fps_elapsed_sum = 0;
        }
    }
}

static void uart_cmd_task(void *arg)
{
    (void)arg;
    char line_buf[64];
    int line_len = 0;

    while (1) {
        int c = fgetc(stdin);
        if (c != EOF && c > 0) {
            if (c == '\r' || c == '\n') {
                if (line_len > 0) {
                    line_buf[line_len] = '\0';
                    if (strcmp(line_buf, "wifi") == 0 || strcmp(line_buf, "w") == 0) {
                        ESP_LOGI("UART_CMD", "Comando recebido: entrar no modo WiFi Auto (APSTA)");
                        if (!wifi_transfer_is_active()) {
                            wifi_transfer_enter_auto();
                        }
                    } else if (strcmp(line_buf, "ap") == 0) {
                        ESP_LOGI("UART_CMD", "Comando recebido: entrar no modo Hotspot AP");
                        if (!wifi_transfer_is_active()) {
                            wifi_transfer_enter(WIFI_TRANSFER_MODE_AP);
                        }
                    } else if (strcmp(line_buf, "sta") == 0) {
                        ESP_LOGI("UART_CMD", "Comando recebido: entrar no modo STA");
                        if (!wifi_transfer_is_active()) {
                            wifi_transfer_enter(WIFI_TRANSFER_MODE_STA);
                        }
                    } else if (strcmp(line_buf, "exit") == 0 || strcmp(line_buf, "x") == 0) {
                        ESP_LOGI("UART_CMD", "Comando recebido: sair do modo WiFi");
                        if (wifi_transfer_is_active()) {
                            wifi_transfer_request_exit();
                        }
                    } else if (strcmp(line_buf, "status") == 0 || strcmp(line_buf, "s") == 0) {
                        char st[64] = {0};
                        int files = 0;
                        wifi_transfer_get_status(st, sizeof(st), &files);
                        ESP_LOGI("UART_CMD", "WiFi: active=%d status='%s' files=%d | Free RAM: %u PSRAM: %u",
                                 wifi_transfer_is_active(), st, files,
                                 (unsigned)esp_get_free_heap_size(),
                                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
                    } else if (strcmp(line_buf, "play") == 0 || strcmp(line_buf, "p") == 0) {
                        ESP_LOGI("UART_CMD", "Comando recebido: toggle play/pause");
                        audio_player_toggle_play_pause();
                    } else if (strcmp(line_buf, "i2c") == 0) {
                        ESP_LOGI("UART_CMD", "Varrendo barramento I2C (SDA=%d, SCL=%d)...", PIN_OLED_SDA, PIN_OLED_SCL);
                        i2c_master_bus_handle_t bus = u8g2_hal_get_bus_handle();
                        if (!bus) {
                            ESP_LOGE("UART_CMD", "Barramento I2C nao inicializado");
                        } else {
                            int cnt = 0;
                            for (uint16_t addr = 0x01; addr < 0x7F; addr++) {
                                if (u8g2_hal_i2c_probe(addr, 20) == ESP_OK) {
                                    cnt++;
                                    const char *desc = "Desconhecido";
                                    if (addr == 0x3C || addr == 0x3D) desc = "Display OLED SSD1306";
                                    else if (addr == 0x68) desc = "RTC DS3231";
                                    else if (addr >= 0x50 && addr <= 0x57) desc = "EEPROM AT24Cxx";
                                    ESP_LOGI("UART_CMD", "  -> Dispositivo encontrado em 0x%02X (%s)", addr, desc);
                                }
                            }
                            ESP_LOGI("UART_CMD", "Total: %d dispositivo(s) encontrado(s)", cnt);
                        }
                    } else if (strcmp(line_buf, "rtc") == 0) {
                        if (!rtc_ds3231_is_available()) {
                            ESP_LOGW("UART_CMD", "DS3231 nao disponivel no barramento.");
                        } else {
                            struct tm rtc_tm;
                            float temp = 0.0f;
                            if (rtc_ds3231_get_time(&rtc_tm) == ESP_OK) {
                                char rtc_str[32];
                                strftime(rtc_str, sizeof(rtc_str), "%Y-%m-%d %H:%M:%S", &rtc_tm);
                                rtc_ds3231_get_temperature(&temp);
                                time_t sys_now = time(NULL);
                                struct tm sys_tm;
                                localtime_r(&sys_now, &sys_tm);
                                char sys_str[32];
                                strftime(sys_str, sizeof(sys_str), "%Y-%m-%d %H:%M:%S", &sys_tm);
                                ESP_LOGI("UART_CMD", "[RTC] Hardware: %s | Temp: %.2f C | Sistema: %s",
                                         rtc_str, temp, sys_str);
                            } else {
                                ESP_LOGE("UART_CMD", "Erro ao ler DS3231.");
                            }
                        }
                    } else if (strncmp(line_buf, "rtc-set ", 8) == 0) {
                        int y, m, d, hr, mn, sc;
                        if (sscanf(line_buf + 8, "%d-%d-%d %d:%d:%d", &y, &m, &d, &hr, &mn, &sc) == 6) {
                            struct tm new_tm = {
                                .tm_year = y - 1900,
                                .tm_mon  = m - 1,
                                .tm_mday = d,
                                .tm_hour = hr,
                                .tm_min  = mn,
                                .tm_sec  = sc,
                            };
                            esp_err_t set_err = rtc_ds3231_set_time(&new_tm);
                            if (set_err == ESP_OK) {
                                rtc_ds3231_sync_system_time();
                                ESP_LOGI("UART_CMD", "DS3231 e relogio do sistema ajustados com sucesso!");
                            } else {
                                ESP_LOGE("UART_CMD", "Falha ao gravar no DS3231: %s", esp_err_to_name(set_err));
                            }
                        } else {
                            ESP_LOGW("UART_CMD", "Uso: rtc-set YYYY-MM-DD HH:MM:SS (ex: rtc-set 2026-09-24 17:30:00)");
                        }
                    } else if (strcmp(line_buf, "rtc-sync") == 0) {
                        if (rtc_ds3231_sync_system_time() == ESP_OK) {
                            ESP_LOGI("UART_CMD", "Hora do sistema sincronizada com o RTC.");
                        }
                    } else if (strcmp(line_buf, "sync") == 0) {
                        ESP_LOGI("UART_CMD", "Iniciando sincronizacao de podcasts com o servidor...");
                        podcast_sync_start();
                    } else if (strcmp(line_buf, "sync-status") == 0) {
                        podcast_sync_progress_t p;
                        podcast_sync_get_progress(&p);
                        ESP_LOGI("UART_CMD", "[PODCAST] Estado: %d | Progresso: %d/%d (%d%%) | Vel: %.1f KB/s | Msg: %s",
                                 p.state, p.current_idx, p.total_count, p.current_pct, p.speed_kbs, p.status_msg);
                    } else if (strncmp(line_buf, "sync-url ", 9) == 0) {
                        const char *url = line_buf + 9;
                        podcast_sync_set_server_url(url);
                        ESP_LOGI("UART_CMD", "URL do servidor de podcast atualizada para: %s", url);
                    } else if (strcmp(line_buf, "sync-cancel") == 0) {
                        podcast_sync_cancel();
                        ESP_LOGI("UART_CMD", "Cancelamento de sincronizacao solicitado.");
                    } else if (strcmp(line_buf, "freq") == 0) {
                        ESP_LOGI("UART_CMD", "[PWR] Clock atual: %d MHz | Modo: %s",
                                 (int)pwr_governor_get_cpu_freq(),
                                 pwr_governor_is_manual_mode() ? "MANUAL (Bancada)" : "AUTOMATICO (Dinamico)");
                    } else if (strcmp(line_buf, "freq auto") == 0) {
                        pwr_governor_set_manual_mode(false);
                        ESP_LOGI("UART_CMD", "[PWR] Modo automatico ativado.");
                    } else if (strcmp(line_buf, "freq 40") == 0) {
                        pwr_governor_set_manual_mode(true);
                        pwr_governor_set_cpu_freq(PWR_FREQ_40MHZ);
                        ESP_LOGI("UART_CMD", "[PWR] Clock travado manualmente em 40 MHz para teste de bancada.");
                    } else if (strcmp(line_buf, "freq 80") == 0) {
                        pwr_governor_set_manual_mode(true);
                        pwr_governor_set_cpu_freq(PWR_FREQ_80MHZ);
                        ESP_LOGI("UART_CMD", "[PWR] Clock travado manualmente em 80 MHz para teste de bancada.");
                    } else if (strcmp(line_buf, "freq 160") == 0) {
                        pwr_governor_set_manual_mode(true);
                        pwr_governor_set_cpu_freq(PWR_FREQ_160MHZ);
                        ESP_LOGI("UART_CMD", "[PWR] Clock travado manualmente em 160 MHz para teste de bancada.");
                    } else if (strcmp(line_buf, "freq 240") == 0) {
                        pwr_governor_set_manual_mode(true);
                        pwr_governor_set_cpu_freq(PWR_FREQ_240MHZ);
                        ESP_LOGI("UART_CMD", "[PWR] Clock travado manualmente em 240 MHz para teste de bancada.");
                    } else if (strcmp(line_buf, "sleep") == 0) {
                        ESP_LOGI("UART_CMD", "[PWR] Entrando em Light Sleep por 5 segundos (ou ate pressionar joystick)...");
                        pwr_governor_enter_light_sleep(5000);
                        ESP_LOGI("UART_CMD", "[PWR] Acordou do Light Sleep!");
                    } else if (strcmp(line_buf, "deepsleep") == 0) {
                        ESP_LOGI("UART_CMD", "[PWR] Entrando em Deep Sleep (<100 uA)... Pressione JOY_UP ou conecte carregador para acordar.");
                        pwr_governor_enter_deep_sleep();
                    }
                    line_len = 0;
                }
            } else if (line_len < (int)sizeof(line_buf) - 1) {
                line_buf[line_len++] = (char)c;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

static void on_deep_sleep_prepare(void)
{
    ESP_LOGI("PWR", "Callback de Deep Sleep: desligando display OLED (Display OFF)...");
    oled_display_blank();
    oled_display_set_power_save(true);
}

void app_main(void)
{
    // Desativa hold de pinos remanescente de ciclos anteriores de sleep
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis((gpio_num_t)PIN_OLED_SDA);
    gpio_hold_dis((gpio_num_t)PIN_OLED_SCL);
    gpio_hold_dis((gpio_num_t)PIN_SD_CMD);
    gpio_hold_dis((gpio_num_t)PIN_SD_CLK);
    gpio_hold_dis((gpio_num_t)PIN_SD_D0);
    gpio_hold_dis((gpio_num_t)PIN_SD_D1);
    gpio_hold_dis((gpio_num_t)PIN_SD_D2);
    gpio_hold_dis((gpio_num_t)PIN_SD_D3);
    gpio_hold_dis((gpio_num_t)PIN_BT_LINK_EN);
    gpio_hold_dis((gpio_num_t)PIN_BT_LINK_UART_TX);
    gpio_hold_dis((gpio_num_t)PIN_BT_LINK_UART_RX);
    gpio_hold_dis((gpio_num_t)PIN_JOY_UP);
    gpio_hold_dis((gpio_num_t)PIN_BATTERY_CHRG);
    gpio_force_unhold_all();

    ESP_LOGI(TAG, "Iniciando mps3...");

    // Inicializa o subsistema de gerenciamento de clock e energia (DFS)
    ESP_LOGI(TAG, "Inicializando governador dinamico de energia...");
    pwr_governor_init();
    pwr_governor_set_sleep_cb(on_deep_sleep_prepare);

    // Boilerplate padrao do ESP-IDF: a particao NVS pode precisar ser
    // reformatada apos mudanca de tamanho de flash/versao - usada aqui
    // pra lembrar a ultima faixa/posicao tocada entre religadas.
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_flash_init());

    esp_err_t ret;

    ret = oled_display_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar o display OLED (codigo %d). Continuando sem display.", ret);
    }

    // --- Varredura de Barramento I2C e Deteccao de RTC DS3231 ---
    i2c_master_bus_handle_t i2c_bus = u8g2_hal_get_bus_handle();
    if (i2c_bus) {
        ESP_LOGI(TAG, "Executando varredura no barramento I2C (SDA=%d, SCL=%d)...", PIN_OLED_SDA, PIN_OLED_SCL);
        int found_devices = 0;
        for (uint16_t addr = 0x01; addr < 0x7F; addr++) {
            if (u8g2_hal_i2c_probe(addr, 20) == ESP_OK) {
                found_devices++;
                const char *desc = "Dispositivo Desconhecido";
                if (addr == 0x3C || addr == 0x3D) desc = "Display OLED SSD1306";
                else if (addr == 0x68) desc = "RTC DS3231";
                else if (addr >= 0x50 && addr <= 0x57) desc = "EEPROM AT24Cxx";
                ESP_LOGI(TAG, "  -> Dispositivo I2C encontrado em 0x%02X (%s)", addr, desc);
            }
        }
        ESP_LOGI(TAG, "Varredura I2C concluida: %d dispositivo(s) detectado(s).", found_devices);

        // Inicializa o RTC DS3231 se detectado
        if (rtc_ds3231_detect(i2c_bus)) {
            if (rtc_ds3231_init(i2c_bus) == ESP_OK) {
                struct tm now_tm;
                float temp = 0.0f;
                if (rtc_ds3231_get_time(&now_tm) == ESP_OK) {
                    char time_str[32];
                    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &now_tm);
                    rtc_ds3231_get_temperature(&temp);
                    ESP_LOGI(TAG, "[RTC DS3231] Data/Hora: %s | Temp: %.2f C", time_str, temp);
                    rtc_ds3231_sync_system_time();
                }
            }
        } else {
            ESP_LOGW(TAG, "[RTC DS3231] Nao detectado no endereco 0x%02X.", DS3231_I2C_ADDR);
        }
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

    ESP_LOGI(TAG, "Iniciando touch_input...");
    ret = touch_input_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar os controles do joystick (codigo %d). Continuando sem controles.", ret);
    }

    ESP_LOGI(TAG, "Iniciando usb_manager...");
    ret = usb_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar o USB Manager");
    }

    // audio_player_start() ANTES da display_task: cria s_state_mutex e
    // s_scan_mutex que a display_task acessa imediatamente ao iniciar
    // (via audio_player_get_state, audio_player_browse_is_root, etc.).
    if (s_sd_ok) {
        ret = audio_player_start(MUSIC_DIR);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao iniciar a task de reproducao.");
        }
    }

    podcast_sync_init();

    // Configura wakeup por hardware ao plugar o carregador (GPIO 11 em nivel alto)
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BATTERY_CHRG, 1);

    // display_task no Core 0 com prioridade 3:
    // Nao disputa com o decodificador/IO do player_task (Core 1),
    // e cede CPU instantaneamente para audio_dsp_task (Core 0, prio 5)
    // e touch_task (Core 0, prio 5).
    TaskHandle_t display_handle = NULL;
    ESP_LOGI(TAG, "Criando display_task no Core 0 com prioridade 3...");
    xTaskCreatePinnedToCore(display_task, "display_task", 8192, NULL, 3, &display_handle, 0);
    touch_input_set_display_task_handle(display_handle);

    ESP_LOGI(TAG, "Criando uart_cmd_task no Core 0...");
    xTaskCreatePinnedToCore(uart_cmd_task, "uart_cmd", 4096, NULL, 1, NULL, 0);

    // --- Autovalidacao de firmware e cancelamento de rollback OTA ---
    const esp_partition_t *running_part = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running_part, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI("OTA", "Novo firmware em verificacao. Validando e cancelando rollback...");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }

    // Se reiniciou apos uma atualizacao OTA, religa o Wi-Fi para confirmacao remota imediata
    nvs_handle_t nvs_h;
    uint8_t ota_reboot = 0;
    if (nvs_open("system", NVS_READWRITE, &nvs_h) == ESP_OK) {
        if (nvs_get_u8(nvs_h, "ota_reboot", &ota_reboot) == ESP_OK && ota_reboot == 1) {
            nvs_erase_key(nvs_h, "ota_reboot");
            nvs_commit(nvs_h);
            ESP_LOGI("OTA", "Reboot pos-OTA detectado. Ativando Wi-Fi para reconexao...");
            wifi_transfer_enter_auto();
        }
        nvs_close(nvs_h);
    }

    ESP_LOGI(TAG, "mps3 rodando.");
}

