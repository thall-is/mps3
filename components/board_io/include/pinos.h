#ifndef PINOS_H
#define PINOS_H

// ============================================================================
//  mps3 — Mapa Oficial de Pinos e Barramentos (ESP32-S3 N16R8)
// ============================================================================

// --- Display OLED SSD1306 (I2C a 400 kHz) -----------------------------------
#define PIN_OLED_SDA        10
#define PIN_OLED_SCL        9

// --- Cartão MicroSD (Barramento Nativo SDMMC 4-Bit a 40 MHz) ----------------
// Linhas com resistores pull-up externos de 10 kΩ conectados ao barramento 3.3V
#define PIN_SD_CLK          5
#define PIN_SD_CMD          6
#define PIN_SD_D0           4
#define PIN_SD_D1           17
#define PIN_SD_D2           16
#define PIN_SD_D3           7

// --- Áudio Digital I2S Master (DAC PCM5102A e ESP32 Companion) --------------
// Gera os clocks para o DAC local e o co-processador Bluetooth em paralelo
#define PIN_I2S_BCLK        48
#define PIN_I2S_LRCK        21
#define PIN_I2S_DOUT        47
#define PIN_I2S_MCLK        8   // Master Clock (MCLK) dedicado para o DAC PCM5102A (SCK)

// --- Enlace Serial UART com o Co-Processador Bluetooth (bt_companion) --------
// Comunicação binária com checksum a 115200 bps 8N1 + linha de reset de hardware
#define PIN_BT_LINK_UART_TX 14  // Conecta ao GPIO 16 (RX) do ESP32 Companion
#define PIN_BT_LINK_UART_RX 13  // Conecta ao GPIO 17 (TX) do ESP32 Companion
#define PIN_BT_LINK_EN      12  // Conecta ao pino EN/CHIP_PU do ESP32 Companion (Reset ativo baixo)

// --- Joystick de Navegação de 5 Vias ----------------------------------------
// Botões acionados em nível baixo (GND) com resistores de pull-up internos
#define PIN_JOY_UP          2
#define PIN_JOY_LEFT        39
#define PIN_JOY_DOWN        41
#define PIN_JOY_RIGHT       42
#define PIN_JOY_CENTER      40

// --- Monitoramento de Bateria Li-Ion & Carregador TP4056 ---------------------
#define PIN_BATTERY_ADC     1   // Entrada analógica ADC1 Canal 0 (divisor resistivo 100k / 100k)
#define PIN_BATTERY_CHRG    11  // Monitor de carga do TP4056 (ativo em nível baixo com pull-up)
#define PIN_BATTERY_STDBY   -1  // Desconectado (pino reservado)

// --- LED RGB Endereçável On-board (WS2812 / NeoPixel) -----------------------
#define PIN_RGB_LED         38

// --- USB Nativo do SoC ESP32-S3 (D- / D+) -----------------------------------
// Pinos dedicados de silício (PHY USB OTG Full-Speed)
#define PIN_USB_DM          19
#define PIN_USB_DP          20

#endif // PINOS_H
