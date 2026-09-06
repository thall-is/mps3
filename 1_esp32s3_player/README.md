# mps3 — Firmware da Placa Principal (ESP32-S3 N16R8)

Este diretório contém o firmware do processador central do **mps3**, desenvolvido para o SoC **ESP32-S3-WROOM-1 (N16R8)** com 16 MB de memória Flash Quad-SPI e 8 MB de Octal PSRAM.

---

## 🛠️ Principais Recursos e Responsabilidades

1. **Armazenamento e Leitura em Alta Velocidade**:
   * Acesso ao cartão micro SD via controlador **SDMMC em 4 vias nativas a 40 MHz**.
   * Sistema de arquivos FAT32 montado com buffers DMA em memória interna para assegurar leitura contínua acima de 15 MB/s, garantindo reprodução estável mesmo nas faixas Hi-Res mais exigentes.
2. **Motor de Decodificação de Áudio (`audio_player`)**:
   * Decodificação em fluxo contínuo de **FLAC (16 e 24 bits nativos, até 96.000 Hz)**, **MP3**, **WAV**, **AAC/M4A** e **OGG Vorbis**.
   * Preservação da resolução de 24 bits sem truncamento através de palavras `int32_t` alinhadas para a saída de áudio digital.
   * Avanço e retrocesso analítico instantâneo no arquivo sem pausas para varredura sequencial.
3. **Equalizador Paramétrico de 10 Bandas (`eq`)**:
   * Filtros IIR biquad processados em ponto flutuante no Core 1.
   * **Bypass automático inteligente acima de 48 kHz**: em faixas de 88.2 kHz ou 96 kHz, o equalizador entra em bypass para preservar a fidelidade da resposta de fase e economizar ciclos de processamento.
4. **Interface Gráfica no OLED SSD1306 (`oled_display` + `menu`)**:
   * Renderização fluida para display OLED (128x64) via I2C a 400 kHz.
   * Navegação em carrossel controlada pelo joystick de 5 direções.
   * Telas dedicadas para Navegador de Pastas, Tela de Reprodução ("Now Playing"), Equalizador, Menu Bluetooth e Informações do Sistema.
5. **Transmissão Digital Master I2S (`board_io/i2s_output.c`)**:
   * Transmissão com resolução de até 24 bits / 96 kHz em slots estéreo de 32 bits.
   * Ajuste automático de taxa de amostragem na troca de faixa para casar perfeitamente com a resolução do arquivo.
6. **Comunicação UART com o Módulo Bluetooth (`bt_link`)**:
   * Comunicação serial a 115.200 bps com pacotes protegidos por checksum, gerenciando busca de fones, pareamento, volume e relatórios de bitrate.
7. **Servidor Web Wi-Fi de Gerenciamento (`wifi_transfer`)**:
   * Ponto de acesso ou cliente de rede (`mps3.local`) que disponibiliza uma página moderna para upload de álbuns via navegador.

---

## ⚠️ Estado dos Recursos: O Que Funciona vs Limitações

| Recurso | Status | Observações Técnicas |
|---|:---:|---|
| **Decodificação FLAC 24b/96kHz** | **100% Funcional** | Decodificação nativa preservando a profundidade de 24 bits. |
| **Decodificação MP3 / WAV / AAC / M4A / OGG** | **100% Funcional** | Reprodução estável de todos os formatos populares. |
| **SDMMC 4-Bit a 40 MHz** | **100% Funcional** | Leitura rápida e sem falhas de buffer. |
| **Interface OLED + Joystick** | **100% Funcional** | Navegação por pastas, exibição de tags ID3v2 e bateria. |
| **Equalizador de 10 Bandas** | **100% Funcional** | Ativo até 48 kHz; bypass automático em 96 kHz. |
| **Gerenciador Web Wi-Fi (`mps3.local`)** | **100% Funcional** | Upload de faixas via navegador direto para o micro SD. |
| **Montagem do SD via USB (USB MSC)** | ❌ **Quebrado** | Conflito com o driver SDMMC. Não utilize a porta USB para transferir arquivos. |
| **Saída de Áudio USB (Placa de Som)** | ❌ **Desativado** | Recurso não suportado nesta versão. |
| **Decodificador Opus** | ⚠️ **Experimental** | Pode demandar mais memória de pilha em arquivos complexos. |

---

## 🔌 Pinagem Oficial da Placa Principal (ESP32-S3)

| Função | Pino GPIO | Observação |
|---|:---:|---|
| **I2S DOUT** | **GPIO 47** | Áudio PCM estéreo para o DAC PCM5102A e Companion |
| **I2S BCLK** | **GPIO 48** | Bit Clock síncrono gerado pelo S3 |
| **I2S LRCK (WS)** | **GPIO 21** | Seleção de canal e taxa (44.1 kHz a 96 kHz) |
| **UART1 TX** | **GPIO 14** | Envia comandos para o Companion (pino RX 16 do Companion) |
| **UART1 RX** | **GPIO 13** | Recebe eventos do Companion (pino TX 17 do Companion) |
| **BT RESET (EN)** | **GPIO 12** | Controle de reset por hardware do co-processador |
| **I2C SDA** | **GPIO 10** | Linha de dados do display OLED SSD1306 |
| **I2C SCL** | **GPIO 9** | Linha de clock do display OLED SSD1306 |
| **SDMMC CLK** | **GPIO 5** | Clock do cartão micro SD (40 MHz) |
| **SDMMC CMD** | **GPIO 6** | Linha de comando com resistor de pull-up externo de 10k |
| **SDMMC D0** | **GPIO 4** | Dado 0 com resistor de pull-up de 10k |
| **SDMMC D1** | **GPIO 17** | Dado 1 com resistor de pull-up de 10k |
| **SDMMC D2** | **GPIO 16** | Dado 2 com resistor de pull-up de 10k |
| **SDMMC D3** | **GPIO 7** | Dado 3 com resistor de pull-up de 10k |
| **Joystick UP** | **GPIO 2** | Entrada digital ativa baixa com pull-up interno |
| **Joystick LEFT** | **GPIO 39** | Entrada digital ativa baixa com pull-up interno |
| **Joystick DOWN** | **GPIO 41** | Entrada digital ativa baixa com pull-up interno |
| **Joystick RIGHT** | **GPIO 42** | Entrada digital ativa baixa com pull-up interno |
| **Joystick CENTER** | **GPIO 40** | Botão de confirmação (Play/Pause/Enter) |
| **ADC Bateria** | **GPIO 1** | Leitura de tensão Li-Ion via divisor resistivo |
| **Status Carga TP4056** | **GPIO 11** | Monitor de carga do TP4056 (ativo baixo) |
| **RGB LED (WS2812)** | **GPIO 38** | LED de status colorido |

---

## 🔨 Como Compilar e Gravar

### Usando ESP-IDF (Recomendado: v6.0.1 ou superior)
```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COM7 flash monitor
```

### Usando PlatformIO
```bash
pio run -t upload --upload-port COM7
```
