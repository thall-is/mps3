#ifndef BATTERY_H
#define BATTERY_H

/**
 * @file battery.h
 * @brief Monitoramento de Tensão Li-Ion e Status de Carga TP4056
 *
 * Realiza a leitura analógica da bateria através do ADC1 (GPIO 1) com calibração
 * de fábrica e mapeamento de curva de descarga Li-Ion (3.2V a 4.2V). Monitora o
 * pino CHRG do módulo TP4056 (GPIO 11) para detecção de cabo de carga conectado.
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Estados operacionais da bateria e do carregador.
 */
typedef enum {
    BATTERY_DISCHARGING = 0, /**< Operação normal em bateria (cabo USB de carga desconectado). */
    BATTERY_CHARGING    = 1, /**< Em processo de carga (cabo conectado, pino CHRG em nível LOW). */
    BATTERY_FULL        = 2, /**< Carga completa concluída (tensão >= 4.18V com cabo conectado). */
} battery_status_t;

/**
 * @brief Inicializa o canal ADC1 One-Shot e configura o pino GPIO de monitoramento do TP4056.
 *
 * @return 
 *   - ESP_OK: Subsistema de bateria inicializado com sucesso.
 *   - ESP_FAIL: Falha ao calibrar o periférico ADC.
 */
esp_err_t battery_init(void);

/**
 * @brief Retorna o percentual estimado de carga da bateria.
 *
 * @return Valor de 0 a 100%.
 */
int battery_get_percent(void);

/**
 * @brief Obtém dados detalhados de telemetria da bateria.
 *
 * @param[out] mv              Ponteiro para preenchimento da tensão medida em milivolts (mV).
 * @param[out] percent         Ponteiro para preenchimento da porcentagem estimada (0 a 100%).
 * @param[out] time_left_mins  Ponteiro para estimativa de autonomia restante em minutos (-1 se carregando).
 */
void battery_get_info(int *mv, int *percent, int *time_left_mins);

/**
 * @brief Informa se a bateria está sendo carregada no momento.
 *
 * @return true se o carregador estiver plugado e ativo; false caso contrário.
 */
bool battery_is_charging(void);

/**
 * @brief Informa se a carga da bateria atingiu 100%.
 *
 * @return true se a bateria estiver totalmente carregada; false caso contrário.
 */
bool battery_is_full(void);

/**
 * @brief Retorna o estado operacional completo da bateria.
 *
 * @return Enum `battery_status_t` (`BATTERY_DISCHARGING`, `BATTERY_CHARGING` ou `BATTERY_FULL`).
 */
battery_status_t battery_get_status(void);

#ifdef __cplusplus
}
#endif

#endif // BATTERY_H
