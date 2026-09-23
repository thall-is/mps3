# Testes do ESP32 BT Companion

Esta pasta contém o ecossistema de testes automatizados do firmware de transmissão Bluetooth A2DP.

O design de testes foi estruturado para permitir verificação contínua e reprodutível **sem depender de hardware real nem de bancada física** para validação algorítmica e de tipos.

---

## 1. Estrutura de Testes

### [A] Testes Unitários de Host (`run_host_tests.sh`)
Cobrem a lógica pura dos componentes de áudio e protocolos de empacotamento:
1. **`ldac_enc_test`**: Valida a biblioteca nativa Sony LDAC (`ldaclib.c`, `ldacBT.c`), garantindo processamento em Q30 (24-bit PCM), alocação de tabelas e geração de pacotes nas taxas 330, 660 e 990 kbps.
2. **`sbc_enc_test`**: Valida o encoder SBC AOSP (Google), incluindo cálculo de bitpool, alocação SNR/Loudness e empacotamento de sub-bandas.
3. **`aptx_enc_test`**: Valida o encoder Qualcomm aptX Standard (filtro QMF 4 sub-bandas, quantização ADPCM).
4. **`aptx_hd_enc_test`**: Valida o encoder Qualcomm aptX HD (resolução de 24 bits, análise espectral QMF).
5. **`payload_test`**: Valida o empacotador de payload RTP A2DP (`a2dp_media_payload.c`), incluindo os cabeçalhos RTP vendor-specific, cálculo de timestamp de 90 kHz e fragmentação de quadros.
6. **`audio_pipeline_test`**: Valida o resampler ponto-fixo 32.32 (`audio_pipeline.c`), interpolação linear, conversão de taxa (ex: 48 kHz $\to$ 44.1 kHz, 96 kHz $\to$ 44.1/48 kHz) e ring buffers.
7. **`bitrate_abr_test`**: Valida o algoritmo de Adaptive Bitrate (`bitrate_abr.c`), máquina de estados de histerese e degradação suave de qualidade sob perda de pacotes.
8. **`integration_pipeline_host`**: Teste de ponta a ponta integrando Pipeline de Áudio $\to$ LDAC Encoder $\to$ Empacotador RTP A2DP.

### [B] Compile-Checks com Stubs ESP-IDF (`compile_check.sh`)
Verifica a integridade de sintaxe, tipos, structs, enums e assinaturas dos módulos que interagem com o sistema operacional e periféricos:
- `uart_ctrl.c` (driver UART e protocolo de comando/telemetria 0xAA 0x55)
- `i2s_input.c` (driver I2S slave)
- `bt_source.c` (stack A2DP Source, registro de SEPs, AVRCP Controller, scan de dispositivos)
- `main.c` (máquina de estados principal, persistência NVS e despacho)

Os stubs estão localizados em `esp_idf_stubs/include/` e reproduzem fielmente os cabeçalhos oficiais do ESP-IDF e FreeRTOS.

---

## 2. Como Rodar os Testes

### No Linux / macOS / WSL / GitHub Actions CI:
```bash
# Rodar os 8 testes unitários de host
./tests/run_host_tests.sh

# Rodar os compile-checks contra os stubs ESP-IDF
./tests/compile_check.sh
```

### No Windows ou Multiplataforma via Python:
```bash
# Executa tanto a verificação de compilação quanto execução nativa se GCC/Clang estiver presente:
python tests/run_tests.py
```
Se o host possuir GCC ou Clang, o script compila e executa todos os 8 binários de teste. Se estiver em ambiente Windows puro com toolchain PlatformIO, o script utiliza o compilador Xtensa para validar a compilação de todas as 12 metas com 100% de precisão.
