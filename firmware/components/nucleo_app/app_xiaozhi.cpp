// XiaoZhi — a voice assistant client, store only (native module, ABI 10: the microphone and TLS).
//
// Speaks the open XiaoZhi protocol (github.com/78/xiaozhi-esp32, MIT): the public service at xiaozhi.me, or a server
// of your own (xiaozhi-esp32-server) by its ws:// address. Hold GO (or press Enter / tap the circle) and speak,
// release to send; the answer is spoken back and its text shown. The protocol and the audio live in xz_core.c; this
// file is the UI: the Cardputer's screen, and with the external panel a native 320x240 view where the circle is the
// microphone itself (tap it to talk, tap again to send).
//
// First start: choose the server ('1' = xiaozhi.me, or type a ws:// / https:// address), then, for xiaozhi.me, add
// the device on the service's console with the 6-digit code shown. Keys: GO hold / Enter talk, s server, f forget
// this device (a new activation).
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

static const unsigned short ACC = 0x2E7F, REC = 0xF8A6, OK = 0x47EA;

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
static const L5 T_BIND    = { { "Aggiungi il dispositivo su xiaozhi.me con:", "Add the device on xiaozhi.me with:",
                                "Anade el dispositivo en xiaozhi.me con:", "Ajoute l'appareil sur xiaozhi.me avec :",
                                "Geraet auf xiaozhi.me hinzufuegen mit:" } };
static const L5 T_HOLD    = { { "Tieni GO e parla", "Hold GO and speak", "Manten GO y habla", "Maintiens GO et parle", "GO halten, sprechen" } };
static const L5 T_TAPTALK = { { "Tocca e parla", "Tap and speak", "Toca y habla", "Touche et parle", "Tippen, sprechen" } };
static const L5 T_TAPSEND = { { "Tocca per inviare", "Tap to send", "Toca para enviar", "Touche pour envoyer", "Tippen zum Senden" } };
static const L5 T_YOU     = { { "Tu", "You", "Tu", "Toi", "Du" } };
static const L5 T_HINT    = { { "GO/Invio parla  s server  f nuovo", "GO/Enter talk  s server  f new id",
                                "GO/Intro habla  s servidor  f nuevo", "GO/Entree parle  s serveur  f nouveau",
                                "GO/Enter reden  s Server  f neu" } };

// ---- state --------------------------------------------------------------------------------------------------------
static bool s_editing;                                      // typing a server address
static char s_edit[96];

static void edit_open(void) { s_editing = true; s_edit[0] = 0; nucleo_app_request_draw(); }

static void talk_toggle(void)
{
    const xz_state_t st = xz_view()->state;
    if (st == XZ_RECORDING) xz_talk(false);
    else if (st == XZ_IDLE || st == XZ_ERROR) xz_talk(true);
    nucleo_app_request_draw();
}

// ---- drawing without flicker -------------------------------------------------------------------------------------
// The app draws straight to the screens (no canvas: its 31 KB are the codec's). So nothing is cleared and redrawn
// unless it changed: a "page" (setup, activation, conversation) is repainted whole only when its text changes or the
// framework asks for a frame on its own; the meter, the seconds and the progress repaint only the microphone's ring
// and the status line, each over its own pixels.
static uint32_t fnv(uint32_t h, const char *s) { for (; s && *s; s++) h = (h ^ (uint8_t)*s) * 16777619u; return h; }

static uint32_t page_key(const xz_view_t *v)
{
    const uint32_t h = 2166136261u;
    if (s_editing || v->state == XZ_SETUP) return fnv(fnv(h, "setup"), s_edit);
    if (v->state == XZ_ACTIVATE) return fnv(fnv(fnv(h, "act"), v->code), v->status);
    return fnv(fnv(fnv(h, "talk"), v->user), v->reply);
}

static unsigned short mic_color(const xz_view_t *v)
{
    return v->state == XZ_RECORDING ? REC : v->state == XZ_PLAYING ? OK : ACC;
}

static int isqrt(int n) { int r = 0; while ((r + 1) * (r + 1) <= n) r++; return r; }

// The annulus ri < radius <= ro in one colour, scanline by scanline (no gaps, no pixel drawn twice).
static void ring(int cx, int cy, int ri, int ro, unsigned short col)
{
    if (ro <= ri) return;
    for (int dy = -ro; dy <= ro; dy++) {
        const int xo = isqrt(ro * ro - dy * dy);
        const int xi = (dy > -ri && dy < ri) ? isqrt(ri * ri - dy * dy) + 1 : 0;
        if (xi == 0) d.drawFastHLine(cx - xo, cy + dy, 2 * xo + 1, col);
        else if (xo >= xi) {
            d.drawFastHLine(cx - xo, cy + dy, xo - xi + 1, col);
            d.drawFastHLine(cx + xi, cy + dy, xo - xi + 1, col);
        }
    }
}

// The microphone: the disc and its glyph only when its colour changes (or on a whole repaint); the meter ring around
// it every time, glow then background out to the largest ring it can have.
static void mic(int cx, int cy, int r, int grow, const xz_view_t *v, bool whole, unsigned short *last)
{
    const unsigned short c = mic_color(v);
    if (whole || c != *last) {
        d.fillCircle(cx, cy, r, c);
        const int t = r > 30 ? 3 : 2;
        d.fillRoundRect(cx - r / 4, cy - r / 2, r / 2, r * 2 / 3, r / 4, THEME_BG);
        for (int o = 0; o < t; o++) {                                                 // the stand: a U under it
            d.drawLine(cx - r * 2 / 5 + o, cy - r / 8, cx - r * 2 / 5 + o, cy + r / 5, THEME_BG);
            d.drawLine(cx + r * 2 / 5 - o, cy - r / 8, cx + r * 2 / 5 - o, cy + r / 5, THEME_BG);
            d.drawLine(cx - r * 2 / 5, cy + r / 5 + o, cx + r * 2 / 5, cy + r / 5 + o, THEME_BG);
        }
        d.fillRect(cx - t / 2, cy + r / 5 + t, t, r / 5, THEME_BG);
        *last = c;
    }
    const int g = v->state == XZ_RECORDING ? v->level * grow / 100 : 0;
    ring(cx, cy, r, r + g, (unsigned short)(REC & 0x7BEF));
    ring(cx, cy, r + g, r + grow + 1, THEME_BG);
}

// One line of text centred in a box: the text over its own background, the rest of the box cleared around it.
static void line_box(int x, int y, int w, int h, const char *t, unsigned short fg)
{
    d.setTextDatum(top_left);
    d.setTextColor(fg, THEME_BG);
    char f[80]; lg::fit(f, sizeof f, t, w);
    const int tw = d.textWidth(f), tx = x + (w - tw) / 2;
    d.fillRect(x, y, tx - x, h, THEME_BG);
    d.drawString(f, tx, y);
    d.fillRect(tx + tw, y, x + w - tx - tw, h, THEME_BG);
}

static const char *state_line(const xz_view_t *v)
{
    static char line[96];
    switch (v->state) {
    case XZ_RECORDING: snprintf(line, sizeof line, "%d.%d s", v->rec_ms / 1000, v->rec_ms % 1000 / 100); return line;
    case XZ_ENCODING: case XZ_PLAYING: snprintf(line, sizeof line, "%s %d%%", v->status, v->progress / 10 * 10); return line;
    default: return v->status;
    }
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
    const uint32_t key = page_key(v);
    // Whole: the page changed, or a frame nobody in this app asked for (seq unchanged: the framework repaints).
    const bool whole = !s_c_valid || key != s_c_key || v->seq == s_c_seq;
    s_c_key = key; s_c_seq = v->seq; s_c_valid = true;
    int y = top + 2;
    if (whole) {
        d.fillRect(0, top, 240, h, THEME_BG);
        if (comp) y = lg::comp_head("XiaoZhi", ACC);
    } else if (comp) {
        y = lg::comp_head("XiaoZhi", ACC);                   // same pixels as before: no visible change
    }
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
        if (comp && whole) lg::comp_keys(tr(T_HINT));
        return;
    }
    if (v->state == XZ_ACTIVATE) {
        if (!whole) return;
        d.setFont(&fonts::Font0); d.setTextColor(THEME_FG, THEME_BG);
        d.drawString(tr(T_BIND), 6, y + 2);
        lg::font(3); d.setTextDatum(top_center); d.setTextColor(ACC, THEME_BG);
        d.drawString(v->code, 120, y + 20);
        d.setFont(&fonts::Font0); d.setTextColor(THEME_MUTED, THEME_BG); d.drawString(v->status, 120, y + 66);
        d.setTextDatum(top_left);
        return;
    }
    // the microphone on the left (its meter grows at most 10 px: it never reaches the text at x 72)
    mic(34, y + 34, 24, 10, v, whole, &s_c_mic);
    d.setFont(&fonts::Font0);
    line_box(0, y + 72, 68, 8, state_line(v), THEME_MUTED);
    if (!whole) return;
    int ty = y;
    if (v->user[0]) {
        d.setFont(&fonts::Font0); d.setTextColor(THEME_MUTED, THEME_BG);
        char l[64]; snprintf(l, sizeof l, "%s:", tr(T_YOU)); d.drawString(l, 72, ty);
        d.setTextColor(THEME_FG, THEME_BG);
        ty += 10 * lg::wrap(v->user, 72, ty + 10, 164, 10, 2) + 12;
    }
    if (v->reply[0]) {
        d.setFont(&fonts::Font2); d.setTextColor(ACC, THEME_BG);
        lg::wrap(v->reply, 72, ty, 164, 15, (top + h - 12 - ty) / 15);
    } else if (!v->user[0]) {
        d.setFont(&fonts::Font2); d.setTextColor(THEME_FG, THEME_BG);
        d.drawString(tr(T_HOLD), 72, y + 26);
    }
    if (comp) lg::comp_keys(tr(T_HINT));
}

// ---- the panel (320 x 210 under the system bar, 320 x 240 without it) -------------------------------------------
static int s_ph = 210;
static const int CX = 92, R = 56, GROW = 20;
static uint32_t s_l_key = 0, s_l_seq = 0;
static unsigned short s_l_mic = 0;
static xz_state_t s_l_state = XZ_SETUP;
static bool s_l_valid = false;

static void lg_paint(bool whole)
{
    const xz_view_t *v = xz_view();
    const int H = s_ph;
    if (s_editing || v->state == XZ_SETUP || v->state == XZ_ACTIVATE) {
        if (whole) {
            d.fillRect(0, 0, 320, H, THEME_BG);
            lg::panel(10, 10, 300, H - 20);
            d.setTextDatum(top_center);
            if (v->state == XZ_ACTIVATE && !s_editing) {
                lg::font(0); d.setTextColor(THEME_FG, lg::card()); d.drawString(tr(T_BIND), 160, 40);
                lg::font(3); d.setTextColor(ACC, lg::card()); d.drawString(v->code, 160, 80);
                lg::font(4); d.setTextColor(THEME_MUTED, lg::card()); d.drawString(v->status, 160, 140);
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
        lg::field(0, 0, 320, H);                             // a tap on the setup card = preset 1
        return;
    }
    const int cy = H / 2;
    if (whole) d.fillRect(0, 0, 320, H, THEME_BG);
    mic(CX, cy, R, GROW, v, whole, &s_l_mic);
    lg::font(4);
    line_box(CX - 80, cy - R - GROW - 12, 160, 10, state_line(v), THEME_MUTED);
    if (whole || v->state != s_l_state) {                    // what a tap does, under the microphone
        lg::font(0);
        const char *under = v->state == XZ_RECORDING ? tr(T_TAPSEND) : (v->state == XZ_IDLE || v->state == XZ_ERROR) ? tr(T_TAPTALK) : "";
        line_box(CX - 80, cy + R + GROW + 2, 160, 16, under, THEME_MUTED);
        if (v->heap_block) {                                 // heap at the last phase change (the field numbers)
            char m[40]; snprintf(m, sizeof m, "heap %u/%u KB", (unsigned)(v->heap_free / 1024), (unsigned)(v->heap_block / 1024));
            lg::font(4); line_box(4, H - 12, 150, 10, m, THEME_MUTED);
        }
        s_l_state = v->state;
    }
    if (whole) {                                             // the conversation
        const int X = 172, W = 140;
        lg::panel(X - 6, 8, W + 10, H - 16);
        d.setTextDatum(top_left);
        int y = 16;
        if (v->user[0]) {
            lg::font(4); d.setTextColor(THEME_MUTED, lg::card()); d.drawString(tr(T_YOU), X, y); y += 10;
            lg::font(0); d.setTextColor(THEME_FG, lg::card()); y += 16 * lg::wrap(v->user, X, y, W, 16, 3) + 6;
        }
        if (v->reply[0]) {
            lg::font(0); d.setTextColor(ACC, lg::card());
            lg::wrap(v->reply, X, y, W, 16, (H - 24 - y) / 16);
        }
    }
    lg::field(CX - R - 8, cy - R - 8, 2 * R + 16, 2 * R + 16);   // the microphone itself is the touch target
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
    else if (st != XZ_ACTIVATE) talk_toggle();
    nucleo_app_request_draw();
    return true;
}

// ---- input and lifecycle ---------------------------------------------------------------------------------------
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
    if (on && (st == XZ_IDLE || st == XZ_ERROR)) xz_talk(true);
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
    s_editing = false; s_edit[0] = 0;
    s_c_valid = s_l_valid = false;                           // the first frame on each screen is whole
    // The 31 KB off-screen canvas would hold the heap's largest block, and the Opus encoder needs 24.6 KB in one
    // piece: draw straight to the panel instead (like ANIMA), every draw repaints what it shows.
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
