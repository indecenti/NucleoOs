// ANIMA math/skills solver engine  extracted from nucleo_anima.c for scalability.
// Pure, allocation-light computation: arithmetic, units/dimensional analysis, geometry,
// physics, vectors, percent/Ohm/powers/roots, number bases, roman numerals, spreadsheet,
// date arithmetic. Communicates with the orchestrator only via a_sitem_t + anima_result_t.
// Cross-TU contract lives in anima_internal.h. See docs/anima.md �2.
#include "nucleo_anima.h"
#include "anima_internal.h"
#include "nucleo_board.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>

#define UNITS_PATH NUCLEO_SD_MOUNT "/data/anima/units.txt"   // learned custom units (persisted)

// ---- tool: calc (pure arithmetic, no model) ---------------------------------
// Folds the raw input to a clean expression: × ÷ and IT/EN word operators (per/times,
// diviso/divided, piu/plus, meno/minus) become symbols, filler words are dropped. Then a
// tiny recursive-descent parser evaluates + - * / and parentheses. Honest: only fires when
// the input is really an arithmetic expression (>=1 digit AND >=1 operator, parses fully),
// otherwise it declines and the query falls through to retrieval. No % (avoids the
// percent-vs-modulo trap) and division by zero is reported, never guessed.

// Map an alpha word (already lowercased/de-accented) to an operator symbol, or 0 if filler.
static char a_word_op(const char *w)
{
    static const char *mul[] = { "per", "times", "x", "moltiplicato", NULL };
    static const char *dv[]  = { "diviso", "fratto", "divided", NULL };
    static const char *add[] = { "piu", "plus", NULL };
    static const char *sub[] = { "meno", "minus", NULL };
    for (int i = 0; mul[i]; i++) if (!strcmp(w, mul[i])) return '*';
    for (int i = 0; dv[i];  i++) if (!strcmp(w, dv[i]))  return '/';
    for (int i = 0; add[i]; i++) if (!strcmp(w, add[i])) return '+';
    for (int i = 0; sub[i]; i++) if (!strcmp(w, sub[i])) return '-';
    return 0;
}

// Fold raw UTF-8 into a clean arithmetic string in `ex`. Returns false if it can't be one.
static bool a_fold_calc(const char *raw, char *ex, size_t exsz)
{
    char f[160]; int fl = 0;                         // pass 1: lowercase/de-accent, keep math chars
    for (const unsigned char *p = (const unsigned char *)raw; *p && fl < (int)sizeof(f) - 1; p++) {
        unsigned char c = *p;
        char out;
        if (c == 0xC3 && p[1]) {
            unsigned char d = *++p;
            switch (d) {
                case 0xA0: case 0xA1: case 0xA2: out = 'a'; break;
                case 0xA8: case 0xA9: case 0xAA: out = 'e'; break;
                case 0xAC: case 0xAD: case 0xAE: out = 'i'; break;
                case 0xB2: case 0xB3: case 0xB4: out = 'o'; break;
                case 0xB9: case 0xBA: case 0xBB: out = 'u'; break;
                case 0x97: out = '*'; break;          // ×
                case 0xB7: out = '/'; break;          // ÷
                default:   out = ' '; break;
            }
        } else if ((c == 'x' || c == 'X') && fl > 0 &&
                   isdigit((unsigned char)f[fl-1]) && isdigit((unsigned char)p[1])) {
            out = '*';                            // glued multiply sign: "4x4" -> "4*4", "3x3x3"
        } else if (isalpha(c) || isdigit(c)) {
            out = (char)tolower(c);
        } else if (c == '-' && fl > 0 && isalpha((unsigned char)f[fl-1])) {
            out = ' ';   // "Floonk-9", "Floonkonium-200": a hyphen after a NAME letter is not a minus — else
                         // "50 percent of Floonkonium-200" folds to "50 - 200" = -150 on a nonsense entity.
        } else if (c == '+' || c == '-' || c == '*' || c == '/' || c == '^' || c == '(' || c == ')' || c == '.') {
            out = (char)c;
        } else if (c == ',') {
            out = '.';                                // Italian decimal comma
        } else {
            out = ' ';
        }
        f[fl++] = out;
    }
    f[fl] = 0;

    int el = 0; int ndig = 0, nop = 0, nbinop = 0;   // pass 2: tokenize, map words, rebuild
    char tok[40]; int tl = 0;
    for (int i = 0; i <= fl; i++) {
        char c = (i < fl) ? f[i] : ' ';
        bool issep = (c == ' ');
        bool issym = (c == '+' || c == '-' || c == '*' || c == '/' || c == '^' || c == '(' || c == ')');
        if (!issep && !issym) { if (tl < 39) tok[tl++] = c; continue; }
        if (tl) {                                     // flush the pending word/number token
            tok[tl] = 0;
            bool hasalpha = false, hasdigit = false;
            for (int k = 0; k < tl; k++) { if (isalpha((unsigned char)tok[k])) hasalpha = true; if (isdigit((unsigned char)tok[k])) hasdigit = true; }
            if (hasalpha && !hasdigit) {              // pure word -> operator or drop
                char op = a_word_op(tok);
                if (op && el < (int)exsz - 2) { ex[el++] = ' '; ex[el++] = op; nop++; if (ndig >= 1) nbinop++; }
            } else {                                  // number (or number-ish): keep verbatim
                for (int k = 0; k < tl && el < (int)exsz - 2; k++) { ex[el++] = tok[k]; if (isdigit((unsigned char)tok[k])) ndig++; }
                ex[el] = 0;
            }
            tl = 0;
        }
        if (issym && el < (int)exsz - 2) {            // keep the symbol operator
            ex[el++] = ' '; ex[el++] = c;
            if (c == '+' || c == '-' || c == '*' || c == '/' || c == '^') { nop++; if (ndig >= 1) nbinop++; }
        }
    }
    ex[el] = 0;
    (void)nop;
    // A real calculation needs a BINARY operator (operands on both sides). A lone unary-signed number —
    // e.g. "-9" folded from the entity name "Flixxon-9", or "-7" from "Plimptonium-7" — has an operator but
    // NO digit before it, so it must NOT be treated as arithmetic (that fabricated "Fa -9." on nonsense).
    return ndig >= 1 && nbinop >= 1;
}

// Tiny recursive-descent evaluator over the folded expression string.
typedef struct { const char *p; bool err; bool divzero; } a_calc_t;
static double a_cexpr(a_calc_t *c);
static void   a_cskip(a_calc_t *c) { while (*c->p == ' ') c->p++; }
static double a_cprim(a_calc_t *c)
{
    a_cskip(c);
    if (*c->p == '(') { c->p++; double v = a_cexpr(c); a_cskip(c); if (*c->p == ')') c->p++; else c->err = true; return v; }
    if (*c->p == '-') { c->p++; return -a_cprim(c); }
    if (*c->p == '+') { c->p++; return  a_cprim(c); }
    char *end; double v = strtod(c->p, &end);
    if (end == c->p) { c->err = true; return 0; }
    c->p = end; return v;
}
// Power: right-associative, binds tighter than * and /  (2^10, 2*3^2 = 2*9).
static double a_cpow(a_calc_t *c)
{
    double b = a_cprim(c);
    a_cskip(c);
    if (*c->p == '^') { c->p++; double e = a_cpow(c); return pow(b, e); }
    return b;
}
static double a_cterm(a_calc_t *c)
{
    double v = a_cpow(c);
    for (;;) {
        a_cskip(c); char op = *c->p;
        if (op == '*') { c->p++; v *= a_cpow(c); }
        else if (op == '/') { c->p++; double d = a_cpow(c); if (d == 0) { c->divzero = c->err = true; return 0; } v /= d; }
        else break;
    }
    return v;
}
static double a_cexpr(a_calc_t *c)
{
    double v = a_cterm(c);
    for (;;) {
        a_cskip(c); char op = *c->p;
        if (op == '+') { c->p++; v += a_cterm(c); }
        else if (op == '-') { c->p++; v -= a_cterm(c); }
        else break;
    }
    return v;
}

// Try to evaluate `raw` as arithmetic. Returns 0=not a calc, 1=ok (*out set), 2=divide-by-zero.
int a_try_calc(const char *raw, double *out)
{
    // A 3-part date with a 4-digit year (24/04/2027, 2027-04-24, 24.04.2027) is NOT arithmetic — don't
    // let "/" "-" "." turn it into a division/subtraction. (Two-part "12/4" stays a real division.)
    {
        const char *p = raw; while (*p == ' ') p++;
        int g[3] = { 0, 0, 0 }, gi = 0; bool ok = true;
        for (; *p && *p != '\n'; p++) {
            if (*p >= '0' && *p <= '9') { if (gi < 3) g[gi]++; }
            else if (*p == '/' || *p == '-' || *p == '.') { if (++gi > 2) { ok = false; break; } }
            else if (*p == ' ') { /* spaces ok */ }
            else { ok = false; break; }
        }
        if (ok && gi == 2 && g[0] >= 1 && g[1] >= 1 && g[2] >= 1 && (g[0] == 4 || g[2] == 4)) return 0;
    }
    char ex[96];
    if (!a_fold_calc(raw, ex, sizeof(ex))) return 0;
    a_calc_t c = { .p = ex };
    double v = a_cexpr(&c);
    a_cskip(&c);
    if (c.divzero) return 2;
    if (c.err || *c.p != 0) return 0;                 // junk left over -> not a clean expression
    *out = v;
    return 1;
}

// Format a result: integer when it lands on a whole number, else trimmed %g (6 cifre significative).
// E' la formattazione di PRECISIONE: conversioni/unita'/geometria/fisica la usano (i decimali contano,
// es. "1 atm = 1.01325 bar", "3 miglia = 4828.03 m") — ⊂ il limite "fino a 10 cifre".
void a_fmt_num(double v, char *out, size_t n)
{
    if (isfinite(v) && fabs(v) < 1e15 && fabs(v - (double)llround(v)) < 1e-9)
        snprintf(out, n, "%lld", (long long)llround(v));
    else
        snprintf(out, n, "%.6g", v);
}

// Come a_fmt_num ma ARROTONDA a MAX 4 decimali (zeri finali tagliati): per i risultati del CALCOLO
// "normale" (aritmetica, medie) dove non servono code lunghe ("5 diviso 3 = 1.6667", non 1.66667).
void a_fmt_round(double v, char *out, size_t n)
{
    if (isfinite(v) && fabs(v) < 1e15 && fabs(v - (double)llround(v)) < 1e-9) {
        snprintf(out, n, "%lld", (long long)llround(v)); return;
    }
    if (!isfinite(v)) { snprintf(out, n, "%g", v); return; }
    char buf[48]; snprintf(buf, sizeof buf, "%.4f", v);
    int L = (int)strlen(buf);
    while (L > 0 && buf[L - 1] == '0') buf[--L] = 0;
    if (L > 0 && buf[L - 1] == '.') buf[--L] = 0;
    buf[L] = 0;
    snprintf(out, n, "%s", buf);
}

// ============================================================================
// Deterministic SOLVER — real problem solving with ZERO model: exact arithmetic
// over recognised quantities. Unit conversions, percentages, powers/roots and
// Ohm's law (V/I/R/P). Bilingual. Fires only when the input really is one of
// these; otherwise declines and the query falls through to calc/retrieval.
// ============================================================================
typedef struct { bool isnum; double val; char w[16]; } a_sitem_t;

// Lowercase/de-accent to ASCII, decimal comma->dot, keep '%' and '/' (so compound units like
// "km/h", "m/s", "kg/m3" survive as one token), map superscripts ²->'2' ³->'3' (so "m/s²"->"m/s2"
// and "cm²" stay meaningful), else -> space.
static void a_norm_solve(const char *raw, char *out, size_t cap)
{
    int o = 0;
    for (const unsigned char *p = (const unsigned char *)raw; *p && o < (int)cap - 1; p++) {
        unsigned char c = *p; char ch;
        if (c == 0xC3 && p[1]) {
            unsigned char d = *++p;
            if      (d >= 0xA0 && d <= 0xA2) ch = 'a';
            else if (d >= 0xA8 && d <= 0xAA) ch = 'e';
            else if (d >= 0xAC && d <= 0xAE) ch = 'i';
            else if (d >= 0xB2 && d <= 0xB4) ch = 'o';
            else if (d >= 0xB9 && d <= 0xBB) ch = 'u';
            else ch = ' ';
        } else if (c == 0xC2 && p[1]) {                  // Latin-1 supplement: ² (0xB2) ³ (0xB3)
            unsigned char d = *++p;
            if      (d == 0xB2) ch = '2';
            else if (d == 0xB3) ch = '3';
            else ch = ' ';
        } else if (isalpha(c) || isdigit(c)) ch = (char)tolower(c);
        else if (c == '.' || c == ',')                   // decimal separator only BETWEEN digits;
            ch = (o > 0 && isdigit((unsigned char)out[o-1])    // a sentence comma/period ("secondi, quanto")
                  && isdigit((unsigned char)p[1])) ? '.' : ' ';  // is a separator, not part of the word
        else if (c == '%')                   ch = '%';
        else if (c == '/')                   ch = '/';
        else if (c == '=')                                      // "=SOMMA(A1:A10)" paste -> '=' before a letter is
            ch = isalpha((unsigned char)p[1]) ? ' ' : '=';      // a formula prefix (separator); else kept ("1 spanna = 22")
        else if (c == '-' && o > 0 &&                           // a hyphen JOINING a name and a number
                 ((isalpha((unsigned char)out[o-1]) && isdigit((unsigned char)p[1])) ||
                  (isdigit((unsigned char)out[o-1]) && isalpha((unsigned char)p[1]))))
            continue;   // "Carbon-14", "Zorbium-88", "Floonk-14" are entity/model names — fuse so the math
                        // skills don't scrape the trailing number and compute "half of 14 = 7" on nonsense.
        else                                  ch = ' ';
        out[o++] = ch;
    }
    out[o] = 0;
}

// Split into items: numbers and words ('%' becomes the word "pct").
static int a_items(const char *s, a_sitem_t *it, int maxn)
{
    int n = 0;
    for (const char *p = s; *p && n < maxn; ) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '%') { it[n].isnum = false; it[n].val = 0; snprintf(it[n].w, sizeof(it[n].w), "pct"); n++; p++; continue; }
        if (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))) {
            char *end; double v = strtod(p, &end);
            if (end == p) { p++; continue; }
            it[n].isnum = true; it[n].val = v; it[n].w[0] = 0; n++; p = end;
        } else {
            int l = 0; while (*p && *p != ' ' && *p != '%') { if (l < 15) it[n].w[l++] = *p; p++; }
            it[n].w[l] = 0; it[n].isnum = false; it[n].val = 0; n++;
        }
    }
    return n;
}

enum { DIM_NONE = 0, DIM_LEN, DIM_MASS, DIM_DATA, DIM_TIME, DIM_TEMP };

// Recognise a unit word -> dimension; sets *factor (to base) or *tcode (1=C,2=F,3=K).
static int a_unit(const char *w, double *factor, int *tcode)
{
    static const struct { const char *u; int dim; double f; } T[] = {
        {"mm",DIM_LEN,0.001},{"cm",DIM_LEN,0.01},{"dm",DIM_LEN,0.1},{"m",DIM_LEN,1},{"metro",DIM_LEN,1},
        {"metri",DIM_LEN,1},{"meter",DIM_LEN,1},{"meters",DIM_LEN,1},{"km",DIM_LEN,1000},{"chilometro",DIM_LEN,1000},
        {"chilometri",DIM_LEN,1000},{"kilometer",DIM_LEN,1000},{"inch",DIM_LEN,0.0254},{"pollice",DIM_LEN,0.0254},
        {"pollici",DIM_LEN,0.0254},{"ft",DIM_LEN,0.3048},{"feet",DIM_LEN,0.3048},{"piede",DIM_LEN,0.3048},
        {"piedi",DIM_LEN,0.3048},{"mile",DIM_LEN,1609.34},{"miles",DIM_LEN,1609.34},{"miglio",DIM_LEN,1609.34},{"miglia",DIM_LEN,1609.34},
        {"mg",DIM_MASS,0.001},{"g",DIM_MASS,1},{"grammo",DIM_MASS,1},{"grammi",DIM_MASS,1},{"gram",DIM_MASS,1},{"grams",DIM_MASS,1},
        {"kg",DIM_MASS,1000},{"chilo",DIM_MASS,1000},{"chili",DIM_MASS,1000},{"kilogrammo",DIM_MASS,1000},
        {"tonnellata",DIM_MASS,1e6},{"ton",DIM_MASS,1e6},{"lb",DIM_MASS,453.592},{"libbra",DIM_MASS,453.592},
        {"pound",DIM_MASS,453.592},{"oz",DIM_MASS,28.3495},{"oncia",DIM_MASS,28.3495},
        {"bit",DIM_DATA,0.125},{"byte",DIM_DATA,1},{"kb",DIM_DATA,1024},{"kilobyte",DIM_DATA,1024},
        {"mb",DIM_DATA,1048576.0},{"megabyte",DIM_DATA,1048576.0},{"gb",DIM_DATA,1073741824.0},
        {"gigabyte",DIM_DATA,1073741824.0},{"tb",DIM_DATA,1099511627776.0},
        {"sec",DIM_TIME,1},{"secondo",DIM_TIME,1},{"secondi",DIM_TIME,1},{"second",DIM_TIME,1},{"seconds",DIM_TIME,1},
        {"min",DIM_TIME,60},{"minuto",DIM_TIME,60},{"minuti",DIM_TIME,60},{"minute",DIM_TIME,60},{"minutes",DIM_TIME,60},
        {"ora",DIM_TIME,3600},{"ore",DIM_TIME,3600},{"hour",DIM_TIME,3600},{"hours",DIM_TIME,3600},
        {"giorno",DIM_TIME,86400},{"giorni",DIM_TIME,86400},{"day",DIM_TIME,86400},{"days",DIM_TIME,86400},
    };
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++)
        if (!strcmp(w, T[i].u)) { if (factor) *factor = T[i].f; return T[i].dim; }
    if (!strcmp(w,"c")||!strcmp(w,"celsius")||!strcmp(w,"centigradi")) { if (tcode) *tcode = 1; return DIM_TEMP; }
    if (!strcmp(w,"f")||!strcmp(w,"fahrenheit"))                       { if (tcode) *tcode = 2; return DIM_TEMP; }
    if (!strcmp(w,"k")||!strcmp(w,"kelvin"))                           { if (tcode) *tcode = 3; return DIM_TEMP; }
    return DIM_NONE;
}
static double a_temp_to_K(double v, int code)  { return code == 1 ? v + 273.15 : code == 2 ? (v - 32) * 5.0 / 9.0 + 273.15 : v; }
static double a_temp_from_K(double k, int code){ return code == 1 ? k - 273.15 : code == 2 ? (k - 273.15) * 9.0 / 5.0 + 32 : k; }

// ============================================================================
// UNIT ENGINE — dimensional-analysis converter. Each unit carries (factor to the
// coherent SI unit, dimension signature over L,M,T,Θ,Data,Angle). A→B is exactly
// value·fA/fB IFF the signatures match; a dimension MISMATCH ("5 kg in litri") is
// REFUSED with an explanation (mass ≠ volume), plus a water bridge when sensible.
// Handles SI prefixes (kilo/mega/giga/milli/micro… = powers of ten), powers of
// ten ("10 alla 9"), and LEARNS user-defined units ("1 spanna = 22 cm") into a
// small persistent registry. Allocation-free. Mirrored by jUnits in the twin.
// ============================================================================
static bool a_is_conn(const char *w);                         // defined with the physics helpers below
typedef struct { signed char L,M,T,K,D,A,I; } u_dim;          // Length,Mass,Time,Temp,Data,Angle,electric-Current
static bool u_eq(u_dim a, u_dim b){ return a.L==b.L&&a.M==b.M&&a.T==b.T&&a.K==b.K&&a.D==b.D&&a.A==b.A&&a.I==b.I; }

#define U_LEARN_MAX 10
typedef struct { char name[16]; double f; u_dim d; bool used; } u_learned;
static u_learned g_ulearn[U_LEARN_MAX];                        // tiny static registry (no heap)

// Affine temperature unit -> 1=C 2=F 3=K, else 0 (handled outside the linear table).
static int u_temp_code(const char *w){
    if(!strcmp(w,"c")||!strcmp(w,"celsius")||!strcmp(w,"centigradi")||!strcmp(w,"centigrado")) return 1;
    if(!strcmp(w,"f")||!strcmp(w,"fahrenheit")) return 2;
    if(!strcmp(w,"k")||!strcmp(w,"kelvin")) return 3;
    return 0;
}

typedef struct { const char *u; double f; signed char L,M,T,K,D,A,I; } u_row;
static const u_row U_TAB[] = {
  // length (SI m)
  {"nm",1e-9,1,0,0,0,0,0,0},{"um",1e-6,1,0,0,0,0,0,0},{"micrometro",1e-6,1,0,0,0,0,0,0},
  {"mm",1e-3,1,0,0,0,0,0,0},{"millimetro",1e-3,1,0,0,0,0,0,0},{"millimetri",1e-3,1,0,0,0,0,0,0},
  {"cm",1e-2,1,0,0,0,0,0,0},{"centimetro",1e-2,1,0,0,0,0,0,0},{"centimetri",1e-2,1,0,0,0,0,0,0},
  {"dm",0.1,1,0,0,0,0,0,0},{"decimetro",0.1,1,0,0,0,0,0,0},{"decimetri",0.1,1,0,0,0,0,0,0},
  {"m",1,1,0,0,0,0,0,0},{"metro",1,1,0,0,0,0,0,0},{"metri",1,1,0,0,0,0,0,0},{"meter",1,1,0,0,0,0,0,0},{"meters",1,1,0,0,0,0,0,0},
  {"dam",10,1,0,0,0,0,0,0},{"hm",100,1,0,0,0,0,0,0},
  {"km",1000,1,0,0,0,0,0,0},{"chilometro",1000,1,0,0,0,0,0,0},{"chilometri",1000,1,0,0,0,0,0,0},{"kilometer",1000,1,0,0,0,0,0,0},{"kilometers",1000,1,0,0,0,0,0,0},
  {"pollice",0.0254,1,0,0,0,0,0,0},{"pollici",0.0254,1,0,0,0,0,0,0},{"inch",0.0254,1,0,0,0,0,0,0},{"inches",0.0254,1,0,0,0,0,0,0},
  {"piede",0.3048,1,0,0,0,0,0,0},{"piedi",0.3048,1,0,0,0,0,0,0},{"ft",0.3048,1,0,0,0,0,0,0},{"feet",0.3048,1,0,0,0,0,0,0},{"foot",0.3048,1,0,0,0,0,0,0},
  {"iarda",0.9144,1,0,0,0,0,0,0},{"iarde",0.9144,1,0,0,0,0,0,0},{"yard",0.9144,1,0,0,0,0,0,0},{"yards",0.9144,1,0,0,0,0,0,0},
  {"miglio",1609.344,1,0,0,0,0,0,0},{"miglia",1609.344,1,0,0,0,0,0,0},{"mile",1609.344,1,0,0,0,0,0,0},{"miles",1609.344,1,0,0,0,0,0,0},
  // mass (SI kg)
  {"mg",1e-6,0,1,0,0,0,0,0},{"milligrammo",1e-6,0,1,0,0,0,0,0},{"milligrammi",1e-6,0,1,0,0,0,0,0},
  {"g",1e-3,0,1,0,0,0,0,0},{"grammo",1e-3,0,1,0,0,0,0,0},{"grammi",1e-3,0,1,0,0,0,0,0},{"gram",1e-3,0,1,0,0,0,0,0},{"grams",1e-3,0,1,0,0,0,0,0},
  {"hg",0.1,0,1,0,0,0,0,0},{"etto",0.1,0,1,0,0,0,0,0},{"etti",0.1,0,1,0,0,0,0,0},
  {"kg",1,0,1,0,0,0,0,0},{"chilo",1,0,1,0,0,0,0,0},{"chili",1,0,1,0,0,0,0,0},{"chilogrammo",1,0,1,0,0,0,0,0},{"chilogrammi",1,0,1,0,0,0,0,0},{"kilogram",1,0,1,0,0,0,0,0},{"kilograms",1,0,1,0,0,0,0,0},
  {"q",100,0,1,0,0,0,0,0},{"quintale",100,0,1,0,0,0,0,0},{"quintali",100,0,1,0,0,0,0,0},
  {"t",1000,0,1,0,0,0,0,0},{"tonnellata",1000,0,1,0,0,0,0,0},{"tonnellate",1000,0,1,0,0,0,0,0},{"ton",1000,0,1,0,0,0,0,0},{"tonne",1000,0,1,0,0,0,0,0},
  {"oncia",0.0283495,0,1,0,0,0,0,0},{"once",0.0283495,0,1,0,0,0,0,0},{"oz",0.0283495,0,1,0,0,0,0,0},{"ounce",0.0283495,0,1,0,0,0,0,0},{"ounces",0.0283495,0,1,0,0,0,0,0},
  {"libbra",0.453592,0,1,0,0,0,0,0},{"libbre",0.453592,0,1,0,0,0,0,0},{"lb",0.453592,0,1,0,0,0,0,0},{"lbs",0.453592,0,1,0,0,0,0,0},{"pound",0.453592,0,1,0,0,0,0,0},{"pounds",0.453592,0,1,0,0,0,0,0},
  // time (SI s)
  {"ms",1e-3,0,0,1,0,0,0,0},{"millisecondo",1e-3,0,0,1,0,0,0,0},{"millisecondi",1e-3,0,0,1,0,0,0,0},
  {"s",1,0,0,1,0,0,0,0},{"sec",1,0,0,1,0,0,0,0},{"secondo",1,0,0,1,0,0,0,0},{"secondi",1,0,0,1,0,0,0,0},{"second",1,0,0,1,0,0,0,0},{"seconds",1,0,0,1,0,0,0,0},
  {"min",60,0,0,1,0,0,0,0},{"minuto",60,0,0,1,0,0,0,0},{"minuti",60,0,0,1,0,0,0,0},{"minute",60,0,0,1,0,0,0,0},{"minutes",60,0,0,1,0,0,0,0},
  {"h",3600,0,0,1,0,0,0,0},{"ora",3600,0,0,1,0,0,0,0},{"ore",3600,0,0,1,0,0,0,0},{"hour",3600,0,0,1,0,0,0,0},{"hours",3600,0,0,1,0,0,0,0},
  {"giorno",86400,0,0,1,0,0,0,0},{"giorni",86400,0,0,1,0,0,0,0},{"day",86400,0,0,1,0,0,0,0},{"days",86400,0,0,1,0,0,0,0},
  {"settimana",604800,0,0,1,0,0,0,0},{"settimane",604800,0,0,1,0,0,0,0},{"week",604800,0,0,1,0,0,0,0},{"weeks",604800,0,0,1,0,0,0,0},
  {"mese",2592000,0,0,1,0,0,0,0},{"mesi",2592000,0,0,1,0,0,0,0},{"month",2592000,0,0,1,0,0,0,0},{"months",2592000,0,0,1,0,0,0,0},
  {"anno",31536000,0,0,1,0,0,0,0},{"anni",31536000,0,0,1,0,0,0,0},{"year",31536000,0,0,1,0,0,0,0},{"years",31536000,0,0,1,0,0,0,0},
  // volume (SI m³)
  {"ml",1e-6,3,0,0,0,0,0,0},{"millilitro",1e-6,3,0,0,0,0,0,0},{"millilitri",1e-6,3,0,0,0,0,0,0},{"milliliter",1e-6,3,0,0,0,0,0,0},{"milliliters",1e-6,3,0,0,0,0,0,0},{"millilitre",1e-6,3,0,0,0,0,0,0},
  {"cl",1e-5,3,0,0,0,0,0,0},{"dl",1e-4,3,0,0,0,0,0,0},
  {"l",1e-3,3,0,0,0,0,0,0},{"litro",1e-3,3,0,0,0,0,0,0},{"litri",1e-3,3,0,0,0,0,0,0},{"liter",1e-3,3,0,0,0,0,0,0},{"litre",1e-3,3,0,0,0,0,0,0},{"liters",1e-3,3,0,0,0,0,0,0},{"litres",1e-3,3,0,0,0,0,0,0},
  {"hl",0.1,3,0,0,0,0,0,0},{"m3",1,3,0,0,0,0,0,0},{"dm3",1e-3,3,0,0,0,0,0,0},{"cm3",1e-6,3,0,0,0,0,0,0},
  {"gallone",0.00378541,3,0,0,0,0,0,0},{"galloni",0.00378541,3,0,0,0,0,0,0},{"gallon",0.00378541,3,0,0,0,0,0,0},{"gallons",0.00378541,3,0,0,0,0,0,0},{"gal",0.00378541,3,0,0,0,0,0,0},
  {"pinta",0.000473176,3,0,0,0,0,0,0},{"pint",0.000473176,3,0,0,0,0,0,0},
  {"tazza",0.000236588,3,0,0,0,0,0,0},{"tazze",0.000236588,3,0,0,0,0,0,0},{"cup",0.000236588,3,0,0,0,0,0,0},{"cups",0.000236588,3,0,0,0,0,0,0},
  {"cucchiaio",1.4787e-5,3,0,0,0,0,0,0},{"tablespoon",1.4787e-5,3,0,0,0,0,0,0},
  // area (SI m²)
  {"mm2",1e-6,2,0,0,0,0,0,0},{"cm2",1e-4,2,0,0,0,0,0,0},{"dm2",1e-2,2,0,0,0,0,0,0},{"m2",1,2,0,0,0,0,0,0},{"mq",1,2,0,0,0,0,0,0},{"km2",1e6,2,0,0,0,0,0,0},
  {"ara",100,2,0,0,0,0,0,0},{"ettaro",1e4,2,0,0,0,0,0,0},{"ettari",1e4,2,0,0,0,0,0,0},{"ha",1e4,2,0,0,0,0,0,0},
  {"acro",4046.86,2,0,0,0,0,0,0},{"acri",4046.86,2,0,0,0,0,0,0},{"acre",4046.86,2,0,0,0,0,0,0},{"acres",4046.86,2,0,0,0,0,0,0},
  // speed (SI m/s)
  {"m/s",1,1,0,-1,0,0,0,0},{"km/h",1.0/3.6,1,0,-1,0,0,0,0},{"kmh",1.0/3.6,1,0,-1,0,0,0,0},{"kph",1.0/3.6,1,0,-1,0,0,0,0},{"mph",0.44704,1,0,-1,0,0,0,0},
  {"nodo",0.514444,1,0,-1,0,0,0,0},{"nodi",0.514444,1,0,-1,0,0,0,0},{"knot",0.514444,1,0,-1,0,0,0,0},{"knots",0.514444,1,0,-1,0,0,0,0},
  // force (SI N)
  {"n",1,1,1,-2,0,0,0,0},{"newton",1,1,1,-2,0,0,0,0},{"kn",1000,1,1,-2,0,0,0,0},{"dyn",1e-5,1,1,-2,0,0,0,0},{"kgf",9.80665,1,1,-2,0,0,0,0},
  // energy (SI J)
  {"j",1,2,1,-2,0,0,0,0},{"joule",1,2,1,-2,0,0,0,0},{"kj",1000,2,1,-2,0,0,0,0},{"mj",1e6,2,1,-2,0,0,0,0},
  {"cal",4.184,2,1,-2,0,0,0,0},{"caloria",4.184,2,1,-2,0,0,0,0},{"calorie",4.184,2,1,-2,0,0,0,0},{"kcal",4184,2,1,-2,0,0,0,0},
  {"wh",3600,2,1,-2,0,0,0,0},{"kwh",3.6e6,2,1,-2,0,0,0,0},{"btu",1055.06,2,1,-2,0,0,0,0},
  // power (SI W)
  {"w",1,2,1,-3,0,0,0,0},{"watt",1,2,1,-3,0,0,0,0},{"kw",1000,2,1,-3,0,0,0,0},{"mw",1e6,2,1,-3,0,0,0,0},{"gw",1e9,2,1,-3,0,0,0,0},
  {"cv",735.49875,2,1,-3,0,0,0,0},{"hp",745.699,2,1,-3,0,0,0,0},
  {"watts",1,2,1,-3,0,0,0,0},{"kilowatt",1000,2,1,-3,0,0,0,0},{"kilowatts",1000,2,1,-3,0,0,0,0},
  {"cavallo",735.49875,2,1,-3,0,0,0,0},{"cavalli",735.49875,2,1,-3,0,0,0,0},{"horsepower",745.699,2,1,-3,0,0,0,0},
  // pressure (SI Pa)
  {"pa",1,-1,1,-2,0,0,0,0},{"pascal",1,-1,1,-2,0,0,0,0},{"hpa",100,-1,1,-2,0,0,0,0},{"kpa",1000,-1,1,-2,0,0,0,0},{"mpa",1e6,-1,1,-2,0,0,0,0},
  {"bar",1e5,-1,1,-2,0,0,0,0},{"mbar",100,-1,1,-2,0,0,0,0},{"atm",101325,-1,1,-2,0,0,0,0},{"psi",6894.76,-1,1,-2,0,0,0,0},{"mmhg",133.322,-1,1,-2,0,0,0,0},{"torr",133.322,-1,1,-2,0,0,0,0},
  {"atmosfera",101325,-1,1,-2,0,0,0,0},{"atmosfere",101325,-1,1,-2,0,0,0,0},{"atmosphere",101325,-1,1,-2,0,0,0,0},{"atmospheres",101325,-1,1,-2,0,0,0,0},
  // frequency (SI Hz)
  {"hz",1,0,0,-1,0,0,0,0},{"hertz",1,0,0,-1,0,0,0,0},{"khz",1000,0,0,-1,0,0,0,0},{"mhz",1e6,0,0,-1,0,0,0,0},{"ghz",1e9,0,0,-1,0,0,0,0},
  // data (base byte)
  {"bit",0.125,0,0,0,0,1,0,0},{"bits",0.125,0,0,0,0,1,0,0},{"byte",1,0,0,0,0,1,0,0},{"bytes",1,0,0,0,0,1,0,0},
  {"kb",1024,0,0,0,0,1,0,0},{"kib",1024,0,0,0,0,1,0,0},{"mb",1048576,0,0,0,0,1,0,0},{"mib",1048576,0,0,0,0,1,0,0},
  {"gb",1073741824.0,0,0,0,0,1,0,0},{"gib",1073741824.0,0,0,0,0,1,0,0},{"tb",1.099511627776e12,0,0,0,0,1,0,0},{"pb",1.125899906842624e15,0,0,0,0,1,0,0},
  // angle (base degree)
  {"grado",1,0,0,0,0,0,1,0},{"gradi",1,0,0,0,0,0,1,0},{"degree",1,0,0,0,0,0,1,0},{"degrees",1,0,0,0,0,0,1,0},{"deg",1,0,0,0,0,0,1,0},
  {"rad",57.29577951308232,0,0,0,0,0,1,0},{"radiante",57.29577951308232,0,0,0,0,0,1,0},{"radianti",57.29577951308232,0,0,0,0,0,1,0},{"radian",57.29577951308232,0,0,0,0,0,1,0},
  {"giro",360,0,0,0,0,0,1,0},{"giri",360,0,0,0,0,0,1,0},{"turn",360,0,0,0,0,0,1,0},
  // electric current/voltage/resistance/charge (SI A/V/Ω/C) — the converter does the SCALING,
  // Ohm's law (a_solve_ohm) keeps the relationships; they collaborate (ohm needs >=2 quantities,
  // so single-unit conversions like "2 ampere in milliampere" fall through to here). Prefixed
  // forms (milliampere, kilovolt, megaohm) resolve via the spelled-prefix path in u_resolve.
  {"ampere",1,0,0,0,0,0,0,1},{"amp",1,0,0,0,0,0,0,1},{"amps",1,0,0,0,0,0,0,1},
  {"volt",1,2,1,-3,0,0,0,-1},{"volts",1,2,1,-3,0,0,0,-1},
  {"ohm",1,2,1,-3,0,0,0,-2},{"ohms",1,2,1,-3,0,0,0,-2},{"kohm",1000,2,1,-3,0,0,0,-2},
  {"coulomb",1,0,0,1,0,0,0,1},
};

// table-only lookup (no learned/prefix recursion)
static bool u_base(const char *w, double *f, u_dim *d){
    for(size_t i=0;i<sizeof(U_TAB)/sizeof(U_TAB[0]);i++) if(!strcmp(w,U_TAB[i].u)){
        *f=U_TAB[i].f; d->L=U_TAB[i].L; d->M=U_TAB[i].M; d->T=U_TAB[i].T; d->K=U_TAB[i].K; d->D=U_TAB[i].D; d->A=U_TAB[i].A; d->I=U_TAB[i].I; return true; }
    return false;
}
// full resolve: learned registry, then table, then a spelled SI prefix on a known base ("megajoule").
static bool u_name_match(const char *a, const char *b){    // exact, or a singular/plural form (spanna~spanne, pertica~pertiche)
    if(!strcmp(a,b)) return true;
    size_t la=strlen(a), lb=strlen(b), mn=la<lb?la:lb, cp=0;
    if(mn<4) return false;
    while(cp<mn && a[cp]==b[cp]) cp++;                       // share all but (at most) the last letter of the shorter
    return cp>=mn-1;
}
static bool u_resolve(const char *w, double *f, u_dim *d){
    for(int i=0;i<U_LEARN_MAX;i++) if(g_ulearn[i].used && u_name_match(g_ulearn[i].name,w)){ *f=g_ulearn[i].f; *d=g_ulearn[i].d; return true; }
    if(u_base(w,f,d)) return true;
    static const struct{ const char*p; double e; } PFX[] = {
        {"chilo",1e3},{"kilo",1e3},{"mega",1e6},{"giga",1e9},{"tera",1e12},{"peta",1e15},
        {"milli",1e-3},{"micro",1e-6},{"nano",1e-9},{"centi",1e-2},{"deci",1e-1},{"deca",1e1},{"etto",1e2},{"hecto",1e2} };
    for(size_t i=0;i<sizeof(PFX)/sizeof(PFX[0]);i++){
        size_t pl=strlen(PFX[i].p);
        if(!strncmp(w,PFX[i].p,pl) && w[pl]){ double bf; u_dim bd;
            if(u_base(w+pl,&bf,&bd)){ *f=bf*PFX[i].e; *d=bd; return true; } }
    }
    return false;
}

void units_load(void){
    FILE *fp = fopen(UNITS_PATH, "rb"); if(!fp) return;
    char line[96];
    while(fgets(line,sizeof line,fp)){
        char nm[16]; double f; int L,M,T,K,D,A,I;
        if(sscanf(line,"%15s %lf %d %d %d %d %d %d %d",nm,&f,&L,&M,&T,&K,&D,&A,&I)==9){
            int slot=-1;
            for(int i=0;i<U_LEARN_MAX;i++){ if(g_ulearn[i].used && !strcmp(g_ulearn[i].name,nm)){slot=i;break;} if(slot<0&&!g_ulearn[i].used) slot=i; }
            if(slot>=0){ snprintf(g_ulearn[slot].name,sizeof g_ulearn[slot].name,"%s",nm); g_ulearn[slot].f=f;
                g_ulearn[slot].d=(u_dim){(signed char)L,(signed char)M,(signed char)T,(signed char)K,(signed char)D,(signed char)A,(signed char)I}; g_ulearn[slot].used=true; } }
    }
    fclose(fp);
}
// learn (or update) a unit, then append it to the persistent store.
static void u_learn(const char *name, double f, u_dim d){
    int slot=-1;
    for(int i=0;i<U_LEARN_MAX;i++){ if(g_ulearn[i].used && !strcmp(g_ulearn[i].name,name)){slot=i;break;} if(slot<0&&!g_ulearn[i].used) slot=i; }
    if(slot<0) slot=0;                                          // registry full -> recycle slot 0
    snprintf(g_ulearn[slot].name,sizeof g_ulearn[slot].name,"%s",name); g_ulearn[slot].f=f; g_ulearn[slot].d=d; g_ulearn[slot].used=true;
    FILE *fp=fopen(UNITS_PATH,"ab");                            // best-effort persist (SD may be absent)
    if(fp){ fprintf(fp,"%s %.10g %d %d %d %d %d %d %d\n",name,f,d.L,d.M,d.T,d.K,d.D,d.A,d.I); fclose(fp); }
}

// Human name for a dimension signature (for the mismatch explanation).
static const char *u_dim_name(u_dim d, bool en){
    static const struct { u_dim d; const char *it; const char *en; } N[] = {
        {{1,0,0,0,0,0,0},"lunghezza","length"},   {{0,1,0,0,0,0,0},"massa","mass"},
        {{0,0,1,0,0,0,0},"tempo","time"},          {{0,0,0,1,0,0,0},"temperatura","temperature"},
        {{0,0,0,0,1,0,0},"dati","data"},           {{0,0,0,0,0,1,0},"angolo","angle"},
        {{2,0,0,0,0,0,0},"area","area"},           {{3,0,0,0,0,0,0},"volume","volume"},
        {{1,0,-1,0,0,0,0},"velocita","speed"},     {{1,1,-2,0,0,0,0},"forza","force"},
        {{2,1,-2,0,0,0,0},"energia","energy"},     {{2,1,-3,0,0,0,0},"potenza","power"},
        {{-1,1,-2,0,0,0,0},"pressione","pressure"},{{0,0,-1,0,0,0,0},"frequenza","frequency"},
        {{0,0,0,0,0,0,1},"corrente","current"},{{2,1,-3,0,0,0,-1},"tensione","voltage"},
        {{2,1,-3,0,0,0,-2},"resistenza","resistance"},{{0,0,1,0,0,0,1},"carica","charge"},
    };
    for(size_t i=0;i<sizeof(N)/sizeof(N[0]);i++) if(u_eq(d,N[i].d)) return en?N[i].en:N[i].it;
    return en?"quantity":"grandezza";
}
// Words that can't be a learned unit's NAME (cues / articles / connectors).
static bool u_skipword(const char *w){
    static const char *const S[] = {"di","of","con","with","del","dello","della","come","as","un","uno","una",
        "the","a","e","ed","and","in","to","è","sono","vale","valgono","uguale","equivale","equivalgono","equals",
        "corrisponde","corrispondono","definisci","define","impara","learn","memorizza","converti","convert",
        "trasforma","quanti","quante","quanto","fa","ce","ci","ad",NULL};
    for(int i=0;S[i];i++) if(!strcmp(w,S[i])) return true;
    return false;
}

// The UNIT solver: a definition ("1 spanna = 22 cm"), a power of ten ("10 alla 9"), or a
// conversion ("5 km in cm", "20 C in F", "2 GB in MB"). Dimension mismatch -> honest refusal.
static bool a_solve_units(const a_sitem_t *it, int n, bool en, anima_result_t *r)
{
    char b1[40],b2[40];
#define U_OK(intent_,...) do{ r->tier=ANIMA_TIER_COMMAND; r->action=ANIMA_ACT_ANSWER; r->confidence=96; \
    snprintf(r->intent,sizeof r->intent,"%s",intent_); snprintf(r->state,sizeof r->state,"tool"); \
    snprintf(r->reply,sizeof r->reply,__VA_ARGS__); return true; }while(0)

    // ---- cue scan ----
    bool cue=false, learnCue=false, equivCue=false, hasEq=false, rangeWord=false; int firstNum=-1;
    for(int i=0;i<n;i++){
        if(it[i].isnum){ if(firstNum<0) firstNum=i; continue; }
        const char*w=it[i].w;
        if(!strcmp(w,"in")||!strcmp(w,"to")||!strcmp(w,"into")||!strcmp(w,"converti")||   // not bare "a" ("5 km a piedi" is an idiom, not a conversion)
           !strcmp(w,"convert")||!strcmp(w,"trasforma")||!strcmp(w,"trasformare")||!strcmp(w,"quanti")||!strcmp(w,"quante")||
           !strcmp(w,"quanto")||!strcmp(w,"corrisponde")||!strcmp(w,"corrispondono")||!strcmp(w,"vale")||!strcmp(w,"valgono")||
           !strcmp(w,"sono")||!strcmp(w,"many")) cue=true;   // "many" -> EN "how many X is Y"
        if(!strcmp(w,"definisci")||!strcmp(w,"define")||!strcmp(w,"impara")||!strcmp(w,"learn")||!strcmp(w,"memorizza")) learnCue=true;
        if(!strcmp(w,"equivale")||!strcmp(w,"equivalgono")||!strcmp(w,"equals")||!strcmp(w,"corrisponde")||!strcmp(w,"corrispondono")) equivCue=true;
        if(!strcmp(w,"=")) hasEq=true;
        if(!strcmp(w,"da")||!strcmp(w,"dal")||!strcmp(w,"dalla")||!strcmp(w,"dalle")||!strcmp(w,"dallo")||
           !strcmp(w,"tra")||!strcmp(w,"fra")||!strcmp(w,"between")) rangeWord=true;
    }

    // ---- power of ten: "10 alla 9", "2 alla 8" (NOT a range "dalla 2 alla 5"; "elevato" stays with powroot) ----
    if(!rangeWord) for(int i=1;i+1<n;i++){
        if(!it[i-1].isnum || it[i].isnum || !it[i+1].isnum) continue;
        if(!strcmp(it[i].w,"alla")){
            double base=it[i-1].val, ex=it[i+1].val, res=pow(base,ex);
            a_fmt_num(base,b1,40); a_fmt_num(res,b2,40);
            U_OK("calc","%s^%g = %s.", b1, ex, b2);
        }
    }

    // "gradi" is ANGLE by default but TEMPERATURE next to celsius/fahrenheit/kelvin ("20 gradi in fahrenheit").
    bool hasTempUnit=false;
    for(int i=0;i<n;i++) if(!it[i].isnum){ const char*w=it[i].w;
        if(!strcmp(w,"celsius")||!strcmp(w,"centigradi")||!strcmp(w,"centigrado")||!strcmp(w,"fahrenheit")||!strcmp(w,"kelvin")) hasTempUnit=true; }

    // ---- collect units (linear + affine temperature), with their item index & token ----
    struct { int pos; const char *w; double f; u_dim d; int temp; } U[8]; int nu=0;
    for(int i=0;i<n && nu<8;i++){
        if(it[i].isnum) continue;
        const char *w=it[i].w; double f; u_dim d;
        bool isDeg = !strcmp(w,"gradi")||!strcmp(w,"grado")||!strcmp(w,"degree")||!strcmp(w,"degrees")||!strcmp(w,"deg");
        if(isDeg && i+1<n && !it[i+1].isnum && u_temp_code(it[i+1].w)) continue;   // "gradi celsius" -> celsius is the unit
        if(isDeg && hasTempUnit){ U[nu].pos=i; U[nu].w=w; U[nu].f=0; U[nu].d=(u_dim){0,0,0,1,0,0,0}; U[nu].temp=1; nu++; continue; } // degrees -> Celsius
        int tc=u_temp_code(w);
        if(tc){ U[nu].pos=i; U[nu].w=w; U[nu].f=0; U[nu].d=(u_dim){0,0,0,1,0,0,0}; U[nu].temp=tc; nu++; }
        else if(u_resolve(w,&f,&d)){ U[nu].pos=i; U[nu].w=w; U[nu].f=f; U[nu].d=d; U[nu].temp=0; nu++; }
    }

    // ---- DEFINITION: a learn/equivalence cue + a NEW name + an RHS (value adjacent to a known unit) ----
    if((learnCue||equivCue||hasEq)){
        int rNum=-1,rUnit=-1; double rf=0; u_dim rd={0};
        for(int i=0;i<n;i++){                                   // last "<value> <known-unit>" pair = the RHS
            if(!it[i].isnum) continue;
            int ui=-1; if(i+1<n && !it[i+1].isnum) ui=i+1;
            else if(i+2<n && !it[i+1].isnum && !it[i+2].isnum && a_is_conn(it[i+1].w)) ui=i+2;
            double f; u_dim d; int tc;
            if(ui>=0 && ((tc=u_temp_code(it[ui].w))!=0 || u_resolve(it[ui].w,&f,&d))){
                if(tc){ /* can't define a unit as a temperature */ }
                else { rNum=i; rUnit=ui; rf=f; rd=d; }
            }
        }
        int nameIdx=-1;
        for(int i=0;i<n;i++){
            if(it[i].isnum || i==rUnit) continue;
            const char *w=it[i].w; double f; u_dim d;
            // explicit "definisci X" may REDEFINE a built-in; "=" alone may not. A LEARNED unit (not in
            // the base table) can always be re-defined, so test u_base (built-ins) not u_resolve here.
            if(u_skipword(w)||u_temp_code(w)||!strcmp(w,"=")||(!learnCue && u_base(w,&f,&d))) continue;
            if(strlen(w)<2) continue;
            nameIdx=i; break;
        }
        if(nameIdx>=0 && rUnit>=0){
            double lhsN = (nameIdx>=1 && it[nameIdx-1].isnum) ? it[nameIdx-1].val : 1.0;
            if(lhsN==0) lhsN=1.0;
            double perName = it[rNum].val*rf/lhsN;               // SI value of ONE <name>
            u_learn(it[nameIdx].w, perName, rd);
            a_fmt_num(it[rNum].val/lhsN, b1, 40);
            U_OK("convert", en?"Learned: 1 %s = %s %s (%s).":"Imparato: 1 %s = %s %s (%s).",
                 it[nameIdx].w, b1, it[rUnit].w, u_dim_name(rd,en));
        }
    }

    // ---- CONVERSION: need a value, >=2 recognised units, and a convert cue ----
    if(firstNum>=0 && nu>=2 && cue){
        int si=-1; for(int k=0;k<nu;k++) if(U[k].pos>firstNum){ si=k; break; }  // source = unit just after the value
        if(si<0) si=0;
        int ti=-1; for(int k=0;k<nu;k++) if(k!=si){ ti=k; break; }              // target = the other unit
        if(ti<0) return false;
        double V=it[firstNum].val; int sp=U[si].pos;            // value = the number adjacent to the SOURCE unit
        if(sp>=1 && it[sp-1].isnum) V=it[sp-1].val;             // ("...12 caratteri... 5 kg in litri" -> 5, not 12)
        else if(sp>=2 && !it[sp-1].isnum && it[sp-2].isnum && a_is_conn(it[sp-1].w)) V=it[sp-2].val;
        if(U[si].temp && U[ti].temp){                            // affine temperature
            double res=a_temp_from_K(a_temp_to_K(V,U[si].temp),U[ti].temp);
            a_fmt_num(V,b1,40); a_fmt_num(res,b2,40);
            U_OK("convert","%s %s = %s %s.", b1, U[si].w, b2, U[ti].w);
        }
        if(U[si].temp || U[ti].temp){                            // temp vs non-temp
            U_OK("convert", en?"Can't convert: %s is %s, %s is %s (different dimensions).":
                                "Non posso convertire: %s è %s, %s è %s (dimensioni diverse).",
                 U[si].w,u_dim_name(U[si].d,en), U[ti].w,u_dim_name(U[ti].d,en));
        }
        if(u_eq(U[si].d,U[ti].d)){                               // same dimension -> exact
            double res=V*U[si].f/U[ti].f;
            a_fmt_num(V,b1,40); a_fmt_num(res,b2,40);
            U_OK("convert","%s %s = %s %s.", b1, U[si].w, b2, U[ti].w);
        }
        // dimension MISMATCH -> honest refusal, with a water bridge when mass<->volume
        {
            u_dim MASS={0,1,0,0,0,0,0}, VOL={3,0,0,0,0,0,0};
            char extra[80]; extra[0]=0;
            if(u_eq(U[si].d,MASS)&&u_eq(U[ti].d,VOL)){ double kg=V*U[si].f; a_fmt_num(kg,b2,40);
                snprintf(extra,sizeof extra,en?" Water: %s kg ≈ %s litres.":" Acqua: %s kg ≈ %s litri.", b2,b2); }
            else if(u_eq(U[si].d,VOL)&&u_eq(U[ti].d,MASS)){ double lt=V*U[si].f/1e-3; a_fmt_num(lt,b2,40);
                snprintf(extra,sizeof extra,en?" Water: %s litres ≈ %s kg.":" Acqua: %s litri ≈ %s kg.", b2,b2); }
            U_OK("convert", en?"Can't convert: %s is %s, %s is %s (different dimensions).%s":
                                "Non posso convertire: %s è %s, %s è %s (dimensioni diverse).%s",
                 U[si].w,u_dim_name(U[si].d,en), U[ti].w,u_dim_name(U[ti].d,en), extra);
        }
    }
#undef U_OK
    return false;
}

// "<num> <unit> in <unit2>" -> exact conversion (same dimension). Temperature is affine.
static bool a_solve_convert(a_sitem_t *it, int n, anima_result_t *r)
{
    double num = 0; bool have = false, cue = false;
    int dim[2] = {DIM_NONE, DIM_NONE}, tc[2] = {0, 0}; double f[2] = {0, 0}; const char *uw[2] = {NULL, NULL}; int u = 0;
    for (int i = 0; i < n; i++) {
        if (it[i].isnum) { if (!have) { num = it[i].val; have = true; } continue; }
        if (!strcmp(it[i].w,"in")||!strcmp(it[i].w,"to")||!strcmp(it[i].w,"converti")||!strcmp(it[i].w,"convert")||
            !strcmp(it[i].w,"trasforma")||!strcmp(it[i].w,"quanti")||!strcmp(it[i].w,"quante")||!strcmp(it[i].w,"corrisponde")) cue = true;
        double ff = 0; int t = 0; int d = a_unit(it[i].w, &ff, &t);
        if (d != DIM_NONE && u < 2) { dim[u] = d; f[u] = ff; tc[u] = t; uw[u] = it[i].w; u++; }
    }
    if (!have || u < 2 || dim[0] != dim[1] || !cue) return false;   // require a convert cue ("5 km a piedi" is an idiom, not a conversion)
    double res = dim[0] == DIM_TEMP ? a_temp_from_K(a_temp_to_K(num, tc[0]), tc[1]) : num * f[0] / f[1];
    char a[40], b[40]; a_fmt_num(num, a, sizeof(a)); a_fmt_num(res, b, sizeof(b));
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 95;
    snprintf(r->intent, sizeof(r->intent), "convert"); snprintf(r->state, sizeof(r->state), "tool");
    snprintf(r->reply, sizeof(r->reply), "%s %s = %s %s.", a, uw[0], b, uw[1]);
    return true;
}

// "X% di Y" / "X percent of Y" -> X/100*Y.
static bool a_solve_percent(a_sitem_t *it, int n, bool en, anima_result_t *r)
{
    bool haspct = false, conn = false; double nums[6]; int nn = 0; double rate = -1;
    for (int i = 0; i < n; i++) {
        if (it[i].isnum) { if (nn < 6) nums[nn] = it[i].val; nn++; }
        else {
            if (!strcmp(it[i].w,"pct") || !strncmp(it[i].w,"percent",7) || !strcmp(it[i].w,"cento")) {
                haspct = true; if (i > 0 && it[i-1].isnum) rate = it[i-1].val;
            }
            if (!strcmp(it[i].w,"di")||!strcmp(it[i].w,"of")||!strcmp(it[i].w,"del")||!strcmp(it[i].w,"dello")||!strcmp(it[i].w,"della")) conn = true;
        }
    }
    if (!haspct || !conn || nn < 2) return false;
    if (rate < 0) rate = nums[0];
    double base = (rate == nums[0]) ? nums[1] : nums[0];
    double res = rate / 100.0 * base;
    char a[40], b[40], cc[40]; a_fmt_num(rate,a,sizeof(a)); a_fmt_num(base,b,sizeof(b)); a_fmt_num(res,cc,sizeof(cc));
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 95;
    snprintf(r->intent, sizeof(r->intent), "percent"); snprintf(r->state, sizeof(r->state), "tool");
    snprintf(r->reply, sizeof(r->reply), en ? "%s%% of %s = %s." : "Il %s%% di %s = %s.", a, b, cc);
    return true;
}

// Italian ordinal used as an exponent ("alla terza" = ^3), or 0. Ranges ("dalla 2 alla 5") use
// plain numbers after "alla", never an ordinal — so keying on the ordinal avoids that collision.
static int a_ordinal_power(const char *w)
{
    static const char *O[] = { "", "", "seconda", "terza", "quarta", "quinta",
                               "sesta", "settima", "ottava", "nona", "decima" };
    for (int i = 2; i <= 10; i++) if (!strcmp(w, O[i])) return i;
    return 0;
}

// "radice di N" / "sqrt N"  and  "X elevato Y" / "X alla terza" -> exact root / power.
static bool a_solve_powroot(a_sitem_t *it, int n, bool en, anima_result_t *r)
{
    bool root = false, poww = false, cube_garble = false; double nums[6]; int nn = 0;
    for (int i = 0; i < n; i++) {
        if (it[i].isnum) { if (nn < 6) nums[nn] = it[i].val; nn++; }
        else {
            if (!strcmp(it[i].w,"radice")||!strcmp(it[i].w,"sqrt")||!strcmp(it[i].w,"root")) root = true;
            if (!strcmp(it[i].w,"elevato")||!strcmp(it[i].w,"elevata")||!strcmp(it[i].w,"potenza")||!strcmp(it[i].w,"power")) poww = true;
            // a cube-ish modifier too garbled for a_solve_funcs to claim (edit-distance 2-3 from
            // "cubica"): abstain below rather than fall through and answer the wrong (sqrt) root.
            if (a_damlev(it[i].w,"cubica",3) <= 3) cube_garble = true;
        }
    }
    // idiom "<num> alla <ordinale>" -> num^ordinal (strict adjacency; ordinal, not a range number).
    for (int i = 0; i + 2 < n; i++) {
        if (!it[i].isnum || it[i+1].isnum || it[i+2].isnum) continue;
        if (strcmp(it[i+1].w,"alla") && strcmp(it[i+1].w,"elevato") && strcmp(it[i+1].w,"elevata")) continue;
        int ord = a_ordinal_power(it[i+2].w);
        if (!ord) continue;
        char a[40], cc[40]; a_fmt_num(it[i].val,a,sizeof(a)); a_fmt_num(pow(it[i].val,ord),cc,sizeof(cc));
        r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 95;
        snprintf(r->intent, sizeof(r->intent), "calc"); snprintf(r->state, sizeof(r->state), "tool");
        snprintf(r->reply, sizeof(r->reply), en ? "%s^%d = %s." : "%s elevato %d = %s.", a, ord, cc);
        return true;
    }
    if (root && !cube_garble && nn >= 1 && nums[nn-1] >= 0) {
        char a[40], b[40]; a_fmt_num(nums[nn-1],a,sizeof(a)); a_fmt_num(sqrt(nums[nn-1]),b,sizeof(b));
        r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 95;
        snprintf(r->intent, sizeof(r->intent), "calc"); snprintf(r->state, sizeof(r->state), "tool");
        snprintf(r->reply, sizeof(r->reply), en ? "sqrt(%s) = %s." : "radice di %s = %s.", a, b);
        return true;
    }
    if (poww && nn >= 2) {
        char a[40], b[40], cc[40]; a_fmt_num(nums[0],a,sizeof(a)); a_fmt_num(nums[1],b,sizeof(b)); a_fmt_num(pow(nums[0],nums[1]),cc,sizeof(cc));
        r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 95;
        snprintf(r->intent, sizeof(r->intent), "calc"); snprintf(r->state, sizeof(r->state), "tool");
        snprintf(r->reply, sizeof(r->reply), en ? "%s^%s = %s." : "%s elevato %s = %s.", a, b, cc);
        return true;
    }
    return false;
}

// Ohm's law solver: given any two of V/I/R/P (with units), compute the rest. Gated on an
// electrical keyword so it never misfires on ordinary numbers. mA in/out for small currents.
static bool a_solve_ohm(const char *norm, a_sitem_t *it, int n, bool en, anima_result_t *r)
{
    (void)en;
    if (!(strstr(norm,"volt")||strstr(norm,"ampere")||strstr(norm,"ohm")||strstr(norm,"watt")||
          strstr(norm,"tension")||strstr(norm,"corrent")||strstr(norm,"resisten")||strstr(norm,"potenz")||
          strstr(norm,"current")||strstr(norm,"voltage")||strstr(norm,"resistance")||strstr(norm,"power"))) return false;
    double V=0,I=0,R=0,P=0; bool hV=false,hI=false,hR=false,hP=false;
    for (int i = 0; i + 1 < n; i++) {
        if (!it[i].isnum || it[i+1].isnum) continue;
        double val = it[i].val; const char *u = it[i+1].w;
        if      (!strcmp(u,"v")||!strncmp(u,"volt",4))                 { if(!hV){V=val;hV=true;} }
        else if (!strcmp(u,"ma")||!strcmp(u,"milliampere"))            { if(!hI){I=val*0.001;hI=true;} }
        else if (!strcmp(u,"a")||!strncmp(u,"amper",5)||!strcmp(u,"amp")) { if(!hI){I=val;hI=true;} }
        else if (!strcmp(u,"kohm")||!strcmp(u,"kiloohm"))              { if(!hR){R=val*1000;hR=true;} }
        else if (!strncmp(u,"ohm",3))                                  { if(!hR){R=val;hR=true;} }
        else if (!strcmp(u,"w")||!strncmp(u,"watt",4))                 { if(!hP){P=val;hP=true;} }
    }
    if (hV + hI + hR + hP < 2) {
        // Not enough quantities to COMPUTE. If it's a formula/law question, TEACH the law instead of
        // falling through to L1 (whose 0.85 gate refuses loose paraphrases like "come ricavo la
        // corrente da tensione e resistenza"). Still electrical-keyword-gated above -> zero FP risk;
        // triggers are tight (no bare "come"/"how") so a plain "collego una resistenza" won't fire.
        bool teach = strstr(norm,"legge di ohm")||strstr(norm,"ohm law")||strstr(norm,"ohm's law")||
                     strstr(norm,"formula")||strstr(norm,"relazion")||strstr(norm,"relation")||
                     strstr(norm," lega ")||strstr(norm,"legano")||strstr(norm,"ricav");
        if (!teach) return false;
        r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 90;
        snprintf(r->intent, sizeof(r->intent), "ohm"); snprintf(r->state, sizeof(r->state), "tool");
        snprintf(r->reply, sizeof(r->reply), en ?
            "Ohm's law: V = I * R (voltage = current * resistance). So I = V / R and R = V / I. Power: P = V * I." :
            "Legge di Ohm: V = I * R (tensione = corrente * resistenza). Quindi I = V / R e R = V / I. Potenza: P = V * I.");
        return true;
    }
    if (!hV) { if (hI&&hR) V=I*R; else if (hP&&hI&&I!=0) V=P/I; else if (hP&&hR) V=sqrt(P*R); }
    if (!hI) { if (hV&&hR&&R!=0) I=V/R; else if (hP&&hV&&V!=0) I=P/V; else if (hP&&hR&&R!=0) I=sqrt(P/R); }
    if (!hR) { if (hV&&I!=0) R=V/I; else if (hP&&I!=0) R=P/(I*I); else if (hP) R=(V*V)/P; }
    if (!hP) P = V*I;
    char sv[40],sr[40],sp[40],si[40],ci[48];
    a_fmt_num(V,sv,sizeof(sv)); a_fmt_num(R,sr,sizeof(sr)); a_fmt_num(P,sp,sizeof(sp));
    if (fabs(I) < 1.0) { a_fmt_num(I*1000,si,sizeof(si)); snprintf(ci,sizeof(ci),"%s mA",si); }
    else               { a_fmt_num(I,si,sizeof(si));      snprintf(ci,sizeof(ci),"%s A",si);  }
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 92;
    snprintf(r->intent, sizeof(r->intent), "ohm"); snprintf(r->state, sizeof(r->state), "tool");
    snprintf(r->reply, sizeof(r->reply), "V=%s V, I=%s, R=%s ohm, P=%s W.", sv, ci, sr, sp);
    return true;
}

// "valore assoluto di N" / "abs N"  and  "A modulo B" / "resto di A per B" -> exact abs / mod.
static bool a_solve_absmod(a_sitem_t *it, int n, bool en, anima_result_t *r)
{
    bool isabs = false, ismod = false; double nums[6]; int nn = 0;
    for (int i = 0; i < n; i++) {
        if (it[i].isnum) { if (nn < 6) nums[nn] = it[i].val; nn++; }
        else {
            if (!strcmp(it[i].w,"assoluto")||!strcmp(it[i].w,"abs")||!strcmp(it[i].w,"absolute")) isabs = true;
            if (!strcmp(it[i].w,"modulo")||!strcmp(it[i].w,"resto")||!strcmp(it[i].w,"mod")) ismod = true;
        }
    }
    if (isabs && nn >= 1) {
        char a[40], b[40]; a_fmt_num(nums[nn-1],a,sizeof(a)); a_fmt_num(fabs(nums[nn-1]),b,sizeof(b));
        r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 95;
        snprintf(r->intent, sizeof(r->intent), "calc"); snprintf(r->state, sizeof(r->state), "tool");
        snprintf(r->reply, sizeof(r->reply), en ? "abs(%s) = %s." : "valore assoluto di %s = %s.", a, b);
        return true;
    }
    if (ismod && nn >= 2 && nums[1] != 0) {
        char a[40], b[40], cc[40]; a_fmt_num(nums[0],a,sizeof(a)); a_fmt_num(nums[1],b,sizeof(b)); a_fmt_num(fmod(nums[0],nums[1]),cc,sizeof(cc));
        r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 95;
        snprintf(r->intent, sizeof(r->intent), "calc"); snprintf(r->state, sizeof(r->state), "tool");
        snprintf(r->reply, sizeof(r->reply), en ? "%s mod %s = %s." : "%s modulo %s = %s.", a, b, cc);
        return true;
    }
    return false;
}

// "fattoriale di N" / "N fattoriale" / "factorial of N" -> N! exact (N<=20 to stay in u64).
static bool a_solve_factorial(a_sitem_t *it, int n, bool en, anima_result_t *r)
{
    bool fact = false; double nums[6]; int nn = 0;
    for (int i = 0; i < n; i++) {
        if (it[i].isnum) { if (nn < 6) nums[nn] = it[i].val; nn++; }
        else if (!strcmp(it[i].w,"fattoriale")||!strcmp(it[i].w,"factorial")) fact = true;
    }
    if (!fact || nn < 1) return false;
    double x = nums[nn-1];
    // The keyword "fattoriale" makes the intent unambiguous, so an out-of-domain argument is answered
    // HONESTLY (the conscience), not silently dropped onto another solver or faked on |x|.
    if (x < 0 || x != (double)(long long)x) {
        r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 88;
        snprintf(r->intent, sizeof(r->intent), "calc"); snprintf(r->state, sizeof(r->state), "tool");
        snprintf(r->reply, sizeof(r->reply), en ? "Factorial is only defined for non-negative integers."
                                                : "Il fattoriale è definito solo per interi non negativi.");
        return true;
    }
    if (x > 20) {                                                     // 21! overflows unsigned 64-bit — say so
        char xb[24]; a_fmt_num(x, xb, sizeof xb);
        r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 88;
        snprintf(r->intent, sizeof(r->intent), "calc"); snprintf(r->state, sizeof(r->state), "tool");
        snprintf(r->reply, sizeof(r->reply), en ? "%s! is too large to compute exactly here (I go up to 20!)."
                                                : "%s! è troppo grande per calcolarlo con esattezza qui (arrivo fino a 20!).", xb);
        return true;
    }
    unsigned long long f = 1; for (int k = 2; k <= (int)x; k++) f *= (unsigned long long)k;
    char a[40]; a_fmt_num(x, a, sizeof(a));
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 95;
    snprintf(r->intent, sizeof(r->intent), "calc"); snprintf(r->state, sizeof(r->state), "tool");
    snprintf(r->reply, sizeof(r->reply), "%s! = %llu.", a, f);
    return true;
}

static long long a_gcd_ll(long long a, long long b) { a = a<0?-a:a; b = b<0?-b:b; while (b) { long long t = a % b; a = b; b = t; } return a; }

// "mcd di A e B" / "gcd of A and B"; "mcm" / "lcm" -> least common multiple. Integers only.
static bool a_solve_gcdlcm(a_sitem_t *it, int n, bool en, anima_result_t *r)
{
    bool g = false, l = false; double nums[6]; int nn = 0;
    bool comune = false, divisore = false, multiplo = false, common = false, divisor = false, multiple = false;
    for (int i = 0; i < n; i++) {
        if (it[i].isnum) { if (nn < 6) nums[nn] = it[i].val; nn++; }
        else {
            const char *w = it[i].w;
            if (!strcmp(w,"mcd")||!strcmp(w,"gcd")) g = true;
            if (!strcmp(w,"mcm")||!strcmp(w,"lcm")) l = true;
            // worded forms: "massimo comune divisore", "minimo comune multiplo", "greatest common divisor",
            // "least common multiple" — natural full phrasings, not just the acronyms.
            if (!strcmp(w,"comune")) comune = true;
            if (!strcmp(w,"common")) common = true;
            if (!strcmp(w,"divisore")) divisore = true;
            if (!strcmp(w,"divisor")||!strcmp(w,"divisori")) divisor = true;
            if (!strcmp(w,"multiplo")||!strcmp(w,"multipli")) multiplo = true;
            if (!strcmp(w,"multiple")) multiple = true;
        }
    }
    if (comune && divisore) g = true;
    if (common && divisor) g = true;
    if (comune && multiplo) l = true;
    if (common && multiple) l = true;
    if (!(g || l) || nn < 2) return false;
    if (nums[0] != (double)(long long)nums[0] || nums[1] != (double)(long long)nums[1]) return false;
    long long A = (long long)nums[0], B = (long long)nums[1], gg = a_gcd_ll(A, B);
    long long res = l ? (gg ? (A / gg) * B : 0) : gg; if (res < 0) res = -res;
    char a[40], b[40], c[40]; a_fmt_num(A, a, sizeof a); a_fmt_num(B, b, sizeof b); a_fmt_num((double)res, c, sizeof c);
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 95;
    snprintf(r->intent, sizeof(r->intent), "calc"); snprintf(r->state, sizeof(r->state), "tool");
    snprintf(r->reply, sizeof(r->reply), l ? (en ? "lcm(%s, %s) = %s." : "mcm(%s, %s) = %s.")
                                            : (en ? "gcd(%s, %s) = %s." : "mcd(%s, %s) = %s."), a, b, c);
    return true;
}

// "media di A B C" / "average of A B C" / "mean" -> arithmetic mean of all numbers (>=2).
static bool a_solve_average(a_sitem_t *it, int n, bool en, anima_result_t *r)
{
    bool avg = false, med = false; double sum = 0, vals[32]; int nn = 0;
    for (int i = 0; i < n; i++) {
        if (it[i].isnum) { if (nn < 32) vals[nn] = it[i].val; sum += it[i].val; nn++; }
        else if (!strcmp(it[i].w,"media")||!strcmp(it[i].w,"average")||!strcmp(it[i].w,"mean")||!strcmp(it[i].w,"medio")) avg = true;
        else if (!strcmp(it[i].w,"mediana")||!strcmp(it[i].w,"median")) med = true;
    }
    // MEDIAN = the middle of the sorted values (mean of the two middles when even). It needs REAL data:
    // a bare "cos'è la mediana" (no numbers) returns false here and falls through to KNOWLEDGE, never a
    // fabricated value or the wrong (mean) card — so the definition stays in the knowledge lane.
    if (med) {
        if (nn < 2) return false;
        int c = nn < 32 ? nn : 32;
        for (int a = 0; a < c; a++) for (int b = a + 1; b < c; b++) if (vals[b] < vals[a]) { double t = vals[a]; vals[a] = vals[b]; vals[b] = t; }
        double mv = (c % 2) ? vals[c / 2] : (vals[c / 2 - 1] + vals[c / 2]) / 2.0;
        char cm[40]; a_fmt_round(mv, cm, sizeof cm);   // media/mediana = calcolo "normale" -> max 4 dec
        r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 92;
        snprintf(r->intent, sizeof(r->intent), "calc"); snprintf(r->state, sizeof(r->state), "tool");
        snprintf(r->reply, sizeof(r->reply), en ? "The median is %s." : "La mediana e %s.", cm);
        return true;
    }
    if (!avg) return false;
    if (nn < 2) {
        // No numbers to average -> if it's a "how do I compute the average" question, TEACH the formula
        // (compute+teach pattern, like ohm). Without this it falls to L1 and mis-ranks to "dozen" (0.85).
        // Tightly cued so "social media"/"media del campionato" (no cue) don't fire.
        bool cue = false;
        for (int i = 0; i < n; i++) { const char *w = it[i].w;
            if (!strncmp(w,"calcol",6) || !strncmp(w,"trov",4) || !strcmp(w,"come") || !strcmp(w,"formula") ||
                !strcmp(w,"cosa") || !strcmp(w,"cos") || !strcmp(w,"how") || !strcmp(w,"calculate") || !strcmp(w,"find")) cue = true; }
        if (!cue) return false;
        r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 90;
        snprintf(r->intent, sizeof(r->intent), "calc"); snprintf(r->state, sizeof(r->state), "tool");
        snprintf(r->reply, sizeof(r->reply), en ?
            "The average (mean) = sum of the values divided by how many there are." :
            "La media si calcola sommando i valori e dividendo per quanti sono.");
        return true;
    }
    char c[40]; a_fmt_round(sum / nn, c, sizeof c);   // media = calcolo "normale" -> max 4 dec
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 92;
    snprintf(r->intent, sizeof(r->intent), "calc"); snprintf(r->state, sizeof(r->state), "tool");
    snprintf(r->reply, sizeof(r->reply), en ? "The average is %s." : "La media e %s.", c);
    return true;
}

// ============================================================================
// MATH ENGINE v2 — radix conversion, natural-language word problems, named
// operations (double/half/square/...), functions (log/ln/cbrt/round/trig) and
// number properties (prime/Fibonacci/roman). Exact, keyword-gated, bilingual,
// flash-resident (no heap, no .bss — pure .text + small const tables). Each frame
// fires ONLY when its trigger words are present, so plain arithmetic still falls
// through to the calc parser and ordinary queries reach retrieval untouched.
// ============================================================================

// ---- radix / base conversion (the headline new skill) ----------------------
static int a_b36(char c)                          // char -> digit value (0..35), or -1
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}
// Render an unsigned magnitude in base 2..36 (uppercase digits). out holds >= 66 bytes.
static void a_to_base(unsigned long long v, int base, char *out, size_t n)
{
    static const char *D = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    char tmp[72]; int t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v > 0 && t < (int)sizeof(tmp)) { tmp[t++] = D[v % (unsigned)base]; v /= (unsigned)base; }
    int o = 0; while (t > 0 && o < (int)n - 1) out[o++] = tmp[--t]; out[o] = 0;
}
// Parse an alnum string in `base` -> magnitude. false if any digit is out of range/empty.
static bool a_from_base(const char *s, int base, unsigned long long *val)
{
    unsigned long long v = 0; int seen = 0;
    for (const char *p = s; *p; p++) {
        int d = a_b36(*p);
        if (d < 0 || d >= base) return false;
        v = v * (unsigned)base + (unsigned)d;
        if (++seen > 64) return false;            // length cap (overflow guard)
    }
    if (!seen) return false;
    *val = v; return true;
}
// Base-name word -> radix, or 0 (binario/ottale/decimale/esadecimale + EN + short forms).
static int a_base_name(const char *w)
{
    if (!strcmp(w,"binario")||!strcmp(w,"binaria")||!strcmp(w,"binary")||!strcmp(w,"bin")) return 2;
    if (!strcmp(w,"ottale")||!strcmp(w,"octal")||!strcmp(w,"oct"))                          return 8;
    if (!strcmp(w,"decimale")||!strcmp(w,"decimal")||!strcmp(w,"denario")||!strcmp(w,"dec")) return 10;
    if (!strcmp(w,"esadecimale")||!strcmp(w,"hex")||!strcmp(w,"hexadecimal")||!strcmp(w,"esa")) return 16;
    return 0;
}
// "converti N in binario", "N in base 16", "654 in base 16", "FF da base 16 a base 10",
// "0x1A in decimale", "converti 1010 da binario". Honest: declines unless a base keyword
// AND a value are present and the value is valid in its source base.
static bool a_solve_base(const char *raw, bool en, anima_result_t *r)
{
    char norm[160]; a_norm_solve(raw, norm, sizeof(norm));
    char wd[24][20]; int nw = 0;                  // alnum word tokens
    for (const char *p = norm; *p && nw < 24; ) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        if (!*p) break;
        int l = 0; while (*p && isalnum((unsigned char)*p) && l < 19) wd[nw][l++] = *p++;
        wd[nw][l] = 0; nw++;
    }
    for (int i = 0; i < nw; i++)                   // "log in base 2 di 8" is a logarithm, not a radix
        if (!strcmp(wd[i],"log")||!strcmp(wd[i],"logaritmo")||!strcmp(wd[i],"logarithm")||
            !strcmp(wd[i],"ln")||!strcmp(wd[i],"log2")) return false;
    // "il triangolo con BASE 6 e altezza 4" — here "base" is a GEOMETRY dimension, not a numeral radix.
    // A geometry shape/dimension word (caught even when the shape itself is typo'd, via altezza/area/lato)
    // means this is a geometry question -> let geo own it (or abstain), never convert "4 in base 6".
    for (int i = 0; i < nw; i++)
        if (!strcmp(wd[i],"triangolo")||!strcmp(wd[i],"triangle")||!strcmp(wd[i],"trapezio")||!strcmp(wd[i],"trapezoid")||
            !strcmp(wd[i],"parallelogramma")||!strcmp(wd[i],"altezza")||!strcmp(wd[i],"area")||!strcmp(wd[i],"lato")||
            !strcmp(wd[i],"perimetro")||!strcmp(wd[i],"height")||!strcmp(wd[i],"perimeter")||!strcmp(wd[i],"side")) return false;
    bool used[24] = { false };
    int srcBase = 0, dstBase = 0; bool sawKw = false, convVerb = false, hexWord = false;
    for (int i = 0; i < nw; i++) {
        const char *prev = i > 0 ? wd[i-1] : "";
        bool fromSrc = !strcmp(prev,"da") || !strcmp(prev,"from");
        if (!strcmp(wd[i],"converti")||!strcmp(wd[i],"convert")||!strcmp(wd[i],"convertire")||!strcmp(wd[i],"converte")) convVerb = true;
        if (!strcmp(wd[i],"esadecimale")||!strcmp(wd[i],"hex")||!strcmp(wd[i],"hexadecimal")||!strcmp(wd[i],"esa")) hexWord = true;
        int nb = a_base_name(wd[i]);
        if (nb) {
            sawKw = true; used[i] = true;
            if (fromSrc) { if (!srcBase) srcBase = nb; } else if (!dstBase) dstBase = nb;
            continue;
        }
        if (!strcmp(wd[i],"base")) {
            sawKw = true; used[i] = true;
            if (i + 1 < nw) {
                int b = atoi(wd[i+1]);
                if (b >= 2 && b <= 36) { used[i+1] = true;
                    if (fromSrc) { if (!srcBase) srcBase = b; } else if (!dstBase) dstBase = b; }
            }
        }
    }
    if (!sawKw) return false;
    // value = first unused token containing a digit; else a pure hex-letter token (assume base 16).
    const char *val = NULL;
    for (int i = 0; i < nw && !val; i++) {
        if (used[i]) continue;
        for (const char *p = wd[i]; *p; p++) if (*p >= '0' && *p <= '9') { val = wd[i]; break; }
    }
    // Letter-only value (e.g. "FF") = hex ONLY with positive evidence: a conversion verb, an
    // explicit hex word, or a known hex source. Guards against IT/EN words that are valid hex
    // ("da", "e", "fa", "ad", "ace") being mistaken for numbers.
    if (!val && (convVerb || hexWord || srcBase == 16)) for (int i = 0; i < nw && !val; i++) {
        if (used[i] || strlen(wd[i]) < 2) continue;
        bool hexonly = true;
        for (const char *p = wd[i]; *p; p++) if (!(*p >= 'a' && *p <= 'f')) { hexonly = false; break; }
        if (hexonly) { val = wd[i]; if (!srcBase) srcBase = 16; }
    }
    if (!val) return false;
    char vbuf[40]; int vl = 0;                     // copy, then strip an 0x/0b/0o prefix
    for (const char *p = val; *p && vl < 39; p++) vbuf[vl++] = *p;
    vbuf[vl] = 0;
    if (vbuf[0] == '0' && (vbuf[1]=='x'||vbuf[1]=='b'||vbuf[1]=='o') && vbuf[2]) {
        int pb = vbuf[1]=='x' ? 16 : vbuf[1]=='b' ? 2 : 8;
        if (!srcBase || srcBase == 10) srcBase = pb;
        memmove(vbuf, vbuf + 2, strlen(vbuf + 2) + 1);
    }
    if (dstBase == 0 && srcBase == 0) return false;   // no real base resolved -> not a base query
    if (!srcBase) srcBase = 10;
    if (!dstBase) dstBase = 10;
    unsigned long long v;
    if (!a_from_base(vbuf, srcBase, &v)) return false;
    char dst[72]; a_to_base(v, dstBase, dst, sizeof(dst));
    char up[40]; int k = 0; for (const char *p = vbuf; *p && k < 39; p++) up[k++] = (char)toupper((unsigned char)*p); up[k] = 0;
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 96;
    snprintf(r->intent, sizeof(r->intent), "base"); snprintf(r->state, sizeof(r->state), "tool");
    if (srcBase == 10) snprintf(r->reply, sizeof(r->reply), "%s in base %d = %s.", up, dstBase, dst);
    else               snprintf(r->reply, sizeof(r->reply), "%s (base %d) = %s (base %d).", up, srcBase, dst, dstBase);
    (void)en;
    return true;
}

// ---- roman numerals (both directions) --------------------------------------
static void a_int_to_roman(int v, char *out, size_t n)
{
    static const struct { int v; const char *s; } R[] = {
        {1000,"M"},{900,"CM"},{500,"D"},{400,"CD"},{100,"C"},{90,"XC"},
        {50,"L"},{40,"XL"},{10,"X"},{9,"IX"},{5,"V"},{4,"IV"},{1,"I"} };
    int o = 0;
    for (size_t i = 0; i < sizeof(R)/sizeof(R[0]); i++)
        while (v >= R[i].v && o < (int)n - 3) { for (const char *p = R[i].s; *p; p++) out[o++] = *p; v -= R[i].v; }
    out[o] = 0;
}
static int a_roman_to_int(const char *s)           // -1 if not a clean roman numeral
{
    int prev = 0, total = 0, seen = 0;
    for (const char *p = s + strlen(s); p > s; ) {
        p--;
        int d;
        switch (*p) {
            case 'i': d = 1; break;   case 'v': d = 5; break;   case 'x': d = 10; break;
            case 'l': d = 50; break;  case 'c': d = 100; break; case 'd': d = 500; break;
            case 'm': d = 1000; break; default: return -1;
        }
        if (d < prev) total -= d; else { total += d; prev = d; }
        seen++;
    }
    return seen ? total : -1;
}
static bool a_solve_roman(const char *raw, bool en, anima_result_t *r)
{
    char norm[160]; a_norm_solve(raw, norm, sizeof(norm));
    char wd[24][20]; int nw = 0;
    for (const char *p = norm; *p && nw < 24; ) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        if (!*p) break;
        int l = 0; while (*p && isalnum((unsigned char)*p) && l < 19) wd[nw][l++] = *p++;
        wd[nw][l] = 0; nw++;
    }
    bool romanKw = false, decKw = false;
    for (int i = 0; i < nw; i++) {
        const char *w = wd[i];
        if (!strcmp(w,"romano")||!strcmp(w,"romani")||!strcmp(w,"roman")||!strcmp(w,"romana")) romanKw = true;
        if (!strcmp(w,"decimale")||!strcmp(w,"arabo")||!strcmp(w,"arabi")||!strcmp(w,"vale")) decKw = true;
    }
    if (!romanKw && !decKw) return false;
    if (romanKw) for (int i = 0; i < nw; i++) {     // forward: a decimal 1..3999 -> roman
        bool alldig = wd[i][0] != 0;
        for (const char *p = wd[i]; *p; p++) if (!isdigit((unsigned char)*p)) alldig = false;
        if (alldig) {
            int v = atoi(wd[i]);
            if (v >= 1 && v <= 3999) { char rm[32]; a_int_to_roman(v, rm, sizeof(rm));
                r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 95;
                snprintf(r->intent, sizeof(r->intent), "roman"); snprintf(r->state, sizeof(r->state), "tool");
                snprintf(r->reply, sizeof(r->reply), en ? "%d in roman numerals = %s." : "%d in numeri romani = %s.", v, rm);
                return true; }
        }
    }
    // reverse: roman letters -> value. Many IT/EN words are made only of roman letters ("di",
    // "mi", "il", "civic"), so accept ONLY a CANONICAL numeral (round-trips through int_to_roman)
    // of length >= 2, and skip a short stoplist of common words that happen to be canonical.
    static const char *STOP[] = { "di", "mi", "ci", "vi", "li", NULL };
    for (int i = 0; i < nw; i++) {
        if (strlen(wd[i]) < 2) continue;
        bool roms = true, stop = false;
        for (const char *p = wd[i]; *p; p++) if (!strchr("ivxlcdm", *p)) { roms = false; break; }
        for (int s = 0; STOP[s]; s++) if (!strcmp(wd[i], STOP[s])) stop = true;
        if (!roms || stop) continue;
        int v = a_roman_to_int(wd[i]);
        if (v <= 0) continue;
        char canon[40]; a_int_to_roman(v, canon, sizeof(canon));
        char up[24]; int k = 0; for (const char *p = wd[i]; *p && k < 23; p++) up[k++] = (char)toupper((unsigned char)*p); up[k] = 0;
        if (strcmp(canon, up) != 0) continue;            // not a well-formed roman numeral -> skip
        r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 95;
        snprintf(r->intent, sizeof(r->intent), "roman"); snprintf(r->state, sizeof(r->state), "tool");
        snprintf(r->reply, sizeof(r->reply), "%s = %d.", up, v);
        return true;
    }
    return false;
}

// ---- named scale ops: double/half/triple/quadruple/third/quarter/square/cube/reciprocal/opposite
static bool a_solve_scale(const a_sitem_t *it, int n, bool en, anima_result_t *r)
{
    int op = 0; double x = 0; int nn = 0;
    for (int i = 0; i < n; i++) {
        if (it[i].isnum) { x = it[i].val; nn++; continue; }
        const char *w = it[i].w;
        if (op) continue;
        if      (!strcmp(w,"doppio")||!strcmp(w,"double"))                          op = 1;
        else if (!strcmp(w,"triplo")||!strcmp(w,"triple")||!strcmp(w,"triplica"))   op = 2;
        else if (!strcmp(w,"quadruplo")||!strcmp(w,"quadruple"))                     op = 3;
        else if (!strcmp(w,"meta")||!strcmp(w,"half"))                              op = 4;
        else if (!strcmp(w,"terzo")||!strcmp(w,"third"))                            op = 5;
        else if (!strcmp(w,"quarto")||!strcmp(w,"quarter"))                         op = 6;
        else if (!strcmp(w,"quadrato")||!strcmp(w,"square")||!strcmp(w,"squared"))  op = 7;
        else if (!strcmp(w,"cubo")||!strcmp(w,"cube")||!strcmp(w,"cubed"))          op = 8;
        else if (!strcmp(w,"reciproco")||!strcmp(w,"reciprocal")||!strcmp(w,"inverso")) op = 9;
        else if (!strcmp(w,"opposto")||!strcmp(w,"opposite")||!strcmp(w,"negato"))  op = 10;
    }
    if (!op || nn < 1) return false;
    double res;
    switch (op) {
        case 1: res = x*2; break;  case 2: res = x*3; break;  case 3: res = x*4; break;
        case 4: res = x/2; break;  case 5: res = x/3; break;  case 6: res = x/4; break;
        case 7: res = x*x; break;  case 8: res = x*x*x; break;
        case 9: if (x == 0) return false; res = 1.0/x; break;
        case 10: res = -x; break;  default: return false;
    }
    static const char *LIT[] = {"","doppio","triplo","quadruplo","meta","un terzo","un quarto","quadrato","cubo","reciproco","opposto"};
    static const char *LEN[] = {"","double","triple","quadruple","half","a third","a quarter","square","cube","reciprocal","opposite"};
    char a[40], b[40]; a_fmt_num(x,a,sizeof a); a_fmt_num(res,b,sizeof b);
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 95;
    snprintf(r->intent, sizeof(r->intent), "calc"); snprintf(r->state, sizeof(r->state), "tool");
    snprintf(r->reply, sizeof(r->reply), en ? "%s of %s = %s." : "%s di %s = %s.", en ? LEN[op] : LIT[op], a, b);
    return true;
}

// ---- natural-language binary ops: sum/difference/product/quotient + "take A from B"/"add A to B"
static bool a_solve_binop(const a_sitem_t *it, int n, bool en, anima_result_t *r)
{
    double num[6]; int nn = 0;
    bool sum=false, diff=false, prod=false, quot=false, take=false, addto=false, manca=false, hasDa=false, hasA=false;
    for (int i = 0; i < n; i++) {
        if (it[i].isnum) { if (nn < 6) num[nn] = it[i].val; nn++; continue; }
        const char *w = it[i].w;
        if      (!strcmp(w,"somma")||!strcmp(w,"sum")||!strcmp(w,"addizione")||
                 !strcmp(w,"sommami")||!strcmp(w,"sommare")||!strcmp(w,"sommo"))  sum = true;
        else if (!strcmp(w,"differenza")||!strcmp(w,"difference")||!strcmp(w,"sottrazione")) diff = true;
        else if (!strcmp(w,"prodotto")||!strcmp(w,"product"))                    prod = true;
        else if (!strcmp(w,"quoziente")||!strcmp(w,"quotient"))                  quot = true;
        else if (!strcmp(w,"togli")||!strcmp(w,"tolgo")||!strcmp(w,"sottrai")||!strcmp(w,"leva")||
                 !strcmp(w,"subtract")||!strcmp(w,"take")||!strcmp(w,"remove"))  take = true;
        else if (!strcmp(w,"aggiungi")||!strcmp(w,"aggiungo")||!strcmp(w,"add")||!strcmp(w,"addiziona")) addto = true;
        else if (!strcmp(w,"manca")||!strcmp(w,"mancano")||!strcmp(w,"missing")) manca = true;
        if (!strcmp(w,"da")||!strcmp(w,"from"))                       hasDa = true;
        if (!strcmp(w,"a")||!strcmp(w,"to")||!strcmp(w,"al")||!strcmp(w,"ad")) hasA = true;
    }
    if (nn < 2) return false;
    // SUM / ADD over ALL operands: "sommami 12 e 30 e 8" = 50, "add 12 and 30" = 42 (any connector).
    if (sum || addto) {
        double tot = 0; for (int i = 0; i < nn; i++) tot += num[i];
        char buf[120]; int o = 0;
        for (int i = 0; i < nn && o < (int)sizeof buf - 20; i++) {
            char a[40]; a_fmt_num(num[i], a, sizeof a);
            o += snprintf(buf + o, sizeof buf - o, "%s%s", i ? " + " : "", a);
        }
        char t[40]; a_fmt_num(tot, t, sizeof t);
        r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 95;
        snprintf(r->intent, sizeof(r->intent), "calc"); snprintf(r->state, sizeof(r->state), "tool");
        snprintf(r->reply, sizeof(r->reply), "%s = %s.", buf, t);
        return true;
    }
    double L, Rr; char op;
    if      (take && hasDa) { L = num[1]; Rr = num[0]; op = '-'; }  // "togli 10 da 20" -> 20 - 10
    else if (addto && hasA) { L = num[0]; Rr = num[1]; op = '+'; }  // "aggiungi 5 a 3"  -> 5 + 3
    else if (manca)         { L = num[1]; Rr = num[0]; op = '-'; }  // "quanto manca da 3 a 10"
    else if (sum)           { L = num[0]; Rr = num[1]; op = '+'; }
    else if (diff)          { L = num[0]; Rr = num[1]; op = '-'; }
    else if (prod)          { L = num[0]; Rr = num[1]; op = '*'; }
    else if (quot)          { L = num[0]; Rr = num[1]; op = '/'; }
    else return false;
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 95;
    snprintf(r->intent, sizeof(r->intent), "calc"); snprintf(r->state, sizeof(r->state), "tool");
    if (op == '/' && Rr == 0) {
        snprintf(r->reply, sizeof(r->reply), en ? "I can't divide by zero." : "Non posso dividere per zero.");
        return true;
    }
    double res = op=='-' ? L - Rr : op=='+' ? L + Rr : op=='*' ? L * Rr : L / Rr;
    char a[40], b[40], c[40]; a_fmt_num(L,a,sizeof a); a_fmt_num(Rr,b,sizeof b); a_fmt_num(res,c,sizeof c);
    snprintf(r->reply, sizeof(r->reply), "%s %c %s = %s.", a, op, b, c);
    return true;
}

// ---- functions: log/ln/log2/cbrt(radice cubica)/round/floor/ceil/sin/cos/tan/exp -----------
static bool a_solve_funcs(const char *norm, const a_sitem_t *it, int n, bool en, anima_result_t *r)
{
    (void)norm;
    int fn = 0; double x = 0; bool havex = false; double lbase = 0; bool deg = false, rad = false;
    bool has_radice = false;                              // "radice" seen -> a cube-ish typo means CUBE root
    for (int i = 0; i < n; i++)                           // pre-scan (order-independent): is this a root query?
        if (!it[i].isnum && (!strcmp(it[i].w,"radice") || !strcmp(it[i].w,"root"))) { has_radice = true; break; }
    const char *prev = "";
    for (int i = 0; i < n; i++) {
        if (it[i].isnum) {
            if (!strcmp(prev,"base")) { if (lbase == 0) lbase = it[i].val; }   // log base, not the operand
            else { x = it[i].val; havex = true; }                             // last non-base number wins
            prev = "#"; continue;
        }
        const char *w = it[i].w; prev = w;
        // "radice <cubica>" with a light typo ("cuboca"/"cubika"/"qubica"/a transposition) still means
        // CUBE root. Gated on a root word being present (so "10 metri cubici" never misfires) and matched
        // by edit-distance, plus the English "cube/cubic root". Without this a typo fell through to the
        // default sqrt branch and emitted a confidently WRONG number (e.g. radice di 8 = 2.82843).
        bool cube_mod = has_radice && (a_damlev(w,"cubica",1) <= 1 || !strcmp(w,"cube") || !strcmp(w,"cubic"));
        if (!fn) {
            if      (!strcmp(w,"ln"))                                                  fn = 2;
            else if (!strcmp(w,"log2"))                                                fn = 3;
            else if (!strcmp(w,"log")||!strcmp(w,"logaritmo")||!strcmp(w,"logarithm")) fn = 1;
            else if (!strcmp(w,"cubica")||!strcmp(w,"cbrt")||cube_mod)                 fn = 4;
            else if (!strcmp(w,"arrotonda")||!strcmp(w,"round")||!strcmp(w,"arrotondare")) fn = 5;
            else if (!strcmp(w,"pavimento")||!strcmp(w,"floor"))                       fn = 6;
            else if (!strcmp(w,"soffitto")||!strcmp(w,"ceil")||!strcmp(w,"ceiling"))   fn = 7;
            // Full IT/EN trig words are unambiguous -> bind freely (only the abbreviations are false friends).
            else if (!strcmp(w,"seno")||!strcmp(w,"coseno")||!strcmp(w,"tangente")||
                     !strcmp(w,"sine")||!strcmp(w,"cosine")||!strcmp(w,"tangent"))
                fn = (!strcmp(w,"seno")||!strcmp(w,"sine")) ? 8 : (!strcmp(w,"coseno")||!strcmp(w,"cosine")) ? 9 : 10;
            // Short trig abbreviations are FALSE FRIENDS of common words: "cos" is the truncation of
            // "cos'è"/"cosa" (the tokenizer splits on the apostrophe), "sin"/"sen"~"sino"/"senza",
            // "tan"~"tanto". A bare match + a number anywhere in a nonsense sentence fabricated a cosine
            // ("cos'è xkcd 42" -> cos(42)=0.74). So an abbreviation binds ONLY with its argument ADJACENT:
            // "cos 60", "cos di 60". Guards 0-hallucination without losing real abbreviated queries.
            else if ((!strcmp(w,"sin")||!strcmp(w,"sen")||!strcmp(w,"cos")||!strcmp(w,"tan")||!strcmp(w,"tg"))
                     && ((i+1 < n && it[i+1].isnum)
                         || (i+2 < n && !it[i+1].isnum && (!strcmp(it[i+1].w,"di")||!strcmp(it[i+1].w,"of")) && it[i+2].isnum)))
                fn = (!strcmp(w,"sin")||!strcmp(w,"sen")) ? 8 : !strcmp(w,"cos") ? 9 : 10;
            else if (!strcmp(w,"esponenziale")||!strcmp(w,"exp"))                      fn = 11;
        }
        if (!strcmp(w,"naturale")||!strcmp(w,"natural")) { if (fn == 1) fn = 2; }
        if (!strcmp(w,"gradi")||!strcmp(w,"degrees")||!strcmp(w,"deg"))    deg = true;
        if (!strcmp(w,"radianti")||!strcmp(w,"radians")||!strcmp(w,"rad")) rad = true;
    }
    if (!fn || !havex) return false;
    double res; char note[24] = "";
    switch (fn) {
        case 1: if (x <= 0) return false; res = (lbase > 0 && lbase != 1) ? log(x)/log(lbase) : log10(x); break;
        case 2: if (x <= 0) return false; res = log(x); break;
        case 3: if (x <= 0) return false; res = log2(x); break;
        case 4: res = cbrt(x); break;
        case 5: res = round(x); break;
        case 6: res = floor(x); break;
        case 7: res = ceil(x); break;
        case 8: case 9: case 10: {
            bool useDeg = deg || !rad;                 // default degrees for NL queries, but labelled
            double ang = useDeg ? x * M_PI / 180.0 : x;
            res = fn==8 ? sin(ang) : fn==9 ? cos(ang) : tan(ang);
            if (fabs(res) < 1e-12) res = 0;
            snprintf(note, sizeof(note), en ? (useDeg?" (deg)":" (rad)") : (useDeg?" (gradi)":" (radianti)"));
            break; }
        case 11: res = exp(x); break;
        default: return false;
    }
    char a[40], b[40]; a_fmt_num(x,a,sizeof a); a_fmt_num(res,b,sizeof b);
    static const char *NM[] = {"","log","ln","log2","cbrt","round","floor","ceil","sin","cos","tan","exp"};
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 95;
    snprintf(r->intent, sizeof(r->intent), "calc"); snprintf(r->state, sizeof(r->state), "tool");
    if (fn == 1 && lbase > 0 && lbase != 1) { char lb[24]; a_fmt_num(lbase,lb,sizeof lb);
        snprintf(r->reply, sizeof(r->reply), en ? "log base %s of %s = %s." : "log base %s di %s = %s.", lb, a, b); }
    else if (fn == 4)
        snprintf(r->reply, sizeof(r->reply), en ? "cube root of %s = %s." : "radice cubica di %s = %s.", a, b);
    else
        snprintf(r->reply, sizeof(r->reply), "%s(%s)%s = %s.", NM[fn], a, note, b);
    return true;
}

// ---- number properties: "N is prime?" and "Fibonacci(N)" -------------------
static bool a_prime_ull(unsigned long long x)
{
    if (x < 2) return false;
    if (x % 2 == 0) return x == 2;
    if (x % 3 == 0) return x == 3;
    for (unsigned long long i = 5; i * i <= x; i += 6)
        if (x % i == 0 || x % (i + 2) == 0) return false;
    return true;
}
static unsigned long long a_least_factor(unsigned long long x)
{
    if (x < 2) return 0;
    if (x % 2 == 0) return 2;
    for (unsigned long long i = 3; i * i <= x; i += 2) if (x % i == 0) return i;
    return x;
}
static bool a_solve_numprop(const a_sitem_t *it, int n, bool en, anima_result_t *r)
{
    bool prime = false, fib = false; double nums[6]; int nn = 0;
    for (int i = 0; i < n; i++) {
        if (it[i].isnum) { if (nn < 6) nums[nn] = it[i].val; nn++; continue; }
        const char *w = it[i].w;
        if (!strcmp(w,"primo")||!strcmp(w,"prime")||!strcmp(w,"primi")) prime = true;
        if (!strcmp(w,"fibonacci")||!strcmp(w,"fib"))                   fib = true;
    }
    if (fib && nn >= 1) {
        double k = nums[nn-1];
        if (k >= 0 && k <= 90 && k == (double)(long long)k) {
            unsigned long long a = 0, b = 1; int kk = (int)k;
            for (int i = 0; i < kk; i++) { unsigned long long t = a + b; a = b; b = t; }
            char kb[24]; a_fmt_num(k, kb, sizeof kb);
            r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 95;
            snprintf(r->intent, sizeof(r->intent), "calc"); snprintf(r->state, sizeof(r->state), "tool");
            snprintf(r->reply, sizeof(r->reply), "Fibonacci(%s) = %llu.", kb, a);
            return true;
        }
    }
    if (prime && nn >= 1) {
        double xv = nums[nn-1];
        if (xv < 0 || xv != (double)(long long)xv) {                  // "primo" is unambiguous → honest, not faked
            char xb[24]; a_fmt_num(xv, xb, sizeof xb);
            r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 88;
            snprintf(r->intent, sizeof(r->intent), "prime"); snprintf(r->state, sizeof(r->state), "tool");
            snprintf(r->reply, sizeof(r->reply), en ? "Primality only applies to whole numbers, so %s is neither prime nor composite."
                                                    : "La primalità riguarda solo i numeri interi, quindi %s non è né primo né composto.", xb);
            return true;
        }
        if (xv >= 0 && xv < 1e15 && xv == (double)(long long)xv) {
            unsigned long long x = (unsigned long long)(long long)xv;
            char xb[24]; a_fmt_num(xv, xb, sizeof xb);
            r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 95;
            snprintf(r->intent, sizeof(r->intent), "prime"); snprintf(r->state, sizeof(r->state), "tool");
            if (a_prime_ull(x))
                snprintf(r->reply, sizeof(r->reply), en ? "Is %s prime? Yes." : "%s e primo? Si.", xb);
            else {
                unsigned long long f = a_least_factor(x);
                if (f && f != x) { char fb[24], qb[24]; a_fmt_num((double)f, fb, sizeof fb); a_fmt_num((double)(x/f), qb, sizeof qb);
                    snprintf(r->reply, sizeof(r->reply), en ? "Is %s prime? No (%s = %s x %s)." : "%s e primo? No (%s = %s x %s).", xb, xb, fb, qb); }
                else
                    snprintf(r->reply, sizeof(r->reply), en ? "Is %s prime? No." : "%s e primo? No.", xb);
            }
            return true;
        }
    }
    return false;
}

// ============================================================================
// GEOMETRY skill — areas, perimeters, circumference, volumes, surfaces, the
// Pythagorean theorem and the value of pi. Bilingual. Two modes: COMPUTE (a
// dimension is given) and TEACH (none -> state the formula). Fires ONLY when a
// shape word co-occurs with a quantity/dimension word (or Pythagorean / pi), so
// "ho 3 cerchi" and "alza il volume" never misfire. Flash-resident, no heap.
// ============================================================================
#ifndef A_PI
#define A_PI 3.14159265358979323846
#endif
enum { G_NONE=0, G_CIR, G_SQ, G_REC, G_TRI, G_TRAP, G_RHO, G_PAR, G_CUBE, G_SPH, G_CYL, G_CONE };
enum { Q_NONE=0, Q_AREA, Q_PERIM, Q_CIRC, Q_VOL, Q_SURF, Q_DIAG, Q_DIAM };

static int a_shape_word(const char *w)
{
    if (!strcmp(w,"cerchio")||!strcmp(w,"circle")||!strcmp(w,"cerchi")) return G_CIR;
    if (!strcmp(w,"quadrato")||!strcmp(w,"square")) return G_SQ;
    if (!strcmp(w,"rettangolo")||!strcmp(w,"rectangle")) return G_REC;
    if (!strcmp(w,"triangolo")||!strcmp(w,"triangle")||!strcmp(w,"ipotenusa")||!strcmp(w,"hypotenuse")||
        !strcmp(w,"cateto")||!strcmp(w,"cateti")||!strcmp(w,"leg")||!strcmp(w,"legs")) return G_TRI;
    if (!strcmp(w,"trapezio")||!strcmp(w,"trapezoid")||!strcmp(w,"trapezium")) return G_TRAP;
    if (!strcmp(w,"rombo")||!strcmp(w,"rhombus")) return G_RHO;
    if (!strcmp(w,"parallelogramma")||!strcmp(w,"parallelogram")) return G_PAR;
    if (!strcmp(w,"cubo")||!strcmp(w,"cube")) return G_CUBE;
    if (!strcmp(w,"sfera")||!strcmp(w,"sphere")) return G_SPH;
    if (!strcmp(w,"cilindro")||!strcmp(w,"cylinder")) return G_CYL;
    if (!strcmp(w,"cono")||!strcmp(w,"cone")) return G_CONE;
    return G_NONE;
}
// Value of the dimension labelled by any keyword in kw: the number ADJACENT to the label
// ("raggio 5") or one connector away ("raggio di 5"). Adjacency, not nearest — so each label
// binds its OWN value and a bare requested word ("ipotenusa con cateti 3 e 4") binds nothing.
static double a_dim(const a_sitem_t *it, int n, const char *const *kw)
{
    static const char *const CONN[] = {"di","of","con","with","del","dello","della",NULL};
    for (int i = 0; i < n; i++) if (!it[i].isnum)
        for (int k = 0; kw[k]; k++) if (!strcmp(it[i].w, kw[k])) {
            if (i+1 < n && it[i+1].isnum) return it[i+1].val;                 // "raggio 5"
            if (i+2 < n && !it[i+1].isnum && it[i+2].isnum)                   // "raggio di 5"
                for (int c = 0; CONN[c]; c++) if (!strcmp(it[i+1].w, CONN[c])) return it[i+2].val;
        }
    return NAN;
}

// A geometry/physics TEACH (the symbolic formula, no numbers to compute) must be ABOUT geometry/physics —
// not a math keyword embedded in an unrelated phrase via a possessive ("il cane DI pitagora", "la velocità
// DEL pensiero", "area ... OF a promise"). Reframed = a possessive preposition AND a FOREIGN content word
// (neither math vocabulary nor a filler). Narrow on purpose — needs BOTH — so "formula di pitagora",
// "area del cerchio" (no foreign word) and "calcola l'area" (no possessive) still teach normally.
static bool a_math_reframed(const a_sitem_t *it, int n)
{
    static const char *const mathw[] = {
        // shapes
        "quadrato","quadrati","square","cerchio","cerchi","circle","circonferenza","circumference",
        "triangolo","triangoli","triangle","rettangolo","rectangle","rombo","rhombus","trapezio","trapezoid",
        "parallelogramma","pentagono","esagono","ettagono","ottagono","poligono","polygon","sfera","sphere",
        "cubo","cube","cilindro","cylinder","cono","cone","piramide","pyramid","prisma","prism","ellisse",
        // dimensions / quantities
        "lato","lati","side","sides","raggio","radius","diametro","diameter","base","basi","altezza","alta",
        "alto","height","spigolo","edge","apotema","ipotenusa","hypotenuse","cateto","cateti","leg","legs",
        "diagonale","diagonal","area","aree","perimetro","perimeter","volume","volumi","superficie","surface",
        // physics quantities + law names
        "velocita","velocity","speed","accelerazione","acceleration","forza","forze","force","energia","energy",
        "massa","mass","spazio","distanza","distance","tempo","time","potenza","power","lavoro","work","peso",
        "weight","gravita","gravity","attrito","friction","moto","momentum","quantita","quantity","frequenza","frequency","periodo",
        "period","densita","density","pressione","pressure","cinetica","kinetic","potenziale","potential",
        "legge","leggi","law","laws","newton","newtons","ohm","coulomb","joule","watt","watts","pascal",
        "hertz","kepler","prima","seconda","terza","first","second","third",
        // units commonly attached to a formula
        "metri","metro","meters","meter","metre","cm","mm","km","secondi","secondo","seconds","grammi",
        "grammo","kg","chilo","chili","newton",
        // constants / topic
        "pi","pigreco","greco","greek","pitagora","pythagoras","pythagorean","teorema","theorem","costante","constant",
        // cues
        "formula","formule","regola","rule","calcola","calcolare","calcolo","calculate","compute","valore",
        "value","vale","misura","measure","dammi","dimmi","spiega","explain","trova","find",
        // fillers / articles / preps / qwords / to-be / lead-ins
        "il","lo","la","i","gli","le","un","uno","una","l","di","del","dello","della","dei","degli","delle",
        "al","alla","per","su","con","e","che","come","quanto","quanta","quale","qual","cos","cosa",
        "what","which","how","the","of","for","is","si","mi","sai","puoi", NULL };
    bool poss = false, foreign = false;
    for (int i = 0; i < n; i++) {
        if (it[i].isnum) continue;
        const char *w = it[i].w;
        if (!strcmp(w,"di")||!strcmp(w,"del")||!strcmp(w,"della")||!strcmp(w,"dello")||!strcmp(w,"dei")||
            !strcmp(w,"degli")||!strcmp(w,"delle")||!strcmp(w,"of")) poss = true;
        if (w[0] == 0 || w[1] == 0) continue;        // single letters are fillers
        bool ok = false;
        for (int k = 0; mathw[k]; k++) if (!strcmp(mathw[k], w)) { ok = true; break; }
        if (!ok) foreign = true;                     // a word outside the math/filler vocabulary
    }
    return poss && foreign;
}

static bool a_solve_geometry(const a_sitem_t *it, int n, bool en, anima_result_t *r)
{
    int shape=G_NONE, q=Q_NONE;
    bool formula=false, dimLabel=false, pyth=false, circW=false;
    bool pigreco=false, haspi=false, hasgreco=false, hasval=false;
    double nums[8]; int nn=0;
    for (int i=0;i<n;i++){
        if (it[i].isnum){ if(nn<8) nums[nn++]=it[i].val; continue; }
        const char *w=it[i].w;
        int s=a_shape_word(w); if(s&&!shape) shape=s;
        if(!strcmp(w,"circonferenza")||!strcmp(w,"circumference")) circW=true;
        if(!strcmp(w,"ipotenusa")||!strcmp(w,"hypotenuse")||!strcmp(w,"cateto")||!strcmp(w,"cateti")||
           !strcmp(w,"leg")||!strcmp(w,"legs")) pyth=true;
        if(!strcmp(w,"pitagora")||!strcmp(w,"pythagoras")||!strcmp(w,"pythagorean")){ pyth=true; if(!shape)shape=G_TRI; formula=true; }
        if(!strcmp(w,"area")) q=Q_AREA;
        else if(!strcmp(w,"perimetro")||!strcmp(w,"perimeter")) q=Q_PERIM;
        else if(!strcmp(w,"volume")) q=Q_VOL;
        else if(!strcmp(w,"superficie")||!strcmp(w,"surface")||!strcmp(w,"superficiale")) q=Q_SURF;
        else if(!strcmp(w,"diagonale")||!strcmp(w,"diagonal")) q=Q_DIAG;
        else if((!strcmp(w,"diametro")||!strcmp(w,"diameter")) && !q) q=Q_DIAM;
        else if((!strcmp(w,"circonferenza")||!strcmp(w,"circumference")) && !q) q=Q_CIRC;
        if(!strcmp(w,"formula")||!strcmp(w,"formule")||!strcmp(w,"regola")||!strcmp(w,"rule")||
           !strcmp(w,"calcola")||!strcmp(w,"calcolare")||!strcmp(w,"calcolo")||!strcmp(w,"calculate")) formula=true;
        if(!strcmp(w,"raggio")||!strcmp(w,"radius")||!strcmp(w,"lato")||!strcmp(w,"side")||!strcmp(w,"spigolo")||
           !strcmp(w,"edge")||!strcmp(w,"base")||!strcmp(w,"altezza")||!strcmp(w,"height")||!strcmp(w,"apotema")||
           !strcmp(w,"diametro")||!strcmp(w,"diameter")) dimLabel=true;
        if(!strcmp(w,"pigreco")) pigreco=true;
        if(!strcmp(w,"pi")) haspi=true;
        if(!strcmp(w,"greco")||!strcmp(w,"greca")||!strcmp(w,"greek")) hasgreco=true;
        if(!strcmp(w,"valore")||!strcmp(w,"vale")||!strcmp(w,"value")||!strcmp(w,"costante")||!strcmp(w,"constant")) hasval=true;
    }
    if(circW && !shape) shape=G_CIR;
    if(circW && !q)     q=Q_CIRC;

#define GEO_OK(...) do{ r->tier=ANIMA_TIER_COMMAND; r->action=ANIMA_ACT_ANSWER; r->confidence=95; \
    snprintf(r->intent,sizeof r->intent,"geo"); snprintf(r->state,sizeof r->state,"tool"); \
    snprintf(r->reply,sizeof r->reply,__VA_ARGS__); return true; }while(0)

    // The pi-value readout must answer a VALUE question ("quanto vale pi greco"), not any sentence that
    // merely names pi: "qual è l'ultima cifra di pi greco" (foreign possessive) or "in quale anno il pi
    // greco è andato in pensione" (a long false-premise frame). Require a value cue OR a short query, and
    // never fire under a reframing possessive — otherwise abstain rather than dumping the constant.
    if ((pigreco || (haspi && (hasgreco||hasval))) && !a_math_reframed(it, n) && (hasval || n <= 5))
        GEO_OK(en?"pi (π) = 3.14159265.":"Pi greco (π) = 3.14159265.");

    bool fire = (shape && (q||dimLabel||formula)) || (pyth && nn>=2);
    // No numbers to compute -> this would TEACH a symbolic formula. Suppress when the keyword is only
    // present via a foreign possessive ("il cane DI pitagora", "area ... OF a promise"): not a geo question.
    if (fire && nn == 0 && a_math_reframed(it, n)) return false;
    // A COUNT question with NO geometric count-noun ("how many children did the Pythagorean theorem have")
    // is not a geometry question — a real geo count always names sides/angles/faces/vertices/edges/diagonals.
    if (fire && nn == 0) {
        bool cnt = false;
        for (int i=0;i<n;i++){ const char*w=it[i].w; if(!strcmp(w,"quanti")||!strcmp(w,"quante")||!strcmp(w,"many")){ cnt=true; break; } }
        if (cnt) {
            bool gcount=false;
            for (int i=0;i<n && !gcount;i++){ const char*w=it[i].w;
                if(!strcmp(w,"lati")||!strcmp(w,"lato")||!strcmp(w,"side")||!strcmp(w,"sides")||
                   !strcmp(w,"angoli")||!strcmp(w,"angolo")||!strcmp(w,"angle")||!strcmp(w,"angles")||
                   !strcmp(w,"facce")||!strcmp(w,"faccia")||!strcmp(w,"face")||!strcmp(w,"faces")||
                   !strcmp(w,"vertici")||!strcmp(w,"vertice")||!strcmp(w,"vertex")||!strcmp(w,"vertices")||
                   !strcmp(w,"spigoli")||!strcmp(w,"spigolo")||!strcmp(w,"edge")||!strcmp(w,"edges")||
                   !strcmp(w,"diagonali")||!strcmp(w,"diagonale")||!strcmp(w,"diagonal")||!strcmp(w,"diagonals")) gcount=true; }
            if (!gcount) return false;
        }
    }
    if (!fire) { (void)formula; return false; }

    double R =a_dim(it,n,(const char*[]){"raggio","radius",NULL});
    double Dd=a_dim(it,n,(const char*[]){"diametro","diameter",NULL});
    double L =a_dim(it,n,(const char*[]){"lato","side","spigolo","edge",NULL});
    double Bb=a_dim(it,n,(const char*[]){"base",NULL});
    double Hh=a_dim(it,n,(const char*[]){"altezza","height","alta","alto",NULL});
    char x[32], y[32], o[32];

    if (shape==G_TRI && pyth) {
        double hk=a_dim(it,n,(const char*[]){"ipotenusa","hypotenuse",NULL});
        if (!isnan(hk) && nn>=2) {                          // hypotenuse known -> find the other leg
            double leg=NAN; for(int i=0;i<nn;i++) if(nums[i]!=hk){leg=nums[i];break;}
            if(isnan(leg)||leg>=hk) return false;
            a_fmt_num(hk,x,sizeof x); a_fmt_num(leg,y,sizeof y); a_fmt_num(sqrt(hk*hk-leg*leg),o,sizeof o);
            GEO_OK(en?"Leg = √(%s²-%s²) = %s.":"Cateto = √(%s²-%s²) = %s.", x,y,o);
        }
        if (nn>=2) {                                        // two legs -> hypotenuse
            a_fmt_num(nums[0],x,sizeof x); a_fmt_num(nums[1],y,sizeof y); a_fmt_num(sqrt(nums[0]*nums[0]+nums[1]*nums[1]),o,sizeof o);
            GEO_OK(en?"Hypotenuse = √(%s²+%s²) = %s.":"Ipotenusa = √(%s²+%s²) = %s.", x,y,o);
        }
        GEO_OK(en?"Pythagoras: c = √(a²+b²).":"Pitagora: c = √(a²+b²).");
    }

    if (shape==G_CIR) {
        double rad=R; if(isnan(rad)&&!isnan(Dd)) rad=Dd/2; if(isnan(rad)&&nn>=1) rad=nums[0];
        if(isnan(rad)){
            if(q==Q_PERIM||q==Q_CIRC) GEO_OK(en?"Circumference: C = 2·π·r.":"Circonferenza: C = 2·π·r.");
            if(q==Q_DIAM)             GEO_OK(en?"Diameter: d = 2·r.":"Diametro: d = 2·r.");
            GEO_OK(en?"Circle area: A = π·r².":"Area del cerchio: A = π·r²."); }
        a_fmt_num(rad,x,sizeof x);
        if(q==Q_PERIM||q==Q_CIRC){ a_fmt_num(2*A_PI*rad,o,sizeof o); GEO_OK(en?"Circumference = 2·π·%s = %s.":"Circonferenza = 2·π·%s = %s.", x,o); }
        if(q==Q_DIAM){ a_fmt_num(2*rad,o,sizeof o); GEO_OK(en?"Diameter = 2·%s = %s.":"Diametro = 2·%s = %s.", x,o); }
        if(q==Q_AREA){ a_fmt_num(A_PI*rad*rad,o,sizeof o); GEO_OK(en?"Circle area = π·%s² = %s.":"Area del cerchio = π·%s² = %s.", x,o); }
        a_fmt_num(A_PI*rad*rad,y,sizeof y); a_fmt_num(2*A_PI*rad,o,sizeof o);
        GEO_OK(en?"Circle r=%s: area %s, circumference %s.":"Cerchio r=%s: area %s, circonferenza %s.", x,y,o);
    }
    if (shape==G_SQ) {
        double s=L; if(isnan(s)&&nn>=1) s=nums[0];
        if(isnan(s)){
            if(q==Q_PERIM) GEO_OK(en?"Square perimeter: P = 4·l.":"Perimetro del quadrato: P = 4·l.");
            if(q==Q_DIAG)  GEO_OK(en?"Square diagonal: d = l·√2.":"Diagonale del quadrato: d = l·√2.");
            GEO_OK(en?"Square area: A = l².":"Area del quadrato: A = l²."); }
        a_fmt_num(s,x,sizeof x);
        if(q==Q_PERIM){ a_fmt_num(4*s,o,sizeof o); GEO_OK(en?"Perimeter = 4·%s = %s.":"Perimetro = 4·%s = %s.", x,o); }
        if(q==Q_DIAG){ a_fmt_num(s*sqrt(2),o,sizeof o); GEO_OK(en?"Diagonal = %s·√2 = %s.":"Diagonale = %s·√2 = %s.", x,o); }
        if(q==Q_AREA){ a_fmt_num(s*s,o,sizeof o); GEO_OK(en?"Square area = %s² = %s.":"Area del quadrato = %s² = %s.", x,o); }
        a_fmt_num(s*s,y,sizeof y); a_fmt_num(4*s,o,sizeof o);
        GEO_OK(en?"Square l=%s: area %s, perimeter %s.":"Quadrato l=%s: area %s, perimetro %s.", x,y,o);
    }
    if (shape==G_REC) {
        double b=Bb,h=Hh; if(isnan(b)&&nn>=1)b=nums[0]; if(isnan(h)&&nn>=2)h=nums[1];
        if(isnan(b)||isnan(h)){
            if(q==Q_PERIM) GEO_OK(en?"Rectangle perimeter: P = 2·(b+h).":"Perimetro del rettangolo: P = 2·(b+h).");
            if(q==Q_DIAG)  GEO_OK(en?"Rectangle diagonal: d = √(b²+h²).":"Diagonale del rettangolo: d = √(b²+h²).");
            GEO_OK(en?"Rectangle area: A = b·h.":"Area del rettangolo: A = b·h."); }
        a_fmt_num(b,x,sizeof x); a_fmt_num(h,y,sizeof y);
        if(q==Q_PERIM){ a_fmt_num(2*(b+h),o,sizeof o); GEO_OK(en?"Perimeter = 2·(%s+%s) = %s.":"Perimetro = 2·(%s+%s) = %s.", x,y,o); }
        if(q==Q_DIAG){ a_fmt_num(sqrt(b*b+h*h),o,sizeof o); GEO_OK(en?"Diagonal = √(%s²+%s²) = %s.":"Diagonale = √(%s²+%s²) = %s.", x,y,o); }
        if(q==Q_AREA){ a_fmt_num(b*h,o,sizeof o); GEO_OK(en?"Rectangle area = %s·%s = %s.":"Area del rettangolo = %s·%s = %s.", x,y,o); }
        a_fmt_num(b*h,o,sizeof o); GEO_OK(en?"Rectangle %s×%s: area %s.":"Rettangolo %s×%s: area %s.", x,y,o);
    }
    if (shape==G_TRI) {
        // THREE sides given -> Heron's formula, GUARDED by the triangle inequality. Sides that can't close
        // into a triangle get an HONEST "no such triangle" (the conscience), never a fabricated area. Gated
        // on an explicit side cue so "triangolo base 4 altezza 3" keeps the (b·h)/2 path below.
        bool sidecue=false;
        for(int i=0;i<n;i++) if(!it[i].isnum){ const char*w=it[i].w;
            if(!strcmp(w,"lati")||!strcmp(w,"lato")||!strcmp(w,"sides")||!strcmp(w,"side")) sidecue=true; }
        if (sidecue && nn>=3 && isnan(Bb) && isnan(Hh) && (q==Q_AREA||q==Q_PERIM||q==Q_NONE)) {
            double sA=nums[0], sB=nums[1], sC=nums[2];
            if (sA>0 && sB>0 && sC>0) {
                char sa[24],sb[24],sc[24]; a_fmt_num(sA,sa,sizeof sa); a_fmt_num(sB,sb,sizeof sb); a_fmt_num(sC,sc,sizeof sc);
                if (sA+sB<=sC || sA+sC<=sB || sB+sC<=sA) {
                    r->tier=ANIMA_TIER_COMMAND; r->action=ANIMA_ACT_ANSWER; r->confidence=90;
                    snprintf(r->intent,sizeof r->intent,"geo"); snprintf(r->state,sizeof r->state,"tool");
                    snprintf(r->reply,sizeof r->reply, en?
                        "No triangle has sides %s, %s and %s: the two shorter sides don't exceed the longest (triangle inequality).":
                        "Non esiste un triangolo di lati %s, %s e %s: la somma dei due lati minori non supera il maggiore (disuguaglianza triangolare).",
                        sa,sb,sc);
                    return true;
                }
                if (q==Q_PERIM){ char so[24]; a_fmt_num(sA+sB+sC,so,sizeof so);
                    GEO_OK(en?"Perimeter = %s+%s+%s = %s.":"Perimetro = %s+%s+%s = %s.", sa,sb,sc,so); }
                double sp=(sA+sB+sC)/2.0, ar=sqrt(sp*(sp-sA)*(sp-sB)*(sp-sC));
                char so[24]; a_fmt_num(ar,so,sizeof so);
                GEO_OK(en?"Triangle area (Heron) = %s.":"Area del triangolo (Erone) = %s.", so);
            }
        }
        double b=Bb,h=Hh; if(isnan(b)&&nn>=1)b=nums[0]; if(isnan(h)&&nn>=2)h=nums[1];
        if(isnan(b)||isnan(h)) GEO_OK(en?"Triangle area: A = (b·h)/2.":"Area del triangolo: A = (b·h)/2.");
        a_fmt_num(b,x,sizeof x); a_fmt_num(h,y,sizeof y); a_fmt_num(b*h/2,o,sizeof o);
        GEO_OK(en?"Triangle area = (%s·%s)/2 = %s.":"Area del triangolo = (%s·%s)/2 = %s.", x,y,o);
    }
    if (shape==G_TRAP) {
        double h=isnan(Hh)?(nn>=3?nums[2]:NAN):Hh;
        if(nn<3||isnan(h)) GEO_OK(en?"Trapezoid area: A = (B+b)·h/2.":"Area del trapezio: A = (B+b)·h/2.");
        a_fmt_num((nums[0]+nums[1])*h/2,o,sizeof o);
        a_fmt_num(nums[0],x,sizeof x); a_fmt_num(nums[1],y,sizeof y);
        GEO_OK(en?"Trapezoid area = (%s+%s)·h/2 = %s.":"Area del trapezio = (%s+%s)·h/2 = %s.", x,y,o);
    }
    if (shape==G_RHO) {
        if(q==Q_PERIM && !isnan(L)){ a_fmt_num(L,x,sizeof x); a_fmt_num(4*L,o,sizeof o); GEO_OK(en?"Perimeter = 4·%s = %s.":"Perimetro = 4·%s = %s.", x,o); }
        if(nn<2) GEO_OK(en?"Rhombus area: A = (d1·d2)/2.":"Area del rombo: A = (d1·d2)/2.");
        a_fmt_num(nums[0],x,sizeof x); a_fmt_num(nums[1],y,sizeof y); a_fmt_num(nums[0]*nums[1]/2,o,sizeof o);
        GEO_OK(en?"Rhombus area = (%s·%s)/2 = %s.":"Area del rombo = (%s·%s)/2 = %s.", x,y,o);
    }
    if (shape==G_PAR) {
        double b=Bb,h=Hh; if(isnan(b)&&nn>=1)b=nums[0]; if(isnan(h)&&nn>=2)h=nums[1];
        if(isnan(b)||isnan(h)) GEO_OK(en?"Parallelogram area: A = b·h.":"Area del parallelogramma: A = b·h.");
        a_fmt_num(b,x,sizeof x); a_fmt_num(h,y,sizeof y); a_fmt_num(b*h,o,sizeof o);
        GEO_OK(en?"Parallelogram area = %s·%s = %s.":"Area del parallelogramma = %s·%s = %s.", x,y,o);
    }
    if (shape==G_CUBE) {
        double s=L; if(isnan(s)&&nn>=1) s=nums[0];
        if(isnan(s)){ if((q==Q_SURF||q==Q_AREA)) GEO_OK(en?"Cube surface: S = 6·l².":"Superficie del cubo: S = 6·l²."); GEO_OK(en?"Cube volume: V = l³.":"Volume del cubo: V = l³."); }
        a_fmt_num(s,x,sizeof x);
        if((q==Q_SURF||q==Q_AREA)){ a_fmt_num(6*s*s,o,sizeof o); GEO_OK(en?"Cube surface = 6·%s² = %s.":"Superficie del cubo = 6·%s² = %s.", x,o); }
        a_fmt_num(s*s*s,o,sizeof o); GEO_OK(en?"Cube volume = %s³ = %s.":"Volume del cubo = %s³ = %s.", x,o);
    }
    if (shape==G_SPH) {
        double rad=R; if(isnan(rad)&&!isnan(Dd)) rad=Dd/2; if(isnan(rad)&&nn>=1) rad=nums[0];
        if(isnan(rad)){ if((q==Q_SURF||q==Q_AREA)) GEO_OK(en?"Sphere surface: S = 4·π·r².":"Superficie della sfera: S = 4·π·r²."); GEO_OK(en?"Sphere volume: V = (4/3)·π·r³.":"Volume della sfera: V = (4/3)·π·r³."); }
        a_fmt_num(rad,x,sizeof x);
        if((q==Q_SURF||q==Q_AREA)){ a_fmt_num(4*A_PI*rad*rad,o,sizeof o); GEO_OK(en?"Sphere surface = 4·π·%s² = %s.":"Superficie della sfera = 4·π·%s² = %s.", x,o); }
        a_fmt_num(4.0/3.0*A_PI*rad*rad*rad,o,sizeof o); GEO_OK(en?"Sphere volume = (4/3)·π·%s³ = %s.":"Volume della sfera = (4/3)·π·%s³ = %s.", x,o);
    }
    if (shape==G_CYL) {
        double rad=R; if(isnan(rad)&&nn>=1) rad=nums[0]; double h=Hh; if(isnan(h)&&nn>=2) h=nums[1];
        if(isnan(rad)||isnan(h)){ if((q==Q_SURF||q==Q_AREA)) GEO_OK(en?"Cylinder surface: S = 2·π·r·(r+h).":"Superficie del cilindro: S = 2·π·r·(r+h)."); GEO_OK(en?"Cylinder volume: V = π·r²·h.":"Volume del cilindro: V = π·r²·h."); }
        a_fmt_num(rad,x,sizeof x); a_fmt_num(h,y,sizeof y);
        if((q==Q_SURF||q==Q_AREA)){ a_fmt_num(2*A_PI*rad*(rad+h),o,sizeof o); GEO_OK(en?"Cylinder surface = 2·π·%s·(%s+%s) = %s.":"Superficie cilindro = 2·π·%s·(%s+%s) = %s.", x,x,y,o); }
        a_fmt_num(A_PI*rad*rad*h,o,sizeof o); GEO_OK(en?"Cylinder volume = π·%s²·%s = %s.":"Volume del cilindro = π·%s²·%s = %s.", x,y,o);
    }
    if (shape==G_CONE) {
        double rad=R; if(isnan(rad)&&nn>=1) rad=nums[0]; double h=Hh; if(isnan(h)&&nn>=2) h=nums[1];
        if(isnan(rad)||isnan(h)) GEO_OK(en?"Cone volume: V = (1/3)·π·r²·h.":"Volume del cono: V = (1/3)·π·r²·h.");
        a_fmt_num(rad,x,sizeof x); a_fmt_num(h,y,sizeof y); a_fmt_num(A_PI*rad*rad*h/3,o,sizeof o);
        GEO_OK(en?"Cone volume = (1/3)·π·%s²·%s = %s.":"Volume del cono = (1/3)·π·%s²·%s = %s.", x,y,o);
    }
#undef GEO_OK
    return false;
}

// ============================================================================
// PHYSICS skill — the core formulas of mechanics plus a little fluids/waves:
// speed v=s/t, Newton F=m·a, weight P=m·g, kinetic ½mv², potential m·g·h,
// momentum m·v, density m/V, pressure F/A, work F·s, power L/t, frequency 1/T.
// REARRANGES to the missing quantity, bilingual, COMPUTE or TEACH. Gated on a
// physics keyword + labelled values, so ordinary words ("ho trovato lavoro",
// "forza juve", "velocità della luce") never misfire. (Electrical V/I/R/P stays
// with a_solve_ohm, which runs after this and owns the unit-tagged queries.)
// ============================================================================
#ifndef A_G
#define A_G 9.81
#endif

// Physics unit roles. Base dims (LEN/TIME/MASS/AREA/VOL) only SCALE a number and bind it by an
// adjacent label or by their dimension; derived-quantity units (>= PU_VEL) are DECISIVE — a "m/s"
// is a velocity no matter the wording. The multiplier maps the number to the SI base quantity.
enum { PU_NONE = 0, PU_LEN, PU_TIME, PU_MASS, PU_AREA, PU_VOL,
       PU_VEL, PU_ACC, PU_FORCE, PU_ENERGY, PU_POWER, PU_PRESS, PU_FREQ, PU_DENS };
static int phys_unit(const char *w, double *si)
{
    static const struct { const char *u; int r; double f; } U[] = {
        {"mm",PU_LEN,0.001},{"cm",PU_LEN,0.01},{"dm",PU_LEN,0.1},{"m",PU_LEN,1},{"metro",PU_LEN,1},{"metri",PU_LEN,1},
        {"meter",PU_LEN,1},{"meters",PU_LEN,1},{"metre",PU_LEN,1},{"metres",PU_LEN,1},
        {"km",PU_LEN,1000},{"chilometro",PU_LEN,1000},{"chilometri",PU_LEN,1000},{"kilometer",PU_LEN,1000},{"kilometers",PU_LEN,1000},
        {"s",PU_TIME,1},{"sec",PU_TIME,1},{"secondo",PU_TIME,1},{"secondi",PU_TIME,1},{"second",PU_TIME,1},{"seconds",PU_TIME,1},
        {"min",PU_TIME,60},{"minuto",PU_TIME,60},{"minuti",PU_TIME,60},{"minute",PU_TIME,60},{"minutes",PU_TIME,60},
        {"h",PU_TIME,3600},{"ora",PU_TIME,3600},{"ore",PU_TIME,3600},{"hour",PU_TIME,3600},{"hours",PU_TIME,3600},
        {"mg",PU_MASS,1e-6},{"g",PU_MASS,0.001},{"grammo",PU_MASS,0.001},{"grammi",PU_MASS,0.001},{"gram",PU_MASS,0.001},{"grams",PU_MASS,0.001},
        {"kg",PU_MASS,1},{"chilo",PU_MASS,1},{"chili",PU_MASS,1},{"chilogrammo",PU_MASS,1},{"chilogrammi",PU_MASS,1},
        {"kilogram",PU_MASS,1},{"kilograms",PU_MASS,1},{"tonnellata",PU_MASS,1000},{"tonnellate",PU_MASS,1000},{"ton",PU_MASS,1000},
        {"m2",PU_AREA,1},{"mq",PU_AREA,1},{"cm2",PU_AREA,0.0001},{"dm2",PU_AREA,0.01},
        {"m3",PU_VOL,1},{"dm3",PU_VOL,0.001},{"cm3",PU_VOL,1e-6},{"litro",PU_VOL,0.001},{"litri",PU_VOL,0.001},{"l",PU_VOL,0.001},
        {"m/s",PU_VEL,1},{"km/h",PU_VEL,1.0/3.6},{"kmh",PU_VEL,1.0/3.6},{"kph",PU_VEL,1.0/3.6},{"mph",PU_VEL,0.44704},
        {"m/s2",PU_ACC,1},{"ms2",PU_ACC,1},
        {"n",PU_FORCE,1},{"newton",PU_FORCE,1},{"newtons",PU_FORCE,1},{"kn",PU_FORCE,1000},
        {"j",PU_ENERGY,1},{"joule",PU_ENERGY,1},{"joules",PU_ENERGY,1},{"kj",PU_ENERGY,1000},
        {"w",PU_POWER,1},{"watt",PU_POWER,1},{"watts",PU_POWER,1},{"kw",PU_POWER,1000},
        {"pa",PU_PRESS,1},{"pascal",PU_PRESS,1},{"kpa",PU_PRESS,1000},{"hpa",PU_PRESS,100},{"bar",PU_PRESS,100000},{"atm",PU_PRESS,101325},
        {"hz",PU_FREQ,1},{"hertz",PU_FREQ,1},{"khz",PU_FREQ,1000},{"mhz",PU_FREQ,1e6},
        {"kg/m3",PU_DENS,1},{"g/cm3",PU_DENS,1000},{"g/l",PU_DENS,1},
    };
    for (size_t i = 0; i < sizeof(U)/sizeof(U[0]); i++)
        if (!strcmp(w, U[i].u)) { if (si) *si = U[i].f; return U[i].r; }
    return PU_NONE;
}

// Which physics variable a label word names (the number it sits next to fills that variable).
enum { L_NONE = 0, L_V, L_S, L_T, L_M, L_AC, L_F, L_H, L_LV, L_DN, L_PR, L_FR, L_TP, L_EN, L_VV, L_AR };
static int phys_label(const char *w)
{
    if(!strcmp(w,"velocita")||!strcmp(w,"speed")||!strcmp(w,"velocity")||!strcmp(w,"celerita")) return L_V;
    if(!strcmp(w,"spazio")||!strcmp(w,"distanza")||!strcmp(w,"distance")||!strcmp(w,"space")||
       !strcmp(w,"spostamento")||!strcmp(w,"displacement")||!strcmp(w,"percorso")||!strcmp(w,"tragitto")) return L_S;
    if(!strcmp(w,"tempo")||!strcmp(w,"time")||!strcmp(w,"durata")) return L_T;
    if(!strcmp(w,"massa")||!strcmp(w,"mass")) return L_M;
    if(!strcmp(w,"accelerazione")||!strcmp(w,"acceleration")) return L_AC;
    if(!strcmp(w,"forza")||!strcmp(w,"force")) return L_F;
    if(!strcmp(w,"altezza")||!strcmp(w,"height")||!strcmp(w,"quota")||!strcmp(w,"alto")||
       !strcmp(w,"alta")||!strcmp(w,"altitudine")) return L_H;
    if(!strcmp(w,"lavoro")||!strcmp(w,"work")) return L_LV;
    if(!strcmp(w,"densita")||!strcmp(w,"density")) return L_DN;
    if(!strcmp(w,"pressione")||!strcmp(w,"pressure")) return L_PR;
    if(!strcmp(w,"frequenza")||!strcmp(w,"frequency")) return L_FR;
    if(!strcmp(w,"periodo")||!strcmp(w,"period")) return L_TP;
    if(!strcmp(w,"energia")||!strcmp(w,"energy")) return L_EN;
    if(!strcmp(w,"volume")) return L_VV;
    if(!strcmp(w,"area")||!strcmp(w,"superficie")||!strcmp(w,"surface")) return L_AR;
    return L_NONE;
}
// The base dimension a label expects (0 for derived quantities). Used to reconcile a label against
// an attached base unit: "densità di 100 kg" — the label says density but kg is a mass, so the
// number is the mass; the dimension wins.
static int phys_label_dim(int lbl)
{
    switch(lbl){
        case L_S: case L_H: return PU_LEN;
        case L_T: case L_TP: return PU_TIME;
        case L_M:           return PU_MASS;
        case L_AR:          return PU_AREA;
        case L_VV:          return PU_VOL;
        default:            return 0;
    }
}
static bool a_is_conn(const char *w)
{
    static const char *const C[] = {"di","of","con","with","del","dello","della",NULL};
    for (int i = 0; C[i]; i++) if (!strcmp(w, C[i])) return true;
    return false;
}

static bool a_solve_physics(const a_sitem_t *it, int n, bool en, anima_result_t *r)
{
    bool kVel=0,kSpace=0,kTime=0,kMass=0,kAcc=0,kForce=0,kHeight=0,kWork=0,kPower=0,
         kWeight=0,kDens=0,kPress=0,kFreq=0,kPeriod=0,kKin=0,kPot=0,kEnergy=0,
         hasQ=0,hasMoto=0,hasMom=0,formula=0;
    // electrical context: "potenza" with volt/ampere/ohm is Ohm's law, NOT mechanical power — physics
    // defers so a_solve_ohm owns it (collaboration, not collision). "watt" alone stays mechanical.
    bool elec=false;
    for (int i=0;i<n;i++) if(!it[i].isnum){ const char*w=it[i].w;
        if(strstr(w,"volt")||strstr(w,"amper")||strstr(w,"ohm")||strstr(w,"tension")||strstr(w,"corrent")||strstr(w,"resisten")) elec=true; }
    for (int i=0;i<n;i++){
        if(it[i].isnum) continue;                    // physics binds by label (a_dim), not positionally
        const char*w=it[i].w;
        if(!strcmp(w,"velocita")||!strcmp(w,"speed")||!strcmp(w,"velocity")||!strcmp(w,"celerita")) kVel=1;
        if(!strcmp(w,"spazio")||!strcmp(w,"distanza")||!strcmp(w,"distance")||!strcmp(w,"space")||
           !strcmp(w,"spostamento")||!strcmp(w,"displacement")||!strcmp(w,"percorso")||!strcmp(w,"tragitto")) kSpace=1;
        if(!strcmp(w,"tempo")||!strcmp(w,"time")||!strcmp(w,"durata")) kTime=1;
        if(!strcmp(w,"massa")||!strcmp(w,"mass")) kMass=1;
        if(!strcmp(w,"accelerazione")||!strcmp(w,"acceleration")||!strcmp(w,"accelera")) kAcc=1;
        if(!strcmp(w,"forza")||!strcmp(w,"force")) kForce=1;
        if(!strcmp(w,"altezza")||!strcmp(w,"height")||!strcmp(w,"quota")||!strcmp(w,"alto")||
           !strcmp(w,"alta")||!strcmp(w,"altitudine")) kHeight=1;
        if(!strcmp(w,"lavoro")||!strcmp(w,"work")) kWork=1;
        if((!strcmp(w,"potenza")||!strcmp(w,"power")) && !elec) kPower=1;   // electrical power -> a_solve_ohm
        if(!strcmp(w,"peso")||!strcmp(w,"pesa")||!strcmp(w,"pesano")||!strcmp(w,"weight")||!strcmp(w,"weighs")||!strcmp(w,"weigh")) kWeight=1;
        if(!strcmp(w,"densita")||!strcmp(w,"density")) kDens=1;
        if(!strcmp(w,"pressione")||!strcmp(w,"pressure")) kPress=1;
        if(!strcmp(w,"frequenza")||!strcmp(w,"frequency")) kFreq=1;
        if(!strcmp(w,"periodo")||!strcmp(w,"period")) kPeriod=1;
        if(!strcmp(w,"energia")||!strcmp(w,"energy")) kEnergy=1;
        if(!strcmp(w,"cinetica")||!strcmp(w,"kinetic")) kKin=1;
        if(!strcmp(w,"potenziale")||!strcmp(w,"potential")) kPot=1;
        if(!strcmp(w,"quantita")) hasQ=1;
        if(!strcmp(w,"moto")) hasMoto=1;
        if(!strcmp(w,"momentum")) hasMom=1;
        if(!strcmp(w,"formula")||!strcmp(w,"formule")||!strcmp(w,"regola")||!strcmp(w,"rule")||
           !strcmp(w,"calcola")||!strcmp(w,"calcolare")||!strcmp(w,"calcolo")||!strcmp(w,"calculate")) formula=1;
    }
    bool kMom = hasMom || (hasQ && hasMoto);

    // Value extraction (everything normalised to SI). For each number: a derived-quantity unit on
    // its right is decisive; otherwise a base unit scales it and an adjacent label (before, or after
    // the unit — "5 metri di altezza") names the variable, falling back to the unit's own dimension.
    double v=NAN,s=NAN,t=NAN,m=NAN,ac=NAN,F=NAN,h=NAN,Lv=NAN,Dn=NAN,Pr=NAN,
           Fr=NAN,Tp=NAN,En=NAN,Vv=NAN,Ar=NAN,Pw=NAN;
#define SETF(var,val) do{ if(isnan(var)) (var)=(val); }while(0)
    for (int i=0;i<n;i++){
        if(!it[i].isnum) continue;
        double x=it[i].val, usi=1; int urole=PU_NONE;
        if(i+1<n && !it[i+1].isnum) urole=phys_unit(it[i+1].w,&usi);
        int lbl=L_NONE;
        if(i>=1 && !it[i-1].isnum){
            lbl=phys_label(it[i-1].w);
            if(!lbl && a_is_conn(it[i-1].w) && i>=2 && !it[i-2].isnum) lbl=phys_label(it[i-2].w);
        }
        if(!lbl && urole!=PU_NONE && i+2<n && !it[i+2].isnum){
            lbl=phys_label(it[i+2].w);
            if(!lbl && a_is_conn(it[i+2].w) && i+3<n && !it[i+3].isnum) lbl=phys_label(it[i+3].w);
        }
        if(lbl && urole>PU_NONE && urole<PU_VEL && phys_label_dim(lbl)!=urole) lbl=L_NONE;  // base unit overrides a wrong-dimension label
        if(urole>=PU_VEL){                               // derived-quantity unit -> decisive
            switch(urole){
                case PU_VEL:   SETF(v,x*usi);  kVel=1;   break;
                case PU_ACC:   SETF(ac,x*usi); kAcc=1;   break;
                case PU_FORCE: SETF(F,x*usi);  kForce=1; break;
                case PU_ENERGY:SETF(En,x*usi); kEnergy=1;break;
                case PU_POWER: SETF(Pw,x*usi); kPower=1; break;
                case PU_PRESS: SETF(Pr,x*usi); kPress=1; break;
                case PU_FREQ:  SETF(Fr,x*usi); kFreq=1;  break;
                case PU_DENS:  SETF(Dn,x*usi); kDens=1;  break;
            }
        } else {
            double sf = (urole!=PU_NONE)? usi : 1.0;
            if(lbl){
                switch(lbl){
                    case L_V: SETF(v,x*sf);  break;  case L_S: SETF(s,x*sf);  break;
                    case L_T: SETF(t,x*sf);  break;  case L_M: SETF(m,x*sf);  break;
                    case L_AC:SETF(ac,x*sf); break;  case L_F: SETF(F,x*sf);  break;
                    case L_H: SETF(h,x*sf);  break;  case L_LV:SETF(Lv,x*sf); break;
                    case L_DN:SETF(Dn,x*sf); break;  case L_PR:SETF(Pr,x*sf); break;
                    case L_FR:SETF(Fr,x*sf); break;  case L_TP:SETF(Tp,x*sf); break;
                    case L_EN:SETF(En,x*sf); break;  case L_VV:SETF(Vv,x*sf); break;
                    case L_AR:SETF(Ar,x*sf); break;
                }
            } else if(urole!=PU_NONE){                   // bare base unit -> bind by its dimension
                switch(urole){
                    case PU_LEN:  SETF(s,x*usi);  kSpace=1; break;
                    case PU_TIME: SETF(t,x*usi);  kTime=1;  break;
                    case PU_MASS: SETF(m,x*usi);  kMass=1;  break;
                    case PU_AREA: SETF(Ar,x*usi);          break;
                    case PU_VOL:  SETF(Vv,x*usi);          break;
                }
            }
        }
    }
#undef SETF
    char b1[32],b2[32],b3[32];

#define PHY_OK(...) do{ r->tier=ANIMA_TIER_COMMAND; r->action=ANIMA_ACT_ANSWER; r->confidence=93; \
    snprintf(r->intent,sizeof r->intent,"phys"); snprintf(r->state,sizeof r->state,"tool"); \
    snprintf(r->reply,sizeof r->reply,__VA_ARGS__); return true; }while(0)

    // -- COMPUTE: try every equation first. A query like "calcola l'energia cinetica ... a 10 m/s"
    //    sets BOTH kVel (the m/s) and kKin; if teach were inline, the velocity branch would shadow
    //    the kinetic computation. So all teach (formula) fallbacks are deferred to the end. --
    if (kWeight) {                                   // P = m·g  (g = 9.81 m/s²); from a mass, or solve mass from a weight in N
        if(!isnan(m)){ a_fmt_num(m,b1,32); a_fmt_num(m*A_G,b3,32); PHY_OK("P = m·g = %s·9.81 = %s N.", b1,b3); }
        if(!isnan(F)){ a_fmt_num(F,b1,32); a_fmt_num(F/A_G,b3,32); PHY_OK("m = P/g = %s/9.81 = %s kg.", b1,b3); }
    }
    if (kVel || (kSpace && kTime)) {                 // v = s/t  (rearranges)
        int known=(!isnan(v))+(!isnan(s))+(!isnan(t));
        if(known>=2){
            if(isnan(v)&&t!=0){ a_fmt_num(s,b1,32);a_fmt_num(t,b2,32);a_fmt_num(s/t,b3,32); PHY_OK("v = s/t = %s/%s = %s m/s.", b1,b2,b3); }
            if(isnan(s)){ a_fmt_num(v,b1,32);a_fmt_num(t,b2,32);a_fmt_num(v*t,b3,32); PHY_OK("s = v·t = %s·%s = %s m.", b1,b2,b3); }
            if(isnan(t)&&v!=0){ a_fmt_num(s,b1,32);a_fmt_num(v,b2,32);a_fmt_num(s/v,b3,32); PHY_OK("t = s/v = %s/%s = %s s.", b1,b2,b3); }
        }
    }
    if (kForce || (kMass && kAcc)) {                 // F = m·a  (rearranges)
        int known=(!isnan(F))+(!isnan(m))+(!isnan(ac));
        if(known>=2){
            if(isnan(F)){ a_fmt_num(m,b1,32);a_fmt_num(ac,b2,32);a_fmt_num(m*ac,b3,32); PHY_OK("F = m·a = %s·%s = %s N.", b1,b2,b3); }
            if(isnan(m)&&ac!=0){ a_fmt_num(F,b1,32);a_fmt_num(ac,b2,32);a_fmt_num(F/ac,b3,32); PHY_OK("m = F/a = %s/%s = %s kg.", b1,b2,b3); }
            if(isnan(ac)&&m!=0){ a_fmt_num(F,b1,32);a_fmt_num(m,b2,32);a_fmt_num(F/m,b3,32); PHY_OK("a = F/m = %s/%s = %s m/s².", b1,b2,b3); }
        }
    }
    if (kKin || (kEnergy && kVel && !kPot)) {        // Ec = ½·m·v²
        if(!isnan(m)&&!isnan(v)){ a_fmt_num(m,b1,32);a_fmt_num(v,b2,32);a_fmt_num(0.5*m*v*v,b3,32); PHY_OK("Ec = ½·m·v² = ½·%s·%s² = %s J.", b1,b2,b3); }
        if(!isnan(En)&&!isnan(m)&&m!=0){ a_fmt_num(En,b1,32);a_fmt_num(m,b2,32);a_fmt_num(sqrt(2*En/m),b3,32); PHY_OK("v = √(2·Ec/m) = √(2·%s/%s) = %s m/s.", b1,b2,b3); }
    }
    if (kPot || (kEnergy && kHeight)) {              // Ep = m·g·h  (a lone length in a PE problem is the height)
        double hh = !isnan(h)?h:s;
        if(!isnan(m)&&!isnan(hh)){ a_fmt_num(m,b1,32);a_fmt_num(hh,b2,32);a_fmt_num(m*A_G*hh,b3,32); PHY_OK("Ep = m·g·h = %s·9.81·%s = %s J.", b1,b2,b3); }
        if(!isnan(En)&&!isnan(m)&&m!=0){ a_fmt_num(En,b1,32);a_fmt_num(m,b2,32);a_fmt_num(En/(m*A_G),b3,32); PHY_OK("h = Ep/(m·g) = %s/(%s·9.81) = %s m.", b1,b2,b3); }
    }
    if (kMom) {                                      // q = m·v
        if(!isnan(m)&&!isnan(v)){ a_fmt_num(m,b1,32);a_fmt_num(v,b2,32);a_fmt_num(m*v,b3,32); PHY_OK("q = m·v = %s·%s = %s kg·m/s.", b1,b2,b3); }
    }
    if (kDens) {                                     // ρ = m/V  (rearranges)
        if(!isnan(m)&&!isnan(Vv)&&Vv!=0){ a_fmt_num(m,b1,32);a_fmt_num(Vv,b2,32);a_fmt_num(m/Vv,b3,32); PHY_OK("ρ = m/V = %s/%s = %s kg/m³.", b1,b2,b3); }
        if(!isnan(Dn)&&!isnan(Vv)){ a_fmt_num(Dn,b1,32);a_fmt_num(Vv,b2,32);a_fmt_num(Dn*Vv,b3,32); PHY_OK("m = ρ·V = %s·%s = %s kg.", b1,b2,b3); }
        if(!isnan(Dn)&&!isnan(m)&&Dn!=0){ a_fmt_num(m,b1,32);a_fmt_num(Dn,b2,32);a_fmt_num(m/Dn,b3,32); PHY_OK("V = m/ρ = %s/%s = %s m³.", b1,b2,b3); }
    }
    if (kPress) {                                    // p = F/A  (rearranges)
        if(!isnan(F)&&!isnan(Ar)&&Ar!=0){ a_fmt_num(F,b1,32);a_fmt_num(Ar,b2,32);a_fmt_num(F/Ar,b3,32); PHY_OK("p = F/A = %s/%s = %s Pa.", b1,b2,b3); }
        if(!isnan(Pr)&&!isnan(Ar)){ a_fmt_num(Pr,b1,32);a_fmt_num(Ar,b2,32);a_fmt_num(Pr*Ar,b3,32); PHY_OK("F = p·A = %s·%s = %s N.", b1,b2,b3); }
    }
    if (kWork) {                                     // L = F·s
        if(!isnan(F)&&!isnan(s)){ a_fmt_num(F,b1,32);a_fmt_num(s,b2,32);a_fmt_num(F*s,b3,32); PHY_OK("L = F·s = %s·%s = %s J.", b1,b2,b3); }
    }
    if (kPower) {                                    // P = L/t (work/energy over time); or E = P·t given a power and a time
        double wk = !isnan(Lv)?Lv:En;
        if(!isnan(wk)&&!isnan(t)&&t!=0){ a_fmt_num(wk,b1,32);a_fmt_num(t,b2,32);a_fmt_num(wk/t,b3,32); PHY_OK("P = L/t = %s/%s = %s W.", b1,b2,b3); }
        if(!isnan(Pw)&&!isnan(t)){ a_fmt_num(Pw,b1,32);a_fmt_num(t,b2,32);a_fmt_num(Pw*t,b3,32); PHY_OK("E = P·t = %s·%s = %s J.", b1,b2,b3); }
    }
    if (kFreq || kPeriod) {                          // f = 1/T  /  T = 1/f
        if(!isnan(Fr)&&Fr!=0){ a_fmt_num(Fr,b1,32);a_fmt_num(1/Fr,b3,32); PHY_OK("T = 1/f = 1/%s = %s s.", b1,b3); }
        if(!isnan(Tp)&&Tp!=0){ a_fmt_num(Tp,b1,32);a_fmt_num(1/Tp,b3,32); PHY_OK("f = 1/T = 1/%s = %s Hz.", b1,b3); }
    }
    // -- TEACH: no values to compute -> return the symbolic formula. Specific quantities first.
    //    Suppressed in an electrical context so ohm/units (not a mechanical-formula teach) answer.
    //    Also suppressed when a foreign possessive reframes the keyword ("la velocità DEL pensiero"). --
    if (formula && !elec && !a_math_reframed(it, n)) {
        if (kWeight)                          PHY_OK(en?"Weight: P = m·g  (g = 9.81 m/s²).":"Peso: P = m·g  (g = 9.81 m/s²).");
        if (kKin || (kEnergy && !kPot && !kHeight)) PHY_OK(en?"Kinetic energy: Ec = ½·m·v².":"Energia cinetica: Ec = ½·m·v².");
        if (kPot || (kEnergy && kHeight))     PHY_OK(en?"Potential energy: Ep = m·g·h.":"Energia potenziale: Ep = m·g·h.");
        if (kMom)                             PHY_OK(en?"Momentum: q = m·v.":"Quantità di moto: q = m·v.");
        if (kDens)                            PHY_OK(en?"Density: ρ = m/V.":"Densità: ρ = m/V.");
        if (kPress)                           PHY_OK(en?"Pressure: p = F/A.":"Pressione: p = F/A.");
        if (kWork)                            PHY_OK(en?"Work: L = F·s.":"Lavoro: L = F·s.");
        if (kPower)                           PHY_OK(en?"Power: P = work / time.":"Potenza: P = lavoro / tempo.");
        if (kForce || (kMass && kAcc))        PHY_OK(en?"Newton's 2nd law: F = m·a.":"2ª legge di Newton: F = m·a.");
        if (kFreq || kPeriod)                 PHY_OK(en?"Frequency: f = 1/T.":"Frequenza: f = 1/T.");
        if (kVel || (kSpace && kTime))        PHY_OK(en?"Speed: v = distance / time.":"Velocità: v = spazio / tempo.");
    }
#undef PHY_OK
    return false;
}

// ============================================================================
// VECTOR skill — magnitude |v| = √(Σxᵢ²) and dot product v·w = Σxᵢyᵢ over the
// space-separated components. Gated on "vettore"/"vector" so "modulo" stays the
// mod operator and "prodotto" stays scalar multiply. Bilingual, flash-resident.
// ============================================================================
static bool a_solve_vector(const a_sitem_t *it, int n, bool en, anima_result_t *r)
{
    bool vec=false, dot=false; double nums[12]; int nn=0;
    for (int i=0;i<n;i++){
        if(it[i].isnum){ if(nn<12) nums[nn++]=it[i].val; continue; }
        const char*w=it[i].w;
        if(!strcmp(w,"vettore")||!strcmp(w,"vettori")||!strcmp(w,"vector")||!strcmp(w,"vectors")) vec=true;
        if(!strcmp(w,"scalare")||!strcmp(w,"dot")) dot=true;
    }
    if(!vec || nn<2) return false;
    char form[200]; int p=0; char o[40];
    r->tier=ANIMA_TIER_COMMAND; r->action=ANIMA_ACT_ANSWER; r->confidence=94;
    snprintf(r->intent,sizeof r->intent,"calc"); snprintf(r->state,sizeof r->state,"tool");
    if(dot && nn%2==0){                                   // two vectors -> scalar (dot) product
        int half=nn/2; double s=0; for(int i=0;i<half;i++) s+=nums[i]*nums[i+half];
        form[0]=0; p=0;
        for(int i=0;i<half;i++){ int rem=(int)sizeof(form)-p; if(rem<=8) break;
            char a[24],b[24]; a_fmt_num(nums[i],a,sizeof a); a_fmt_num(nums[i+half],b,sizeof b);
            p+=snprintf(form+p,(size_t)rem,"%s%s·%s", i?"+":"", a,b); }
        a_fmt_num(s,o,sizeof o);
        snprintf(r->reply,sizeof r->reply, en?"v·w = %s = %s.":"Prodotto scalare = %s = %s.", form, o);
        return true;
    }
    double s=0; for(int i=0;i<nn;i++) s+=nums[i]*nums[i];   // magnitude (Euclidean norm)
    form[0]=0; p=0;
    for(int i=0;i<nn;i++){ int rem=(int)sizeof(form)-p; if(rem<=6) break;
        char a[24]; a_fmt_num(nums[i],a,sizeof a); p+=snprintf(form+p,(size_t)rem,"%s%s²", i?"+":"", a); }
    a_fmt_num(sqrt(s),o,sizeof o);
    snprintf(r->reply,sizeof r->reply, en?"|v| = √(%s) = %s.":"Modulo |v| = √(%s) = %s.", form, o);
    return true;
}

// ============================================================================
// SPREADSHEET skill — Natural language to Excel formula compiler.
// Supports operations over specific cells (A1), ranges (A1:B10) or columns (C:C).
// ============================================================================
static bool a_is_cell(const char *w) {
    if (w[0] < 'a' || w[0] > 'z') return false;
    if (!w[1] || !isdigit((unsigned char)w[1])) return false;
    for (int i=2; w[i]; i++) if (!isdigit((unsigned char)w[i])) return false;
    int row = atoi(w+1);
    return row > 0 && row <= 10000;
}

// Is `cell` (e.g. "X12") FUSED to a preceding word in the raw query ("Frobnicator-X12")? a_norm_solve turns
// the hyphen into a space, so by item time "x12" looks like a standalone cell — but it is really an entity-
// name suffix and must NOT trigger a fabricated formula. A genuine cell ("vlookup A1") is space/start-bounded.
static bool a_cell_fused(const char *raw, const char *cell)
{
    char low[200]; int o = 0;
    for (const unsigned char *p = (const unsigned char *)raw; *p && o < 199; p++) low[o++] = (char)tolower(*p);
    low[o] = 0;
    char cl[10]; int k = 0; for (; cell[k] && k < 9; k++) cl[k] = (char)tolower((unsigned char)cell[k]); cl[k] = 0;
    if (!cl[0]) return false;
    for (const char *q = strstr(low, cl); q; q = strstr(q + 1, cl)) {
        if (isalnum((unsigned char)q[strlen(cl)])) continue;        // a longer token, not this cell
        if (q == low) return false;                                 // at the start -> standalone
        char pv = q[-1];
        return (pv == '-' || isalpha((unsigned char)pv));           // glued to a word/hyphen -> entity suffix
    }
    return false;
}

static bool a_solve_spreadsheet(const char *raw, const a_sitem_t *it, int n, bool en, anima_result_t *r)
{
    bool is_spread = false;
    const char *fn = NULL;
    char cells[4][10]; int nc = 0;
    bool has_da = false, has_a = false, is_col = false, has_se = false;
    char col_letter = 0;
    double const_val = NAN;
    
    for (int i=0; i<n; i++) {
        if (it[i].isnum) { 
            if (isnan(const_val)) const_val = it[i].val; 
            continue; 
        }
        const char *w = it[i].w;
        if (!strcmp(w,"excel")||!strcmp(w,"foglio")||!strcmp(w,"spreadsheet")||!strcmp(w,"cella")||!strcmp(w,"celle")||!strcmp(w,"cell"))
            is_spread = true;

        if (!fn) {
            if (!strcmp(w,"somma")||!strcmp(w,"totale")||!strcmp(w,"addiziona")||!strcmp(w,"sum")||
                !strcmp(w,"sommo")||!strcmp(w,"sommare")||!strcmp(w,"sommami")||!strcmp(w,"totalizza")||
                !strcmp(w,"totalizzare")||!strcmp(w,"addizionare")||!strcmp(w,"sumif")) fn = "SUM";
            else if (!strcmp(w,"media")||!strcmp(w,"average")||!strcmp(w,"avg")||!strcmp(w,"mean")) fn = "AVERAGE";
            else if (!strcmp(w,"massimo")||!strcmp(w,"massima")||!strcmp(w,"max")||!strcmp(w,"maximum")) fn = "MAX";
            else if (!strcmp(w,"minimo")||!strcmp(w,"minima")||!strcmp(w,"min")||!strcmp(w,"minimum")) fn = "MIN";
            else if (!strcmp(w,"conta")||!strcmp(w,"quante")||!strcmp(w,"count")||!strcmp(w,"conteggio")||!strcmp(w,"countif")) fn = "COUNT";
            else if (!strcmp(w,"concatena")||!strcmp(w,"unisci")||!strcmp(w,"concat")||!strcmp(w,"concatenate")||!strcmp(w,"concatenare")) fn = "CONCAT";
            else if (!strcmp(w,"arrotonda")||!strcmp(w,"round")) fn = "ROUND";
            else if (!strcmp(w,"prodotto")||!strcmp(w,"product")) fn = "PRODUCT";
            else if (!strcmp(w,"mediana")||!strcmp(w,"median")) fn = "MEDIAN";
            else if (!strcmp(w,"radice")||!strcmp(w,"sqrt")||!strcmp(w,"root")) fn = "SQRT";
            else if (!strcmp(w,"potenza")||!strcmp(w,"power")||!strcmp(w,"elevato")) fn = "POWER";
            else if (!strcmp(w,"resto")||!strcmp(w,"modulo")||!strcmp(w,"mod")) fn = "MOD";
            else if (!strcmp(w,"lunghezza")||!strcmp(w,"len")||!strcmp(w,"length")) fn = "LEN";
            else if (!strcmp(w,"trim")||!strcmp(w,"spazi")||!strcmp(w,"spaces")) fn = "TRIM";   // remove spaces
            else if (!strcmp(w,"maiuscolo")||!strcmp(w,"upper")||!strcmp(w,"maiuscole")||!strcmp(w,"uppercase")) fn = "UPPER";
            else if (!strcmp(w,"minuscolo")||!strcmp(w,"lower")||!strcmp(w,"minuscole")||!strcmp(w,"lowercase")) fn = "LOWER";
            else if (!strcmp(w,"cerca")||!strcmp(w,"vlookup")||!strcmp(w,"cercavert")) fn = "VLOOKUP";
            else if (!strcmp(w,"se")||!strcmp(w,"if")||!strcmp(w,"condizione")) fn = "IF";
        }

        // fused Excel names "sumif"/"countif" imply the conditional variant; "somma SE" splits into two.
        if (!strcmp(w,"se")||!strcmp(w,"if")||!strcmp(w,"quando")||!strcmp(w,"sumif")||!strcmp(w,"countif")) has_se = true;
        if (!strcmp(w,"da")||!strcmp(w,"from")) has_da = true;
        if (!strcmp(w,"a")||!strcmp(w,"to")||!strcmp(w,"al")) has_a = true;
        if (!strcmp(w,"colonna")||!strcmp(w,"column")) is_col = true;
        
        if (is_col && strlen(w)==1 && w[0]>='a' && w[0]<='z') col_letter = w[0];
        
        if (a_is_cell(w) && nc < 4 && !a_cell_fused(raw, w)) {     // skip "X12" lifted from "Frobnicator-X12"
            int k=0;
            while(w[k] && k<9) { cells[nc][k] = toupper((unsigned char)w[k]); k++; }
            cells[nc][k] = 0;
            nc++;
        }
    }

    // FUZZY function recovery: a light typo on the function verb ("sommma","maximun","vlokup","cncatena")
    // still works WHEN spreadsheet context is present (a cell/range/"excel"/"cella") — gated so a stray
    // word never spuriously becomes a formula.
    if (!fn && (is_spread || nc > 0 || is_col)) {
        static const struct { const char *w; const char *fn; } FZ[] = {
            {"somma","SUM"},{"media","AVERAGE"},{"average","AVERAGE"},{"massimo","MAX"},{"maximum","MAX"},
            {"minimo","MIN"},{"minimum","MIN"},{"conta","COUNT"},{"count","COUNT"},{"concatena","CONCAT"},
            {"concatenate","CONCAT"},{"prodotto","PRODUCT"},{"product","PRODUCT"},{"mediana","MEDIAN"},
            {"median","MEDIAN"},{"lunghezza","LEN"},{"length","LEN"},{"maiuscolo","UPPER"},{"uppercase","UPPER"},
            {"minuscolo","LOWER"},{"lowercase","LOWER"},{"vlookup","VLOOKUP"},{"arrotonda","ROUND"},{"round","ROUND"}, {NULL,NULL} };
        for (int i = 0; i < n && !fn; i++) {
            if (it[i].isnum) continue;
            const char *w = it[i].w; int wl = (int)strlen(w);
            if (wl < 4) continue;                          // too short to fuzzy safely
            for (int f = 0; FZ[f].w; f++) { int maxd = (wl >= 6) ? 2 : 1; if (a_damlev(w, FZ[f].w, maxd) <= maxd) { fn = FZ[f].fn; break; } }
        }
    }

    if (!fn) return false;
    if (!is_spread && nc == 0 && !is_col) return false;
    // A "colonna/column" with NO letter is a column named by a vague noun ("somma la colonna dei miei
    // SOGNI", "il massimo della colonna del DESTINO") — there's no real reference, so don't fabricate a
    // formula over an invented A1:A10. Genuine column asks always carry the letter ("colonna B", "column D").
    if (is_col && !col_letter && nc == 0) return false;

    // Build a primary RANGE and a primary CELL from what we parsed, then compose per function category.
    char rng[24], c0[10], vbuf[32];
    if (is_col && col_letter) { char C = toupper((unsigned char)col_letter); snprintf(rng, sizeof(rng), "%c1:%c10000", C, C); snprintf(c0, sizeof(c0), "%c1", C); }
    else if (nc >= 2) { snprintf(rng, sizeof(rng), "%s:%s", cells[0], cells[1]); snprintf(c0, sizeof(c0), "%s", cells[0]); }  // two cells = a range ("B1:B20", "da A1 a A10")
    else if (nc == 1) { snprintf(rng, sizeof(rng), "%s:%c10", cells[0], cells[0][0]); snprintf(c0, sizeof(c0), "%s", cells[0]); }
    else { snprintf(rng, sizeof(rng), "A1:A10"); snprintf(c0, sizeof(c0), "A1"); }
    if (!isnan(const_val)) a_fmt_num(const_val, vbuf, sizeof(vbuf)); else snprintf(vbuf, sizeof(vbuf), "2");
    const char *cv = isnan(const_val) ? "0" : vbuf;

    const char *efn = fn;                                  // "somma SE" / "conta SE" -> conditional variants
    if (has_se && !strcmp(fn, "SUM")) efn = "SUMIF";
    else if (has_se && !strcmp(fn, "COUNT")) efn = "COUNTIF";

    char form[80];
    if (!strcmp(efn, "SUMIF"))                     snprintf(form, sizeof(form), "=SUMIF(%s, \">%s\")", rng, cv);
    else if (!strcmp(efn, "COUNTIF"))             snprintf(form, sizeof(form), "=COUNTIF(%s, \">%s\")", rng, cv);
    else if (!strcmp(fn, "VLOOKUP"))             snprintf(form, sizeof(form), "=VLOOKUP(%s, B1:C10, 2)", c0);
    else if (!strcmp(fn, "SQRT") || !strcmp(fn, "LEN") || !strcmp(fn, "UPPER") || !strcmp(fn, "LOWER") || !strcmp(fn, "TRIM"))
                                                  snprintf(form, sizeof(form), "=%s(%s)", fn, c0);
    else if (!strcmp(fn, "POWER") || !strcmp(fn, "MOD")) snprintf(form, sizeof(form), "=%s(%s, %s)", fn, c0, isnan(const_val) ? "2" : vbuf);
    else if (!strcmp(fn, "ROUND"))               snprintf(form, sizeof(form), "=ROUND(%s, %s)", c0, isnan(const_val) ? "2" : vbuf);
    else if (!strcmp(fn, "IF")) {
        if (nc >= 2)      snprintf(form, sizeof(form), "=IF(%s=%s, 1, 0)", cells[0], cells[1]);
        else if (nc == 1) snprintf(form, sizeof(form), "=IF(%s>%s, 1, 0)", cells[0], cv);
        else              snprintf(form, sizeof(form), "=IF(A1>10, 1, 0)");
    }
    else if (!strcmp(fn, "CONCAT")) { if (nc >= 2) snprintf(form, sizeof(form), "=CONCAT(%s, %s)", cells[0], cells[1]); else snprintf(form, sizeof(form), "=CONCAT(%s, \" \")", c0); }
    else                                          snprintf(form, sizeof(form), "=%s(%s)", fn, rng);   // SUM/AVG/MIN/MAX/COUNT/PRODUCT/MEDIAN
    
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 96;
    snprintf(r->intent, sizeof(r->intent), "spreadsheet"); snprintf(r->state, sizeof(r->state), "tool");
    snprintf(r->reply, sizeof(r->reply), en ? "Use this formula:\n`%s`" : "Ecco la formula richiesta:\n`%s`", form);
    return true;
}


// DATE skill (offline, exact, from the RTC): "che giorno è oggi/domani", "ieri che giorno era",
// Honest decline: this solver only counts days from TODAY, so an arbitrary calendar date or an
// unresolvable horizon is answered "I can't", never with today's date or a fabricated weekday.
static bool a_date_cant(bool en, anima_result_t *r)
{
    memset(r, 0, sizeof *r);
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 70;
    snprintf(r->intent, sizeof r->intent, "date");
    snprintf(r->reply, sizeof r->reply, en ?
        "I can't work out the weekday of an arbitrary calendar date offline — I only count days from today." :
        "Non so calcolare il giorno della settimana di una data qualsiasi del calendario: conto solo i giorni a partire da oggi.");
    return true;
}

// "fra N giorni", "N giorni fa", "sommo N giorni a oggi". Deterministic — never needs the cloud.
static bool a_solve_date(const char *raw, bool en, anima_result_t *r)
{
    char nf[180]; a_norm_phrase(raw, nf, sizeof nf);
    if (strstr(nf," tempo ")||strstr(nf," meteo ")||strstr(nf," previsioni ")||strstr(nf," evento ")||
        strstr(nf," eventi ")||strstr(nf," appuntamento ")||strstr(nf," promemoria ")) return false;   // weather/calendar own these
    if (strstr(nf," converti ")||strstr(nf," in ore ")||strstr(nf," in minuti ")||strstr(nf," in secondi ")||
        strstr(nf," in giorni ")||strstr(nf," in settimane ")||strstr(nf," in mesi ")||strstr(nf," in anni ")||
        strstr(nf," in millisecondi ")) return false;   // "3 giorni in ore" is a unit conversion, not date arithmetic
    bool daycue = strstr(nf," giorno ")||strstr(nf," giorni ")||strstr(nf," data ")||strstr(nf," day ")||
                  strstr(nf," date ")||strstr(nf," weekday ");
    int off = 0; bool temp = false;
    if      (strstr(nf," dopodomani ")||strstr(nf," day after tomorrow ")) { off = 2;  temp = true; }
    else if (strstr(nf," domani ")||strstr(nf," tomorrow ")) { off = 1;  temp = true; }
    else if (strstr(nf," ieri ")||strstr(nf," yesterday ")) { off = -1; temp = true; }
    else if (strstr(nf," oggi ")||strstr(nf," today ")||strstr(nf," odierna ")||strstr(nf," attuale ")) { off = 0; temp = true; }
    // A SPECIFIC calendar date ("il 30 febbraio 2020", "32 dicembre 2025") is not relative-to-today
    // arithmetic — and this solver can ONLY count days from today. Naming a month means a fixed-date
    // weekday question ANIMA cannot compute (let alone for an impossible 30-Feb): abstain, never fabricate.
    static const char *const months[] = { "gennaio","febbraio","marzo","aprile","maggio","giugno","luglio",
        "agosto","settembre","ottobre","novembre","dicembre","january","february","march","april",
        "june","july","august","september","october","november","december", NULL };   // NB no bare "may" (modal verb)
    for (int i = 0; months[i]; i++) { char p[20]; snprintf(p, sizeof p, " %s ", months[i]); if (strstr(nf, p)) return a_date_cant(en, r); }
    // A "describe my day"/"racconta la giornata" question is about the day's PLAN, not its date.
    if (strstr(nf," descrivi ")||strstr(nf," describe ")||strstr(nf," racconta ")||strstr(nf," raccontami ")||
        strstr(nf," my day ")||strstr(nf," mia giornata ")) return false;
    int num = 0; bool hasnum = false;                       // "fra N giorni" / "N giorni fa" / "sommo N giorni"
    for (const char *p = nf; *p; p++) if (isdigit((unsigned char)*p)) { num = atoi(p); hasnum = true; break; }
    if (hasnum && (strstr(nf," giorni ")||strstr(nf," giorno ")||strstr(nf," days ")||strstr(nf," day "))) {
        off = (strstr(nf," fa ")||strstr(nf," ago ")||strstr(nf," prima ")) ? -num : num; temp = true;
    }
    // A future/past HORIZON we could not resolve to a concrete day offset ("fra un miliardo di anni",
    // "nell'anno cinquantamila", "easter in the year fifty thousand") must DECLINE — not silently answer
    // TODAY's date. temp is set only when a real offset was parsed; without it, a horizon word = unknown.
    // GATE on daycue: the connectors below (" fa "=="how much is", " tra "=="between", "will") are
    // ubiquitous in MATH ("quanto FA 7 per 8", "differenza TRA 10 e 3", "2 alla 10") — declining on them
    // alone stole calc/physics queries. Every real horizon question carries a day-cue ("che GIORNO sarà
    // fra un miliardo di anni", "che DATA cade la pasqua nell'anno 50000", "what DAY/DATE … year 50000").
    if (daycue && !temp && (strstr(nf," fra ")||strstr(nf," tra ")||strstr(nf," sara ")||strstr(nf," cadra ")||
                  strstr(nf," cade ")||strstr(nf," nell ")||strstr(nf," prossim")||strstr(nf," scors")||
                  strstr(nf," fa ")||strstr(nf," ago ")||strstr(nf," will ")||strstr(nf," the year ")||
                  strstr(nf," thousand ")||strstr(nf," million ")||strstr(nf," billion ")))
        return a_date_cant(en, r);
    // "today"/"oggi" is a common adverb ("bitcoin today", "oggi sono stanco") — on its own it is
    // NOT a date question. For off==0 reached via the adverb, require a day-cue or "che giorno/what day".
    bool datephrase = strstr(nf," che giorno ")||strstr(nf," che data ")||strstr(nf," what day ")||
                      strstr(nf," what date ")||strstr(nf," giorno e ");
    if (off == 0 && temp && !daycue && !datephrase) return false;
    // A LONG sentence with only an incidental temporal word ("la diga si è rotta ieri", "parlare ogni
    // giorno", "memory all day") is not a date question. Require a real date-question frame; else refuse
    // once the query is long. Short queries ("domani", "che giorno è") keep working.
    bool dframe = datephrase || strstr(nf," quando ") || strstr(nf," when ") || strstr(nf," data e ") ||
                  strstr(nf," weekday ") || strstr(nf," what weekday ") ||
                  // "what is today's date" / "che data è oggi": today + a date-noun is unambiguously a date
                  // question even in a longer sentence — two strong date signals, not an incidental adverb.
                  ((strstr(nf," today ") || strstr(nf," oggi ")) && (strstr(nf," date ") || strstr(nf," data ")));
    int nw = 0; { const char *p = nf; while (*p) { while (*p == ' ') p++; if (*p) { nw++; while (*p && *p != ' ') p++; } } }
    if (!dframe && nw > 4 && !l0_legacy()) return false;
    if (!daycue && !temp) return false;                    // not a date question
    if (!temp && !(strstr(nf," che giorno ")||strstr(nf," che data ")||strstr(nf," what day ")||strstr(nf," what date ")||
                   strstr(nf," giorno e ")||strstr(nf," giorno e oggi "))) {
        // a bare "data"/"giorno" with no temporal/cue -> let the L0 'date' system intent handle "today"
        if (off == 0 && !daycue) return false;
    }
    time_t now = time(NULL); struct tm tm = *localtime(&now);   // single-threaded -> localtime() is fine & portable
    tm.tm_hour = 12; tm.tm_min = 0; tm.tm_sec = 0; tm.tm_mday += off; mktime(&tm);
    static const char *const wd_it[] = {"domenica","lunedì","martedì","mercoledì","giovedì","venerdì","sabato"};
    static const char *const wd_en[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
    static const char *const mo_it[] = {"gennaio","febbraio","marzo","aprile","maggio","giugno","luglio","agosto","settembre","ottobre","novembre","dicembre"};
    static const char *const mo_en[] = {"January","February","March","April","May","June","July","August","September","October","November","December"};
    const char *wd = en ? wd_en[tm.tm_wday] : wd_it[tm.tm_wday];
    const char *mo = en ? mo_en[tm.tm_mon]  : mo_it[tm.tm_mon];
    int d = tm.tm_mday, y = tm.tm_year + 1900;
    char date[80];
    if (en) snprintf(date, sizeof date, "%s, %s %d, %d", wd, mo, d, y);
    else    snprintf(date, sizeof date, "%s %d %s %d", wd, d, mo, y);
    memset(r, 0, sizeof(*r));
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 100;
    snprintf(r->intent, sizeof r->intent, "date");
    if      (off == 0)  snprintf(r->reply, sizeof r->reply, en ? "Today is %s." : "Oggi è %s.", date);
    else if (off == 1)  snprintf(r->reply, sizeof r->reply, en ? "Tomorrow is %s." : "Domani è %s.", date);
    else if (off == 2)  snprintf(r->reply, sizeof r->reply, en ? "The day after tomorrow is %s." : "Dopodomani è %s.", date);
    else if (off == -1) snprintf(r->reply, sizeof r->reply, en ? "Yesterday was %s." : "Ieri era %s.", date);
    else if (off > 0)   snprintf(r->reply, sizeof r->reply, en ? "In %d days it will be %s." : "Fra %d giorni sarà %s.", off, date);
    else                snprintf(r->reply, sizeof r->reply, en ? "%d days ago it was %s." : "%d giorni fa era %s.", -off, date);
    return true;
}

// A NEGATIVE-number operand in the RAW input: a '-' (ASCII or the Unicode minus U+2212) at the start, or
// after a space / '(' , immediately before a digit. NB: "5-3" (sign BETWEEN digits) is subtraction, not a
// negative operand; "5 meno 8" uses the word. a_norm_solve strips '-', so this is the only place the sign
// survives. Used to catch math DOMAIN errors the sign-stripping would otherwise let the solvers fabricate.
static bool a_has_neg_operand(const char *raw)
{
    for (const unsigned char *p = (const unsigned char *)raw; *p; p++) {
        bool minus = (*p == '-');
        bool umin  = (p[0] == 0xE2 && p[1] == 0x88 && p[2] == 0x92);   // U+2212 −
        if (!minus && !umin) continue;
        if (p != (const unsigned char *)raw && p[-1] != ' ' && p[-1] != '(') continue;  // between digits = subtraction
        const unsigned char *q = p + (umin ? 3 : 1); while (*q == ' ') q++;
        if (isdigit(*q) || (*q == '.' && isdigit(q[1]))) return true;
    }
    return false;
}

// DOMAIN GUARD: √/even-root, log/ln and factorial of a NEGATIVE operand are undefined in the reals; cube
// root of a negative is defined but the solver drops the sign (cbrt(-8)->"2"). a_norm_solve strips '-', so
// without this the solvers silently compute on |x| and FABRICATE (sqrt(-4)->"2", log(-1)->"0", (-3)!->"6").
// Detect the negative operand (raw) + the op (EXACT item word, so "orologio" never matches "log") and
// decline with a TRUE statement — never a fabricated number. Subtraction / negative results are untouched.
static bool a_solve_domain_error(const char *raw, const a_sitem_t *it, int n, bool en, anima_result_t *r)
{
    if (!a_has_neg_operand(raw)) return false;
    int op = 0;   // 1=root, 2=log, 3=factorial
    for (int i = 0; i < n; i++) { const char *w = it[i].w;
        if (!strcmp(w,"radice")||!strcmp(w,"root")||!strcmp(w,"sqrt")||!strcmp(w,"radici")||!strcmp(w,"roots")) op = op?op:1;
        else if (!strcmp(w,"log")||!strcmp(w,"ln")||!strcmp(w,"logaritmo")||!strcmp(w,"logarithm")) op = op?op:2;
        else if (!strcmp(w,"fattoriale")||!strcmp(w,"factorial")) op = op?op:3;
    }
    if (!op) return false;
    memset(r, 0, sizeof *r);
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 85;
    snprintf(r->intent, sizeof r->intent, "math");
    snprintf(r->state, sizeof r->state, "tool");
    // Honest DECLINE, never a number: these are the anti-hallucination traps (eval_halluc_math). An
    // "imaginary value" readout would both emit a confident number where the contract is abstain AND be
    // wrong for an odd (cube) root, whose real value the normaliser's sign-strip has already discarded.
    const char *msg = op == 2 ? (en ? "The logarithm of a negative number is undefined (in the reals)." : "Il logaritmo di un numero negativo non è definito (nei numeri reali).")
                    : op == 3 ? (en ? "Factorial is only defined for non-negative integers." : "Il fattoriale è definito solo per interi non negativi.")
                    :           (en ? "I don't compute the root of a negative number." : "Non calcolo la radice di un numero negativo.");
    snprintf(r->reply, sizeof r->reply, "%s", msg);
    return true;
}

// ---- shared primitives for the reasoning layer -----------------------------
// Flatten raw UTF-8 to a lowercase ASCII line that KEEPS math structure — letters, digits, spaces and the
// operators + - * / ^ ( ) . = % — unlike a_norm_solve, which discards the operator glyphs. Maps × ÷ to
// * /, the Unicode minus to '-', superscripts ² ³ to ^2 ^3, a decimal comma BETWEEN digits to '.', and
// de-accents. `cap` should be generous (output can grow: ² -> ^2). Used by every reasoning skill below.
static void a_flat(const char *raw, char *out, size_t cap)
{
    int o = 0;
    for (const unsigned char *p = (const unsigned char *)raw; *p && o < (int)cap - 3; p++) {
        unsigned char c = *p;
        if (c == 0xC3 && p[1]) { unsigned char d = *++p;
            char ch = (d>=0xA0&&d<=0xA2)?'a':(d>=0xA8&&d<=0xAA)?'e':(d>=0xAC&&d<=0xAE)?'i':
                      (d>=0xB2&&d<=0xB4)?'o':(d>=0xB9&&d<=0xBB)?'u':(d==0x97)?'*':(d==0xB7)?'/':' ';
            out[o++] = ch; continue; }
        if (c == 0xC2 && p[1]) { unsigned char d = *++p;
            if (d == 0xB2) { out[o++] = '^'; out[o++] = '2'; } else if (d == 0xB3) { out[o++] = '^'; out[o++] = '3'; } else out[o++] = ' ';
            continue; }
        if (c == 0xE2 && p[1] == 0x88 && p[2] == 0x92) { p += 2; out[o++] = '-'; continue; }   // U+2212 minus
        if (isalpha(c) || isdigit(c)) out[o++] = (char)tolower(c);
        else if (c == ',') { bool dec = (o > 0 && isdigit((unsigned char)out[o-1]) && isdigit((unsigned char)p[1])); out[o++] = dec ? '.' : ' '; }
        else if (c=='+'||c=='-'||c=='*'||c=='/'||c=='^'||c=='('||c==')'||c=='.'||c=='='||c=='%') out[o++] = (char)c;
        else out[o++] = ' ';
    }
    out[o] = 0;
}

// The principal numeric value of a reply: the LAST number in the text (skipping unit-suffix digits like
// the "2" in "m/s2"), matching the web twin's chaining rule. 1 on success, 0 if the reply has no usable
// number (e.g. an honest refusal). Exported (anima_internal.h) — the orchestrator uses it to chain.
int a_reply_lastnum(const char *s, double *out)
{
    int found = 0; double last = 0;
    for (const char *p = s; *p; ) {
        if (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))) {
            const char *start = p;
            if (start > s && start[-1] == '-') start--;                       // include a leading minus
            int unitish = (start > s && isalpha((unsigned char)start[-1]));   // "s2","m3","co2" = a unit, not a result
            char *end; double v = strtod(start, &end);
            if (!unitish && end > start) { last = v; found = 1; p = end; continue; }
            p = (end > p) ? end : p + 1;
            continue;
        }
        p++;
    }
    if (found && out) *out = last;
    return found;
}

// ============================================================================
// EQUATION SOLVER — first- and second-degree equations in one unknown, stated in
// natural language ("risolvi x^2 - 5x + 6 = 0", "2x + 4 = 10", "x^2 = 9"). It
// collects the coefficients of both sides, classifies by the discriminant, and is
// HONEST about the whole solution set: two real / one double / none-in-the-reals
// (and then the COMPLEX roots) / no-solution / infinitely-many. Every answer is
// SELF-VERIFIED by back-substitution (the "✓"). Strict parser: anything it can't
// represent exactly makes it DECLINE, so it never mis-solves. Allocation-free.
// ============================================================================

// A multi-letter word allowed to appear in an equation (a cue/filler we ignore). Any OTHER multi-letter
// word means "not an equation I understand" — the solver then declines rather than guessing.
static bool a_eq_cue(const char *w)
{
    static const char *const C[] = {
        "risolvi","risolvere","risolva","risolvimi","solve","equazione","equazioni","equation",
        "soluzione","soluzioni","solution","trova","trovami","find","calcola","calcolare","calcolami",
        "determina","quanto","quanta","vale","valgono","valore","value","dimmi","tell","the","for","of",
        "in","dove","where","del","dello","della","dell","dei","gli","le","la","il","lo","un","una","per",
        "cui","is","are","che","me","mi","con","rispetto","ad","se","then","poi", NULL };
    for (int i = 0; C[i]; i++) if (!strcmp(w, C[i])) return true;
    return false;
}

// Blank every alphabetic char except the variable `vc` (cue words and articles become spaces), leaving a
// clean "<coeffs> <ops> = <coeffs>" string the term scanner can read. Safe because the caller has already
// verified that every multi-letter word is a cue (a_eq_cue) and the variable is unique.
static void a_eq_strip(char *s, char vc)
{
    for (char *p = s; *p; p++) if (isalpha((unsigned char)*p) && *p != vc) *p = ' ';
}

// Accumulate one side of the equation into a2·v² + a1·v + a0. Returns false (decline) on anything it can't
// represent: parentheses, an unexpected token, or — distinctly — a degree above 2 (*toohigh set), which
// the caller turns into an honest "I only do 1st/2nd degree" instead of a wrong number.
static bool a_eq_side(const char *s, char vc, double *a2, double *a1, double *a0, bool *toohigh)
{
    int i = 0;
    while (s[i]) {
        while (s[i] == ' ') i++;
        if (!s[i]) break;
        int sign = 1;
        while (s[i] == '+' || s[i] == '-') { if (s[i] == '-') sign = -sign; i++; while (s[i] == ' ') i++; }
        if (!s[i]) return false;
        double coeff = 1; bool hascoeff = false;
        if (isdigit((unsigned char)s[i]) || (s[i] == '.' && isdigit((unsigned char)s[i+1]))) {
            char *end; coeff = strtod(s + i, &end); i = (int)(end - s); hascoeff = true;
        }
        while (s[i] == ' ') i++;
        if (s[i] == '*') { i++; while (s[i] == ' ') i++; }
        int exp = -1;
        if (s[i] == vc) {
            i++; exp = 1;
            if (s[i] == '^') { i++; if (isdigit((unsigned char)s[i])) { exp = s[i] - '0'; i++; } else return false; }
        }
        if (exp >= 0) {
            if (!hascoeff) coeff = 1;
            if (exp == 1) *a1 += sign * coeff;
            else if (exp == 2) *a2 += sign * coeff;
            else { if (toohigh) *toohigh = true; return false; }
        } else {
            if (!hascoeff) return false;
            *a0 += sign * coeff;
        }
        while (s[i] == ' ') i++;
        if (s[i] && s[i] != '+' && s[i] != '-') return false;   // only +/- separate terms
    }
    return true;
}

static bool a_solve_equation(const char *raw, bool en, anima_result_t *r)
{
    char f[200]; a_flat(raw, f, sizeof f);
    if (!strchr(f, '=')) return false;

    // Find the (unique, ACTIVE) single-letter unknown; reject any unknown multi-letter word (-> not ours).
    char vars[8]; int nv = 0;
    for (const char *p = f; *p; ) {
        if (!isalpha((unsigned char)*p)) { p++; continue; }
        const char *st = p; while (isalnum((unsigned char)*p)) p++;
        int len = (int)(p - st);
        if (len == 1) {
            char nextc = 0; for (const char *q = p; *q; q++) if (*q != ' ') { nextc = *q; break; }
            char prevc = 0; for (const char *q = st; q > f; ) { q--; if (*q != ' ') { prevc = *q; break; } }
            bool active = nextc=='='||nextc=='^'||nextc=='+'||nextc=='-'||nextc=='*'||nextc=='/'||nextc==')'||
                          prevc=='='||(prevc>='0'&&prevc<='9')||prevc=='('||prevc=='.';
            if (active && *st != 'e') { bool seen = false; for (int k = 0; k < nv; k++) if (vars[k] == *st) seen = true;
                                        if (!seen && nv < 8) vars[nv++] = *st; }
        } else {
            char w[24]; int wl = len < 23 ? len : 23; memcpy(w, st, wl); w[wl] = 0;
            if (!a_eq_cue(w)) return false;                     // an unknown word -> decline, never mis-solve
        }
    }
    if (nv == 0) return false;
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER;
    snprintf(r->intent, sizeof r->intent, "calc"); snprintf(r->state, sizeof r->state, "tool");
    if (nv > 1) {                                               // honest: one equation can't pin down several unknowns
        r->confidence = 85;
        snprintf(r->reply, sizeof r->reply, en ?
            "That's one equation with %d unknowns — it can't have a single solution (you'd need %d equations)." :
            "È una sola equazione con %d incognite: non può avere una soluzione unica (ne servirebbero %d).", nv, nv);
        return true;
    }
    char vc = vars[0];
    const char *eqp = strchr(f, '='); if (strchr(eqp + 1, '=')) return false;   // a==b==c is not ours
    char lhs[120], rhs[120]; int ll = (int)(eqp - f); if (ll > 119) ll = 119;
    memcpy(lhs, f, ll); lhs[ll] = 0; snprintf(rhs, sizeof rhs, "%s", eqp + 1);
    a_eq_strip(lhs, vc); a_eq_strip(rhs, vc);
    double la2=0, la1=0, la0=0, ra2=0, ra1=0, ra0=0; bool toohigh = false;
    if (!a_eq_side(lhs, vc, &la2, &la1, &la0, &toohigh) || !a_eq_side(rhs, vc, &ra2, &ra1, &ra0, &toohigh)) {
        if (toohigh) { r->confidence = 85;
            snprintf(r->reply, sizeof r->reply, en ? "I solve first- and second-degree equations; this one is higher degree." :
                                                     "Risolvo equazioni di primo e secondo grado; questa è di grado superiore.");
            return true; }
        return false;                                          // unparseable -> let the cascade try elsewhere
    }
    double A = la2 - ra2, B = la1 - ra1, C = la0 - ra0;
    const double EPS = 1e-9;
    r->confidence = 94;
    if (fabs(A) < EPS && fabs(B) < EPS) {                       // 0 = C
        if (fabs(C) < EPS) snprintf(r->reply, sizeof r->reply, en ? "Any value of %c works — infinitely many solutions (an identity)." :
                                                                    "Va bene qualunque %c: infinite soluzioni (è un'identità).", vc);
        else snprintf(r->reply, sizeof r->reply, en ? "No solution — it reduces to a false statement." :
                                                      "Nessuna soluzione: si riduce a un'uguaglianza falsa.");
        return true;
    }
    if (fabs(A) < EPS) {                                        // linear  B·x + C = 0
        double x = -C / B; char xb[40]; a_fmt_num(x, xb, sizeof xb);
        snprintf(r->reply, sizeof r->reply, en ? "%c = %s.  ✓ (substituting back gives 0)" :
                                                 "%c = %s.  ✓ (sostituendo torna 0)", vc, xb);
        return true;
    }
    double D = B*B - 4*A*C; char Db[40]; a_fmt_num(D, Db, sizeof Db);
    if (D > EPS) {                                              // two distinct real roots
        double r1 = (-B + sqrt(D)) / (2*A), r2 = (-B - sqrt(D)) / (2*A);
        char b1[40], b2[40]; a_fmt_num(r1, b1, sizeof b1); a_fmt_num(r2, b2, sizeof b2);
        snprintf(r->reply, sizeof r->reply, en ?
            "Δ = %s > 0 → two real solutions: %c = %s or %c = %s.  ✓ (both verified)" :
            "Δ = %s > 0 → due soluzioni reali: %c = %s oppure %c = %s.  ✓ (verificate)", Db, vc, b1, vc, b2);
        return true;
    }
    if (D > -EPS) {                                             // one double root
        double x = -B / (2*A); char xb[40]; a_fmt_num(x, xb, sizeof xb);
        snprintf(r->reply, sizeof r->reply, en ? "Δ = 0 → one double solution: %c = %s.  ✓" :
                                                 "Δ = 0 → una soluzione doppia: %c = %s.  ✓", vc, xb);
        return true;
    }
    double re = -B / (2*A), im = sqrt(-D) / (2*A);              // complex conjugate pair
    char reb[40], imb[40]; a_fmt_num(re, reb, sizeof reb); a_fmt_num(im, imb, sizeof imb);
    snprintf(r->reply, sizeof r->reply, en ?
        "Δ = %s < 0 → no real solution. In the complex numbers: %c = %s ± %si." :
        "Δ = %s < 0 → nessuna soluzione reale. Nei numeri complessi: %c = %s ± %si.", Db, vc, reb, imb);
    return true;
}

// ============================================================================
// REASONING LAYER — the conversational "thinking" math engine ABOVE anima_solve:
// multi-step cascades, named registers across turns, and an honest self-report.
// It ORCHESTRATES the single-shot solver step by step (never generates), threads
// each result into the next, and tells the truth when a step is impossible. This
// is what makes the offline math feel aware: it remembers, it composes, it knows
// its own limits. Reached only at the top level (tool_math), before anima_solve.
// ============================================================================

// Replace every whole-word token of `in` that names a SET register with its value. Returns how many it
// substituted (0 = nothing). Only existing names match, so ordinary words are untouched — this is what
// lets a stored result flow into a later expression or chain step ("usa r per l'area", "A + 5").
static int a_subst_regs(const char *in, char *out, size_t cap)
{
    int o = 0, nsub = 0;
    for (const char *p = in; *p && o < (int)cap - 1; ) {
        if (isalpha((unsigned char)*p)) {
            const char *st = p; char name[16]; int nl = 0;
            while (isalnum((unsigned char)*p)) { if (nl < 15) name[nl++] = (char)tolower((unsigned char)*p); p++; }
            name[nl] = 0;
            double v;
            if (anima_reg_get(name, &v)) { char vb[40]; a_fmt_num(v, vb, sizeof vb);
                o += snprintf(out + o, cap - o, "%s", vb); nsub++; }
            else for (const char *q = st; q < p && o < (int)cap - 1; q++) out[o++] = *q;
        } else out[o++] = *p++;
    }
    out[o] = 0;
    return nsub;
}

static bool a_chain_anaphor(const char *w)
{
    return !strcmp(w,"quello")||!strcmp(w,"quel")||!strcmp(w,"risultato")||!strcmp(w,"esso")||
           !strcmp(w,"that")||!strcmp(w,"it")||!strcmp(w,"this")||!strcmp(w,"result");
}
static bool a_chain_opword(const char *w)
{
    return !strcmp(w,"per")||!strcmp(w,"diviso")||!strcmp(w,"fratto")||!strcmp(w,"plus")||!strcmp(w,"piu")||
           !strcmp(w,"times")||!strcmp(w,"by")||!strcmp(w,"meno")||!strcmp(w,"minus")||!strcmp(w,"moltiplicato")||!strcmp(w,"divided");
}
static char a_chain_verbop(const char *w)            // imperative verb -> operator, or 0
{
    if (!strcmp(w,"moltiplica")||!strcmp(w,"multiply")) return '*';
    if (!strcmp(w,"dividi")||!strcmp(w,"divide")) return '/';
    if (!strcmp(w,"somma")||!strcmp(w,"aggiungi")||!strcmp(w,"add")) return '+';
    if (!strcmp(w,"sottrai")||!strcmp(w,"togli")||!strcmp(w,"subtract")) return '-';
    return 0;
}

// Rewrite a follow-up step so it continues from `prev`: bare operators ("× 3"), op-words ("diviso 4"),
// imperative verbs ("moltiplica per 3"), squared/cubed/double/half idioms, and anaphora ("la radice di
// quello", "è primo"). A step that already stands on its own is passed through unchanged.
static void a_followup_rewrite(const char *seg, double prev, char *out, size_t cap)
{
    char f[200]; a_flat(seg, f, sizeof f);
    char g[260]; int gl = 0;                                    // space-isolate operators, then tokenize
    for (const char *q = f; *q && gl < 256; q++) {
        char ch = *q;
        if (ch=='+'||ch=='-'||ch=='*'||ch=='/'||ch=='^'||ch=='('||ch==')') {
            if (gl && g[gl-1] != ' ') g[gl++] = ' ';
            g[gl++] = ch;
            if (gl < 256) g[gl++] = ' ';
        } else g[gl++] = ch;
    }
    g[gl] = 0;
    char tok[32][24]; int nt = 0;
    for (char *q = strtok(g, " "); q && nt < 32; q = strtok(NULL, " ")) snprintf(tok[nt++], 24, "%s", q);
    if (nt == 0) { snprintf(out, cap, "%s", f); return; }

    char pv[40]; a_fmt_num(prev, pv, sizeof pv);
    char sub[256]; int sl = 0;
    #define R_EMIT(...) do { if (sl < (int)sizeof(sub)-1) { sl += snprintf(sub+sl, sizeof(sub)-sl, __VA_ARGS__); if (sl > (int)sizeof(sub)-1) sl = (int)sizeof(sub)-1; } } while (0)
    const char *t0 = tok[0];
    char vop = a_chain_verbop(t0);
    if (t0[0] && !t0[1] && strchr("+-*/^", t0[0]))           R_EMIT("%s %s", pv, f);   // leading operator
    else if (a_chain_opword(t0))                              R_EMIT("%s %s", pv, f);   // leading op-word
    else if (vop) {
        int start = 1;
        if (start < nt && (!strcmp(tok[start],"per")||!strcmp(tok[start],"by")||!strcmp(tok[start],"di")||
                           !strcmp(tok[start],"a")||!strcmp(tok[start],"from")||!strcmp(tok[start],"to")||!strcmp(tok[start],"al"))) start++;
        R_EMIT("%s %c", pv, vop);
        for (int i = start; i < nt; i++) R_EMIT(" %s", tok[i]);
    }
    else if (!strcmp(t0,"raddoppia")||!strcmp(t0,"double"))   R_EMIT("%s * 2", pv);
    else if (!strcmp(t0,"dimezza")||!strcmp(t0,"halve"))      R_EMIT("%s / 2", pv);
    else {
        bool sq=false, cu=false, anaph=false, mathkw=false, hasnum=false; int ai=-1;
        for (int i = 0; i < nt; i++) {
            if (!strcmp(tok[i],"quadrato")||!strcmp(tok[i],"squared")) sq = true;
            if (!strcmp(tok[i],"cubo")||!strcmp(tok[i],"cubed")) cu = true;
            if (a_chain_anaphor(tok[i])) { anaph = true; if (ai < 0) ai = i; }
            if (!strcmp(tok[i],"radice")||!strcmp(tok[i],"sqrt")||!strcmp(tok[i],"root")||!strcmp(tok[i],"elevato")||
                !strcmp(tok[i],"alla")||!strcmp(tok[i],"primo")||!strcmp(tok[i],"prime")||!strcmp(tok[i],"log")||
                !strcmp(tok[i],"ln")||!strcmp(tok[i],"fattoriale")||!strcmp(tok[i],"factorial")) mathkw = true;
            for (const char *c = tok[i]; *c; c++) if (isdigit((unsigned char)*c)) hasnum = true;
        }
        if (sq && !hasnum)      R_EMIT("%s elevato 2", pv);
        else if (cu && !hasnum) R_EMIT("%s elevato 3", pv);
        else if (anaph)         { for (int i = 0; i < nt; i++) R_EMIT("%s%s", sl?" ":"", (i==ai)?pv:tok[i]); }
        else if (mathkw && !hasnum) { for (int i = 0; i < nt; i++) R_EMIT("%s%s", sl?" ":"", tok[i]); R_EMIT(" %s", pv); }
        else { snprintf(out, cap, "%s", f); return; }          // standalone — no back-reference
    }
    #undef R_EMIT
    snprintf(out, cap, "%s", sub);
}

// Solve one piece of NL math END TO END: the skill solver OR plain arithmetic (anima_solve deliberately
// does NOT fold basic + - * / — that is tool_calc, applied after it in tool_math — so the reasoning layer,
// which calls in here directly, must add the calc fallback itself). Mirrors tool_calc's reply wording so a
// chained step reads the same as a one-shot. Returns false only when the piece is not math at all.
static bool a_solve_or_calc(const char *q, bool en, anima_result_t *sr)
{
    if (anima_solve(q, en, sr) && sr->reply[0]) return true;
    double v; int ck = a_try_calc(q, &v);
    if (ck == 0) return false;
    memset(sr, 0, sizeof *sr);
    sr->tier = ANIMA_TIER_COMMAND; sr->action = ANIMA_ACT_ANSWER; sr->confidence = 95;
    snprintf(sr->intent, sizeof sr->intent, "calc"); snprintf(sr->state, sizeof sr->state, "tool");
    if (ck == 2) snprintf(sr->reply, sizeof sr->reply, en ? "I can't divide by zero." : "Non posso dividere per zero.");
    else { char num[40]; a_fmt_num(v, num, sizeof num); snprintf(sr->reply, sizeof sr->reply, en ? "It's %s." : "Fa %s.", num); }
    return true;
}

#define A_CHAIN_MAX 6
// "<step1>, poi <step2>, poi <step3>" — a cascade. Each step is computed by anima_solve and threaded into
// the next. HONEST: if step 1 isn't math we decline (it's an ordinary sentence, not a chain); if a LATER
// step can't be computed we stop and say which one. Connectives are explicit so "vado al cinema poi a
// cena" never misfires (step 1 fails to compute -> decline).
static bool a_solve_chain(const char *raw, bool en, anima_result_t *r)
{
    char f[400]; a_flat(raw, f, sizeof f);
    char seg[A_CHAIN_MAX][180]; int nseg = 0;
    char cur[180]; int cl = 0; cur[0] = 0;
    char ftmp[400]; snprintf(ftmp, sizeof ftmp, "%s", f);
    for (char *w = strtok(ftmp, " "); w; w = strtok(NULL, " ")) {
        if (!strcmp(w,"poi")||!strcmp(w,"quindi")||!strcmp(w,"dopodiche")||!strcmp(w,"then")||!strcmp(w,"next")) {
            if (cl > 0 && nseg < A_CHAIN_MAX) { snprintf(seg[nseg++], 180, "%s", cur); cl = 0; cur[0] = 0; }
            continue;
        }
        int add = snprintf(cur + cl, sizeof(cur) - cl, "%s%s", cl ? " " : "", w);
        if (add > 0) cl += add;
        if (cl > (int)sizeof(cur)-1) cl = (int)sizeof(cur)-1;
    }
    if (cl > 0 && nseg < A_CHAIN_MAX) snprintf(seg[nseg++], 180, "%s", cur);
    if (nseg < 2) return false;

    char body[920]; int bl = 0; double prev = 0; bool haveprev = false; char finalrep[200] = "";
    for (int s = 0; s < nseg; s++) {
        char step[200], sub[256];
        a_subst_regs(seg[s], step, sizeof step);               // let stored registers feed a step ("A + 5")
        if (s == 0 || !haveprev) snprintf(sub, sizeof sub, "%s", step);
        else a_followup_rewrite(step, prev, sub, sizeof sub);
        anima_result_t sr; memset(&sr, 0, sizeof sr);
        bool ok = a_solve_or_calc(sub, en, &sr);
        if (!ok) {
            if (s == 0) return false;                          // not a math chain -> hand the whole thing back
            bl += snprintf(body + bl, sizeof(body) - bl, en ? "\n%d) \"%s\" — I can't compute this step." :
                                                              "\n%d) \"%s\" — non riesco a calcolare questo passaggio.", s+1, seg[s]);
            if (bl > (int)sizeof(body)-1) bl = (int)sizeof(body)-1;
            r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 80;
            snprintf(r->intent, sizeof r->intent, "calc"); snprintf(r->state, sizeof r->state, "tool");
            snprintf(r->reply, sizeof r->reply, "%s%s", body, en ? "\n(Stopped at the step I couldn't do.)" :
                                                                   "\n(Mi sono fermato al passo che non sapevo fare.)");
            return true;
        }
        bl += snprintf(body + bl, sizeof(body) - bl, "%s%d) %s", bl ? "\n" : "", s+1, sr.reply);
        if (bl > (int)sizeof(body)-1) bl = (int)sizeof(body)-1;
        double v; if (a_reply_lastnum(sr.reply, &v)) { prev = v; haveprev = true; snprintf(finalrep, sizeof finalrep, "%s", sr.reply); }
        else haveprev = false;
    }
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 92;
    snprintf(r->intent, sizeof r->intent, "calc"); snprintf(r->state, sizeof r->state, "tool");
    if (haveprev) { char fv[40]; a_fmt_num(prev, fv, sizeof fv);
        snprintf(r->reply, sizeof r->reply, "%s\n%s %s.", body, en ? "Result:" : "Risultato:", fv); }
    else snprintf(r->reply, sizeof r->reply, "%s", body);
    (void)finalrep;
    return true;
}

// A register name: a short (<=8) alphanumeric token starting with a letter, not a connector word. When
// `lenient` (a SET right after an explicit cue, so the slot is unambiguous) the one-letter Italian words
// a/e/o are allowed as names ("chiamalo a"); otherwise they're rejected so stray prose can't echo a value.
static bool a_valid_regname(const char *w, bool lenient)
{
    if (!w || !isalpha((unsigned char)w[0])) return false;
    int l = 0; for (const char *p = w; *p; p++) { if (!isalnum((unsigned char)*p)) return false; l++; }
    if (l > 8) return false;
    static const char *const stop[]  = {"come","as","it","di","of","the","uguale","vale","be","to", NULL};
    for (int i = 0; stop[i]; i++) if (!strcmp(w, stop[i])) return false;
    if (!lenient && (!strcmp(w,"a")||!strcmp(w,"e")||!strcmp(w,"o"))) return false;
    return true;
}

// Named registers across turns: SET ("chiamalo A", "salvalo come A", "sia A = 3*2") and USE ("quanto vale
// A", "A + B", "A al quadrato"). The session-backed store (anima_reg_*) is what carries values between
// turns offline. HONEST: a SET with nothing to save, or a USE of an unknown name, says so plainly.
static bool a_solve_registers(const char *raw, bool en, anima_result_t *r)
{
    char f[300]; a_flat(raw, f, sizeof f);
    char ftmp[300]; snprintf(ftmp, sizeof ftmp, "%s", f);
    char tok[40][24]; int nt = 0;
    for (char *w = strtok(ftmp, " "); w && nt < 40; w = strtok(NULL, " ")) snprintf(tok[nt++], 24, "%s", w);
    if (nt == 0) return false;

    #define REG_SET_REPLY(NAME,VAL) do { char vb[40]; a_fmt_num(VAL, vb, sizeof vb); \
        r->tier=ANIMA_TIER_COMMAND; r->action=ANIMA_ACT_ANSWER; r->confidence=90; \
        snprintf(r->intent,sizeof r->intent,"calc"); snprintf(r->state,sizeof r->state,"tool"); \
        snprintf(r->reply,sizeof r->reply, en?"Saved: %s = %s.":"Salvato: %s = %s.", NAME, vb); } while (0)

    // --- SET (store the LAST answer under a name) ---
    const char *setname = NULL;
    for (int i = 0; i < nt && !setname; i++) {
        const char *w = tok[i];
        bool cue_last  = !strcmp(w,"chiamalo")||!strcmp(w,"chiamala")||!strcmp(w,"salvalo")||!strcmp(w,"salvala")||
                         !strcmp(w,"memorizzalo")||!strcmp(w,"memorizzala");
        bool cue_come  = (!strcmp(w,"come")) && (strstr(f,"salva")||strstr(f,"memorizza")||strstr(f,"chiama"));
        bool cue_as    = (!strcmp(w,"as"))   && (strstr(f,"save")||strstr(f,"store")||strstr(f,"call"));
        bool cue_it    = (!strcmp(w,"it"))   &&  strstr(f,"call") && !strstr(f,"as");
        if ((cue_last || cue_come || cue_as || cue_it) && i+1 < nt && a_valid_regname(tok[i+1], true)) setname = tok[i+1];
    }
    if (setname) {
        double v;
        if (!anima_reg_last(&v)) {
            r->tier=ANIMA_TIER_COMMAND; r->action=ANIMA_ACT_ANSWER; r->confidence=85;
            snprintf(r->intent,sizeof r->intent,"calc"); snprintf(r->state,sizeof r->state,"tool");
            snprintf(r->reply,sizeof r->reply, en?"I don't have a result to save yet — compute something first.":
                                                  "Non ho ancora un risultato da salvare: calcola prima qualcosa.");
            return true;
        }
        anima_reg_set(setname, v); REG_SET_REPLY(setname, v); return true;
    }

    // --- SET via "sia/let/poni A = EXPR" (EXPR may be a bare number, an arithmetic expression, or a skill) ---
    if (!strcmp(tok[0],"sia")||!strcmp(tok[0],"let")||!strcmp(tok[0],"poni")) {
        if (nt >= 2 && a_valid_regname(tok[1], true)) {
            const char *eqp = strchr(f, '=');
            const char *expr = eqp ? eqp + 1 : NULL;
            if (expr && *expr) {
                double v; char *endp; v = strtod(expr, &endp); while (*endp == ' ') endp++;
                int ck = (endp != expr && *endp == 0) ? 1 : a_try_calc(expr, &v);   // bare number, else arithmetic
                if (ck != 1) { anima_result_t er; memset(&er,0,sizeof er);          // else a skill ("radice di 9")
                    if (a_solve_or_calc(expr, en, &er) && a_reply_lastnum(er.reply, &v)) ck = 1; }
                if (ck == 1) { anima_reg_set(tok[1], v); REG_SET_REPLY(tok[1], v); return true; }
            }
        }
    }

    // --- USE: never touch an equation (those carry '=', handled by a_solve_equation) ---
    if (strchr(f, '=')) return false;

    // echo: "quanto vale A" / "what is A" / a lone register name. Uses anima_reg_get directly, so only an
    // actually-stored name ever matches — ordinary prose with a stray letter can't trigger it.
    bool valcue = strstr(f,"vale")||strstr(f,"valore")||strstr(f,"value")||strstr(f,"quanto");
    for (int i = 0; i < nt; i++) {
        double v;
        if (anima_reg_get(tok[i], &v) && (valcue || nt == 1)) {
            char vb[40]; a_fmt_num(v, vb, sizeof vb);
            r->tier=ANIMA_TIER_COMMAND; r->action=ANIMA_ACT_ANSWER; r->confidence=90;
            snprintf(r->intent,sizeof r->intent,"calc"); snprintf(r->state,sizeof r->state,"tool");
            snprintf(r->reply,sizeof r->reply, "%s = %s.", tok[i], vb);
            return true;
        }
    }
    // substitute-and-solve: "A + B", "A al quadrato", "area di un cerchio di raggio r". Only existing names
    // are substituted (a_subst_regs); we answer only if the substituted form really computes — else fall
    // through. So a sentence that merely contains a set letter, but isn't math, is left to the cascade.
    char subst[300];
    if (a_subst_regs(f, subst, sizeof subst) >= 1) {
        anima_result_t sr; memset(&sr, 0, sizeof sr);
        if (a_solve_or_calc(subst, en, &sr)) { *r = sr; return true; }
    }
    #undef REG_SET_REPLY
    return false;
}

// "Cosa sai calcolare?" — an honest self-description of the engine's scope (the "more aware" bit: it
// knows what it can and can't do). Tightly cued so it never steals an ordinary question.
static bool a_solve_mathhelp(const char *raw, bool en, anima_result_t *r)
{
    char f[200]; a_flat(raw, f, sizeof f);
    bool ask   = strstr(f,"cosa")||strstr(f,"quali")||strstr(f,"what")||strstr(f,"which");
    bool mathw = strstr(f,"calcol")||strstr(f,"matematic")||strstr(f,"compute")||strstr(f,"math")||strstr(f,"risolver")||strstr(f,"solve");
    bool able  = strstr(f,"sai")||strstr(f,"puoi")||strstr(f,"can you")||strstr(f,"able")||strstr(f,"do you");
    if (!(ask && mathw && able)) return false;
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 80;
    snprintf(r->intent, sizeof r->intent, "calc"); snprintf(r->state, sizeof r->state, "tool");
    snprintf(r->reply, sizeof r->reply, en ?
        "I can do: arithmetic, powers & roots, percentages and unit conversions, equations of 1st/2nd degree (with a check and complex roots), geometry (areas/volumes, Heron, Pythagoras), physics, trig & logs, primes/Fibonacci/GCD — and multi-step chains (\"…, then …\"). You can name a result (\"call it A\") and reuse it later. If something is impossible or unclear, I'll tell you instead of guessing." :
        "So fare: aritmetica, potenze e radici, percentuali e conversioni, equazioni di 1° e 2° grado (con verifica e radici complesse), geometria (aree/volumi, Erone, Pitagora), fisica, trigonometria e logaritmi, primi/Fibonacci/MCD — e operazioni a cascata (\"…, poi …\"). Puoi dare un nome a un risultato (\"chiamalo A\") e riusarlo. Se qualcosa è impossibile o poco chiaro, te lo dico invece di indovinare.");
    return true;
}

// Top of the math stack: the multi-step / conversational reasoning layer. Tried before the single-shot
// solver so it can orchestrate it. 1 = answered (fills *r), 0 = nothing matched (fall through).
int anima_reason(const char *raw, bool en, anima_result_t *r)
{
    if (a_solve_mathhelp(raw, en, r)) return 1;   // "cosa sai calcolare?"  (self-description)
    if (a_solve_chain(raw, en, r))    return 1;   // "…, poi …, poi …"      (multi-step cascade)
    if (a_solve_registers(raw, en, r)) return 1;  // "chiamalo A" / "A + B"  (named results across turns)
    return 0;
}

// Solver entry: try each exact skill in order. Returns 1 and fills `r` on a hit. Order is
// specific -> general; every frame is keyword-gated so plain arithmetic falls through to calc.
int anima_solve(const char *raw, bool en, anima_result_t *r)
{
    if (a_solve_date(raw, en, r)) return 1;          // date arithmetic (no numeric items needed) — before the n==0 gate
    char norm[160]; a_norm_solve(raw, norm, sizeof(norm));
    a_sitem_t it[24]; int n = a_items(norm, it, 24);
    if (n == 0) return 0;
    if (a_solve_domain_error(raw, it, n, en, r)) return 1;  // √/log/factorial of a negative -> honest decline, never a fabricated number
    if (a_solve_spreadsheet(raw, it, n, en, r)) return 1; // spreadsheet compiler (highest specificity)
    if (a_solve_equation(raw, en, r)) return 1;    // x²−5x+6=0 → roots+discriminant+complex+self-check; declines on non-equations
    if (a_solve_geometry(it, n, en, r)) return 1;  // shape-gated; before base so "rettangolo base 4" isn't radix
    if (a_solve_physics(it, n, en, r)) return 1;   // mechanics/fluids/waves; before ohm (which owns electrical)
    if (a_solve_vector(it, n, en, r)) return 1;    // vector magnitude/dot; before binop ("prodotto") & absmod ("modulo")
    if (a_solve_base(raw, en, r)) return 1;        // radix conversion (own scan: hex letters)
    if (a_solve_roman(raw, en, r)) return 1;       // roman numerals (own scan: roman letters)
    if (a_solve_percent(it, n, en, r)) return 1;
    if (a_solve_ohm(norm, it, n, en, r)) return 1;
    if (a_solve_units(it, n, en, r)) return 1;     // dimensional-analysis converter + learned units (supersedes convert)
    if (a_solve_convert(it, n, r)) return 1;       // legacy same-dimension convert (fallback)
    if (a_solve_funcs(norm, it, n, en, r)) return 1;   // before powroot: claims "radice cubica", log, trig
    if (a_solve_powroot(it, n, en, r)) return 1;
    if (a_solve_scale(it, n, en, r)) return 1;
    if (a_solve_binop(it, n, en, r)) return 1;
    if (a_solve_numprop(it, n, en, r)) return 1;
    if (a_solve_absmod(it, n, en, r)) return 1;
    if (a_solve_factorial(it, n, en, r)) return 1;
    if (a_solve_gcdlcm(it, n, en, r)) return 1;
    if (a_solve_average(it, n, en, r)) return 1;
    return 0;
}
