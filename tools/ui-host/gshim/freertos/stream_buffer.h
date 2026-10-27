// ui-host gallery shim (FreeRTOS)
#pragma once
#include <stddef.h>
#include "freertos/FreeRTOS.h"
typedef void *StreamBufferHandle_t;
#ifdef __cplusplus
extern "C" {
#endif
StreamBufferHandle_t xStreamBufferCreate(size_t n, size_t trig);
size_t xStreamBufferSend(StreamBufferHandle_t s, const void *data, size_t n, TickType_t t);
size_t xStreamBufferReceive(StreamBufferHandle_t s, void *data, size_t n, TickType_t t);
size_t xStreamBufferBytesAvailable(StreamBufferHandle_t s);
void vStreamBufferDelete(StreamBufferHandle_t s);
BaseType_t xStreamBufferReset(StreamBufferHandle_t s);
#ifdef __cplusplus
}
#endif
