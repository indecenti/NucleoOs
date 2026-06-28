// nucleo_swarm — swarm MEMBERSHIP / join layer. Pure SANS-I/O, composed from nucleo_swarm_sec.
//
// "How you join a swarm", WPA-style: set the same PASSPHRASE on every device -> the same PSK
// (derived by an injected KDF: PBKDF2-HMAC-SHA256 on the device, a mock in the host gate). Knowing
// the passphrase = being a member. An inbound frame is ADMITTED only if its HMAC verifies under the
// PSK (and, if MAC-pinning is enforced, the sender is pinned). An empty passphrase = an OPEN swarm
// (no auth) — the honest "not configured yet" state. Discovery is automatic (MESH gossip on the
// agreed channel); this layer decides membership, not discovery. See docs/swarm-architecture.md.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "nucleo_swarm_sec.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SW_PSK_LEN   32            // = SW_MAC_OUT (HMAC-SHA256 / PBKDF2 output)
#define SW_PASS_MIN  8             // WPA-style passphrase length policy
#define SW_PASS_MAX  63
#define SW_SALT      "nucleo-swarm-v1"   // KDF salt PREFIX (version/domain separation). The FULL salt MUST
                                         // be SW_SALT + 0x1f + the swarm NAME (network.swarm.name — an
                                         // SSID-like shared, non-secret id), so different swarms with the
                                         // same passphrase derive DIFFERENT PSKs. A global constant salt
                                         // would let ONE precomputed table attack every NucleoOS swarm.
                                         // The device shim builds the salt; swarm_join takes the final bytes.
                                         // Rule: same name + same passphrase = same swarm.

// Injected KDF: derive a 32-byte PSK from (passphrase, salt). Device = PBKDF2-HMAC-SHA256 (>=4096
// iters); host gate = a deterministic mock. Return 0 on success.
typedef int (*sw_kdf_fn)(void *user, const char *pass, int plen,
                         const uint8_t *salt, int slen, uint8_t out[SW_PSK_LEN]);

typedef struct {
    uint8_t   psk[SW_PSK_LEN];
    bool      have_psk;            // false = OPEN swarm (no passphrase configured)
    sw_pins_t pins;                // optional MAC trust-pin (sw_pin_* on &m->pins)
} swarm_member_t;

void swarm_member_init(swarm_member_t *m);

// Join (or re-key) a swarm from a passphrase. An empty / too-short passphrase CLEARS the key and
// leaves the swarm OPEN (returns 0). A valid passphrase derives the PSK (returns 1). >SW_PASS_MAX or
// a KDF failure returns -1 (and leaves the swarm OPEN, fail-safe). `salt` MUST be SW_SALT + 0x1f +
// the swarm name (see SW_SALT) — never a bare global constant.
int  swarm_join(swarm_member_t *m, const char *passphrase, const uint8_t *salt, int slen,
                sw_kdf_fn kdf, void *u);

bool swarm_is_open(const swarm_member_t *m);   // true if no passphrase set (no auth)

// Admit an inbound raw frame from sender `mac6`. Rejects non-mesh frames and (if pinning is enforced)
// unpinned MACs. CLOSED swarm: requires a valid HMAC under the PSK and strips the tag. OPEN swarm:
// accepts the frame as-is. Returns the payload length to hand to the mesh layer, or -1 on reject.
int  swarm_admit(const swarm_member_t *m, const uint8_t *mac6, const uint8_t *buf, int len,
                 sw_mac_fn mac, void *u);

// Seal an outbound mesh frame for our swarm. CLOSED: append the HMAC tag (needs cap >= len+SW_TAG_LEN).
// OPEN: no-op. Returns the new length, or -1 on error.
int  swarm_seal_out(const swarm_member_t *m, uint8_t *buf, int len, int cap, sw_mac_fn mac, void *u);

#ifdef __cplusplus
}
#endif
