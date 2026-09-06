#include "sbc_enc.h"
#include <stdio.h>
#include <math.h>

int main(void)
{
    sbc_enc_handle_t h = sbc_enc_open(44100, SBC_ENC_BITPOOL_HIGH);
    if (!h) { printf("FALHA: sbc_enc_open retornou NULL\n"); return 1; }

    int frame_samples = sbc_enc_frame_samples(h);
    printf("frame_samples = %d (esperado 128 = 16 blocos * 8 subbandas)\n", frame_samples);
    if (frame_samples != 128) { printf("FALHA\n"); return 1; }

    int16_t pcm[128 * 2];
    uint8_t out[256];
    long total_bytes = 0;
    int frames_ok = 0;
    double phase = 0.0;

    // 200 frames de um tom de 1kHz estereo - o suficiente pra' varios
    // ciclos, igual ao teste do LDAC.
    for (int f = 0; f < 200; f++) {
        for (int i = 0; i < frame_samples; i++) {
            int16_t s = (int16_t)(sin(phase) * 12000.0);
            pcm[i * 2 + 0] = s;
            pcm[i * 2 + 1] = s;
            phase += 2.0 * M_PI * 1000.0 / 44100.0;
        }
        int n = sbc_enc_process(h, pcm, out, sizeof(out));
        if (n <= 0) { printf("erro no frame %d\n", f); continue; }
        total_bytes += n;
        frames_ok++;
    }

    printf("frames codificados: %d / 200\n", frames_ok);
    printf("bytes totais: %ld\n", total_bytes);
    double seconds = 200.0 * frame_samples / 44100.0;
    double kbps = (total_bytes * 8.0 / 1000.0) / seconds;
    printf("taxa efetiva: %.1f kbps\n", kbps);

    sbc_enc_close(h);

    // SBC com bitpool 53 em 44.1kHz joint-stereo fica tipicamente em
    // torno de 320kbps (o teto pratico do SBC de alta qualidade) -
    // faixa generosa pra' nao acoplar o teste a um numero exato.
    if (frames_ok != 200) { printf("FALHA: nem todo frame produziu saida\n"); return 1; }
    if (kbps < 250 || kbps > 400) { printf("FALHA: taxa fora do esperado\n"); return 1; }

    printf("OK\n");
    return 0;
}
