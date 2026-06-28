// nucleo_chorus — federated capability directory over the MESH gossip seam.
//
// "A bulletin board, not a boss." Each device GOSSIPS a tiny self-describing manifest (which
// capabilities it offers, which knowledge domains it holds, its current free RAM, whether it is
// busy) as a `chorus.cap` event over MESH. Every device keeps a small directory of peers' manifests
// and, when it needs something, CONTENT-ROUTES the request to the best peer — no leases, no master
// election, no commanded mode changes. The serving device decides locally whether to honour a
// request (and may enter exclusive mode for that one transfer); the requester only picks who to ask.
//
// Pure SANS-I/O: no esp_*/FreeRTOS/SD. Rides the eventbus/MESH JSON payload (no own wire codec),
// so it is fully host-tested (npm run chorus:test). See docs/swarm-architecture.md.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CH_PEERS    8            // directory size (mirrors the mesh/link peer ceiling)
#define CH_ID_MAX   24           // device id (matches NM_ID_MAX)
#define CH_DOM_BUF  40           // packed space-separated domain tokens, e.g. "history code"
#define CH_TTL_MS   30000        // a manifest older than this is stale (peer gone / radio lost)

// Capability bits — a CLOSED allowlist; extend deliberately (the request side is gated in P3).
#define CH_CAP_ANIMA_L1   0x0001u   // offline L1 retrieval against this device's knowledge shard
#define CH_CAP_CLIPBOARD  0x0002u   // shared clipboard sink/source
#define CH_CAP_PRESENCE   0x0004u   // liveness / roster

typedef struct {
    char     id[CH_ID_MAX + 1];
    uint32_t caps;               // CH_CAP_* bitfield
    uint16_t free_kb;            // advertised free heap (the requester's tie-breaker)
    uint8_t  busy;               // 1 = serving something else; skip when routing
    char     dom[CH_DOM_BUF];    // space-separated domain tokens
    uint32_t dig;                // digest over this peer's shard set (domain@ver#hash;...) — see chorus_digest
    uint32_t last_ms;            // monotonic time of the last manifest (for TTL + LRU eviction)
    uint8_t  used;
} chorus_peer_t;

typedef struct {
    chorus_peer_t peers[CH_PEERS];
    char     self_id[CH_ID_MAX + 1];
    uint32_t self_caps;
    char     self_dom[CH_DOM_BUF];
    uint32_t self_dig;           // our own shard-set digest, advertised in the manifest
} chorus_ctx_t;

// One organ's reply to a routed query. NOTE: the wire never carries a query VECTOR — only the query
// TEXT goes out (the organ re-encodes with its resident encoder, zero new buffers) and only this
// small result comes back. `conf` is the L1 confidence (0..100, = cosine%); `dig` is the answering
// organ's shard-set digest (must match what it advertised — anti stale/swapped-shard).
typedef struct {
    char     src[CH_ID_MAX + 1];
    uint32_t dig;
    int      conf;
} chorus_answer_t;

void chorus_init(chorus_ctx_t *c, const char *self_id, uint32_t caps, const char *domains);

// Stable digest over a shard descriptor string, e.g. "history@2#a1b2;code@1#c3d4". Bind the advertised
// domains to concrete shard content+version so routing is DERIVED from a verifiable manifest rather
// than a free-text hint. (P2 = integrity; P3 adds HMAC authenticity under the swarm PSK.)
uint32_t chorus_digest(const char *shard_descriptor);

// Set our advertised shard-set digest (recompute with chorus_digest over our local shards).
void chorus_set_shard_digest(chorus_ctx_t *c, uint32_t dig);

// Fuse organ answers: among candidates whose src is a known, non-stale peer whose advertised digest
// MATCHES the answer's dig (rejects stale/swapped shards) and whose conf >= min_conf, pick the best
// (highest conf; tie -> lowest src). Writes the winner id into who and its index into *which.
// Returns 1 if a winner was chosen, 0 otherwise.
int chorus_fuse(const chorus_ctx_t *c, const chorus_answer_t *cands, int n, int min_conf,
                uint32_t now_ms, char *who, int wcap, int *which);

// Build our self-manifest JSON payload for nucleo_event_publish("chorus.cap", ...). free_kb/busy
// are sampled live at call time. Returns the length written, or -1 if it doesn't fit `cap`.
int  chorus_self_manifest(const chorus_ctx_t *c, uint16_t free_kb, uint8_t busy, char *out, int cap);

// Ingest a peer manifest (from an injected `chorus.cap` event). now_ms = monotonic clock.
// Returns 1 = upserted, 0 = ignored (own id / unparseable).
int  chorus_on_manifest(chorus_ctx_t *c, const char *src, const char *payload_json, uint32_t now_ms);

// Route: the best peer offering capability bit `cap` and (optionally) domain token `domain`, that
// is not busy and not stale at now_ms. Tie-break: most free_kb, then lowest id (deterministic).
// Writes the chosen id into out. Returns 1 if a peer was found, 0 if none.
int  chorus_route(const chorus_ctx_t *c, uint32_t cap, const char *domain, uint32_t now_ms,
                  char *out, int outcap);

// Drop stale peers (route already ignores them; this reclaims slots). Returns the live count.
int  chorus_gc(chorus_ctx_t *c, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
