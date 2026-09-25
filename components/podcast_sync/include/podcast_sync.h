#ifndef PODCAST_SYNC_H
#define PODCAST_SYNC_H

/**
 * @file podcast_sync.h
 * @brief Subsistema Autônomo de Sincronização e Download de Podcasts em Segundo Plano
 *
 * Gerencia o download em lote de episódios novos diretamente para o cartão MicroSD
 * a partir de servidores locais ou relays móveis (Android/Termux), com suporte a
 * retomada de downloads parciais e sincronização automática ao plugar o carregador.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PODCAST_DEFAULT_LOCAL_SERVER     "http://192.168.15.216:8088"  /**< URL do servidor local de desenvolvimento. */
#define PODCAST_DEFAULT_TAILSCALE_SERVER "http://100.120.93.115:8088"  /**< URL do servidor remoto via Tailscale. */
#define PODCAST_MAX_EPISODES             32                            /**< Limite de episódios no catálogo por sincronização. */

/**
 * @brief Estados da máquina de estados de sincronização de podcasts.
 */
typedef enum {
    PODCAST_SYNC_IDLE = 0,          /**< Ocioso / aguardando gatilho de execução. */
    PODCAST_SYNC_CONNECTING_WIFI,   /**< Estabelecendo conexão Wi-Fi com o ponto de acesso. */
    PODCAST_SYNC_CONNECTING_SERVER, /**< Conectando ao servidor HTTP de podcasts. */
    PODCAST_SYNC_FETCHING_CATALOG,  /**< Obtendo e parseando o catálogo JSON de episódios. */
    PODCAST_SYNC_DOWNLOADING,       /**< Baixando fluxo de arquivo de áudio para o cartão SD. */
    PODCAST_SYNC_FINISHED,          /**< Sincronização concluída com sucesso. */
    PODCAST_SYNC_ERROR,             /**< Erro de rede ou gravação no sistema de arquivos. */
} podcast_sync_state_t;

/**
 * @brief Estrutura com métricas de progresso da sincronização de podcasts.
 */
typedef struct {
    podcast_sync_state_t state;         /**< Estado operacional corrente. */
    char current_program[32];           /**< Nome do programa / canal. */
    char current_title[64];             /**< Título do episódio sendo transferido. */
    size_t current_file_bytes;          /**< Tamanho total do episódio em bytes. */
    size_t current_downloaded_bytes;    /**< Bytes já transferidos e gravados no SD. */
    int current_pct;                    /**< Progresso percentual do arquivo atual (0-100%). */
    int current_idx;                    /**< Índice do episódio atual (1-based). */
    int total_count;                    /**< Total de episódios novos a baixar. */
    float speed_kbs;                    /**< Velocidade média de download em KB/s. */
    char status_msg[32];                /**< Mensagem textual breve para exibição na UI. */
} podcast_sync_progress_t;

/**
 * @brief Inicializa o componente de sincronização de podcasts e lê a configuração da NVS.
 *
 * @return ESP_OK em caso de sucesso.
 */
esp_err_t podcast_sync_init(void);

/**
 * @brief Dispara a tarefa de sincronização em segundo plano (`podcast_sync_task`).
 *
 * Conecta ao Wi-Fi se necessário, consulta o catálogo do servidor e baixa novos episódios.
 *
 * @return ESP_OK se a tarefa foi disparada; ESP_ERR_INVALID_STATE se já ocupada.
 */
esp_err_t podcast_sync_start(void);

/**
 * @brief Solicita o cancelamento imediato da sincronização ativa.
 */
void podcast_sync_cancel(void);

/**
 * @brief Informa se o subsistema de podcasts está ativamente ocupado no momento.
 *
 * @return true se a sincronização estiver em execução; false se ociosa.
 */
bool podcast_sync_is_busy(void);

/**
 * @brief Obtém o estado atual e as métricas de progresso para exibição na tela OLED.
 *
 * @param[out] out Ponteiro para a estrutura `podcast_sync_progress_t`.
 */
void podcast_sync_get_progress(podcast_sync_progress_t *out);

/**
 * @brief Configura e persiste na NVS o endereço URL base do servidor de podcasts.
 *
 * @param[in] url URL do servidor (ex: "http://192.168.1.100:8088").
 */
void podcast_sync_set_server_url(const char *url);

/**
 * @brief Obtém o endereço URL do servidor de podcasts atualmente configurado.
 *
 * @param[out] out_url Buffer de destino para a string.
 * @param[in]  max_len Tamanho máximo do buffer de saída.
 */
void podcast_sync_get_server_url(char *out_url, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif // PODCAST_SYNC_H
