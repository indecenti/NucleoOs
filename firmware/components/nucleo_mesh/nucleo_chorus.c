// nucleo_chorus — pure SANS-I/O capability directory. No esp_*/FreeRTOS/SD: host-tested.
#include "nucleo_chorus.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static void cpstr(char *dst, const char *src, int cap)   // bounded copy + NUL
{
    if (!src) { dst[0] = '\0'; return; }
    int i = 0; for (; i < cap - 1 && src[i]; i++) dst[i] = src[i]; dst[i] = '\0';
}

void chorus_init(chorus_ctx_t *c, const char *self_id, uint32_t caps, const char *domains)
{
    memset(c, 0, sizeof(*c));
    cpstr(c->self_id, self_id, CH_ID_MAX + 1);
    c->self_caps = caps;
    cpstr(c->self_dom, domains ? domains : "", CH_DOM_BUF);
}

// ---- tiny, defensive JSON field readers (we own both ends; keep it small + bounded) ---------
// strtoUL, not strtol: fields are non-negative and `dig` is a full u32 (often > 2^31). On LLP64
// (Windows: long is 32-bit) strtol would SATURATE at 2^31-1 and the digest would not round-trip.
// strtoul covers 0..2^32-1; the (long) cast preserves the bit pattern for the (uint32_t) callers.
static long jint(const char *j, const char *key, long def)
{
    char needle[24]; snprintf(needle, sizeof needle, "\"%s\"", key);
    const char *p = strstr(j, needle);
    if (!p) return def;
    p = strchr(p + strlen(needle), ':');
    if (!p) return def;
    return (long)strtoul(p + 1, NULL, 10);
}
static void jstr(const char *j, const char *key, char *out, int cap)
{
    out[0] = '\0';
    char needle[24]; snprintf(needle, sizeof needle, "\"%s\"", key);
    const char *p = strstr(j, needle);
    if (!p) return;
    p = strchr(p + strlen(needle), ':'); if (!p) return;
    p = strchr(p, '"');                  if (!p) return;   // opening quote of the value
    p++;
    int i = 0; for (; i < cap - 1 && p[i] && p[i] != '"'; i++) out[i] = p[i];
    out[i] = '\0';
}

// Whole-token membership: "history" matches dom "history code" but not "code"/"smart"/"his".
static int dom_has(const char *dom, const char *tok)
{
    if (!tok || !tok[0]) return 1;                   // no domain constraint
    size_t tl = strlen(tok);
    const char *p = dom;
    while (*p) {
        while (*p == ' ') p++;
        const char *s = p;
        while (*p && *p != ' ') p++;
        if ((size_t)(p - s) == tl && strncmp(s, tok, tl) == 0) return 1;
    }
    return 0;
}

uint32_t chorus_digest(const char *s)
{
    uint32_t h = 2166136261u;                        // FNV-1a/32
    if (s) for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}

void chorus_set_shard_digest(chorus_ctx_t *c, uint32_t dig) { c->self_dig = dig; }

int chorus_self_manifest(const chorus_ctx_t *c, uint16_t free_kb, uint8_t busy, char *out, int cap)
{
    int n = snprintf(out, cap, "{\"caps\":%u,\"free\":%u,\"busy\":%u,\"dig\":%u,\"dom\":\"%s\"}",
                     (unsigned)c->self_caps, (unsigned)free_kb, (unsigned)(busy ? 1 : 0),
                     (unsigned)c->self_dig, c->self_dom);
    return (n < 0 || n >= cap) ? -1 : n;
}

static chorus_peer_t *find_or_alloc(chorus_ctx_t *c, const char *id)
{
    chorus_peer_t *oldest = NULL;
    for (int i = 0; i < CH_PEERS; i++) {
        if (c->peers[i].used && strcmp(c->peers[i].id, id) == 0) return &c->peers[i];
        if (!c->peers[i].used) { if (!oldest || oldest->used) oldest = &c->peers[i]; }
        else if (oldest && oldest->used && c->peers[i].last_ms < oldest->last_ms) oldest = &c->peers[i];
        else if (!oldest) oldest = &c->peers[i];
    }
    return oldest;   // a free slot if any, else the least-recently-refreshed entry
}

int chorus_on_manifest(chorus_ctx_t *c, const char *src, const char *payload_json, uint32_t now_ms)
{
    if (!src || !src[0] || !payload_json) return 0;
    if (strcmp(src, c->self_id) == 0) return 0;          // never route to ourselves
    chorus_peer_t *p = find_or_alloc(c, src);
    if (!p) return 0;
    if (!(p->used && strcmp(p->id, src) == 0)) {          // freshly allocated slot
        memset(p, 0, sizeof(*p));
        cpstr(p->id, src, CH_ID_MAX + 1);
        p->used = 1;
    }
    long caps = jint(payload_json, "caps", 0);
    long fr   = jint(payload_json, "free", 0);
    long bz   = jint(payload_json, "busy", 0);
    p->caps    = (uint32_t)caps;
    p->free_kb = (uint16_t)(fr < 0 ? 0 : (fr > 65535 ? 65535 : fr));
    p->busy    = bz ? 1 : 0;
    p->dig     = (uint32_t)jint(payload_json, "dig", 0);
    jstr(payload_json, "dom", p->dom, CH_DOM_BUF);
    p->last_ms = now_ms;
    return 1;
}

static int stale(const chorus_peer_t *p, uint32_t now_ms)
{
    uint32_t age = now_ms >= p->last_ms ? now_ms - p->last_ms : 0;
    return age > CH_TTL_MS;
}

int chorus_route(const chorus_ctx_t *c, uint32_t cap, const char *domain, uint32_t now_ms,
                 char *out, int outcap)
{
    const chorus_peer_t *best = NULL;
    for (int i = 0; i < CH_PEERS; i++) {
        const chorus_peer_t *p = &c->peers[i];
        if (!p->used || p->busy) continue;
        if (stale(p, now_ms)) continue;
        if (cap && !(p->caps & cap)) continue;
        if (!dom_has(p->dom, domain)) continue;
        if (!best || p->free_kb > best->free_kb ||
            (p->free_kb == best->free_kb && strcmp(p->id, best->id) < 0))
            best = p;
    }
    if (!best) { if (outcap > 0) out[0] = '\0'; return 0; }
    cpstr(out, best->id, outcap);
    return 1;
}

int chorus_gc(chorus_ctx_t *c, uint32_t now_ms)
{
    int live = 0;
    for (int i = 0; i < CH_PEERS; i++) {
        if (!c->peers[i].used) continue;
        if (stale(&c->peers[i], now_ms)) c->peers[i].used = 0;
        else live++;
    }
    return live;
}

static const chorus_peer_t *find_peer(const chorus_ctx_t *c, const char *id)
{
    for (int i = 0; i < CH_PEERS; i++)
        if (c->peers[i].used && strcmp(c->peers[i].id, id) == 0) return &c->peers[i];
    return NULL;
}

int chorus_fuse(const chorus_ctx_t *c, const chorus_answer_t *cands, int n, int min_conf,
                uint32_t now_ms, char *who, int wcap, int *which)
{
    int best = -1, best_conf = -1;
    for (int i = 0; i < n; i++) {
        const chorus_answer_t *a = &cands[i];
        const chorus_peer_t *p = find_peer(c, a->src);
        if (!p || stale(p, now_ms)) continue;        // unknown/stale organ
        if (p->dig != a->dig) continue;              // answer's shard config != advertised (stale/swapped)
        if (a->conf < min_conf) continue;            // below the answer floor
        if (a->conf > best_conf ||
            (a->conf == best_conf && best >= 0 && strcmp(a->src, cands[best].src) < 0)) {
            best = i; best_conf = a->conf;
        }
    }
    if (best < 0) { if (wcap > 0) who[0] = '\0'; if (which) *which = -1; return 0; }
    cpstr(who, cands[best].src, wcap);
    if (which) *which = best;
    return 1;
}
