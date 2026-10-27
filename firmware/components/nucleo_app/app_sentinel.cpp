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
#include "app_large.h"           // two screens: the tracker list + details on the 320x240 panel
#include "nucleo_touch.h"

static const unsigned short ACC = C_GREEN, WARN = C_YELLOW, ALERT = C_RED, SURF = 0x10A2;

enum Screen { LIST, INFO };
static Screen s_screen = LIST;
static bool   s_up_ok = false;
static int    s_sel = 0, s_lscroll = 0;   // selection + scroll for the two-screen list

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

// Selecting a tracker (so the panel's details card / the companion can show one). Taps arrive here as
// rows; up/down walk the list. Harmless on the Cardputer-only build (no visible selection there).
static void on_key(int key, char ch) {
    if (s_screen != LIST || !s_up_ok) return;
    int n = sentinel_tracker_count();
    if (n <= 0) return;
    if (nucleo_touch_is_row(key, ch)) { int i = s_lscroll + (ch - NUCLEO_TOUCH_ROW); if (i < n) s_sel = i; }
    else if (key == NK_UP)   s_sel = (s_sel + n - 1) % n;
    else if (key == NK_DOWN) s_sel = (s_sel + 1) % n;
    else return;
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

// ===== two screens ==============================================================================================
// The panel carries the defence dashboard: a following-alert banner, the scrollable tracker list on the left
// (tap a row to inspect it) and the picked tracker's details on the right. TAB (header menu glyph) flips to a
// full-panel "what this does" card. The Cardputer becomes the companion: the alert count big, the total, and
// the picked tracker — the glanceable summary while the panel shows everything.
static void lg_track_row(int i, int x, int y, int w, int h, bool focus, unsigned short bg, void *) {
    sentinel_view_t v;
    if (!sentinel_tracker_get(i, &v)) { lg::row_text(x, y, w, h, focus, bg, "-", nullptr, MUTED); return; }
    char right[20]; snprintf(right, sizeof right, "%ddB %lus", v.rssi, (unsigned long)v.age_s);
    unsigned short col = v.following ? ALERT : ((v.flags & 0x01) ? WARN : FG);
    char nm[32]; snprintf(nm, sizeof nm, "%s%s", v.following ? "<< " : "", sentinel_type_name(v.type));
    lg::row_text(x, y, w, h, focus, bg, nm, right, focus ? INK : col);
}

static void lg_info_panel(int x, int y, int w, int h) {
    lg::panel(x, y, w, h);
    const unsigned short cb = lg::card();
    lg::font(1); d.setTextColor(ACC, cb); d.drawString(TR("Che cosa fa", "What it does"), x + 12, y + 10);
    lg::font(0); d.setTextColor(FG, cb);
    int ty = y + 36;
    ty += 17 * lg::wrap(TR("Rileva i tracker Bluetooth che restano con te: AirTag/FindMy, SmartTag, Tile, Chipolo.",
                           "Detects Bluetooth trackers that stay with you: AirTag/FindMy, SmartTag, Tile, Chipolo."),
                        x + 12, ty, w - 24, 17, 3);
    ty += 8;
    d.setTextColor(ALERT, cb); d.drawString(TR("<<  ti sta seguendo", "<<  following you"), x + 12, ty); ty += 20;
    d.setTextColor(WARN, cb);  d.drawString(TR("!   in modo smarrito", "!   in lost mode"), x + 12, ty); ty += 24;
    lg::font(0); d.setTextColor(C_GREEN, cb);
    d.drawString(TR("Solo ascolto - zero TX", "Listen only - zero TX"), x + 12, ty);
}

static void draw_large(void) {
    const int Wp = d.width(), Hp = d.height();
    nucleo_touch_clear();
    const int following = s_up_ok ? sentinel_tracker_following() : 0;
    const int n = s_up_ok ? sentinel_tracker_count() : 0;
    const char *status = !s_up_ok ? TR("BT off", "BT off")
                       : nucleo_ble_is_synced() ? TR("scansione", "scanning") : TR("avvio...", "starting...");
    int y = lg::header("Sentinel", status, following ? ALERT : ACC, NK_BACK, NK_TAB);
    d.fillRect(0, y, Wp, Hp - y, BG);

    if (s_screen == INFO) { lg_info_panel(12, y + 6, Wp - 24, Hp - y - 14); nucleo_touch_commit(); return; }
    if (!s_up_ok) {
        lg::message(12, y + 6, Wp - 24, Hp - y - 14, TR("Bluetooth non attivo", "Bluetooth not active"),
                    TR("Riapri l'app per riprovare.", "Reopen the app to retry."), WARN);
        nucleo_touch_commit(); return;
    }
    if (n > 0 && s_sel >= n) s_sel = n - 1;
    if (following > 0) {   // the headline: someone is following you
        d.fillRoundRect(8, y + 4, Wp - 16, 30, 7, ALERT);
        char b[40]; snprintf(b, sizeof b, TR("!  %d tracker ti segue", "!  %d tracker following you"), following);
        lg::font(1); d.setTextColor(INK, ALERT); d.setTextDatum(middle_left); d.drawString(b, 20, y + 19); d.setTextDatum(top_left);
        y += 38;
    }
    const int LW = 178;
    if (n > 0) {
        lg::list(0, y + 2, LW, Hp - y - 6, lg::ROW, n, s_sel, &s_lscroll, lg_track_row, nullptr, ACC);
        // details card for the picked tracker
        sentinel_view_t v; const int dx = LW + 4, dw = Wp - LW - 10;
        lg::panel(dx, y + 6, dw, Hp - y - 14);
        const unsigned short cb = lg::card();
        if (sentinel_tracker_get(s_sel, &v)) {
            lg::font(1); d.setTextColor(v.following ? ALERT : ACC, cb);
            char nm[28]; lg::fit(nm, sizeof nm, sentinel_type_name(v.type), dw - 20);
            d.drawString(nm, dx + 10, y + 16);
            d.drawFastHLine(dx + 10, y + 38, dw - 20, LINE);
            int ky = y + 48;
            char val[16];
            snprintf(val, sizeof val, "%d dBm", v.rssi); lg::kv(dx + 10, ky, dw - 20, "RSSI", val, FG, cb); ky += 24;
            snprintf(val, sizeof val, "%lu s", (unsigned long)v.age_s); lg::kv(dx + 10, ky, dw - 20, TR("Visto da", "Seen for"), val, FG, cb); ky += 24;
            snprintf(val, sizeof val, "%u", v.hits); lg::kv(dx + 10, ky, dw - 20, TR("Avvist.", "Hits"), val, MUTED, cb); ky += 28;
            const char *st = v.following ? TR("TI SEGUE", "FOLLOWING") : (v.flags & 0x01) ? TR("smarrito", "lost mode") : TR("presente", "present");
            lg::font(1); d.setTextColor(v.following ? ALERT : ((v.flags & 0x01) ? WARN : C_GREEN), cb);
            d.drawString(st, dx + 10, ky);
        }
    } else {
        lg::message(0, y + 6, Wp, Hp - y - 14, TR("Nessun tracker", "No trackers"),
                    TR("Solo ascolto - zero TX.", "Listen only - zero TX."), C_GREEN);
    }
    nucleo_touch_commit();
}

static void draw_companion(void) {
    const int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    int y = lg::comp_head("SENTINEL", ACC);
    if (!s_up_ok) {
        d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(WARN, BG); d.drawString(TR("Bluetooth non attivo", "Bluetooth off"), 8, y + 8);
        d.setFont(&fonts::Font0); return;
    }
    const int following = sentinel_tracker_following(), n = sentinel_tracker_count();
    d.setFont(&fonts::FreeSansBold18pt7b); d.setTextColor(following ? ALERT : C_GREEN, BG);
    char big[8]; snprintf(big, sizeof big, "%d", following);
    d.drawString(big, 8, y + 2);
    d.setFont(&fonts::Font0); d.setTextColor(following ? ALERT : MUTED, BG);
    d.drawString(following ? TR("TI SEGUONO", "FOLLOWING") : TR("nessuno ti segue", "none following"), 54, y + 10);
    char m[28]; snprintf(m, sizeof m, TR("%d tracker vicini", "%d trackers nearby"), n);
    d.setTextColor(ACC, BG); d.drawString(m, 8, y + 44);
    sentinel_view_t v;
    if (n > 0 && sentinel_tracker_get(s_sel, &v)) {
        char ln[40]; snprintf(ln, sizeof ln, "> %s  %ddB", sentinel_type_name(v.type), v.rssi);
        d.setTextColor(FG, BG); d.drawString(ln, 8, y + 62);
    }
    lg::comp_keys(TR(";/. scegli  TAB info  ESC esci", ";/. pick  TAB info  ESC back"));
}

static void on_draw(void) {
    if (nucleo_app_two_screens()) { draw_companion(); return; }
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, 240, h, BG);
    if (s_screen == INFO) draw_info(); else draw_list(h);
}

extern "C" void nucleo_register_sentinel(void) {
    static const nucleo_app_def_t app = {
        "sentinel", "Sentinel", "Security",
        "Anti-stalking tracker detector (BLE, passive)",
        'S', 0x2DEB, on_enter, on_key, on_tick, on_draw, on_exit,
        NX_SOLO | NX_DEEP_OFFLINE | NX_BLE, nullptr, draw_large
    };
    nucleo_app_register(&app);
}
