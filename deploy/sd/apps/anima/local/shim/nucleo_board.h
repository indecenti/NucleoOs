// WASM override of nucleo_board.h — one of only two wasm-specific headers. Every other ESP-IDF
// shim is SHARED with the host harness (build.ps1 puts tools/anima-host/shim on the include path
// after this dir), so a shim the firmware newly needs is added once, in tools/anima-host/shim.
// The ANIMA cascade only reads NUCLEO_SD_MOUNT (L1 encoder/index, learned facts, dictionaries,
// session). Under Emscripten the knowledge pack is mounted into the in-memory filesystem at /sd,
// so the firmware's fopen("/sd/data/anima/...") paths resolve there unchanged; the host points
// the same macro at ./sd instead. Keep HLOG in sync with tools/anima-host/shim/nucleo_board.h.
#pragma once

#define NUCLEO_SD_MOUNT   "/sd"
#define NUCLEO_CFG_MOUNT  "/cfg"
#define NUCLEO_CFG_LABEL  "cfg"

// HLOG(): the real header gates heap tracing (NUCLEO_HEAPLOG) used by nucleo_anima_l1.c.
// No constrained heap to trace in the browser — make it a no-op so sources compile unchanged.
#ifndef HLOG
#define HLOG(tag, ...) ((void)0)
#endif
