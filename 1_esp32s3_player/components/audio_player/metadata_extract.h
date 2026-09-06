#ifndef METADATA_EXTRACT_H
#define METADATA_EXTRACT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char title[128];
    char artist[64];
    char album[64];
    char format_name[16];      // "ALAC", "OPUS", "OGG", "M4A", etc.
    uint32_t total_sec;        // 0 = duracao desconhecida
    bool duration_is_estimate; // true = aproximada (tamanho/bitrate), nao exata
    uint32_t sample_rate;      // taxa de amostragem em Hz
    uint16_t bits_per_sample;  // bits por amostra (ex: 16, 24)
    uint32_t bitrate;          // taxa de bits em bps

    // Pra' seek rapido (ver audio_player_seek_to_fast() em audio_player.cpp):
    // posicao em bytes de onde comecam os frames de audio de verdade
    // (depois de ID3v2/blocos de metadados FLAC/cabecalho WAV) e uma
    // media de bytes por segundo de audio a partir dali. Com os dois, da'
    // pra' estimar um offset de bytes pra' qualquer instante da faixa e
    // ir direto pra' la' com fseek, em vez de decodificar (e descartar)
    // tudo desde o comeco - essencial pra' podcasts de horas.
    // avg_byte_rate == 0 significa "sem estimativa" (qualquer coisa que falhou ao calcular)
    uint64_t audio_data_offset;
    uint32_t avg_byte_rate;
} track_metadata_t;

// Le' as tags do arquivo (ID3v2 pra .mp3/.aac, VORBIS_COMMENT pra .flac) E
// calcula a duracao total (exata quando da' - FLAC via STREAMINFO, MP3 via
// cabecalho Xing/VBRI, WAV via tamanho do chunk "data" - ou estimada por
// tamanho/bitrate quando nao da', ver duration_is_estimate). Campos sem
// tag/info correspondente ficam como string vazia ("") ou 0, NUNCA lixo -
// o chamador decide o fallback (ex: usar o nome do arquivo quando
// title[0] == '\0').
//
// M4A (.m4a) ainda nao tem duracao/tags extraidas aqui - formato baseado
// em atomos MP4, parser proprio nao implementado ainda; fica com
// total_sec=0 e as tags vazias (fallback pro nome do arquivo).
//
// So' le' os primeiros KB do arquivo (onde essas tags/cabecalhos ficam) -
// rapido, nao decodifica audio nenhum.
void metadata_extract(const char *abs_path, track_metadata_t *out);

#ifdef __cplusplus
}
#endif

#endif // METADATA_EXTRACT_H
