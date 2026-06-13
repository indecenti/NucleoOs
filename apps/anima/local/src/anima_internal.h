// ANIMA private cross-TU interface — firmware-internal, NOT the public API.
// (The public API is include/nucleo_anima.h.) This header is the contract between the
// orchestrator (nucleo_anima.c) and the math/skills solver engine (anima_solve.c), both
// of which are private to this component. See docs/anima.md §2.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "nucleo_anima.h"   // anima_result_t and the tier/action enums

#ifdef __cplusplus
extern "C" {
#endif

// --- exported by anima_solve.c, called by the orchestrator -------------------

// Unified math/skills solver: date arithmetic, spreadsheet, geometry, physics, vectors,
// dimensional-analysis units, percent / Ohm / powers / roots / bases / roman / binop / ...
// Returns 1 when it answered exactly (fills *r), 0 to let the retrieval cascade take over.
int  anima_solve(const char *raw, bool en, anima_result_t *r);

// Pure arithmetic: fold IT/EN word operators to symbols and evaluate + - * / and parentheses.
// Returns 0 = not an expression, 1 = ok (result in *out), 2 = division by zero.
int  a_try_calc(const char *raw, double *out);

// Format a double into `out`: trims trailing zeros, locale-free (always '.' decimal).
void a_fmt_num(double v, char *out, size_t n);

// Restore user-defined units learned in a previous session (best-effort; SD may be absent).
void units_load(void);

// --- exported by anima_text.c (pure text utilities), called by the orchestrator ---

// Bilingual typo rescue: bounded Damerau-Levenshtein over ANIMA's command vocabulary.
// Applied only on a cascade miss. Returns 1 if it changed a word (corrected in `out`), 0 otherwise.
int  a_spellfix(const char *raw, char *out, size_t outsz);

// Damerau-Levenshtein distance between two short words, capped at `max` (returns max+1 once the
// cap is exceeded). Lets the solver tolerate a light typo in a math modifier ("cubica").
int  a_damlev(const char *a, const char *b, int max);

// Strip foreign-script clutter (Arabic/Cyrillic/CJK/...) from a reply IN PLACE — the device
// can't render it. Keeps Latin/Greek/punctuation; only ever shrinks the string.
void a_strip_foreign(char *s);

// --- defined in nucleo_anima.c, called by anima_solve.c ----------------------

// Normalize a phrase for substring matching: lowercase, de-accent, drop punctuation,
// and space-pad both ends so " word " tests are exact.
void a_norm_phrase(const char *raw, char *out, size_t cap);

// Host-only A/B switch for the pre-hardening lax behaviour (always false on device).
bool l0_legacy(void);

#ifdef __cplusplus
}
#endif
