#ifndef ESP_SYSTEM_H
#define ESP_SYSTEM_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline uint32_t esp_get_free_heap_size(void) {
    return 180000;
}

static inline uint32_t esp_get_minimum_free_heap_size(void) {
    return 150000;
}

static inline void esp_restart(void) {
}

#ifdef __cplusplus
}
#endif

#endif // ESP_SYSTEM_H
