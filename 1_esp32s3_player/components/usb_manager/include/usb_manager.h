#ifndef USB_MANAGER_H
#define USB_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Modos que o usuario pode escolher
typedef enum {
    USB_MODE_NONE = 0,
    USB_MODE_MSC, // Pendrive
    USB_MODE_DAC  // Placa de Som
} usb_mode_t;

// Inicia a task e configura o TinyUSB como dispositivo composto (Sentinel mode)
esp_err_t usb_manager_init(void);

// Chamado pela UI quando o usuario escolhe um modo apos conectar
void usb_manager_set_mode(usb_mode_t mode);

// Retorna se o cabo USB esta conectado ao PC (Host detectado)
bool usb_manager_is_connected(void);

// Envia comandos de multimidia HID para o PC
// Comandos comuns: 0=Play/Pause, 1=Next, 2=Prev, 3=VolUp, 4=VolDown
void usb_manager_send_hid(int command);

#ifdef __cplusplus
}
#endif

#endif // USB_MANAGER_H

