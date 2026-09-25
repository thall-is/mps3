#pragma once

/**
 * @file pwr_governor.h
 * @brief Governador Dinâmico de Frequência (DFS) e Gerenciamento de Energia do SoC
 *
 * Controla o escalonamento dinâmico de clock da CPU Xtensa LX7 (40, 80, 160, 240 MHz),
 * preservando o PLL base (`BBPLL`) travado e estável durante a reprodução de áudio para
 * não interromper os barramentos I2S e SDMMC. Gerencia os modos de repouso Light Sleep
 * e Deep Sleep (< 100 µA) com acionamento por hardware via GPIO RTC.
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Frequências nominais de operação do núcleo da CPU em MHz.
 */
typedef enum {
    PWR_FREQ_40MHZ  = 40,   /**< Modo de economia extrema (XTAL / divisor reduzido). */
    PWR_FREQ_80MHZ  = 80,   /**< Reprodução padrão (MP3/WAV/FLAC <= 48k) com tela apagada. */
    PWR_FREQ_160MHZ = 160,  /**< Interface gráfica OLED ativa a 25 FPS e equalizador paramétrico. */
    PWR_FREQ_240MHZ = 240,  /**< Transferência Wi-Fi, upload OTA, Seek veloz e FLAC 24/192k extremo. */
} pwr_freq_t;

/**
 * @brief Inicializa o subsistema de gerenciamento de clock e energia.
 *
 * Registra o estado inicial de clock e prepara as fontes de despertar do RTC.
 *
 * @return ESP_OK em caso de sucesso.
 */
esp_err_t pwr_governor_init(void);

/**
 * @brief Comuta a frequência de operação da CPU dinamicamente (40, 80, 160 ou 240 MHz).
 * 
 * @param[in] freq Frequência alvo em MHz.
 *
 * @return 
 *   - ESP_OK: Frequência comutada com sucesso.
 *   - ESP_ERR_INVALID_ARG: Frequência não suportada pelo hardware.
 *
 * @note Thread-safe. Protegido por mutex interno.
 */
esp_err_t pwr_governor_set_cpu_freq(pwr_freq_t freq);

/**
 * @brief Retorna a frequência de operação atual da CPU.
 *
 * @return Frequência atual em MHz (`pwr_freq_t`).
 */
pwr_freq_t pwr_governor_get_cpu_freq(void);

/**
 * @brief Avalia o estado geral do sistema e seleciona o perfil de clock ideal:
 * - Tela acordada (Navegação/UI): 160 MHz.
 * - Tela em repouso/bloqueada (MP3/WAV/AAC padrão <= 48kHz ou Pausado): 80 MHz.
 * - Tela em repouso/bloqueada (FLAC Hi-Res 96k): 160 MHz.
 * - Hi-Res Extremo (FLAC 24/192k) ou Wi-Fi ativo ou Seek: 240 MHz.
 *
 * @param[in] screen_active true se o display OLED estiver ligado/ativo.
 * @param[in] wifi_active   true se o rádio Wi-Fi ou servidor HTTPD estiver ativo.
 * @param[in] seeking       true se há um salto de busca (seek) em processamento.
 * @param[in] playing       true se há decodificação ativa de música no momento.
 * @param[in] sample_rate   Taxa de amostragem da música atual em Hz.
 */
void pwr_governor_update(bool screen_active, bool wifi_active, bool seeking, bool playing, uint32_t sample_rate);

/**
 * @brief Ativa ou desativa o modo manual de clock.
 *
 * Quando ativo, impede ajustes automáticos pelo governador para permitir testes de bancada.
 *
 * @param[in] manual true para travar no clock atual; false para permitir ajuste automático.
 */
void pwr_governor_set_manual_mode(bool manual);

/**
 * @brief Retorna se o modo manual de clock está ativo.
 *
 * @return true se o modo manual estiver ativo; false caso contrário.
 */
bool pwr_governor_is_manual_mode(void);

/**
 * @brief Entra em modo Light Sleep (consumo reduzido ~2-3 mA).
 *
 * Permite despertar imediato através de pulsos no joystick, inserção do carregador (GPIO 11) ou timer.
 * 
 * @param[in] timeout_ms Tempo máximo de repouso em milissegundos (0 = sem timeout de timer).
 *
 * @return ESP_OK após o retorno da suspensão.
 */
esp_err_t pwr_governor_enter_light_sleep(uint32_t timeout_ms);

/**
 * @brief Tipo de callback executado antes da entrada em suspensão profunda.
 */
typedef void (*pwr_sleep_cb_t)(void);

/**
 * @brief Registra callback invocado antes de entrar em Deep Sleep (ex: desligar display e isolar pinos).
 *
 * @param[in] cb Ponteiro para a função de callback.
 */
void pwr_governor_set_sleep_cb(pwr_sleep_cb_t cb);

/**
 * @brief Entra em modo Deep Sleep definitivo (< 100 µA).
 *
 * Desliga periféricos e configura o despertar por hardware nos pinos JOY_UP (GPIO 2)
 * e BATTERY_CHRG (GPIO 11) no domínio `ESP_PD_DOMAIN_RTC_PERIPH`.
 * Não retorna (reinicia a CPU a partir do bootloader ROM ao despertar).
 */
void pwr_governor_enter_deep_sleep(void);

#ifdef __cplusplus
}
#endif
