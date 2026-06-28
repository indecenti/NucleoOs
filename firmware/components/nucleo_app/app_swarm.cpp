// app_swarm.cpp — "Sciame": the device-to-device swarm test surface (no PC needed).
//
// Shows our identity + membership state, the peers discovered via gossiped manifests, and a PING
// action whose pongs prove a bidirectional, authenticated round-trip. The engine (nucleo_swarm_espnow.c)
// owns the radio/crypto state and self-pumps on its own task; this file is the thin UI.
//
// RAM: exclusive_flags = NX_NET_APP — the framework frees ~70KB (httpd/mDNS/voice/L1) for the app's
// whole foreground life while Wi-Fi STA (which ESP-NOW rides) stays up. Same as Vicino.
#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "nucleo_exclusive.h"
#include "launcher_theme.h"
#include "app_gfx.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>

extern "C" {
#include "nucleo_swarm_espnow.h"
}

static const unsigned short SURF = 0x10A2, CAP = 0x1A8B, ACC = C_BLUE, GRN = C_GREEN, AMB = C_YELLOW;
static int s_sel = 0;

static void txt(int x, int y, const char *s, uint16_t fg, uint16_t bg, int sz) {
    d.setTextSize(sz); d.setTextColor(fg, bg); d.setCursor(x, y); d.print(s);
}

static void on_draw(void) {
    int ch = nucleo_app_content_height();
    d.fillRect(0, 0, W, ch, BG);

    // ── header: big title + membership pill (state at a glance) ──
    txt(8, 5, "Sciame", ACC, BG, 2);
    bool open = swarm_svc_is_open();
    const char *stx = open ? "APERTO" : "PROTETTO";
    uint16_t sc = open ? AMB : GRN;
    int pw = (int)strlen(stx) * 6 + 12;
    d.fillRoundRect(W - 8 - pw, 5, pw, 16, 8, sc);
    txt(W - 8 - pw + 6, 9, stx, INK, sc, 1);
    d.drawFastHLine(0, 25, W, LINE);

    int n = swarm_svc_peer_count();
    if (!n) {
        // big, readable empty state — say what to do (smartwatch idiom)
        txt(10, 40, "Nessun vicino", FG, BG, 2);
        txt(10, 68, "Apri Sciame sull'altro", MUTED, BG, 1);
        txt(10, 82, "Cardputer, stesso canale.", MUTED, BG, 1);
    } else {
        // focus-enlarged centered list (same visual language as Vicino/Wi-Fi)
        if (s_sel >= n) s_sel = n - 1;
        if (s_sel < 0)  s_sel = 0;
        int top = 29, bot = ch - 19, cy = (top + bot) / 2;
        d.setClipRect(0, top, W, bot - top);
        for (int i = 0; i < n; i++) {
            int dist = i - s_sel, h = (dist == 0) ? 40 : 26, y;
            if (dist == 0)     y = cy - 20;
            else if (dist < 0) y = cy - 20 + dist * 26;
            else               y = cy + 20 + (dist - 1) * 26;
            if (y + h <= top || y >= bot) continue;
            bool f = (i == s_sel);
            d.fillRoundRect(4, y, W - 8, h - 2, 8, f ? CAP : BG);
            if (f) d.fillRoundRect(4, y + 3, 5, h - 8, 2, ACC);
            char nm[24]; snprintf(nm, sizeof nm, "%.18s", swarm_svc_peer_id(i));
            txt(16, y + (h - (f ? 16 : 8)) / 2, nm, f ? FG : MUTED, f ? CAP : BG, f ? 2 : 1);   // selected = big
            int busy = swarm_svc_peer_busy(i);
            char rb[16]; snprintf(rb, sizeof rb, "%dKB", swarm_svc_peer_free(i));
            txt(W - 14 - (int)strlen(rb) * 6, y + (h - 8) / 2, rb, busy ? AMB : GRN, f ? CAP : BG, 1);
        }
        d.clearClipRect();
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
    if (key == NK_UP)        { if (s_sel > 0) s_sel--; }
    else if (key == NK_DOWN) { if (s_sel < n - 1) s_sel++; }
    else if (key == NK_ENTER || chr == 'p' || chr == 'P' || chr == ' ') swarm_svc_ping();
    nucleo_app_request_draw();
}

static void on_tick(void) { nucleo_app_request_draw(); }   // 5Hz refresh of peers/activity

static void on_enter(void) {
    s_sel = 0;
    nucleo_app_set_hint("INVIO ping   SU/GIU   esc");
    if (!swarm_svc_start()) nucleo_app_set_hint("ESP-NOW non avviato   esc");
}
static void on_exit(void) { swarm_svc_stop(); }

extern "C" void nucleo_register_swarm(void) {
    static const nucleo_app_def_t app = {
        "swarm", "Sciame", "Connect",
        "Trova i Cardputer vicini e fai un ping (test sciame ESP-NOW)",
        'S', C_BLUE,
        on_enter, on_key, on_tick, on_draw, on_exit,
        NX_NET_APP,                            // ~70KB freed, Wi-Fi STA stays up (ESP-NOW rides it)
    };
    nucleo_app_register(&app);
}
