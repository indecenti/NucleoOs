# Offline voice (concatenative TTS)

**Offline** speech synthesis on the Cardputer (ESP32-S3, **no PSRAM**, ~512 KB SRAM). No on-board
phonemic synthesis (PicoTTS wants ~1.1 MB and needs PSRAM; eSpeak is borderline and robotic): instead we
**concatenate pre-voiced clips**. It is MOSAICO's voice twin: *grounded by construction*.

## Idea
Offline ANIMA **does not generate** text, it **retrieves** it from a finite set → its speech is
**pre-voiceable**. On the PC (once) the clips are generated with a high-quality TTS; at runtime the
device **pastes** the right pieces together. Natural voice, ~zero RAM, fully offline.

## What it speaks (policy)
The device speaks ONLY the **on-device** answers that make sense to say aloud:
- ✅ commands/L0, status (time/battery/date/space), launch confirmations ("Apro musica" — "Opening music"), fixed answers.
- ❌ **knowledge** (FACT/STITCH/REMOTE tiers) and **calculator** (intent `calc`) → it plays the phrase
  "**leggila sullo schermo**" ("read it on the screen", clip `read_it`) instead.
- ❌ text that is **too long** (>140 chars) or that **looks like code** (backticks/braces/tags/operators/keywords
  or a density >12% of technical characters) → "leggila".
- ❌ a word **not covered** by a clip → "leggila" (never spelling / a wrong reading).
- **Online** is a separate matter (the browser uses `speechSynthesis`; the device does not voice
  online answers).

## Firmware architecture (`firmware/components/nucleo_tts`)
- `nucleo_tts_plan.c` (PURE, testable): IT/EN text → `CLIP`/`PAUSE`/`UNKNOWN` tokens. Expands numbers
  into per-language cardinals (IT with elision: *ventitré*, *millenovecento…*; EN *twenty-three*), **greedy
  phrase** matching (common phrases win over single words), `nucleo_tts_text_speakable()` (content
  guard) and `nucleo_tts_full_slug()` (slug of the whole text, == the pipeline's `slugify`).
  - `nucleo_tts_speak_time(h, m, lang)` (PURE): composes the TIME into a **speakable phrase exact to the
    minute** made only of clips in the pack — IT *"Sono le 9 e 30" / "…in punto" / "…e un quarto" /
    "…e mezza" / "…meno un quarto" / "Mezzogiorno" / "Mezzanotte"* (≈ "It is 9:30" / "…o'clock" /
    "…quarter past" / "…half past" / "…quarter to" / "Noon" / "Midnight"); EN *"It is 9 30" / "…o'clock" /
    "noon" / "midnight"*. **Never** `HH:MM` (the `:` would become a pause and `:00` a "zero"). The same
    text goes to the screen and to the voice. The call sites (`app_anima.cpp`, `anima_get` in httpd, the
    `serve-shell.mjs` simulator) use it with an RTC guard (`now > 2023-01-01`, otherwise "orologio non
    impostato" — "clock not set").
- `nucleo_tts_index.c` (PURE): **RAM/CPU/IO-light** retrieval. NOT 1 file per clip in a FAT folder
  (the `stat` would be O(N)): **ONE `index.bin`** (slug→offset,len, SORTED) + **ONE `clips.pcm`**
  (concatenated PCM). Finding a clip = **binary search** via `fseek` (~log₂N reads of 56 B), ~zero RAM.
- `nucleo_tts.c`: opens the language's index, tries the **whole-fixed-answer → clip** lookup (for
  phrases longer than the phrase matcher's 6 words), otherwise plans and renders (CLIPs read from the blob,
  PAUSE = silence) into `/sd/data/tts/_say.wav` @ the index rate → `nucleo_audio_play()`. If the utterance
  has an `UNKNOWN` token → it plays `read_it`. A "speak" switch is persisted (`speak.cfg`, default ON).
  - **Anti-click (concatenation quality):** the clips are *trim-silence* + *loudnorm* → they start/end
    at a **non-zero** amplitude level: pasted raw, there is a jump at each join = an audible "tick".
    `nucleo_tts_declick_chunk()` (PURE, tested) applies a short **linear fade** (~16 samples @24kHz ≈
    0.7ms) to both edges of EVERY clip while `copy_clip` streams it → the clip|clip and clip|silence
    joins pass through ~0 and are smooth. **Zero RAM** (it scales in place the few edge samples of the
    already-existing buffer, no resampling, `render`'s IO/FD stays intact). A format marker
    (`fmt.ver` / `TTS_RENDER_FMT`) invalidates the per-slug WAV cache once at boot, so the fixed
    phrases too (`read_it`/identity/"non lo so" — "I don't know") are re-rendered with the new treatment.

On-device call sites: `app_anima.cpp` (`speak_result`) and `nucleo_voice.c` (PTT, only with no web client).
Toggle: native Settings ("Voce" / Voice row) + web Settings (AI tab) via `/api/tts`.

## File format (on SD: `/sd/data/tts/<lang>/`)
- `index.bin`: `"NTI1"` | u32 rate | u32 count | count SORTED records `{ char slug[48]; u32 off; u32 len }`.
- `clips.pcm`: mono 16-bit PCM @rate, all the clips concatenated (no per-clip header).
- `speak.cfg` / `_say.wav`: written at runtime by the firmware.
**Only 2 files per language** go on the device (FAT-friendly), not tens of thousands.

## RAM on the device (verified — adversarial audit)
The design is **RAM-light by construction**: the index is NOT in RAM (binary search via `fseek`), the blob
is **streamed** (fixed 1KB buffer), no large resident structure → the TTS adds **~0** to the idle budget
(the Cardputer's critical point, no-PSRAM).
- **Stack**: peak ~6.6 KB for one `nucleo_tts_say()` (`norm[1024]` + `u[96]≈5.4KB` in the planner). The
  stacks of the tasks that call it: ANIMA worker **30 KB**, voice task **16 KB** → 59–78% free.
  The recursion of `emit_int_it/en` is **bounded to 3 levels** (~72 B), with a digit-by-digit fallback
  beyond one million → no overflow risk.
- **Heap**: the only dynamic alloc = the token array `malloc(96 × ~56 = 5.4 KB)`, freed right after.
  It happens ONLY on the **offline-command** path (knowledge/calculator go to `read_hint`, no malloc)
  → healthy heap (~31 KB largest block) → ample margin; if it failed, `say()` returns false (no crash).
- **Files**: at peak 3 handles (index + blob + _say.wav), all closed on EVERY return (`render` handles
  blob/out; the caller always closes the index it lent) → no leak.
- **.bss**: negligible (a few bytes of state). The buffers are sized by the 140-char guard.

## Pipeline (PC, `tools/nucleo-tts/`)
- **Engine:** `gen_edge.py` — **Edge-TTS** (Microsoft neural voices, free, online only during generation;
  IT=Elsa / EN=Aria), **24 kHz**, **silence trim** at the edges + loudnorm. Async/concurrent/resumable.
  **Local/offline** alternative: `build_voice.py` (piper/espeak/pico engine) — the fallback if a 100%
  offline generation is needed.
- **Vocabulary (shared module `build_voice.py`):** four layers, deduplicated: the mandatory pack
  (IT/EN cardinals generated in code + `read_it`); `lexicon.<lang>.txt` (curated, real ANIMA phrases/
  words); `lexicon.wf.<lang>.txt` (authored by the `tts-coverage` workflow: ~470 IT/EN entries
  *grounded* on real speech, adversarially verified); `freq.<lang>.txt` (the ~3000 most frequent
  words from OpenSubtitles → conjugations by usage). `slugify` **folds accents like the
  firmware** (the slug MUST match) and `sanitize_text` strips what the TTS would read badly.
- **Targeted lexicon:** the set is aimed at ANIMA's **REAL speech** (extracted from the firmware), not at
  the whole dictionary — so the blob stays small (~tens/hundreds of MB) and the `fseek` is immediate.
- **Pack:** `build_index.py` packs the `.wav` files into `index.bin`+`clips.pcm` (byte order == the
  device's `strcmp`). `gen_enrich.ps1` / `gen_all.ps1` orchestrate gen→pack→(copy to H: with `-CopyToSd`).

## Tests
- `npm run anima:tts` (also inside `npm run anima:gate`): compiles the real firmware C with MinGW and
  verifies the **planner** (IT/EN: numbers/dates/decimals, phrases, UNKNOWN, code/length guard,
  full_slug, mathspeak, eq_result, translator, **decompose boundaries**, **anti-click** incl. per-chunk
  equivalence) and the **index** (binary search). 87/87 + 11/11. No hardware.
- **ANTI-REGRESSION IS PART OF THE BUILD:** the planner ctest is not just a separate command — the
  `CMakeLists.txt` of `nucleo_tts` compiles+runs it **inside `idf.py build`** (via `find_program(gcc)` +
  `cmake -E env PATH=<mingw bin>` because gcc loads its DLLs from there). If the text→clip logic regresses,
  the **firmware does NOT build** (the gate is a build dependency of the `COMPONENT_LIB`, it runs before the
  link). Stamp + `DEPENDS` → it re-fires ONLY when `nucleo_tts_plan.c` / the ctest / the header change
  (incremental builds of other components = zero cost). **Graceful skip** if the host gcc is missing (CI/
  minimal machines stay green; `anima:tts` remains the full net with index/replies/time).
- `npm run anima:tts-replies`: proves END-TO-END that **ANIMA's real answers actually get spoken** —
  it loads the generated index (`deploy/sd-safe/data/tts/<lang>/index.bin`) and for each answer
  (`tools/anima-host/test-replies.<lang>.txt`) replicates the decision of `nucleo_tts_say`: WHOLE-clip /
  SPEAK(clip list) / READ_IT, printing the **uncovered words**. Calc/code/long answers (rightly) go
  to READ_IT; the speakable ones must have 0 uncovered. A clean SKIP if the index has not been
  generated yet.

## Status / to do
**RELEASED on the hardware** (FLASHED on COM3, ~1.76 MB; voice pool on SD H: IT 12504 / EN 12507 clips, 0
uncovered answers, 0/1440 uncovered minutes). Live: compositional planner (composes words that were never
recorded from their sub-pieces), mathspeak (`= % ^ +`), `eq_result`/`has_mathtypo` (geometry/physics speak the
result), translator homograph fix, rounding, WDT pets + 5s FATFS timeout, per-slug WAV cache, **anti-regression
build gate**. Deliberate stability choices: SYNCHRONOUS voice (async exhausted FDs/heap), no
FD refactor of `render` without host coverage. **To verify on hardware**: the real assembly latency
from the blob on SD and the quality on the tiny speaker. See memory note `nucleo-tts-offline`.

**Not yet flashed:** **anti-click** on the clip edges (`nucleo_tts_declick_chunk`, gate 87/87, `render`'s
IO/FD intact, zero RAM). To be confirmed **by ear** after the flash: concatenated phrases should
sound smoother at the joins (no "tick"). Reversible (just stop calling the declick in
`copy_clip`). The deterministic text coverage is already saturated (replies 0 uncovered, time 0/1440) → to
*read even more things* one would need to widen the clip pool (regen) or add acronym spelling (letter clips).
