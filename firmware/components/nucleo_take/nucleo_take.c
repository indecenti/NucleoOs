// nucleo_take — VAD segmenter + append-only journal for a recording. See include/nucleo_take.h.
// No ESP-IDF here on purpose: tools/anima-host/take-check.mjs compiles THIS file on the PC and
// asserts the segmenter against synthetic speech/silence before it ever meets a real mic.
#include "nucleo_take.h"
#include <string.h>
#include <stdlib.h>
#include <limits.h>

// Gate thresholds are derived from the tracked floor, never hardcoded: the original Cardputer's
// PDM mic and the ADV's ES8311 sit at very different noise floors. Multipliers are /10.
#define TK_OPEN_MULT   30      // 3.0x floor -> open
#define TK_CLOSE_MULT  18      // 1.8x floor -> close (hysteresis, so a breath doesn't chop a phrase)
#define TK_OPEN_MIN    380     // absolute guards: a dead-silent room must not make the gate paranoid
#define TK_CLOSE_MIN   230
#define TK_FLOOR_MAX   4000    // a loud constant hum must not blind the gate entirely
#define TK_SEED_MS     800     // bootstrap: take the minimum seen, then track slowly
#define TK_FLOOR_SHIFT 4       // EMA: floor += (rms - floor) >> 4

// Integer sqrt (Newton, 32-bit result) — the audio path must not pull in libm.
static uint32_t isqrt64(uint64_t v)
{
    if (v == 0) return 0;
    uint64_t x = v, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + v / x) / 2; }
    return (uint32_t)x;
}

int nucleo_take_rms(const int16_t *pcm, int nsamples)
{
    if (!pcm || nsamples <= 0) return 0;
    uint64_t acc = 0;
    for (int i = 0; i < nsamples; i++) { int64_t s = pcm[i]; acc += (uint64_t)(s * s); }
    return (int)isqrt64(acc / (uint64_t)nsamples);
}

void nucleo_take_vad_init(nucleo_take_vad_t *v, int rate_hz, int bytes_per_sample)
{
    memset(v, 0, sizeof *v);
    v->rate_hz          = rate_hz > 0 ? rate_hz : 16000;
    v->bytes_per_sample = bytes_per_sample > 0 ? bytes_per_sample : 2;
    v->min_voice_ms     = 200;    // shorter than this is a click, a key press, a chair
    v->min_sil_ms       = 600;    // longer than a breath, shorter than a real pause
    v->max_seg_ms       = 90000;  // hard cut: bounds one upload and one transcript reply
    v->roll_ms          = 250;    // keep the attack and the tail — the file is already the buffer
    v->seed_ms          = TK_SEED_MS;
    v->floor_rms        = 0;
}

static long tk_ms_to_bytes(const nucleo_take_vad_t *v, int ms)
{
    return (long)((int64_t)ms * v->rate_hz * v->bytes_per_sample / 1000);
}

static double tk_b2t(const nucleo_take_vad_t *v, long b)
{
    return (double)b / ((double)v->rate_hz * (double)v->bytes_per_sample);
}

// Close [seg_b0, b_cut) with the given voicing and re-open the next segment at b_cut.
static void tk_emit(nucleo_take_vad_t *v, long b_cut, bool voiced, nucleo_take_seg_t *out)
{
    out->index  = v->index++;
    out->b0     = v->seg_b0;
    out->b1     = b_cut;
    out->t0     = tk_b2t(v, out->b0);
    out->t1     = tk_b2t(v, out->b1);
    out->rms    = v->acc_ms > 0 ? (int)(v->acc_rms / v->acc_ms) : 0;
    out->voiced = voiced;
    v->seg_b0   = b_cut;
    v->acc_rms  = 0;
    v->acc_ms   = 0;
    v->run_ms   = 0;
}

bool nucleo_take_vad_feed(nucleo_take_vad_t *v, const int16_t *pcm, int nsamples, long b0,
                          nucleo_take_seg_t *out)
{
    if (!v || !pcm || !out || nsamples <= 0) return false;

    const int  rms   = nucleo_take_rms(pcm, nsamples);
    int        ms    = (int)((int64_t)nsamples * 1000 / v->rate_hz);
    if (ms <= 0) ms = 1;
    const long b_end = b0 + (long)nsamples * v->bytes_per_sample;

    // Noise floor: seeded from the quietest of the first TK_SEED_MS, then tracked ONLY while the
    // gate is closed — tracking during speech would let the floor climb until the gate went deaf.
    if (v->seed_ms > 0) {
        v->floor_rms = (v->floor_rms == 0 || rms < v->floor_rms) ? rms : v->floor_rms;
        v->seed_ms  -= ms;
    } else if (rms < v->floor_rms) {
        // Fast attack DOWNWARD in any state. A take that starts mid-sentence seeds the floor from
        // speech, which would leave the gate deaf; letting it fall whenever something quieter shows
        // up recovers within a second or so. Erring quiet only ever makes the gate more sensitive.
        v->floor_rms -= (v->floor_rms - rms) >> 2;
    } else if (v->st == 0 && rms < v->floor_rms * 2) {
        v->floor_rms += (rms - v->floor_rms) >> TK_FLOOR_SHIFT;
    }
    if (v->floor_rms > TK_FLOOR_MAX) v->floor_rms = TK_FLOOR_MAX;
    if (v->floor_rms < 0)            v->floor_rms = 0;

    int g_open  = v->floor_rms * TK_OPEN_MULT  / 10;
    int g_close = v->floor_rms * TK_CLOSE_MULT / 10;
    if (g_open  < TK_OPEN_MIN)  g_open  = TK_OPEN_MIN;
    if (g_close < TK_CLOSE_MIN) g_close = TK_CLOSE_MIN;

    // Level accounting: a frame only counts toward the segment it actually belongs to, so a pause
    // never inherits the level of the speech that ended it.
    if (v->st == 1 || rms < g_open) { v->acc_rms += (int64_t)rms * ms; v->acc_ms += ms; }

    bool closed = false;

    if (v->st == 0) {                                   // ── in a pause
        if (rms >= g_open) {
            if (v->run_ms == 0) v->run_b0 = b0;
            v->run_ms += ms;
            if (v->run_ms >= v->min_voice_ms) {          // voice confirmed -> the pause ends
                long cut = v->run_b0 - tk_ms_to_bytes(v, v->roll_ms);   // pre-roll: keep the attack
                if (cut < v->seg_b0) cut = v->seg_b0;
                if (cut > v->seg_b0) { tk_emit(v, cut, false, out); closed = true; }
                else                 { v->seg_b0 = cut; }
                v->st      = 1;
                v->acc_rms = (int64_t)rms * ms;          // the new segment starts at this frame's level
                v->acc_ms  = ms;
                v->run_ms  = 0;
            }
        } else {
            v->run_ms = 0;
        }
        return closed;
    }

    // ── in speech
    if (rms < g_close) {
        if (v->run_ms == 0) v->run_b0 = b0;
        v->run_ms += ms;
        if (v->run_ms >= v->min_sil_ms) {                // pause confirmed -> the phrase ends
            long cut = v->run_b0 + tk_ms_to_bytes(v, v->roll_ms);       // post-roll: keep the tail
            if (cut > b_end)     cut = b_end;
            if (cut < v->seg_b0) cut = v->seg_b0;
            tk_emit(v, cut, true, out);
            v->st = 0;
            return true;
        }
    } else {
        v->run_ms = 0;
    }

    // Hard cut mid-speech: bounds one upload. Only reached when no natural boundary showed up.
    if (b_end - v->seg_b0 >= tk_ms_to_bytes(v, v->max_seg_ms)) {
        // A hard cut is also evidence of a LEVEL LOCK: while the gate is open the floor only ever
        // falls, so a steady hum that opened it (fan/AC starting mid-take) would otherwise hold
        // st=1 forever and upload hours of noise Whisper hallucinates over. No one speaks 90 s
        // without a 600 ms pause — pull the floor halfway toward the sustained level on each cut:
        // true hum converges in 2-3 cuts and the gate closes; real speech is corrected back by the
        // fast downward attack at its first pause.
        if (v->floor_rms < rms) v->floor_rms += (rms - v->floor_rms) >> 1;
        tk_emit(v, b_end, true, out);
        return true;
    }
    return false;
}

bool nucleo_take_vad_flush(nucleo_take_vad_t *v, long b_end, nucleo_take_seg_t *out)
{
    if (!v || !out || b_end <= v->seg_b0) return false;
    tk_emit(v, b_end, v->st == 1, out);
    return true;
}

// ── journal ────────────────────────────────────────────────────────────────────────────────────
// Every line is flushed. An unflushed line is the ONLY thing a power cut can cost, which is the
// whole point: the take stays readable up to the last boundary the device committed.

// The error of a buffered write only surfaces at fflush (a full SD accepts fprintf into the stdio
// buffer just fine) — so every writer checks the flush, or "bytes written" would be a lie.
static int tk_seal(FILE *f, int n)
{
    if (fflush(f) != 0 || ferror(f) || n < 0) return -1;
    return n;
}

int nucleo_take_journal_open(FILE *f, int rate_hz, int channels, int bits,
                             const char *fmt, int hdr_bytes, long start_epoch)
{
    if (!f) return -1;
    int n = fprintf(f, "{\"t\":\"open\",\"v\":%d,\"rate\":%d,\"ch\":%d,\"bits\":%d,"
                       "\"fmt\":\"%s\",\"hdr\":%d,\"start\":%ld}\n",
                    NUCLEO_TAKE_JOURNAL_V, rate_hz, channels, bits, fmt ? fmt : "wav",
                    hdr_bytes, start_epoch);
    return tk_seal(f, n);
}

// Seconds are printed as integer milliseconds split by hand rather than with %f. The journal is
// written from the mic task, and pulling the floating-point formatter into that path costs stack
// we do not have to spare — and this way the bytes are identical whatever the libc does.
#define TK_SEC_FMT "%ld.%03ld"
#define TK_SEC_ARG(x) (long)((int64_t)((x) * 1000.0 + 0.5) / 1000), (long)((int64_t)((x) * 1000.0 + 0.5) % 1000)

int nucleo_take_journal_seg(FILE *f, const nucleo_take_seg_t *s)
{
    if (!f || !s) return -1;
    int n = fprintf(f, "{\"t\":\"seg\",\"i\":%d,\"t0\":" TK_SEC_FMT ",\"t1\":" TK_SEC_FMT
                       ",\"b0\":%ld,\"b1\":%ld,\"rms\":%d,\"voiced\":%s}\n",
                    s->index, TK_SEC_ARG(s->t0), TK_SEC_ARG(s->t1),
                    s->b0, s->b1, s->rms, s->voiced ? "true" : "false");
    return tk_seal(f, n);
}

int nucleo_take_journal_end(FILE *f, double dur_s, long data_bytes, int segs)
{
    if (!f) return -1;
    int n = fprintf(f, "{\"t\":\"end\",\"dur\":" TK_SEC_FMT ",\"bytes\":%ld,\"segs\":%d}\n",
                    TK_SEC_ARG(dur_s), data_bytes, segs);
    return tk_seal(f, n);
}

// ── reading a journal back ─────────────────────────────────────────────────────────────────────
// Everything below streams. The parser is hand-rolled rather than cJSON so nucleo_take stays free of
// dependencies and host-compilable — and because it only ever reads journals written by the writers
// above, whose field order is fixed.

// Read one line into `buf`, discarding anything past `cap`. A txt line can be kilobytes long, but
// every field a reader needs sits in the first ~120 bytes, so a small window is enough and bounded.
static bool tk_readline(FILE *f, char *buf, int cap)
{
    int n = 0, c;
    while ((c = fgetc(f)) != EOF && c != '\n') {
        if (n < cap - 1) buf[n++] = (char)c;
    }
    buf[n] = 0;
    return n > 0 || c == '\n';
}

// Position just past `"key":`, or NULL. First match wins — see the parsing note in the header.
static const char *tk_key(const char *s, const char *key)
{
    char pat[24];
    int n = snprintf(pat, sizeof pat, "\"%s\":", key);
    if (n <= 0 || n >= (int)sizeof pat) return NULL;
    const char *p = strstr(s, pat);
    return p ? p + n : NULL;
}

static long tk_long(const char *s, const char *key, long dflt)
{
    const char *p = tk_key(s, key);
    if (!p) return dflt;
    while (*p == ' ') p++;
    long sign = 1;
    if (*p == '-') { sign = -1; p++; }
    if (*p < '0' || *p > '9') return dflt;
    long v = 0;
    // Saturate instead of overflowing: a corrupt digit run (torn sector) must degrade to "a huge
    // value the caller clamps", never to signed-overflow UB or a sign flip.
    while (*p >= '0' && *p <= '9') {
        int d = *p++ - '0';
        if (v > (LONG_MAX - d) / 10) { v = LONG_MAX; while (*p >= '0' && *p <= '9') p++; break; }
        v = v * 10 + d;
    }
    return sign > 0 ? v : (v == LONG_MAX ? LONG_MIN : -v);
}

static bool tk_is_type(const char *s, const char *type)
{
    const char *p = tk_key(s, "t");
    if (!p || *p != '"') return false;
    size_t n = strlen(type);
    return !strncmp(p + 1, type, n) && p[1 + n] == '"';
}

// ── writing a transcript entry ─────────────────────────────────────────────────────────────────
// The text is escaped straight into the stream, so a 6 KB transcript needs no 6 KB buffer.
static void tk_put_escaped(FILE *f, const char *s)
{
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
            case '"':  fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\n': fputs("\\n", f);  break;
            case '\r': fputs("\\r", f);  break;
            case '\t': fputs("\\t", f);  break;
            default:
                if (c < 0x20) fprintf(f, "\\u%04x", c);   // control chars would break the line format
                else          fputc(c, f);
        }
    }
}

// lang/engine are our own short tokens; keep them to a safe alphabet rather than escaping them, so a
// malformed provider name can never produce a malformed journal.
static void tk_put_token(FILE *f, const char *s)
{
    for (int i = 0; s && s[i] && i < 48; i++) {
        char c = s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '/')
            fputc(c, f);
    }
}

int nucleo_take_journal_txt(FILE *f, int i0, int i1, const char *lang, const char *engine,
                            const char *text)
{
    if (!f || i0 < 0 || i1 < i0) return -1;
    fprintf(f, "{\"t\":\"txt\",\"i\":%d,\"j\":%d,\"lang\":\"", i0, i1);
    tk_put_token(f, lang ? lang : "");
    fputs("\",\"engine\":\"", f);
    tk_put_token(f, engine ? engine : "");
    fputs("\",\"text\":\"", f);
    tk_put_escaped(f, text);
    int n = fputs("\"}\n", f);
    return tk_seal(f, n) < 0 ? -1 : 1;
}

// ── resume ─────────────────────────────────────────────────────────────────────────────────────
int nucleo_take_scan_done(FILE *tx, nucleo_take_todo_t *t)
{
    if (!t) return -1;
    memset(t, 0, sizeof *t);
    if (!tx) return 0;                                   // no prior attempt: everything is pending
    rewind(tx);
    char line[192];
    while (tk_readline(tx, line, sizeof line)) {
        if (!tk_is_type(line, "txt")) continue;
        long i0 = tk_long(line, "i", -1);
        long i1 = tk_long(line, "j", i0);
        if (i0 < 0 || i1 < i0) continue;
        // Clamp BEFORE looping: a corrupt "j" (torn sector full of digits) must cost a bounded
        // overflow count, not minutes of spinning (or forever, at LONG_MAX) in the resume path.
        if (i1 >= NUCLEO_TAKE_MAX_SEGS) {
            long lost = i1 - NUCLEO_TAKE_MAX_SEGS + 1;
            t->overflow = (lost > INT_MAX - t->overflow) ? INT_MAX : t->overflow + (int)lost;
            i1 = NUCLEO_TAKE_MAX_SEGS - 1;
        }
        for (long i = i0; i <= i1; i++) t->done[i >> 3] |= (uint8_t)(1u << (i & 7));
        t->ranges++;
    }
    return t->ranges;
}

static bool tk_is_done(const nucleo_take_todo_t *t, long i)
{
    if (!t || i < 0) return false;
    if (i >= NUCLEO_TAKE_MAX_SEGS) return false;         // untracked -> redo it rather than skip it
    return (t->done[i >> 3] & (1u << (i & 7))) != 0;
}

bool nucleo_take_next_batch(FILE *journal, const nucleo_take_todo_t *done, long budget,
                            nucleo_take_batch_t *b)
{
    if (!journal || !b) return false;
    memset(b, 0, sizeof *b);
    if (budget <= 0) budget = 1;

    char line[192];
    while (tk_readline(journal, line, sizeof line)) {
        if (!tk_is_type(line, "seg")) continue;
        const char *v = tk_key(line, "voiced");
        if (!v || strncmp(v, "true", 4)) continue;       // a pause is never uploaded
        long i  = tk_long(line, "i",  -1);
        long b0 = tk_long(line, "b0", -1);
        long b1 = tk_long(line, "b1", -1);
        if (i < 0 || b0 < 0 || b1 <= b0) continue;
        if (tk_is_done(done, i)) continue;

        if (b->n == 0) b->first = (int)i;
        b->b0[b->n] = b0;
        b->b1[b->n] = b1;
        b->n++;
        b->bytes += b1 - b0;
        b->last = (int)i;

        if (b->bytes >= budget || b->n >= NUCLEO_TAKE_BATCH_MAX) return true;
    }
    return b->n > 0;                                     // EOF with a partial batch is still a batch
}

// ── flattening ─────────────────────────────────────────────────────────────────────────────────
// Entries are emitted in ASCENDING first-segment order, not file order: a batch that failed
// mid-take and was repaired on a later resume sits at the END of the tx journal, and emitting in
// file order would read "segments 1-39, 41-80, then 40". The selection is O(passes^2) over a
// few-KB journal — still fully streaming, nothing buffered.
long nucleo_take_flatten(FILE *tx, FILE *out)
{
    if (!tx || !out) return -1;
    long written = 0;
    bool first = true;
    long last_i = -1;                                     // highest first-index already emitted
    int c;
    for (;;) {
        // One pass: find the entry with the smallest first-index not yet emitted. An entry whose
        // index was already covered (a batch journalled twice by a retry) is skipped, never doubled.
        rewind(tx);
        char head[192];
        long best = LONG_MAX, best_pos = -1;
        for (;;) {
            long start = ftell(tx);
            if (!tk_readline(tx, head, sizeof head)) break;
            if (!tk_is_type(head, "txt")) continue;
            long i0 = tk_long(head, "i", -1);
            if (i0 < 0 || i0 <= last_i || i0 >= best) continue;
            best = i0; best_pos = start;
        }
        if (best_pos < 0) break;
        last_i = best;

        if (fseek(tx, best_pos, SEEK_SET) != 0) break;
        if (!tk_readline(tx, head, sizeof head)) break;
        const char *tp = tk_key(head, "text");
        if (!tp || *tp != '"') continue;
        if (fseek(tx, best_pos + (long)(tp - head) + 1, SEEK_SET) != 0) break;

        // Unescape in flight, one char at a time — a transcript can be far larger than any buffer
        // we are willing to hold. The separating space is emitted lazily so an empty entry (a
        // silence batch journalled as done) adds nothing.
        bool put_any = false;
        while ((c = fgetc(tx)) != EOF) {
            if (c == '"') break;                          // end of the JSON string
            if (c == '\\') {
                int e = fgetc(tx);
                if (e == EOF) break;
                switch (e) {
                    case 'n': c = '\n'; break;
                    case 'r': c = '\r'; break;
                    case 't': c = '\t'; break;
                    case 'u': {                            // only ever emitted for control chars
                        char hex[5] = {0};
                        for (int k = 0; k < 4; k++) { int h = fgetc(tx); if (h == EOF) { fflush(out); return written; } hex[k] = (char)h; }
                        c = (int)strtol(hex, NULL, 16);
                        break;
                    }
                    default: c = e; break;                 // \" and \\ map to themselves
                }
            }
            if (!put_any && !first) { fputc(' ', out); written++; }
            put_any = true; first = false;
            fputc(c, out);
            written++;
        }
    }
    fflush(out);
    return written;
}

// ── WAV durability ─────────────────────────────────────────────────────────────────────────────
void nucleo_take_wav_hdr(uint8_t h[44], uint32_t data_len, uint32_t rate, uint16_t ch, uint16_t bits)
{
    uint32_t byte_rate = rate * ch * (bits / 8), riff = 36 + data_len, fmt_len = 16;
    uint16_t block_align = (uint16_t)(ch * (bits / 8)), pcm = 1;
    memcpy(h,      "RIFF", 4);       memcpy(h + 4,  &riff, 4);
    memcpy(h + 8,  "WAVEfmt ", 8);   memcpy(h + 16, &fmt_len, 4);
    memcpy(h + 20, &pcm, 2);         memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &rate, 4);        memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 32, &block_align, 2); memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);       memcpy(h + 40, &data_len, 4);
}

long nucleo_take_wav_datalen(uint32_t claimed, long file_size, int hdr_bytes)
{
    long avail = file_size - hdr_bytes;
    if (avail <= 0) return 0;
    // A header claiming 0 is an interrupted take; one claiming more than the file holds is a lie.
    // Either way the bytes on disk are the truth.
    if (claimed == 0 || (long)claimed > avail) return avail;
    return (long)claimed;
}

int nucleo_take_wav_repair(const char *path)
{
    if (!path) return -1;
    FILE *f = fopen(path, "r+b");
    if (!f) return -1;

    uint8_t h[44];
    // Only a canonical 44-byte PCM header is touched. A file with extra chunks before `data` has a
    // different layout, and rewriting 44 bytes over it would destroy audio — bail instead.
    if (fread(h, 1, sizeof h, f) != sizeof h || memcmp(h, "RIFF", 4) ||
        memcmp(h + 8, "WAVE", 4) || memcmp(h + 36, "data", 4)) { fclose(f); return -1; }

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long size = ftell(f);

    uint32_t claimed, rate; uint16_t ch, bits;
    memcpy(&claimed, h + 40, 4);
    memcpy(&rate,    h + 24, 4);
    memcpy(&ch,      h + 22, 2);
    memcpy(&bits,    h + 34, 2);

    long real = nucleo_take_wav_datalen(claimed, size, 44);
    if (real <= 0)                  { fclose(f); return -1; }
    if ((uint32_t)real == claimed)  { fclose(f); return 0; }

    nucleo_take_wav_hdr(h, (uint32_t)real, rate, ch, bits);
    int ok = (fseek(f, 0, SEEK_SET) == 0) && fwrite(h, 1, sizeof h, f) == sizeof h;
    fflush(f);
    fclose(f);
    return ok ? 1 : -1;
}

// ── language ID, offline ───────────────────────────────────────────────────────────────────────
// Function words carry the signal: they are the highest-frequency tokens in any language and they
// barely overlap across these five. Words shared between languages ("que", "una", "con") do no harm
// because scoring counts every match and the distinctive ones decide the winner.
static const char *const LID_IT[] = { "che","non","per","con","una","del","della","sono","questo","anche",
    "come","piu","ma","gli","nel","alla","dei","essere","fare","molto","perche","quando","tutto","siamo",
    "hanno","questa","nella","dalla","quello","cosa", NULL };
static const char *const LID_EN[] = { "the","and","that","for","with","this","have","from","not","are",
    "was","you","they","would","there","which","about","been","will","what","when","your","just","like",
    "more","should","could","their","because","were", NULL };
static const char *const LID_ES[] = { "que","los","las","por","con","una","del","para","como","pero",
    "este","son","muy","todo","cuando","porque","tiene","hay","desde","tambien","sobre","hasta","donde",
    "ellos","nosotros","esta","sus","mas","hacer","puede", NULL };
static const char *const LID_FR[] = { "les","des","une","pour","dans","qui","pas","avec","sur","est",
    "sont","nous","vous","mais","comme","tout","plus","cette","aussi","etre","fait","bien","leur","donc",
    "elle","ils","cest","ete","peut","alors", NULL };
static const char *const LID_DE[] = { "der","die","das","und","ist","nicht","ein","eine","mit","auch",
    "auf","fur","dass","sich","von","dem","den","aber","wie","wir","haben","werden","sein","noch","oder",
    "nur","schon","kann","wenn","sind", NULL };

static const char *const *const LID_SETS[] = { LID_IT, LID_EN, LID_ES, LID_FR, LID_DE };
static const char *const       LID_CODES[] = { "it", "en", "es", "fr", "de" };
#define LID_N 5

// Fold a token to bare lowercase ASCII: accents are dropped rather than modelled, so "perché" and
// "perche" score the same and the tables stay plain 7-bit. UTF-8 two-byte Latin-1 supplement letters
// map to their unaccented base; anything else is skipped.
static int lid_fold(const char *s, int len, char *out, int cap)
{
    int n = 0;
    for (int i = 0; i < len && n < cap - 1; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xC3 && i + 1 < len) {                       // C3 xx = the accented Latin block
            unsigned char d = (unsigned char)s[++i] | 0x20;   // fold case within the block
            if      (d >= 0xA0 && d <= 0xA5) out[n++] = 'a';
            else if (d >= 0xA8 && d <= 0xAB) out[n++] = 'e';
            else if (d >= 0xAC && d <= 0xAF) out[n++] = 'i';
            else if (d >= 0xB2 && d <= 0xB6) out[n++] = 'o';
            else if (d >= 0xB9 && d <= 0xBC) out[n++] = 'u';
            else if (d == 0xA7)              out[n++] = 'c';
            else if (d == 0xB1)              out[n++] = 'n';
            continue;
        }
        if (c >= 'A' && c <= 'Z') out[n++] = (char)(c + 32);
        else if (c >= 'a' && c <= 'z') out[n++] = (char)c;
    }
    out[n] = 0;
    return n;
}

static bool lid_in(const char *const *set, const char *w)
{
    for (int i = 0; set[i]; i++) if (!strcmp(set[i], w)) return true;
    return false;
}

int nucleo_take_lid(const char *text, char *out, int cap)
{
    if (out && cap) out[0] = 0;
    if (!text || !out || cap < 3) return 0;

    int hits[LID_N]; memset(hits, 0, sizeof hits);
    int tokens = 0;
    const char *p = text;
    while (*p) {
        // A token is a run of letters (ASCII, or the UTF-8 accented block, which starts 0xC3).
        while (*p && !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (unsigned char)*p == 0xC3)) p++;
        const char *b = p;
        while (*p && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                      (unsigned char)*p >= 0x80)) p++;
        if (p == b) break;
        char w[24];
        if (lid_fold(b, (int)(p - b), w, (int)sizeof w) < 1) continue;
        tokens++;
        for (int L = 0; L < LID_N; L++) if (lid_in(LID_SETS[L], w)) hits[L]++;
    }

    // Under eight words there is not enough evidence for anyone to be confident, and pretending
    // otherwise is how a take ends up half-labelled Welsh.
    if (tokens < 8) return 0;

    int best = 0, second = 0, bi = 0;
    for (int L = 0; L < LID_N; L++) {
        if (hits[L] > best) { second = best; best = hits[L]; bi = L; }
        else if (hits[L] > second) second = hits[L];
    }
    if (best == 0) return 0;

    // Confidence blends how dense the function words are with how far ahead the winner is. A text
    // where two languages tie scores low on purpose, and the caller keeps what it had.
    int density = best * 100 / tokens;              // 0..100, typically 20-45 for real prose
    int margin  = (best - second) * 100 / best;     // 0..100
    int conf    = (density * 40 / 45) + (margin * 60 / 100);
    if (conf > 100) conf = 100;
    if (margin < 20) conf = conf / 3;               // too close to call
    snprintf(out, cap, "%s", LID_CODES[bi]);
    return conf;
}

// ── IMA ADPCM 4:1 ──────────────────────────────────────────────────────────────────────────────
// Both tables are const, so they live in flash and cost no RAM. The encoder state is two ints.
static const int8_t  TK_IDX[16]  = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static const int16_t TK_STEP[89] = {
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

void nucleo_take_adpcm_init(nucleo_take_adpcm_t *st)
{
    if (st) { st->pred = 0; st->index = 0; }
}

long nucleo_take_adpcm_size(long nsamples)
{
    if (nsamples <= 0) return 0;
    long blocks = (nsamples + NUCLEO_TAKE_ADPCM_SPB - 1) / NUCLEO_TAKE_ADPCM_SPB;
    return blocks * NUCLEO_TAKE_ADPCM_BLOCK;
}

static void tk_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)(v >> 8); }
static void tk_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);       p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF);
}

void nucleo_take_adpcm_hdr(uint8_t h[NUCLEO_TAKE_ADPCM_HDR], long nsamples, uint32_t rate)
{
    uint32_t data = (uint32_t)nucleo_take_adpcm_size(nsamples);
    // fmt is 20 bytes for IMA ADPCM (16 + cbSize + wSamplesPerBlock) and a `fact` chunk carries the
    // TRUE sample count, which is how a decoder knows to drop the padding in the final block.
    memcpy(h, "RIFF", 4);            tk_le32(h + 4, 52 + data);   /* 60 - 8 + data */
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);       tk_le32(h + 16, 20);
    tk_le16(h + 20, 0x0011);                                      /* WAVE_FORMAT_IMA_ADPCM */
    tk_le16(h + 22, 1);                                           /* mono */
    tk_le32(h + 24, rate);
    tk_le32(h + 28, (uint32_t)((uint64_t)rate * NUCLEO_TAKE_ADPCM_BLOCK / NUCLEO_TAKE_ADPCM_SPB));
    tk_le16(h + 32, NUCLEO_TAKE_ADPCM_BLOCK);                     /* nBlockAlign */
    tk_le16(h + 34, 4);                                           /* bits per sample */
    tk_le16(h + 36, 2);                                           /* cbSize */
    tk_le16(h + 38, NUCLEO_TAKE_ADPCM_SPB);                       /* wSamplesPerBlock */
    memcpy(h + 40, "fact", 4);       tk_le32(h + 44, 4);
    tk_le32(h + 48, (uint32_t)(nsamples > 0 ? nsamples : 0));
    memcpy(h + 52, "data", 4);       tk_le32(h + 56, data);
}

int nucleo_take_adpcm_block(nucleo_take_adpcm_t *st, const int16_t *pcm, int nsamples, uint8_t *out)
{
    if (!st || !pcm || !out || nsamples <= 0 || nsamples > NUCLEO_TAKE_ADPCM_SPB) return -1;

    // The block header holds the first sample verbatim plus the step index, which is what makes
    // every block independently decodable — a dropped block can never poison the rest of the take.
    st->pred = pcm[0];
    tk_le16(out, (uint16_t)(int16_t)st->pred);
    out[2] = (uint8_t)st->index;
    out[3] = 0;

    int nib = 0;
    for (int i = 1; i < NUCLEO_TAKE_ADPCM_SPB; i++) {
        int sample = (i < nsamples) ? pcm[i] : pcm[nsamples - 1];   // pad the tail, `fact` trims it
        int step   = TK_STEP[st->index];
        int diff   = sample - st->pred;
        int code   = 0;
        if (diff < 0) { code = 8; diff = -diff; }
        int vpdiff = step >> 3;
        if (diff >= step) { code |= 4; diff -= step; vpdiff += step; }
        step >>= 1;
        if (diff >= step) { code |= 2; diff -= step; vpdiff += step; }
        step >>= 1;
        if (diff >= step) { code |= 1; vpdiff += step; }

        st->pred += (code & 8) ? -vpdiff : vpdiff;
        if (st->pred >  32767) st->pred =  32767;
        if (st->pred < -32768) st->pred = -32768;
        st->index += TK_IDX[code];
        if (st->index < 0)  st->index = 0;
        if (st->index > 88) st->index = 88;

        // Nibbles are packed low-first, two per byte, starting at offset 4.
        if ((nib & 1) == 0) out[4 + (nib >> 1)] = (uint8_t)(code & 0x0F);
        else                out[4 + (nib >> 1)] |= (uint8_t)((code & 0x0F) << 4);
        nib++;
    }
    return NUCLEO_TAKE_ADPCM_BLOCK;
}
