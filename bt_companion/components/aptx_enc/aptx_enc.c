#include "aptx_enc.h"
#include "aptXbtenc.h"

#include <stdlib.h>
#include <string.h>

struct aptx_enc_s {
    void *state;
};

aptx_enc_handle_t aptx_enc_open(uint32_t sample_rate)
{
    if (sample_rate != 44100 && sample_rate != 48000) return NULL;

    struct aptx_enc_s *h = calloc(1, sizeof(*h));
    if (!h) return NULL;

    h->state = calloc(1, (size_t)SizeofAptxbtenc());
    if (!h->state) { free(h); return NULL; }

    // endian=0 (little endian) - mesmo valor usado pelo consumidor real
    // do Android (a2dp_vendor_aptx_encoder.cc chama
    // aptx_encoder_init_func(state, 0)).
    if (aptxbtenc_init(h->state, 0) != 0) {
        free(h->state);
        free(h);
        return NULL;
    }
    // sync_mode=0 (stereo sync) - o modo padrao/normal do A2DP.
    aptxbtenc_setsync_mode(h->state, 0);

    return h;
}

void aptx_enc_close(aptx_enc_handle_t h)
{
    if (!h) return;
    free(h->state);
    free(h);
}

int aptx_enc_frame_samples(void)
{
    return 4;
}

int aptx_enc_process(aptx_enc_handle_t h, const int16_t *pcm_stereo_s16,
                      size_t frame_count, uint8_t *out_buf, size_t out_buf_cap)
{
    if (!h || !pcm_stereo_s16 || !out_buf) return -1;
    if (frame_count == 0 || (frame_count % 4) != 0) return -1;

    size_t groups = frame_count / 4;
    size_t bytes_needed = groups * 4; // 4 bytes de saida por grupo de 4 frames (compressao 4:1)
    if (bytes_needed > out_buf_cap) return -1;

    size_t out_pos = 0;
    for (size_t g = 0; g < groups; g++) {
        int32_t pcmL[4], pcmR[4];
        for (int i = 0; i < 4; i++) {
            size_t frame_idx = g * 4 + i;
            pcmL[i] = pcm_stereo_s16[frame_idx * 2 + 0];
            pcmR[i] = pcm_stereo_s16[frame_idx * 2 + 1];
        }

        // buffer[0]/buffer[1] = codewords de 16 bits (L, R) - ver
        // aptXbtenc.c vendorizado.
        int16_t codeword[2];
        aptxbtenc_encodestereo(h->state, pcmL, pcmR, codeword);

        // Empacotamento confirmado contra o consumidor real do AOSP
        // (a2dp_vendor_aptx_encoder.cc, aptx_encode_16bit()): 2 bytes
        // big-endian por canal, L depois R. Sem nenhum cabecalho
        // adicional - e' so' isso que vai no ar (ver aptx_enc.h).
        out_buf[out_pos + 0] = (uint8_t)((codeword[0] >> 8) & 0xff);
        out_buf[out_pos + 1] = (uint8_t)((codeword[0] >> 0) & 0xff);
        out_buf[out_pos + 2] = (uint8_t)((codeword[1] >> 8) & 0xff);
        out_buf[out_pos + 3] = (uint8_t)((codeword[1] >> 0) & 0xff);
        out_pos += 4;
    }

    return (int)out_pos;
}
