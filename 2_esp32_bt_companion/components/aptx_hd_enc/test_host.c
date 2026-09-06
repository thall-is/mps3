#include "aptx_hd_enc.h"
#include <stdio.h>
#include <math.h>

int main(void)
{
    aptx_hd_enc_handle_t h = aptx_hd_enc_open(44100);
    if (!h) { printf("FALHA: aptx_hd_enc_open retornou NULL\n"); return 1; }

    int16_t pcm[4 * 2];
    uint8_t out[6];
    long total_bytes = 0;
    int groups_ok = 0;
    double phase = 0.0;

    for (int g = 0; g < 200; g++) {
        for (int i = 0; i < 4; i++) {
            int16_t s = (int16_t)(sin(phase) * 12000.0);
            pcm[i * 2 + 0] = s;
            pcm[i * 2 + 1] = s;
            phase += 2.0 * M_PI * 1000.0 / 44100.0;
        }
        int n = aptx_hd_enc_process(h, pcm, 4, out, sizeof(out));
        if (n != 6) { printf("erro no grupo %d (retornou %d)\n", g, n); continue; }
        total_bytes += n;
        groups_ok++;
    }

    printf("grupos codificados: %d / 200\n", groups_ok);
    printf("bytes totais: %ld\n", total_bytes);
    double seconds = 200.0 * 4 / 44100.0;
    double kbps = (total_bytes * 8.0 / 1000.0) / seconds;
    printf("taxa efetiva: %.1f kbps (esperado ~529kbps - taxa fixa do aptX HD em 44.1kHz/24bit)\n", kbps);

    aptx_hd_enc_close(h);

    if (groups_ok != 200) { printf("FALHA: nem todo grupo produziu saida\n"); return 1; }
    if (kbps < 500 || kbps > 560) { printf("FALHA: taxa fora do esperado\n"); return 1; }

    printf("OK\n");
    return 0;
}
