// AVCEB Voice Manager — native Cardputer app.
//
// Tab 0 — TRIGGERS : elenco dei template salvati su /system/voice/*.tpl
//          Del = elimina, R = avvia registrazione guidata del template selezionato
// Tab 1 — RECORD   : workflow "tieni FN + parla" per registrare una nuova parola
//          L'utente scrive il nome della parola, premi Invio, poi usi FN per incidere.
// Tab 2 — STATUS   : numero di template, lingua Anima, WS clients, engine on/off
//
// Tutto offline. Nessuna rete richiesta.
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
#include "nucleo_storage.h"
#include "nucleo_ui.h"
#include "nucleo_board.h"
#include "esp_log.h"
}

#include "app_gfx.h"

// Palette — verde elettrico / dark come il motore
static const unsigned short
    BG   = 0x0841,
    FG   = 0xFFFF,
    ACC  = 0x07E0,   // verde lime
    ACC2 = 0xFD20,   // arancio per stati attivi
    MUTED= 0x8C71,
    DIM  = 0x4208,
    LINE = 0x2945,
    C_RED = 0xF800;

#define TPL_PATH NUCLEO_SD_MOUNT "/system/voice"
#define MAX_TPLS 20
#define W 240
#define H 135

// WS client count (resolved at link from nucleo_ws, no REQUIRES cycle)
extern "C" int nucleo_ws_client_count(void);

// ── State ────────────────────────────────────────────────────────────────────
static int  s_tab  = 0;     // 0=triggers 1=record 2=status
static int  s_sel  = 0;
static char s_tpl_names[MAX_TPLS][32];
static int  s_tpl_count = 0;
static bool s_tpl_overflow = false;   // more than MAX_TPLS templates on SD (only first MAX shown/matched)

// Record-tab state
enum RecStep { RS_IDLE, RS_WAITING_FN, RS_DONE, RS_TOO_SHORT };
static RecStep s_rec_step  = RS_IDLE;
static char    s_rec_word[32] = "";
static int     s_rec_timer = 0;  // ticks countdown for feedback display

static void scan_tpls(void)
{
    s_tpl_count = 0;
    s_tpl_overflow = false;
    mkdir(TPL_PATH, 0775);
    DIR *dir = opendir(TPL_PATH);
    if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir))) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".tpl")) continue;
        if (s_tpl_count >= MAX_TPLS) { s_tpl_overflow = true; continue; }   // count past the cap to warn
        // Strip extension for display
        int len = (int)(dot - e->d_name);
        if (len >= (int)sizeof(s_tpl_names[0])) len = (int)sizeof(s_tpl_names[0]) - 1;
        memcpy(s_tpl_names[s_tpl_count], e->d_name, len);
        s_tpl_names[s_tpl_count][len] = '\0';
        s_tpl_count++;
    }
    closedir(dir);
    if (s_sel >= s_tpl_count) s_sel = s_tpl_count > 0 ? s_tpl_count - 1 : 0;
}

// ── App lifecycle ─────────────────────────────────────────────────────────────
static void on_tab(void)
{
    s_tab = (s_tab + 1) % 3;
    if (s_tab == 0) scan_tpls();
    if (s_tab == 1) { s_rec_step = RS_IDLE; s_rec_word[0] = '\0'; }
    nucleo_app_request_draw();
}

static void enter(void)
{
    nucleo_app_set_tab_handler(on_tab);
    nucleo_app_set_hint("TAB schede  R aggiungi  Del elimina");
    s_tab = 0; s_sel = 0;
    nucleo_voice_set_test_mode(true);   // recognize but DON'T act: a stray GO here must not fire a real command
    nucleo_voice_request(true);         // hold the lazy engine up while training/testing here
    scan_tpls();
}

static void exit_app(void)
{
    s_rec_step = RS_IDLE;
    nucleo_voice_set_test_mode(false);   // restore normal dispatch for the rest of the OS
    // Release the app hold. The engine frees its 16 KB UNLESS the user explicitly
    // pinned "always listen" (STATUS tab) — that opt-in survives leaving the app.
    nucleo_voice_request(false);
}

static void tick(void)
{
    // Countdown for feedback message on RECORD tab
    if (s_rec_timer > 0) { s_rec_timer--; if (s_rec_timer == 0) nucleo_app_request_draw(); }

    // While waiting for a recording, POLL the engine for the outcome — the live event sink
    // (voice/learned) is owned by the WebSocket layer, so a native app cannot subscribe; it
    // must poll. Without this the screen used to freeze on "IN ASCOLTO" forever.
    if (s_rec_step == RS_WAITING_FN) {
        nucleo_voice_learn_t lr = nucleo_voice_take_learn_result();
        if (lr == NUCLEO_VOICE_LEARN_OK)             { s_rec_step = RS_DONE;      scan_tpls(); }
        else if (lr == NUCLEO_VOICE_LEARN_TOO_SHORT) { s_rec_step = RS_TOO_SHORT; }
        nucleo_app_request_draw();   // also keeps the pulsing-mic animation alive (~5x/s)
    }
}

// ── Input ─────────────────────────────────────────────────────────────────────
static void on_key(int key, char ch)
{
    // ─ TAB 0: TRIGGERS ──────────────────────────────────────────────────────
    if (s_tab == 0) {
        if (app_ui_list_key(key, ch, &s_sel, s_tpl_count, nullptr, nullptr)) {
            // handled
        } else if (key == NK_DEL || (ch == 'd' || ch == 'D')) {
            if (s_tpl_count > 0 && s_sel < s_tpl_count) {
                char path[128];
                snprintf(path, sizeof(path), "%s/%s.tpl", TPL_PATH, s_tpl_names[s_sel]);
                unlink(path);
                scan_tpls();
            }
        } else if (ch == 'r' || ch == 'R') {
            // Jump to the RECORD tab to (re)train. Pre-fill the selected template's name so
            // re-training a command is one keypress, not retyping it.
            s_tab = 1;
            s_rec_step = RS_IDLE;
            if (s_tpl_count > 0 && s_sel < s_tpl_count)
                snprintf(s_rec_word, sizeof(s_rec_word), "%s", s_tpl_names[s_sel]);
            else
                s_rec_word[0] = '\0';
        } else {
            return;
        }
        nucleo_app_request_draw();
        return;
    }

    // ─ TAB 1: RECORD ────────────────────────────────────────────────────────
    if (s_tab == 1) {
        if (s_rec_step == RS_IDLE) {
            // Prompt for the word name, pre-filled if we arrived via 'R' on a selected template.
            char word[32] = "";
            snprintf(word, sizeof(word), "%s", s_rec_word);
            nucleo_ui_input("Nome parola (es: registratore)", word, sizeof(word), 0);
            if (!word[0]) { nucleo_app_request_draw(); return; }
            snprintf(s_rec_word, sizeof(s_rec_word), "%s", word);
            nucleo_voice_arm_learning_mode(s_rec_word);
            s_rec_step = RS_WAITING_FN;
        } else if (s_rec_step == RS_DONE || s_rec_step == RS_TOO_SHORT) {
            // Any key resets
            s_rec_step = RS_IDLE;
            s_rec_word[0] = '\0';
            scan_tpls();
        } else {
            return;
        }
        nucleo_app_request_draw();
        return;
    }

    // ─ TAB 2: STATUS ────────────────────────────────────────────────────────
    if (s_tab == 2) {
        if (ch == 'a' || ch == 'A') {
            // Persistent "sempre attiva": pins the engine up (PTT works from the home
            // screen) and writes voice.alwaysOn to settings.json. Survives reboot.
            nucleo_voice_set_always_on(!nucleo_voice_always_on());
        } else {
            return;
        }
        nucleo_app_request_draw();
    }
}

// ── Draw helpers ──────────────────────────────────────────────────────────────
static void draw_tabs(int top_y)
{
    static const char *LABELS[] = { "TEMPLATES", "ADDESTRA", "INFO" };
    int tw = 240 / 3;
    for (int i = 0; i < 3; i++) {
        bool active = (i == s_tab);
        unsigned short bg = active ? ACC : BG;
        unsigned short fg = active ? BG  : DIM;
        
        d.fillRoundRect(i * tw + 4, top_y + 2, tw - 8, 18, 6, bg);
        d.setTextColor(fg, bg);
        d.setTextSize(1);
        int lx = i * tw + 4 + (tw - 8 - strlen(LABELS[i])*6)/2;
        d.setCursor(lx, top_y + 7);
        d.print(LABELS[i]);
    }
    d.drawFastHLine(0, top_y + 22, W, LINE);
}

static const char *tl_label(int i, void *) { return s_tpl_names[i]; }

static void draw_triggers(int top_y)
{
    draw_tabs(top_y);
    int y0 = top_y + 26;
    int h  = nucleo_app_content_height() - 26 - 16;

    if (s_tpl_count == 0) {
        d.setTextSize(1); d.setTextColor(DIM, BG);
        d.setCursor(12, y0 + 14); d.print("Nessun template salvato.");
        d.fillRoundRect(12, y0 + 34, 240-24, 22, 6, ACC);
        d.setTextColor(BG, ACC);
        d.setCursor(W/2 - 60, y0 + 41); d.print("Premi R per Addestrare");
        return;
    }
    app_ui_list(y0, h, s_tpl_count, s_sel, tl_label, nullptr, nullptr, nullptr);

    // Footer with selected trigger info
    int fy = nucleo_app_content_top() + nucleo_app_content_height() - 16;
    d.fillRect(0, fy, 240, 16, BG); d.drawFastHLine(0, fy, 240, LINE);
    d.setTextSize(1); d.setTextColor(MUTED, BG);
    if (s_tpl_count > 0 && s_sel < s_tpl_count) {
        d.setCursor(8, fy + 4);
        d.print("Premi ");
        d.setTextColor(C_RED, BG); d.print("Del");
        d.setTextColor(MUTED, BG); d.print(" per eliminare");
    }
}

static void draw_record(int top_y)
{
    draw_tabs(top_y);
    int y = top_y + 28;

    switch (s_rec_step) {
    case RS_IDLE:
        d.fillRoundRect(8, y, 240 - 16, 80, 8, 0x10A2); // Box scuro
        d.setTextColor(FG, 0x10A2); d.setCursor(16, y + 8); d.setTextSize(2);
        d.print("Nuova Parola");
        d.setTextSize(1); d.setTextColor(ACC, 0x10A2); d.setCursor(16, y + 30);
        d.print("1. Premi Invio e scrivi nome");
        d.setTextColor(DIM, 0x10A2); d.setCursor(16, y + 46);
        d.print("2. Tieni premuto GO e parla");
        d.setTextColor(DIM, 0x10A2); d.setCursor(16, y + 62);
        d.print("100% Offline (nessun WAV)");
        break;

    case RS_WAITING_FN: {
        d.fillRoundRect(8, y, W - 16, 80, 8, 0x2000); // Box rosso tenue
        d.setTextColor(ACC2, 0x2000); d.setTextSize(2); d.setCursor(16, y + 8);
        d.print("IN ASCOLTO...");
        
        d.setTextSize(1); d.setTextColor(FG, 0x2000); d.setCursor(16, y + 30);
        d.print("Parola selezionata:");
        
        d.setTextColor(ACC, 0x2000); d.setTextSize(2); d.setCursor(16, y + 44);
        d.print(s_rec_word);
        
        // Animated mic icon
        int64_t t = esp_timer_get_time() / 200000;
        int radius = 10 + (t % 4);
        unsigned short pulse = (t & 1) ? C_RED : ACC2;
        d.fillCircle(W - 30, y + 40, radius, pulse);
        d.fillCircle(W - 30, y + 40, 6, FG);
        d.setTextColor(BG); d.setTextSize(1);
        break;
    }

    case RS_DONE:
        d.fillRoundRect(8, y, W - 16, 80, 8, 0x03E0); // Box verde tenue
        d.setTextSize(2); d.setTextColor(ACC, 0x03E0); d.setCursor(16, y + 16);
        d.print("SALVATO!");
        d.setTextSize(1); d.setTextColor(FG, 0x03E0); d.setCursor(16, y + 44);
        char msg2[48]; snprintf(msg2, sizeof(msg2), "'%s.tpl' creato.", s_rec_word);
        d.print(msg2);
        d.setTextColor(MUTED, 0x03E0); d.setCursor(16, y + 60);
        d.print("Premi un tasto.");
        break;

    case RS_TOO_SHORT:
        d.fillRoundRect(8, y, W - 16, 80, 8, 0x5800); // Box rosso/arancio
        d.setTextSize(2); d.setTextColor(C_RED, 0x5800); d.setCursor(16, y + 12);
        d.print("ERRORE AUDIO");
        d.setTextSize(1); d.setTextColor(FG, 0x5800); d.setCursor(16, y + 36);
        d.print("Troppo corto o silenzio.");
        d.setTextColor(MUTED, 0x5800); d.setCursor(16, y + 54);
        d.print("Tieni GO piu a lungo.");
        break;
    }
}

static void draw_status(int top_y)
{
    draw_tabs(top_y);
    int y = top_y + 26;
    int ws = nucleo_ws_client_count();

    bool aon = nucleo_voice_always_on();
    struct { const char *k; const char *v; unsigned short col; } rows[] = {
        { "TEMPLATE",  nullptr,          ACC   },
        { "WS CLIENT", nullptr,          ws > 0 ? ACC : MUTED },
        { "ROUTING",   ws > 0 ? "Web" : "Locale", ws > 0 ? ACC2 : ACC },
        { "LINGUA",    "IT/EN (Anima)", MUTED  },
        { "SEMPRE ON", aon ? "Si" : "No", aon ? ACC2 : MUTED },
    };
    const int NROWS = (int)(sizeof(rows) / sizeof(rows[0]));
    // Fill dynamic fields
    char tpl_str[24], ws_str[8];
    if (s_tpl_overflow) snprintf(tpl_str, sizeof(tpl_str), "%d+ (max %d!)", s_tpl_count, MAX_TPLS);
    else                snprintf(tpl_str, sizeof(tpl_str), "%d / %d", s_tpl_count, MAX_TPLS);
    snprintf(ws_str,  sizeof(ws_str),  "%d", ws);
    rows[0].v = tpl_str;
    if (s_tpl_overflow) rows[0].col = ACC2;   // warn: only the first MAX_TPLS are matched
    rows[1].v = ws_str;

    d.fillRoundRect(8, y, 240 - 16, 12 + NROWS * 16, 8, 0x10A2);

    for (int i = 0; i < NROWS; i++) {
        int ry = y + 6 + i * 16;
        d.setTextSize(1); d.setTextColor(MUTED, 0x10A2); d.setCursor(16,  ry); d.print(rows[i].k);
        d.setTextColor(rows[i].col, 0x10A2);             d.setCursor(104, ry); d.print(rows[i].v);
    }

    // Hint: how to toggle the persistent always-on opt-in.
    d.setTextSize(1); d.setTextColor(MUTED, BG);
    d.setCursor(16, y + 12 + NROWS * 16 + 6);
    d.print("A = Sempre attiva (PTT da home)");
}

static void draw(void)
{
    int top = nucleo_app_content_top();
    d.fillRect(0, top, 240, nucleo_app_content_height(), BG);

    switch (s_tab) {
    case 0: draw_triggers(top); break;
    case 1: draw_record(top);   break;
    case 2: draw_status(top);   break;
    }
}

// ── Registration ──────────────────────────────────────────────────────────────
extern "C" void nucleo_register_voice(void)
{
    static const nucleo_app_def_t app = {
        "voice", "Voice Trainer", "Voice",
        "Addestratore vocale locale",
        'V', ACC, enter, on_key, tick, draw, exit_app
    };
    nucleo_app_register(&app);
}
