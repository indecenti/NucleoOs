Perfetto! Ho i dati necessari. Procedo con la compilazione della mappa finale:

## MAPPA nucleo_anima â€” Cervello ANIMA On-Device

**1. LISTA FILE (scopo 1-riga)**

- **nucleo_anima.c** (3500+ linee): Orchestratore L0 + cascata dispatcher; tokenizer, intent-matching, L0 intents (app/sistema/FAQ), agentic controller FSM
- **nucleo_anima.h** (include/nucleo_anima.h): API pubblica, enum tier/action, result struct, dichiarazioni tier HDC/online
- **nucleo_anima_l1.c**: Encoder + index RAG semantico; AKB2/AKB4/AKB5 query, cosine gate (0.85), prefilter popcount asimmetrico M=16
- **anima_l1.h**: API L1 (query/encode/unload/policy AUTO/ON/OFF); clarify band, MOSAICO/L2 span-stitch
- **nucleo_anima_hdc.c**: Hyperdimensional reasoning (VSA 2048-bit, permutation-KGE), deductive tier entity/edge graph, coherence gate 4.0Ïƒ
- **nucleo_anima_online.c**: Teacher cloud (Grok/Claude), transcribe, verify claims vs KGE/L1
- **nucleo_anima_learn.c**: Learned triples (mind.<lang>.jsonl) loader, AKB5 shard manifest, profile storage
- **nucleo_anima_facet.c**: Typed facet lookup (occupation/gender), no generation
- **anima_solve.c**: Math solver (percent/Ohm/unitÃ /potenze/radici/modulo), registri numerici conversazionali
- **anima_text.c**: Normalization, accents folding IT/EN
- **anima_internal.h**: Dichiarazioni inter-tier (l0_legacy, a_norm_phrase, units_load)
- **nucleo_anima_bench.c**: Micro-benchmark int8 MAC, popcount, SD read throughput
- **nucleo_anima_profile.c**: Profiling, timing instrumentation
- **nucleo_anima_translate.c**: Offline dictionary multilang
- **nucleo_knowledge_manifest.h**: Costanti corpus/versioning

**2. ORDINE DISPATCH CASCATA (file:riga rami principali)**

Flusso cascata in nucleo_anima_query (nucleo_anima.c:2830+):

```
Riga 2860-2870: Resolve clarify L1 (pick ordinale)
Riga 2872-2903: Resolve knowledge<->skill clarify (cos'Ã¨ vs calcola)
Riga 2908-2919: Drill-down "tell me more" (L1 detail)
Riga 2924:      Dialogue acts (sei sicuro, spiegati meglio, ricapitola, thanks, deny)
Riga 2929-2958: Online-only mode (bypass tutta cascata se flag attivo)
Riga 2970-3004: LIVE tier (weather/news, F_WEATHER|F_NEWS gate) [nucleo_anima_online_live]
Riga 3011-3014: CODE generation (online) [nucleo_anima_online_code]
Riga 3020-3028: ENTITY questions (online) [nucleo_anima_online_is_about]
Riga 3035-3042: FACT Wikidata (online) [nucleo_anima_online_is_fact]
Riga 3054-3086: CONVERSATIONAL FOCUS SHIFT + HDC reasoner [nucleo_anima_hdc_reason]
Riga 3091-3115: STRUCTURED deduction HDC (fatto-questions) [nucleo_anima_hdc_reason]
Riga 3116-3130: NEURO-SYMBOLIC combinator (composizione fatti) [nucleo_anima_combinator]
Riga 3131-3140: NSPCG generative (explain questions + discover chain) [nucleo_anima_pcg_generate, nucleo_anima_pcg_detect]
Riga 3141+   : FACET tier (categoric lookup) [nucleo_anima_facet]
Riga 3150+   : L0 query (intent match + L0 tools) [l0_query]
Riga 3160+   : L1 semantic + MOSAICO enrichment [nucleo_anima_l1_query + l1_stitch]
Riga 3180+   : L1 clarify band (top-1 vs runner-up) [nucleo_anima_l1_band]
```

Dettaglio L0 (l0_query â†’ riga 2435+):

```
Riga 2450+:  Profile (app/file/topic) + session recovery
Riga 2460-2470: Skills auto-routing (math / translate / image-gen / create-file)
Riga 2480-2510: L0 intents table (battery/time/storage/date/version/uptime/whoami/FAQ statiche)
Riga 2513-2545: L1 fallback (topic-stripping per openers conversazionali)
```

**3. L1: CARICAMENTO INDICE + DIMENSIONI BUFFER + POLICY**

- **Caricamento** (nucleo_anima_l1.c:294-344):
  - Indice AKB3 (K centroidi, N vettori, dimensione D): file "/data/anima/anima-it-index.bin"
  - **Streaming centroids**: K*D int8 slab NO LONGER in RAM â€” read-streamed da file al query time
  - **Directory in RAM**: s_cdir = malloc(K*2*u32) â‰ˆ K*8 bytes [tiny, ~768 B per K=96]
  - **Encoder**: ~3 MB su SD o memory-mapped da flash partition "anima_enc" (fast path, zero I/O)
  - **Hot-row cache encoder**: malloc adaptive, ceiling ENC_CACHE=128 slot, floor 16 (L1_MAXDIM=256 â†’ s_ec_row = slot*256 bytes)

- **Buffer statici** (nucleo_anima_l1.c:188-190):
  - s_ec_id[ENC_CACHE] = 512 B .bss (slot tags)
  - s_ec_row: malloc'd lazily (0 .bss, heap gestito da ec_cache_acquire)

- **Policy L1** (nucleo_anima.h:95-106 + nucleo_anima_l1.c:485-518):
  - **ANIMA_L1_AUTO (0)**: Default; L1 unload quando cloud teacher configured+available (gate: if s_l1_online_brain || s_l1_ext_brain â†’ unload)
  - **ANIMA_L1_ON (1)**: Force always serve (user override via ANIMA app)
  - **ANIMA_L1_OFF (2)**: Never serve (user override)
  - Orchestrator sets s_l1_online_brain = nucleo_anima_online_available() && teacher_configured() (nucleo_anima.c:2839)

- **Prefiltro popcount** (nucleo_anima_l1.c:109-130, 1143-1200):
  - **AKB4 asymmetric holographic**: opzionale, backward-compatible
  - **Signature trailer**: "ASIG" 16-byte footer EOF (sig_base offset, sig_bits=D, version)
  - **Asymmetric score**: q[k] accumulated over DB set-sign bits (int8 query vs 1-bit signature) â†’ 4x piÃ¹ veloce vs Hamming simmetrico
  - **Prefilter M=16** survivors (era 64 symmetric): L1_PREFILTER_M=16, L1_PREFILTER_MMAX=64 host override
  - **Host env override**: ANIMA_L1_PFM (0-64), ANIMA_L1_NOASYM (force symmetric fallback)

**4. HEAP GATES (guard soglie + file:riga)**

- **Gate L1 serving** (nucleo_anima_l1.c:497-518):
  - nucleo_anima_l1_serving() riga 497: return !(s_l1_online_brain || s_l1_ext_brain) se AUTO
  - nucleo_anima_l1_set_online_brain()/set_external_brain() riga 509-517: set flags + unload if !serving

- **Online gate 2-sbarre** (nucleo_anima.c:2839, 2995, 3012, 3021, 3036, 3076):
  - **Sbarra 1** (pre-TLS): if nucleo_anima_online_available() â†’ nucleo_anima_l1_unload() (sgancia heap per handshake)
  - **Sbarra 2** (ring-gate): l1_heap_bytes() (nucleo_anima_l1.c:479) = s_cdir + s_ec_row; web /api/heap somma al largest_free_block

- **DIAG breadcrumb RTC** (nucleo_anima.c:2809):
  - g_anima_stage u32 RTC_NOINIT: 0xB0 entry, 0xC0 L1, 0xC1 L1 load SD, 0xD0 online
  - Sopravvive warm reboot, loggato al crash /api/logs

- **Runtime guardie**:
  - ec_cache_acquire() riga 193-206: best-effort malloc down from ENC_CACHE=128 â†’ 64 â†’ 32 â†’ 16
  - load_index() riga 317-321: fallback hand-back s_ec_row if cdir malloc fails, retry once
  - l1_encode() riga 238-280: fallback stackrow[L1_MAXDIM] se s_ec_row==NULL (cache freed/OOM)

**5. BUFFER STATICI / .bss GRANDI**

Dichiarazioni (grep output):

```c
// nucleo_anima.c riga 548-589 â€” s_session struct (static, NO malloc):
struct {
  char last_app[64];                  // ~64 B
  char last_file[64];                 // ~64 B
  char last_topic[96];                // ~96 B
  char pending_tool[16];              // ~16 B
  char pending_slot[16];              // ~16 B
  char pending_arg[48];               // ~48 B
  char clarify_opt[2][32];            // ~64 B
  struct { char q[80]; char a[200]; } chat[ANIMA_CHAT=4];  // ~1.1 KB
  struct { char input[64]; char intent[16]; char domain[12]; char arg[32]; } ring[ANIMA_RING=8];  // ~832 B
  struct { char name[12]; double val; bool used; } reg[ANIMA_REGS=8];  // ~144 B
  double last_num; bool has_last;     // ~12 B
  char foc_subject[48]; char foc_relation[24];  // ~72 B
  uint32_t foc_turn, turn; bool dirty; // ~12 B
  struct { char reply[200]; anima_tier_t tier; int conf; char intent[16]; } last;  // ~228 B
} s_session                           // â‰ˆ 3.2 KB .bss, accettabile
```

Dichiarazioni L1 (nucleo_anima_l1.c):

```c
// Riga 188-190 â€” encoder cache tag array (fixed .bss):
static uint32_t s_ec_id[ENC_CACHE=128];  // 512 B .bss
// s_ec_row, s_ec_slots: heap allocation (NOT .bss)
// s_cdir, s_idx, s_cnorm, s_centroids: dinamico (malloc)
```

Dichiarazioni HDC (nucleo_anima_hdc.c):

```c
// Riga 54, 76 â€” scratch allocati dinamicamente (NOT static):
static int8_t *g_simcnt;  // allocated in hdc_selftest (transient, freed)
static int8_t *g_bcnt;    // allocated in hdc_selftest (transient, freed)
// Codebook entity HV: malloc'd transiently per query (kg_t.cb)
```

Dichiarazioni trace (nucleo_anima.c:1192-1205):

```c
static char s_trace[112];           // thought-log steps, reset per query
static char s_tool_content[AG_CONTENT_MAX=200];  // composed payload
static char *s_long_reply = NULL;   // malloc'd overflow reply per query
```

**6. PUNTI MALLOC/CALLOC/STRDUP IN PATH CALDI**

Caricamento L1 query (nucleo_anima_l1.c:443-452):

```
Riga 317-320: malloc(cdir_sz = K*8) [load_index â†’ ensure_index â†’ l1_encode]
Riga 193-206: malloc adaptive hot-row cache, worst-case K*D slot [ec_cache_acquire]
```

HDC reasoner (nucleo_anima_hdc.c:98-102):

```
Riga 99-102: malloc pool (NSLOT*HDC_W*sizeof u32 = 18*64*4 â‰ˆ 4.5 KB)
           malloc g_simcnt (HDC_D*sizeof int8 = 2048 B)
           malloc g_bcnt (HDC_D*sizeof int8 = 2048 B)
           [all freed before return â€” transient selftest only]
```

KGE query (nucleo_anima_hdc.c:~250+):

```
kg_t.cb: malloc(n * HDC_W * sizeof u32) [n<=12 entities, max 3 KB]
[free'd before return â€” transient per query]
```

Online tier (nucleo_anima_online.c):

```
Riga 640: free(newline) [response text malloc'd]
Riga 915, 937: malloc acc.buf (HTTP_CAP), free'd
Riga 1226: malloc resp (HTTP_CAP), free'd per query
Riga 2701-2763: malloc body, resp (cloud calls), free'd
```

L0/agent (nucleo_anima.c):

```
Riga 1215: s_long_reply = strdup(s) [overflow reply, freed next query]
Riga 2203: s_tool_content buffer (static 200 B, no malloc)
```

**7. HDC/KGE/NSPCG STRUTTURE DATI MEMORIA**

Dichiarazioni (nucleo_anima_hdc.c:25-185):

```c
#define HDC_D 2048               // hypervector bits; HDC_W = D/32 = 64 words
#define HDC_W 64
typedef uint32_t hv_t[HDC_W];    // 256 B per HV

// Knowledge graph entity codebook (transient, malloc'd per query):
typedef struct {
    char name[KG_MAXENT=12][KG_NAMELEN=48];
    char label[KG_MAXENT][KG_VALLEN=96];
    uint32_t *cb;                 // n consecutive HVs (n entity count, max 12)
    int edge_h[KG_MAXTRIP=48];    // triple head idx
    int edge_t[KG_MAXTRIP];       // triple tail idx
    int edge_k[KG_MAXTRIP];       // rotation amount rel_shift
    int n, ntrip, seed_idx;
    int ambig_n;                  // proactive disambiguation candidate count
    char ambig[KG_AMBIG=6][KG_VALLEN];
} kg_t;
```

Memoria percorsi:

- **Deductive (hdc_reason)**: kg_t malloc'd per query, min ~2.3 KB (n=1), max ~3.6 KB (n=12)
- **Combinator (fact composition)**: same kg_t riutilizzato, edges consulted per compose pattern
- **NSPCG (generative explain)**: kg_t + chain discovery per relation permutazione
- **Coherence gate**: HDC_REASON_GATE = 4.0Ïƒ (margin/std), refuse se coh < gate (honesty, non-fabrication)

Popcount accelerazione (nucleo_anima_hdc.c:40, 135-141):

```c
// Hamming distance built-in popcount per bit-distance (HD_STD â‰ˆ sqrt(D)/2 = 22.63)
static inline int hamming(const uint32_t *a, const uint32_t *b) {
    int d = 0;
    for (int i = 0; i < HDC_W; i++)
        d += __builtin_popcount(a[i] ^ b[i]);
    return d;
}

// Timing selftest (riga 136-141): ~popcount throughput measurement on S3
// ~30k HV Hamming/ms reported â†’ 8192 HV scansione ~0.27 sec (fast-path bench)
```

---

**MAPPA SINTETICA HEAP/PERCORSI:**

| Modulo | Malloc | Dimensione | When | Where |
|--------|--------|-----------|------|-------|
| L1 dir | cdir | K*8 B (max 768 B) | Query L1 | load_index |
| L1 encoder-cache | ec_row | adaptive 16-128 slot * D (max 32 KB) | Query L1 | ec_cache_acquire |
| HDC selftest | pool+scratch | 18 HV + 2*simcnt (~9 KB) | Boot once | selftest only |
| KGE query | kg.cb | n*256 B (nâ‰¤12, max 3 KB) | HDC query | hdc_reason |
| Online | resp/body | HTTP_CAP (~64 KB) | Online call | per query |
| Session | s_session (static) | ~3.2 KB .bss | Global | persistent |

---

**CONCLUSIONE MAPPA:**

ANIMA Ã¨ una cascata 8-tier (L0â†’L1â†’HDCâ†’online) con **streaming L1** (centroids file, dir in RAM) + **transient KGE** (build per-query, free dopo). Gate doppi online (pre-TLS unload L1, heap_bytes advisory). Policy AUTO sgancia L1 quando cloud teacher o browser LLM attivo. Popcount nativo ESP32-S3 (~30k HV/ms) alimenta deduction coerenza-gated. Zero generazione fino a Grok/Claude; offline Ã¨ fact-only (L1 cards + KGE reasoning).
