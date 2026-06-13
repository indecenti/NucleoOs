// On-device Push-To-Talk Voice Command Engine (AVCEB)
// Captures I2S PCM audio while the FN key is held, extracts MFCC features in real-time,
// and compares them against saved templates using Dynamic Time Warping (DTW).
// Emits events to nucleo_eventbus upon successful matches.
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the voice engine background task.
// Must be called after I2S and keyboard initialization.
esp_err_t nucleo_voice_init(void);

// Manual on/off pin (sticky). Brings the lazy 16 KB engine up on true, releases it
// on false if no other holder remains.
void nucleo_voice_enable(bool enable);

// App/UI hold: keep the engine up while a foreground app needs it; pass false on
// leave. The engine self-deletes (returning its 16 KB) when the last holder leaves.
void nucleo_voice_request(bool on);

// Push-to-Talk trigger from the GO side button, driven by the main UI loop (the single owner of
// the button timing). true = start listening now: brings the lazy engine up, sounds a polyphonic
// "ready" chime, then captures while held. false = stop and recognize. The engine is freed after
// the burst is processed (unless always-on or a foreground app keeps it pinned). The engine no
// longer polls the button itself, so a torch-tap and a talk-hold can never both fire on one press.
void nucleo_voice_ptt(bool on);

// Exclusive-mode temporary suspend: frees the engine without clearing holders, and
// restores it on suspend(false) only if a holder is still present.
void nucleo_voice_suspend(bool suspend);

// Persistent "always listen": sticky pin (engine up from boot) + writes voice.alwaysOn
// to the shared SD settings.json. Read at boot by nucleo_voice_init(). Survives reboot.
void nucleo_voice_set_always_on(bool on);
bool nucleo_voice_always_on(void);

// Triggers the "Learning Mode". The next time PTT is used, the engine will
// NOT perform a comparison, but instead save the extracted MFCC template
// under the given filename (e.g. "macro_L.tpl") in /sd/voice/.
// Returns ESP_OK if learning mode armed successfully.
esp_err_t nucleo_voice_arm_learning_mode(const char *template_filename);

// Check if the voice engine is currently capturing audio (PTT held).
bool nucleo_voice_is_listening(void);

// True while a Push-to-Talk session is in flight: from the GO-hold engaging the engine until the
// engine has fully finished (capture + recognition/dispatch + the final "done" beep) and released
// its transient hold. The UI loop polls this to keep the screen blanked + the 32 KB canvas freed
// for the WHOLE session — including the async dispatch AFTER the button is released — and to restore
// them only once the end beep has played. Distinct from is_listening(), which is true only while the
// mic is actually open (it goes false at button release, before recognition runs).
bool nucleo_voice_ptt_engaged(void);

// Retrieve the sentence recognized so far during the current PTT session.
void nucleo_voice_get_live_sentence(char *out, size_t max);

// ── Native-app introspection (POLL-based) ────────────────────────────────────
// The live event sink (voice/learned, voice/match, voice/state, voice/result) is
// single-consumer and owned by the WebSocket layer, so native apps cannot subscribe.
// These let a native app read the same signals by polling (e.g. from its on_tick).

// One-shot outcome of a learning capture, for the Voice Trainer's record flow:
// returns the pending result and clears it (so it fires exactly once).
typedef enum {
    NUCLEO_VOICE_LEARN_NONE = 0,   // nothing happened since the last poll
    NUCLEO_VOICE_LEARN_OK,         // template captured + saved
    NUCLEO_VOICE_LEARN_TOO_SHORT,  // PTT released with too little audio (or learn timed out)
} nucleo_voice_learn_t;
nucleo_voice_learn_t nucleo_voice_take_learn_result(void);

// Last per-word match telemetry (DTW distances). `seq` increments on every new match,
// so a poller can detect a fresh one. Returns false until the first match of the session.
typedef struct {
    char    word[32];
    int32_t dist, second, radius;
    uint32_t seq;
} nucleo_voice_match_t;
bool nucleo_voice_last_match(nucleo_voice_match_t *out);

// Last dispatched recognition: the fused sentence ANIMA resolved and what it did.
// `action` mirrors anima_action_t; `launched` is true only if an app was actually opened.
// `seq` increments per dispatch. Returns false until the first dispatch of the session.
typedef struct {
    char     sentence[96];
    char     reply[160];
    int      action;
    bool     matched;    // ANIMA produced a result (tier != NONE)
    bool     launched;   // an app was actually launched (false in test mode / on failure)
    uint32_t seq;
} nucleo_voice_result_t;
bool nucleo_voice_last_result(nucleo_voice_result_t *out);

// Test mode: while ON, recognition still matches and reports match/result telemetry, but the
// dispatch does NOT execute (no app launch, no on-device TTS, no WS routing) — so a "try it"
// surface can exercise the recognizer without side effects. Off by default; the native Voce
// console sets it on entry and clears it on exit.
void nucleo_voice_set_test_mode(bool on);
bool nucleo_voice_test_mode(void);

#ifdef __cplusplus
}
#endif
