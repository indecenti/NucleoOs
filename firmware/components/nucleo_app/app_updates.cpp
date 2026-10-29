// app_updates — native release-update surface: the boot dialog (how to update / next boot / ignore).
// Engine in nucleo_update.c, decisions in update_policy.c (host-gated).
//
// "Check now" lends the screen canvas to the TLS handshake (the one block of the ~22 KB it needs), as the
// daily background check does. The on-device download still can't run on this chip today: the release site
// sends 16 KB TLS records to an 8 KB receive buffer. So the dialog no longer offers a button that ends in "site unreachable": on a
// stand-alone unit it shows exactly where the update works — the web console's Settings > Updates, which
// verifies the image (SHA-256) and is covered by the automatic rollback; under M5Launcher, the Launcher's
// own OTA list. (The Solo install engine below stays for a future SD-staged installer.)
//
// Boot contract: the run loop launches this app when nucleo_update_dialog_pending() (NVS-only,
// no network). Esc = "show me again next boot" (nothing persisted); [2] persists the dismissal
// of THIS tag; [1] reclaims RAM (NX_NET_APP, runtime enter — the def carries no flags so merely
// LOOKING at the dialog never drops the web server) and streams the OTA image.
#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "launcher_theme.h"
#include "app_gfx.h"
#include "nucleo_update.h"
#include "update_policy.h"
#include "nucleo_exclusive.h"
#include "nucleo_i18n.h"
#include "nucleo_guest.h"   // installed by M5Launcher: no self-OTA, the update comes from Launcher's list
#include "app_large.h"      // two screens: the version card + big buttons on the panel, the Cardputer as companion
#include "nucleo_touch.h"

#include <string.h>
#include <stdio.h>
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Reboot-into-Solo (fresh, unfragmented heap) for the install — the outbound TLS + flash need a big
// contiguous block that only a fresh boot has. Provided by nucleo_app.cpp (link-time, no header dep).
extern "C" void nucleo_app_solo_request_id(const char *id);   // never returns (warm reboot)
extern "C" bool nucleo_anima_solo_active(void);               // true when this boot IS a Solo boot
extern "C" const char *nucleo_setup_ip(void);                // the device address, for the "update from the browser" card

// Five-language literal pick (pomodoro pattern; Font0 is ASCII so strings are accent-folded).
static const char *PT(const char *it, const char *en, const char *es, const char *fr, const char *de)
{
    const char *l = nucleo_i18n_lang();
    switch (l[0]) {
        case 'i': return it;
        case 'e': return l[1] == 's' ? es : en;
        case 'f': return fr;
        case 'd': return de;
        default:  return en;
    }
}

enum { UI_MAIN, UI_CONFIRM, UI_FLASH };
static int  s_ui = UI_MAIN;
static int  s_sel = 0;                 // selected menu row
static bool s_dirty = true;
static bool s_exclusive = false;       // we entered NX_NET_APP for the install
static bool s_dismissed_now = false;   // "ignore this version" chosen in this session
static bool s_busy_retry = false;      // install couldn't start (check running) — show a retry hint
static bool s_flash_dd = false;        // flash screen runs canvas-free + direct-draw (max heap for TLS)
static bool s_flash_bg = false;        // the static flash background has been painted once
static bool s_lent = false;            // "Check now": the canvas is lent to the TLS handshake until it ends
static upd_phase_t s_last_phase = UPD_IDLE;
static int  s_last_pct = -2;
static int  s_last_kb  = -1;            // also redraw on bytes-received change (live counter / barber-pole)

static void mark(void) { s_dirty = true; nucleo_app_request_draw(); }

// Current firmware, shortened to the triplet ("v0.2.11") — the full PROJECT_VER is wider than
// the screen at size 2 and the build metadata is noise here (About shows it in full).
static void cur_triplet(char *out, size_t cap)
{
    upd_semver_t v;
    const esp_app_desc_t *app = esp_app_get_description();
    if (app && upd_parse_semver(app->version, &v)) snprintf(out, cap, "v%d.%d.%d", v.maj, v.min, v.pat);
    else snprintf(out, cap, "%s", app ? app->version : "?");
}

static bool update_available(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    return upd_cmp(app ? app->version : "?", nucleo_update_latest_tag()) < 0;
}

static int menu_rows(void) { return update_available() && !s_dismissed_now ? 3 : 1; }

static void set_hint_for_ui(void)
{
    if (s_ui == UI_FLASH)
        nucleo_app_set_hint(PT("Non spegnere il dispositivo", "Do not power off the device",
                               "No apagues el dispositivo", "N'eteignez pas l'appareil",
                               "Geraet nicht ausschalten"));
    else if (s_ui == UI_CONFIRM)
        nucleo_app_set_hint(PT("ENTER / ESC indietro", "ENTER / ESC back",       // the card only informs
                               "ENTER / ESC volver", "ENTREE / ESC retour",
                               "ENTER / ESC zurueck"));
    else if (menu_rows() == 3)
        nucleo_app_set_hint(PT("1-3 scegli  ESC = al prossimo avvio", "1-3 pick  ESC = next boot",
                               "1-3 elige  ESC = proximo arranque", "1-3 choisir  ESC = prochain demarrage",
                               "1-3 waehlen  ESC = naechster Start"));
    else
        nucleo_app_set_hint(PT("ENTER controlla  ESC esci", "ENTER check  ESC exit",
                               "ENTER comprobar  ESC salir", "ENTER verifier  ESC quitter",
                               "ENTER pruefen  ESC beenden"));
}

// ── actions ───────────────────────────────────────────────────────────────────
// (No "update now" action: see the header — the dialog shows where the update works instead.)

// The REAL install, run only from the fresh-heap Solo boot. Free the 32 KB canvas for TLS+flash
// headroom (httpd/L1/mDNS aren't even up in Solo, so there's little else to reclaim) and draw the
// progress canvas-free (direct-draw, per-region self-clear — never fillScreen).
static void run_install_now(void)
{
    nucleo_app_release_buffers();
    nucleo_app_set_direct_draw(true);
    s_flash_dd = true; s_flash_bg = false;
    s_ui = UI_FLASH;
    set_hint_for_ui();
    if (!nucleo_update_start()) { s_busy_retry = true; s_ui = UI_MAIN; }
    mark();
}

static void act_row(int row)
{
    if (menu_rows() == 3) {
        if (row == 0) { s_ui = UI_CONFIRM; set_hint_for_ui(); mark(); }
        else if (row == 1) { nucleo_app_exit(); }                       // next boot: persist nothing
        else if (row == 2) { nucleo_update_dismiss_latest(); s_dismissed_now = true; s_sel = 0; set_hint_for_ui(); mark(); }
    } else {
        if (!s_lent) { nucleo_screen_hold(true); s_lent = true; }       // the handshake's block: the screen canvas
        if (!nucleo_update_kick_check(false)) s_busy_retry = true;      // a check is already running
        mark();
    }
}

// ── input ─────────────────────────────────────────────────────────────────────
static bool on_back(int key)
{
    (void)key;
    if (s_ui == UI_CONFIRM) { s_ui = UI_MAIN; set_hint_for_ui(); mark(); return true; }
    if (s_ui == UI_FLASH) {
        nucleo_update_state_t st; nucleo_update_get_state(&st);
        if (st.phase == UPD_FAILED) return false;    // failed: let Esc close (safety-net restores)
        // In the Solo install boot, Esc must always be able to bail: aborting mid-download is safe
        // (the OTA slot is only made bootable at the very end), and Esc warm-reboots back to the full
        // OS. This is the escape hatch if the fetch ever stalls, so the user is never trapped.
        if (nucleo_anima_solo_active()) return false;
        return true;                                 // full-OS mid-flash: never abandon the stream
    }
    return false;                                    // UI_MAIN: Esc = "again next boot", just leave
}

static void on_key(int key, char ch)
{
    if (s_busy_retry) { s_busy_retry = false; mark(); }   // any key clears the transient retry notice
    if (s_ui == UI_MAIN) {
        int rows = menu_rows();
        if (key == NK_UP)   { s_sel = (s_sel + rows - 1) % rows; mark(); }
        else if (key == NK_DOWN) { s_sel = (s_sel + 1) % rows; mark(); }
        else if (key == NK_ENTER) act_row(s_sel);
        else if (ch >= '1' && ch < (char)('1' + rows)) act_row(ch - '1');
    } else if (s_ui == UI_CONFIRM) {
        // The card only explains where the update works (browser, or M5Launcher); ENTER goes back.
        if (key == NK_ENTER) { s_ui = UI_MAIN; set_hint_for_ui(); mark(); }
    }
}

static void on_tick(void)
{
    // Redraw only when the engine state actually moved (phase or visible percent).
    nucleo_update_state_t st; nucleo_update_get_state(&st);
    if (st.phase != s_last_phase || st.pct != s_last_pct || st.recv_kb != s_last_kb) {
        s_last_phase = st.phase; s_last_pct = st.pct; s_last_kb = st.recv_kb;
        if (s_ui == UI_MAIN) set_hint_for_ui();      // a finished check can add/remove menu rows
        mark();
    }
}

// ── drawing (canvas mode: full repaints are SRAM writes, blitted atomically) ──
static void center(const char *s, int y, int size, uint16_t col)
{
    d.setTextSize(size); d.setTextColor(col, BG);
    d.setCursor((W - (int)strlen(s) * 6 * size) / 2, y);
    d.print(s);
}

static uint16_t upd_status_line(char *out, size_t cap);   // the status line (shared with the panel + companion)

static void draw_main(void)
{
    char cur[24]; cur_triplet(cur, sizeof cur);
    center(PT("AGGIORNAMENTI", "UPDATES", "ACTUALIZACIONES", "MISES A JOUR", "UPDATES"), 6, 1, MUTED);
    center(cur, 20, 2, FG);

    char line[64];
    const uint16_t scol = upd_status_line(line, sizeof line);
    center(line, 42, 1, scol);

    // Menu rows (selection bar + quick-select digits).
    const char *rows3[3] = {
        PT("Come aggiornare", "How to update", "Como actualizar", "Comment mettre a jour", "So aktualisieren"),
        PT("Al prossimo avvio", "Next boot", "Proximo arranque", "Prochain demarrage", "Naechster Start"),
        PT("Ignora questa versione", "Ignore this version", "Ignorar esta version", "Ignorer cette version", "Diese Version ignorieren"),
    };
    const char *row1 = PT("Controlla ora", "Check now", "Comprobar ahora", "Verifier maintenant", "Jetzt pruefen");
    int rows = menu_rows();
    int y0 = 58, rh = 20;
    for (int i = 0; i < rows; i++) {
        const char *txt = rows == 3 ? rows3[i] : row1;
        bool sel = (i == s_sel);
        if (sel) d.fillRoundRect(10, y0 + i * rh - 2, W - 20, rh - 2, 4, C_BLUE);
        d.setTextSize(1); d.setTextColor(sel ? BG : MUTED, sel ? C_BLUE : BG);
        d.setCursor(18, y0 + i * rh + 3); d.printf("%d", i + 1);
        d.setTextColor(sel ? BG : FG, sel ? C_BLUE : BG);
        d.setCursor(34, y0 + i * rh + 3); d.print(txt);
    }
}

static void draw_confirm(void)
{
    char msg[48];
    if (!nucleo_guest_self_ota_allowed()) {   // M5Launcher's OTA list owns updates — or a layout we don't know
        const bool launcher = nucleo_guest_hosted();
        snprintf(msg, sizeof msg, "%s %s", PT("Disponibile", "Available", "Disponible", "Disponible", "Verfuegbar"), nucleo_update_latest_tag());
        center(msg, 24, 2, FG);
        if (launcher) {
            // The Launcher's OTA list carries NucleoOS only once it is published on M5Burner; until then
            // (and always, as a sure path) the working way is the release's merged .bin on the SD, installed
            // from the Launcher's SD menu — exactly how a hosted unit is brought up. docs/m5launcher.md.
            center(PT("Aggiorna da M5Launcher:", "Update from M5Launcher:", "Actualiza desde M5Launcher:",
                      "Mettez a jour via M5Launcher :", "Update ueber M5Launcher:"), 50, 1, MUTED);
            center(PT(".bin della release sulla SD,", "release .bin on the SD card,", ".bin de la release en la SD,",
                      ".bin de la release sur la SD,", "Release-.bin auf die SD,"), 66, 1, FG);
            center(PT("poi SD > file > Install", "then SD > file > Install", "luego SD > archivo > Install",
                      "puis SD > fichier > Install", "dann SD > Datei > Install"), 80, 1, FG);
        } else {
            center(PT("Aggiorna via USB:", "Update over USB:", "Actualiza por USB:",
                      "Mettez a jour par USB :", "Update per USB:"), 54, 1, MUTED);
            center("indecenti.github.io/NucleoOs", 68, 1, FG);
        }
        center(PT("ENTER = OK", "ENTER = OK", "ENTER = OK", "ENTREE = OK", "ENTER = OK"), 100, 1, C_GREEN);
        return;
    }
    snprintf(msg, sizeof msg, "%s %s", PT("Disponibile", "Available", "Disponible", "Disponible", "Verfuegbar"), nucleo_update_latest_tag());
    center(msg, 20, 2, FG);
    center(PT("Aggiorna dal browser:", "Update from a browser:", "Actualiza desde un navegador:",
              "Mettez a jour depuis un navigateur :", "Update im Browser:"), 46, 1, MUTED);
    const char *ip = nucleo_setup_ip();
    char url[40];
    snprintf(url, sizeof url, "http://%s", (ip && ip[0]) ? ip : "192.168.4.1");   // hotspot: the AP address
    center(url, 60, 1, FG);
    center(PT("Impostazioni > Aggiornamenti", "Settings > Updates", "Ajustes > Actualizaciones",
              "Reglages > Mises a jour", "Einstellungen > Updates"), 74, 1, FG);
    center(PT("Verifica SHA-256, rollback automatico", "SHA-256 verified, automatic rollback",
              "SHA-256 verificado, rollback automatico", "SHA-256 verifie, rollback automatique",
              "SHA-256 geprueft, autom. Rollback"), 92, 1, MUTED);
    center(PT("ENTER = OK", "ENTER = OK", "ENTER = OK", "ENTREE = OK", "ENTER = OK"), 108, 1, C_GREEN);
}

// The flash screen runs canvas-free + direct-draw (the canvas RAM went to the TLS handshake). So it
// must NOT fillScreen on every frame (that flashes the panel). Paint the static chrome ONCE, then
// self-clear only the dynamic band (phase label + progress) each update — native-app anti-flicker #2.
static void draw_flash(void)
{
    nucleo_update_state_t st; nucleo_update_get_state(&st);
    static int s_flash_phase = -1;
    const int bx = 20, by = 66, bw = W - 40, bh = 14;

    if (!s_flash_bg) { d.fillScreen(BG); s_flash_bg = true; s_flash_phase = -1; }  // one-time clear on entry (canvas-free)

    // The whole band (y=8 down) is repainted ONCE per phase — NOT on every percent tick. Clearing it
    // each tick on the canvas-free DIRECT path flashed the entire "DO NOT POWER OFF" screen at the
    // download rate (native-app anti-flicker #2). Within a phase, only the bar fill + counter update,
    // each in its own small box.
    bool newphase = (st.phase != s_flash_phase);
    if (newphase) {
        s_flash_phase = st.phase;
        d.fillRect(0, 8, W, H - 8, BG);
        if (st.phase == UPD_FAILED) {
            center(PT("AGGIORNAMENTO FALLITO", "UPDATE FAILED", "ACTUALIZACION FALLIDA", "MISE A JOUR ECHOUEE", "UPDATE FEHLGESCHLAGEN"), 24, 1, C_RED);
            center(st.err, 46, 1, FG);
            center(PT("ESC per uscire", "ESC to exit", "ESC para salir", "ESC pour quitter", "ESC zum Beenden"), 84, 1, MUTED);
        } else if (st.phase == UPD_REBOOTING) {
            center(PT("Fatto! Riavvio...", "Done! Rebooting...", "Listo! Reiniciando...", "Termine ! Redemarrage...", "Fertig! Neustart..."), 52, 2, C_GREEN);
        } else {
            center(st.phase == UPD_VERIFYING
                       ? PT("Verifica SHA-256...", "Verifying SHA-256...", "Verificando SHA-256...", "Verification SHA-256...", "Pruefe SHA-256...")
                       : PT("Aggiornamento in corso", "Updating", "Actualizando", "Mise a jour", "Aktualisiere"),
                   18, 2, FG);
            center(PT("NON SPEGNERE", "DO NOT POWER OFF", "NO APAGAR", "NE PAS ETEINDRE", "NICHT AUSSCHALTEN"), 44, 1, C_YELLOW);
            d.drawRoundRect(bx, by, bw, bh, 4, MUTED);                                  // static bar frame
            if (st.phase == UPD_VERIFYING) d.fillRoundRect(bx + 2, by + 2, bw - 4, bh - 4, 3, C_GREEN);  // verify: full, static
        }
    }
    if (st.phase != UPD_DOWNLOADING) return;   // only the download phase has per-tick dynamics

    // Dynamic: progress fill + counter, each self-clearing its own box (no full-band wipe).
    if (st.pct >= 0) {
        int fill = (bw - 4) * st.pct / 100;
        if (fill > 1) d.fillRoundRect(bx + 2, by + 2, fill, bh - 4, 3, C_GREEN);        // grows over prior fill
        char p[24]; snprintf(p, sizeof p, "%d%%  %d/%d KB", st.pct, st.recv_kb, st.total_kb);
        d.fillRect(0, by + bh + 6, W, 12, BG); center(p, by + bh + 8, 1, MUTED);        // counter (width varies)
    } else {                                                                             // unknown length: barber-pole
        int fill = (bw - 4) / 3, off = (st.recv_kb * 7) % (bw - 4 - fill);
        d.fillRect(bx + 2, by + 2, bw - 4, bh - 4, BG);                                  // clear bar interior (segment moves)
        d.fillRoundRect(bx + 2 + off, by + 2, fill, bh - 4, 3, C_GREEN);
        char p[24]; snprintf(p, sizeof p, "%d KB", st.recv_kb);
        d.fillRect(0, by + bh + 6, W, 12, BG); center(p, by + bh + 8, 1, MUTED);
    }
}

// ===== two screens ==============================================================================================
// The panel carries the update as one card (see draw_large); the flash progress gets its own panel layout. The Cardputer becomes the companion: version, status, key map.
static uint16_t upd_status_line(char *out, size_t cap)
{
    nucleo_update_state_t st; nucleo_update_get_state(&st);
    const char *latest = nucleo_update_latest_tag();
    uint16_t scol = MUTED;
    if (s_busy_retry) { snprintf(out, cap, "%s", PT("Controllo in corso, riprova", "A check is running, retry", "Comprobacion en curso, reintenta", "Verification en cours, reessayez", "Pruefung laeuft, erneut versuchen")); scol = C_YELLOW; }
    else if (st.phase == UPD_CHECKING) snprintf(out, cap, "%s", PT("Controllo in corso...", "Checking...", "Comprobando...", "Verification...", "Pruefe..."));
    else if (st.phase == UPD_CHECK_FAIL) { snprintf(out, cap, "%s", st.err); scol = C_YELLOW; }
    else if (update_available() && !s_dismissed_now) { snprintf(out, cap, "%s %s", PT("Disponibile:", "Available:", "Disponible:", "Disponible :", "Verfuegbar:"), latest); scol = C_GREEN; }
    else if (latest[0]) snprintf(out, cap, "%s (%s)", PT("Sei aggiornato", "Up to date", "Actualizado", "A jour", "Aktuell"), latest);
    else snprintf(out, cap, "%s", PT("Nessun controllo ancora", "Never checked yet", "Sin comprobar aun", "Jamais verifie", "Noch nie geprueft"));
    return scol;
}

// Where the update works for THIS unit, inside the card at (x, y), w wide: the browser console (its address big),
// or, hosted by M5Launcher / on a layout without self-OTA, the Launcher's SD install or the USB web flasher.
static void lg_how(int x, int y, int w, unsigned short cb)
{
    lg::font(1); d.setTextColor(FG, cb);
    if (!nucleo_guest_self_ota_allowed()) {
        if (nucleo_guest_hosted()) {
            d.drawString(PT("Aggiorna da M5Launcher:", "Update from M5Launcher:", "Actualiza desde M5Launcher:", "Mettez a jour via M5Launcher :", "Update ueber M5Launcher:"), x, y);
            lg::font(0); d.setTextColor(MUTED, cb);
            lg::wrap(PT(".bin della release sulla SD, poi SD > file > Install", "release .bin on the SD card, then SD > file > Install", ".bin de la release en la SD, luego SD > archivo > Install", ".bin de la release sur la SD, puis SD > fichier > Install", "Release-.bin auf die SD, dann SD > Datei > Install"), x, y + 26, w, 18, 3);
        } else {
            d.drawString(PT("Aggiorna via USB:", "Update over USB:", "Actualiza por USB:", "Mettez a jour par USB :", "Update per USB:"), x, y);
            d.setTextColor(C_BLUE, cb); char u[40]; lg::fit(u, sizeof u, "indecenti.github.io/NucleoOs", w); d.drawString(u, x, y + 28);
        }
        return;
    }
    d.drawString(PT("Aggiorna dal browser:", "Update from a browser:", "Actualiza desde un navegador:", "Mettez a jour depuis un navigateur :", "Update im Browser:"), x, y);
    const char *ip = nucleo_setup_ip();
    char url[48]; snprintf(url, sizeof url, "http://%s", (ip && ip[0]) ? ip : "192.168.4.1");
    lg::font(2); d.setTextColor(C_BLUE, cb); char uu[48]; lg::fit(uu, sizeof uu, url, w); d.drawString(uu, x, y + 22);
    lg::font(0); d.setTextColor(FG, cb);
    d.drawString(PT("Impostazioni > Aggiornamenti", "Settings > Updates", "Ajustes > Actualizaciones", "Reglages > Mises a jour", "Einstellungen > Updates"), x, y + 50);
    d.setTextColor(MUTED, cb);
    d.drawString(PT("Verifica SHA-256, rollback automatico", "SHA-256 verified, automatic rollback", "SHA-256 verificado, rollback automatico", "SHA-256 verifie, rollback automatique", "SHA-256 geprueft, autom. Rollback"), x, y + 68);
}

static void draw_flash_large(void)
{
    const int W = d.width(), H = d.height();
    nucleo_update_state_t st; nucleo_update_get_state(&st);
    d.fillRect(0, 0, W, H, BG);
    if (st.phase == UPD_FAILED) { lg::message(0, 0, W, H, PT("AGGIORNAMENTO FALLITO", "UPDATE FAILED", "ACTUALIZACION FALLIDA", "MISE A JOUR ECHOUEE", "UPDATE FEHLGESCHLAGEN"), st.err, C_RED); return; }
    if (st.phase == UPD_REBOOTING) { lg::message(0, 0, W, H, PT("Fatto! Riavvio...", "Done! Rebooting...", "Listo! Reiniciando...", "Termine ! Redemarrage...", "Fertig! Neustart..."), nullptr, C_GREEN); return; }
    lg::font(2); d.setTextDatum(top_center); d.setTextColor(FG, BG);
    d.drawString(st.phase == UPD_VERIFYING ? PT("Verifica SHA-256...", "Verifying SHA-256...", "Verificando SHA-256...", "Verification SHA-256...", "Pruefe SHA-256...")
                                           : PT("Aggiornamento in corso", "Updating", "Actualizando", "Mise a jour", "Aktualisiere"), W / 2, 44);
    lg::font(1); d.setTextColor(C_YELLOW, BG); d.drawString(PT("NON SPEGNERE", "DO NOT POWER OFF", "NO APAGAR", "NE PAS ETEINDRE", "NICHT AUSSCHALTEN"), W / 2, 84);
    d.setTextDatum(top_left);
    lg::bar(30, 124, W - 60, 16, st.phase == UPD_VERIFYING ? 100 : (st.pct < 0 ? 0 : st.pct), C_GREEN);
    if (st.phase == UPD_DOWNLOADING) {
        lg::font(0); d.setTextDatum(top_center); d.setTextColor(MUTED, BG);
        char p[32]; snprintf(p, sizeof p, "%d%%  %d/%d KB", st.pct < 0 ? 0 : st.pct, st.recv_kb, st.total_kb);
        d.drawString(p, W / 2, 150); d.setTextDatum(top_left);
    }
}

// One card: the version you have -> the one available, where the update works (the "how to update" text lives
// in the card, so UI_CONFIRM shows the same card), and the two choices as the card's own actions. Up to date: the
// version as a hero number, the status line and a Check now action.
static void draw_large(void)
{
    const int W = d.width(), H = d.height();
    nucleo_touch_clear();
    if (s_ui == UI_FLASH) { draw_flash_large(); nucleo_touch_commit(); return; }
    char cur[24]; cur_triplet(cur, sizeof cur);
    const int y = lg::header(PT("Aggiornamenti", "Updates", "Actualizaciones", "Mises a jour", "Updates"), nullptr, C_GREEN);
    d.fillRect(0, y, W, H - y, BG);
    const int cx = 10, cy = y + 6, cw = W - 20, chh = H - cy - 8, tx = cx + 16;
    lg::panel(cx, cy, cw, chh);
    const unsigned short cb = lg::card();
    if (menu_rows() == 3) {
        lg::font(2); d.setTextColor(MUTED, cb); d.drawString(cur, tx, cy + 16);
        const int ax = tx + d.textWidth(cur) + 10;
        d.fillTriangle(ax, cy + 18, ax, cy + 34, ax + 9, cy + 26, MUTED);
        lg::font(3); d.setTextColor(C_GREEN, cb); d.drawString(nucleo_update_latest_tag(), ax + 18, cy + 10);
        d.drawFastHLine(tx, cy + 48, cw - 32, LINE);
        lg_how(tx, cy + 58, cw - 32, cb);
        if (s_sel == 0) d.drawRoundRect(tx - 8, cy + 52, cw - 16, 92, 8, C_GREEN);   // the keyboard's focus
        const char *act[2] = { PT("Al riavvio", "Next boot", "Al reiniciar", "Au redemarrage", "Naechster Start"),
                               PT("Ignora", "Ignore", "Ignorar", "Ignorer", "Ignorieren") };
        const int bw = (cw - 30) / 2;
        for (int i = 0; i < 2; i++) {
            const bool sel = s_sel == i + 1;
            lg::button(cx + 10 + i * (bw + 10), cy + chh - 50, bw, 40, act[i], sel ? C_GREEN : LINE, sel ? INK : FG, NK_CHAR, (char)('2' + i));
        }
    } else {
        lg::font(3); d.setTextDatum(top_center); d.setTextColor(FG, cb); d.drawString(cur, W / 2, cy + 22);
        char line[64]; const uint16_t scol = upd_status_line(line, sizeof line);
        lg::font(1); d.setTextColor(scol, cb); char sl[64]; lg::fit(sl, sizeof sl, line, cw - 24); d.drawString(sl, W / 2, cy + 66);
        d.setTextDatum(top_left);
        lg::button(cx + 30, cy + chh - 60, cw - 60, 46, PT("Controlla ora", "Check now", "Comprobar ahora", "Verifier maintenant", "Jetzt pruefen"), C_GREEN, INK, NK_CHAR, '1');
    }
    nucleo_touch_commit();
}

static void draw_companion(void)
{
    const int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, W, h, BG);
    const int y = lg::comp_head("UPDATES", C_GREEN);
    if (s_ui == UI_FLASH) {
        nucleo_update_state_t st; nucleo_update_get_state(&st);
        d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(st.phase == UPD_FAILED ? C_RED : C_YELLOW, BG);
        d.drawString(st.phase == UPD_FAILED ? PT("Fallito", "Failed", "Fallida", "Echouee", "Fehlgeschlagen")
                                            : PT("Non spegnere", "Do not power off", "No apagar", "Ne pas eteindre", "Nicht ausschalten"), 8, y + 4);
        d.setFont(&fonts::Font0); d.setTextColor(FG, BG);
        char p[40]; if (st.pct >= 0) snprintf(p, sizeof p, "%d%%  %d/%d KB", st.pct, st.recv_kb, st.total_kb); else snprintf(p, sizeof p, "%d KB", st.recv_kb);
        d.drawString(p, 8, y + 28);
        lg::comp_keys("");
        return;
    }
    char cur[24]; cur_triplet(cur, sizeof cur);
    d.setFont(&fonts::FreeSansBold12pt7b); d.setTextColor(FG, BG); d.drawString(cur, 8, y + 6);
    char line[64]; const uint16_t scol = upd_status_line(line, sizeof line);
    d.setFont(&fonts::Font0); d.setTextColor(scol, BG); char f[44]; snprintf(f, sizeof f, "%.40s", line); d.drawString(f, 8, y + 40);
    lg::comp_keys(s_ui == UI_CONFIRM ? PT("INVIO ok  ESC indietro", "ENTER ok  ESC back", "INTRO ok  ESC atras", "ENTREE ok  ESC retour", "ENTER ok  ESC zurueck")
                : menu_rows() == 3 ? PT("1-3 scegli  ESC prossimo avvio", "1-3 pick  ESC next boot", "1-3 elige  ESC prox arranque", "1-3 choisir  ESC demarrage", "1-3 waehlen  ESC Start")
                : PT("INVIO controlla  ESC esci", "ENTER check  ESC exit", "INTRO comprobar  ESC salir", "ENTREE verifier  ESC quitter", "ENTER pruefen  ESC beenden"));
}

static void canvas_back(void)
{
    if (!s_lent) return;
    s_lent = false;
    nucleo_screen_hold(false);
    for (int i = 0; i < 6 && !nucleo_screen_acquire(); i++) vTaskDelay(pdMS_TO_TICKS(20));
    nucleo_app_force_repaint();
}

static void on_draw(void)
{
    if (s_lent && !nucleo_update_busy()) canvas_back();
    if (!s_dirty) return;
    s_dirty = false;
    if (nucleo_app_two_screens()) { draw_companion(); return; }
    if (s_ui == UI_FLASH) { draw_flash(); return; }   // owns its own clears (canvas-free direct-draw)
    d.fillScreen(BG);
    if (s_ui == UI_MAIN) draw_main();
    else draw_confirm();
}

// ── lifecycle ─────────────────────────────────────────────────────────────────
static void on_enter(void)
{
    s_ui = UI_MAIN; s_sel = 0; s_dismissed_now = false; s_exclusive = false;
    s_last_phase = UPD_IDLE; s_last_pct = -2;
    s_flash_dd = false; s_flash_bg = false; s_busy_retry = false;
    nucleo_app_set_back_handler(on_back);
    // Launched by a fresh-heap Solo boot to DO the install (armed by "Update now")? Run it now.
    if (nucleo_anima_solo_active() && nucleo_update_boot_install_armed()) {
        nucleo_update_disarm_boot_install();   // consume once — a failed install won't loop-reboot
        run_install_now();
        return;
    }
    set_hint_for_ui();
    mark();
}

static void on_exit(void)
{
    canvas_back();
    // Leave direct-draw mode so the framework re-acquires the canvas for the next app.
    if (s_flash_dd) { nucleo_app_set_direct_draw(false); s_flash_dd = false; s_flash_bg = false; }
    // Restore the reclaimed subsystems if an install bailed out (success path never returns —
    // the engine reboots). The framework's close safety-net would also catch this; being explicit
    // keeps the pairing local and obvious.
    if (s_exclusive) { nucleo_exclusive_exit(); s_exclusive = false; }
}

extern "C" void nucleo_register_updates(void)
{
    static const nucleo_app_def_t app = {
        "updates",
        "Updates",
        "System",
        "Check and install NucleoOS releases",
        'U', C_GREEN,
        on_enter, on_key, on_tick, on_draw, on_exit,
        0, nullptr, draw_large,
    };
    nucleo_app_register(&app);
}
