// Host assertions for nucleo_take: the VAD segmenter, the journal writer and the WAV recovery
// helper, compiled from the REAL firmware C (firmware/components/nucleo_take/nucleo_take.c) and
// run on the PC. Driven by take-check.mjs (npm run take:test).
//
// The signal is synthetic and deterministic (a square wave at a chosen amplitude, so RMS == amp
// exactly, no libm): silence is a low amplitude, speech is a high one. That makes every boundary
// assertion a hard number instead of a judgement call.
#include "nucleo_take.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>          /* host-only: the SNR check. The firmware never links libm. */

static int g_pass = 0, g_fail = 0;

static void ok(int cond, const char *what)
{
    if (cond) { g_pass++; }
    else      { g_fail++; printf("  FAIL %s\n", what); }
}

static void okf(int cond, const char *fmt, double a, double b)
{
    if (cond) { g_pass++; }
    else      { g_fail++; printf("  FAIL %s (got %.3f, want %.3f)\n", fmt, a, b); }
}

#define RATE 16000
#define BPS  2
#define FRAME 512                       /* 32 ms, exactly what record_task feeds */
#define BYTES_PER_SEC (RATE * BPS)

// Square wave of the given amplitude: RMS == amp, no libm, fully deterministic.
static void fill(int16_t *buf, int n, int amp, int *phase)
{
    for (int i = 0; i < n; i++) {
        buf[i] = (int16_t)((*phase < 4) ? amp : -amp);
        *phase = (*phase + 1) & 7;
    }
}

// ── a tiny capture harness: feed `ms` of a given amplitude, collecting closed segments ──────────
typedef struct { nucleo_take_seg_t s[256]; int n; } segs_t;

typedef struct { nucleo_take_vad_t v; long pos; int phase; segs_t *out; } cap_t;

static void cap_init(cap_t *c, segs_t *out)
{
    nucleo_take_vad_init(&c->v, RATE, BPS);
    c->pos = 0; c->phase = 0; c->out = out; out->n = 0;
}

// Feeds EXACTLY `ms` of audio: whole 32 ms frames plus a short remainder, so every boundary the
// test asserts is the boundary the caller asked for and not a rounding artefact.
static void cap_feed(cap_t *c, int amp, int ms)
{
    int16_t buf[FRAME];
    int left = ms * RATE / 1000;
    while (left > 0) {
        int n = left < FRAME ? left : FRAME;
        fill(buf, n, amp, &c->phase);
        nucleo_take_seg_t s;
        if (nucleo_take_vad_feed(&c->v, buf, n, c->pos, &s) && c->out->n < 256)
            c->out->s[c->out->n++] = s;
        c->pos += (long)n * BPS;
        left -= n;
    }
}

static void cap_flush(cap_t *c)
{
    nucleo_take_seg_t s;
    if (nucleo_take_vad_flush(&c->v, c->pos, &s) && c->out->n < 256) c->out->s[c->out->n++] = s;
}

// The timeline must have no holes and no overlaps: b1 of N is b0 of N+1, indices are monotonic,
// and the last segment ends exactly where the capture did. A hole here would silently drop audio
// from the transcript, which is the one failure a recorder must never have.
static void assert_contiguous(const segs_t *g, long end, const char *label)
{
    char msg[128];
    long expect = 0;
    for (int i = 0; i < g->n; i++) {
        snprintf(msg, sizeof msg, "%s: seg %d starts where seg %d ended", label, i, i - 1);
        ok(g->s[i].b0 == expect, msg);
        snprintf(msg, sizeof msg, "%s: seg %d index is %d", label, i, i);
        ok(g->s[i].index == i, msg);
        snprintf(msg, sizeof msg, "%s: seg %d is non-empty", label, i);
        ok(g->s[i].b1 > g->s[i].b0, msg);
        expect = g->s[i].b1;
    }
    snprintf(msg, sizeof msg, "%s: timeline reaches the end of the capture", label);
    ok(expect == end, msg);
}

#define QUIET 60
#define LOUD  6000

// ── 1. RMS ──────────────────────────────────────────────────────────────────────────────────────
static void t_rms(void)
{
    int16_t buf[FRAME]; int ph = 0;
    memset(buf, 0, sizeof buf);
    ok(nucleo_take_rms(buf, FRAME) == 0, "rms: silence is 0");
    fill(buf, FRAME, 1000, &ph);
    ok(nucleo_take_rms(buf, FRAME) == 1000, "rms: square wave amplitude == rms");
    fill(buf, FRAME, 32000, &ph);
    ok(nucleo_take_rms(buf, FRAME) == 32000, "rms: near full scale does not overflow");
    ok(nucleo_take_rms(NULL, FRAME) == 0 && nucleo_take_rms(buf, 0) == 0, "rms: rejects bad input");
}

// ── 2. the basic shape: pause / speech / pause / speech / pause ──────────────────────────────────
static void t_shape(void)
{
    segs_t g; cap_t c; cap_init(&c, &g);
    cap_feed(&c, QUIET, 2000);
    cap_feed(&c, LOUD,  3000);
    cap_feed(&c, QUIET, 1500);
    cap_feed(&c, LOUD,  4000);
    cap_feed(&c, QUIET, 1000);
    cap_flush(&c);

    ok(g.n == 5, "shape: five segments");
    if (g.n != 5) { printf("  (got %d)\n", g.n); return; }
    ok(!g.s[0].voiced && g.s[1].voiced && !g.s[2].voiced && g.s[3].voiced && !g.s[4].voiced,
       "shape: voicing alternates pause/speech/pause/speech/pause");

    // Pre-roll pulls the boundary 250 ms BEFORE the onset, post-roll pushes it 250 ms after.
    okf(g.s[1].t0 > 1.73 && g.s[1].t0 < 1.77, "shape: speech 1 starts a pre-roll early", g.s[1].t0, 1.75);
    okf(g.s[1].t1 > 5.23 && g.s[1].t1 < 5.27, "shape: speech 1 ends a post-roll late",   g.s[1].t1, 5.25);
    okf(g.s[3].t0 > 6.23 && g.s[3].t0 < 6.27, "shape: speech 2 starts a pre-roll early", g.s[3].t0, 6.25);
    okf(g.s[3].t1 > 10.73 && g.s[3].t1 < 10.77, "shape: speech 2 ends a post-roll late",  g.s[3].t1, 10.75);

    ok(g.s[1].rms > LOUD / 2 && g.s[0].rms < LOUD / 4, "shape: level tells speech from pause");
    assert_contiguous(&g, c.pos, "shape");
}

// ── 3. a click must not open a segment ──────────────────────────────────────────────────────────
static void t_click(void)
{
    segs_t g; cap_t c; cap_init(&c, &g);
    cap_feed(&c, QUIET, 2000);
    cap_feed(&c, LOUD,  96);      // 96 ms: a key press, a chair, a knock — under min_voice_ms
    cap_feed(&c, QUIET, 2000);
    cap_flush(&c);
    ok(g.n == 1 && !g.s[0].voiced, "click: a 96 ms transient never opens a segment");
    assert_contiguous(&g, c.pos, "click");
}

// ── 4. a breath must not split a phrase ─────────────────────────────────────────────────────────
static void t_breath(void)
{
    segs_t g; cap_t c; cap_init(&c, &g);
    cap_feed(&c, QUIET, 1500);
    cap_feed(&c, LOUD,  2000);
    cap_feed(&c, QUIET, 320);     // 320 ms: a breath — under min_sil_ms, must ride over
    cap_feed(&c, LOUD,  2000);
    cap_feed(&c, QUIET, 1500);
    cap_flush(&c);
    ok(g.n == 3, "breath: pause / ONE phrase / pause");
    if (g.n == 3) ok(g.s[1].voiced && g.s[1].t1 - g.s[1].t0 > 4.0,
                     "breath: the phrase survived the 320 ms gap intact");
    assert_contiguous(&g, c.pos, "breath");
}

// ── 5. the hard cut bounds one upload ───────────────────────────────────────────────────────────
static void t_hardcut(void)
{
    segs_t g; cap_t c; cap_init(&c, &g);
    c.v.max_seg_ms = 2000;                 // 2 s instead of 90 s, so the test stays quick
    cap_feed(&c, QUIET, 1000);
    cap_feed(&c, LOUD,  7000);             // uninterrupted speech: no natural boundary exists
    cap_flush(&c);
    int voiced = 0;
    for (int i = 0; i < g.n; i++) if (g.s[i].voiced) voiced++;
    ok(voiced >= 3, "hardcut: continuous speech is cut into bounded pieces");
    for (int i = 0; i < g.n; i++) {
        if (!g.s[i].voiced) continue;
        char m[96]; snprintf(m, sizeof m, "hardcut: piece %d is within the cap", i);
        ok(g.s[i].t1 - g.s[i].t0 <= 2.05, m);
    }
    assert_contiguous(&g, c.pos, "hardcut");
}

// ── 6. the gate adapts to the board's noise floor ───────────────────────────────────────────────
// The ADV's ES8311 idles far louder than the original PDM mic. A fixed threshold would call that
// hum "speech" and upload two hours of nothing.
static void t_adaptive(void)
{
    segs_t g; cap_t c; cap_init(&c, &g);
    cap_feed(&c, 1500, 2500);              // noisy room: well above the 380 absolute guard
    cap_feed(&c, 9000, 3000);
    cap_feed(&c, 1500, 2000);
    cap_flush(&c);
    ok(g.n == 3, "adaptive: a 1500-RMS hum is still recognised as a pause");
    if (g.n == 3) ok(!g.s[0].voiced && g.s[1].voiced && !g.s[2].voiced,
                     "adaptive: only the 9000-RMS speech opens the gate");
    assert_contiguous(&g, c.pos, "adaptive");
}

// ── 7. a take that starts mid-sentence must not stay deaf ───────────────────────────────────────
// The floor seeds from the first 800 ms. If that is speech, the gate would be set impossibly high;
// the downward fast-attack has to recover it.
static void t_hotstart(void)
{
    segs_t g; cap_t c; cap_init(&c, &g);
    cap_feed(&c, LOUD,  2000);             // recording starts with someone already talking
    cap_feed(&c, QUIET, 2000);
    cap_feed(&c, LOUD,  2500);
    cap_feed(&c, QUIET, 1500);
    cap_flush(&c);
    int voiced = 0;
    for (int i = 0; i < g.n; i++) if (g.s[i].voiced) voiced++;
    ok(voiced >= 1, "hotstart: the gate recovers and still finds the later speech");
    ok(g.n >= 3, "hotstart: the take is segmented, not one undifferentiated block");
    assert_contiguous(&g, c.pos, "hotstart");
}

// ── 7b. a steady tone that STARTS mid-take must not lock the gate in "voice" forever ─────────────
// Regression for the hard-cut floor bump. The adaptive floor only climbs while the gate is CLOSED,
// so a hum that begins AFTER the 800 ms seed window (a fan/AC kicking in) used to hold st=1 for the
// rest of the take — every max_seg piece voiced, hours of drone uploaded for Whisper to hallucinate
// over. The bump pulls the floor toward the sustained level on each hard cut, so the gate closes and
// the tone is recognised as the new floor.
static void t_hum_onset(void)
{
    segs_t g; cap_t c; cap_init(&c, &g);
    c.v.max_seg_ms = 2000;                 // 2 s hard cuts, so the test stays quick
    cap_feed(&c, QUIET, 1000);             // seed the floor low (quiet room)
    cap_feed(&c, LOUD, 30000);             // then 30 s of unbroken tone: ~15 hard-cut windows
    cap_feed(&c, QUIET, 1000);
    cap_flush(&c);
    int voiced = 0, silent = 0;
    for (int i = 0; i < g.n; i++) (g.s[i].voiced ? voiced++ : silent++);
    if (voiced > 5) printf("  (hum-onset voiced=%d silent=%d of %d)\n", voiced, silent, g.n);
    ok(silent >= 1, "hum-onset: a sustained tone eventually reads as non-voice, not endless speech");
    ok(voiced <= 5, "hum-onset: only the onset is uploaded, not the whole 30 s drone");
    assert_contiguous(&g, c.pos, "hum-onset");
}

// ── 8. an all-silence take ──────────────────────────────────────────────────────────────────────
static void t_allsilence(void)
{
    segs_t g; cap_t c; cap_init(&c, &g);
    cap_feed(&c, QUIET, 5000);
    cap_flush(&c);
    ok(g.n == 1 && !g.s[0].voiced, "silence: an empty room is one silent segment, never uploaded");
    assert_contiguous(&g, c.pos, "silence");
}

// ── 9. WAV recovery ─────────────────────────────────────────────────────────────────────────────
static void t_wav(void)
{
    uint8_t h[44];
    nucleo_take_wav_hdr(h, 32000, 16000, 1, 16);
    ok(!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4) && !memcmp(h + 36, "data", 4),
       "wav: RIFF/WAVE/data magic in place");
    uint32_t dl; memcpy(&dl, h + 40, 4);
    ok(dl == 32000, "wav: data_len round-trips little-endian");
    uint32_t br; memcpy(&br, h + 28, 4);
    ok(br == 32000, "wav: byte rate is 32000 for 16k mono 16-bit");

    // The case this whole component exists for: a take killed at minute 118.
    ok(nucleo_take_wav_datalen(0, 44 + 230000000L, 44) == 230000000L,
       "wav: a header claiming 0 is overruled by the bytes on disk");
    ok(nucleo_take_wav_datalen(999999999, 44 + 1000, 44) == 1000,
       "wav: a header claiming more than the file holds is clamped");
    ok(nucleo_take_wav_datalen(500, 44 + 1000, 44) == 500,
       "wav: an honest header is believed");
    ok(nucleo_take_wav_datalen(0, 20, 44) == 0, "wav: a file shorter than a header holds nothing");
}

// ── 10. journal ─────────────────────────────────────────────────────────────────────────────────
// Writes the real take from test 2 to build/take-journal.ndjson; take-check.mjs then parses it as
// JSON and re-checks the timeline from the outside, the way the browser will.
static void t_journal(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) { g_fail++; printf("  FAIL journal: cannot open %s\n", path); return; }

    segs_t g; cap_t c; cap_init(&c, &g);
    ok(nucleo_take_journal_open(f, RATE, 1, 16, "wav", 44, 1755782400L) > 0, "journal: open line written");
    cap_feed(&c, QUIET, 1500);
    cap_feed(&c, LOUD,  2500);
    cap_feed(&c, QUIET, 1200);
    cap_feed(&c, LOUD,  2500);
    cap_feed(&c, QUIET, 1200);
    cap_flush(&c);
    for (int i = 0; i < g.n; i++) ok(nucleo_take_journal_seg(f, &g.s[i]) > 0, "journal: seg line written");
    ok(nucleo_take_journal_end(f, (double)c.pos / BYTES_PER_SEC, c.pos, g.n) > 0, "journal: end line written");
    fclose(f);

    ok(nucleo_take_journal_seg(NULL, &g.s[0]) < 0, "journal: a NULL file is refused, not crashed");
}

// ── 11. one-shot header repair ──────────────────────────────────────────────────────────────────
// The scenario the whole component exists for: a take killed at minute 118. The header still says
// data_len 0, so no decoder will open it — until someone opens the take again and we fix it.
static void write_wav(const char *path, uint32_t claimed, int payload)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint8_t h[44];
    nucleo_take_wav_hdr(h, claimed, RATE, 1, 16);
    fwrite(h, 1, sizeof h, f);
    for (int i = 0; i < payload; i++) fputc(0x11, f);
    fclose(f);
}

static uint32_t read_datalen(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0xFFFFFFFFu;
    uint8_t h[44]; uint32_t dl = 0xFFFFFFFFu;
    if (fread(h, 1, sizeof h, f) == sizeof h) memcpy(&dl, h + 40, 4);
    fclose(f);
    return dl;
}

static void t_repair(const char *path)
{
    write_wav(path, 0, 1000);                      // interrupted: header never finalised
    ok(nucleo_take_wav_repair(path) == 1, "repair: an interrupted take is repaired");
    ok(read_datalen(path) == 1000, "repair: data_len now matches the bytes on disk");
    ok(nucleo_take_wav_repair(path) == 0, "repair: running it again is a no-op");

    write_wav(path, 999999, 1000);                 // stale header claiming more than exists
    ok(nucleo_take_wav_repair(path) == 1, "repair: an over-claiming header is clamped");
    ok(read_datalen(path) == 1000, "repair: clamped to the real length");

    write_wav(path, 1000, 1000);                   // already honest
    ok(nucleo_take_wav_repair(path) == 0, "repair: an honest header is left alone");

    FILE *f = fopen(path, "wb");                   // not a WAV at all
    if (f) { fwrite("not a wav file at all........................", 1, 44, f); fclose(f); }
    ok(nucleo_take_wav_repair(path) < 0, "repair: a non-WAV file is refused, not rewritten");
    ok(nucleo_take_wav_repair("build/does-not-exist.wav") < 0, "repair: a missing file is refused");
    ok(nucleo_take_wav_repair(NULL) < 0, "repair: a NULL path is refused");
    remove(path);
}


// ── 12. IMA ADPCM 4:1 ───────────────────────────────────────────────────────────────────────────
// This codec exists because MP3 does not fit: shine's encoder context is one ~96 KB malloc, bigger
// than this chip's largest free block. So ADPCM has to earn its place on two axes — the state must
// stay tiny, and the audio must survive well enough for an ASR model. Both are asserted here, plus
// the property the uploader depends on: the encoded size is an exact function of the sample count,
// known before a single byte is encoded.
static const int8_t  D_IDX[16]  = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static const int16_t D_STEP[89] = {
        7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
       19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
       50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
      130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
      337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
      876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
     2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
     5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

// Reference decoder, written against the IMA spec rather than against our encoder. The point is to
// prove the bytes are decodable by somebody else — which is exactly what the endpoint will do.
static int adpcm_decode_block(const uint8_t *in, int16_t *out)
{
    int pred = (int16_t)(in[0] | (in[1] << 8));
    int index = in[2];
    out[0] = (int16_t)pred;
    for (int i = 1; i < NUCLEO_TAKE_ADPCM_SPB; i++) {
        int nib = i - 1;
        int code = (nib & 1) ? (in[4 + (nib >> 1)] >> 4) & 0x0F : in[4 + (nib >> 1)] & 0x0F;
        int step = D_STEP[index];
        int vpdiff = step >> 3;
        if (code & 4) vpdiff += step;
        if (code & 2) vpdiff += step >> 1;
        if (code & 1) vpdiff += step >> 2;
        pred += (code & 8) ? -vpdiff : vpdiff;
        if (pred >  32767) pred =  32767;
        if (pred < -32768) pred = -32768;
        index += D_IDX[code];
        if (index < 0)  index = 0;
        if (index > 88) index = 88;
        out[i] = (int16_t)pred;
    }
    return NUCLEO_TAKE_ADPCM_SPB;
}

static uint32_t rd_le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

#define TEST_SAMPLES (NUCLEO_TAKE_ADPCM_SPB * 4 + 137)   /* 4 full blocks + a short padded one */

static void t_adpcm(void)
{
    ok(sizeof(nucleo_take_adpcm_t) <= 16, "adpcm: encoder state stays under 16 bytes");

    // Exact-size contract — the uploader must state Content-Length before encoding anything.
    ok(nucleo_take_adpcm_size(0) == 0, "adpcm: nothing encodes to nothing");
    ok(nucleo_take_adpcm_size(1) == NUCLEO_TAKE_ADPCM_BLOCK, "adpcm: one sample still costs one block");
    ok(nucleo_take_adpcm_size(NUCLEO_TAKE_ADPCM_SPB) == NUCLEO_TAKE_ADPCM_BLOCK, "adpcm: a full block is one block");
    ok(nucleo_take_adpcm_size(NUCLEO_TAKE_ADPCM_SPB + 1) == 2 * NUCLEO_TAKE_ADPCM_BLOCK, "adpcm: one over spills into a second");

    uint8_t h[NUCLEO_TAKE_ADPCM_HDR];
    nucleo_take_adpcm_hdr(h, TEST_SAMPLES, RATE);
    ok(!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4) && !memcmp(h + 12, "fmt ", 4) &&
       !memcmp(h + 40, "fact", 4) && !memcmp(h + 52, "data", 4), "adpcm: RIFF/fmt/fact/data chunks in place");
    ok(rd_le16(h + 20) == 0x0011, "adpcm: format tag is WAVE_FORMAT_IMA_ADPCM");
    ok(rd_le16(h + 22) == 1 && rd_le32(h + 24) == RATE, "adpcm: mono at the capture rate");
    ok(rd_le32(h + 16) == 20 && rd_le16(h + 36) == 2, "adpcm: fmt chunk is the 20-byte ADPCM form");
    ok(rd_le16(h + 32) == NUCLEO_TAKE_ADPCM_BLOCK && rd_le16(h + 34) == 4, "adpcm: block align 256, 4 bits");
    ok(rd_le16(h + 38) == NUCLEO_TAKE_ADPCM_SPB, "adpcm: samples-per-block declared");
    ok(rd_le32(h + 48) == (uint32_t)TEST_SAMPLES, "adpcm: fact chunk carries the TRUE sample count");
    ok(rd_le32(h + 56) == (uint32_t)nucleo_take_adpcm_size(TEST_SAMPLES), "adpcm: data size matches the size contract");
    ok(rd_le32(h + 4) == 52 + rd_le32(h + 56), "adpcm: RIFF size covers the whole file");

    // A signal shaped the way speech is: a mid-frequency carrier under a slow envelope. A pure
    // square wave would be a worst case that tells us nothing useful about voice.
    static int16_t src[TEST_SAMPLES];
    for (int i = 0; i < TEST_SAMPLES; i++) {
        int tri = ((i % 40) < 20) ? (i % 40) : (40 - (i % 40));      /* 400 Hz triangle at 16 kHz */
        int env = 200 + ((i / 97) % 60) * 20;                        /* slowly breathing amplitude */
        src[i] = (int16_t)((tri - 10) * env / 6);
    }

    static uint8_t enc[NUCLEO_TAKE_ADPCM_BLOCK * 8];
    static int16_t dec[NUCLEO_TAKE_ADPCM_SPB * 8];
    nucleo_take_adpcm_t st; nucleo_take_adpcm_init(&st);
    int blocks = 0;
    for (long off = 0; off < TEST_SAMPLES; off += NUCLEO_TAKE_ADPCM_SPB) {
        int n = (int)(TEST_SAMPLES - off);
        if (n > NUCLEO_TAKE_ADPCM_SPB) n = NUCLEO_TAKE_ADPCM_SPB;
        int w = nucleo_take_adpcm_block(&st, src + off, n, enc + blocks * NUCLEO_TAKE_ADPCM_BLOCK);
        ok(w == NUCLEO_TAKE_ADPCM_BLOCK, "adpcm: every block is exactly 256 bytes");
        blocks++;
    }
    ok((long)blocks * NUCLEO_TAKE_ADPCM_BLOCK == nucleo_take_adpcm_size(TEST_SAMPLES),
       "adpcm: the blocks emitted match the size promised in the header");

    for (int b = 0; b < blocks; b++)
        adpcm_decode_block(enc + b * NUCLEO_TAKE_ADPCM_BLOCK, dec + b * NUCLEO_TAKE_ADPCM_SPB);

    // Every block starts on the exact original sample: the header stores it verbatim, which is what
    // makes blocks independently decodable and a dropped block harmless to the rest.
    for (int b = 0; b < blocks; b++) {
        char m[96]; snprintf(m, sizeof m, "adpcm: block %d starts on an exact sample", b);
        ok(dec[b * NUCLEO_TAKE_ADPCM_SPB] == src[b * NUCLEO_TAKE_ADPCM_SPB], m);
    }

    double sig = 0, noise = 0;
    for (int i = 0; i < TEST_SAMPLES; i++) {
        double d = (double)dec[i] - (double)src[i];
        sig += (double)src[i] * src[i];
        noise += d * d;
    }
    double snr = (noise > 0) ? 10.0 * log10(sig / noise) : 99.0;
    okf(snr > 20.0, "adpcm: round-trip SNR is good enough for ASR", snr, 20.0);
    printf("  adpcm: %d samples -> %d blocks (%.2f:1), SNR %.1f dB, state %d bytes\n",
           TEST_SAMPLES, blocks, (double)(TEST_SAMPLES * 2) / (blocks * NUCLEO_TAKE_ADPCM_BLOCK),
           snr, (int)sizeof(nucleo_take_adpcm_t));

    // Padding the final block must not disturb the real samples in front of it.
    int last0 = (blocks - 1) * NUCLEO_TAKE_ADPCM_SPB;
    int realn = TEST_SAMPLES - last0;
    double lnoise = 0, lsig = 0;
    for (int i = 0; i < realn; i++) {
        double d = (double)dec[last0 + i] - (double)src[last0 + i];
        lsig += (double)src[last0 + i] * src[last0 + i]; lnoise += d * d;
    }
    ok(lnoise <= 0 || 10.0 * log10(lsig / lnoise) > 20.0, "adpcm: the padded final block keeps its real samples clean");

    nucleo_take_adpcm_t bad; nucleo_take_adpcm_init(&bad);
    uint8_t sink[NUCLEO_TAKE_ADPCM_BLOCK];
    ok(nucleo_take_adpcm_block(&bad, src, 0, sink) < 0, "adpcm: an empty block is refused");
    ok(nucleo_take_adpcm_block(&bad, src, NUCLEO_TAKE_ADPCM_SPB + 1, sink) < 0, "adpcm: an oversized block is refused");
    ok(nucleo_take_adpcm_block(NULL, src, 10, sink) < 0, "adpcm: a NULL state is refused");
}


// ── 13. reading the journal back: batches, resume, flattening ───────────────────────────────────
// This is where the design either pays off or does not. A transcriber must be able to take only the
// voiced audio, pick up exactly where a previous attempt died, and rebuild the flat transcript it
// already paid for — all without ever holding the take in RAM.
static void write_journal(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    segs_t g; cap_t c; cap_init(&c, &g);
    nucleo_take_journal_open(f, RATE, 1, 16, "wav", 44, 1755782400L);
    cap_feed(&c, QUIET, 1000);
    cap_feed(&c, LOUD,  3000);
    cap_feed(&c, QUIET, 1500);
    cap_feed(&c, LOUD,  2000);
    cap_feed(&c, QUIET, 1500);
    cap_feed(&c, LOUD,  4000);
    cap_feed(&c, QUIET, 1000);
    cap_flush(&c);
    for (int i = 0; i < g.n; i++) nucleo_take_journal_seg(f, &g.s[i]);
    nucleo_take_journal_end(f, (double)c.pos / BYTES_PER_SEC, c.pos, g.n);
    fclose(f);
}

static void t_reader(const char *dir_hint)
{
    char jp[300], tp[300], fp2[300];
    snprintf(jp,  sizeof jp,  "%s.j.ndjson",  dir_hint);
    snprintf(tp,  sizeof tp,  "%s.tx.ndjson", dir_hint);
    snprintf(fp2, sizeof fp2, "%s.flat.txt",  dir_hint);
    write_journal(jp);

    nucleo_take_todo_t *todo = (nucleo_take_todo_t *)calloc(1, sizeof *todo);
    nucleo_take_batch_t b;
    ok(todo != NULL, "reader: todo bitmap allocates");
    if (!todo) return;

    // One generous batch must sweep up every voiced segment and nothing else.
    FILE *j = fopen(jp, "rb");
    ok(j != NULL, "reader: journal opens");
    if (!j) { free(todo); return; }
    ok(nucleo_take_next_batch(j, todo, 1 << 30, &b), "reader: a generous budget yields a batch");
    ok(b.n == 3, "reader: exactly the three voiced segments are batched");
    long voiced_bytes = b.bytes;
    ok(voiced_bytes > 0, "reader: the batch carries audio");
    // 9 s of voice out of 14 s recorded, plus the pre/post-roll: well under the whole take.
    ok(voiced_bytes < 14L * BYTES_PER_SEC, "reader: the pauses were left out of the batch");
    ok(!nucleo_take_next_batch(j, todo, 1 << 30, &b), "reader: nothing is left after the sweep");
    fclose(j);

    // A tight budget must split, and the splits must tile the same audio exactly once.
    j = fopen(jp, "rb");
    long total = 0; int batches = 0, segs_seen = 0, last_idx = -1;
    while (nucleo_take_next_batch(j, todo, 2L * BYTES_PER_SEC, &b)) {
        ok(b.n >= 1, "reader: a batch is never empty");
        ok(b.first > last_idx, "reader: batches move forward through the journal");
        last_idx = b.last;
        total += b.bytes; segs_seen += b.n; batches++;
        if (batches > 10) break;
    }
    fclose(j);
    ok(batches >= 2, "reader: a tight budget really does split the work");
    ok(segs_seen == 3 && total == voiced_bytes, "reader: the splits tile the same audio, once each");

    // Resume: record the first batch as transcribed, then confirm it is skipped.
    FILE *tx = fopen(tp, "wb");
    ok(tx != NULL, "reader: transcript journal opens");
    if (!tx) { free(todo); return; }
    j = fopen(jp, "rb");
    nucleo_take_next_batch(j, todo, 2L * BYTES_PER_SEC, &b);
    fclose(j);
    // Includes a raw 0x01: below 0x20 and not \n/\r/\t, so it forces the \u%04x writer branch and
    // flatten's 4-hex-digit \u reader — the path a plain "quotes+tabs" string never exercises.
    const char *TRICKY = "He said \"ciao\" \\ then\na new line\tand a tab\x01""done.";
    ok(nucleo_take_journal_txt(tx, b.first, b.last, "it", "groq/whisper-large-v3", TRICKY) > 0,
       "reader: a transcript entry is written");
    fclose(tx);

    tx = fopen(tp, "rb");
    ok(nucleo_take_scan_done(tx, todo) == 1, "reader: the resume scan finds the one entry");
    fclose(tx);
    ok(todo->overflow == 0, "reader: nothing overflowed the bitmap");

    j = fopen(jp, "rb");
    long left = 0; int left_segs = 0;
    while (nucleo_take_next_batch(j, todo, 1 << 30, &b)) { left += b.bytes; left_segs += b.n; }
    fclose(j);
    ok(left_segs < 3 && left < voiced_bytes, "reader: a resumed run skips what was already done");
    ok(left_segs > 0, "reader: ...but still has the rest to do");

    // Flattening must reproduce the text byte for byte — quotes, backslashes and control characters
    // all survive the round trip, because a transcript is arbitrary text from a microphone.
    tx = fopen(tp, "rb");
    FILE *flat = fopen(fp2, "wb");
    long n = nucleo_take_flatten(tx, flat);
    fclose(flat); fclose(tx);
    ok(n == (long)strlen(TRICKY), "reader: flatten writes exactly the original length");
    FILE *rd = fopen(fp2, "rb");
    char got[256] = {0};
    if (rd) { size_t r = fread(got, 1, sizeof got - 1, rd); got[r] = 0; fclose(rd); }
    ok(!strcmp(got, TRICKY), "reader: quotes, backslashes, newlines and tabs survive the round trip");

    // Two entries join with exactly one space.
    tx = fopen(tp, "ab");
    nucleo_take_journal_txt(tx, 90, 90, "en", "browser/whisper-wasm", "second");
    fclose(tx);
    tx = fopen(tp, "rb"); flat = fopen(fp2, "wb");
    n = nucleo_take_flatten(tx, flat);
    fclose(flat); fclose(tx);
    ok(n == (long)strlen(TRICKY) + 1 + 6, "reader: entries are joined with a single space");

    // Provenance is per entry: a take may legitimately be half one engine and half another.
    tx = fopen(tp, "rb");
    char line[512]; int engines = 0;
    while (fgets(line, sizeof line, tx)) {
        if (strstr(line, "whisper-large-v3")) engines |= 1;
        if (strstr(line, "whisper-wasm"))     engines |= 2;
    }
    fclose(tx);
    ok(engines == 3, "reader: each entry records which engine produced it");

    ok(nucleo_take_scan_done(NULL, todo) == 0, "reader: no prior attempt means nothing is done");
    ok(!nucleo_take_next_batch(NULL, todo, 1000, &b), "reader: a missing journal yields no batch");

    // A torn LAST line (power cut mid-write, no trailing newline) must cost only that line: the C
    // parser skips it and still delivers every intact segment before it.
    {
        FILE *t = fopen(jp, "wb");
        fprintf(t, "{\"t\":\"open\",\"v\":1,\"rate\":16000,\"ch\":1,\"bits\":16,\"fmt\":\"wav\",\"hdr\":44,\"start\":0}\n");
        fprintf(t, "{\"t\":\"seg\",\"i\":0,\"t0\":0.000,\"t1\":1.000,\"b0\":0,\"b1\":32000,\"rms\":900,\"voiced\":true}\n");
        fprintf(t, "{\"t\":\"seg\",\"i\":1,\"t0\":1.000,\"t1\":2.000,\"b0\":32000,\"b1\":64000,\"rms\":900,\"voi");   // torn mid-field
        fclose(t);
        nucleo_take_todo_t td; memset(&td, 0, sizeof td);
        FILE *jt = fopen(jp, "rb");
        long got_bytes = 0;
        while (jt && nucleo_take_next_batch(jt, &td, 1L << 30, &b)) got_bytes += b.bytes;
        if (jt) fclose(jt);
        ok(got_bytes == 32000, "reader: a torn last seg line is skipped, the intact one survives");
    }

    // A corrupt digit run in "j" must not hang scan_done (saturating clamp, not signed overflow).
    {
        FILE *t = fopen(tp, "wb");
        fprintf(t, "{\"t\":\"txt\",\"i\":0,\"j\":99999999999999999999,\"lang\":\"en\",\"engine\":\"x\",\"text\":\"hi\"}\n");
        fclose(t);
        nucleo_take_todo_t td; memset(&td, 0, sizeof td);
        FILE *jt = fopen(tp, "rb");
        int ranges = nucleo_take_scan_done(jt, &td);
        if (jt) fclose(jt);
        ok(ranges == 1 && td.overflow > 0, "reader: a corrupt 'j' clamps to overflow instead of spinning");
    }

    free(todo);
    remove(jp); remove(tp); remove(fp2);
}


// ── 14. offline language ID ─────────────────────────────────────────────────────────────────────
// The second opinion on Whisper's language guess. What matters is not only getting the five OS
// languages right, but ABSTAINING when the evidence is thin — a wrong lock is worse than no lock,
// because it drives the transcript language, the summary language and the TTS voice for the whole take.
static void lid_case(const char *text, const char *want, int minconf, const char *what)
{
    char got[8];
    int conf = nucleo_take_lid(text, got, sizeof got);
    char m[160];
    snprintf(m, sizeof m, "lid: %s", what);
    if (want) {
        if (conf >= minconf && !strcmp(got, want)) g_pass++;
        else { g_fail++; printf("  FAIL %s (got \"%s\" conf %d, want \"%s\" >=%d)\n", m, got, conf, want, minconf); }
    } else {
        if (conf == 0) g_pass++;
        else { g_fail++; printf("  FAIL %s (expected an abstention, got \"%s\" conf %d)\n", m, got, conf); }
    }
}

static void t_lid(void)
{
    lid_case("Allora, quello che volevo dire e che non siamo ancora pronti per la riunione, "
             "anche perche del materiale non e stato preparato come avevamo deciso.",
             "it", 40, "an Italian meeting note");
    lid_case("So what I wanted to say is that we are not ready for the meeting yet, "
             "because some of the material has not been prepared the way we agreed.",
             "en", 40, "the same note in English");
    lid_case("Lo que queria decir es que todavia no estamos listos para la reunion, "
             "porque parte del material no se ha preparado como habiamos decidido.",
             "es", 40, "the same note in Spanish");
    lid_case("Ce que je voulais dire, c'est que nous ne sommes pas encore prets pour la reunion, "
             "parce qu'une partie des documents n'a pas ete preparee comme nous avions decide.",
             "fr", 40, "the same note in French");
    lid_case("Was ich sagen wollte ist, dass wir noch nicht bereit fur das Treffen sind, "
             "weil ein Teil der Unterlagen nicht so vorbereitet wurde wie wir es beschlossen haben.",
             "de", 40, "the same note in German");

    // Accents must not change the verdict: the folder strips them, so a transcript with or without
    // them scores identically. Whisper output has them; a keyboard-typed note often does not.
    char a[8], b[8];
    int ca = nucleo_take_lid("Non e vero che siamo piu pronti, perche il materiale della riunione non c'e ancora", a, sizeof a);
    int cb = nucleo_take_lid("Non è vero che siamo più pronti, perché il materiale della riunione non c'è ancora", b, sizeof b);
    ok(!strcmp(a, "it") && !strcmp(b, "it"), "lid: accented and unaccented Italian agree");
    ok(ca == cb, "lid: accents do not move the confidence");

    // Abstentions. Each of these would be a wrong lock if we guessed.
    lid_case("Ciao.", NULL, 0, "five words are not evidence");
    lid_case("", NULL, 0, "empty text abstains");
    lid_case("Nucleo Cardputer ESP32 microSD firmware OTA WiFi Bluetooth JSON HTTP TLS mbedTLS heap",
             NULL, 0, "a pile of proper nouns abstains");

    char sink[8];
    ok(nucleo_take_lid(NULL, sink, sizeof sink) == 0, "lid: NULL text abstains");
    ok(nucleo_take_lid("whatever", NULL, 0) == 0, "lid: no output buffer abstains");

    // Real transcripts are messy: filler, false starts, numbers. The verdict must survive that.
    lid_case("Eh, allora, dicevo... no, aspetta, 3 4 5, dunque il punto e che non abbiamo ancora "
             "deciso niente e quindi come facciamo per la settimana prossima, non lo so.",
             "it", 30, "Italian with filler, false starts and numbers");

    char code[8];
    int conf = nucleo_take_lid("The quick brown fox jumps over the lazy dog and then the dog was not "
                               "amused because that is what dogs are like when they have been woken",
                               code, sizeof code);
    printf("  lid: five languages, abstains under 8 tokens; sample English conf %d\n", conf);
}

// ── 15. flatten emits in segment order, not file order ──────────────────────────────────────────
// Regression: a batch that failed mid-take and was transcribed on a LATER resume is appended at the
// end of the tx journal. Flattening in file order would then read "…39, 41…80, 40" — chronologically
// wrong. Flatten must order by ascending first-segment index. Also: a batch journalled twice by a
// retry must appear once.
static void t_flatten_order(void)
{
    const char *tp = "build/take-flatorder.tx.ndjson";
    const char *op = "build/take-flatorder.out";
    FILE *tx = fopen(tp, "wb");
    ok(tx != NULL, "flatten-order: journal opens");
    if (!tx) return;
    nucleo_take_journal_txt(tx, 5, 5, "en", "x", "ccc");     // written out of order (a late resume)
    nucleo_take_journal_txt(tx, 0, 1, "en", "x", "aaa");
    nucleo_take_journal_txt(tx, 2, 2, "en", "x", "bbb");
    nucleo_take_journal_txt(tx, 0, 1, "en", "x", "aaa");     // duplicate of the [0..1] batch (a retry)
    fclose(tx);

    tx = fopen(tp, "rb");
    FILE *out = fopen(op, "wb");
    long n = nucleo_take_flatten(tx, out);
    fclose(out); if (tx) fclose(tx);

    char got[64] = {0};
    FILE *rd = fopen(op, "rb");
    if (rd) { size_t r = fread(got, 1, sizeof got - 1, rd); got[r] = 0; fclose(rd); }
    ok(!strcmp(got, "aaa bbb ccc"), "flatten-order: entries come out in ascending segment order, deduped");
    ok(n == 11, "flatten-order: length is the three joined texts, the duplicate dropped");
    remove(tp); remove(op);
}

int main(int argc, char **argv)
{
    const char *jpath = argc > 1 ? argv[1] : "build/take-journal.ndjson";
    const char *wpath = argc > 2 ? argv[2] : "build/take-repair.wav";
    printf("nucleo_take host gate\n");
    t_rms();
    t_shape();
    t_click();
    t_breath();
    t_hardcut();
    t_adaptive();
    t_hotstart();
    t_hum_onset();
    t_allsilence();
    t_wav();
    t_journal(jpath);
    t_repair(wpath);
    t_adpcm();
    t_reader(wpath);
    t_flatten_order();
    t_lid();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
