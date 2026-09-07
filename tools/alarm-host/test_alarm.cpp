// Host harness for the native Allarme app: it compiles the REAL firmware app_alarm.cpp against the
// stubs and drives it through scripted scenarios on a simulated clock — arm, silent hit, recording,
// auto re-arm, second hit, PIN, layout. Run: node tools/alarm-host/run.mjs   (or npm run alarm:test)
#include "host_stubs.h"
#include "stubs/app_gfx.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <direct.h>
#include <dirent.h>

extern "C" void nucleo_register_alarm(void);

static int g_fail, g_checks;
static void ok(bool cond, const char *what)
{
    g_checks++;
    if (!cond) { g_fail++; printf("  FAIL  %s\n", what); }
    else       { printf("  ok    %s\n", what); }
}

// ---- driving the app -------------------------------------------------------
static void step(int ms)                       // one run-loop iteration: the framework polls ~50 Hz
{
    for (int t = 0; t < ms; t += 20) { g_now_us += 20000; if (g_poll) g_poll(); }
}
static void key(int k, char ch)
{
    if ((k == 4 /*NK_BACK*/ || k == 7 /*NK_LEFT*/) && g_back) { g_back(k); return; }
    if (k == 5 /*NK_TAB*/ && g_tab) { g_tab(); return; }
    if (g_app.on_key) g_app.on_key(k, ch);
}
#define K_CHAR 1
#define K_ENTER 2
#define K_BACK 3
#define K_TAB 4
#define K_UP 5
#define K_DOWN 6
#define K_LEFT 7
#define K_RIGHT 8
static void press(int k, char ch = 0)
{
    if (k == K_BACK || k == K_LEFT) { if (g_back) g_back(k); return; }
    if (k == K_TAB) { if (g_tab) g_tab(); return; }
    if (g_app.on_key) g_app.on_key(k, ch);
}
static void digits(const char *s) { for (const char *p = s; *p; p++) press(K_CHAR, *p); }

static void frame(void) { g_texts = 0; if (g_app.on_draw) g_app.on_draw(); }
static bool shows(const char *needle)
{
    for (int i = 0; i < g_texts; i++) if (strstr(g_txt[i], needle)) return true;
    return false;
}
static bool screen_dark(void) { return g_panel == 0; }

// ---- layout assertions -----------------------------------------------------
// The panel is 240x135 with a 14 px hint bar the app must not paint into, so every glyph must live
// inside 0..240 x 0..121 — and two strings in the same frame must not sit on top of each other.
static bool layout_clean(const char *screen)
{
    bool good = true;
    for (int i = 0; i < g_texts; i++) {
        g_box_t b = g_text[i];
        if (b.x < 0 || b.x + b.w > 240) {
            printf("  FAIL  %s: \"%s\" overflows horizontally (x=%d w=%d)\n", screen, g_txt[i], b.x, b.w);
            good = false;
        }
        if (b.y < 0 || b.y + b.h > 121) {
            printf("  FAIL  %s: \"%s\" overflows the content area (y=%d h=%d)\n", screen, g_txt[i], b.y, b.h);
            good = false;
        }
        for (int j = i + 1; j < g_texts; j++) {
            g_box_t c = g_text[j];
            if (b.x < c.x + c.w && c.x < b.x + b.w && b.y < c.y + c.h && c.y < b.y + b.h) {
                printf("  FAIL  %s: \"%s\" and \"%s\" overlap\n", screen, g_txt[i], g_txt[j]);
                good = false;
            }
        }
    }
    return good;
}
static void check_layout(const char *screen)
{
    g_checks++;
    if (layout_clean(screen)) printf("  ok    layout %s (%d strings)\n", screen, g_texts);
    else g_fail++;
}

// ---- SD sandbox helpers ----------------------------------------------------
static char SD[512];
static void sd_path(char *out, size_t n, const char *rel) { snprintf(out, n, "%s%s", SD, rel); }
static long file_size(const char *rel)
{
    char p[700]; sd_path(p, sizeof p, rel);
    struct stat st;
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}
static bool read_all(const char *rel, char *buf, size_t n)
{
    char p[700]; sd_path(p, sizeof p, rel);
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    size_t r = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[r] = 0;
    return true;
}
// Newest WAV in the recordings folder, and how many there are.
static int wav_count(char *newest, size_t n)
{
    char dir[700]; sd_path(dir, sizeof dir, "/sd/data/Alarm/rec");
    DIR *d = opendir(dir);
    if (!d) return 0;
    int count = 0;
    char best[128] = "";
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".wav")) continue;
        count++;
        if (strcmp(e->d_name, best) > 0) snprintf(best, sizeof best, "%s", e->d_name);
    }
    closedir(d);
    if (count && newest) snprintf(newest, n, "/sd/data/Alarm/rec/%s", best);
    return count;
}
static bool find_wav(char *rel, size_t n) { return wav_count(rel, n) > 0; }
static long wav_payload(const char *rel)      // "data" size as last synced INTO the file
{
    char p[700]; sd_path(p, sizeof p, rel);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    unsigned char h[44];
    size_t n = fread(h, 1, 44, f);
    fclose(f);
    if (n != 44) return -1;
    return (long)(h[40] | (h[41] << 8) | (h[42] << 16) | ((unsigned)h[43] << 24));
}
static unsigned le32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }

// ---- menu navigation -------------------------------------------------------
static void open_menu(void) { press(K_TAB); }
static void menu_row(int row1based) { press(K_CHAR, (char)('0' + row1based)); }   // 1-9 quick-select

int main(int argc, char **argv)
{
    if (argc > 1) snprintf(SD, sizeof SD, "%s", argv[1]);
    else          snprintf(SD, sizeof SD, "%s", "sdroot");
    host_sd_root(SD);
    { // the sandbox needs the "/sd" mount point itself: on the device it is always there
      char m[600]; _mkdir(SD); snprintf(m, sizeof m, "%s/sd", SD); _mkdir(m); }
    nucleo_register_alarm();
    printf("app: %s / %s  (exclusive flags 0x%02x)\n\n", g_app.id, g_app.name, g_app.exclusive_flags);

    // ================= 1. opening the app =================
    printf("[1] apertura: 10 s acceso, poi buio\n");
    g_app.on_enter();
    step(1000);
    ok(!screen_dark(), "lo schermo e' acceso subito dopo l'apertura");
    frame(); ok(shows("DISARMATO"), "mostra DISARMATO");
    check_layout("disarmato");
    step(8000);
    ok(!screen_dark(), "ancora acceso a 9 s");
    step(2000);
    ok(screen_dark(), "spento dopo 10 s");
    frame(); ok(g_texts == 0, "a schermo spento non compone nulla");

    // ================= 2. wake =================
    printf("\n[2] risveglio col tasto\n");
    press(K_CHAR, '5');
    ok(!screen_dark(), "un tasto qualsiasi riaccende");
    frame(); ok(shows("DISARMATO"), "il primo tasto e' ingoiato dal risveglio (nessuna azione)");
    step(16000);
    ok(screen_dark(), "si rispegne 15 s dopo il tasto");

    // ================= 3. silent mode + recording via the menu =================
    printf("\n[3] menu: modo silenzioso + registrazione 1 min\n");
    press(K_CHAR, 'x');                     // wake
    open_menu();
    frame(); check_layout("impostazioni");
    ok(g_texts >= 5, "il menu mostra almeno 5 righe (era 3 su 10)");
    menu_row(2); press(K_RIGHT);            // Modo -> Silenzioso
    frame(); ok(shows("Silenzioso"), "modo = Silenzioso");
    menu_row(9);                                   // Registra muto
    for (int i = 0; i < 8; i++) { frame(); if (shows("1min")) break; press(K_RIGHT); }
    frame(); ok(shows("1min"), "durata registrazione = 1 min");
    check_layout("impostazioni/registra");
    press(K_TAB);                            // leave the sheet (persists to SD)
    ok(file_size("/sd/system/config/alarm.json") > 0, "le opzioni sono salvate su SD");

    // ================= 4. arm =================
    printf("\n[4] armo\n");
    press(K_ENTER);
    frame(); check_layout("countdown");
    step(8000);                              // 5 s countdown + the 1.5 s arm grace
    frame(); ok(shows("ARMATO"), "armato dopo il ritardo");
    check_layout("armato");
    ok(g_mic_is_open, "il microfono e' aperto in ARMATO");
    ok(g_voice_suspended == 1, "il motore vocale e' sospeso");
    step(4000);
    ok(screen_dark(), "3 s dopo l'armo lo schermo e' spento");

    // ================= 5. silent hit -> recording =================
    printf("\n[5] scatto silenzioso: nessun suono, nastro parte\n");
    int siren_before = g_siren_calls;
    g_mic_level_pct = 90;                    // loud room
    step(400);
    ok(g_siren_calls == siren_before, "nessuna sirena in modo silenzioso");
    ok(screen_dark(), "lo schermo resta spento");
    char wav[200];
    ok(find_wav(wav, sizeof wav), "il file WAV e' stato creato");
    long sz1 = wav_payload(wav);
    ok(sz1 == 0, "l'header e' gia' sulla card a registrazione appena aperta");
    g_mic_level_pct = 5;                     // room goes quiet again
    step(4000);
    long sz2 = wav_payload(wav);
    ok(sz2 > sz1, "il WAV cresce e l'header resta veritiero (sync ~2 s)");

    // ================= 6. auto re-arm must not stop the tape =================
    printf("\n[6] riarmo automatico durante la registrazione\n");
    step(20000);                             // default rearm = 20 s
    long sz3 = wav_payload(wav);
    ok(sz3 > sz2, "il nastro NON si interrompe al riarmo automatico");
    ok(g_mic_is_open, "il microfono resta aperto attraverso il riarmo");
    frame(); ok(g_texts == 0, "schermo ancora spento dopo il riarmo");

    // ================= 7. a second hit must not restart or stop it =================
    printf("\n[7] secondo scatto mentre registra\n");
    g_mic_level_pct = 95;
    step(400);
    g_mic_level_pct = 5;
    long sz4 = wav_payload(wav);
    step(4000);
    ok(wav_payload(wav) > sz4, "un nuovo scatto non interrompe la registrazione");
    ok(wav_count(NULL, 0) == 1, "nessun secondo file: la registrazione continua nello stesso");

    // ================= 8. it stops on time, with a valid WAV header ==========
    printf("\n[8] chiusura del nastro e header WAV\n");
    step(70000);                             // 1 min setting + the bump from the second hit
    long fin = file_size(wav);
    char head[64];
    char p[700]; sd_path(p, sizeof p, wav);
    FILE *f = fopen(p, "rb");
    size_t hn = f ? fread(head, 1, 44, f) : 0;
    if (f) fclose(f);
    ok(hn == 44, "header WAV di 44 byte");
    if (hn == 44) {
        const unsigned char *h = (const unsigned char *)head;
        unsigned riff = le32(h + 4), data = le32(h + 40), rate = le32(h + 24);
        ok(!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4), "magic RIFF/WAVE");
        ok(rate == 16000, "campionamento 16 kHz");
        ok(data == (unsigned)(fin - 44), "il campo data corrisponde ai byte scritti");
            ok(riff == data + 36, "il campo RIFF e' coerente");
        long secs = (long)data / 32000;
        ok(secs >= 55 && secs <= 130, "durata coerente con 1 min + estensione (misurati ~32 KB/s)");
    }

    // ================= 9. PIN =================
    printf("\n[9] PIN: sbagliato resta armato, giusto disarma\n");
    press(K_CHAR, 'w');                      // wake
    digits("1234");
    frame(); ok(!shows("DISARMATO"), "PIN errato non disarma");
    digits("0000");
    frame(); ok(shows("DISARMATO"), "PIN corretto disarma");
    ok(g_voice_suspended == 0, "il motore vocale e' stato restituito");
    ok(!g_mic_is_open, "il microfono e' chiuso da disarmato");

    // ================= 10. PIN change is a two-stage challenge ==============
    printf("\n[10] cambio PIN con conferma del vecchio\n");
    open_menu();
    menu_row(6); press(K_ENTER);             // PIN row
    frame(); ok(shows("PIN attuale"), "chiede prima il PIN attuale");
    check_layout("pin attuale");
    digits("9999");
    frame(); ok(shows("PIN errato") || shows("PIN attuale"), "PIN attuale errato non passa alla fase 2");
    ok(!shows("Nuovo PIN"), "non arriva al nuovo PIN con il vecchio sbagliato");
    digits("0000");
    frame(); ok(shows("Nuovo PIN"), "col PIN giusto chiede il nuovo");
    digits("4321");
    frame(); ok(shows("PIN aggiornato"), "conferma il cambio");
    press(K_TAB);
    press(K_ENTER); step(6000);              // arm again
    digits("0000");
    frame(); ok(!shows("DISARMATO"), "il vecchio PIN non funziona piu'");
    digits("4321");
    frame(); ok(shows("DISARMATO"), "il nuovo PIN disarma");

    // ================= 11. log =================
    printf("\n[11] log NDJSON su SD\n");
    char log[8000];
    ok(read_all("/sd/data/Alarm/alarm.ndjson", log, sizeof log), "il log esiste");
    ok(strstr(log, "\"ev\":\"armed\"") != NULL, "registra l'armo");
    ok(strstr(log, "\"ev\":\"trigger\"") != NULL, "registra lo scatto");
    ok(strstr(log, "\"ev\":\"rearm\"") != NULL, "registra il riarmo");
    ok(strstr(log, "\"ev\":\"rec_start\"") != NULL, "registra l'avvio della registrazione");
    ok(strstr(log, "\"ev\":\"rec_stop\"") != NULL, "registra la fine della registrazione");
    ok(strstr(log, "\"ev\":\"pin_bad\"") != NULL, "registra i PIN errati (manomissione)");
    ok(strstr(log, "\"bat\":77") != NULL, "ogni riga porta la batteria");

    // ================= 12. wiping the log needs the PIN ====================
    printf("\n[12] cancellazione log protetta da PIN\n");
    open_menu();
    for (int i = 0; i < 9; i++) press(K_DOWN);   // to "Cancella log" (row 10)
    press(K_ENTER);
    frame(); ok(shows("PIN"), "chiede il PIN prima di cancellare");
    digits("1111");
    ok(file_size("/sd/data/Alarm/alarm.ndjson") > 0, "PIN errato: il log resta");
    digits("4321");
    ok(read_all("/sd/data/Alarm/alarm.ndjson", log, sizeof log), "dopo il wipe il log esiste ancora");
    ok(strstr(log, "\"ev\":\"wiped\"") != NULL, "e contiene la riga 'wiped'");
    ok(strstr(log, "\"ev\":\"armed\"") == NULL, "il resto e' stato cancellato");
    press(K_TAB);

    // ================= 13. siren mode =====================================
    printf("\n[13] modo sirena: suona, non registra, si riarma\n");
    open_menu(); menu_row(2); press(K_RIGHT);    // back to Sirena
    frame(); ok(shows("Sirena"), "modo = Sirena");
    press(K_TAB);
    press(K_ENTER); step(8000);                    // 5 s countdown + the 1.5 s arm grace
    int before = g_siren_calls;
    g_mic_level_pct = 95;
    step(400);
    ok(g_siren_calls > before, "la sirena suona");
    ok(!g_mic_is_open, "il microfono e' rilasciato alla sirena");
    frame(); check_layout("allarme sirena");
    int stops = g_siren_stops;
    step(21000);
    ok(g_siren_stops > stops, "la sirena si ferma al riarmo automatico");
    frame(); ok(g_texts == 0 || screen_dark(), "e lo schermo torna spento");
    g_mic_level_pct = 0;
    step(4000);
    press(K_CHAR, 'z'); digits("4321");
    frame(); ok(shows("DISARMATO"), "disarmato");

    // ================= 14. no IMU + "Movimento" must still watch ===========
    printf("\n[14] board senza IMU con sorgente Movimento\n");
    open_menu(); menu_row(1); press(K_RIGHT);    // Sorgente -> Movimento
    press(K_TAB);
    press(K_ENTER); step(8000);
    frame(); ok(shows("ARMATO"), "si arma comunque (fallback microfono)");
    ok(g_mic_is_open, "il microfono sorveglia");
    press(K_CHAR, 'q'); digits("4321");

    // ================= 15. a mic that fails to open is retried =============
    printf("\n[15] microfono occupato: retry e avviso\n");
    open_menu(); menu_row(1); press(K_RIGHT); press(K_RIGHT);   // back to Microfono
    press(K_TAB);
    g_mic_open_ok = false;
    press(K_ENTER); step(8000);
    ok(!g_mic_is_open, "il mic non si apre (I2S occupato)");
    frame(); ok(shows("mic KO"), "lo dice a schermo invece di fingere di ascoltare");
    check_layout("armato mic KO");
    int opens = g_mic_opens;
    g_mic_open_ok = true;
    step(2000);
    ok(g_mic_opens > opens && g_mic_is_open, "riprova e recupera il microfono");
    press(K_CHAR, 'q'); digits("4321");

    g_app.on_exit();
    ok(!screen_dark(), "uscendo dall'app lo schermo non resta spento");
    ok(g_voice_suspended == 0, "e la voce non resta sospesa");

    printf("\n%d/%d verifiche superate\n", g_checks - g_fail, g_checks);
    return g_fail ? 1 : 0;
}
