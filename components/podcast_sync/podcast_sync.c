#include "podcast_sync.h"
#include "podcast_history.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "sd_card.h"
#include "wifi_transfer.h"
#include "audio_player.h"
#include "battery.h"

static const char *TAG = "podcast_sync";

#define PODCAST_BASE_DIR             SD_MOUNT_POINT "/Podcasts"
#define SYNC_AUTO_WAIT_TIMEOUT_MS    (90 * 1000)      // 90s aguardando /api/sync/begin no modo automatico
#define SYNC_MANUAL_WAIT_TIMEOUT_MS  (5 * 60 * 1000)  // 5 min aguardando servidor no modo manual
#define SYNC_ACTIVE_IDLE_TIMEOUT_MS  (120 * 1000)     // 120s de inatividade entre uploads apos /api/sync/begin
#define SYNC_AUTO_COOLDOWN_SEC       (6 * 3600)       // 6 horas de cooldown minimo entre auto-syncs
static SemaphoreHandle_t s_sync_mutex = NULL;
static volatile bool s_is_busy = false;
static volatile bool s_auto_mode = false;
static volatile bool s_cancel_requested = false;
static volatile bool s_sync_begun = false;
static volatile bool s_sync_completed = false;
static volatile uint32_t s_last_activity_ms = 0;
static uint32_t s_last_sync_boot_ms = 0;
static podcast_sync_progress_t s_progress;

static char s_configured_url[128] = PODCAST_DEFAULT_LOCAL_SERVER;

static uint32_t now_ms32(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void set_state_and_title(podcast_sync_state_t st, const char *msg, const char *prog, const char *title)
{
    if (!s_sync_mutex) return;
    if (xSemaphoreTake(s_sync_mutex, portMAX_DELAY) == pdTRUE) {
        s_progress.state = st;
        if (msg) {
            strncpy(s_progress.status_msg, msg, sizeof(s_progress.status_msg) - 1);
            s_progress.status_msg[sizeof(s_progress.status_msg) - 1] = '\0';
        }
        if (prog) {
            strncpy(s_progress.current_program, prog, sizeof(s_progress.current_program) - 1);
            s_progress.current_program[sizeof(s_progress.current_program) - 1] = '\0';
        }
        if (title) {
            strncpy(s_progress.current_title, title, sizeof(s_progress.current_title) - 1);
            s_progress.current_title[sizeof(s_progress.current_title) - 1] = '\0';
        }
        xSemaphoreGive(s_sync_mutex);
    }
}

void podcast_sync_notify_activity(void)
{
    s_last_activity_ms = now_ms32();
}

void podcast_sync_on_begin(int total_files, uint64_t total_bytes)
{
    s_last_activity_ms = now_ms32();
    s_sync_begun = true;
    s_sync_completed = false;

    if (!s_sync_mutex) return;
    if (xSemaphoreTake(s_sync_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_progress.state = PODCAST_SYNC_DOWNLOADING;
        s_progress.total_count = total_files > 0 ? total_files : 0;
        s_progress.current_idx = 0;
        s_progress.current_pct = 0;
        s_progress.current_file_bytes = (size_t)total_bytes;
        strncpy(s_progress.status_msg, "RECEBENDO PODCASTS...", sizeof(s_progress.status_msg) - 1);
        s_progress.status_msg[sizeof(s_progress.status_msg) - 1] = '\0';
        strncpy(s_progress.current_program, "PODCAST SYNC", sizeof(s_progress.current_program) - 1);
        s_progress.current_program[sizeof(s_progress.current_program) - 1] = '\0';
        strncpy(s_progress.current_title, "Recebendo episodios...", sizeof(s_progress.current_title) - 1);
        s_progress.current_title[sizeof(s_progress.current_title) - 1] = '\0';
        xSemaphoreGive(s_sync_mutex);
    }
}

void podcast_sync_mark_completed(void)
{
    s_last_activity_ms = now_ms32();
    s_last_sync_boot_ms = s_last_activity_ms ? s_last_activity_ms : 1;
    s_sync_completed = true;
    s_sync_begun = false;

    time_t now_epoch = time(NULL);
    nvs_handle_t h;
    if (nvs_open("mps3_sync", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "last_sync_ep", (uint32_t)now_epoch);
        nvs_commit(h);
        nvs_close(h);
    }

    if (s_sync_mutex && xSemaphoreTake(s_sync_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_progress.state = PODCAST_SYNC_FINISHED;
        s_progress.current_pct = 100;
        s_progress.speed_kbs = 0.0f;
        strncpy(s_progress.status_msg, "SINCRONIZADO!", sizeof(s_progress.status_msg) - 1);
        s_progress.status_msg[sizeof(s_progress.status_msg) - 1] = '\0';
        strncpy(s_progress.current_title, "Sincronizado!", sizeof(s_progress.current_title) - 1);
        s_progress.current_title[sizeof(s_progress.current_title) - 1] = '\0';
        xSemaphoreGive(s_sync_mutex);
    }

    ESP_LOGI(TAG, "Sessao de sincronizacao de podcasts marcada como concluida (epoch=%lu).",
             (unsigned long)now_epoch);
}

typedef enum {
    SYNC_PHASE_IDLE = 0,
    SYNC_PHASE_START,
    SYNC_PHASE_WAIT_WIFI,
    SYNC_PHASE_ACTIVE,
    SYNC_PHASE_DONE_HOLD,
    SYNC_PHASE_CLEANUP,
} sync_phase_t;

static volatile sync_phase_t s_phase = SYNC_PHASE_IDLE;
static uint32_t s_phase_start_ms = 0;
static bool s_wifi_started_by_sync = false;
static bool s_stepping = false;

static void podcast_sync_step(void)
{
    if (!s_is_busy || s_phase == SYNC_PHASE_IDLE) return;

    uint32_t now = now_ms32();

    switch (s_phase) {
        case SYNC_PHASE_START: {
            ESP_LOGI(TAG, "Iniciando sessao Push (%s) via state machine...",
                     s_auto_mode ? "AUTO" : "MANUAL");
            s_cancel_requested = false;
            s_sync_begun = false;
            s_sync_completed = false;
            s_last_activity_ms = now;
            s_phase_start_ms = now;
            s_wifi_started_by_sync = false;

            struct stat st_dir;
            if (stat(PODCAST_BASE_DIR, &st_dir) != 0) {
                mkdir(PODCAST_BASE_DIR, 0777);
            }

            set_state_and_title(PODCAST_SYNC_CONNECTING_WIFI, "CONECTANDO WIFI...", "PODCAST SYNC", "Conectando Wi-Fi...");

            if (!wifi_transfer_is_active()) {
                s_wifi_started_by_sync = true;
                esp_err_t werr = wifi_transfer_enter(WIFI_TRANSFER_MODE_STA);
                if (werr != ESP_OK) {
                    werr = wifi_transfer_enter_auto();
                }
                if (werr != ESP_OK) {
                    ESP_LOGE(TAG, "Nao foi possivel iniciar Wi-Fi para sincronizacao: %s", esp_err_to_name(werr));
                    set_state_and_title(PODCAST_SYNC_ERROR, "FALHA NO WIFI", "PODCAST SYNC", "Sem rede Wi-Fi");
                    s_phase_start_ms = now;
                    s_phase = SYNC_PHASE_CLEANUP;
                    break;
                }
            }
            s_phase_start_ms = now;
            s_phase = SYNC_PHASE_WAIT_WIFI;
            break;
        }

        case SYNC_PHASE_WAIT_WIFI: {
            wifi_transfer_poll();
            if (s_cancel_requested) {
                set_state_and_title(PODCAST_SYNC_IDLE, "CANCELADO", "PODCAST SYNC", "Cancelado pelo usuario");
                s_phase_start_ms = now;
                s_phase = SYNC_PHASE_CLEANUP;
                break;
            }
            if (wifi_transfer_has_sta_ip()) {
                // Bombea mais um poll para garantir que start_httpd() e mDNS subam imediatamente
                wifi_transfer_poll();
                char ip_status[64] = {0};
                int dummy_files = 0;
                wifi_transfer_get_status(ip_status, sizeof(ip_status), &dummy_files);

                char wait_title[80];
                snprintf(wait_title, sizeof(wait_title), "IP: %s", ip_status[0] ? ip_status : "mps3.local");
                set_state_and_title(PODCAST_SYNC_CONNECTING_SERVER, "AGUARDANDO SERVIDOR...", "PODCAST SYNC", wait_title);
                s_last_activity_ms = now;
                s_phase_start_ms = now;
                s_phase = SYNC_PHASE_ACTIVE;
                ESP_LOGI(TAG, "MPS3 pronto para Push Sync em %s (mDNS _mps3._tcp ativo). Aguardando servidor...", wait_title);
                break;
            }
            if ((now - s_phase_start_ms) >= 25000) {
                ESP_LOGW(TAG, "Timeout ao conectar ao Wi-Fi para Push Sync.");
                set_state_and_title(PODCAST_SYNC_ERROR, "FALHA NO WIFI", "PODCAST SYNC", "Falha na conexao");
                s_phase_start_ms = now;
                s_phase = SYNC_PHASE_CLEANUP;
            }
            break;
        }

        case SYNC_PHASE_ACTIVE: {
            wifi_transfer_poll();

            if (s_sync_completed) {
                set_state_and_title(PODCAST_SYNC_FINISHED, "SINCRONIZADO!", "PODCAST SYNC", "Sincronizado!");
                s_phase_start_ms = now;
                s_phase = SYNC_PHASE_DONE_HOLD;
                break;
            }
            if (s_cancel_requested || !wifi_transfer_is_active()) {
                set_state_and_title(PODCAST_SYNC_IDLE, "CANCELADO", "PODCAST SYNC", "Cancelado pelo usuario");
                s_phase_start_ms = now;
                s_phase = SYNC_PHASE_CLEANUP;
                break;
            }

            wifi_transfer_progress_t wp;
            wifi_transfer_get_progress(&wp);
            if (wp.in_progress) {
                s_last_activity_ms = now;
                s_sync_begun = true;
                if (s_sync_mutex && xSemaphoreTake(s_sync_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    s_progress.state = PODCAST_SYNC_DOWNLOADING;
                    strncpy(s_progress.status_msg, "RECEBENDO PODCASTS...", sizeof(s_progress.status_msg) - 1);
                    s_progress.status_msg[sizeof(s_progress.status_msg) - 1] = '\0';
                    if (wp.filename[0]) {
                        strncpy(s_progress.current_title, wp.filename, sizeof(s_progress.current_title) - 1);
                        s_progress.current_title[sizeof(s_progress.current_title) - 1] = '\0';
                    }
                    s_progress.current_pct = wp.file_pct;
                    s_progress.speed_kbs = wp.kbps;
                    if (wp.idx > 0) s_progress.current_idx = wp.idx;
                    if (wp.count > 0) s_progress.total_count = wp.count;
                    xSemaphoreGive(s_sync_mutex);
                }
            }

            uint32_t timeout_ms = s_sync_begun
                ? SYNC_ACTIVE_IDLE_TIMEOUT_MS
                : (s_auto_mode ? SYNC_AUTO_WAIT_TIMEOUT_MS : SYNC_MANUAL_WAIT_TIMEOUT_MS);

            if ((now - s_last_activity_ms) >= timeout_ms) {
                ESP_LOGW(TAG, "Timeout de inatividade na sessao Push (%u s sem atividade). Encerrando...",
                         (unsigned)(timeout_ms / 1000));
                set_state_and_title(PODCAST_SYNC_IDLE, "TEMPO ESGOTADO", "PODCAST SYNC", "Servidor nao conectou");
                s_phase_start_ms = now;
                s_phase = SYNC_PHASE_CLEANUP;
            }
            break;
        }

        case SYNC_PHASE_DONE_HOLD: {
            wifi_transfer_poll();
            if ((now - s_phase_start_ms) >= 2000) {
                s_phase_start_ms = now;
                s_phase = SYNC_PHASE_CLEANUP;
            }
            break;
        }

        case SYNC_PHASE_CLEANUP: {
            if (s_wifi_started_by_sync && wifi_transfer_is_active()) {
                ESP_LOGI(TAG, "Desligando Wi-Fi apos sessao de sincronizacao de podcasts...");
                wifi_transfer_request_exit();
                wifi_transfer_poll();
                if (wifi_transfer_is_active() && (now - s_phase_start_ms) < 8000) {
                    break;
                }
            }
            s_wifi_started_by_sync = false;
            s_is_busy = false;
            s_auto_mode = false;
            s_phase = SYNC_PHASE_IDLE;
            break;
        }

        default:
            s_is_busy = false;
            s_phase = SYNC_PHASE_IDLE;
            break;
    }
}

esp_err_t podcast_sync_init(void)
{
    if (!s_sync_mutex) {
        s_sync_mutex = xSemaphoreCreateMutex();
    }
    memset(&s_progress, 0, sizeof(s_progress));
    s_progress.state = PODCAST_SYNC_IDLE;
    strcpy(s_progress.status_msg, "Pronto");

    nvs_handle_t h;
    if (nvs_open("podcast", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_configured_url);
        nvs_get_str(h, "server_url", s_configured_url, &len);
        nvs_close(h);
    }

    ESP_LOGI(TAG, "podcast_sync (Push Engine v2.0) inicializado.");
    return ESP_OK;
}

static esp_err_t start_sync_internal(bool auto_mode)
{
    if (s_is_busy) {
        ESP_LOGW(TAG, "Sincronizacao ja esta em andamento.");
        return ESP_ERR_INVALID_STATE;
    }
    podcast_sync_init();
    s_auto_mode = auto_mode;
    s_cancel_requested = false;
    s_sync_begun = false;
    s_sync_completed = false;
    s_last_activity_ms = now_ms32();
    s_phase_start_ms = s_last_activity_ms;
    s_phase = SYNC_PHASE_START;
    s_is_busy = true;
    set_state_and_title(PODCAST_SYNC_CONNECTING_WIFI, "CONECTANDO WIFI...", "PODCAST SYNC", "Conectando Wi-Fi...");
    return ESP_OK;
}

esp_err_t podcast_sync_start_manual(void)
{
    return start_sync_internal(false);
}

esp_err_t podcast_sync_start(void)
{
    return podcast_sync_start_manual();
}

esp_err_t podcast_sync_check_auto(void)
{
    if (s_is_busy || wifi_transfer_is_active()) {
        return ESP_ERR_INVALID_STATE;
    }

    playback_state_t st;
    audio_player_get_state(&st);
    if (st.playing && !st.paused) {
        ESP_LOGD(TAG, "Auto-sync adiado: reprodutor de audio ativo.");
        return ESP_ERR_INVALID_STATE;
    }

    int bat_pct = battery_get_percent();
    bool charging = battery_is_charging();
    if (bat_pct < 15 && !charging) {
        ESP_LOGW(TAG, "Auto-sync ignorado: bateria baixa (%d%%) e sem carregador.", bat_pct);
        return ESP_ERR_INVALID_STATE;
    }

    if (wifi_transfer_get_known_count() <= 0) {
        ESP_LOGW(TAG, "Auto-sync ignorado: nenhuma rede Wi-Fi conhecida salva.");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t last_ep = 0;
    nvs_handle_t h;
    if (nvs_open("mps3_sync", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "last_sync_ep", &last_ep);
        nvs_close(h);
    }

    time_t now_epoch = time(NULL);
    if (now_epoch > 1700000000 && last_ep > 1700000000) {
        if ((uint32_t)now_epoch >= last_ep && ((uint32_t)now_epoch - last_ep) < SYNC_AUTO_COOLDOWN_SEC) {
            ESP_LOGI(TAG, "Auto-sync em cooldown (%lu s desde a ultima sincronizacao, minimo %u s).",
                     (unsigned long)((uint32_t)now_epoch - last_ep), (unsigned)SYNC_AUTO_COOLDOWN_SEC);
            return ESP_ERR_INVALID_STATE;
        }
    } else if (s_last_sync_boot_ms > 0) {
        uint32_t elapsed_ms = now_ms32() - s_last_sync_boot_ms;
        if (elapsed_ms < (uint32_t)SYNC_AUTO_COOLDOWN_SEC * 1000U) {
            ESP_LOGI(TAG, "Auto-sync em cooldown de sessao (%u s decorridos).", (unsigned)(elapsed_ms / 1000));
            return ESP_ERR_INVALID_STATE;
        }
    }

    ESP_LOGI(TAG, "Pre-condicoes satisfeitas! Iniciando auto-sync de podcasts (timeout=90s)...");
    return start_sync_internal(true);
}

void podcast_sync_request_cancel(void)
{
    s_cancel_requested = true;
}

void podcast_sync_cancel(void)
{
    podcast_sync_request_cancel();
}

bool podcast_sync_is_busy(void)
{
    return s_is_busy;
}

bool podcast_sync_is_auto_mode(void)
{
    return s_auto_mode;
}

void podcast_sync_get_progress(podcast_sync_progress_t *out)
{
    if (!out || !s_sync_mutex) return;
    bool do_step = false;
    if (xSemaphoreTake(s_sync_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_is_busy && !s_stepping) {
            s_stepping = true;
            do_step = true;
        }
        xSemaphoreGive(s_sync_mutex);
    }
    if (do_step) {
        podcast_sync_step();
        if (xSemaphoreTake(s_sync_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_stepping = false;
            xSemaphoreGive(s_sync_mutex);
        } else {
            s_stepping = false;
        }
    }
    if (xSemaphoreTake(s_sync_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        *out = s_progress;
        xSemaphoreGive(s_sync_mutex);
    }
}

void podcast_sync_set_server_url(const char *url)
{
    if (!url) return;
    strncpy(s_configured_url, url, sizeof(s_configured_url) - 1);
    s_configured_url[sizeof(s_configured_url) - 1] = '\0';

    nvs_handle_t h;
    if (nvs_open("podcast", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "server_url", s_configured_url);
        nvs_commit(h);
        nvs_close(h);
    }
}

void podcast_sync_get_server_url(char *out_url, size_t max_len)
{
    if (!out_url || max_len == 0) return;
    strncpy(out_url, s_configured_url, max_len - 1);
    out_url[max_len - 1] = '\0';
}
