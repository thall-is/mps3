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

---

## 6. Arquitetura de Memória Flash, Dual-Bank e Rollback OTA

O ESP32-S3 conta com 16 MB de memória Flash Quad-SPI (`partitions.csv` / `espidf/partitions.csv`), estruturada para atualizações remotas seguras e autônomas:

```csv
# Name,     Type, SubType,  Offset,   Size,     Flags
nvs,      data, nvs,      0x9000,   0x6000,
phy_init, data, phy,      0xf000,   0x1000,
otadata,  data, ota,      0x10000,  0x2000,
ota_0,    app,  ota_0,    0x20000,  0x400000,
ota_1,    app,  ota_1,    0x420000, 0x400000,
```

### Princípios do Subsistema OTA

1. **Dual-Bank Assíncrono com Dois Bancos de 4 MB**:
   - As partições `ota_0` e `ota_1` possuem 4.194.304 bytes cada, suportando o executável com folga de mais de 50%.
   - A escrita ocorre sequencialmente na partição inativa (`esp_ota_get_next_update_partition()`), sem interromper a execução do sistema até a confirmação final.

2. **Validação Pré-Flash e Verificação de Projeto**:
   - Antes de iniciar o apagamento de blocos de Flash, o backend OTA (`wifi_transfer.c`) valida o cabeçalho inicial de 288 bytes:
     - Magic Byte ESP32: `0xE9`
     - Magic Word `esp_app_desc_t`: `0xABCD5432`
     - Pertencimento ao projeto: `project_name == "mps3"`
   - Binários corrompidos, incompletos ou compilados para outro projeto são rejeitados imediatamente (`HTTP 400 Bad Request`), preservando a partição intacta.

3. **Mecanismo Anti-Brick e Cancelamento de Rollback**:
   - Compilado com `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`.
   - Ao reiniciar, o bootloader inicializa a partição recém-gravada no estado `ESP_OTA_IMG_PENDING_VERIFY`.
   - Se o novo firmware falhar ao inicializar, entrar em pânico ou resetar pelo watchdog, o bootloader reverte automaticamente o boot para a partição anterior saudável.
   - Apenas quando o hardware inicializa com sucesso o display OLED, o host SDMMC, os buffers DMA I2S e as tarefas FreeRTOS, a rotina `esp_ota_mark_app_valid_cancel_rollback()` é acionada, consolidando a nova versão em definitivo.

4. **Identificação e Auto-Descoberta por MAC de Hardware**:
   - O endpoint `GET /api/ota` reporta o MAC físico (`28:84:85:52:35:84`).
   - O utilitário CLI `versionamento/ota_upload.py` varre as interfaces locais (tabela ARP e ping sweep ativo) para localizar e autenticar o dispositivo automaticamente, permitindo atualização transparente sem dependência de IP fixo ou consultas manuais a roteadores.

---

## 7. Pipeline Dual-Core de Transferência Wi-Fi e Servidor Web (`wifi_transfer`)

Para atingir vazão sustentada de **1,30 a 1,46 MB/s** na escrita em cartão microSD (FAT32 sobre SDMMC 4-bit) através do navegador web, o subsistema HTTP (`components/wifi_transfer/wifi_transfer.c`) implementa um pipeline assíncrono acoplado ao Cache L1 de 32 KB da PSRAM:

1. **Produtor TCP (Core 1 — `httpd`, Prioridade 5, Stack 8 KB em DRAM)**:
   - Drena o socket TCP via `httpd_req_recv()` usando um buffer enxuto persistente (`s_recv_buf_persist`, 4–8 KB) e empurra os blocos imediatamente para um **RingBuffer elástico de 3 MB na PSRAM Octal** (`s_rb_persist`), mantendo a janela de transferência 100% quente no Cache L1 de 32 KB da PSRAM (`memcpy` a **44,4 MB/s**).
2. **Consumidor SDMMC DMA (Core 0 — `upload_writer_task`, Prioridade 5, Stack 8 KB em PSRAM)**:
   - Cria diretórios (`mkdir_p_for_file`) e abre o arquivo (`fopen`) em paralelo no Core 0 enquanto o Core 1 já recebe os primeiros pacotes TCP (`open_ms <= 4 ms`).
   - Consome fatias de **16 KB (32 setores)** para um *bounce buffer* persistente alinhado em SRAM DMA interna (`s_bounce_persist`, `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA`), executando comandos SDMMC `CMD25` (*Write Multiple Block*) diretos sem realocação por setor.
3. **Isolamento de Pilha dos Handlers HTTP (`http_scratch_alloc`)**:
   - Todos os buffers temporários de caminhos UTF-8, serialização JSON e leitura de diretórios (`list_scratch_t`, `upload_scratch_t`, `dl_scratch_t`, `del_scratch_t`) são alocados dinamicamente na PSRAM via `http_scratch_alloc()`, garantindo consumo de pilha `< 128 bytes` na task `httpd` mesmo sob rajadas concorrentes de requisições do navegador.

---

## 8. Organização de Diretórios do Repositório

```text
mps3/
├── components/          # Módulos do firmware ESP32-S3 (audio_player, wifi_transfer, oled_display, eq, usb_manager, etc.)
├── espidf/              # Projeto principal ESP-IDF v6.0+ (CMakeLists.txt, sdkconfig.defaults, partitions.csv, main/)
├── src/                 # Espelho do ponto de entrada main.c para compatibilidade de build
├── bt_companion/        # Firmware do co-processador ESP32 Clássico (A2DP Source LDAC/aptX/SBC + AVRCP)
├── bt_audio_sink/       # Firmware auxiliar receptor Bluetooth A2DP Sink para testes de bancada
├── docs/                # Documentação técnica (ARCHITECTURE.md, PROTOCOL.md, WIRING.md, Doxyfile)
├── tools/               # Ferramentas de desenvolvimento, empacotamento web e benchmarks em tempo real
│   ├── pack_web.py          # Compactador GZIP (index.html -> include/index_html_gz.h)
│   ├── wifi_speed_bench.py  # Benchmark de rede (Sink/Source), pipeline SD e fluxo humano de navegador web
│   ├── mps3_nav.py          # Automação HIL de navegação e captura de tela OLED via UART
│   ├── testbench_monitor.py # Monitor serial de telemetria de bancada
│   ├── upload_album.py      # Utilitário CLI para envio de álbuns via HTTP PUT
│   ├── mobile_relay/        # Relay para sincronização via Hotspot Android/Termux
│   └── podcast_server/      # Servidor e scraper automatizado de podcasts
├── tests/               # Suítes de testes automatizados (Web API, OTA MAC Discovery, Play/Pause, USB MSC, Navegação)
└── versionamento/       # Automação de build, versionamento de binários e deploy OTA/Serial (mps3_version.ps1/.sh, ota_upload.py)
```

---

## 9. Arquitetura de Segurança e Proteções Embarcadas

1. **Gestão de Segredos e Credenciais Wi-Fi**:
   - Nenhuma senha de rede doméstica é versionada no código-fonte (`sdkconfig` local e `.mps3_last_ip` são estritamente ignorados pelo `.gitignore`; `Kconfig.projbuild` mantém `WIFI_STA_SSID` e `WIFI_STA_PASSWORD` vazios por padrão).
   - O endpoint `GET /api/wifi/networks` jamais expõe as senhas salvas em NVS, retornando apenas os nomes das redes (`ssid`) devidamente escapados via `json_escape()`.
   - Os comandos de diagnóstico UART (`nets` e `net-set`) mascaram as senhas (`********`) nos logs seriais para evitar vazamento acidental em capturas de terminal.
2. **Proteção contra *Path Traversal* e Truncamento de Caminhos (`build_abs_path` & `url_decode`)**:
   - Todas as rotas de arquivo (`/api/list`, `/api/upload`, `/api/download`, `/api/delete`, `/api/mkdir`, `/api/rename`, `/api/copy`) validam o caminho relativo através de `build_abs_path()`, que rejeita sequências `..`, barras invertidas `\`, caracteres de controle ASCII (`< 0x20`) e qualquer truncamento de buffer (`snprintf >= out_len`).
   - `url_decode()` higieniza caracteres proibidos pelo sistema de arquivos FAT32 (`?`, `*`, `:`, `<`, `>`, `|`, `"`, `U+FF1F`).
3. **Mitigação de Negação de Serviço (DoS) e Exaustão de Sockets/Memória**:
   - O streaming de áudio (`GET /api/download`) aplica `SO_SNDTIMEO = 8s` e `Connection: close`, impedindo que conexões `<audio>` pausadas pelo navegador retenham a thread do servidor HTTP.
   - O upload (`PUT /api/upload`) é protegido pelo mutex `s_upload_lock` (retornando `HTTP 503` a tentativas concorrentes) e suspende varreduras pesadas da FAT (`esp_vfs_fat_info()`) durante a escrita ativa.
4. **Integridade de Firmware OTA**:
   - `POST /api/ota` exige bateria mínima segura (`>= 15%` ou carregador conectado), valida os cabeçalhos binários da imagem ESP-IDF (`0xE9`, `0xABCD5432`, `project_name == "mps3"`) antes de tocar na Flash e conta com *rollback* automático de bootloader (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`).

