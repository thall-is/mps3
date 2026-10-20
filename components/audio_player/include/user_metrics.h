#ifndef USER_METRICS_H
#define USER_METRICS_H

/**
 * @file user_metrics.h
 * @brief Subsistema de Telemetria de Escuta e Estatísticas do Usuário ("MPS3 Wrapped")
 *
 * Coleta passivamente métricas de reprodução em RAM durante a decodificação (zero I/O
 * durante o fluxo de áudio) e persiste os eventos no cartão microSD apenas na transição
 * de faixas em `/sdcard/.mps3_analytics/events_YYYY.jsonl` e `tracks_summary.json`.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Metadados e motivo de encerramento de uma sessão de escuta.
 */
typedef struct {
    const char *rel_path;       /**< Caminho relativo a /sdcard (ex: "Musicas/Jazz/faixa.flac"). */
    const char *title;          /**< Título da faixa. */
    const char *artist;         /**< Nome do artista. */
    const char *album;          /**< Nome do álbum. */
    const char *codec;          /**< Formato do codec ("FLAC", "MP3", "WAV", etc.). */
    uint32_t sample_rate;       /**< Taxa de amostragem em Hz (ex: 44100, 192000). */
    uint32_t bits_per_sample;   /**< Resolução em bits (16, 24, 32). */
    uint32_t bitrate_bps;       /**< Bitrate médio em bps. */
    uint32_t duration_sec;      /**< Duração total da faixa em segundos. */
    uint32_t final_elapsed_sec; /**< Posição final do cursor em segundos ao encerrar. */
    const char *end_reason;     /**< Motivo do término ("eof", "skip_next", "skip_prev", "select", "stop"). */
} user_metrics_end_info_t;

/**
 * @brief Inicializa o coletor de métricas em PSRAM e carrega o resumo existente do SD.
 */
void user_metrics_init(void);

/**
 * @brief Notifica o início (ou retomada pós-seek) da reprodução de uma faixa em RAM.
 *
 * @param[in] rel_path Caminho relativo da faixa iniciada.
 */
void user_metrics_on_track_begin(const char *rel_path);

/**
 * @brief Acumula em RAM os frames efetivamente ouvidos e o volume instantâneo (zero I/O).
 *
 * @param[in] frames      Quantidade de frames estéreo enviados ao pipeline DSP.
 * @param[in] sample_rate Taxa de amostragem atual em Hz.
 * @param[in] volume_pct  Volume atual (0 a 100%).
 */
void user_metrics_add_listened_frames(uint32_t frames, uint32_t sample_rate, int volume_pct);

/**
 * @brief Finaliza a sessão da faixa ou contabiliza um seek na mesma faixa.
 *
 * Se `is_seek_restart` for `true`, mantém a sessão aberta em RAM e incrementa o contador
 * de seeks. Caso contrário, grava o evento no arquivo `.jsonl` e atualiza `tracks_summary.json`.
 *
 * @param[in] info            Estrutura com metadados e razão de término da faixa.
 * @param[in] is_seek_restart true se a saída de `play_track` foi apenas para um salto de seek.
 */
void user_metrics_on_track_end(const user_metrics_end_info_t *info, bool is_seek_restart);

#ifdef __cplusplus
}
#endif

#endif // USER_METRICS_H

