// Session-slot policy for nucleo_auth: which slot a newly paired session takes, and how recency is
// rebuilt after a reboot. Pure C (no IDF) so tools/anima-host/authslot-check.mjs compiles and tests
// the real code on the PC.
//
// A new session takes a FREE slot when there is one; only a full table evicts, and then the session
// used LEAST RECENTLY — never the one a browser is using right now. (The old ring wrote at a head index
// that restarted at 0 after every reboot, so one pairing from a CLI tool could evict the live desktop
// session while 31 stale ones survived.)
#pragma once
#include <stdbool.h>
#include <stdint.h>

// used[i]: slot i holds a session. lru[i]: recency stamp (larger = used more recently).
// Returns the slot to write: the first free one, else the least recently used. n must be > 0.
int auth_pick_slot(const bool *used, const uint32_t *lru, int n);

// After a reboot only the persisted wall-clock "last seen" (day granularity; 0 = never seen with a set
// clock) survives. Rank the used slots by it — oldest -> 1 ... newest -> k, ties broken by slot order —
// and write the ranks into lru (free slots get 0). Returns k, the next recency stamp base.
uint32_t auth_rank_seen(const bool *used, const int64_t *seen, uint32_t *lru, int n);
