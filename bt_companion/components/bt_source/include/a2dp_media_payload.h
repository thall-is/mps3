#ifndef A2DP_MEDIA_PAYLOAD_H
#define A2DP_MEDIA_PAYLOAD_H

// Monta o payload de midia do A2DP em cima de frames ja codificados.
// NEM TODO CODEC usa o mesmo cabecalho - confirmado contra o
// consumidor real do AOSP pra' cada um (ver comentario em cada
// funcao abaixo):
//
//   SBC, LDAC        -> a2dp_media_write_header()          (RTP + 1 byte de frame_count)
//   aptX HD          -> a2dp_media_write_rtp_header_only() (so' RTP, sem frame_count)
//   aptX (classico)  -> NENHUM cabecalho - manda os bytes do codec direto
//                        (ver aptx_enc.h pra' entender por que: a
//                        sincronizacao vem embutida no proprio
//                        bitstream do codec, nao numa camada de
//                        framing externa)
//
// Formato do cabecalho RTP (12 bytes, RFC 3550 simplificado) e' o
// mesmo nos dois casos que o usam:
//   byte0: V(2 bits)=2, P(1)=0, X(1)=0, CC(4)=0
//   byte1: M(1)=0, PT(7)=96 (dynamic payload type)
//   bytes2-3:  sequence_number (big-endian, incrementa 1 por pacote)
//   bytes4-7:  timestamp (big-endian, incrementa pelo numero de
//              amostras PCM que geraram este pacote)
//   bytes8-11: SSRC (fixo, qualquer valor - 1 aqui, seguindo o exemplo
//              do BlueZ)
//
// No caso SBC/LDAC, mais 1 byte depois do RTP (A2DP Spec):
//   bits[7:4]: flags de fragmentacao (F, S, L, RFA - sempre 0 quando nao fragmentado)
//   bits[3:0]: frame_count (quantos frames do codec vem a seguir)

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define A2DP_RTP_HEADER_LEN   12
#define A2DP_MEDIA_HEADER_LEN 13 // 12 (RTP) + 1 (payload header especifico do A2DP) - so' SBC/LDAC

typedef struct {
    uint16_t seq_num;    // estado do chamador - incrementar a cada pacote enviado
    uint32_t timestamp;  // estado do chamador - acumular amostras PCM enviadas
} a2dp_media_stream_state_t;

// Pra SBC e LDAC: RTP (12 bytes) + 1 byte de frame_count. frame_count
// precisa caber em 4 bits (1-15).
bool a2dp_media_write_header(uint8_t *out, size_t out_cap,
                              a2dp_media_stream_state_t *state,
                              uint8_t frame_count, uint32_t samples_in_packet);

// Pra aptX HD: so' o cabecalho RTP (12 bytes), sem byte de frame_count
// - o consumidor real do AOSP nao usa esse campo pra aptX/aptX HD (ver
// a2dp_vendor_aptx_hd_encoder.cc).
bool a2dp_media_write_rtp_header_only(uint8_t *out, size_t out_cap,
                                       a2dp_media_stream_state_t *state,
                                       uint32_t samples_in_packet);

#ifdef __cplusplus
}
#endif

#endif // A2DP_MEDIA_PAYLOAD_H
