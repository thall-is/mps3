// Teste de integracao de ponta a ponta - o unico teste deste projeto
// que liga TODOS os pedacos reais (nao mocks) que rodam fora do
// ESP-IDF: audio_pipeline (acumulador + extracao) + ldac_enc (o
// encoder de verdade da Sony) + a2dp_media_payload (empacotador RTP).
// So' fica de fora o que exige mesmo o ESP-IDF/hardware: I2S,
// Bluetooth, UART.
//
// Simula exatamente o cenario que main.c enfrenta: uma fonte de PCM de
// 32 bits alinhado a esquerda (formato real confirmado contra
// esp_codec_decoder_adapter.h do projeto principal) entregue em
// leituras de TAMANHO IRREGULAR (como o I2S faria na pratica, nao
// sempre o tamanho exato que o encoder quer), passando pelo mesmo
// acumulador que main.c usa, gerando pacotes A2DP completos.
//
// Uso: ./integration_pipeline_host [--dump saida.bin]
//   --dump: escreve os pacotes A2DP gerados num arquivo binario bruto
//           (cada pacote = 2 bytes de tamanho little-endian + os bytes
//           do pacote) - da' pra' inspecionar depois com um script
//           separado ou so' conferir o tamanho do arquivo.

#include "audio_pipeline.h"
#include "ldac_enc.h"
#include "a2dp_media_payload.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#define SAMPLE_RATE 44100
#define DURATION_SECONDS 2.0
#define TOTAL_FRAMES ((size_t)(SAMPLE_RATE * DURATION_SECONDS))

typedef struct {
    ldac_enc_handle_t enc;
    a2dp_media_stream_state_t stream_state;

    // estatisticas coletadas pra' validar no final
    int packets_emitted;
    long total_ldac_bytes;
    long total_frames_fed;       // toda amostra PCM que chegou ao encoder, tenha ele emitido pacote ou nao neste chunk
    uint16_t last_seq;
    bool have_last_seq;
    int seq_gaps;                // deveria ficar em 0 - cada pacote precisa seguir o anterior
    uint32_t last_timestamp;
    bool have_last_timestamp;
    int timestamp_went_backwards;

    FILE *dump_file;
} pipeline_ctx_t;

static void on_chunk(void *ctx_v, const int32_t *frames, size_t frame_count)
{
    pipeline_ctx_t *ctx = (pipeline_ctx_t *)ctx_v;

    static int16_t pcm16[256 * 2];
    static uint8_t ldac_out[679];
    static uint8_t packet[A2DP_MEDIA_HEADER_LEN + sizeof(ldac_out)];

    audio_pipeline_extract16(frames, frame_count * 2, pcm16);

    int frame_num = 0;
    int n = ldac_enc_process(ctx->enc, pcm16, ldac_out, sizeof(ldac_out), &frame_num);
    if (n <= 0 || frame_num <= 0) {
        // Sem saida imediata neste chunk - NORMAL (o LDAC agrupa varios
        // chunks internamente antes de emitir um transport frame, ver
        // ldac_enc.h e a proporcao ~1/3 observada em ldac_enc_test) -
        // mas o PCM deste chunk foi consumido pelo encoder mesmo assim,
        // entao ainda conta pra' cobertura.
        ctx->total_frames_fed += (long)frame_count;
        return;
    }
    ctx->total_frames_fed += (long)frame_count;

    if (frame_num > 0x0F) { fprintf(stderr, "frame_num absurdo: %d\n", frame_num); exit(1); }

    bool hdr_ok = a2dp_media_write_header(packet, sizeof(packet), &ctx->stream_state,
                                           (uint8_t)frame_num, (uint32_t)frame_count);
    assert(hdr_ok);
    memcpy(packet + A2DP_MEDIA_HEADER_LEN, ldac_out, (size_t)n);
    size_t packet_len = A2DP_MEDIA_HEADER_LEN + (size_t)n;

    // --- validacoes de sanidade em cada pacote ---
    assert(packet_len <= 679); // nunca deve estourar o MTU minimo garantido do LDAC

    uint16_t seq = (uint16_t)((packet[2] << 8) | packet[3]);
    if (ctx->have_last_seq) {
        uint16_t expected = (uint16_t)(ctx->last_seq + 1); // wraparound de uint16 e' esperado e correto
        if (seq != expected) ctx->seq_gaps++;
    }
    ctx->last_seq = seq;
    ctx->have_last_seq = true;

    uint32_t timestamp = ((uint32_t)packet[4] << 24) | ((uint32_t)packet[5] << 16) |
                          ((uint32_t)packet[6] << 8) | packet[7];
    if (ctx->have_last_timestamp && timestamp < ctx->last_timestamp) {
        ctx->timestamp_went_backwards++;
    }
    ctx->last_timestamp = timestamp;
    ctx->have_last_timestamp = true;

    ctx->packets_emitted++;
    ctx->total_ldac_bytes += n;

    if (ctx->dump_file) {
        uint16_t len16 = (uint16_t)packet_len;
        fwrite(&len16, sizeof(len16), 1, ctx->dump_file);
        fwrite(packet, 1, packet_len, ctx->dump_file);
    }
}

// Gera PCM de 32 bits ALINHADO A ESQUERDA (o mesmo formato que a placa
// principal manda de verdade - ver comentario em audio_pipeline.h) para
// um tom senoidal simples, estereo.
static void fill_synthetic_pcm(int32_t *out, size_t frame_count, double *phase, double freq_hz)
{
    for (size_t i = 0; i < frame_count; i++) {
        int16_t s = (int16_t)(sin(*phase) * 12000.0);
        int32_t left_justified = ((int32_t)s) << 16; // exatamente a convencao confirmada
        out[i * 2]     = left_justified;
        out[i * 2 + 1] = left_justified;
        *phase += 2.0 * M_PI * freq_hz / SAMPLE_RATE;
    }
}

int main(int argc, char **argv)
{
    FILE *dump_file = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            dump_file = fopen(argv[i + 1], "wb");
            if (!dump_file) { perror("fopen"); return 1; }
        }
    }

    pipeline_ctx_t ctx = {0};
    ctx.dump_file = dump_file;
    ctx.enc = ldac_enc_open(679, SAMPLE_RATE, LDAC_ENC_QUALITY_STANDARD);
    if (!ctx.enc) { fprintf(stderr, "FALHA: ldac_enc_open\n"); return 1; }

    int32_t chunk_backing[128 * 2];
    audio_pipeline_acc_t acc;
    audio_pipeline_acc_init(&acc, chunk_backing, 128);

    // Simula leituras de I2S IRREGULARES (nao multiplas de 128) por
    // toda a duracao do teste - exatamente o padrao que
    // i2s_input_read_frames(..., 64, ...) produziria na pratica (pedido
    // de 64, mas pode vir menos por timeout/DMA parcial). Alterna entre
    // varios tamanhos de leitura de proposito.
    size_t read_sizes[] = {64, 17, 64, 200, 3, 64, 91};
    size_t read_idx = 0;
    double phase = 0.0;
    size_t frames_generated = 0;

    int32_t scratch[512];

    while (frames_generated < TOTAL_FRAMES) {
        size_t n = read_sizes[read_idx % (sizeof(read_sizes) / sizeof(read_sizes[0]))];
        read_idx++;
        if (frames_generated + n > TOTAL_FRAMES) n = TOTAL_FRAMES - frames_generated;
        if (n == 0) break;

        fill_synthetic_pcm(scratch, n, &phase, 1000.0);
        audio_pipeline_acc_feed(&acc, scratch, n, on_chunk, &ctx);
        frames_generated += n;
    }

    ldac_enc_close(ctx.enc);
    if (dump_file) fclose(dump_file);

    // --- relatorio ---
    double duration_s = (double)frames_generated / SAMPLE_RATE;
    double kbps = (ctx.total_ldac_bytes * 8.0 / 1000.0) / duration_s;
    double coverage = (double)ctx.total_frames_fed / (double)frames_generated;

    printf("frames PCM gerados:        %zu (%.2fs a %dHz)\n", frames_generated, duration_s, SAMPLE_RATE);
    printf("pacotes A2DP emitidos:     %d\n", ctx.packets_emitted);
    printf("bytes LDAC totais:         %ld\n", ctx.total_ldac_bytes);
    printf("taxa efetiva:              %.1f kbps\n", kbps);
    printf("cobertura (frames que chegaram ao encoder / gerados): %.1f%%\n", coverage * 100.0);
    printf("furos na sequencia RTP:    %d (esperado: 0)\n", ctx.seq_gaps);
    printf("timestamp andou p/ tras:   %d vezes (esperado: 0)\n", ctx.timestamp_went_backwards);

    // --- veredito ---
    int fail = 0;
    if (ctx.packets_emitted < 50) { printf("FALHA: poucos pacotes emitidos\n"); fail = 1; }
    if (kbps < 400 || kbps > 900) { printf("FALHA: taxa fora do esperado pra Standard Quality\n"); fail = 1; }
    if (coverage < 0.95) { printf("FALHA: cobertura baixa demais - amostras sendo perdidas em algum lugar\n"); fail = 1; }
    if (ctx.seq_gaps != 0) { printf("FALHA: sequence number RTP com furo\n"); fail = 1; }
    if (ctx.timestamp_went_backwards != 0) { printf("FALHA: timestamp RTP nao monotonico\n"); fail = 1; }

    if (fail) { printf("\nRESULTADO: FALHOU\n"); return 1; }
    printf("\nRESULTADO: OK\n");
    return 0;
}
