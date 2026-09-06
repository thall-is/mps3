# Integração com o mps3 (placa principal)

Este documento descreve o lado da placa principal (ESP32-S3, projeto
`mps3`) — o `bt_companion` é só metade da história. Os dois firmwares
são **projetos ESP-IDF separados** (chips diferentes, builds
diferentes), ligados fisicamente por I2S (áudio) + UART (controle).

## O que existe do lado do mps3

| Componente | Papel |
|---|---|
| `components/uart_ctrl` | **Cópia exata** de `bt_companion/components/uart_ctrl` — o protocolo é simétrico, o código é literalmente o mesmo dos dois lados. |
| `components/bt_link` | Novo — só existe do lado do mps3. Fala `uart_ctrl`, se registra no menu principal (item "Bluetooth"), desenha a tela de status. |

Mudanças pequenas em componentes que já existiam:
- `components/board_io/i2s_output.c/.h` — um callback opcional
  (`i2s_output_set_rate_change_cb()`) chamado toda vez que a taxa de
  amostragem muda, pra `bt_link` avisar o companheiro sem
  `audio_player.cpp` precisar saber nada sobre Bluetooth.
- `components/menu/menu.h` / `menu.c` — um campo novo em
  `menu_item_t` (`request_action`, opcional — `NULL` não muda nada
  pros itens que já existiam) e a função `menu_request_action_active()`
  que o aciona.
- `components/touch_input/touch_input.c` — um toque curto em
  `JOY_CENTER` (que fica sem uso enquanto um modo de tela cheia está
  ativo) agora chama `menu_request_action_active()`.
- `components/oled_display/oled_display.c` — um `case` novo em
  `oled_display_show_main_menu()` pro ícone do Bluetooth no carrossel
  (texto simples, sem bitmap customizado — ver comentário no código
  sobre por que).
- `components/board_io/include/pinos.h` — 3 pinos novos
  (`PIN_BT_LINK_UART_TX/RX/EN`).

## O fluxo de pareamento, passo a passo

1. Usuário navega até "Bluetooth" no carrossel do menu principal e
   seleciona.
2. `bt_link_on_select()` (mps3) manda `UART_CMD_GET_STATUS` (sincroniza
   com o que o companheiro já sabe — cobre o caso dele já estar
   conectado de antes) e, se o estado voltar `IDLE`, manda também
   `UART_CMD_PAIR_START`.
3. O companheiro (`bt_source_start_pairing()`) fica descobrível/
   conectável e responde com `UART_EVT_STATUS` (`state=PAIRING`).
4. mps3 mostra "Pareando..." na tela (`bt_link_draw_status()`).
5. Usuário parea pelo fone/caixa (procurando "mps3-bt" na lista de
   dispositivos Bluetooth dele — ver `bt_source_init()` no
   companheiro).
6. Companheiro detecta a conexão A2DP + qual codec foi negociado
   (`ESP_A2D_AUDIO_CFG_EVT`) e manda outro `UART_EVT_STATUS`
   (`state=CONNECTED_LDAC` ou `CONNECTED_SBC`).
7. mps3 atualiza a tela: nome do codec + últimos 2 octetos do endereço
   + taxa de amostragem atual.
8. Usuário pode sair da tela (segurar `JOY_LEFT`) e continuar
   navegando/trocando de faixa — o áudio continua saindo pelo
   Bluetooth o tempo todo (ver `bt_link.h` pra a motivação dessa
   escolha de design, diferente do modo WiFi que é exclusivo).
9. A qualquer momento, um toque curto em `JOY_CENTER` dentro da tela
   de status manda `UART_CMD_DISCONNECT` (se conectado) ou
   `UART_CMD_PAIR_START`/`PAIR_STOP` (se ainda não conectado) — ver
   `bt_link_request_action()`.

## Estado da Implementação e Validação

- **Persistência de Pareamento**: Totalmente implementada via NVS (`last_bda`). O sistema auto-reconecta ao último fone pareado ao ligar.
- **Linha de Reset por Hardware**: O pino `PIN_BT_LINK_EN` (GPIO 12 no S3) controla a linha EN/CHIP_PU do ESP32 Companion para reiniciar o chip de rádio de forma limpa caso necessário.
- **Validação em Hardware Real**: Conexão comprovada com fones comerciais via Sony LDAC (24-bit / 96 kHz) com bitrate adaptativo ABR dinâmico (330 a 990 kbps) e SBC de alta qualidade (Bitpool 53). Áudio contínuo e sem interrupções.

## Se o protocolo UART precisar mudar

`uart_ctrl.h`/`uart_ctrl.c` são **arquivos idênticos nos dois
projetos** — não há mecanismo de versionamento entre os dois
firmwares (nenhum handshake de "versão do protocolo"). Se adicionar um
comando novo, mudar o formato de um payload, ou qualquer coisa que
quebre compatibilidade: atualize os dois arquivos juntos, nos dois
repositórios, e grave os dois firmwares na mesma hora. Não há proteção
automática contra os dois lados ficarem dessincronizados (uma placa
com protocolo v1 falando com a outra em v2 provavelmente vai só
descartar quadros por CRC/comando desconhecido, silenciosamente — ver
`uart_ctrl.c`, `default:` do parser).
