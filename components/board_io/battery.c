#include "battery.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "pinos.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_timer.h"

static const char *TAG = "battery";

// Divisor resistivo 2:1 (ex: dois resistores de 100k+100k entre Vbat e GND).
#define BATTERY_DIVIDER_RATIO 2.0f

#define ADC_ATTEN     ADC_ATTEN_DB_12

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_cali_handle = NULL;
static adc_unit_t s_adc_unit = ADC_UNIT_1;
static adc_channel_t s_adc_channel = ADC_CHANNEL_0;
static bool s_cali_enabled = false;
static bool s_initialized = false;

// NVS EWMA State
static float s_avg_drain_rate = 0.10f; // 0.10 %/min = ~16h default
static float s_avg_charge_rate = 1.00f; // 1.0 %/min = ~1.5h default
static int s_last_saved_percent = -1;
static int64_t s_last_saved_time_us = 0;

static int s_cached_mv = 0;
static int s_cached_time_left = -1;
static bool s_is_charging = false;

static bool try_init_calibration(void)
{
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = s_adc_unit,
        .chan = s_adc_channel,
        .atten = ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    return adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle) == ESP_OK;
}

esp_err_t battery_init(void)
{
    // Configura os pinos de monitoramento do TP4056 (GPIO 11 e 12)
    // Entradas digitais com pull-down interno:
    // Quando vai pra UP (1), esta carregando. Quando esta em DOWN (0), nao esta carregando.
    uint64_t mask = (1ULL << PIN_BATTERY_CHRG);
#if defined(PIN_BATTERY_STDBY) && (PIN_BATTERY_STDBY >= 0)
    mask |= (1ULL << PIN_BATTERY_STDBY);
#endif
    gpio_config_t io_conf = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    ESP_LOGI(TAG, "Pinos de monitoramento TP4056 configurados: CHRG=GPIO%d, STDBY=%d",
             PIN_BATTERY_CHRG, PIN_BATTERY_STDBY);

    if (adc_oneshot_io_to_channel(PIN_BATTERY_ADC, &s_adc_unit, &s_adc_channel) != ESP_OK) {
        ESP_LOGE(TAG, "Pino PIN_BATTERY_ADC (%d) nao tem funcionalidade de ADC!", PIN_BATTERY_ADC);
    }

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = s_adc_unit,
    };
    esp_err_t ret = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao criar unidade ADC: %s", esp_err_to_name(ret));
        return ret;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_oneshot_config_channel(s_adc_handle, s_adc_channel, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao configurar canal ADC: %s", esp_err_to_name(ret));
        return ret;
    }

    s_cali_enabled = try_init_calibration();
    if (!s_cali_enabled) {
        ESP_LOGW(TAG, "Calibracao do ADC indisponivel - leitura de bateria sera aproximada.");
    }

    // Carrega taxas aprendidas da NVS
    nvs_handle_t h;
    if (nvs_open("mps3_settings", NVS_READONLY, &h) == ESP_OK) {
        int32_t val;
        if (nvs_get_i32(h, "bat_drain_rate", &val) == ESP_OK) s_avg_drain_rate = val / 10000.0f;
        if (nvs_get_i32(h, "bat_charge_rate", &val) == ESP_OK) s_avg_charge_rate = val / 10000.0f;
        nvs_close(h);
    }

    s_initialized = true;
    return ESP_OK;
}

battery_status_t battery_get_status(void)
{
    if (!s_initialized) return BATTERY_DISCHARGING;

    int chrg_level  = gpio_get_level((gpio_num_t)PIN_BATTERY_CHRG);
#if defined(PIN_BATTERY_STDBY) && (PIN_BATTERY_STDBY >= 0)
    int stdby_level = gpio_get_level((gpio_num_t)PIN_BATTERY_STDBY);
#else
    int stdby_level = 0;
#endif

    static int s_last_chrg = -1;
    static int s_last_stdby = -1;
    if (chrg_level != s_last_chrg || stdby_level != s_last_stdby) {
        s_last_chrg = chrg_level;
        s_last_stdby = stdby_level;
        ESP_LOGI(TAG, "TP4056 GPIO: CHRG(11)=%d (%s), STDBY=%d",
                 chrg_level, chrg_level ? "UP/CARREGANDO" : "DOWN/DESCONECTADO", stdby_level);
    }

    // Se o pino 12 estiver em UP (ou a bateria estiver >= 4.16V enquanto conectada), carga cheia
    if (stdby_level == 1 || (chrg_level == 1 && s_cached_mv >= 4160)) {
        return BATTERY_FULL;
    }

    // Quando o pino 11 estiver em UP (1), esta carregando
    if (chrg_level == 1) {
        return BATTERY_CHARGING;
    }

    // Quando nao estiver em UP (0), nao esta carregando (descarregando na bateria)
    return BATTERY_DISCHARGING;
}

bool battery_is_charging(void)
{
    return battery_get_status() == BATTERY_CHARGING;
}

bool battery_is_full(void)
{
    return battery_get_status() == BATTERY_FULL;
}

void battery_get_info(int* mv, int* percent, int* time_left_mins)
{
    int pct = battery_get_percent();
    if (percent) *percent = pct;
    if (mv) *mv = s_cached_mv;
    if (time_left_mins) *time_left_mins = s_cached_time_left;
}

static void update_battery_history(int percent, int64_t now)
{
    battery_status_t status = battery_get_status();
    if (status == BATTERY_FULL) {
        s_cached_time_left = 0;
    } else if (status == BATTERY_CHARGING) {
        s_cached_time_left = (s_avg_charge_rate > 0.001f) ? (int)((100.0f - percent) / s_avg_charge_rate) : -1;
    } else {
        s_cached_time_left = (s_avg_drain_rate > 0.001f) ? (int)(percent / s_avg_drain_rate) : -1;
    }

    if (s_last_saved_time_us == 0 || s_last_saved_percent < 0) {
        s_last_saved_time_us = now;
        s_last_saved_percent = percent;
        return;
    }

    int dp = percent - s_last_saved_percent;
    int abs_dp = dp > 0 ? dp : -dp;
    int64_t dt_us = now - s_last_saved_time_us;
    float dt_min = dt_us / 60000000.0f;

    // Atualiza taxas de consumo se houver diferenca significativa (>= 2%) por pelo menos 5 min
    if (abs_dp >= 2 && dt_min >= 5.0f) {
        float current_rate = (float)abs_dp / dt_min;
        nvs_handle_t h;
        if (nvs_open("mps3_settings", NVS_READWRITE, &h) == ESP_OK) {
            if (dp < 0 && current_rate > 0.01f && current_rate < 5.0f) { // Descarregando
                s_avg_drain_rate = (s_avg_drain_rate * 0.8f) + (current_rate * 0.2f);
                nvs_set_i32(h, "bat_drain_rate", (int32_t)(s_avg_drain_rate * 10000.0f));
            } else if (dp > 0 && current_rate > 0.01f && current_rate < 10.0f) { // Carregando
                s_avg_charge_rate = (s_avg_charge_rate * 0.8f) + (current_rate * 0.2f);
                nvs_set_i32(h, "bat_charge_rate", (int32_t)(s_avg_charge_rate * 10000.0f));
            }
            nvs_commit(h);
            nvs_close(h);
        }
        s_last_saved_time_us = now;
        s_last_saved_percent = percent;
    }
}

// Curva OCV nao-linear para celulas Li-Ion / LiPo (3.20V a 4.18V)
static int voltage_to_percent(int mv)
{
    struct { int mv; int pct; } curve[] = {
        {4180, 100},
        {4050, 90},
        {3900, 75},
        {3780, 60},
        {3680, 45},
        {3580, 25},
        {3480, 10},
        {3350, 3},
        {3200, 0},
    };
    const int n = sizeof(curve) / sizeof(curve[0]);

    if (mv >= curve[0].mv) return 100;
    if (mv <= curve[n - 1].mv) return 0;

    for (int i = 0; i < n - 1; i++) {
        if (mv <= curve[i].mv && mv >= curve[i + 1].mv) {
            int mv_span  = curve[i].mv - curve[i + 1].mv;
            int pct_span = curve[i].pct - curve[i + 1].pct;
            int mv_off   = mv - curve[i + 1].mv;
            return curve[i + 1].pct + (mv_off * pct_span) / mv_span;
        }
    }
    return 0;
}

#define BATTERY_OVERSAMPLE_COUNT 128
#define BATTERY_CACHE_INTERVAL_US (3 * 1000 * 1000) // 3 segundos de cache

static int s_cached_percent = -1;
static int64_t s_last_read_us = 0;

static int battery_read_percent_now(void)
{
    int64_t raw_sum = 0;
    for (int i = 0; i < BATTERY_OVERSAMPLE_COUNT; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc_handle, s_adc_channel, &raw) != ESP_OK) {
            return -1;
        }
        raw_sum += raw;
    }
    int raw_avg = (int)(raw_sum / BATTERY_OVERSAMPLE_COUNT);

    int mv_at_pin;
    if (s_cali_enabled) {
        if (adc_cali_raw_to_voltage(s_cali_handle, raw_avg, &mv_at_pin) != ESP_OK) {
            return -1;
        }
    } else {
        mv_at_pin = (raw_avg * 3300) / 4095;
    }

    int battery_mv = (int)(mv_at_pin * BATTERY_DIVIDER_RATIO);
    static int s_filtered_mv = 0;
    if (s_filtered_mv == 0) {
        s_filtered_mv = battery_mv;
    } else {
        s_filtered_mv = (s_filtered_mv * 3 + battery_mv) / 4;
    }
    s_cached_mv = s_filtered_mv;

    battery_status_t status = battery_get_status();
    s_is_charging = (status == BATTERY_CHARGING);
    bool is_full = (status == BATTERY_FULL);

    int pct = voltage_to_percent(s_filtered_mv);

    if (is_full) {
        pct = 100;
    } else if (!s_is_charging && s_cached_percent >= 0) {
        // Na descarga normal, a porcentagem nao sobe por flutuacoes momentaneas de ruido
        if (pct > s_cached_percent) {
            pct = s_cached_percent;
        }
    }

    return pct;
}

int battery_get_percent(void)
{
    if (!s_initialized) return -1;

    int64_t now_us = esp_timer_get_time();
    if (s_last_read_us != 0 && (now_us - s_last_read_us) < BATTERY_CACHE_INTERVAL_US) {
        return s_cached_percent;
    }

    int percent = battery_read_percent_now();
    if (percent >= 0) {
        s_cached_percent = percent;
        s_last_read_us = now_us;
        update_battery_history(percent, now_us);
        return percent;
    }
    return s_cached_percent;
}

