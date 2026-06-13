// ANIMA online tier — the only part of ANIMA that touches the network (docs/anima-online.md).
//
// It answers encyclopedic "chi è / cos'è X" questions that L0+L1 didn't know, by reaching a
// STRUCTURED source (Wikipedia REST summary) and relaying its ready-made `extract` — ANIMA
// never generates, it relays a frozen answer, exactly as offline. Every fetched answer is
// written to a "learned" card cache on the SD, so the SAME question is answered instantly and
// OFFLINE forever after: the device's brain grows from real use (self-distilling edge AI).
//
// Honesty is preserved: this runs ONLY on an L0+L1 miss, ONLY for an answerable entity intent,
// and the fetch happens ONLY with Wi-Fi. No connection -> serve the cache or refuse; never
// fabricate. See nucleo_anima.c for where it sits in the cascade.
#pragma once
#include <stdbool.h>
#include "nucleo_anima.h"

#ifdef __cplusplus
extern "C" {
#endif

// True if the device currently has internet (STA associated, has an IP) AND the user hasn't
// forced offline-only. Cheap, no I/O. Every network tier gates on this.
bool nucleo_anima_online_available(void);

// User master switch for the network tiers (persisted by the ANIMA app). OFF -> offline-only:
// cache + recall still answer, the network is never touched. Default ON.
void nucleo_anima_set_online(bool on);
bool nucleo_anima_online_enabled(void);

// Detect a "who/what is X" knowledge question in `input` (IT+EN). On a match, fills `entity`
// with the search term (display form, accents kept) and `slug` with its normalized id form
// (lowercase ascii, '-' separated) for cache/dedup, then returns 1. Returns 0 otherwise.
int nucleo_anima_online_entity(const char *input, bool en,
                               char *entity, int entity_cap, char *slug, int slug_cap);

// Answer an entity question. Offline-first: looks up the learned cache by `slug`; on a hit
// (and not past its freshness window) fills `out` and returns 1 with zero network use. On a
// cache miss, only if online, fetches + validates a structured summary in `lang`, fills `out`,
// and appends a learned card (unless the query is ephemeral). Returns 0 on an honest miss.
int nucleo_anima_online_answer(const char *entity, const char *slug, bool en, anima_result_t *out);

// Detect & answer a LIVE, time/place-bound question — weather, exchange rate, news. The answer is
// served FRESH and is NEVER cached: persisting volatile data would make ANIMA assert stale facts
// as timeless (docs/anima-online.md §6). Returns 1 if this was a live intent (answered, asked for
// a missing city, or a contextual honest refusal when offline / no source); 0 if not a live intent.
int nucleo_anima_online_live(const char *input, bool en, anima_result_t *out);

// True if `input` is a LIVE-data question (weather/news) that must take priority over the L0 FAQ/
// date intents — a weather phrasing carries words ("che", "fa", "domani") the capabilities/date
// intents would otherwise grab. The cascade calls this BEFORE try_cascade to route weather right
// (mirrors the simulator, where the weather tier overrides a stray L0 match). Cheap, no I/O.
bool nucleo_anima_online_is_live(const char *input, bool en);

// Semantic recall over LEARNED cards (offline, no network): match `query` against the device's
// learned-vector sidecar via the shared encoder. Answers a PARAPHRASE of something already learned
// when the match clears RECALL_THRESH; returns 0 otherwise (refuse rather than misattribute). 0 too
// if the encoder isn't loaded or nothing has been learned yet.
int nucleo_anima_online_recall(const char *query, bool en, anima_result_t *out);

// Wikidata precise facts: "quando è nato/morto X", "capitale di X", "chi ha scritto / autore di X"
// (IT+EN). Deterministic, NO key (Wikidata is free). Returns 1 if answered. Online-only (offline → 0).
int nucleo_anima_online_fact(const char *input, bool en, anima_result_t *out);

// True if `input` is a structured fact-question — the cascade uses this to give the precise fact
// PRIORITY over a generic entity bio (so "chi è X" and "quando è morto X" don't return the same text).
bool nucleo_anima_online_is_fact(const char *input, bool en);

// Bare-noun entity fallback: a short command-less noun ("batman", "einstein") that L0/L1/clarify all
// missed, treated as an entity lookup. STRICT (the resolved title must contain the asked word) and
// same-language, so junk / unrelated pages stay an honest miss. Offline-first; learns on a hit.
int nucleo_anima_online_entity_bare(const char *input, bool en, anima_result_t *out);

// Open-ended cloud teacher (provider-agnostic LLM: Claude/Gemini/OpenAI). DISABLED until the
// user configures an API key in the app. Wired as the last cascade tier; returns 0 (honest
// miss) while unconfigured. Designed in docs/anima-online.md §4, implemented in a later phase.
int nucleo_anima_online_teacher(const char *input, bool en, anima_result_t *out);

// PURE Grok passthrough for "Solo online" mode: a direct chat with the LLM (no JSON classification, no
// Wikipedia truth gate, no learning) — the raw model answer. Returns 1 if answered, 0 if no key/offline.
int nucleo_anima_online_teacher_pure(const char *input, bool en, anima_result_t *out);

// Grok chat WITH optional one-turn context (ctx_q=prev user msg, ctx_a=prev assistant reply) — the
// universal "save-the-day" fallback that resolves follow-ups/pronouns. Live answer, not cached. NULL ctx
// = one-shot. Returns 1 if answered, 0 if no key/offline.
int nucleo_anima_online_chat(const char *input, const char *ctx_q, const char *ctx_a, bool en, anima_result_t *out);

// CODE generation: returns ONE professional, fenced code snippet (verbatim, newlines preserved, larger
// budget than chat). For "scrivimi/dammi un esempio di codice python". Returns 1 if answered, 0 if no key/offline.
int nucleo_anima_online_code(const char *input, bool en, anima_result_t *out);

// True if the query is a SPECIFIC question about an entity ("cosa ha fatto X" / "per cosa è famoso X" /
// "what did X do") that the frozen bio can't answer — route it to Grok (teacher_pure) when online+key.
bool nucleo_anima_online_is_about(const char *input, bool en);

// Self-improving knowledge: re-vet a LEARNED card for `topic` that was saved without Grok, now that a
// teacher key is configured — Grok CONFIRM marks it permanent, VETO drops the false positive. Best-effort,
// no-op offline / no key / for baked cards. Call after an L1 fact hit in hybrid mode.
void nucleo_anima_online_upgrade(const char *topic, bool en);

#ifdef __cplusplus
}
#endif
