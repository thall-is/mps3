#ifndef LDAC_ENC_H
#define LDAC_ENC_H

// Wrapper de alto nivel em cima do encoder LDAC real da Sony (vendorizado
// em vendor/, Apache 2.0 - ver ldac_enc/CMakeLists.txt). Esta camada e'
// codigo novo deste projeto; ela nao mexe no algoritmo de compressao em
// si, so' adapta a API do libldac (pensada pra Android/telefone) ao
// nosso pipeline: um frame de PCM entra, um "ldac_transport_frame" (o
// payload que vai dentro do pacote A2DP) sai.
//
// STATUS: Totalmente integrado ao pipeline de áudio do mps3. O codificador
// recebe amostras de 32 bits vindas do barramento I2S Slave, preserva a
// dinâmica nativa de 24 bits em Q30 (ldac_enc_process_s32) e despacha os
// pacotes de mídia diretamente para a pilha Bluedroid (via patch Vendor SEP),
// operando em até 990 kbps com 24-bit / 96 kHz comprovados em bancada.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LDAC_ENC_QUALITY_HIGH = 0,   // ~909 kbps @44.1k / ~990 kbps @48k
    LDAC_ENC_QUALITY_STANDARD,   // ~606 kbps @44.1k / ~660 kbps @48k
    LDAC_ENC_QUALITY_MOBILE,     // ~303 kbps @44.1k / ~330 kbps @48k
} ldac_enc_quality_t;

typedef struct ldac_enc_s *ldac_enc_handle_t;

// mtu: tamanho maximo do payload L2CAP por pacote (o A2DP tipicamente usa
// 679 bytes - o valor minimo de MTU que o LDAC exige, ver nota no header
// original). sample_rate: 44100, 48000, 88200 ou 96000 - os unicos que o
// LDAC aceita. Retorna NULL em caso de falha (parametro invalido ou sem
// memoria).
ldac_enc_handle_t ldac_enc_open(int mtu, uint32_t sample_rate, ldac_enc_quality_t quality);

void ldac_enc_close(ldac_enc_handle_t h);

// Ajusta a qualidade em um handle ja aberto (ex: caindo pra MOBILE se a
// conexao BT estiver com muita perda de pacote - controle de taxa
// adaptativo, ver tambem abr/ na fonte vendorizada pra uma logica pronta
// disso caso valha a pena portar depois).
bool ldac_enc_set_quality(ldac_enc_handle_t h, ldac_enc_quality_t quality);

// Quantos samples (por canal) o encoder espera por chamada de
// ldac_enc_process(), dado a taxa de amostragem do handle. E' 128 pra
// 44.1/48kHz e 256 pra 88.2/96kHz - ver tabela no header original.
// Chame isso pra saber quanto PCM juntar antes de cada ldac_enc_process().
int ldac_enc_frame_samples(ldac_enc_handle_t h);

// pcm_stereo_s16: PCM entrelacado (L,R,L,R...), 16 bits, o numero de
// frames retornado por ldac_enc_frame_samples(). out_buf/out_buf_cap: buffer
// de saida fornecido pelo chamador (679 bytes cobre folgado um
// ldac_transport_frame). out_frame_count (pode ser NULL): recebe quantos
// frames LDAC foram efetivamente produzidos nesta chamada - necessario
// pra preencher o campo frame_count do cabecalho RTP do A2DP (ver
// bt_source.c) quando for de fato montar o pacote pra transmitir; sem
// isso o receptor nao sabe quantos frames vem no payload. Retorna o
// numero de bytes escritos em out_buf, ou -1 em erro (consulte
// ldac_enc_last_error()).
int ldac_enc_process(ldac_enc_handle_t h, const int16_t *pcm_stereo_s16,
                      uint8_t *out_buf, size_t out_buf_cap, int *out_frame_count);

// pcm_stereo_s32: PCM entrelacado (L,R,L,R...), 32 bits (onde 24 bits
// ocupam os bits mais significativos, left-justified como no I2S nativo).
// Permite transmissao de 24 bits reais sem truncamento para 16 bits.
int ldac_enc_process_s32(ldac_enc_handle_t h, const int32_t *pcm_stereo_s32, size_t frame_count,
                         uint8_t *out_buf, size_t out_buf_cap, int *out_frame_count);

int ldac_enc_last_error(ldac_enc_handle_t h);

#ifdef __cplusplus
}
#endif

#endif // LDAC_ENC_H
