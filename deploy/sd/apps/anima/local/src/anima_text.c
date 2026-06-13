// ANIMA pure text-utility tier  extracted from nucleo_anima.c for scalability.
// Stateless string processing the orchestrator leans on: bilingual typo correction
// (bounded Damerau-Levenshtein over the command vocabulary) and foreign-script output
// cleanup. No shared state, no SD/network  just libc. Exports in anima_internal.h.
#include "anima_internal.h"
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>

// ============================================================================
// Bilingual typo tolerance — a tiny autocorrect (the SymSpell idea at command scale).
// Each query word is matched against ANIMA's own command/operator vocabulary by bounded
// Damerau-Levenshtein (so swaps like "ofto"->"foto", "pre"->"per", "cpiare"->"capire" resolve).
// Applied ONLY when the cascade misses (correct-on-failure) -> queries that already work are
// never touched, so routing can't regress by construction. Knowledge typos are handled upstream
// by the L1 char-n-gram encoder (fastText-style), so this targets the brittle L0 layer.
// ============================================================================
static const char *SPELL_VOCAB[] = {
    "apri","aprire","apre","avvia","lancia","esegui","mostra","mostrami","vai","voglio","portami",
    "vedere","vedi","guarda","guardare","vorrei","fammi",
    "open","show","launch","run","start","avviare","aprilo","aprila","riapri","ripeti","rifai","ancora","again","repeat",
    "crea","creare","crei","nuovo","nuova","create","make","file","files","documento","documenti","document",
    "nota","note","appunti","blocco","testo","foglio","cartella",
    "excel","spreadsheet","fogli","tabella","celle","terminale","terminal","shell","console","bash",
    "foto","fotografie","immagini","immagine","galleria","photos","photo","pictures","gallery",
    "musica","canzoni","brani","audio","music","songs","lettore","player",
    "video","filmati","film","movies","movie","calcolatrice","calcoli","calculator","math",
    "orologio","sveglia","timer","clock","cronometro","impostazioni","settings","opzioni","options",
    "monitor","stato","risorse","status","telecomando","infrarossi","remote","registratore","voce","recorder","record",
    "cestino","trash","aggiornamenti","aggiorna","updates","update","giochi","gioco","emulatore","games","game",
    "sciame","swarm","automazioni","automazione","automation","calendario","agenda","calendar",
    "ora","ore","orario","adesso","time","spazio","disco","scheda","archiviazione","storage","space","batteria","carica","battery",
    "per","piu","meno","diviso","fratto","moltiplicato","times","plus","minus","divided",
    "radice","sqrt","root","elevato","potenza","percento","percent","metri","centimetri","grammi","minuti",
    "volt","ampere","ohm","corrente","tensione","resistenza","watt","celsius","fahrenheit","legge",
    "chi","sei","cosa","come","quanto","quale","aiuto","comandi","help","puoi","presentati",
    "dimmi","dammi","raccontami","spiegami","fammi","approfondisci","dettagli","esempio","continua","tell","explain","give",
    "primo","secondo","prima","seconda","first","second","quello","questo","quella","questa",
    "connesso","connessione","collegato","collegata","online","connected","wifi","rete","internet","network",
    "memoria","libera","libero","liberi","disponibile","occupato","versione","version","uptime","acceso","capacita",
    // valid command/temporal words that MUST NOT be "corrected" into a vocab neighbour (a word present
    // here is distance-0 -> left untouched). Fixes cancella->cartella, domani->comandi, weekday garble,
    // and also auto-corrects THEIR typos toward the right word.
    "cancella","cancellare","elimina","eliminare","rimuovi","annulla","cancel","delete","remove",
    "domani","oggi","ieri","dopodomani","stasera","stamattina","stanotte","tomorrow","today","yesterday","tonight",
    "lunedi","martedi","mercoledi","giovedi","venerdi","sabato","domenica","prossimo","prossima",
    "monday","tuesday","wednesday","thursday","friday","saturday","sunday","week","settimana","settimane",
    "ricordami","ricorda","promemoria","reminder","evento","eventi","appuntamento","riunione","incontro","scadenza","meeting","pianifica",
    "aggiungi","aggiunge","dottore","medico","dentista","dimenticare","dimenticarmi","forget","palestra","bollette","medicine",
};

// Damerau-Levenshtein distance capped at `max` (adjacent transposition = one edit). Short words.
// Exported via anima_internal.h: the solver reuses it to fuzzy-match the "cubica" root modifier.
int a_damlev(const char *a, const char *b, int max)
{
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la - lb > max || lb - la > max) return max + 1;
    if (lb > 38 || la > 38) return max + 1;
    int prev2[40], prev[40], cur[40];
    for (int j = 0; j <= lb; j++) { prev[j] = j; prev2[j] = 0; }
    for (int i = 1; i <= la; i++) {
        cur[0] = i; int best = i;
        for (int j = 1; j <= lb; j++) {
            int cost = (a[i-1] == b[j-1]) ? 0 : 1;
            int v = prev[j] + 1;
            if (cur[j-1] + 1 < v) v = cur[j-1] + 1;
            if (prev[j-1] + cost < v) v = prev[j-1] + cost;
            if (i > 1 && j > 1 && a[i-1] == b[j-2] && a[i-2] == b[j-1] && prev2[j-2] + 1 < v) v = prev2[j-2] + 1;
            cur[j] = v; if (v < best) best = v;
        }
        if (best > max) return max + 1;            // whole row over cap -> bail
        memcpy(prev2, prev, sizeof(int) * (lb + 1));
        memcpy(prev,  cur,  sizeof(int) * (lb + 1));
    }
    return prev[lb];
}

static bool a_transpose1(const char *a, const char *b)   // exactly one adjacent swap
{
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la != lb || la < 2) return false;
    int i = 0; while (i < la && a[i] == b[i]) i++;
    if (i >= la - 1) return false;
    if (a[i] == b[i+1] && a[i+1] == b[i]) { int k = i + 2; while (k < la && a[k] == b[k]) k++; return k == la; }
    return false;
}

// Best unambiguous vocab correction for a lowercase word, or NULL. The precision guard that
// matters: SHORT words (3-5) are corrected only on a single adjacent TRANSPOSITION (swapped
// letters), never a substitution — so a valid word like "che" is never "fixed" to "chi". Longer
// words (>=6), where a valid vocab neighbour is rare, also allow one insert/delete/substitute.
static const char *a_spell_word(const char *w)
{
    int len = (int)strlen(w);
    if (len < 3) return NULL;
    if (len < 6) {                                  // short: transposition only, unambiguous
        const char *cand = NULL; int n = 0;
        for (size_t i = 0; i < sizeof(SPELL_VOCAB) / sizeof(SPELL_VOCAB[0]); i++) {
            const char *v = SPELL_VOCAB[i];
            if (!strcmp(v, w)) return NULL;
            if (a_transpose1(w, v)) { cand = v; n++; }
        }
        return (n == 1) ? cand : NULL;
    }
    const char *best = NULL; int bestd = 3, ties = 0;   // long (>=6): Damerau-Levenshtein <= 2
    for (size_t i = 0; i < sizeof(SPELL_VOCAB) / sizeof(SPELL_VOCAB[0]); i++) {
        const char *v = SPELL_VOCAB[i];
        if (!strcmp(v, w)) return NULL;
        int d = a_damlev(w, v, 2);
        if (d < bestd) { bestd = d; best = v; ties = 0; }
        else if (d == bestd) ties++;
    }
    // The STEM must survive: a typo rarely hits the first 3 letters, so require them to match. Without this,
    // a plausible content word was rewritten into a COMMAND verb via two substitutions — "morira"->"mostra"
    // (-> launch code-runner), "ultimo"->"uptime" (-> uptime readout). Two stem-altering subs = a different
    // word, not a typo. Real command typos ("calcolatirce", "mostar") preserve the first 3 letters.
    // PREFIX guard: if the candidate is a strict prefix of the token (token = vocab + a 1-2 char suffix),
    // the token is a LONGER valid word, not a typo — "discord" is not a misspelling of "disco" (it would
    // misroute to SD-storage), "disconnetti"/"discoteca"-class likewise. A real typo perturbs the body.
    if (best) { size_t lb = strlen(best); if (lb < (size_t)len && !strncmp(w, best, lb)) return NULL; }
    // SAME-LENGTH DOUBLE-SUBSTITUTION guard: a real typo of a 6+ letter word is almost always ONE edit. Two
    // substitutions that KEEP the length don't fix a typo — they rewrite a valid word into a different one
    // ("planes"->"player" -> launch media-player, "morira"->"mostra"). So a same-length correction must be a
    // single edit (d<=1); only a LENGTH-CHANGING typo (a dropped or doubled letter) may spend the full d<=2.
    int maxd = (best && strlen(best) == (size_t)len) ? 1 : 2;
    return (best && bestd <= maxd && ties == 0 && !strncmp(w, best, 3)) ? best : NULL;
}

// Rewrite `raw`, correcting each word toward the vocab; numbers / dotted tokens (filenames) pass
// through untouched. Returns 1 if anything changed.
int a_spellfix(const char *raw, char *out, size_t outsz)
{
    int o = 0, changed = 0;
    for (const char *p = raw; *p; ) {
        if (*p == ' ' || *p == '\t') { if (o < (int)outsz - 1) out[o++] = ' '; p++; continue; }
        char tok[40]; int tl = 0; bool hasdigit = false, hasdot = false;
        while (*p && *p != ' ' && *p != '\t') {
            if (tl < 39) tok[tl++] = *p;
            if (isdigit((unsigned char)*p)) hasdigit = true;
            if (*p == '.') hasdot = true;
            p++;
        }
        tok[tl] = 0;
        char low[40]; int li = 0;
        for (int k = 0; k < tl && li < 39; k++) low[li++] = (char)tolower((unsigned char)tok[k]);
        low[li] = 0;
        const char *fix = (hasdigit || hasdot) ? NULL : a_spell_word(low);
        if (fix) changed = 1;
        for (const char *qq = fix ? fix : tok; *qq && o < (int)outsz - 1; qq++) out[o++] = *qq;
    }
    out[o] = 0;
    return changed;
}

// Remove foreign-script clutter (Arabic/Cyrillic/Hebrew/CJK transliterations) from a reply IN PLACE —
// the device can't render them and they bury the substance (e.g. an entity bio that leads with the name
// in Arabic). Keeps Latin, Greek (math π/λ), punctuation, symbols, em-dash. Universal output cleanup so
// even cards learned BEFORE the fetch-side cleanup still display clean. Only ever shrinks the string.
void a_strip_foreign(char *s)
{
    // 1) drop foreign SCRIPTS + standalone COMBINING marks (Russian stress ́ etc.) — keep Latin, Greek
    //    (math π/λ at 0xCE/0xCF), punctuation, symbols, em-dash.
    int o = 0; bool gap = false;
    for (unsigned char *p = (unsigned char *)s; *p; ) {
        unsigned char c = *p;
        int len = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        if (c == 0xCC ||                                  // U+0300–033F combining diacriticals (orphan accents)
            (c >= 0xD0 && c <= 0xDF) ||                   // Cyrillic/Arabic/Hebrew/Syriac/Thaana
            (c >= 0xE3 && c <= 0xED)) { p += len; gap = true; continue; }   // CJK/kana/Hangul
        if (gap && o > 0 && s[o-1] != ' ') s[o++] = ' ';
        gap = false;
        for (int k = 0; k < len && *p; k++) s[o++] = (char)*p++;
    }
    s[o] = 0;
    // 2) excise transliteration clauses "in arabo …" / "in russo …" (now emptied of script) up to the
    //    next clause boundary — otherwise we'd leave "in arabo  ?," / "in russo  ́ ," dangling.
    static const char *const langs[] = { " in arabo", " in russo", " in cinese", " in giapponese",
        " in ebraico", " in coreano", " in hindi", " in persiano", " in turco", " in sanscrito", " in aramaico", NULL };
    for (int i = 0; langs[i]; i++) {
        char *a;
        while ((a = strstr(s, langs[i])) != NULL) {
            char *e = a + strlen(langs[i]);
            while (*e && *e != ',' && *e != ';' && *e != '.' && *e != ')') e++;   // clause end
            memmove(a, e, strlen(e) + 1);
        }
    }
    // 3) tidy: collapse double spaces, drop " ," / " ;" / " )" and doubled commas, trim ends.
    int w = 0;
    for (int r = 0; s[r]; r++) {
        char c = s[r];
        if (c == ' ' && (w == 0 || s[w-1] == ' ')) continue;
        if ((c == ',' || c == ';' || c == ')') && w > 0 && s[w-1] == ' ') w--;
        if (c == ',' && w > 0 && s[w-1] == ',') continue;
        s[w++] = c;
    }
    while (w > 0 && (s[w-1] == ' ' || s[w-1] == ',')) w--;
    s[w] = 0;
}
