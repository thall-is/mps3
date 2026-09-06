#ifndef U8G2_H_STUB
#define U8G2_H_STUB
#include <stdint.h>

typedef struct { int dummy; } u8g2_t;
typedef const uint8_t *u8g2_font_t; // aproximacao - o real usa uint8_t[] extern, mas so' testamos que o simbolo existe e o tipo passa como ponteiro

extern const uint8_t u8g2_font_4x6_tr[];
extern const uint8_t u8g2_font_5x7_tr[];
extern const uint8_t u8g2_font_6x10_tr[];
extern const uint8_t u8g2_font_6x13_tr[];
extern const uint8_t u8g2_font_haxrcorp4089_tr[];
extern const uint8_t u8g2_font_helvB08_tr[];
extern const uint8_t u8g2_font_profont10_tr[];
extern const uint8_t u8g2_font_profont15_tr[];
extern const uint8_t u8g2_font_profont17_tr[];
extern const uint8_t u8g2_font_profont22_tr[];
extern const uint8_t u8g2_font_t0_17_tr[];
extern const uint8_t u8g2_font_timR18_tr[];

static inline void u8g2_ClearBuffer(u8g2_t *u) { (void)u; }
static inline void u8g2_SendBuffer(u8g2_t *u) { (void)u; }
static inline void u8g2_InitDisplay(u8g2_t *u) { (void)u; }
static inline void u8g2_SetPowerSave(u8g2_t *u, uint8_t save) { (void)u; (void)save; }
static inline void u8g2_SetDrawColor(u8g2_t *u, uint8_t c) { (void)u; (void)c; }
static inline void u8g2_SetFont(u8g2_t *u, const uint8_t *f) { (void)u; (void)f; }
static inline void u8g2_SetFontMode(u8g2_t *u, uint8_t m) { (void)u; (void)m; }
static inline void u8g2_SetBitmapMode(u8g2_t *u, uint8_t m) { (void)u; (void)m; }
static inline void u8g2_SetClipWindow(u8g2_t *u, int16_t x0, int16_t y0, int16_t x1, int16_t y1) { (void)u; (void)x0; (void)y0; (void)x1; (void)y1; }
static inline void u8g2_SetMaxClipWindow(u8g2_t *u) { (void)u; }
static inline int u8g2_DrawStr(u8g2_t *u, int16_t x, int16_t y, const char *s) { (void)u; (void)x; (void)y; (void)s; return 0; }
static inline int u8g2_GetStrWidth(u8g2_t *u, const char *s) { (void)u; (void)s; return 0; }
static inline void u8g2_DrawBox(u8g2_t *u, int16_t x, int16_t y, int16_t w, int16_t h) { (void)u; (void)x; (void)y; (void)w; (void)h; }
static inline void u8g2_DrawFrame(u8g2_t *u, int16_t x, int16_t y, int16_t w, int16_t h) { (void)u; (void)x; (void)y; (void)w; (void)h; }
static inline void u8g2_DrawHLine(u8g2_t *u, int16_t x, int16_t y, int16_t w) { (void)u; (void)x; (void)y; (void)w; }
static inline void u8g2_DrawLine(u8g2_t *u, int16_t x0, int16_t y0, int16_t x1, int16_t y1) { (void)u; (void)x0; (void)y0; (void)x1; (void)y1; }
static inline void u8g2_DrawPixel(u8g2_t *u, int16_t x, int16_t y) { (void)u; (void)x; (void)y; }
static inline void u8g2_DrawTriangle(u8g2_t *u, int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2) {
    (void)u; (void)x0; (void)y0; (void)x1; (void)y1; (void)x2; (void)y2;
}
static inline void u8g2_DrawXBMP(u8g2_t *u, int16_t x, int16_t y, int16_t w, int16_t h, const uint8_t *bits) {
    (void)u; (void)x; (void)y; (void)w; (void)h; (void)bits;
}
typedef struct { int dummy; } u8x8_t;
typedef uint8_t (*u8x8_msg_cb)(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr);
typedef enum { U8G2_R0 = 0, U8G2_R1, U8G2_R2, U8G2_R3, U8G2_MIRROR } u8g2_cb_t;
static inline void u8g2_Setup_ssd1306_i2c_128x64_noname_f(u8g2_t *u, u8g2_cb_t rotation,
                                                            u8x8_msg_cb byte_cb, u8x8_msg_cb gpio_cb) {
    (void)u; (void)rotation; (void)byte_cb; (void)gpio_cb;
}
#endif
