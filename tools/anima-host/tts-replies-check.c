// Verifica END-TO-END del "si scatena correttamente?": carica l'INDICE REALE (index.bin generato) e
// per ogni risposta di ANIMA replica la decisione di nucleo_tts_say() — WHOLE-clip (risposta fissa),
// SPEAK (lista clip composte), o READ_IT (guardia, o parola NON coperta: la stampa). Linka la C reale
// del firmware (nucleo_tts_plan.c + nucleo_tts_index.c). Uso:
//   tts-replies-check <index.bin> <it|en> <replies.txt>
#include "nucleo_tts.h"
#include "nucleo_tts_index.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static tts_index_t IX;
static bool has_clip(const char *slug, void *ud) {
    (void)ud; return slug && slug[0] && tts_index_find(&IX, slug, NULL, NULL);
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "uso: %s <index.bin> <it|en> <replies.txt>\n", argv[0]); return 2; }
    const char *idxp = argv[1], *lang = argv[2], *file = argv[3];
    if (!tts_index_open(&IX, idxp)) { fprintf(stderr, "indice non apribile: %s\n", idxp); return 2; }
    printf("Indice %s: %u clip, %u Hz\n\n", idxp, IX.count, IX.rate);

    FILE *f = fopen(file, "rb");
    if (!f) { fprintf(stderr, "replies non apribile: %s\n", file); return 2; }
    char line[1024];
    int n = 0, speak = 0, whole = 0, guard = 0, uncov = 0;
    char missing[4000]; missing[0] = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line; while (*s == ' ' || *s == '\t') s++;
        size_t L = strlen(s); while (L && (s[L-1]=='\n'||s[L-1]=='\r')) s[--L]=0;
        if (!L || s[0] == '#') continue;
        n++;
        // Rispecchia nucleo_tts_say(): "parlabilizza" = % ^ PRIMA di decidere (la guardia non blocca piu'
        // su '=', il planner vede "per cento"/"elevato"). Cosi' il gate misura cio' che il device dice davvero.
        char sp[1024]; nucleo_tts_mathspeak(s, sp, sizeof sp, lang); s = sp;
        char full[48]; nucleo_tts_full_slug(s, full, sizeof full);
        if (full[0] && tts_index_find(&IX, full, NULL, NULL)) { whole++; printf("  WHOLE  %s\n", s); continue; }
        if (!nucleo_tts_text_speakable(s, 140)) { guard++; printf("  READ*  %-50s  (guardia: lungo/codice)\n", s); continue; }
        tts_token_t tok[96];
        int m = nucleo_tts_plan(s, lang, has_clip, NULL, tok, 96);
        int unk = 0; char miss[256]; miss[0]=0;
        for (int i = 0; i < m; i++) if (tok[i].kind == TTS_TOK_UNKNOWN) {
            unk++; if (strlen(miss)+strlen(tok[i].slug)+2 < sizeof miss) { if(miss[0])strcat(miss,", "); strcat(miss,tok[i].slug); }
        }
        if (unk) { uncov++; printf("  READ?  %-50s  MANCA: %s\n", s, miss);
                   if (strlen(missing)+strlen(miss)+2 < sizeof missing) { if(missing[0])strcat(missing,", "); strcat(missing,miss); } }
        else     { speak++; printf("  SPEAK  %s\n", s); }
    }
    fclose(f); tts_index_close(&IX);
    printf("\n%d risposte: SPEAK %d, WHOLE %d, READ(guardia) %d, READ(scoperte) %d\n", n, speak, whole, guard, uncov);
    if (uncov) printf("PAROLE DA AGGIUNGERE: %s\n", missing);
    return 0;
}
