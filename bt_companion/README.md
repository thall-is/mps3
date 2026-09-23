# bt_companion — Co-Processador de Transmissão Bluetooth do mps3

Este diretório contém o firmware do transmissor sem fio do **mps3**, desenvolvido para o **ESP32 clássico (ESP32-D0WD-V3)**.

---

## 🎯 Por Que um Segundo Microcontrolador?

O processador principal (**ESP32-S3**) conta apenas com rádio **Bluetooth Low Energy (BLE)**. A transmissão de áudio sem fio de alta resolução com fones de ouvido comerciais requer o perfil **A2DP**, que roda sobre a pilha de **Bluetooth Clássico (BR/EDR)** — inexistente no hardware do S3.

O `bt_companion` atua como uma ponte transparente de rádio de altíssima performance:
* Recebe o áudio PCM digital puro diretamente do S3 através do barramento físico **I2S em modo Slave**.
* Troca comandos e status com o S3 através de uma porta serial **UART dedicada**.
* Executa a pilha Bluetooth A2DP Source com suporte ao codec audiófilo **Sony LDAC em 24-bit / 96 kHz** e **SBC**.

---

## ⚠️ Transparência de Codecs: O Que Funciona

| Codec | Resolução & Frequência | Bitrate Típico | Status Real no mps3 |
|---|:---:|:---:|:---:|
| **Sony LDAC** | **24-bit / 96 kHz** (e 44.1/48k) | **330, 660 até 990 kbps** | ✅ **100% Funcional & Hi-Res Real** |
| **SBC** | 16-bit / 44.1 ou 48 kHz | ~328 kbps (Bitpool 53) | ✅ **100% Funcional (Compatibilidade Universal)** |
| **Qualcomm aptX** | 16-bit / 44.1 kHz | 352 kbps | ❌ **Não funcional nesta versão** |
| **Qualcomm aptX HD** | 24-bit / 48 kHz | 576 kbps | ❌ **Não funcional nesta versão** |

> **Nota sobre aptX e aptX HD**: Embora o projeto contenha os arquivos fonte dos encoders aptX, a negociação de handshake e o empacotamento RTP AVDTP Vendor da Qualcomm não concluem de forma estável com fones comerciais. No momento, o mps3 transmite áudio de alta fidelidade via **Sony LDAC** e assegura retrocompatibilidade total com **SBC**.

---

## 🔬 A Tecnologia do Sony LDAC no mps3

1. **Entrada Nativa em 24-bit (`LDACBT_SMPL_FMT_S32`)**:
   * O codificador da Sony recebe diretamente as amostras de 32 bits vindas da interface I2S Slave (`ldac_enc_process_s32`).
   * A biblioteca converte os dados para ponto fixo Q30 (`val >> 1`), preservando integralmente os **24 bits mais significativos de resolução nativa**, sem rebaixamento para 16 bits.
2. **Taxa de Bits Adaptativa (ABR) em Tempo Real**:
   * O driver monitora o fluxo de envio para o rádio. Se o canal sofrer instabilidade ou interferência eletromagnética, o ABR ajusta rapidamente o bitrate:
     * **HQ (Qualidade Máxima)**: 990 kbps (áudio mestre sem compressão perceptível).
     * **SQ (Qualidade Padrão)**: 660 kbps (ótimo equilíbrio entre qualidade e alcance).
     * **MQ (Prioridade de Sinal)**: 330 kbps (mantém o som tocando sem estalos).
   * Conforme o sinal é restabelecido, o bitrate retorna de forma transparente para os 990 kbps.
3. **Reamostragem Contínua em Ponto Fixo 32.32 (`audio_pipeline`)**:
   * Quando uma faixa em 96 kHz toca em um fone conectado via LDAC 96 kHz, o fluxo entra em **Passthrough direto** (zero conversão e zero atraso).
   * Caso haja diferença entre a taxa da faixa e a suportada pelo fone (ex: áudio em 48 kHz e fone conectado em SBC 44.1 kHz), um conversor interno em ponto fixo faz a adaptação com fidelidade espectral.

---

## 🧩 Patches Obrigatórios do ESP-IDF Bluedroid

Para que a pilha Bluedroid do ESP-IDF aceite registrar o endpoint do Sony LDAC (`ESP_A2D_MCT_NON_A2DP`), aplique os 4 patches disponíveis na pasta `esp-idf-patches/`:

1. **`0001-add-ldac-vendor-cie-to-esp_a2d_mcc_t.patch`**: Adiciona o suporte à estrutura de capacidade de codecs proprietários (Vendor CIE).
2. **`0002-bump-avdt-codec-size-for-ldac.patch`**: Expande os buffers internos de canal AVDTP para acomodar os parâmetros estendidos da Sony.
3. **`0003-add-non-a2dp-vendor-codec-to-btc_av_reg_sep.patch`**: Habilita o registro do Stream Endpoint proprietário (`ESP_A2D_MCT_NON_A2DP`) no Bluedroid.
4. **`0004-vendor-codec-buff-alloc-send-and-rtp-header.patch`**: Implementa o despacho de pacotes de mídia para o codec LDAC na camada BTC/BTA sem duplicação de cabeçalho RTP e define prioridade Hi-Res.

### Como aplicar:
```bash
cd /seu/caminho/esp-idf
git apply /caminho/para/mps3/2_esp32_bt_companion/esp-idf-patches/*.patch
```

---

## 🧪 Testes no Computador (Host Tests)

Você pode verificar e testar todo o pipeline de áudio, os encoders e o ABR direto no computador:
```bash
cd tests
./run_host_tests.sh
```
O script roda 7 testes unitários compilados com GCC nativo, validando a codificação LDAC, o empacotamento RTP e o alinhamento de amostras.

---

## 🔌 Pinagem do ESP32 Companion

| Sinal | Pino GPIO | Conexão Externa |
|---|:---:|---|
| **I2S BCLK** | **GPIO 26** | Conectado ao GPIO 48 do ESP32-S3 (Bit Clock) |
| **I2S WS (LRCK)** | **GPIO 25** | Conectado ao GPIO 21 do ESP32-S3 (Word Select) |
| **I2S DIN** | **GPIO 22** | Conectado ao GPIO 47 do ESP32-S3 (Dado PCM) |
| **UART RX** | **GPIO 16** | Conectado ao GPIO 14 (TX) do ESP32-S3 |
| **UART TX** | **GPIO 17** | Conectado ao GPIO 13 (RX) do ESP32-S3 |
| **EN / CHIP_PU** | **EN** | Conectado ao GPIO 12 do ESP32-S3 (Reset de hardware) |

---

## 🔨 Como Compilar e Gravar

```bash
idf.py set-target esp32
idf.py build
idf.py -p COM12 flash monitor
```
