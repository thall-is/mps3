#ifndef APTX_ENC_H
#define APTX_ENC_H

// Wrapper em cima do encoder aptX real da Qualcomm (vendorizado em
// vendor/, liberado como parte do AOSP em 2023 - ver
// vendor/PROVENANCE.md pra' detalhes da licenca). aptX classico so'
// suporta 44.1/48kHz, estereo, PCM 16 bits - sem opcao de qualidade
// (taxa fixa, ~352kbps em 44.1kHz).
//
// IMPORTANTE (ver a2dp_media_payload.h): ao contrario de LDAC/SBC/aptX
// HD, o payload A2DP do aptX classico NAO leva cabecalho RTP nenhum -
// e' um stream continuo de codewords crus, com a sincronizacao
// embutida no proprio bitstream do codec (o "auto-sync" que o encoder
// insere sozinho). bt_source_send_media_packet() recebe os bytes
// direto, sem passar por a2dp_media_write_header().

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aptx_enc_s *aptx_enc_handle_t;

// sample_rate: 44100 ou 48000 (os unicos que o mps3 usa). Retorna NULL
// em erro.
aptx_enc_handle_t aptx_enc_open(uint32_t sample_rate);

void aptx_enc_close(aptx_enc_handle_t h);

// Quantos frames (por canal) o encoder consome por "unidade" de
// processamento - sempre 4 (fixo pelo algoritmo do aptX). Chamadas a
// aptx_enc_process() devem passar um multiplo disso.
int aptx_enc_frame_samples(void);

// pcm_stereo_s16: PCM entrelacado (L,R,L,R...), 16 bits, frame_count
// frames (frame_count deve ser multiplo de 4). Processa internamente em
// blocos de 4 amostras, escrevendo 4 bytes por bloco em out_buf
// (compressao fixa 4:1 - out_buf precisa caber frame_count bytes).
// Retorna bytes escritos, ou -1 em erro (frame_count nao multiplo de 4,
// ou buffer pequeno demais).
int aptx_enc_process(aptx_enc_handle_t h, const int16_t *pcm_stereo_s16,
                      size_t frame_count, uint8_t *out_buf, size_t out_buf_cap);

#ifdef __cplusplus
}
#endif

#endif // APTX_ENC_H
