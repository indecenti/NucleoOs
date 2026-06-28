// nucleo_mesh — the MESH seam: gossip the local event bus across ESP-NOW peers.
//
// This is the PURE, SANS-I/O core (like nucleo_link's protocol core): it never touches the radio
// or the event bus directly — it drives everything through the nmesh_io_t callbacks, so it is
// zero-heap, RAM-bounded, and fully host-testable on a PC (npm run mesh:test, mesh-ctest.c).
//
// Model ("a shared bulletin board, not a boss"): a device GOSSIPS its locally-published mesh.* /
// chorus.* events to peers; an incoming gossip frame is de-duplicated by (origin-id, origin-seq)
// and, if fresh, INJECTED onto the local bus so apps see it exactly like a local event — no
// swarm-aware app code needed. Loop prevention is structural: a foreign event (one that arrived
// via inject, so its bus `src` is non-NULL) is NEVER re-forwarded.
//
// Wire frame (one ESP-NOW datagram, <= NM_MTU bytes, little-endian):
//   "NM" ver flags | seq(u32) | id_len id[..] | topic_len topic[..] | pay_len payload[..]
// Larger payloads are simply not gossiped (tx_toobig++) — mesh.* events are small by design;
// CHORUS manifests fit in <=208 B. Chunking, if ever needed, belongs to a later layer.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NM_VER          1
#define NM_MTU          250          // ESP-NOW max payload (matches NLINK_MTU)
#define NM_TAG_RESERVE  8            // tail bytes reserved for the optional swarm HMAC tag (== SW_TAG_LEN):
                                     // a frame is encoded to <= NM_MTU - NM_TAG_RESERVE so sealing never
                                     // overruns the 250-byte radio MTU (silent gossip loss). See nucleo_swarm_sec.h.
#define NM_ID_MAX       24           // device id length in a gossip frame
#define NM_TOPIC_MAX    48           // mirrors NUCLEO_EVENT_TOPIC_MAX
#define NM_PAYLOAD_MAX  208          // mirrors NUCLEO_EVENT_PAYLOAD_MAX
#define NM_PEERS        8            // de-dup table size (mirrors nucleo_link peer ceiling)

// I/O seam the host implements (radio + local bus). All sized for stack/static allocation.
typedef struct {
    void *user;
    // Send one framed gossip datagram to the air (broadcast). Return 0 on success.
    int  (*send)(void *user, const uint8_t *buf, int len);
    // Inject a foreign event onto the LOCAL event bus (mirrors nucleo_event_inject).
    void (*inject)(void *user, const char *src, const char *topic, const char *payload);
} nmesh_io_t;

// Per-origin de-dup state: a 32-entry sliding window above/below the high-water seq, so reordered
// frames within the window are accepted once and exact duplicates are rejected (mirrors the
// recv_base/recv_mask idea in nucleo_link). Fully public so it can live in .bss (no heap).
typedef struct {
    char     id[NM_ID_MAX + 1];
    uint32_t base;       // highest origin-seq seen from this peer
    uint32_t mask;       // bit i set => (base-1-i) already seen
    uint32_t lru;        // for eviction when the table is full
    uint8_t  used;
} nmesh_peer_t;

typedef struct {
    const nmesh_io_t *io;
    char     self_id[NM_ID_MAX + 1];
    nmesh_peer_t peers[NM_PEERS];
    uint32_t lru_clock;
    // counters — telemetry + host-test assertions
    uint32_t tx, tx_toobig, rx_fresh, rx_dup, rx_bad, skipped_foreign, skipped_nontopic;
} nmesh_ctx_t;

void nmesh_init(nmesh_ctx_t *c, const nmesh_io_t *io, const char *self_id);

// True iff a topic is gossip-eligible (mesh.* / chorus.*). Everything else stays strictly local.
bool nmesh_topic_gossips(const char *topic);

// Call from the local event-bus sink. Gossips the event iff it is gossip-eligible AND
// locally-originated (src == NULL/empty). Foreign events (src set) are never re-forwarded.
void nmesh_on_local_event(nmesh_ctx_t *c, uint32_t seq, const char *topic,
                          const char *payload, const char *src);

// Call when a gossip datagram arrives from the radio. De-dups by (origin-id, origin-seq) and, if
// fresh, calls io->inject(). Returns 1 = fresh (injected), 0 = duplicate/own-echo, -1 = malformed.
int nmesh_on_frame(nmesh_ctx_t *c, const uint8_t *buf, int len);

#ifdef __cplusplus
}
#endif
