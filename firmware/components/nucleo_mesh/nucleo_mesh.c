// nucleo_mesh — pure SANS-I/O core. No esp_* / FreeRTOS / SD deps: host-tested via mesh-ctest.c.
#include "nucleo_mesh.h"
#include <string.h>

void nmesh_init(nmesh_ctx_t *c, const nmesh_io_t *io, const char *self_id)
{
    memset(c, 0, sizeof(*c));
    c->io = io;
    if (self_id) { strncpy(c->self_id, self_id, NM_ID_MAX); c->self_id[NM_ID_MAX] = '\0'; }
}

bool nmesh_topic_gossips(const char *t)
{
    return t && (strncmp(t, "mesh.", 5) == 0 || strncmp(t, "chorus.", 7) == 0);
}

// ---- wire codec ------------------------------------------------------------
static int nm_encode(const char *src, uint32_t seq, const char *topic, const char *payload,
                     uint8_t *out)
{
    int sl = (int)strlen(src), tl = (int)strlen(topic), pl = (int)strlen(payload);
    if (sl < 1 || sl > NM_ID_MAX || tl < 1 || tl >= NM_TOPIC_MAX || pl >= NM_PAYLOAD_MAX) return -1;
    int n = 0;
    out[n++] = 'N'; out[n++] = 'M'; out[n++] = NM_VER; out[n++] = 0;
    out[n++] = (uint8_t)seq;        out[n++] = (uint8_t)(seq >> 8);
    out[n++] = (uint8_t)(seq >> 16); out[n++] = (uint8_t)(seq >> 24);
    out[n++] = (uint8_t)sl; memcpy(out + n, src, sl);     n += sl;
    out[n++] = (uint8_t)tl; memcpy(out + n, topic, tl);   n += tl;
    out[n++] = (uint8_t)pl; memcpy(out + n, payload, pl); n += pl;
    // Reserve the swarm auth-tag headroom so a sealed frame still fits the radio MTU (see NM_TAG_RESERVE).
    return n > NM_MTU - NM_TAG_RESERVE ? -1 : n;
}

static int nm_decode(const uint8_t *b, int len, char *src, uint32_t *seq, char *topic, char *payload)
{
    if (len < 11 || b[0] != 'N' || b[1] != 'M' || b[2] != NM_VER) return -1;
    int n = 4;
    *seq = (uint32_t)b[n] | ((uint32_t)b[n+1] << 8) | ((uint32_t)b[n+2] << 16) | ((uint32_t)b[n+3] << 24);
    n += 4;
    int sl = b[n++]; if (sl < 1 || sl > NM_ID_MAX || n + sl > len) return -1;
    memcpy(src, b + n, sl); src[sl] = '\0'; n += sl;
    if (n >= len) return -1;
    int tl = b[n++]; if (tl < 1 || tl >= NM_TOPIC_MAX || n + tl > len) return -1;
    memcpy(topic, b + n, tl); topic[tl] = '\0'; n += tl;
    if (n >= len) return -1;
    int pl = b[n++]; if (pl < 0 || pl >= NM_PAYLOAD_MAX || n + pl > len) return -1;
    memcpy(payload, b + n, pl); payload[pl] = '\0';
    return 0;
}

// ---- de-dup (sliding 32-window per origin) ---------------------------------
// Returns 1 if (src,seq) is fresh (and records it), 0 if already seen.
static int nm_seen(nmesh_ctx_t *c, const char *src, uint32_t seq)
{
    nmesh_peer_t *p = NULL;
    int freeslot = -1, lru = -1; uint32_t lruv = 0xFFFFFFFFu;
    for (int i = 0; i < NM_PEERS; i++) {
        if (c->peers[i].used) {
            if (strcmp(c->peers[i].id, src) == 0) { p = &c->peers[i]; break; }
            if (c->peers[i].lru < lruv) { lruv = c->peers[i].lru; lru = i; }
        } else if (freeslot < 0) freeslot = i;
    }
    if (!p) {                                   // first frame from this peer (or evict LRU)
        p = &c->peers[freeslot >= 0 ? freeslot : lru];
        memset(p, 0, sizeof(*p));
        strncpy(p->id, src, NM_ID_MAX); p->id[NM_ID_MAX] = '\0';
        p->used = 1; p->base = seq; p->mask = 0; p->lru = ++c->lru_clock;
        return 1;
    }
    p->lru = ++c->lru_clock;
    if (seq > p->base) {                         // newer: advance window, record old base as seen
        uint32_t d = seq - p->base;
        p->mask = (d >= 32) ? 0 : ((p->mask << d) | (1u << (d - 1)));
        p->base = seq;
        return 1;
    }
    if (seq == p->base) return 0;                // exact high-water duplicate
    uint32_t diff = p->base - seq;               // older: in-window?
    if (diff > 32) return 0;                      // too old to track — assume seen
    uint32_t bit = 1u << (diff - 1);
    if (p->mask & bit) return 0;
    p->mask |= bit;
    return 1;
}

// ---- public ----------------------------------------------------------------
void nmesh_on_local_event(nmesh_ctx_t *c, uint32_t seq, const char *topic,
                          const char *payload, const char *src)
{
    if (src && src[0]) { c->skipped_foreign++; return; }        // loop prevention: never re-forward
    if (!nmesh_topic_gossips(topic)) { c->skipped_nontopic++; return; }
    uint8_t f[NM_MTU];
    int n = nm_encode(c->self_id, seq, topic, payload && payload[0] ? payload : "{}", f);
    if (n < 0) { c->tx_toobig++; return; }
    if (c->io->send && c->io->send(c->io->user, f, n) == 0) c->tx++;
}

int nmesh_on_frame(nmesh_ctx_t *c, const uint8_t *buf, int len)
{
    char src[NM_ID_MAX + 1], topic[NM_TOPIC_MAX], payload[NM_PAYLOAD_MAX];
    uint32_t seq;
    if (nm_decode(buf, len, src, &seq, topic, payload) != 0) { c->rx_bad++; return -1; }
    if (strcmp(src, c->self_id) == 0) { c->rx_dup++; return 0; }  // our own broadcast echoed back
    if (!nm_seen(c, src, seq))        { c->rx_dup++; return 0; }
    c->rx_fresh++;
    if (c->io->inject) c->io->inject(c->io->user, src, topic, payload);
    return 1;
}
