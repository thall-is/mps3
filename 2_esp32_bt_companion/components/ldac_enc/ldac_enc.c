#include "ldac_enc.h"
#include "ldacBT.h"

#include <stdlib.h>
#include <string.h>

struct ldac_enc_s {
    HANDLE_LDAC_BT bt;
    int            mtu;
    uint32_t       sample_rate;
};

static int quality_to_eqmid(ldac_enc_quality_t q)
{
    switch (q) {
        case LDAC_ENC_QUALITY_HIGH:     return LDACBT_EQMID_HQ;
        case LDAC_ENC_QUALITY_STANDARD: return LDACBT_EQMID_SQ;
        case LDAC_ENC_QUALITY_MOBILE:   return LDACBT_EQMID_MQ;
        default:                        return LDACBT_EQMID_SQ;
    }
}

ldac_enc_handle_t ldac_enc_open(int mtu, uint32_t sample_rate, ldac_enc_quality_t quality)
{
    if (sample_rate != 44100 && sample_rate != 48000 &&
        sample_rate != 88200 && sample_rate != 96000) {
        return NULL;
    }

    struct ldac_enc_s *h = calloc(1, sizeof(*h));
    if (!h) return NULL;

    h->bt = ldacBT_get_handle();
    if (!h->bt) {
        free(h);
        return NULL;
    }

    h->mtu = mtu;
    h->sample_rate = sample_rate;

    int ret = ldacBT_init_handle_encode(h->bt, mtu, quality_to_eqmid(quality),
                                         LDACBT_CHANNEL_MODE_STEREO,
                                         LDACBT_SMPL_FMT_S32, (int)sample_rate);
    if (ret != 0) {
        ldacBT_free_handle(h->bt);
        free(h);
        return NULL;
    }

    return h;
}

void ldac_enc_close(ldac_enc_handle_t h)
{
    if (!h) return;
    if (h->bt) {
        ldacBT_close_handle(h->bt);
        ldacBT_free_handle(h->bt);
    }
    free(h);
}

bool ldac_enc_set_quality(ldac_enc_handle_t h, ldac_enc_quality_t quality)
{
    if (!h) return false;
    return ldacBT_set_eqmid(h->bt, quality_to_eqmid(quality)) == 0;
}

int ldac_enc_frame_samples(ldac_enc_handle_t h)
{
    if (!h) return 0;
    // Ver tabela no header original: 128 amostras/canal em 44.1/48kHz,
    // 256 em 88.2/96kHz.
    return (h->sample_rate == 88200 || h->sample_rate == 96000) ? 256 : 128;
}

int ldac_enc_process_s32(ldac_enc_handle_t h, const int32_t *pcm_stereo_s32, size_t frame_count,
                         uint8_t *out_buf, size_t out_buf_cap, int *out_frame_count)
{
    if (!h || !pcm_stereo_s32 || !out_buf || frame_count == 0) return -1;

    const uint8_t *pcm_ptr = (const uint8_t *)pcm_stereo_s32;
    int bytes_remaining = (int)(frame_count * 2 /* canais */ * sizeof(int32_t));
    int total_stream_sz = 0;
    int total_frame_num = 0;

    while (bytes_remaining > 0) {
        int pcm_used = 0;
        int stream_sz = 0;
        int frame_num = 0;

        int ret = ldacBT_encode(h->bt, (void *)(uintptr_t)pcm_ptr, &pcm_used,
                                out_buf + total_stream_sz, &stream_sz, &frame_num);
        if (ret != 0) return -1;

        if (pcm_used <= 0 && stream_sz <= 0) {
            // Nenhum dado consumido nem gerado - evita loop infinito
            break;
        }

        if (pcm_used > 0) {
            pcm_ptr += pcm_used;
            bytes_remaining -= pcm_used;
        }

        if (stream_sz > 0) {
            total_stream_sz += stream_sz;
            total_frame_num += frame_num;
            if ((size_t)total_stream_sz > out_buf_cap) return -1;
        }
    }

    if (out_frame_count) *out_frame_count = total_frame_num;
    return total_stream_sz;
}

int ldac_enc_process(ldac_enc_handle_t h, const int16_t *pcm_stereo_s16,
                      uint8_t *out_buf, size_t out_buf_cap, int *out_frame_count)
{
    if (!h || !pcm_stereo_s16 || !out_buf) return -1;

    int frame_samples = ldac_enc_frame_samples(h);
    int total_samples = frame_samples * 2;
    int32_t temp_s32[512]; // max 256 frames * 2 canais
    if (total_samples > 512) return -1;

    for (int i = 0; i < total_samples; i++) {
        temp_s32[i] = ((int32_t)pcm_stereo_s16[i]) << 16;
    }

    return ldac_enc_process_s32(h, temp_s32, (size_t)frame_samples, out_buf, out_buf_cap, out_frame_count);
}

int ldac_enc_last_error(ldac_enc_handle_t h)
{
    if (!h) return LDACBT_ERR_FATAL;
    return ldacBT_get_error_code(h->bt);
}
