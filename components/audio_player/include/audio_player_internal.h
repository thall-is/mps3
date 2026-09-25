#pragma once

/**
 * @file audio_player_internal.h
 * @brief Estruturas Internas e Pipeline DSP Dual-Core do Audio Player
 *
 * Define os comandos assíncronos do reprodutor, os mutexes de sincronização do
 * estado interno e as funções de comunicação inter-núcleo (Core 1 Decodificador ➔
 * Core 0 Audio DSP Task / I2S DMA).
 */

#include "audio_player.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Comandos de controle assíncronos enviados à tarefa do decodificador (`player_task`).
 */
typedef enum {
    PLAYER_CMD_NONE,          /**< Nenhum comando pendente. */
    PLAYER_CMD_NEXT,          /**< Avançar para a próxima faixa. */
    PLAYER_CMD_PREV,          /**< Voltar para a faixa anterior. */
    PLAYER_CMD_RESTART,       /**< Reiniciar a faixa atual do início. */
    PLAYER_CMD_SEEK_FWD,      /**< Salto de avanço rápido (Seek Forward). */
    PLAYER_CMD_SEEK_BWD,      /**< Salto de retrocesso rápido (Seek Backward). */
    PLAYER_CMD_PLAY_INDEX,    /**< Tocar faixa específica por índice. */
    PLAYER_CMD_USB_TAKEOVER,  /**< Liberar cartão SD para modo USB MSC. */
    PLAYER_CMD_USB_RESTORE,   /**< Restaurar cartão SD após modo USB MSC. */
    PLAYER_CMD_PLAY_URL,      /**< Iniciar streaming de Web Rádio. */
    PLAYER_CMD_STOP_URL       /**< Interromper streaming de Web Rádio. */
} player_cmd_t;

extern SemaphoreHandle_t s_state_mutex;
extern playback_state_t s_state;
extern volatile int s_volume_percent;
extern volatile player_cmd_t s_pending_cmd;
extern volatile bool s_paused;
extern volatile bool s_usb_takeover_active;

/**
 * @brief Adquire o mutex de sincronização de estado (`s_state_mutex`).
 *
 * Bloqueia a execução até que o lock seja concedido (timeout de 1000 ms).
 */
void state_lock(void);

/**
 * @brief Libera o mutex de sincronização de estado (`s_state_mutex`).
 */
void state_unlock(void);

/**
 * @brief Envia um bloco de amostras PCM da decodificação (Core 1) para a fila de DSP (Core 0).
 *
 * Aloca ou reaproveita blocos da fila livre (`s_dsp_free_queue`), preenche as amostras estéreo
 * de 32 bits e enfileira em `s_dsp_ready_queue`.
 *
 * @param[in] samples Ponteiro para as amostras PCM estéreo intercaladas (L/R).
 * @param[in] count   Número total de amostras escalares (frames * 2).
 * @param[in] rate    Taxa de amostragem em Hz do bloco enviado.
 *
 * @return 
 *   - ESP_OK: Bloco enfileirado com sucesso.
 *   - ESP_ERR_TIMEOUT: Fila de processamento cheia ou travada.
 */
esp_err_t audio_dsp_send_pcm(const int32_t *samples, size_t count, uint32_t rate);

/**
 * @brief Descarta todos os blocos PCM acumulados na fila de DSP.
 *
 * Chamado durante operações de seek ou troca de faixa para evitar que áudio antigo
 * continue tocando após o salto.
 */
void audio_dsp_flush(void);

/**
 * @brief Aguarda o esgotamento dos buffers DMA de áudio do I2S.
 *
 * Garante que todo o áudio decodificado foi fisicamente emitido pelo DAC antes
 * de alterar taxas de amostragem ou desmontar recursos.
 */
void audio_dsp_drain(void);

/**
 * @brief Notifica a tarefa de DSP e reconfigura o divisor de clock do driver I2S.
 *
 * @param[in] rate Nova taxa de amostragem em Hz (ex: 44100, 48000, 96000, 192000).
 */
void audio_dsp_set_rate(uint32_t rate);

/**
 * @brief Dispara uma rampa suave de volume (*fade-in*) na tarefa de DSP.
 *
 * Previne estalos acústicos (*pops/clicks*) no DAC PCM5102A ao iniciar novas faixas.
 *
 * @param[in] sample_rate Taxa de amostragem da faixa para calibração da duração da rampa.
 */
void audio_dsp_trigger_fade_in(uint32_t sample_rate);

/**
 * @brief Garante a alocação de buffer temporário de conversão estéreo em SRAM rápida.
 *
 * @param[in] needed_samples Número mínimo de amostras necessárias.
 *
 * @return Ponteiro para a área de memória interna alocada.
 */
int32_t *ensure_stereo_scratch(size_t needed_samples);

#ifdef __cplusplus
}
/**
 * @brief Loop de conexão e recepção de pacotes HTTP para Web Rádio.
 *
 * @param[in] url URL do servidor de streaming.
 */
void play_web_radio(const char *url);
#endif
