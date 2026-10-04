#ifndef PODCAST_HISTORY_H
#define PODCAST_HISTORY_H

#include <stdbool.h>
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
 * @brief Callback registrado no audio_player para notificacao automatica de reproducao completa.
 */
void podcast_history_audio_player_cb(const char *rel_path, uint32_t elapsed_sec, uint32_t total_sec);

#ifdef __cplusplus
}
#endif

#endif // PODCAST_HISTORY_H
