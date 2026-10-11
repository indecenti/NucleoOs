// Session-slot policy and the compact NVS record — see include/auth_slots.h.
#include "auth_slots.h"

int auth_pick_slot(const bool *used, const uint32_t *lru, int n)
{
    int victim = 0;
    for (int i = 0; i < n; i++) {
        if (!used[i]) return i;                   // a free slot always wins: nothing is evicted
        if (lru[i] < lru[victim]) victim = i;     // strict <: ties keep the lowest slot (deterministic)
    }
    return victim;
}

uint32_t auth_rank_seen(const bool *used, const int64_t *seen, uint32_t *lru, int n)
{
    uint32_t k = 0;
    for (int i = 0; i < n; i++) {
        lru[i] = 0;
        if (!used[i]) continue;
        uint32_t r = 1;                           // 1 + the number of used slots strictly older than i
        for (int j = 0; j < n; j++)
            if (used[j] && j != i && (seen[j] < seen[i] || (seen[j] == seen[i] && j < i))) r++;
        lru[i] = r;
        k++;
    }
    return k;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool tok_bytes(const char *hex, uint8_t *out)        // false for anything but 48 hex digits
{
    for (int i = 0; i < AUTH_TOK_BYTES; i++) {
        int hi = hexval(hex[2 * i]), lo = hi < 0 ? -1 : hexval(hex[2 * i + 1]);
        if (lo < 0) return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return hex[AUTH_TOK_HEX] == '\0';
}

static bool pin_ok(const char *pin)
{
    if (!pin) return false;
    for (int i = 0; i < 6; i++) if (pin[i] < '0' || pin[i] > '9') return false;
    return pin[6] == '\0';
}

size_t auth_rec_encode(uint8_t *out, size_t cap, const char *pin, const char (*tok)[AUTH_TOK_HEX + 1],
                       const int64_t *seen, const uint32_t *lru, int n, int keep)
{
    if (keep < 0) keep = 0;
    if (keep > 255) keep = 255;
    if (cap < AUTH_REC_HDR) return 0;
    out[0] = AUTH_REC_VER;
    for (int i = 0; i < 6; i++) out[2 + i] = pin_ok(pin) ? (uint8_t)pin[i] : 0;
    int count = 0;
    uint32_t below = UINT32_MAX; int below_slot = -1;           // the last pick: (recency, slot), newest first
    while (count < keep) {
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (!tok[i][0]) continue;
            bool after_last = below_slot < 0 || lru[i] < below || (lru[i] == below && i > below_slot);
            if (!after_last) continue;
            if (best < 0 || lru[i] > lru[best]) best = i;       // strict >: ties keep the lowest slot
        }
        if (best < 0) break;
        below = lru[best]; below_slot = best;
        uint8_t *e = out + AUTH_REC_SIZE(count);
        if (AUTH_REC_SIZE(count + 1) > cap) return 0;
        if (!tok_bytes(tok[best], e)) continue;                 // malformed: never persisted
        int64_t s = seen[best];
        uint32_t u = (s > 0 && s <= (int64_t)UINT32_MAX) ? (uint32_t)s : 0;
        for (int b = 0; b < 4; b++) e[AUTH_TOK_BYTES + b] = (uint8_t)(u >> (8 * b));
        count++;
    }
    out[1] = (uint8_t)count;
    return AUTH_REC_SIZE(count);
}

int auth_rec_decode(const uint8_t *rec, size_t len, char *pin, char (*tok)[AUTH_TOK_HEX + 1], int64_t *seen, int n)
{
    static const char hex[] = "0123456789abcdef";
    pin[0] = '\0';
    for (int i = 0; i < n; i++) { tok[i][0] = '\0'; seen[i] = 0; }
    if (!rec || len < AUTH_REC_HDR || rec[0] != AUTH_REC_VER || len != AUTH_REC_SIZE(rec[1])) return -1;
    bool none = true, digits = true;
    for (int i = 0; i < 6; i++) { none = none && rec[2 + i] == 0; digits = digits && rec[2 + i] >= '0' && rec[2 + i] <= '9'; }
    if (!none && !digits) return -1;
    if (digits) { for (int i = 0; i < 6; i++) pin[i] = (char)rec[2 + i]; pin[6] = '\0'; }
    int count = rec[1] < n ? rec[1] : n;
    for (int k = 0; k < count; k++) {                            // entry k (newest first) -> slot count-1-k
        const uint8_t *e = rec + AUTH_REC_SIZE(k);
        char *t = tok[count - 1 - k];
        for (int b = 0; b < AUTH_TOK_BYTES; b++) { t[2 * b] = hex[e[b] >> 4]; t[2 * b + 1] = hex[e[b] & 15]; }
        t[AUTH_TOK_HEX] = '\0';
        uint32_t u = 0;
        for (int b = 0; b < 4; b++) u |= (uint32_t)e[AUTH_TOK_BYTES + b] << (8 * b);
        seen[count - 1 - k] = u;
    }
    return count;
}

// NVS entries (32 B) a blob of len bytes takes: its index entry + up to two chunks (a header each) + the data.
static long rec_need(size_t len) { return (long)(3 + (len + 31) / 32) * 32; }

int auth_rec_save(const auth_store_t *st, uint8_t *buf, const char *pin, const char (*tok)[AUTH_TOK_HEX + 1],
                  const int64_t *seen, const uint32_t *lru, int n, int live, bool removed)
{
    int r = AUTH_PUT_ERROR;
    if (buf) {
        size_t len = auth_rec_encode(buf, AUTH_REC_SIZE(live), pin, tok, seen, lru, n, live);
        r = len ? st->put(st->ctx, buf, len) : AUTH_PUT_ERROR;
        if (r == AUTH_PUT_OK) return buf[1];
    }
    if (r == AUTH_PUT_NOSPACE) {
        // Start over from nothing: the old copy may be damaged by the failed write, and without it a fresh
        // record has twice the room. A power cut in this window loses pairing; it never resurrects a session.
        st->wipe(st->ctx);
        long room = st->room ? st->room(st->ctx) : -1;
        int keep = live;
        while (room >= 0 && keep > 0 && rec_need(AUTH_REC_SIZE(keep)) > room) keep--;   // pick what fits up front
        for (;;) {
            size_t len = auth_rec_encode(buf, AUTH_REC_SIZE(live), pin, tok, seen, lru, n, keep);
            r = len ? st->put(st->ctx, buf, len) : AUTH_PUT_ERROR;
            if (r == AUTH_PUT_OK) return buf[1];
            if (r != AUTH_PUT_NOSPACE || keep == 0) break;
            keep /= 2;
        }
        st->wipe(st->ctx);                                        // a half-written fresh copy goes too
        return -1;
    }
    if (removed) st->wipe(st->ctx);                               // the old record may hold a removed session
    return -1;
}
