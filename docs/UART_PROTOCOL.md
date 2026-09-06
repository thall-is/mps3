# Protocolo de Comunicação Serial UART — S3 🠄🠆 Companion

Interface serial assíncrona full-duplex configurada em **115.200 baud, 8 bits de dados, sem paridade, 1 stop bit (8N1)**.

---

## 1. Estrutura do Pacote Binário

Todos os frames transmitidos por ambos os lados utilizam sincronismo de 2 bytes e proteção por checksum:

| Campo | Tamanho | Valor / Descrição |
|---|:---:|---|
| **Sync 1** | 1 byte | `0xAA` (Prefixo de sincronismo 1) |
| **Sync 2** | 1 byte | `0x55` (Prefixo de sincronismo 2) |
| **Command ID** | 1 byte | Código identificador da instrução / evento |
| **Length** | 2 bytes | Tamanho do payload em Little-Endian (`uint16_t`) |
| **Payload** | *N* bytes | Dados específicos do comando |
| **Checksum** | 1 byte | Soma de verificação: `(0xAA + 0x55 + Cmd + LenL + LenH + ΣPayload) & 0xFF` |

---

## 2. Comandos da Placa Principal para o Companion (S3 ➔ ESP32)

| Comando | ID | Payload | Descrição |
|---|:---:|:---:|---|
| `UART_CMD_PING` | `0x01` | *Nenhum* | Testa presença do co-processador (responde com `PONG`). |
| `UART_CMD_SCAN_START` | `0x02` | *Nenhum* | Inicia busca de fones Bluetooth próximos (~12.8s). |
| `UART_CMD_SCAN_STOP` | `0x03` | *Nenhum* | Encerra a busca de fones imediatamente. |
| `UART_CMD_CONNECT_ADDR`| `0x04` | 6 bytes (`BD_ADDR`) | Conecta ao endereço MAC especificado. Cancela busca com segurança se ativa. |
| `UART_CMD_DISCONNECT` | `0x07` | *Nenhum* | Desconecta do fone ativo. |
| `UART_CMD_CONNECT_KNOWN`| `0x08` | *Nenhum* | Conecta ao último dispositivo salvo na NVS. |
| `UART_CMD_FORGET_ALL` | `0x09` | *Nenhum* | Apaga endereço MAC e chaves de segurança salvas na NVS. |
| `UART_CMD_SET_SAMPLE_RATE`| `0x0B` | 4 bytes (`uint32_t` LE) | Informa a taxa I2S exata da faixa atual (ex: `44100`, `96000`). |
| `UART_CMD_GET_STATUS` | `0x0C` | *Nenhum* | Requisita status completo imediato. |
| `UART_CMD_SET_VOLUME` | `0x0D` | 1 byte (`0..100`) | Ajusta volume absoluto do fone via AVRCP Controller. |
| `UART_CMD_SET_CODEC` | `0x1C` | 1 byte (`uint8_t`) | Configura preferência de codec:<br>• `0`: Automático (Auto-negociação de maior fidelidade)<br>• `1`: Forçar Sony LDAC<br>• `2`: Forçar Qualcomm aptX HD<br>• `3`: Forçar Qualcomm aptX<br>• `4`: Forçar SBC Padrão |

---

## 3. Eventos e Notificações do Companion para a Placa Principal (ESP32 ➔ S3)

| Evento | ID | Payload | Descrição |
|---|:---:|:---:|---|
| `UART_CMD_PONG` | `0x81` | *Nenhum* | Confirmação de vida em resposta a `PING`. |
| `UART_EVT_STATUS` | `0x82` | 11 bytes | Notificação periódica ou após alteração de estado:<br>• `state` (1B): Estado da máquina (`IDLE`, `SCANNING`, `CONNECTED`, etc.)<br>• `bt_addr` (6B): MAC do fone ativo<br>• `sample_rate` (4B LE): Frequência do codec |
| `UART_EVT_SCAN_RESULT` | `0x83` | 39 bytes | Notifica fone encontrado:<br>• `bda` (6B): Endereço MAC<br>• `rssi` (1B com sinal): Intensidade do sinal em dBm<br>• `name` (32B): Nome legível do dispositivo |
| `UART_EVT_SCAN_COMPLETE`| `0x84` | *Nenhum* | Notifica término da varredura de fones. |
| `UART_EVT_VOLUME_CHANGED`| `0x85` | 1 byte (`0..100`) | Usuário alterou o volume físico na concha do fone via AVRCP. O S3 atualiza o slider da tela. |
| `UART_EVT_CODEC_INFO` | `0x94` | 7 bytes | **Telemetria de Codec em Tempo Real:**<br>• `codec_id` (1B): `1`=LDAC, `2`=aptX HD, `3`=aptX, `4`=SBC<br>• `bitrate_kbps` (2B LE): Bitrate efetivo negociado (ex: `990`, `660`, `330`, `576`, `328`)<br>• `sample_rate` (4B LE): Taxa de amostragem de transmissão |
