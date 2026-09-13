#ifndef BITMAPS_H
#define BITMAPS_H

#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

// Novos bitmaps do menu principal (mockups do Lopaka)

// Player – 128x64, 4 bits por pixel (o array gerado como image_imgbitmap_png_4_bits)
#define MENU_PLAYER_W 128
#define MENU_PLAYER_H 64
extern const unsigned char menu_player_bits[];

// USB – 128x64, 1 bit por pixel
#define MENU_USB_W 128
#define MENU_USB_H 64
extern const unsigned char menu_usb_bits[];

// Menu animações
#define MENU_WIFI_FRAMES 28
#define MENU_CONF_FRAMES 28

// Animação Wi‑Fi – 28 frames de 64x64
#define MENU_WIFI_W 64
#define MENU_WIFI_H 64
#define MENU_WIFI_FRAMES 28
extern const unsigned char *menu_wifi_frames[MENU_WIFI_FRAMES];

// Animações do WiFi (novas telas)
#define WIFI_ATTENTION_CLOUD_FRAMES 28
#define WIFI_SYNC_CLOUD_FRAMES      28
#define WIFI_DOWNLOAD_CLOUD_FRAMES  28

extern const unsigned char *wifi_attention_cloud_frames[WIFI_ATTENTION_CLOUD_FRAMES];
extern const unsigned char *wifi_sync_cloud_frames[WIFI_SYNC_CLOUD_FRAMES];
extern const unsigned char *wifi_download_cloud_frames[WIFI_DOWNLOAD_CLOUD_FRAMES];



// Animação Conf (chave inglesa) – 28 frames de 64x64
#define MENU_CONF_W 64
#define MENU_CONF_H 64
#define MENU_CONF_FRAMES 28
extern const unsigned char *menu_conf_frames[MENU_CONF_FRAMES];

// Animação Game of Life – arquivo fornecido contém 24 frames de 64x64.
#define MENU_GAME_W 64
#define MENU_GAME_H 64
#define MENU_GAME_FRAMES 24
extern const unsigned char *menu_game_frames[MENU_GAME_FRAMES];

void bitmap_animations_init(void);

// Todos os bitmaps do projeto, num lugar so'. Cada um vem com suas
// dimensoes como #define ao lado - use ESSAS constantes nas chamadas de
// u8g2_DrawXBMP() em vez de repetir os numeros soltos, senao um bitmap
// regerado com tamanho diferente desalinha silenciosamente do valor
// hardcoded em outro arquivo.

// Setas decorativas (ao lado do nome do artista, tela de reproducao).
#define BITMAP_BUTTON_ARROW_W 4
#define BITMAP_BUTTON_ARROW_H 7
extern const unsigned char image_ButtonLeft_bits[];
extern const unsigned char image_ButtonRight_bits[];

// Icones da tela de modo USB (oled_display_show_usb_mode).
#define BITMAP_DRIVE_W 112
#define BITMAP_DRIVE_H 35
extern const unsigned char image_Drive_bits[];

#define BITMAP_USBTREE_W 48
#define BITMAP_USBTREE_H 22
extern const unsigned char image_UsbTree_bits[];

#ifdef __cplusplus
}
#endif

#endif // BITMAPS_H
extern const unsigned char image_seele_bits[];
