#ifndef OLED_DISPLAY_H
#define OLED_DISPLAY_H

/**
 * @file oled_display.h
 * @brief Interface Gráfica de Usuário e Controle do Display OLED SSD1306 (128x64)
 *
 * Gerencia a renderização de telas, animações, layouts e menus monocromáticos
 * através da biblioteca U8g2 operando sobre o barramento I2C Master (GPIO 10 SDA,
 * GPIO 9 SCL) a 400 kHz, sincronizado na taxa estável de 25.0 FPS cravados.
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "audio_player.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inicializa o display OLED SSD1306 e a camada gráfica U8g2.
 *
 * Configura o barramento I2C, os modos transparentes de fontes (`u8g2_SetFontMode`)
 * e bitmaps para evitar artefatos em caixas de seleção, e liga a alimentação de tela.
 *
 * @return ESP_OK se o display respondeu e foi inicializado com sucesso.
 */
esp_err_t oled_display_init(void);

/**
 * @brief Desenha o menu principal em carrossel horizontal com ícones animados.
 *
 * @param[in] cursor Índice do ícone selecionado (Player, WiFi, Conf, USB, Game).
 */
void oled_display_show_main_menu(int cursor);

/**
 * @brief Atualiza contadores de frames e transições de animação gráfica.
 *
 * Deve ser invocada a cada ciclo da `display_task` antes da renderização de tela.
 */
void oled_display_update_animations(void);

/**
 * @brief Renderiza a tela principal de reprodução de música (Now Playing).
 *
 * Exibe status de Play/Pause, percentual da bateria, artista, título com rolagem
 * de texto (*marquee*), barra de progresso proporcional com tempo embutido em modo XOR,
 * álbum e badges decorativos com formato, bits por amostra e taxa de amostragem.
 *
 * @param[in] state Ponteiro para o estado atual de reprodução (`playback_state_t`).
 */
void oled_display_show_now_playing(const playback_state_t *state);

/**
 * @brief Exibe a lista de navegação de arquivos e diretórios.
 *
 * Desenha os itens com cursor visual, rolagem de nomes longos e barra superior
 * fina de reprodução caso uma música esteja tocando em segundo plano.
 *
 * @param[in] names       Vetor de strings com os nomes das entradas.
 * @param[in] count       Total de entradas na lista.
 * @param[in] cursor      Índice do item selecionado.
 * @param[in] now_playing Estado da música em segundo plano (ou NULL).
 */
void oled_display_show_list(const char **names, int count, int cursor,
                             const playback_state_t *now_playing);

/**
 * @brief Exibe mensagem informativa temporária de até duas linhas de texto.
 *
 * @param[in] line1 Texto da primeira linha.
 * @param[in] line2 Texto da segunda linha (ou NULL).
 */
void oled_display_show_message(const char *line1, const char *line2);

/**
 * @brief Exibe a lista de fones de ouvido e dispositivos Bluetooth encontrados.
 *
 * @param[in] names    Vetor com nomes dos dispositivos descobertos.
 * @param[in] count    Quantidade de dispositivos na lista.
 * @param[in] cursor   Índice do dispositivo focado.
 * @param[in] scanning true se a varredura Bluetooth ainda estiver ativa.
 */
void oled_display_show_bt_devices(const char **names, int count, int cursor, bool scanning);

/**
 * @brief Exibe status de conexão Bluetooth ativa com fone/caixa de som.
 *
 * @param[in] dev_name    Nome do dispositivo conectado.
 * @param[in] codec_name  Nome do codec ativo (ex: "Sony LDAC", "SBC").
 * @param[in] sample_rate Frequência em Hz (ex: 96000).
 * @param[in] rssi        Potência do sinal de rádio em dBm.
 */
void oled_display_show_bt_connected(const char *dev_name, const char *codec_name, uint32_t sample_rate, int8_t rssi);

/**
 * @brief Limpa o buffer de vídeo e apaga todos os pixels da tela.
 */
void oled_display_blank(void);

/**
 * @brief Exibe popup de volume em tela cheia com barra percentual e valor numérico.
 *
 * @param[in] volume_percent Volume atual de 0 a 100%.
 */
void oled_display_show_volume(int volume_percent);

/**
 * @brief Exibe popup de ajuste de balanço estéreo L/R.
 *
 * @param[in] balance Valor de -100 (L) a +100 (R).
 */
void oled_display_show_balance(int balance);

/**
 * @brief Exibe tela de bloqueio com ícone de cadeado.
 */
void oled_display_show_locked(void);

/**
 * @brief Reinicializa a matriz do protetor de tela Game of Life com padrão aleatório.
 */
void oled_display_reset_life(void);

/**
 * @brief Exibe tela indicando que o cartão MicroSD está cedido ao host USB.
 */
void oled_display_show_usb_mode(void);

/**
 * @brief Exibe tela genérica de transferência de arquivos (USB ou Wi-Fi).
 *
 * @param[in] label       Rótulo ou cabeçalho da operação.
 * @param[in] filename    Nome do arquivo em transferência.
 * @param[in] percent     Progresso percentual do arquivo atual (0 a 100).
 * @param[in] eta_sec     Tempo estimado restante em segundos (< 0 mostra calculando).
 * @param[in] overall_pct Progresso do lote total (-1 se avulso).
 * @param[in] count       Total de arquivos do lote.
 */
void oled_display_show_transfer(const char *label, const char *filename, int percent, int eta_sec,
                                 int overall_pct, int count);

/**
 * @brief Exibe tela de espera de conexão Wi-Fi (modo ocioso).
 *
 * @param[in] ip         Endereço IP atribuído.
 * @param[in] mdns_host  Nome mDNS da máquina (ex: "mps3.local").
 * @param[in] dns_active true se o servidor DNS captive portal estiver ligado.
 * @param[in] rssi       Intensidade do sinal da rede Wi-Fi.
 */
void oled_display_show_wifi_idle(const char *ip, const char *mdns_host, bool dns_active, int rssi);

/**
 * @brief Exibe tela de Wi-Fi conectado com contagem de clientes e endereço web.
 *
 * @param[in] ip        Endereço IP ativo.
 * @param[in] mdns_host Nome de domínio local.
 * @param[in] rssi      Sinal em dBm.
 * @param[in] clients   Número de clientes simultâneos conectados.
 */
void oled_display_show_wifi_connected(const char *ip, const char *mdns_host, int rssi, int clients);

/**
 * @brief Exibe tela detalhada de transferência de arquivos via Wi-Fi.
 *
 * @param[in] filename    Nome do arquivo sendo transferido.
 * @param[in] file_pct    Progresso percentual do arquivo atual.
 * @param[in] overall_pct Progresso geral do lote.
 * @param[in] idx         Índice do arquivo atual.
 * @param[in] count       Total de arquivos no lote.
 * @param[in] kbps        Velocidade de transferência em KB/s.
 * @param[in] eta_sec     Tempo restante do arquivo atual.
 * @param[in] total_eta   Tempo estimado restante para o lote completo.
 * @param[in] rssi        Nível de sinal Wi-Fi.
 */
void oled_display_show_wifi_transfer(const char *filename, int file_pct, int overall_pct, int idx, int count, float kbps, int eta_sec, int total_eta, int rssi);

/**
 * @brief Exibe tela de confirmação de conclusão de transferência Wi-Fi.
 *
 * @param[in] files_received Quantidade de arquivos recebidos com sucesso.
 */
void oled_display_show_wifi_done(int files_received);

/**
 * @brief Renderiza a interface visual de ajuste do equalizador paramétrico.
 *
 * @param[in] gains          Vetor de ganhos em dB das 10 bandas + ganho geral.
 * @param[in] selected_band  Banda atualmente selecionada pelo cursor.
 * @param[in] eq_enabled     true se o equalizador estiver ativo; false se em bypass.
 */
void oled_display_show_eq(const float gains[11], int selected_band, bool eq_enabled);

/**
 * @brief Exibe a interface de controle do LED RGB WS2812 on-board.
 *
 * @param[in] r                Valor do canal Vermelho (0 a 255).
 * @param[in] g                Valor do canal Verde (0 a 255).
 * @param[in] b                Valor do canal Azul (0 a 255).
 * @param[in] selected_channel Canal em ajuste (0=R, 1=G, 2=B).
 * @param[in] enabled          Estado ligado/desligado do LED.
 */
void oled_display_show_led(uint8_t r, uint8_t g, uint8_t b, int selected_channel, bool enabled);

/**
 * @brief Ajusta o nível de brilho/contraste do display SSD1306.
 *
 * @param[in] level Contraste de 0 (mínimo legível) a 255 (brilho máximo).
 */
void oled_display_set_brightness(uint8_t level);

/**
 * @brief Liga ou desliga o modo de economia de energia do controlador SSD1306.
 *
 * @param[in] enable true para colocar o display em sleep (apagar matriz); false para religar.
 */
void oled_display_set_power_save(bool enable);

/**
 * @brief Exibe o menu de opções da tela (ajuste de brilho e temporizador de repouso).
 *
 * @param[in] cursor      Índice do parâmetro em foco.
 * @param[in] brightness  Nível de brilho configurado.
 * @param[in] timeout_idx Índice da opção de timeout de tela.
 */
void oled_display_show_tela(int cursor, uint8_t brightness, int timeout_idx);

/**
 * @brief Renderiza a tela superior de atalhos rápidos (Top Screen).
 *
 * Disposta verticalmente com Bateria no topo, Equalizador no meio e Volume na base.
 *
 * @param[in] volume          Volume percentual atual.
 * @param[in] eq_cfg          Ponteiro para a configuração do equalizador.
 * @param[in] eq_focus        Preset de equalização selecionado.
 * @param[in] voltage         Tensão da bateria em Volts.
 * @param[in] percentage      Porcentagem da bateria.
 * @param[in] time_left_mins  Estimativa de autonomia restante em minutos.
 * @param[in] cursor          Cursor vertical (0=Volume base, 1=EQ meio, 2=Bateria topo).
 */
void oled_display_show_top_screen(int volume, const void *eq_cfg, int eq_focus, float voltage, int percentage, int time_left_mins, int cursor);

/**
 * @brief Exibe o diálogo de seleção de perfil USB ao conectar o cabo de dados.
 *
 * @param[in] cursor Opção selecionada (0=Flash CDC, 1=DAC Áudio, 2=Armazenamento MSC, 3=Cancelar).
 */
void oled_display_show_usb_prompt(int cursor);

/**
 * @brief Exibe status do modo USB Mass Storage (Cartão MicroSD montado no PC).
 */
void oled_display_show_usb_msc(void);

/**
 * @brief Exibe interface visual completa do modo USB DAC UAC2 24-bit.
 *
 * Contém badges de resolução, indicador de streaming reativo e barra de volume.
 */
void oled_display_show_usb_dac(void);

/**
 * @brief Exibe tela de alerta de ausência ou falha de montagem do cartão MicroSD.
 */
void oled_display_show_sd_error(void);

/**
 * @brief Exibe indicador animado de leitura e indexação da biblioteca de músicas.
 */
void oled_display_show_loading(void);

/**
 * @brief Exibe a lista de seleção rápida de perfis predefinidos de EQ (Presets).
 *
 * @param[in] cursor        Índice selecionado pelo joystick.
 * @param[in] active_preset Índice do preset atualmente ativo.
 * @param[in] eq_enabled    Estado ligado/desligado do equalizador.
 * @param[in] eq_cfg        Ponteiro para a estrutura de presets.
 */
void oled_display_show_eq_preset_list(int cursor, int active_preset, bool eq_enabled, const void *eq_cfg);

/**
 * @brief Renderiza teclado virtual alfanumérico em tela para digitação de senhas.
 *
 * @param[in] text          Texto já digitado.
 * @param[in] cursor        Posição do cursor no texto.
 * @param[in] grid_x        Posição horizontal na grade de teclas.
 * @param[in] grid_y        Posição vertical na grade de teclas.
 * @param[in] page          Página de caracteres (maiúsculas, minúsculas, números/símbolos).
 * @param[in] is_confirming true se o botão de confirmação estiver em foco.
 */
void oled_display_show_keyboard(const char *text, int cursor, int grid_x, int grid_y, int page, bool is_confirming);

/**
 * @brief Exibe notificação de arquivo com formato ou compressão não suportada.
 *
 * @param[in] filename Nome do arquivo incompatível.
 */
void oled_display_show_unsupported(const char *filename);

/**
 * @brief Exibe tela de configuração do modo de ordenação de músicas (Nome A-Z vs Data mtime).
 *
 * @param[in] mode Modo selecionado (0=Nome, 1=Data).
 */
void oled_display_show_sort_mode(int mode);

/**
 * @brief Exibe menu de configuração do temporizador de suspensão profunda (Deep Sleep).
 *
 * @param[in] current_idx Opção selecionada (Desligado, 1 min, 5 min, 15 min, atalho [>]).
 */
void oled_display_show_deepsleep_cfg(int current_idx);

/**
 * @brief Renderiza barra de progresso proporcional durante a gravação de firmware OTA.
 *
 * @param[in] percent    Progresso de gravação (0 a 100%).
 * @param[in] status_msg Mensagem de status ou alerta de não desligar o aparelho.
 */
void oled_display_show_ota_progress(int percent, const char *status_msg);

/**
 * @brief Exibe tela de status de sincronização automática de episódios de podcasts.
 *
 * @param[in] program    Nome do canal ou podcast.
 * @param[in] title      Título do episódio sendo baixado.
 * @param[in] cur_idx    Índice do episódio atual no lote.
 * @param[in] total_idx  Quantidade total de episódios a sincronizar.
 * @param[in] percent    Progresso percentual do download.
 * @param[in] speed_kbs  Velocidade de transferência em KB/s.
 * @param[in] status_msg Mensagem contextual de status.
 */
void oled_display_show_podcast_sync(const char *program, const char *title,
                                   int cur_idx, int total_idx,
                                   int percent, float speed_kbs,
                                   const char *status_msg);

/**
 * @brief Renderiza matriz gráfica de QR Code para conexão direta ao Wi-Fi ou web manager.
 *
 * @param[in] ssid      Nome da rede Wi-Fi / SSID.
 * @param[in] pass      Senha da rede Wi-Fi (ou vazio para redes abertas).
 * @param[in] tag_label Rótulo exibido no cabeçalho da tela.
 * @param[in] cur_idx   Índice da rede exibida no carrossel.
 * @param[in] total_idx Total de redes disponíveis para alternância.
 */
void oled_display_show_wifi_qr(const char *ssid, const char *pass,
                               const char *tag_label, int cur_idx, int total_idx);

#ifdef __cplusplus
}
#endif

#endif // OLED_DISPLAY_H
