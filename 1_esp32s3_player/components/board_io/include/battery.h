#ifndef BATTERY_H
#define BATTERY_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BATTERY_DISCHARGING = 0, // Descarregando (uso normal, cabo desconectado)
    BATTERY_CHARGING    = 1, // Carregando (cabo conectado, LED vermelho do TP4056 ativo)
    BATTERY_FULL        = 2, // Carga completa (cabo conectado, LED verde/azul do TP4056 ativo)
} battery_status_t;

esp_err_t battery_init(void);
int battery_get_percent(void);
void battery_get_info(int* mv, int* percent, int* time_left_mins);
bool battery_is_charging(void);
bool battery_is_full(void);
battery_status_t battery_get_status(void);

#ifdef __cplusplus
}
#endif

#endif // BATTERY_H

