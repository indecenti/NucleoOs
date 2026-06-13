// ANIMA L1 — on-device semantic tier (docs/anima.md). This is the real inference:
// the query is embedded by the distilled micro-encoder (hashed char+word n-grams ->
// summed int8 rows read from the SD encoder table -> L2 normalize), then matched by
// cosine against a small RAM-resident index of labelled Italian examples (RAG). It
// understands paraphrases/synonyms, not keywords.
//
// The hashing (FNV-1a), text normalization and feature scheme MUST match
// tools/anima/distill.py / build_index.py byte-for-byte, or the embeddings diverge.
#include "nucleo_anima.h"
#include "anima_l1.h"
#include "nucleo_board.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <math.h>
#include "esp_log.h"
#include "esp_partition.h"      // optional: encoder memory-mapped from a flash partition (fast path)
#include "esp_heap_caps.h"

static const char *TAG = "anima.l1";
extern uint32_t g_anima_stage;   // DIAG breadcrumb (defined in nucleo_anima.c)

#define ENC_PATH   NUCLEO_SD_MOUNT "/data/anima/anima-it-encoder.bin"
#define IDX_PATH   NUCLEO_SD_MOUNT "/data/anima/anima-it-index.bin"
#define L1_MAXWORDS 16
#define L1_WORDLEN  24
#define L1_TBUF     192
#define L1_MAXDIM   256
#define L1_COS_MIN  0.85f      // answer gate. Eval shows in-scope answers land @0.85+; the old 0.66
                               // let WRONG cards through (measured: "perché il cielo è blu"->esercizio
                               // 0.68, "effimero"->UE 0.81). Raised to 0.85 so only confident matches
                               // answer offline; anything weaker escalates to the teacher (Grok) in
                               // hybrid — a correct online answer beats a confident-but-wrong L1 card.
// Dialogic clarify band: when the best card lands in [LO, MIN) AND a runner-up is within MARGIN,
// ANIMA asks "did you mean X or Y?" instead of refusing. Conservative on purpose (real use, not
// just the OOD eval): only KNOWLEDGE cards qualify (see the answer-action check in the band), BOTH
// candidates must clear LO, and LO is raised to 0.56 so weak/irrelevant matches (meta or off-topic
// questions) fall through to an honest "non lo so" rather than a confusing clarify.
// Clarify band floor. Kept narrow at 0.82 by MEASUREMENT: lowering it to 0.72 to catch more
// near-ties was tried and reverted — in [0.72,0.82) the two closest cards are often SPURIOUS
// hashing coincidences, not genuine alternatives ("cos'è la ram" near-tied the "media" and "AI"
// cards at 0.77 and offered them as nonsense options). The encoder simply can't tell apart
// in-scope from off-topic down there, so a wider band surfaces garbage. Only genuine [0.82,0.85)
// near-ties qualify; everything weaker stays an honest miss. See tools/anima-host/ood-check.mjs.
#define L1_COS_LO       0.82f
#define L1_BAND_MARGIN  0.08f

// EVIDENTIAL RESCUE (CRAG-style cross-evidence gate). A distilled encoder places an in-scope
// PARAPHRASE below the 0.85 answer gate (measured: "come ricavo la corrente…"→0.76, "differenza ram
// vs flash"→0.78) — but it stands out from the pack (margin top1−top2 ≈ 0.16). A WRONG/out-of-scope
// match does the opposite: it scores alike to its neighbours (measured: "perché il cielo è blu"→0.68
// margin 0.015, "senso della vita"→0.63 margin 0.006). So the MARGIN is the independent evidence the
// bare absolute lacks: accept a mid-confidence match ONLY when it is distinctly the best. This recovers
// real paraphrases the hard gate refused, while the tiny-margin OOD cases (and anything <0.72) still
// fall through to an honest miss / the clarify band. Tuned conservatively on the trace above.
#define L1_RESCUE_ABS    0.72f  // floor: below this, never rescue (the match is just weak)
#define L1_RESCUE_MARGIN 0.12f  // top1 must beat top2 by this much (distinctly-best evidence)

// HOST-ONLY gate/recall instrumentation: lets tools/anima sweep the gate, rescue and probe count
// from env vars WITHOUT recompiling, to separate gate-victims from AKB2-recall-victims with measured
// evidence. Compiled out of the device build entirely (#ifdef ANIMA_HOST -> device is byte-identical).
#ifdef ANIMA_HOST
static float anima_env_f(const char *n, float d) { const char *e = getenv(n); return (e && *e) ? (float)atof(e) : d; }
static int   anima_env_i(const char *n, int d)   { const char *e = getenv(n); return (e && *e) ? atoi(e) : d; }
#define L1_TOPC_CAP 128   // host can keep/probe far more clusters (full-recall ablation); device stays L1_TOPC
#else
#define L1_TOPC_CAP L1_TOPC
#endif

// ---- encoder header (from the open .bin on SD, or a memory-mapped flash partition) ----
static FILE     *s_enc;
static long      s_enc_off;        // byte offset of the int8 table (within the file OR the mapping)
static uint32_t  s_H, s_D;
static uint32_t  s_charn[6]; static int s_ncharn; static uint32_t s_wordn;
// Fast path: if a flash data partition named "anima_enc" holds the ANE2 encoder, we memory-map it and
// read rows by direct pointer — no FATFS, no SD seek, no 4 KB read amplification (the ~10x L1 win).
// Falls back to the SD file when the partition is absent, so the assistant works either way.
static const int8_t            *s_enc_map;     // base of the int8 table in the mapping (NULL = SD path)
static esp_partition_mmap_handle_t s_map_handle;

// ---- AKB2 clustered index: centroids + directory in RAM, vectors + answers on SD ----
#define L1_TOPC 6          // MAX clusters to probe; the actual count is chosen adaptively. Raised to 6 as the
                           // corpus grew (~30k vectors / K=93): an answer card can land several clusters away
                           // from the query's nearest centroid. Extra clusters = a few more cheap SD reads.
static FILE     *s_idx;            // kept open; vectors/answers streamed from it
// The index file is a VARIABLE (not the IDX_PATH constant) so the AKB5 router can point L1 at a category
// SHARD and reuse the entire proven load+search per shard. Defaults to the flat index; restored after a
// sharded query. s_shard_path holds the transient shard path the router builds.
static const char *s_idx_path = IDX_PATH;
static char        s_shard_path[192];
static bool        s_akb5_on  = false;   // a valid AKB5 manifest is present (probed once at init)
static bool        s_in_akb5  = false;   // reentrancy guard: true while the router searches a shard
static long      s_vec_base;       // file offset of the vectors section
static uint32_t  s_K, s_N;
static int8_t   *s_centroids;      // K*D int8 (RAM)
static float    *s_cnorm;          // K precomputed centroid L2 norms (query-invariant -> compute once)
static uint32_t *s_cdir;           // K*2 (vecOff, count) pairs (RAM)
static bool      s_ready;

// ---- AKB4 asymmetric "holographic" prefilter (optional, backward-compatible) ----------------
// An AKB4 index appends a sign-signature trailer (one D-bit sign mask per vector) + an "ASIG"
// footer at EOF; the magic & body stay AKB3, so this firmware reads a plain AKB3 index exactly
// as before. When the trailer is present we replace the brute-force "exact-cosine over every
// probed vector" rerank with a two-stage ANN: rank candidates by an ASYMMETRIC score — the
// full-precision int8 query against the 1-bit DB sign signature (score = sum of q[k] over the
// DB's set sign bits, rank-equivalent to int8(q).sign(db)) — then read & exact-cosine-rerank only
// the few survivors. The asymmetric score reads the same 8x-smaller signatures as the old
// symmetric Hamming filter but retains the exact cosine-winner far better (measured on the real
// 36k-vector index: OOD top-M retention 77%->92% at M=16), so M shrinks 64->16: ~4x fewer
// scattered int8 SD reads in Pass 2 at HIGHER retention, with the gate still seeing EXACT cosines
// (zero recalibration). The symmetric path is retained as a host ablation (ANIMA_L1_NOASYM).
// Derivation/knee: tools/anima/holo_probe.py + holo_cascade.py.
static long s_sig_base;            // file offset of the signature section (0 = plain AKB3 -> exact path)
static int  s_sigb;                // signature bytes per vector (D/8)
#define L1_PREFILTER_M    16          // exact-rerank survivors after the ASYMMETRIC prefilter (recall/cost
                                      // knee). Was 64 under the symmetric Hamming filter; the asymmetric
                                      // score retains the cosine-winner so much better that M shrinks 4x
                                      // (4x fewer scattered int8 SD reads) at HIGHER retention. See below.
#define L1_PREFILTER_MMAX 64          // compile-time cap for the host knee-sweep override (ANIMA_L1_PFM)
#define L1_SCORE_MIN (-2000000000)    // empty/pre-fill sentinel: below any real asym (>=-D*127) or -Hamming

#ifdef ANIMA_HOST
// HOST-ONLY cost meter (compiled out of the device build): with ANIMA_L1_STATS=1 each query prints
// what the exact path WOULD have cost (every probed vector) vs what the prefilter actually did.
static long s_st_cand, s_st_surv;
#endif

// ---- FNV-1a 32-bit with a leading tag byte (matches distill.py feats()) ----
static uint32_t fnv1a_tag(uint8_t tag, const char *s, int len)
{
    uint32_t h = 0x811c9dc5u;
    h = (h ^ tag) * 0x01000193u;
    for (int i = 0; i < len; i++) h = (h ^ (uint8_t)s[i]) * 0x01000193u;
    return h;
}

// Normalize -> lowercase ASCII words, Italian accents folded (matches normalize_text()).
static int l1_words(const char *in, char w[L1_MAXWORDS][L1_WORDLEN])
{
    int nw = 0, len = 0; char cur[L1_WORDLEN];
    for (const unsigned char *p = (const unsigned char *)in; ; p++) {
        unsigned char c = *p; char out = 0;
        if (c == 0xC3 && p[1]) {
            unsigned char d = *++p;
            switch (d) {
                case 0xA0: case 0xA1: case 0xA2: out = 'a'; break;
                case 0xA8: case 0xA9: case 0xAA: out = 'e'; break;
                case 0xAC: case 0xAD: case 0xAE: out = 'i'; break;
                case 0xB2: case 0xB3: case 0xB4: out = 'o'; break;
                case 0xB9: case 0xBA: case 0xBB: out = 'u'; break;
                default: out = 0; break;
            }
        } else if (isalnum(c)) out = (char)tolower(c);

        if (out) { if (len < L1_WORDLEN - 1) cur[len++] = out; }
        else {
            if (len) { if (nw < L1_MAXWORDS) { cur[len] = 0; memcpy(w[nw++], cur, len + 1); } len = 0; }
            if (c == 0) break;
        }
    }
    return nw;
}

// Encoder rows live on the SD (a ~3 MB table). A small direct-mapped RAM cache of hot rows
// (function words, frequent n-grams) cuts repeated SD reads — the dominant L1 cost.
// The row table is malloc'd lazily (ec_cache_acquire) and FREED in nucleo_anima_l1_unload(), so
// it returns to the heap whenever a foreground app opens — exactly like the cluster index.
// It re-acquires on the next encode; if the heap can't fit it, acc_row falls back to reading each
// row straight from SD (slower, still correct), so the assistant never breaks under pressure.
//
// ADAPTIVE (degrade-to-fit): ENC_CACHE is a CEILING, not a fixed size. ec_cache_acquire() takes the
// largest cache the (fragmented) heap can give — ceiling 128, halving down to a 16-slot floor — and
// records the real count in s_ec_slots. This is STRICTLY >= a fixed size: where a fixed 13 KB block
// would fail to allocate and drop L1 to no-cache (every row from SD), the adaptive path still lands
// a smaller cache. Retrieval results are byte-identical at any slot count (the cache is transparent;
// slot count only changes hit/miss). The freed app .bss (recorder/photos lazy buffers) is what lets
// the bigger block land contiguously when ANIMA runs at the launcher.
#define ENC_CACHE 128                        // ceiling; actual = s_ec_slots (<=128, power-of-two down to 16)
static uint32_t s_ec_id[ENC_CACHE];          // slot tags sized to the ceiling (512 B .bss)
static int8_t  *s_ec_row;                    // s_ec_slots * s_D int8 rows, malloc'd lazily (heap, not .bss)
static int      s_ec_slots;                  // rows actually allocated this acquire (0 = no cache, SD fallback)

// Best-effort (re)allocation of the hot-row cache. A fresh buffer starts all-empty (no slot valid).
static void ec_cache_acquire(void)
{
    if (s_ec_row || s_D == 0) return;
    for (int want = ENC_CACHE; want >= 16; want >>= 1) {   // largest power-of-two cache that fits
        s_ec_row = malloc((size_t)want * s_D);
        if (s_ec_row) { s_ec_slots = want; break; }
    }
    if (s_ec_row) for (int i = 0; i < s_ec_slots; i++) s_ec_id[i] = 0xFFFFFFFFu;
    else s_ec_slots = 0;
    HLOG(TAG, "ec_cache %uB (%d slots): %s free=%u largest=%u",
         (unsigned)(s_ec_slots * s_D), s_ec_slots, s_ec_row ? "ok" : "OOM",
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}

// Read encoder row `id` from cache (or SD on a miss) and add it to the accumulator. When the cache
// isn't resident (freed for an app, or OOM) it reads straight into a stack row — correct, just slower.
static void acc_row(uint32_t id, int32_t *acc)
{
    if (s_H == 0) return;                       // guard: a corrupt encoder (height 0) -> id %= 0 = divide-by-zero PANIC
    id %= s_H;
    int8_t  stackrow[L1_MAXDIM];
    int8_t *row;
    if (s_enc_map) {                            // flash-mapped: direct pointer, zero I/O (no cache needed)
        const int8_t *r = s_enc_map + (size_t)id * s_D;
        for (uint32_t k = 0; k < s_D; k++) acc[k] += r[k];
        return;
    }
    if (s_ec_row) {
        uint32_t slot = id % (uint32_t)s_ec_slots;   // s_ec_slots>0 whenever s_ec_row!=NULL
        row = s_ec_row + (size_t)slot * s_D;
        if (s_ec_id[slot] != id) {              // miss -> read once from SD, cache it
            if (fseek(s_enc, s_enc_off + (long)id * s_D, SEEK_SET) != 0) return;
            if (fread(row, 1, s_D, s_enc) != s_D) return;
            s_ec_id[slot] = id;
        }
    } else {                                    // cache freed/unavailable -> direct SD read, no caching
        row = stackrow;
        if (fseek(s_enc, s_enc_off + (long)id * s_D, SEEK_SET) != 0) return;
        if (fread(row, 1, s_D, s_enc) != s_D) return;
    }
    for (uint32_t k = 0; k < s_D; k++) acc[k] += row[k];
}

// Embed `text` with the device encoder -> int8 unit vector in qv[s_D]. Returns false on error.
static bool l1_encode(const char *text, int8_t *qv)
{
    g_anima_stage = 0xCA;                          // DIAG: in l1_encode (query embedding)
    char w[L1_MAXWORDS][L1_WORDLEN];
    int nw = l1_words(text, w);
    if (nw == 0) return false;

    ec_cache_acquire();                 // re-grab the hot-row cache if a prior unload freed it
    int32_t acc[L1_MAXDIM]; memset(acc, 0, sizeof(int32_t) * s_D);

    // char n-grams over "^w1^w2...^$"  (tag 0x01)
    char t[L1_TBUF]; int tl = 0;
    t[tl++] = '^';
    for (int i = 0; i < nw && tl < L1_TBUF - 2; i++) {
        for (int j = 0; w[i][j] && tl < L1_TBUF - 2; j++) t[tl++] = w[i][j];
        t[tl++] = '^';
    }
    t[tl - 1] = '$';   // replace trailing '^' with '$' (matches "^"+"^".join+"$")
    for (int n = 0; n < s_ncharn; n++) {
        int g = (int)s_charn[n];
        for (int i = 0; i + g <= tl; i++) acc_row(fnv1a_tag(0x01, t + i, g), acc);
    }
    // word unigrams + bigrams (tag 0x02)
    for (int i = 0; i < nw; i++) acc_row(fnv1a_tag(0x02, w[i], strlen(w[i])), acc);
    if (s_wordn >= 2) {
        for (int i = 0; i + 1 < nw; i++) {
            char bg[L1_WORDLEN * 2 + 1]; int bl = 0;
            for (int j = 0; w[i][j]; j++) bg[bl++] = w[i][j];
            bg[bl++] = ' ';
            for (int j = 0; w[i+1][j]; j++) bg[bl++] = w[i+1][j];
            acc_row(fnv1a_tag(0x02, bg, bl), acc);
        }
    }

    // L2 normalize -> int8 (unit * 127), matching build_index.py
    double nrm = 0; for (uint32_t k = 0; k < s_D; k++) nrm += (double)acc[k] * acc[k];
    nrm = sqrt(nrm); if (nrm < 1e-9) return false;
    for (uint32_t k = 0; k < s_D; k++) {
        int v = (int)lround(acc[k] / nrm * 127.0);
        qv[k] = (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v);
    }
    return true;
}

static uint16_t rd_u16(FILE *f) { uint8_t b[2]; if (fread(b,1,2,f)!=2) return 0; return b[0] | (b[1]<<8); }
static uint32_t rd_u32(FILE *f) { uint8_t b[4]; if (fread(b,1,4,f)!=4) return 0; return b[0]|(b[1]<<8)|(b[2]<<16)|((uint32_t)b[3]<<24); }
// Read a u16-length-prefixed string into a bounded buffer (skips any overflow). 0 ok, -1 err.
static int rd_cstr(FILE *f, char *out, int cap)
{
    uint16_t len = rd_u16(f);
    int n = len < cap - 1 ? len : cap - 1;
    if (n > 0 && fread(out, 1, n, f) != (size_t)n) { out[0] = 0; return -1; }
    if (len > n) fseek(f, len - n, SEEK_CUR);
    out[n] = 0; return 0;
}

static bool load_index(void)
{
    s_idx = fopen(s_idx_path, "rb");
    if (!s_idx) return false;
    char magic[4]; if (fread(magic,1,4,s_idx)!=4 || memcmp(magic,"AKB3",4)) { fclose(s_idx); s_idx=NULL; return false; }
    uint32_t d = rd_u32(s_idx); s_K = rd_u32(s_idx); s_N = rd_u32(s_idx);
    if (d != s_D || s_K == 0 || s_K > 1024 || s_N == 0) { fclose(s_idx); s_idx=NULL; return false; }
    size_t ctrd_sz = (size_t)s_K * s_D;
    size_t cdir_sz = (size_t)s_K * 2 * sizeof(uint32_t);
    HLOG(TAG, "load K=%u D=%u N=%u need=%u free=%u largest=%u",
         (unsigned)s_K, (unsigned)s_D, (unsigned)s_N, (unsigned)(ctrd_sz + cdir_sz),
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    s_centroids = malloc(ctrd_sz);
    s_cdir = malloc(cdir_sz);
    if (!s_centroids || !s_cdir) {
        ESP_LOGW(TAG, "load OOM centroids=%s cdir=%s free=%u largest=%u",
                 s_centroids ? "ok" : "FAIL", s_cdir ? "ok" : "FAIL",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        free(s_centroids); s_centroids = NULL;
        free(s_cdir);      s_cdir = NULL;
        fclose(s_idx); s_idx=NULL; return false;
    }
    if (fread(s_centroids, 1, ctrd_sz, s_idx) != ctrd_sz) {
        free(s_centroids); s_centroids = NULL;
        free(s_cdir);      s_cdir = NULL;
        fclose(s_idx); s_idx=NULL; return false;
    }
    for (uint32_t c = 0; c < s_K; c++) { s_cdir[c*2] = rd_u32(s_idx); s_cdir[c*2+1] = rd_u32(s_idx); }
    s_vec_base = ftell(s_idx);   // vectors section follows; streamed per cluster
    // Precompute centroid L2 norms ONCE — they are query-invariant, so recomputing sum(cv^2)+sqrt for
    // all K centroids on every query (the old hot loop) was pure waste. K floats, trivial RAM.
    s_cnorm = malloc((size_t)s_K * sizeof(float));
    if (s_cnorm) for (uint32_t c = 0; c < s_K; c++) {
        const int8_t *cv = s_centroids + (size_t)c * s_D;
        int32_t s = 0; for (uint32_t k = 0; k < s_D; k++) s += (int32_t)cv[k] * cv[k];
        s_cnorm[c] = sqrtf((float)s);
    }

    // AKB4 trailer probe: a 16-byte footer "ASIG | u32 sig_off | u32 sig_bits | u32 ver" at EOF
    // signals an appended sign-signature section. Optional and additive — absence leaves s_sig_base
    // at 0 and the query falls back to the exact path below, byte-identical to the AKB3 behaviour.
    s_sig_base = 0; s_sigb = 0;
    if ((s_D % 64) == 0 && fseek(s_idx, -16, SEEK_END) == 0) {
        uint8_t ft[16];
        if (fread(ft, 1, 16, s_idx) == 16 && memcmp(ft, "ASIG", 4) == 0) {
            uint32_t soff  = ft[4] | (ft[5]<<8) | (ft[6]<<16) | ((uint32_t)ft[7]<<24);
            uint32_t sbits = ft[8] | (ft[9]<<8) | (ft[10]<<16) | ((uint32_t)ft[11]<<24);
            if (sbits == s_D && soff > 0) { s_sig_base = (long)soff; s_sigb = (int)(s_D / 8); }
        }
    }
    return true;
}

// Sign signature of an int8 vector: bit k = (v[k] >= 0), packed little-endian into D/64 u64 words.
// MUST match the augmenter (tools/anima/augment_akb4.py: numpy packbits 'little', tie 0 -> 1) so
// the device query signature and the stored index signatures are bit-compatible.
static void l1_sign_sig(const int8_t *v, uint32_t dim, uint64_t *sig)
{
    int words = (int)(dim >> 6);
    for (int w = 0; w < words; w++) sig[w] = 0;
    for (uint32_t k = 0; k < dim; k++) if (v[k] >= 0) sig[k >> 6] |= (uint64_t)1 << (k & 63);
}

// Try to memory-map the ANE2 encoder from a flash data partition named "anima_enc". On success the
// header fields are parsed from the mapping and s_enc_map points at the int8 table. Returns false
// (cleanly) when the partition is absent/invalid, so init falls back to the SD encoder file.
static bool map_encoder_from_flash(void)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "anima_enc");
    if (!p) return false;
    const void *base = NULL;
    if (esp_partition_mmap(p, 0, p->size, ESP_PARTITION_MMAP_DATA, &base, &s_map_handle) != ESP_OK) return false;
    const uint8_t *m = (const uint8_t *)base;
    #define RDU32(pp) ((uint32_t)(pp)[0] | ((uint32_t)(pp)[1]<<8) | ((uint32_t)(pp)[2]<<16) | ((uint32_t)(pp)[3]<<24))
    if (memcmp(m, "ANE2", 4) != 0) goto fail;
    { const uint8_t *q = m + 4;
      s_H = RDU32(q); q += 4; s_D = RDU32(q); q += 4; s_ncharn = (int)RDU32(q); q += 4; s_wordn = RDU32(q); q += 4;
      if (s_H == 0 || s_D == 0 || s_D > L1_MAXDIM || s_ncharn <= 0 || s_ncharn > 6) goto fail;
      for (int i = 0; i < s_ncharn; i++) { s_charn[i] = RDU32(q); q += 4; }
      q += 4;                                  // scale (f32) — unused for ranking
      s_enc_off = (long)(q - m);               // table offset within the mapping
      if (s_enc_off + (long)s_H * s_D > (long)p->size) goto fail;   // table must fit the partition
      s_enc_map = (const int8_t *)(m + s_enc_off);
      ESP_LOGI(TAG, "encoder MMAP'd from flash '%s' %ux%u — fast L1 (no SD reads)", p->label, (unsigned)s_H, (unsigned)s_D);
      return true; }
fail:
    esp_partition_munmap(s_map_handle); s_map_handle = 0; s_enc_map = NULL;
    return false;
    #undef RDU32
}

bool nucleo_anima_l1_init(void)
{
    s_ready = false;
    HLOG(TAG, "init free=%u min=%u largest=%u",
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
         (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    // Fast path first: encoder mapped from internal flash (instant random access via the flash cache).
    if (!map_encoder_from_flash()) {
        // Fallback: the encoder is a ~3 MB file on the SD card (slower random reads, but always works).
        s_enc = fopen(ENC_PATH, "rb");
        if (!s_enc) { ESP_LOGW(TAG, "no encoder (flash partition or %s) — L1 disabled", ENC_PATH); return false; }
        char magic[4];
        if (fread(magic,1,4,s_enc)!=4 || memcmp(magic,"ANE2",4)) { ESP_LOGW(TAG,"bad encoder magic"); fclose(s_enc); s_enc=NULL; return false; }
        s_H = rd_u32(s_enc); s_D = rd_u32(s_enc);
        s_ncharn = (int)rd_u32(s_enc); s_wordn = rd_u32(s_enc);
        if (s_H == 0 || s_D == 0 || s_D > L1_MAXDIM || s_ncharn <= 0 || s_ncharn > 6) { fclose(s_enc); s_enc=NULL; return false; }
        for (int i = 0; i < s_ncharn; i++) s_charn[i] = rd_u32(s_enc);
        rd_u32(s_enc);                       // scale (f32) — unused for ranking
        s_enc_off = ftell(s_enc);
    }

    // The flat index is the default; an AKB5 sharded manifest (probed once here, encoder dim known) takes
    // over transparently if present. Either alone is enough to enable L1; a device may ship AKB5-only.
    bool flat_ok = load_index();
    s_ready = true;                            // set FIRST: akb5_available() bails while !s_ready (cleared below if nothing loads)
#ifdef ANIMA_HOST
    // Host A/B harness: the SD tree's flat index is the calibrated gate fixture, so AKB5 stays OFF by
    // default (gates run flat) and is armed only with ANIMA_AKB5=1 — letting us run the SAME gates both
    // ways and prove AKB5 == flat. The device build below auto-activates from manifest presence instead.
    s_akb5_on = anima_env_i("ANIMA_AKB5", 0) ? nucleo_anima_l1_akb5_available() : false;
#else
    // AKB5 sharded routing RE-ENABLED now that the L1 stack overflow is fixed (the crash was the deep
    // cascade overflowing the 16K task stack -> now 30K, NOT the AKB5 churn itself). The per-shard
    // unload/reload still fragments the heap, but load_index degrades gracefully on malloc-fail (no
    // crash); we measure largest_free_block via /api/heap and only add a shared arena if it collapses
    // below the online-TLS contiguity bar.
    s_akb5_on = nucleo_anima_l1_akb5_available();   // device: a valid SD manifest transparently takes over
#endif
    if (!flat_ok && !s_akb5_on) { s_ready = false; ESP_LOGW(TAG, "no/invalid index at %s (L1 disabled)", IDX_PATH); if (s_enc){fclose(s_enc); s_enc=NULL;} return false; }
    if (s_akb5_on) {
        nucleo_anima_l1_unload();              // drop the flat centroids; queries load the routed shard instead
        ESP_LOGI(TAG, "L1 ready AKB5 enc=%ux%u free=%u largest=%u",
                 (unsigned)s_H, (unsigned)s_D,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    } else {
        ESP_LOGI(TAG, "L1 ready AKB2 N=%u K=%u ram=%uB free=%u largest=%u",
                 (unsigned)s_N, (unsigned)s_K, (unsigned)(s_K * s_D + s_K * 8),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    }
    return true;
}

// Lazy (re)load of the ~18 KB cluster index. The centroids are freed whenever a foreground app
// opens (nucleo_anima_l1_unload) so the assistant holds NO RAM while you use the rest of the OS;
// the next query reloads them from SD in one ~18 KB read. The encoder file stays open (tiny).
static bool ensure_index(void)
{
    if (s_centroids) return true;                  // already resident
    if (!s_ready) return false;                    // no encoder -> L1 disabled
    g_anima_stage = 0xC1;                          // DIAG: loading the L1 index from SD (first use)
    HLOG(TAG, "ensure_index reload free=%u largest=%u",
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    return load_index();                           // reopen s_idx + re-read centroids/dir
}

// Free the in-RAM cluster index (centroids + directory). Safe to call repeatedly; the next query
// reloads it. Lets the launcher hand the ~18 KB to whatever app/decoder is running.
void nucleo_anima_l1_unload(void)
{
#if NUCLEO_HEAPLOG
    size_t before = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
#endif
    free(s_centroids); s_centroids = NULL;
    free(s_cnorm);     s_cnorm = NULL;
    free(s_cdir);      s_cdir = NULL;
    free(s_ec_row);    s_ec_row = NULL; s_ec_slots = 0;   // hand the hot-row cache back (re-acquired on next encode)
    if (s_idx) { fclose(s_idx); s_idx = NULL; }
#if NUCLEO_HEAPLOG
    size_t after = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    ESP_LOGI(TAG, "unload: free %u->%u (+%u) largest=%u",
             (unsigned)before, (unsigned)after, (unsigned)(after - before),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
#endif
}

// Bytes nucleo_anima_l1_unload() would hand back to the heap right now — the reclaim the online path
// frees right before a TLS handshake. The web layer adds this to the current largest-free-block when
// deciding whether going online is feasible, so a heap that looks tight ONLY because L1 is resident no
// longer vetoes every online turn on this PSRAM-less chip. ~31 KB when loaded, 0 when already unloaded.
size_t nucleo_anima_l1_heap_bytes(void)
{
    return (s_centroids ? (size_t)s_K * s_D : 0)
         + (s_cnorm     ? (size_t)s_K * sizeof(float) : 0)
         + (s_cdir      ? (size_t)s_K * 2 * sizeof(uint32_t) : 0)
         + (s_ec_row    ? (size_t)s_ec_slots * s_D : 0);
}

// Read the AKB3 answer record at `ansoff` into `out`:
//   u8 action | cstr arg | cstr reply_it | cstr reply_en | cstr detail_it | cstr detail_en
// want_detail returns the longer drill-down text (0 if the card has none). The caller sets
// confidence/budget. Factored out so the query, the clarify band, and the pick all share it.
static int l1_read_answer(long ansoff, bool en, bool want_detail, anima_result_t *out)
{
    if (ansoff < 0 || fseek(s_idx, ansoff, SEEK_SET) != 0) return 0;
    uint8_t act = 0; if (fread(&act, 1, 1, s_idx) != 1) return 0;
    char arg[24], rit[384], ren[384], dit[384], den[384];   // reply buffers match result.reply[384] (was 256 -> clipped mid-word)
    if (rd_cstr(s_idx, arg, sizeof(arg)) || rd_cstr(s_idx, rit, sizeof(rit)) || rd_cstr(s_idx, ren, sizeof(ren)) ||
        rd_cstr(s_idx, dit, sizeof(dit)) || rd_cstr(s_idx, den, sizeof(den))) return 0;
    const char *rep;
    if (want_detail) {
        rep = (en && den[0]) ? den : (dit[0] ? dit : (en && ren[0]) ? ren : rit);
        if (!((en && den[0]) || dit[0])) return 0;     // no extra detail on this card
    } else {
        rep = (en && ren[0]) ? ren : rit;
    }
    memset(out, 0, sizeof(*out));
    out->tier = ANIMA_TIER_FACT;
    out->action = act == 1 ? ANIMA_ACT_LAUNCH : act == 2 ? ANIMA_ACT_SYSTEM : ANIMA_ACT_ANSWER;
    snprintf(out->intent, sizeof(out->intent), "l1");
    snprintf(out->arg, sizeof(out->arg), "%s", arg);
    snprintf(out->reply, sizeof(out->reply), "%s", rep);
    return 1;
}

// Read a clarify candidate: fill `out` with a short label (reply up to the first sentence) and
// return its action (ANIMA_ACT_NONE on read error). The band uses the action to keep ONLY
// knowledge (answer) cards — launch/system cards make nonsensical "did you mean Apro X?" options.
static anima_action_t l1_label(long ansoff, bool en, char *out, int cap)
{
    static anima_result_t tmp;
    out[0] = 0;
    if (!l1_read_answer(ansoff, en, false, &tmp)) return ANIMA_ACT_NONE;
    int i = 0;
    for (; tmp.reply[i] && i < cap - 1 && tmp.reply[i] != '.' && tmp.reply[i] != '\n'; i++) out[i] = tmp.reply[i];
    out[i] = 0;
    return tmp.action;
}

// Top-2 DISTINCT candidates (distinct answer offset) from the most recent query — the substrate
// for the dialogic clarify band. Updated on every nucleo_anima_l1_query() call.
static struct { float c1, c2; long a1, a2; } s_band;

// EVIDENCE-COVERAGE guard (conformal "answer only if the evidence covers the question's scope").
// A RESCUE-band match (mid cosine, distinctly-best AMONG MY CARDS) can still be a GLOBAL default
// applied to a NARROWER question: "il fiume più lungo DELLA SIBERIA" finds only "il Nilo", rescued at
// high margin yet scope-wrong. So for a SUPERLATIVE question carrying a prepositional scope qualifier
// the matched card's reply does NOT cover, withhold the rescue (honest miss) instead of asserting the
// global default. Targets exactly the measured confidently-wrong class; only runs in the rescue band,
// so high-confidence (>=gate) answers and non-superlative paraphrases are never touched.
// Returns true = scope is covered (safe to answer); false = uncovered qualifier -> abstain.
// Whole-word containment (ASCII, lowercased): true only if `w` appears in `hay` bounded by
// non-alphanumerics. Avoids the substring trap where "ande" (delle Ande) matches inside "grande".
static bool l1_word_in(const char *hay, const char *w)
{
    size_t wl = strlen(w); if (!wl) return false;
    for (const char *p = strstr(hay, w); p; p = strstr(p + 1, w)) {
        char before = (p == hay) ? ' ' : p[-1];
        char after  = p[wl];
        if (!isalnum((unsigned char)before) && !isalnum((unsigned char)after)) return true;
    }
    return false;
}

// A specific Proper Noun named in the query but absent from the answer = a generic card applied to an
// unknown/fabricated entity ("...il linguaggio Floonk" -> the generic programming-language card;
// "parlami di Aldric Venmoor" -> a generic bio). A genuine entity question is answered by a card that
// NAMES the entity. Returns true => uncovered (caller abstains). Skipped when the user title-cased the
// whole query (capitalization then carries no proper-noun signal). `query`=RAW cased text; `clow`=lower answer.
static bool l1_proper_noun_uncovered(const char *query, const char *clow)
{
    int words = 0, caps = 0;                            // pass 1: detect title-casing
    for (const char *p = query; *p; ) {
        while (*p && !isalnum((unsigned char)*p) && (unsigned char)*p < 0x80) p++;
        if (!*p) break;
        char f = *p; int len = 0;
        while (*p && (isalnum((unsigned char)*p) || (unsigned char)*p >= 0x80)) { p++; len++; }
        if (len >= 2) { words++; if (f >= 'A' && f <= 'Z') caps++; }
    }
    if (words >= 3 && caps * 2 > words) return false;   // title-cased input -> don't trust capitalization

    bool first = true;                                  // pass 2: uncovered capitalized proper noun
    for (const char *p = query; *p; ) {
        while (*p && !isalnum((unsigned char)*p) && (unsigned char)*p < 0x80) p++;
        if (!*p) break;
        char w[40]; int k = 0; char f = *p;
        while (*p && (isalnum((unsigned char)*p) || (unsigned char)*p >= 0x80)) { if (k < 39) w[k++] = *p; p++; }
        w[k] = 0;
        if (!first && k >= 4 && f >= 'A' && f <= 'Z') {
            char lw[40]; int li = 0;
            for (int i = 0; w[i] && li < 39; i++) lw[li++] = (char)tolower((unsigned char)w[i]);
            lw[li] = 0;
            if (!l1_word_in(clow, lw)) return true;     // named entity the answer never mentions
        }
        first = false;
    }
    return false;
}

static bool l1_scope_covered(const char *query, const char *reply)
{
    char q[160], c[256]; size_t i;
    for (i = 0; query[i] && i + 1 < sizeof q; i++) q[i] = (char)tolower((unsigned char)query[i]);
    q[i] = 0;
    for (i = 0; reply && reply[i] && i + 1 < sizeof c; i++) c[i] = (char)tolower((unsigned char)reply[i]);
    c[i] = 0;

    // PREMISE/RELATION: a DATE question ("quando è morto/nato X") answered by a card holding NO year is
    // a biography, not a date — a relation mismatch, often a FALSE PREMISE (the person is alive). The
    // KGE died/born tier owns real dates (and carries a year); a yearless L1 bio here -> abstain.
    bool death_q = l1_word_in(q,"morto") || l1_word_in(q,"morta") || l1_word_in(q,"died") || l1_word_in(q,"death");
    bool birth_q = l1_word_in(q,"nato") || l1_word_in(q,"nata") || l1_word_in(q,"nascita") || l1_word_in(q,"born") || l1_word_in(q,"birth");
    if (death_q) {   // the answer must ASSERT a death, not be a generic bio (Ronaldo's bio carries numbers but no death)
        bool death_a = l1_word_in(c,"morto") || l1_word_in(c,"morta") || l1_word_in(c,"died") ||
                       l1_word_in(c,"deceduto") || l1_word_in(c,"scomparso");
        if (!death_a) return false;
    }
    if (birth_q) {
        bool birth_a = l1_word_in(c,"nato") || l1_word_in(c,"nata") || l1_word_in(c,"born") || l1_word_in(c,"nascita");
        if (!birth_a) return false;
    }

    // COUNT/POSSESSION subject: "quanti X ha <SUBJECT>" must be ANSWERED ABOUT that subject. A named
    // subject the reply never mentions means a generic fact about the wrong thing slipped through
    // ("quanti continenti ha Giove" -> Earth's seven continents; "quanti anni ha Dante" -> ANIMA's own
    // self-age card). Abstain. (No "ha"+entity -> not gated: "quanti continenti ci sono" still answers.)
    bool count_q = l1_word_in(q,"quanti") || l1_word_in(q,"quante") || l1_word_in(q,"quanto") || l1_word_in(q,"quanta")
                || (l1_word_in(q,"how") && (l1_word_in(q,"many") || l1_word_in(q,"much")));   // EN "how many X does Y have"
    if (count_q) {
        char ct[28][24]; int cn = 0;
        for (const char *p = q; *p && cn < 28; ) {
            while (*p == ' ' || *p == '\'') p++;
            if (!*p) break;
            int k = 0; while (*p && *p != ' ' && *p != '\'' && k < 23) ct[cn][k++] = *p++; ct[cn][k] = 0; cn++;
        }
        // subject markers: IT possessive/of-phrase + EN "does/do/in/on Y" (the named subject follows)
        static const char *const subp[] = { "ha","di","del","dello","della","dei","delle","of","does","do","in","on", NULL };
        static const char *const art2[] = { "la","il","lo","i","gli","le","un","uno","una","l","the","a", NULL };
        for (int t = 0; t + 1 < cn; t++) {
            bool sp = false; for (int s = 0; subp[s]; s++) if (!strcmp(ct[t], subp[s])) { sp = true; break; }
            if (!sp) continue;
            int j = t + 1; for (int a = 0; art2[a]; a++) if (!strcmp(ct[j], art2[a])) { j++; break; }
            if (j >= cn || strlen(ct[j]) < 4) continue;
            if (!l1_word_in(c, ct[j])) return false;       // named subject absent from the answer -> wrong subject
        }
    }

    // NAMED-ENTITY COVERAGE: a Proper Noun the question names but the answer never mentions = a generic
    // card applied to an unknown/fabricated entity. Runs for ALL answers (not just superlatives).
    if (l1_proper_noun_uncovered(query, c)) return false;

    // ATTRIBUTE COVERAGE: a query naming a SPECIFIC narrow attribute/relation must be answered by a card
    // that actually carries it. A bio/definition card often matches the ENTITY but not the asked ATTRIBUTE
    // ("codice fiscale di Leonardo" -> Leonardo's bio; "temperature inside a black hole" -> the black-hole
    // definition; "Caesar's twin brother" -> Caesar's bio; "marathons Leonardo won" -> Leonardo's bio).
    // Those are right-entity / wrong-attribute fabrications-by-omission. The list is narrow on purpose: a
    // card legitimately ANSWERING one of these always contains the word (or, for temperature, a degree
    // marker), so this is zero-FP — "what is a password" (answer carries "password") still passes.
    {
        // personal secrets / IDs are unknowable to an offline KB -> abstain on the possessive form.
        bool mine = l1_word_in(q,"mia")||l1_word_in(q,"mio")||l1_word_in(q,"miei")||l1_word_in(q,"mie")||l1_word_in(q,"my");
        if (mine && (l1_word_in(q,"password")||l1_word_in(q,"pin")||l1_word_in(q,"email")||l1_word_in(q,"fiscale"))) return false;
        static const char *const attrw[] = { "password","fiscale","gemello","gemella","gemelli","twin",
            "cugino","cugina","cousin","maratona","maratone","marathon","marathons", NULL };
        for (int a = 0; attrw[a]; a++) if (l1_word_in(q, attrw[a]) && !l1_word_in(c, attrw[a])) return false;
        if ((l1_word_in(q,"temperatura")||l1_word_in(q,"temperature")) &&
            !(l1_word_in(c,"temperatura")||l1_word_in(c,"temperature")||strstr(c,"gradi")||strstr(c,"celsius")||strstr(c,"kelvin")||strstr(c,"fahrenheit"))) return false;
    }

    static const char *const sup[] = { "piu", "pi\xc3\xb9", "massim", "minim", "maggior", "miglior",
        "most", "longest", "highest", "tallest", "largest", "biggest", "deepest", "greatest", NULL };
    bool is_sup = false;
    for (int s = 0; sup[s]; s++) if (strstr(q, sup[s])) { is_sup = true; break; }
    if (!is_sup) return true;                         // guard only the superlative class

    // RELATION match: a superlative question must be answered by a superlative-framed card. A plain
    // attribute card ("La capitale di Canada è Ottawa") answering "la città PIÙ POPOLOSA del Canada"
    // is a relation mismatch (right entity, wrong attribute) -> abstain.
    bool reply_sup = false;
    for (int s = 0; sup[s]; s++) if (strstr(c, sup[s])) { reply_sup = true; break; }
    if (!reply_sup) return false;

    // tokenize the (lowercased) query on spaces AND apostrophes ("dell'antartide" -> "dell","antartide",
    // else the elided preposition glues to the qualifier and the scope check never sees it).
    char tok[28][24]; int nt = 0;
    for (const char *p = q; *p && nt < 28; ) {
        while (*p == ' ' || *p == '\'') p++;
        if (!*p) break;
        int k = 0; while (*p && *p != ' ' && *p != '\'' && k < 23) tok[nt][k++] = *p++; tok[nt][k] = 0; nt++;
    }
    static const char *const prep[]   = { "di","del","dello","della","dei","delle","degli","dell",
                                          "in","sul","sulla","nel","nella","of",
                                          "all","dall","sull","nell","coll","dello","negli", NULL };  // incl. elided forms
    static const char *const art[]    = { "la","il","lo","i","gli","le","un","uno","una","l","the", NULL };
    static const char *const global[] = { "mondo","world","terra","earth","universo","tutti","tutto",
                                          "all","sempre","sistema", NULL };   // generic scope = always covered
    // EXCLUSION scope: "il più grande [tranne/oltre/escluso/dopo/eccetto/besides/except/beyond] X" REMOVES X
    // from the candidate set, so an answer that IS X is precisely wrong. The global-default card ("…è Giove")
    // names the very planet that "oltre Giove" excludes -> abstain (the correct answer would be the next one).
    static const char *const excl[] = { "tranne","eccetto","escluso","esclusa","esclusi","salvo","oltre",
                                        "dopo","besides","except","excluding","beyond","after","other", NULL };
    for (int t = 0; t + 1 < nt; t++) {
        bool isx = false; for (int e = 0; excl[e]; e++) if (!strcmp(tok[t], excl[e])) { isx = true; break; }
        if (!isx) continue;
        int j = t + 1;
        if (!strcmp(tok[j], "than")) j++;             // "other than X"
        else for (int a = 0; art[a]; a++) if (!strcmp(tok[j], art[a])) { j++; break; }
        if (j >= nt || strlen(tok[j]) < 4) continue;
        if (l1_word_in(c, tok[j])) return false;      // the answer names the EXCLUDED entity -> wrong
    }
    // HEAD-NOUN: the noun being ranked must appear in the card. "lago più grande del Sahara" answered by
    // "il deserto più grande è il Sahara" ranks the WRONG subject (lake vs desert). IT: the noun precedes
    // "più"; EN: it follows the superlative adjective (skip one article). Misses -> abstain.
    for (int t = 0; t < nt; t++) {
        bool itmark = !strcmp(tok[t], "piu") || !strcmp(tok[t], "pi\xc3\xb9");
        bool enmark = !strcmp(tok[t],"largest")||!strcmp(tok[t],"biggest")||!strcmp(tok[t],"tallest")||
                      !strcmp(tok[t],"longest")||!strcmp(tok[t],"deepest")||!strcmp(tok[t],"highest")||
                      !strcmp(tok[t],"greatest")||!strcmp(tok[t],"smallest")||!strcmp(tok[t],"shortest");
        const char *head = NULL;
        if (itmark && t >= 1) head = tok[t-1];
        else if (enmark && t + 1 < nt) {
            int j = t + 1; for (int a = 0; art[a]; a++) if (!strcmp(tok[j], art[a])) { j++; break; }
            if (j < nt) head = tok[j];
        }
        if (head && strlen(head) >= 4 && !l1_word_in(c, head)) return false;  // ranked subject absent from the card
    }
    for (int t = 0; t + 1 < nt; t++) {
        bool isprep = false; for (int p = 0; prep[p]; p++) if (!strcmp(tok[t], prep[p])) { isprep = true; break; }
        if (!isprep) continue;
        int j = t + 1;                                // skip one article after the preposition
        for (int a = 0; art[a]; a++) if (!strcmp(tok[j], art[a])) { j++; break; }
        if (j >= nt) break;
        const char *qual = tok[j];
        if (strlen(qual) < 4) continue;               // short words aren't restrictive qualifiers
        bool isglobal = false; for (int g = 0; global[g]; g++) if (!strcmp(qual, global[g])) { isglobal = true; break; }
        if (isglobal) continue;
        if (!l1_word_in(c, qual)) return false;       // restrictive qualifier the card never mentions -> scope mismatch
    }
    return true;
}

// RESCUE ON-TOPIC: a mid-confidence rescue (cosine in [0.72,0.85)) must be TOPICALLY about the query.
// If the query has >=2 salient content words (>=5 chars, not stop/question words) and NONE appear in the
// matched reply, the card is off-topic — e.g. "il primo imperatore di Marte" rescued the moon-landing
// card on the lone shared word "primo". Abstain. Conservative (needs >=2 unmatched salient words) so
// genuine single-keyword paraphrases are never blocked. High-gate (>=0.85) answers skip this entirely.
static bool l1_rescue_on_topic(const char *query, const char *reply)
{
    char q[160], c[256]; size_t i;
    for (i = 0; query[i] && i + 1 < sizeof q; i++) q[i] = (char)tolower((unsigned char)query[i]);
    q[i] = 0;
    for (i = 0; reply && reply[i] && i + 1 < sizeof c; i++) c[i] = (char)tolower((unsigned char)reply[i]);
    c[i] = 0;
    static const char *const stop[] = { "quale","quali","quanto","quanti","quanta","quante","cosa","come",
        "quando","dove","perche","significa","significato","primo","prima","secondo","seconda","terzo","ultima",
        "grande","piccolo","piccola","essere","stato","stata","sono","della","dello","delle","degli","nella",
        "nello","negli","sulla","sullo","questo","quella","what","which","where","when","whose","first","second",
        "third","about","there","their","these","those", NULL };
    int salient = 0;
    for (const char *p = q; *p; ) {
        while (*p == ' ' || *p == '\'') p++;
        char w[40]; int k = 0; while (*p && *p != ' ' && *p != '\'' && k < 39) w[k++] = *p++; w[k] = 0;
        if (k < 5) continue;
        bool st = false; for (int s = 0; stop[s]; s++) if (!strcmp(w, stop[s])) { st = true; break; }
        if (st) continue;
        if (l1_word_in(c, w)) return true;            // a content word of the query is in the reply -> on-topic
        salient++;
    }
    return salient < 2;                                // >=2 salient words, none matched -> off-topic -> abstain
}

// LONE-WORD COLLISION: a query that is a SINGLE salient content word the card never mentions is a cross-
// lingual / stem encoder collision, not an answer — bare "gatto" margin-rescued the Unix "cat" command card
// ("Visualizza il contenuto di un file"). Used only in the rescue band AND only when there is no lexical
// corroboration (a typo like "varaible" keeps lex_ok and is NOT a collision). Multi-word queries keep the
// on-topic rule above; an in-card single word (the normal recall case) passes untouched.
static bool l1_lone_uncovered(const char *query, const char *reply)
{
    char q[160], c[256]; size_t i;
    for (i = 0; query[i] && i + 1 < sizeof q; i++) q[i] = (char)tolower((unsigned char)query[i]); q[i] = 0;
    for (i = 0; reply && reply[i] && i + 1 < sizeof c; i++) c[i] = (char)tolower((unsigned char)reply[i]); c[i] = 0;
    // A BARE single content word (<=3 tokens, the rest framing) the card never mentions is a cross-lingual
    // encoder collision ("gatto" -> the Unix "cat" command card), not an answer. Limited to bare queries: a
    // fuller sentence ("spiegami come dipingere a olio" -> the painting card; "why is the sun a cube") shares
    // meaning, not always the surface word, so requiring lexical coverage there would mute legit recall.
    static const char *const stop[] = { "cosa","cos","come","quale","quali","quando","dove","perche","chi",
        "che","what","which","where","when","who","how","una","uno","della","dello","the","this","that", NULL };
    int salient = 0, total = 0; char only[40] = {0};
    for (const char *p = q; *p; ) {
        while (*p && !isalnum((unsigned char)*p) && *p != '\'') p++;
        if (!*p) break;
        char w[40]; int k = 0; while (*p && isalnum((unsigned char)*p) && k < 39) w[k++] = *p++; w[k] = 0;
        while (*p == '\'') p++;
        if (k >= 1) total++;
        if (total > 3) return false;                   // a real sentence, not a bare word
        if (k < 4) continue;
        bool st = false; for (int s = 0; stop[s]; s++) if (!strcmp(w, stop[s])) { st = true; break; }
        if (st) continue;
        if (++salient > 1) return false;
        snprintf(only, sizeof only, "%s", w);
    }
    return salient == 1 && !l1_word_in(c, only);
}

// LEXICAL corroboration — the second evidence channel behind the dual-channel gate. A query content
// word that is a near-typo (bounded Damerau-Levenshtein <=1/2) of a word in the matched card means the
// query is a MISSPELLING of this very card, not out-of-distribution noise. So a mid-confidence match
// ("cos'è una varaible" -> the variabile card @0.79) earns acceptance the bare cosine gate refused,
// while genuine junk ("asdkfj") — which is lexically close to NOTHING in the card — stays refused.
extern int a_damlev(const char *a, const char *b, int max);   // exported from anima_text.c
// AKB5 PREMISE COVERAGE — the global-competition substitute for the sharded path. The flat index abstains
// on a false-premise query ("capitale di Marte", "quanti followers ha il Medioevo", "quando ha twittato
// Colombo") because a competing card globally pulls it into the clarify band. A narrow shard has no such
// competitor, so the lone topical card (Malta because marte~malta, or the Medioevo description) clears the
// gate and answers a question never asked. Guard: in AKB5 mode ONLY, every salient query word — the entity
// AND the relation/property — must be lexically present (damlev-tolerant) in the matched card. A correct
// hit covers fully ("cos'è la fotosintesi" -> the fotosintesi card); a false premise leaves the entity
// ("marte") or the property ("followers"/"capitale") uncovered -> abstain. Question/lead-in words are not
// salient (so "qual è la capitale di X" hinges on capitale+X, not on "qual"). Flat is untouched: it keeps
// its competition-based gating, so this never changes the shipped default. See nl-stress / cross-topic.
static bool l1_premise_covered(const char *query, const char *reply)
{
    if (!reply || !reply[0]) return true;                       // nothing to check against -> don't block
    char q[200], c[400]; size_t i;
    for (i = 0; query[i] && i + 1 < sizeof q; i++) q[i] = (char)tolower((unsigned char)query[i]); q[i] = 0;
    for (i = 0; reply[i] && i + 1 < sizeof c; i++) c[i] = (char)tolower((unsigned char)reply[i]); c[i] = 0;
    // PREMISE-MARKER guard (measured root cause, akb5-diff.mjs over 7418 q). A BROAD "every salient word
    // covered" guard killed 52 legit SEMANTIC recalls (describe-without-naming, synonyms) — fatal for a
    // semantic retriever. So we guard ONLY the two narrow classes the sharded path actually mis-answers,
    // and leave everything else to pure cosine recall:
    //  (1) ANACHRONISM / social-media premises on real entities — "quando ha twittato Colombo", "followers
    //      del Medioevo", "email di Napoleone": the marker verb is absent from any card, so if a query marker
    //      isn't in the matched card it's a deflection to an unrelated true fact -> abstain.
    //  (2) CAPITAL look-ups — the char-ngram encoder confuses short country names ("capitale di Marte" ~
    //      Malta) and a topical card answers a celestial body ("capitale della Luna" -> Moon).
    static const char *const social[] = { "twittato","twitta","tweet","twitter","followers","follower",
        "instagram","tiktok","hashtag","postato","snapchat","facebook","youtube","retweet","stories",
        "email","gmail","whatsapp", /* modern channels: anachronistic for historical persons */
        // FACTUAL-ATTRIBUTE markers: a country card states only the capital, so "valuta/lingua/popolazione
        // del Giappone" matched it and answered the capital (wrong attribute). If the asked attribute word
        // isn't in the card, that's a category misroute -> abstain (ANIMA has no currency/language data).
        "valuta","moneta","currency","lingua","language","popolazione","abitanti","population","inhabitants",
        "continente","continent", NULL };
    for (int s = 0; social[s]; s++) if (l1_word_in(q, social[s]) && !l1_word_in(c, social[s])) return false;
    bool q_cap = l1_word_in(q, "capitale") || l1_word_in(q, "capital");
    if (!q_cap) return true;                                  // not a capital query -> no guard
    if (!(l1_word_in(c, "capitale") || l1_word_in(c, "capital")))
        return false;                                        // card states no capital ("della Luna" -> Moon) -> abstain
    // the COUNTRY token of the query must actually be in the card (damlev-tolerant for typo/inflection);
    // framing/scope words are not the entity, so they're skipped.
    static const char *const stop[] = {
        "qual","quale","quali","quanto","della","dello","delle","dell","degli","nella","sulla","capitale",
        "capital","what","which","where","whats","city","citta","stato","state","paese","country","nazione",
        "nation","world","mondo","dimmi","sai","dire","know","tell","quella","quello", NULL };
    char rw[64][24]; int rn = 0;                               // card words (>=4 chars)
    for (const char *p = c; *p && rn < 64; ) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        int k = 0; char w[24]; while (*p && isalnum((unsigned char)*p) && k < 23) w[k++] = *p++; w[k] = 0;
        if (k >= 4) { memcpy(rw[rn], w, k + 1); rn++; }
    }
    for (const char *p = q; *p; ) {                            // every COUNTRY/entity token must be in the card
        while (*p && !isalnum((unsigned char)*p) && *p != '\'') p++;
        if (!*p) break;
        char w[24]; int k = 0; while (*p && isalnum((unsigned char)*p) && k < 23) w[k++] = *p++; w[k] = 0;
        while (*p == '\'') p++;
        if (k < 4) continue;                                   // short tokens never the entity
        bool st = false; for (int s = 0; stop[s]; s++) if (!strcmp(w, stop[s])) { st = true; break; }
        if (st) continue;
        int maxd = (k >= 7) ? 2 : 1, ok = 0;
        for (int r = 0; r < rn; r++) if (a_damlev(w, rw[r], maxd) <= maxd) { ok = 1; break; }
        if (!ok) return false;                                 // entity absent from card -> Marte vs Malta -> abstain
    }
    return true;
}

static bool l1_lexically_corroborated(const char *query, const char *reply)
{
    if (!reply || !reply[0]) return false;
    char q[160], c[256]; size_t i;
    for (i = 0; query[i] && i + 1 < sizeof q; i++) q[i] = (char)tolower((unsigned char)query[i]); q[i] = 0;
    for (i = 0; reply && reply[i] && i + 1 < sizeof c; i++) c[i] = (char)tolower((unsigned char)reply[i]); c[i] = 0;
    static const char *const stop[] = { "cosa","cos","come","quale","quali","quando","dove","perche","chi",
        "una","uno","della","dello","delle","degli","what","which","where","when","that","this","the", NULL };
    // reply words (>=4 chars), kept for comparison
    char rw[48][24]; int rn = 0;
    for (const char *p = c; *p && rn < 48; ) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        int k = 0; char w[24]; while (*p && (isalnum((unsigned char)*p)) && k < 23) w[k++] = *p++; w[k] = 0;
        if (k >= 4) { memcpy(rw[rn], w, k + 1); rn++; }
    }
    // FULL coverage: corroborate only if EVERY salient query word is a near-typo of some card word. A
    // typo'd query ("varaible") covers fully; a SCOPED query ("capitale della BAVIERA") leaves the
    // discriminating entity uncovered -> NOT a misspelling of this card -> no lexical rescue.
    int salient = 0, covered = 0;
    for (const char *p = q; *p; ) {
        while (*p && !isalnum((unsigned char)*p) && *p != '\'') p++;
        if (!*p) break;
        char w[24]; int k = 0; while (*p && (isalnum((unsigned char)*p)) && k < 23) w[k++] = *p++; w[k] = 0;
        while (*p == '\'') p++;                                  // skip an elision tick
        if (k < 4) continue;
        bool st = false; for (int s = 0; stop[s]; s++) if (!strcmp(w, stop[s])) { st = true; break; }
        if (st) continue;
        salient++;
        int maxd = (k >= 7) ? 2 : 1;                            // longer words tolerate one more edit
        for (int r = 0; r < rn; r++) if (a_damlev(w, rw[r], maxd) <= maxd) { covered++; break; }
    }
    return salient >= 1 && covered == salient;
}

int nucleo_anima_l1_query(const char *text, bool en, bool want_detail, anima_result_t *out)
{
    g_anima_stage = 0xC0;                          // DIAG: entered the L1 semantic tier
    // Transparent AKB5 routing: when a category-sharded manifest is present, EVERY caller (and the
    // stitch/band that follow) is served from the right shard with no change at the call sites. The
    // router re-enters this function once per shard with s_in_akb5 set, so those run the flat search.
    if (s_akb5_on && !s_in_akb5) return nucleo_anima_l1_akb5_query(text, en, want_detail, out);
    s_band.a1 = s_band.a2 = -1; s_band.c1 = s_band.c2 = -2.0f;
    if (!s_ready || !text || !ensure_index()) return 0;     // reload the index on demand if it was freed
    g_anima_stage = 0xC8;                          // DIAG: flat L1 search (index resident, encoding)
    static int8_t qv[L1_MAXDIM];
    if (!l1_encode(text, qv)) return 0;
    // float (hardware FPU on the S3) not double (software-emulated). Norm via an int32 sum-of-squares
    // (exact: |qv|<=127, D<=256 -> <=4.1M, fits int32), then one hardware sqrtf.
    int32_t qss = 0; for (uint32_t k = 0; k < s_D; k++) qss += (int32_t)qv[k]*qv[k];
    float qn = sqrtf((float)qss);
    if (qn < 1e-6f) return 0;

    // 1) nearest TOPC centroids (RAM, instant). Centroid norms are PRECOMPUTED (s_cnorm) — they never
    //    change between queries, so the old per-query sum(cv^2)+sqrt over all K*D was pure waste.
    int keep = L1_TOPC;
#ifdef ANIMA_HOST
    keep = anima_env_i("L1_KEEP", L1_TOPC); if (keep > L1_TOPC_CAP) keep = L1_TOPC_CAP; if (keep < 1) keep = 1;
#endif
    int topc[L1_TOPC_CAP]; float topv[L1_TOPC_CAP];
    for (int i = 0; i < keep; i++) { topc[i] = -1; topv[i] = -2.0f; }
    g_anima_stage = 0xCC;                          // DIAG: centroid scan (flat search)
    for (uint32_t c = 0; c < s_K; c++) {
        const int8_t *cv = s_centroids + (size_t)c * s_D;
        int32_t dot = 0; for (uint32_t k = 0; k < s_D; k++) dot += (int32_t)qv[k]*cv[k];
        float cn = s_cnorm ? s_cnorm[c] : 0.0f;
        if (!s_cnorm) { int32_t s = 0; for (uint32_t k = 0; k < s_D; k++) s += (int32_t)cv[k]*cv[k]; cn = sqrtf((float)s); }
        float cos = (float)dot / (qn * cn + 1e-6f);
        for (int i = 0; i < keep; i++)
            if (cos > topv[i]) { for (int j = keep-1; j > i; j--) { topv[j]=topv[j-1]; topc[j]=topc[j-1]; } topv[i]=cos; topc[i]=(int)c; break; }
    }

    // Governor-lite (adaptive probe): probe ONE cluster when the best centroid clearly wins
    // (cheaper SD/CPU), more when it's ambiguous (better recall). margin = best vs 2nd cosine.
    float margin = (topc[1] >= 0) ? (topv[0] - topv[1]) : 9.0f;
    // Probe >=3 clusters by default: as the corpus grew (~30k vectors over K=93), the answer card can
    // sit 2-3 clusters away from the query's nearest centroid. Only a near-unambiguous best stays at 2.
    int nprobe = margin >= 0.28f ? 2 : margin >= 0.10f ? 3 : L1_TOPC;
#ifdef ANIMA_HOST
    { int ov = anima_env_i("L1_NPROBE", 0); if (ov > 0) nprobe = ov; }
#endif
    if (nprobe > keep) nprobe = keep;   // never probe more clusters than we kept (device: keep==L1_TOPC, no-op)

    // 2) rerank the probed clusters; track the top-2 DISTINCT cards (by answer offset).
    float bestcos = -2.0f, secondcos = -2.0f; long best_ansoff = -1, second_ansoff = -1;
    static int8_t row[L1_MAXDIM];
    bool use_prefilter = (s_sig_base != 0);
#ifdef ANIMA_HOST
    if (getenv("ANIMA_L1_EXACT")) use_prefilter = false;   // force the brute-force path (parity diff testing)
#endif
    if (use_prefilter) {
        g_anima_stage = 0xC9;                      // DIAG: flat prefilter (signature SD reads)
        // ---- FAST PATH (AKB4): asymmetric prefilter, then exact rerank of M survivors --------------
        // Pass 1: stream each probed cluster's signature block (D/8 B/vec) and keep the M globally best
        // by the ASYMMETRIC score sum_k q[k] over the DB's set sign bits (full-precision query, 1-bit DB;
        // rank-equivalent to int8(q).sign(db)) — same 8x-smaller signatures as the old Hamming filter, no
        // int8 vector reads / sqrt here, but it keeps the cosine-winner far more often (so M is 4x smaller).
        // Pass 2 reads & exact-cosine-reranks ONLY those M, so the gate still sees EXACT cosines (identical
        // semantics — just far fewer, better-chosen candidates). Knee: tools/anima/holo_probe.py + holo_cascade.py.
        enum { SIGCHUNK = 16 };
        const int words = s_sigb / 8;
        int M = L1_PREFILTER_M, use_asym = 1;
        uint64_t qsig[L1_MAXDIM / 64];
#ifdef ANIMA_HOST
        { int ov = anima_env_i("L1_PFM", 0); if (ov > 0 && ov <= L1_PREFILTER_MMAX) M = ov; }
        if (getenv("ANIMA_L1_NOASYM")) use_asym = 0;       // ablation: rank by the old symmetric Hamming
#endif
        if (!use_asym) l1_sign_sig(qv, s_D, qsig);          // qsig only needed by the Hamming ablation
        struct kept_t { int32_t score; uint32_t idx; } kept[L1_PREFILTER_MMAX];   // score: larger is better
        for (int i = 0; i < M; i++) { kept[i].score = L1_SCORE_MIN; kept[i].idx = 0; }
        int worst = 0, worst_score = L1_SCORE_MIN;          // worst = smallest score among the kept M
        uint8_t sbuf[SIGCHUNK * (L1_MAXDIM / 8)];          // <=16 signatures per SD read (<=512 B stack)
#ifdef ANIMA_HOST
        s_st_cand = 0; s_st_surv = 0;
#endif
        for (int t = 0; t < nprobe; t++) {
            int c = topc[t]; if (c < 0) continue;
            uint32_t off = s_cdir[c*2], cnt = s_cdir[c*2+1];
            if (cnt == 0) continue;
#ifdef ANIMA_HOST
            s_st_cand += cnt;
#endif
            if (fseek(s_idx, s_sig_base + (long)off * s_sigb, SEEK_SET) != 0) continue;
            for (uint32_t done = 0; done < cnt; ) {
                uint32_t chunk = cnt - done; if (chunk > SIGCHUNK) chunk = SIGCHUNK;
                size_t want = (size_t)chunk * s_sigb;
                if (fread(sbuf, 1, want, s_idx) != want) break;
                for (uint32_t r = 0; r < chunk; r++) {
                    const uint8_t *sp = sbuf + (size_t)r * s_sigb;
                    int32_t score;
                    if (use_asym) {                        // asymmetric: q[k] accumulated over set sign bits
                        score = 0;
                        for (int w = 0; w < words; w++) {
                            uint64_t bits; memcpy(&bits, sp + (size_t)w * 8, 8);
                            int base = w * 64;
                            while (bits) { int b = __builtin_ctzll(bits); score += qv[base + b]; bits &= bits - 1; }
                        }
                    } else {                               // symmetric Hamming (negated so larger is better)
                        int h = 0;
                        for (int w = 0; w < words; w++) {
                            uint64_t vw; memcpy(&vw, sp + (size_t)w * 8, 8);
                            h += __builtin_popcountll(qsig[w] ^ vw);
                        }
                        score = -h;
                    }
                    if (score > worst_score) {            // displace the current worst slot, then rescan
                        kept[worst].score = score; kept[worst].idx = off + done + r;
                        worst_score = kept[0].score; worst = 0;
                        for (int i = 1; i < M; i++) if (kept[i].score < worst_score) { worst_score = kept[i].score; worst = i; }
                    }
                }
                done += chunk;
            }
        }
        // Pass 2: exact cosine on the survivors only. Sort by idx so the SD reads move forward-only.
        for (int i = 1; i < M; i++) {
            struct kept_t key = kept[i]; int j = i - 1;
            while (j >= 0 && kept[j].idx > key.idx) { kept[j+1] = kept[j]; j--; } kept[j+1] = key;
        }
        for (int i = 0; i < M; i++) {
            if (kept[i].score == L1_SCORE_MIN) continue;  // empty slot: fewer than M candidates were probed
#ifdef ANIMA_HOST
            s_st_surv++;
#endif
            if (fseek(s_idx, s_vec_base + (long)kept[i].idx * (s_D + 4), SEEK_SET) != 0) continue;
            if (fread(row, 1, s_D, s_idx) != s_D) continue;
            uint32_t ansoff = rd_u32(s_idx);
            int32_t dot = 0, vss = 0;
            for (uint32_t k = 0; k < s_D; k++) { dot += (int32_t)qv[k]*row[k]; vss += (int32_t)row[k]*row[k]; }
            float cos = (float)dot / (qn * sqrtf((float)vss) + 1e-6f);
            long ao = (long)ansoff;
            if (cos > bestcos) {
                if (ao != best_ansoff) { secondcos = bestcos; second_ansoff = best_ansoff; }
                bestcos = cos; best_ansoff = ao;
            } else if (ao != best_ansoff && cos > secondcos) {
                secondcos = cos; second_ansoff = ao;
            }
        }
    } else {
        // ---- EXACT PATH (plain AKB3): brute-force cosine over every probed vector (unchanged) ----
        for (int t = 0; t < nprobe; t++) {
            int c = topc[t]; if (c < 0) continue;
            uint32_t off = s_cdir[c*2], cnt = s_cdir[c*2+1];
            if (cnt == 0) continue;
            if (fseek(s_idx, s_vec_base + (long)off * (s_D + 4), SEEK_SET) != 0) continue;
            for (uint32_t r = 0; r < cnt; r++) {
                if (fread(row, 1, s_D, s_idx) != s_D) break;
                uint32_t ansoff = rd_u32(s_idx);
                int32_t dot = 0, vss = 0;
                for (uint32_t k = 0; k < s_D; k++) { dot += (int32_t)qv[k]*row[k]; vss += (int32_t)row[k]*row[k]; }
                float cos = (float)dot / (qn * sqrtf((float)vss) + 1e-6f);   // float FPU, not emulated double
                long ao = (long)ansoff;
                if (cos > bestcos) {
                    if (ao != best_ansoff) { secondcos = bestcos; second_ansoff = best_ansoff; }  // demote old best
                    bestcos = cos; best_ansoff = ao;
                } else if (ao != best_ansoff && cos > secondcos) {
                    secondcos = cos; second_ansoff = ao;
                }
            }
        }
    }
    s_band.c1 = bestcos; s_band.a1 = best_ansoff; s_band.c2 = secondcos; s_band.a2 = second_ansoff;
#ifdef ANIMA_HOST
    if (s_sig_base && getenv("ANIMA_L1_STATS")) {
        long ed = s_st_cand, fd = s_st_surv, sb = s_st_cand * s_sigb + s_st_surv * (s_D + 4);
        long eb = s_st_cand * (long)(s_D + 4);
        fprintf(stderr, "L1STATS exact{dots=%ld vecB=%ld}  fast{dots=%ld ioB=%ld}  dots=%.1fx  io=%.1fx\n",
                ed, eb, fd, sb, fd ? (double)ed/fd : 0.0, sb ? (double)eb/sb : 0.0);
    }
#endif

    // Evidential gate: high absolute, OR a mid-confidence match that is distinctly the best (strong
    // margin = corroborating evidence the encoder picked the right card — see L1_RESCUE_* above).
    // The margin is only meaningful against a REAL runner-up, so the rescue requires a distinct
    // second card (else a lone-card cluster would rescue any weak 0.72+ match at "infinite" margin).
    float ev_margin = bestcos - secondcos;
    float gate_min = L1_COS_MIN, rescue_abs = L1_RESCUE_ABS, rescue_marg = L1_RESCUE_MARGIN;
#ifdef ANIMA_HOST
    gate_min = anima_env_f("L1_GATE", gate_min); rescue_abs = anima_env_f("L1_RABS", rescue_abs);
    rescue_marg = anima_env_f("L1_RMARG", rescue_marg);
#endif
    // Below the absolute rescue floor -> no signal worth corroborating; never answer.
    if (best_ansoff < 0 || bestcos < rescue_abs) return 0;
    bool is_rescue = bestcos < gate_min;        // in the rescue band -> needs a second evidence channel

    // 3) read the winning bilingual answer record from SD NOW, so the rescue can corroborate the match
    //    lexically before accepting. (want_detail with no detail -> honest "nothing more".)
    if (!l1_read_answer(best_ansoff, en, want_detail, out)) return 0;

    // DUAL-CHANNEL evidential gate. The hard cosine gate, OR — in the rescue band — a SECOND independent
    // channel: (a) MARGIN, the match is distinctly the best; OR (b) LEXICAL, a query word is a near-typo
    // of a word in the matched card (the query misspells THIS card, not OOD). Recovers typo'd queries
    // ("varaible"->variabile @0.79) without lowering the gate for genuine junk (no lexical match).
    bool margin_ok = (second_ansoff >= 0 && ev_margin >= rescue_marg);
    bool lex_ok    = l1_lexically_corroborated(text, out->reply);
    // The rescue band (margin OR lexical) recovers only ANSWER cards (knowledge). A launch/tool card must
    // clear the HARD gate or arrive via the L0 open intent — never let a statement that merely names an
    // app ("che bella la calcolatrice") be rescued into OPENING it (inverse-of-intent action).
    bool rescued = (out->action == ANIMA_ACT_ANSWER) && (margin_ok || lex_ok);
    if (!(bestcos >= gate_min || rescued)) { memset(out, 0, sizeof *out); return 0; }
    // EVIDENCE COVERAGE: a superlative answer whose scope qualifier the card never mentions is a
    // global-default-applied-to-a-narrower-question ("fiume più lungo DELLA CINA" -> "il Nilo") ->
    // abstain rather than answer confidently wrong. Runs in BOTH bands: a strong cosine to the global
    // card doesn't make the scoped answer right. Transparent to non-superlatives (returns covered).
    if (!l1_scope_covered(text, out->reply)) { memset(out, 0, sizeof *out); return 0; }
    // mid-confidence rescues must also be on-topic (kills off-topic rescues like "imperatore di Marte").
    if (is_rescue && !l1_rescue_on_topic(text, out->reply)) { memset(out, 0, sizeof *out); return 0; }
    // a LONE salient word absent from the card, rescued only by margin (no typo corroboration), is an encoder
    // collision ("gatto" -> the "cat" command card) -> abstain rather than answer confidently off-topic. Kept
    // to the rescue band: at the absolute gate the same check muted legit identity/greeting/hyphenated cards
    // ("come ti chiami" -> "Sono ANIMA"; "wifi" -> "Wi-Fi") whose answer rightly omits the query token.
    if (is_rescue && !lex_ok && l1_lone_uncovered(text, out->reply)) { memset(out, 0, sizeof *out); return 0; }
    // AKB5 only: the sharded path has no global runner-up to pull a false premise into the clarify band,
    // so require full premise+entity coverage instead. Flat (s_in_akb5 == false) keeps its competition gate.
    if (s_in_akb5 && out->action == ANIMA_ACT_ANSWER && !l1_premise_covered(text, out->reply)) {
        memset(out, 0, sizeof *out); return 0;
    }
    out->confidence = (int)(bestcos * 100.0f + 0.5f);
    out->budget = nprobe;                       // micro-thought: clusters actually probed
    return 1;
}

// Dialogic clarify band: call after nucleo_anima_l1_query() returned 0 (no direct answer). If the
// best candidate is moderately similar (in [LO, MIN)) and a runner-up is genuinely competing
// (margin < MARGIN), fill `out` with a "did you mean X or Y?" question and hand back the two
// answer offsets so the caller can resolve the pick. Returns 0 (caller refuses honestly) otherwise.
// Safe by construction: a clarify is a question, never an asserted fact -> zero false positives.
int nucleo_anima_l1_band(bool en, anima_result_t *out, long *ans1, long *ans2)
{
    if (!s_ready || s_band.a1 < 0 || s_band.a2 < 0) return 0;
    if (!(s_band.c1 >= L1_COS_LO && s_band.c1 < L1_COS_MIN)) return 0;
    if (s_band.c2 < L1_COS_LO) return 0;                            // BOTH options must be plausible
    if ((s_band.c1 - s_band.c2) >= L1_BAND_MARGIN) return 0;        // not genuinely competing
    // The cascade unloads L1 before the HDC/online tiers; the band runs AFTER them, so the index
    // (s_idx) is closed by now and l1_label would read nothing. Reload it to read the two labels —
    // cheap and rare, and the heavy tiers that needed the contiguous heap have already finished.
    if (!ensure_index()) return 0;
    char la[64], lb[64];
    // Knowledge disambiguation only: both candidates must be answer cards (not app/system), else a
    // typo'd launch ("apri le ofto") or an off-topic query offers nonsense ("did you mean Apro X?").
    if (l1_label(s_band.a1, en, la, sizeof(la)) != ANIMA_ACT_ANSWER) return 0;
    if (l1_label(s_band.a2, en, lb, sizeof(lb)) != ANIMA_ACT_ANSWER) return 0;
    if (!la[0] || !lb[0]) return 0;
    memset(out, 0, sizeof(*out));
    out->tier = ANIMA_TIER_FACT;
    out->action = ANIMA_ACT_ANSWER;
    out->awaiting = 1;
    out->confidence = (int)(s_band.c1 * 100.0f + 0.5f);
    snprintf(out->intent, sizeof(out->intent), "clarify");
    snprintf(out->state, sizeof(out->state), "clarify");
    snprintf(out->reply, sizeof(out->reply),
             en ? "Not sure — did you mean: 1) %s  or  2) %s?" : "Non sono sicuro: intendi 1) %s  o  2) %s?", la, lb);
    *ans1 = s_band.a1; *ans2 = s_band.a2;
    return 1;
}

// Resolve a clarify pick: read the full answer at `ansoff` (one of the two offered offsets).
int nucleo_anima_l1_read(long ansoff, bool en, anima_result_t *out)
{
    if (!s_ready || !l1_read_answer(ansoff, en, false, out)) return 0;
    out->confidence = 75;                        // a confirmed choice, not a raw cosine score
    return 1;
}

// ---- MOSAICO (L2 span-stitch) -------------------------------------------------------------------
// Helpers below build a fuller answer by COPYING verbatim frozen spans; the connectives they insert
// (". ", " Inoltre, ", " Also, ") are the ONLY non-frozen text — the fluency-grounded gate strips them
// and proves every remaining sentence is a verbatim corpus field.

// Does any >=4-char query token appear as a whole word in `reply`? (accent-blind lexical anchor)
static bool l1_shares_token(const char *query, const char *reply)
{
    char ql[200], rl[400]; size_t i;
    for (i = 0; query[i] && i + 1 < sizeof ql; i++) ql[i] = (char)tolower((unsigned char)query[i]); ql[i] = 0;
    for (i = 0; reply[i] && i + 1 < sizeof rl; i++) rl[i] = (char)tolower((unsigned char)reply[i]); rl[i] = 0;
    char tok[40]; int k = 0;
    for (const char *p = ql; ; p++) {
        if (isalnum((unsigned char)*p)) { if (k < 39) tok[k++] = *p; }
        else { tok[k] = 0; if (k >= 4 && l1_word_in(rl, tok)) return true; k = 0; if (!*p) break; }
    }
    return false;
}

// Is `span`'s head (first ~40 chars) already present in `buf`? Avoids restating the lead in the detail.
static bool l1_head_in(const char *buf, const char *span)
{
    char key[48], low[400]; int i;
    for (i = 0; span[i] && i < 40; i++) key[i] = (char)tolower((unsigned char)span[i]); key[i] = 0;
    if (i < 8) return false;
    for (i = 0; buf[i] && i + 1 < (int)sizeof low; i++) low[i] = (char)tolower((unsigned char)buf[i]); low[i] = 0;
    return strstr(low, key) != NULL;
}

static void l1_join(char *buf, size_t cap, const char *glue, const char *span)
{
    size_t n = strlen(buf);
    snprintf(buf + n, cap - n, "%s%s", glue, span);
}

int nucleo_anima_l1_stitch(const char *query, bool en, anima_result_t *io)
{
    if (!s_ready || !io || io->action != ANIMA_ACT_ANSWER) return 0;
    long a1 = s_band.a1, a2 = s_band.a2; float c2 = s_band.c2;
    if (a1 < 0) return 0;
    float stitch_c2 = 0.80f;                       // runner-up cosine floor (high: never staple a weak match)
#ifdef ANIMA_HOST
    stitch_c2 = anima_env_f("L1_STITCH_C2", stitch_c2);
#endif
    char buf[384]; snprintf(buf, sizeof buf, "%s", io->reply);
    if (!buf[0]) return 0;
    int stitched = 0;

    // SPAN 1 — the SAME card's drill-down detail (deeper text on the very entity just answered).
    anima_result_t d;
    if (l1_read_answer(a1, en, /*want_detail*/true, &d) && d.reply[0] && !l1_head_in(buf, d.reply)) {
        char ec = buf[strlen(buf) - 1];
        const char *glue = (ec == '.' || ec == '!' || ec == '?') ? " " : ". ";
        if (strlen(buf) + strlen(glue) + strlen(d.reply) < 360) { l1_join(buf, sizeof buf, glue, d.reply); stitched++; }
    }

    // SPAN 2 — a topically-coherent runner-up card: high cosine AND shares a query word AND passes the
    // same scope guard AND isn't a duplicate. Strict so MOSAICO never bolts on an off-topic fact.
    if (a2 >= 0 && a2 != a1 && c2 >= stitch_c2) {
        anima_result_t r2;
        if (l1_read_answer(a2, en, /*want_detail*/false, &r2) && r2.action == ANIMA_ACT_ANSWER && r2.reply[0]
            && l1_shares_token(query, r2.reply) && l1_scope_covered(query, r2.reply) && !l1_head_in(buf, r2.reply)) {
            char ec = buf[strlen(buf) - 1];
            const char *glue = (ec == '.' || ec == '!' || ec == '?') ? (en ? " Also, " : " Inoltre, ")
                                                                      : (en ? ". Also, " : ". Inoltre, ");
            if (strlen(buf) + strlen(glue) + strlen(r2.reply) < 360) { l1_join(buf, sizeof buf, glue, r2.reply); stitched++; }
        }
    }

    if (!stitched) return 0;
    io->tier = ANIMA_TIER_STITCH;
    snprintf(io->intent, sizeof io->intent, "mosaico");
    snprintf(io->reply, sizeof io->reply, "%s", buf);
    return 1;
}

// Diagnostics: expose the most-recent query's top-2 distinct cosines (see anima_l1.h).
void nucleo_anima_l1_last_band(float *c1, float *c2) { if (c1) *c1 = s_band.c1; if (c2) *c2 = s_band.c2; }

// Encoder dimension (0 if L1/encoder isn't loaded). Lets other tiers size a query vector.
int nucleo_anima_l1_dim(void) { return s_ready ? (int)s_D : 0; }

// Embed `text` with the SAME device encoder L1 uses, into out[>=dim]. Returns the dim (>0) or 0 if
// the encoder isn't loaded / text is empty. Shared so the online tier can recall learned cards by
// meaning using vectors that are consistent with the query (see nucleo_anima_online.c recall).
int nucleo_anima_l1_encode(const char *text, int8_t *out, int cap)
{
    if (!s_ready || !text || !out || cap < (int)s_D) return 0;
    if (!l1_encode(text, out)) return 0;
    return (int)s_D;
}

// ============================================================================
// AKB5 — category-SHARDED scalable index (docs/anima-knowledge-scale.md Move 1).
// The manifest (anima-it-akb5.bin) is a 2-level IVF: it holds every shard's coarse centroids, STREAMED
// per query (no resident router). The query is routed to the few best-matching category shards; each is a
// self-contained AKB4 file searched by REUSING the entire proven nucleo_anima_l1_query (search + AKB4
// prefilter + 0.85 gate + evidence guards) — zero duplication. Only ONE shard's centroids are resident at
// a time, so RAM stays flat at any #shards/GB; per-query SD touches just the manifest + a few shards.
// Built by tools/anima/build_akb5.py. Absent manifest -> the device transparently uses the flat index.
// ============================================================================
#define AKB5_MANIFEST  NUCLEO_SD_MOUNT "/data/anima/anima-it-akb5.bin"
#define AKB5_SHARDDIR  NUCLEO_SD_MOUNT "/data/anima/akb5/"
#define AKB5_MAXSHARD  256
#define AKB5_PROBE     3        // top distinct shards to search per query (recall ~= flat at this corpus)

// Cheap probe: is a valid AKB5 manifest present whose dim matches the encoder? (open + magic + dim)
bool nucleo_anima_l1_akb5_available(void)
{
    if (!s_ready) return false;
    FILE *f = fopen(AKB5_MANIFEST, "rb"); if (!f) return false;
    char mg[4]; uint32_t d = 0; bool ok = (fread(mg, 1, 4, f) == 4 && memcmp(mg, "AKB5", 4) == 0);
    if (ok) d = rd_u32(f);
    fclose(f);
    return ok && d == s_D;
}

// AKB5 sharded query. Streams the manifest centroids, ranks shards by their best query·centroid, then
// searches the top AKB5_PROBE shards via nucleo_anima_l1_query (full search+gate). Keeps the highest-
// confidence answer. Restores the flat index path on exit. Returns 1 if a shard answered above the gate.
int nucleo_anima_l1_akb5_query(const char *text, bool en, bool want_detail, anima_result_t *out)
{
    if (!s_ready || !text) return 0;
    FILE *f = fopen(AKB5_MANIFEST, "rb"); if (!f) return 0;
    char mg[4]; if (fread(mg, 1, 4, f) != 4 || memcmp(mg, "AKB5", 4) != 0) { fclose(f); return 0; }
    uint32_t D = rd_u32(f), ns = rd_u32(f);
    if (D != s_D || ns == 0 || ns > AKB5_MAXSHARD) { fclose(f); return 0; }
    g_anima_stage = 0xC2;                          // DIAG: AKB5 manifest header valid

    int8_t qv[L1_MAXDIM];
    if (nucleo_anima_l1_encode(text, qv, L1_MAXDIM) != (int)D) { fclose(f); return 0; }
    double qn2 = 0; for (uint32_t k = 0; k < D; k++) qn2 += (double)qv[k] * qv[k];
    float qn = sqrtf((float)qn2) + 1e-6f;

    struct shard { char name[32]; uint16_t K; float best; } *tab = malloc((size_t)ns * sizeof *tab);
    if (!tab) { fclose(f); return 0; }
    for (uint32_t i = 0; i < ns; i++) {
        int nl = fgetc(f);
        if (nl < 0 || nl > 31 || fread(tab[i].name, 1, (size_t)nl, f) != (size_t)nl) { free(tab); fclose(f); return 0; }
        tab[i].name[nl] = 0;
        rd_u32(f); rd_u32(f);                 // ncards, N (unused in routing)
        tab[i].K = rd_u16(f); tab[i].best = -2.0f;
        if (tab[i].K == 0 || tab[i].K > 4096) { free(tab); fclose(f); return 0; }  // corrupt shard centroid count -> honest miss
    }
    // centroid block: per shard, K centroids as 1-BIT SIGNS (ceil(D/8) bytes each). Stream and keep each
    // shard's best ASYMMETRIC score (int8 query x 1-bit sign / |q|*sqrt(D)) — the same holographic measure
    // L1's prefilter uses, so routing recall is unchanged but the manifest is 8x smaller / faster to read.
    uint32_t nbytes = (D + 7) / 8;
    float inv = 1.0f / (qn * (sqrtf((float)D) + 1e-6f));
    uint8_t cb[(L1_MAXDIM + 7) / 8];
    for (uint32_t i = 0; i < ns && !feof(f); i++) {
        for (uint16_t c = 0; c < tab[i].K; c++) {
            if (fread(cb, 1, nbytes, f) != nbytes) { i = ns; break; }
            int32_t score = 0;
            for (uint32_t k = 0; k < D; k++)
                score += ((cb[k >> 3] >> (k & 7)) & 1) ? (int32_t)qv[k] : -(int32_t)qv[k];
            float s = (float)score * inv;
            if (s > tab[i].best) tab[i].best = s;
        }
    }
    fclose(f);
    g_anima_stage = 0xC4;                          // DIAG: AKB5 shard routing done

    // SCALE-AWARE probe (see firmware twin): probe ~ns/4 shards so a deep home shard is still reached as the
    // corpus grows to dozens of shards. Host/WASM override via ANIMA_AKB5_PROBE (the browser sets it high).
    int probe = (int)ns / 4;
    if (probe < AKB5_PROBE) probe = AKB5_PROBE;   // floor: the proven top-4 (unchanged for <=16 shards)
    if (probe > 20) probe = 20;                    // ceiling: bound SD reads/query
#ifdef ANIMA_HOST
    probe = anima_env_i("ANIMA_AKB5_PROBE", probe);   // host A/B sweep override
#endif
    if (probe < 1) probe = 1; if (probe > (int)ns) probe = (int)ns;
    if (getenv("ANIMA_AKB5_TRACE"))            // diagnostic (host): every shard's routing cosine
        for (uint32_t i = 0; i < ns; i++) fprintf(stderr, "[akb5] %-26s route=%.3f\n", tab[i].name, tab[i].best);
    anima_result_t best; memset(&best, 0, sizeof best);
    int best_conf = -1, answered = 0; char best_name[32] = {0};
    s_in_akb5 = true;                          // per-shard nucleo_anima_l1_query must run FLAT (no re-route)
    for (int pick = 0; pick < probe; pick++) {
        int bi = -1; float bv = -2.0f;
        for (uint32_t i = 0; i < ns; i++) if (tab[i].best > bv) { bv = tab[i].best; bi = (int)i; }
        if (bi < 0) break;
        if (getenv("ANIMA_AKB5_TRACE")) fprintf(stderr, "[akb5]  -> probe #%d: %s (route=%.3f)\n", pick+1, tab[bi].name, bv);
        tab[bi].best = -3.0f;                 // consume this shard
        snprintf(s_shard_path, sizeof s_shard_path, "%s%s", AKB5_SHARDDIR, tab[bi].name);
        s_idx_path = s_shard_path; nucleo_anima_l1_unload();    // reload L1 from this shard
        g_anima_stage = 0xC5;                      // DIAG: about to query a shard (re-entrant flat)
        anima_result_t tmp; memset(&tmp, 0, sizeof tmp);
        if (nucleo_anima_l1_query(text, en, want_detail, &tmp) && tmp.confidence > best_conf) {
            best_conf = tmp.confidence; best = tmp; answered = 1;
            snprintf(best_name, sizeof best_name, "%s", tab[bi].name);
        }
    }
    free(tab);
    // Leave the WINNING shard resident (re-run on it) so a following nucleo_anima_l1_stitch()/_band()
    // reads the right shard and s_band reflects THIS query. If nothing answered, restore the flat default.
    if (answered) {
        snprintf(s_shard_path, sizeof s_shard_path, "%s%s", AKB5_SHARDDIR, best_name);
        s_idx_path = s_shard_path; nucleo_anima_l1_unload();
        anima_result_t tmp; memset(&tmp, 0, sizeof tmp);
        nucleo_anima_l1_query(text, en, want_detail, &tmp);
        *out = best;
    } else {
        s_idx_path = IDX_PATH; nucleo_anima_l1_unload();
    }
    s_in_akb5 = false;
    return answered;
}
