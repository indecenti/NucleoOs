// Host shim for nucleo_board.h. The real header is just Cardputer pin macros + mount
// points; ANIMA only ever reads NUCLEO_SD_MOUNT (session, telemetry, L1 encoder/index).
// We point it at a local ./sd tree so the harness reads/writes beside the exe instead of
// a device path, and no firmware source needs editing.
#pragma once

#define NUCLEO_SD_MOUNT   "./sd"
#define NUCLEO_CFG_MOUNT  "./cfg"
#define NUCLEO_CFG_LABEL  "cfg"

// HLOG(): the real nucleo_board.h adds compile-time-gated heap tracing (NUCLEO_HEAPLOG) used by
// nucleo_anima_l1.c. On the host there is no constrained heap to trace — make it a no-op so the
// firmware sources compile unchanged. Keep in sync with the real header's HLOG signature.
#ifndef HLOG
#define HLOG(tag, ...) ((void)0)
#endif
