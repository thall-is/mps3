#include "aptx_hd_enc.h"
#include "aptXHDbtenc.h"

#include <stdlib.h>
#include <string.h>

struct aptx_hd_enc_s {
    void *state;
};

aptx_hd_enc_handle_t aptx_hd_enc_open(uint32_t sample_rate)
{
    if (sample_rate != 44100 && sample_rate != 48000) return NULL;

    struct aptx_hd_enc_s *h = calloc(1, sizeof(*h));
    if (!h) return NULL;

    h->state = calloc(1, (size_t)SizeofAptxhdbtenc());
    if (!h->state) { free(h); return NULL; }

    if (aptxhdbtenc_init(h->state, 0) != 0) { // endian=0, mesmo valor do consumidor real
        free(h->state);
        free(h);
        return NULL;
    }

    return h;
}

void aptx_hd_enc_close(aptx_hd_enc_handle_t h)
{
    if (!h) return;
    free(h->state);
    free(h);
}

int aptx_hd_enc_frame_samples(void)
{
    return 4;
}

int aptx_hd_enc_process_s32(aptx_hd_enc_handle_t h, const int32_t *pcm_stereo_s32,
                             size_t frame_count, uint8_t *out_buf, size_t out_buf_cap)
{
    if (!h || !pcm_stereo_s32 || !out_buf) return -1;
    if (frame_count == 0 || (frame_count % 4) != 0) return -1;

    size_t groups = frame_count / 4;
    size_t bytes_needed = groups * 6;
    if (bytes_needed > out_buf_cap) return -1;

    size_t out_pos = 0;
    for (size_t g = 0; g < groups; g++) {
        int32_t pcmL[4], pcmR[4];
        for (int i = 0; i < 4; i++) {
            size_t frame_idx = g * 4 + i;
            // Desloca 8 bits para a direita: amostra de 24 bits nos bits 31..8
            // torna-se um valor Q23 (24 bits com sinal, alinhado a direita no int32).
            pcmL[i] = pcm_stereo_s32[frame_idx * 2 + 0] >> 8;
            pcmR[i] = pcm_stereo_s32[frame_idx * 2 + 1] >> 8;
        }

        int32_t codeword[2];
        aptxhdbtenc_encodestereo(h->state, pcmL, pcmR, codeword);

        out_buf[out_pos + 0] = (uint8_t)((codeword[0] >> 16) & 0xff);
        out_buf[out_pos + 1] = (uint8_t)((codeword[0] >> 8) & 0xff);
        out_buf[out_pos + 2] = (uint8_t)((codeword[0] >> 0) & 0xff);
        out_buf[out_pos + 3] = (uint8_t)((codeword[1] >> 16) & 0xff);
        out_buf[out_pos + 4] = (uint8_t)((codeword[1] >> 8) & 0xff);
        out_buf[out_pos + 5] = (uint8_t)((codeword[1] >> 0) & 0xff);
        out_pos += 6;
    }

    return (int)out_pos;
}

int aptx_hd_enc_process(aptx_hd_enc_handle_t h, const int16_t *pcm_stereo_s16,
                         size_t frame_count, uint8_t *out_buf, size_t out_buf_cap)
{
    if (!h || !pcm_stereo_s16 || !out_buf) return -1;
    if (frame_count == 0 || (frame_count % 4) != 0) return -1;

    size_t groups = frame_count / 4;
    size_t bytes_needed = groups * 6; // 6 bytes de saida por grupo de 4 frames (3 por canal)
    if (bytes_needed > out_buf_cap) return -1;

    size_t out_pos = 0;
    for (size_t g = 0; g < groups; g++) {
        int32_t pcmL[4], pcmR[4];
        for (int i = 0; i < 4; i++) {
            size_t frame_idx = g * 4 + i;
            // Alarga 16 bits pra' 24 (desloca 8 bits pra' esquerda)
            pcmL[i] = ((int32_t)pcm_stereo_s16[frame_idx * 2 + 0]) << 8;
            pcmR[i] = ((int32_t)pcm_stereo_s16[frame_idx * 2 + 1]) << 8;
        }

        int32_t codeword[2]; // so' os 24 bits baixos de cada elemento sao significativos
        aptxhdbtenc_encodestereo(h->state, pcmL, pcmR, codeword);

        out_buf[out_pos + 0] = (uint8_t)((codeword[0] >> 16) & 0xff);
        out_buf[out_pos + 1] = (uint8_t)((codeword[0] >> 8) & 0xff);
        out_buf[out_pos + 2] = (uint8_t)((codeword[0] >> 0) & 0xff);
        out_buf[out_pos + 3] = (uint8_t)((codeword[1] >> 16) & 0xff);
        out_buf[out_pos + 4] = (uint8_t)((codeword[1] >> 8) & 0xff);
        out_buf[out_pos + 5] = (uint8_t)((codeword[1] >> 0) & 0xff);
        out_pos += 6;
    }

    return (int)out_pos;
}
