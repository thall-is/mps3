#ifndef AUDIO_PIPELINE_H
#define AUDIO_PIPELINE_H

// Modulo puro (sem NENHUMA dependencia de ESP-IDF/FreeRTOS) com a parte
// do pipeline de audio que da' pra testar de verdade no host: extracao
// de 32->16 bits e o acumulador que junta leituras de I2S de tamanho
// variavel/irregular em chunks completos do tamanho que o encoder LDAC
// espera. Feito separado de main.c exatamente pra' isso - poder rodar
// com gcc puro em vez de precisar do toolchain Xtensa (ver
// test_audio_pipeline_host.c).

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Extrai os 16 bits mais significativos de cada amostra de 32 bits
// (formato PCM estereo entrelacado, L,R,L,R...). Confirmado contra o
// codigo real da placa principal (esp_codec_decoder_adapter.h,
// drain_pending()) que TODA amostra - 16, 24 ou 32 bits de origem - sai
// alinhada a esquerda na palavra de 32 bits antes do I2S; entao os 16
// bits mais significativos sempre contem os 16 bits mais significativos
// da amostra original, seja qual for a profundidade de bits do arquivo
// tocado. out precisa caber sample_count int16_t (sample_count =
// frame_count*2 pra estereo).
void audio_pipeline_extract16(const int32_t *raw, size_t sample_count, int16_t *out);

// Extrai amostras em formato Q23 (24 bits alinhados a direita no int32 com sinal)
// a partir do stream I2S de 32 bits onde o audio de 24 bits esta' alinhado a esquerda.
void audio_pipeline_extract24_q23(const int32_t *raw, size_t sample_count, int32_t *out);

// Acumulador de frames PCM (int32, estereo entrelacado) - junta
// leituras de tamanho arbitrario (o I2S pode entregar menos que o
// pedido, por timeout ou DMA parcial) ate' ter exatamente
// capacity_frames acumulados, momento em que chama on_full_chunk() com
// o buffer cheio e reseta pra' zero. Se uma unica leitura trouxer mais
// frames do que cabe pra completar o chunk atual, o excesso vira o
// inicio do proximo chunk (nao descarta nada, mesmo com leituras
// desalinhadas com o tamanho do chunk).
typedef struct {
    int32_t *buf;            // capacidade: capacity_frames*2 int32 (fornecido pelo chamador)
    size_t   capacity_frames;
    size_t   filled_frames;
} audio_pipeline_acc_t;

void audio_pipeline_acc_init(audio_pipeline_acc_t *acc, int32_t *backing_buf, size_t capacity_frames);

typedef void (*audio_pipeline_chunk_cb_t)(void *ctx, const int32_t *frames, size_t frame_count);

// Alimenta o acumulador com frame_count frames novos (raw). Chama
// on_full_chunk() uma vez pra cada chunk completo formado (pode ser
// mais de uma vez numa unica chamada, se raw trouxer frames suficientes
// pra' varios chunks de uma vez - ex: depois de um timeout longo).
void audio_pipeline_acc_feed(audio_pipeline_acc_t *acc, const int32_t *raw, size_t frame_count,
                              audio_pipeline_chunk_cb_t on_full_chunk, void *ctx);

// Reamostrador estereo por interpolacao linear em ponto fixo 32.32
// Converte streams PCM estereo (int32 entrelacado) entre taxas arbitrarias
// (ex: 48000 -> 44100, 44100 -> 48000, 96000 -> 44100).
typedef struct {
    uint32_t in_rate;
    uint32_t out_rate;
    uint64_t phase;
    uint64_t step;
    int32_t  carry_l;
    int32_t  carry_r;
    bool     has_carry;
} audio_pipeline_resampler_t;

void audio_pipeline_resampler_init(audio_pipeline_resampler_t *r, uint32_t in_rate, uint32_t out_rate);
size_t audio_pipeline_resample(audio_pipeline_resampler_t *r, const int32_t *in, size_t in_frames,
                               int32_t *out, size_t out_capacity);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_PIPELINE_H
