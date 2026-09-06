# mps3 — Especificação do Protocolo Serial UART

Este documento especifica o protocolo de comunicação serial ponto a ponto entre a **Placa Principal (ESP32-S3)** e o **Co-Processador Bluetooth (`bt_companion`)**.

---

## ⚙️ Camada Física

* **Padrão**: UART assíncrona full-duplex.
* **Velocidade**: **115.200 bps**.
* **Configuração**: 8 bits de dados, sem paridade, 1 stop bit (**8N1**).
* **Tensão Lógica**: TTL 3.3V.

---

## 📦 Estrutura do Pacote Binário

Todos os comandos e eventos transmitidos utilizam o seguinte formato de quadro:

```
+------------+------------+---------+----------+--------------------+----------+
| SYNC_1     | SYNC_2     | CMD/EVT | LEN      | PAYLOAD            | CHECKSUM |
| (1 byte)   | (1 byte)   | (1 byte)| (1 byte) | (0 a 32 bytes)     | (1 byte) |
| 0xAA       | 0x55       | [ID]    | [N]      | [D0, D1, ... DN-1] | [XOR]    |
+------------+------------+---------+----------+--------------------+----------+
```

* **SYNC_1 (`0xAA`) e SYNC_2 (`0x55`)**: Marcadores de 2 bytes para início do quadro.
* **CMD/EVT (1 byte)**: Identificador do comando enviado pelo S3 ou do evento enviado pelo Companion.
* **LEN (1 byte)**: Quantidade de bytes de dados (`0` a `32`).
* **PAYLOAD (`LEN` bytes)**: Dados específicos da mensagem.
* **CHECKSUM (1 byte)**: Soma de verificação calculada pelo **XOR de todos os bytes** desde `CMD` até o final de `PAYLOAD`.

---

## 📋 Comandos da Placa Principal → Companion (`uart_ctrl_cmd_t`)

| ID | Nome do Comando | Tamanho do Payload | Conteúdo do Payload e Ação |
|:---:|---|:---:|---|
| `0x01` | `UART_CMD_PING` | 0 | Teste de comunicação (o Companion deve responder com `PONG`). |
| `0x02` | `UART_CMD_PONG` | 0 | Resposta de vida. |
| `0x03` | `UART_CMD_SCAN_START` | 0 | Inicia a busca por fones de ouvido Bluetooth próximos. |
| `0x04` | `UART_CMD_SCAN_STOP` | 0 | Encerra a busca por dispositivos. |
| `0x05` | `UART_CMD_CONNECT_ADDR`| 6 | Endereço MAC (6 bytes) do fone selecionado no menu do S3. |
| `0x06` | `UART_CMD_CONNECT_KNOWN`| 0 | Conecta ao último dispositivo pareado salvo na memória NVS. |
| `0x07` | `UART_CMD_DISCONNECT` | 0 | Desconecta o dispositivo Bluetooth atual. |
| `0x08` | `UART_CMD_GET_STATUS` | 0 | Solicita o estado da conexão e do codec ativo. |
| `0x09` | `UART_CMD_SET_QUALITY`| 1 | Define a qualidade LDAC: `0=HIGH (990k)`, `1=STANDARD (660k)`, `2=MOBILE (330k)`. |
| `0x0A` | `UART_CMD_FORGET_ALL` | 0 | Limpa todos os dispositivos pareados salvos na memória NVS. |
| `0x0B` | `UART_CMD_SET_SAMPLE_RATE`| 4 | Frequência de amostragem em Hz (uint32 Little-Endian) para configurar o clock I2S. |
| `0x0C` | `UART_CMD_SET_VOLUME` | 1 | Nível de volume em percentual (`0` a `100`). Repassado ao fone via AVRCP. |
| `0x0D` | `UART_CMD_SET_CODEC` | 1 | Codec preferencial: `0=Auto`, `1=LDAC`, `2=aptX-HD`, `3=aptX`, `4=SBC`. |

---

## 📢 Eventos do Companion → Placa Principal (`uart_ctrl_evt_t`)

| ID | Nome do Evento | Tamanho do Payload | Conteúdo do Payload e Significado |
|:---:|---|:---:|---|
| `0x80` | `UART_EVT_STATUS` | 8 | Estado da conexão (`uint8`), endereço MAC (`6 bytes`) e taxa de amostragem (`uint8`). |
| `0x81` | `UART_EVT_SCAN_RESULT`| 1 a 32 | Dispositivo encontrado: `MAC (6 bytes)` + `RSSI (int8)` + `Nome do Dispositivo (string)`. |
| `0x82` | `UART_EVT_SCAN_COMPLETE`| 0 | Notifica que a busca terminou para atualizar o menu no display. |
| `0x83` | `UART_EVT_VOLUME_CHANGED`| 1 | Notifica que o usuário alterou o volume físico no fone de ouvido (`0` a `100%`). |
| `0x84` | `UART_EVT_CODEC_INFO` | 3 | Codec negociado (`uint8`) e taxa de transferência atual em kbps (`uint16 LE`). |
