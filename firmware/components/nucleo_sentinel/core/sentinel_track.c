#include "sentinel_track.h"
#include <string.h>

void sentinel_track_init(sentinel_tracker_table *t, uint32_t follow_window_s, uint16_t follow_hits) {
    memset(t, 0, sizeof *t);
    t->follow_window = follow_window_s;
    t->follow_hits = follow_hits ? follow_hits : 1;
}

static int find(const sentinel_tracker_table *t, const uint8_t addr[6]) {
    for (int i = 0; i < t->n; i++)
        if (memcmp(t->e[i].addr, addr, 6) == 0) return i;
    return -1;
}

int sentinel_track_seen(sentinel_tracker_table *t, const uint8_t addr[6],
                        sentinel_tracker_t type, uint8_t flags, int8_t rssi, uint32_t now) {
    int i = find(t, addr);
    if (i < 0) {
        if (t->n >= SENTINEL_TRACK_CAP) return -1;
        i = t->n++;
        memset(&t->e[i], 0, sizeof t->e[i]);
        memcpy(t->e[i].addr, addr, 6);
        t->e[i].first_seen = now;
    }
    sentinel_track_entry *e = &t->e[i];
    e->type = type;
    e->flags |= flags;                 // sticky: once seen separated, keep the flag
    e->rssi = rssi;
    e->last_seen = now;
    if (e->hits < 0xFFFF) e->hits++;

    // Following = a tracker that has been in range across a long enough window
    // with enough sightings. A separated FindMy tag qualifies faster (it is only
    // broadcasting because it is away from its owner — i.e. potentially on you).
    uint32_t span = now - e->first_seen;
    uint32_t window = (e->flags & SENTINEL_F_SEPARATED) ? (t->follow_window / 2) : t->follow_window;
    if (span >= window && e->hits >= t->follow_hits) e->following = 1;
    return i;
}

int sentinel_track_following(const sentinel_tracker_table *t) {
    int c = 0;
    for (int i = 0; i < t->n; i++) if (t->e[i].following) c++;
    return c;
}

void sentinel_track_expire(sentinel_tracker_table *t, uint32_t now, uint32_t ttl) {
    int w = 0;
    for (int i = 0; i < t->n; i++) {
        if (now - t->e[i].last_seen <= ttl) {
            if (w != i) t->e[w] = t->e[i];
            w++;
        }
    }
    t->n = w;
}
