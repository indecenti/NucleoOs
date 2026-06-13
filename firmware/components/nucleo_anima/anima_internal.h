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

// MULTI-STEP REASONING over the math engine — the conversational, "thinking" layer that sits ABOVE
// anima_solve: cascades ("…, poi moltiplica per 3, poi dimmi se è primo"), named registers across
// turns ("chiamalo A" … "A + B"), and a math self-description ("cosa sai calcolare?"). It orchestrates
// anima_solve step-by-step and is HONEST: a step it can't compute is reported, never faked. Runs only
// at the top level (the orchestrator's math entry), before anima_solve. 1 = answered, 0 = fall through.
int  anima_reason(const char *raw, bool en, anima_result_t *r);

// The principal numeric value carried by a reply: the LAST number in the text (the web twin's chaining
// convention) — so a follow-up can continue from "Fa 2430." → 2430. Returns 0 when the reply has no
// usable number (e.g. an honest refusal), which the cascade treats as "the chain can't continue".
int  a_reply_lastnum(const char *s, double *out);

// --- conversational numeric memory (defined in nucleo_anima.c, used by the reasoning layer) ---------
// Tiny RAM-only register file backed by the session and CLEARED ON RESET (never persisted to SD): the
// last answer plus a few user-named results. This is how an offline cascade "remembers" across turns.
int  anima_reg_get(const char *name, double *out);   // 1 if a register `name` (case-insensitive) is set
void anima_reg_set(const char *name, double val);    // create / overwrite; oldest evicted when full
int  anima_reg_last(double *out);                     // 1 if there is a remembered last answer
void anima_reg_set_last(double val);                  // remember the last numeric answer this turn

// Pure arithmetic: fold IT/EN word operators to symbols and evaluate + - * / and parentheses.
// Returns 0 = not an expression, 1 = ok (result in *out), 2 = division by zero.
int  a_try_calc(const char *raw, double *out);

// Format a double into `out`: trims trailing zeros, locale-free (always '.' decimal).
void a_fmt_num(double v, char *out, size_t n);     // precisione (%.6g): conversioni/unita'/geometria/fisica
void a_fmt_round(double v, char *out, size_t n);   // max 4 decimali: calcolo "normale" (aritmetica/medie)

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
