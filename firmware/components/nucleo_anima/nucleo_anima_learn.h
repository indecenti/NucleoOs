// ANIMA user-teach tier — offline, network-free durable learning.
//
// Lets the USER teach ANIMA a fact ("ricorda che / remember that ...") that becomes recallable by
// PARAPHRASE, fully offline, using the device's own distilled encoder + a conservative evidence gate.
// Unlike the online learned cache (nucleo_anima_online.c, Wikipedia-fed and network-bound), this TU has
// ZERO network dependency (encoder + libc only) so it also runs in the host harness. One language-neutral
// store on SD; the encoder maps IT/EN into one space, so a fact taught in either language can be recalled.
//
// Store layout (SD /data/anima/learned/, both files keyed by the same id, kept in lockstep):
//   user.tsv  one record per line: id \t trigger \t reply
//   user.vec  binary sidecar:      u8 idlen | id | u8 dim | int8 vec[dim]   (mirrors the online recall format)
// Both are rewritten streaming (O(dim) RAM) — no full-file load — so the PSRAM-less device stays in budget.
#pragma once
#include <stdbool.h>
#include "nucleo_anima.h"   // anima_result_t + the tier/action enums

#ifdef __cplusplus
extern "C" {
#endif

// Store a user-taught fact: `subject` is what future questions will ask about (it is what gets embedded
// and later matched); `fact` is the verbatim reply ANIMA will speak back. `en` is advisory only (the store
// is language-neutral). Returns 1 = stored, 0 = encoder not loaded (recall would be off), -1 = REFUSED
// because the statement is volatile/temporal (the volatility law: never freeze "today/now" as timeless).
int  nucleo_anima_learn_put(const char *subject, const char *fact, bool en);

// Recall a user-taught fact by paraphrase. Fills *out (tier FACT, action ANSWER, intent "recall") and
// returns 1 ONLY on a confident, lexically-corroborated hit; 0 otherwise (honest abstain). No network.
// Gate (stricter than a bare cosine): pass if cosine >= STRONG, or cosine >= FLOOR AND the query shares a
// content word with the matched trigger (Damerau-corroborated) — the dual-channel discipline of the L1 tier.
int  nucleo_anima_learn_recall(const char *query, bool en, anima_result_t *out);

// True if `text` carries a volatility marker (oggi/adesso/today/now/weather/price...) and so must NEVER be
// frozen as a timeless fact. Exposed so the teach frame can refuse before writing. Mirrors online is_ephemeral.
bool nucleo_anima_learn_is_volatile(const char *text);

#ifdef __cplusplus
}
#endif
