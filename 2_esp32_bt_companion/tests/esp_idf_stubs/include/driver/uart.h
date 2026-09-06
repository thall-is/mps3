#ifndef DRIVER_UART_H_STUB
#define DRIVER_UART_H_STUB
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#define UART_PIN_NO_CHANGE (-1)

typedef enum { UART_NUM_0, UART_NUM_1, UART_NUM_2 } uart_port_t;
typedef enum { UART_DATA_5_BITS, UART_DATA_6_BITS, UART_DATA_7_BITS, UART_DATA_8_BITS } uart_word_length_t;
typedef enum { UART_STOP_BITS_1 = 1, UART_STOP_BITS_1_5, UART_STOP_BITS_2 } uart_stop_bits_t;
typedef enum { UART_PARITY_DISABLE = 0, UART_PARITY_EVEN, UART_PARITY_ODD } uart_parity_t;
typedef enum { UART_HW_FLOWCTRL_DISABLE = 0, UART_HW_FLOWCTRL_RTS, UART_HW_FLOWCTRL_CTS, UART_HW_FLOWCTRL_CTS_RTS } uart_hw_flowcontrol_t;
typedef enum { UART_SCLK_DEFAULT = 0, UART_SCLK_APB, UART_SCLK_XTAL } uart_sclk_t;

typedef struct {
    int baud_rate;
    uart_word_length_t data_bits;
    uart_parity_t parity;
    uart_stop_bits_t stop_bits;
    uart_hw_flowcontrol_t flow_ctrl;
    uint8_t rx_flow_ctrl_thresh;
    uint32_t rx_glitch_filt_thresh;
    union { uart_sclk_t source_clk; };
    struct { uint32_t allow_pd : 1; uint32_t backup_before_sleep : 1; } flags;
} uart_config_t;

typedef void *QueueHandle_t_fwd;

static inline esp_err_t uart_param_config(uart_port_t p, const uart_config_t *c) { (void)p; (void)c; return ESP_OK; }
static inline esp_err_t uart_set_pin(uart_port_t p, int tx, int rx, int rts, int cts) { (void)p; (void)tx; (void)rx; (void)rts; (void)cts; return ESP_OK; }
static inline esp_err_t uart_driver_install(uart_port_t p, int rx_buf, int tx_buf, int qsize, void *q, int flags) {
    (void)p; (void)rx_buf; (void)tx_buf; (void)qsize; (void)q; (void)flags; return ESP_OK;
}
static inline int uart_write_bytes(uart_port_t p, const void *src, size_t size) { (void)p; (void)src; return (int)size; }
static inline int uart_read_bytes(uart_port_t p, void *buf, uint32_t len, uint32_t ticks) {
    (void)p; (void)buf; (void)len; (void)ticks; return 0;
}
#endif
