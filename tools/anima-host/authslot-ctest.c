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
//   5. The compact NVS record round-trips the PIN and every session; cut to fit, it keeps the sessions used
//      most recently; it never persists a free slot or a malformed token, and rejects a corrupt record.
//   6. Its save policy, against a fake NVS with the real rules (update = old + new room, a failed write may
//      damage the old copy): the stored sessions are always a SUBSET of the live ones — no evicted or revoked
//      session comes back after a reboot — and a full NVS keeps the most recently used that fit.
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

// ---- section 6: a fake NVS that follows the real rules, and a model of nucleo_auth's table ----
// An update is written BEFORE the old version is dropped (room for both); a write that runs out of room may
// damage the old copy (NVS's multi-page rollback); the old-format text takes room until it is erased.
#define NEED(len) ((long)(3 + ((len) + 31) / 32) * 32)
static struct { long cap, legacy; size_t len; bool damaged; int fail_err; uint8_t data[AUTH_REC_SIZE(N)]; } F;
static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }
static int fput(void *c, const uint8_t *r, size_t len)
{
    (void)c;
    if (F.fail_err) return AUTH_PUT_ERROR;
    long used = F.legacy + (F.len ? NEED(F.len) : 0);
    if (used + NEED(len) > F.cap) { if (F.len && (rnd() & 1)) F.damaged = true; return AUTH_PUT_NOSPACE; }
    memcpy(F.data, r, len); F.len = len; F.damaged = false;
    return AUTH_PUT_OK;
}
static void fwipe(void *c) { (void)c; F.len = 0; F.legacy = 0; F.damaged = false; }
static long froom(void *c) { (void)c; return F.cap - F.legacy - (F.len ? NEED(F.len) : 0); }
static const auth_store_t FS = { fput, fwipe, froom, NULL };

static char mt[N][AUTH_TOK_HEX + 1]; static int64_t ms[N]; static uint32_t ml[N];
static uint8_t mbuf[AUTH_REC_SIZE(N)];
static uint32_t mserial = 1; static int mlive; static bool mremoved;
static void m_reset(void) { memset(mt, 0, sizeof mt); memset(ms, 0, sizeof ms); memset(ml, 0, sizeof ml); mlive = 0; mremoved = false; }
static void m_pair(void)                                     // mint_token: a free slot, else evict the LRU
{
    bool u[N]; for (int i = 0; i < N; i++) u[i] = mt[i][0] != '\0';
    int sl = auth_pick_slot(u, ml, N);
    if (u[sl]) mremoved = true; else mlive++;
    mserial++;
    snprintf(mt[sl], sizeof mt[sl], "%08x%040x", (unsigned)mserial, 0u);
    ml[sl] = mserial; ms[sl] = 1760000000LL + mserial;
}
static void m_drop(int i) { if (mt[i][0]) { mt[i][0] = '\0'; ml[i] = 0; mlive--; mremoved = true; } }   // revoke / TTL
static int m_save(void)
{
    int k = auth_rec_save(&FS, mbuf, "123456", (const char (*)[AUTH_TOK_HEX + 1])mt, ms, ml, N, mlive, mremoved);
    if (k >= 0) mremoved = false;
    return k;
}
static int stored(char (*bk)[AUTH_TOK_HEX + 1])               // decode what the fake holds (-1: damaged/corrupt)
{
    static int64_t bs[N]; char pn[7];
    if (!F.len) return 0;
    if (F.damaged) return -1;
    return auth_rec_decode(F.data, F.len, pn, bk, bs, N);
}
static bool stored_is_subset(void)                           // every stored session is live right now
{
    static char bk[N][AUTH_TOK_HEX + 1];
    int c = stored(bk);
    if (c < 0) return false;                                  // a damaged copy left in place = a reboot surprise
    for (int k = 0; k < c; k++) {
        bool found = false;
        for (int i = 0; i < N && !found; i++) found = mt[i][0] && !strcmp(mt[i], bk[k]);
        if (!found) return false;
    }
    return true;
}

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

    // 5. the compact NVS record (the only tier on a unit without /cfg)
    {
        static char tk[N][AUTH_TOK_HEX + 1], back[N][AUTH_TOK_HEX + 1];
        static int64_t sn[N], sb[N]; static uint32_t lr[N]; static uint8_t rec[AUTH_REC_SIZE(N)];
        char pin[7];
        memset(tk, 0, sizeof tk); memset(lr, 0, sizeof lr);
        for (int i = 0; i < N; i++) {
            for (int c = 0; c < AUTH_TOK_HEX; c++) tk[i][c] = "0123456789abcdef"[(i * 7 + c * 3) & 15];
            tk[i][0] = "0123456789abcdef"[i & 15]; tk[i][1] = "0123456789abcdef"[i >> 4];   // unique per slot
            sn[i] = 1760000000LL + i; lr[i] = (uint32_t)(i + 1);
        }
        size_t len = auth_rec_encode(rec, sizeof rec, "123456", tk, sn, lr, N, N);
        CHECK(len == AUTH_REC_SIZE(N) && len == 904, "a full table encodes in 904 bytes (the JSON text was ~2.3 KB)");
        int got = auth_rec_decode(rec, len, pin, back, sb, N);
        bool same = got == N && !strcmp(pin, "123456");
        for (int k = 0; same && k < N; k++) {                     // decoded oldest first: here, the same slots
            same = !strcmp(back[k], tk[k]) && sb[k] == sn[k];
        }
        CHECK(same, "round trip keeps the PIN, every token and its last-seen");
        CHECK(rec[AUTH_REC_HDR] == 0xf1 && rec[AUTH_REC_SIZE(N - 1)] == 0x00,     // tk[31] first, tk[0] last
              "the record lists the most recently used first");

        lr[3] = 1000;                                             // slot 3 is the browser in use right now
        len = auth_rec_encode(rec, sizeof rec, "123456", tk, sn, lr, N, 4);
        got = auth_rec_decode(rec, len, pin, back, sb, N);
        CHECK(got == 4 && !strcmp(back[3], tk[3]) && !strcmp(back[2], tk[N - 1]) && !strcmp(back[0], tk[N - 3]),
              "a record cut to fit keeps the sessions used most recently");
        CHECK(back[4][0] == '\0', "slots past the record are cleared on decode");

        len = auth_rec_encode(rec, sizeof rec, "123456", tk, sn, lr, N, 0);
        got = auth_rec_decode(rec, len, pin, back, sb, N);
        CHECK(len == AUTH_REC_HDR && got == 0 && !strcmp(pin, "123456"), "keep 0 still persists the PIN");

        tk[5][7] = 'z'; tk[9][0] = '\0';                          // a malformed token and a free slot
        len = auth_rec_encode(rec, sizeof rec, "", tk, sn, lr, N, N);
        got = auth_rec_decode(rec, len, pin, back, sb, N);
        bool absent = true;
        for (int k = 0; k < got; k++) absent = absent && strcmp(back[k], tk[5]) && back[k][0];
        CHECK(got == N - 2 && absent, "free slots and malformed tokens are never persisted");
        CHECK(pin[0] == '\0', "no PIN in RAM -> none in the record (init mints a fresh one)");

        sn[0] = -5; sn[1] = 1LL << 40;                            // out-of-range clocks persist as never-seen
        tk[5][7] = 'a'; tk[9][0] = 'f';
        len = auth_rec_encode(rec, sizeof rec, "000042", tk, sn, lr, N, N);
        got = auth_rec_decode(rec, len, pin, back, sb, N);
        int zeros = 0;
        for (int k = 0; k < got; k++) if (!strcmp(back[k], tk[0]) || !strcmp(back[k], tk[1])) zeros += sb[k] == 0;
        CHECK(zeros == 2 && !strcmp(pin, "000042"), "a last-seen outside u32 is stored as 0, never wrapped");

        CHECK(auth_rec_decode(rec, len - 1, pin, back, sb, N) == -1, "a truncated record is rejected");
        rec[0] = 2;
        CHECK(auth_rec_decode(rec, len, pin, back, sb, N) == -1 && back[0][0] == '\0', "an unknown version is rejected and clears the table");
        rec[0] = AUTH_REC_VER; rec[3] = 'x';
        CHECK(auth_rec_decode(rec, len, pin, back, sb, N) == -1, "a PIN that is not six digits is rejected");
        CHECK(auth_rec_encode(rec, AUTH_REC_SIZE(2), "123456", tk, sn, lr, N, N) == 0, "a short buffer refuses, never overflows");

        // A clock that was never set: every last-seen is 0. After a reboot the session used last must still
        // rank newest (a new pairing evicts the least recently used), and a re-encode must not reorder.
        for (int i = 0; i < N; i++) { sn[i] = 0; lr[i] = (uint32_t)(i + 1); }
        lr[7] = 5000;                                             // slot 7: the browser in use
        len = auth_rec_encode(rec, sizeof rec, "123456", tk, sn, lr, N, N);
        got = auth_rec_decode(rec, len, pin, back, sb, N);
        bool bu[N]; uint32_t bl[N];
        for (int i = 0; i < N; i++) bu[i] = back[i][0] != '\0';
        auth_rank_seen(bu, sb, bl, N);
        int newest = 0;
        for (int i = 1; i < N; i++) if (bl[i] > bl[newest]) newest = i;
        CHECK(got == N && !strcmp(back[newest], tk[7]), "equal last-seen: the session used last still ranks newest after a reboot");
        CHECK(auth_pick_slot(bu, bl, N) != newest, "and a new pairing does not evict it");
        static uint8_t rec2[AUTH_REC_SIZE(N)];
        size_t len2 = auth_rec_encode(rec2, sizeof rec2, "123456", (const char (*)[AUTH_TOK_HEX + 1])back, sb, bl, N, N);
        CHECK(len2 == len && !memcmp(rec, rec2, len), "a reloaded table re-encodes to the same bytes (no rewrite per boot)");
    }

    // 6. the save policy against the fake NVS: the stored sessions are always a SUBSET of the live ones
    {
        static char bk[N][AUTH_TOK_HEX + 1];
        m_reset(); memset(&F, 0, sizeof F); F.cap = 4096;
        for (int i = 0; i < 20; i++) m_pair();
        CHECK(m_save() == 20 && stored_is_subset(), "room to spare: every session is stored");

        F.legacy = 4096 - NEED(F.len) - 64;                       // the old-format text fills NVS
        m_pair();
        CHECK(m_save() == 21 && F.legacy == 0 && stored_is_subset(), "old text in the way: dropped, then the full record fits");

        memset(&F, 0, sizeof F); F.cap = NEED(AUTH_REC_SIZE(6));        // nearly full: room for exactly six sessions
        int inuse = 0; while (!mt[inuse][0]) inuse++;
        ml[inuse] = mserial + 100;                                // the browser in use (paired long ago)
        int k = m_save(), c = stored(bk);
        bool has = false;
        for (int i = 0; i < c; i++) has = has || !strcmp(bk[i], mt[inuse]);
        CHECK(k == 6 && c == 6 && has && stored_is_subset(), "NVS nearly full: the most recently used that fit, the one in use among them");

        F.cap = 64; F.len = 0;
        CHECK(m_save() == -1 && F.len == 0, "no room even for the PIN: -1 and no stale record");

        memset(&F, 0, sizeof F); F.cap = 4096;
        CHECK(m_save() == mlive, "baseline save");
        m_pair(); F.fail_err = 1;
        CHECK(m_save() == -1 && F.len > 0 && stored_is_subset(), "I/O error after an addition: the old record (a subset) stays");
        m_drop(2);
        CHECK(m_save() == -1 && F.len == 0, "I/O error after a removal: the old record is erased (fail-closed)");
        F.fail_err = 0;
        CHECK(m_save() == mlive, "the next save recovers");
        m_drop(4);
        CHECK(auth_rec_save(&FS, NULL, "123456", (const char (*)[AUTH_TOK_HEX + 1])mt, ms, ml, N, mlive, true) == -1 && F.len == 0,
              "out of memory after a removal: the old record is erased");

        // random pairings, revocations, space pressure, old text and I/O errors
        m_reset(); memset(&F, 0, sizeof F); F.cap = 2048;
        bool ok = true; int saved = 0, shed = 0;
        for (int step = 0; step < 20000 && ok; step++) {
            uint32_t op = rnd() % 100;
            if (op < 55) m_pair();
            else if (op < 75) m_drop((int)(rnd() % N));
            else if (op < 85) F.cap = 256 + (long)(rnd() % 1600);
            else if (op < 92) F.legacy = (long)(rnd() % 1200);
            else { int i = (int)(rnd() % N); if (mt[i][0]) ml[i] = ++mserial; }
            F.fail_err = (rnd() % 23) == 0;
            int kk = m_save();
            saved += kk >= 0; shed += kk >= 0 && kk < mlive;
            ok = stored_is_subset() && kk <= mlive;
        }
        CHECK(ok && saved > 1000 && shed > 100, "20000 random steps: the stored sessions are always a subset of the live ones");
    }

    printf("authslot: %d/%d passed\n", g_total - g_fail, g_total);
    return g_fail ? 1 : 0;
}
