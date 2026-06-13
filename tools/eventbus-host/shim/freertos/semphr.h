// Host shim for FreeRTOS semphr.h — no-op mutex (the host test is single-threaded; the eventbus's
// real locking is exercised on-device). Just enough to compile nucleo_eventbus.c unchanged.
#pragma once
#include "freertos/FreeRTOS.h"

typedef void *SemaphoreHandle_t;

static int s_host_mutex_token;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)&s_host_mutex_token; }
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void)s; (void)t; return pdTRUE; }
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
