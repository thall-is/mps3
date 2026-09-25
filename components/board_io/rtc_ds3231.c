#include "rtc_ds3231.h"

#include <string.h>
#include <sys/time.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

static const char *TAG = "ds3231";

static i2c_master_bus_handle_t s_bus_handle = NULL;
static i2c_master_dev_handle_t s_rtc_dev = NULL;
static bool s_available = false;

static inline uint8_t bcd_to_dec(uint8_t val)
{
    return (uint8_t)(((val >> 4) * 10) + (val & 0x0F));
}

static inline uint8_t dec_to_bcd(uint8_t val)
{
    return (uint8_t)(((val / 10) << 4) | (val % 10));
}

bool rtc_ds3231_detect(i2c_master_bus_handle_t bus_handle)
{
    if (!bus_handle) {
        return false;
    }
    esp_err_t err = i2c_master_probe(bus_handle, DS3231_I2C_ADDR, 50);
    return (err == ESP_OK);
}

esp_err_t rtc_ds3231_init(i2c_master_bus_handle_t bus_handle)
{
    if (!bus_handle) {
        ESP_LOGE(TAG, "Bus handle I2C nulo fornecido ao DS3231");
        return ESP_ERR_INVALID_ARG;
    }
    s_bus_handle = bus_handle;

    if (!rtc_ds3231_detect(bus_handle)) {
        ESP_LOGW(TAG, "DS3231 nao respondeu no endereco 0x%02X", DS3231_I2C_ADDR);
        s_available = false;
        return ESP_ERR_NOT_FOUND;
    }

    if (!s_rtc_dev) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = DS3231_I2C_ADDR,
            .scl_speed_hz = 400000,
        };
        esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &s_rtc_dev);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao registrar dispositivo DS3231 no barramento I2C: %s", esp_err_to_name(ret));
            s_available = false;
            return ret;
        }
    }

    // Le registradores de controle e status (0x0E e 0x0F)
    uint8_t reg_addr = 0x0E;
    uint8_t status_buf[2] = {0};
    esp_err_t err = i2c_master_transmit_receive(s_rtc_dev, &reg_addr, 1, status_buf, 2, 100);
    if (err == ESP_OK) {
        uint8_t ctrl = status_buf[0];
        uint8_t stat = status_buf[1];

        // Se bit 7 (OSF) de 0x0F estiver setado, oscilador parou (bateria trocada ou inicializacao)
        if (stat & 0x80) {
            ESP_LOGW(TAG, "DS3231: Oscillator Stop Flag (OSF) ativo. Hora pode necessitar ajuste.");
            // Limpa OSF
            uint8_t clear_stat[2] = {0x0F, (uint8_t)(stat & ~0x80)};
            i2c_master_transmit(s_rtc_dev, clear_stat, 2, 100);
        }

        // Garante que EOSC (bit 7 de 0x0E) e' 0 para que o oscilador continue rodando na bateria
        if (ctrl & 0x80) {
            uint8_t enable_osc[2] = {0x0E, (uint8_t)(ctrl & ~0x80)};
            i2c_master_transmit(s_rtc_dev, enable_osc, 2, 100);
            ESP_LOGI(TAG, "DS3231: Oscilador reabilitado (EOSC limpo).");
        }
    }

    s_available = true;
    ESP_LOGI(TAG, "DS3231 inicializado com sucesso (I2C addr 0x%02X)", DS3231_I2C_ADDR);
    return ESP_OK;
}

bool rtc_ds3231_is_available(void)
{
    return s_available;
}

esp_err_t rtc_ds3231_get_time(struct tm *timeinfo)
{
    if (!s_available || !s_rtc_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!timeinfo) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t reg_addr = 0x00;
    uint8_t raw[7] = {0};
    esp_err_t err = i2c_master_transmit_receive(s_rtc_dev, &reg_addr, 1, raw, 7, 100);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao ler registradores de tempo do DS3231: %s", esp_err_to_name(err));
        return err;
    }

    memset(timeinfo, 0, sizeof(struct tm));
    timeinfo->tm_sec = bcd_to_dec(raw[0] & 0x7F);
    timeinfo->tm_min = bcd_to_dec(raw[1] & 0x7F);

    // Trata formato 24h ou 12h
    if (raw[2] & 0x40) {
        // Modo 12h: bit 5 e' AM/PM (1 = PM)
        uint8_t hour = bcd_to_dec(raw[2] & 0x1F);
        if (raw[2] & 0x20) {
            if (hour < 12) hour += 12;
        } else {
            if (hour == 12) hour = 0;
        }
        timeinfo->tm_hour = hour;
    } else {
        // Modo 24h
        timeinfo->tm_hour = bcd_to_dec(raw[2] & 0x3F);
    }

    timeinfo->tm_wday = (raw[3] & 0x07) - 1; // 1-7 convertido para 0-6
    if (timeinfo->tm_wday < 0) timeinfo->tm_wday = 0;

    timeinfo->tm_mday = bcd_to_dec(raw[4] & 0x3F);

    // Mes (1-12) -> tm_mon (0-11)
    uint8_t month = bcd_to_dec(raw[5] & 0x1F);
    timeinfo->tm_mon = (month > 0) ? (month - 1) : 0;

    // Ano (00-99)
    uint8_t year_2digit = bcd_to_dec(raw[6]);
    bool century = (raw[5] & 0x80) != 0;
    int full_year = 2000 + year_2digit + (century ? 100 : 0);
    timeinfo->tm_year = full_year - 1900; // Anos desde 1900

    return ESP_OK;
}

esp_err_t rtc_ds3231_set_time(const struct tm *timeinfo)
{
    if (!s_available || !s_rtc_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!timeinfo) {
        return ESP_ERR_INVALID_ARG;
    }

    int full_year = timeinfo->tm_year + 1900;
    bool century = (full_year >= 2100);
    uint8_t year_2digit = (uint8_t)(full_year % 100);
    uint8_t month = (uint8_t)(timeinfo->tm_mon + 1);
    uint8_t wday = (uint8_t)(timeinfo->tm_wday + 1); // 1-7

    uint8_t tx[8];
    tx[0] = 0x00; // Registrador inicial
    tx[1] = dec_to_bcd((uint8_t)timeinfo->tm_sec) & 0x7F;
    tx[2] = dec_to_bcd((uint8_t)timeinfo->tm_min) & 0x7F;
    tx[3] = dec_to_bcd((uint8_t)timeinfo->tm_hour) & 0x3F; // Modo 24h (bit 6 = 0)
    tx[4] = dec_to_bcd(wday) & 0x07;
    tx[5] = dec_to_bcd((uint8_t)timeinfo->tm_mday) & 0x3F;
    tx[6] = (uint8_t)((dec_to_bcd(month) & 0x1F) | (century ? 0x80 : 0x00));
    tx[7] = dec_to_bcd(year_2digit);

    esp_err_t err = i2c_master_transmit(s_rtc_dev, tx, 8, 100);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao gravar tempo no DS3231: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Hora atualizada no DS3231: %04d-%02d-%02d %02d:%02d:%02d",
             full_year, month, timeinfo->tm_mday,
             timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
    return ESP_OK;
}

esp_err_t rtc_ds3231_get_temperature(float *temp_c)
{
    if (!s_available || !s_rtc_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!temp_c) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t reg_addr = 0x11;
    uint8_t raw[2] = {0};
    esp_err_t err = i2c_master_transmit_receive(s_rtc_dev, &reg_addr, 1, raw, 2, 100);
    if (err != ESP_OK) {
        return err;
    }

    int8_t msb = (int8_t)raw[0];
    uint8_t lsb = (uint8_t)(raw[1] >> 6); // Bits 7-6
    *temp_c = (float)msb + ((float)lsb * 0.25f);
    return ESP_OK;
}

esp_err_t rtc_ds3231_sync_system_time(void)
{
    struct tm tm_val;
    esp_err_t err = rtc_ds3231_get_time(&tm_val);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Falha ao ler DS3231 para sincronizacao do sistema: %s", esp_err_to_name(err));
        return err;
    }

    time_t t = mktime(&tm_val);
    if (t == (time_t)-1) {
        ESP_LOGE(TAG, "Data invalida retornada pelo DS3231 para mktime");
        return ESP_FAIL;
    }

    struct timeval tv = {
        .tv_sec = t,
        .tv_usec = 0
    };
    settimeofday(&tv, NULL);

    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_val);
    ESP_LOGI(TAG, "Horario do sistema POSIX sincronizado com RTC: %s", buf);
    return ESP_OK;
}

esp_err_t rtc_ds3231_get_clock_info(rtc_clock_info_t *info)
{
    if (!info) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(info, 0, sizeof(rtc_clock_info_t));

    if (!s_available || !s_rtc_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    static int s_last_sec = -1;
    static int64_t s_last_tick_us = 0;

    // Le registradores de tempo (0x00 a 0x06)
    uint8_t reg_addr = 0x00;
    uint8_t raw[7] = {0};
    esp_err_t err = i2c_master_transmit_receive(s_rtc_dev, &reg_addr, 1, raw, 7, 100);
    if (err != ESP_OK) {
        return err;
    }

    int sec = bcd_to_dec(raw[0] & 0x7F);
    int min = bcd_to_dec(raw[1] & 0x7F);
    int hour;
    if (raw[2] & 0x40) {
        hour = bcd_to_dec(raw[2] & 0x1F);
        if (raw[2] & 0x20) {
            if (hour < 12) hour += 12;
        } else {
            if (hour == 12) hour = 0;
        }
    } else {
        hour = bcd_to_dec(raw[2] & 0x3F);
    }

    int wday = (raw[3] & 0x07) - 1;
    if (wday < 0) wday = 0;
    int day = bcd_to_dec(raw[4] & 0x3F);
    int month = bcd_to_dec(raw[5] & 0x1F);
    int year_2digit = bcd_to_dec(raw[6]);
    bool century = (raw[5] & 0x80) != 0;
    int year = 2000 + year_2digit + (century ? 100 : 0);

    int64_t now_us = esp_timer_get_time();
    if (sec != s_last_sec) {
        s_last_sec = sec;
        s_last_tick_us = now_us;
    }

    int millis = 0;
    if (s_last_tick_us > 0) {
        int64_t delta_ms = (now_us - s_last_tick_us) / 1000;
        if (delta_ms < 0) delta_ms = 0;
        if (delta_ms > 999) delta_ms = 999;
        millis = (int)delta_ms;
    }

    // Le temperatura (0x11 e 0x12)
    float temp_c = 0.0f;
    uint8_t temp_reg = 0x11;
    uint8_t temp_raw[2] = {0};
    if (i2c_master_transmit_receive(s_rtc_dev, &temp_reg, 1, temp_raw, 2, 50) == ESP_OK) {
        int8_t msb = (int8_t)temp_raw[0];
        uint8_t lsb = (uint8_t)(temp_raw[1] >> 6);
        temp_c = (float)msb + ((float)lsb * 0.25f);
    }

    // Le status (0x0F) para verificar Oscillator Stop Flag (OSF)
    bool osc_stopped = false;
    uint8_t stat_reg = 0x0F;
    uint8_t stat_val = 0;
    if (i2c_master_transmit_receive(s_rtc_dev, &stat_reg, 1, &stat_val, 1, 50) == ESP_OK) {
        osc_stopped = (stat_val & 0x80) != 0;
    }

    info->hour = hour;
    info->min = min;
    info->sec = sec;
    info->millis = millis;
    info->day = day;
    info->month = month;
    info->year = year;
    info->wday = wday;
    info->temp_c = temp_c;
    info->rtc_ok = true;
    info->osc_stopped = osc_stopped;
    return ESP_OK;
}

