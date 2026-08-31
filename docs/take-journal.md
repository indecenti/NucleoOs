# The take journal — a recording is a stream, not a file

**Status:** phases 0-4 shipped, built and gated. Not flashed. Open items at the end.
**Code:** `firmware/components/nucleo_take/` · **Gate:** `npm run take:test`

## The problem

A two-hour take is ~230 MB of 16 kHz mono WAV. Three things broke at that length, and none of them
were about which speech-to-text provider we used:

| | Before | Consequence |
|---|---|---|
| WAV header | `data_len` written only at stop | a crash or flat battery at minute 118 left a file no player would open |
| SD writes | `fwrite` return ignored | a card hiccup truncated the take in silence |
| Transcription | re-derive 90 s boundaries by parsing the whole WAV, 80 fresh TLS sessions | ~32 min of continuous upload, 80 chances to OOM on a heap whose largest free block is ~31 KB |

The fix is not a better provider. It is a better shape for the data.

## The shape

A take is an **append-only stream with an append-only index**. Nothing is ever rewritten, so there
is no window in which the file is inconsistent.

```
/data/Recordings/rec-20260821-143200.wav       the audio
/data/Recordings/rec-20260821-143200.ndjson    the journal
```

```ndjson
{"t":"open","v":1,"rate":16000,"ch":1,"bits":16,"fmt":"wav","hdr":44,"start":1755782400}
{"t":"seg","i":0,"t0":0.000,"t1":1.750,"b0":0,"b1":56000,"rms":58,"voiced":false}
{"t":"seg","i":1,"t0":1.750,"t1":5.250,"b0":56000,"b1":168000,"rms":4102,"voiced":true}
{"t":"end","dur":11.500,"bytes":368000,"segs":5}
```

Rules the format lives by:

- **One JSON object per line, flushed on write.** A power cut costs the last unflushed line — one
  segment — never the take. The host gate asserts exactly this by truncating the file and re-parsing.
- **Offsets are DATA offsets, not file offsets.** `open.hdr` records the container's header size once.
  The index therefore survives a change of container (phase 2 swaps WAV for MP3).
- **The timeline has no holes.** `b1` of segment *N* is `b0` of *N+1*, indices are monotonic, and the
  last segment ends where the capture did. A hole would silently drop audio from a transcript, which
  is the one failure a recorder must not have.
- **Pauses are segments too**, flagged `voiced:false`. They are never uploaded — Whisper invents text
  over silence — but they keep the timeline continuous and they carry the level for the waveform.

## Why the index is written during recording

Because the segmenter is free at that moment. The RMS of every chunk is already computed for the
level meter; the VAD is the same number with hysteresis on it. Writing one line per segment costs a
few hundred bytes of SD per minute.

The payoff is that **the index exists before anybody asks for a transcript**. Transcription stops
being "parse 230 MB, slice, upload, hope" and becomes: read the journal, find `seg` lines with no
text yet, fill them in. Which means:

- **Resumable.** Interrupted at segment 40 of 80? Restart fills 41 onwards.
- **Engine-agnostic.** Each filled line records which engine produced it, so a take may legitimately
  be half Groq Whisper and half in-browser Whisper WASM.
- **Two-headed.** The device can start a take in Solo boot (no HTTP server, big contiguous heap) and
  the browser can finish it when the console next connects. Same file, no handshake.

## The segmenter

`nucleo_take_vad_*` cuts on silence rather than on a wall clock.

- **Adaptive gate.** The original Cardputer's PDM mic and the ADV's ES8311 idle at completely
  different noise floors, so the thresholds are multiples of a *tracked* floor (3.0× to open, 1.8× to
  close) with absolute guards. The floor is seeded from the quietest of the first 800 ms, then tracks
  upward only while the gate is shut — tracking during speech would let it climb until the gate went
  deaf. It tracks *downward* in any state, so a take that starts mid-sentence recovers in about a
  second instead of staying deaf for the whole recording.
- **Hysteresis in time, not just level.** Voice must hold 200 ms to open a segment (a chair, a key
  press and a knock are all shorter) and silence must hold 600 ms to close one (longer than a breath,
  shorter than a real pause).
- **Pre/post-roll of 250 ms** around every voiced run, so no attack is clipped and no tail is cut.
  This costs nothing: the audio is already on the SD, so the file *is* the lookahead buffer.
- **A hard cut at 90 s** for uninterrupted speech, which bounds one upload and one transcript reply.

On real material 30-50 % of a meeting is pause. Those bytes are now never sent.

## Durability

Three rules, and one deliberate omission.

- **The capture loop only ever appends.** It flushes every ~5 s, so a crash costs at most five
  seconds of buffered audio, and it never seeks. The obvious alternative — patching the WAV header
  in place every 30 s — was rejected: seeking back to the end of a 230 MB FAT file walks the cluster
  chain, ~50-100 ms with the mic still running. That is long enough to drop samples. **Nothing gets
  to stall the capture path.**
- **The header is repaired later instead, once.** `nucleo_take_wav_repair()` runs when a take is
  played or transcribed — one file, mic idle, a no-op on every take that is already fine. It refuses
  anything that is not a canonical 44-byte PCM WAV rather than risk rewriting real audio.
- **`nucleo_take_wav_datalen()` treats the bytes on disk as the truth**, overruling a header that
  claims 0 (interrupted) or claims more than the file holds (stale). The transcriber's `wav_parse()`
  applies the same rule, so a take killed at minute 118 transcribes in full even before any repair.
- **A short `fwrite` stops the take cleanly** and publishes `rec.writefail`, instead of letting a
  card hiccup truncate the recording in silence.

One consequence to keep in mind when reading a journal: `seg` lines are flushed immediately, but the
audio they point at may still be in the stdio buffer. **The file size is authoritative** — a reader
must clamp segment offsets to the bytes that actually exist. That is the correct direction for the
error to point: the index may briefly promise slightly more than the file holds, never less.

## Gate

`npm run take:test` compiles the real `nucleo_take.c` on the PC and runs 188 assertions over
synthetic speech — deterministic square waves, so every boundary is an exact number rather than a
judgement call: basic shape, a 96 ms click that must not open a segment, a 320 ms breath that must
not split a phrase, the hard cut, a noisy board, a take that starts mid-sentence, an empty room, and
both halves of the recovery path, the ADPCM codec (exact sizing, header fields, block independence,
round-trip SNR against a reference decoder written from the spec), the journal reader (batching,
resume, escaping round-trip) and the language ID including its abstentions. Then `take-check.mjs`
re-reads the journal the run produced as JSON and checks the contract from the outside, the way the
browser will — tearing off the tail to prove a power cut costs one line — and lints the uploader
source for the two persistent-connection rules it must never break.

## Why not MP3 — the arithmetic

The obvious way to stop uploading 230 MB is to encode the take. It does not fit, and the margin is
not close enough to be worth a try.

`shine` (the fixed-point MP3 encoder everyone reaches for on an MCU) allocates its whole context in a
**single** `malloc(sizeof(shine_global_config))`. From the struct, with `MAX_GRANULES = 2`,
`GRANULE_SIZE = 576`, `SBLIMIT = 32`, `HAN_SIZE = 512`:

| field | bytes |
|---|---:|
| `l3_enc[2][2][576]` | 9 216 |
| `l3_sb_sample[2][3][18][32]` | 13 824 |
| `mdct_freq[2][2][576]` | 9 216 |
| `l3loop.int2idx[10000]` (int) | **40 000** |
| `l3loop.xrsq + xrabs` | 4 608 |
| `l3loop.steptab + steptabi` | 1 536 |
| `subband.fl[32][64]` | 8 192 |
| `subband.x[2][512]` | 4 096 |
| `mdct.cos_l[18][36]` | 2 592 |
| side info, ratio, scalefactors, bitstream, wave, mpeg | ~2 000 |
| **total, one contiguous block** | **≈ 95 KB** |

Those arrays are dimensioned `[MAX_CHANNELS]` unconditionally, so **mono buys nothing**. Against this
device — ~150 KB free and ~86 KB largest block at boot start, ~40 KB free and ~31 KB largest by the
time httpd is up — a 95 KB contiguous allocation is not tight, it is impossible at every moment after
boot. For calibration: the Helix *decoder*, at ~20 KB spread over eight small blocks, already needed
an OOM-reclaim-and-retry path in `nucleo_audio_mp3.c`.

Opus is no better (30-50 KB of encoder state). So the codec had to be one whose state is a rounding
error, and that is IMA ADPCM: **8 bytes** of encoder state, two `const` tables in flash, 4:1.

## What the four phases actually did

**1 — one TLS session per take.** `esp_http_client_close()` calls `esp_transport_close()`, so the old
per-slice `init`/`cleanup` really did pay 80 handshakes on a 2 h take — and, worse, 80 alloc/free
cycles of the mbedTLS record buffers on a heap whose largest block is ~31 KB. Now one connection
serves the whole take: `esp_http_client_open()` re-dials only when `state < HTTP_STATE_CONNECTED`, and
`esp_http_client_prepare()` (which `open()` always runs) resets the parser and the request line. The
two rules that keeps honest — drain every response, never `close()` between requests — are enforced by
a source lint in the gate, and the lint was checked against a deliberate violation.

*The trade this makes:* the TLS buffers are now held for the whole take instead of being freed
between segments. That is the right way round — one allocation lottery instead of eighty — and the
recorder already runs its AI jobs with the launcher canvas freed and the L1 index unloaded.

**2 — IMA ADPCM 4:1 on the way out.** Encoded straight into the socket: one 1 010-byte read, one
256-byte block, no temp file, nothing buffered. Blocks are self-contained (each carries its own
predictor and step index) and the encoded size is an exact function of the sample count, so
Content-Length is known before anything is encoded. Round-trip SNR on speech-shaped signal is 37 dB.
Because no endpoint promises to decode it, the first segment of each take is the probe: a **4xx**
drops the take to PCM permanently and re-sends that segment; a 5xx or a timeout is a transient and
does not cost the 4x. `"tx_codec": "pcm"` in `teacher.json` opts out entirely.

**3 — the transcriber walks the journal.** Instead of slicing on a wall clock, it reads
`<take>.ndjson`, batches consecutive **voiced** segments up to 90 s and uploads only those. Pauses are
never sent. Results go to `<take>.tx.ndjson` before the flat sidecar, so a run that dies at batch 40
of 80 resumes at 41 and the flat file is rebuilt from the journal rather than appended to twice. Each
entry records the engine that produced it. Takes recorded before this feature — and files uploaded
through the web app — have no journal and fall through to the old fixed-length path untouched.

**4 — the language is decided once, and checked.** `nucleo_take_lid()` scores function words across
the five OS languages, folding accents so a transcript reads the same with or without them. It
**abstains** under eight tokens or when two languages are too close, because a wrong lock is worse
than no lock: the language drives the transcript, the summary and the TTS voice for the whole take.
It fills in when the engine reported nothing and overrules the engine only when the text is emphatic
(confidence ≥ 70) — which is the case Whisper actually gets wrong, drifting to a random language on a
short or noisy segment. On the browser side, `CAPMATRIX` gains `audioLLM`: an audio-native model
(Gemini) reaches audio-to-text without a Whisper endpoint, in one call instead of two, and
`routeFor({capability:'whisper'})` now falls back to it instead of declining.

## What that adds up to for a 2 h take

| | before | after |
|---|---|---|
| bytes uploaded | 230 MB | ~35 MB (≈40 % pauses dropped, then 4:1) |
| TLS handshakes | 80 | 1 |
| mbedTLS buffer alloc/free cycles | 80 | 1 |
| interrupted at minute 118 | unreadable file, restart from zero | header repaired, resumes at the next batch |
| RAM added on the device | — | 8 B codec state, ~1.1 KB transient heap, ~750 B stack |
| firmware size | — | +8.0 KB (8 240 B) |

## Not done, deliberately

The **in-browser Whisper rung** (WebGPU/WASM, keyless, offline) is the one engine that would remove
the API key entirely. It is not in this pass because the model is 40-190 MB and this repo already
fights a 2 GB push limit — so it must load from a CDN and cache in the browser, the way the Vosk
models already do, and that needs live verification rather than a blind commit. The ladder policy it
plugs into (`audioLLM` routing, per-entry engine provenance in the transcript journal) is in place.

The **incremental fold** — folding each batch into a bounded running summary instead of a second
map-reduce pass — is also still open. Worth being precise about why it slipped: its headline benefit
was said to be O(1) RAM, and on re-reading `nucleo_anima_summarize_file()` that was already true, it
streams 6 KB windows. What is left is saving the second pass's round trips, which is real but much
smaller than it looked, and it changes model behaviour in a way that needs a live key to tune.
