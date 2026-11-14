# Arquitetura do Sistema e Pipeline de Áudio — mps3

O **mps3** adota uma topologia de co-processamento distribuído (**Dual-MCU**) para contornar as restrições físicas dos microcontroladores de mercado e viabilizar um reprodutor de áudio audiófilo de padrão comercial.

---

## 1. Topologia Dual-MCU: Por que dois microcontroladores?

* **ESP32-S3 (Placa Principal / Player):**
  - **Processador:** Xtensa Dual-Core 32-bit LX7 @ 240 MHz com instruções SIMD vetoriais.
  - **Memória:** 16 MB Flash Quad-SPI + 8 MB Octal-SPI PSRAM a 80 MHz (largura de banda superior para descompressão FLAC, ring buffer de 1024 KB para WAV 384 kHz / 32-bit e heaps de rede).
  - **Periféricos:** Host SDMMC nativo de 4 bits operando a 40 MHz, barramento I2S Master DMA com saída estéreo PCM, display OLED I2C acelerado a 800 kHz (até 62.5 FPS), expansor I2C PCF8574 para botões dedicados de volume e joystick, USB nativo (TinyUSB CDC/DAC/MSC).
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
|  | MicroSD (SDMMC 4b) | -----> |  Decodificador Hi-Res | -----> |   Equalizador SVF  |  |
|  |  FLAC / MP3 / WAV  |        | (WAV 384k/32b, FLAC)  |        |   10 Bandas (384k) |  |
|  +--------------------+        +-----------------------+        +--------------------+  |
|                                            |                               |            |
|                                [RingBuffer 1MB PSRAM]                      v            |
|                                            |                    +--------------------+  |
|  +--------------------+        +-----------+-----------+        |  Driver I2S Master |  |
|  | Display OLED I2C   | <..... | Menu / GUI / Joystick |        |  DMA 32-bit Stereo |  |
|  | (SSD1306 a 62.5FPS)|        | + Expansor PCF8574    |        +---------+----------+  |
|  +--------------------+        +-----------------------+                  |             |
|                                            :                              | I2S 4-Fios  |
+--------------------------------------------:------------------------------:-------------+
                                             : (115200 8N1)                 | (MCLK 49M)  |
                                             v                              |             |
+--------------------------------------------------------------------+      |             |
|                     ESP32 Clássico (Co-Processador)                |      |             |
|                                                                    |      |             |
|  +--------------------+   +--------------------+   +-------------+ |      |             |
|  | Controle UART Link |   |  Reamostrador 32.32|   |  I2S Slave  | <------+ (Até 96 kHz)|
|  | (Core 0 - Estado)  |   | (96k/48k -> 44.1k) |   | (PIN 26/25) | |      |             |
|  +--------------------+   +--------------------+   +-------------+ |      |             |
|             |                         |                            |      |             |
|             v                         v                            |      |             |
|  +--------------------+   +--------------------+                   |      |             |
|  |  AVRCP Controller  |   | Multi-Codec Engine |                   |      |             |
|  |  (Absolute Volume) |   | LDAC/aptX-HD/aptX  |                   |      |             |
|  +--------------------+   +--------------------+                   |      |             |
|             |                         |                            |      |             |
|             +------------+------------+                            |      |             |
|                          v                                         |      |             |
|             +--------------------------+                           |      |             |
|             |  Pilha Bluedroid BT A2DP |                           |      |             |
|             |  Multi-SEP (4 SEIDs)     |                           |      |             |
|             +--------------------------+                           |      |             |
|                          |                                         |      |             |
|                          |                                         |      v             |
|                          |                                         +------------------+ |
|                          |                                         |   DAC PCM5102A   | |
|                          |                                         | (Até 384kHz/32b) | <+
|                          |                                         +------------------+ |
|                          |                                                (Line-Out P2) |
+--------------------------:--------------------------------------------------------------+
                           : Bluetooth A2DP / AVRCP
                           v
               +-----------------------+
               | Fone de Ouvido / Sink |
               | Bluetooth Sem Fio     |
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
* **Comutação Dinâmica:** Quando o S3 inicia uma nova faixa com taxa distinta, despacha `UART_CMD_SET_SAMPLE_RATE` (0x16). O co-processador ajusta imediatamente os divisores do periférico I2S Slave sem interrupção de áudio audível.

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

## 5. Distribuição de Tarefas (Core Pinning) & Otimizações de CPU

### ESP32-S3 (Player)
* **Core 0:** GUI OLED SSD1306 (sustentando até 62.5 FPS via I2C a 800 kHz), leitura do joystick e expansor I2C PCF8574, tarefa DSP de áudio (`audio_dsp_task`, prioridade 5 com Fast-Path de volume sem branching), VU meter balístico estéreo decimado (stride 8x em taxas > 192 kHz), servidor web Wi-Fi modular (`mps3.local`), pilha USB nativa (CDC/DAC/MSC).
* **Core 1:** Decodificação de arquivos de áudio em tempo real (`player_task`, prioridade 5: WAV até 32-bit/384 kHz, FLAC até 24-bit/192 kHz com FastSeek, MP3, AAC, M4A, OGG), pipeline assíncrono com ring buffer de 1024 KB em Octal PSRAM com cedência cooperativa de CPU, gatekeeper multinível de formato e alimentação DMA I2S Master.

### ESP32 Companion
* **Core 0:** Pilha Bluedroid (HCI, L2CAP, AVDTP, AVRCP), recepção e envio de pacotes de controle UART.
* **Core 1:** Tarefa de áudio dedicada (`audio_task`, prioridade 18, stack 8 KB): leitura I2S Slave, reamostrador 32.32, codificação LDAC/aptX/SBC e despacho de pacotes de mídia para a pilha de rádio.

### 5.1 Pipeline Assíncrono de I/O em PSRAM Octal e FastSeek
Para reproduzir áudio extremo em **WAV 32-bit / 384 kHz estéreo** (vazão contínua de **3.072.000 bytes/s**), o subsistema de leitura do cartão SDMMC opera com total desacoplamento da decodificação:
- **Ring Buffer de 1024 KB (1 MB) em PSRAM Octal (`ASYNC_IO_RING_SIZE`)**: Oferece margem contínua de 333 ms a 384 kHz / 32 bits e mais de 1 segundo a 192 kHz / 24 bits.
- **Notificação por Histerese**: A tarefa leitora (`audio_reader_task`, Core 0) só é acordada para leituras em lote no SDMMC quando o espaço livre no ring buffer for $\ge 64$ KB (`ASYNC_IO_READ_CHUNK`), eliminando acordares desnecessários a cada milissegundo.
- **Cedência Cooperativa de CPU**: Se o buffer já possuir mais de 256 KB preenchidos, a tarefa leitora cede a CPU com `vTaskDelay(1)`, garantindo que a tarefa gráfica (`display_task`, prioridade 3) sustente taxas entre 45 e 62.5 FPS mesmo em reproduções Hi-Res extremas.
- **FastSeek**: Em operações de salto temporal, o buffer assíncrono cancela imediatamente o prefetch em andamento e reancora a leitura no novo deslocamento de bytes, garantindo resposta tátil imediata ao usuário.

### 5.2 Gatekeeper Multinível de Formato & Safe Resume na NVS
- **Proteção Anti-Brick**: Arquivos codificados fora dos padrões suportados (como FLAC 32-bit, que não possui representação válida nos quadros da libFLAC) são interceptados logo na leitura de metadados (`play_track()`) e antes da alocação de buffers.
- **Safe Resume NVS**: A gravação de estado de retomada (`last_path`) na memória NVS só é realizada após a confirmação de cabeçalho válido (`header_ready = true`). Caso um arquivo rejeitado seja identificado, qualquer chave residual é expurgada da NVS, impedindo loops de watchdog ou travamentos em reinicializações.
- **Diagnóstico Visual na Rejeição**: Quando uma faixa excede os limites físicos de amostragem ou profundidade de bits, o display OLED exibe a tela de rejeição (`oled_display_show_unsupported_ex`) renderizando o monólito **SEELE 01 (`image_seele_bits`)** ao lado do badge do codec (`FLAC`, `WAV`) e o teto máximo suportado (`MAX 24-BIT / 192 KHZ` ou `MAX 32-BIT / 384 KHZ`).

### 5.3 Equalizador SVF Trapezoidal Linear (384 kHz), Decimação do VU Meter e Fast-Path de Volume
- **Equalizador SVF Trapezoidal Linear de 10 Bandas (Andrew Simper / Cytomic)**: Filtros *Peaking Bell* sintetizados em dupla precisão (`double` 64-bit, `g = tan(pi * f0 / Fs)`) e executados em `float` 32-bit em `IRAM_ATTR`, com ganhos de **-15 dB a +15 dB** por banda e Pre-Amp geral de **-30 dB a +30 dB**. Opera continuamente de **44.1 kHz a 384 kHz sem bypass** (rejeitando apenas bandas acima de `0.45 * Fs`), processado em tiles de 64 pares L/R (`EQ_TILE_PAIRS`) com canais intercalados para FPU ILP do Xtensa LX7, *double-buffering* lock-free (`s_eq_pool[2]`) e *clamp* seguro Q31 (`2147483520.0f`).
- **Decimação Balística (Stride 8x)**: Para frequências acima de 192 kHz (ex: WAV 384 kHz), o motor de análise de picos de áudio (`audio_meter_feed`) aplica stride 8x, preservando a taxa de amostragem efetiva de análise em 48 kHz e reduzindo em 50% o processamento do Core 0.
- **Fast-Path de Ganho**: Durante o ajuste de volume em `audio_dsp_task`, quando ambos os canais operam em volume nominal $\le 100\%$ (`final_L <= 32768 && final_R <= 32768`), o teste condicional de saturação `int64` é omitido, economizando mais de 2 milhões de instruções de branch por segundo.

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

Para atingir vazão sustentada de até **3,8 MB/s na escrita em cartão microSD** (FAT32 sobre SDMMC 4-bit com pré-alocação contígua `f_expand`) e **~1,51 MB/s em download HTTP** através do navegador web, o subsistema HTTP (`components/wifi_transfer/wifi_transfer.c`) implementa um pipeline assíncrono acoplado à PSRAM Octal e ao barramento SDMMC DMA:

0. **Gerenciamento Dinâmico de DMA Contíguo (`i2s_output_deinit`)**:
   - Ao preparar o pipeline de transferência pesada (`prepare_transfer_pipeline`), desaloca temporariamente os buffers DMA do I2S (~96 KB de DRAM interna contígua) para liberar SRAM DMA ao *bounce buffer* SDMMC, restaurando o subsistema de áudio automaticamente após a sessão Wi-Fi.
1. **Produtor / Consumidor TCP (`httpd` — Core 1)**:
   - Drena ou alimenta o socket TCP via `httpd_req_recv()`/`httpd_resp_send_chunk()` usando um buffer persistente em PSRAM (`s_recv_buf_persist`, **32 KB**, `UPLOAD_RECV_BUF_SIZE`) acoplado a um **RingBuffer elástico de até 4 MB na PSRAM Octal** (`s_rb_persist`, `UPLOAD_RINGBUF_SIZE`).
2. **Worker Unificado SDMMC DMA (Core 1 — `transfer_worker_task` / `"up_wr"`, Prioridade 4, Stack 16 KB em PSRAM e TCB em DRAM interna)**:
   - Opera tanto trabalhos de escrita (`WORKER_JOB_UPLOAD`, com pré-alocação contígua `f_expand` no FAT32 eliminando atualizações repetidas da tabela FAT) quanto de leitura (`WORKER_JOB_DOWNLOAD`).
   - Opera diretamente sobre um *bounce buffer* persistente alinhado em 16 bytes na SRAM DMA interna (`s_bounce_persist`, **até 64 KB / 128 setores ou 32 KB / 64 setores** via `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA`), executando rajadas SDMMC `CMD25`/`CMD18` sem realocação por setor.
3. **Isolamento de Pilha dos Handlers HTTP (`http_scratch_alloc`)**:
   - Todos os buffers temporários de caminhos UTF-8, serialização JSON e leitura de diretórios (`list_scratch_t`, `upload_scratch_t`, `dl_scratch_t`, `del_scratch_t`) são alocados dinamicamente na PSRAM via `http_scratch_alloc()`, garantindo baixo consumo de pilha na task `httpd` mesmo sob rajadas concorrentes de requisições do navegador.
4. **Sincronização Push de Podcasts v2.0 (`podcast_sync` + `mps3_sync`) e Dupla Verificação SHA-256**:
   - O MPS3 opera como servidor passivo (`podcast_sync.c` refatorado em máquina de estados não-bloqueante executada pela `display_task`, com **0 bytes** de alocação extra de pilha em DRAM interna), anunciando `_mps3._tcp` via mDNS (`mac`, `ver`, `proto=2`) quando ativado pelo menu ("Sincronizar Podcasts"), comando UART `sync`, ou automaticamente ao conectar o carregador TP4056 (respeitando *cooldown* de 6h em NVS).
   - O cliente multiplataforma (`tools/mps3_sync/`) descobre o MPS3 na LAN, consulta `GET /api/sync/manifest` (episódios presentes no SD + histórico `played_history` + espaço livre + bateria), reconcilia o livro-razão (`state.json`) e aplica a política determinística de retenção (**3 mais recentes de cada programa + todos os não ouvidos**).
   - **Commit Atômico em 3 Etapas com Duplo SHA-256 (`mps3_sha256_ctx_t` PSA com fallback em software)**:
     1. `PUT /api/upload?path=/Podcasts/.staging/<Show>/<ep>` com cabeçalho `X-Expected-SHA256`: calcula o SHA-256 por hardware (`psa_hash_setup`, com fallback em software) em tempo real durante a recepção na `transfer_worker_task`; em caso de divergência, remove o arquivo parcial e retorna `HTTP 422 Unprocessable Entity`.
     2. `POST /api/sync/verify`: relê o arquivo gravado fisicamente no cartão microSD (usando `s_bounce_persist` em SRAM DMA) e recalcula o SHA-256 antes do commit.
     3. `POST /api/rename`: move atomicamente de `/Podcasts/.staging/<Show>/<ep>` para `/Podcasts/<Show>/<ep>`, garantindo que o player nunca veja arquivos parciais (pastas iniciadas por `.` são ignoradas pelo `audio_player`).
5. **Central Web Modular Embarcada (`components/wifi_transfer/web/src/`)**:
   - Código-fonte organizado de forma 100% modular em `src/views/`, `src/css/` e `src/js/`, empacotado deterministicamente em Gzip nível 9 (`index_html_gz.h`, ~142 KB), servido diretamente da memória Flash com `Content-Encoding: gzip` e zero bytes de RAM dinâmica.
   - **Player de Áudio Web no Navegador**: Mini player inferior e visualizador FFT completo em canvas via Web Audio API para reprodução e streaming remoto de faixas contidas no MicroSD.
   - **Scraper e Sincronizador de Podcasts no Navegador (v3.0)**: Integração com Apple Podcasts API e extrator isolado do YouTube em JS para download e conversão direta no browser sem exigir servidores intermediários.
   - **Painel Analítico MPS3 Wrapped**: Retrospectiva musical com geração de Stories 1080p em Canvas e sincronização com smartphone via escaneamento de QR Code ISO-18004.

---

## 8. Organização de Diretórios do Repositório

```text
mps3/
├── components/          # Módulos do firmware ESP32-S3 (audio_player, wifi_transfer, oled_display, eq, board_io/pcf8574, etc.)
│   └── wifi_transfer/   # Servidor web e OTA backend
│       └── web/src/     # Código-fonte modular da Central Web (views, css, js)
├── espidf/              # Projeto principal ESP-IDF v6.0+ (CMakeLists.txt, sdkconfig.defaults, partitions.csv, main/)
├── src/                 # Espelho do ponto de entrada main.c para compatibilidade de build
├── bt_companion/        # Firmware do co-processador ESP32 Clássico (A2DP Source LDAC/aptX/SBC + AVRCP)
├── bt_audio_sink/       # Firmware auxiliar receptor Bluetooth A2DP Sink para testes de bancada
├── docs/                # Documentação técnica (ARCHITECTURE.md, PROCEDIMENTO_RECUPERACAO_SDCARD.md, WIRING.md, PROTOCOL.md)
├── tools/               # Ferramentas de desenvolvimento, empacotamento web e utilitários
│   ├── pack_web.py          # Montador e compactador GZIP modular (src/ -> index_html_gz.h)
│   ├── recover_mps3_sd.py   # Resgate automatizado de cartões MicroSD corrompidos (FOUND.000)
│   ├── web_dev_simulator.py # Servidor local para desenvolvimento e hot-reload da interface web
│   ├── yt_scraper_core.py   # Extrator de transmissões YouTube CLI e suíte de testes de canal
│   ├── wifi_speed_bench.py  # Benchmark de velocidade de rede (Sink/Source) e pipeline SD
│   ├── mps3_nav.py          # Automação HIL de navegação e captura de tela OLED via UART
│   ├── upload_album.py      # Utilitário CLI para envio de álbuns via HTTP PUT
│   ├── mobile_relay/        # Relay para sincronização via Hotspot Android/Termux
│   └── podcast_server/      # Servidor local de sincronização de podcasts
├── tests/               # Suítes de testes automatizados (Web API, Scraper, Wrapped, Shuffle/Repeat, MSC, OTA)
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

