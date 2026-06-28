// chorus-ctest.c — host gate for nucleo_chorus (the CHORUS capability directory) running ON TOP of
// nucleo_mesh, exactly as on-device. A 3-node broadcast swarm (A,B,C) each gossips its self-manifest
// as a `chorus.cap` event; the MESH layer carries it over a lossy/duplicating channel; each node
// ingests peers' manifests into its directory. Proves the ADR (docs/swarm-architecture.md):
//   1. manifest CONVERGENCE through loss — every node learns the others' caps/domains/free;
//   2. content ROUTING by capability + domain, tie-broken by advertised free_kb;
//   3. BUSY-reroute — a peer that flips busy is skipped;
//   4. TTL age-out — a peer that stops refreshing becomes unroutable.
// Plus unit checks for the manifest codec / JSON robustness / domain token matching.
// Run via `npm run chorus:test`.
#include "nucleo_mesh.h"
#include "nucleo_chorus.h"
#include <stdio.h>
#include <string.h>

// ---- deterministic PRNG -----------------------------------------------------
static uint32_t RNG = 0x12345678u;
static void     seed(uint32_t s) { RNG = s ? s : 1; }
static uint32_t rnd(void) { RNG ^= RNG << 13; RNG ^= RNG >> 17; RNG ^= RNG << 5; return RNG; }
static int      chance(int pct) { return pct > 0 && (int)(rnd() % 100) < pct; }

static int FAILS = 0;
static void check(const char *name, int cond, const char *detail)
{
    printf("  [%s] %s%s%s\n", cond ? "ok" : "FAIL", name, detail ? " — " : "", detail ? detail : "");
    if (!cond) FAILS++;
}

// ---- 3-node broadcast swarm -------------------------------------------------
#define NODES 3
typedef struct { int dest; int len; uint8_t buf[NM_MTU]; } air_t;
#define AIRMAX 8192

static struct {
    air_t air[AIRMAX]; int an;
    int   loss, dup;
} CH;

typedef struct {
    nmesh_ctx_t  mesh;
    chorus_ctx_t ch;
    char         id[CH_ID_MAX + 1];
    uint16_t     free_kb;
    uint8_t      busy;
    uint32_t     seq;
    int          idx;
} node_t;
static node_t NODE[NODES];
static uint32_t NOWMS;

// mesh send: broadcast this frame to every OTHER node, with per-link loss/dup.
static int air_send(void *user, const uint8_t *b, int n)
{
    node_t *from = user;
    for (int j = 0; j < NODES; j++) {
        if (j == from->idx) continue;
        if (chance(CH.loss)) continue;
        if (CH.an < AIRMAX) { CH.air[CH.an].dest = j; CH.air[CH.an].len = n; memcpy(CH.air[CH.an].buf, b, n); CH.an++; }
        if (chance(CH.dup) && CH.an < AIRMAX) { CH.air[CH.an] = CH.air[CH.an - 1]; CH.an++; }
    }
    return 0;
}
// mesh inject: a foreign event landed on this node's bus; feed chorus.* into the directory.
static void air_inject(void *user, const char *src, const char *topic, const char *payload)
{
    node_t *n = user;
    if (strcmp(topic, "chorus.cap") == 0) chorus_on_manifest(&n->ch, src, payload, NOWMS);
}

static nmesh_io_t IO[NODES];   // one io per node (user = &NODE[i])

static void node_setup(int i, const char *id, uint32_t caps, const char *dom, uint16_t free_kb)
{
    node_t *n = &NODE[i];
    memset(n, 0, sizeof(*n));
    n->idx = i; n->free_kb = free_kb; n->busy = 0;
    strncpy(n->id, id, CH_ID_MAX); n->id[CH_ID_MAX] = 0;
    IO[i].user = n; IO[i].send = air_send; IO[i].inject = air_inject;
    nmesh_init(&n->mesh, &IO[i], id);
    chorus_init(&n->ch, id, caps, dom);
}

// One gossip round: each node emits its self-manifest, then the air is fully delivered.
static void gossip_round(void)
{
    for (int i = 0; i < NODES; i++) {
        char pay[160];
        if (chorus_self_manifest(&NODE[i].ch, NODE[i].free_kb, NODE[i].busy, pay, sizeof pay) < 0) continue;
        nmesh_on_local_event(&NODE[i].mesh, ++NODE[i].seq, "chorus.cap", pay, NULL);
    }
    while (CH.an > 0) {
        int idx = (int)(rnd() % (uint32_t)CH.an);       // random delivery order
        air_t a = CH.air[idx]; CH.air[idx] = CH.air[--CH.an];
        nmesh_on_frame(&NODE[a.dest].mesh, a.buf, a.len);
    }
    NOWMS += 1000;                                       // 1s between rounds
}

int main(void)
{
    seed(0xC0FFEE);
    printf("nucleo_chorus host gate\n");

    // ---- unit: manifest codec + JSON robustness + domain matching ----
    printf("manifest codec:\n");
    {
        chorus_ctx_t c; chorus_init(&c, "nucleo-X", CH_CAP_ANIMA_L1 | CH_CAP_CLIPBOARD, "history code");
        char m[160]; int n = chorus_self_manifest(&c, 27, 0, m, sizeof m);
        check("manifest builds", n > 0, m);
        chorus_ctx_t d; chorus_init(&d, "nucleo-Y", 0, "");
        int up = chorus_on_manifest(&d, "nucleo-X", m, 5000);
        int got = 0; char who[CH_ID_MAX + 1];
        got = chorus_route(&d, CH_CAP_ANIMA_L1, "history", 5000, who, sizeof who);
        check("round-trips + routes", up == 1 && got == 1 && strcmp(who, "nucleo-X") == 0, who);
        check("own manifest ignored", chorus_on_manifest(&d, "nucleo-Y", m, 5000) == 0, NULL);
        check("garbage payload safe", chorus_on_manifest(&d, "nucleo-Z", "not json", 5000) == 1, NULL);
    }

    // ---- P2: shard digest (derived routing) + answer fusion by score ----
    // The wire carries only QUERY TEXT out and a small (src,dig,conf) result back — never a vector.
    printf("shard digest + answer fusion:\n");
    {
        uint32_t d1 = chorus_digest("history@2#a1b2;code@1#c3d4");
        uint32_t d1b = chorus_digest("history@2#a1b2;code@1#c3d4");
        uint32_t d3 = chorus_digest("history@3#a1b2;code@1#c3d4");   // version bump
        check("digest stable",    d1 == d1b, NULL);
        check("digest sensitive", d1 != d3,  NULL);

        chorus_ctx_t M; chorus_init(&M, "nucleo-A", CH_CAP_ANIMA_L1, "");
        chorus_ctx_t Bm; chorus_init(&Bm, "nucleo-B", CH_CAP_ANIMA_L1, "history"); chorus_set_shard_digest(&Bm, d1);
        chorus_ctx_t Cm; chorus_init(&Cm, "nucleo-C", CH_CAP_ANIMA_L1, "history"); chorus_set_shard_digest(&Cm, d3);
        char mb[160], mc[160];
        chorus_self_manifest(&Bm, 18, 0, mb, sizeof mb);
        chorus_self_manifest(&Cm, 30, 0, mc, sizeof mc);
        chorus_on_manifest(&M, "nucleo-B", mb, 1000);
        chorus_on_manifest(&M, "nucleo-C", mc, 1000);

        char who[CH_ID_MAX + 1]; int which = -1;
        chorus_answer_t a1[2] = { { "nucleo-B", d1, 86 }, { "nucleo-C", d3, 91 } };
        check("fuse -> highest conf (C)", chorus_fuse(&M, a1, 2, 80, 1000, who, sizeof who, &which) && !strcmp(who, "nucleo-C") && which == 1, who);

        chorus_answer_t a2[2] = { { "nucleo-B", d1, 86 }, { "nucleo-C", 0xDEADBEEFu, 91 } }; // C's dig != advertised
        check("fuse rejects swapped-shard -> B", chorus_fuse(&M, a2, 2, 80, 1000, who, sizeof who, &which) && !strcmp(who, "nucleo-B"), who);

        chorus_answer_t a3[2] = { { "nucleo-B", d1, 70 }, { "nucleo-C", d3, 60 } };
        check("fuse floor: all below -> none", chorus_fuse(&M, a3, 2, 85, 1000, who, sizeof who, &which) == 0, NULL);

        chorus_answer_t a4[1] = { { "nucleo-Z", 123, 99 } };
        check("fuse ignores unknown organ", chorus_fuse(&M, a4, 1, 80, 1000, who, sizeof who, &which) == 0, NULL);
    }

    // ---- convergence + routing over a lossy channel ----
    printf("3-node convergence (loss=25%% dup=10%%):\n");
    CH.an = 0; CH.loss = 25; CH.dup = 10; NOWMS = 10000;
    node_setup(0, "nucleo-A", CH_CAP_ANIMA_L1,                 "",             20);  // requester
    node_setup(1, "nucleo-B", CH_CAP_ANIMA_L1 | CH_CAP_CLIPBOARD, "history art",  18);
    node_setup(2, "nucleo-C", CH_CAP_ANIMA_L1,                 "history code", 30);
    for (int r = 0; r < 12; r++) gossip_round();

    chorus_ctx_t *A = &NODE[0].ch;
    char who[CH_ID_MAX + 1];
    check("A knows 2 peers", chorus_gc(A, NOWMS) == 2, NULL);
    {
        // B/C learned from A's perspective
        int okB = 0, okC = 0;
        for (int i = 0; i < CH_PEERS; i++) {
            if (!A->peers[i].used) continue;
            if (!strcmp(A->peers[i].id, "nucleo-B")) okB = A->peers[i].caps == (CH_CAP_ANIMA_L1 | CH_CAP_CLIPBOARD) && A->peers[i].free_kb == 18;
            if (!strcmp(A->peers[i].id, "nucleo-C")) okC = A->peers[i].free_kb == 30;
        }
        check("B manifest converged (caps+free)", okB, NULL);
        check("C manifest converged (free=30)", okC, NULL);
    }
    check("route L1+history -> C (more free)", chorus_route(A, CH_CAP_ANIMA_L1, "history", NOWMS, who, sizeof who) && !strcmp(who, "nucleo-C"), who);
    check("route L1+art -> B (only holder)",   chorus_route(A, CH_CAP_ANIMA_L1, "art",     NOWMS, who, sizeof who) && !strcmp(who, "nucleo-B"), who);
    check("route L1+code -> C (only holder)",  chorus_route(A, CH_CAP_ANIMA_L1, "code",    NOWMS, who, sizeof who) && !strcmp(who, "nucleo-C"), who);
    check("route L1+science -> none",          chorus_route(A, CH_CAP_ANIMA_L1, "science", NOWMS, who, sizeof who) == 0, NULL);
    check("route clipboard -> B (domainless)", chorus_route(A, CH_CAP_CLIPBOARD, NULL,     NOWMS, who, sizeof who) && !strcmp(who, "nucleo-B"), who);

    // ---- busy-reroute: C goes busy, history must fall back to B ----
    printf("busy-reroute:\n");
    NODE[2].busy = 1;
    for (int r = 0; r < 6; r++) gossip_round();
    check("busy C skipped -> B", chorus_route(A, CH_CAP_ANIMA_L1, "history", NOWMS, who, sizeof who) && !strcmp(who, "nucleo-B"), who);

    // ---- TTL: stop all gossip, advance past the window -> directory goes cold ----
    printf("TTL age-out:\n");
    NOWMS += CH_TTL_MS + 5000;
    check("all stale -> no route", chorus_route(A, CH_CAP_ANIMA_L1, "history", NOWMS, who, sizeof who) == 0, NULL);
    check("gc reclaims slots", chorus_gc(A, NOWMS) == 0, NULL);

    printf(FAILS ? "\nRESULT: %d FAILED\n" : "\nRESULT: all passed\n", FAILS);
    return FAILS ? 1 : 0;
}
