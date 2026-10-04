#include "podcast_history.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "sd_card.h"
#include "audio_player.h"

static const char *TAG = "podcast_history";

#define HISTORY_FILE_PATH SD_MOUNT_POINT "/Podcasts/.played_history"
#define MAX_HISTORY_ITEMS 512
#define PATH_ITEM_LEN     192

typedef struct {
    char rel_path[PATH_ITEM_LEN];
    uint32_t timestamp;
    uint32_t duration_sec;
} history_entry_t;

static history_entry_t *s_entries = NULL;
static int s_entry_count = 0;
static SemaphoreHandle_t s_hist_mutex = NULL;
static bool s_initialized = false;

// Normaliza o caminho para remocao de prefixos como "/sdcard/", "sdcard/", "Podcasts/", barras invertidas
static void normalize_path(const char *src, char *dst, size_t max_len)
{
    if (!src || !dst || max_len == 0) return;
    const char *p = src;

    // Pula barras iniciais
    while (*p == '/' || *p == '\\') p++;

    // Remove prefixo SD_MOUNT_POINT (ex: "sdcard/")
    const char *sd_prefix = "sdcard/";
    if (strncasecmp(p, sd_prefix, strlen(sd_prefix)) == 0) {
        p += strlen(sd_prefix);
        while (*p == '/' || *p == '\\') p++;
    }

    // Remove prefixo "Podcasts/" se presente
    const char *pod_prefix = "Podcasts/";
    if (strncasecmp(p, pod_prefix, strlen(pod_prefix)) == 0) {
        p += strlen(pod_prefix);
        while (*p == '/' || *p == '\\') p++;
    }

    size_t i = 0;
    while (*p && i + 1 < max_len) {
        if (*p == '\\') {
            dst[i++] = '/';
        } else {
            dst[i++] = *p;
        }
        p++;
    }
    dst[i] = '\0';
}

esp_err_t podcast_history_init(void)
{
    if (!s_hist_mutex) {
        s_hist_mutex = xSemaphoreCreateMutex();
    }

    if (xSemaphoreTake(s_hist_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (!s_entries) {
        s_entries = (history_entry_t *)heap_caps_malloc(
            sizeof(history_entry_t) * MAX_HISTORY_ITEMS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_entries) {
            s_entries = (history_entry_t *)malloc(sizeof(history_entry_t) * MAX_HISTORY_ITEMS);
        }
    }

    s_entry_count = 0;

    FILE *f = fopen(HISTORY_FILE_PATH, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f) && s_entry_count < MAX_HISTORY_ITEMS) {
            // Formato: caminho|timestamp|duracao
            char *p = line;
            while (*p && (*p == '\r' || *p == '\n')) *p++ = '\0';
            if (line[0] == '#' || line[0] == '\0') continue;

            char *token1 = strtok(line, "|\r\n");
            char *token2 = strtok(NULL, "|\r\n");
            char *token3 = strtok(NULL, "|\r\n");

            if (token1) {
                normalize_path(token1, s_entries[s_entry_count].rel_path, PATH_ITEM_LEN);
                s_entries[s_entry_count].timestamp = token2 ? (uint32_t)strtoul(token2, NULL, 10) : 0;
                s_entries[s_entry_count].duration_sec = token3 ? (uint32_t)strtoul(token3, NULL, 10) : 0;
                s_entry_count++;
            }
        }
        fclose(f);
        ESP_LOGI(TAG, "Carregados %d episodios do historico de reproducao.", s_entry_count);
    } else {
        ESP_LOGI(TAG, "Nenhum arquivo de historico anterior encontrado (%s).", HISTORY_FILE_PATH);
    }

    s_initialized = true;
    xSemaphoreGive(s_hist_mutex);

    // Registra callback no audio_player
    audio_player_register_track_completed_cb(podcast_history_audio_player_cb);

    return ESP_OK;
}

bool podcast_history_is_played(const char *rel_path)
{
    if (!rel_path || rel_path[0] == '\0') return false;

    char norm[PATH_ITEM_LEN];
    normalize_path(rel_path, norm, sizeof(norm));

    if (!s_initialized) {
        podcast_history_init();
    }

    bool found = false;
    if (s_hist_mutex && xSemaphoreTake(s_hist_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < s_entry_count; i++) {
            if (strcasecmp(s_entries[i].rel_path, norm) == 0) {
                found = true;
                break;
            }
        }
        xSemaphoreGive(s_hist_mutex);
    }
    return found;
}

void podcast_history_mark_played(const char *rel_path, uint32_t duration_sec)
{
    if (!rel_path || rel_path[0] == '\0') return;

    char norm[PATH_ITEM_LEN];
    normalize_path(rel_path, norm, sizeof(norm));

    if (!s_initialized) {
        podcast_history_init();
    }

    if (!s_hist_mutex || xSemaphoreTake(s_hist_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    // Verifica se ja esta registrado
    for (int i = 0; i < s_entry_count; i++) {
        if (strcasecmp(s_entries[i].rel_path, norm) == 0) {
            xSemaphoreGive(s_hist_mutex);
            return; // Ja existe
        }
    }

    uint32_t now_ts = (uint32_t)(esp_timer_get_time() / 1000000ULL);

    if (s_entry_count < MAX_HISTORY_ITEMS) {
        strncpy(s_entries[s_entry_count].rel_path, norm, PATH_ITEM_LEN - 1);
        s_entries[s_entry_count].rel_path[PATH_ITEM_LEN - 1] = '\0';
        s_entries[s_entry_count].timestamp = now_ts;
        s_entries[s_entry_count].duration_sec = duration_sec;
        s_entry_count++;
    }

    // Persiste no arquivo no SD
    FILE *f = fopen(HISTORY_FILE_PATH, "a");
    if (f) {
        fprintf(f, "%s|%lu|%lu\n", norm, (unsigned long)now_ts, (unsigned long)duration_sec);
        fclose(f);
        ESP_LOGI(TAG, "Episodio marcado como ouvido: '%s' (%lu s)", norm, (unsigned long)duration_sec);
    } else {
        ESP_LOGW(TAG, "Falha ao gravar no historico %s", HISTORY_FILE_PATH);
    }

    xSemaphoreGive(s_hist_mutex);
}

void podcast_history_remove_entry(const char *rel_path)
{
    if (!rel_path || rel_path[0] == '\0' || !s_hist_mutex) return;

    char norm[PATH_ITEM_LEN];
    normalize_path(rel_path, norm, sizeof(norm));

    if (xSemaphoreTake(s_hist_mutex, portMAX_DELAY) != pdTRUE) return;

    int idx = -1;
    for (int i = 0; i < s_entry_count; i++) {
        if (strcasecmp(s_entries[i].rel_path, norm) == 0) {
            idx = i;
            break;
        }
    }

    if (idx >= 0) {
        for (int i = idx; i < s_entry_count - 1; i++) {
            s_entries[i] = s_entries[i + 1];
        }
        s_entry_count--;

        // Reescreve arquivo
        FILE *f = fopen(HISTORY_FILE_PATH, "w");
        if (f) {
            for (int i = 0; i < s_entry_count; i++) {
                fprintf(f, "%s|%lu|%lu\n", s_entries[i].rel_path,
                        (unsigned long)s_entries[i].timestamp,
                        (unsigned long)s_entries[i].duration_sec);
            }
            fclose(f);
        }
        ESP_LOGI(TAG, "Entrada removida do historico: '%s'", norm);
    }

    xSemaphoreGive(s_hist_mutex);
}

void podcast_history_audio_player_cb(const char *rel_path, uint32_t elapsed_sec, uint32_t total_sec)
{
    if (!rel_path) return;

    // Apenas monitora faixas que pertencem ao diretorio de Podcasts
    if (strcasestr(rel_path, "podcast") != NULL || strcasestr(rel_path, "Podcasts") != NULL) {
        ESP_LOGI(TAG, "Audio player concluiu podcast: %s (%lu / %lu s)", rel_path,
                 (unsigned long)elapsed_sec, (unsigned long)total_sec);
        podcast_history_mark_played(rel_path, total_sec);
    }
}
