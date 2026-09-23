#include "aptx_enc.h"
#include <stdio.h>
#include <math.h>

int main(void)
{
    aptx_enc_handle_t h = aptx_enc_open(44100);
    if (!h) { printf("FALHA: aptx_enc_open retornou NULL\n"); return 1; }

    // 200 grupos de 4 frames = 800 frames - tom de 1kHz estereo, mesmo
    // padrao dos outros testes de encoder deste projeto.
    int16_t pcm[4 * 2];
    uint8_t out[4];
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
        int n = aptx_enc_process(h, pcm, 4, out, sizeof(out));
        if (n != 4) { printf("erro no grupo %d (retornou %d)\n", g, n); continue; }
        total_bytes += n;
        groups_ok++;
    }

    printf("grupos codificados: %d / 200\n", groups_ok);
    printf("bytes totais: %ld\n", total_bytes);
    double seconds = 200.0 * 4 / 44100.0;
    double kbps = (total_bytes * 8.0 / 1000.0) / seconds;
    printf("taxa efetiva: %.1f kbps (esperado ~352kbps - taxa fixa do aptX em 44.1kHz)\n", kbps);

    // Rejeicao de frame_count invalido (nao multiplo de 4) - antes de
    // fechar o handle, pra' nao usar memoria ja' liberada.
    int16_t junk[3 * 2] = {0};
    uint8_t junk_out[8];
    if (aptx_enc_process(h, junk, 3, junk_out, sizeof(junk_out)) != -1) {
        printf("FALHA: deveria rejeitar frame_count nao multiplo de 4\n");
        return 1;
    }

    aptx_enc_close(h);

    if (groups_ok != 200) { printf("FALHA: nem todo grupo produziu saida\n"); return 1; }
    // aptX e' taxa fixa (nao variavel como LDAC/SBC) - a margem aqui e'
    // so' pra' arredondamento, nao pra' variacao real.
    if (kbps < 340 || kbps > 365) { printf("FALHA: taxa fora do esperado\n"); return 1; }

    printf("OK\n");
    return 0;
}
