# Arquitetura do bt_companion

Visão consolidada da engenharia interna do co-processador Bluetooth do **mps3**. Para detalhes sobre pinagem, patches do ESP-IDF e como compilar, consulte o [`README.md`](README.md).

---

## 📐 Panorama Geral

```text
                    Placa Principal (ESP32-S3 - mps3)
                                  |
                    I2S (BCLK: 48, LRCK: 21, DOUT: 47)
                    UART (TX: 14 -> RX: 16 | RX: 13 <- TX: 17)
                                  |
                                  v
+---------------------------------------------------------------+
|                 bt_companion (ESP32 Clássico)                 |
|                                                               |
|  uart_ctrl -------> main.c <------- i2s_input                 |
|  (Comandos e         |  |           (I2S Slave RX)            |
|   Telemetria)        |  |                                     |
|                      |  v                                     |
|                      |  audio_pipeline                        |
|                      |  - Acumula frames em chunks contínuos  |
|                      |  - Reamostrador 32.32 (pass-thru 96k)  |
|                      |         |                              |
|                      |         v                              |
|                      |  Qual codec foi negociado no AVDTP?    |
|                      |         |                              |
|                      |   +-----+-----+                        |
|                      |   |           |                        |
|                      |   v (24-bit)  v (16-bit)               |
|                      |  Sony LDAC    SBC                      |
|                      |  ldac_enc     sbc_enc                  |
|                      |  (Q30 s32)    (extract16)              |
|                      |   |           |                        |
|                      |   v           |                        |
|                      |  bitrate_abr  |                        |
|                      |  (330-990k)   |                        |
|                      |   |           |                        |
|                      |   +-----+-----+                        |
|                      |         |                              |
|                      |         v                              |
|                      |   a2dp_media_payload (Cabeçalho RTP)   |
|                      |         |                              |
|                      v         v                              |
|                   bt_source (Pilha Bluedroid Patcheada)       |
|                      |                                        |
+----------------------|----------------------------------------+
                       v
                Bluetooth A2DP (Fones / Caixas de Som)
```

---

## 🧩 Os Componentes do Firmware

| Componente | Responsabilidade | Origem do Código | Testável no Computador (Host)? |
|---|---|---|:---:|
| **`uart_ctrl`** | Protocolo serial com a placa principal (quadros com checksum) | Desenvolvido no projeto | Sim (via stubs) |
| **`i2s_input`** | Driver de recepção de áudio digital I2S em modo Slave | Desenvolvido no projeto | Sim (via stubs) |
| **`audio_pipeline`** | Buffer acumulador + Reamostrador em ponto fixo 32.32 + conversão 16-bit | Desenvolvido no projeto | **Sim (100% nativo)** |
| **`ldac_enc`** | Codificador Sony LDAC (suporta entrada nativa 32-bit Q30) | Vendorizado (Sony AOSP) + wrapper | **Sim (100% nativo)** |
| **`sbc_enc`** | Codificador padrão SBC (Bitpool 53 de alta fidelidade) | Vendorizado (BlueZ / AOSP) + wrapper | **Sim (100% nativo)** |
| **`bitrate_abr`** | Algoritmo de adaptação dinâmica de bitrate para LDAC (330 a 990 kbps) | Desenvolvido no projeto | **Sim (100% nativo)** |
| **`bt_source`** | Pilha A2DP Source, registro de múltiplos SEPs e despacho de pacotes | Bluedroid ESP-IDF com patches locais | Requer silício ESP32 |
| **`main`** | Tarefas FreeRTOS, sincronismo de taxas e orquestração | Desenvolvido no projeto | Requer silício ESP32 |

*(Nota: os componentes `aptx_enc` e `aptx_hd_enc` também estão presentes no repositório, mas seu handshake AVDTP não opera de forma estável com fones comerciais atuais. O fluxo ativo de produção utiliza **Sony LDAC** e **SBC**).*

---

## 🔬 Decisões Críticas de Arquitetura

1. **Separação Rígida entre Matemática Pura e Hardware**:
   * Todos os módulos matemáticos (`audio_pipeline`, `ldac_enc`, `sbc_enc`, `bitrate_abr`, `a2dp_media_payload`) não realizam chamadas a drivers do ESP-IDF.
   * Isso permitiu criar uma suíte completa de **testes de bancada para computador** (`tests/run_host_tests.sh`), executada diretamente com `gcc` no Linux ou Windows, garantindo que o algoritmo de compressão e o empacotamento estejam perfeitos antes de gravar no microcontrolador.

2. **Preservação Total de 24 Bits no Sony LDAC**:
   * O pipeline não descarta nem trunca bits para o encoder LDAC. O S3 envia amostras estéreo em slots de 32 bits; o `audio_pipeline` as mantém intactas e as entrega para `ldac_enc_process_s32()`.
   * A biblioteca interna da Sony converte para ponto fixo Q30 (`val >> 1`), preservando os **24 bits mais significativos de resolução nativa**.

3. **Reamostragem Contínua em Ponto Fixo 32.32**:
   * Se a taxa de amostragem da música for idêntica à do fone (ex: FLAC 96 kHz com LDAC conectado a 96 kHz), o reamostrador entra em modo **Passthrough direto** (zero atraso e zero perda matemática).
   * Se houver divergência (ex: arquivo em 48 kHz tocando em fone conectado em SBC 44.1 kHz), um acumulador de fase de 64 bits em ponto fixo 32.32 interpola as amostras com preservação de resíduo (*carry*), impedindo qualquer estalo ou ruído de fase.

---

## 🌊 Fluxo de um Chunk de Áudio em Tempo Real

1. **Recepção I2S Slave**: O driver `i2s_input` lê blocos de amostras sincronizados pelo clock gerado pela placa principal (Master).
2. **Buffer Acumulador**: `audio_pipeline_acc_feed()` agrupa as amostras até formar um chunk completo (128 amostras para 44.1/48 kHz ou 256 amostras para 88.2/96 kHz).
3. **Processamento (`on_chunk_ready`)**:
   * **Reamostragem**: Se a taxa de entrada for diferente da taxa negociada com o fone, o conversor 32.32 ajusta a amostragem.
   * **Codificação**:
     * Para **LDAC**: Amostras de 32 bits são codificadas diretamente via `ldac_enc_process_s32()`.
     * Para **SBC**: `audio_pipeline_extract16()` converte as amostras para `int16_t` antes de invocar `sbc_enc_process()`.
   * **Encapsulamento de Mídia**: `a2dp_media_write_header()` formata o cabeçalho de transporte RTP com contadores de sequência e timestamp.
   * **Transmissão Sem Fio**: `bt_source_send_media_packet()` despacha os bytes para a fila L2CAP/HCI do rádio Bluetooth.
   * **ABR Dinâmico**: Se o envio falhar por interferência no sinal, o módulo `bitrate_abr_report()` degrada preventivamente o bitrate do LDAC (990k → 660k → 330k) para não interromper a reprodução, recuperando os 990 kbps assim que o link estabilizar.
