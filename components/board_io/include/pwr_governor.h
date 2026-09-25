#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PWR_FREQ_40MHZ  = 40,
    PWR_FREQ_80MHZ  = 80,
    PWR_FREQ_160MHZ = 160,
    PWR_FREQ_240MHZ = 240,
} pwr_freq_t;

/**
 * @brief Inicializa o subsistema de gerenciamento de clock e energia.
 */
esp_err_t pwr_governor_init(void);

/**
 * @brief Comuta a frequência de operação da CPU dinamicamente (80, 160 ou 240 MHz).
 * 
 * @param freq Frequência alvo em MHz.
 * @return esp_err_t ESP_OK em caso de sucesso.
 */
esp_err_t pwr_governor_set_cpu_freq(pwr_freq_t freq);

/**
 * @brief Retorna a frequência de operação atual da CPU.
 */
pwr_freq_t pwr_governor_get_cpu_freq(void);

/**
 * @brief Avalia o estado geral do sistema e seleciona o perfil de clock ideal:
 * - Tela acordada (Navegação/UI): 160 MHz
 * - Tela em repouso/bloqueada (MP3/WAV/AAC padrão <= 48kHz ou Pausado): 80 MHz
 * - Tela em repouso/bloqueada (FLAC Hi-Res 96k): 160 MHz
 * - Hi-Res Extremo (FLAC 24/192k) ou Wi-Fi ativo ou Seek: 240 MHz
 */
void pwr_governor_update(bool screen_active, bool wifi_active, bool seeking, bool playing, uint32_t sample_rate);

/**
 * @brief Ativa ou desativa o modo manual de clock (trava a frequência para medição na bancada).
 */
void pwr_governor_set_manual_mode(bool manual);

/**
 * @brief Retorna se o modo manual está ativo.
 */
bool pwr_governor_is_manual_mode(void);

/**
 * @brief Entra em modo Light Sleep (consumo reduzido ~2-3 mA) com retorno via interrupção
 * nos pinos do joystick (qualquer tecla) ou inserção do carregador (GPIO 11) ou timer.
 * 
 * @param timeout_ms Tempo máximo de repouso em milissegundos (0 = sem timer).
 * @return esp_err_t ESP_OK após acordar.
 */
esp_err_t pwr_governor_enter_light_sleep(uint32_t timeout_ms);

typedef void (*pwr_sleep_cb_t)(void);

/**
 * @brief Registra callback invocado antes de entrar em Deep Sleep (ex: desligar display).
 */
void pwr_governor_set_sleep_cb(pwr_sleep_cb_t cb);

/**
 * @brief Entra em modo Deep Sleep definitivo (< 100 uA).
 * Desliga periféricos e configura despertar via RTC nos pinos JOY_UP (GPIO 2) e BATTERY_CHRG (GPIO 11).
 * Não retorna (reinicia a CPU via bootloader ao acordar).
 */
void pwr_governor_enter_deep_sleep(void);

#ifdef __cplusplus
}
#endif
