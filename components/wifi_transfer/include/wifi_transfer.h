#ifndef WIFI_TRANSFER_H
#define WIFI_TRANSFER_H

/**
 * @file wifi_transfer.h
 * @brief Subsistema de Transferência de Músicas via Wi-Fi, Servidor Web e Atualizações OTA
 *
 * Gerencia a pilha de rede Wi-Fi do ESP-IDF em modo SoftAP (Hotspot local) ou Station (STA),
 * o servidor de captive portal DNS, o servidor HTTPD interno na porta 80 servindo a
 * aplicação web de gerenciamento e upload de arquivos, e o endpoint de streaming de firmware
 * Over-The-Air (OTA) com rollback anti-brick.
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Modos de operação da interface Wi-Fi.
 */
typedef enum {
    WIFI_TRANSFER_MODE_STA,   /**< Conecta na rede Wi-Fi local configurada. */
    WIFI_TRANSFER_MODE_AP,    /**< Cria Hotspot próprio (SoftAP) com captive portal. */
    WIFI_TRANSFER_MODE_APSTA, /**< Modo misto: SoftAP e Station ativos simultaneamente. */
} wifi_transfer_mode_t;

/**
 * @brief Ativa o modo de transferência Wi-Fi e inicializa os serviços de rede.
 *
 * Pausa a reprodução de áudio, inicializa o rádio Wi-Fi no modo escolhido e
 * inicia o servidor HTTPD para recepção de conexões locais (`http://mps3.local`).
 *
 * @param[in] mode Modo de rede desejado (`WIFI_TRANSFER_MODE_STA` ou `WIFI_TRANSFER_MODE_AP`).
 *
 * @return ESP_OK se o rádio foi ligado com sucesso; código de erro caso contrário.
 */
esp_err_t wifi_transfer_enter(wifi_transfer_mode_t mode);

/**
 * @brief Processa periodicamente a máquina de estados e eventos do modo Wi-Fi.
 *
 * Deve ser invocada a cada ciclo do loop principal da `display_task`.
 *
 * @return true no ciclo exato em que o modo Wi-Fi foi encerrado e finalizado; false enquanto ativo.
 */
bool wifi_transfer_poll(void);

/**
 * @brief Solicita o encerramento do modo Wi-Fi e desligamento do rádio.
 *
 * Seguro para ser chamado a partir de qualquer tarefa (ISR/Thread-safe).
 */
void wifi_transfer_request_exit(void);

/**
 * @brief Informa se o subsistema Wi-Fi está ligado e operando no momento.
 *
 * @return true se o rádio Wi-Fi estiver ativo; false se desligado.
 */
bool wifi_transfer_is_active(void);

/**
 * @brief Informa se há uma atualização de firmware OTA sendo gravada na Flash.
 *
 * @return true se o processo de OTA estiver ocupado; false caso contrário.
 */
bool wifi_transfer_is_ota_busy(void);

/**
 * @brief Informa se há upload ou download de arquivos em andamento no servidor web.
 *
 * @return true se houver transferência de arquivo ativa; false caso ocioso.
 */
bool wifi_transfer_is_transferring(void);

/**
 * @brief Informa se a interface Station recebeu um endereço IP válido via DHCP.
 *
 * @return true se conectado ao roteador com IP válido; false caso contrário.
 */
bool wifi_transfer_has_sta_ip(void);

/**
 * @brief Obtém o endereço IP do gateway da rede local conectada.
 *
 * @param[out] out_gw  Buffer de destino para a string do IP.
 * @param[in]  max_len Tamanho máximo do buffer.
 *
 * @return true se o IP foi copiado com sucesso.
 */
bool wifi_transfer_get_gateway_ip(char *out_gw, size_t max_len);

/**
 * @brief Consulta as credenciais do Hotspot SoftAP gerado pelo MPS3.
 *
 * @param[out] out_ssid Buffer que recebe o nome da rede (SSID).
 * @param[in]  ssid_len Tamanho do buffer de SSID.
 * @param[out] out_pass Buffer que recebe a senha da rede.
 * @param[in]  pass_len Tamanho do buffer de senha.
 */
void wifi_transfer_get_ap_credentials(char *out_ssid, size_t ssid_len, char *out_pass, size_t pass_len);

/**
 * @brief Retorna a quantidade de redes Wi-Fi conhecidas salvas na NVS.
 *
 * @return Número de redes cadastradas.
 */
int wifi_transfer_get_known_count(void);

/**
 * @brief Obtém as credenciais de uma rede salva na NVS pelo seu índice.
 *
 * @param[in]  idx      Índice da rede (0 a count-1).
 * @param[out] out_ssid Buffer de saída para o SSID.
 * @param[in]  ssid_len Tamanho do buffer de SSID.
 * @param[out] out_pass Buffer de saída para a senha.
 * @param[in]  pass_len Tamanho do buffer de senha.
 *
 * @return true se os dados foram recuperados com sucesso.
 */
bool wifi_transfer_get_known_network(int idx, char *out_ssid, size_t ssid_len, char *out_pass, size_t pass_len);

/**
 * @brief Preenche uma string formatada com o status atual do Wi-Fi para exibição no display OLED.
 *
 * @param[out] out            Buffer de saída para a string de status.
 * @param[in]  out_len        Tamanho do buffer de saída.
 * @param[out] files_received Ponteiro onde será gravado o número de arquivos recebidos na sessão.
 */
void wifi_transfer_get_status(char *out, size_t out_len, int *files_received);

/**
 * @brief Estrutura que descreve o progresso de transferências de arquivos via web.
 */
typedef struct {
    bool in_progress;       /**< true se há transferência ocorrendo neste instante. */
    char filename[64];      /**< Nome do arquivo sendo transferido. */
    int file_pct;           /**< Progresso percentual do arquivo atual (0-100%). */
    int overall_pct;        /**< Progresso percentual do lote total (-1 se avulso). */
    int idx;                /**< Índice do arquivo atual no lote. */
    int count;              /**< Quantidade total de arquivos no lote. */
    float kbps;             /**< Velocidade média de transferência em KB/s. */
    int eta_sec;            /**< Estimativa de tempo restante do arquivo atual em segundos. */
    int total_eta_sec;      /**< Estimativa de tempo restante do lote completo em segundos. */
} wifi_transfer_progress_t;

/**
 * @brief Obtém os dados de progresso da transferência ativa para desenho da barra na UI.
 *
 * @param[out] out Ponteiro para a estrutura `wifi_transfer_progress_t`.
 */
void wifi_transfer_get_progress(wifi_transfer_progress_t *out);

/**
 * @brief Inicia o Wi-Fi com seleção automática inteligente.
 *
 * Tenta conectar nas redes salvas na NVS; se nenhuma estiver ao alcance ou falhar,
 * sobe automaticamente o Hotspot SoftAP com captive portal.
 *
 * @return ESP_OK se o procedimento foi iniciado com sucesso.
 */
esp_err_t wifi_transfer_enter_auto(void);

/**
 * @brief Informa se o servidor DNS captive portal está em execução.
 *
 * @return true se o DNS estiver ativo; false caso contrário.
 */
bool wifi_transfer_is_dns_active(void);

/**
 * @brief Retorna o modo de operação Wi-Fi atual.
 *
 * @return Enum `wifi_transfer_mode_t`.
 */
wifi_transfer_mode_t wifi_transfer_get_mode(void);

/**
 * @brief Retorna a quantidade de smartphones/computadores conectados ao SoftAP do MPS3.
 *
 * @return Número de clientes associados.
 */
int wifi_transfer_get_connected_clients(void);

/**
 * @brief Registra o item "WiFi" no menu carrossel principal do sistema.
 */
void wifi_transfer_register_menu_entry(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_TRANSFER_H
