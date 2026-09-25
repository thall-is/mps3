#ifndef PODCAST_SYNC_H
#define PODCAST_SYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PODCAST_DEFAULT_LOCAL_SERVER     "http://192.168.15.216:8088"
#define PODCAST_DEFAULT_TAILSCALE_SERVER "http://100.120.93.115:8088"
#define PODCAST_MAX_EPISODES             32

typedef enum {
    PODCAST_SYNC_IDLE = 0,
    PODCAST_SYNC_CONNECTING_WIFI,
    PODCAST_SYNC_CONNECTING_SERVER,
    PODCAST_SYNC_FETCHING_CATALOG,
    PODCAST_SYNC_DOWNLOADING,
    PODCAST_SYNC_FINISHED,
    PODCAST_SYNC_ERROR,
} podcast_sync_state_t;

typedef struct {
    podcast_sync_state_t state;
    char current_program[32];
    char current_title[64];
    size_t current_file_bytes;
    size_t current_downloaded_bytes;
    int current_pct;       // 0-100% do arquivo atual
    int current_idx;       // 1-based (ex: 1)
    int total_count;       // total de novos episodios a baixar
    float speed_kbs;       // velocidade em KB/s
    char status_msg[32];   // status textual breve
} podcast_sync_progress_t;

/**
 * @brief Inicializa o subsistema de sincronizacao de podcasts.
 */
esp_err_t podcast_sync_init(void);

/**
 * @brief Dispara a sincronizacao em segundo plano (cria podcast_sync_task).
 *        Conecta ao Wi-Fi se necessario, consulta o catalogo e baixa novos episodios.
 */
esp_err_t podcast_sync_start(void);

/**
 * @brief Solicita cancelamento da sincronizacao ativa.
 */
void podcast_sync_cancel(void);

/**
 * @brief Retorna se a sincronizacao esta ativa no momento.
 */
bool podcast_sync_is_busy(void);

/**
 * @brief Obtem o progresso e estado atuais para exibicao na UI/OLED.
 */
void podcast_sync_get_progress(podcast_sync_progress_t *out);

/**
 * @brief Configura a URL base do servidor de podcast na NVS.
 */
void podcast_sync_set_server_url(const char *url);

/**
 * @brief Le a URL configurada do servidor (local ou Tailscale).
 */
void podcast_sync_get_server_url(char *out_url, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif // PODCAST_SYNC_H

