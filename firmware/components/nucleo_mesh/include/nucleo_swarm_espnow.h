// nucleo_swarm_espnow — device engine for the Swarm app: binds the pure cores (nucleo_mesh +
// nucleo_chorus + nucleo_swarm[_sec]) to the real ESP-NOW radio + mbedtls. Mirrors nucleo_link_espnow:
// one service task drains the RX queue, the app stays a thin UI. Runs while the Swarm app is
// foreground (exclusive_flags = NX_NET_APP frees ~70KB; Wi-Fi STA — which ESP-NOW rides — stays up).
//
// First-test scope: presence (peers discovered via gossiped chorus.cap manifests) + a ping/pong
// round-trip. Membership defaults to OPEN; set a name+passphrase to close it (HMAC over a PBKDF2 PSK).
// The L1 "organ" (answer a peer's offline query) is the next step, gated on the load-on-accept path.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- lifecycle (app on_enter / on_exit) -----------------------------------
bool        swarm_svc_start(void);     // init ESP-NOW on the current Wi-Fi; false if it can't
void        swarm_svc_stop(void);      // tear down ESP-NOW, free state

// ---- identity / membership -------------------------------------------------
const char *swarm_svc_name(void);      // our device id (gossiped as the origin)
int         swarm_svc_channel(void);   // current Wi-Fi channel (peers must share it)
bool        swarm_svc_is_open(void);   // true = no passphrase (anyone co-channel is admitted)
// Join/close the swarm: empty pass -> OPEN (0); valid -> CLOSED, PSK = PBKDF2(pass, SW_SALT|name) (1);
// out-of-policy/KDF error -> -1 (stays OPEN). name is the SSID-like salt; both must match across devices.
int         swarm_svc_set_passphrase(const char *name, const char *pass);

// ---- directory (discovered peers, from gossiped manifests) -----------------
int         swarm_svc_peer_count(void);          // fresh (non-stale) peers
const char *swarm_svc_peer_id(int i);
int         swarm_svc_peer_free(int i);          // advertised free KB
int         swarm_svc_peer_busy(int i);

// ---- actions / status ------------------------------------------------------
void        swarm_svc_ping(void);                // broadcast a ping; peers reply (pong recorded)
void        swarm_svc_last(char *out, int cap);  // last activity line for the UI

#ifdef __cplusplus
}
#endif
