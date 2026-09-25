#include "podcast_sync.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "sd_card.h"
#include "wifi_transfer.h"
#include "audio_player.h"

static const char *TAG = "podcast_sync";

#define PODCAST_BASE_DIR                 SD_MOUNT_POINT "/Podcasts"
#define PODCAST_MAX_RECENT_PER_PROGRAM   2             // Maximo de episodios recentes por programa na sincronizacao automatica
#define SYNC_BUFFER_SIZE                 (32 * 1024)   // 32 KB de buffer de transferencia em PSRAM
#define CATALOG_BUFFER_SIZE              (64 * 1024)   // 64 KB para o catalogo JSON

typedef struct {
    char id[16];
    char program[32];
    char title[64];
    char filename[96];
    char rel_path[160];
    size_t size_bytes;
    uint32_t mtime;
} podcast_item_t;

static SemaphoreHandle_t s_sync_mutex = NULL;
static TaskHandle_t s_sync_task_handle = NULL;
static volatile bool s_is_busy = false;
static volatile bool s_cancel_requested = false;
static podcast_sync_progress_t s_progress;

static char s_configured_url[128] = PODCAST_DEFAULT_LOCAL_SERVER;

static void set_state(podcast_sync_state_t st, const char *msg)
{
    if (xSemaphoreTake(s_sync_mutex, portMAX_DELAY) == pdTRUE) {
        s_progress.state = st;
        if (msg) {
            strncpy(s_progress.status_msg, msg, sizeof(s_progress.status_msg) - 1);
            s_progress.status_msg[sizeof(s_progress.status_msg) - 1] = '\0';
        }
        xSemaphoreGive(s_sync_mutex);
    }
}

// Codifica espacos e caracteres especiais para URL
static void url_encode(const char *src, char *dst, size_t dst_len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t d = 0;
    for (size_t s = 0; src[s] != '\0' && d + 4 < dst_len; s++) {
        unsigned char c = (unsigned char)src[s];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '/') {
            dst[d++] = (char)c;
        } else {
            dst[d++] = '%';
            dst[d++] = hex[(c >> 4) & 0x0F];
            dst[d++] = hex[c & 0x0F];
        }
    }
    dst[d] = '\0';
}

// Cria diretorio recursivamente se nao existir
static void ensure_dir_exists(const char *dir_path)
{
    struct stat st;
    if (stat(dir_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        return;
    }
    char tmp[256];
    strncpy(tmp, dir_path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (stat(tmp, &st) != 0) {
                mkdir(tmp, 0777);
            }
            *p = '/';
        }
    }
    if (stat(tmp, &st) != 0) {
        mkdir(tmp, 0777);
    }
}

static void json_get_field(const char *obj_start, const char *obj_end,
                             const char *key, char *dst, size_t max_len)
{
    dst[0] = '\0';
    char search[48];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *f = strstr(obj_start, search);
    if (f && f < obj_end) {
        f += strlen(search);
        while (*f == ' ' || *f == '\t') f++;
        if (*f == '\"') {
            f++;
            const char *end_val = strchr(f, '\"');
            if (end_val && end_val <= obj_end) {
                size_t len = (size_t)(end_val - f);
                if (len >= max_len) len = max_len - 1;
                strncpy(dst, f, len);
                dst[len] = '\0';
            }
        }
    }
}

// Parser JSON leve especifico para o catalogo do mps3_podcast_server
static int parse_catalog_json(const char *json_str, podcast_item_t *items, int max_items)
{
    const char *p = strstr(json_str, "\"podcasts\":");
    if (!p) {
        return 0;
    }
    p = strchr(p, '[');
    if (!p) {
        return 0;
    }
    p++;

    int count = 0;
    while (*p && count < max_items) {
        const char *obj_start = strchr(p, '{');
        if (!obj_start) break;
        const char *obj_end = strchr(obj_start, '}');
        if (!obj_end) break;

        podcast_item_t *item = &items[count];
        memset(item, 0, sizeof(podcast_item_t));

        json_get_field(obj_start, obj_end, "id", item->id, sizeof(item->id));
        json_get_field(obj_start, obj_end, "program", item->program, sizeof(item->program));
        json_get_field(obj_start, obj_end, "title", item->title, sizeof(item->title));
        json_get_field(obj_start, obj_end, "filename", item->filename, sizeof(item->filename));
        json_get_field(obj_start, obj_end, "rel_path", item->rel_path, sizeof(item->rel_path));

        // Extrai size_bytes
        const char *sb = strstr(obj_start, "\"size_bytes\":");
        if (sb && sb < obj_end) {
            sb += 13;
            item->size_bytes = (size_t)strtoull(sb, NULL, 10);
        }

        // Extrai mtime
        const char *mt = strstr(obj_start, "\"mtime\":");
        if (mt && mt < obj_end) {
            mt += 8;
            item->mtime = (uint32_t)strtoul(mt, NULL, 10);
        }

        if (item->filename[0] != '\0' && item->size_bytes > 0) {
            count++;
        }
        p = obj_end + 1;
    }
    return count;
}

// Tarefa de execucao em segundo plano
static void podcast_sync_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "Iniciando podcast_sync_task no Core %d (prio %d)...",
             xPortGetCoreID(), (int)uxTaskPriorityGet(NULL));

    s_is_busy = true;
    s_cancel_requested = false;
    bool wifi_was_started_by_sync = false;

    // 1. Garantir conexao Wi-Fi
    set_state(PODCAST_SYNC_CONNECTING_WIFI, "Conectando WiFi...");
    if (!wifi_transfer_is_active()) {
        ESP_LOGI(TAG, "WiFi inativo. Ativando wifi_transfer_enter_auto()...");
        wifi_was_started_by_sync = true;
        wifi_transfer_enter_auto();
    }

    // Aguarda obtencao de IP STA na rede local (ate 25 segundos)
    uint32_t wait_start = (uint32_t)(esp_timer_get_time() / 1000ULL);
    bool wifi_ready = false;
    while (!s_cancel_requested && (uint32_t)(esp_timer_get_time() / 1000ULL) - wait_start < 25000) {
        if (wifi_transfer_has_sta_ip()) {
            wifi_ready = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (!wifi_ready && wifi_transfer_is_active()) {
        // Se nao pegou IP STA apos 25s, verifica se tem IP em status (ex: Hotspot AP ativo)
        char status_str[64] = {0};
        int files = 0;
        wifi_transfer_get_status(status_str, sizeof(status_str), &files);
        if (status_str[0] && strcmp(status_str, "Conectando...") != 0) {
            wifi_ready = true;
        }
    }

    if (!wifi_ready || s_cancel_requested) {
        ESP_LOGE(TAG, "Nao foi possivel conectar ao WiFi dentro do tempo limite.");
        set_state(PODCAST_SYNC_ERROR, "Falha no WiFi");
        if (wifi_was_started_by_sync) wifi_transfer_request_exit();
        s_is_busy = false;
        vTaskDelete(NULL);
        return;
    }

    // 2. Alocar buffers em PSRAM
    char *catalog_buf = (char *)heap_caps_malloc(CATALOG_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *sync_buf = (uint8_t *)heap_caps_malloc(SYNC_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!catalog_buf || !sync_buf) {
        ESP_LOGE(TAG, "Falha ao alocar memoria em PSRAM para sincronizacao.");
        set_state(PODCAST_SYNC_ERROR, "Memoria insuficiente");
        if (catalog_buf) free(catalog_buf);
        if (sync_buf) free(sync_buf);
        if (wifi_was_started_by_sync) wifi_transfer_request_exit();
        s_is_busy = false;
        vTaskDelete(NULL);
        return;
    }

    // 3. Consultar Catalogo do Servidor (tenta LAN, Gateway Hotspot e Tailscale)
    set_state(PODCAST_SYNC_FETCHING_CATALOG, "Buscando catalogo...");

    char gw_url[96] = {0};
    char gw_ip[32] = {0};
    bool has_gw = wifi_transfer_get_gateway_ip(gw_ip, sizeof(gw_ip));
    if (has_gw && strlen(gw_ip) > 0) {
        snprintf(gw_url, sizeof(gw_url), "http://%s:8088", gw_ip);
    }

    const char *servers_to_try[3];
    int num_servers = 0;
    servers_to_try[num_servers++] = s_configured_url;
    if (has_gw && gw_url[0] != '\0' && strcmp(gw_url, s_configured_url) != 0) {
        servers_to_try[num_servers++] = gw_url;
    }
    servers_to_try[num_servers++] = PODCAST_DEFAULT_TAILSCALE_SERVER;

    int catalog_len = 0;
    const char *active_server = NULL;

    for (int s_idx = 0; s_idx < num_servers && !s_cancel_requested; s_idx++) {
        char url[160];
        snprintf(url, sizeof(url), "%s/api/podcasts?limit=25", servers_to_try[s_idx]);
        ESP_LOGI(TAG, "Tentando obter catalogo em: %s", url);

        esp_http_client_config_t http_cfg = {
            .url = url,
            .timeout_ms = 6000,
            .buffer_size = 4096,
        };
        esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
        if (!client) continue;

        esp_err_t err = esp_http_client_open(client, 0);
        if (err == ESP_OK) {
            esp_http_client_fetch_headers(client);
            int status = esp_http_client_get_status_code(client);
            if (status == 200) {
                catalog_len = esp_http_client_read(client, catalog_buf, CATALOG_BUFFER_SIZE - 1);
                if (catalog_len > 0) {
                    catalog_buf[catalog_len] = '\0';
                    active_server = servers_to_try[s_idx];
                    ESP_LOGI(TAG, "Catalogo recebido com sucesso de %s (%d bytes)", active_server, catalog_len);
                    esp_http_client_close(client);
                    esp_http_client_cleanup(client);
                    break;
                }
            } else {
                ESP_LOGW(TAG, "Servidor %s retornou HTTP %d para catalogo", servers_to_try[s_idx], status);
            }
            esp_http_client_close(client);
        } else {
            ESP_LOGW(TAG, "Falha de conexao com %s: %s", servers_to_try[s_idx], esp_err_to_name(err));
        }
        esp_http_client_cleanup(client);
    }

    if (catalog_len <= 0 || !active_server) {
        ESP_LOGE(TAG, "Nao foi possivel obter catalogo de nenhum servidor.");
        set_state(PODCAST_SYNC_ERROR, "Servidor offline");
        free(catalog_buf);
        free(sync_buf);
        if (wifi_was_started_by_sync) wifi_transfer_request_exit();
        s_is_busy = false;
        vTaskDelete(NULL);
        return;
    }

    // 4. Parse do Catalogo
    podcast_item_t *items = (podcast_item_t *)heap_caps_malloc(
        sizeof(podcast_item_t) * PODCAST_MAX_EPISODES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int total_available = parse_catalog_json(catalog_buf, items, PODCAST_MAX_EPISODES);
    free(catalog_buf); // Libera buffer do JSON

    ESP_LOGI(TAG, "Catalogo processado: %d podcast(s) disponiveis no servidor.", total_available);

    ensure_dir_exists(PODCAST_BASE_DIR);

    // 5. Identificar episodios novos ou incompletos
    int items_to_download[PODCAST_MAX_EPISODES];
    size_t resume_offsets[PODCAST_MAX_EPISODES];
    int download_count = 0;

    for (int i = 0; i < total_available; i++) {
        char prog_dir[256];
        snprintf(prog_dir, sizeof(prog_dir), "%s/%s", PODCAST_BASE_DIR, items[i].program);
        ensure_dir_exists(prog_dir);

        char final_file[384];
        snprintf(final_file, sizeof(final_file), "%s/%s", prog_dir, items[i].filename);

        char part_file[384];
        snprintf(part_file, sizeof(part_file), "%s/%s.part", prog_dir, items[i].filename);

        struct stat st_final;
        if (stat(final_file, &st_final) == 0 && (size_t)st_final.st_size == items[i].size_bytes) {
            // Arquivo final ja existe e tem tamanho integro: pula!
            continue;
        }

        struct stat st_part;
        size_t offset = 0;
        if (stat(part_file, &st_part) == 0 && (size_t)st_part.st_size < items[i].size_bytes) {
            offset = (size_t)st_part.st_size;
            ESP_LOGI(TAG, "Arquivo parcial detectado (%u de %u B). Retomando download...",
                     (unsigned)offset, (unsigned)items[i].size_bytes);
        }

        // Limita a sincronizacao automatica aos episodios mais recentes por programa
        int count_prog = 0;
        for (int k = 0; k < download_count; k++) {
            if (strcmp(items[items_to_download[k]].program, items[i].program) == 0) {
                count_prog++;
            }
        }
        if (count_prog >= PODCAST_MAX_RECENT_PER_PROGRAM) {
            ESP_LOGD(TAG, "Ignorando '%s' (limite de %d episodios recentes atingido para '%s')",
                     items[i].filename, PODCAST_MAX_RECENT_PER_PROGRAM, items[i].program);
            continue;
        }

        items_to_download[download_count] = i;
        resume_offsets[download_count] = offset;
        download_count++;
    }

    ESP_LOGI(TAG, "Episodios necessitando download: %d", download_count);

    if (download_count == 0) {
        set_state(PODCAST_SYNC_FINISHED, "Tudo atualizado!");
        ESP_LOGI(TAG, "Todos os podcasts ja estao atualizados.");
        free(items);
        free(sync_buf);
        s_is_busy = false;
        vTaskDelete(NULL);
        return;
    }

    // 6. Download dos novos episodios
    set_state(PODCAST_SYNC_DOWNLOADING, "Baixando episodios...");
    if (xSemaphoreTake(s_sync_mutex, portMAX_DELAY) == pdTRUE) {
        s_progress.total_count = download_count;
        xSemaphoreGive(s_sync_mutex);
    }

    for (int d = 0; d < download_count && !s_cancel_requested; d++) {
        int idx = items_to_download[d];
        size_t start_offset = resume_offsets[d];
        podcast_item_t *cur = &items[idx];

        char prog_dir[256];
        snprintf(prog_dir, sizeof(prog_dir), "%s/%s", PODCAST_BASE_DIR, cur->program);
        ensure_dir_exists(prog_dir);

        char final_file[384];
        snprintf(final_file, sizeof(final_file), "%s/%s", prog_dir, cur->filename);
        char part_file[384];
        snprintf(part_file, sizeof(part_file), "%s/%s.part", prog_dir, cur->filename);

        // Atualiza progresso da UI
        if (xSemaphoreTake(s_sync_mutex, portMAX_DELAY) == pdTRUE) {
            s_progress.current_idx = d + 1;
            strncpy(s_progress.current_program, cur->program, sizeof(s_progress.current_program) - 1);
            strncpy(s_progress.current_title, cur->title, sizeof(s_progress.current_title) - 1);
            s_progress.current_file_bytes = cur->size_bytes;
            s_progress.current_downloaded_bytes = start_offset;
            s_progress.current_pct = (int)((start_offset * 100) / cur->size_bytes);
            xSemaphoreGive(s_sync_mutex);
        }

        // Monta URL de download com encode de espacos
        char enc_path[256];
        url_encode(cur->rel_path, enc_path, sizeof(enc_path));
        char download_url[384];
        snprintf(download_url, sizeof(download_url), "%s/podcasts/%s", active_server, enc_path);

        ESP_LOGI(TAG, "[%d/%d] Baixando '%s' de %s (Offset=%u B)",
                 d + 1, download_count, cur->filename, download_url, (unsigned)start_offset);

        esp_http_client_config_t dl_cfg = {
            .url = download_url,
            .timeout_ms = 10000,
            .buffer_size = 32768,
        };
        esp_http_client_handle_t dl_client = esp_http_client_init(&dl_cfg);
        if (!dl_client) {
            ESP_LOGE(TAG, "Falha ao inicializar cliente HTTP para %s", cur->filename);
            continue;
        }

        if (start_offset > 0) {
            char range_hdr[48];
            snprintf(range_hdr, sizeof(range_hdr), "bytes=%u-", (unsigned)start_offset);
            esp_http_client_set_header(dl_client, "Range", range_hdr);
        }

        esp_err_t err = esp_http_client_open(dl_client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao conectar no endpoint do arquivo: %s", esp_err_to_name(err));
            esp_http_client_cleanup(dl_client);
            continue;
        }

        esp_http_client_fetch_headers(dl_client);
        int http_status = esp_http_client_get_status_code(dl_client);
        if (http_status != 200 && http_status != 206) {
            ESP_LOGE(TAG, "Servidor retornou HTTP %d para download de %s", http_status, cur->filename);
            esp_http_client_close(dl_client);
            esp_http_client_cleanup(dl_client);
            continue;
        }

        FILE *fp = fopen(part_file, start_offset > 0 ? "ab" : "wb");
        if (!fp) {
            ESP_LOGE(TAG, "Falha ao abrir arquivo local: %s", part_file);
            esp_http_client_close(dl_client);
            esp_http_client_cleanup(dl_client);
            continue;
        }

        char *io_buf = (char *)heap_caps_malloc(65536, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (io_buf) {
            setvbuf(fp, io_buf, _IOFBF, 65536);
        }

        size_t total_written = start_offset;
        uint32_t speed_start_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        size_t bytes_since_speed = 0;

        while (!s_cancel_requested && total_written < cur->size_bytes) {
            int read_len = esp_http_client_read(dl_client, (char *)sync_buf, SYNC_BUFFER_SIZE);
            if (read_len <= 0) {
                break; // Concluido ou timeout
            }

            size_t written = fwrite(sync_buf, 1, (size_t)read_len, fp);
            total_written += written;
            bytes_since_speed += written;

            // Calcula velocidade a cada 1 segundo
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
            uint32_t speed_elapsed = now_ms - speed_start_ms;
            float cur_speed = 0.0f;
            if (speed_elapsed >= 1000) {
                cur_speed = (float)bytes_since_speed / (float)speed_elapsed; // KB/s
                speed_start_ms = now_ms;
                bytes_since_speed = 0;
            }

            // Atualiza progresso atomico
            if (xSemaphoreTake(s_sync_mutex, 0) == pdTRUE) {
                s_progress.current_downloaded_bytes = total_written;
                s_progress.current_pct = (int)((total_written * 100) / cur->size_bytes);
                if (cur_speed > 0.0f) s_progress.speed_kbs = cur_speed;
                xSemaphoreGive(s_sync_mutex);
            }

            taskYIELD();
        }

        fclose(fp);
        if (io_buf) free(io_buf);
        esp_http_client_close(dl_client);
        esp_http_client_cleanup(dl_client);

        // Se completou 100%, renomeia de .part para o arquivo definitivo
        if (total_written >= cur->size_bytes) {
            rename(part_file, final_file);
            ESP_LOGI(TAG, "Episodio '%s' baixado e salvo com sucesso (%u B)!",
                     cur->filename, (unsigned)total_written);
        } else {
            ESP_LOGW(TAG, "Download incompleto (%u de %u B). Salvo como .part para retomada futura.",
                     (unsigned)total_written, (unsigned)cur->size_bytes);
        }
    }

    free(items);
    free(sync_buf);

    if (s_cancel_requested) {
        set_state(PODCAST_SYNC_IDLE, "Cancelado");
    } else {
        set_state(PODCAST_SYNC_FINISHED, "Concluido com sucesso!");
        ESP_LOGI(TAG, "Sincronizacao de podcasts finalizada com sucesso!");
    }

    if (wifi_was_started_by_sync) {
        ESP_LOGI(TAG, "Desativando Wi-Fi apos sincronizacao de podcasts...");
        wifi_transfer_request_exit();
    } else {
        audio_player_reacquire_sd_after_usb();
    }

    s_is_busy = false;
    vTaskDelete(NULL);
}

esp_err_t podcast_sync_init(void)
{
    if (!s_sync_mutex) {
        s_sync_mutex = xSemaphoreCreateMutex();
    }
    memset(&s_progress, 0, sizeof(s_progress));
    s_progress.state = PODCAST_SYNC_IDLE;
    strcpy(s_progress.status_msg, "Pronto");

    // Le URL da NVS se existente
    nvs_handle_t h;
    if (nvs_open("podcast", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_configured_url);
        nvs_get_str(h, "server_url", s_configured_url, &len);
        nvs_close(h);
    }

    ESP_LOGI(TAG, "podcast_sync inicializado (Servidor: %s)", s_configured_url);
    return ESP_OK;
}

esp_err_t podcast_sync_start(void)
{
    if (s_is_busy) {
        ESP_LOGW(TAG, "Sincronizacao ja esta em andamento.");
        return ESP_ERR_INVALID_STATE;
    }
    podcast_sync_init();
    BaseType_t ret = xTaskCreatePinnedToCore(
        podcast_sync_task, "podcast_sync", 10240, NULL, 5, &s_sync_task_handle, 1);
    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

void podcast_sync_cancel(void)
{
    s_cancel_requested = true;
}

bool podcast_sync_is_busy(void)
{
    return s_is_busy;
}

void podcast_sync_get_progress(podcast_sync_progress_t *out)
{
    if (!out || !s_sync_mutex) return;
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

