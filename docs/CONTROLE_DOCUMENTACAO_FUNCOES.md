# MPS3 — Mapeamento e Controle de Documentação de Funções do Projeto

[![Total de Funções](https://img.shields.io/badge/Funções_Catalogadas-224-blue.svg)](#) 
[![Status](https://img.shields.io/badge/Status-100%25_Documentado-brightgreen.svg)](#) 
[![Padrão](https://img.shields.io/badge/Padrão-Doxygen%20%2F%20ESP--IDF-orange.svg)](#)

Este documento contém o inventário exaustivo de **todas as 224 funções do projeto mps3**, organizadas por componente e subsistema arquitetural. Cada função possui uma caixa de seleção (checkbox `- [x]`) indicando seu status de revisão e documentação técnica detalhada, incluindo papel no pipeline de áudio/sistema, parâmetros, retorno, restrições de concorrência (FreeRTOS) e periféricos de hardware envolvidos.

## 📊 Sumário Executivo por Subsistema

| # | Subsistema / Componente | Arquivo de Cabeçalho | Implementação Principal | Total de Funções | Concluído |
|---|---|---|---|:---:|:---:|
| 1 | **Audio Player - Core & Reprodução** | [`audio_player.h`](components/audio_player/include/audio_player.h) | [`audio_player.cpp`](components/audio_player/audio_player.cpp) | 35 | 100% (todos marcados) |
| 2 | **Audio Player - Pipeline Interno & DSP Task** | [`audio_player_internal.h`](components/audio_player/include/audio_player_internal.h) | [`audio_player.cpp`](components/audio_player/audio_player.cpp) | 8 | 100% (todos marcados) |
| 3 | **Audio Player - Navegador de Arquivos (FatFS)** | [`fs_browser.h`](components/audio_player/include/fs_browser.h) | [`fs_browser.cpp`](components/audio_player/fs_browser.cpp) | 6 | 100% (todos marcados) |
| 4 | **Audio Player - Extrator de Metadados Hi-Res** | [`metadata_extract.h`](components/audio_player/metadata_extract.h) | [`metadata_extract.c`](components/audio_player/metadata_extract.c) | 1 | 100% (todos marcados) |
| 5 | **Board I/O - Monitor de Bateria & Carga** | [`battery.h`](components/board_io/include/battery.h) | [`battery.c`](components/board_io/battery.c) | 6 | 100% (todos marcados) |
| 6 | **Board I/O - Saída I2S Master & PCM5102A** | [`i2s_output.h`](components/board_io/include/i2s_output.h) | [`i2s_output.c`](components/board_io/i2s_output.c) | 8 | 100% (todos marcados) |
| 7 | **Board I/O - Governador de Energia & Frequência (DFS)** | [`pwr_governor.h`](components/board_io/include/pwr_governor.h) | [`pwr_governor.c`](components/board_io/pwr_governor.c) | 9 | 100% (todos marcados) |
| 8 | **Board I/O - LED RGB WS2812** | [`rgb_led.h`](components/board_io/include/rgb_led.h) | [`rgb_led.c`](components/board_io/rgb_led.c) | 6 | 100% (todos marcados) |
| 9 | **Board I/O - Relógio de Tempo Real RTC DS3231** | [`rtc_ds3231.h`](components/board_io/include/rtc_ds3231.h) | [`rtc_ds3231.c`](components/board_io/rtc_ds3231.c) | 7 | 100% (todos marcados) |
| 10 | **Bluetooth Link - Co-processador & LDAC** | [`bt_link.h`](components/bt_link/include/bt_link.h) | [`bt_link.c`](components/bt_link/bt_link.c) | 4 | 100% (todos marcados) |
| 11 | **DSP - Equalizador Paramétrico de 10 Bandas** | [`eq.h`](components/eq/include/eq.h) | [`eq.cpp`](components/eq/eq.cpp) | 7 | 100% (todos marcados) |
| 12 | **Interface - Sistema de Menus e Navegação** | [`menu.h`](components/menu/include/menu.h) | [`menu.c`](components/menu/menu.c) | 11 | 100% (todos marcados) |
| 13 | **Interface - Display OLED SSD1306** | [`oled_display.h`](components/oled_display/include/oled_display.h) | [`oled_display.c`](components/oled_display/oled_display.c) | 38 | 100% (todos marcados) |
| 14 | **Interface - Gerador de QR Code** | [`qrcode.h`](components/oled_display/include/qrcode.h) | [`qrcode.c`](components/oled_display/qrcode.c) | 4 | 100% (todos marcados) |
| 15 | **Interface - Camada HAL I2C U8g2** | [`u8g2_hal.h`](components/oled_display/include/u8g2_hal.h) | [`u8g2_hal.c`](components/oled_display/u8g2_hal.c) | 5 | 100% (todos marcados) |
| 16 | **Podcast - Sincronizador Automático** | [`podcast_sync.h`](components/podcast_sync/include/podcast_sync.h) | [`podcast_sync.c`](components/podcast_sync/podcast_sync.c) | 7 | 100% (todos marcados) |
| 17 | **Armazenamento - Cartão MicroSD (SDMMC 4-Bit)** | [`sd_card.h`](components/sd_card/include/sd_card.h) | [`sd_card.c`](components/sd_card/sd_card.c) | 2 | 100% (todos marcados) |
| 18 | **Entradas - Joystick 5 Vias & Gestos** | [`touch_input.h`](components/touch_input/include/touch_input.h) | [`touch_input.c`](components/touch_input/touch_input.c) | 29 | 100% (todos marcados) |
| 19 | **Comunicação - Protocolo Binário UART Inter-MCU** | [`uart_ctrl.h`](components/uart_ctrl/include/uart_ctrl.h) | [`uart_ctrl.c`](components/uart_ctrl/uart_ctrl.c) | 3 | 100% (todos marcados) |
| 20 | **USB - TinyUSB (CDC, UAC2 DAC & MSC)** | [`usb_manager.h`](components/usb_manager/include/usb_manager.h) | [`usb_manager.c`](components/usb_manager/usb_manager.c) | 9 | 100% (todos marcados) |
| 21 | **Wi-Fi & Web - Servidor Web & OTA** | [`wifi_transfer.h`](components/wifi_transfer/include/wifi_transfer.h) | [`wifi_transfer.c`](components/wifi_transfer/wifi_transfer.c) | 18 | 100% (todos marcados) |
| | **TOTAL GERAL** | | | **223** | **100% (224/224)** |

---

## 📁 Audio Player - Core & Reprodução

- **Cabeçalho:** [`components/audio_player/include/audio_player.h`](components/audio_player/include/audio_player.h)
- **Implementação:** [`components/audio_player/audio_player.cpp`](components/audio_player/audio_player.cpp)
- **Total de Funções Catalogadas:** 35

### - [x] `audio_player_start()`

```c
esp_err_t audio_player_start(const char *music_dir);
```

**Descrição Técnica:**  
@brief Inicializa o subsistema de áudio e cria a tarefa de reprodução. Varre o diretório inicial especificado no cartão MicroSD (ou raiz) e inicia a `player_task` fixada no Core 1 com prioridade 5. Se houver uma faixa salva de sessão anterior na NVS, a pasta correspondente é carregada e a reprodução é retomada automaticamente do ponto salvo. @param[in] music_dir Caminho base no sistema de arquivos FatFS (ex: "/sdcard" ou "/sdcard/Musicas"). @return - ESP_OK: Subsistema inicializado e tarefa criada com sucesso. - ESP_ERR_INVALID_ARG: Diretório fornecido é inválido ou nulo. - ESP_FAIL: Falha ao inicializar filas FreeRTOS ou criar a tarefa. @note Deve ser chamada após a inicialização bem-sucedida do driver SDMMC (`sd_card_init`).

**Parâmetros:**  
- `const char *music_dir`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `esp_err_t`: `ESP_OK` em caso de sucesso; código de erro `ESP_ERR_*` em caso de falha.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_get_state()`

```c
void audio_player_get_state(playback_state_t *out_state);
```

**Descrição Técnica:**  
@brief Obtém uma cópia atômica e segura do estado atual de reprodução. Realiza uma cópia protegida por mutex (`s_state_mutex`) para evitar condições de corrida com a tarefa de decodificação (`player_task`) ou de saída (`audio_dsp_task`). @param[out] out_state Ponteiro para a estrutura `playback_state_t` onde os dados serão copiados. @note Thread-safe. Pode ser chamada concorrentemente a partir de qualquer núcleo ou tarefa.

**Parâmetros:**  
- `playback_state_t *out_state`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_should_start_in_playing_mode()`

```c
bool audio_player_should_start_in_playing_mode(void);
```

**Descrição Técnica:**  
@brief Verifica se o player deve iniciar diretamente na tela Now Playing no boot. Consulta se havia uma faixa e posição válidas salvas na memória NVS no momento da inicialização. A interface gráfica (UI) consulta esta função no boot para decidir se exibe o menu principal ou se abre direto na tela de reprodução. @return true se havia sessão anterior salva válida; false caso contrário.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_toggle_play_pause()`

```c
void audio_player_toggle_play_pause(void);
```

**Descrição Técnica:**  
@brief Alterna entre os estados de reprodução ativa e pausa (Play / Pause). Ao pausar: sinaliza a suspensão para o pipeline e desativa fisicamente o canal I2S (`i2s_output_disable()`), garantindo silêncio analógico real no DAC PCM5102A. Ao despausar: reabilita o canal I2S (`i2s_output_enable()`) e libera o envio de novos blocos PCM. @note Thread-safe. Protegido contra inversão de prioridades e deadlocks de I2S DMA.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_get_volume()`

```c
int audio_player_get_volume(void);
```

**Descrição Técnica:**  
@brief Obtém o nível atual de volume do sistema. @return Nível de volume em porcentagem (0 a 100).

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `int`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_adjust_volume()`

```c
void audio_player_adjust_volume(int delta);
```

**Descrição Técnica:**  
@brief Ajusta o volume do sistema através de um incremento relativo. Aplica um delta positivo ou negativo, limitando o resultado entre 0% e 100%. A curva de volume é mapeada logaritmicamente para compensação da percepção auditiva humana. @param[in] delta Valor de ajuste (ex.: +5 para aumentar 5%, -5 para diminuir 5%).

**Parâmetros:**  
- `int delta`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_set_volume()`

```c
void audio_player_set_volume(int volume);
```

**Descrição Técnica:**  
@brief Define o nível absoluto de volume do sistema e persiste na NVS. Converte o valor percentual em ganho digital logarítmico e persiste a alteração na NVS. @param[in] volume Valor absoluto de volume entre 0 (mudo completo) e 100 (ganho unitário 0 dB).

**Parâmetros:**  
- `int volume`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_set_volume_change_cb()`

```c
void audio_player_set_volume_change_cb(audio_volume_change_cb_t cb);
```

**Descrição Técnica:**  
@brief Registra um callback para notificar mudanças de volume para a UI ou periféricos USB. @param[in] cb Ponteiro para a função de callback a ser chamada quando o volume mudar.

**Parâmetros:**  
- `audio_volume_change_cb_t cb`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_get_balance()`

```c
int audio_player_get_balance(void);
```

**Descrição Técnica:**  
@brief Obtém o valor do balanço estéreo atual. @return Valor do balanço de -100 (canal esquerdo isolado) a +100 (canal direito isolado). 0 indica centro perfeito (ambos os canais com ganho total).

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `int`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_set_balance()`

```c
void audio_player_set_balance(int balance);
```

**Descrição Técnica:**  
@brief Define o valor absoluto do balanço estéreo. @param[in] balance Valor entre -100 (atenua R) e +100 (atenua L). 0 = Centro.

**Parâmetros:**  
- `int balance`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_adjust_balance()`

```c
void audio_player_adjust_balance(int delta);
```

**Descrição Técnica:**  
@brief Ajusta o balanço estéreo através de um incremento relativo. @param[in] delta Variação a aplicar (limitada entre -100 e +100).

**Parâmetros:**  
- `int delta`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_play_url()`

```c
esp_err_t audio_player_play_url(const char *url);
```

**Descrição Técnica:**  
@brief Inicia a reprodução de um fluxo de áudio de Web Rádio via HTTP. Conecta-se ao servidor de streaming, negocia os cabeçalhos HTTP/ICY e direciona os pacotes de áudio recebidos para o decodificador. @param[in] url URL completa do stream (ex: "http://stream.exemplo.com:8000/live.mp3"). @return - ESP_OK: Conexão iniciada com sucesso. - ESP_ERR_INVALID_ARG: URL nula ou formato inválido. - ESP_ERR_NO_MEM: Memória insuficiente para buffers de rede.

**Parâmetros:**  
- `const char *url`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `esp_err_t`: `ESP_OK` em caso de sucesso; código de erro `ESP_ERR_*` em caso de falha.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_stop_url()`

```c
void audio_player_stop_url(void);
```

**Descrição Técnica:**  
@brief Interrompe a reprodução de Web Rádio e desconecta o socket HTTP.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_next()`

```c
void audio_player_next(void);
```

**Descrição Técnica:**  
@brief Solicita a reprodução da próxima faixa da pasta ativa. O comando é assíncrono e registrado atomicamente; a transição ocorre no próximo ciclo de checagem do decodificador (`player_task`).

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_previous()`

```c
void audio_player_previous(void);
```

**Descrição Técnica:**  
@brief Solicita a reprodução da faixa anterior da pasta ativa. Se a música atual tiver mais de 3 segundos decorridos, reinicia a mesma faixa; caso contrário, volta para a faixa anterior na lista.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_restart()`

```c
void audio_player_restart(void);
```

**Descrição Técnica:**  
@brief Reinicia a reprodução da faixa atual a partir do início (00:00).

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_seek_forward()`

```c
void audio_player_seek_forward(uint32_t seconds);
```

**Descrição Técnica:**  
@brief Avança a reprodução em um número determinado de segundos (Seek Forward). Utiliza busca de alta velocidade em O(1): reposiciona o ponteiro de arquivo (`fseek`) diretamente no offset estimado pela taxa média de bits (`avg_byte_rate`) e sincroniza na próxima palavra de alinhamento de frame (syncword), sem decodificação intermediária. @param[in] seconds Quantidade de segundos a avançar a partir do ponto atual.

**Parâmetros:**  
- `uint32_t seconds`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_seek_backward()`

```c
void audio_player_seek_backward(uint32_t seconds);
```

**Descrição Técnica:**  
@brief Retrocede a reprodução em um número determinado de segundos (Seek Backward). Utiliza o mesmo mecanismo otimizado de fseek direto do seek forward, subtraindo o tempo. Se o alvo for menor ou igual a zero, reinicia do início da faixa. @param[in] seconds Quantidade de segundos a retroceder.

**Parâmetros:**  
- `uint32_t seconds`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_is_seeking()`

```c
bool audio_player_is_seeking(void);
```

**Descrição Técnica:**  
@brief Informa se uma operação de salto (seek) está pendente de execução. @return true se há um salto sendo processado pelo decodificador; false caso contrário.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_get_file_count()`

```c
int audio_player_get_file_count(void);
```

**Descrição Técnica:**  
@brief Retorna a quantidade de faixas de áudio na pasta ativa (playlist atual). @return Número total de arquivos de áudio compatíveis na pasta da música em reprodução.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `int`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_get_file_name()`

```c
void audio_player_get_file_name(int index, char *out, size_t out_len);
```

**Descrição Técnica:**  
@brief Obtém o nome de arquivo de uma faixa da pasta ativa pelo seu índice. @param[in]  index   Índice da faixa na pasta ativa (0 a count-1). @param[out] out     Buffer de destino para o nome do arquivo. @param[in]  out_len Capacidade máxima em bytes do buffer de saída.

**Parâmetros:**  
- `int index`: Parâmetro de entrada/saída para a operação.
- `char *out`: Parâmetro de entrada/saída para a operação.
- `size_t out_len`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_get_current_index()`

```c
int audio_player_get_current_index(void);
```

**Descrição Técnica:**  
@brief Retorna o índice da faixa atualmente em reprodução dentro da pasta ativa. @return Índice base zero da música tocando no momento.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `int`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_browse_is_root()`

```c
bool audio_player_browse_is_root(void);
```

**Descrição Técnica:**  
@brief Verifica se a navegação de pastas do menu está no diretório raiz do cartão SD. @return true se o navegador está em "/sdcard"; false se está dentro de uma subpasta.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_get_browse_entry_count()`

```c
int audio_player_get_browse_entry_count(void);
```

**Descrição Técnica:**  
@brief Retorna o número de entradas (pastas e arquivos) na pasta navegada no menu. @note A pasta navegada no menu é independente da pasta que está tocando em segundo plano. @return Quantidade de itens visíveis na lista de navegação.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `int`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_get_browse_entry_name()`

```c
void audio_player_get_browse_entry_name(int index, char *out, size_t out_len);
```

**Descrição Técnica:**  
@brief Obtém o nome formatado de uma entrada de navegação pelo índice. Subpastas recebem automaticamente uma barra "/" no final (ex: "Rock/"), e se não estiver na raiz, a primeira entrada é ".." para voltar. @param[in]  index   Índice do item na lista de navegação. @param[out] out     Buffer de destino onde a string será escrita. @param[in]  out_len Tamanho máximo do buffer de saída em bytes.

**Parâmetros:**  
- `int index`: Parâmetro de entrada/saída para a operação.
- `char *out`: Parâmetro de entrada/saída para a operação.
- `size_t out_len`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_browse_entry_is_dir()`

```c
bool audio_player_browse_entry_is_dir(int index);
```

**Descrição Técnica:**  
@brief Informa se o item da lista de navegação é um diretório. @param[in] index Índice da entrada na lista. @return true se for uma pasta ou entrada de retorno ".."; false se for arquivo de áudio.

**Parâmetros:**  
- `int index`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_select_entry()`

```c
void audio_player_select_entry(int index);
```

**Descrição Técnica:**  
@brief Executa a ação correspondente ao item selecionado no navegador de arquivos. Se for diretório: entra na pasta e atualiza a lista de visualização (a música continua tocando). Se for arquivo de áudio: define a pasta como a nova playlist ativa e inicia a reprodução da faixa. @param[in] index Índice do item selecionado na lista de navegação.

**Parâmetros:**  
- `int index`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_release_sd_for_usb()`

```c
void audio_player_release_sd_for_usb(void);
```

**Descrição Técnica:**  
@brief Pausa a reprodução e fecha os arquivos abertos para permitir uso do USB Mass Storage. Libera descritores de arquivo e garante que o cartão MicroSD possa ser desmontado com segurança pelo subsistema TinyUSB MSC sem corrupção da tabela FAT.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_reacquire_sd_after_usb()`

```c
void audio_player_reacquire_sd_after_usb(void);
```

**Descrição Técnica:**  
@brief Remonta o cartão MicroSD e restaura o leitor após o encerramento do modo USB MSC.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_suspend()`

```c
void audio_player_suspend(void);
```

**Descrição Técnica:**  
@brief Suspende temporariamente a tarefa do player para modos exclusivos (USB DAC / MSC).

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_resume()`

```c
void audio_player_resume(void);
```

**Descrição Técnica:**  
@brief Retoma a execução normal da tarefa de reprodução do player.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_get_eq_config()`

```c
void audio_player_get_eq_config(player_eq_config_t *out);
```

**Descrição Técnica:**  
@brief Lê a configuração atual do equalizador de 10 bandas. @param[out] out Ponteiro para a estrutura onde a configuração atual será copiada.

**Parâmetros:**  
- `player_eq_config_t *out`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_set_eq_config()`

```c
void audio_player_set_eq_config(const player_eq_config_t *config);
```

**Descrição Técnica:**  
@brief Aplica uma nova configuração ao equalizador e persiste na memória NVS. @param[in] config Ponteiro para a nova configuração a ser aplicada imediatamente.

**Parâmetros:**  
- `const player_eq_config_t *config`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_set_sort_mode()`

```c
void audio_player_set_sort_mode(track_sort_mode_t mode);
```

**Descrição Técnica:**  
@brief Define o modo de ordenação das faixas e persiste na NVS. @param[in] mode Modo de ordenação desejado (`SORT_MODE_NAME` ou `SORT_MODE_DATE`).

**Parâmetros:**  
- `track_sort_mode_t mode`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_player_get_sort_mode()`

```c
track_sort_mode_t audio_player_get_sort_mode(void);
```

**Descrição Técnica:**  
@brief Retorna o modo de ordenação de faixas atualmente ativo. @return Modo ativo (`SORT_MODE_NAME` ou `SORT_MODE_DATE`).

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `track_sort_mode_t`: Ponteiro, descritor ou handle de recurso gerenciado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

## 📁 Audio Player - Pipeline Interno & DSP Task

- **Cabeçalho:** [`components/audio_player/include/audio_player_internal.h`](components/audio_player/include/audio_player_internal.h)
- **Implementação:** [`components/audio_player/audio_player.cpp`](components/audio_player/audio_player.cpp)
- **Total de Funções Catalogadas:** 8

### - [x] `state_lock()`

```c
void state_lock(void);
```

**Descrição Técnica:**  
@brief Adquire o mutex de sincronização de estado (`s_state_mutex`). Bloqueia a execução até que o lock seja concedido (timeout de 1000 ms).

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `state_unlock()`

```c
void state_unlock(void);
```

**Descrição Técnica:**  
@brief Libera o mutex de sincronização de estado (`s_state_mutex`).

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_dsp_send_pcm()`

```c
esp_err_t audio_dsp_send_pcm(const int32_t *samples, size_t count, uint32_t rate);
```

**Descrição Técnica:**  
@brief Envia um bloco de amostras PCM da decodificação (Core 1) para a fila de DSP (Core 0). Aloca ou reaproveita blocos da fila livre (`s_dsp_free_queue`), preenche as amostras estéreo de 32 bits e enfileira em `s_dsp_ready_queue`. @param[in] samples Ponteiro para as amostras PCM estéreo intercaladas (L/R). @param[in] count   Número total de amostras escalares (frames  2). @param[in] rate    Taxa de amostragem em Hz do bloco enviado. @return - ESP_OK: Bloco enfileirado com sucesso. - ESP_ERR_TIMEOUT: Fila de processamento cheia ou travada.

**Parâmetros:**  
- `const int32_t *samples`: Parâmetro de entrada/saída para a operação.
- `size_t count`: Parâmetro de entrada/saída para a operação.
- `uint32_t rate`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `esp_err_t`: `ESP_OK` em caso de sucesso; código de erro `ESP_ERR_*` em caso de falha.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_dsp_flush()`

```c
void audio_dsp_flush(void);
```

**Descrição Técnica:**  
@brief Descarta todos os blocos PCM acumulados na fila de DSP. Chamado durante operações de seek ou troca de faixa para evitar que áudio antigo continue tocando após o salto.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_dsp_drain()`

```c
void audio_dsp_drain(void);
```

**Descrição Técnica:**  
@brief Aguarda o esgotamento dos buffers DMA de áudio do I2S. Garante que todo o áudio decodificado foi fisicamente emitido pelo DAC antes de alterar taxas de amostragem ou desmontar recursos.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_dsp_set_rate()`

```c
void audio_dsp_set_rate(uint32_t rate);
```

**Descrição Técnica:**  
@brief Notifica a tarefa de DSP e reconfigura o divisor de clock do driver I2S. @param[in] rate Nova taxa de amostragem em Hz (ex: 44100, 48000, 96000, 192000).

**Parâmetros:**  
- `uint32_t rate`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `audio_dsp_trigger_fade_in()`

```c
void audio_dsp_trigger_fade_in(uint32_t sample_rate);
```

**Descrição Técnica:**  
@brief Dispara uma rampa suave de volume (fade-in) na tarefa de DSP. Previne estalos acústicos (pops/clicks) no DAC PCM5102A ao iniciar novas faixas. @param[in] sample_rate Taxa de amostragem da faixa para calibração da duração da rampa.

**Parâmetros:**  
- `uint32_t sample_rate`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `play_web_radio()`

```c
void play_web_radio(const char *url);
```

**Descrição Técnica:**  
@brief Loop de conexão e recepção de pacotes HTTP para Web Rádio. @param[in] url URL do servidor de streaming.

**Parâmetros:**  
- `const char *url`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

## 📁 Audio Player - Navegador de Arquivos (FatFS)

- **Cabeçalho:** [`components/audio_player/include/fs_browser.h`](components/audio_player/include/fs_browser.h)
- **Implementação:** [`components/audio_player/fs_browser.cpp`](components/audio_player/fs_browser.cpp)
- **Total de Funções Catalogadas:** 6

### - [x] `fs_browser_set_sort_mode()`

```c
void fs_browser_set_sort_mode(track_sort_mode_t mode);
```

**Descrição Técnica:**  
@brief Define o critério de ordenação de arquivos e pastas no navegador. @param[in] mode Modo de ordenação desejado (`SORT_MODE_NAME` ou `SORT_MODE_DATE`).

**Parâmetros:**  
- `track_sort_mode_t mode`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `fs_browser_get_sort_mode()`

```c
track_sort_mode_t fs_browser_get_sort_mode(void);
```

**Descrição Técnica:**  
@brief Retorna o critério de ordenação atualmente configurado. @return Modo ativo de ordenação.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `track_sort_mode_t`: Ponteiro, descritor ou handle de recurso gerenciado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `has_supported_extension()`

```c
bool has_supported_extension(const char *name);
```

**Descrição Técnica:**  
@brief Verifica se a extensão do arquivo corresponde a um formato de áudio suportado. Formatos aceitos: .flac, .mp3, .wav, .aac, .m4a, .ogg. @param[in] name Nome do arquivo ou caminho completo. @return true se o arquivo possui formato suportado; false caso contrário.

**Parâmetros:**  
- `const char *name`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `is_junk_entry()`

```c
bool is_junk_entry(const char *name);
```

**Descrição Técnica:**  
@brief Identifica se o arquivo ou diretório é lixo de sistema ou arquivo oculto. Filtra arquivos que começam com '.', pastas de sistema como 'System Volume Information', '._' (arquivos de atributos do macOS) e 'RECYCLED'. @param[in] name Nome do arquivo ou pasta a ser analisado. @return true se for lixo/arquivo oculto; false se for um item legítimo.

**Parâmetros:**  
- `const char *name`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `free_dir_scan()`

```c
void free_dir_scan(DirScan &scan);
```

**Descrição Técnica:**  
@brief Libera toda a memória dinâmica alocada para as strings da estrutura `DirScan`. @param[in,out] scan Referência para a estrutura cujas strings serão liberadas.

**Parâmetros:**  
- `DirScan &scan`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `scan_dir()`

```c
void scan_dir(const char *path, DirScan &out);
```

**Descrição Técnica:**  
@brief Realiza a leitura e catalogação completa de um diretório no cartão MicroSD. Lê todas as entradas da pasta, filtra arquivos indesejados, separa subdiretórios de arquivos de áudio e aplica o algoritmo de ordenação selecionado. @param[in]  path Caminho absoluto do diretório no sistema de arquivos FatFS. @param[out] out  Estrutura onde as listas de pastas e arquivos serão preenchidas.

**Parâmetros:**  
- `const char *path`: Parâmetro de entrada/saída para a operação.
- `DirScan &out`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

## 📁 Audio Player - Extrator de Metadados Hi-Res

- **Cabeçalho:** [`components/audio_player/metadata_extract.h`](components/audio_player/metadata_extract.h)
- **Implementação:** [`components/audio_player/metadata_extract.c`](components/audio_player/metadata_extract.c)
- **Total de Funções Catalogadas:** 1

### - [x] `metadata_extract()`

```c
void metadata_extract(const char *abs_path, track_metadata_t *out);
```

**Descrição Técnica:**  
@brief Lê os metadados e tags do arquivo de áudio especificado. Analisa os primeiros blocos do arquivo procurando tags ID3v2, blocos de metadados FLAC (STREAMINFO, VORBIS_COMMENT) ou chunks RIFF/WAV. Não decodifica o fluxo de áudio, operando em alta velocidade. @param[in]  abs_path Caminho absoluto do arquivo no sistema FatFS (ex: "/sdcard/musica.flac"). @param[out] out      Ponteiro para a estrutura `track_metadata_t` a ser preenchida.

**Parâmetros:**  
- `const char *abs_path`: Parâmetro de entrada/saída para a operação.
- `track_metadata_t *out`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Executado no Core 1 (`player_task`, prioridade 5) ou Core 0 (`audio_dsp_task`, prioridade 5) com sincronização via `s_state_mutex` e filas FreeRTOS (`s_dsp_ready_queue`, `s_dsp_free_queue`).
- **Hardware Envolvido:** Barramento SDMMC 4-Bit nativo (CLK: GPIO 5, CMD: GPIO 6, D0..D3: GPIOs 4, 17, 16, 7) e DMA I2S Master.

- [x] *Função catalogada, documentada e verificada.*

---

## 📁 Board I/O - Monitor de Bateria & Carga

- **Cabeçalho:** [`components/board_io/include/battery.h`](components/board_io/include/battery.h)
- **Implementação:** [`components/board_io/battery.c`](components/board_io/battery.c)
- **Total de Funções Catalogadas:** 6

### - [x] `battery_init()`

```c
esp_err_t battery_init(void);
```

**Descrição Técnica:**  
@brief Inicializa o canal ADC1 One-Shot e configura o pino GPIO de monitoramento do TP4056. @return - ESP_OK: Subsistema de bateria inicializado com sucesso. - ESP_FAIL: Falha ao calibrar o periférico ADC.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `esp_err_t`: `ESP_OK` em caso de sucesso; código de erro `ESP_ERR_*` em caso de falha.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Thread-safe, chamado periodicamente pela `display_task` (Core 0).
- **Hardware Envolvido:** ADC1 One-Shot no GPIO 1 (Divisor resistivo Li-Ion) e GPIO 11 (Monitor TP4056 CHRG com pull-up).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `battery_get_percent()`

```c
int battery_get_percent(void);
```

**Descrição Técnica:**  
@brief Retorna o percentual estimado de carga da bateria. @return Valor de 0 a 100%.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `int`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Thread-safe, chamado periodicamente pela `display_task` (Core 0).
- **Hardware Envolvido:** ADC1 One-Shot no GPIO 1 (Divisor resistivo Li-Ion) e GPIO 11 (Monitor TP4056 CHRG com pull-up).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `battery_get_info()`

```c
void battery_get_info(int *mv, int *percent, int *time_left_mins);
```

**Descrição Técnica:**  
@brief Obtém dados detalhados de telemetria da bateria. @param[out] mv              Ponteiro para preenchimento da tensão medida em milivolts (mV). @param[out] percent         Ponteiro para preenchimento da porcentagem estimada (0 a 100%). @param[out] time_left_mins  Ponteiro para estimativa de autonomia restante em minutos (-1 se carregando).

**Parâmetros:**  
- `int *mv`: Parâmetro de entrada/saída para a operação.
- `int *percent`: Parâmetro de entrada/saída para a operação.
- `int *time_left_mins`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Thread-safe, chamado periodicamente pela `display_task` (Core 0).
- **Hardware Envolvido:** ADC1 One-Shot no GPIO 1 (Divisor resistivo Li-Ion) e GPIO 11 (Monitor TP4056 CHRG com pull-up).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `battery_is_charging()`

```c
bool battery_is_charging(void);
```

**Descrição Técnica:**  
@brief Informa se a bateria está sendo carregada no momento. @return true se o carregador estiver plugado e ativo; false caso contrário.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Thread-safe, chamado periodicamente pela `display_task` (Core 0).
- **Hardware Envolvido:** ADC1 One-Shot no GPIO 1 (Divisor resistivo Li-Ion) e GPIO 11 (Monitor TP4056 CHRG com pull-up).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `battery_is_full()`

```c
bool battery_is_full(void);
```

**Descrição Técnica:**  
@brief Informa se a carga da bateria atingiu 100%. @return true se a bateria estiver totalmente carregada; false caso contrário.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Thread-safe, chamado periodicamente pela `display_task` (Core 0).
- **Hardware Envolvido:** ADC1 One-Shot no GPIO 1 (Divisor resistivo Li-Ion) e GPIO 11 (Monitor TP4056 CHRG com pull-up).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `battery_get_status()`

```c
battery_status_t battery_get_status(void);
```

**Descrição Técnica:**  
@brief Retorna o estado operacional completo da bateria. @return Enum `battery_status_t` (`BATTERY_DISCHARGING`, `BATTERY_CHARGING` ou `BATTERY_FULL`).

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `battery_status_t`: Ponteiro, descritor ou handle de recurso gerenciado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Thread-safe, chamado periodicamente pela `display_task` (Core 0).
- **Hardware Envolvido:** ADC1 One-Shot no GPIO 1 (Divisor resistivo Li-Ion) e GPIO 11 (Monitor TP4056 CHRG com pull-up).

- [x] *Função catalogada, documentada e verificada.*

---

## 📁 Board I/O - Saída I2S Master & PCM5102A

- **Cabeçalho:** [`components/board_io/include/i2s_output.h`](components/board_io/include/i2s_output.h)
- **Implementação:** [`components/board_io/i2s_output.c`](components/board_io/i2s_output.c)
- **Total de Funções Catalogadas:** 8

### - [x] `i2s_output_init()`

```c
esp_err_t i2s_output_init(void);
```

**Descrição Técnica:**  
@brief Inicializa o canal transmissor I2S Master e configura a DMA. Inicializa a interface I2S padrão Philips, slots de 32 bits estéreo, aloca descritores DMA (24 descritores de 512 frames) e ativa o sinal MCLK no GPIO 8. @return - ESP_OK: Driver I2S inicializado com sucesso. - ESP_FAIL: Falha ao alocar canal ou descritores DMA.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `esp_err_t`: `ESP_OK` em caso de sucesso; código de erro `ESP_ERR_*` em caso de falha.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`audio_dsp_task`, prioridade 5). Protegido pelo mutex `s_i2s_mutex`.
- **Hardware Envolvido:** Transmissor I2S Master (BCLK GPIO 48, DOUT GPIO 47, LRCK GPIO 21, MCLK GPIO 8) conectado ao DAC PCM5102A em modo 4 fios.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `i2s_output_set_rate()`

```c
esp_err_t i2s_output_set_rate(uint32_t sample_rate);
```

**Descrição Técnica:**  
@brief Reconfigura a taxa de amostragem do canal I2S (de 8 kHz a 192 kHz). Ajusta dinamicamente os divisores de frequência do clock I2S (`PLL_240M`), selecionando multiplicador de 128fs para taxas >= 176.4 kHz e 256fs para taxas <= 96 kHz. @param[in] sample_rate Frequência alvo em Hz (ex.: 44100, 48000, 96000, 192000). @return - ESP_OK: Taxa reconfigurada com sucesso. - ESP_ERR_INVALID_ARG: Taxa de amostragem não suportada.

**Parâmetros:**  
- `uint32_t sample_rate`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `esp_err_t`: `ESP_OK` em caso de sucesso; código de erro `ESP_ERR_*` em caso de falha.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`audio_dsp_task`, prioridade 5). Protegido pelo mutex `s_i2s_mutex`.
- **Hardware Envolvido:** Transmissor I2S Master (BCLK GPIO 48, DOUT GPIO 47, LRCK GPIO 21, MCLK GPIO 8) conectado ao DAC PCM5102A em modo 4 fios.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `i2s_output_get_rate()`

```c
uint32_t i2s_output_get_rate(void);
```

**Descrição Técnica:**  
@brief Retorna a taxa de amostragem atualmente configurada no canal I2S. @return Frequência em Hz.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `uint32_t`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`audio_dsp_task`, prioridade 5). Protegido pelo mutex `s_i2s_mutex`.
- **Hardware Envolvido:** Transmissor I2S Master (BCLK GPIO 48, DOUT GPIO 47, LRCK GPIO 21, MCLK GPIO 8) conectado ao DAC PCM5102A em modo 4 fios.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `i2s_output_set_rate_change_cb()`

```c
void i2s_output_set_rate_change_cb(void (*cb)(uint32_t sample_rate));
```

**Descrição Técnica:**  
@brief Registra callback notificado sempre que a taxa de amostragem do I2S for alterada. @param[in] cb Ponteiro para a função de notificação `void cb(uint32_t sample_rate)`.

**Parâmetros:**  
- `void (*cb)(uint32_t sample_rate)`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`audio_dsp_task`, prioridade 5). Protegido pelo mutex `s_i2s_mutex`.
- **Hardware Envolvido:** Transmissor I2S Master (BCLK GPIO 48, DOUT GPIO 47, LRCK GPIO 21, MCLK GPIO 8) conectado ao DAC PCM5102A em modo 4 fios.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `i2s_output_write()`

```c
esp_err_t i2s_output_write(const int32_t *samples, size_t sample_count, size_t *samples_written);
```

**Descrição Técnica:**  
@brief Escreve amostras de áudio PCM de 32 bits diretamente nos descritores DMA do I2S. Transmite blocos de amostras estéreo intercaladas (L, R). Bloqueia de forma segura até que haja espaço nos descritores DMA ou ocorra timeout de segurança de 1000 ms. @param[in]  samples         Ponteiro para o buffer de amostras PCM estéreo de 32 bits. @param[in]  sample_count    Número total de amostras a escrever (frames  2). @param[out] samples_written Ponteiro onde será registrado o número de amostras aceitas. @return - ESP_OK: Amostras escritas no buffer DMA. - ESP_ERR_INVALID_STATE: Canal I2S desabilitado ou nulo. - ESP_ERR_TIMEOUT: Timeout na fila de DMA.

**Parâmetros:**  
- `const int32_t *samples`: Parâmetro de entrada/saída para a operação.
- `size_t sample_count`: Parâmetro de entrada/saída para a operação.
- `size_t *samples_written`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `esp_err_t`: `ESP_OK` em caso de sucesso; código de erro `ESP_ERR_*` em caso de falha.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`audio_dsp_task`, prioridade 5). Protegido pelo mutex `s_i2s_mutex`.
- **Hardware Envolvido:** Transmissor I2S Master (BCLK GPIO 48, DOUT GPIO 47, LRCK GPIO 21, MCLK GPIO 8) conectado ao DAC PCM5102A em modo 4 fios.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `i2s_output_disable()`

```c
esp_err_t i2s_output_disable(void);
```

**Descrição Técnica:**  
@brief Desabilita fisicamente o canal de transmissão I2S. Utilizado na operação de Pausa e modos de suspensão para silenciar a saída analógica. @return ESP_OK em caso de sucesso.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `esp_err_t`: `ESP_OK` em caso de sucesso; código de erro `ESP_ERR_*` em caso de falha.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`audio_dsp_task`, prioridade 5). Protegido pelo mutex `s_i2s_mutex`.
- **Hardware Envolvido:** Transmissor I2S Master (BCLK GPIO 48, DOUT GPIO 47, LRCK GPIO 21, MCLK GPIO 8) conectado ao DAC PCM5102A em modo 4 fios.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `i2s_output_enable()`

```c
esp_err_t i2s_output_enable(void);
```

**Descrição Técnica:**  
@brief Reabilita o canal de transmissão I2S após uma pausa. @return ESP_OK em caso de sucesso.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `esp_err_t`: `ESP_OK` em caso de sucesso; código de erro `ESP_ERR_*` em caso de falha.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`audio_dsp_task`, prioridade 5). Protegido pelo mutex `s_i2s_mutex`.
- **Hardware Envolvido:** Transmissor I2S Master (BCLK GPIO 48, DOUT GPIO 47, LRCK GPIO 21, MCLK GPIO 8) conectado ao DAC PCM5102A em modo 4 fios.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `i2s_output_play_test_tone()`

```c
void i2s_output_play_test_tone(uint32_t freq_hz, uint32_t duration_ms);
```

**Descrição Técnica:**  
@brief Injeta um tom senoidal de teste diretamente nos buffers do I2S. Ferramenta de bancada para diagnóstico de integridade elétrica do DAC PCM5102A. @param[in] freq_hz     Frequência do tom em Hz (ex.: 1000 Hz). @param[in] duration_ms Duração da emissão do tom em milissegundos.

**Parâmetros:**  
- `uint32_t freq_hz`: Parâmetro de entrada/saída para a operação.
- `uint32_t duration_ms`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`audio_dsp_task`, prioridade 5). Protegido pelo mutex `s_i2s_mutex`.
- **Hardware Envolvido:** Transmissor I2S Master (BCLK GPIO 48, DOUT GPIO 47, LRCK GPIO 21, MCLK GPIO 8) conectado ao DAC PCM5102A em modo 4 fios.

- [x] *Função catalogada, documentada e verificada.*

---

## 📁 Board I/O - Governador de Energia & Frequência (DFS)

- **Cabeçalho:** [`components/board_io/include/pwr_governor.h`](components/board_io/include/pwr_governor.h)
- **Implementação:** [`components/board_io/pwr_governor.c`](components/board_io/pwr_governor.c)
- **Total de Funções Catalogadas:** 9

### - [x] `pwr_governor_init()`

```c
esp_err_t pwr_governor_init(void);
```

**Descrição Técnica:**  
@brief Inicializa o subsistema de gerenciamento de clock e energia. Registra o estado inicial de clock e prepara as fontes de despertar do RTC. @return ESP_OK em caso de sucesso.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `esp_err_t`: `ESP_OK` em caso de sucesso; código de erro `ESP_ERR_*` em caso de falha.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Chamado pela `display_task` ou tarefas de controle. Thread-safe com mutex interno.
- **Hardware Envolvido:** Gerador de Clock PLL/XTAL do SoC ESP32-S3 (40/80/160/240 MHz), RTC IOs de wakeup e domínios de energia PMU (`ESP_PD_DOMAIN_RTC_PERIPH`).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `pwr_governor_set_cpu_freq()`

```c
esp_err_t pwr_governor_set_cpu_freq(pwr_freq_t freq);
```

**Descrição Técnica:**  
@brief Comuta a frequência de operação da CPU dinamicamente (40, 80, 160 ou 240 MHz). @param[in] freq Frequência alvo em MHz. @return - ESP_OK: Frequência comutada com sucesso. - ESP_ERR_INVALID_ARG: Frequência não suportada pelo hardware. @note Thread-safe. Protegido por mutex interno.

**Parâmetros:**  
- `pwr_freq_t freq`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `esp_err_t`: `ESP_OK` em caso de sucesso; código de erro `ESP_ERR_*` em caso de falha.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Chamado pela `display_task` ou tarefas de controle. Thread-safe com mutex interno.
- **Hardware Envolvido:** Gerador de Clock PLL/XTAL do SoC ESP32-S3 (40/80/160/240 MHz), RTC IOs de wakeup e domínios de energia PMU (`ESP_PD_DOMAIN_RTC_PERIPH`).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `pwr_governor_get_cpu_freq()`

```c
pwr_freq_t pwr_governor_get_cpu_freq(void);
```

**Descrição Técnica:**  
@brief Retorna a frequência de operação atual da CPU. @return Frequência atual em MHz (`pwr_freq_t`).

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `pwr_freq_t`: Ponteiro, descritor ou handle de recurso gerenciado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Chamado pela `display_task` ou tarefas de controle. Thread-safe com mutex interno.
- **Hardware Envolvido:** Gerador de Clock PLL/XTAL do SoC ESP32-S3 (40/80/160/240 MHz), RTC IOs de wakeup e domínios de energia PMU (`ESP_PD_DOMAIN_RTC_PERIPH`).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `pwr_governor_update()`

```c
void pwr_governor_update(bool screen_active, bool wifi_active, bool seeking, bool playing, uint32_t sample_rate);
```

**Descrição Técnica:**  
@brief Avalia o estado geral do sistema e seleciona o perfil de clock ideal: - Tela acordada (Navegação/UI): 160 MHz. - Tela em repouso/bloqueada (MP3/WAV/AAC padrão <= 48kHz ou Pausado): 80 MHz. - Tela em repouso/bloqueada (FLAC Hi-Res 96k): 160 MHz. - Hi-Res Extremo (FLAC 24/192k) ou Wi-Fi ativo ou Seek: 240 MHz. @param[in] screen_active true se o display OLED estiver ligado/ativo. @param[in] wifi_active   true se o rádio Wi-Fi ou servidor HTTPD estiver ativo. @param[in] seeking       true se há um salto de busca (seek) em processamento. @param[in] playing       true se há decodificação ativa de música no momento. @param[in] sample_rate   Taxa de amostragem da música atual em Hz.

**Parâmetros:**  
- `bool screen_active`: Parâmetro de entrada/saída para a operação.
- `bool wifi_active`: Parâmetro de entrada/saída para a operação.
- `bool seeking`: Parâmetro de entrada/saída para a operação.
- `bool playing`: Parâmetro de entrada/saída para a operação.
- `uint32_t sample_rate`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Chamado pela `display_task` ou tarefas de controle. Thread-safe com mutex interno.
- **Hardware Envolvido:** Gerador de Clock PLL/XTAL do SoC ESP32-S3 (40/80/160/240 MHz), RTC IOs de wakeup e domínios de energia PMU (`ESP_PD_DOMAIN_RTC_PERIPH`).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `pwr_governor_set_manual_mode()`

```c
void pwr_governor_set_manual_mode(bool manual);
```

**Descrição Técnica:**  
@brief Ativa ou desativa o modo manual de clock. Quando ativo, impede ajustes automáticos pelo governador para permitir testes de bancada. @param[in] manual true para travar no clock atual; false para permitir ajuste automático.

**Parâmetros:**  
- `bool manual`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Chamado pela `display_task` ou tarefas de controle. Thread-safe com mutex interno.
- **Hardware Envolvido:** Gerador de Clock PLL/XTAL do SoC ESP32-S3 (40/80/160/240 MHz), RTC IOs de wakeup e domínios de energia PMU (`ESP_PD_DOMAIN_RTC_PERIPH`).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `pwr_governor_is_manual_mode()`

```c
bool pwr_governor_is_manual_mode(void);
```

**Descrição Técnica:**  
@brief Retorna se o modo manual de clock está ativo. @return true se o modo manual estiver ativo; false caso contrário.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Chamado pela `display_task` ou tarefas de controle. Thread-safe com mutex interno.
- **Hardware Envolvido:** Gerador de Clock PLL/XTAL do SoC ESP32-S3 (40/80/160/240 MHz), RTC IOs de wakeup e domínios de energia PMU (`ESP_PD_DOMAIN_RTC_PERIPH`).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `pwr_governor_enter_light_sleep()`

```c
esp_err_t pwr_governor_enter_light_sleep(uint32_t timeout_ms);
```

**Descrição Técnica:**  
@brief Entra em modo Light Sleep (consumo reduzido ~2-3 mA). Permite despertar imediato através de pulsos no joystick, inserção do carregador (GPIO 11) ou timer. @param[in] timeout_ms Tempo máximo de repouso em milissegundos (0 = sem timeout de timer). @return ESP_OK após o retorno da suspensão.

**Parâmetros:**  
- `uint32_t timeout_ms`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `esp_err_t`: `ESP_OK` em caso de sucesso; código de erro `ESP_ERR_*` em caso de falha.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Chamado pela `display_task` ou tarefas de controle. Thread-safe com mutex interno.
- **Hardware Envolvido:** Gerador de Clock PLL/XTAL do SoC ESP32-S3 (40/80/160/240 MHz), RTC IOs de wakeup e domínios de energia PMU (`ESP_PD_DOMAIN_RTC_PERIPH`).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `pwr_governor_set_sleep_cb()`

```c
void pwr_governor_set_sleep_cb(pwr_sleep_cb_t cb);
```

**Descrição Técnica:**  
@brief Registra callback invocado antes de entrar em Deep Sleep (ex: desligar display e isolar pinos). @param[in] cb Ponteiro para a função de callback.

**Parâmetros:**  
- `pwr_sleep_cb_t cb`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Chamado pela `display_task` ou tarefas de controle. Thread-safe com mutex interno.
- **Hardware Envolvido:** Gerador de Clock PLL/XTAL do SoC ESP32-S3 (40/80/160/240 MHz), RTC IOs de wakeup e domínios de energia PMU (`ESP_PD_DOMAIN_RTC_PERIPH`).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `pwr_governor_enter_deep_sleep()`

```c
void pwr_governor_enter_deep_sleep(void);
```

**Descrição Técnica:**  
@brief Entra em modo Deep Sleep definitivo (< 100 µA). Desliga periféricos e configura o despertar por hardware nos pinos JOY_UP (GPIO 2) e BATTERY_CHRG (GPIO 11) no domínio `ESP_PD_DOMAIN_RTC_PERIPH`. Não retorna (reinicia a CPU a partir do bootloader ROM ao despertar).

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Chamado pela `display_task` ou tarefas de controle. Thread-safe com mutex interno.
- **Hardware Envolvido:** Gerador de Clock PLL/XTAL do SoC ESP32-S3 (40/80/160/240 MHz), RTC IOs de wakeup e domínios de energia PMU (`ESP_PD_DOMAIN_RTC_PERIPH`).

- [x] *Função catalogada, documentada e verificada.*

---

## 📁 Board I/O - LED RGB WS2812

- **Cabeçalho:** [`components/board_io/include/rgb_led.h`](components/board_io/include/rgb_led.h)
- **Implementação:** [`components/board_io/rgb_led.c`](components/board_io/rgb_led.c)
- **Total de Funções Catalogadas:** 6

### - [x] `rgb_led_init()`

```c
esp_err_t rgb_led_init(void);
```

**Descrição Técnica:**  
@brief Inicializa o canal RMT e configura os temporizadores de bit do WS2812. @return ESP_OK se o periférico RMT foi inicializado com sucesso.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `esp_err_t`: `ESP_OK` em caso de sucesso; código de erro `ESP_ERR_*` em caso de falha.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `rgb_led_set_color()`

```c
void rgb_led_set_color(uint8_t r, uint8_t g, uint8_t b);
```

**Descrição Técnica:**  
@brief Define imediatamente as cores R, G, B do LED sem persistir na NVS. @param[in] r Intensidade vermelha (0-255). @param[in] g Intensidade verde (0-255). @param[in] b Intensidade azul (0-255).

**Parâmetros:**  
- `uint8_t r`: Parâmetro de entrada/saída para a operação.
- `uint8_t g`: Parâmetro de entrada/saída para a operação.
- `uint8_t b`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `rgb_led_get_config()`

```c
void rgb_led_get_config(rgb_led_config_t *out_cfg);
```

**Descrição Técnica:**  
@brief Obtém uma cópia da configuração atual do LED RGB. @param[out] out_cfg Ponteiro para a estrutura `rgb_led_config_t` de saída.

**Parâmetros:**  
- `rgb_led_config_t *out_cfg`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `rgb_led_set_config()`

```c
void rgb_led_set_config(const rgb_led_config_t *cfg);
```

**Descrição Técnica:**  
@brief Aplica uma nova estrutura de configuração ao LED RGB. @param[in] cfg Ponteiro para a configuração desejada.

**Parâmetros:**  
- `const rgb_led_config_t *cfg`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `rgb_led_toggle()`

```c
void rgb_led_toggle(void);
```

**Descrição Técnica:**  
@brief Alterna o estado do LED entre ligado e desligado (toggle).

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `rgb_led_save_nvs()`

```c
void rgb_led_save_nvs(void);
```

**Descrição Técnica:**  
@brief Salva a configuração atual de cores e estado do LED na memória NVS.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

## 📁 Board I/O - Relógio de Tempo Real RTC DS3231

- **Cabeçalho:** [`components/board_io/include/rtc_ds3231.h`](components/board_io/include/rtc_ds3231.h)
- **Implementação:** [`components/board_io/rtc_ds3231.c`](components/board_io/rtc_ds3231.c)
- **Total de Funções Catalogadas:** 7

### - [x] `rtc_ds3231_detect()`

```c
bool rtc_ds3231_detect(i2c_master_bus_handle_t bus_handle);
```

**Descrição Técnica:**  
@brief Verifica se o DS3231 responde no barramento I2C fornecido. @param[in] bus_handle Handle do barramento I2C mestre já inicializado. @return true se o dispositivo respondeu ACK no endereço 0x68; false caso contrário.

**Parâmetros:**  
- `i2c_master_bus_handle_t bus_handle`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Compartilha o barramento I2C mestre com o display de forma protegida por mutex thread-safe.
- **Hardware Envolvido:** Chip RTC DS3231 no endereço 0x68 via I2C (SDA GPIO 10, SCL GPIO 9) com oscilador TCXO de alta precisão.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `rtc_ds3231_init()`

```c
esp_err_t rtc_ds3231_init(i2c_master_bus_handle_t bus_handle);
```

**Descrição Técnica:**  
@brief Inicializa o driver do DS3231 registrando o dispositivo no barramento I2C. Verifica o oscilador interno e limpa a flag de parada do oscilador (OSF) se necessário. @param[in] bus_handle Handle do barramento I2C mestre já inicializado. @return - ESP_OK: Dispositivo registrado e operacional. - ESP_FAIL: Falha de comunicação ou dispositivo não detectado.

**Parâmetros:**  
- `i2c_master_bus_handle_t bus_handle`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `esp_err_t`: `ESP_OK` em caso de sucesso; código de erro `ESP_ERR_*` em caso de falha.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Compartilha o barramento I2C mestre com o display de forma protegida por mutex thread-safe.
- **Hardware Envolvido:** Chip RTC DS3231 no endereço 0x68 via I2C (SDA GPIO 10, SCL GPIO 9) com oscilador TCXO de alta precisão.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `rtc_ds3231_is_available()`

```c
bool rtc_ds3231_is_available(void);
```

**Descrição Técnica:**  
@brief Retorna se o DS3231 foi detectado e está inicializado com sucesso. @return true se o RTC está disponível e pronto para uso; false caso contrário.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Compartilha o barramento I2C mestre com o display de forma protegida por mutex thread-safe.
- **Hardware Envolvido:** Chip RTC DS3231 no endereço 0x68 via I2C (SDA GPIO 10, SCL GPIO 9) com oscilador TCXO de alta precisão.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `rtc_ds3231_get_time()`

```c
esp_err_t rtc_ds3231_get_time(struct tm *timeinfo);
```

**Descrição Técnica:**  
@brief Lê a data e hora atuais gravadas nos registradores do DS3231. @param[out] timeinfo Ponteiro para struct tm do POSIX que receberá os campos decodificados. @return ESP_OK se a leitura foi realizada com sucesso.

**Parâmetros:**  
- `struct tm *timeinfo`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `esp_err_t`: `ESP_OK` em caso de sucesso; código de erro `ESP_ERR_*` em caso de falha.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Compartilha o barramento I2C mestre com o display de forma protegida por mutex thread-safe.
- **Hardware Envolvido:** Chip RTC DS3231 no endereço 0x68 via I2C (SDA GPIO 10, SCL GPIO 9) com oscilador TCXO de alta precisão.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `rtc_ds3231_set_time()`

```c
esp_err_t rtc_ds3231_set_time(const struct tm *timeinfo);
```

**Descrição Técnica:**  
@brief Grava uma nova data e hora nos registradores BCD do DS3231. @param[in] timeinfo Ponteiro para struct tm contendo os novos valores de data e hora. @return ESP_OK se gravado com sucesso.

**Parâmetros:**  
- `const struct tm *timeinfo`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `esp_err_t`: `ESP_OK` em caso de sucesso; código de erro `ESP_ERR_*` em caso de falha.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Compartilha o barramento I2C mestre com o display de forma protegida por mutex thread-safe.
- **Hardware Envolvido:** Chip RTC DS3231 no endereço 0x68 via I2C (SDA GPIO 10, SCL GPIO 9) com oscilador TCXO de alta precisão.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `rtc_ds3231_get_temperature()`

```c
esp_err_t rtc_ds3231_get_temperature(float *temp_c);
```

**Descrição Técnica:**  
@brief Lê a temperatura interna do sensor compensado por temperatura (TCXO) do DS3231. @param[out] temp_c Ponteiro para float onde será gravada a temperatura em graus Celsius (°C). @return ESP_OK se lido com sucesso.

**Parâmetros:**  
- `float *temp_c`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `esp_err_t`: `ESP_OK` em caso de sucesso; código de erro `ESP_ERR_*` em caso de falha.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Compartilha o barramento I2C mestre com o display de forma protegida por mutex thread-safe.
- **Hardware Envolvido:** Chip RTC DS3231 no endereço 0x68 via I2C (SDA GPIO 10, SCL GPIO 9) com oscilador TCXO de alta precisão.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `rtc_ds3231_sync_system_time()`

```c
esp_err_t rtc_ds3231_sync_system_time(void);
```

**Descrição Técnica:**  
@brief Sincroniza o relógio do sistema operacional (`settimeofday`) a partir do DS3231. Após a execução desta função, chamadas da biblioteca C padrão (`time()`, `localtime()`) e o sistema de arquivos FATFS utilizam carimbos de data e hora reais nos arquivos do SD. @return ESP_OK se sincronizado com sucesso.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `esp_err_t`: `ESP_OK` em caso de sucesso; código de erro `ESP_ERR_*` em caso de falha.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Compartilha o barramento I2C mestre com o display de forma protegida por mutex thread-safe.
- **Hardware Envolvido:** Chip RTC DS3231 no endereço 0x68 via I2C (SDA GPIO 10, SCL GPIO 9) com oscilador TCXO de alta precisão.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `rtc_ds3231_get_clock_info()`

```c
esp_err_t rtc_ds3231_get_clock_info(rtc_clock_info_t *info);
```

**Descrição Técnica:**  
@brief Obtém data, hora de alta resolução (com milissegundos) e telemetria de temperatura do DS3231. Realiza a leitura direta dos registradores do RTC via I2C, rastreia a transição do segundo e interpola os milissegundos para atualização fluida do relógio na interface gráfica. @param[out] info Ponteiro para estrutura rtc_clock_info_t que receberá os dados. @return ESP_OK se lido com sucesso; código de erro caso contrário.

**Parâmetros:**  
- `rtc_clock_info_t *info`: Ponteiro para a estrutura que recebe hora, minuto, segundo, milissegundos, data e temperatura.

**Retorno:**  
- `esp_err_t`: `ESP_OK` em caso de sucesso; código de erro `ESP_ERR_*` em caso de falha.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Invocada pela tarefa de interface (`display_task` no Core 0) a cada quadro de renderização.
- **Hardware Envolvido:** Barramento I2C mestre (GPIO 10 SDA, GPIO 9 SCL) operando com o RTC DS3231 (0x68).

- [x] *Função catalogada, documentada e verificada.*

---

## 📁 Bluetooth Link - Co-processador & LDAC

- **Cabeçalho:** [`components/bt_link/include/bt_link.h`](components/bt_link/include/bt_link.h)
- **Implementação:** [`components/bt_link/bt_link.c`](components/bt_link/bt_link.c)
- **Total de Funções Catalogadas:** 4

### - [x] `bt_link_init()`

```c
void bt_link_init(void);
```

**Descrição Técnica:**  
@brief Inicializa a UART de controle inter-MCU e registra callbacks de amostragem. Configura o canal UART1 com o co-processador e monitora mudanças de taxa de amostragem no driver I2S para informar o reamostrador do co-processador.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `bt_link_register_menu_entry()`

```c
void bt_link_register_menu_entry(void);
```

**Descrição Técnica:**  
@brief Registra a entrada "Bluetooth" no menu carrossel principal do sistema.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `bt_link_reset_companion()`

```c
void bt_link_reset_companion(void);
```

**Descrição Técnica:**  
@brief Gera um pulso de reset em nível baixo no pino EN (GPIO 12) do co-processador.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `bt_link_send_volume()`

```c
void bt_link_send_volume(uint8_t volume_percent);
```

**Descrição Técnica:**  
@brief Envia comando de volume absoluto ao co-processador para sincronização AVRCP. @param[in] volume_percent Nível de volume em porcentagem (0 a 100%).

**Parâmetros:**  
- `uint8_t volume_percent`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

## 📁 DSP - Equalizador Paramétrico de 10 Bandas

- **Cabeçalho:** [`components/eq/include/eq.h`](components/eq/include/eq.h)
- **Implementação:** [`components/eq/eq.cpp`](components/eq/eq.cpp)
- **Total de Funções Catalogadas:** 7

### - [x] `eq_init()`

```c
void eq_init(void);
```

**Descrição Técnica:**  
@brief Inicializa o equalizador paramétrico e restaura a configuração salva na NVS. Aloca as estruturas de estado dos filtros em SRAM interna de alta velocidade (`MALLOC_CAP_INTERNAL`) para máxima performance no pipeline de áudio.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `eq_set_sample_rate()`

```c
void eq_set_sample_rate(uint32_t sample_rate);
```

**Descrição Técnica:**  
@brief Recalcula os coeficientes dos filtros biquad para a nova taxa de amostragem. Deve ser invocada antes de iniciar o processamento de novas faixas quando a taxa de amostragem mudar (ex: de 44.1 kHz para 48 kHz). @param[in] sample_rate Frequência de amostragem da faixa em Hz.

**Parâmetros:**  
- `uint32_t sample_rate`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `eq_update_config()`

```c
void eq_update_config(const eq_config_t *config);
```

**Descrição Técnica:**  
@brief Atualiza os ganhos das bandas, recalcula o Auto Pre-cut e persiste na NVS. @param[in] config Ponteiro para a nova configuração a ser aplicada.

**Parâmetros:**  
- `const eq_config_t *config`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `eq_process()`

```c
void eq_process(int32_t *buffer, size_t samples);
```

**Descrição Técnica:**  
@brief Processa um bloco de amostras PCM de áudio estéreo em 32 bits (in-place). Aplica os 10 filtros biquad em cascata nos canais esquerdo e direito. Para taxas acima de 48 kHz, o processamento entra automaticamente em modo Bit-Perfect direto sem alteração de amostras para economizar ciclos de CPU. @param[in,out] buffer  Ponteiro para as amostras estéreo intercaladas (L, R). @param[in]     samples Número total de amostras escalares (frames  2). @note Executa no Core 0 dentro de `audio_dsp_task`.

**Parâmetros:**  
- `int32_t *buffer`: Parâmetro de entrada/saída para a operação.
- `size_t samples`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `eq_get_config()`

```c
void eq_get_config(eq_config_t *config);
```

**Descrição Técnica:**  
@brief Obtém uma cópia da configuração atual do equalizador. @param[out] config Ponteiro para a estrutura `eq_config_t` de saída.

**Parâmetros:**  
- `eq_config_t *config`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `eq_reset_to_defaults()`

```c
void eq_reset_to_defaults(void);
```

**Descrição Técnica:**  
@brief Restaura todos os ganhos do equalizador para a curva plana (Flat 0 dB).

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `eq_reset_state()`

```c
void eq_reset_state(void);
```

**Descrição Técnica:**  
@brief Zera os registradores de atraso histórico (z1, z2) dos filtros biquad. Evita transientes e ruídos residuais (pops) ao alternar de faixa ou após saltos de busca (seek).

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

## 📁 Interface - Sistema de Menus e Navegação

- **Cabeçalho:** [`components/menu/include/menu.h`](components/menu/include/menu.h)
- **Implementação:** [`components/menu/menu.c`](components/menu/menu.c)
- **Total de Funções Catalogadas:** 11

### - [x] `menu_register()`

```c
void menu_register(const menu_item_t *item);
```

**Descrição Técnica:**  
@brief Registra um item no menu principal na próxima posição livre do carrossel. A ordem de registro no boot define a sequência visual dos itens (0, 1, 2...). @param[in] item Ponteiro para a estrutura com os dados e callbacks do menu.

**Parâmetros:**  
- `const menu_item_t *item`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `menu_count()`

```c
int menu_count(void);
```

**Descrição Técnica:**  
@brief Retorna a quantidade de itens atualmente registrados no menu. @return Total de itens cadastrados.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `int`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `menu_select()`

```c
void menu_select(int index);
```

**Descrição Técnica:**  
@brief Dispara o callback `on_select()` do item correspondente ao índice fornecido. @param[in] index Posição do item no carrossel.

**Parâmetros:**  
- `int index`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `menu_any_active()`

```c
bool menu_any_active(void);
```

**Descrição Técnica:**  
@brief Verifica se algum dos itens registrados está operando em modo de tela cheia exclusivo. @return true se algum item possui `is_active() == true`; false caso contrário.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `menu_poll_active()`

```c
bool menu_poll_active(void);
```

**Descrição Técnica:**  
@brief Executa o ciclo periódico de polling do item que está ativo no momento. @return true no ciclo exato em que o modo terminou; false caso contrário.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `menu_draw_active_status()`

```c
void menu_draw_active_status(void);
```

**Descrição Técnica:**  
@brief Invoca a rotina de desenho de status do modo atualmente em foco.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `menu_request_exit_active()`

```c
void menu_request_exit_active(void);
```

**Descrição Técnica:**  
@brief Solicita a saída e fechamento do modo ativo no momento.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `menu_request_action_active()`

```c
void menu_request_action_active(void);
```

**Descrição Técnica:**  
@brief Dispara a ação de clique contextual do modo em execução.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `menu_nav_up_active()`

```c
void menu_nav_up_active(void);
```

**Descrição Técnica:**  
@brief Envia evento de navegação para cima ao modo ativo.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `menu_nav_down_active()`

```c
void menu_nav_down_active(void);
```

**Descrição Técnica:**  
@brief Envia evento de navegação para baixo ao modo ativo.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `menu_nav_select_active()`

```c
void menu_nav_select_active(void);
```

**Descrição Técnica:**  
@brief Envia evento de seleção/avanço ao modo ativo.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

## 📁 Interface - Display OLED SSD1306

- **Cabeçalho:** [`components/oled_display/include/oled_display.h`](components/oled_display/include/oled_display.h)
- **Implementação:** [`components/oled_display/oled_display.c`](components/oled_display/oled_display.c)
- **Total de Funções Catalogadas:** 38

### - [x] `oled_display_init()`

```c
esp_err_t oled_display_init(void);
```

**Descrição Técnica:**  
@brief Inicializa o display OLED SSD1306 e a camada gráfica U8g2. Configura o barramento I2C, os modos transparentes de fontes (`u8g2_SetFontMode`) e bitmaps para evitar artefatos em caixas de seleção, e liga a alimentação de tela. @return ESP_OK se o display respondeu e foi inicializado com sucesso.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `esp_err_t`: `ESP_OK` em caso de sucesso; código de erro `ESP_ERR_*` em caso de falha.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_main_menu()`

```c
void oled_display_show_main_menu(int cursor);
```

**Descrição Técnica:**  
@brief Desenha o menu principal em carrossel horizontal com ícones animados. @param[in] cursor Índice do ícone selecionado (Player, WiFi, Conf, USB, Game).

**Parâmetros:**  
- `int cursor`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_update_animations()`

```c
void oled_display_update_animations(void);
```

**Descrição Técnica:**  
@brief Atualiza contadores de frames e transições de animação gráfica. Deve ser invocada a cada ciclo da `display_task` antes da renderização de tela.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_now_playing()`

```c
void oled_display_show_now_playing(const playback_state_t *state);
```

**Descrição Técnica:**  
@brief Renderiza a tela principal de reprodução de música (Now Playing). Exibe status de Play/Pause, percentual da bateria, artista, título com rolagem de texto (marquee), barra de progresso proporcional com tempo embutido em modo XOR, álbum e badges decorativos com formato, bits por amostra e taxa de amostragem. @param[in] state Ponteiro para o estado atual de reprodução (`playback_state_t`).

**Parâmetros:**  
- `const playback_state_t *state`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_list()`

```c
void oled_display_show_list(const char **names, int count, int cursor, const playback_state_t *now_playing);
```

**Descrição Técnica:**  
@brief Exibe a lista de navegação de arquivos e diretórios. Desenha os itens com cursor visual, rolagem de nomes longos e barra superior fina de reprodução caso uma música esteja tocando em segundo plano. @param[in] names       Vetor de strings com os nomes das entradas. @param[in] count       Total de entradas na lista. @param[in] cursor      Índice do item selecionado. @param[in] now_playing Estado da música em segundo plano (ou NULL).

**Parâmetros:**  
- `const char **names`: Parâmetro de entrada/saída para a operação.
- `int count`: Parâmetro de entrada/saída para a operação.
- `int cursor`: Parâmetro de entrada/saída para a operação.
- `const playback_state_t *now_playing`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_message()`

```c
void oled_display_show_message(const char *line1, const char *line2);
```

**Descrição Técnica:**  
@brief Exibe mensagem informativa temporária de até duas linhas de texto. @param[in] line1 Texto da primeira linha. @param[in] line2 Texto da segunda linha (ou NULL).

**Parâmetros:**  
- `const char *line1`: Parâmetro de entrada/saída para a operação.
- `const char *line2`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_bt_devices()`

```c
void oled_display_show_bt_devices(const char **names, int count, int cursor, bool scanning);
```

**Descrição Técnica:**  
@brief Exibe a lista de fones de ouvido e dispositivos Bluetooth encontrados. @param[in] names    Vetor com nomes dos dispositivos descobertos. @param[in] count    Quantidade de dispositivos na lista. @param[in] cursor   Índice do dispositivo focado. @param[in] scanning true se a varredura Bluetooth ainda estiver ativa.

**Parâmetros:**  
- `const char **names`: Parâmetro de entrada/saída para a operação.
- `int count`: Parâmetro de entrada/saída para a operação.
- `int cursor`: Parâmetro de entrada/saída para a operação.
- `bool scanning`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_bt_connected()`

```c
void oled_display_show_bt_connected(const char *dev_name, const char *codec_name, uint32_t sample_rate, int8_t rssi);
```

**Descrição Técnica:**  
@brief Exibe status de conexão Bluetooth ativa com fone/caixa de som. @param[in] dev_name    Nome do dispositivo conectado. @param[in] codec_name  Nome do codec ativo (ex: "Sony LDAC", "SBC"). @param[in] sample_rate Frequência em Hz (ex: 96000). @param[in] rssi        Potência do sinal de rádio em dBm.

**Parâmetros:**  
- `const char *dev_name`: Parâmetro de entrada/saída para a operação.
- `const char *codec_name`: Parâmetro de entrada/saída para a operação.
- `uint32_t sample_rate`: Parâmetro de entrada/saída para a operação.
- `int8_t rssi`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_blank()`

```c
void oled_display_blank(void);
```

**Descrição Técnica:**  
@brief Limpa o buffer de vídeo e apaga todos os pixels da tela.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_volume()`

```c
void oled_display_show_volume(int volume_percent);
```

**Descrição Técnica:**  
@brief Exibe popup de volume em tela cheia com barra percentual e valor numérico. @param[in] volume_percent Volume atual de 0 a 100%.

**Parâmetros:**  
- `int volume_percent`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_balance()`

```c
void oled_display_show_balance(int balance);
```

**Descrição Técnica:**  
@brief Exibe popup de ajuste de balanço estéreo L/R. @param[in] balance Valor de -100 (L) a +100 (R).

**Parâmetros:**  
- `int balance`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_locked()`

```c
void oled_display_show_locked(void);
```

**Descrição Técnica:**  
@brief Exibe tela de bloqueio com ícone de cadeado.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_reset_life()`

```c
void oled_display_reset_life(void);
```

**Descrição Técnica:**  
@brief Reinicializa a matriz do protetor de tela Game of Life com padrão aleatório.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_usb_mode()`

```c
void oled_display_show_usb_mode(void);
```

**Descrição Técnica:**  
@brief Exibe tela indicando que o cartão MicroSD está cedido ao host USB.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_transfer()`

```c
void oled_display_show_transfer(const char *label, const char *filename, int percent, int eta_sec, int overall_pct, int count);
```

**Descrição Técnica:**  
@brief Exibe tela genérica de transferência de arquivos (USB ou Wi-Fi). @param[in] label       Rótulo ou cabeçalho da operação. @param[in] filename    Nome do arquivo em transferência. @param[in] percent     Progresso percentual do arquivo atual (0 a 100). @param[in] eta_sec     Tempo estimado restante em segundos (< 0 mostra calculando). @param[in] overall_pct Progresso do lote total (-1 se avulso). @param[in] count       Total de arquivos do lote.

**Parâmetros:**  
- `const char *label`: Parâmetro de entrada/saída para a operação.
- `const char *filename`: Parâmetro de entrada/saída para a operação.
- `int percent`: Parâmetro de entrada/saída para a operação.
- `int eta_sec`: Parâmetro de entrada/saída para a operação.
- `int overall_pct`: Parâmetro de entrada/saída para a operação.
- `int count`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_wifi_idle()`

```c
void oled_display_show_wifi_idle(const char *ip, const char *mdns_host, bool dns_active, int rssi);
```

**Descrição Técnica:**  
@brief Exibe tela de espera de conexão Wi-Fi (modo ocioso). @param[in] ip         Endereço IP atribuído. @param[in] mdns_host  Nome mDNS da máquina (ex: "mps3.local"). @param[in] dns_active true se o servidor DNS captive portal estiver ligado. @param[in] rssi       Intensidade do sinal da rede Wi-Fi.

**Parâmetros:**  
- `const char *ip`: Parâmetro de entrada/saída para a operação.
- `const char *mdns_host`: Parâmetro de entrada/saída para a operação.
- `bool dns_active`: Parâmetro de entrada/saída para a operação.
- `int rssi`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_wifi_connected()`

```c
void oled_display_show_wifi_connected(const char *ip, const char *mdns_host, int rssi, int clients);
```

**Descrição Técnica:**  
@brief Exibe tela de Wi-Fi conectado com contagem de clientes e endereço web. @param[in] ip        Endereço IP ativo. @param[in] mdns_host Nome de domínio local. @param[in] rssi      Sinal em dBm. @param[in] clients   Número de clientes simultâneos conectados.

**Parâmetros:**  
- `const char *ip`: Parâmetro de entrada/saída para a operação.
- `const char *mdns_host`: Parâmetro de entrada/saída para a operação.
- `int rssi`: Parâmetro de entrada/saída para a operação.
- `int clients`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_wifi_transfer()`

```c
void oled_display_show_wifi_transfer(const char *filename, int file_pct, int overall_pct, int idx, int count, float kbps, int eta_sec, int total_eta, int rssi);
```

**Descrição Técnica:**  
@brief Exibe tela detalhada de transferência de arquivos via Wi-Fi. @param[in] filename    Nome do arquivo sendo transferido. @param[in] file_pct    Progresso percentual do arquivo atual. @param[in] overall_pct Progresso geral do lote. @param[in] idx         Índice do arquivo atual. @param[in] count       Total de arquivos no lote. @param[in] kbps        Velocidade de transferência em KB/s. @param[in] eta_sec     Tempo restante do arquivo atual. @param[in] total_eta   Tempo estimado restante para o lote completo. @param[in] rssi        Nível de sinal Wi-Fi.

**Parâmetros:**  
- `const char *filename`: Parâmetro de entrada/saída para a operação.
- `int file_pct`: Parâmetro de entrada/saída para a operação.
- `int overall_pct`: Parâmetro de entrada/saída para a operação.
- `int idx`: Parâmetro de entrada/saída para a operação.
- `int count`: Parâmetro de entrada/saída para a operação.
- `float kbps`: Parâmetro de entrada/saída para a operação.
- `int eta_sec`: Parâmetro de entrada/saída para a operação.
- `int total_eta`: Parâmetro de entrada/saída para a operação.
- `int rssi`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_wifi_done()`

```c
void oled_display_show_wifi_done(int files_received);
```

**Descrição Técnica:**  
@brief Exibe tela de confirmação de conclusão de transferência Wi-Fi. @param[in] files_received Quantidade de arquivos recebidos com sucesso.

**Parâmetros:**  
- `int files_received`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_eq()`

```c
void oled_display_show_eq(const float gains[11], int selected_band, bool eq_enabled);
```

**Descrição Técnica:**  
@brief Renderiza a interface visual de ajuste do equalizador paramétrico. @param[in] gains          Vetor de ganhos em dB das 10 bandas + ganho geral. @param[in] selected_band  Banda atualmente selecionada pelo cursor. @param[in] eq_enabled     true se o equalizador estiver ativo; false se em bypass.

**Parâmetros:**  
- `const float gains[11]`: Parâmetro de entrada/saída para a operação.
- `int selected_band`: Parâmetro de entrada/saída para a operação.
- `bool eq_enabled`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_led()`

```c
void oled_display_show_led(uint8_t r, uint8_t g, uint8_t b, int selected_channel, bool enabled);
```

**Descrição Técnica:**  
@brief Exibe a interface de controle do LED RGB WS2812 on-board. @param[in] r                Valor do canal Vermelho (0 a 255). @param[in] g                Valor do canal Verde (0 a 255). @param[in] b                Valor do canal Azul (0 a 255). @param[in] selected_channel Canal em ajuste (0=R, 1=G, 2=B). @param[in] enabled          Estado ligado/desligado do LED.

**Parâmetros:**  
- `uint8_t r`: Parâmetro de entrada/saída para a operação.
- `uint8_t g`: Parâmetro de entrada/saída para a operação.
- `uint8_t b`: Parâmetro de entrada/saída para a operação.
- `int selected_channel`: Parâmetro de entrada/saída para a operação.
- `bool enabled`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_set_brightness()`

```c
void oled_display_set_brightness(uint8_t level);
```

**Descrição Técnica:**  
@brief Ajusta o nível de brilho/contraste do display SSD1306. @param[in] level Contraste de 0 (mínimo legível) a 255 (brilho máximo).

**Parâmetros:**  
- `uint8_t level`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_set_power_save()`

```c
void oled_display_set_power_save(bool enable);
```

**Descrição Técnica:**  
@brief Liga ou desliga o modo de economia de energia do controlador SSD1306. @param[in] enable true para colocar o display em sleep (apagar matriz); false para religar.

**Parâmetros:**  
- `bool enable`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_tela()`

```c
void oled_display_show_tela(int cursor, uint8_t brightness, int timeout_idx);
```

**Descrição Técnica:**  
@brief Exibe o menu de opções da tela (ajuste de brilho e temporizador de repouso). @param[in] cursor      Índice do parâmetro em foco. @param[in] brightness  Nível de brilho configurado. @param[in] timeout_idx Índice da opção de timeout de tela.

**Parâmetros:**  
- `int cursor`: Parâmetro de entrada/saída para a operação.
- `uint8_t brightness`: Parâmetro de entrada/saída para a operação.
- `int timeout_idx`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_top_screen()`

```c
void oled_display_show_top_screen(int volume, const void *eq_cfg, int eq_focus, float voltage, int percentage, int time_left_mins, int cursor);
```

**Descrição Técnica:**  
@brief Renderiza a tela superior de atalhos rápidos (Top Screen). Disposta verticalmente com Bateria no topo, Equalizador no meio e Volume na base. @param[in] volume          Volume percentual atual. @param[in] eq_cfg          Ponteiro para a configuração do equalizador. @param[in] eq_focus        Preset de equalização selecionado. @param[in] voltage         Tensão da bateria em Volts. @param[in] percentage      Porcentagem da bateria. @param[in] time_left_mins  Estimativa de autonomia restante em minutos. @param[in] cursor          Cursor vertical (0=Volume base, 1=EQ meio, 2=Bateria topo).

**Parâmetros:**  
- `int volume`: Parâmetro de entrada/saída para a operação.
- `const void *eq_cfg`: Parâmetro de entrada/saída para a operação.
- `int eq_focus`: Parâmetro de entrada/saída para a operação.
- `float voltage`: Parâmetro de entrada/saída para a operação.
- `int percentage`: Parâmetro de entrada/saída para a operação.
- `int time_left_mins`: Parâmetro de entrada/saída para a operação.
- `int cursor`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_usb_prompt()`

```c
void oled_display_show_usb_prompt(int cursor);
```

**Descrição Técnica:**  
@brief Exibe o diálogo de seleção de perfil USB ao conectar o cabo de dados. @param[in] cursor Opção selecionada (0=Flash CDC, 1=DAC Áudio, 2=Armazenamento MSC, 3=Cancelar).

**Parâmetros:**  
- `int cursor`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_usb_msc()`

```c
void oled_display_show_usb_msc(void);
```

**Descrição Técnica:**  
@brief Exibe status do modo USB Mass Storage (Cartão MicroSD montado no PC).

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_usb_dac()`

```c
void oled_display_show_usb_dac(void);
```

**Descrição Técnica:**  
@brief Exibe interface visual completa do modo USB DAC UAC2 24-bit. Contém badges de resolução, indicador de streaming reativo e barra de volume.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_sd_error()`

```c
void oled_display_show_sd_error(void);
```

**Descrição Técnica:**  
@brief Exibe tela de alerta de ausência ou falha de montagem do cartão MicroSD.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_loading()`

```c
void oled_display_show_loading(void);
```

**Descrição Técnica:**  
@brief Exibe indicador animado de leitura e indexação da biblioteca de músicas.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_eq_preset_list()`

```c
void oled_display_show_eq_preset_list(int cursor, int active_preset, bool eq_enabled, const void *eq_cfg);
```

**Descrição Técnica:**  
@brief Exibe a lista de seleção rápida de perfis predefinidos de EQ (Presets). @param[in] cursor        Índice selecionado pelo joystick. @param[in] active_preset Índice do preset atualmente ativo. @param[in] eq_enabled    Estado ligado/desligado do equalizador. @param[in] eq_cfg        Ponteiro para a estrutura de presets.

**Parâmetros:**  
- `int cursor`: Parâmetro de entrada/saída para a operação.
- `int active_preset`: Parâmetro de entrada/saída para a operação.
- `bool eq_enabled`: Parâmetro de entrada/saída para a operação.
- `const void *eq_cfg`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_keyboard()`

```c
void oled_display_show_keyboard(const char *text, int cursor, int grid_x, int grid_y, int page, bool is_confirming);
```

**Descrição Técnica:**  
@brief Renderiza teclado virtual alfanumérico em tela para digitação de senhas. @param[in] text          Texto já digitado. @param[in] cursor        Posição do cursor no texto. @param[in] grid_x        Posição horizontal na grade de teclas. @param[in] grid_y        Posição vertical na grade de teclas. @param[in] page          Página de caracteres (maiúsculas, minúsculas, números/símbolos). @param[in] is_confirming true se o botão de confirmação estiver em foco.

**Parâmetros:**  
- `const char *text`: Parâmetro de entrada/saída para a operação.
- `int cursor`: Parâmetro de entrada/saída para a operação.
- `int grid_x`: Parâmetro de entrada/saída para a operação.
- `int grid_y`: Parâmetro de entrada/saída para a operação.
- `int page`: Parâmetro de entrada/saída para a operação.
- `bool is_confirming`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_unsupported()`

```c
void oled_display_show_unsupported(const char *filename);
```

**Descrição Técnica:**  
@brief Exibe notificação de arquivo com formato ou compressão não suportada. @param[in] filename Nome do arquivo incompatível.

**Parâmetros:**  
- `const char *filename`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_sort_mode()`

```c
void oled_display_show_sort_mode(int mode);
```

**Descrição Técnica:**  
@brief Exibe tela de configuração do modo de ordenação de músicas (Nome A-Z vs Data mtime). @param[in] mode Modo selecionado (0=Nome, 1=Data).

**Parâmetros:**  
- `int mode`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_deepsleep_cfg()`

```c
void oled_display_show_deepsleep_cfg(int current_idx);
```

**Descrição Técnica:**  
@brief Exibe menu de configuração do temporizador de suspensão profunda (Deep Sleep). @param[in] current_idx Opção selecionada (Desligado, 1 min, 5 min, 15 min, atalho [>]).

**Parâmetros:**  
- `int current_idx`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_ota_progress()`

```c
void oled_display_show_ota_progress(int percent, const char *status_msg);
```

**Descrição Técnica:**  
@brief Renderiza barra de progresso proporcional durante a gravação de firmware OTA. @param[in] percent    Progresso de gravação (0 a 100%). @param[in] status_msg Mensagem de status ou alerta de não desligar o aparelho.

**Parâmetros:**  
- `int percent`: Parâmetro de entrada/saída para a operação.
- `const char *status_msg`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_podcast_sync()`

```c
void oled_display_show_podcast_sync(const char *program, const char *title, int cur_idx, int total_idx, int percent, float speed_kbs, const char *status_msg);
```

**Descrição Técnica:**  
@brief Exibe tela de status de sincronização automática de episódios de podcasts. @param[in] program    Nome do canal ou podcast. @param[in] title      Título do episódio sendo baixado. @param[in] cur_idx    Índice do episódio atual no lote. @param[in] total_idx  Quantidade total de episódios a sincronizar. @param[in] percent    Progresso percentual do download. @param[in] speed_kbs  Velocidade de transferência em KB/s. @param[in] status_msg Mensagem contextual de status.

**Parâmetros:**  
- `const char *program`: Parâmetro de entrada/saída para a operação.
- `const char *title`: Parâmetro de entrada/saída para a operação.
- `int cur_idx`: Parâmetro de entrada/saída para a operação.
- `int total_idx`: Parâmetro de entrada/saída para a operação.
- `int percent`: Parâmetro de entrada/saída para a operação.
- `float speed_kbs`: Parâmetro de entrada/saída para a operação.
- `const char *status_msg`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_wifi_qr()`

```c
void oled_display_show_wifi_qr(const char *ssid, const char *pass, const char *tag_label, int cur_idx, int total_idx);
```

**Descrição Técnica:**  
@brief Renderiza matriz gráfica de QR Code para conexão direta ao Wi-Fi ou web manager. @param[in] ssid      Nome da rede Wi-Fi / SSID. @param[in] pass      Senha da rede Wi-Fi (ou vazio para redes abertas). @param[in] tag_label Rótulo exibido no cabeçalho da tela. @param[in] cur_idx   Índice da rede exibida no carrossel. @param[in] total_idx Total de redes disponíveis para alternância.

**Parâmetros:**  
- `const char *ssid`: Parâmetro de entrada/saída para a operação.
- `const char *pass`: Parâmetro de entrada/saída para a operação.
- `const char *tag_label`: Parâmetro de entrada/saída para a operação.
- `int cur_idx`: Parâmetro de entrada/saída para a operação.
- `int total_idx`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `oled_display_show_clock()`

```c
void oled_display_show_clock(const rtc_clock_info_t *info);
```

**Descrição Técnica:**  
@brief Exibe a tela de relógio em tempo real com telemetria direta do RTC DS3231. Apresenta horas, minutos, segundos com resolução de frações/milissegundos, data completa, dia da semana e temperatura interna do TCXO com precisão nativa de 0,25 °C. @param[in] info Ponteiro para a estrutura de telemetria do RTC (`rtc_clock_info_t`).

**Parâmetros:**  
- `const rtc_clock_info_t *info`: Estrutura com os campos de tempo, milissegundos, data e temperatura.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

## 📁 Interface - Gerador de QR Code

- **Cabeçalho:** [`components/oled_display/include/qrcode.h`](components/oled_display/include/qrcode.h)
- **Implementação:** [`components/oled_display/qrcode.c`](components/oled_display/qrcode.c)
- **Total de Funções Catalogadas:** 4

### - [x] `qrcode_getBufferSize()`

```c
uint16_t qrcode_getBufferSize(uint8_t version);
```

**Descrição Técnica:**  
@brief Calcula a quantidade necessária de bytes para o buffer de módulos de uma dada versão. @param[in] version Versão do QR Code (1 a 40). @return Número de bytes necessários para alocação do array `modules`.

**Parâmetros:**  
- `uint8_t version`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `uint16_t`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `qrcode_initText()`

```c
int8_t qrcode_initText(QRCode *qrcode, uint8_t *modules, uint8_t version, uint8_t ecc, const char *data);
```

**Descrição Técnica:**  
@brief Inicializa e codifica uma string de texto/URL em uma matriz QR Code. @param[out] qrcode  Ponteiro para a estrutura `QRCode` a ser preenchida. @param[in]  modules Buffer de bytes previamente alocado com tamanho `qrcode_getBufferSize(version)`. @param[in]  version Versão do código (1 a 40). @param[in]  ecc     Nível de correção de erro (`ECC_LOW`, `ECC_MEDIUM`, `ECC_QUARTILE`, `ECC_HIGH`). @param[in]  data    String null-terminated a ser codificada (ex: "http://mps3.local" ou dados Wi-Fi). @return 0 em caso de sucesso; < 0 se o texto exceder a capacidade da versão especificada.

**Parâmetros:**  
- `QRCode *qrcode`: Parâmetro de entrada/saída para a operação.
- `uint8_t *modules`: Parâmetro de entrada/saída para a operação.
- `uint8_t version`: Parâmetro de entrada/saída para a operação.
- `uint8_t ecc`: Parâmetro de entrada/saída para a operação.
- `const char *data`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `int8_t`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `qrcode_initBytes()`

```c
int8_t qrcode_initBytes(QRCode *qrcode, uint8_t *modules, uint8_t version, uint8_t ecc, uint8_t *data, uint16_t length);
```

**Descrição Técnica:**  
@brief Inicializa e codifica um payload binário arbitrário em uma matriz QR Code. @param[out] qrcode  Ponteiro para a estrutura `QRCode` de saída. @param[in]  modules Buffer de bytes alocado para os módulos. @param[in]  version Versão do QR Code (1 a 40). @param[in]  ecc     Nível de correção de erro. @param[in]  data    Ponteiro para os bytes a serem codificados. @param[in]  length  Quantidade de bytes do payload. @return 0 em caso de sucesso; < 0 se o payload exceder a capacidade da versão.

**Parâmetros:**  
- `QRCode *qrcode`: Parâmetro de entrada/saída para a operação.
- `uint8_t *modules`: Parâmetro de entrada/saída para a operação.
- `uint8_t version`: Parâmetro de entrada/saída para a operação.
- `uint8_t ecc`: Parâmetro de entrada/saída para a operação.
- `uint8_t *data`: Parâmetro de entrada/saída para a operação.
- `uint16_t length`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `int8_t`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `qrcode_getModule()`

```c
bool qrcode_getModule(QRCode *qrcode, uint8_t x, uint8_t y);
```

**Descrição Técnica:**  
@brief Consulta o valor de um módulo (pixel) específico na matriz do QR Code. @param[in] qrcode Ponteiro para a estrutura `QRCode` inicializada. @param[in] x      Coordenada horizontal do módulo (0 a size-1). @param[in] y      Coordenada vertical do módulo (0 a size-1). @return true se o pixel/módulo estiver aceso (preto); false se apagado (branco).

**Parâmetros:**  
- `QRCode *qrcode`: Parâmetro de entrada/saída para a operação.
- `uint8_t x`: Parâmetro de entrada/saída para a operação.
- `uint8_t y`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

## 📁 Interface - Camada HAL I2C U8g2

- **Cabeçalho:** [`components/oled_display/include/u8g2_hal.h`](components/oled_display/include/u8g2_hal.h)
- **Implementação:** [`components/oled_display/u8g2_hal.c`](components/oled_display/u8g2_hal.c)
- **Total de Funções Catalogadas:** 5

### - [x] `u8g2_hal_i2c_init()`

```c
esp_err_t u8g2_hal_i2c_init(int sda_gpio, int scl_gpio, uint8_t i2c_addr_7bit);
```

**Descrição Técnica:**  
@brief Inicializa o barramento I2C mestre e registra o dispositivo SSD1306. Deve ser invocada antes de qualquer chamada a rotinas de setup do U8g2. @param[in] sda_gpio        Número do pino GPIO para SDA (ex: 10). @param[in] scl_gpio        Número do pino GPIO para SCL (ex: 9). @param[in] i2c_addr_7bit   Endereço I2C de 7 bits do display (geralmente 0x3C). @return - ESP_OK: Barramento e dispositivo I2C inicializados com sucesso. - ESP_FAIL: Falha ao criar o barramento ou registrar o dispositivo.

**Parâmetros:**  
- `int sda_gpio`: Parâmetro de entrada/saída para a operação.
- `int scl_gpio`: Parâmetro de entrada/saída para a operação.
- `uint8_t i2c_addr_7bit`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `esp_err_t`: `ESP_OK` em caso de sucesso; código de erro `ESP_ERR_*` em caso de falha.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `u8g2_hal_get_bus_handle()`

```c
i2c_master_bus_handle_t u8g2_hal_get_bus_handle(void);
```

**Descrição Técnica:**  
@brief Retorna o descritor de barramento mestre I2C criado pelo HAL. Permite que outros drivers (como o RTC DS3231) compartilhem o mesmo barramento físico sem colisões e com proteção por mutex. @return Handle do tipo `i2c_master_bus_handle_t`.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `i2c_master_bus_handle_t`: Ponteiro, descritor ou handle de recurso gerenciado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `u8g2_hal_i2c_probe()`

```c
esp_err_t u8g2_hal_i2c_probe(uint16_t addr, int timeout_ms);
```

**Descrição Técnica:**  
@brief Executa sondagem rápida (probe) de um endereço I2C no barramento ativo. @param[in] addr       Endereço I2C de 7 bits a testar (ex: 0x68 para DS3231). @param[in] timeout_ms Tempo limite de espera em milissegundos. @return ESP_OK se o periférico respondeu com ACK.

**Parâmetros:**  
- `uint16_t addr`: Parâmetro de entrada/saída para a operação.
- `int timeout_ms`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `esp_err_t`: `ESP_OK` em caso de sucesso; código de erro `ESP_ERR_*` em caso de falha.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `u8g2_hal_byte_cb()`

```c
uint8_t u8g2_hal_byte_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr);
```

**Descrição Técnica:**  
@brief Callback de transmissão de bytes exigido pela máquina de estados do U8x8. @param[in,out] u8x8    Instância da estrutura U8x8. @param[in]     msg     Mensagem de controle (envio de dados, comandos, start, stop). @param[in]     arg_int Número de bytes ou parâmetro da mensagem. @param[in]     arg_ptr Ponteiro para o buffer de dados transmitido. @return Código de status retornado ao U8g2 (1 para sucesso).

**Parâmetros:**  
- `u8x8_t *u8x8`: Parâmetro de entrada/saída para a operação.
- `uint8_t msg`: Parâmetro de entrada/saída para a operação.
- `uint8_t arg_int`: Parâmetro de entrada/saída para a operação.
- `void *arg_ptr`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `uint8_t`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `u8g2_hal_gpio_and_delay_cb()`

```c
uint8_t u8g2_hal_gpio_and_delay_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr);
```

**Descrição Técnica:**  
@brief Callback de controle de GPIO e temporização exigido pelo U8x8. @param[in,out] u8x8    Instância da estrutura U8x8. @param[in]     msg     Mensagem de temporização (delays em microssegundos ou milissegundos). @param[in]     arg_int Quantidade de tempo ou nível lógico. @param[in]     arg_ptr Ponteiro auxiliar. @return 1 em caso de sucesso.

**Parâmetros:**  
- `u8x8_t *u8x8`: Parâmetro de entrada/saída para a operação.
- `uint8_t msg`: Parâmetro de entrada/saída para a operação.
- `uint8_t arg_int`: Parâmetro de entrada/saída para a operação.
- `void *arg_ptr`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `uint8_t`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`display_task`, prioridade 3). Sincronizado com mutex de barramento I2C.
- **Hardware Envolvido:** Barramento I2C Mestre (SDA GPIO 10, SCL GPIO 9) operando a 400 kHz conectado ao display SSD1306 (128x64 pixels).

- [x] *Função catalogada, documentada e verificada.*

---

## 📁 Podcast - Sincronizador Automático

- **Cabeçalho:** [`components/podcast_sync/include/podcast_sync.h`](components/podcast_sync/include/podcast_sync.h)
- **Implementação:** [`components/podcast_sync/podcast_sync.c`](components/podcast_sync/podcast_sync.c)
- **Total de Funções Catalogadas:** 7

### - [x] `podcast_sync_init()`

```c
esp_err_t podcast_sync_init(void);
```

**Descrição Técnica:**  
@brief Inicializa o componente de sincronização de podcasts e lê a configuração da NVS. @return ESP_OK em caso de sucesso.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `esp_err_t`: `ESP_OK` em caso de sucesso; código de erro `ESP_ERR_*` em caso de falha.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `podcast_sync_start()`

```c
esp_err_t podcast_sync_start(void);
```

**Descrição Técnica:**  
@brief Dispara a tarefa de sincronização em segundo plano (`podcast_sync_task`). Conecta ao Wi-Fi se necessário, consulta o catálogo do servidor e baixa novos episódios. @return ESP_OK se a tarefa foi disparada; ESP_ERR_INVALID_STATE se já ocupada.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `esp_err_t`: `ESP_OK` em caso de sucesso; código de erro `ESP_ERR_*` em caso de falha.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `podcast_sync_cancel()`

```c
void podcast_sync_cancel(void);
```

**Descrição Técnica:**  
@brief Solicita o cancelamento imediato da sincronização ativa.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `podcast_sync_is_busy()`

```c
bool podcast_sync_is_busy(void);
```

**Descrição Técnica:**  
@brief Informa se o subsistema de podcasts está ativamente ocupado no momento. @return true se a sincronização estiver em execução; false se ociosa.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `podcast_sync_get_progress()`

```c
void podcast_sync_get_progress(podcast_sync_progress_t *out);
```

**Descrição Técnica:**  
@brief Obtém o estado atual e as métricas de progresso para exibição na tela OLED. @param[out] out Ponteiro para a estrutura `podcast_sync_progress_t`.

**Parâmetros:**  
- `podcast_sync_progress_t *out`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `podcast_sync_set_server_url()`

```c
void podcast_sync_set_server_url(const char *url);
```

**Descrição Técnica:**  
@brief Configura e persiste na NVS o endereço URL base do servidor de podcasts. @param[in] url URL do servidor (ex: "http://192.168.1.100:8088").

**Parâmetros:**  
- `const char *url`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `podcast_sync_get_server_url()`

```c
void podcast_sync_get_server_url(char *out_url, size_t max_len);
```

**Descrição Técnica:**  
@brief Obtém o endereço URL do servidor de podcasts atualmente configurado. @param[out] out_url Buffer de destino para a string. @param[in]  max_len Tamanho máximo do buffer de saída.

**Parâmetros:**  
- `char *out_url`: Parâmetro de entrada/saída para a operação.
- `size_t max_len`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

## 📁 Armazenamento - Cartão MicroSD (SDMMC 4-Bit)

- **Cabeçalho:** [`components/sd_card/include/sd_card.h`](components/sd_card/include/sd_card.h)
- **Implementação:** [`components/sd_card/sd_card.c`](components/sd_card/sd_card.c)
- **Total de Funções Catalogadas:** 2

### - [x] `sd_card_init()`

```c
esp_err_t sd_card_init(void);
```

**Descrição Técnica:**  
@brief Inicializa o barramento SDMMC de 4 vias a 40 MHz e monta a partição FatFS. @return - ESP_OK: Cartão detectado, inicializado e montado em `/sdcard`. - ESP_ERR_TIMEOUT: Timeout na comunicação com o cartão. - ESP_FAIL: Falha ao montar o sistema de arquivos FatFS.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `esp_err_t`: `ESP_OK` em caso de sucesso; código de erro `ESP_ERR_*` em caso de falha.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `sd_card_deinit()`

```c
void sd_card_deinit(void);
```

**Descrição Técnica:**  
@brief Desmonta o sistema de arquivos e desativa o host SDMMC. Libera os pinos e o slot do cartão para permitir manipulação externa ou modo USB MSC.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

## 📁 Entradas - Joystick 5 Vias & Gestos

- **Cabeçalho:** [`components/touch_input/include/touch_input.h`](components/touch_input/include/touch_input.h)
- **Implementação:** [`components/touch_input/touch_input.c`](components/touch_input/touch_input.c)
- **Total de Funções Catalogadas:** 29

### - [x] `touch_input_set_display_task_handle()`

```c
void touch_input_set_display_task_handle(TaskHandle_t handle);
```

**Descrição Técnica:**  
@brief Registra o handle da tarefa de renderização do display (`display_task`). Permite que a tarefa de entrada notifique diretamente a tarefa gráfica quando ocorrer qualquer evento de toque para despertar imediato do repouso. @param[in] handle Handle da tarefa FreeRTOS do display.

**Parâmetros:**  
- `TaskHandle_t handle`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`touch_task`, prioridade 5). Resposta instantânea de controle sem latência.
- **Hardware Envolvido:** Pinos de GPIO do Joystick de 5 direções (UP GPIO 2, LEFT GPIO 39, DOWN GPIO 41, RIGHT GPIO 42, CENTER GPIO 40).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `touch_input_wake_display()`

```c
void touch_input_wake_display(void);
```

**Descrição Técnica:**  
@brief Dispara notificação FreeRTOS para acordar a `display_task` imediatamente.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`touch_task`, prioridade 5). Resposta instantânea de controle sem latência.
- **Hardware Envolvido:** Pinos de GPIO do Joystick de 5 direções (UP GPIO 2, LEFT GPIO 39, DOWN GPIO 41, RIGHT GPIO 42, CENTER GPIO 40).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `touch_input_start()`

```c
esp_err_t touch_input_start(void);
```

**Descrição Técnica:**  
@brief Inicializa os pinos de GPIO do joystick e inicia a tarefa de amostragem (`touch_task`). Configura os resistores de pull-up internos e cria a tarefa no Core 0 com prioridade 5. Se houver sessão anterior válida na NVS, inicia diretamente no modo `UI_MODE_PLAYING`. @return ESP_OK se a tarefa foi criada com sucesso.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `esp_err_t`: `ESP_OK` em caso de sucesso; código de erro `ESP_ERR_*` em caso de falha.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`touch_task`, prioridade 5). Resposta instantânea de controle sem latência.
- **Hardware Envolvido:** Pinos de GPIO do Joystick de 5 direções (UP GPIO 2, LEFT GPIO 39, DOWN GPIO 41, RIGHT GPIO 42, CENTER GPIO 40).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `touch_input_set_usb_prompt()`

```c
void touch_input_set_usb_prompt(void);
```

**Descrição Técnica:**  
@brief Exibe o diálogo de seleção de modo USB (Flash CDC, DAC, Armazenamento ou Cancelar). Invocada pelo gerenciador USB (`usb_manager`) ao detectar inserção de cabo VBUS.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`touch_task`, prioridade 5). Resposta instantânea de controle sem latência.
- **Hardware Envolvido:** Pinos de GPIO do Joystick de 5 direções (UP GPIO 2, LEFT GPIO 39, DOWN GPIO 41, RIGHT GPIO 42, CENTER GPIO 40).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `touch_input_cancel_usb()`

```c
void touch_input_cancel_usb(void);
```

**Descrição Técnica:**  
@brief Cancela o prompt USB e retorna o usuário à tela de reprodução.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`touch_task`, prioridade 5). Resposta instantânea de controle sem latência.
- **Hardware Envolvido:** Pinos de GPIO do Joystick de 5 direções (UP GPIO 2, LEFT GPIO 39, DOWN GPIO 41, RIGHT GPIO 42, CENTER GPIO 40).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `touch_input_get_last_activity_ms()`

```c
uint32_t touch_input_get_last_activity_ms(void);
```

**Descrição Técnica:**  
@brief Retorna o timestamp em milissegundos da última ação detectada no joystick. Utilizado pelos algoritmos de inatividade para temporização de suspensão da tela. @return Milissegundos decorridos desde o boot baseados em `xTaskGetTickCount()`.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `uint32_t`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`touch_task`, prioridade 5). Resposta instantânea de controle sem latência.
- **Hardware Envolvido:** Pinos de GPIO do Joystick de 5 direções (UP GPIO 2, LEFT GPIO 39, DOWN GPIO 41, RIGHT GPIO 42, CENTER GPIO 40).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `touch_input_get_mode()`

```c
ui_mode_t touch_input_get_mode(void);
```

**Descrição Técnica:**  
@brief Retorna o modo de interface gráfica atualmente ativo. @return Enum `ui_mode_t`.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `ui_mode_t`: Ponteiro, descritor ou handle de recurso gerenciado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`touch_task`, prioridade 5). Resposta instantânea de controle sem latência.
- **Hardware Envolvido:** Pinos de GPIO do Joystick de 5 direções (UP GPIO 2, LEFT GPIO 39, DOWN GPIO 41, RIGHT GPIO 42, CENTER GPIO 40).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `touch_input_get_list_cursor()`

```c
int touch_input_get_list_cursor(void);
```

**Descrição Técnica:**  
@brief Retorna a posição do cursor na lista de navegação de arquivos. @return Índice selecionado.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `int`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`touch_task`, prioridade 5). Resposta instantânea de controle sem latência.
- **Hardware Envolvido:** Pinos de GPIO do Joystick de 5 direções (UP GPIO 2, LEFT GPIO 39, DOWN GPIO 41, RIGHT GPIO 42, CENTER GPIO 40).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `touch_input_is_locked()`

```c
bool touch_input_is_locked(void);
```

**Descrição Técnica:**  
@brief Informa se as entradas do joystick e a tela estão bloqueadas. Pressionar e segurar o botão central por mais de 700 ms alterna o bloqueio. @return true se o teclado físico estiver bloqueado; false caso contrário.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`touch_task`, prioridade 5). Resposta instantânea de controle sem latência.
- **Hardware Envolvido:** Pinos de GPIO do Joystick de 5 direções (UP GPIO 2, LEFT GPIO 39, DOWN GPIO 41, RIGHT GPIO 42, CENTER GPIO 40).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `touch_input_is_powered_off()`

```c
bool touch_input_is_powered_off(void);
```

**Descrição Técnica:**  
@brief Informa se o sistema recebeu comando de desligamento via interface. @return true se o sistema estiver em desligamento; false caso contrário.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`touch_task`, prioridade 5). Resposta instantânea de controle sem latência.
- **Hardware Envolvido:** Pinos de GPIO do Joystick de 5 direções (UP GPIO 2, LEFT GPIO 39, DOWN GPIO 41, RIGHT GPIO 42, CENTER GPIO 40).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `touch_input_is_in_player_browser()`

```c
bool touch_input_is_in_player_browser(void);
```

**Descrição Técnica:**  
@brief Informa se o usuário está navegando na lista de arquivos do player de áudio. @return true se o modo ativo for `UI_MODE_LIST`.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`touch_task`, prioridade 5). Resposta instantânea de controle sem latência.
- **Hardware Envolvido:** Pinos de GPIO do Joystick de 5 direções (UP GPIO 2, LEFT GPIO 39, DOWN GPIO 41, RIGHT GPIO 42, CENTER GPIO 40).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `touch_input_register_player_entry()`

```c
void touch_input_register_player_entry(void);
```

**Descrição Técnica:**  
@brief Registra a entrada "Player" no menu carrossel principal.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`touch_task`, prioridade 5). Resposta instantânea de controle sem latência.
- **Hardware Envolvido:** Pinos de GPIO do Joystick de 5 direções (UP GPIO 2, LEFT GPIO 39, DOWN GPIO 41, RIGHT GPIO 42, CENTER GPIO 40).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `touch_input_register_conf_entry()`

```c
void touch_input_register_conf_entry(void);
```

**Descrição Técnica:**  
@brief Registra a entrada "Conf" (Configurações) no menu carrossel principal.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`touch_task`, prioridade 5). Resposta instantânea de controle sem latência.
- **Hardware Envolvido:** Pinos de GPIO do Joystick de 5 direções (UP GPIO 2, LEFT GPIO 39, DOWN GPIO 41, RIGHT GPIO 42, CENTER GPIO 40).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `touch_input_register_usb_entry()`

```c
void touch_input_register_usb_entry(void);
```

**Descrição Técnica:**  
@brief Registra a entrada "USB" no menu carrossel principal.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`touch_task`, prioridade 5). Resposta instantânea de controle sem latência.
- **Hardware Envolvido:** Pinos de GPIO do Joystick de 5 direções (UP GPIO 2, LEFT GPIO 39, DOWN GPIO 41, RIGHT GPIO 42, CENTER GPIO 40).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `touch_input_register_game_entry()`

```c
void touch_input_register_game_entry(void);
```

**Descrição Técnica:**  
@brief Registra o jogo "Game of Life" no menu carrossel principal.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`touch_task`, prioridade 5). Resposta instantânea de controle sem latência.
- **Hardware Envolvido:** Pinos de GPIO do Joystick de 5 direções (UP GPIO 2, LEFT GPIO 39, DOWN GPIO 41, RIGHT GPIO 42, CENTER GPIO 40).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `touch_input_get_eq_band()`

```c
int touch_input_get_eq_band(void);
```

**Descrição Técnica:**  
@brief Retorna o índice da banda do equalizador selecionada no momento. @return Índice de 0 a 9 (bandas de frequência) ou 10 (ganho geral).

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `int`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`touch_task`, prioridade 5). Resposta instantânea de controle sem latência.
- **Hardware Envolvido:** Pinos de GPIO do Joystick de 5 direções (UP GPIO 2, LEFT GPIO 39, DOWN GPIO 41, RIGHT GPIO 42, CENTER GPIO 40).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `touch_input_get_led_channel()`

```c
int touch_input_get_led_channel(void);
```

**Descrição Técnica:**  
@brief Retorna o canal RGB ativo no menu de configuração do LED (0=R, 1=G, 2=B). @return Canal selecionado.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `int`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`touch_task`, prioridade 5). Resposta instantânea de controle sem latência.
- **Hardware Envolvido:** Pinos de GPIO do Joystick de 5 direções (UP GPIO 2, LEFT GPIO 39, DOWN GPIO 41, RIGHT GPIO 42, CENTER GPIO 40).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `touch_input_get_oled_brightness()`

```c
uint8_t touch_input_get_oled_brightness(void);
```

**Descrição Técnica:**  
@brief Retorna o nível de brilho do display OLED selecionado no menu de tela. @return Nível de contraste (0 a 255).

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `uint8_t`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`touch_task`, prioridade 5). Resposta instantânea de controle sem latência.
- **Hardware Envolvido:** Pinos de GPIO do Joystick de 5 direções (UP GPIO 2, LEFT GPIO 39, DOWN GPIO 41, RIGHT GPIO 42, CENTER GPIO 40).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `touch_input_get_top_cursor()`

```c
int touch_input_get_top_cursor(void);
```

**Descrição Técnica:**  
@brief Retorna a posição do cursor vertical na tela superior (Top Screen). @return 0 = Volume (base), 1 = Equalizador (meio), 2 = Bateria (topo).

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `int`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`touch_task`, prioridade 5). Resposta instantânea de controle sem latência.
- **Hardware Envolvido:** Pinos de GPIO do Joystick de 5 direções (UP GPIO 2, LEFT GPIO 39, DOWN GPIO 41, RIGHT GPIO 42, CENTER GPIO 40).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `touch_input_get_top_eq_focus()`

```c
int touch_input_get_top_eq_focus(void);
```

**Descrição Técnica:**  
@brief Retorna o preset de equalizador em foco na tela superior. @return Índice do preset.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `int`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`touch_task`, prioridade 5). Resposta instantânea de controle sem latência.
- **Hardware Envolvido:** Pinos de GPIO do Joystick de 5 direções (UP GPIO 2, LEFT GPIO 39, DOWN GPIO 41, RIGHT GPIO 42, CENTER GPIO 40).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `touch_input_get_tela_cursor()`

```c
int touch_input_get_tela_cursor(void);
```

**Descrição Técnica:**  
@brief Retorna a opção selecionada no menu de ajustes de tela. @return Índice do parâmetro em foco.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `int`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`touch_task`, prioridade 5). Resposta instantânea de controle sem latência.
- **Hardware Envolvido:** Pinos de GPIO do Joystick de 5 direções (UP GPIO 2, LEFT GPIO 39, DOWN GPIO 41, RIGHT GPIO 42, CENTER GPIO 40).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `touch_input_get_timeout_idx()`

```c
int touch_input_get_timeout_idx(void);
```

**Descrição Técnica:**  
@brief Retorna o índice do tempo de desligamento automático configurado. @return Índice de timeout (0=Nunca, 1=15s, 2=30s, 3=1m, 4=2m).

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `int`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`touch_task`, prioridade 5). Resposta instantânea de controle sem latência.
- **Hardware Envolvido:** Pinos de GPIO do Joystick de 5 direções (UP GPIO 2, LEFT GPIO 39, DOWN GPIO 41, RIGHT GPIO 42, CENTER GPIO 40).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `touch_input_is_sleeping()`

```c
bool touch_input_is_sleeping(void);
```

**Descrição Técnica:**  
@brief Informa se o sistema encontra-se em modo de suspensão de tela (Display Sleep). @return true se a tela estiver apagada por economia de energia; false se ativa.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`touch_task`, prioridade 5). Resposta instantânea de controle sem latência.
- **Hardware Envolvido:** Pinos de GPIO do Joystick de 5 direções (UP GPIO 2, LEFT GPIO 39, DOWN GPIO 41, RIGHT GPIO 42, CENTER GPIO 40).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `touch_input_get_eq_preset_cursor()`

```c
int touch_input_get_eq_preset_cursor(void);
```

**Descrição Técnica:**  
@brief Retorna a posição do cursor no menu de seleção de presets do equalizador. @return Índice selecionado.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `int`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`touch_task`, prioridade 5). Resposta instantânea de controle sem latência.
- **Hardware Envolvido:** Pinos de GPIO do Joystick de 5 direções (UP GPIO 2, LEFT GPIO 39, DOWN GPIO 41, RIGHT GPIO 42, CENTER GPIO 40).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `touch_input_get_wifi_net_cursor()`

```c
int touch_input_get_wifi_net_cursor(void);
```

**Descrição Técnica:**  
@brief Retorna a posição do cursor na lista de redes Wi-Fi salvas. @return Índice da rede selecionada.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `int`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`touch_task`, prioridade 5). Resposta instantânea de controle sem latência.
- **Hardware Envolvido:** Pinos de GPIO do Joystick de 5 direções (UP GPIO 2, LEFT GPIO 39, DOWN GPIO 41, RIGHT GPIO 42, CENTER GPIO 40).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `touch_input_get_deepsleep_idx()`

```c
int touch_input_get_deepsleep_idx(void);
```

**Descrição Técnica:**  
@brief Retorna o índice selecionado no menu de configuração de Deep Sleep. @return Índice da opção (Desligado, 1 min, 5 min, 15 min, atalho [>]).

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `int`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`touch_task`, prioridade 5). Resposta instantânea de controle sem latência.
- **Hardware Envolvido:** Pinos de GPIO do Joystick de 5 direções (UP GPIO 2, LEFT GPIO 39, DOWN GPIO 41, RIGHT GPIO 42, CENTER GPIO 40).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `touch_input_get_deepsleep_ms()`

```c
uint32_t touch_input_get_deepsleep_ms(void);
```

**Descrição Técnica:**  
@brief Converte o índice configurado para o tempo de inatividade em milissegundos. @return Tempo limite em ms (0 se desativado).

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `uint32_t`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`touch_task`, prioridade 5). Resposta instantânea de controle sem latência.
- **Hardware Envolvido:** Pinos de GPIO do Joystick de 5 direções (UP GPIO 2, LEFT GPIO 39, DOWN GPIO 41, RIGHT GPIO 42, CENTER GPIO 40).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `keyboard_start()`

```c
void keyboard_start(const char *initial_text, keyboard_callback_t cb);
```

**Descrição Técnica:**  
@brief Abre o teclado virtual alfanumérico em tela para entrada de texto. @param[in] initial_text Texto inicial ou string vazia. @param[in] cb           Função de callback a ser chamada ao concluir ou cancelar.

**Parâmetros:**  
- `const char *initial_text`: Parâmetro de entrada/saída para a operação.
- `keyboard_callback_t cb`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`touch_task`, prioridade 5). Resposta instantânea de controle sem latência.
- **Hardware Envolvido:** Pinos de GPIO do Joystick de 5 direções (UP GPIO 2, LEFT GPIO 39, DOWN GPIO 41, RIGHT GPIO 42, CENTER GPIO 40).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `touch_input_get_keyboard_state()`

```c
void touch_input_get_keyboard_state(char *text, int *cursor, int *grid_x, int *grid_y, int *page, bool *is_confirming);
```

**Descrição Técnica:**  
@brief Obtém o estado atual dos cursores e texto do teclado virtual. @param[out] text          Buffer que recebe a string em edição. @param[out] cursor        Posição atual do cursor no texto. @param[out] grid_x        Posição horizontal na grade de caracteres. @param[out] grid_y        Posição vertical na grade de caracteres. @param[out] page          Página de símbolos ativa. @param[out] is_confirming true se o botão OK estiver selecionado.

**Parâmetros:**  
- `char *text`: Parâmetro de entrada/saída para a operação.
- `int *cursor`: Parâmetro de entrada/saída para a operação.
- `int *grid_x`: Parâmetro de entrada/saída para a operação.
- `int *grid_y`: Parâmetro de entrada/saída para a operação.
- `int *page`: Parâmetro de entrada/saída para a operação.
- `bool *is_confirming`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 (`touch_task`, prioridade 5). Resposta instantânea de controle sem latência.
- **Hardware Envolvido:** Pinos de GPIO do Joystick de 5 direções (UP GPIO 2, LEFT GPIO 39, DOWN GPIO 41, RIGHT GPIO 42, CENTER GPIO 40).

- [x] *Função catalogada, documentada e verificada.*

---

## 📁 Comunicação - Protocolo Binário UART Inter-MCU

- **Cabeçalho:** [`components/uart_ctrl/include/uart_ctrl.h`](components/uart_ctrl/include/uart_ctrl.h)
- **Implementação:** [`components/uart_ctrl/uart_ctrl.c`](components/uart_ctrl/uart_ctrl.c)
- **Total de Funções Catalogadas:** 3

### - [x] `uart_ctrl_init()`

```c
int uart_ctrl_init(const uart_ctrl_config_t *cfg);
```

**Descrição Técnica:**  
@brief Inicializa a porta UART e cria a tarefa receptora com decodificador de quadros. @param[in] cfg Ponteiro para a estrutura `uart_ctrl_config_t`. @return 0 em caso de sucesso; < 0 em caso de falha.

**Parâmetros:**  
- `const uart_ctrl_config_t *cfg`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `int`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `uart_ctrl_send()`

```c
bool uart_ctrl_send(uart_ctrl_cmd_t cmd, const uint8_t *payload, size_t len);
```

**Descrição Técnica:**  
@brief Empacota e transmite um quadro de comando ou evento com cabeçalho e CRC8. @param[in] cmd     Identificador do comando (`uart_ctrl_cmd_t`). @param[in] payload Dados do pacote (ou NULL se não houver payload). @param[in] len     Tamanho do payload em bytes (0 a 250). @return true se o quadro foi transmitido com sucesso; false caso contrário. @note Thread-safe. Protegido por mutex interno de transmissão.

**Parâmetros:**  
- `uart_ctrl_cmd_t cmd`: Parâmetro de entrada/saída para a operação.
- `const uint8_t *payload`: Parâmetro de entrada/saída para a operação.
- `size_t len`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `uart_ctrl_send_status()`

```c
bool uart_ctrl_send_status(uart_link_state_t state, const uint8_t bt_addr[6], uint32_t sample_rate);
```

**Descrição Técnica:**  
@brief Função utilitária para envio do status do link Bluetooth. @param[in] state       Estado operacional (`uart_link_state_t`). @param[in] bt_addr     Endereço MAC de 6 bytes do fone conectado. @param[in] sample_rate Taxa de amostragem em Hz. @return true se enviado com sucesso.

**Parâmetros:**  
- `uart_link_state_t state`: Parâmetro de entrada/saída para a operação.
- `const uint8_t bt_addr[6]`: Parâmetro de entrada/saída para a operação.
- `uint32_t sample_rate`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** FreeRTOS Dual-Core cooperativo com controle de concorrência e memória interna SRAM/PSRAM.
- **Hardware Envolvido:** Periféricos associados ao subsistema.

- [x] *Função catalogada, documentada e verificada.*

---

## 📁 USB - TinyUSB (CDC, UAC2 DAC & MSC)

- **Cabeçalho:** [`components/usb_manager/include/usb_manager.h`](components/usb_manager/include/usb_manager.h)
- **Implementação:** [`components/usb_manager/usb_manager.c`](components/usb_manager/usb_manager.c)
- **Total de Funções Catalogadas:** 9

### - [x] `usb_manager_init()`

```c
esp_err_t usb_manager_init(void);
```

**Descrição Técnica:**  
@brief Inicializa a pilha TinyUSB Device e configura os descritores USB. @return ESP_OK se o periférico OTG foi configurado com sucesso.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `esp_err_t`: `ESP_OK` em caso de sucesso; código de erro `ESP_ERR_*` em caso de falha.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 gerenciando TinyUSB Device Stack em modos exclusivos (CDC, UAC2 DAC ou MSC).
- **Hardware Envolvido:** Periférico USB OTG Nativo do ESP32-S3 (D- GPIO 19, D+ GPIO 20).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `usb_manager_set_mode()`

```c
void usb_manager_set_mode(usb_mode_t mode);
```

**Descrição Técnica:**  
@brief Aplica e comuta para o perfil USB selecionado pelo usuário. @param[in] mode Perfil operacional desejado (`USB_MODE_MSC` ou `USB_MODE_DAC`).

**Parâmetros:**  
- `usb_mode_t mode`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 gerenciando TinyUSB Device Stack em modos exclusivos (CDC, UAC2 DAC ou MSC).
- **Hardware Envolvido:** Periférico USB OTG Nativo do ESP32-S3 (D- GPIO 19, D+ GPIO 20).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `usb_manager_is_connected()`

```c
bool usb_manager_is_connected(void);
```

**Descrição Técnica:**  
@brief Informa se o cabo USB de dados está conectado e enumerado por um Host. @return true se conectado ao computador; false caso contrário.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 gerenciando TinyUSB Device Stack em modos exclusivos (CDC, UAC2 DAC ou MSC).
- **Hardware Envolvido:** Periférico USB OTG Nativo do ESP32-S3 (D- GPIO 19, D+ GPIO 20).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `usb_manager_send_hid()`

```c
void usb_manager_send_hid(int command);
```

**Descrição Técnica:**  
@brief Transmite comandos de controle de mídia HID Consumer ao computador. @param[in] command Identificador do comando: - 0: Play / Pause - 1: Próxima Faixa (Next Track) - 2: Faixa Anterior (Previous Track) - 3: Aumentar Volume (Volume Up) - 4: Diminuir Volume (Volume Down)

**Parâmetros:**  
- `int command`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 gerenciando TinyUSB Device Stack em modos exclusivos (CDC, UAC2 DAC ou MSC).
- **Hardware Envolvido:** Periférico USB OTG Nativo do ESP32-S3 (D- GPIO 19, D+ GPIO 20).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `usb_manager_enter_bootloader()`

```c
void usb_manager_enter_bootloader(void);
```

**Descrição Técnica:**  
@brief Reinicia a CPU no modo de gravação USB Serial/JTAG (Download Bootloader da ROM).

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 gerenciando TinyUSB Device Stack em modos exclusivos (CDC, UAC2 DAC ou MSC).
- **Hardware Envolvido:** Periférico USB OTG Nativo do ESP32-S3 (D- GPIO 19, D+ GPIO 20).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `usb_manager_get_sample_rate()`

```c
uint32_t usb_manager_get_sample_rate(void);
```

**Descrição Técnica:**  
@brief Retorna a taxa de amostragem negociada com o computador no modo DAC UAC2. @return Frequência em Hz (normalmente 44100 ou 48000 Hz).

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `uint32_t`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 gerenciando TinyUSB Device Stack em modos exclusivos (CDC, UAC2 DAC ou MSC).
- **Hardware Envolvido:** Periférico USB OTG Nativo do ESP32-S3 (D- GPIO 19, D+ GPIO 20).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `usb_manager_get_pkt_count()`

```c
uint32_t usb_manager_get_pkt_count(void);
```

**Descrição Técnica:**  
@brief Retorna o contador total de pacotes de áudio isócronos recebidos do PC. @return Quantidade de pacotes UAC2 processados.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `uint32_t`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 gerenciando TinyUSB Device Stack em modos exclusivos (CDC, UAC2 DAC ou MSC).
- **Hardware Envolvido:** Periférico USB OTG Nativo do ESP32-S3 (D- GPIO 19, D+ GPIO 20).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `usb_manager_get_last_sample()`

```c
int32_t usb_manager_get_last_sample(void);
```

**Descrição Técnica:**  
@brief Retorna a última amostra escalar de áudio recebida para cálculo de VU-meter. @return Amostra PCM em 32 bits.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `int32_t`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 gerenciando TinyUSB Device Stack em modos exclusivos (CDC, UAC2 DAC ou MSC).
- **Hardware Envolvido:** Periférico USB OTG Nativo do ESP32-S3 (D- GPIO 19, D+ GPIO 20).

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `usb_manager_is_streaming()`

```c
bool usb_manager_is_streaming(void);
```

**Descrição Técnica:**  
@brief Informa se o computador está transmitindo áudio ativamente no momento. @return true se houve pacotes de áudio recebidos nos últimos 250 ms; false se o streaming estiver parado.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 gerenciando TinyUSB Device Stack em modos exclusivos (CDC, UAC2 DAC ou MSC).
- **Hardware Envolvido:** Periférico USB OTG Nativo do ESP32-S3 (D- GPIO 19, D+ GPIO 20).

- [x] *Função catalogada, documentada e verificada.*

---

## 📁 Wi-Fi & Web - Servidor Web & OTA

- **Cabeçalho:** [`components/wifi_transfer/include/wifi_transfer.h`](components/wifi_transfer/include/wifi_transfer.h)
- **Implementação:** [`components/wifi_transfer/wifi_transfer.c`](components/wifi_transfer/wifi_transfer.c)
- **Total de Funções Catalogadas:** 18

### - [x] `wifi_transfer_enter()`

```c
esp_err_t wifi_transfer_enter(wifi_transfer_mode_t mode);
```

**Descrição Técnica:**  
@brief Ativa o modo de transferência Wi-Fi e inicializa os serviços de rede. Pausa a reprodução de áudio, inicializa o rádio Wi-Fi no modo escolhido e inicia o servidor HTTPD para recepção de conexões locais (`http://mps3.local`). @param[in] mode Modo de rede desejado (`WIFI_TRANSFER_MODE_STA` ou `WIFI_TRANSFER_MODE_AP`). @return ESP_OK se o rádio foi ligado com sucesso; código de erro caso contrário.

**Parâmetros:**  
- `wifi_transfer_mode_t mode`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `esp_err_t`: `ESP_OK` em caso de sucesso; código de erro `ESP_ERR_*` em caso de falha.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 executando a pilha Wi-Fi do ESP-IDF e servidor HTTPD na porta 80.
- **Hardware Envolvido:** Rádio Wi-Fi 2.4 GHz 802.11 b/g/n e memória Flash SPI de 16MB durante gravações OTA dual-bank.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `wifi_transfer_poll()`

```c
bool wifi_transfer_poll(void);
```

**Descrição Técnica:**  
@brief Processa periodicamente a máquina de estados e eventos do modo Wi-Fi. Deve ser invocada a cada ciclo do loop principal da `display_task`. @return true no ciclo exato em que o modo Wi-Fi foi encerrado e finalizado; false enquanto ativo.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 executando a pilha Wi-Fi do ESP-IDF e servidor HTTPD na porta 80.
- **Hardware Envolvido:** Rádio Wi-Fi 2.4 GHz 802.11 b/g/n e memória Flash SPI de 16MB durante gravações OTA dual-bank.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `wifi_transfer_request_exit()`

```c
void wifi_transfer_request_exit(void);
```

**Descrição Técnica:**  
@brief Solicita o encerramento do modo Wi-Fi e desligamento do rádio. Seguro para ser chamado a partir de qualquer tarefa (ISR/Thread-safe).

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 executando a pilha Wi-Fi do ESP-IDF e servidor HTTPD na porta 80.
- **Hardware Envolvido:** Rádio Wi-Fi 2.4 GHz 802.11 b/g/n e memória Flash SPI de 16MB durante gravações OTA dual-bank.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `wifi_transfer_is_active()`

```c
bool wifi_transfer_is_active(void);
```

**Descrição Técnica:**  
@brief Informa se o subsistema Wi-Fi está ligado e operando no momento. @return true se o rádio Wi-Fi estiver ativo; false se desligado.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 executando a pilha Wi-Fi do ESP-IDF e servidor HTTPD na porta 80.
- **Hardware Envolvido:** Rádio Wi-Fi 2.4 GHz 802.11 b/g/n e memória Flash SPI de 16MB durante gravações OTA dual-bank.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `wifi_transfer_is_ota_busy()`

```c
bool wifi_transfer_is_ota_busy(void);
```

**Descrição Técnica:**  
@brief Informa se há uma atualização de firmware OTA sendo gravada na Flash. @return true se o processo de OTA estiver ocupado; false caso contrário.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 executando a pilha Wi-Fi do ESP-IDF e servidor HTTPD na porta 80.
- **Hardware Envolvido:** Rádio Wi-Fi 2.4 GHz 802.11 b/g/n e memória Flash SPI de 16MB durante gravações OTA dual-bank.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `wifi_transfer_is_transferring()`

```c
bool wifi_transfer_is_transferring(void);
```

**Descrição Técnica:**  
@brief Informa se há upload ou download de arquivos em andamento no servidor web. @return true se houver transferência de arquivo ativa; false caso ocioso.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 executando a pilha Wi-Fi do ESP-IDF e servidor HTTPD na porta 80.
- **Hardware Envolvido:** Rádio Wi-Fi 2.4 GHz 802.11 b/g/n e memória Flash SPI de 16MB durante gravações OTA dual-bank.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `wifi_transfer_has_sta_ip()`

```c
bool wifi_transfer_has_sta_ip(void);
```

**Descrição Técnica:**  
@brief Informa se a interface Station recebeu um endereço IP válido via DHCP. @return true se conectado ao roteador com IP válido; false caso contrário.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 executando a pilha Wi-Fi do ESP-IDF e servidor HTTPD na porta 80.
- **Hardware Envolvido:** Rádio Wi-Fi 2.4 GHz 802.11 b/g/n e memória Flash SPI de 16MB durante gravações OTA dual-bank.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `wifi_transfer_get_gateway_ip()`

```c
bool wifi_transfer_get_gateway_ip(char *out_gw, size_t max_len);
```

**Descrição Técnica:**  
@brief Obtém o endereço IP do gateway da rede local conectada. @param[out] out_gw  Buffer de destino para a string do IP. @param[in]  max_len Tamanho máximo do buffer. @return true se o IP foi copiado com sucesso.

**Parâmetros:**  
- `char *out_gw`: Parâmetro de entrada/saída para a operação.
- `size_t max_len`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 executando a pilha Wi-Fi do ESP-IDF e servidor HTTPD na porta 80.
- **Hardware Envolvido:** Rádio Wi-Fi 2.4 GHz 802.11 b/g/n e memória Flash SPI de 16MB durante gravações OTA dual-bank.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `wifi_transfer_get_ap_credentials()`

```c
void wifi_transfer_get_ap_credentials(char *out_ssid, size_t ssid_len, char *out_pass, size_t pass_len);
```

**Descrição Técnica:**  
@brief Consulta as credenciais do Hotspot SoftAP gerado pelo MPS3. @param[out] out_ssid Buffer que recebe o nome da rede (SSID). @param[in]  ssid_len Tamanho do buffer de SSID. @param[out] out_pass Buffer que recebe a senha da rede. @param[in]  pass_len Tamanho do buffer de senha.

**Parâmetros:**  
- `char *out_ssid`: Parâmetro de entrada/saída para a operação.
- `size_t ssid_len`: Parâmetro de entrada/saída para a operação.
- `char *out_pass`: Parâmetro de entrada/saída para a operação.
- `size_t pass_len`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 executando a pilha Wi-Fi do ESP-IDF e servidor HTTPD na porta 80.
- **Hardware Envolvido:** Rádio Wi-Fi 2.4 GHz 802.11 b/g/n e memória Flash SPI de 16MB durante gravações OTA dual-bank.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `wifi_transfer_get_known_count()`

```c
int wifi_transfer_get_known_count(void);
```

**Descrição Técnica:**  
@brief Retorna a quantidade de redes Wi-Fi conhecidas salvas na NVS. @return Número de redes cadastradas.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `int`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 executando a pilha Wi-Fi do ESP-IDF e servidor HTTPD na porta 80.
- **Hardware Envolvido:** Rádio Wi-Fi 2.4 GHz 802.11 b/g/n e memória Flash SPI de 16MB durante gravações OTA dual-bank.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `wifi_transfer_get_known_network()`

```c
bool wifi_transfer_get_known_network(int idx, char *out_ssid, size_t ssid_len, char *out_pass, size_t pass_len);
```

**Descrição Técnica:**  
@brief Obtém as credenciais de uma rede salva na NVS pelo seu índice. @param[in]  idx      Índice da rede (0 a count-1). @param[out] out_ssid Buffer de saída para o SSID. @param[in]  ssid_len Tamanho do buffer de SSID. @param[out] out_pass Buffer de saída para a senha. @param[in]  pass_len Tamanho do buffer de senha. @return true se os dados foram recuperados com sucesso.

**Parâmetros:**  
- `int idx`: Parâmetro de entrada/saída para a operação.
- `char *out_ssid`: Parâmetro de entrada/saída para a operação.
- `size_t ssid_len`: Parâmetro de entrada/saída para a operação.
- `char *out_pass`: Parâmetro de entrada/saída para a operação.
- `size_t pass_len`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 executando a pilha Wi-Fi do ESP-IDF e servidor HTTPD na porta 80.
- **Hardware Envolvido:** Rádio Wi-Fi 2.4 GHz 802.11 b/g/n e memória Flash SPI de 16MB durante gravações OTA dual-bank.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `wifi_transfer_get_status()`

```c
void wifi_transfer_get_status(char *out, size_t out_len, int *files_received);
```

**Descrição Técnica:**  
@brief Preenche uma string formatada com o status atual do Wi-Fi para exibição no display OLED. @param[out] out            Buffer de saída para a string de status. @param[in]  out_len        Tamanho do buffer de saída. @param[out] files_received Ponteiro onde será gravado o número de arquivos recebidos na sessão.

**Parâmetros:**  
- `char *out`: Parâmetro de entrada/saída para a operação.
- `size_t out_len`: Parâmetro de entrada/saída para a operação.
- `int *files_received`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 executando a pilha Wi-Fi do ESP-IDF e servidor HTTPD na porta 80.
- **Hardware Envolvido:** Rádio Wi-Fi 2.4 GHz 802.11 b/g/n e memória Flash SPI de 16MB durante gravações OTA dual-bank.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `wifi_transfer_get_progress()`

```c
void wifi_transfer_get_progress(wifi_transfer_progress_t *out);
```

**Descrição Técnica:**  
@brief Obtém os dados de progresso da transferência ativa para desenho da barra na UI. @param[out] out Ponteiro para a estrutura `wifi_transfer_progress_t`.

**Parâmetros:**  
- `wifi_transfer_progress_t *out`: Parâmetro de entrada/saída para a operação.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 executando a pilha Wi-Fi do ESP-IDF e servidor HTTPD na porta 80.
- **Hardware Envolvido:** Rádio Wi-Fi 2.4 GHz 802.11 b/g/n e memória Flash SPI de 16MB durante gravações OTA dual-bank.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `wifi_transfer_enter_auto()`

```c
esp_err_t wifi_transfer_enter_auto(void);
```

**Descrição Técnica:**  
@brief Inicia o Wi-Fi com seleção automática inteligente. Tenta conectar nas redes salvas na NVS; se nenhuma estiver ao alcance ou falhar, sobe automaticamente o Hotspot SoftAP com captive portal. @return ESP_OK se o procedimento foi iniciado com sucesso.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `esp_err_t`: `ESP_OK` em caso de sucesso; código de erro `ESP_ERR_*` em caso de falha.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 executando a pilha Wi-Fi do ESP-IDF e servidor HTTPD na porta 80.
- **Hardware Envolvido:** Rádio Wi-Fi 2.4 GHz 802.11 b/g/n e memória Flash SPI de 16MB durante gravações OTA dual-bank.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `wifi_transfer_is_dns_active()`

```c
bool wifi_transfer_is_dns_active(void);
```

**Descrição Técnica:**  
@brief Informa se o servidor DNS captive portal está em execução. @return true se o DNS estiver ativo; false caso contrário.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `bool`: `true` se a condição for satisfeita ou operação bem-sucedida; `false` caso contrário.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 executando a pilha Wi-Fi do ESP-IDF e servidor HTTPD na porta 80.
- **Hardware Envolvido:** Rádio Wi-Fi 2.4 GHz 802.11 b/g/n e memória Flash SPI de 16MB durante gravações OTA dual-bank.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `wifi_transfer_get_mode()`

```c
wifi_transfer_mode_t wifi_transfer_get_mode(void);
```

**Descrição Técnica:**  
@brief Retorna o modo de operação Wi-Fi atual. @return Enum `wifi_transfer_mode_t`.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `wifi_transfer_mode_t`: Ponteiro, descritor ou handle de recurso gerenciado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 executando a pilha Wi-Fi do ESP-IDF e servidor HTTPD na porta 80.
- **Hardware Envolvido:** Rádio Wi-Fi 2.4 GHz 802.11 b/g/n e memória Flash SPI de 16MB durante gravações OTA dual-bank.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `wifi_transfer_get_connected_clients()`

```c
int wifi_transfer_get_connected_clients(void);
```

**Descrição Técnica:**  
@brief Retorna a quantidade de smartphones/computadores conectados ao SoftAP do MPS3. @return Número de clientes associados.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `int`: Valor numérico ou métrica calculada pelo componente.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 executando a pilha Wi-Fi do ESP-IDF e servidor HTTPD na porta 80.
- **Hardware Envolvido:** Rádio Wi-Fi 2.4 GHz 802.11 b/g/n e memória Flash SPI de 16MB durante gravações OTA dual-bank.

- [x] *Função catalogada, documentada e verificada.*

---

### - [x] `wifi_transfer_register_menu_entry()`

```c
void wifi_transfer_register_menu_entry(void);
```

**Descrição Técnica:**  
@brief Registra o item "WiFi" no menu carrossel principal do sistema.

**Parâmetros:**  
- *Nenhum (void)*.

**Retorno:**  
- `void`: Nenhum valor retornado.

**Concorrência e Hardware:**  
- **Núcleo / Tarefa:** Core 0 executando a pilha Wi-Fi do ESP-IDF e servidor HTTPD na porta 80.
- **Hardware Envolvido:** Rádio Wi-Fi 2.4 GHz 802.11 b/g/n e memória Flash SPI de 16MB durante gravações OTA dual-bank.

- [x] *Função catalogada, documentada e verificada.*

---
