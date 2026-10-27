// ui-host gallery shim (FreeRTOS): semaphores that never block.
#pragma once
#include "freertos/FreeRTOS.h"
typedef void *SemaphoreHandle_t;
#ifdef __cplusplus
extern "C" {
#endif
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t t);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s);
void vSemaphoreDelete(SemaphoreHandle_t s);
#ifdef __cplusplus
}
#endif
