// nucleo_tts_plan — the "brain" of the concatenative TTS: Italian text -> a sequence of tokens
// CLIP/PAUSE tokens. Pure (string.h/ctype.h), allocation-free, host-compilable: see
// tools/anima-host/tts-plan-ctest.c for the test. Logic documented in nucleo_tts.h.
#include "nucleo_tts.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdint.h>

// RAM-safe: the text is capped at 220 chars by the guard (nucleo_tts_text_speakable), so it can't
// produce more than ~160 units/tokens. u[MAX_UNITS] lives on the voice_task's stack (16KB): 160 is the
// safe ceiling (coexists with norm[1024]). Kept == TOK_MAX in nucleo_tts.c.
#define MAX_UNITS   160
#define MAX_PHRASE   6     // max words in a phrase match (MOSAICO-style)

// Round Italian cardinals, pre-rendered as atomic clips (mandatory numbers package).
static const char *HUNDREDS[10] = {
    "", "cento", "duecento", "trecento", "quattrocento", "cinquecento",
    "seicento", "settecento", "ottocento", "novecento"
};
static const char *THOUSANDS[10] = {
    "", "mille", "duemila", "tremila", "quattromila", "cinquemila",
    "seimila", "settemila", "ottomila", "novemila"
};

// ---- normalization: UTF-8 -> lowercase ASCII, fold Italian accents --------------------------
static void normalize_ascii(const char *in, char *out, int cap)
{
    int o = 0;
    for (int i = 0; in[i] && o < cap - 1; ) {
        unsigned char c = (unsigned char)in[i];
        if (c == 0xC3 && in[i + 1]) {               // Latin-1 Supplement accents (à è é ì ò ù ç ñ ...)
            unsigned char b = (unsigned char)in[i + 1];
            char f = 0;
            if      ((b >= 0x80 && b <= 0x85) || (b >= 0xA0 && b <= 0xA5)) f = 'a';
            else if ((b >= 0x88 && b <= 0x8B) || (b >= 0xA8 && b <= 0xAB)) f = 'e';
            else if ((b >= 0x8C && b <= 0x8F) || (b >= 0xAC && b <= 0xAF)) f = 'i';
            else if ((b >= 0x92 && b <= 0x96) || (b >= 0xB2 && b <= 0xB6)) f = 'o';
            else if ((b >= 0x99 && b <= 0x9C) || (b >= 0xB9 && b <= 0xBC)) f = 'u';
            else if (b == 0x87 || b == 0xA7) f = 'c';      // ç
            else if (b == 0x91 || b == 0xB1) f = 'n';      // ñ
            if (f) out[o++] = f;
            i += 2;
        } else if (c < 0x80) {
            out[o++] = (char)tolower(c);
            i += 1;
        } else {
            i += 1;                                  // other multibyte: discard the byte
        }
    }
    out[o] = 0;
}

// Like normalize_ascii but PRESERVES the case of ASCII letters (à->a, À->A): used to recognize
// all-caps acronyms (USB/GPS) for spelling out. The slug is then always lowercase (the lexer does tolower).
static void normalize_keepcase(const char *in, char *out, int cap)
{
    int o = 0;
    for (int i = 0; in[i] && o < cap - 1; ) {
        unsigned char c = (unsigned char)in[i];
        if (c == 0xC3 && in[i + 1]) {               // Latin-1 accents: 0x80-0x9F uppercase, 0xA0-0xBF lowercase
            unsigned char b = (unsigned char)in[i + 1];
            char f = 0; int up = 0;
            if      (b >= 0x80 && b <= 0x85) { f = 'a'; up = 1; } else if (b >= 0xA0 && b <= 0xA5) f = 'a';
            else if (b >= 0x88 && b <= 0x8B) { f = 'e'; up = 1; } else if (b >= 0xA8 && b <= 0xAB) f = 'e';
            else if (b >= 0x8C && b <= 0x8F) { f = 'i'; up = 1; } else if (b >= 0xAC && b <= 0xAF) f = 'i';
            else if (b >= 0x92 && b <= 0x96) { f = 'o'; up = 1; } else if (b >= 0xB2 && b <= 0xB6) f = 'o';
            else if (b >= 0x99 && b <= 0x9C) { f = 'u'; up = 1; } else if (b >= 0xB9 && b <= 0xBC) f = 'u';
            else if (b == 0x87)              { f = 'c'; up = 1; } else if (b == 0xA7)              f = 'c';
            else if (b == 0x91)              { f = 'n'; up = 1; } else if (b == 0xB1)              f = 'n';
            if (f) out[o++] = up ? (char)(f - 32) : f;
            i += 2;
        } else if (c < 0x80) {
            out[o++] = (char)c;                      // PRESERVES the case (no tolower here)
            i += 1;
        } else {
            i += 1;
        }
    }
    out[o] = 0;
}

// ---- intermediate units (word / number / pause) ----------------------------------------------
typedef enum { U_WORD = 0, U_NUMBER, U_PAUSE } unit_kind_t;
typedef struct { unit_kind_t kind; char s[48]; int ms; bool allcaps; } unit_t;

static bool is_decimal_dot(const char *a, int len, const char *p)
{
    // ',' or '.' is a decimal separator only if it sits BETWEEN two digits.
    return len > 0 && isdigit((unsigned char)a[len - 1]) && isdigit((unsigned char)p[1]);
}

// Splits the normalized ASCII string into units. Sentence/clause-ending punctuation becomes
// PAUSE; a number (with sign and/or decimal) stays a single NUMBER unit; the rest are WORD.
// Splits the NORMALIZED-keepcase string (case preserved on ASCII letters) into units. Saves the slug
// ALWAYS lowercase (tolower) and flags `allcaps` if the word is made of >=2 letters ALL uppercase
// (acronym: USB/GPS; internal digits don't count, so "MP3" stays allcaps).
static int lex_units(const char *a, unit_t *u, int max)
{
    int n = 0, ci = 0, al = 0, up = 0;
    char cur[48];
    char curkind = 0;   // 0 = empty, 'w' = word, 'd' = numeric
    #define FLUSH() do { if (ci) { cur[ci] = 0; if (n < max) { \
        u[n].kind = (curkind == 'd') ? U_NUMBER : U_WORD; \
        strncpy(u[n].s, cur, sizeof(u[n].s) - 1); u[n].s[sizeof(u[n].s) - 1] = 0; u[n].ms = 0; \
        u[n].allcaps = (curkind == 'w' && al >= 2 && up == al); n++; } \
        ci = 0; curkind = 0; al = 0; up = 0; } } while (0)
    #define PAUSE(d) do { if (n < max) { u[n].kind = U_PAUSE; u[n].s[0] = 0; u[n].ms = (d); u[n].allcaps = false; n++; } } while (0)

    for (int i = 0; a[i]; i++) {
        char c = a[i];
        if (isdigit((unsigned char)c)) {
            if (ci == 0) curkind = 'd';
            if (ci < (int)sizeof(cur) - 1) cur[ci++] = c;
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            if (ci == 0) curkind = 'w';
            al++; if (c >= 'A' && c <= 'Z') up++;        // count for the acronym flag
            if (ci < (int)sizeof(cur) - 1) cur[ci++] = (char)tolower((unsigned char)c);
        } else if (c == '-') {
            // negative sign only at the start of a number (cur empty and a digit right after)
            if (ci == 0 && isdigit((unsigned char)a[i + 1])) { curkind = 'd'; cur[ci++] = '-'; }
            else FLUSH();
        } else if (c == ',' || c == '.') {
            if (curkind == 'd' && is_decimal_dot(cur, ci, &a[i])) {
                if (ci < (int)sizeof(cur) - 1) cur[ci++] = ',';   // canonicalize the decimal separator to ','
            } else {
                FLUSH();
                PAUSE(c == '.' ? 320 : 160);
            }
        } else if (c == '!' || c == '?') {
            FLUSH(); PAUSE(360);
        } else if (c == ';' || c == ':') {
            FLUSH(); PAUSE(220);
        } else {
            FLUSH();   // space or other punctuation: separator without a pause
        }
    }
    FLUSH();
    #undef FLUSH
    #undef PAUSE
    return n;
}

// ---- token emission --------------------------------------------------------------------------
typedef struct { tts_token_t *out; int max; int n; bool en; } sink_t;

static void push_clip(sink_t *s, const char *slug, bool fb)
{
    if (s->n >= s->max || !slug || !slug[0]) return;
    s->out[s->n].kind = TTS_TOK_CLIP;
    strncpy(s->out[s->n].slug, slug, sizeof(s->out[s->n].slug) - 1);
    s->out[s->n].slug[sizeof(s->out[s->n].slug) - 1] = 0;
    s->out[s->n].ms = 0;
    s->out[s->n].fallback = fb;
    s->n++;
}
static void push_pause(sink_t *s, int ms)
{
    if (s->n >= s->max || ms <= 0) return;
    s->out[s->n].kind = TTS_TOK_PAUSE;
    s->out[s->n].slug[0] = 0;
    s->out[s->n].ms = ms;
    s->out[s->n].fallback = false;
    s->n++;
}
static void push_num(sink_t *s, int v, bool fb)   // clip "nV" (atomic 0..99)
{
    char b[8]; int o = 0; b[o++] = 'n';
    if (v >= 10) b[o++] = (char)('0' + v / 10);
    b[o++] = (char)('0' + v % 10);
    b[o] = 0; push_clip(s, b, fb);
}

// > 999999 (or overflow): digit-by-digit reading (graceful fallback), from the most significant.
static void emit_big_digits(sink_t *s, long v)
{
    if (v < 0) v = -v;
    char rev[24]; int rn = 0; long t = v;
    while (t > 0 && rn < (int)sizeof(rev)) { rev[rn++] = (char)('0' + t % 10); t /= 10; }
    if (rn == 0) { push_num(s, 0, true); return; }
    for (int i = rn - 1; i >= 0; i--) push_num(s, rev[i] - '0', true);
}

// Integer -> ITALIAN cardinal. 0..99 atomic (resolves agglutination: ventuno/ventotto);
// 101..999 = "duecento.." + remainder; 1000..999999 = "mille"/"duemila.." + remainder (1984 -> "mille"
// "novecento" "ottantaquattro", natural segmentation for years). No internal "e".
static void emit_int_it(sink_t *s, long v)
{
    if (v < 0) v = -v;
    if (v < 100)     { push_num(s, (int)v, false); return; }   // 0..99 atomic; 100 -> "cento"
    if (v < 1000)    { int h = (int)(v / 100), r = (int)(v % 100);
                       push_clip(s, HUNDREDS[h], false); if (r) emit_int_it(s, r); return; }
    if (v < 1000000) { int th = (int)(v / 1000), r = (int)(v % 1000);
                       if (th == 1)      push_clip(s, "mille", false);
                       else if (th <= 9) push_clip(s, THOUSANDS[th], false);
                       else { emit_int_it(s, th); push_clip(s, "mila", false); }
                       if (r) emit_int_it(s, r);
                       return; }
    emit_big_digits(s, v);
}

// Integer -> ENGLISH cardinal. 0..99 atomic ("twenty-three"); then composition "two hundred
// thirty-four", "one thousand nine hundred eighty-four". Connectors "hundred"/"thousand".
static void emit_int_en(sink_t *s, long v)
{
    if (v < 0) v = -v;
    if (v < 100)     { push_num(s, (int)v, false); return; }
    if (v < 1000)    { int h = (int)(v / 100), r = (int)(v % 100);
                       push_num(s, h, false); push_clip(s, "hundred", false); if (r) emit_int_en(s, r); return; }
    if (v < 1000000) { int th = (int)(v / 1000), r = (int)(v % 1000);
                       emit_int_en(s, th); push_clip(s, "thousand", false); if (r) emit_int_en(s, r); return; }
    emit_big_digits(s, v);
}

// NUMBER unit ("-?digits(,digits)?") -> clip. Sign -> "meno"/"minus"; decimal -> "virgola"/"point"
// + digits. The language (s->en) picks the cardinals and connectors.
static void emit_number(sink_t *s, const char *str)
{
    const char *p = str;
    if (*p == '-') { push_clip(s, s->en ? "minus" : "meno", false); p++; }
    long ip = 0; int ndig = 0;
    while (*p && *p != ',' && ndig < 12) { if (*p >= '0' && *p <= '9') { ip = ip * 10 + (*p - '0'); ndig++; } p++; }
    if (ndig > 6) {                                  // too large for the value: digit-by-digit
        const char *q = str; if (*q == '-') q++;
        for (; *q && *q != ','; q++) if (*q >= '0' && *q <= '9') push_num(s, *q - '0', true);
    } else if (s->en) emit_int_en(s, ip);
    else              emit_int_it(s, ip);
    if (*p == ',') {                                 // decimal part: "virgola"/"point" then digits
        push_clip(s, s->en ? "point" : "virgola", false);
        for (p++; *p; p++) if (*p >= '0' && *p <= '9') push_num(s, *p - '0', false);
    }
}

// Unknown word (no clip): emits an UNKNOWN token, keeping the WORD (for diagnostics:
// states exactly what's missing). Spelling isn't attempted here: nucleo_tts_say() will play "leggila".
static void push_unknown(sink_t *s, const char *word)
{
    if (s->n >= s->max) return;
    s->out[s->n].kind = TTS_TOK_UNKNOWN;
    snprintf(s->out[s->n].slug, sizeof s->out[s->n].slug, "%s", word ? word : "");
    s->out[s->n].ms = 0;
    s->out[s->n].fallback = true;
    s->n++;
}

// ---- ACRONYM spelling: USB/GPS/MP3 (all-caps, uncovered) -> letter-by-letter ----
// An all-caps acronym that has NO clip of its own is spelled out: each letter with its
// lett_<x> clip ("u esse bi"), each digit with the nN cardinal. All-or-nothing: only emits if EVERY letter has
// its clip (otherwise -> UNKNOWN -> "leggila"), so before the letter clips are deployed the
// behavior stays unchanged. Only 2..6 characters (real acronyms; beyond that, spelling sounds worse
// than "leggila"). `w` is already lowercase (the slug); the allcaps flag on the original case already decided this.
#define TTS_ACRO_MIN 2
#define TTS_ACRO_MAX 6
static bool spell_acronym(sink_t *s, const char *w, tts_has_clip_fn has, void *ud)
{
    int len = (int)strlen(w);
    if (len < TTS_ACRO_MIN || len > TTS_ACRO_MAX || !has) return false;
    bool any_letter = false;
    for (int k = 0; k < len; k++) {                  // coverage BEFORE emitting (no partial spelling)
        char c = w[k];
        if (c >= '0' && c <= '9') continue;          // digit -> nN (always in the mandatory package)
        if (c < 'a' || c > 'z') return false;        // character not spellable
        char slug[7] = { 'l', 'e', 't', 't', '_', c, 0 };
        if (!has(slug, ud)) return false;            // missing letter clip -> no spelling
        any_letter = true;
    }
    if (!any_letter) return false;                   // digits only (e.g. "00"): leave it to the numeric path
    for (int k = 0; k < len; k++) {
        char c = w[k];
        if (c >= '0' && c <= '9') push_num(s, c - '0', true);
        else { char slug[7] = { 'l', 'e', 't', 't', '_', c, 0 }; push_clip(s, slug, true); }
    }
    return true;
}

// ---- COMPOSITIONAL planner: scale coverage WITHOUT new clips ------------------------------
// An uncovered word (compounds/derivatives: "portacenere", "buonasera", "watercolor", "autostrada")
// is DECOMPOSED into 2..3 already-covered sub-words (LONGEST prefix first + backtracking; each
// part >= 3 chars to avoid absurd fragmentation). This way thousands of compound/derived words are
// spoken by concatenating existing clips, at ZERO file cost. Safe: only triggers on uncovered words
// and ONLY if the WHOLE word is made of real clips (>=3 chars) -> no random spelling, or UNKNOWN.
#define TTS_DECOMP_MINSUB   3
#define TTS_DECOMP_MAXPARTS 3
static int tts_decompose(const char *w, int len, tts_has_clip_fn has, void *ud, char parts[][48], int maxp)
{
    if (len == 0) return 0;                                  // fully consumed -> success
    if (maxp <= 0) return -1;                                // too many parts
    for (int L = (len < 47 ? len : 47); L >= TTS_DECOMP_MINSUB; L--) {   // longest prefix first
        char pre[48];
        for (int k = 0; k < L; k++) pre[k] = w[k];
        pre[L] = 0;
        if (has && has(pre, ud)) {
            int rest = tts_decompose(w + L, len - L, has, ud, parts + 1, maxp - 1);
            if (rest >= 0) { for (int k = 0; k <= L; k++) parts[0][k] = pre[k]; return rest + 1; }
        }
    }
    return -1;
}

// ---- slug of the whole text (== build_voice.py's slugify) ------------------------------
void nucleo_tts_full_slug(const char *text, char *out, int cap)
{
    if (!out || cap <= 0) return;
    out[0] = 0;
    if (!text) return;
    char norm[1024];
    normalize_ascii(text, norm, sizeof norm);        // fold accents -> lowercase ascii
    int o = 0; bool us = false;
    for (int i = 0; norm[i] && o < cap - 1; i++) {
        char c = norm[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) { out[o++] = c; us = false; }
        else if (!us && o > 0) { out[o++] = '_'; us = true; }
    }
    while (o > 0 && out[o - 1] == '_') o--;           // no trailing '_'
    out[o] = 0;
}

// ---- content guard (OFFLINE) ----------------------------------------------------------
bool nucleo_tts_text_speakable(const char *text, int max_chars)
{
    if (!text || !text[0]) return false;
    int len = (int)strlen(text);
    if (max_chars > 0 && len > max_chars) return false;            // too long

    // Strong signals of code/markup: these never appear in a spoken reply.
    static const char *CODE[] = {
        "```", "</", "/>", "=>", "->", "::", "#include", "function", "def ", "return ",
        "printf", "console.", "import ", "void ", "int ", NULL
    };
    for (int i = 0; CODE[i]; i++) if (strstr(text, CODE[i])) return false;
    // '=' = always an expression/assignment (a calculation): never in natural speech -> "leggila".
    // (NB: nucleo_tts_say applies mathspeak BEFORE the guard, so the '=' '%' '^' '+' from calc/percent
    // have already become words; an '=' that reaches here comes from a dense formula -> "leggila" is correct.)
    if (strchr(text, '`') || strchr(text, '{') || strchr(text, '}') || strchr(text, '=')) return false;

    // Math typography (geometry/physics/vector/equation formulas): never in natural speech, and
    // the planner doesn't know how to say it (it would drop it, losing the meaning: "pi times r squared" -> "r").
    // Better to just say "leggila" for the whole thing. UTF-8 sequences: · ² ³ ½ √ π ρ ± ≈ Δ ✓ × ÷.
    static const char *const MATHSYM[] = {
        "\xC2\xB7", "\xC2\xB2", "\xC2\xB3", "\xC2\xBD",          // · ² ³ ½
        "\xE2\x88\x9A", "\xCF\x80", "\xCF\x81",                  // √ π ρ
        "\xC2\xB1", "\xE2\x89\x88", "\xCE\x94", "\xE2\x9C\x93",  // ± ≈ Δ ✓
        "\xC3\x97", "\xC3\xB7", NULL                             // × ÷
    };
    for (int i = 0; MATHSYM[i]; i++) if (strstr(text, MATHSYM[i])) return false;

    // Density of "technical" characters: >12% -> it's code/an expression, not speech.
    int code_ch = 0;
    for (const char *p = text; *p; p++) if (strchr(";()[]=<>/\\|*_", (unsigned char)*p)) code_ch++;
    if (len > 0 && code_ch * 100 / len > 12) return false;
    return true;
}

// ---- SPEAKABLE text for the time --------------------------------------------------------------
// Same text on BOTH SCREEN and VOICE. The idiomatic forms (quarter/half/noon/midnight) are
// used ONLY when the time falls exactly on them -> it always stays precise to the minute, never rounded.
void nucleo_tts_speak_time(char *out, int n, int h, int m, const char *lang)
{
    if (!out || n <= 0) return;
    out[0] = 0;
    if (h < 0 || h > 23 || m < 0 || m > 59) return;
    bool en = lang && lang[0] == 'e' && lang[1] == 'n';

    if (en) {
        if (m == 0) {
            if      (h == 0)  snprintf(out, n, "It is midnight");
            else if (h == 12) snprintf(out, n, "It is noon");
            else              snprintf(out, n, "It is %d o'clock", h);
        } else {
            snprintf(out, n, "It is %d %d", h, m);          // "It is 9 30" -> "nine thirty"
        }
        return;
    }

    // Italian (24h). Name of the hour: 0 -> mezzanotte, 12 -> mezzogiorno, otherwise "Sono le N".
    #define HOURNAME(buf, hh) do {                                            \
        if      ((hh) == 0)  snprintf((buf), sizeof(buf), "Mezzanotte");      \
        else if ((hh) == 12) snprintf((buf), sizeof(buf), "Mezzogiorno");     \
        else                 snprintf((buf), sizeof(buf), "Sono le %d", (hh)); \
    } while (0)

    char base[32];
    if (m == 45) {                                          // 4:45 -> "le 5 meno un quarto"; 11:45 -> "mezzogiorno meno un quarto"
        HOURNAME(base, (h + 1) % 24);
        snprintf(out, n, "%s meno un quarto", base);
    } else {
        HOURNAME(base, h);
        if (m == 0) {
            if (h == 0 || h == 12) snprintf(out, n, "%s", base);             // "Mezzanotte" / "Mezzogiorno"
            else                   snprintf(out, n, "%s in punto", base);    // "Sono le 9 in punto"
        }
        else if (m == 15) snprintf(out, n, "%s e un quarto", base);
        else if (m == 30) snprintf(out, n, "%s e mezza", base);
        else              snprintf(out, n, "%s e %d", base, m);              // "Sono le 9 e 7" / "Mezzanotte e 7"
    }
    #undef HOURNAME
}

// ---- "speech-ify" the math symbols ------------------------------------------------------
// The solver puts symbols in its answers that speech can't handle: '=' (the "looks like code" guard
// blocks the WHOLE sentence), '%' and '^' (the lexer discards them as separators, losing "per cento"/"elevato").
// Here we replace them with the matching WORDS before planning, so "Fa 16", "Il 20% di 150
// = 30", "5^3 = 125" get spoken. Only touches these three: the others (/ · ² ³ ( ) π) remain and, if
// present, make the utterance fall back to "leggila" (fair: ohm/geometry are symbol-dense).
// Pure/allocation-free; no-op if none of the three are present. `out` always terminated (truncates if needed).
void nucleo_tts_mathspeak(const char *in, char *out, int n, const char *lang)
{
    if (!out || n <= 0) return;
    out[0] = 0;
    if (!in) return;
    bool en = lang && lang[0] == 'e' && lang[1] == 'n';
    const char *EQ = en ? " equals "          : " uguale a ";
    const char *PC = en ? " percent "         : " per cento ";
    const char *PW = en ? " to the power of "  : " elevato ";
    const char *PL = en ? " plus "             : " piu ";   // '+' in sums/perimeters ("3+4+5", "5 + 3")
    int o = 0;
    for (int i = 0; in[i] && o < n - 1; i++) {
        const char *rep = (in[i] == '=') ? EQ : (in[i] == '%') ? PC :
                          (in[i] == '^') ? PW : (in[i] == '+') ? PL : NULL;
        if (rep) for (int k = 0; rep[k] && o < n - 1; k++) out[o++] = rep[k];
        else     out[o++] = in[i];
    }
    out[o] = 0;
}

// True if `text` contains dense math typography (· ² ³ √ π Δ ½) = it's a FORMULA (geometry/
// physics/vectors) that the voice can't say: the call site can then say just the numeric RESULT.
bool nucleo_tts_has_mathtypo(const char *text)
{
    if (!text) return false;
    static const char *const S[] = {
        "\xC2\xB7", "\xC2\xB2", "\xC2\xB3", "\xC2\xBD",          // · ² ³ ½
        "\xE2\x88\x9A", "\xCF\x80", "\xCE\x94", NULL             // √ π Δ
    };
    for (int i = 0; S[i]; i++) if (strstr(text, S[i])) return true;
    return false;
}

// ---- reading speed (WAV header rate) ---------------------------------------------
int nucleo_tts_speed_clamp(int pct)
{
    if (pct < TTS_SPEED_MIN) return TTS_SPEED_MIN;
    if (pct > TTS_SPEED_MAX) return TTS_SPEED_MAX;
    return pct;
}

uint32_t nucleo_tts_speed_rate(uint32_t base_rate, int pct)
{
    pct = nucleo_tts_speed_clamp(pct);
    uint64_t r = (uint64_t)base_rate * (uint32_t)pct / 100u;
    if (r < 8000)  r = 8000;        // below/above these the I2S (and play_wav) aren't reliable:
    if (r > 48000) r = 48000;       // keep the rate sane regardless (the pct clamp is usually enough)
    return (uint32_t)r;
}

// ---- anti-click: linear fade at the clip edges (see nucleo_tts.h) -------------------------
// Works per CHUNK: for each buffer sample it computes its GLOBAL index within the clip (base+i) and
// applies a linear gain if it falls within the first/last `fade` samples. This way the result is identical
// no matter how the streaming splits up the clip. Sample-aligned (byte-by-byte LE: no cast to int16* ->
// no misalignment fault on xtensa). The clip|clip and clip|silence seams pass through ~0.
void nucleo_tts_declick_chunk(unsigned char *buf, int nbytes, uint32_t chunk_off, uint32_t clip_len, int fade)
{
    if (!buf || nbytes < 2 || (chunk_off & 1u) || clip_len < 2) return;   // needs sample alignment
    int total = (int)(clip_len / 2);                 // total samples in the clip
    int F = fade;
    if (F <= 0 || total < 4) return;                  // clip too short: no audible click to smooth
    if (F > total / 2) F = total / 2;                 // fade-in and fade-out don't overlap
    int base = (int)(chunk_off / 2);                  // global index of the chunk's 1st sample
    int ns = nbytes / 2;
    for (int i = 0; i < ns; i++) {
        int g = base + i;
        if (g >= total) break;                        // beyond the clip (oversized chunk): stop
        int num = -1;                                 // -1 = full gain (sample untouched)
        if (g < F)              num = g;              // fade-in: 0 .. F-1  (out of F) -> first sample = 0
        else if (g >= total - F) num = total - 1 - g; // fade-out: F-1 .. 0 (out of F) -> last sample = 0
        if (num >= 0) {
            int s = (int16_t)((uint16_t)buf[2 * i] | ((uint16_t)buf[2 * i + 1] << 8));   // LE -> int16
            s = (int)((long)s * num / F);
            buf[2 * i]     = (unsigned char)(s & 0xFF);
            buf[2 * i + 1] = (unsigned char)((s >> 8) & 0xFF);
        }
    }
}

// ---- numeric result of a formula (geometry/physics) ------------------------------------
// Formula answers ("Area del cerchio = π·5² = 78.5398.") are symbol-dense -> the guard sends them
// to "leggila". But the RESULT after the LAST "= " is often a CLEAN number: we extract it so the
// voice says "Il risultato e' 78.5398" instead of "leggila". true ONLY if the result is a pure number
// (digits/sign/comma/dot/spaces): forms with units ("= 25 m/s") or formulas without "=" -> false.
bool nucleo_tts_eq_result(const char *reply, char *out, int n)
{
    if (!reply || !out || n <= 0) return false;
    out[0] = 0;
    const char *last = NULL;
    for (const char *m = reply; (m = strstr(m, "= ")) != NULL; m += 2) last = m + 2;   // last "= "
    if (!last) return false;
    int o = 0;
    for (const char *q = last; *q && o < n - 1; q++) out[o++] = *q;
    while (o > 0 && (out[o - 1] == '.' || out[o - 1] == ' ')) o--;     // strip trailing dot/spaces
    out[o] = 0;
    if (!out[0]) return false;
    for (const char *q = out; *q; q++)                                  // must be a pure NUMBER
        if (!((*q >= '0' && *q <= '9') || *q == '.' || *q == ',' || *q == '-' || *q == ' ')) return false;
    return true;
}

// ---- translator: speak the translation IN ITS OWN language ------------------------------
// The translator skill replies `"<src>" in <lingua>: <target>.` — the <target> is in the OTHER language,
// so the (mono-language) voice index doesn't cover it and the whole sentence would fall back to "leggila". Here we extract
// the translated WORD and its LANGUAGE ("en"/"it"): so nucleo_tts_say(word, lang) speaks it with
// the RIGHT index ("how do you say dog in Italian" -> says "cane" in Italian). The homograph/miss
// forms (with "->"/"<->") -> false: they fall back to "leggila" (correct, they're explanations). Pure/testable.
bool nucleo_tts_translate_word(const char *reply, char *word, int wn, char *lang, int ln)
{
    if (!reply || !word || wn <= 0 || !lang || ln <= 0) return false;
    word[0] = 0; lang[0] = 0;
    static const char *const M[][2] = {
        {" in inglese: ", "en"}, {" in italiano: ", "it"},
        {" in English: ", "en"}, {" in Italian: ", "it"}, {NULL, NULL}
    };
    const char *p = NULL, *lg = NULL;
    for (int i = 0; M[i][0]; i++) { const char *m = strstr(reply, M[i][0]); if (m) { p = m + strlen(M[i][0]); lg = M[i][1]; break; } }
    if (!p) return false;
    int o = 0;
    while (*p && *p != '.' && o < wn - 1) word[o++] = *p++;   // the translation up to the final dot
    while (o > 0 && word[o - 1] == ' ') o--;
    word[o] = 0;
    snprintf(lang, ln, "%s", lg);
    return word[0] != 0;
}

// ---- short "gist": the FIRST sentence of a long answer -------------------------------------
// "Mosaic voice" INNOVATION: a descriptive answer (knowledge/MOSAICO) is too long for
// speech, but the FIRST sentence is usually the definition/gist ("La fotosintesi e' il processo..."). We
// extract it and try to say it SHORT; with a rich pool it's often covered -> spoken, otherwise "leggila".
// Stops at the first '.'/'!'/'?' that truly ends a sentence (not a decimal "3.14" nor an abbreviation with a
// single letter "Dr."). Copies up to out[n-1]; if no terminator, copies (truncated) and leaves it to the guard.
void nucleo_tts_first_sentence(const char *in, char *out, int n)
{
    if (!out || n <= 0) return;
    out[0] = 0;
    if (!in) return;
    int i = 0; while (in[i] == ' ' || in[i] == '\t' || in[i] == '\n') i++;   // skip leading spaces
    int o = 0;
    for (; in[i] && o < n - 1; i++) {
        char c = in[i];
        out[o++] = c;
        if (c == '.' || c == '!' || c == '?') {
            char nx = in[i + 1];
            bool between_digits = (c == '.') && o >= 2 &&
                                  out[o - 2] >= '0' && out[o - 2] <= '9' && nx >= '0' && nx <= '9';
            bool initial = (c == '.') && o >= 2 && out[o - 2] >= 'A' && out[o - 2] <= 'Z' &&
                           (o < 3 || out[o - 3] == ' ');   // "Dr." "J." -> not end of sentence
            if (!between_digits && !initial && (nx == 0 || nx == ' ' || nx == '\n' || nx == '\t' || nx == '"'))
                break;                                       // end of first sentence
        }
    }
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\t')) o--;
    out[o] = 0;
}

// ---- public planner -------------------------------------------------------------------------
int nucleo_tts_plan(const char *text, const char *lang,
                    tts_has_clip_fn has_clip, void *ud,
                    tts_token_t *out, int max)
{
    if (!text || !out || max <= 0) return 0;
    bool en = lang && lang[0] == 'e' && lang[1] == 'n';   // "en" -> English, otherwise Italian

    char norm[1024];
    normalize_keepcase(text, norm, sizeof(norm));   // case preserved: the lexer flags acronyms (allcaps)

    unit_t u[MAX_UNITS];
    int nu = lex_units(norm, u, MAX_UNITS);

    sink_t s = { out, max, 0, en };
    for (int i = 0; i < nu && s.n < max; ) {
        if (u[i].kind == U_PAUSE) { push_pause(&s, u[i].ms); i++; continue; }
        if (u[i].kind == U_NUMBER) { emit_number(&s, u[i].s); i++; continue; }

        // U_WORD: tries a greedy PHRASE match (consecutive words joined by '_'), from the longest.
        int span = 0;
        while (span < MAX_PHRASE && i + span < nu && u[i + span].kind == U_WORD) span++;
        bool matched = false;
        for (int k = span; k >= 1; k--) {
            char slug[48]; int o = 0;
            bool toolong = false;
            for (int j = 0; j < k; j++) {
                const char *w = u[i + j].s;
                if (j > 0) { if (o < (int)sizeof(slug) - 1) slug[o++] = '_'; else { toolong = true; break; } }
                for (int c = 0; w[c]; c++) { if (o < (int)sizeof(slug) - 1) slug[o++] = w[c]; else { toolong = true; break; } }
                if (toolong) break;
            }
            slug[o] = 0;
            if (!toolong && o > 0 && has_clip && has_clip(slug, ud)) {
                push_clip(&s, slug, false);
                i += k;
                matched = true;
                break;
            }
        }
        if (matched) continue;

        // no phrase: a single covered word -> clip; otherwise, if it's an uncovered all-caps
        // ACRONYM (USB/GPS/MP3), spell it with the letter clips; then try to DECOMPOSE it into
        // covered sub-words (compounds/derivatives, zero new files); if even that fails -> UNKNOWN (-> "leggila").
        if (has_clip && has_clip(u[i].s, ud)) { push_clip(&s, u[i].s, false); i++; continue; }
        if (u[i].allcaps && spell_acronym(&s, u[i].s, has_clip, ud)) { i++; continue; }
        char parts[TTS_DECOMP_MAXPARTS + 1][48];
        // OVERFLOW-SAFE: every sub-part is >= MINSUB chars, so a decomposition (>=2 parts) requires
        // >= 2*MINSUB chars. Below that threshold it's MATHEMATICALLY impossible: skip it without wasting
        // binary searches on SD (SPI bus shared with the display) in the common case of a short uncovered word.
        int wl = (int)strlen(u[i].s);
        int np = (has_clip && wl >= 2 * TTS_DECOMP_MINSUB) ? tts_decompose(u[i].s, wl, has_clip, ud, parts, TTS_DECOMP_MAXPARTS) : -1;
        if (np >= 2) for (int j = 0; j < np && s.n < max; j++) push_clip(&s, parts[j], true);   // composition
        else         push_unknown(&s, u[i].s);
        i++;
    }
    return s.n;
}
