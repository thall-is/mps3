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
#include "eq.h"
#include "class/audio/audio_device.h"
#include "class/hid/hid_device.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "tinyusb_cdc_acm.h"
#include <math.h>
#include <string.h>

static const char *TAG = "usb_manager";
static volatile usb_mode_t s_current_mode = USB_MODE_NONE;
static volatile bool s_connected = false;
static volatile bool s_switching_mode = false;
static tinyusb_msc_storage_handle_t s_msc_handle = NULL;
static uint32_t s_sample_rate = 48000;
static int16_t s_volume[3] = { 0, 0, 0 }; // master, ch1, ch2 (0 dB em 1/256 dB)
static uint8_t s_mute[3] = { 0, 0, 0 };
static volatile uint32_t s_usb_pkt_count = 0;
static volatile int32_t s_usb_last_sample = 0;
static volatile int64_t s_last_packet_time_us = 0;

uint32_t usb_manager_get_sample_rate(void)
{
    return s_sample_rate;
}

uint32_t usb_manager_get_pkt_count(void)
{
    return s_usb_pkt_count;
}

int32_t usb_manager_get_last_sample(void)
{
    return s_usb_last_sample;
}

bool usb_manager_is_streaming(void)
{
    if (s_current_mode != USB_MODE_DAC) {
        return false;
    }
    if (s_last_packet_time_us == 0) {
        return false;
    }
    int64_t diff = esp_timer_get_time() - s_last_packet_time_us;
    return (diff >= 0 && diff <= 250000); // 250 ms timeout
}

// Forca reboot imediato no modo de download (ROM bootloader para gravacao via esptool)
void usb_manager_enter_bootloader(void)
{
    ESP_LOGI(TAG, "Entrando no modo DOWNLOAD BOOTLOADER da ROM via RTC_CNTL_FORCE_DOWNLOAD_BOOT...");
    REG_SET_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

static void cdc_line_state_changed_cb(int itf, cdcacm_event_t *event)
{
    if (!event->line_state_changed_data.dtr && event->line_state_changed_data.rts) {
        ESP_LOGI(TAG, "esptool detectado via DTR/RTS! Reiniciando em modo BOOTLOADER...");
        usb_manager_enter_bootloader();
    }
}

static void cdc_line_coding_changed_cb(int itf, cdcacm_event_t *event)
{
    if (event->line_coding_changed_data.p_line_coding->bit_rate == 1200) {
        ESP_LOGI(TAG, "esptool detectado via 1200 bps touch! Reiniciando em modo BOOTLOADER...");
        usb_manager_enter_bootloader();
    }
}

// Declaracao da API do esp_tinyusb para troca dinamica de descritores
extern esp_err_t tinyusb_descriptors_set(tinyusb_port_t port, const tinyusb_desc_config_t *config);

// -----------------------------------------------------------------------------
// EVENTOS TINYUSB (Wrapper do esp_tinyusb)
// -----------------------------------------------------------------------------
static void tinyusb_event_callback(tinyusb_event_t *event, void *arg)
{
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        ESP_LOGI(TAG, "Cabo USB Conectado ao PC!");
        s_connected = true;
        if (s_current_mode == USB_MODE_NONE) {
            ESP_LOGI(TAG, "Exibindo tela de selecao de modo USB");
            touch_input_set_usb_prompt();
        }
        break;

    case TINYUSB_EVENT_DETACHED:
        ESP_LOGI(TAG, "Cabo USB Desconectado.");
        s_connected = false;
        if (!s_switching_mode) {
            if (s_current_mode != USB_MODE_NONE) {
                usb_manager_set_mode(USB_MODE_NONE);
            }
            touch_input_cancel_usb();
        }
        break;

    default:
        break;
    }
}

// Callbacks de gerenciamento de energia USB (Selective Suspend do Windows/Host)
void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    ESP_LOGD(TAG, "USB Suspend (idle do Host)");
}

void tud_resume_cb(void)
{
    ESP_LOGD(TAG, "USB Resume (Host ativo)");
}

// -----------------------------------------------------------------------------
// AUDIO TASK (DAC de Mesa com Balanco e EQ de 10 Bandas)
static TaskHandle_t s_audio_task_handle = NULL;

// -----------------------------------------------------------------------------
// AUDIO TASK (DAC de Mesa com Pre-roll DMA, Balanco e EQ 10 Bandas)
// -----------------------------------------------------------------------------
static void usb_audio_task(void *arg)
{
    static uint8_t usb_rx_buf[1024] __attribute__((aligned(4)));
    static int32_t i2s_buf[512];
    uint32_t pkt_count = 0;

    while (1) {
        if (s_current_mode == USB_MODE_DAC) {
            // Aguarda notificacao da ISR USB (ou timeout de 5ms com HZ=1000)
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));

            uint32_t read = 0;

            while ((read = tud_audio_read(usb_rx_buf, sizeof(usb_rx_buf))) > 0) {
                s_last_packet_time_us = esp_timer_get_time();
                pkt_count++;

                if (pkt_count == 1 || pkt_count % 1000 == 0) {
                    ESP_LOGI(TAG, "DAC Streaming ativo: pacote #%lu (%u bytes lidos do USB)", (unsigned long)pkt_count, (unsigned)read);
                }

                // Garante alinhamento estrito em frames estéreo de 8 bytes (4 bytes L + 4 bytes R)
                read = read - (read % 8);
                if (read == 0) continue;

                // 1. Ler amostras nativas de 24-bit em subslot de 32 bits (alinhadas ao MSB)
                size_t num_samples = read / 4;
                if (num_samples > 512) num_samples = 512;
                num_samples = num_samples - (num_samples % 2);

                const int32_t *src32 = (const int32_t *)usb_rx_buf;
                for (size_t i = 0; i < num_samples; i++) {
                    i2s_buf[i] = src32[i];
                }
                s_usb_pkt_count = pkt_count;
                if (num_samples > 0) s_usb_last_sample = i2s_buf[0];

                // 2. Aplicar Mute, Volume (DAP e UAC2 Host) e Balanco L/R
                bool mute_L = (s_mute[0] != 0) || (s_mute[1] != 0);
                bool mute_R = (s_mute[0] != 0) || (s_mute[2] != 0);
                int vol = audio_player_get_volume(); // 0 a 100%

                if ((mute_L && mute_R) || vol <= 0) {
                    memset(i2s_buf, 0, num_samples * sizeof(int32_t));
                } else {
                    // Ganho de volume do DAP: 0 a 100% (1.0f em 100% para nivel padrao de linha e bit-perfect).
                    // Como tud_audio_set_req_entity_cb mapeia s_volume do Host para audio_player_set_volume(),
                    // dap_gain atua como o ganho master unico (evita atenuacao duplicada ao quadrado).
                    float dap_gain = (vol >= 100) ? 1.0f : (vol <= 0 ? 0.0f : powf(10.0f, (-50.0f * (1.0f - (float)vol / 100.0f)) / 20.0f));

                    // Balanco L/R configurado no DAP (-100 a +100)
                    int bal = audio_player_get_balance();
                    float bal_L = (bal > 0) ? ((float)(100 - bal) / 100.0f) : 1.0f;
                    float bal_R = (bal < 0) ? ((float)(100 + bal) / 100.0f) : 1.0f;

                    // Balanco L/R eventual do Host (se o mixer do Host tiver canais L e R dessincronizados)
                    float host_bal_L = 1.0f;
                    float host_bal_R = 1.0f;
                    if (s_volume[1] < s_volume[2]) {
                        host_bal_L = powf(10.0f, ((float)(s_volume[1] - s_volume[2]) / 256.0f) / 20.0f);
                    } else if (s_volume[2] < s_volume[1]) {
                        host_bal_R = powf(10.0f, ((float)(s_volume[2] - s_volume[1]) / 256.0f) / 20.0f);
                    }

                    float gL = mute_L ? 0.0f : (dap_gain * bal_L * host_bal_L);
                    float gR = mute_R ? 0.0f : (dap_gain * bal_R * host_bal_R);

                    // Multiplica apenas se houver atenuacao para preservar fidelidade bit-perfect
                    if (gL < 0.9999f || gR < 0.9999f) {
                        int32_t multL = (int32_t)(gL * 32768.0f);
                        int32_t multR = (int32_t)(gR * 32768.0f);
                        if (multL < 0) multL = 0;
                        if (multR < 0) multR = 0;
                        for (size_t i = 0; i < num_samples; i += 2) {
                            i2s_buf[i] = (int32_t)(((int64_t)i2s_buf[i] * multL) >> 15);
                            if (i + 1 < num_samples) {
                                i2s_buf[i + 1] = (int32_t)(((int64_t)i2s_buf[i + 1] * multR) >> 15);
                            }
                        }
                    }
                }

                // 3. Aplicar Equalizador de 10 bandas
                eq_process(i2s_buf, num_samples);

                // 4. Escrever diretamente nos descritores DMA do I2S
                size_t written = 0;
                i2s_output_write(i2s_buf, num_samples, &written);
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
    ESP_LOGI(TAG, "Iniciando usb_manager (Modos Exclusivos: CDC 0x4000 / MSC 0x4002 / DAC 0x4006)");

    // Habilita Double Buffering de hardware no controlador Synopsys DWC2 para os endpoints Bulk (MSC / CDC)
    const tud_configure_dwc2_t dwc2_cfg = {
        .bm_double_buffered = (1 << 1) | (1 << 2), // EP1 (MSC IN) e EP2 (CDC IN)
        .vbus_sensing = false,
    };
    tud_configure(0, TUD_CFGID_DWC2, &dwc2_cfg);

    const tinyusb_config_t tusb_cfg = {
        .descriptor = {
            .device = &desc_device_cdc,
            .string = usb_manager_cdc_string_desc_arr,
            .string_count = usb_manager_cdc_string_desc_count,
            .full_speed_config = desc_configuration_cdc,
            .high_speed_config = desc_configuration_cdc,
        },
        .phy = {
            .skip_setup = false,
            .self_powered = false, // Ignorar VBUS externo, forcar PULL-UP D+ imediato!
        },
        .task = {
            .size = 12288,
            .priority = 7,
            .xCoreID = 0,
        },
        .event_cb = tinyusb_event_callback,
    };

    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao instalar o TinyUSB (err=%d)", err);
        return err;
    }

    // Instala driver CDC ACM para porta COM de logs e auto-reset pelo esptool
    const tinyusb_config_cdcacm_t cdc_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = NULL,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = cdc_line_state_changed_cb,
        .callback_line_coding_changed = cdc_line_coding_changed_cb,
    };
    err = tinyusb_cdcacm_init(&cdc_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Driver CDC ACM instalado com auto-reset de flash ativado");
    } else {
        ESP_LOGW(TAG, "Aviso: Falha ao registrar CDC ACM (err=%d)", err);
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

    // Task de audio no Core 0 com prioridade 6
    xTaskCreatePinnedToCore(usb_audio_task, "usb_audio", 4096, NULL, 6, &s_audio_task_handle, 0);
    ESP_LOGI(TAG, "TinyUSB (Core 0, prio 7) e usb_audio (Core 0, prio 6) iniciados com sucesso");

    return ESP_OK;
}

// -----------------------------------------------------------------------------
// CONTROLE DE MODO EXCLUSIVO (Standby/CDC, MSC, DAC)
// -----------------------------------------------------------------------------
void usb_manager_set_mode(usb_mode_t mode)
{
    if (s_current_mode == mode) return;

    usb_mode_t prev_mode = s_current_mode;
    s_switching_mode = true;
    s_current_mode = mode;
    ESP_LOGI(TAG, "Modo USB alterado de %d para %d", prev_mode, mode);

    // 1. Sempre desconecta do Host antes de qualquer troca de modo para re-enumeracao limpa
    ESP_LOGI(TAG, "Desconectando USB do Host para troca de modo exclusivo...");
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(150));

    // --- SAIDA DE MODOS EXCLUSIVOS ---
    if (prev_mode == USB_MODE_MSC && mode != USB_MODE_MSC) {
        ESP_LOGI(TAG, "Saindo do MODO EXCLUSIVO: USB Storage. Restaurando sistema...");
        // Desvincula o storage do TinyUSB MSC com seguranca
        if (s_msc_handle) {
            tinyusb_msc_delete_storage(s_msc_handle);
            s_msc_handle = NULL;
            ESP_LOGI(TAG, "Storage MSC desvinculado");
        }
        // Desmonta o VFS antigo e remonta o FatFS limpo no SDMMC nativo
        sd_card_deinit();
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_err_t remount_err = sd_card_init();
        if (remount_err != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao remontar SD Card apos USB (err=%d)", remount_err);
        } else {
            ESP_LOGI(TAG, "SD Card remontado com sucesso em %s", SD_MOUNT_POINT);
        }
        // Notifica o audio player para reescanear o cartao
        audio_player_reacquire_sd_after_usb();
    } else if (prev_mode == USB_MODE_DAC && mode != USB_MODE_DAC) {
        ESP_LOGI(TAG, "Saindo do MODO EXCLUSIVO: USB DAC. Retomando player local...");
        s_usb_pkt_count = 0;
        s_last_packet_time_us = 0;
        tud_audio_clear_ep_out_ff();
        i2s_output_enable();
        audio_player_reacquire_sd_after_usb();
    }

    // --- ENTRADA EM MODOS EXCLUSIVOS ---
    if (mode == USB_MODE_NONE) {
        ESP_LOGI(TAG, "Entrando no MODO EXCLUSIVO: USB Serial / Flash (PID 0x4000)");
        // Aplica descritores de CDC Puro (Porta COM serial para flash e logs)
        const tinyusb_desc_config_t cdc_desc_cfg = {
            .device = &desc_device_cdc,
            .string = usb_manager_cdc_string_desc_arr,
            .string_count = usb_manager_cdc_string_desc_count,
            .full_speed_config = desc_configuration_cdc,
            .high_speed_config = desc_configuration_cdc,
        };
        tinyusb_descriptors_set(TINYUSB_PORT_FULL_SPEED_0, &cdc_desc_cfg);
        vTaskDelay(pdMS_TO_TICKS(100));
        tud_connect();
        ESP_LOGI(TAG, "USB Serial CDC Puro conectado ao Host");

    } else if (mode == USB_MODE_MSC) {
        ESP_LOGI(TAG, "Entrando no MODO EXCLUSIVO: USB Storage Puro (Pendrive - PID 0x4002)");
        // 1. Pausa o player e fecha arquivos abertos
        audio_player_release_sd_for_usb();
        i2s_output_disable();

        // 2. Configura descritores de Armazenamento Puro (sem UAC2, sem HID, sem CDC)
        const tinyusb_desc_config_t msc_desc_cfg = {
            .device = &desc_device_msc,
            .string = usb_manager_msc_string_desc_arr,
            .string_count = usb_manager_msc_string_desc_count,
            .full_speed_config = desc_configuration_msc,
            .high_speed_config = desc_configuration_msc,
        };
        tinyusb_descriptors_set(TINYUSB_PORT_FULL_SPEED_0, &msc_desc_cfg);

        // 3. Entrega o storage exclusivamente para o host USB com buffer de 32 KB
        sdmmc_card_t *card = sd_card_get_handle();
        if (card && !s_msc_handle) {
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
                .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB, // Direto pro USB sem tocar no FatFS local!
            };
            esp_err_t s_err = tinyusb_msc_new_storage_sdmmc(&storage_cfg, &s_msc_handle);
            if (s_err == ESP_OK) {
                ESP_LOGI(TAG, "Storage SDMMC registrado exclusivamente no USB Host");
            } else {
                ESP_LOGE(TAG, "Falha ao registrar SDMMC no MSC (err=%d)", s_err);
            }
        }

        // 4. Reconecta ao Host
        vTaskDelay(pdMS_TO_TICKS(100));
        tud_connect();
        ESP_LOGI(TAG, "USB MSC Puro re-enumerado no Host");

    } else if (mode == USB_MODE_DAC) {
        ESP_LOGI(TAG, "Entrando no MODO EXCLUSIVO: USB DAC Puro (PID 0x4006 com EQ e Balanco)");
        s_usb_pkt_count = 0;
        s_last_packet_time_us = 0;
        // 1. Garante que o player_task liberou o SD e esta em espera passiva
        audio_player_release_sd_for_usb();
        tud_audio_clear_ep_out_ff();
        // 2. Reconfigura o clock I2S e EQ para 48.000 Hz
        s_sample_rate = 48000;
        i2s_output_set_rate(48000);
        i2s_output_enable();
        eq_set_sample_rate(48000);

        // 3. Toca bipe duplo de 880 Hz no PCM5102A para confirmar fone e DAC 100% audiveis
        ESP_LOGI(TAG, "Tocando bipe duplo de 880 Hz no PCM5102A...");
        i2s_output_play_test_tone(880, 150);
        vTaskDelay(pdMS_TO_TICKS(50));
        i2s_output_play_test_tone(880, 150);

        // 4. Configura descritores de DAC Puro + HID (sem MSC, sem CDC)
        const tinyusb_desc_config_t dac_desc_cfg = {
            .device = &desc_device_dac,
            .string = usb_manager_dac_string_desc_arr,
            .string_count = usb_manager_dac_string_desc_count,
            .full_speed_config = desc_configuration_dac,
            .high_speed_config = desc_configuration_dac,
        };
        tinyusb_descriptors_set(TINYUSB_PORT_FULL_SPEED_0, &dac_desc_cfg);

        // 5. Reconecta ao Host
        vTaskDelay(pdMS_TO_TICKS(100));
        tud_connect();
        ESP_LOGI(TAG, "USB DAC Puro re-enumerado no Host");
    }

    s_switching_mode = false;
}

bool usb_manager_is_connected(void)
{
    return s_connected || tud_mounted();
}

// -----------------------------------------------------------------------------
// AUDIO CLASS CALLBACKS & ENTITY REQUESTS (UAC2)
// -----------------------------------------------------------------------------

bool tud_audio_rx_done_isr(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt_setting)
{
    (void)rhport; (void)n_bytes_received; (void)func_id; (void)ep_out; (void)cur_alt_setting;
    if (s_audio_task_handle != NULL) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        vTaskNotifyGiveFromISR(s_audio_task_handle, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
    return true;
}

#if CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP
void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf, audio_feedback_params_t *feedback_param)
{
    (void)func_id;
    (void)alt_itf;
    feedback_param->method = AUDIO_FEEDBACK_METHOD_FIFO_COUNT;
    feedback_param->sample_freq = s_sample_rate;
    feedback_param->fifo_count.fifo_threshold = (s_sample_rate * 2 * 4 / 1000) * 2;
}
#endif

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport;
    uint8_t const itf = tu_u16_low(p_request->wIndex);
    uint8_t const alt = tu_u16_low(p_request->wValue);
    ESP_LOGI(TAG, "UAC2 Set Interface %d Alt %d", itf, alt);
    if (alt == 0) {
        s_last_packet_time_us = 0;
    }
    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport; (void)p_request;
    s_last_packet_time_us = 0;
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
                audio20_control_range_4_n_t(2) rangef = {
                    .wNumSubRanges = tu_htole16(2),
                    .subrange = {
                        { .bMin = (int32_t)tu_htole32(44100), .bMax = (int32_t)tu_htole32(44100), .bRes = 0 },
                        { .bMin = (int32_t)tu_htole32(48000), .bMax = (int32_t)tu_htole32(48000), .bRes = 0 },
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
                int vol = audio_player_get_volume();
                int16_t cur_dap_v = (vol >= 100) ? 0 : (int16_t)(((int32_t)vol * 12800) / 100 - 12800);
                if (ch_idx == 0 && s_volume[0] != cur_dap_v) {
                    s_volume[0] = cur_dap_v;
                }
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
            uint32_t new_rate = (uint32_t)tu_le32toh(cur->bCur);
            if (new_rate == 44100 || new_rate == 48000) {
                s_sample_rate = new_rate;
                ESP_LOGI(TAG, "UAC2 Set Sample Rate: %lu Hz", (unsigned long)s_sample_rate);
                i2s_output_set_rate(new_rate);
                eq_set_sample_rate(new_rate);
                return true;
            } else {
                ESP_LOGW(TAG, "UAC2 Taxa de amostragem nao suportada: %lu Hz", (unsigned long)new_rate);
                return false;
            }
        } else if (entity_id == UAC2_ENTITY_FEATURE_UNIT) {
            if (ctrl_sel == AUDIO20_FU_CTRL_MUTE) {
                uint8_t ch_idx = (ch < 3) ? ch : 0;
                s_mute[ch_idx] = buf[0];
                return true;
            } else if (ctrl_sel == AUDIO20_FU_CTRL_VOLUME) {
                uint8_t ch_idx = (ch < 3) ? ch : 0;
                s_volume[ch_idx] = (int16_t)(buf[0] | (buf[1] << 8));
                if (ch_idx == 0 || ch_idx == 1) {
                    int16_t v = s_volume[ch_idx];
                    int pct;
                    if (v >= 0) {
                        pct = 100;
                    } else if (v <= -12800) {
                        pct = 0;
                    } else {
                        pct = (int)(((int32_t)(v + 12800) * 100) / 12800);
                        if (pct < 0) pct = 0;
                        if (pct > 100) pct = 100;
                    }
                    audio_player_set_volume(pct);
                }
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
    if ((!s_connected && !tud_mounted()) || s_current_mode != USB_MODE_DAC) return;
    
    uint16_t key = 0;
    switch(command) {
        case 0: key = HID_USAGE_CONSUMER_PLAY_PAUSE; break;
        case 1: key = HID_USAGE_CONSUMER_SCAN_NEXT_TRACK; break;
        case 2: key = HID_USAGE_CONSUMER_SCAN_PREVIOUS_TRACK; break;
        case 3: key = HID_USAGE_CONSUMER_VOLUME_INCREMENT; break;
        case 4: key = HID_USAGE_CONSUMER_VOLUME_DECREMENT; break;
        default: return;
    }

    int wait_retry = 25;
    while (!tud_hid_ready() && wait_retry-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (!tud_hid_ready()) {
        ESP_LOGW(TAG, "HID endpoint ocupado ou nao pronto (cmd=%d)", command);
        return;
    }

    tud_hid_report(0, &key, 2);
    vTaskDelay(pdMS_TO_TICKS(10));

    wait_retry = 25;
    while (!tud_hid_ready() && wait_retry-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
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
