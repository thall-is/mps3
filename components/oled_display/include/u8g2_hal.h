#ifndef U8G2_HAL_H
#define U8G2_HAL_H

/**
 * @file u8g2_hal.h
 * @brief Camada de Abstração de Hardware (HAL) I2C para a Biblioteca Gráfica U8g2
 *
 * Implementa a ponte de comunicação entre as rotinas de baixo nível da biblioteca
 * U8g2/U8x8 e o driver mestre I2C moderno do ESP-IDF (`esp_driver_i2c_master`),
 * fornecendo suporte a compartilhamento seguro do barramento físico com outros periféricos
 * (como o RTC DS3231).
 */

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa o barramento I2C mestre e registra o dispositivo SSD1306.
 *
 * Deve ser invocada antes de qualquer chamada a rotinas de setup do U8g2.
 *
 * @param[in] sda_gpio        Número do pino GPIO para SDA (ex: 10).
 * @param[in] scl_gpio        Número do pino GPIO para SCL (ex: 9).
 * @param[in] i2c_addr_7bit   Endereço I2C de 7 bits do display (geralmente 0x3C).
 *
 * @return 
 *   - ESP_OK: Barramento e dispositivo I2C inicializados com sucesso.
 *   - ESP_FAIL: Falha ao criar o barramento ou registrar o dispositivo.
 */
esp_err_t u8g2_hal_i2c_init(int sda_gpio, int scl_gpio, uint8_t i2c_addr_7bit);

/**
 * @brief Retorna o descritor de barramento mestre I2C criado pelo HAL.
 *
 * Permite que outros drivers (como o RTC DS3231) compartilhem o mesmo barramento
 * físico sem colisões e com proteção por mutex.
 *
 * @return Handle do tipo `i2c_master_bus_handle_t`.
 */
i2c_master_bus_handle_t u8g2_hal_get_bus_handle(void);

/**
 * @brief Executa sondagem rápida (*probe*) de um endereço I2C no barramento ativo.
 *
 * @param[in] addr       Endereço I2C de 7 bits a testar (ex: 0x68 para DS3231).
 * @param[in] timeout_ms Tempo limite de espera em milissegundos.
 *
 * @return ESP_OK se o periférico respondeu com ACK.
 */
esp_err_t u8g2_hal_i2c_probe(uint16_t addr, int timeout_ms);

/**
 * @brief Callback de transmissão de bytes exigido pela máquina de estados do U8x8.
 *
 * @param[in,out] u8x8    Instância da estrutura U8x8.
 * @param[in]     msg     Mensagem de controle (envio de dados, comandos, start, stop).
 * @param[in]     arg_int Número de bytes ou parâmetro da mensagem.
 * @param[in]     arg_ptr Ponteiro para o buffer de dados transmitido.
 *
 * @return Código de status retornado ao U8g2 (1 para sucesso).
 */
uint8_t u8g2_hal_byte_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr);

/**
 * @brief Callback de controle de GPIO e temporização exigido pelo U8x8.
 *
 * @param[in,out] u8x8    Instância da estrutura U8x8.
 * @param[in]     msg     Mensagem de temporização (delays em microssegundos ou milissegundos).
 * @param[in]     arg_int Quantidade de tempo ou nível lógico.
 * @param[in]     arg_ptr Ponteiro auxiliar.
 *
 * @return 1 em caso de sucesso.
 */
uint8_t u8g2_hal_gpio_and_delay_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr);

#ifdef __cplusplus
}
#endif

#endif // U8G2_HAL_H
