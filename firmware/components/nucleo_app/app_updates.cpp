// app_updates — native release-update surface: the boot dialog (update / next boot / ignore)
// and the on-device installer with live progress. Engine in nucleo_update.c, decisions in
// update_policy.c (host-gated), download verified SHA-256 + rollback-covered.
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

#include <string.h>
#include <stdio.h>
#include "esp_app_desc.h"

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
static upd_phase_t s_last_phase = UPD_IDLE;
static int  s_last_pct = -2;

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
        nucleo_app_set_hint(PT("ENTER conferma  ESC annulla", "ENTER confirm  ESC cancel",
                               "ENTER confirmar  ESC cancelar", "ENTER confirmer  ESC annuler",
                               "ENTER bestaetigen  ESC abbrechen"));
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
static void start_install(void)
{
    // Reclaim ~47 KB (httpd + L1 + mDNS + voice; Wi-Fi stays up — we need it) only NOW, when the
    // user actually confirmed. The framework's close safety-net restores everything if we bail.
    nucleo_exclusive_info_t inf;
    nucleo_exclusive_enter(NX_NET_APP, &inf);
    s_exclusive = true;
    if (!nucleo_update_start()) {
        // The worker slot is held by the ~daily background check (a ~2 s window). Don't enter the
        // flash screen with no task behind it (that would sit at 0% forever): restore RAM and ask
        // the user to retry in a moment.
        nucleo_exclusive_exit(); s_exclusive = false;
        s_busy_retry = true; s_ui = UI_MAIN; set_hint_for_ui(); mark();
        return;
    }
    s_ui = UI_FLASH;
    set_hint_for_ui();
    mark();
}

static void act_row(int row)
{
    if (menu_rows() == 3) {
        if (row == 0) { s_ui = UI_CONFIRM; set_hint_for_ui(); mark(); }
        else if (row == 1) { nucleo_app_exit(); }                       // next boot: persist nothing
        else if (row == 2) { nucleo_update_dismiss_latest(); s_dismissed_now = true; s_sel = 0; set_hint_for_ui(); mark(); }
    } else {
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
        return true;                                 // mid-flash: never abandon the stream
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
        if (key == NK_ENTER) start_install();
    }
}

static void on_tick(void)
{
    // Redraw only when the engine state actually moved (phase or visible percent).
    nucleo_update_state_t st; nucleo_update_get_state(&st);
    if (st.phase != s_last_phase || st.pct != s_last_pct) {
        s_last_phase = st.phase; s_last_pct = st.pct;
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

static void draw_main(void)
{
    char cur[24]; cur_triplet(cur, sizeof cur);
    center(PT("AGGIORNAMENTI", "UPDATES", "ACTUALIZACIONES", "MISES A JOUR", "UPDATES"), 6, 1, MUTED);
    center(cur, 20, 2, FG);

    nucleo_update_state_t st; nucleo_update_get_state(&st);
    char line[64];
    const char *latest = nucleo_update_latest_tag();
    uint16_t scol = MUTED;
    if (s_busy_retry) { snprintf(line, sizeof line, "%s", PT("Controllo in corso, riprova", "A check is running, retry", "Comprobacion en curso, reintenta", "Verification en cours, reessayez", "Pruefung laeuft, erneut versuchen")); scol = C_YELLOW; }
    else if (st.phase == UPD_CHECKING) snprintf(line, sizeof line, "%s", PT("Controllo in corso...", "Checking...", "Comprobando...", "Verification...", "Pruefe..."));
    else if (st.phase == UPD_CHECK_FAIL) { snprintf(line, sizeof line, "%s", st.err); scol = C_YELLOW; }
    else if (update_available() && !s_dismissed_now) { snprintf(line, sizeof line, "%s %s", PT("Disponibile:", "Available:", "Disponible:", "Disponible :", "Verfuegbar:"), latest); scol = C_GREEN; }
    else if (latest[0]) snprintf(line, sizeof line, "%s (%s)", PT("Sei aggiornato", "Up to date", "Actualizado", "A jour", "Aktuell"), latest);
    else snprintf(line, sizeof line, "%s", PT("Nessun controllo ancora", "Never checked yet", "Sin comprobar aun", "Jamais verifie", "Noch nie geprueft"));
    center(line, 42, 1, scol);

    // Menu rows (selection bar + quick-select digits).
    const char *rows3[3] = {
        PT("Aggiorna ora", "Update now", "Actualizar ahora", "Mettre a jour", "Jetzt aktualisieren"),
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
    snprintf(msg, sizeof msg, "%s %s?", PT("Aggiornare a", "Update to", "Actualizar a", "Mettre a jour vers", "Aktualisieren auf"), nucleo_update_latest_tag());
    center(msg, 30, 2, FG);
    center(PT("Scarica l'immagine verificata e riavvia.", "Downloads the verified image and reboots.",
              "Descarga la imagen verificada y reinicia.", "Telecharge l'image verifiee et redemarre.",
              "Laedt das verifizierte Image und startet neu."), 62, 1, MUTED);
    center(PT("ENTER = Si", "ENTER = Yes", "ENTER = Si", "ENTREE = Oui", "ENTER = Ja"), 84, 1, C_GREEN);
}

static void draw_flash(void)
{
    nucleo_update_state_t st; nucleo_update_get_state(&st);
    if (st.phase == UPD_FAILED) {
        center(PT("AGGIORNAMENTO FALLITO", "UPDATE FAILED", "ACTUALIZACION FALLIDA", "MISE A JOUR ECHOUEE", "UPDATE FEHLGESCHLAGEN"), 24, 1, C_RED);
        center(st.err, 46, 1, FG);
        center(PT("ESC per uscire", "ESC to exit", "ESC para salir", "ESC pour quitter", "ESC zum Beenden"), 84, 1, MUTED);
        return;
    }
    if (st.phase == UPD_REBOOTING) {
        center(PT("Fatto! Riavvio...", "Done! Rebooting...", "Listo! Reiniciando...", "Termine ! Redemarrage...", "Fertig! Neustart..."), 52, 2, C_GREEN);
        return;
    }
    center(st.phase == UPD_VERIFYING
               ? PT("Verifica SHA-256...", "Verifying SHA-256...", "Verificando SHA-256...", "Verification SHA-256...", "Pruefe SHA-256...")
               : PT("Aggiornamento in corso", "Updating", "Actualizando", "Mise a jour", "Aktualisiere"),
           18, 2, FG);
    center(PT("NON SPEGNERE", "DO NOT POWER OFF", "NO APAGAR", "NE PAS ETEINDRE", "NICHT AUSSCHALTEN"), 44, 1, C_YELLOW);

    // Progress bar: frame always; fill by pct, or a barber-pole third when length is unknown.
    int bx = 20, by = 66, bw = W - 40, bh = 14;
    d.drawRoundRect(bx, by, bw, bh, 4, MUTED);
    if (st.phase == UPD_DOWNLOADING && st.pct >= 0) {
        int fill = (bw - 4) * st.pct / 100;
        if (fill > 1) d.fillRoundRect(bx + 2, by + 2, fill, bh - 4, 3, C_GREEN);
        char p[24]; snprintf(p, sizeof p, "%d%%  %d/%d KB", st.pct, st.recv_kb, st.total_kb);
        center(p, by + bh + 8, 1, MUTED);
    } else if (st.phase == UPD_DOWNLOADING) {
        int fill = (bw - 4) / 3, off = (st.recv_kb * 7) % (bw - 4 - fill);
        d.fillRoundRect(bx + 2 + off, by + 2, fill, bh - 4, 3, C_GREEN);
        char p[24]; snprintf(p, sizeof p, "%d KB", st.recv_kb);
        center(p, by + bh + 8, 1, MUTED);
    } else {
        d.fillRoundRect(bx + 2, by + 2, bw - 4, bh - 4, 3, C_GREEN);
    }
}

static void on_draw(void)
{
    if (!s_dirty) return;
    s_dirty = false;
    d.fillScreen(BG);
    if (s_ui == UI_MAIN) draw_main();
    else if (s_ui == UI_CONFIRM) draw_confirm();
    else draw_flash();
}

// ── lifecycle ─────────────────────────────────────────────────────────────────
static void on_enter(void)
{
    s_ui = UI_MAIN; s_sel = 0; s_dismissed_now = false; s_exclusive = false;
    s_last_phase = UPD_IDLE; s_last_pct = -2;
    nucleo_app_set_back_handler(on_back);
    set_hint_for_ui();
    mark();
}

static void on_exit(void)
{
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
    };
    nucleo_app_register(&app);
}
