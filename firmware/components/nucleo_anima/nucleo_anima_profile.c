// ANIMA personal-profile tier — see nucleo_anima_profile.h. Deterministic, network-free, host-runnable.
#include "nucleo_anima_profile.h"
#include "nucleo_board.h"      // NUCLEO_SD_MOUNT
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>            // atoi

#define P_TSV   NUCLEO_SD_MOUNT "/data/anima/profile.tsv"   // field<TAB>value, one per line
#define P_VAL   96             // max stored value length
#define P_LO    256            // working lowercase buffer

// ---- tiny field store (field-keyed TSV, streaming rewrite) -----------------

static int pget(const char *field, char *out, int cap)
{
    FILE *f = fopen(P_TSV, "r");
    if (!f) return 0;
    size_t fl = strlen(field); int found = 0; static char ln[256];
    while (fgets(ln, sizeof(ln), f)) {
        char *t = strchr(ln, '\t'); if (!t) continue;
        if ((size_t)(t - ln) != fl || memcmp(ln, field, fl)) continue;
        char *v = t + 1; size_t n = strlen(v);
        while (n && (v[n-1] == '\n' || v[n-1] == '\r')) n--;
        if (n >= (size_t)cap) n = cap - 1;
        memcpy(out, v, n); out[n] = 0; found = out[0] ? 1 : 0; break;
    }
    fclose(f);
    return found;
}

static void pset(const char *field, const char *value)
{
    size_t fl = strlen(field);
    char tmp[160]; snprintf(tmp, sizeof(tmp), "%s.tmp", P_TSV);
    FILE *out = fopen(tmp, "w");
    if (!out) return;
    FILE *in = fopen(P_TSV, "r");
    if (in) {
        static char ln[256];
        while (fgets(ln, sizeof(ln), in)) {
            char *t = strchr(ln, '\t'); if (!t) continue;
            if ((size_t)(t - ln) == fl && !memcmp(ln, field, fl)) continue;   // drop the old value
            fputs(ln, out);
            size_t L = strlen(ln); if (L == 0 || ln[L-1] != '\n') fputc('\n', out);
        }
        fclose(in);
    }
    fprintf(out, "%s\t%s\n", field, value);
    fclose(out);
    remove(P_TSV);
    if (rename(tmp, P_TSV) != 0) remove(tmp);
}

// ---- text helpers ----------------------------------------------------------

// Lowercase ASCII into `lo` (keeping UTF-8 multibyte like è intact, so lo[] stays byte-aligned with raw[]).
static void prep_lo(const char *raw, char *lo, int cap)
{
    int o = 0;
    for (; raw[o] && o < cap - 1; o++) lo[o] = (char)tolower((unsigned char)raw[o]);
    lo[o] = 0;
}

// strstr at a left word boundary (start, space, or punctuation before): avoids "mi chiamo" inside a word.
static const char *wb_find(const char *lo, const char *pat)
{
    for (const char *p = lo; (p = strstr(p, pat)); p++) {
        if (p == lo) return p;
        unsigned char b = (unsigned char)p[-1];
        if (b == ' ' || b == ',' || b == '.' || b == ';' || b == ':' || b == '\'' || b == '"') return p;
    }
    return NULL;
}

// Copy raw[off..] into out, trim leading spaces and trailing spaces/.?!, cap length. Returns out length.
static int extract_value(const char *raw, size_t off, char *out, int cap)
{
    const char *s = raw + off;
    while (*s == ' ') s++;
    int n = 0;
    for (; s[n] && n < cap - 1; n++) out[n] = s[n];
    while (n > 0 && (out[n-1] == ' ' || out[n-1] == '.' || out[n-1] == '?' || out[n-1] == '!' ||
                     out[n-1] == ',' || out[n-1] == '\n' || out[n-1] == '\r')) n--;
    out[n] = 0;
    return n;
}

// First integer in `lo` (for age). Returns its string in out and length, or 0 if none.
static int first_int(const char *lo, char *out, int cap)
{
    const char *p = lo; while (*p && !isdigit((unsigned char)*p)) p++;
    int n = 0; while (p[n] && isdigit((unsigned char)p[n]) && n < cap - 1) { out[n] = p[n]; n++; }
    out[n] = 0; return n;
}

// ---- field tables ----------------------------------------------------------

typedef struct { const char *field; const char *lead; } pat_t;

// RECALL patterns (questions). Checked FIRST so "come mi chiamo" is a recall, not a set of name="come".
// "" field = the whole-profile summary.
static const pat_t RECALL[] = {
    { "",        "cosa sai di me" }, { "", "che cosa sai di me" }, { "", "che sai di me" }, { "", "cosa sai su di me" },
    { "",        "il mio profilo" }, { "", "what do you know about me" }, { "", "tell me about myself" },
    { "",        "tell me about me" }, { "", "my profile" }, { "", "what's my profile" },
    { "name",    "come mi chiamo" }, { "name", "qual e il mio nome" }, { "name", "qual è il mio nome" },
    { "name",    "quale e il mio nome" }, { "name", "quale è il mio nome" }, { "name", "ti ricordi il mio nome" },
    { "name",    "what is my name" }, { "name", "what's my name" }, { "name", "do you know my name" },
    { "name",    "do you remember my name" }, { "name", "what am i called" },
    { "age",     "quanti anni ho" }, { "age", "che eta ho" }, { "age", "che età ho" },
    { "age",     "how old am i" }, { "age", "what is my age" }, { "age", "what's my age" },
    { "city",    "dove abito" }, { "city", "dove vivo" }, { "city", "in che citta vivo" }, { "city", "in che città vivo" },
    { "city",    "in quale citta abito" }, { "city", "where do i live" }, { "city", "what city do i live in" },
    { "job",     "che lavoro faccio" }, { "job", "qual e il mio lavoro" }, { "job", "qual è il mio lavoro" },
    { "job",     "che lavoro ho" }, { "job", "what is my job" }, { "job", "what's my job" },
    { "job",     "what do i do for work" }, { "job", "what do i do for a living" },
    { "email",   "qual e la mia email" }, { "email", "qual è la mia email" }, { "email", "qual e la mia mail" },
    { "email",   "qual è la mia mail" }, { "email", "what is my email" }, { "email", "what's my email" },
    { "birthday","quando e il mio compleanno" }, { "birthday", "quando è il mio compleanno" },
    { "birthday","quando sono nato" }, { "birthday", "quando sono nata" }, { "birthday", "when is my birthday" },
    { "birthday","when's my birthday" }, { "birthday", "what's my birthday" },
    { NULL, NULL },
};

// SET patterns (statements). value = remainder after the lead. Longer/more-specific leads first.
static const pat_t SET[] = {
    { "name",    "mi chiamo " }, { "name", "il mio nome e " }, { "name", "il mio nome è " },
    { "name",    "puoi chiamarmi " }, { "name", "chiamami " }, { "name", "my name is " },
    { "name",    "you can call me " }, { "name", "call me " }, { "name", "i'm called " }, { "name", "i am called " },
    { "city",    "abito a " }, { "city", "abito in " }, { "city", "vivo a " }, { "city", "vivo in " },
    { "city",    "i live in " }, { "city", "i live at " },
    // NB: no bare "faccio il/la X" — it is ambiguous ("faccio la doccia/spesa/somma" is not a job).
    { "job",     "di lavoro faccio " }, { "job", "il mio lavoro e " }, { "job", "il mio lavoro è " },
    { "job",     "lavoro come " }, { "job", "di mestiere faccio " },
    { "job",     "my job is " }, { "job", "i work as an " }, { "job", "i work as a " }, { "job", "i work as " },
    { "email",   "la mia email e " }, { "email", "la mia email è " }, { "email", "la mia mail e " },
    { "email",   "la mia mail è " }, { "email", "my email is " }, { "email", "my email address is " },
    { "birthday","il mio compleanno e " }, { "birthday", "il mio compleanno è " }, { "birthday", "sono nato il " },
    { "birthday","sono nata il " }, { "birthday", "my birthday is " }, { "birthday", "i was born on " },
    { NULL, NULL },
};

// Age SET leads (the value is the first integer that follows, with an "anni/years/age" cue present).
static const char *const AGE_SET[] = { "ho ", "i am ", "i'm ", "my age is ", NULL };

// localized field label for the summary
static const char *flabel(const char *f, bool en)
{
    if (!strcmp(f, "name"))     return en ? "name" : "nome";
    if (!strcmp(f, "age"))      return en ? "age" : "età";
    if (!strcmp(f, "city"))     return en ? "city" : "città";
    if (!strcmp(f, "job"))      return en ? "job" : "lavoro";
    if (!strcmp(f, "email"))    return "email";
    if (!strcmp(f, "birthday")) return en ? "birthday" : "compleanno";
    return f;
}

static void reply_recall(anima_result_t *r, const char *field, const char *val, bool en)
{
    if (!strcmp(field, "name"))      snprintf(r->reply, sizeof r->reply, en ? "Your name is %s." : "Ti chiami %s.", val);
    else if (!strcmp(field, "age"))  snprintf(r->reply, sizeof r->reply, en ? "You are %s years old." : "Hai %s anni.", val);
    else if (!strcmp(field, "city")) snprintf(r->reply, sizeof r->reply, en ? "You live in %s." : "Abiti a %s.", val);
    else if (!strcmp(field, "job"))  snprintf(r->reply, sizeof r->reply, en ? "Your job: %s." : "Il tuo lavoro: %s.", val);
    else if (!strcmp(field, "email"))snprintf(r->reply, sizeof r->reply, en ? "Your email is %s." : "La tua email è %s.", val);
    else                             snprintf(r->reply, sizeof r->reply, en ? "Your birthday is %s." : "Il tuo compleanno è %s.", val);
}

static void reply_unset(anima_result_t *r, const char *field, bool en)
{
    if (!strcmp(field, "name"))
        snprintf(r->reply, sizeof r->reply, en ? "I don't know your name yet — tell me!" : "Non so ancora come ti chiami — dimmelo!");
    else
        snprintf(r->reply, sizeof r->reply, en ? "I don't know your %s yet — tell me!" : "Non conosco ancora il tuo/la tua %s — dimmelo!", flabel(field, en));
}

static void fill(anima_result_t *r, bool set)
{
    r->tier = ANIMA_TIER_COMMAND; r->action = ANIMA_ACT_ANSWER; r->confidence = 92;
    snprintf(r->intent, sizeof r->intent, "profile");
    snprintf(r->state, sizeof r->state, "tool");
    (void)set;
}

// Build the "what do you know about me" summary from whatever fields are set.
static int do_summary(anima_result_t *r, bool en)
{
    static const char *const ORDER[] = { "name", "age", "city", "job", "email", "birthday", NULL };
    char body[320]; int o = 0; int count = 0;
    for (int i = 0; ORDER[i]; i++) {
        char v[P_VAL];
        if (pget(ORDER[i], v, sizeof v)) {
            int w = snprintf(body + o, sizeof(body) - o, "%s%s: %s", count ? ", " : "", flabel(ORDER[i], en), v);
            if (w > 0 && o + w < (int)sizeof(body)) o += w;
            count++;
        }
    }
    fill(r, false);
    if (!count) snprintf(r->reply, sizeof r->reply, en ? "I don't know anything about you yet — tell me about yourself!"
                                                       : "Non so ancora niente di te — raccontami qualcosa!");
    else snprintf(r->reply, sizeof r->reply, en ? "Here's what I know about you: %s." : "Ecco cosa so di te: %s.", body);
    return 1;
}

// A plausible name: <=4 words and not starting with a temporal/question word — so "chiamami quando
// arrivi" (call me WHEN you arrive) does NOT mis-set the name, while "chiamami Capo" does.
static bool name_plausible(const char *v)
{
    char first[24]; int n = 0; const char *p = v; while (*p == ' ') p++;
    while (p[n] && p[n] != ' ' && n < 23) { first[n] = (char)tolower((unsigned char)p[n]); n++; }
    first[n] = 0;
    static const char *const bad[] = { "quando","when","se","if","dopo","after","appena","once","piu","più",
        "oggi","domani","stasera","ora","adesso","now","perche","perché","why","come","how","tra","fra","in", NULL };
    for (int i = 0; bad[i]; i++) if (!strcmp(first, bad[i])) return false;
    int words = 1; for (const char *q = v; *q; q++) if (*q == ' ' && q[1] && q[1] != ' ') words++;
    return words <= 4;
}

int nucleo_anima_profile(const char *raw, bool en, anima_result_t *r)
{
    if (!raw || !*raw) return 0;
    // Collapse runs of whitespace to a single space (and trim) so a literal phrase match ("mi chiamo ")
    // survives sloppy spacing ("mi    chiamo   Anna"). rawc is the source for value extraction too, so the
    // stored name is clean; lo is built from rawc and stays byte-aligned with it.
    char rawc[P_LO]; { size_t o = 0; bool sp = false;
        for (const char *p = raw; *p && o < sizeof(rawc) - 1; p++) {
            if (*p == ' ' || *p == '\t') { sp = true; continue; }
            if (sp && o) rawc[o++] = ' ';
            sp = false; rawc[o++] = *p; }
        rawc[o] = 0; }
    raw = rawc;
    char lo[P_LO]; prep_lo(raw, lo, sizeof lo);

    // 1) RECALL first (questions). A whole-profile summary, a per-field recall, or an honest "don't know yet".
    for (int i = 0; RECALL[i].lead; i++) {
        if (!strstr(lo, RECALL[i].lead)) continue;
        if (RECALL[i].field[0] == 0) return do_summary(r, en);
        char v[P_VAL];
        fill(r, false);
        if (pget(RECALL[i].field, v, sizeof v)) reply_recall(r, RECALL[i].field, v, en);
        else reply_unset(r, RECALL[i].field, en);
        return 1;
    }

    // 2) AGE set: a first-person age cue + an "anni/years/age" word + a number -> store the number.
    if ((strstr(lo, " anni") || strstr(lo, "years") || strstr(lo, "year old") || strstr(lo, "my age is"))) {
        for (int i = 0; AGE_SET[i]; i++) {
            const char *p = wb_find(lo, AGE_SET[i]);
            if (!p) continue;
            char num[8];
            if (first_int(p, num, sizeof num) > 0 && atoi(num) > 0 && atoi(num) < 130) {
                pset("age", num);
                fill(r, true);
                snprintf(r->reply, sizeof r->reply, en ? "Got it — you're %s." : "Ok, hai %s anni: me lo ricorderò.", num);
                return 1;
            }
        }
    }

    // 3) Field SET (statements). Value = remainder after the lead (from raw, case preserved).
    for (int i = 0; SET[i].lead; i++) {
        const char *p = wb_find(lo, SET[i].lead);
        if (!p) continue;
        char val[P_VAL];
        size_t off = (size_t)(p - lo) + strlen(SET[i].lead);
        if (extract_value(raw, off, val, sizeof val) < 1) continue;     // empty value -> not a set
        if (!strcmp(SET[i].field, "name") && !name_plausible(val)) continue;   // "chiamami quando arrivi" -> not a name
        pset(SET[i].field, val);
        fill(r, true);
        if (!strcmp(SET[i].field, "name"))      snprintf(r->reply, sizeof r->reply, en ? "Nice to meet you, %s!" : "Piacere, %s!", val);
        else if (!strcmp(SET[i].field, "city")) snprintf(r->reply, sizeof r->reply, en ? "Got it — you live in %s." : "Ok, ricorderò che abiti a %s.", val);
        else if (!strcmp(SET[i].field, "job"))  snprintf(r->reply, sizeof r->reply, en ? "Got it — your job: %s." : "Ok, ricorderò il tuo lavoro: %s.", val);
        else if (!strcmp(SET[i].field, "email"))snprintf(r->reply, sizeof r->reply, en ? "Saved your email: %s." : "Ho salvato la tua email: %s.", val);
        else                                    snprintf(r->reply, sizeof r->reply, en ? "Got it — I'll remember: %s." : "Ok, me lo ricorderò: %s.", val);
        return 1;
    }

    return 0;
}
