#ifndef APTX_HD_ENC_H
#define APTX_HD_ENC_H

// Wrapper em cima do encoder aptX HD real da Qualcomm (vendorizado em
// vendor/, mesma liberacao Apache 2.0 do aptX classico - ver
// vendor/PROVENANCE.md). 44.1/48kHz, estereo, PCM 24 bits, taxa fixa
// (~576kbps em 44.1kHz).
//
// RESSALVA DE PRECISAO: o pipeline deste projeto (audio_pipeline.c)
// hoje so' extrai 16 bits de cada amostra vinda da placa principal
// (ver audio_pipeline.h). Este wrapper aceita PCM 16 bits e o alarga
// pra' 24 bits preenchendo os 8 bits menos significativos com zero -
// produz um bitstream aptX HD valido e interoperavel, mas SEM o ganho
// real de faixa dinamica que o HD e' capaz de entregar com uma fonte
// de 24 bits de verdade. Extrair os 24 bits completos exigiria mudar
// audio_pipeline_extract16() (ou adicionar um extract24 paralelo) -
// nao feito ainda, listado como melhoria futura no README.
//
// Formato de saida confirmado contra o consumidor real do AOSP
// (a2dp_vendor_aptx_hd_encoder.cc, aptx_hd_encode_24bit()): RTP header
// padrao de 12 bytes (ver a2dp_media_write_header - mas SEM o byte de
// frame_count que SBC/LDAC usam) seguido de 3 bytes big-endian por
// canal (L depois R).

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aptx_hd_enc_s *aptx_hd_enc_handle_t;

aptx_hd_enc_handle_t aptx_hd_enc_open(uint32_t sample_rate);
void aptx_hd_enc_close(aptx_hd_enc_handle_t h);

int aptx_hd_enc_frame_samples(void);

// pcm_stereo_s16: PCM entrelacado 16 bits, frame_count multiplo de 4.
// Escreve 6 bytes por grupo de 4 frames (compressao 4:1, 24 bits por
// amostra - ver aptx_hd_enc.h). Retorna bytes escritos, ou -1 em erro.
int aptx_hd_enc_process(aptx_hd_enc_handle_t h, const int16_t *pcm_stereo_s16,
                         size_t frame_count, uint8_t *out_buf, size_t out_buf_cap);

// pcm_stereo_s32: PCM entrelacado 32 bits (com audio 24 bits nos bits 31..8).
// Desloca >> 8 para produzir o formato Q23 exato do encoder Qualcomm, entregando 24 bits nativos.
int aptx_hd_enc_process_s32(aptx_hd_enc_handle_t h, const int32_t *pcm_stereo_s32,
                             size_t frame_count, uint8_t *out_buf, size_t out_buf_cap);

#ifdef __cplusplus
}
#endif

#endif // APTX_HD_ENC_H
