// "Voce" — native voice-command console for the on-device recognizer (AVCEB).
//
// The companion to the Voice Trainer (app_voice.cpp): the trainer CREATES templates,
// this app PROVES them. Two tabs:
//
//   Tab 0 — PROVA   : a live test bench. Hold GO and speak; on release you see WHAT was
//                     recognized, HOW confident the recognizer was (a bar + verdict derived
//                     from the same DTW distance/margin/radius the web "Prova" tab uses),
//                     and WHAT ANIMA resolved it to. Runs in TEST MODE: it recognizes but
//                     does NOT execute, so testing "apri musica" ten times won't keep opening
//                     Music or yank you out of the console.
//   Tab 1 — DIAGNOSI: an intelligent self-check. Lists every trained command with the best
//                     reliability seen this session — so the recognizer tells YOU which words
//                     are solid and which to re-train, instead of you guessing.
//
// 100% offline. The engine can't push events to a native app (the live sink belongs to the
// WebSocket layer), so this app POLLS nucleo_voice's introspection API from on_tick.
#include "nucleo_app.h"
#include "app_ui.h"
#include "nucleo_exclusive.h"   // free RAM on enter so the recognizer's heap gate can pass
#include "nucleo_i18n.h"        // TR(it,en): UI labels follow the system language
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

extern "C" {
#include "nucleo_voice.h"
#include "nucleo_anima.h"   // anima_action_t (LAUNCH/SYSTEM/ANSWER) for the result verb
#include "nucleo_board.h"
#include "esp_log.h"
}

#include "app_gfx.h"

// ── Palette ───────────────────────────────────────────────────────────────────
static const unsigned short
    BG    = 0x0841,
    FG    = 0xFFFF,
    ACC   = 0x4DD9,   // cyan — distinct from the green Voice Trainer
    PANEL = 0x10A2,
    MUTED = 0x8C71,
    DIM   = 0x4208,
    LINE  = 0x2945,
    C_OTT = 0x07E0,   // Ottimo  — green
    C_BUO = 0x9FE0,   // Buono   — yellow-green
    C_DIS = 0xFD20,   // Discreto— orange
    C_DEB = 0xF800;   // Debole  — red

#define TPL_PATH NUCLEO_SD_MOUNT "/system/voice"
#define MAX_TPLS 20
#define W 240

// Confidence tiers (mirror apps/voice-manager/www/app.js classify()).
static const unsigned short TIER_COL[5] = { MUTED, C_DEB, C_DIS, C_BUO, C_OTT };
static const int           TIER_PCT[5]  = { 0, 25, 48, 72, 92 };
static const char *tier_name(int t)
{
    switch (t) {
        case 0:  return TR("non provato", "untested");
        case 1:  return TR("Debole",   "Weak");
        case 2:  return TR("Discreto", "Fair");
        case 3:  return TR("Buono",    "Good");
        default: return TR("Ottimo",   "Great");
    }
}

typedef struct { int tier; int pct; unsigned short col; } conf_t;

// Same logic as the web telemetry panel, in integer percent math (no floats needed).
static conf_t classify(int32_t dist, int32_t second, int32_t radius)
{
    int margin = (second > 0) ? (int)((int64_t)dist * 100 / second) : 100;  // lower = clearer winner
    int ratio  = (radius > 0) ? (int)((int64_t)dist * 100 / radius) : 100;  // <100 = inside radius
    int t;
    if (ratio <= 60 || margin <= 45)      t = 4;   // Ottimo
    else if (ratio <= 100 || margin <= 65) t = 3;  // Buono
    else if (margin <= 75)                 t = 2;  // Discreto
    else                                   t = 1;  // Debole
    conf_t c = { t, TIER_PCT[t], TIER_COL[t] };
    return c;
}

// ── State ─────────────────────────────────────────────────────────────────────
static int  s_tab = 0;        // 0=PROVA 1=DIAGNOSI
static int  s_sel = 0;        // DIAGNOSI list cursor

static char s_tpl_names[MAX_TPLS][32];
static int  s_diag_tier[MAX_TPLS];   // best tier seen this session, per command (0 = untested)
static int  s_tpl_count = 0;

static uint32_t s_seen_match = 0, s_seen_result = 0;
static int      s_recog = 0;         // recognitions this session
static bool     s_have_m = false, s_have_r = false;
static nucleo_voice_match_t  s_m;
static nucleo_voice_result_t s_r;

static void scan_tpls(void)
{
    s_tpl_count = 0;
    mkdir(TPL_PATH, 0775);
    DIR *dir = opendir(TPL_PATH);
    if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir)) && s_tpl_count < MAX_TPLS) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".tpl")) continue;
        int len = (int)(dot - e->d_name);
        if (len >= (int)sizeof(s_tpl_names[0])) len = (int)sizeof(s_tpl_names[0]) - 1;
        memcpy(s_tpl_names[s_tpl_count], e->d_name, len);
        s_tpl_names[s_tpl_count][len] = '\0';
        s_diag_tier[s_tpl_count] = 0;
        s_tpl_count++;
    }
    closedir(dir);
    if (s_sel >= s_tpl_count) s_sel = s_tpl_count > 0 ? s_tpl_count - 1 : 0;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────
static void on_tab(void)
{
    s_tab ^= 1;
    if (s_tab == 1) scan_tpls();
    nucleo_app_request_draw();
}

static void enter(void)
{
    // Free ~64 KB (httpd + L1 + mDNS) yet keep the voice engine: the recognizer needs ~38 KB on PTT,
    // which the loaded heap can't spare — without this the PTT heap gate silently refuses every press.
    nucleo_exclusive_enter(NX_HTTPD | NX_ANIMA_L1 | NX_DISCOVERY, nullptr);
    nucleo_app_set_tab_handler(on_tab);
    nucleo_app_set_hint(TR("TAB schede  GO+parla per provare", "TAB tabs  GO+speak to test"));
    s_tab = 0; s_sel = 0; s_recog = 0; s_have_m = s_have_r = false;
    nucleo_voice_request(true);       // keep the lazy engine up while we test
    nucleo_voice_set_test_mode(true); // recognize but DON'T act (no launches/TTS)
    // Don't replay a stale match/result left over from a previous session.
    nucleo_voice_match_t m;  s_seen_match  = nucleo_voice_last_match(&m)  ? m.seq : 0;
    nucleo_voice_result_t r; s_seen_result = nucleo_voice_last_result(&r) ? r.seq : 0;
    scan_tpls();
}

static void exit_app(void)
{
    nucleo_voice_set_test_mode(false);
    nucleo_voice_request(false);
    if (nucleo_exclusive_active()) nucleo_exclusive_exit();   // bring httpd / L1 / mDNS back (guarded, like app_anima)
}

static void tick(void)
{
    bool changed = false;

    nucleo_voice_match_t m;
    if (nucleo_voice_last_match(&m) && m.seq != s_seen_match) {
        s_seen_match = m.seq; s_m = m; s_have_m = true; s_recog++;
        conf_t c = classify(m.dist, m.second, m.radius);
        for (int i = 0; i < s_tpl_count; i++)
            if (!strcmp(s_tpl_names[i], m.word)) { if (c.tier > s_diag_tier[i]) s_diag_tier[i] = c.tier; break; }
        changed = true;
    }
    nucleo_voice_result_t r;
    if (nucleo_voice_last_result(&r) && r.seq != s_seen_result) {
        s_seen_result = r.seq; s_r = r; s_have_r = true; changed = true;
    }
    if (changed) nucleo_app_request_draw();
}

// ── Input ─────────────────────────────────────────────────────────────────────
static void on_key(int key, char ch)
{
    if (s_tab == 1) {
        if (app_ui_list_key(key, ch, &s_sel, s_tpl_count, nullptr, nullptr)) { nucleo_app_request_draw(); return; }
        if (ch == 'r' || ch == 'R') { scan_tpls(); nucleo_app_request_draw(); }  // refresh
        return;
    }
    // PROVA: clear the last result with Del/Backspace so the bench is clean for the next test.
    if (key == NK_DEL || ch == 'c' || ch == 'C') {
        s_have_m = s_have_r = false; s_recog = 0; nucleo_app_request_draw();
    }
}

// ── Draw ──────────────────────────────────────────────────────────────────────
static void draw_tabs(int top_y)
{
    const char *LBL[2] = { TR("PROVA", "TEST"), TR("DIAGNOSI", "DIAGNOSE") };
    int tw = 240 / 2;
    for (int i = 0; i < 2; i++) {
        bool active = (i == s_tab);
        unsigned short bg = active ? ACC : BG, fg = active ? BG : DIM;
        d.fillRoundRect(i * tw + 4, top_y + 2, tw - 8, 18, 6, bg);
        d.setTextColor(fg, bg); d.setTextSize(1);
        int lx = i * tw + 4 + (tw - 8 - (int)strlen(LBL[i]) * 6) / 2;
        d.setCursor(lx, top_y + 7); d.print(LBL[i]);
    }
    d.drawFastHLine(0, top_y + 22, W, LINE);
}

static void conf_bar(int x, int y, int w, int pct, unsigned short col)
{
    d.drawRoundRect(x, y, w, 8, 3, LINE);
    int fw = w * pct / 100; if (fw < 0) fw = 0; if (fw > w) fw = w;
    if (fw > 0) d.fillRoundRect(x, y, fw, 8, 3, col);
}

static void draw_prova(int top_y)
{
    draw_tabs(top_y);
    int y = top_y + 28;

    if (!s_have_m) {
        d.fillRoundRect(8, y, W - 16, 74, 8, PANEL);
        d.setTextSize(2); d.setTextColor(ACC, PANEL); d.setCursor(16, y + 10); d.print(TR("Tieni GO", "Hold GO"));
        d.setTextColor(FG, PANEL); d.setCursor(16, y + 30); d.print(TR("e parla", "and speak"));
        d.setTextSize(1); d.setTextColor(MUTED, PANEL); d.setCursor(16, y + 54);
        d.print(TR("Modo prova: riconosce ma NON agisce.", "Test mode: recognizes but does NOT act."));
        // Footer hint.
        d.setTextColor(DIM, BG); d.setCursor(16, top_y + nucleo_app_content_height() - 14);
        d.print(s_tpl_count > 0 ? TR("Pronuncia un comando addestrato.", "Say a trained command.")
                                : TR("Nessun comando: usa Voice Trainer.", "No command: use Voice Trainer."));
        return;
    }

    conf_t c = classify(s_m.dist, s_m.second, s_m.radius);

    // Recognized word + verdict chip.
    d.fillRoundRect(8, y, W - 16, 46, 8, PANEL);
    d.setTextSize(2); d.setTextColor(FG, PANEL);
    char w[20]; snprintf(w, sizeof(w), "%.16s", s_m.word);
    d.setCursor(14, y + 6); d.print(w);
    // verdict label, right-aligned
    const char *vl = tier_name(c.tier);
    int vw = (int)strlen(vl) * 6 + 10;
    d.fillRoundRect(W - 8 - vw, y + 6, vw, 14, 6, c.col);
    d.setTextSize(1); d.setTextColor(BG, c.col); d.setCursor(W - 8 - vw + 5, y + 9); d.print(vl);
    conf_bar(14, y + 30, W - 28, c.pct, c.col);

    // What ANIMA resolved it to (test mode: not executed).
    int ry = y + 54;
    d.setTextSize(1);
    if (s_have_r) {
        const char *verb = TR("Risposta", "Answer");
        if (s_r.action == ANIMA_ACT_LAUNCH)      verb = TR("Avvierebbe", "Would launch");
        else if (s_r.action == ANIMA_ACT_SYSTEM) verb = TR("Stato", "Status");
        else if (!s_r.matched)                   verb = TR("Nessun comando", "No command");
        d.setTextColor(ACC, BG); d.setCursor(12, ry); d.print(verb);
        d.setTextColor(FG, BG);  d.setCursor(12, ry + 12);
        char rb[34]; snprintf(rb, sizeof(rb), "%.32s", s_r.matched ? s_r.reply : "—");
        d.print(rb);
    }

    // Raw telemetry + counter (small).
    d.setTextColor(MUTED, BG); d.setCursor(12, ry + 30);
    char tm[40]; snprintf(tm, sizeof(tm), "d %ld  2o %ld  r %ld",
                          (long)s_m.dist, (long)s_m.second, (long)s_m.radius);
    d.print(tm);
    char cnt[18]; snprintf(cnt, sizeof(cnt), "n.%d", s_recog);
    d.setCursor(W - (int)strlen(cnt) * 6 - 12, ry + 30); d.print(cnt);
}

// DIAGNOSI list providers.
static const char *dg_label(int i, void *) { return s_tpl_names[i]; }
static const char *dg_right(int i, void *) { return tier_name(s_diag_tier[i]); }
static unsigned short dg_color(int i, void *) { return TIER_COL[s_diag_tier[i]]; }

static void draw_diagnosi(int top_y)
{
    draw_tabs(top_y);
    int y0 = top_y + 26;
    int h  = nucleo_app_content_height() - 26 - 14;

    if (s_tpl_count == 0) {
        d.setTextSize(1); d.setTextColor(DIM, BG);
        d.setCursor(12, y0 + 14); d.print(TR("Nessun comando addestrato.", "No trained command."));
        d.setCursor(12, y0 + 30); d.print(TR("Apri Voice Trainer per crearne.", "Open Voice Trainer to create one."));
        return;
    }
    app_ui_list(y0, h, s_tpl_count, s_sel, dg_label, dg_right, dg_color, nullptr);

    // Footer: legend / guidance.
    int fy = top_y + nucleo_app_content_height() - 14;
    d.fillRect(0, fy, W, 14, BG); d.drawFastHLine(0, fy, W, LINE);
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(8, fy + 3);
    d.print(TR("Prova ogni comando; rosso = riaddestra", "Test each command; red = retrain"));
}

static void draw(void)
{
    int top = nucleo_app_content_top();
    d.fillRect(0, top, 240, nucleo_app_content_height(), BG);
    if (s_tab == 0) draw_prova(top);
    else            draw_diagnosi(top);
}

// ── Registration ──────────────────────────────────────────────────────────────
extern "C" void nucleo_register_voicelab(void)
{
    static const nucleo_app_def_t app = {
        "voicelab", "Voice Lab", "Voice",
        "Test and diagnose voice recognition",
        'P', ACC, enter, on_key, tick, draw, exit_app
    };
    nucleo_app_register(&app);
}
