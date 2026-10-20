#ifndef PODCAST_HISTORY_H
#define PODCAST_HISTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa o subsistema de historico de reproducao de podcasts.
 * Carrega a tabela de episodios concluidos a partir de /sdcard/Podcasts/.played_history.
 */
esp_err_t podcast_history_init(void);

/**
 * @brief Verifica se um episodio de podcast ja foi ouvido (>= 90% ou concluido).
 *
 * @param[in] rel_path Caminho relativo do podcast (ex: "Podcasts/Canal/ep.mp3" ou "Canal/ep.mp3").
 * @return true se ja foi reproduzido, false caso contrario.
 */
bool podcast_history_is_played(const char *rel_path);

/**
 * @brief Marca um episodio como ouvido e persiste imediatamente no SD.
 *
 * @param[in] rel_path Caminho do episodio.
 * @param[in] duration_sec Duracao total em segundos.
 */
void podcast_history_mark_played(const char *rel_path, uint32_t duration_sec);

/**
 * @brief Remove uma entrada do historico caso necessario.
 *
 * @param[in] rel_path Caminho do episodio.
 */
void podcast_history_remove_entry(const char *rel_path);

/**
 * @brief Desmarca um episodio como ouvido no historico e persiste no SD.
 *
 * @param[in] rel_path Caminho do episodio.
 * @return ESP_OK se removido ou inexistente.
 */
esp_err_t podcast_history_unmark_played(const char *rel_path);

/**
 * @brief Retorna a quantidade total de entradas no historico de reproducao.
 */
int podcast_history_get_count(void);

/**
 * @brief Obtem uma entrada do historico pelo indice (thread-safe).
 *
 * @param[in]  idx          Indice (0 a count-1).
 * @param[out] out_rel_path Buffer para caminho relativo ("Show/ep.mp3").
 * @param[in]  max_len      Tamanho maximo do buffer.
 * @param[out] out_ts       Timestamp opcional (pode ser NULL).
 * @param[out] out_dur      Duracao em segundos opcional (pode ser NULL).
 * @return true se o indice for valido e copiado com sucesso.
 */
bool podcast_history_get_entry(int idx, char *out_rel_path, size_t max_len, uint32_t *out_ts, uint32_t *out_dur);

/**
 * @brief Callback registrado no audio_player para notificacao automatica de reproducao completa.
 */
void podcast_history_audio_player_cb(const char *rel_path, uint32_t elapsed_sec, uint32_t total_sec);

#ifdef __cplusplus
}
#endif

#endif // PODCAST_HISTORY_H
