// On-device voice recorder: captures the Cardputer's PDM microphone via I2S RX
// and streams 16-bit PCM into a WAV file on the SD card (/data/Recordings).
//
// Design (matches the project's "device serves bytes, client does the heavy
// lifting" principle, see docs/media.md): the ESP only writes uncompressed WAV
// — trivial, ~tens of KB RAM, no codec. MP3 encoding happens in the browser
// (the Recorder web app), so the device never pays for an encoder.
#pragma once
#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>
#include <stdint.h>

// Initialise the I2S PDM RX channel for the built-in microphone. Safe to call
// once at boot; the channel stays disabled until a recording starts.
esp_err_t nucleo_recorder_init(void);

// Register /api/rec/{start,stop,status} on an existing HTTP server.
esp_err_t nucleo_recorder_register(httpd_handle_t server);

// ---- shared control API ----------------------------------------------------
// Drives the single mic/recorder used by BOTH the HTTP handlers and the native
// on-device Recorder app (app_recorder.cpp), so there is one source of truth and
// only one recording can run at a time.

// Begin a new WAV in /data/Recordings. Returns ESP_ERR_INVALID_STATE if a
// recording is already running or the mic failed to init, ESP_FAIL if the writer
// task could not start, ESP_OK otherwise.
esp_err_t nucleo_recorder_start(void);

// Ask the writer task to finalise the current WAV (header patched asynchronously).
void nucleo_recorder_stop(void);

// Wind down a live /api/rec/stream dictation worker and wait (bounded, <=3.5 s) for it to release
// its async request. MUST run before httpd stops, or the worker's send/complete is a use-after-free.
void nucleo_recorder_stream_abort(void);

// Who holds the single mic right now (the gate behind nucleo_recorder_start's CAS). Lets a caller tell
// "my own take is still finalizing" (REC) apart from "a web dictation stream owns it" (STREAM) instead
// of blaming the web for every refusal.
typedef enum { NUCLEO_MIC_IDLE = 0, NUCLEO_MIC_REC = 1, NUCLEO_MIC_STREAM = 2 } nucleo_mic_owner_t;
nucleo_mic_owner_t nucleo_recorder_owner(void);

// Non-blocking preempt: the physical operator at the keyboard outranks a remote web dictation. If a
// /api/rec/stream worker owns the mic, ask it to wind down (it releases within ~one socket-send timeout)
// and return false; the caller then polls nucleo_recorder_owner() and starts once it reads IDLE. Returns
// true if the mic is already free. A finalizing record-to-SD take (REC) is left alone (returns false).
bool nucleo_recorder_release_stream(void);

bool        nucleo_recorder_is_recording(void);
// True until the mic is FULLY released (I2S RX channel closed + WAV finalized) — this lingers ~one read
// timeout (≤200 ms) AFTER stop(), while is_recording() is already false. Speaker playback shares the I2S
// WS line, so anything that plays audio right after a recording must wait for this, not is_recording().
bool        nucleo_recorder_is_busy(void);
int         nucleo_recorder_level(void);     // last RMS level, 0..100 (for the meter)
uint32_t    nucleo_recorder_seconds(void);   // elapsed seconds of the active/last file
const char *nucleo_recorder_path(void);      // web path of the active/last file (/data/...)
