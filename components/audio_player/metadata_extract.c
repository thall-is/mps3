#include "metadata_extract.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h> // strcasecmp
#include <stdint.h>
#include <ctype.h>
#include <sys/stat.h>

// Tamanho maximo de dados de metadado que aceitamos varrer por arquivo -
// tanto o cabecalho ID3v2 quanto o conjunto de METADATA_BLOCKs do FLAC
// tem esse tamanho declarado no proprio arquivo; um valor absurdo aqui
// (arquivo corrompido/malicioso) e' sinal de dado invalido, nao motivo
// pra' varrer o arquivo inteiro.
#define MAX_TAG_SCAN_BYTES (512 * 1024)

// Declarada mais abaixo, usada por compute_mp3_duration() pra' pular a
// tag ID3v2 (se houver) antes de procurar o primeiro frame MPEG - ver
// nota no corpo de compute_mp3_duration().
static uint32_t id3v2_total_tag_size(FILE *fp);

static void copy_trim(char *dst, size_t dst_len, const char *src, size_t src_len)
{
    if (src_len >= dst_len) src_len = dst_len - 1;
    memcpy(dst, src, src_len);
    dst[src_len] = '\0';
    // apara espacos/controle no final (tags as vezes vem com padding)
    while (src_len > 0 && (unsigned char)dst[src_len - 1] <= ' ') {
        dst[--src_len] = '\0';
    }
}

// --- ID3v2 (MP3) -----------------------------------------------------------

static uint32_t syncsafe32(const uint8_t *b)
{
    return ((uint32_t)(b[0] & 0x7f) << 21) | ((uint32_t)(b[1] & 0x7f) << 14) |
           ((uint32_t)(b[2] & 0x7f) << 7)  |  (uint32_t)(b[3] & 0x7f);
}

// Decodifica o conteudo de um frame de texto ID3v2 (1 byte de encoding +
// texto) pro buffer de saida. So' precisamos de ASCII pra exibir (o
// resto vira '?' no sanitize_for_display de qualquer forma), entao a
// conversao de UTF-16 aqui e' propositalmente simples: pega so' os code
// units que cabem num byte.
static void decode_id3_text(const uint8_t *data, uint32_t len, char *out, size_t out_len)
{
    out[0] = '\0';
    if (len == 0) return;
    uint8_t encoding = data[0];
    const uint8_t *text = data + 1;
    uint32_t text_len = len - 1;

    if (encoding == 0 || encoding == 3) {
        // ISO-8859-1 ou UTF-8: 1 byte por caractere (UTF-8 multi-byte
        // vira lixo >=0x80, que o sanitize_for_display ja filtra na
        // exibicao - aceitavel pra' esse player).
        copy_trim(out, out_len, (const char *)text, text_len);
        return;
    }

    // UTF-16 (com ou sem BOM) - anda de 2 em 2 bytes, pega so' o byte
    // que cabe em ASCII.
    bool big_endian = false;
    uint32_t i = 0;
    if (text_len >= 2 && text[0] == 0xFE && text[1] == 0xFF) { big_endian = true; i = 2; }
    else if (text_len >= 2 && text[0] == 0xFF && text[1] == 0xFE) { big_endian = false; i = 2; }

    size_t o = 0;
    for (; i + 1 < text_len && o + 1 < out_len; i += 2) {
        uint16_t unit = big_endian ? ((text[i] << 8) | text[i + 1])
                                    : ((text[i + 1] << 8) | text[i]);
        out[o++] = (unit >= 0x20 && unit < 0x7F) ? (char)unit : '?';
    }
    out[o] = '\0';
    // apara espacos no final
    while (o > 0 && out[o - 1] == ' ') out[--o] = '\0';
}

static void extract_id3v2(FILE *fp, track_metadata_t *out)
{
    uint8_t hdr[10];
    if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) return;
    if (memcmp(hdr, "ID3", 3) != 0) return;

    uint8_t major = hdr[3];
    uint8_t flags = hdr[5];
    uint32_t tag_size = syncsafe32(&hdr[6]);
    if (tag_size == 0 || tag_size > MAX_TAG_SCAN_BYTES) return;
    if (major < 3) return; // ID3v2.2 usa frames de 3 bytes - fora de escopo, raro hoje em dia

    uint32_t remaining = tag_size;

    if (flags & 0x40) { // cabecalho estendido presente - pula
        uint8_t ext_size_buf[4];
        if (fread(ext_size_buf, 1, 4, fp) != 4) return;
        uint32_t ext_size = (major == 4) ? syncsafe32(ext_size_buf)
                                          : (uint32_t)((ext_size_buf[0] << 24) | (ext_size_buf[1] << 16) |
                                             (ext_size_buf[2] << 8) | ext_size_buf[3]);
        uint32_t bytes_to_skip = (major == 4) ? (ext_size >= 4 ? ext_size - 4 : 0) : ext_size;
        if ((uint64_t)bytes_to_skip + 4 > remaining) return;
        fseek(fp, bytes_to_skip, SEEK_CUR);
        remaining -= (4 + bytes_to_skip);
    }

    uint8_t *buf = (uint8_t *)malloc(4096);
    if (!buf) return;

    while (remaining > 10) {
        uint8_t fhdr[10];
        if (fread(fhdr, 1, 10, fp) != 10) break;
        remaining -= 10;

        if (fhdr[0] == 0) break; // padding - acabaram os frames de verdade

        char frame_id[5] = { (char)fhdr[0], (char)fhdr[1], (char)fhdr[2], (char)fhdr[3], '\0' };
        uint32_t frame_size = (major == 4)
            ? syncsafe32(&fhdr[4])
            : ((uint32_t)fhdr[4] << 24) | ((uint32_t)fhdr[5] << 16) | ((uint32_t)fhdr[6] << 8) | fhdr[7];

        if (frame_size == 0 || frame_size > remaining) break;

        bool want = (strcmp(frame_id, "TIT2") == 0 && out->title[0] == '\0') ||
                    (strcmp(frame_id, "TPE1") == 0 && out->artist[0] == '\0') ||
                    (strcmp(frame_id, "TALB") == 0 && out->album[0] == '\0');

        if (want && frame_size <= 4096) {
            if (fread(buf, 1, frame_size, fp) != frame_size) break;
            if (strcmp(frame_id, "TIT2") == 0) decode_id3_text(buf, frame_size, out->title, sizeof(out->title));
            else if (strcmp(frame_id, "TPE1") == 0) decode_id3_text(buf, frame_size, out->artist, sizeof(out->artist));
            else decode_id3_text(buf, frame_size, out->album, sizeof(out->album));
        } else {
            fseek(fp, frame_size, SEEK_CUR);
        }
        remaining -= frame_size;

        if (out->title[0] && out->artist[0] && out->album[0]) break; // achou tudo, nao precisa continuar
    }

    free(buf);
}

// --- VORBIS_COMMENT (FLAC) --------------------------------------------------

static uint32_t read_u32_le(FILE *fp, bool *ok)
{
    uint8_t b[4];
    if (fread(b, 1, 4, fp) != 4) { *ok = false; return 0; }
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void extract_vorbis_comment_block(FILE *fp, uint32_t block_len, track_metadata_t *out)
{
    bool ok = true;
    uint32_t vendor_len = read_u32_le(fp, &ok);
    if (!ok || vendor_len > block_len) return;
    fseek(fp, vendor_len, SEEK_CUR);
    uint32_t consumed = 4 + vendor_len;
    if (consumed + 4 > block_len) return;

    uint32_t comment_count = read_u32_le(fp, &ok);
    consumed += 4;
    if (!ok) return;

    char kv[400];
    for (uint32_t i = 0; i < comment_count && consumed + 4 <= block_len; i++) {
        uint32_t len = read_u32_le(fp, &ok);
        consumed += 4;
        if (!ok || consumed + len > block_len) break;
        if (len >= sizeof(kv)) { fseek(fp, len, SEEK_CUR); consumed += len; continue; }
        if (fread(kv, 1, len, fp) != len) break;
        kv[len] = '\0';
        consumed += len;

        char *eq = strchr(kv, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = kv;
        const char *value = eq + 1;

        if (strcasecmp(key, "TITLE") == 0 && out->title[0] == '\0') {
            copy_trim(out->title, sizeof(out->title), value, strlen(value));
        } else if (strcasecmp(key, "ARTIST") == 0 && out->artist[0] == '\0') {
            copy_trim(out->artist, sizeof(out->artist), value, strlen(value));
        } else if (strcasecmp(key, "ALBUM") == 0 && out->album[0] == '\0') {
            copy_trim(out->album, sizeof(out->album), value, strlen(value));
        }
    }
}

static void extract_flac(FILE *fp, track_metadata_t *out)
{
    char magic[4];
    if (fread(magic, 1, 4, fp) != 4 || memcmp(magic, "fLaC", 4) != 0) return;

    uint32_t scanned = 4;
    for (;;) {
        uint8_t bhdr[4];
        if (fread(bhdr, 1, 4, fp) != 4) return;
        scanned += 4;
        if (scanned > MAX_TAG_SCAN_BYTES) return;

        bool is_last = (bhdr[0] & 0x80) != 0;
        uint8_t block_type = bhdr[0] & 0x7f;
        uint32_t block_len = ((uint32_t)bhdr[1] << 16) | ((uint32_t)bhdr[2] << 8) | bhdr[3];

        if (block_type == 0 && block_len >= 34) {
            // STREAMINFO - sempre o primeiro bloco (obrigatorio pela spec).
            // sample_rate(20 bits) + channels-1(3) + bits_per_sample-1(5) +
            // total_samples(36) formam um campo de 64 bits comecando no
            // byte 10 do corpo do bloco.
            uint8_t si[34];
            if (fread(si, 1, 34, fp) == 34) {
                uint64_t v = 0;
                for (int i = 0; i < 8; i++) v = (v << 8) | si[10 + i];
                uint32_t sample_rate = (uint32_t)((v >> 44) & 0xFFFFFULL);
                uint64_t total_samples = v & 0xFFFFFFFFFULL; // 36 bits
                if (sample_rate > 0 && total_samples > 0) {
                    out->total_sec = (uint32_t)(total_samples / sample_rate);
                    out->duration_is_estimate = false; // STREAMINFO guarda a contagem EXATA
                }
                out->sample_rate = sample_rate;
                out->bits_per_sample = (uint16_t)(((v >> 36) & 0x1FULL) + 1);
                long extra = (long)block_len - 34;
                if (extra > 0) fseek(fp, extra, SEEK_CUR);
            }
        } else if (block_type == 4) { // VORBIS_COMMENT
            long block_start = ftell(fp);
            extract_vorbis_comment_block(fp, block_len, out);
            fseek(fp, block_start + block_len, SEEK_SET);
            // NAO retorna aqui - precisa continuar ate' o ultimo bloco de
            // verdade pra' saber onde o audio comeca (pode ter um bloco
            // PICTURE - capa de album - depois desse). So' existe um
            // VORBIS_COMMENT por arquivo, mas pode nao ser o ultimo bloco.
        } else {
            fseek(fp, block_len, SEEK_CUR);
        }

        scanned += block_len;
        if (is_last) {
            out->audio_data_offset = (uint64_t)ftell(fp); // logo apos o ultimo bloco de metadados = 1o frame de audio
            return;
        }
    }
}

// --- Duracao a partir do cabecalho (MP3/WAV/AAC/FLAC) ----------------------
// Calcula a duracao total da faixa diretamente a partir dos cabecalhos do
// arquivo (tags ID3v2/TLEN, Xing/VBR, formato RIFF WAV ou amostragem FLAC),
// sem necessidade de decodificar antecipadamente os fluxos de audio.

static const int MP3_BITRATE_TABLE[2][3][14] = {
    { // MPEG1
        {32,64,96,128,160,192,224,256,288,320,352,384,416,448}, // Layer I
        {32,48,56,64,80,96,112,128,160,192,224,256,320,384},    // Layer II
        {32,40,48,56,64,80,96,112,128,160,192,224,256,320},     // Layer III
    },
    { // MPEG2 / MPEG2.5 (Layer II e III usam a MESMA tabela)
        {32,48,56,64,80,96,112,128,144,160,176,192,224,256},    // Layer I
        {8,16,24,32,40,48,56,64,80,96,112,128,144,160},         // Layer II
        {8,16,24,32,40,48,56,64,80,96,112,128,144,160},         // Layer III
    },
};
static const int MP3_SAMPLE_RATE_TABLE[3][3] = {
    {44100, 48000, 32000}, // MPEG1
    {22050, 24000, 16000}, // MPEG2
    {11025, 12000, 8000},  // MPEG2.5
};
static const int MP3_SAMPLES_PER_FRAME[2][3] = {
    {384, 1152, 1152}, // MPEG1: Layer I, II, III
    {384, 1152, 576},  // MPEG2/2.5: Layer I, II, III
};

// Acha o primeiro cabecalho de frame MPEG audio valido em buf[0..len) e
// devolve sample_rate/amostras-por-frame/bitrate dele. false se nao achar.
static bool parse_first_mp3_frame(const uint8_t *buf, size_t len,
                                   int *sample_rate, int *samples_per_frame, int *bitrate_kbps)
{
    for (size_t i = 0; i + 4 <= len; i++) {
        if (buf[i] != 0xFF || (buf[i + 1] & 0xE0) != 0xE0) continue;

        int version_bits = (buf[i + 1] >> 3) & 0x03; // 00=2.5 01=reservado 10=2 11=1
        int layer_bits    = (buf[i + 1] >> 1) & 0x03; // 00=reservado 01=III 10=II 11=I
        if (version_bits == 1 || layer_bits == 0) continue;

        int mpeg2_group = (version_bits == 3) ? 0 : 1;
        int layer_idx = (layer_bits == 3) ? 0 : (layer_bits == 2) ? 1 : 2;
        int version_table_idx = (version_bits == 3) ? 0 : (version_bits == 2) ? 1 : 2;

        int bitrate_idx = (buf[i + 2] >> 4) & 0x0F;
        int srate_idx    = (buf[i + 2] >> 2) & 0x03;
        if (bitrate_idx == 0 || bitrate_idx == 15 || srate_idx == 3) continue;

        *bitrate_kbps = MP3_BITRATE_TABLE[mpeg2_group][layer_idx][bitrate_idx - 1];
        *sample_rate = MP3_SAMPLE_RATE_TABLE[version_table_idx][srate_idx];
        *samples_per_frame = MP3_SAMPLES_PER_FRAME[mpeg2_group][layer_idx];
        return true;
    }
    return false;
}

static void compute_mp3_duration(FILE *fp, uint64_t file_size, track_metadata_t *out)
{
    // Buffer de 8KB - NUNCA na pilha (player_task tem so' 12KB de stack
    // no total, ja' consumidos em boa parte por play_track()/metadata_extract()
    // - um array local desse tamanho aqui estourava a pilha e derrubava
    // o aparelho, sempre que tocava um MP3).
    uint8_t *buf = (uint8_t *)malloc(8192);
    if (!buf) return;
    // BUG ENCONTRADO: isso costumava reler sempre a partir do byte 0 do
    // arquivo, incluindo a tag ID3v2 inteira. Se a tag tiver uma capa de
    // album embutida (bem comum, facilmente > 8KB), o primeiro frame MPEG
    // de verdade cai fora desses 8KB, parse_first_mp3_frame() nunca acha
    // nada, e a faixa fica sem total_sec/avg_byte_rate - sem avisar, o
    // seek rapido simplesmente nunca ativa nesses arquivos e cai pro modo
    // lento (decodificar+descartar) o tempo todo. Pula a tag primeiro.
    uint32_t skip = id3v2_total_tag_size(fp);
    if (skip >= file_size) { free(buf); return; } // tag cobre o arquivo inteiro - invalido
    fseek(fp, (long)skip, SEEK_SET);
    size_t n = fread(buf, 1, 8192, fp);
    if (n < 20) { free(buf); return; }

    // Cabecalho Xing/Info (VBR/CBR com TOC) ou VBRI (Fraunhofer) - da' a
    // contagem EXATA de frames quando presente.
    uint32_t xing_frames = 0;
    bool has_xing = false;
    for (size_t i = 0; i + 8 <= n; i++) {
        if (memcmp(buf + i, "Xing", 4) == 0 || memcmp(buf + i, "Info", 4) == 0) {
            uint32_t flags = ((uint32_t)buf[i+4]<<24)|((uint32_t)buf[i+5]<<16)|((uint32_t)buf[i+6]<<8)|buf[i+7];
            if ((flags & 0x01) && i + 12 <= n) {
                xing_frames = ((uint32_t)buf[i+8]<<24)|((uint32_t)buf[i+9]<<16)|((uint32_t)buf[i+10]<<8)|buf[i+11];
                has_xing = xing_frames > 0;
            }
            break;
        }
        if (memcmp(buf + i, "VBRI", 4) == 0 && i + 18 <= n) {
            xing_frames = ((uint32_t)buf[i+14]<<24)|((uint32_t)buf[i+15]<<16)|((uint32_t)buf[i+16]<<8)|buf[i+17];
            has_xing = xing_frames > 0;
            break;
        }
    }

    int sample_rate = 0, samples_per_frame = 0, bitrate_kbps = 0;
    if (!parse_first_mp3_frame(buf, n, &sample_rate, &samples_per_frame, &bitrate_kbps)) { free(buf); return; }

    out->audio_data_offset = skip;
    out->sample_rate = sample_rate;
    out->bits_per_sample = 16;
    if (bitrate_kbps > 0) {
        out->bitrate = (uint32_t)(bitrate_kbps * 1000);
        out->avg_byte_rate = (uint32_t)(bitrate_kbps * 1000 / 8);
    }

    if (has_xing && sample_rate > 0) {
        out->total_sec = (uint32_t)(((uint64_t)xing_frames * samples_per_frame) / sample_rate);
        out->duration_is_estimate = false;
        if (out->total_sec > 0 && file_size > skip) {
            out->avg_byte_rate = (uint32_t)((file_size - skip) / out->total_sec);
        }
        free(buf);
        return;
    }

    if (bitrate_kbps > 0) {
        out->total_sec = (uint32_t)((file_size * 8) / ((uint64_t)bitrate_kbps * 1000));
        out->duration_is_estimate = true; // CBR: praticamente exato; VBR sem Xing/VBRI: aproximado
    }
    free(buf);
}

static void compute_wav_duration(FILE *fp, uint64_t file_size, track_metadata_t *out)
{
    uint8_t hdr[12];
    fseek(fp, 0, SEEK_SET);
    if (fread(hdr, 1, 12, fp) != 12) return;
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) return;

    uint32_t byte_rate = 0, data_size = 0;
    bool have_fmt = false, have_data = false;

    while (!have_data) {
        uint8_t chdr[8];
        if (fread(chdr, 1, 8, fp) != 8) break;
        uint32_t chunk_size = (uint32_t)chdr[4] | ((uint32_t)chdr[5] << 8) |
                               ((uint32_t)chdr[6] << 16) | ((uint32_t)chdr[7] << 24);

        if (memcmp(chdr, "fmt ", 4) == 0 && chunk_size >= 16) {
            uint8_t f[16];
            if (fread(f, 1, 16, fp) != 16) break;
            out->sample_rate = (uint32_t)f[4] | ((uint32_t)f[5] << 8) | ((uint32_t)f[6] << 16) | ((uint32_t)f[7] << 24);
            byte_rate = (uint32_t)f[8] | ((uint32_t)f[9] << 8) | ((uint32_t)f[10] << 16) | ((uint32_t)f[11] << 24);
            out->bits_per_sample = (uint16_t)f[14] | ((uint16_t)f[15] << 8);
            out->bitrate = byte_rate * 8;
            have_fmt = true;
            long extra = (long)chunk_size - 16;
            if (extra > 0) fseek(fp, extra, SEEK_CUR);
            if (chunk_size & 1) fseek(fp, 1, SEEK_CUR);
        } else if (memcmp(chdr, "data", 4) == 0) {
            data_size = chunk_size;
            have_data = true;
            out->audio_data_offset = (uint64_t)ftell(fp); // logo apos o cabecalho do chunk "data" = 1o byte de PCM
        } else {
            fseek(fp, chunk_size + (chunk_size & 1), SEEK_CUR);
        }
    }
    if (!have_fmt || byte_rate == 0) return;
    out->avg_byte_rate = byte_rate;

    if (have_data && data_size > 0 && data_size != 0xFFFFFFFFu) {
        out->total_sec = data_size / byte_rate;
        out->duration_is_estimate = false;
        return;
    }
    // Chunk "data" sem tamanho valido (raro) - estima descontando um
    // cabecalho WAV tipico de 44 bytes.
    if (file_size > 44) {
        out->total_sec = (uint32_t)((file_size - 44) / byte_rate);
        out->duration_is_estimate = true;
        if (out->audio_data_offset == 0) out->audio_data_offset = 44;
    }
}

static void compute_aac_duration(FILE *fp, uint64_t file_size, track_metadata_t *out)
{
    static const int SR_TABLE[13] = {96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};

    // Buffer de 4KB - NUNCA na pilha (mesmo motivo de compute_mp3_duration
    // acima: player_task so' tem 12KB de stack no total).
    uint8_t *buf = (uint8_t *)malloc(4096);
    if (!buf) return;
    fseek(fp, 0, SEEK_SET);
    size_t n = fread(buf, 1, 4096, fp);
    if (n < 7) { free(buf); return; }

    // Acha o primeiro syncword ADTS valido (pode ter um ID3v2 antes,
    // embora incomum em .aac cru). Mascara 0xF6/alvo 0xF0 no 2o byte
    // checa o nibble de sync + "layer"(2 bits, sempre 00 no AAC),
    // ignorando ID(1 bit) e protection_absent(1 bit) - podem ser 0 ou 1.
    size_t pos = 0;
    bool found = false;
    for (; pos + 7 <= n; pos++) {
        if (buf[pos] == 0xFF && (buf[pos + 1] & 0xF6) == 0xF0) { found = true; break; }
    }
    if (!found) { free(buf); return; }

    int sr_idx = (buf[pos + 2] >> 2) & 0x0F;
    if (sr_idx >= 13) { free(buf); return; }
    int sample_rate = SR_TABLE[sr_idx];
    if (sample_rate == 0) { free(buf); return; }
    out->sample_rate = sample_rate;
    out->bits_per_sample = 16;

    // Anda por ate' 20 frames consecutivos somando o tamanho, pra tirar
    // uma media (AAC costuma ser VBR - um unico frame nao e' confiavel).
    size_t p = pos;
    int frames_seen = 0;
    uint64_t total_frame_len = 0;
    while (frames_seen < 20 && p + 7 <= n) {
        if (buf[p] != 0xFF || (buf[p + 1] & 0xF6) != 0xF0) break;
        uint32_t frame_len = (((uint32_t)buf[p+3] & 0x03) << 11) | ((uint32_t)buf[p+4] << 3) |
                              (((uint32_t)buf[p+5] >> 5) & 0x07);
        if (frame_len < 7 || p + frame_len > n) break;
        total_frame_len += frame_len;
        frames_seen++;
        p += frame_len;
    }
    if (frames_seen == 0) { free(buf); return; }

    double avg_frame_len = (double)total_frame_len / frames_seen;
    double total_frames_est = (double)file_size / avg_frame_len;
    out->total_sec = (uint32_t)((total_frames_est * 1024.0) / sample_rate);
    out->duration_is_estimate = true; // AAC nao guarda contagem total de amostras - sempre estimativa
    out->audio_data_offset = pos;
    out->avg_byte_rate = (uint32_t)((avg_frame_len * sample_rate) / 1024.0);
    free(buf);
}

// --- Entrada publica ---------------------------------------------------------

// Tamanho TOTAL da tag ID3v2 (cabecalho+corpo+rodape, se houver), sem
// precisar entender frame por frame - so' le' o cabecalho fixo de 10
// bytes e o campo de tamanho "syncsafe". 0 se o arquivo nao comeca com
// "ID3". Usado so' pra achar a BORDA onde o audio comeca (ver
// audio_data_offset em metadata_extract.h) - mais confiavel pra isso do
// que confiar na posicao final de extract_id3v2() (que pode parar no
// meio se achar um frame invalido). Sempre restaura a posicao de fp.
static uint32_t id3v2_total_tag_size(FILE *fp)
{
    long pos_before = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t hdr[10];
    uint32_t result = 0;
    if (fread(hdr, 1, 10, fp) == 10 && hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3') {
        uint32_t tag_size = ((uint32_t)(hdr[6] & 0x7F) << 21) | ((uint32_t)(hdr[7] & 0x7F) << 14) |
                             ((uint32_t)(hdr[8] & 0x7F) << 7)  | ((uint32_t)(hdr[9] & 0x7F));
        bool has_footer = (hdr[5] & 0x10) != 0;
        result = 10 + tag_size + (has_footer ? 10 : 0);
    }
    fseek(fp, pos_before, SEEK_SET);
    return result;
}

// --- M4A / ALAC / MP4 --------------------------------------------------------

static void extract_m4a(FILE *fp, uint64_t file_size, track_metadata_t *out)
{
    uint64_t pos = 0;
    uint64_t moov_pos = 0;
    uint64_t moov_size = 0;
    uint64_t mdat_pos = 0;
    bool found_moov = false;

    // Varre caixas de topo para achar moov e mdat
    while (pos + 8 <= file_size) {
        if (fseek(fp, (long)pos, SEEK_SET) != 0) break;
        uint8_t hdr[8];
        if (fread(hdr, 1, 8, fp) != 8) break;
        uint64_t bsz = ((uint64_t)hdr[0] << 24) | ((uint64_t)hdr[1] << 16) |
                       ((uint64_t)hdr[2] << 8)  | (uint64_t)hdr[3];
        char typ[5] = { (char)hdr[4], (char)hdr[5], (char)hdr[6], (char)hdr[7], 0 };
        uint64_t hlen = 8;
        if (bsz == 1) {
            uint8_t ext[8];
            if (fread(ext, 1, 8, fp) != 8) break;
            bsz = 0;
            for (int i = 0; i < 8; i++) bsz = (bsz << 8) | ext[i];
            hlen = 16;
        } else if (bsz == 0) {
            bsz = file_size - pos;
        }
        if (bsz < hlen || pos + bsz > file_size) break;

        if (strcmp(typ, "moov") == 0 && !found_moov) {
            moov_pos = pos;
            moov_size = bsz;
            found_moov = true;
        } else if (strcmp(typ, "mdat") == 0 && mdat_pos == 0) {
            mdat_pos = pos;
        }
        pos += bsz;
    }

    if (!found_moov || moov_size == 0) return;
    out->audio_data_offset = mdat_pos > 0 ? (mdat_pos + 8) : 0;

    // Carrega o conteudo de moov (ou ate 512KB) para varredura em memoria
    uint32_t to_read = (moov_size > (uint64_t)MAX_TAG_SCAN_BYTES) ? (uint32_t)MAX_TAG_SCAN_BYTES : (uint32_t)moov_size;
    uint8_t *moov = (uint8_t *)malloc(to_read);
    if (!moov) return;

    if (fseek(fp, (long)moov_pos, SEEK_SET) != 0 || fread(moov, 1, to_read, fp) != to_read) {
        free(moov);
        return;
    }

    // 1. Acha mvhd para timescale e duracao exata
    for (uint32_t i = 0; i + 32 <= to_read; i++) {
        if (memcmp(moov + i, "mvhd", 4) == 0 && i >= 4) {
            uint8_t ver = moov[i + 4];
            uint32_t timescale = 0;
            uint64_t duration = 0;
            if (ver == 0 && i + 28 <= to_read) {
                timescale = ((uint32_t)moov[i + 16] << 24) | ((uint32_t)moov[i + 17] << 16) |
                            ((uint32_t)moov[i + 18] << 8)  | (uint32_t)moov[i + 19];
                duration = ((uint32_t)moov[i + 20] << 24) | ((uint32_t)moov[i + 21] << 16) |
                           ((uint32_t)moov[i + 22] << 8)  | (uint32_t)moov[i + 23];
            } else if (ver == 1 && i + 36 <= to_read) {
                timescale = ((uint32_t)moov[i + 24] << 24) | ((uint32_t)moov[i + 25] << 16) |
                            ((uint32_t)moov[i + 26] << 8)  | (uint32_t)moov[i + 27];
                duration = 0;
                for (int b = 0; b < 8; b++) duration = (duration << 8) | moov[i + 28 + b];
            }
            if (timescale > 0) {
                out->total_sec = (uint32_t)(duration / timescale);
                out->duration_is_estimate = false;
            }
            break;
        }
    }

    // 2. Acha stsd e identifica se o codec e' ALAC ou AAC
    bool is_alac = false;
    for (uint32_t i = 0; i + 36 <= to_read; i++) {
        if (memcmp(moov + i, "alac", 4) == 0 && i >= 4) {
            is_alac = true;
            snprintf(out->format_name, sizeof(out->format_name), "ALAC");
            // Se houver sub-bloco alac com configuracao especifica:
            for (uint32_t j = i + 4; j + 36 <= to_read; j++) {
                if (memcmp(moov + j, "alac", 4) == 0 && j >= 4) {
                    uint8_t *cfg = moov + j + 8; // ALACSpecificConfig
                    out->bits_per_sample = cfg[5];
                    out->sample_rate = ((uint32_t)cfg[20] << 24) | ((uint32_t)cfg[21] << 16) |
                                       ((uint32_t)cfg[22] << 8)  | (uint32_t)cfg[23];
                    out->bitrate = ((uint32_t)cfg[16] << 24) | ((uint32_t)cfg[17] << 16) |
                                   ((uint32_t)cfg[18] << 8)  | (uint32_t)cfg[19];
                    break;
                }
            }
            if (out->bits_per_sample == 0) out->bits_per_sample = 16;
            break;
        } else if (memcmp(moov + i, "mp4a", 4) == 0 && i >= 4) {
            snprintf(out->format_name, sizeof(out->format_name), "M4A");
            out->bits_per_sample = 16;
            if (i + 34 <= to_read) {
                out->sample_rate = ((uint32_t)moov[i + 24] << 8) | (uint32_t)moov[i + 25];
            }
            break;
        }
    }
    if (out->format_name[0] == '\0') {
        snprintf(out->format_name, sizeof(out->format_name), is_alac ? "ALAC" : "M4A");
        if (out->bits_per_sample == 0) out->bits_per_sample = 16;
    }

    // 3. Extrai tags ilst (©nam, ©ART, ©alb)
    for (uint32_t i = 0; i + 24 <= to_read; i++) {
        char *target = NULL;
        size_t max_len = 0;
        if (memcmp(moov + i, "\xa9" "nam", 4) == 0 && out->title[0] == '\0') {
            target = out->title; max_len = sizeof(out->title);
        } else if (memcmp(moov + i, "\xa9" "ART", 4) == 0 && out->artist[0] == '\0') {
            target = out->artist; max_len = sizeof(out->artist);
        } else if (memcmp(moov + i, "\xa9" "alb", 4) == 0 && out->album[0] == '\0') {
            target = out->album; max_len = sizeof(out->album);
        }
        if (target && i >= 4) {
            uint32_t atom_sz = ((uint32_t)moov[i-4] << 24) | ((uint32_t)moov[i-3] << 16) |
                               ((uint32_t)moov[i-2] << 8)  | (uint32_t)moov[i-1];
            if (atom_sz >= 16 && i + atom_sz <= to_read) {
                for (uint32_t d = i + 4; d + 16 <= i + atom_sz; d++) {
                    if (memcmp(moov + d, "data", 4) == 0 && d >= 4) {
                        uint32_t data_sz = ((uint32_t)moov[d-4] << 24) | ((uint32_t)moov[d-3] << 16) |
                                           ((uint32_t)moov[d-2] << 8)  | (uint32_t)moov[d-1];
                        if (data_sz > 16 && d + data_sz - 4 <= to_read) {
                            uint32_t txt_len = data_sz - 16;
                            copy_trim(target, max_len, (const char *)(moov + d + 12), txt_len);
                        }
                        break;
                    }
                }
            }
        }
    }

    if (out->total_sec > 0 && file_size > 0) {
        if (out->bitrate == 0) {
            out->bitrate = (uint32_t)((file_size * 8) / out->total_sec);
        }
        out->avg_byte_rate = (uint32_t)(file_size / out->total_sec);
    }

    free(moov);
}

// --- Ogg / Opus / Vorbis -----------------------------------------------------

static void extract_ogg(FILE *fp, uint64_t file_size, track_metadata_t *out)
{
    if (fseek(fp, 0, SEEK_SET) != 0) return;
    uint8_t buf[256];
    if (fread(buf, 1, sizeof(buf), fp) < 64) return;
    if (memcmp(buf, "OggS", 4) != 0) return;

    uint8_t nsegs = buf[26];
    uint32_t payload_offset = 27 + nsegs;
    if (payload_offset + 32 > sizeof(buf)) return;

    const uint8_t *payload = buf + payload_offset;
    bool is_opus = false;

    if (memcmp(payload, "OpusHead", 8) == 0) {
        is_opus = true;
        snprintf(out->format_name, sizeof(out->format_name), "OPUS");
        out->sample_rate = 48000;
        out->bits_per_sample = 16;
    } else if (memcmp(payload, "\x01vorbis", 7) == 0) {
        snprintf(out->format_name, sizeof(out->format_name), "OGG");
        out->bits_per_sample = 16;
        out->sample_rate = (uint32_t)payload[12] | ((uint32_t)payload[13] << 8) |
                           ((uint32_t)payload[14] << 16) | ((uint32_t)payload[15] << 24);
        int32_t br_nom = (int32_t)((uint32_t)payload[20] | ((uint32_t)payload[21] << 8) |
                                   ((uint32_t)payload[22] << 16) | ((uint32_t)payload[23] << 24));
        if (br_nom > 0) out->bitrate = (uint32_t)br_nom;
    } else {
        return;
    }

    // Le as tags na segunda pagina Ogg (comentarios Vorbis / OpusTags) nos primeiros 8KB
    fseek(fp, 0, SEEK_SET);
    uint8_t *head8k = (uint8_t *)malloc(8192);
    if (head8k) {
        size_t n_head = fread(head8k, 1, 8192, fp);
        for (size_t i = 28; i + 16 < n_head; i++) {
            if (memcmp(head8k + i, "OpusTags", 8) == 0) {
                fseek(fp, (long)(i + 8), SEEK_SET);
                extract_vorbis_comment_block(fp, 4096, out);
                break;
            } else if (memcmp(head8k + i, "\x03vorbis", 7) == 0) {
                fseek(fp, (long)(i + 7), SEEK_SET);
                extract_vorbis_comment_block(fp, 4096, out);
                break;
            }
        }

        // Localiza o primeiro pacote de audio real (primeira pagina com granule_pos > 0)
        // para definir audio_data_offset
        size_t scan_pos = 0;
        while (scan_pos + 27 < n_head) {
            if (memcmp(head8k + scan_pos, "OggS", 4) != 0) {
                scan_pos++;
                continue;
            }
            uint64_t granule = (uint64_t)head8k[scan_pos + 6] | ((uint64_t)head8k[scan_pos + 7] << 8) |
                               ((uint64_t)head8k[scan_pos + 8] << 16) | ((uint64_t)head8k[scan_pos + 9] << 24) |
                               ((uint64_t)head8k[scan_pos + 10] << 32) | ((uint64_t)head8k[scan_pos + 11] << 40) |
                               ((uint64_t)head8k[scan_pos + 12] << 48) | ((uint64_t)head8k[scan_pos + 13] << 56);
            uint8_t n_s = head8k[scan_pos + 26];
            uint32_t body = 0;
            if (scan_pos + 27 + n_s <= n_head) {
                for (uint8_t s = 0; s < n_s; s++) body += head8k[scan_pos + 27 + s];
            }
            if (granule > 0) {
                out->audio_data_offset = scan_pos;
                break;
            }
            scan_pos += 27 + n_s + body;
        }
        free(head8k);
    }
    if (out->audio_data_offset == 0) {
        out->audio_data_offset = is_opus ? 138 : 4096;
    }

    // Acha a ultima pagina OggS nos ultimos 64KB para obter o granule_pos final (duracao exata)
    uint32_t tail_read = (file_size > 65536) ? 65536 : (uint32_t)file_size;
    long tail_pos = (long)(file_size - tail_read);
    fseek(fp, tail_pos, SEEK_SET);

    uint8_t *tail = (uint8_t *)malloc(tail_read);
    if (tail) {
        size_t n_tail = fread(tail, 1, tail_read, fp);
        if (n_tail >= 28) {
            for (int i = (int)n_tail - 28; i >= 0; i--) {
                if (memcmp(tail + i, "OggS", 4) == 0) {
                    uint64_t granule = (uint64_t)tail[i + 6] | ((uint64_t)tail[i + 7] << 8) |
                                       ((uint64_t)tail[i + 8] << 16) | ((uint64_t)tail[i + 9] << 24) |
                                       ((uint64_t)tail[i + 10] << 32) | ((uint64_t)tail[i + 11] << 40) |
                                       ((uint64_t)tail[i + 12] << 48) | ((uint64_t)tail[i + 13] << 56);
                    if (granule > 0 && granule != 0xFFFFFFFFFFFFFFFFULL) {
                        if (is_opus) {
                            out->total_sec = (uint32_t)(granule / 48000ULL);
                        } else if (out->sample_rate > 0) {
                            out->total_sec = (uint32_t)(granule / out->sample_rate);
                        }
                        out->duration_is_estimate = false;
                        break;
                    }
                }
            }
        }
        free(tail);
    }

    if (out->total_sec > 0 && file_size > 0) {
        if (out->bitrate == 0) {
            out->bitrate = (uint32_t)((file_size * 8) / out->total_sec);
        }
        out->avg_byte_rate = (uint32_t)(file_size / out->total_sec);
    }
}

void metadata_extract(const char *abs_path, track_metadata_t *out)
{
    memset(out, 0, sizeof(*out));

    const char *ext = strrchr(abs_path, '.');
    if (!ext) return;

    struct stat st;
    uint64_t file_size = (stat(abs_path, &st) == 0) ? (uint64_t)st.st_size : 0;

    FILE *fp = fopen(abs_path, "rb");
    if (!fp) return;

    if (strcasecmp(ext, ".mp3") == 0) {
        snprintf(out->format_name, sizeof(out->format_name), "MP3");
        extract_id3v2(fp, out);
        compute_mp3_duration(fp, file_size, out);
        out->audio_data_offset = id3v2_total_tag_size(fp);
    } else if (strcasecmp(ext, ".flac") == 0) {
        snprintf(out->format_name, sizeof(out->format_name), "FLAC");
        extract_flac(fp, out);
    } else if (strcasecmp(ext, ".wav") == 0) {
        snprintf(out->format_name, sizeof(out->format_name), "WAV");
        compute_wav_duration(fp, file_size, out);
    } else if (strcasecmp(ext, ".aac") == 0) {
        snprintf(out->format_name, sizeof(out->format_name), "AAC");
        extract_id3v2(fp, out);
        compute_aac_duration(fp, file_size, out);
        out->audio_data_offset = id3v2_total_tag_size(fp);
    } else if (strcasecmp(ext, ".m4a") == 0 || strcasecmp(ext, ".m4b") == 0 || strcasecmp(ext, ".alac") == 0) {
        extract_m4a(fp, file_size, out);
        if (strcasecmp(ext, ".alac") == 0) {
            snprintf(out->format_name, sizeof(out->format_name), "ALAC");
            if (out->bits_per_sample == 0) out->bits_per_sample = 24;
        }
    } else if (strcasecmp(ext, ".ogg") == 0 || strcasecmp(ext, ".opus") == 0) {
        extract_ogg(fp, file_size, out);
        if (strcasecmp(ext, ".opus") == 0) {
            snprintf(out->format_name, sizeof(out->format_name), "OPUS");
        }
    }

    // Garante que o bitrate e avg_byte_rate estejam sempre preenchidos se total_sec > 0
    if (out->total_sec > 0 && file_size > 0) {
        if (out->bitrate == 0) {
            out->bitrate = (uint32_t)((file_size * 8) / out->total_sec);
        }
        if (out->avg_byte_rate == 0) {
            uint64_t audio_bytes = (file_size > out->audio_data_offset) ? (file_size - out->audio_data_offset) : file_size;
            out->avg_byte_rate = (uint32_t)(audio_bytes / out->total_sec);
        }
    }

    fclose(fp);
}
