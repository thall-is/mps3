// Teste standalone (roda no host, fora do ESP-IDF) so' pra validar que o
// encoder vendorizado + wrapper realmente processam PCM e produzem
// bitstream LDAC plausivel, antes de confiar nisso dentro do firmware.
#include "ldac_enc.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

int main(void)
{
    ldac_enc_handle_t h = ldac_enc_open(679, 44100, LDAC_ENC_QUALITY_STANDARD);
    if (!h) { printf("FALHA: ldac_enc_open retornou NULL\n"); return 1; }

    int frame_samples = ldac_enc_frame_samples(h);
    printf("frame_samples = %d (esperado 128 para 44.1kHz)\n", frame_samples);

    int16_t pcm[256 * 2]; // margem para 128 ou 256
    uint8_t out[679];
    long total_bytes = 0;
    int total_frames_ok = 0;

    // 200 frames de um tom senoidal 1kHz estereo - o suficiente pra
    // passar da fase de "priming" do encoder (ele pode levar alguns
    // frames pra comecar a produzir saida, por causa do MDCT) e validar
    // varios ciclos.
    static double phase = 0.0;
    for (int f = 0; f < 200; f++) {
        for (int i = 0; i < frame_samples; i++) {
            int16_t s = (int16_t)(sin(phase) * 12000.0);
            pcm[i * 2 + 0] = s;
            pcm[i * 2 + 1] = s;
            phase += 2.0 * M_PI * 1000.0 / 44100.0;
        }
    int n = ldac_enc_process(h, pcm, out, sizeof(out), NULL);
        if (n < 0) {
            printf("erro no frame %d: codigo %d\n", f, ldac_enc_last_error(h));
            continue;
        }
        if (n > 0) {
            total_bytes += n;
            total_frames_ok++;
        }
    }

    printf("frames com saida: %d / 200\n", total_frames_ok);
    printf("bytes totais codificados: %ld\n", total_bytes);
    double seconds = 200.0 * frame_samples / 44100.0;
    double kbps = (total_bytes * 8.0 / 1000.0) / seconds;
    printf("taxa efetiva: %.1f kbps (esperado ~660kbps para SQ em 44.1kHz)\n", kbps);

    ldac_enc_close(h);

    // Nem toda chamada de ldac_enc_process() produz saida imediata: o
    // LDAC agrupa varios blocos MDCT de 128 amostras num unico
    // "ldac_transport_frame" (o pacote que de fato sai pela rede),
    // conforme o MTU configurado - entao e' normal so' uma fracao das
    // chamadas retornar bytes > 0. O que importa e' a taxa media.
    if (total_frames_ok < 30) { printf("FALHA: encoder nao produziu quase nenhuma saida\n"); return 1; }
    if (kbps < 400 || kbps > 900) { printf("FALHA: taxa fora do esperado pra SQ\n"); return 1; }

    printf("OK\n");
    return 0;
}
