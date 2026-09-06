#include "uart_ctrl.h"

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "uart_ctrl";

#define UART_SYNC_BYTE   0xA5
#define UART_MAX_PAYLOAD 250
// SYNC + LEN + CMD + payload + CRC
#define UART_MAX_FRAME   (3 + UART_MAX_PAYLOAD + 1)
#define UART_RX_BUF_SIZE 512
#define UART_TX_BUF_SIZE 256

static uart_ctrl_config_t s_cfg;
static SemaphoreHandle_t s_tx_mutex;
static bool s_initialized = false;

// CRC8/Maxim (poly 0x8C refletido, init 0x00) - leve o bastante pra
// rodar por byte sem tabela, e comum o suficiente pra achar
// implementacoes de referencia caso precise conferir em outra linguagem
// (ex: no lado do ESP32-S3 escrito em Python/outro stack de teste).
static uint8_t crc8_maxim(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x01) ? (uint8_t)((crc >> 1) ^ 0x8C) : (uint8_t)(crc >> 1);
        }
    }
    return crc;
}

bool uart_ctrl_send(uart_ctrl_cmd_t cmd, const uint8_t *payload, size_t len)
{
    if (!s_initialized || len > UART_MAX_PAYLOAD) return false;

    uint8_t frame[UART_MAX_FRAME];
    size_t pos = 0;
    frame[pos++] = UART_SYNC_BYTE;
    frame[pos++] = (uint8_t)len;
    frame[pos++] = (uint8_t)cmd;
    if (len > 0 && payload) {
        memcpy(&frame[pos], payload, len);
        pos += len;
    }
    // CRC cobre CMD + payload (indices 2..pos-1 no frame montado)
    frame[pos] = crc8_maxim(&frame[2], pos - 2);
    pos += 1;

    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    int written = uart_write_bytes(s_cfg.uart_num, (const char *)frame, pos);
    xSemaphoreGive(s_tx_mutex);

    return written == (int)pos;
}

bool uart_ctrl_send_status(uart_link_state_t state, const uint8_t bt_addr[6], uint32_t sample_rate)
{
    uart_status_payload_t st = {0};
    st.state = (uint8_t)state;
    if (bt_addr) memcpy(st.bt_addr, bt_addr, 6);
    st.sample_rate = sample_rate;
    return uart_ctrl_send(UART_EVT_STATUS, (const uint8_t *)&st, sizeof(st));
}

// Maquina de estados simples de parsing, byte a byte, direto do driver
// UART (sem depender de "receber o pacote inteiro de uma vez" - a UART
// entrega em pedaços arbitrários).
typedef enum { WAIT_SYNC, WAIT_LEN, WAIT_CMD, WAIT_PAYLOAD, WAIT_CRC } parse_state_t;

static void rx_task(void *arg)
{
    (void)arg;
    uint8_t byte;
    parse_state_t state = WAIT_SYNC;
    uint8_t payload[UART_MAX_PAYLOAD];
    uint8_t len = 0, cmd = 0, payload_idx = 0;

    while (1) {
        int n = uart_read_bytes(s_cfg.uart_num, &byte, 1, pdMS_TO_TICKS(1000));
        if (n <= 0) continue; // timeout - so' um jeito de nao bloquear pra sempre, sem trabalho a fazer

        switch (state) {
            case WAIT_SYNC:
                if (byte == UART_SYNC_BYTE) state = WAIT_LEN;
                break;

            case WAIT_LEN:
                len = byte;
                payload_idx = 0;
                state = (len > UART_MAX_PAYLOAD) ? WAIT_SYNC : WAIT_CMD;
                break;

            case WAIT_CMD:
                cmd = byte;
                state = (len == 0) ? WAIT_CRC : WAIT_PAYLOAD;
                break;

            case WAIT_PAYLOAD:
                payload[payload_idx++] = byte;
                if (payload_idx >= len) state = WAIT_CRC;
                break;

            case WAIT_CRC: {
                // Recalcula CRC sobre CMD+payload pra conferir contra o
                // byte recebido.
                uint8_t buf[1 + UART_MAX_PAYLOAD];
                buf[0] = cmd;
                if (len > 0) memcpy(&buf[1], payload, len);
                uint8_t expected = crc8_maxim(buf, 1 + len);

                if (expected == byte) {
                    if (s_cfg.rx_cb) {
                        s_cfg.rx_cb((uart_ctrl_cmd_t)cmd, payload, len, s_cfg.user_ctx);
                    }
                } else {
                    ESP_LOGW(TAG, "CRC invalido (cmd=0x%02x len=%d) - quadro descartado", cmd, len);
                }
                state = WAIT_SYNC;
                break;
            }
        }
    }
}

int uart_ctrl_init(const uart_ctrl_config_t *cfg)
{
    if (!cfg || !cfg->rx_cb) return -1;
    s_cfg = *cfg;

    uart_config_t uart_cfg = {
        .baud_rate = cfg->baud_rate > 0 ? cfg->baud_rate : 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(cfg->uart_num, &uart_cfg);
    if (err != ESP_OK) return -1;

    err = uart_set_pin(cfg->uart_num, cfg->tx_gpio, cfg->rx_gpio,
                        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return -1;

    err = uart_driver_install(cfg->uart_num, UART_RX_BUF_SIZE, UART_TX_BUF_SIZE, 0, NULL, 0);
    if (err != ESP_OK) return -1;

    s_tx_mutex = xSemaphoreCreateMutex();
    if (!s_tx_mutex) return -1;

    s_initialized = true;

    BaseType_t ok = xTaskCreate(rx_task, "uart_ctrl_rx", 4096, NULL, 10, NULL);
    if (ok != pdPASS) {
        s_initialized = false;
        return -1;
    }

    ESP_LOGI(TAG, "UART controle pronto (uart%d, tx=%d rx=%d, %d baud)",
             cfg->uart_num, cfg->tx_gpio, cfg->rx_gpio, uart_cfg.baud_rate);
    return 0;
}
