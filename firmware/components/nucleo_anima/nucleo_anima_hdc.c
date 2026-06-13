// ANIMA hyperdimensional reasoning core (HDC/VSA + permutation-KGE) — ON-DEVICE.
//
// Mirrors tools/anima/hdc.mjs + kge.mjs: a binary hypervector algebra in which ANIMA reasons OFFLINE
// using only the ESP32-S3's native cheap ops — XOR (bind), popcount (distance), bit-rotation (relation
// permutation), bitwise majority (bundle). nucleo_anima_hdc_selftest() proves, on real silicon:
//   - quasi-orthogonality (concentration of measure)   - SEMANTIC atoms (SimHash: near text → near HV)
//   - key→value RECALL by unbinding                     - DEDUCTION: transitive + inverse by rotation
//   - RESONANCE COHERENCE as intrinsic honesty          - popcount throughput (validates the memory plan)
//
// Memory-safe: the self-test allocates its scratch on the HEAP and frees it when done, so it costs ZERO
// permanent RAM (important on a PSRAM-less chip). It runs once at boot (before the apps), gated by
// CONFIG_NUCLEO_ANIMA_HDC_SELFTEST in main.c — when that's off the function is simply never called.
#include "nucleo_anima.h"
#include "nucleo_board.h"          // NUCLEO_SD_MOUNT (mind.<lang>.jsonl path for the deductive tier)
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "anima.hdc";

#define HDC_D 2048                 // hypervector width in bits. 2048 ≫ KG_MAXENT·fan-in (~12), so the
                                   // bundle/recall margin in σ-units is unchanged vs 8192 — but every
                                   // transient allocation below shrinks 4× (scratch 16 KB→4 KB, HV 1 KB→256 B),
                                   // which is what matters on this fragmentation-bound, PSRAM-less heap.
#define HDC_W (HDC_D / 32)         // 64 words = 256 B per hypervector
#define HDC_STD 22.63f             // std of Hamming between two random HVs = sqrt(D)/2
typedef uint32_t hv_t[HDC_W];

// --- deterministic PRNG (reproducible atoms; on-device we regenerate random vectors, never store them) ---
static uint32_t fnv1a(const char *s) { uint32_t h = 2166136261u; while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; } return h; }
static uint32_t s_rng = 1;
static inline void rseed(uint32_t s) { s_rng = s ? s : 1; }
static inline uint32_t rnd(void) { uint32_t x = s_rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; s_rng = x; return x; }

// --- core binary ops (each maps to a native S3 instruction) -----------------------------------------
static inline int hamming(const uint32_t *a, const uint32_t *b) { int d = 0; for (int i = 0; i < HDC_W; i++) d += __builtin_popcount(a[i] ^ b[i]); return d; }
static inline void hv_xor(uint32_t *o, const uint32_t *a, const uint32_t *b) { for (int i = 0; i < HDC_W; i++) o[i] = a[i] ^ b[i]; }   // bind
static void hv_random(uint32_t *o, const char *seed) { rseed(fnv1a(seed)); for (int i = 0; i < HDC_W; i++) o[i] = rnd(); }              // structural atom
static void hv_rotate(uint32_t *o, const uint32_t *a, int k) {                            // permute = relation operator
    k %= HDC_D; if (k < 0) k += HDC_D;
    for (int i = 0; i < HDC_W; i++) o[i] = 0;
    for (int i = 0; i < HDC_D; i++) if ((a[i >> 5] >> (i & 31)) & 1) { int j = i + k; if (j >= HDC_D) j -= HDC_D; o[j >> 5] |= (1u << (j & 31)); }
}

// SimHash semantic atom: features = word tokens + boundary char-3-grams. Shared features → shared
// random contributions → small Hamming (near text → near HV). Scratch counter is heap (g_simcnt).
// int8 (not int16): per-atom signed feature counts stay well within ±127 (a concept has a handful of
// features), and on a PSRAM-less S3 the 8 KB this saves vs int16 is the difference between fitting the
// reasoner and not. Saturation is impossible in practice; sign(count) is all the SimHash needs.
static int8_t *g_simcnt;
static void feat_add(const char *feat) { rseed(fnv1a(feat)); for (int i = 0; i < HDC_D; i++) g_simcnt[i] += (rnd() & 1) ? 1 : -1; }
static void hv_semantic(uint32_t *o, const char *text) {
    memset(g_simcnt, 0, (size_t)HDC_D * sizeof(int8_t));
    const char *p = text; char tok[48];
    while (*p) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        int tl = 0; while (*p && isalnum((unsigned char)*p) && tl < 47) { tok[tl++] = (char)tolower((unsigned char)*p); p++; }
        if (!tl) break;
        tok[tl] = 0;
        char f[64]; snprintf(f, sizeof f, "w:%s", tok); feat_add(f);
        char g[64]; snprintf(g, sizeof g, "^%s$", tok); int gl = (int)strlen(g);
        for (int i = 0; i + 3 <= gl; i++) { char tg[8] = { 'g', ':', g[i], g[i + 1], g[i + 2], 0 }; feat_add(tg); }
    }
    for (int i = 0; i < HDC_W; i++) o[i] = 0;
    for (int i = 0; i < HDC_D; i++) if (g_simcnt[i] > 0) o[i >> 5] |= (1u << (i & 31));
}

// Bundle (superposition) by bitwise majority. `items` = n consecutive hypervectors. Scratch heap (g_bcnt).
// int8 (not int32): g_bcnt[i] is a per-bit ON-count over the bundled items, so 0..n; with the KG capped at
// KG_MAXENT entities a node bundles at most (incoming edges + 1) items, far under 127. Halves the 32 KB
// int32 footprint to 8 KB — essential on the PSRAM-less S3. Keep bundles small (n < 127) and it never wraps.
static int8_t *g_bcnt;
static void hv_bundle(uint32_t *o, const uint32_t *items, int n) {
    memset(g_bcnt, 0, (size_t)HDC_D * sizeof(int8_t));
    for (int k = 0; k < n; k++) { const uint32_t *v = items + (size_t)k * HDC_W; for (int i = 0; i < HDC_D; i++) g_bcnt[i] += (v[i >> 5] >> (i & 31)) & 1; }
    for (int i = 0; i < HDC_W; i++) o[i] = 0;
    for (int i = 0; i < HDC_D; i++) if (g_bcnt[i] * 2 > n) o[i >> 5] |= (1u << (i & 31));   // odd n → no ties
}

// Cleanup: nearest of n consecutive stored items; MARGIN (2nd−1st) in std units = the coherence.
typedef struct { int idx; int dist; float coh; } cleanup_t;
static cleanup_t cleanup(const uint32_t *probe, const uint32_t *cb, int n) {
    int bd = 1 << 30, sd = 1 << 30, bi = -1;
    for (int i = 0; i < n; i++) { int d = hamming(probe, cb + (size_t)i * HDC_W); if (d < bd) { sd = bd; bd = d; bi = i; } else if (d < sd) sd = d; }
    cleanup_t r; r.idx = bi; r.dist = bd; r.coh = (float)(sd - bd) / HDC_STD; return r;
}
static int rel_shift(const char *rel) { char b[48]; snprintf(b, sizeof b, "rel:%s", rel); return 1 + (int)(fnv1a(b) % (HDC_D - 2)); }

// =====================================================================================================
#define CHECK(cond, ...) do { if (cond) { pass++; ESP_LOGI(TAG, "  PASS " __VA_ARGS__); } else { fail++; ESP_LOGW(TAG, "  FAIL " __VA_ARGS__); } } while (0)
#define NSLOT 18                                   // hypervector working slots (18 KB heap, transient)
#define S(i) (pool + (size_t)(i) * HDC_W)          // slot i of the heap pool

void nucleo_anima_hdc_selftest(void) {
    uint32_t *pool = malloc((size_t)NSLOT * HDC_W * sizeof(uint32_t));
    g_simcnt = malloc((size_t)HDC_D * sizeof(int8_t));
    g_bcnt   = malloc((size_t)HDC_D * sizeof(int8_t));
    if (!pool || !g_simcnt || !g_bcnt) { ESP_LOGW(TAG, "self-test skipped: not enough heap"); free(pool); free(g_simcnt); free(g_bcnt); return; }
    int pass = 0, fail = 0;
    ESP_LOGI(TAG, "=== ANIMA HDC/KGE self-test (D=%d, %d B/HV) ===", HDC_D, HDC_W * 4);

    // 1) CONCENTRATION OF MEASURE — two random concepts are quasi-orthogonal (~D/2 apart).
    hv_random(S(0), "alpha"); hv_random(S(1), "beta");
    { int d = hamming(S(0), S(1)); CHECK(abs(d - HDC_D / 2) < 6 * (int)HDC_STD, "ortho: random pair Hamming=%d (~%d)", d, HDC_D / 2); }

    // 2) SEMANTIC ATOMS — near text stays correlated, unrelated stays orthogonal.
    hv_semantic(S(0), "gatto"); hv_semantic(S(1), "il gatto"); hv_semantic(S(2), "giappone");
    { int near = hamming(S(0), S(1)), far = hamming(S(0), S(2));
      CHECK(near + 200 < far, "semantic: near=%d < far=%d (SimHash preserva il significato)", near, far); }

    // 3) KEY→VALUE RECALL — bundle (role⊗value) pairs, recover one by unbinding its role.
    //    roles S0..2, values S3..5 (contiguous codebook), pairs S6..8 (contiguous), bundle S9, probe S10.
    hv_random(S(0), "role:born"); hv_random(S(1), "role:capital"); hv_random(S(2), "role:field");
    hv_semantic(S(3), "1879"); hv_semantic(S(4), "berna"); hv_semantic(S(5), "fisica");
    hv_xor(S(6), S(0), S(3)); hv_xor(S(7), S(1), S(4)); hv_xor(S(8), S(2), S(5));
    hv_bundle(S(9), S(6), 3); hv_xor(S(10), S(9), S(2));
    { cleanup_t c = cleanup(S(10), S(3), 3); CHECK(c.idx == 2, "recall: unbind 'field' → vals[%d] (atteso 2, coh %.1f)", c.idx, c.coh); }

    // 4) DEDUCTION by rotation — relations compose. Chain Lione→Francia→Europa→Terra (one relation).
    //    ents S0..3 (contiguous codebook). h1/h2 = scratch S10/S11, inverse S12, beyond-chain S13.
    { int k = rel_shift("si_trova_in"); const char *names[4] = { "lione", "francia", "europa", "terra" };
      hv_semantic(S(0), names[0]); hv_rotate(S(1), S(0), k); hv_rotate(S(2), S(1), k); hv_rotate(S(3), S(2), k);
      hv_rotate(S(10), S(0), k); hv_rotate(S(11), S(10), k);                          // two hops
      cleanup_t c2 = cleanup(S(11), S(0), 4);
      CHECK(c2.idx == 2 && c2.coh > 4.0f, "deduce 2-hop: lione→?→? = %s (atteso europa, coh %.1f)", names[c2.idx >= 0 ? c2.idx : 0], c2.coh);
      hv_rotate(S(12), S(1), HDC_D - k); cleanup_t ci = cleanup(S(12), S(0), 4);      // inverse
      CHECK(ci.idx == 0, "inverse: francia ←(si_trova_in) = %s (atteso lione)", names[ci.idx >= 0 ? ci.idx : 0]);
      hv_rotate(S(13), S(3), k); cleanup_t cu = cleanup(S(13), S(0), 4);              // beyond the chain
      CHECK(cu.coh < 3.0f, "honesty: oltre la catena → coerenza bassa %.1f (rifiuta, non inventa)", cu.coh); }

    // 5) TIMING — popcount throughput on real silicon; validates the flash-brain plan (docs/anima-memory.md).
    { hv_random(S(0), "t"); hv_random(S(1), "u"); volatile int acc = 0; enum { ITER = 30000 };
      int64_t t0 = esp_timer_get_time();
      for (int i = 0; i < ITER; i++) acc += hamming(S(0), S(1));
      int64_t dt = esp_timer_get_time() - t0; (void)acc;
      double hv_ms = (double)ITER / ((double)dt / 1000.0);
      ESP_LOGI(TAG, "  timing: %.0f HV/ms popcount (%d B/HV) → scan di 8192 HV ~ %.1f ms", hv_ms, HDC_W * 4, 8192.0 / hv_ms); }

    ESP_LOGI(TAG, "=== HDC self-test: %d PASSED, %d FAILED ===", pass, fail);
    free(pool); free(g_simcnt); free(g_bcnt);
}

// =====================================================================================================
// ON-DEVICE DEDUCTIVE TIER — wire the HDC/KGE engine above into the real query cascade.
//
// Loads the learned triples (mind.<lang>.jsonl), grows a permutation-KGE over them (port of
// tools/anima/kge.mjs KG.build), detects a fact question (forward "quando è nato X" / "X di Y"
// AND inverse "qual è la capitale di Francia" AND transitive "in che continente è Lione"),
// resolves a possibly-partial name, then DEDUCES the answer by rotating the relation forward,
// backward, or composing it — answering facts NEVER literally stored. Gates on resonance
// coherence (>= HDC_REASON_GATE): below the gate it returns false (refuses) — honesty is a
// convergence property, never a fabrication. Additive: called only on an offline-cascade miss.
//
// RAM: ONE flat codebook of N<=KG_MAXENT entity HVs (256 B each) + the int8 scratch (2 KB+2 KB),
// all malloc'd transiently and FREED before return. malloc-fail or N>cap -> false (no regression).
// =====================================================================================================
#define HDC_REASON_GATE 4.0f        // coherence units (margin/σ). Bench: entailed >> this >> noise.
#define KG_MAXENT       12          // hard cap on entities held in RAM (3 KB of HVs worst case)
#define KG_MAXTRIP      48          // hard cap on EDGES held for one query's subgraph (kept tiny by design)
#define KG_FILESCAN     4096        // hard cap on mind-file LINES scanned per pass (subgraph load reads the
                                    // whole file but interns only the query-relevant subgraph -> O(KG_MAXENT))
#define KG_NAMELEN      48          // slug / name buffer
#define KG_VALLEN       96          // human value string ("14 marzo 1879")
#define KG_AMBIG        6           // distinct name-matches collected for proactive disambiguation

typedef struct {
    char name[KG_MAXENT][KG_NAMELEN];   // slug (identity) of each entity
    char label[KG_MAXENT][KG_VALLEN];   // human label/value for the reply (empty -> use name)
    int  n;                             // entity count
    int  edge_h[KG_MAXTRIP];            // triple head entity index
    int  edge_t[KG_MAXTRIP];            // triple tail entity index
    int  edge_k[KG_MAXTRIP];            // relation rotation amount (rel_shift)
    int  ntrip;
    int  seed_idx;                      // entity index of the query subject (the BFS seed)
    // PROACTIVE DISAMBIGUATION: when a bare/partial name (no exact slug) matches >=2 DISTINCT entities that
    // carry the asked relation ("quando è nato trump" -> 9 Trumps), don't silently guess — name the
    // candidates and ask. General across every KGE relation (born/country/author/capital/located_in).
    int  ambig_n;                       // 0 = unambiguous; >=2 = number of distinct name-matches found
    char ambig[KG_AMBIG][KG_VALLEN];    // their human labels (for the clarify message)
    uint32_t *cb;                       // flat codebook: n consecutive entity HVs (the embedding E)
} kg_t;

// ---- slugify (mirror serve-shell.mjs): lowercase, de-accent (Italian á-ú folded), non-alnum -> '-',
//      collapse runs, trim leading/trailing '-'. ASCII only (the C tokenizer already saw UTF-8). ------
static void kg_slug(const char *in, char *out, size_t cap) {
    size_t o = 0; int prev_dash = 1;   // prev_dash=1 suppresses a leading '-'
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 1 < cap; p++) {
        unsigned char c = *p; char ch = 0;
        if (c == 0xC3 && p[1]) {        // 2-byte UTF-8 Latin-1 supplement -> base letter
            unsigned char d = *++p;
            if      (d >= 0xA0 && d <= 0xA2) ch = 'a';
            else if (d >= 0xA8 && d <= 0xAA) ch = 'e';
            else if (d >= 0xAC && d <= 0xAE) ch = 'i';
            else if (d >= 0xB2 && d <= 0xB4) ch = 'o';
            else if (d >= 0xB9 && d <= 0xBB) ch = 'u';
            else ch = 0;
        } else if (isalnum(c)) {
            ch = (char)tolower(c);
        }
        if (ch) { out[o++] = ch; prev_dash = 0; }
        else if (!prev_dash) { out[o++] = '-'; prev_dash = 1; }
    }
    while (o > 0 && out[o - 1] == '-') o--;   // trim trailing dash
    out[o] = 0;
}

// Intern an entity by slug; returns its index (existing or new), or -1 if the cap is hit. The first
// non-empty human label/value seen for a slug is kept (for the reply).
static int kg_intern(kg_t *g, const char *slug, const char *label) {
    for (int i = 0; i < g->n; i++)
        if (strcmp(g->name[i], slug) == 0) {
            if (!g->label[i][0] && label && label[0]) snprintf(g->label[i], KG_VALLEN, "%s", label);
            return i;
        }
    if (g->n >= KG_MAXENT) return -1;
    int i = g->n++;
    snprintf(g->name[i], KG_NAMELEN, "%s", slug);
    snprintf(g->label[i], KG_VALLEN, "%s", (label && label[0]) ? label : "");
    return i;
}

// Minimal JSON-string field extractor: finds "\"key\":\"...\"" in one mind line and copies the
// (unescaped-enough) value. The mind writer emits plain JSON.stringify — values here are short
// labels/dates with no embedded quotes/backslashes, so a literal scan is sufficient and allocation-free.
static bool kg_json_str(const char *line, const char *key, char *out, size_t cap) {
    char pat[24]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(line, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < cap) {
        if (*p == '\\' && p[1]) p++;     // copy the escaped char literally (good enough for labels)
        out[o++] = *p++;
    }
    out[o] = 0;
    return true;
}

// --- slug segment-matching (shared by the subgraph SEED and the combinator's getFact) ---------------
// `needle` is a full hyphen-delimited SEGMENT of `hay` (or equals it). Segment-, not substring-, matching
// so a short surname only binds at a token boundary: "einstein"~"albert-einstein" (segment) yes, but the
// combinator's "ali" never binds to "natalie" (substring 'ali' is not a segment). Kills the >=200-entity
// substring collision the review flagged without losing legitimate partial-name lookups.
static bool slug_has_segment(const char *hay, const char *needle) {
    size_t nl = strlen(needle); if (!nl) return false;
    for (const char *p = strstr(hay, needle); p; p = strstr(p + 1, needle)) {
        bool lb = (p == hay) || (p[-1] == '-');
        bool rb = (p[nl] == 0) || (p[nl] == '-');
        if (lb && rb) return true;
    }
    return false;
}
static bool slug_seg_match(const char *a, const char *b) {
    return !strcmp(a, b) || slug_has_segment(a, b) || slug_has_segment(b, a);
}

// Parse one mind-file line as an edge of relation `relfilter`. Fills the slugged head/tail and the human
// head-label / tail-value (for replies). Returns false if the line is not a matching edge. Allocation-free.
static bool kg_edge(const char *line, const char *relfilter,
                    char *hslug, char *tslug, char *hlabel, char *tval, size_t cap) {
    char subj[KG_VALLEN], rel[KG_NAMELEN], val[KG_VALLEN], label[KG_VALLEN];
    if (!kg_json_str(line, "subject", subj, sizeof subj)) return false;
    if (!kg_json_str(line, "rel",     rel,  sizeof rel))  return false;
    if (!kg_json_str(line, "value",   val,  sizeof val))  return false;
    if (relfilter && strcmp(rel, relfilter) != 0) return false;
    if (!kg_json_str(line, "label",   label, sizeof label)) label[0] = 0;
    kg_slug(label[0] ? label : subj, hslug, cap);     // head identity = label (else subject)
    kg_slug(val, tslug, cap);                         // tail identity = value
    snprintf(hlabel, cap, "%s", label[0] ? label : subj);
    snprintf(tval,   cap, "%s", val);
    return hslug[0] && tslug[0];
}

// Find the index of an interned entity by slug, or -1.
static int kg_find(const kg_t *g, const char *slug) {
    for (int i = 0; i < g->n; i++) if (!strcmp(g->name[i], slug)) return i;
    return -1;
}

// Build the embedding (port of KG.build): roots (no incoming edge) anchor to their semantic vector;
// every other node = majority-bundle of [semanticHV(self), rotate(E[head], k) for each incoming edge].
// Sweep (longestChain+2) times so the deepest chain propagates. cb = the flat entity HV array (codebook).
// Uses ONE scratch slot pool sized to the max incoming fan-in + 1. Returns false on malloc failure.
static bool kg_build(kg_t *g) {
    int n = g->n;
    // longest chain (crude DAG depth) -> sweep count, mirroring KG.longestChain. Bellman-style
    // relaxation: at most n passes converges for any acyclic graph; cycles self-cap via the n bound.
    int depth = 1, dep[KG_MAXENT];
    for (int i = 0; i < n; i++) dep[i] = 0;
    for (int pass = 0; pass < n; pass++) {
        int changed = 0;
        for (int e = 0; e < g->ntrip; e++) {
            int h = g->edge_h[e], t = g->edge_t[e];
            if (dep[h] + 1 > dep[t]) { dep[t] = dep[h] + 1; changed = 1; }
        }
        if (!changed) break;
    }
    for (int i = 0; i < n; i++) if (dep[i] + 1 > depth) depth = dep[i] + 1;
    if (depth > 16) depth = 16;
    int sweeps = depth + 2;

    // scratch: codebook E (n HVs) + a next-buffer (n HVs) + a bundle staging pool (KG_MAXENT+1 HVs).
    uint32_t *E    = malloc((size_t)n * HDC_W * sizeof(uint32_t));
    uint32_t *next = malloc((size_t)n * HDC_W * sizeof(uint32_t));
    uint32_t *stg  = malloc((size_t)(KG_MAXENT + 1) * HDC_W * sizeof(uint32_t));
    if (!E || !next || !stg) { free(E); free(next); free(stg); return false; }
    #define EV(i)   (E    + (size_t)(i) * HDC_W)
    #define NV(i)   (next + (size_t)(i) * HDC_W)
    #define SV(i)   (stg  + (size_t)(i) * HDC_W)

    for (int i = 0; i < n; i++) hv_semantic(EV(i), g->name[i]);     // init: semantic anchor
    for (int s = 0; s < sweeps; s++) {
        for (int e = 0; e < n; e++) {
            // gather incoming edges of entity e
            hv_semantic(SV(0), g->name[e]);                        // light semantic anchor (item 0)
            int cnt = 1;
            for (int k = 0; k < g->ntrip && cnt <= KG_MAXENT; k++)
                if (g->edge_t[k] == e) { hv_rotate(SV(cnt), EV(g->edge_h[k]), g->edge_k[k]); cnt++; }
            if (cnt == 1) hv_semantic(NV(e), g->name[e]);          // root: stays its semantic identity
            else          hv_bundle(NV(e), SV(0), cnt);            // bundle relational evidence + anchor
        }
        memcpy(E, next, (size_t)n * HDC_W * sizeof(uint32_t));     // E <- next
    }
    g->cb = E;                                                     // codebook = grown entity vectors
    free(next); free(stg);
    #undef EV
    #undef NV
    #undef SV
    return true;
}

static void kg_free(kg_t *g) { free(g->cb); g->cb = NULL; }

// ---- query NLU (mirror serve-shell.mjs factDetect/inverseDetect/locDetect, ASCII-folded) -----------
// We fold the query the same way the tokenizer does (lowercase, drop Italian accents) into `nf`, then
// pattern-match. `rel` is the relation NAME used to learn the triple (so rel_shift matches the graph).

// Strip a leading article/preposition from an extracted name (mirror stripLead, ASCII subset).
static void kg_strip_lead(char *s) {
    static const char *lead[] = { "della ","dello ","delle ","degli ","dei ","del ","di ","the ",
                                  "la ","il ","lo ","le ","gli ","un ","uno ","una ","a ","an ", NULL };
    for (;;) {
        int cut = 0;
        for (int i = 0; lead[i]; i++) { size_t l = strlen(lead[i]); if (!strncmp(s, lead[i], l)) { cut = (int)l; break; } }
        if (!cut) break;
        memmove(s, s + cut, strlen(s + cut) + 1);
    }
}

// Fold raw UTF-8 query -> lowercase ASCII, accents removed, punctuation/?! trimmed. Mirrors a_tokenize's
// folding but keeps spaces (so we can substring-match phrases). Output is NUL-terminated.
static void kg_fold(const char *raw, char *out, size_t cap) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)raw; *p && o + 1 < cap; p++) {
        unsigned char c = *p; char ch;
        if (c == 0xC3 && p[1]) {
            unsigned char d = *++p;
            if      (d >= 0xA0 && d <= 0xA2) ch = 'a';
            else if (d >= 0xA8 && d <= 0xAA) ch = 'e';
            else if (d >= 0xAC && d <= 0xAE) ch = 'i';
            else if (d >= 0xB2 && d <= 0xB4) ch = 'o';
            else if (d >= 0xB9 && d <= 0xBB) ch = 'u';
            else ch = ' ';
        } else if (isalnum(c)) ch = (char)tolower(c);
        else ch = ' ';
        out[o++] = ch;
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = 0;
}

// A detected question: direction + the relation + the extracted entity name.
typedef enum { DIR_FWD, DIR_INV, DIR_TRANS } kg_dir_t;
typedef struct { kg_dir_t dir; const char *rel; char name[KG_VALLEN]; } kg_det_t;

// Copy the tail of `nf` starting at byte offset `at` into det->name, stripping a leading article.
static void kg_take(kg_det_t *det, const char *nf, size_t at) {
    snprintf(det->name, sizeof det->name, "%s", nf + at);
    // trim trailing spaces
    for (int i = (int)strlen(det->name) - 1; i >= 0 && det->name[i] == ' '; i--) det->name[i] = 0;
    kg_strip_lead(det->name);
}

// Match "<lead-words> <name>" where the trigger phrase ends at substring `key`; the NAME is whatever
// follows `key` in the folded query. Returns true and fills det on a hit.
static bool kg_after(kg_det_t *det, const char *nf, const char *key, kg_dir_t dir, const char *rel) {
    const char *p = strstr(nf, key);
    if (!p) return false;
    size_t at = (size_t)(p - nf) + strlen(key);
    while (nf[at] == ' ') at++;
    if (!nf[at]) return false;
    det->dir = dir; det->rel = rel; kg_take(det, nf, at);
    return det->name[0] && strlen(det->name) >= 2;
}

// Detect a fact question over the folded query. Order matters: inverse/transitive triggers are more
// specific than the bare "capitale" forward trigger, so test them first.
static bool kg_detect(const char *nf, kg_det_t *det) {
    // --- INVERSE: "qual e la capitale della francia" / "capital of france" -> capital⁻¹ (country->city)
    if (kg_after(det, nf, "capitale di ",  DIR_INV, "capital")) return true;
    if (kg_after(det, nf, "capitale del ", DIR_INV, "capital")) return true;
    if (kg_after(det, nf, "capitale dello ", DIR_INV, "capital")) return true;
    if (kg_after(det, nf, "capitale della ", DIR_INV, "capital")) return true;
    if (kg_after(det, nf, "capital of ",   DIR_INV, "capital")) return true;
    // --- TRANSITIVE location: "in che continente e lione" / "dove si trova lione" -> located_in chain
    if (kg_after(det, nf, "in che continente ", DIR_TRANS, "located_in") ||
        kg_after(det, nf, "in quale continente ", DIR_TRANS, "located_in") ||
        kg_after(det, nf, "in che paese ",   DIR_TRANS, "located_in") ||
        kg_after(det, nf, "in quale paese ", DIR_TRANS, "located_in") ||
        kg_after(det, nf, "in che stato ",   DIR_TRANS, "located_in") ||
        kg_after(det, nf, "in quale stato ", DIR_TRANS, "located_in") ||
        kg_after(det, nf, "dove si trova ",  DIR_TRANS, "located_in") ||
        kg_after(det, nf, "what continent ", DIR_TRANS, "located_in") ||
        kg_after(det, nf, "where is ",       DIR_TRANS, "located_in")) {
        // the transitive triggers can swallow a leading "e "/"si trova " — strip a residual connector.
        if (!strncmp(det->name, "e ", 2)) memmove(det->name, det->name + 2, strlen(det->name + 2) + 1);
        if (!strncmp(det->name, "si trova ", 9)) memmove(det->name, det->name + 9, strlen(det->name + 9) + 1);
        kg_strip_lead(det->name);
        return det->name[0] && strlen(det->name) >= 2;
    }
    // --- FORWARD: born / died / author / capital(city->country)
    if (strstr(nf, "nato") || strstr(nf, "nata") || strstr(nf, "nascita")) {
        if (kg_after(det, nf, "nato ",     DIR_FWD, "born") ||
            kg_after(det, nf, "nata ",     DIR_FWD, "born") ||
            kg_after(det, nf, "nascita di ", DIR_FWD, "born") ||
            kg_after(det, nf, "nascita ",  DIR_FWD, "born")) return true;
    }
    // EN "when was/is/were X born": the NAME sits BETWEEN the lead-in and "born" (not after it).
    {
        const char *bp = strstr(nf, " born");
        static const char *leads[] = { "when was ", "when is ", "when were ", "when was ", NULL };
        for (int i = 0; bp && leads[i]; i++) {
            const char *lp = strstr(nf, leads[i]);
            if (!lp || lp >= bp) continue;
            size_t at = (size_t)(lp - nf) + strlen(leads[i]);
            size_t len = (size_t)(bp - (nf + at));
            if (len == 0 || len >= KG_VALLEN) continue;
            char tmp[KG_VALLEN]; memcpy(tmp, nf + at, len); tmp[len] = 0;
            det->dir = DIR_FWD; det->rel = "born";
            snprintf(det->name, sizeof det->name, "%s", tmp);
            kg_strip_lead(det->name);
            if (det->name[0] && strlen(det->name) >= 2) return true;
        }
    }
    if (strstr(nf, "morto") || strstr(nf, "morta")) {
        if (kg_after(det, nf, "morto ", DIR_FWD, "died") ||
            kg_after(det, nf, "morta ", DIR_FWD, "died")) return true;
    }
    if (kg_after(det, nf, "ha scritto ", DIR_FWD, "author")) return true;
    if (kg_after(det, nf, "autore di ",  DIR_FWD, "author")) return true;
    if (kg_after(det, nf, "autore ",     DIR_FWD, "author")) return true;
    if (kg_after(det, nf, "who wrote ",  DIR_FWD, "author")) return true;
    // FORWARD nationality / country-of-origin. Edge "country" (subject -> country value) is in
    // mind.it.jsonl; the FWD read (DIR_FWD) returns the stored value. Edge-grounded: a subject with no
    // country edge falls to the honesty gate (ans.idx<0) and ABSTAINS, so "da dove viene la luce" cannot
    // fabricate. Name-after forms only (no trailing "from", which would collide with the TRANS "where is").
    if (kg_after(det, nf, "di che nazionalita e ", DIR_FWD, "country") ||
        kg_after(det, nf, "di che nazionalita ",   DIR_FWD, "country") ||
        kg_after(det, nf, "di che paese e ",        DIR_FWD, "country") ||
        kg_after(det, nf, "di che nazione e ",      DIR_FWD, "country") ||
        kg_after(det, nf, "da dove viene ",         DIR_FWD, "country") ||
        kg_after(det, nf, "nazionalita di ",        DIR_FWD, "country") ||
        kg_after(det, nf, "what nationality is ",   DIR_FWD, "country") ||
        kg_after(det, nf, "what nationality was ",  DIR_FWD, "country")) return true;
    // forward "capitale di X" is the INVERSE direction (handled above); a bare "X capitale_di Y" forward
    // is not a natural question, so we don't synthesize one.
    return false;
}

// Trim a human label to its salient name for a clarify message: drop anything after the first comma
// ("Frederick Christ Trump Sr., detto Fred," -> "Frederick Christ Trump Sr.") and cap the length.
static void kg_shortname(const char *label, char *out, size_t cap) {
    size_t j = 0;
    for (size_t i = 0; label[i] && j + 1 < cap; i++) {
        if (label[i] == ',') break;
        out[j++] = label[i];
    }
    while (j && out[j-1] == ' ') j--;
    out[j] = 0;
    if (!out[0]) snprintf(out, cap, "%s", label);   // never empty
}

// SUBGRAPH-RELEVANT load: instead of ALL edges of det->rel (which overflowed the KG_MAXENT=12 codebook
// the moment the corpus grew past a dozen geo nodes), load ONLY the subgraph reachable from the detected
// subject, so n stays tiny (2-3) no matter how many triples the mind file holds. Three full-file passes,
// each capped at KG_FILESCAN lines; RAM stays O(KG_MAXENT).
//   SEED   deterministic, exact-preferring, >=4 chars: FWD/TRANS match the edge HEAD slug, INV the TAIL.
//   EXPAND FWD/TRANS = bounded forward BFS (intern tails of edges whose head is interned);
//          INV        = one backward hop (intern heads of edges whose tail == seed).
//   EDGES  every rel edge with BOTH endpoints interned (dup (h,t,k) skipped) — a full rescan, not a
//          frontier pass, so diamond/non-tree internal edges are not dropped.
// Returns the edge count (0 = refuse). Sets g->seed_idx to the subject entity.
static int kg_load_subgraph(kg_t *g, const char *lang, const kg_det_t *det) {
    char path[160];
    snprintf(path, sizeof path, NUCLEO_SD_MOUNT "/data/anima/learned/mind.%s.jsonl",
             (lang && lang[0]) ? lang : "it");
    char qslug[KG_NAMELEN]; kg_slug(det->name, qslug, sizeof qslug);
    if (strlen(qslug) < 4) return 0;                  // matches the >=4 lexical floor of the old guard
    const bool inv = (det->dir == DIR_INV);
    const char *rel = det->rel;
    const int k = rel_shift(rel);
    g->seed_idx = -1;

    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char line[512], hs[KG_NAMELEN], ts[KG_NAMELEN], hl[KG_VALLEN], tv[KG_VALLEN];

    // --- SEED: exact slug wins; else the SHORTEST slug that contains qslug as a segment; else first.
    //     Alongside, collect DISTINCT name-matches for proactive disambiguation (below). ---
    char seed_slug[KG_NAMELEN] = "", seed_label[KG_VALLEN] = "";
    int best_len = 1 << 30; bool exact = false;
    char cand_slug[KG_AMBIG][KG_NAMELEN]; int ncand = 0;   // distinct candidate slugs seen (capped)
    int n = 0;
    while (fgets(line, sizeof line, f) && n < KG_FILESCAN) {
        n++;
        if (!kg_edge(line, rel, hs, ts, hl, tv, KG_NAMELEN)) continue;
        const char *cand  = inv ? ts : hs;
        const char *clabel = inv ? tv : hl;
        if (!slug_seg_match(cand, qslug)) continue;
        bool cexact = !strcmp(cand, qslug);
        int clen = (int)strlen(cand);
        // record each DISTINCT matching entity (slug) + its label, for the ambiguity check after the scan
        bool seen = false;
        for (int j = 0; j < ncand; j++) if (!strcmp(cand_slug[j], cand)) { seen = true; break; }
        if (!seen && ncand < KG_AMBIG) {
            snprintf(cand_slug[ncand], KG_NAMELEN, "%s", cand);
            snprintf(g->ambig[ncand], KG_VALLEN, "%s", (clabel && clabel[0]) ? clabel : cand);
            ncand++;
        }
        if (exact && !cexact) continue;
        if ((cexact && !exact) || clen < best_len) {
            exact = cexact; best_len = clen;
            snprintf(seed_slug, sizeof seed_slug, "%s", cand);
            snprintf(seed_label, sizeof seed_label, "%s", clabel);
        }
    }
    if (!seed_slug[0]) { fclose(f); return 0; }       // subject not a head/tail of this relation -> refuse
    // AMBIGUOUS: a bare/partial name (no exact slug match) that hit >=2 distinct entities. The caller turns
    // this into a proactive clarify instead of answering about whichever one the seed heuristic happened to
    // pick. An EXACT name match ("donald trump") is never ambiguous, so full names still answer directly.
    g->ambig_n = (!exact && ncand >= 2) ? ncand : 0;
    if (g->ambig_n >= 2) { fclose(f); return 1; }     // candidates already in g->ambig[]; skip the expand
    int seed_idx = kg_intern(g, seed_slug, seed_label);
    if (seed_idx < 0) { fclose(f); return 0; }

    // --- EXPAND ---
    if (inv) {                                        // one backward hop: tail==seed -> intern head
        rewind(f); n = 0;
        while (fgets(line, sizeof line, f) && n < KG_FILESCAN) {
            n++;
            if (!kg_edge(line, rel, hs, ts, hl, tv, KG_NAMELEN)) continue;
            if (!strcmp(ts, seed_slug) && kg_find(g, hs) < 0 && g->n < KG_MAXENT)
                kg_intern(g, hs, hl);
        }
    } else {                                          // forward BFS: head interned -> intern tail
        for (int depth = 0; depth < 6; depth++) {
            int before = g->n;
            rewind(f); n = 0;
            while (fgets(line, sizeof line, f) && n < KG_FILESCAN) {
                n++;
                if (!kg_edge(line, rel, hs, ts, hl, tv, KG_NAMELEN)) continue;
                if (kg_find(g, hs) >= 0 && kg_find(g, ts) < 0 && g->n < KG_MAXENT)
                    kg_intern(g, ts, tv);
            }
            if (g->n == before) break;                // frontier exhausted
        }
    }

    // --- EDGES: every rel edge with both endpoints interned (full rescan, dedup) ---
    rewind(f); n = 0;
    while (fgets(line, sizeof line, f) && n < KG_FILESCAN && g->ntrip < KG_MAXTRIP) {
        n++;
        if (!kg_edge(line, rel, hs, ts, hl, tv, KG_NAMELEN)) continue;
        int hi = kg_find(g, hs), ti = kg_find(g, ts);
        if (hi < 0 || ti < 0) continue;
        bool dup = false;
        for (int e = 0; e < g->ntrip; e++)
            if (g->edge_h[e] == hi && g->edge_t[e] == ti && g->edge_k[e] == k) { dup = true; break; }
        if (dup) continue;
        g->edge_h[g->ntrip] = hi; g->edge_t[g->ntrip] = ti; g->edge_k[g->ntrip] = k; g->ntrip++;
    }
    fclose(f);
    g->seed_idx = seed_idx;
    return g->ntrip;
}

// Public entry: try to DEDUCE an answer to `query` from the learned mind. Returns true (and fills out)
// only when a fact question resolves above the coherence gate; false = refuse (let the caller continue).
// Whole-word containment (ASCII, lowercased): `w` in `hay` bounded by non-alphanumerics. Avoids the
// substring trap where a short query token spuriously matches inside a longer word of the subject/answer.
static bool hdc_word_in(const char *hay, const char *w) {
    size_t wl = strlen(w); if (!wl) return false;
    for (const char *p = strstr(hay, w); p; p = strstr(p + 1, w)) {
        char before = (p == hay) ? ' ' : p[-1];
        char after  = p[wl];
        if (!isalnum((unsigned char)before) && !isalnum((unsigned char)after)) return true;
    }
    return false;
}

bool nucleo_anima_hdc_reason(const char *query, const char *lang, anima_result_t *out) {
    if (!query || !out) return false;
    char nf[256]; kg_fold(query, nf, sizeof nf);
    kg_det_t det;
    if (!kg_detect(nf, &det)) return false;            // not a fact question -> skip cheaply

    bool en = lang && (lang[0] == 'e' || lang[0] == 'E');

    // Allocate the int8 scratch the HDC primitives need (freed before every return below).
    g_simcnt = malloc((size_t)HDC_D * sizeof(int8_t));
    g_bcnt   = malloc((size_t)HDC_D * sizeof(int8_t));
    uint32_t *probe = malloc((size_t)2 * HDC_W * sizeof(uint32_t));   // 2 scratch HVs
    kg_t *g = calloc(1, sizeof(kg_t));
    if (!g_simcnt || !g_bcnt || !probe || !g) {
        free(g_simcnt); g_simcnt = NULL; free(g_bcnt); g_bcnt = NULL; free(probe); free(g);
        return false;                                  // OOM -> graceful refuse (no regression)
    }

    bool answered = false;
    int nt = kg_load_subgraph(g, lang ? lang : "it", &det);  // load ONLY the subject's relevant subgraph

    // PROACTIVE DISAMBIGUATION: the bare name matched >=2 distinct entities for this relation. Don't guess —
    // name the candidates and ask. 2 -> "Intendi A o B?"; >2 -> list a few + "sii più preciso". General to
    // every KGE relation (born/country/author/capital). A full/exact name never reaches here, so it answers.
    if (g->ambig_n >= 2) {
        char a0[64], a1[64], a2[64];
        kg_shortname(g->ambig[0], a0, sizeof a0);
        kg_shortname(g->ambig[1], a1, sizeof a1);
        char dreply[256];
        if (g->ambig_n == 2)
            snprintf(dreply, sizeof dreply, en ? "Did you mean %s or %s?" : "Intendi %s o %s?", a0, a1);
        else {
            kg_shortname(g->ambig[2], a2, sizeof a2);
            snprintf(dreply, sizeof dreply,
                en ? "There are several entries for \"%s\" (%s, %s, %s\xE2\x80\xA6). Please be more specific."
                   : "Ci sono pi\xC3\xB9 voci per \"%s\" (%s, %s, %s\xE2\x80\xA6). Sii pi\xC3\xB9 preciso.",
                det.name, a0, a1, a2);
        }
        memset(out, 0, sizeof *out);
        out->tier = ANIMA_TIER_FACT; out->action = ANIMA_ACT_ANSWER;
        snprintf(out->intent, sizeof out->intent, "clarify");
        snprintf(out->state,  sizeof out->state,  "idle");
        snprintf(out->reply,  sizeof out->reply,  "%s", dreply);
        out->confidence = 50;
        answered = true; goto cleanup;
    }
    if (nt <= 0) goto cleanup;                          // subject not a head/tail of this relation -> refuse
    if (!kg_build(g)) goto cleanup;                     // build OOM -> refuse
    if (g->n < 2) goto cleanup;                         // n=1: cleanup() 2nd-best stays at init -> garbage
                                                        // coherence; the gate is meaningless, so refuse here.
    int subj = g->seed_idx, k = rel_shift(det.rel);     // the seed IS the subject (lexically + role correct
    if (subj < 0 || subj >= g->n) goto cleanup;         // by construction: SEED matched the slug + direction)

    cleanup_t ans; const char *ansname = NULL; char reply[256];
    ans.idx = -1; ans.coh = 0;
    if (det.dir == DIR_TRANS) {
        // GENUINE multi-hop DEDUCTION: compose located_in by rotation, hop to hop (snapping to the clean
        // entity vector between hops to denoise), until a TERMINAL node (no outgoing edge). Handles a
        // country (1 hop -> continent) and a city (2 hops -> country -> continent) uniformly; the answer
        // (e.g. Parigi -> Europa) is NEVER stored. Gate on the WEAKEST hop. This is the HDC/KGE showcase.
        int cur = subj; float mincoh = 1e9f;
        for (int hop = 0; hop < 4; hop++) {
            hv_rotate(probe, g->cb + (size_t)cur * HDC_W, k);
            cleanup_t step = cleanup(probe, g->cb, g->n);
            if (step.idx < 0 || step.idx == cur || step.coh < HDC_REASON_GATE) break;
            if (step.coh < mincoh) mincoh = step.coh;
            cur = step.idx;
            bool has_out = false;
            for (int e = 0; e < g->ntrip; e++) if (g->edge_h[e] == cur) { has_out = true; break; }
            if (!has_out) { ans.idx = cur; ans.coh = mincoh; break; }   // reached the terminal (continent)
        }
    } else {
        // FWD (born/author) and INV (capital): the answer is a DIRECTLY STORED edge. Read it from the
        // subgraph deterministically — the n=2 HDC cleanup has data-dependent coherence variance that can
        // dip below the gate for some slug pairs (e.g. kenya/nairobi vs cile/santiago), and the edge IS
        // the fact, so it is the correct, robust source. (HDC stays the engine for the TRANS deduction.)
        for (int e = 0; e < g->ntrip; e++) {
            if (det.dir == DIR_INV && g->edge_t[e] == subj) { ans.idx = g->edge_h[e]; break; }   // tail==subj -> head
            if (det.dir == DIR_FWD && g->edge_h[e] == subj) { ans.idx = g->edge_t[e]; break; }   // head==subj -> tail
        }
        if (ans.idx >= 0) ans.coh = HDC_REASON_GATE + 1.0f;            // edge-grounded -> above gate
    }
    if (ans.idx < 0 || ans.coh < HDC_REASON_GATE) goto cleanup;   // honesty gate
    if (ans.idx == subj) goto cleanup;                 // a relation must MOVE off the subject
    ansname = g->label[ans.idx][0] ? g->label[ans.idx] : g->name[ans.idx];

    // Build the human reply (the value/label of the deduced entity).
    {
        const char *who = g->label[subj][0] ? g->label[subj] : g->name[subj];
        if (det.dir == DIR_INV)
            snprintf(reply, sizeof reply, en ? "%s is the capital of %s." : "%s e la capitale di %s.", ansname, who);
        else if (det.dir == DIR_TRANS)
            snprintf(reply, sizeof reply, en ? "%s is in %s." : "%s si trova in %s.", who, ansname);
        else if (!strcmp(det.rel, "born"))
            snprintf(reply, sizeof reply, en ? "%s was born on %s." : "%s e nato/a il %s.", who, ansname);
        else if (!strcmp(det.rel, "died"))
            snprintf(reply, sizeof reply, en ? "%s died on %s." : "%s e morto/a il %s.", who, ansname);
        else if (!strcmp(det.rel, "author"))
            snprintf(reply, sizeof reply, en ? "%s was written by %s." : "%s e stato scritto da %s.", who, ansname);
        else if (!strcmp(det.rel, "country"))
            snprintf(reply, sizeof reply, en ? "%s is from %s." : "%s e di %s.", who, ansname);
        else
            snprintf(reply, sizeof reply, "%s: %s.", who, ansname);
    }

    // EVIDENCE COVERAGE (KGE specificity guard): the deduction used subject `who`. If the query carries
    // an EXTRA, MORE-SPECIFIC entity token that neither the subject nor the answer covers — "capitale
    // dello stato del KARNATAKA in india", where the subgraph seed snapped to the broader "india" — the
    // reasoner answered a different (broader) question. Refuse rather than assert the wrong-scope fact.
    // Relation/question words are whitelisted, so the genuine deductions (capitale/continente/nato…) pass.
    {
        const char *who_cov = g->label[subj][0] ? g->label[subj] : g->name[subj];
        char wf[128], af[128];
        kg_fold(who_cov, wf, sizeof wf);
        kg_fold(ansname, af, sizeof af);
        static const char *const wl[] = {
            // question + relation words (the deduction's own vocabulary — must not count as "uncovered")
            "qual","quale","quali","cosa","chi","quando","dove","come",
            "anno","anni","year","data","date","era","quel","quella",
            "capitale","capital","continente","continent","paese","country","stato","state","regione",
            "nazionalita","nazione","nationality","viene","provenienza",   // nationality/country-of-origin question words
            "provincia","citta","city","nato","nata","nascita","born","morto","morta","died","autore",
            "author","scritto","wrote","trova","where","when","what","mondo","world",
            // articles / prepositions (structural glue, never a restrictive entity)
            "dello","della","delle","degli","dell","nella","nello","sulla","sullo","questo","quella", NULL };
        for (const char *p = nf; *p; ) {
            while (*p == ' ') p++;
            char w[40]; int k = 0; while (*p && *p != ' ' && k < 39) w[k++] = *p++; w[k] = 0;
            if (k < 4) continue;
            bool ok = false;
            for (int i = 0; wl[i]; i++) if (!strcmp(w, wl[i])) { ok = true; break; }
            if (ok) continue;
            if (hdc_word_in(wf, w) || hdc_word_in(af, w)) continue;  // covered by the subject or the answer
            goto cleanup;                                  // uncovered salient entity -> scope mismatch -> refuse
        }
    }

    memset(out, 0, sizeof *out);
    out->tier   = ANIMA_TIER_FACT;
    out->action = ANIMA_ACT_ANSWER;
    snprintf(out->intent, sizeof out->intent, "hdc");
    snprintf(out->state,  sizeof out->state,  "idle");
    snprintf(out->reply,  sizeof out->reply,  "%s", reply);
    int conf = 80 + (int)(ans.coh + 0.5f);             // coherence -> confidence (80..95)
    out->confidence = conf > 95 ? 95 : conf;
    // Declare the structured focus: the relation token used and the resolved subject entity. Lets the
    // orchestrator re-aim THIS reasoner on a bare follow-up ("e newton?") without any text subtraction.
    snprintf(out->relation, sizeof out->relation, "%s", det.rel);
    snprintf(out->subject,  sizeof out->subject,  "%s",
             g->label[subj][0] ? g->label[subj] : g->name[subj]);
    answered = true;
    ESP_LOGI(TAG, "reason: '%s' -> %s (dir=%d coh=%.1f)", det.name, ansname, (int)det.dir, ans.coh);

cleanup:
    kg_free(g);
    free(g); free(probe);
    free(g_simcnt); g_simcnt = NULL;
    free(g_bcnt);   g_bcnt = NULL;
    return answered;
}

// Detect-only companion: if `query` is a fact question, report the structured relation token and the
// extracted entity name WITHOUT building the KG or reasoning (a cheap fold + pattern match). Lets the
// orchestrator capture the conversational focus from the QUERY's structure even when another tier (an
// L1 card) produced the answer. Returns false (and leaves the buffers untouched) on a non-fact query.
bool nucleo_anima_hdc_detect(const char *query, char *rel, size_t rcap, char *subj, size_t scap) {
    if (!query) return false;
    char nf[256]; kg_fold(query, nf, sizeof nf);
    kg_det_t det;
    if (!kg_detect(nf, &det)) return false;
    if (!det.name[0]) return false;
    if (rel  && rcap) snprintf(rel,  rcap, "%s", det.rel);
    if (subj && scap) snprintf(subj, scap, "%s", det.name);
    return true;
}

// =====================================================================================================
// NEURO-SYMBOLIC COMBINATOR TIER — COMPUTE answers by COMPOSING >=2 learned facts (port of
// tools/anima/combinator.mjs). The leap past recall/deduction: "chi e nato prima Dante o Einstein",
// "quanti anni tra la nascita di Dante e Einstein", "Einstein era europeo", "Dante e Colombo erano
// connazionali" — answers that exist NOWHERE as a single stored triple until you combine facts.
//
// HONEST by construction: a typed composition (compare / subtract / geo-contain / equal) over facts
// SUPPLIED by the same learned triple store the deductive tier reads (mind.<lang>.jsonl). No generation
// -> cannot hallucinate. If ANY required fact is missing it does NOT fabricate: it returns an honest
// "non ho i dati" miss (still answered=true, to STOP the cascade so no later entity/recall tier hands
// back a random bio for what was clearly a compositional question — mirrors the sim's `compositional`
// anti-hijack flag). Confidence = min of the source confidences (evidential: the weakest source bounds
// it). RAM: NO HDC vectors, NO codebook — a single bounded line scan of the mind file. Tiny + transient.
// =====================================================================================================
#define COMB_VALLEN 96
#define COMB_MAXSCAN 1024   // per-lookup line budget for comb_get_fact — INDEPENDENT of the KG edge cap,
                            // so a born/country fact deep in a ~1100-line mind file still resolves
                            // (the old shared KG_MAXTRIP=48 made everything past line 48 invisible).
                            // RAM-free: a bounded sequential line scan, no hypervectors.

// getFact oracle: scan mind.<lang>.jsonl for a triple whose head (label, else subject) slug-matches
// `ent_slug` AND whose rel is one of `rels` (NUL-terminated list, NULL-terminated array). Copies the
// value into `value` and the source confidence (born=95, else 90) into `*conf`. Returns true on a hit.
// Allocation-free per line (fixed stack buffers); one sequential pass, bounded by KG_MAXTRIP lines.
static bool comb_get_fact(const char *lang, const char *ent_slug,
                          const char *const *rels, char *value, size_t vcap, int *conf) {
    char path[160];
    snprintf(path, sizeof path, NUCLEO_SD_MOUNT "/data/anima/learned/mind.%s.jsonl",
             (lang && lang[0]) ? lang : "it");
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char line[512];
    int scanned = 0;
    bool hit = false;
    while (!hit && scanned < COMB_MAXSCAN && fgets(line, sizeof line, f)) {
        scanned++;
        char subj[COMB_VALLEN], rel[KG_NAMELEN], val[COMB_VALLEN], label[COMB_VALLEN];
        if (!kg_json_str(line, "subject", subj, sizeof subj)) continue;
        if (!kg_json_str(line, "rel",     rel,  sizeof rel))  continue;
        if (!kg_json_str(line, "value",   val,  sizeof val))  continue;
        // rel must be in the requested set
        bool rel_ok = false;
        for (int i = 0; rels[i]; i++) if (strcmp(rel, rels[i]) == 0) { rel_ok = true; break; }
        if (!rel_ok) continue;
        if (!kg_json_str(line, "label", label, sizeof label)) label[0] = 0;
        // head identity: slug of the human label if present, else the subject.
        char hslug[KG_NAMELEN];
        kg_slug(label[0] ? label : subj, hslug, sizeof hslug);
        if (!hslug[0]) continue;
        // SEGMENT match (not substring): "einstein"~"albert-einstein" and "dante"~"dante-alighieri" match,
        // but at ~hundreds of people a short surname ("ali","john","west") no longer binds to an unrelated
        // person whose slug merely contains those letters. Min 3 chars; "pinco" never matches.
        bool match = slug_seg_match(hslug, ent_slug) && strlen(ent_slug) >= 3;
        if (!match) continue;
        snprintf(value, vcap, "%s", val);
        if (conf) *conf = 90;
        hit = true;
    }
    fclose(f);
    return hit;
}

// born-year fact: pull the FIRST 4-digit run (a year) out of the born value ("14 marzo 1879" -> 1879).
// Returns the year, or 0 if no born fact / no 4-digit year (a missing fact -> caller refuses).
static int comb_get_year(const char *lang, const char *ent_slug, int *conf) {
    static const char *const rels[] = { "born", NULL };
    char val[COMB_VALLEN];
    if (!comb_get_fact(lang, ent_slug, rels, val, sizeof val, conf)) return 0;
    if (conf) *conf = 95;   // a born fact is high-confidence (evidential 0.95, mirrors the reference)
    for (const char *p = val; *p; p++) {
        if (p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9' &&
            p[2] >= '0' && p[2] <= '9' && p[3] >= '0' && p[3] <= '9' &&
            !(p[4] >= '0' && p[4] <= '9') &&
            (p == val || !(p[-1] >= '0' && p[-1] <= '9')))
            return (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
    }
    return 0;   // value present but no parsable year -> refuse (year 0)
}

// country/nationality fact -> human value ("Italia"). Returns true on a hit.
static bool comb_get_country(const char *lang, const char *ent_slug, char *out, size_t cap, int *conf) {
    static const char *const rels[] = { "country", "nationality", NULL };
    return comb_get_fact(lang, ent_slug, rels, out, cap, conf);
}
// continent of a country ("Italia" -> "Europa"). `located_in` covers the geo store that already encodes
// country->continent; an explicit `continent` rel is preferred. Returns true on a hit.
static bool comb_get_continent(const char *lang, const char *country_slug, char *out, size_t cap, int *conf) {
    static const char *const rels[] = { "continent", "located_in", NULL };
    return comb_get_fact(lang, country_slug, rels, out, cap, conf);
}

// Capitalize the first letter of each whitespace-separated word (ASCII), in place.
static void comb_cap(char *s) {
    int start = 1;
    for (char *p = s; *p; p++) {
        if (start && *p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
        start = (*p == ' ' || *p == '-');
    }
}

// Strip a leading article/preposition from an extracted entity (ASCII subset of cleanEnt's regex).
static void comb_strip_art(char *s) {
    static const char *art[] = { "il ", "lo ", "la ", "l ", "i ", "gli ", "le ",
                                 "un ", "uno ", "una ", NULL };
    for (int i = 0; art[i]; i++) {
        size_t l = strlen(art[i]);
        if (!strncmp(s, art[i], l)) { memmove(s, s + l, strlen(s + l) + 1); return; }
    }
}
// Trim trailing spaces in place.
static void comb_rtrim(char *s) {
    for (int i = (int)strlen(s) - 1; i >= 0 && s[i] == ' '; i--) s[i] = 0;
}

typedef enum { OP_NONE, OP_OLDER, OP_YOUNGER, OP_DIFF, OP_CONT, OP_SAMECTRY } comb_op_t;
typedef struct { comb_op_t op; char a[COMB_VALLEN]; char b[COMB_VALLEN]; char cont[24]; } comb_q_t;

// Map an Italian demonym (already accent-folded, lowercase) to its continent slug. Empty -> no match.
static const char *comb_demonym(const char *d) {
    if (!strncmp(d, "europe", 6))   return "europa";   // europeo / european
    if (!strncmp(d, "asiatic", 7))  return "asia";     // asiatico
    if (!strncmp(d, "asian", 5))    return "asia";     // asian (EN)
    if (!strncmp(d, "american", 8)) return "america";  // americano / american
    if (!strncmp(d, "african", 7))  return "africa";   // africano / african
    if (!strncmp(d, "oceanian", 8)) return "oceania";  // oceaniano / oceanian
    return NULL;
}

// Split "<A> <sep-word> <B>" on the FIRST standalone occurrence of separator token `sep` (e.g. " o ",
// " e ", " ed "). Fills a/b (article-stripped, trimmed). Returns true if both sides are non-empty.
static bool comb_split(const char *s, const char *sep, char *a, size_t acap, char *b, size_t bcap) {
    const char *p = strstr(s, sep);
    if (!p) return false;
    size_t alen = (size_t)(p - s);
    if (alen == 0 || alen + 1 >= acap) return false;
    memcpy(a, s, alen); a[alen] = 0;
    snprintf(b, bcap, "%s", p + strlen(sep));
    comb_rtrim(a); comb_strip_art(a);
    comb_rtrim(b); comb_strip_art(b);
    return a[0] && b[0];
}

// Parse the folded query into a composition op (port of combinator.mjs parseQuery). Returns true on a
// recognized compositional pattern; the caller then COMPUTES (or refuses if a fact is missing).
static bool comb_parse(const char *nf, comb_q_t *p) {
    memset(p, 0, sizeof *p);
    const char *s = nf;
    const char *rest;

    // --- "chi e nato prima A o B" / "chi e piu vecchio|anziano A o B" -> OLDER
    if (!strncmp(s, "chi e nato prima ", 17))      rest = s + 17, p->op = OP_OLDER;
    else if (!strncmp(s, "chi e piu vecchio ", 18)) rest = s + 18, p->op = OP_OLDER;
    else if (!strncmp(s, "chi e piu anziano ", 18)) rest = s + 18, p->op = OP_OLDER;
    // --- "chi e piu giovane|recente A o B" / "chi e nato dopo A o B" -> YOUNGER
    else if (!strncmp(s, "chi e piu giovane ", 18)) rest = s + 18, p->op = OP_YOUNGER;
    else if (!strncmp(s, "chi e piu recente ", 18)) rest = s + 18, p->op = OP_YOUNGER;
    else if (!strncmp(s, "chi e nato dopo ", 16))   rest = s + 16, p->op = OP_YOUNGER;
    // EN: "who is older|who was born first ... A or B", "who is younger|who was born later ... A or B"
    else if (!strncmp(s, "who is older ", 13))       rest = s + 13, p->op = OP_OLDER;
    else if (!strncmp(s, "who was born first ", 19)) rest = s + 19, p->op = OP_OLDER;
    else if (!strncmp(s, "who is younger ", 15))     rest = s + 15, p->op = OP_YOUNGER;
    else if (!strncmp(s, "who was born later ", 19)) rest = s + 19, p->op = OP_YOUNGER;
    else rest = NULL;
    if (rest) {
        if (*rest == ',') rest++;
        while (*rest == ' ') rest++;
        if (!strncmp(rest, "tra ", 4)) rest += 4;        // "chi e piu vecchio TRA A E B" (natural Italian)
        else if (!strncmp(rest, "fra ", 4)) rest += 4;
        else if (!strncmp(rest, "between ", 8)) rest += 8;   // "who is older BETWEEN A AND B" (EN)
        // operands joined by " o "/" or " (A o B) OR " ed "/" and "/" e " (tra A e B)
        if (comb_split(rest, " o ",   p->a, sizeof p->a, p->b, sizeof p->b) ||
            comb_split(rest, " or ",  p->a, sizeof p->a, p->b, sizeof p->b) ||
            comb_split(rest, " ed ",  p->a, sizeof p->a, p->b, sizeof p->b) ||
            comb_split(rest, " and ", p->a, sizeof p->a, p->b, sizeof p->b) ||
            comb_split(rest, " e ",   p->a, sizeof p->a, p->b, sizeof p->b)) return true;
        p->op = OP_NONE;
    }

    // --- "quanti anni [passano|ci sono] tra|fra|separano|dividono|intercorrono [la nascita di] A e[d] B"
    if ((rest = strstr(s, "quanti anni")) || (rest = strstr(s, "how many years"))) {
        static const char *seps[] = { " tra ", " fra ", " between ", " separano ", " dividono ", " intercorrono ", NULL };
        const char *mid = NULL; size_t seplen = 0;
        for (int i = 0; seps[i]; i++) { const char *m = strstr(rest, seps[i]); if (m) { mid = m + strlen(seps[i]); seplen = strlen(seps[i]); break; } }
        (void)seplen;
        if (mid) {
            while (*mid == ' ') mid++;
            if (!strncmp(mid, "la nascita di ", 14)) mid += 14;
            else if (!strncmp(mid, "the birth of ", 13)) mid += 13;
            // operands joined by " ed "/" and " (preferred) or " e "
            char tmp[256]; snprintf(tmp, sizeof tmp, "%s", mid);
            if (comb_split(tmp, " ed ",  p->a, sizeof p->a, p->b, sizeof p->b) ||
                comb_split(tmp, " and ", p->a, sizeof p->a, p->b, sizeof p->b) ||
                comb_split(tmp, " e ",   p->a, sizeof p->a, p->b, sizeof p->b)) {
                // a possible "quella di " / "la nascita di " prefix on B
                if (!strncmp(p->b, "quella di ", 10)) { memmove(p->b, p->b + 10, strlen(p->b + 10) + 1); comb_strip_art(p->b); }
                if (!strncmp(p->b, "la nascita di ", 14)) { memmove(p->b, p->b + 14, strlen(p->b + 14) + 1); comb_strip_art(p->b); }
                p->op = OP_DIFF;
                return true;
            }
        }
    }

    // --- "A e[ra] europeo|asiatico|americano|africano|oceaniano" -> CONTINENT membership
    {
        static const char *dem[] = { "europeo","europea","asiatico","asiatica","americano","americana",
                                     "africano","africana","oceaniano","oceaniana",
                                     "european","asian","american","african","oceanian", NULL };
        for (int i = 0; dem[i]; i++) {
            char needle[24]; snprintf(needle, sizeof needle, " %s", dem[i]);
            const char *d = strstr(s, needle);
            if (!d) continue;
            // the demonym must be preceded by a copula: " era "/" e " (IT) or " was "/" is " (EN)
            // subject: IT statement "X era <dem>" (subject BEFORE the copula) OR EN/IT question
            // "was/is/were/are X <dem>" (subject BETWEEN a LEADING copula and the demonym).
            const char *a_start = s, *a_end = d;
            static const char *qlead[] = { "was ", "is ", "were ", "are ", NULL };
            bool isq = false;
            for (int j = 0; qlead[j]; j++) { size_t l = strlen(qlead[j]); if (!strncmp(s, qlead[j], l)) { a_start = s + l; isq = true; break; } }
            if (!isq) {
                const char *cop = NULL;
                const char *cands[2] = { strstr(s, " era "), strstr(s, " e ") };
                for (int j = 0; j < 2; j++) if (cands[j] && cands[j] < d && (!cop || cands[j] > cop)) cop = cands[j];
                if (!cop) continue;
                a_end = cop;
            }
            size_t alen = (size_t)(a_end - a_start);
            if (alen == 0 || alen + 1 >= sizeof p->a) continue;
            memcpy(p->a, a_start, alen); p->a[alen] = 0;
            comb_rtrim(p->a); comb_strip_art(p->a);
            const char *c = comb_demonym(dem[i]);
            if (!p->a[0] || !c) continue;
            snprintf(p->cont, sizeof p->cont, "%s", c);
            p->op = OP_CONT;
            return true;
        }
    }

    // --- "A e B erano|sono connazionali | dello stesso paese | della stessa nazione" -> SAME COUNTRY
    {
        // IT tails carry the verb ("A e B erano connazionali"); EN question tails are ENDINGS
        // ("were A and B compatriots") with the verb at the FRONT of the head, stripped below.
        static const char *tails[] = {
            " erano connazionali", " sono connazionali", " erano dello stesso paese",
            " sono dello stesso paese", " erano della stessa nazione", " sono della stessa nazione",
            " compatriots", " from the same country", " of the same nationality", NULL };
        const char *tail = NULL;
        for (int i = 0; tails[i]; i++) { const char *t = strstr(s, tails[i]); if (t) { tail = t; break; } }
        if (tail) {
            char head[256];
            size_t hl = (size_t)(tail - s);
            if (hl > 0 && hl < sizeof head) {
                memcpy(head, s, hl); head[hl] = 0;
                static const char *lead[] = { "were ", "are ", "was ", "is ", NULL };   // EN question copula
                for (int j = 0; lead[j]; j++) { size_t l = strlen(lead[j]); if (!strncmp(head, lead[j], l)) { memmove(head, head + l, strlen(head + l) + 1); break; } }
                if (comb_split(head, " e ",   p->a, sizeof p->a, p->b, sizeof p->b) ||
                    comb_split(head, " and ", p->a, sizeof p->a, p->b, sizeof p->b)) {
                    p->op = OP_SAMECTRY;
                    return true;
                }
            }
        }
    }

    return false;
}

// Public entry: COMPUTE a compositional answer from the learned triples. Returns true (and fills *out)
// when the query IS compositional — EITHER with the computed answer, OR with an honest "non ho i dati"
// miss when a required fact is absent (so the cascade STOPS and no later tier fabricates a bio for it).
// Returns false ONLY when the query is not a recognized composition (then the cascade continues).
bool nucleo_anima_combinator(const char *query, const char *lang, anima_result_t *out) {
    if (!query || !out) return false;
    char nf[256]; kg_fold(query, nf, sizeof nf);
    comb_q_t p;
    if (!comb_parse(nf, &p)) return false;       // not compositional -> let the cascade continue

    bool en = lang && (lang[0] == 'e' || lang[0] == 'E');
    char reply[256] = {0};
    int  conf = 0;
    bool computed = false;

    char aslug[KG_NAMELEN], bslug[KG_NAMELEN];
    kg_slug(p.a, aslug, sizeof aslug);
    kg_slug(p.b, bslug, sizeof bslug);

    if (p.op == OP_OLDER || p.op == OP_YOUNGER) {
        int ca = 0, cb = 0;
        int ya = comb_get_year(lang, aslug, &ca);
        int yb = comb_get_year(lang, bslug, &cb);
        if (ya && yb) {
            char an[COMB_VALLEN], bn[COMB_VALLEN];
            snprintf(an, sizeof an, "%s", p.a); comb_cap(an);
            snprintf(bn, sizeof bn, "%s", p.b); comb_cap(bn);
            // earlier = smaller year
            const char *en_name = (ya <= yb) ? an : bn; int en_y = (ya <= yb) ? ya : yb;
            const char *la_name = (ya <= yb) ? bn : an; int la_y = (ya <= yb) ? yb : ya;
            const char *pick_name = (p.op == OP_OLDER) ? en_name : la_name;
            int pick_y = (p.op == OP_OLDER) ? en_y : la_y;
            snprintf(reply, sizeof reply,
                     en ? "%s (%d). %s was born in %d, %s in %d."
                        : "%s (%d). %s e nato nel %d, %s nel %d.",
                     pick_name, pick_y, en_name, en_y, la_name, la_y);
            conf = (ca < cb ? ca : cb);
            computed = true;
        }
    } else if (p.op == OP_DIFF) {
        int ca = 0, cb = 0;
        int ya = comb_get_year(lang, aslug, &ca);
        int yb = comb_get_year(lang, bslug, &cb);
        if (ya && yb) {
            int d = ya > yb ? ya - yb : yb - ya;
            char an[COMB_VALLEN], bn[COMB_VALLEN];
            snprintf(an, sizeof an, "%s", p.a); comb_cap(an);
            snprintf(bn, sizeof bn, "%s", p.b); comb_cap(bn);
            snprintf(reply, sizeof reply,
                     en ? "Between the birth of %s (%d) and that of %s (%d) there are %d years."
                        : "Tra la nascita di %s (%d) e quella di %s (%d) ci sono %d anni.",
                     an, ya, bn, yb, d);
            conf = (ca < cb ? ca : cb);
            computed = true;
        }
    } else if (p.op == OP_CONT) {
        char country[COMB_VALLEN]; int cc = 0;
        if (comb_get_country(lang, aslug, country, sizeof country, &cc)) {
            char cslug[KG_NAMELEN]; kg_slug(country, cslug, sizeof cslug);
            char cont[COMB_VALLEN]; int ck = 0;
            if (comb_get_continent(lang, cslug, cont, sizeof cont, &ck)) {
                char contslug[KG_NAMELEN]; kg_slug(cont, contslug, sizeof contslug);
                if (!strncmp(contslug, "europ", 5)) strcpy(contslug, "europa");   // EN 'europe' == IT 'europa'
                bool yes = (strstr(contslug, p.cont) || strstr(p.cont, contslug));
                char an[COMB_VALLEN]; snprintf(an, sizeof an, "%s", p.a); comb_cap(an);
                char wantcap[24]; snprintf(wantcap, sizeof wantcap, "%s", p.cont); comb_cap(wantcap);
                if (en && !strcmp(wantcap, "Europa")) strcpy(wantcap, "Europe");   // display the asked continent in EN
                if (yes)
                    snprintf(reply, sizeof reply,
                             en ? "Yes: %s was from %s, which is in %s."
                                : "Si: %s era di %s, che e in %s.", an, country, cont);
                else
                    snprintf(reply, sizeof reply,
                             en ? "No: %s was from %s, which is in %s, not in %s."
                                : "No: %s era di %s, che e in %s, non in %s.", an, country, cont, wantcap);
                conf = (cc < ck ? cc : ck);
                computed = true;
            }
        }
    } else if (p.op == OP_SAMECTRY) {
        char cta[COMB_VALLEN], ctb[COMB_VALLEN]; int ca = 0, cb = 0;
        if (comb_get_country(lang, aslug, cta, sizeof cta, &ca) &&
            comb_get_country(lang, bslug, ctb, sizeof ctb, &cb)) {
            char sa[KG_NAMELEN], sb[KG_NAMELEN];
            kg_slug(cta, sa, sizeof sa); kg_slug(ctb, sb, sizeof sb);
            bool same = strcmp(sa, sb) == 0;
            char an[COMB_VALLEN], bn[COMB_VALLEN];
            snprintf(an, sizeof an, "%s", p.a); comb_cap(an);
            snprintf(bn, sizeof bn, "%s", p.b); comb_cap(bn);
            if (same)
                snprintf(reply, sizeof reply,
                         en ? "Yes: %s and %s were both from %s."
                            : "Si: %s e %s erano entrambi di %s.", an, bn, cta);
            else
                snprintf(reply, sizeof reply,
                         en ? "No: %s was from %s, %s from %s."
                            : "No: %s era di %s, %s di %s.", an, cta, bn, ctb);
            conf = (ca < cb ? ca : cb);
            computed = true;
        }
    }

    memset(out, 0, sizeof *out);
    out->tier   = ANIMA_TIER_FACT;
    out->action = ANIMA_ACT_ANSWER;
    snprintf(out->state, sizeof out->state, "idle");
    if (computed) {
        snprintf(out->intent, sizeof out->intent, "combinator");
        snprintf(out->reply,  sizeof out->reply,  "%s", reply);
        out->confidence = conf > 0 ? conf : 90;
        ESP_LOGI(TAG, "combinator: op=%d -> %s (conf %d)", (int)p.op, reply, out->confidence);
    } else {
        // Compositional question, but a required fact is missing -> REFUSE honestly. Still return true so
        // the cascade STOPS here: a later entity/recall tier must NOT answer a comparison with a random bio.
        snprintf(out->intent, sizeof out->intent, "combinator");
        snprintf(out->reply,  sizeof out->reply,
                 en ? "I don't have the data to work that out."
                    : "Non ho i dati per calcolarlo.");
        out->confidence = 0;
        ESP_LOGI(TAG, "combinator: op=%d compositional but a fact is missing -> honest refuse", (int)p.op);
    }
    return true;
}

// =====================================================================================================
// NSPCG — NEURO-SYMBOLIC PROOF-CARRYING GENERATION (port of tools/anima/pcg.mjs). The leap past
// recall/deduction/composition: ANIMA GENERATES a sentence that exists NOWHERE in the corpus, carrying
// its own machine-checkable proof — and stays HALLUCINATION-IMPOSSIBLE BY CONSTRUCTION.
//
//   • the deductive tier (hdc_reason) WALKS a single relation forward ("Parigi -> Europa");
//   • the combinator COMPUTES over >=2 facts but only with fixed templates;
//   • NSPCG DISCOVERS a MIXED-relation derivation chain by itself, VERBALIZES it into a novel grounded
//     clause-by-clause explanation, and attaches a proof that re-derives the claim from scratch.
//
// Example on the device's own mind: "perche Einstein e in Europa?" -> it never learned that edge, but it
// finds  Einstein --(country)--> Germania --(located_in)--> Europa,  proves every hop is a STORED triple,
// and answers "Albert Einstein e di un paese che si trova in Europa, perche Albert Einstein e di Germania
// e Germania si trova in Europa." Cross-domain (biography + geography), assembled, grounded, refusable.
//
// HOW IT STAYS HONEST: each hop is proposed by HDC (rotate the entity vector by the relation, cleanup,
// gate on resonance coherence) AND must CONCUR with the symbol store (a real stored edge to the SAME
// entity). HDC discovers, symbols verify — if they disagree, or no chain reaches the target, it REFUSES
// (returns false) instead of confabulating a bridge. A final pcg_verify() re-checks the whole proof tree
// (every hop stored, chain connected end-to-end, endpoints + closure relation correct) before answering.
//
// RAM: identical envelope to hdc_reason (same kg_load/kg_build/cleanup on <=KG_MAXENT entity HVs + the
// int8 scratch, all malloc'd transiently and freed before return). Additive: called on an offline miss,
// after combinator + hdc_reason, so it can only ADD answers the cascade was about to send to the network.
// =====================================================================================================
#define PCG_NREL     3          // the chain-able relations (containment + provenance) from the mind file
#define PCG_MAXDEPTH 4          // longest derivation chain explored (matches pcg.mjs maxDepth)
#define PCG_BEAM     4          // beam width of the autonomous search (matches kge.mjs reach)
static const char *const PCG_RELS[PCG_NREL] = { "located_in", "capital", "country" };

// Map a relation name <-> its rotation amount (rel_shift). country/located_in/capital all "climb" toward
// a larger container, so a forward walk from a city/person reaches its country then its continent.
static int pcg_relidx(const char *rel) {
    for (int i = 0; i < PCG_NREL; i++) if (!strcmp(rel, PCG_RELS[i])) return i;
    return -1;
}
static const char *pcg_relname(int k) {
    for (int i = 0; i < PCG_NREL; i++) if (rel_shift(PCG_RELS[i]) == k) return PCG_RELS[i];
    return "rel";
}
// located_in + capital are pure geographic containment (city->country->continent); country (person->
// country) is provenance — a path that STARTS with it reads "is from a country (transitively) in <T>".
static bool pcg_is_containment(int k) { return k == rel_shift("located_in") || k == rel_shift("capital"); }

// Read one mind line as an edge over ANY of the PCG relations; reports which one (relidx). Mirrors
// kg_edge but accepts the small relation SET and returns the match index. Allocation-free.
static bool pcg_read_edge(const char *line, int *relidx,
                          char *hslug, char *tslug, char *hlabel, char *tval, size_t cap) {
    char subj[KG_VALLEN], rel[KG_NAMELEN], val[KG_VALLEN], label[KG_VALLEN];
    if (!kg_json_str(line, "subject", subj, sizeof subj)) return false;
    if (!kg_json_str(line, "rel",     rel,  sizeof rel))  return false;
    if (!kg_json_str(line, "value",   val,  sizeof val))  return false;
    int ri = pcg_relidx(rel);
    if (ri < 0) return false;
    if (!kg_json_str(line, "label", label, sizeof label)) label[0] = 0;
    kg_slug(label[0] ? label : subj, hslug, cap);
    kg_slug(val, tslug, cap);
    snprintf(hlabel, cap, "%s", label[0] ? label : subj);
    snprintf(tval,   cap, "%s", val);
    *relidx = ri;
    return hslug[0] && tslug[0];
}

// Load the MIXED-relation subgraph reachable forward from `headname` (the chain's start), over the PCG
// relations only — so the codebook stays tiny (a city/person -> country -> continent is 2-3 nodes) no
// matter how big the mind file is. Three bounded passes (seed / forward-BFS / edge-collect). Reuses
// kg_intern/kg_find. Returns the edge count (0 = head not found). Sets g->seed_idx to the head entity.
static int pcg_load(kg_t *g, const char *lang, const char *headname) {
    char path[160];
    snprintf(path, sizeof path, NUCLEO_SD_MOUNT "/data/anima/learned/mind.%s.jsonl",
             (lang && lang[0]) ? lang : "it");
    char qslug[KG_NAMELEN]; kg_slug(headname, qslug, sizeof qslug);
    if (strlen(qslug) < 3) return 0;
    g->seed_idx = -1;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char line[512], hs[KG_NAMELEN], ts[KG_NAMELEN], hl[KG_VALLEN], tv[KG_VALLEN];
    int ri;

    // SEED: the head entity (a HEAD of some PCG edge) — exact slug wins, else the SHORTEST slug that
    // contains qslug as a segment ("einstein" -> "albert-einstein"). The chain always starts here.
    char seed_slug[KG_NAMELEN] = "", seed_label[KG_VALLEN] = "";
    int best_len = 1 << 30; bool exact = false; int scanned = 0;
    while (fgets(line, sizeof line, f) && scanned < KG_FILESCAN) {
        scanned++;
        if (!pcg_read_edge(line, &ri, hs, ts, hl, tv, KG_NAMELEN)) continue;
        if (!slug_seg_match(hs, qslug)) continue;
        bool cexact = !strcmp(hs, qslug); int clen = (int)strlen(hs);
        if (exact && !cexact) continue;
        if ((cexact && !exact) || clen < best_len) {
            exact = cexact; best_len = clen;
            snprintf(seed_slug, sizeof seed_slug, "%s", hs);
            snprintf(seed_label, sizeof seed_label, "%s", hl);
        }
    }
    if (!seed_slug[0]) { fclose(f); return 0; }
    int seed_idx = kg_intern(g, seed_slug, seed_label);
    if (seed_idx < 0) { fclose(f); return 0; }

    // EXPAND: forward BFS — intern the tail of any PCG edge whose head is already interned.
    for (int depth = 0; depth < 6; depth++) {
        int before = g->n;
        rewind(f); scanned = 0;
        while (fgets(line, sizeof line, f) && scanned < KG_FILESCAN) {
            scanned++;
            if (!pcg_read_edge(line, &ri, hs, ts, hl, tv, KG_NAMELEN)) continue;
            if (kg_find(g, hs) >= 0 && kg_find(g, ts) < 0 && g->n < KG_MAXENT)
                kg_intern(g, ts, tv);
        }
        if (g->n == before) break;
    }

    // EDGES: every PCG edge with BOTH endpoints interned (dedup on h,t,k). A city carrying both
    // capital->country AND located_in->country keeps both (distinct k) — the search picks the best.
    rewind(f); scanned = 0;
    while (fgets(line, sizeof line, f) && scanned < KG_FILESCAN && g->ntrip < KG_MAXTRIP) {
        scanned++;
        if (!pcg_read_edge(line, &ri, hs, ts, hl, tv, KG_NAMELEN)) continue;
        int hi = kg_find(g, hs), ti = kg_find(g, ts);
        if (hi < 0 || ti < 0 || hi == ti) continue;
        int k = rel_shift(PCG_RELS[ri]);
        bool dup = false;
        for (int e = 0; e < g->ntrip; e++)
            if (g->edge_h[e] == hi && g->edge_t[e] == ti && g->edge_k[e] == k) { dup = true; break; }
        if (dup) continue;
        g->edge_h[g->ntrip] = hi; g->edge_t[g->ntrip] = ti; g->edge_k[g->ntrip] = k; g->ntrip++;
    }
    fclose(f);
    g->seed_idx = seed_idx;
    return g->ntrip;
}

// Is (h --k--> t) a REAL stored edge of the loaded subgraph? The symbolic half of the soundness gate.
static bool pcg_stored(const kg_t *g, int h, int k, int t) {
    for (int e = 0; e < g->ntrip; e++)
        if (g->edge_h[e] == h && g->edge_k[e] == k && g->edge_t[e] == t) return true;
    return false;
}
// Does entity `idx` have an OUTGOING containment edge? A node with none is a TERMINAL of the
// containment lattice (a continent) — the most-general certain answer to an open "where" question.
static bool pcg_has_containment_out(const kg_t *g, int idx) {
    for (int e = 0; e < g->ntrip; e++)
        if (g->edge_h[e] == idx && pcg_is_containment(g->edge_k[e])) return true;
    return false;
}

// A discovered + grounded derivation chain (<=PCG_MAXDEPTH hops).
typedef struct {
    int   ent;                     // current entity index (the chain's running tail)
    int   len;                     // number of hops
    int   hh[PCG_MAXDEPTH];        // head entity index of each hop
    int   ht[PCG_MAXDEPTH];        // tail entity index of each hop
    int   hk[PCG_MAXDEPTH];        // relation rotation of each hop
    float hc[PCG_MAXDEPTH];        // resonance coherence of each hop
    float min_coh;                 // weakest hop (bounds the confidence — evidential)
} pcg_chain_t;

// AUTONOMOUS multi-hop discovery (port of kge.mjs reach + pcg.mjs walkAndGround, fused). Beam search
// from `start`: at each node try every PCG relation, rotate + cleanup (HDC proposes), and accept the hop
// ONLY if it resonates above the gate AND lands on an entity backed by a REAL stored edge (symbols
// dispose). Two modes:
//   target >= 0 (CLOSED question — why/bridge): the SHORTEST grounded chain that reaches `target`.
//   target <  0 (OPEN question  — where):       the device discovers the conclusion ITSELF — the DEEPEST
//                grounded chain to a TERMINAL of the containment lattice (the most-general certain answer,
//                e.g. the continent). The endpoint is not supplied; ANIMA decides what it can certainly say.
static bool pcg_reach(kg_t *g, int start, int target, float gate, uint32_t *probe, pcg_chain_t *best) {
    pcg_chain_t frontier[PCG_BEAM];
    pcg_chain_t cand[PCG_BEAM * PCG_NREL];
    bool seen[KG_MAXENT];
    for (int i = 0; i < g->n; i++) seen[i] = false;
    seen[start] = true;
    frontier[0].ent = start; frontier[0].len = 0; frontier[0].min_coh = 1e9f;
    int nf = 1;
    bool have_best = false; long best_score = -1;        // open mode: track the deepest terminal chain

    for (int d = 0; d < PCG_MAXDEPTH; d++) {
        int nc = 0;
        for (int fi = 0; fi < nf; fi++) {
            pcg_chain_t *node = &frontier[fi];
            for (int ri = 0; ri < PCG_NREL; ri++) {
                int k = rel_shift(PCG_RELS[ri]);
                hv_rotate(probe, g->cb + (size_t)node->ent * HDC_W, k);
                cleanup_t c = cleanup(probe, g->cb, g->n);
                if (c.idx < 0 || c.idx == node->ent || c.coh < gate) continue;  // HDC: incoherent landing
                if (seen[c.idx]) continue;                                       // no cycles
                if (!pcg_stored(g, node->ent, k, c.idx)) continue;               // symbols disagree -> reject
                if (nc >= PCG_BEAM * PCG_NREL) continue;
                pcg_chain_t *nn = &cand[nc++];
                *nn = *node;
                nn->hh[node->len] = node->ent; nn->ht[node->len] = c.idx;
                nn->hk[node->len] = k;         nn->hc[node->len] = c.coh;
                nn->len = node->len + 1; nn->ent = c.idx;
                nn->min_coh = node->min_coh < c.coh ? node->min_coh : c.coh;
            }
        }
        if (!nc) break;
        // keep the highest-coherence expansions (beam); sort candidates by min_coh desc.
        for (int i = 0; i < nc; i++)
            for (int j = i + 1; j < nc; j++)
                if (cand[j].min_coh > cand[i].min_coh) { pcg_chain_t t = cand[i]; cand[i] = cand[j]; cand[j] = t; }
        nf = nc < PCG_BEAM ? nc : PCG_BEAM;
        for (int i = 0; i < nf; i++) { frontier[i] = cand[i]; seen[cand[i].ent] = true; }
        for (int i = 0; i < nf; i++) {
            if (target >= 0) {
                // CLOSED: first depth that reaches the target wins (shortest, most-direct explanation).
                if (frontier[i].ent == target) { *best = frontier[i]; return true; }
            } else {
                // OPEN: prefer a TERMINAL endpoint, then a deeper chain, then higher coherence.
                bool term = !pcg_has_containment_out(g, frontier[i].ent);
                long score = (term ? 1000000L : 0) + (long)frontier[i].len * 1000 + (long)(frontier[i].min_coh + 0.5f);
                if (!have_best || score > best_score) { *best = frontier[i]; best_score = score; have_best = true; }
            }
        }
    }
    return (target < 0) ? have_best : false;
}

// The closure relation a PATH entails: all-containment -> "located_in"; provenance-then-containment ->
// "from_country"; anything else -> the weakest TRUE reading "connesso" (connected to). Soundness over
// fluency: never assert a specific relation the path can't justify. (0 / 1 / 2 respectively.)
static int pcg_closure(const pcg_chain_t *c) {
    bool all_cont = true;
    for (int i = 0; i < c->len; i++) if (!pcg_is_containment(c->hk[i])) all_cont = false;
    if (all_cont) return 0;
    if (c->hk[0] == rel_shift("country")) {
        bool rest = true;
        for (int i = 1; i < c->len; i++) if (!pcg_is_containment(c->hk[i])) rest = false;
        if (rest) return 1;
    }
    return 2;
}

// The standalone proof checker — the "carrying" has teeth. Re-derives the claim WITHOUT trusting the
// search: every hop a real stored edge, the chain connected end-to-end, its endpoints == head/target,
// and the asserted closure the one the path entails. Port of pcg.mjs verifyProof. False = reject.
static bool pcg_verify(const kg_t *g, const pcg_chain_t *c, int head, int target, int closure) {
    if (c->len < 1) return false;
    for (int i = 0; i < c->len; i++) if (!pcg_stored(g, c->hh[i], c->hk[i], c->ht[i])) return false;
    for (int i = 1; i < c->len; i++) if (c->hh[i] != c->ht[i - 1]) return false;
    if (c->hh[0] != head) return false;
    if (c->ht[c->len - 1] != target) return false;
    if (pcg_closure(c) != closure) return false;
    return true;
}

// ---- surface realization (thin grammar — NOT where the novelty lives) ----
// Human display name: prefer the stored label/value ("Albert Einstein","Europa"); else de-slug + cap.
static void pcg_disp(const kg_t *g, int idx, char *out, size_t cap) {
    if (g->label[idx][0]) { snprintf(out, cap, "%s", g->label[idx]); return; }
    snprintf(out, cap, "%s", g->name[idx]);
    for (char *p = out; *p; p++) if (*p == '-' || *p == '_') *p = ' ';
    comb_cap(out);
}
// One verified edge -> one grounded clause (ASCII "e", matching the device reply convention).
static void pcg_clause(bool en, int k, const char *h, const char *t, char *out, size_t cap) {
    if (k == rel_shift("located_in")) snprintf(out, cap, en ? "%s is located in %s"   : "%s si trova in %s",     h, t);
    else if (k == rel_shift("capital")) snprintf(out, cap, en ? "%s is the capital of %s" : "%s e la capitale di %s", h, t);
    else if (k == rel_shift("country")) snprintf(out, cap, en ? "%s is from %s"         : "%s e di %s",            h, t);
    else snprintf(out, cap, "%s -> %s", h, t);
}
// The deduced, never-stored conclusion the chain entails.
static void pcg_conclude(bool en, int closure, const char *h, const char *t, char *out, size_t cap) {
    if (closure == 0)      snprintf(out, cap, en ? "%s is located in %s"               : "%s si trova in %s",                   h, t);
    else if (closure == 1) snprintf(out, cap, en ? "%s is from a country located in %s": "%s e di un paese che si trova in %s", h, t);
    else                   snprintf(out, cap, en ? "%s is connected to %s"             : "%s e collegato a %s",                 h, t);
}

// ---- tight NL front-end (a guard, not the engine — the cascade already has the real NLU) ----
typedef enum { PCG_NONE, PCG_WHY, PCG_BRIDGE, PCG_WHERE } pcg_mode_t;
typedef struct { pcg_mode_t mode; char a[KG_VALLEN]; char b[KG_VALLEN]; } pcg_q_t;

// Strip leading article/preposition + trailing spaces from an extracted name.
static void pcg_clean(char *s) {
    for (int i = (int)strlen(s) - 1; i >= 0 && s[i] == ' '; i--) s[i] = 0;
    kg_strip_lead(s);
    for (int i = (int)strlen(s) - 1; i >= 0 && s[i] == ' '; i--) s[i] = 0;
}
// After `trigger`, split the tail on the FIRST separator (tried in array order) into a/b. Both sides
// must be >=2 chars. Returns true on a clean split.
static bool pcg_split_after(const char *s, const char *trigger, const char *const *seps,
                            char *a, size_t acap, char *b, size_t bcap) {
    const char *p = strstr(s, trigger);
    if (!p) return false;
    const char *rest = p + strlen(trigger);
    for (int i = 0; seps[i]; i++) {
        const char *m = strstr(rest, seps[i]);
        if (!m) continue;
        size_t alen = (size_t)(m - rest);
        if (alen == 0 || alen + 1 >= acap) continue;
        memcpy(a, rest, alen); a[alen] = 0;
        snprintf(b, bcap, "%s", m + strlen(seps[i]));
        pcg_clean(a); pcg_clean(b);
        if (a[0] && b[0] && strlen(a) >= 2 && strlen(b) >= 2) return true;
    }
    return false;
}
// OPEN form: take everything after `trigger` as the head name (no target), dropping a residual
// connector the trigger may have swallowed ("in che continente E roma", "dove SI TROVA roma").
static bool pcg_take_after(const char *s, const char *trigger, char *a, size_t acap) {
    const char *p = strstr(s, trigger);
    if (!p) return false;
    const char *rest = p + strlen(trigger);
    while (*rest == ' ') rest++;
    snprintf(a, acap, "%s", rest);
    if (!strncmp(a, "e ", 2))        memmove(a, a + 2, strlen(a + 2) + 1);
    if (!strncmp(a, "si trova ", 9)) memmove(a, a + 9, strlen(a + 9) + 1);
    pcg_clean(a);
    return a[0] && strlen(a) >= 2;
}
// Parse the folded query into a WHY ("perche X (e) in Y") or BRIDGE ("come e collegato X a Y") request.
// Returns false for anything else (the cascade then continues). The deductive tier already owns the
// plain "in che continente / dove si trova" forms, so NSPCG's surface is strictly the NEW explanation.
static bool pcg_parse(const char *nf, pcg_q_t *p) {
    memset(p, 0, sizeof *p);
    static const char *const why_sep[]    = { " si trova in ", " e in ", " in ", " a ", NULL };
    static const char *const why_sep_en[] = { " located in ", " in ", NULL };
    static const char *const br_sep[]     = { " allo ", " alla ", " agli ", " all ", " con ", " ad ", " al ", " ai ", " a ", " e ", NULL };
    static const char *const br_sep_en[]  = { " connected to ", " related to ", " linked to ", NULL };
    static const char *const br_sep_en2[] = { " and ", " to ", " with ", NULL };

    static const char *const why_trig[]    = { "perche ", NULL };
    for (int i = 0; why_trig[i]; i++)
        if (pcg_split_after(nf, why_trig[i], why_sep, p->a, sizeof p->a, p->b, sizeof p->b)) { p->mode = PCG_WHY; return true; }
    static const char *const why_trig_en[] = { "why is ", "why was ", "why are ", "why were ", NULL };
    for (int i = 0; why_trig_en[i]; i++)
        if (pcg_split_after(nf, why_trig_en[i], why_sep_en, p->a, sizeof p->a, p->b, sizeof p->b)) { p->mode = PCG_WHY; return true; }

    static const char *const br_trig[] = {
        "come e collegato ", "come e collegata ", "come e connesso ", "come e connessa ",
        "come e legato ", "come e legata ", "come sono collegati ", "come sono collegate ",
        "che lega ", "che collega ", "che unisce ", NULL };
    for (int i = 0; br_trig[i]; i++)
        if (pcg_split_after(nf, br_trig[i], br_sep, p->a, sizeof p->a, p->b, sizeof p->b)) { p->mode = PCG_BRIDGE; return true; }
    static const char *const br_trig_en[] = { "how is ", "how was ", NULL };
    for (int i = 0; br_trig_en[i]; i++)
        if (pcg_split_after(nf, br_trig_en[i], br_sep_en, p->a, sizeof p->a, p->b, sizeof p->b)) { p->mode = PCG_BRIDGE; return true; }
    static const char *const br_trig_en2[] = { "what connects ", "what links ", "how are ", NULL };
    for (int i = 0; br_trig_en2[i]; i++)
        if (pcg_split_after(nf, br_trig_en2[i], br_sep_en2, p->a, sizeof p->a, p->b, sizeof p->b)) { p->mode = PCG_BRIDGE; return true; }

    // WHERE (open: continent-level containment). Deliberately NOT "in che paese/stato" — those want a
    // specific granularity the deductive tier keeps; here the device discovers the most-general container.
    static const char *const where_trig[] = {
        "in che continente ", "in quale continente ", "dove si trova ", "dove si trovano ",
        "what continent ", "where is ", "where are ", NULL };
    for (int i = 0; where_trig[i]; i++)
        if (pcg_take_after(nf, where_trig[i], p->a, sizeof p->a)) { p->mode = PCG_WHERE; return true; }
    return false;
}

// Public entry: GENERATE a proof-carrying explanation, or refuse. Returns true (and fills *out: tier
// FACT, intent "pcg") only when an explanation question resolves to a grounded, self-verified chain;
// false = not an explanation question OR no grounded chain (let the cascade continue to the network).
bool nucleo_anima_pcg_generate(const char *query, const char *lang, anima_result_t *out) {
    if (!query || !out) return false;
    char nf[256]; kg_fold(query, nf, sizeof nf);
    pcg_q_t pq;
    if (!pcg_parse(nf, &pq)) return false;               // not an explanation question -> skip cheaply
    bool en = lang && (lang[0] == 'e' || lang[0] == 'E');

    g_simcnt = malloc((size_t)HDC_D * sizeof(int8_t));
    g_bcnt   = malloc((size_t)HDC_D * sizeof(int8_t));
    uint32_t *probe = malloc((size_t)HDC_W * sizeof(uint32_t));
    kg_t *g = calloc(1, sizeof(kg_t));
    if (!g_simcnt || !g_bcnt || !probe || !g) {
        free(g_simcnt); g_simcnt = NULL; free(g_bcnt); g_bcnt = NULL; free(probe); free(g);
        return false;                                    // OOM -> graceful refuse (no regression)
    }

    bool answered = false;
    bool where = (pq.mode == PCG_WHERE);                 // open question: the device discovers the endpoint
    int nt = pcg_load(g, lang ? lang : "it", pq.a);      // mixed-relation subgraph from the chain's head
    if (nt <= 0) goto pcg_done;

    int head = g->seed_idx;
    if (head < 0) goto pcg_done;
    int target = -1;                                     // -1 = open (where); >=0 = the asked endpoint
    if (!where) {
        char tslug[KG_NAMELEN]; kg_slug(pq.b, tslug, sizeof tslug);
        target = kg_find(g, tslug);
        if (target < 0)                                  // the target may be a partial name of an interned node
            for (int i = 0; i < g->n; i++) if (slug_seg_match(g->name[i], tslug)) { target = i; break; }
        if (target < 0 || head == target) goto pcg_done;
    }
    if (!kg_build(g)) goto pcg_done;                     // build OOM -> refuse
    if (g->n < 2) goto pcg_done;

    pcg_chain_t chain;
    if (!pcg_reach(g, head, target, HDC_REASON_GATE, probe, &chain)) goto pcg_done;  // no grounded chain -> refuse
    int concl_target = where ? chain.ht[chain.len - 1] : target;   // open: the discovered terminal endpoint
    int closure = pcg_closure(&chain);
    if (!pcg_verify(g, &chain, head, concl_target, closure)) goto pcg_done;          // proof must hold -> else refuse

    // VERBALIZE: conclusion + (multi-hop or "connesso" -> ", perche " + one clause per verified hop).
    {
        char hd[64], tl[64], body[640], concl[200];
        pcg_disp(g, head, hd, sizeof hd);
        pcg_disp(g, concl_target, tl, sizeof tl);
        pcg_conclude(en, closure, hd, tl, concl, sizeof concl);
        snprintf(body, sizeof body, "%s", concl);
        if (chain.len > 1 || closure == 2) {
            strncat(body, en ? ", because " : ", perche ", sizeof body - strlen(body) - 1);
            for (int i = 0; i < chain.len; i++) {
                char hh[64], tt[64], cl[200];
                pcg_disp(g, chain.hh[i], hh, sizeof hh);
                pcg_disp(g, chain.ht[i], tt, sizeof tt);
                pcg_clause(en, chain.hk[i], hh, tt, cl, sizeof cl);
                if (i > 0) strncat(body, en ? " and " : " e ", sizeof body - strlen(body) - 1);
                strncat(body, cl, sizeof body - strlen(body) - 1);
            }
        }
        strncat(body, ".", sizeof body - strlen(body) - 1);

        memset(out, 0, sizeof *out);
        out->tier = ANIMA_TIER_FACT; out->action = ANIMA_ACT_ANSWER;
        snprintf(out->intent, sizeof out->intent, "pcg");
        snprintf(out->state,  sizeof out->state,  "idle");
        snprintf(out->reply,  sizeof out->reply,  "%s", body);
        int conf = 78 + (int)(chain.min_coh + 0.5f);
        out->confidence = conf > 95 ? 95 : (conf < 70 ? 70 : conf);
        snprintf(out->subject,  sizeof out->subject,  "%s", hd);
        snprintf(out->relation, sizeof out->relation, "%s",
                 closure == 0 ? "located_in" : (closure == 1 ? "country" : "connesso"));
        answered = true;
        ESP_LOGI(TAG, "pcg: %s -> %s (%d hops, min_coh %.1f): %s",
                 g->name[head], g->name[concl_target], chain.len, chain.min_coh, body);
    }

pcg_done:
    kg_free(g);
    free(g); free(probe);
    free(g_simcnt); g_simcnt = NULL;
    free(g_bcnt);   g_bcnt   = NULL;
    return answered;
}

// Detect-only companion (parse, no KG build): is `query` an NSPCG-shaped question (where / why / bridge)?
// Lets the orchestrator reclaim L1's heap for the KG build ONLY when NSPCG will actually try — a normal
// fact-question keeps its L1 index. Mirrors nucleo_anima_hdc_detect's cheap-probe role. (lang unused;
// the patterns are bilingual and language-agnostic at this stage.)
bool nucleo_anima_pcg_detect(const char *query, const char *lang) {
    (void)lang;
    if (!query) return false;
    char nf[256]; kg_fold(query, nf, sizeof nf);
    pcg_q_t pq;
    return pcg_parse(nf, &pq);
}
