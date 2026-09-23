#include "sbc_enc.h"
#include "sbc.h"

#include <stdlib.h>
#include <string.h>

struct sbc_enc_s {
    sbc_t ctx;
    struct sbc_frame frame;
};

sbc_enc_handle_t sbc_enc_open(uint32_t sample_rate, int bitpool)
{
    if (sample_rate != 44100 && sample_rate != 48000) return NULL;
    if (bitpool < 2 || bitpool > 250) return NULL;

    struct sbc_enc_s *h = calloc(1, sizeof(*h));
    if (!h) return NULL;

    sbc_reset(&h->ctx);

    h->frame.msbc = false;
    h->frame.freq = (sample_rate == 44100) ? SBC_FREQ_44K1 : SBC_FREQ_48K;
    h->frame.mode = SBC_MODE_JOINT_STEREO;
    h->frame.bam  = SBC_BAM_LOUDNESS;
    h->frame.nblocks = 16;
    h->frame.nsubbands = 8;
    h->frame.bitpool = bitpool;

    return h;
}

void sbc_enc_close(sbc_enc_handle_t h)
{
    free(h);
}

int sbc_enc_frame_samples(sbc_enc_handle_t h)
{
    if (!h) return 0;
    return h->frame.nblocks * h->frame.nsubbands; // 16*8 = 128
}

int sbc_enc_process(sbc_enc_handle_t h, const int16_t *pcm_stereo_s16,
                     uint8_t *out_buf, size_t out_buf_cap)
{
    if (!h || !pcm_stereo_s16 || !out_buf) return -1;

    // Buffer entrelacado (L,R,L,R...) -> canal L comeca no indice 0,
    // canal R no indice 1, ambos com stride 2 - a API do libsbc aceita
    // isso diretamente via pitch, sem precisar desintercalar.
    int ret = sbc_encode(&h->ctx,
                          &pcm_stereo_s16[0], 2,
                          &pcm_stereo_s16[1], 2,
                          &h->frame, out_buf, (unsigned)out_buf_cap);
    if (ret != 0) return -1;

    return (int)sbc_get_frame_size(&h->frame);
}
