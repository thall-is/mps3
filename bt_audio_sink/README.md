# bt_audio_sink — Receptor de Bancada A2DP para Validação do mps3

[![Baseado em](https://img.shields.io/badge/Baseado%20em-WillyBilly06%2FESP32--A2DP--SINK-blue.svg)](https://github.com/WillyBilly06/ESP32-A2DP-SINK-WITH-CODECS-UPDATED)
[![Hardware](https://img.shields.io/badge/Hardware-ESP32%20WROOM%20(Internal%20RAM)-darkblue.svg)](#requisitos-de-hardware)
[![Codecs](https://img.shields.io/badge/Codecs-LDAC%20(24b%2F96k)%20%7C%20aptX--HD%20%7C%20Opus%20%7C%20AAC%20%7C%20SBC-purple.svg)](#codecs-suportados)
[![Licença](https://img.shields.io/badge/Licenca-MIT-green.svg)](LICENSE)

Este subprojeto contém o firmware de um **Receptor de Áudio Bluetooth (A2DP Sink)** de alta fidelidade desenvolvido para o microcontrolador **ESP32 clássico**.

> 💡 **Atribuição Upstream:** Este módulo é baseado no projeto de código aberto [**ESP32-A2DP-SINK-WITH-CODECS-UPDATED**](https://github.com/WillyBilly06/ESP32-A2DP-SINK-WITH-CODECS-UPDATED) mantido por [**WillyBilly06**](https://github.com/WillyBilly06), distribuído sob a licença **MIT**. Agradecimentos à comunidade pelo desenvolvimento e refinamento desta pilha!

---

## 🎯 Finalidade no Ecossistema mps3

O `bt_audio_sink` é utilizado como **ferramenta de teste e análise de bancada** para validar a cadeia de áudio do **mps3**:

1. **Validação do Transmissor `bt_companion`:**
   * Permite parear o transmissor LDAC do mps3 diretamente com este microcontrolador, eliminando a necessidade de fones comerciais fechados durante o desenvolvimento.
2. **Telemetria e Auditoria de Pacotes:**
   * Exibe no monitor serial as estatísticas em tempo real de recepção de pacotes Bluetooth, taxa real de dados (kbps), perdas de pacotes e estabilidade de jitter.
3. **Saída I2S Analisável:**
   * Direciona o áudio digital PCM decodificado (inclusive em **24-bit / 96 kHz** no LDAC) para uma interface física **I2S Master**, permitindo a conexão direta com analisadores de áudio, osciloscópios ou placas de captura para auditar a pureza do sinal sonoro.

---

## 🎧 Codecs Suportados

O firmware integra decodificadores nativos para múltiplos codecs A2DP:
* **Sony LDAC**: até 96 kHz / 24-bit com ABR.
* **aptX e aptX-HD**: 44.1 / 48 kHz (16 e 24 bits).
* **aptX-LL (Low Latency)**: baixa latência para monitoramento.
* **Opus**: 48 kHz / 16-bit.
* **AAC**: 44.1 kHz estéreo.
* **SBC**: compatibilidade universal com padrão Bluetooth.

---

## 🔌 Pinagem Padrão (ESP32 Sink)

### Saída I2S Master (DAC de Bancada ou Analisador)
| Sinal I2S | Pino GPIO | Função |
|---|:---:|---|
| **BCLK (Bit Clock)** | **GPIO 26** | Clock de bits I2S gerado pelo sink |
| **WS / LRCK (Word Select)** | **GPIO 25** | Clock de canais (44.1 / 48 / 96 kHz) |
| **DOUT (Data Out)** | **GPIO 22** | Fluxo de dados PCM decodificado |

---

## 🔨 Como Compilar e Gravar

O projeto é configurado para o framework **ESP-IDF v5.x** e opera inteiramente na memória RAM interna do ESP32 (SRAM), não exigindo chip com PSRAM externa:

```bash
# 1. Navegue para a pasta do sink
cd 3_esp32_bt_audio_sink

# 2. Defina o alvo do microcontrolador
idf.py set-target esp32

# 3. Compile o firmware
idf.py build

# 4. Grave e monitore na porta serial (ajuste COM3 para a sua porta)
idf.py -p COM3 flash monitor
```

---

## 🔐 Segurança do Modo Recovery (Criptografia OTA)

O firmware de recuperação (`recovery/`) suporta atualização remota criptografada via AES-256-CBC:
1. Para gerar uma chave própria segura, execute:
   ```bash
   python tools/encrypt_firmware.py --generate-key
   ```
2. Copie o arquivo de exemplo `recovery/main/recovery_key.h.example` para `recovery/main/recovery_key.h` e insira sua chave.
3. Para empacotar novas atualizações sem commitar binários pesados no repositório:
   ```bash
   python tools/encrypt_firmware.py build/bt_audio_sink.bin --version 1.2.0
   ```
*(O repositório ignora arquivos `.enc`, `recovery_key.h` e `ota_key.bin` para segurança do usuário).*

---

## 📄 Licença e Créditos

Distribuído sob a licença **MIT**.
* Projeto original: [WillyBilly06/ESP32-A2DP-SINK-WITH-CODECS-UPDATED](https://github.com/WillyBilly06/ESP32-A2DP-SINK-WITH-CODECS-UPDATED)
* Licença do projeto original: **MIT License**
