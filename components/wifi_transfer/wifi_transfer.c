#include "wifi_transfer.h"
#include "sd_card.h"
#include "audio_player.h"
#include "oled_display.h"
#include "menu.h"

#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_partition.h"
#include "esp_timer.h"

#include "eq.h"
#include "podcast_sync.h"
#include "podcast_history.h"
#include "psa/crypto.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include "nvs_flash.h"
#include <lwip/sockets.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <mdns.h>
#include <lwip/tcp.h>
#include <lwip/tcpip.h>
#include <lwip/priv/tcp_priv.h>
#include <lwip/udp.h>

static const char *TAG = "wifi_transfer";

// =========================================================================
// Web Radio Server (TCP Port 8000)
// =========================================================================
#include "freertos/ringbuf.h"
#include <fcntl.h>

#if 0 // EXPERIMENTAL_RADIO_SERVER (desativado temporariamente)
#define RADIO_PORT 8000
#define MAX_RADIO_CLIENTS 5
static int s_radio_clients[MAX_RADIO_CLIENTS];
static SemaphoreHandle_t s_radio_clients_mux = NULL;
static RingbufHandle_t s_radio_ringbuf = NULL;
static TaskHandle_t s_radio_task_handle = NULL;
static int s_radio_listen_sock = -1;

extern void (*g_radio_pcm_hook)(const int16_t *data, size_t len);

static void radio_pcm_hook_impl(const int16_t *data, size_t len) {
    if (s_radio_ringbuf) {
        xRingbufferSend(s_radio_ringbuf, (void*)data, len, 0); // Don't block!
    }
}

// Minimal WAV header for 44100Hz 16-bit Stereo
static const uint8_t wav_header[44] = {
    'R','I','F','F',
    0xFF, 0xFF, 0xFF, 0x7F, // chunk size (fake huge size for streaming)
    'W','A','V','E',
    'f','m','t',' ',
    16, 0, 0, 0, // Subchunk1Size
    1, 0, // AudioFormat (PCM)
    2, 0, // NumChannels (Stereo)
    0x44, 0xAC, 0x00, 0x00, // SampleRate (44100)
    0x10, 0xB1, 0x02, 0x00, // ByteRate (176400)
    4, 0, // BlockAlign
    16, 0, // BitsPerSample
    'd','a','t','a',
    0xFF, 0xFF, 0xFF, 0x7F // chunk size
};

static void radio_server_task(void *arg) {
    s_radio_ringbuf = xRingbufferCreate(64 * 1024, RINGBUF_TYPE_BYTEBUF);
    s_radio_clients_mux = xSemaphoreCreateMutex();
    for(int i=0; i<MAX_RADIO_CLIENTS; i++) s_radio_clients[i] = -1;
    
    // g_radio_pcm_hook = radio_pcm_hook_impl;

    s_radio_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    
    // Set listening socket to non-blocking
    int flags = fcntl(s_radio_listen_sock, F_GETFL, 0);
    fcntl(s_radio_listen_sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(RADIO_PORT);
    
    bind(s_radio_listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    listen(s_radio_listen_sock, MAX_RADIO_CLIENTS);
    
    ESP_LOGI(TAG, "Web Radio Server rodando na porta %d", RADIO_PORT);

    while (1) {
        // Aceitar novos clientes
        struct sockaddr_in source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(s_radio_listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock >= 0) {
            ESP_LOGI(TAG, "Novo ouvinte da Rádio conectado!");
            
            // Set client socket to non-blocking
            int cflags = fcntl(sock, F_GETFL, 0);
            fcntl(sock, F_SETFL, cflags | O_NONBLOCK);
            
            // Envia cabeçalho HTTP + WAV Head
            const char *http_hdr = "HTTP/1.0 200 OK\r\nContent-Type: audio/wav\r\nConnection: close\r\n\r\n";
            send(sock, http_hdr, strlen(http_hdr), 0);
            send(sock, wav_header, sizeof(wav_header), 0);
            
            xSemaphoreTake(s_radio_clients_mux, portMAX_DELAY);
            bool added = false;
            for(int i=0; i<MAX_RADIO_CLIENTS; i++) {
                if (s_radio_clients[i] == -1) {
                    s_radio_clients[i] = sock;
                    added = true;
                    break;
                }
            }
            xSemaphoreGive(s_radio_clients_mux);
            if (!added) {
                close(sock);
            }
        }

        // Puxar áudio do RingBuffer e enviar pra todos
        size_t item_size = 0;
        void *item = xRingbufferReceiveUpTo(s_radio_ringbuf, &item_size, pdMS_TO_TICKS(10), 4096);
        if (item) {
            xSemaphoreTake(s_radio_clients_mux, portMAX_DELAY);
            for(int i=0; i<MAX_RADIO_CLIENTS; i++) {
                if (s_radio_clients[i] != -1) {
                    int sent = send(s_radio_clients[i], item, item_size, 0);
                    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        // Erro real (desconexão)
                        ESP_LOGI(TAG, "Ouvinte %d desconectado", i);
                        close(s_radio_clients[i]);
                        s_radio_clients[i] = -1;
                    }
                }
            }
            xSemaphoreGive(s_radio_clients_mux);
            vRingbufferReturnItem(s_radio_ringbuf, item);
        } else {
            // Se não tem áudio e não aceitou cliente, dá yield
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}
#endif // EXPERIMENTAL_RADIO_SERVER


esp_err_t wifi_transfer_enter_auto(void);
static wifi_transfer_mode_t s_mode;

// =========================================================================
// HTML embutido (Compactado com Gzip)
// =========================================================================
#include "index_html_gz.h"
#include "battery.h"


static inline void *http_scratch_alloc(size_t sz)
{
    void *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(sz);
    if (p) memset(p, 0, sz);
    return p;
}

static bool s_progress_active = false;
static uint32_t s_status_last_calc_ms = 0;

static esp_err_t api_now_playing_get_handler(httpd_req_t *req)
{
    typedef struct {
        playback_state_t st;
        char json[512];
    } np_scratch_t;

    np_scratch_t *sc = (np_scratch_t *)http_scratch_alloc(sizeof(np_scratch_t));
    if (!sc) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    audio_player_get_state(&sc->st);

    snprintf(sc->json, sizeof(sc->json), "{\"state\":\"%s\", \"title\":\"%s\", \"artist\":\"%s\"}",
        sc->st.playing ? "playing" : "stopped",
        sc->st.title,
        sc->st.artist);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, sc->json, -1);
    free(sc);
    return ESP_OK;
}

static esp_err_t http_captive_204_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t http_captive_apple_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    const char *apple_resp = "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";
    httpd_resp_send(req, apple_resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    if (s_mode == WIFI_TRANSFER_MODE_AP || s_mode == WIFI_TRANSFER_MODE_APSTA) {
        // Redireciona qualquer URL desconhecida para o IP do AP
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "404 Not Found");
    return ESP_FAIL;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    esp_err_t ret = httpd_resp_send(req, (const char *)index_html_gz, index_html_gz_size);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Falha ao enviar HTML gzip (%d bytes): %s", (int)index_html_gz_size, esp_err_to_name(ret));
    }
    return ret;
}



static esp_err_t api_status_get_handler(httpd_req_t *req)
{
    static uint64_t cached_total = 0, cached_free = 0;
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    
    // esp_vfs_fat_info é pesadíssimo: nunca executa durante upload ativo (s_progress_active)
    // e só recalcula 1x por minuto ou após conclusão de upload/exclusão (s_status_last_calc_ms == 0).
    if (!s_progress_active && (cached_total == 0 || s_status_last_calc_ms == 0 || (now - s_status_last_calc_ms > 60000))) {
        esp_vfs_fat_info(SD_MOUNT_POINT, &cached_total, &cached_free);
        s_status_last_calc_ms = now ? now : 1;
    }
    uint64_t total_bytes = cached_total;
    uint64_t free_bytes = cached_free;
    
    int mv = 0, percent = 0, time_left = -1;
    battery_get_info(&mv, &percent, &time_left);

    char json[192];
    snprintf(json, sizeof(json),
             "{\"total\":%llu,\"free\":%llu,\"battery_mv\":%d,\"battery_percent\":%d,\"battery_time_left\":%d,\"sd_status\":\"%s\"}",
             (unsigned long long)total_bytes, (unsigned long long)free_bytes, mv, percent, time_left,
             s_progress_active ? "writing" : "free");
             
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

static esp_err_t api_space_get_handler(httpd_req_t *req)
{
    uint64_t total_bytes = 0, free_bytes = 0;
    esp_err_t err = esp_vfs_fat_info(SD_MOUNT_POINT, &total_bytes, &free_bytes);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_fat_info falhou: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nao foi possivel ler o espaco do cartao");
        return ESP_FAIL;
    }

    char json[96];
    snprintf(json, sizeof(json), "{\"total\":%llu,\"free\":%llu}",
             (unsigned long long)total_bytes, (unsigned long long)free_bytes);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

#define WIFI_STA_MAX_RETRY 5

// --- Estado --------------------------------------------------------------
static bool s_stack_ready = false;
static esp_netif_t *s_netif_sta = NULL;
static esp_netif_t *s_netif_ap = NULL;
static httpd_handle_t s_httpd = NULL;

static volatile bool s_active = false;
static volatile bool s_exit_requested = false;
static volatile bool s_got_ip = false;
static volatile bool s_ap_ready = false;
static volatile bool s_sta_failed = false;
static int s_sta_retry = 0;
static volatile int s_files_received = 0;
static char s_status[64] = "";

// Controle de fallback automÃ¡tico STA -> AP
static bool s_auto_fallback = false;

static int s_dns_socket = -1;
static TaskHandle_t s_dns_task_handle = NULL;

// --- Progresso -----------------------------------------------------------
static SemaphoreHandle_t s_progress_mutex = NULL;
static char s_progress_name[64] = "";
static size_t s_progress_file_done = 0;
static size_t s_progress_file_total = 0;
static uint32_t s_progress_file_start_ms = 0;
static int s_progress_idx = 1;
static int s_progress_count = 1;
static long long s_progress_batch_total = 0;
static long long s_progress_batch_done_before = 0;

typedef enum {
    WIFI_UI_IDLE,
    WIFI_UI_CONNECTED,
    WIFI_UI_TRANSFERRING,
    WIFI_UI_DONE,
    WIFI_UI_OTA_UPDATING,
    WIFI_UI_OTA_FINISHED
} wifi_ui_state_t;

static wifi_ui_state_t s_ui_state = WIFI_UI_IDLE;
static uint32_t s_done_show_until_ms = 0;
static wifi_ui_state_t s_wifi_last_drawn_state = (wifi_ui_state_t)-1;
static uint32_t s_wifi_last_draw_ms = 0;

static volatile bool s_ota_in_progress = false;
static volatile int s_ota_pct = 0;
static char s_ota_status[32] = {0};

bool wifi_transfer_is_dns_active(void) {
    return s_dns_socket >= 0;
}

wifi_transfer_mode_t wifi_transfer_get_mode(void) {
    return s_mode;
}

int wifi_transfer_get_connected_clients(void) {
    if (s_mode != WIFI_TRANSFER_MODE_AP && s_mode != WIFI_TRANSFER_MODE_APSTA) return -1;
    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        return sta_list.num;
    }
    return 0;
}

static void update_ui_state(void) {
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    // Se estiver no estado DONE e o tempo expirou, volta para CONNECTED
    if (s_ui_state == WIFI_UI_DONE && now > s_done_show_until_ms) {
        s_ui_state = WIFI_UI_CONNECTED;
        return;
    }

    // Se estiver transferindo ou em atualizacao OTA, não muda
    if (s_ui_state == WIFI_UI_TRANSFERRING || s_ui_state == WIFI_UI_OTA_UPDATING || s_ui_state == WIFI_UI_OTA_FINISHED) return;

    if (s_mode == WIFI_TRANSFER_MODE_STA) {
        if (s_got_ip) {
            if (s_ui_state == WIFI_UI_IDLE) s_ui_state = WIFI_UI_CONNECTED;
        } else {
            if (s_ui_state == WIFI_UI_CONNECTED) s_ui_state = WIFI_UI_IDLE;
        }
    } else { // AP ou APSTA
        int clients = wifi_transfer_get_connected_clients();
        if (clients > 0 || (s_mode == WIFI_TRANSFER_MODE_APSTA && s_got_ip)) {
            if (s_ui_state == WIFI_UI_IDLE) s_ui_state = WIFI_UI_CONNECTED;
        } else {
            if (s_ui_state == WIFI_UI_CONNECTED) s_ui_state = WIFI_UI_IDLE;
        }
    }
}

static bool s_mdns_started = false;
static char s_mdns_mac_str[18] = {0};

static void start_mdns_service(void)
{
    if (s_mdns_started) return;
    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Falha ao inicializar mDNS: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set("mps3");
    mdns_instance_name_set("mps3");
    if (!mdns_service_exists("_http", "_tcp", NULL)) {
        mdns_service_add("MPS3 Radio", "_http", "_tcp", 80, NULL, 0);
    }

    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) {
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }
    snprintf(s_mdns_mac_str, sizeof(s_mdns_mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    mdns_txt_item_t mps3_txt[] = {
        { "mac", s_mdns_mac_str },
        { "sync_ready", "1" },
        { "version", "2.0" }
    };
    if (!mdns_service_exists("_mps3", "_tcp", NULL)) {
        mdns_service_add("MPS3 Sync", "_mps3", "_tcp", 80, mps3_txt, 3);
    }

    s_mdns_started = true;
    ESP_LOGI(TAG, "mDNS iniciado: http://mps3.local (_mps3._tcp mac=%s)", s_mdns_mac_str);
}

static void stop_mdns_service(void)
{
    if (s_mdns_started) {
        mdns_free();
        s_mdns_started = false;
    }
}

static void dns_server_task(void *arg)
{
    (void)arg;
    struct sockaddr_in server_addr, client_addr;
    uint8_t buf[512];

    s_dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_socket < 0) {
        ESP_LOGE(TAG, "Falha ao criar socket DNS");
        vTaskDelete(NULL);
        return;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(53);
    if (bind(s_dns_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Falha ao bindar socket DNS");
        close(s_dns_socket);
        s_dns_socket = -1;
        vTaskDelete(NULL);
        return;
    }

    while (s_dns_socket >= 0) {
        socklen_t addr_len = sizeof(client_addr);
        int len = recvfrom(s_dns_socket, buf, sizeof(buf), 0,
                           (struct sockaddr *)&client_addr, &addr_len);
        if (len < 0) {
            ESP_LOGW(TAG, "DNS recvfrom failed, aborting task");
            break;
        }
        if (len < 12) continue;

        if ((buf[2] & 0x80) != 0) continue;
        uint16_t qdcount = (buf[4] << 8) | buf[5];
        if (qdcount != 1) continue;

        // Caminha pela secao Question para encontrar o fim exato (RFC 1035)
        int pos = 12;
        while (pos < len && buf[pos] != 0) {
            int label_len = buf[pos];
            if (pos + 1 + label_len > len) break;
            pos += label_len + 1;
        }
        if (pos >= len || buf[pos] != 0) continue;
        pos++; // Pula o terminador nulo de QNAME
        pos += 4; // Pula QTYPE (2 bytes) + QCLASS (2 bytes)
        if (pos > len) continue;

        // pos agora marca exatamente o fim da secao Question
        if (pos + 16 > 512) continue;

        uint8_t response[512];
        // Copia SOMENTE o Header (12 bytes) e a Question original
        memcpy(response, buf, pos);

        // Header da resposta (RFC 1035)
        response[2] = 0x81; // QR=1 (Response), Opcode=0, AA=0, TC=0, RD=1
        response[3] = 0x80; // RA=1 (Recursion Available), Z=0, RCODE=0 (No error)
        response[4] = 0x00; response[5] = 0x01; // QDCOUNT = 1
        response[6] = 0x00; response[7] = 0x01; // ANCOUNT = 1
        response[8] = 0x00; response[9] = 0x00; // NSCOUNT = 0
        response[10] = 0x00; response[11] = 0x00; // ARCOUNT = 0 (descarta EDNS0 OPT da consulta!)

        // Secao Answer imediatamente apos a Question
        int rsp_len = pos;
        response[rsp_len++] = 0xC0;
        response[rsp_len++] = 0x0C; // Pointer para QNAME em offset 12
        response[rsp_len++] = 0x00; response[rsp_len++] = 0x01; // TYPE A
        response[rsp_len++] = 0x00; response[rsp_len++] = 0x01; // CLASS IN
        response[rsp_len++] = 0x00; response[rsp_len++] = 0x00;
        response[rsp_len++] = 0x00; response[rsp_len++] = 0x3C; // TTL = 60s
        response[rsp_len++] = 0x00; response[rsp_len++] = 0x04; // RDLENGTH = 4

        // Obtem o IP do AP (192.168.4.1)
        esp_netif_ip_info_t ip_info;
        uint32_t ap_ip = htonl(0xC0A80401); // fallback 192.168.4.1
        if (s_netif_ap && esp_netif_get_ip_info(s_netif_ap, &ip_info) == ESP_OK) {
            ap_ip = ip_info.ip.addr;
        }
        memcpy(&response[rsp_len], &ap_ip, 4);
        rsp_len += 4;

        sendto(s_dns_socket, response, rsp_len, 0,
               (struct sockaddr *)&client_addr, addr_len);
    }
    if (s_dns_socket >= 0) {
        close(s_dns_socket);
        s_dns_socket = -1;
    }
    s_dns_task_handle = NULL;
    vTaskDelete(NULL);
}

static void progress_begin(const char *name, size_t file_total, int idx, int count, long long batch_total)
{
    if (!s_progress_mutex) return;
    xSemaphoreTake(s_progress_mutex, portMAX_DELAY);
    if (idx <= 1) {
        s_progress_batch_done_before = 0;
    }
    strncpy(s_progress_name, name, sizeof(s_progress_name) - 1);
    s_progress_name[sizeof(s_progress_name) - 1] = '\0';
    s_progress_file_done = 0;
    s_progress_file_total = file_total;
    s_progress_file_start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_progress_idx = idx;
    s_progress_count = count;
    s_progress_batch_total = batch_total;
    s_progress_active = true;
    xSemaphoreGive(s_progress_mutex);
    s_ui_state = WIFI_UI_TRANSFERRING;
}

static void progress_update(size_t done)
{
    s_progress_file_done = done;
}

static void progress_finish_file(size_t file_total)
{
    if (!s_progress_mutex) return;
    xSemaphoreTake(s_progress_mutex, portMAX_DELAY);
    s_progress_batch_done_before += (long long)file_total;
    xSemaphoreGive(s_progress_mutex);
}

static void progress_end(void)
{
    if (!s_progress_mutex) return;
    xSemaphoreTake(s_progress_mutex, portMAX_DELAY);
    s_progress_active = false;
    s_status_last_calc_ms = 0; // invalida cache para que o proximo /api/status mostre o espaco atualizado
    if (s_ui_state == WIFI_UI_TRANSFERRING) {
        s_ui_state = WIFI_UI_DONE;
        s_done_show_until_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) + 3000;
    }
    xSemaphoreGive(s_progress_mutex);
}

void wifi_transfer_get_progress(wifi_transfer_progress_t *out)
{
    memset(out, 0, sizeof(*out));
    out->eta_sec = -1;
    out->overall_pct = -1;
    out->total_eta_sec = -1;
    if (!s_progress_mutex) return;

    xSemaphoreTake(s_progress_mutex, portMAX_DELAY);
    out->in_progress = s_progress_active;
    if (s_progress_active) {
        strncpy(out->filename, s_progress_name, sizeof(out->filename) - 1);
        out->filename[sizeof(out->filename) - 1] = '\0';
        out->file_pct = s_progress_file_total > 0
            ? (int)(((uint64_t)s_progress_file_done * 100) / s_progress_file_total)
            : 0;

        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        uint32_t elapsed_ms = now_ms - s_progress_file_start_ms;
        if (elapsed_ms > 300) {
            out->kbps = ((float)s_progress_file_done / 1024.0f) / ((float)elapsed_ms / 1000.0f);
            if (out->kbps > 0.05f && s_progress_file_done < s_progress_file_total) {
                size_t remaining_bytes = s_progress_file_total - s_progress_file_done;
                out->eta_sec = (int)((float)remaining_bytes / 1024.0f / out->kbps);
            }
        }

        if (s_progress_count > 1 && s_progress_batch_total > 0) {
            long long overall_done = s_progress_batch_done_before + (long long)s_progress_file_done;
            out->overall_pct = (int)((overall_done * 100) / s_progress_batch_total);
            out->idx = s_progress_idx;
            out->count = s_progress_count;

            // Calcula total_eta_sec
            if (out->kbps > 0.05f) {
                long long remaining_batch = s_progress_batch_total - overall_done;
                out->total_eta_sec = (int)((float)remaining_batch / 1024.0f / out->kbps);
            } else {
                out->total_eta_sec = -1;
            }
        }
    }
    xSemaphoreGive(s_progress_mutex);
}

// --- Utilitarios de caminho -----------------------------------------------
static void url_decode(char *dst, const char *src, size_t dst_len)
{
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && di + 1 < dst_len; si++) {
        char c = src[si];
        if (c == '+') {
            dst[di++] = ' ';
        } else if (c == '%' && src[si + 1] != '\0' && src[si + 2] != '\0') {
            char hex[3] = { src[si + 1], src[si + 2], '\0' };
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
        } else {
            dst[di++] = c;
        }
    }
    dst[di] = '\0';

    // Higienizacao de caracteres especiais para FATFS:
    // O caractere Unicode '？' (Fullwidth Question Mark, U+FF1F) em UTF-8 eh: 0xEF 0xBC 0x9F.
    // O FAT32 nao aceita '?' e a codepage 850 nao suporta U+FF1F. Substituimos por '-'
    for (size_t i = 0; dst[i] != '\0'; i++) {
        if ((unsigned char)dst[i] == 0xEF && (unsigned char)dst[i+1] == 0xBC && (unsigned char)dst[i+2] == 0x9F) {
            dst[i] = '-';
            // Desloca os proximos bytes para eliminar os 2 bytes extras do caractere UTF-8 multibyte
            memmove(&dst[i+1], &dst[i+3], strlen(&dst[i+3]) + 1);
        } else if (dst[i] == '?' || dst[i] == '*' || dst[i] == ':' || dst[i] == '<' || dst[i] == '>' || dst[i] == '|' || dst[i] == '"') {
            dst[i] = '-';
        }
    }
}

static bool build_abs_path(const char *rel, char *out, size_t out_len)
{
    if (!rel || !out || out_len == 0) return false;
    while (*rel == '/') rel++;
    if (strstr(rel, "..") != NULL || strchr(rel, '\\') != NULL) return false;
    for (const unsigned char *p = (const unsigned char *)rel; *p != '\0'; p++) {
        if (*p < 0x20) return false;
    }
    int n;
    if (rel[0] == '\0') {
        n = snprintf(out, out_len, "%s", SD_MOUNT_POINT);
    } else {
        n = snprintf(out, out_len, "%s/%s", SD_MOUNT_POINT, rel);
    }
    return (n > 0 && (size_t)n < out_len);
}

static void mkdir_p_for_file(const char *abs_file_path)
{
    char *tmp = (char *)http_scratch_alloc(600);
    if (!tmp) return;
    strncpy(tmp, abs_file_path, 599);
    tmp[599] = '\0';
    size_t root_len = strlen(SD_MOUNT_POINT);
    for (size_t i = root_len + 1; tmp[i] != '\0'; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            struct stat st;
            if (stat(tmp, &st) != 0) {
                if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
                    ESP_LOGW(TAG, "mkdir falhou para '%s': errno=%d (%s)", tmp, errno, strerror(errno));
                } else {
                    ESP_LOGI(TAG, "Diretorio criado: '%s'", tmp);
                }
            }
            tmp[i] = '/';
        }
    }
    free(tmp);
}

static esp_err_t recursive_delete(const char *abs_path)
{
    struct stat st;
    if (stat(abs_path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!S_ISDIR(st.st_mode)) {
        return remove(abs_path) == 0 ? ESP_OK : ESP_FAIL;
    }

    DIR *d = opendir(abs_path);
    if (!d) return ESP_FAIL;
    struct dirent *entry;
    char *child = (char *)http_scratch_alloc(512);
    if (!child) {
        closedir(d);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        snprintf(child, 512, "%s/%s", abs_path, entry->d_name);
        err = recursive_delete(child);
        if (err != ESP_OK) break;
    }
    free(child);
    closedir(d);
    if (err != ESP_OK) return err;
    return rmdir(abs_path) == 0 ? ESP_OK : ESP_FAIL;
}

static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 2 < out_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c >= 0x20) {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

// --- Autenticacao opcional por X-MPS3-Key (NVS namespace "mps3_sync", key "api_key") ---
static char s_sync_api_key[65] = {0};
static bool s_sync_api_key_loaded = false;
static volatile bool s_sync_in_progress = false;

static bool check_api_key(httpd_req_t *req)
{
    if (!s_sync_api_key_loaded) {
        s_sync_api_key[0] = '\0';
        nvs_handle_t h;
        if (nvs_open("mps3_sync", NVS_READONLY, &h) == ESP_OK) {
            size_t len = sizeof(s_sync_api_key);
            if (nvs_get_str(h, "api_key", s_sync_api_key, &len) != ESP_OK) {
                s_sync_api_key[0] = '\0';
            }
            nvs_close(h);
        }
        s_sync_api_key_loaded = true;
    }

    if (s_sync_api_key[0] == '\0') {
        return true; // Chave vazia/ausente: 100% transparente
    }

    char provided[68] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-MPS3-Key", provided, sizeof(provided)) != ESP_OK) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"unauthorized\"}", HTTPD_RESP_USE_STRLEN);
        return false;
    }

    size_t expected_len = strlen(s_sync_api_key);
    size_t provided_len = strlen(provided);
    uint8_t diff = (expected_len != provided_len) ? 1 : 0;
    size_t cmp_len = (expected_len < provided_len) ? expected_len : provided_len;
    for (size_t i = 0; i < cmp_len; i++) {
        diff |= (uint8_t)(s_sync_api_key[i] ^ provided[i]);
    }

    if (diff != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"unauthorized\"}", HTTPD_RESP_USE_STRLEN);
        return false;
    }

    return true;
}

// --- Handlers HTTP ---------------------------------------------------------

static esp_err_t api_list_get_handler(httpd_req_t *req)
{
    typedef struct {
        char query[512];
        char path_enc[400];
        char rel_path[400];
        char abs_path[600];
        char name_esc[300];
        char chunk[420];
        char out_buf[4096];
    } list_scratch_t;

    list_scratch_t *sc = (list_scratch_t *)http_scratch_alloc(sizeof(list_scratch_t));
    if (!sc) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (httpd_req_get_url_query_str(req, sc->query, sizeof(sc->query)) == ESP_OK) {
        httpd_query_key_value(sc->query, "path", sc->path_enc, sizeof(sc->path_enc));
        url_decode(sc->rel_path, sc->path_enc, sizeof(sc->rel_path));
    }

    if (!build_abs_path(sc->rel_path, sc->abs_path, sizeof(sc->abs_path))) {
        free(sc);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "caminho invalido");
        return ESP_FAIL;
    }

    DIR *d = opendir(sc->abs_path);
    if (!d) {
        free(sc);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "pasta nao encontrada");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"entries\":[");

    struct dirent *entry;
    bool first = true;
    int out_len = 0;
    int files_yield = 0;

    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        
        bool is_dir = (entry->d_type == DT_DIR);
        long size = 0;
        
        json_escape(entry->d_name, sc->name_esc, sizeof(sc->name_esc));
        int len = snprintf(sc->chunk, sizeof(sc->chunk), "%s{\"name\":\"%s\",\"dir\":%s,\"size\":%ld}",
                 first ? "" : ",", sc->name_esc, is_dir ? "true" : "false", size);
                 
        if (out_len + len >= (int)sizeof(sc->out_buf) - 1) {
            httpd_resp_send_chunk(req, sc->out_buf, out_len);
            out_len = 0;
        }
        memcpy(sc->out_buf + out_len, sc->chunk, len);
        out_len += len;
        first = false;
        
        files_yield++;
        if (files_yield % 32 == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    closedir(d);
    
    if (out_len > 0) {
        httpd_resp_send_chunk(req, sc->out_buf, out_len);
    }
    free(sc);
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// --- Pipeline de upload: httpd (Core 1, recepcao TCP) -> RingBuffer PSRAM (3 MB) -> writer task (Core 0, SD DMA) ---
#define UPLOAD_RINGBUF_SIZE          (3 * 1024 * 1024)  // "pulmao" elastico de 3 MB em PSRAM
#define UPLOAD_RINGBUF_MIN           (64 * 1024)
#define UPLOAD_RECV_BUF_SIZE         (8 * 1024)         // 8 KB (mantem dados 100% no Cache L1 de 32 KB da PSRAM)
#define UPLOAD_BOUNCE_SIZE           (16 * 1024)        // 32 setores = 16 KB em SRAM DMA interna (3.58 MB/s medidos)
#define UPLOAD_LOCK_TIMEOUT_MS       5000
#define UPLOAD_RING_SEND_TIMEOUT_MS  60000

static SemaphoreHandle_t s_upload_lock = NULL;
static uint8_t *s_bounce_persist = NULL;   // bounce DMA reservado antes do esp_wifi_init
static size_t s_bounce_persist_sz = 0;
static RingbufHandle_t s_rb_persist = NULL; // RingBuffer de 3 MB em PSRAM reutilizado na sessao Wi-Fi
static size_t s_rb_persist_sz = 0;
static char *s_recv_buf_persist = NULL;     // Buffer de recepcao TCP de 32 KB em PSRAM reutilizado
static size_t s_recv_buf_persist_sz = 0;

#define UPLOAD_WRITER_STACK_SIZE     16384 // 16 KB de pilha em PSRAM (ampla folga para VFS e FatFS, 0 bytes de DRAM interna)

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t data[64];
    uint32_t datalen;
} mps3_sha256_ctx_t;

static const uint32_t s_sha256_k[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL, 0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
    0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL, 0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
    0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL, 0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL, 0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
    0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL, 0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL, 0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL, 0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
    0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL, 0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
};

#define MPS3_ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define MPS3_CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MPS3_MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define MPS3_EP0(x)       (MPS3_ROTR32(x, 2) ^ MPS3_ROTR32(x, 13) ^ MPS3_ROTR32(x, 22))
#define MPS3_EP1(x)       (MPS3_ROTR32(x, 6) ^ MPS3_ROTR32(x, 11) ^ MPS3_ROTR32(x, 25))
#define MPS3_SIG0(x)      (MPS3_ROTR32(x, 7) ^ MPS3_ROTR32(x, 18) ^ ((x) >> 3))
#define MPS3_SIG1(x)      (MPS3_ROTR32(x, 17) ^ MPS3_ROTR32(x, 19) ^ ((x) >> 10))

static void mps3_sha256_transform(mps3_sha256_ctx_t *ctx, const uint8_t data[64])
{
    uint32_t m[64];
    for (int i = 0, j = 0; i < 16; ++i, j += 4) {
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) |
               ((uint32_t)data[j + 2] << 8) | ((uint32_t)data[j + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        m[i] = MPS3_SIG1(m[i - 2]) + m[i - 7] + MPS3_SIG0(m[i - 15]) + m[i - 16];
    }

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h + MPS3_EP1(e) + MPS3_CH(e, f, g) + s_sha256_k[i] + m[i];
        uint32_t t2 = MPS3_EP0(a) + MPS3_MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void mps3_sha256_init(mps3_sha256_ctx_t *ctx)
{
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667UL; ctx->state[1] = 0xbb67ae85UL;
    ctx->state[2] = 0x3c6ef372UL; ctx->state[3] = 0xa54ff53aUL;
    ctx->state[4] = 0x510e527fUL; ctx->state[5] = 0x9b05688cUL;
    ctx->state[6] = 0x1f83d9abUL; ctx->state[7] = 0x5be0cd19UL;
}

static void mps3_sha256_update(mps3_sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
    size_t i = 0;
    if (ctx->datalen > 0) {
        size_t take = 64 - ctx->datalen;
        if (take > len) take = len;
        memcpy(ctx->data + ctx->datalen, data, take);
        ctx->datalen += (uint32_t)take;
        i += take;
        if (ctx->datalen == 64) {
            mps3_sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
    while (i + 64 <= len) {
        mps3_sha256_transform(ctx, data + i);
        ctx->bitlen += 512;
        i += 64;
    }
    if (i < len) {
        size_t rem = len - i;
        memcpy(ctx->data, data + i, rem);
        ctx->datalen = (uint32_t)rem;
    }
}

static void mps3_sha256_final(mps3_sha256_ctx_t *ctx, uint8_t hash[32])
{
    uint32_t i = ctx->datalen;
    ctx->bitlen += (uint64_t)ctx->datalen * 8ULL;

    ctx->data[i++] = 0x80;
    if (ctx->datalen < 56) {
        while (i < 56) ctx->data[i++] = 0x00;
    } else {
        while (i < 64) ctx->data[i++] = 0x00;
        mps3_sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }

    for (int b = 0; b < 8; ++b) {
        ctx->data[63 - b] = (uint8_t)((ctx->bitlen >> (b * 8)) & 0xFFU);
    }
    mps3_sha256_transform(ctx, ctx->data);

    for (int j = 0; j < 8; ++j) {
        hash[j * 4 + 0] = (uint8_t)((ctx->state[j] >> 24) & 0xFFU);
        hash[j * 4 + 1] = (uint8_t)((ctx->state[j] >> 16) & 0xFFU);
        hash[j * 4 + 2] = (uint8_t)((ctx->state[j] >> 8) & 0xFFU);
        hash[j * 4 + 3] = (uint8_t)(ctx->state[j] & 0xFFU);
    }
}

typedef struct {
    char abs_path[512];
    FILE *fp;
    RingbufHandle_t rb;
    SemaphoreHandle_t done_sem;
    uint8_t *bounce;
    size_t bounce_size;
    volatile bool producer_done;  // httpd ja empurrou todos os bytes (ou desistiu)
    volatile bool abort;          // descartar o restante sem gravar
    volatile bool failed;         // erro de abertura ou escrita no SD
    volatile int open_errno;      // errno caso fopen falhe
    volatile size_t written;      // bytes realmente gravados no SD
    mps3_sha256_ctx_t sha_ctx;    // Contexto SHA-256 zero-alloc embutido
    char sha256_hex[65];          // SHA-256 hex calculado on-the-fly durante a escrita no SD
    // Telemetria detalhada do consumidor (Core 0)
    int64_t open_us;
    int64_t wr_wait_us;
    int64_t memcpy_us;
    int64_t fwrite_us;
    int64_t fwrite_max_us;
    int64_t fclose_us;
    uint32_t fwrite_calls;
    uint32_t fwrite_slow_cnt;
} upload_ctx_t;

static upload_ctx_t s_upload_ctx;
static char s_last_upload_stats_json[512] = "{}\0";

static void upload_writer_task(void *arg)
{
    upload_ctx_t *ctx = (upload_ctx_t *)arg;
    uint8_t *bounce = ctx->bounce;
    size_t chunk_sz = ctx->bounce_size;
    size_t bounce_filled = 0;

    mps3_sha256_init(&ctx->sha_ctx);

    // Etapa 1 do Consumidor (Core 0): criar diretorios e abrir arquivo em paralelo
    // enquanto o Core 1 ja esta drenando a janela TCP inicial para a PSRAM!
    int64_t t_open0 = esp_timer_get_time();
    mkdir_p_for_file(ctx->abs_path);
    FILE *fp = fopen(ctx->abs_path, "wb");
    if (!fp) {
        ctx->open_errno = errno;
        ctx->failed = true;
        ctx->open_us = esp_timer_get_time() - t_open0;
        ESP_LOGE(TAG, "[WRITER] Nao foi possivel criar %s (errno=%d: %s)",
                 ctx->abs_path, ctx->open_errno, strerror(ctx->open_errno));
    } else {
        setvbuf(fp, NULL, _IONBF, 0);
        ctx->fp = fp;
        ctx->open_us = esp_timer_get_time() - t_open0;
    }

    while (true) {
        bool was_done = ctx->producer_done;
        size_t needed = chunk_sz - bounce_filled;
        size_t got = 0;
        void *item = NULL;

        if (needed > 0) {
            int64_t twait0 = esp_timer_get_time();
            TickType_t wait_ticks = was_done ? 0 : pdMS_TO_TICKS(10);
            item = xRingbufferReceiveUpTo(ctx->rb, &got, wait_ticks, needed);
            ctx->wr_wait_us += esp_timer_get_time() - twait0;
            if (item) {
                if (!ctx->abort && !ctx->failed) {
                    int64_t tmc0 = esp_timer_get_time();
                    memcpy(bounce + bounce_filled, item, got);
                    ctx->memcpy_us += esp_timer_get_time() - tmc0;
                    bounce_filled += got;
                }
                vRingbufferReturnItem(ctx->rb, item);
            }
        }

        // Escreve quando o bounce DMA encher (32 KB multi-bloco) OU no bloco final
        if (bounce_filled == chunk_sz || (bounce_filled > 0 && was_done && !item)) {
            if (!ctx->abort && !ctx->failed && ctx->fp) {
                int64_t tw0 = esp_timer_get_time();
                size_t w = fwrite(bounce, 1, bounce_filled, ctx->fp);
                int64_t twd = esp_timer_get_time() - tw0;
                ctx->fwrite_us += twd;
                ctx->fwrite_calls++;
                if (twd > ctx->fwrite_max_us) ctx->fwrite_max_us = twd;
                if (twd > 200000) ctx->fwrite_slow_cnt++;
                if (w != bounce_filled) {
                    ESP_LOGE(TAG, "[WRITER] fwrite falhou: tentou %zu bytes, escreveu %zu (errno=%d: %s)",
                             bounce_filled, w, errno, strerror(errno));
                    ctx->failed = true;
                } else {
                    mps3_sha256_update(&ctx->sha_ctx, bounce, bounce_filled);
                    ctx->written += bounce_filled;
                }
            }
            bounce_filled = 0;
            if ((ctx->fwrite_calls % 4) == 0) {
                vTaskDelay(1); // Cede 1 tick ao IDLE0 a cada 4 blocos (64-128 KB) para alimentar TWDT
            }
        }

        if (was_done && bounce_filled == 0 && !item) {
            break;
        }
    }

    if (ctx->fp) {
        int64_t t_close0 = esp_timer_get_time();
        fclose(ctx->fp);
        ctx->fp = NULL;
        ctx->fclose_us = esp_timer_get_time() - t_close0;
    }

    uint8_t digest[32] = {0};
    mps3_sha256_final(&ctx->sha_ctx, digest);
    for (int i = 0; i < 32; i++) {
        snprintf(&ctx->sha256_hex[i * 2], 3, "%02x", digest[i]);
    }
    ctx->sha256_hex[64] = '\0';

    xSemaphoreGive(ctx->done_sem);  // ultima coisa tocando ctx
    vTaskDelay(portMAX_DELAY);      // aguarda delecao externa pela task criadora
}

static esp_err_t api_upload_put_handler(httpd_req_t *req)
{
    if (!check_api_key(req)) return ESP_FAIL;
    podcast_sync_notify_activity();

    int64_t t_handler_start = esp_timer_get_time();

    if (s_ota_in_progress) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Atualizacao OTA em andamento");
        return ESP_FAIL;
    }

    audio_player_stop_url();

    typedef struct {
        char query[512];
        char path_enc[400];
        char rel_path[400];
        char abs_path[512];
        char fail_detail[128];
        char expected_sha256[68];
        char err_json[224];
    } upload_scratch_t;

    upload_scratch_t *sc = (upload_scratch_t *)http_scratch_alloc(sizeof(upload_scratch_t));
    if (!sc) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int idx = 1, count = 1;
    long long batch_total = 0;

    if (httpd_req_get_url_query_str(req, sc->query, sizeof(sc->query)) != ESP_OK ||
        httpd_query_key_value(sc->query, "path", sc->path_enc, sizeof(sc->path_enc)) != ESP_OK) {
        free(sc);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "faltou o parametro 'path'");
        return ESP_FAIL;
    }
    url_decode(sc->rel_path, sc->path_enc, sizeof(sc->rel_path));

    char num_buf[20];
    if (httpd_query_key_value(sc->query, "idx", num_buf, sizeof(num_buf)) == ESP_OK) idx = atoi(num_buf);
    if (httpd_query_key_value(sc->query, "count", num_buf, sizeof(num_buf)) == ESP_OK) count = atoi(num_buf);
    if (httpd_query_key_value(sc->query, "batchTotal", num_buf, sizeof(num_buf)) == ESP_OK) batch_total = atoll(num_buf);
    if (idx < 1) idx = 1;
    if (count < 1) count = 1;

    if (!build_abs_path(sc->rel_path, sc->abs_path, sizeof(sc->abs_path)) || sc->rel_path[0] == '\0') {
        free(sc);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "caminho invalido");
        return ESP_FAIL;
    }

    if (httpd_req_get_hdr_value_str(req, "X-Expected-SHA256", sc->expected_sha256, sizeof(sc->expected_sha256)) != ESP_OK) {
        sc->expected_sha256[0] = '\0';
    }

    const char *display_name = sc->rel_path;
    const char *slash = strrchr(sc->rel_path, '/');
    if (slash) display_name = slash + 1;

    if (!s_upload_lock) s_upload_lock = xSemaphoreCreateMutex();
    if (!s_upload_lock || xSemaphoreTake(s_upload_lock, pdMS_TO_TICKS(UPLOAD_LOCK_TIMEOUT_MS)) != pdTRUE) {
        free(sc);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "Outro upload em andamento", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    int64_t t_alloc0 = esp_timer_get_time();
    size_t content_len = req->content_len;
    progress_begin(display_name, content_len, idx, count, batch_total > 0 ? batch_total : (long long)content_len);

    // Bounce DMA: usa o buffer persistente reservado antes do Wi-Fi; senao tenta alocar dinamicamente.
    size_t bounce_sz = s_bounce_persist_sz;
    uint8_t *bounce = s_bounce_persist;
    bool bounce_dynamic = false;
    if (!bounce) {
        bounce_dynamic = true;
        bounce_sz = UPLOAD_BOUNCE_SIZE;
        bounce = (uint8_t *)heap_caps_aligned_alloc(16, bounce_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!bounce) {
            bounce_sz = 16 * 1024;
            bounce = (uint8_t *)heap_caps_aligned_alloc(16, bounce_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        }
        if (!bounce) {
            bounce_sz = 8 * 1024;
            bounce = (uint8_t *)heap_caps_aligned_alloc(16, bounce_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        }
        if (!bounce) {
            bounce_sz = 4 * 1024;
            bounce = (uint8_t *)heap_caps_aligned_alloc(16, bounce_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        }
    }

    // RingBuffer de 3 MB em PSRAM persistente durante a sessao Wi-Fi (0 ms de alocacao entre arquivos)
    if (!s_rb_persist) {
        size_t try_rb = UPLOAD_RINGBUF_SIZE;
        while (!s_rb_persist && try_rb >= UPLOAD_RINGBUF_MIN) {
            s_rb_persist = xRingbufferCreateWithCaps(try_rb, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (s_rb_persist) {
                s_rb_persist_sz = try_rb;
            } else {
                try_rb /= 2;
            }
        }
    } else {
        // Garante que o RingBuffer persistente esteja limpo antes de iniciar novo arquivo
        size_t dummy_sz = 0;
        void *dummy_item = NULL;
        while ((dummy_item = xRingbufferReceiveUpTo(s_rb_persist, &dummy_sz, 0, 65536)) != NULL) {
            vRingbufferReturnItem(s_rb_persist, dummy_item);
        }
    }

    // Buffer de recepcao TCP em PSRAM (preserva 100% da SRAM interna para o anel RX do Wi-Fi e lwIP)
    if (!s_recv_buf_persist) {
        s_recv_buf_persist_sz = UPLOAD_RECV_BUF_SIZE;
        s_recv_buf_persist = (char *)heap_caps_malloc(s_recv_buf_persist_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_recv_buf_persist) {
            s_recv_buf_persist_sz = 6144;
            s_recv_buf_persist = (char *)heap_caps_malloc(s_recv_buf_persist_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
    }

    memset(&s_upload_ctx, 0, sizeof(s_upload_ctx));
    strlcpy(s_upload_ctx.abs_path, sc->abs_path, sizeof(s_upload_ctx.abs_path));
    s_upload_ctx.bounce = bounce;
    s_upload_ctx.bounce_size = bounce_sz;
    s_upload_ctx.rb = s_rb_persist;
    s_upload_ctx.done_sem = xSemaphoreCreateBinary();

    // Pilha do escritor em PSRAM e TCB em DRAM interna (0 bytes de DRAM interna para stack)
    StaticTask_t *writer_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    StackType_t *writer_stack = (StackType_t *)heap_caps_aligned_alloc(16, UPLOAD_WRITER_STACK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    size_t recv_sz = s_recv_buf_persist_sz;
    char *buf = s_recv_buf_persist;

    TaskHandle_t th = NULL;
    bool writer_started = false;
    if (s_upload_ctx.rb && s_upload_ctx.done_sem && buf && bounce && writer_tcb && writer_stack) {
        th = xTaskCreateStaticPinnedToCore(
            upload_writer_task,
            "up_wr",
            UPLOAD_WRITER_STACK_SIZE,
            &s_upload_ctx,
            5, // Prioridade 5 no Core 0 (paralelismo real com httpd no Core 1)
            writer_stack,
            writer_tcb,
            0  // Core 0: paralelismo real com httpd no Core 1
        );
        writer_started = (th != NULL);
    }
    int64_t setup_alloc_us = esp_timer_get_time() - t_alloc0;

    if (!writer_started) {
        if (s_upload_ctx.done_sem) vSemaphoreDelete(s_upload_ctx.done_sem);
        if (bounce && bounce_dynamic) heap_caps_free(bounce);
        if (writer_stack) heap_caps_free(writer_stack);
        if (writer_tcb) heap_caps_free(writer_tcb);
        xSemaphoreGive(s_upload_lock);
        free(sc);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sem memoria para o pipeline de upload");
        progress_end();
        return ESP_FAIL;
    }

    size_t remaining = content_len;
    bool ok = true;
    int consecutive_timeouts = 0;
    const int max_consecutive_timeouts = 100;
    uint32_t last_prog_update_ms = 0;
    int64_t t_recv_us = 0, t_recv_max_us = 0, t_ring_us = 0;
    uint32_t n_recv = 0, n_recv_slow = 0, n_ring_calls = 0, n_ring_full = 0;
    strncpy(sc->fail_detail, "conexao interrompida durante o envio", sizeof(sc->fail_detail) - 1);

    // Produtor (Core 1): le para o buffer de recepcao e empurra imediatamente para o RingBuffer
    while (remaining > 0) {
        if (s_upload_ctx.failed) {
            if (s_upload_ctx.open_errno != 0) {
                snprintf(sc->fail_detail, sizeof(sc->fail_detail), "nao foi possivel criar o arquivo (%s)", strerror(s_upload_ctx.open_errno));
            } else {
                snprintf(sc->fail_detail, sizeof(sc->fail_detail), "fwrite SD erro (cartao cheio ou falha de escrita)");
            }
            ok = false;
            break;
        }

        int to_read = (int)recv_sz;
        if (to_read > (int)remaining) to_read = (int)remaining;

        int64_t tp0 = esp_timer_get_time();
        int received = httpd_req_recv(req, buf, to_read);
        int64_t trd = esp_timer_get_time() - tp0;
        t_recv_us += trd;
        if (trd > t_recv_max_us) t_recv_max_us = trd;
        if (trd > 50000) n_recv_slow++;
        n_recv++;

        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++consecutive_timeouts > max_consecutive_timeouts) {
                ESP_LOGE(TAG, "Upload de %s abortado: timeout demais seguidos", display_name);
                snprintf(sc->fail_detail, sizeof(sc->fail_detail), "timeout no recv (%d timeouts seguidos)", consecutive_timeouts);
                ok = false;
                break;
            }
            continue;
        }
        consecutive_timeouts = 0;
        if (received <= 0) {
            ESP_LOGE(TAG, "httpd_req_recv erro: %d", received);
            snprintf(sc->fail_detail, sizeof(sc->fail_detail), "httpd_req_recv erro ret=%d", received);
            ok = false;
            break;
        }

        int64_t tp1 = esp_timer_get_time();
        BaseType_t ring_ok = xRingbufferSend(s_upload_ctx.rb, buf, (size_t)received, pdMS_TO_TICKS(UPLOAD_RING_SEND_TIMEOUT_MS));
        int64_t trng = esp_timer_get_time() - tp1;
        t_ring_us += trng;
        n_ring_calls++;
        if (trng > 5000) n_ring_full++;
        if (ring_ok != pdTRUE) {
            ESP_LOGE(TAG, "RingBuffer travou (SD nao drena)");
            snprintf(sc->fail_detail, sizeof(sc->fail_detail), "SD nao esta drenando o buffer de upload");
            ok = false;
            break;
        }
        remaining -= (size_t)received;

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if (now_ms - last_prog_update_ms >= 250) {
            last_prog_update_ms = now_ms;
            progress_update(s_upload_ctx.written);
            podcast_sync_notify_activity();
        }
    }

    // Se o loop abortou com bytes residuais e nao houve falha, descarta; caso contrario ja empurrou tudo
    // Sinaliza fim ao escritor e mede o tempo de drenagem residual do RingBuffer para o SD
    int64_t t_drain0 = esp_timer_get_time();
    if (!ok) s_upload_ctx.abort = true;
    s_upload_ctx.producer_done = true;
    xSemaphoreTake(s_upload_ctx.done_sem, portMAX_DELAY);
    int64_t drain_us = esp_timer_get_time() - t_drain0;

    if (s_upload_ctx.failed) {
        if (ok) {
            if (s_upload_ctx.open_errno != 0) {
                snprintf(sc->fail_detail, sizeof(sc->fail_detail), "nao foi possivel criar o arquivo (%s)", strerror(s_upload_ctx.open_errno));
            } else {
                snprintf(sc->fail_detail, sizeof(sc->fail_detail), "fwrite SD erro (cartao cheio ou falha de escrita)");
            }
        }
        ok = false;
    }

    if (th) {
        vTaskDelete(th);
        th = NULL;
    }
    if (writer_stack) {
        heap_caps_free(writer_stack);
        writer_stack = NULL;
    }
    if (writer_tcb) {
        heap_caps_free(writer_tcb);
        writer_tcb = NULL;
    }
    if (s_upload_ctx.bounce) {
        if (bounce_dynamic) heap_caps_free(s_upload_ctx.bounce);
        s_upload_ctx.bounce = NULL;
    }
    vSemaphoreDelete(s_upload_ctx.done_sem);

    int64_t total_us = esp_timer_get_time() - t_handler_start;
    uint32_t total_ms = (uint32_t)(total_us / 1000);
    uint32_t alloc_ms = (uint32_t)(setup_alloc_us / 1000);
    uint32_t open_ms = (uint32_t)(s_upload_ctx.open_us / 1000);
    uint32_t recv_ms = (uint32_t)(t_recv_us / 1000);
    uint32_t recv_max_ms = (uint32_t)(t_recv_max_us / 1000);
    uint32_t ring_ms = (uint32_t)(t_ring_us / 1000);
    uint32_t wr_wait_ms = (uint32_t)(s_upload_ctx.wr_wait_us / 1000);
    uint32_t memcpy_ms = (uint32_t)(s_upload_ctx.memcpy_us / 1000);
    uint32_t fwrite_ms = (uint32_t)(s_upload_ctx.fwrite_us / 1000);
    uint32_t wr_calls = s_upload_ctx.fwrite_calls;
    uint32_t wr_avg_ms = wr_calls ? (fwrite_ms / wr_calls) : 0;
    uint32_t wr_max_ms = (uint32_t)(s_upload_ctx.fwrite_max_us / 1000);
    uint32_t wr_slow = s_upload_ctx.fwrite_slow_cnt;
    uint32_t drain_ms = (uint32_t)(drain_us / 1000);
    uint32_t fclose_ms = (uint32_t)(s_upload_ctx.fclose_us / 1000);
    float speed_mbs = (total_ms > 0) ? (((float)content_len / (1024.0f * 1024.0f)) / ((float)total_ms / 1000.0f)) : 0.0f;

    snprintf(s_last_upload_stats_json, sizeof(s_last_upload_stats_json),
             "{\"bytes\":%u,\"total_ms\":%u,\"speed_mbs\":%.2f,\"alloc_ms\":%u,\"open_ms\":%u,"
             "\"recv_ms\":%u,\"recv_calls\":%u,\"recv_max_ms\":%u,\"recv_slow\":%u,"
             "\"ring_ms\":%u,\"ring_calls\":%u,\"ring_full\":%u,\"wr_wait_ms\":%u,\"memcpy_ms\":%u,"
             "\"fwrite_ms\":%u,\"wr_calls\":%u,\"wr_avg_ms\":%u,\"wr_max_ms\":%u,\"wr_slow\":%u,"
             "\"drain_ms\":%u,\"fclose_ms\":%u,\"rb_kb\":%u,\"bounce_kb\":%u,\"recv_kb\":%u,\"sha256\":\"%s\"}",
             (unsigned)content_len, (unsigned)total_ms, speed_mbs, (unsigned)alloc_ms, (unsigned)open_ms,
             (unsigned)recv_ms, (unsigned)n_recv, (unsigned)recv_max_ms, (unsigned)n_recv_slow,
             (unsigned)ring_ms, (unsigned)n_ring_calls, (unsigned)n_ring_full, (unsigned)wr_wait_ms, (unsigned)memcpy_ms,
             (unsigned)fwrite_ms, (unsigned)wr_calls, (unsigned)wr_avg_ms, (unsigned)wr_max_ms, (unsigned)wr_slow,
             (unsigned)drain_ms, (unsigned)fclose_ms,
             (unsigned)(s_rb_persist_sz / 1024), (unsigned)(bounce_sz / 1024), (unsigned)(recv_sz / 1024),
             s_upload_ctx.sha256_hex);

    ESP_LOGW(TAG, "[TELEMETRY] %s", s_last_upload_stats_json);

    xSemaphoreGive(s_upload_lock);

    if (!ok) {
        remove(sc->abs_path);
        ESP_LOGE(TAG, "Envio de %s interrompido: %s", display_name, sc->fail_detail);
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, sc->fail_detail);
        free(sc);
        progress_end();
        return ESP_FAIL;
    }

    if (sc->expected_sha256[0] != '\0' && strcasecmp(sc->expected_sha256, s_upload_ctx.sha256_hex) != 0) {
        remove(sc->abs_path);
        ESP_LOGE(TAG, "SHA256 mismatch em %s: esperado=%s, calculado=%s",
                 sc->rel_path, sc->expected_sha256, s_upload_ctx.sha256_hex);
        snprintf(sc->err_json, sizeof(sc->err_json),
                 "{\"error\":\"sha256_mismatch\",\"expected\":\"%s\",\"actual\":\"%s\"}",
                 sc->expected_sha256, s_upload_ctx.sha256_hex);
        httpd_resp_set_status(req, "422 Unprocessable Entity");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "X-Content-SHA256", s_upload_ctx.sha256_hex);
        httpd_resp_set_hdr(req, "X-Upload-Stats", s_last_upload_stats_json);
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_send(req, sc->err_json, HTTPD_RESP_USE_STRLEN);
        free(sc);
        progress_end();
        return ESP_FAIL;
    }

    progress_finish_file(content_len);
    if (idx >= count) progress_end();

    s_files_received++;
    ESP_LOGI(TAG, "Recebido: %s (%d/%d) em %u ms (%.2f MB/s) sha256=%s",
             sc->rel_path, idx, count, (unsigned)total_ms, speed_mbs, s_upload_ctx.sha256_hex);
    snprintf(sc->err_json, sizeof(sc->err_json),
             "{\"status\":\"ok\",\"bytes\":%u,\"sha256\":\"%s\"}",
             (unsigned)content_len, s_upload_ctx.sha256_hex);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "X-Content-SHA256", s_upload_ctx.sha256_hex);
    httpd_resp_set_hdr(req, "X-Upload-Stats", s_last_upload_stats_json);
    httpd_resp_send(req, sc->err_json, HTTPD_RESP_USE_STRLEN);
    free(sc);
    return ESP_OK;
}

static esp_err_t api_download_get_handler(httpd_req_t *req)
{
    typedef struct {
        char query[512];
        char path_enc[400];
        char rel_path[400];
        char abs_path[600];
        char range_hdr[64];
        char crange_str[128];
    } dl_scratch_t;

    dl_scratch_t *sc = (dl_scratch_t *)http_scratch_alloc(sizeof(dl_scratch_t));
    if (!sc) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (httpd_req_get_url_query_str(req, sc->query, sizeof(sc->query)) != ESP_OK ||
        httpd_query_key_value(sc->query, "path", sc->path_enc, sizeof(sc->path_enc)) != ESP_OK) {
        free(sc);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "faltou o parametro 'path'");
        return ESP_FAIL;
    }
    url_decode(sc->rel_path, sc->path_enc, sizeof(sc->rel_path));

    if (!build_abs_path(sc->rel_path, sc->abs_path, sizeof(sc->abs_path)) || sc->rel_path[0] == '\0') {
        free(sc);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "caminho invalido");
        return ESP_FAIL;
    }

    if (s_progress_active) {
        free(sc);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "Upload em andamento no SD", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    struct stat st;
    if (stat(sc->abs_path, &st) != 0 || S_ISDIR(st.st_mode)) {
        free(sc);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Arquivo nao encontrado ou e diretorio");
        return ESP_FAIL;
    }

    FILE *fp = fopen(sc->abs_path, "rb");
    if (!fp) {
        free(sc);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Falha ao abrir arquivo");
        return ESP_FAIL;
    }

    fseek(fp, 0, SEEK_END);
    size_t fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (strstr(sc->abs_path, ".flac") || strstr(sc->abs_path, ".FLAC")) httpd_resp_set_type(req, "audio/flac");
    else if (strstr(sc->abs_path, ".wav") || strstr(sc->abs_path, ".WAV")) httpd_resp_set_type(req, "audio/wav");
    else if (strstr(sc->abs_path, ".m4a") || strstr(sc->abs_path, ".M4A")) httpd_resp_set_type(req, "audio/mp4");
    else httpd_resp_set_type(req, "audio/mpeg");

    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");

    if (fsize == 0) {
        free(sc);
        httpd_resp_send(req, NULL, 0);
        fclose(fp);
        return ESP_OK;
    }

    size_t start = 0;
    size_t end = fsize - 1;
    bool is_range = false;

    if (httpd_req_get_hdr_value_str(req, "Range", sc->range_hdr, sizeof(sc->range_hdr)) == ESP_OK) {
        if (sscanf(sc->range_hdr, "bytes=%zu-%zu", &start, &end) == 2) {
            is_range = true;
        } else if (sscanf(sc->range_hdr, "bytes=%zu-", &start) == 1) {
            is_range = true;
            end = fsize - 1;
        }
    }

    if (is_range && (start > end || start >= fsize)) {
        free(sc);
        httpd_resp_set_status(req, "416 Range Not Satisfiable");
        httpd_resp_send(req, "Range invalido", HTTPD_RESP_USE_STRLEN);
        fclose(fp);
        return ESP_FAIL;
    }
    if (end >= fsize) end = fsize - 1;

    size_t content_length = end - start + 1;

    if (is_range) {
        httpd_resp_set_status(req, "206 Partial Content");
        snprintf(sc->crange_str, sizeof(sc->crange_str), "bytes %zu-%zu/%zu", start, end, fsize);
        httpd_resp_set_hdr(req, "Content-Range", sc->crange_str);
    }
    httpd_resp_set_hdr(req, "Connection", "close");

    // Timeout curto de envio (8s) no socket de download para nunca travar a thread httpd se o navegador pausar a musica
    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd >= 0) {
        struct timeval tv_dl = { .tv_sec = 8, .tv_usec = 0 };
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv_dl, sizeof(tv_dl));
    }

    fseek(fp, start, SEEK_SET);

    size_t chunk_sz = 32768;
    char *chunk = (char *)heap_caps_malloc(chunk_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chunk) {
        chunk_sz = 4096;
        chunk = (char *)malloc(chunk_sz);
    }
    if (!chunk) {
        free(sc);
        fclose(fp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    size_t remaining = content_length;
    while (remaining > 0) {
        if (s_progress_active) {
            ESP_LOGW(TAG, "Download interrompido: inicio de upload no SD");
            break;
        }
        size_t to_read = (remaining < chunk_sz) ? remaining : chunk_sz;
        size_t read_bytes = fread(chunk, 1, to_read, fp);
        if (read_bytes <= 0) break;
        
        if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
            free(chunk);
            free(sc);
            fclose(fp);
            return ESP_FAIL;
        }
        remaining -= read_bytes;
    }

    if (remaining > 0) {
        free(chunk);
        free(sc);
        fclose(fp);
        return ESP_FAIL;
    }

    httpd_resp_send_chunk(req, NULL, 0); 
    free(chunk);
    free(sc);
    fclose(fp);
    return ESP_OK;
}

static void get_json_string(const char *json, const char *key, char *out, size_t max_len);

static esp_err_t api_delete_handler(httpd_req_t *req)
{
    if (!check_api_key(req)) return ESP_FAIL;
    podcast_sync_notify_activity();

    typedef struct {
        char body[512];
        char query[512];
        char path_enc[400];
        char rel_path[400];
        char abs_path[600];
    } del_scratch_t;

    del_scratch_t *sc = (del_scratch_t *)http_scratch_alloc(sizeof(del_scratch_t));
    if (!sc) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (httpd_req_get_url_query_str(req, sc->query, sizeof(sc->query)) == ESP_OK &&
        httpd_query_key_value(sc->query, "path", sc->path_enc, sizeof(sc->path_enc)) == ESP_OK) {
        url_decode(sc->rel_path, sc->path_enc, sizeof(sc->rel_path));
    } else if (req->content_len > 0) {
        int to_read = (req->content_len < sizeof(sc->body) - 1) ? (int)req->content_len : (int)(sizeof(sc->body) - 1);
        int ret = httpd_req_recv(req, sc->body, to_read);
        if (ret > 0) {
            sc->body[ret] = '\0';
            get_json_string(sc->body, "path", sc->rel_path, sizeof(sc->rel_path));
        }
    }

    if (sc->rel_path[0] == '\0') {
        free(sc);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"missing_path\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    if (!build_abs_path(sc->rel_path, sc->abs_path, sizeof(sc->abs_path)) || sc->rel_path[0] == '\0') {
        free(sc);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"invalid_path\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    if (recursive_delete(sc->abs_path) != ESP_OK) {
        free(sc);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"delete_failed\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    s_status_last_calc_ms = 0;
    ESP_LOGI(TAG, "Apagado: %s", sc->rel_path);
    free(sc);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Extrai um campo string simples de um corpo JSON raso ({"campo":"valor"}).
// Reaproveitado pelos handlers de rede abaixo - mesma abordagem manual ja'
// usada no resto do arquivo (sem puxar dependencia de cJSON so' pra isso).
static void json_extract_field(const char *buf, const char *field, char *out, size_t out_len)
{
    out[0] = '\0';
    char needle[24];
    snprintf(needle, sizeof(needle), "\"%s\"", field);
    char *p = strstr(buf, needle);
    if (!p) return;
    p = strchr(p, ':');
    if (!p) return;
    p = strchr(p, '"');
    if (!p) return;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < out_len - 1) out[i++] = *p++;
    out[i] = '\0';
}

// --- Lista de redes conhecidas ---------------------------------------------
// Guardada no NVS (namespace "wifi_cfg") como "net_count" + pares
// "netN_ssid"/"netN_pass". Ordem = ordem de preferencia (indice 0 e' a
// ultima rede que funcionou - ver known_networks_put_front()), entao ao
// entrar no modo STA a gente tenta a mais recentemente usada primeiro.
#define WIFI_KNOWN_MAX   8
#define WIFI_SSID_MAX_LEN 33
#define WIFI_PASS_MAX_LEN 65
// Buffer bem folgado pras chaves "netN_ssid"/"netN_pass" do NVS. N nunca
// passa de WIFI_KNOWN_MAX-1 (um digito), mas o GCC nao sabe disso ao
// analisar o snprintf("net%d_ssid", i) - pra' provar formalmente que nao
// ha' truncamento (-Werror=format-truncation) ele assume o pior caso de
// int (ate' 11 digitos+sinal), entao o buffer precisa sobrar margem pra'
// isso mesmo o conteudo real sendo sempre bem menor.
#define WIFI_NVS_KEY_LEN 24

typedef struct {
    char ssid[WIFI_SSID_MAX_LEN];
    char pass[WIFI_PASS_MAX_LEN];
} wifi_known_net_t;

static wifi_known_net_t s_known[WIFI_KNOWN_MAX];
static int s_known_count = 0;
static bool s_known_loaded = false;

static esp_err_t known_networks_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("wifi_cfg", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_u8(h, "net_count", (uint8_t)s_known_count);
    for (int i = 0; i < s_known_count; i++) {
        char key[WIFI_NVS_KEY_LEN];
        snprintf(key, sizeof(key), "net%d_ssid", i);
        nvs_set_str(h, key, s_known[i].ssid);
        snprintf(key, sizeof(key), "net%d_pass", i);
        nvs_set_str(h, key, s_known[i].pass);
    }
    // Limpa entradas que sobraram do NVS caso a lista tenha encolhido
    // (ex: depois de uma remocao).
    for (int i = s_known_count; i < WIFI_KNOWN_MAX; i++) {
        char key[WIFI_NVS_KEY_LEN];
        snprintf(key, sizeof(key), "net%d_ssid", i);
        nvs_erase_key(h, key);
        snprintf(key, sizeof(key), "net%d_pass", i);
        nvs_erase_key(h, key);
    }

    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void known_networks_load(void)
{
    if (s_known_loaded) return;
    s_known_loaded = true;
    s_known_count = 0;

    nvs_handle_t h;
    if (nvs_open("wifi_cfg", NVS_READONLY, &h) == ESP_OK) {
        uint8_t count = 0;
        if (nvs_get_u8(h, "net_count", &count) == ESP_OK) {
            if (count > WIFI_KNOWN_MAX) count = WIFI_KNOWN_MAX;
            for (int i = 0; i < count; i++) {
                char key[WIFI_NVS_KEY_LEN];
                size_t len;

                snprintf(key, sizeof(key), "net%d_ssid", i);
                len = sizeof(s_known[s_known_count].ssid);
                if (nvs_get_str(h, key, s_known[s_known_count].ssid, &len) != ESP_OK) continue;

                snprintf(key, sizeof(key), "net%d_pass", i);
                len = sizeof(s_known[s_known_count].pass);
                if (nvs_get_str(h, key, s_known[s_known_count].pass, &len) != ESP_OK) {
                    s_known[s_known_count].pass[0] = '\0';
                }
                s_known_count++;
            }
        }
        nvs_close(h);
    }

    // Migracao: versoes anteriores guardavam so' uma rede, nas chaves
    // "ssid"/"pass". Se a lista nova ainda estiver vazia e essas chaves
    // antigas existirem, importa como a primeira rede conhecida.
    if (s_known_count == 0 &&
        nvs_open("wifi_cfg", NVS_READONLY, &h) == ESP_OK) {
        char ssid[WIFI_SSID_MAX_LEN] = {0};
        size_t len = sizeof(ssid);
        if (nvs_get_str(h, "ssid", ssid, &len) == ESP_OK && ssid[0] != '\0') {
            char pass[WIFI_PASS_MAX_LEN] = {0};
            len = sizeof(pass);
            nvs_get_str(h, "pass", pass, &len);
            strlcpy(s_known[0].ssid, ssid, sizeof(s_known[0].ssid));
            strlcpy(s_known[0].pass, pass, sizeof(s_known[0].pass));
            s_known_count = 1;
        }
        nvs_close(h);
        if (s_known_count == 1) {
            known_networks_save();
            ESP_LOGI(TAG, "Migrada credencial WiFi unica antiga para a lista de redes conhecidas");
        }
    }
}

int wifi_transfer_get_known_count(void)
{
    known_networks_load();
    return s_known_count;
}

bool wifi_transfer_get_known_network(int idx, char *out_ssid, size_t ssid_len, char *out_pass, size_t pass_len)
{
    known_networks_load();
    if (idx < 0 || idx >= s_known_count) return false;
    if (out_ssid && ssid_len > 0) {
        strncpy(out_ssid, s_known[idx].ssid, ssid_len - 1);
        out_ssid[ssid_len - 1] = '\0';
    }
    if (out_pass && pass_len > 0) {
        strncpy(out_pass, s_known[idx].pass, pass_len - 1);
        out_pass[pass_len - 1] = '\0';
    }
    return true;
}

void wifi_transfer_get_ap_credentials(char *out_ssid, size_t ssid_len, char *out_pass, size_t pass_len)
{
    if (out_ssid && ssid_len > 0) {
        strncpy(out_ssid, CONFIG_WIFI_AP_SSID, ssid_len - 1);
        out_ssid[ssid_len - 1] = '\0';
    }
    if (out_pass && pass_len > 0) {
        strncpy(out_pass, CONFIG_WIFI_AP_PASSWORD, pass_len - 1);
        out_pass[pass_len - 1] = '\0';
    }
}

// Insere/atualiza uma rede no topo da lista (mais recente = maior
// prioridade). Se a lista ja' estiver cheia e a rede for nova, descarta a
// mais antiga (ultima posicao) pra' abrir espaco.
static void known_networks_put_front(const char *ssid, const char *pass)
{
    known_networks_load();

    int existing = -1;
    for (int i = 0; i < s_known_count; i++) {
        if (strcmp(s_known[i].ssid, ssid) == 0) { existing = i; break; }
    }

    wifi_known_net_t entry;
    strncpy(entry.ssid, ssid, sizeof(entry.ssid) - 1);
    entry.ssid[sizeof(entry.ssid) - 1] = '\0';
    strncpy(entry.pass, pass, sizeof(entry.pass) - 1);
    entry.pass[sizeof(entry.pass) - 1] = '\0';

    int shift_from;
    if (existing >= 0) {
        shift_from = existing;
    } else {
        shift_from = (s_known_count < WIFI_KNOWN_MAX) ? s_known_count : WIFI_KNOWN_MAX - 1;
        if (s_known_count < WIFI_KNOWN_MAX) s_known_count++;
    }
    for (int i = shift_from; i > 0; i--) s_known[i] = s_known[i - 1];
    s_known[0] = entry;

    known_networks_save();
}

void wifi_transfer_save_network(const char *ssid, const char *pass)
{
    known_networks_put_front(ssid, pass);
}

static bool known_networks_remove(const char *ssid)
{
    known_networks_load();
    for (int i = 0; i < s_known_count; i++) {
        if (strcmp(s_known[i].ssid, ssid) == 0) {
            for (int j = i; j < s_known_count - 1; j++) s_known[j] = s_known[j + 1];
            s_known_count--;
            known_networks_save();
            return true;
        }
    }
    return false;
}

// GET /api/wifi/networks -> lista de redes conhecidas (sem senha - a
// pagina web nunca precisa reler a senha, so' escrever uma nova).
static esp_err_t api_wifi_networks_get_handler(httpd_req_t *req)
{
    known_networks_load();

    size_t jsz = 64 + WIFI_KNOWN_MAX * (WIFI_SSID_MAX_LEN * 2 + 16);
    char *json = (char *)http_scratch_alloc(jsz);
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    char ssid_esc[WIFI_SSID_MAX_LEN * 2];
    int off = snprintf(json, jsz, "{\"networks\":[");
    for (int i = 0; i < s_known_count && off < (int)jsz - 1; i++) {
        json_escape(s_known[i].ssid, ssid_esc, sizeof(ssid_esc));
        off += snprintf(json + off, jsz - off, "%s{\"ssid\":\"%s\"}",
                         i > 0 ? "," : "", ssid_esc);
    }
    snprintf(json + off, jsz - off, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

// POST /api/wifi/networks -> {"ssid":"...","pass":"..."} adiciona (ou
// atualiza a senha de) uma rede conhecida, colocando-a no topo da lista.
static esp_err_t api_wifi_networks_post_handler(httpd_req_t *req)
{
    char buf[160];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "corpo vazio");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    char ssid[WIFI_SSID_MAX_LEN] = {0};
    char pass[WIFI_PASS_MAX_LEN] = {0};
    json_extract_field(buf, "ssid", ssid, sizeof(ssid));
    json_extract_field(buf, "pass", pass, sizeof(pass));

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid vazio");
        return ESP_FAIL;
    }

    known_networks_put_front(ssid, pass);
    ESP_LOGI(TAG, "Rede WiFi salva na lista de conhecidas: SSID=%s (%d/%d)", ssid, s_known_count, WIFI_KNOWN_MAX);

    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// DELETE /api/wifi/networks -> {"ssid":"..."} remove uma rede da lista.
static esp_err_t api_wifi_networks_delete_handler(httpd_req_t *req)
{
    char buf[64];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "corpo vazio");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    char ssid[WIFI_SSID_MAX_LEN] = {0};
    json_extract_field(buf, "ssid", ssid, sizeof(ssid));
    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid vazio");
        return ESP_FAIL;
    }

    if (!known_networks_remove(ssid)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "rede nao encontrada");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Rede WiFi removida da lista de conhecidas: SSID=%s", ssid);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// POST /api/webradio -> {"url":"..."}
#include "audio_player.h"

// POST /api/control/play -> {"path":"..."}
static esp_err_t api_control_play_handler(httpd_req_t *req)
{
    typedef struct {
        char buf[512];
        char rel_path[256];
        char abs_path[400];
    } play_scratch_t;

    play_scratch_t *sc = (play_scratch_t *)http_scratch_alloc(sizeof(play_scratch_t));
    if (!sc) return ESP_FAIL;

    int received = httpd_req_recv(req, sc->buf, sizeof(sc->buf) - 1);
    if (received <= 0) { free(sc); return ESP_FAIL; }
    sc->buf[received] = '\0';

    json_extract_field(sc->buf, "path", sc->rel_path, sizeof(sc->rel_path));
    build_abs_path(sc->rel_path, sc->abs_path, sizeof(sc->abs_path));
    
    ESP_LOGI(TAG, "DJ Play: %s", sc->abs_path);
    free(sc);
    
    httpd_resp_send(req, "{\"status\":\"ok\"}", -1);
    return ESP_OK;
}

static esp_err_t api_webradio_play_handler(httpd_req_t *req)
{
    typedef struct {
        char buf[512];
        char url[256];
    } radio_scratch_t;

    radio_scratch_t *sc = (radio_scratch_t *)http_scratch_alloc(sizeof(radio_scratch_t));
    if (!sc) return ESP_FAIL;

    int received = httpd_req_recv(req, sc->buf, sizeof(sc->buf) - 1);
    if (received <= 0) {
        free(sc);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "corpo vazio");
        return ESP_FAIL;
    }
    sc->buf[received] = '\0';

    json_extract_field(sc->buf, "url", sc->url, sizeof(sc->url));
    if (sc->url[0] == '\0') {
        free(sc);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "url vazia");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Web Radio play solicitada: %s", sc->url);
    audio_player_play_url(sc->url);
    free(sc);
    
    httpd_resp_send(req, "{\"status\":\"ok\"}", -1);
    return ESP_OK;
}

// POST /api/webradio/stop
static esp_err_t api_webradio_stop_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Web Radio stop solicitada");
    audio_player_stop_url();
    httpd_resp_send(req, "{\"status\":\"ok\"}", -1);
    return ESP_OK;
}


// --- EQ Handlers ---
static esp_err_t api_eq_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    
    typedef struct {
        player_eq_config_t cfg;
        char buf[4096];
    } eq_get_scratch_t;

    eq_get_scratch_t *sc = (eq_get_scratch_t *)http_scratch_alloc(sizeof(eq_get_scratch_t));
    if (!sc) return ESP_FAIL;

    audio_player_get_eq_config(&sc->cfg);
    
    int offset = snprintf(sc->buf, sizeof(sc->buf), "{\"enabled\":%s,\"active_preset_idx\":%d,\"presets\":[",
                          sc->cfg.enabled ? "true" : "false", sc->cfg.active_preset_idx);
    
    for (int p = 0; p < PLAYER_EQ_MAX_PRESETS; p++) {
        offset += snprintf(sc->buf + offset, sizeof(sc->buf) - offset, "{\"name\":\"%s\",\"overall_gain\":%.1f,\"band_gains\":[",
                           sc->cfg.presets[p].name, sc->cfg.presets[p].overall_gain);
        for (int b = 0; b < PLAYER_EQ_BANDS; b++) {
            offset += snprintf(sc->buf + offset, sizeof(sc->buf) - offset, "%.1f%s",
                               sc->cfg.presets[p].band_gains[b], (b == PLAYER_EQ_BANDS - 1) ? "" : ",");
        }
        offset += snprintf(sc->buf + offset, sizeof(sc->buf) - offset, "]}%s", (p == PLAYER_EQ_MAX_PRESETS - 1) ? "" : ",");
    }
    snprintf(sc->buf + offset, sizeof(sc->buf) - offset, "]}");
    
    httpd_resp_send(req, sc->buf, HTTPD_RESP_USE_STRLEN);
    free(sc);
    return ESP_OK;
}

static void get_json_string(const char *json, const char *key, char *out, size_t max_len) {
    out[0] = '\0';
    if (!json || !key) return;
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return;
    p += strlen(search);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p != '"') return;
    p++;
    const char *end = strchr(p, '"');
    if (!end) return;
    size_t len = end - p;
    if (len >= max_len) len = max_len - 1;
    strncpy(out, p, len);
    out[len] = '\0';
}

static esp_err_t api_eq_post_handler(httpd_req_t *req) {
    typedef struct {
        player_eq_config_t cfg;
        char buf[4096];
    } eq_post_scratch_t;

    eq_post_scratch_t *sc = (eq_post_scratch_t *)http_scratch_alloc(sizeof(eq_post_scratch_t));
    if (!sc) return ESP_FAIL;

    int ret = httpd_req_recv(req, sc->buf, req->content_len < 4095 ? req->content_len : 4095);
    if (ret <= 0) { free(sc); return ESP_FAIL; }
    sc->buf[ret] = '\0';
    
    audio_player_get_eq_config(&sc->cfg);
    
    const char *p = strstr(sc->buf, "\"enabled\"");
    if(p) {
        p += 9;
        while (*p && (*p == ' ' || *p == '\t' || *p == ':')) p++;
        sc->cfg.enabled = (*p == 't' || *p == 'T' || *p == '1');
    }
    
    p = strstr(sc->buf, "\"active_preset_idx\"");
    if(p) {
        p += 19;
        while (*p && (*p == ' ' || *p == '\t' || *p == ':')) p++;
        sc->cfg.active_preset_idx = atoi(p);
    }
    
    p = strstr(sc->buf, "\"presets\"");
    if(p) {
        for(int i=0; i<10; i++) {
            p = strstr(p, "\"name\"");
            if(!p) break;
            p += 6;
            while (*p && (*p == ' ' || *p == '\t' || *p == ':')) p++;
            if (*p == '"') {
                p++;
                const char *end_name = strchr(p, '"');
                if(end_name) {
                    size_t l = end_name - p;
                    if(l >= 16) l = 15;
                    strncpy(sc->cfg.presets[i].name, p, l);
                    sc->cfg.presets[i].name[l] = '\0';
                }
            }
            p = strstr(p, "\"overall_gain\"");
            if(p) {
                p += 14;
                while (*p && (*p == ' ' || *p == '\t' || *p == ':')) p++;
                sc->cfg.presets[i].overall_gain = atof(p);
            }
            
            p = strstr(p, "\"band_gains\"");
            if(p) {
                p += 12;
                while (*p && (*p != '[')) p++;
                if (*p == '[') {
                    p++;
                    for(int b=0; b<10; b++) {
                        while (*p && (*p == ' ' || *p == '\t')) p++;
                        sc->cfg.presets[i].band_gains[b] = atof(p);
                        p = strchr(p, ',');
                        if(!p) break;
                        p++;
                    }
                }
            }
        }
    }
    
    audio_player_set_eq_config(&sc->cfg);
    free(sc);
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_rename_post_handler(httpd_req_t *req) {
    if (!check_api_key(req)) return ESP_FAIL;
    podcast_sync_notify_activity();

    typedef struct {
        char buf[1024];
        char old_path[400];
        char new_path[400];
        char old_abs[600];
        char new_abs[600];
    } rename_scratch_t;

    rename_scratch_t *sc = (rename_scratch_t *)http_scratch_alloc(sizeof(rename_scratch_t));
    if (!sc) return ESP_FAIL;

    int ret = httpd_req_recv(req, sc->buf, req->content_len < sizeof(sc->buf) - 1 ? req->content_len : sizeof(sc->buf) - 1);
    if (ret <= 0) { free(sc); return ESP_FAIL; }
    sc->buf[ret] = '\0';
    
    get_json_string(sc->buf, "old_path", sc->old_path, sizeof(sc->old_path));
    get_json_string(sc->buf, "new_path", sc->new_path, sizeof(sc->new_path));
    
    if (!build_abs_path(sc->old_path, sc->old_abs, sizeof(sc->old_abs)) ||
        !build_abs_path(sc->new_path, sc->new_abs, sizeof(sc->new_abs))) {
        free(sc);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"error\":\"caminho invalido\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    size_t old_len = strlen(sc->old_abs);
    if (strncmp(sc->old_abs, sc->new_abs, old_len) == 0 &&
        (sc->new_abs[old_len] == '/' || sc->new_abs[old_len] == '\0')) {
        free(sc);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"error\":\"destino nao pode ser o proprio item ou uma subpasta dele\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    mkdir_p_for_file(sc->new_abs);
    if (rename(sc->old_abs, sc->new_abs) != 0) {
        free(sc);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "{\"error\":\"falha ao renomear\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    s_status_last_calc_ms = 0;
    free(sc);
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_move_post_handler(httpd_req_t *req) {
    return api_rename_post_handler(req);
}

static esp_err_t api_copy_post_handler(httpd_req_t *req) {
    typedef struct {
        char buf[1024];
        char src_path[400];
        char dest_path[400];
        char src_abs[600];
        char dest_abs[600];
        char cpy_buf[8192];
    } copy_scratch_t;

    copy_scratch_t *sc = (copy_scratch_t *)http_scratch_alloc(sizeof(copy_scratch_t));
    if (!sc) return ESP_FAIL;

    int ret = httpd_req_recv(req, sc->buf, req->content_len < sizeof(sc->buf) - 1 ? req->content_len : sizeof(sc->buf) - 1);
    if (ret <= 0) { free(sc); return ESP_FAIL; }
    sc->buf[ret] = '\0';
    
    get_json_string(sc->buf, "src_path", sc->src_path, sizeof(sc->src_path));
    get_json_string(sc->buf, "dest_path", sc->dest_path, sizeof(sc->dest_path));
    
    if (!build_abs_path(sc->src_path, sc->src_abs, sizeof(sc->src_abs)) ||
        !build_abs_path(sc->dest_path, sc->dest_abs, sizeof(sc->dest_abs))) {
        free(sc);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"error\":\"caminho invalido\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    mkdir_p_for_file(sc->dest_abs);
    FILE *fs = fopen(sc->src_abs, "rb");
    if (!fs) {
        free(sc);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "{\"error\":\"origem nao encontrada\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    FILE *fd = fopen(sc->dest_abs, "wb");
    if (!fd) {
        fclose(fs);
        free(sc);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "{\"error\":\"falha ao criar destino\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    size_t r;
    while ((r = fread(sc->cpy_buf, 1, sizeof(sc->cpy_buf), fs)) > 0) {
        if (fwrite(sc->cpy_buf, 1, r, fd) != r) break;
    }
    fclose(fd);
    fclose(fs);
    free(sc);
    s_status_last_calc_ms = 0;

    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_mkdir_post_handler(httpd_req_t *req) {
    if (!check_api_key(req)) return ESP_FAIL;
    podcast_sync_notify_activity();

    typedef struct {
        char buf[1024];
        char dir_path[400];
        char abs_path[600];
        char abs_file[620];
        char err_msg[128];
    } mkdir_scratch_t;

    mkdir_scratch_t *sc = (mkdir_scratch_t *)http_scratch_alloc(sizeof(mkdir_scratch_t));
    if (!sc) return ESP_FAIL;

    int ret = httpd_req_recv(req, sc->buf, req->content_len < sizeof(sc->buf) - 1 ? req->content_len : sizeof(sc->buf) - 1);
    if (ret <= 0) { free(sc); return ESP_FAIL; }
    sc->buf[ret] = '\0';
    
    get_json_string(sc->buf, "path", sc->dir_path, sizeof(sc->dir_path));
    if (sc->dir_path[0] == '\0') {
        free(sc);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"error\":\"path vazio\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    
    if (!build_abs_path(sc->dir_path, sc->abs_path, sizeof(sc->abs_path))) {
        free(sc);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"error\":\"caminho invalido\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    snprintf(sc->abs_file, sizeof(sc->abs_file), "%s/", sc->abs_path);
    mkdir_p_for_file(sc->abs_file);

    struct stat st;
    if (stat(sc->abs_path, &st) != 0) {
        if (mkdir(sc->abs_path, 0777) != 0 && errno != EEXIST) {
            snprintf(sc->err_msg, sizeof(sc->err_msg), "{\"error\":\"falha ao criar pasta (%s)\"}", strerror(errno));
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_send(req, sc->err_msg, HTTPD_RESP_USE_STRLEN);
            free(sc);
            return ESP_FAIL;
        }
    }

    free(sc);
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// --- OTA Handlers -----------------------------------------------------------

static void ota_restart_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Reiniciando MPS3 agora...");
    esp_restart();
}

static esp_err_t api_ota_get_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    int bat_pct = battery_get_percent();
    bool charging = battery_is_charging();

    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) {
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    char *json = (char *)http_scratch_alloc(384);
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    snprintf(json, 384,
             "{\"running_version\":\"%s\",\"running_partition\":\"%s\","
             "\"next_partition\":\"%s\",\"battery_percent\":%d,"
             "\"battery_charging\":%s,\"ota_in_progress\":%s,\"mac\":\"%s\"}",
             app_desc ? app_desc->version : "unknown",
             running ? running->label : "unknown",
             next ? next->label : "unknown",
             bat_pct,
             charging ? "true" : "false",
             s_ota_in_progress ? "true" : "false",
             mac_str);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

#define OTA_BUFF_SIZE 4096

static esp_err_t api_ota_post_handler(httpd_req_t *req)
{
    // Bloquear concorrencia se ja ocupado
    if (s_ota_in_progress) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Atualizacao OTA ja esta em andamento\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    if (s_ui_state == WIFI_UI_TRANSFERRING) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Transferencia de arquivos em andamento. Aguarde terminar.\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    // Checar bateria: se < 15% e sem carregador conectado, recusar
    int bat_pct = battery_get_percent();
    bool charging = battery_is_charging();
    if (bat_pct < 15 && !charging) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Bateria baixa (<15%). Conecte o carregador USB para atualizar.\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    const int min_hdr = (int)(sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t));
    int total_len = req->content_len;
    if (total_len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Content-Length ausente ou zerado\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    if (total_len < min_hdr) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Arquivo de firmware muito pequeno ou corrompido\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    // Obter particao alvo
    const esp_partition_t *target_part = esp_ota_get_next_update_partition(NULL);
    if (!target_part) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Nenhuma particao OTA disponivel na flash\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    if ((size_t)total_len > target_part->size) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Arquivo .bin maior que a particao flash de destino\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    // Parar audio se estiver tocando para liberar barramento SPI e evitar conflitos
    playback_state_t st;
    audio_player_get_state(&st);
    if (st.playing && !st.paused) {
        audio_player_toggle_play_pause();
    }
    audio_player_stop_url();

    // Trava estado OTA imediatamente para bloquear requisicoes concorrentes e saida acidental
    s_ota_in_progress = true;
    s_ota_pct = 0;
    snprintf(s_ota_status, sizeof(s_ota_status), "Validando cabecalho...");
    s_ui_state = WIFI_UI_OTA_UPDATING;

    // Reutiliza o buffer DMA persistente s_bounce_persist (8 KB na SRAM interna) se disponivel,
    // eliminando qualquer alocacao adicional na DRAM interna que causaria escassez e 'wifi:mem fail'
    char *ota_write_data = NULL;
    bool ota_buf_allocated = false;
    if (s_bounce_persist && s_bounce_persist_sz >= OTA_BUFF_SIZE) {
        ota_write_data = (char *)s_bounce_persist;
    } else {
        ota_write_data = (char *)heap_caps_malloc(OTA_BUFF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
        if (!ota_write_data) {
            ota_write_data = (char *)malloc(OTA_BUFF_SIZE);
        }
        ota_buf_allocated = true;
    }

    if (!ota_write_data) {
        s_ota_in_progress = false;
        s_ui_state = WIFI_UI_CONNECTED;
        s_ota_status[0] = '\0';
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Memoria insuficiente para alocar buffer OTA\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    // Receber primeiro chunk de pelo menos min_hdr (288 bytes) para validar cabecalhos antes de gravar na flash
    int first_chunk_len = 0;
    int first_chunk_timeouts = 0;
    while (first_chunk_len < min_hdr && first_chunk_len < total_len) {
        int r = httpd_req_recv(req, ota_write_data + first_chunk_len, OTA_BUFF_SIZE - first_chunk_len);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                if (++first_chunk_timeouts > 10) {
                    ESP_LOGE(TAG, "Timeout excessivo aguardando cabecalho OTA");
                    break;
                }
                continue;
            }
            break;
        }
        first_chunk_timeouts = 0;
        first_chunk_len += r;
    }

    if (first_chunk_len < min_hdr) {
        if (ota_buf_allocated) free(ota_write_data);
        s_ota_in_progress = false;
        s_ui_state = WIFI_UI_CONNECTED;
        s_ota_status[0] = '\0';
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Falha de rede ao receber cabecalho do firmware\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    // Validar cabecalho de imagem ESP32: byte 0 == ESP_IMAGE_HEADER_MAGIC (0xE9)
    esp_image_header_t *img_hdr = (esp_image_header_t *)ota_write_data;
    if (img_hdr->magic != ESP_IMAGE_HEADER_MAGIC) {
        ESP_LOGE(TAG, "Magic header invalido: 0x%02X (esperado 0x%02X)", img_hdr->magic, ESP_IMAGE_HEADER_MAGIC);
        if (ota_buf_allocated) free(ota_write_data);
        s_ota_in_progress = false;
        s_ui_state = WIFI_UI_CONNECTED;
        s_ota_status[0] = '\0';
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Arquivo invalido: cabecalho de imagem ESP32 ausente\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    // Validar esp_app_desc_t no offset 0x20
    esp_app_desc_t app_desc;
    memcpy(&app_desc, ota_write_data + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(esp_app_desc_t));
    if (app_desc.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        strncmp(app_desc.project_name, "mps3", sizeof(app_desc.project_name)) != 0) {
        app_desc.project_name[sizeof(app_desc.project_name) - 1] = '\0';
        ESP_LOGE(TAG, "esp_app_desc invalido: magic=0x%08X (esp: 0x%08X), project='%s' (esp: 'mps3')",
                 (unsigned int)app_desc.magic_word, (unsigned int)ESP_APP_DESC_MAGIC_WORD, app_desc.project_name);
        if (ota_buf_allocated) free(ota_write_data);
        s_ota_in_progress = false;
        s_ui_state = WIFI_UI_CONNECTED;
        s_ota_status[0] = '\0';
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Arquivo .bin nao pertence ao projeto mps3 ou esta corrompido!\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    app_desc.project_name[sizeof(app_desc.project_name) - 1] = '\0';
    app_desc.version[sizeof(app_desc.version) - 1] = '\0';
    ESP_LOGI(TAG, "Firmware validado! Projeto: '%s', Versao: '%s'. Gravando em %s...",
             app_desc.project_name, app_desc.version, target_part->label);

    snprintf(s_ota_status, sizeof(s_ota_status), "Gravando flash...");

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(target_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin falhou: %s", esp_err_to_name(err));
        s_ota_in_progress = false;
        s_ui_state = WIFI_UI_CONNECTED;
        s_ota_status[0] = '\0';
        if (ota_buf_allocated) free(ota_write_data);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Falha ao inicializar particao flash OTA\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    int bytes_written = 0;
    int chunk_accum = first_chunk_len;
    int block_cnt = 0;
    bool write_failed = false;
    int consecutive_timeouts = 0;

    // Loop de recepcao e gravacao com acumulacao de blocos alinhados a 4 KB
    while (bytes_written + chunk_accum < total_len) {
        // Completa o buffer de 4 KB
        while (chunk_accum < OTA_BUFF_SIZE && (bytes_written + chunk_accum < total_len)) {
            int to_read = (total_len - (bytes_written + chunk_accum) < (OTA_BUFF_SIZE - chunk_accum))
                          ? (int)(total_len - (bytes_written + chunk_accum))
                          : (int)(OTA_BUFF_SIZE - chunk_accum);
            int r = httpd_req_recv(req, ota_write_data + chunk_accum, to_read);
            if (r < 0) {
                if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                    if (++consecutive_timeouts > 20) {
                        ESP_LOGE(TAG, "OTA abortado: conexao inativa por muito tempo (timeouts)");
                        write_failed = true;
                        break;
                    }
                    continue;
                }
                ESP_LOGE(TAG, "httpd_req_recv falhou: %d", r);
                write_failed = true;
                break;
            }
            if (r == 0) {
                write_failed = true;
                break;
            }
            consecutive_timeouts = 0;
            chunk_accum += r;
        }

        if (write_failed) break;

        // Grava o bloco na particao OTA
        if (chunk_accum > 0) {
            err = esp_ota_write(ota_handle, (const void *)ota_write_data, chunk_accum);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write falhou (%d bytes): %s", chunk_accum, esp_err_to_name(err));
                write_failed = true;
                break;
            }
            bytes_written += chunk_accum;
            chunk_accum = 0;
            s_ota_pct = (int)((bytes_written * 100LL) / total_len);
            vTaskDelay(1);
        }
    }

    // Se sobrou algum dado final acumulado (ex: ultimo setor parcial)
    if (!write_failed && chunk_accum > 0) {
        err = esp_ota_write(ota_handle, (const void *)ota_write_data, chunk_accum);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write falhou no bloco final (%d bytes): %s", chunk_accum, esp_err_to_name(err));
            write_failed = true;
        } else {
            bytes_written += chunk_accum;
            chunk_accum = 0;
            s_ota_pct = (int)((bytes_written * 100LL) / total_len);
        }
    }

    if (ota_buf_allocated) {
        free(ota_write_data);
    }

    if (write_failed || bytes_written != total_len) {
        ESP_LOGE(TAG, "OTA abortado: gravados %d de %d bytes", bytes_written, total_len);
        esp_ota_abort(ota_handle);
        s_ota_in_progress = false;
        s_ui_state = WIFI_UI_CONNECTED;
        s_ota_status[0] = '\0';
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Falha de conexao ou escrita durante gravacao da flash\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    err = esp_ota_end(ota_handle);
    bool ota_valid = (err == ESP_OK);
    if (!ota_valid) {
        ESP_LOGW(TAG, "esp_ota_end retornou %s. Validando SHA-256 diretamente da particao %s...",
                 esp_err_to_name(err), target_part->label);

        if (total_len > 32) {
            size_t sha_len = total_len - 32;
            mps3_sha256_ctx_t sha_ctx;
            mps3_sha256_init(&sha_ctx);

            uint8_t check_buf[1024];
            size_t rd_offset = 0;
            bool read_err = false;
            while (rd_offset < sha_len) {
                size_t to_rd = (sha_len - rd_offset < sizeof(check_buf)) ? (sha_len - rd_offset) : sizeof(check_buf);
                if (esp_partition_read(target_part, rd_offset, check_buf, to_rd) != ESP_OK) {
                    read_err = true;
                    break;
                }
                mps3_sha256_update(&sha_ctx, check_buf, to_rd);
                rd_offset += to_rd;
            }

            if (!read_err) {
                uint8_t calc_hash[32];
                mps3_sha256_final(&sha_ctx, calc_hash);

                uint8_t expected_hash[32];
                if (esp_partition_read(target_part, sha_len, expected_hash, 32) == ESP_OK) {
                    if (memcmp(calc_hash, expected_hash, 32) == 0) {
                        ESP_LOGI(TAG, "SHA-256 verificado com SUCESSO na particao %s! Firmware 100%% integro.", target_part->label);
                        ota_valid = true;
                    } else {
                        ESP_LOGE(TAG, "Hash SHA-256 da flash diverge do hash esperado da imagem!");
                    }
                }
            }
        }
    }

    if (!ota_valid) {
        s_ota_in_progress = false;
        s_ui_state = WIFI_UI_CONNECTED;
        s_ota_status[0] = '\0';
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Validacao da imagem OTA falhou apos gravacao\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(target_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition falhou: %s", esp_err_to_name(err));
        s_ota_in_progress = false;
        s_ui_state = WIFI_UI_CONNECTED;
        s_ota_status[0] = '\0';
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Falha ao definir nova particao de boot\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    s_ota_pct = 100;
    snprintf(s_ota_status, sizeof(s_ota_status), "Reiniciando...");
    s_ui_state = WIFI_UI_OTA_FINISHED;
    ESP_LOGI(TAG, "OTA concluido com sucesso na particao %s! Reiniciando em 1.5s...", target_part->label);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"Firmware gravado com sucesso! Reiniciando o MPS3...\"}", HTTPD_RESP_USE_STRLEN);

    // Salvar indicador de reboot pós-OTA na NVS para reentrar no modo Wi-Fi e confirmar atualização
    nvs_handle_t nvs_h;
    if (nvs_open("system", NVS_READWRITE, &nvs_h) == ESP_OK) {
        nvs_set_u8(nvs_h, "ota_reboot", 1);
        nvs_commit(nvs_h);
        nvs_close(nvs_h);
    }

    const esp_timer_create_args_t restart_timer_args = {
        .callback = &ota_restart_timer_cb,
        .name = "ota_restart"
    };
    esp_timer_handle_t restart_timer;
    if (esp_timer_create(&restart_timer_args, &restart_timer) == ESP_OK) {
        esp_timer_start_once(restart_timer, 1500 * 1000); // 1500 ms
    } else {
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }

    return ESP_OK;
}

static esp_err_t api_podcasts_local_get_handler(httpd_req_t *req)
{
    typedef struct {
        char base_dir[128];
        char chunk[384];
        char prog_path[512];
        char file_path[768];
        char prog_esc[96];
        char name_esc[160];
    } pod_scratch_t;

    pod_scratch_t *sc = (pod_scratch_t *)http_scratch_alloc(sizeof(pod_scratch_t));
    if (!sc) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr_chunk(req, "{\"podcasts\":[");

    snprintf(sc->base_dir, sizeof(sc->base_dir), "%s/Podcasts", SD_MOUNT_POINT);

    DIR *d_base = opendir(sc->base_dir);
    if (!d_base) {
        free(sc);
        httpd_resp_sendstr_chunk(req, "]}");
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_OK;
    }

    struct dirent *prog_entry;
    bool first = true;

    while ((prog_entry = readdir(d_base)) != NULL) {
        if (prog_entry->d_name[0] == '.') continue;
        if (prog_entry->d_type != DT_DIR) continue;

        snprintf(sc->prog_path, sizeof(sc->prog_path), "%s/%s", sc->base_dir, prog_entry->d_name);
        DIR *d_prog = opendir(sc->prog_path);
        if (!d_prog) continue;

        struct dirent *ep_entry;
        while ((ep_entry = readdir(d_prog)) != NULL) {
            if (ep_entry->d_name[0] == '.') continue;
            if (ep_entry->d_type == DT_DIR) continue;
            if (strstr(ep_entry->d_name, ".part") != NULL) continue;

            snprintf(sc->file_path, sizeof(sc->file_path), "%s/%s", sc->prog_path, ep_entry->d_name);
            struct stat st;
            long size = 0;
            if (stat(sc->file_path, &st) == 0) {
                size = (long)st.st_size;
            }

            json_escape(prog_entry->d_name, sc->prog_esc, sizeof(sc->prog_esc));
            json_escape(ep_entry->d_name, sc->name_esc, sizeof(sc->name_esc));

            snprintf(sc->chunk, sizeof(sc->chunk), "%s{\"program\":\"%s\",\"filename\":\"%s\",\"size\":%ld}",
                     first ? "" : ",", sc->prog_esc, sc->name_esc, size);
            first = false;
            httpd_resp_sendstr_chunk(req, sc->chunk);
        }
        closedir(d_prog);
    }
    closedir(d_base);
    free(sc);

    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t api_sync_manifest_get_handler(httpd_req_t *req)
{
    if (!check_api_key(req)) return ESP_FAIL;
    podcast_sync_notify_activity();

    typedef struct {
        char base_dir[128];
        char prog_path[512];
        char file_path[768];
        char rel_path[640];
        char prog_esc[256];
        char name_esc[384];
        char rel_esc[768];
        char chunk[2048];
    } manifest_scratch_t;

    manifest_scratch_t *sc = (manifest_scratch_t *)http_scratch_alloc(sizeof(manifest_scratch_t));
    if (!sc) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) {
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }

    uint64_t total_bytes = 0, free_bytes = 0;
    esp_vfs_fat_info(SD_MOUNT_POINT, &total_bytes, &free_bytes);

    int bat_pct = battery_get_percent();
    bool charging = battery_is_charging();
    const esp_app_desc_t *app_desc = esp_app_get_description();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    snprintf(sc->chunk, sizeof(sc->chunk),
             "{\"device_mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
             "\"firmware_version\":\"%s\","
             "\"free_bytes\":%llu,\"total_bytes\":%llu,"
             "\"battery_pct\":%d,\"charging\":%s,\"podcasts\":[",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             app_desc ? app_desc->version : "unknown",
             (unsigned long long)free_bytes, (unsigned long long)total_bytes,
             bat_pct, charging ? "true" : "false");
    httpd_resp_sendstr_chunk(req, sc->chunk);

    snprintf(sc->base_dir, sizeof(sc->base_dir), "%s/Podcasts", SD_MOUNT_POINT);
    DIR *d_base = opendir(sc->base_dir);
    if (d_base) {
        struct dirent *prog_entry;
        bool first = true;

        while ((prog_entry = readdir(d_base)) != NULL) {
            if (prog_entry->d_name[0] == '.') continue;
            if (prog_entry->d_type != DT_DIR) continue;

            snprintf(sc->prog_path, sizeof(sc->prog_path), "%s/%s", sc->base_dir, prog_entry->d_name);
            DIR *d_prog = opendir(sc->prog_path);
            if (!d_prog) continue;

            struct dirent *ep_entry;
            while ((ep_entry = readdir(d_prog)) != NULL) {
                if (ep_entry->d_name[0] == '.') continue;
                if (ep_entry->d_type == DT_DIR) continue;
                if (strstr(ep_entry->d_name, ".part") != NULL) continue;

                snprintf(sc->file_path, sizeof(sc->file_path), "%s/%s", sc->prog_path, ep_entry->d_name);
                snprintf(sc->rel_path, sizeof(sc->rel_path), "Podcasts/%s/%s", prog_entry->d_name, ep_entry->d_name);

                struct stat st;
                long long size = 0;
                long long mtime = 0;
                if (stat(sc->file_path, &st) == 0) {
                    size = (long long)st.st_size;
                    mtime = (long long)st.st_mtime;
                }

                bool played = podcast_history_is_played(sc->rel_path);

                json_escape(prog_entry->d_name, sc->prog_esc, sizeof(sc->prog_esc));
                json_escape(ep_entry->d_name, sc->name_esc, sizeof(sc->name_esc));
                json_escape(sc->rel_path, sc->rel_esc, sizeof(sc->rel_esc));

                snprintf(sc->chunk, sizeof(sc->chunk),
                         "%s{\"program\":\"%s\",\"filename\":\"%s\",\"rel_path\":\"%s\","
                         "\"size\":%lld,\"mtime\":%lld,\"played\":%s}",
                         first ? "" : ",", sc->prog_esc, sc->name_esc, sc->rel_esc,
                         size, mtime, played ? "true" : "false");
                first = false;
                httpd_resp_sendstr_chunk(req, sc->chunk);
            }
            closedir(d_prog);
        }
        closedir(d_base);
    }

    httpd_resp_sendstr_chunk(req, "],\"played_history\":[");

    int hist_cnt = podcast_history_get_count();
    bool first_hist = true;
    for (int i = 0; i < hist_cnt; i++) {
        if (podcast_history_get_entry(i, sc->rel_path, sizeof(sc->rel_path), NULL, NULL)) {
            json_escape(sc->rel_path, sc->rel_esc, sizeof(sc->rel_esc));
            snprintf(sc->chunk, sizeof(sc->chunk), "%s\"%s\"", first_hist ? "" : ",", sc->rel_esc);
            first_hist = false;
            httpd_resp_sendstr_chunk(req, sc->chunk);
        }
    }

    free(sc);
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t api_sync_verify_post_handler(httpd_req_t *req)
{
    if (!check_api_key(req)) return ESP_FAIL;
    podcast_sync_notify_activity();

    typedef struct {
        char body[1024];
        char query[512];
        char path_enc[400];
        char rel_path[400];
        char abs_path[600];
        char rel_esc[450];
        char expected_sha256[68];
        char sha256_hex[65];
        char resp_json[640];
        mps3_sha256_ctx_t sha_ctx;
        uint8_t read_buf[32768];
    } verify_scratch_t;

    verify_scratch_t *sc = (verify_scratch_t *)http_scratch_alloc(sizeof(verify_scratch_t));
    if (!sc) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (req->content_len > 0) {
        int to_read = (req->content_len < sizeof(sc->body) - 1) ? (int)req->content_len : (int)(sizeof(sc->body) - 1);
        int ret = httpd_req_recv(req, sc->body, to_read);
        if (ret > 0) {
            sc->body[ret] = '\0';
            get_json_string(sc->body, "path", sc->rel_path, sizeof(sc->rel_path));
            get_json_string(sc->body, "expected_sha256", sc->expected_sha256, sizeof(sc->expected_sha256));
            if (sc->expected_sha256[0] == '\0') {
                get_json_string(sc->body, "sha256", sc->expected_sha256, sizeof(sc->expected_sha256));
            }
        }
    }

    if (sc->rel_path[0] == '\0') {
        if (httpd_req_get_url_query_str(req, sc->query, sizeof(sc->query)) == ESP_OK &&
            httpd_query_key_value(sc->query, "path", sc->path_enc, sizeof(sc->path_enc)) == ESP_OK) {
            url_decode(sc->rel_path, sc->path_enc, sizeof(sc->rel_path));
        }
    }

    if (sc->rel_path[0] == '\0' || !build_abs_path(sc->rel_path, sc->abs_path, sizeof(sc->abs_path))) {
        free(sc);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"invalid_path\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    struct stat st;
    if (stat(sc->abs_path, &st) != 0 || S_ISDIR(st.st_mode)) {
        free(sc);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"not_found\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    FILE *fp = fopen(sc->abs_path, "rb");
    if (!fp) {
        free(sc);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"not_found\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    mps3_sha256_init(&sc->sha_ctx);

    uint8_t *rd_buf = (!s_progress_active && s_bounce_persist && s_bounce_persist_sz >= 16384)
        ? s_bounce_persist
        : sc->read_buf;
    size_t rd_buf_sz = (rd_buf == s_bounce_persist) ? s_bounce_persist_sz : sizeof(sc->read_buf);

    size_t total_read = 0;
    size_t r;
    int chunks = 0;
    while ((r = fread(rd_buf, 1, rd_buf_sz, fp)) > 0) {
        mps3_sha256_update(&sc->sha_ctx, rd_buf, r);
        total_read += r;
        chunks++;
        if (chunks % 16 == 0) {
            podcast_sync_notify_activity();
            vTaskDelay(1);
        }
    }
    fclose(fp);

    uint8_t digest[32] = {0};
    mps3_sha256_final(&sc->sha_ctx, digest);

    for (int i = 0; i < 32; i++) {
        snprintf(&sc->sha256_hex[i * 2], 3, "%02x", digest[i]);
    }
    sc->sha256_hex[64] = '\0';

    if (sc->expected_sha256[0] != '\0' && strcasecmp(sc->expected_sha256, sc->sha256_hex) != 0) {
        snprintf(sc->resp_json, sizeof(sc->resp_json),
                 "{\"error\":\"sha256_mismatch\",\"expected\":\"%s\",\"actual\":\"%s\"}",
                 sc->expected_sha256, sc->sha256_hex);
        httpd_resp_set_status(req, "422 Unprocessable Entity");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, sc->resp_json, HTTPD_RESP_USE_STRLEN);
        free(sc);
        return ESP_FAIL;
    }

    json_escape(sc->rel_path, sc->rel_esc, sizeof(sc->rel_esc));
    snprintf(sc->resp_json, sizeof(sc->resp_json),
             "{\"status\":\"ok\",\"path\":\"%s\",\"size\":%llu,\"sha256\":\"%s\"}",
             sc->rel_esc, (unsigned long long)total_read, sc->sha256_hex);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, sc->resp_json, HTTPD_RESP_USE_STRLEN);
    free(sc);
    return ESP_OK;
}

static esp_err_t api_sync_begin_post_handler(httpd_req_t *req)
{
    if (!check_api_key(req)) return ESP_FAIL;
    podcast_sync_notify_activity();

    // Pausa reproducao se estiver ativa para liberar barramento SD durante o sync
    playback_state_t st;
    audio_player_get_state(&st);
    if (st.playing && !st.paused) {
        audio_player_toggle_play_pause();
    }
    audio_player_stop_url();

    char *staging_dir = (char *)http_scratch_alloc(256);
    if (!staging_dir) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int total_files = 0;
    uint64_t plan_bytes = 0;
    if (req->content_len > 0) {
        int to_read = (req->content_len < 255) ? (int)req->content_len : 255;
        int ret = httpd_req_recv(req, staging_dir, to_read);
        if (ret > 0) {
            staging_dir[ret] = '\0';
            const char *pf = strstr(staging_dir, "\"total_files\"");
            if (pf) {
                pf = strchr(pf, ':');
                if (pf) total_files = atoi(pf + 1);
            }
            const char *pb = strstr(staging_dir, "\"total_bytes\"");
            if (pb) {
                pb = strchr(pb, ':');
                if (pb) plan_bytes = (uint64_t)strtoull(pb + 1, NULL, 10);
            }
        }
    }

    snprintf(staging_dir, 256, "%s/Podcasts", SD_MOUNT_POINT);
    mkdir(staging_dir, 0777);

    snprintf(staging_dir, 256, "%s/Podcasts/.staging", SD_MOUNT_POINT);
    recursive_delete(staging_dir);
    mkdir(staging_dir, 0777);

    s_sync_in_progress = true;
    podcast_sync_on_begin(total_files, plan_bytes);

    uint64_t total_bytes = 0, free_bytes = 0;
    esp_vfs_fat_info(SD_MOUNT_POINT, &total_bytes, &free_bytes);

    snprintf(staging_dir, 256, "{\"status\":\"ok\",\"free_bytes\":%llu}", (unsigned long long)free_bytes);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, staging_dir, HTTPD_RESP_USE_STRLEN);
    free(staging_dir);
    return ESP_OK;
}

static esp_err_t api_sync_done_post_handler(httpd_req_t *req)
{
    if (!check_api_key(req)) return ESP_FAIL;
    podcast_sync_notify_activity();

    char *staging_dir = (char *)http_scratch_alloc(256);
    if (staging_dir) {
        snprintf(staging_dir, 256, "%s/Podcasts/.staging", SD_MOUNT_POINT);
        recursive_delete(staging_dir);
        free(staging_dir);
    }

    s_sync_in_progress = false;
    s_status_last_calc_ms = 0;
    progress_end();
    podcast_sync_mark_completed();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_podcasts_played_post_handler(httpd_req_t *req)
{
    if (!check_api_key(req)) return ESP_FAIL;
    podcast_sync_notify_activity();

    typedef struct {
        char buf[1024];
        char rel_path[400];
    } played_scratch_t;

    played_scratch_t *sc = (played_scratch_t *)http_scratch_alloc(sizeof(played_scratch_t));
    if (!sc) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int ret = httpd_req_recv(req, sc->buf, req->content_len < sizeof(sc->buf) - 1 ? req->content_len : sizeof(sc->buf) - 1);
    if (ret <= 0) {
        free(sc);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"empty_body\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    sc->buf[ret] = '\0';

    get_json_string(sc->buf, "rel_path", sc->rel_path, sizeof(sc->rel_path));
    if (sc->rel_path[0] == '\0') {
        get_json_string(sc->buf, "path", sc->rel_path, sizeof(sc->rel_path));
    }
    if (sc->rel_path[0] == '\0') {
        free(sc);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"missing_rel_path\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    bool played = true;
    const char *p = strstr(sc->buf, "\"played\"");
    if (p) {
        p += 8;
        while (*p && (*p == ' ' || *p == '\t' || *p == ':')) p++;
        if (*p == 'f' || *p == 'F' || *p == '0') {
            played = false;
        }
    }

    if (played) {
        podcast_history_mark_played(sc->rel_path, 0);
    } else {
        podcast_history_unmark_played(sc->rel_path);
    }

    free(sc);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t http_socket_open_cb(httpd_handle_t hd, int sockfd)
{
    int enable_nodelay = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (char *)&enable_nodelay, sizeof(enable_nodelay));

    struct timeval tv;
    tv.tv_sec = 60;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof(tv));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (char *)&tv, sizeof(tv));
    return ESP_OK;
}

static esp_err_t api_bench_sink_handler(httpd_req_t *req)
{
    size_t total_len = req->content_len;
    size_t received_total = 0;
    size_t buf_sz = 32768;
    char *rx_buf = (char *)heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rx_buf) {
        buf_sz = 4096;
        rx_buf = (char *)malloc(buf_sz);
    }
    if (!rx_buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int64_t t0 = esp_timer_get_time();
    int timeout_count = 0;
    uint32_t recv_calls = 0;
    int64_t recv_max_us = 0;

    while (received_total < total_len) {
        int to_read = (int)(total_len - received_total);
        if (to_read > (int)buf_sz) to_read = (int)buf_sz;
        int64_t tr0 = esp_timer_get_time();
        int ret = httpd_req_recv(req, rx_buf, to_read);
        int64_t trd = esp_timer_get_time() - tr0;
        if (trd > recv_max_us) recv_max_us = trd;
        recv_calls++;
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                if (++timeout_count > 50) break;
                continue;
            }
            break;
        }
        timeout_count = 0;
        received_total += ret;
    }
    int64_t t1 = esp_timer_get_time();
    free(rx_buf);
    int64_t elapsed_us = t1 - t0;
    float elapsed_s = (elapsed_us > 0) ? ((float)elapsed_us / 1000000.0f) : 0.001f;
    float mbs = ((float)received_total / (1024.0f * 1024.0f)) / elapsed_s;

    char resp[192];
    snprintf(resp, sizeof(resp), "{\"bytes\":%u,\"time_ms\":%u,\"speed_mbs\":%.2f,\"recv_calls\":%u,\"recv_max_ms\":%u}",
             (unsigned)received_total, (unsigned)(elapsed_us / 1000), mbs,
             (unsigned)recv_calls, (unsigned)(recv_max_us / 1000));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t api_bench_source_handler(httpd_req_t *req)
{
    char query[64] = {0};
    char size_str[32] = {0};
    size_t total_send = 10 * 1024 * 1024; // 10 MB padrao
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "size", size_str, sizeof(size_str)) == ESP_OK) {
            unsigned long long val = strtoull(size_str, NULL, 10);
            if (val >= 1024ULL && val <= 500ULL * 1024ULL * 1024ULL) total_send = (size_t)val;
        }
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Connection", "close");
    size_t chunk_sz = 32768;
    char *chunk = (char *)heap_caps_malloc(chunk_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chunk) {
        chunk_sz = 4096;
        chunk = (char *)malloc(chunk_sz);
    }
    if (!chunk) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    memset(chunk, 0xA5, chunk_sz);

    size_t sent = 0;
    while (sent < total_send) {
        size_t to_send = total_send - sent;
        if (to_send > chunk_sz) to_send = chunk_sz;
        if (httpd_resp_send_chunk(req, chunk, to_send) != ESP_OK) {
            free(chunk);
            httpd_resp_sendstr_chunk(req, NULL);
            return ESP_FAIL;
        }
        sent += to_send;
    }
    free(chunk);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t api_bench_sd_clock_handler(httpd_req_t *req)
{
    char query[64] = {0};
    char freq_str[32] = {0};
    uint32_t freq_khz = SDMMC_FREQ_DEFAULT;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "freq", freq_str, sizeof(freq_str)) == ESP_OK) {
            int mhz = atoi(freq_str);
            if (mhz == 40) freq_khz = SDMMC_FREQ_HIGHSPEED;
            else freq_khz = SDMMC_FREQ_DEFAULT;
        }
    }
    sd_card_set_frequency(freq_khz);
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"freq_khz\":%u,\"freq_mhz\":%.1f}",
             (unsigned)freq_khz, (double)freq_khz / 1000.0);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t api_analytics_manifest_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char *line = (char *)http_scratch_alloc(1024);
    if (!line) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_sendstr_chunk(req, "{\"status\":\"ok\",\"analytics_dir\":\"/.mps3_analytics\",\"files\":[");
    DIR *dir = opendir("/sdcard/.mps3_analytics");
    bool first = true;
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char full[320];
            snprintf(full, sizeof(full), "/sdcard/.mps3_analytics/%s", ent->d_name);
            struct stat st;
            if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
                snprintf(line, 1024, "%s{\"name\":\"%s\",\"download_path\":\"/.mps3_analytics/%s\",\"size\":%llu,\"mtime\":%llu}",
                         first ? "" : ",", ent->d_name, ent->d_name,
                         (unsigned long long)st.st_size, (unsigned long long)st.st_mtime);
                httpd_resp_sendstr_chunk(req, line);
                first = false;
            }
        }
        closedir(dir);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);
    free(line);
    return ESP_OK;
}

static esp_err_t start_httpd(void)
{
    if (s_httpd) return ESP_OK;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192; // 8 KB na DRAM interna (todos os handlers HTTP agora alocam buffers em PSRAM, usando <256B de stack)
    config.task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    config.core_id = 1; // Roda no Core 1 com 240 MHz livres
    config.ctrl_port = 32768;
    config.max_uri_handlers = 48;
    config.max_open_sockets = 10;
    config.backlog_conn = 8;
    config.lru_purge_enable = true;
    config.keep_alive_enable = true;
    config.keep_alive_idle = 10;
    config.keep_alive_interval = 5;
    config.keep_alive_count = 5;
    config.send_wait_timeout = 60;
    config.recv_wait_timeout = 60;
    config.open_fn = http_socket_open_cb;

    // Limpa qualquer PCB orfao deixado em LISTEN na porta 80 por sessoes anteriores
    LOCK_TCPIP_CORE();
    for (struct tcp_pcb_listen *p = tcp_listen_pcbs.listen_pcbs; p != NULL; ) {
        struct tcp_pcb_listen *next = p->next;
        if (p->local_port == config.server_port) {
            ESP_LOGW(TAG, "Fechando PCB preso na porta %d antes de subir httpd!", p->local_port);
            tcp_close((struct tcp_pcb *)p);
        }
        p = next;
    }
    UNLOCK_TCPIP_CORE();

    static uint16_t s_ctrl_port_cur = 32768;
    size_t largest_int = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    config.task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    if (largest_int >= 16384) {
        config.stack_size = 6144;
    } else {
        config.stack_size = 4096;
    }

    config.ctrl_port = s_ctrl_port_cur;
    ESP_LOGI(TAG, "Iniciando servidor HTTP (modo=%s, ip=%s, ctrl_port=%d, stack=%u, largest_int=%u)...",
             s_mode == WIFI_TRANSFER_MODE_STA ? "STA" : "AP", s_status, config.ctrl_port,
             (unsigned)config.stack_size, (unsigned)largest_int);
    esp_err_t err = httpd_start(&s_httpd, &config);
    if (err == ESP_ERR_NO_MEM || err == ESP_ERR_HTTPD_ALLOC_MEM || err == ESP_ERR_HTTPD_TASK) {
        s_ctrl_port_cur = (s_ctrl_port_cur >= 32800) ? 32768 : (uint16_t)(s_ctrl_port_cur + 1);
        config.ctrl_port = s_ctrl_port_cur;
        config.stack_size = 4608;
        ESP_LOGW(TAG, "Tentando httpd_start com stack=4608 (ctrl_port=%u)...", (unsigned)config.ctrl_port);
        err = httpd_start(&s_httpd, &config);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao subir o servidor HTTP: %s (codigo %d) | internal free=%u, maior bloco=%u, ctrl_port=%u",
                 esp_err_to_name(err), (int)err,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)s_ctrl_port_cur);
        // Proxima tentativa usa outra porta de controle (evita EADDRINUSE no socket UDP de controle).
        s_ctrl_port_cur = (s_ctrl_port_cur >= 32800) ? 32768 : (uint16_t)(s_ctrl_port_cur + 1);
        s_httpd = NULL;
        return err;
    }
    ESP_LOGI(TAG, "Servidor HTTP iniciado com sucesso na porta 80");

    httpd_uri_t now_playing_uri = { .uri = "/api/now_playing", .method = HTTP_GET, .handler = api_now_playing_get_handler };
    httpd_uri_t root_uri        = { .uri = "/",                .method = HTTP_GET, .handler = root_get_handler };
    httpd_uri_t index_uri       = { .uri = "/index.html",      .method = HTTP_GET, .handler = root_get_handler };
    httpd_uri_t gen204_uri      = { .uri = "/generate_204",    .method = HTTP_GET, .handler = http_captive_204_handler };
    httpd_uri_t gen204_alt_uri  = { .uri = "/gen_204",         .method = HTTP_GET, .handler = http_captive_204_handler };
    httpd_uri_t apple_uri       = { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = http_captive_apple_handler };
    httpd_uri_t list_uri   = { .uri = "/api/list",   .method = HTTP_GET,    .handler = api_list_get_handler };
    httpd_uri_t space_uri  = { .uri = "/api/space",  .method = HTTP_GET,    .handler = api_space_get_handler };
    httpd_uri_t status_uri = { .uri = "/api/status", .method = HTTP_GET,    .handler = api_status_get_handler };
    httpd_uri_t upload_uri = { .uri = "/api/upload", .method = HTTP_PUT,    .handler = api_upload_put_handler };
    httpd_uri_t delete_uri = { .uri = "/api/delete", .method = HTTP_DELETE, .handler = api_delete_handler };
    httpd_uri_t delete_post_uri = { .uri = "/api/delete", .method = HTTP_POST, .handler = api_delete_handler };
    httpd_uri_t download_uri = { .uri = "/api/download", .method = HTTP_GET, .handler = api_download_get_handler };
    httpd_uri_t wifi_nets_get_uri    = { .uri = "/api/wifi/networks", .method = HTTP_GET,    .handler = api_wifi_networks_get_handler };
    httpd_uri_t wifi_nets_post_uri   = { .uri = "/api/wifi/networks", .method = HTTP_POST,   .handler = api_wifi_networks_post_handler };
    httpd_uri_t wifi_nets_delete_uri = { .uri = "/api/wifi/networks", .method = HTTP_DELETE, .handler = api_wifi_networks_delete_handler };
    
    httpd_uri_t control_play_uri = { .uri = "/api/control/play", .method = HTTP_POST, .handler = api_control_play_handler };
    httpd_uri_t webradio_play_uri    = { .uri = "/api/webradio",      .method = HTTP_POST,   .handler = api_webradio_play_handler };
    httpd_uri_t webradio_stop_uri    = { .uri = "/api/webradio/stop", .method = HTTP_POST,   .handler = api_webradio_stop_handler };

    httpd_register_uri_handler(s_httpd, &now_playing_uri);
    httpd_register_uri_handler(s_httpd, &root_uri);
    httpd_register_uri_handler(s_httpd, &index_uri);
    httpd_register_uri_handler(s_httpd, &gen204_uri);
    httpd_register_uri_handler(s_httpd, &gen204_alt_uri);
    httpd_register_uri_handler(s_httpd, &apple_uri);
    httpd_register_uri_handler(s_httpd, &list_uri);
    httpd_register_uri_handler(s_httpd, &space_uri);
    httpd_register_uri_handler(s_httpd, &status_uri);
    httpd_register_uri_handler(s_httpd, &upload_uri);
    httpd_register_uri_handler(s_httpd, &delete_uri);
    httpd_register_uri_handler(s_httpd, &delete_post_uri);
    httpd_register_uri_handler(s_httpd, &download_uri);
    httpd_register_uri_handler(s_httpd, &wifi_nets_get_uri);
    httpd_register_uri_handler(s_httpd, &wifi_nets_post_uri);
    httpd_register_uri_handler(s_httpd, &wifi_nets_delete_uri);
    httpd_register_uri_handler(s_httpd, &control_play_uri);
    httpd_register_uri_handler(s_httpd, &webradio_play_uri);
    httpd_register_uri_handler(s_httpd, &webradio_stop_uri);
    httpd_uri_t eq_get_uri = { .uri = "/api/eq", .method = HTTP_GET, .handler = api_eq_get_handler };
    httpd_uri_t eq_post_uri = { .uri = "/api/eq", .method = HTTP_POST, .handler = api_eq_post_handler };
    httpd_uri_t rename_uri = { .uri = "/api/rename", .method = HTTP_POST, .handler = api_rename_post_handler };
    httpd_uri_t move_uri = { .uri = "/api/move", .method = HTTP_POST, .handler = api_move_post_handler };
    httpd_uri_t copy_uri = { .uri = "/api/copy", .method = HTTP_POST, .handler = api_copy_post_handler };
    httpd_uri_t mkdir_uri = { .uri = "/api/mkdir", .method = HTTP_POST, .handler = api_mkdir_post_handler };
    httpd_register_uri_handler(s_httpd, &eq_get_uri);
    httpd_register_uri_handler(s_httpd, &eq_post_uri);
    httpd_register_uri_handler(s_httpd, &rename_uri);
    httpd_register_uri_handler(s_httpd, &move_uri);
    httpd_register_uri_handler(s_httpd, &copy_uri);
    httpd_register_uri_handler(s_httpd, &mkdir_uri);
    httpd_uri_t ota_get_uri = { .uri = "/api/ota", .method = HTTP_GET, .handler = api_ota_get_handler };
    httpd_uri_t ota_post_uri = { .uri = "/api/ota", .method = HTTP_POST, .handler = api_ota_post_handler };
    httpd_register_uri_handler(s_httpd, &ota_get_uri);
    httpd_register_uri_handler(s_httpd, &ota_post_uri);

    httpd_uri_t podcasts_local_get_uri = { .uri = "/api/podcasts_local", .method = HTTP_GET, .handler = api_podcasts_local_get_handler };
    httpd_register_uri_handler(s_httpd, &podcasts_local_get_uri);

    httpd_uri_t sync_manifest_uri = { .uri = "/api/sync/manifest", .method = HTTP_GET,  .handler = api_sync_manifest_get_handler };
    httpd_uri_t sync_verify_uri   = { .uri = "/api/sync/verify",   .method = HTTP_POST, .handler = api_sync_verify_post_handler };
    httpd_uri_t sync_begin_uri    = { .uri = "/api/sync/begin",    .method = HTTP_POST, .handler = api_sync_begin_post_handler };
    httpd_uri_t sync_done_uri     = { .uri = "/api/sync/done",     .method = HTTP_POST, .handler = api_sync_done_post_handler };
    httpd_uri_t podcasts_played_uri = { .uri = "/api/podcasts/played", .method = HTTP_POST, .handler = api_podcasts_played_post_handler };
    httpd_register_uri_handler(s_httpd, &sync_manifest_uri);
    httpd_register_uri_handler(s_httpd, &sync_verify_uri);
    httpd_register_uri_handler(s_httpd, &sync_begin_uri);
    httpd_register_uri_handler(s_httpd, &sync_done_uri);
    httpd_register_uri_handler(s_httpd, &podcasts_played_uri);

    httpd_uri_t bench_sink_uri = { .uri = "/api/bench_sink", .method = HTTP_POST, .handler = api_bench_sink_handler };
    httpd_uri_t bench_source_uri = { .uri = "/api/bench_source", .method = HTTP_GET, .handler = api_bench_source_handler };
    httpd_uri_t analytics_manifest_uri = { .uri = "/api/analytics/manifest", .method = HTTP_GET, .handler = api_analytics_manifest_get_handler };
    httpd_register_uri_handler(s_httpd, &bench_sink_uri);
    httpd_register_uri_handler(s_httpd, &bench_source_uri);
    httpd_register_uri_handler(s_httpd, &analytics_manifest_uri);

    httpd_uri_t bench_sd_clock_uri = { .uri = "/api/bench/sd_clock", .method = HTTP_POST, .handler = api_bench_sd_clock_handler };
    httpd_register_uri_handler(s_httpd, &bench_sd_clock_uri);

    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, http_404_error_handler);

    return ESP_OK;
}


// --- WiFi ------------------------------------------------------------------

static bool s_bw_is_ht40 = false;

// Sequencia de candidatos tentados no modo STA nesta sessao: a lista de
// redes conhecidas (da mais recente pra' mais antiga), seguida do SSID
// fixo do Kconfig (se configurado e ainda nao estiver na lista) como
// ultimo recurso. wifi_event_handler avanca s_candidate_idx sozinho
// quando uma rede esgota as tentativas, sem precisar que ninguem fique
// chamando wifi_transfer_enter() de novo.
static wifi_known_net_t s_candidates[WIFI_KNOWN_MAX + 1];
static int s_candidate_count = 0;
static int s_candidate_idx = 0;

static int build_sta_candidates(void)
{
    known_networks_load();

    int n = 0;
    for (int i = 0; i < s_known_count && n < WIFI_KNOWN_MAX; i++) {
        s_candidates[n++] = s_known[i];
    }

    if (strlen(CONFIG_WIFI_STA_SSID) > 0) {
        bool already_known = false;
        for (int i = 0; i < n; i++) {
            if (strcmp(s_candidates[i].ssid, CONFIG_WIFI_STA_SSID) == 0) { already_known = true; break; }
        }
        if (!already_known && n < WIFI_KNOWN_MAX + 1) {
            strncpy(s_candidates[n].ssid, CONFIG_WIFI_STA_SSID, sizeof(s_candidates[n].ssid) - 1);
            s_candidates[n].ssid[sizeof(s_candidates[n].ssid) - 1] = '\0';
            strncpy(s_candidates[n].pass, CONFIG_WIFI_STA_PASSWORD, sizeof(s_candidates[n].pass) - 1);
            s_candidates[n].pass[sizeof(s_candidates[n].pass) - 1] = '\0';
            n++;
        }
    }

    return n;
}

// Aplica o candidato s_candidates[s_candidate_idx] no radio STA (ja'
// rodando) e reconecta. Usada pelo wifi_event_handler pra' pular pro
// proximo candidato quando o atual esgota as tentativas - a primeira
// tentativa da sessao e' configurada direto em wifi_transfer_enter()
// (que ainda precisa dar esp_wifi_start()).
static esp_err_t apply_sta_candidate(void)
{
    wifi_config_t wifi_config = { 0 };
    strncpy((char *)wifi_config.sta.ssid, s_candidates[s_candidate_idx].ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, s_candidates[s_candidate_idx].pass, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = strlen(s_candidates[s_candidate_idx].pass) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

    ESP_LOGI(TAG, "Tentando rede conhecida %d/%d: SSID=%s",
             s_candidate_idx + 1, s_candidate_count, s_candidates[s_candidate_idx].ssid);

    s_sta_retry = 0;
    s_got_ip = false;
    return esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
            uint8_t reason = disc ? disc->reason : 0;
            s_got_ip = false;
            s_bw_is_ht40 = false;
            ESP_LOGW(TAG, "STA desconectou (reason=%u). Tentativa %d/%d (Candidato %d/%d: SSID=%s)",
                     reason, s_sta_retry + 1, WIFI_STA_MAX_RETRY,
                     s_candidate_idx + 1, s_candidate_count,
                     (s_candidate_idx < s_candidate_count) ? s_candidates[s_candidate_idx].ssid : "N/A");
            if (s_active && (s_mode == WIFI_TRANSFER_MODE_STA || s_mode == WIFI_TRANSFER_MODE_APSTA)) {
                if (s_sta_retry < WIFI_STA_MAX_RETRY) {
                    s_sta_retry++;
                    esp_wifi_connect();
                } else if (s_candidate_idx + 1 < s_candidate_count) {
                    // Essa rede esgotou as tentativas - passa pra' proxima
                    // rede conhecida antes de desistir e cair pro hotspot.
                    s_candidate_idx++;
                    s_sta_retry = 0;
                    apply_sta_candidate();
                } else {
                    s_sta_failed = true;
                    ESP_LOGW(TAG, "Tentativas STA esgotadas. Hotspot AP (192.168.4.1) continua ativo!");
                }
            }
        } else if (event_id == WIFI_EVENT_AP_START) {
            s_ap_ready = true;
            ESP_LOGI(TAG, "Hotspot AP iniciado! SSID='%s' IP=192.168.4.1", CONFIG_WIFI_AP_SSID);
        } else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
            wifi_event_ap_staconnected_t *st = (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "Cliente conectou ao Hotspot AP (AID=%d)", (int)st->aid);
        } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
            wifi_event_ap_stadisconnected_t *st = (wifi_event_ap_stadisconnected_t *)event_data;
            ESP_LOGI(TAG, "Cliente desconectou do Hotspot AP (AID=%d)", (int)st->aid);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        snprintf(s_status, sizeof(s_status), IPSTR, IP2STR(&evt->ip_info.ip));
        s_got_ip = true;
        s_sta_retry = 0;
        s_sta_failed = false;

        // Com o link STA associado, desativa modem sleep, aplica potencia maxima (84 = 21 dBm)
        // e fixa HT20 (20 MHz) livre de interferencia de coexistencia 2.4 GHz (~2.46 MB/s sustentados)
        esp_wifi_set_ps(WIFI_PS_NONE);
        esp_wifi_set_max_tx_power(84);
        esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);
        s_bw_is_ht40 = false;

        ESP_LOGI(TAG, "===> CONECTADO NA REDE WIFI LOCAL (PS_NONE, BW20, TX=84)! <===");
        ESP_LOGI(TAG, "IP STA obtido: %s | Hotspot AP: 192.168.4.1", s_status);
        ESP_LOGI(TAG, "Acesse pelo navegador: http://%s ou http://192.168.4.1 ou http://mps3.local", s_status);

        // Rede que funcionou vai pro topo da lista de conhecidas - da'
        // proxima vez essa e' a primeira tentativa, sem precisar navegar
        // pelas outras antes de chegar nela.
        if (s_candidate_idx < s_candidate_count) {
            known_networks_put_front(s_candidates[s_candidate_idx].ssid, s_candidates[s_candidate_idx].pass);
        }
    }
}

static esp_err_t ensure_stack_ready(void)
{
    if (s_stack_ready) return ESP_OK;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    if (!s_netif_sta) s_netif_sta = esp_netif_create_default_wifi_sta();
    if (!s_netif_ap)  s_netif_ap  = esp_netif_create_default_wifi_ap();

    ESP_LOGI(TAG, "[MEM] pre-init: internal free=%u, largest DMA block=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));

    // Com CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y, os buffers dinamicos RX/TX ficam na PSRAM (8 MB),
    // enquanto static_rx_buf_num fica na SRAM interna.
    // NUNCA alocar buffers auxiliares antes do esp_wifi_init para garantir que o driver Wi-Fi
    // tenha toda a SRAM interna DMA contígua necessária para inicializar seus descritores e tarefas.
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.static_rx_buf_num = 4;
    cfg.dynamic_rx_buf_num = 32;
    cfg.tx_buf_type = 0;        // Buffer de transmissao estatico em SRAM DMA para confianca absoluta de TX/ARP/ACK
    cfg.static_tx_buf_num = 8;  // 8 buffers estaticos DMA na SRAM interna (~12.8 KB)
    cfg.dynamic_tx_buf_num = 32;
    cfg.cache_tx_buf_num = 16;
    cfg.rx_mgmt_buf_type = 0;
    cfg.rx_ba_win = 32;         // Janela Block Ack 32
    cfg.mgmt_sbuf_num = 8;
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_init customizado falhou (%s), tentando DEFAULT...", esp_err_to_name(err));
        wifi_init_config_t def_cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&def_cfg);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init falhou: %s", esp_err_to_name(err));
        return err;
    }

    // Aloca o bounce DMA de upload APÓS o Wi-Fi já ter subido com sucesso
    if (!s_bounce_persist) {
        size_t max_dma = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (max_dma >= 16384 + 1024) {
            s_bounce_persist_sz = 16384;
            s_bounce_persist = (uint8_t *)heap_caps_aligned_alloc(16, s_bounce_persist_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        } else if (max_dma >= 8192 + 1024) {
            s_bounce_persist_sz = 8192;
            s_bounce_persist = (uint8_t *)heap_caps_aligned_alloc(16, s_bounce_persist_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        } else if (max_dma >= 4096) {
            s_bounce_persist_sz = 4096;
            s_bounce_persist = (uint8_t *)heap_caps_aligned_alloc(16, s_bounce_persist_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        }
    }

    ESP_LOGI(TAG, "[MEM] pos-init: internal free=%u, largest DMA block=%u, bounce reservado=%u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)s_bounce_persist_sz);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    if (!s_progress_mutex) {
        s_progress_mutex = xSemaphoreCreateMutex();
    }

    s_stack_ready = true;
    return ESP_OK;
}

esp_err_t wifi_transfer_enter(wifi_transfer_mode_t mode)
{
    if (s_active) return ESP_ERR_INVALID_STATE;
    
    // Se havia uma sessao HTTP residual, encerra antes de comecar
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    stop_mdns_service();

    // Pausa a música automaticamente ao entrar no WiFi
    playback_state_t st;
    audio_player_get_state(&st);
    if (st.playing && !st.paused) {
        audio_player_toggle_play_pause();
    }

    if (mode == WIFI_TRANSFER_MODE_STA || mode == WIFI_TRANSFER_MODE_APSTA) {
        s_candidate_count = build_sta_candidates();
        s_candidate_idx = 0;
        if (s_candidate_count == 0 && mode == WIFI_TRANSFER_MODE_STA) {
            strncpy(s_status, "SSID nao configurado", sizeof(s_status) - 1);
            s_status[sizeof(s_status) - 1] = '\0';
            return ESP_ERR_INVALID_STATE;
        }
    }

    audio_player_release_sd_for_usb();

    esp_err_t err = ensure_stack_ready();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar o WiFi: %s", esp_err_to_name(err));
        audio_player_reacquire_sd_after_usb();
        return err;
    }

    // Comuta o barramento SDMMC dinamicamente para 20 MHz (Default Speed)
    // para eliminar a interferencia da 61a harmonica de 40 MHz (2440 MHz) sobre o canal 6 do Wi-Fi (2437 MHz).
    sd_card_set_frequency(SDMMC_FREQ_DEFAULT);

    s_mode = mode;
    s_files_received = 0;
    s_exit_requested = false;
    s_got_ip = false;
    s_ap_ready = false;
    s_sta_failed = false;
    s_sta_retry = 0;
    s_auto_fallback = (mode == WIFI_TRANSFER_MODE_STA);
    s_wifi_last_drawn_state = (wifi_ui_state_t)-1;
    s_wifi_last_draw_ms = 0;
    if (mode == WIFI_TRANSFER_MODE_AP || mode == WIFI_TRANSFER_MODE_APSTA) {
        strncpy(s_status, "192.168.4.1", sizeof(s_status) - 1);
    } else {
        strncpy(s_status, "Conectando...", sizeof(s_status) - 1);
    }
    s_status[sizeof(s_status) - 1] = '\0';

    wifi_config_t ap_config = { 0 };
    wifi_config_t sta_config = { 0 };

    if (mode == WIFI_TRANSFER_MODE_AP || mode == WIFI_TRANSFER_MODE_APSTA) {
        strncpy((char *)ap_config.ap.ssid, CONFIG_WIFI_AP_SSID, sizeof(ap_config.ap.ssid) - 1);
        ap_config.ap.ssid_len = strlen(CONFIG_WIFI_AP_SSID);
        strncpy((char *)ap_config.ap.password, CONFIG_WIFI_AP_PASSWORD, sizeof(ap_config.ap.password) - 1);
        ap_config.ap.channel = 11;
        ap_config.ap.beacon_interval = 100;
        ap_config.ap.max_connection = 4;
        ap_config.ap.authmode = strlen(CONFIG_WIFI_AP_PASSWORD) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

        ESP_LOGI(TAG, "===> MODO HOTSPOT ATIVO <===");
        ESP_LOGI(TAG, "Rede gerada pelo MPS3: SSID='%s' | Senha='%s' | Canal=11", CONFIG_WIFI_AP_SSID, CONFIG_WIFI_AP_PASSWORD);
        ESP_LOGI(TAG, "Conecte o seu PC/celular a essa rede e acesse: http://192.168.4.1 ou http://mps3.local");
    }

    if (mode == WIFI_TRANSFER_MODE_STA || mode == WIFI_TRANSFER_MODE_APSTA) {
        if (s_candidate_count > 0) {
            strncpy((char *)sta_config.sta.ssid, s_candidates[s_candidate_idx].ssid, sizeof(sta_config.sta.ssid) - 1);
            strncpy((char *)sta_config.sta.password, s_candidates[s_candidate_idx].pass, sizeof(sta_config.sta.password) - 1);
            sta_config.sta.threshold.authmode = strlen(s_candidates[s_candidate_idx].pass) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

            ESP_LOGI(TAG, "Tentando rede conhecida %d/%d: SSID=%s",
                     s_candidate_idx + 1, s_candidate_count, s_candidates[s_candidate_idx].ssid);
        }
    }

    if (mode == WIFI_TRANSFER_MODE_APSTA) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        if (s_candidate_count > 0) {
            esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        }
    } else if (mode == WIFI_TRANSFER_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_AP);
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    } else {
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao ligar o radio WiFi: %s", esp_err_to_name(err));
        audio_player_reacquire_sd_after_usb();
        return err;
    }

    s_active = true;
    // No modo de transferencia WiFi, desativa o modem sleep para resposta TCP instantanea:
    esp_wifi_set_ps(WIFI_PS_NONE);
    // Potencia no maximo absoluto do hardware ESP32-S3 (84 = 21.0 dBm) para ganho maximo de antena:
    esp_wifi_set_max_tx_power(84);

    wifi_interface_t ifx = (mode == WIFI_TRANSFER_MODE_STA) ? WIFI_IF_STA : WIFI_IF_AP;
    esp_wifi_set_bandwidth(ifx, WIFI_BW20);

    ESP_LOGI(TAG, "Modo WiFi (%s) iniciado",
             mode == WIFI_TRANSFER_MODE_APSTA ? "AP+STA simultaneo" :
             (mode == WIFI_TRANSFER_MODE_STA ? "estacao" : "hotspot"));
    return ESP_OK;
}

esp_err_t wifi_transfer_enter_auto(void)
{
    int candidates = build_sta_candidates();
    if (candidates > 0) {
        esp_err_t err = wifi_transfer_enter(WIFI_TRANSFER_MODE_STA);
        if (err == ESP_OK) {
            s_auto_fallback = true;
            return ESP_OK;
        }
    }
    ESP_LOGI(TAG, "Nenhuma rede STA disponivel, iniciando direto em modo AP...");
    s_auto_fallback = false;
    return wifi_transfer_enter(WIFI_TRANSFER_MODE_AP);
}

bool wifi_transfer_poll(void)
{
    if (!s_active) return false;

    update_ui_state();

    static uint32_t s_last_httpd_attempt_ms = 0;
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    // Keepalive de ARP/broadcast na rede local a cada 3s para evitar que roteadores e switches percam o MAC do MPS3
    static uint32_t s_last_arp_keepalive_ms = 0;
    if (s_got_ip && (now_ms - s_last_arp_keepalive_ms >= 3000)) {
        s_last_arp_keepalive_ms = now_ms;
        int ks = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (ks >= 0) {
            int bcast = 1;
            setsockopt(ks, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));
            struct sockaddr_in dst = {
                .sin_family = AF_INET,
                .sin_port = htons(5353),
                .sin_addr.s_addr = inet_addr("255.255.255.255")
            };
            const char bcast_msg[] = "MPS3";
            sendto(ks, bcast_msg, sizeof(bcast_msg), 0, (struct sockaddr *)&dst, sizeof(dst));
            close(ks);
        }
    }

    if (!s_httpd) {
        bool can_start = false;
        if (s_mode == WIFI_TRANSFER_MODE_APSTA && (s_ap_ready || s_got_ip)) {
            can_start = true;
        } else if (s_mode == WIFI_TRANSFER_MODE_AP && s_ap_ready) {
            can_start = true;
        } else if (s_mode == WIFI_TRANSFER_MODE_STA && s_got_ip) {
            can_start = true;
        }

        if (can_start) {
            if (now_ms - s_last_httpd_attempt_ms >= 1000) {
                s_last_httpd_attempt_ms = now_ms;
                if (start_httpd() == ESP_OK) {
                    start_mdns_service();
                    s_auto_fallback = false;
                    // Inicia o servidor DNS cativo se AP estiver ativo
                    if ((s_mode == WIFI_TRANSFER_MODE_AP || s_mode == WIFI_TRANSFER_MODE_APSTA) && !s_dns_task_handle) {
                        xTaskCreate(dns_server_task, "dns", 4096, NULL, 5, &s_dns_task_handle);
                    }
                }
            }
        }
    } else if (s_mode == WIFI_TRANSFER_MODE_STA && s_sta_failed) {
        if (s_auto_fallback) {
            ESP_LOGW(TAG, "STA falhou apos tentativas, trocando para AP...");
            esp_wifi_stop();
            s_active = false;

            s_mode = WIFI_TRANSFER_MODE_AP;
            s_files_received = 0;
            s_exit_requested = false;
            s_got_ip = false;
            s_ap_ready = false;
            s_sta_failed = false;
            s_sta_retry = 0;
            strncpy(s_status, "192.168.4.1", sizeof(s_status) - 1);
            s_status[sizeof(s_status) - 1] = '\0';

            wifi_config_t wifi_config = { 0 };
            strncpy((char *)wifi_config.ap.ssid, CONFIG_WIFI_AP_SSID, sizeof(wifi_config.ap.ssid) - 1);
            wifi_config.ap.ssid_len = strlen(CONFIG_WIFI_AP_SSID);
            strncpy((char *)wifi_config.ap.password, CONFIG_WIFI_AP_PASSWORD, sizeof(wifi_config.ap.password) - 1);
            wifi_config.ap.channel = 11;
            wifi_config.ap.max_connection = 4;
            wifi_config.ap.authmode = strlen(CONFIG_WIFI_AP_PASSWORD) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

            ESP_LOGI(TAG, "===> MODO HOTSPOT ATIVO (Fallback) <===");
            ESP_LOGI(TAG, "Rede gerada pelo MPS3: SSID='%s' | Senha='%s' | Canal=11", CONFIG_WIFI_AP_SSID, CONFIG_WIFI_AP_PASSWORD);
            ESP_LOGI(TAG, "Conecte o seu PC/celular a essa rede e acesse: http://192.168.4.1 ou http://mps3.local");

            esp_wifi_set_mode(WIFI_MODE_AP);
            esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
            esp_wifi_start();
            s_active = true;
            s_auto_fallback = false;
        } else {
            strncpy(s_status, "Rede indisponivel", sizeof(s_status) - 1);
            s_status[sizeof(s_status) - 1] = '\0';
        }
    }

    if (s_exit_requested && !s_ota_in_progress) {
        s_last_httpd_attempt_ms = 0;
        if (s_httpd) {
            httpd_stop(s_httpd);
            s_httpd = NULL;
        }
        // Libera buffers PSRAM persistentes da sessao Wi-Fi ao voltar para o player de musica
        if (s_rb_persist) {
            vRingbufferDeleteWithCaps(s_rb_persist);
            s_rb_persist = NULL;
            s_rb_persist_sz = 0;
        }
        if (s_recv_buf_persist) {
            free(s_recv_buf_persist);
            s_recv_buf_persist = NULL;
            s_recv_buf_persist_sz = 0;
        }
        // Para o servidor DNS
        if (s_dns_task_handle) {
            if (s_dns_socket >= 0) {
                close(s_dns_socket);
                s_dns_socket = -1;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            s_dns_task_handle = NULL;
        }
        stop_mdns_service();
        esp_wifi_stop();
        s_active = false;
        s_ui_state = WIFI_UI_IDLE;
        s_auto_fallback = false;
        s_status[0] = '\0';
        progress_end();


        // Remontagem robusta do cartao SD (reinicializa o host SDMMC e remonta o FatFS do zero)
        ESP_LOGI(TAG, "Remontando cartao SD apos encerramento do WiFi...");
        sd_card_deinit();
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_err_t remount_err = sd_card_init();
        if (remount_err != ESP_OK) {
            ESP_LOGE(TAG, "Falha ao remontar SD Card apos WiFi: %s", esp_err_to_name(remount_err));
        } else {
            ESP_LOGI(TAG, "SD Card remontado com sucesso apos WiFi (40 MHz High-Speed restaurado)");
            sd_card_set_frequency(SDMMC_FREQ_HIGHSPEED);
        }
        audio_player_reacquire_sd_after_usb();
        ESP_LOGI(TAG, "Modo WiFi encerrado (%d arquivo(s) recebido(s))", s_files_received);
        return true;
    }

    return false;
}

void wifi_transfer_request_exit(void)
{
    if (s_ota_in_progress) {
        ESP_LOGW(TAG, "Tentativa de sair do menu WiFi ignorada: atualizacao OTA em andamento!");
        return;
    }
    s_exit_requested = true;
}

bool wifi_transfer_is_active(void)        { return s_active; }
bool wifi_transfer_is_ota_busy(void)      { return s_ota_in_progress; }
bool wifi_transfer_is_transferring(void)  { return s_progress_active || s_ota_in_progress; }
bool wifi_transfer_has_sta_ip(void)       { return s_got_ip; }

bool wifi_transfer_get_gateway_ip(char *out_gw, size_t max_len)
{
    if (!out_gw || max_len < 8) return false;
    out_gw[0] = '\0';
    if (!s_netif_sta) return false;

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(s_netif_sta, &ip_info) == ESP_OK) {
        if (ip_info.gw.addr != 0) {
            snprintf(out_gw, max_len, IPSTR, IP2STR(&ip_info.gw));
            return true;
        }
    }
    return false;
}

void wifi_transfer_get_status(char *out, size_t out_len, int *files_received)
{
    if (out && out_len > 0) {
        strlcpy(out, s_status, out_len);
    }
    if (files_received) *files_received = s_files_received;
}

// --- Registro no menu principal ------------------------------------------
// wifi_transfer_enter_auto() ja' existe (tenta a rede de casa, cai pro
// hotspot se nao der certo) - so' precisa de um wrapper void(void) pro
// menu. draw_status() reproduz a mesma logica de montar o rotulo/progresso
// que antes vivia em main.c (display_task) - so' mudou de arquivo.

static void wifi_menu_on_select(void)
{
    oled_display_show_loading();
    wifi_transfer_enter_auto();
}

static void wifi_menu_draw_status(void)
{
    static int s_cached_rssi = -50;
    static uint32_t s_last_rssi_ms = 0;

    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    bool state_changed = (s_ui_state != s_wifi_last_drawn_state);

    // Controla a frequencia de redesenho I2C:
    // - Em transferencia: 100 ms (~10 FPS) para barra de progresso fluida
    // - Em repouso/conectado: 250 ms (~4 FPS) ou imediato na mudanca de estado
    uint32_t min_interval_ms = (s_ui_state == WIFI_UI_TRANSFERRING || s_ui_state == WIFI_UI_OTA_UPDATING) ? 100 : 250;
    if (!state_changed && (now_ms - s_wifi_last_draw_ms < min_interval_ms)) {
        return;
    }
    s_wifi_last_draw_ms = now_ms;
    s_wifi_last_drawn_state = s_ui_state;

    wifi_transfer_progress_t prog;
    wifi_transfer_get_progress(&prog);

    char status[64];
    int files = 0;
    wifi_transfer_get_status(status, sizeof(status), &files);

    // Extrai IP do status
    char ip[16] = "0.0.0.0";
    if (strchr(status, '.') != NULL) {
        strlcpy(ip, status, sizeof(ip));
    }

    // Nunca chama esp_wifi_sta_get_rssi() durante transferencia ativa (evita lock IPC na task Wi-Fi!)
    int rssi = -1;
    if (s_mode == WIFI_TRANSFER_MODE_STA || s_mode == WIFI_TRANSFER_MODE_APSTA) {
        if (!s_progress_active && !s_ota_in_progress && (s_last_rssi_ms == 0 || (now_ms - s_last_rssi_ms >= 5000))) {
            if (esp_wifi_sta_get_rssi(&s_cached_rssi) == ESP_OK) {
                s_last_rssi_ms = now_ms;
            }
        }
        rssi = s_cached_rssi;
    }

    bool dns_active = (s_dns_socket >= 0);

    switch (s_ui_state) {
        case WIFI_UI_IDLE:
            if (s_mode == WIFI_TRANSFER_MODE_APSTA && s_got_ip) {
                oled_display_show_wifi_connected(ip, "mps3.local", rssi, 0);
            } else if (s_mode == WIFI_TRANSFER_MODE_AP || s_mode == WIFI_TRANSFER_MODE_APSTA) {
                oled_display_show_wifi_qr(CONFIG_WIFI_AP_SSID, CONFIG_WIFI_AP_PASSWORD, "AP MPS3", 1, 1);
            } else {
                oled_display_show_wifi_idle(ip, "mps3.local", dns_active, rssi);
            }
            break;
        case WIFI_UI_CONNECTED: {
            int clients = (s_mode == WIFI_TRANSFER_MODE_AP || s_mode == WIFI_TRANSFER_MODE_APSTA) ? wifi_transfer_get_connected_clients() : 0;
            oled_display_show_wifi_connected(ip, "mps3.local", rssi, clients);
            break;
        }
        case WIFI_UI_TRANSFERRING:
            oled_display_show_wifi_transfer(
                prog.filename,
                prog.file_pct,
                prog.overall_pct >= 0 ? prog.overall_pct : 0,
                prog.idx,
                prog.count,
                prog.kbps,
                prog.eta_sec,
                prog.total_eta_sec,
                rssi
            );
            break;
        case WIFI_UI_DONE:
            oled_display_show_wifi_done(files);
            break;
        case WIFI_UI_OTA_UPDATING:
        case WIFI_UI_OTA_FINISHED:
            oled_display_show_ota_progress(s_ota_pct, s_ota_status);
            break;
        default:
            break;
    }
}

void wifi_transfer_register_menu_entry(void)
{
    static const menu_item_t item = {
        .name = "WiFi",
        .on_select = wifi_menu_on_select,
        .is_active = wifi_transfer_is_active,
        .poll = wifi_transfer_poll,
        .draw_status = wifi_menu_draw_status,
        .request_exit = wifi_transfer_request_exit,
    };
    menu_register(&item);
}





