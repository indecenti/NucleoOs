// ANIMA offline translation tier — typed, deterministic, grounded IT<->EN dictionary lookup.
//
// The user asks to translate a word/phrase ("traduci casa in inglese", "come si dice grazie in
// english", "translate dog to italian") and ANIMA answers from an on-SD bilingual dictionary by
// EXACT key lookup. Like the profile tier, it is grounded by construction: a key is either present
// (exact answer) or absent (honest decline) — no encoder, no cosine, no generation, so it cannot
// hallucinate a translation. Scope is word + common-phrase (a dictionary/phrasebook, NOT sentence MT,
// which is physically impossible on the PSRAM-less device — sentence translation lives in the web tier).
//
// Data: SD /data/anima/dict-it-en.tsv (IT key -> EN) and dict-en-it.tsv (EN key -> IT), generated from
// tools/anima/dict/seed.it-en.tsv by tools/anima/gen_dicts.py. Sorted by key in strcmp order so the scan
// early-exits. Keys are normalized exactly like a_tokenize() (lowercase + Italian accent fold), so the
// query phrase normalizes to the same key here.
#pragma once
#include <stdbool.h>
#include "nucleo_anima.h"   // anima_result_t + the tier/action enums

#ifdef __cplusplus
extern "C" {
#endif

// Try to handle `raw` as a translation request. Detection is a strong, zero-false-positive trigger
// (an explicit translate verb "traduci"/"translate", or the frame "come si dice"/"how do you say");
// a non-translation query never matches. On a clear request it fills *r (tier COMMAND, action ANSWER,
// intent "translate") and returns 1 — with the grounded translation on a dictionary hit, or an honest
// decline on a miss (still 1, to stop the cascade so no later tier fabricates one). Returns 0 only when
// the input is not a translation request, so the cascade continues. `en` selects the reply language.
int nucleo_anima_translate(const char *raw, bool en, anima_result_t *r);

// Detect-ONLY: true if `raw` is a translation request (no dictionary lookup). The orchestrator uses this
// to route translation to the online teacher (Grok) in hybrid/online mode, with the dictionary as the
// offline floor. Mirrors the same trigger as nucleo_anima_translate (zero false positives).
bool nucleo_anima_translate_is_request(const char *raw);

#ifdef __cplusplus
}
#endif
