#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char filename[128];   // nome do arquivo no cartao (sempre disponivel)
    char title[128];      // titulo: da tag (TIT2/TITLE) se houver, senao = filename sem extensao
    char artist[64];      // "" se a faixa nao tiver tag de artista
    char album[64];       // "" se a faixa nao tiver tag de album
    char format_name[16]; // ex.: "Flac"
    uint32_t bits_per_sample;
    uint32_t sample_rate;
    uint32_t bitrate;
    char last_error[48];  // "" normalmente. Preenchido quando uma faixa
                           // falha ao carregar (formato nao suportado
                           // pela biblioteca instalada, arquivo
                           // corrompido, etc) - a tela mostra isso por
                           // alguns segundos em vez de pular direto pra
                           // proxima faixa em silencio.
    uint32_t elapsed_sec;
    uint32_t total_sec;
    bool duration_is_estimate; // true so' quando nem o Xing/VBRI nem a
                                // contagem exata do formato deram certo
                                // (caiu no fallback tamanho/bitrate)
    bool playing;       // true enquanto uma faixa esta sendo decodificada/tocada
    bool paused;        // true enquanto o play/pause estiver pausado
    bool track_loaded;  // true depois que o cabecalho da faixa atual foi lido
    bool no_files_found; // true se nao foi encontrado nenhum arquivo suportado no SD
    
    // Web Radio
    bool is_web_radio;
    char web_radio_url[256];
} playback_state_t;

// Varre a raiz do SD e cria a task que toca automaticamente. Se houver
// uma faixa/posicao salva de uma sessao anterior (NVS), a pasta dela e'
// aberta e a reproducao comeca de la' (ver
// audio_player_should_start_in_playing_mode()).
esp_err_t audio_player_start(const char *music_dir);

// Copia o estado atual de reproducao (thread-safe) para *out_state.
void audio_player_get_state(playback_state_t *out_state);

// true se havia uma faixa/posicao salva de uma sessao anterior valida no
// momento do boot - a UI deve chamar isso UMA VEZ, logo apos
// audio_player_start(), pra decidir se abre direto na tela de reproducao
// (em vez do menu) e resumir de onde parou.
bool audio_player_should_start_in_playing_mode(void);

// Alterna play/pause. Pausar desliga fisicamente o I2S (silencio real,
// nao so' "parar de escrever amostras").
void audio_player_toggle_play_pause(void);

// Volume (0-100). Aplicado com curva logaritmica (percepcao humana de
int  audio_player_get_volume(void);
void audio_player_adjust_volume(int delta);
void audio_player_set_volume(int volume);

// Callback invocado quando o volume e' alterado pelo usuario
typedef void (*audio_volume_change_cb_t)(int volume);
void audio_player_set_volume_change_cb(audio_volume_change_cb_t cb);

// Balanço estéreo L/R (-100 a +100). 0 = Centro (ambos a 100%),
// < 0 = atenua canal direito (R), > 0 = atenua canal esquerdo (L).
int  audio_player_get_balance(void);
void audio_player_set_balance(int balance);
void audio_player_adjust_balance(int delta);

// --- Web Radio -----------------------------------------------------------
esp_err_t audio_player_play_url(const char *url);
void audio_player_stop_url(void);

// --- Navegacao de reproducao (comandada pelo joystick ou interface) -------
// Todas essas funcoes apenas registram um pedido; a troca de faixa
// acontece no proximo ponto de checagem do player_task (poucos ms).
// "next/previous/restart" operam dentro da PASTA de onde a faixa atual
// foi tocada (ver audio_player_select_entry() abaixo) - navegar o menu
// pra outra pasta em segundo plano nao muda a playlist da faixa tocando.
void audio_player_next(void);
void audio_player_previous(void);
void audio_player_restart(void);

// Avanca `seconds` na faixa atual. Seek de verdade: reabre o arquivo e
// pula direto (fseek) pro offset de byte estimado pela taxa media de
// bits (meta.avg_byte_rate, calculada em metadata_extract.c) - O(1),
// nao decodifica nem descarta nada no meio do caminho. Funciona pra
// FLAC/MP3/WAV/AAC (M4A ainda nao calcula avg_byte_rate - nesse caso
// cai pro fallback de decodificar desde o inicio, automaticamente).
// Chamadas repetidas (ex: segurando o botao) acumulam o alvo e so' o
// ultimo pedido efetivamente executa - pedidos anteriores em andamento
// sao substituidos antes de completar, sem trabalho extra.
void audio_player_seek_forward(uint32_t seconds);

// Volta `seconds` na faixa atual. Mesmo mecanismo de
// audio_player_seek_forward (fseek O(1) pro offset estimado), so' que
// subtraindo em vez de somar. Se o resultado for <= 0, volta pro
// comeco da faixa.
void audio_player_seek_backward(uint32_t seconds);

// Lista de arquivos de audio da pasta ONDE A FAIXA ATUAL ESTA' TOCANDO
// (playlist ativa - usada pro proprio player e pra tela de "proximas
// faixas"). Independente da pasta que o MENU esta mostrando no momento
// (ver secao de navegacao abaixo).
int audio_player_get_file_count(void);
void audio_player_get_file_name(int index, char *out, size_t out_len); // out[0]='\0' se index invalido
int audio_player_get_current_index(void);          // indice da faixa atual na playlist ativa

// --- Navegacao de pastas (usada pelo menu) --------------------------------
// A lista "de navegacao" (o que o MENU mostra) e' independente da playlist
// ativa acima - pode ser percorrida livremente enquanto uma musica toca em
// segundo plano, sem afetar o "next/previous" dela. So' quando o usuario
// efetivamente SELECIONA um arquivo de audio (audio_player_select_entry)
// e' que a playlist ativa muda pra' pasta correspondente.
//
// A lista mistura, nessa ordem: ".." (se nao estiver na raiz), subpastas,
// arquivos de audio - todos ja' ordenados alfabeticamente dentro de cada
// grupo.
bool audio_player_browse_is_root(void); // true se a navegacao esta' na raiz do cartao
int audio_player_get_browse_entry_count(void);
void audio_player_get_browse_entry_name(int index, char *out, size_t out_len); // ja' formatado pra exibir (pastas com "/" no fim, ".." pra voltar)
bool audio_player_browse_entry_is_dir(int index); // true pra pastas e ".."; false pra arquivos de audio

// Aplica a selecao do item `index` da lista de navegacao:
//  - ".." ou pasta -> o menu passa a mostrar aquela pasta (nao afeta o
//    que esta' tocando).
//  - arquivo de audio -> comeca a tocar ele (playlist ativa passa a ser
//    a pasta onde ele esta').
void audio_player_select_entry(int index);

// --- Hooks para o modo de armazenamento USB (ver usb_storage.h) ---------
// Pausa a reproducao e fecha todos os arquivos abertos no cartao SD
// (faixa atual + prefetch) de forma limpa, deixando o cartao livre pra'
// ser desmontado e entregue ao driver MSC. Chamado quando um host USB se
// conecta.
void audio_player_release_sd_for_usb(void);
void audio_player_reacquire_sd_after_usb(void);

// Suspende e retoma a task do player para operacao de modos exclusivos (USB MSC e DAC)
void audio_player_suspend(void);
void audio_player_resume(void);

// --- Equalizador (10 bandas + ganho geral, persistido na NVS) -------------
// Estrutura de configuracao do EQ (espelha eq_config_t de eq.h para que
// touch_input.c e oled_display.c nao precisem incluir eq.h diretamente).
#define PLAYER_EQ_BANDS 10

#define PLAYER_EQ_MAX_PRESETS 10
#define PLAYER_EQ_PRESET_NAME_LEN 16

typedef struct {
    char name[PLAYER_EQ_PRESET_NAME_LEN];
    float band_gains[PLAYER_EQ_BANDS];
    float overall_gain;
} player_eq_preset_t;

typedef struct {
    bool enabled;
    int active_preset_idx;
    player_eq_preset_t presets[PLAYER_EQ_MAX_PRESETS];
} player_eq_config_t;

// Le' a configuracao atual do equalizador.
void audio_player_get_eq_config(player_eq_config_t *out);

// Aplica e persiste na NVS uma nova configuracao do equalizador.
void audio_player_set_eq_config(const player_eq_config_t *config);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_PLAYER_H
