// nucleo_take — the take JOURNAL: a recording is an append-only stream, not a file.
//
// WHY: a 2 h take is ~230 MB of WAV whose header is only patched at stop — a crash or a flat
// battery at minute 118 leaves data_len = 0 and an unreadable file. And the transcription path
// has to re-derive segment boundaries by parsing the whole WAV every time it resumes.
//
// The journal fixes both. While the mic runs we append one NDJSON line per segment to
// <take>.ndjson — offsets, timestamps, level, voiced/silent. Nothing is ever rewritten and no
// seek-back is needed, so a crash costs at most the last unflushed line. The index therefore
// exists BEFORE anyone thinks about transcribing, and whoever transcribes later (device now,
// device after a reboot, or the browser) just fills in the lines that have no text yet.
//
// This file is deliberately free of ESP-IDF: it is compiled and asserted on the PC by
// tools/anima-host/take-check.mjs (npm run take:test) before it ever runs on the device.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define NUCLEO_TAKE_JOURNAL_V 1

// ── segment ────────────────────────────────────────────────────────────────────────────────────
// Byte offsets are DATA offsets (the PCM stream), NOT file offsets: the "open" line records the
// header size so a reader adds it once. Keeps the journal valid if the container ever changes.
typedef struct {
    int    index;        // monotonic, counts silent segments too so the timeline has no holes
    double t0, t1;       // seconds from the start of the take
    long   b0, b1;       // data-byte offsets [b0, b1)
    int    rms;          // mean RMS over the segment, 0..32767
    bool   voiced;       // false = a pause. Never uploaded: Whisper invents text over silence.
} nucleo_take_seg_t;

// ── VAD segmenter ──────────────────────────────────────────────────────────────────────────────
// Cuts on silence instead of a wall clock, so no word is split across an upload boundary and the
// pauses (30-50 % of a real meeting) are never sent at all. The gate is ADAPTIVE: the original
// Cardputer's PDM mic and the ADV's ES8311 sit at completely different noise floors, so a fixed
// threshold would be deaf on one board and hair-trigger on the other.
typedef struct {
    // config (set by _init, overridable afterwards)
    int  rate_hz;
    int  bytes_per_sample;   // 2 = mono 16-bit
    int  min_voice_ms;       // voice must hold this long to open a segment (rejects clicks)
    int  min_sil_ms;         // silence must hold this long to close one (rides over breaths)
    int  max_seg_ms;         // hard cut even mid-speech: bounds one upload
    int  roll_ms;            // pre/post-roll kept around a voiced run so attacks aren't clipped
    // state. Everything is derived from the BYTE offset the caller feeds — one clock, no drift
    // between a sample counter and the file, and a resumed take needs no replay to re-sync.
    int      st;             // 0 = silence, 1 = voice
    int      index;
    long     seg_b0;         // start of the segment being built
    long     run_b0;         // start of the current sub-run (voice run in silence, or vice versa)
    int      run_ms;
    int64_t  acc_rms, acc_ms;   // level accumulator for the segment being built
    int      floor_rms;      // tracked noise floor
    int      seed_ms;        // bootstrap window still open?
} nucleo_take_vad_t;

void nucleo_take_vad_init(nucleo_take_vad_t *v, int rate_hz, int bytes_per_sample);

// Feed one capture chunk (contract: <= 200 ms per call; the recorder feeds 32 ms).
// `b0` is the data-byte offset of the chunk's FIRST sample. Returns true when a segment just
// closed, filling *out. At most one segment closes per call.
bool nucleo_take_vad_feed(nucleo_take_vad_t *v, const int16_t *pcm, int nsamples, long b0,
                          nucleo_take_seg_t *out);

// Close whatever segment is still open at the end of the take. Returns false if there is nothing.
bool nucleo_take_vad_flush(nucleo_take_vad_t *v, long b_end, nucleo_take_seg_t *out);

// RMS of a chunk, 0..32767 (integer sqrt — no libm on the audio path).
int  nucleo_take_rms(const int16_t *pcm, int nsamples);

// ── journal writer ─────────────────────────────────────────────────────────────────────────────
// `f` is opened "a" (or "w" for a fresh take). Every writer fflushes: an unflushed line is the
// only thing a power cut can cost us. Return: bytes written, or <0.
int nucleo_take_journal_open(FILE *f, int rate_hz, int channels, int bits,
                             const char *fmt, int hdr_bytes, long start_epoch);
int nucleo_take_journal_seg(FILE *f, const nucleo_take_seg_t *s);
int nucleo_take_journal_end(FILE *f, double dur_s, long data_bytes, int segs);

// ── reading a journal back: batches, resume, flattening ────────────────────────────────────────
// This is the half that makes the journal pay off. A transcriber no longer slices the WAV on a wall
// clock and hopes; it walks the journal, takes only the segments that hold voice, and skips the ones
// it has already done. Everything here streams: the largest structure is a few hundred bytes on the
// caller's stack, whatever the length of the take.
//
// Parsing note: this reads journals THIS code wrote. Field order is fixed by the writers above, so
// the parser looks for the first occurrence of each key — which is why "i" is always emitted before
// "text", whose contents are arbitrary.

// Which segments already have a transcript. One bit each; a take longer than this simply loses the
// resume optimisation (it re-transcribes), it never produces a wrong result.
#define NUCLEO_TAKE_MAX_SEGS 4096
typedef struct {
    uint8_t done[NUCLEO_TAKE_MAX_SEGS / 8];   // 512 B — allocate it, don't put it on a 4 KB stack
    int     ranges;                            // txt lines seen
    int     overflow;                          // segments beyond MAX_SEGS that we could not track
} nucleo_take_todo_t;

// One upload's worth of audio: the voiced ranges to concatenate, and which journal segments they
// are. Silence between them is simply not sent — on real material that is 30-50 % of the take.
#define NUCLEO_TAKE_BATCH_MAX 48
typedef struct {
    long b0[NUCLEO_TAKE_BATCH_MAX];
    long b1[NUCLEO_TAKE_BATCH_MAX];
    int  n;
    long bytes;      // total PCM bytes across the ranges = what the uploader must send
    int  first;      // journal index of the first segment in the batch
    int  last;       // ...and the last, so one txt line can cover the whole batch
} nucleo_take_batch_t;

// Append a transcript covering journal segments [i0..i1]. `text` is escaped as it streams, so no
// intermediate buffer is needed however long the transcript is. Returns >0 on success.
int nucleo_take_journal_txt(FILE *f, int i0, int i1, const char *lang, const char *engine,
                            const char *text);

// Read a transcript journal and mark every segment that already has text. Returns the number of
// txt lines seen, or -1. Pass a freshly zeroed nucleo_take_todo_t.
int nucleo_take_scan_done(FILE *tx, nucleo_take_todo_t *t);

// Collect the next batch of voiced, not-yet-transcribed segments, up to `budget` PCM bytes, reading
// the journal forward from its current position. Returns false when there is nothing left.
bool nucleo_take_next_batch(FILE *journal, const nucleo_take_todo_t *done, long budget,
                            nucleo_take_batch_t *b);

// Rebuild the flat transcript from a transcript journal (unescaping as it streams, one space between
// entries) — this is how a resumed take recovers the text it already paid for. Returns chars written.
long nucleo_take_flatten(FILE *tx, FILE *out);

// ── WAV durability ─────────────────────────────────────────────────────────────────────────────
// Canonical 44-byte little-endian PCM header. Written once at open with data_len 0 and made final
// only at stop — NEVER rewritten while recording (the seek-back would stall the mic loop). An
// interrupted take therefore claims data_len 0 until nucleo_take_wav_repair() fixes it, once,
// with the mic idle.
void nucleo_take_wav_hdr(uint8_t h[44], uint32_t data_len, uint32_t rate, uint16_t ch, uint16_t bits);

// How many PCM bytes a WAV really holds, ignoring a header that claims 0 or lies. `file_size` is
// the size on disk. Returns 0 if the file is smaller than a header.
long nucleo_take_wav_datalen(uint32_t claimed, long file_size, int hdr_bytes);

// Make an interrupted take's header honest, once. Opens `path` "r+b", compares the claimed length
// with the bytes on disk and rewrites the 44-byte header only if they disagree. Returns 1 when it
// repaired, 0 when the header was already right, <0 when the file is not a canonical PCM WAV.
//
// Call this with the MIC IDLE and on one file at a time: it seeks to the end, and on a 230 MB FAT
// file that is a cluster-chain walk. Never in the capture loop — that is why the recorder only
// flushes while it records and leaves the repair to whoever opens the take next.
int nucleo_take_wav_repair(const char *path);

// ── language ID, offline ───────────────────────────────────────────────────────────────────────
// Whisper reports the language it heard, and that is the best signal we have — but it is unreliable
// on short audio and it can drift mid-take (segment 40 of a two-hour Italian meeting suddenly comes
// back "Welsh"). So the transcript gets a second opinion from the text itself: function-word scoring
// over the five languages the OS ships. Both tables are const, so this costs flash, not RAM.
//
// Returns confidence 0..100 and writes an ISO code ("it"/"en"/"es"/"fr"/"de") into `out`. A return of
// 0 means "not enough evidence" — too short, or two languages too close to call. Abstaining is the
// correct answer for a five-word segment, and the caller keeps whatever it already had.
int nucleo_take_lid(const char *text, char *out, int cap);

// ── IMA ADPCM 4:1 (the codec we can actually afford) ────────────────────────────────────────────
// Uploading a 2 h take as raw PCM is ~230 MB, and the obvious answer — encode MP3 on the device —
// is not available to us: shine's encoder context is a SINGLE malloc of ~96 KB (l3_enc 9 KB +
// l3_sb_sample 13.5 KB + mdct_freq 9 KB + subband 12 KB + l3loop, whose int2idx[10000] alone is
// 40 KB), which is larger than this chip's largest free block at any point after boot. Even the
// Helix DECODER, at ~20 KB across eight small blocks, already needs an OOM-retry path here.
//
// IMA ADPCM gives 4:1 for eight bytes of state and two const tables that live in flash. Quality is
// ~20 dB SNR at 16 kHz — full bandwidth preserved, which is what an ASR model actually cares about.
//
// Blocks are self-contained (each carries its own predictor + step index), 256 bytes each, holding
// 505 samples: one verbatim in the block header plus 504 nibbles. The last block is padded with its
// final sample and the true length is declared in the WAV `fact` chunk, so the encoded size is an
// exact function of the sample count — the uploader can state Content-Length before encoding a
// single byte and stream straight into the socket with no temp file and no buffering.
#define NUCLEO_TAKE_ADPCM_BLOCK 256   // bytes per block (mono)
#define NUCLEO_TAKE_ADPCM_SPB   505   // samples per block: 1 in the header + (256-4)*2 nibbles
#define NUCLEO_TAKE_ADPCM_HDR   60    // RIFF + fmt(20) + fact + data chunk headers

typedef struct { int32_t pred; int32_t index; } nucleo_take_adpcm_t;

void nucleo_take_adpcm_init(nucleo_take_adpcm_t *st);

// Exact encoded payload size, in bytes, for `nsamples` mono samples (last block padded).
long nucleo_take_adpcm_size(long nsamples);

// 60-byte IMA ADPCM WAV header for `nsamples` mono samples at `rate`.
void nucleo_take_adpcm_hdr(uint8_t h[NUCLEO_TAKE_ADPCM_HDR], long nsamples, uint32_t rate);

// Encode ONE block. `nsamples` may be < NUCLEO_TAKE_ADPCM_SPB only for the final block, which is
// padded with its last sample. Always writes exactly NUCLEO_TAKE_ADPCM_BLOCK bytes. Returns those
// bytes, or -1 on bad input.
int nucleo_take_adpcm_block(nucleo_take_adpcm_t *st, const int16_t *pcm, int nsamples, uint8_t *out);
