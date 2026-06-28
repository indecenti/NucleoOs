// nucleo_swarm_sec — the swarm security gate (P3), pure SANS-I/O so it is host-tested.
//
// The validation made these MANDATORY, not deferrable: the ESP-NOW transport is open
// (peers added on any PING, p.encrypt=false, CRC32-only), so before any swarm frame is acted on it
// must be AUTHENTICATED. Three pure primitives, host-proven; the device wires them to mbedtls
// HMAC-SHA256 + esp_now PMK/LMK encryption (that thin shim is the only build-gated part).
//   1. sw_demux   — route a raw frame by leading magic ("NL" = Vicino link, "NM" = mesh/chorus).
//   2. sw_seal/open — append/verify a per-swarm HMAC tag (the hash fn is INJECTED: mbedtls on
//                     device, a deterministic mock in the host gate). Verify is constant-time.
//   3. sw_pin_*   — a MAC trust-pin allowlist: only pinned peers may be auto-accepted.
// See docs/swarm-architecture.md (P3). PSK/LMK key management + the radio binding live in the shim.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SW_TAG_LEN  8     // truncated HMAC tag appended to a gossip frame (bounds frame growth)
#define SW_MAC_OUT  32    // HMAC-SHA256 output length

// Injected MAC: HMAC-SHA256(key,msg) -> out[32]. Return 0 on success. Device = mbedtls_md_hmac;
// host gate = a deterministic mock (proves framing/compare, not crypto strength).
typedef int (*sw_mac_fn)(void *user, const uint8_t *key, int klen,
                         const uint8_t *msg, int mlen, uint8_t out[SW_MAC_OUT]);

enum { SW_KIND_UNKNOWN = 0, SW_KIND_LINK, SW_KIND_MESH };
int sw_demux(const uint8_t *buf, int len);

// Append an auth tag over buf[0..len). Needs cap >= len + SW_TAG_LEN. Returns the new length, or -1.
// NOTE: a sealed frame is SW_TAG_LEN longer, so the mesh layer must keep its frame <= 250 - SW_TAG_LEN.
int sw_seal(const uint8_t *key, int klen, uint8_t *buf, int len, int cap, sw_mac_fn mac, void *u);

// Verify the trailing tag (CONSTANT-TIME). Returns the payload length (len - SW_TAG_LEN) on success,
// or -1 on any failure (too short, bad MAC, forged/tampered). The caller passes the payload onward.
int sw_open(const uint8_t *key, int klen, const uint8_t *buf, int len, sw_mac_fn mac, void *u);

// MAC trust-pin: a small allowlist. With enforce=false everything is allowed (open swarm); with
// enforce=true only pinned MACs pass — checked BEFORE any auto-accept / mode change on the organ.
#define SW_PINS 8
typedef struct { uint8_t mac[SW_PINS][6]; int n; bool enforce; } sw_pins_t;
void sw_pin_reset(sw_pins_t *p, bool enforce);
bool sw_pin_add(sw_pins_t *p, const uint8_t mac[6]);
bool sw_pin_allowed(const sw_pins_t *p, const uint8_t mac[6]);

#ifdef __cplusplus
}
#endif
