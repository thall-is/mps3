#ifndef FREERTOS_TASK_H_STUB
#define FREERTOS_TASK_H_STUB
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
static inline void vTaskDelay(TickType_t ticks) { (void)ticks; }
static inline TickType_t xTaskGetTickCount(void) { return 0; }
static inline BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack,
                                      void *arg, UBaseType_t prio, TaskHandle_t *handle) {
    (void)fn; (void)name; (void)stack; (void)arg; (void)prio; (void)handle;
    return pdPASS;
}
static inline BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack,
                                                  void *arg, UBaseType_t prio, TaskHandle_t *handle, int core) {
    (void)fn; (void)name; (void)stack; (void)arg; (void)prio; (void)handle; (void)core;
    return pdPASS;
}
#endif
