// ANIMA offline translation tier — see nucleo_anima_translate.h.
// Network-free (libc only), so it compiles and runs in the host harness like the profile/learn tiers.
// Self-contained: it re-tokenizes the RAW input with a byte-for-byte port of a_tokenize() (the firmware
// normalizer is static in nucleo_anima.c), so the query phrase produces the same keys the generator wrote.
#include "nucleo_anima_translate.h"
#include "nucleo_board.h"      // NUCLEO_SD_MOUNT
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdbool.h>

#define T_MAX_TOKENS 24       // mirror firmware a_tokenize()
#define T_TOK_LEN    24       // mirror firmware (cap = 23 usable chars)

#define DICT_IT_EN  NUCLEO_SD_MOUNT "/data/anima/dict-it-en.tsv"   // IT key -> EN translations
#define DICT_EN_IT  NUCLEO_SD_MOUNT "/data/anima/dict-en-it.tsv"   // EN key -> IT translations

// Fold an Italian accented vowel (the byte AFTER 0xC3) to bare ASCII; 0 if not one we fold.
// EXACTLY the switch in a_tokenize() (NOT the wider learn.c map) — the keys were generated with this set.
static char t_fold(unsigned char d)
{
    switch (d) {
        case 0xA0: case 0xA1: case 0xA2: return 'a';   // à á â
        case 0xA8: case 0xA9: case 0xAA: return 'e';   // è é ê
        case 0xAC: case 0xAD: case 0xAE: return 'i';   // ì í î
        case 0xB2: case 0xB3: case 0xB4: return 'o';   // ò ó ô
        case 0xB9: case 0xBA: case 0xBB: return 'u';   // ù ú û
        default: return 0;
    }
}

// Port of a_tokenize(): fold IT vowels, keep lowercased ASCII alnum, split on everything else.
static int t_tokenize(const char *in, char tok[T_MAX_TOKENS][T_TOK_LEN])
{
    int n = 0, len = 0;
    char cur[T_TOK_LEN];
    for (const unsigned char *p = (const unsigned char *)in; ; p++) {
        unsigned char c = *p;
        char out = 0;
        if (c == 0xC3 && p[1]) { out = t_fold(*++p); }
        else if (isalnum(c))   { out = (char)tolower(c); }
        if (out) {
            if (len < T_TOK_LEN - 1) cur[len++] = out;
        } else {
            if (len > 0 && n < T_MAX_TOKENS) { cur[len] = 0; memcpy(tok[n++], cur, len + 1); len = 0; }
            if (c == 0) break;
        }
    }
    return n;
}

static bool t_eq(const char *a, const char *b) { return strcmp(a, b) == 0; }
static bool t_starts(const char *s, const char *pre) { return strncmp(s, pre, strlen(pre)) == 0; }

// An explicit translate verb ("traduci/traduce/traduco/tradurre/traducimi", "translate/translating").
static bool t_is_verb(const char *w)
{
    return t_starts(w, "traduc") || t_starts(w, "tradur") || t_starts(w, "translat");
}
// The noun "traduzione/translation" — a weaker trigger, accepted only WITH an explicit target language.
static bool t_is_noun(const char *w)
{
    return t_starts(w, "traduzion") || t_eq(w, "translation") || t_eq(w, "translations");
}
// A target-language word -> 'e' (translate INTO English) / 'i' (INTO Italian) / 0 (not a language).
static char t_lang(const char *w)
{
    if (t_starts(w, "ingles") || t_eq(w, "english")) return 'e';
    if (t_starts(w, "italian") || t_eq(w, "italiano")) return 'i';
    return 0;
}
// Function words trimmed from the BORDERS of the target span (never from the middle, so "come stai"
// survives). Deliberately excludes come/how/say/etc. — those are real content unless part of a trigger.
static bool t_is_border_word(const char *w)
{
    static const char *fw[] = { "di","del","dello","della","dell","la","il","lo","le","l","un","una","uno",
                                "parola","parole","frase","word","words","phrase","the","a","an","mi","me",
                                "per","in","into","to","verso","nel","nella","that","this", NULL };
    for (int i = 0; fw[i]; i++) if (t_eq(w, fw[i])) return true;
    return false;
}

// Is the token sequence `pat` (nul-terminated list) present consecutively starting somewhere in tok[]?
// Returns the start index or -1. Used for the multi-word frames ("come si dice", "how do you say").
static int t_phrase(char tok[T_MAX_TOKENS][T_TOK_LEN], int ntok, const char *const *pat)
{
    int np = 0; while (pat[np]) np++;
    for (int s = 0; s + np <= ntok; s++) {
        int k = 0;
        for (; k < np; k++) if (!t_eq(tok[s + k], pat[k])) break;
        if (k == np) return s;
    }
    return -1;
}

// Exact dictionary lookup by BINARY SEARCH over an SD file sorted by key in strcmp/byte order. The full
// FreeDict bilingual dictionary is ~60k lines / ~2 MB — a linear scan would crawl megabytes off the SD per
// query. Instead we bisect the byte range (~log2(size) ≈ 22 seeks), each step seeking to the midpoint and
// snapping to the next line start, then LINEAR-scan the final small window (robust against line-boundary
// edge cases). One ~1 KB line buffer, ZERO resident RAM — so the whole dictionary is queried on the MCU in
// a couple dozen disk reads. Returns 1 and fills `out` (readable translations) on an exact key hit, else 0.
static int t_lookup(const char *path, const char *key, char *out, size_t cap)
{
    FILE *f = fopen(path, "rb");                   // binary: ftell/fseek offsets must be byte-exact
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long hi = ftell(f), lo = 0;
    char line[1024];
    int c, found = 0;
    while (hi - lo > 4096) {                        // bisect down to a small window
        long mid = lo + (hi - lo) / 2;
        fseek(f, mid, SEEK_SET);
        while ((c = fgetc(f)) != EOF && c != '\n') {}   // snap to the start of the next line
        long ls = ftell(f);
        if (ls >= hi || !fgets(line, sizeof line, f)) { hi = mid; continue; }
        char *tab = strchr(line, '\t');
        if (!tab) { lo = ftell(f); continue; }
        *tab = 0;
        if (strcmp(line, key) < 0) lo = ftell(f);  // key sorts AFTER this line -> search right
        else hi = mid;                             // key sorts at/before this line -> search left
    }
    // linear scan the final window from lo; a bounded ~8 KB read, exact and edge-case-proof. NB: lo is
    // ALWAYS a line boundary here (it only ever advances to an ftell() taken right after a full line), so
    // we must NOT skip a "partial" line — that would drop the window's first line (the bug that hid "sole").
    fseek(f, lo, SEEK_SET);
    long scanned = 0;
    while (scanned < 8192 && fgets(line, sizeof line, f)) {
        scanned += (long)strlen(line);
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        int cmp = strcmp(line, key);
        if (cmp == 0) {
            char *val = tab + 1; size_t vl = strlen(val);
            while (vl && (val[vl-1] == '\n' || val[vl-1] == '\r' || val[vl-1] == ' ')) val[--vl] = 0;
            snprintf(out, cap, "%s", val); found = 1; break;
        }
        if (cmp > 0) break;                        // sorted: passed where the key would be
    }
    fclose(f);
    return found;
}

// Multi-word frames that signal a translation request ("come si dice", "how do you say"). File-scope so
// both the translator and the detect-only entry point share one definition.
static const char *const T_FR_csd[]  = { "come", "si", "dice", NULL };
static const char *const T_FR_csdn[] = { "come", "si", "dicono", NULL };
static const char *const T_FR_hdys[] = { "how", "do", "you", "say", NULL };
static const char *const T_FR_hdis[] = { "how", "do", "i", "say", NULL };
static const char *const T_FR_hts[]  = { "how", "to", "say", NULL };
static const char *const *const T_FRAMES[] = { T_FR_csd, T_FR_csdn, T_FR_hdys, T_FR_hdis, T_FR_hts, NULL };

// Mark each present frame's tokens in `excl` (when non-NULL) and return whether any frame matched.
static bool t_mark_frames(char tok[T_MAX_TOKENS][T_TOK_LEN], int ntok, bool *excl)
{
    bool found = false;
    for (int fi = 0; T_FRAMES[fi]; fi++) {
        int at = t_phrase(tok, ntok, T_FRAMES[fi]);
        if (at >= 0) { found = true; if (excl) for (int k = 0; T_FRAMES[fi][k]; k++) excl[at + k] = true; }
    }
    return found;
}

// Detect-ONLY: is `raw` a translation request? No dictionary lookup. Lets the orchestrator route to the
// online teacher (Grok) in hybrid/online mode, keeping the dictionary as the offline floor / fallback.
bool nucleo_anima_translate_is_request(const char *raw)
{
    if (!raw || !raw[0]) return false;
    char tok[T_MAX_TOKENS][T_TOK_LEN];
    int ntok = t_tokenize(raw, tok);
    if (ntok < 1) return false;
    bool verb = false, noun = false; char lang = 0;
    for (int i = 0; i < ntok; i++) {
        if (t_is_verb(tok[i])) verb = true;
        else if (t_is_noun(tok[i])) noun = true;
        char l = t_lang(tok[i]); if (l && !lang) lang = l;
    }
    return verb || t_mark_frames(tok, ntok, NULL) || (noun && lang);
}

int nucleo_anima_translate(const char *raw, bool en, anima_result_t *r)
{
    if (!raw || !raw[0]) return 0;
    char tok[T_MAX_TOKENS][T_TOK_LEN];
    int ntok = t_tokenize(raw, tok);
    if (ntok < 1) return 0;                        // empty input -> nothing to do

    // --- classify each token; detect the trigger -------------------------------------------------
    bool excl[T_MAX_TOKENS] = { 0 };
    bool verb = false, noun = false;
    char lang = 0;                                  // target language: 'e' EN, 'i' IT, 0 = auto
    for (int i = 0; i < ntok; i++) {
        if (t_is_verb(tok[i])) { excl[i] = true; verb = true; continue; }
        if (t_is_noun(tok[i])) { excl[i] = true; noun = true; continue; }
        char l = t_lang(tok[i]);
        if (l) { excl[i] = true; if (!lang) lang = l; }
    }
    // Multi-word frames. Each present frame flags the trigger and masks its own tokens (excl).
    bool phrase = t_mark_frames(tok, ntok, excl);

    bool trigger = verb || phrase || (noun && lang);
    if (!trigger) return 0;                         // not a translation request -> let the cascade run

    // --- isolate the target span: the longest run of non-excluded tokens, border-trimmed -----------
    int bs = -1, bl = 0, cs = -1, cl = 0;
    for (int i = 0; i <= ntok; i++) {
        if (i < ntok && !excl[i]) { if (cs < 0) { cs = i; cl = 1; } else cl++; }
        else { if (cl > bl) { bl = cl; bs = cs; } cs = -1; cl = 0; }
    }
    int s = bs, e = bs + bl;                        // [s, e)
    while (s < e && t_is_border_word(tok[s])) s++;  // trim leading function words
    while (e > s && t_is_border_word(tok[e - 1])) e--; // trim trailing

    // A NUMBER is not an IT<->EN word: "traduci 10 in binario" is a base conversion, "traduci 5" is nothing.
    // Decline so the math/base skill owns it — don't swallow the turn with a dictionary miss.
    for (int i = s; i < e; i++) {
        bool alldig = tok[i][0] != 0;
        for (const char *p = tok[i]; *p; p++) if (!isdigit((unsigned char)*p)) { alldig = false; break; }
        if (alldig) return 0;
    }

    r->tier = ANIMA_TIER_COMMAND;
    r->action = ANIMA_ACT_ANSWER;
    snprintf(r->intent, sizeof r->intent, "translate");
    snprintf(r->state, sizeof r->state, "tool");

    if (s >= e) {                                   // a translate verb, but nothing to translate
        r->confidence = 60;
        snprintf(r->reply, sizeof r->reply,
                 en ? "What should I translate? e.g. \"translate dog to Italian\"."
                    : "Cosa traduco? es. \"traduci cane in inglese\".");
        return 1;
    }

    // Rebuild the normalized key from the span (single-spaced) — same shape gen_dicts.py wrote.
    char key[T_MAX_TOKENS * T_TOK_LEN];
    int ko = 0;
    for (int i = s; i < e; i++) {
        if (i > s && ko < (int)sizeof key - 1) key[ko++] = ' ';
        for (const char *q = tok[i]; *q && ko < (int)sizeof key - 1; q++) key[ko++] = *q;
    }
    key[ko] = 0;

    // A single-letter source is degenerate and an accent-fold trap: "traduci è" folds to "e" and would
    // return "and" (è = copula "is", NOT the conjunction "e"/and). Decline rather than assert a false hit.
    if (ko <= 1) return 0;

    // --- lookup, honoring the requested direction (or auto-detecting it) ---------------------------
    char val[512], itv[512], env[512];
    bool to_en = false, hit = false;
    bool in_it = t_lookup(DICT_IT_EN, key, itv, sizeof itv);   // key is an Italian headword
    bool in_en = t_lookup(DICT_EN_IT, key, env, sizeof env);   // key is an English headword
    // HOMOGRAPH GUARD: a word that exists in BOTH languages ("male", "sole", "estate", "fame", "camera",
    // "fine") is ambiguous about its SOURCE — the engine must NOT confidently emit one assumed-source
    // reading ("translate male" / "how do you say male" -> "evil" for the English word "male"). Show BOTH
    // labelled readings so a single answer can never mislead. Applies to EVERY path (explicit direction,
    // "come si dice"/"how do you say" frames, and bare auto) — the frame/auto path set no t_lang, which is
    // exactly where the IT-first default used to surface the wrong-language sense at conf 95.
    // DIREZIONE ESPLICITA ("in inglese"/"in italiano") -> la SORGENTE e' l'ALTRA lingua, quindi NON e'
    // ambigua: traduci in quel verso. (Era il bug: "traduci cane in inglese" dava il dump omografo invece
    // di "dog" perche' la guardia omografo scattava PRIMA della direzione.)
    if (lang == 'e')      { hit = in_it; to_en = true;  if (hit) snprintf(val, sizeof val, "%s", itv); }
    else if (lang == 'i') { hit = in_en; to_en = false; if (hit) snprintf(val, sizeof val, "%s", env); }
    // AUTO (nessuna direzione): se la parola esiste in ENTRAMBE, la SORGENTE e' ambigua -> mostra i due
    // sensi etichettati per non ingannare ("translate male" -> non assumere l'EN "evil"). Solo qui.
    else if (in_it && in_en) {
        r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 75;
        snprintf(r->intent, sizeof r->intent, "translate"); snprintf(r->state, sizeof r->state, "tool");
        snprintf(r->reply, sizeof r->reply,
                 en ? "\"%s\" exists in both languages. IT->EN: %s. EN->IT: %s."
                    : "\"%s\" esiste in entrambe le lingue. IT->EN: %s. EN->IT: %s.", key, itv, env);
        snprintf(r->trace, sizeof r->trace, en ? "dict · homograph" : "dizionario · omografo");
        return 1;
    }
    else {                                          // auto, non-omografo: deduci la lingua sorgente
        if (in_it) { hit = true; to_en = true;  snprintf(val, sizeof val, "%s", itv); }
        else if (in_en) { hit = true; to_en = false; snprintf(val, sizeof val, "%s", env); }
    }

    const char *ln = to_en ? (en ? "English" : "inglese") : (en ? "Italian" : "italiano");
    if (hit) {
        r->confidence = 95;
        snprintf(r->reply, sizeof r->reply, "\"%s\" %s %s: %s.",
                 key, en ? "in" : "in", ln, val);
        snprintf(r->trace, sizeof r->trace, en ? "dict lookup · %s" : "dizionario · %s", ln);
        return 1;
    }

    // Miss on an explicit translation request: decline honestly and STOP (no later tier fabricates one).
    r->confidence = 55;
    snprintf(r->reply, sizeof r->reply,
             en ? "I don't have \"%s\" in the offline IT<->EN dictionary."
                : "Non ho \"%s\" nel dizionario offline IT<->EN.", key);
    snprintf(r->trace, sizeof r->trace, en ? "dict miss" : "dizionario: assente");
    return 1;
}
