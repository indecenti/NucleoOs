// ANIMA — on-device offline natural-language assistant. See docs/anima.md.
//
// Phase 0: the L0 orchestrator only — normalize -> tokenize -> intent match ->
// confidence gate -> action. Pure C, allocation-free, no SD/model dependency.
// Higher tiers (L1 binary retrieval, L2 span-stitch, L3 cloud) are specified in
// docs/anima.md and plug in behind this same entry point.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Which cascade tier produced the result (see docs/anima.md §2).
typedef enum {
    ANIMA_TIER_NONE = 0,   // nothing matched with enough confidence
    ANIMA_TIER_COMMAND,    // L0: command / static FAQ hit
    ANIMA_TIER_FACT,       // L1: frozen retrieved answer        (future)
    ANIMA_TIER_STITCH,     // L2: MOSAICO span-stitch            (future)
    ANIMA_TIER_REMOTE,     // L3: cloud fallback                 (future)
} anima_tier_t;

// What the caller should do with a result.
typedef enum {
    ANIMA_ACT_NONE = 0,
    ANIMA_ACT_LAUNCH,      // open an app: arg = app id (e.g. "photo-viewer")
    ANIMA_ACT_SYSTEM,      // report live state: arg = "battery" | "time" | "storage"
    ANIMA_ACT_ANSWER,      // static answer already in `reply`
    ANIMA_ACT_TOOL,        // run a tool: intent = tool name (e.g. "create_file"), arg = parameter
} anima_action_t;

typedef struct {
    anima_tier_t   tier;
    anima_action_t action;
    char intent[24];       // intent id, for logging/telemetry
    char arg[64];          // app id / system key / routed file path (empty if none)
    char reply[1024];      // human-facing text (web shows all; native clips on render). L0/L1/card
                           // replies stay well under 256; online bios ~360; a CODE snippet from the
                           // online model uses the larger budget (fenced markdown, web renders it).
    int  confidence;       // 0..100
    int  budget;           // micro-thought: clusters probed by L1 (0 if not used)
    int  from_memory;      // micro-thought: 1 if resolved from utility memory (a follow-up)
    // --- agentic controller (deterministic, no generation) ---
    int  awaiting;         // 1 if this reply is a question expecting a follow-up (slot/clarify)
    char state[12];        // FSM state that produced this turn: idle|slot|clarify|followup|tool
    char corrected[64];    // typo-corrected query actually understood ("" if input was clean)
    char trace[112];       // visible reasoning trace: the steps this turn took (" · " joined)
    // --- conversational focus (set by the deductive tier so a follow-up can re-aim it) ---
    char subject[48];      // the entity the reasoner anchored ("Albert Einstein"); "" if none
    char relation[24];     // structured relation token it used ("born"|"capital"|"located_in"|...); "" if none
} anima_result_t;

// Load the command pack for `lang` ("it" for now). Idempotent.
esp_err_t nucleo_anima_init(const char *lang);

// Run the cascade on a UTF-8 input line. Understands IT+EN; replies in `lang`
// ("en" -> English, anything else -> Italian). Always returns (tier NONE if unsure).
anima_result_t nucleo_anima_query(const char *input, const char *lang);

// Spine gate: serialize nucleo_anima_query() across its two callers (web handler + native worker) so
// only ONE cascade owns the shared L1/session state at once. The web handler try-locks (returns a lean
// 503 "busy" on contention, never blocking the server); the native worker polls briefly then answers
// "busy". Without this, two concurrent queries free/read the same L1 buffers -> use-after-free + reboot.
bool nucleo_anima_try_lock(void);
void nucleo_anima_unlock(void);

// L1 index RAM management, exposed to the web layer so server-side TLS fetchers (online ANIMA tiers,
// /api/proxy, /api/llm) can reclaim the L1 index for the mbedTLS handshake's contiguous-heap need on
// this PSRAM-less chip. _unload() frees it (reloads transparently on the next query); _heap_bytes()
// reports how much that reclaim would free right now (0 when already unloaded).
void   nucleo_anima_l1_unload(void);
size_t nucleo_anima_l1_heap_bytes(void);

// Record a file as the current context for follow-ups (the executor calls this once a
// create_file actually leaves a file on disk, or when the named file already exists).
void nucleo_anima_note_file(const char *path);

// Cloud speech-to-text + summary (no on-device ASR model exists; see nucleo_anima_online.c).
// transcribe(): streams the audio at `path` to the Whisper endpoint; lang_hint="auto" lets
//   Whisper detect the spoken language (returned in out_lang), else forces that language.
//   Returns transcript length in out_text, or -1 (no key / offline / error).
// summarize(): summarizes `text` with the cloud teacher in `lang` ("it"/"en"); -1 on failure.
// Used by BOTH /api/transcribe (web) and the native Recorder app, so the device is the single
// transcription service for the whole OS.
int nucleo_anima_transcribe(const char *path, const char *lang_hint, char *out_text, int tcap, char *out_lang, int lcap);
int nucleo_anima_summarize(const char *text, const char *lang, char *out, int cap);

// Content channel for multi-step "compose THEN act": when the controller computes a payload too
// large for the 64-byte arg field (e.g. a file body composed from a calculation or a literal text
// clause), it stashes it here and the EXECUTOR writes it. Returns "" when the last turn produced no
// payload (then create_file makes an empty file, the legacy behavior). Valid until the next query.
const char *nucleo_anima_tool_content(void);

// Overflow reply channel: a reply too long for result.reply[1024] (a multi-line CODE snippet from the
// online model) is stashed here on the heap by the online tier; the web layer serves THIS verbatim when
// present (cJSON handles long strings) instead of the clipped struct field. NULL/"" = use result.reply.
const char *nucleo_anima_long_reply(void);
void        nucleo_anima_set_long_reply(const char *s);

// Close the agentic loop: the executor reports the real outcome of the last action back to
// the controller (e.g. create_file succeeded / was blocked / already existed). `ok` drives
// whether the action enters working memory and clears any pending slot. Best-effort.
void nucleo_anima_observe(const char *intent, bool ok);

// Forget the conversational state (pending slot, last app/file/topic, working-memory ring).
// The session otherwise persists across reboots on the SD. Used by "pulisci conversazione".
void nucleo_anima_reset_session(void);

// Phase 0 on-device micro-benchmark: measures int8 MAC throughput, Hamming/popcount
// throughput, and SD sequential read MB/s, then logs derived latency estimates.
// Enable with CONFIG_NUCLEO_ANIMA_BENCH and read it over `idf.py -p COM3 monitor`.
void nucleo_anima_benchmark(void);

// On-device self-test of the hyperdimensional reasoning core (HDC/VSA + permutation-KGE):
// semantic atoms, key->value recall, deduction by rotation, resonance-coherence honesty, and
// popcount throughput. Enable with CONFIG_NUCLEO_ANIMA_HDC_SELFTEST; read over `idf.py monitor`.
// No-op when the flag is off (zero cost). Mirrors tools/anima/hdc.mjs + kge.mjs.
void nucleo_anima_hdc_selftest(void);

// On-device DEDUCTIVE tier (HDC/permutation-KGE): grow a knowledge graph over the learned triples
// (mind.<lang>.jsonl), detect a fact question (forward "quando e nato X" / inverse "capitale di X" /
// transitive "in che continente e X") and DEDUCE the answer by composing relation-rotations — answering
// facts never literally stored, gated by resonance coherence (refuses rather than fabricating). Returns
// true and fills *out (tier FACT, action ANSWER, intent "hdc") only above the gate; false = no answer.
// Additive: nucleo_anima_query() calls it on an offline-cascade miss, before the online tiers.
bool nucleo_anima_hdc_reason(const char *query, const char *lang, anima_result_t *out);

// Detect-only companion to the deductive tier: if `query` is a fact question, fill `rel` with the
// structured relation token ("born"|"died"|"capital"|"located_in"|"author") and `subj` with the
// extracted entity name, and return true — WITHOUT building the KG or reasoning. Lets the orchestrator
// capture the conversational focus from the QUERY structure even when another tier (e.g. an L1 card)
// produced the answer, so a bare follow-up ("e newton?") can re-aim the reasoner. False = not a fact Q.
bool nucleo_anima_hdc_detect(const char *query, char *rel, size_t rcap, char *subj, size_t scap);

// NEURO-SYMBOLIC COMBINATOR tier (mirrors tools/anima/combinator.mjs): COMPUTE an answer by COMPOSING
// >=2 learned facts that exist as NO single stored triple — "chi e nato prima A o B" (year compare),
// "quanti anni tra la nascita di A e B" (subtraction), "X era europeo" (nationality->continent),
// "A e B erano connazionali" (country equality). Pulls the facts from the SAME learned triple store
// (mind.<lang>.jsonl) the deductive tier reads; pure integer/string composition, no generation -> cannot
// hallucinate. Returns true (and fills *out: tier FACT, action ANSWER, intent "combinator", confidence =
// min of the source confidences) when the query is compositional — EITHER with the computed answer, OR,
// if a required fact is missing, with an honest "non ho i dati" refuse (still true, to STOP the cascade
// so no later tier fabricates a bio for it). Returns false ONLY when the pattern doesn't match (continue).
// Runs as the FIRST reasoning tier (before nucleo_anima_hdc_reason) so a composition isn't mis-parsed.
bool nucleo_anima_combinator(const char *query, const char *lang, anima_result_t *out);

// TYPED-FACET tier (nucleo_anima_facet.c, mirrors docs/anima-knowledge-graph.md): answer a CATEGORICAL
// facet question — "che lavoro faceva X / di cosa si occupava X" (occupation), "X è uomo o donna / di che
// sesso è X" (gender) — by EXACT lookup in data/anima/learned/facets.<lang>.jsonl. Source-anchored, no
// generation -> cannot hallucinate; returns 0 (abstain) on an unknown entity or a wrong-type subject
// (a place has no occupation). Runs before the fuzzy L1/KGE tiers so a precise typed answer wins; `died`
// stays with the KGE (relational), categorical facets live here (the KGE's many-to-one fan-in won't cleanup).
int nucleo_anima_facet(const char *raw, bool en, anima_result_t *r);

// CROSS-SUBSTRATE GROUNDED VERIFICATION (ANIMA Forge, docs/anima-forge.md): judge a STRUCTURED claim
// extracted from a GENERATIVE answer (the browser M4 local-LLM, or M3/Grok) against the device's own
// zero-hallucination brain — "generative proposes, deterministic disposes". The client extracts the
// claims (apps/anima/www/forge/extract.js) and sends them here; the device RE-DERIVES numbers
// (a_try_calc) and CHECKS facts (KGE/L1, abstain-not-fabricate). kind="numeric" (key=expression,
// asserted=the printed number) or kind="fact" (key=the question e.g. "capitale della francia",
// asserted=the claimed answer). Conservative by design: returns UNKNOWN unless strongly grounded, so
// the caller renders ⚠ unverified (never silently trusts); CONTRADICTED only when the brain holds a
// confident DIFFERENT answer (the LENS-style veto generalized to generated output). `evidence` (out,
// may be NULL) receives the brain's grounded value for display.
typedef enum { ANIMA_VERIFY_UNKNOWN = 0, ANIMA_VERIFY_CONFIRMED = 1, ANIMA_VERIFY_CONTRADICTED = -1 } anima_verify_t;
anima_verify_t nucleo_anima_verify_claim(const char *kind, const char *key, const char *asserted,
                                         const char *lang, char *evidence, int evcap);

#ifdef __cplusplus
}
#endif
