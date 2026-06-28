// Internal contract shared between the engine (nucleo_audio.c) and the MP3 decode
// loop (nucleo_audio_mp3.c). Not part of the public API.
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

// Shared MP3 decode scratch: one input ring + one PCM output buffer. The file decoder
// (nucleo_audio_mp3.c) and the radio decoder (nucleo_audio_http.c) are mutually exclusive
// — only the single audio player task ever runs, and only one of them at a time — so they
// share ONE buffer set instead of each owning a static .bss copy. Frees ~6.6 KB of
// permanent .bss for the PSRAM-less heap (the ANIMA L1 centroid slab needs a large
// contiguous block, and shrinking .bss grows the heap arena at its edge). Defined once in
// nucleo_audio.c.
#define NUCLEO_AUDIO_IN_SZ   2048             // input ring (>= one MP3 frame, 1441 max)
#define NUCLEO_AUDIO_OUT_SZ  (1152 * 2)       // max PCM samples per frame (1152 * 2 ch)
extern uint8_t nucleo_audio_in[NUCLEO_AUDIO_IN_SZ];
extern int16_t nucleo_audio_out[NUCLEO_AUDIO_OUT_SZ];

// (Re)configure the I2S TX clock/slots for a stream. Safe to call again on a rate
// change mid-file; lazily creates the channel on first use.
esp_err_t nucleo_audio_i2s_rate(int sample_rate, int channels);

// Write interleaved 16-bit PCM to the speaker (blocking, honours stop).
esp_err_t nucleo_audio_i2s_write(const int16_t *pcm, size_t bytes);

// Cooperative transport: returns false once a stop is requested; blocks while paused.
bool nucleo_audio_keep_running(void);

// Progress accounting (drives elapsed/duration/progress getters).
void nucleo_audio_set_total_bytes(uint32_t bytes);     // file size, for the progress %
void nucleo_audio_add_file_bytes(uint32_t bytes);      // bytes consumed from the file
void nucleo_audio_add_samples(uint32_t frames, int rate);  // output frames, for elapsed

// Seek support for the decode loop. If a seek (initial resume-start or live) is pending,
// returns true and sets *byte_off to the file position to jump to; the engine has already
// re-based the elapsed clock. The decode loop must fseek there and flush its input buffer.
bool nucleo_audio_poll_seek(long *byte_off);

// WDT-safe absolute file positioning. FATFS here is built WITHOUT fast-seek, so fseek(SEEK_SET) to a
// far offset walks the cluster chain O(distance) in one uninterruptible blocking call — a multi-second
// freeze on a multi-MB MP3 that trips the watchdog and reboots (the resume-reboot). Hop forward in
// bounded steps, petting the WDT between hops. Mirrors the video player's seek_far.
void nucleo_audio_seek_far(FILE *f, long off);

// Decode an MP3 stream (already-open file) to the speaker. Implemented in
// nucleo_audio_mp3.c so the Helix dependency stays isolated.
void nucleo_audio_play_mp3(FILE *f);

// Pull + decode a live MP3 radio stream over HTTP until stop is requested (auto-reconnects).
// Implemented in nucleo_audio_http.c. `url` must outlive the call (engine owns s_path).
void nucleo_audio_stream_url(const char *url);
