// app_airspace.cpp — Security > Airspace: passive Wi-Fi attack detector.
//
// DEFENSIVE, never transmits. Watches 802.11 management frames (via the
// nucleo_wifiatk promiscuous monitor, no .pcap) and raises an alert on a deauth
// flood, broadcast deauth, evil-twin AP, or beacon flood around you. Separate app
// from Sentinel's BLE tracker because this chip has no Wi-Fi/BT coexistence — the
// monitor owns the radio (NX_NET_APP) and restores the OS network on exit.
#include "nucleo_app.h"
#include "launcher_theme.h"
#include "app_gfx.h"
#include "app_ui.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
extern "C" {
#include "nucleo_sentinel.h"
#include "nucleo_wifiatk.h"
#include "nucleo_i18n.h"
}

static const unsigned short ACC = C_BLUE, WARN = C_YELLOW, ALERT = C_RED, GOOD = C_GREEN;
static bool s_on = false;

static void txt(int x, int y, const char *s, uint16_t fg, uint16_t bg, int sz) {
    d.setTextSize(sz); d.setTextColor(fg, bg); d.setCursor(x, y); d.print(s);
}

static void on_enter(void) {
    sentinel_airspace_start();
    // Be honest about whether the sniffer actually came up: it fails if another Wi-Fi radio tool
    // (e.g. a Deauth Flood left running) already owns the radio, or if heap/promiscuous setup failed.
    // Showing "listening / no anomalies" while doing nothing is the worst failure mode for a monitor.
    s_on = nucleo_wifiatk_sniffer_running();
    nucleo_app_set_hint(s_on ? TR("monitor passivo   esc esci", "passive monitor   esc back")
                             : TR("radio occupata   esc esci", "radio busy   esc back"));
    nucleo_app_request_draw();
}

static void on_exit(void) {
    if (s_on) { sentinel_airspace_stop(); s_on = false; }
}

static void on_tick(void) { nucleo_app_request_draw(); }

static void on_draw(void) {
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    uint8_t a = s_on ? sentinel_airspace_alerts() : 0;
    int y0 = app_ui_title("Airspace", a ? ALERT : ACC, s_on ? TR("in ascolto", "listening") : "off");

    char ln[52];
    snprintf(ln, sizeof ln, "ch %d  deauth %u  beacon %u",
             nucleo_wifiatk_sniffer_channel(),
             nucleo_wifiatk_sniffer_deauth(), nucleo_wifiatk_sniffer_beacons());
    txt(8, y0 + 6, ln, MUTED, BG, 1);

    int y = y0 + 24;
    if (a & SENTINEL_A_DEAUTH_FLOOD)     { txt(8, y, TR("! deauth flood", "! deauth flood"), ALERT, BG, 2); y += 20; }
    if (a & SENTINEL_A_BROADCAST_DEAUTH) { txt(8, y, TR("! deauth broadcast", "! broadcast deauth"), ALERT, BG, 2); y += 20; }
    if (a & SENTINEL_A_EVIL_TWIN)        { txt(8, y, TR("! gemello malevolo", "! evil twin AP"), ALERT, BG, 2); y += 20; }
    if (a & SENTINEL_A_BEACON_FLOOD)     { txt(8, y, TR("! beacon flood", "! beacon flood"), WARN, BG, 2); y += 20; }
    if (!s_on)   txt(8, y, TR("Non avviato (radio occupata)", "Not started (radio busy)"), WARN, BG, 2);
    else if (!a) txt(8, y, TR("Nessuna anomalia.", "No anomalies."), GOOD, BG, 2);

    txt(8, h - 14, TR("solo ascolto - zero TX", "listen only - zero TX"), DIM, BG, 1);
}

extern "C" void nucleo_register_airspace(void) {
    static const nucleo_app_def_t app = {
        "airspace", "Airspace", "Security",
        "Wi-Fi attack detector (deauth / evil-twin, passive)",
        'A', 0xFD20, on_enter, nullptr, on_tick, on_draw, on_exit, 0
    };
    nucleo_app_register(&app);
}
