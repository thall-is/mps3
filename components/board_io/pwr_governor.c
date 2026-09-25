#include "pwr_governor.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "pinos.h"
#include "rgb_led.h"
#include "i2s_output.h"
#include "esp_log.h"

static const char *TAG = "PWR_GOV";

static pwr_freq_t s_current_freq = PWR_FREQ_240MHZ;
static bool s_initialized = false;
static bool s_manual_mode = false;
static pwr_sleep_cb_t s_sleep_cb = NULL;

extern void rtc_clk_bbpll_add_consumer(void);

esp_err_t pwr_governor_init(void)
{
    if (s_initialized) return ESP_OK;

    // Registra consumidor permanente do BBPLL:
    // Garante que quando a CPU comutar para 40 MHz (XTAL), o hardware NÃO desligue
    // o PLL de 480 MHz, mantendo ativos o I2S_CLK_SRC_PLL_240M e o SDMMC_CLK_SRC (160 MHz)!
    rtc_clk_bbpll_add_consumer();

    // Configuração inicial com 240 MHz
    esp_pm_config_t pm_cfg = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 240,
        .light_sleep_enable = false,
    };
    esp_err_t err = esp_pm_configure(&pm_cfg);
    if (err == ESP_OK) {
        s_current_freq = PWR_FREQ_240MHZ;
        s_initialized = true;
        s_manual_mode = false;
        ESP_LOGI(TAG, "Governador de clock inicializado com sucesso (Base: 240 MHz, BBPLL protegido).");
    } else {
        ESP_LOGE(TAG, "Falha ao inicializar esp_pm_configure: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t pwr_governor_set_cpu_freq(pwr_freq_t freq)
{
    if (freq != PWR_FREQ_40MHZ && freq != PWR_FREQ_80MHZ && freq != PWR_FREQ_160MHZ && freq != PWR_FREQ_240MHZ) {
        return ESP_ERR_INVALID_ARG;
    }

    if (freq == s_current_freq) {
        return ESP_OK;
    }

    esp_pm_config_t pm_cfg = {
        .max_freq_mhz = (int)freq,
        .min_freq_mhz = (int)freq,
        .light_sleep_enable = false,
    };
    esp_err_t err = esp_pm_configure(&pm_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Clock da CPU comutado: %d MHz -> %d MHz", (int)s_current_freq, (int)freq);
        s_current_freq = freq;
    } else {
        ESP_LOGE(TAG, "Erro ao comutar clock para %d MHz: %s", (int)freq, esp_err_to_name(err));
    }
    return err;
}

pwr_freq_t pwr_governor_get_cpu_freq(void)
{
    return s_current_freq;
}

void pwr_governor_set_manual_mode(bool manual)
{
    s_manual_mode = manual;
    ESP_LOGI(TAG, "Modo manual de clock: %s", manual ? "ATIVADO (Trava manual para bancada)" : "DESATIVADO (Transição Dinâmica)");
}

bool pwr_governor_is_manual_mode(void)
{
    return s_manual_mode;
}

void pwr_governor_update(bool screen_active, bool wifi_active, bool seeking, bool playing, uint32_t sample_rate)
{
    // Se o operador na bancada travou o clock manualmente (ex: via 'freq 80'), respeita a trava
    if (s_manual_mode) {
        return;
    }

    // Prioridade 1: Wi-Fi, OTA, Sincronizacao ou Seek ativo -> Throughput Maximo (240 MHz - "Sprint-to-Idle")
    if (wifi_active || seeking) {
        pwr_governor_set_cpu_freq(PWR_FREQ_240MHZ);
        return;
    }

    // Prioridade 2: Hi-Res Extremo (ex: FLAC 176.4k ou 192k) -> 240 MHz
    if (playing && sample_rate > 96000) {
        pwr_governor_set_cpu_freq(PWR_FREQ_240MHZ);
        return;
    }

    // Prioridade 3: Tela Ligada (Navegacao de Menus, Listas, UI fluida) -> 160 MHz
    if (screen_active) {
        pwr_governor_set_cpu_freq(PWR_FREQ_160MHZ);
        return;
    }

    // Prioridade 4: Tela Bloqueada/Apagada (No Bolso)
    // Se estiver tocando Hi-Res intermediario (88.2k ou 96k): 160 MHz
    if (playing && sample_rate > 48000) {
        pwr_governor_set_cpu_freq(PWR_FREQ_160MHZ);
        return;
    }

    // Se estiver tocando MP3/WAV/AAC padrao (<= 48kHz) ou Pausado -> Maxima Economia (80 MHz)
    pwr_governor_set_cpu_freq(PWR_FREQ_80MHZ);
}

esp_err_t pwr_governor_enter_light_sleep(uint32_t timeout_ms)
{
    // Wakeup por GPIO em nível baixo (joystick e carregador ativos em GND)
    gpio_wakeup_enable((gpio_num_t)PIN_JOY_UP, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable((gpio_num_t)PIN_JOY_DOWN, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable((gpio_num_t)PIN_JOY_LEFT, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable((gpio_num_t)PIN_JOY_RIGHT, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable((gpio_num_t)PIN_JOY_CENTER, GPIO_INTR_LOW_LEVEL);
    
    // Apenas monitora o pino do carregador se ele atualmente estiver em nível alto (desconectado)
    bool chrg_connected = (gpio_get_level((gpio_num_t)PIN_BATTERY_CHRG) == 0);
    if (!chrg_connected) {
        gpio_wakeup_enable((gpio_num_t)PIN_BATTERY_CHRG, GPIO_INTR_LOW_LEVEL);
    }

    esp_sleep_enable_gpio_wakeup();

    if (timeout_ms > 0) {
        esp_sleep_enable_timer_wakeup((uint64_t)timeout_ms * 1000ULL);
    }

    esp_err_t err = esp_light_sleep_start();

    // Desabilita wakeup apos retornar para operacao normal
    gpio_wakeup_disable((gpio_num_t)PIN_JOY_UP);
    gpio_wakeup_disable((gpio_num_t)PIN_JOY_DOWN);
    gpio_wakeup_disable((gpio_num_t)PIN_JOY_LEFT);
    gpio_wakeup_disable((gpio_num_t)PIN_JOY_RIGHT);
    gpio_wakeup_disable((gpio_num_t)PIN_JOY_CENTER);
    if (!chrg_connected) {
        gpio_wakeup_disable((gpio_num_t)PIN_BATTERY_CHRG);
    }

    return err;
}

void pwr_governor_set_sleep_cb(pwr_sleep_cb_t cb)
{
    s_sleep_cb = cb;
}

void pwr_governor_enter_deep_sleep(void)
{
    ESP_LOGI(TAG, "Preparando para entrar em Deep Sleep (<100 uA)...");

    // 1. Invoca callback de preparação (desliga display OLED via 0xAE Display OFF)
    if (s_sleep_cb != NULL) {
        s_sleep_cb();
    }

    // 2. Desliga LED RGB (WS2812)
    rgb_led_config_t led_cfg;
    rgb_led_get_config(&led_cfg);
    led_cfg.enabled = false;
    rgb_led_set_config(&led_cfg);

    // 3. Desativa barramento de áudio I2S (corta MCLK e relógios do DAC PCM5102A)
    i2s_output_disable();

    // 4. Desliga o co-processador Bluetooth (segura o pino EN/CHIP_PU em nível baixo)
#if defined(PIN_BT_LINK_EN) && (PIN_BT_LINK_EN >= 0)
    gpio_set_direction((gpio_num_t)PIN_BT_LINK_EN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PIN_BT_LINK_EN, 0);
    gpio_hold_en((gpio_num_t)PIN_BT_LINK_EN);
#endif
    // Desativa UART TX/RX do Companion para eliminar phantom powering e fuga pelo diodo ESD
#if defined(PIN_BT_LINK_UART_TX) && (PIN_BT_LINK_UART_TX >= 0)
    gpio_set_direction((gpio_num_t)PIN_BT_LINK_UART_TX, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)PIN_BT_LINK_UART_TX, GPIO_FLOATING);
#endif
#if defined(PIN_BT_LINK_UART_RX) && (PIN_BT_LINK_UART_RX >= 0)
    gpio_set_direction((gpio_num_t)PIN_BT_LINK_UART_RX, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)PIN_BT_LINK_UART_RX, GPIO_FLOATING);
#endif

    // 5. Configura pinos de barramentos em modo seguro (sem hold para não travar o boot)
    gpio_set_direction((gpio_num_t)PIN_SD_CLK, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)PIN_SD_CLK, GPIO_PULLDOWN_ONLY);

    gpio_set_pull_mode((gpio_num_t)PIN_OLED_SDA, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode((gpio_num_t)PIN_OLED_SCL, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode((gpio_num_t)PIN_SD_CMD, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode((gpio_num_t)PIN_SD_D0, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode((gpio_num_t)PIN_SD_D1, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode((gpio_num_t)PIN_SD_D2, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode((gpio_num_t)PIN_SD_D3, GPIO_PULLUP_ONLY);

    // 6. Configura wakeup por RTC GPIO nos pinos JOY_UP (GPIO 2) e BATTERY_CHRG (GPIO 11)
    uint64_t ext1_mask = (1ULL << PIN_JOY_UP);
    bool chrg_connected = (gpio_get_level((gpio_num_t)PIN_BATTERY_CHRG) == 0);
    if (!chrg_connected) {
        // Apenas adiciona o carregador como fonte de despertar se estiver na bateria (desconectado)
        ext1_mask |= (1ULL << PIN_BATTERY_CHRG);
        rtc_gpio_init((gpio_num_t)PIN_BATTERY_CHRG);
        rtc_gpio_set_direction((gpio_num_t)PIN_BATTERY_CHRG, RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pullup_en((gpio_num_t)PIN_BATTERY_CHRG);
        rtc_gpio_pulldown_dis((gpio_num_t)PIN_BATTERY_CHRG);
        // NUNCA chamar gpio_hold_en em pino de wakeup (o hold congela o estado e bloqueia o sinal!)
    } else {
        ESP_LOGI(TAG, "Carregador/USB ja conectado na bancada. Wakeup configurado exclusivamente no JOY_UP (GPIO 2).");
    }

    // Mantem o pullup do JOY_UP ativo no dominio RTC durante o deep sleep
    rtc_gpio_init((gpio_num_t)PIN_JOY_UP);
    rtc_gpio_set_direction((gpio_num_t)PIN_JOY_UP, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en((gpio_num_t)PIN_JOY_UP);
    rtc_gpio_pulldown_dis((gpio_num_t)PIN_JOY_UP);
    // NUNCA chamar gpio_hold_en no JOY_UP!

    esp_sleep_enable_ext1_wakeup_io(ext1_mask, ESP_EXT1_WAKEUP_ANY_LOW);

    // Mantem o dominio RTC_PERIPH ligado no Deep Sleep para alimentar os pull-ups dos RTC GPIOs
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    gpio_deep_sleep_hold_en();

    ESP_LOGI(TAG, "Entrando em Deep Sleep agora. Pressione JOY_UP para acordar.");
    esp_deep_sleep_start();
}
