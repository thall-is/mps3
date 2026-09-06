#include "usb_manager.h"
#include "usb_descriptors.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tusb.h"
#include "tinyusb_msc.h"

#include "i2s_output.h"
#include "touch_input.h"
#include "sd_card.h"
#include "audio_player.h"
#include "class/audio/audio_device.h"
#include "class/hid/hid_device.h"

static const char *TAG = "usb_manager";
static volatile usb_mode_t s_current_mode = USB_MODE_NONE;
static volatile bool s_connected = false;
static tinyusb_msc_storage_handle_t s_msc_handle = NULL;

// -----------------------------------------------------------------------------
// EVENTOS TINYUSB (Wrapper do esp_tinyusb)
// -----------------------------------------------------------------------------
static void tinyusb_event_callback(tinyusb_event_t *event, void *arg)
{
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        ESP_LOGI(TAG, "Cabo USB Conectado!");
        s_connected = true;
        break;

    case TINYUSB_EVENT_DETACHED:
        ESP_LOGI(TAG, "Cabo USB Desconectado.");
        s_connected = false;
        if (s_current_mode != USB_MODE_NONE) {
            usb_manager_set_mode(USB_MODE_NONE);
        }
        touch_input_cancel_usb();
        break;

    default:
        break;
    }
}

// -----------------------------------------------------------------------------
// AUDIO TASK (DAC)
// -----------------------------------------------------------------------------
static void usb_audio_task(void *arg)
{
    while (1) {
        if (s_current_mode == USB_MODE_DAC && s_connected) {
            uint8_t buf[192]; // up to 48kHz * 2 ch * 2 bytes = 192 bytes per ms
            uint32_t read = tud_audio_read(buf, sizeof(buf));
            if (read > 0) {
                // Converter 16-bit (2 bytes) Little Endian para 32-bit (I2S MSB)
                size_t num_samples = read / 2;
                int32_t samples32[num_samples];
                for (size_t i = 0; i < num_samples; i++) {
                    uint8_t b0 = buf[i * 2 + 0];
                    uint8_t b1 = buf[i * 2 + 1];
                    int16_t s16 = (int16_t)((b0) | (b1 << 8));
                    samples32[i] = (int32_t)s16 << 16; // Shift to 32-bit MSB
                }
                size_t written = 0;
                i2s_output_write(samples32, num_samples, &written);
            } else {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

// -----------------------------------------------------------------------------
// INICIALIZACAO
// -----------------------------------------------------------------------------
esp_err_t usb_manager_init(void)
{
    ESP_LOGI(TAG, "Iniciando usb_manager (Composite UAC2 Audio + MSC + HID)");

    const tinyusb_config_t tusb_cfg = {
        .descriptor = {
            .device = &desc_device,
            .string = usb_manager_string_desc_arr,
            .string_count = usb_manager_string_desc_count,
            .full_speed_config = desc_configuration,
            .high_speed_config = desc_configuration,
        },
        .phy = {
            .skip_setup = false,
            .self_powered = false, // Ignorar VBUS externo, forcar PULL-UP D+ imediato!
        },
        .task = {
            .priority = 5,
        },
        .event_cb = tinyusb_event_callback,
    };

    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao instalar o TinyUSB (err=%d)", err);
        return err;
    }

    // Instala driver MSC com auto_mount_off para termos controle total manual
    const tinyusb_msc_driver_config_t msc_driver_cfg = {
        .user_flags = {
            .auto_mount_off = 1,
        },
    };
    err = tinyusb_msc_install_driver(&msc_driver_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao instalar driver MSC (err=%d)", err);
    }

    xTaskCreate(usb_audio_task, "usb_audio", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "TinyUSB e usb_audio task iniciados com sucesso");

    return ESP_OK;
}

// -----------------------------------------------------------------------------
// CONTROLE DE MODO (Prompt, MSC, DAC)
// -----------------------------------------------------------------------------
void usb_manager_set_mode(usb_mode_t mode)
{
    if (s_current_mode == mode) return;

    usb_mode_t prev_mode = s_current_mode;
    s_current_mode = mode;
    ESP_LOGI(TAG, "Modo USB alterado de %d para %d", prev_mode, mode);

    // Inicializa o storage MSC para o SD se ainda nao foi criado
    if (!s_msc_handle) {
        sdmmc_card_t *card = sd_card_get_handle();
        if (card) {
            const tinyusb_msc_storage_config_t storage_cfg = {
                .medium = {
                    .card = card,
                },
                .fat_fs = {
                    .base_path = SD_MOUNT_POINT,
                    .do_not_format = true,
                    .config = {
                        .max_files = 5,
                    },
                },
                .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
            };
            esp_err_t err = tinyusb_msc_new_storage_sdmmc(&storage_cfg, &s_msc_handle);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Storage SDMMC registrado no MSC");
            } else {
                ESP_LOGE(TAG, "Falha ao registrar SDMMC no MSC (err=%d)", err);
            }
        }
    }

    if (prev_mode == USB_MODE_MSC && mode != USB_MODE_MSC) {
        // 1. Remonta o storage no ESP32 (/sdcard)
        if (s_msc_handle) {
            tinyusb_msc_set_storage_mount_point(s_msc_handle, TINYUSB_MSC_STORAGE_MOUNT_APP);
            ESP_LOGI(TAG, "Storage remontado no ESP32 (/sdcard)");
        }
        // 2. Notifica o audio player para reescanear e destravar a task
        audio_player_reacquire_sd_after_usb();
    }

    if (mode == USB_MODE_MSC) {
        // 1. Pausa o player se estiver tocando e fecha arquivos abertos
        audio_player_release_sd_for_usb();

        // 2. Entrega o storage exclusivamente para o host USB
        if (s_msc_handle) {
            tinyusb_msc_set_storage_mount_point(s_msc_handle, TINYUSB_MSC_STORAGE_MOUNT_USB);
            ESP_LOGI(TAG, "Storage montado no Host USB (Pendrive)");
        }
    } else if (mode == USB_MODE_DAC) {
        // Modo DAC: pausa o player de audio local
        playback_state_t st;
        audio_player_get_state(&st);
        if (st.playing) {
            audio_player_toggle_play_pause();
        }
        i2s_output_set_rate(48000);
    }
}

bool usb_manager_is_connected(void)
{
    return s_connected;
}

// -----------------------------------------------------------------------------
// AUDIO CLASS CALLBACKS & ENTITY REQUESTS (UAC2)
// -----------------------------------------------------------------------------
static uint32_t s_sample_rate = 48000;
static int16_t s_volume[3] = { 0, 0, 0 }; // master, ch1, ch2 (0 dB)
static uint8_t s_mute[3] = { 0, 0, 0 };

bool tud_audio_rx_done_pre_read_cb(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt_setting)
{
    (void)rhport; (void)func_id; (void)ep_out; (void)cur_alt_setting; (void)n_bytes_received;
    return true;
}

bool tud_audio_rx_done_post_read_cb(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt_setting)
{
    (void)rhport; (void)func_id; (void)ep_out; (void)cur_alt_setting; (void)n_bytes_received;
    return true;
}

/* tud_audio_feedback_params_cb REMOVED (No feedback EP) */

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport;
    uint8_t const itf = tu_u16_low(p_request->wIndex);
    uint8_t const alt = tu_u16_low(p_request->wValue);
    ESP_LOGI(TAG, "UAC2 Set Interface %d Alt %d", itf, alt);
    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport; (void)p_request;
    return true;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    uint8_t const entity_id = TU_U16_HIGH(p_request->wIndex);
    uint8_t const ctrl_sel   = TU_U16_HIGH(p_request->wValue);
    uint8_t const ch         = TU_U16_LOW(p_request->wValue);

    if (entity_id == UAC2_ENTITY_CLOCK) {
        if (ctrl_sel == AUDIO20_CS_CTRL_SAM_FREQ) {
            if (p_request->bRequest == AUDIO20_CS_REQ_CUR) {
                audio20_control_cur_4_t curf = { .bCur = (int32_t)tu_htole32(s_sample_rate) };
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &curf, sizeof(curf));
            } else if (p_request->bRequest == AUDIO20_CS_REQ_RANGE) {
                audio20_control_range_4_n_t(1) rangef = {
                    .wNumSubRanges = tu_htole16(1),
                    .subrange[0] = {
                        .bMin = (int32_t)tu_htole32(48000),
                        .bMax = (int32_t)tu_htole32(48000),
                        .bRes = 0
                    }
                };
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &rangef, sizeof(rangef));
            }
        } else if (ctrl_sel == AUDIO20_CS_CTRL_CLK_VALID && p_request->bRequest == AUDIO20_CS_REQ_CUR) {
            audio20_control_cur_1_t cur_valid = { .bCur = 1 };
            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &cur_valid, sizeof(cur_valid));
        }
    } else if (entity_id == UAC2_ENTITY_FEATURE_UNIT) {
        if (ctrl_sel == AUDIO20_FU_CTRL_MUTE && p_request->bRequest == AUDIO20_CS_REQ_CUR) {
            uint8_t ch_idx = (ch < 3) ? ch : 0;
            audio20_control_cur_1_t cur_mute = { .bCur = s_mute[ch_idx] };
            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &cur_mute, sizeof(cur_mute));
        } else if (ctrl_sel == AUDIO20_FU_CTRL_VOLUME) {
            if (p_request->bRequest == AUDIO20_CS_REQ_RANGE) {
                audio20_control_range_2_n_t(1) range_vol = {
                    .wNumSubRanges = tu_htole16(1),
                    .subrange[0] = {
                        .bMin = tu_htole16(-12800), // -50 dB
                        .bMax = tu_htole16(0),      // 0 dB
                        .bRes = tu_htole16(256)     // 1 dB step
                    }
                };
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &range_vol, sizeof(range_vol));
            } else if (p_request->bRequest == AUDIO20_CS_REQ_CUR) {
                uint8_t ch_idx = (ch < 3) ? ch : 0;
                audio20_control_cur_2_t cur_vol = { .bCur = tu_htole16(s_volume[ch_idx]) };
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &cur_vol, sizeof(cur_vol));
            }
        }
    }
    return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf)
{
    (void)rhport;
    uint8_t const entity_id = TU_U16_HIGH(p_request->wIndex);
    uint8_t const ctrl_sel   = TU_U16_HIGH(p_request->wValue);
    uint8_t const ch         = TU_U16_LOW(p_request->wValue);

    if (p_request->bRequest == AUDIO20_CS_REQ_CUR) {
        if (entity_id == UAC2_ENTITY_CLOCK && ctrl_sel == AUDIO20_CS_CTRL_SAM_FREQ) {
            audio20_control_cur_4_t const *cur = (audio20_control_cur_4_t const *)buf;
            s_sample_rate = (uint32_t)cur->bCur;
            ESP_LOGI(TAG, "UAC2 Set Sample Rate: %lu Hz", s_sample_rate);
            return true;
        } else if (entity_id == UAC2_ENTITY_FEATURE_UNIT) {
            if (ctrl_sel == AUDIO20_FU_CTRL_MUTE) {
                uint8_t ch_idx = (ch < 3) ? ch : 0;
                s_mute[ch_idx] = buf[0];
                return true;
            } else if (ctrl_sel == AUDIO20_FU_CTRL_VOLUME) {
                uint8_t ch_idx = (ch < 3) ? ch : 0;
                s_volume[ch_idx] = (int16_t)(buf[0] | (buf[1] << 8));
                return true;
            }
        }
    }
    return false;
}

// -----------------------------------------------------------------------------
// HID CLASS
// -----------------------------------------------------------------------------
void usb_manager_send_hid(int command)
{
    if (!s_connected || s_current_mode != USB_MODE_DAC) return;
    
    uint16_t key = 0;
    switch(command) {
        case 0: key = HID_USAGE_CONSUMER_PLAY_PAUSE; break;
        case 1: key = HID_USAGE_CONSUMER_SCAN_NEXT_TRACK; break;
        case 2: key = HID_USAGE_CONSUMER_SCAN_PREVIOUS_TRACK; break;
        case 3: key = HID_USAGE_CONSUMER_VOLUME_INCREMENT; break;
        case 4: key = HID_USAGE_CONSUMER_VOLUME_DECREMENT; break;
        default: return;
    }

    tud_hid_report(0, &key, 2);
    vTaskDelay(pdMS_TO_TICKS(10));
    key = 0;
    tud_hid_report(0, &key, 2);
}

uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen)
{
    (void) itf; (void) report_id; (void) report_type; (void) buffer; (void) reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize)
{
    (void) itf; (void) report_id; (void) report_type; (void) buffer; (void) bufsize;
}
