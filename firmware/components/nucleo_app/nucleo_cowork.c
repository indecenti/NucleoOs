// nucleo_cowork — see include/nucleo_cowork.h.
//
// Two binary semaphores make the hand-off: `go` wakes the worker with a job, `idle` is given back when it is done
// (and is "given" while no job runs, so run() and wait() simply take it). The job's function and argument are
// written before `go` is given and read after it is taken: the semaphore orders them across the cores.
#include "nucleo_cowork.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "cowork";

static TaskHandle_t      s_task;
static SemaphoreHandle_t s_go, s_idle;
static void (*volatile s_fn)(void *);
static void *volatile s_arg;
static volatile bool     s_quit;

static void worker(void *unused)
{
    (void)unused;
    for (;;) {
        xSemaphoreTake(s_go, portMAX_DELAY);
        if (s_quit) break;
        s_fn(s_arg);
        xSemaphoreGive(s_idle);
    }
    xSemaphoreGive(s_idle);
    vTaskDelete(NULL);
}

static bool start(void)
{
    if (s_task) return true;
    if (!s_go) s_go = xSemaphoreCreateBinary();
    if (!s_idle) s_idle = xSemaphoreCreateBinary();
    if (!s_go || !s_idle) return false;
    xSemaphoreGive(s_idle);                            // idle: no job yet
    s_quit = false;
    const BaseType_t other = xPortGetCoreID() ? 0 : 1;
    // One priority above the caller's usual level, so a job starts as soon as it is handed over.
    if (xTaskCreatePinnedToCore(worker, "cowork", 4096, NULL, uxTaskPriorityGet(NULL) + 1, &s_task, other) != pdPASS) {
        s_task = NULL;
        xSemaphoreTake(s_idle, 0);
        ESP_LOGW(TAG, "no memory for the worker");
        return false;
    }
    ESP_LOGI(TAG, "worker on core %d", (int)other);
    return true;
}

bool nucleo_cowork_run(void (*fn)(void *), void *arg)
{
    if (!fn || !start()) return false;
    xSemaphoreTake(s_idle, portMAX_DELAY);             // the previous job is done
    s_fn = fn; s_arg = arg;
    xSemaphoreGive(s_go);
    return true;
}

void nucleo_cowork_wait(void)
{
    if (!s_task) return;
    xSemaphoreTake(s_idle, portMAX_DELAY);
    xSemaphoreGive(s_idle);
}

void nucleo_cowork_stop(void)
{
    if (!s_task) return;
    xSemaphoreTake(s_idle, portMAX_DELAY);             // let the running job finish
    s_quit = true;
    xSemaphoreGive(s_go);
    xSemaphoreTake(s_idle, portMAX_DELAY);             // the worker's last word before it deletes itself
    s_task = NULL;
    vSemaphoreDelete(s_go); vSemaphoreDelete(s_idle);
    s_go = s_idle = NULL;
}
