# mps3 v2.5 — Player de Áudio Digital Hi-Res (DAP) Dual-MCU com DAC 32-bit / 384 kHz, FastSeek e Transmissão Sony LDAC 24-bit / 96 kHz

[![Dispositivo](https://img.shields.io/badge/Dispositivo-mps3%20v2.5-blue.svg)](#visao-geral)
[![Hardware](https://img.shields.io/badge/Hardware-ESP32--S3%20%2B%20ESP32-darkblue.svg)](docs/WIRING.md)
[![Hi-Res Audio DAC](https://img.shields.io/badge/DAC%20PCM5102A-32--bit%20%2F%20384kHz%20(4--Fios)-gold.svg)](#o-que-funciona-muito-bem)
[![FastSeek](https://img.shields.io/badge/FLAC%20FastSeek-24b%2F192k-blueviolet.svg)](#o-que-funciona-muito-bem)
[![Central Web](https://img.shields.io/badge/Central%20Web-Modular%20%7C%20Scraper%20%7C%20Player-cyan.svg)](#o-que-funciona-muito-bem)
[![Bluetooth](https://img.shields.io/badge/Bluetooth-Sony%20LDAC%20(24b%2F96k%20%7C%20990kbps)%20%7C%20SBC-purple.svg)](#o-que-funciona-muito-bem)
[![Power DFS](https://img.shields.io/badge/Power-DFS%20(40--240MHz)%20%7C%20Deep%20Sleep-brightgreen.svg)](#o-que-funciona-muito-bem)
[![Licença](https://img.shields.io/badge/Licenca-MIT%20%2F%20Apache%202.0-green.svg)](LICENSE)

O **mps3** é um player de áudio digital portátil de alta fidelidade (**Hi-Res Digital Audio Player / DAP**) de arquitetura aberta, construído em torno de **dois microcontroladores (Dual-MCU)** e um pipeline interno **Dual-Core assimétrico**. O projeto foi projetado para entregar uma experiência sonora pura e sem perdas, reproduzindo músicas do cartão micro SD com resolução de estúdio tanto pela saída analógica cabeada via DAC dedicado TI PCM5102A em até **32-bit / 384 kHz reais** (modo 4-fios com MCLK dedicado), quanto sem fio pelo Bluetooth com o codec audiófilo **Sony LDAC em 24-bit / 96 kHz**.

A divisão de trabalho entre processadores e núcleos independentes evita gargalos de desempenho e garante áudio fluido sem qualquer engasgo:

1. **Placa Principal (ESP32-S3 N16R8 — Dual-Core Xtensa LX7 @ 240 MHz)**:
   * **Core 1 (Player & I/O Task)**: Leitura de alta velocidade do micro SD (SDMMC 4 vias a 40 MHz), pipeline assíncrono com ring buffer de 1 MB em PSRAM Octal e decodificação contínua de formatos Hi-Res (**WAV até 32-bit / 384 kHz nativo**, **FLAC 24-bit até 192 kHz com FastSeek**, MP3, AAC, M4A, OGG) com janela deslizante de cópias reduzidas e gatekeeper multinível anti-brick.
   * **Core 0 (DSP, Áudio DMA & Interface)**: Tarefa dedicada de processamento de áudio (`audio_dsp_task`) gerenciando volume logarítmico com Fast-Path, balanço estéreo, equalizador SVF trapezoidal linear de 10 bandas (-15 a +15 dB, operando continuamente até 384 kHz sem bypass), VU meter balístico estéreo decimado (stride 8x), entrega contínua nos descritores DMA do I2S, interface gráfica OLED SSD1306 fluida até 62.5 FPS via I2C a 800 kHz e leitura de joystick/expansor I2C PCF8574 sem latência.
   * Conta também com subsistema **USB TinyUSB nativo** em modos exclusivos (Serial CDC, USB DAC UAC2 e USB Mass Storage) e Central Web embarcada modular com player web, raspador de podcasts no navegador e analytics.

2. **Co-Processador de Bluetooth (`bt_companion` — ESP32 Clássico)**:
   Dedicado exclusivamente ao rádio sem fio. Recebe o áudio PCM digital vindo do S3 via I2S, processa a codificação em tempo real com **Sony LDAC 24-bit / 96 kHz** (com taxa dinâmica adaptativa entre 330 e 990 kbps) ou **SBC** de alta qualidade, além de sincronizar o volume absoluto do fone via AVRCP.

3. **Receptor de Validação (`bt_audio_sink` — ESP32 Clássico)**:
   Firmware auxiliar de bancada para receber, auditar e testar a integridade e latência dos pacotes transmitidos pelo `mps3`.

---

## ⚠️ Transparência do Projeto: O Que Funciona e Limitações Atuais

Este projeto preza pela honestidade técnica e relata com clareza o estado real de cada funcionalidade comprovado em bancada:

### ✅ O Que Funciona Muito Bem

* **Saída Cabeada Hi-Res Extrema (PCM5102A até 32-bit / 384 kHz e FLAC 24-bit / 192 kHz)**: DAC Texas Instruments PCM5102A operando em modo **4-fios com Master Clock (MCLK) dedicado no GPIO 8** e clock base `PLL_240M` (multiplicadores de clock até 49.152 MHz para 384 kHz). Reproduz linearmente arquivos WAV PCM até **32-bit / 384 kHz** e FLAC comprimido sem perdas até **24-bit / 192 kHz**, sustentado por um ring buffer de I/O assíncrono de **1 MB em PSRAM Octal** com taxa de leitura de mais de 3.0 MB/s.
* **Busca Veloz e Responsiva (FastSeek)**: Algoritmo de busca rápida por syncwords e quadros de metadados em faixas FLAC e WAV, permitindo pular trechos de áudio sem travamentos e com cancelamento imediato de prefetch.
* **Gatekeeper Multinível Anti-Brick, Tela SEELE 01 & Safe Resume em NVS**: Proteção de hardware ativa contra arquivos fora de especificação (ex: FLAC 32-bit ou taxas acima dos limites físicos). O player rejeita a faixa antes da alocação de buffers, cancela prefetch em andamento, expurga chaves residuais de boot-loop na NVS e renderiza a tela de diagnóstico visual com o monólito **SEELE 01 (`image_seele_bits`)** e badges de limite (`FLAC MAX 24-BIT / 192 KHZ` e `WAV MAX 32-BIT / 384 KHZ`).
* **Modos de Reprodução Shuffle & Repeat / Loop**:
  * **Shuffle**: Desativado (`OFF`), Pasta (`FOLDER` com permutação pseudoaleatória Fisher-Yates alocada em PSRAM Octal sem repetições até esgotar a pasta) e Global (`ALL` em todo o acervo do cartão SD).
  * **Repeat**: Desativado (`NONE`), Repetir Lista (`ALL`) e Repetir Faixa 1x (`ONE` com liberação automática de loop ao avançar manualmente).
  * Status e modo ativos exibidos dinamicamente na tela Now Playing e no menu de contexto.
* **Display OLED Fluido a até 62.5 FPS com VU Meter Decimado**: Interface gráfica SSD1306 operando a alta taxa de quadros (tempo de quadro entre 1.3 ms e 17.4 ms via barramento I2C acelerado a 800 kHz). Conta com VU meter estéreo balístico com decimação dinâmica de 8x para frequências >192 kHz, mantendo o consumo de CPU do Core 0 estável.
* **Áudio Hi-Res Sem Fio em 24-bit / 96 kHz via Sony LDAC**: Arquivos FLAC de 24 bits e 96.000 Hz são lidos do micro SD e transmitidos via I2S para o co-processador Bluetooth, que codifica pelo encoder Sony LDAC nativamente em 24-bit / 96 kHz com até **990 kbps**, chegando ao fone sem perda de resolução.
* **Sony LDAC com Bitrate Adaptativo (ABR)**: Opera em até **990 kbps (qualidade máxima)**. Se houver interferência no sinal sem fio, o sistema ajusta temporariamente a taxa para 660 ou 330 kbps para manter o som contínuo sem estalos, retornando aos 990 kbps assim que o enlace estabilizar.
* **SBC de Alta Fidelidade**: Garante compatibilidade imediata com qualquer fone ou caixa Bluetooth do mercado em 44.1 ou 48 kHz (Bitpool 53, estéreo de alta qualidade).
* **Decodificação de Múltiplos Formatos no S3**: Suporte completo a **FLAC** (16 e 24 bits, até 192 kHz na saída cabeada e 96 kHz no Bluetooth), **WAV** (PCM 16, 24 e 32 bits até 384 kHz), **MP3** (CBR/VBR até 320 kbps), **AAC / M4A** e **OGG Vorbis**.
* **Leitura Rápida do Cartão SD (SDMMC 4-Bit 40 MHz)**: Barramento nativo de 4 vias a 40 MHz, permitindo navegar ágil pelas pastas e carregar arquivos pesados sem esvaziamento de buffer.
* **Equalizador Paramétrico SVF de 10 Bandas (384 kHz)**: Filtros *SVF Trapezoidais Lineares* (*Andrew Simper / Cytomic Peaking Bell*) sintetizados em dupla precisão (`double` 64-bit) e executados em `IRAM_ATTR` em tiles de 64 pares com ILP, ganhos de **-15 dB a +15 dB** por banda e Pre-Amp de **-30 dB a +30 dB**. Conta com *double-buffering* lock-free (`s_eq_pool[2]`) em SRAM interna, fusão direta de ganho/balanço L/R, Fast-Path de volume e operação contínua em todas as taxas (**44.1 kHz a 384 kHz sem bypass**).
* **Interface Monocromática Fluida no OLED SSD1306**: Navegação intuitiva com joystick de 5 direções ou expansor PCF8574, menus em carrossel, visualização da biblioteca por pastas e arquivos, além da tela "Now Playing" com dados da faixa, formato, taxa de amostragem, profundidade de bits, tempo decorrido e modos de loop/shuffle.
* **Persistência de Estado Segura (Safe NVS)**: Grava e recupera automaticamente a última música tocada, o ponto exato onde a reprodução foi pausada, o volume atual, a preferência de codec e o modo de ordenação das faixas, validando o cabeçalho antes de persistir para evitar boot-loops.
* **Ordenação Flexível de Faixas (Nome A-Z vs Data / Tracklist Original)**: Configurado diretamente no menu **"Conf" (Opção 6 — "Ordenar")**. Permite alternar instantaneamente entre a ordem alfabética clássica e a ordem por data de modificação (`mtime` do cartão SD), preservando a sequência original de faixas das gravações/álbuns. A preferência é gravada na NVS e a pasta ativa é reordenada imediatamente.
* **Expansor I2C PCF8574 para Controle de Volume e Entradas**: Suporte ao expansor de 8 portas I2C PCF8574 compartilhando o barramento SDA/SCL, viabilizando leitura de joystick e botões dedicados de volume (`VOL+` / `VOL-`) economizando pinos valiosos de GPIO.
* **Ferramenta de Resgate e Recuperação de SD Card (`tools/recover_mps3_sd.py`)**: Utilitário Python para recuperação de cartões FAT32 corrompidos após desconexão sem ejeção segura, identificando automaticamente arquivos `.CHK`, extraindo tags Vorbis Comments/ID3 e reconstruindo as pastas originais (`/Musicas`, `/Podcasts`).
* **Governador Dinâmico de Clock e Gerenciamento de Energia (DFS)**:
  * **Escalonamento Inteligente de Frequência**: Ajusta a CPU Xtensa LX7 dinamicamente entre **240 MHz**, **160 MHz**, **80 MHz** e **40 MHz** conforme a demanda de carga em tempo real.
  * **240 MHz**: Operações intensivas de I/O e rede (transferência Wi-Fi, upload OTA, busca/seek veloz, decodificação Hi-Res extrema >96 kHz e USB MSC).
  * **160 MHz**: Interface gráfica ativa, renderização do display OLED a até 62.5 FPS dinâmicos e navegação de menus com equalizador paramétrico SVF de 10 bandas.
  * **80 MHz**: Reprodução contínua de áudio padrão (MP3, WAV, FLAC até 48 kHz) com tela apagada/repouso, mantendo o PLL base (`BBPLL`) travado e estável para nunca causar jitter no barramento I2S ou falhas de leitura no SDMMC.
  * **40 MHz**: Repouso ocioso com economia drástica de bateria e redução de aquecimento.
* **Deep Sleep (< 100 µA) e Suspensão Inteligente**:
  * **Modo Suspensão Profunda**: Desativa núcleos de processamento, rádios e barramentos, reduzindo a drenagem do circuito a níveis sub-miliampere.
  * **Configurável no Menu "Conf"**: Opção para ajustar o tempo de inatividade até o Deep Sleep (1 min, 5 min, 15 min, etc.), com atalho de disparo imediato via direcional direito `[>]`.
  * **Wakeup por Hardware no Botão Físico**: Acorda instantaneamente ao pressionar o botão Direcional CIMA (`JOY_UP`, GPIO 2), roteado no domínio de energia `RTC_PERIPH` com resistor pull-up ativo.
  * **Auto-Wakeup ao Conectar Carregador**: O monitor de carga (`PIN_BATTERY_CHRG`, GPIO 11) religa o dispositivo automaticamente assim que o cabo USB de recarga é conectado.
  * **Proteção contra Phantom Powering e Pad Hold**: Liberação e retenção de pads estruturadas para evitar retenção de nível lógico nas linhas de controle e I2C.
* **Relógio de Tempo Real (RTC DS3231) em Barramento I2C Compartilhado**:
  * Chip Maxim/Analog Devices DS3231 de altíssima precisão com oscilador a cristal compensado por temperatura (TCXO) integrado.
  * Barramento I2C mestre compartilhado de forma thread-safe com o display OLED SSD1306 (SDA no GPIO 10 e SCL no GPIO 9).
  * Sincronização com o relógio POSIX (`settimeofday()`) no boot, garantindo que faixas criadas, modificadas ou recebidas via Wi-Fi recebam carimbo de data e hora reais.
  * Sensor interno de temperatura ambiente com leitura exposta ao sistema.
* **QR Code Nativo no Display OLED SSD1306**:
  * Renderização em tempo real de matrizes QR Code monocromáticas diretamente na tela de 128x64 pixels.
  * Permite apontar a câmera de qualquer smartphone para conectar instantaneamente ao Hotspot Wi-Fi (SoftAP) do MPS3 sem digitar credenciais, ou acessar o endereço web `http://mps3.local`.
* **Sincronização Automática de Podcasts (`podcast_sync`)**:
  * Subsistema autônomo em segundo plano para download de episódios novos e parciais via Wi-Fi diretamente para a biblioteca do micro SD.
  * Integração com scripts de automação de bancada e servidor de relay para Android/Termux (`tools/mobile_relay` e `tools/podcast_server`).
* **Atualização de Firmware Over-The-Air (OTA) Dual-Bank com Rollback Anti-Brick**:
  * **Layout Dual-Bank Seguro na Flash de 16MB**: Dois bancos de aplicação de 4MB cada (`ota_0` em `0x20000` e `ota_1` em `0x420000`) com partição de controle `otadata` em `0x10000`, permitindo gravação em segundo plano sem risco de perda de configurações na NVS.
  * **Rollback Automático Anti-Brick**: Habilitado via `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. A aplicação precisa inicializar com sucesso todos os periféricos vitais (SDMMC, I2S, tarefas FreeRTOS) e chamar `esp_ota_mark_app_valid_cancel_rollback()` para ser confirmada; em caso de falha de boot ou crash prematuro, o bootloader reverte autonomamente para a partição anterior saudável.
  * **Auto-Descoberta Inteligente por MAC de Hardware (`28:84:85:52:35:84`)**: Os scripts CLI localizam o MPS3 na rede local combinando leitura de cache ARP do sistema operacional, varredura rápida de rede (ping sweep multithread), mDNS e rotas de contingência, eliminando a necessidade de descobrir o IP atribuído via DHCP.
  * **Interface Web Amigável (`/api/ota`)**: Modal visual no gerenciador web (`http://mps3.local/`) com leitura de partição ativa, versão, nível de bateria (< 15% bloqueia gravação com alerta de segurança), barra de progresso em tempo real e contagem regressiva de reconexão.
  * **Feedback Visual em Tempo Real no OLED**: Renderização com barra de progresso proporcional em modo XOR (`u8g2_SetDrawColor(..., 2)`), percentual dinâmico e aviso de segurança anti-desligamento ("NAO DESLIGUE O MPS3!").
* **Central Web Embarcada Modular (`http://mps3.local`)**: Servida diretamente da Flash SPI do ESP32-S3 em um único payload Gzip nível 9 (`index_html_gz.h`, apenas 142 KB compactado) consumindo 0 bytes de RAM dinâmica e apenas 1 socket TCP no LwIP:
  * **Arquitetura 100% Modular (`components/wifi_transfer/web/src/`)**: Estruturada em templates HTML5, folhas de estilo CSS temáticas e scripts JS isolados, montada deterministicamente via `tools/pack_web.py`.
  * **Explorador e Gerenciador de Arquivos MicroSD**: Upload recursivo de pastas, downloads diretos, criação de pastas, movimentação e cópia com proteção rigorosa contra movimentação circular. Extração automática de metadados ID3/Vorbis e geração de miniaturas `.cover.jpg` de alta velocidade.
  * **Player de Áudio Web Integrado**: Mini Player inferior responsivo e Fullscreen Player no navegador com equalizador de 3 bandas local e analisador de espectro FFT em tempo real via Web Audio API.
  * **Motor Híbrido de Podcasts v3.0 no Navegador**:
    - *Descoberta Apple Podcasts*: Integração direta com a API pública de busca do Apple Podcasts para preenchimento de feeds RSS oficiais.
    - *Extrator de Transmissões YouTube*: Parser isolado para transmissões e lives (`/streams`), filtrando membros de canais e selecionando o stream de áudio com melhor taxa de bits sem necessidade de servidor intermediário.
    - *Planejador Determinístico*: Retenção automática dos 3 episódios mais recentes de cada programa e de todos os não ouvidos, com reserva preventiva de 32 MB livres no cartão SD.
  * **Painel Analítico "MPS3 Wrapped"**:
    - Histórico e estatísticas de escuta (tempo total, faixas mais ouvidas, artistas favoritos e distribuição por resolução Hi-Res).
    - Renderizador gráfico via Canvas 1080p para geração de Stories e cards sociais prontos para compartilhamento.
    - Ponte móvel instantânea para smartphone via escaneamento de QR Code ISO-18004.
* **Controle de Volume Bidirecional (AVRCP)**: Modificar o volume no joystick do mps3 atualiza o volume no fone, e os botões físicos do próprio fone também refletem instantaneamente no mps3 via UART.
* **Modos USB Exclusivos (TinyUSB Device)**: Chaveamento limpo e isolado de perfis USB sem sobreposição de descritores:
  * **Detecção Automática e Menu de Seleção (Estilo Android)**: Ao conectar o cabo na porta USB de dados (GPIO 19/20), o mps3 detecta o evento de conexão e abre instantaneamente um menu interativo no OLED:
    1. **Flash (CDC / Debug)**: Para gravação de firmware e logs seriais.
    2. **DAC (Placa de som USB)**: Placa de som estéreo UAC2 24-bit.
    3. **Armazenamento (MSC)**: Montagem do cartão MicroSD como unidade externa no PC.
    4. **Nada a fazer**: Fecha o prompt e permanece no player de áudio normalmente.
    *Inclui timeout automático de 15 segundos para fechar o diálogo sem interromper a música caso nenhuma tecla seja pressionada.*
  * **Modo Flash / CDC (`0x4000`)**: Console CDC com interceptação DTR/RTS e 1200 bps touch para reboot automático no bootloader ROM da Espressif (flashing sem pressionar botões).
  * **Modo DAC USB / UAC2 (`0x4006`)**: Placa de som USB estéreo de alta fidelidade (UAC2 24-bit em subslots de 32 bits, taxas de 44.1 kHz e 48 kHz):
    * **Interface Visual Refinada no OLED**: Badge estilizado `[USB DAC]`, indicação de resolução (`44.1k / 24b` ou `48k / 24b`), indicador dinâmico e reativo de streaming (`● STREAMING ATIVO` / `○ AGUARDANDO USB...`), preset ativo do equalizador e barra de volume integrada.
    * **Controle de Volume Bidirecional (HID Consumer)**: Ajustar o volume no joystick (`JOY_UP` / `JOY_DOWN`) envia comandos nativos de mídia ao computador (Volume Up/Down) via interface HID com repeat suave, sincronizando o mixer do Windows/Linux/macOS sem dupla atenuação.
    * **Resistência à Suspensão Seletiva (Selective Suspend)**: Suporte a ciclos de economia de energia do driver de áudio do sistema operacional sem desconectar ou fechar a tela do DAC.
    * **Acesso Imediato ao Equalizador**: Joystick para a Direita (`JOY_RIGHT`) abre diretamente o menu de Presets do Equalizador de 10 bandas em tempo real durante a reprodução do computador. Joystick para a Esquerda (`JOY_LEFT`) retorna instantaneamente ao DAC; segurar Esquerda retorna ao player de músicas.
  * **Modo Armazenamento / MSC (`0x4002`)**: Montagem do cartão SD como drive USB no Windows/Linux via SDMMC de 4 vias (buffer de 8 KB com DWC2 double-buffering).

### 💾 Desempenho USB Mass Storage (MSC) e Limitações

A PHY USB interna do ESP32-S3 é restrita ao padrão **USB 2.0 Full-Speed (12 Mbps nominal)**. Por esse motivo, as taxas de transferência são baixas em comparação com leitores dedicados, sendo indicada para cópias rápidas de álbuns pontuais sem necessidade de remover o cartão:

* **Leitura**: ~720 KB/s (~0.7 MB/s)
* **Escrita**: ~490 KB/s (~0.5 MB/s)

> **Nota técnica**: O barramento Full-Speed opera em pacotes Bulk de até 64 bytes. Com os tempos de inter-packet, handshakes de barramento e encapsulamento SCSI, a taxa física útil fica limitada a cerca de 750–850 KB/s no barramento. A escrita sofre atraso adicional dos ciclos de programação da memória Flash do cartão MicroSD. Para grandes cargas iniciais de músicas, o uso de um leitor externo USB 3.0 no computador continua sendo muito mais prático.

### ❌ O Que Não Funciona ou Está Desativado Nesta Versão

* **Codecs Qualcomm aptX e aptX HD no Bluetooth Companion**: **Não funcionam**. Embora as rotinas dos encoders e as descrições dos endpoints estejam presentes na árvore do projeto, o fluxo de empacotamento RTP e a negociação AVDTP falham na sincronização com fones comerciais. **A transmissão Bluetooth ocorre com estabilidade comprovada via Sony LDAC e SBC**.
* **Decodificador Opus**: **Experimental**. Faixas Opus com taxas de compressão complexas demandam um tamanho de pilha FreeRTOS muito alto, podendo gerar instabilidades se a tarefa de áudio estiver com limites restritos de memória.

---

## 📐 Arquitetura do Sistema

```text
+---------------------------------------------------------------------------------------------------------+
|                                    mps3 (Placa Principal - ESP32-S3)                                    |
|                                                                                                         |
|   [CORE 1: I/O & DECODIFICAÇÃO (Prio 5)]                                                                |
|  +--------------------+        +-----------------------+                                                |
|  | MicroSD (SDMMC 4b) | -----> |  Decodificador Hi-Res |                                                |
|  | (40 MHz FATFS)     |        | (WAV 384k/32b,        |                                                |
|  |                    |        |  FLAC 24b/192k, MP3)  |                                                |
|  +--------------------+        +-----------+-----------+                                                |
|                                            |                                                            |
|                     [Fila DSP 96 Blocos (768 KB PSRAM) + RingBuf 1 MB PSRAM]                            |
|                                            |                                                            |
|                                            v                                                            |
|   [CORE 0: DSP, SAÍDA & INTERFACE]         |                                                            |
|  +-----------------------------------------+-----------+        +--------------------+                  |
|  | audio_dsp_task (Prio 5): Vol / Balanço / Fade       | -----> |  Driver I2S Master |                  |
|  | + Equalizador SVF 10-Bandas (até 384 kHz)           |        |  DMA 32-bit Stereo |                  |
|  +-----------------------------------------------------+        +---------+----------+                  |
|                                                                           |                             |
|  +--------------------+        +-----------------------+                  | I2S 4-Fios:                 |
|  | Display OLED I2C   | <..... | Menu / GUI / Joystick |                  | DOUT (47), BCLK (48),       |
|  |(SSD1306 a 62.5 FPS)|        | (touch_task Prio 4    |                  | LRCK (21), MCLK (8)         |
|  +--------------------+        |  + Expansor PCF8574)  |                  |                             |
|                                +-----------+-----------+                  |                             |
|                                            :                              |                             |
|                                            : UART Link                    |                             |
+--------------------------------------------:------------------------------:-----------------------------+
                                             : (115200 8N1)                 |                             |
                                             v                              |                             |
+--------------------------------------------------------------------+      |                             |
|                     ESP32 Clássico (Co-Processador bt_companion)   |      |                             |
|                                                                    |      |                             |
|  +--------------------+   +--------------------+   +-------------+ |      |                             |
|  | Controle UART Link |   |  Reamostrador 32.32|   |  I2S Slave  | <------+ (Até 96 kHz)                |
|  | (Core 0 - Estado)  |   | (Pass-thru em 96k) |   | (PIN 26/25) | |      |                             |
|  +--------------------+   +--------------------+   +-------------+ |      |                             |
|             |                         |                            |      |                             |
|             v                         v                            |      |                             |
|  +--------------------+   +--------------------+                   |      |                             |
|  |  AVRCP Controller  |   | Sony LDAC Encoder  |                   |      |                             |
|  |  (Absolute Volume) |   | (24-bit / 96 kHz)  |                   |      |                             |
|  +--------------------+   +--------------------+                   |      |                             |
|             |                         |                            |      |                             |
|             +------------+------------+                            |      |                             |
|                          v                                         |      |                             |
|             +--------------------------+                           |      |                             |
|             | Pilha Bluedroid BT A2DP  |                           |      |                             |
|             | Multi-SEP (Vendor LDAC)  |                           |      |                             |
|             +--------------------------+                           |      |                             |
|                          |                                         |      |                             |
+--------------------------:-----------------------------------------+      |                             |
                           : Bluetooth A2DP / AVRCP                         v                             |
                           v                                       +-------------------+                  |
               +-----------------------+                           |   DAC PCM5102A    |                  |
               | Fone de Ouvido / Caixa|                           | (Modo 4-Fios MCLK)| <----------------+
               | Bluetooth Sem Fio     |                           | (Até 384kHz/32b)  |   (Até 384 kHz)
               +-----------------------+                           +-------------------+
```

---

## 🗂️ Estrutura do Repositório

O projeto adota o **ESP-IDF v6.0.1 puro** como *Single Source of Truth* para compilação e gravação:

| Diretório / Arquivo | Finalidade |
|---|---|
| **[`espidf/`](espidf/)** | **Ponto de entrada oficial ESP-IDF (ESP32-S3)**: `CMakeLists.txt`, `main/`, `sdkconfig.defaults` (configuração de 8KB USB MSC, PSRAM Octal 8MB, Flash 16MB QIO, Rollback OTA). |
| **[`components/`](components/)** | Componentes modulares independentes: `audio_player`, `board_io` (RTC DS3231, PCF8574 & Power Governor), `usb_manager`, `sd_card`, `oled_display` (com QR Code), `touch_input`, `eq`, `i2s_output`, `wifi_transfer` (servidor web modular + OTA backend), `podcast_sync`, etc. |
| **[`versionamento/`](versionamento/)** | **Scripts de Versionamento e Upload**: `mps3_version.ps1` (PowerShell), `mps3_version.sh` (Bash) e `ota_upload.py` (upload OTA com auto-descoberta inteligente por MAC de hardware). |
| **[`bt_companion/`](bt_companion/)** | **Firmware do Co-Processador Bluetooth (ESP32)**: Transmissor de áudio Sony LDAC 24-bit / 96 kHz e SBC de alta qualidade com controle de volume AVRCP. |
| **[`bt_audio_sink/`](bt_audio_sink/)** | **Firmware Receptor de Teste (ESP32)**: Receptor Bluetooth A2DP Sink para validação e auditoria em bancada do áudio transmitido pelo mps3. |
| **[`tests/`](tests/)** | Scripts de bancada e automação: testes unitários da Central Web modular, scraper do navegador, MPS3 Wrapped, suítes de playlists/shuffle/repeat e benchmark de velocidade. |
| **[`docs/`](docs/)** | Diagramas de ligação elétrica ([WIRING.md](docs/WIRING.md)), arquitetura do sistema ([ARCHITECTURE.md](docs/ARCHITECTURE.md)), procedimento de recuperação FAT32 ([PROCEDIMENTO_RECUPERACAO_SDCARD.md](docs/PROCEDIMENTO_RECUPERACAO_SDCARD.md)), controle de documentação de funções e protocolo UART ([PROTOCOL.md](docs/PROTOCOL.md)). |
| **[`tools/`](tools/)** | Utilitários de empacotamento web (`pack_web.py`), recuperação de SD (`recover_mps3_sd.py`), simulador de desenvolvimento web (`web_dev_simulator.py`), extrator YouTube CLI (`yt_scraper_core.py`), relay móvel para podcasts (`mobile_relay/`) e servidor local (`podcast_server/`). |
| **[`partitions.csv`](partitions.csv)** | Tabela de partições dual-bank (`otadata` @ 0x10000, `ota_0` de 4MB @ 0x20000, `ota_1` de 4MB @ 0x420000). |

---

## 🔌 Tabela Geral de Pinagem e Conexões

### 1. Barramento de Áudio Digital I2S (Compartilhado entre os chips)
*O ESP32-S3 gera os sinais de sincronismo (Master). O DAC PCM5102A e o ESP32 Companion recebem os dados em paralelo.*

| Sinal I2S | ESP32-S3 (Master TX) | ESP32 Companion (Slave RX) | DAC PCM5102A | Observações |
|---|:---:|:---:|:---:|---|
| **BCLK** (Bit Clock) | **GPIO 48** | **GPIO 26** | **BCK** | Clock de bits síncrono |
| **LRCK / WS** (Word Select) | **GPIO 21** | **GPIO 25** | **LCK** | Frequência de amostragem da faixa (até 384 kHz) |
| **DOUT / DIN** (Dados PCM) | **GPIO 47** | **GPIO 22** | **DIN** | Dados de áudio PCM estéreo |
| **MCLK** (Master Clock) | **GPIO 8** | — | **SCK** | Master Clock dedicado (até 49,152 MHz em 384k / 24,576 MHz em 192k, jumper SCK-GND removido) |
| **GND** | GND | GND | GND | Terra comum de referência |

### 2. Barramento de Controle UART (Comunicação S3 ↔ Companion)

| Sinal | ESP32-S3 | ESP32 Companion | Parâmetros |
|---|:---:|:---:|---|
| **TX → RX** | **GPIO 14** (TXD1) | **GPIO 16** (RXD1) | 115200 bps, 8N1, binário com checksum |
| **RX ← TX** | **GPIO 13** (RXD1) | **GPIO 17** (TXD1) | 115200 bps, 8N1, binário com checksum |
| **RESET / EN** | **GPIO 12** | **EN / CHIP_PU** | Reset por hardware do co-processador |

### 3. Periféricos da Placa Principal (ESP32-S3)

| Periférico | Função | Pinos no ESP32-S3 |
|---|---|---|
| **Display OLED SSD1306** | I2C Mestre (128x64 monocromático a 800 kHz) | **SDA: GPIO 10** \| **SCL: GPIO 9** (alimentação 3.3V) |
| **RTC DS3231** | Relógio de alta precisão I2C (compartilhado) | **SDA: GPIO 10** \| **SCL: GPIO 9** (mesmo barramento I2C, thread-safe) |
| **Expansor I2C PCF8574** | Entradas dedicadas de volume e joystick | **SDA: GPIO 10** \| **SCL: GPIO 9** (barramento I2C compartilhado) |
| **Cartão micro SD** | Barramento SDMMC 4-Bit (Slot nativo) | **CLK: GPIO 5** \| **CMD: GPIO 6** \| **D0: GPIO 4** \| **D1: GPIO 17** \| **D2: GPIO 16** \| **D3: GPIO 7** |
| **Joystick de 5 Vias** | Botões de navegação & Wakeup | **UP: GPIO 2 (RTC IO / Wakeup)** \| **LEFT: GPIO 39** \| **DOWN: GPIO 41** \| **RIGHT: GPIO 42** \| **CENTER: GPIO 40** |
| **Monitor de Bateria & TP4056** | Divisor Li-Ion + status TP4056 | **ADC: GPIO 1** (Tensão Li-Ion) \| **CHRG: GPIO 11 (Wakeup Carga)** |
| **LED RGB WS2812** | Feedback visual on-board | **DIN: GPIO 38** |
| **I2S MCLK (Master Clock)** | Clock dedicado para o DAC PCM5102A | **MCLK: GPIO 8** (Ligue ao pino SCK do DAC) |
| **Porta USB Dados & Debug** | Conexão OTG Nativa ESP32-S3 (CDC / DAC / MSC) | **D-: GPIO 19** \| **D+: GPIO 20** |
| **Porta USB Carga Bateria** | Alimentação independente do carregador TP4056 | Sem conexão de dados com a MCU (Apenas VBUS/GND ao TP4056) |

> ℹ️ **Arquitetura de Portas USB**: O mps3 conta com **duas portas USB físicas distintas**:
> 1. **Porta de Carga**: Conectada exclusivamente ao módulo carregador de bateria de lítio TP4056. Utilizada apenas para carregar o dispositivo com segurança.
> 2. **Porta de Dados e Debug**: Conectada diretamente ao hardware USB OTG nativo do ESP32-S3 (GPIOs 19 e 20). É esta porta que executa a gravação de firmware, o console serial de debug, o DAC USB (UAC2) e o Mass Storage (MSC), disparando a seleção automática no display ao ser plugada.

---

## 📚 Fontes, Créditos e Referências de Código

O desenvolvimento do mps3 utilizou componentes consolidados do ecossistema de áudio aberto:

1. **Sony Corporation — Codificador LDAC**:
   * O código fonte do codificador em ponto fixo (`components/ldac_enc/vendor/`) pertence à **Sony Corporation** e foi disponibilizado sob licença **Apache License, Version 2.0** no repositório do [Android Open Source Project (AOSP) - platform/external/libldac](https://android.googlesource.com/platform/external/libldac/).
2. **Espressif Systems — Framework ESP-IDF e Codecs de Áudio**:
   * O motor de decodificação no ESP32-S3 utiliza o componente [`espressif__esp_audio_codec`](https://components.espressif.com/components/espressif/esp_audio_codec), incorporando decodificadores otimizados de **libFLAC**, **Helix MP3**, **PVMP3** e **PVMP4**.
   * A pilha Bluetooth deriva do framework **Bluedroid** da Espressif, customizada com patches para suportar múltiplos SEPs e codecs proprietários (`ESP_A2D_MCT_NON_A2DP`).
3. **BlueZ Linux Bluetooth Subsystem — Codificador SBC**:
   * A codificação SBC no `bt_companion` foi adaptada da implementação do Linux BlueZ, projetada para execução eficiente em microcontroladores.
4. **Andrew Simper (Cytomic) & Robert Bristow-Johnson — Teoria de Filtros SVF / Audio EQ**:
   * A síntese de filtros do equalizador paramétrico SVF trapezoidal linear de 10 bandas baseia-se nas formulações matemáticas de domínio público de **Andrew Simper (Cytomic)** para Peaking Bell Integrators e nas referências clássicas de equalização paramétrica de **Robert Bristow-Johnson**.

---

## 🚀 Como Compilar e Gravar o Firmware

### Pré-requisitos
* Computador com Windows 10/11 ou Linux.
* **ESP-IDF v6.0.1 ou v6.2.0** instalado e configurado no terminal.
* Cabo USB conectado ao ESP32-S3.

### Compilando e Gravando a Placa Principal (ESP32-S3)
```bash
# 1. Acessar o subdiretório ESP-IDF oficial do projeto
cd espidf

# 2. Inicializar o ambiente do ESP-IDF (caso não esteja no PATH)
# Exemplo Windows: C:\esp\v6.0.1\esp-idf\export.bat (ou export.ps1)

# 3. Configurar target para ESP32-S3 (apenas na primeira compilação)
idf.py set-target esp32s3

# 4. Compilar
idf.py build

# 5. Gravar no ESP32-S3 via cabo serial (ajustar porta COM conforme seu sistema)
idf.py -p COM7 flash

# 6. Monitorar logs de execução
idf.py -p COM7 monitor
```

### Atualização e Upload Remoto via Wi-Fi (OTA)

O MPS3 pode ser atualizado via rede sem fio, sem precisar conectar o cabo serial ao computador:

#### 1. Via Scripts de Versionamento (CLI com Auto-Descoberta por MAC)
Basta colocar o MPS3 no modo Wi-Fi (conectado na mesma rede que o computador). O script localiza automaticamente a placa pelo MAC `28:84:85:52:35:84` (ou aceita IP opcional):

```powershell
# Windows (PowerShell)
.\versionamento\mps3_version.ps1 ota                # Envia o binario mais recente via OTA
.\versionamento\mps3_version.ps1 build-ota          # Compila nova versao e envia via OTA
.\versionamento\mps3_version.ps1 ota 192.168.1.100  # Envia apontando para IP fixo
```

```bash
# Linux / macOS / Git Bash
./versionamento/mps3_version.sh ota                 # Envia o binario mais recente via OTA
./versionamento/mps3_version.sh build-ota           # Compila nova versao e envia via OTA
./versionamento/mps3_version.sh ota 192.168.1.100   # Envia apontando para IP fixo
```

#### 2. Via Interface Web
1. Entre no modo Wi-Fi no menu do MPS3 e conecte-se na rede.
2. No computador ou celular, abra o navegador em `http://mps3.local/` (ou no IP exibido na tela).
3. Clique no botão **"🚀 Atualizar"** no cabeçalho.
4. Selecione o arquivo `mps3.bin` e acompanhe a barra de progresso.
5. O MPS3 grava a nova imagem no banco livre (`ota_0` ou `ota_1`), reinicia e comuta a partição automaticamente.

### Executando o Benchmark de Velocidade USB MSC
Com o player em modo USB Storage conectado ao computador (montado como letra `D:` ou equivalente):
```bash
python tests/benchmark_msc.py D
```

---

## 📄 Licença

O projeto **mps3** é distribuído sob a licença **MIT**, respeitando as licenças dos componentes de terceiros integrados:
* O codificador Sony LDAC é licenciado sob **Apache License 2.0** pela Sony Corporation.
* As bibliotecas de áudio e a pilha de rede pertencem à Espressif Systems sob licenças Apache 2.0 / Modified MIT.
Consulte o arquivo [`LICENSE`](LICENSE) para o texto legal completo.
