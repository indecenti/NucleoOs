#pragma once
#include "FreeRTOS.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*TaskFunction_t)(void *);
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio, TaskHandle_t *out);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio, TaskHandle_t *out, BaseType_t core);
void vTaskDelete(TaskHandle_t t);
void vTaskDelay(TickType_t t);
TickType_t xTaskGetTickCount(void);
#ifdef __cplusplus
}
#endif
