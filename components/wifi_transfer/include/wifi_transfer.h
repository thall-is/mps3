#ifndef WIFI_TRANSFER_H
#define WIFI_TRANSFER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_TRANSFER_MODE_STA,   // entra na rede WiFi de casa (CONFIG_WIFI_STA_SSID/PASSWORD)
    WIFI_TRANSFER_MODE_AP,    // o player cria a propria rede (CONFIG_WIFI_AP_SSID/PASSWORD)
    WIFI_TRANSFER_MODE_APSTA, // modo misto: hotspot e estacao simultaneos
} wifi_transfer_mode_t;

// Entra no modo de transferencia por WiFi: pausa a reproducao (mesmo
// hook usado pelo modo USB - audio_player_release_sd_for_usb()), sobe o
// WiFi (estacao OU hotspot, conforme "mode") e, assim que a rede estiver
// pronta, um servidor HTTP com uma pagina de gerenciamento de arquivos
// (http://<ip>/ - navega pastas, envia arquivos/pastas inteiras, apaga
// arquivos/pastas). Chame wifi_transfer_poll() repetidamente depois -
// mesmo padrao do usb_storage_poll().
//
// Retorna ESP_OK se CONSEGUIU INICIAR a tentativa (nao significa que ja'
// esta' conectado - streaming de conexao acontece em wifi_transfer_poll()
// / veja wifi_transfer_get_status()). So' devolve erro se nem conseguiu
// comecar (ex: SSID de estacao nao configurado, falha ao subir o
// radio).
esp_err_t wifi_transfer_enter(wifi_transfer_mode_t mode);

// Chame a cada iteracao do loop de desenho enquanto o modo WiFi estiver
// ativo. Sobe o servidor HTTP assim que a rede estiver pronta e processa
// pedidos de saida (wifi_transfer_request_exit()) - desliga o servidor e
// o radio WiFi, e chama audio_player_reacquire_sd_after_usb() (rescaneia
// o cartao - os arquivos enviados aparecem no menu). Retorna true no
// exato ciclo em que o modo WiFi terminou.
bool wifi_transfer_poll(void);

// Pede pra sair do modo WiFi (ex: toque na barra). Seguro de chamar de
// qualquer task - wifi_transfer_poll() e' quem processa de verdade.
void wifi_transfer_request_exit(void);

bool wifi_transfer_is_active(void);
bool wifi_transfer_is_ota_busy(void);
bool wifi_transfer_is_transferring(void);
bool wifi_transfer_has_sta_ip(void);
bool wifi_transfer_get_gateway_ip(char *out_gw, size_t max_len);

// Consulta credenciais do Hotspot AP gerado pelo MPS3
void wifi_transfer_get_ap_credentials(char *out_ssid, size_t ssid_len, char *out_pass, size_t pass_len);

// Consulta redes Wi-Fi salvas na NVS
int wifi_transfer_get_known_count(void);
bool wifi_transfer_get_known_network(int idx, char *out_ssid, size_t ssid_len, char *out_pass, size_t pass_len);

// Preenche "out" com uma linha de status pronta pra' mostrar na tela:
// "Conectando..." / "192.168.x.x" (IP assim que pronto pra' receber
// upload) / "Rede indisponivel" (falha ao entrar na rede de casa - senha
// errada, fora de alcance, etc.) / "SSID nao configurado". Tambem
// devolve quantos arquivos ja' foram recebidos nesta sessao.
void wifi_transfer_get_status(char *out, size_t out_len, int *files_received);

// Progresso do upload em andamento agora mesmo, pra' desenhar a barra na
// tela do proprio dispositivo (a pagina web calcula o progresso dela
// sozinha, no navegador - isso aqui e' so' pro OLED). in_progress=false
// quando nao ha' nenhum upload rolando neste exato instante (entre
// arquivos de um lote, ou fora do modo WiFi).
typedef struct {
    bool in_progress;
    char filename[64];      // so' o nome do arquivo (sem o caminho de pastas)
    int file_pct;           // 0-100, progresso do arquivo atual
    int overall_pct;        // 0-100, progresso do LOTE inteiro; -1 se so' 1 arquivo (upload avulso)
    int idx;                // indice (1-based) do arquivo atual dentro do lote; so' valido se overall_pct >= 0
    int count;              // total de arquivos do lote; so' valido se overall_pct >= 0
    float kbps;             // velocidade media do arquivo atual, em KB/s
    int eta_sec;            // segundos estimados pra' terminar o arquivo atual; -1 se ainda nao da' pra' estimar
    int total_eta_sec;      // NOVO
} wifi_transfer_progress_t;

void wifi_transfer_get_progress(wifi_transfer_progress_t *out);

esp_err_t wifi_transfer_enter_auto(void);

// Novas funções auxiliares para consulta de estado
bool wifi_transfer_is_dns_active(void);
wifi_transfer_mode_t wifi_transfer_get_mode(void);
int wifi_transfer_get_connected_clients(void);

// Registra o item "WiFi" no menu principal (ver menu.h). Chame uma vez no
// boot, na posicao em que "WiFi" deve aparecer no carrossel (ver a secao
// "Registro do menu principal" em main.c).
void wifi_transfer_register_menu_entry(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_TRANSFER_H
