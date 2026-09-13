#ifndef OLED_DISPLAY_H
#define OLED_DISPLAY_H

#include <stdint.h>
#include "esp_err.h"
#include "audio_player.h" // playback_state_t

#ifdef __cplusplus
extern "C" {
#endif

// Inicializa o barramento I2C e o display SSD1306 via u8g2.
esp_err_t oled_display_init(void);

// Novo menu principal com ícones (Player, WiFi, Conf, USB, Game).
void oled_display_show_main_menu(int cursor);

// Atualiza os frames das animações (WiFi e Conf). Deve ser chamada
// a cada iteração do loop da display_task (antes de desenhar).
void oled_display_update_animations(void);

// Tela de reproducao: icone de play/pause + bateria no topo, nome do
// artista (com setas decorativas), titulo rolando em fonte grande, barra
// de progresso (10px) com o tempo decorrido/total EMBUTIDO dentro dela (o
// texto inverte - fica "negativo" - na parte que o preenchimento ja'
// cobriu, pra continuar legivel), album, e o formato do arquivo (ex.:
// ".Flac / 16 Bits / 44Khz") como 3 "crachas" com colchete decorativo.
void oled_display_show_now_playing(const playback_state_t *state);

// Tela de menu: lista de musicas/pastas com o item selecionado marcado
// por uma seta e rolando horizontalmente se o nome for mais longo que a
// tela. Se `now_playing` tiver uma faixa carregada (track_loaded), mostra
// tambem uma barrinha fina de reproducao fixa no topo (tempo decorrido/
// total + titulo rolando), mesmo navegando pelo menu.
void oled_display_show_list(const char **names, int count, int cursor,
                             const playback_state_t *now_playing);

// Mostra uma mensagem simples de 2 linhas (status, erros, "sem cartao", etc).
void oled_display_show_message(const char *line1, const char *line2);

// Telas do Bluetooth
void oled_display_show_bt_devices(const char **names, int count, int cursor, bool scanning);
void oled_display_show_bt_connected(const char *dev_name, const char *codec_name, uint32_t sample_rate, int8_t rssi);

// Apaga o conteudo da tela (usado por inatividade de controles, ou
// quando o dispositivo entra em repouso/bloqueio).
void oled_display_blank(void);

// Mostra uma barra de volume (0-100) ocupando a tela inteira, exibida
// temporariamente quando o usuario ajusta o volume pelos controles.
void oled_display_show_volume(int volume_percent);
void oled_display_show_balance(int balance);

// Desenha uma geracao do Game of Life. Usado pelo item Game do menu.
void oled_display_show_locked(void);

// Reinicia o Game of Life com um padrao aleatorio novo.
void oled_display_reset_life(void);

// Mostra o indicador de "cartao SD cedido ao host USB" (ver usb_storage.h -
// exibida enquanto usb_storage_is_exposed() for true).
void oled_display_show_usb_mode(void);

// Tela de progresso de transferencia (USB ou WiFi): rotulo (ex: status da
// rede/modo), nome do arquivo atual (rolando se for longo) e barra de
// progresso com % e tempo estimado restante embaixo. percent fora de
// 0-100 e' grampeado; eta_sec < 0 mostra "calculando..." em vez de um
// numero. overall_pct/count descrevem o progresso de um LOTE de varios
// arquivos - passe overall_pct=-1 e count=0 (ou 1) quando nao houver esse
// conceito (ex.: modo USB, ou upload avulso de 1 arquivo so').
void oled_display_show_transfer(const char *label, const char *filename, int percent, int eta_sec,
                                 int overall_pct, int count);

// Telas de status WiFi
void oled_display_show_wifi_idle(const char *ip, const char *mdns_host, bool dns_active, int rssi);
void oled_display_show_wifi_connected(const char *ip, const char *mdns_host, int rssi, int clients);
void oled_display_show_wifi_transfer(const char *filename, int file_pct, int overall_pct, int idx, int count, float kbps, int eta_sec, int total_eta, int rssi);
void oled_display_show_wifi_done(int files_received);

// Tela do equalizador: mostra 4 bandas + ganho geral como barras verticais,
// com a banda selecionada destacada. selected_band: 0-3=bandas, 4=ganho geral.
// eq_enabled: true quando o EQ esta ativo.
// gains: array de 11 floats [bass, mid, midhigh, treble, overall] em dB.
void oled_display_show_eq(const float gains[11], int selected_band, bool eq_enabled);
void oled_display_show_led(uint8_t r, uint8_t g, uint8_t b, int selected_channel, bool enabled);
void oled_display_set_brightness(uint8_t level);
void oled_display_set_power_save(bool enable);
void oled_display_show_tela(int cursor, uint8_t brightness, int timeout_idx);
void oled_display_show_top_screen(int volume, const void *eq_cfg, int eq_focus, float voltage, int percentage, int time_left_mins, int cursor);
void oled_display_show_usb_prompt(int cursor);
void oled_display_show_usb_msc(void);
void oled_display_show_usb_dac(void);
void oled_display_show_sd_error(void);
void oled_display_show_loading(void);

void oled_display_show_eq_preset_list(int cursor, int active_preset, bool eq_enabled, const void *eq_cfg);
void oled_display_show_keyboard(const char *text, int cursor, int grid_x, int grid_y, int page, bool is_confirming);
void oled_display_show_unsupported(const char *filename);

#ifdef __cplusplus
}
#endif

#endif // OLED_DISPLAY_H
