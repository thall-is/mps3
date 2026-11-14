# mps3 — Especificação do Protocolo Serial UART Inter-MCU

Este documento especifica o protocolo de comunicação serial ponto a ponto entre a **Placa Principal (ESP32-S3)** e o **Co-Processador Bluetooth (`bt_companion` — ESP32 Clássico)**.

---

## ⚙️ Camada Física

* **Padrão**: UART assíncrona full-duplex (`UART_NUM_1`).
* **Velocidade**: **115.200 bps**.
* **Configuração**: 8 bits de dados, sem paridade, 1 stop bit (**8N1**).
* **Tensão Lógica**: TTL 3.3V.
* **Pinagem**:
  * **ESP32-S3**: TX = GPIO 14, RX = GPIO 13, RESET = GPIO 12 (ativo em nível baixo).
  * **ESP32 Companion**: RX = GPIO 16, TX = GPIO 17, EN/CHIP_PU = conectado ao GPIO 12 do S3.

---

## 📦 Estrutura do Pacote Binário

Todos os comandos e eventos transmitidos utilizam o seguinte formato de quadro com delimitação e soma de verificação por CRC:

```text
+------------+------------+---------+--------------------+----------+
| SYNC_BYTE  | LEN        | CMD/EVT | PAYLOAD            | CRC8     |
| (1 byte)   | (1 byte)   | (1 byte)| (0 a 250 bytes)    | (1 byte) |
| 0xA5       | [N]        | [ID]    | [D0, D1, ... DN-1] | [CRC]    |
+------------+------------+---------+--------------------+----------+
```

* **SYNC_BYTE (`0xA5`)**: Marcador de início de quadro (1 byte).
* **LEN (1 byte)**: Quantidade de bytes de dados do payload (`0` a `250`).
* **CMD/EVT (1 byte)**: Identificador do comando enviado pelo S3 ou do evento enviado pelo Companion (`uart_ctrl_cmd_t`).
* **PAYLOAD (`LEN` bytes)**: Dados específicos da mensagem (opcional).
* **CRC8 (1 byte)**: Checksum calculado via algoritmo **CRC8/Maxim** (polinômio refletido `0x8C`, valor inicial `0x00`), cobrindo os bytes de `CMD/EVT` e todos os bytes de `PAYLOAD`.

### Algoritmo do CRC8/Maxim:
```c
static uint8_t crc8_maxim(const uint8_t *data, size_t len) {
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x01) ? (uint8_t)((crc >> 1) ^ 0x8C) : (uint8_t)(crc >> 1);
        }
    }
    return crc;
}
```

---

## 📋 Comandos da Placa Principal → Companion (`uart_ctrl_cmd_t`)

| ID | Nome do Comando | Tamanho do Payload | Conteúdo do Payload e Ação |
|:---:|---|:---:|---|
| `0x01` | `UART_CMD_PING` | 0 | Teste de conectividade (o Companion responde com `UART_CMD_PONG`). |
| `0x10` | `UART_CMD_PAIR_START` | 0 | Entra em modo de pareamento descobridor. |
| `0x11` | `UART_CMD_PAIR_STOP` | 0 | Interrompe o modo de pareamento. |
| `0x12` | `UART_CMD_CONNECT_KNOWN` | 0 | Conecta ao último fone de ouvido pareado salvo na memória NVS. |
| `0x13` | `UART_CMD_DISCONNECT` | 0 | Desconecta do fone de ouvido atualmente conectado. |
| `0x14` | `UART_CMD_FORGET_ALL` | 0 | Apaga a lista de dispositivos pareados na memória NVS. |
| `0x15` | `UART_CMD_SET_QUALITY` | 1 | Define qualidade de transmissão LDAC: `0=HIGH (990 kbps)`, `1=STANDARD (660 kbps)`, `2=MOBILE (330 kbps)`. |
| `0x16` | `UART_CMD_SET_SAMPLE_RATE` | 4 | Frequência de amostragem em Hz (`uint32` Little-Endian, ex: 44100, 48000, 88200, 96000) para configurar divisores I2S Slave. |
| `0x17` | `UART_CMD_GET_STATUS` | 0 | Solicita envio imediato de status (`UART_EVT_STATUS`). |
| `0x18` | `UART_CMD_SCAN_START` | 0 | Inicia busca ativa de fones Bluetooth próximos (`esp_gap_bt_start_discovery`). |
| `0x19` | `UART_CMD_SCAN_STOP` | 0 | Cancela a varredura ativa de dispositivos. |
| `0x1A` | `UART_CMD_CONNECT_ADDR` | 6 | Endereço MAC (`BDA`, 6 bytes) do fone selecionado no menu do MPS3. |
| `0x1B` | `UART_CMD_SET_VOLUME` | 1 | Volume absoluto (`0` a `100`%). Transmitido ao fone via AVRCP Absolute Volume. |
| `0x1C` | `UART_CMD_SET_CODEC` | 1 | Preferência de codec (`0=Auto`, `1=LDAC`, `2=aptX-HD`, `3=aptX`, `4=SBC`). |

---

## 📢 Eventos do Companion → Placa Principal (`uart_ctrl_cmd_t`)

| ID | Nome do Evento | Tamanho do Payload | Conteúdo do Payload e Significado |
|:---:|---|:---:|---|
| `0x81` | `UART_CMD_PONG` | 0 | Resposta imediata de vida ao comando `UART_CMD_PING`. |
| `0x90` | `UART_EVT_STATUS` | 11 | Estado do link: `uint8_t state` (`uart_link_state_t`), `uint8_t bt_addr[6]` e `uint32_t sample_rate` (Little-Endian). |
| `0x91` | `UART_EVT_SCAN_RESULT` | 39 | Fone descoberto: `uint8_t bda[6]`, `int8_t rssi` e `char name[32]` (null-terminated). |
| `0x92` | `UART_EVT_SCAN_COMPLETE` | 1 | Fim da varredura: `uint8_t count` (total de fones encontrados). |
| `0x93` | `UART_EVT_VOLUME_CHANGED` | 1 | Volume alterado pelos botões físicos do próprio fone: `uint8_t volume_percent` (0..100%). |
| `0x94` | `UART_EVT_CODEC_INFO` | 3 | Codec e bitrate efetivo: `uint8_t codec` (`codec_preference_t`) e `uint16_t bitrate_kbps` (Little-Endian). |

### Estados do Link (`uart_link_state_t`):
- `0 = UART_LINK_IDLE`: Desconectado e inativo.
- `1 = UART_LINK_PAIRING`: Modo pareamento aguardando fone.
- `2 = UART_LINK_SCANNING`: Realizando varredura ativa.
- `3 = UART_LINK_CONNECTING`: Conectando ao dispositivo selecionado.
- `4 = UART_LINK_CONNECTED_SBC`: Conectado e transmitindo via SBC.
- `5 = UART_LINK_CONNECTED_LDAC`: Conectado e transmitindo via Sony LDAC 24-bit / 96 kHz.
- `6 = UART_LINK_CONNECTED_APTX`: Conectado via aptX (reservado).
- `7 = UART_LINK_CONNECTED_APTX_HD`: Conectado via aptX HD (reservado).
