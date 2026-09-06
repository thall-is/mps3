# esp_idf_stubs

Headers-stub que imitam a API real do ESP-IDF o suficiente pra' compilar
(com `gcc` puro, sem o toolchain Xtensa) os arquivos deste projeto que
usam driver/UART, driver/I2S, driver/GPIO, Bluedroid (`esp_bt*.h`),
FreeRTOS, u8g2 (só no lado mps3), etc.

## O que isso PROVA e o que NÃO prova

**Prova**: que o código compila sem erro de sintaxe, tipo incompatível,
campo de struct inexistente, assinatura de função errada, contagem de
argumento errada, macro que colide com nome de campo (achamos um desses
de verdade — ver `driver/i2s_std.h`, `I2S_CHANNEL_DEFAULT_CONFIG`) e
outras classes de erro que só aparecem tentando compilar de verdade,
não só lendo o código.

**NÃO prova**: que o código funciona. Os stubs não implementam
comportamento real (a maioria das funções só retorna `ESP_OK` sem fazer
nada) — não validam timing, não validam a pilha Bluetooth de verdade,
não rodam em hardware nenhum. "Compila contra o stub" é uma barra bem
mais baixa que "funciona no ESP32 real", mas é uma barra concreta e
real, e pegou pelo menos um bug de verdade antes de qualquer hardware
entrar em cena (ver abaixo).

## De onde veio cada valor

Os campos/enums/assinaturas nos stubs foram tirados **lendo o
código-fonte real do ESP-IDF** (o mesmo commit travado documentado no
README principal,
`08e0d30a74ad0bfd5a34933142b80f45619ee410`) — não "de memória". Onde
um valor não pôde ser confirmado lendo o header real diretamente (ex:
`UART_SCLK_DEFAULT`, que não apareceu numa busca no repo), foi
confirmado via busca web contra exemplos oficiais/documentação atual do
ESP-IDF antes de usar.

## Bugs reais que este processo achou

1. **`bt_link.c` (mps3)**: usava `UART_NUM_1` sem incluir
   `driver/uart.h` — `uart_ctrl.h` é proposital sem depender do
   ESP-IDF, então esse tipo não vinha de lá. Erro de link-time
   silencioso até tentar compilar de verdade. Corrigido.
2. **Este próprio stub** (não o projeto): o primeiro rascunho de
   `I2S_CHANNEL_DEFAULT_CONFIG` tinha um parâmetro de macro chamado
   `role` que colidia com o campo de struct `.role` — o pré-processador
   substituiu `.role` por `.I2S_ROLE_MASTER` (o valor do parâmetro),
   gerando um erro bizarro só entendível olhando a expansão da macro
   (`gcc -E`). Renomeado o parâmetro pra evitar a colisão.

## Como rodar

Ver `tests/compile_check.sh` na raiz do projeto — compila cada arquivo
que depende do ESP-IDF contra esses stubs, um por um, e reporta erros e
contagem de warnings.
