// anima_person.c — fixed-record reader for data/anima/anima-person-ambig.bin (format: anima_person.h and
// tools/anima/build_person_ambig.mjs). Plain stdio, so the host gate runs this very file.
#include "anima_person.h"
#include "nucleo_board.h"      // NUCLEO_SD_MOUNT
#include <string.h>

#define AP_PATH  NUCLEO_SD_MOUNT "/data/anima/anima-person-ambig.bin"
#define AP_HDR   16
#define AP_SREC  24             // surname[20] + u16 off + u8 n + u8 0
#define AP_PREC  80             // given[32] + display[48]

static uint16_t le16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }

bool ap_open(ap_db_t *db)
{
    db->f = NULL; db->ns = db->np = 0;
    FILE *f = fopen(AP_PATH, "rb");
    if (!f) return false;
    setvbuf(f, NULL, _IONBF, 0);                  // record-sized reads: no stdio buffer on the heap
    uint8_t h[AP_HDR];
    if (fread(h, 1, sizeof h, f) != sizeof h || memcmp(h, "APAMB1\0\0", 8) != 0) { fclose(f); return false; }
    db->f = f; db->ns = le16(h + 8); db->np = le16(h + 10);
    return true;
}

void ap_close(ap_db_t *db)
{
    if (db->f) fclose(db->f);
    db->f = NULL; db->ns = db->np = 0;
}

static bool rec(ap_db_t *db, long at, uint8_t *buf, size_t n)
{
    return fseek(db->f, at, SEEK_SET) == 0 && fread(buf, 1, n, db->f) == n;
}

bool ap_surname(ap_db_t *db, const char *surname, uint16_t *off, uint8_t *n)
{
    if (!db->f || !surname || !surname[0]) return false;
    int lo = 0, hi = (int)db->ns - 1;
    uint8_t r[AP_SREC];
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (!rec(db, AP_HDR + (long)m * AP_SREC, r, sizeof r)) return false;
        r[AP_SURNAME_MAX - 1] = 0;
        int c = strcmp((const char *)r, surname);
        if (c == 0) {
            *off = le16(r + 20); *n = r[22];
            return *n > 0 && (uint32_t)*off + *n <= db->np;
        }
        if (c < 0) lo = m + 1; else hi = m - 1;
    }
    return false;
}

bool ap_person(ap_db_t *db, uint16_t idx, ap_person_t *p)
{
    if (!db->f || idx >= db->np) return false;
    long at = AP_HDR + (long)db->ns * AP_SREC + (long)idx * AP_PREC;
    if (!rec(db, at, (uint8_t *)p, sizeof *p)) return false;
    p->given[AP_GIVEN_MAX - 1] = 0; p->display[AP_DISPLAY_MAX - 1] = 0;
    return true;
}
