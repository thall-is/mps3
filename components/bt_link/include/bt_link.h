#ifndef BT_LINK_H
#define BT_LINK_H

// Ponte entre esta placa e o companheiro de Bluetooth (ESP32 classico,
// projeto bt_companion) - fala o protocolo de components/uart_ctrl e
// se registra no menu principal (ver menu.h) como mais um item, logo
// apos o Wifi.
//
// A reproducao de audio Bluetooth roda em paralelo com a DAC local.
// Sair da tela de status mantem o audio tocando continuamente.
// Para desconectar ou parear, usa-se o botao central dentro da tela.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Inicializa a UART de controle com o companheiro e registra o
// callback de mudanca de taxa de amostragem no i2s_output.
void bt_link_init(void);

// Registra o item "Bluetooth" no menu principal.
void bt_link_register_menu_entry(void);

// Fornece um pulso de reset no pino EN do companheiro
void bt_link_reset_companion(void);

// Envia comando de volume absoluto ao companheiro (0-100%)
void bt_link_send_volume(uint8_t volume_percent);

#ifdef __cplusplus
}
#endif

#endif // BT_LINK_H

