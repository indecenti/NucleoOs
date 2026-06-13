// ANIMA personal-profile tier — typed, deterministic, offline cross-session "ANIMA knows you".
//
// The user states facts about THEMSELF in natural language ("mi chiamo X", "ho N anni", "abito a Y",
// "call me X", "my email is Z") and ANIMA stores them in CANONICAL typed fields, then answers first-person
// questions ("come mi chiamo?", "quanti anni ho?", "cosa sai di me?"). Unlike the fuzzy paraphrase recall of
// the learn tier (nucleo_anima_learn.c, for arbitrary world-facts), this tier is DETERMINISTIC: a field is
// either set (exact answer) or unset (honest "I don't know yet") — zero cosine, so zero hallucination by
// construction. It is esp-claw's user.md evolved: typed + actionable + offline + gated, not prompt-text.
//
// Store: SD /data/anima/profile.tsv — one "field<TAB>value" line per known field. Tiny + bounded.
#pragma once
#include <stdbool.h>
#include "nucleo_anima.h"   // anima_result_t + the tier/action enums

#ifdef __cplusplus
extern "C" {
#endif

// Try to handle `raw` as a profile SET ("mi chiamo X") or RECALL ("come mi chiamo") in language `en`.
// On a match fills *r (tier COMMAND, action ANSWER, intent "profile") and returns 1; else returns 0 so
// the cascade continues. First-person by construction (every frame carries mi/mio/io/my/I/me), so a
// third-person question ("come si chiama Marco") never matches. Recall of an UNSET field is an honest
// "non lo so ancora — dimmelo" (still handled), never a fabricated value.
int nucleo_anima_profile(const char *raw, bool en, anima_result_t *r);

#ifdef __cplusplus
}
#endif
