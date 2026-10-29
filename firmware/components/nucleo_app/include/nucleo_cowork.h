// nucleo_cowork — the ESP32-S3's second core for a native app's heavy loop.
//
// A native app runs on one core; the other idles through most of a game (Wi-Fi is down in a Solo boot). This
// lends it: run(fn, arg) hands one job to a worker task pinned to the OTHER core and returns at once; the caller
// keeps working while it runs. One job at a time: run() first waits for the previous one, wait() blocks until the
// current one is done. The worker is created on the first run() (4 KB stack) and removed by stop(), which a
// module calls when its session ends — nothing is held while no app uses it.
//
// The job runs in another task: it must not touch what the caller changes meanwhile (hand it a snapshot), and a
// driver it uses (the panel, for instance) must be used only from the worker while the job may be running.
// Exported to store modules at ABI level 6.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Run fn(arg) on the other core. Waits for the previous job first. False when the worker cannot be created
// (no memory): the caller then runs fn itself.
bool nucleo_cowork_run(void (*fn)(void *arg), void *arg);
// Block until the current job (if any) has finished.
void nucleo_cowork_wait(void);
// Wait for the current job, then remove the worker and give its stack back.
void nucleo_cowork_stop(void);

#ifdef __cplusplus
}
#endif
