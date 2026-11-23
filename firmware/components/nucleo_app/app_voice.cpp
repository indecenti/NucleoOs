// Voice — the one native app for the on-device voice engine (AVCEB): train words, bind each word to an app,
// test the recognizer. It replaces the former Voice Trainer + Voice Lab, which shared the same list of trained
// words and each cost its own Solo reboot to open.
//
// Tab 0 — COMMANDS : the trained words (/system/voice/*.tpl), each with the app it opens and how reliably it was
//                    recognized this session. Enter = pick the app to open, U = unbind, R = re-train, Del = delete.
// Tab 1 — TRAIN    : name a word, hold GO and say it.
// Tab 2 — TEST     : hold GO and speak; shows the word, the confidence verdict and what would happen. Test mode
//                    recognizes but does NOT act; E switches to "run" mode, where a bound word really opens its app.
// Tab 3 — INFO     : template count, routing, always-on.
//
// Runs as a Solo app (NX_SOLO): the recognizer needs a ~26 KB contiguous block on PTT, which only the fresh Solo
// heap gives (the full OS leaves ~18 KB). Fully offline. No network required.
#include "nucleo_app.h"
#include "app_ui.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

extern "C" {
#include "nucleo_voice.h"
#include "nucleo_anima.h"   // anima_action_t (LAUNCH/SYSTEM/ANSWER) for the result verb
#include "nucleo_storage.h"
#include "nucleo_ui.h"
#include "nucleo_board.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
}

#include "app_gfx.h"
#include "nucleo_exclusive.h"   // full-OS fallback only: free RAM so the recognizer's heap gate can pass
#include "nucleo_i18n.h"        // TR5(it,en,es,fr,de): UI labels follow the system language
#include "nucleo_theme.h"       // themed chrome (BG/FG/MUTED/DIM/LINE follow the active OS theme)
#include "app_large.h"          // two screens: the app on the 320x240 panel, the Cardputer as companion/PTT
#include "nucleo_touch.h"
#include "esp_timer.h"

// Chrome follows the active theme; ACC is the app's identity accent (its registered launcher color);
// the other colours are semantic status colours (named, never bare hex in a draw call).
#define BG    THEME_BG
#define FG    THEME_FG
#define MUTED THEME_MUTED
#define DIM   THEME_DIM
#define LINE  THEME_LINE
static const unsigned short
    ACC    = 0x07E0,   // lime green — app accent (registered)
    ACC2   = 0xFD20,   // orange for active states
    PANEL  = 0x10A2,   // result card
    C_RED  = 0xF800,   // status red
    REC_BG = 0x2000,   // capture box (live recording)
    OK_BG  = 0x03E0,   // success box (saved)
    ERR_BG = 0x5800,   // audio-error box
    C_OTT  = 0x07E0,   // Great  — green
    C_BUO  = 0x9FE0,   // Good   — yellow-green
    C_DIS  = 0xFD20,   // Fair   — orange
    C_DEB  = 0xF800;   // Weak   — red

#define TPL_PATH NUCLEO_SD_MOUNT "/system/voice"
#define MAX_TPLS 20
#define W 240
#define H 135

// WS client count (resolved at link from nucleo_ws, no REQUIRES cycle)
extern "C" int nucleo_ws_client_count(void);
// The app registry, for the "open with" picker (nucleo_app.cpp) and the localized launcher names.
int                nucleo_app_count(void);
extern "C" bool    nucleo_app_info_at(int i, const char **id, const char **title);
extern "C" const char *launcher_app_localized_name(const char *id);

// ── Confidence tiers (mirror apps/voice-manager/www/app.js classify()) ─────────────────────────────────────────
static const unsigned short TIER_COL[5] = { MUTED, C_DEB, C_DIS, C_BUO, C_OTT };
static const int            TIER_PCT[5] = { 0, 25, 48, 72, 92 };
static const char *tier_name(int t)
{
    switch (t) {
        case 0:  return TR5("non provato", "untested", "sin probar", "non teste", "ungetestet");
        case 1:  return TR5("Debole",   "Weak",   "Debil",     "Faible",  "Schwach");
        case 2:  return TR5("Discreto", "Fair",   "Discreto",  "Moyen",   "Mittel");
        case 3:  return TR5("Buono",    "Good",   "Bueno",     "Bon",     "Gut");
        default: return TR5("Ottimo",   "Great",  "Excelente", "Excellent", "Sehr gut");
    }
}
typedef struct { int tier; int pct; unsigned short col; } conf_t;
// Integer percent math, same logic as the web telemetry panel.
static conf_t classify(int32_t dist, int32_t second, int32_t radius)
{
    int margin = (second > 0) ? (int)((int64_t)dist * 100 / second) : 100;  // lower = clearer winner
    int ratio  = (radius > 0) ? (int)((int64_t)dist * 100 / radius) : 100;  // <100 = inside radius
    int t;
    if (ratio <= 60 || margin <= 45)       t = 4;
    else if (ratio <= 100 || margin <= 65) t = 3;
    else if (margin <= 75)                 t = 2;
    else                                   t = 1;
    conf_t c = { t, TIER_PCT[t], TIER_COL[t] };
    return c;
}

// ── State ────────────────────────────────────────────────────────────────────
enum { TAB_CMD = 0, TAB_TRAIN, TAB_TEST, TAB_INFO, TAB_N };
typedef struct { char name[32]; char app[24]; signed char tier; } tpl_t;   // word · bound app id · best tier this session

static int  s_tab  = TAB_CMD;
static int  s_sel  = 0;
static int  s_lg_scroll = 0;       // two screens: the list's first visible row
static bool s_confirm_del, s_del_yes;   // pending "delete?" card (tab CMD)
static tpl_t *s_t = nullptr;       // [MAX_TPLS], heap-allocated on enter (zero RAM at boot)
static int  s_tpl_count = 0;
static bool s_tpl_overflow = false;   // more than MAX_TPLS templates on SD (only the first MAX are shown/matched)

// "Open with" picker (tab CMD, modal)
typedef struct { const char *id; const char *label; } pick_t;
static bool    s_pick = false;
static pick_t *s_pk = nullptr;
static int     s_pk_n = 0, s_pk_sel = 0, s_pk_scroll = 0;

// Train-tab state
enum RecStep { RS_IDLE, RS_WAITING_FN, RS_DONE, RS_TOO_SHORT };
static RecStep s_rec_step  = RS_IDLE;
static char    s_rec_word[32] = "";
static int     s_rec_timer = 0;  // ticks countdown for feedback display

// Test-tab state
static bool     s_live = false;      // false = test mode (recognize, do not act); true = a bound word opens its app
static uint32_t s_seen_match = 0, s_seen_result = 0;
static int      s_recog = 0;         // recognitions this session
static bool     s_have_m = false, s_have_r = false;
static nucleo_voice_match_t  s_m;
static nucleo_voice_result_t s_r;

// ── Helpers ──────────────────────────────────────────────────────────────────
// The name the launcher shows for an app id (localized), else its registry title, else the id itself.
static const char *app_label(const char *id)
{
    if (!id || !id[0]) return "";
    for (int i = 0, n = nucleo_app_count(); i < n; i++) {
        const char *aid, *title;
        if (nucleo_app_info_at(i, &aid, &title) && aid && !strcmp(aid, id)) {
            const char *loc = launcher_app_localized_name(aid);
            return loc ? loc : (title ? title : id);
        }
    }
    return id;
}

static void scan_tpls(void)
{
    if (!s_t) { s_tpl_count = 0; return; }   // buffer freed (app not active) — nothing to scan into
    tpl_t *old = (tpl_t *)malloc(sizeof(tpl_t) * MAX_TPLS);   // keep this session's reliability across a rescan
    int old_n = s_tpl_count;
    if (old) memcpy(old, s_t, sizeof(tpl_t) * MAX_TPLS); else old_n = 0;
    s_tpl_count = 0;
    s_tpl_overflow = false;
    mkdir(TPL_PATH, 0775);
    DIR *dir = opendir(TPL_PATH);
    if (dir) {
        struct dirent *e;
        while ((e = readdir(dir))) {
            const char *dot = strrchr(e->d_name, '.');
            if (!dot || strcmp(dot, ".tpl")) continue;
            if (s_tpl_count >= MAX_TPLS) { s_tpl_overflow = true; continue; }   // count past the cap to warn
            tpl_t *t = &s_t[s_tpl_count];
            memset(t, 0, sizeof *t);
            int len = (int)(dot - e->d_name);
            if (len >= (int)sizeof t->name) len = (int)sizeof t->name - 1;
            memcpy(t->name, e->d_name, len);
            nucleo_voice_binding_get(t->name, t->app, sizeof t->app);
            for (int k = 0; k < old_n; k++) if (!strcmp(old[k].name, t->name)) { t->tier = old[k].tier; break; }
            s_tpl_count++;
        }
        closedir(dir);
    }
    free(old);
    if (s_sel >= s_tpl_count) s_sel = s_tpl_count > 0 ? s_tpl_count - 1 : 0;
}

static void pick_close(void) { s_pick = false; free(s_pk); s_pk = nullptr; s_pk_n = 0; }

static void pick_open(void)
{
    if (s_tpl_count == 0) return;
    int n = nucleo_app_count();
    s_pk = (pick_t *)malloc(sizeof(pick_t) * (n > 0 ? n : 1));
    if (!s_pk) return;
    s_pk_n = 0; s_pk_sel = 0; s_pk_scroll = 0;
    const char *cur = s_t[s_sel].app;
    for (int i = 0; i < n; i++) {
        const char *id, *title;
        if (!nucleo_app_info_at(i, &id, &title) || !id || !id[0] || !strcmp(id, "voice")) continue;
        if (!strcmp(id, cur)) s_pk_sel = s_pk_n;   // start on the app already bound
        s_pk[s_pk_n].id = id; s_pk[s_pk_n].label = app_label(id);
        s_pk_n++;
    }
    s_pick = s_pk_n > 0;
    if (!s_pick) pick_close();
}

static void pick_choose(void)
{
    if (!s_pick || s_pk_sel >= s_pk_n || s_sel >= s_tpl_count) return;
    tpl_t *t = &s_t[s_sel];
    if (nucleo_voice_binding_set(t->name, s_pk[s_pk_sel].id) == ESP_OK)
        snprintf(t->app, sizeof t->app, "%s", s_pk[s_pk_sel].id);
    pick_close();
}

static void unbind_selected(void)
{
    if (s_sel >= s_tpl_count || !s_t[s_sel].app[0]) return;
    nucleo_voice_binding_set(s_t[s_sel].name, "");
    s_t[s_sel].app[0] = 0;
}

static void clear_bench(void) { s_have_m = s_have_r = false; s_recog = 0; }

static void set_tab(int t)
{
    s_tab = ((t % TAB_N) + TAB_N) % TAB_N;
    s_confirm_del = false;
    pick_close();
    if (s_tab == TAB_CMD) scan_tpls();
    if (s_tab == TAB_TRAIN) { s_rec_step = RS_IDLE; s_rec_word[0] = '\0'; }
    nucleo_app_request_draw();
}

static void set_live(bool on)
{
    s_live = on;
    nucleo_voice_set_test_mode(!on);   // test mode ON = recognize but DON'T act
    nucleo_voice_set_bound_only(on);   // run mode: only a bound word acts (no TTS / ANIMA worker in the Solo boot)
    nucleo_app_request_draw();
}

// ── App lifecycle ─────────────────────────────────────────────────────────────
static void on_tab(void) { set_tab(s_tab + 1); }

// Back/Esc closes an open modal (picker, delete card) instead of leaving the app.
static bool on_back(int)
{
    if (s_pick)        { pick_close();            nucleo_app_request_draw(); return true; }
    if (s_confirm_del) { s_confirm_del = false;   nucleo_app_request_draw(); return true; }
    return false;
}

static void enter(void)
{
    // Solo boot (NX_SOLO in the def, below): a fresh heap with ~32 KB contiguous, which the recognizer needs
    // (a 26 KB block on PTT + a 30 KB ANIMA worker). httpd/mDNS/L1 never started there, so skip exclusive_enter
    // (its exit would start httpd from scratch right before the warm reboot). The full-OS path is never taken
    // once NX_SOLO is set, but kept correct.
    if (!nucleo_anima_solo_active())
        nucleo_exclusive_enter(NX_HTTPD | NX_ANIMA_L1 | NX_DISCOVERY, nullptr);
    ESP_LOGI("voice", "enter: solo=%d free=%u largest=%u", (int)nucleo_anima_solo_active(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    nucleo_app_set_tab_handler(on_tab);
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_hint(TR5("TAB schede  Invio apri con  R addestra", "TAB tabs  Enter open with  R train",
                            "TAB pestanas  Intro abrir con  R entrena", "TAB onglets  Entree ouvrir  R entrainer",
                            "TAB Reiter  Enter oeffne mit  R lernen"));
    s_tab = TAB_CMD; s_sel = 0; s_confirm_del = false; s_pick = false;
    s_live = false; clear_bench();
    nucleo_voice_set_test_mode(true);   // recognize but DON'T act until the user switches to run mode (E)
    nucleo_voice_set_bound_only(false);
    nucleo_voice_request(true);         // hold the lazy engine up while training/testing here
    // Don't replay a stale match/result left over from a previous session.
    nucleo_voice_match_t m;  s_seen_match  = nucleo_voice_last_match(&m)  ? m.seq : 0;
    nucleo_voice_result_t r; s_seen_result = nucleo_voice_last_result(&r) ? r.seq : 0;
    if (!s_t) s_t = (tpl_t *)calloc(MAX_TPLS, sizeof(tpl_t));
    scan_tpls();
}

static void exit_app(void)
{
    pick_close();
    s_rec_step = RS_IDLE;
    nucleo_voice_set_test_mode(false);   // restore normal dispatch for the rest of the OS
    nucleo_voice_set_bound_only(false);
    // Release the app hold. The engine frees its 16 KB UNLESS the user explicitly
    // pinned "always listen" (INFO tab) — that opt-in survives leaving the app.
    nucleo_voice_request(false);
    if (nucleo_exclusive_active()) nucleo_exclusive_exit();   // bring httpd / L1 / mDNS back (guarded, like app_anima)
    free(s_t); s_t = nullptr;   // zero RAM while the app is closed
    s_tpl_count = 0;
}

static void tick(void)
{
    // Countdown for feedback message on the TRAIN tab
    if (s_rec_timer > 0) { s_rec_timer--; if (s_rec_timer == 0) nucleo_app_request_draw(); }

    // While waiting for a recording, POLL the engine for the outcome — the live event sink
    // (voice/learned) is owned by the WebSocket layer, so a native app cannot subscribe; it
    // must poll. Without this the screen used to freeze on "LISTENING" forever.
    if (s_rec_step == RS_WAITING_FN) {
        nucleo_voice_learn_t lr = nucleo_voice_take_learn_result();
        if (lr == NUCLEO_VOICE_LEARN_OK)             { s_rec_step = RS_DONE;      scan_tpls(); }
        else if (lr == NUCLEO_VOICE_LEARN_TOO_SHORT) { s_rec_step = RS_TOO_SHORT; }
        nucleo_app_request_draw();   // also keeps the pulsing-mic animation alive (~5x/s)
    }

    // Recognition telemetry: the matched word (+ its reliability) and what the engine did with the sentence.
    bool changed = false;
    nucleo_voice_match_t m;
    if (nucleo_voice_last_match(&m) && m.seq != s_seen_match) {
        s_seen_match = m.seq; s_m = m; s_have_m = true; s_recog++;
        conf_t c = classify(m.dist, m.second, m.radius);
        for (int i = 0; i < s_tpl_count; i++)
            if (!strcmp(s_t[i].name, m.word)) { if (c.tier > s_t[i].tier) s_t[i].tier = (signed char)c.tier; break; }
        changed = true;
    }
    nucleo_voice_result_t r;
    if (nucleo_voice_last_result(&r) && r.seq != s_seen_result) {
        s_seen_result = r.seq; s_r = r; s_have_r = true; changed = true;
    }
    if (changed) nucleo_app_request_draw();
}

// ── Input ─────────────────────────────────────────────────────────────────────
static const char *pk_label(int i, void *) { return (i >= 0 && i < s_pk_n) ? s_pk[i].label : ""; }

static void on_key(int key, char ch)
{
    if (key == NK_CHAR && ch >= 1 && ch <= TAB_N) { set_tab(ch - 1); return; }   // panel tab tap: keys 1..4

    // The "open with" picker owns every key while it is up.
    if (s_pick) {
        if (nucleo_app_two_screens()) {
            int t = lg::row_tap(key, ch, s_pk_scroll, s_pk_n, &s_pk_sel);
            if (t == 2) { pick_choose(); nucleo_app_request_draw(); return; }
            if (t == 1) { nucleo_app_request_draw(); return; }
        }
        if (key == NK_ENTER) { pick_choose(); nucleo_app_request_draw(); return; }
        if (app_ui_list_key(key, ch, &s_pk_sel, s_pk_n, pk_label, nullptr)) nucleo_app_request_draw();
        return;
    }

    // ─ TAB COMMANDS ──────────────────────────────────────────────────────────
    if (s_tab == TAB_CMD) {
        if (s_confirm_del) {                          // delete card is up: it owns every key
            int r = app_ui_confirm_key(key, ch, &s_del_yes);
            if (r == 1 && s_tpl_count > 0 && s_sel < s_tpl_count) {
                char path[128];
                snprintf(path, sizeof(path), "%s/%s.tpl", TPL_PATH, s_t[s_sel].name);
                unlink(path);
                nucleo_voice_binding_set(s_t[s_sel].name, "");   // a deleted word leaves no stale binding behind
                scan_tpls();
            }
            if (r >= 0) s_confirm_del = false;
            nucleo_app_request_draw();
            return;
        }
        if (nucleo_app_two_screens()) {               // panel: tap a row to pick, tap the card to choose its app
            int t = lg::row_tap(key, ch, s_lg_scroll, s_tpl_count, &s_sel);
            if (t == 1) { nucleo_app_request_draw(); return; }
            if (t == 2) { key = NK_ENTER; ch = 0; }   // a second tap on the same row = pick its app
        }
        if (key == NK_ENTER || ch == 'a' || ch == 'A') {
            pick_open();
        } else if (app_ui_list_key(key, ch, &s_sel, s_tpl_count, nullptr, nullptr)) {
            // handled
        } else if (key == NK_DEL || ch == 'd' || ch == 'D') {
            if (s_tpl_count > 0 && s_sel < s_tpl_count) { s_confirm_del = true; s_del_yes = false; }
        } else if (ch == 'u' || ch == 'U') {
            unbind_selected();
        } else if (ch == 'r' || ch == 'R') {
            // Jump to the TRAIN tab to (re)train. Pre-fill the selected word's name so re-training a
            // command is one keypress, not retyping it.
            s_tab = TAB_TRAIN;
            s_rec_step = RS_IDLE;
            if (s_tpl_count > 0 && s_sel < s_tpl_count) snprintf(s_rec_word, sizeof(s_rec_word), "%s", s_t[s_sel].name);
            else s_rec_word[0] = '\0';
        } else {
            return;
        }
        nucleo_app_request_draw();
        return;
    }

    // ─ TAB TRAIN ─────────────────────────────────────────────────────────────
    if (s_tab == TAB_TRAIN) {
        if (s_rec_step == RS_IDLE) {
            // Prompt for the word name, pre-filled if we arrived via 'R' on a selected template.
            char word[32] = "";
            snprintf(word, sizeof(word), "%s", s_rec_word);
            nucleo_ui_input(TR5("Nome parola (es: anima)", "Word name (e.g. anima)", "Palabra (ej: anima)", "Nom du mot (ex: anima)", "Wortname (z.B. anima)"), word, sizeof(word), 0);
            if (!word[0]) { nucleo_app_request_draw(); return; }
            snprintf(s_rec_word, sizeof(s_rec_word), "%s", word);
            nucleo_voice_arm_learning_mode(s_rec_word);
            s_rec_step = RS_WAITING_FN;
        } else if (s_rec_step == RS_DONE || s_rec_step == RS_TOO_SHORT) {
            s_rec_step = RS_IDLE;                    // any key resets
            s_rec_word[0] = '\0';
            scan_tpls();
        } else {
            return;
        }
        nucleo_app_request_draw();
        return;
    }

    // ─ TAB TEST ──────────────────────────────────────────────────────────────
    if (s_tab == TAB_TEST) {
        if (ch == 'e' || ch == 'E') { set_live(!s_live); return; }
        if (key == NK_DEL || ch == 'c' || ch == 'C') { clear_bench(); nucleo_app_request_draw(); }
        return;
    }

    // ─ TAB INFO ──────────────────────────────────────────────────────────────
    if (ch == 'a' || ch == 'A') {
        // Persistent "always on": pins the engine up (PTT works from the home screen) and writes
        // voice.alwaysOn to settings.json. Survives reboot.
        nucleo_voice_set_always_on(!nucleo_voice_always_on());
        nucleo_app_request_draw();
    }
}

// ── Compact (Cardputer) drawing ───────────────────────────────────────────────
static const char *tab_label(int i)
{
    switch (i) {
        case TAB_CMD:   return TR5("COMANDI",  "COMMANDS", "COMANDOS", "ORDRES",   "BEFEHLE");
        case TAB_TRAIN: return TR5("ADDESTRA", "TRAIN",    "ENTRENAR", "ENTRAIN.", "LERNEN");
        case TAB_TEST:  return TR5("PROVA",    "TEST",     "PRUEBA",   "TEST",     "TEST");
        default:        return "INFO";
    }
}

static void draw_tabs(int top_y)
{
    const int tw = W / TAB_N;
    for (int i = 0; i < TAB_N; i++) {
        const char *lab = tab_label(i);
        bool active = (i == s_tab);
        unsigned short bg = active ? ACC : BG;
        unsigned short fg = active ? BG  : DIM;
        d.fillRoundRect(i * tw + 3, top_y + 2, tw - 6, 18, 6, bg);
        d.setTextColor(fg, bg);
        d.setTextSize(1);
        d.setCursor(i * tw + 3 + (tw - 6 - (int)strlen(lab) * 6) / 2, top_y + 7);
        d.print(lab);
    }
    d.drawFastHLine(0, top_y + 22, W, LINE);
}

static const char *tl_label(int i, void *) { return (s_t && i >= 0 && i < s_tpl_count) ? s_t[i].name : ""; }
static const char *tl_right(int i, void *)
{
    if (!s_t || i < 0 || i >= s_tpl_count) return "";
    if (s_t[i].app[0]) return app_label(s_t[i].app);
    return s_t[i].tier ? tier_name(s_t[i].tier) : "";   // untested words stay quiet: no "untested" on every row
}
static unsigned short tl_color(int i, void *)
{
    if (!s_t || i < 0 || i >= s_tpl_count) return MUTED;
    return s_t[i].app[0] ? ACC : TIER_COL[(int)s_t[i].tier];
}

static void draw_cmds(int top_y)
{
    draw_tabs(top_y);
    int y0 = top_y + 26;
    int h  = nucleo_app_content_height() - 26 - 16;

    if (s_pick) {   // the "open with" picker replaces the list
        d.setTextSize(1); d.setTextColor(ACC, BG); d.setCursor(8, y0 + 2);
        char hd[72]; snprintf(hd, sizeof hd, TR5("\"%s\" apre:", "\"%s\" opens:", "\"%s\" abre:", "\"%s\" ouvre :", "\"%s\" oeffnet:"), s_t[s_sel].name);
        d.print(hd);
        app_ui_list(y0 + 14, h - 14, s_pk_n, s_pk_sel, pk_label, nullptr, nullptr, nullptr);
        return;
    }
    if (s_tpl_count == 0) {
        d.setTextSize(1); d.setTextColor(DIM, BG);
        d.setCursor(12, y0 + 14); d.print(TR5("Nessuna parola addestrata.", "No trained words.", "Sin palabras entrenadas.", "Aucun mot appris.", "Keine Woerter gelernt."));
        d.fillRoundRect(12, y0 + 34, W - 24, 22, 6, ACC);
        d.setTextColor(BG, ACC);
        d.setCursor(W / 2 - 60, y0 + 41); d.print(TR5("Premi R per addestrare", "Press R to train", "Pulsa R entrenar", "Appuyez R entrainer", "Druecke R trainiere"));
        return;
    }
    app_ui_list(y0, h, s_tpl_count, s_sel, tl_label, tl_right, tl_color, nullptr);

    if (s_confirm_del && s_sel < s_tpl_count) {
        app_ui_confirm(TR5("Elimina parola?", "Delete word?", "Borrar palabra?", "Supprimer mot ?", "Wort loeschen?"), s_t[s_sel].name, s_del_yes);
        return;
    }
    // Footer: what Enter does for the selected word
    int fy = nucleo_app_content_top() + nucleo_app_content_height() - 16;
    d.fillRect(0, fy, W, 16, BG); d.drawFastHLine(0, fy, W, LINE);
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(8, fy + 4);
    d.print(s_t[s_sel].app[0] ? TR5("Invio cambia app  U scollega", "Enter change app  U unbind", "Intro cambia  U desvincula", "Entree change  U delie", "Enter aendern  U loesen")
                              : TR5("Invio: scegli l'app da aprire", "Enter: pick the app to open", "Intro: elige la app", "Entree : choisir l'app", "Enter: App waehlen"));
}

static void draw_train(int top_y)
{
    draw_tabs(top_y);
    int y = top_y + 28;

    switch (s_rec_step) {
    case RS_IDLE:
        d.fillRoundRect(8, y, W - 16, 80, 8, LINE);
        d.setTextColor(FG, LINE); d.setCursor(16, y + 8); d.setTextSize(2);
        d.print(TR5("Nuova Parola", "New Word", "Nueva Palab", "Nouveau Mot", "Neues Wort"));
        d.setTextSize(1); d.setTextColor(ACC, LINE); d.setCursor(16, y + 30);
        d.print(TR5("1. Premi Invio e scrivi nome", "1. Press Enter, type a name", "1. Pulsa INTRO, escribe nom", "1. Appuyez ENTREE, tapez nom", "1. Druecke ENTER, gib Namen"));
        d.setTextColor(DIM, LINE); d.setCursor(16, y + 46);
        d.print(TR5("2. Tieni premuto GO e parla", "2. Hold GO and speak", "2. Manten GO y habla", "2. Appuyez GO et parlez", "2. Halte GO und sprich"));
        d.setTextColor(DIM, LINE); d.setCursor(16, y + 62);
        d.print(TR5("100% Offline (nessun WAV)", "100% offline (no WAV)", "100% Sin WAV", "100% Hors ligne (no WAV)", "100% Offline (kein WAV)"));
        break;

    case RS_WAITING_FN: {
        // Honest state: the engine is only truly capturing while GO is held (is_listening). Until
        // then we tell the user to HOLD GO instead of a misleading "listening" with a fake pulse.
        bool listening = nucleo_voice_is_listening();
        unsigned short box = listening ? REC_BG : LINE;
        d.fillRoundRect(8, y, W - 16, 80, 8, box);
        d.setTextColor(listening ? ACC2 : ACC, box); d.setTextSize(2); d.setCursor(16, y + 8);
        d.print(listening ? TR5("IN ASCOLTO...", "LISTENING...", "ESCUCHANDO...", "ECOUTE...", "HOERT..") : TR5("TIENI GO e parla", "HOLD GO & speak", "MANTEN GO y habla", "TIENS GO et parle", "HALTE GO & sprich"));
        d.setTextSize(1); d.setTextColor(FG, box); d.setCursor(16, y + 30);
        d.print(TR5("Parola:", "Word:", "Palabra:", "Mot:", "Wort:"));
        d.setTextColor(ACC, box); d.setTextSize(2); d.setCursor(16, y + 44);
        d.print(s_rec_word);
        // Mic icon pulses ONLY while truly capturing (real feedback); a static ring while it waits.
        if (listening) {
            int64_t t = esp_timer_get_time() / 200000;
            d.fillCircle(W - 30, y + 40, 10 + (int)(t % 4), (t & 1) ? C_RED : ACC2);
            d.fillCircle(W - 30, y + 40, 6, FG);
        } else {
            d.drawCircle(W - 30, y + 40, 11, MUTED);
            d.fillCircle(W - 30, y + 40, 5, DIM);
        }
        break;
    }

    case RS_DONE:
        d.fillRoundRect(8, y, W - 16, 80, 8, OK_BG);
        d.setTextSize(2); d.setTextColor(ACC, OK_BG); d.setCursor(16, y + 16);
        d.print(TR5("SALVATO!", "SAVED!", "GUARDADO!", "ENREGISTRE", "GESPEICHERT!"));
        d.setTextSize(1); d.setTextColor(FG, OK_BG); d.setCursor(16, y + 44);
        char msg2[48]; snprintf(msg2, sizeof(msg2), TR5("'%s' salvata.", "'%s' saved.", "'%s' guardada.", "'%s' enregistre.", "'%s' gespeichert."), s_rec_word);
        d.print(msg2);
        d.setTextColor(MUTED, OK_BG); d.setCursor(16, y + 60);
        d.print(TR5("Poi in COMANDI: Invio = app", "Then COMMANDS: Enter = app", "Luego COMANDOS: Intro", "Puis ORDRES : Entree", "Dann BEFEHLE: Enter"));
        break;

    case RS_TOO_SHORT:
        d.fillRoundRect(8, y, W - 16, 80, 8, ERR_BG);
        d.setTextSize(2); d.setTextColor(C_RED, ERR_BG); d.setCursor(16, y + 12);
        d.print(TR5("ERRORE AUDIO", "AUDIO ERROR", "ERROR AUDIO", "ERREUR AUDIO", "AUDIOFEHLER"));
        d.setTextSize(1); d.setTextColor(FG, ERR_BG); d.setCursor(16, y + 36);
        d.print(TR5("Troppo corto o silenzio.", "Too short or silent.", "Muy corto o silencio.", "Trop court ou silence.", "Zu kurz oder stumm."));
        d.setTextColor(MUTED, ERR_BG); d.setCursor(16, y + 54);
        d.print(TR5("Tieni GO piu a lungo.", "Hold GO longer.", "Manten GO mas tiempo.", "Tenez GO plus long.", "Halte GO laenger."));
        break;
    }
}

static void conf_bar(int x, int y, int w, int pct, unsigned short col)
{
    d.drawRoundRect(x, y, w, 8, 3, LINE);
    int fw = w * pct / 100; if (fw < 0) fw = 0; if (fw > w) fw = w;
    if (fw > 0) d.fillRoundRect(x, y, fw, 8, 3, col);
}

// What the engine did with the sentence: the verb line for the result card.
static const char *result_verb(void)
{
    if (s_r.action == ANIMA_ACT_LAUNCH) {
        if (s_live) return s_r.launched ? TR5("Avviata", "Opened", "Abierta", "Ouverte", "Geoeffnet")
                                        : TR5("Non trovata", "Not found", "No encontrada", "Introuvable", "Nicht gefunden");
        return TR5("Aprirebbe", "Would open", "Abriria", "Ouvrirait", "Wuerde oeffnen");
    }
    if (s_r.action == ANIMA_ACT_SYSTEM) return TR5("Stato", "Status", "Estado", "Statut", "Status");
    if (!s_r.matched)                   return TR5("Nessun comando", "No command", "Sin comando", "Pas de commande", "Kein Befehl");
    return TR5("Risposta", "Answer", "Respuesta", "Reponse", "Antwort");
}
// For a launch the reply is the app id: show its name.
static const char *result_text(void)
{
    if (!s_r.matched) return "-";
    return s_r.action == ANIMA_ACT_LAUNCH ? app_label(s_r.reply) : s_r.reply;
}
static const char *mode_label(void)
{
    return s_live ? TR5("ESEGUI", "RUN", "EJECUTA", "EXECUTE", "AUSFUEHREN") : TR5("PROVA", "TEST", "PRUEBA", "TEST", "TEST");
}

static void draw_test(int top_y)
{
    draw_tabs(top_y);
    int y = top_y + 28;

    // Mode chip, right: the mode is the safety — it must always be visible. E switches it.
    d.setTextSize(1);
    d.setTextColor(s_live ? BG : MUTED, s_live ? ACC2 : LINE);
    const char *ml = mode_label();
    int mw = (int)strlen(ml) * 6 + 10;
    d.fillRoundRect(W - 8 - mw, top_y + 24, mw, 12, 5, s_live ? ACC2 : LINE);
    d.setCursor(W - 8 - mw + 5, top_y + 26); d.print(ml);
    y += 8;

    if (!s_have_m) {
        d.fillRoundRect(8, y, W - 16, 64, 8, PANEL);
        d.setTextSize(2); d.setTextColor(ACC, PANEL); d.setCursor(16, y + 6); d.print(TR5("Tieni GO", "Hold GO", "Manten GO", "Tenez GO", "Halte GO"));
        d.setTextColor(FG, PANEL); d.setCursor(16, y + 26); d.print(TR5("e parla", "and speak", "y habla", "et parlez", "und sprich"));
        d.setTextSize(1); d.setTextColor(MUTED, PANEL); d.setCursor(16, y + 48);
        d.print(s_live ? TR5("Modo ESEGUI: apre le app.", "RUN mode: opens the apps.", "Modo EJECUTA: abre apps.", "Mode EXECUTE: ouvre.", "AUSFUEHREN: oeffnet.")
                       : TR5("Modo prova: NON agisce.", "Test mode: does NOT act.", "Modo prueba: NO actua.", "Mode test: N'agit PAS.", "Testmodus: handelt NICHT."));
        d.setTextColor(DIM, BG); d.setCursor(8, top_y + nucleo_app_content_height() - 14);
        d.print(TR5("E cambia modo  C pulisci", "E switch mode  C clear", "E cambia modo  C limpia", "E change mode  C efface", "E Modus  C leeren"));
        return;
    }

    conf_t c = classify(s_m.dist, s_m.second, s_m.radius);
    d.fillRoundRect(8, y, W - 16, 40, 8, PANEL);
    d.setTextSize(2); d.setTextColor(FG, PANEL);
    char w[20]; snprintf(w, sizeof(w), "%.12s", s_m.word);
    d.setCursor(14, y + 5); d.print(w);
    const char *vl = tier_name(c.tier);
    int vw = (int)strlen(vl) * 6 + 10;
    d.fillRoundRect(W - 8 - vw - 6, y + 5, vw, 14, 6, c.col);
    d.setTextSize(1); d.setTextColor(BG, c.col); d.setCursor(W - 8 - vw - 6 + 5, y + 8); d.print(vl);
    conf_bar(14, y + 28, W - 28, c.pct, c.col);

    int ry = y + 46;
    if (s_have_r) {
        d.setTextColor(ACC, BG); d.setCursor(12, ry); d.print(result_verb());
        d.setTextColor(FG, BG);  d.setCursor(12, ry + 11);
        char rb[34]; snprintf(rb, sizeof(rb), "%.32s", result_text());
        d.print(rb);
    }
    d.setTextColor(MUTED, BG); d.setCursor(12, ry + 24);
    char tm[40]; snprintf(tm, sizeof(tm), "d %ld  2o %ld  r %ld", (long)s_m.dist, (long)s_m.second, (long)s_m.radius);
    d.print(tm);
    char cnt[18]; snprintf(cnt, sizeof(cnt), "n.%d", s_recog);
    d.setCursor(W - (int)strlen(cnt) * 6 - 12, ry + 24); d.print(cnt);
}

static void draw_info(int top_y)
{
    draw_tabs(top_y);
    int y = top_y + 26;
    int ws = nucleo_ws_client_count();
    bool aon = nucleo_voice_always_on();
    struct { const char *k; const char *v; unsigned short col; } rows[] = {
        { TR5("PAROLE", "WORDS", "PALABRAS", "MOTS", "WOERTER"), nullptr, ACC },
        { "WS CLIENT", nullptr, ws > 0 ? ACC : MUTED },
        { "ROUTING",   ws > 0 ? "Web" : TR5("Locale", "Local", "Local", "Local", "Lokal"), ws > 0 ? ACC2 : ACC },
        { TR5("LINGUA", "LANGUAGE", "IDIOMA", "LANGUE", "SPRACHE"), "IT EN ES FR DE", MUTED },
        { TR5("SEMPRE ON", "ALWAYS ON", "SIEMPRE ON", "TOUJOURS ON", "IMMER AN"), aon ? TR5("Si", "Yes", "Si", "Oui", "Ja") : "No", aon ? ACC2 : MUTED },
    };
    const int NROWS = (int)(sizeof(rows) / sizeof(rows[0]));
    char tpl_str[24], ws_str[8];
    if (s_tpl_overflow) snprintf(tpl_str, sizeof(tpl_str), "%d+ (max %d!)", s_tpl_count, MAX_TPLS);
    else                snprintf(tpl_str, sizeof(tpl_str), "%d / %d", s_tpl_count, MAX_TPLS);
    snprintf(ws_str, sizeof(ws_str), "%d", ws);
    rows[0].v = tpl_str;
    if (s_tpl_overflow) rows[0].col = ACC2;   // warn: only the first MAX_TPLS are matched
    rows[1].v = ws_str;

    d.fillRoundRect(8, y, W - 16, 12 + NROWS * 16, 8, LINE);
    for (int i = 0; i < NROWS; i++) {
        int ry = y + 6 + i * 16;
        d.setTextSize(1); d.setTextColor(MUTED, LINE); d.setCursor(16, ry); d.print(rows[i].k);
        d.setTextColor(rows[i].col, LINE);             d.setCursor(104, ry); d.print(rows[i].v);
    }
    d.setTextSize(1); d.setTextColor(MUTED, BG);
    d.setCursor(16, y + 12 + NROWS * 16 + 6);
    d.print(TR5("A = Sempre attiva (PTT da home)", "A = Always on (PTT from home)", "A = Siempre on (PTT inicio)", "A = Toujours on (PTT accueil)", "A = Immer an (PTT Startseite)"));
}

// ===== two screens ==============================================================================================
// The external panel carries the whole app: the word list + a detail card (COMMANDS, with the app each word
// opens and the "open with" picker), a big training card (TRAIN), the test bench (TEST) and the status rows (INFO).
// The Cardputer is the companion and, crucially, where the typing + the GO push-to-talk happen. Touch is on the
// content: a tab picks it, a row taps to pick and again to choose its app, the card's mic = re-train, its bin =
// delete, its chain = unbind, the training card = type the name / continue, the bench = clear, the mode switch.
static void lg_mic(int x, int y, int r, unsigned short ring, unsigned short dot)   // the mic disc: ring + core
{
    d.fillCircle(x, y, r, ring); d.fillCircle(x, y, r * 2 / 5, dot);
}

static void lg_tpl_row(int i, int x, int y, int w, int h, bool focus, unsigned short bg, void *)
{
    lg::row_text(x, y, w, h, focus, bg, s_t ? s_t[i].name : "?", s_t && s_t[i].app[0] ? ">" : nullptr, s_t && s_t[i].app[0] ? ACC : FG);
}

static void lg_pick(int yb, int Wd, int Hd)
{
    lg::text_list(0, yb + 2, Wd, Hd - yb - 4, s_pk_n, s_pk_sel, &s_pk_scroll, pk_label, nullptr, nullptr, nullptr, ACC);
}

static void lg_tab_cmds(int yb, int Wd, int Hd)
{
    if (s_tpl_count == 0) {
        lg::message(0, yb, Wd, Hd - yb, TR5("Nessuna parola", "No words", "Sin palabras", "Aucun mot", "Keine Woerter"),
                    TR5("Vai su Addestra per crearne una.", "Go to Train to create one.", "Ve a Entrenar para crear una.", "Allez sur Entrainer.", "Gehe zu Lernen."), MUTED);
        return;
    }
    const int listw = 150;
    lg::list(0, yb + 2, listw, Hd - yb - 4, lg::ROW, s_tpl_count, s_sel, &s_lg_scroll, lg_tpl_row, nullptr, ACC);
    d.fillRect(listw, yb, Wd - listw, Hd - yb, BG);
    const int px = listw + 4, py = yb + 6, pw = Wd - listw - 10, ph = Hd - yb - 12;
    lg::panel(px, py, pw, ph);
    const unsigned short cb = lg::card();
    const tpl_t *t = &s_t[s_sel];
    lg::font(2); d.setTextColor(ACC, cb);
    char nm[40]; lg::fit(nm, sizeof nm, t->name, pw - 20); d.drawString(nm, px + 12, py + 10);
    // reliability this session
    lg::font(0); d.setTextColor(TIER_COL[(int)t->tier], cb);
    char tn[40]; lg::fit(tn, sizeof tn, tier_name(t->tier), pw - 24); d.drawString(tn, px + 12, py + 38);
    // the app it opens: the card's main action (tap = pick / change it)
    lg::font(0); d.setTextColor(MUTED, cb); d.drawString(TR5("Apre", "Opens", "Abre", "Ouvre", "Oeffnet"), px + 12, py + 62);
    lg::font(1); d.setTextColor(t->app[0] ? FG : DIM, cb);
    char an[40]; lg::fit(an, sizeof an, t->app[0] ? app_label(t->app) : TR5("(nessuna: tocca)", "(none: tap)", "(ninguna: toca)", "(aucune : touchez)", "(keine: tippen)"), pw - 24);
    d.drawString(an, px + 12, py + 80);
    // bottom row: re-train mic, unbind, delete (registered first: they win over the card)
    const int by = py + ph - 40;
    d.fillRoundRect(px + pw - 40, by, 32, 32, 8, C_RED);
    d.fillRect(px + pw - 31, by + 12, 14, 13, 0xFFFF); d.fillRect(px + pw - 33, by + 8, 18, 3, 0xFFFF); d.fillRect(px + pw - 27, by + 5, 6, 3, 0xFFFF);
    nucleo_touch_add(px + pw - 46, by - 6, 44, 44, NK_DEL, 0);
    lg_mic(px + 24, by + 16, 16, ACC, BG);
    nucleo_touch_add(px + 4, by - 6, 44, 44, NK_CHAR, 'r');
    if (t->app[0]) {
        lg::button(px + 54, by, pw - 54 - 50, 32, TR5("Togli", "Unbind", "Quitar", "Delier", "Loesen"), LINE, FG, NK_CHAR, 'u');
    }
    nucleo_touch_add(px, py, pw, ph - 48, NK_ENTER, 0);   // the rest of the card: pick the app
    if (s_confirm_del) {                                  // modal: only its own two buttons take taps (first target wins)
        nucleo_touch_clear();
        const int cw = Wd - 80, cx = 40, chh = 120, cy = (Hd - chh) / 2;
        lg::panel(cx, cy, cw, chh, LINE); d.drawRoundRect(cx, cy, cw, chh, 8, C_RED);
        lg::font(1); d.setTextColor(C_RED, LINE); d.drawString(TR5("Eliminare la parola?", "Delete the word?", "Borrar la palabra?", "Supprimer le mot ?", "Wort loeschen?"), cx + 16, cy + 12);
        lg::font(0); d.setTextColor(FG, LINE); char dn[40]; lg::fit(dn, sizeof dn, s_t[s_sel].name, cw - 32); d.drawString(dn, cx + 16, cy + 40);
        const int bw = (cw - 48) / 2;
        lg::button(cx + 16, cy + chh - 46, bw, 34, TR5("No", "No", "No", "Non", "Nein"), s_del_yes ? LINE : ACC, s_del_yes ? FG : BG, NK_CHAR, 'n');
        lg::button(cx + 32 + bw, cy + chh - 46, bw, 34, TR5("Elimina", "Delete", "Borrar", "Supprimer", "Loeschen"), s_del_yes ? C_RED : LINE, 0xFFFF, NK_CHAR, 'y');
    }
}

// One card for every training step: its colour, a big title, two lines and the mic disc. Tapping it is the step's
// own action (type the name / continue); while waiting for GO it is not a target (the Cardputer holds GO).
static void lg_tab_train(int yb, int Wd, int Hd)
{
    const int bx = 16, bw = Wd - 32, by = yb + 8, bh = Hd - yb - 16;
    const bool listening = s_rec_step == RS_WAITING_FN && nucleo_voice_is_listening();
    unsigned short box = LINE, tc = FG, ring = DIM;
    const char *t1, *l1, *l2;
    switch (s_rec_step) {
    case RS_IDLE:
        t1 = TR5("Nuova parola", "New word", "Nueva palabra", "Nouveau mot", "Neues Wort");
        l1 = TR5("Tocca qui e scrivi il nome", "Tap here and type the name", "Toca aqui y escribe el nombre", "Touchez ici, tapez le nom", "Tippe hier, gib den Namen ein");
        l2 = TR5("Poi tieni premuto GO e parla", "Then hold GO and speak", "Luego manten GO y habla", "Puis tenez GO et parlez", "Dann halte GO und sprich"); ring = ACC; break;
    case RS_WAITING_FN:
        if (listening) { box = REC_BG; tc = ACC2; }
        t1 = listening ? TR5("IN ASCOLTO...", "LISTENING...", "ESCUCHANDO...", "ECOUTE...", "HOERT...") : TR5("TIENI GO e parla", "HOLD GO & speak", "MANTEN GO y habla", "TIENS GO et parle", "HALTE GO & sprich");
        l1 = TR5("Parola:", "Word:", "Palabra:", "Mot :", "Wort:"); l2 = s_rec_word;
        if (listening) { int64_t t = esp_timer_get_time() / 200000; ring = (t & 1) ? C_RED : ACC2; }
        break;
    case RS_DONE:
        box = OK_BG; tc = ACC; t1 = TR5("SALVATA!", "SAVED!", "GUARDADA!", "ENREGISTRE !", "GESPEICHERT!"); l1 = s_rec_word;
        l2 = TR5("Tocca per continuare", "Tap to continue", "Toca para seguir", "Touchez pour continuer", "Tippe zum Weiter"); ring = ACC; break;
    default:
        box = ERR_BG; tc = C_RED; t1 = TR5("ERRORE AUDIO", "AUDIO ERROR", "ERROR DE AUDIO", "ERREUR AUDIO", "AUDIOFEHLER");
        l1 = TR5("Troppo corto o silenzio.", "Too short or silent.", "Muy corto o silencio.", "Trop court ou silence.", "Zu kurz oder stumm.");
        l2 = TR5("Tocca e tieni GO piu a lungo.", "Tap and hold GO longer.", "Toca y manten GO mas.", "Touchez, tenez GO plus.", "Tippe, halte GO laenger."); ring = C_RED; break;
    }
    lg::panel(bx, by, bw, bh, box);
    lg_mic(bx + bw - 50, by + 44, 26, ring, FG);
    lg::font(2); d.setTextColor(tc, box);
    char f[48]; lg::fit(f, sizeof f, t1, bw - 110); d.drawString(f, bx + 18, by + 22);
    lg::font(0); d.setTextColor(FG, box); lg::fit(f, sizeof f, l1, bw - 36); d.drawString(f, bx + 18, by + 92);
    lg::font(s_rec_step == RS_WAITING_FN ? 2 : 0); d.setTextColor(s_rec_step == RS_WAITING_FN ? ACC : MUTED, box);
    lg::fit(f, sizeof f, l2, bw - 36); d.drawString(f, bx + 18, by + 116);
    if (s_rec_step != RS_WAITING_FN) nucleo_touch_add(bx, by, bw, bh, NK_ENTER, 0);
}

static void lg_tab_test(int yb, int Wd, int Hd)
{
    const int bx = 24, bw = Wd - 48, by = yb + 10, bh = Hd - yb - 20;
    const int cx = bx + bw / 2;
    lg::panel(bx, by, bw, bh, PANEL);
    // the mode switch (top right of the card): the safety must be visible and one tap away
    const char *ml = mode_label();
    lg::font(0); const int mw = (int)d.textWidth(ml) + 22;
    d.fillRoundRect(bx + bw - mw - 10, by + 8, mw, 22, 8, s_live ? ACC2 : LINE);
    d.setTextDatum(middle_center); d.setTextColor(s_live ? BG : MUTED, s_live ? ACC2 : LINE);
    d.drawString(ml, bx + bw - mw - 10 + mw / 2, by + 19); d.setTextDatum(top_left);

    if (!s_have_m) {
        lg::font(2); d.setTextColor(ACC, PANEL); d.setTextDatum(top_center);
        d.drawString(TR5("Tieni GO e parla", "Hold GO & speak", "Manten GO y habla", "Tenez GO et parlez", "Halte GO & sprich"), cx, by + 40);
        char ln[64];
        lg::font(0); d.setTextColor(FG, PANEL);
        lg::fit(ln, sizeof ln, s_live ? TR5("Modo ESEGUI: una parola collegata apre la sua app.", "RUN mode: a bound word opens its app.", "Modo EJECUTA: la palabra abre su app.", "Mode EXECUTE : le mot ouvre son app.", "AUSFUEHREN: Wort oeffnet seine App.")
                                      : TR5("Modo prova: riconosce ma NON agisce.", "Test mode: recognizes but does NOT act.", "Modo prueba: reconoce pero NO actua.", "Mode test : reconnait sans agir.", "Testmodus: erkennt, handelt NICHT."), bw - 24);
        d.drawString(ln, cx, by + 76);
        d.setTextColor(MUTED, PANEL);
        lg::fit(ln, sizeof ln, s_tpl_count > 0 ? TR5("Pronuncia una parola addestrata.", "Say a trained word.", "Di una palabra entrenada.", "Dites un mot appris.", "Sag ein gelerntes Wort.")
                                               : TR5("Nessuna parola: vai su Addestra.", "No word: go to Train.", "Sin palabra: ve a Entrenar.", "Aucun mot : Entrainer.", "Kein Wort: gehe zu Lernen."), bw - 24);
        d.drawString(ln, cx, by + 98);
        d.setTextDatum(top_left);
        const int my = by + bh - 30;                    // the mic: pulses while GO is held on the Cardputer
        if (nucleo_voice_is_listening()) {
            int64_t t = esp_timer_get_time() / 200000; int r = 14 + (int)(t % 5);
            d.fillCircle(cx, my, r, (t & 1) ? C_DEB : C_DIS); d.fillCircle(cx, my, 8, FG);
        } else { d.drawCircle(cx, my, 16, MUTED); d.fillCircle(cx, my, 7, DIM); }
    } else {
        conf_t c = classify(s_m.dist, s_m.second, s_m.radius);
        lg::font(3); d.setTextColor(FG, PANEL);
        char w[24]; lg::fit(w, sizeof w, s_m.word, bw - 130 - mw); d.drawString(w, bx + 16, by + 12);
        const char *vl = tier_name(c.tier);
        lg::font(1); const int vw = (int)d.textWidth(vl) + 18;
        d.fillRoundRect(bx + 16, by + 50, vw, 24, 8, c.col);
        d.setTextDatum(middle_center); d.setTextColor(BG, c.col); d.drawString(vl, bx + 16 + vw / 2, by + 62); d.setTextDatum(top_left);
        lg::bar(bx + 16 + vw + 12, by + 56, bw - 32 - vw - 12, 12, c.pct, c.col);
        int ry = by + 86;
        if (s_have_r) {
            lg::font(0); d.setTextColor(ACC, PANEL); d.drawString(result_verb(), bx + 16, ry);
            lg::font(1); d.setTextColor(FG, PANEL);
            char rb[48]; lg::fit(rb, sizeof rb, result_text(), bw - 32); d.drawString(rb, bx + 16, ry + 16);
        }
        lg::font(0); d.setTextColor(MUTED, PANEL);
        char tm[48]; snprintf(tm, sizeof tm, "d %ld  2o %ld  r %ld   n.%d", (long)s_m.dist, (long)s_m.second, (long)s_m.radius, s_recog);
        d.drawString(tm, bx + 16, by + bh - 44);
        d.setTextColor(DIM, PANEL); d.drawString(TR5("Tocca per pulire", "Tap to clear", "Toca para limpiar", "Touchez pour effacer", "Tippen zum Leeren"), bx + 16, by + bh - 24);
        nucleo_touch_add(bx, by + 36, bw, bh - 36, NK_CHAR, 'c');    // the result card: tap = clear the bench
    }
    nucleo_touch_add(bx + bw - mw - 16, by + 2, mw + 12, 34, NK_CHAR, 'e');   // first registered target wins: the mode switch
}

static void lg_tab_info(int yb, int Wd, int Hd)
{
    const int px = 20, py = yb + 10, pw = Wd - 40, ph = 4 * 24 + 16;
    d.fillRect(0, yb, Wd, Hd - yb, BG);
    lg::panel(px, py, pw, ph);
    const unsigned short cb = lg::card();
    const int ws = nucleo_ws_client_count();
    const bool aon = nucleo_voice_always_on();
    char tpl[24];
    if (s_tpl_overflow) snprintf(tpl, sizeof tpl, "%d+ (max %d)", s_tpl_count, MAX_TPLS);
    else                snprintf(tpl, sizeof tpl, "%d / %d", s_tpl_count, MAX_TPLS);
    char wss[8]; snprintf(wss, sizeof wss, "%d", ws);
    int ky = py + 12;
    lg::kv(px + 16, ky, pw - 32, TR5("Parole", "Words", "Palabras", "Mots", "Woerter"), tpl, s_tpl_overflow ? ACC2 : ACC, cb); ky += 24;
    lg::kv(px + 16, ky, pw - 32, "WS client", wss, ws > 0 ? ACC : MUTED, cb); ky += 24;
    lg::kv(px + 16, ky, pw - 32, "Routing", ws > 0 ? "Web" : TR5("Locale", "Local", "Local", "Local", "Lokal"), ws > 0 ? ACC2 : ACC, cb); ky += 24;
    lg::kv(px + 16, ky, pw - 32, TR5("Lingua", "Language", "Idioma", "Langue", "Sprache"), "IT EN ES FR DE", MUTED, cb);
    // the always-on switch: the row itself toggles it (PTT from the home screen, saved in settings)
    const int ry = py + ph + 8;
    const lg::Setting st = { TR5("Sempre attiva", "Always on", "Siempre activa", "Toujours active", "Immer an"), nullptr, nullptr, lg::ROW_TOGGLE, -1, -1, aon, false, 0, 0, 0 };
    lg::setting(px, ry, pw, 46, st, false, ACC);
    nucleo_touch_add(px, ry, pw, 42, NK_CHAR, 'a');
}

static void draw_large(void)
{
    const int Wd = d.width(), Hd = d.height();
    nucleo_touch_clear();
    if (s_pick) {   // the picker takes the whole panel: a title and the app list
        char hd[72]; snprintf(hd, sizeof hd, TR5("\"%s\" apre...", "\"%s\" opens...", "\"%s\" abre...", "\"%s\" ouvre...", "\"%s\" oeffnet..."), s_t ? s_t[s_sel].name : "");
        const int y = lg::header(hd, nullptr, ACC, NK_BACK, 0);
        lg_pick(y, Wd, Hd);
        nucleo_touch_commit();
        return;
    }
    char rt[16] = ""; if (s_tab == TAB_CMD) snprintf(rt, sizeof rt, "%d", s_tpl_count);
    const int y = lg::header("Voice", rt[0] ? rt : nullptr, ACC, NK_BACK, 0);
    const char *tabs[TAB_N] = { tab_label(0), tab_label(1), tab_label(2), tab_label(3) };
    const int yb = lg::tabs(y, tabs, TAB_N, s_tab, ACC, "\x01\x02\x03\x04");   // tap a tab = that tab
    if (s_tab == TAB_CMD)        lg_tab_cmds(yb, Wd, Hd);
    else if (s_tab == TAB_TRAIN) lg_tab_train(yb, Wd, Hd);
    else if (s_tab == TAB_TEST)  lg_tab_test(yb, Wd, Hd);
    else                         lg_tab_info(yb, Wd, Hd);
    nucleo_touch_commit();
}

// The Cardputer's own screen while the panel carries the app: a glanceable summary + the keys that matter here.
static void draw_companion(void)
{
    const int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, W, h, BG);
    const int y = lg::comp_head("VOICE", ACC);
    d.setFont(&fonts::Font0); d.setTextColor(ACC, BG); d.drawString(tab_label(s_tab), 8, y + 2);
    if (s_tab == TAB_CMD) {
        d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(FG, BG);
        d.drawString(s_tpl_count > 0 ? s_t[s_sel].name : TR5("Nessuna parola", "No words", "Sin palabras", "Aucun mot", "Keine Woerter"), 8, y + 18);
        d.setFont(&fonts::Font0); d.setTextColor(s_tpl_count > 0 && s_t[s_sel].app[0] ? ACC : DIM, BG);
        d.drawString(s_tpl_count > 0 && s_t[s_sel].app[0] ? app_label(s_t[s_sel].app) : "-", 8, y + 42);
        lg::comp_keys(TR5("su/giu  Invio app  R addestra  Del", "up/dn  Enter app  R train  Del", "arr/abj  Intro app  R  Supr", "haut/bas  Entree  R  Suppr", "auf/ab  Enter App  R  Entf"));
    } else if (s_tab == TAB_TRAIN) {
        const bool listening = nucleo_voice_is_listening();
        d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(listening ? ACC2 : ACC, BG);
        d.drawString(s_rec_step == RS_WAITING_FN ? (listening ? TR5("Parla ora!", "Speak now!", "Habla ya!", "Parlez !", "Sprich jetzt!") : TR5("Tieni GO e parla", "Hold GO & speak", "Manten GO y habla", "Tenez GO et parlez", "Halte GO & sprich"))
                     : s_rec_step == RS_DONE ? TR5("Salvata", "Saved", "Guardada", "Enregistre", "Gespeichert")
                     : s_rec_step == RS_TOO_SHORT ? TR5("Riprova", "Try again", "Reintenta", "Reessayez", "Nochmal")
                     : TR5("Scrivi il nome", "Type the name", "Escribe el nombre", "Tapez le nom", "Namen eingeben"), 8, y + 18);
        if (s_rec_word[0]) { d.setFont(&fonts::Font0); d.setTextColor(FG, BG); d.drawString(s_rec_word, 8, y + 44); }
        lg::comp_keys(s_rec_step == RS_IDLE ? TR5("INVIO scrivi il nome  TAB schede", "ENTER type the name  TAB tabs", "INTRO escribe el nombre  TAB", "ENTREE tapez le nom  TAB", "ENTER Namen eingeben  TAB")
                                            : TR5("TIENI GO per registrare", "HOLD GO to record", "MANTEN GO para grabar", "TENEZ GO pour enregistrer", "GO halten zum Aufnehmen"));
    } else if (s_tab == TAB_TEST) {
        if (s_have_m) {
            conf_t c = classify(s_m.dist, s_m.second, s_m.radius);
            d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(FG, BG); d.drawString(s_m.word, 8, y + 18);
            d.setFont(&fonts::Font0); d.setTextColor(c.col, BG); d.drawString(tier_name(c.tier), 8, y + 44);
        } else {
            d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(nucleo_voice_is_listening() ? C_DIS : ACC, BG);
            d.drawString(nucleo_voice_is_listening() ? TR5("Parla ora!", "Speak now!", "Habla ya!", "Parlez !", "Sprich jetzt!") : TR5("Tieni GO e parla", "Hold GO & speak", "Manten GO y habla", "Tenez GO et parlez", "Halte GO & sprich"), 8, y + 20);
        }
        d.setFont(&fonts::Font0); d.setTextColor(s_live ? ACC2 : MUTED, BG); d.drawString(mode_label(), W - 8 - (int)strlen(mode_label()) * 6, y + 2);
        lg::comp_keys(TR5("TIENI GO  E modo  C pulisci", "HOLD GO  E mode  C clear", "MANTEN GO  E modo  C limpia", "TENEZ GO  E mode  C efface", "GO halten  E Modus  C leeren"));
    } else {
        char l[48]; snprintf(l, sizeof l, "%s %d/%d", TR5("Parole", "Words", "Palabras", "Mots", "Woerter"), s_tpl_count, MAX_TPLS);
        d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(FG, BG); d.drawString(l, 8, y + 18);
        d.setFont(&fonts::Font0); d.setTextColor(nucleo_voice_always_on() ? ACC2 : MUTED, BG);
        d.drawString(nucleo_voice_always_on() ? TR5("Sempre attiva: Si", "Always on: Yes", "Siempre activa: Si", "Toujours active : Oui", "Immer an: Ja") : TR5("Sempre attiva: No", "Always on: No", "Siempre activa: No", "Toujours active : Non", "Immer an: Nein"), 8, y + 44);
        lg::comp_keys(TR5("A sempre attiva  TAB schede", "A always on  TAB tabs", "A siempre activa  TAB", "A toujours active  TAB", "A immer an  TAB"));
    }
}

static void draw(void)
{
    if (nucleo_app_two_screens()) { draw_companion(); return; }
    int top = nucleo_app_content_top();
    d.fillRect(0, top, W, nucleo_app_content_height(), BG);
    switch (s_tab) {
    case TAB_CMD:   draw_cmds(top);  break;
    case TAB_TRAIN: draw_train(top); break;
    case TAB_TEST:  draw_test(top);  break;
    default:        draw_info(top);  break;
    }
}

// ── Registration ──────────────────────────────────────────────────────────────
extern "C" void nucleo_register_voice(void)
{
    static const nucleo_app_def_t app = {
        "voice", "Voice", "System",
        "Train, bind and test spoken commands",
        'V', ACC, enter, on_key, tick, draw, exit_app, NX_SOLO | NX_WIFI, nullptr, draw_large
    };
    nucleo_app_register(&app);
}
