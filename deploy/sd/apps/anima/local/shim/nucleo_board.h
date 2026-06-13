// WASM shim for nucleo_board.h. The real header is Cardputer pins + mount points;
// the ANIMA cascade only ever reads NUCLEO_SD_MOUNT (L1 encoder/index, learned facts,
// dictionaries, session). Under Emscripten the knowledge pack is mounted into the
// in-memory filesystem at /sd, so the firmware's fopen("/sd/data/anima/...") paths
// resolve there unchanged — no cascade source is edited. Mirrors tools/anima-host/shim.
#pragma once

#define NUCLEO_SD_MOUNT   "/sd"
#define NUCLEO_CFG_MOUNT  "/cfg"
#define NUCLEO_CFG_LABEL  "cfg"

// HLOG(): the real header gates heap tracing (NUCLEO_HEAPLOG) used by nucleo_anima_l1.c.
// No constrained heap to trace in the browser — make it a no-op so sources compile unchanged.
#ifndef HLOG
#define HLOG(tag, ...) ((void)0)
#endif
