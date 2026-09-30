// nucleo_tts_index — RAM-light clip index for the concatenative voice.
//
// WHY: an SD folder with tens of thousands of .wav files (the dictionary) is pathological on
// FATFS — opening/stat-ing a file = a linear directory scan (no native index) =
// hundreds of ms and heavy I/O per word. Instead: ONE sorted index file (slug -> offset,len) +
// ONE PCM blob with all the clips concatenated. Finding a clip = a binary search in the index via
// fseek (~log2(N) reads of 56 bytes), ~zero RAM, minimal CPU/I-O. Same philosophy as the L1
// centroid streaming. Pure stdio: host-compilable and testable (tools/anima-host/ttsidx-ctest.c).
//
// index.bin format (little-endian): "NTI1" | uint32 rate | uint32 count | count records SORTED
// by slug (byte-order strcmp): char slug[48] (null-pad) + uint32 off + uint32 len. (record = 56 B)
// clips.pcm: mono 16-bit PCM @rate, all clips back-to-back (no per-clip header).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#define TTS_IDX_SLUG   48
#define TTS_IDX_REC    (TTS_IDX_SLUG + 8)   // slug[48] + off(4) + len(4)
#define TTS_IDX_HDR    12                   // magic(4) + rate(4) + count(4)

typedef struct { FILE *f; uint32_t count; uint32_t rate; } tts_index_t;

// Opens the index. Returns true and fills ix if valid; false if missing/corrupt (voice not installed).
bool tts_index_open(tts_index_t *ix, const char *index_path);

// Binary search: true if the slug exists, filling *off/*len (position in the clips.pcm blob).
bool tts_index_find(tts_index_t *ix, const char *slug, uint32_t *off, uint32_t *len);

void tts_index_close(tts_index_t *ix);
