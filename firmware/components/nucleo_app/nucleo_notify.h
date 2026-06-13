// Unified notification backbone — one entry point for every source, on the device side.
// Mirror of the web Notification Center contract (see docs/notify-protocol.md).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { NOTIFY_INFO = 0, NOTIFY_SUCCESS, NOTIFY_WARN, NOTIFY_CRITICAL } notify_level_t;

// Emit a notification. EVENT-DRIVEN: this does work only when called — there is no task and no
// polling behind it (the only periodic caller is the existing 15 s calendar service). It:
//   1) appends a compact line to the SD journal (history; size-rotated, ~zero RAM),
//   2) publishes "notify.post" on the event bus -> WebSocket -> the web Notification Center,
//   3) if NO web client is driving the device, plays the level's polyphonic chime (synthesized
//      once to SD, then cached) and raises the on-screen banner. While a client is connected the
//      device stays silent and lets the client own the alert (same rule as the calendar service).
//
// All strings may be NULL/empty. `id` is an optional coalescing key. `action` follows the copilot
// contract: "app:<id>", "file:<path>", "anima:<query>", or "" / NULL for none.
void nucleo_notify_emit(const char *src, notify_level_t lvl, const char *id,
                        const char *title, const char *body, const char *action);

// Push-to-Talk "ready, talk now" earcon: plays the cached voice chime and BLOCKS until it finishes,
// so the caller can claim the I2S mic next (speaker + PDM mic share the WS line). The voice engine
// calls this via a link-time extern (it does not include this header — that would force a component
// REQUIRES cycle), so the symbol must keep C linkage and this exact name.
void nucleo_voice_ready_chime(void);

// Push-to-Talk "done listening" earcon: the counterpart played after recognition, BLOCKS until it
// finishes. Same link-time-extern contract as the ready chime (keep C linkage + this exact name).
void nucleo_voice_done_chime(void);

#ifdef __cplusplus
}
#endif
