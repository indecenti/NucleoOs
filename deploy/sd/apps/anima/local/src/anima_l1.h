// Internal: ANIMA L1 semantic tier (nucleo_anima_l1.c). Used by nucleo_anima.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "nucleo_anima.h"

// Load the SD encoder + index. Best-effort: returns false (L1 stays off) if files absent.
bool nucleo_anima_l1_init(void);

// Free the in-RAM cluster index (~18 KB) so the assistant holds no RAM while you use other apps;
// the next query transparently reloads it from SD. Call it when a foreground app opens.
void nucleo_anima_l1_unload(void);

// Semantic match. Fills `out` (reply in EN if `en`, else IT) and returns 1 on a confident hit.
// want_detail returns the card's drill-down text (the "tell me more" follow-up) instead of the
// short reply, and returns 0 if that card has no detail.
int  nucleo_anima_l1_query(const char *text, bool en, bool want_detail, anima_result_t *out);

// AKB5 (category-sharded scalable index). _available() is a cheap probe for a valid manifest matching
// the encoder dim; _akb5_query() routes the query to the best shards and reuses _query() per shard,
// returning the best-confidence hit (same contract as _query). Absent manifest -> use the flat _query.
bool nucleo_anima_l1_akb5_available(void);
int  nucleo_anima_l1_akb5_query(const char *text, bool en, bool want_detail, anima_result_t *out);

// Dialogic clarify band: call right after nucleo_anima_l1_query() returns 0. If the top-1 is
// moderately similar and a runner-up competes, fills `out` with a "did you mean X or Y?" question
// and returns the two candidate answer offsets via ans1/ans2 (returns 1); else 0. A clarify is a
// question, never an asserted fact (zero false positives). See tools/anima/band_eval.py.
int  nucleo_anima_l1_band(bool en, anima_result_t *out, long *ans1, long *ans2);

// Resolve a clarify pick: read the full answer at one of the offered offsets into `out`.
int  nucleo_anima_l1_read(long ansoff, bool en, anima_result_t *out);

// MOSAICO (L2 span-stitch): enrich a confident L1 answer `io` (its reply = the best card's short
// answer, freshly produced by nucleo_anima_l1_query for `query`, so s_band still holds this query's
// top-2 answer offsets) into a fuller, STILL-GROUNDED reply by appending VERBATIM frozen spans from
// the SAME index: the best card's drill-down detail, then a topically-coherent runner-up card. Pure
// span-copy (NEST-style attribution) — every word stays a frozen card field, so it cannot fabricate;
// and it only ever ENRICHES an answer L1 already gave, so on a miss there is nothing to enrich. Returns
// 1 and rewrites io->reply (tier STITCH, intent "mosaico") iff it fused >=1 extra span; else 0.
int  nucleo_anima_l1_stitch(const char *query, bool en, anima_result_t *io);

// Share the distilled encoder with other tiers (the online learned-card recall). dim() is the
// vector length (0 if the encoder/index isn't loaded); encode() fills out[>=dim] and returns dim
// (or 0). Vectors are consistent with L1's own query embedding, so cosine across them is valid.
int  nucleo_anima_l1_dim(void);
int  nucleo_anima_l1_encode(const char *text, int8_t *out, int cap);

// Diagnostics: the top-2 DISTINCT cosines of the most recent nucleo_anima_l1_query(). Lets the
// harness / telemetry see how close a miss was — a borderline near-match (recoverable by the
// evidential gate) vs a true miss (card absent). Stale if L0 answered without reaching L1.
void nucleo_anima_l1_last_band(float *c1, float *c2);
