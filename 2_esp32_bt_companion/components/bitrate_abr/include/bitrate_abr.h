#ifndef BITRATE_ABR_H
#define BITRATE_ABR_H

// Logica pura (sem NENHUMA dependencia de ESP-IDF) de adaptacao
// automatica de taxa/qualidade - comeca no nivel mais alto e cai pro
// mais baixo quando a conexao Bluetooth nao consegue escoar os
// pacotes, com histerese pra' nao ficar alternando (flapping) a cada
// falha isolada. Feito separado de main.c/bt_source.c exatamente pra'
// poder testar no host com gcc puro (ver test_bitrate_abr_host.c).
//
// Uso tipico (main.c): a cada tentativa de
// bt_source_send_media_packet(), reporta o resultado com
// bitrate_abr_report(); se o retorno indicar mudanca de nivel, chama
// ldac_enc_set_quality() com o nivel novo.
//
// So' esta' ligado ao LDAC hoje (os outros 3 codecs deste projeto sao
// taxa fixa - aptX/aptX HD por definicao do proprio codec, SBC porque
// o wrapper deste projeto nao expoe troca de bitpool em tempo real
// ainda) - mas a logica em si e' generica o bastante pra' qualquer
// escala de niveis ordenada por qualidade decrescente.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Valores em ORDEM DE QUALIDADE DECRESCENTE (0 = melhor) - casa
// diretamente com ldac_enc_quality_t (LDAC_ENC_QUALITY_HIGH=0,
// STANDARD=1, MOBILE=2), entao dá pra' usar o valor retornado aqui
// direto num cast pra' ldac_enc_quality_t sem tabela de conversao.
typedef enum {
    BITRATE_ABR_TIER_HIGH = 0,
    BITRATE_ABR_TIER_STANDARD = 1,
    BITRATE_ABR_TIER_MOBILE = 2,
    BITRATE_ABR_TIER_COUNT
} bitrate_abr_tier_t;

// Quantos envios falhos seguidos disparam uma queda de nivel. Baixo de
// proposito (reage rapido a um link ruim) - assimetrico com o valor de
// subida (ver abaixo), que e' bem mais alto: cair rapido, subir devagar
// e' o padrao usual de ABR (evita alternar qualidade a troco de uma
// falha isolada, mas tambem nao demora reagindo a uma degradacao real).
#define BITRATE_ABR_DOWNGRADE_AFTER_FAILS 10

// Quantos envios OK seguidos, num nivel ja' rebaixado, disparam uma
// tentativa de subir de novo. Bem mais alto que o de queda de
// proposito - so' sobe depois de um periodo longo e estavel, pra'
// evitar ficar oscilando entre dois niveis quando o link esta' na
// fronteira.
#define BITRATE_ABR_UPGRADE_AFTER_OK 500

typedef struct {
    bitrate_abr_tier_t tier;
    int consecutive_fail;
    int consecutive_ok;
} bitrate_abr_state_t;

// Inicializa no nivel indicado (tipicamente BITRATE_ABR_TIER_HIGH - "de
// maior qualidade" - ver motivacao no chat que originou este modulo).
void bitrate_abr_init(bitrate_abr_state_t *st, bitrate_abr_tier_t start_tier);

// Reporta o resultado de UM envio (ok=true se
// bt_source_send_media_packet() retornou ESP_OK, false em qualquer
// outro caso - fila cheia, mtu excedido, etc). Atualiza o estado
// internamente. Retorna true se o nivel mudou nesta chamada (o
// chamador deve entao reconfigurar o encoder pro novo st->tier).
bool bitrate_abr_report(bitrate_abr_state_t *st, bool ok);

#ifdef __cplusplus
}
#endif

#endif // BITRATE_ABR_H
