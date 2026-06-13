// Host shim for FreeRTOS.h — just the bits nucleo_eventbus.c references. The eventbus is
// single-task-safe by design; the host test is single-threaded, so the mutex is a no-op.
#pragma once
#include <stdint.h>

typedef uint32_t TickType_t;
typedef int      BaseType_t;

#define portMAX_DELAY ((TickType_t)0xffffffffU)
#define pdTRUE        ((BaseType_t)1)
#define pdFALSE       ((BaseType_t)0)
