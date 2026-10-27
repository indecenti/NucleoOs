// App store client — device glue: runs the engine (store_engine.h) on its own task over the SD-content HTTPS
// transport, so the UI keeps drawing. One operation at a time. Design and upgrade rules: docs/app-store.md.
#pragma once
#include <stdbool.h>
#include "store_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NUCLEO_STORE_URL "https://indecenti.github.io/nucleoos-cardputer-store/"

typedef enum { NST_IDLE, NST_BUSY, NST_DONE, NST_FAILED } nst_status_t;
typedef enum { NST_OP_REFRESH, NST_OP_INSTALL, NST_OP_REMOVE } nst_op_t;

// Start an operation (false = one is already running, or it could not start). The app must already hold
// the RAM for TLS (it runs in NX_NET_APP exclusive mode).
bool nucleo_store_start(nst_op_t op, const char *id);
nst_status_t nucleo_store_status(void);       // NST_DONE / NST_FAILED stay until the next start
nst_op_t     nucleo_store_op(void);
const st_state_t *nucleo_store_state(void);   // live progress / last result
const char  *nucleo_store_error(void);        // localized one-liner for NST_FAILED ("" otherwise)
const char  *nucleo_store_os_version(void);   // "x.y.z" of this firmware, for min_os

#ifdef __cplusplus
}
#endif
