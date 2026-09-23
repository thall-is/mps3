#ifndef FREERTOS_STREAM_BUFFER_H_STUB
#define FREERTOS_STREAM_BUFFER_H_STUB
#include "freertos/FreeRTOS.h"
#include <stddef.h>
typedef void *StreamBufferHandle_t;
static inline StreamBufferHandle_t xStreamBufferCreate(size_t cap, size_t trigger) { (void)cap; (void)trigger; return (StreamBufferHandle_t)1; }
static inline size_t xStreamBufferSend(StreamBufferHandle_t s, const void *data, size_t len, TickType_t t) { (void)s; (void)data; (void)t; return len; }
static inline size_t xStreamBufferReceive(StreamBufferHandle_t s, void *data, size_t len, TickType_t t) { (void)s; (void)data; (void)t; return len; }
#endif
