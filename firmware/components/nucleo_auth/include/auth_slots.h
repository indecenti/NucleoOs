// Session-slot policy for nucleo_auth: which slot a newly paired session takes, how recency is
// rebuilt after a reboot, and the compact record kept in NVS. Pure C (no IDF) so
// tools/anima-host/authslot-check.mjs compiles and tests the real code on the PC.
//
// A new session takes a FREE slot when there is one; only a full table evicts, and then the session
// used LEAST RECENTLY — never the one a browser is using right now. (The old ring wrote at a head index
// that restarted at 0 after every reboot, so one pairing from a CLI tool could evict the live desktop
// session while 31 stale ones survived.)
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// used[i]: slot i holds a session. lru[i]: recency stamp (larger = used more recently).
// Returns the slot to write: the first free one, else the least recently used. n must be > 0.
int auth_pick_slot(const bool *used, const uint32_t *lru, int n);

// After a reboot only the persisted wall-clock "last seen" (day granularity; 0 = never seen with a set
// clock) survives. Rank the used slots by it — oldest -> 1 ... newest -> k, ties broken by slot order —
// and write the ranks into lru (free slots get 0). Returns k, the next recency stamp base.
uint32_t auth_rank_seen(const bool *used, const int64_t *seen, uint32_t *lru, int n);

// ---- compact NVS record (the fallback tier when the unit has no /cfg partition) ----
// On a unit installed by M5Launcher there is no /cfg: the pairing state lives in NVS alone — a 20 KB
// partition shared with the Launcher, PHY calibration and the Wi-Fi config. The old copy there was the
// auth.json text (~72 B per session, ~2.3 KB full), and once NVS could no longer place it every save failed
// silently: each new pairing was lost at the next reboot (the web-profile handoff showed the PIN screen
// again) and a revocation could come back. The record below is binary (28 B per session, 904 B full).
// Layout, little-endian: ver(1) count(1) pin(6 ASCII digits, or zeros) then count x { token(24 bytes)
// last_seen(u32 seconds; 0 = never seen with a set clock) }.
#define AUTH_REC_VER       1
#define AUTH_TOK_HEX       48                       // a session token: 24 random bytes as lowercase hex
#define AUTH_TOK_BYTES     (AUTH_TOK_HEX / 2)
#define AUTH_REC_HDR       8
#define AUTH_REC_ENT       (AUTH_TOK_BYTES + 4)
#define AUTH_REC_SIZE(n)   ((size_t)AUTH_REC_HDR + (size_t)(n) * AUTH_REC_ENT)

// Write pin + at most `keep` sessions into out (cap bytes), the most recently used first (lru, larger =
// newer; ties by slot order), so a record cut to fit always holds the sessions in use. Slots that are free
// or hold a malformed token are skipped. Returns the bytes written (AUTH_REC_SIZE(count)), 0 if cap is short.
size_t auth_rec_encode(uint8_t *out, size_t cap, const char *pin, const char (*tok)[AUTH_TOK_HEX + 1],
                       const int64_t *seen, const uint32_t *lru, int n, int keep);

// Parse a record into pin (7 bytes; "" when the record holds none) and slots 0..count-1 of tok/seen (the
// rest cleared), the OLDEST in slot 0 — so auth_rank_seen, which breaks equal last-seen by slot order, ranks
// the most recently used session newest even when every last-seen is 0 (a clock that was never set), and a
// re-encode emits the same order (no needless rewrite). Returns count, or -1 for a malformed record (wrong
// version, length or digits).
int auth_rec_decode(const uint8_t *rec, size_t len, char *pin, char (*tok)[AUTH_TOK_HEX + 1], int64_t *seen, int n);

// The save policy for the record, with the storage injected (nucleo_auth.c binds it to NVS, the host gate to
// a fake that enforces NVS's rules). NVS writes an update BEFORE it drops the old version, so an update needs
// room for both, and a multi-page write that runs out of room can damage the old copy on its rollback.
typedef struct {
    int  (*put)(void *ctx, const uint8_t *rec, size_t len);   // AUTH_PUT_*
    void (*wipe)(void *ctx);                                  // erase the record and the old-format text
    long (*room)(void *ctx);                                  // bytes the store can still take, -1 unknown
    void *ctx;
} auth_store_t;
#define AUTH_PUT_OK      0
#define AUTH_PUT_NOSPACE 1
#define AUTH_PUT_ERROR   (-1)

// Persist pin + the live sessions (buf: AUTH_REC_SIZE(live) bytes, NULL = out of memory). The full record
// first; if it does not fit, wipe and write a fresh one holding as many of the most recently used sessions
// as the room allows, halving down to the PIN alone. The stored sessions must stay a SUBSET of the live
// ones, or an evicted/revoked session would come back after a reboot: removed = a session was revoked,
// expired or evicted since the last successful save — then any failure wipes the old record (fail-closed:
// every browser re-pairs, none returns). Returns the sessions kept, or -1 when nothing new was stored.
int auth_rec_save(const auth_store_t *st, uint8_t *buf, const char *pin, const char (*tok)[AUTH_TOK_HEX + 1],
                  const int64_t *seen, const uint32_t *lru, int n, int live, bool removed);
