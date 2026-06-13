// ANIMA typed-facet tier — answers CATEGORICAL facet questions about a known entity by EXACT lookup in
// data/anima/learned/facets.<lang>.jsonl: "che lavoro faceva X / di cosa si occupava X" (occupation) and
// "X è uomo o donna / di che sesso è X" (gender). Source-anchored, allocation-free, network-free,
// host-runnable. ABSTAINS on a miss (unknown entity, or a place that has no such facet) so it can never
// fabricate — the 0-hallucination contract. Categorical facets live here (NOT in the holographic KGE,
// whose many-to-one fan-in won't cleanup); `died` stays in the KGE (relational). See
// docs/anima-knowledge-graph.md. Mirrors the slug / JSON-scan primitives of nucleo_anima_hdc.c (kept
// local for zero cross-file coupling, matching the codebase style).
#include "nucleo_anima.h"
#include "nucleo_board.h"      // NUCLEO_SD_MOUNT
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#ifndef ANIMA_HOST
#include "esp_log.h"
#include "mbedtls/sha256.h"
#include "nucleo_knowledge_manifest.h"   // VKL_FACETS_{IT,EN}_SHA256 — the firmware-embedded root of trust
#endif

#define FF_LO   320            // working folded-lowercase buffer
#define FF_SLUG 64
#define FF_VAL  96

// Lowercase + FOLD Italian accents to ASCII (è->e, à->a, ...) into `out`, leaving spaces/punct/digits.
// Folding (not byte-preserving like profile.prep_lo) lets the lead/tail tables stay pure ASCII so an
// accented query ("di che sesso è X") matches without a combinatorial set of accented variants. We never
// need the original case: the reply uses the file's `label`, the subject is matched by slug.
static void prep_fold(const char *raw, char *out, size_t cap) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)raw; *p && o + 1 < cap; p++) {
        unsigned char c = *p; char ch;
        if (c == 0xC3 && p[1]) {                 // 2-byte UTF-8 Latin-1 supplement -> base letter
            unsigned char d = *++p;
            if      (d >= 0xA0 && d <= 0xA2) ch = 'a';
            else if (d >= 0xA8 && d <= 0xAA) ch = 'e';
            else if (d >= 0xAC && d <= 0xAE) ch = 'i';
            else if (d >= 0xB2 && d <= 0xB4) ch = 'o';
            else if (d >= 0xB9 && d <= 0xBB) ch = 'u';
            else ch = ' ';
        } else if (c < 0x80) {
            ch = (char)tolower(c);
        } else {
            ch = ' ';                            // any other multibyte -> separator
        }
        out[o++] = ch;
    }
    out[o] = 0;
}

// slugify (mirror nucleo_anima_hdc.c kg_slug): lowercase ASCII, non-alnum -> '-', collapse, trim.
static void ff_slug(const char *in, char *out, size_t cap) {
    size_t o = 0; int prev_dash = 1;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 1 < cap; p++) {
        unsigned char c = *p; char ch = 0;
        if (isalnum(c)) ch = (char)tolower(c);
        if (ch) { out[o++] = ch; prev_dash = 0; }
        else if (!prev_dash) { out[o++] = '-'; prev_dash = 1; }
    }
    while (o > 0 && out[o - 1] == '-') o--;
    out[o] = 0;
}

// minimal JSON string-field extractor (mirror kg_json_str): "\"key\":\"value\"".
static bool ff_json(const char *line, const char *key, char *out, size_t cap) {
    char pat[24]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(line, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < cap) { if (*p == '\\' && p[1]) p++; out[o++] = *p++; }
    out[o] = 0;
    return true;
}

// `needle` is a full hyphen-delimited SEGMENT of `hay` (or equal) — so "einstein"~"albert-einstein" but
// not a mid-word substring (mirror slug_has_segment).
static bool ff_seg(const char *hay, const char *needle) {
    size_t nl = strlen(needle); if (!nl) return false;
    for (const char *p = strstr(hay, needle); p; p = strstr(p + 1, needle)) {
        bool lb = (p == hay) || (p[-1] == '-');
        bool rb = (p[nl] == 0) || (p[nl] == '-');
        if (lb && rb) return true;
    }
    return false;
}
static bool ff_seg_match(const char *a, const char *b) {
    return !strcmp(a, b) || ff_seg(a, b) || ff_seg(b, a);
}

// strstr at a left word boundary (start/space/punct before) — avoids matching a lead inside a word.
static const char *ff_wb(const char *lo, const char *pat) {
    for (const char *p = lo; (p = strstr(p, pat)); p++) {
        if (p == lo) return p;
        unsigned char b = (unsigned char)p[-1];
        if (b == ' ' || b == ',' || b == '.' || b == ';' || b == ':' || b == '\'' || b == '"') return p;
    }
    return NULL;
}

// Scan facets.<lang>.jsonl for an entity whose subject-slug segment-matches `qslug` carrying relation
// `rel`; fill value + label. Exact subject wins, else the SHORTEST containing-segment slug (mirrors the
// KGE seed rule). One file pass, stack buffers only. Returns false (abstain) on no match.
static bool facet_lookup(const char *qslug, const char *rel, const char *lang,
                         char *val, size_t vcap, char *label, size_t lcap) {
    if (!qslug[0] || strlen(qslug) < 3) return false;
    char path[160];
    snprintf(path, sizeof path, NUCLEO_SD_MOUNT "/data/anima/learned/facets.%s.jsonl",
             (lang && lang[0]) ? lang : "it");
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char line[512], subj[FF_SLUG], rl[24], v[FF_VAL], lb[FF_VAL];
    char best_v[FF_VAL] = "", best_l[FF_VAL] = ""; int best_len = 1 << 30; bool exact = false;
    while (fgets(line, sizeof line, f)) {
        if (!ff_json(line, "rel", rl, sizeof rl) || strcmp(rl, rel) != 0) continue;
        if (!ff_json(line, "subject", subj, sizeof subj)) continue;
        if (!ff_seg_match(subj, qslug)) continue;
        bool cexact = !strcmp(subj, qslug);
        int clen = (int)strlen(subj);
        if (exact && !cexact) continue;
        if (cexact || clen < best_len) {
            if (!ff_json(line, "value", v, sizeof v)) continue;
            if (!ff_json(line, "label", lb, sizeof lb)) lb[0] = 0;
            exact = cexact; best_len = clen;
            snprintf(best_v, sizeof best_v, "%s", v);
            snprintf(best_l, sizeof best_l, "%s", lb[0] ? lb : subj);
        }
    }
    fclose(f);
    if (!best_v[0]) return false;
    snprintf(val, vcap, "%s", best_v);
    snprintf(label, lcap, "%s", best_l);
    return true;
}

// Trim leading/trailing spaces; strip trailing connector words (e/era/was/is/fu) and a leading "is ".
static void clean_name(char *s) {
    char *p = s; while (*p == ' ') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    if (!strncmp(s, "is ", 3)) memmove(s, s + 3, strlen(s + 3) + 1);
    // strip trailing connectors AND the EN location prepositions left by "what continent is X IN / located /
    // on / from" — without this, "einstein in" doesn't segment-match "albert-einstein" and the gate misses.
    for (int pass = 0; pass < 3; pass++) {
        size_t n = strlen(s);
        while (n && (s[n-1] == ' ' || s[n-1] == '?' || s[n-1] == '.' || s[n-1] == '!' || s[n-1] == ',')) s[--n] = 0;
        const char *last = s; for (const char *q = s; *q; q++) if (*q == ' ') last = q + 1;
        static const char *const conn[] = { "e", "era", "was", "is", "fu", "in", "on", "from", "located", "at", "again", NULL };
        bool cut = false;
        for (int i = 0; conn[i]; i++)
            if (!strcmp(last, conn[i])) { size_t off = (size_t)(last - s); if (off) { s[off-1] = 0; cut = true; } break; }
        if (!cut) break;
    }
}

static void fill(anima_result_t *r, const char *rel, const char *label) {
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 90;
    snprintf(r->intent, sizeof r->intent, "facet");
    snprintf(r->state, sizeof r->state, "tool");
    snprintf(r->subject, sizeof r->subject, "%s", label);
    snprintf(r->relation, sizeof r->relation, "%s", rel);
}

// "a"/"an" by first letter (EN occupation article).
static const char *art_en(const char *w) {
    char c = (char)tolower((unsigned char)w[0]);
    return (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u') ? "an" : "a";
}

// lead tables: the entity name follows the lead phrase.
static const char *const OCC_LEADS[] = {
    "di cosa si occupava ", "di cosa si occupa ", "che lavoro faceva ", "che lavoro fa ",
    "che mestiere faceva ", "che mestiere fa ", "che professione aveva ", "che lavoro svolgeva ",
    "qual era il lavoro di ", "qual e il lavoro di ", "qual era la professione di ", "qual era il mestiere di ",
    "what was the occupation of ", "what was the profession of ", "what was the job of ",
    "what was the role of ", "occupation of ", "profession of ",
    NULL,
};
static const char *const GEN_LEADS[] = {
    "di che sesso e ", "di che sesso era ", "che sesso ha ", "che sesso aveva ",
    "qual e il sesso di ", "qual e il genere di ", "che genere e ",
    "what gender is ", "what gender was ", "what sex is ", "what sex was ",
    "what is the gender of ", "what is the sex of ",
    NULL,
};
// tail markers for gender: the name PRECEDES a man-or-woman disjunction.
static const char *const GEN_TAILS[] = {
    " uomo o donna", " donna o uomo", " maschio o femmina", " femmina o maschio",
    " a man or a woman", " a woman or a man", " male or female", " female or male",
    NULL,
};

static bool answer_occupation(const char *nf, bool en, anima_result_t *r) {
    for (int i = 0; OCC_LEADS[i]; i++) {
        const char *p = ff_wb(nf, OCC_LEADS[i]);
        if (!p) continue;
        char name[FF_LO]; snprintf(name, sizeof name, "%s", p + strlen(OCC_LEADS[i]));
        clean_name(name);
        char qslug[FF_SLUG]; ff_slug(name, qslug, sizeof qslug);
        char val[FF_VAL], label[FF_VAL];
        if (!facet_lookup(qslug, "occupation", en ? "en" : "it", val, sizeof val, label, sizeof label)) continue;
        fill(r, "occupation", label);
        if (en) snprintf(r->reply, sizeof r->reply, "%s was %s %s.", label, art_en(val), val);
        else    snprintf(r->reply, sizeof r->reply, "%s era %s.", label, val);
        return true;
    }
    return false;
}

static bool answer_gender(const char *nf, bool en, anima_result_t *r) {
    char name[FF_LO] = "";
    for (int i = 0; GEN_LEADS[i] && !name[0]; i++) {
        const char *p = ff_wb(nf, GEN_LEADS[i]);
        if (p) { snprintf(name, sizeof name, "%s", p + strlen(GEN_LEADS[i])); }
    }
    if (!name[0]) {
        for (int i = 0; GEN_TAILS[i] && !name[0]; i++) {
            const char *m = strstr(nf, GEN_TAILS[i]);
            if (m) { size_t plen = (size_t)(m - nf); if (plen && plen < sizeof name) { memcpy(name, nf, plen); name[plen] = 0; } }
        }
    }
    if (!name[0]) return false;
    clean_name(name);
    char qslug[FF_SLUG]; ff_slug(name, qslug, sizeof qslug);
    char val[FF_VAL], label[FF_VAL];
    if (!facet_lookup(qslug, "gender", en ? "en" : "it", val, sizeof val, label, sizeof label)) return false;
    bool female = strstr(val, "donna") || strstr(val, "woman");
    fill(r, "gender", label);
    if (en) snprintf(r->reply, sizeof r->reply, "%s is a %s.", label, female ? "woman" : "man");
    else    snprintf(r->reply, sizeof r->reply, female ? "%s è una donna." : "%s è un uomo.", label);
    return true;
}

// ---- TYPE-GATE: a place-relation question asked about a PERSON ----------------------------------
// Classify a subject slug by its isa facet: 2 = a place is present (let the geo/KGE tier answer),
// 1 = a person and NOT a place (a category error for a place-question), 0 = unknown. Refusing only on
// kind==1 means we NEVER block a legitimate geo question, and an ambiguous name (both a person and a
// place, e.g. a capital that shares a surname) defers to the place reading — safe by construction.
// Scan one facets file, updating the place/person flags for `qslug`.
//   PLACE match must be EXACT (subj == qslug): a place is named in full ("francia", "parigi"), so a
//     coincidental sub-segment ("colombo" inside "cristoforo-colombo", "washington" inside
//     "denzel-washington") must NOT count as "this subject is a place" — that bug let the bio leak.
//   PERSON match stays SEGMENT-based so a surname binds ("einstein" -> "albert-einstein").
static void isa_scan(const char *path, const char *qslug, bool *has_place, bool *has_person,
                     char *pl, size_t plcap, int *pbest) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[512], subj[FF_SLUG], rl[24], v[FF_VAL], lb[FF_VAL];
    while (fgets(line, sizeof line, f)) {
        if (!ff_json(line, "rel", rl, sizeof rl) || strcmp(rl, "isa") != 0) continue;
        if (!ff_json(line, "subject", subj, sizeof subj)) continue;
        if (!ff_json(line, "value", v, sizeof v)) continue;
        if ((strstr(v, "luogo") || strstr(v, "place")) && !strcmp(subj, qslug)) { *has_place = true; }
        else if ((strstr(v, "persona") || strstr(v, "person")) && ff_seg_match(subj, qslug)) {
            *has_person = true;
            bool ex = !strcmp(subj, qslug); int cl = (int)strlen(subj);
            if (ex || cl < *pbest) { *pbest = ex ? 0 : cl;
                if (ff_json(line, "label", lb, sizeof lb) && lb[0]) snprintf(pl, plcap, "%s", lb);
                else snprintf(pl, plcap, "%s", subj); }
        }
    }
    fclose(f);
}

// Classify a subject slug: 2 = a place (EXACT in EITHER language file -> let the geo tier answer),
// 1 = a person and not a place (a category error for a place-question -> honest refusal), 0 = unknown.
// Checking BOTH languages closes the cross-corpus gap (an EN place like "jordan" has no IT row — its
// slug is "giordania" — so an Italian session would otherwise refuse it as the person "jordan-love").
static int isa_kind(const char *qslug, const char *lang, char *plabel, size_t lcap) {
    (void)lang;
    if (!qslug[0] || strlen(qslug) < 3) return 0;
    bool has_place = false, has_person = false; char pl[FF_VAL] = ""; int pbest = 1 << 30;
    isa_scan(NUCLEO_SD_MOUNT "/data/anima/learned/facets.it.jsonl", qslug, &has_place, &has_person, pl, sizeof pl, &pbest);
    isa_scan(NUCLEO_SD_MOUNT "/data/anima/learned/facets.en.jsonl", qslug, &has_place, &has_person, pl, sizeof pl, &pbest);
    if (has_place) return 2;
    if (has_person) { snprintf(plabel, lcap, "%s", pl); return 1; }
    return 0;
}

// strip a leading connector/article the place lead left behind ("...continente è X" -> folded "e x").
static void strip_lead_words(char *s) {
    // connectors/fillers/articles a place lead can leave before the entity ("...continente fa parte X",
    // "...continente si trova la X", "...città di X"). Folded text, so "è" is already "e".
    static const char *const lead[] = {
        "e ", "si trova ", "si trovava ", "fa parte ", "fa parte del ", "fa parte della ", "fa parte dei ",
        "the ", "the city of ", "city of ", "citta di ", "citta ", "stato di ", "stato ", "nazione di ",
        "nazione ", "paese di ", "paese ", "regione di ", "regione ", "il ", "lo ", "la ", "l ", "i ", "gli ",
        "del ", "dello ", "della ", "dei ", "di ", "in ", "su ", "sul ", "sulla ", "sullo ", "sui ", "esattamente ", NULL };
    for (bool again = true; again; ) {
        again = false;
        while (*s == ' ') memmove(s, s + 1, strlen(s));
        for (int i = 0; lead[i]; i++) {
            size_t L = strlen(lead[i]);
            if (!strncmp(s, lead[i], L)) { memmove(s, s + L, strlen(s + L) + 1); again = true; break; }
        }
    }
}

// place-relation leads: the entity name follows. Mirror the KGE inverse/transitive triggers.
static const char *const PLACE_LEADS[] = {
    "in che continente ", "in quale continente ", "su quale continente ", "su che continente ",
    "di che continente ", "che continente e ", "capitale di ", "capitale del ", "capitale dello ",
    "capitale della ", "capitale dell ", "in che paese ", "in quale paese ", "in che nazione ",
    "in quale nazione ", "in che stato ", "in quale stato ", "in che citta ", "in quale citta ",
    "in che regione ", "in quale regione ", "dove si trova ", "dove si trovava ", "dove e ", "dov'e ",
    "di dove e ", "di dov'e ",
    "what continent is ", "which continent is ", "what is the capital of ", "what is the capital city of ",
    "capital city of ", "capital of ", "where is ", "what country is ", "in which country is ",
    "in what country is ", "what state is ",
    NULL,
};

// Honest type refusal: "in che continente è Einstein?" -> Einstein is a person, not a place. Source-
// anchored on the isa facet; stops the cascade so no L1 bio dodges the category error. Returns false
// (and the cascade continues into the geo/KGE tier) when the subject is a place or unknown.
static bool answer_typegate(const char *nf, bool en, anima_result_t *r) {
    for (int i = 0; PLACE_LEADS[i]; i++) {
        const char *p = ff_wb(nf, PLACE_LEADS[i]);
        if (!p) continue;
        char name[FF_LO]; snprintf(name, sizeof name, "%s", p + strlen(PLACE_LEADS[i]));
        strip_lead_words(name); clean_name(name);
        char qslug[FF_SLUG]; ff_slug(name, qslug, sizeof qslug);
        char plabel[FF_VAL];
        if (isa_kind(qslug, en ? "en" : "it", plabel, sizeof plabel) == 1) {
            fill(r, "isa", plabel);
            if (en) snprintf(r->reply, sizeof r->reply, "%s is a person, not a place.", plabel);
            else    snprintf(r->reply, sizeof r->reply, "%s è una persona, non un luogo.", plabel);
            return true;
        }
        // a place or unknown subject for THIS lead -> keep trying other leads (no early bail), then defer.
    }
    return false;
}

// ON-DEVICE INTEGRITY (the VKL root of trust): before trusting facets.<lang>.jsonl on the removable SD,
// verify its SHA-256 against the value embedded in the firmware (.bin, off-SD, reversible — not eFuse).
// A tampered/missing knowledge file -> this tier ABSTAINS (fail-safe), so the device never answers from
// knowledge it can't authenticate. Lazy + cached (the hash is computed once per language). On the host
// harness (logic-only gates, no mbedtls) it's a no-op so the gate stays green.
static bool facets_trusted(bool en) {
#ifdef ANIMA_HOST
    (void)en; return true;
#else
    static int cache[2] = { -1, -1 };
    int idx = en ? 1 : 0;
    if (cache[idx] >= 0) return cache[idx] != 0;
    char path[160];
    snprintf(path, sizeof path, NUCLEO_SD_MOUNT "/data/anima/learned/facets.%s.jsonl", en ? "en" : "it");
    const char *want = en ? VKL_FACETS_EN_SHA256 : VKL_FACETS_IT_SHA256;
    FILE *f = fopen(path, "rb");
    if (!f) { cache[idx] = 0; return false; }
    mbedtls_sha256_context c; mbedtls_sha256_init(&c); mbedtls_sha256_starts(&c, 0);
    unsigned char buf[512]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) mbedtls_sha256_update(&c, buf, n);
    fclose(f);
    unsigned char dig[32]; mbedtls_sha256_finish(&c, dig); mbedtls_sha256_free(&c);
    char hex[65]; for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", dig[i]);
    cache[idx] = (strcmp(hex, want) == 0) ? 1 : 0;
    if (!cache[idx]) ESP_LOGW("anima.vkl", "facets.%s.jsonl integrity FAILED (tampered SD?) -> abstaining", en ? "en" : "it");
    return cache[idx] != 0;
#endif
}

// Public entry: try to answer a typed-facet question. Returns 1 (and fills *r) on a grounded hit;
// 0 = not a facet question OR the entity isn't known for that facet (abstain -> cascade continues).
int nucleo_anima_facet(const char *raw, bool en, anima_result_t *r) {
    if (!raw || !*raw) return 0;
    if (!facets_trusted(en)) return 0;          // unauthenticated SD knowledge -> abstain (fail-safe)
    char nf[FF_LO]; prep_fold(raw, nf, sizeof nf);
    if (answer_occupation(nf, en, r)) return 1;
    if (answer_gender(nf, en, r)) return 1;
    if (answer_typegate(nf, en, r)) return 1;
    return 0;
}
