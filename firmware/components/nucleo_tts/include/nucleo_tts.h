// nucleo_tts — on-device OFFLINE voice via concatenation of pre-rendered clips.
//
// WHY THIS WAY: the Cardputer (ESP32-S3, NO PSRAM, ~512KB SRAM) cannot synthesize
// phonemes in real time — PicoTTS wants ~1.1MB of RAM even with mmap'd resources (needs PSRAM),
// eSpeak ~120KB at the absolute limit. BUT offline ANIMA is NOT a generative LLM: it's a
// retrieval cascade over a FINITE corpus, known at build time. So we pre-voice offline (on the PC,
// with a quality TTS) the corpus + the closed-class words (numbers/dates/units/connectives),
// and at runtime we CONCATENATE the clips. Natural voice, ~zero RAM, purely offline and standalone. It's the
// voice twin of MOSAICO: "grounded by construction".
//
// FLOW: text -> nucleo_tts_plan() (this module, pure+testable) -> a sequence of CLIP/PAUSE tokens
// -> nucleo_tts_say() assembles the clips' PCM into ONE temporary WAV and plays it via nucleo_audio
// (the battle-tested audio path isn't touched). The clips live on SD (/sd/data/tts/it/<slug>.wav),
// NOT in flash (preference: no bakeable assets in the image).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- planner (pure, host-compilable: no ESP/SD) -----------------------------------------

typedef enum {
    TTS_TOK_CLIP = 0,   // plays the clip identified by `slug` (word/phrase/number)
    TTS_TOK_PAUSE,      // insert `ms` of silence (punctuation/prosody)
    TTS_TOK_UNKNOWN,    // word NOT covered by any clip -> the utterance isn't speakable
                        // cleanly: nucleo_tts_say() falls back to the "leggila sullo schermo" phrase.
} tts_tok_kind_t;

typedef struct {
    tts_tok_kind_t kind;
    char slug[48];      // CLIP: clip slug -> file /sd/data/tts/<lang>/<slug>.wav
    int  ms;            // PAUSE: silence duration in ms
    bool fallback;      // CLIP: 1 if produced by fallback (e.g. letter-by-letter spelling)
} tts_token_t;

// Callback: true if the slug has a rendered clip. Lets the planner degrade gracefully
// (spelling) on unknown words. `ud` is opaque (e.g. the language). The numbers/connectives of the
// "mandatory package" are emitted by the planner regardless (build_voice.py always generates them).
typedef bool (*tts_has_clip_fn)(const char *slug, void *ud);

// Plans an utterance into a sequence of tokens. Normalizes (lowercase, fold accents), expands
// numbers/decimals/sign into the language's cardinals (lang "en" -> English, otherwise Italian),
// tries a PHRASE match (greedy, MOSAICO-style) BEFORE a word match — so common phrases
// take precedence — and falls back to spelling for unknown words. Returns the tokens written (<= max).
int nucleo_tts_plan(const char *text, const char *lang,
                    tts_has_clip_fn has_clip, void *ud,
                    tts_token_t *out, int max);

// Slug of the whole `text` (fold accents + lowercase + words joined by '_', truncated to cap-1) — IDENTICAL
// to build_voice.py's slugify(). Used for the "whole fixed answer -> single clip" lookup (greetings,
// "non lo so", pairing...): so they play with natural prosody, beyond the planner's 6-word limit.
void nucleo_tts_full_slug(const char *text, char *out, int cap);

// Content guard (OFFLINE): true only if `text` makes SENSE to speak aloud. False if it's
// too long (> max_chars, 0 = no limit) or "looks like code"/markup (backticks, braces, tags,
// operators, keywords, a high density of technical characters). Call sites, when it's false, play
// "leggila sullo schermo" instead of reading out chunks of code or gibberish. Pure/testable.
bool nucleo_tts_text_speakable(const char *text, int max_chars);

// Composes the TIME (h 0..23, m 0..59) into a phrase EXACT TO THE MINUTE made ONLY of clips from the
// mandatory package (numbers 0..99 + "sono le"/"e"/"in punto"/"e un quarto"/"e mezza"/"meno un quarto"/"mezzogiorno"/
// "mezzanotte" for IT; "it is"/"o'clock"/"noon"/"midnight" for EN). 24h. NO ":"/zero-padding
// (which the planner would read as a pause/"zero"). The same text goes to BOTH the SCREEN and the VOICE, so it stays
// readable. lang "en" -> English, otherwise Italian. Pure/testable (see tts-time-ctest.c).
void nucleo_tts_speak_time(char *out, int n, int h, int m, const char *lang);

// "Speech-ifies" the math symbols in solver answers: '=' -> "uguale a"/"equals",
// '%' -> "per cento"/"percent", '^' -> "elevato"/"to the power of". Without this, '=' would trip the
// "looks like code" guard (everything falls back to "leggila") and '%'/'^' would be dropped (losing the meaning). The
// other symbols (/ · ² ³ ( ) π) remain as-is: if present, the utterance falls back to "leggila" (ohm,
// geometry). nucleo_tts_say()/say_or() already apply it internally. Pure/testable (tts-plan-ctest).
void nucleo_tts_mathspeak(const char *in, char *out, int n, const char *lang);

// Extracts the FIRST sentence (gist) of `in` into `out` (max n). "Mosaic voice" INNOVATION: descriptive/
// knowledge answers are long -> instead of "leggila" it speaks the short gist (the first
// sentence, usually the definition). nucleo_tts_say() then speaks it if covered by the pool, otherwise reads it.
// Pure/testable (tts-plan-ctest). Doesn't stop on decimals ("3.14") or abbreviations ("Dr.").
void nucleo_tts_first_sentence(const char *in, char *out, int n);

// Extracts from the translator's reply (`"<src>" in <lingua>: <target>.`) the translated WORD into `word`
// and its LANGUAGE ("en"/"it") into `lang`; returns true for the main form. This way the voice speaks
// the translation with the RIGHT index ("how do you say dog in Italian" -> "cane" in Italian) instead of
// "leggila" (the target is in the other language, not covered by the mono-language index). Pure/testable.
bool nucleo_tts_translate_word(const char *reply, char *word, int wn, char *lang, int ln);

// Extracts the numeric RESULT after the last "= " of a formula answer (geometry/physics) into `out`,
// if it's a CLEAN number (no units/symbols). This way the voice says "Il risultato e' 78.5398" instead of
// "leggila" on the symbol-dense formula. Returns true if found and clean. Pure/testable.
bool nucleo_tts_eq_result(const char *reply, char *out, int n);

// True if `text` has dense math typography (· ² ³ √ π Δ ½) = a formula (geometry/physics). Pure.
bool nucleo_tts_has_mathtypo(const char *text);

// ---- reading speed (zero-RAM: rate of the output WAV header) --------------------------
// The voice speeds up/slows down by changing the SAMPLE-RATE declared in the assembled WAV's header:
// the DAC scans the SAME samples faster (clips + pauses shrink together) — no
// buffer, no sample touched, ZERO RAM/CPU cost. Tape-speed: above 100% the pitch rises slightly
// (acceptable for "a bit faster"). Speed is a PERCENTAGE integer (100 = natural).
#define TTS_SPEED_MIN 70     // 0.70x (slower)
#define TTS_SPEED_MAX 160    // 1.60x (faster)
#define TTS_SPEED_DEF 110    // default: a touch faster than natural (users found it "a bit slow")
#define TTS_SPEED_STEP 5     // recommended step for sliders/▲▼

// Clamps the percentage to [TTS_SPEED_MIN, TTS_SPEED_MAX]. Pure/testable.
int nucleo_tts_speed_clamp(int pct);
// output I2S rate = base * pct/100, with pct clamped and the result kept within [8000,48000] Hz
// (safe limits for I2S; play_wav rejects out-of-range). Pure/testable (see tts-plan-ctest.c).
uint32_t nucleo_tts_speed_rate(uint32_t base_rate, int pct);

// ---- anti-click: smoothing the clip edges (concatenation quality) -------------------
// The package's clips are trim-silence + loudnorm -> they start/end at a
// non-zero amplitude level: when spliced together, there's a jump at the seams = an audible "tick"/click. The
// classic fix in concatenative synthesis is a short linear fade at both edges of EVERY clip, so the
// seam passes through ~0 and is smooth. ~16 samples @24kHz ≈ 0.7ms: kills the click without dulling
// the crispness of the sounds. Cost: scales in place the few edge samples of the already-existing
// streaming buffer (no RAM, no resampling, no "real" sample touched besides the edges).
#define TTS_DECLICK_SAMPLES 16

// Applies the fade to the clip edges working on ONE chunk of the stream: `buf`/`nbytes` is the current
// piece (mono 16-bit LE PCM), `chunk_off` its position in BYTES from the start of the clip, `clip_len`
// the clip's total bytes, `fade` the fade length in SAMPLES. Smooths the first and last `fade`
// samples of the clip wherever they fall within the chunk -> independent of how it's split into chunks. Sample-
// aligned (handles a chunk straddling the fade edge); no-op on clips that are too short. Pure/testable
// (alignment-safe via LE bytes, no cast to int16*). See tts-plan-ctest.c.
void nucleo_tts_declick_chunk(unsigned char *buf, int nbytes, uint32_t chunk_off, uint32_t clip_len, int fade);

// ---- firmware service (on-device only; see nucleo_tts.c) -----------------------------------

// Sets the default language and checks that the voice package is on SD (stat of n0.wav in
// /sd/data/tts/<lang>/). Idempotent. Nothing loaded into RAM: the clips' existence is checked
// at runtime with stat. Returns true if the voice for `lang` is installed.
bool nucleo_tts_init(const char *lang);

// Speaks `text` in `lang` (NULL/"" -> default language): plans it, and IF the utterance is
// entirely covered by clips (no unknown word) assembles the PCM into /sd/data/tts/_say.wav and
// plays it. OTHERWISE (an uncovered word -> would sound wrong) it doesn't try: it plays the canonical
// "leggila sullo schermo" phrase (clip "read_it"). This way the voice is NEVER wrong: it either says the right thing,
// or invites the user to read it. Interrupts any audio in progress. Non-blocking. False if the voice isn't installed.
bool nucleo_tts_say(const char *text, const char *lang);

// Like nucleo_tts_say(), but if `text` is NOT entirely speakable (uncovered content/word)
// speaks `fallback` (e.g. "Done") instead of "leggila sullo schermo". For operation CONFIRMATIONS
// (add a reminder, create a file...) where the OUTCOME matters, not the exact variable-content text.
bool nucleo_tts_say_or(const char *text, const char *fallback, const char *lang);

// Like nucleo_tts_say(), but if the text is NOT entirely speakable it stays SILENT and returns false —
// instead of playing "leggila sullo schermo". For callers reading several sentences that want just ONE read hint:
// call say_quiet for each sentence, fire read_hint ONCE the first time one returns false, then stop.
bool nucleo_tts_say_quiet(const char *text, const char *lang);

// Directly plays the "leggila sullo schermo" phrase (clip "read_it"). Call sites use it for
// answers that should NOT be spoken (knowledge, calculator): the user knows there's something to read.
bool nucleo_tts_read_hint(const char *lang);

// True if the voice for the default language is installed on SD.
bool nucleo_tts_available(void);

// "Speak when queried by the Cardputer" toggle (default ON). Persisted on SD
// (/sd/data/tts/speak.cfg). When OFF, nucleo_tts_say() is a no-op that returns false — so the
// on-device call sites (the ANIMA app, PTT voice) stay silent. Set by both the native Settings and the
// web (/api/tts). Does NOT touch the browser's web path (that one uses client-side speechSynthesis).
void nucleo_tts_set_enabled(bool on);
bool nucleo_tts_enabled(void);

// On-device reading speed, percentage (100 = natural; default TTS_SPEED_DEF). Persisted on
// SD (/sd/data/tts/speed.cfg). The value is clamped to [TTS_SPEED_MIN,TTS_SPEED_MAX]. set() invalidates the
// per-slug WAV cache (the rate is "baked" into the files): so the new speed applies immediately even to fixed clips.
// ALL surfaces set it (the web shell + Settings app + ANIMA web/native) through one single source.
void nucleo_tts_set_speed(int pct);
int  nucleo_tts_speed(void);

// Stops the current playback (delegates to nucleo_audio_stop).
void nucleo_tts_stop(void);

#ifdef __cplusplus
}
#endif
