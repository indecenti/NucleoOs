// nucleo_pnet — minimal best-effort ESP-NOW datagram layer for real-time native games.
//
// Vicino (nucleo_link) and Sciame (nucleo_mesh) ride ESP-NOW too, but their job is RELIABILITY:
// sliding window + ACK + CRC + retransmission. A 2-player action game wants the OPPOSITE — the
// smallest possible round-trip and tolerance for the odd dropped frame (the next snapshot supersedes
// it anyway). So this is a deliberately tiny, connectionless layer: send a raw datagram to a peer
// (or broadcast), drain whatever arrived. No windows, no ACKs, no SD, no service task.
//
// Channel: ESP-NOW only reaches peers on the SAME Wi-Fi channel. We follow the proven nucleo_link
// rule and add AP coverage: if we're associated as STA *or* running a SoftAP, ESP-NOW rides that
// channel (peers on the same network meet automatically); otherwise we park on channel 1 so two
// otherwise-idle devices can still find each other for a local match. pnet_channel() exposes it so
// the lobby can tell the player which channel both Cardputers must share.
//
// RAM: the app that uses this declares exclusive_flags = NX_NET_APP (frees ~70KB; Wi-Fi STA, which
// ESP-NOW rides, stays up). The RX callback only copies bytes into a small FreeRTOS queue; all
// parsing happens on the app task in pnet_recv().
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PNET_MAXMSG 200          // max datagram payload we carry (well under the 250-byte ESP-NOW MTU)

typedef struct {
    uint8_t mac[6];              // sender MAC
    int     len;                 // payload length
    uint8_t buf[PNET_MAXMSG];    // payload
} pnet_pkt_t;

// ---- lifecycle (app on_enter / on_exit) -----------------------------------
bool        pnet_start(void);    // init ESP-NOW on the current Wi-Fi + RX queue; false if it can't
void        pnet_stop(void);     // unregister + deinit ESP-NOW, free the queue

// ---- identity / link -------------------------------------------------------
const char *pnet_name(void);     // this device's name (for HELLO/JOIN payloads)
int         pnet_channel(void);  // current Wi-Fi channel (peers must share it)

// ---- datagrams -------------------------------------------------------------
// Send `len` bytes to peer `mac6` (NULL = broadcast). Best-effort: returns 0 if the radio accepted
// the frame, -1 otherwise. Auto-registers the peer the first time we send to a new unicast MAC.
int         pnet_send(const uint8_t *mac6, const void *buf, int len);

// Pop one received datagram into `out`. Returns true if one was available, false if the queue is
// empty. Call repeatedly (e.g. from the game poll) to drain everything that arrived this frame.
bool        pnet_recv(pnet_pkt_t *out);

#ifdef __cplusplus
}
#endif
