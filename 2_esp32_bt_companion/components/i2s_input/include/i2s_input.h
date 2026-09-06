#ifndef I2S_INPUT_H
#define I2S_INPUT_H

// Entrada I2S deste companheiro: ele e' o SLAVE (BCLK/WS vem da placa
// principal, o ESP32-S3, que e' quem manda o clock - ver
// components/board_io/i2s_output.c no projeto principal). Este modulo
// so' escuta o mesmo barramento que ja alimenta o DAC PCM5102 (fiado em
// paralelo, sem nada extra em hardware).
//
// ATENCAO - formato ainda a confirmar contra o hardware real: a placa
// principal manda slots de 32 bits, formato Philips, estereo (ver
// I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,...) no
// i2s_output.c dela). Este driver le' os mesmos slots de 32 bits. O que
// NAO esta' confirmado aqui e' quantos bits de audio real estao
// ocupados dentro de cada slot de 32 (os 16 mais significativos? 24?) -
// isso depende de como audio_player.cpp empacota a amostra antes de
// chamar i2s_output_write(). i2s_input_read_frames() abaixo entrega o
// slot de 32 bits cru (com sinal, alinhado a esquerda) - a extracao
// pros 16 bits que o encoder LDAC espera fica pro chamador (main.c),
// documentada la' com a mesma ressalva.

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int bclk_gpio;
    int ws_gpio;
    int din_gpio;
} i2s_input_config_t;

esp_err_t i2s_input_init(const i2s_input_config_t *cfg);
esp_err_t i2s_input_set_rate(uint32_t sample_rate);

// Le' ate' frame_count frames estereo (1 frame = 1 amostra L + 1 amostra
// R, cada uma um int32_t = o slot de 32 bits cru vindo do barramento).
// out precisa caber frame_count*2 int32_t. Bloqueia (com timeout) ate'
// ter dados - e' pra ser chamado de uma tarefa dedicada that then
// entrega pro encoder. Retorna o numero de FRAMES (nao amostras) lidos,
// ou <0 em erro.
int i2s_input_read_frames(int32_t *out, size_t frame_count, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // I2S_INPUT_H
