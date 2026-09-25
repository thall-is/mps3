#ifndef RTC_DS3231_H
#define RTC_DS3231_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DS3231_I2C_ADDR   0x68
#define AT24C32_I2C_ADDR  0x57

/**
 * @brief Verifica se o DS3231 responde no barramento I2C fornecido.
 *
 * @param bus_handle Handle do barramento I2C mestre já inicializado.
 * @return true se o dispositivo respondeu ACK no endereço 0x68.
 */
bool rtc_ds3231_detect(i2c_master_bus_handle_t bus_handle);

/**
 * @brief Inicializa o driver do DS3231 registrando o dispositivo no barramento I2C.
 *        Verifica o oscilador e limpa a flag de parada (OSF) se necessário.
 *
 * @param bus_handle Handle do barramento I2C mestre já inicializado.
 * @return ESP_OK em caso de sucesso.
 */
esp_err_t rtc_ds3231_init(i2c_master_bus_handle_t bus_handle);

/**
 * @brief Retorna se o DS3231 foi detectado e está inicializado com sucesso.
 */
bool rtc_ds3231_is_available(void);

/**
 * @brief Lê a data e hora atuais do DS3231.
 *
 * @param timeinfo Ponteiro para struct tm do POSIX que receberá a data e hora.
 * @return ESP_OK se lido com sucesso.
 */
esp_err_t rtc_ds3231_get_time(struct tm *timeinfo);

/**
 * @brief Grava uma nova data e hora no DS3231.
 *
 * @param timeinfo Ponteiro para struct tm contendo os novos valores.
 * @return ESP_OK se gravado com sucesso.
 */
esp_err_t rtc_ds3231_set_time(const struct tm *timeinfo);

/**
 * @brief Lê a temperatura interna do sensor compensado do DS3231.
 *
 * @param temp_c Ponteiro para float que receberá a temperatura em °C.
 * @return ESP_OK se lido com sucesso.
 */
esp_err_t rtc_ds3231_get_temperature(float *temp_c);

/**
 * @brief Sincroniza o relógio do sistema operacional (settimeofday) a partir do DS3231.
 *        Após esta chamada, funções libc padrão (time, localtime) e o FATFS
 *        usarão o horário real para data/hora de arquivos.
 *
 * @return ESP_OK se sincronizado com sucesso.
 */
esp_err_t rtc_ds3231_sync_system_time(void);

#ifdef __cplusplus
}
#endif

#endif // RTC_DS3231_H

