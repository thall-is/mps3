#include "audio_pipeline.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

// --- teste 1: extract16 ------------------------------------------------
static void test_extract16(void)
{
    // Valores desenhados pra' cobrir: positivo grande, negativo grande,
    // zero, e um valor tipico de amostra de 16 bits ja' alinhada a
    // esquerda (confirma que >>16 recupera o valor original exato pro
    // caso de fonte de 16 bits, que e' o caso mais comum/facil de
    // verificar objetivamente).
    int32_t raw[] = {
        (int32_t)0x7FFF0000,  // +32767 alinhado a esquerda
        (int32_t)0x80000000,  // -32768 alinhado a esquerda
        0x00000000,           // silencio
        (int32_t)0x12340000,  // 0x1234 = 4660, alinhado a esquerda
        (int32_t)0xFFFF0000,  // -1 (16 bits) alinhado a esquerda
    };
    int16_t out[5];
    audio_pipeline_extract16(raw, 5, out);

    assert(out[0] == 32767);
    assert(out[1] == -32768);
    assert(out[2] == 0);
    assert(out[3] == 4660);
    assert(out[4] == -1);

    printf("test_extract16: OK\n");
}

// --- teste 2: acumulador com leituras alinhadas -------------------------
typedef struct {
    int chunks_seen;
    int32_t last_chunk_first_sample;
    int32_t last_chunk_last_sample;
} acc_test_ctx_t;

static void on_chunk(void *ctx_v, const int32_t *frames, size_t frame_count)
{
    acc_test_ctx_t *ctx = (acc_test_ctx_t *)ctx_v;
    ctx->chunks_seen++;
    ctx->last_chunk_first_sample = frames[0];
    ctx->last_chunk_last_sample = frames[(frame_count - 1) * 2];
}

static void test_acc_exact_chunks(void)
{
    int32_t backing[128 * 2];
    audio_pipeline_acc_t acc;
    audio_pipeline_acc_init(&acc, backing, 128);

    acc_test_ctx_t ctx = {0};

    // 3 chunks exatos de 128 frames cada, alimentados de uma vez -
    // confirma que uma unica chamada gerando varios chunks completos
    // funciona (nao so' o caso "1 leitura = 1 chunk").
    int32_t big[128 * 3 * 2];
    for (size_t f = 0; f < 128 * 3; f++) {
        big[f * 2]     = (int32_t)(f * 1000);      // canal L: valor previsivel
        big[f * 2 + 1] = (int32_t)(f * 1000 + 1);  // canal R
    }

    audio_pipeline_acc_feed(&acc, big, 128 * 3, on_chunk, &ctx);

    assert(ctx.chunks_seen == 3);
    // ultimo chunk (frames 256..383): primeiro frame L = 256*1000
    assert(ctx.last_chunk_first_sample == (int32_t)(256 * 1000));
    // ultimo frame do ultimo chunk (frame 383) L = 383*1000
    assert(ctx.last_chunk_last_sample == (int32_t)(383 * 1000));
    assert(acc.filled_frames == 0); // nada sobrando

    printf("test_acc_exact_chunks: OK (%d chunks)\n", ctx.chunks_seen);
}

// --- teste 3: leituras IRREGULARES, desalinhadas com o tamanho do chunk
static void test_acc_irregular_reads(void)
{
    int32_t backing[100 * 2];
    audio_pipeline_acc_t acc;
    audio_pipeline_acc_init(&acc, backing, 100); // chunk de 100 frames pra facilitar a conta

    acc_test_ctx_t ctx = {0};

    // Sequencia de leituras de tamanhos bem irregulares: 1, 37, 200, 50,
    // 12 frames = 300 frames no total = exatamente 3 chunks de 100. Os
    // tamanhos NAO sao multiplos nem divisores de 100 de proposito -
    // e' o cenario real de um I2S entregando quantidades que nao batem
    // com o tamanho do chunk do encoder.
    size_t reads[] = {1, 37, 200, 50, 12};
    int32_t scratch[512];
    int32_t frame_counter = 0;

    for (size_t r = 0; r < sizeof(reads) / sizeof(reads[0]); r++) {
        size_t n = reads[r];
        for (size_t f = 0; f < n; f++) {
            scratch[f * 2]     = frame_counter;      // valor = indice do frame, sequencial e unico
            scratch[f * 2 + 1] = -frame_counter;
            frame_counter++;
        }
        audio_pipeline_acc_feed(&acc, scratch, n, on_chunk, &ctx);
    }

    assert(frame_counter == 300);
    assert(ctx.chunks_seen == 3);
    // 3o chunk (frames 200..299): primeiro frame L deve ser 200
    assert(ctx.last_chunk_first_sample == 200);
    // ultimo frame do 3o chunk (frame 299) L deve ser 299
    assert(ctx.last_chunk_last_sample == 299);
    assert(acc.filled_frames == 0);

    printf("test_acc_irregular_reads: OK (%d chunks, sem perda de amostra)\n", ctx.chunks_seen);
}

// --- teste 4: leituras que NAO fecham chunk nenhum (menos que a capacidade)
static void test_acc_partial_no_chunk_yet(void)
{
    int32_t backing[128 * 2];
    audio_pipeline_acc_t acc;
    audio_pipeline_acc_init(&acc, backing, 128);
    acc_test_ctx_t ctx = {0};

    int32_t scratch[64 * 2];
    memset(scratch, 0, sizeof(scratch));
    audio_pipeline_acc_feed(&acc, scratch, 64, on_chunk, &ctx); // so' metade do chunk

    assert(ctx.chunks_seen == 0);           // nao deveria ter disparado nada ainda
    assert(acc.filled_frames == 64);        // os 64 frames continuam guardados, nao foram perdidos

    printf("test_acc_partial_no_chunk_yet: OK (nada perdido, esperando completar)\n");
}

int main(void)
{
    test_extract16();
    test_acc_exact_chunks();
    test_acc_irregular_reads();
    test_acc_partial_no_chunk_yet();
    printf("\nTODOS OS TESTES PASSARAM\n");
    return 0;
}
