// nucleo_tts_index — indice clip RAM-light per la voce concatenativa.
//
// PERCHE': una cartella SD con decine di migliaia di file .wav (il dizionario) e' patologica su
// FATFS — aprire/stat-are un file = scansione lineare della directory (nessun indice nativo) =
// centinaia di ms e tanto I/O per parola. Invece: UN file indice ordinato (slug -> offset,len) +
// UN blob PCM con tutte le clip concatenate. Trovare una clip = ricerca binaria nell'indice via
// fseek (~log2(N) letture da 56 byte), RAM ~zero, CPU/I-O minimi. Stessa filosofia dello streaming
// centroidi L1. Puro stdio: host-compilabile e testabile (tools/anima-host/ttsidx-ctest.c).
//
// Formato index.bin (little-endian): "NTI1" | uint32 rate | uint32 count | count record ORDINATI
// per slug (strcmp byte-order): char slug[48] (null-pad) + uint32 off + uint32 len. (record = 56 B)
// clips.pcm: PCM mono 16-bit @rate, tutte le clip una dopo l'altra (niente header per-clip).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#define TTS_IDX_SLUG   48
#define TTS_IDX_REC    (TTS_IDX_SLUG + 8)   // slug[48] + off(4) + len(4)
#define TTS_IDX_HDR    12                   // magic(4) + rate(4) + count(4)

typedef struct { FILE *f; uint32_t count; uint32_t rate; } tts_index_t;

// Apre l'indice. Ritorna true e riempie ix se valido; false se assente/corrotto (voce non installata).
bool tts_index_open(tts_index_t *ix, const char *index_path);

// Ricerca binaria: true se lo slug esiste, riempiendo *off/*len (posizione nel blob clips.pcm).
bool tts_index_find(tts_index_t *ix, const char *slug, uint32_t *off, uint32_t *len);

void tts_index_close(tts_index_t *ix);
