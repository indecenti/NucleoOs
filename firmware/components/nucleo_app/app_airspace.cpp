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
#include "app_large.h"      // two screens: the airspace dashboard on the 320x240 panel
#include "nucleo_touch.h"

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
    nucleo_app_set_hint(s_on ? TR5("monitor passivo   esc esci", "passive monitor   esc back", "monitor pasivo   esc volver", "moniteur passif   esc retour", "passiver monitor   esc zuruck")
                             : TR5("radio occupata   esc esci", "radio busy   esc back", "radio ocup.   esc volver", "radio occ.   esc retour", "radio besetzt   esc zur."));
    nucleo_app_request_draw();
}

static void on_exit(void) {
    if (s_on) { sentinel_airspace_stop(); s_on = false; }
}

static void on_tick(void) { nucleo_app_request_draw(); }

static void draw_large(void);
static void draw_companion(void);

static void on_draw(void) {
    if (nucleo_app_two_screens()) { draw_companion(); return; }
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    uint8_t a = s_on ? sentinel_airspace_alerts() : 0;
    int y0 = app_ui_title("Airspace", a ? ALERT : ACC, s_on ? TR5("in ascolto", "listening", "en escucha", "en ecoute", "hoert zu") : "off");

    char ln[52];
    snprintf(ln, sizeof ln, "ch %d  deauth %u  beacon %u",
             nucleo_wifiatk_sniffer_channel(),
             nucleo_wifiatk_sniffer_deauth(), nucleo_wifiatk_sniffer_beacons());
    txt(8, y0 + 6, ln, MUTED, BG, 1);

    int y = y0 + 24;
    if (a & SENTINEL_A_DEAUTH_FLOOD)     { txt(8, y, TR5("! deauth flood", "! deauth flood", "! deauth flood", "! deauth flood", "! deauth flood"), ALERT, BG, 2); y += 20; }
    if (a & SENTINEL_A_BROADCAST_DEAUTH) { txt(8, y, TR5("! deauth broadcast", "! broadcast deauth", "! broadcast deauth", "! broadcast deauth", "! broadcast deauth"), ALERT, BG, 2); y += 20; }
    if (a & SENTINEL_A_EVIL_TWIN)        { txt(8, y, TR5("! gemello malevolo", "! evil twin AP", "! evil twin AP", "! evil twin AP", "! evil twin AP"), ALERT, BG, 2); y += 20; }
    if (a & SENTINEL_A_BEACON_FLOOD)     { txt(8, y, TR5("! beacon flood", "! beacon flood", "! beacon flood", "! beacon flood", "! beacon flood"), WARN, BG, 2); y += 20; }
    if (!s_on)   txt(8, y, TR5("Non avviato (radio occupata)", "Not started (radio busy)", "No iniciado (radio ocupada)", "Non demarr. (radio occ.)", "nicht gestart. (radio bes.)"), WARN, BG, 2);
    else if (!a) txt(8, y, TR5("Nessuna anomalia.", "No anomalies.", "Sin anomalias.", "Aucune anomalie.", "keine anomalien."), GOOD, BG, 2);

    txt(8, h - 14, TR5("solo ascolto - zero TX", "listen only - zero TX", "solo escucha - sin TX", "ecoute seul - zero TX", "nur hoeren - kein TX"), DIM, BG, 1);
}

// ===== two screens ==============================================================================================
// The panel carries the defence dashboard: a big listening/alert status, the live frame counters (channel /
// deauth / beacon) in a card, and a row of alert cards — one per raised anomaly. No controls: it is a passive
// monitor, so the only touch target is the header chevron (close). The Cardputer becomes the companion: the
// alert count big, the counters, and which anomalies are up.
struct AlertDef { uint8_t bit; const char *it; const char *en; unsigned short col; };
static const AlertDef ALERTS[] = {
    { SENTINEL_A_DEAUTH_FLOOD,     "Deauth flood",      "Deauth flood",      ALERT },
    { SENTINEL_A_BROADCAST_DEAUTH, "Deauth broadcast",  "Broadcast deauth",  ALERT },
    { SENTINEL_A_EVIL_TWIN,        "Gemello malevolo",  "Evil-twin AP",      ALERT },
    { SENTINEL_A_BEACON_FLOOD,     "Beacon flood",      "Beacon flood",      WARN  },
};
static const int N_ALERT = (int)(sizeof(ALERTS) / sizeof(ALERTS[0]));

static void draw_large(void) {
    const int Wp = d.width(), Hp = d.height();
    nucleo_touch_clear();
    const uint8_t a = s_on ? sentinel_airspace_alerts() : 0;
    const char *status = !s_on ? TR("radio occupata", "radio busy") : a ? TR("ALLARME", "ALERT") : TR("in ascolto", "listening");
    int y = lg::header("Airspace", status, a ? ALERT : (s_on ? ACC : WARN), NK_BACK);
    d.fillRect(0, y, Wp, Hp - y, BG);

    // Live frame counters in a card.
    lg::panel(12, y + 8, Wp - 24, 78);
    const unsigned short cb = lg::card();
    char v[16];
    snprintf(v, sizeof v, "%d", nucleo_wifiatk_sniffer_channel());
    lg::kv(26, y + 18, Wp - 52, TR("Canale", "Channel"), s_on ? v : "-", FG, cb);
    snprintf(v, sizeof v, "%u", nucleo_wifiatk_sniffer_deauth());
    lg::kv(26, y + 42, Wp - 52, "Deauth", s_on ? v : "-", s_on && nucleo_wifiatk_sniffer_deauth() ? ALERT : MUTED, cb);
    snprintf(v, sizeof v, "%u", nucleo_wifiatk_sniffer_beacons());
    lg::kv(26, y + 66, Wp - 52, "Beacon", s_on ? v : "-", FG, cb);

    int cy = y + 96;
    if (!s_on) {
        lg::message(12, cy, Wp - 24, Hp - cy - 24, TR("Non avviato", "Not started"),
                    TR("Radio occupata da un'altra app.", "Radio busy with another tool."), WARN);
    } else if (!a) {
        lg::message(12, cy, Wp - 24, Hp - cy - 24, TR("Nessuna anomalia", "No anomalies"),
                    TR("L'aria e' pulita.", "The airspace is clean."), C_GREEN);
    } else {
        for (int i = 0; i < N_ALERT; i++) {
            if (!(a & ALERTS[i].bit)) continue;
            d.fillRoundRect(12, cy, Wp - 24, 28, 7, ALERTS[i].col);
            lg::font(1); d.setTextColor(INK, ALERTS[i].col); d.setTextDatum(middle_left);
            char b[40]; snprintf(b, sizeof b, "!  %s", TR(ALERTS[i].it, ALERTS[i].en));
            d.drawString(b, 26, cy + 15); d.setTextDatum(top_left);
            cy += 34;
        }
    }
    lg::font(0); d.setTextColor(DIM, BG);
    d.setTextDatum(bottom_left); d.drawString(TR("solo ascolto - zero TX", "listen only - zero TX"), 12, Hp - 4);
    d.setTextDatum(top_left);
    nucleo_touch_commit();
}

static void draw_companion(void) {
    const int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    int y = lg::comp_head("AIRSPACE", ACC);
    const uint8_t a = s_on ? sentinel_airspace_alerts() : 0;
    int na = 0; for (int i = 0; i < N_ALERT; i++) if (a & ALERTS[i].bit) na++;
    d.setFont(&fonts::FreeSansBold18pt7b); d.setTextColor(a ? ALERT : (s_on ? C_GREEN : WARN), BG);
    if (!s_on) { d.setFont(&fonts::FreeSansBold9pt7b); d.drawString(TR("radio occupata", "radio busy"), 8, y + 4); }
    else { char n[4]; snprintf(n, sizeof n, "%d", na); d.drawString(n, 8, y + 2);
           d.setFont(&fonts::Font0); d.setTextColor(a ? ALERT : MUTED, BG);
           d.drawString(a ? TR("ANOMALIE", "ANOMALIES") : TR("aria pulita", "airspace clean"), 48, y + 10); }
    if (s_on) {
        d.setFont(&fonts::Font0); d.setTextColor(ACC, BG);
        char ln[40]; snprintf(ln, sizeof ln, "ch %d  deauth %u  beacon %u", nucleo_wifiatk_sniffer_channel(),
                              nucleo_wifiatk_sniffer_deauth(), nucleo_wifiatk_sniffer_beacons());
        d.drawString(ln, 8, y + 42);
        int ly = y + 58;
        for (int i = 0; i < N_ALERT && ly < top + h - 18; i++) if (a & ALERTS[i].bit) {
            d.setTextColor(ALERTS[i].col, BG); char b[32]; snprintf(b, sizeof b, "! %s", TR(ALERTS[i].it, ALERTS[i].en));
            d.drawString(b, 8, ly); ly += 12;
        }
    }
    lg::comp_keys(TR("solo ascolto - zero TX   ESC esci", "listen only - zero TX   ESC back"));
}

extern "C" void nucleo_register_airspace(void) {
    static const nucleo_app_def_t app = {
        "airspace", "Airspace", "Security",
        "Wi-Fi attack detector (deauth / evil-twin, passive)",
        'A', 0xFD20, on_enter, nullptr, on_tick, on_draw, on_exit, 0, nullptr, draw_large
    };
    nucleo_app_register(&app);
}
