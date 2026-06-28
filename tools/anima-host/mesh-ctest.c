// mesh-ctest.c — host gate for nucleo_mesh (the MESH gossip seam). Compiles the pure SANS-I/O core
// (firmware/components/nucleo_mesh/nucleo_mesh.c) and proves, on the PC, the invariants the ADR
// (docs/swarm-architecture.md) makes load-bearing:
//   1. only mesh.* / chorus.* topics are gossiped; everything else stays local;
//   2. a foreign event (bus src != NULL) is NEVER re-forwarded (structural loop prevention);
//   3. a locally-published mesh.* event is injected on the peer EXACTLY ONCE with src/topic/payload intact;
//   4. de-dup by (origin id, origin seq) — exact dups, reordered-within-window, and own-echo all rejected;
//   5. malformed frames are rejected with no inject;
//   6. under a lossy/duplicating/reordering channel, NO origin-seq is ever injected twice.
// Run via `npm run mesh:test`.
#include "nucleo_mesh.h"
#include <stdio.h>
#include <string.h>

// ---- deterministic PRNG (reproducible runs; no rand()) ----------------------
static uint32_t RNG = 0x12345678u;
static void     seed(uint32_t s) { RNG = s ? s : 1; }
static uint32_t rnd(void) { RNG ^= RNG << 13; RNG ^= RNG >> 17; RNG ^= RNG << 5; return RNG; }
static int      chance(int pct) { return pct > 0 && (int)(rnd() % 100) < pct; }

// ---- simulated radio channel (toward B) + inject log (at B) -----------------
#define QMAX 8192
#define SMAX 1024
typedef struct { int len; uint8_t buf[NM_MTU]; } frame_t;
static struct {
    frame_t q[QMAX]; int qn;
    int loss, dup, reorder;
    int  inject_calls;
    char last_src[NM_ID_MAX + 1], last_topic[NM_TOPIC_MAX], last_payload[NM_PAYLOAD_MAX];
    int  inj_count[SMAX];
} W;

static void reset(int loss, int dup, int reorder)
{
    memset(&W, 0, sizeof W);
    W.loss = loss; W.dup = dup; W.reorder = reorder;
}

static int a_send(void *u, const uint8_t *b, int n)        // A -> air (toward B)
{
    (void)u;
    if (chance(W.loss)) return 0;                          // dropped on the air; local send still ok
    if (W.qn < QMAX) { W.q[W.qn].len = n; memcpy(W.q[W.qn].buf, b, n); W.qn++; }
    if (chance(W.dup) && W.qn < QMAX) { W.q[W.qn] = W.q[W.qn - 1]; W.qn++; }
    return 0;
}
static int  noop_send(void *u, const uint8_t *b, int n) { (void)u; (void)b; (void)n; return 0; }
static void noop_inject(void *u, const char *s, const char *t, const char *p) { (void)u; (void)s; (void)t; (void)p; }
static void b_inject(void *u, const char *src, const char *topic, const char *pay)
{
    (void)u;
    W.inject_calls++;
    strncpy(W.last_src, src, NM_ID_MAX);          W.last_src[NM_ID_MAX] = 0;
    strncpy(W.last_topic, topic, NM_TOPIC_MAX - 1);  W.last_topic[NM_TOPIC_MAX - 1] = 0;
    strncpy(W.last_payload, pay, NM_PAYLOAD_MAX - 1); W.last_payload[NM_PAYLOAD_MAX - 1] = 0;
}
static const nmesh_io_t IOA = { NULL, a_send,    noop_inject };
static const nmesh_io_t IOB = { NULL, noop_send, b_inject };

static uint32_t fseq(const uint8_t *b) {
    return (uint32_t)b[4] | ((uint32_t)b[5] << 8) | ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24);
}
static void deliver_all(nmesh_ctx_t *B)
{
    while (W.qn > 0) {
        int idx = W.reorder ? (int)(rnd() % (uint32_t)W.qn) : 0;   // random pick == reorder
        frame_t f = W.q[idx]; W.q[idx] = W.q[--W.qn];
        if (nmesh_on_frame(B, f.buf, f.len) == 1) {
            uint32_t s = fseq(f.buf); if (s < SMAX) W.inj_count[s]++;
        }
    }
}

static int FAILS = 0;
static void check(const char *name, int cond, const char *detail)
{
    printf("  [%s] %s%s%s\n", cond ? "ok" : "FAIL", name, detail ? " — " : "", detail ? detail : "");
    if (!cond) FAILS++;
}

int main(void)
{
    seed(0xB0A7D);
    printf("nucleo_mesh host gate\n");

    // 1) topic gossip-eligibility (pure)
    printf("topic gating:\n");
    check("mesh.* gossips",     nmesh_topic_gossips("mesh.clipboard"), NULL);
    check("chorus.* gossips",   nmesh_topic_gossips("chorus.cap"), NULL);
    check("wifi.* local-only",  !nmesh_topic_gossips("wifi.status"), NULL);
    check("system.* local-only",!nmesh_topic_gossips("system.boot"), NULL);
    check("NULL safe",          !nmesh_topic_gossips(NULL), NULL);

    // 2) basic relay: one local mesh event injected once on the peer, intact
    printf("relay + payload integrity:\n");
    {
        reset(0, 0, 0);
        nmesh_ctx_t A, B; nmesh_init(&A, &IOA, "nucleo-A"); nmesh_init(&B, &IOB, "nucleo-B");
        nmesh_on_local_event(&A, 10, "mesh.clipboard", "{\"t\":\"hi\"}", NULL);
        check("A gossiped one frame", A.tx == 1, NULL);
        deliver_all(&B);
        check("B injected once", W.inject_calls == 1 && B.rx_fresh == 1, NULL);
        check("src preserved",   strcmp(W.last_src, "nucleo-A") == 0, W.last_src);
        check("topic preserved", strcmp(W.last_topic, "mesh.clipboard") == 0, W.last_topic);
        check("payload preserved", strcmp(W.last_payload, "{\"t\":\"hi\"}") == 0, W.last_payload);
    }

    // 3) non-gossip topic stays local
    printf("local-only topics:\n");
    {
        reset(0, 0, 0);
        nmesh_ctx_t A, B; nmesh_init(&A, &IOA, "nucleo-A"); nmesh_init(&B, &IOB, "nucleo-B");
        nmesh_on_local_event(&A, 11, "wifi.status", "{\"rssi\":-50}", NULL);
        check("not gossiped", A.tx == 0 && A.skipped_nontopic == 1 && W.qn == 0, NULL);
    }

    // 4) loop prevention: a foreign event (src set) is never re-forwarded
    printf("loop prevention:\n");
    {
        reset(0, 0, 0);
        nmesh_ctx_t A; nmesh_init(&A, &IOA, "nucleo-A");
        nmesh_on_local_event(&A, 12, "mesh.presence", "{}", "nucleo-C");  // arrived from a peer
        check("foreign not re-forwarded", A.tx == 0 && A.skipped_foreign == 1, NULL);
    }

    // 5) de-dup: exact duplicate frame
    printf("de-dup:\n");
    {
        reset(0, 0, 0);
        nmesh_ctx_t A, B; nmesh_init(&A, &IOA, "nucleo-A"); nmesh_init(&B, &IOB, "nucleo-B");
        nmesh_on_local_event(&A, 20, "mesh.x", "{\"v\":1}", NULL);
        if (W.qn > 0 && W.qn < QMAX) { W.q[W.qn] = W.q[W.qn - 1]; W.qn++; }   // duplicate it on the air
        deliver_all(&B);
        check("exact dup rejected", W.inject_calls == 1 && B.rx_fresh == 1 && B.rx_dup == 1, NULL);
    }

    // 6) reordered-within-window: distinct seqs each once; a re-sent old one is a dup
    {
        reset(0, 0, 1);
        nmesh_ctx_t A, B; nmesh_init(&A, &IOA, "nucleo-A"); nmesh_init(&B, &IOB, "nucleo-B");
        for (uint32_t s = 30; s <= 33; s++) nmesh_on_local_event(&A, s, "mesh.k", "{}", NULL);
        if (W.qn < QMAX) { W.q[W.qn] = W.q[1]; W.qn++; }    // re-send seq 31 (the 2nd enqueued)
        deliver_all(&B);
        int each_once = W.inj_count[30] == 1 && W.inj_count[31] == 1 && W.inj_count[32] == 1 && W.inj_count[33] == 1;
        check("4 distinct injected once each", W.inject_calls == 4 && B.rx_fresh == 4 && each_once, NULL);
        check("reordered old re-send is dup", B.rx_dup == 1, NULL);
    }

    // 6b) seal headroom: a frame must encode to <= NM_MTU - NM_TAG_RESERVE so the swarm auth tag fits
    printf("seal headroom:\n");
    {
        reset(0, 0, 0);
        nmesh_ctx_t A, B; nmesh_init(&A, &IOA, "nucleo-A"); nmesh_init(&B, &IOB, "nucleo-B");
        char topic[NM_TOPIC_MAX];                       // 47-char "mesh.xxx...": maximal topic
        memset(topic, 'x', NM_TOPIC_MAX - 1); topic[NM_TOPIC_MAX - 1] = 0;
        topic[0] = 'm'; topic[1] = 'e'; topic[2] = 's'; topic[3] = 'h'; topic[4] = '.';
        const int overhead = 11 + 8 + (NM_TOPIC_MAX - 1);   // hdr + id("nucleo-A"=8) + topic(47)
        char pay[NM_PAYLOAD_MAX];
        int over = (NM_MTU - NM_TAG_RESERVE) - overhead + 1;   // 1 byte past the sealable budget
        memset(pay, 'a', over); pay[over] = 0;
        nmesh_on_local_event(&A, 1, topic, pay, NULL);
        check("over-budget frame dropped (reserves tag)", A.tx == 0 && A.tx_toobig == 1, NULL);
        int fit = over - 1;
        memset(pay, 'a', fit); pay[fit] = 0;
        nmesh_on_local_event(&A, 2, topic, pay, NULL);
        char d[64]; snprintf(d, sizeof d, "len=%d <= %d", W.qn ? W.q[W.qn - 1].len : -1, NM_MTU - NM_TAG_RESERVE);
        check("max frame sent within headroom", A.tx == 1 && W.qn >= 1 && W.q[W.qn - 1].len <= NM_MTU - NM_TAG_RESERVE, d);
    }

    // 7) malformed frames
    printf("malformed:\n");
    {
        reset(0, 0, 0);
        nmesh_ctx_t B; nmesh_init(&B, &IOB, "nucleo-B");
        uint8_t too_short[4] = { 'N', 'M', 1, 0 };
        check("short frame -1", nmesh_on_frame(&B, too_short, 4) == -1, NULL);
        uint8_t bad_magic[12] = { 'X', 'M', 1, 0, 1, 0, 0, 0, 1, 'A', 1, 0 };
        check("bad magic -1", nmesh_on_frame(&B, bad_magic, 12) == -1, NULL);
        uint8_t bad_idlen[12] = { 'N', 'M', 1, 0, 5, 0, 0, 0, 30 /*>NM_ID_MAX*/, 'a', 1, 0 };
        check("bad id_len -1", nmesh_on_frame(&B, bad_idlen, 12) == -1, NULL);
        check("no inject on malformed", W.inject_calls == 0 && B.rx_bad == 3, NULL);
    }

    // 8) own broadcast echoed back is ignored
    printf("self-echo:\n");
    {
        reset(0, 0, 0);
        nmesh_ctx_t A, B; nmesh_init(&A, &IOA, "nucleo-B");  // A shares B's id
        nmesh_init(&B, &IOB, "nucleo-B");
        nmesh_on_local_event(&A, 40, "mesh.x", "{}", NULL);
        deliver_all(&B);
        check("self-echo not injected", W.inject_calls == 0 && B.rx_dup >= 1, NULL);
    }

    // 9) lossy / duplicating / reordering channel: never inject any origin-seq twice
    printf("lossy channel soak:\n");
    {
        reset(30, 15, 1);
        nmesh_ctx_t A, B; nmesh_init(&A, &IOA, "nucleo-A"); nmesh_init(&B, &IOB, "nucleo-B");
        const int N = 300;
        for (uint32_t s = 1; s <= (uint32_t)N; s++) nmesh_on_local_event(&A, s, "mesh.evt", "{\"n\":1}", NULL);
        deliver_all(&B);
        int dbl = 0, injected = 0;
        for (int s = 1; s <= N; s++) { if (W.inj_count[s] > 1) dbl++; if (W.inj_count[s] == 1) injected++; }
        char d[120];
        snprintf(d, sizeof d, "tx=%u injected=%d fresh=%u dup=%u (loss/dup/reorder)",
                 A.tx, injected, B.rx_fresh, B.rx_dup);
        check("A emitted every event",        A.tx == (uint32_t)N, NULL);
        check("no origin-seq injected twice", dbl == 0, d);
        check("fresh count == injected",      (int)B.rx_fresh == injected, NULL);
    }

    printf(FAILS ? "\nRESULT: %d FAILED\n" : "\nRESULT: all passed\n", FAILS);
    return FAILS ? 1 : 0;
}
