// Host test of the TTS planner (firmware/components/nucleo_tts/nucleo_tts_plan.c): Italian text
// -> sequence of CLIP/PAUSE tokens. Compiles the real firmware C, no hardware.
// Build+run (PowerShell): see tools/anima-host/tts-check.mjs (npm run anima:tts).
// Expected per-token encoding: "C:slug" (clip), "F:slug" (fallback/spelling clip), "P:ms" (pause).
#include "nucleo_tts.h"
#include <stdio.h>
#include <string.h>

// Fake manifest: the slugs that "exist" as clips (words/phrases). The numbers and connectives of the
// mandatory pack (n0..n99, cento.., mille, mila, virgola, meno) are emitted by the planner
// WITHOUT consulting the manifest, so they are not needed here.
static const char *MANIFEST[] = {
    "ciao", "sono", "le", "e", "gradi", "perche", "buona", "sera", "come_stai", "buona_sera",
    "hello", "how_are_you", "good_evening", "degrees",          // English
    "il", "di", "fa", "per_cento", "uguale_a", "elevato", "piu",   // mathspeak IT
    "of", "percent", "equals", "plus",                             // mathspeak EN
    "casa", "via", "sud", "porta",                                 // real words >=3 chars for the decompose boundaries
    // letter clips for acronym spelling (NB: q and z deliberately OMITTED -> "missing letter" test)
    "lett_u", "lett_s", "lett_b", "lett_g", "lett_p", "lett_m", "lett_h", "lett_t", "lett_l",
    NULL
};
static bool has_clip(const char *slug, void *ud) {
    (void)ud;
    for (int i = 0; MANIFEST[i]; i++) if (!strcmp(MANIFEST[i], slug)) return true;
    return false;
}

static void encode(const tts_token_t *t, int n, char *out, int cap) {
    int o = 0;
    for (int i = 0; i < n && o < cap - 1; i++) {
        if (i) o += snprintf(out + o, cap - o, " ");
        if (t[i].kind == TTS_TOK_PAUSE)        o += snprintf(out + o, cap - o, "P:%d", t[i].ms);
        else if (t[i].kind == TTS_TOK_UNKNOWN) o += snprintf(out + o, cap - o, "U");
        else                                   o += snprintf(out + o, cap - o, "C:%s", t[i].slug);
    }
    out[o < cap ? o : cap - 1] = 0;
}

static int g_pass = 0, g_fail = 0;
static void check(const char *label, const char *lang, const char *in, const char *expect) {
    tts_token_t tok[96];
    int n = nucleo_tts_plan(in, lang, has_clip, NULL, tok, 96);
    char got[512];
    encode(tok, n, got, sizeof(got));
    if (!strcmp(got, expect)) { g_pass++; printf("  ok   %-22s | %s\n", label, got); }
    else { g_fail++; printf("  FAIL %-22s\n    in:  \"%s\"\n    exp: %s\n    got: %s\n", label, in, expect, got); }
}

static void chk(const char *label, int cond) {
    if (cond) { g_pass++; printf("  ok   %s\n", label); }
    else      { g_fail++; printf("  FAIL %s\n", label); }
}

// Real device pipeline: mathspeak (= % ^ -> words) BEFORE the planner. Insensitive to the double
// spaces introduced by the substitution (the lexer treats them as separators).
static void check_ms(const char *label, const char *lang, const char *in, const char *expect) {
    char sp[512];
    nucleo_tts_mathspeak(in, sp, sizeof sp, lang);
    tts_token_t tok[96];
    int n = nucleo_tts_plan(sp, lang, has_clip, NULL, tok, 96);
    char got[512];
    encode(tok, n, got, sizeof(got));
    if (!strcmp(got, expect)) { g_pass++; printf("  ok   %-22s | %s\n", label, got); }
    else { g_fail++; printf("  FAIL %-22s\n    in:  \"%s\"\n    ms:  \"%s\"\n    exp: %s\n    got: %s\n", label, in, sp, expect, got); }
}

int main(void) {
    printf("TTS planner (IT + EN) — testo -> clip\n");

    // --- ITALIAN ---
    check("it saluto",     "it", "Ciao!",            "C:ciao P:360");
    check("it frase nota", "it", "Come stai?",       "C:come_stai P:360");
    check("it frase>parole","it", "Buona sera",      "C:buona_sera");          // greedy: the phrase beats the words
    check("it accento",    "it", "Perch\xC3\xA9",    "C:perche");              // "Perché" -> perche
    check("it ora",        "it", "Sono le 9 e 30.",  "C:sono C:le C:n9 C:e C:n30 P:320");
    check("it unita",      "it", "23 gradi",         "C:n23 C:gradi");
    check("it anno 1984",  "it", "1984",             "C:mille C:novecento C:n84");
    check("it cento",      "it", "100",              "C:cento");
    check("it 234",        "it", "234",              "C:duecento C:n34");
    check("it 2345",       "it", "2345",             "C:duemila C:trecento C:n45");
    check("it 12000",      "it", "12000",            "C:n12 C:mila");
    check("it decimale",   "it", "3,14",             "C:n3 C:virgola C:n1 C:n4");
    check("it negativo",   "it", "-5 gradi",         "C:meno C:n5 C:gradi");
    check("it ignota",     "it", "Zxqwv",            "U");                      // neither covered NOR decomposable -> UNKNOWN
    check("it mista",      "it", "23 Zxqwv gradi",   "C:n23 U C:gradi");        // 1 unknown in the middle
    check("it composito",  "it", "Buonasera",        "C:buona C:sera");         // DECOMPOSED: buona+sera (zero new files)
    check("it comp+ignota","it", "Buonaseraxqz",     "U");                      // buona+sera+xqz(no) -> not decomposable -> UNKNOWN

    // --- ACRONYM spelling: uncovered all-caps acronym -> letter-by-letter (lett_X clips) ---
    check("ac USB",        "it", "USB",              "C:lett_u C:lett_s C:lett_b");      // u esse bi
    check("ac GPS",        "it", "GPS",              "C:lett_g C:lett_p C:lett_s");
    check("ac alnum MP3",  "it", "MP3",              "C:lett_m C:lett_p C:n3");           // letters + digit
    check("ac in frase",   "it", "23 USB",           "C:n23 C:lett_u C:lett_s C:lett_b"); // acronym between numbers
    check("ac minuscolo",  "it", "usb",              "U");                                // NOT all caps -> no spelling
    check("ac lettera mancante","it","QZ",           "U");                                // lett_q/lett_z missing -> UNKNOWN (no partial spelling)
    check("ac parola-coperta","it","CIAO",           "C:ciao");                           // all caps but COVERED: the word clip wins
    // BOUNDARIES of the compositional planner (P1: below 2*MINSUB=6 chars decomposition is impossible -> no wasted IO).
    check("it comp 6char", "it", "viasud",           "C:via C:sud");            // 3+3 = exact minimum decomposable
    check("it comp prefisso","it","casavia",         "C:casa C:via");           // LONGEST prefix first: casa(4)+via(3)
    check("it comp 5+3",   "it", "portasud",         "C:porta C:sud");          // 5+3: "porta" beats "port" as a prefix
    check("it corta scoperta","it","viaca",          "U");                      // 5 chars: decomposition is impossible (P1 skips), stays UNKNOWN
    check("it corta 4char","it", "casx",             "U");                      // 4 chars uncovered: UNKNOWN, no useless probe

    // --- ENGLISH ---
    check("en greeting",   "en", "Hello!",           "C:hello P:360");
    check("en phrase",     "en", "How are you?",     "C:how_are_you P:360");
    check("en units",      "en", "23 degrees",       "C:n23 C:degrees");
    check("en year 1984",  "en", "1984",             "C:n1 C:thousand C:n9 C:hundred C:n84");
    check("en 234",        "en", "234",              "C:n2 C:hundred C:n34");
    check("en 2345",       "en", "2345",             "C:n2 C:thousand C:n3 C:hundred C:n45");
    check("en 12000",      "en", "12000",            "C:n12 C:thousand");
    check("en decimal",    "en", "3.14",             "C:n3 C:point C:n1 C:n4");
    check("en negative",   "en", "-5 degrees",       "C:minus C:n5 C:degrees");

    // --- content guard (no code / nothing too long -> "leggila", i.e. read it on screen) ---
    chk("ok frase breve",   nucleo_tts_text_speakable("Sono le 9 e 30.", 140));
    chk("ok frase normale", nucleo_tts_text_speakable("La batteria e al novanta per cento.", 140));
    chk("no printf",       !nucleo_tts_text_speakable("usa printf(\"ciao\");", 140));
    chk("no function",     !nucleo_tts_text_speakable("function f(){ return 1 }", 140));
    chk("no backtick",     !nucleo_tts_text_speakable("esegui `ls -la` adesso", 140));
    chk("no tag html",     !nucleo_tts_text_speakable("<div>ciao</div>", 140));
    chk("no troppo lungo", !nucleo_tts_text_speakable(
        "Questa risposta e' volutamente lunghissima e supera di parecchio il limite di centoquaranta caratteri previsto per la voce offline percio' va letta.", 140));
    chk("no densita codice", !nucleo_tts_text_speakable("a=(b[0]+c[1])*d/e;", 140));
    chk("no uguale (calc)",  !nucleo_tts_text_speakable("5 elevato 3 = 125", 140));

    // --- mathspeak: = % ^ -> words (so calc/percent/power are SPOKEN instead of "leggila") ---
    char fs_ms[64];
    check_ms("ms it fa",      "it", "Fa 16.",              "C:fa C:n16 P:320");
    check_ms("ms it percent", "it", "Il 20% di 150 = 30.", "C:il C:n20 C:per_cento C:di C:cento C:n50 C:uguale_a C:n30 P:320");
    check_ms("ms it potenza", "it", "5^3 = 125.",          "C:n5 C:elevato C:n3 C:uguale_a C:cento C:n25 P:320");
    check_ms("ms en percent", "en", "20% of 150 = 30.",    "C:n20 C:percent C:of C:n1 C:hundred C:n50 C:equals C:n30 P:320");
    check_ms("ms it somma",   "it", "5 + 3 = 8.",          "C:n5 C:piu C:n3 C:uguale_a C:n8 P:320");
    nucleo_tts_mathspeak("La media e 7.", fs_ms, sizeof fs_ms, "it");
    chk("ms no-op",          !strcmp(fs_ms, "La media e 7."));

    // --- short gist: first sentence of a long reply (mosaic voice) ---
    char gs[160];
    nucleo_tts_first_sentence("La fotosintesi e' il processo. Avviene nelle piante. Produce ossigeno.", gs, sizeof gs);
    chk("gist 1a frase",    !strcmp(gs, "La fotosintesi e' il processo."));
    nucleo_tts_first_sentence("  Roma e' la capitale d'Italia, fondata nel 753 a.C.", gs, sizeof gs);
    chk("gist trim+virgola", !strcmp(gs, "Roma e' la capitale d'Italia, fondata nel 753 a.C."));
    nucleo_tts_first_sentence("Pi greco vale 3.14 circa. Serve per i cerchi.", gs, sizeof gs);
    chk("gist no-decimale",  !strcmp(gs, "Pi greco vale 3.14 circa."));
    nucleo_tts_first_sentence("Senza punto finale qui", gs, sizeof gs);
    chk("gist senza punto",  !strcmp(gs, "Senza punto finale qui"));

    // --- translator: extract the translated word + its language (the voice says it with the right index) ---
    char tw[80], tl[8];
    chk("xl it->en",   nucleo_tts_translate_word("\"cane\" in inglese: dog.", tw, sizeof tw, tl, sizeof tl)
                       && !strcmp(tw, "dog") && !strcmp(tl, "en"));
    chk("xl en->it",   nucleo_tts_translate_word("\"dog\" in italiano: cane.", tw, sizeof tw, tl, sizeof tl)
                       && !strcmp(tw, "cane") && !strcmp(tl, "it"));
    chk("xl EN-sess",  nucleo_tts_translate_word("\"cane\" in English: dog.", tw, sizeof tw, tl, sizeof tl)
                       && !strcmp(tw, "dog") && !strcmp(tl, "en"));
    chk("xl frase",    nucleo_tts_translate_word("\"buonasera\" in inglese: good evening.", tw, sizeof tw, tl, sizeof tl)
                       && !strcmp(tw, "good evening") && !strcmp(tl, "en"));
    chk("xl omografo", !nucleo_tts_translate_word("\"radio\" esiste in entrambe le lingue. IT->EN: radio. EN->IT: radio.", tw, sizeof tw, tl, sizeof tl));
    chk("xl miss",     !nucleo_tts_translate_word("Non ho \"xyz\" nel dizionario offline IT<->EN.", tw, sizeof tw, tl, sizeof tl));

    // --- dense formula -> say only the numeric RESULT (geometry/physics) ---
    char er[48];
    chk("typo geo",     nucleo_tts_has_mathtypo("Area del cerchio = \xCF\x80\xC2\xB75\xC2\xB2 = 78.5398."));
    chk("typo no-calc", !nucleo_tts_has_mathtypo("Il 20% di 150 = 30."));
    chk("eq geo",       nucleo_tts_eq_result("Area del cerchio = \xCF\x80\xC2\xB75\xC2\xB2 = 78.5398.", er, sizeof er) && !strcmp(er, "78.5398"));
    chk("eq formula",  !nucleo_tts_eq_result("Area del cerchio: A = \xCF\x80\xC2\xB7r\xC2\xB2.", er, sizeof er));
    chk("eq unita",    !nucleo_tts_eq_result("v = s/t = 90000/3600 = 25 m/s.", er, sizeof er));
    chk("eq no-eq",    !nucleo_tts_eq_result("Fa 96.", er, sizeof er));

    // dense math typography (geometry/physics/vectors) -> "leggila" (the guard rejects it)
    chk("no pi greco",      !nucleo_tts_text_speakable("Area del cerchio = \xCF\x80\xC2\xB7r\xC2\xB2.", 140));
    chk("no radice quadra", !nucleo_tts_text_speakable("d = \xE2\x88\x9A(a\xC2\xB2+b\xC2\xB2).", 140));
    chk("no cubo sym",      !nucleo_tts_text_speakable("Volume = l\xC2\xB3.", 140));
    chk("no delta",         !nucleo_tts_text_speakable("\xCE\x94 = 4 > 0.", 140));
    chk("ok pulito",         nucleo_tts_text_speakable("doppio di 5 fa 10.", 140));

    // --- whole slug (fixed-reply lookup -> clip) ---
    char fs[48];
    nucleo_tts_full_slug("non lo so", fs, sizeof fs);                  chk("slug non_lo_so",  !strcmp(fs, "non_lo_so"));
    nucleo_tts_full_slug("Sono ANIMA, l'assistente offline.", fs, sizeof fs); chk("slug identita",   !strcmp(fs, "sono_anima_l_assistente_offline"));
    nucleo_tts_full_slug("Perch\xC3\xA9?", fs, sizeof fs);             chk("slug accento",    !strcmp(fs, "perche"));
    nucleo_tts_full_slug("La batteria \xC3\xA8 al 90 per cento.", fs, sizeof fs); chk("slug batteria", !strcmp(fs, "la_batteria_e_al_90_per_cento"));

    // --- reading speed: % clamp + rate header (tape-speed, zero-RAM) ---
    chk("spd clamp lo",   nucleo_tts_speed_clamp(10)  == TTS_SPEED_MIN);
    chk("spd clamp hi",   nucleo_tts_speed_clamp(999) == TTS_SPEED_MAX);
    chk("spd clamp ok",   nucleo_tts_speed_clamp(110) == 110);
    chk("spd rate 100",   nucleo_tts_speed_rate(24000, 100) == 24000);   // neutral = identical to before
    chk("spd rate 110",   nucleo_tts_speed_rate(24000, 110) == 26400);   // +10% faster
    chk("spd rate 70",    nucleo_tts_speed_rate(24000, 70)  == 16800);   // slower
    chk("spd rate clamp", nucleo_tts_speed_rate(24000, 5) == nucleo_tts_speed_rate(24000, TTS_SPEED_MIN));
    // I2S SAFETY: the rate handed to the audio always stays in [8000,48000] Hz for any input -> no
    // absurd I2S clock that would silence/distort the speaker (regression guard for the speed->audio path).
    chk("spd rate i2s-cap",   nucleo_tts_speed_rate(48000, 160) == 48000);   // 48k×1.6=76.8k -> capped
    chk("spd rate i2s-floor", nucleo_tts_speed_rate(8000, 70)   == 8000);    // 8k×0.7=5.6k -> floored
    chk("spd rate sane-all",  nucleo_tts_speed_rate(24000, 160) >= 8000 && nucleo_tts_speed_rate(24000, 160) <= 48000);

    // --- anti-click: linear fade at the clip edges during assembly (joint quality) ---
    {
        enum { NS = 40, FD = 8 };                       // fake clip: 40 samples of constant amplitude 1000
        unsigned char a[NS * 2], b[NS * 2];
        #define SAMP(p, k) ((int)(int16_t)((uint16_t)(p)[2*(k)] | ((uint16_t)(p)[2*(k)+1] << 8)))
        for (int i = 0; i < NS; i++) { a[2*i] = (unsigned char)(1000 & 0xFF); a[2*i+1] = (unsigned char)((1000 >> 8) & 0xFF); }
        memcpy(b, a, sizeof a);

        nucleo_tts_declick_chunk(a, sizeof a, 0, NS * 2, FD);   // the whole clip in a single chunk
        chk("declick primo=0",   SAMP(a, 0) == 0);             // start edge zeroed -> smooth joint
        chk("declick ultimo=0",  SAMP(a, NS - 1) == 0);        // end edge zeroed
        chk("declick centro pieno", SAMP(a, 20) == 1000);      // the "real" audio in the middle is intact
        int prev = -1, mono = 1;
        for (int i = 0; i < FD; i++) { int s = SAMP(a, i); if (s < prev) mono = 0; prev = s; }
        chk("declick fade-in monotono", mono);

        // per-chunk EQUIVALENCE: splitting the same clip into 2 chunks must give the SAME result as the single chunk
        nucleo_tts_declick_chunk(b, 30, 0, NS * 2, FD);                  // chunk #1: bytes 0..29 (15 samples)
        nucleo_tts_declick_chunk(b + 30, sizeof b - 30, 30, NS * 2, FD); // chunk #2: bytes 30.. (the rest, covers the fade-out)
        chk("declick chunked == unico", memcmp(a, b, sizeof a) == 0);

        // very short clip (3 samples): no fade, no crash, data intact
        unsigned char c[3 * 2]; for (int i = 0; i < 3; i++) { c[2*i] = 5; c[2*i+1] = 0; }
        nucleo_tts_declick_chunk(c, sizeof c, 0, 3 * 2, FD);
        chk("declick clip corta intatta", c[0] == 5 && c[2] == 5 && c[4] == 5);
        #undef SAMP
    }

    printf("\n%d ok, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
