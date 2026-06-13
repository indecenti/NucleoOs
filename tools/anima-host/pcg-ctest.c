// ON-DEVICE C proof of NSPCG (Neuro-Symbolic Proof-Carrying Generation) — the firmware-grade twin of
// tools/anima/pcg.mjs. Same standard as entity-ctest.c: the HDC primitives (fnv1a/rotate/bundle/cleanup/
// semantic/rel_shift) are bit-identical to nucleo_anima_hdc.c (the project keeps C firmware + hdc.mjs +
// selftest in lockstep; this adds the device-grade PCG path). The NEW on-device logic is reach() (the
// autonomous beam search the firmware lacked), walk+ground (HDC discovers, the symbol table verifies),
// the proof-carrying verbalizer, and the standalone proof checker. Proves it compiles under the device
// toolchain (gnu11) and behaves exactly like the JS twin — fully on host, ZERO change to the live cascade.
//   gcc -std=gnu11 -O0 tools/anima-host/pcg-ctest.c -o build/pcgctest -lm && build/pcgctest
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// ===================== HDC primitives — bit-identical to nucleo_anima_hdc.c =====================
#define HDC_D 8192
#define HDC_W (HDC_D / 32)
#define HDC_STD 45.25f
#define GATE 4.0f

static uint32_t fnv1a(const char *s) { uint32_t h = 2166136261u; while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; } return h; }
static uint32_t s_rng = 1;
static inline void rseed(uint32_t s) { s_rng = s ? s : 1; }
static inline uint32_t rnd(void) { uint32_t x = s_rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; s_rng = x; return x; }
static inline int hamming(const uint32_t *a, const uint32_t *b) { int d = 0; for (int i = 0; i < HDC_W; i++) d += __builtin_popcount(a[i] ^ b[i]); return d; }
static void hv_rotate(uint32_t *o, const uint32_t *a, int k) {
    k %= HDC_D; if (k < 0) k += HDC_D;
    for (int i = 0; i < HDC_W; i++) o[i] = 0;
    for (int i = 0; i < HDC_D; i++) if ((a[i >> 5] >> (i & 31)) & 1) { int j = i + k; if (j >= HDC_D) j -= HDC_D; o[j >> 5] |= (1u << (j & 31)); }
}
static int8_t *g_simcnt, *g_bcnt;
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
static void hv_bundle(uint32_t *o, const uint32_t *items, int n) {
    memset(g_bcnt, 0, (size_t)HDC_D * sizeof(int8_t));
    for (int k = 0; k < n; k++) { const uint32_t *v = items + (size_t)k * HDC_W; for (int i = 0; i < HDC_D; i++) g_bcnt[i] += (v[i >> 5] >> (i & 31)) & 1; }
    for (int i = 0; i < HDC_W; i++) o[i] = 0;
    for (int i = 0; i < HDC_D; i++) if (g_bcnt[i] * 2 > n) o[i >> 5] |= (1u << (i & 31));
}
typedef struct { int idx; int dist; float coh; } cleanup_t;
static cleanup_t cleanup(const uint32_t *probe, const uint32_t *cb, int n) {
    int bd = 1 << 30, sd = 1 << 30, bi = -1;
    for (int i = 0; i < n; i++) { int d = hamming(probe, cb + (size_t)i * HDC_W); if (d < bd) { sd = bd; bd = d; bi = i; } else if (d < sd) sd = d; }
    cleanup_t r; r.idx = bi; r.dist = bd; r.coh = (float)(sd - bd) / HDC_STD; return r;
}
static int rel_shift(const char *rel) { char b[48]; snprintf(b, sizeof b, "rel:%s", rel); return 1 + (int)(fnv1a(b) % (HDC_D - 2)); }

// ===================== tiny in-RAM knowledge graph (multi-relation) =====================
#define PMAXENT 16
#define PMAXTRIP 32
#define PNAME 32
typedef struct {
    char name[PMAXENT][PNAME];
    int  n;
    int  eh[PMAXTRIP], et[PMAXTRIP], ek[PMAXTRIP];   // head idx, tail idx, rotation amount
    char erel[PMAXTRIP][PNAME];                       // relation NAME (verbalize + closure + verify)
    int  ntrip;
    uint32_t *cb;                                     // codebook: n consecutive entity HVs
} kg_t;

static int kg_intern(kg_t *g, const char *name) {
    for (int i = 0; i < g->n; i++) if (!strcmp(g->name[i], name)) return i;
    if (g->n >= PMAXENT) return -1;
    int i = g->n++; snprintf(g->name[i], PNAME, "%s", name); return i;
}
static void kg_add(kg_t *g, const char *h, const char *rel, const char *t) {
    if (g->ntrip >= PMAXTRIP) return;
    int hi = kg_intern(g, h), ti = kg_intern(g, t); if (hi < 0 || ti < 0) return;
    int e = g->ntrip++; g->eh[e] = hi; g->et[e] = ti; g->ek[e] = rel_shift(rel);
    snprintf(g->erel[e], PNAME, "%s", rel);
}
// build the embedding (port of kg_build): roots = semantic anchor; others = majority-bundle of
// [semanticHV(self), rotate(E[head],k) for each incoming edge]; sweep (depth+2) times.
static int kg_build(kg_t *g) {
    int n = g->n, dep[PMAXENT];
    for (int i = 0; i < n; i++) dep[i] = 0;
    for (int pass = 0; pass < n; pass++) { int ch = 0; for (int e = 0; e < g->ntrip; e++) { int h = g->eh[e], t = g->et[e]; if (dep[h] + 1 > dep[t]) { dep[t] = dep[h] + 1; ch = 1; } } if (!ch) break; }
    int depth = 1; for (int i = 0; i < n; i++) if (dep[i] + 1 > depth) depth = dep[i] + 1; if (depth > 16) depth = 16;
    int sweeps = depth + 2;
    uint32_t *E = malloc((size_t)n * HDC_W * 4), *next = malloc((size_t)n * HDC_W * 4), *stg = malloc((size_t)(PMAXENT + 1) * HDC_W * 4);
    if (!E || !next || !stg) { free(E); free(next); free(stg); return 0; }
    #define EV(i) (E + (size_t)(i) * HDC_W)
    #define NV(i) (next + (size_t)(i) * HDC_W)
    #define SV(i) (stg + (size_t)(i) * HDC_W)
    for (int i = 0; i < n; i++) hv_semantic(EV(i), g->name[i]);
    for (int s = 0; s < sweeps; s++) {
        for (int e = 0; e < n; e++) {
            hv_semantic(SV(0), g->name[e]); int cnt = 1;
            for (int k = 0; k < g->ntrip && cnt <= PMAXENT; k++) if (g->et[k] == e) { hv_rotate(SV(cnt), EV(g->eh[k]), g->ek[k]); cnt++; }
            if (cnt == 1) hv_semantic(NV(e), g->name[e]); else hv_bundle(NV(e), SV(0), cnt);
        }
        memcpy(E, next, (size_t)n * HDC_W * 4);
    }
    g->cb = E; free(next); free(stg);
    #undef EV
    #undef NV
    #undef SV
    return 1;
}

// distinct relations present in the graph (reach explores all of them, blind to which node owns which)
static int kg_rels(const kg_t *g, char out[][PNAME], int cap) {
    int n = 0;
    for (int e = 0; e < g->ntrip; e++) { int seen = 0; for (int j = 0; j < n; j++) if (!strcmp(out[j], g->erel[e])) { seen = 1; break; } if (!seen && n < cap) snprintf(out[n++], PNAME, "%s", g->erel[e]); }
    return n;
}
static int kg_is_edge(const kg_t *g, int h, const char *rel, int t) {
    for (int e = 0; e < g->ntrip; e++) if (g->eh[e] == h && g->et[e] == t && !strcmp(g->erel[e], rel)) return 1;
    return 0;
}

// ===================== reach() — AUTONOMOUS widest-path discovery (the firmware lacked this) =====================
// Max-min-coherence BFS over the built embedding: from `head`, try EVERY relation at each node, cleanup,
// keep hops above the gate, and record the chain that maximizes the WEAKEST hop's coherence to each node.
// Back-pointers reconstruct the path. The device discovers the chain itself — no path is supplied.
static int reach_path(const kg_t *g, int head, int target, char relseq[][PNAME], int *nhops, float *mincoh, int maxDepth) {
    char rels[PMAXTRIP][PNAME]; int nrel = kg_rels(g, rels, PMAXTRIP);
    float best[PMAXENT]; int from[PMAXENT]; char fromrel[PMAXENT][PNAME];
    for (int i = 0; i < g->n; i++) { best[i] = -1e9f; from[i] = -2; fromrel[i][0] = 0; }
    best[head] = 1e9f; from[head] = -1;
    uint32_t *probe = malloc((size_t)HDC_W * 4); if (!probe) return 0;
    int layer[PMAXENT], nlayer = 1; layer[0] = head;
    for (int d = 0; d < maxDepth && nlayer; d++) {
        int nxt[PMAXENT], nn = 0;
        for (int li = 0; li < nlayer; li++) {
            int u = layer[li];
            for (int r = 0; r < nrel; r++) {
                hv_rotate(probe, g->cb + (size_t)u * HDC_W, rel_shift(rels[r]));
                cleanup_t c = cleanup(probe, g->cb, g->n);
                if (c.idx < 0 || c.idx == u || c.coh < GATE) continue;
                float w = best[u] < c.coh ? best[u] : c.coh;
                if (w > best[c.idx]) {
                    best[c.idx] = w; from[c.idx] = u; snprintf(fromrel[c.idx], PNAME, "%s", rels[r]);
                    int seen = 0; for (int j = 0; j < nn; j++) if (nxt[j] == c.idx) { seen = 1; break; }
                    if (!seen && nn < PMAXENT) nxt[nn++] = c.idx;
                }
            }
        }
        for (int i = 0; i < nn; i++) layer[i] = nxt[i]; nlayer = nn;
    }
    free(probe);
    if (from[target] == -2) return 0;                 // unreachable
    // reconstruct head..target
    int chain[PMAXENT], cn = 0; char crel[PMAXENT][PNAME]; int cur = target;
    while (cur != head && from[cur] >= -1 && cn < PMAXENT) { chain[cn] = cur; snprintf(crel[cn], PNAME, "%s", fromrel[cur]); cn++; if (from[cur] < 0) break; cur = from[cur]; }
    if (cur != head) return 0;
    *nhops = cn; *mincoh = best[target];
    for (int i = 0; i < cn; i++) snprintf(relseq[i], PNAME, "%s", crel[cn - 1 - i]);  // reverse to head..target order
    return 1;
}

// ===================== relation algebra + surface realization (mirror pcg.mjs) =====================
static int is_containment(const char *r) { return !strcmp(r, "si_trova_in") || !strcmp(r, "capitale_di") || !strcmp(r, "parte_di"); }
static void closure_rel(char relseq[][PNAME], int n, char *out, size_t cap) {
    int allc = 1; for (int i = 0; i < n; i++) if (!is_containment(relseq[i])) { allc = 0; break; }
    if (allc) { snprintf(out, cap, "si_trova_in"); return; }
    int rest = 1; for (int i = 1; i < n; i++) if (!is_containment(relseq[i])) { rest = 0; break; }
    if (!strcmp(relseq[0], "nato_in") && rest) { snprintf(out, cap, "nato_in"); return; }
    snprintf(out, cap, "connesso");
}
static void deslug(const char *s, char *out, size_t cap) { size_t o = 0; for (size_t i = 0; s[i] && o + 1 < cap; i++) out[o++] = (s[i] == '_' || s[i] == '-') ? ' ' : s[i]; out[o] = 0; }
static void cap_words(const char *s, char *out, size_t cap) {
    char d[96]; deslug(s, d, sizeof d); size_t o = 0; int start = 1;
    for (size_t i = 0; d[i] && o + 1 < cap; i++) { char c = d[i]; if (start && c >= 'a' && c <= 'z') c = (char)(c - 32); out[o++] = c; start = (d[i] == ' '); }
    out[o] = 0;
}
// Italian "di + article" contraction for the object of a relation.
static void di_of(const char *name, char *out, size_t cap) {
    char d[96]; deslug(name, d, sizeof d); char l = (char)tolower((unsigned char)d[0]);
    char C[96]; cap_words(name, C, sizeof C);
    if (l == 'a' || l == 'e' || l == 'i' || l == 'o' || l == 'u') { snprintf(out, cap, "dell'%s", C); return; }
    char ln[96]; for (size_t i = 0; d[i]; i++) ln[i] = (char)tolower((unsigned char)d[i]), ln[i + 1] = 0;
    const char *m[] = { "giappone", "regno unito", "portogallo", "brasile", NULL };
    const char *f[] = { "francia", "spagna", "germania", "cina", NULL };
    for (int i = 0; m[i]; i++) if (!strcmp(ln, m[i])) { snprintf(out, cap, "del %s", C); return; }
    for (int i = 0; f[i]; i++) if (!strcmp(ln, f[i])) { snprintf(out, cap, "della %s", C); return; }
    snprintf(out, cap, "di %s", C);
}
// one verified edge -> one grounded clause
static void clause(int en, const char *h, const char *rel, const char *t, char *out, size_t cap) {
    char H[96], T[96]; cap_words(h, H, sizeof H); cap_words(t, T, sizeof T);
    if (!strcmp(rel, "si_trova_in")) snprintf(out, cap, en ? "%s is located in %s" : "%s si trova in %s", H, T);
    else if (!strcmp(rel, "capitale_di")) { if (en) snprintf(out, cap, "%s is the capital of %s", H, T); else { char di[96]; di_of(t, di, sizeof di); snprintf(out, cap, "%s è la capitale %s", H, di); } }
    else if (!strcmp(rel, "parte_di")) { if (en) snprintf(out, cap, "%s is part of %s", H, T); else { char di[96]; di_of(t, di, sizeof di); snprintf(out, cap, "%s fa parte %s", H, di); } }
    else if (!strcmp(rel, "nato_in")) snprintf(out, cap, en ? "%s was born in %s" : "%s è nato a %s", H, T);
    else snprintf(out, cap, "%s %s %s", H, rel, T);
}
static void conclude(int en, const char *rule, const char *h, const char *t, char *out, size_t cap) {
    char H[96], T[96]; cap_words(h, H, sizeof H); cap_words(t, T, sizeof T);
    if (!strcmp(rule, "nato_in")) snprintf(out, cap, en ? "%s was born in a place located in %s" : "%s è nato in un luogo che si trova in %s", H, T);
    else if (!strcmp(rule, "connesso")) snprintf(out, cap, en ? "%s is connected to %s" : "%s è collegato a %s", H, T);
    else snprintf(out, cap, en ? "%s is located in %s" : "%s si trova in %s", H, T);
}

// ===================== the proof-carrying generator =====================
typedef struct {
    int ok;
    char reply[512];
    int nh; int hh[8], ht[8]; char hr[8][PNAME];   // derivation hops (head idx, tail idx, relation)
    int claim_h, claim_t; char rule[16];
    int conf;
} pcg_t;

static int confOf(float minc) { return (int)((100.0f * minc) / (minc + GATE) + 0.5f); }

// DERIVE: discover a verified chain head->target, ground every hop, verbalize, build the proof envelope.
static int pcg_derive(const kg_t *g, int en, const char *head, const char *target, pcg_t *out) {
    memset(out, 0, sizeof *out);
    int h = -1, t = -1;
    for (int i = 0; i < g->n; i++) { if (!strcmp(g->name[i], head)) h = i; if (!strcmp(g->name[i], target)) t = i; }
    if (h < 0 || t < 0) return 0;
    char relseq[PMAXENT][PNAME]; int nh; float minc;
    if (!reach_path(g, h, t, relseq, &nh, &minc, 5)) return 0;     // no chain -> refuse
    if (nh < 1) return 0;
    // walk + GROUND: re-materialize each hop and require it to be a REAL stored edge (HDC proposes, symbols dispose)
    int cur = h;
    for (int i = 0; i < nh; i++) {
        uint32_t probe[HDC_W]; hv_rotate(probe, g->cb + (size_t)cur * HDC_W, rel_shift(relseq[i]));
        cleanup_t c = cleanup(probe, g->cb, g->n);
        if (c.idx < 0 || c.coh < GATE) return 0;
        if (!kg_is_edge(g, cur, relseq[i], c.idx)) return 0;       // HDC invented an edge the symbols don't back -> refuse
        out->hh[i] = cur; snprintf(out->hr[i], PNAME, "%s", relseq[i]); out->ht[i] = c.idx;
        cur = c.idx;
    }
    if (cur != t) return 0;
    out->nh = nh; out->claim_h = h; out->claim_t = t;
    closure_rel(relseq, nh, out->rule, sizeof out->rule);
    out->conf = confOf(minc);
    // verbalize: conclusion + "perché/because" chain of verified clauses
    char concl[160]; conclude(en, out->rule, g->name[h], g->name[t], concl, sizeof concl);
    char buf[512]; int p = snprintf(buf, sizeof buf, "%s", concl);
    if (nh > 1 || !strcmp(out->rule, "connesso")) {
        p += snprintf(buf + p, sizeof buf - p, "%s", en ? ", because " : ", perché ");
        for (int i = 0; i < nh; i++) {
            char cl[160]; clause(en, g->name[out->hh[i]], out->hr[i], g->name[out->ht[i]], cl, sizeof cl);
            p += snprintf(buf + p, sizeof buf - p, "%s%s", i ? (en ? " and " : " e ") : "", cl);
        }
    }
    p += snprintf(buf + p, sizeof buf - p, ".");
    snprintf(out->reply, sizeof out->reply, "%s", buf);
    out->ok = 1;
    return 1;
}

// the STANDALONE proof checker — re-verifies a claim without trusting the generator (the "carrying" has teeth)
static int pcg_verify(const kg_t *g, const pcg_t *pr) {
    if (!pr->ok || pr->nh < 1) return 0;
    for (int i = 0; i < pr->nh; i++) if (!kg_is_edge(g, pr->hh[i], pr->hr[i], pr->ht[i])) return 0;      // every hop a real edge
    for (int i = 1; i < pr->nh; i++) if (pr->ht[i - 1] != pr->hh[i]) return 0;                            // chain connected
    if (pr->hh[0] != pr->claim_h || pr->ht[pr->nh - 1] != pr->claim_t) return 0;                          // endpoints match
    char rule[16]; char rs[PMAXENT][PNAME]; for (int i = 0; i < pr->nh; i++) snprintf(rs[i], PNAME, "%s", pr->hr[i]);
    closure_rel(rs, pr->nh, rule, sizeof rule);
    if (strcmp(rule, pr->rule)) return 0;                                                                 // closure honest
    return 1;
}

// ===================== the battery (mirrors pcg-eval.mjs) =====================
static int contains_ci(const char *hay, const char *needle) {
    char H[512], N[128]; size_t i;
    for (i = 0; hay[i] && i < sizeof H - 1; i++) H[i] = (char)tolower((unsigned char)hay[i]); H[i] = 0;
    for (i = 0; needle[i] && i < sizeof N - 1; i++) N[i] = (char)tolower((unsigned char)needle[i]); N[i] = 0;
    return strstr(H, N) != NULL;
}

int main(void) {
    g_simcnt = malloc((size_t)HDC_D); g_bcnt = malloc((size_t)HDC_D);
    if (!g_simcnt || !g_bcnt) { printf("OOM\n"); return 2; }
    kg_t *g = calloc(1, sizeof(kg_t));
    kg_add(g, "parigi", "capitale_di", "francia");
    kg_add(g, "lione", "si_trova_in", "francia");
    kg_add(g, "roma", "capitale_di", "italia");
    kg_add(g, "tokyo", "capitale_di", "giappone");
    kg_add(g, "francia", "si_trova_in", "europa");
    kg_add(g, "italia", "si_trova_in", "europa");
    kg_add(g, "giappone", "si_trova_in", "asia");
    kg_add(g, "europa", "si_trova_in", "emisfero_nord");
    kg_add(g, "dante", "nato_in", "firenze");
    kg_add(g, "firenze", "si_trova_in", "italia");
    if (!kg_build(g)) { printf("build fail\n"); return 2; }

    printf("\n=== NSPCG (C, on-device) — generazione proof-carrying, primitivi bit-identici al firmware ===\n\n");

    // 1) GENERATE novel grounded sentences
    const char *CASES[][3] = { {"tokyo","asia","asia"}, {"parigi","europa","europa"}, {"roma","emisfero_nord","emisfero nord"}, {"dante","europa","europa"} };
    int genOk = 0; pcg_t proofs[4]; int nproof = 0;
    for (int i = 0; i < 4; i++) {
        pcg_t r; int got = pcg_derive(g, 0, CASES[i][0], CASES[i][1], &r);
        int novel = got && !kg_is_edge(g, r.claim_h, r.rule, r.claim_t);   // conclusion never stored
        int right = got && contains_ci(r.reply, CASES[i][2]);
        int multi = got && r.nh >= 2;
        int ok = got && novel && right && multi;
        if (ok) { genOk++; proofs[nproof++] = r; }
        printf("  %-22s %s\n", CASES[i][0], got ? r.reply : "—");
        printf("  %-22s [hops %d · conf %d · %s] %s\n", "", got ? r.nh : 0, got ? r.conf : 0, novel ? "NUOVA" : "nota", ok ? "OK" : "FAIL");
    }

    // 2) PROOF re-verifies
    int proofAll = (nproof == 4);
    for (int i = 0; i < nproof; i++) if (!pcg_verify(g, &proofs[i])) proofAll = 0;
    printf("\n  prove ri-verificabili: %s\n", proofAll ? "tutte valide" : "INVALIDE");

    // 3) TAMPER caught
    pcg_t forged = proofs[0]; forged.ht[forged.nh - 1] = kg_intern(g, "europa");  // swap tail to a false entity
    int tamper = (pcg_verify(g, &forged) == 0);
    printf("  falsificazione smascherata: %s\n", tamper ? "si" : "NO");

    // 4) AUTONOMOUS (dante -> europa, 3 hops discovered)
    pcg_t d; int autoOk = pcg_derive(g, 0, "dante", "europa", &d) && d.nh >= 3;
    printf("  ragionamento autonomo: %s (%d salti)\n", autoOk ? "si" : "no", autoOk ? d.nh : 0);

    // 5) ZERO-HALLUCINATION refusals
    pcg_t z; int ref = 0;
    if (!pcg_derive(g, 0, "tokyo", "europa", &z)) ref++;     // disconnected
    if (!pcg_derive(g, 0, "dante", "asia", &z)) ref++;       // unreachable
    if (!pcg_derive(g, 0, "lione", "asia", &z)) ref++;       // wrong continent
    printf("  onesta (rifiuti corretti): %d/3\n", ref);

    // 6) BILINGUAL
    pcg_t en; int enOk = pcg_derive(g, 1, "tokyo", "asia", &en) && contains_ci(en.reply, "located in asia") && pcg_verify(g, &en.ok ? &en : &en);
    printf("  bilingue: %s  \"%s\"\n", enOk ? "si" : "no", enOk ? en.reply : "—");

    int fail = genOk < 4 || !proofAll || !tamper || !autoOk || ref < 3 || !enOk;
    printf("\n[pcg-ctest] generate %d/4 · prove %s · tamper %s · autonomo %s · onesta %d/3 · bilingue %s -> %s\n",
           genOk, proofAll ? "ok" : "X", tamper ? "ok" : "X", autoOk ? "ok" : "X", ref, enOk ? "ok" : "X", fail ? "FAIL" : "PASS");
    free(g); free(g_simcnt); free(g_bcnt);
    return fail ? 1 : 0;
}
