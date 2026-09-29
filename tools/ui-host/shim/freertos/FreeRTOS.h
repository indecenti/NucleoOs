// ui-host: just enough FreeRTOS surface for the UI sources to compile. Tasks never run on the host:
// xTaskCreate reports success but the scene drives state directly.
#pragma once
#include <stdint.h>
typedef void *TaskHandle_t;
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
#define pdPASS 1
#define pdFAIL 0
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 0xFFFFFFFFu
#define tskIDLE_PRIORITY 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portTICK_PERIOD_MS 1
