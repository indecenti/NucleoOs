// Session-slot policy — see include/auth_slots.h.
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
