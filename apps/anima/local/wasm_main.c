// ANIMA Local — WebAssembly entry point.
//
// Exposes the offline cascade (nucleo_anima_query) to JavaScript as a single JSON-returning call.
// The engine is compiled DIRECTLY from firmware/components/nucleo_anima (see engine-src.mjs) with the
// host harness's online stub, so this runs the pure offline cascade — L0 intents + L1 retrieval +
// HDC/KGE deduction + facet/profile/learn — the same C as the Cardputer with Wi-Fi off and as
// tools/anima-host's anima.exe, just on the client's CPU. The knowledge pack is mounted at /sd first.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>     // setenv (PC-grade retrieval knob)
#include <emscripten.h>
#include "nucleo_anima.h"

// Fingerprint of the sources this module was built from (engine-src.mjs engineHash()), injected by
// build.ps1 through a generated -include header. parity.mjs compares it with the sources on disk.
#ifndef ANIMA_LOCAL_BUILD_ID
#define ANIMA_LOCAL_BUILD_ID "unstamped"
#endif
EMSCRIPTEN_KEEPALIVE
const char *anima_build_id(void) { return ANIMA_LOCAL_BUILD_ID; }

// Overflow channels (declared in nucleo_anima.h): long_reply is filled by the online tier only, so it
// stays "" with the stub; tool_content carries a composed file body for "compose THEN act".

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

// The browser's PC-GRADE runtime knobs, applied by anima_init and reported by anima_knobs (one table,
// so what parity.mjs replays on anima.exe is by construction what the browser runs). All are read by the
// firmware's ANIMA_HOST hooks (this build sets -DANIMA_HOST), so they are pure runtime knobs.
static const struct { const char *k, *v; int keep_if_set; } k_knobs[] = {
    // Full rerank pool: the device shrank it to 16 to cut SD reads (prefilter M 16->64). The reranker sees
    // more candidates from the SAME adaptively-probed clusters, so recall rises while the gate still judges
    // EXACT cosines. CERTIFIED 0 new fabrications on the full host gate (45/45; describe-stress fab 0,
    // cross-topic 103/103, halluc 0/441). We deliberately do NOT widen nprobe/keep — probing distant
    // clusters surfaced near-misses that fabricated (tested and rejected, to protect zero-hallucination).
    { "L1_PFM", "64", 0 },
    // Category-SHARDED AKB5 index when a manifest is mounted (the browser's local-only EXTENDED brain —
    // thousands of grounded cards past the device's curated set). No manifest mounted -> no-op -> flat
    // index. Recall is certified by apps/anima/local/akb5-recall-cert (new-knowledge answered + abstention).
    { "ANIMA_AKB5", "1", 0 },
    // AKB5 ROUTING: the device probes only the top-4 shards/query to bound SD reads. In the browser the
    // shards live in RAM (MEMFS), so probing many more is ~free and FIXES routing recall once the corpus
    // grows to dozens of shards (e.g. 25k+ people sub-sharded by domain): a person's home shard must be in
    // the probed set or they're invisible. Searched shards still each apply the EXACT 0.85 gate, so wider
    // routing adds RECALL, never fabrication. A prior anima_set_env wins (device-vs-PC A/B sweeps).
    { "ANIMA_AKB5_PROBE", "24", 1 },
};

// Initialize the cascade for `lang` ("it"/"en"). Call ONCE after the /sd pack is mounted.
EMSCRIPTEN_KEEPALIVE
int anima_init(const char *lang) {
    for (size_t i = 0; i < sizeof k_knobs / sizeof k_knobs[0]; i++)
        if (!(k_knobs[i].keep_if_set && getenv(k_knobs[i].k))) setenv(k_knobs[i].k, k_knobs[i].v, 1);
    return (int)nucleo_anima_init(lang && lang[0] ? lang : "it");
}

// The knobs in effect, as "K=V\n" lines (current values, so an anima_set_env override shows up).
EMSCRIPTEN_KEEPALIVE
const char *anima_knobs(void) {
    static char buf[256];
    int p = 0; buf[0] = 0;
    for (size_t i = 0; i < sizeof k_knobs / sizeof k_knobs[0]; i++) {
        const char *v = getenv(k_knobs[i].k);
        if (v && p < (int)sizeof buf) {
            int n = snprintf(buf + p, sizeof buf - p, "%s=%s\n", k_knobs[i].k, v);
            if (n > 0) p += n;
        }
    }
    return buf;
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
