// On-device audio playback: streams a WAV or MP3 from the SD card to the Cardputer
// speaker over I2S. WAV is sent as raw PCM (no decoder); MP3 is decoded with the Helix
// fixed-point decoder (managed component) — light enough for the ESP32-S3 without PSRAM.
//
// HARDWARE NOTE: the speaker shares the I2S word-select line (GPIO43) with the PDM mic
// clock (see nucleo_board.h), so playback and recording are mutually exclusive. The
// engine refuses to start while a recording is active and only claims the I2S TX pins
// while actually playing, freeing them on stop.
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start (or restart) playback of `path` (a /sd/... absolute path or web /data/... path;
// the caller passes whatever the FATFS open accepts). Returns ESP_ERR_INVALID_STATE if a
// recording is in progress. Decoder is chosen by extension (.mp3 vs .wav/other).
esp_err_t nucleo_audio_play(const char *path);

// Like nucleo_audio_play, but begin at start_ms into a clip of known duration_ms (pass 0/0
// to start from the top). Lets the video player resume where the viewer left off. The seek
// is byte-offset based — exact on CBR, slightly approximate on VBR MP3 — which is plenty
// given the loose A/V coupling. WAV start-seek is not supported (begins at 0).
esp_err_t nucleo_audio_play_at(const char *path, uint32_t start_ms, uint32_t duration_ms);

// Like nucleo_audio_play_at, but the MP3 is a WINDOW inside `path` starting at byte `file_off`
// for `file_len` bytes (the rest of the file is not audio). Used by the NFV v3 video player,
// which appends the clip's mono MP3 as the LAST section of the .nfv so one file holds both
// streams. The decoder seeks to file_off and reads to EOF (== the window's end, since it is the
// last section). start_ms/duration_ms work exactly as in play_at, relative to the window.
esp_err_t nucleo_audio_play_window(const char *path, uint32_t start_ms, uint32_t duration_ms,
                                   uint32_t file_off, uint32_t file_len);

// Stream and play a live MP3 radio over plain HTTP (Icecast-style endless stream). Decodes
// with Helix to the speaker and auto-reconnects if the stream drops. Pass an http:// URL.
// No seek/duration (it is live). nucleo_audio_stop() ends it.
esp_err_t nucleo_audio_play_url(const char *url);

// Seek the currently-playing MP3 to ms (needs the duration passed at play time). No-op if
// nothing is playing. The elapsed clock re-bases so A/V sync follows automatically.
void nucleo_audio_seek(uint32_t ms);

void nucleo_audio_stop(void);
// Block until the current playback FINISHES on its own (no-op if idle; capped at max_ms, then proceeds).
// Lets a caller SERIALIZE a heavy RAM op (e.g. ANIMA's calendar write) after the voice so the peaks
// don't stack. Unlike stop(), it does NOT cut the audio short.
void nucleo_audio_wait_idle(uint32_t max_ms);
bool nucleo_audio_playing(void);   // non-blocking: true se una clip/stream sta suonando (per pollare uno stop)
void nucleo_audio_toggle_pause(void);

// Optional reclaim hook. On this PSRAM-less chip the player task needs a CONTIGUOUS heap block for
// its stack; under fragmentation (e.g. the offline ANIMA index resident) xTaskCreate fails and the
// play is dropped in SILENCE — the "offline no voice" bug. If the first spawn fails, start_play calls
// this hook to hand a block back (the app layer registers nucleo_anima_l1_unload_if_idle), THEN
// retries. Step-wise: free, then play. nucleo_audio stays layer-clean (it never names ANIMA).
typedef void (*nucleo_audio_reclaim_fn)(void);
void nucleo_audio_set_reclaim_cb(nucleo_audio_reclaim_fn fn);

bool        nucleo_audio_is_playing(void);
bool        nucleo_audio_is_paused(void);
const char *nucleo_audio_path(void);       // currently/last played file path
uint32_t    nucleo_audio_elapsed(void);    // seconds played so far
uint32_t    nucleo_audio_elapsed_ms(void); // milliseconds played so far (sample-accurate; A/V master clock)
uint32_t    nucleo_audio_duration_ms(void); // ms total: exact when set (play_at), else estimated
                                            // from the running byte/time rate; 0 until playback warms up
int         nucleo_audio_progress(void);    // 0..100 (by decoded file position)

// Software output volume, 0..100 (%). Persists across tracks; applied to WAV and MP3.
void nucleo_audio_set_volume(int pct);
int  nucleo_audio_volume(void);

// Hard mute: silences output WITHOUT changing the volume value (the UI keeps showing it).
// Independent of pause; persists across seek and track changes until set false. Not auto-cleared
// on stop, so the owner (e.g. the video player) resets it when it exits.
void nucleo_audio_set_mute(bool muted);
bool nucleo_audio_is_muted(void);

// Pop-free fade-in: ramp the effective gain 0 -> current volume over dur_ms (<=0 => 150 ms).
// Call right after nucleo_audio_play/play_at to soften the click on the small speaker.
void nucleo_audio_fade_in(int dur_ms);

// One-shot procedural sound effect for games (short decaying noise "clack"). kind: 0 = low "throw"
// rumble, 1 = sharp "settle" clack. strength 5..100. No-op while a track plays; frees the I2S after.
// Blocks ~60-90 ms — call on discrete events (e.g. a dice throw/settle), never per frame.
void nucleo_audio_blip(int kind, int strength);

// One-shot square-wave TONE (a loud beep) on the speaker. freq 100..6000 Hz, ms 5..2000, strength 5..100.
// Honours the software volume, no-op while a track plays, opens/frees the I2S like blip(). Integer-only,
// with a short anti-pop fade. BLOCKS for ~ms — call on discrete events (alarm siren, timer beep), never
// per frame. Alternate two freqs across calls for a two-tone siren.
void nucleo_audio_tone(int freq, int ms, int strength);

// Continuous alarm SIREN: a harsh square wave WAILED 1.8->4.2 kHz at near-full amplitude. KEEPS the I2S
// TX open across calls (phase persists) so calling it back-to-back from an alarm loop is a TRULY gapless,
// piercing siren — far louder/nastier than repeated tone() beeps. Honours the software volume; no-op while
// a track plays. BLOCKS ~dur_ms. Call nucleo_audio_siren_stop() to silence and release the I2S (mic/recorder).
void nucleo_audio_siren(int dur_ms);
void nucleo_audio_siren_stop(void);

#ifdef __cplusplus
}
#endif
