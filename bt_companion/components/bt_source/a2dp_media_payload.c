#include "a2dp_media_payload.h"

static bool write_rtp_header(uint8_t *out, size_t out_cap,
                              a2dp_media_stream_state_t *state,
                              uint32_t samples_in_packet)
{
    if (!out || !state || out_cap < A2DP_RTP_HEADER_LEN) return false;

    out[0] = 0x80; // V=2, P=0, X=0, CC=0  ->  1000 0000
    out[1] = 0x60; // M=0, PT=96 (0x60)    ->  0110 0000

    out[2] = (uint8_t)(state->seq_num >> 8);
    out[3] = (uint8_t)(state->seq_num & 0xFF);

    out[4] = (uint8_t)(state->timestamp >> 24);
    out[5] = (uint8_t)(state->timestamp >> 16);
    out[6] = (uint8_t)(state->timestamp >> 8);
    out[7] = (uint8_t)(state->timestamp & 0xFF);

    uint32_t ssrc = 1; // valor fixo - so' precisa ser consistente durante a sessao, nao unico globalmente
    out[8]  = (uint8_t)(ssrc >> 24);
    out[9]  = (uint8_t)(ssrc >> 16);
    out[10] = (uint8_t)(ssrc >> 8);
    out[11] = (uint8_t)(ssrc & 0xFF);

    state->seq_num++;
    state->timestamp += samples_in_packet;

    return true;
}

bool a2dp_media_write_header(uint8_t *out, size_t out_cap,
                              a2dp_media_stream_state_t *state,
                              uint8_t frame_count, uint32_t samples_in_packet)
{
    if (out_cap < A2DP_MEDIA_HEADER_LEN) return false;
    if (frame_count == 0 || frame_count > 0x0F) return false;

    // Escreve o RTP primeiro SEM avancar seq_num/timestamp ainda (usa
    // um estado temporario) - so' confirma o avanco depois de validar
    // frame_count, pra' nao inconsistir o estado se a chamada falhar
    // por um frame_count invalido.
    a2dp_media_stream_state_t tmp = *state;
    if (!write_rtp_header(out, out_cap, &tmp, samples_in_packet)) return false;

    // --- cabecalho de payload especifico do A2DP (1 byte) ---
    // A2DP spec (SBC / LDAC): bits[7:4] sao flags (F, S, L, RFA = 0 quando nao fragmentado),
    // bits[3:0] e' o numero de frames no pacote (frame_count).
    out[12] = (uint8_t)(frame_count & 0x0F);

    *state = tmp;
    return true;
}

bool a2dp_media_write_rtp_header_only(uint8_t *out, size_t out_cap,
                                       a2dp_media_stream_state_t *state,
                                       uint32_t samples_in_packet)
{
    return write_rtp_header(out, out_cap, state, samples_in_packet);
}
