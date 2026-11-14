#ifndef METADATA_EXTRACT_H
#define METADATA_EXTRACT_H

/**
 * @file metadata_extract.h
 * @brief Extrator de Metadados e Tags de Faixas Hi-Res
 *
 * Extrai tags ID3v1/ID3v2 (MP3/AAC), Vorbis Comment e blocos STREAMINFO (FLAC),
 * cabeçalhos RIFF (WAV), calculando duração exata, taxa de amostragem, bit depth
 * e taxa média de bytes para suporte a Seek em O(1).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Estrutura que armazena os metadados extraídos de um arquivo de áudio.
 */
typedef struct {
    char title[128];            /**< Título da música (tag TIT2/TITLE ou vazio). */
    char artist[64];            /**< Nome do artista (tag TPE1/ARTIST ou vazio). */
    char album[64];             /**< Nome do álbum (tag TALB/ALBUM ou vazio). */
    char genre[32];             /**< Gênero musical (tag TCON/GENRE/©gen ou vazio). */
    uint16_t release_year;      /**< Ano de lançamento (tag TYER/TDRC/DATE/©day ou 0). */
    char format_name[16];       /**< Formato do arquivo (ex.: "FLAC", "MP3", "WAV", "OGG"). */
    uint32_t total_sec;         /**< Duração total em segundos (0 = desconhecida). */
    bool duration_is_estimate;  /**< true se a duração foi estimada por tamanho/bitrate. */
    uint32_t sample_rate;       /**< Taxa de amostragem em Hz (ex.: 44100, 96000, 192000). */
    uint16_t bits_per_sample;   /**< Resolução de bits (ex.: 16, 24 bits). */
    uint32_t bitrate;           /**< Taxa de bits em bps. */

    /**
     * Offset em bytes onde começam os dados reais de áudio (após ID3v2/cabeçalhos)
     * e média de bytes por segundo para salto rápido via fseek O(1).
     */
    uint64_t audio_data_offset; /**< Posição do primeiro frame de áudio útil no arquivo. */
    uint32_t avg_byte_rate;     /**< Taxa média de bytes por segundo (0 = indisponível). */

    float replaygain_track_db;  /**< Ganho ReplayGain da faixa em dB (ex: -6.5f). */
    float replaygain_album_db;  /**< Ganho ReplayGain do álbum em dB (ex: -5.2f). */
    bool has_replaygain_track;  /**< true se tag REPLAYGAIN_TRACK_GAIN foi extraída. */
    bool has_replaygain_album;  /**< true se tag REPLAYGAIN_ALBUM_GAIN foi extraída. */
} track_metadata_t;

/**
 * @brief Lê os metadados e tags do arquivo de áudio especificado.
 *
 * Analisa os primeiros blocos do arquivo procurando tags ID3v2, blocos de metadados
 * FLAC (STREAMINFO, VORBIS_COMMENT) ou chunks RIFF/WAV.
 * Não decodifica o fluxo de áudio, operando em alta velocidade.
 *
 * @param[in]  abs_path Caminho absoluto do arquivo no sistema FatFS (ex: "/sdcard/musica.flac").
 * @param[out] out      Ponteiro para a estrutura `track_metadata_t` a ser preenchida.
 */
void metadata_extract(const char *abs_path, track_metadata_t *out);

#ifdef __cplusplus
}
#endif

#endif // METADATA_EXTRACT_H
