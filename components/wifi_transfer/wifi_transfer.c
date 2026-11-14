#include "wifi_transfer.h"
#include "sd_card.h"
#include "audio_player.h"
#include "oled_display.h"
#include "qrcode.h"
#include "menu.h"
#include "i2s_output.h"

#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_partition.h"
#include "esp_timer.h"

#include "eq.h"
#include "podcast_sync.h"
#include "podcast_history.h"
#include "psa/crypto.h"
#include "esp_vfs_fat.h"
#include <errno.h>
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

static inline void set_cors_pna_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Private-Network", "true");
    httpd_resp_set_hdr(req, "Access-Control-Expose-Headers", "X-Content-SHA256, X-Upload-Stats, X-Download-Stats, Content-Range, Content-Length");
}

static esp_err_t api_options_preflight_handler(httpd_req_t *req)
{
    set_cors_pna_headers(req);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Range, Authorization, X-Requested-With, X-API-Key, X-Expected-SHA256");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t api_now_playing_get_handler(httpd_req_t *req)
{
    set_cors_pna_headers(req);
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
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");

    esp_err_t ret = httpd_resp_send(req, (const char *)index_html_gz, index_html_gz_size);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Falha ao enviar HTML gzip (%d bytes): %s", (int)index_html_gz_size, esp_err_to_name(ret));
    }
    return ret;
}



static char s_status[64] = "";
static bool s_bw_is_ht40 = false;

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

    char *json = (char *)http_scratch_alloc(320);
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    const char *ip_str = (s_status[0] >= '0' && s_status[0] <= '9')
                         ? s_status
                         : ((s_mode == WIFI_TRANSFER_MODE_AP || s_mode == WIFI_TRANSFER_MODE_APSTA) ? "192.168.4.1" : "");
    const char *wmode = (s_mode == WIFI_TRANSFER_MODE_STA) ? "STA" : "AP";
    snprintf(json, 320,
             "{\"total\":%llu,\"free\":%llu,\"battery_mv\":%d,\"battery_percent\":%d,\"battery_time_left\":%d,\"sd_status\":\"%s\",\"ip\":\"%s\",\"wifi_mode\":\"%s\",\"bw\":\"%s\",\"dma_free\":%u,\"dma_max\":%u}",
             (unsigned long long)total_bytes, (unsigned long long)free_bytes, mv, percent, time_left,
             s_progress_active ? "writing" : "free", ip_str, wmode, s_bw_is_ht40 ? "HT40" : "HT20",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));

    httpd_resp_set_type(req, "application/json");
    set_cors_pna_headers(req);
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t api_space_get_handler(httpd_req_t *req)
{
    set_cors_pna_headers(req);
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

static char s_last_upload_stats_json[1280] = "{}\0";
static char s_last_download_stats_json[1024] = "{}\0";

static esp_err_t api_upload_stats_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    set_cors_pna_headers(req);
    httpd_resp_sendstr(req, s_last_upload_stats_json);
    return ESP_OK;
}

static esp_err_t api_download_stats_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    set_cors_pna_headers(req);
    httpd_resp_sendstr(req, s_last_download_stats_json);
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
static volatile int s_active_transfers = 0;
static volatile int64_t s_last_transfer_end_us = 0;
static int s_sd_clock_mode = 3; // 0=20MHz Fixo, 1=Hibrido(20->40MHz), 2=40MHz Continuo, 3=Auto-Channel
static uint8_t *s_bounce_persist = NULL;       // bounce DMA persistente em SRAM interna
static size_t s_bounce_persist_sz = 0;

// Controle de fallback automático STA -> AP
static bool s_auto_fallback = false;

static int s_dns_socket = -1;
static TaskHandle_t s_dns_task_handle = NULL;

// --- Progresso -----------------------------------------------------------
static SemaphoreHandle_t s_progress_mutex = NULL;
static char s_progress_name[128] = "";
static volatile size_t s_progress_file_done = 0;
static volatile size_t s_progress_file_total = 0;
static volatile uint32_t s_progress_file_start_ms = 0;
static volatile int s_progress_idx = 1;
static volatile int s_progress_count = 1;
static volatile long long s_progress_batch_total = 0;
static volatile long long s_progress_batch_done_before = 0;

typedef enum {
    WIFI_UI_IDLE,
    WIFI_UI_CONNECTED,
    WIFI_UI_TRANSFERRING,
    WIFI_UI_STREAMING,
    WIFI_UI_DONE,
    WIFI_UI_OTA_UPDATING,
    WIFI_UI_OTA_FINISHED
} wifi_ui_state_t;

static volatile wifi_ui_state_t s_ui_state = WIFI_UI_IDLE;
static uint32_t s_done_show_until_ms = 0;
static wifi_ui_state_t s_wifi_last_drawn_state = (wifi_ui_state_t)-1;
static uint32_t s_wifi_last_draw_ms = 0;

static volatile bool s_stream_active = false;
static uint32_t s_stream_expire_ms = 0;
static char s_stream_filename[64] = {0};
static size_t s_stream_file_size = 0;
static size_t s_stream_file_done = 0;
static float s_stream_kbps = 0.0f;

static volatile bool s_ota_in_progress = false;
static volatile int s_ota_pct = 0;
static char s_ota_status[32] = {0};

static void transfer_heap_diag(const char *tag)
{
    ESP_LOGI(TAG, "[DIAG %s] DRAM livre=%u (min=%u, maior=%u) | DMA maior=%u | PSRAM livre=%u",
             tag,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static inline void transfer_boost_begin(void)
{
    __atomic_add_fetch(&s_active_transfers, 1, __ATOMIC_SEQ_CST);
    s_ui_state = WIFI_UI_TRANSFERRING;
    esp_wifi_set_max_tx_power(76); /* 19.0 dBm: alta velocidade calibrada sem brownout */
    transfer_heap_diag("boost_begin");
}

static inline void transfer_boost_end(void)
{
    transfer_heap_diag("boost_end");
    s_last_transfer_end_us = esp_timer_get_time();
    int rem = __atomic_sub_fetch(&s_active_transfers, 1, __ATOMIC_SEQ_CST);
    if (rem <= 0) {
        __atomic_store_n(&s_active_transfers, 0, __ATOMIC_SEQ_CST);
    }
}

bool wifi_transfer_is_boost_active(void)
{
    if (!s_active) return false;
    if (__atomic_load_n(&s_active_transfers, __ATOMIC_SEQ_CST) > 0) return true;
    int64_t last_end = s_last_transfer_end_us;
    if (last_end > 0 && (esp_timer_get_time() - last_end) < 1500000LL) return true;
    if (s_ui_state == WIFI_UI_TRANSFERRING && s_progress_active) return true;
    return false;
}

void wifi_transfer_set_sd_clock_mode(int mode)
{
    if (mode < 0) mode = 0;
    if (mode > 3) mode = 3;
    s_sd_clock_mode = mode;
}

int wifi_transfer_get_sd_clock_mode(void)
{
    return s_sd_clock_mode;
}

static int s_transfer_display_fps = 4;
static uint32_t s_transfer_display_interval_ms = 250;
static void wifi_menu_draw_status(void);

void wifi_transfer_set_display_fps(int fps)
{
    if (fps < 0) fps = 0;
    if (fps > 60) fps = 60;
    s_transfer_display_fps = fps;
    s_transfer_display_interval_ms = (fps > 0) ? (uint32_t)(1000 / fps) : 0;
    ESP_LOGI(TAG, "Transfer display FPS configurado para %d (%u ms)", fps, (unsigned)s_transfer_display_interval_ms);
}

int wifi_transfer_get_display_fps(void)
{
    return s_transfer_display_fps;
}

uint32_t wifi_transfer_get_display_interval_ms(void)
{
    return s_transfer_display_interval_ms;
}

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

    // Se estava em streaming, verifica a histerese de expiracao (3s)
    if (s_ui_state == WIFI_UI_STREAMING) {
        if (now > s_stream_expire_ms) {
            s_stream_active = false;
            s_ui_state = s_got_ip ? WIFI_UI_CONNECTED : WIFI_UI_IDLE;
        }
        return;
    }

    // Se estava em WIFI_UI_TRANSFERRING mas a transferencia encerrou:
    if (s_ui_state == WIFI_UI_TRANSFERRING && !s_progress_active) {
        if (s_files_received > 0 && now <= s_done_show_until_ms) {
            s_ui_state = WIFI_UI_DONE;
        } else {
            s_ui_state = s_got_ip ? WIFI_UI_CONNECTED : WIFI_UI_IDLE;
        }
        return;
    }

    // Se estiver no estado DONE e o tempo expirou, volta para CONNECTED
    if (s_ui_state == WIFI_UI_DONE && now > s_done_show_until_ms) {
        s_ui_state = WIFI_UI_CONNECTED;
        return;
    }

    // Se estiver transferindo, em streaming ou em atualizacao OTA, não muda
    if (s_ui_state == WIFI_UI_TRANSFERRING || s_ui_state == WIFI_UI_STREAMING ||
        s_ui_state == WIFI_UI_OTA_UPDATING || s_ui_state == WIFI_UI_OTA_FINISHED) return;

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
    if (batch_total > 0) {
        s_progress_batch_total = batch_total;
    } else if (idx <= 1 || s_progress_batch_total <= 0) {
        s_progress_batch_total = (long long)file_total * (count > 1 ? count : 1);
    }
    s_progress_active = true;
    xSemaphoreGive(s_progress_mutex);
    s_ui_state = WIFI_UI_TRANSFERRING;
}

static void progress_update(size_t done)
{
    if (done > s_progress_file_done) {
        s_progress_file_done = done;
    }
    if (done > s_progress_file_total) {
        s_progress_file_total = done;
    }
}

static void progress_finish_file(size_t file_total)
{
    if (!s_progress_mutex) return;
    xSemaphoreTake(s_progress_mutex, portMAX_DELAY);
    s_progress_batch_done_before += (long long)file_total;
    if (s_progress_batch_done_before > s_progress_batch_total) {
        s_progress_batch_total = s_progress_batch_done_before;
    }
    xSemaphoreGive(s_progress_mutex);
}

static void progress_end(void)
{
    if (!s_progress_mutex) return;
    xSemaphoreTake(s_progress_mutex, portMAX_DELAY);
    s_progress_active = false;
    s_status_last_calc_ms = 0; // invalida cache para que o proximo /api/status mostre o espaco atualizado
    if (s_stream_active) {
        s_ui_state = WIFI_UI_STREAMING;
    } else {
        s_done_show_until_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) + 1500;
        if (s_ui_state == WIFI_UI_TRANSFERRING) {
            s_ui_state = (s_files_received > 0) ? WIFI_UI_DONE : (s_got_ip ? WIFI_UI_CONNECTED : WIFI_UI_IDLE);
        }
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
        if (out->file_pct < 0) out->file_pct = 0;
        if (out->file_pct > 100) out->file_pct = 100;

        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        uint32_t elapsed_ms = now_ms - s_progress_file_start_ms;
        if (elapsed_ms > 100) {
            out->kbps = ((float)s_progress_file_done / 1024.0f) / ((float)elapsed_ms / 1000.0f);
            if (out->kbps > 0.05f && s_progress_file_done < s_progress_file_total) {
                size_t remaining_bytes = s_progress_file_total - s_progress_file_done;
                out->eta_sec = (int)((float)remaining_bytes / 1024.0f / out->kbps);
            }
        }

        if (s_progress_count > 1) {
            out->idx = s_progress_idx;
            out->count = s_progress_count;
            if (s_progress_batch_total > 0) {
                long long overall_done = s_progress_batch_done_before + (long long)s_progress_file_done;
                long long eff_batch_total = s_progress_batch_total;
                if (overall_done > eff_batch_total) eff_batch_total = overall_done;
                int raw_batch_pct = (int)((overall_done * 100) / eff_batch_total);
                int idx_based_pct = ((s_progress_idx - 1) * 100 + out->file_pct) / s_progress_count;
                // Usa media ponderada ou idx_based_pct se batch_total divergir da estimativa inicial
                out->overall_pct = (raw_batch_pct > 100) ? idx_based_pct : raw_batch_pct;
                if (out->overall_pct < 0) out->overall_pct = 0;
                if (out->overall_pct > 100) out->overall_pct = 100;

                if (out->kbps > 0.05f && eff_batch_total > overall_done) {
                    long long remaining_batch = eff_batch_total - overall_done;
                    out->total_eta_sec = (int)((float)remaining_batch / 1024.0f / out->kbps);
                } else {
                    out->total_eta_sec = -1;
                }
            } else {
                out->overall_pct = ((s_progress_idx - 1) * 100 + out->file_pct) / s_progress_count;
                if (out->overall_pct < 0) out->overall_pct = 0;
                if (out->overall_pct > 100) out->overall_pct = 100;
            }
        } else {
            out->idx = 1;
            out->count = 1;
            out->overall_pct = out->file_pct;
            out->total_eta_sec = out->eta_sec;
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

    // Higienizacao de caracteres especiais para FATFS aplicada abaixo
    for (size_t i = 0; dst[i] != '\0'; i++) {
        if ((unsigned char)dst[i] == 0xEF && (unsigned char)dst[i+1] == 0xBC && (unsigned char)dst[i+2] == 0x9F) {
            dst[i] = '-';
            memmove(&dst[i+1], &dst[i+3], strlen(&dst[i+3]) + 1);
        } else if (dst[i] == '?' || dst[i] == '*' || dst[i] == ':' || dst[i] == '<' || dst[i] == '>' || dst[i] == '|' || dst[i] == '"') {
            dst[i] = '-';
        }
    }
}

static void sanitize_fatfs_path_inplace(char *s)
{
    if (!s) return;
    for (size_t i = 0; s[i] != '\0'; i++) {
        if ((unsigned char)s[i] == 0xEF && (unsigned char)s[i+1] == 0xBC && (unsigned char)s[i+2] == 0x9F) {
            s[i] = '-';
            memmove(&s[i+1], &s[i+3], strlen(&s[i+3]) + 1);
        } else if (s[i] == '?' || s[i] == '*' || s[i] == ':' || s[i] == '<' || s[i] == '>' || s[i] == '|' || s[i] == '"') {
            s[i] = '-';
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
    if (n <= 0 || (size_t)n >= out_len) return false;
    size_t root_len = strlen(SD_MOUNT_POINT);
    if ((size_t)n > root_len + 1) {
        sanitize_fatfs_path_inplace(out + root_len + 1);
    }
    return true;
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

static uint16_t cp850_to_unicode(uint8_t c)
{
    if (c < 0x80) return c;
    static const uint16_t cp850_map[128] = {
        0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
        0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
        0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
        0x00FF, 0x00D6, 0x00DC, 0x00F8, 0x00A3, 0x00D8, 0x00D7, 0x0192,
        0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
        0x00BF, 0x00AE, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
        0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x00C1, 0x00C2, 0x00C0,
        0x00A9, 0x2563, 0x2551, 0x2557, 0x255D, 0x00A2, 0x00A5, 0x2510,
        0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x00E3, 0x00C3,
        0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x00A4,
        0x00F0, 0x00D0, 0x00CA, 0x00CB, 0x00C8, 0x0131, 0x00CD, 0x00CE,
        0x00CF, 0x2518, 0x250C, 0x2588, 0x2584, 0x00A6, 0x00CC, 0x2580,
        0x00D3, 0x00DF, 0x00D4, 0x00D2, 0x00F5, 0x00D5, 0x00B5, 0x00FE,
        0x00DE, 0x00DA, 0x00DB, 0x00D9, 0x00FD, 0x00DD, 0x00AF, 0x00B4,
        0x00AD, 0x00B1, 0x2017, 0x00BE, 0x00B6, 0x00A7, 0x00F7, 0x00B8,
        0x00B0, 0x00A8, 0x00B7, 0x00B9, 0x00B3, 0x00B2, 0x25A0, 0x00A0
    };
    return cp850_map[c - 0x80];
}

static void json_escape(const char *in, char *out, size_t out_len)
{
    if (!in || !out || out_len == 0) return;
    size_t o = 0;
    size_t len = strlen(in);
    for (size_t i = 0; i < len && o + 4 < out_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c == '\n') {
            if (o + 2 < out_len) { out[o++] = '\\'; out[o++] = 'n'; }
        } else if (c == '\r') {
            if (o + 2 < out_len) { out[o++] = '\\'; out[o++] = 'r'; }
        } else if (c == '\t') {
            if (o + 2 < out_len) { out[o++] = '\\'; out[o++] = 't'; }
        } else if (c < 0x20) {
            // pula caracteres de controle ascii
        } else if (c < 0x80) {
            out[o++] = (char)c;
        } else {
            // Verifica se e inicio de sequencia UTF-8 valida
            if ((c & 0xE0) == 0xC0 && (i + 1 < len) && ((unsigned char)in[i + 1] & 0xC0) == 0x80) {
                out[o++] = (char)c;
                out[o++] = in[++i];
            } else if ((c & 0xF0) == 0xE0 && (i + 2 < len) &&
                       ((unsigned char)in[i + 1] & 0xC0) == 0x80 &&
                       ((unsigned char)in[i + 2] & 0xC0) == 0x80) {
                out[o++] = (char)c;
                out[o++] = in[++i];
                out[o++] = in[++i];
            } else if ((c & 0xF8) == 0xF0 && (i + 3 < len) &&
                       ((unsigned char)in[i + 1] & 0xC0) == 0x80 &&
                       ((unsigned char)in[i + 2] & 0xC0) == 0x80 &&
                       ((unsigned char)in[i + 3] & 0xC0) == 0x80) {
                out[o++] = (char)c;
                out[o++] = in[++i];
                out[o++] = in[++i];
                out[o++] = in[++i];
            } else {
                // Byte isolado nao-UTF8 (ex: CP850 da FAT32): converte para UTF-8 valido
                uint16_t u = cp850_to_unicode(c);
                if (u < 0x80) {
                    out[o++] = (char)u;
                } else if (u < 0x800) {
                    out[o++] = (char)(0xC0 | (u >> 6));
                    out[o++] = (char)(0x80 | (u & 0x3F));
                } else {
                    out[o++] = (char)(0xE0 | (u >> 12));
                    out[o++] = (char)(0x80 | ((u >> 6) & 0x3F));
                    out[o++] = (char)(0x80 | (u & 0x3F));
                }
            }
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
    set_cors_pna_headers(req);
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

// --- Pipeline de Transferencia: httpd (Core 1) <-> RingBuffer PSRAM (4 MB) <-> worker persistente (Core 1, SD DMA) ---
#define UPLOAD_RINGBUF_SIZE          (4 * 1024 * 1024)  // "pulmao" elastico de 4 MB em PSRAM
#define UPLOAD_RINGBUF_MIN           (64 * 1024)
#define UPLOAD_RECV_BUF_SIZE         (32 * 1024)        // 32 KB (drena a janela TCP do lwIP de uma so vez)
#define UPLOAD_BOUNCE_SIZE           (32 * 1024)        // 64 setores = 32 KB (ou 64 KB) em SRAM DMA interna
#define UPLOAD_LOCK_TIMEOUT_MS       5000
#define UPLOAD_RING_SEND_TIMEOUT_MS  60000

static SemaphoreHandle_t s_upload_lock = NULL;
static volatile bool s_upload_io_busy = false;
static RingbufHandle_t s_rb_persist = NULL;    // RingBuffer de 4 MB em PSRAM reutilizado na sessao Wi-Fi
static size_t s_rb_persist_sz = 0;
static char *s_recv_buf_persist = NULL;        // Buffer TCP de 32 KB em PSRAM reutilizado
static size_t s_recv_buf_persist_sz = 0;

#define UPLOAD_WRITER_STACK_SIZE     16384     // 16 KB de pilha em PSRAM (0 bytes de DRAM interna)
static SemaphoreHandle_t s_wr_start_sem = NULL;
static SemaphoreHandle_t s_wr_done_sem = NULL;
static TaskHandle_t s_wr_task_handle = NULL;
static StaticTask_t *s_wr_tcb = NULL;
static StackType_t *s_wr_stack = NULL;

typedef enum {
    WORKER_JOB_NONE = 0,
    WORKER_JOB_UPLOAD = 1,
    WORKER_JOB_DOWNLOAD = 2,
} transfer_worker_job_t;

typedef struct {
    psa_hash_operation_t psa_op;
    bool hw_active;
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
    memset(ctx, 0, sizeof(*ctx));
    ctx->psa_op = psa_hash_operation_init();
    if (psa_hash_setup(&ctx->psa_op, PSA_ALG_SHA_256) == PSA_SUCCESS) {
        ctx->hw_active = true;
        return;
    }
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667UL; ctx->state[1] = 0xbb67ae85UL;
    ctx->state[2] = 0x3c6ef372UL; ctx->state[3] = 0xa54ff53aUL;
    ctx->state[4] = 0x510e527fUL; ctx->state[5] = 0x9b05688cUL;
    ctx->state[6] = 0x1f83d9abUL; ctx->state[7] = 0x5be0cd19UL;
}

static void mps3_sha256_update(mps3_sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
    if (ctx->hw_active) {
        if (len > 0 && data != NULL) {
            psa_hash_update(&ctx->psa_op, data, len);
        }
        return;
    }
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
    if (ctx->hw_active) {
        size_t hash_len = 0;
        if (psa_hash_finish(&ctx->psa_op, hash, 32, &hash_len) != PSA_SUCCESS) {
            psa_hash_abort(&ctx->psa_op);
            memset(hash, 0, 32);
        }
        ctx->hw_active = false;
        return;
    }
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
    volatile transfer_worker_job_t job_type;
    char abs_path[512];
    size_t total_size;            // Tamanho total esperado para pre-alocacao contigua f_expand
    FILE *fp;
    RingbufHandle_t rb;
    uint8_t *bounce;
    size_t bounce_size;
    bool calc_sha256;             // Calcula SHA-256 sob demanda apenas se X-Expected-SHA256 foi enviado
    bool boost_drain_40mhz;       // Modo Hibrido: promove SDMMC para 40 MHz ao iniciar a drenagem final
    volatile bool producer_done;  // httpd ja empurrou todos os bytes (ou desistiu)
    volatile bool abort;          // descartar o restante sem gravar
    volatile bool failed;         // erro de abertura ou escrita/leitura no SD
    volatile int open_errno;      // errno caso fopen falhe
    volatile size_t written;      // bytes realmente gravados/lidos no SD
    mps3_sha256_ctx_t sha_ctx;    // Contexto SHA-256 acelerado por hardware (PSA/ESP-SHA DMA)
    char sha256_hex[65];          // SHA-256 hex calculado on-the-fly durante a escrita no SD
    // Telemetria em microssegundos do consumidor de Upload (Core 1)
    int64_t open_us;
    int64_t mkdir_us;
    int64_t unlink_us;
    int64_t f_expand_us;
    int64_t fopen_us;
    int64_t wr_wait_us;
    int64_t memcpy_us;
    int64_t fwrite_us;
    int64_t fwrite_max_us;
    int64_t sha256_us;
    int64_t yield_us;
    int64_t fclose_us;
    uint32_t fwrite_calls;
    uint32_t fwrite_slow_cnt;
    // Telemetria em microssegundos do produtor de Download (Core 1)
    size_t dl_total_bytes;
    int64_t dl_sd_read_us;
    int64_t dl_fread_max_us;
    int64_t dl_ring_send_us;
    uint32_t dl_fread_calls;
    volatile bool dl_reader_done;
} upload_ctx_t;

static upload_ctx_t s_upload_ctx;

static void get_wifi_radio_telemetry(uint8_t *out_ch, int *out_rssi)
{
    uint8_t pri_ch = 0;
    wifi_second_chan_t sec_ch = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&pri_ch, &sec_ch) != ESP_OK) {
        pri_ch = 0;
    }
    int rssi = 0;
    if (s_mode == WIFI_TRANSFER_MODE_STA || s_mode == WIFI_TRANSFER_MODE_APSTA) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            rssi = ap_info.rssi;
            if (pri_ch == 0) pri_ch = ap_info.primary;
        }
    }
    if (out_ch) *out_ch = pri_ch;
    if (out_rssi) *out_rssi = rssi;
}

static bool apply_transfer_sd_clock_start(uint8_t wifi_ch, bool is_download)
{
    bool hybrid_drain = false;
    uint32_t target_khz = SDMMC_FREQ_DEFAULT;

    if (s_sd_clock_mode == 0) {
        target_khz = SDMMC_FREQ_DEFAULT;
        hybrid_drain = false;
    } else if (s_sd_clock_mode == 1) {
        target_khz = SDMMC_FREQ_DEFAULT;
        hybrid_drain = !is_download;
    } else if (s_sd_clock_mode == 2) {
        target_khz = SDMMC_FREQ_HIGHSPEED;
        hybrid_drain = false;
    } else {
        // Modo 3 (Auto-Channel): 40 MHz continuo fora da zona do 61º harmonico (canais 5..8), senao Hibrido
        if (wifi_ch > 0 && (wifi_ch <= 4 || wifi_ch >= 9)) {
            target_khz = SDMMC_FREQ_HIGHSPEED;
            hybrid_drain = false;
        } else {
            target_khz = SDMMC_FREQ_DEFAULT;
            hybrid_drain = !is_download;
        }
    }

    if (sd_card_get_frequency() != target_khz) {
        sd_card_set_frequency(target_khz);
    }
    return hybrid_drain;
}

static void upload_writer_run(upload_ctx_t *ctx)
{
    uint8_t *bounce = ctx->bounce;
    size_t chunk_sz = ctx->bounce_size;
    size_t bounce_filled = 0;
    bool drain_clk_applied = false;

    if (ctx->calc_sha256) {
        int64_t t_sha0 = esp_timer_get_time();
        mps3_sha256_init(&ctx->sha_ctx);
        ctx->sha256_us += esp_timer_get_time() - t_sha0;
    } else {
        ctx->sha256_hex[0] = '\0';
    }

    int64_t t_open0 = esp_timer_get_time();
    int64_t t_sub = t_open0;
    mkdir_p_for_file(ctx->abs_path);
    ctx->mkdir_us = esp_timer_get_time() - t_sub;

    t_sub = esp_timer_get_time();
    struct stat st_exist;
    if (stat(ctx->abs_path, &st_exist) == 0) {
        unlink(ctx->abs_path);
    }
    ctx->unlink_us = esp_timer_get_time() - t_sub;

    t_sub = esp_timer_get_time();
    bool contiguous = false;
    if (ctx->total_size >= (256 * 1024)) {
        if (esp_vfs_fat_create_contiguous_file("/sdcard", ctx->abs_path, (uint64_t)ctx->total_size, true) == ESP_OK) {
            contiguous = true;
        }
    }
    ctx->f_expand_us = esp_timer_get_time() - t_sub;

    t_sub = esp_timer_get_time();
    FILE *fp = fopen(ctx->abs_path, contiguous ? "r+b" : "wb");
    if (!fp && contiguous) {
        fp = fopen(ctx->abs_path, "wb");
    }
    if (!fp) {
        ctx->open_errno = errno;
        ctx->failed = true;
        ctx->fopen_us = esp_timer_get_time() - t_sub;
        ctx->open_us = esp_timer_get_time() - t_open0;
        ESP_LOGE(TAG, "[WRITER] Nao foi possivel criar %s (errno=%d: %s)",
                 ctx->abs_path, ctx->open_errno, strerror(ctx->open_errno));
    } else {
        setvbuf(fp, NULL, _IONBF, 0);
        ctx->fp = fp;
        ctx->fopen_us = esp_timer_get_time() - t_sub;
        ctx->open_us = esp_timer_get_time() - t_open0;
    }

    int64_t last_block_or_yield_us = esp_timer_get_time();

    while (true) {
        bool was_done = ctx->producer_done;
        if (was_done && ctx->boost_drain_40mhz && !drain_clk_applied) {
            if (sd_card_get_frequency() != SDMMC_FREQ_HIGHSPEED) {
                sd_card_set_frequency(SDMMC_FREQ_HIGHSPEED);
            }
            drain_clk_applied = true;
        }

        size_t needed = chunk_sz - bounce_filled;
        size_t got = 0;
        void *item = NULL;

        if (needed > 0) {
            int64_t twait0 = esp_timer_get_time();
            TickType_t wait_ticks = was_done ? 0 : pdMS_TO_TICKS(10);
            item = xRingbufferReceiveUpTo(ctx->rb, &got, wait_ticks, needed);
            int64_t waited_us = esp_timer_get_time() - twait0;
            ctx->wr_wait_us += waited_us;
            if (waited_us >= 1000 || !item) {
                last_block_or_yield_us = esp_timer_get_time();
            }
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

        // Escreve quando o bounce DMA encher (32/64 KB multi-bloco) OU no bloco final
        if (bounce_filled == chunk_sz || (bounce_filled > 0 && was_done && !item)) {
            if (was_done && ctx->boost_drain_40mhz && !drain_clk_applied) {
                if (sd_card_get_frequency() != SDMMC_FREQ_HIGHSPEED) {
                    sd_card_set_frequency(SDMMC_FREQ_HIGHSPEED);
                }
                drain_clk_applied = true;
            }
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
                    if (ctx->calc_sha256) {
                        int64_t t_sha0 = esp_timer_get_time();
                        mps3_sha256_update(&ctx->sha_ctx, bounce, bounce_filled);
                        ctx->sha256_us += esp_timer_get_time() - t_sha0;
                    }
                    ctx->written += bounce_filled;
                }
            }
            bounce_filled = 0;

            int64_t now_us = esp_timer_get_time();
            if ((now_us - last_block_or_yield_us) > 500000LL) {
                int64_t ty0 = now_us;
                vTaskDelay(1);
                int64_t ty1 = esp_timer_get_time();
                ctx->yield_us += (ty1 - ty0);
                last_block_or_yield_us = ty1;
            }
        }

        if (was_done && bounce_filled == 0 && !item) {
            break;
        }
    }

    if (ctx->boost_drain_40mhz && !drain_clk_applied) {
        if (sd_card_get_frequency() != SDMMC_FREQ_HIGHSPEED) {
            sd_card_set_frequency(SDMMC_FREQ_HIGHSPEED);
        }
        drain_clk_applied = true;
    }

    if (ctx->fp) {
        int64_t t_close0 = esp_timer_get_time();
        fclose(ctx->fp);
        ctx->fp = NULL;
        ctx->fclose_us = esp_timer_get_time() - t_close0;
    }

    if (ctx->calc_sha256) {
        int64_t t_sha0 = esp_timer_get_time();
        uint8_t digest[32] = {0};
        mps3_sha256_final(&ctx->sha_ctx, digest);
        for (int i = 0; i < 32; i++) {
            snprintf(&ctx->sha256_hex[i * 2], 3, "%02x", digest[i]);
        }
        ctx->sha256_hex[64] = '\0';
        ctx->sha256_us += esp_timer_get_time() - t_sha0;
    }
}

static void download_reader_run(upload_ctx_t *ctx)
{
    uint8_t *bounce = ctx->bounce;
    size_t chunk_sz = ctx->bounce_size;
    size_t remaining = ctx->dl_total_bytes;
    int64_t last_yield_us = esp_timer_get_time();

    while (remaining > 0 && !ctx->abort) {
        size_t to_read = (remaining < chunk_sz) ? remaining : chunk_sz;
        int64_t tr0 = esp_timer_get_time();
        size_t r = fread(bounce, 1, to_read, ctx->fp);
        int64_t trd = esp_timer_get_time() - tr0;
        ctx->dl_sd_read_us += trd;
        ctx->dl_fread_calls++;
        if (trd > ctx->dl_fread_max_us) ctx->dl_fread_max_us = trd;

        if (r == 0) {
            ESP_LOGW(TAG, "Download: fread retornou 0 (restante=%u, errno=%d, eof=%d)",
                     (unsigned)remaining, errno, feof(ctx->fp));
            ctx->failed = true;
            break;
        }
        remaining -= r;

        while (!ctx->abort) {
            int64_t ts0 = esp_timer_get_time();
            BaseType_t ok = xRingbufferSend(ctx->rb, bounce, r, pdMS_TO_TICKS(100));
            int64_t tsd = esp_timer_get_time() - ts0;
            ctx->dl_ring_send_us += tsd;
            if (tsd >= 1000) {
                last_yield_us = esp_timer_get_time();
            }
            if (ok == pdTRUE) {
                ctx->written += r;
                break;
            }
        }

        int64_t now_us = esp_timer_get_time();
        if ((now_us - last_yield_us) > 500000LL) {
            vTaskDelay(1);
            last_yield_us = esp_timer_get_time();
        }
    }

    ctx->dl_reader_done = true;
}

static void transfer_worker_task(void *arg)
{
    upload_ctx_t *ctx = (upload_ctx_t *)arg;
    while (true) {
        if (xSemaphoreTake(s_wr_start_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (ctx->job_type == WORKER_JOB_UPLOAD) {
            upload_writer_run(ctx);
        } else if (ctx->job_type == WORKER_JOB_DOWNLOAD) {
            download_reader_run(ctx);
        }
        ctx->job_type = WORKER_JOB_NONE;
        xSemaphoreGive(s_wr_done_sem);
    }
}

static bool ensure_transfer_pipeline_ready(void)
{
    if (!s_upload_lock) {
        s_upload_lock = xSemaphoreCreateMutex();
    }
    if (!s_wr_start_sem) {
        s_wr_start_sem = xSemaphoreCreateBinary();
    }
    if (!s_wr_done_sem) {
        s_wr_done_sem = xSemaphoreCreateBinary();
    }

    // Desaloca buffers DMA do I2S (~96 KB DRAM interna) temporariamente se estiverem ativos
    if (i2s_output_is_initialized()) {
        ESP_LOGI(TAG, "[PIPELINE] Desalocando buffers DMA do I2S (~96 KB) para acelerar SDMMC...");
        i2s_output_deinit();
    }

    // Bounce DMA persistente em SRAM interna: aloca se ainda nao existir
    if (!s_bounce_persist) {
        size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t max_dma = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        size_t target_sz = 0;

        if (max_dma >= 65536 && free_int >= (65536 + 45 * 1024)) {
            target_sz = 65536;
        } else if (max_dma >= 32768 && free_int >= (32768 + 24 * 1024)) {
            target_sz = 32768;
        } else if (max_dma >= 16384 && free_int >= (16384 + 12 * 1024)) {
            target_sz = 16384;
        } else if (max_dma >= 8192) {
            target_sz = 8192;
        } else if (max_dma >= 4096) {
            target_sz = 4096;
        }

        if (target_sz > 0) {
            uint8_t *new_b = (uint8_t *)heap_caps_aligned_alloc(16, target_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            if (new_b) {
                s_bounce_persist = new_b;
                s_bounce_persist_sz = target_sz;
                ESP_LOGI(TAG, "[PIPELINE] Bounce DMA persistente alocado: %u KB (DRAM livre=%u B, max_dma=%u B)",
                         (unsigned)(target_sz / 1024),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
            }
        }
    }

    // RingBuffer de ate 4 MB em PSRAM persistente durante a sessao Wi-Fi
    if (!s_rb_persist) {
        size_t try_rb = UPLOAD_RINGBUF_SIZE;
        size_t free_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        if (try_rb > free_psram && free_psram > UPLOAD_RINGBUF_MIN) {
            try_rb = (free_psram > 2 * 1024 * 1024) ? (2 * 1024 * 1024) : (free_psram / 2);
        }
        while (!s_rb_persist && try_rb >= UPLOAD_RINGBUF_MIN) {
            s_rb_persist = xRingbufferCreateWithCaps(try_rb, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (s_rb_persist) {
                s_rb_persist_sz = try_rb;
            } else {
                try_rb /= 2;
            }
        }
    } else {
        size_t dummy_sz = 0;
        void *dummy_item = NULL;
        while ((dummy_item = xRingbufferReceiveUpTo(s_rb_persist, &dummy_sz, 0, 65536)) != NULL) {
            vRingbufferReturnItem(s_rb_persist, dummy_item);
        }
    }

    // Buffer TCP de 32 KB em PSRAM persistente
    if (!s_recv_buf_persist) {
        s_recv_buf_persist_sz = UPLOAD_RECV_BUF_SIZE;
        s_recv_buf_persist = (char *)heap_caps_malloc(s_recv_buf_persist_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_recv_buf_persist) {
            s_recv_buf_persist_sz = 8192;
            s_recv_buf_persist = (char *)heap_caps_malloc(s_recv_buf_persist_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
    }

    // Task worker persistente no Core 1 (Prio 4, Stack 16 KB em PSRAM, TCB em DRAM)
    if (!s_wr_task_handle && s_wr_start_sem && s_wr_done_sem) {
        if (!s_wr_tcb) {
            s_wr_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (!s_wr_stack) {
            s_wr_stack = (StackType_t *)heap_caps_aligned_alloc(16, UPLOAD_WRITER_STACK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (s_wr_tcb && s_wr_stack) {
            s_wr_task_handle = xTaskCreateStaticPinnedToCore(
                transfer_worker_task,
                "up_wr",
                UPLOAD_WRITER_STACK_SIZE,
                &s_upload_ctx,
                4,
                s_wr_stack,
                s_wr_tcb,
                1
            );
        }
    }

    return (s_bounce_persist != NULL && s_rb_persist != NULL && s_recv_buf_persist != NULL &&
            s_wr_task_handle != NULL && s_wr_start_sem != NULL && s_wr_done_sem != NULL);
}

static void teardown_transfer_pipeline(void)
{
    if (s_wr_task_handle) {
        vTaskDelete(s_wr_task_handle);
        s_wr_task_handle = NULL;
    }
    if (s_wr_stack) {
        heap_caps_free(s_wr_stack);
        s_wr_stack = NULL;
    }
    if (s_wr_tcb) {
        heap_caps_free(s_wr_tcb);
        s_wr_tcb = NULL;
    }
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
    if (s_bounce_persist) {
        heap_caps_free(s_bounce_persist);
        s_bounce_persist = NULL;
        s_bounce_persist_sz = 0;
    }
    __atomic_store_n(&s_active_transfers, 0, __ATOMIC_SEQ_CST);
    s_last_transfer_end_us = 0;

    // Restaura o clock do SDMMC para 40 MHz High-Speed
    if (sd_card_get_frequency() != SDMMC_FREQ_HIGHSPEED) {
        sd_card_set_frequency(SDMMC_FREQ_HIGHSPEED);
    }
}

static esp_err_t api_upload_put_handler(httpd_req_t *req)
{
    set_cors_pna_headers(req);
    if (!check_api_key(req)) return ESP_FAIL;
    podcast_sync_notify_activity();

    int64_t t_handler_start = esp_timer_get_time();

    if (s_ota_in_progress) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Atualizacao OTA em andamento");
        return ESP_FAIL;
    }

    audio_player_stop_url();

    typedef struct {
        char query[1024];
        char path_enc[512];
        char rel_path[450];
        char abs_path[600];
        char title_enc[256];
        char display_title[128];
        char fail_detail[128];
        char expected_sha256[68];
        char err_json[256];
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
    if (httpd_query_key_value(sc->query, "batchTotal", num_buf, sizeof(num_buf)) == ESP_OK ||
        httpd_query_key_value(sc->query, "batch_total", num_buf, sizeof(num_buf)) == ESP_OK) {
        batch_total = atoll(num_buf);
    }
    if (httpd_query_key_value(sc->query, "title", sc->title_enc, sizeof(sc->title_enc)) == ESP_OK) {
        url_decode(sc->display_title, sc->title_enc, sizeof(sc->display_title));
    }
    if (idx < 1) idx = 1;
    if (count < 1) count = 1;

    if (!build_abs_path(sc->rel_path, sc->abs_path, sizeof(sc->abs_path)) || sc->rel_path[0] == '\0') {
        free(sc);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "caminho invalido");
        return ESP_FAIL;
    }

    if (httpd_req_get_hdr_value_str(req, "X-Expected-SHA256", sc->expected_sha256, sizeof(sc->expected_sha256)) != ESP_OK) {
        if (httpd_query_key_value(sc->query, "sha256", sc->expected_sha256, sizeof(sc->expected_sha256)) != ESP_OK) {
            sc->expected_sha256[0] = '\0';
        }
    }

    const char *display_name = sc->rel_path;
    const char *slash = strrchr(sc->rel_path, '/');
    if (slash) display_name = slash + 1;
    if (sc->display_title[0] != '\0') display_name = sc->display_title;

    if (!s_upload_lock) s_upload_lock = xSemaphoreCreateMutex();
    if (!s_upload_lock || xSemaphoreTake(s_upload_lock, pdMS_TO_TICKS(UPLOAD_LOCK_TIMEOUT_MS)) != pdTRUE) {
        free(sc);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "Outro upload em andamento", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }
    int64_t pre_lock_us = esp_timer_get_time() - t_handler_start;

    s_upload_io_busy = true;
    transfer_boost_begin();

    int64_t t_alloc0 = esp_timer_get_time();
    size_t content_len = req->content_len;
    progress_begin(display_name, content_len, idx, count, batch_total > 0 ? batch_total : (long long)content_len);

    uint8_t wifi_ch = 0;
    int rssi_dbm = 0;
    get_wifi_radio_telemetry(&wifi_ch, &rssi_dbm);
    bool use_hybrid_drain = apply_transfer_sd_clock_start(wifi_ch, false);

    bool pipeline_ok = ensure_transfer_pipeline_ready();
    int64_t setup_alloc_us = esp_timer_get_time() - t_alloc0;

    if (!pipeline_ok) {
        s_upload_io_busy = false;
        transfer_boost_end();
        xSemaphoreGive(s_upload_lock);
        free(sc);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sem memoria para o pipeline de upload");
        progress_end();
        return ESP_FAIL;
    }

    size_t bounce_sz = s_bounce_persist_sz;
    size_t recv_sz = s_recv_buf_persist_sz;
    char *buf = s_recv_buf_persist;

    memset(&s_upload_ctx, 0, sizeof(s_upload_ctx));
    s_upload_ctx.job_type = WORKER_JOB_UPLOAD;
    strlcpy(s_upload_ctx.abs_path, sc->abs_path, sizeof(s_upload_ctx.abs_path));
    s_upload_ctx.total_size = content_len;
    s_upload_ctx.bounce = s_bounce_persist;
    s_upload_ctx.bounce_size = bounce_sz;
    s_upload_ctx.rb = s_rb_persist;
    s_upload_ctx.calc_sha256 = (sc->expected_sha256[0] != '\0');
    s_upload_ctx.boost_drain_40mhz = use_hybrid_drain;

    xSemaphoreTake(s_wr_done_sem, 0);
    xSemaphoreGive(s_wr_start_sem);

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
            consecutive_timeouts++;
            if (consecutive_timeouts > max_consecutive_timeouts) {
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
        size_t rx_done = content_len - remaining;

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if (now_ms - last_prog_update_ms >= 50) {
            last_prog_update_ms = now_ms;
            progress_update(rx_done);
            podcast_sync_notify_activity();
            vTaskDelay(1);
        }
    }

    // Sinaliza fim ao escritor persistente e mede o tempo de drenagem residual do RingBuffer para o SD
    int64_t t_drain0 = esp_timer_get_time();
    if (!ok) s_upload_ctx.abort = true;
    s_upload_ctx.producer_done = true;
    xSemaphoreTake(s_wr_done_sem, portMAX_DELAY);
    int64_t drain_us = esp_timer_get_time() - t_drain0;

    int64_t t_teardown0 = esp_timer_get_time();
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

    // Se modo Hibrido promoveu para 40 MHz no drain, restaura 20 MHz ao final para o proximo arquivo iniciar limpo
    uint32_t end_sd_khz = sd_card_get_frequency();
    if (use_hybrid_drain && end_sd_khz != SDMMC_FREQ_DEFAULT) {
        sd_card_set_frequency(SDMMC_FREQ_DEFAULT);
    }
    int64_t teardown_us = esp_timer_get_time() - t_teardown0;

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
    float speed_mbs = (total_us > 0) ? (((float)content_len / (1024.0f * 1024.0f)) / ((float)total_us / 1000000.0f)) : 0.0f;
    float sd_write_mbps = (s_upload_ctx.fwrite_us > 0)
        ? (((float)s_upload_ctx.written / (1024.0f * 1024.0f)) / ((float)s_upload_ctx.fwrite_us / 1000000.0f))
        : 0.0f;

    snprintf(s_last_upload_stats_json, sizeof(s_last_upload_stats_json),
             "{\"bytes\":%u,\"total_us\":%lld,\"total_ms\":%u,\"mbps\":%.2f,\"speed_mbs\":%.2f,\"sd_write_mbps\":%.2f,"
             "\"pre_lock_us\":%lld,\"alloc_us\":%lld,\"alloc_ms\":%u,"
             "\"open_us\":%lld,\"open_ms\":%u,\"mkdir_us\":%lld,\"unlink_us\":%lld,\"f_expand_us\":%lld,\"fopen_us\":%lld,"
             "\"recv_us\":%lld,\"recv_ms\":%u,\"recv_calls\":%u,\"recv_max_us\":%lld,\"recv_max_ms\":%u,\"recv_slow\":%u,"
             "\"ring_us\":%lld,\"ring_ms\":%u,\"ring_calls\":%u,\"ring_full\":%u,"
             "\"wr_wait_us\":%lld,\"wr_wait_ms\":%u,\"memcpy_us\":%lld,\"memcpy_ms\":%u,"
             "\"fwrite_us\":%lld,\"fwrite_ms\":%u,\"fwrite_calls\":%u,\"wr_calls\":%u,\"wr_avg_ms\":%u,"
             "\"fwrite_max_us\":%lld,\"wr_max_ms\":%u,\"fwrite_slow\":%u,\"wr_slow\":%u,"
             "\"sha256_us\":%lld,\"yield_us\":%lld,\"drain_us\":%lld,\"drain_ms\":%u,"
             "\"fclose_us\":%lld,\"fclose_ms\":%u,\"teardown_us\":%lld,"
             "\"wifi_ch\":%u,\"rssi_dbm\":%d,\"sd_clk_mode\":%d,\"sd_khz\":%u,"
             "\"rb_kb\":%u,\"bounce_kb\":%u,\"recv_kb\":%u,\"sha256\":\"%s\"}",
             (unsigned)content_len, (long long)total_us, (unsigned)total_ms, speed_mbs, speed_mbs, sd_write_mbps,
             (long long)pre_lock_us, (long long)setup_alloc_us, (unsigned)alloc_ms,
             (long long)s_upload_ctx.open_us, (unsigned)open_ms,
             (long long)s_upload_ctx.mkdir_us, (long long)s_upload_ctx.unlink_us,
             (long long)s_upload_ctx.f_expand_us, (long long)s_upload_ctx.fopen_us,
             (long long)t_recv_us, (unsigned)recv_ms, (unsigned)n_recv,
             (long long)t_recv_max_us, (unsigned)recv_max_ms, (unsigned)n_recv_slow,
             (long long)t_ring_us, (unsigned)ring_ms, (unsigned)n_ring_calls, (unsigned)n_ring_full,
             (long long)s_upload_ctx.wr_wait_us, (unsigned)wr_wait_ms,
             (long long)s_upload_ctx.memcpy_us, (unsigned)memcpy_ms,
             (long long)s_upload_ctx.fwrite_us, (unsigned)fwrite_ms,
             (unsigned)wr_calls, (unsigned)wr_calls, (unsigned)wr_avg_ms,
             (long long)s_upload_ctx.fwrite_max_us, (unsigned)wr_max_ms,
             (unsigned)wr_slow, (unsigned)wr_slow,
             (long long)s_upload_ctx.sha256_us, (long long)s_upload_ctx.yield_us,
             (long long)drain_us, (unsigned)drain_ms,
             (long long)s_upload_ctx.fclose_us, (unsigned)fclose_ms, (long long)teardown_us,
             (unsigned)wifi_ch, rssi_dbm, s_sd_clock_mode, (unsigned)end_sd_khz,
             (unsigned)(s_rb_persist_sz / 1024), (unsigned)(bounce_sz / 1024), (unsigned)(recv_sz / 1024),
             s_upload_ctx.sha256_hex);

    ESP_LOGI(TAG, "[TELEMETRY] %s", s_last_upload_stats_json);

    s_upload_io_busy = false;
    transfer_boost_end();
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
             sc->rel_path, idx, count, (unsigned)total_ms, speed_mbs,
             s_upload_ctx.sha256_hex[0] ? s_upload_ctx.sha256_hex : "skipped");
    snprintf(sc->err_json, sizeof(sc->err_json),
             "{\"status\":\"ok\",\"bytes\":%u,\"sha256\":\"%s\"}",
             (unsigned)content_len, s_upload_ctx.sha256_hex);
    httpd_resp_set_type(req, "application/json");
    if (s_upload_ctx.sha256_hex[0] != '\0') {
        httpd_resp_set_hdr(req, "X-Content-SHA256", s_upload_ctx.sha256_hex);
    }
    httpd_resp_set_hdr(req, "X-Upload-Stats", s_last_upload_stats_json);
    httpd_resp_send(req, sc->err_json, HTTPD_RESP_USE_STRLEN);
    free(sc);
    return ESP_OK;
}

static esp_err_t api_download_get_handler(httpd_req_t *req)
{
    set_cors_pna_headers(req);
    int64_t t_dl_start = esp_timer_get_time();

    typedef struct {
        char query[512];
        char path_enc[400];
        char rel_path[400];
        char abs_path[600];
        char range_hdr[64];
        char crange_str[128];
        char dl_flag[8];
        char stream_flag[8];
        char disp_hdr[220];
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

    if (s_upload_io_busy) {
        free(sc);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "Upload em andamento no SD", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    int64_t t_open0 = esp_timer_get_time();
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
    setvbuf(fp, NULL, _IONBF, 0);

    size_t fsize = (size_t)st.st_size;
    int64_t stat_open_us = esp_timer_get_time() - t_open0;

    const char *ext = strrchr(sc->abs_path, '.');
    if (ext && strcasecmp(ext, ".png") == 0) httpd_resp_set_type(req, "image/png");
    else if (ext && (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0)) httpd_resp_set_type(req, "image/jpeg");
    else if (ext && strcasecmp(ext, ".webp") == 0) httpd_resp_set_type(req, "image/webp");
    else if (ext && strcasecmp(ext, ".mp4") == 0) httpd_resp_set_type(req, "video/mp4");
    else if (ext && strcasecmp(ext, ".webm") == 0) httpd_resp_set_type(req, "video/webm");
    else if (ext && strcasecmp(ext, ".svg") == 0) httpd_resp_set_type(req, "image/svg+xml");
    else if (ext && (strcasecmp(ext, ".json") == 0 || strcasecmp(ext, ".jsonl") == 0)) httpd_resp_set_type(req, "application/json; charset=utf-8");
    else if (ext && strcasecmp(ext, ".flac") == 0) httpd_resp_set_type(req, "audio/flac");
    else if (ext && strcasecmp(ext, ".wav") == 0) httpd_resp_set_type(req, "audio/wav");
    else if (ext && strcasecmp(ext, ".m4a") == 0) httpd_resp_set_type(req, "audio/mp4");
    else if (ext && (strcasecmp(ext, ".ogg") == 0 || strcasecmp(ext, ".opus") == 0)) httpd_resp_set_type(req, "audio/ogg");
    else httpd_resp_set_type(req, "audio/mpeg");

    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
    if (httpd_query_key_value(sc->query, "dl", sc->dl_flag, sizeof(sc->dl_flag)) == ESP_OK && sc->dl_flag[0] == '1') {
        const char *bname = strrchr(sc->abs_path, '/');
        bname = bname ? (bname + 1) : "MPS3_Wrapped.png";
        snprintf(sc->disp_hdr, sizeof(sc->disp_hdr), "attachment; filename=\"%.*s\"", 180, bname);
        httpd_resp_set_hdr(req, "Content-Disposition", sc->disp_hdr);
    }

    bool is_stream = false;
    if (httpd_query_key_value(sc->query, "stream", sc->stream_flag, sizeof(sc->stream_flag)) == ESP_OK && sc->stream_flag[0] == '1') {
        is_stream = true;
    }

    if (fsize == 0) {
        httpd_resp_send(req, NULL, 0);
        free(sc);
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
    // Mantem Keep-Alive ativo para reutilizacao de socket TCP entre downloads consecutivos

    int sockfd = httpd_req_to_sockfd(req);
    if (sockfd >= 0) {
        int nodelay = 1;
        setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
        int sndbuf = 65536;
        setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, (const char *)&sndbuf, sizeof(sndbuf));
        struct timeval tv_dl = { .tv_sec = 8, .tv_usec = 0 };
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv_dl, sizeof(tv_dl));
    }

    if (start > 0) {
        int64_t ts0 = esp_timer_get_time();
        fseek(fp, start, SEEK_SET);
        stat_open_us += esp_timer_get_time() - ts0;
    }

    transfer_boost_begin();
    uint8_t wifi_ch = 0;
    int rssi_dbm = 0;
    get_wifi_radio_telemetry(&wifi_ch, &rssi_dbm);
    apply_transfer_sd_clock_start(wifi_ch, true);

    bool locked_pipeline = false;
    if (!s_upload_lock) s_upload_lock = xSemaphoreCreateMutex();
    if (s_upload_lock && xSemaphoreTake(s_upload_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (ensure_transfer_pipeline_ready()) {
            locked_pipeline = true;
        } else {
            xSemaphoreGive(s_upload_lock);
        }
    }

    int64_t sd_read_us = 0, fread_max_us = 0, ring_us = 0, ring_send_us = 0, net_send_us = 0, send_max_us = 0, fclose_us = 0;
    uint32_t fread_calls = 0, send_calls = 0, send_slow = 0;
    size_t sent_total = 0;
    bool dl_ok = true;

    const char *slash = strrchr(sc->rel_path, '/');
    const char *display_name = slash ? (slash + 1) : sc->rel_path;
    progress_begin(display_name, content_length, 1, 1, (long long)content_length);
    if (is_stream) {
        s_stream_active = true;
        s_stream_file_size = fsize;
        s_stream_file_done = (start + content_length > fsize) ? fsize : (start + content_length);
        strncpy(s_stream_filename, display_name, sizeof(s_stream_filename) - 1);
        s_stream_filename[sizeof(s_stream_filename) - 1] = '\0';
        s_stream_expire_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) + 3000;
        s_ui_state = WIFI_UI_STREAMING;
    }
    uint32_t last_dl_prog_ms = 0;

    if (locked_pipeline) {
        char *send_buf = s_recv_buf_persist;
        size_t send_buf_sz = s_recv_buf_persist_sz;

        memset(&s_upload_ctx, 0, sizeof(s_upload_ctx));
        s_upload_ctx.job_type = WORKER_JOB_DOWNLOAD;
        s_upload_ctx.fp = fp;
        s_upload_ctx.rb = s_rb_persist;
        s_upload_ctx.bounce = s_bounce_persist;
        s_upload_ctx.bounce_size = s_bounce_persist_sz;
        s_upload_ctx.dl_total_bytes = content_length;

        xSemaphoreTake(s_wr_done_sem, 0);
        xSemaphoreGive(s_wr_start_sem);

        size_t remaining = content_length;
        while (remaining > 0) {
            size_t want = (remaining < send_buf_sz) ? remaining : send_buf_sz;
            size_t filled = 0;
            while (filled < want) {
                bool rdr_done = s_upload_ctx.dl_reader_done;
                size_t got = 0;
                int64_t trw0 = esp_timer_get_time();
                void *item = xRingbufferReceiveUpTo(
                    s_upload_ctx.rb, &got,
                    rdr_done ? 0 : pdMS_TO_TICKS(50),
                    want - filled
                );
                ring_us += esp_timer_get_time() - trw0;
                if (item) {
                    memcpy(send_buf + filled, item, got);
                    filled += got;
                    vRingbufferReturnItem(s_upload_ctx.rb, item);
                } else if (rdr_done || s_upload_ctx.failed) {
                    break;
                }
            }

            if (filled == 0) {
                ESP_LOGW(TAG, "Download: stream interrompido (restante=%u, reader_done=%d, failed=%d)",
                         (unsigned)remaining, (int)s_upload_ctx.dl_reader_done, (int)s_upload_ctx.failed);
                dl_ok = false;
                break;
            }

            int64_t ts0 = esp_timer_get_time();
            esp_err_t s_err = httpd_resp_send_chunk(req, send_buf, filled);
            int64_t tsd = esp_timer_get_time() - ts0;
            net_send_us += tsd;
            send_calls++;
            if (tsd > send_max_us) send_max_us = tsd;
            if (tsd > 50000) send_slow++;

            if (s_err != ESP_OK) {
                s_upload_ctx.abort = true;
                dl_ok = false;
                break;
            }
            remaining -= filled;
            sent_total += filled;

            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
            if (now_ms - last_dl_prog_ms >= 50) {
                last_dl_prog_ms = now_ms;
                progress_update(sent_total);
                if (is_stream) {
                    s_stream_file_done = (start + sent_total > fsize) ? fsize : (start + sent_total);
                    s_stream_expire_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) + 3000;
                }
                podcast_sync_notify_activity();
                vTaskDelay(1);
            }
        }

        if (!dl_ok) {
            s_upload_ctx.abort = true;
            // Drena o RingBuffer para desbloquear imediatamente o leitor caso estivesse cheio
            while (!s_upload_ctx.dl_reader_done) {
                size_t dummy_sz = 0;
                void *dummy = xRingbufferReceiveUpTo(s_upload_ctx.rb, &dummy_sz, pdMS_TO_TICKS(10), 65536);
                if (dummy) vRingbufferReturnItem(s_upload_ctx.rb, dummy);
            }
        }
        xSemaphoreTake(s_wr_done_sem, portMAX_DELAY);

        sd_read_us = s_upload_ctx.dl_sd_read_us;
        fread_max_us = s_upload_ctx.dl_fread_max_us;
        fread_calls = s_upload_ctx.dl_fread_calls;
        ring_send_us = s_upload_ctx.dl_ring_send_us;

        xSemaphoreGive(s_upload_lock);
    } else {
        // Fallback sincrono caso s_upload_lock esteja ocupado por outra operacao
        size_t chunk_sz = 32768;
        char *chunk = (char *)heap_caps_malloc(chunk_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!chunk) {
            chunk_sz = 4096;
            chunk = (char *)malloc(chunk_sz);
        }
        if (!chunk) {
            transfer_boost_end();
            free(sc);
            fclose(fp);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
            return ESP_FAIL;
        }

        size_t remaining = content_length;
        while (remaining > 0) {
            if (s_upload_io_busy) {
                dl_ok = false;
                break;
            }
            size_t to_read = (remaining < chunk_sz) ? remaining : chunk_sz;
            int64_t tr0 = esp_timer_get_time();
            size_t read_bytes = fread(chunk, 1, to_read, fp);
            int64_t trd = esp_timer_get_time() - tr0;
            sd_read_us += trd;
            fread_calls++;
            if (trd > fread_max_us) fread_max_us = trd;
            if (read_bytes == 0) {
                dl_ok = false;
                break;
            }

            int64_t ts0 = esp_timer_get_time();
            esp_err_t s_err = httpd_resp_send_chunk(req, chunk, read_bytes);
            int64_t tsd = esp_timer_get_time() - ts0;
            net_send_us += tsd;
            send_calls++;
            if (tsd > send_max_us) send_max_us = tsd;
            if (tsd > 50000) send_slow++;
            if (s_err != ESP_OK) {
                dl_ok = false;
                break;
            }
            remaining -= read_bytes;
            sent_total += read_bytes;

            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
            if (now_ms - last_dl_prog_ms >= 50) {
                last_dl_prog_ms = now_ms;
                progress_update(sent_total);
                if (is_stream) {
                    s_stream_file_done = (start + sent_total > fsize) ? fsize : (start + sent_total);
                    s_stream_expire_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) + 3000;
                }
                podcast_sync_notify_activity();
                vTaskDelay(1);
            }
        }
        free(chunk);
    }

    if (dl_ok) {
        int64_t ts0 = esp_timer_get_time();
        httpd_resp_send_chunk(req, NULL, 0);
        net_send_us += esp_timer_get_time() - ts0;
    }

    int64_t tc0 = esp_timer_get_time();
    fclose(fp);
    fclose_us = esp_timer_get_time() - tc0;

    int64_t total_us = esp_timer_get_time() - t_dl_start;
    uint32_t total_ms = (uint32_t)(total_us / 1000);
    float mbps = (total_us > 0) ? (((float)sent_total / (1024.0f * 1024.0f)) / ((float)total_us / 1000000.0f)) : 0.0f;
    float sd_read_mbps = (sd_read_us > 0) ? (((float)sent_total / (1024.0f * 1024.0f)) / ((float)sd_read_us / 1000000.0f)) : 0.0f;

    snprintf(s_last_download_stats_json, sizeof(s_last_download_stats_json),
             "{\"bytes\":%u,\"total_us\":%lld,\"total_ms\":%u,\"mbps\":%.2f,\"speed_mbs\":%.2f,\"sd_read_mbps\":%.2f,"
             "\"stat_open_us\":%lld,\"stat_open_ms\":%u,"
             "\"sd_read_us\":%lld,\"sd_read_ms\":%u,\"fread_calls\":%u,\"fread_max_us\":%lld,\"fread_max_ms\":%u,"
             "\"ring_us\":%lld,\"ring_ms\":%u,\"ring_send_us\":%lld,"
             "\"net_send_us\":%lld,\"net_send_ms\":%u,\"send_calls\":%u,\"send_max_us\":%lld,\"send_max_ms\":%u,\"send_slow\":%u,"
             "\"fclose_us\":%lld,\"fclose_ms\":%u,"
             "\"wifi_ch\":%u,\"rssi_dbm\":%d,\"sd_clk_mode\":%d,\"sd_khz\":%u,"
             "\"rb_kb\":%u,\"bounce_kb\":%u,\"overlapped\":%s}",
             (unsigned)sent_total, (long long)total_us, (unsigned)total_ms, mbps, mbps, sd_read_mbps,
             (long long)stat_open_us, (unsigned)(stat_open_us / 1000),
             (long long)sd_read_us, (unsigned)(sd_read_us / 1000), (unsigned)fread_calls,
             (long long)fread_max_us, (unsigned)(fread_max_us / 1000),
             (long long)ring_us, (unsigned)(ring_us / 1000), (long long)ring_send_us,
             (long long)net_send_us, (unsigned)(net_send_us / 1000), (unsigned)send_calls,
             (long long)send_max_us, (unsigned)(send_max_us / 1000), (unsigned)send_slow,
             (long long)fclose_us, (unsigned)(fclose_us / 1000),
             (unsigned)wifi_ch, rssi_dbm, s_sd_clock_mode, (unsigned)sd_card_get_frequency(),
             (unsigned)(s_rb_persist_sz / 1024), (unsigned)(s_bounce_persist_sz / 1024),
             locked_pipeline ? "true" : "false");

    ESP_LOGI(TAG, "[DL_TELEMETRY] %s", s_last_download_stats_json);

    if (is_stream) {
        s_stream_file_done = (start + sent_total > fsize) ? fsize : (start + sent_total);
        s_stream_kbps = mbps * 1024.0f;
        s_stream_expire_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) + 3000;
        s_stream_active = true;
    }

    progress_finish_file(content_length);
    progress_end();
    transfer_boost_end();
    free(sc);
    return dl_ok ? ESP_OK : ESP_FAIL;
}

static void get_json_string(const char *json, const char *key, char *out, size_t max_len);

static esp_err_t api_delete_handler(httpd_req_t *req)
{
    set_cors_pna_headers(req);
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

bool wifi_transfer_delete_network(const char *ssid)
{
    return known_networks_remove(ssid);
}

bool wifi_transfer_promote_network(const char *ssid)
{
    known_networks_load();
    for (int i = 0; i < s_known_count; i++) {
        if (strcmp(s_known[i].ssid, ssid) == 0) {
            char pass_copy[WIFI_PASS_MAX_LEN];
            strlcpy(pass_copy, s_known[i].pass, sizeof(pass_copy));
            known_networks_put_front(ssid, pass_copy);
            return true;
        }
    }
    return false;
}

// GET /api/wifi/networks -> lista de redes conhecidas (sem senha - a
// pagina web nunca precisa reler a senha, so' escrever uma nova).
static esp_err_t api_wifi_networks_get_handler(httpd_req_t *req)
{
    set_cors_pna_headers(req);
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
    set_cors_pna_headers(req);
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
    set_cors_pna_headers(req);
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
    set_cors_pna_headers(req);
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
    set_cors_pna_headers(req);
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
    set_cors_pna_headers(req);
    ESP_LOGI(TAG, "Web Radio stop solicitada");
    audio_player_stop_url();
    httpd_resp_send(req, "{\"status\":\"ok\"}", -1);
    return ESP_OK;
}


// --- EQ Handlers ---
static esp_err_t api_eq_get_handler(httpd_req_t *req) {
    set_cors_pna_headers(req);
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

static int http_recv_body_full(httpd_req_t *req, char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return 0;
    buf[0] = '\0';
    if (req->content_len == 0) return 0;
    size_t to_read = (req->content_len < buf_size - 1) ? req->content_len : (buf_size - 1);
    size_t total = 0;
    int timeouts = 0;
    while (total < to_read) {
        int r = httpd_req_recv(req, buf + total, (int)(to_read - total));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 4) break;
            continue;
        }
        if (r <= 0) break;
        total += (size_t)r;
    }
    buf[total] = '\0';
    return (int)total;
}

static void get_json_string(const char *json, const char *key, char *out, size_t max_len) {
    if (!out || max_len == 0) return;
    out[0] = '\0';
    if (!json || !key) return;
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    size_t slen = strlen(search);
    const char *cur = json;
    while ((cur = strstr(cur, search)) != NULL) {
        const char *p = cur + slen;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p != ':') {
            cur += slen;
            continue;
        }
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p != '"') return;
        p++;
        size_t o = 0;
        while (*p != '\0' && *p != '"' && o + 1 < max_len) {
            if (*p == '\\' && p[1] != '\0') {
                p++;
                if (*p == 'n') out[o++] = '\n';
                else if (*p == 'r') out[o++] = '\r';
                else if (*p == 't') out[o++] = '\t';
                else out[o++] = *p;
                p++;
            } else {
                out[o++] = *p++;
            }
        }
        out[o] = '\0';
        return;
    }
}

static esp_err_t api_eq_post_handler(httpd_req_t *req) {
    set_cors_pna_headers(req);
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
    set_cors_pna_headers(req);
    if (!check_api_key(req)) return ESP_FAIL;
    podcast_sync_notify_activity();

    typedef struct {
        char buf[1536];
        char query[1024];
        char src_enc[512];
        char dst_enc[512];
        char old_path[450];
        char new_path[450];
        char old_abs[600];
        char new_abs[600];
        char err_msg[256];
    } rename_scratch_t;

    rename_scratch_t *sc = (rename_scratch_t *)http_scratch_alloc(sizeof(rename_scratch_t));
    if (!sc) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (req->content_len > 0 && http_recv_body_full(req, sc->buf, sizeof(sc->buf)) > 0) {
        get_json_string(sc->buf, "old_path", sc->old_path, sizeof(sc->old_path));
        if (sc->old_path[0] == '\0') get_json_string(sc->buf, "src", sc->old_path, sizeof(sc->old_path));
        if (sc->old_path[0] == '\0') get_json_string(sc->buf, "src_path", sc->old_path, sizeof(sc->old_path));

        get_json_string(sc->buf, "new_path", sc->new_path, sizeof(sc->new_path));
        if (sc->new_path[0] == '\0') get_json_string(sc->buf, "dst", sc->new_path, sizeof(sc->new_path));
        if (sc->new_path[0] == '\0') get_json_string(sc->buf, "dest_path", sc->new_path, sizeof(sc->new_path));
    }

    if ((sc->old_path[0] == '\0' || sc->new_path[0] == '\0') &&
        httpd_req_get_url_query_str(req, sc->query, sizeof(sc->query)) == ESP_OK) {
        if (sc->old_path[0] == '\0') {
            if (httpd_query_key_value(sc->query, "src", sc->src_enc, sizeof(sc->src_enc)) == ESP_OK ||
                httpd_query_key_value(sc->query, "old_path", sc->src_enc, sizeof(sc->src_enc)) == ESP_OK ||
                httpd_query_key_value(sc->query, "old", sc->src_enc, sizeof(sc->src_enc)) == ESP_OK) {
                url_decode(sc->old_path, sc->src_enc, sizeof(sc->old_path));
            }
        }
        if (sc->new_path[0] == '\0') {
            if (httpd_query_key_value(sc->query, "dst", sc->dst_enc, sizeof(sc->dst_enc)) == ESP_OK ||
                httpd_query_key_value(sc->query, "new_path", sc->dst_enc, sizeof(sc->dst_enc)) == ESP_OK ||
                httpd_query_key_value(sc->query, "new", sc->dst_enc, sizeof(sc->dst_enc)) == ESP_OK) {
                url_decode(sc->new_path, sc->dst_enc, sizeof(sc->new_path));
            }
        }
    }

    httpd_resp_set_type(req, "application/json");

    if (sc->old_path[0] == '\0' || sc->new_path[0] == '\0' ||
        !build_abs_path(sc->old_path, sc->old_abs, sizeof(sc->old_abs)) ||
        !build_abs_path(sc->new_path, sc->new_abs, sizeof(sc->new_abs))) {
        free(sc);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"error\":\"caminho invalido\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    if (strcmp(sc->old_abs, sc->new_abs) == 0) {
        free(sc);
        httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    struct stat st_src;
    if (stat(sc->old_abs, &st_src) != 0) {
        ESP_LOGW(TAG, "Rename/move origem ausente: %s", sc->old_abs);
        free(sc);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "{\"error\":\"origem nao encontrada\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    size_t old_len = strlen(sc->old_abs);
    if (S_ISDIR(st_src.st_mode) &&
        strncmp(sc->old_abs, sc->new_abs, old_len) == 0 &&
        (sc->new_abs[old_len] == '/' || sc->new_abs[old_len] == '\0')) {
        free(sc);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "{\"error\":\"destino nao pode ser subpasta da propria origem\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    mkdir_p_for_file(sc->new_abs);

    // FatFS f_rename falha com EEXIST caso o arquivo de destino ja exista no cartao SD
    struct stat st_dst;
    if (stat(sc->new_abs, &st_dst) == 0 && !S_ISDIR(st_dst.st_mode)) {
        remove(sc->new_abs);
    }

    if (rename(sc->old_abs, sc->new_abs) != 0) {
        int err_no = errno;
        ESP_LOGE(TAG, "Falha em rename('%s' -> '%s'): errno=%d (%s)", sc->old_abs, sc->new_abs, err_no, strerror(err_no));
        snprintf(sc->err_msg, sizeof(sc->err_msg), "{\"error\":\"falha ao mover/renomear (%s)\"}", strerror(err_no));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, sc->err_msg, HTTPD_RESP_USE_STRLEN);
        free(sc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Movido/renomeado: %s -> %s", sc->old_path, sc->new_path);
    s_status_last_calc_ms = 0;
    free(sc);
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_move_post_handler(httpd_req_t *req) {
    return api_rename_post_handler(req);
}

static esp_err_t api_copy_post_handler(httpd_req_t *req) {
    set_cors_pna_headers(req);
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
    set_cors_pna_headers(req);
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
    set_cors_pna_headers(req);
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
    set_cors_pna_headers(req);
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
    audio_player_stop_url();
    {
        playback_state_t *st_tmp = (playback_state_t *)http_scratch_alloc(sizeof(playback_state_t));
        if (st_tmp) {
            audio_player_get_state(st_tmp);
            if (st_tmp->playing && !st_tmp->paused) {
                audio_player_toggle_play_pause();
            }
            free(st_tmp);
        }
    }

    // Encerra a task mDNS (cuja stack fica em PSRAM) antes de iniciar escrita na SPI Flash,
    // evitando panic esp_task_stack_is_sane_cache_disabled() durante o erase de blocos OTA!
    stop_mdns_service();

    // Trava estado OTA imediatamente para bloquear requisicoes concorrentes e saida acidental
    s_ota_in_progress = true;
    s_ota_pct = 0;
    podcast_sync_notify_activity();
    snprintf(s_ota_status, sizeof(s_ota_status), "Validando cabecalho...");
    s_ui_state = WIFI_UI_OTA_UPDATING;

    // Reutiliza o buffer DMA persistente s_bounce_persist (8 KB na SRAM interna) se disponivel,
    // eliminando qualquer alocacao adicional na DRAM interna que causaria escassez e 'wifi:mem fail'
    char *ota_write_data = NULL;
    bool ota_buf_allocated = false;
    if (s_bounce_persist && s_bounce_persist_sz >= OTA_BUFF_SIZE) {
        ota_write_data = (char *)s_bounce_persist;
    } else {
        ota_write_data = (char *)heap_caps_malloc(OTA_BUFF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ota_buf_allocated = (ota_write_data != NULL);
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

    // Validar esp_app_desc_t no offset 0x20 diretamente no buffer (sem alocar 256B na stack do httpd)
    const esp_app_desc_t *app_desc = (const esp_app_desc_t *)(ota_write_data + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    if (app_desc->magic_word != ESP_APP_DESC_MAGIC_WORD ||
        strncmp(app_desc->project_name, "mps3", sizeof(app_desc->project_name)) != 0) {
        ESP_LOGE(TAG, "esp_app_desc invalido: magic=0x%08X (esp: 0x%08X)",
                 (unsigned int)app_desc->magic_word, (unsigned int)ESP_APP_DESC_MAGIC_WORD);
        if (ota_buf_allocated) free(ota_write_data);
        s_ota_in_progress = false;
        s_ui_state = WIFI_UI_CONNECTED;
        s_ota_status[0] = '\0';
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Arquivo .bin nao pertence ao projeto mps3 ou esta corrompido!\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Firmware validado! Projeto: '%.31s', Versao: '%.31s'. Gravando em %s...",
             app_desc->project_name, app_desc->version, target_part->label);

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
            podcast_sync_notify_activity();
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
    set_cors_pna_headers(req);
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
    set_cors_pna_headers(req);

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
    set_cors_pna_headers(req);
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

    long long expected_size = -1;
    if (req->content_len > 0 && http_recv_body_full(req, sc->body, sizeof(sc->body)) > 0) {
        get_json_string(sc->body, "path", sc->rel_path, sizeof(sc->rel_path));
        get_json_string(sc->body, "expected_sha256", sc->expected_sha256, sizeof(sc->expected_sha256));
        if (sc->expected_sha256[0] == '\0') {
            get_json_string(sc->body, "sha256", sc->expected_sha256, sizeof(sc->expected_sha256));
        }
        const char *psz = strstr(sc->body, "\"expected_size\"");
        if (psz) {
            psz = strchr(psz, ':');
            if (psz) expected_size = strtoll(psz + 1, NULL, 10);
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

    bool locked_bounce = false;
    if (s_upload_lock && s_bounce_persist && s_bounce_persist_sz >= 4096) {
        if (xSemaphoreTake(s_upload_lock, pdMS_TO_TICKS(UPLOAD_LOCK_TIMEOUT_MS)) == pdTRUE) {
            locked_bounce = true;
        }
    }
    uint8_t *rd_buf = locked_bounce ? s_bounce_persist : sc->read_buf;
    size_t rd_buf_sz = locked_bounce ? s_bounce_persist_sz : sizeof(sc->read_buf);

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
    if (locked_bounce) {
        xSemaphoreGive(s_upload_lock);
    }
    fclose(fp);

    uint8_t digest[32] = {0};
    mps3_sha256_final(&sc->sha_ctx, digest);

    for (int i = 0; i < 32; i++) {
        snprintf(&sc->sha256_hex[i * 2], 3, "%02x", digest[i]);
    }
    sc->sha256_hex[64] = '\0';

    if ((expected_size >= 0 && (long long)total_read != expected_size) ||
        (sc->expected_sha256[0] != '\0' && strcasecmp(sc->expected_sha256, sc->sha256_hex) != 0)) {
        snprintf(sc->resp_json, sizeof(sc->resp_json),
                 "{\"verified\":false,\"error\":\"sha256_or_size_mismatch\",\"expected\":\"%s\",\"actual\":\"%s\",\"size\":%llu}",
                 sc->expected_sha256, sc->sha256_hex, (unsigned long long)total_read);
        httpd_resp_set_status(req, "422 Unprocessable Entity");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, sc->resp_json, HTTPD_RESP_USE_STRLEN);
        free(sc);
        return ESP_FAIL;
    }

    json_escape(sc->rel_path, sc->rel_esc, sizeof(sc->rel_esc));
    snprintf(sc->resp_json, sizeof(sc->resp_json),
             "{\"status\":\"ok\",\"verified\":true,\"path\":\"%s\",\"size\":%llu,\"sha256\":\"%s\"}",
             sc->rel_esc, (unsigned long long)total_read, sc->sha256_hex);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, sc->resp_json, HTTPD_RESP_USE_STRLEN);
    free(sc);
    return ESP_OK;
}

static esp_err_t api_sync_begin_post_handler(httpd_req_t *req)
{
    set_cors_pna_headers(req);
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
    set_cors_pna_headers(req);
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
    set_cors_pna_headers(req);
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

static uint64_t get_json_uint64(const char *json, const char *key, uint64_t def_val)
{
    if (!json || !key) return def_val;
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return def_val;
    p = strchr(p + strlen(search), ':');
    if (!p) return def_val;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '"') p++;
    if (*p == 't' || *p == 'T') return 1;
    if (*p == 'f' || *p == 'F') return 0;
    if (*p < '0' || *p > '9') return def_val;
    return (uint64_t)strtoull(p, NULL, 10);
}

#define HTTPS_WORKER_STACK_SIZE (24 * 1024)

typedef struct {
    httpd_req_t *req;
    SemaphoreHandle_t done_sem;
    char req_buf[4096];
    char target_url[2048];
    char method[16];
    char ua[256];
    char client_name[32];
    char client_version[32];
    char visitor_id[192];
    const char *post_body;
    size_t post_body_len;
    int status_code;
    int tls_err;
    int tls_flags;
    bool headers_sent;
    bool chunk_failed;
    esp_err_t perform_err;
} yt_relay_ctx_t;

static esp_err_t yt_relay_http_event_handler(esp_http_client_event_t *evt)
{
    yt_relay_ctx_t *ctx = (yt_relay_ctx_t *)evt->user_data;
    if (!ctx || !ctx->req) return ESP_OK;

    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
        int status = esp_http_client_get_status_code(evt->client);
        if (status >= 300 && status < 400) {
            return ESP_OK;
        }
        ctx->status_code = status;
        if (!ctx->headers_sent) {
            ctx->headers_sent = true;
            if (status >= 400) {
                httpd_resp_set_status(ctx->req, "502 Bad Gateway");
            }
        }
        if (httpd_resp_send_chunk(ctx->req, (const char *)evt->data, evt->data_len) != ESP_OK) {
            ctx->chunk_failed = true;
            return ESP_FAIL;
        }
    } else if (evt->event_id == HTTP_EVENT_ON_FINISH) {
        int status = esp_http_client_get_status_code(evt->client);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            esp_http_client_set_redirection(evt->client);
        }
    }
    return ESP_OK;
}

static void yt_relay_worker_task(void *arg)
{
    yt_relay_ctx_t *ctx = (yt_relay_ctx_t *)arg;
    bool is_post = (strcasecmp(ctx->method, "GET") != 0);

    esp_http_client_config_t cfg = {
        .url = ctx->target_url,
        .method = is_post ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = 20000,
        .buffer_size = 8192,
        .buffer_size_tx = 4096,
        .max_redirection_count = 6,
        .disable_auto_redirect = true,
        .skip_cert_common_name_check = false,
        .event_handler = yt_relay_http_event_handler,
        .user_data = ctx,
        .user_agent = ctx->ua[0] ? ctx->ua : "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36",
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ctx->perform_err = ESP_ERR_NO_MEM;
    } else {
        esp_http_client_set_header(client, "Accept-Encoding", "identity");
        esp_http_client_set_header(client, "Accept-Language", "pt-BR,pt;q=0.9,en-US;q=0.8");
        if (strstr(ctx->target_url, "youtube.com") != NULL) {
            esp_http_client_set_header(client, "Origin", "https://www.youtube.com");
        }
        if (ctx->client_name[0] != '\0') {
            esp_http_client_set_header(client, "X-YouTube-Client-Name", ctx->client_name);
        }
        if (ctx->client_version[0] != '\0') {
            esp_http_client_set_header(client, "X-YouTube-Client-Version", ctx->client_version);
        }
        if (ctx->visitor_id[0] != '\0') {
            esp_http_client_set_header(client, "X-Goog-Visitor-Id", ctx->visitor_id);
        }
        if (is_post) {
            esp_http_client_set_header(client, "Content-Type", "application/json");
            if (ctx->post_body && ctx->post_body_len > 0) {
                esp_http_client_set_post_field(client, ctx->post_body, (int)ctx->post_body_len);
            }
        }
        ctx->perform_err = esp_http_client_perform(client);
        ctx->status_code = esp_http_client_get_status_code(client);
        ctx->tls_err = esp_http_client_get_errno(client);
        esp_http_client_cleanup(client);
    }

    xSemaphoreGive(ctx->done_sem);
    vTaskDelay(portMAX_DELAY);
}

static esp_err_t api_yt_innertube_post_handler(httpd_req_t *req)
{
    set_cors_pna_headers(req);
    if (!check_api_key(req)) return ESP_FAIL;
    podcast_sync_notify_activity();

    yt_relay_ctx_t *ctx = (yt_relay_ctx_t *)http_scratch_alloc(sizeof(yt_relay_ctx_t));
    if (!ctx) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int nread = http_recv_body_full(req, ctx->req_buf, sizeof(ctx->req_buf));
    if (nread <= 0) {
        free(ctx);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"empty_body\"}");
        return ESP_FAIL;
    }

    char *nl = strchr(ctx->req_buf, '\n');
    if (nl) {
        *nl = '\0';
        ctx->post_body = nl + 1;
        ctx->post_body_len = (size_t)nread - (size_t)(ctx->post_body - ctx->req_buf);
    } else {
        ctx->post_body = "";
        ctx->post_body_len = 0;
    }

    get_json_string(ctx->req_buf, "url", ctx->target_url, sizeof(ctx->target_url));
    get_json_string(ctx->req_buf, "method", ctx->method, sizeof(ctx->method));
    get_json_string(ctx->req_buf, "ua", ctx->ua, sizeof(ctx->ua));
    get_json_string(ctx->req_buf, "client_name", ctx->client_name, sizeof(ctx->client_name));
    get_json_string(ctx->req_buf, "client_version", ctx->client_version, sizeof(ctx->client_version));
    get_json_string(ctx->req_buf, "visitor_id", ctx->visitor_id, sizeof(ctx->visitor_id));

    if (strncmp(ctx->target_url, "http://", 7) != 0 && strncmp(ctx->target_url, "https://", 8) != 0) {
        free(ctx);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"invalid_url\"}");
        return ESP_FAIL;
    }

    ctx->req = req;
    ctx->done_sem = xSemaphoreCreateBinary();
    StaticTask_t *tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    StackType_t *stack = (StackType_t *)heap_caps_aligned_alloc(16, HTTPS_WORKER_STACK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!ctx->done_sem || !tcb || !stack) {
        if (ctx->done_sem) vSemaphoreDelete(ctx->done_sem);
        if (tcb) heap_caps_free(tcb);
        if (stack) heap_caps_free(stack);
        free(ctx);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json; charset=utf-8");

    TaskHandle_t th = xTaskCreateStaticPinnedToCore(
        yt_relay_worker_task, "yt_rel", HTTPS_WORKER_STACK_SIZE, ctx, 4, stack, tcb, 1
    );
    if (!th) {
        vSemaphoreDelete(ctx->done_sem);
        heap_caps_free(tcb);
        heap_caps_free(stack);
        free(ctx);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    xSemaphoreTake(ctx->done_sem, portMAX_DELAY);
    vTaskDelete(th);
    vSemaphoreDelete(ctx->done_sem);
    heap_caps_free(stack);
    heap_caps_free(tcb);

    if (ctx->headers_sent) {
        httpd_resp_sendstr_chunk(req, NULL);
    } else if (ctx->perform_err == ESP_OK && ctx->status_code >= 200 && ctx->status_code < 300) {
        httpd_resp_sendstr(req, "{}");
    } else {
        char err_json[200];
        snprintf(err_json, sizeof(err_json), "{\"error\":\"relay_failed\",\"http_status\":%d,\"esp_err\":%d,\"tls_err\":%d,\"tls_flags\":%d}",
                 ctx->status_code, (int)ctx->perform_err, ctx->tls_err, ctx->tls_flags);
        httpd_resp_set_status(req, "502 Bad Gateway");
        httpd_resp_sendstr(req, err_json);
    }

    free(ctx);
    return ESP_OK;
}

typedef struct {
    httpd_req_t *req;
    SemaphoreHandle_t done_sem;
    char req_buf[3072];
    char target_url[2048];
    char rel_path[450];
    char abs_path[600];
    char display_title[128];
    char ua[256];
    char range_hdr[64];
    char ndjson_line[256];
    bool append;
    int idx;
    int count;
    uint64_t total_bytes;
    uint64_t batch_total;
    uint64_t offset_bytes;
    uint64_t written_bytes;
    uint8_t *bounce;
    size_t bounce_sz;
    size_t bounce_filled;
    FILE *fp;
    int64_t t_start_us;
    uint32_t last_report_ms;
    uint32_t fwrite_calls;
    int status_code;
    bool failed;
    bool client_aborted;
    char err_detail[128];
} pull_direct_ctx_t;

static esp_err_t pull_direct_http_event_handler(esp_http_client_event_t *evt)
{
    pull_direct_ctx_t *ctx = (pull_direct_ctx_t *)evt->user_data;
    if (!ctx || !ctx->req) return ESP_OK;

    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
        int status = esp_http_client_get_status_code(evt->client);
        if (status >= 300 && status < 400) {
            return ESP_OK;
        }
        ctx->status_code = status;
        if (status >= 400) {
            ctx->failed = true;
            snprintf(ctx->err_detail, sizeof(ctx->err_detail), "CDN retornou HTTP %d", status);
            return ESP_FAIL;
        }
        if (ctx->total_bytes == 0) {
            int64_t clen = esp_http_client_get_content_length(evt->client);
            if (clen > 0) {
                ctx->total_bytes = ctx->offset_bytes + (uint64_t)clen;
                s_progress_file_total = (size_t)ctx->total_bytes;
            }
        }

        const uint8_t *src = (const uint8_t *)evt->data;
        size_t rem = (size_t)evt->data_len;
        while (rem > 0) {
            size_t space = ctx->bounce_sz - ctx->bounce_filled;
            size_t to_copy = (rem < space) ? rem : space;
            memcpy(ctx->bounce + ctx->bounce_filled, src, to_copy);
            ctx->bounce_filled += to_copy;
            src += to_copy;
            rem -= to_copy;

            if (ctx->bounce_filled == ctx->bounce_sz) {
                size_t w = fwrite(ctx->bounce, 1, ctx->bounce_sz, ctx->fp);
                ctx->bounce_filled = 0;
                ctx->fwrite_calls++;
                if (w != ctx->bounce_sz) {
                    ctx->failed = true;
                    snprintf(ctx->err_detail, sizeof(ctx->err_detail), "Erro SD fwrite (%s)", strerror(errno));
                    return ESP_FAIL;
                }
                ctx->written_bytes += w;
                if ((ctx->fwrite_calls % 32) == 0) {
                    vTaskDelay(1);
                }
            }
        }

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if (now_ms - ctx->last_report_ms >= 250) {
            ctx->last_report_ms = now_ms;
            uint64_t cur_total_rx = ctx->offset_bytes + ctx->written_bytes + ctx->bounce_filled;
            progress_update((size_t)cur_total_rx);
            podcast_sync_notify_activity();

            float dt_s = (float)(esp_timer_get_time() - ctx->t_start_us) / 1000000.0f;
            float kbps = (dt_s > 0.05f) ? (((float)(ctx->written_bytes + ctx->bounce_filled) / 1024.0f) / dt_s) : 0.0f;
            snprintf(ctx->ndjson_line, sizeof(ctx->ndjson_line),
                     "{\"rx\":%llu,\"total\":%llu,\"speed_kbs\":%.1f}\n",
                     (unsigned long long)cur_total_rx,
                     (unsigned long long)ctx->total_bytes,
                     kbps);
            if (httpd_resp_sendstr_chunk(ctx->req, ctx->ndjson_line) != ESP_OK) {
                ctx->client_aborted = true;
                ctx->failed = true;
                snprintf(ctx->err_detail, sizeof(ctx->err_detail), "Cancelado pelo navegador");
                return ESP_FAIL;
            }
        }
    } else if (evt->event_id == HTTP_EVENT_ON_FINISH) {
        int status = esp_http_client_get_status_code(evt->client);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            esp_http_client_set_redirection(evt->client);
        }
    }
    return ESP_OK;
}

static void pull_direct_worker_task(void *arg)
{
    pull_direct_ctx_t *ctx = (pull_direct_ctx_t *)arg;
    ctx->t_start_us = esp_timer_get_time();

    mkdir_p_for_file(ctx->abs_path);
    ctx->fp = fopen(ctx->abs_path, ctx->append ? "ab" : "wb");
    if (!ctx->fp) {
        ctx->failed = true;
        snprintf(ctx->err_detail, sizeof(ctx->err_detail), "Falha ao abrir no SD (%s)", strerror(errno));
        xSemaphoreGive(ctx->done_sem);
        vTaskDelay(portMAX_DELAY);
        return;
    }
    setvbuf(ctx->fp, NULL, _IONBF, 0);

    bool is_gv = (strstr(ctx->target_url, "googlevideo.com") != NULL);
    int max_attempts = is_gv ? 3 : 8;

    for (int attempt = 0; attempt < max_attempts; attempt++) {
        if (ctx->client_aborted) break;
        if (attempt > 0) {
            ESP_LOGW(TAG, "pull_direct auto-resume (tentativa %d/%d, ja gravado=%llu B)...",
                     attempt + 1, max_attempts, (unsigned long long)ctx->written_bytes);
            vTaskDelay(pdMS_TO_TICKS(600));
            ctx->failed = false;
            ctx->err_detail[0] = '\0';
        }

        esp_http_client_config_t cfg = {
            .url = ctx->target_url,
            .method = HTTP_METHOD_GET,
            .timeout_ms = 30000,
            .buffer_size = 16384,
            .buffer_size_tx = 4096,
            .max_redirection_count = 8,
            .disable_auto_redirect = true,
            .skip_cert_common_name_check = false,
            .event_handler = pull_direct_http_event_handler,
            .user_data = ctx,
            .user_agent = ctx->ua[0] ? ctx->ua : "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15",
        };

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            ctx->failed = true;
            snprintf(ctx->err_detail, sizeof(ctx->err_detail), "Sem memoria para cliente HTTPS");
            break;
        }

        esp_http_client_set_header(client, "Accept", "*/*");
        char resume_range[64] = {0};
        if (ctx->written_bytes > 0 && !is_gv) {
            snprintf(resume_range, sizeof(resume_range), "bytes=%llu-",
                     (unsigned long long)(ctx->offset_bytes + ctx->written_bytes));
            esp_http_client_set_header(client, "Range", resume_range);
        } else if (ctx->range_hdr[0] != '\0') {
            esp_http_client_set_header(client, "Range", ctx->range_hdr);
        }

        uint64_t before_attempt = ctx->written_bytes;
        esp_err_t perr = esp_http_client_perform(client);
        ctx->status_code = esp_http_client_get_status_code(client);

        if (!ctx->client_aborted && ctx->bounce_filled > 0 && ctx->fp) {
            size_t w = fwrite(ctx->bounce, 1, ctx->bounce_filled, ctx->fp);
            if (w != ctx->bounce_filled) {
                ctx->failed = true;
                snprintf(ctx->err_detail, sizeof(ctx->err_detail), "Erro SD fwrite final (%s)", strerror(errno));
                ctx->bounce_filled = 0;
                esp_http_client_cleanup(client);
                break;
            } else {
                ctx->written_bytes += w;
            }
            ctx->bounce_filled = 0;
        }
        esp_http_client_cleanup(client);

        if (ctx->client_aborted) break;

        uint64_t total_rx = ctx->offset_bytes + ctx->written_bytes;
        bool reached_expected = (ctx->total_bytes > 0 && total_rx >= ctx->total_bytes);

        if (!ctx->failed && ((perr == ESP_OK && ctx->status_code >= 200 && ctx->status_code < 300 && ctx->written_bytes > 0) || reached_expected)) {
            ctx->failed = false;
            break;
        }

        // Se o CDN retornou 4xx definitivo (ex: 403/404) ou erro de SD, nao insiste em loop longo
        if (ctx->status_code >= 400 && ctx->status_code != 408 && ctx->status_code != 429 && ctx->status_code < 500) {
            ctx->failed = true;
            snprintf(ctx->err_detail, sizeof(ctx->err_detail), "Falha HTTPS (status=%d, err=%s)",
                     ctx->status_code, esp_err_to_name(perr));
            break;
        }

        // Se for googlevideo com &range= na query e ja escreveu parte sem terminar, reabre "wb" na proxima tentativa do fragmento se offset_bytes==0
        if (is_gv && ctx->written_bytes > before_attempt && !reached_expected) {
            if (!ctx->append && ctx->fp) {
                fclose(ctx->fp);
                ctx->fp = fopen(ctx->abs_path, "wb");
                ctx->written_bytes = 0;
            }
        }

        ctx->failed = true;
        snprintf(ctx->err_detail, sizeof(ctx->err_detail), "Falha HTTPS (status=%d, err=%s)",
                 ctx->status_code, esp_err_to_name(perr));
    }

    if (ctx->fp) {
        fclose(ctx->fp);
        ctx->fp = NULL;
    }

    xSemaphoreGive(ctx->done_sem);
    vTaskDelay(portMAX_DELAY);
}

static esp_err_t api_sync_pull_direct_post_handler(httpd_req_t *req)
{
    set_cors_pna_headers(req);
    if (!check_api_key(req)) return ESP_FAIL;
    podcast_sync_notify_activity();

    if (s_ota_in_progress) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA em andamento");
        return ESP_FAIL;
    }

    audio_player_stop_url();

    pull_direct_ctx_t *ctx = (pull_direct_ctx_t *)http_scratch_alloc(sizeof(pull_direct_ctx_t));
    if (!ctx) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (http_recv_body_full(req, ctx->req_buf, sizeof(ctx->req_buf)) <= 0) {
        free(ctx);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"empty_body\"}");
        return ESP_FAIL;
    }

    get_json_string(ctx->req_buf, "url", ctx->target_url, sizeof(ctx->target_url));
    get_json_string(ctx->req_buf, "staging_path", ctx->rel_path, sizeof(ctx->rel_path));
    get_json_string(ctx->req_buf, "title", ctx->display_title, sizeof(ctx->display_title));
    get_json_string(ctx->req_buf, "ua", ctx->ua, sizeof(ctx->ua));
    get_json_string(ctx->req_buf, "range", ctx->range_hdr, sizeof(ctx->range_hdr));
    ctx->append = (get_json_uint64(ctx->req_buf, "append", 0) != 0);
    ctx->idx = (int)get_json_uint64(ctx->req_buf, "idx", 1);
    ctx->count = (int)get_json_uint64(ctx->req_buf, "count", 1);
    ctx->total_bytes = get_json_uint64(ctx->req_buf, "total_bytes", 0);
    ctx->batch_total = get_json_uint64(ctx->req_buf, "batch_total", 0);
    ctx->offset_bytes = get_json_uint64(ctx->req_buf, "offset_bytes", 0);

    if ((strncmp(ctx->target_url, "http://", 7) != 0 && strncmp(ctx->target_url, "https://", 8) != 0) ||
        ctx->rel_path[0] == '\0' || !build_abs_path(ctx->rel_path, ctx->abs_path, sizeof(ctx->abs_path))) {
        free(ctx);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"invalid_params\"}");
        return ESP_FAIL;
    }

    if (!s_upload_lock) {
        s_upload_lock = xSemaphoreCreateMutex();
    }
    if (!s_upload_lock || xSemaphoreTake(s_upload_lock, pdMS_TO_TICKS(15000)) != pdTRUE) {
        free(ctx);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"upload_busy\"}");
        return ESP_FAIL;
    }
    s_upload_io_busy = true;
    transfer_boost_begin();
    ensure_transfer_pipeline_ready();

    const char *slash = strrchr(ctx->rel_path, '/');
    const char *file_part = slash ? (slash + 1) : ctx->rel_path;
    const char *display_name = (ctx->display_title[0] != '\0') ? ctx->display_title : file_part;
    if (!ctx->append) {
        progress_begin(display_name, (size_t)ctx->total_bytes, ctx->idx, ctx->count, (long long)ctx->batch_total);
    }

    bool bounce_dyn = false;
    if (s_bounce_persist && s_bounce_persist_sz >= 8192) {
        ctx->bounce = s_bounce_persist;
        ctx->bounce_sz = s_bounce_persist_sz;
    } else {
        bounce_dyn = true;
        ctx->bounce_sz = 32768;
        ctx->bounce = (uint8_t *)heap_caps_malloc(ctx->bounce_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    ctx->req = req;
    ctx->done_sem = xSemaphoreCreateBinary();
    StaticTask_t *tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    StackType_t *stack = (StackType_t *)heap_caps_aligned_alloc(16, HTTPS_WORKER_STACK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!ctx->bounce || !ctx->done_sem || !tcb || !stack) {
        if (bounce_dyn && ctx->bounce) heap_caps_free(ctx->bounce);
        if (ctx->done_sem) vSemaphoreDelete(ctx->done_sem);
        if (tcb) heap_caps_free(tcb);
        if (stack) heap_caps_free(stack);
        transfer_boost_end();
        s_upload_io_busy = false;
        xSemaphoreGive(s_upload_lock);
        progress_end();
        free(ctx);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/x-ndjson; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    TaskHandle_t th = xTaskCreateStaticPinnedToCore(
        pull_direct_worker_task, "pull_sd", HTTPS_WORKER_STACK_SIZE, ctx, 4, stack, tcb, 1
    );
    if (!th) {
        if (bounce_dyn && ctx->bounce) heap_caps_free(ctx->bounce);
        vSemaphoreDelete(ctx->done_sem);
        heap_caps_free(tcb);
        heap_caps_free(stack);
        transfer_boost_end();
        s_upload_io_busy = false;
        xSemaphoreGive(s_upload_lock);
        progress_end();
        free(ctx);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    xSemaphoreTake(ctx->done_sem, portMAX_DELAY);
    vTaskDelete(th);
    vSemaphoreDelete(ctx->done_sem);
    heap_caps_free(stack);
    heap_caps_free(tcb);
    if (bounce_dyn && ctx->bounce) heap_caps_free(ctx->bounce);

    transfer_boost_end();
    s_upload_io_busy = false;
    xSemaphoreGive(s_upload_lock);

    if (!ctx->client_aborted) {
        if (!ctx->failed) {
            uint64_t final_rx = ctx->offset_bytes + ctx->written_bytes;
            if (ctx->total_bytes == 0 || final_rx >= ctx->total_bytes) {
                progress_finish_file((size_t)final_rx);
                if (ctx->idx >= ctx->count) progress_end();
            } else {
                progress_update((size_t)final_rx);
            }
            s_files_received++;
            snprintf(ctx->ndjson_line, sizeof(ctx->ndjson_line),
                     "{\"done\":true,\"status\":\"ok\",\"bytes\":%llu,\"total\":%llu}\n",
                     (unsigned long long)ctx->written_bytes,
                     (unsigned long long)final_rx);
        } else {
            progress_end();
            snprintf(ctx->ndjson_line, sizeof(ctx->ndjson_line),
                     "{\"done\":true,\"status\":\"error\",\"error\":\"%s\",\"http_status\":%d}\n",
                     ctx->err_detail[0] ? ctx->err_detail : "download_failed", ctx->status_code);
        }
        httpd_resp_sendstr_chunk(req, ctx->ndjson_line);
        httpd_resp_sendstr_chunk(req, NULL);
    } else {
        progress_end();
    }

    free(ctx);
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
    set_cors_pna_headers(req);
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

    transfer_boost_begin();
    progress_begin("Benchmark SINK", total_len, 1, 1, (long long)total_len);

    int64_t t0 = esp_timer_get_time();
    int timeout_count = 0;
    uint32_t recv_calls = 0;
    int64_t recv_max_us = 0;
    uint32_t last_prog_ms = 0;

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

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if (now_ms - last_prog_ms >= 50) {
            last_prog_ms = now_ms;
            progress_update(received_total);
            podcast_sync_notify_activity();
            vTaskDelay(1);
        }
    }
    int64_t t1 = esp_timer_get_time();
    free(rx_buf);

    progress_update(received_total);
    progress_finish_file(total_len);
    progress_end();
    transfer_boost_end();

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
    set_cors_pna_headers(req);
    char query[64] = {0};
    char size_str[32] = {0};
    size_t total_send = 10 * 1024 * 1024; // 10 MB padrao
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "size", size_str, sizeof(size_str)) == ESP_OK) {
            unsigned long long val = strtoull(size_str, NULL, 10);
            if (val >= 1024ULL && val <= 500ULL * 1024ULL * 1024ULL) total_send = (size_t)val;
        }
    }

    {
        char tx_str[16] = {0};
        if (httpd_query_key_value(query, "tx", tx_str, sizeof(tx_str)) == ESP_OK) {
            int txq = atoi(tx_str);
            if (txq >= 8 && txq <= 84) {
                esp_err_t te = esp_wifi_set_max_tx_power((int8_t)txq);
                ESP_LOGW(TAG, "[BENCH] TX power -> %d (0.25 dBm) err=%s", txq, esp_err_to_name(te));
            }
        }
    }
    int64_t src_diag_us = esp_timer_get_time();
    transfer_heap_diag("source_begin");
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Connection", "close");
    {
        int sfd = httpd_req_to_sockfd(req);
        if (sfd >= 0) {
            int nodelay = 1;
            setsockopt(sfd, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
            int sndbuf = 65536;
            setsockopt(sfd, SOL_SOCKET, SO_SNDBUF, (const char *)&sndbuf, sizeof(sndbuf));
            struct timeval tv_src = { .tv_sec = 8, .tv_usec = 0 };
            setsockopt(sfd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv_src, sizeof(tv_src));
        }
    }
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

    transfer_boost_begin();
    progress_begin("Benchmark SOURCE", total_send, 1, 1, (long long)total_send);

    size_t sent = 0;
    uint32_t last_prog_ms = 0;
    while (sent < total_send) {
        size_t to_send = total_send - sent;
        if (to_send > chunk_sz) to_send = chunk_sz;
        if (httpd_resp_send_chunk(req, chunk, to_send) != ESP_OK) {
            ESP_LOGW(TAG, "[BENCH] source: send_chunk falhou apos %u bytes", (unsigned)sent);
            transfer_heap_diag("source_fail");
            free(chunk);
            httpd_resp_sendstr_chunk(req, NULL);
            progress_finish_file(sent);
            progress_end();
            transfer_boost_end();
            return ESP_FAIL;
        }
        sent += to_send;

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if (now_ms - last_prog_ms >= 50) {
            last_prog_ms = now_ms;
            progress_update(sent);
            podcast_sync_notify_activity();
            vTaskDelay(1);
        }

        if (esp_timer_get_time() - src_diag_us > 1000000LL) {
            src_diag_us = esp_timer_get_time();
            ESP_LOGI(TAG, "[BENCH] source enviado=%u KB", (unsigned)(sent / 1024));
            transfer_heap_diag("source_1s");
        }
    }
    transfer_heap_diag("source_end");
    free(chunk);
    httpd_resp_sendstr_chunk(req, NULL);

    progress_update(sent);
    progress_finish_file(total_send);
    progress_end();
    transfer_boost_end();

    return ESP_OK;
}

static esp_err_t api_screen_get_handler(httpd_req_t *req)
{
    set_cors_pna_headers(req);
    uint8_t *fb = (uint8_t *)malloc(1024);
    if (!fb) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    if (oled_display_capture_buffer(fb) != ESP_OK) {
        free(fb);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, (const char *)fb, 1024);
    free(fb);
    return ESP_OK;
}

static esp_err_t api_bench_sd_clock_handler(httpd_req_t *req)
{
    set_cors_pna_headers(req);
    char query[96] = {0};
    char freq_str[32] = {0};
    char khz_str[32] = {0};
    char mode_str[32] = {0};
    uint32_t freq_khz = SDMMC_FREQ_DEFAULT;
    bool freq_set = false;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "mode", mode_str, sizeof(mode_str)) == ESP_OK) {
            int m = atoi(mode_str);
            wifi_transfer_set_sd_clock_mode(m);
            if (m == 0 || m == 1) {
                freq_khz = SDMMC_FREQ_DEFAULT;
                freq_set = true;
            } else if (m == 2) {
                freq_khz = SDMMC_FREQ_HIGHSPEED;
                freq_set = true;
            }
        }
        if (httpd_query_key_value(query, "khz", khz_str, sizeof(khz_str)) == ESP_OK) {
            int khz = atoi(khz_str);
            freq_khz = (khz >= 30000) ? SDMMC_FREQ_HIGHSPEED : SDMMC_FREQ_DEFAULT;
            freq_set = true;
            if (mode_str[0] == '\0') {
                wifi_transfer_set_sd_clock_mode((freq_khz == SDMMC_FREQ_HIGHSPEED) ? 2 : 0);
            }
        }
        if (httpd_query_key_value(query, "freq", freq_str, sizeof(freq_str)) == ESP_OK) {
            int mhz = atoi(freq_str);
            freq_khz = (mhz >= 30) ? SDMMC_FREQ_HIGHSPEED : SDMMC_FREQ_DEFAULT;
            freq_set = true;
            if (mode_str[0] == '\0') {
                wifi_transfer_set_sd_clock_mode((freq_khz == SDMMC_FREQ_HIGHSPEED) ? 2 : 0);
            }
        }
    }
    if (freq_set) {
        sd_card_set_frequency(freq_khz);
    } else {
        uint8_t ch = 0;
        int rssi = 0;
        get_wifi_radio_telemetry(&ch, &rssi);
        (void)apply_transfer_sd_clock_start(ch, false);
        freq_khz = sd_card_get_frequency();
    }
    char resp[160];
    snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"freq_khz\":%u,\"freq_mhz\":%.1f,\"mode\":%d}",
             (unsigned)freq_khz, (double)freq_khz / 1000.0, wifi_transfer_get_sd_clock_mode());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t api_bench_oled_fps_handler(httpd_req_t *req)
{
    set_cors_pna_headers(req);
    char query[64] = {0};
    char fps_str[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "fps", fps_str, sizeof(fps_str)) == ESP_OK) {
            int fps = atoi(fps_str);
            wifi_transfer_set_display_fps(fps);
        }
    }
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"fps\":%d,\"interval_ms\":%u}",
             wifi_transfer_get_display_fps(),
             (unsigned)wifi_transfer_get_display_interval_ms());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static esp_err_t api_analytics_manifest_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    set_cors_pna_headers(req);

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

static esp_err_t api_qr_svg_get_handler(httpd_req_t *req)
{
    typedef struct {
        char query[384];
        char text_enc[280];
        char text[256];
        QRCode qr;
        uint8_t modules[512];
        char chunk[512];
    } qr_scratch_t;

    qr_scratch_t *sc = (qr_scratch_t *)http_scratch_alloc(sizeof(qr_scratch_t));
    if (!sc) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    if (httpd_req_get_url_query_str(req, sc->query, sizeof(sc->query)) != ESP_OK ||
        httpd_query_key_value(sc->query, "text", sc->text_enc, sizeof(sc->text_enc)) != ESP_OK) {
        free(sc);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "faltou o parametro text");
        return ESP_FAIL;
    }
    url_decode(sc->text, sc->text_enc, sizeof(sc->text));
    if (sc->text[0] == '\0') {
        free(sc);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "texto vazio");
        return ESP_FAIL;
    }
    static const uint16_t CAP_M[11] = {0, 14, 26, 42, 62, 84, 106, 122, 152, 180, 213};
    static const uint16_t CAP_L[11] = {0, 17, 32, 53, 78, 106, 134, 154, 192, 230, 271};
    size_t tlen = strlen(sc->text);
    bool ok = false;
    for (uint8_t v = 2; v <= 10; v++) {
        if (tlen <= CAP_M[v] && qrcode_getBufferSize(v) <= sizeof(sc->modules) &&
            qrcode_initText(&sc->qr, sc->modules, v, ECC_MEDIUM, sc->text) == 0) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        for (uint8_t v = 2; v <= 10; v++) {
            if (tlen <= CAP_L[v] && qrcode_getBufferSize(v) <= sizeof(sc->modules) &&
                qrcode_initText(&sc->qr, sc->modules, v, ECC_LOW, sc->text) == 0) {
                ok = true;
                break;
            }
        }
    }
    if (!ok) {
        free(sc);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "payload excede capacidade QR");
        return ESP_FAIL;
    }
    int margin = 2;
    int view_sz = sc->qr.size + margin * 2;
    httpd_resp_set_type(req, "image/svg+xml");
    set_cors_pna_headers(req);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    snprintf(sc->chunk, sizeof(sc->chunk),
             "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %d %d\" shape-rendering=\"crispEdges\">"
             "<rect width=\"%d\" height=\"%d\" fill=\"#ffffff\"/><path fill=\"#000000\" d=\"",
             view_sz, view_sz, view_sz, view_sz);
    httpd_resp_sendstr_chunk(req, sc->chunk);
    size_t pos = 0;
    for (uint8_t y = 0; y < sc->qr.size; y++) {
        for (uint8_t x = 0; x < sc->qr.size; x++) {
            if (qrcode_getModule(&sc->qr, x, y)) {
                int n = snprintf(sc->chunk + pos, sizeof(sc->chunk) - pos, "M%d,%dh1v1h-1z", (int)x + margin, (int)y + margin);
                if (n > 0) pos += (size_t)n;
                if (pos > sizeof(sc->chunk) - 48) {
                    sc->chunk[pos] = '\0';
                    httpd_resp_sendstr_chunk(req, sc->chunk);
                    pos = 0;
                }
            }
        }
    }
    if (pos > 0) {
        sc->chunk[pos] = '\0';
        httpd_resp_sendstr_chunk(req, sc->chunk);
    }
    httpd_resp_sendstr_chunk(req, "\"/></svg>");
    httpd_resp_sendstr_chunk(req, NULL);
    free(sc);
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
    config.uri_match_fn = httpd_uri_match_wildcard;
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
    httpd_uri_t upload_stats_uri = { .uri = "/api/upload/stats", .method = HTTP_GET, .handler = api_upload_stats_get_handler };
    httpd_uri_t download_stats_uri = { .uri = "/api/download/stats", .method = HTTP_GET, .handler = api_download_stats_get_handler };
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
    httpd_register_uri_handler(s_httpd, &upload_stats_uri);
    httpd_register_uri_handler(s_httpd, &download_stats_uri);
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
    httpd_uri_t yt_innertube_uri    = { .uri = "/api/yt/innertube",    .method = HTTP_POST, .handler = api_yt_innertube_post_handler };
    httpd_uri_t sync_pull_direct_uri = { .uri = "/api/sync/pull_direct", .method = HTTP_POST, .handler = api_sync_pull_direct_post_handler };
    httpd_register_uri_handler(s_httpd, &sync_manifest_uri);
    httpd_register_uri_handler(s_httpd, &sync_verify_uri);
    httpd_register_uri_handler(s_httpd, &sync_begin_uri);
    httpd_register_uri_handler(s_httpd, &sync_done_uri);
    httpd_register_uri_handler(s_httpd, &podcasts_played_uri);
    httpd_register_uri_handler(s_httpd, &yt_innertube_uri);
    httpd_register_uri_handler(s_httpd, &sync_pull_direct_uri);

    httpd_uri_t bench_sink_uri = { .uri = "/api/bench_sink", .method = HTTP_POST, .handler = api_bench_sink_handler };
    httpd_uri_t bench_source_uri = { .uri = "/api/bench_source", .method = HTTP_GET, .handler = api_bench_source_handler };
    httpd_uri_t analytics_manifest_uri = { .uri = "/api/analytics/manifest", .method = HTTP_GET, .handler = api_analytics_manifest_get_handler };
    httpd_uri_t qr_svg_uri = { .uri = "/api/qr", .method = HTTP_GET, .handler = api_qr_svg_get_handler };
    httpd_register_uri_handler(s_httpd, &bench_sink_uri);
    httpd_register_uri_handler(s_httpd, &bench_source_uri);
    httpd_register_uri_handler(s_httpd, &analytics_manifest_uri);
    httpd_register_uri_handler(s_httpd, &qr_svg_uri);

    httpd_uri_t bench_sd_clock_uri = { .uri = "/api/bench/sd_clock", .method = HTTP_POST, .handler = api_bench_sd_clock_handler };
    httpd_register_uri_handler(s_httpd, &bench_sd_clock_uri);

    httpd_uri_t bench_oled_fps_uri = { .uri = "/api/bench/oled_fps", .method = HTTP_POST, .handler = api_bench_oled_fps_handler };
    httpd_uri_t bench_oled_fps_get_uri = { .uri = "/api/bench/oled_fps", .method = HTTP_GET, .handler = api_bench_oled_fps_handler };
    httpd_register_uri_handler(s_httpd, &bench_oled_fps_uri);
    httpd_register_uri_handler(s_httpd, &bench_oled_fps_get_uri);

    httpd_uri_t screen_get_uri = { .uri = "/api/screen", .method = HTTP_GET, .handler = api_screen_get_handler };
    httpd_register_uri_handler(s_httpd, &screen_get_uri);

    httpd_uri_t api_options_uri = { .uri = "/api/*", .method = HTTP_OPTIONS, .handler = api_options_preflight_handler };
    httpd_register_uri_handler(s_httpd, &api_options_uri);

    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, http_404_error_handler);

    // Pre-aquece o pipeline persistente de transferencia (bounce DMA 64KB, RingBuffer 4MB PSRAM e worker Core 1)
    ensure_transfer_pipeline_ready();

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
    wifi_config.sta.threshold.authmode = strlen(s_candidates[s_candidate_idx].pass) ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

    ESP_LOGI(TAG, "Tentando rede conhecida %d/%d: SSID=%s",
             s_candidate_idx + 1, s_candidate_count, s_candidates[s_candidate_idx].ssid);

    s_sta_retry = 0;
    s_got_ip = false;
    return esp_wifi_connect();
}

typedef enum {
    WIFI_SUBSTATE_SUBMENU = 0,
    WIFI_SUBSTATE_AP_ACTIVE,
    WIFI_SUBSTATE_STA_ACTIVE,
    WIFI_SUBSTATE_SCAN_LIST,
    WIFI_SUBSTATE_KEYBOARD,
    WIFI_SUBSTATE_MANAGE_SAVED,
    WIFI_SUBSTATE_NETWORK_ACTION
} wifi_substate_t;

static wifi_substate_t s_wifi_substate = WIFI_SUBSTATE_SUBMENU;
static bool s_wifi_menu_open = false;
static bool s_sd_released_for_wifi = false;
static int s_submenu_cursor = 0;
static int s_scan_cursor = 0;
static int s_scan_top_idx = 0;
static char s_selected_ssid[33] = {0};
static char s_kb_input[65] = {0};
static int s_kb_r = 1;
static int s_kb_c = 0;
static int s_kb_shift = 0;
static int s_manage_cursor = 0;
static int s_manage_top_idx = 0;
static int s_action_cursor = 0;
static bool s_ap_qr_mode = false;

static bool s_is_scanning = false;
static wifi_ap_record_t s_scan_records[12];
static uint16_t s_scan_count = 0;
static esp_timer_handle_t s_sta_reconnect_timer = NULL;

static void wifi_acquire_sd_and_pause_audio(void)
{
    playback_state_t st;
    audio_player_get_state(&st);
    if (st.playing && !st.paused) {
        audio_player_toggle_play_pause();
    }
    if (!s_sd_released_for_wifi) {
        audio_player_release_sd_for_usb();
        sd_card_set_frequency(SDMMC_FREQ_DEFAULT);
        s_sd_released_for_wifi = true;
    }
    if (i2s_output_is_initialized()) {
        ESP_LOGI(TAG, "Liberando buffers DMA do I2S (~96 KB) para o subsistema Wi-Fi/SDMMC...");
        i2s_output_deinit();
    }
}

static void wifi_release_sd_to_audio(bool full_remount)
{
    if (sd_card_get_frequency() != SDMMC_FREQ_HIGHSPEED) {
        sd_card_set_frequency(SDMMC_FREQ_HIGHSPEED);
    }
    if (!s_sd_released_for_wifi) return;
    if (full_remount || s_files_received > 0) {
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
    } else {
        sd_card_set_frequency(SDMMC_FREQ_HIGHSPEED);
    }
    // Aguarda o encerramento assincrono do driver Wi-Fi para liberar memoria DMA
    vTaskDelay(pdMS_TO_TICKS(100));

    if (!i2s_output_is_initialized()) {
        i2s_output_init();
    }

    audio_player_reacquire_sd_after_usb();
    s_sd_released_for_wifi = false;
}

static void sta_reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (!s_active || s_is_scanning ||
        s_wifi_substate == WIFI_SUBSTATE_SCAN_LIST ||
        s_wifi_substate == WIFI_SUBSTATE_KEYBOARD ||
        (s_mode != WIFI_TRANSFER_MODE_STA && s_mode != WIFI_TRANSFER_MODE_APSTA)) {
        return;
    }
    if (s_got_ip) {
        return;
    }
    if (s_sta_retry < WIFI_STA_MAX_RETRY) {
        s_sta_retry++;
        ESP_LOGI(TAG, "Reconectando STA (tentativa %d/%d)...", s_sta_retry, WIFI_STA_MAX_RETRY);
        esp_wifi_connect();
    } else {
        // Avanca pro proximo candidato ou cicla de volta para o primeiro candidato conhecido
        if (s_candidate_count > 0) {
            s_candidate_idx = (s_candidate_idx + 1) % s_candidate_count;
        }
        s_sta_retry = 0;
        ESP_LOGI(TAG, "Ciclando para proximo candidato STA (%d/%d)...", s_candidate_idx + 1, s_candidate_count);
        apply_sta_candidate();
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            if (!s_is_scanning &&
                s_wifi_substate != WIFI_SUBSTATE_SCAN_LIST &&
                s_wifi_substate != WIFI_SUBSTATE_KEYBOARD) {
                esp_wifi_connect();
            }
        } else if (event_id == WIFI_EVENT_SCAN_DONE) {
            uint16_t ap_count = 0;
            esp_wifi_scan_get_ap_num(&ap_count);
            if (ap_count > 24) ap_count = 24;
            if (ap_count > 0) {
                wifi_ap_record_t *raw_aps = (wifi_ap_record_t *)heap_caps_malloc(
                    ap_count * sizeof(wifi_ap_record_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!raw_aps) {
                    raw_aps = (wifi_ap_record_t *)malloc(ap_count * sizeof(wifi_ap_record_t));
                }
                if (raw_aps) {
                    uint16_t fetch_n = ap_count;
                    if (esp_wifi_scan_get_ap_records(&fetch_n, raw_aps) == ESP_OK) {
                        uint16_t unique_n = 0;
                        for (uint16_t i = 0; i < fetch_n && unique_n < 12; i++) {
                            if (raw_aps[i].ssid[0] == '\0') continue; // Ignora redes ocultas/vazias
                            bool dup = false;
                            for (uint16_t j = 0; j < unique_n; j++) {
                                if (strcmp((const char *)s_scan_records[j].ssid, (const char *)raw_aps[i].ssid) == 0) {
                                    dup = true;
                                    break;
                                }
                            }
                            if (!dup) {
                                s_scan_records[unique_n++] = raw_aps[i];
                            }
                        }
                        s_scan_count = unique_n;
                    }
                    free(raw_aps);
                } else {
                    if (ap_count > 12) ap_count = 12;
                    s_scan_count = ap_count;
                    esp_wifi_scan_get_ap_records(&s_scan_count, s_scan_records);
                }
            } else {
                s_scan_count = 0;
            }
            if (s_scan_cursor >= (int)s_scan_count) {
                s_scan_cursor = (s_scan_count > 0) ? ((int)s_scan_count - 1) : 0;
            }
            s_is_scanning = false;
            ESP_LOGI(TAG, "Wi-Fi scan concluido: %d redes unicas encontradas", (int)s_scan_count);
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            if (s_is_scanning ||
                s_wifi_substate == WIFI_SUBSTATE_SCAN_LIST ||
                s_wifi_substate == WIFI_SUBSTATE_KEYBOARD) {
                return;
            }
            wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
            uint8_t reason = disc ? disc->reason : 0;
            s_got_ip = false;
            if (s_bw_is_ht40) {
                ESP_LOGW(TAG, "STA desconectou em HT40. Recuando para HT20 no proximo retry...");
                esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);
                s_bw_is_ht40 = false;
            }
            ESP_LOGW(TAG, "STA desconectou (reason=%u). Tentativa %d/%d (Candidato %d/%d: SSID=%s)",
                     reason, s_sta_retry + 1, WIFI_STA_MAX_RETRY,
                     s_candidate_idx + 1, s_candidate_count,
                     (s_candidate_idx < s_candidate_count) ? s_candidates[s_candidate_idx].ssid : "N/A");
            if (s_active && (s_mode == WIFI_TRANSFER_MODE_STA || s_mode == WIFI_TRANSFER_MODE_APSTA)) {
                if (!s_sta_reconnect_timer) {
                    const esp_timer_create_args_t timer_args = {
                        .callback = &sta_reconnect_timer_cb,
                        .name = "sta_recon_tmr"
                    };
                    esp_timer_create(&timer_args, &s_sta_reconnect_timer);
                }
                if (s_sta_reconnect_timer) {
                    esp_timer_stop(s_sta_reconnect_timer);
                    // Backoff inteligente: se o AP desassociou por rate-limit ou inatividade (reason=4, 205, 2),
                    // aguarda 2.5s para que o roteador limpe a tabela de estados.
                    // Caso contrario, aguarda 1.0s.
                    uint64_t delay_us = (reason == 4 || reason == 205 || reason == 2) ? 2500000ULL : 1000000ULL;
                    if (s_sta_retry >= WIFI_STA_MAX_RETRY) {
                        // Ao esgotar as tentativas e chavear de rede, aguarda 5s de cooldown
                        delay_us = 5000000ULL;
                    }
                    esp_timer_start_once(s_sta_reconnect_timer, delay_us);
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

        if (s_sta_reconnect_timer) {
            esp_timer_stop(s_sta_reconnect_timer);
        }

        if (s_netif_sta) {
            esp_netif_dns_info_t dns_bk = {0};
            dns_bk.ip.u_addr.ip4.addr = ESP_IP4TOADDR(8, 8, 8, 8);
            dns_bk.ip.type = ESP_IPADDR_TYPE_V4;
            esp_netif_set_dns_info(s_netif_sta, ESP_NETIF_DNS_BACKUP, &dns_bk);
            esp_netif_dns_info_t dns_fb = {0};
            dns_fb.ip.u_addr.ip4.addr = ESP_IP4TOADDR(1, 1, 1, 1);
            dns_fb.ip.type = ESP_IPADDR_TYPE_V4;
            esp_netif_set_dns_info(s_netif_sta, ESP_NETIF_DNS_FALLBACK, &dns_fb);
        }

        // Transiciona automaticamente o OLED para a tela de transferencia ativa
        s_wifi_substate = (s_mode == WIFI_TRANSFER_MODE_AP) ? WIFI_SUBSTATE_AP_ACTIVE : WIFI_SUBSTATE_STA_ACTIVE;
        s_wifi_menu_open = true;

        // Com o link STA associado, desativa modem sleep, aplica potencia maxima (84 = 21 dBm)
        // e fixa HT20 (20 MHz) livre de interferencia de coexistencia 2.4 GHz (~2.46 MB/s sustentados)
        esp_wifi_set_ps(WIFI_PS_NONE);
        esp_wifi_set_max_tx_power(76); /* 19.0 dBm: alta velocidade calibrada (1.67 MB/s) sem brownout */
        // Tenta negociar canal largo HT40 (40 MHz, ate 150 Mbps PHY)
        esp_err_t bw_err = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW40);
        if (bw_err == ESP_OK) {
            wifi_bandwidth_t cur_bw = WIFI_BW20;
            esp_wifi_get_bandwidth(WIFI_IF_STA, &cur_bw);
            s_bw_is_ht40 = (cur_bw == WIFI_BW40);
            ESP_LOGI(TAG, "Largura de banda Wi-Fi negociada: %s", s_bw_is_ht40 ? "HT40 (40 MHz - 150 Mbps)" : "HT20 (20 MHz - fallback AP)");
        } else {
            esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);
            s_bw_is_ht40 = false;
            ESP_LOGW(TAG, "esp_wifi_set_bandwidth HT40 falhou (%s), recuando para HT20", esp_err_to_name(bw_err));
        }

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

    // NUNCA alocar buffers auxiliares antes de esp_wifi_init para garantir 100% de DRAM aos descritores do radio Wi-Fi
    // Com CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y, os buffers dinamicos RX/TX ficam na PSRAM (8 MB),
    // enquanto static_rx_buf_num fica na SRAM interna.
    // NUNCA alocar buffers auxiliares antes do esp_wifi_init para garantir que o driver Wi-Fi
    // tenha toda a SRAM interna DMA contígua necessária para inicializar seus descritores e tarefas.
    // Configuracao Wi-Fi otimizada para PSRAM Octal + SRAM DMA preservada para Beacon/AP/Probe
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.static_rx_buf_num = 4;   // Reduz de 8 para 4 buffers RX em SRAM interna (poupa 6.4 KB de SRAM)
    cfg.dynamic_rx_buf_num = 32; // Buffers dinamicos alocados em PSRAM Octal (8 MB)
    cfg.tx_buf_type = 1;        // Buffers de transmissao dinamicos alocados em PSRAM Octal
    cfg.static_tx_buf_num = 0;  // Zero buffers estaticos em DRAM interna (poupa 12.8 KB de SRAM contigua)
    cfg.dynamic_tx_buf_num = 32;
    cfg.cache_tx_buf_num = 16;
    cfg.rx_mgmt_buf_type = 0;
    cfg.rx_ba_win = 16;
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

    // Nao pre-aloca s_bounce_persist aqui para nao canibalizar a DRAM contigua do beacon softAP e probe requests!
    // O handle_file_upload_stream ja aloca o bounce buffer dinamicamente sob demanda quando o upload comeca.

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
    if (s_sta_reconnect_timer) {
        esp_timer_stop(s_sta_reconnect_timer);
    }
    // Se havia uma sessao HTTP residual, encerra antes de comecar
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    stop_mdns_service();

    if (s_active) {
        esp_wifi_scan_stop();
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_active = false;
        s_is_scanning = false;
    }

    if (mode == WIFI_TRANSFER_MODE_STA || mode == WIFI_TRANSFER_MODE_APSTA) {
        s_candidate_count = build_sta_candidates();
        s_candidate_idx = 0;
        if (s_candidate_count == 0 && mode == WIFI_TRANSFER_MODE_STA) {
            strncpy(s_status, "Sem redes salvas", sizeof(s_status) - 1);
            s_status[sizeof(s_status) - 1] = '\0';
            return ESP_ERR_INVALID_STATE;
        }
    }

    wifi_acquire_sd_and_pause_audio();

    esp_err_t err = ensure_stack_ready();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar o WiFi: %s", esp_err_to_name(err));
        wifi_release_sd_to_audio(false);
        return err;
    }

    s_mode = mode;
    s_files_received = 0;
    s_exit_requested = false;
    s_got_ip = false;
    s_ap_ready = false;
    s_sta_failed = false;
    s_sta_retry = 0;
    s_is_scanning = false;
    s_auto_fallback = false;
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
            sta_config.sta.threshold.authmode = strlen(s_candidates[s_candidate_idx].pass) ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
            sta_config.sta.pmf_cfg.capable = true;
            sta_config.sta.pmf_cfg.required = false;

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

    // Define subestado e flags ANTES de esp_wifi_start() para que WIFI_EVENT_STA_START
    // (que preempta esta task imediatamente) enxergue o estado correto e dispare esp_wifi_connect().
    s_active = true;
    s_wifi_substate = (mode == WIFI_TRANSFER_MODE_AP) ? WIFI_SUBSTATE_AP_ACTIVE : WIFI_SUBSTATE_STA_ACTIVE;
    s_wifi_menu_open = true;

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao ligar o radio WiFi: %s", esp_err_to_name(err));
        s_active = false;
        wifi_release_sd_to_audio(false);
        return err;
    }

    // No modo de transferencia WiFi, desativa o modem sleep para resposta TCP instantanea:
    esp_wifi_set_ps(WIFI_PS_NONE);
    // Potencia calibrada em 19.0 dBm (76) para maximo ganho sem brownouts:
    esp_wifi_set_max_tx_power(76); /* 19.0 dBm: alta velocidade calibrada (1.67 MB/s) sem brownout */

    if (mode == WIFI_TRANSFER_MODE_STA) {
        esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW40);
        esp_wifi_connect();
    } else {
        esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20);
        if (mode == WIFI_TRANSFER_MODE_APSTA && s_candidate_count > 0) {
            esp_wifi_connect();
        }
    }

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
    if (!s_active) {
        if (s_exit_requested && !s_ota_in_progress) {
            s_exit_requested = false;
            s_wifi_menu_open = false;
            s_wifi_substate = WIFI_SUBSTATE_SUBMENU;
            wifi_release_sd_to_audio(false);
            return true;
        }
        return false;
    }

    update_ui_state();

    static uint32_t s_last_httpd_attempt_ms = 0;
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    // Sondagem periódica de promoção HT20 -> HT40 (a cada 30s quando ocioso)
    static uint32_t s_last_ht40_probe_ms = 0;
    if (s_got_ip && !s_bw_is_ht40 && !s_upload_io_busy && !wifi_transfer_is_boost_active() && !s_ota_in_progress && (now_ms - s_last_ht40_probe_ms >= 30000)) {
        s_last_ht40_probe_ms = now_ms;
        esp_err_t err_probe = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW40);
        if (err_probe == ESP_OK) {
            wifi_bandwidth_t cur_bw = WIFI_BW20;
            esp_wifi_get_bandwidth(WIFI_IF_STA, &cur_bw);
            if (cur_bw == WIFI_BW40) {
                s_bw_is_ht40 = true;
                ESP_LOGI(TAG, ">>> [WIFI] Link promovido para HT40 (40 MHz - ate 150 Mbps PHY)! <<<");
            }
        }
    }

    // Keepalive de ARP/broadcast na rede local a cada 3s para evitar que roteadores e switches percam o MAC do MPS3
    // Suspenso durante o Transfer Boost Mode para nao abrir sockets UDP nem emitir broadcast durante rajadas TCP!
    static uint32_t s_last_arp_keepalive_ms = 0;
    if (s_got_ip && !s_ota_in_progress && !wifi_transfer_is_boost_active() && (now_ms - s_last_arp_keepalive_ms >= 3000)) {
        s_last_arp_keepalive_ms = now_ms;
        int ks = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (ks >= 0) {
            int bcast = 1;
            setsockopt(ks, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));
            struct sockaddr_in dst = {
                .sin_family = AF_INET,
                .sin_port = htons(9),
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
        s_exit_requested = false;
        s_last_httpd_attempt_ms = 0;
        if (s_sta_reconnect_timer) {
            esp_timer_stop(s_sta_reconnect_timer);
        }
        if (s_httpd) {
            httpd_stop(s_httpd);
            s_httpd = NULL;
        }
        if (s_dns_task_handle) {
            if (s_dns_socket >= 0) {
                close(s_dns_socket);
                s_dns_socket = -1;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            s_dns_task_handle = NULL;
        }
        stop_mdns_service();
        if (s_active) {
            esp_wifi_scan_stop();
            esp_wifi_stop();
            s_active = false;
        }
        s_is_scanning = false;
        s_wifi_menu_open = false;
        s_wifi_substate = WIFI_SUBSTATE_SUBMENU;
        s_ui_state = WIFI_UI_IDLE;
        s_auto_fallback = false;
        s_status[0] = '\0';
        progress_end();

        // Libera recursos persistentes (worker Core 1, bounce DMA interno, RingBuffer 4MB PSRAM e recv_buf)
        teardown_transfer_pipeline();

        if (sd_card_get_frequency() != SDMMC_FREQ_HIGHSPEED) {
            sd_card_set_frequency(SDMMC_FREQ_HIGHSPEED);
        }
        wifi_release_sd_to_audio(s_files_received > 0);
        ESP_LOGI(TAG, "Modo WiFi encerrado (%d arquivo(s) recebido(s))", s_files_received);
        return true;
    }

    return false;
}

static void wifi_substate_stop_radio(void);

void wifi_transfer_request_exit(void)
{
    if (s_ota_in_progress) {
        ESP_LOGW(TAG, "Tentativa de sair do menu WiFi ignorada: atualizacao OTA em andamento!");
        return;
    }
    // Se o usuario segurar ESQUERDA (hold 600ms) dentro de subtelas (Teclado, Scan, Redes Salvas),
    // recua um nivel na hierarquia em vez de derrubar toda a sessao Wi-Fi.
    if (s_wifi_substate == WIFI_SUBSTATE_KEYBOARD) {
        s_wifi_substate = WIFI_SUBSTATE_SCAN_LIST;
        return;
    }
    if (s_wifi_substate == WIFI_SUBSTATE_NETWORK_ACTION) {
        s_wifi_substate = WIFI_SUBSTATE_MANAGE_SAVED;
        return;
    }
    if (s_wifi_substate == WIFI_SUBSTATE_SCAN_LIST ||
        s_wifi_substate == WIFI_SUBSTATE_MANAGE_SAVED ||
        s_wifi_substate == WIFI_SUBSTATE_AP_ACTIVE ||
        s_wifi_substate == WIFI_SUBSTATE_STA_ACTIVE) {
        wifi_substate_stop_radio();
        s_wifi_menu_open = false;
        return;
    }
    if (!s_active) {
        s_wifi_menu_open = false;
        s_exit_requested = false;
        if (sd_card_get_frequency() != SDMMC_FREQ_HIGHSPEED) {
            sd_card_set_frequency(SDMMC_FREQ_HIGHSPEED);
        }
        wifi_release_sd_to_audio(false);
        return;
    }
    s_exit_requested = true;
}

bool wifi_transfer_is_active(void)        { return s_wifi_menu_open || s_active; }
bool wifi_transfer_is_ota_busy(void)      { return s_ota_in_progress; }
bool wifi_transfer_is_transferring(void)  { return s_progress_active || s_ota_in_progress; }
bool wifi_transfer_has_sta_ip(void)       { return s_got_ip; }

bool wifi_transfer_is_overlay_active(void)
{
    if (!s_active && !s_wifi_menu_open) return false;
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (s_progress_active || s_ota_in_progress) return true;
    if (s_ui_state == WIFI_UI_DONE) {
        if (now <= s_done_show_until_ms) {
            return true;
        } else {
            s_ui_state = s_got_ip ? WIFI_UI_CONNECTED : WIFI_UI_IDLE;
        }
    }
    return false;
}

void wifi_transfer_draw_overlay(void)
{
    if (s_transfer_display_fps <= 0) return;
    wifi_menu_draw_status();
}

esp_err_t wifi_transfer_scan_start(void)
{
    s_exit_requested = false;
    wifi_acquire_sd_and_pause_audio();

    esp_err_t err = ensure_stack_ready();
    if (err != ESP_OK) return err;

    if (s_sta_reconnect_timer) {
        esp_timer_stop(s_sta_reconnect_timer);
    }

    s_is_scanning = true;
    s_scan_count = 0;

    if (s_active) {
        esp_wifi_scan_stop();
        esp_wifi_disconnect();
        esp_wifi_set_mode(WIFI_MODE_STA);
    } else {
        esp_wifi_set_mode(WIFI_MODE_STA);
        err = esp_wifi_start();
        if (err != ESP_OK) {
            s_is_scanning = false;
            return err;
        }
        s_active = true;
    }

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 200,
    };
    err = esp_wifi_scan_start(&scan_cfg, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_start falhou (%s), reiniciando STA para scan...", esp_err_to_name(err));
        esp_wifi_stop();
        esp_wifi_set_mode(WIFI_MODE_STA);
        if (esp_wifi_start() == ESP_OK) {
            err = esp_wifi_scan_start(&scan_cfg, false);
        }
        if (err != ESP_OK) {
            s_is_scanning = false;
        }
    }
    return err;
}

esp_err_t wifi_transfer_scan_get_results(void *out_aps, int max_aps, int *out_count)
{
    if (out_count) *out_count = (int)s_scan_count;
    if (out_aps && max_aps > 0 && s_scan_count > 0) {
        int copy_n = (s_scan_count < (uint16_t)max_aps) ? (int)s_scan_count : max_aps;
        memcpy(out_aps, s_scan_records, copy_n * sizeof(wifi_ap_record_t));
    }
    return ESP_OK;
}

static void wifi_substate_stop_radio(void)
{
    if (s_sta_reconnect_timer) {
        esp_timer_stop(s_sta_reconnect_timer);
    }
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    if (s_dns_task_handle) {
        if (s_dns_socket >= 0) {
            close(s_dns_socket);
            s_dns_socket = -1;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        s_dns_task_handle = NULL;
    }
    stop_mdns_service();
    if (s_active) {
        esp_wifi_scan_stop();
        esp_wifi_stop();
        s_active = false;
    }
    s_is_scanning = false;
    s_exit_requested = false;
    s_ui_state = WIFI_UI_IDLE;
    s_auto_fallback = false;
    s_status[0] = '\0';
    progress_end();

    teardown_transfer_pipeline();

    if (sd_card_get_frequency() != SDMMC_FREQ_HIGHSPEED) {
        sd_card_set_frequency(SDMMC_FREQ_HIGHSPEED);
    }
    wifi_release_sd_to_audio(s_files_received > 0);

    s_wifi_substate = WIFI_SUBSTATE_SUBMENU;
}

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

// --- Registro e Despacho de Navegacao no Menu -------------------------------

static void wifi_menu_on_select(void)
{
    s_exit_requested = false;
    s_wifi_menu_open = true;
    if (s_active && !s_is_scanning &&
        (s_wifi_substate == WIFI_SUBSTATE_AP_ACTIVE || s_wifi_substate == WIFI_SUBSTATE_STA_ACTIVE)) {
        s_wifi_substate = (s_mode == WIFI_TRANSFER_MODE_AP) ? WIFI_SUBSTATE_AP_ACTIVE : WIFI_SUBSTATE_STA_ACTIVE;
    } else {
        s_wifi_substate = WIFI_SUBSTATE_SUBMENU;
        s_submenu_cursor = 0;
    }
}

static void wifi_menu_request_action(void)
{
    switch (s_wifi_substate) {
        case WIFI_SUBSTATE_SUBMENU:
            if (s_submenu_cursor == 0) {
                // 1. Abrir Hotspot (AP) deterministico
                s_auto_fallback = false;
                if (wifi_transfer_enter(WIFI_TRANSFER_MODE_AP) == ESP_OK) {
                    s_wifi_substate = WIFI_SUBSTATE_AP_ACTIVE;
                    s_ap_qr_mode = false;
                }
            } else if (s_submenu_cursor == 1) {
                // 2. Conectar Wi-Fi (STA)
                s_auto_fallback = false;
                if (wifi_transfer_enter(WIFI_TRANSFER_MODE_STA) == ESP_OK) {
                    s_wifi_substate = WIFI_SUBSTATE_STA_ACTIVE;
                } else {
                    // Se nao houver redes salvas na NVS, leva direto para Buscar Redes (Scan)!
                    s_wifi_substate = WIFI_SUBSTATE_SCAN_LIST;
                    s_scan_cursor = 0;
                    s_scan_top_idx = 0;
                    wifi_transfer_scan_start();
                }
            } else if (s_submenu_cursor == 2) {
                // 3. Buscar Redes (Scan)
                s_wifi_substate = WIFI_SUBSTATE_SCAN_LIST;
                s_scan_cursor = 0;
                s_scan_top_idx = 0;
                wifi_transfer_scan_start();
            } else if (s_submenu_cursor == 3) {
                // 4. Gerenciar Redes Salvas
                known_networks_load();
                s_wifi_substate = WIFI_SUBSTATE_MANAGE_SAVED;
                s_manage_cursor = 0;
                s_manage_top_idx = 0;
            }
            break;

        case WIFI_SUBSTATE_AP_ACTIVE:
            s_ap_qr_mode = !s_ap_qr_mode;
            break;

        case WIFI_SUBSTATE_SCAN_LIST:
            if (!s_is_scanning && s_scan_count == 0) {
                wifi_transfer_scan_start();
                break;
            }
            if (s_scan_count > 0 && s_scan_cursor < (int)s_scan_count) {
                wifi_ap_record_t *ap = &s_scan_records[s_scan_cursor];
                strlcpy(s_selected_ssid, (char *)ap->ssid, sizeof(s_selected_ssid));
                if (ap->authmode == WIFI_AUTH_OPEN) {
                    wifi_transfer_save_network(s_selected_ssid, "");
                    wifi_transfer_enter(WIFI_TRANSFER_MODE_STA);
                    s_wifi_substate = WIFI_SUBSTATE_STA_ACTIVE;
                } else {
                    s_kb_input[0] = '\0';
                    s_kb_r = 1;
                    s_kb_c = 0;
                    s_kb_shift = 0;
                    // Se a rede ja estiver salva na NVS, pre-preenche a senha e posiciona no OK
                    known_networks_load();
                    for (int i = 0; i < s_known_count; i++) {
                        if (strcmp(s_known[i].ssid, s_selected_ssid) == 0 && s_known[i].pass[0] != '\0') {
                            strlcpy(s_kb_input, s_known[i].pass, sizeof(s_kb_input));
                            s_kb_r = 3;
                            s_kb_c = 9; // Cursor direto em "OK" para conexao com 1 clique
                            break;
                        }
                    }
                    s_wifi_substate = WIFI_SUBSTATE_KEYBOARD;
                }
            }
            break;

        case WIFI_SUBSTATE_KEYBOARD: {
            static const char *kb_lower[4][10] = {
                {"1","2","3","4","5","6","7","8","9","0"},
                {"q","w","e","r","t","y","u","i","o","p"},
                {"a","s","d","f","g","h","j","k","l","_"},
                {"z","x","c","v","b","n","m","^","<","OK"}
            };
            static const char *kb_upper[4][10] = {
                {"1","2","3","4","5","6","7","8","9","0"},
                {"Q","W","E","R","T","Y","U","I","O","P"},
                {"A","S","D","F","G","H","J","K","L","_"},
                {"Z","X","C","V","B","N","M","^","<","OK"}
            };
            static const char *kb_sym[4][10] = {
                {"!","@","#","$","%","&","*","-","+","="},
                {"/","?","<",">","(",")","[","]","{","}"},
                {"~","^",":",";",",",".","'","\"","|","_"},
                {"\\","`","$","?","!","#","*","^","<","OK"}
            };
            const char *key = (s_kb_shift == 1) ? kb_upper[s_kb_r][s_kb_c] :
                              (s_kb_shift == 2) ? kb_sym[s_kb_r][s_kb_c] :
                                                  kb_lower[s_kb_r][s_kb_c];

            if (strcmp(key, "^") == 0) {
                s_kb_shift = (s_kb_shift + 1) % 3;
            } else if (strcmp(key, "<") == 0) {
                size_t len = strlen(s_kb_input);
                if (len > 0) s_kb_input[len - 1] = '\0';
            } else if (strcmp(key, "OK") == 0) {
                wifi_transfer_save_network(s_selected_ssid, s_kb_input);
                wifi_transfer_enter(WIFI_TRANSFER_MODE_STA);
                s_wifi_substate = WIFI_SUBSTATE_STA_ACTIVE;
            } else if (strcmp(key, "_") == 0) {
                size_t len = strlen(s_kb_input);
                if (len + 1 < sizeof(s_kb_input)) {
                    s_kb_input[len] = ' ';
                    s_kb_input[len + 1] = '\0';
                }
            } else {
                size_t len = strlen(s_kb_input);
                if (len + 1 < sizeof(s_kb_input)) {
                    s_kb_input[len] = key[0];
                    s_kb_input[len + 1] = '\0';
                }
            }
            break;
        }

        case WIFI_SUBSTATE_MANAGE_SAVED:
            if (s_known_count > 0 && s_manage_cursor < s_known_count) {
                s_action_cursor = 0;
                s_wifi_substate = WIFI_SUBSTATE_NETWORK_ACTION;
            }
            break;

        case WIFI_SUBSTATE_NETWORK_ACTION:
            if (s_manage_cursor < s_known_count) {
                const char *cur_ssid = s_known[s_manage_cursor].ssid;
                if (s_action_cursor == 0) {
                    // 1. Conectar Agora
                    wifi_transfer_promote_network(cur_ssid);
                    wifi_transfer_enter(WIFI_TRANSFER_MODE_STA);
                    s_wifi_substate = WIFI_SUBSTATE_STA_ACTIVE;
                } else if (s_action_cursor == 1) {
                    // 2. Ver Senha (mostrado no proprio display)
                } else if (s_action_cursor == 2) {
                    // 3. Mover para o Topo
                    wifi_transfer_promote_network(cur_ssid);
                    s_manage_cursor = 0;
                    s_wifi_substate = WIFI_SUBSTATE_MANAGE_SAVED;
                } else if (s_action_cursor == 3) {
                    // 4. Apagar Rede
                    wifi_transfer_delete_network(cur_ssid);
                    if (s_manage_cursor >= s_known_count && s_manage_cursor > 0) {
                        s_manage_cursor--;
                    }
                    s_wifi_substate = WIFI_SUBSTATE_MANAGE_SAVED;
                }
            }
            break;

        default:
            break;
    }
}

static void wifi_menu_nav_up(void)
{
    switch (s_wifi_substate) {
        case WIFI_SUBSTATE_SUBMENU:
            if (s_submenu_cursor > 0) s_submenu_cursor--;
            break;
        case WIFI_SUBSTATE_SCAN_LIST:
            if (s_scan_cursor > 0) {
                s_scan_cursor--;
                if (s_scan_cursor < s_scan_top_idx) s_scan_top_idx = s_scan_cursor;
            }
            break;
        case WIFI_SUBSTATE_KEYBOARD:
            s_kb_r = (s_kb_r > 0) ? s_kb_r - 1 : 3;
            break;
        case WIFI_SUBSTATE_MANAGE_SAVED:
            if (s_manage_cursor > 0) {
                s_manage_cursor--;
                if (s_manage_cursor < s_manage_top_idx) s_manage_top_idx = s_manage_cursor;
            }
            break;
        case WIFI_SUBSTATE_NETWORK_ACTION:
            if (s_action_cursor > 0) s_action_cursor--;
            break;
        default:
            break;
    }
}

static void wifi_menu_nav_down(void)
{
    switch (s_wifi_substate) {
        case WIFI_SUBSTATE_SUBMENU:
            if (s_submenu_cursor < 3) s_submenu_cursor++;
            break;
        case WIFI_SUBSTATE_SCAN_LIST:
            if (s_scan_cursor + 1 < (int)s_scan_count) {
                s_scan_cursor++;
                if (s_scan_cursor >= s_scan_top_idx + 4) s_scan_top_idx = s_scan_cursor - 3;
            }
            break;
        case WIFI_SUBSTATE_KEYBOARD:
            s_kb_r = (s_kb_r < 3) ? s_kb_r + 1 : 0;
            break;
        case WIFI_SUBSTATE_MANAGE_SAVED:
            if (s_manage_cursor + 1 < s_known_count) {
                s_manage_cursor++;
                if (s_manage_cursor >= s_manage_top_idx + 4) s_manage_top_idx = s_manage_cursor - 3;
            }
            break;
        case WIFI_SUBSTATE_NETWORK_ACTION:
            if (s_action_cursor < 3) s_action_cursor++;
            break;
        default:
            break;
    }
}

static void wifi_menu_nav_select(void)
{
    switch (s_wifi_substate) {
        case WIFI_SUBSTATE_KEYBOARD:
            s_kb_c = (s_kb_c < 9) ? s_kb_c + 1 : 0;
            break;
        case WIFI_SUBSTATE_SUBMENU:
        case WIFI_SUBSTATE_SCAN_LIST:
        case WIFI_SUBSTATE_MANAGE_SAVED:
        case WIFI_SUBSTATE_NETWORK_ACTION:
            wifi_menu_request_action();
            break;
        default:
            break;
    }
}

static void wifi_menu_nav_left(void)
{
    switch (s_wifi_substate) {
        case WIFI_SUBSTATE_KEYBOARD:
            s_kb_c = (s_kb_c > 0) ? s_kb_c - 1 : 9;
            break;
        case WIFI_SUBSTATE_NETWORK_ACTION:
            s_wifi_substate = WIFI_SUBSTATE_MANAGE_SAVED;
            break;
        case WIFI_SUBSTATE_MANAGE_SAVED:
        case WIFI_SUBSTATE_SCAN_LIST:
            wifi_substate_stop_radio();
            break;
        case WIFI_SUBSTATE_AP_ACTIVE:
        case WIFI_SUBSTATE_STA_ACTIVE:
            wifi_substate_stop_radio();
            break;
        case WIFI_SUBSTATE_SUBMENU:
            s_wifi_menu_open = false;
            wifi_transfer_request_exit();
            break;
        default:
            s_wifi_substate = WIFI_SUBSTATE_SUBMENU;
            break;
    }
}

static void wifi_menu_draw_status(void)
{
    static int s_cached_rssi = -50;
    static uint32_t s_last_rssi_ms = 0;
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    // Se ha uma transferencia ou atualizacao OTA em andamento (ou tela de conclusao DONE),
    // a tela de progresso assume prioridade maxima sobre qualquer submenu:
    if (s_ui_state != WIFI_UI_TRANSFERRING && s_ui_state != WIFI_UI_STREAMING && s_ui_state != WIFI_UI_DONE &&
        s_ui_state != WIFI_UI_OTA_UPDATING && s_ui_state != WIFI_UI_OTA_FINISHED) {
        switch (s_wifi_substate) {
            case WIFI_SUBSTATE_SUBMENU:
                oled_display_show_wifi_submenu(s_submenu_cursor);
                return;
            case WIFI_SUBSTATE_SCAN_LIST: {
                wifi_scan_display_ap_t d_aps[12];
                int cnt = (s_scan_count > 12) ? 12 : s_scan_count;
                for (int i = 0; i < cnt; i++) {
                    strlcpy(d_aps[i].ssid, (char *)s_scan_records[i].ssid, sizeof(d_aps[i].ssid));
                    d_aps[i].rssi = s_scan_records[i].rssi;
                    d_aps[i].authmode = s_scan_records[i].authmode;
                }
                oled_display_show_wifi_scan(d_aps, cnt, s_scan_cursor, s_scan_top_idx, s_is_scanning);
                return;
            }
            case WIFI_SUBSTATE_KEYBOARD:
                oled_display_show_wifi_keyboard(s_selected_ssid, s_kb_input, s_kb_r, s_kb_c, s_kb_shift);
                return;
            case WIFI_SUBSTATE_MANAGE_SAVED: {
                char ssids[8][33] = {{0}};
                char passes[8][65] = {{0}};
                known_networks_load();
                for (int i = 0; i < s_known_count && i < 8; i++) {
                    strlcpy(ssids[i], s_known[i].ssid, sizeof(ssids[i]));
                    strlcpy(passes[i], s_known[i].pass, sizeof(passes[i]));
                }
                oled_display_show_wifi_manage((const char (*)[33])ssids, (const char (*)[65])passes, s_known_count, s_manage_cursor, s_manage_top_idx);
                return;
            }
            case WIFI_SUBSTATE_NETWORK_ACTION: {
                const char *ssid = (s_manage_cursor < s_known_count) ? s_known[s_manage_cursor].ssid : "";
                const char *pass = (s_manage_cursor < s_known_count) ? s_known[s_manage_cursor].pass : "";
                oled_display_show_wifi_network_action(ssid, pass, s_action_cursor);
                return;
            }
            case WIFI_SUBSTATE_AP_ACTIVE:
                if (s_ap_qr_mode) {
                    oled_display_show_wifi_qr(CONFIG_WIFI_AP_SSID, CONFIG_WIFI_AP_PASSWORD, "AP MPS3", 1, 1);
                    return;
                }
                break;
            case WIFI_SUBSTATE_STA_ACTIVE:
            default:
                break;
        }
    }

    bool is_transferring = (s_ui_state == WIFI_UI_TRANSFERRING || s_ui_state == WIFI_UI_STREAMING || s_ui_state == WIFI_UI_OTA_UPDATING);
    bool state_changed = (s_ui_state != s_wifi_last_drawn_state);

    if (!is_transferring) {
        if (!state_changed && (now_ms - s_wifi_last_draw_ms < 250)) {
            return;
        }
    }
    s_wifi_last_draw_ms = now_ms;
    s_wifi_last_drawn_state = s_ui_state;

    wifi_transfer_progress_t prog;
    wifi_transfer_get_progress(&prog);
    if (s_ui_state == WIFI_UI_TRANSFERRING) {
        static uint32_t s_draw_cnt = 0;
        ESP_LOGD("OVERLAY_LOG", "[#%u] done=%u/%u (%d%%), kbps=%.1f",
                 (unsigned)++s_draw_cnt,
                 (unsigned)s_progress_file_done, (unsigned)s_progress_file_total,
                 prog.file_pct, prog.kbps);
    }

    char status[64];
    int files = 0;
    wifi_transfer_get_status(status, sizeof(status), &files);

    // Extrai IP do status apenas se for um endereco IPv4 real (evita falso positivo com "Conectando...")
    char ip[16] = "0.0.0.0";
    if ((s_mode == WIFI_TRANSFER_MODE_AP || s_mode == WIFI_TRANSFER_MODE_APSTA || s_got_ip) &&
        status[0] >= '1' && status[0] <= '9' && strchr(status, '.') != NULL) {
        strlcpy(ip, status, sizeof(ip));
    }

    // Nunca chama esp_wifi_sta_get_rssi() durante transferencia ativa (evita lock IPC na task Wi-Fi!)
    int rssi = -1;
    if (s_mode == WIFI_TRANSFER_MODE_STA || s_mode == WIFI_TRANSFER_MODE_APSTA) {
        if (s_got_ip && !s_progress_active && !s_ota_in_progress && (s_last_rssi_ms == 0 || (now_ms - s_last_rssi_ms >= 5000))) {
            if (esp_wifi_sta_get_rssi(&s_cached_rssi) == ESP_OK) {
                s_last_rssi_ms = now_ms;
            }
        }
        rssi = s_got_ip ? s_cached_rssi : -1;
    }

    bool dns_active = (s_dns_socket >= 0);

    switch (s_ui_state) {
        case WIFI_UI_IDLE:
            if (s_mode == WIFI_TRANSFER_MODE_APSTA && s_got_ip) {
                oled_display_show_wifi_connected(ip, "mps3.local", rssi, 0);
            } else if (s_mode == WIFI_TRANSFER_MODE_AP || s_mode == WIFI_TRANSFER_MODE_APSTA) {
                oled_display_show_wifi_qr(CONFIG_WIFI_AP_SSID, CONFIG_WIFI_AP_PASSWORD, "AP MPS3", 1, 1);
            } else if (s_got_ip) {
                oled_display_show_wifi_connected(ip, "mps3.local", rssi, 0);
            } else {
                char sta_msg[32] = "Buscando rede...";
                if (s_candidate_idx < s_candidate_count && s_candidates[s_candidate_idx].ssid[0] != '\0') {
                    snprintf(sta_msg, sizeof(sta_msg), "Rede: %.14s", s_candidates[s_candidate_idx].ssid);
                } else if (status[0] != '\0') {
                    strlcpy(sta_msg, status, sizeof(sta_msg));
                }
                oled_display_show_wifi_idle("", sta_msg, dns_active, rssi);
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
        case WIFI_UI_STREAMING:
            oled_display_show_wifi_streaming(
                s_stream_filename,
                s_stream_file_size > 0 ? (int)(((uint64_t)s_stream_file_done * 100) / s_stream_file_size) : 0,
                s_stream_kbps,
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
        .request_action = wifi_menu_request_action,
        .nav_up = wifi_menu_nav_up,
        .nav_down = wifi_menu_nav_down,
        .nav_select = wifi_menu_nav_select,
        .nav_left = wifi_menu_nav_left,
    };
    menu_register(&item);
}

const char* wifi_transfer_get_substate_name(void)
{
    if (!s_wifi_menu_open && !s_active) return "OFF";
    switch (s_wifi_substate) {
        case WIFI_SUBSTATE_SUBMENU: return "SUBMENU";
        case WIFI_SUBSTATE_AP_ACTIVE: return "AP_ACTIVE";
        case WIFI_SUBSTATE_STA_ACTIVE: return "STA_ACTIVE";
        case WIFI_SUBSTATE_SCAN_LIST: return "SCAN_LIST";
        case WIFI_SUBSTATE_KEYBOARD: return "KEYBOARD";
        case WIFI_SUBSTATE_MANAGE_SAVED: return "MANAGE_SAVED";
        case WIFI_SUBSTATE_NETWORK_ACTION: return "NETWORK_ACTION";
        default: return "UNKNOWN";
    }
}

int wifi_transfer_get_submenu_cursor(void)
{
    return s_submenu_cursor;
}

bool wifi_transfer_is_menu_open(void)
{
    return s_wifi_menu_open;
}
