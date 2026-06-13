// nucleo_link_espnow.h — the device-side engine behind the "Vicino" app.
//
// Binds the pure protocol core (nucleo_link.h) and the Bruce codec (nucleo_link_bruce.h) to the real
// ESP-NOW radio + SD card, and owns ALL the heavy state so the native app (app_link.cpp) stays a thin
// UI. The app calls this; later the HTTP API and ANIMA skill will too.
//
// RAM/optimization notes (PSRAM-less Cardputer, ~18KB free):
//   • The app declares exclusive_flags = NX_NET_APP, so the framework frees ~70KB (httpd/mDNS/voice/L1)
//     for the WHOLE time Vicino is foreground — Wi-Fi STA stays up, which ESP-NOW needs.
//   • Files STREAM through SD: at most one frame + a small read block is buffered, never the whole file
//     (the protocol core is zero-heap / RAM-O(window)). Works for files far larger than RAM.
//   • The ESP-NOW receive callback (Wi-Fi task context) only copies bytes into a FreeRTOS queue;
//     all parsing/disk I/O happens in nlink_svc_pump() on the app task.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "nucleo_link.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { NLINK_PROTO_NUCLEO = 0, NLINK_PROTO_BRUCE = 1 } nlink_proto_t;

typedef struct {
    int      active;        // a transfer is in flight
    int      sending;       // 1 = we send, 0 = we receive
    int      proto;         // nlink_proto_t
    int      state;         // nlink_state (RUN/DRAIN/DONE/FAIL...)
    int      reason;        // nlink_reason on failure
    uint32_t done, total;   // bytes
    uint32_t rate_bps;      // throughput estimate
    char     name[64];      // file (or command) name
    char     peer[NLINK_NAME_MAX + 1];
} nlink_status_t;

// ---- lifecycle (app on_enter / on_exit) -----------------------------------
bool        nlink_svc_start(void);          // init ESP-NOW on the current Wi-Fi; false if it can't
void        nlink_svc_stop(void);           // tear down ESP-NOW, close any open files
void        nlink_svc_pump(void);           // drive the state machine + drain RX queue (call from on_tick)

// ---- identity / link -------------------------------------------------------
const char *nlink_svc_name(void);
int         nlink_svc_channel(void);
const char *nlink_svc_inbox(void);          // where received files land (default /sd/data/Vicino)
void        nlink_svc_set_inbox(const char *path);

// ---- discovery -------------------------------------------------------------
void        nlink_svc_discover(nlink_proto_t proto);   // ping the air; peers populate the list
int         nlink_svc_peer_count(void);
const char *nlink_svc_peer_name(int i);
const uint8_t *nlink_svc_peer_mac(int i);
int         nlink_svc_peer_proto(int i);    // which mode this peer answered on
void        nlink_svc_peer_clear(void);

// ---- file transfer ---------------------------------------------------------
// Begin sending `path` to peer `i` using `proto`. false if busy / file unreadable.
bool        nlink_svc_send_file(int peer_i, const char *path, nlink_proto_t proto);
// Arm receive mode for `proto`. An inbound OFFER raises a pending decision (poll below).
void        nlink_svc_listen(nlink_proto_t proto);
bool        nlink_svc_offer_pending(char *name, int ncap, char *from, int fcap, uint32_t *size);
void        nlink_svc_offer_answer(bool accept);       // accept/reject the pending offer

// ---- gated commands (NOT Bruce's arbitrary exec) ---------------------------
// Send a command verb to a peer. The receiver must explicitly confirm before it runs (allowlist + UI).
bool        nlink_svc_send_cmd(int peer_i, const char *cmd, nlink_proto_t proto);
bool        nlink_svc_cmd_pending(char *cmd, int ccap, char *from, int fcap);
void        nlink_svc_cmd_confirm(bool ok);            // run (true) or discard (false) the pending command

// ---- status / control ------------------------------------------------------
void        nlink_svc_status(nlink_status_t *out);
void        nlink_svc_cancel(void);

#ifdef __cplusplus
}
#endif
