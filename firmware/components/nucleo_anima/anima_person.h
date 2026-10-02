// anima_person.h — the WHICH-PERSON table (surnames shared by people in the knowledge corpus), read from
// the SD: data/anima/anima-person-ambig.bin, generated from the corpus by tools/anima/build_person_ambig.mjs
// and shipped in the same verified SD payload as the knowledge index it describes.
//
// It used to be compiled in (42 KB of flash); on the SD it costs neither flash nor heap: fixed-size
// records, binary-searched with fseek, one record at a time on the caller's stack. Without the file
// (no SD, older SD content) every lookup simply fails: ANIMA answers as it would without the table.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define AP_SURNAME_MAX 20     // record field sizes, NUL included (must match the generator)
#define AP_GIVEN_MAX   32
#define AP_DISPLAY_MAX 48

typedef struct {
    char given[AP_GIVEN_MAX];       // space-separated given names, folded ASCII ("donald jr.")
    char display[AP_DISPLAY_MAX];   // the full display name, UTF-8 ("Donald Trump Jr.")
} ap_person_t;

typedef struct { FILE *f; uint16_t ns, np; } ap_db_t;

// Open the table (header validated). false: no file / not this format — every lookup then fails.
bool ap_open(ap_db_t *db);
void ap_close(ap_db_t *db);

// Binary search for a folded surname. true: the group of people carrying it, as [off, off+n).
bool ap_surname(ap_db_t *db, const char *surname, uint16_t *off, uint8_t *n);

// Person `idx` (0..np-1).
bool ap_person(ap_db_t *db, uint16_t idx, ap_person_t *p);
