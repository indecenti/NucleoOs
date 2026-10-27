// app_swarm.cpp — "Sciame": the device-to-device swarm test surface (no PC needed).
//
// Shows our identity + membership state, the peers discovered via gossiped manifests, and a PING
// action whose pongs prove a bidirectional, authenticated round-trip. The engine (nucleo_swarm_espnow.c)
// owns the radio/crypto state and self-pumps on its own task; this file is the thin UI.
//
// UI is the OS-standard native look shared with the launcher / Files / Vicino: an app_ui_title accent
// header (size-2 title + hairline + accent underline) with a membership pill, and the shared
// app_ui_list widget for the peer list — the focused row and its neighbours are the big readable
// size-2 pill, with the launcher's 1-9 quick-jump + type-ahead for free.
//
// RAM: exclusive_flags = NX_NET_APP — the framework frees ~70KB (httpd/mDNS/voice/L1) for the app's
// whole foreground life while Wi-Fi STA (which ESP-NOW rides) stays up. Same as Vicino.
#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "nucleo_exclusive.h"
#include "launcher_theme.h"
#include "app_gfx.h"
#include "app_ui.h"            // shared focused-list widget + type-ahead/quick-select nav
#include "app_large.h"         // two screens: the swarm on the 320x240 panel, the Cardputer as companion
#include "nucleo_touch.h"
#include "nucleo_i18n.h"       // TR(it,en): hints follow the system language
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>

extern "C" {
#include "nucleo_swarm_espnow.h"
}

static const unsigned short SURF = 0x10A2, ACC = C_BLUE, GRN = C_GREEN, AMB = C_YELLOW;
static int s_sel = 0;
static int s_lg_scroll = 0;              // first visible row on the panel list

static void txt(int x, int y, const char *s, uint16_t fg, uint16_t bg, int sz) {
    d.setTextSize(sz); d.setTextColor(fg, bg); d.setCursor(x, y); d.print(s);
}

// ---- shared-list providers -------------------------------------------------
static const char *sw_label(int i, void *) { return swarm_svc_peer_id(i); }
static const char *sw_right(int i, void *) { static char b[16]; snprintf(b, sizeof b, "%dKB", swarm_svc_peer_free(i)); return b; }
static unsigned short sw_col(int i, void *) { return swarm_svc_peer_busy(i) ? AMB : GRN; }   // free=green, busy=amber

// ===== two screens ==============================================================================================
// The panel carries the swarm: identity + membership in the header, the peers as a touch list, and one big PING
// button at the foot. The Cardputer (where the typing / keys live) becomes the companion: membership, how many
// peers, the pick, the last activity, and the key map.
static void draw_large(void)
{
    const int W = d.width(), H = d.height();
    nucleo_touch_clear();
    char chs[16]; snprintf(chs, sizeof chs, "ch%d", swarm_svc_channel());
    const int y = lg::header(TR("Sciame", "Swarm"), chs, ACC);
    // identity (left) + membership pill (right), on one line under the header
    const bool open = swarm_svc_is_open();
    const char *stx = open ? TR("APERTO", "OPEN") : TR("PROTETTO", "SECURE");
    const unsigned short sc = open ? AMB : GRN;
    lg::font(0);
    const int pw = d.textWidth(stx) + 16;
    d.fillRoundRect(W - 8 - pw, y + 6, pw, 18, 9, sc);
    d.setTextDatum(middle_center); d.setTextColor(INK, sc); d.drawString(stx, W - 8 - pw / 2, y + 6 + 9);
    d.setTextDatum(top_left);
    lg::font(1); d.setTextColor(FG, BG);
    char me[40]; lg::fit(me, sizeof me, swarm_svc_name(), W - 8 - pw - 20);
    d.drawString(me, 12, y + 8);

    const int bh = 48, foot = 18;
    const int listy = y + 34;
    const int listh = H - listy - bh - foot - 8;
    const int n = swarm_svc_peer_count();
    if (!n) {
        lg::message(0, listy, W, listh + foot, TR("Nessun vicino", "No peers"),
                    TR("Apri Sciame sull'altro dispositivo, stesso canale.",
                       "Open Swarm on the other device, same channel."), MUTED);
    } else {
        if (s_sel >= n) s_sel = n - 1;
        if (s_sel < 0)  s_sel = 0;
        lg::text_list(0, listy, W, listh, n, s_sel, &s_lg_scroll, sw_label, sw_right, sw_col, nullptr, ACC);
        char last[48]; swarm_svc_last(last, sizeof last);
        d.fillRect(0, listy + listh, W, foot, BG);
        if (last[0]) { lg::font(0); d.setTextColor(AMB, BG); char f[52]; lg::fit(f, sizeof f, last, W - 20); d.drawString(f, 12, listy + listh + 3); }
    }
    lg::button(12, H - bh - 4, W - 24, bh, TR("Ping", "Ping"), ACC, INK, NK_ENTER, 0);
    nucleo_touch_commit();
}

// The Cardputer while the panel shows the swarm.
static void draw_companion(void)
{
    const int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    const int y = lg::comp_head("SWARM", ACC);
    const bool open = swarm_svc_is_open();
    d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(open ? AMB : GRN, BG);
    d.drawString(open ? TR("Aperto", "Open") : TR("Protetto", "Secure"), 8, y + 4);
    const int n = swarm_svc_peer_count();
    d.setFont(&fonts::Font0); d.setTextColor(FG, BG);
    char l[40]; snprintf(l, sizeof l, TR("%d vicini  -  ch%d", "%d peers  -  ch%d"), n, swarm_svc_channel());
    d.drawString(l, 8, y + 26);
    if (n) { const char *p = swarm_svc_peer_id(s_sel < n ? s_sel : 0); d.setTextColor(MUTED, BG); char b[44]; snprintf(b, sizeof b, "-> %.20s", p ? p : ""); d.drawString(b, 8, y + 40); }
    char last[48]; swarm_svc_last(last, sizeof last);
    if (last[0]) { d.setTextColor(AMB, BG); char f[40]; snprintf(f, sizeof f, "%.32s", last); d.drawString(f, 8, y + 54); }
    lg::comp_keys(TR("INVIO ping   su/giu   ESC", "ENTER ping   up/dn   ESC"));
}

static void on_draw(void) {
    if (nucleo_app_two_screens()) { draw_companion(); return; }
    int ch = nucleo_app_content_height();
    d.fillRect(0, 0, W, ch, BG);

    // ── header: accent title + hairline (app_ui_title) with a membership pill on the right ──
    int y0 = app_ui_title(TR("Sciame", "Swarm"), ACC, nullptr);
    bool open = swarm_svc_is_open();
    const char *stx = open ? TR("APERTO", "OPEN") : TR("PROTETTO", "SECURE");
    uint16_t sc = open ? AMB : GRN;
    int pw = (int)strlen(stx) * 6 + 12;
    d.fillRoundRect(W - 8 - pw, 4, pw, 15, 7, sc);
    txt(W - 8 - pw + 6, 8, stx, INK, sc, 1);

    int n = swarm_svc_peer_count();
    int bot = ch - 19;
    if (!n) {
        // big, readable empty state — say what to do (smartwatch idiom)
        txt(10, y0 + 14, TR("Nessun vicino", "No peers"), FG, BG, 2);
        txt(10, y0 + 40, TR("Apri Sciame sull'altro", "Open Swarm on the other"), MUTED, BG, 1);
        txt(10, y0 + 54, TR("Cardputer, stesso canale.", "Cardputer, same channel."), MUTED, BG, 1);
    } else {
        if (s_sel >= n) s_sel = n - 1;
        if (s_sel < 0)  s_sel = 0;
        app_ui_list(y0, bot - y0, n, s_sel, sw_label, sw_right, sw_col, nullptr);
    }

    // ── footer: live activity, else identity + the ping affordance ──
    char last[48]; swarm_svc_last(last, sizeof last);
    int iy = ch - 17;
    d.fillRoundRect(6, iy, W - 12, 15, 5, SURF);
    if (last[0]) txt(12, iy + 4, last, AMB, SURF, 1);
    else { char ft[48]; snprintf(ft, sizeof ft, TR("io: %.12s   INVIO = ping", "me: %.12s   ENTER = ping"), swarm_svc_name()); txt(12, iy + 4, ft, MUTED, SURF, 1); }
}

static void on_key(int key, char chr) {
    int n = swarm_svc_peer_count();
    if (lg::row_tap(key, chr, s_lg_scroll, n, &s_sel)) { nucleo_app_request_draw(); return; }   // a tapped peer row
    if (app_ui_list_key(key, chr, &s_sel, n, sw_label, nullptr)) { nucleo_app_request_draw(); return; }
    if (key == NK_ENTER || chr == 'p' || chr == 'P' || chr == ' ') swarm_svc_ping();
    nucleo_app_request_draw();
}

static void on_tick(void) { nucleo_app_request_draw(); }   // 5Hz refresh of peers/activity (buffered: no flicker)

static void on_enter(void) {
    s_sel = 0;
    nucleo_app_set_hint(TR5("invio ping   su/giu   esc", "enter ping   up/dn   esc", "intro ping   ar/ab   esc", "entrer ping   h/b   esc", "enter ping   auf/ab   esc"));
    if (!swarm_svc_start()) nucleo_app_set_hint(TR5("ESP-NOW non avviato   esc", "ESP-NOW not started   esc", "ESP-NOW no iniciado   esc", "ESP-NOW non demarr   esc", "ESP-NOW nicht gestart   esc"));
}
static void on_exit(void) { swarm_svc_stop(); }

extern "C" void nucleo_register_swarm(void) {
    static const nucleo_app_def_t app = {
        "swarm", "Sciame", "Messaging",
        "Trova i Cardputer vicini e fai un ping (test sciame ESP-NOW)",
        'S', C_BLUE,
        on_enter, on_key, on_tick, on_draw, on_exit,
        NX_NET_APP,                            // ~70KB freed, Wi-Fi STA stays up (ESP-NOW rides it)
        nullptr, draw_large,
    };
    nucleo_app_register(&app);
}
