#ifndef SBC_ENC_H
#define SBC_ENC_H

// Wrapper de alto nivel em cima do encoder SBC real do Google
// (google/libsbc, Apache 2.0, vendorizado em vendor/ - ver
// sbc_enc/CMakeLists.txt). SBC e' o fallback obrigatorio do A2DP: todo
// receptor Bluetooth com A2DP tem que suportar SBC, entao registrar um
// SEP em SBC (alem do LDAC) garante que o companheiro consegue
// conectar em QUALQUER fone/caixa, mesmo sem suporte a LDAC - ver
// bt_source.c, LDAC continua com prioridade (SEID menor) quando ambos
// sao suportados pelo peer.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sbc_enc_s *sbc_enc_handle_t;

// sample_rate: 44100 ou 48000 (os unicos que o mps3 de fato usa - ver
// README). bitpool: controla a taxa/qualidade (ver
// SBC_BITPOOL_MEDIUM/HIGH abaixo) - valores tipicos vao de ~20
// (qualidade baixa) a 53 (qualidade maxima em 44.1kHz estereo, joint
// stereo, 8 subbandas, 16 blocos - a configuracao "padrao de
// referencia" do proprio A2DP spec). Retorna NULL em erro.
sbc_enc_handle_t sbc_enc_open(uint32_t sample_rate, int bitpool);

void sbc_enc_close(sbc_enc_handle_t h);

// bitpool tipico pra' qualidade alta em 44.1/48kHz estereo joint-stereo
// (mesma config de referencia usada pelo proprio A2DP spec / BlueZ por
// padrao).
#define SBC_ENC_BITPOOL_HIGH 53

// Quantos frames (por canal) o encoder consome por chamada de
// sbc_enc_process() - fixo em 128 (16 blocos x 8 subbandas), a
// configuracao que este wrapper sempre usa.
int sbc_enc_frame_samples(sbc_enc_handle_t h);

// pcm_stereo_s16: PCM entrelacado (L,R,L,R...), 16 bits,
// sbc_enc_frame_samples() frames. out_buf/out_buf_cap: buffer de saida
// (um frame SBC nessa config fica em torno de 116 bytes - 256 cobre
// folgado). Retorna bytes escritos, ou -1 em erro.
int sbc_enc_process(sbc_enc_handle_t h, const int16_t *pcm_stereo_s16,
                     uint8_t *out_buf, size_t out_buf_cap);

#ifdef __cplusplus
}
#endif

#endif // SBC_ENC_H
