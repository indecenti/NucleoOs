// Launcher rendering. See launcher_render.h.
#include "launcher_render.h"
#include "launcher_menu.h"
#include "nucleo_kbd.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include "esp_system.h"      // esp_restart() for the SISTEMA > Riavvia action
#include "esp_heap_caps.h"   // free-heap readout for the SISTEMA > RAM row
#include "nucleo_i18n.h"     // system language (TR/it-en) for the Control Center labels

// The real display (defined in nucleo_ui.cpp). The launcher always draws to it directly;
// only the animated list band is composited off-screen first. We do NOT include app_gfx.h
// here on purpose — that header redefines `d` as the movable app-draw target, but the
// launcher chrome must always hit the physical screen.
extern M5GFX d;

// Shared off-screen back-buffer (nucleo_ui.cpp). The launcher list band composites into it and
// blits the band region with a clipped push; apps reuse the same canvas. See app_gfx.h.
M5Canvas *nucleo_screen(void);
void      nucleo_screen_release(void);

// Network info + pairing PIN (resolved at link; no component dependency).
extern "C" const char *nucleo_setup_mode(void);        // "sta" | "ap"
extern "C" const char *nucleo_setup_ssid(void);
extern "C" const char *nucleo_setup_ip(void);
extern "C" const char *nucleo_setup_device_name(void);
extern "C" const char *nucleo_auth_pin(void);

// Evil Portal live state, so the status bar can flag it while it runs in the background.
extern "C" bool nucleo_evilportal_running(void);
extern "C" int  nucleo_evilportal_captures(void);
extern "C" bool          nucleo_wifiatk_deauth_running(void);   // background radio-offensive ops: alert bar
extern "C" unsigned long nucleo_wifiatk_frames(void);
extern "C" bool          nucleo_wifiatk_beacon_running(void);
extern "C" int           nucleo_wifiatk_beacon_count(void);

// Quick-settings the Control Center drives: backlight, audio, battery, system.
extern "C" void nucleo_app_set_brightness(int pct);
extern "C" int  nucleo_app_brightness(void);
extern "C" int  nucleo_audio_volume(void);
extern "C" void nucleo_audio_set_volume(int pct);
extern "C" bool nucleo_audio_is_muted(void);
extern "C" void nucleo_audio_set_mute(bool muted);
extern "C" bool nucleo_audio_is_playing(void);
extern "C" bool nucleo_audio_is_paused(void);
extern "C" const char *nucleo_audio_path(void);
extern "C" int  nucleo_power_battery_pct(void);             // 0..100, -1 = unavailable

// System hooks (nucleo_setup, nucleo_usbmsc — resolved at final link, no cycle).
extern "C" int  nucleo_setup_rssi(void);
extern "C" bool nucleo_setup_time_synced(void);
extern "C" void nucleo_usbmsc_request(void);

// ---- hint + instruction text ------------------------------------------------
static char s_hint[48] = "";
static char s_instr[44] = "";          // one-line description of the focused row
static unsigned short s_hint_bg = INK, s_hint_fg = MUTED;   // hint-bar theme (app-overridable)
void        launcher_render_set_hint(const char *h) { strncpy(s_hint, h ? h : "", sizeof(s_hint) - 1); }
const char *launcher_render_hint(void) { return s_hint; }
void        launcher_render_set_hint_colors(unsigned short bg, unsigned short fg) { s_hint_bg = bg; s_hint_fg = fg; }
void        launcher_render_reset_hint_colors(void) { s_hint_bg = INK; s_hint_fg = MUTED; }

void launcher_render_update_chrome(void)
{
    // Description line: what the focused row does (falls back to the menu's own blurb).
    const MenuNode *cur = launcher_focused();
    const char *desc = (cur && cur->desc && cur->desc[0]) ? cur->desc : launcher_node()->desc;
    strncpy(s_instr, desc ? desc : "", sizeof(s_instr) - 1);
    s_instr[sizeof(s_instr) - 1] = 0;

    // Hint line: the controls that actually do something right now. The launcher is a 2-D tile grid,
    // so the four arrows MOVE the focus; backtick (Esc) goes back. The context menu (`/`) is gone:
    // its native actions were no-ops (pin/info) and ENTER already opens the app. Worst-case home hint
    // "arrows move  enter open  esc back" is 33 chars (~198 px), well within the 240 px width.
    if (launcher_filter()[0]) {
        snprintf(s_hint, sizeof(s_hint), "filtro \"%.12s\"   -   esc azzera", launcher_filter());
    } else if (launcher_depth() > 0) {
        snprintf(s_hint, sizeof(s_hint), "scorri   -   invio apre   -   esc indietro");
    } else {
        snprintf(s_hint, sizeof(s_hint), "scorri   -   invio apre");
    }
    s_hint[sizeof(s_hint) - 1] = 0;
}

// Signature gate for the CHROME (status + hint bars). They are STATIC while you scroll WITHIN a menu —
// the clock is the 1 Hz tick's job, and the per-item description lives in the hero card (the buffered
// LIST), not here. The old code marked the chrome dirty on EVERY launcher key, so it re-wiped the top and
// bottom bars (direct fillRect = clear-then-draw) on each press = visible flicker. Latch a signature of
// what the bars actually SHOW and report a change only when it differs: depth/category, the /filter,
// and the Wi-Fi bar level + mode. Per ANTI-FLICKER.md (gate on real content change). The chrome does
// NOT depend on WHICH row is focused (the grid shows the name in the buffered LIST, not in the bars),
// so the focused row is deliberately OUT of the signature: mixing its kind in re-wiped the bars on
// every scroll that crossed an app/category boundary (e.g. ANIMA -> a category on Home) = scroll
// flicker. The clock/date are excluded too — they ride the 1 Hz in-place tick, not a wipe.
bool launcher_render_chrome_changed(void)
{
    const MenuNode *node = launcher_node();
    bool sta  = !strcmp(nucleo_setup_mode(), "sta") && nucleo_setup_ssid()[0];
    int  rssi = nucleo_setup_rssi();
    int  wlvl = (!sta || rssi == 0) ? 1 : rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -78 ? 2 : 1;  // same map as draw_wifi
    uint32_t sig = 2166136261u;                       // FNV-1a over the chrome-visible state
    #define MIX(v) do { sig = (sig ^ (uint32_t)(v)) * 16777619u; } while (0)
    MIX(launcher_depth());
    MIX((uintptr_t)(node ? node->id : 0));            // category identity (breadcrumb glyph/name/count)
    MIX(sta ? 1 : 0); MIX(wlvl);                      // Wi-Fi gauge (coarse bars, not dBm noise)
    for (const char *f = launcher_filter(); *f; f++) MIX((unsigned char)*f);  // /filter chip + filter hint
    #undef MIX
    static uint32_t s_last = 0; static bool s_init = false;
    if (s_init && sig == s_last) return false;
    s_last = sig; s_init = true; return true;
}

// ---- chrome (drawn directly; static during a scroll) ------------------------
// Number of children in a menu node (for the "how many inside" badge). 0 for apps/actions.
static int node_child_count(const MenuNode *m)
{
    if (!m || m->kind != N_MENU || !m->items) return 0;
    int n = 0; while (m->items[n]) n++; return n;
}

// Honest Wi-Fi indicator: four rising bars whose FILL tracks the real signal. STA = green, AP/setup
// = amber; bars above the measured level are drawn dim so the glyph reads as a strength gauge (the
// smartwatch idiom) rather than a flat icon. rssi 0 (not associated) -> 1 amber bar. There is no
// battery on bare M5GFX, so the top-right is spent entirely on this. Occupies a 19x9 box at (x,y).
static void draw_wifi(int x, int y, bool sta, int rssi)
{
    unsigned short on = sta ? C_GREEN : C_YELLOW;
    // Map dBm to 0..4 lit bars (>=-55 full, <=-88 one). AP mode / unknown -> a single amber bar.
    int lvl;
    if (!sta || rssi == 0) lvl = 1;
    else if (rssi >= -55)  lvl = 4;
    else if (rssi >= -67)  lvl = 3;
    else if (rssi >= -78)  lvl = 2;
    else                   lvl = 1;
    for (int i = 0; i < 4; i++) {
        int bh = 2 + i * 2;                                  // 2,4,6,8 px tall
        d.fillRect(x + i * 5, y + 9 - bh, 3, bh, i < lvl ? on : LINE);
    }
}

// Battery gauge removed: the ADC reading was unreliable on this unit, so showing it was misleading.
// The date (day + month) now occupies that right-cluster space instead.

// ---- launcher icon set (bold filled silhouettes) ---------------------------------------
// Each icon is a bold filled glyph centred at (cx,cy) inside a 2*r box, using one ink colour `col`
// plus the badge colour `bg` for clean cut-outs (so internal detail reads without thin outlines).
// This is the hand-kept port of web/device/icons.js — KEEP THE TWO IN SYNC (same coords, same forms).
// Unmapped ids fall back to a bold letter glyph so a new app never draws blank.

// Thick line as a filled quad (exact width, no drawWideLine ambiguity). Used for the few diagonals.
template <typename T> static void icon_line(T *g, float x0, float y0, float x1, float y1, float w, uint16_t c)
{
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) { g->fillCircle((int)lroundf(x0), (int)lroundf(y0), (int)lroundf(w / 2), c); return; }
    float px = -dy / len * (w / 2), py = dx / len * (w / 2);
    g->fillTriangle((int)lroundf(x0 + px), (int)lroundf(y0 + py), (int)lroundf(x0 - px), (int)lroundf(y0 - py), (int)lroundf(x1 + px), (int)lroundf(y1 + py), c);
    g->fillTriangle((int)lroundf(x1 + px), (int)lroundf(y1 + py), (int)lroundf(x1 - px), (int)lroundf(y1 - py), (int)lroundf(x0 - px), (int)lroundf(y0 - py), c);
}

template <typename T> static void ui_icon(T *g, int cx, int cy, int r, const char *id, char letter, uint16_t col, uint16_t bg)
{
    if (!id) id = "";
    const float s = (float)r, Tk = (s * 0.22f > 2.0f ? s * 0.22f : 2.0f);
    #define RI(v)              ((int)lroundf((float)(v)))
    #define CLR(rr,w,h)        RI(fminf((float)(rr), fminf((float)(w) / 2.0f, (float)(h) / 2.0f)))
    #define IBX(x,y,w,h,c)     g->fillRect(RI(x),RI(y),RI(w),RI(h),(c))
    #define IFC(x,y,rr,c)      g->fillCircle(RI(x),RI(y),RI(rr),(c))
    #define ITR(a,b,p,q,u,v,c) g->fillTriangle(RI(a),RI(b),RI(p),RI(q),RI(u),RI(v),(c))
    #define IRR(x,y,w,h,rr,c)  g->fillRoundRect(RI(x),RI(y),RI(w),RI(h),CLR(rr,w,h),(c))
    #define IL(a,b,p,q,w,c)    icon_line(g,(float)(a),(float)(b),(float)(p),(float)(q),(float)(w),(c))
    #define IEL(x,y,rx,ry,c)   do { g->drawEllipse(RI(x),RI(y),RI(rx),RI(ry),(c)); g->drawEllipse(RI(x),RI(y),RI((float)(rx)-1.0f),RI((float)(ry)-1.0f),(c)); } while (0)

    if (!strcmp(id, "clock")) {
        IFC(cx, cy, s, col); IFC(cx, cy, s - Tk, bg);
        IBX(cx - Tk / 2, cy - s * 0.6f, Tk, s * 0.6f, col); IBX(cx, cy - Tk / 2, s * 0.5f, Tk, col); IFC(cx, cy, Tk * 0.7f, col);
    } else if (!strcmp(id, "anima")) {
        float a = s, b = s * 0.34f;
        ITR(cx, cy - a, cx - b, cy, cx + b, cy, col); ITR(cx, cy + a, cx - b, cy, cx + b, cy, col);
        ITR(cx - a, cy, cx, cy - b, cx, cy + b, col); ITR(cx + a, cy, cx, cy - b, cx, cy + b, col);
        float gx = cx + s * 0.66f, gy = cy - s * 0.66f, c = s * 0.3f;
        ITR(gx, gy - c, gx - c * 0.4f, gy, gx + c * 0.4f, gy, col); ITR(gx, gy + c, gx - c * 0.4f, gy, gx + c * 0.4f, gy, col);
    } else if (!strcmp(id, "calc")) {
        IRR(cx - s, cy - s, 2 * s, 2 * s, s * 0.28f, col);
        IRR(cx - s * 0.66f, cy - s * 0.7f, s * 1.32f, s * 0.5f, 2, bg);
        for (int ry = 0; ry < 2; ry++) for (int cc = 0; cc < 3; cc++) IFC(cx - s * 0.5f + cc * s * 0.5f, cy + s * 0.08f + ry * s * 0.5f, Tk * 0.45f, bg);
    } else if (!strcmp(id, "files")) {
        IRR(cx - s, cy - s * 0.6f, s * 0.95f, s * 0.4f, Tk * 0.6f, col);
        IRR(cx - s, cy - s * 0.3f, 2 * s, s * 1.5f, s * 0.22f, col);
        IBX(cx - s * 0.7f, cy - s * 0.1f, s * 1.4f, Tk * 0.55f, bg);
    } else if (!strcmp(id, "calendar")) {
        IRR(cx - s, cy - s * 0.78f, 2 * s, s * 1.66f, s * 0.22f, col);
        IBX(cx - s, cy - s * 0.78f, 2 * s, s * 0.5f, col);
        IRR(cx - s + Tk, cy - s * 0.28f + Tk, 2 * s - 2 * Tk, s * 1.16f - 2 * Tk, 2, bg);
        IBX(cx - s * 0.55f, cy - s, Tk, s * 0.34f, col); IBX(cx + s * 0.55f - Tk, cy - s, Tk, s * 0.34f, col);
        IFC(cx, cy + s * 0.34f, Tk * 0.7f, col);
    } else if (!strcmp(id, "notepad")) {
        IRR(cx - s * 0.8f, cy - s, s * 1.6f, 2 * s, s * 0.2f, col);
        for (int i = 0; i < 3; i++) IBX(cx - s * 0.5f, cy - s * 0.45f + i * s * 0.5f, s * (1.0f - i * 0.18f), Tk * 0.55f, bg);
    } else if (!strcmp(id, "usb")) {
        IBX(cx - Tk, cy - s, 2 * Tk, s * 0.4f, col);
        IRR(cx - s * 0.5f, cy - s * 0.62f, s, s * 1.62f, s * 0.2f, col);
        IBX(cx - s * 0.5f, cy + s * 0.12f, s, Tk * 0.55f, bg);
    } else if (!strcmp(id, "usbkbd")) {
        IRR(cx - s, cy - s * 0.6f, 2 * s, s * 1.2f, s * 0.22f, col);
        for (int ry = 0; ry < 2; ry++) for (int cc = 0; cc < 4; cc++) IBX(cx - s * 0.72f + cc * s * 0.46f, cy - s * 0.32f + ry * s * 0.42f, Tk * 0.6f, Tk * 0.6f, bg);
        IBX(cx - s * 0.4f, cy + s * 0.34f, s * 0.8f, Tk * 0.55f, bg);
    } else if (!strcmp(id, "music")) {
        IFC(cx - s * 0.45f, cy + s * 0.55f, Tk * 1.05f, col);
        IBX(cx - s * 0.45f + Tk * 0.6f, cy - s * 0.82f, Tk * 0.8f, s * 1.45f, col);
        ITR(cx - s * 0.45f + Tk * 1.4f, cy - s * 0.82f, cx - s * 0.45f + Tk * 1.4f, cy - s * 0.12f, cx + s * 0.72f, cy - s * 0.45f, col);
    } else if (!strcmp(id, "video")) {
        IRR(cx - s, cy - s * 0.75f, 2 * s, s * 1.5f, s * 0.22f, col);
        ITR(cx - s * 0.3f, cy - s * 0.42f, cx - s * 0.3f, cy + s * 0.42f, cx + s * 0.5f, cy, bg);
    } else if (!strcmp(id, "Media")) {
        ITR(cx - s * 0.7f, cy - s, cx - s * 0.7f, cy + s, cx + s, cy, col);
    } else if (!strcmp(id, "photos")) {
        IRR(cx - s, cy - s, 2 * s, 2 * s, s * 0.22f, col);
        IRR(cx - s + Tk, cy - s + Tk, 2 * s - 2 * Tk, 2 * s - 2 * Tk, 2, bg);
        IFC(cx - s * 0.4f, cy - s * 0.4f, Tk * 0.8f, col);
        ITR(cx - s * 0.85f, cy + s * 0.62f, cx - s * 0.1f, cy - s * 0.15f, cx + s * 0.25f, cy + s * 0.62f, col);
        ITR(cx - s * 0.05f, cy + s * 0.62f, cx + s * 0.45f, cy + s * 0.05f, cx + s * 0.85f, cy + s * 0.62f, col);
    } else if (!strcmp(id, "recorder") || !strcmp(id, "voice") || !strcmp(id, "Voice")) {
        IRR(cx - Tk * 1.15f, cy - s, Tk * 2.3f, s * 1.25f, Tk * 1.15f, col);
        IL(cx - s * 0.58f, cy, cx - s * 0.58f, cy + s * 0.22f, Tk * 0.6f, col); IL(cx + s * 0.58f, cy, cx + s * 0.58f, cy + s * 0.22f, Tk * 0.6f, col);
        IL(cx - s * 0.58f, cy + s * 0.2f, cx, cy + s * 0.5f, Tk * 0.6f, col); IL(cx + s * 0.58f, cy + s * 0.2f, cx, cy + s * 0.5f, Tk * 0.6f, col);
        IBX(cx - Tk / 2, cy + s * 0.45f, Tk, s * 0.35f, col); IBX(cx - s * 0.45f, cy + s * 0.78f, s * 0.9f, Tk * 0.6f, col);
    } else if (!strcmp(id, "micspec")) {
        static const float mh[5] = { 0.55f, 1.0f, 0.4f, 0.85f, 0.6f };
        for (int i = 0; i < 5; i++) IBX(cx - s + i * (2 * s / 5.0f) + Tk * 0.2f, cy + s - 2 * s * mh[i], Tk, 2 * s * mh[i], col);
    } else if (!strcmp(id, "voicelab")) {
        IRR(cx - s, cy - s * 0.8f, 2 * s, s * 1.25f, s * 0.3f, col);
        ITR(cx - s * 0.5f, cy + s * 0.35f, cx - s * 0.5f, cy + s, cx, cy + s * 0.4f, col);
        for (int i = -1; i <= 1; i++) IFC(cx + i * s * 0.5f, cy - s * 0.18f, Tk * 0.5f, bg);
    } else if (!strcmp(id, "info") || !strcmp(id, "Connect")) {
        IFC(cx, cy + s * 0.7f, Tk * 0.85f, col);
        ITR(cx - s * 0.55f, cy + s * 0.15f, cx + s * 0.55f, cy + s * 0.15f, cx, cy + s * 0.55f, col);
        ITR(cx - s * 0.28f, cy + s * 0.33f, cx + s * 0.28f, cy + s * 0.33f, cx, cy + s * 0.55f, bg);
        ITR(cx - s, cy - s * 0.42f, cx + s, cy - s * 0.42f, cx, cy + s * 0.1f, col);
        ITR(cx - s * 0.62f, cy - s * 0.18f, cx + s * 0.62f, cy - s * 0.18f, cx, cy + s * 0.1f, bg);
    } else if (!strcmp(id, "sysmon")) {
        static const float sh[3] = { 0.5f, 1.0f, 0.7f };
        for (int i = 0; i < 3; i++) IBX(cx - s * 0.8f + i * s * 0.72f, cy + s * 0.7f - 1.4f * s * sh[i], Tk, 1.4f * s * sh[i], col);
        IBX(cx - s, cy + s * 0.7f, 2 * s, Tk * 0.55f, col);
    } else if (!strcmp(id, "System")) {
        IRR(cx - s * 0.7f, cy - s * 0.7f, s * 1.4f, s * 1.4f, s * 0.16f, col);
        IRR(cx - s * 0.3f, cy - s * 0.3f, s * 0.6f, s * 0.6f, 2, bg);
        for (int i = -1; i <= 1; i++) {
            IBX(cx + i * s * 0.42f - Tk * 0.3f, cy - s, Tk * 0.6f, s * 0.32f, col); IBX(cx + i * s * 0.42f - Tk * 0.3f, cy + s * 0.68f, Tk * 0.6f, s * 0.32f, col);
            IBX(cx - s, cy + i * s * 0.42f - Tk * 0.3f, s * 0.32f, Tk * 0.6f, col); IBX(cx + s * 0.68f, cy + i * s * 0.42f - Tk * 0.3f, s * 0.32f, Tk * 0.6f, col);
        }
    } else if (!strcmp(id, "radio")) {
        IRR(cx - s, cy - s * 0.3f, 2 * s, s * 1.25f, s * 0.18f, col);
        IL(cx + s * 0.4f, cy - s * 0.3f, cx + s * 0.85f, cy - s, Tk * 0.6f, col); IFC(cx + s * 0.85f, cy - s, Tk * 0.6f, col);
        IFC(cx - s * 0.45f, cy + s * 0.32f, s * 0.38f, bg); IFC(cx + s * 0.5f, cy + s * 0.32f, Tk * 0.8f, bg);
    } else if (!strcmp(id, "remote")) {
        IRR(cx - s, cy - s * 0.8f, 2 * s, s * 1.3f, s * 0.18f, col);
        IRR(cx - s + Tk, cy - s * 0.8f + Tk, 2 * s - 2 * Tk, s * 1.3f - 2 * Tk, 2, bg);
        IFC(cx - s * 0.55f, cy + s * 0.82f, Tk * 0.7f, col); ITR(cx - s * 0.78f, cy + s * 0.55f, cx - s * 0.32f, cy + s * 0.55f, cx - s * 0.55f, cy + s * 0.85f, col);
    } else if (!strcmp(id, "ir")) {
        IRR(cx - s * 0.5f, cy - s, s, 2 * s, s * 0.3f, col);
        IFC(cx - s * 0.02f, cy - s * 0.62f, Tk * 0.45f, bg); IBX(cx - s * 0.22f, cy - s * 0.12f, s * 0.4f, Tk * 0.45f, bg); IBX(cx - s * 0.22f, cy + s * 0.28f, s * 0.4f, Tk * 0.45f, bg);
        IL(cx + s * 0.6f, cy - s * 0.7f, cx + s, cy - s, Tk * 0.6f, col); IL(cx + s * 0.6f, cy - s * 0.35f, cx + s, cy - s * 0.55f, Tk * 0.6f, col);
    } else if (!strcmp(id, "qr")) {
        const float e = s * 0.6f; static const int o[3][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 } };
        for (int k = 0; k < 3; k++) { int ox = o[k][0], oy = o[k][1];
            IBX(cx + ox * e - e * 0.5f, cy + oy * e - e * 0.5f, e, e, col); IBX(cx + ox * e - e * 0.26f, cy + oy * e - e * 0.26f, e * 0.52f, e * 0.52f, bg); IFC(cx + ox * e, cy + oy * e, e * 0.16f, col); }
        static const float d[4][2] = { { 0.32f, 0.32f }, { 0.72f, 0.42f }, { 0.42f, 0.72f }, { 0.74f, 0.74f } };
        for (int k = 0; k < 4; k++) IBX(cx + d[k][0] * s, cy + d[k][1] * s, Tk * 0.55f, Tk * 0.55f, col);
    } else if (!strcmp(id, "notify")) {
        IRR(cx - s * 0.72f, cy - s * 0.7f, s * 1.44f, s * 1.2f, s * 0.7f, col);
        IBX(cx - s * 0.82f, cy + s * 0.34f, s * 1.64f, Tk * 0.65f, col);
        IBX(cx - Tk / 2, cy - s, Tk, Tk * 0.8f, col); IFC(cx, cy + s * 0.74f, Tk * 0.6f, col);
    } else if (!strcmp(id, "torch")) {
        IRR(cx - s, cy - s * 0.45f, s * 0.9f, s * 0.9f, s * 0.18f, col);
        ITR(cx - s * 0.12f, cy - s * 0.62f, cx + s * 0.38f, cy - s, cx + s * 0.38f, cy + s, col); ITR(cx - s * 0.12f, cy + s * 0.62f, cx + s * 0.38f, cy + s, cx + s * 0.38f, cy - s, col);
        IBX(cx + s * 0.34f, cy - s * 0.55f, Tk * 0.8f, s * 1.1f, col);
        IL(cx + s * 0.6f, cy - s * 0.5f, cx + s, cy - s * 0.8f, Tk * 0.55f, col); IL(cx + s * 0.6f, cy, cx + s, cy, Tk * 0.55f, col); IL(cx + s * 0.6f, cy + s * 0.5f, cx + s, cy + s * 0.8f, Tk * 0.55f, col);
    } else if (!strcmp(id, "theme")) {
        IFC(cx, cy, s, col); IBX(cx, cy - s, s + 1, 2 * s, bg); IEL(cx, cy, s, s, col);
    } else if (!strcmp(id, "wifi")) {
        for (int i = 0; i < 3; i++) { float yy = cy - s * 0.55f + i * s * 0.55f; IBX(cx - s, yy - Tk * 0.28f, 2 * s, Tk * 0.55f, col); IFC(cx - s * 0.5f + i * s * 0.5f, yy, Tk * 0.85f, col); }
    } else if (!strcmp(id, "link")) {
        float a = s * 0.78f;
        IL(cx - a, cy, cx + a, cy - a, Tk * 0.6f, col); IL(cx - a, cy, cx + a, cy + a, Tk * 0.6f, col);
        IFC(cx - a, cy, Tk * 1.05f, col); IFC(cx + a, cy - a, Tk * 1.05f, col); IFC(cx + a, cy + a, Tk * 1.05f, col);
    } else if (!strcmp(id, "swarm")) {
        // a hub with peers all around it — a mesh/swarm of devices (distinct from link's 3 nodes)
        const float R = s * 0.84f;
        static const float o[6][2] = { {1.0f,0.0f},{0.5f,0.87f},{-0.5f,0.87f},{-1.0f,0.0f},{-0.5f,-0.87f},{0.5f,-0.87f} };
        for (int k = 0; k < 6; k++) { float px = cx + R * o[k][0], py = cy + R * o[k][1];
            IL(cx, cy, px, py, Tk * 0.5f, col); IFC(px, py, Tk * 0.66f, col); }
        IFC(cx, cy, Tk * 1.05f, col);
    } else if (!strcmp(id, "ssh")) {
        IRR(cx - s, cy - s * 0.8f, 2 * s, s * 1.6f, s * 0.18f, col);
        IBX(cx - s, cy - s * 0.8f, 2 * s, s * 0.42f, col); IRR(cx - s + Tk * 0.6f, cy - s * 0.28f, 2 * s - Tk * 1.2f, s - Tk * 0.6f, 2, bg);
        ITR(cx - s * 0.5f, cy - s * 0.05f, cx - s * 0.5f, cy + s * 0.32f, cx - s * 0.08f, cy + s * 0.14f, col);
        IBX(cx, cy + s * 0.24f, s * 0.45f, Tk * 0.55f, col);
    } else if (!strcmp(id, "ethernet")) {
        IRR(cx - s * 0.72f, cy - s * 0.8f, s * 1.44f, s * 1.3f, s * 0.16f, col);
        for (int i = 0; i < 4; i++) IBX(cx - s * 0.5f + i * s * 0.32f, cy - s * 0.8f, Tk * 0.45f, s * 0.4f, bg);
        IBX(cx - Tk * 0.7f, cy + s * 0.5f, Tk * 1.4f, s * 0.5f, col);
    } else if (!strcmp(id, "beacon")) {
        ITR(cx - s * 0.8f, cy + s, cx + s * 0.8f, cy + s, cx, cy - s * 0.15f, col);
        IRR(cx - s * 0.5f, cy + s * 0.82f, s, Tk * 0.7f, 2, bg);
        IFC(cx, cy - s * 0.28f, Tk * 0.8f, col);
        IL(cx + s * 0.34f, cy - s * 0.55f, cx + s * 0.62f, cy - s * 0.78f, Tk * 0.5f, col); IL(cx + s * 0.34f, cy - s * 0.18f, cx + s * 0.66f, cy - s * 0.32f, Tk * 0.5f, col);
        IL(cx - s * 0.34f, cy - s * 0.55f, cx - s * 0.62f, cy - s * 0.78f, Tk * 0.5f, col); IL(cx - s * 0.34f, cy - s * 0.18f, cx - s * 0.66f, cy - s * 0.32f, Tk * 0.5f, col);
    } else if (!strcmp(id, "wifiatk")) {
        IFC(cx - s * 0.3f, cy + s * 0.58f, Tk * 0.75f, col);
        ITR(cx - s * 0.8f, cy + s * 0.05f, cx + s * 0.2f, cy + s * 0.05f, cx - s * 0.3f, cy + s * 0.5f, col); ITR(cx - s * 0.55f, cy + s * 0.22f, cx - s * 0.05f, cy + s * 0.22f, cx - s * 0.3f, cy + s * 0.5f, bg);
        IL(cx + s * 0.1f, cy - s, cx + s, cy - s * 0.2f, Tk * 0.85f, col); IL(cx + s, cy - s, cx + s * 0.1f, cy - s * 0.2f, Tk * 0.85f, col);
    } else if (!strcmp(id, "evilportal")) {
        IRR(cx - s, cy - s * 0.85f, 2 * s, s * 1.7f, s * 0.16f, col);
        IBX(cx - s, cy - s * 0.85f, 2 * s, s * 0.4f, col); IRR(cx - s + Tk * 0.6f, cy - s * 0.4f + Tk * 0.6f, 2 * s - Tk * 1.2f, s * 1.18f, 2, bg);
        IRR(cx - s * 0.4f, cy + s * 0.02f, s * 0.8f, s * 0.56f, 2, col); IEL(cx, cy - s * 0.05f, s * 0.26f, s * 0.26f, col);
    } else if (!strcmp(id, "sniffer")) {
        // radar: a source dot with concentric signal rings + a sweep line (listening to the air)
        IEL(cx, cy, s * 0.95f, s * 0.95f, col);
        IEL(cx, cy, s * 0.60f, s * 0.60f, col);
        IFC(cx, cy, Tk * 0.9f, col);
        IL(cx, cy, cx + s * 0.78f, cy - s * 0.55f, Tk * 0.55f, col);
    } else if (!strcmp(id, "weather")) {
        // a sun behind a cloud
        IFC(cx - s * 0.30f, cy - s * 0.38f, s * 0.34f, col);                                  // sun disc
        IL(cx - s * 0.30f, cy - s * 0.96f, cx - s * 0.30f, cy - s * 0.74f, Tk * 0.7f, col);   // ray up
        IL(cx - s * 0.84f, cy - s * 0.38f, cx - s * 0.62f, cy - s * 0.38f, Tk * 0.7f, col);   // ray left
        IL(cx - s * 0.68f, cy - s * 0.76f, cx - s * 0.50f, cy - s * 0.58f, Tk * 0.7f, col);   // ray nw
        IL(cx + s * 0.06f, cy - s * 0.76f, cx - s * 0.10f, cy - s * 0.58f, Tk * 0.7f, col);   // ray ne
        IFC(cx - s * 0.50f, cy + s * 0.30f, s * 0.34f, col);                                  // cloud puffs
        IFC(cx + s * 0.55f, cy + s * 0.30f, s * 0.40f, col);
        IFC(cx + s * 0.05f, cy + s * 0.06f, s * 0.46f, col);
        IRR(cx - s * 0.85f, cy + s * 0.26f, s * 1.7f, s * 0.55f, s * 0.27f, col);
    } else if (!strcmp(id, "ble")) {
        // Bluetooth rune: vertical stem + the two crossed right "wings"
        float a = s * 0.6f, q = s * 0.5f;
        IL(cx, cy - s, cx, cy + s, Tk * 0.7f, col);
        IL(cx, cy - s, cx + a, cy - q, Tk * 0.7f, col);
        IL(cx + a, cy - q, cx - a, cy + q, Tk * 0.7f, col);
        IL(cx - a, cy - q, cx + a, cy + q, Tk * 0.7f, col);
        IL(cx + a, cy + q, cx, cy + s, Tk * 0.7f, col);
    } else if (!strcmp(id, "payloads")) {
        // a lightning bolt — keystroke injection
        ITR(cx - s * 0.30f, cy - s, cx + s * 0.40f, cy - s * 0.2f, cx - s * 0.10f, cy - s * 0.2f, col);
        ITR(cx + s * 0.30f, cy + s, cx - s * 0.40f, cy + s * 0.2f, cx + s * 0.10f, cy + s * 0.2f, col);
    } else if (!strcmp(id, "Security")) {
        IRR(cx - s, cy - s, 2 * s, s, s * 0.32f, col);
        ITR(cx - s, cy - s * 0.5f, cx + s, cy - s * 0.5f, cx, cy + s, col);
    } else if (!strcmp(id, "Games")) {
        IRR(cx - s, cy - s * 0.5f, 2 * s, s * 1.05f, s * 0.5f, col);
        IBX(cx - s * 0.62f, cy - Tk * 0.28f, s * 0.5f, Tk * 0.55f, bg); IBX(cx - s * 0.42f, cy - s * 0.25f, Tk * 0.55f, s * 0.5f, bg);
        IFC(cx + s * 0.4f, cy - s * 0.12f, Tk * 0.55f, bg); IFC(cx + s * 0.66f, cy + s * 0.1f, Tk * 0.55f, bg); IFC(cx + s * 0.14f, cy + s * 0.1f, Tk * 0.55f, bg);
    } else if (!strcmp(id, "reactor")) {
        IEL(cx, cy, s, s * 0.42f, col); IEL(cx, cy, s * 0.42f, s, col);
        IFC(cx, cy, Tk * 1.05f, col); IFC(cx + s * 0.92f, cy, Tk * 0.55f, col); IFC(cx, cy - s * 0.92f, Tk * 0.55f, col);
    } else if (!strcmp(id, "pong")) {
        // two paddles + ball + a hint of the centre net
        IRR(cx - s * 0.92f, cy - s * 0.6f, s * 0.26f, s * 1.2f, 2, col);   // left paddle
        IRR(cx + s * 0.66f, cy - s * 0.15f, s * 0.26f, s * 1.2f, 2, col);  // right paddle (offset)
        IFC(cx + s * 0.06f, cy + s * 0.04f, s * 0.22f, col);              // ball
        IFC(cx, cy - s * 0.62f, Tk * 0.35f, col); IFC(cx, cy + s * 0.62f, Tk * 0.35f, col);  // net dots
    } else if (!strcmp(id, "tanks")) {
        // tank: hull + wheels + turret + raised barrel
        IRR(cx - s * 0.9f, cy + s * 0.12f, s * 1.8f, s * 0.5f, 3, col);
        IFC(cx - s * 0.55f, cy + s * 0.66f, Tk * 0.55f, col); IFC(cx, cy + s * 0.66f, Tk * 0.55f, col); IFC(cx + s * 0.55f, cy + s * 0.66f, Tk * 0.55f, col);
        IRR(cx - s * 0.34f, cy - s * 0.22f, s * 0.68f, s * 0.42f, 2, col);
        IL(cx + s * 0.2f, cy - s * 0.05f, cx + s, cy - s * 0.62f, Tk * 0.6f, col);
    } else if (!strcmp(id, "stelle")) {
        static const float p[5][2] = { { -0.78f, -0.5f }, { -0.05f, -0.15f }, { 0.7f, -0.6f }, { 0.4f, 0.6f }, { -0.55f, 0.7f } };
        static const int lk[4][2] = { { 0, 1 }, { 1, 2 }, { 1, 3 }, { 3, 4 } };
        for (int k = 0; k < 4; k++) IL(cx + p[lk[k][0]][0] * s, cy + p[lk[k][0]][1] * s, cx + p[lk[k][1]][0] * s, cy + p[lk[k][1]][1] * s, Tk * 0.35f, col);
        static const float sr[5] = { 0.55f, 0.9f, 0.5f, 0.62f, 0.45f };
        for (int i = 0; i < 5; i++) IFC(cx + p[i][0] * s, cy + p[i][1] * s, Tk * sr[i], col);
    } else if (!strcmp(id, "giardino")) {
        IRR(cx - s * 0.85f, cy + s * 0.55f, s * 1.7f, s * 0.45f, s * 0.16f, col);
        IBX(cx - Tk * 0.4f, cy - s * 0.25f, Tk * 0.8f, s * 0.85f, col);
        IFC(cx - s * 0.34f, cy - s * 0.28f, s * 0.34f, col); IFC(cx + s * 0.34f, cy - s * 0.28f, s * 0.34f, col); IFC(cx, cy - s * 0.62f, s * 0.3f, col);
    } else if (!strcmp(id, "slots")) {
        IRR(cx - s * 0.85f, cy - s * 0.7f, s * 1.7f, s * 1.5f, s * 0.16f, col);
        IRR(cx - s * 0.6f, cy - s * 0.45f, s * 1.2f, s * 0.9f, 2, bg);
        IBX(cx - s * 0.2f, cy - s * 0.45f, Tk * 0.45f, s * 0.9f, col); IBX(cx + s * 0.2f - Tk * 0.45f, cy - s * 0.45f, Tk * 0.45f, s * 0.9f, col);
        IBX(cx + s * 0.85f, cy - s * 0.6f, Tk * 0.7f, s * 0.7f, col); IFC(cx + s * 0.85f + Tk * 0.35f, cy - s * 0.6f, Tk * 0.7f, col);
    } else if (!strcmp(id, "Tools")) {
        for (int i = 0; i < 8; i++) { float a = i * 0.785398f; IBX(cx + cosf(a) * s * 0.84f - Tk * 0.45f, cy + sinf(a) * s * 0.84f - Tk * 0.45f, Tk * 0.9f, Tk * 0.9f, col); }
        IFC(cx, cy, s * 0.6f, col); IFC(cx, cy, s * 0.24f, bg);
    } else if (!strcmp(id, "Office")) {
        IRR(cx - s, cy - s * 0.4f, 2 * s, s * 1.3f, s * 0.16f, col);
        IRR(cx - s * 0.45f, cy - s * 0.8f, s * 0.9f, s * 0.5f, s * 0.16f, col); IRR(cx - s * 0.28f, cy - s * 0.62f, s * 0.56f, s * 0.4f, 2, bg);
        IBX(cx - s, cy + s * 0.06f, 2 * s, Tk * 0.65f, bg);
    } else if (!strcmp(id, "device")) {                                         // phone: body, screen, earpiece, home
        IRR(cx - s * 0.58f, cy - s, s * 1.16f, 2 * s, s * 0.22f, col);
        IRR(cx - s * 0.4f, cy - s * 0.68f, s * 0.8f, s * 1.25f, 2, bg);
        IBX(cx - s * 0.18f, cy - s * 0.85f, s * 0.36f, Tk * 0.4f, bg); IFC(cx, cy + s * 0.78f, Tk * 0.5f, bg);
    } else if (!strcmp(id, "poker")) {                                          // playing card with a heart pip
        IRR(cx - s * 0.68f, cy - s, s * 1.36f, 2 * s, s * 0.18f, col);
        IFC(cx - s * 0.2f, cy - s * 0.18f, s * 0.22f, bg); IFC(cx + s * 0.2f, cy - s * 0.18f, s * 0.22f, bg);
        ITR(cx - s * 0.41f, cy - s * 0.06f, cx + s * 0.41f, cy - s * 0.06f, cx, cy + s * 0.45f, bg);
        IFC(cx - s * 0.44f, cy - s * 0.76f, Tk * 0.42f, bg); IFC(cx + s * 0.44f, cy + s * 0.76f, Tk * 0.42f, bg);
    } else if (!strcmp(id, "pinball")) {                                        // portrait table: body, ball, two flippers
        IRR(cx - s * 0.62f, cy - s, s * 1.24f, 2 * s, s * 0.3f, col);
        IRR(cx - s * 0.42f, cy - s * 0.82f, s * 0.84f, s * 1.52f, 2, bg);
        IFC(cx + s * 0.12f, cy - s * 0.34f, Tk * 0.72f, col);                    // ball
        IL(cx - s * 0.3f, cy + s * 0.52f, cx + s * 0.02f, cy + s * 0.28f, Tk * 0.6f, col);   // left flipper
        IL(cx + s * 0.3f, cy + s * 0.52f, cx - s * 0.02f, cy + s * 0.28f, Tk * 0.6f, col);   // right flipper
    } else if (!strcmp(id, "level")) {                                          // spirit level: pill vial, bubble, two gauge ticks
        IRR(cx - s, cy - s * 0.42f, 2 * s, s * 0.84f, s * 0.42f, col);
        IRR(cx - s + Tk * 0.6f, cy - s * 0.42f + Tk * 0.6f, 2 * s - Tk * 1.2f, s * 0.84f - Tk * 1.2f, s * 0.36f, bg);
        IBX(cx - s * 0.34f, cy - s * 0.42f, Tk * 0.5f, s * 0.84f, col);
        IBX(cx + s * 0.34f - Tk * 0.5f, cy - s * 0.42f, Tk * 0.5f, s * 0.84f, col);
        IFC(cx, cy, Tk * 1.15f, col);                                           // the bubble
    } else if (!strcmp(id, "dice")) {                                           // die face: rounded square + 5 pips
        IRR(cx - s, cy - s, 2 * s, 2 * s, s * 0.28f, col);
        IFC(cx - s * 0.45f, cy - s * 0.45f, Tk * 0.6f, bg);
        IFC(cx + s * 0.45f, cy - s * 0.45f, Tk * 0.6f, bg);
        IFC(cx, cy, Tk * 0.6f, bg);
        IFC(cx - s * 0.45f, cy + s * 0.45f, Tk * 0.6f, bg);
        IFC(cx + s * 0.45f, cy + s * 0.45f, Tk * 0.6f, bg);
    } else if (!strcmp(id, "goniometer")) {                                     // protractor: top half-disc + pivot + angle arm
        IFC(cx, cy + s * 0.45f, s, col);                                        // full disc, dropped...
        IBX(cx - s - 1, cy + s * 0.45f, 2 * s + 2, s + 2, bg);                  // ...mask below centre -> half-disc (flat base down)
        IBX(cx - s * 0.5f, cy + s * 0.05f, s, s * 0.42f, bg);                   // inner protractor cut-out
        IL(cx, cy + s * 0.45f, cx + s * 0.66f, cy - s * 0.55f, Tk * 0.55f, col);// angle arm from the pivot
        IFC(cx, cy + s * 0.45f, Tk * 0.7f, col);                               // pivot dot
    } else if (!strcmp(id, "Hardware")) {                                       // DIP microchip: body + side legs + pin-1 dot
        IRR(cx - s * 0.56f, cy - s * 0.72f, s * 1.12f, s * 1.44f, s * 0.12f, col);
        IRR(cx - s * 0.28f, cy - s * 0.44f, s * 0.56f, s * 0.9f, 2, bg);
        for (int i = -1; i <= 1; i++) {
            IBX(cx - s, cy + i * s * 0.42f - Tk * 0.28f, s * 0.44f, Tk * 0.56f, col);
            IBX(cx + s * 0.56f, cy + i * s * 0.42f - Tk * 0.28f, s * 0.44f, Tk * 0.56f, col);
        }
        IFC(cx - s * 0.28f, cy - s * 0.46f, Tk * 0.5f, col);                    // pin-1 notch dot
    } else if (!strcmp(id, "pedometer")) {                                      // footprint: ball + heel + a row of toes
        IFC(cx, cy - s * 0.05f, s * 0.52f, col);                                // ball of the foot
        IFC(cx + s * 0.12f, cy + s * 0.66f, s * 0.32f, col);                    // heel (smaller, offset)
        IFC(cx - s * 0.42f, cy - s * 0.55f, Tk * 0.34f, col);                   // toes...
        IFC(cx - s * 0.15f, cy - s * 0.72f, Tk * 0.38f, col);
        IFC(cx + s * 0.13f, cy - s * 0.74f, Tk * 0.36f, col);
        IFC(cx + s * 0.37f, cy - s * 0.62f, Tk * 0.32f, col);
    } else if (!strcmp(id, "alarm")) {                                          // alarm bell: dome body + base rim + clapper
        IFC(cx, cy - s * 0.6f, Tk * 0.4f, col);                                 // top knob
        ITR(cx, cy - s * 0.5f, cx - s * 0.6f, cy + s * 0.4f, cx + s * 0.6f, cy + s * 0.4f, col);  // bell body
        IFC(cx, cy - s * 0.42f, s * 0.3f, col);                                 // round the shoulders
        IRR(cx - s * 0.66f, cy + s * 0.36f, s * 1.32f, Tk * 0.5f, Tk * 0.2f, col);  // base rim
        IFC(cx, cy + s * 0.66f, Tk * 0.42f, col);                              // clapper
    } else if (!strcmp(id, "screensaver")) {                                    // crescent moon + 3 stars (sleep/idle)
        IFC(cx - s * 0.18f, cy + s * 0.08f, s * 0.76f, col);                   // moon disc
        IFC(cx + s * 0.22f, cy - s * 0.08f, s * 0.56f, bg);                    // bite out → crescent
        IFC(cx + s * 0.72f, cy - s * 0.64f, Tk * 0.65f, col);                  // star top-right
        IFC(cx + s * 0.80f, cy + s * 0.28f, Tk * 0.45f, col);                  // star mid-right
        IFC(cx - s * 0.18f, cy - s * 0.88f, Tk * 0.50f, col);                  // star top
    } else if (!strcmp(id, "pixel-fix")) {                                      // display frame + 2×2 pixel grid (3 lit + 1 stuck)
        IRR(cx - s, cy - s * 0.82f, 2 * s, s * 1.64f, s * 0.2f, col);          // monitor bezel
        IRR(cx - s + Tk, cy - s * 0.82f + Tk, 2 * s - 2 * Tk, s * 1.28f, 2, bg); // screen area
        IBX(cx - Tk * 0.6f, cy + s * 0.48f, Tk * 1.2f, s * 0.34f, col);        // stand stem
        IBX(cx - s * 0.38f, cy + s * 0.78f, s * 0.76f, Tk * 0.55f, col);       // stand base
        float ph = s * 0.52f, pw = s * 0.48f, gp = Tk * 0.55f;
        IBX(cx - pw - gp * 0.5f, cy - ph - gp * 0.5f, pw, ph, col);            // top-left pixel (lit)
        IBX(cx + gp * 0.5f,      cy - ph - gp * 0.5f, pw, ph, col);            // top-right pixel (lit)
        IBX(cx - pw - gp * 0.5f, cy + gp * 0.5f,      pw, ph, col);            // bottom-left pixel (lit)
        // bottom-right stays bg = stuck/dead pixel intentionally dark
    } else {
        g->setTextSize(r >= 12 ? 3 : (r >= 6 ? 2 : 1)); g->setTextColor(col);   // fallback: a bold letter
        int cw = (r >= 12 ? 18 : (r >= 6 ? 12 : 6)), ch = (r >= 12 ? 24 : (r >= 6 ? 16 : 8));
        g->setCursor(cx - cw / 2 + 1, cy - ch / 2); g->print(letter);
        g->setTextSize(1);                                                      // MUST reset: else it leaks into the title (Font4 x3 = huge)
    }
    #undef RI
    #undef CLR
    #undef IBX
    #undef IFC
    #undef ITR
    #undef IRR
    #undef IL
    #undef IEL
}

// Public wrappers so sibling launcher TUs (the Game front-end's procedural poster) can draw the real
// vector icons — to the off-screen canvas (M5Canvas) or straight to the display (M5GFX). The ui_icon
// template instantiates for each target.
void launcher_draw_icon(M5Canvas *c, int cx, int cy, int r, const char *id, char letter,
                        unsigned short col, unsigned short bg)
{
    ui_icon(c, cx, cy, r, id, letter, col, bg);
}
void launcher_draw_icon(M5GFX *c, int cx, int cy, int r, const char *id, char letter,
                        unsigned short col, unsigned short bg)
{
    ui_icon(c, cx, cy, r, id, letter, col, bg);
}

// Red alert label for ANY background radio-offensive op (Evil Portal / Deauth Flood / Beacon Spam).
// All three keep running after you leave their app and suspend the OS network/web — the bar makes that
// unmissable. Returns the label (into buf) or NULL when nothing offensive is armed.
static const char *offensive_alert(char *buf, int cap)
{
    if (nucleo_evilportal_running())     { snprintf(buf, cap, "EVIL PORTAL  %d catt.", nucleo_evilportal_captures()); return buf; }
    if (nucleo_wifiatk_deauth_running()) { snprintf(buf, cap, "DEAUTH FLOOD  %lu fr", nucleo_wifiatk_frames());        return buf; }
    if (nucleo_wifiatk_beacon_running()) { snprintf(buf, cap, "BEACON SPAM  %d SSID", nucleo_wifiatk_beacon_count()); return buf; }
    return NULL;
}

void launcher_render_status_bar(void)
{
    // While any radio-offensive op runs in the background the whole top bar becomes a red alert — an
    // unmissable reminder that the device is attacking (and that the OS network/web UI are suspended).
    // Shown at any menu depth.
    char ab[28]; const char *alert = offensive_alert(ab, sizeof ab);
    if (alert) {
        d.fillRect(0, 0, W, BAR, C_RED);
        d.fillCircle(9, BAR / 2, 3, INK);
        d.setTextSize(1); d.setTextColor(INK, C_RED); d.setCursor(18, 4); d.print(alert);
        d.drawFastHLine(0, BAR - 1, W, LINE);
        return;
    }
    d.fillRect(0, 0, W, BAR, INK);
    d.setTextSize(1);
    bool sta = !strcmp(nucleo_setup_mode(), "sta") && nucleo_setup_ssid()[0];

    const MenuNode *node = launcher_node();
    if (launcher_depth() > 0) {
        // Breadcrumb: a colour chip carrying the category icon + the name in the big Font2 face
        // (legible), with the Wi-Fi gauge and the item count packed from the right edge inward.
        d.fillRoundRect(2, 1, 14, 14, 3, node->color);
        ui_icon(&d, 9, 8, 6, node->id, node->icon, INK, node->color);
        d.setFont(&fonts::Font2); d.setTextColor(FG, INK); d.setCursor(20, 0);
        char b[16]; snprintf(b, sizeof(b), "%.12s", node->label); d.print(b);
        d.setFont(&fonts::Font0); d.setTextSize(1);
        int rx = W - 2;
        draw_wifi(rx - 19, 4, sta, nucleo_setup_rssi());   rx -= 19 + 6;   // signal far right
        int cnt = node_child_count(node);
        if (cnt > 0) {
            char cc[12]; snprintf(cc, sizeof cc, "%d app", cnt);
            d.setTextColor(MUTED, INK); d.setCursor(rx - (int)strlen(cc) * 6, 4); d.print(cc);   // left of the gauges
        }
    } else {
        // Home (smartwatch face): a bold clock anchors the LEFT in the 16px-tall Font2 face; the
        // Wi-Fi gauge, the date (day + month) and the network name form ONE right-aligned cluster,
        // packed from the right edge inward. The date has PRIORITY over the SSID, so day/month is
        // shown whenever it fits. Drawn only when no filter chip claims the right.
        time_t now = time(NULL); struct tm *tm = localtime(&now);
        char t[8]; snprintf(t, sizeof(t), "%02d:%02d", tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0);
        d.setFont(&fonts::Font2); d.setTextColor(FG, INK); d.setCursor(6, 0); d.print(t);  // big white clock
        int clock_r = 6 + (int)d.textWidth(t);
        d.setFont(&fonts::Font0); d.setTextSize(1);

        if (!launcher_filter()[0]) {
            // Right cluster from the edge: [|||] Wi-Fi, then [day month], then [ssid] if it still
            // fits. Each piece is dropped if it would crowd the clock, so nothing overlaps the time.
            int rx = W - 2;
            draw_wifi(rx - 19, 4, sta, nucleo_setup_rssi());   rx -= 19 + 6;   // antenna gauge, far right
            static const char *const WD[7]  = { "dom","lun","mar","mer","gio","ven","sab" };
            static const char *const MO[12] = { "gen","feb","mar","apr","mag","giu","lug","ago","set","ott","nov","dic" };
            char dt[16] = "";
            if (tm && now > 1672531200) snprintf(dt, sizeof dt, "%s %d %s", WD[tm->tm_wday], tm->tm_mday, MO[tm->tm_mon]);
            int dw = (int)strlen(dt) * 6;
            if (dt[0] && rx - dw > clock_r + 6) {
                d.setTextColor(C_YELLOW, INK); d.setCursor(rx - dw, 4); d.print(dt);
                rx -= dw + 8;
            }
            char net[12]; snprintf(net, sizeof net, "%.10s", sta ? nucleo_setup_ssid() : "Setup AP");
            int nw = (int)strlen(net) * 6;
            if (rx - nw > clock_r + 8) {
                d.setTextColor(sta ? MUTED : C_YELLOW, INK); d.setCursor(rx - nw, 4); d.print(net);
            }
        }
    }

    if (launcher_filter()[0]) {
        // Filter chip, right-aligned so it never collides with the title/clock.
        char fb[20]; snprintf(fb, sizeof fb, "/%.10s", launcher_filter());
        int fw = (int)strlen(fb) * 6;
        d.fillRect(W - fw - 4, 0, fw + 4, BAR - 1, 0x1926);
        d.setTextColor(C_GREEN, 0x1926); d.setCursor(W - fw - 1, 4); d.print(fb);
    }

    d.drawFastHLine(0, BAR - 1, W, LINE);
}

// Once-a-second clock refresh WITHOUT wiping the bar. The full chrome repaint did a fillRect
// over all three bars every second, flashing them black; here we overwrite only the HH:MM
// digits in place (opaque background) so the top bar stays steady. At menu depth the bar shows
// a breadcrumb instead of a clock, so there is nothing to tick.
void launcher_render_clock_tick(void)
{
    if (launcher_depth() > 0) return;
    char ab[28]; const char *alert = offensive_alert(ab, sizeof ab);
    if (alert) {                                   // alert bar owns the strip: refresh the count in place
        d.fillRect(18, 0, W - 18, BAR - 1, C_RED);
        d.setTextSize(1); d.setTextColor(INK, C_RED); d.setCursor(18, 4); d.print(alert);
        return;
    }
    char t[8]; time_t now = time(NULL); struct tm *tm = localtime(&now);
    snprintf(t, sizeof(t), "%02d:%02d", tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0);
    d.fillRect(0, 0, 56, BAR - 1, INK);                       // wipe the clock cell (Font2 is wider than the old 6x8)
    d.setFont(&fonts::Font2); d.setTextColor(FG, INK); d.setCursor(6, 0); d.print(t);
    d.setFont(&fonts::Font0); d.setTextSize(1);               // leave the global font at the framework default
    // The date (day + month) lives in the right cluster, repainted by the status bar on navigation;
    // it changes once a day, so the 1 Hz tick only needs to refresh the HH:MM digits on the left.
}

// Draw one footer line, centered inside its dark band and clipped to the band width so it
// can never bleed below its strip or run off the right edge. The classic font cell is 8 px
// tall (descenders included), so the vertical inset that keeps equal margins top and bottom
// is (band_h - 8) / 2. Horizontally we copy only as many 6-px glyphs as fit before the right
// margin, so a long string is hard-truncated instead of spilling past the screen.
static void footer_line(int y, int band_h, unsigned short bg, unsigned short fg,
                        int x, const char *text)
{
    int maxch = (W - x) / 6;                // 6 px per glyph; last column lands inside the screen
    if (maxch < 0) maxch = 0;
    if (maxch > 47) maxch = 47;
    char b[48];
    snprintf(b, sizeof b, "%.*s", maxch, text ? text : "");
    int ty = y + (band_h - 8) / 2;          // 8 px font cell -> equal top/bottom margin
    d.setTextSize(1); d.setTextColor(fg, bg); d.setCursor(x, ty); d.print(b);
}

// Retired: the focused-row description now lives inside the hero card (draw_list), reclaiming this
// strip for the list band. Kept as a no-op so the header symbol and call sites stay valid.
void launcher_render_instr_bar(void) { }

void launcher_render_hint_bar(void)
{
    int y = H - HINT;
    d.fillRect(0, y, W, HINT, s_hint_bg); d.drawFastHLine(0, y, W, LINE);
    int len = (int)strlen(s_hint); if (len > 39) len = 39;
    int x = (W - len * 6) / 2; if (x < 4) x = 4;                 // centred hint reads cleaner
    footer_line(y, HINT, s_hint_bg, s_hint_fg, x, s_hint);
}

void launcher_render_chrome(void) { launcher_render_status_bar(); launcher_render_hint_bar(); }

// ---- horizontal icon carousel (smartwatch app-drawer) -----------------------
// Three icons in a row: the centred one is the big, bright SELECTED app/category; its neighbours peek
// in smaller and dimmer on each side, so the focus reads at a glance. The title sits below the icon in
// Font2; a secondary line gives the description (apps) or the item count (categories); a dot rail marks
// the position. The whole rail slides horizontally (s_carousel eased toward the focus). Zero heap: the
// band still composites into the ONE shared back-buffer and blits once. Mirrors web/device/device.js.
static const int CAR_SLOT    = 84;     // horizontal spacing between adjacent icons (left/right peek)
static const int CAR_R_C     = 32;     // centred badge radius (fills the band top-down)
static const int CAR_R_S     = 19;     // side badge radius (neighbours stay prominent)
static const int CAR_ICON_DY = 34;     // icon-row centre, pushed up to use the band top

static float s_carousel     = 0.0f;    // continuous carousel index, eased toward launcher_sel()
static bool  s_band_buffered = true;   // set by launcher_render_list: was the last band blit buffered?

// Linear blend of two RGB565 colours (t: 0 -> a, 1 -> b). Used to dim the side icons toward the
// background and to mute the focus halo, the way the web sim does with rgb() interpolation.
static uint16_t mix565(uint16_t a, uint16_t b, float t)
{
    int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
    int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
    int r  = ar + (int)((br - ar) * t + 0.5f);
    int g  = ag + (int)((bg - ag) * t + 0.5f);
    int bl = ab + (int)((bb - ab) * t + 0.5f);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

// Copy the widest prefix of `s` that fits `maxw` px under the CURRENTLY selected font into `out`
// (hard truncation, no ellipsis — names rarely overflow at this width). Mirrors the web fit().
template <typename T> static void fit_width(T *g, const char *s, int maxw, char *out, int outcap)
{
    int n = (int)strlen(s); if (n > outcap - 1) n = outcap - 1;
    for (; n > 0; n--) { memcpy(out, s, n); out[n] = 0; if ((int)g->textWidth(out) <= maxw) return; }
    out[0] = 0;
}

// Signed distance from the eased focus `pos` to item `i`. On a wrapping rail (>=3 items) it returns
// the NEAREST copy, so the last item peeks to the left of the first and the rail is never visually
// empty on one side — the "always three icons" idiom.
static float car_cdist(int i, float pos, int n, bool wrap)
{
    float d = (float)i - pos;
    if (wrap) d -= (float)n * roundf(d / (float)n);
    return d;
}

// Ease the carousel toward the focused index. Returns true while still moving (the run loop keeps
// requesting redraws), false once settled (so an idle rail stops repainting — anti-flicker #4). For a
// wrapping rail it eases along the SHORTEST way round, so stepping past either end glides one slot.
bool launcher_render_step_scroll(void)
{
    int n = launcher_visible_count();
    if (n <= 0) { s_carousel = 0.0f; return false; }
    float target = (float)launcher_sel();
    // Snap (no glide) whenever the list itself changes under us — a new menu level or a filter that
    // resized it — so we never sweep across a brand-new set of items.
    static const void *s_last_node = nullptr; static int s_last_n = -1;
    const void *node = (const void *)launcher_node();
    if (node != s_last_node || n != s_last_n) {
        s_last_node = node; s_last_n = n;
        s_carousel = (target > (float)(n - 1)) ? (float)(n - 1) : target;
        return false;
    }
    // No off-screen canvas (heap fragmented after a media app): the band draws DIRECT, so an eased
    // slide would be N per-frame clear-then-draws = flicker. SNAP to target — one redraw, no anim.
    if (!s_band_buffered) { s_carousel = (target > (float)(n - 1)) ? (float)(n - 1) : target; return false; }
    if (n >= 3) {                                                            // circular: shortest way round
        float d = target - s_carousel; d -= (float)n * roundf(d / (float)n);
        if (d < 0.02f && d > -0.02f) { s_carousel = target; return false; }
        s_carousel += d * 0.30f;
        s_carousel = fmodf(s_carousel, (float)n); if (s_carousel < 0.0f) s_carousel += (float)n;
        return true;
    }
    if (target > (float)(n - 1)) target = (float)(n - 1);                    // 1-2 items: plain ease
    float d = target - s_carousel;
    if (d < 0.05f && d > -0.05f) { s_carousel = target; return false; }
    s_carousel += d * 0.30f;
    return true;
}

// Count "rosette": a dark pill with an accent rim + white number, straddling the icon's top-right
// corner (notification-badge idiom). Replaces the "N app" subtitle row for categories, reclaiming
// the band for a bigger icon + title.
template <typename T> static void draw_badge(T *c, int cx, int cy, int count, uint16_t accent)
{
    char s[8]; snprintf(s, sizeof s, "%d", count);
    c->setFont(&fonts::Font2); c->setTextSize(1);
    int tw = (int)c->textWidth(s), h = 17, w = tw + 11; if (w < 17) w = 17;
    c->fillRoundRect(cx - w / 2,     cy - h / 2,     w,     h,     h / 2,       accent);   // accent rim
    c->fillRoundRect(cx - w / 2 + 2, cy - h / 2 + 2, w - 4, h - 4, (h - 4) / 2, INK);      // dark fill
    c->setTextColor(FG, INK);
    c->setCursor(cx - tw / 2, cy - 8); c->print(s);                                        // Font2 ~16px, centred
}

// Dot rail / position indicator, centred at (cx, y). Collapses to "k/n" past 13 items.
template <typename T> static void draw_dots(T *c, int n, int sel, int cx, int y, uint16_t accent)
{
    if (n <= 1) return;
    if (n > 13) {
        char b[12]; snprintf(b, sizeof b, "%d/%d", sel + 1, n);
        c->setFont(&fonts::Font0); c->setTextSize(1); c->setTextColor(MUTED, BG);
        c->setCursor(cx - (int)c->textWidth(b) / 2, y - 4); c->print(b);
        return;
    }
    int gap = 10, x0 = cx - (n - 1) * gap / 2;
    for (int i = 0; i < n; i++) {
        int dx = x0 + i * gap;
        if (i == sel) c->fillRoundRect(dx - 3, y - 1, 7, 3, 1, accent);   // active = capsule "you are here"
        else          c->fillCircle(dx, y, 1, DIM);                        // inactive = dot
    }
}

// Now-playing complication: a tiny equalizer (dark pill + 3 green bars) in the icon's bottom-right
// corner, shown on the audio source's icon while it plays. Animates while the band repaints (i.e.
// while you scroll), freezes when idle — so it costs nothing extra when you're not interacting.
template <typename T> static void draw_eq(T *c, int ex, int ey, int w, unsigned frame)
{
    int h = (int)(w * 0.72f); if (h < 6) h = 6;
    c->fillRoundRect(ex - w / 2, ey - h / 2, w, h, h / 3, INK);
    int base = ey + h / 2 - 2, step = (w - 4) / 3;
    for (int i = 0; i < 3; i++) {
        int bh = (int)(h * (0.3f + 0.5f * fabsf(sinf(frame * 0.4f + i * 1.4f))) + 0.5f);
        c->fillRect(ex - w / 2 + 3 + i * step, base - bh, 2, bh, C_GREEN);
    }
}

// ---- the launcher carousel --------------------------------------------------
// Rendered into a band-local coordinate space (top = `base`): for the off-screen canvas base = 0 and
// the result is pushed at y = LIST_TOP; for the direct fallback base = LIST_TOP. The focused item is a
// big centred badge; neighbours peek smaller/dimmer; the rail slides via s_carousel (eased in
// launcher_render_step_scroll).
template <typename T> static void draw_list(T *c, int base)
{
    if (nucleo_theme_has_bg_image()) nucleo_theme_draw_bg_slice(c, 0, base, W, LIST_BAND_H);
    else                            c->fillRect(0, base, W, LIST_BAND_H, BG);

    int n = launcher_visible_count();
    if (n == 0) {
        c->setFont(&fonts::Font2); c->setTextColor(DIM, BG);
        c->setCursor(54, base + LIST_BAND_H / 2 - 8); c->print("Nessuna app");
        c->setFont(&fonts::Font0); c->setTextSize(1);
        return;
    }
    int   sel    = launcher_sel();
    bool  wrap   = (n >= 3);                                            // 3+ items -> infinite/wrapping rail
    float pos    = s_carousel;
    if (!wrap) { if (pos < 0.0f) pos = 0.0f; if (pos > (float)(n - 1)) pos = (float)(n - 1); }
    int   iconCY = base + CAR_ICON_DY;

    c->setClipRect(0, base, W, LIST_BAND_H);

    // Now-playing state (for the equalizer complication): which audio source, if any, is live.
    static unsigned s_eq_frame = 0; s_eq_frame++;
    bool audio_on = nucleo_audio_is_playing() && !nucleo_audio_is_paused();
    const char *ap = audio_on ? nucleo_audio_path() : "";
    bool is_stream = ap && strstr(ap, "://");

    // Collect the (up to ~4) on-screen slots, then draw far-to-near so the centre lands on top even
    // mid-slide. `n` can be large but only a handful are ever within 1.8 slots of the focus.
    int idx[6], cnt = 0;
    for (int i = 0; i < n && cnt < 6; i++) { float d = car_cdist(i, pos, n, wrap); if (d < 1.8f && d > -1.8f) idx[cnt++] = i; }
    for (int a = 0; a < cnt; a++)
        for (int b = a + 1; b < cnt; b++) {
            float da = car_cdist(idx[a], pos, n, wrap), db = car_cdist(idx[b], pos, n, wrap);
            if (da < 0) da = -da;
            if (db < 0) db = -db;
            if (db > da) { int tmp = idx[a]; idx[a] = idx[b]; idx[b] = tmp; }
        }
    for (int k = 0; k < cnt; k++) {
        const MenuNode *it = launcher_nth_visible(idx[k]);
        if (!it) continue;
        float d = car_cdist(idx[k], pos, n, wrap), ad = d < 0 ? -d : d;
        float t = 1.0f - ad; if (t < 0) t = 0;                          // 1 centred, 0 a full slot away
        int x  = (int)(W / 2 + d * CAR_SLOT + 0.5f);
        int r  = (int)(CAR_R_S + (CAR_R_C - CAR_R_S) * t + 0.5f);
        int cy = iconCY + (int)((1.0f - t) * 6.0f + 0.5f);              // neighbours drop a touch (arc/depth)
        uint16_t badge = mix565(LINE, it->color, 0.40f + 0.60f * t);    // badge fill (dim neighbour -> bright focus)
        int cr = (int)(r * 0.34f + 0.5f);                               // rounded-square corner radius
        if (t > 0.6f) c->fillRoundRect(x - r - 4, cy - r - 4, 2 * r + 8, 2 * r + 8, cr + 3, mix565(BG, it->color, 0.50f));  // soft halo
        c->fillRoundRect(x - r, cy - r, 2 * r, 2 * r, cr, badge);       // square icon badge (bigger than a disc)
        if (t > 0.85f) {                                               // crisp accent ring -> stronger focus pop
            uint16_t rc = mix565(it->color, FG, 0.40f);
            c->drawRoundRect(x - r - 2, cy - r - 2, 2 * r + 4, 2 * r + 4, cr + 2, rc);
            c->drawRoundRect(x - r - 3, cy - r - 3, 2 * r + 6, 2 * r + 6, cr + 2, rc);
        }
        int      gr   = (int)(r * 0.68f + 0.5f);                        // glyph half-size fills the square badge
        uint16_t gcol = (t > 0.5f) ? INK : mix565(BG, FG, 0.62f);       // dark glyph on the bright focus
        ui_icon(c, x, cy, gr, it->id, it->icon, gcol, badge);
        if (it->kind == N_MENU && t > 0.85f)                            // category: count rosette on the top-right corner
            draw_badge(c, x + (int)(r * 0.82f), cy - (int)(r * 0.82f), node_child_count(it), it->color);
        if (audio_on && ((is_stream && !strcmp(it->id, "radio")) || (!is_stream && !strcmp(it->id, "music"))))
            draw_eq(c, x + (int)(r * 0.6f), cy + (int)(r * 0.6f), (int)(r * 0.55f), s_eq_frame);   // now playing
    }
    c->clearClipRect();

    // Big title for the centred item, below the icon (the count rides the rosette, not a text row).
    const MenuNode *cur = launcher_nth_visible(sel);
    if (cur) {
        char buf[28];
        c->setTextSize(1); c->setTextColor(FG, BG);                     // size 1 always (defend against a leak)
        // Biggest font that still fits: short names stay bold Font4; long ones (Impostazioni, Connection)
        // drop to Font2 so they read at a sane size instead of filling the whole band.
        c->setFont(&fonts::Font4); int fh = 26;
        if ((int)c->textWidth(cur->label) > W - 20) { c->setFont(&fonts::Font2); fh = 16; }
        fit_width(c, cur->label, W - 12, buf, sizeof buf);
        c->setCursor(W / 2 - (int)c->textWidth(buf) / 2, base + 84 - fh / 2); c->print(buf);   // centre at base+84
        draw_dots(c, n, sel, W / 2, base + 102, cur->color);
    }
    c->setFont(&fonts::Font0); c->setTextSize(1);                       // leave the font at the framework default
}

// The list band composites into the shared back-buffer (see nucleo_ui.cpp). We draw the band
// into the top of that canvas and blit only the band region with a destination-clipped push,
// so the chrome below it is untouched. Decoder apps call nucleo_screen_release() directly to
// hand the canvas RAM to the codec; it re-acquires lazily here.
void launcher_render_list(void)
{
    M5Canvas *c = nucleo_screen();
    s_band_buffered = (c != nullptr);                          // tells step_scroll to snap (not animate) on the direct path
    if (c) {
        draw_list(c, 0);                                        // band-local: rows 0..LIST_BAND_H of the canvas
        d.setClipRect(0, LIST_TOP, W, LIST_BAND_H);            // land it in the band region only...
        c->pushSprite(0, LIST_TOP);                            // ...one blit -> no flicker (rows below are clipped)
        d.clearClipRect();
    } else {
        // No canvas (heap fragmented after a radio session): draw direct, batched into ONE SPI
        // transaction so the clear→rows repaint is as quick as possible. Paired with the snap-scroll
        // above (no per-frame animation) this keeps the menu clean instead of a flickery mess.
        d.startWrite();
        draw_list(&d, LIST_TOP);
        d.endWrite();
    }
}

// ---- Control Center overlay (tabbed quick settings) -------------------------
// A full-screen sheet raised with TAB from anywhere, with navigation IDENTICAL to the Music /
// Video settings sheets (app_player.cpp / app_video.cpp):
//   · a persistent segmented tab bar; the header (s_cc_row == -1) is the tab cursor;
//   · RIGHT / LEFT page to the next / previous tab from ANYWHERE (header or a row), never close;
//   · DOWN from the header drops into the rows; UP from row 0 returns to the header;
//   · UP/DOWN walk the rows; ENTER acts (toggle / cycle / play-pause, or arms a slider);
//   · a slider arms an EDIT mode on ENTER — then UP/RIGHT raise, DOWN/LEFT lower, ENTER/Esc end;
//   · Esc/Back pops one level hierarchically: edit -> row -> header -> close the sheet (Tab also closes).
// The look mirrors the Music/Video sheet too: same palette, segmented tabs, accent-railed
// carousel rows, slider knobs, toggle pills and value chips. The RETE tab renders the IP and
// pairing PIN at size-2 so they read across the room.
//
// Palette mirrors app_player.cpp / app_video.cpp so the sheet matches the Music/Video look.
static const unsigned short CC_BG   = 0x0841;  // void-blue sheet background
static const unsigned short CC_SURF = 0x10A2;  // raised surface / slider track
static const unsigned short CC_CAP  = 0x1A8B;  // focused capsule background
static const unsigned short CC_ACC  = 0x4DDF;  // bright-blue accent (tab + rail)
static const unsigned short CC_GRN  = 0x8FF3;  // positive / playing

// launcher_render_control_center_key return codes (so the launcher can pop/close hierarchically).
enum { CC_NONE = 0, CC_REDRAW = 1, CC_CLOSE = 2, CC_SCREEN_OFF = 3, CC_LAUNCH = 4 };
static const char *s_cc_launch_id = nullptr;  // set before returning CC_LAUNCH

enum { CC_TEXT = 0, CC_NOTE, CC_SLIDER, CC_TOGGLE, CC_CYCLE, CC_ACTION, CC_BIG, CC_BATTERY, CC_SIGNAL };   // row kinds
// Action ids — the durable identity of each interactive row, so the key handler dispatches on
// MEANING (not on a fragile tab+row coordinate that shifts when a dynamic row appears).
enum { A_NONE = 0, A_BRIGHT, A_VOLUME, A_MUTE, A_SCREEN,
       A_LANG, A_THEME, A_USB, A_REBOOT, A_USBKBD, A_REMOTE };

static const int CC_NTABS = 3;
// Tab names, localized.
static const char *cc_tab_name(int i)
{
    switch (i) {
        case 0:  return TR("RAPIDE", "QUICK");
        case 1:  return TR("RETE", "NETWORK");
        default: return TR("SISTEMA", "SYSTEM");
    }
}

// Theme cycling (SISTEMA tab): the OS theme registry is the single source of truth.
static const char *cc_theme_name(void)
{
    int cnt = 0; const nucleo_theme_t *all = nucleo_theme_get_all(&cnt);
    const char *cur = nucleo_theme_get_current();
    for (int i = 0; i < cnt; i++) if (cur && all[i].id && !strcmp(all[i].id, cur)) return all[i].name;
    return cur ? cur : "?";
}
static void cc_theme_cycle(int dir)
{
    int cnt = 0; const nucleo_theme_t *all = nucleo_theme_get_all(&cnt);
    if (!all || cnt <= 0) return;
    const char *cur = nucleo_theme_get_current();
    int idx = 0; for (int i = 0; i < cnt; i++) if (cur && all[i].id && !strcmp(all[i].id, cur)) { idx = i; break; }
    idx = (idx + dir + cnt) % cnt;
    nucleo_theme_set(all[idx].id);
}

static int  s_cc_tab     = 0;          // 0=RAPIDE 1=RETE 2=SISTEMA
static int  s_cc_row     = -1;         // -1 = tab header, 0..n-1 = row
static bool s_cc_edit    = false;      // a slider is in adjust mode (UP/DN/L/R change it)
static int  s_cc_confirm = A_NONE;     // a destructive action awaiting a second ENTER (USB/Riavvia)

struct CcItem { const char *label; char val[32]; int kind; int act; int slider; bool on; unsigned short col; };

// Build the rows of the active tab into `it` (max 7). Returns the row count.
static int cc_build(CcItem *it)
{
    memset(it, 0, sizeof(CcItem) * 7);
    bool sta = !strcmp(nucleo_setup_mode(), "sta") && nucleo_setup_ip()[0];
    if (s_cc_tab == 0) {                                       // RAPIDE — luce, audio, stato
        int n = 0;
        it[n].label = TR("Web Client", "Web Client");    it[n].kind = CC_ACTION; it[n].act = A_REMOTE;  it[n].col = CC_ACC; n++;
        it[n].label = TR("Luminosita", "Brightness"); it[n].kind = CC_SLIDER; it[n].act = A_BRIGHT; it[n].slider = nucleo_app_brightness(); it[n].col = C_YELLOW; n++;
        it[n].label = TR("Volume", "Volume");         it[n].kind = CC_SLIDER; it[n].act = A_VOLUME; it[n].slider = nucleo_audio_volume();   it[n].col = CC_GRN;   n++;
        it[n].label = TR("Muto", "Mute");             it[n].kind = CC_TOGGLE; it[n].act = A_MUTE;   it[n].on = nucleo_audio_is_muted();     it[n].col = CC_GRN;   n++;
        it[n].label = TR("Schermo off", "Screen off"); it[n].kind = CC_ACTION; it[n].act = A_SCREEN; it[n].col = C_BLUE; n++;
        it[n].label = TR("Tastiera USB", "USB Keyboard"); it[n].kind = CC_ACTION; it[n].act = A_USBKBD; it[n].col = CC_ACC; n++;
        int bpct = nucleo_power_battery_pct();
        it[n].label = TR("Batteria", "Battery"); it[n].kind = CC_BATTERY; it[n].slider = bpct;
        it[n].col = (bpct >= 0 && bpct < 20) ? C_RED : (bpct < 50 ? C_YELLOW : CC_GRN);
        n++;
        return n;
    }
    if (s_cc_tab == 1) {                                       // RETE — IP, PIN, WiFi, segnale
        it[0].label = "IP"; it[0].kind = CC_BIG; it[0].col = CC_GRN;
        snprintf(it[0].val, sizeof it[0].val, "%s", sta ? nucleo_setup_ip() : "192.168.4.1");
        it[1].label = "PIN"; it[1].kind = CC_BIG; it[1].col = C_YELLOW;
        snprintf(it[1].val, sizeof it[1].val, "%s", nucleo_auth_pin());
        it[2].label = TR("WiFi", "WiFi"); it[2].kind = CC_BIG; it[2].col = FG;
        snprintf(it[2].val, sizeof it[2].val, "%.16s", sta ? nucleo_setup_ssid() : "Setup AP");
        it[3].label = TR("Segnale", "Signal"); it[3].kind = CC_SIGNAL;
        it[3].slider = sta ? nucleo_setup_rssi() : 0;
        it[3].col = MUTED;
        return 4;
    }
    // SISTEMA — lingua, tema, USB, riavvio, stato
    int n = 0;
    it[n].label = TR("Lingua", "Language"); it[n].kind = CC_CYCLE; it[n].act = A_LANG; it[n].col = C_BLUE;
    snprintf(it[n].val, sizeof it[n].val, "%s", nucleo_i18n_is_en() ? "English" : "Italiano"); n++;
    it[n].label = TR("Tema", "Theme"); it[n].kind = CC_CYCLE; it[n].act = A_THEME; it[n].col = C_BLUE;
    snprintf(it[n].val, sizeof it[n].val, "%.10s", cc_theme_name()); n++;
    it[n].label = TR("USB Drive", "USB Drive"); it[n].kind = CC_ACTION; it[n].act = A_USB;    it[n].col = C_BLUE; n++;
    it[n].label = TR("Riavvia", "Restart");     it[n].kind = CC_ACTION; it[n].act = A_REBOOT; it[n].col = C_RED;  n++;
    // Compact info strip — RAM · hostname · time (one row instead of three).
    char tt[8]; time_t now = time(NULL); struct tm *tm = localtime(&now);
    snprintf(tt, sizeof tt, "%02d:%02d%s", tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0, nucleo_setup_time_synced() ? "" : "?");
    it[n].label = ""; it[n].kind = CC_NOTE; it[n].col = FG;
    snprintf(it[n].val, sizeof it[n].val, "%uKB  %.8s  %s",
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024),
        nucleo_setup_device_name(), tt);
    n++;
    return n;
}

void launcher_render_control_center_open(void) { s_cc_tab = 0; s_cc_row = -1; s_cc_edit = false; s_cc_confirm = A_NONE; }
int  launcher_render_control_center_tab(void)  { return s_cc_tab; }

// Adjust the focused slider by delta, dispatching on the row's action id (robust to layout).
static void cc_slider_adjust(int delta)
{
    CcItem it[7]; int n = cc_build(it);
    if (s_cc_row < 0 || s_cc_row >= n) return;
    if (it[s_cc_row].act == A_VOLUME) nucleo_audio_set_volume(nucleo_audio_volume() + delta);
    else                              nucleo_app_set_brightness(nucleo_app_brightness() + delta);
}

int launcher_render_control_center_key(int key, char ch)
{
    (void)ch;
    CcItem it[7]; int n = cc_build(it);
    if (s_cc_row >= n) s_cc_row = n - 1;

    // --- slider adjust mode (identical to Music/Video edit) ---
    if (s_cc_edit) {
        if      (key == NK_RIGHT || key == NK_UP)   cc_slider_adjust(+5);
        else if (key == NK_LEFT  || key == NK_DOWN) cc_slider_adjust(-5);
        else if (key == NK_ENTER || key == NK_BACK) s_cc_edit = false;
        return CC_REDRAW;
    }

    // Any non-ENTER keystroke abandons a pending destructive confirmation.
    if (key != NK_ENTER) s_cc_confirm = A_NONE;

    // --- RIGHT / LEFT page the tabs from anywhere (header or a row); they never close ---
    if (key == NK_RIGHT) {
        s_cc_tab = (s_cc_tab + 1) % CC_NTABS;
        if (s_cc_row >= 0) s_cc_row = 0;
        return CC_REDRAW;
    }
    if (key == NK_LEFT) {
        s_cc_tab = (s_cc_tab + CC_NTABS - 1) % CC_NTABS;
        if (s_cc_row >= 0) s_cc_row = 0;
        return CC_REDRAW;
    }

    // --- Esc/Back: pop one level (row -> header -> close the sheet) ---
    if (key == NK_BACK) {
        if (s_cc_row >= 0) { s_cc_row = -1; return CC_REDRAW; }
        return CC_CLOSE;
    }

    // --- header (tab cursor) ---
    if (s_cc_row == -1) {
        if (key == NK_DOWN) { s_cc_row = 0; return CC_REDRAW; }
        return CC_NONE;
    }

    // --- rows ---
    if (key == NK_UP)   { s_cc_row = (s_cc_row > 0) ? s_cc_row - 1 : -1; return CC_REDRAW; }  // row 0 -> header
    if (key == NK_DOWN) { if (s_cc_row < n - 1) s_cc_row++; return CC_REDRAW; }
    if (key == NK_ENTER) {
        int act = it[s_cc_row].act;
        if (it[s_cc_row].kind == CC_SLIDER) { s_cc_edit = true; return CC_REDRAW; }
        switch (act) {
            case A_MUTE:   nucleo_audio_set_mute(!nucleo_audio_is_muted()); return CC_REDRAW;
            case A_SCREEN:  return CC_SCREEN_OFF;
            case A_USBKBD:  s_cc_launch_id = "usbkbd"; return CC_LAUNCH;
            case A_REMOTE:  s_cc_launch_id = "remote";  return CC_LAUNCH;
            case A_THEME:  cc_theme_cycle(+1);                            return CC_REDRAW;
            case A_LANG:  nucleo_i18n_set_en(!nucleo_i18n_is_en());       return CC_REDRAW;
            case A_USB:   case A_REBOOT:
                if (s_cc_confirm == act) { if (act == A_USB) nucleo_usbmsc_request(); else esp_restart(); }
                else s_cc_confirm = act;
                return CC_REDRAW;
            default: return CC_NONE;
        }
    }
    return CC_NONE;
}

const char *launcher_render_control_center_launch_id(void) { return s_cc_launch_id; }

// Segmented tab bar — full-height pill for active tab, every pixel used.
template <typename T> static void cc_tabbar(T *g, int active, bool hdr)
{
    const int TH = 26;                                  // tab bar total height
    const int seg = W / CC_NTABS;
    for (int i = 0; i < CC_NTABS; i++) {
        const char *nm = cc_tab_name(i);
        int x = i * seg, tw = (int)strlen(nm) * 6;
        int ty = (TH - 8) / 2;                         // vertically centred text (size-1 = 8px)
        if (i == active) {
            unsigned short bg = hdr ? CC_ACC : CC_SURF;
            unsigned short fg = hdr ? INK : FG;
            g->fillRoundRect(x + 1, 1, seg - 2, TH - 2, 7, bg);
            g->setTextSize(1); g->setTextColor(fg, bg);
            g->setCursor(x + (seg - tw) / 2, ty); g->print(nm);
        } else {
            g->setTextSize(1); g->setTextColor(hdr ? MUTED : DIM, CC_BG);
            g->setCursor(x + (seg - tw) / 2, ty); g->print(nm);
        }
    }
    g->drawFastHLine(0, TH, W, LINE);
}

// One settings row, styled exactly like the Music/Video sheet (app_video.cpp draw_set_row):
// rounded capsule + accent rail when focused, label at size-2, then a kind-specific control.
template <typename T> static void cc_set_row(T *g, int y, bool focus, const CcItem *it, bool edit)
{
    int h = focus ? 50 : 32;
    unsigned short rbg = focus ? CC_CAP : CC_BG;
    g->fillRoundRect(4, y, 232, h - 2, 9, rbg);
    if (focus) g->fillRoundRect(4, y + 3, 5, h - 8, 2, CC_ACC);   // accent rail

    if (it->kind == CC_BIG) {
        // Small label, then the value at size-2 below it — readability win for IP/PIN.
        g->setTextSize(1); g->setTextColor(focus ? FG : MUTED, rbg);
        g->setCursor(16, y + (focus ? 8 : 5)); g->print(it->label);
        g->setTextSize(2); g->setTextColor(it->col, rbg);
        g->setCursor(16, y + (focus ? 24 : 15)); g->print(it->val);
        return;
    }

    if (it->kind == CC_NOTE) {
        // Compact single line, all size-1: small grey label + small value (e.g. the SSID),
        // deliberately less prominent than the big IP/PIN rows below it.
        g->setTextSize(1);
        g->setTextColor(MUTED, rbg);
        g->setCursor(16, y + (h - 8) / 2); g->print(it->label);
        g->setTextColor(focus ? it->col : MUTED, rbg);
        g->setCursor(16 + ((int)strlen(it->label) + 1) * 6, y + (h - 8) / 2); g->print(it->val);
        return;
    }

    g->setTextSize(2); g->setTextColor(focus ? FG : MUTED, rbg);
    g->setCursor(16, y + (h - 16) / 2 - 1); g->print(it->label);

    if (it->kind == CC_BATTERY) {
        int pct = it->slider;
        // Battery body: outline + proportional fill + nub, right-aligned.
        int bw = focus ? 46 : 34, bh = focus ? 16 : 11;
        int bx = 225 - bw, by = y + (h - bh) / 2;
        int nh = bh / 2, ny = by + (bh - nh) / 2;
        g->fillRoundRect(bx, by, bw, bh, 2, CC_SURF);
        g->fillRect(bx + bw, ny, 3, nh, CC_SURF);                       // positive terminal nub
        if (pct > 0) {
            int fw = (bw - 2) * pct / 100; if (fw < 1) fw = 1;
            g->fillRoundRect(bx + 1, by + 1, fw, bh - 2, 1, it->col);
        }
        if (pct >= 0) {
            char buf[8]; snprintf(buf, sizeof buf, "%d%%", pct);
            if (focus) {
                g->setTextSize(2); g->setTextColor(it->col, rbg);
                g->setCursor(bx - (int)strlen(buf) * 12 - 6, y + (h - 16) / 2 - 1);
            } else {
                g->setTextSize(1); g->setTextColor(it->col, rbg);
                g->setCursor(bx - (int)strlen(buf) * 6 - 4, y + (h - 8) / 2);
            }
            g->print(buf);
        }
        return;
    }
    if (it->kind == CC_SIGNAL) {
        // 4 bars, heights 4/7/10/13px, width 4px, gap 2px; bars lit = signal strength.
        int rssi = it->slider;
        int bars = (rssi == 0) ? 0 : (rssi >= -60) ? 4 : (rssi >= -70) ? 3 : (rssi >= -80) ? 2 : 1;
        unsigned short bc = (bars <= 1) ? C_RED : (bars <= 2) ? C_YELLOW : CC_GRN;
        const int bw = 4, gap = 2, bh[4] = {4, 7, 10, 13};
        int bx0 = 228 - (4 * bw + 3 * gap);
        int base = y + (h + 13) / 2;
        for (int b = 0; b < 4; b++)
            g->fillRoundRect(bx0 + b * (bw + gap), base - bh[b], bw, bh[b], 1,
                             b < bars ? bc : CC_SURF);
        return;
    }
    if (it->kind == CC_SLIDER) {
        int sw = focus ? 96 : 64, sh = 12, bx = 230 - sw, vy = y + (h - sh) / 2;
        g->fillRoundRect(bx, vy, sw, sh, sh / 2, CC_SURF);
        int onw = it->slider * sw / 100; if (onw < 0) onw = 0; if (onw > sw) onw = sw;
        if (onw > 0) g->fillRoundRect(bx, vy, onw, sh, sh / 2, it->col);
        int kx = bx + onw; if (kx < bx + 6) kx = bx + 6; if (kx > bx + sw - 6) kx = bx + sw - 6;
        g->fillCircle(kx, vy + sh / 2, edit ? sh / 2 + 2 : sh / 2 + 1, FG);
        if (edit) g->drawRoundRect(bx - 2, vy - 2, sw + 4, sh + 4, (sh + 4) / 2, CC_ACC);
        return;
    }
    if (it->kind == CC_TOGGLE) {
        int sw = 42, sh = 20, bx = 230 - sw, vy = y + (h - sh) / 2;
        g->fillRoundRect(bx, vy, sw, sh, sh / 2, it->on ? CC_GRN : CC_SURF);
        int kx = it->on ? bx + sw - sh / 2 - 1 : bx + sh / 2 + 1;
        g->fillCircle(kx, vy + sh / 2, sh / 2 - 3, it->on ? INK : MUTED);
        return;
    }
    if (it->kind == CC_ACTION) {
        if (s_cc_confirm == it->act) {
            const char *q = "Confermi?";
            g->setTextSize(1); g->setTextColor(C_RED, rbg);
            g->setCursor(230 - (int)strlen(q) * 6, y + (h - 8) / 2); g->print(q);
        } else {
            int bw = 28, bh = 22, bx = 230 - bw, vy = y + (h - bh) / 2;
            g->fillRoundRect(bx, vy, bw, bh, 6, focus ? it->col : CC_SURF);
            unsigned short ar = focus ? INK : MUTED;
            int ax = bx + bw / 2 - 2, ay = vy + bh / 2;
            g->fillTriangle(ax, ay - 4, ax, ay + 4, ax + 4, ay, ar);    // chevron
        }
        return;
    }
    if (it->kind == CC_CYCLE && it->val[0]) {
        // Cycled value: a size-2 pill, right-aligned (Tema / Cervello / ...).
        int vw = (int)strlen(it->val) * 12 + 14, vh = 22, bx = 230 - vw, vy = y + (h - vh) / 2;
        if (focus) g->fillRoundRect(bx, vy, vw, vh, 6, CC_SURF);
        g->setTextSize(2); g->setTextColor(it->col, focus ? CC_SURF : rbg);
        g->setCursor(bx + 7, vy + 3); g->print(it->val);
        return;
    }
    // CC_TEXT read-only value, size-1, right-aligned (RSSI / RAM / model / time ...).
    if (it->val[0]) {
        g->setTextSize(1); g->setTextColor(it->col, rbg);
        g->setCursor(230 - (int)strlen(it->val) * 6, y + (h - 8) / 2); g->print(it->val);
    }
}

// Compose the whole sheet into `g` (an off-screen canvas, or the display as a fallback).
// Layout mirrors app_video.cpp draw_settings: a dimmed top-down preview while the header is
// focused, then a centered carousel (focused row enlarged) once inside the rows.
template <typename T> static void cc_draw(T *g)
{
    const int CH = H;
    g->fillScreen(CC_BG);
    bool hdr = (s_cc_row == -1);
    cc_tabbar(g, s_cc_tab, hdr);

    CcItem it[7]; int n = cc_build(it);
    if (s_cc_row >= n) s_cc_row = n - 1;

    const int AREA_TOP = 27, AREA_BOT = CH - 12;       // content window (tab bar above, hint below)
    g->setClipRect(0, AREA_TOP, W, AREA_BOT - AREA_TOP);
    if (hdr) {
        int y = AREA_TOP + 2;                           // dimmed preview; DOWN to focus
        for (int i = 0; i < n && y < AREA_BOT; i++) { cc_set_row(g, y, false, &it[i], false); y += 34; }
    } else {
        int f = s_cc_row;
        // Top-biased cy: row 0 sits near AREA_TOP, deeper rows slide to centre.
        int cy_top = AREA_TOP + 25 + f * 32;           // if rows were stacked from top
        int cy_ctr = (AREA_TOP + AREA_BOT) / 2;        // classic centre
        int cy = (cy_top < cy_ctr) ? cy_top : cy_ctr;
        for (int i = 0; i < n; i++) {
            int dist = i - f, h = (dist == 0) ? 50 : 32, y;
            if (dist == 0)     y = cy - h / 2;
            else if (dist < 0) y = cy - 25 + dist * 32;
            else               y = cy + 25 + (dist - 1) * 32;
            if (y + h > AREA_TOP && y < AREA_BOT)
                cc_set_row(g, y, i == f, &it[i], i == f && s_cc_edit);
        }
    }
    g->clearClipRect();

    // Footer hint (Italian, matches the Music/Video sheet style) — context aware.
    const char *hint; unsigned short hc = DIM;
    int fk = (!hdr && s_cc_row >= 0 && s_cc_row < n) ? it[s_cc_row].kind : -1;
    if      (s_cc_edit)               hint = TR("L/R regola   ENTER ok", "L/R adjust   ENTER ok");
    else if (hdr)                     hint = TR("L/R scheda   DOWN righe   ESC chiudi", "L/R tab   DOWN rows   ESC close");
    else if (s_cc_confirm != A_NONE)  { hint = TR("ENTER conferma   ESC annulla", "ENTER confirm   ESC cancel"); hc = C_RED; }
    else if (fk == CC_ACTION)         hint = TR("ENTER esegui   L/R scheda", "ENTER run   L/R tab");
    else if (fk == CC_TOGGLE)    hint = TR("ENTER attiva/disattiva   L/R scheda", "ENTER toggle   L/R tab");
    else if (fk == CC_CYCLE)     hint = TR("ENTER cambia   L/R scheda", "ENTER change   L/R tab");
    else if (fk == CC_SLIDER)    hint = TR("ENTER regola   L/R scheda", "ENTER adjust   L/R tab");
    else                         hint = TR("su/giu riga   L/R scheda   ESC indietro", "up/dn row   L/R tab   ESC back");
    g->setTextSize(1); g->setTextColor(hc, CC_BG); g->setCursor(8, CH - 10); g->print(hint);
}

// The sheet is a full-screen overlay repainted every second (clock + Now-Playing time) and on
// every adjust key. Drawing it straight to the display means fillScreen(INK) + a full redraw
// flash blank-then-paint = flicker. So we composite into the shared back-buffer and blit in ONE
// pushSprite. No separate allocation: the launcher list underneath isn't drawing while the
// sheet is up, so the one permanent buffer serves both (the buffer is taller than the screen;
// the panel clips the extra rows on push).
void launcher_render_control_center(void)
{
    M5Canvas *c = nucleo_screen();
    if (c) { cc_draw(c); c->pushSprite(0, 0); }            // one blit -> no flicker
    else   { cc_draw(&d); }                                // low-heap fallback: direct (may flicker)
}

void launcher_render_control_center_close(void) { }       // no-op: buffer is shared (kept for call sites)
