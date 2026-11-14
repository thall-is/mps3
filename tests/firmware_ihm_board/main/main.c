#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "u8g2.h"
#include "u8g2_hal.h"
#include "rtc_ds3231.h"
#include "pcf8574.h"

static const char *TAG = "ihm_sd_validator";

// I2C (Display OLED + RTC DS3231 + PCF8574)
#define PIN_I2C_SDA     15
#define PIN_I2C_SCL     7
#define PIN_PCF_INT     GPIO_NUM_2
#define OLED_ADDR       0x3C

// SDMMC Host (MicroSD 4-Bit)
#define PIN_SD_CLK      GPIO_NUM_11 // CLK
#define PIN_SD_CMD      GPIO_NUM_10 // Pino 3 do SD
#define PIN_SD_D0       GPIO_NUM_12 // MISO / Pino 7 do SD
#define PIN_SD_D1       GPIO_NUM_8  // Pino 8 do SD
#define PIN_SD_D2       GPIO_NUM_18 // Pino 1 do SD
#define PIN_SD_D3       GPIO_NUM_9  // Pino 2 do SD
#define PIN_SD_CD       GPIO_NUM_17 // SD Detect (GND quando inserido)

#define SD_MOUNT_POINT  "/sdcard"

typedef struct {
    uint8_t pin_up;      // Default: 4
    uint8_t pin_down;    // Default: 6
    uint8_t pin_left;    // Default: 5
    uint8_t pin_right;   // Default: 7
    uint8_t pin_center;  // Default: 3
    uint8_t magic;
} ihm_button_map_t;

#define IHM_CONFIG_NAMESPACE "ihm_cfg"
#define IHM_CONFIG_KEY       "btn_map"
#define IHM_MAP_MAGIC        0x5A

static ihm_button_map_t s_btn_map = {
    .pin_up = 4,
    .pin_down = 6,
    .pin_left = 5,
    .pin_right = 7,
    .pin_center = 3,
    .magic = IHM_MAP_MAGIC
};

static u8g2_t s_u8g2;
static sdmmc_card_t *s_card = NULL;
static bool s_sd_mounted = false;
static char s_sd_info_str[48] = "SD: Aguardando...";

static bool ihm_load_button_map(ihm_button_map_t *map)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(IHM_CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS: Namespace '%s' nao encontrado (%s)", IHM_CONFIG_NAMESPACE, esp_err_to_name(err));
        return false;
    }
    size_t required_size = sizeof(ihm_button_map_t);
    err = nvs_get_blob(handle, IHM_CONFIG_KEY, map, &required_size);
    nvs_close(handle);
    if (err == ESP_OK && map->magic == IHM_MAP_MAGIC) {
        ESP_LOGI(TAG, "NVS: Mapeamento carregado com sucesso: U=P%d, D=P%d, L=P%d, R=P%d, C=P%d",
                 map->pin_up, map->pin_down, map->pin_left, map->pin_right, map->pin_center);
        return true;
    }
    ESP_LOGW(TAG, "NVS: Dados invalidos ou ausentes (err=%s, magic=0x%02X)", esp_err_to_name(err), map->magic);
    return false;
}

static esp_err_t ihm_save_button_map(const ihm_button_map_t *map)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(IHM_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao abrir NVS para gravacao: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(handle, IHM_CONFIG_KEY, map, sizeof(ihm_button_map_t));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
        ESP_LOGI(TAG, "NVS: Gravado com sucesso! U=P%d, D=P%d, L=P%d, R=P%d, C=P%d",
                 map->pin_up, map->pin_down, map->pin_left, map->pin_right, map->pin_center);
    } else {
        ESP_LOGE(TAG, "NVS: Erro ao salvar blob: %s", esp_err_to_name(err));
    }
    nvs_close(handle);
    return err;
}

static int ihm_wait_for_press(int step, int total, const char *dir_name, const char *arrow_symbol)
{
    ESP_LOGI(TAG, "[PASSO %d/%d] Aguardando direcao: %s [%s]...", step, total, dir_name, arrow_symbol);

    // 1. Aguardar botões serem soltos com timeout de segurança (máx 500ms)
    uint8_t raw = 0xFF;
    for (int t = 0; t < 25; t++) {
        pcf8574_read_raw(&raw);
        if (raw == 0xFF) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    // 2. Aguardar pressionamento de qualquer pino do PCF8574
    int detected_pin = -1;
    while (detected_pin == -1) {
        pcf8574_read_raw(&raw);

        for (int p = 0; p < 8; p++) {
            if ((raw & (1 << p)) == 0) {
                // Debounce de 40ms
                vTaskDelay(pdMS_TO_TICKS(40));
                uint8_t confirm = 0xFF;
                pcf8574_read_raw(&confirm);
                if ((confirm & (1 << p)) == 0) {
                    detected_pin = p;
                    break;
                }
            }
        }

        // Renderização OLED de instrução
        u8g2_ClearBuffer(&s_u8g2);
        u8g2_SetDrawColor(&s_u8g2, 1);

        u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
        u8g2_DrawBox(&s_u8g2, 0, 0, 128, 12);
        u8g2_SetDrawColor(&s_u8g2, 0);
        char header_buf[32];
        snprintf(header_buf, sizeof(header_buf), "CALIB [%d/%d]", step, total);
        u8g2_DrawStr(&s_u8g2, 34, 10, header_buf);
        u8g2_SetDrawColor(&s_u8g2, 1);

        u8g2_DrawFrame(&s_u8g2, 4, 15, 120, 31);
        u8g2_SetFont(&s_u8g2, u8g2_font_7x14B_tr);
        char prompt_buf[48];
        snprintf(prompt_buf, sizeof(prompt_buf), "%s [%s]", dir_name, arrow_symbol);
        int w = u8g2_GetStrWidth(&s_u8g2, prompt_buf);
        u8g2_DrawStr(&s_u8g2, (128 - w) / 2, 31, prompt_buf);

        u8g2_SetFont(&s_u8g2, u8g2_font_5x7_tr);
        u8g2_DrawStr(&s_u8g2, 16, 42, "Aperte o botao desejado");

        char status_buf[48];
        snprintf(status_buf, sizeof(status_buf), "RAW:0x%02X P:[%d%d%d%d%d%d%d%d]",
                 raw,
                 (raw & 0x80) ? 1 : 0, (raw & 0x40) ? 1 : 0,
                 (raw & 0x20) ? 1 : 0, (raw & 0x10) ? 1 : 0,
                 (raw & 0x08) ? 1 : 0, (raw & 0x04) ? 1 : 0,
                 (raw & 0x02) ? 1 : 0, (raw & 0x01) ? 1 : 0);
        u8g2_DrawStr(&s_u8g2, 4, 58, status_buf);

        u8g2_SendBuffer(&s_u8g2);
        vTaskDelay(pdMS_TO_TICKS(25));
    }

    ESP_LOGI(TAG, ">>> [%d/%d] Direcao '%s' detectada no pino P%d! <<<", step, total, dir_name, detected_pin);

    // Feedback visual imediato
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_DrawBox(&s_u8g2, 4, 15, 120, 31);
    u8g2_SetDrawColor(&s_u8g2, 0);
    u8g2_SetFont(&s_u8g2, u8g2_font_7x14B_tr);
    char ok_buf[48];
    snprintf(ok_buf, sizeof(ok_buf), "%s -> P%d OK!", dir_name, detected_pin);
    int ok_w = u8g2_GetStrWidth(&s_u8g2, ok_buf);
    u8g2_DrawStr(&s_u8g2, (128 - ok_w) / 2, 35, ok_buf);
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_SendBuffer(&s_u8g2);

    // Aguardar o usuário soltar o botão com timeout (máx 1s)
    for (int t = 0; t < 50; t++) {
        pcf8574_read_raw(&raw);
        if ((raw & (1 << detected_pin)) != 0) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    return detected_pin;
}

static void ihm_run_calibration_wizard(void)
{
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "   INICIANDO ASSISTENTE DE CALIBRACAO DE BOTOES   ");
    ESP_LOGI(TAG, "==================================================");

    ihm_button_map_t new_map;
    new_map.magic = IHM_MAP_MAGIC;

    new_map.pin_up     = (uint8_t)ihm_wait_for_press(1, 5, "CIMA (UP)", "^");
    new_map.pin_down   = (uint8_t)ihm_wait_for_press(2, 5, "BAIXO (DOWN)", "v");
    new_map.pin_left   = (uint8_t)ihm_wait_for_press(3, 5, "ESQUERDA (LEFT)", "<");
    new_map.pin_right  = (uint8_t)ihm_wait_for_press(4, 5, "DIREITA (RIGHT)", ">");
    new_map.pin_center = (uint8_t)ihm_wait_for_press(5, 5, "CENTRO (CLICK)", "OK");

    s_btn_map = new_map;
    ihm_save_button_map(&s_btn_map);

    // Resumo salvo na tela
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    u8g2_DrawBox(&s_u8g2, 0, 0, 128, 12);
    u8g2_SetDrawColor(&s_u8g2, 0);
    u8g2_DrawStr(&s_u8g2, 12, 10, "CALIBRACAO SALVA!");
    u8g2_SetDrawColor(&s_u8g2, 1);

    u8g2_SetFont(&s_u8g2, u8g2_font_5x7_tr);
    char summary1[48], summary2[48];
    snprintf(summary1, sizeof(summary1), "U:P%d | D:P%d | L:P%d",
             s_btn_map.pin_up, s_btn_map.pin_down, s_btn_map.pin_left);
    snprintf(summary2, sizeof(summary2), "R:P%d | CENTRO:P%d",
             s_btn_map.pin_right, s_btn_map.pin_center);
    u8g2_DrawStr(&s_u8g2, 8, 28, summary1);
    u8g2_DrawStr(&s_u8g2, 8, 40, summary2);
    u8g2_DrawStr(&s_u8g2, 8, 56, "Gravado com sucesso na NVS!");
    u8g2_SendBuffer(&s_u8g2);

    vTaskDelay(pdMS_TO_TICKS(2000));
}

static char s_sd_pin_diag[64] = "D0:? D1:? D2:? D3:?";

static void test_sd_pins_electrical(void)
{
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "   DIAGNOSTICO ELETRICO DOS PINOS SD (GPIO)       ");
    ESP_LOGI(TAG, "==================================================");

    const struct {
        const char *name;
        gpio_num_t pin;
        const char *sd_pin;
    } pins[] = {
        {"CLK ", PIN_SD_CLK, "Pino 5"},
        {"CMD ", PIN_SD_CMD, "Pino 3"},
        {"DAT0", PIN_SD_D0,  "Pino 7 (MISO)"},
        {"DAT1", PIN_SD_D1,  "Pino 8"},
        {"DAT2", PIN_SD_D2,  "Pino 1"},
        {"DAT3", PIN_SD_D3,  "Pino 2 (CS/CD)"},
        {"CD  ", PIN_SD_CD,  "Detect (G17)"}
    };

    int results[7][3]; // [pin][0:PU, 1:PD, 2:FLT]

    for (int i = 0; i < 7; i++) {
        gpio_reset_pin(pins[i].pin);
        gpio_set_direction(pins[i].pin, GPIO_MODE_INPUT);

        // 1. Pull-Up
        gpio_set_pull_mode(pins[i].pin, GPIO_PULLUP_ONLY);
        vTaskDelay(pdMS_TO_TICKS(10));
        results[i][0] = gpio_get_level(pins[i].pin);

        // 2. Pull-Down
        gpio_set_pull_mode(pins[i].pin, GPIO_PULLDOWN_ONLY);
        vTaskDelay(pdMS_TO_TICKS(10));
        results[i][1] = gpio_get_level(pins[i].pin);

        // 3. Floating
        gpio_set_pull_mode(pins[i].pin, GPIO_FLOATING);
        vTaskDelay(pdMS_TO_TICKS(10));
        results[i][2] = gpio_get_level(pins[i].pin);

        const char *status_str = "OK (Normal)";
        if (results[i][0] == 0 && results[i][1] == 0) {
            status_str = "CURTO COM GND (Preso em 0V)";
        } else if (results[i][0] == 1 && results[i][1] == 1) {
            status_str = "PULL-UP EXTERNO FORTE (ou 3.3V)";
        } else if (results[i][0] == 1 && results[i][1] == 0) {
            status_str = "ALERTA: FLUTUANDO (Sem contato externo)";
        }

        ESP_LOGI(TAG, "  [%s] GPIO %2d (%-14s) -> PU:%d | PD:%d | FLT:%d => %s",
                 pins[i].name, pins[i].pin, pins[i].sd_pin,
                 results[i][0], results[i][1], results[i][2], status_str);
    }

    snprintf(s_sd_pin_diag, sizeof(s_sd_pin_diag), "D0:%s D1:%s D2:%s D3:%s",
             (results[2][0] == 0) ? "GND" : ((results[2][1] == 0) ? "FLT" : "OK"),
             (results[3][0] == 0) ? "GND" : ((results[3][1] == 0) ? "FLT" : "OK"),
             (results[4][0] == 0) ? "GND" : ((results[4][1] == 0) ? "FLT" : "OK"),
             (results[5][0] == 0) ? "GND" : ((results[5][1] == 0) ? "FLT" : "OK"));

    ESP_LOGI(TAG, "Diagnostico preliminar linhas de dados: %s", s_sd_pin_diag);
    ESP_LOGI(TAG, "==================================================");
}

static void test_sd_mount(void)
{
    if (s_sd_mounted) return;

    // 1. Executa diagnostico eletrico dos pinos antes da inicializacao do protocolo
    test_sd_pins_electrical();

    // Habilita logs detalhados do driver SDMMC para identificar comandos e respostas
    esp_log_level_set("sdmmc_req", ESP_LOG_DEBUG);
    esp_log_level_set("sdmmc_cmd", ESP_LOG_DEBUG);
    esp_log_level_set("sdmmc_periph", ESP_LOG_DEBUG);
    esp_log_level_set("sdmmc_sd", ESP_LOG_DEBUG);

    ESP_LOGI(TAG, "--- TESTE 1: Tentando montagem SDMMC 4-Bit a 400 kHz ---");
    snprintf(s_sd_info_str, sizeof(s_sd_info_str), "SD: Testando 4-bit 400k...");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_PROBING; // 400 kHz

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = PIN_SD_CLK;
    slot_config.cmd = PIN_SD_CMD;
    slot_config.d0  = PIN_SD_D0;
    slot_config.d1  = PIN_SD_D1;
    slot_config.d2  = PIN_SD_D2;
    slot_config.d3  = PIN_SD_D3;
    slot_config.cd  = PIN_SD_CD;
    slot_config.width = 4;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, ">>> SUCESSO EM 4-BIT @ 400 kHz! Todas as linhas responderam!");
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        s_card = NULL;

        ESP_LOGI(TAG, "--- TESTE 2: Tentando SDMMC 4-Bit a 20 MHz (Alta Velocidade) ---");
        host.max_freq_khz = SDMMC_FREQ_DEFAULT; // 20 MHz
        ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
        if (ret == ESP_OK) {
            s_sd_mounted = true;
            uint64_t total = 0, free = 0;
            esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free);
            double gb = (double)total / (1024.0 * 1024.0 * 1024.0);
            ESP_LOGI(TAG, ">>> SUCESSO TOTAL! 4-BIT @ 20 MHz MONTADO! Cap: %.2f GB", gb);
            snprintf(s_sd_info_str, sizeof(s_sd_info_str), "SD 4B:%.1fGB [20MHz OK]", gb);
            return;
        } else {
            ESP_LOGW(TAG, "4-bit funciona a 400kHz mas falhou a 20MHz (%s) -> Ruido/fios longos/pullup fraco!", esp_err_to_name(ret));
            // Tenta 10 MHz
            host.max_freq_khz = 10000;
            ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
            if (ret == ESP_OK) {
                s_sd_mounted = true;
                snprintf(s_sd_info_str, sizeof(s_sd_info_str), "SD 4B: 10MHz OK");
                return;
            }
        }
    } else {
        ESP_LOGE(TAG, ">>> Falha em 4-Bit: ret=0x%x (%s) <<<", ret, esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "--- TESTE 3: Fallback para SDMMC 1-Bit (Usa apenas D0/G12) ---");
    slot_config.width = 1;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (ret == ESP_OK) {
        s_sd_mounted = true;
        uint64_t total_bytes = 0, free_bytes = 0;
        esp_vfs_fat_info(SD_MOUNT_POINT, &total_bytes, &free_bytes);
        double cap_gb = (double)total_bytes / (1024.0 * 1024.0 * 1024.0);
        ESP_LOGI(TAG, ">>> SD 1-BIT MONTADO COM SUCESSO! Cap: %.2f GB", cap_gb);
        snprintf(s_sd_info_str, sizeof(s_sd_info_str), "SD 1B:%.1fGB (1-Bit)", cap_gb);
    } else {
        ESP_LOGE(TAG, "Falha total ao montar SD: %s", esp_err_to_name(ret));
        snprintf(s_sd_info_str, sizeof(s_sd_info_str), "SD ERR: %s", esp_err_to_name(ret));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "   MPS3 — Validador IHM + SDMMC 4-Bit (ESP32-S3)  ");
    ESP_LOGI(TAG, "==================================================");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // 1. Configura GPIO 2 (/INT do PCF8574) com Pull-Up
    gpio_config_t int_cfg = {
        .pin_bit_mask = (1ULL << PIN_PCF_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&int_cfg);

    // 2. Configura GPIO 17 (SD Detect) com Pull-Up
    gpio_config_t cd_cfg = {
        .pin_bit_mask = (1ULL << PIN_SD_CD),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cd_cfg);

    // 3. Inicializa I2C (SDA=15, SCL=7) @ 400kHz e OLED SSD1306 (U8G2_R0)
    ESP_LOGI(TAG, "Iniciando I2C (SDA=%d, SCL=%d)...", PIN_I2C_SDA, PIN_I2C_SCL);
    u8g2_hal_i2c_init(PIN_I2C_SDA, PIN_I2C_SCL, OLED_ADDR);

    u8g2_Setup_ssd1306_i2c_128x64_noname_f(&s_u8g2, U8G2_R0,
                                            u8g2_hal_byte_cb, u8g2_hal_gpio_and_delay_cb);
    s_u8g2.u8x8.cad_cb = u8x8_cad_mps3_fast_i2c;

    u8g2_InitDisplay(&s_u8g2);
    u8g2_SetPowerSave(&s_u8g2, 0);
    u8g2_SetContrast(&s_u8g2, 255);
    u8g2_SetBitmapMode(&s_u8g2, 1);
    u8g2_SetFontMode(&s_u8g2, 1);

    i2c_master_bus_handle_t i2c_bus = u8g2_hal_get_bus_handle();

    // 4. Inicializa RTC DS3231 e PCF8574
    bool rtc_ok = (i2c_bus && rtc_ds3231_detect(i2c_bus) && rtc_ds3231_init(i2c_bus) == ESP_OK);
    bool pcf_ok = (i2c_bus && pcf8574_init(i2c_bus) == ESP_OK);
    uint8_t pcf_addr = pcf_ok ? pcf8574_get_address() : 0;

    ESP_LOGI(TAG, "I2C: RTC DS3231: %s | PCF8574: %s (0x%02X)",
             rtc_ok ? "OK" : "FALHA", pcf_ok ? "OK" : "FALHA", pcf_addr);

    // 5. Verificação da calibração na NVS
    bool has_calib = ihm_load_button_map(&s_btn_map);
    if (!has_calib) {
        ESP_LOGI(TAG, "NVS: Usando mapa padrao de botoes (U4, D6, L5, R7, C3).");
    }

    uint8_t last_raw = 0xFF;
    int last_int_level = -1;
    int last_cd_level = -1;
    int hold_btn_ticks = 0;
    int64_t last_heartbeat = 0;

    // Executa diagnóstico elétrico dos pinos SD imediatamente no boot
    test_sd_pins_electrical();

    // Se o cartão já estiver presente no boot, testa a montagem
    if (gpio_get_level(PIN_SD_CD) == 0) {
        ESP_LOGI(TAG, "Cartao SD detectado no boot (G17=0)! Disparando teste de montagem...");
        test_sd_mount();
    }

    // Loop Principal
    while (true) {
        int int_level = gpio_get_level(PIN_PCF_INT);
        bool int_active = (int_level == 0);

        int cd_level = gpio_get_level(PIN_SD_CD);
        bool card_present = (cd_level == 0); // Ativo em nível baixo

        // Se o cartão foi inserido e ainda não montou, dispara teste de montagem
        if (card_present && !s_sd_mounted) {
            ESP_LOGI(TAG, "Cartao SD detectado no GPIO 17! Iniciando teste de montagem...");
            test_sd_mount();
        } else if (!card_present && s_sd_mounted) {
            ESP_LOGW(TAG, "Cartao SD removido!");
            esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
            s_card = NULL;
            s_sd_mounted = false;
            snprintf(s_sd_info_str, sizeof(s_sd_info_str), "SD: Cartao Removido");
        } else if (!card_present && !s_sd_mounted) {
            snprintf(s_sd_info_str, sizeof(s_sd_info_str), "SD: Sem Cartao [G17=1]");
        }

        // Leitura do RTC
        struct tm tm_info = {0};
        float temp = 0.0f;
        char time_str[16] = "--:--:--";
        char temp_str[16] = "--.-C";

        if (rtc_ok && rtc_ds3231_get_time(&tm_info) == ESP_OK) {
            snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d",
                     tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
            if (rtc_ds3231_get_temperature(&temp) == ESP_OK) {
                snprintf(temp_str, sizeof(temp_str), "%.1fC", temp);
            }
        }

        // Leitura do PCF8574
        uint8_t raw = 0xFF;
        if (pcf_ok) {
            pcf8574_read_raw(&raw);
        }

        bool joy_up     = ((raw & (1u << s_btn_map.pin_up)) == 0);
        bool joy_down   = ((raw & (1u << s_btn_map.pin_down)) == 0);
        bool joy_left   = ((raw & (1u << s_btn_map.pin_left)) == 0);
        bool joy_right  = ((raw & (1u << s_btn_map.pin_right)) == 0);
        bool joy_center = ((raw & (1u << s_btn_map.pin_center)) == 0);

        // Telemetria UART com nomes calibrados das direções
        if (raw != last_raw || int_level != last_int_level || cd_level != last_cd_level) {
            char dir_str[64] = "";
            if (joy_up)     strcat(dir_str, "[CIMA] ");
            if (joy_down)   strcat(dir_str, "[BAIXO] ");
            if (joy_left)   strcat(dir_str, "[ESQUERDA] ");
            if (joy_right)  strcat(dir_str, "[DIREITA] ");
            if (joy_center) strcat(dir_str, "[CENTRO] ");
            if (strlen(dir_str) == 0) strcpy(dir_str, "[SOLTO]");

            ESP_LOGI(TAG, "[IHM] %-22s | RAW:0x%02X | INT:%s | SD:%s",
                     dir_str, raw, int_active ? "0(ATIVO)" : "1(HI)",
                     card_present ? "INSERIDO" : "AUSENTE");
            last_raw = raw;
            last_int_level = int_level;
            last_cd_level = cd_level;
        }

        // Heartbeat periódico a cada 2.5 segundos com telemetria dos pinos SD
        if (esp_timer_get_time() - last_heartbeat > 2500000LL) {
            last_heartbeat = esp_timer_get_time();
            ESP_LOGI(TAG, "[HEARTBEAT] SD:%s | DIAG:%s | RAW:0x%02X | INT:%d | CD(G17):%d",
                     s_sd_info_str, s_sd_pin_diag, raw, int_level, cd_level);
        }

        // ====================================================================
        // RENDERIZAÇÃO GRÁFICA NO OLED (128x64)
        // ====================================================================
        u8g2_ClearBuffer(&s_u8g2);

        // 1. Barra de Título Superior
        u8g2_SetDrawColor(&s_u8g2, 1);
        u8g2_DrawBox(&s_u8g2, 0, 0, 128, 10);
        u8g2_SetDrawColor(&s_u8g2, 0);
        u8g2_SetFont(&s_u8g2, u8g2_font_5x7_tr);
        u8g2_DrawStr(&s_u8g2, 6, 8, "MPS3 - IHM + SD TESTER");
        u8g2_SetDrawColor(&s_u8g2, 1);

        // 2. Linha do Cartão SD (Destaque Principal)
        u8g2_SetFont(&s_u8g2, u8g2_font_5x7_tr);
        if (s_sd_mounted) {
            u8g2_DrawBox(&s_u8g2, 2, 12, 124, 11);
            u8g2_SetDrawColor(&s_u8g2, 0);
            u8g2_DrawStr(&s_u8g2, 4, 20, s_sd_info_str);
            u8g2_SetDrawColor(&s_u8g2, 1);
        } else {
            u8g2_DrawFrame(&s_u8g2, 2, 12, 124, 11);
            u8g2_DrawStr(&s_u8g2, 4, 20, s_sd_info_str);
        }

        // 3. Status RTC e Pino /INT
        char rtc_buf[48];
        snprintf(rtc_buf, sizeof(rtc_buf), "RTC:%s %s", time_str, temp_str);
        u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
        u8g2_DrawStr(&s_u8g2, 2, 31, rtc_buf);

        if (int_active) {
            u8g2_DrawBox(&s_u8g2, 92, 24, 34, 8);
            u8g2_SetDrawColor(&s_u8g2, 0);
            u8g2_DrawStr(&s_u8g2, 94, 30, "INT:0");
            u8g2_SetDrawColor(&s_u8g2, 1);
        } else {
            u8g2_DrawFrame(&s_u8g2, 92, 24, 34, 8);
            u8g2_DrawStr(&s_u8g2, 94, 30, "INT:1");
        }

        // 4. Cruz do D-PAD (Centro-Esquerda)
        if (joy_up)     { u8g2_DrawBox(&s_u8g2, 18, 34, 12, 9); u8g2_SetDrawColor(&s_u8g2, 0); u8g2_DrawStr(&s_u8g2, 22, 41, "^"); u8g2_SetDrawColor(&s_u8g2, 1); }
        else            { u8g2_DrawFrame(&s_u8g2, 18, 34, 12, 9); u8g2_DrawStr(&s_u8g2, 22, 41, "^"); }

        if (joy_left)   { u8g2_DrawBox(&s_u8g2, 4, 44, 12, 9); u8g2_SetDrawColor(&s_u8g2, 0); u8g2_DrawStr(&s_u8g2, 8, 51, "<"); u8g2_SetDrawColor(&s_u8g2, 1); }
        else            { u8g2_DrawFrame(&s_u8g2, 4, 44, 12, 9); u8g2_DrawStr(&s_u8g2, 8, 51, "<"); }

        if (joy_center) { u8g2_DrawBox(&s_u8g2, 18, 44, 12, 9); u8g2_SetDrawColor(&s_u8g2, 0); u8g2_DrawStr(&s_u8g2, 22, 51, "o"); u8g2_SetDrawColor(&s_u8g2, 1); }
        else            { u8g2_DrawFrame(&s_u8g2, 18, 44, 12, 9); u8g2_DrawStr(&s_u8g2, 22, 51, "o"); }

        if (joy_right)  { u8g2_DrawBox(&s_u8g2, 32, 44, 12, 9); u8g2_SetDrawColor(&s_u8g2, 0); u8g2_DrawStr(&s_u8g2, 36, 51, ">"); u8g2_SetDrawColor(&s_u8g2, 1); }
        else            { u8g2_DrawFrame(&s_u8g2, 32, 44, 12, 9); u8g2_DrawStr(&s_u8g2, 36, 51, ">"); }

        if (joy_down)   { u8g2_DrawBox(&s_u8g2, 18, 54, 12, 9); u8g2_SetDrawColor(&s_u8g2, 0); u8g2_DrawStr(&s_u8g2, 22, 61, "v"); u8g2_SetDrawColor(&s_u8g2, 1); }
        else            { u8g2_DrawFrame(&s_u8g2, 18, 54, 12, 9); u8g2_DrawStr(&s_u8g2, 22, 61, "v"); }

        // 5. Linha dos Pinos do PCF8574 no lado direito
        u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
        char pcf_pins[32];
        snprintf(pcf_pins, sizeof(pcf_pins), "B:[%d%d%d%d%d%d%d%d]",
                 (raw & 0x80) ? 1 : 0, (raw & 0x40) ? 1 : 0,
                 (raw & 0x20) ? 1 : 0, (raw & 0x10) ? 1 : 0,
                 (raw & 0x08) ? 1 : 0, (raw & 0x04) ? 1 : 0,
                 (raw & 0x02) ? 1 : 0, (raw & 0x01) ? 1 : 0);
        u8g2_DrawStr(&s_u8g2, 52, 41, pcf_pins);

        char cal_info[32];
        snprintf(cal_info, sizeof(cal_info), "U%d D%d L%d R%d C%d",
                 s_btn_map.pin_up, s_btn_map.pin_down, s_btn_map.pin_left,
                 s_btn_map.pin_right, s_btn_map.pin_center);
        u8g2_DrawStr(&s_u8g2, 52, 50, cal_info);

        u8g2_DrawStr(&s_u8g2, 52, 60, s_sd_pin_diag);

        u8g2_SendBuffer(&s_u8g2);

        // Se segurar qualquer botão por 3 segundos, dispara assistente de calibração
        if (raw != 0xFF) {
            hold_btn_ticks++;
            if (hold_btn_ticks >= 60) {
                hold_btn_ticks = 0;
                ESP_LOGI(TAG, "Botao mantido por 3s. Disparando assistente de calibracao...");
                ihm_run_calibration_wizard();
            }
        } else {
            hold_btn_ticks = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(50)); // ~20 FPS
    }
}
