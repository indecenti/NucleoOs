# On-device voice keyword engine (AVCEB v2)

The native push-to-talk recognizer that runs **on the Cardputer** (not the
browser Vosk path). Speaker-dependent, fully offline, no PSRAM. It is what the
**Voice Manager** web app trains via `POST /api/voice/learn` + the `voice/learned`
WebSocket event.

## Pipeline (`nucleo_voice_dsp.c` + `nucleo_voice.c`)

Hold **FN (G0)** and speak. Per spoken word ("burst"):

```
PDM 16 kHz → VAD (adaptive RMS, noise-floor tracking) → pre-emphasis → 25ms/10ms frames → Hann → FFT(512)
→ 26 mel filters (relative spectral floor) → log → DCT → 13 MFCC
→ CMN (per-utterance cepstral mean removal — channel/mic robustness)
→ resample to 64 canonical frames (speaking-rate normalization)
→ append delta (Δ-MFCC: ±2-frame time-derivative — captures transitions, channel-invariant)
→ int16 quantize → banded DTW vs the template store → best label
```

Matched labels are fused into a sentence and handed to `nucleo_anima_query()`,
which maps them to an intent/action (launch app, answer, …). So the engine
recognizes **words**; ANIMA turns word sequences into **commands** — the command
set can be larger than the trained vocabulary through composition.

The voice task is **pinned to APP_CPU (core 1)** — the codebase convention for
real-time audio — so I2S capture + FFT + DTW run off core 0, which the Wi-Fi/lwIP/
httpd stack drives. Capture stays glitch-free even while the Voice Manager web
client is connected. The match is I/O-bound (SD reads), not CPU-bound, so it is
deliberately *not* parallelised across cores — the 2 s budget makes that pointless.

The VAD threshold is **adaptive**: it tracks the ambient noise floor
(`threshold = clamp(floor × 3, 250..4000)`) rather than using a fixed level, so it
captures a quiet speaker in a silent room and rejects ambient in a noisy one.

### What's novel: self-consolidating, self-calibrating templates
Every **in-radius** match EMA-blends the spoken utterance back into its template
(`vdsp_consolidate`) *and* nudges that template's **accept radius** toward the
observed distance (`vdsp_radius_update`). So each word both sharpens toward the
centroid of *your* pronunciations and learns its own difficulty — no hand-tuned
absolute threshold. Adapted templates + radii are persisted (debounced) to SD on
PTT release.

## Memory discipline (PSRAM-less, ~76 KB free heap)
A template is `64 × 26 × int16` = **3328 B**. A full 20-word *resident* cache would
be ~67 KB — too close to the device's free heap to allocate reliably. So there is
**no resident cache**: templates live on SD and are **streamed one at a time**
during a match (`tpl_match_sd`), peak ~7 KB. Matching all 20 reads ~67 KB from SD
(tens of ms) — trivial against the 2 s budget, and it fits no matter how many words
are trained.

- `vdsp_ctx` (~7 KB precomputed tables): resident only while voice is ENABLED.
- `vdsp_acc` (~24 KB) + PCM chunk + 2 match buffers (~7 KB): allocated on PTT press,
  **freed on release**. Raw audio is never held — only compact MFCC frames.
- The EMA-adapted winner is written straight back to its `.tpl` on a confident match.
- Voice **disabled** (exclusive mode for a heavy native app): the task frees everything.
- Voice **stays enabled under web focus** (a connected web client): PTT is now cheap,
  and the Voice Manager + voice→web command routing both need it live.

## Accept policy — self-calibrating, mostly no tuning
A word is accepted if it clears a loose **ceiling** AND (clearly beats the
runner-up by **margin** OR falls within that template's self-tuned **radius**).
The margin gate is scale-free, so it survives the unknown absolute scale of real
voice; the radius gate calibrates itself per word from your repetitions. Constants
in `nucleo_voice.c` (distances are `vdsp_dtw`'s per-frame-averaged L1 MFCC units):

| #define | default | meaning |
|---|---|---|
| `VOICE_ABS_CEILING` | 8000 | hard sanity ceiling; rejects pure garbage |
| `VOICE_MARGIN_NUM/DEN` | 4 / 3 | margin accept: best ≤ 0.75 × second-best |
| `VOICE_ADAPT_ALPHA` | 38 | EMA pull (~0.15) of a confirmed utterance into its template |
| `VDSP_RADIUS_INIT` (dsp.h) | 6000 | loose starting radius; tightens with use |

In practice you should rarely touch these. Watch the **`voice/match`** event
(`{word,dist,second,radius}`, visible in `/api/logs` and the Voice Manager) to
see live distances; only widen `VOICE_ABS_CEILING` if good matches are rejected,
or tighten the margin if confusions slip through. Pick acoustically distinct,
multi-syllable words; avoid monosyllables and minimal pairs. 10–15 commands are
comfortable; ~20 with good word choice.

## Incompatible templates
On load, any `.tpl` not in the current `VTP3` format is auto-renamed to
`.tpl.old` (so the web app stops showing it as trained). Re-train it.

## Template format
Versioned `.tpl` (magic `VTP3`) under `/system/voice/`: 4-byte magic + 32-byte
label + `64×26×int16` (static MFCC + delta) + 4-byte int32 accept-radius. Older
templates are auto-quarantined to `.tpl.old` on load. Re-train via the Voice
Manager web app.

## Host verification
`tools/voice-host/build.ps1` compiles the **same** DSP C into a PC exe and checks
the math (FFT, mel, MFCC, CMN gain-robustness, rate invariance, DTW
discrimination, EMA, rejection) — 10/10. It does **not** prove real-voice
recognition rate; that is the on-device test.
