#include "oled_display.h"
#include "pinos.h"
#include "u8g2_hal.h"
#include "battery.h"
#include "bitmaps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2s_output.h"
#include "qrcode.h"
#include "audio_meter.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "u8g2.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

static const char *TAG = "oled_display";

#define OLED_WIDTH     128
#define OLED_HEIGHT    64
#define OLED_I2C_ADDR  0x3C

static u8g2_t s_u8g2;

static bool s_ready = false;
static uint8_t s_target_brightness = 255;
static uint8_t s_current_brightness = 255;

// Shadow framebuffer de 1024 bytes para Dirty-Page Tracking (atualizacao diferencial por pagina de 128B)
static uint8_t s_prev_fb[1024];
static bool s_fb_dirty_all = true;

// Tabela LUT de Seno em ponto fixo Q10 (0..90 graus, escala 1024 = 1.0)
// Elimina chamadas a sinf/cosf/roundf em software/FPU durante animacoes de UI a 40 FPS
static const int16_t s_sin_q10_lut[91] = {
       0,   18,   36,   54,   71,   89,  107,  125,  143,  160,
     178,  195,  213,  230,  248,  265,  282,  299,  316,  333,
     350,  367,  384,  400,  416,  433,  449,  465,  481,  496,
     512,  527,  543,  558,  573,  587,  602,  616,  630,  644,
     658,  672,  685,  698,  711,  724,  737,  749,  761,  773,
     784,  796,  807,  818,  828,  839,  849,  859,  868,  878,
     887,  896,  904,  912,  920,  928,  935,  943,  949,  956,
     962,  968,  974,  979,  984,  989,  994,  998, 1002, 1005,
    1008, 1011, 1014, 1016, 1018, 1020, 1022, 1023, 1023, 1024,
    1024
};

static inline int fast_sin_q10(int deg)
{
    deg %= 360;
    if (deg < 0) deg += 360;
    if (deg <= 90)  return  s_sin_q10_lut[deg];
    if (deg <= 180) return  s_sin_q10_lut[180 - deg];
    if (deg <= 270) return -s_sin_q10_lut[deg - 180];
    return -s_sin_q10_lut[360 - deg];
}

static inline int fast_cos_q10(int deg)
{
    return fast_sin_q10(deg + 90);
}

static inline int fast_scale_trig(int radius, int trig_q10)
{
    int prod = radius * trig_q10;
    return (prod >= 0) ? ((prod + 512) >> 10) : ((prod - 512) >> 10);
}

static void apply_brightness_if_needed(void) {
    if (s_target_brightness != s_current_brightness) {
        s_current_brightness = s_target_brightness;
        u8g2_SetContrast(&s_u8g2, s_current_brightness);
    }
}

static void oled_flush_buffer(void)
{
    uint8_t *fb = u8g2_GetBufferPtr(&s_u8g2);
    if (!fb || s_fb_dirty_all) {
        u8g2_SendBuffer(&s_u8g2);
        if (fb) {
            memcpy(s_prev_fb, fb, 1024);
        }
        s_fb_dirty_all = false;
        return;
    }

    // Dirty-Page Update: verifica cada uma das 8 paginas horizontais (128 bytes por pagina)
    // e transmite via I2C apenas as paginas cujo conteudo mudou em relacao ao frame anterior.
    for (uint8_t page = 0; page < 8; page++) {
        size_t offset = (size_t)page * 128;
        if (memcmp(fb + offset, s_prev_fb + offset, 128) != 0) {
            u8g2_UpdateDisplayArea(&s_u8g2, 0, page, 16, 1);
            memcpy(s_prev_fb + offset, fb + offset, 128);
        }
    }
}

// Modos de visualizacao na tela Now Playing
static oled_display_viz_mode_t s_viz_mode = VIZ_MODE_CLASSIC;
static uint32_t s_overlay_until_ms = 0;
static char s_last_track_title[128] = {0};

void oled_display_set_viz_mode(oled_display_viz_mode_t mode)
{
    if (mode >= VIZ_MODE_COUNT) mode = VIZ_MODE_CLASSIC;
    s_viz_mode = mode;
    nvs_handle_t h;
    if (nvs_open("mps3_settings", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "viz_mode", (uint8_t)mode);
        nvs_commit(h);
        nvs_close(h);
    }
}

oled_display_viz_mode_t oled_display_get_viz_mode(void)
{
    return s_viz_mode;
}

void oled_display_cycle_viz_mode(void)
{
    oled_display_viz_mode_t next = (oled_display_viz_mode_t)((s_viz_mode + 1) % VIZ_MODE_COUNT);
    oled_display_set_viz_mode(next);
}

void oled_display_trigger_viz_overlay(void)
{
    s_overlay_until_ms = (uint32_t)(esp_timer_get_time() / 1000ULL) + 3000;
}

// Frames para animações WiFi
static int s_wifi_attention_frame = 0;
static int s_wifi_sync_frame = 0;
static int s_wifi_download_frame = 0;
static uint32_t s_wifi_last_frame_ms = 0;

esp_err_t oled_display_init(void)
{
    esp_err_t ret = u8g2_hal_i2c_init(PIN_OLED_SDA, PIN_OLED_SCL, OLED_I2C_ADDR);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar I2C para o OLED: %s", esp_err_to_name(ret));
        return ret;
    }

    u8g2_Setup_ssd1306_i2c_128x64_noname_f(&s_u8g2, U8G2_R2,
                                            u8g2_hal_byte_cb, u8g2_hal_gpio_and_delay_cb);
    // Substitui o CAD padrao (que fragmenta transferencias em blocos de 24 bytes)
    // pelo nosso CAD de alta velocidade em burst de pagina completa (128 bytes).
    s_u8g2.u8x8.cad_cb = u8x8_cad_mps3_fast_i2c;

    u8g2_InitDisplay(&s_u8g2);
    u8g2_SetPowerSave(&s_u8g2, 0);
    u8g2_SetBitmapMode(&s_u8g2, 1);
    u8g2_SetFontMode(&s_u8g2, 1);
    u8g2_ClearBuffer(&s_u8g2);
    apply_brightness_if_needed();
    s_fb_dirty_all = true;
    oled_flush_buffer();

    nvs_handle_t h;
    if (nvs_open("mps3_settings", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, "viz_mode", &v) == ESP_OK && v < VIZ_MODE_COUNT) {
            s_viz_mode = (oled_display_viz_mode_t)v;
        }
        nvs_close(h);
    }

    s_ready = true;
    return ESP_OK;
}

static void format_time(uint32_t total_seconds, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    uint32_t m = total_seconds / 60;
    uint32_t s = total_seconds % 60;
    if (m > 99) m = 99;
    snprintf(out, out_len, "%02d:%02d", (int)(m % 100), (int)(s % 60));
}

// As fontes u8g2 usadas aqui (u8g2_font_5x7_tr, u8g2_font_t0_17_tr - sufixo
// "_tr" = "transparent" + "restricted", so' tem glifos pro intervalo ASCII
// 0x20-0x7E) nao cobrem letras acentuadas. Nomes de arquivo em portugues
// (ex: "musica.flac", "nao-me-deixe.mp3") gravados no cartao SD acabam em
// CP850/Latin-1 (ver CONFIG_FATFS_CODEPAGE_850 no sdkconfig.defaults), com
// bytes >= 0x80 pros caracteres acentuados - fora do intervalo que essas
// fontes entendem, o que fazia esses bytes virarem glifos aleatorios/
// errados na tela ("hieroglifos"). Filtramos aqui: qualquer byte fora do
// ASCII imprimivel vira '?' antes de desenhar - perde o acento, mas
// garante que o resto do nome sempre aparece legivel.
static void sanitize_for_display(const char *in, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    if (!in) {
        out[0] = '\0';
        return;
    }
    size_t i = 0;
    for (; in[i] != '\0' && i + 1 < out_len; i++) {
        unsigned char c = (unsigned char)in[i];
        out[i] = (c >= 0x20 && c <= 0x7E) ? (char)c : '?';
    }
    out[i] = '\0';
}

static inline void set_px(int x, int y, bool on)
{
    u8g2_SetDrawColor(&s_u8g2, on ? 1 : 0);
    u8g2_DrawPixel(&s_u8g2, x, y);
}

// =========================================================================
// Rolagem horizontal (marquee) por PIXEL - usa fonte proporcional real do
// u8g2, entao rolar por "janela de caracteres" (como fariamos com uma
// fonte monoespacada) nao funciona bem. Em vez disso desenhamos o texto
// numa posicao X que desliza continuamente, e uma segunda copia logo
// depois pra criar um loop sem costura.
// =========================================================================
typedef struct {
    char text[128];
    int offset_px;
} marquee_t;

static void marquee_reset_if_new(marquee_t *m, const char *text)
{
    if (strcmp(m->text, text) != 0) {
        strncpy(m->text, text, sizeof(m->text) - 1);
        m->text[sizeof(m->text) - 1] = '\0';
        m->offset_px = 0;
    }
}

// Desenha `m->text` em (x_start,y) usando a fonte JA' selecionada. Se
// couber no espaco disponivel (da largura da tela ate' o final, a partir
// de x_start), fica estatico; senao rola continuamente. x_start != 0 e'
// usado na lista (o cursor reserva as primeiras colunas pra' seta) - o
// texto (estatico OU rolando) precisa comecar exatamente onde a janela de
// recorte (clip window) tambem comeca, senao as primeiras colunas do
// texto ficam cortadas pela propria janela (bug corrigido: antes
// desenhava sempre a partir de x=0, mesmo com um clip window comecando
// em x=8 - "comia" a primeira letra do nome).
static void draw_marquee(marquee_t *m, const char *text, int x_start, int y, int step_px)
{
    marquee_reset_if_new(m, text);

    int available_w = OLED_WIDTH - x_start;
    int text_w = u8g2_GetStrWidth(&s_u8g2, m->text);
    if (text_w <= available_w) {
        u8g2_DrawStr(&s_u8g2, x_start, y, m->text);
        m->offset_px = 0;
        return;
    }

    const int gap = 16; // espaco entre o fim e a copia repetida
    int x = x_start - m->offset_px;
    u8g2_DrawStr(&s_u8g2, x, y, m->text);
    u8g2_DrawStr(&s_u8g2, x + text_w + gap, y, m->text);

    m->offset_px += step_px;
    if (m->offset_px >= text_w + gap) {
        m->offset_px = 0;
    }
}

// Funcao para marquee com largura disponivel explicita
static void draw_marquee_width(marquee_t *m, const char *text, int x_start, int available_w, int y, int step_px)
{
    marquee_reset_if_new(m, text);

    int text_w = u8g2_GetStrWidth(&s_u8g2, m->text);
    if (text_w <= available_w) {
        int tx = x_start + (available_w - text_w) / 2;
        u8g2_DrawStr(&s_u8g2, tx, y, m->text);
        m->offset_px = 0;
        return;
    }

    const int gap = 16;
    int x = x_start - m->offset_px;
    u8g2_DrawStr(&s_u8g2, x, y, m->text);
    u8g2_DrawStr(&s_u8g2, x + text_w + gap, y, m->text);

    m->offset_px += step_px;
    if (m->offset_px >= text_w + gap) {
        m->offset_px = 0;
    }
}

static marquee_t s_now_playing_marquee = {0};
static marquee_t s_artist_marquee = {0};
static marquee_t s_album_marquee = {0};
static marquee_t s_list_cursor_marquee = {0};
static marquee_t s_transfer_name_marquee = {0};


// =========================================================================
// Icone de play/pause, desenhado por primitivas (sem bitmap) - triangulo
// pro "play", duas barras pro "pause".
// =========================================================================
static const uint8_t image_Play_bits[] = {0x01,0x03,0x07,0x0f,0x1f,0x0f,0x07,0x03,0x01};
static const uint8_t image_Pause_bits[] = {0x1b,0x1b,0x1b,0x1b,0x1b,0x1b,0x1b,0x1b,0x1b};

static void draw_play_pause_icon(bool is_playing)
{
    u8g2_SetDrawColor(&s_u8g2, 1);
    if (is_playing) {
        u8g2_DrawXBM(&s_u8g2, 0, 1, 5, 9, image_Pause_bits);
    } else {
        u8g2_DrawXBM(&s_u8g2, 1, 1, 5, 9, image_Play_bits);
    }
}

// Icone de bateria desenhado por primitivas (moldura + terminal +
// preenchimento proporcional) - assim funciona pra qualquer percentual
// real, em vez de depender de bitmaps fixos por faixa de carga.
// percent < 0 (leitura indisponivel) nao desenha nada - mesma convencao
// ja usada no resto do firmware pra "sem sensor de bateria".
static const char *glyph_rows(char c, int row)
{
    static const char *digits[10][5] = {
        {"111","101","101","101","111"}, // 0
        {"010","110","010","010","111"}, // 1
        {"111","001","111","100","111"}, // 2
        {"111","001","111","001","111"}, // 3
        {"101","101","111","001","001"}, // 4
        {"111","100","111","001","111"}, // 5
        {"111","100","111","101","111"}, // 6
        {"111","001","001","001","001"}, // 7
        {"111","101","111","101","111"}, // 8
        {"111","101","111","001","111"}, // 9
    };
    static const char *colon[5] = {"000","010","000","010","000"};
    static const char *tilde[5] = {"000","101","010","101","000"};
    static const char *percent[5] = {"101","001","010","100","101"};
    if (c >= '0' && c <= '9') return digits[c - '0'][row];
    if (c == ':') return colon[row];
    if (c == '~') return tilde[row];
    if (c == '%') return percent[row];
    return "000";
}
#define GLYPH_W       3
#define GLYPH_H       5
#define GLYPH_ADVANCE 4
static void oled_draw_mini_text(int x, int y, const char *text)
{
    int cx = x;
    for (const char *p = text; *p; p++) {
        for (int row = 0; row < GLYPH_H; row++) {
            const char *bits = glyph_rows(*p, row);
            for (int col = 0; col < GLYPH_W; col++) {
                if (bits[col] == '1') u8g2_DrawPixel(&s_u8g2, cx + col, y + row);
            }
        }
        cx += GLYPH_ADVANCE;
    }
}
static int text_width_px(const char *text)
{
    int len = (int)strlen(text);
    return len > 0 ? (len * GLYPH_ADVANCE - 1) : 0;
}

#define BAR_H          11
#define BAR_INTERIOR_H (BAR_H - 2) // 9
#define BAR_INTERIOR_W (OLED_WIDTH - 2) // 126

static uint8_t s_bar_shadow[BAR_INTERIOR_W][BAR_INTERIOR_H];

static inline void shadow_set(int x, int y, bool on)
{
    if (x < 0 || x >= BAR_INTERIOR_W || y < 0 || y >= BAR_INTERIOR_H) return;
    s_bar_shadow[x][y] = on ? 1 : 0;
}
static inline bool shadow_get(int x, int y)
{
    if (x < 0 || x >= BAR_INTERIOR_W || y < 0 || y >= BAR_INTERIOR_H) return false;
    return s_bar_shadow[x][y] != 0;
}

static void shadow_draw_char(int x, int y, char c)
{
    for (int row = 0; row < GLYPH_H; row++) {
        const char *bits = glyph_rows(c, row);
        for (int col = 0; col < GLYPH_W; col++) {
            if (bits[col] == '1') shadow_set(x + col, y + row, true);
        }
    }
}

static void shadow_draw_text(int x, int y, const char *text)
{
    int cx = x;
    for (const char *p = text; *p; p++) {
        shadow_draw_char(cx, y, *p);
        cx += GLYPH_ADVANCE;
    }
}

static void draw_progress_bar_with_time(int bar_x, int bar_y, int bar_w, int bar_h,
                                         uint32_t elapsed_sec, uint32_t total_sec,
                                         bool duration_is_estimate)
{
    memset(s_bar_shadow, 0, sizeof(s_bar_shadow));

    char elapsed_str[8], total_str_raw[8], total_str[10];
    format_time(elapsed_sec, elapsed_str, sizeof(elapsed_str));
    format_time(total_sec, total_str_raw, sizeof(total_str_raw));
    if (duration_is_estimate) {
        snprintf(total_str, sizeof(total_str), "~%s", total_str_raw);
    } else {
        snprintf(total_str, sizeof(total_str), "%s", total_str_raw);
    }

    int text_y = (BAR_INTERIOR_H - GLYPH_H) / 2;

    shadow_draw_text(2, text_y, elapsed_str);

    int total_w = text_width_px(total_str);
    int total_x = BAR_INTERIOR_W - 2 - total_w;
    shadow_draw_text(total_x, text_y, total_str);

    int fill_w = 0;
    if (total_sec > 0) {
        fill_w = (int)(((int64_t)BAR_INTERIOR_W * elapsed_sec) / total_sec);
        if (fill_w > BAR_INTERIOR_W) fill_w = BAR_INTERIOR_W;
    }
    for (int x = 0; x < fill_w; x++) {
        for (int y = 0; y < BAR_INTERIOR_H; y++) {
            shadow_set(x, y, !shadow_get(x, y));
        }
    }

    // Moldura (1px, sempre visivel - fora do mecanismo de inversao).
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_DrawFrame(&s_u8g2, bar_x, bar_y, bar_w, bar_h);

    for (int x = 0; x < BAR_INTERIOR_W; x++) {
        for (int y = 0; y < BAR_INTERIOR_H; y++) {
            set_px(bar_x + 1 + x, bar_y + 1 + y, shadow_get(x, y));
        }
    }
    u8g2_SetDrawColor(&s_u8g2, 1);
}
static void draw_battery_icon(int y, int percent)
{
    if (percent < 0) return;
    if (percent > 100) percent = 100;

    battery_status_t status = battery_get_status();
    bool is_charging = (status == BATTERY_CHARGING);
    bool is_full = (status == BATTERY_FULL);

    // Se bateria critica (< 10%) e descarregando, pisca o icone a cada 500ms
    if (percent < 10 && !is_charging && !is_full) {
        if (((esp_timer_get_time() / 500000) % 2) == 0) {
            return; // Ciclo apagado do piscar
        }
    }

    char buf[8];
    if (is_full) {
        snprintf(buf, sizeof(buf), "100");
    } else {
        snprintf(buf, sizeof(buf), "%d", percent);
    }
    int tw = text_width_px(buf);
    
    int w = tw + 4;
    int h = 9;
    int x = 128 - w;

    // Se estiver carregando, recua o frame para dar espaco ao raio no canto direito
    if (is_charging) {
        x -= 5;
    }

    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_DrawFrame(&s_u8g2, x, y, w, h);
    
    // Terminal de contato da bateria
    u8g2_DrawVLine(&s_u8g2, x - 1, y + 2, 5);

    // Texto da porcentagem dentro do frame
    oled_draw_mini_text(x + 2, y + 2, buf);

    // Desenha icone de raio estilizado se estiver carregando
    if (is_charging) {
        int rx = x + w + 1;
        u8g2_DrawPixel(&s_u8g2, rx + 2, y + 1);
        u8g2_DrawPixel(&s_u8g2, rx + 1, y + 2);
        u8g2_DrawLine(&s_u8g2, rx + 0, y + 3, rx + 3, y + 3);
        u8g2_DrawPixel(&s_u8g2, rx + 2, y + 4);
        u8g2_DrawPixel(&s_u8g2, rx + 1, y + 5);
        u8g2_DrawPixel(&s_u8g2, rx + 0, y + 6);
    }
}

// format_line vem pronto como UMA string (ex: ".Flac   16 Bits  44Khz",
// montada em audio_player.cpp com varios espacos como separador). O
// layout novo mostra isso como 3 "crachas" separadas - em vez de mudar
// audio_player.cpp pra' expor 3 campos, so' separamos aqui onde houver 2+
// espacos seguidos (o formato sempre usa isso entre os campos, nunca
// dentro de um campo - "16 Bits" tem 1 espaco so', fica junto).
static void draw_viz_dynamic_overlay(const playback_state_t *state)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (now_ms >= s_overlay_until_ms) return;

    // Tarja de overlay superior com fundo opaco
    u8g2_SetDrawColor(&s_u8g2, 0);
    u8g2_DrawBox(&s_u8g2, 0, 0, 128, 11);
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_DrawHLine(&s_u8g2, 0, 10, 128);

    draw_play_pause_icon(state->playing && !state->paused);
    draw_battery_icon(1, battery_get_percent());

    // Tempo decorrido / total
    char el_str[8], tot_str[8], time_buf[20];
    format_time(state->elapsed_sec, el_str, sizeof(el_str));
    format_time(state->total_sec, tot_str, sizeof(tot_str));
    snprintf(time_buf, sizeof(time_buf), "%s/%s", el_str, tot_str);

    u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
    int time_w = u8g2_GetStrWidth(&s_u8g2, time_buf);
    int time_x = 113 - time_w;
    u8g2_DrawStr(&s_u8g2, time_x, 8, time_buf);

    // Titulo com marquee no espaco entre icone e tempo
    char safe_title[128];
    sanitize_for_display(state->title, safe_title, sizeof(safe_title));
    int title_x0 = 14;
    int title_x1 = time_x - 3;
    int avail_w = title_x1 - title_x0;
    if (avail_w > 15) {
        u8g2_SetClipWindow(&s_u8g2, title_x0, 0, title_x1, 10);
        int tw = u8g2_GetStrWidth(&s_u8g2, safe_title);
        if (tw <= avail_w) {
            u8g2_DrawStr(&s_u8g2, title_x0 + (avail_w - tw)/2, 8, safe_title);
        } else {
            draw_marquee_width(&s_artist_marquee, safe_title, title_x0, avail_w, 8, 1);
        }
        u8g2_SetMaxClipWindow(&s_u8g2);
    }
}

static void oled_display_show_now_playing_classic(const playback_state_t *state)
{
    // --- Topo: play/pause, artista dinamico, bateria ---
    draw_play_pause_icon(state->playing && !state->paused);
    draw_battery_icon(1, battery_get_percent());
    
    if (state->artist[0] != '\0') {
        char safe_artist[64];
        sanitize_for_display(state->artist, safe_artist, sizeof(safe_artist));
        u8g2_SetFont(&s_u8g2, u8g2_font_helvB08_tr);
        int aw = u8g2_GetStrWidth(&s_u8g2, safe_artist);
        
        int max_left = 12;
        int max_right = 113;
        int center_x = 64;
        
        int br_left = center_x - aw/2 - 3;
        int br_right = center_x + aw/2 + 3;
        
        bool needs_marquee = false;
        if (br_left < max_left || br_right > max_right) {
            br_left = max_left;
            br_right = max_right;
            needs_marquee = true;
        }
        
        // Linhas decorativas (brackets) no topo
        u8g2_DrawLine(&s_u8g2, br_left, 11, br_right, 11);
        u8g2_DrawLine(&s_u8g2, br_left, 10, br_left, 0);
        u8g2_DrawLine(&s_u8g2, br_right, 11, br_right, 0);
        
        // Texto
        if (!needs_marquee) {
            u8g2_DrawStr(&s_u8g2, center_x - aw/2, 9, safe_artist);
        } else {
            u8g2_SetClipWindow(&s_u8g2, br_left + 2, 0, br_right - 1, 10);
            draw_marquee(&s_artist_marquee, safe_artist, br_left + 2, 9, 1);
            u8g2_SetMaxClipWindow(&s_u8g2);
        }
    }

    // --- Titulo da musica ---
    char safe_title[128];
    sanitize_for_display(state->title, safe_title, sizeof(safe_title));
    u8g2_SetFont(&s_u8g2, u8g2_font_t0_22b_tr);
    
    int tw = u8g2_GetStrWidth(&s_u8g2, safe_title);
    if (tw <= 128) {
        int tx = (128 - tw) / 2;
        u8g2_DrawStr(&s_u8g2, tx, 28, safe_title);
    } else {
        u8g2_SetClipWindow(&s_u8g2, 0, 12, 128, 31);
        draw_marquee(&s_now_playing_marquee, safe_title, 0, 28, 1);
        u8g2_SetMaxClipWindow(&s_u8g2);
    }

    // --- Barra de progresso com tempo embutido ---
    draw_progress_bar_with_time(0, 33, 128, 11, state->elapsed_sec, state->total_sec, state->duration_is_estimate);

    // --- Album ---
    if (state->album[0] != '\0') {
        char safe_album[64];
        sanitize_for_display(state->album, safe_album, sizeof(safe_album));
        u8g2_SetFont(&s_u8g2, u8g2_font_helvB08_tr);
        int alw = u8g2_GetStrWidth(&s_u8g2, safe_album);
        
        u8g2_SetDrawColor(&s_u8g2, 1); 
        
        if (alw <= 124) {
            u8g2_DrawStr(&s_u8g2, (128 - alw) / 2, 53, safe_album);
        } else {
            u8g2_SetClipWindow(&s_u8g2, 2, 45, 126, 54);
            draw_marquee(&s_album_marquee, safe_album, 2, 53, 1);
            u8g2_SetMaxClipWindow(&s_u8g2);
        }
    }

    // --- 4 Cards dinamicos em forma de Brackets (Formato, Res, SR, Bitrate) ---
    char card_fmt[20], card_res[20], card_sr[20], card_br[20];
    
    const char *fmt_str = state->format_name;
    if (fmt_str[0] == '\0' && state->filename[0] != '\0') {
        const char *dot = strrchr(state->filename, '.');
        if (dot) fmt_str = dot + 1;
    }

    if (fmt_str[0] != '\0') {
        if (strncasecmp(fmt_str, "Conect", 6) == 0 || strcasecmp(fmt_str, "WebRadio") == 0) {
            snprintf(card_fmt, sizeof(card_fmt), "RADIO");
        } else {
            snprintf(card_fmt, sizeof(card_fmt), "%s", fmt_str);
            if (card_fmt[0] == '.') {
                memmove(card_fmt, card_fmt + 1, strlen(card_fmt));
            }
            for (int i = 0; card_fmt[i]; i++) {
                if (card_fmt[i] >= 'a' && card_fmt[i] <= 'z') card_fmt[i] -= 32;
            }
            if (strlen(card_fmt) > 5) card_fmt[5] = '\0';
        }

        int bits = (int)state->bits_per_sample;
        if (bits <= 0) {
            if (strcasecmp(card_fmt, "ALAC") == 0) bits = 24;
            else bits = 16;
        }
        snprintf(card_res, sizeof(card_res), "%db", bits);

        if (state->sample_rate > 0) {
            if (state->sample_rate % 1000 == 0) {
                snprintf(card_sr, sizeof(card_sr), "%uk", (unsigned)(state->sample_rate / 1000));
            } else {
                snprintf(card_sr, sizeof(card_sr), "%.1fk", state->sample_rate / 1000.0f);
            }
        } else {
            snprintf(card_sr, sizeof(card_sr), "44.1k");
        }

        if (state->bitrate > 0) {
            snprintf(card_br, sizeof(card_br), "%uk", (unsigned)((state->bitrate + 500) / 1000));
        } else {
            snprintf(card_br, sizeof(card_br), "VBR");
        }
    } else {
        card_fmt[0] = '\0';
    }

    if (card_fmt[0] != '\0') {
        u8g2_SetDrawColor(&s_u8g2, 1);
        u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
        
        int w_fmt = u8g2_GetStrWidth(&s_u8g2, card_fmt);
        int w_res = u8g2_GetStrWidth(&s_u8g2, card_res);
        int w_sr  = u8g2_GetStrWidth(&s_u8g2, card_sr);
        int w_br  = u8g2_GetStrWidth(&s_u8g2, card_br);
        
        int pad = 2;
        int bw_fmt = w_fmt + pad * 2;
        int bw_res = w_res + pad * 2;
        int bw_sr  = w_sr + pad * 2;
        int bw_br  = w_br + pad * 2;
        
        int total_bw = bw_fmt + bw_res + bw_sr + bw_br;
        int gap_w = (128 - total_bw) / 3;
        if (gap_w < 1) gap_w = 1;
        
        int x = 0;
        
        #define DRAW_BRACKET(bw, ctext) \
            u8g2_SetDrawColor(&s_u8g2, 1); \
            u8g2_DrawLine(&s_u8g2, x, 63, x, 57); \
            u8g2_DrawLine(&s_u8g2, x+1, 57, x+bw-1, 57); \
            u8g2_DrawLine(&s_u8g2, x+bw-1, 63, x+bw-1, 58); \
            u8g2_DrawStr(&s_u8g2, x + 1 + (bw - 2 - u8g2_GetStrWidth(&s_u8g2, ctext))/2, 64, ctext); \
            x += bw + gap_w;
            
        DRAW_BRACKET(bw_fmt, card_fmt);
        DRAW_BRACKET(bw_res, card_res);
        DRAW_BRACKET(bw_sr, card_sr);
        DRAW_BRACKET(bw_br, card_br);
        
        #undef DRAW_BRACKET
    }
}

static void oled_display_show_now_playing_hybrid(const playback_state_t *state)
{
    // 1. Topo: Play/Pause, Artista com brackets, Bateria
    draw_play_pause_icon(state->playing && !state->paused);
    draw_battery_icon(1, battery_get_percent());
    
    if (state->artist[0] != '\0') {
        char safe_artist[64];
        sanitize_for_display(state->artist, safe_artist, sizeof(safe_artist));
        u8g2_SetFont(&s_u8g2, u8g2_font_helvB08_tr);
        int aw = u8g2_GetStrWidth(&s_u8g2, safe_artist);
        int max_left = 12, max_right = 113, center_x = 64;
        int br_left = center_x - aw/2 - 3, br_right = center_x + aw/2 + 3;
        bool needs_marquee = false;
        if (br_left < max_left || br_right > max_right) {
            br_left = max_left;
            br_right = max_right;
            needs_marquee = true;
        }
        u8g2_DrawLine(&s_u8g2, br_left, 11, br_right, 11);
        u8g2_DrawLine(&s_u8g2, br_left, 10, br_left, 0);
        u8g2_DrawLine(&s_u8g2, br_right, 11, br_right, 0);
        if (!needs_marquee) {
            u8g2_DrawStr(&s_u8g2, center_x - aw/2, 9, safe_artist);
        } else {
            u8g2_SetClipWindow(&s_u8g2, br_left + 2, 0, br_right - 1, 10);
            draw_marquee(&s_artist_marquee, safe_artist, br_left + 2, 9, 1);
            u8g2_SetMaxClipWindow(&s_u8g2);
        }
    }

    // 2. Titulo da musica (22px)
    char safe_title[128];
    sanitize_for_display(state->title, safe_title, sizeof(safe_title));
    u8g2_SetFont(&s_u8g2, u8g2_font_t0_22b_tr);
    int tw = u8g2_GetStrWidth(&s_u8g2, safe_title);
    if (tw <= 128) {
        u8g2_DrawStr(&s_u8g2, (128 - tw) / 2, 28, safe_title);
    } else {
        u8g2_SetClipWindow(&s_u8g2, 0, 12, 128, 31);
        draw_marquee(&s_now_playing_marquee, safe_title, 0, 28, 1);
        u8g2_SetMaxClipWindow(&s_u8g2);
    }

    // 3. Barra de progresso com tempo embutido (Y=33..43)
    draw_progress_bar_with_time(0, 33, 128, 11, state->elapsed_sec, state->total_sec, state->duration_is_estimate);

    // 4. Album compacto (Y=44..51)
    if (state->album[0] != '\0') {
        char safe_album[64];
        sanitize_for_display(state->album, safe_album, sizeof(safe_album));
        u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
        int alw = u8g2_GetStrWidth(&s_u8g2, safe_album);
        if (alw <= 126) {
            u8g2_DrawStr(&s_u8g2, (128 - alw) / 2, 50, safe_album);
        } else {
            u8g2_SetClipWindow(&s_u8g2, 1, 45, 127, 51);
            draw_marquee(&s_album_marquee, safe_album, 1, 50, 1);
            u8g2_SetMaxClipWindow(&s_u8g2);
        }
    }

    // 5. Mini VU Estéreo L/R na base (Y=53..63)
    float peak_l = 0.0f, peak_r = 0.0f, hold_l = 0.0f, hold_r = 0.0f;
    audio_meter_get_levels(&peak_l, &peak_r, &hold_l, &hold_r, NULL, NULL);

    const int vu_x = 9;
    const int vu_w = 118; // Frame de 118px -> 116px uteis
    const int vu_inner_w = 114;

    // --- Canal L ---
    u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
    u8g2_DrawStr(&s_u8g2, 0, 57, "L");
    u8g2_DrawFrame(&s_u8g2, vu_x, 53, vu_w, 5);
    int fill_l = (int)(peak_l * (float)vu_inner_w + 0.5f);
    if (fill_l > vu_inner_w) fill_l = vu_inner_w;
    if (fill_l > 0) {
        u8g2_DrawBox(&s_u8g2, vu_x + 2, 54, fill_l, 3);
    }
    int hold_l_px = (int)(hold_l * (float)vu_inner_w + 0.5f);
    if (hold_l_px > vu_inner_w - 2) hold_l_px = vu_inner_w - 2;
    if (hold_l_px > 0) {
        u8g2_DrawBox(&s_u8g2, vu_x + 2 + hold_l_px, 54, 2, 3);
    }

    // --- Canal R ---
    u8g2_DrawStr(&s_u8g2, 0, 63, "R");
    u8g2_DrawFrame(&s_u8g2, vu_x, 59, vu_w, 5);
    int fill_r = (int)(peak_r * (float)vu_inner_w + 0.5f);
    if (fill_r > vu_inner_w) fill_r = vu_inner_w;
    if (fill_r > 0) {
        u8g2_DrawBox(&s_u8g2, vu_x + 2, 60, fill_r, 3);
    }
    int hold_r_px = (int)(hold_r * (float)vu_inner_w + 0.5f);
    if (hold_r_px > vu_inner_w - 2) hold_r_px = vu_inner_w - 2;
    if (hold_r_px > 0) {
        u8g2_DrawBox(&s_u8g2, vu_x + 2 + hold_r_px, 60, 2, 3);
    }
}

static void oled_display_show_now_playing_spectrum(const playback_state_t *state)
{
    float bands[AUDIO_METER_SPECTRUM_BANDS] = {0};
    float peaks[AUDIO_METER_SPECTRUM_BANDS] = {0};
    audio_meter_get_spectrum_bands(bands, peaks);

    // Linha de base estetica
    u8g2_DrawHLine(&s_u8g2, 2, 63, 124);

    // 11 bandas de espectro: largura 7px, gap 4px, x0 = 5 (11*7 + 10*4 = 117 px)
    const int bar_w = 7;
    const int bar_step = 11;
    const int x0 = 5;
    const int max_h = 50;

    for (int i = 0; i < AUDIO_METER_SPECTRUM_BANDS; i++) {
        int bx = x0 + i * bar_step;
        int h = (int)(bands[i] * (float)max_h + 0.5f);
        if (h > max_h) h = max_h;

        if (h > 0) {
            // Segmentos Hi-Fi (2px de altura com 1px de espacamento)
            for (int by = 61; by > 61 - h; by -= 3) {
                u8g2_DrawBox(&s_u8g2, bx, by - 1, bar_w, 2);
            }
        }

        // Marcador flutuante de pico (Peak-Hold)
        int ph = (int)(peaks[i] * (float)max_h + 0.5f);
        if (ph > max_h) ph = max_h;
        if (ph < h) ph = h;
        int peak_y = 61 - ph - 2;
        if (peak_y >= 2) {
            u8g2_DrawHLine(&s_u8g2, bx, peak_y, bar_w);
        }
    }

    // Overlay dinamico com titulo e tempo (some apos 3s)
    draw_viz_dynamic_overlay(state);
}

static void oled_display_show_now_playing_analog(const playback_state_t *state)
{
    float db_l = -40.0f, db_r = -40.0f;
    audio_meter_get_levels(NULL, NULL, NULL, NULL, &db_l, &db_r);

    // Molduras vintage dos 2 medidores estilo McIntosh / Technics
    u8g2_DrawFrame(&s_u8g2, 2, 12, 60, 52);
    u8g2_DrawFrame(&s_u8g2, 66, 12, 60, 52);

    u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
    u8g2_DrawStr(&s_u8g2, 5, 20, "L");
    u8g2_DrawStr(&s_u8g2, 69, 20, "R");

    // Escala com marcas graduadas nos 2 medidores
    // Canal L: Pivô (32, 63), Raio 35. Canal R: Pivô (96, 63), Raio 35
    static const int8_t s_scale_deg[6] = { -34, -18, -5, 7, 20, 34 };

    for (int m = 0; m < 6; m++) {
        int deg = s_scale_deg[m];
        int sin_q10 = fast_sin_q10(deg);
        int cos_q10 = fast_cos_q10(deg);

        int dx32 = fast_scale_trig(32, sin_q10);
        int dy32 = fast_scale_trig(32, cos_q10);
        int dx36 = fast_scale_trig(36, sin_q10);
        int dy36 = fast_scale_trig(36, cos_q10);

        // Arco do Canal L
        u8g2_DrawLine(&s_u8g2, 32 + dx32, 63 - dy32, 32 + dx36, 63 - dy36);

        // Arco do Canal R
        u8g2_DrawLine(&s_u8g2, 96 + dx32, 63 - dy32, 96 + dx36, 63 - dy36);
    }

    // Texto de decibeis
    u8g2_DrawStr(&s_u8g2, 28, 22, "dB");
    u8g2_DrawStr(&s_u8g2, 92, 22, "dB");

    // --- Ponteiro Canal L ---
    float norm_l = (db_l - (-40.0f)) / 43.0f; // Mapeia -40dB a +3dB
    if (norm_l < 0.0f) norm_l = 0.0f;
    if (norm_l > 1.0f) norm_l = 1.0f;
    int ang_l_deg = -34 + (int)(norm_l * 68.0f + 0.5f);
    int px_l = 32 + fast_scale_trig(34, fast_sin_q10(ang_l_deg));
    int py_l = 63 - fast_scale_trig(34, fast_cos_q10(ang_l_deg));
    u8g2_DrawLine(&s_u8g2, 32, 63, px_l, py_l);
    u8g2_DrawDisc(&s_u8g2, 32, 63, 2, U8G2_DRAW_ALL);

    // --- Ponteiro Canal R ---
    float norm_r = (db_r - (-40.0f)) / 43.0f;
    if (norm_r < 0.0f) norm_r = 0.0f;
    if (norm_r > 1.0f) norm_r = 1.0f;
    int ang_r_deg = -34 + (int)(norm_r * 68.0f + 0.5f);
    int px_r = 96 + fast_scale_trig(34, fast_sin_q10(ang_r_deg));
    int py_r = 63 - fast_scale_trig(34, fast_cos_q10(ang_r_deg));
    u8g2_DrawLine(&s_u8g2, 96, 63, px_r, py_r);
    u8g2_DrawDisc(&s_u8g2, 96, 63, 2, U8G2_DRAW_ALL);

    // Overlay dinamico com titulo e tempo (some apos 3s)
    draw_viz_dynamic_overlay(state);
}

void oled_display_show_now_playing(const playback_state_t *state)
{
    if (!s_ready || !state) return;

    // Detecta troca de musica para engatilhar overlay dinamico de 3s
    if (strncmp(state->title, s_last_track_title, sizeof(s_last_track_title)) != 0) {
        strncpy(s_last_track_title, state->title, sizeof(s_last_track_title) - 1);
        s_last_track_title[sizeof(s_last_track_title) - 1] = '\0';
        oled_display_trigger_viz_overlay();
    }

    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);

    switch (s_viz_mode) {
        case VIZ_MODE_HYBRID:
            oled_display_show_now_playing_hybrid(state);
            break;
        case VIZ_MODE_SPECTRUM:
            oled_display_show_now_playing_spectrum(state);
            break;
        case VIZ_MODE_ANALOG:
            oled_display_show_now_playing_analog(state);
            break;
        case VIZ_MODE_CLASSIC:
        default:
            oled_display_show_now_playing_classic(state);
            break;
    }

    apply_brightness_if_needed();
    oled_flush_buffer();
}

static marquee_t s_list_topbar_marquee = {0};

void oled_display_show_list(const char **names, int count, int cursor, const playback_state_t *now_playing)
{
    if (!s_ready) return;

    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);
    
    bool show_topbar = now_playing && now_playing->track_loaded;
    const int list_top = show_topbar ? 10 : 0;
    
    if (show_topbar) {
        // --- Modura top bar ---
        u8g2_DrawLine(&s_u8g2, 0, 0, 0, 8);
        u8g2_DrawLine(&s_u8g2, 127, 0, 127, 8);
        u8g2_DrawHLine(&s_u8g2, 0, 8, 128);
        
        // --- Barra de progresso (fill_w) ---
        if (now_playing->total_sec > 0) {
            int fill_w = (int)(((long)128 * now_playing->elapsed_sec) / now_playing->total_sec);
            if (fill_w > 128) fill_w = 128;
            if (fill_w > 0) {
                u8g2_DrawBox(&s_u8g2, 0, 0, fill_w, 8);
            }
        }
        
        // --- Textos em XOR mode ---
        u8g2_SetFontMode(&s_u8g2, 1);
        u8g2_SetDrawColor(&s_u8g2, 2);
        
        char elapsed_str[8], total_str[8];
        format_time(now_playing->elapsed_sec, elapsed_str, sizeof(elapsed_str));
        format_time(now_playing->total_sec, total_str, sizeof(total_str));
        
        int text_y = 6; // Baseline para a fonte do titulo
        int mini_y = 2; // Y top-left para a mini fonte (altura 5) dentro do topbar (altura 9)
        
        oled_draw_mini_text(2, mini_y, elapsed_str);
        
        int total_w = text_width_px(total_str);
        oled_draw_mini_text(126 - total_w, mini_y, total_str);
        
        // --- Titulo da musica ---
        u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
        char safe_title[128];
        sanitize_for_display(now_playing->title, safe_title, sizeof(safe_title));
        int elapsed_w = text_width_px(elapsed_str);
        int title_x0 = 2 + elapsed_w + 3;
        int title_x1 = 126 - total_w - 3;
        int title_area_w = title_x1 - title_x0;
        
        if (title_area_w > 0) {
            int title_w = u8g2_GetStrWidth(&s_u8g2, safe_title);
            u8g2_SetClipWindow(&s_u8g2, title_x0, 0, title_x1, 8);
            if (title_w <= title_area_w) {
                int tx = title_x0 + (title_area_w - title_w) / 2;
                u8g2_DrawStr(&s_u8g2, tx, text_y, safe_title);
            } else {
                draw_marquee_width(&s_list_topbar_marquee, safe_title, title_x0, title_area_w, text_y, 1);
            }
            u8g2_SetMaxClipWindow(&s_u8g2);
        }
        u8g2_SetDrawColor(&s_u8g2, 1); // Restaura cor normal
    }

    if (count <= 0) {
        u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
        u8g2_DrawStr(&s_u8g2, 1, list_top + 9, "Lista vazia");
        apply_brightness_if_needed();
        oled_flush_buffer();
        return;
    }

    if (cursor < 0) cursor = 0;
    if (cursor > count - 1) cursor = count - 1;

    // --- Configurações da lista ---
    const int row_h = 11;
    const int list_h = 64 - list_top;
    const int rows_visible = (list_h + row_h - 1) / row_h; // teto da divisão, permite 5 linhas mesmo cortando o último pixel

    static int s_window_start = 0;
    if (cursor == 0) s_window_start = 0;
    else if (cursor < s_window_start) {
        s_window_start = cursor;
    } else if (cursor >= s_window_start + rows_visible) {
        s_window_start = cursor - rows_visible + 1;
    }
    if (s_window_start > count - rows_visible) s_window_start = count - rows_visible;
    if (s_window_start < 0) s_window_start = 0;

    // Calcula scrollbar dinamicamente
    if (count > rows_visible) {
        int sb_y = list_top + 1;
        int sb_h = list_h - 2;
        int thumb_h = (sb_h * rows_visible) / count;
        if (thumb_h < 4) thumb_h = 4;
        
        int max_scroll = count - rows_visible;
        int thumb_y = sb_y + ((sb_h - thumb_h) * s_window_start) / max_scroll;
        
        u8g2_DrawFrame(&s_u8g2, 125, thumb_y, 3, thumb_h);
    }
    
    // --- Renderiza itens ---
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    char safe_name[64];
    for (int i = 0; i < rows_visible; i++) {
        int idx = s_window_start + i;
        if (idx >= count) break;
        
        int y_box = list_top + i * row_h;
        int y_text = y_box + 8; // baseline
        
        sanitize_for_display(names[idx], safe_name, sizeof(safe_name));
        
        if (idx == cursor) {
            u8g2_SetDrawColor(&s_u8g2, 1);
            u8g2_DrawBox(&s_u8g2, 0, y_box, 124, row_h);
            
            u8g2_SetDrawColor(&s_u8g2, 0);
            u8g2_SetClipWindow(&s_u8g2, 1, y_box, 123, y_box + row_h);
            int tw = u8g2_GetStrWidth(&s_u8g2, safe_name);
            if (tw <= 122) {
                u8g2_DrawStr(&s_u8g2, 1, y_text, safe_name);
            } else {
                draw_marquee(&s_list_cursor_marquee, safe_name, 1, y_text, 1);
            }
            u8g2_SetMaxClipWindow(&s_u8g2);
            u8g2_SetDrawColor(&s_u8g2, 1);
        } else {
            u8g2_SetClipWindow(&s_u8g2, 1, y_box, 123, y_box + row_h);
            u8g2_DrawStr(&s_u8g2, 1, y_text, safe_name);
            u8g2_SetMaxClipWindow(&s_u8g2);
        }
    }
    
    apply_brightness_if_needed();
    oled_flush_buffer();
}

void oled_display_show_message(const char *line1, const char *line2)
{
    if (!s_ready) return;
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    u8g2_DrawStr(&s_u8g2, 0, 12, line1);
    if (line2) {
        u8g2_DrawStr(&s_u8g2, 0, 26, line2);
    }
    apply_brightness_if_needed();
    oled_flush_buffer();
}

void oled_display_show_bt_devices(const char **names, int count, int cursor, bool scanning)
{
    if (!s_ready) return;
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);

    // Cabecalho
    u8g2_SetFont(&s_u8g2, u8g2_font_profont11_tr);
    u8g2_DrawStr(&s_u8g2, 0, 9, "Bluetooth");
    u8g2_SetFont(&s_u8g2, u8g2_font_5x7_tf);
    if (scanning) {
        u8g2_DrawStr(&s_u8g2, 64, 9, "[Buscando...]");
    } else {
        u8g2_DrawStr(&s_u8g2, 85, 9, "[Pronto]");
    }
    u8g2_DrawHLine(&s_u8g2, 0, 11, 128);

    // Janela de visualizacao de 3 itens com scroll
    int start_idx = 0;
    if (cursor >= 3) {
        start_idx = cursor - 2;
    }
    if (start_idx + 3 > count && count >= 3) {
        start_idx = count - 3;
    }
    if (start_idx < 0) start_idx = 0;

    int y = 23;
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    for (int i = start_idx; i < count && i < start_idx + 3; i++) {
        if (i == cursor) {
            u8g2_DrawBox(&s_u8g2, 0, y - 9, 128, 12);
            u8g2_SetDrawColor(&s_u8g2, 0); // texto invertido
            u8g2_DrawStr(&s_u8g2, 2, y, names[i]);
            u8g2_SetDrawColor(&s_u8g2, 1); // restaura
        } else {
            u8g2_DrawStr(&s_u8g2, 2, y, names[i]);
        }
        y += 13;
    }

    // Rodape
    u8g2_SetFont(&s_u8g2, u8g2_font_5x7_tf);
    u8g2_DrawStr(&s_u8g2, 0, 62, "Dir/Centro: OK | Segure Esq: Sair");

    apply_brightness_if_needed();
    oled_flush_buffer();
}

void oled_display_show_bt_connected(const char *dev_name, const char *codec_name, uint32_t sample_rate, int8_t rssi)
{
    if (!s_ready) return;
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);

    // Cabecalho
    u8g2_SetFont(&s_u8g2, u8g2_font_profont11_tr);
    u8g2_DrawStr(&s_u8g2, 0, 9, "BT Conectado");
    u8g2_DrawHLine(&s_u8g2, 0, 11, 128);

    // Nome do fone
    u8g2_SetFont(&s_u8g2, u8g2_font_6x12_tr);
    if (dev_name && dev_name[0] != '\0') {
        u8g2_DrawStr(&s_u8g2, 0, 24, dev_name);
    } else {
        u8g2_DrawStr(&s_u8g2, 0, 24, "Fone Pareado");
    }

    // Codec e taxa
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    char line[32];
    snprintf(line, sizeof(line), "%s %u.%ukHz",
             codec_name ? codec_name : "SBC",
             (unsigned)(sample_rate / 1000), (unsigned)((sample_rate / 100) % 10));
    u8g2_DrawStr(&s_u8g2, 0, 37, line);

    if (rssi != 0) {
        snprintf(line, sizeof(line), "Sinal: %d dBm", rssi);
        u8g2_DrawStr(&s_u8g2, 0, 49, line);
    }

    // Rodape
    u8g2_SetFont(&s_u8g2, u8g2_font_5x7_tf);
    u8g2_DrawStr(&s_u8g2, 0, 62, "Dir: Testar Som | O: Descon");

    apply_brightness_if_needed();
    oled_flush_buffer();
}

void oled_display_blank(void)
{
    if (!s_ready) return;
    u8g2_ClearBuffer(&s_u8g2);
    apply_brightness_if_needed();
    s_fb_dirty_all = true;
    oled_flush_buffer();
}

void oled_display_show_volume(int volume_percent)
{
    if (!s_ready) return;
    if (volume_percent < 0) volume_percent = 0;
    if (volume_percent > 100) volume_percent = 100;

    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);

    char label[16];
    snprintf(label, sizeof(label), "Volume %d%%", volume_percent);
    int label_w = u8g2_GetStrWidth(&s_u8g2, label);
    u8g2_DrawStr(&s_u8g2, (OLED_WIDTH - label_w) / 2, 20, label);

    const int bar_x = 8, bar_y = 34, bar_w = OLED_WIDTH - 16, bar_h = 14;
    u8g2_DrawFrame(&s_u8g2, bar_x, bar_y, bar_w, bar_h);
    int fill_w = ((bar_w - 2) * volume_percent) / 100;
    if (fill_w > 0) {
        u8g2_DrawBox(&s_u8g2, bar_x + 1, bar_y + 1, fill_w, bar_h - 2);
    }

    apply_brightness_if_needed();
    oled_flush_buffer();
}

void oled_display_show_balance(int balance)
{
    if (!s_ready) return;
    if (balance < -100) balance = -100;
    if (balance > 100) balance = 100;

    int pct_L = 100;
    int pct_R = 100;
    if (balance < 0) {
        pct_R = 100 + balance;
    } else if (balance > 0) {
        pct_L = 100 - balance;
    }

    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);

    // Titulo
    const char *title = "BALANCO L / R";
    int tw = u8g2_GetStrWidth(&s_u8g2, title);
    u8g2_DrawStr(&s_u8g2, (OLED_WIDTH - tw) / 2, 14, title);

    // Valores em escala de 3 digitos (000% a 100%)
    char str_L[16], str_R[16];
    snprintf(str_L, sizeof(str_L), "L: %03d%%", pct_L);
    snprintf(str_R, sizeof(str_R), "R: %03d%%", pct_R);
    u8g2_DrawStr(&s_u8g2, 8, 28, str_L);
    int rw = u8g2_GetStrWidth(&s_u8g2, str_R);
    u8g2_DrawStr(&s_u8g2, OLED_WIDTH - 8 - rw, 28, str_R);

    // Indicador central
    char c_str[16];
    if (balance == 0) {
        snprintf(c_str, sizeof(c_str), "CTR");
    } else if (balance < 0) {
        snprintf(c_str, sizeof(c_str), "L%03d", -balance);
    } else {
        snprintf(c_str, sizeof(c_str), "R%03d", balance);
    }
    int cw = u8g2_GetStrWidth(&s_u8g2, c_str);
    u8g2_DrawStr(&s_u8g2, (OLED_WIDTH - cw) / 2, 28, c_str);

    // Barra de balanco (X=10 a 118, Y=36 a 46)
    const int bar_x = 10, bar_y = 35, bar_w = OLED_WIDTH - 20, bar_h = 11;
    const int mid_x = bar_x + bar_w / 2; // 64
    u8g2_DrawFrame(&s_u8g2, bar_x, bar_y, bar_w, bar_h);

    // Linha central do balanço
    u8g2_DrawLine(&s_u8g2, mid_x, bar_y - 2, mid_x, bar_y + bar_h + 1);

    // Preenchimento indicador a partir do centro
    int cur_x = mid_x + (balance * (bar_w / 2 - 3)) / 100;
    if (balance < 0) {
        u8g2_DrawBox(&s_u8g2, cur_x, bar_y + 2, mid_x - cur_x, bar_h - 4);
    } else if (balance > 0) {
        u8g2_DrawBox(&s_u8g2, mid_x, bar_y + 2, cur_x - mid_x, bar_h - 4);
    }
    // Cursor pontual destacado
    u8g2_DrawBox(&s_u8g2, cur_x - 1, bar_y + 1, 3, bar_h - 2);

    // Dicas no rodape
    u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
    const char *hint = "< L / R > Ajustar   [OK] Voltar";
    int hw = u8g2_GetStrWidth(&s_u8g2, hint);
    u8g2_DrawStr(&s_u8g2, (OLED_WIDTH - hw) / 2, 60, hint);

    apply_brightness_if_needed();
    oled_flush_buffer();
}

static marquee_t s_transfer_marquee = {0};

// Tela de transferencia: rotulo (ex: status da rede), nome do arquivo
// (rolando se for longo) e barra de progresso com % e tempo estimado
// embaixo. percent fora de 0-100 e' grampeado; eta_sec < 0 mostra
// "calculando..." em vez de um numero (usado enquanto ainda nao ha' taxa
// de transferencia confiavel medida). overall_pct/count descrevem o
// progresso de um LOTE de varios arquivos (upload WiFi em lote); passe
// overall_pct=-1 e count<=1 quando so' houver um arquivo por vez (ex.:
// modo USB nao tem esse conceito) - a segunda barra so' aparece se
// count>1 e overall_pct>=0.

void oled_display_show_transfer(const char *label, const char *filename, int percent, int eta_sec,
                                 int overall_pct, int count)
{
    if (!s_ready) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);

    // ---- Layout com barras de mesma altura (12px) ----
    const int label_y = 8;
    const int name_y = 20;
    const int bar_h = 12;                 // mesma altura para as duas
    const int bar1_y = 24;
    const int bar2_y = 38;                // 42 - 28 = 14px de espaÃ§amento (bar_h + 2)
    const int eta_y = 60;

    // ---- Status ----
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    char safe_label[64];
    sanitize_for_display(label, safe_label, sizeof(safe_label));
    u8g2_DrawStr(&s_u8g2, 0, label_y, safe_label);

    // ---- Nome do arquivo (marquee com clip) ----
    u8g2_SetFont(&s_u8g2, u8g2_font_6x13_tr);
    char safe_name[128];
    sanitize_for_display(filename && filename[0] ? filename : "aguardando...", safe_name, sizeof(safe_name));
    int ascent = 10;
    int descent = 3;
    int margin = 1;
    u8g2_SetClipWindow(&s_u8g2, 0, name_y - ascent - margin, OLED_WIDTH, name_y + descent + margin);
    draw_marquee(&s_transfer_marquee, safe_name, 0, name_y, 2);
    u8g2_SetMaxClipWindow(&s_u8g2);

    // ---- FunÃ§Ã£o auxiliar para desenhar uma barra ----
    // (inline para evitar duplicaÃ§Ã£o)
    const int bar_x = 4;
    const int bar_w = OLED_WIDTH - 8;

    // Barra 1: arquivo atual
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    u8g2_DrawFrame(&s_u8g2, bar_x, bar1_y, bar_w, bar_h);
    int fill1 = ((bar_w - 2) * percent) / 100;
    if (fill1 > 0) {
        u8g2_DrawBox(&s_u8g2, bar_x + 1, bar1_y + 1, fill1, bar_h - 2);
    }
    char pct_label[8];
    snprintf(pct_label, sizeof(pct_label), "%d%%", percent);
    int pct_w = u8g2_GetStrWidth(&s_u8g2, pct_label);
    int pct_x = bar_x + (bar_w - pct_w) / 2;
    bool over_fill = (pct_x - bar_x) < fill1;
    u8g2_SetDrawColor(&s_u8g2, over_fill ? 0 : 1);
    u8g2_DrawStr(&s_u8g2, pct_x, bar1_y + 10, pct_label);
    u8g2_SetDrawColor(&s_u8g2, 1);

    // Barra 2: progresso geral (sÃ³ se count > 1 e overall_pct >= 0)
    if (count > 1 && overall_pct >= 0) {
        u8g2_DrawFrame(&s_u8g2, bar_x, bar2_y, bar_w, bar_h);
        int fill2 = ((bar_w - 2) * overall_pct) / 100;
        if (fill2 > 0) {
            u8g2_DrawBox(&s_u8g2, bar_x + 1, bar2_y + 1, fill2, bar_h - 2);
        }
        char overall_label[16];
        snprintf(overall_label, sizeof(overall_label), "Geral %d%%", overall_pct);
        int ol_w = u8g2_GetStrWidth(&s_u8g2, overall_label);
        int ol_x = bar_x + (bar_w - ol_w) / 2;
        bool over_fill2 = (ol_x - bar_x) < fill2;
        u8g2_SetDrawColor(&s_u8g2, over_fill2 ? 0 : 1);
        u8g2_DrawStr(&s_u8g2, ol_x, bar2_y + 10, overall_label);
        u8g2_SetDrawColor(&s_u8g2, 1);
    }

    // ---- ETA ----
    char eta_label[32];
    if (percent >= 100) {
        snprintf(eta_label, sizeof(eta_label), "Concluido");
    } else if (eta_sec < 0) {
        snprintf(eta_label, sizeof(eta_label), "Calculando tempo...");
    } else if (eta_sec < 60) {
        snprintf(eta_label, sizeof(eta_label), "~%ds restantes", eta_sec);
    } else {
        snprintf(eta_label, sizeof(eta_label), "~%dmin restantes", (eta_sec + 30) / 60);
    }
    int eta_w = u8g2_GetStrWidth(&s_u8g2, eta_label);
    u8g2_DrawStr(&s_u8g2, (OLED_WIDTH - eta_w) / 2, eta_y, eta_label);

    apply_brightness_if_needed();
    oled_flush_buffer();
}
// =========================================================================
// Game of Life - tela de bloqueio. Grade de celulas maiores que 1px (ver
// LIFE_CELL_PX) pra' ficar visivel numa tela de 128x64; wraparound nas
// bordas (topologia toroidal, sem "morte" artificial na borda); reseeda
// sozinho se o padrao morrer todo ou travar num ciclo estatico por um
// tempo, pra' nao ficar horas com a mesma imagem parada enquanto
// bloqueado.
// =========================================================================
#define LIFE_CELL_PX 1
#define LIFE_COLS (OLED_WIDTH / LIFE_CELL_PX)   // 128
#define LIFE_ROWS (OLED_HEIGHT / LIFE_CELL_PX)  // 64
#define LIFE_SEED_DENSITY_PCT 28  // % de celulas vivas ao semear - denso
                                  // demais morre rapido por superlotacao,
                                  // esparso demais morre por falta de
                                  // vizinhos; 28% costuma dar padroes com
                                  // vida razoavelmente longa.
#define LIFE_STAGNANT_LIMIT   20 // geracoes sem mudanca (ou tudo morto)
                                  // antes de semear de novo sozinho.

static uint8_t s_life_grid[LIFE_ROWS][LIFE_COLS];
static uint8_t s_life_next[LIFE_ROWS][LIFE_COLS];
static bool s_life_seeded = false;
static int s_life_stagnant_gens = 0;

static void life_seed_random(void)
{
    for (int y = 0; y < LIFE_ROWS; y++) {
        for (int x = 0; x < LIFE_COLS; x++) {
            s_life_grid[y][x] = ((int)(esp_random() % 100) < LIFE_SEED_DENSITY_PCT) ? 1 : 0;
        }
    }
    s_life_stagnant_gens = 0;
}

void oled_display_reset_life(void)
{
    life_seed_random();
    s_life_seeded = true;
}

static int life_count_neighbors(int x, int y)
{
    int count = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            int nx = (x + dx + LIFE_COLS) % LIFE_COLS; // wraparound horizontal
            int ny = (y + dy + LIFE_ROWS) % LIFE_ROWS; // wraparound vertical
            count += s_life_grid[ny][nx];
        }
    }
    return count;
}

static void life_step(void)
{
    bool changed = false;
    int alive_count = 0;

    for (int y = 0; y < LIFE_ROWS; y++) {
        for (int x = 0; x < LIFE_COLS; x++) {
            int n = life_count_neighbors(x, y);
            bool alive = s_life_grid[y][x] != 0;
            bool next_alive = alive ? (n == 2 || n == 3) : (n == 3); // regras classicas B3/S23
            s_life_next[y][x] = next_alive ? 1 : 0;
            if (next_alive != alive) changed = true;
            if (next_alive) alive_count++;
        }
    }
    memcpy(s_life_grid, s_life_next, sizeof(s_life_grid));

    if (!changed || alive_count == 0) {
        s_life_stagnant_gens++;
    } else {
        s_life_stagnant_gens = 0;
    }
    if (s_life_stagnant_gens >= LIFE_STAGNANT_LIMIT) {
        life_seed_random();
    }
}

void oled_display_show_locked(void)
{
    if (!s_ready) return;
    if (!s_life_seeded) {
        life_seed_random();
        s_life_seeded = true;
    }
    life_step(); // 1 geracao por chamada - o chamador ja' repete isso a cada 250ms

    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);
    for (int y = 0; y < LIFE_ROWS; y++) {
        for (int x = 0; x < LIFE_COLS; x++) {
            if (s_life_grid[y][x]) {
                u8g2_DrawBox(&s_u8g2, x * LIFE_CELL_PX, y * LIFE_CELL_PX, LIFE_CELL_PX, LIFE_CELL_PX);
            }
        }
    }
    apply_brightness_if_needed();
    oled_flush_buffer();
}

void oled_display_show_usb_mode(void)
{
    if (!s_ready) return;
    
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);
    
    // ConfiguraÃ§Ã£o para exibir bitmaps corretamente
    u8g2_SetFontMode(&s_u8g2, 1);
    u8g2_SetBitmapMode(&s_u8g2, 1);
    
    // Desenha o Ã­cone do Drive USB usando as dimensÃµes definidas no bitmaps.h
    u8g2_DrawXBMP(&s_u8g2, 9, 12, BITMAP_DRIVE_W, BITMAP_DRIVE_H, image_Drive_bits);
    
    // Desenha o Ã­cone da Ãrvore USB usando as dimensÃµes definidas no bitmaps.h
    u8g2_DrawXBMP(&s_u8g2, 31, 17, BITMAP_USBTREE_W, BITMAP_USBTREE_H, image_UsbTree_bits);
    
    apply_brightness_if_needed();
    oled_flush_buffer();
}

// Controle de animação do menu principal
static int wifi_frame = 0;
static int conf_frame = 0;
static int game_frame = 0;
static int s_menu_anim_tick = 0;
static uint32_t last_anim_ms = 0;
#define ANIM_INTERVAL_MS 42   // ~24 fps (mesmo ritmo dos mockups)

void oled_display_update_animations(void) {
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // Atualiza animações WiFi a cada 42ms (~24fps)
    if (now - s_wifi_last_frame_ms >= 42) {
        s_wifi_attention_frame = (s_wifi_attention_frame + 1) % WIFI_ATTENTION_CLOUD_FRAMES;
        s_wifi_sync_frame = (s_wifi_sync_frame + 1) % WIFI_SYNC_CLOUD_FRAMES;
        s_wifi_download_frame = (s_wifi_download_frame + 1) % WIFI_DOWNLOAD_CLOUD_FRAMES;
        s_wifi_last_frame_ms = now;
    }

    // Atualiza animações do menu principal (mesmo ritmo)
    if (now - last_anim_ms >= ANIM_INTERVAL_MS) {
        wifi_frame = (wifi_frame + 1) % MENU_WIFI_FRAMES;
        conf_frame = (conf_frame + 1) % MENU_CONF_FRAMES;
        game_frame = (game_frame + 1) % MENU_GAME_FRAMES;
        s_menu_anim_tick++;
        last_anim_ms = now;
    }
}

// Ícone WiFi grande (64x46) extraído pixel a pixel do mockup do usuário
#include "menu_wifi_big.inc"

static bool s_menu_anim_enabled = true;

bool oled_display_is_anim_enabled(void)
{
    return s_menu_anim_enabled;
}

void oled_display_set_anim_enabled(bool en)
{
    s_menu_anim_enabled = en;
}

// Tag com cantos chanfrados em 45 graus (superior-esquerdo e inferior-direito)
static void draw_menu_chamfered_tag(int x, int y, int w, int h)
{
    u8g2_SetDrawColor(&s_u8g2, 1);
    for (int r = 0; r < h; r++) {
        int x_start = x;
        int x_end = x + w - 1;
        if (r == 0) x_start = x + 3;
        else if (r == 1) x_start = x + 2;
        else if (r == 2) x_start = x + 1;

        if (r == h - 3) x_end = x + w - 2;
        else if (r == h - 2) x_end = x + w - 3;
        else if (r == h - 1) x_end = x + w - 4;

        if (x_end >= x_start) {
            u8g2_DrawHLine(&s_u8g2, x_start, y + r, x_end - x_start + 1);
        }
    }
}

// Rosa dos Ventos / Navigation Compass (9x9 pixels)
static void draw_nav_compass(int cx, int cy, bool up, bool down, bool left, bool right, bool center)
{
    u8g2_SetDrawColor(&s_u8g2, 1);
    if (up) {
        u8g2_DrawPixel(&s_u8g2, cx, cy - 4);
        u8g2_DrawPixel(&s_u8g2, cx - 1, cy - 3);
        u8g2_DrawPixel(&s_u8g2, cx + 1, cy - 3);
    }
    if (down) {
        u8g2_DrawPixel(&s_u8g2, cx - 1, cy + 3);
        u8g2_DrawPixel(&s_u8g2, cx + 1, cy + 3);
        u8g2_DrawPixel(&s_u8g2, cx, cy + 4);
    }
    if (left) {
        u8g2_DrawPixel(&s_u8g2, cx - 3, cy - 1);
        u8g2_DrawPixel(&s_u8g2, cx - 4, cy);
        u8g2_DrawPixel(&s_u8g2, cx - 3, cy + 1);
    }
    if (right) {
        u8g2_DrawPixel(&s_u8g2, cx + 3, cy - 1);
        u8g2_DrawPixel(&s_u8g2, cx + 4, cy);
        u8g2_DrawPixel(&s_u8g2, cx + 3, cy + 1);
    }
    if (center) {
        u8g2_DrawPixel(&s_u8g2, cx, cy);
    }
}

// Badge de Bateria no canto inferior direito [59] (11x9 pixels)
static void draw_battery_badge(int x, int y, int percent)
{
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_DrawFrame(&s_u8g2, x, y, 11, 9);
    if (percent < 0) percent = 0;
    if (percent > 99) percent = 99;
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", percent);
    oled_draw_mini_text(x + 2, y + 2, buf);
}

// Barra superior de reprodução no Menu Principal
static void draw_main_menu_topbar(const playback_state_t *now_playing)
{
    // Moldura lateral e linha divisória inferior
    u8g2_DrawLine(&s_u8g2, 0, 0, 0, 8);
    u8g2_DrawLine(&s_u8g2, 127, 0, 127, 8);
    u8g2_DrawHLine(&s_u8g2, 0, 8, 128);

    // Barra de progresso (fill_w)
    if (now_playing->total_sec > 0) {
        int fill_w = (int)(((long)128 * now_playing->elapsed_sec) / now_playing->total_sec);
        if (fill_w > 128) fill_w = 128;
        if (fill_w > 0) {
            u8g2_DrawBox(&s_u8g2, 0, 0, fill_w, 8);
        }
    }

    // Textos em modo XOR
    u8g2_SetFontMode(&s_u8g2, 1);
    u8g2_SetDrawColor(&s_u8g2, 2);

    char elapsed_str[8], total_str[8];
    format_time(now_playing->elapsed_sec, elapsed_str, sizeof(elapsed_str));
    format_time(now_playing->total_sec, total_str, sizeof(total_str));

    int mini_y = 2; // Y para mini-fonte (altura 5) dentro de Y=0..8
    oled_draw_mini_text(2, mini_y, elapsed_str);

    int total_w = text_width_px(total_str);
    oled_draw_mini_text(126 - total_w, mini_y, total_str);

    // Título da música
    u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
    char safe_title[128];
    sanitize_for_display(now_playing->title, safe_title, sizeof(safe_title));
    int elapsed_w = text_width_px(elapsed_str);
    int title_x0 = 2 + elapsed_w + 3;
    int title_x1 = 126 - total_w - 3;
    int title_area_w = title_x1 - title_x0;

    if (title_area_w > 0) {
        int title_w = u8g2_GetStrWidth(&s_u8g2, safe_title);
        u8g2_SetClipWindow(&s_u8g2, title_x0, 0, title_x1, 8);
        if (title_w <= title_area_w) {
            int tx = title_x0 + (title_area_w - title_w) / 2;
            u8g2_DrawStr(&s_u8g2, tx, 6, safe_title);
        } else {
            draw_marquee_width(&s_list_topbar_marquee, safe_title, title_x0, title_area_w, 6, 1);
        }
        u8g2_SetMaxClipWindow(&s_u8g2);
    }
    u8g2_SetDrawColor(&s_u8g2, 1);
}

// -------------------------------------------------------------------------
// ÍCONES ANIMADOS DO MENU PRINCIPAL (Área Segura: X in [42, 108], Y in [14, 60])
// -------------------------------------------------------------------------

static void draw_animated_play_icon(int frame)
{
    u8g2_SetDrawColor(&s_u8g2, 1);

    // 1. Linha de base tática em Y=58
    u8g2_DrawHLine(&s_u8g2, 48, 58, 56);

    // 2. 11 Barras de espectro sonoro altas e densas (3px largura, 2px gap) com Peak-Hold flutuante
    // Cobrem horizontalmente de X=49 a X=103 (largura total 53px, perfeitamente centrada em CX=76)
    // Alturas de 12px a 40px (atingindo Y=17, com 9px limpos de respiro abaixo da topbar)
    static const uint8_t s_heights[16][11] = {
        { 14, 22, 32, 38, 28, 40, 34, 38, 26, 18, 12 },
        { 18, 28, 36, 32, 36, 34, 38, 32, 30, 24, 16 },
        { 24, 34, 40, 26, 38, 28, 32, 26, 34, 28, 20 },
        { 28, 38, 32, 20, 32, 22, 26, 20, 38, 32, 24 },
        { 34, 40, 24, 16, 26, 18, 22, 16, 32, 36, 28 },
        { 38, 32, 18, 12, 20, 14, 18, 22, 26, 38, 32 },
        { 32, 24, 14, 18, 16, 20, 24, 28, 20, 32, 36 },
        { 26, 18, 20, 24, 22, 26, 30, 34, 16, 26, 30 },
        { 20, 14, 26, 30, 28, 32, 36, 38, 14, 20, 24 },
        { 16, 20, 32, 36, 34, 38, 40, 32, 20, 16, 18 },
        { 12, 26, 38, 40, 38, 40, 34, 26, 26, 14, 14 },
        { 16, 32, 40, 34, 40, 34, 28, 20, 32, 20, 12 },
        { 22, 38, 34, 28, 34, 28, 22, 16, 36, 26, 16 },
        { 28, 40, 28, 22, 28, 22, 18, 22, 40, 32, 22 },
        { 32, 34, 22, 16, 22, 18, 24, 28, 34, 38, 26 },
        { 22, 28, 16, 22, 18, 24, 30, 34, 28, 30, 18 },
    };

    int f_idx = s_menu_anim_enabled ? (frame % 16) : 0;
    int x_start = 49;
    int base_y = 57;

    for (int i = 0; i < 11; i++) {
        int h = s_heights[f_idx][i];
        int bx = x_start + i * 5;
        u8g2_DrawBox(&s_u8g2, bx, base_y - h, 3, h);
        int peak_y = base_y - h - 3;
        if (peak_y >= 12) {
            u8g2_DrawHLine(&s_u8g2, bx, peak_y, 3);
        }
    }

    // 3. Triângulo Play central: interior apagado em 0 (recorta as barras) e borda delimitada em 1
    // Centralizado visualmente em CX=76, CY=37, altura=23px, base=19px
    int play_x0 = 67;
    int play_cy = 37;
    int play_h = 23;
    int half_h = play_h / 2;
    int w_total = 19;

    // Passo A: Preenche o interior do triângulo com cor 0 (preto), apagando as barras que passam por trás
    u8g2_SetDrawColor(&s_u8g2, 0);
    for (int r = 0; r < play_h; r++) {
        int dy = abs(r - half_h);
        int w = (int)roundf((float)w_total * (1.0f - ((float)dy / (float)(half_h + 1))));
        if (w < 1) w = 1;
        int y = (play_cy - half_h) + r;
        u8g2_DrawHLine(&s_u8g2, play_x0, y, w);
    }

    // Passo B: Delimita a borda do triângulo com cor 1 (branco sólido)
    u8g2_SetDrawColor(&s_u8g2, 1);
    // Linha vertical base
    u8g2_DrawVLine(&s_u8g2, play_x0, play_cy - half_h, play_h);
    // Arestas diagonais superior e inferior
    for (int r = 0; r < play_h; r++) {
        int dy = abs(r - half_h);
        int w = (int)roundf((float)w_total * (1.0f - ((float)dy / (float)(half_h + 1))));
        int y = (play_cy - half_h) + r;
        u8g2_DrawPixel(&s_u8g2, play_x0 + w - 1, y);
    }
}

static void draw_animated_wifi_icon(int frame)
{
    int fr = s_menu_anim_enabled ? (frame % MENU_WIFI_FRAMES) : 0;
    u8g2_SetDrawColor(&s_u8g2, 1);
    if (menu_wifi_frames[fr]) {
        u8g2_DrawXBMP(&s_u8g2, 43, 8, 64, 64, menu_wifi_frames[fr]);
    } else {
        u8g2_DrawXBMP(&s_u8g2, 43, 17, 64, 46, menu_wifi_big_bits);
    }
}

static void draw_animated_bt_icon(int frame)
{
    u8g2_SetDrawColor(&s_u8g2, 1);

    // 1. Runa Bluetooth oficial 32x32 perfeitamente proporcionada
    static const uint8_t s_bt_rune_bits[] = {
        0x00, 0x1c, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x00,
        0x00, 0x7c, 0x00, 0x00, 0x00, 0xfc, 0x01, 0x00,
        0x00, 0xfc, 0x03, 0x00, 0x00, 0xfc, 0x0f, 0x00,
        0x00, 0xbc, 0x1f, 0x00, 0x00, 0x3c, 0x3f, 0x00,
        0xc0, 0x3c, 0xfc, 0x00, 0xe0, 0x3d, 0xf8, 0x01,
        0xe0, 0x3f, 0xf8, 0x01, 0xc0, 0x3f, 0xfc, 0x00,
        0x80, 0x3f, 0x3f, 0x00, 0x00, 0xbf, 0x1f, 0x00,
        0x00, 0xfe, 0x07, 0x00, 0x00, 0xfc, 0x03, 0x00,
        0x00, 0xfc, 0x03, 0x00, 0x00, 0xfe, 0x07, 0x00,
        0x00, 0xbf, 0x1f, 0x00, 0x80, 0x3f, 0x3f, 0x00,
        0xc0, 0x3f, 0xfc, 0x00, 0xe0, 0x3f, 0xf8, 0x01,
        0xe0, 0x3d, 0xf8, 0x01, 0xc0, 0x3c, 0xfc, 0x00,
        0x00, 0x3c, 0x3f, 0x00, 0x00, 0xbc, 0x1f, 0x00,
        0x00, 0xfc, 0x0f, 0x00, 0x00, 0xfc, 0x03, 0x00,
        0x00, 0xfc, 0x01, 0x00, 0x00, 0x7c, 0x00, 0x00,
        0x00, 0x3c, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x00,
    };
    u8g2_DrawXBMP(&s_u8g2, 60, 20, 32, 32, s_bt_rune_bits);

    // 2. 4 Ondas de rádio contínuas e perfeitamente simétricas irradiando de cada lado
    int cx = 74;
    int cy = 36;
    int wave_phase = s_menu_anim_enabled ? ((frame / 3) % 5) : 3;
    static const int s_bt_radii[4] = { 14, 19, 24, 29 };

    for (int w = 0; w < 4; w++) {
        if (w <= wave_phase) {
            int r = s_bt_radii[w];
            // Abertura angular de ~68° (dy máximo = r * sin(34°))
            int max_dy = (int)(r * 0.559f + 0.5f);
            int r2 = r * r;
            for (int dy = -max_dy; dy <= max_dy; dy++) {
                int dx = (int)(sqrtf((float)(r2 - dy * dy)) + 0.5f);
                int ry = cy + dy;
                if (ry >= 12 && ry <= 60) {
                    int rx = cx + dx;
                    int lx = cx - dx;
                    if (rx >= 43 && rx <= 110) {
                        u8g2_DrawPixel(&s_u8g2, rx, ry);
                    }
                    if (lx >= 43 && lx <= 110) {
                        u8g2_DrawPixel(&s_u8g2, lx, ry);
                    }
                }
            }
        }
    }
}

static void draw_animated_conf_icon(int frame)
{
    int fr = s_menu_anim_enabled ? (frame % MENU_CONF_FRAMES) : 0;
    u8g2_SetDrawColor(&s_u8g2, 1);
    if (menu_conf_frames[fr]) {
        u8g2_DrawXBMP(&s_u8g2, 50, 12, MENU_CONF_W, MENU_CONF_H, menu_conf_frames[fr]);
    }
}

// Árvore Clássica USB extraída em fidelidade matemática 100% da arte em pixels do usuário
// Dimensões: 24x49 px, X=63, Y=11
// O interior do quadrado em (83, 21) foi desmarcado no bitmap para rotação vetorial fluida
static const uint8_t s_usb_tree_chassis_xbm[] = {
    0x00, 0x08, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x3e, 0x00, 0x00, 0x7f, 0x00,
    0x80, 0xff, 0x00, 0x00, 0x08, 0x00, 0x00, 0x08, 0x00, 0x00, 0x08, 0x00,
    0x00, 0x08, 0x00, 0x00, 0x08, 0x00, 0x00, 0x08, 0x00, 0x00, 0x08, 0x00,
    0x00, 0x08, 0x00, 0x1c, 0x08, 0x00, 0x3e, 0x08, 0x10, 0x7f, 0x08, 0x10,
    0x7f, 0x08, 0x10, 0x7f, 0x0c, 0x10, 0x3e, 0x0c, 0x10, 0x1c, 0x0c, 0x10,
    0x08, 0x0c, 0x18, 0x08, 0x0c, 0x18, 0x08, 0x0c, 0x0c, 0x08, 0x0c, 0x06,
    0x08, 0x0c, 0x03, 0x08, 0x8c, 0x01, 0x18, 0xcc, 0x00, 0x18, 0x6c, 0x00,
    0x30, 0x3c, 0x00, 0x60, 0x1c, 0x00, 0xc0, 0x0c, 0x00, 0x80, 0x0d, 0x00,
    0x00, 0x0f, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x0c, 0x00,
    0x00, 0x0c, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x0c, 0x00,
    0x00, 0x0c, 0x00, 0x00, 0x7f, 0x00, 0x00, 0x7f, 0x00, 0x80, 0xff, 0x00,
    0x80, 0xff, 0x00, 0x80, 0xff, 0x00, 0x00, 0x7f, 0x00, 0x00, 0x7f, 0x00,
    0x00, 0x1c, 0x00,
};

static const int8_t s_usb_sq_corners[16][4][2] = {
    { {80, 18}, {86, 18}, {86, 24}, {80, 24} }, // Frame 0
    { {81, 17}, {87, 19}, {85, 25}, {79, 23} }, // Frame 1
    { {83, 17}, {87, 21}, {83, 25}, {79, 21} }, // Frame 2
    { {85, 17}, {87, 23}, {81, 25}, {79, 19} }, // Frame 3
    { {86, 18}, {86, 24}, {80, 24}, {80, 18} }, // Frame 4
    { {87, 19}, {85, 25}, {79, 23}, {81, 17} }, // Frame 5
    { {87, 21}, {83, 25}, {79, 21}, {83, 17} }, // Frame 6
    { {87, 23}, {81, 25}, {79, 19}, {85, 17} }, // Frame 7
    { {86, 24}, {80, 24}, {80, 18}, {86, 18} }, // Frame 8
    { {85, 25}, {79, 23}, {81, 17}, {87, 19} }, // Frame 9
    { {83, 25}, {79, 21}, {83, 17}, {87, 21} }, // Frame 10
    { {81, 25}, {79, 19}, {85, 17}, {87, 23} }, // Frame 11
    { {80, 24}, {80, 18}, {86, 18}, {86, 24} }, // Frame 12
    { {79, 23}, {81, 17}, {87, 19}, {85, 25} }, // Frame 13
    { {79, 21}, {83, 17}, {87, 21}, {83, 25} }, // Frame 14
    { {79, 19}, {85, 17}, {87, 23}, {81, 25} }, // Frame 15
};

static const int8_t s_usb_orbit_pts[16][2][2] = {
    { {71, 27}, {61, 27} }, // Frame 0
    { {71, 25}, {61, 29} }, // Frame 1
    { {70, 23}, {62, 31} }, // Frame 2
    { {68, 22}, {64, 32} }, // Frame 3
    { {66, 22}, {66, 32} }, // Frame 4
    { {64, 22}, {68, 32} }, // Frame 5
    { {62, 23}, {70, 31} }, // Frame 6
    { {61, 25}, {71, 29} }, // Frame 7
    { {61, 27}, {71, 27} }, // Frame 8
    { {61, 29}, {71, 25} }, // Frame 9
    { {62, 31}, {70, 23} }, // Frame 10
    { {64, 32}, {68, 22} }, // Frame 11
    { {66, 32}, {66, 22} }, // Frame 12
    { {68, 32}, {64, 22} }, // Frame 13
    { {70, 31}, {62, 23} }, // Frame 14
    { {71, 29}, {61, 25} }, // Frame 15
};

static void draw_animated_usb_icon(int frame)
{
    u8g2_SetDrawColor(&s_u8g2, 1);

    // 1. Chassi Clássico da Árvore USB (Base exata do usuário)
    u8g2_DrawXBMP(&s_u8g2, 63, 11, 24, 49, s_usb_tree_chassis_xbm);

    if (!s_menu_anim_enabled) {
        // Modo estático: desenha o quadrado sólido original
        u8g2_DrawBox(&s_u8g2, 80, 18, 7, 7);
        return;
    }

    int f = frame % 16;

    // 2. Cinética do Quadrado Direito: rotação 360° em torno do eixo
    const int8_t (*c)[2] = s_usb_sq_corners[f];
    for (int i = 0; i < 4; i++) {
        int next = (i + 1) % 4;
        u8g2_DrawLine(&s_u8g2, c[i][0], c[i][1], c[next][0], c[next][1]);
    }
    u8g2_DrawPixel(&s_u8g2, 83, 21); // Pivô central

    // 3. Cinética do Círculo Esquerdo: satélites em órbita contínua
    u8g2_DrawPixel(&s_u8g2, s_usb_orbit_pts[f][0][0], s_usb_orbit_pts[f][0][1]);
    u8g2_DrawPixel(&s_u8g2, s_usb_orbit_pts[f][1][0], s_usb_orbit_pts[f][1][1]);

    // 4. Fluxo de Pacotes / Cometas nas Trilhas
    if (f <= 1) {
        // Flash na base circular
        u8g2_SetDrawColor(&s_u8g2, 0);
        u8g2_DrawPixel(&s_u8g2, 74, 55);
        u8g2_SetDrawColor(&s_u8g2, 1);
        u8g2_DrawPixel(&s_u8g2, 73, 55);
        u8g2_DrawPixel(&s_u8g2, 75, 55);
    } else if (f >= 2 && f <= 4) {
        // Pulso subindo pelo tronco central
        int ty = 51 - (f - 2) * 5;
        u8g2_SetDrawColor(&s_u8g2, 0);
        u8g2_DrawPixel(&s_u8g2, 74, ty);
        u8g2_DrawPixel(&s_u8g2, 75, ty);
        u8g2_SetDrawColor(&s_u8g2, 1);
        u8g2_DrawPixel(&s_u8g2, 73, ty);
        u8g2_DrawPixel(&s_u8g2, 76, ty);
    } else if (f >= 5 && f <= 7) {
        // Três vias simultâneas
        int cy = 35 - (f - 5) * 8;
        if (cy >= 16 && cy <= 35) {
            u8g2_SetDrawColor(&s_u8g2, 0);
            u8g2_DrawPixel(&s_u8g2, 74, cy);
            u8g2_SetDrawColor(&s_u8g2, 1);
            u8g2_DrawPixel(&s_u8g2, 73, cy);
            u8g2_DrawPixel(&s_u8g2, 75, cy);
        }
        if (f == 5) {
            u8g2_SetDrawColor(&s_u8g2, 0); u8g2_DrawPixel(&s_u8g2, 71, 39);
            u8g2_SetDrawColor(&s_u8g2, 1); u8g2_DrawPixel(&s_u8g2, 70, 39);
            u8g2_SetDrawColor(&s_u8g2, 0); u8g2_DrawPixel(&s_u8g2, 77, 37);
            u8g2_SetDrawColor(&s_u8g2, 1); u8g2_DrawPixel(&s_u8g2, 78, 37);
        } else if (f == 6) {
            u8g2_SetDrawColor(&s_u8g2, 0); u8g2_DrawPixel(&s_u8g2, 67, 34);
            u8g2_SetDrawColor(&s_u8g2, 1); u8g2_DrawPixel(&s_u8g2, 66, 34);
            u8g2_SetDrawColor(&s_u8g2, 0); u8g2_DrawPixel(&s_u8g2, 81, 33);
            u8g2_SetDrawColor(&s_u8g2, 1); u8g2_DrawPixel(&s_u8g2, 82, 33);
        } else if (f == 7) {
            u8g2_SetDrawColor(&s_u8g2, 0); u8g2_DrawPixel(&s_u8g2, 66, 29);
            u8g2_SetDrawColor(&s_u8g2, 1); u8g2_DrawPixel(&s_u8g2, 65, 29); u8g2_DrawPixel(&s_u8g2, 67, 29);
            u8g2_SetDrawColor(&s_u8g2, 0); u8g2_DrawPixel(&s_u8g2, 84, 26);
            u8g2_SetDrawColor(&s_u8g2, 1); u8g2_DrawPixel(&s_u8g2, 83, 26); u8g2_DrawPixel(&s_u8g2, 85, 26);
        }
    }

    // 5. Ondas de Choque de Ressonância (Impacto nos Terminais)
    u8g2_SetDrawColor(&s_u8g2, 1);
    if (f == 8) {
        u8g2_DrawCircle(&s_u8g2, 66, 27, 4, U8G2_DRAW_ALL);
        u8g2_DrawFrame(&s_u8g2, 83 - 4, 21 - 4, 9, 9);
        u8g2_DrawPixel(&s_u8g2, 74, 10);
    } else if (f == 9) {
        u8g2_DrawCircle(&s_u8g2, 66, 27, 5, U8G2_DRAW_ALL);
        u8g2_DrawFrame(&s_u8g2, 83 - 5, 21 - 5, 11, 11);
        u8g2_DrawPixel(&s_u8g2, 74, 9);
        u8g2_DrawPixel(&s_u8g2, 73, 9);
        u8g2_DrawPixel(&s_u8g2, 75, 9);
    } else if (f == 10) {
        u8g2_DrawPixel(&s_u8g2, 74, 9);
    }

    u8g2_SetDrawColor(&s_u8g2, 1);
}

static void draw_animated_games_icon(int frame)
{
    u8g2_SetDrawColor(&s_u8g2, 1);

    // Moldura estilo Console / Arcade Split-Screen (59x48 px, centralizada em X=50..108, Y=12..59)
    u8g2_DrawFrame(&s_u8g2, 50, 12, 59, 48);
    // Cantos chanfrados do monitor
    u8g2_SetDrawColor(&s_u8g2, 0);
    u8g2_DrawPixel(&s_u8g2, 50, 12);
    u8g2_DrawPixel(&s_u8g2, 108, 12);
    u8g2_DrawPixel(&s_u8g2, 50, 59);
    u8g2_DrawPixel(&s_u8g2, 108, 59);
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_DrawPixel(&s_u8g2, 51, 13);
    u8g2_DrawPixel(&s_u8g2, 107, 13);
    u8g2_DrawPixel(&s_u8g2, 51, 58);
    u8g2_DrawPixel(&s_u8g2, 107, 58);

    // Linha de cabeçalho do monitor
    u8g2_DrawHLine(&s_u8g2, 53, 15, 53);

    // Divisória pontilhada central separando Tetris (esquerda) e Game of Life (direita)
    for (int y = 16; y <= 57; y++) {
        if (y % 3 != 0) {
            u8g2_DrawPixel(&s_u8g2, 79, y);
        }
    }

    // =========================================================
    // 1. TETRIS (Lado Esquerdo: X = 53..78)
    // =========================================================
    // Pilha de blocos estáticos na base com visual vazado clássico (células 4x4)
    // Linha inferior (Y = 54..57)
    static const uint8_t row1_cols[] = { 0, 1, 3, 4, 5 };
    for (int i = 0; i < 5; i++) {
        int bx = 53 + row1_cols[i] * 4;
        u8g2_DrawBox(&s_u8g2, bx, 54, 4, 4);
        u8g2_SetDrawColor(&s_u8g2, 0);
        u8g2_DrawPixel(&s_u8g2, bx + 1, 55);
        u8g2_SetDrawColor(&s_u8g2, 1);
    }
    // Segunda linha (Y = 50..53)
    static const uint8_t row2_cols[] = { 0, 1, 4 };
    for (int i = 0; i < 3; i++) {
        int bx = 53 + row2_cols[i] * 4;
        u8g2_DrawBox(&s_u8g2, bx, 50, 4, 4);
        u8g2_SetDrawColor(&s_u8g2, 0);
        u8g2_DrawPixel(&s_u8g2, bx + 1, 51);
        u8g2_SetDrawColor(&s_u8g2, 1);
    }
    // Terceira linha (Y = 46..49)
    u8g2_DrawBox(&s_u8g2, 53, 46, 4, 4);
    u8g2_SetDrawColor(&s_u8g2, 0);
    u8g2_DrawPixel(&s_u8g2, 54, 47);
    u8g2_SetDrawColor(&s_u8g2, 1);

    // Peça em queda animada (alterna peça T e peça J)
    if (s_menu_anim_enabled) {
        int piece_type = (frame / 12) % 2;
        int drop = 17 + ((frame * 3) % 27);
        if (piece_type == 0) {
            // Peça T descendo na coluna central
            u8g2_DrawBox(&s_u8g2, 61, drop, 4, 4);
            u8g2_DrawBox(&s_u8g2, 65, drop, 4, 4);
            u8g2_DrawBox(&s_u8g2, 69, drop, 4, 4);
            u8g2_DrawBox(&s_u8g2, 65, drop + 4, 4, 4);
            u8g2_SetDrawColor(&s_u8g2, 0);
            u8g2_DrawPixel(&s_u8g2, 62, drop + 1);
            u8g2_DrawPixel(&s_u8g2, 66, drop + 1);
            u8g2_DrawPixel(&s_u8g2, 70, drop + 1);
            u8g2_DrawPixel(&s_u8g2, 66, drop + 5);
            u8g2_SetDrawColor(&s_u8g2, 1);
        } else {
            // Peça J descendo
            u8g2_DrawBox(&s_u8g2, 65, drop, 4, 4);
            u8g2_DrawBox(&s_u8g2, 65, drop + 4, 4, 4);
            u8g2_DrawBox(&s_u8g2, 65, drop + 8, 4, 4);
            u8g2_DrawBox(&s_u8g2, 61, drop + 8, 4, 4);
            u8g2_SetDrawColor(&s_u8g2, 0);
            u8g2_DrawPixel(&s_u8g2, 66, drop + 1);
            u8g2_DrawPixel(&s_u8g2, 66, drop + 5);
            u8g2_DrawPixel(&s_u8g2, 66, drop + 9);
            u8g2_DrawPixel(&s_u8g2, 62, drop + 9);
            u8g2_SetDrawColor(&s_u8g2, 1);
        }
    } else {
        // Modo estático: T estático
        u8g2_DrawBox(&s_u8g2, 61, 28, 4, 4);
        u8g2_DrawBox(&s_u8g2, 65, 28, 4, 4);
        u8g2_DrawBox(&s_u8g2, 69, 28, 4, 4);
        u8g2_DrawBox(&s_u8g2, 65, 32, 4, 4);
    }

    // =========================================================
    // 2. GAME OF LIFE (Lado Direito: X = 81..106)
    // =========================================================
    // Glider autônomo viajando diagonalmente pela tela
    static const int8_t s_glider_shapes[4][5][2] = {
        { {1,0}, {2,1}, {0,2}, {1,2}, {2,2} },
        { {0,1}, {2,1}, {1,2}, {2,2}, {1,3} },
        { {2,0}, {0,1}, {2,1}, {1,2}, {2,2} },
        { {1,0}, {2,1}, {0,2}, {1,2}, {2,2} }
    };
    int g_phase = s_menu_anim_enabled ? ((frame / 2) % 4) : 0;
    int g_step  = s_menu_anim_enabled ? ((frame / 2) % 6) : 2;
    int g_x0 = 82 + g_step * 2;
    int g_y0 = 19 + g_step * 3;

    for (int i = 0; i < 5; i++) {
        int cx = g_x0 + s_glider_shapes[g_phase][i][0] * 3;
        int cy = g_y0 + s_glider_shapes[g_phase][i][1] * 3;
        if (cx >= 82 && cx <= 104 && cy >= 17 && cy <= 45) {
            u8g2_DrawBox(&s_u8g2, cx, cy, 2, 2);
        }
    }

    // Bloco estável 2x2 (Still Life) no canto inferior direito
    u8g2_DrawBox(&s_u8g2, 101, 51, 2, 2);
    u8g2_DrawBox(&s_u8g2, 104, 51, 2, 2);
    u8g2_DrawBox(&s_u8g2, 101, 54, 2, 2);
    u8g2_DrawBox(&s_u8g2, 104, 54, 2, 2);

    // Oscilador Blinker no rodapé direito
    if ((frame / 2) % 2 == 0) {
        u8g2_DrawBox(&s_u8g2, 84, 53, 2, 2);
        u8g2_DrawBox(&s_u8g2, 87, 53, 2, 2);
        u8g2_DrawBox(&s_u8g2, 90, 53, 2, 2);
    } else {
        u8g2_DrawBox(&s_u8g2, 87, 50, 2, 2);
        u8g2_DrawBox(&s_u8g2, 87, 53, 2, 2);
        u8g2_DrawBox(&s_u8g2, 87, 56, 2, 2);
    }
}

static void draw_animated_chrono_icon(int frame)
{
    u8g2_SetDrawColor(&s_u8g2, 1);

    // Centro do mostrador circular vintage: CX = 75, CY = 42, Raio R = 17
    // Proporciona 8 pixels limpos de folga abaixo da topbar (Y=8) e 4 pixels na base (Y=63)
    int cx = 75;
    int cy = 42;
    int r = 17;

    // 1. Coroa superior vintage (haste e botao central)
    // Haste de fixação
    u8g2_DrawBox(&s_u8g2, cx - 2, cy - r - 2, 5, 3);
    // Botão / coroa
    u8g2_DrawBox(&s_u8g2, cx - 4, cy - r - 5, 9, 3);
    // Anel / alça de bolso no topo (topo em Y = 16, folga de 8 px da barra superior)
    u8g2_DrawCircle(&s_u8g2, cx, cy - r - 5, 4, U8G2_DRAW_UPPER_RIGHT | U8G2_DRAW_UPPER_LEFT);

    // 2. Botão lateral de Lap a 45 graus (canto superior direito)
    u8g2_DrawLine(&s_u8g2, cx + 12, cy - 12, cx + 15, cy - 15);
    u8g2_DrawLine(&s_u8g2, cx + 13, cy - 11, cx + 16, cy - 14);

    // 3. Mostrador circular duplo do cronometro
    u8g2_DrawCircle(&s_u8g2, cx, cy, r, U8G2_DRAW_ALL);
    u8g2_DrawCircle(&s_u8g2, cx, cy, r - 2, U8G2_DRAW_ALL);

    // 4. Marcadores de segundos nos quadrantes principais (12, 3, 6, 9)
    u8g2_DrawVLine(&s_u8g2, cx, cy - r + 3, 3); // 12h
    u8g2_DrawHLine(&s_u8g2, cx + r - 5, cy, 3); // 3h
    u8g2_DrawVLine(&s_u8g2, cx, cy + r - 5, 3); // 6h
    u8g2_DrawHLine(&s_u8g2, cx - r + 3, cy, 3); // 9h

    // 5. Sub-mostrador pequeno de minutos em CX=75, CY=cy-5 (raio 4)
    u8g2_DrawCircle(&s_u8g2, cx, cy - 5, 4, U8G2_DRAW_ALL);
    int mini_angle = (frame * 6) % 360;
    int mx = cx + fast_scale_trig(3, fast_sin_q10(mini_angle));
    int my = (cy - 5) - fast_scale_trig(3, fast_cos_q10(mini_angle));
    u8g2_DrawLine(&s_u8g2, cx, cy - 5, mx, my);

    // 6. Pivô central
    u8g2_DrawDisc(&s_u8g2, cx, cy, 2, U8G2_DRAW_ALL);

    // 7. Ponteiro principal rotativo animado (varre 360 graus suavemente)
    int angle_deg = s_menu_anim_enabled ? ((frame * 15) % 360) : 300;
    int sin_q10 = fast_sin_q10(angle_deg);
    int cos_q10 = fast_cos_q10(angle_deg);
    int hx = cx + fast_scale_trig(12, sin_q10);
    int hy = cy - fast_scale_trig(12, cos_q10);
    int tx = cx - fast_scale_trig(4, sin_q10);
    int ty = cy + fast_scale_trig(4, cos_q10);

    u8g2_DrawLine(&s_u8g2, tx, ty, hx, hy);
}

void oled_display_show_main_menu(int cursor, const playback_state_t *now_playing)
{
    if (!s_ready) return;

    if (cursor < 0) cursor = 0;
    if (cursor > 6) cursor = 6;

    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetFontMode(&s_u8g2, 1);
    u8g2_SetBitmapMode(&s_u8g2, 1);
    u8g2_SetDrawColor(&s_u8g2, 1);

    bool show_topbar = now_playing && (now_playing->track_loaded || now_playing->playing);
    if (show_topbar) {
        draw_main_menu_topbar(now_playing);
    }

    // 1. Ícones Animados Centrais (Área Segura: X in [42, 108], Y in [14, 60])
    switch (cursor) {
        case 0: // Play (Equalizador de Espectro Cyberpunk + Triângulo Play)
            draw_animated_play_icon(s_menu_anim_tick);
            break;
        case 1: // WiFi (Animação de Ondas de Rádio 64x64 em loop)
            draw_animated_wifi_icon(s_menu_anim_tick);
            break;
        case 2: // Bluetooth (Runa Central + Ondas de Rádio Irradiando)
            draw_animated_bt_icon(s_menu_anim_tick);
            break;
        case 3: // Conf (Engrenagens Mecânicas Rotativas Sincronizadas)
            draw_animated_conf_icon(s_menu_anim_tick);
            break;
        case 4: // USB (Tridente USB com Fluxo de Dados)
            draw_animated_usb_icon(s_menu_anim_tick);
            break;
        case 5: // Games (Console Arcade: Tetris + Conway's Game of Life)
            draw_animated_games_icon(s_menu_anim_tick);
            break;
        case 6: // Chrono (Cronômetro Clássico Vintage com Ponteiro Rotativo)
            draw_animated_chrono_icon(s_menu_anim_tick);
            break;
    }

    // 2. Carrossel circular à esquerda com Tag Chanfrada
    static const char *s_menu_labels[7] = {
        "Play", "Wifi", "BT", "Conf", "USB", "Games", "Chrono"
    };

    int prev_idx  = (cursor - 1 + 7) % 7;
    int cur_idx   = cursor;
    int next_idx  = (cursor + 1) % 7;
    int next2_idx = (cursor + 2) % 7;

    u8g2_SetFont(&s_u8g2, u8g2_font_helvB08_tr);

    // Item -1 (anterior): baseline Y = 21
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_DrawStr(&s_u8g2, 4, 21, s_menu_labels[prev_idx]);

    // Item 0 (selecionado): tag chanfrada Y = 26..37 (altura 12)
    const char *sel_text = s_menu_labels[cur_idx];
    int sel_w = u8g2_GetStrWidth(&s_u8g2, sel_text);
    int tag_w = sel_w + 8;
    if (tag_w < 28) tag_w = 28;

    draw_menu_chamfered_tag(0, 26, tag_w, 12);

    u8g2_SetFontMode(&s_u8g2, 1);
    u8g2_SetDrawColor(&s_u8g2, 2); // XOR
    u8g2_DrawStr(&s_u8g2, 4, 35, sel_text);
    u8g2_SetDrawColor(&s_u8g2, 1);

    // Item +1 (próximo): baseline Y = 49
    u8g2_DrawStr(&s_u8g2, 4, 49, s_menu_labels[next_idx]);

    // Item +2 (segundo próximo): baseline Y = 61
    u8g2_DrawStr(&s_u8g2, 3, 61, s_menu_labels[next2_idx]);

    // 3. Rosa dos Ventos no quadrante direito (122, 48)
    // Cima, Baixo, Direita, Centro sempre válidos; Esquerda abre Now Playing
    draw_nav_compass(122, 48, true, true, true, true, true);

    // 4. Badge de Bateria [XX] no canto inferior direito (117, 55)
    draw_battery_badge(117, 55, battery_get_percent());

    apply_brightness_if_needed();
    oled_flush_buffer();
}

// =========================================================================
// Telas de status WiFi
// =========================================================================

void oled_display_show_wifi_idle(const char *ip, const char *mdns_host, bool dns_active, int rssi)
{
    if (!s_ready) return;

    if (ip && ip[0] && strcmp(ip, "0.0.0.0") != 0) {
        oled_display_show_wifi_connected(ip, mdns_host, rssi, 0);
        return;
    }

    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_SetBitmapMode(&s_u8g2, 1);
    u8g2_SetFontMode(&s_u8g2, 1);

    // Frame superior com RSSI e Status
    u8g2_DrawFrame(&s_u8g2, 0, 0, 128, 12);
    u8g2_SetFont(&s_u8g2, u8g2_font_haxrcorp4089_tr);
    if (rssi != -1) {
        char db[16];
        snprintf(db, sizeof(db), "%d dBm", rssi);
        u8g2_DrawStr(&s_u8g2, 3, 9, db);
    } else {
        u8g2_DrawStr(&s_u8g2, 3, 9, "WiFi");
    }
    u8g2_DrawStr(&s_u8g2, 44, 9, "CONECTANDO");

    // Mensagem de busca centralizada
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    const char *msg = "Buscando rede...";
    int mw = u8g2_GetStrWidth(&s_u8g2, msg);
    u8g2_DrawStr(&s_u8g2, (128 - mw) / 2, 33, msg);

    // Subtexto inferior
    u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
    const char *sub = "Aguardando IP da rede...";
    int sw = u8g2_GetStrWidth(&s_u8g2, sub);
    u8g2_DrawStr(&s_u8g2, (128 - sw) / 2, 50, sub);

    apply_brightness_if_needed();
    oled_flush_buffer();
}

void oled_display_show_wifi_connected(const char *ip, const char *mdns_host, int rssi, int clients)
{
    if (!s_ready) return;

    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_SetBitmapMode(&s_u8g2, 1);
    u8g2_SetFontMode(&s_u8g2, 1);

    // Barra superior: Frame 0..12 com RSSI, Status e Clientes
    u8g2_DrawFrame(&s_u8g2, 0, 0, 128, 12);
    u8g2_SetFont(&s_u8g2, u8g2_font_haxrcorp4089_tr);
    if (rssi != -1) {
        char db[16];
        snprintf(db, sizeof(db), "%d dBm", rssi);
        u8g2_DrawStr(&s_u8g2, 3, 9, db);
    } else {
        u8g2_DrawStr(&s_u8g2, 3, 9, "WiFi");
    }

    u8g2_DrawStr(&s_u8g2, 44, 9, "CONECTADO");

    if (clients > 0) {
        char cli_str[16];
        snprintf(cli_str, sizeof(cli_str), "Cli:%d", clients);
        int cw = u8g2_GetStrWidth(&s_u8g2, cli_str);
        u8g2_DrawStr(&s_u8g2, 125 - cw, 9, cli_str);
    }

    // Caixa central destacada para o IP
    u8g2_DrawRFrame(&s_u8g2, 2, 15, 124, 19, 3);
    u8g2_SetFont(&s_u8g2, u8g2_font_profont15_tr);
    const char *ip_str = (ip && ip[0] && strcmp(ip, "0.0.0.0") != 0) ? ip : "192.168.4.1";
    int ip_w = u8g2_GetStrWidth(&s_u8g2, ip_str);
    u8g2_DrawStr(&s_u8g2, (128 - ip_w) / 2, 29, ip_str);

    // Linha inferior: URL mDNS (http://mps3.local)
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    const char *host = (mdns_host && mdns_host[0]) ? mdns_host : "mps3.local";
    char url_buf[32];
    snprintf(url_buf, sizeof(url_buf), "http://%s", host);
    int url_w = u8g2_GetStrWidth(&s_u8g2, url_buf);
    u8g2_DrawStr(&s_u8g2, (128 - url_w) / 2, 46, url_buf);

    // Instrucao na base da tela
    u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
    const char *hint = "Acesse pelo navegador";
    int hint_w = u8g2_GetStrWidth(&s_u8g2, hint);
    u8g2_DrawStr(&s_u8g2, (128 - hint_w) / 2, 59, hint);

    apply_brightness_if_needed();
    oled_flush_buffer();
}

void oled_display_show_wifi_transfer(const char *filename, int file_pct, int overall_pct,
                                     int idx, int count, float kbps, int eta_sec, int total_eta, int rssi)
{
    if (!s_ready) return;

    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_SetBitmapMode(&s_u8g2, 1);
    u8g2_SetFontMode(&s_u8g2, 1);

    // Frame superior (mockup: rect 9: (0,0,65,12))
    u8g2_DrawFrame(&s_u8g2, 0, 0, 65, 12);
    u8g2_SetFont(&s_u8g2, u8g2_font_haxrcorp4089_tr);

        // Exibe RSSI real se disponÃ­vel, senÃ£o "-40db" (fallback)
    if (rssi != -1) {
        char db_str[8];
        snprintf(db_str, sizeof(db_str), "%d dBm", rssi);
        u8g2_DrawStr(&s_u8g2, 2, 9, db_str);   // PosiÃ§Ã£o ajustada para caber
    } else {
        u8g2_DrawStr(&s_u8g2, 17, 9, "-0db");  // fallback
    }

    // --- Nome do arquivo (com rolagem se necessÃ¡rio) ---
    u8g2_SetFont(&s_u8g2, u8g2_font_profont15_tr);
    const int name_x = 1;
    const int name_y = 25;
    const int name_max_w = 100;   // Largura mÃ¡xima antes de colidir com ETA

    // Prepara string segura (sanitizaÃ§Ã£o)
    char safe_name[64];
    sanitize_for_display(filename && filename[0] ? filename : "---", safe_name, sizeof(safe_name));

    // Verifica largura do texto
    int text_w = u8g2_GetStrWidth(&s_u8g2, safe_name);
    if (text_w <= name_max_w) {
        // Cabe: desenha normalmente
        u8g2_DrawStr(&s_u8g2, name_x, name_y, safe_name);
    } else {
        // NÃ£o cabe: aplica rolagem com clipping
        u8g2_SetClipWindow(&s_u8g2, name_x, name_y - 10, name_x + name_max_w, name_y + 5);
        draw_marquee(&s_transfer_name_marquee, safe_name, name_x, name_y, 2);
        u8g2_SetMaxClipWindow(&s_u8g2);
    }


    // Velocidade (mockup: (66,11) -> "987Kbps/s")
    char speed_str[16];
    if (kbps >= 1024) snprintf(speed_str, sizeof(speed_str), "%.1fMB/s", kbps/1024);
    else snprintf(speed_str, sizeof(speed_str), "%.0fKB/s", kbps);
    u8g2_DrawStr(&s_u8g2, 66, 11, speed_str);

    // ETA do arquivo (mockup: (108,27) -> "67s")
    if (eta_sec >= 0) {
        char eta_str[16];
        snprintf(eta_str, sizeof(eta_str), "%ds", eta_sec);
        u8g2_DrawStr(&s_u8g2, 108, 27, eta_str);
    }

    // Barra de progresso do arquivo (mockup: rect 1 copy 2: (0,29,128,11))
    u8g2_DrawFrame(&s_u8g2, 0, 29, 128, 11);
    if (file_pct > 0) {
        int fill_w = (128 - 2) * file_pct / 100;
        u8g2_DrawBox(&s_u8g2, 1, 30, fill_w, 9);
    }
    // Porcentagem dentro da barra (mockup: (59,38) -> "1%" com draw color 2)
    u8g2_SetDrawColor(&s_u8g2, 2);
    char pct_str[8];
    snprintf(pct_str, sizeof(pct_str), "%d%%", file_pct);
    u8g2_DrawStr(&s_u8g2, 59, 39, pct_str);
    u8g2_SetDrawColor(&s_u8g2, 1);

    // Linha "Total:" e Ã­ndice (mockup: (0,51) "Total:", (47,51) "|1/9|")
    u8g2_DrawStr(&s_u8g2, 0, 51, "Total:");
    char idx_str[12];
    snprintf(idx_str, sizeof(idx_str), "|%d/%d|", idx, count);
    u8g2_DrawStr(&s_u8g2, 47, 52, idx_str);

    // ETA total (mockup: (94,51) -> "2m65s")
    if (total_eta >= 0) {
        char total_eta_str[16];
        if (total_eta >= 60) snprintf(total_eta_str, sizeof(total_eta_str), "%dm%ds", total_eta/60, total_eta%60);
        else snprintf(total_eta_str, sizeof(total_eta_str), "%ds", total_eta);
        u8g2_DrawStr(&s_u8g2, 94, 51, total_eta_str);
    }

    // Barra de progresso geral (mockup: rect 1 copy 1: (0,53,128,11))
    u8g2_DrawFrame(&s_u8g2, 0, 53, 128, 11);
    if (overall_pct > 0) {
        int fill_w = (128 - 2) * overall_pct / 100;
        u8g2_DrawBox(&s_u8g2, 1, 54, fill_w, 9);
    }
    // Porcentagem geral (mockup: (57,62) -> "60%")
    u8g2_SetDrawColor(&s_u8g2, 2);
    char overall_str[8];
    snprintf(overall_str, sizeof(overall_str), "%d%%", overall_pct);
    u8g2_DrawStr(&s_u8g2, 57, 62, overall_str);
    u8g2_SetDrawColor(&s_u8g2, 1);

    apply_brightness_if_needed();
    oled_flush_buffer();
}

void oled_display_show_wifi_done(int files_received)
{
    if (!s_ready) return;

    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_SetBitmapMode(&s_u8g2, 1);
    u8g2_SetFontMode(&s_u8g2, 1);

    // AnimaÃ§Ã£o download_from_cloud (posiÃ§Ã£o: (33,3))
    u8g2_DrawXBMP(&s_u8g2, 33, 3, 64, 64,
                  wifi_download_cloud_frames[s_wifi_download_frame]);

    // Texto "+N" (mockup: (57,33) com fonte profont17)
    u8g2_SetFont(&s_u8g2, u8g2_font_profont17_tr);
    char buf[8];
    snprintf(buf, sizeof(buf), "+%d", files_received);
    u8g2_DrawStr(&s_u8g2, 57, 33, buf);

    apply_brightness_if_needed();
    oled_flush_buffer();
}
// =========================================================================
// Tela do Equalizador (10 bandas + Geral)
// =========================================================================
void oled_display_show_eq(const float gains[11], int selected_band, bool eq_enabled)
{
    if (!s_ready) return;

    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);

    // Cabecalho superior
    u8g2_SetFont(&s_u8g2, u8g2_font_5x8_tf);
    
    // Mostra o status (ON / OFF) no canto superior esquerdo dentro de uma box
    u8g2_DrawStr(&s_u8g2, 2, 8, "EQ:");
    if (eq_enabled) {
        u8g2_DrawBox(&s_u8g2, 20, 0, 16, 9);
        u8g2_SetDrawColor(&s_u8g2, 0);
        u8g2_DrawStr(&s_u8g2, 22, 7, "ON");
        u8g2_SetDrawColor(&s_u8g2, 1);
    } else {
        u8g2_DrawFrame(&s_u8g2, 20, 0, 19, 9);
        u8g2_DrawStr(&s_u8g2, 22, 7, "OFF");
    }

    if (selected_band < 0) selected_band = 0;
    if (selected_band > 10) selected_band = 10;

    // Mostra a banda atual e ganho no canto superior direito
    const char* band_labels[11] = {
        "31Hz", "62Hz", "125Hz", "250Hz", "500Hz",
        "1kHz", "2kHz", "4kHz", "8kHz", "16kHz", "Global"
    };
    char info_buf[32];
    snprintf(info_buf, sizeof(info_buf), "%s: %+.1fdB", band_labels[selected_band], gains[selected_band]);
    int str_w = u8g2_GetStrWidth(&s_u8g2, info_buf);
    u8g2_DrawStr(&s_u8g2, 126 - str_w, 8, info_buf);

    // Linha separadora do cabecalho
    u8g2_DrawHLine(&s_u8g2, 0, 10, 128);

    // Desenho das 11 barras
    // Altura disponivel para a barra (area util): y=14 ate y=63 (50 pixels de altura).
    // O centro e' y=38 (0 dB).
    
    const int num_bars = 11;
    const int bar_w = 8;
    const int spacing = 3;
    const int start_x = 4;
    const int center_y = 38;
    const int max_h_half = 24; // Barra maxima para cima ou para baixo (altura)

    for (int i = 0; i < num_bars; i++) {
        int x = start_x + i * (bar_w + spacing);
        
        float db = gains[i];
        float max_db = (i == 10) ? 30.0f : 15.0f;
        
        // Converte dB para pixels
        float px = (db / max_db) * max_h_half;
        int pxh = (int)fabsf(px);
        if (pxh == 0) pxh = 1; // Sempre mostra um tracinho mesmo no 0
        
        if (px > 0) {
            // Ganho positivo (barra pra cima a partir do centro)
            u8g2_DrawBox(&s_u8g2, x, center_y - pxh, bar_w, pxh);
        } else {
            // Ganho negativo (barra pra baixo a partir do centro)
            u8g2_DrawBox(&s_u8g2, x, center_y, bar_w, pxh);
        }

        // Se for a selecionada, faz um contorno ao redor da area toda dela
        if (i == selected_band) {
            u8g2_DrawFrame(&s_u8g2, x - 1, 12, bar_w + 2, 52);
        }
    }

    apply_brightness_if_needed();
    oled_flush_buffer();
}


static const uint8_t image_Speaker_bits[] = {0x00,0x00,0x60,0x00,0x60,0x01,0x6e,0x03,0xef,0x06,0xef,0x04,0xef,0x0c,0xef,0x0c,0xef,0x0c,0xef,0x04,0xef,0x06,0x6e,0x03,0x60,0x01,0x60,0x00,0x00,0x00,0x00,0x00};
static const uint8_t image_Battery_bits[] = {0x00,0x00,0x00,0x00,0x80,0x01,0xc0,0x03,0x20,0x04,0x20,0x04,0xa0,0x05,0x20,0x04,0xa0,0x05,0x20,0x04,0xa0,0x05,0x20,0x04,0xc0,0x03,0x00,0x00,0x00,0x00,0x00,0x00};
static const uint8_t image_menu_settings_sliders_square_bits[] = {0x00,0x00,0x1e,0x00,0xf3,0x3f,0x1e,0x00,0x00,0x00,0x00,0x00,0x00,0x1e,0xff,0x33,0x00,0x1e,0x00,0x00,0x00,0x00,0x1e,0x00,0xf3,0x3f,0x1e,0x00,0x00,0x00,0x00,0x00};

// =========================================================================
// =========================================================================
// Telas de USB Sentinela (S4: Colmeia Hexagonal Simétrica - Geometria 45°)
// =========================================================================

static void draw_crisp_arrow(int x, int y, int dir)
{
    if (dir == 1) { // UP (largura 7, altura 4)
        u8g2_DrawHLine(&s_u8g2, x + 3, y, 1);
        u8g2_DrawHLine(&s_u8g2, x + 2, y + 1, 3);
        u8g2_DrawHLine(&s_u8g2, x + 1, y + 2, 5);
        u8g2_DrawHLine(&s_u8g2, x,     y + 3, 7);
    } else if (dir == 4) { // DOWN (largura 7, altura 4)
        u8g2_DrawHLine(&s_u8g2, x,     y, 7);
        u8g2_DrawHLine(&s_u8g2, x + 1, y + 1, 5);
        u8g2_DrawHLine(&s_u8g2, x + 2, y + 2, 3);
        u8g2_DrawHLine(&s_u8g2, x + 3, y + 3, 1);
    } else if (dir == 3) { // LEFT (largura 4, altura 7)
        u8g2_DrawPixel(&s_u8g2, x + 3, y);
        u8g2_DrawHLine(&s_u8g2, x + 2, y + 1, 2);
        u8g2_DrawHLine(&s_u8g2, x + 1, y + 2, 3);
        u8g2_DrawHLine(&s_u8g2, x,     y + 3, 4);
        u8g2_DrawHLine(&s_u8g2, x + 1, y + 4, 3);
        u8g2_DrawHLine(&s_u8g2, x + 2, y + 5, 2);
        u8g2_DrawPixel(&s_u8g2, x + 3, y + 6);
    } else if (dir == 2) { // RIGHT (largura 4, altura 7)
        u8g2_DrawPixel(&s_u8g2, x,     y);
        u8g2_DrawHLine(&s_u8g2, x,     y + 1, 2);
        u8g2_DrawHLine(&s_u8g2, x,     y + 2, 3);
        u8g2_DrawHLine(&s_u8g2, x,     y + 3, 4);
        u8g2_DrawHLine(&s_u8g2, x,     y + 4, 3);
        u8g2_DrawHLine(&s_u8g2, x,     y + 5, 2);
        u8g2_DrawPixel(&s_u8g2, x,     y + 6);
    }
}

static void draw_colmeia_capsule(int x, int y, int w, int h, const char *title, int dir_arrow, bool is_sel)
{
    const int c = 4; // Chanfro exato de 45 graus (deltaX = deltaY = 4)

    if (is_sel) {
        // Preenchimento sólido chanfrado 100% simétrico
        u8g2_SetDrawColor(&s_u8g2, 1);
        u8g2_DrawBox(&s_u8g2, x, y + c, w, h - 2 * c);
        for (int i = 0; i < c; i++) {
            u8g2_DrawHLine(&s_u8g2, x + c - i, y + i, w - 2 * (c - i));
            u8g2_DrawHLine(&s_u8g2, x + c - i, y + h - 1 - i, w - 2 * (c - i));
        }
        // Conteúdo negativo em preto sobre fundo aceso
        u8g2_SetDrawColor(&s_u8g2, 0);
    } else {
        // Contorno chanfrado 45°
        u8g2_SetDrawColor(&s_u8g2, 1);
        u8g2_DrawHLine(&s_u8g2, x + c, y, w - 2 * c);
        u8g2_DrawHLine(&s_u8g2, x + c, y + h - 1, w - 2 * c);
        u8g2_DrawVLine(&s_u8g2, x, y + c, h - 2 * c);
        u8g2_DrawVLine(&s_u8g2, x + w - 1, y + c, h - 2 * c);
        u8g2_DrawLine(&s_u8g2, x, y + c, x + c, y);
        u8g2_DrawLine(&s_u8g2, x + w - c - 1, y, x + w - 1, y + c);
        u8g2_DrawLine(&s_u8g2, x, y + h - c - 1, x + c, y + h - 1);
        u8g2_DrawLine(&s_u8g2, x + w - c - 1, y + h - 1, x + w - 1, y + h - c - 1);
    }

    u8g2_SetFont(&s_u8g2, u8g2_font_helvB08_tr);
    int tw = u8g2_GetStrWidth(&s_u8g2, title);
    const int arr_w = (dir_arrow == 1 || dir_arrow == 4) ? 7 : 4;
    const int gap = 3;
    int total_w = tw + arr_w + gap;
    int start_x = x + (w - total_w) / 2;
    int ty = y + 11;

    if (dir_arrow == 1 || dir_arrow == 4) {
        // CIMA (1) ou BAIXO (4): Seta 7x4 perfeitamente alinhada
        draw_crisp_arrow(start_x, y + 5, dir_arrow);
        u8g2_DrawStr(&s_u8g2, start_x + arr_w + gap, ty, title);
    } else if (dir_arrow == 3) {
        // ESQUERDA (3): Seta 4x7 alinhada à altura exata das maiúsculas
        draw_crisp_arrow(start_x, y + 4, 3);
        u8g2_DrawStr(&s_u8g2, start_x + arr_w + gap, ty, title);
    } else if (dir_arrow == 2) {
        // DIREITA (2): Seta 4x7 alinhada à altura exata das maiúsculas
        u8g2_DrawStr(&s_u8g2, start_x, ty, title);
        draw_crisp_arrow(start_x + tw + gap, y + 4, 2);
    } else {
        u8g2_DrawStr(&s_u8g2, x + (w - tw) / 2, ty, title);
    }
}

static void draw_hex_center(int cx, int cy, bool is_sel)
{
    // Octógono central perfeitamente simétrico de 20 × 20 px:
    // X em [54..73], Y em [22..41], chanfros exatos 45° de deltaX = deltaY = 7
    if (is_sel) {
        u8g2_SetDrawColor(&s_u8g2, 1);
        for (int dy = 0; dy < 7; dy++) {
            u8g2_DrawHLine(&s_u8g2, 61 - dy, 22 + dy, 6 + 2 * dy);
            u8g2_DrawHLine(&s_u8g2, 61 - dy, 41 - dy, 6 + 2 * dy);
        }
        u8g2_DrawBox(&s_u8g2, 54, 29, 20, 6);
        u8g2_SetDrawColor(&s_u8g2, 0);
    } else {
        u8g2_SetDrawColor(&s_u8g2, 1);
        // 4 Lados planos de 6 px:
        u8g2_DrawHLine(&s_u8g2, 61, 22, 6);
        u8g2_DrawHLine(&s_u8g2, 61, 41, 6);
        u8g2_DrawVLine(&s_u8g2, 54, 29, 6);
        u8g2_DrawVLine(&s_u8g2, 73, 29, 6);
        // 4 Chanfros diagonais 45°:
        u8g2_DrawLine(&s_u8g2, 54, 29, 61, 22);
        u8g2_DrawLine(&s_u8g2, 66, 22, 73, 29);
        u8g2_DrawLine(&s_u8g2, 54, 34, 61, 41);
        u8g2_DrawLine(&s_u8g2, 66, 41, 73, 34);
    }

    u8g2_SetFont(&s_u8g2, u8g2_font_5x8_tf);
    const char *lbl = "USB";
    int lw = u8g2_GetStrWidth(&s_u8g2, lbl);
    u8g2_DrawStr(&s_u8g2, cx - lw / 2, cy + 3, lbl);
}

static void draw_bus_traces(int sel_dir)
{
    u8g2_SetDrawColor(&s_u8g2, 1);

    // CIMA: Conecta Cápsula Topo (Y=16) ao Octógono (Y=22), X=62 e X=65
    if (sel_dir == 1) {
        u8g2_DrawBox(&s_u8g2, 61, 16, 6, 7);
    } else {
        u8g2_DrawVLine(&s_u8g2, 62, 16, 7);
        u8g2_DrawVLine(&s_u8g2, 65, 16, 7);
    }

    // BAIXO: Conecta Octógono (Y=41) à Cápsula Baixo (Y=47), X=62 e X=65
    if (sel_dir == 4) {
        u8g2_DrawBox(&s_u8g2, 61, 41, 6, 7);
    } else {
        u8g2_DrawVLine(&s_u8g2, 62, 41, 7);
        u8g2_DrawVLine(&s_u8g2, 65, 41, 7);
    }

    // ESQUERDA: Conecta Cápsula Esquerda (X=47) ao Octógono (X=54), Y=30 e Y=33
    if (sel_dir == 3) {
        u8g2_DrawBox(&s_u8g2, 47, 29, 8, 6);
    } else {
        u8g2_DrawHLine(&s_u8g2, 47, 30, 8);
        u8g2_DrawHLine(&s_u8g2, 47, 33, 8);
    }

    // DIREITA: Conecta Octógono (X=73) à Cápsula Direita (X=80), Y=30 e Y=33
    if (sel_dir == 2) {
        u8g2_DrawBox(&s_u8g2, 73, 29, 8, 6);
    } else {
        u8g2_DrawHLine(&s_u8g2, 73, 30, 8);
        u8g2_DrawHLine(&s_u8g2, 73, 33, 8);
    }
}

void oled_display_show_usb_prompt(int cursor)
{
    if (!s_ready) return;
    u8g2_ClearBuffer(&s_u8g2);

    const int bw = 46;
    const int bh = 15;
    const int cx = 64;
    const int cy = 32;

    // 1. Barramentos condutores de dados
    draw_bus_traces(cursor);

    // 2. Nó central octogonal [ USB ] (cursor == 0 destaca o centro)
    draw_hex_center(cx, cy, cursor == 0);

    // 3. Quatro Cápsulas Colmeia rigorosamente idênticas (46 × 15 px):
    // Cima (1): DAC
    draw_colmeia_capsule(cx - bw / 2, 2, bw, bh, "DAC", 1, cursor == 1);

    // Baixo (4): SAIR
    draw_colmeia_capsule(cx - bw / 2, 47, bw, bh, "SAIR", 4, cursor == 4);

    // Esquerda (3): COM
    draw_colmeia_capsule(2, cy - bh / 2, bw, bh, "COM", 3, cursor == 3);

    // Direita (2): MSC
    draw_colmeia_capsule(128 - bw - 2, cy - bh / 2, bw, bh, "MSC", 2, cursor == 2);

    apply_brightness_if_needed();
    oled_flush_buffer();
}

void oled_display_show_usb_msc(void)
{
    if (!s_ready) return;
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_SetFont(&s_u8g2, u8g2_font_5x8_tf);
    u8g2_DrawStr(&s_u8g2, 10, 25, "Modo Pendrive Ativo");
    u8g2_SetFont(&s_u8g2, u8g2_font_tom_thumb_4x6_t_all);
    u8g2_DrawStr(&s_u8g2, 10, 45, "Segure ESQUERDA para sair");
    apply_brightness_if_needed();
    oled_flush_buffer();
}

static const uint8_t image_SDQuestion_copy_1_bits[] = {
0xe0,0xff,0xff,0xff,0x00,0xf0,0xff,0xff,0xff,0x01,0xf0,0xff,0x3c,0xff,0x01,0x70,0x92,0x24,0xc9,0x01,
0x70,0x92,0x24,0xc9,0x01,0x70,0x92,0x24,0xc9,0x01,0x70,0x92,0x24,0xc9,0x01,0x70,0x92,0x24,0xc9,0x01,
0xf0,0xff,0xff,0xff,0x01,0xf0,0xff,0xff,0xff,0x01,0xf0,0xff,0xff,0xff,0x01,0xf0,0xff,0xff,0xff,0x01,
0xf0,0x7f,0xc0,0xff,0x01,0xf0,0x3f,0x80,0xff,0x01,0xf0,0x1f,0x00,0xff,0x01,0xf0,0x0f,0x1f,0xfe,0x01,
0xe8,0x87,0x3f,0xfc,0x01,0xf4,0xc7,0x7f,0xfc,0x01,0xfa,0xc7,0x7f,0xfc,0x01,0xfe,0xc7,0x7f,0xfc,0x01,
0xfe,0xc7,0x7f,0xfc,0x01,0xfe,0xef,0x3f,0xfc,0x01,0xfe,0xff,0x1f,0xfe,0x01,0xfe,0xff,0x0f,0xff,0x01,
0xf8,0xff,0x87,0xff,0x01,0xf8,0xff,0xc3,0xff,0x01,0xf8,0xff,0xe3,0xff,0x01,0xf8,0xff,0xf1,0xff,0x01,
0xe8,0xff,0xf1,0xff,0x01,0xf4,0xff,0xf1,0xff,0x01,0xfa,0xff,0xf1,0xff,0x01,0xfe,0xff,0xfb,0xff,0x01,
0xfe,0xff,0xff,0xff,0x01,0xfe,0xff,0xff,0xff,0x01,0xfe,0xff,0xf1,0xff,0x01,0xfe,0xff,0xf1,0xff,0x01,
0xfe,0xff,0xf1,0xff,0x01,0xfe,0xff,0xff,0xff,0x01,0xfe,0xff,0xff,0xff,0x01,0xfe,0xff,0xff,0xff,0x01,
0xfe,0xff,0xff,0xff,0x01,0xfe,0xff,0xff,0xff,0x01,0xfc,0xff,0xff,0xff,0x00};

static const uint8_t image_hourglass0_bits[] = {0x00,0x00,0x00,0xe0,0xff,0x0f,0x20,0x00,0x08,0xc0,0xff,0x07,0x80,0x00,0x02,0x80,0xfe,0x02,0x80,0xfe,0x02,0x80,0x7c,0x02,0x00,0x39,0x01,0x00,0x92,0x00,0x00,0x44,0x00,0x00,0x28,0x00,0x00,0x28,0x00,0x00,0x44,0x00,0x00,0x92,0x00,0x00,0x01,0x01,0x80,0x00,0x02,0x80,0x00,0x02,0x80,0x00,0x02,0x80,0x00,0x02,0xc0,0xff,0x07,0x20,0x00,0x08,0xe0,0xff,0x0f,0x00,0x00,0x00};
static const uint8_t image_hourglass1_bits[] = {0x00,0x00,0x00,0xe0,0xff,0x0f,0x20,0x00,0x08,0xc0,0xff,0x07,0x80,0x00,0x02,0x80,0x00,0x02,0x80,0xfe,0x02,0x80,0x7c,0x02,0x00,0x39,0x01,0x00,0x92,0x00,0x00,0x44,0x00,0x00,0x28,0x00,0x00,0x28,0x00,0x00,0x44,0x00,0x00,0x92,0x00,0x00,0x01,0x01,0x80,0x10,0x02,0x80,0x7c,0x02,0x80,0xfe,0x02,0x80,0x00,0x02,0xc0,0xff,0x07,0x20,0x00,0x08,0xe0,0xff,0x0f,0x00,0x00,0x00};
static const uint8_t image_hourglass2_bits[] = {0x00,0x00,0x00,0xe0,0xff,0x0f,0x20,0x00,0x08,0xc0,0xff,0x07,0x80,0x00,0x02,0x80,0x00,0x02,0x80,0x00,0x02,0x80,0x7c,0x02,0x00,0x39,0x01,0x00,0x92,0x00,0x00,0x44,0x00,0x00,0x28,0x00,0x00,0x28,0x00,0x00,0x44,0x00,0x00,0x92,0x00,0x00,0x01,0x01,0x80,0x7c,0x02,0x80,0xfe,0x02,0x80,0xfe,0x02,0x80,0x00,0x02,0xc0,0xff,0x07,0x20,0x00,0x08,0xe0,0xff,0x0f,0x00,0x00,0x00};
static const uint8_t image_hourglass3_bits[] = {0x00,0x00,0x00,0xe0,0xff,0x0f,0x20,0x00,0x08,0xc0,0xff,0x07,0x80,0x00,0x02,0x80,0x00,0x02,0x80,0x00,0x02,0x80,0x00,0x02,0x00,0x01,0x01,0x00,0x82,0x00,0x00,0x44,0x00,0x00,0x28,0x00,0x00,0x28,0x00,0x00,0x44,0x00,0x00,0x92,0x00,0x00,0x39,0x01,0x80,0x7c,0x02,0x80,0xfe,0x02,0x80,0xfe,0x02,0x80,0x00,0x02,0xc0,0xff,0x07,0x20,0x00,0x08,0xe0,0xff,0x0f,0x00,0x00,0x00};
static const uint8_t image_hourglass4_bits[] = {0x00,0x02,0x00,0x00,0x07,0x00,0x80,0x02,0x00,0x40,0x05,0x00,0xa0,0x08,0x00,0x50,0x10,0x00,0x28,0x10,0x00,0x14,0x10,0x00,0x0a,0x10,0x00,0x07,0x10,0x00,0x0a,0x10,0x00,0x10,0xe0,0x07,0xe0,0x07,0x08,0x00,0x28,0x50,0x00,0xe8,0xe7,0x00,0xe8,0x53,0x00,0xe8,0x29,0x00,0xe8,0x14,0x00,0x48,0x0a,0x00,0x10,0x05,0x00,0xa0,0x02,0x00,0x40,0x01,0x00,0xe0,0x00,0x00,0x40,0x00};
static const uint8_t image_hourglass5_bits[] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x06,0x00,0x60,0x0a,0x00,0x50,0xfa,0x00,0x5f,0x0a,0x81,0x50,0x0a,0x42,0x50,0x0a,0x24,0x50,0x0a,0x18,0x5e,0x0a,0xc0,0x5f,0x0a,0x98,0x5f,0x0a,0x24,0x5f,0x0a,0x42,0x5e,0x0a,0x81,0x50,0xfa,0x00,0x5f,0x0a,0x00,0x50,0x06,0x00,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
static const uint8_t image_hourglass6_bits[] = {0x00,0x40,0x00,0x00,0xe0,0x00,0x00,0x40,0x01,0x00,0xa0,0x02,0x00,0x10,0x05,0x00,0x08,0x0a,0x00,0x08,0x14,0x00,0xc8,0x29,0x00,0xe8,0x53,0x00,0xe8,0xe7,0x00,0xe8,0x53,0xe0,0x07,0x08,0x10,0xe0,0x07,0x0a,0x10,0x00,0x07,0x10,0x00,0x0a,0x10,0x00,0x14,0x10,0x00,0x28,0x10,0x00,0x50,0x10,0x00,0xa0,0x08,0x00,0x40,0x05,0x00,0x80,0x02,0x00,0x00,0x07,0x00,0x00,0x02,0x00};
static const uint8_t image_LoadingHourglass_bits[] = {0x00,0x00,0x00,0xe0,0xff,0x0f,0x20,0x00,0x08,0xc0,0xff,0x07,0x80,0x00,0x02,0x80,0xaa,0x02,0x80,0x54,0x02,0x80,0x28,0x02,0x00,0x11,0x01,0x00,0x82,0x00,0x00,0x54,0x00,0x00,0x28,0x00,0x00,0x28,0x00,0x00,0x44,0x00,0x00,0x92,0x00,0x00,0x01,0x01,0x80,0x00,0x02,0x80,0x00,0x02,0x80,0x00,0x02,0x80,0x00,0x02,0xc0,0xff,0x07,0x20,0x00,0x08,0xe0,0xff,0x0f,0x00,0x00,0x00};

static const uint8_t* const hourglass_frames[] = {
    image_LoadingHourglass_bits,
    image_hourglass0_bits,
    image_hourglass1_bits,
    image_hourglass2_bits,
    image_hourglass3_bits,
    image_hourglass4_bits,
    image_hourglass5_bits,
    image_hourglass6_bits
};

void oled_display_show_loading(void) {
    if (!s_ready) return;
    
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    int frame = (int)((now_ms / 125) % 8); // Muda de frame a cada 125ms perfeitamente ritmado
    
    u8g2_ClearBuffer(&s_u8g2);
    
    u8g2_DrawXBM(&s_u8g2, 52, 20, 24, 24, hourglass_frames[frame]);
    
    u8g2_SetFont(&s_u8g2, u8g2_font_helvB08_tr);
    u8g2_DrawStr(&s_u8g2, 31, 56, "Carregando...");
    
    apply_brightness_if_needed();
    oled_flush_buffer();
}

void oled_display_show_sd_error(void) {
    if (!s_ready) return;
    
    static int sd_anim_frame = 0;
    sd_anim_frame++;
    
    // Elipse vertical (focos empilhados no eixo Y): movimento mais forte no Y que no X
    int deg = (sd_anim_frame * 9) % 360;
    int offset_y = fast_scale_trig(4, fast_sin_q10(deg)); // Amplitude maior no Y
    int offset_x = fast_scale_trig(1, fast_cos_q10(deg)); // Amplitude menor no X
    
    u8g2_ClearBuffer(&s_u8g2);
    
    // Icone SD SDQuestion
    u8g2_DrawXBM(&s_u8g2, 12 + offset_x, 8 + offset_y, 35, 43, image_SDQuestion_copy_1_bits);
    
    // Textos
    u8g2_SetFont(&s_u8g2, u8g2_font_helvB08_tr);
    u8g2_DrawStr(&s_u8g2, 55, 29, "Erro ao montar");
    u8g2_DrawStr(&s_u8g2, 65, 39, "o SD card !");
    
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    const char *base_text = "verifique a fiacao ";
    u8g2_DrawStr(&s_u8g2, 6, 64, base_text);
    
    // Animacao de "ciscar" para a exclamacao (um pulinho esporadico rapido)
    int excl_x = 6 + u8g2_GetStrWidth(&s_u8g2, base_text);
    int excl_y = 64;
    if ((sd_anim_frame % 20) < 3) {
        excl_y -= 2; // Pula 2 pixels pra cima rapidinho
    }
    u8g2_DrawStr(&s_u8g2, excl_x, excl_y, "!");
    
    apply_brightness_if_needed();
    oled_flush_buffer();
}

extern uint32_t usb_manager_get_pkt_count(void);
extern bool usb_manager_is_streaming(void);

void oled_display_show_usb_dac(void)
{
    if (!s_ready) return;
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);

    bool streaming = usb_manager_is_streaming();

    // 1. Topo: Ícone de streaming (Play/Pause), Título estilizado e Bateria com Raio
    draw_play_pause_icon(streaming);
    draw_battery_icon(1, battery_get_percent());

    // Título central com brackets decorativos (mesmo padrão do artista na reprodução)
    const char *title = "USB DAC";
    u8g2_SetFont(&s_u8g2, u8g2_font_helvB08_tr);
    int tw = u8g2_GetStrWidth(&s_u8g2, title);
    int center_x = 64;
    int br_left = center_x - tw / 2 - 3;
    int br_right = center_x + tw / 2 + 3;
    u8g2_DrawLine(&s_u8g2, br_left, 11, br_right, 11);
    u8g2_DrawLine(&s_u8g2, br_left, 10, br_left, 0);
    u8g2_DrawLine(&s_u8g2, br_right, 11, br_right, 0);
    u8g2_DrawStr(&s_u8g2, center_x - tw / 2, 9, title);

    // Divisória superior
    u8g2_DrawHLine(&s_u8g2, 0, 13, 128);

    // 2. Linha de Status e Volume (y = 15..20)
    u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
    int vol = audio_player_get_volume();
    char vol_str[16];
    snprintf(vol_str, sizeof(vol_str), "Vol: %d%%", vol);
    u8g2_DrawStr(&s_u8g2, 0, 20, vol_str);

    const char *status_str = streaming ? "STREAMING ATIVO" : "AGUARDANDO USB...";
    int sw = u8g2_GetStrWidth(&s_u8g2, status_str);
    u8g2_DrawStr(&s_u8g2, 128 - sw, 20, status_str);

    // 3. Área Central: O Grande VU Meter Estéreo L / R (114 px de largura x 8 px de altura)
    float peak_l = 0.0f, peak_r = 0.0f, hold_l = 0.0f, hold_r = 0.0f;
    audio_meter_get_levels(&peak_l, &peak_r, &hold_l, &hold_r, NULL, NULL);

    const int bar_x = 10;
    const int bar_w = 117; // Frame de 117 px -> 115 px úteis de barra
    const int bar_inner_w = 115;
    const int bar_h = 8;
    const int bar_inner_h = 6;

    // --- Canal L ---
    u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
    u8g2_DrawStr(&s_u8g2, 0, 28, "L");
    u8g2_DrawFrame(&s_u8g2, bar_x, 22, bar_w, bar_h);
    int fill_l = (int)(peak_l * (float)bar_inner_w + 0.5f);
    if (fill_l > bar_inner_w) fill_l = bar_inner_w;
    if (fill_l > 0) {
        u8g2_DrawBox(&s_u8g2, bar_x + 1, 23, fill_l, bar_inner_h);
    }
    int hold_l_px = (int)(hold_l * (float)bar_inner_w + 0.5f);
    if (hold_l_px > bar_inner_w - 2) hold_l_px = bar_inner_w - 2;
    if (hold_l_px >= 2) {
        u8g2_DrawBox(&s_u8g2, bar_x + 1 + hold_l_px, 23, 2, bar_inner_h);
    }

    // --- Canal R ---
    u8g2_DrawStr(&s_u8g2, 0, 39, "R");
    u8g2_DrawFrame(&s_u8g2, bar_x, 33, bar_w, bar_h);
    int fill_r = (int)(peak_r * (float)bar_inner_w + 0.5f);
    if (fill_r > bar_inner_w) fill_r = bar_inner_w;
    if (fill_r > 0) {
        u8g2_DrawBox(&s_u8g2, bar_x + 1, 34, fill_r, bar_inner_h);
    }
    int hold_r_px = (int)(hold_r * (float)bar_inner_w + 0.5f);
    if (hold_r_px > bar_inner_w - 2) hold_r_px = bar_inner_w - 2;
    if (hold_r_px >= 2) {
        u8g2_DrawBox(&s_u8g2, bar_x + 1 + hold_r_px, 34, 2, bar_inner_h);
    }

    // Régua de escala em dB (alinhada precisamente à escala de -40 dBFS a 0 dBFS)
    u8g2_SetFont(&s_u8g2, u8g2_font_tom_thumb_4x6_t_all);
    u8g2_DrawStr(&s_u8g2, 10, 48, "-40");
    u8g2_DrawStr(&s_u8g2, 51, 48, "-24");
    u8g2_DrawStr(&s_u8g2, 85, 48, "-12");
    u8g2_DrawStr(&s_u8g2, 104, 48, "-6");
    u8g2_DrawStr(&s_u8g2, 120, 48, "0");

    // Divisória inferior
    u8g2_DrawHLine(&s_u8g2, 0, 50, 128);

    // 4. Rodapé: Cards no estilo da Reprodução ([ 24b ], [ 48k ], [ Rock ]) + Navegação
    uint32_t rate = i2s_output_get_rate();
    if (rate == 0) rate = 48000;
    char card_sr[16];
    if (rate % 1000 == 0) {
        snprintf(card_sr, sizeof(card_sr), "%uk", (unsigned)(rate / 1000));
    } else {
        snprintf(card_sr, sizeof(card_sr), "%.1fk", (float)rate / 1000.0f);
    }

    player_eq_config_t eq_cfg;
    audio_player_get_eq_config(&eq_cfg);
    char card_eq[16];
    if (eq_cfg.enabled) {
        snprintf(card_eq, sizeof(card_eq), "%.5s", eq_cfg.presets[eq_cfg.active_preset_idx].name);
    } else {
        snprintf(card_eq, sizeof(card_eq), "OFF");
    }

    u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);

    #define DRAW_DAC_BRACKET(bx, bw, ctext) do { \
        u8g2_SetDrawColor(&s_u8g2, 1); \
        u8g2_DrawLine(&s_u8g2, (bx), 63, (bx), 56); \
        u8g2_DrawLine(&s_u8g2, (bx)+1, 56, (bx)+(bw)-1, 56); \
        u8g2_DrawLine(&s_u8g2, (bx)+(bw)-1, 63, (bx)+(bw)-1, 56); \
        u8g2_DrawStr(&s_u8g2, (bx) + 1 + ((bw) - 2 - u8g2_GetStrWidth(&s_u8g2, (ctext)))/2, 63, (ctext)); \
    } while (0)

    int pad = 3;
    int bw_res = u8g2_GetStrWidth(&s_u8g2, "24b") + pad * 2;
    int bw_sr  = u8g2_GetStrWidth(&s_u8g2, card_sr) + pad * 2;
    int bw_eq  = u8g2_GetStrWidth(&s_u8g2, card_eq) + pad * 2;

    int cur_x = 0;
    DRAW_DAC_BRACKET(cur_x, bw_res, "24b");
    cur_x += bw_res + 2;
    DRAW_DAC_BRACKET(cur_x, bw_sr, card_sr);
    cur_x += bw_sr + 2;
    DRAW_DAC_BRACKET(cur_x, bw_eq, card_eq);

    #undef DRAW_DAC_BRACKET

    // Navegação alinhada à direita no rodapé
    const char *nav_str = "< Sair | EQ >";
    u8g2_SetFont(&s_u8g2, u8g2_font_tom_thumb_4x6_t_all);
    int nw = u8g2_GetStrWidth(&s_u8g2, nav_str);
    u8g2_DrawStr(&s_u8g2, 128 - nw, 63, nav_str);

    apply_brightness_if_needed();
    oled_flush_buffer();
}



void oled_display_show_led(uint8_t r, uint8_t g, uint8_t b, int selected_channel, bool enabled)
{
    if (!s_ready) return;
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);
    
    u8g2_SetFont(&s_u8g2, u8g2_font_5x8_tf);
    u8g2_DrawStr(&s_u8g2, 2, 10, "LED RGB (GPIO 38)");
    
    char status_str[24];
    snprintf(status_str, sizeof(status_str), "[%s]", enabled ? "LIGADO" : "DESLIGADO");
    u8g2_DrawStr(&s_u8g2, 85, 10, status_str);

    // 3 Canais: R, G, B
    const char *labels[3] = {"R", "G", "B"};
    uint8_t vals[3] = {r, g, b};
    int y_starts[3] = {25, 38, 51};

    for (int i = 0; i < 3; i++) {
        int y = y_starts[i];
        if (selected_channel == i) {
            u8g2_DrawBox(&s_u8g2, 2, y - 7, 124, 11);
            u8g2_SetDrawColor(&s_u8g2, 0); // Texto e barra invertidos quando selecionado
        } else {
            u8g2_SetDrawColor(&s_u8g2, 1);
        }

        char lbl[12];
        snprintf(lbl, sizeof(lbl), "%s: %3d", labels[i], vals[i]);
        u8g2_DrawStr(&s_u8g2, 6, y + 1, lbl);

        // Barra de intensidade
        u8g2_DrawFrame(&s_u8g2, 48, y - 5, 74, 8);
        int fill_w = (int)vals[i] * 72 / 255;
        if (fill_w > 0) {
            u8g2_DrawBox(&s_u8g2, 49, y - 4, fill_w, 6);
        }
    }

    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_SetFont(&s_u8g2, u8g2_font_tom_thumb_4x6_t_all);
    u8g2_DrawStr(&s_u8g2, 2, 63, "UP/DN:Canal | L/R:Ajuste | MID:Power");

    apply_brightness_if_needed();
    oled_flush_buffer();
}

void oled_display_set_brightness(uint8_t level)
{
    s_target_brightness = level;
}

typedef struct {
    char name[16];
    float band_gains[10];
    float overall_gain;
} local_player_eq_preset_t;

typedef struct {
    bool enabled;
    int active_preset_idx;
    local_player_eq_preset_t presets[10];
} local_player_eq_config_t;

void oled_display_show_top_screen(int volume, const void *eq_cfg, int eq_focus, float voltage, int percentage, int time_left_mins, int cursor)
{
    if (!s_ready) return;
    const local_player_eq_config_t *cfg = (const local_player_eq_config_t *)eq_cfg;
    
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetBitmapMode(&s_u8g2, 1);
    u8g2_SetFontMode(&s_u8g2, 1);

    // =========================================================================
    // 1o Item (Topo, y=0..20): BATERIA (cursor == 2)
    // =========================================================================
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_DrawFrame(&s_u8g2, 0, 0, 128, 21);
    if (cursor == 2) u8g2_DrawBox(&s_u8g2, 0, 0, 21, 21);
    else u8g2_DrawFrame(&s_u8g2, 0, 0, 21, 21);
    u8g2_DrawLine(&s_u8g2, 20, 0, 20, 21);

    u8g2_SetDrawColor(&s_u8g2, 2);
    u8g2_DrawXBM(&s_u8g2, 3, 3, 16, 16, image_Battery_bits);
    u8g2_SetDrawColor(&s_u8g2, 1);

    bool is_charging = battery_is_charging();
    bool is_full = battery_is_full();

    char pct_str[16];
    if (is_full) {
        snprintf(pct_str, sizeof(pct_str), "100%%");
    } else if (percentage < 0) {
        snprintf(pct_str, sizeof(pct_str), "--%%");
    } else {
        int safe_pct = percentage > 100 ? 100 : percentage;
        snprintf(pct_str, sizeof(pct_str), "%d%%", safe_pct);
    }
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    u8g2_DrawStr(&s_u8g2, 25, 15, pct_str);

    u8g2_DrawLine(&s_u8g2, 53, 0, 53, 21);

    char v_str[16];
    snprintf(v_str, sizeof(v_str), "%.2fV", voltage);
    u8g2_DrawStr(&s_u8g2, 55, 15, v_str);

    u8g2_DrawLine(&s_u8g2, 88, 0, 88, 21);

    char time_str[16];
    if (is_full) {
        snprintf(time_str, sizeof(time_str), "CHEIA");
    } else if (is_charging) {
        if (time_left_mins > 0) {
            snprintf(time_str, sizeof(time_str), "+%dh%02dm", time_left_mins / 60, time_left_mins % 60);
        } else {
            snprintf(time_str, sizeof(time_str), "CARGA");
        }
    } else {
        if (time_left_mins >= 0) {
            snprintf(time_str, sizeof(time_str), "%dh%02dm", time_left_mins / 60, time_left_mins % 60);
        } else {
            snprintf(time_str, sizeof(time_str), "--h--");
        }
    }
    u8g2_DrawStr(&s_u8g2, 90, 15, time_str);

    // =========================================================================
    // 2o Item (Meio, y=21..42): PRESETS DO EQUALIZADOR (cursor == 1)
    // =========================================================================
    u8g2_DrawFrame(&s_u8g2, 0, 21, 128, 21);
    if (cursor == 1) u8g2_DrawBox(&s_u8g2, 0, 21, 21, 21);
    else u8g2_DrawFrame(&s_u8g2, 0, 21, 21, 21);
    u8g2_DrawLine(&s_u8g2, 20, 21, 20, 42);

    u8g2_SetDrawColor(&s_u8g2, 2);
    u8g2_DrawXBM(&s_u8g2, 4, 24, 14, 16, image_menu_settings_sliders_square_bits);
    u8g2_SetDrawColor(&s_u8g2, 1);

    if (eq_focus < 0) eq_focus = 0;
    if (eq_focus > 9) eq_focus = 9;

    bool is_active = (cfg && cfg->enabled && cfg->active_preset_idx == eq_focus);

    u8g2_SetFont(&s_u8g2, u8g2_font_5x7_tr);
    if (eq_focus > 0) {
        u8g2_DrawStr(&s_u8g2, 23, 35, "<");
    }

    if (is_active) {
        u8g2_DrawBox(&s_u8g2, 29, 24, 88, 15);
    } else if (cursor == 1) {
        u8g2_DrawFrame(&s_u8g2, 29, 24, 88, 15);
    }

    char p_label[24];
    const char *pname = (cfg && cfg->presets[eq_focus].name[0] != '\0') ? cfg->presets[eq_focus].name : "Preset";
    snprintf(p_label, sizeof(p_label), "%d:%s", eq_focus + 1, pname);

    if (is_active) u8g2_SetDrawColor(&s_u8g2, 2);
    u8g2_DrawStr(&s_u8g2, 32, 35, p_label);
    if (is_active) u8g2_SetDrawColor(&s_u8g2, 1);

    if (eq_focus < 9) {
        u8g2_DrawStr(&s_u8g2, 118, 35, ">");
    }

    u8g2_DrawXBM(&s_u8g2, 122, 28, 4, 7, image_ButtonRight_bits);

    // =========================================================================
    // 3o Item (Base, y=42..63): VOLUME (cursor == 0)
    // =========================================================================
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_DrawFrame(&s_u8g2, 0, 42, 128, 22);
    if (cursor == 0) u8g2_DrawBox(&s_u8g2, 0, 42, 21, 22);
    else u8g2_DrawFrame(&s_u8g2, 0, 42, 21, 22);
    u8g2_DrawLine(&s_u8g2, 20, 42, 20, 63);

    u8g2_SetDrawColor(&s_u8g2, 2); // XOR para icone
    u8g2_DrawXBM(&s_u8g2, 3, 45, 16, 16, image_Speaker_bits);
    u8g2_SetDrawColor(&s_u8g2, 1);

    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    // Barra de volume
    int vol_w = (volume * 64) / 100;
    if (vol_w > 0) {
        u8g2_DrawBox(&s_u8g2, 24, 47, vol_w, 11);
    }
    if (cursor == 0) {
        u8g2_DrawFrame(&s_u8g2, 23, 46, 66, 13);
    }

    char vol_str[16];
    snprintf(vol_str, sizeof(vol_str), "%d%%", volume);
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_DrawStr(&s_u8g2, 93, 56, vol_str);

    apply_brightness_if_needed();
    oled_flush_buffer();
}

void oled_display_set_power_save(bool enable) {
    if (!s_ready) return;
    u8g2_SetPowerSave(&s_u8g2, enable ? 1 : 0);
    s_fb_dirty_all = true;
    if (!enable) {
        apply_brightness_if_needed();
        oled_flush_buffer();
    }
}

void oled_display_show_tela(int cursor, uint8_t brightness, int timeout_idx, int fallback_idx) {
    if (!s_ready) return;
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetFont(&s_u8g2, u8g2_font_profont11_tr);

    // Title
    u8g2_DrawStr(&s_u8g2, 42, 9, "TELA");
    u8g2_DrawLine(&s_u8g2, 0, 11, 127, 11);

    // Calculate pct
    static const uint8_t BRIGHTNESS_CURVE[] = {1, 3, 7, 15, 30, 50, 80, 120, 180, 255};
    int pct = 100;
    for (int k=0; k<=9; k++) {
        if (brightness <= BRIGHTNESS_CURVE[k]) {
            pct = (k + 1) * 10;
            break;
        }
    }

    // Row 0: Brightness (Y: 13..28)
    char buf[32];
    snprintf(buf, sizeof(buf), "Brilho: %d%%", pct);
    u8g2_DrawStr(&s_u8g2, 5, 24, buf);
    int fill_w = (pct * 48) / 100;
    u8g2_DrawFrame(&s_u8g2, 74, 16, 50, 9);
    if (fill_w > 0) u8g2_DrawBox(&s_u8g2, 75, 17, fill_w, 7);
    if (cursor == 0) u8g2_DrawFrame(&s_u8g2, 2, 13, 123, 15);

    // Row 1: Timeout de Tela (Y: 30..45)
    const char* timeouts[] = {"Nunca", "15s", "30s", "1 min", "2 min", "5 min"};
    int t_idx = timeout_idx;
    if (t_idx < 0) t_idx = 0;
    if (t_idx >= 6) t_idx = 5;
    snprintf(buf, sizeof(buf), "Auto-off: %s", timeouts[t_idx]);
    u8g2_DrawStr(&s_u8g2, 5, 41, buf);
    if (cursor == 1) u8g2_DrawFrame(&s_u8g2, 2, 30, 123, 15);

    // Row 2: Retorno para Reprodução (Y: 47..62)
    const char* fallbacks[] = {"Desativado", "5s", "10s", "15s", "30s", "1 min"};
    int fb_idx = fallback_idx;
    if (fb_idx < 0) fb_idx = 0;
    if (fb_idx >= 6) fb_idx = 5;
    snprintf(buf, sizeof(buf), "Retorno: %s", fallbacks[fb_idx]);
    u8g2_DrawStr(&s_u8g2, 5, 58, buf);
    if (cursor == 2) u8g2_DrawFrame(&s_u8g2, 2, 47, 123, 15);

    apply_brightness_if_needed();
    oled_flush_buffer();
}
void oled_display_show_unsupported(const char *filename)
{
    if (!s_ready) return;
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);
    
    // rect 6
    u8g2_DrawBox(&s_u8g2, 6, 25, 29, 15);
    // seele
    u8g2_DrawXBM(&s_u8g2, 46, 0, 82, 64, image_seele_bits);
    
    // extract extension
    const char *ext = (filename != NULL) ? strrchr(filename, '.') : NULL;
    if (!ext) ext = "arquivo";

    // string 2
    u8g2_SetFont(&s_u8g2, u8g2_font_helvB08_tr); // A fonte certa para caber
    u8g2_DrawStr(&s_u8g2, 1, 12, "Formato");
    
    // string 3
    u8g2_SetDrawColor(&s_u8g2, 0); // Texto invertido na caixa preenchida
    int ext_w = u8g2_GetStrWidth(&s_u8g2, ext);
    u8g2_DrawStr(&s_u8g2, 6 + (29 - ext_w)/2, 36, ext);
    
    // string 4
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_DrawStr(&s_u8g2, 0, 62, "Nao");
    
    // string 5
    u8g2_DrawStr(&s_u8g2, 21, 62, "Suportado!");
    
    apply_brightness_if_needed();
    oled_flush_buffer();
}



void oled_display_show_eq_preset_list(int cursor, int active_preset, bool eq_enabled, const void *eq_cfg)
{
    if (!s_ready) return;
    const local_player_eq_config_t *cfg = (const local_player_eq_config_t *)eq_cfg;
    
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetBitmapMode(&s_u8g2, 1);
    u8g2_SetFontMode(&s_u8g2, 1);
    
    // Header
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_DrawBox(&s_u8g2, 0, 0, 128, 12);
    u8g2_SetDrawColor(&s_u8g2, 0); // invert
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    u8g2_DrawStr(&s_u8g2, 4, 9, "PERFIL EQ");
    
    // Badge box para status ON / OFF
    const char *st_str = eq_enabled ? "ON" : "OFF";
    int badge_w = eq_enabled ? 22 : 26;
    int badge_x = 128 - badge_w - 2;
    u8g2_SetDrawColor(&s_u8g2, 0); // fundo preto do badge dentro da barra branca
    u8g2_DrawBox(&s_u8g2, badge_x, 1, badge_w, 10);
    u8g2_SetDrawColor(&s_u8g2, 1); // texto branco dentro do badge
    int tx = badge_x + (badge_w - u8g2_GetStrWidth(&s_u8g2, st_str)) / 2;
    u8g2_DrawStr(&s_u8g2, tx, 9, st_str);
    
    // 3 items visible simultaneously
    int top_idx = 0;
    if (cursor > 2) {
        top_idx = cursor - 2;
        if (top_idx > 7) top_idx = 7;
    }
    
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    for (int row = 0; row < 3; row++) {
        int idx = top_idx + row;
        if (idx >= 10) break;
        
        int y = 14 + row * 13;
        bool is_cursor = (idx == cursor);
        bool is_active = (eq_enabled && idx == active_preset);
        
        if (is_cursor) {
            u8g2_SetDrawColor(&s_u8g2, 1);
            u8g2_DrawBox(&s_u8g2, 0, y, 128, 12);
            u8g2_SetDrawColor(&s_u8g2, 0); // invert text
        } else {
            u8g2_SetDrawColor(&s_u8g2, 1);
        }
        
        char line[32];
        const char *name = (cfg && cfg->presets[idx].name[0] != '\0') ? cfg->presets[idx].name : "Preset";
        snprintf(line, sizeof(line), "%s%d.%s", is_active ? "*" : " ", idx + 1, name);
        u8g2_DrawStr(&s_u8g2, 2, y + 10, line);
        
        if (is_cursor) {
            u8g2_DrawStr(&s_u8g2, 118, y + 10, ">");
        }
    }
    
    // Footer hints
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_DrawLine(&s_u8g2, 0, 54, 128, 54);
    u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
    u8g2_DrawStr(&s_u8g2, 2, 62, "> Bandas | Segure: Renomear");
    
    apply_brightness_if_needed();
    oled_flush_buffer();
}

void oled_display_show_keyboard(const char *text, int cursor, int grid_x, int grid_y, int page, bool is_confirming)
{
    if (!s_ready) return;
    
    u8g2_ClearBuffer(&s_u8g2);
    
    // Draw text box
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_DrawFrame(&s_u8g2, 0, 0, 128, 14);
    
    // Draw text
    u8g2_SetFont(&s_u8g2, u8g2_font_5x7_tr);
    char buf[24];
    snprintf(buf, sizeof(buf), "%s", text ? text : "");
    int text_x = 3;
    u8g2_DrawStr(&s_u8g2, text_x, 10, buf);
    
    // Draw cursor
    int cur_pos = cursor;
    if (cur_pos < 0) cur_pos = 0;
    int blen = (int)strlen(buf);
    if (cur_pos > blen) cur_pos = blen;
    char tmp[24];
    if (cur_pos >= (int)sizeof(tmp)) cur_pos = (int)sizeof(tmp) - 1;
    memcpy(tmp, buf, cur_pos);
    tmp[cur_pos] = '\0';
    int cx = text_x + u8g2_GetStrWidth(&s_u8g2, tmp);
    u8g2_DrawLine(&s_u8g2, cx, 2, cx, 11);
    
    // Draw keyboard grid
    int y_start = 17;
    int key_w = 12;
    int key_h = 13;
    
    static const char kbd_pages[3][3][10] = {
        {
            {'q','w','e','r','t','y','u','i','o','p'},
            {'a','s','d','f','g','h','j','k','l','\0'},
            {'z','x','c','v','b','n','m','_','<','\r'}
        },
        {
            {'Q','W','E','R','T','Y','U','I','O','P'},
            {'A','S','D','F','G','H','J','K','L','\0'},
            {'Z','X','C','V','B','N','M','_','<','\r'}
        },
        {
            {'1','2','3','4','5','6','7','8','9','0'},
            {'!','@','#','$','%','&','*','-','+','\0'},
            {'(',')','[',']','{','}','?','_','<','\r'}
        }
    };
    
    int cur_page = page;
    if (cur_page < 0 || cur_page > 2) cur_page = 0;
    
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 10; c++) {
            char ch = kbd_pages[cur_page][r][c];
            if (ch == '\0') continue;
            
            int kx = 4 + c * key_w;
            int ky = y_start + r * key_h;
            
            bool is_focused = (!is_confirming && r == grid_y && c == grid_x);
            if (is_focused) {
                u8g2_SetDrawColor(&s_u8g2, 1);
                u8g2_DrawBox(&s_u8g2, kx, ky, key_w, key_h);
                u8g2_SetDrawColor(&s_u8g2, 0); // invert
            } else {
                u8g2_SetDrawColor(&s_u8g2, 1);
            }
            
            if (ch == '_') {
                u8g2_DrawLine(&s_u8g2, kx+3, ky+10, kx+9, ky+10);
                u8g2_DrawLine(&s_u8g2, kx+3, ky+8, kx+3, ky+10);
                u8g2_DrawLine(&s_u8g2, kx+9, ky+8, kx+9, ky+10);
            } else if (ch == '<') {
                u8g2_DrawLine(&s_u8g2, kx+2, ky+6, kx+5, ky+3);
                u8g2_DrawLine(&s_u8g2, kx+2, ky+6, kx+5, ky+9);
                u8g2_DrawLine(&s_u8g2, kx+2, ky+6, kx+9, ky+6);
            } else if (ch == '\r') {
                u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
                u8g2_DrawStr(&s_u8g2, kx+2, ky+9, "OK");
                u8g2_SetFont(&s_u8g2, u8g2_font_5x7_tr);
            } else {
                char str[2] = {ch, '\0'};
                int w = u8g2_GetStrWidth(&s_u8g2, str);
                u8g2_DrawStr(&s_u8g2, kx + (key_w - w)/2, ky + 9, str);
            }
            u8g2_SetDrawColor(&s_u8g2, 1);
        }
    }
    
    // Bottom indicator
    u8g2_SetFont(&s_u8g2, u8g2_font_5x7_tr);
    const char *pstr = (cur_page == 0) ? "[abc]" : (cur_page == 1) ? "[ABC]" : "[123]";
    u8g2_DrawStr(&s_u8g2, 128 - u8g2_GetStrWidth(&s_u8g2, pstr) - 2, 62, pstr);
    
    u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
    u8g2_DrawStr(&s_u8g2, 2, 62, "2xCentro: Pag");
    
    // Modal confirmation dialog
    if (is_confirming) {
        u8g2_SetDrawColor(&s_u8g2, 0); // clear background
        u8g2_DrawBox(&s_u8g2, 8, 8, 112, 48);
        u8g2_SetDrawColor(&s_u8g2, 1);
        u8g2_DrawFrame(&s_u8g2, 8, 8, 112, 48);
        
        u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
        u8g2_DrawStr(&s_u8g2, 24, 22, "Salvar nome?");
        
        u8g2_SetFont(&s_u8g2, u8g2_font_5x7_tr);
        char quote_name[24];
        snprintf(quote_name, sizeof(quote_name), "\"%s\"", text ? text : "");
        int qw = u8g2_GetStrWidth(&s_u8g2, quote_name);
        u8g2_DrawStr(&s_u8g2, (128 - qw)/2, 34, quote_name);
        
        u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
        u8g2_DrawStr(&s_u8g2, 14, 48, "<- Voltar");
        u8g2_DrawStr(&s_u8g2, 86, 48, "OK ->");
    }
    
    apply_brightness_if_needed();
    oled_flush_buffer();
}

void oled_display_show_sort_mode(int mode)
{
    if (!s_ready) return;
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_SetFont(&s_u8g2, u8g2_font_profont12_tr);

    // Titulo
    u8g2_DrawStr(&s_u8g2, 18, 10, "ORDENAR FAIXAS");
    u8g2_DrawLine(&s_u8g2, 0, 13, 127, 13);

    // Opcao 0: Nome A-Z
    bool is_name = (mode == 0);
    if (is_name) {
        u8g2_DrawBox(&s_u8g2, 4, 18, 120, 15);
        u8g2_SetDrawColor(&s_u8g2, 0);
    } else {
        u8g2_DrawFrame(&s_u8g2, 4, 18, 120, 15);
        u8g2_SetDrawColor(&s_u8g2, 1);
    }
    u8g2_DrawStr(&s_u8g2, 10, 29, "Nome (A -> Z)");

    // Opcao 1: Data de Modificacao
    u8g2_SetDrawColor(&s_u8g2, 1);
    if (!is_name) {
        u8g2_DrawBox(&s_u8g2, 4, 36, 120, 15);
        u8g2_SetDrawColor(&s_u8g2, 0);
    } else {
        u8g2_DrawFrame(&s_u8g2, 4, 36, 120, 15);
        u8g2_SetDrawColor(&s_u8g2, 1);
    }
    u8g2_DrawStr(&s_u8g2, 10, 47, "Data (Tracklist)");

    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_SetFont(&s_u8g2, u8g2_font_tom_thumb_4x6_t_all);
    u8g2_DrawStr(&s_u8g2, 8, 60, "JOY: Alternar | ESQ: Voltar");

    apply_brightness_if_needed();
    oled_flush_buffer();
}

void oled_display_show_deepsleep_cfg(int current_idx)
{
    if (!s_ready) return;
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);

    // Titulo
    u8g2_SetFont(&s_u8g2, u8g2_font_profont12_tr);
    u8g2_DrawStr(&s_u8g2, 28, 10, "DEEP SLEEP");
    u8g2_DrawLine(&s_u8g2, 0, 13, 127, 13);

    // Subtitulo / Label
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    u8g2_DrawStr(&s_u8g2, 8, 26, "Auto desligar apos:");

    // Opcoes
    static const char *labels[] = {
        "Desativado",
        "1 minuto",
        "3 minutos",
        "5 minutos",
        "10 minutos",
        "15 minutos",
        "30 minutos"
    };
    int idx = current_idx;
    if (idx < 0) idx = 0;
    if (idx >= 7) idx = 6;

    // Moldura de selecao
    u8g2_DrawFrame(&s_u8g2, 8, 31, 112, 18);
    u8g2_SetFont(&s_u8g2, u8g2_font_profont12_tr);
    int text_w = u8g2_GetStrWidth(&s_u8g2, labels[idx]);
    int text_x = (128 - text_w) / 2;
    u8g2_DrawStr(&s_u8g2, text_x, 44, labels[idx]);

    // Setas indicativas laterais
    if (idx > 0) {
        u8g2_DrawStr(&s_u8g2, 14, 44, "<");
    }
    if (idx < 6) {
        u8g2_DrawStr(&s_u8g2, 108, 44, ">");
    }

    // Rodape com instrucoes
    u8g2_SetFont(&s_u8g2, u8g2_font_tom_thumb_4x6_t_all);
    u8g2_DrawStr(&s_u8g2, 4, 60, "[>] Dormir Agora  |  [OK] Salvar");

    apply_brightness_if_needed();
    oled_flush_buffer();
}

void oled_display_show_ota_progress(int percent, const char *status_msg)
{
    if (!s_ready) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);

    // Titulo
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    const char *title = "ATUALIZANDO FIRMWARE";
    int tw = u8g2_GetStrWidth(&s_u8g2, title);
    u8g2_DrawStr(&s_u8g2, (OLED_WIDTH - tw) / 2, 9, title);
    u8g2_DrawHLine(&s_u8g2, 0, 12, OLED_WIDTH);

    // Barra de progresso com moldura e preenchimento proporcional (0-100%)
    const int bar_x = 8;
    const int bar_y = 20;
    const int bar_w = OLED_WIDTH - 16; // 112 px
    const int bar_h = 14;

    u8g2_DrawFrame(&s_u8g2, bar_x, bar_y, bar_w, bar_h);
    int fill = ((bar_w - 2) * percent) / 100;
    if (fill > 0) {
        u8g2_DrawBox(&s_u8g2, bar_x + 1, bar_y + 1, fill, bar_h - 2);
    }

    // Texto de porcentagem centralizado na barra
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    char pct_label[16];
    snprintf(pct_label, sizeof(pct_label), "%d%%", percent);
    int pw = u8g2_GetStrWidth(&s_u8g2, pct_label);
    int px = bar_x + (bar_w - pw) / 2;
    u8g2_SetDrawColor(&s_u8g2, 2); // Modo XOR (inversao) para legibilidade perfeita sobre a barra
    u8g2_DrawStr(&s_u8g2, px, bar_y + 11, pct_label);
    u8g2_SetDrawColor(&s_u8g2, 1);

    // Mensagem de status
    u8g2_SetFont(&s_u8g2, u8g2_font_5x8_tf);
    const char *msg = (status_msg && status_msg[0]) ? status_msg : "Gravando Flash SPI...";
    int mw = u8g2_GetStrWidth(&s_u8g2, msg);
    u8g2_DrawStr(&s_u8g2, (OLED_WIDTH - mw) / 2, 47, msg);

    // Aviso fixo na base
    u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
    const char *warn = "NAO DESLIGUE O MPS3!";
    int ww = u8g2_GetStrWidth(&s_u8g2, warn);
    u8g2_DrawStr(&s_u8g2, (OLED_WIDTH - ww) / 2, 60, warn);

    apply_brightness_if_needed();
    oled_flush_buffer();
}

static marquee_t s_podcast_marquee;

void oled_display_show_podcast_sync(const char *program, const char *title,
                                   int cur_idx, int total_idx,
                                   int percent, float speed_kbs,
                                   const char *status_msg)
{
    if (!s_ready) return;

    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_SetBitmapMode(&s_u8g2, 1);
    u8g2_SetFontMode(&s_u8g2, 1);

    // Linha 1: Titulo / Top Bar
    u8g2_SetFont(&s_u8g2, u8g2_font_5x8_tf);
    char header[48];
    if (total_idx > 0) {
        snprintf(header, sizeof(header), "[%d/%d] %.16s", cur_idx, total_idx,
                 (program && program[0]) ? program : "PODCAST");
    } else {
        snprintf(header, sizeof(header), "SINCRONIZANDO PODCASTS");
    }
    u8g2_DrawStr(&s_u8g2, 2, 8, header);

    // Linha divisoria
    u8g2_DrawHLine(&s_u8g2, 0, 10, OLED_WIDTH);

    // Linha 2: Titulo do Episodio (com marquee se longo)
    u8g2_SetFont(&s_u8g2, u8g2_font_profont11_tr);
    char safe_title[80];
    sanitize_for_display((title && title[0]) ? title : status_msg, safe_title, sizeof(safe_title));
    int title_w = u8g2_GetStrWidth(&s_u8g2, safe_title);
    if (title_w <= OLED_WIDTH - 4) {
        u8g2_DrawStr(&s_u8g2, 2, 22, safe_title);
    } else {
        u8g2_SetClipWindow(&s_u8g2, 2, 12, OLED_WIDTH - 2, 24);
        draw_marquee(&s_podcast_marquee, safe_title, 2, 22, 2);
        u8g2_SetMaxClipWindow(&s_u8g2);
    }

    // Linha 3: Barra de Progresso
    const int bar_x = 4;
    const int bar_y = 27;
    const int bar_w = OLED_WIDTH - 8; // 120 px
    const int bar_h = 12;

    u8g2_DrawFrame(&s_u8g2, bar_x, bar_y, bar_w, bar_h);
    int fill = ((bar_w - 2) * percent) / 100;
    if (fill > 0) {
        u8g2_DrawBox(&s_u8g2, bar_x + 1, bar_y + 1, fill, bar_h - 2);
    }

    // Porcentagem invertida sobre a barra
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    char pct_str[16];
    snprintf(pct_str, sizeof(pct_str), "%d%%", percent);
    int pw = u8g2_GetStrWidth(&s_u8g2, pct_str);
    u8g2_SetDrawColor(&s_u8g2, 2); // XOR
    u8g2_DrawStr(&s_u8g2, bar_x + (bar_w - pw) / 2, bar_y + 10, pct_str);
    u8g2_SetDrawColor(&s_u8g2, 1);

    // Linha 4: Velocidade e Status
    u8g2_SetFont(&s_u8g2, u8g2_font_5x8_tf);
    char info_str[48];
    if (speed_kbs >= 1024.0f) {
        snprintf(info_str, sizeof(info_str), "%.1f MB/s", speed_kbs / 1024.0f);
    } else if (speed_kbs > 0.0f) {
        snprintf(info_str, sizeof(info_str), "%.0f KB/s", speed_kbs);
    } else {
        snprintf(info_str, sizeof(info_str), "%.24s", (status_msg && status_msg[0]) ? status_msg : "Aguardando...");
    }
    u8g2_DrawStr(&s_u8g2, 4, 51, info_str);

    // Linha 5: Atalho de cancelamento no rodape
    u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
    const char *footer = "< Voltar / Cancelar";
    u8g2_DrawStr(&s_u8g2, OLED_WIDTH - u8g2_GetStrWidth(&s_u8g2, footer) - 4, 61, footer);

    apply_brightness_if_needed();
    oled_flush_buffer();
}

void oled_display_show_wifi_qr(const char *ssid, const char *pass,
                               const char *tag_label, int cur_idx, int total_idx)
{
    if (!s_ready) return;

    // Constroi payload padronizado no formato WIFI:S:<SSID>;T:<WPA|nopass>;P:<PASS>;;
    char qr_text[128];
    if (pass && pass[0] != '\0') {
        snprintf(qr_text, sizeof(qr_text), "WIFI:S:%s;T:WPA;P:%s;;", (ssid && ssid[0]) ? ssid : "", pass);
    } else {
        snprintf(qr_text, sizeof(qr_text), "WIFI:S:%s;T:nopass;;", (ssid && ssid[0]) ? ssid : "");
    }

    // Cache local de geracao do QR Code para economizar CPU
    static char s_last_qr_text[128] = {0};
    static QRCode s_cached_qrcode;
    static uint8_t s_cached_modules[200]; // Suficiente para QR Code ate Versao 5 (172 bytes)
    static int8_t s_cached_res = -1;

    if (strcmp(qr_text, s_last_qr_text) != 0) {
        strncpy(s_last_qr_text, qr_text, sizeof(s_last_qr_text) - 1);
        s_last_qr_text[sizeof(s_last_qr_text) - 1] = '\0';
        s_cached_res = -1;

        // Tenta versoes 3, 4, 5 e depois 2
        for (uint8_t v = 3; v <= 5; v++) {
            if (qrcode_initText(&s_cached_qrcode, s_cached_modules, v, ECC_LOW, qr_text) == 0) {
                s_cached_res = 0;
                break;
            }
        }
        if (s_cached_res != 0) {
            if (qrcode_initText(&s_cached_qrcode, s_cached_modules, 2, ECC_LOW, qr_text) == 0) {
                s_cached_res = 0;
            }
        }
    }

    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_SetBitmapMode(&s_u8g2, 1);
    u8g2_SetFontMode(&s_u8g2, 1);

    // Fundo branco do QR Code de 62x62 pixels no lado esquerdo
    u8g2_DrawBox(&s_u8g2, 1, 1, 62, 62);

    if (s_cached_res == 0) {
        int scale = 1;
        if (s_cached_qrcode.size * 2 <= 62) {
            scale = 2;
        }
        int qr_px = s_cached_qrcode.size * scale;
        int ox = 1 + (62 - qr_px) / 2;
        int oy = 1 + (62 - qr_px) / 2;

        u8g2_SetDrawColor(&s_u8g2, 0); // Modulos escuros no fundo branco
        for (uint8_t y = 0; y < s_cached_qrcode.size; y++) {
            for (uint8_t x = 0; x < s_cached_qrcode.size; x++) {
                if (qrcode_getModule(&s_cached_qrcode, x, y)) {
                    u8g2_DrawBox(&s_u8g2, ox + x * scale, oy + y * scale, scale, scale);
                }
            }
        }
    } else {
        u8g2_SetDrawColor(&s_u8g2, 0);
        u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
        u8g2_DrawStr(&s_u8g2, 6, 32, "QR ERRO");
    }

    // Lado direito da tela: informacoes da rede (cor branca)
    u8g2_SetDrawColor(&s_u8g2, 1);

    // Linha 1: Tag e contador de paginas [X/N] (garantia matematica de nao sobreposicao)
    u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
    if (total_idx > 1) {
        char idx_str[16];
        snprintf(idx_str, sizeof(idx_str), "[%d/%d]", cur_idx, total_idx);
        int iw = u8g2_GetStrWidth(&s_u8g2, idx_str);
        
        // Desenha marcador de pagina alinhado a direita
        u8g2_DrawStr(&s_u8g2, OLED_WIDTH - iw, 8, idx_str);

        // Prepara tag com corte de seguranca caso exceda o espaco restante
        int max_tag_w = (OLED_WIDTH - iw - 2) - 66;
        if (max_tag_w > 0) {
            char safe_tag[16];
            strncpy(safe_tag, tag_label ? tag_label : "WIFI", sizeof(safe_tag) - 1);
            safe_tag[sizeof(safe_tag) - 1] = '\0';
            while (strlen(safe_tag) > 0 && u8g2_GetStrWidth(&s_u8g2, safe_tag) > max_tag_w) {
                safe_tag[strlen(safe_tag) - 1] = '\0';
            }
            u8g2_DrawStr(&s_u8g2, 66, 8, safe_tag);
        }
    } else {
        u8g2_DrawStr(&s_u8g2, 66, 8, tag_label ? tag_label : "HOTSPOT AP");
    }
    u8g2_DrawHLine(&s_u8g2, 65, 11, OLED_WIDTH - 65);

    // Linha 2 e 3: SSID
    u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
    u8g2_DrawStr(&s_u8g2, 66, 19, "REDE:");
    u8g2_SetFont(&s_u8g2, u8g2_font_haxrcorp4089_tr);
    char disp_ssid[24];
    strncpy(disp_ssid, (ssid && ssid[0]) ? ssid : "Sem SSID", sizeof(disp_ssid) - 1);
    disp_ssid[sizeof(disp_ssid) - 1] = '\0';
    while (strlen(disp_ssid) > 0 && u8g2_GetStrWidth(&s_u8g2, disp_ssid) > (OLED_WIDTH - 66)) {
        disp_ssid[strlen(disp_ssid) - 1] = '\0';
    }
    u8g2_DrawStr(&s_u8g2, 66, 28, disp_ssid);

    // Linha 4 e 5: Senha
    u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
    u8g2_DrawStr(&s_u8g2, 66, 38, "SENHA:");
    u8g2_SetFont(&s_u8g2, u8g2_font_haxrcorp4089_tr);
    char disp_pass[24];
    if (pass && pass[0]) {
        strncpy(disp_pass, pass, sizeof(disp_pass) - 1);
        disp_pass[sizeof(disp_pass) - 1] = '\0';
    } else {
        strncpy(disp_pass, "<aberta>", sizeof(disp_pass) - 1);
        disp_pass[sizeof(disp_pass) - 1] = '\0';
    }
    while (strlen(disp_pass) > 0 && u8g2_GetStrWidth(&s_u8g2, disp_pass) > (OLED_WIDTH - 66)) {
        disp_pass[strlen(disp_pass) - 1] = '\0';
    }
    u8g2_DrawStr(&s_u8g2, 66, 47, disp_pass);

    // Linha 6: IP e Host para acesso Web
    u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
    u8g2_DrawStr(&s_u8g2, 66, 56, "192.168.4.1");
    u8g2_DrawStr(&s_u8g2, 66, 63, "mps3.local");

    apply_brightness_if_needed();
    oled_flush_buffer();
}

void oled_display_show_clock(const rtc_clock_info_t *info)
{
    if (!s_ready) return;
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);

    if (!info || !info->rtc_ok) {
        u8g2_SetFont(&s_u8g2, u8g2_font_profont12_tr);
        u8g2_DrawStr(&s_u8g2, 12, 11, "RELOGIO RTC DS3231");
        u8g2_DrawLine(&s_u8g2, 0, 14, 127, 14);

        u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
        u8g2_DrawStr(&s_u8g2, 6, 32, "MODULO NAO DETECTADO");
        u8g2_SetFont(&s_u8g2, u8g2_font_tom_thumb_4x6_t_all);
        u8g2_DrawStr(&s_u8g2, 16, 46, "I2C SDA:10 SCL:9 (0x68)");
        u8g2_DrawStr(&s_u8g2, 22, 58, "[ESQ] Voltar ao Menu");

        apply_brightness_if_needed();
        oled_flush_buffer();
        return;
    }

    // Cabecalho
    u8g2_SetFont(&s_u8g2, u8g2_font_profont11_tr);
    u8g2_DrawStr(&s_u8g2, 4, 10, "RELOGIO RTC");
    u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
    u8g2_DrawStr(&s_u8g2, 76, 9, "TCXO +/-2ppm");
    u8g2_DrawLine(&s_u8g2, 0, 12, 127, 12);

    // Horario Principal: HH:MM:SS
    char hms_str[16];
    snprintf(hms_str, sizeof(hms_str), "%02d:%02d:%02d", info->hour, info->min, info->sec);
    u8g2_SetFont(&s_u8g2, u8g2_font_profont17_tr);
    u8g2_DrawStr(&s_u8g2, 14, 30, hms_str);

    // Milissegundos / Fracoes de segundo: .mmm
    char ms_str[8];
    snprintf(ms_str, sizeof(ms_str), ".%03d", info->millis);
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    u8g2_DrawStr(&s_u8g2, 88, 30, ms_str);

    // Data completa: DIA, DD/MM/AAAA
    static const char *wdays[] = {"DOM", "SEG", "TER", "QUA", "QUI", "SEX", "SAB"};
    char date_str[32];
    snprintf(date_str, sizeof(date_str), "%s, %02d/%02d/%04d",
             wdays[info->wday % 7], info->day, info->month, info->year);
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    int date_w = u8g2_GetStrWidth(&s_u8g2, date_str);
    int date_x = (128 - date_w) / 2;
    u8g2_DrawStr(&s_u8g2, date_x, 44, date_str);

    // Linha divisoria do rodape
    u8g2_DrawLine(&s_u8g2, 0, 48, 127, 48);

    // Telemetria do TCXO: Temperatura com resolucao maxima nativa de 0.25 C
    char temp_str[24];
    snprintf(temp_str, sizeof(temp_str), "TEMP: %.2f C", info->temp_c);
    u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
    u8g2_DrawStr(&s_u8g2, 4, 56, temp_str);

    // Status da bateria do RTC / OSF
    if (info->osc_stopped) {
        u8g2_DrawStr(&s_u8g2, 78, 56, "BAT: AJUSTAR!");
    } else {
        u8g2_DrawStr(&s_u8g2, 88, 56, "BAT: OK");
    }

    // Dica de saida
    u8g2_DrawStr(&s_u8g2, 36, 63, "[ESQ] Voltar");

    apply_brightness_if_needed();
    oled_flush_buffer();
}

void oled_display_show_chrono(bool running, uint32_t elapsed_ms, const uint32_t *laps, int lap_count)
{
    if (!s_ready) return;
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetFontMode(&s_u8g2, 1);
    u8g2_SetBitmapMode(&s_u8g2, 1);
    u8g2_SetDrawColor(&s_u8g2, 1);

    // 1. Cabeçalho: "CRONOMETRO" e status [RODANDO] / [PAUSADO] / [ZERADO]
    u8g2_SetFont(&s_u8g2, u8g2_font_profont11_tr);
    u8g2_DrawStr(&s_u8g2, 3, 10, "CRONOMETRO");
    u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
    const char *status_str = running ? "[RODANDO]" : (elapsed_ms == 0 ? "[ZERADO]" : "[PAUSADO]");
    int sw = u8g2_GetStrWidth(&s_u8g2, status_str);
    u8g2_DrawStr(&s_u8g2, 126 - sw, 9, status_str);
    u8g2_DrawHLine(&s_u8g2, 0, 12, 128);

    // 2. Display Principal de Tempo: MM:SS.cc
    uint32_t total_sec = elapsed_ms / 1000;
    uint32_t mins = total_sec / 60;
    uint32_t secs = total_sec % 60;
    uint32_t cents = (elapsed_ms % 1000) / 10;
    if (mins > 999) mins = 999;

    char main_time[32];
    snprintf(main_time, sizeof(main_time), "%02u:%02u.%02u", (unsigned)mins, (unsigned)secs, (unsigned)cents);
    u8g2_SetFont(&s_u8g2, u8g2_font_logisoso16_tn);
    int tw = u8g2_GetStrWidth(&s_u8g2, main_time);
    int tx = (128 - tw) / 2;
    u8g2_DrawStr(&s_u8g2, tx, 33, main_time);

    // 3. Seção de Voltas (Laps)
    u8g2_DrawHLine(&s_u8g2, 4, 37, 120);
    u8g2_SetFont(&s_u8g2, u8g2_font_5x7_tr);
    if (lap_count > 0 && laps) {
        char lap_buf[32];
        int last_idx = lap_count - 1;
        uint32_t l_sec = laps[last_idx] / 1000;
        uint32_t l_m = l_sec / 60;
        uint32_t l_s = l_sec % 60;
        uint32_t l_c = (laps[last_idx] % 1000) / 10;
        snprintf(lap_buf, sizeof(lap_buf), "V%d: %02u:%02u.%02u", last_idx + 1, (unsigned)l_m, (unsigned)l_s, (unsigned)l_c);
        u8g2_DrawStr(&s_u8g2, 6, 47, lap_buf);

        if (lap_count > 1) {
            int prev_idx = lap_count - 2;
            uint32_t p_sec = laps[prev_idx] / 1000;
            uint32_t p_m = p_sec / 60;
            uint32_t p_s = p_sec % 60;
            uint32_t p_c = (laps[prev_idx] % 1000) / 10;
            snprintf(lap_buf, sizeof(lap_buf), "V%d: %02u:%02u.%02u", prev_idx + 1, (unsigned)p_m, (unsigned)p_s, (unsigned)p_c);
            u8g2_DrawStr(&s_u8g2, 70, 47, lap_buf);
        }
    } else {
        u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
        u8g2_DrawStr(&s_u8g2, 24, 46, "NENHUMA VOLTA MARCADA");
    }

    // 4. Rodapé com Instruções de Controle
    u8g2_DrawHLine(&s_u8g2, 0, 51, 128);
    u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
    if (running) {
        u8g2_DrawStr(&s_u8g2, 4, 61, "[C] Pausar  [DIR] Volta  [ESQ] Sair");
    } else {
        u8g2_DrawStr(&s_u8g2, 4, 61, "[C] Iniciar  [DIR] Zerar  [ESQ] Sair");
    }

    apply_brightness_if_needed();
    oled_flush_buffer();
}

void oled_display_show_lock_clock(const rtc_clock_info_t *info, bool is_locked, const char *track_title, int battery_pct, bool battery_charging)
{
    if (!s_ready) return;
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetFontMode(&s_u8g2, 1);
    u8g2_SetBitmapMode(&s_u8g2, 1);
    u8g2_SetDrawColor(&s_u8g2, 1);

    // 1. Barra Superior: Ícone de Cadeado/Status à esquerda, Bateria à direita
    u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
    if (is_locked) {
        u8g2_DrawFrame(&s_u8g2, 4, 3, 6, 5); // corpo do cadeado
        u8g2_DrawCircle(&s_u8g2, 6, 2, 2, U8G2_DRAW_UPPER_RIGHT | U8G2_DRAW_UPPER_LEFT); // alça
        u8g2_DrawStr(&s_u8g2, 13, 8, "BLOQUEADO");
    } else {
        u8g2_DrawStr(&s_u8g2, 4, 8, "MPS3 REPOUSO");
    }

    if (battery_pct >= 0) {
        char bat_buf[16];
        snprintf(bat_buf, sizeof(bat_buf), "%d%%%s", battery_pct, battery_charging ? "+" : "");
        int bw = u8g2_GetStrWidth(&s_u8g2, bat_buf);
        u8g2_DrawStr(&s_u8g2, 115 - bw, 8, bat_buf);
        draw_battery_badge(118, 2, battery_pct);
    }
    u8g2_DrawHLine(&s_u8g2, 0, 10, 128);

    // 2. Horário Principal em destaque
    char hms[16];
    if (info && info->rtc_ok) {
        snprintf(hms, sizeof(hms), "%02d:%02d:%02d", info->hour, info->min, info->sec);
    } else {
        snprintf(hms, sizeof(hms), "--:--:--");
    }
    u8g2_SetFont(&s_u8g2, u8g2_font_logisoso16_tn);
    int hw = u8g2_GetStrWidth(&s_u8g2, hms);
    u8g2_DrawStr(&s_u8g2, (128 - hw) / 2, 29, hms);

    // 3. Data completa com Sakamoto: QUI, 15/10/2026
    static const char *wdays[] = {"DOM", "SEG", "TER", "QUA", "QUI", "SEX", "SAB"};
    char date_buf[32];
    if (info && info->rtc_ok) {
        snprintf(date_buf, sizeof(date_buf), "%s, %02d/%02d/%04d",
                 wdays[info->wday % 7], info->day, info->month, info->year);
    } else {
        snprintf(date_buf, sizeof(date_buf), "RTC INDISPONIVEL");
    }
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    int dw = u8g2_GetStrWidth(&s_u8g2, date_buf);
    u8g2_DrawStr(&s_u8g2, (128 - dw) / 2, 42, date_buf);

    u8g2_DrawHLine(&s_u8g2, 0, 47, 128);

    // 4. Rodapé: Título da música tocando ou instrução de desbloqueio
    u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
    if (is_locked) {
        u8g2_DrawStr(&s_u8g2, 16, 56, "[SEGURE C] DESBLOQUEAR");
        if (track_title && track_title[0]) {
            char title_short[32];
            snprintf(title_short, sizeof(title_short), "▶ %s", track_title);
            title_short[26] = '\0';
            u8g2_DrawStr(&s_u8g2, 4, 63, title_short);
        }
    } else {
        if (track_title && track_title[0]) {
            char title_short[32];
            snprintf(title_short, sizeof(title_short), "▶ %s", track_title);
            title_short[26] = '\0';
            u8g2_DrawStr(&s_u8g2, 4, 55, title_short);
        } else {
            u8g2_DrawStr(&s_u8g2, 24, 55, "ENTRANDO EM REPOUSO");
        }
        u8g2_DrawStr(&s_u8g2, 22, 63, "[TOQUE] CANCELAR SLEEP");
    }

    apply_brightness_if_needed();
    oled_flush_buffer();
}

void oled_display_show_lock_clock_cfg(int opt_idx)
{
    if (!s_ready) return;
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetFontMode(&s_u8g2, 1);
    u8g2_SetBitmapMode(&s_u8g2, 1);
    u8g2_SetDrawColor(&s_u8g2, 1);

    // Cabeçalho
    u8g2_SetFont(&s_u8g2, u8g2_font_profont11_tr);
    u8g2_DrawStr(&s_u8g2, 8, 10, "RELOGIO DE BLOQUEIO");
    u8g2_DrawHLine(&s_u8g2, 0, 12, 128);

    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    u8g2_DrawStr(&s_u8g2, 12, 26, "Tempo de Exibicao:");

    static const char *opts[] = {"Desativado", "3 segundos", "5 segundos", "8 segundos", "10 segundos", "15 segundos"};
    int idx = opt_idx;
    if (idx < 0) idx = 0;
    if (idx >= 6) idx = 5;

    u8g2_DrawFrame(&s_u8g2, 14, 32, 100, 16);
    u8g2_SetFont(&s_u8g2, u8g2_font_7x13B_tr);
    int ow = u8g2_GetStrWidth(&s_u8g2, opts[idx]);
    u8g2_DrawStr(&s_u8g2, (128 - ow) / 2, 44, opts[idx]);

    // Instruções no rodapé
    u8g2_DrawHLine(&s_u8g2, 0, 52, 128);
    u8g2_SetFont(&s_u8g2, u8g2_font_4x6_tr);
    u8g2_DrawStr(&s_u8g2, 8, 60, "[CIMA/BAIXO] Ajustar  [C/ESQ] Salvar");

    apply_brightness_if_needed();
    oled_flush_buffer();
}

esp_err_t oled_display_capture_buffer(uint8_t *dest_buf)
{
    if (!s_ready || !dest_buf) return ESP_FAIL;
    uint8_t *ptr = u8g2_GetBufferPtr(&s_u8g2);
    if (!ptr) return ESP_FAIL;
    memcpy(dest_buf, ptr, 1024);
    return ESP_OK;
}

