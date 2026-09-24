// ANIMA learned-card recall — NETWORK-FREE, so the device, the host harness and the browser WASM all
// compile the SAME code (it used to live in nucleo_anima_online.c, which only the device builds; the
// host/WASM linked a stub that always missed, so the browser never used what the device had learned).
//
// Store (written by the online tier): learned/<lang>.jsonl — one card per line, printed by cJSON —
// and learned/<lang>.vec, the paraphrase sidecar (u8 idlen | id | u16-LE dim | int8 vec[dim]).
// Parsing here is a tiny JSON-string reader, not cJSON: the host/WASM builds have no cJSON, and a
// line printed by cJSON has a known shape (keys in insertion order, standard escapes, raw UTF-8).
#include "nucleo_anima.h"
#include "nucleo_anima_online.h"   // nucleo_anima_online_recall (public name kept for the cascade)
#include "anima_internal.h"        // a_damlev
#include "anima_l1.h"              // nucleo_anima_l1_encode / _dim
#include "nucleo_board.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

extern uint32_t g_anima_stage;     // DIAG breadcrumb (defined in nucleo_anima.c)

#define LEARN_DIR      NUCLEO_SD_MOUNT "/data/anima/learned"
#define RECALL_DIM     256          // max encoder dim we size buffers for (L1_MAXDIM)
// Three bands, measured on the host encoder (tools/anima-host/recall-check.mjs): a query that NAMES the
// card's whole title sits at ~0.70-0.74 ("dimmi cos'è la zorblax industries"), an unrelated one at
// <=0.51 ("il lago di como" vs "Lago Quarzino"). The vector alone is trusted only at STRONG.
#define RECALL_NAMED   0.65f        // query names EVERY distinctive word of the card title
#define RECALL_THRESH  0.75f        // query names at least one title word
#define RECALL_STRONG  0.90f        // vector alone (a near-verbatim paraphrase of the card)

// One scan-line buffer for the whole learned store, shared with the online tier (was its private
// .bss): defining it here keeps it a single 1.5 KB block on the device.
char g_anima_scan_line[1536];

static void learned_path(char *dst, int cap, bool en, const char *ext)
{
    snprintf(dst, cap, LEARN_DIR "/%s.%s", en ? "en" : "it", ext);
}

// Decode the JSON string starting at `p` (which must point at its opening quote) into out[cap].
// Returns the position just past the closing quote, or NULL. Standard escapes; \uXXXX -> UTF-8 (BMP).
static const char *json_str(const char *p, char *out, int cap)
{
    if (!p || *p != '"') return NULL;
    int o = 0;
    for (p++; *p && *p != '"'; p++) {
        unsigned c = (unsigned char)*p;
        if (c == '\\') {
            char e = *++p;
            if (!e) return NULL;
            if (e == 'u') {
                unsigned v = 0;
                for (int i = 0; i < 4; i++) {
                    char h = p[1 + i];
                    v = v * 16 + (unsigned)(isdigit((unsigned char)h) ? h - '0' : (tolower((unsigned char)h) - 'a' + 10));
                    if (!isxdigit((unsigned char)h)) return NULL;
                }
                p += 4;
                if (v < 0x80) { if (o < cap - 1) out[o++] = (char)v; }
                else if (v < 0x800) { if (o < cap - 2) { out[o++] = (char)(0xC0 | (v >> 6)); out[o++] = (char)(0x80 | (v & 0x3F)); } }
                else if (o < cap - 3) { out[o++] = (char)(0xE0 | (v >> 12)); out[o++] = (char)(0x80 | ((v >> 6) & 0x3F)); out[o++] = (char)(0x80 | (v & 0x3F)); }
                continue;
            }
            c = (unsigned char)(e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b' : e == 'f' ? '\f' : e);
        }
        if (o < cap - 1) out[o++] = (char)c;
    }
    if (*p != '"') return NULL;
    out[o] = 0;
    return p + 1;
}

// Inside the object that follows `"obj":{` in `line`, the value of key `lang` as a string.
static bool json_obj_lang_str(const char *line, const char *obj, const char *lang, char *out, int cap)
{
    char k[24]; snprintf(k, sizeof k, "\"%s\":{", obj);
    const char *o = strstr(line, k);
    if (!o) return false;
    char kl[12]; snprintf(kl, sizeof kl, "\"%s\":\"", lang);
    const char *v = strstr(o, kl);
    if (!v) return false;
    const char *end = strchr(o, '}');                   // the string must belong to THIS object
    if (end && v > end) return false;
    return json_str(v + strlen(kl) - 1, out, cap) != NULL;
}

// Read the learned card `id`: its reply (in `en`, else the other language) into *out and, when
// asks/acap are given, its ask phrasings for that language joined by '\n'. 1 = found.
static int learned_read(bool en, const char *id, anima_result_t *out, char *asks, int acap)
{
    char path[160]; learned_path(path, sizeof path, en, "jsonl");
    char idq[84]; snprintf(idq, sizeof idq, "\"id\":\"%s\"", id);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int found = 0;
    while (fgets(g_anima_scan_line, sizeof g_anima_scan_line, f)) {
        if (!strstr(g_anima_scan_line, idq)) continue;
        char txt[sizeof out->reply];
        const char *l1 = en ? "en" : "it", *l2 = en ? "it" : "en";
        if (!json_obj_lang_str(g_anima_scan_line, "reply", l1, txt, sizeof txt) &&
            !json_obj_lang_str(g_anima_scan_line, "reply", l2, txt, sizeof txt)) break;
        if (!txt[0]) break;
        memset(out, 0, sizeof *out);
        out->tier = ANIMA_TIER_FACT; out->action = ANIMA_ACT_ANSWER;
        snprintf(out->reply, sizeof out->reply, "%s", txt);
        if (asks && acap > 0) {
            asks[0] = 0;
            char k[16]; snprintf(k, sizeof k, "\"%s\":[", l1);
            const char *a = strstr(g_anima_scan_line, "\"ask\":{");
            const char *arr = a ? strstr(a, k) : NULL;
            if (!arr) { snprintf(k, sizeof k, "\"%s\":[", l2); arr = a ? strstr(a, k) : NULL; }
            if (arr) {
                const char *p = arr + strlen(k);
                int o = 0;
                while (*p == '"') {
                    char s[96];
                    p = json_str(p, s, sizeof s);
                    if (!p) break;
                    o += snprintf(asks + o, (size_t)(acap - o > 0 ? acap - o : 0), "%s%s", o ? "\n" : "", s);
                    if (o >= acap - 1) break;
                    if (*p == ',') p++;
                }
            }
        }
        found = 1;
        break;
    }
    fclose(f);
    return found;
}

// Does the query name the card? Words of the card's TITLE (its first ask phrasing) are looked up in
// the query, typo-tolerant — and when the title has distinctive words (>=5 letters) only those count,
// so a generic "lago" can't recall "Lago Quarzino" for "il lago di garda". The vector alone at 0.75 is
// hashing-coincidence territory for a char-n-gram encoder; the anchor keeps recall to the thing asked.
// Returns 0 = no title word named, 1 = some, 2 = every counted title word named.
static int recall_lex_anchor(const char *query, const char *asks)
{
    char title[96]; int t = 0;
    while (asks[t] && asks[t] != '\n' && t < (int)sizeof title - 1) { title[t] = asks[t]; t++; }
    title[t] = 0;
    int minlen = 3;
    for (const unsigned char *p = (const unsigned char *)title; *p; ) {        // any distinctive word?
        int k = 0; while (*p && (isalnum(*p) || *p >= 0x80)) { k++; p++; }
        if (k >= 5) { minlen = 5; break; }
        while (*p && !isalnum(*p) && *p < 0x80) p++;
    }
    asks = title;
    static const char *const stop[] = { "chi","che","cos","cosa","come","quale","quali","quando","dove","perche",
        "il","lo","la","le","gli","un","uno","una","di","del","della","dei","delle","da","in","con","su","per",
        "what","who","which","when","where","why","how","the","is","are","was","of","and","for", NULL };
    char qw[32][24]; int nq = 0;
    for (const unsigned char *p = (const unsigned char *)query; *p && nq < 32; ) {
        while (*p && !isalnum(*p) && *p < 0x80) p++;
        int k = 0;
        while (*p && (isalnum(*p) || *p >= 0x80)) { if (k < 23) qw[nq][k++] = (char)tolower(*p); p++; }
        if (k) { qw[nq][k] = 0; nq++; }
    }
    int counted = 0, named = 0;
    for (const unsigned char *p = (const unsigned char *)asks; *p; ) {
        while (*p && !isalnum(*p) && *p < 0x80) p++;
        char w[24]; int k = 0;
        while (*p && (isalnum(*p) || *p >= 0x80)) { if (k < 23) w[k++] = (char)tolower(*p); p++; }
        w[k] = 0;
        if (k < minlen) continue;
        bool st = false; for (int s = 0; stop[s]; s++) if (!strcmp(w, stop[s])) { st = true; break; }
        if (st) continue;
        counted++;
        for (int j = 0; j < nq; j++) if (strlen(qw[j]) >= 3 && a_damlev(w, qw[j], 1) <= 1) { named++; break; }
    }
    return !named ? 0 : named == counted ? 2 : 1;
}

// Semantic recall of a learned card: nearest learned vector to the query, gated by the three bands
// above — the lower the cosine, the more of the card's title the query must name.
int nucleo_anima_online_recall(const char *query, bool en, anima_result_t *out)
{
    g_anima_stage = 0xD0;                          // DIAG: entered online/learned recall
    int D = nucleo_anima_l1_dim();
    if (D <= 0 || D > RECALL_DIM || !query) return 0;       // encoder not loaded -> recall off

    // Open the sidecar FIRST: with nothing learned yet, skip the (costly) query encode entirely.
    char vp[170]; learned_path(vp, sizeof vp, en, "vec");
    FILE *in = fopen(vp, "rb");
    if (!in) return 0;

    static int8_t qv[RECALL_DIM];
    if (nucleo_anima_l1_encode(query, qv, RECALL_DIM) != D) { fclose(in); return 0; }
    // int8 vectors: each squared term <=127^2 and D<=256, so the norm sums are exact in int32.
    int32_t qn2 = 0; for (int k = 0; k < D; k++) qn2 += (int32_t)qv[k] * qv[k];
    float qn = sqrtf((float)qn2); if (qn < 1e-6f) { fclose(in); return 0; }
    char bestid[80] = ""; float best = -2.0f;
    static char rid[80]; static int8_t rv[RECALL_DIM]; uint8_t l, db[2];
    while (fread(&l, 1, 1, in) == 1) {
        if (l == 0 || l >= sizeof(rid) || fread(rid, 1, l, in) != l || fread(db, 1, 2, in) != 2) break;
        int d = db[0] | (db[1] << 8);                        // u16-LE dim
        if (d != D) { if (d <= 0 || d > RECALL_DIM || fseek(in, d, SEEK_CUR) != 0) break; continue; }
        if (fread(rv, 1, d, in) != (size_t)d) break;
        long dot = 0; int32_t vn = 0;
        for (int k = 0; k < D; k++) { dot += (int)qv[k] * rv[k]; vn += (int32_t)rv[k] * rv[k]; }
        float cos = (float)dot / (qn * sqrtf((float)vn) + 1e-9f);
        if (cos > best) { best = cos; rid[l] = 0; snprintf(bestid, sizeof(bestid), "%s", rid); }
    }
    fclose(in);
#ifdef ANIMA_HOST
    if (getenv("RECALL_TRACE")) fprintf(stderr, "[recall] best=%.3f id=%s\n", best, bestid);   // host-only tuning aid
#endif
    if (best < RECALL_NAMED || !bestid[0]) return 0;        // not confidently the same thing
    char asks[512];
    if (!learned_read(en, bestid, out, asks, sizeof asks)) return 0;   // card may have been evicted since
    if (best < RECALL_STRONG) {
        int named = recall_lex_anchor(query, asks);
        if (named == 0 || (best < RECALL_THRESH && named < 2)) { memset(out, 0, sizeof *out); return 0; }
    }
    out->confidence = (int)(best * 100.0f + 0.5f);
    snprintf(out->intent, sizeof(out->intent), "recall");
    return 1;
}
