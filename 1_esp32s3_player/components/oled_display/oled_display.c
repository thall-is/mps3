#include "oled_display.h"
#include "pinos.h"
#include "u8g2_hal.h"
#include "battery.h"
#include "bitmaps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

static void apply_brightness_if_needed(void) {
    if (s_target_brightness != s_current_brightness) {
        s_current_brightness = s_target_brightness;
        u8g2_SetContrast(&s_u8g2, s_current_brightness);
    }
}


// Frames para animaÃ§Ãµes WiFi
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
    u8g2_InitDisplay(&s_u8g2);
    u8g2_SetPowerSave(&s_u8g2, 0);
    u8g2_ClearBuffer(&s_u8g2);
    apply_brightness_if_needed();
    u8g2_SendBuffer(&s_u8g2);

    s_ready = true;
    return ESP_OK;
}

static void format_time(uint32_t total_seconds, char *out, size_t out_len)
{
    uint32_t m = total_seconds / 60;
    uint32_t s = total_seconds % 60;
    snprintf(out, out_len, "%02u:%02u", (unsigned)m, (unsigned)s);
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
void oled_display_show_now_playing(const playback_state_t *state)
{
    if (!s_ready || !state) return;

    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);

    // --- Topo: play/pause, artista dinâmico, bateria ---
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
    u8g2_SetFont(&s_u8g2, u8g2_font_t0_22b_tr); // A fonte do mockup
    
    int tw = u8g2_GetStrWidth(&s_u8g2, safe_title);
    if (tw <= 128) {
        int tx = (128 - tw) / 2;
        u8g2_DrawStr(&s_u8g2, tx, 28, safe_title);
    } else {
        u8g2_SetClipWindow(&s_u8g2, 0, 12, 128, 31);
        draw_marquee(&s_now_playing_marquee, safe_title, 0, 28, 2);
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
    
    // Extrai formato de state->format_name ou da extensao de filename
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
        
        int pad = 2; // espacamento de padding
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
    apply_brightness_if_needed();
    u8g2_SendBuffer(&s_u8g2);
}

static marquee_t s_list_topbar_marquee = {0};

// Nova funÃ§Ã£o para marquee com largura disponÃ­vel explÃ­cita
void draw_marquee_width(marquee_t *m, const char *text, int x_start, int available_w, int y, int step_px)
{
    marquee_reset_if_new(m, text);

    int text_w = u8g2_GetStrWidth(&s_u8g2, m->text);
    if (text_w <= available_w) {
        // Centraliza no espaÃ§o disponÃ­vel
        int tx = x_start + (available_w - text_w) / 2;
        u8g2_DrawStr(&s_u8g2, tx, y, m->text);
        m->offset_px = 0;
        return;
    }

    const int gap = 16; // espaÃ§o entre o fim e a cÃ³pia repetida
    int x = x_start - m->offset_px;
    u8g2_DrawStr(&s_u8g2, x, y, m->text);
    u8g2_DrawStr(&s_u8g2, x + text_w + gap, y, m->text);

    m->offset_px += step_px;
    if (m->offset_px >= text_w + gap) {
        m->offset_px = 0;
    }
}

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
        u8g2_SendBuffer(&s_u8g2);
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
                draw_marquee(&s_list_cursor_marquee, safe_name, 1, y_text, 2);
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
    u8g2_SendBuffer(&s_u8g2);
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
    u8g2_SendBuffer(&s_u8g2);
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
    u8g2_SendBuffer(&s_u8g2);
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
    u8g2_SendBuffer(&s_u8g2);
}

void oled_display_blank(void)
{
    if (!s_ready) return;
    u8g2_ClearBuffer(&s_u8g2);
    apply_brightness_if_needed();
    u8g2_SendBuffer(&s_u8g2);
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
    u8g2_SendBuffer(&s_u8g2);
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
    u8g2_SendBuffer(&s_u8g2);
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
    u8g2_SendBuffer(&s_u8g2);
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
    u8g2_SendBuffer(&s_u8g2);
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
    u8g2_SendBuffer(&s_u8g2);
}

// Controle de animaÃ§Ã£o do menu principal
static int wifi_frame = 0;
static int conf_frame = 0;
static int game_frame = 0;
static uint32_t last_anim_ms = 0;
#define ANIM_INTERVAL_MS 42   // ~24 fps (mesmo ritmo dos mockups)

void oled_display_update_animations(void) {
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // Atualiza animaÃ§Ãµes WiFi a cada 42ms (~24fps)
    if (now - s_wifi_last_frame_ms >= 42) {
        s_wifi_attention_frame = (s_wifi_attention_frame + 1) % WIFI_ATTENTION_CLOUD_FRAMES;
        s_wifi_sync_frame = (s_wifi_sync_frame + 1) % WIFI_SYNC_CLOUD_FRAMES;
        s_wifi_download_frame = (s_wifi_download_frame + 1) % WIFI_DOWNLOAD_CLOUD_FRAMES;
        s_wifi_last_frame_ms = now;
    }

    // Atualiza animaÃ§Ãµes do menu principal (mesmo ritmo)
    if (now - last_anim_ms >= ANIM_INTERVAL_MS) {
        wifi_frame = (wifi_frame + 1) % MENU_WIFI_FRAMES;
        conf_frame = (conf_frame + 1) % MENU_CONF_FRAMES;
        game_frame = (game_frame + 1) % MENU_GAME_FRAMES;
        last_anim_ms = now;
    }
}

void oled_display_show_main_menu(int cursor) {
    if (!s_ready) return;

    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetFontMode(&s_u8g2, 1);
    u8g2_SetBitmapMode(&s_u8g2, 1);
    u8g2_SetDrawColor(&s_u8g2, 1);

    switch (cursor) {
        case 0: // Player
            // Icone estatico do Player (128x64, 4bpp)
            u8g2_DrawXBMP(&s_u8g2, 32, 0, MENU_PLAYER_W, MENU_PLAYER_H, menu_player_bits);
            // play
            u8g2_SetFont(&s_u8g2, u8g2_font_profont22_tr);
            u8g2_DrawStr(&s_u8g2, 1, 39, "Play");
            // wifi
            u8g2_SetFont(&s_u8g2, u8g2_font_profont15_tr);
            u8g2_DrawStr(&s_u8g2, 7, 53, "Wifi");
            // BT
            u8g2_SetFont(&s_u8g2, u8g2_font_profont10_tr);
            u8g2_DrawStr(&s_u8g2, 10, 62, "BT");
            break;

        case 1: // WiFi
            // Animacao WiFi (64x64)
            u8g2_DrawXBMP(&s_u8g2, 64, -1, MENU_WIFI_W, MENU_WIFI_H,
                          menu_wifi_frames[wifi_frame]);
            u8g2_SetFont(&s_u8g2, u8g2_font_profont22_tr);
            u8g2_DrawStr(&s_u8g2, 1, 39, "Wifi");
            u8g2_SetFont(&s_u8g2, u8g2_font_profont15_tr);
            u8g2_DrawStr(&s_u8g2, 7, 53, "BT");
            u8g2_SetFont(&s_u8g2, u8g2_font_profont10_tr);
            u8g2_DrawStr(&s_u8g2, 12, 64, "Conf");
            u8g2_SetFont(&s_u8g2, u8g2_font_profont15_tr);
            u8g2_DrawStr(&s_u8g2, 7, 18, "Play");
            break;

        case 2: // Bluetooth
            // Emblema Bluetooth renderizado geometricamente com precisao
            u8g2_DrawCircle(&s_u8g2, 94, 31, 27, U8G2_DRAW_ALL);
            u8g2_DrawCircle(&s_u8g2, 94, 31, 26, U8G2_DRAW_ALL);
            for (int d = -1; d <= 1; d++) {
                u8g2_DrawLine(&s_u8g2, 94 + d, 14, 94 + d, 48);
                u8g2_DrawLine(&s_u8g2, 94 + d, 14, 105 + d, 23);
                u8g2_DrawLine(&s_u8g2, 105 + d, 23, 83 + d, 39);
                u8g2_DrawLine(&s_u8g2, 83 + d, 23, 105 + d, 39);
                u8g2_DrawLine(&s_u8g2, 105 + d, 39, 94 + d, 48);
            }
            u8g2_SetFont(&s_u8g2, u8g2_font_profont22_tr);
            u8g2_DrawStr(&s_u8g2, 2, 37, "BT");
            u8g2_SetFont(&s_u8g2, u8g2_font_profont15_tr);
            u8g2_DrawStr(&s_u8g2, 9, 20, "Wifi");
            u8g2_DrawStr(&s_u8g2, 7, 53, "Conf");
            u8g2_SetFont(&s_u8g2, u8g2_font_profont10_tr);
            u8g2_DrawStr(&s_u8g2, 9, 6, "Play");
            u8g2_DrawStr(&s_u8g2, 12, 62, "USB");
            break;

        case 3: // Conf (chave inglesa)
            // Animacao Conf (64x64)
            u8g2_DrawXBMP(&s_u8g2, 62, 2, MENU_CONF_W, MENU_CONF_H,
                          menu_conf_frames[conf_frame]);
            u8g2_SetFont(&s_u8g2, u8g2_font_profont22_tr);
            u8g2_DrawStr(&s_u8g2, 0, 37, "Conf");
            u8g2_SetFont(&s_u8g2, u8g2_font_profont15_tr);
            u8g2_DrawStr(&s_u8g2, 9, 20, "BT");
            u8g2_DrawStr(&s_u8g2, 11, 53, "USB");
            u8g2_SetFont(&s_u8g2, u8g2_font_profont10_tr);
            u8g2_DrawStr(&s_u8g2, 9, 6, "Wifi");
            u8g2_DrawStr(&s_u8g2, 12, 62, "Life");
            break;

        case 4: // USB
            u8g2_DrawXBMP(&s_u8g2, 25, 4, MENU_USB_W, MENU_USB_H, menu_usb_bits);
            u8g2_SetFont(&s_u8g2, u8g2_font_profont22_tr);
            u8g2_DrawStr(&s_u8g2, 5, 37, "USB");
            u8g2_SetFont(&s_u8g2, u8g2_font_profont10_tr);
            u8g2_DrawStr(&s_u8g2, 12, 8, "BT");
            u8g2_SetFont(&s_u8g2, u8g2_font_profont15_tr);
            u8g2_DrawStr(&s_u8g2, 9, 20, "Conf");
            u8g2_DrawStr(&s_u8g2, 8, 53, "Life");
            break;

        case 5: // Game of Life
            u8g2_DrawXBMP(&s_u8g2, 62, 0, MENU_GAME_W, MENU_GAME_H,
                          menu_game_frames[game_frame]);
            u8g2_SetFont(&s_u8g2, u8g2_font_profont22_tr);
            u8g2_DrawStr(&s_u8g2, 0, 37, "Life");
            u8g2_SetFont(&s_u8g2, u8g2_font_profont15_tr);
            u8g2_DrawStr(&s_u8g2, 13, 20, "USB");
            u8g2_SetFont(&s_u8g2, u8g2_font_profont10_tr);
            u8g2_DrawStr(&s_u8g2, 13, 7, "Conf");
            break;
        }

    apply_brightness_if_needed();
    u8g2_SendBuffer(&s_u8g2);
}

// =========================================================================
// Telas de status WiFi
// =========================================================================

void oled_display_show_wifi_idle(const char *ip, const char *mdns_host, bool dns_active, int rssi)
{
    if (!s_ready) return;

    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_SetBitmapMode(&s_u8g2, 1);
    u8g2_SetFontMode(&s_u8g2, 1);

    // AnimaÃ§Ã£o attention_cloud (posiÃ§Ã£o: (32,3))
    u8g2_DrawXBMP(&s_u8g2, 32, 3, 64, 64,
                  wifi_attention_cloud_frames[s_wifi_attention_frame]);

    // mDNS (canto superior direito) â€“ mockup: (68,9)
    u8g2_SetFont(&s_u8g2, u8g2_font_profont10_tr);
    u8g2_DrawStr(&s_u8g2, 68, 9, "//");
    if (mdns_host) u8g2_DrawStr(&s_u8g2, 76, 9, mdns_host);

    // IP (mockup: (59,64))
    u8g2_DrawStr(&s_u8g2, 59, 64, ip ? ip : "0.0.0.0");

    // DNS local (mockup: (0,64))
    if (dns_active) {
        u8g2_DrawStr(&s_u8g2, 0, 64, "DNS local");
    }

    // Frame superior esquerdo (mockup: (0,0,65,12))
    u8g2_SetFont(&s_u8g2, u8g2_font_haxrcorp4089_tr);
    u8g2_DrawFrame(&s_u8g2, 0, 0, 65, 12);
    if (rssi != -1) {
        char db[8];
        snprintf(db, sizeof(db), "%d dBm", rssi);
        u8g2_DrawStr(&s_u8g2, 2, 9, db);
    } else {
        u8g2_DrawStr(&s_u8g2, 2, 9, "AP");
    }

    // Barra de sinal (se RSSI disponÃ­vel)
    if (rssi != -1) {
        int pct = 0;
        if (rssi <= -90) pct = 0;
        else if (rssi >= -30) pct = 100;
        else pct = (int)((rssi + 90) * 100 / 60);
        if (pct > 100) pct = 100;
        if (pct < 0) pct = 0;

        int bar_x = 0, bar_y = 55, bar_w = 50, bar_h = 6;
        u8g2_DrawFrame(&s_u8g2, bar_x, bar_y, bar_w, bar_h);
        if (pct > 0) {
            int fill_w = (bar_w - 2) * pct / 100;
            u8g2_DrawBox(&s_u8g2, bar_x + 1, bar_y + 1, fill_w, bar_h - 2);
        }
    }

    apply_brightness_if_needed();
    u8g2_SendBuffer(&s_u8g2);
}

void oled_display_show_wifi_connected(const char *ip, const char *mdns_host, int rssi, int clients)
{
    if (!s_ready) return;

    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_SetBitmapMode(&s_u8g2, 1);
    u8g2_SetFontMode(&s_u8g2, 1);

    // AnimaÃ§Ã£o synchronize_cloud (posiÃ§Ã£o: (32,3))
    u8g2_DrawXBMP(&s_u8g2, 32, 3, 64, 64,
                  wifi_sync_cloud_frames[s_wifi_sync_frame]);

    // Texto "Conectado" (mockup: (42,63))
    u8g2_SetFont(&s_u8g2, u8g2_font_profont10_tr);
    u8g2_DrawStr(&s_u8g2, 42, 63, "Conectado");

    // Frame superior com RSSI (mockup: (53,9) para "-0db")
    u8g2_SetFont(&s_u8g2, u8g2_font_haxrcorp4089_tr);
    u8g2_DrawFrame(&s_u8g2, 0, 0, 128, 12);
    if (rssi != -1) {
        char db[8];
        snprintf(db, sizeof(db), "%d dBm", rssi);
        u8g2_DrawStr(&s_u8g2, 53, 9, db);
    } else {
        u8g2_DrawStr(&s_u8g2, 53, 9, "-0db");
    }

    apply_brightness_if_needed();
    u8g2_SendBuffer(&s_u8g2);
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
    u8g2_SendBuffer(&s_u8g2);
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
    u8g2_SendBuffer(&s_u8g2);
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
    u8g2_SendBuffer(&s_u8g2);
}


static const uint8_t image_Speaker_bits[] = {0x00,0x00,0x60,0x00,0x60,0x01,0x6e,0x03,0xef,0x06,0xef,0x04,0xef,0x0c,0xef,0x0c,0xef,0x0c,0xef,0x04,0xef,0x06,0x6e,0x03,0x60,0x01,0x60,0x00,0x00,0x00,0x00,0x00};
static const uint8_t image_Battery_bits[] = {0x00,0x00,0x00,0x00,0x80,0x01,0xc0,0x03,0x20,0x04,0x20,0x04,0xa0,0x05,0x20,0x04,0xa0,0x05,0x20,0x04,0xa0,0x05,0x20,0x04,0xc0,0x03,0x00,0x00,0x00,0x00,0x00,0x00};
static const uint8_t image_menu_settings_sliders_square_bits[] = {0x00,0x00,0x1e,0x00,0xf3,0x3f,0x1e,0x00,0x00,0x00,0x00,0x00,0x00,0x1e,0xff,0x33,0x00,0x1e,0x00,0x00,0x00,0x00,0x1e,0x00,0xf3,0x3f,0x1e,0x00,0x00,0x00,0x00,0x00};

// =========================================================================
// Telas de USB Sentinela
// =========================================================================
void oled_display_show_usb_prompt(int cursor)
{
    if (!s_ready) return;
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_SetFont(&s_u8g2, u8g2_font_5x8_tf);

    u8g2_DrawStr(&s_u8g2, 2, 10, "USB Conectado ao PC!");
    u8g2_DrawStr(&s_u8g2, 2, 22, "Escolha o modo:");

    // Opcao 1: Pendrive
    if (cursor == 0) u8g2_DrawBox(&s_u8g2, 2, 30-7, 124, 9);
    u8g2_SetDrawColor(&s_u8g2, cursor == 0 ? 0 : 1);
    u8g2_DrawStr(&s_u8g2, 4, 30, "1. Armazenamento");

    // Opcao 2: DAC
    u8g2_SetDrawColor(&s_u8g2, 1);
    if (cursor == 1) u8g2_DrawBox(&s_u8g2, 2, 42-7, 124, 9);
    u8g2_SetDrawColor(&s_u8g2, cursor == 1 ? 0 : 1);
    u8g2_DrawStr(&s_u8g2, 4, 42, "2. DAC USB (Audio)");

    apply_brightness_if_needed();
    u8g2_SendBuffer(&s_u8g2);
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
    u8g2_SendBuffer(&s_u8g2);
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
    
    static int call_count = 0;
    call_count++;
    int frame = (call_count / 3) % 8; // Muda a cada 3 chamadas (150ms) pra nÃ£o ficar rÃ¡pido demais
    
    u8g2_ClearBuffer(&s_u8g2);
    
    u8g2_DrawXBM(&s_u8g2, 52, 20, 24, 24, hourglass_frames[frame]);
    
    u8g2_SetFont(&s_u8g2, u8g2_font_helvB08_tr);
    u8g2_DrawStr(&s_u8g2, 31, 56, "Carregando...");
    
    apply_brightness_if_needed();
    u8g2_SendBuffer(&s_u8g2);
}

void oled_display_show_sd_error(void) {
    if (!s_ready) return;
    
    static int sd_anim_frame = 0;
    sd_anim_frame++;
    
    // Elipse vertical (focos empilhados no eixo Y): movimento mais forte no Y que no X
    float t = sd_anim_frame * 0.15f;
    int offset_y = (int)(4.0f * sinf(t)); // Amplitude maior no Y
    int offset_x = (int)(1.0f * cosf(t)); // Amplitude menor no X
    
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
    u8g2_SendBuffer(&s_u8g2);
}

void oled_display_show_usb_dac(void)
{
    if (!s_ready) return;
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_SetFont(&s_u8g2, u8g2_font_5x8_tf);
    u8g2_DrawStr(&s_u8g2, 20, 15, "DAC USB (UAC2)");
    u8g2_DrawStr(&s_u8g2, 5, 25, "Qualidade: 24-bits 48kHz");
    
    u8g2_SetFont(&s_u8g2, u8g2_font_tom_thumb_4x6_t_all);
    u8g2_DrawStr(&s_u8g2, 10, 45, "Use o joystick para Play/Pause");
    u8g2_DrawStr(&s_u8g2, 10, 55, "Segure ESQUERDA p/ sair");
    apply_brightness_if_needed();
    u8g2_SendBuffer(&s_u8g2);
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
    u8g2_SendBuffer(&s_u8g2);
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
    // 1o Item (Topo, y=0..20): VOLUME (cursor == 0)
    // =========================================================================
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_DrawFrame(&s_u8g2, 0, 0, 127, 21);
    if (cursor == 0) u8g2_DrawBox(&s_u8g2, 0, 0, 21, 21);
    else u8g2_DrawFrame(&s_u8g2, 0, 0, 21, 21);
    u8g2_DrawLine(&s_u8g2, 20, 0, 20, 21);

    u8g2_SetDrawColor(&s_u8g2, 2); // XOR para icone
    u8g2_DrawXBM(&s_u8g2, 3, 3, 16, 16, image_Speaker_bits);
    u8g2_SetDrawColor(&s_u8g2, 1);

    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    // Barra de volume
    int vol_w = (volume * 64) / 100;
    if (vol_w > 0) {
        u8g2_DrawBox(&s_u8g2, 24, 5, vol_w, 11);
    }
    if (cursor == 0) {
        u8g2_DrawFrame(&s_u8g2, 23, 4, 66, 13);
    }

    char vol_str[8];
    snprintf(vol_str, sizeof(vol_str), "%d%%", volume);
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    u8g2_SetDrawColor(&s_u8g2, 1);
    u8g2_DrawStr(&s_u8g2, 93, 14, vol_str);

    // =========================================================================
    // 2o Item (Meio, y=21..42): PRESETS DO EQUALIZADOR (cursor == 1)
    // =========================================================================
    u8g2_DrawFrame(&s_u8g2, 0, 21, 127, 21);
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
    // 3o Item (Base, y=42..63): BATERIA (cursor == 2)
    // =========================================================================
    u8g2_DrawFrame(&s_u8g2, 0, 42, 127, 22);
    if (cursor == 2) u8g2_DrawBox(&s_u8g2, 0, 42, 21, 22);
    else u8g2_DrawFrame(&s_u8g2, 0, 42, 21, 22);
    u8g2_DrawLine(&s_u8g2, 20, 42, 20, 63);

    u8g2_SetDrawColor(&s_u8g2, 2);
    u8g2_DrawXBM(&s_u8g2, 3, 45, 16, 16, image_Battery_bits);
    u8g2_SetDrawColor(&s_u8g2, 1);

    bool is_charging = battery_is_charging();
    bool is_full = battery_is_full();

    char pct_str[8];
    if (is_full) {
        snprintf(pct_str, sizeof(pct_str), "100%%");
    } else {
        snprintf(pct_str, sizeof(pct_str), "%d%%", percentage);
    }
    u8g2_SetFont(&s_u8g2, u8g2_font_6x10_tr);
    u8g2_DrawStr(&s_u8g2, 25, 57, pct_str);

    u8g2_DrawLine(&s_u8g2, 53, 42, 53, 63);

    char v_str[8];
    snprintf(v_str, sizeof(v_str), "%.2fV", voltage);
    u8g2_DrawStr(&s_u8g2, 55, 57, v_str);

    u8g2_DrawLine(&s_u8g2, 88, 42, 88, 63);

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
    u8g2_DrawStr(&s_u8g2, 90, 57, time_str);

    apply_brightness_if_needed();
    u8g2_SendBuffer(&s_u8g2);
}

void oled_display_set_power_save(bool enable) {
    if (!s_ready) return;
    u8g2_SetPowerSave(&s_u8g2, enable ? 1 : 0);
    if (!enable) {
        apply_brightness_if_needed();
        u8g2_SendBuffer(&s_u8g2);
    }
}

void oled_display_show_tela(int cursor, uint8_t brightness, int timeout_idx) {
    if (!s_ready) return;
    u8g2_ClearBuffer(&s_u8g2);
    u8g2_SetFont(&s_u8g2, u8g2_font_profont12_tr);

    // Title
    u8g2_DrawStr(&s_u8g2, 40, 10, "TELA");
    u8g2_DrawLine(&s_u8g2, 0, 13, 127, 13);

    // Calculate pct
    static const uint8_t BRIGHTNESS_CURVE[] = {1, 3, 7, 15, 30, 50, 80, 120, 180, 255};
    int pct = 100;
    for (int k=0; k<=9; k++) {
        if (brightness <= BRIGHTNESS_CURVE[k]) {
            pct = (k + 1) * 10;
            break;
        }
    }

    // Row 0: Brightness
    char buf[32];
    snprintf(buf, sizeof(buf), "Brilho: %d%%", pct);
    u8g2_DrawStr(&s_u8g2, 5, 30, buf);
    int fill_w = (pct * 60) / 100;
    u8g2_DrawFrame(&s_u8g2, 60, 22, 62, 9);
    if (fill_w > 0) u8g2_DrawBox(&s_u8g2, 61, 23, fill_w, 7);
    
    if (cursor == 0) u8g2_DrawFrame(&s_u8g2, 2, 19, 123, 15);

    // Row 1: Timeout
    const char* timeouts[] = {"Nunca", "15s", "30s", "1 min", "2 min", "5 min"};
    int t_idx = timeout_idx;
    if (t_idx < 0) t_idx = 0;
    if (t_idx >= 6) t_idx = 5;
    snprintf(buf, sizeof(buf), "Auto-off: %s", timeouts[t_idx]);
    u8g2_DrawStr(&s_u8g2, 5, 50, buf);
    
    if (cursor == 1) u8g2_DrawFrame(&s_u8g2, 2, 39, 123, 15);

    apply_brightness_if_needed();
    u8g2_SendBuffer(&s_u8g2);
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
    u8g2_SendBuffer(&s_u8g2);
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
    u8g2_SendBuffer(&s_u8g2);
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
    if (cur_pos > (int)strlen(buf)) cur_pos = (int)strlen(buf);
    char tmp[24];
    strncpy(tmp, buf, cur_pos);
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
    u8g2_SendBuffer(&s_u8g2);
}
