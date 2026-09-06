#include "u8g2_hal.h"

#include <stdbool.h>
#include <string.h>
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"
#include "esp_log.h"

static const char *TAG = "u8g2_hal";

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;

// O protocolo u8x8 entrega os bytes em pequenos blocos entre
// START_TRANSFER/END_TRANSFER. E' bem mais eficiente acumular tudo aqui e
// mandar de uma vez com i2c_master_transmit() no END_TRANSFER, em vez de
// uma transacao I2C por bloco.
//
// BUG CORRIGIDO (2 rodadas): o buffer original era de so' 64 bytes,
// pequeno demais pra' uma pagina do SSD1306 (128 bytes de pixel + alguns
// comandos), e o codigo IGNORAVA silenciosamente qualquer byte alem do
// limite - corrompia a tela, especialmente devastador pra' fonte pequena
// 4x6 (aparencia de "hieroglifos"). Aumentamos pra' 1200 bytes, mas o
// problema voltou a aparecer - sinal de que uma transferencia logica do
// u8g2 pode, em algum caso, ser maior do que estimamos. Em vez de tentar
// adivinhar um numero "grande o suficiente" de novo, a logica abaixo
// agora NUNCA descarta dados: se o buffer ficar cheio no MEIO de uma
// transferencia, manda o que ja' tem acumulado agora mesmo (quebra em
// mais de uma transacao I2C) e continua acumulando o resto - correto
// independente do tamanho real da transferencia. O buffer em si tambem
// foi aumentado pra' 2048 bytes (o dobro do framebuffer inteiro do
// SSD1306 128x64) como margem extra, pra' minimizar quantas vezes essa
// quebra em varias transacoes precisa acontecer na pratica.
static uint8_t s_txbuf[2048];
static size_t s_txlen = 0;

// Primeiro byte de CADA transferencia logica u8x8 (entre START_TRANSFER e
// END_TRANSFER) e' sempre o "control byte" do protocolo I2C do SSD1306
// (0x00 = bytes seguintes sao comando, 0x40 = bytes seguintes sao dado de
// pixel). O chip usa isso pra' interpretar TODOS os bytes daquela
// transacao I2C - se quebrarmos a transferencia em mais de uma transacao
// (flush no meio, ver SEND abaixo), a transacao de continuacao PRECISA
// comecar com esse mesmo control byte, senao o SSD1306 interpreta o
// primeiro byte da continuacao como se fosse um novo control byte -
// desalinhando tudo que vem depois (era essa a causa real da fonte 4x6
// virar "hieroglifo": nao faltava espaco no buffer, faltava reenviar o
// control byte quando o buffer enchia no meio de uma transferencia).
static uint8_t s_ctrl_byte = 0;
static bool s_have_ctrl = false;

static void flush_txbuf(void)
{
    if (s_txlen > 0 && s_dev) {
        i2c_master_transmit(s_dev, s_txbuf, s_txlen, 1000);
    }
    s_txlen = 0;
}

esp_err_t u8g2_hal_i2c_init(int sda_gpio, int scl_gpio, uint8_t i2c_addr_7bit)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao criar barramento I2C: %s", esp_err_to_name(ret));
        return ret;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = i2c_addr_7bit,
        .scl_speed_hz = 400000,
    };
    ret = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao adicionar dispositivo I2C: %s", esp_err_to_name(ret));
    }
    return ret;
}

uint8_t u8g2_hal_byte_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8; // assinatura fixa exigida por u8x8_msg_cb - nao usado aqui

    switch (msg) {
        case U8X8_MSG_BYTE_INIT:
            // barramento ja' inicializado via u8g2_hal_i2c_init()
            break;

        case U8X8_MSG_BYTE_SET_DC:
            break; // I2C nao usa linha D/C

        case U8X8_MSG_BYTE_START_TRANSFER:
            s_txlen = 0;
            s_have_ctrl = false;
            break;

        case U8X8_MSG_BYTE_SEND: {
            const uint8_t *data = (const uint8_t *)arg_ptr;
            for (uint8_t i = 0; i < arg_int; i++) {
                // O primeiro byte que passa por aqui depois de um
                // START_TRANSFER e' sempre o control byte desta
                // transferencia logica - guardamos antes de acumular.
                if (!s_have_ctrl) {
                    s_ctrl_byte = data[i];
                    s_have_ctrl = true;
                }

                if (s_txlen >= sizeof(s_txbuf)) {
                    // Buffer encheu no MEIO de uma transferencia logica -
                    // manda o que ja' tem acumulado agora (em vez de
                    // descartar o resto, que era o bug original) e segue
                    // acumulando o restante numa NOVA transacao I2C. Essa
                    // nova transacao precisa comecar com o mesmo control
                    // byte da transferencia original, senao o SSD1306
                    // reinterpreta o proximo byte como control byte e
                    // corrompe o resto do quadro (era essa a causa real
                    // da fonte 4x6 virar "hieroglifo").
                    flush_txbuf();
                    s_txbuf[s_txlen++] = s_ctrl_byte;
                }
                s_txbuf[s_txlen++] = data[i];
            }
            break;
        }

        case U8X8_MSG_BYTE_END_TRANSFER:
            flush_txbuf();
            break;

        default:
            return 0;
    }
    return 1;
}

uint8_t u8g2_hal_gpio_and_delay_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8;    // assinatura fixa exigida por u8x8_msg_cb - nao usado aqui
    (void)arg_ptr; // idem - nenhuma das mensagens tratadas abaixo precisa dele

    switch (msg) {
        case U8X8_MSG_GPIO_AND_DELAY_INIT:
            break;
        case U8X8_MSG_DELAY_MILLI:
            vTaskDelay(pdMS_TO_TICKS(arg_int));
            break;
        case U8X8_MSG_DELAY_10MICRO:
            esp_rom_delay_us(10);
            break;
        case U8X8_MSG_DELAY_100NANO:
            break; // negligivel pro nosso caso, ignorar
        case U8X8_MSG_GPIO_I2C_CLOCK:
        case U8X8_MSG_GPIO_I2C_DATA:
            break; // pinos gerenciados pelo driver i2c_master (sem bit-banging)
        default:
            return 0;
    }
    return 1;
}
