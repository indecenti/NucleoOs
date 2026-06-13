// Prova ESAUSTIVA del "deve dirmi l'ora precisa, sempre pronunciabile": per OGNI minuto del giorno
// (24x60 = 1440) compone l'orario con nucleo_tts_speak_time() e lo pianifica (nucleo_tts_plan) contro
// l'INDICE REALE delle clip (index.bin generato) -> ogni minuto deve dare 0 token UNKNOWN, altrimenti
// finirebbe in "leggila sullo schermo". Linka la C reale del firmware. Uso:
//   tts-time-ctest <it_index.bin> <en_index.bin>
#include "nucleo_tts.h"
#include "nucleo_tts_index.h"
#include <stdio.h>
#include <string.h>

static tts_index_t IX;
static bool has_clip(const char *slug, void *ud) {
    (void)ud; return slug && slug[0] && tts_index_find(&IX, slug, NULL, NULL);
}

// Ritorna i minuti scoperti per `lang`, o -1 se l'indice non e' apribile (SKIP).
static int check_lang(const char *idxp, const char *lang)
{
    if (!tts_index_open(&IX, idxp)) { printf("[%s] SKIP (indice non apribile: %s)\n", lang, idxp); return -1; }
    printf("[%s] indice %s: %u clip, %u Hz\n", lang, idxp, IX.count, IX.rate);
    int bad = 0, shown = 0;
    for (int h = 0; h < 24; h++) for (int m = 0; m < 60; m++) {
        char txt[64];
        nucleo_tts_speak_time(txt, sizeof txt, h, m, lang);

        // Coperto come risposta-fissa-intera? (es. "Mezzogiorno") -> ok.
        char full[48]; nucleo_tts_full_slug(txt, full, sizeof full);
        if (full[0] && tts_index_find(&IX, full, NULL, NULL)) continue;

        tts_token_t tok[96];
        int nt = nucleo_tts_plan(txt, lang, has_clip, NULL, tok, 96);
        char miss[160]; miss[0] = 0; int unk = 0;
        for (int i = 0; i < nt; i++) if (tok[i].kind == TTS_TOK_UNKNOWN) {
            unk++;
            if (strlen(miss) + strlen(tok[i].slug) + 2 < sizeof miss) { if (miss[0]) strcat(miss, ", "); strcat(miss, tok[i].slug); }
        }
        if (unk) { bad++; if (shown++ < 24) printf("  [%s] %02d:%02d -> \"%s\"  MANCA: %s\n", lang, h, m, txt, miss); }
    }
    tts_index_close(&IX);
    printf("[%s] %d/1440 minuti NON pronunciabili\n\n", lang, bad);
    return bad;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "uso: %s <it_index.bin> <en_index.bin>\n", argv[0]); return 2; }
    int bad = 0, ran = 0, r;
    if ((r = check_lang(argv[1], "it")) >= 0) { ran++; bad += r; }
    if ((r = check_lang(argv[2], "en")) >= 0) { ran++; bad += r; }
    if (!ran) { printf("Nessun indice da verificare (genera prima le clip).\n"); return 0; }
    printf(bad ? "FALLITO: %d minuti scoperti\n" : "OK: ogni minuto del giorno e' pronunciabile (0 scoperti)\n", bad);
    return bad ? 1 : 0;
}
