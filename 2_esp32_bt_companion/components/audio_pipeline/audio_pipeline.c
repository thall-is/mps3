#include "audio_pipeline.h"
#include <string.h>

void audio_pipeline_extract16(const int32_t *raw, size_t sample_count, int16_t *out)
{
    for (size_t i = 0; i < sample_count; i++) {
        out[i] = (int16_t)(raw[i] >> 16);
    }
}

void audio_pipeline_extract24_q23(const int32_t *raw, size_t sample_count, int32_t *out)
{
    for (size_t i = 0; i < sample_count; i++) {
        out[i] = raw[i] >> 8;
    }
}

void audio_pipeline_acc_init(audio_pipeline_acc_t *acc, int32_t *backing_buf, size_t capacity_frames)
{
    acc->buf = backing_buf;
    acc->capacity_frames = capacity_frames;
    acc->filled_frames = 0;
}

void audio_pipeline_acc_feed(audio_pipeline_acc_t *acc, const int32_t *raw, size_t frame_count,
                              audio_pipeline_chunk_cb_t on_full_chunk, void *ctx)
{
    size_t raw_offset = 0;

    while (frame_count > 0) {
        size_t space_left = acc->capacity_frames - acc->filled_frames;
        size_t take = (frame_count < space_left) ? frame_count : space_left;

        // buf e' estereo entrelacado -> 2 int32 por frame
        memcpy(&acc->buf[acc->filled_frames * 2], &raw[raw_offset * 2], take * 2 * sizeof(int32_t));

        acc->filled_frames += take;
        raw_offset += take;
        frame_count -= take;

        if (acc->filled_frames == acc->capacity_frames) {
            if (on_full_chunk) on_full_chunk(ctx, acc->buf, acc->capacity_frames);
            acc->filled_frames = 0;
        }
    }
}

void audio_pipeline_resampler_init(audio_pipeline_resampler_t *r, uint32_t in_rate, uint32_t out_rate)
{
    if (!r) return;
    r->in_rate = in_rate;
    r->out_rate = out_rate;
    r->phase = 0;
    r->carry_l = 0;
    r->carry_r = 0;
    r->has_carry = false;
    if (in_rate > 0 && out_rate > 0) {
        r->step = ((uint64_t)in_rate << 32) / out_rate;
    } else {
        r->step = 1ULL << 32;
    }
}

size_t audio_pipeline_resample(audio_pipeline_resampler_t *r, const int32_t *in, size_t in_frames,
                               int32_t *out, size_t out_capacity)
{
    if (!r || !in || !out || in_frames == 0 || out_capacity == 0) return 0;

    if (r->in_rate == r->out_rate || r->in_rate == 0 || r->out_rate == 0) {
        size_t take = (in_frames < out_capacity) ? in_frames : out_capacity;
        memcpy(out, in, take * 2 * sizeof(int32_t));
        return take;
    }

    if (!r->has_carry) {
        r->carry_l = in[0];
        r->carry_r = in[1];
        r->has_carry = true;
    }

    // Buffer local: [0] = carry do bloco anterior, [1..in_frames] = frames atuais
    int32_t stack_buf[1026];
    if (in_frames > 512) {
        return 0;
    }

    stack_buf[0] = r->carry_l;
    stack_buf[1] = r->carry_r;
    memcpy(&stack_buf[2], in, in_frames * 2 * sizeof(int32_t));
    size_t total_frames = in_frames + 1;

    size_t out_frames = 0;
    uint64_t phase = r->phase;
    uint64_t step = r->step;

    while (out_frames < out_capacity) {
        size_t k = (size_t)(phase >> 32);
        if (k >= total_frames - 1) {
            break;
        }

        int32_t p0_l = stack_buf[k * 2];
        int32_t p0_r = stack_buf[k * 2 + 1];
        int32_t p1_l = stack_buf[(k + 1) * 2];
        int32_t p1_r = stack_buf[(k + 1) * 2 + 1];

        int64_t frac = (int64_t)((phase & 0xFFFFFFFFULL) >> 16); // [0..65535]
        int32_t out_l = p0_l + (int32_t)(((int64_t)(p1_l - p0_l) * frac) >> 16);
        int32_t out_r = p0_r + (int32_t)(((int64_t)(p1_r - p0_r) * frac) >> 16);

        out[out_frames * 2]     = out_l;
        out[out_frames * 2 + 1] = out_r;
        out_frames++;

        phase += step;
    }

    size_t k_consumed = total_frames - 1;
    r->carry_l = stack_buf[k_consumed * 2];
    r->carry_r = stack_buf[k_consumed * 2 + 1];
    phase -= ((uint64_t)k_consumed << 32);
    r->phase = phase;

    return out_frames;
}
