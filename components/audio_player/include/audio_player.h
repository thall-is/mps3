#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

/**
 * @file audio_player.h
 * @brief Subsistema Central de Reprodução de Áudio Digital (Player Core & Pipeline)
 *
 * Fornece a interface de controle do reprodutor de áudio digital Hi-Res (DAP).
 * Gerencia a decodificação multiformato (FLAC 24b/192k, MP3, WAV, AAC, M4A, OGG),
 * navegação de faixas e pastas no cartão MicroSD, controle de volume logarítmico,
 * balanço estéreo L/R, equalizador de 10 bandas e integração com rádio web.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Estrutura que representa o estado completo de reprodução em tempo real.
 */
typedef struct {
    char filename[128];         /**< Nome do arquivo no cartão MicroSD. */
    char title[128];            /**< Título da faixa (extraído de tags ID3/Vorbis ou nome sem extensão). */
    char artist[64];            /**< Nome do artista ou string vazia se ausente. */
    char album[64];             /**< Nome do álbum ou string vazia se ausente. */
    char format_name[16];       /**< Nome legível do formato (ex.: "FLAC", "MP3", "WAV"). */
    uint32_t bits_per_sample;   /**< Resolução de bits por amostra (ex.: 16, 24 ou 32 bits). */
    uint32_t sample_rate;       /**< Taxa de amostragem em Hz (ex.: 44100, 48000, 96000, 192000). */
    uint32_t bitrate;           /**< Taxa de bits média calculada em bps. */
    char last_error[48];        /**< Descrição de erro para exibição na UI quando a faixa falha. */
    uint32_t elapsed_sec;       /**< Tempo decorrido de reprodução da faixa atual em segundos. */
    uint32_t total_sec;         /**< Duração total da faixa em segundos. */
    bool duration_is_estimate;  /**< true se a duração foi estimada por tamanho/bitrate (sem cabeçalho exato). */
    bool playing;               /**< true enquanto uma faixa está sendo ativamente decodificada e tocada. */
    bool paused;                /**< true se a reprodução está atualmente pausada. */
    bool track_loaded;          /**< true após o cabeçalho e metadados da faixa terem sido processados. */
    bool no_files_found;        /**< true se nenhum arquivo de áudio compatível foi localizado no cartão SD. */
    
    // Web Radio
    bool is_web_radio;          /**< true se o fluxo atual provém de uma estação de Web Rádio (HTTP). */
    char web_radio_url[256];    /**< URL da estação de rádio web atualmente conectada. */

    // Modos de Reproducao (Loop e Shuffle)
    uint8_t loop_mode;          /**< Modo de repeticao: 0=NONE, 1=ALL, 2=TRACK (audio_player_loop_mode_t). */
    uint8_t shuffle_mode;       /**< Modo aleatorio: 0=OFF, 1=FOLDER, 2=ALL (audio_player_shuffle_mode_t). */
    uint8_t loop_track_repeat_count; /**< Repeticoes restantes da faixa: 1=primeira execucao (icone com 1), 0=repetindo (icone simples sem 1). */
    bool is_favorite;           /**< true se a faixa atual consta na playlist Favoritas / ativa. */
} playback_state_t;

/**
 * @brief Modos de Repetição de Áudio (Loop).
 */
typedef enum {
    AUDIO_LOOP_NONE  = 0,  /**< Linear: para ao término da pasta ou playlist. */
    AUDIO_LOOP_ALL   = 1,  /**< Repete Sempre: reinicia do início em ciclo infinito. */
    AUDIO_LOOP_TRACK = 2   /**< Repete 1: repete a mesma faixa continuamente ao término. */
} audio_player_loop_mode_t;

/**
 * @brief Modos de Reprodução Aleatória (Shuffle).
 */
typedef enum {
    AUDIO_SHUFFLE_OFF    = 0, /**< Desativado: reproduz na ordem alfabética ou tags. */
    AUDIO_SHUFFLE_FOLDER = 1, /**< Aleatório na Pasta/Playlist atual via permutação Fisher-Yates em PSRAM. */
    AUDIO_SHUFFLE_ALL    = 2  /**< Aleatório Global: sorteia faixas por todas as pastas do MicroSD. */
} audio_player_shuffle_mode_t;

/**
 * @brief Inicializa o subsistema de áudio e cria a tarefa de reprodução.
 *
 * Varre o diretório inicial especificado no cartão MicroSD (ou raiz) e inicia a `player_task`
 * fixada no Core 1 com prioridade 5. Se houver uma faixa salva de sessão anterior na NVS,
 * a pasta correspondente é carregada e a reprodução é retomada automaticamente do ponto salvo.
 *
 * @param[in] music_dir Caminho base no sistema de arquivos FatFS (ex: "/sdcard" ou "/sdcard/Musicas").
 *
 * @return 
 *   - ESP_OK: Subsistema inicializado e tarefa criada com sucesso.
 *   - ESP_ERR_INVALID_ARG: Diretório fornecido é inválido ou nulo.
 *   - ESP_FAIL: Falha ao inicializar filas FreeRTOS ou criar a tarefa.
 *
 * @note Deve ser chamada após a inicialização bem-sucedida do driver SDMMC (`sd_card_init`).
 */
esp_err_t audio_player_start(const char *music_dir);

/**
 * @brief Obtém uma cópia atômica e segura do estado atual de reprodução.
 *
 * Realiza uma cópia protegida por mutex (`s_state_mutex`) para evitar condições de corrida
 * com a tarefa de decodificação (`player_task`) ou de saída (`audio_dsp_task`).
 *
 * @param[out] out_state Ponteiro para a estrutura `playback_state_t` onde os dados serão copiados.
 *
 * @note Thread-safe. Pode ser chamada concorrentemente a partir de qualquer núcleo ou tarefa.
 */
void audio_player_get_state(playback_state_t *out_state);

/**
 * @brief Verifica se o player deve iniciar diretamente na tela Now Playing no boot.
 *
 * Consulta se havia uma faixa e posição válidas salvas na memória NVS no momento da inicialização.
 * A interface gráfica (UI) consulta esta função no boot para decidir se exibe o menu principal
 * ou se abre direto na tela de reprodução.
 *
 * @return true se havia sessão anterior salva válida; false caso contrário.
 */
bool audio_player_should_start_in_playing_mode(void);

/**
 * @brief Alterna entre os estados de reprodução ativa e pausa (Play / Pause).
 *
 * Ao pausar: sinaliza a suspensão para o pipeline e desativa fisicamente o canal I2S
 * (`i2s_output_disable()`), garantindo silêncio analógico real no DAC PCM5102A.
 * Ao despausar: reabilita o canal I2S (`i2s_output_enable()`) e libera o envio de novos blocos PCM.
 *
 * @note Thread-safe. Protegido contra inversão de prioridades e deadlocks de I2S DMA.
 */
void audio_player_toggle_play_pause(void);

/**
 * @brief Obtém o nível atual de volume do sistema.
 *
 * @return Nível de volume em porcentagem (0 a 100).
 */
int audio_player_get_volume(void);

/**
 * @brief Ajusta o volume do sistema através de um incremento relativo.
 *
 * Aplica um delta positivo ou negativo, limitando o resultado entre 0% e 100%.
 * A curva de volume é mapeada logaritmicamente para compensação da percepção auditiva humana.
 *
 * @param[in] delta Valor de ajuste (ex.: +5 para aumentar 5%, -5 para diminuir 5%).
 */
void audio_player_adjust_volume(int delta);

/**
 * @brief Define o nível absoluto de volume do sistema e persiste na NVS.
 *
 * Converte o valor percentual em ganho digital logarítmico e persiste a alteração na NVS.
 *
 * @param[in] volume Valor absoluto de volume entre 0 (mudo completo) e 100 (ganho unitário 0 dB).
 */
void audio_player_set_volume(int volume);

/**
 * @brief Tipo de função de callback para notificações de alteração de volume.
 *
 * @param[in] volume Novo volume percentual (0 a 100).
 */
typedef void (*audio_volume_change_cb_t)(int volume);

/**
 * @brief Registra um callback para notificar mudanças de volume para a UI ou periféricos USB.
 *
 * @param[in] cb Ponteiro para a função de callback a ser chamada quando o volume mudar.
 */
void audio_player_set_volume_change_cb(audio_volume_change_cb_t cb);

/**
 * @brief Tipo de função de callback para notificações de conclusão de faixa (>= 90% ou EOF).
 *
 * @param[in] rel_path    Caminho relativo ao ponto de montagem SD (ex: "Podcasts/Show/ep.mp3").
 * @param[in] elapsed_sec Segundos reproduzidos da faixa.
 * @param[in] total_sec   Duração total estimada/real da faixa em segundos.
 */
typedef void (*audio_track_completed_cb_t)(const char *rel_path, uint32_t elapsed_sec, uint32_t total_sec);

/**
 * @brief Registra um callback invocado quando uma faixa atinge >= 90% de reprodução ou chega ao fim.
 *
 * @param[in] cb Ponteiro para a função de callback.
 */
void audio_player_register_track_completed_cb(audio_track_completed_cb_t cb);

/**
 * @brief Obtém o valor do balanço estéreo atual.
 *
 * @return Valor do balanço de -100 (canal esquerdo isolado) a +100 (canal direito isolado).
 *         0 indica centro perfeito (ambos os canais com ganho total).
 */
int audio_player_get_balance(void);

/**
 * @brief Define o valor absoluto do balanço estéreo.
 *
 * @param[in] balance Valor entre -100 (atenua R) e +100 (atenua L). 0 = Centro.
 */
void audio_player_set_balance(int balance);

/**
 * @brief Ajusta o balanço estéreo através de um incremento relativo.
 *
 * @param[in] delta Variação a aplicar (limitada entre -100 e +100).
 */
void audio_player_adjust_balance(int delta);

/**
 * @brief Inicia a reprodução de um fluxo de áudio de Web Rádio via HTTP.
 *
 * Conecta-se ao servidor de streaming, negocia os cabeçalhos HTTP/ICY e direciona
 * os pacotes de áudio recebidos para o decodificador.
 *
 * @param[in] url URL completa do stream (ex: "http://stream.exemplo.com:8000/live.mp3").
 *
 * @return 
 *   - ESP_OK: Conexão iniciada com sucesso.
 *   - ESP_ERR_INVALID_ARG: URL nula ou formato inválido.
 *   - ESP_ERR_NO_MEM: Memória insuficiente para buffers de rede.
 */
esp_err_t audio_player_play_url(const char *url);

/**
 * @brief Interrompe a reprodução de Web Rádio e desconecta o socket HTTP.
 */
void audio_player_stop_url(void);

/**
 * @brief Solicita a reprodução da próxima faixa da pasta ativa.
 *
 * O comando é assíncrono e registrado atomicamente; a transição ocorre no próximo
 * ciclo de checagem do decodificador (`player_task`).
 */
void audio_player_next(void);

/**
 * @brief Solicita a reprodução da faixa anterior da pasta ativa.
 *
 * Se a música atual tiver mais de 3 segundos decorridos, reinicia a mesma faixa;
 * caso contrário, volta para a faixa anterior na lista.
 */
void audio_player_previous(void);

/**
 * @brief Reinicia a reprodução da faixa atual a partir do início (00:00).
 */
void audio_player_restart(void);

/**
 * @brief Avança a reprodução em um número determinado de segundos (Seek Forward).
 *
 * Utiliza busca de alta velocidade em O(1): reposiciona o ponteiro de arquivo (`fseek`)
 * diretamente no offset estimado pela taxa média de bits (`avg_byte_rate`) e sincroniza
 * na próxima palavra de alinhamento de frame (*syncword*), sem decodificação intermediária.
 *
 * @param[in] seconds Quantidade de segundos a avançar a partir do ponto atual.
 */
void audio_player_seek_forward(uint32_t seconds);

/**
 * @brief Retrocede a reprodução em um número determinado de segundos (Seek Backward).
 *
 * Utiliza o mesmo mecanismo otimizado de fseek direto do seek forward, subtraindo o tempo.
 * Se o alvo for menor ou igual a zero, reinicia do início da faixa.
 *
 * @param[in] seconds Quantidade de segundos a retroceder.
 */
void audio_player_seek_backward(uint32_t seconds);

/**
 * @brief Informa se uma operação de salto (seek) está pendente de execução.
 *
 * @return true se há um salto sendo processado pelo decodificador; false caso contrário.
 */
bool audio_player_is_seeking(void);

/**
 * @brief Retorna a quantidade de faixas de áudio na pasta ativa (playlist atual).
 *
 * @return Número total de arquivos de áudio compatíveis na pasta da música em reprodução.
 */
int audio_player_get_file_count(void);

/**
 * @brief Obtém o nome de arquivo de uma faixa da pasta ativa pelo seu índice.
 *
 * @param[in]  index   Índice da faixa na pasta ativa (0 a count-1).
 * @param[out] out     Buffer de destino para o nome do arquivo.
 * @param[in]  out_len Capacidade máxima em bytes do buffer de saída.
 */
void audio_player_get_file_name(int index, char *out, size_t out_len);

/**
 * @brief Retorna o índice da faixa atualmente em reprodução dentro da pasta ativa.
 *
 * @return Índice base zero da música tocando no momento.
 */
int audio_player_get_current_index(void);

/**
 * @brief Verifica se a navegação de pastas do menu está no diretório raiz do cartão SD.
 *
 * @return true se o navegador está em "/sdcard"; false se está dentro de uma subpasta.
 */
bool audio_player_browse_is_root(void);

/**
 * @brief Verifica se a pasta atualmente navegada no menu e a mesma que esta tocando.
 *
 * @return true se s_browse_dir for identico a s_playback_dir; false caso contrario.
 */
bool audio_player_browse_is_playback_dir(void);

/**
 * @brief Retorna o indice da faixa que esta tocando no navegador de pastas atual.
 *
 * @return Indice relativo a lista visivel (excluindo '..'), ou -1 se nao estiver na pasta de reproducao.
 */
int audio_player_get_current_track_browse_index(void);

/**
 * @brief Retorna o número de entradas (pastas e arquivos) na pasta navegada no menu.
 *
 * @note A pasta navegada no menu é independente da pasta que está tocando em segundo plano.
 *
 * @return Quantidade de itens visíveis na lista de navegação.
 */
int audio_player_get_browse_entry_count(void);

/**
 * @brief Obtém o nome formatado de uma entrada de navegação pelo índice.
 *
 * Subpastas recebem automaticamente uma barra "/" no final (ex: "Rock/"),
 * e se não estiver na raiz, a primeira entrada é ".." para voltar.
 *
 * @param[in]  index   Índice do item na lista de navegação.
 * @param[out] out     Buffer de destino onde a string será escrita.
 * @param[in]  out_len Tamanho máximo do buffer de saída em bytes.
 */
void audio_player_get_browse_entry_name(int index, char *out, size_t out_len);

/**
 * @brief Informa se o item da lista de navegação é um diretório.
 *
 * @param[in] index Índice da entrada na lista.
 *
 * @return true se for uma pasta ou entrada de retorno ".."; false se for arquivo de áudio.
 */
bool audio_player_browse_entry_is_dir(int index);

/**
 * @brief Executa a ação correspondente ao item selecionado no navegador de arquivos.
 *
 * Se for diretório: entra na pasta e atualiza a lista de visualização (a música continua tocando).
 * Se for arquivo de áudio: define a pasta como a nova playlist ativa e inicia a reprodução da faixa.
 *
 * @param[in] index Índice do item selecionado na lista de navegação.
 */
void audio_player_select_entry(int index);

/**
 * @brief Pausa a reprodução e fecha os arquivos abertos para permitir uso do USB Mass Storage.
 *
 * Libera descritores de arquivo e garante que o cartão MicroSD possa ser desmontado
 * com segurança pelo subsistema TinyUSB MSC sem corrupção da tabela FAT.
 */
void audio_player_release_sd_for_usb(void);

/**
 * @brief Remonta o cartão MicroSD e restaura o leitor após o encerramento do modo USB MSC.
 */
void audio_player_reacquire_sd_after_usb(void);

/**
 * @brief Suspende temporariamente a tarefa do player para modos exclusivos (USB DAC / MSC).
 */
void audio_player_suspend(void);

/**
 * @brief Retoma a execução normal da tarefa de reprodução do player.
 */
void audio_player_resume(void);

#define PLAYER_EQ_BANDS 10              /**< Quantidade de bandas do equalizador (10 bandas biquad). */
#define PLAYER_EQ_MAX_PRESETS 10        /**< Número máximo de perfis predefinidos de EQ. */
#define PLAYER_EQ_PRESET_NAME_LEN 16    /**< Tamanho máximo do nome de cada preset em caracteres. */

/**
 * @brief Estrutura de perfil de equalização (Preset).
 */
typedef struct {
    char name[PLAYER_EQ_PRESET_NAME_LEN]; /**< Nome do preset (ex: "Rock", "Bass", "Vocal"). */
    float band_gains[PLAYER_EQ_BANDS];    /**< Ganhos individuais de cada banda em dB (-12 a +12 dB). */
    float overall_gain;                   /**< Ganho pré-amplificador geral (Pre-cut) em dB. */
} player_eq_preset_t;

/**
 * @brief Estrutura de configuração geral do equalizador persistida na NVS.
 */
typedef struct {
    bool enabled;                                         /**< Estado do equalizador (ativo ou bypass). */
    int active_preset_idx;                                /**< Índice do preset atualmente selecionado. */
    player_eq_preset_t presets[PLAYER_EQ_MAX_PRESETS];    /**< Matriz de presets disponíveis. */
} player_eq_config_t;

/**
 * @brief Lê a configuração atual do equalizador de 10 bandas.
 *
 * @param[out] out Ponteiro para a estrutura onde a configuração atual será copiada.
 */
void audio_player_get_eq_config(player_eq_config_t *out);

/**
 * @brief Aplica uma nova configuração ao equalizador e persiste na memória NVS.
 *
 * @param[in] config Ponteiro para a nova configuração a ser aplicada imediatamente.
 */
void audio_player_set_eq_config(const player_eq_config_t *config);

/**
 * @brief Modos de ordenação da lista de faixas de áudio.
 */
typedef enum {
    SORT_MODE_NAME = 0, /**< Ordenação alfabética clássica por nome de arquivo (A-Z). */
    SORT_MODE_DATE = 1, /**< Ordenação cronológica por data de modificação (`mtime`), preservando a tracklist original. */
} track_sort_mode_t;

/**
 * @brief Define o modo de ordenação das faixas e persiste na NVS.
 *
 * @param[in] mode Modo de ordenação desejado (`SORT_MODE_NAME` ou `SORT_MODE_DATE`).
 */
void audio_player_set_sort_mode(track_sort_mode_t mode);

/**
 * @brief Retorna o modo de ordenação de faixas atualmente ativo.
 *
 * @return Modo ativo (`SORT_MODE_NAME` ou `SORT_MODE_DATE`).
 */
track_sort_mode_t audio_player_get_sort_mode(void);

/**
 * @brief Modos de operação do ReplayGain automático.
 */
typedef enum {
    RG_MODE_OFF   = 0, /**< ReplayGain desativado (0.0 dB). */
    RG_MODE_TRACK = 1, /**< Ganho por faixa (fallback para álbum). */
    RG_MODE_ALBUM = 2  /**< Ganho por álbum (fallback para faixa). */
} replaygain_mode_t;

/**
 * @brief Modos de persistência de múltiplos bookmarks (Podcasts / Audiolivros).
 */
typedef enum {
    BOOKMARK_MODE_OFF  = 0, /**< Bookmarks individuais desativados. */
    BOOKMARK_MODE_AUTO = 1, /**< Automático (/Podcasts, /Audiobooks, .m4b ou >= 15 min). */
    BOOKMARK_MODE_ALL  = 2  /**< Salva posição individual para todas as faixas. */
} bookmark_mode_t;

/**
 * @brief Ativa ou desativa a reprodução Gapless Real e persiste na NVS.
 */
void audio_player_set_gapless_enabled(bool enabled);

/**
 * @brief Retorna se a reprodução Gapless Real está ativa.
 */
bool audio_player_get_gapless_enabled(void);

/**
 * @brief Ativa ou desativa o Fade-In/Fade-Out suave anti-click (10-15 ms) e persiste na NVS.
 */
void audio_player_set_soft_fade_enabled(bool enabled);

/**
 * @brief Retorna se o Fade-In/Fade-Out suave está ativo.
 */
bool audio_player_get_soft_fade_enabled(void);

/**
 * @brief Define o modo de ReplayGain automático e persiste na NVS.
 */
void audio_player_set_replaygain_mode(replaygain_mode_t mode);

/**
 * @brief Retorna o modo de ReplayGain atualmente ativo.
 */
replaygain_mode_t audio_player_get_replaygain_mode(void);

/**
 * @brief Define o modo de múltiplos Bookmarks e persiste na NVS.
 */
void audio_player_set_bookmark_mode(bookmark_mode_t mode);

/**
 * @brief Retorna o modo de múltiplos Bookmarks atualmente ativo.
 */
bookmark_mode_t audio_player_get_bookmark_mode(void);

/**
 * @brief Ativa ou desativa o Salto Alfabético Rápido (A-Z Jump) e persiste na NVS.
 */
void audio_player_set_az_jump_enabled(bool enabled);

/**
 * @brief Retorna se o Salto Alfabético Rápido (A-Z Jump) está ativo.
 */
bool audio_player_get_az_jump_enabled(void);

/**
 * @brief Obtém o modo de repetição (loop) configurado no momento.
 * @return Modo ativo (AUDIO_LOOP_NONE, AUDIO_LOOP_ALL, AUDIO_LOOP_TRACK).
 */
audio_player_loop_mode_t audio_player_get_loop_mode(void);

/**
 * @brief Define o modo de repetição (loop) e persiste na memória NVS.
 * @param[in] mode Novo modo desejado.
 */
void audio_player_set_loop_mode(audio_player_loop_mode_t mode);

/**
 * @brief Alterna ciclicamente o modo de repetição (NONE -> ALL -> TRACK -> NONE).
 * @return Novo modo ativo após a transição.
 */
audio_player_loop_mode_t audio_player_cycle_loop_mode(void);

/**
 * @brief Alterna no sentido inverso o modo de repeticao (NONE -> TRACK -> ALL -> NONE).
 * @return Novo modo ativo apos a transicao.
 */
audio_player_loop_mode_t audio_player_prev_loop_mode(void);

/**
 * @brief Obtém o modo de reprodução aleatória (shuffle) configurado no momento.
 * @return Modo ativo (AUDIO_SHUFFLE_OFF, AUDIO_SHUFFLE_FOLDER, AUDIO_SHUFFLE_ALL).
 */
audio_player_shuffle_mode_t audio_player_get_shuffle_mode(void);

/**
 * @brief Define o modo de reprodução aleatória (shuffle) e persiste na memória NVS.
 * @param[in] mode Novo modo desejado.
 */
void audio_player_set_shuffle_mode(audio_player_shuffle_mode_t mode);

/**
 * @brief Alterna ciclicamente o modo aleatório (OFF -> FOLDER -> ALL -> OFF).
 * @return Novo modo ativo após a transição.
 */
audio_player_shuffle_mode_t audio_player_cycle_shuffle_mode(void);

/**
 * @brief Alterna no sentido inverso o modo aleatorio (OFF -> ALL -> FOLDER -> OFF).
 * @return Novo modo ativo apos a transicao.
 */
audio_player_shuffle_mode_t audio_player_prev_shuffle_mode(void);

/**
 * @brief Salta para a próxima ou anterior letra inicial na lista de navegação atual.
 *
 * @param[in]  cur_cursor Índice real atual na lista (incluindo ".." se houver).
 * @param[in]  dir_step   Direção do salto (+1 para próxima letra, -1 para letra anterior).
 * @param[out] out_letter Letra inicial de destino alcançada (ex: 'A'..'Z' ou '#').
 * @return Novo índice real do cursor na lista de navegação.
 */
int audio_player_jump_browse_letter(int cur_cursor, int dir_step, char *out_letter);

/**
 * @brief Adiciona ou remove a faixa atual da playlist on-the-go /sdcard/Favoritos.m3u8.
 *
 * @param[out] out_added Recebe true se a faixa foi adicionada, false se foi removida.
 * @return true se a operação no cartão SD foi bem-sucedida.
 */
bool audio_player_toggle_favorite_current(bool *out_added);

/**
 * @brief Retorna a quantidade total de playlists (.m3u e .m3u8) disponíveis em /sdcard/Playlists.
 */
int audio_player_get_playlists_count(void);

/**
 * @brief Obtém o nome e status da playlist pelo índice (índice 0 é sempre Favoritos, padrão estilo Spotify).
 *
 * @param[in]  index         Índice na lista (0 .. count-1).
 * @param[out] out_name      Buffer para receber o nome da playlist.
 * @param[in]  out_len       Tamanho do buffer.
 * @param[out] out_is_active Recebe true se esta for a playlist alvo selecionada no momento.
 * @return true se obteve os dados com sucesso.
 */
bool audio_player_get_playlist_by_index(int index, char *out_name, size_t out_len, bool *out_is_active);

/**
 * @brief Define a playlist alvo ativa através do seu índice na lista de /sdcard/Playlists.
 *
 * @param[in] index Índice selecionado.
 * @return true se selecionada com sucesso.
 */
bool audio_player_set_target_playlist_by_index(int index);

/**
 * @brief Navega diretamente para a pasta /sdcard/Playlists no explorador de arquivos.
 */
void audio_player_browse_to_playlists(void);

/**
 * @brief Garante que o diretório /sdcard/Playlists exista e migra arquivos .m3u/.m3u8 legados da raiz.
 */
void audio_player_ensure_playlists_migrated(void);

/**
 * @brief Retorna o nome do arquivo da playlist alvo ativa (ex: "Favoritos.m3u8").
 */
const char *audio_player_get_target_playlist(void);

/**
 * @brief Alterna a playlist alvo ativa entre os arquivos .m3u e .m3u8 existentes na raiz do SD.
 *
 * @param[in] delta Direção (+1 para próxima, -1 para anterior).
 */
void audio_player_cycle_target_playlist(int delta);

/**
 * @brief Cria um novo arquivo de playlist (.m3u8 ou .m3u) em /sdcard e o define como playlist alvo ativa.
 *
 * @param[in]  name         Nome digitado pelo usuário (se não tiver extensão .m3u/.m3u8, anexa .m3u8).
 * @param[out] out_filename Buffer opcional para receber o nome final criado.
 * @param[in]  out_len      Tamanho do buffer out_filename.
 * @return true se o arquivo foi criado com sucesso.
 */
bool audio_player_create_playlist(const char *name, char *out_filename, size_t out_len);

/**
 * @brief Exclui permanentemente um arquivo de playlist (.m3u/.m3u8) em /sdcard/Playlists.
 *
 * Se a playlist apagada for a playlist alvo atual, o sistema automaticamente
 * redefine a lista alvo para "Favoritos.m3u8" e atualiza a NVS.
 * Arquivos da lista padrão de Favoritos são protegidos contra exclusão.
 *
 * @param[in] name_or_filename Nome ou caminho do arquivo da playlist a ser apagada.
 * @return true se a playlist foi excluída com sucesso, false em caso de erro ou se protegida.
 */
bool audio_player_delete_playlist(const char *name_or_filename);

/**
 * @brief Adiciona todas as faixas da pasta atual à playlist alvo ativa.
 *
 * @param[out] out_added_count Recebe a quantidade de faixas inéditas adicionadas.
 * @return true se a operação foi concluída com sucesso.
 */
bool audio_player_add_current_folder_to_target_playlist(int *out_added_count);

/**
 * @brief Adiciona/remove o item selecionado no navegador (faixa ou pasta inteira) na playlist alvo ativa.
 *
 * @param[in]  browse_idx Índice real do item em s_browse_scan (incluindo offset de ".." se aplicável).
 * @param[out] out_added  Recebe true se adicionado, false se removido.
 * @param[out] out_count  Quantidade de faixas afetadas.
 * @return true se a operação no SD foi bem-sucedida.
 */
bool audio_player_toggle_browse_item_in_target_playlist(int browse_idx, bool *out_added, int *out_count);

/**
 * @brief Adiciona (like=true) ou remove (like=false) a faixa atual à playlist alvo ativa.
 */
bool audio_player_like_current(bool like, bool *out_status);

/**
 * @brief Adiciona (like=true) ou remove (like=false) o item focado do navegador à playlist alvo ativa.
 */
bool audio_player_like_browse_item(int browse_idx, bool like, bool *out_status);

/**
 * @brief Retorna o diretório atualmente em foco no navegador de arquivos.
 */
void audio_player_get_browse_dir(char *out, size_t out_len);

/**
 * @brief Re-escaneia o diretório atualmente aberto no navegador de arquivos.
 */
void audio_player_rescan_browser(void);

/**
 * @brief Retorna o navegador de arquivos imediatamente para a raiz (/sdcard).
 */
void audio_player_browse_to_root(void);

/**
 * @brief Retorna true se a faixa atual em reprodução já consta na playlist alvo ativa.
 */
bool audio_player_is_current_favorite(void);

/**
 * @brief Retorna true se o item do navegador em browse_idx já consta na playlist alvo ativa.
 */
bool audio_player_is_browse_item_favorite(int browse_idx);

/**
 * @brief Limpa o registro da última faixa reproduzida na NVS para recuperação de boot ou falha.
 */
void audio_player_clear_resume_state(void);

/**
 * @brief Define ou limpa mensagem de erro da reprodução para exibição na interface visual.
 *
 * @param[in] filename  Nome do arquivo com erro (ou NULL).
 * @param[in] error_msg Mensagem de erro descritiva (ou NULL para limpar).
 */
void audio_player_set_last_error(const char *filename, const char *error_msg);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_PLAYER_H
