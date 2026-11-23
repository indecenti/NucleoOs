// XiaoZhi — a voice assistant client, store only (native module, ABI 10: the microphone and TLS).
//
// Speaks the open XiaoZhi protocol (github.com/78/xiaozhi-esp32, MIT): the public service at xiaozhi.me, or a server
// of your own (xiaozhi-esp32-server) by its ws:// address. Hold GO (or press Enter) and speak, release (or Enter) to
// send; the answer is spoken back and its text shown. The protocol and the audio live in xz_core.c; this file is the
// UI.
//
// The Cardputer: the microphone (red while it listens, green while it speaks) and its status on the left, the
// conversation on the right (Up / Down scroll it). With the external panel the Cardputer becomes the companion (the
// microphone, the status, the keys) and the panel shows only the conversation, the way ANIMA does: your words on the
// right, the answers behind an accent bar on the left, the state in the header. Drag to scroll; tap the last answer
// to hear it again.
//
// First start: choose the server ('1' = xiaozhi.me, or type a ws:// / https:// address), then, for xiaozhi.me, add
// the device on the service's console with the 6-digit code shown. Keys: GO hold / Enter talk, r replay the last
// answer, Up / Down scroll, s server, f forget this device (a new activation).
// Store only: compiled for the native module (NAP_MODULE) and by the host harnesses, empty in the firmware.
#if defined(NAP_MODULE) || !defined(ESP_PLATFORM)
#include "nucleo_app.h"
#include "nucleo_board.h"      // NUCLEO_SD_MOUNT
#include "nucleo_theme.h"
#include "nucleo_touch.h"
#include "nucleo_kbd.h"
#include "app_large.h"
#include "app_gfx.h"
#include "xz_core.h"
#include <M5GFX.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

static const unsigned short ACC = 0x2E7F, REC = 0xF8A6, OK = 0x47EA, WHITE = 0xFFFF;

// ---- language (settings.json -> ui.language; nucleo_tr5 is not in the ABI) --------------------------------------
static int s_lang = 1;                                      // 0 it, 1 en, 2 es, 3 fr, 4 de
struct L5 { const char *t[5]; };
static const char *tr(const L5 &l) { return l.t[s_lang]; }
static const char *const LOCALE[5] = { "it-IT", "en-US", "es-ES", "fr-FR", "de-DE" };

static void read_lang(void)
{
    s_lang = 1;
    FILE *f = fopen(NUCLEO_SD_MOUNT "/system/config/settings.json", "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    if (n <= 0 || n > 16384) { fclose(f); return; }
    char *buf = (char *)malloc(n + 1);
    if (!buf) { fclose(f); return; }
    size_t got = fread(buf, 1, n, f); fclose(f); buf[got] = 0;
    cJSON *root = cJSON_Parse(buf); free(buf);
    if (!root) return;
    cJSON *ui = cJSON_GetObjectItem(root, "ui");
    cJSON *lg = ui ? cJSON_GetObjectItem(ui, "language") : nullptr;
    if (lg && lg->valuestring) {
        static const char *const CODES[5] = { "it", "en", "es", "fr", "de" };
        for (int i = 0; i < 5; i++) if (!strncmp(lg->valuestring, CODES[i], 2)) s_lang = i;
    }
    cJSON_Delete(root);
}

static const L5 T_SETUP   = { { "Scegli il server", "Choose a server", "Elige el servidor", "Choisis le serveur", "Server waehlen" } };
static const L5 T_PRESET  = { { "1  xiaozhi.me (servizio pubblico)", "1  xiaozhi.me (public service)", "1  xiaozhi.me (servicio publico)",
                                "1  xiaozhi.me (service public)", "1  xiaozhi.me (oeffentlich)" } };
static const L5 T_TYPE    = { { "o scrivi ws://... e Invio", "or type ws://... and Enter", "o escribe ws://... e Intro",
                                "ou tape ws://... et Entree", "oder ws://... tippen, Enter" } };
static const L5 T_BIND    = { { "Codice per xiaozhi.me:", "Code for xiaozhi.me:", "Codigo para xiaozhi.me:",
                                "Code pour xiaozhi.me :", "Code fuer xiaozhi.me:" } };
// The state in the user's language (the core's status strings are English: kept for errors, which name a cause).
static const L5 T_ST[] = {
    { { "scegli il server", "choose a server", "elige el servidor", "choisis le serveur", "Server waehlen" } },
    { { "contatto il servizio", "contacting the service", "contactando", "connexion au service", "verbinde" } },
    { { "aspetto l'associazione", "waiting to be added", "esperando el alta", "en attente d'ajout", "warte auf Freigabe" } },
    { { "pronto", "ready", "listo", "pret", "bereit" } },
    { { "ti ascolto", "listening", "escucho", "j'ecoute", "hoere zu" } },
    { { "preparo", "encoding", "preparo", "encodage", "kodiere" } },
    { { "invio", "sending", "enviando", "envoi", "sende" } },
    { { "penso...", "thinking...", "pensando...", "je reflechis...", "denke..." } },
    { { "parlo", "speaking", "hablando", "je parle", "spreche" } },
};
static const L5 T_WIFI    = { { "aspetto il Wi-Fi", "waiting for Wi-Fi", "esperando el Wi-Fi", "attente du Wi-Fi", "warte auf WLAN" } };
static const L5 T_HOLD    = { { "Tieni premuto GO e parla", "Hold GO and speak", "Manten GO y habla", "Maintiens GO et parle",
                                "GO halten und sprechen" } };
static const L5 T_HINT    = { { "GO/Invio parla  r riascolta  s server", "GO/Enter talk  r replay  s server",
                                "GO/Intro habla  r repetir  s servidor", "GO/Entree parle  r reecoute  s serveur",
                                "GO/Enter reden  r nochmal  s Server" } };
static const L5 T_COMPK   = { { "GO/Invio parla  r riascolta  \x18\x19 scorri", "GO/Enter talk  r replay  \x18\x19 scroll",
                                "GO/Intro habla  r repetir  \x18\x19 mover", "GO/Entree parle  r reecoute  \x18\x19 defile",
                                "GO/Enter reden  r nochmal  \x18\x19 blaettern" } };

// ---- state --------------------------------------------------------------------------------------------------------
static bool s_editing;                                      // typing a server address
static char s_edit[96];
static int s_back_lines;                                    // how far the conversation is scrolled up (0 = the end)

static void edit_open(void) { s_editing = true; s_edit[0] = 0; nucleo_app_request_draw(); }

static void talk_toggle(void)
{
    const xz_state_t st = xz_view()->state;
    if (st == XZ_RECORDING) xz_talk(false);
    else if (st == XZ_IDLE || st == XZ_ERROR) { s_back_lines = 0; xz_talk(true); }
    nucleo_app_request_draw();
}

static void replay(void) { if (xz_replay()) nucleo_app_request_draw(); }

// ---- drawing without flicker -------------------------------------------------------------------------------------
// The app draws straight to the screens (no canvas: its 31 KB are the codec's), so nothing is cleared and redrawn
// unless it changed. A "page" (setup, activation, conversation, scroll position) is repainted whole only when it
// changes or when the framework asks for a frame on its own; the meter, the seconds and the progress repaint only
// the microphone's ring and the status line, each over its own pixels.
static uint32_t fnv(uint32_t h, const char *s) { for (; s && *s; s++) h = (h ^ (uint8_t)*s) * 16777619u; return h; }

static uint32_t page_key(const xz_view_t *v)
{
    uint32_t h = 2166136261u;
    if (s_editing || v->state == XZ_SETUP) return fnv(fnv(h, "setup"), s_edit);
    if (v->state == XZ_ACTIVATE) return fnv(fnv(fnv(h, "act"), v->code), v->status);
    const xz_turn_t *t; const int n = xz_history(&t);
    h = fnv(h, "talk");
    for (int i = 0; i < n; i++) h = fnv(fnv(h, t[i].user), t[i].reply);
    h = fnv(fnv(h, v->user), v->reply);
    return (h ^ (uint32_t)s_back_lines) * 16777619u;
}

static unsigned short mic_color(const xz_view_t *v)
{
    return v->state == XZ_RECORDING ? REC : v->state == XZ_PLAYING ? OK : ACC;
}

static int isqrt(int n) { int r = 0; while ((r + 1) * (r + 1) <= n) r++; return r; }

// The annulus ri < radius <= ro in one colour, scanline by scanline (no gaps, no pixel drawn twice). lower = only
// the bottom half (dy >= 0): the microphone's holder.
static void ring(int cx, int cy, int ri, int ro, unsigned short col, bool lower = false)
{
    if (ro <= ri) return;
    for (int dy = lower ? 0 : -ro; dy <= ro; dy++) {
        const int xo = isqrt(ro * ro - dy * dy + ro);       // radius + 1/2: round, no one-pixel nub at the equator
        const int xi = (dy >= -ri && dy <= ri) ? isqrt(ri * ri - dy * dy + ri) + 1 : 0;
        if (xi == 0) d.drawFastHLine(cx - xo, cy + dy, 2 * xo + 1, col);
        else if (xo >= xi) {
            d.drawFastHLine(cx - xo, cy + dy, xo - xi + 1, col);
            d.drawFastHLine(cx + xi, cy + dy, xo - xi + 1, col);
        }
    }
}

// A studio microphone in white on the disc: a rounded capsule with three grille slots, the U-shaped holder around
// its lower half, the stem and the foot. Proportional to the disc's radius r.
static void mic_glyph(int cx, int cy, int r, unsigned short disc)
{
    const int cw = r * 9 / 20 | 1, ch = r * 4 / 5, top = cy - r * 11 / 20;       // the capsule
    d.fillRoundRect(cx - cw / 2, top, cw, ch, cw / 2, WHITE);
    const int slot = r > 30 ? 2 : 1;
    for (int i = 1; i <= 3; i++) d.fillRect(cx - cw / 4, top + ch * i / 5, cw / 2 + 1, slot, disc);
    const int hy = top + ch / 2, hr = r * 9 / 20, ht = r > 30 ? 3 : 2;          // the holder: a lower half ring
    ring(cx, hy, hr - ht, hr, WHITE, true);
    d.fillRect(cx - hr, hy - r / 10, ht, r / 10, WHITE);                        // its two arms up to mid-capsule
    d.fillRect(cx + hr - ht + 1, hy - r / 10, ht, r / 10, WHITE);
    const int sy = hy + hr, sh = r / 5;                                         // stem and foot
    d.fillRect(cx - ht / 2, sy, ht, sh, WHITE);
    d.fillRoundRect(cx - r * 3 / 10, sy + sh, r * 3 / 5, ht + 1, ht / 2 + 1, WHITE);
}

// The microphone: the disc and its glyph only when its colour changes (or on a whole repaint); the meter ring around
// it every time, glow then background out to the largest ring it can have.
static void mic(int cx, int cy, int r, int grow, const xz_view_t *v, bool whole, unsigned short *last)
{
    const unsigned short c = mic_color(v);
    if (whole || c != *last) {
        d.fillCircle(cx, cy, r, c);
        mic_glyph(cx, cy, r, c);
        *last = c;
    }
    const int g = v->state == XZ_RECORDING ? v->level * grow / 100 : 0;
    ring(cx, cy, r, r + g, (unsigned short)(REC & 0x7BEF));
    ring(cx, cy, r + g, r + grow + 1, THEME_BG);
}

// One line of text in a box (centred, or right-aligned): over its own background, the rest of the box cleared.
static void line_box(int x, int y, int w, int h, const char *t, unsigned short fg, unsigned short bg, bool right = false)
{
    d.setTextDatum(top_left);
    d.setTextColor(fg, bg);
    char f[96]; lg::fit(f, sizeof f, t, w);
    const int tw = d.textWidth(f), tx = right ? x + w - tw : x + (w - tw) / 2;
    d.fillRect(x, y, tx - x, h, bg);
    d.drawString(f, tx, y);
    d.fillRect(tx + tw, y, x + w - tx - tw, h, bg);
}

static const char *state_word(const xz_view_t *v)
{
    if (!strcmp(v->status, "waiting for Wi-Fi")) return tr(T_WIFI);
    if (v->state == XZ_ERROR) return v->status;              // the cause, as the core names it
    if (v->state == XZ_IDLE && strcmp(v->status, "ready")) return v->status;   // "too short", "nothing heard"...
    return (int)v->state < (int)(sizeof T_ST / sizeof T_ST[0]) ? tr(T_ST[v->state]) : v->status;
}

static const char *state_line(const xz_view_t *v)
{
    static char line[96];
    switch (v->state) {
    case XZ_RECORDING: snprintf(line, sizeof line, "%s  %d.%d s", tr(T_ST[XZ_RECORDING]), v->rec_ms / 1000, v->rec_ms % 1000 / 100); return line;
    case XZ_ENCODING: case XZ_PLAYING: snprintf(line, sizeof line, "%s %d%%", state_word(v), v->progress / 10 * 10); return line;
    default: return state_word(v);
    }
}

// ---- the conversation, laid out in lines -------------------------------------------------------------------------
// Every turn (the history, then this one): the user's words right-aligned, the answer left behind an accent bar, a
// gap between turns. Lines point into the core's strings (stable between two polls); recomputed on a whole repaint.
enum { L_USER = 0, L_ANSWER = 1, L_GAP = 2 };
struct Line { const char *p; uint16_t n; uint8_t kind; uint8_t last; };   // last: belongs to the newest answer
static const int MAXL = 96;
static Line s_lines[MAXL];
static int s_nlines;

static int text_w(const char *p, int n)
{
    char b[128]; if (n > 127) n = 127;
    memcpy(b, p, n); b[n] = 0;
    return d.textWidth(b);
}

static void wrap_into(const char *s, int maxw, uint8_t kind, uint8_t last)
{
    while (*s == ' ') s++;
    while (*s && s_nlines < MAXL) {
        int n = 0, fit = 0;
        while (s[n]) {                                       // grow word by word while it fits
            int e = n; while (s[e] == ' ') e++;
            while (s[e] && s[e] != ' ') e++;
            if (text_w(s, e) > maxw) break;
            fit = n = e;
        }
        if (!fit) {                                          // one word wider than the line: cut it
            fit = 1; while (s[fit] && text_w(s, fit + 1) <= maxw) fit++;
        }
        s_lines[s_nlines++] = { s, (uint16_t)fit, kind, last };
        s += fit; while (*s == ' ') s++;
    }
}

static void layout(const xz_view_t *v, int maxw)
{
    s_nlines = 0;
    const xz_turn_t *t; const int n = xz_history(&t);
    for (int i = 0; i <= n; i++) {
        const char *u = i < n ? t[i].user : v->user, *a = i < n ? t[i].reply : v->reply;
        if (!u[0] && !a[0]) continue;
        if (s_nlines && s_nlines < MAXL) s_lines[s_nlines++] = { "", 0, L_GAP, 0 };
        if (u[0]) wrap_into(u, maxw * 4 / 5, L_USER, 0);
        if (a[0]) wrap_into(a, maxw, L_ANSWER, i == n || (i == n - 1 && !v->user[0] && !v->reply[0]));
    }
}

// Draws the lines that fit in [x, x+w) x [y, y+h) at line height lh, the end of the conversation at the bottom unless
// scrolled up. Returns the first line index shown. bar = the answers' accent bar colour.
static int chat(int x, int y, int w, int h, int lh, unsigned short fg, unsigned short muted, unsigned short bg)
{
    const int vis = h / lh;
    if (s_back_lines > s_nlines - vis) s_back_lines = s_nlines - vis > 0 ? s_nlines - vis : 0;
    const int first = s_nlines > vis ? s_nlines - vis - s_back_lines : 0;
    d.setTextDatum(top_left);
    for (int i = first, row = 0; i < s_nlines && row < vis; i++, row++) {
        const Line &L = s_lines[i];
        const int ly = y + row * lh;
        if (L.kind == L_GAP) continue;
        char b[128]; const int n = L.n < 127 ? L.n : 127; memcpy(b, L.p, n); b[n] = 0;
        if (L.kind == L_USER) {
            d.setTextColor(muted, bg);
            d.drawString(b, x + w - d.textWidth(b), ly);
        } else {
            d.fillRect(x, ly, 3, lh, ACC);                  // the answer's accent bar
            d.setTextColor(fg, bg);
            d.drawString(b, x + 8, ly);
        }
    }
    if (s_back_lines > 0) d.fillTriangle(x + w - 6, y + h - 2, x + w, y + h - 2, x + w - 3, y + h - 6, ACC);   // more below
    return first;
}

// ---- the Cardputer (240x135) -----------------------------------------------------------------------------------
static uint32_t s_c_key = 0, s_c_seq = 0;
static unsigned short s_c_mic = 0;
static bool s_c_valid = false;

static void draw(void)
{
    const xz_view_t *v = xz_view();
    const int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    const bool comp = nucleo_app_two_screens();
    const uint32_t key = page_key(v) ^ (comp ? 0x9E3779B9u : 0);
    // Whole: the page changed, or a frame nobody in this app asked for (seq unchanged: the framework repaints).
    const bool whole = !s_c_valid || key != s_c_key || v->seq == s_c_seq;
    s_c_key = key; s_c_seq = v->seq; s_c_valid = true;
    int y = top + 2;
    if (whole) d.fillRect(0, top, 240, h, THEME_BG);
    if (comp) y = lg::comp_head("XiaoZhi", ACC);             // same pixels each time: no visible change
    d.setTextDatum(top_left);
    if (s_editing || v->state == XZ_SETUP) {
        d.setFont(&fonts::Font2); d.setTextColor(ACC, THEME_BG); d.drawString(tr(T_SETUP), 6, y); y += 18;
        d.setTextColor(THEME_FG, THEME_BG); d.drawString(tr(T_PRESET), 6, y); y += 16;
        d.setTextColor(THEME_MUTED, THEME_BG); d.drawString(tr(T_TYPE), 6, y); y += 18;
        d.fillRoundRect(4, y, 232, 20, 5, lg::card()); d.drawRoundRect(4, y, 232, 20, 5, ACC);
        char shown[64]; d.setFont(&fonts::Font0);
        const char *t = s_edit; while (*t && d.textWidth(t) > 220) t++;
        snprintf(shown, sizeof shown, "%s_", t);
        d.setTextColor(THEME_FG, lg::card()); d.drawString(shown, 9, y + 7);
        return;
    }
    if (v->state == XZ_ACTIVATE) {
        if (!whole) return;
        d.setFont(&fonts::Font0); d.setTextColor(THEME_FG, THEME_BG);
        d.drawString(tr(T_BIND), 6, y + 2);
        lg::font(3); d.setTextDatum(top_center); d.setTextColor(ACC, THEME_BG);
        d.drawString(v->code, 120, y + 20);
        d.setFont(&fonts::Font0); d.setTextColor(THEME_MUTED, THEME_BG); d.drawString(state_word(v), 120, y + 66);
        d.setTextDatum(top_left);
        return;
    }
    if (comp) {
        // The companion: the microphone big, the status beside it, the keys. The conversation is on the panel.
        mic(46, y + 44, 30, 10, v, whole, &s_c_mic);
        d.setFont(&fonts::Font2);
        line_box(92, y + 30, 144, 16, state_line(v), mic_color(v) == ACC ? THEME_FG : mic_color(v), THEME_BG);
        if (whole) {
            d.setFont(&fonts::Font0); d.setTextColor(THEME_MUTED, THEME_BG);
            d.drawString(tr(T_HOLD), 92, y + 52);
            lg::comp_keys(tr(T_COMPK));
        }
        return;
    }
    // One screen: the microphone and its status on the left, the conversation on the right.
    mic(32, y + 30, 22, 8, v, whole, &s_c_mic);
    d.setFont(&fonts::Font0);
    char secs[16]; snprintf(secs, sizeof secs, "%d.%d s", v->rec_ms / 1000, v->rec_ms % 1000 / 100);
    line_box(0, y + 64, 64, 8, v->state == XZ_RECORDING ? secs : state_line(v), THEME_MUTED, THEME_BG);   // 64 px: the seconds alone
    if (!whole) return;
    d.setFont(&fonts::Font0);
    layout(v, 162);
    if (s_nlines) chat(70, y, 166, top + h - y - 2, 10, THEME_FG, THEME_MUTED, THEME_BG);
    else { d.setFont(&fonts::Font2); d.setTextColor(THEME_FG, THEME_BG); d.drawString(tr(T_HOLD), 72, y + 22); }
}

// ---- the panel (320 x 210 under the system bar, 320 x 240 without it): the conversation, as ANIMA -----------------
static int s_ph = 210;
static uint32_t s_l_key = 0, s_l_seq = 0;
static bool s_l_valid = false;
static int s_l_first = 0, s_l_y0 = 0, s_l_lh = 18;          // where the last whole frame put its lines (for taps)

static void lg_paint(bool whole)
{
    const xz_view_t *v = xz_view();
    const int H = s_ph, W = 320;
    if (s_editing || v->state == XZ_SETUP || v->state == XZ_ACTIVATE) {
        if (whole) {
            d.fillRect(0, 0, W, H, THEME_BG);
            lg::panel(10, 10, 300, H - 20);
            d.setTextDatum(top_center);
            if (v->state == XZ_ACTIVATE && !s_editing) {
                lg::font(0); d.setTextColor(THEME_FG, lg::card()); d.drawString(tr(T_BIND), 160, 40);
                lg::font(3); d.setTextColor(ACC, lg::card()); d.drawString(v->code, 160, 80);
                lg::font(4); d.setTextColor(THEME_MUTED, lg::card()); d.drawString(state_word(v), 160, 140);
            } else {
                lg::font(1); d.setTextColor(ACC, lg::card()); d.drawString(tr(T_SETUP), 160, 30);
                lg::font(0); d.setTextColor(THEME_FG, lg::card()); d.drawString(tr(T_PRESET), 160, 70);
                d.setTextColor(THEME_MUTED, lg::card()); d.drawString(tr(T_TYPE), 160, 96);
                char shown[100]; snprintf(shown, sizeof shown, "%s_", s_edit);
                char fitted[100]; lg::fit(fitted, sizeof fitted, shown, 270);
                d.setTextColor(THEME_FG, lg::card()); d.drawString(fitted, 160, 130);
            }
            d.setTextDatum(top_left);
        }
        lg::field(0, 0, W, H);                               // a tap on the setup card = preset 1
        return;
    }
    // The header: back, the name, the state at the right (repainted alone as it changes).
    if (whole) lg::header("XiaoZhi", "", ACC);
    else nucleo_touch_add(0, 0, 96, lg::HEAD + 2, NK_BACK, 0);   // the header's Back target, kept between frames
    lg::font(0);
    const unsigned short sc = mic_color(v) == ACC ? THEME_MUTED : mic_color(v);
    line_box(120, 7, W - 130, 16, state_line(v), sc, THEME_BG, true);
    const int y0 = lg::HEAD + 4, bh = H - y0 - 4, lh = 18;
    if (whole) {
        d.fillRect(0, y0, W, H - y0, THEME_BG);
        lg::font(0);
        layout(v, W - 28);
        if (s_nlines) s_l_first = chat(10, y0, W - 20, bh, lh, THEME_FG, THEME_MUTED, THEME_BG);
        else {
            d.setTextDatum(middle_center); d.setTextColor(THEME_MUTED, THEME_BG);
            d.drawString(tr(T_HOLD), W / 2, y0 + bh / 2);
            d.setTextDatum(top_left);
        }
        s_l_y0 = y0; s_l_lh = lh;
    }
    // Drag scrolls the conversation (a line per step); a tap on the newest answer plays it again.
    nucleo_touch_add_scroll(0, y0, W, bh, lh, NK_CHAR, NUCLEO_TOUCH_VIEW_BACK, NK_CHAR, NUCLEO_TOUCH_VIEW_FWD);
    lg::field(0, y0, W, bh);
}

static void draw_large(void)
{
    const int ph = d.height() > 0 && d.height() <= 240 ? d.height() : 210;
    const xz_view_t *v = xz_view();
    const uint32_t key = page_key(v);
    const bool whole = !s_l_valid || key != s_l_key || v->seq == s_l_seq || ph != s_ph;
    s_ph = ph; s_l_key = key; s_l_seq = v->seq; s_l_valid = true;
    // Straight to the panel, not lg::frame: frame composes through the shared canvas and would take back the
    // 31 KB block this app gave up for the codec.
    nucleo_touch_clear();
    lg_paint(whole);
    nucleo_touch_commit();
}

static bool lg_tap(int key, char ch)
{
    int x, y;
    if (!lg::tap(key, ch, &x, &y)) return false;
    const xz_state_t st = xz_view()->state;
    if (s_editing || st == XZ_SETUP) { s_editing = false; xz_set_server(XZ_DEFAULT_OTA); }
    else if (st != XZ_ACTIVATE) {
        const int row = (y - s_l_y0) / (s_l_lh > 0 ? s_l_lh : 18), i = s_l_first + row;
        if (i >= 0 && i < s_nlines && s_lines[i].kind == L_ANSWER && s_lines[i].last) replay();
    }
    nucleo_app_request_draw();
    return true;
}

// ---- input and lifecycle ---------------------------------------------------------------------------------------
static void scroll(int by)
{
    s_back_lines += by;
    if (s_back_lines < 0) s_back_lines = 0;                  // the upper bound is clamped by chat()
    nucleo_app_request_draw();
}

static void on_key(int key, char ch)
{
    if (lg_tap(key, ch)) return;
    const xz_state_t st = xz_view()->state;
    if (s_editing || st == XZ_SETUP) {
        if (!s_editing) s_editing = true;
        const size_t n = strlen(s_edit);
        if (key == NK_CHAR && ch == '1' && n == 0) { s_editing = false; xz_set_server(XZ_DEFAULT_OTA); }
        else if (key == NK_CHAR && ch >= ' ' && n + 1 < sizeof s_edit) { s_edit[n] = ch; s_edit[n + 1] = 0; }
        else if (key == NK_DEL && n) s_edit[n - 1] = 0;
        else if (key == NK_ENTER && n) { if (xz_set_server(s_edit)) s_editing = false; }
        nucleo_app_request_draw();
        return;
    }
    if (key == NK_ENTER) talk_toggle();
    else if (key == NK_UP || (key == NK_CHAR && ch == NUCLEO_TOUCH_VIEW_BACK)) scroll(1);
    else if (key == NK_DOWN || (key == NK_CHAR && ch == NUCLEO_TOUCH_VIEW_FWD)) scroll(-1);
    else if (key == NK_CHAR && ch == 'r') replay();
    else if (key == NK_CHAR && ch == 's') edit_open();
    else if (key == NK_CHAR && ch == 'f') { xz_forget(); nucleo_app_request_draw(); }
}

static bool on_back(int key)
{
    (void)key;
    if (s_editing && xz_view()->state != XZ_SETUP) { s_editing = false; nucleo_app_request_draw(); return true; }
    return false;                                            // close the app
}

static void on_ptt(bool on)
{
    const xz_state_t st = xz_view()->state;
    if (on && (st == XZ_IDLE || st == XZ_ERROR)) { s_back_lines = 0; xz_talk(true); }
    else if (!on && st == XZ_RECORDING) xz_talk(false);
    nucleo_app_request_draw();
}

static bool on_poll(void)
{
    const bool changed = xz_poll();
    const xz_state_t st = xz_view()->state;
    nucleo_app_set_keep_awake(st != XZ_IDLE && st != XZ_SETUP && st != XZ_ERROR);
    if (changed) nucleo_app_request_draw();
    return changed;
}

static void on_enter(void)
{
    read_lang();
    s_editing = false; s_edit[0] = 0; s_back_lines = 0;
    s_c_valid = s_l_valid = false;                           // the first frame on each screen is whole
    // The 31 KB off-screen canvas would hold the heap's largest block, and the Opus encoder needs 24.6 KB in one
    // piece: draw straight to the screens instead (like ANIMA).
    nucleo_app_release_buffers();
    nucleo_app_set_direct_draw(true);
    xz_init(NUCLEO_SD_MOUNT "/data/xiaozhi", LOCALE[s_lang]);
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_ptt_handler(on_ptt);
    nucleo_app_set_poll_handler(on_poll);
    nucleo_app_set_hint(tr(T_HINT));
}

static void on_exit(void) { xz_shutdown(); }

extern "C" void nucleo_register_xiaozhi(void)
{
    static const nucleo_app_def_t app = {
        "xiaozhi", "XiaoZhi", "Tools", "Voice assistant (XiaoZhi protocol)",
        'X', ACC, on_enter, on_key, nullptr, draw, on_exit, 0, nullptr, draw_large
    };
    nucleo_app_register(&app);
}
#endif  // NAP_MODULE || !ESP_PLATFORM
