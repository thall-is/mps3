#ifndef PCF8574_H
#define PCF8574_H

/**
 * @file pcf8574.h
 * @brief Driver I2C para o Expansor de I/O de 8-Bit PCF8574 / PCF8574A
 *
 * Suporta auto-detecção em toda a faixa de endereços (0x20..0x27 e 0x38..0x3F),
 * compartilhando o barramento I2C mestre do ESP32-S3 (SDA=GPIO 10, SCL=GPIO 9).
 * Opera no modo de portas quasi-bidirecionais ativas em nível baixo (GND).
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PCF8574_DEFAULT_ADDR   0x20
#define PCF8574A_DEFAULT_ADDR  0x38

/**
 * @brief Inicializa o PCF8574 no barramento I2C fornecido.
 *
 * Varre as faixas 0x20..0x27 e 0x38..0x3F para encontrar o dispositivo.
 * Ao encontrar, registra o dispositivo e envia 0xFF para configurar todos
 * os pinos P0..P7 como entradas com pull-up interno.
 *
 * @param bus_handle Handle do barramento I2C mestre (u8g2_hal_get_bus_handle()).
 * @return ESP_OK se o expansor foi detectado e configurado com sucesso.
 */
esp_err_t pcf8574_init(i2c_master_bus_handle_t bus_handle);

/**
 * @brief Retorna se o PCF8574 está presente e respondendo no barramento.
 */
bool pcf8574_is_available(void);

/**
 * @brief Retorna o endereço I2C de 7 bits detectado durante o init (ex: 0x20 ou 0x38).
 */
uint8_t pcf8574_get_address(void);

/**
 * @brief Lê o byte bruto das 8 portas do PCF8574 (P0..P7).
 *
 * Cada bit representa uma porta:
 * - 1: Pino em nível alto (Solto / Pull-up interno)
 * - 0: Pino em nível baixo (Pressionado para GND)
 *
 * @param[out] val Ponteiro para armazenar o byte lido.
 * @return ESP_OK ou erro I2C.
 */
esp_err_t pcf8574_read_raw(uint8_t *val);

/**
 * @brief Escreve um byte de saída no PCF8574.
 *
 * @param val Valor a transmitir.
 * @return ESP_OK ou erro I2C.
 */
esp_err_t pcf8574_write_raw(uint8_t val);

/**
 * @brief Verifica se um pino específico (0 a 7) está pressionado (nível baixo / GND).
 *
 * @param pin Número do pino (0 a 7).
 * @return true se o botão estiver pressionado para o GND, false se solto ou erro.
 */
bool pcf8574_get_pin_pressed(uint8_t pin);

#ifdef __cplusplus
}
#endif

#endif // PCF8574_H

