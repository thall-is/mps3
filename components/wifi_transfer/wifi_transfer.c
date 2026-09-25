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
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

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


static esp_err_t api_now_playing_get_handler(httpd_req_t *req)
{
    playback_state_t st;
    audio_player_get_state(&st);
    
    char json[512];
    snprintf(json, sizeof(json), "{\"state\":\"%s\", \"title\":\"%s\", \"artist\":\"%s\"}", 
        st.playing ? "playing" : "stopped",
        st.title,
        st.artist);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, -1);
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
    static uint32_t last_calc_ms = 0;
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    
    // esp_vfs_fat_info é pesadíssimo, ele pode ler milhares de setores do FAT 
    // dependendo da placa. Fazer isso a cada 8s por causa do web page polling
    // trava o SD card inteiro! Só recalcula 1x por minuto ou no boot.
    if (cached_total == 0 || (now - last_calc_ms > 60000)) {
        esp_vfs_fat_info(SD_MOUNT_POINT, &cached_total, &cached_free);
        last_calc_ms = now;
    }
    uint64_t total_bytes = cached_total;
    uint64_t free_bytes = cached_free;
    
    int mv = 0, percent = 0, time_left = -1;
    battery_get_info(&mv, &percent, &time_left);

    char json[150];
    snprintf(json, sizeof(json), "{\"total\":%llu,\"free\":%llu,\"battery_mv\":%d,\"battery_percent\":%d,\"battery_time_left\":%d}",
             (unsigned long long)total_bytes, (unsigned long long)free_bytes, mv, percent, time_left);
             
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
static bool s_progress_active = false;
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
    s_mdns_started = true;
    ESP_LOGI(TAG, "mDNS iniciado: http://mps3.local");
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
    if (!s_progress_mutex) return;
    xSemaphoreTake(s_progress_mutex, portMAX_DELAY);
    s_progress_file_done = done;
    xSemaphoreGive(s_progress_mutex);
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
}

static bool build_abs_path(const char *rel, char *out, size_t out_len)
{
    if (!rel) return false;
    while (*rel == '/') rel++;
    if (strstr(rel, "..") != NULL) return false;
    if (rel[0] == '\0') {
        snprintf(out, out_len, "%s", SD_MOUNT_POINT);
    } else {
        snprintf(out, out_len, "%s/%s", SD_MOUNT_POINT, rel);
    }
    return true;
}

static void mkdir_p_for_file(const char *abs_file_path)
{
    char tmp[600];
    strncpy(tmp, abs_file_path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
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
    char child[700];
    esp_err_t err = ESP_OK;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        snprintf(child, sizeof(child), "%s/%s", abs_path, entry->d_name);
        err = recursive_delete(child);
        if (err != ESP_OK) break;
    }
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

// --- Handlers HTTP ---------------------------------------------------------

static esp_err_t api_list_get_handler(httpd_req_t *req)
{
    char query[256] = {0};
    char path_enc[192] = {0};
    char rel_path[192] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "path", path_enc, sizeof(path_enc));
        url_decode(rel_path, path_enc, sizeof(rel_path));
    }

    char abs_path[400];
    if (!build_abs_path(rel_path, abs_path, sizeof(abs_path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "caminho invalido");
        return ESP_FAIL;
    }

    DIR *d = opendir(abs_path);
    if (!d) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "pasta nao encontrada");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"entries\":[");

    struct dirent *entry;
    bool first = true;
    char name_esc[300];
    char chunk[420];
    char out_buf[2048];
    int out_len = 0;
    int files_yield = 0;

    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        
        // Em vez de stat(), confiamos no d_type pra saber se eh pasta.
        // O tamanho fica como 0, pois nao vale a pena parar o cartao SD p/ calcular.
        bool is_dir = (entry->d_type == DT_DIR);
        long size = 0;
        
        json_escape(entry->d_name, name_esc, sizeof(name_esc));
        int len = snprintf(chunk, sizeof(chunk), "%s{\"name\":\"%s\",\"dir\":%s,\"size\":%ld}",
                 first ? "" : ",", name_esc, is_dir ? "true" : "false", size);
                 
        if (out_len + len >= sizeof(out_buf) - 1) {
            httpd_resp_send_chunk(req, out_buf, out_len);
            out_len = 0;
        }
        memcpy(out_buf + out_len, chunk, len);
        out_len += len;
        first = false;
        
        // Pausa cooperativa pra nao engasgar o audio bloqueando o SD Card
        files_yield++;
        if (files_yield % 3 == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    closedir(d);
    
    if (out_len > 0) {
        httpd_resp_send_chunk(req, out_buf, out_len);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t api_upload_put_handler(httpd_req_t *req)
{
    if (s_ota_in_progress) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Atualizacao OTA em andamento");
        return ESP_FAIL;
    }

    // Parar qualquer reproducao local para liberar o SD e evitar concorrencia
    playback_state_t st;
    audio_player_get_state(&st);
    if (st.playing && !st.paused) {
        audio_player_toggle_play_pause();
    }
    audio_player_stop_url();

    char query[512];
    char path_enc[400] = {0};
    char rel_path[400] = {0};
    int idx = 1, count = 1;
    long long batch_total = 0;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", path_enc, sizeof(path_enc)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "faltou o parametro 'path'");
        return ESP_FAIL;
    }
    url_decode(rel_path, path_enc, sizeof(rel_path));

    char num_buf[20];
    if (httpd_query_key_value(query, "idx", num_buf, sizeof(num_buf)) == ESP_OK) idx = atoi(num_buf);
    if (httpd_query_key_value(query, "count", num_buf, sizeof(num_buf)) == ESP_OK) count = atoi(num_buf);
    if (httpd_query_key_value(query, "batchTotal", num_buf, sizeof(num_buf)) == ESP_OK) batch_total = atoll(num_buf);
    if (idx < 1) idx = 1;
    if (count < 1) count = 1;

    char abs_path[600];
    if (!build_abs_path(rel_path, abs_path, sizeof(abs_path)) || rel_path[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "caminho invalido");
        return ESP_FAIL;
    }

    mkdir_p_for_file(abs_path);

    const char *display_name = rel_path;
    const char *slash = strrchr(rel_path, '/');
    if (slash) display_name = slash + 1;

    FILE *fp = fopen(abs_path, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Nao foi possivel criar %s (errno=%d: %s)", abs_path, errno, strerror(errno));
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg), "nao foi possivel criar o arquivo no cartao (%s)", strerror(errno));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, err_msg);
        return ESP_FAIL;
    }

    size_t content_len = req->content_len;
    progress_begin(display_name, content_len, idx, count, batch_total > 0 ? batch_total : (long long)content_len);

    size_t buf_size = 32768;
    char *buf = (char *)heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) {
        buf_size = 16384;
        buf = (char *)heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    }
    if (!buf) {
        buf_size = 8192;
        buf = (char *)malloc(buf_size);
    }
    if (!buf) {
        fclose(fp);
        remove(abs_path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sem memoria para o buffer de upload");
        progress_end();
        return ESP_FAIL;
    }

    size_t remaining = content_len;
    bool ok = true;
    int consecutive_timeouts = 0;
    const int max_consecutive_timeouts = 100;
    uint32_t last_prog_update_ms = 0;
    size_t buf_fill = 0;
    char fail_detail[128] = "conexao interrompida durante o envio";

    while (remaining > 0) {
        int to_read = (int)(buf_size - buf_fill);
        if (to_read > (int)remaining) to_read = (int)remaining;

        int received = httpd_req_recv(req, buf + buf_fill, to_read);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++consecutive_timeouts > max_consecutive_timeouts) {
                ESP_LOGE(TAG, "Upload de %s abortado: timeout demais seguidos", display_name);
                snprintf(fail_detail, sizeof(fail_detail), "timeout no recv (%d timeouts seguidos)", consecutive_timeouts);
                ok = false;
                break;
            }
            continue;
        }
        consecutive_timeouts = 0;
        if (received <= 0) {
            ESP_LOGE(TAG, "httpd_req_recv erro: %d", received);
            snprintf(fail_detail, sizeof(fail_detail), "httpd_req_recv erro ret=%d", received);
            ok = false;
            break;
        }

        buf_fill += (size_t)received;
        remaining -= (size_t)received;

        // Grava em blocos alinhados de 32 KB para velocidade maxima no SDMMC
        if (buf_fill >= buf_size || remaining == 0) {
            size_t written = fwrite(buf, 1, buf_fill, fp);
            if (written != buf_fill) {
                ESP_LOGE(TAG, "fwrite no SD falhou: esperado=%d, gravado=%d (errno=%d: %s)",
                         (int)buf_fill, (int)written, errno, strerror(errno));
                snprintf(fail_detail, sizeof(fail_detail), "fwrite SD erro: esperado=%d ret=%d (errno=%d: %s)",
                         (int)buf_fill, (int)written, errno, strerror(errno));
                ok = false;
                break;
            }
            buf_fill = 0;
        }

        // Throttle progress_update: atualiza a cada 150ms para eliminar disputa de mutex
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if (now_ms - last_prog_update_ms >= 150 || remaining == 0) {
            last_prog_update_ms = now_ms;
            progress_update(content_len - remaining);
        }
    }
    free(buf);
    fclose(fp);

    if (!ok) {
        remove(abs_path);
        ESP_LOGE(TAG, "Envio de %s interrompido: %s", display_name, fail_detail);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, fail_detail);
        progress_end();
        return ESP_FAIL;
    }

    progress_finish_file(content_len);
    if (idx >= count) progress_end();

    s_files_received++;
    ESP_LOGI(TAG, "Recebido: %s (%d/%d)", rel_path, idx, count);
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t api_download_get_handler(httpd_req_t *req)
{
    char query[256] = {0};
    char path_enc[192] = {0};
    char rel_path[192] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", path_enc, sizeof(path_enc)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "faltou o parametro 'path'");
        return ESP_FAIL;
    }
    url_decode(rel_path, path_enc, sizeof(rel_path));

    char abs_path[400];
    if (!build_abs_path(rel_path, abs_path, sizeof(abs_path)) || rel_path[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "caminho invalido");
        return ESP_FAIL;
    }

    if (s_progress_active) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "Upload em andamento no SD", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    FILE *fp = fopen(abs_path, "rb");
    if (!fp) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Arquivo nao encontrado");
        return ESP_FAIL;
    }

    fseek(fp, 0, SEEK_END);
    size_t fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (strstr(abs_path, ".flac") || strstr(abs_path, ".FLAC")) httpd_resp_set_type(req, "audio/flac");
    else if (strstr(abs_path, ".wav") || strstr(abs_path, ".WAV")) httpd_resp_set_type(req, "audio/wav");
    else if (strstr(abs_path, ".m4a") || strstr(abs_path, ".M4A")) httpd_resp_set_type(req, "audio/mp4");
    else httpd_resp_set_type(req, "audio/mpeg");

    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");

    if (fsize == 0) {
        httpd_resp_send(req, NULL, 0);
        fclose(fp);
        return ESP_OK;
    }

    size_t start = 0;
    size_t end = fsize - 1;
    bool is_range = false;

    char range_hdr[64];
    if (httpd_req_get_hdr_value_str(req, "Range", range_hdr, sizeof(range_hdr)) == ESP_OK) {
        if (sscanf(range_hdr, "bytes=%zu-%zu", &start, &end) == 2) {
            is_range = true;
        } else if (sscanf(range_hdr, "bytes=%zu-", &start) == 1) {
            is_range = true;
            end = fsize - 1;
        }
    }

    if (is_range && (start > end || start >= fsize)) {
        httpd_resp_set_status(req, "416 Range Not Satisfiable");
        httpd_resp_send(req, "Range invalido", HTTPD_RESP_USE_STRLEN);
        fclose(fp);
        return ESP_FAIL;
    }
    if (end >= fsize) end = fsize - 1;

    size_t content_length = end - start + 1;

    if (is_range) {
        httpd_resp_set_status(req, "206 Partial Content");
        char crange_str[128];
        snprintf(crange_str, sizeof(crange_str), "bytes %zu-%zu/%zu", start, end, fsize);
        httpd_resp_set_hdr(req, "Content-Range", crange_str);
    }

    fseek(fp, start, SEEK_SET);

    char *chunk = malloc(4096);
    if (!chunk) {
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
        size_t to_read = (remaining < 4096) ? remaining : 4096;
        size_t read_bytes = fread(chunk, 1, to_read, fp);
        if (read_bytes <= 0) break;
        
        if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
            free(chunk);
            fclose(fp);
            return ESP_FAIL;
        }
        remaining -= read_bytes;
    }

    if (remaining > 0) {
        free(chunk);
        fclose(fp);
        return ESP_FAIL;
    }

    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send_chunk(req, NULL, 0); 
    free(chunk);
    fclose(fp);
    return ESP_OK;
}

static esp_err_t api_delete_handler(httpd_req_t *req)
{
    char query[256] = {0};
    char path_enc[192] = {0};
    char rel_path[192] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", path_enc, sizeof(path_enc)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "faltou o parametro 'path'");
        return ESP_FAIL;
    }
    url_decode(rel_path, path_enc, sizeof(rel_path));

    char abs_path[400];
    if (!build_abs_path(rel_path, abs_path, sizeof(abs_path)) || rel_path[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "caminho invalido");
        return ESP_FAIL;
    }

    if (recursive_delete(abs_path) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "falha ao apagar");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Apagado: %s", rel_path);
    httpd_resp_send(req, NULL, 0);
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
            strncpy(s_known[0].ssid, ssid, sizeof(s_known[0].ssid) - 1);
            strncpy(s_known[0].pass, pass, sizeof(s_known[0].pass) - 1);
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

    char json[64 + WIFI_KNOWN_MAX * (WIFI_SSID_MAX_LEN + 16)];
    int off = snprintf(json, sizeof(json), "{\"networks\":[");
    for (int i = 0; i < s_known_count && off < (int)sizeof(json) - 1; i++) {
        off += snprintf(json + off, sizeof(json) - off, "%s{\"ssid\":\"%s\"}",
                         i > 0 ? "," : "", s_known[i].ssid);
    }
    snprintf(json + off, sizeof(json) - off, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
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
    char buf[512];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) return ESP_FAIL;
    buf[received] = '\0';

    char rel_path[256] = {0};
    json_extract_field(buf, "path", rel_path, sizeof(rel_path));
    
    char abs_path[400];
    build_abs_path(rel_path, abs_path, sizeof(abs_path));
    
    ESP_LOGI(TAG, "DJ Play: %s", abs_path);
    // audio_player_play_local_file(abs_path);
    
    httpd_resp_send(req, "{\"status\":\"ok\"}", -1);
    return ESP_OK;
}

static esp_err_t api_webradio_play_handler(httpd_req_t *req)
{
    char buf[512];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "corpo vazio");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    char url[256] = {0};
    json_extract_field(buf, "url", url, sizeof(url));
    if (url[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "url vazia");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Web Radio play solicitada: %s", url);
    audio_player_play_url(url);
    
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
    
    player_eq_config_t cfg;
    audio_player_get_eq_config(&cfg);
    
    char *buf = (char*)malloc(4096);
    if (!buf) return ESP_FAIL;
    
    int offset = snprintf(buf, 4096, "{\"enabled\":%s,\"active_preset_idx\":%d,\"presets\":[", cfg.enabled ? "true" : "false", cfg.active_preset_idx);
    
    for (int p = 0; p < PLAYER_EQ_MAX_PRESETS; p++) {
        offset += snprintf(buf + offset, 4096 - offset, "{\"name\":\"%s\",\"overall_gain\":%.1f,\"band_gains\":[", cfg.presets[p].name, cfg.presets[p].overall_gain);
        for (int b = 0; b < PLAYER_EQ_BANDS; b++) {
            offset += snprintf(buf + offset, 4096 - offset, "%.1f%s", cfg.presets[p].band_gains[b], (b == PLAYER_EQ_BANDS - 1) ? "" : ",");
        }
        offset += snprintf(buf + offset, 4096 - offset, "]}%s", (p == PLAYER_EQ_MAX_PRESETS - 1) ? "" : ",");
    }
    snprintf(buf + offset, 4096 - offset, "]}");
    
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);
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
    char *buf = (char*)malloc(4096);
    if(!buf) return ESP_FAIL;
    int ret = httpd_req_recv(req, buf, req->content_len < 4095 ? req->content_len : 4095);
    if (ret <= 0) { free(buf); return ESP_FAIL; }
    buf[ret] = '\0';
    
    player_eq_config_t cfg;
    audio_player_get_eq_config(&cfg);
    
    const char *p = strstr(buf, "\"enabled\"");
    if(p) {
        p += 9;
        while (*p && (*p == ' ' || *p == '\t' || *p == ':')) p++;
        cfg.enabled = (*p == 't' || *p == 'T' || *p == '1');
    }
    
    p = strstr(buf, "\"active_preset_idx\"");
    if(p) {
        p += 19;
        while (*p && (*p == ' ' || *p == '\t' || *p == ':')) p++;
        cfg.active_preset_idx = atoi(p);
    }
    
    p = strstr(buf, "\"presets\"");
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
                    strncpy(cfg.presets[i].name, p, l);
                    cfg.presets[i].name[l] = '\0';
                }
            }
            p = strstr(p, "\"overall_gain\"");
            if(p) {
                p += 14;
                while (*p && (*p == ' ' || *p == '\t' || *p == ':')) p++;
                cfg.presets[i].overall_gain = atof(p);
            }
            
            p = strstr(p, "\"band_gains\"");
            if(p) {
                p += 12;
                while (*p && (*p != '[')) p++;
                if (*p == '[') {
                    p++;
                    for(int b=0; b<10; b++) {
                        while (*p && (*p == ' ' || *p == '\t')) p++;
                        cfg.presets[i].band_gains[b] = atof(p);
                        p = strchr(p, ',');
                        if(!p) break;
                        p++;
                    }
                }
            }
        }
    }
    
    free(buf);
    audio_player_set_eq_config(&cfg);
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_rename_post_handler(httpd_req_t *req) {
    char buf[512];
    int ret = httpd_req_recv(req, buf, req->content_len < sizeof(buf) - 1 ? req->content_len : sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';
    
    char old_path[128] = {0}, new_path[128] = {0};
    get_json_string(buf, "old_path", old_path, sizeof(old_path));
    get_json_string(buf, "new_path", new_path, sizeof(new_path));
    
    char old_abs[300], new_abs[300];
    if (!build_abs_path(old_path, old_abs, sizeof(old_abs)) ||
        !build_abs_path(new_path, new_abs, sizeof(new_abs))) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"error\":\"caminho invalido\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    // Proteção: não permitir mover/renomear item para dentro dele mesmo ou subpasta dele
    size_t old_len = strlen(old_abs);
    if (strncmp(old_abs, new_abs, old_len) == 0 &&
        (new_abs[old_len] == '/' || new_abs[old_len] == '\0')) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"error\":\"destino nao pode ser o proprio item ou uma subpasta dele\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    mkdir_p_for_file(new_abs);
    if (rename(old_abs, new_abs) != 0) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "{\"error\":\"falha ao renomear\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_move_post_handler(httpd_req_t *req) {
    return api_rename_post_handler(req);
}

static esp_err_t api_copy_post_handler(httpd_req_t *req) {
    char buf[512];
    int ret = httpd_req_recv(req, buf, req->content_len < sizeof(buf) - 1 ? req->content_len : sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';
    
    char src_path[128] = {0}, dest_path[128] = {0};
    get_json_string(buf, "src_path", src_path, sizeof(src_path));
    get_json_string(buf, "dest_path", dest_path, sizeof(dest_path));
    
    char src_abs[300], dest_abs[300];
    if (!build_abs_path(src_path, src_abs, sizeof(src_abs)) ||
        !build_abs_path(dest_path, dest_abs, sizeof(dest_abs))) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"error\":\"caminho invalido\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    mkdir_p_for_file(dest_abs);
    FILE *fs = fopen(src_abs, "rb");
    if (!fs) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "{\"error\":\"origem nao encontrada\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    FILE *fd = fopen(dest_abs, "wb");
    if (!fd) {
        fclose(fs);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "{\"error\":\"falha ao criar destino\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    char *cpy_buf = (char*)malloc(4096);
    if (!cpy_buf) {
        fclose(fd);
        fclose(fs);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "{\"error\":\"sem memoria\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    size_t r;
    while ((r = fread(cpy_buf, 1, 4096, fs)) > 0) {
        if (fwrite(cpy_buf, 1, r, fd) != r) break;
    }
    free(cpy_buf);
    fclose(fd);
    fclose(fs);

    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_mkdir_post_handler(httpd_req_t *req) {
    char buf[256];
    int ret = httpd_req_recv(req, buf, req->content_len < sizeof(buf) - 1 ? req->content_len : sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';
    
    char dir_path[128] = {0};
    get_json_string(buf, "path", dir_path, sizeof(dir_path));
    if (dir_path[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"error\":\"path vazio\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    
    char abs_path[300];
    if (!build_abs_path(dir_path, abs_path, sizeof(abs_path))) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"error\":\"caminho invalido\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    char abs_file[320];
    snprintf(abs_file, sizeof(abs_file), "%s/", abs_path);
    mkdir_p_for_file(abs_file);

    struct stat st;
    if (stat(abs_path, &st) != 0) {
        if (mkdir(abs_path, 0777) != 0 && errno != EEXIST) {
            char err_msg[128];
            snprintf(err_msg, sizeof(err_msg), "{\"error\":\"falha ao criar pasta (%s)\"}", strerror(errno));
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_send(req, err_msg, HTTPD_RESP_USE_STRLEN);
            return ESP_FAIL;
        }
    }

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

    char json[384];
    snprintf(json, sizeof(json),
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

    // Buffer de 4 KB alocado obrigatoriamente na DRAM interna (obrigatorio para operacoes de Flash/OTA no ESP32)
    char *ota_write_data = (char *)heap_caps_malloc(OTA_BUFF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    if (!ota_write_data) {
        ota_write_data = (char *)malloc(OTA_BUFF_SIZE);
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

    // Receber primeiro chunk de pelo menos min_hdr (288 bytes) para validar cabecalhos antes de apagar flash
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
        free(ota_write_data);
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
        free(ota_write_data);
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
        free(ota_write_data);
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
        free(ota_write_data);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Falha ao inicializar particao flash OTA\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    // Gravar o primeiro chunk ja lido
    err = esp_ota_write(ota_handle, (const void *)ota_write_data, first_chunk_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write falhou no primeiro bloco: %s", esp_err_to_name(err));
        esp_ota_abort(ota_handle);
        s_ota_in_progress = false;
        s_ui_state = WIFI_UI_CONNECTED;
        s_ota_status[0] = '\0';
        free(ota_write_data);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Falha ao gravar bloco inicial na flash\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    int bytes_written = first_chunk_len;
    s_ota_pct = (int)((bytes_written * 100LL) / total_len);
    int block_cnt = 0;
    bool write_failed = false;
    int consecutive_timeouts = 0;

    while (bytes_written < total_len) {
        int to_read = (total_len - bytes_written < OTA_BUFF_SIZE) ? (total_len - bytes_written) : OTA_BUFF_SIZE;
        int r = httpd_req_recv(req, ota_write_data, to_read);
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
        if (r == 0) break;
        consecutive_timeouts = 0;

        err = esp_ota_write(ota_handle, (const void *)ota_write_data, r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write falhou (%d bytes): %s", r, esp_err_to_name(err));
            write_failed = true;
            break;
        }

        bytes_written += r;
        s_ota_pct = (int)((bytes_written * 100LL) / total_len);

        block_cnt++;
        if (block_cnt % 4 == 0) {
            vTaskDelay(1);
        }
    }

    free(ota_write_data);

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
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end falhou: %s", esp_err_to_name(err));
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
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr_chunk(req, "{\"podcasts\":[");

    char base_dir[128];
    snprintf(base_dir, sizeof(base_dir), "%s/Podcasts", SD_MOUNT_POINT);

    DIR *d_base = opendir(base_dir);
    if (!d_base) {
        httpd_resp_sendstr_chunk(req, "]}");
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_OK;
    }

    struct dirent *prog_entry;
    bool first = true;
    char chunk[384];

    while ((prog_entry = readdir(d_base)) != NULL) {
        if (prog_entry->d_name[0] == '.') continue;
        if (prog_entry->d_type != DT_DIR) continue;

        char prog_path[512];
        snprintf(prog_path, sizeof(prog_path), "%s/%s", base_dir, prog_entry->d_name);
        DIR *d_prog = opendir(prog_path);
        if (!d_prog) continue;

        struct dirent *ep_entry;
        while ((ep_entry = readdir(d_prog)) != NULL) {
            if (ep_entry->d_name[0] == '.') continue;
            if (ep_entry->d_type == DT_DIR) continue;
            if (strstr(ep_entry->d_name, ".part") != NULL) continue;

            char file_path[768];
            snprintf(file_path, sizeof(file_path), "%s/%s", prog_path, ep_entry->d_name);
            struct stat st;
            long size = 0;
            if (stat(file_path, &st) == 0) {
                size = (long)st.st_size;
            }

            char prog_esc[96];
            char name_esc[160];
            json_escape(prog_entry->d_name, prog_esc, sizeof(prog_esc));
            json_escape(ep_entry->d_name, name_esc, sizeof(name_esc));

            snprintf(chunk, sizeof(chunk), "%s{\"program\":\"%s\",\"filename\":\"%s\",\"size\":%ld}",
                     first ? "" : ",", prog_esc, name_esc, size);
            first = false;
            httpd_resp_sendstr_chunk(req, chunk);
        }
        closedir(d_prog);
    }
    closedir(d_base);

    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t start_httpd(void)
{
    if (s_httpd) return ESP_OK;

    static uint16_t s_ctrl_port = 32768;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    config.task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT; // Stack na DRAM interna (essencial para que chamadas flash/OTA nao crashem com cache desativado)
    config.core_id = 1; // Roda no Core 1 com 240 MHz livres enquanto o player esta pausado!
    config.ctrl_port = s_ctrl_port++;
    if (s_ctrl_port > 32800) s_ctrl_port = 32768;
    config.max_uri_handlers = 32;
    config.max_open_sockets = 7;
    config.backlog_conn = 5;
    config.lru_purge_enable = true;
    config.keep_alive_enable = true;
    config.keep_alive_idle = 5;
    config.keep_alive_interval = 5;
    config.keep_alive_count = 3;
    config.send_wait_timeout = 15;
    config.recv_wait_timeout = 15;

    ESP_LOGI(TAG, "Iniciando servidor HTTP (modo=%s, ip=%s, ctrl_port=%d)...",
             s_mode == WIFI_TRANSFER_MODE_STA ? "STA" : "AP", s_status, config.ctrl_port);

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

    esp_err_t err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao subir o servidor HTTP: %s", esp_err_to_name(err));
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

    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, http_404_error_handler);

    return ESP_OK;
}


// --- WiFi ------------------------------------------------------------------

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
            if (s_active && (s_mode == WIFI_TRANSFER_MODE_STA || s_mode == WIFI_TRANSFER_MODE_APSTA) && !s_got_ip) {
                if (s_sta_retry < WIFI_STA_MAX_RETRY) {
                    s_sta_retry++;
                    esp_wifi_connect();
                } else if (s_candidate_idx + 1 < s_candidate_count) {
                    // Essa rede esgotou as tentativas - passa pra' proxima
                    // rede conhecida antes de desistir e cair pro hotspot.
                    s_candidate_idx++;
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

        ESP_LOGI(TAG, "===> CONECTADO NA REDE WIFI LOCAL! <===");
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

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.static_rx_buf_num = 4;
    cfg.dynamic_rx_buf_num = 16;
    cfg.dynamic_tx_buf_num = 16;
    cfg.mgmt_sbuf_num = 16;
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;

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

    s_mode = mode;
    s_files_received = 0;
    s_exit_requested = false;
    s_got_ip = false;
    s_ap_ready = false;
    s_sta_failed = false;
    s_sta_retry = 0;
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
        ap_config.ap.channel = 1;
        ap_config.ap.beacon_interval = 100;
        ap_config.ap.max_connection = 4;
        ap_config.ap.authmode = strlen(CONFIG_WIFI_AP_PASSWORD) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

        ESP_LOGI(TAG, "===> MODO HOTSPOT ATIVO <===");
        ESP_LOGI(TAG, "Rede gerada pelo MPS3: SSID='%s' | Senha='%s' | Canal=1", CONFIG_WIFI_AP_SSID, CONFIG_WIFI_AP_PASSWORD);
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
    // Potencia calibrada em ~18 dBm (72) para conexao estavel e forte com o roteador:
    esp_wifi_set_max_tx_power(72);

    if (mode == WIFI_TRANSFER_MODE_APSTA) {
        esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20);
        esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);
    } else {
        wifi_interface_t ifx = (mode == WIFI_TRANSFER_MODE_STA) ? WIFI_IF_STA : WIFI_IF_AP;
        esp_wifi_set_bandwidth(ifx, WIFI_BW20);
    }

    ESP_LOGI(TAG, "Modo WiFi (%s) iniciado",
             mode == WIFI_TRANSFER_MODE_APSTA ? "AP+STA simultaneo" :
             (mode == WIFI_TRANSFER_MODE_STA ? "estacao" : "hotspot"));
    return ESP_OK;
}

esp_err_t wifi_transfer_enter_auto(void)
{
    // O modo automatico inicia em APSTA para garantir que o Hotspot "mps3-player"
    // (192.168.4.1) fique SEMPRE disponivel imediatamente para conexao direta do PC/celular,
    // e simultaneamente tenta conectar a rede Wi-Fi local cadastrada se houver.
    int candidates = build_sta_candidates();
    if (candidates > 0) {
        return wifi_transfer_enter(WIFI_TRANSFER_MODE_APSTA);
    }
    ESP_LOGI(TAG, "Nenhuma rede STA disponivel, iniciando em modo AP...");
    return wifi_transfer_enter(WIFI_TRANSFER_MODE_AP);
}

bool wifi_transfer_poll(void)
{
    if (!s_active) return false;

    update_ui_state();

    static uint32_t s_last_httpd_attempt_ms = 0;
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

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
            wifi_config.ap.max_connection = 4;
            wifi_config.ap.authmode = strlen(CONFIG_WIFI_AP_PASSWORD) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

            ESP_LOGI(TAG, "===> MODO HOTSPOT ATIVO (Fallback) <===");
            ESP_LOGI(TAG, "Rede gerada pelo MPS3: SSID='%s' | Senha='%s'", CONFIG_WIFI_AP_SSID, CONFIG_WIFI_AP_PASSWORD);
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
            ESP_LOGI(TAG, "SD Card remontado com sucesso apos WiFi");
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
        strncpy(out, s_status, out_len - 1);
        out[out_len - 1] = '\0';
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
    wifi_transfer_progress_t prog;
    wifi_transfer_get_progress(&prog);

    char status[64];
    int files = 0;
    wifi_transfer_get_status(status, sizeof(status), &files);

    // Extrai IP do status
    char ip[16] = "0.0.0.0";
    if (strchr(status, '.') != NULL) {
        strncpy(ip, status, sizeof(ip)-1);
        ip[sizeof(ip)-1] = '\0';
    }

    int rssi = 0;
    if (s_mode == WIFI_TRANSFER_MODE_STA || s_mode == WIFI_TRANSFER_MODE_APSTA) {
        esp_wifi_sta_get_rssi(&rssi);
    } else {
        rssi = -1;
    }

    bool dns_active = (s_dns_socket >= 0);

    switch (s_ui_state) {
        case WIFI_UI_IDLE:
            if (s_mode == WIFI_TRANSFER_MODE_AP || s_mode == WIFI_TRANSFER_MODE_APSTA) {
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
            // Obter RSSI
            if (s_mode == WIFI_TRANSFER_MODE_STA || s_mode == WIFI_TRANSFER_MODE_APSTA) {
                esp_wifi_sta_get_rssi(&rssi);
            } else {
                rssi = -1;
            }
            oled_display_show_wifi_transfer(
                prog.filename,
                prog.file_pct,
                prog.overall_pct >= 0 ? prog.overall_pct : 0,
                prog.idx,
                prog.count,
                prog.kbps,
                prog.eta_sec,
                prog.total_eta_sec,
                rssi   // <-- passando RSSI
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





