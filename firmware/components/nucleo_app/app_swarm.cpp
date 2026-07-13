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
#include "nucleo_i18n.h"       // TR(it,en): hints follow the system language
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>

extern "C" {
#include "nucleo_swarm_espnow.h"
}

static const unsigned short SURF = 0x10A2, ACC = C_BLUE, GRN = C_GREEN, AMB = C_YELLOW;
static int s_sel = 0;

static void txt(int x, int y, const char *s, uint16_t fg, uint16_t bg, int sz) {
    d.setTextSize(sz); d.setTextColor(fg, bg); d.setCursor(x, y); d.print(s);
}

// ---- shared-list providers -------------------------------------------------
static const char *sw_label(int i, void *) { return swarm_svc_peer_id(i); }
static const char *sw_right(int i, void *) { static char b[16]; snprintf(b, sizeof b, "%dKB", swarm_svc_peer_free(i)); return b; }
static unsigned short sw_col(int i, void *) { return swarm_svc_peer_busy(i) ? AMB : GRN; }   // free=green, busy=amber

static void on_draw(void) {
    int ch = nucleo_app_content_height();
    d.fillRect(0, 0, W, ch, BG);

    // ── header: accent title + hairline (app_ui_title) with a membership pill on the right ──
    int y0 = app_ui_title("Sciame", ACC, nullptr);
    bool open = swarm_svc_is_open();
    const char *stx = open ? "APERTO" : "PROTETTO";
    uint16_t sc = open ? AMB : GRN;
    int pw = (int)strlen(stx) * 6 + 12;
    d.fillRoundRect(W - 8 - pw, 4, pw, 15, 7, sc);
    txt(W - 8 - pw + 6, 8, stx, INK, sc, 1);

    int n = swarm_svc_peer_count();
    int bot = ch - 19;
    if (!n) {
        // big, readable empty state — say what to do (smartwatch idiom)
        txt(10, y0 + 14, "Nessun vicino", FG, BG, 2);
        txt(10, y0 + 40, "Apri Sciame sull'altro", MUTED, BG, 1);
        txt(10, y0 + 54, "Cardputer, stesso canale.", MUTED, BG, 1);
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
    else { char ft[48]; snprintf(ft, sizeof ft, "io: %.12s   INVIO = ping", swarm_svc_name()); txt(12, iy + 4, ft, MUTED, SURF, 1); }
}

static void on_key(int key, char chr) {
    int n = swarm_svc_peer_count();
    if (app_ui_list_key(key, chr, &s_sel, n, sw_label, nullptr)) { nucleo_app_request_draw(); return; }
    if (key == NK_ENTER || chr == 'p' || chr == 'P' || chr == ' ') swarm_svc_ping();
    nucleo_app_request_draw();
}

static void on_tick(void) { nucleo_app_request_draw(); }   // 5Hz refresh of peers/activity (buffered: no flicker)

static void on_enter(void) {
    s_sel = 0;
    nucleo_app_set_hint(TR("invio ping   su/giu   esc", "enter ping   up/dn   esc"));
    if (!swarm_svc_start()) nucleo_app_set_hint(TR("ESP-NOW non avviato   esc", "ESP-NOW not started   esc"));
}
static void on_exit(void) { swarm_svc_stop(); }

extern "C" void nucleo_register_swarm(void) {
    static const nucleo_app_def_t app = {
        "swarm", "Sciame", "Communication",
        "Trova i Cardputer vicini e fai un ping (test sciame ESP-NOW)",
        'S', C_BLUE,
        on_enter, on_key, on_tick, on_draw, on_exit,
        NX_NET_APP,                            // ~70KB freed, Wi-Fi STA stays up (ESP-NOW rides it)
    };
    nucleo_app_register(&app);
}
