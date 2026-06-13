// Prova host del retrieval indicizzato (firmware/components/nucleo_tts/nucleo_tts_index.c): crea un
// index.bin con record ORDINATI, poi verifica che la ricerca binaria del codice C reale del device
// trovi gli slug giusti (off/len) e rifiuti quelli assenti. Prova che "il Cardputer recupera bene".
// Build+run: vedi tools/anima-host/tts-check.mjs (npm run anima:tts).
#include "nucleo_tts_index.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_pass = 0, g_fail = 0;
static void ok(const char *what, int cond) {
    if (cond) { g_pass++; printf("  ok   %s\n", what); }
    else      { g_fail++; printf("  FAIL %s\n", what); }
}

// scrive un record da 56 byte: slug[48] null-pad + off + len (little-endian)
static void wrec(FILE *f, const char *slug, unsigned off, unsigned len) {
    char s[48]; memset(s, 0, sizeof s); strncpy(s, slug, 47);
    fwrite(s, 1, 48, f);
    unsigned char b[8] = { off&255,(off>>8)&255,(off>>16)&255,(off>>24)&255,
                           len&255,(len>>8)&255,(len>>16)&255,(len>>24)&255 };
    fwrite(b, 1, 8, f);
}
static void wu32(FILE *f, unsigned v) { unsigned char b[4]={v&255,(v>>8)&255,(v>>16)&255,(v>>24)&255}; fwrite(b,1,4,f); }

int main(void) {
    printf("TTS index — ricerca binaria su index.bin\n");
    const char *path = "build/_ttsidx_test.bin";
    // slug ORDINATI per strcmp byte-order (come fa build_index.py): cifre < '_' < minuscole
    struct { const char *slug; unsigned off, len; } rec[] = {
        {"buona_sera", 100, 50}, {"ciao", 0, 30}, {"n23", 30, 40}, {"n9", 70, 30}, {"zebra", 150, 60},
    };
    int n = (int)(sizeof rec / sizeof rec[0]);
    FILE *f = fopen(path, "wb");
    if (!f) { printf("FAIL cannot create %s\n", path); return 1; }
    fwrite("NTI1", 1, 4, f); wu32(f, 24000); wu32(f, (unsigned)n);
    for (int i = 0; i < n; i++) wrec(f, rec[i].slug, rec[i].off, rec[i].len);
    fclose(f);

    tts_index_t ix;
    ok("open", tts_index_open(&ix, path));
    ok("rate=24000", ix.rate == 24000);
    ok("count=5", ix.count == 5);

    uint32_t off = 0, len = 0;
    ok("find ciao",       tts_index_find(&ix, "ciao", &off, &len) && off==0   && len==30);
    ok("find n23",        tts_index_find(&ix, "n23", &off, &len)  && off==30  && len==40);
    ok("find n9",         tts_index_find(&ix, "n9", &off, &len)   && off==70  && len==30);
    ok("find buona_sera", tts_index_find(&ix, "buona_sera", &off, &len) && off==100 && len==50);
    ok("find zebra",      tts_index_find(&ix, "zebra", &off, &len) && off==150 && len==60);
    ok("miss 'pippo'",   !tts_index_find(&ix, "pippo", &off, &len));
    ok("miss 'n2'",      !tts_index_find(&ix, "n2", &off, &len));     // prefisso non e' match
    ok("miss empty",     !tts_index_find(&ix, "", &off, &len));
    tts_index_close(&ix);
    remove(path);

    printf("\n%d ok, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
