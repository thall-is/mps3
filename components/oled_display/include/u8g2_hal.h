#ifndef U8G2_HAL_H
#define U8G2_HAL_H

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

// Inicializa o barramento I2C (driver moderno i2c_master, mesmo usado no
// resto do projeto) e prepara os callbacks u8x8 abaixo. Chame ANTES de
// qualquer u8g2_Setup_*.
esp_err_t u8g2_hal_i2c_init(int sda_gpio, int scl_gpio, uint8_t i2c_addr_7bit);

// Retorna o handle do barramento mestre I2C criado pelo u8g2_hal.
// Permite que outros dispositivos no mesmo barramento (ex: RTC DS3231) compartilhem o barramento.
i2c_master_bus_handle_t u8g2_hal_get_bus_handle(void);

// Sondagem rapida (probe) de um endereco I2C de 7 bits no barramento ativo.
esp_err_t u8g2_hal_i2c_probe(uint16_t addr, int timeout_ms);

// Callbacks exigidos pela assinatura de u8g2_Setup_ssd1306_i2c_128x64_noname_f.
uint8_t u8g2_hal_byte_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr);
uint8_t u8g2_hal_gpio_and_delay_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr);

#ifdef __cplusplus
}
#endif

#endif // U8G2_HAL_H

