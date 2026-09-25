#pragma once

/**
 * @file rgb_led.h
 * @brief Driver de Controle do LED RGB Inteligente WS2812 On-Board
 *
 * Utiliza o periférico RMT (Remote Control) do ESP32-S3 para gerar os pulsos
 * de alta precisão de 800 kHz no GPIO 38, controlando cor (RGB) e intensidade
 * luminosa com persistência das preferências na NVS.
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Estrutura de configuração do LED RGB.
 */
typedef struct {
    uint8_t r;     /**< Intensidade do canal Vermelho (0 a 255). */
    uint8_t g;     /**< Intensidade do canal Verde (0 a 255). */
    uint8_t b;     /**< Intensidade do canal Azul (0 a 255). */
    bool enabled;  /**< Estado ligado (`true`) ou desligado (`false`). */
} rgb_led_config_t;

/**
 * @brief Inicializa o canal RMT e configura os temporizadores de bit do WS2812.
 *
 * @return ESP_OK se o periférico RMT foi inicializado com sucesso.
 */
esp_err_t rgb_led_init(void);

/**
 * @brief Define imediatamente as cores R, G, B do LED sem persistir na NVS.
 *
 * @param[in] r Intensidade vermelha (0-255).
 * @param[in] g Intensidade verde (0-255).
 * @param[in] b Intensidade azul (0-255).
 */
void rgb_led_set_color(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Obtém uma cópia da configuração atual do LED RGB.
 *
 * @param[out] out_cfg Ponteiro para a estrutura `rgb_led_config_t` de saída.
 */
void rgb_led_get_config(rgb_led_config_t *out_cfg);

/**
 * @brief Aplica uma nova estrutura de configuração ao LED RGB.
 *
 * @param[in] cfg Ponteiro para a configuração desejada.
 */
void rgb_led_set_config(const rgb_led_config_t *cfg);

/**
 * @brief Alterna o estado do LED entre ligado e desligado (*toggle*).
 */
void rgb_led_toggle(void);

/**
 * @brief Salva a configuração atual de cores e estado do LED na memória NVS.
 */
void rgb_led_save_nvs(void);

#ifdef __cplusplus
}
#endif
