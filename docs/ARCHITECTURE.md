# Arquitetura do Sistema e Pipeline de Áudio — mps3

O **mps3** adota uma topologia de co-processamento distribuído (**Dual-MCU**) para contornar as restrições físicas dos microcontroladores de mercado e viabilizar um reprodutor de áudio audiófilo de padrão comercial.

---

## 1. Topologia Dual-MCU: Por que dois microcontroladores?

* **ESP32-S3 (Placa Principal / Player):**
  - **Processador:** Xtensa Dual-Core 32-bit LX7 @ 240 MHz com instruções SIMD vetoriais.
  - **Memória:** 16 MB Flash Quad-SPI + 8 MB Octal-SPI PSRAM a 80 MHz (largura de banda superior para descompressão FLAC e heaps de rede).
  - **Periféricos:** Host SDMMC nativo de 4 bits operando a 40 MHz, barramento I2S Master DMA com saída estéreo PCM, display OLED I2C em 400 kHz, USB nativo (TinyUSB MSC).
  - **Limitação Física do Silício:** O rádio 2.4 GHz do ESP32-S3 suporta **apenas Bluetooth Low Energy (BLE 5.0)**. O perfil padrão de transmissão de áudio sem fio em alta resolução, **A2DP (Advanced Audio Distribution Profile)**, opera estritamente sobre **Bluetooth Clássico (BR/EDR)**.
* **ESP32 Clássico (Co-Processador bt_companion):**
  - **Processador:** Xtensa Dual-Core 32-bit LX6 @ 240 MHz.
  - **Rádio Integrado:** Bluetooth Clássico BR/EDR com suporte nativo a modulações GFSK, pi/4-DQPSK e 8DPSK (taxas de 1, 2 e 3 Mbps).
  - **Papel Exclusivo:** Atua como terminal I2S Slave síncrono, receptor de comandos de controle UART, motor de reamostragem contínua em tempo real e transmissor A2DP Source multi-codec (com foco operacional em **Sony LDAC** e **SBC**).

---

## 2. Diagrama de Blocos Funcional

```text
+-----------------------------------------------------------------------------------------+
|                                  ESP32-S3 (Placa Principal)                             |
|                                                                                         |
|  +--------------------+        +-----------------------+        +--------------------+  |
|  | MicroSD (SDMMC 4b) | -----> |  Decodificador Hi-Res | -----> |   Equalizador IIR  |  |
|  |  FLAC / MP3 / WAV  |        |  (Core 1 - 24b/96kHz) |        |   10 Bandas Biquad |  |
|  +--------------------+        +-----------------------+        +--------------------+  |
|                                                                            |            |
|  +--------------------+        +-----------------------+                   v            |
|  | Display OLED I2C   | <..... | Menu / GUI / Joystick |        +--------------------+  |
|  | (SSD1306 128x64)   |        | (Core 0 - Interface)  |        |  Driver I2S Master |  |
|  +--------------------+        +-----------------------+        |  DMA 32-bit Stereo |  |
|                                            :                    +--------------------+  |
|                                            : UART                          | I2S        |
+--------------------------------------------:-------------------------------:------------+
                                             : (115200 8N1)                  |            |
                                             v                               |            |
+--------------------------------------------------------------------+       |            |
|                     ESP32 Clássico (Co-Processador)                |       |            |
|                                                                    |       |            |
|  +--------------------+   +--------------------+   +-------------+ |       |            |
|  | Controle UART Link |   |  Reamostrador 32.32|   |  I2S Slave  | <-------+            |
|  | (Core 0 - Estado)  |   | (96k/48k -> 44.1k) |   | (PIN 26/25) | |       |            |
|  +--------------------+   +--------------------+   +-------------+ |       |            |
|             |                         |                            |       |            |
|             v                         v                            |       |            |
|  +--------------------+   +--------------------+                   |       |            |
|  |  AVRCP Controller  |   | Multi-Codec Engine |                   |       |            |
|  |  (Absolute Volume) |   | LDAC/aptX-HD/aptX  |                   |       |            |
|  +--------------------+   +--------------------+                   |       |            |
|             |                         |                            |       |            |
|             +------------+------------+                            |       |            |
|                          v                                         |       |            |
|             +--------------------------+                           |       |            |
|             |  Pilha Bluedroid BT A2DP |                           |       |            |
|             |  Multi-SEP (4 SEIDs)     |                           |       |            |
|             +--------------------------+                           |       |            |
|                          |                                         |       |            |
+--------------------------:-----------------------------------------+       |            |
                           : Bluetooth A2DP / AVRCP                          v            |
                           v                                        +-------------------+ |
               +-----------------------+                            |   DAC PCM5102A    | |
               | Fone de Ouvido / Sink |                            | (Saída P2 Fones)  | <+
               | Bluetooth Sem Fio     |                            +-------------------+
               +-----------------------+
```

---

## 3. Pipeline de Reamostragem Hi-Res em Ponto Fixo 32.32

### O Desafio
A especificação Bluetooth A2DP para codecs como SBC limita a taxa de amostragem oficial a 44.1 kHz ou 48 kHz. Ao reproduzir no ESP32-S3 áudios de altíssima definição (FLAC 24-bit/96kHz ou 88.2kHz), o barramento I2S opera fisicamente nessas frequências. Se as amostras fossem descartadas ou decimadas ingenuamente, haveria aliasing severo, estalos e distorção harmônica intolerável.

### A Solução Matemática
O componente `audio_pipeline` implementa interpolação linear contínua utilizando acumulador de fase de 64 bits em **ponto fixo 32.32**:

$$\text{step} = \left( \frac{\text{in\_rate}}{\text{out\_rate}} \right) \times 2^{32}$$

* **Preservação de Resíduo de Fase (*Carry*):** O interpolador guarda as últimas amostras esquerda e direita (`carry_l`, `carry_r`) e o resíduo sub-amostral da fase entre chamadas DMA. A forma de onda gerada é rigorosamente contínua através das fronteiras dos buffers.
* **Baixa Sobrecarga:** Totalmente baseado em aritmética inteira e deslocamento de bits, demandando menos de 8% de um núcleo de CPU a 240 MHz.
* **Comutação Dinâmica:** Quando o S3 inicia uma nova faixa com taxa distinta, despacha `UART_CMD_SET_SAMPLE_RATE` (0x0B). O co-processador ajusta imediatamente os divisores do periférico I2S Slave sem interrupção de áudio audível.

---

## 4. Multi-SEP e Suporte a Codecs Proprietários no Bluedroid

### Limitações Nativas do ESP-IDF
1. `ESP_A2D_MAX_SEPS` vem originalmente fixado em 1.
2. Codecs que utilizam tipo de mídia `0xFF` (`ESP_A2D_MCT_NON_A2DP`) têm sua alocação de buffer rejeitada por padrão em `btc_av_audio_buff_alloc()`, retornando ponteiro nulo e gerando erro `ESP_ERR_NO_MEM`.
3. A camada de transporte AVDTP injeta um cabeçalho RTP de 12 bytes redundante se o sinalizador `*p_no_rtp_hdr` não for configurado como `TRUE`.

### Inovações do Projeto mps3
Com os patches da pasta `esp-idf-patches/` e a compilação com `CONFIG_BT_A2DP_SEP_NUM_MAX=4`:
* Registramos 4 Stream Endpoints simultâneos:
  - **SEID 0:** Sony LDAC (Vendor ID `0x00012D`, Codec ID `0x00AA`)
  - **SEID 1:** Qualcomm aptX HD (Vendor ID `0x0000D7`, Codec ID `0x0024`)
  - **SEID 2:** Qualcomm aptX (Vendor ID `0x00004F`, Codec ID `0x0001`)
  - **SEID 3:** SBC Padrão (Bitpool 53, alta qualidade)
* **Memória Sob Controle:** O encoder LDAC aloca ~24 KB de RAM. Com todos os 4 encoders abertos e a pilha Bluetooth ativa, **sobram mais de 185 KB livres de SRAM**, dispensando PSRAM externa no co-processador!

---

## 5. Distribuição de Tarefas (Core Pinning)

### ESP32-S3 (Player)
* **Core 0:** GUI OLED SSD1306, leitura com aceleração do joystick, servidor web Wi-Fi (mps3.local), pilha USB MSC.
* **Core 1:** Decodificação de arquivos de áudio (FLAC, MP3, WAV, AAC, Opus), equalizador IIR de 10 bandas, alimentação DMA I2S.

### ESP32 Companion
* **Core 0:** Pilha Bluedroid (HCI, L2CAP, AVDTP, AVRCP), recepção e envio de pacotes de controle UART.
* **Core 1:** Tarefa de áudio dedicada (`audio_task`, prioridade 18, stack 8 KB): leitura I2S Slave, reamostrador 32.32, codificação LDAC/aptX/SBC e despacho de pacotes de mídia para a pilha de rádio.
