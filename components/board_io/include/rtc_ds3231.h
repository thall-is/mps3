#ifndef RTC_DS3231_H
#define RTC_DS3231_H

/**
 * @file rtc_ds3231.h
 * @brief Driver para Relógio de Tempo Real (RTC) Maxim/Analog Devices DS3231
 *
 * Fornece interface de comunicação I2C mestre para o chip DS3231 (endereço 0x68),
 * integrado no mesmo barramento I2C do display OLED SSD1306 (SDA GPIO 10, SCL GPIO 9).
 * Realiza conversões BCD, leitura de temperatura interna com compensação TCXO
 * e sincronização automática da pilha de tempo POSIX do ESP-IDF (`settimeofday`).
 */

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DS3231_I2C_ADDR   0x68 /**< Endereço I2C de 7 bits do RTC DS3231. */
#define AT24C32_I2C_ADDR  0x57 /**< Endereço I2C da memória EEPROM auxiliar AT24C32. */

/**
 * @brief Verifica se o DS3231 responde no barramento I2C fornecido.
 *
 * @param[in] bus_handle Handle do barramento I2C mestre já inicializado.
 *
 * @return true se o dispositivo respondeu ACK no endereço 0x68; false caso contrário.
 */
bool rtc_ds3231_detect(i2c_master_bus_handle_t bus_handle);

/**
 * @brief Inicializa o driver do DS3231 registrando o dispositivo no barramento I2C.
 *
 * Verifica o oscilador interno e limpa a flag de parada do oscilador (OSF) se necessário.
 *
 * @param[in] bus_handle Handle do barramento I2C mestre já inicializado.
 *
 * @return 
 *   - ESP_OK: Dispositivo registrado e operacional.
 *   - ESP_FAIL: Falha de comunicação ou dispositivo não detectado.
 */
esp_err_t rtc_ds3231_init(i2c_master_bus_handle_t bus_handle);

/**
 * @brief Retorna se o DS3231 foi detectado e está inicializado com sucesso.
 *
 * @return true se o RTC está disponível e pronto para uso; false caso contrário.
 */
bool rtc_ds3231_is_available(void);

/**
 * @brief Lê a data e hora atuais gravadas nos registradores do DS3231.
 *
 * @param[out] timeinfo Ponteiro para struct tm do POSIX que receberá os campos decodificados.
 *
 * @return ESP_OK se a leitura foi realizada com sucesso.
 */
esp_err_t rtc_ds3231_get_time(struct tm *timeinfo);

/**
 * @brief Grava uma nova data e hora nos registradores BCD do DS3231.
 *
 * @param[in] timeinfo Ponteiro para struct tm contendo os novos valores de data e hora.
 *
 * @return ESP_OK se gravado com sucesso.
 */
esp_err_t rtc_ds3231_set_time(const struct tm *timeinfo);

/**
 * @brief Lê a temperatura interna do sensor compensado por temperatura (TCXO) do DS3231.
 *
 * @param[out] temp_c Ponteiro para float onde será gravada a temperatura em graus Celsius (°C).
 *
 * @return ESP_OK se lido com sucesso.
 */
esp_err_t rtc_ds3231_get_temperature(float *temp_c);

/**
 * @brief Sincroniza o relógio do sistema operacional (`settimeofday`) a partir do DS3231.
 *
 * Após a execução desta função, chamadas da biblioteca C padrão (`time()`, `localtime()`)
 * e o sistema de arquivos FATFS utilizam carimbos de data e hora reais nos arquivos do SD.
 *
 * @return ESP_OK se sincronizado com sucesso.
 */
esp_err_t rtc_ds3231_sync_system_time(void);

/**
 * @brief Informações detalhadas de data, hora e telemetria lidas do RTC DS3231.
 */
typedef struct {
    int hour;         /**< Horas (00 a 23). */
    int min;          /**< Minutos (00 a 59). */
    int sec;          /**< Segundos (00 a 59). */
    int millis;       /**< Milissegundos decorridos do segundo atual (0 a 999 ms). */
    int day;          /**< Dia do mês (01 a 31). */
    int month;        /**< Mês (01 a 12). */
    int year;         /**< Ano completo (ex: 2026). */
    int wday;         /**< Dia da semana (0 = Domingo a 6 = Sábado). */
    float temp_c;     /**< Temperatura interna do TCXO em °C (resolução nativa de 0,25 °C). */
    bool rtc_ok;      /**< true se a comunicação I2C com o DS3231 teve sucesso. */
    bool osc_stopped; /**< true se o bit OSF estiver ativo (troca de bateria / oscilador parado). */
} rtc_clock_info_t;

/**
 * @brief Obtém data, hora de alta resolução (com milissegundos) e telemetria de temperatura do DS3231.
 *
 * Realiza a leitura direta dos registradores do RTC via I2C, rastreia a transição do segundo
 * e interpola os milissegundos para atualização fluida do relógio na interface gráfica.
 *
 * @param[out] info Ponteiro para estrutura rtc_clock_info_t que receberá os dados.
 *
 * @return ESP_OK se lido com sucesso; código de erro caso contrário.
 */
esp_err_t rtc_ds3231_get_clock_info(rtc_clock_info_t *info);

#ifdef __cplusplus
}
#endif

#endif // RTC_DS3231_H
