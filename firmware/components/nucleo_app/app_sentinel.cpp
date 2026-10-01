// app_sentinel.cpp — Security > Sentinel: passive anti-stalking tracker detector.
//
// DEFENSIVE, never transmits. Piggybacks on the BLE passive scan: every nearby
// advertisement is classified (AirTag/FindMy, Samsung SmartTag, Tile, Chipolo)
// and a device that stays with you long enough is flagged as "following".
//
// PLUG-AND-PLAY: flagged NX_SOLO|NX_DEEP_OFFLINE|NX_BLE, so opening it reboots
// ONCE into a fresh, unfragmented heap with Bluetooth kept for that boot (this
// chip has no Wi-Fi/BT coexistence, so BLE needs a dedicated boot) and the app
// re-opens itself with the radio already up — no "enable Bluetooth + reboot +
// reopen" dance. Esc reboots back to the normal OS (BLE released, RAM reclaimed).
#include "nucleo_app.h"
#include "launcher_theme.h"
#include "app_gfx.h"
#include "app_ui.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
extern "C" {
#include "nucleo_ble.h"
#include "nucleo_sentinel.h"
#include "nucleo_exclusive.h"
#include "nucleo_i18n.h"         // TR(it, en)
}

static const unsigned short ACC = C_GREEN, WARN = C_YELLOW, ALERT = C_RED, SURF = 0x10A2;

enum Screen { LIST, INFO };
static Screen s_screen = LIST;
static bool   s_up_ok = false;

static void txt(int x, int y, const char *s, uint16_t fg, uint16_t bg, int sz) {
    d.setTextSize(sz); d.setTextColor(fg, bg); d.setCursor(x, y); d.print(s);
}

static bool on_back(int key) {
    (void)key;
    if (s_screen != LIST) { s_screen = LIST; nucleo_app_request_draw(); return true; }
    return false;   // from the list, let the framework close the app (reboots to OS)
}
static void on_tab(void) { s_screen = (s_screen == LIST) ? INFO : LIST; nucleo_app_request_draw(); }

static void on_enter(void) {
    // We are in the Solo boot: NX_DEEP_OFFLINE applied (Wi-Fi/httpd/mDNS/voice/L1
    // down) and NX_BLE kept the controller — so the radio is present and comes up.
    s_screen = LIST;
    s_up_ok = nucleo_ble_radio_present() && nucleo_ble_up();
    if (s_up_ok) { nucleo_ble_scan_start(); sentinel_tracker_start(); }
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_tab_handler(on_tab);
    nucleo_app_set_hint(TR5("tab: info   esc: esci", "tab: info   esc: back", "tab: info   esc: salir", "tab: info   esc: retour", "tab: info   esc: zurueck"));
    nucleo_app_request_draw();
}

static void on_exit(void) {
    if (!s_up_ok) return;
    sentinel_tracker_stop();
    nucleo_ble_scan_stop();
    nucleo_ble_down();
}

static void on_tick(void) {
    if (s_up_ok) sentinel_tracker_tick();
    nucleo_app_request_draw();
}

static void draw_list(int h) {
    if (!s_up_ok) {   // BLE didn't come up (rare after the BLE-Solo Wi-Fi-skip fix)
        int y0 = app_ui_title("Sentinel", WARN, nullptr);
        txt(8, y0 + 14, TR5("Bluetooth non attivo.", "Bluetooth not active.", "Bluetooth no activo.", "Bluetooth non actif.", "Bluetooth nicht aktiv."), FG, BG, 1);
        txt(8, y0 + 30, TR5("Riprova ad aprire l'app.", "Reopen the app to retry.", "Abre de nuevo la app.", "Rouvrez app pr essayer.", "Oeffne app zum versuchen."), MUTED, BG, 1);
        txt(8, y0 + 46, TR5("esc: torna al sistema", "esc: back to the system", "esc: vuelve al sistema", "esc: retour au systeme", "esc: zurueck zum system"), DIM, BG, 1);
        return;
    }
    int following = sentinel_tracker_following();
    int y0 = app_ui_title("Sentinel", following ? ALERT : ACC,
                          nucleo_ble_is_synced() ? TR5("scansione", "scanning", "escaneando", "balayage", "scan") : TR5("avvio...", "starting...", "iniciando...", "demarrage...", "starten..."));

    if (following > 0) {   // prominent alert banner
        d.fillRoundRect(6, y0 + 2, W - 12, 20, 5, ALERT);
        char b[40];
        snprintf(b, sizeof b, TR5("!  %d tracker ti segue", "!  %d tracker following you", "!  %d tracker te sigue", "!  %d tracker te suivant", "!  %d tracker folgt dir"), following);
        txt(12, y0 + 7, b, 0x0000, ALERT, 1);
        y0 += 22;
    }

    int cnt = sentinel_tracker_count();
    char cb[24]; snprintf(cb, sizeof cb, TR5("%d tracker vicini", "%d trackers nearby", "%d trackers cercanos", "%d trackers proches", "%d tracker nah"), cnt);
    txt(8, y0 + 6, cb, following ? ALERT : ACC, BG, 1);

    int y = y0 + 22; sentinel_view_t v; char ln[56];
    for (int i = 0; i < cnt && i < 6; i++) {
        if (!sentinel_tracker_get(i, &v)) break;
        const char *tag = v.following ? "<<" : (v.flags & 0x01 /*SEPARATED*/ ? " !" : "  ");
        snprintf(ln, sizeof ln, "%-16.16s %4d %3lus%s", sentinel_type_name(v.type),
                 v.rssi, (unsigned long)v.age_s, tag);
        txt(8, y, ln, v.following ? ALERT : FG, BG, 1);
        y += 12;
    }
    if (!cnt) txt(8, y, TR5("Nessun tracker rilevato.", "No trackers detected.", "Sin tracker detectado.", "Pas de tracker detecte.", "Kein tracker erkannt."), DIM, BG, 1);
}

static void draw_info(void) {
    int y0 = app_ui_title("Sentinel", ACC, TR5("difesa", "defense", "defensa", "defense", "schutz"));
    txt(8, y0 + 6,  TR5("Rileva tracker che ti seguono.", "Detects trackers following you.", "Detecta trackers que te siguen.", "Detecte les trackers te suivant.", "Erkennt tracker die dir folgen."), FG, BG, 1);
    txt(8, y0 + 20, TR5("AirTag/FindMy, SmartTag, Tile,", "AirTag/FindMy, SmartTag, Tile,", "AirTag/FindMy, SmartTag, Tile,", "AirTag/FindMy, SmartTag, Tile,", "AirTag/FindMy, SmartTag, Tile,"), MUTED, BG, 1);
    txt(8, y0 + 32, TR5("Chipolo. Solo ascolto, zero TX.", "Chipolo. Listen only, zero TX.", "Chipolo. Solo escucha, cero TX.", "Chipolo. Ecoute seule, zero TX.", "Chipolo. Nur hoer, kein TX."), MUTED, BG, 1);
    txt(8, y0 + 50, TR5("<< = ti sta seguendo", "<< = following you", "<< = te sigue", "<< = te suivant", "<< = folgt dir"), ALERT, BG, 1);
    txt(8, y0 + 62, TR5(" ! = in modo smarrito", " ! = in lost mode", " ! = en modo perdido", " ! = en mode perdu", " ! = lost Modus"), WARN, BG, 1);
    txt(8, y0 + 80, TR5("tab: torna alla lista", "tab: back to the list", "tab: vuelve a lista", "tab: retour a liste", "tab: zurueck zu liste"), DIM, BG, 1);
}

static void on_draw(void) {
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    if (s_screen == INFO) draw_info(); else draw_list(h);
}

extern "C" void nucleo_register_sentinel(void) {
    static const nucleo_app_def_t app = {
        "sentinel", "Sentinel", "Security",
        "Anti-stalking tracker detector (BLE, passive)",
        'S', 0x2DEB, on_enter, nullptr, on_tick, on_draw, on_exit,
        NX_SOLO | NX_DEEP_OFFLINE | NX_BLE
    };
    nucleo_app_register(&app);
}
