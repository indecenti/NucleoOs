// ANIMA user-teach tier — see nucleo_anima_learn.h. Network-free (encoder + libc only), so it also
// compiles and runs in the host harness, unlike the Wikipedia-fed online tier. Mirrors that tier's
// on-disk vector format and its streaming, O(dim)-RAM rewrite — no full-file load, safe on the PSRAM-less
// device. Store lives directly under /data/anima/ (which already exists on host AND device) so no mkdir.
#include "nucleo_anima_learn.h"
#include "anima_l1.h"          // nucleo_anima_l1_encode / _dim (the shared distilled encoder)
#include "anima_internal.h"    // a_damlev (lexical-corroboration channel)
#include "nucleo_board.h"      // NUCLEO_SD_MOUNT
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>              // sqrt

#define LEARN_DIM     256       // == L1_MAXDIM / RECALL_DIM: the widest encoder vector we buffer
#define LEARN_MAX     128       // bounded store: drop the oldest beyond this many user facts
#define LEARN_FLOOR   0.68f     // semantic floor: a real paraphrase of a short subject lands ~0.70-0.80 (measured),
                                // while an unrelated query sits ~0.20 — a wide margin. The full-coverage lexical
                                // channel (below) is the discriminative guard. Together: refuse rather than misattribute.
#define U_TRIG_MAX    120       // stored trigger (subject) cap
#define U_REPLY_MAX   360       // stored reply (fact) cap — fits anima_result_t.reply
#define U_ID_MAX      72        // "user." + slug

#define U_TSV  NUCLEO_SD_MOUNT "/data/anima/user.tsv"   // id \t trigger \t reply  (one per line)
#define U_VEC  NUCLEO_SD_MOUNT "/data/anima/user.vec"   // u8 idlen | id | u8 dim | int8 vec[dim]

// Fold a lowercase Italian accented vowel (the byte AFTER 0xC3 in UTF-8) to bare ASCII; 0 if not one.
static char fold_it(unsigned char d)
{
    if (d >= 0xA0 && d <= 0xA5) return 'a';   // à á â ã ä å
    if (d >= 0xA8 && d <= 0xAB) return 'e';   // è é ê ë
    if (d >= 0xAC && d <= 0xAF) return 'i';   // ì í î ï
    if (d == 0xB1)              return 'n';   // ñ
    if (d >= 0xB2 && d <= 0xB6) return 'o';   // ò ó ô õ ö
    if (d >= 0xB9 && d <= 0xBC) return 'u';   // ù ú û ü
    return 0;
}

// alnum + accent-fold -> hyphen-separated slug (the internal id stem). Collisions just overwrite (fine).
static void learn_slug(char *dst, int cap, const char *src)
{
    int o = 0; bool sep = false;
    for (const unsigned char *p = (const unsigned char *)src; *p && o < cap - 1; p++) {
        char c = 0;
        if (*p == 0xC3 && p[1]) { c = fold_it(p[1]); if (c) p++; }
        else if (isalnum(*p)) c = (char)tolower(*p);
        if (c) { if (sep && o > 0) dst[o++] = '-'; if (o < cap - 1) dst[o++] = c; sep = false; }
        else if (o > 0) sep = true;            // collapse runs; never a leading separator
    }
    dst[o] = 0;
}

// Copy into a single TSV field: tabs/newlines -> space, trim ends, cap length. Keeps one record per line.
static void san_line(char *dst, int cap, const char *src)
{
    int o = 0;
    while (*src == ' ') src++;
    for (const unsigned char *p = (const unsigned char *)src; *p && o < cap - 1; p++) {
        unsigned char c = *p;
        dst[o++] = (c == '\t' || c == '\n' || c == '\r') ? ' ' : (char)c;
    }
    while (o > 0 && dst[o - 1] == ' ') o--;
    dst[o] = 0;
}

// Split into lowercased, accent-folded content words (alnum runs). Returns the count (<= maxw).
static int words_of(const char *s, char w[][24], int maxw)
{
    int n = 0, o = 0; const unsigned char *p = (const unsigned char *)s;
    while (n < maxw) {
        unsigned char ch = *p; char c = 0; int adv = 1;
        if (ch == 0xC3 && p[1]) { c = fold_it(p[1]); adv = 2; }
        else if (isalnum(ch))   c = (char)tolower(ch);
        if (c) { if (o < 23) w[n][o++] = c; }
        else if (o > 0) { w[n][o] = 0; n++; o = 0; }
        if (ch == 0) break;
        p += adv;
    }
    return n;
}

// Tiny IT+EN stop-word set: these never count as distinguishing content for coverage.
static bool is_stopword(const char *w)
{
    static const char *const S[] = {
        "il","lo","la","le","gli","un","uno","una","di","del","della","dei","delle","dello","degli",
        "da","in","con","su","per","tra","fra","al","allo","alla","ai","agli","alle","che"," del",
        "the","a","an","of","to","for","is","are","and","or","on","at","by","my","your","that","this", NULL };
    for (int i = 0; S[i]; i++) if (!strcmp(w, S[i])) return true;
    return false;
}

// Lexical channel (FULL COVERAGE): EVERY salient (len>=3, non-stop) content word of the matched TRIGGER
// must appear in the query (Damerau<=1, typo-tolerant). This is the discriminative guard: it is what stops
// a same-shape, different-subject query — "il compleanno di MARCO" against the trigger "il compleanno di
// LUCA" — from being wrongly rescued, because the distinguishing word (luca) is REQUIRED and absent. It is
// corroborated against the trigger (not the reply), so it stays symmetric across IT and EN. Mirrors the
// full-coverage spirit of L1's l1_lexically_corroborated.
static bool lex_corroborated(const char *query, const char *trigger)
{
    char qw[24][24], tw[24][24];
    int nq = words_of(query, qw, 24), nt = words_of(trigger, tw, 24);
    int required = 0, covered = 0;
    for (int i = 0; i < nt; i++) {
        if (strlen(tw[i]) < 3 || is_stopword(tw[i])) continue;
        required++;
        for (int j = 0; j < nq; j++) {
            if (strlen(qw[j]) < 3) continue;
            if (a_damlev(tw[i], qw[j], 1) <= 1) { covered++; break; }
        }
    }
    return required > 0 && covered == required;     // all distinguishing trigger words present
}

bool nucleo_anima_learn_is_volatile(const char *text)
{
    char lo[512]; int o = 0;
    lo[o++] = ' ';
    for (const unsigned char *p = (const unsigned char *)text; *p && o < (int)sizeof(lo) - 2; p++)
        lo[o++] = (char)tolower(*p);
    lo[o++] = ' '; lo[o] = 0;
    // Volatility law (docs/anima-online.md §6): a "now / today / weather / price" claim must never be
    // frozen as a timeless fact. Markers padded with spaces are matched word-bounded.
    static const char *const T[] = {
        " oggi ", " domani ", " dopodomani ", " stamattina ", " stamani ", " stasera ", " stanotte ",
        " adesso ", " in questo momento ", " in tempo reale ", " questa settimana ", " questo mese ",
        " attuale ", " attualmente ", " di oggi ", " meteo ", " previsioni ",
        " today ", " tomorrow ", " tonight ", " right now ", " this week ", " this month ",
        " latest ", " currently ", " weather ", " forecast ", " price ", " prezzo ", " quotazione ", NULL };
    for (int i = 0; T[i]; i++) if (strstr(lo, T[i])) return true;
    return false;
}

// Rewrite the binary sidecar streaming (O(dim) RAM): drop the prior record for `id` and the oldest
// beyond LEARN_MAX, append the fresh vector last. Atomic temp + rename (FatFs: remove the stale copy
// first, since rename() won't overwrite). Record: u8 idlen | id | u16 dim(LE) | int8 vec[dim].
// NOTE: dim is u16 (not u8 like the online vec_sync) so it survives a 256-dim encoder — the device runs
// the 192-dim AUG encoder, but the host harness runs the 256-dim one, and 256 would overflow a byte to 0.
static void vec_put(const char *id, const int8_t *v, int D)
{
    uint8_t idl = (uint8_t)strlen(id);
    if (idl == 0 || idl >= 80) return;

    int total = 0;
    FILE *in = fopen(U_VEC, "rb");
    if (in) {
        uint8_t l; unsigned char db[2]; char rid[80];
        while (fread(&l, 1, 1, in) == 1) {
            if (l == 0 || l >= sizeof(rid) || fread(rid, 1, l, in) != l || fread(db, 1, 2, in) != 2) break;
            int d = db[0] | (db[1] << 8);
            if (fseek(in, d, SEEK_CUR) != 0) break;
            if (!(l == idl && !memcmp(rid, id, l))) total++;
        }
        fclose(in);
    }
    int skip = total >= LEARN_MAX ? (total - (LEARN_MAX - 1)) : 0;

    char tmp[180]; snprintf(tmp, sizeof(tmp), "%s.tmp", U_VEC);
    FILE *out = fopen(tmp, "wb");
    if (!out) return;
    in = fopen(U_VEC, "rb");
    if (in) {
        uint8_t l; unsigned char db[2]; static char rid[80]; static int8_t rv[LEARN_DIM];
        while (fread(&l, 1, 1, in) == 1) {
            if (l == 0 || l >= sizeof(rid) || fread(rid, 1, l, in) != l || fread(db, 1, 2, in) != 2) break;
            int d = db[0] | (db[1] << 8);
            if (d <= 0 || d > LEARN_DIM) { if (fseek(in, d, SEEK_CUR) != 0) break; continue; }
            if (fread(rv, 1, d, in) != (size_t)d) break;
            if (l == idl && !memcmp(rid, id, l)) continue;   // replaced by the fresh vector
            if (skip > 0) { skip--; continue; }              // drop oldest to stay bounded
            fwrite(&l, 1, 1, out); fwrite(rid, 1, l, out); fwrite(db, 1, 2, out); fwrite(rv, 1, d, out);
        }
        fclose(in);
    }
    unsigned char dd[2] = { (unsigned char)(D & 0xFF), (unsigned char)((D >> 8) & 0xFF) };
    fwrite(&idl, 1, 1, out); fwrite(id, 1, idl, out); fwrite(dd, 1, 2, out); fwrite(v, 1, D, out);
    fclose(out);
    remove(U_VEC);
    if (rename(tmp, U_VEC) != 0) remove(tmp);
}

// Rewrite the text record file in lockstep with the sidecar: same drop-same-id + oldest-eviction policy.
static void tsv_put(const char *id, const char *trig, const char *reply)
{
    size_t idl = strlen(id);

    int total = 0;
    FILE *in = fopen(U_TSV, "r");
    if (in) {
        static char ln[640];
        while (fgets(ln, sizeof(ln), in)) {
            char *tab = strchr(ln, '\t'); if (!tab) continue;
            size_t k = (size_t)(tab - ln);
            if (!(k == idl && !memcmp(ln, id, k))) total++;
        }
        fclose(in);
    }
    int skip = total >= LEARN_MAX ? (total - (LEARN_MAX - 1)) : 0;

    char tmp[180]; snprintf(tmp, sizeof(tmp), "%s.tmp", U_TSV);
    FILE *out = fopen(tmp, "w");
    if (!out) return;
    in = fopen(U_TSV, "r");
    if (in) {
        static char ln[640];
        while (fgets(ln, sizeof(ln), in)) {
            char *tab = strchr(ln, '\t'); if (!tab) continue;
            size_t k = (size_t)(tab - ln);
            if (k == idl && !memcmp(ln, id, k)) continue;
            if (skip > 0) { skip--; continue; }
            fputs(ln, out);
            size_t L = strlen(ln); if (L == 0 || ln[L - 1] != '\n') fputc('\n', out);
        }
        fclose(in);
    }
    fprintf(out, "%s\t%s\t%s\n", id, trig, reply);
    fclose(out);
    remove(U_TSV);
    if (rename(tmp, U_TSV) != 0) remove(tmp);
}

// Fetch the trigger + reply for an exact id from the TSV. Returns 1 on hit.
static int tsv_get(const char *id, char *trig, int tcap, char *reply, int rcap)
{
    FILE *f = fopen(U_TSV, "r");
    if (!f) return 0;
    size_t idl = strlen(id); int found = 0; static char ln[640];
    while (fgets(ln, sizeof(ln), f)) {
        char *t1 = strchr(ln, '\t'); if (!t1) continue;
        size_t k = (size_t)(t1 - ln);
        if (!(k == idl && !memcmp(ln, id, k))) continue;
        char *t2 = strchr(t1 + 1, '\t'); if (!t2) continue;
        size_t tl = (size_t)(t2 - (t1 + 1)); if (tl >= (size_t)tcap) tl = tcap - 1;
        memcpy(trig, t1 + 1, tl); trig[tl] = 0;
        char *rp = t2 + 1; size_t rl = strlen(rp);
        while (rl && (rp[rl - 1] == '\n' || rp[rl - 1] == '\r')) rl--;
        if (rl >= (size_t)rcap) rl = rcap - 1;
        memcpy(reply, rp, rl); reply[rl] = 0;
        found = 1; break;
    }
    fclose(f);
    return found;
}

int nucleo_anima_learn_put(const char *subject, const char *fact, bool en)
{
    (void)en;
    if (!subject || !fact || !*subject || !*fact) return 0;

    char both[512]; snprintf(both, sizeof(both), "%s %s", subject, fact);
    if (nucleo_anima_learn_is_volatile(both)) return -1;     // never freeze a volatile statement

    int D = nucleo_anima_l1_dim();
    if (D <= 0 || D > LEARN_DIM) return 0;                    // encoder absent -> recall would be off
    static int8_t v[LEARN_DIM];
    if (nucleo_anima_l1_encode(subject, v, LEARN_DIM) != D) return 0;

    char slug[64]; learn_slug(slug, sizeof(slug), subject);
    if (!slug[0]) return 0;
    char id[U_ID_MAX]; snprintf(id, sizeof(id), "user.%s", slug);

    char trig[U_TRIG_MAX + 1], reply[U_REPLY_MAX + 1];
    san_line(trig, sizeof(trig), subject);
    san_line(reply, sizeof(reply), fact);
    if (!trig[0] || !reply[0]) return 0;

    vec_put(id, v, D);
    tsv_put(id, trig, reply);
    return 1;
}

int nucleo_anima_learn_recall(const char *query, bool en, anima_result_t *out)
{
    (void)en;
    int D = nucleo_anima_l1_dim();
    if (D <= 0 || D > LEARN_DIM || !query) return 0;

    FILE *in = fopen(U_VEC, "rb");                            // nothing learned -> skip the encode entirely
    if (!in) return 0;

    static int8_t qv[LEARN_DIM];
    if (nucleo_anima_l1_encode(query, qv, LEARN_DIM) != D) { fclose(in); return 0; }
    double qn = 0; for (int k = 0; k < D; k++) qn += (double)qv[k] * qv[k];
    qn = sqrt(qn); if (qn < 1e-9) { fclose(in); return 0; }

    char bestid[80] = ""; float best = -2.0f;
    static char rid[80]; static int8_t rv[LEARN_DIM]; uint8_t l; unsigned char db[2];
    while (fread(&l, 1, 1, in) == 1) {
        if (l == 0 || l >= sizeof(rid) || fread(rid, 1, l, in) != l || fread(db, 1, 2, in) != 2) break;
        int d = db[0] | (db[1] << 8);
        if (d != D) { if (fseek(in, d, SEEK_CUR) != 0) break; continue; }
        if (fread(rv, 1, d, in) != (size_t)d) break;
        long dot = 0; double vn = 0;
        for (int k = 0; k < D; k++) { dot += (int)qv[k] * rv[k]; vn += (double)rv[k] * rv[k]; }
        float cos = (float)(dot / (qn * sqrt(vn) + 1e-9));
        if (cos > best) { best = cos; rid[l] = 0; snprintf(bestid, sizeof(bestid), "%s", rid); }
    }
    fclose(in);
    if (best < LEARN_FLOOR || !bestid[0]) return 0;          // semantic floor: not confidently the same topic

    char trig[U_TRIG_MAX + 1], reply[U_REPLY_MAX + 1];
    if (!tsv_get(bestid, trig, sizeof(trig), reply, sizeof(reply))) return 0;   // record evicted since

    // Dual-channel evidence: the semantic floor is necessary but NOT sufficient — a same-shape query with a
    // different subject ("compleanno di marco" vs "...luca") can still clear it. So ALSO require full lexical
    // coverage of the trigger's distinguishing words. Both channels must agree, or ANIMA abstains.
    if (!lex_corroborated(query, trig)) return 0;

    memset(out, 0, sizeof(*out));
    out->tier = ANIMA_TIER_FACT; out->action = ANIMA_ACT_ANSWER;
    out->confidence = (int)(best * 100.0f + 0.5f);
    snprintf(out->intent, sizeof(out->intent), "recall");
    snprintf(out->reply, sizeof(out->reply), "%s", reply);
    return 1;
}
