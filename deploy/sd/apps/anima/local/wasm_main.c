// ANIMA Local — WebAssembly entry point.
//
// Exposes the EXACT offline cascade (nucleo_anima_query) to JavaScript as a single
// JSON-returning call. The network tier is stubbed (stub/anima_online_stub.c), so this
// runs the pure offline cascade — L0 intents + L1 retrieval + HDC/KGE deduction +
// facet/profile/learn — byte-identical to a Cardputer with Wi-Fi off, just on the
// client's CPU instead of the MCU's. The knowledge pack is mounted at /sd before init.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>     // setenv (PC-grade retrieval knob)
#include <emscripten.h>
#include "nucleo_anima.h"

// Overflow channels filled by the cascade (see nucleo_anima.h). long_reply is online-only
// (stubbed -> "" here); tool_content carries a composed file body for "compose THEN act".
const char *nucleo_anima_long_reply(void);
const char *nucleo_anima_tool_content(void);

static char s_json[32768];   // reused per call: offline replies are < 1 KB, content bounded

// Append `s` as a JSON string literal (with surrounding quotes). UTF-8 bytes pass through
// verbatim (already valid UTF-8 from the cascade); only JSON's mandatory escapes are applied.
static void json_str(char *dst, int cap, int *pos, const char *s) {
    int p = *pos;
    if (p < cap - 1) dst[p++] = '"';
    for (const unsigned char *u = (const unsigned char *)(s ? s : ""); *u && p < cap - 8; u++) {
        unsigned char c = *u;
        switch (c) {
            case '"':  dst[p++] = '\\'; dst[p++] = '"';  break;
            case '\\': dst[p++] = '\\'; dst[p++] = '\\'; break;
            case '\n': dst[p++] = '\\'; dst[p++] = 'n';  break;
            case '\r': dst[p++] = '\\'; dst[p++] = 'r';  break;
            case '\t': dst[p++] = '\\'; dst[p++] = 't';  break;
            default:
                if (c < 0x20) { int n = snprintf(dst + p, cap - p, "\\u%04x", c); if (n > 0) p += n; }
                else dst[p++] = (char)c;
        }
    }
    if (p < cap - 1) dst[p++] = '"';
    dst[p] = 0;
    *pos = p;
}

// Initialize the cascade for `lang` ("it"/"en"). Call ONCE after the /sd pack is mounted.
EMSCRIPTEN_KEEPALIVE
int anima_init(const char *lang) {
    // PC-GRADE RETRIEVAL — Browser mode runs on a real CPU+RAM, not the 18 KB MCU. Restore the FULL
    // rerank pool the device shrank to 16 to cut SD reads (prefilter M 16->64): the reranker sees more
    // candidates from the SAME adaptively-probed clusters, so recall rises while the gate still judges
    // EXACT cosines. CERTIFIED 0 new fabrications on the full host gate (45/45; describe-stress fab 0,
    // cross-topic 103/103, halluc 0/441). We deliberately do NOT widen nprobe/keep — probing distant
    // clusters surfaced near-misses that fabricated (tested and rejected, to protect zero-hallucination).
    // Read by the L1 tier's ANIMA_HOST hook (this build sets -DANIMA_HOST), so it's a pure runtime knob.
    setenv("L1_PFM", "64", 1);
    // PC-grade scalable knowledge: use the category-SHARDED AKB5 index when a manifest is mounted (the
    // browser's local-only EXTENDED brain — thousands of grounded cards past the device's curated set).
    // No manifest mounted -> no-op -> flat index (base build behaves as before). Recall is certified by
    // apps/anima/local/akb5-recall-cert (new-knowledge answered + abstention holds).
    setenv("ANIMA_AKB5", "1", 1);
    // PC-grade AKB5 ROUTING — the device probes only the top-4 shards/query to bound SD reads. In the
    // browser the shards live in RAM (MEMFS), so probing many more is ~free and FIXES routing recall once
    // the corpus grows to dozens of shards (e.g. 25k+ people sub-sharded by domain): a person's home shard
    // must be in the probed set or they're invisible. Searched shards still each apply the EXACT 0.85 gate,
    // so wider routing adds RECALL, never fabrication. Overridable via ANIMA_AKB5_PROBE for A/B sweeps.
    if (!getenv("ANIMA_AKB5_PROBE")) setenv("ANIMA_AKB5_PROBE", "24", 1);
    return (int)nucleo_anima_init(lang && lang[0] ? lang : "it");
}

// Set an env knob BEFORE anima_init (e.g. ANIMA_AKB5_PROBE) — lets the harness A/B device-vs-PC probe on
// one build. anima_init only defaults the probe when unset, so a prior set here wins.
EMSCRIPTEN_KEEPALIVE
void anima_set_env(const char *k, const char *v) { if (k && v) setenv(k, v, 1); }

// Forget conversational state (pending slot, last app/file/topic, working memory).
EMSCRIPTEN_KEEPALIVE
void anima_reset(void) { nucleo_anima_reset_session(); }

// Run the cascade and return the full result as a JSON object (pointer to a static buffer,
// valid until the next call — JS reads it with UTF8ToString, no free needed).
EMSCRIPTEN_KEEPALIVE
const char *anima_query_json(const char *input, const char *lang) {
    anima_result_t r = nucleo_anima_query(input ? input : "", lang && lang[0] ? lang : "it");
    const char *lr = nucleo_anima_long_reply();
    const char *tc = nucleo_anima_tool_content();
    const char *reply = (lr && lr[0]) ? lr : r.reply;   // long channel wins when set (long code blocks)

    char *d = s_json; int cap = (int)sizeof s_json; int p = 0;
    d[p++] = '{';
    #define COMMA() do { if (p > 1 && p < cap - 1) d[p++] = ','; } while (0)
    #define NUM(k, v) do { COMMA(); int n = snprintf(d + p, cap - p, "\"%s\":%d", (k), (int)(v)); if (n > 0) p += n; } while (0)
    #define STR(k, v) do { COMMA(); int n = snprintf(d + p, cap - p, "\"%s\":", (k)); if (n > 0) p += n; json_str(d, cap, &p, (v)); } while (0)
    NUM("tier",        r.tier);
    NUM("action",      r.action);
    NUM("confidence",  r.confidence);
    NUM("awaiting",    r.awaiting);
    NUM("from_memory", r.from_memory);
    NUM("budget",      r.budget);
    STR("intent",      r.intent);
    STR("arg",         r.arg);
    STR("state",       r.state);
    STR("corrected",   r.corrected);
    STR("trace",       r.trace);
    STR("subject",     r.subject);
    STR("relation",    r.relation);
    STR("reply",       reply);
    STR("content",     (tc && tc[0]) ? tc : "");
    #undef STR
    #undef NUM
    #undef COMMA
    if (p < cap - 1) d[p++] = '}';
    d[p] = 0;
    return s_json;
}
