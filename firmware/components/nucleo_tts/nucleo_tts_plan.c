// nucleo_tts_plan — il "cervello" del TTS concatenativo: testo italiano -> sequenza di token
// CLIP/PAUSE. Puro (string.h/ctype.h), allocation-free, host-compilabile: vedi
// tools/anima-host/tts-plan-ctest.c per la prova. Logica documentata in nucleo_tts.h.
#include "nucleo_tts.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdint.h>

// RAM-safe: il testo e' limitato a 220 char dalla guardia (nucleo_tts_text_speakable), quindi non puo'
// produrre piu' di ~160 unita'/token. u[MAX_UNITS] vive sullo stack del voice_task (16KB): 160 e' il
// ceiling sicuro (coesiste con norm[1024]). Tenuto == TOK_MAX in nucleo_tts.c.
#define MAX_UNITS   160
#define MAX_PHRASE   6     // parole massime in un match a frase (MOSAICO-style)

// Cardinali italiani round, pre-renderizzati come clip atomiche (pacchetto numeri obbligatorio).
static const char *HUNDREDS[10] = {
    "", "cento", "duecento", "trecento", "quattrocento", "cinquecento",
    "seicento", "settecento", "ottocento", "novecento"
};
static const char *THOUSANDS[10] = {
    "", "mille", "duemila", "tremila", "quattromila", "cinquemila",
    "seimila", "settemila", "ottomila", "novemila"
};

// ---- normalizzazione: UTF-8 -> ASCII minuscolo, fold accenti italiani --------------------------
static void normalize_ascii(const char *in, char *out, int cap)
{
    int o = 0;
    for (int i = 0; in[i] && o < cap - 1; ) {
        unsigned char c = (unsigned char)in[i];
        if (c == 0xC3 && in[i + 1]) {               // accenti latin-1 supplement (à è é ì ò ù ç ñ ...)
            unsigned char b = (unsigned char)in[i + 1];
            char f = 0;
            if      ((b >= 0x80 && b <= 0x85) || (b >= 0xA0 && b <= 0xA5)) f = 'a';
            else if ((b >= 0x88 && b <= 0x8B) || (b >= 0xA8 && b <= 0xAB)) f = 'e';
            else if ((b >= 0x8C && b <= 0x8F) || (b >= 0xAC && b <= 0xAF)) f = 'i';
            else if ((b >= 0x92 && b <= 0x96) || (b >= 0xB2 && b <= 0xB6)) f = 'o';
            else if ((b >= 0x99 && b <= 0x9C) || (b >= 0xB9 && b <= 0xBC)) f = 'u';
            else if (b == 0x87 || b == 0xA7) f = 'c';      // ç
            else if (b == 0x91 || b == 0xB1) f = 'n';      // ñ
            if (f) out[o++] = f;
            i += 2;
        } else if (c < 0x80) {
            out[o++] = (char)tolower(c);
            i += 1;
        } else {
            i += 1;                                  // altro multibyte: scarta il byte
        }
    }
    out[o] = 0;
}

// Come normalize_ascii ma PRESERVA il case delle lettere ASCII (à->a, À->A): serve a riconoscere gli
// acronimi tutto-maiuscolo (USB/GPS) da sillabare. Lo slug poi e' sempre minuscolo (il lex fa tolower).
static void normalize_keepcase(const char *in, char *out, int cap)
{
    int o = 0;
    for (int i = 0; in[i] && o < cap - 1; ) {
        unsigned char c = (unsigned char)in[i];
        if (c == 0xC3 && in[i + 1]) {               // accenti latin-1: 0x80-0x9F maiuscole, 0xA0-0xBF minuscole
            unsigned char b = (unsigned char)in[i + 1];
            char f = 0; int up = 0;
            if      (b >= 0x80 && b <= 0x85) { f = 'a'; up = 1; } else if (b >= 0xA0 && b <= 0xA5) f = 'a';
            else if (b >= 0x88 && b <= 0x8B) { f = 'e'; up = 1; } else if (b >= 0xA8 && b <= 0xAB) f = 'e';
            else if (b >= 0x8C && b <= 0x8F) { f = 'i'; up = 1; } else if (b >= 0xAC && b <= 0xAF) f = 'i';
            else if (b >= 0x92 && b <= 0x96) { f = 'o'; up = 1; } else if (b >= 0xB2 && b <= 0xB6) f = 'o';
            else if (b >= 0x99 && b <= 0x9C) { f = 'u'; up = 1; } else if (b >= 0xB9 && b <= 0xBC) f = 'u';
            else if (b == 0x87)              { f = 'c'; up = 1; } else if (b == 0xA7)              f = 'c';
            else if (b == 0x91)              { f = 'n'; up = 1; } else if (b == 0xB1)              f = 'n';
            if (f) out[o++] = up ? (char)(f - 32) : f;
            i += 2;
        } else if (c < 0x80) {
            out[o++] = (char)c;                      // PRESERVA il case (niente tolower qui)
            i += 1;
        } else {
            i += 1;
        }
    }
    out[o] = 0;
}

// ---- unita' intermedie (parola / numero / pausa) ----------------------------------------------
typedef enum { U_WORD = 0, U_NUMBER, U_PAUSE } unit_kind_t;
typedef struct { unit_kind_t kind; char s[48]; int ms; bool allcaps; } unit_t;

static bool is_decimal_dot(const char *a, int len, const char *p)
{
    // ',' o '.' e' separatore decimale solo se sta TRA due cifre.
    return len > 0 && isdigit((unsigned char)a[len - 1]) && isdigit((unsigned char)p[1]);
}

// Spezza la stringa ASCII normalizzata in unita'. La punteggiatura di fine frase/inciso diventa
// PAUSE; un numero (con segno e/o decimale) resta una sola unita' NUMBER; il resto sono WORD.
// Spezza la stringa NORMALIZZATA-keepcase (case preservato sulle lettere ASCII) in unita'. Salva lo slug
// SEMPRE minuscolo (tolower) e marca `allcaps` se la parola e' fatta di >=2 lettere TUTTE maiuscole
// (acronimo: USB/GPS; le cifre interne non contano, cosi' "MP3" resta allcaps).
static int lex_units(const char *a, unit_t *u, int max)
{
    int n = 0, ci = 0, al = 0, up = 0;
    char cur[48];
    char curkind = 0;   // 0 = vuoto, 'w' = parola, 'd' = numerico
    #define FLUSH() do { if (ci) { cur[ci] = 0; if (n < max) { \
        u[n].kind = (curkind == 'd') ? U_NUMBER : U_WORD; \
        strncpy(u[n].s, cur, sizeof(u[n].s) - 1); u[n].s[sizeof(u[n].s) - 1] = 0; u[n].ms = 0; \
        u[n].allcaps = (curkind == 'w' && al >= 2 && up == al); n++; } \
        ci = 0; curkind = 0; al = 0; up = 0; } } while (0)
    #define PAUSE(d) do { if (n < max) { u[n].kind = U_PAUSE; u[n].s[0] = 0; u[n].ms = (d); u[n].allcaps = false; n++; } } while (0)

    for (int i = 0; a[i]; i++) {
        char c = a[i];
        if (isdigit((unsigned char)c)) {
            if (ci == 0) curkind = 'd';
            if (ci < (int)sizeof(cur) - 1) cur[ci++] = c;
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            if (ci == 0) curkind = 'w';
            al++; if (c >= 'A' && c <= 'Z') up++;        // conteggio per il flag acronimo
            if (ci < (int)sizeof(cur) - 1) cur[ci++] = (char)tolower((unsigned char)c);
        } else if (c == '-') {
            // segno negativo solo all'inizio di un numero (cur vuoto e cifra subito dopo)
            if (ci == 0 && isdigit((unsigned char)a[i + 1])) { curkind = 'd'; cur[ci++] = '-'; }
            else FLUSH();
        } else if (c == ',' || c == '.') {
            if (curkind == 'd' && is_decimal_dot(cur, ci, &a[i])) {
                if (ci < (int)sizeof(cur) - 1) cur[ci++] = ',';   // canonicalizza il decimale a ','
            } else {
                FLUSH();
                PAUSE(c == '.' ? 320 : 160);
            }
        } else if (c == '!' || c == '?') {
            FLUSH(); PAUSE(360);
        } else if (c == ';' || c == ':') {
            FLUSH(); PAUSE(220);
        } else {
            FLUSH();   // spazio o altra punteggiatura: separatore senza pausa
        }
    }
    FLUSH();
    #undef FLUSH
    #undef PAUSE
    return n;
}

// ---- emissione token --------------------------------------------------------------------------
typedef struct { tts_token_t *out; int max; int n; bool en; } sink_t;

static void push_clip(sink_t *s, const char *slug, bool fb)
{
    if (s->n >= s->max || !slug || !slug[0]) return;
    s->out[s->n].kind = TTS_TOK_CLIP;
    strncpy(s->out[s->n].slug, slug, sizeof(s->out[s->n].slug) - 1);
    s->out[s->n].slug[sizeof(s->out[s->n].slug) - 1] = 0;
    s->out[s->n].ms = 0;
    s->out[s->n].fallback = fb;
    s->n++;
}
static void push_pause(sink_t *s, int ms)
{
    if (s->n >= s->max || ms <= 0) return;
    s->out[s->n].kind = TTS_TOK_PAUSE;
    s->out[s->n].slug[0] = 0;
    s->out[s->n].ms = ms;
    s->out[s->n].fallback = false;
    s->n++;
}
static void push_num(sink_t *s, int v, bool fb)   // clip "nV" (atomici 0..99)
{
    char b[8]; int o = 0; b[o++] = 'n';
    if (v >= 10) b[o++] = (char)('0' + v / 10);
    b[o++] = (char)('0' + v % 10);
    b[o] = 0; push_clip(s, b, fb);
}

// > 999999 (o overflow): lettura cifra-per-cifra (fallback grazioso), dalla piu' significativa.
static void emit_big_digits(sink_t *s, long v)
{
    if (v < 0) v = -v;
    char rev[24]; int rn = 0; long t = v;
    while (t > 0 && rn < (int)sizeof(rev)) { rev[rn++] = (char)('0' + t % 10); t /= 10; }
    if (rn == 0) { push_num(s, 0, true); return; }
    for (int i = rn - 1; i >= 0; i--) push_num(s, rev[i] - '0', true);
}

// Intero -> cardinale ITALIANO. 0..99 atomico (risolve l'agglutinazione: ventuno/ventotto);
// 101..999 = "duecento.." + resto; 1000..999999 = "mille"/"duemila.." + resto (1984 -> "mille"
// "novecento" "ottantaquattro", segmentazione naturale per gli anni). Niente "e" interno.
static void emit_int_it(sink_t *s, long v)
{
    if (v < 0) v = -v;
    if (v < 100)     { push_num(s, (int)v, false); return; }   // 0..99 atomici; 100 -> "cento"
    if (v < 1000)    { int h = (int)(v / 100), r = (int)(v % 100);
                       push_clip(s, HUNDREDS[h], false); if (r) emit_int_it(s, r); return; }
    if (v < 1000000) { int th = (int)(v / 1000), r = (int)(v % 1000);
                       if (th == 1)      push_clip(s, "mille", false);
                       else if (th <= 9) push_clip(s, THOUSANDS[th], false);
                       else { emit_int_it(s, th); push_clip(s, "mila", false); }
                       if (r) emit_int_it(s, r);
                       return; }
    emit_big_digits(s, v);
}

// Intero -> cardinale INGLESE. 0..99 atomico ("twenty-three"); poi composizione "two hundred
// thirty-four", "one thousand nine hundred eighty-four". Connettori "hundred"/"thousand".
static void emit_int_en(sink_t *s, long v)
{
    if (v < 0) v = -v;
    if (v < 100)     { push_num(s, (int)v, false); return; }
    if (v < 1000)    { int h = (int)(v / 100), r = (int)(v % 100);
                       push_num(s, h, false); push_clip(s, "hundred", false); if (r) emit_int_en(s, r); return; }
    if (v < 1000000) { int th = (int)(v / 1000), r = (int)(v % 1000);
                       emit_int_en(s, th); push_clip(s, "thousand", false); if (r) emit_int_en(s, r); return; }
    emit_big_digits(s, v);
}

// Unita' NUMBER ("-?digits(,digits)?") -> clip. Segno -> "meno"/"minus"; decimale -> "virgola"/"point"
// + cifre. La lingua (s->en) sceglie cardinali e connettori.
static void emit_number(sink_t *s, const char *str)
{
    const char *p = str;
    if (*p == '-') { push_clip(s, s->en ? "minus" : "meno", false); p++; }
    long ip = 0; int ndig = 0;
    while (*p && *p != ',' && ndig < 12) { if (*p >= '0' && *p <= '9') { ip = ip * 10 + (*p - '0'); ndig++; } p++; }
    if (ndig > 6) {                                  // troppo grande per il valore: cifra-per-cifra
        const char *q = str; if (*q == '-') q++;
        for (; *q && *q != ','; q++) if (*q >= '0' && *q <= '9') push_num(s, *q - '0', true);
    } else if (s->en) emit_int_en(s, ip);
    else              emit_int_it(s, ip);
    if (*p == ',') {                                 // parte decimale: "virgola"/"point" poi cifre
        push_clip(s, s->en ? "point" : "virgola", false);
        for (p++; *p; p++) if (*p >= '0' && *p <= '9') push_num(s, *p - '0', false);
    }
}

// Parola ignota (nessuna clip): emette un token UNKNOWN, conservando la PAROLA (per diagnostica:
// dice esattamente cosa manca). NON si prova lo spelling; sara' nucleo_tts_say() a suonare "leggila".
static void push_unknown(sink_t *s, const char *word)
{
    if (s->n >= s->max) return;
    s->out[s->n].kind = TTS_TOK_UNKNOWN;
    snprintf(s->out[s->n].slug, sizeof s->out[s->n].slug, "%s", word ? word : "");
    s->out[s->n].ms = 0;
    s->out[s->n].fallback = true;
    s->n++;
}

// ---- sillabazione ACRONIMI: USB/GPS/MP3 (tutto-maiuscolo, non coperti) -> lettera-per-lettera ----
// Una sigla tutto-maiuscolo che NON ha una clip propria si dice compitando: ogni lettera con la clip
// lett_<x> ("u esse bi"), ogni cifra col cardinale nN. Tutto-o-niente: emette SOLO se OGNI lettera ha
// la sua clip (altrimenti -> UNKNOWN -> "leggila"), cosi' prima del deploy delle clip lettera il
// comportamento resta invariato. Solo 2..6 caratteri (le sigle reali; oltre, compitare suona peggio
// di "leggila"). `w` e' gia' minuscolo (lo slug); il flag allcaps sul case originale l'ha gia' deciso.
#define TTS_ACRO_MIN 2
#define TTS_ACRO_MAX 6
static bool spell_acronym(sink_t *s, const char *w, tts_has_clip_fn has, void *ud)
{
    int len = (int)strlen(w);
    if (len < TTS_ACRO_MIN || len > TTS_ACRO_MAX || !has) return false;
    bool any_letter = false;
    for (int k = 0; k < len; k++) {                  // copertura PRIMA di emettere (niente sillabe parziali)
        char c = w[k];
        if (c >= '0' && c <= '9') continue;          // cifra -> nN (sempre nel pacchetto obbligatorio)
        if (c < 'a' || c > 'z') return false;        // carattere non sillababile
        char slug[7] = { 'l', 'e', 't', 't', '_', c, 0 };
        if (!has(slug, ud)) return false;            // manca la clip lettera -> niente spelling
        any_letter = true;
    }
    if (!any_letter) return false;                   // solo cifre (es. "00"): lascia la via numerica
    for (int k = 0; k < len; k++) {
        char c = w[k];
        if (c >= '0' && c <= '9') push_num(s, c - '0', true);
        else { char slug[7] = { 'l', 'e', 't', 't', '_', c, 0 }; push_clip(s, slug, true); }
    }
    return true;
}

// ---- planner COMPOSIZIONALE: scala la copertura SENZA nuove clip ------------------------------
// Una parola non coperta (composti/derivati: "portacenere", "buonasera", "watercolor", "autostrada")
// viene SCOMPOSTA in 2..3 sotto-parole gia' coperte (prefisso PIU' LUNGO prima + backtracking; ogni
// parte >= 3 char per evitare frammentazione assurda). Cosi' migliaia di parole composte/derivate si
// pronunciano concatenando clip esistenti, a costo ZERO file. Sicuro: scatta SOLO su parole non coperte
// e SOLO se l'INTERA parola e' fatta di clip reali (>=3 char) -> niente spelling casuale, o UNKNOWN.
#define TTS_DECOMP_MINSUB   3
#define TTS_DECOMP_MAXPARTS 3
static int tts_decompose(const char *w, int len, tts_has_clip_fn has, void *ud, char parts[][48], int maxp)
{
    if (len == 0) return 0;                                  // consumata tutta -> successo
    if (maxp <= 0) return -1;                                // troppe parti
    for (int L = (len < 47 ? len : 47); L >= TTS_DECOMP_MINSUB; L--) {   // prefisso piu' lungo prima
        char pre[48];
        for (int k = 0; k < L; k++) pre[k] = w[k];
        pre[L] = 0;
        if (has && has(pre, ud)) {
            int rest = tts_decompose(w + L, len - L, has, ud, parts + 1, maxp - 1);
            if (rest >= 0) { for (int k = 0; k <= L; k++) parts[0][k] = pre[k]; return rest + 1; }
        }
    }
    return -1;
}

// ---- slug dell'intero testo (== slugify di build_voice.py) ------------------------------------
void nucleo_tts_full_slug(const char *text, char *out, int cap)
{
    if (!out || cap <= 0) return;
    out[0] = 0;
    if (!text) return;
    char norm[1024];
    normalize_ascii(text, norm, sizeof norm);        // fold accenti -> ascii minuscolo
    int o = 0; bool us = false;
    for (int i = 0; norm[i] && o < cap - 1; i++) {
        char c = norm[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) { out[o++] = c; us = false; }
        else if (!us && o > 0) { out[o++] = '_'; us = true; }
    }
    while (o > 0 && out[o - 1] == '_') o--;           // niente '_' finale
    out[o] = 0;
}

// ---- guardia di contenuto (OFFLINE) ----------------------------------------------------------
bool nucleo_tts_text_speakable(const char *text, int max_chars)
{
    if (!text || !text[0]) return false;
    int len = (int)strlen(text);
    if (max_chars > 0 && len > max_chars) return false;            // troppo lungo

    // Segnali forti di codice/markup: in una risposta a voce NON compaiono mai.
    static const char *CODE[] = {
        "```", "</", "/>", "=>", "->", "::", "#include", "function", "def ", "return ",
        "printf", "console.", "import ", "void ", "int ", NULL
    };
    for (int i = 0; CODE[i]; i++) if (strstr(text, CODE[i])) return false;
    // '=' = sempre un'espressione/assegnazione (calcolo): mai in un parlato naturale -> "leggila".
    // (NB: nucleo_tts_say applica mathspeak PRIMA della guardia, quindi i '=' '%' '^' '+' di calc/percent
    // sono gia' diventati parole; un '=' che arriva qui e' di una formula densa -> giusto "leggila".)
    if (strchr(text, '`') || strchr(text, '{') || strchr(text, '}') || strchr(text, '=')) return false;

    // Tipografia matematica (formule di geometria/fisica/vettori/equazioni): mai nel parlato naturale, e
    // il planner non le sa dire (le scarterebbe perdendo il senso: "pi greco per r al quadrato" -> "r").
    // Meglio "leggila" intero. Sequenze UTF-8: · ² ³ ½ √ π ρ ± ≈ Δ ✓ × ÷.
    static const char *const MATHSYM[] = {
        "\xC2\xB7", "\xC2\xB2", "\xC2\xB3", "\xC2\xBD",          // · ² ³ ½
        "\xE2\x88\x9A", "\xCF\x80", "\xCF\x81",                  // √ π ρ
        "\xC2\xB1", "\xE2\x89\x88", "\xCE\x94", "\xE2\x9C\x93",  // ± ≈ Δ ✓
        "\xC3\x97", "\xC3\xB7", NULL                             // × ÷
    };
    for (int i = 0; MATHSYM[i]; i++) if (strstr(text, MATHSYM[i])) return false;

    // Densita' di caratteri "tecnici": >12% -> e' codice/espressione, non parlato.
    int code_ch = 0;
    for (const char *p = text; *p; p++) if (strchr(";()[]=<>/\\|*_", (unsigned char)*p)) code_ch++;
    if (len > 0 && code_ch * 100 / len > 12) return false;
    return true;
}

// ---- testo PARLABILE dell'orario --------------------------------------------------------------
// Stesso testo a SCHERMO e a VOCE. Le forme idiomatiche (quarto/mezza/mezzogiorno/mezzanotte) si
// usano SOLO quando l'ora ci cade esatta -> resta sempre preciso al minuto, mai un arrotondamento.
void nucleo_tts_speak_time(char *out, int n, int h, int m, const char *lang)
{
    if (!out || n <= 0) return;
    out[0] = 0;
    if (h < 0 || h > 23 || m < 0 || m > 59) return;
    bool en = lang && lang[0] == 'e' && lang[1] == 'n';

    if (en) {
        if (m == 0) {
            if      (h == 0)  snprintf(out, n, "It is midnight");
            else if (h == 12) snprintf(out, n, "It is noon");
            else              snprintf(out, n, "It is %d o'clock", h);
        } else {
            snprintf(out, n, "It is %d %d", h, m);          // "It is 9 30" -> "nine thirty"
        }
        return;
    }

    // Italiano (24h). Nome dell'ora: 0 -> mezzanotte, 12 -> mezzogiorno, altrimenti "Sono le N".
    #define HOURNAME(buf, hh) do {                                            \
        if      ((hh) == 0)  snprintf((buf), sizeof(buf), "Mezzanotte");      \
        else if ((hh) == 12) snprintf((buf), sizeof(buf), "Mezzogiorno");     \
        else                 snprintf((buf), sizeof(buf), "Sono le %d", (hh)); \
    } while (0)

    char base[32];
    if (m == 45) {                                          // 4:45 -> "le 5 meno un quarto"; 11:45 -> "mezzogiorno meno un quarto"
        HOURNAME(base, (h + 1) % 24);
        snprintf(out, n, "%s meno un quarto", base);
    } else {
        HOURNAME(base, h);
        if (m == 0) {
            if (h == 0 || h == 12) snprintf(out, n, "%s", base);             // "Mezzanotte" / "Mezzogiorno"
            else                   snprintf(out, n, "%s in punto", base);    // "Sono le 9 in punto"
        }
        else if (m == 15) snprintf(out, n, "%s e un quarto", base);
        else if (m == 30) snprintf(out, n, "%s e mezza", base);
        else              snprintf(out, n, "%s e %d", base, m);              // "Sono le 9 e 7" / "Mezzanotte e 7"
    }
    #undef HOURNAME
}

// ---- "parlabilizza" i simboli matematici ------------------------------------------------------
// Il solver mette nelle risposte simboli che il parlato non gestisce: '=' (la guardia "sa di codice"
// blocca l'INTERA frase), '%' e '^' (il lexer li scarta come separatori, perdendo "per cento"/"elevato").
// Qui li sostituiamo con le PAROLE corrispondenti PRIMA di pianificare, cosi' "Fa 16", "Il 20% di 150
// = 30", "5^3 = 125" si pronunciano. Tocca SOLO questi tre: gli altri (/ · ² ³ ( ) π) restano e, se
// presenti, fanno cadere l'enunciato in fallback/"leggila" (onesto: ohm/geometria sono simbolo-densi).
// Puro/allocation-free; no-op se nessuno dei tre e' presente. `out` sempre terminato (tronca se serve).
void nucleo_tts_mathspeak(const char *in, char *out, int n, const char *lang)
{
    if (!out || n <= 0) return;
    out[0] = 0;
    if (!in) return;
    bool en = lang && lang[0] == 'e' && lang[1] == 'n';
    const char *EQ = en ? " equals "          : " uguale a ";
    const char *PC = en ? " percent "         : " per cento ";
    const char *PW = en ? " to the power of "  : " elevato ";
    const char *PL = en ? " plus "             : " piu ";   // '+' nelle somme/perimetri ("3+4+5", "5 + 3")
    int o = 0;
    for (int i = 0; in[i] && o < n - 1; i++) {
        const char *rep = (in[i] == '=') ? EQ : (in[i] == '%') ? PC :
                          (in[i] == '^') ? PW : (in[i] == '+') ? PL : NULL;
        if (rep) for (int k = 0; rep[k] && o < n - 1; k++) out[o++] = rep[k];
        else     out[o++] = in[i];
    }
    out[o] = 0;
}

// True se `text` contiene tipografia matematica densa (· ² ³ √ π Δ ½) = e' una FORMULA (geometria/
// fisica/vettori) che la voce non sa dire: il call-site puo' allora dire il solo RISULTATO numerico.
bool nucleo_tts_has_mathtypo(const char *text)
{
    if (!text) return false;
    static const char *const S[] = {
        "\xC2\xB7", "\xC2\xB2", "\xC2\xB3", "\xC2\xBD",          // · ² ³ ½
        "\xE2\x88\x9A", "\xCF\x80", "\xCE\x94", NULL             // √ π Δ
    };
    for (int i = 0; S[i]; i++) if (strstr(text, S[i])) return true;
    return false;
}

// ---- velocita' di lettura (rate dell'header WAV) ---------------------------------------------
int nucleo_tts_speed_clamp(int pct)
{
    if (pct < TTS_SPEED_MIN) return TTS_SPEED_MIN;
    if (pct > TTS_SPEED_MAX) return TTS_SPEED_MAX;
    return pct;
}

uint32_t nucleo_tts_speed_rate(uint32_t base_rate, int pct)
{
    pct = nucleo_tts_speed_clamp(pct);
    uint64_t r = (uint64_t)base_rate * (uint32_t)pct / 100u;
    if (r < 8000)  r = 8000;        // sotto/sopra questi l'I2S (e play_wav) non sono affidabili:
    if (r > 48000) r = 48000;       // tieni il rate sano comunque (il clamp del pct di solito basta)
    return (uint32_t)r;
}

// ---- anti-click: fade lineare ai bordi della clip (vedi nucleo_tts.h) -------------------------
// Lavora per CHUNK: per ogni campione del buffer ne calcola l'indice GLOBALE nella clip (base+i) e
// applica un guadagno lineare se cade nei primi/ultimi `fade` campioni. Cosi' il risultato e' identico
// comunque lo streaming spezzi la clip. Allineata al campione (LE byte-a-byte: niente cast a int16* ->
// nessun fault di disallineamento su xtensa). Le giunzioni clip|clip e clip|silenzio passano per ~0.
void nucleo_tts_declick_chunk(unsigned char *buf, int nbytes, uint32_t chunk_off, uint32_t clip_len, int fade)
{
    if (!buf || nbytes < 2 || (chunk_off & 1u) || clip_len < 2) return;   // serve allineamento al campione
    int total = (int)(clip_len / 2);                 // campioni totali della clip
    int F = fade;
    if (F <= 0 || total < 4) return;                  // clip troppo corta: nessun click udibile da smussare
    if (F > total / 2) F = total / 2;                 // fade-in e fade-out non si sovrappongono
    int base = (int)(chunk_off / 2);                  // indice globale del 1o campione del chunk
    int ns = nbytes / 2;
    for (int i = 0; i < ns; i++) {
        int g = base + i;
        if (g >= total) break;                        // oltre la clip (chunk sovradimensionato): stop
        int num = -1;                                 // -1 = guadagno pieno (campione intatto)
        if (g < F)              num = g;              // fade-in: 0 .. F-1  (su F) -> primo campione = 0
        else if (g >= total - F) num = total - 1 - g; // fade-out: F-1 .. 0 (su F) -> ultimo campione = 0
        if (num >= 0) {
            int s = (int16_t)((uint16_t)buf[2 * i] | ((uint16_t)buf[2 * i + 1] << 8));   // LE -> int16
            s = (int)((long)s * num / F);
            buf[2 * i]     = (unsigned char)(s & 0xFF);
            buf[2 * i + 1] = (unsigned char)((s >> 8) & 0xFF);
        }
    }
}

// ---- risultato numerico di una formula (geometria/fisica) ------------------------------------
// Le risposte-formula ("Area del cerchio = π·5² = 78.5398.") sono simbolo-dense -> la guardia le manda
// a "leggila". Ma il RISULTATO dopo l'ULTIMO "= " e' spesso un numero PULITO: lo estraiamo cosi' la
// voce dice "Il risultato e' 78.5398" invece di "leggila". true SOLO se il risultato e' un numero puro
// (cifre/segno/virgola/punto/spazi): le forme con unita' ("= 25 m/s") o le formule senza "=" -> false.
bool nucleo_tts_eq_result(const char *reply, char *out, int n)
{
    if (!reply || !out || n <= 0) return false;
    out[0] = 0;
    const char *last = NULL;
    for (const char *m = reply; (m = strstr(m, "= ")) != NULL; m += 2) last = m + 2;   // ultimo "= "
    if (!last) return false;
    int o = 0;
    for (const char *q = last; *q && o < n - 1; q++) out[o++] = *q;
    while (o > 0 && (out[o - 1] == '.' || out[o - 1] == ' ')) o--;     // togli punto/spazi finali
    out[o] = 0;
    if (!out[0]) return false;
    for (const char *q = out; *q; q++)                                  // dev'essere un NUMERO puro
        if (!((*q >= '0' && *q <= '9') || *q == '.' || *q == ',' || *q == '-' || *q == ' ')) return false;
    return true;
}

// ---- traduttore: pronuncia la traduzione NELLA SUA lingua ------------------------------------
// La skill traduttore risponde `"<src>" in <lingua>: <target>.` — il <target> e' nell'ALTRA lingua,
// quindi l'indice voce (mono-lingua) non lo copre e l'intera frase andrebbe in "leggila". Qui estraiamo
// la PAROLA tradotta e la sua LINGUA ("en"/"it"): cosi' nucleo_tts_say(word, lang) la pronuncia con
// l'indice GIUSTO ("come si dice cane in inglese" -> dice "dog" in inglese). Le forme omografo/miss
// (con "->"/"<->") -> false: cadono in "leggila" (giusto, sono spiegazioni). Pura/testabile.
bool nucleo_tts_translate_word(const char *reply, char *word, int wn, char *lang, int ln)
{
    if (!reply || !word || wn <= 0 || !lang || ln <= 0) return false;
    word[0] = 0; lang[0] = 0;
    static const char *const M[][2] = {
        {" in inglese: ", "en"}, {" in italiano: ", "it"},
        {" in English: ", "en"}, {" in Italian: ", "it"}, {NULL, NULL}
    };
    const char *p = NULL, *lg = NULL;
    for (int i = 0; M[i][0]; i++) { const char *m = strstr(reply, M[i][0]); if (m) { p = m + strlen(M[i][0]); lg = M[i][1]; break; } }
    if (!p) return false;
    int o = 0;
    while (*p && *p != '.' && o < wn - 1) word[o++] = *p++;   // la traduzione fino al punto finale
    while (o > 0 && word[o - 1] == ' ') o--;
    word[o] = 0;
    snprintf(lang, ln, "%s", lg);
    return word[0] != 0;
}

// ---- "gist" breve: la PRIMA frase di una risposta lunga -------------------------------------
// INNOVAZIONE "voce a mosaico": una risposta descrittiva (conoscenza/MOSAICO) e' troppo lunga per la
// voce, ma la PRIMA frase e' di solito la definizione/gist ("La fotosintesi e' il processo..."). La
// estraiamo e proviamo a dirla BREVE; col pool ricco e' spesso coperta -> parlata, altrimenti "leggila".
// Termina alla prima '.'/'!'/'?' che chiude davvero una frase (non un decimale "3.14" ne' una sigla a
// lettera singola "Dr."). Copia fino a out[n-1]; se nessun terminatore, copia (troncato) e ci pensa la guardia.
void nucleo_tts_first_sentence(const char *in, char *out, int n)
{
    if (!out || n <= 0) return;
    out[0] = 0;
    if (!in) return;
    int i = 0; while (in[i] == ' ' || in[i] == '\t' || in[i] == '\n') i++;   // salta spazi iniziali
    int o = 0;
    for (; in[i] && o < n - 1; i++) {
        char c = in[i];
        out[o++] = c;
        if (c == '.' || c == '!' || c == '?') {
            char nx = in[i + 1];
            bool between_digits = (c == '.') && o >= 2 &&
                                  out[o - 2] >= '0' && out[o - 2] <= '9' && nx >= '0' && nx <= '9';
            bool initial = (c == '.') && o >= 2 && out[o - 2] >= 'A' && out[o - 2] <= 'Z' &&
                           (o < 3 || out[o - 3] == ' ');   // "Dr." "J." -> non fine frase
            if (!between_digits && !initial && (nx == 0 || nx == ' ' || nx == '\n' || nx == '\t' || nx == '"'))
                break;                                       // fine prima frase
        }
    }
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\t')) o--;
    out[o] = 0;
}

// ---- planner pubblico -------------------------------------------------------------------------
int nucleo_tts_plan(const char *text, const char *lang,
                    tts_has_clip_fn has_clip, void *ud,
                    tts_token_t *out, int max)
{
    if (!text || !out || max <= 0) return 0;
    bool en = lang && lang[0] == 'e' && lang[1] == 'n';   // "en" -> inglese, altrimenti italiano

    char norm[1024];
    normalize_keepcase(text, norm, sizeof(norm));   // case preservato: lex marca gli acronimi (allcaps)

    unit_t u[MAX_UNITS];
    int nu = lex_units(norm, u, MAX_UNITS);

    sink_t s = { out, max, 0, en };
    for (int i = 0; i < nu && s.n < max; ) {
        if (u[i].kind == U_PAUSE) { push_pause(&s, u[i].ms); i++; continue; }
        if (u[i].kind == U_NUMBER) { emit_number(&s, u[i].s); i++; continue; }

        // U_WORD: prova un match a FRASE greedy (parole consecutive unite da '_'), dal piu' lungo.
        int span = 0;
        while (span < MAX_PHRASE && i + span < nu && u[i + span].kind == U_WORD) span++;
        bool matched = false;
        for (int k = span; k >= 1; k--) {
            char slug[48]; int o = 0;
            bool toolong = false;
            for (int j = 0; j < k; j++) {
                const char *w = u[i + j].s;
                if (j > 0) { if (o < (int)sizeof(slug) - 1) slug[o++] = '_'; else { toolong = true; break; } }
                for (int c = 0; w[c]; c++) { if (o < (int)sizeof(slug) - 1) slug[o++] = w[c]; else { toolong = true; break; } }
                if (toolong) break;
            }
            slug[o] = 0;
            if (!toolong && o > 0 && has_clip && has_clip(slug, ud)) {
                push_clip(&s, slug, false);
                i += k;
                matched = true;
                break;
            }
        }
        if (matched) continue;

        // nessuna frase: parola singola coperta -> clip; altrimenti, se e' un ACRONIMO tutto-maiuscolo
        // (USB/GPS/MP3) non coperto, sillabalo con le clip lettera; poi prova a SCOMPORLA in sotto-parole
        // coperte (composti/derivati, zero file nuovi); se nemmeno cosi' -> UNKNOWN (-> "leggila").
        if (has_clip && has_clip(u[i].s, ud)) { push_clip(&s, u[i].s, false); i++; continue; }
        if (u[i].allcaps && spell_acronym(&s, u[i].s, has_clip, ud)) { i++; continue; }
        char parts[TTS_DECOMP_MAXPARTS + 1][48];
        // SOVRACCARICO: ogni sotto-parte e' >= MINSUB char, quindi una scomposizione (>=2 parti) richiede
        // >= 2*MINSUB char. Sotto quella soglia e' MATEMATICAMENTE impossibile: saltala senza sprecare
        // ricerche binarie su SD (bus SPI condiviso col display) nel caso comune della parola corta scoperta.
        int wl = (int)strlen(u[i].s);
        int np = (has_clip && wl >= 2 * TTS_DECOMP_MINSUB) ? tts_decompose(u[i].s, wl, has_clip, ud, parts, TTS_DECOMP_MAXPARTS) : -1;
        if (np >= 2) for (int j = 0; j < np && s.n < max; j++) push_clip(&s, parts[j], true);   // composizione
        else         push_unknown(&s, u[i].s);
        i++;
    }
    return s.n;
}
