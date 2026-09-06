#ifndef NVS_FLASH_H
#define NVS_FLASH_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t nvs_handle_t;

typedef enum {
    NVS_READONLY,
    NVS_READWRITE
} nvs_open_mode_t;

static inline esp_err_t nvs_flash_init(void) { return ESP_OK; }
static inline esp_err_t nvs_flash_erase(void) { return ESP_OK; }

static inline esp_err_t nvs_open(const char* name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle) {
    (void)name; (void)open_mode; if (out_handle) *out_handle = 1; return ESP_OK;
}
static inline void nvs_close(nvs_handle_t handle) { (void)handle; }
static inline esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return ESP_OK; }
static inline esp_err_t nvs_erase_all(nvs_handle_t handle) { (void)handle; return ESP_OK; }

static inline esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key, const void* value, size_t length) {
    (void)handle; (void)key; (void)value; (void)length; return ESP_OK;
}
static inline esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* out_value, size_t* length) {
    (void)handle; (void)key; (void)out_value; (void)length; return ESP_ERR_NOT_FOUND;
}

static inline esp_err_t nvs_set_u8(nvs_handle_t handle, const char* key, uint8_t value) {
    (void)handle; (void)key; (void)value; return ESP_OK;
}
static inline esp_err_t nvs_get_u8(nvs_handle_t handle, const char* key, uint8_t *out_value) {
    (void)handle; (void)key; (void)out_value; return ESP_ERR_NOT_FOUND;
}

#ifdef __cplusplus
}
#endif

#endif // NVS_FLASH_H
