// Host gate for the pairing-session table: compile the REAL slot policy
// (firmware/components/nucleo_auth/auth_slots.c) and prove its invariants on the PC.
// Wired as `npm run authslot:test` (tools/anima-host/authslot-check.mjs).
//
// Invariants under test:
//   1. A free slot is always taken first — nothing is evicted while there is room.
//   2. A full table evicts the least recently used session, never the one in use.
//   3. The regression: a browser in use survives any number of CLI pairings, including right after a
//      reboot (the old ring restarted its head at 0 and could evict the live desktop session).
//   4. After a reboot, recency is rebuilt from the persisted "last seen": oldest first, never-seen (0)
//      oldest of all, ties by slot order; free slots rank 0.
#include "auth_slots.h"
#include <stdio.h>
#include <string.h>

#define N 32
static int g_fail = 0, g_total = 0;
#define CHECK(cond, name) do { g_total++; if (!(cond)) { g_fail++; printf("FAIL %-60s\n", name); } } while (0)

// A tiny model of nucleo_auth's table driven by the real policy.
static bool used[N]; static uint32_t lru[N]; static int64_t seen[N]; static uint32_t tick; static int owner[N];
static void reset(void) { memset(used, 0, sizeof used); memset(lru, 0, sizeof lru); memset(seen, 0, sizeof seen); memset(owner, 0, sizeof owner); tick = 0; }
static int pair(int who, int64_t now) { int s = auth_pick_slot(used, lru, N); used[s] = true; lru[s] = ++tick; seen[s] = now; owner[s] = who; return s; }
static void touch(int s) { lru[s] = ++tick; }
static bool alive(int who) { for (int i = 0; i < N; i++) if (used[i] && owner[i] == who) return true; return false; }
static void reboot(void) { tick = auth_rank_seen(used, seen, lru, N); }

int main(void)
{
    // 1. free slots first
    reset();
    for (int i = 0; i < N; i++) CHECK(pair(100 + i, 1000 + i) == i, "empty table fills slots in order");
    reset();
    for (int i = 0; i < N; i++) pair(100 + i, 1000 + i);
    used[7] = false; lru[7] = 0;                                   // a session expired / was revoked
    CHECK(pair(999, 5000) == 7, "a freed slot is reused before any eviction");
    { bool fu[2] = { true, false }; uint32_t fl[2] = { 1, 5 };      // free slot with a stale recency stamp
      CHECK(auth_pick_slot(fu, fl, 2) == 1, "a free slot wins even over a lower-recency live session"); }

    // 2. full table -> least recently used
    reset();
    for (int i = 0; i < N; i++) pair(100 + i, 1000 + i);
    touch(0);                                                      // slot 0 (the oldest pairing) is in use now
    CHECK(pair(999, 5000) == 1, "full table evicts the LRU, not the oldest-paired in-use session");
    CHECK(alive(100), "the in-use session survives the eviction");

    // 3. the regression: the live browser survives a stream of CLI pairings, before and after a reboot
    reset();
    for (int i = 0; i < N - 1; i++) pair(100 + i, 1000 + i);       // 31 stale CLI sessions
    int br = pair(1, 2000);                                        // the desktop browser pairs last
    for (int k = 0; k < 200; k++) { touch(br); pair(500 + k, 3000 + k); }
    CHECK(alive(1), "browser in use survives 200 CLI pairings");
    // reboot with the browser "last seen" persisted as the newest day, then pair again at once
    reset();
    for (int i = 0; i < N; i++) { used[i] = true; owner[i] = 100 + i; seen[i] = 1000 + i; }
    owner[0] = 1; seen[0] = 9000;                                  // browser sits in slot 0 (the old ring's head after reboot)
    reboot();
    CHECK(pair(999, 9100) != 0 && alive(1), "after reboot a pairing does not evict slot 0's live browser");
    CHECK(!alive(101), "after reboot the oldest-seen session is the one evicted");

    // 4. rank from persisted last-seen
    reset();
    bool u[4] = { true, true, false, true }; int64_t s[4] = { 500, 0, 0, 500 }; uint32_t l[4];
    uint32_t k = auth_rank_seen(u, s, l, 4);
    CHECK(k == 3, "rank returns the number of live sessions");
    CHECK(l[1] == 1, "never-seen (0) ranks oldest");
    CHECK(l[0] == 2 && l[3] == 3, "equal last-seen: ties broken by slot order");
    CHECK(l[2] == 0, "free slot ranks 0");
    CHECK(auth_pick_slot(u, l, 4) == 2, "with a free slot present it is picked over the LRU");
    u[2] = true; l[2] = 4;
    CHECK(auth_pick_slot(u, l, 4) == 1, "full: the never-seen session is evicted first");

    printf("authslot: %d/%d passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
