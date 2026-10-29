// Launcher rendering. See launcher_render.h.
#include "launcher_render.h"
#include "launcher_menu.h"
#include "nucleo_kbd.h"
#include <M5GFX.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include "esp_system.h"      // esp_restart() for the Control Center's Restart shortcut
#include "esp_timer.h"       // esp_timer_get_time() for the frame-rate-independent carousel ease
#include "nucleo_i18n.h"     // system language (TR/it-en) for the Control Center labels
#include "ui_glyph.h"         // the quick menu's row glyphs (also the Control Center's, further down)
#include "nucleo_touch.h"    // the large Control Center: tap a tile / shortcut
#include "nucleo_extdisp.h"  // the second screen's own backlight slider

// The real display (defined in nucleo_ui.cpp). The launcher always draws to it directly;
// only the animated list band is composited off-screen first. We do NOT include app_gfx.h
// here on purpose — that header redefines `d` as the movable app-draw target, but the
// launcher chrome must always hit the physical screen.
extern M5GFX d;

// Apps installed from the Store carry a 32x32 drawn icon (store_apps.cpp); the launcher paints it in the
// app's colour. Weak so host harnesses that don't link store_apps.cpp still build (returns none there).
extern "C" const uint8_t *nucleo_store_app_icon(const char *id) __attribute__((weak));

// Shared off-screen back-buffer (nucleo_ui.cpp). The launcher list band composites into it and
// blits the band region with a clipped push; apps reuse the same canvas. See app_gfx.h.
M5Canvas *nucleo_screen(void);
void      nucleo_screen_release(void);
M5Canvas *nucleo_screen_wide(int w);   // the same bytes seen w px wide: the 320-wide strips of the large surface

// Geometry follows the live surface: the internal 240x135, or 320x240 while an external panel hosts the
// launcher natively (nucleo_surface.h). W and H are read at draw time, so every layout below adapts; the
// Control Center and the other compact screens only ever run on the compact surface (= 240x135).
#define W ((int)::d.width())
#define H ((int)::d.height())
static inline bool lr_large(void) { return ::d.width() >= 300; }
// The large surface: a taller status bar and no hint bar (the whole height goes to the carousel); BY
// centres the status bar's contents in whatever height it has.
#undef BAR
#undef HINT
#define BAR  (lr_large() ? 30 : 16)                  // large: a finger-sized bar (Back button, tap = Control Center)
#define HINT (lr_large() ? 0 : 14)
#define BY   ((BAR - 16) / 2)

// Network info + pairing PIN (resolved at link; no component dependency).
extern "C" const char *nucleo_setup_mode(void);        // "sta" | "ap"
extern "C" const char *nucleo_setup_ssid(void);
extern "C" const char *nucleo_setup_ip(void);
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
extern "C" void nucleo_app_persist_prefs(void);   // save brightness/volume/mute to settings.json
extern "C" int  nucleo_audio_volume(void);
extern "C" void nucleo_audio_set_volume(int pct);
extern "C" bool nucleo_audio_is_muted(void);
extern "C" void nucleo_audio_set_mute(bool muted);
extern "C" bool nucleo_audio_is_playing(void);
extern "C" bool nucleo_audio_is_paused(void);
extern "C" const char *nucleo_audio_path(void);
extern "C" int  nucleo_power_battery_pct(void);             // 0..100, -1 = unavailable

// System hooks (nucleo_setup — resolved at final link, no cycle).
extern "C" int  nucleo_setup_rssi(void);
extern "C" bool nucleo_setup_time_synced(void);

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

    // Hint line: the controls that actually do something right now. Arrows move the focus; Esc goes
    // back; ENTER opens. When an app is focused, '*' pins/unpins it to the top of Home (ANIMA excluded,
    // it already lives there). s_hint is 48 B; the longest string below is ~30 chars, well within 240 px.
    bool app = cur && cur->kind == N_APP && strcmp(cur->id, "anima") && strcmp(cur->id, "store") && strcmp(cur->id, LAUNCHER_SETTINGS_SEARCH_ID);
    const char *pinw = app ? (launcher_is_pinned(cur->id) ? TR5("* togli", "* unpin", "* soltar", "* retirer", "* loesen") : TR5("* fissa", "* pin", "* fijar", "* epingler", "* anheften")) : "";
    if (launcher_filter()[0]) {
        snprintf(s_hint, sizeof(s_hint), TR5("cerca \"%.12s\"   esc azzera", "find \"%.12s\"   esc clear", "buscar \"%.12s\"   esc borrar",
                                               "chercher \"%.12s\"  esc effacer", "suche \"%.12s\"   esc leeren"), launcher_filter());
    } else if (launcher_depth() > 0) {
        if (app) snprintf(s_hint, sizeof(s_hint), TR5("invio apri   %s   esc indietro", "enter open   %s   esc back", "enter abrir   %s   esc atras",
                                         "enter ouvrir   %s   esc retour", "enter oeffnen  %s  esc zurueck"), pinw);
        else     snprintf(s_hint, sizeof(s_hint), "%s", TR5("invio apri   esc indietro", "enter open   esc back", "enter abrir   esc atras",
                                                         "enter ouvrir   esc retour", "enter oeffnen   esc zurueck"));
    } else {
        if (app) snprintf(s_hint, sizeof(s_hint), TR5("invio apri   %s   tab rapide", "enter open   %s   tab quick", "enter abrir   %s   tab rapido",
                                         "enter ouvrir   %s   tab rapide", "enter oeffnen  %s  tab schnell"), pinw);
        else     snprintf(s_hint, sizeof(s_hint), "%s", TR5("invio apri   digita cerca   tab rapide", "enter open   type to find   tab quick",
                                                         "enter abrir   teclea busca   tab rapido", "enter ouvrir  tape cherche  tab rapide",
                                                         "enter oeffnen  tippe sucht  tab schnell"));   // teach Spotlight + the Control Center
    }
    s_hint[sizeof(s_hint) - 1] = 0;
}

// Single RSSI->bars map (0..4) so the top-bar Wi-Fi gauge, the chrome-change signature and the
// Control Center Signal row can never disagree — they used to carry three different threshold sets
// (the top bar and CC showed a different bar count for the same signal). rssi==0 = no reading -> 0
// bars; callers that want an "AP mode / associating" glyph substitute a single amber bar themselves.
static int wifi_bars(int rssi)
{
    if (rssi == 0)   return 0;
    if (rssi >= -55) return 4;
    if (rssi >= -67) return 3;
    if (rssi >= -78) return 2;
    return 1;
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
    int  wlvl = (!sta || rssi == 0) ? 1 : wifi_bars(rssi);   // same map as draw_wifi (shared helper)
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

// Localized DISPLAY label for a launcher node. Category tiles carry an English id used for
// bucketing + icon lookup (never change it); show them in the user's language here. Apps keep
// their own name (author's choice, already per-app). Display-only — the id/glyph are untouched,
// so this also renders "Hardware" as Sensori/Sensors without a rename. Zero RAM (flash literals).
// App display names in BOTH languages. Registrations historically carry a single `name` (a mix of IT
// and EN); this is the ONE place that makes the launcher title bilingual — no touching 61 app
// registrations, zero per-app RAM (flash const), and live (node_label picks at render). Only apps whose
// name differs by language need a row; pure proper nouns (ANIMA, Mail, SSH, Pong, BLE, Snake, Yahtzee,
// Tanks, Tank Duel, Payloads, IR Remote, Evil Portal, Deauth Flood, WiFi Sniffer, …) fall through to
// the app's own name.
// App display names in all five shipped languages (it/en/es/fr/de). ASCII only — the on-TFT font has
// no accented glyphs, so French/German accents are dropped (Chronometre, Wuerfel) rather than rendered
// as tofu. Game names and brand-ish words stay close across languages on purpose.
static const struct { const char *id, *it, *en, *es, *fr, *de; } APP_NAME_TR[] = {
    { "alarm",       "Allarme",             "Alarm",          "Alarma",         "Alarme",           "Wecker" },
    { "brawler",     "Scorribanda",         "Brawl",          "Pelea",          "Bagarre",          "Keilerei" },
    { "calc",        "Calcolatrice",        "Calculator",     "Calculadora",    "Calculatrice",     "Rechner" },
    { "calendar",    "Calendario",          "Calendar",       "Calendario",     "Calendrier",       "Kalender" },
    { "chrono",      "Cronometro",          "Stopwatch",      "Cronometro",     "Chronometre",      "Stoppuhr" },
    { "clock",       "Orologio",            "Clock",          "Reloj",          "Horloge",          "Uhr" },
    { "dice",        "Dadi",                "Dice",           "Dados",          "Des",              "Wuerfel" },
    { "files",       "File",                "Files",          "Archivos",       "Fichiers",         "Dateien" },
    { "giardino",    "Giardino",            "Sand Garden",    "Jardin",         "Jardin",           "Sandgarten" },
    { "goniometer",  "Goniometro",          "Protractor",     "Transportador",  "Rapporteur",       "Winkelmesser" },
    { "info",        "Connessione",         "Connection",     "Conexion",       "Connexion",        "Verbindung" },
    { "ir",          "Telecomando IR",      "IR Remote",      "Mando IR",       "Telecommande IR",  "IR-Fernbedienung" },
    { "level",       "Livella",             "Level",          "Nivel",          "Niveau",           "Wasserwaage" },
    { "link",        "Vicino",              "Nearby",         "Cerca",          "Proche",           "Nahe" },
    { "mail",        "Posta",               "Mail",           "Correo",         "Courrier",         "Mail" },
    { "micspec",     "Spettro Mic",         "Mic Spectrum",   "Espectro Mic",   "Spectre Mic",      "Mik-Spektrum" },
    { "music",       "Musica",              "Music",          "Musica",         "Musique",          "Musik" },
    { "notepad",     "Note",                "Notes",          "Notas",          "Notes",            "Notizen" },
    { "notify",      "Notifiche",           "Notifications",  "Notificaciones", "Notifications",    "Hinweise" },
    { "orde",        "Orde",                "Hordes",         "Hordas",         "Hordes",           "Horden" },
    { "pedometer",   "Contapassi",          "Pedometer",      "Podometro",      "Podometre",        "Schrittzaehler" },
    { "photos",      "Foto",                "Photos",         "Fotos",          "Photos",           "Fotos" },
    { "presence",    "Presenza",            "Presence",       "Presencia",      "Presence",         "Praesenz" },
    { "pinball",     "Flipper",             "Pinball",        "Pinball",        "Flipper",          "Flipper" },
    { "qr",          "Codice QR",           "QR Code",        "Codigo QR",      "Code QR",          "QR-Code" },
    { "radio",       "Radio",               "Radio",          "Radio",          "Radio",            "Radio" },
    { "reactor",     "Reattore",            "Reactor",        "Reactor",        "Reacteur",         "Reaktor" },
    { "recorder",    "Registratore vocale", "Voice Recorder", "Grabadora",      "Dictaphone",       "Rekorder" },
    { "remote",      "Controllo remoto",    "Remote Control", "Control remoto", "Controle distant", "Fernsteuerung" },
    { "screensaver", "Salvaschermo",        "Screensaver",    "Salvapantallas", "Economiseur",      "Bildschirmschoner" },
    { "slots",       "Slot",                "Slots",          "Tragaperras",    "Machines",         "Automat" },
    { "snake",       "Serpente",            "Snake",          "Serpiente",      "Serpent",          "Schlange" },
    { "stelle",      "Costellazioni",       "Constellations", "Constelaciones", "Constellations",   "Sternbilder" },
    { "store",       "Store",               "Store",          "Tienda",         "Boutique",         "Store" },
    { "swarm",       "Sciame",              "Swarm",          "Enjambre",       "Essaim",           "Schwarm" },
    { "sysmon",      "Stato sistema",       "System Status",  "Estado sistema", "Etat systeme",     "Systemstatus" },
    { "tankd",       "Duello Carri",        "Tank Duel",      "Duelo Tanques",  "Duel de Chars",    "Panzerduell" },
    { "tanks",       "Carri",               "Tanks",          "Tanques",        "Chars",            "Panzer" },
    { "theme",       "Tema",                "Theme",          "Tema",           "Theme",            "Design" },
    { "torch",       "Torcia",              "Torch",          "Linterna",       "Lampe",            "Taschenlampe" },
    { "usb",         "Unita USB",           "USB Drive",      "Disco USB",      "Disque USB",       "USB-Disk" },
    { "usbweb",      "Web via USB",         "Web via USB",    "Web por USB",    "Web via USB",      "Web ueber USB" },
    { "usbkbd",      "Tastiera USB",        "USB Keyboard",   "Teclado USB",    "Clavier USB",      "USB-Tastatur" },
    { "voice",       "Trainer vocale",      "Voice Trainer",  "Entrenador voz", "Coach vocal",      "Sprachtrainer" },
    { "voicelab",    "Laboratorio voce",    "Voice Lab",      "Lab de voz",     "Labo vocal",       "Sprachlabor" },
    { "weather",     "Meteo",               "Weather",        "Tiempo",         "Meteo",            "Wetter" },
    { "wifi",        "Impostazioni",        "Settings",       "Ajustes",        "Reglages",         "Einstellungen" },
};

// Localized launcher name for an app id in the ACTIVE language, or NULL when the app uses its own
// (language-neutral) name. Shared with the menu filter so Spotlight search matches the name shown in
// the current language too — not only the app's default-language spelling.
// An app's title in one of the five shipped languages (0 it, 1 en, 2 es, 3 fr, 4 de), or NULL when the
// app has no translated title (proper nouns). Spotlight (launcher_menu.cpp) scores a query against all
// five, so an app is found by any name a user knows it by, whatever the OS language.
extern "C" const char *launcher_app_title(const char *id, int lang)
{
    for (unsigned i = 0; i < sizeof APP_NAME_TR / sizeof APP_NAME_TR[0]; i++) {
        if (strcmp(id, APP_NAME_TR[i].id)) continue;
        switch (lang) {
            case 0: return APP_NAME_TR[i].it;
            case 1: return APP_NAME_TR[i].en;
            case 2: return APP_NAME_TR[i].es;
            case 3: return APP_NAME_TR[i].fr;
            case 4: return APP_NAME_TR[i].de;
            default: return nullptr;
        }
    }
    return nullptr;
}

extern "C" const char *launcher_app_localized_name(const char *id)
{
    for (unsigned i = 0; i < sizeof APP_NAME_TR / sizeof APP_NAME_TR[0]; i++)
        if (!strcmp(id, APP_NAME_TR[i].id))
            return TR5(APP_NAME_TR[i].it, APP_NAME_TR[i].en, APP_NAME_TR[i].es, APP_NAME_TR[i].fr, APP_NAME_TR[i].de);
    return nullptr;
}

static const char *node_label(const MenuNode *n)
{
    if (!n) return "";
    const char *id = n->id;
    // Category labels in all five shipped languages (ASCII only). Web OS / Media stay as-is everywhere.
    if      (!strcmp(id, "Web OS"))        return "Web OS";
    else if (!strcmp(id, "Media"))         return "Media";
    else if (!strcmp(id, "Office"))        return TR5("Produttivita", "Office", "Oficina", "Bureau", "Buero");
    else if (!strcmp(id, "Tools"))         return TR5("Strumenti", "Tools", "Herramientas", "Outils", "Werkzeuge");
    else if (!strcmp(id, "System"))        return TR5("Sistema", "System", "Sistema", "Systeme", "System");
    else if (!strcmp(id, "Connect"))       return TR5("Connetti", "Connect", "Conectar", "Connecter", "Verbinden");
    else if (!strcmp(id, "Messaging"))     return TR5("Messaggi", "Messaging", "Mensajes", "Messages", "Nachrichten");
    else if (!strcmp(id, "Security"))      return TR5("Sicurezza", "Security", "Seguridad", "Securite", "Sicherheit");
    else if (!strcmp(id, "Measure"))       return TR5("Misura", "Measure", "Medir", "Mesure", "Messen");
    else if (!strcmp(id, "Games"))         return TR5("Giochi", "Games", "Juegos", "Jeux", "Spiele");
    else if (!strcmp(n->id, LAUNCHER_RECENT_ID)) return TR5("Recenti", "Recent", "Recientes", "Recents", "Zuletzt");
    else if (!strcmp(n->id, LAUNCHER_SETTINGS_SEARCH_ID)) {       // "3 settings": what the query finds inside Settings
        static char b[24]; int k = launcher_settings_hits();
        snprintf(b, sizeof b, "%d %s", k, k == 1 ? TR5("impostazione", "setting", "ajuste", "reglage", "Option")
                                                 : TR5("impostazioni", "settings", "ajustes", "reglages", "Optionen"));
        return b;
    }
    // app node: bilingual title from the central table above; proper-noun apps fall through to their name.
    const char *loc = launcher_app_localized_name(id);
    return loc ? loc : n->label;
}

// Honest Wi-Fi indicator: four rising bars whose FILL tracks the real signal. STA = green, AP/setup
// = amber; bars above the measured level are drawn dim so the glyph reads as a strength gauge (the
// smartwatch idiom) rather than a flat icon. rssi 0 (not associated) -> 1 amber bar. There is no
// battery on bare M5GFX, so the top-right is spent entirely on this. Occupies a 19x9 box at (x,y).
static void draw_wifi(int x, int y, bool online, int rssi)
{
    // Lit bars are GREEN when we truly have a link, RED when offline (no STA IP / not associated) — so
    // the gauge alone tells the real state at a glance, never a green "connected" glyph on a dead link.
    unsigned short on = online ? C_GREEN : C_RED;
    // Map dBm to 0..4 lit bars (>=-55 full, <=-88 one). Offline / unknown -> a single (red) bar.
    int lvl;
    if (!online || rssi == 0) lvl = 1;
    else if (rssi >= -55)  lvl = 4;
    else if (rssi >= -67)  lvl = 3;
    else if (rssi >= -78)  lvl = 2;
    else                   lvl = 1;
    for (int i = 0; i < 4; i++) {
        int bh = 2 + i * 2;                                  // 2,4,6,8 px tall
        d.fillRect(x + i * 5, y + 9 - bh, 3, bh, i < lvl ? on : LINE);
    }
}

// Tiny battery pip for the home right-cluster: 12x8 outline + proportional fill + terminal nub.
// (The gauge is calibrated now — eFuse + EMA, see the Battery notes — so it's honest to show it.)
// Drawn only on a real reading (pct >= 0). Colour matches the Control Center battery row.
static void draw_battery_pip(int x, int y, int pct)
{
    unsigned short c = (pct < 20) ? C_RED : (pct < 50) ? C_YELLOW : C_GREEN;
    d.drawRoundRect(x, y, 12, 8, 1, MUTED);            // body outline
    d.fillRect(x + 12, y + 3, 2, 3, MUTED);            // + terminal nub
    int fw = (10 * pct) / 100; if (fw < 1 && pct > 0) fw = 1; if (fw > 10) fw = 10;
    if (fw > 0) d.fillRect(x + 1, y + 1, fw, 6, c);    // proportional fill
}

// ---- launcher icon set (bold filled silhouettes) ---------------------------------------
// Each icon is a bold filled glyph centred at (cx,cy) inside a 2*r box, using one ink colour `col`
// plus the badge colour `bg` for clean cut-outs (so internal detail reads without thin outlines).
// This is the hand-kept port of web/device/icons.js — KEEP THE TWO IN SYNC (same coords, same forms).
// Unmapped ids fall back to a bold letter glyph so a new app never draws blank.

// Thick line as a filled quad (exact width, no drawWideLine ambiguity). Used for the few diagonals.
static void icon_line(lgfx::v1::LGFXBase *g, float x0, float y0, float x1, float y1, float w, uint16_t c)
{
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) { g->fillCircle((int)lroundf(x0), (int)lroundf(y0), (int)lroundf(w / 2), c); return; }
    float px = -dy / len * (w / 2), py = dx / len * (w / 2);
    g->fillTriangle((int)lroundf(x0 + px), (int)lroundf(y0 + py), (int)lroundf(x0 - px), (int)lroundf(y0 - py), (int)lroundf(x1 + px), (int)lroundf(y1 + py), c);
    g->fillTriangle((int)lroundf(x1 + px), (int)lroundf(y1 + py), (int)lroundf(x1 - px), (int)lroundf(y1 - py), (int)lroundf(x0 - px), (int)lroundf(y0 - py), c);
}

// Out-of-line primitive helpers. Each rounds its float coords (the old RI macro) and draws once; the
// interpreter and every procedural branch call these, so the lroundf+draw glue is not inlined per op.
static inline int ri(float v) { return (int)lroundf(v); }
static void P_box(lgfx::v1::LGFXBase *g, float x, float y, float w, float h, uint16_t c) { g->fillRect(ri(x), ri(y), ri(w), ri(h), c); }
static void P_fc(lgfx::v1::LGFXBase *g, float x, float y, float rr, uint16_t c) { g->fillCircle(ri(x), ri(y), ri(rr), c); }
static void P_tr(lgfx::v1::LGFXBase *g, float a, float b, float p, float q, float u, float v, uint16_t c) { g->fillTriangle(ri(a), ri(b), ri(p), ri(q), ri(u), ri(v), c); }
static void P_rr(lgfx::v1::LGFXBase *g, float x, float y, float w, float h, float rr, uint16_t c) { float r = fminf(rr, fminf(w / 2.0f, h / 2.0f)); g->fillRoundRect(ri(x), ri(y), ri(w), ri(h), ri(r), c); }  // CLR: clamp to half the min side
static void P_el(lgfx::v1::LGFXBase *g, float x, float y, float rx, float ry, uint16_t c) { g->drawEllipse(ri(x), ri(y), ri(rx), ri(ry), c); g->drawEllipse(ri(x), ri(y), ri(rx - 1.0f), ri(ry - 1.0f), c); }  // 2 px ring
static void P_hl(lgfx::v1::LGFXBase *g, float x, float y, float w, uint16_t c) { g->drawFastHLine(ri(x), ri(y), ri(w), c); }

// Compact drawing-op table for the icons whose primitives sit at coordinates linear in (cx,cy,s,Tk).
// One interpreter loop (in ui_icon) replays these instead of one inlined if/else branch per icon — the
// same forms as the IBX/IFC/ITR/IRR/IL/IEL/HL macros, just as data. Each op is:
//   byte0 = prim(0..6) | color<<3 | hasK<<4 | hasTk<<5;  prim 7 = END.
//   then (if hasTk) a Tk-scalar bitmask byte, (if hasK) a K-scalar bitmask byte; then per scalar:
//   int16 LE sCoeff(x100); [int16 LE TkCoeff(x100) if its Tk-mask bit]; [int8 K if its K-mask bit].
// The operand's value is reconstructed EXACTLY as the old branch wrote it:
//   value = base + s*(sCoeff/100) + Tk*(TkCoeff/100) + K      (base = cx/cy/0, chosen by prim+position),
// so the rounded pixels are byte-identical. prims: 0 BOX 1 FC 2 TR 3 RR 4 LN 5 EL 6 HL (see ui_icon).
// Icons needing loops/trig/other float shapes stay as procedural branches below. Still mirrors
// web/device/icons.js — KEEP THE TWO IN SYNC (same coords, same forms).
static const uint8_t ICON_OPS[] = {
  /*    0 clock */ 0x01,0x00,0x00,0x00,0x00,0x64,0x00,0x29,0x04,0x00,0x00,0x00,0x00,0x64,0x00,0x9c,0xff,0x20,0x05,0x00,0x00,0xce,0xff,0xc4,0xff,0x00,0x00,0x64,0x00,0x3c,0x00,0x20,0x0a,0x00,0x00,0x00,0x00,0xce,0xff,0x32,0x00,0x00,0x00,0x64,0x00,0x21,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x46,0x00,0x07,
  /*   56 pomodoro */ 0x01,0x00,0x00,0x10,0x00,0x56,0x00,0x09,0xde,0xff,0xf4,0xff,0x14,0x00,0x20,0x05,0x00,0x00,0xd8,0xff,0xa1,0xff,0x00,0x00,0x50,0x00,0x1e,0x00,0x02,0x00,0x00,0xa1,0xff,0xd6,0xff,0xde,0xff,0x2a,0x00,0xde,0xff,0x02,0xbe,0xff,0xe2,0xff,0xfa,0xff,0xd4,0xff,0xec,0xff,0xfe,0xff,0x02,0x42,0x00,0xe2,0xff,0x06,0x00,0xd4,0xff,0x14,0x00,0xfe,0xff,0x07,
  /*  124 files */ 0x23,0x10,0x9c,0xff,0xc4,0xff,0x5f,0x00,0x28,0x00,0x00,0x00,0x3c,0x00,0x03,0x9c,0xff,0xe2,0xff,0xc8,0x00,0x96,0x00,0x16,0x00,0x28,0x08,0xba,0xff,0xf6,0xff,0x8c,0x00,0x00,0x00,0x37,0x00,0x07,
  /*  162 calendar */ 0x03,0x9c,0xff,0xb2,0xff,0xc8,0x00,0xa6,0x00,0x16,0x00,0x00,0x9c,0xff,0xb2,0xff,0xc8,0x00,0x32,0x00,0x3b,0x0f,0x10,0x9c,0xff,0x64,0x00,0xe4,0xff,0x64,0x00,0xc8,0x00,0x38,0xff,0x74,0x00,0x38,0xff,0x00,0x00,0x02,0x20,0x04,0xc9,0xff,0x9c,0xff,0x00,0x00,0x64,0x00,0x22,0x00,0x20,0x05,0x37,0x00,0x9c,0xff,0x9c,0xff,0x00,0x00,0x64,0x00,0x22,0x00,0x21,0x04,0x00,0x00,0x22,0x00,0x00,0x00,0x46,0x00,0x07,
  /*  241 usb */ 0x20,0x05,0x00,0x00,0x9c,0xff,0x9c,0xff,0x00,0x00,0xc8,0x00,0x28,0x00,0x03,0xce,0xff,0xc2,0xff,0x64,0x00,0xa2,0x00,0x14,0x00,0x28,0x08,0xce,0xff,0x0c,0x00,0x64,0x00,0x00,0x00,0x37,0x00,0x07,
  /*  279 usbweb */ 0x05,0x00,0x00,0xe4,0xff,0x3e,0x00,0x3e,0x00,0x05,0x00,0x00,0xe4,0xff,0x1a,0x00,0x3e,0x00,0x06,0xc2,0xff,0xe4,0xff,0x7c,0x00,0x20,0x05,0x00,0x00,0xce,0xff,0x22,0x00,0x00,0x00,0x64,0x00,0x28,0x00,0x13,0x10,0xe2,0xff,0x48,0x00,0x3c,0x00,0x22,0x00,0x00,0x00,0x02,0x07,
  /*  332 music */ 0x21,0x04,0xd3,0xff,0x37,0x00,0x00,0x00,0x69,0x00,0x20,0x05,0xd3,0xff,0x3c,0x00,0xae,0xff,0x00,0x00,0x50,0x00,0x91,0x00,0x22,0x05,0xd3,0xff,0x8c,0x00,0xae,0xff,0xd3,0xff,0x8c,0x00,0xf4,0xff,0x48,0x00,0xd3,0xff,0x07,
  /*  375 gbemu */ 0x03,0xc2,0xff,0x9c,0xff,0x7c,0x00,0xc8,0x00,0x1a,0x00,0x08,0xd8,0xff,0xb2,0xff,0x50,0x00,0x3e,0x00,0x08,0xde,0xff,0x0e,0x00,0x0a,0x00,0x22,0x00,0x08,0xd2,0xff,0x1a,0x00,0x22,0x00,0x0a,0x00,0x09,0x1e,0x00,0x16,0x00,0x0d,0x00,0x09,0x02,0x00,0x2a,0x00,0x0d,0x00,0x07,
  /*  428 ggemu */ 0x03,0x9c,0xff,0xc2,0xff,0xc8,0x00,0x7c,0x00,0x28,0x00,0x08,0xdc,0xff,0xd6,0xff,0x48,0x00,0x54,0x00,0x08,0xb0,0xff,0xfa,0xff,0x1e,0x00,0x0c,0x00,0x08,0xb9,0xff,0xeb,0xff,0x0c,0x00,0x2a,0x00,0x09,0x38,0x00,0xfa,0xff,0x0b,0x00,0x09,0x50,0x00,0x0e,0x00,0x0b,0x00,0x07,
  /*  481 video */ 0x03,0x9c,0xff,0xb5,0xff,0xc8,0x00,0x96,0x00,0x16,0x00,0x0a,0xe2,0xff,0xd6,0xff,0xe2,0xff,0x2a,0x00,0x32,0x00,0x00,0x00,0x07,
  /*  506 Web OS */ 0x05,0x00,0x00,0x00,0x00,0x5c,0x00,0x5c,0x00,0x05,0x00,0x00,0x00,0x00,0x28,0x00,0x5c,0x00,0x06,0xa4,0xff,0x00,0x00,0xb8,0x00,0x07,
  /*  532 Media */ 0x02,0xba,0xff,0x9c,0xff,0xba,0xff,0x64,0x00,0x64,0x00,0x00,0x00,0x07,
  /*  546 photos */ 0x03,0x9c,0xff,0x9c,0xff,0xc8,0x00,0xc8,0x00,0x16,0x00,0x3b,0x0f,0x10,0x9c,0xff,0x64,0x00,0x9c,0xff,0x64,0x00,0xc8,0x00,0x38,0xff,0xc8,0x00,0x38,0xff,0x00,0x00,0x02,0x21,0x04,0xd8,0xff,0xd8,0xff,0x00,0x00,0x50,0x00,0x02,0xab,0xff,0x3e,0x00,0xf6,0xff,0xf1,0xff,0x19,0x00,0x3e,0x00,0x02,0xfb,0xff,0x3e,0x00,0x2d,0x00,0x05,0x00,0x55,0x00,0x3e,0x00,0x07,
  /*  616 recorder/voice */ 0x23,0x15,0x00,0x00,0x8d,0xff,0x9c,0xff,0x00,0x00,0xe6,0x00,0x7d,0x00,0x00,0x00,0x73,0x00,0x24,0x10,0xc6,0xff,0x00,0x00,0xc6,0xff,0x16,0x00,0x00,0x00,0x3c,0x00,0x24,0x10,0x3a,0x00,0x00,0x00,0x3a,0x00,0x16,0x00,0x00,0x00,0x3c,0x00,0x24,0x10,0xc6,0xff,0x14,0x00,0x00,0x00,0x32,0x00,0x00,0x00,0x3c,0x00,0x24,0x10,0x3a,0x00,0x14,0x00,0x00,0x00,0x32,0x00,0x00,0x00,0x3c,0x00,0x20,0x05,0x00,0x00,0xce,0xff,0x2d,0x00,0x00,0x00,0x64,0x00,0x23,0x00,0x20,0x08,0xd3,0xff,0x4e,0x00,0x5a,0x00,0x00,0x00,0x3c,0x00,0x07,
  /*  717 info/Connect */ 0x21,0x04,0x00,0x00,0x46,0x00,0x00,0x00,0x55,0x00,0x02,0xc9,0xff,0x0f,0x00,0x37,0x00,0x0f,0x00,0x00,0x00,0x37,0x00,0x0a,0xe4,0xff,0x21,0x00,0x1c,0x00,0x21,0x00,0x00,0x00,0x37,0x00,0x02,0x9c,0xff,0xd6,0xff,0x64,0x00,0xd6,0xff,0x00,0x00,0x0a,0x00,0x0a,0xc2,0xff,0xee,0xff,0x3e,0x00,0xee,0xff,0x00,0x00,0x0a,0x00,0x07,
  /*  780 radio */ 0x03,0x9c,0xff,0xe2,0xff,0xc8,0x00,0x7d,0x00,0x12,0x00,0x24,0x10,0x28,0x00,0xe2,0xff,0x55,0x00,0x9c,0xff,0x00,0x00,0x3c,0x00,0x21,0x04,0x55,0x00,0x9c,0xff,0x00,0x00,0x3c,0x00,0x09,0xd3,0xff,0x20,0x00,0x26,0x00,0x29,0x04,0x32,0x00,0x20,0x00,0x00,0x00,0x50,0x00,0x07,
  /*  833 remote */ 0x03,0x9c,0xff,0xb0,0xff,0xc8,0x00,0x82,0x00,0x12,0x00,0x3b,0x0f,0x10,0x9c,0xff,0x64,0x00,0xb0,0xff,0x64,0x00,0xc8,0x00,0x38,0xff,0x82,0x00,0x38,0xff,0x00,0x00,0x02,0x21,0x04,0xc9,0xff,0x52,0x00,0x00,0x00,0x46,0x00,0x02,0xb2,0xff,0x37,0x00,0xe0,0xff,0x37,0x00,0xc9,0xff,0x55,0x00,0x07,
  /*  890 ir */ 0x03,0xce,0xff,0x9c,0xff,0x64,0x00,0xc8,0x00,0x1e,0x00,0x29,0x04,0xfe,0xff,0xc2,0xff,0x00,0x00,0x2d,0x00,0x28,0x08,0xea,0xff,0xf4,0xff,0x28,0x00,0x00,0x00,0x2d,0x00,0x28,0x08,0xea,0xff,0x1c,0x00,0x28,0x00,0x00,0x00,0x2d,0x00,0x24,0x10,0x3c,0x00,0xba,0xff,0x64,0x00,0x9c,0xff,0x00,0x00,0x3c,0x00,0x24,0x10,0x3c,0x00,0xdd,0xff,0x64,0x00,0xc9,0xff,0x00,0x00,0x3c,0x00,0x07,
  /*  964 notify */ 0x03,0xb8,0xff,0xba,0xff,0x90,0x00,0x78,0x00,0x46,0x00,0x20,0x08,0xae,0xff,0x22,0x00,0xa4,0x00,0x00,0x00,0x41,0x00,0x20,0x0d,0x00,0x00,0xce,0xff,0x9c,0xff,0x00,0x00,0x64,0x00,0x00,0x00,0x50,0x00,0x21,0x04,0x00,0x00,0x4a,0x00,0x00,0x00,0x3c,0x00,0x07,
  /* 1014 torch */ 0x03,0x9c,0xff,0xd3,0xff,0x5a,0x00,0x5a,0x00,0x12,0x00,0x02,0xf4,0xff,0xc2,0xff,0x26,0x00,0x9c,0xff,0x26,0x00,0x64,0x00,0x02,0xf4,0xff,0x3e,0x00,0x26,0x00,0x64,0x00,0x26,0x00,0x9c,0xff,0x20,0x04,0x22,0x00,0xc9,0xff,0x00,0x00,0x50,0x00,0x6e,0x00,0x24,0x10,0x3c,0x00,0xce,0xff,0x64,0x00,0xb0,0xff,0x00,0x00,0x37,0x00,0x24,0x10,0x3c,0x00,0x00,0x00,0x64,0x00,0x00,0x00,0x00,0x00,0x37,0x00,0x24,0x10,0x3c,0x00,0x32,0x00,0x64,0x00,0x50,0x00,0x00,0x00,0x37,0x00,0x07,
  /* 1106 theme */ 0x01,0x00,0x00,0x00,0x00,0x64,0x00,0x18,0x04,0x00,0x00,0x9c,0xff,0x64,0x00,0x01,0xc8,0x00,0x05,0x00,0x00,0x00,0x00,0x64,0x00,0x64,0x00,0x07,
  /* 1134 mail */ 0x03,0x9c,0xff,0xbe,0xff,0xc8,0x00,0x84,0x00,0x12,0x00,0x2c,0x10,0xaa,0xff,0xd2,0xff,0x00,0x00,0x10,0x00,0x00,0x00,0x3c,0x00,0x2c,0x10,0x56,0x00,0xd2,0xff,0x00,0x00,0x10,0x00,0x00,0x00,0x3c,0x00,0x07,
  /* 1174 link */ 0x24,0x10,0xb2,0xff,0x00,0x00,0x4e,0x00,0xb2,0xff,0x00,0x00,0x3c,0x00,0x24,0x10,0xb2,0xff,0x00,0x00,0x4e,0x00,0x4e,0x00,0x00,0x00,0x3c,0x00,0x21,0x04,0xb2,0xff,0x00,0x00,0x00,0x00,0x69,0x00,0x21,0x04,0x4e,0x00,0xb2,0xff,0x00,0x00,0x69,0x00,0x21,0x04,0x4e,0x00,0x4e,0x00,0x00,0x00,0x69,0x00,0x07,
  /* 1233 ssh */ 0x03,0x9c,0xff,0xb0,0xff,0xc8,0x00,0xa0,0x00,0x12,0x00,0x00,0x9c,0xff,0xb0,0xff,0xc8,0x00,0x2a,0x00,0x3b,0x0d,0x10,0x9c,0xff,0x3c,0x00,0xe4,0xff,0xc8,0x00,0x88,0xff,0x64,0x00,0xc4,0xff,0x00,0x00,0x02,0x02,0xce,0xff,0xfb,0xff,0xce,0xff,0x20,0x00,0xf8,0xff,0x0e,0x00,0x20,0x08,0x00,0x00,0x18,0x00,0x2d,0x00,0x00,0x00,0x37,0x00,0x07,
  /* 1299 beacon */ 0x02,0xb0,0xff,0x64,0x00,0x50,0x00,0x64,0x00,0x00,0x00,0xf1,0xff,0x3b,0x08,0x10,0xce,0xff,0x52,0x00,0x64,0x00,0x00,0x00,0x46,0x00,0x00,0x00,0x02,0x21,0x04,0x00,0x00,0xe4,0xff,0x00,0x00,0x50,0x00,0x24,0x10,0x22,0x00,0xc9,0xff,0x3e,0x00,0xb2,0xff,0x00,0x00,0x32,0x00,0x24,0x10,0x22,0x00,0xee,0xff,0x42,0x00,0xe0,0xff,0x00,0x00,0x32,0x00,0x24,0x10,0xde,0xff,0xc9,0xff,0xc2,0xff,0xb2,0xff,0x00,0x00,0x32,0x00,0x24,0x10,0xde,0xff,0xee,0xff,0xbe,0xff,0xe0,0xff,0x00,0x00,0x32,0x00,0x07,
  /* 1395 wifiatk */ 0x21,0x04,0xe2,0xff,0x3a,0x00,0x00,0x00,0x4b,0x00,0x02,0xb0,0xff,0x05,0x00,0x14,0x00,0x05,0x00,0xe2,0xff,0x32,0x00,0x0a,0xc9,0xff,0x16,0x00,0xfb,0xff,0x16,0x00,0xe2,0xff,0x32,0x00,0x24,0x10,0x0a,0x00,0x9c,0xff,0x64,0x00,0xec,0xff,0x00,0x00,0x55,0x00,0x24,0x10,0x64,0x00,0x9c,0xff,0x0a,0x00,0xec,0xff,0x00,0x00,0x55,0x00,0x07,
  /* 1460 evilportal */ 0x03,0x9c,0xff,0xab,0xff,0xc8,0x00,0xaa,0x00,0x10,0x00,0x00,0x9c,0xff,0xab,0xff,0xc8,0x00,0x28,0x00,0x3b,0x07,0x10,0x9c,0xff,0x3c,0x00,0xd8,0xff,0x3c,0x00,0xc8,0x00,0x88,0xff,0x76,0x00,0x00,0x00,0x02,0x13,0x10,0xd8,0xff,0x02,0x00,0x50,0x00,0x38,0x00,0x00,0x00,0x02,0x05,0x00,0x00,0xfb,0xff,0x1a,0x00,0x1a,0x00,0x07,
  /* 1523 sniffer */ 0x05,0x00,0x00,0x00,0x00,0x5f,0x00,0x5f,0x00,0x05,0x00,0x00,0x00,0x00,0x3c,0x00,0x3c,0x00,0x21,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x5a,0x00,0x24,0x10,0x00,0x00,0x00,0x00,0x4e,0x00,0xc9,0xff,0x00,0x00,0x37,0x00,0x07,
  /* 1566 weather */ 0x01,0xe2,0xff,0xda,0xff,0x22,0x00,0x24,0x10,0xe2,0xff,0xa0,0xff,0xe2,0xff,0xb6,0xff,0x00,0x00,0x46,0x00,0x24,0x10,0xac,0xff,0xda,0xff,0xc2,0xff,0xda,0xff,0x00,0x00,0x46,0x00,0x24,0x10,0xbc,0xff,0xb4,0xff,0xce,0xff,0xc6,0xff,0x00,0x00,0x46,0x00,0x24,0x10,0x06,0x00,0xb4,0xff,0xf6,0xff,0xc6,0xff,0x00,0x00,0x46,0x00,0x01,0xce,0xff,0x1e,0x00,0x22,0x00,0x01,0x37,0x00,0x1e,0x00,0x28,0x00,0x01,0x05,0x00,0x06,0x00,0x2e,0x00,0x03,0xab,0xff,0x1a,0x00,0xaa,0x00,0x37,0x00,0x1b,0x00,0x07,
  /* 1662 ble */ 0x24,0x10,0x00,0x00,0x9c,0xff,0x00,0x00,0x64,0x00,0x00,0x00,0x46,0x00,0x24,0x10,0x00,0x00,0x9c,0xff,0x3c,0x00,0xce,0xff,0x00,0x00,0x46,0x00,0x24,0x10,0x3c,0x00,0xce,0xff,0xc4,0xff,0x32,0x00,0x00,0x00,0x46,0x00,0x24,0x10,0xc4,0xff,0xce,0xff,0x3c,0x00,0x32,0x00,0x00,0x00,0x46,0x00,0x24,0x10,0x3c,0x00,0x32,0x00,0x00,0x00,0x64,0x00,0x00,0x00,0x46,0x00,0x07,
  /* 1733 payloads */ 0x02,0xe2,0xff,0x9c,0xff,0x28,0x00,0xec,0xff,0xf6,0xff,0xec,0xff,0x02,0x1e,0x00,0x64,0x00,0xd8,0xff,0x14,0x00,0x0a,0x00,0x14,0x00,0x07,
  /* 1760 sentinel */ 0x03,0x9c,0xff,0x9c,0xff,0xc8,0x00,0x64,0x00,0x20,0x00,0x02,0x9c,0xff,0xce,0xff,0x64,0x00,0xce,0xff,0x00,0x00,0x64,0x00,0x0d,0x00,0x00,0xf4,0xff,0x2e,0x00,0x1e,0x00,0x29,0x04,0x00,0x00,0xf4,0xff,0x00,0x00,0x4b,0x00,0x07,
  /* 1804 fido */ 0x01,0xd3,0xff,0x00,0x00,0x34,0x00,0x29,0x04,0xd3,0xff,0x00,0x00,0x34,0x00,0x9c,0xff,0x20,0x0a,0xfe,0xff,0x00,0x00,0xd3,0xff,0x62,0x00,0x00,0x00,0x5a,0x00,0x20,0x04,0x3a,0x00,0x00,0x00,0x00,0x00,0x50,0x00,0x32,0x00,0x20,0x04,0x56,0x00,0x00,0x00,0x00,0x00,0x50,0x00,0x20,0x00,0x07,
  /* 1860 airspace */ 0x05,0x00,0x00,0x00,0x00,0x5f,0x00,0x5f,0x00,0x05,0x00,0x00,0x00,0x00,0x3e,0x00,0x3e,0x00,0x05,0x00,0x00,0x00,0x00,0x1e,0x00,0x1e,0x00,0x21,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x5a,0x00,0x07,
  /* 1898 Security */ 0x03,0x9c,0xff,0x9c,0xff,0xc8,0x00,0x64,0x00,0x20,0x00,0x02,0x9c,0xff,0xce,0xff,0x64,0x00,0xce,0xff,0x00,0x00,0x64,0x00,0x07,
  /* 1923 Games */ 0x03,0x9c,0xff,0xce,0xff,0xc8,0x00,0x69,0x00,0x32,0x00,0x28,0x0a,0xc2,0xff,0x00,0x00,0xe4,0xff,0x32,0x00,0x00,0x00,0x37,0x00,0x28,0x04,0xd6,0xff,0xe7,0xff,0x00,0x00,0x37,0x00,0x32,0x00,0x29,0x04,0x28,0x00,0xf4,0xff,0x00,0x00,0x37,0x00,0x29,0x04,0x42,0x00,0x0a,0x00,0x00,0x00,0x37,0x00,0x29,0x04,0x0e,0x00,0x0a,0x00,0x00,0x00,0x37,0x00,0x07,
  /* 1991 reactor */ 0x05,0x00,0x00,0x00,0x00,0x64,0x00,0x2a,0x00,0x05,0x00,0x00,0x00,0x00,0x2a,0x00,0x64,0x00,0x21,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x69,0x00,0x21,0x04,0x5c,0x00,0x00,0x00,0x00,0x00,0x37,0x00,0x21,0x04,0x00,0x00,0xa4,0xff,0x00,0x00,0x37,0x00,0x07,
  /* 2040 pong */ 0x13,0x10,0xa4,0xff,0xc4,0xff,0x1a,0x00,0x78,0x00,0x00,0x00,0x02,0x13,0x10,0x42,0x00,0xf1,0xff,0x1a,0x00,0x78,0x00,0x00,0x00,0x02,0x01,0x06,0x00,0x04,0x00,0x16,0x00,0x21,0x04,0x00,0x00,0xc2,0xff,0x00,0x00,0x23,0x00,0x21,0x04,0x00,0x00,0x3e,0x00,0x00,0x00,0x23,0x00,0x07,
  /* 2094 tanks */ 0x13,0x10,0xa6,0xff,0x0c,0x00,0xb4,0x00,0x32,0x00,0x00,0x00,0x03,0x21,0x04,0xc9,0xff,0x42,0x00,0x00,0x00,0x37,0x00,0x21,0x04,0x00,0x00,0x42,0x00,0x00,0x00,0x37,0x00,0x21,0x04,0x37,0x00,0x42,0x00,0x00,0x00,0x37,0x00,0x13,0x10,0xde,0xff,0xea,0xff,0x44,0x00,0x2a,0x00,0x00,0x00,0x02,0x24,0x10,0x14,0x00,0xfb,0xff,0x64,0x00,0xc2,0xff,0x00,0x00,0x3c,0x00,0x07,
  /* 2165 giardino */ 0x03,0xab,0xff,0x37,0x00,0xaa,0x00,0x2d,0x00,0x10,0x00,0x20,0x05,0x00,0x00,0xd8,0xff,0xe7,0xff,0x00,0x00,0x50,0x00,0x55,0x00,0x01,0xde,0xff,0xe4,0xff,0x22,0x00,0x01,0x22,0x00,0xe4,0xff,0x22,0x00,0x01,0x00,0x00,0xc2,0xff,0x1e,0x00,0x07,
  /* 2212 slots */ 0x03,0xab,0xff,0xba,0xff,0xaa,0x00,0x96,0x00,0x10,0x00,0x1b,0x10,0xc4,0xff,0xd3,0xff,0x78,0x00,0x5a,0x00,0x00,0x00,0x02,0x20,0x04,0xec,0xff,0xd3,0xff,0x00,0x00,0x2d,0x00,0x5a,0x00,0x20,0x05,0x14,0x00,0xd3,0xff,0xd3,0xff,0x00,0x00,0x2d,0x00,0x5a,0x00,0x20,0x04,0x55,0x00,0xc4,0xff,0x00,0x00,0x46,0x00,0x46,0x00,0x21,0x05,0x55,0x00,0x23,0x00,0xc4,0xff,0x00,0x00,0x46,0x00,0x07,
  /* 2287 Office */ 0x03,0x9c,0xff,0xd8,0xff,0xc8,0x00,0x82,0x00,0x10,0x00,0x03,0xd3,0xff,0xb0,0xff,0x5a,0x00,0x32,0x00,0x10,0x00,0x1b,0x10,0xe4,0xff,0xc2,0xff,0x38,0x00,0x28,0x00,0x00,0x00,0x02,0x28,0x08,0x9c,0xff,0x06,0x00,0xc8,0x00,0x00,0x00,0x41,0x00,0x07,
  /* 2335 device */ 0x03,0xc6,0xff,0x9c,0xff,0x74,0x00,0xc8,0x00,0x16,0x00,0x1b,0x10,0xd8,0xff,0xbc,0xff,0x50,0x00,0x7d,0x00,0x00,0x00,0x02,0x28,0x08,0xee,0xff,0xab,0xff,0x24,0x00,0x00,0x00,0x28,0x00,0x29,0x04,0x00,0x00,0x4e,0x00,0x00,0x00,0x32,0x00,0x07,
  /* 2382 poker */ 0x03,0xbc,0xff,0x9c,0xff,0x88,0x00,0xc8,0x00,0x12,0x00,0x09,0xec,0xff,0xee,0xff,0x16,0x00,0x09,0x14,0x00,0xee,0xff,0x16,0x00,0x0a,0xd7,0xff,0xfa,0xff,0x29,0x00,0xfa,0xff,0x00,0x00,0x2d,0x00,0x29,0x04,0xd4,0xff,0xb4,0xff,0x00,0x00,0x2a,0x00,0x29,0x04,0x2c,0x00,0x4c,0x00,0x00,0x00,0x2a,0x00,0x07,
  /* 2441 pinball */ 0x03,0xc2,0xff,0x9c,0xff,0x7c,0x00,0xc8,0x00,0x1e,0x00,0x1b,0x10,0xd6,0xff,0xae,0xff,0x54,0x00,0x98,0x00,0x00,0x00,0x02,0x21,0x04,0x0c,0x00,0xde,0xff,0x00,0x00,0x48,0x00,0x24,0x10,0xe2,0xff,0x34,0x00,0x02,0x00,0x1c,0x00,0x00,0x00,0x3c,0x00,0x24,0x10,0x1e,0x00,0x34,0x00,0xfe,0xff,0x1c,0x00,0x00,0x00,0x3c,0x00,0x07,
  /* 2504 level */ 0x03,0x9c,0xff,0xd6,0xff,0xc8,0x00,0x54,0x00,0x2a,0x00,0x2b,0x0f,0x9c,0xff,0x3c,0x00,0xd6,0xff,0x3c,0x00,0xc8,0x00,0x88,0xff,0x54,0x00,0x88,0xff,0x24,0x00,0x20,0x04,0xde,0xff,0xd6,0xff,0x00,0x00,0x32,0x00,0x54,0x00,0x20,0x05,0x22,0x00,0xce,0xff,0xd6,0xff,0x00,0x00,0x32,0x00,0x54,0x00,0x21,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x73,0x00,0x07,
  /* 2572 dice */ 0x03,0x9c,0xff,0x9c,0xff,0xc8,0x00,0xc8,0x00,0x1c,0x00,0x29,0x04,0xd3,0xff,0xd3,0xff,0x00,0x00,0x3c,0x00,0x29,0x04,0x2d,0x00,0xd3,0xff,0x00,0x00,0x3c,0x00,0x29,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x3c,0x00,0x29,0x04,0xd3,0xff,0x2d,0x00,0x00,0x00,0x3c,0x00,0x29,0x04,0x2d,0x00,0x2d,0x00,0x00,0x00,0x3c,0x00,0x07,
  /* 2634 goniometer */ 0x01,0x00,0x00,0x2d,0x00,0x64,0x00,0x18,0x0d,0x9c,0xff,0xff,0x2d,0x00,0xc8,0x00,0x02,0x64,0x00,0x02,0x08,0xce,0xff,0x05,0x00,0x64,0x00,0x2a,0x00,0x24,0x10,0x00,0x00,0x2d,0x00,0x42,0x00,0xc9,0xff,0x00,0x00,0x37,0x00,0x21,0x04,0x00,0x00,0x2d,0x00,0x00,0x00,0x46,0x00,0x07,
  /* 2688 pedometer */ 0x01,0x00,0x00,0xfb,0xff,0x34,0x00,0x01,0x0c,0x00,0x42,0x00,0x20,0x00,0x21,0x04,0xd6,0xff,0xc9,0xff,0x00,0x00,0x22,0x00,0x21,0x04,0xf1,0xff,0xb8,0xff,0x00,0x00,0x26,0x00,0x21,0x04,0x0d,0x00,0xb6,0xff,0x00,0x00,0x24,0x00,0x21,0x04,0x25,0x00,0xc2,0xff,0x00,0x00,0x20,0x00,0x07,
  /* 2743 alarm */ 0x21,0x04,0x00,0x00,0xc4,0xff,0x00,0x00,0x28,0x00,0x02,0x00,0x00,0xce,0xff,0xc4,0xff,0x28,0x00,0x3c,0x00,0x28,0x00,0x01,0x00,0x00,0xd6,0xff,0x1e,0x00,0x23,0x18,0xbe,0xff,0x24,0x00,0x84,0x00,0x00,0x00,0x32,0x00,0x00,0x00,0x14,0x00,0x21,0x04,0x00,0x00,0x42,0x00,0x00,0x00,0x2a,0x00,0x07,
  /* 2800 screensaver */ 0x01,0xee,0xff,0x08,0x00,0x4c,0x00,0x09,0x16,0x00,0xf8,0xff,0x38,0x00,0x21,0x04,0x48,0x00,0xc0,0xff,0x00,0x00,0x41,0x00,0x21,0x04,0x50,0x00,0x1c,0x00,0x00,0x00,0x2d,0x00,0x21,0x04,0xee,0xff,0xa8,0xff,0x00,0x00,0x32,0x00,0x07,
};
struct IconEntry { const char *id; uint16_t off; };
static const IconEntry ICON_TAB[] = {
    { "clock", 0 },
    { "pomodoro", 56 },
    { "files", 124 },
    { "calendar", 162 },
    { "usb", 241 },
    { "usbweb", 279 },
    { "music", 332 },
    { "gbemu", 375 },
    { "ggemu", 428 },
    { "video", 481 },
    { "Web OS", 506 },
    { "Media", 532 },
    { "photos", 546 },
    { "recorder", 616 },
    { "voice", 616 },
    { "info", 717 },
    { "Connect", 717 },
    { "radio", 780 },
    { "remote", 833 },
    { "ir", 890 },
    { "notify", 964 },
    { "torch", 1014 },
    { "theme", 1106 },
    { "mail", 1134 },
    { "link", 1174 },
    { "ssh", 1233 },
    { "beacon", 1299 },
    { "wifiatk", 1395 },
    { "evilportal", 1460 },
    { "sniffer", 1523 },
    { "weather", 1566 },
    { "ble", 1662 },
    { "payloads", 1733 },
    { "sentinel", 1760 },
    { "fido", 1804 },
    { "airspace", 1860 },
    { "Security", 1898 },
    { "Games", 1923 },
    { "reactor", 1991 },
    { "pong", 2040 },
    { "tanks", 2094 },
    { "giardino", 2165 },
    { "slots", 2212 },
    { "Office", 2287 },
    { "device", 2335 },
    { "poker", 2382 },
    { "pinball", 2441 },
    { "level", 2504 },
    { "dice", 2572 },
    { "goniometer", 2634 },
    { "pedometer", 2688 },
    { "alarm", 2743 },
    { "screensaver", 2800 },
};
static const int ICON_TAB_N = 53;
static const uint8_t ICON_NS[7]      = { 4, 3, 6, 5, 5, 4, 3 };           // scalars per primitive
static const uint8_t ICON_BASE[7][6] = {                                  // 1 = +cx, 2 = +cy, 0 = size
    { 1, 2, 0, 0, 0, 0 }, { 1, 2, 0, 0, 0, 0 }, { 1, 2, 1, 2, 1, 2 },
    { 1, 2, 0, 0, 0, 0 }, { 1, 2, 1, 2, 0, 0 }, { 1, 2, 0, 0, 0, 0 }, { 1, 2, 0, 0, 0, 0 },
};

static void ui_icon(lgfx::v1::LGFXBase *g, int cx, int cy, int r, const char *id, char letter, uint16_t col, uint16_t bg)
{
    if (!id) id = "";
    const float s = (float)r, Tk = (s * 0.22f > 2.0f ? s * 0.22f : 2.0f);
    // The procedural branches below draw through the same out-of-line primitive helpers the interpreter
    // uses (P_box/P_fc/P_tr/P_rr/P_el + icon_line), so the per-call rounding+draw glue exists once.
    #define IBX(x,y,w,h,c)     P_box(g,(float)(x),(float)(y),(float)(w),(float)(h),(c))
    #define IFC(x,y,rr,c)      P_fc(g,(float)(x),(float)(y),(float)(rr),(c))
    #define ITR(a,b,p,q,u,v,c) P_tr(g,(float)(a),(float)(b),(float)(p),(float)(q),(float)(u),(float)(v),(c))
    #define IRR(x,y,w,h,rr,c)  P_rr(g,(float)(x),(float)(y),(float)(w),(float)(h),(float)(rr),(c))
    #define IL(a,b,p,q,w,c)    icon_line(g,(float)(a),(float)(b),(float)(p),(float)(q),(float)(w),(c))
    #define IEL(x,y,rx,ry,c)   P_el(g,(float)(x),(float)(y),(float)(rx),(float)(ry),(c))

    // Most icons are a short list of filled primitives at coordinates linear in (cx,cy,s,Tk): those live
    // as a compact op table (ICON_OPS) a single loop replays, instead of one inlined branch each. The
    // reconstructed float for every operand is computed the same way the old code wrote it (base + one
    // s-product + one Tk-product + an int), so the rounded pixels are identical.
    for (int e = 0; e < ICON_TAB_N; e++) {
        if (strcmp(id, ICON_TAB[e].id)) continue;
        const uint8_t *p = ICON_OPS + ICON_TAB[e].off;
        for (;;) {
            uint8_t code = *p++;
            int prim = code & 0x07;
            if (prim == 7) break;                                   // END
            uint16_t c = (code & 0x08) ? bg : col;
            uint8_t tcmask = (code & 0x20) ? *p++ : 0;              // which scalars carry a Tk term
            uint8_t kmask  = (code & 0x10) ? *p++ : 0;              // which scalars carry an int const
            const uint8_t *bk = ICON_BASE[prim];
            float v[6];
            for (int i = 0, n = ICON_NS[prim]; i < n; i++) {
                int16_t sh = (int16_t)(p[0] | (p[1] << 8)); p += 2;
                int16_t th = 0; if (tcmask >> i & 1) { th = (int16_t)(p[0] | (p[1] << 8)); p += 2; }
                int8_t kk = 0; if (kmask >> i & 1) kk = (int8_t)(*p++);
                float base = bk[i] == 1 ? (float)cx : bk[i] == 2 ? (float)cy : 0.0f;
                v[i] = base + s * ((float)sh / 100.0f) + Tk * ((float)th / 100.0f) + (float)kk;
            }
            switch (prim) {
                case 0: P_box(g, v[0], v[1], v[2], v[3], c); break;
                case 1: P_fc(g, v[0], v[1], v[2], c); break;
                case 2: P_tr(g, v[0], v[1], v[2], v[3], v[4], v[5], c); break;
                case 3: P_rr(g, v[0], v[1], v[2], v[3], v[4], c); break;
                case 4: icon_line(g, v[0], v[1], v[2], v[3], v[4], c); break;
                case 5: P_el(g, v[0], v[1], v[2], v[3], c); break;
                case 6: P_hl(g, v[0], v[1], v[2], c); break;
            }
        }
        return;
    }

    // The rest stay procedural: loops, trig, or coordinates the table form can't reproduce bit-exactly.
    if (!strcmp(id, "store")) {                                         // shopping bag + download arrow
        IFC(cx, cy - s * 0.38f, s * 0.46f, col); IFC(cx, cy - s * 0.38f, s * 0.46f - Tk * 0.8f, bg);   // handle
        IRR(cx - s * 0.82f, cy - s * 0.34f, s * 1.64f, s * 1.3f, s * 0.22f, col);                      // bag
        IBX(cx - Tk * 0.36f, cy - s * 0.14f, Tk * 0.72f, s * 0.48f, bg);                                // arrow stem
        ITR(cx - s * 0.34f, cy + s * 0.3f, cx + s * 0.34f, cy + s * 0.3f, cx, cy + s * 0.7f, bg);       // arrow head

    } else if (!strcmp(id, "chrono")) {                                 // stopwatch: dial ring + top plunger + single hand
        IBX(cx - Tk * 0.4f, cy - s * 0.98f, Tk * 0.8f, s * 0.34f, col);        // plunger stem
        IFC(cx, cy - s * 0.98f, Tk * 0.6f, col);                              // plunger knob
        IFC(cx, cy + s * 0.14f, s * 0.80f, col); IFC(cx, cy + s * 0.14f, s * 0.80f - Tk, bg);   // dial body -> ring
        IBX(cx - Tk * 0.35f, cy + s * 0.14f - s * 0.50f, Tk * 0.7f, s * 0.50f, col);            // hand (points up)
        IFC(cx, cy + s * 0.14f, Tk * 0.8f, col);                              // centre hub

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

    } else if (!strcmp(id, "notepad")) {
        IRR(cx - s * 0.8f, cy - s, s * 1.6f, 2 * s, s * 0.2f, col);
        for (int i = 0; i < 3; i++) IBX(cx - s * 0.5f, cy - s * 0.45f + i * s * 0.5f, s * (1.0f - i * 0.18f), Tk * 0.55f, bg);

    } else if (!strcmp(id, "usbkbd")) {
        IRR(cx - s, cy - s * 0.6f, 2 * s, s * 1.2f, s * 0.22f, col);
        for (int ry = 0; ry < 2; ry++) for (int cc = 0; cc < 4; cc++) IBX(cx - s * 0.72f + cc * s * 0.46f, cy - s * 0.32f + ry * s * 0.42f, Tk * 0.6f, Tk * 0.6f, bg);
        IBX(cx - s * 0.4f, cy + s * 0.34f, s * 0.8f, Tk * 0.55f, bg);

    } else if (!strcmp(id, "micspec")) {
        static const float mh[5] = { 0.55f, 1.0f, 0.4f, 0.85f, 0.6f };
        for (int i = 0; i < 5; i++) IBX(cx - s + i * (2 * s / 5.0f) + Tk * 0.2f, cy + s - 2 * s * mh[i], Tk, 2 * s * mh[i], col);

    } else if (!strcmp(id, "voicelab")) {
        IRR(cx - s, cy - s * 0.8f, 2 * s, s * 1.25f, s * 0.3f, col);
        ITR(cx - s * 0.5f, cy + s * 0.35f, cx - s * 0.5f, cy + s, cx, cy + s * 0.4f, col);
        for (int i = -1; i <= 1; i++) IFC(cx + i * s * 0.5f, cy - s * 0.18f, Tk * 0.5f, bg);

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

    } else if (!strcmp(id, "qr")) {
        const float e = s * 0.6f; static const int o[3][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 } };
        for (int k = 0; k < 3; k++) { int ox = o[k][0], oy = o[k][1];
            IBX(cx + ox * e - e * 0.5f, cy + oy * e - e * 0.5f, e, e, col); IBX(cx + ox * e - e * 0.26f, cy + oy * e - e * 0.26f, e * 0.52f, e * 0.52f, bg); IFC(cx + ox * e, cy + oy * e, e * 0.16f, col); }
        static const float d[4][2] = { { 0.32f, 0.32f }, { 0.72f, 0.42f }, { 0.42f, 0.72f }, { 0.74f, 0.74f } };
        for (int k = 0; k < 4; k++) IBX(cx + d[k][0] * s, cy + d[k][1] * s, Tk * 0.55f, Tk * 0.55f, col);

    } else if (!strcmp(id, "wifi")) {
        for (int i = 0; i < 3; i++) { float yy = cy - s * 0.55f + i * s * 0.55f; IBX(cx - s, yy - Tk * 0.28f, 2 * s, Tk * 0.55f, col); IFC(cx - s * 0.5f + i * s * 0.5f, yy, Tk * 0.85f, col); }

    } else if (!strcmp(id, "Messaging")) {
        // category: speech bubble with a tail + three dots
        IRR(cx - s, cy - s * 0.8f, 2 * s, s * 1.28f, s * 0.34f, col);
        ITR(cx - s * 0.5f, cy + s * 0.3f, cx - s * 0.08f, cy + s * 0.3f, cx - s * 0.62f, cy + s, col);
        for (int i = -1; i <= 1; i++) IFC(cx + i * s * 0.42f, cy - s * 0.14f, Tk * 0.5f, bg);

    } else if (!strcmp(id, "swarm")) {
        // a hub with peers all around it — a mesh/swarm of devices (distinct from link's 3 nodes)
        const float R = s * 0.84f;
        static const float o[6][2] = { {1.0f,0.0f},{0.5f,0.87f},{-0.5f,0.87f},{-1.0f,0.0f},{-0.5f,-0.87f},{0.5f,-0.87f} };
        for (int k = 0; k < 6; k++) { float px = cx + R * o[k][0], py = cy + R * o[k][1];
            IL(cx, cy, px, py, Tk * 0.5f, col); IFC(px, py, Tk * 0.66f, col); }
        IFC(cx, cy, Tk * 1.05f, col);

    } else if (!strcmp(id, "ethernet")) {
        IRR(cx - s * 0.72f, cy - s * 0.8f, s * 1.44f, s * 1.3f, s * 0.16f, col);
        for (int i = 0; i < 4; i++) IBX(cx - s * 0.5f + i * s * 0.32f, cy - s * 0.8f, Tk * 0.45f, s * 0.4f, bg);
        IBX(cx - Tk * 0.7f, cy + s * 0.5f, Tk * 1.4f, s * 0.5f, col);

    } else if (!strcmp(id, "stelle")) {
        static const float p[5][2] = { { -0.78f, -0.5f }, { -0.05f, -0.15f }, { 0.7f, -0.6f }, { 0.4f, 0.6f }, { -0.55f, 0.7f } };
        static const int lk[4][2] = { { 0, 1 }, { 1, 2 }, { 1, 3 }, { 3, 4 } };
        for (int k = 0; k < 4; k++) IL(cx + p[lk[k][0]][0] * s, cy + p[lk[k][0]][1] * s, cx + p[lk[k][1]][0] * s, cy + p[lk[k][1]][1] * s, Tk * 0.35f, col);
        static const float sr[5] = { 0.55f, 0.9f, 0.5f, 0.62f, 0.45f };
        for (int i = 0; i < 5; i++) IFC(cx + p[i][0] * s, cy + p[i][1] * s, Tk * sr[i], col);

    } else if (!strcmp(id, "Tools")) {
        for (int i = 0; i < 8; i++) { float a = i * 0.785398f; IBX(cx + cosf(a) * s * 0.84f - Tk * 0.45f, cy + sinf(a) * s * 0.84f - Tk * 0.45f, Tk * 0.9f, Tk * 0.9f, col); }
        IFC(cx, cy, s * 0.6f, col); IFC(cx, cy, s * 0.24f, bg);

    } else if (!strcmp(id, "Measure")) {                                       // DIP microchip: body + side legs + pin-1 dot
        IRR(cx - s * 0.56f, cy - s * 0.72f, s * 1.12f, s * 1.44f, s * 0.12f, col);
        IRR(cx - s * 0.28f, cy - s * 0.44f, s * 0.56f, s * 0.9f, 2, bg);
        for (int i = -1; i <= 1; i++) {
            IBX(cx - s, cy + i * s * 0.42f - Tk * 0.28f, s * 0.44f, Tk * 0.56f, col);
            IBX(cx + s * 0.56f, cy + i * s * 0.42f - Tk * 0.28f, s * 0.44f, Tk * 0.56f, col);
        }
        IFC(cx - s * 0.28f, cy - s * 0.46f, Tk * 0.5f, col);                    // pin-1 notch dot

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
    } else if (r >= 10 && nucleo_store_app_icon && nucleo_store_app_icon(id)) {   // app installed from the Store: its 32x32 drawn icon
        const uint8_t *bm = nucleo_store_app_icon(id);
        float side = s * 1.7f, px = side / 32.0f, ox = cx - side * 0.5f, oy = cy - side * 0.5f;
        for (int row = 0; row < 32; row++)
            for (int cl = 0; cl < 32; cl++)
                if (bm[row * 4 + cl / 8] & (0x80 >> (cl % 8)))
                    P_box(g, ox + cl * px, oy + row * px, px + 0.9f, px + 0.9f, col);
    } else {
        g->setTextSize(r >= 12 ? 3 : (r >= 6 ? 2 : 1)); g->setTextColor(col);   // fallback: a bold letter
        int cw = (r >= 12 ? 18 : (r >= 6 ? 12 : 6)), ch = (r >= 12 ? 24 : (r >= 6 ? 16 : 8));
        g->setCursor(cx - cw / 2 + 1, cy - ch / 2); g->print(letter);
        g->setTextSize(1);                                                      // MUST reset: else it leaks into the title (Font4 x3 = huge)
    }
    #undef IBX
    #undef IFC
    #undef ITR
    #undef IRR
    #undef IL
    #undef IEL
}

// Public wrappers so sibling launcher TUs (the Game front-end's procedural poster) can draw the real
// vector icons — to the off-screen canvas (M5Canvas) or straight to the display (M5GFX). Both derive
// from LGFXBase, so ui_icon takes the base pointer and there is a single shared implementation.
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
    if (nucleo_evilportal_running())     { snprintf(buf, cap, TR("EVIL PORTAL  %d catt.", "EVIL PORTAL  %d caps"), nucleo_evilportal_captures()); return buf; }
    if (nucleo_wifiatk_deauth_running()) { snprintf(buf, cap, "DEAUTH FLOOD  %lu fr", nucleo_wifiatk_frames());        return buf; }
    if (nucleo_wifiatk_beacon_running()) { snprintf(buf, cap, "BEACON SPAM  %d SSID", nucleo_wifiatk_beacon_count()); return buf; }
    return NULL;
}

// "mar 29 set": day + month in the system language (it/en/es/fr/de; ASCII 3-letter forms, English is the
// floor). Empty while the clock was never set.
static void day_month(char *dt, int cap, time_t now, const struct tm *tm)
{
    static const char *const WD[5][7] = {
        { "dom","lun","mar","mer","gio","ven","sab" }, { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" },
        { "dom","lun","mar","mie","jue","vie","sab" }, { "dim","lun","mar","mer","jeu","ven","sam" },
        { "So","Mo","Di","Mi","Do","Fr","Sa" } };
    static const char *const MO[5][12] = {
        { "gen","feb","mar","apr","mag","giu","lug","ago","set","ott","nov","dic" },
        { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" },
        { "ene","feb","mar","abr","may","jun","jul","ago","sep","oct","nov","dic" },
        { "jan","fev","mar","avr","mai","jun","jul","aou","sep","oct","nov","dec" },
        { "Jan","Feb","Mar","Apr","Mai","Jun","Jul","Aug","Sep","Okt","Nov","Dez" } };
    const char *lc = nucleo_i18n_lang();
    int li = !strcmp(lc, "it") ? 0 : !strcmp(lc, "es") ? 2 : !strcmp(lc, "fr") ? 3 : !strcmp(lc, "de") ? 4 : 1;
    dt[0] = 0;
    if (tm && now > 1672531200) snprintf(dt, cap, "%s %d %s", WD[li][tm->tm_wday], tm->tm_mday, MO[li][tm->tm_mon]);
}

static uint16_t mix565(uint16_t a, uint16_t b, float t);   // below, with the carousel

// ---- the panel's status bar (30 px) ----------------------------------------
// A soft band (lighter at the top) over a hairline; the clock in bold; the date, the network and the
// count in Font2, legible at arm's length; inside a category a raised Back pill (tap = back), the
// category's chip and its name in bold. The whole bar is a tap target for the Control Center.
static uint16_t bar_shade(int y) { return mix565(mix565(INK, LINE, 0.55f), INK, (float)y / (BAR - 1)); }
static void bar_band(int x, int w)
{
    for (int y = 0; y < BAR - 1; y++) d.drawFastHLine(x, y, w, bar_shade(y));
    d.drawFastHLine(x, BAR - 1, w, LINE);
}
static void bar_clock_large(void)
{
    time_t now = time(NULL); struct tm *tm = localtime(&now);
    char t[8]; snprintf(t, sizeof(t), "%02d:%02d", tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0);
    bar_band(0, 72);
    d.setFont(&fonts::FreeSansBold12pt7b); d.setTextSize(1); d.setTextColor(FG);
    d.setTextDatum(middle_left); d.drawString(t, 8, BAR / 2); d.setTextDatum(top_left);
    d.setFont(&fonts::Font0);
}
static void status_large(bool sta)
{
    bar_band(0, W);
    const MenuNode *node = launcher_node();
    d.setFont(&fonts::Font2); d.setTextSize(1);
    int rx = W - 6;
    draw_wifi(rx - 19, BAR / 2 - 4, sta, nucleo_setup_rssi()); rx -= 19 + 8;
    if (launcher_depth() > 0) {
        const int py = 4, ph = BAR - 9, pw = 46;                                   // the raised Back pill
        d.fillRoundRect(4, py + 2, pw, ph, ph / 2, mix565(INK, 0x0000, 0.5f));      // its shadow
        const uint16_t body = mix565(LINE, FG, 0.16f);
        d.fillRoundRect(4, py, pw, ph, ph / 2, body);
        d.drawFastHLine(4 + ph / 2, py + 1, pw - ph, mix565(body, 0xFFFF, 0.25f)); // a highlight along its top
        const int ay = py + ph / 2;
        for (int k = 0; k < 3; k++) { d.drawLine(31 + k, ay - 7, 24 + k, ay, FG); d.drawLine(24 + k, ay, 31 + k, ay + 7, FG); }
        const int cx = 58, cs = 20, cy = (BAR - cs) / 2 - 1;                        // the category chip
        d.fillRoundRect(cx, cy, cs, cs, 5, node->color);
        ui_icon(&d, cx + cs / 2, cy + cs / 2, 8, strcmp(node->id, LAUNCHER_RECENT_ID) ? node->id : "clock", node->icon, INK, node->color);
        int cnt = node_child_count(node);
        if (cnt > 0) {
            char cc[12]; snprintf(cc, sizeof cc, "%d app", cnt);
            d.setFont(&fonts::Font2); d.setTextColor(MUTED); d.setTextDatum(middle_right); d.drawString(cc, rx, BAR / 2);
            rx -= (int)d.textWidth(cc) + 10;
        }
        char b[24]; snprintf(b, sizeof b, "%s", node_label(node));
        d.setFont(&fonts::FreeSansBold9pt7b);
        while (b[0] && (int)d.textWidth(b) > rx - (cx + cs + 8)) b[strlen(b) - 1] = 0;
        d.setTextColor(FG); d.setTextDatum(middle_left); d.drawString(b, cx + cs + 7, BAR / 2);
    } else {
        bar_clock_large();
        d.setFont(&fonts::Font2);
        if (!launcher_filter()[0]) {
            int bpct = nucleo_power_battery_pct();
            if (bpct >= 0) { draw_battery_pip(rx - 14, BAR / 2 - 4, bpct); rx -= 14 + 10; }
            char dt[16]; time_t now = time(NULL); struct tm *tm = localtime(&now);
            day_month(dt, sizeof dt, now, tm);
            d.setFont(&fonts::Font2); d.setTextDatum(middle_right);
            if (dt[0]) { d.setTextColor(C_YELLOW); d.drawString(dt, rx, BAR / 2); rx -= (int)d.textWidth(dt) + 12; }
            char net[16]; snprintf(net, sizeof net, "%.12s", sta ? nucleo_setup_ssid() : "offline");
            if (rx - (int)d.textWidth(net) > 80) { d.setTextColor(sta ? MUTED : C_RED); d.drawString(net, rx, BAR / 2); }
        }
    }
    if (launcher_filter()[0]) {                                                    // the search, as a green chip
        char fb[20]; snprintf(fb, sizeof fb, "/%.12s", launcher_filter());
        d.setFont(&fonts::Font2);
        const int fw = (int)d.textWidth(fb) + 14, fx = W - fw - 32;
        d.fillRoundRect(fx, 5, fw, BAR - 11, (BAR - 11) / 2, 0x1926);
        d.setTextColor(C_GREEN); d.setTextDatum(middle_center); d.drawString(fb, fx + fw / 2, BAR / 2 - 1);
    }
    d.setTextDatum(top_left); d.setFont(&fonts::Font0); d.setTextSize(1);
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
        d.setTextSize(1); d.setTextColor(INK, C_RED); d.setCursor(18, 4 + BY); d.print(alert);
        d.drawFastHLine(0, BAR - 1, W, LINE);
        return;
    }
    bool sta_l = !strcmp(nucleo_setup_mode(), "sta") && nucleo_setup_ip()[0];
    if (lr_large()) { status_large(sta_l); return; }                  // the panel's own bar
    d.fillRect(0, 0, W, BAR, INK);
    d.setTextSize(1);
    // "Online" = a REAL client link: STA mode AND a live IP. mode/ssid alone go stale on a drop (the
    // old test showed the last network as if still connected); s_ip is cleared the instant the link
    // drops, so it is the honest gate for both the gauge colour and the SSID-vs-"offline" label.
    bool sta = !strcmp(nucleo_setup_mode(), "sta") && nucleo_setup_ip()[0];

    const MenuNode *node = launcher_node();
    if (launcher_depth() > 0) {
        // Breadcrumb: a colour chip carrying the category icon + the name in the big Font2 face
        // (legible), with the Wi-Fi gauge and the item count packed from the right edge inward.
        // On the touch panel a back chevron leads it, so the breadcrumb reads as "tap here to go back".
        int cx = 2;
        if (lr_large()) {                                    // a Back button the finger finds: pill + bold chevron
            const int bh = BAR - 6, ay = BAR / 2;
            d.fillRoundRect(3, 3, 44, bh, bh / 2, LINE);
            for (int k = 0; k < 3; k++) { d.drawLine(29 + k, ay - 7, 22 + k, ay, FG); d.drawLine(22 + k, ay, 29 + k, ay + 7, FG); }
            cx = 54;
        }
        d.fillRoundRect(cx, 1 + BY, 14, 14, 3, node->color);
        ui_icon(&d, cx + 7, 8 + BY, 6, strcmp(node->id, LAUNCHER_RECENT_ID) ? node->id : "clock", node->icon, INK, node->color);
        d.setFont(&fonts::Font2); d.setTextColor(FG, INK); d.setCursor(cx + 18, BY);
        char b[16]; snprintf(b, sizeof(b), "%.14s", node_label(node)); d.print(b);
        d.setFont(&fonts::Font0); d.setTextSize(1);
        int rx = W - 2;
        draw_wifi(rx - 19, 4 + BY, sta, nucleo_setup_rssi());   rx -= 19 + 6;   // signal far right
        int cnt = node_child_count(node);
        if (cnt > 0) {
            char cc[12]; snprintf(cc, sizeof cc, "%d app", cnt);
            d.setTextColor(MUTED, INK); d.setCursor(rx - (int)strlen(cc) * 6, 4 + BY); d.print(cc);   // left of the gauges
        }
    } else {
        // Home (smartwatch face): a bold clock anchors the LEFT in the 16px-tall Font2 face; the
        // Wi-Fi gauge, the date (day + month) and the network name form ONE right-aligned cluster,
        // packed from the right edge inward. The date has PRIORITY over the SSID, so day/month is
        // shown whenever it fits. Drawn only when no filter chip claims the right.
        time_t now = time(NULL); struct tm *tm = localtime(&now);
        char t[8]; snprintf(t, sizeof(t), "%02d:%02d", tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0);
        d.setFont(&fonts::Font2); d.setTextColor(FG, INK); d.setCursor(6, BY); d.print(t);  // big white clock
        int clock_r = 6 + (int)d.textWidth(t);
        d.setFont(&fonts::Font0); d.setTextSize(1);

        if (!launcher_filter()[0]) {
            // Right cluster from the edge: [|||] Wi-Fi, then [day month], then [ssid] if it still
            // fits. Each piece is dropped if it would crowd the clock, so nothing overlaps the time.
            int rx = W - 2;
            draw_wifi(rx - 19, 4 + BY, sta, nucleo_setup_rssi());   rx -= 19 + 6;   // antenna gauge, far right
            int bpct = nucleo_power_battery_pct();
            if (bpct >= 0) { draw_battery_pip(rx - 14, 4 + BY, bpct); rx -= 14 + 6; }   // pip left of the gauge
            char dt[16];
            day_month(dt, sizeof dt, now, tm);
            int dw = (int)strlen(dt) * 6;
            if (dt[0] && rx - dw > clock_r + 6) {
                d.setTextColor(C_YELLOW, INK); d.setCursor(rx - dw, 4 + BY); d.print(dt);
                rx -= dw + 8;
            }
            // Connected -> the live SSID in muted grey; not connected -> a small red "offline" right
            // next to the gauge, so a missing/lost network reads instantly instead of a stale name.
            char net[12]; snprintf(net, sizeof net, "%.10s", sta ? nucleo_setup_ssid() : "offline");
            int nw = (int)strlen(net) * 6;
            if (rx - nw > clock_r + 8) {
                d.setTextColor(sta ? MUTED : C_RED, INK); d.setCursor(rx - nw, 4 + BY); d.print(net);
            }
        }
    }

    if (launcher_filter()[0]) {
        // Filter chip, right-aligned so it never collides with the title/clock.
        char fb[20]; snprintf(fb, sizeof fb, "/%.10s", launcher_filter());
        int fw = (int)strlen(fb) * 6;
        d.fillRect(W - fw - 4, 0, fw + 4, BAR - 1, 0x1926);
        d.setTextColor(C_GREEN, 0x1926); d.setCursor(W - fw - 1, 4 + BY); d.print(fb);
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
        d.setTextSize(1); d.setTextColor(INK, C_RED); d.setCursor(18, 4 + BY); d.print(alert);
        return;
    }
    // The right cluster (Wi-Fi gauge + SSID/"offline") is normally repainted only on navigation, so a
    // link state change while the user sits on the home screen — e.g. the background join landing a few
    // seconds after boot, or a drop — would otherwise stay invisible until they moved. Detect the flip
    // here (the 1 Hz tick is the only thing repainting the idle home bar) and do ONE full repaint so the
    // gauge colour and the SSID/"offline" label update on their own. It's momentary and only on a flip,
    // so it doesn't reintroduce the per-second black flash the in-place clock refresh was built to avoid.
    static int s_last_online = -1;
    int online_now = (!strcmp(nucleo_setup_mode(), "sta") && nucleo_setup_ip()[0]) ? 1 : 0;
    if (online_now != s_last_online) { s_last_online = online_now; launcher_render_status_bar(); return; }
    char t[8]; time_t now = time(NULL); struct tm *tm = localtime(&now);
    snprintf(t, sizeof(t), "%02d:%02d", tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0);
    if (lr_large()) { bar_clock_large(); return; }            // the panel: its bold clock on its band
    d.fillRect(0, 0, 56, BAR - 1, INK);                       // wipe the clock cell (Font2 is wider than the old 6x8)
    d.setFont(&fonts::Font2); d.setTextColor(FG, INK); d.setCursor(6, BY); d.print(t);
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
    if (HINT <= 0) return;                                       // large surface: no hint bar
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
// Compact values first; the large surface (320x210 band) gets bigger badges, a wider rail and room for the
// focused app's description under its title.
#define CAR_SLOT    (lr_large() ? 118 : 84)   // horizontal spacing between adjacent icons (left/right peek)
#define CAR_R_C     (lr_large() ? 50 : 32)    // centred badge radius (fills the band top-down)
#define CAR_R_S     (lr_large() ? 30 : 19)    // side badge radius (neighbours stay prominent)
#define CAR_ICON_DY (lr_large() ? 70 : 34)    // icon-row centre, pushed up to use the band top
#define CAR_TITLE_Y (lr_large() ? 150 : 84)   // title centre, below the icon
#define CAR_DOTS_Y  (lr_large() ? 200 : 102)  // position rail

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
    // Frame-rate-independent glide: alpha = 1 - exp(-dt/tau). The old code stepped a fixed 0.30 PER
    // REDRAW, so the scroll sped up or dragged as the loop cadence changed (chrome ticks, app churn).
    // tau = 56 ms reproduces exactly 0.30 at the ~50 Hz idle loop but stays constant off-cadence. A big
    // gap (just returned from an app) is clamped so the first step is a fast catch-up, not a teleport.
    static int64_t s_last_us = 0;
    int64_t nowu = esp_timer_get_time();
    float dt = s_last_us ? (float)(nowu - s_last_us) * 1e-6f : 0.020f;
    s_last_us = nowu;
    if (dt > 0.100f) dt = 0.100f;
    float alpha = 1.0f - expf(-dt / 0.056f);
    if (n >= 3) {                                                            // circular: shortest way round
        float d = target - s_carousel; d -= (float)n * roundf(d / (float)n);
        if (d < 0.02f && d > -0.02f) { s_carousel = target; return false; }
        s_carousel += d * alpha;
        s_carousel = fmodf(s_carousel, (float)n); if (s_carousel < 0.0f) s_carousel += (float)n;
        return true;
    }
    if (target > (float)(n - 1)) target = (float)(n - 1);                    // 1-2 items: plain ease
    float d = target - s_carousel;
    if (d < 0.05f && d > -0.05f) { s_carousel = target; return false; }
    s_carousel += d * alpha;
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
    c->setTextColor(FG);                                                                    // no box: keep the rim whole
    c->setTextDatum(middle_center); c->drawString(s, cx, cy + 1); c->setTextDatum(top_left);
}

// Dot rail / position indicator, centred at (cx, y). Collapses to "k/n" past 13 items.
template <typename T> static void draw_dots(T *c, int n, int sel, int cx, int y, uint16_t accent)
{
    if (n <= 1) return;
    if (n > 13) {
        char b[12]; snprintf(b, sizeof b, "%d/%d", sel + 1, n);
        c->setFont(&fonts::Font0); c->setTextSize(1); c->setTextColor(MUTED, BG);
        c->setCursor(cx - (int)c->textWidth(b) / 2, y - 6); c->print(b);   // 8 px glyphs end above the band edge
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

// ---- the launcher grid (external 320x240 panel) -----------------------------
// A tiny screen scrolls a one-at-a-time carousel; a 320x240 panel with touch shows a real home screen:
// a grid of app tiles (icon + name), many readable at once, each one a tap target. Replaces the scaled
// carousel on the large surface only — the Cardputer keeps the carousel. Keyboard focus is a lit card
// under the tile; touch just taps a tile. The grid scrolls in pixels: a key keeps the focused tile in view
// with the smallest move, a drag moves the content itself (NUCLEO_TOUCH_VIEW_*) and leaves the focus alone,
// rows cut by the band's edges are clipped. Inside a category the first cell is a Back tile.
#define GRID_COLS   4
#define GRID_CELLW  (W / GRID_COLS)               // 80 px on the 320-wide panel
#define GRID_RH     (LIST_BAND_H / 3)             // three whole rows fill the band (70 px under the 30 px bar)
#define GRID_TOP    0
#define GRID_BADGE  38                            // the icon badge (40 when focused)
#define GRID_STEP   6                             // px the content moves per drag key
static int s_grid_off = 0;                        // scroll offset of the grid band, in px
static int s_grid_max = 0;                        // its largest value (content taller than the band)
static const void *s_grid_node = nullptr;         // the menu the offset belongs to (a new one starts at the top)
static int s_grid_sel = -1;                       // the focus the offset was last fitted to

static int grid_lead(void) { return launcher_depth() > 0 ? 1 : 0; }   // cell 0 = Back inside a category

// Fit the offset: a new menu starts at the top; a focus that moved brings its whole row into view. The band
// is exactly three rows, so a key always lands on whole rows (nothing cut at the edges); a drag's offset is
// kept as it is. Pure layout (no draw), so the touch targets land where the tiles do.
static void grid_fit(void)
{
    int cells = launcher_visible_count() + grid_lead(), rows = (cells + GRID_COLS - 1) / GRID_COLS;
    int content = GRID_TOP + rows * GRID_RH;
    s_grid_max = content > LIST_BAND_H ? content - LIST_BAND_H : 0;
    const void *node = (const void *)launcher_node();
    if (node != s_grid_node) { s_grid_node = node; s_grid_off = 0; s_grid_sel = -1; }
    int sel = launcher_sel();
    if (sel != s_grid_sel) {
        int ry = GRID_TOP + (sel + grid_lead()) / GRID_COLS * GRID_RH;
        if (ry < s_grid_off) s_grid_off = ry;
        if (ry + GRID_RH > s_grid_off + LIST_BAND_H) s_grid_off = ry + GRID_RH - LIST_BAND_H;
        s_grid_sel = sel;
    }
    if (s_grid_off > s_grid_max) s_grid_off = s_grid_max;
    if (s_grid_off < 0) s_grid_off = 0;
}

// The first item index whose tile is (at least partly) on screen: touch slot k is that item + k.
static int grid_first(void)
{
    grid_fit();
    int cell = (s_grid_off > GRID_TOP ? (s_grid_off - GRID_TOP) / GRID_RH : 0) * GRID_COLS - grid_lead();
    return cell < 0 ? 0 : cell;
}

int launcher_render_large_cols(void) { return lr_large() ? GRID_COLS : 0; }
int launcher_render_grid_first(void) { return grid_first(); }

// A drag on the grid: move the content by one step. True when it moved (repaint).
bool launcher_render_grid_view(int dir)
{
    int o = s_grid_off + dir * GRID_STEP;
    o = o < 0 ? 0 : o > s_grid_max ? s_grid_max : o;
    if (o == s_grid_off) return false;
    s_grid_off = o;
    return true;
}

// A badge with depth: a soft drop shadow, the colour, a gloss on its upper part and a darker rim at the
// bottom. The glyph goes on top (its own background is `col`, the gloss is faint enough not to show seams).
template <typename T> static void grid_badge(T *c, int x, int y, int s, uint16_t col, uint16_t under)
{
    const int r = s / 4;
    c->fillRoundRect(x + 1, y + 3, s, s, r, mix565(under, 0x0000, 0.55f));          // shadow
    c->fillRoundRect(x, y, s, s, r, mix565(col, 0x0000, 0.22f));                       // bottom rim
    c->fillRoundRect(x, y, s, s - 2, r, col);
    c->fillRoundRect(x + 3, y + 2, s - 6, s / 2 - 2, r - 2, mix565(col, 0xFFFF, 0.10f));   // gloss
}

// The label under a tile: one line when it fits, else two (cut at the last space that fits; a single long
// word is split where it must). Font2, centred in the cell; the second line ends in ".." only when even two
// lines are not enough.
template <typename T> static void grid_label(T *c, const char *lab, int cx, int y, int maxw, uint16_t col, uint16_t bg)
{
    c->setFont(&fonts::Font2); c->setTextSize(1); c->setTextColor(col, bg); c->setTextDatum(top_center);
    if ((int)c->textWidth(lab) <= maxw) { c->drawString(lab, cx, y + 6); c->setTextDatum(top_left); return; }
    char l1[24], l2[24];
    int n = (int)strlen(lab), fit = 0, sp = 0;                       // the longest first line that fits, and its last space
    for (int i = 1; i <= n && i < (int)sizeof l1; i++) {
        snprintf(l1, sizeof l1, "%.*s", i, lab);
        if ((int)c->textWidth(l1) > maxw) break;
        fit = i;
        if (lab[i] == ' ') sp = i;
    }
    int brk = sp;
    bool hyph = false;
    if (brk) {                                                        // a space break whose rest still overflows:
        const char *r2 = lab + brk + 1;                               // hyphenate instead, the rest fits better
        if ((int)c->textWidth(r2) > maxw) brk = 0;
    }
    if (!brk) {                                                       // one long word: a hyphen at a syllable
        auto vow = [](char ch) { return strchr("aeiouyAEIOUY", ch) != nullptr; };
        for (int i = fit - 1; i >= 3 && !brk; i--) {                  // ...a vowel, then consonant + vowel
            snprintf(l1, sizeof l1, "%.*s-", i, lab);
            if ((int)c->textWidth(l1) <= maxw && vow(lab[i - 1]) && !vow(lab[i]) && vow(lab[i + 1]) && n - i >= 3) brk = i;
        }
        if (!brk) brk = fit > 1 ? fit - 1 : 1;
        hyph = true;
    }
    snprintf(l1, sizeof l1, hyph ? "%.*s-" : "%.*s", brk, lab);
    const char *rest = lab + brk; while (*rest == ' ') rest++;
    snprintf(l2, sizeof l2, "%s", rest);
    if ((int)c->textWidth(l2) > maxw) {                               // still too long: "prefix.."
        int k = (int)strlen(l2);
        for (; k > 0; k--) { char t[26]; snprintf(t, sizeof t, "%.*s..", k, rest); if ((int)c->textWidth(t) <= maxw) break; }
        snprintf(l2, sizeof l2, "%.*s..", k, rest);
    }
    c->drawString(l1, cx, y - 1);
    c->drawString(l2, cx, y + 11);
    c->setTextDatum(top_left);
}

// The Back tile (cell 0 inside a category): a chevron on a quiet badge, labelled like the tiles.
template <typename T> static void grid_back_tile(T *c, int cx, int top)
{
    const int s = GRID_BADGE, x = cx - s / 2, y = top + 4, cy = y + s / 2 - 1;
    grid_badge(c, x, y, s, mix565(LINE, FG, 0.18f), BG);
    for (int k = 0; k < 4; k++) {                                   // a 4 px-thick chevron
        c->drawLine(cx + 4 + k - 2, cy - 10, cx - 7 + k - 2, cy, FG);
        c->drawLine(cx - 7 + k - 2, cy, cx + 4 + k - 2, cy + 10, FG);
    }
    grid_label(c, TR5("Indietro", "Back", "Atras", "Retour", "Zurueck"), cx, top + 43, GRID_CELLW - 6, mix565(BG, FG, 0.80f), BG);
}


// ---- the quick menu (panel) ----------------------------------------------------------------------------------
// A long press on a tile (or '*') opens a card over the grid for that app: Open, Pin to Home / Unpin, Info, and
// Uninstall for an app the store installed (asked first). Keys: up/down move, Enter does it, Esc steps back; a tap
// does a row at once, a tap outside closes. Info turns the card into the app's facts. ~170 B of state.
extern "C" bool nucleo_store_app_facts(const char *id, char *version, int vn, char *kind, int kn, char *desc, int dn);
extern "C" bool nucleo_store_app_uninstall(const char *id);
enum { QM_OPEN, QM_PIN, QM_INFO, QM_REMOVE };
enum { QM_ACTIONS, QM_FACTS, QM_CONFIRM };
static int  s_qm_item = -1, s_qm_sel = 0, s_qm_mode = QM_ACTIONS, s_qm_n = 0;
static int8_t s_qm_act[4];
static bool s_qm_store = false;
static char s_qm_ver[16], s_qm_kind[8], s_qm_desc[124];
static const void *s_qm_node = nullptr;                    // the menu it was opened in (a change closes it)

static const MenuNode *qm_it(void)
{
    if (s_qm_item < 0 || (const void *)launcher_node() != s_qm_node) { s_qm_item = -1; return nullptr; }
    const MenuNode *it = launcher_nth_visible(s_qm_item);
    if (!it) s_qm_item = -1;
    return it;
}
bool launcher_qm_active(void) { return lr_large() && qm_it() != nullptr; }

void launcher_qm_open(int item)
{
    s_qm_item = item; s_qm_node = (const void *)launcher_node(); s_qm_sel = 0; s_qm_mode = QM_ACTIONS; s_qm_n = 0;
    const MenuNode *it = qm_it();
    if (!it) return;
    s_qm_store = it->kind == N_APP && nucleo_store_app_facts(it->id, s_qm_ver, sizeof s_qm_ver, s_qm_kind, sizeof s_qm_kind, s_qm_desc, sizeof s_qm_desc);
    s_qm_act[s_qm_n++] = QM_OPEN;
    if (it->kind == N_APP) s_qm_act[s_qm_n++] = QM_PIN;
    s_qm_act[s_qm_n++] = QM_INFO;
    if (s_qm_store) s_qm_act[s_qm_n++] = QM_REMOVE;
}
static void qm_close(void) { s_qm_item = -1; }

// 0 = not open (the grid takes the key), 1 = done (repaint), 2 = open the app (the caller launches it).
int launcher_qm_key(int key, char ch)
{
    const MenuNode *it = qm_it();
    if (!it) return 0;
    const bool row = nucleo_touch_is_row(key, ch);
    const int slot = row ? ch - NUCLEO_TOUCH_ROW : -1;
    if (nucleo_touch_is_hold(key, ch)) return 1;
    if (s_qm_mode == QM_FACTS) { if (key == NK_BACK || key == NK_ENTER || row) s_qm_mode = QM_ACTIONS; return 1; }
    if (s_qm_mode == QM_CONFIRM) {
        if (key == NK_LEFT || key == NK_RIGHT || key == NK_UP || key == NK_DOWN) s_qm_sel ^= 1;
        else if (key == NK_BACK || (key == NK_CHAR && (ch == 'n' || ch == 'N'))) { s_qm_mode = QM_ACTIONS; s_qm_sel = 0; }
        else if (key == NK_ENTER || row || (key == NK_CHAR && (ch == 'y' || ch == 'Y' || ch == 's' || ch == 'S'))) {
            const bool yes = row ? slot == 1 : key == NK_ENTER ? s_qm_sel == 1 : true;
            if (yes) { char id[33]; snprintf(id, sizeof id, "%s", it->id); qm_close(); nucleo_store_app_uninstall(id); }
            else { s_qm_mode = QM_ACTIONS; s_qm_sel = 0; }
        }
        return 1;
    }
    if (key == NK_UP)   { s_qm_sel = (s_qm_sel + s_qm_n - 1) % s_qm_n; return 1; }
    if (key == NK_DOWN) { s_qm_sel = (s_qm_sel + 1) % s_qm_n; return 1; }
    if (key == NK_BACK || (key == NK_CHAR && ch == '*')) { qm_close(); return 1; }
    if (key != NK_ENTER && !(row && slot < s_qm_n)) return 1;      // the card is modal: other keys do nothing
    switch (s_qm_act[row ? slot : s_qm_sel]) {
        case QM_OPEN:   qm_close(); return 2;
        case QM_PIN:    launcher_toggle_pin(it->id); qm_close(); return 1;
        case QM_INFO:   s_qm_mode = QM_FACTS; return 1;
        case QM_REMOVE: s_qm_mode = QM_CONFIRM; s_qm_sel = 0; return 1;
    }
    return 1;
}

// Word-wrapped Font2 text in a box, at most `maxl` lines (the last ends in ".." when cut). Returns the lines drawn.
template <typename T> static int qm_wrap(T *c, const char *s, int x, int y, int w, int maxl, uint16_t col, uint16_t bg)
{
    c->setFont(&fonts::Font2); c->setTextSize(1); c->setTextColor(col, bg); c->setTextDatum(top_left);
    char ln[48]; int lines = 0;
    while (*s && lines < maxl) {
        int n = 0, cut = 0;
        while (s[n] && n < (int)sizeof ln - 3) {
            ln[n] = s[n]; ln[n + 1] = 0;
            if ((int)c->textWidth(ln) > w) break;
            if (s[n] == ' ') cut = n;
            n++;
        }
        if (!s[n]) cut = n; else if (!cut) cut = n;
        ln[cut] = 0;
        if (lines == maxl - 1 && s[cut]) { while (cut > 0 && (int)c->textWidth(ln) + 10 > w) ln[--cut] = 0; strcat(ln, ".."); }
        c->drawString(ln, x, y + lines * 15);
        s += cut; while (*s == ' ') s++;
        lines++;
    }
    return lines;
}

// A trash can (no glyph for it in the set), centred at (cx, cy).
template <typename T> static void qm_trash(T *c, int cx, int cy, uint16_t col)
{
    c->fillRect(cx - 6, cy - 6, 12, 2, col); c->fillRect(cx - 2, cy - 8, 4, 2, col);
    c->drawRect(cx - 5, cy - 3, 10, 10, col); c->drawRect(cx - 4, cy - 3, 8, 10, col);
    c->drawFastVLine(cx - 1, cy - 1, 6, col); c->drawFastVLine(cx + 2, cy - 1, 6, col);
}

static const char *qm_label(int act, const MenuNode *it)
{
    switch (act) {
        case QM_OPEN: return TR5("Apri", "Open", "Abrir", "Ouvrir", "Oeffnen");
        case QM_PIN:  return launcher_is_pinned(it->id) ? TR5("Togli da Home", "Unpin from Home", "Quitar de inicio", "Retirer de l'accueil", "Von Home loesen")
                                                         : TR5("Fissa in Home", "Pin to Home", "Fijar en inicio", "Epingler a l'accueil", "An Home anheften");
        case QM_INFO: return TR5("Info", "Info", "Info", "Infos", "Info");
        default:      return TR5("Disinstalla", "Uninstall", "Desinstalar", "Desinstaller", "Entfernen");
    }
}

// Where the card sits (band coordinates) and how tall it is for what it shows.
static int qm_card_h(void) { return s_qm_mode == QM_ACTIONS ? 58 + s_qm_n * 34 + 6 : s_qm_mode == QM_FACTS ? 186 : 136; }
enum { QM_W = 236 };

template <typename T> static void qm_draw(T *c, int base)
{
    const MenuNode *it = qm_it();
    if (!it) return;
    const int h = qm_card_h(), x = (W - QM_W) / 2, y = base + (LIST_BAND_H - h) / 2;
    const uint16_t card = mix565(BG, FG, 0.09f), edge = mix565(card, it->color, 0.55f);
    c->fillRoundRect(x + 3, y + 5, QM_W, h, 14, mix565(BG, 0x0000, 0.65f));           // shadow
    c->fillRoundRect(x, y, QM_W, h, 14, card);
    c->drawRoundRect(x, y, QM_W, h, 14, edge);
    c->drawFastHLine(x + 14, y + 1, QM_W - 28, mix565(card, 0xFFFF, 0.18f));           // light along the top
    // the header: the app's badge, its whole name, where it comes from
    grid_badge(c, x + 12, y + 10, GRID_BADGE, it->color, card);
    const char *gid = !strcmp(it->id, LAUNCHER_RECENT_ID) ? "clock" : !strcmp(it->id, LAUNCHER_SETTINGS_SEARCH_ID) ? "wifi" : it->id;
    ui_icon(c, x + 12 + GRID_BADGE / 2, y + 10 + GRID_BADGE / 2 - 1, (int)(GRID_BADGE / 2 * 0.68f + 0.5f), gid, it->icon, mix565(BG, FG, 0.95f), it->color);
    char t[40];
    c->setFont(&fonts::FreeSansBold9pt7b); c->setTextSize(1);
    snprintf(t, sizeof t, "%s", node_label(it));
    while (t[0] && (int)c->textWidth(t) > QM_W - 70) t[strlen(t) - 1] = 0;
    c->setTextColor(FG, card); c->setTextDatum(top_left); c->drawString(t, x + 62, y + 11);
    char sub[48];
    if (s_qm_store) snprintf(sub, sizeof sub, "Store - %s%s%s", !strcmp(s_qm_kind, "native") ? "Native C" : "Web app", s_qm_ver[0] ? "  v" : "", s_qm_ver);
    else if (it->kind == N_MENU) snprintf(sub, sizeof sub, TR5("%d app", "%d apps", "%d apps", "%d apps", "%d Apps"), node_child_count(it));
    else snprintf(sub, sizeof sub, "%s", TR5("Integrata nel sistema", "Built into the system", "Integrada en el sistema", "Integree au systeme", "Im System integriert"));
    c->setFont(&fonts::Font2); c->setTextColor(MUTED, card); c->drawString(sub, x + 62, y + 31);
    c->drawFastHLine(x + 10, y + 56, QM_W - 20, mix565(card, FG, 0.12f));
    if (s_qm_mode == QM_ACTIONS) {
        for (int k = 0; k < s_qm_n; k++) {
            const int ry = y + 60 + k * 34, a = s_qm_act[k];
            const bool f = k == s_qm_sel, danger = a == QM_REMOVE;
            const uint16_t acc = danger ? C_RED : it->color, rbg = f ? mix565(card, acc, 0.30f) : card;
            if (f) { c->fillRoundRect(x + 6, ry, QM_W - 12, 31, 9, rbg); c->drawRoundRect(x + 6, ry, QM_W - 12, 31, 9, acc); }
            const int gx = x + 26, gy = ry + 15;
            c->fillCircle(gx, gy, 11, f ? acc : mix565(card, acc, 0.35f));
            const uint16_t gi = f ? INK : FG, gb = f ? acc : mix565(card, acc, 0.35f);
            if (a == QM_OPEN) ui_glyph(c, UG_NEXT, gx, gy, 7, gi, gb);
            else if (a == QM_PIN) ui_glyph(c, UG_STAR, gx, gy, 7, gi, gb);
            else if (a == QM_INFO) ui_glyph(c, UG_INFO, gx, gy, 7, gi, gb);
            else qm_trash(c, gx, gy, gi);
            c->setFont(&fonts::FreeSans9pt7b); c->setTextColor(danger && !f ? C_RED : FG, rbg);
            c->setTextDatum(middle_left); c->drawString(qm_label(a, it), x + 46, gy + 1);
        }
    } else if (s_qm_mode == QM_FACTS) {
        const char *k1 = TR5("Categoria", "Category", "Categoria", "Categorie", "Kategorie");
        const char *k2 = TR5("In Home", "On Home", "En inicio", "Accueil", "Auf Home");
        const char *k3 = TR5("Origine", "Source", "Origen", "Origine", "Herkunft");
        char v3[32];
        if (s_qm_store) snprintf(v3, sizeof v3, "Store %s", s_qm_ver[0] ? s_qm_ver : "");
        else snprintf(v3, sizeof v3, "%s", TR5("Sistema", "System", "Sistema", "Systeme", "System"));
        const char *cat = launcher_depth() > 0 ? node_label(launcher_node()) : "Home";
        const char *rows[3][2] = { { k1, cat }, { k2, it->kind == N_APP && launcher_is_pinned(it->id) ? TR5("si", "yes", "si", "oui", "ja") : "-" }, { k3, v3 } };
        c->setFont(&fonts::Font2);
        for (int r = 0; r < 3; r++) {
            const int ry = y + 64 + r * 18;
            c->setTextDatum(top_left); c->setTextColor(MUTED, card); c->drawString(rows[r][0], x + 14, ry);
            c->setTextDatum(top_right); c->setTextColor(FG, card); c->drawString(rows[r][1], x + QM_W - 14, ry);
        }
        c->drawFastHLine(x + 10, y + 120, QM_W - 20, mix565(card, FG, 0.12f));
        const char *d = s_qm_desc[0] ? s_qm_desc
                      : it->kind == N_MENU ? TR5("Una categoria del launcher: tocca per aprirla.", "A launcher category: tap to open it.", "Una categoria del lanzador: toca para abrirla.", "Une categorie du lanceur : touchez pour l'ouvrir.", "Eine Launcher-Kategorie: zum Oeffnen tippen.")
                      : s_qm_store ? "" : TR5("Integrata nel sistema NucleoOS.", "Built into NucleoOS.", "Integrada en NucleoOS.", "Integree a NucleoOS.", "In NucleoOS integriert.");
        qm_wrap(c, d, x + 14, y + 126, QM_W - 28, 3, FG, card);
        c->setFont(&fonts::Font2); c->setTextColor(DIM, card); c->setTextDatum(bottom_center);
        c->drawString(TR5("tocca o Esc per tornare", "tap or Esc to go back", "toca o Esc para volver", "touchez ou Esc pour revenir", "tippen oder Esc: zurueck"), x + QM_W / 2, y + h - 6);
    } else {                                                                        // QM_CONFIRM
        c->setFont(&fonts::FreeSans9pt7b); c->setTextColor(FG, card); c->setTextDatum(top_center);
        c->drawString(TR5("Disinstallare?", "Uninstall?", "Desinstalar?", "Desinstaller ?", "Entfernen?"), x + QM_W / 2, y + 64);
        const int bw = (QM_W - 36) / 2, by = y + h - 46;
        for (int k = 0; k < 2; k++) {
            const int bx = x + 12 + k * (bw + 12);
            const bool f = k == s_qm_sel;
            const uint16_t acc = k ? C_RED : mix565(card, FG, 0.35f), fill = f ? acc : mix565(card, acc, 0.25f);
            c->fillRoundRect(bx, by + 3, bw, 36, 10, mix565(card, 0x0000, 0.5f));
            c->fillRoundRect(bx, by, bw, 36, 10, fill);
            c->setFont(&fonts::FreeSansBold9pt7b); c->setTextColor(f ? (k ? INK : FG) : FG, fill); c->setTextDatum(middle_center);
            c->drawString(k ? TR5("Disinstalla", "Uninstall", "Desinstalar", "Desinstaller", "Entfernen") : "No", bx + bw / 2, by + 18);
        }
    }
    c->setTextDatum(top_left); c->setFont(&fonts::Font0); c->setTextSize(1);
}

// The card's touch targets (panel coordinates), and a tap outside it closes it.
static void qm_register_touch(void)
{
    const int h = qm_card_h(), x = (W - QM_W) / 2, y = LIST_TOP + (LIST_BAND_H - h) / 2;
    if (s_qm_mode == QM_ACTIONS)
        for (int k = 0; k < s_qm_n; k++) nucleo_touch_add(x + 6, y + 60 + k * 34, QM_W - 12, 31, NK_CHAR, (char)(NUCLEO_TOUCH_ROW + k));
    else if (s_qm_mode == QM_CONFIRM) {
        const int bw = (QM_W - 36) / 2, by = y + h - 46;
        for (int k = 0; k < 2; k++) nucleo_touch_add(x + 12 + k * (bw + 12), by, bw, 36, NK_CHAR, (char)(NUCLEO_TOUCH_ROW + k));
    } else nucleo_touch_add(x, y, QM_W, h, NK_BACK, 0);
    nucleo_touch_add(x, y, QM_W, h, NK_CHAR, 0x01);         // the rest of the card: nothing (not a close)
    nucleo_touch_add(0, 0, W, H, NK_BACK, 0);              // outside: close
}

// The item under a panel point (a long press): its visible-list index, or -1.
int launcher_render_grid_item_at(int px, int py)
{
    if (!lr_large() || py < LIST_TOP || py >= LIST_TOP + LIST_BAND_H) return -1;
    grid_fit();
    const int row = (py - LIST_TOP + s_grid_off - GRID_TOP) / GRID_RH, col = px / GRID_CELLW;
    const int i = row * GRID_COLS + col - grid_lead();
    return col < GRID_COLS && i >= 0 && i < launcher_visible_count() ? i : -1;
}

template <typename T> static void draw_grid(T *c, int base)
{
    c->fillRect(0, base, W, LIST_BAND_H, BG);
    int n = launcher_visible_count(), lead = grid_lead();
    if (n == 0 && !lead) {
        const char *none = launcher_filter()[0] ? TR5("Nessun risultato", "No matches", "Sin resultados", "Aucun resultat", "Keine Treffer")
                                                 : TR5("Nessuna app", "No apps", "Ninguna app", "Aucune app", "Keine Apps");
        c->setFont(&fonts::FreeSans9pt7b); c->setTextColor(DIM, BG);
        c->setTextDatum(middle_center); c->drawString(none, W / 2, base + LIST_BAND_H / 2);
        c->setTextDatum(top_left); c->setFont(&fonts::Font0); c->setTextSize(1);
        return;
    }
    grid_fit();
    int sel = launcher_sel(), cells = n + lead, rows = (cells + GRID_COLS - 1) / GRID_COLS;
    static unsigned s_eq_frame = 0; s_eq_frame++;
    bool audio_on = nucleo_audio_is_playing() && !nucleo_audio_is_paused();
    const char *ap = audio_on ? nucleo_audio_path() : "";
    bool is_stream = ap && strstr(ap, "://");

    c->setClipRect(0, base, W, LIST_BAND_H);                        // rows a drag leaves cut stay inside the band
    for (int row = 0; row < rows; row++) {
        const int top = base + GRID_TOP + row * GRID_RH - s_grid_off;
        if (top + GRID_RH <= base || top >= base + LIST_BAND_H) continue;
        for (int j = 0; j < GRID_COLS; j++) {
            const int cell = row * GRID_COLS + j, i = cell - lead;
            if (cell >= cells) break;
            const int x0 = j * GRID_CELLW, cx = x0 + GRID_CELLW / 2;
            if (i < 0) { grid_back_tile(c, cx, top); continue; }
            const MenuNode *it = launcher_nth_visible(i);
            if (!it) continue;
            const bool scrim = s_qm_item >= 0;                      // the quick menu is open: the grid steps back
            const bool focus = (i == sel) && !scrim;
            // Keyboard focus: a tinted card with the item's colour as its rim, wholly inside the cell (a row is
            // always shown whole, so it is never cut), the badge a touch larger.
            const uint16_t under = focus ? mix565(BG, it->color, 0.18f) : BG;
            if (focus) {
                c->fillRoundRect(x0 + 3, top + 1, GRID_CELLW - 6, GRID_RH - 2, 12, under);
                c->drawRoundRect(x0 + 3, top + 1, GRID_CELLW - 6, GRID_RH - 2, 12, it->color);
                c->drawRoundRect(x0 + 4, top + 2, GRID_CELLW - 8, GRID_RH - 4, 11, mix565(under, it->color, 0.55f));
            }
            const int s = focus ? GRID_BADGE + 2 : GRID_BADGE, bx = cx - s / 2, by = top + (focus ? 3 : 4);
            const uint16_t badge = focus ? it->color : scrim ? mix565(BG, it->color, 0.28f) : mix565(LINE, it->color, 0.85f);
            grid_badge(c, bx, by, s, badge, under);
            const int r = s / 2, cy = by + r - 1;
            uint16_t gcol = focus ? INK : scrim ? mix565(BG, FG, 0.35f) : mix565(BG, FG, 0.95f);
            const char *gid = !strcmp(it->id, LAUNCHER_RECENT_ID) ? "clock"
                            : !strcmp(it->id, LAUNCHER_SETTINGS_SEARCH_ID) ? "wifi" : it->id;
            ui_icon(c, cx, cy, (int)(r * 0.68f + 0.5f), gid, it->icon, gcol, badge);
            if (it->kind == N_MENU)
                draw_badge(c, bx + s - 3, by + 3, node_child_count(it), it->color);
            else if (launcher_is_pinned(it->id)) {
                const int px = bx + s - 3, py = by + 3;
                c->fillCircle(px, py, 5, INK); c->fillCircle(px, py, 4, C_YELLOW);
            }
            if (audio_on && ((is_stream && !strcmp(it->id, "radio")) || (!is_stream && !strcmp(it->id, "music"))))
                draw_eq(c, cx + (int)(r * 0.6f), cy + (int)(r * 0.6f), (int)(r * 0.55f), s_eq_frame);
            grid_label(c, node_label(it), cx, top + 43, GRID_CELLW - 6, focus ? FG : mix565(BG, FG, scrim ? 0.30f : 0.78f), under);
        }
    }
    c->clearClipRect();
    // the scroll knob on the right edge when the grid overflows: a rounded pill on a faint track
    if (s_grid_max > 0) {
        int track = LIST_BAND_H - 10, content = LIST_BAND_H + s_grid_max, kh = track * LIST_BAND_H / content; if (kh < 18) kh = 18;
        int ky = base + 5 + (track - kh) * s_grid_off / s_grid_max;
        c->fillRoundRect(W - 4, base + 5, 3, track, 1, mix565(BG, LINE, 0.7f));
        c->fillRoundRect(W - 4, ky, 3, kh, 1, mix565(FG, BG, 0.25f));
    }
    if (s_qm_item >= 0) qm_draw(c, base);                           // the quick menu over the grid
    c->setFont(&fonts::Font0); c->setTextSize(1);
}

// ---- the launcher carousel --------------------------------------------------
// Rendered into a band-local coordinate space (top = `base`): for the off-screen canvas base = 0 and
// the result is pushed at y = LIST_TOP; for the direct fallback base = LIST_TOP. The focused item is a
// big centred badge; neighbours peek smaller/dimmer; the rail slides via s_carousel (eased in
// launcher_render_step_scroll).
template <typename T> static void draw_list(T *c, int base)
{
    if (lr_large()) { draw_grid(c, base); return; }     // the panel shows a touch grid, not the scaled rail
    if (nucleo_theme_has_bg_image()) nucleo_theme_draw_bg_slice(c, 0, base, W, LIST_BAND_H);   // the image is 240x135
    else                            c->fillRect(0, base, W, LIST_BAND_H, BG);

    int n = launcher_visible_count();
    if (n == 0) {
        const char *none = TR5("Nessuna app", "No apps", "Ninguna app", "Aucune app", "Keine Apps");
        c->setFont(&fonts::Font2); c->setTextColor(DIM, BG);
        c->setCursor((W - (int)c->textWidth(none)) / 2, base + LIST_BAND_H / 2 - 8); c->print(none);
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
        int cy = iconCY + (int)((1.0f - t) * (lr_large() ? 10.0f : 6.0f) + 0.5f);   // neighbours drop a touch (arc/depth)
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
        const char *gid = !strcmp(it->id, LAUNCHER_RECENT_ID) ? "clock"                   // Recent wears a clock,
                        : !strcmp(it->id, LAUNCHER_SETTINGS_SEARCH_ID) ? "wifi" : it->id;   // the Settings result the Settings glyph
        ui_icon(c, x, cy, gr, gid, it->icon, gcol, badge);
        if (it->kind == N_MENU && t > 0.85f)                            // category: count rosette on the top-right corner
            draw_badge(c, x + (int)(r * 0.82f), cy - (int)(r * 0.82f), node_child_count(it), it->color);
        else if (it->kind == N_APP && t > 0.6f && launcher_is_pinned(it->id)) {   // pinned app: yellow dot, top-right
            int px = x + (int)(r * 0.82f), py = cy - (int)(r * 0.82f);
            c->fillCircle(px, py, 4, INK); c->fillCircle(px, py, 3, C_YELLOW);
        }
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
        const char *lab = node_label(cur);                              // localized for category tiles
        c->setFont(&fonts::Font4); int fh = 26;
        if ((int)c->textWidth(lab) > W - 20) { c->setFont(&fonts::Font2); fh = 16; }
        fit_width(c, lab, W - 12, buf, sizeof buf);
        c->setCursor(W / 2 - (int)c->textWidth(buf) / 2, base + CAR_TITLE_Y - fh / 2); c->print(buf);
        draw_dots(c, n, sel, W / 2, base + CAR_DOTS_Y, cur->color);
    }
    c->setFont(&fonts::Font0); c->setTextSize(1);                       // leave the font at the framework default
}

// The list band composites into the shared back-buffer (see nucleo_ui.cpp). We draw the band
// into the top of that canvas and blit only the band region with a destination-clipped push,
// so the chrome below it is untouched. Decoder apps call nucleo_screen_release() directly to
// hand the canvas RAM to the codec; it re-acquires lazily here.
// The large grid's touch targets: one per visible tile (tap = open that app / enter that category), a
// scroll area over the whole grid (a drag walks the rows), and the status-bar breadcrumb as Back inside a
// category. Panel coordinates; registered once per frame (the band itself may blit in several strips).
static void grid_register_touch(void)
{
    nucleo_touch_clear();
    if (launcher_qm_active()) { qm_register_touch(); nucleo_touch_commit(); return; }
    const int n = launcher_visible_count(), lead = grid_lead(), first = grid_first(), cells = n + lead;
    const int rows = (cells + GRID_COLS - 1) / GRID_COLS, y0 = LIST_TOP, y1 = LIST_TOP + LIST_BAND_H;
    if (lead) nucleo_touch_add(0, 0, 64, BAR + 4, NK_BACK, 0);                             // the Back button
    nucleo_touch_add(lead ? 64 : 0, 0, lead ? W - 64 : W, BAR, NK_TAB, 0);                  // the bar: Control Center
    for (int row = 0; row < rows; row++) {
        const int top = LIST_TOP + GRID_TOP + row * GRID_RH - s_grid_off;
        const int ty = top > y0 ? top : y0, by = top + GRID_RH < y1 ? top + GRID_RH : y1;
        if (by - ty < 20) continue;                                  // a sliver at the edge is not a target
        for (int j = 0; j < GRID_COLS; j++) {
            const int cell = row * GRID_COLS + j, i = cell - lead;
            if (cell >= cells) break;
            if (i < 0) { nucleo_touch_add(j * GRID_CELLW, ty, GRID_CELLW, by - ty, NK_BACK, 0); continue; }
            const int slot = i - first;
            if (slot < 0 || slot >= 16) continue;                    // NUCLEO_TOUCH_ROW has 16 slots
            nucleo_touch_add(j * GRID_CELLW, ty, GRID_CELLW, by - ty, NK_CHAR, (char)(NUCLEO_TOUCH_ROW + slot));
        }
    }
    nucleo_touch_add_scroll(0, LIST_TOP, W, LIST_BAND_H, GRID_STEP, NK_CHAR, NUCLEO_TOUCH_VIEW_BACK, NK_CHAR, NUCLEO_TOUCH_VIEW_FWD);
    nucleo_touch_commit();
}

void launcher_render_list(void)
{
    M5Canvas *c = lr_large() ? nucleo_screen_wide(W) : nucleo_screen();
    if (lr_large()) grid_register_touch();
    s_band_buffered = (c != nullptr);                          // tells step_scroll to snap (not animate) on the direct path
    if (c && c->height() >= LIST_BAND_H) {
        draw_list(c, 0);                                        // band-local: rows 0..LIST_BAND_H of the canvas
        d.setClipRect(0, LIST_TOP, W, LIST_BAND_H);            // land it in the band region only...
        c->pushSprite(0, LIST_TOP);                            // ...one blit -> no flicker (rows below are clipped)
        d.clearClipRect();
    } else if (c) {
        // Large surface: the band (320x218) is taller than the 32 KB canvas seen 320 wide, so it is composited
        // in strips through the SAME bytes — no extra RAM. Each strip is a finished slice of the frame, so the
        // panel never shows a cleared-then-redrawn intermediate. The bytes are seen as RGB565 (~50 rows a strip):
        // the tiles keep their exact theme colours (8-bit RGB332 turned the muted badges olive / grey).
        static M5Canvas s_l16;
        s_l16.setColorDepth(16);
        s_l16.setBuffer(c->getBuffer(), W, c->height() / 2);
        const int sh = s_l16.height();
        for (int s = 0; s < LIST_BAND_H; s += sh) {
            draw_list(&s_l16, -s);                              // band row s lands on canvas row 0
            int h = LIST_BAND_H - s < sh ? LIST_BAND_H - s : sh;
            d.setClipRect(0, LIST_TOP + s, W, h);
            s_l16.pushSprite(&d, 0, LIST_TOP + s);
            d.clearClipRect();
        }
    } else {
        // No canvas (heap fragmented after a radio session): draw direct, batched into ONE SPI
        // transaction so the clear→rows repaint is as quick as possible. Paired with the snap-scroll
        // above (no per-frame animation) this keeps the menu clean instead of a flickery mess.
        d.startWrite();
        draw_list(&d, LIST_TOP);
        d.endWrite();
    }
}

// ---- Control Center overlay (smartwatch quick settings) ----------------------
// Raised with TAB from anywhere. A status strip over a SCROLLING column of four cards, Wear-OS style:
//
//   14:05  CasaNet                 |||  [=] 85%    status strip: clock · network · signal · battery
//   [1 mute] [2 torch] [3 moon] [4 hotspot]        icon tiles — keys 1-4 fire them directly
//   Audio attivo               invio silenzia      caption (Font2): the focused tile + what ENTER does
//   (sun) Luminosita                     70%       brightness card: caption + track (LEFT/RIGHT adjust)
//   (mon) Secondo schermo                80%       ADV with a second screen: its own backlight + [Calibra] touch
//   (spk) Volume  invio muto             40%       volume card (ENTER toggles mute)
//   [5 gear] [6 web] [7 kbd] [8 usb] [9 power]     shortcuts — keys 5-9 fire them directly
//   6 Web 192.168.1.42          PIN 314159         caption: on the Web shortcut, the IP and pairing PIN
//
// The focused card is always fully in view (the column scrolls under the strip, eased when buffered) and a
// knob on the right edge shows where you are. Every caption is Font2 when it fits the column and Font0
// when a translation does not, so no text ever runs off the panel. The viewport ends at the last row THIS
// frame can draw: the back-buffer's height when buffered — on the ADV the canvas is fitted to the heap's
// largest block, 240x130, not 135 — or the panel's when direct; the rows below a short canvas are cleared
// on the panel, so they never keep the previous frame (the launcher footer used to show through there).
//
// Keys: UP/DOWN move between cards (wrap); LEFT/RIGHT move inside a card or adjust a slider; ENTER acts;
// 1-9 select AND fire the n-th tile/shortcut; Esc (or TAB) closes. Focus and scroll are REMEMBERED across
// opens (resume where you left off). Disruptive actions (Hotspot, which drops the Wi-Fi client link, and
// Restart) arm on the first ENTER/digit and fire on the second; any other key disarms — the focus turns
// red and the caption asks.
//
// Colours are theme roles (BG/FG/MUTED/DIM/LINE/INK/THEME_ACC) plus the named semantic C_* accents only.
// Language, theme, USB-drive and network details live in the Settings app (the gear shortcut), so the
// panel stays a panel. Zero .bss beyond a few focus bytes; composited into the shared back-buffer.
#include "ui_glyph.h"

extern "C" bool        nucleo_setup_ap_intended(void);   // hotspot chosen by the user (not a rescue fallback)
extern "C" bool        nucleo_setup_ap_active(void);     // SoftAP reachable right now
extern "C" const char *nucleo_setup_ap_ssid(void);
extern "C" void        nucleo_setup_start_ap(void);
extern "C" void        nucleo_setup_stop_ap(void);
extern "C" bool        nucleo_setup_config_loaded(void); // false in Wi-Fi-skipped Solo boots (BLE / NX_WIFI / USB-web)

// launcher_render_control_center_key return codes (see launcher_render.h).
enum { CC_NONE = 0, CC_REDRAW = 1, CC_CLOSE = 2, CC_SCREEN_OFF = 3, CC_LAUNCH = 4, CC_TORCH = 5 };
enum { CL_TILES = 0, CL_BRIGHT, CL_XBRIGHT, CL_VOLUME, CL_SHORT, CC_NLINES };   // the focusable lines (CL_XBRIGHT: ADV + panel)
enum { TL_MUTE = 0, TL_TORCH, TL_SCREEN, TL_HOTSPOT, CC_NTILES };          // toggle tiles
enum { SC_SETTINGS = 0, SC_WEB, SC_USBKBD, SC_USBDRIVE, SC_RESTART, CC_NSHORT };   // shortcuts
#define CC_ARM_HOTSPOT  TL_HOTSPOT
#define CC_ARM_RESTART  (100 + SC_RESTART)

// Geometry. The body is a virtual column scrolled under the strip; every element keeps a 2 px focus-ring
// margin that never shares a row with a neighbour's ring, so a focus move only redraws two outlines.
// Geometry: the compact numbers (240x135, a scrolling column) and the large ones (an external 320x240 panel,
// where the whole column fits at once with bigger tiles and full-width sliders). Read at draw time.
static const int CC_STRIP_H = 18;
static const int CC_BODY_Y  = CC_STRIP_H + 1;                  // first body row (below the strip rule)
static const int CC_CARD_H_S[CC_NLINES] = { 58, 34, 34, 34, 52 };   // card heights: tiles, bright, 2nd-screen bright, volume, shortcuts
static const int CC_CARD_H_L[CC_NLINES] = { 64, 40, 40, 40, 58 };
#define CC_X0       (lr_large() ? 6 : 4)                        // card column; the scroll knob sits right of it
#define CC_XW       (lr_large() ? 308 : 228)
#define CC_KNOB_X   (W - 4)                                     // 2 px scroll knob
// The second screen's card exists only with a panel attached that has something to set: its backlight (the
// module's LED on a GPIO) and/or its touch (calibration). Otherwise it has no height and the column is the
// four cards it always was.
static bool cc_xbri_ok(void) { return nucleo_extdisp_brightness_ok(); }
static bool cc_cal_ok(void)  { return nucleo_extdisp_active() && nucleo_extdisp_touch(); }
static bool cc_line_on(int line) { return line != CL_XBRIGHT || (nucleo_extdisp_active() && (cc_xbri_ok() || cc_cal_ok())); }
static int  cc_card_h(int line)  { return cc_line_on(line) ? (lr_large() ? CC_CARD_H_L : CC_CARD_H_S)[line] : 0; }
// Card tops in the column: stacked with a 4 px gap (none under the compact tiles, whose caption fills it).
static int  cc_card_y(int line)
{
    int y = lr_large() ? 2 : 0;
    for (int i = 0; i < line; i++) if (cc_card_h(i)) y += cc_card_h(i) + ((i == CL_TILES && !lr_large()) ? 0 : 4);
    return y;
}
#define CC_COL_H    (cc_card_y(CL_SHORT) + cc_card_h(CL_SHORT)) // the whole column (186 compact / 216 large without CL_XBRIGHT)
#define CC_TILE_W   (lr_large() ? 72 : 54)
#define CC_TILE_P   (lr_large() ? 78 : 58)
#define CC_TILE_H   (lr_large() ? 40 : 34)
#define CC_TILE_DY  3
#define CC_TILE_CAP (lr_large() ? 46 : 40)
#define CC_SHORT_W  (lr_large() ? 56 : 42)
#define CC_SHORT_P  (lr_large() ? 62 : 46)
#define CC_SHORT_H  (lr_large() ? 34 : 26)
#define CC_SHORT_DY 3
#define CC_SHORT_CAP (lr_large() ? 40 : 33)
#define CC_GLYPH_T  (lr_large() ? 14 : 10)                      // tile / shortcut icon radius
#define CC_GLYPH_S  (lr_large() ? 11 : 8)
static const int CC_CAP_H = 16;                                // one caption line (Font2 height)
#define CC_TRK_X    (lr_large() ? 16 : 12)                      // slider track in its card
#define CC_TRK_W    (lr_large() ? 288 : 212)
#define CC_TRK_H    (lr_large() ? 8 : 6)
#define CC_TRK_DY   (lr_large() ? 26 : 24)
#define CC_CAL_W    76                                          // large: the [Calibra] pill right of the panel's track
// A slider's track width: on the large panel the second screen's track leaves room for its Calibrate pill.
static int cc_trk_w(int line) { return (line == CL_XBRIGHT && lr_large() && cc_cal_ok()) ? CC_TRK_W - CC_CAL_W - 10 : CC_TRK_W; }

static int  s_cc_scroll = 0;           // body scroll in rows — kept across opens with the focus (resume)
static bool s_cc_anim   = false;       // buffered scroll still easing toward the focused card
static bool s_cc_snap   = true;        // the next paint jumps straight to the target (set on open)

static int  s_cc_line  = CL_TILES;     // focused line — kept across opens (resume)
static int  s_cc_tile  = 0;            // focused tile
static int  s_cc_short = 0;            // focused shortcut
static int  s_cc_arm   = -1;           // armed disruptive action (CC_ARM_*), -1 = none
static bool s_cc_prefs = false;        // brightness/volume moved: persist ONCE on close, not per key
static const char *s_cc_launch_id = nullptr;   // set before returning CC_LAUNCH

static bool cc_online(void) { return !strcmp(nucleo_setup_mode(), "sta") && nucleo_setup_ip()[0]; }

// Hotspot needs this boot's setup config: in a Wi-Fi-skipped Solo boot s_mode is only the RAM default
// ("ap"), so the tile would lie ON and toggling it would persist defaults over setup.json + wake ~48 KB
// of Wi-Fi next to NimBLE. There the tile is drawn disabled and ignores ENTER.
static bool cc_hotspot_ok(void) { return nucleo_setup_config_loaded(); }

static bool cc_tile_on(int i)
{
    if (i == TL_MUTE)    return nucleo_audio_is_muted();
    if (i == TL_HOTSPOT) return cc_hotspot_ok() && nucleo_setup_ap_intended();
    return false;                                          // Torch / Screen off are momentary actions
}
static unsigned short cc_tile_col(int i) { return i == TL_MUTE ? C_RED : i == TL_HOTSPOT ? C_YELLOW : THEME_ACC; }
static int cc_tile_glyph(int i) { return i == TL_MUTE ? UG_MUTE : i == TL_TORCH ? UG_TORCH : i == TL_SCREEN ? UG_MOON : UG_HOTSPOT; }
static int cc_short_glyph(int i)
{
    static const int G[CC_NSHORT] = { UG_GEAR, UG_MONITOR, UG_KEYBOARD, UG_USB, UG_POWER };
    return G[i];
}

void launcher_render_control_center_invalidate(void);
void launcher_render_control_center_open(void)          // focus kept: resume; first paint is a full one
{
    s_cc_arm = -1; s_cc_prefs = false; s_cc_snap = true; launcher_render_control_center_invalidate();
}

// Forget the remembered focus and scroll (the first-boot state). The UI host scenes start from it, so a
// golden never depends on which panel scene ran before it in the same process.
void launcher_render_control_center_reset(void)
{
    s_cc_line = CL_TILES; s_cc_tile = 0; s_cc_short = 0; s_cc_scroll = 0; s_cc_arm = -1; s_cc_snap = true;
}

void launcher_render_control_center_close(void)
{
    nucleo_touch_clear(); nucleo_touch_commit();               // its tile/shortcut targets end with it
    s_cc_arm = -1;
    if (s_cc_prefs) {                                          // one settings.json write per visit
        s_cc_prefs = false; nucleo_app_persist_prefs();
        if (nucleo_extdisp_brightness_ok()) nucleo_extdisp_set_brightness(nucleo_extdisp_brightness(), true);
    }
}

const char *launcher_render_control_center_launch_id(void) { return s_cc_launch_id; }

static int cc_tile_act(int i, int armed)
{
    switch (i) {
        case TL_MUTE:   nucleo_audio_set_mute(!nucleo_audio_is_muted()); nucleo_app_persist_prefs(); return CC_REDRAW;
        case TL_TORCH:  return CC_TORCH;
        case TL_SCREEN: return CC_SCREEN_OFF;
        default:
            if (!cc_hotspot_ok()) { s_cc_arm = -1; return CC_NONE; }                 // disabled this boot
            if (armed != CC_ARM_HOTSPOT) { s_cc_arm = CC_ARM_HOTSPOT; return CC_REDRAW; }
            s_cc_arm = -1;
            if (nucleo_setup_ap_intended()) nucleo_setup_stop_ap(); else nucleo_setup_start_ap();
            return CC_REDRAW;
    }
}

static int cc_short_act(int i, int armed)
{
    switch (i) {
        case SC_SETTINGS: s_cc_launch_id = "wifi";   return CC_LAUNCH;
        case SC_WEB:      s_cc_launch_id = "remote"; return CC_LAUNCH;
        case SC_USBKBD:   s_cc_launch_id = "usbkbd"; return CC_LAUNCH;
        case SC_USBDRIVE: s_cc_launch_id = "usb";    return CC_LAUNCH;
        default:
            if (armed != CC_ARM_RESTART) { s_cc_arm = CC_ARM_RESTART; return CC_REDRAW; }
            launcher_render_control_center_close();       // flush a pending brightness/volume first
            esp_restart();
            return CC_NONE;
    }
}

int launcher_render_control_center_key(int key, char ch)
{
    int  armed = s_cc_arm;
    bool digit = (key == NK_CHAR && ch >= '1' && ch < '1' + CC_NTILES);
    bool dshort = (key == NK_CHAR && ch >= '1' + CC_NTILES && ch < '1' + CC_NTILES + CC_NSHORT);
    if (key != NK_ENTER && !digit && !dshort) s_cc_arm = -1;   // any other key disarms a pending action

    if (key == NK_BACK) return CC_CLOSE;
    if (digit) {                                           // 1-4: fire a tile directly (smartwatch quick keys)
        s_cc_line = CL_TILES; s_cc_tile = ch - '1';
        if (armed != s_cc_tile) { armed = -1; s_cc_arm = -1; }
        return cc_tile_act(s_cc_tile, armed);
    }
    if (dshort) {                                          // 5-9: fire a shortcut directly (9 = Restart arms)
        s_cc_line = CL_SHORT; s_cc_short = ch - '1' - CC_NTILES;
        if (armed != 100 + s_cc_short) { armed = -1; s_cc_arm = -1; }
        return cc_short_act(s_cc_short, armed);
    }
    if (nucleo_touch_is_field(key, ch)) {                  // large: a tap on a slider card sets it where the finger is
        int tx = 0, ty = 0;
        if (!nucleo_touch_last(&tx, &ty)) return CC_NONE;
        for (int line = CL_BRIGHT; line <= CL_VOLUME; line++) {
            int oy = CC_BODY_Y + cc_card_y(line) - s_cc_scroll;
            if (!cc_line_on(line) || ty < oy || ty >= oy + cc_card_h(line)) continue;
            if (line == CL_XBRIGHT && !cc_xbri_ok()) return CC_NONE;
            int v = (tx - CC_TRK_X) * 100 / cc_trk_w(line); v = v < 0 ? 0 : v > 100 ? 100 : v;
            s_cc_line = line;
            if (line == CL_BRIGHT)       nucleo_app_set_brightness(v < 10 ? 10 : v);
            else if (line == CL_XBRIGHT) nucleo_extdisp_set_brightness(v, false);
            else                         nucleo_audio_set_volume(v);
            s_cc_prefs = true;
            return CC_REDRAW;
        }
        return CC_NONE;
    }
    if ((key == NK_CHAR && ch == '0') || (key == NK_ENTER && s_cc_line == CL_XBRIGHT)) {   // [Calibra] / ENTER on the card
        if (!cc_cal_ok() || !nucleo_extdisp_recalibrate()) return CC_NONE;
        s_cc_line = CL_XBRIGHT;
        return CC_CLOSE;                                   // the calibration screen takes the panel
    }
    if (key == NK_UP || key == NK_DOWN) {
        do s_cc_line = (s_cc_line + (key == NK_DOWN ? 1 : CC_NLINES - 1)) % CC_NLINES;
        while (!cc_line_on(s_cc_line));
        return CC_REDRAW;
    }
    if (key == NK_LEFT || key == NK_RIGHT) {
        int dir = (key == NK_RIGHT) ? 1 : -1;
        switch (s_cc_line) {
            case CL_TILES:  s_cc_tile  = (s_cc_tile  + dir + CC_NTILES) % CC_NTILES; break;
            case CL_SHORT:  s_cc_short = (s_cc_short + dir + CC_NSHORT) % CC_NSHORT; break;
            case CL_BRIGHT: nucleo_app_set_brightness(nucleo_app_brightness() + dir * 5); s_cc_prefs = true; break;
            case CL_XBRIGHT: if (!cc_xbri_ok()) return CC_NONE;
                             nucleo_extdisp_set_brightness(nucleo_extdisp_brightness() + dir * 5, false); s_cc_prefs = true; break;
            default:        nucleo_audio_set_volume(nucleo_audio_volume() + dir * 5);     s_cc_prefs = true; break;
        }
        return CC_REDRAW;
    }
    if (key == NK_ENTER) {
        if (s_cc_line == CL_TILES)  return cc_tile_act(s_cc_tile, armed);
        if (s_cc_line == CL_SHORT)  return cc_short_act(s_cc_short, armed);
        if (s_cc_line == CL_VOLUME) { nucleo_audio_set_mute(!nucleo_audio_is_muted()); nucleo_app_persist_prefs(); return CC_REDRAW; }
        return CC_NONE;                                    // brightness: LEFT/RIGHT only
    }
    return (armed != s_cc_arm) ? CC_REDRAW : CC_NONE;      // a stray key that only disarmed: repaint
}

// Signature of everything the panel SHOWS (clock minute, battery, link, hotspot, audio, light).
static uint32_t cc_sig(void)
{
    time_t now = time(NULL); struct tm tmv; localtime_r(&now, &tmv);
    uint32_t sig = 2166136261u;
    #define MIX(v) do { sig = (sig ^ (uint32_t)(v)) * 16777619u; } while (0)
    MIX(tmv.tm_hour); MIX(tmv.tm_min); MIX(nucleo_setup_time_synced());
    MIX(nucleo_power_battery_pct());
    MIX(cc_online()); MIX(wifi_bars(nucleo_setup_rssi()));
    MIX(nucleo_setup_ap_active()); MIX(nucleo_setup_ap_intended());
    MIX(nucleo_audio_is_muted()); MIX(nucleo_audio_volume()); MIX(nucleo_app_brightness());
    MIX(nucleo_extdisp_brightness_ok() ? nucleo_extdisp_brightness() : -1); MIX(cc_cal_ok());
    #undef MIX
    return sig;
}
static uint32_t s_cc_drawn_sig = 0;    // latched by every draw (key-driven or tick-driven)

// 1 Hz: true only when the panel's content changed since it was last DRAWN — a static panel is never
// re-blitted, and a key-driven redraw is not repeated by the next tick (ANTI-FLICKER.md).
bool launcher_render_control_center_tick(void) { return cc_sig() != s_cc_drawn_sig; }

// Every loop iteration (~50 Hz), like the launcher list: true while the scroll is still easing toward the
// focused card, so each frame moves it one step. (The 1 Hz tick above moved it once a second: the panel sat
// half-scrolled for seconds after every key.)
bool launcher_render_control_center_animating(void) { return s_cc_anim; }

// ---- drawing ------------------------------------------------------------------------------------
// One painter for both paths. With the shared back-buffer the whole panel is composed and blitted once.
// WITHOUT it (heap fragmented — the usual state on the ADV after Wi-Fi) the panel is drawn straight to
// the display, so every paint is INCREMENTAL: each element remembers what it last put on screen (s_ccs)
// and repaints only what changed, and never by clearing first —
//   · focus = a 2 px ring OUTSIDE the element (outlines only): moving the focus redraws two rings;
//   · a colour change redraws the glyph/label over the same fill (identical shapes, no blank frame);
//   · a slider lifts only its old knob, repaints the track as two non-overlapping pieces, drops the knob;
//   · text lines are fixed-width fields drawn opaque, so a shorter string overwrites a longer one.
// Only opening the panel (or an overlay having painted over it) costs one full paint.
struct CcShown {
    bool     valid;
    int16_t  vh;                       // viewport bottom of the last paint (canvas height or the panel's)
    uint32_t strip, cap[CC_NLINES];    // strip + one caption hash per card
    uint8_t  tile_fill[CC_NTILES];
    uint16_t tile_ink[CC_NTILES], tile_ring[CC_NTILES];
    uint8_t  sl_glyph[3], sl_kr[3];    // sliders: 0 brightness, 1 volume, 2 second-screen brightness
    uint16_t sl_ink[3], sl_ring[3], sl_col[3];
    int16_t  sl_val[3], sl_kx[3];
    uint16_t sh_ring[CC_NSHORT], sh_ink[CC_NSHORT];
    int16_t  clock_r;
};
static CcShown s_ccs;

void launcher_render_control_center_invalidate(void) { s_ccs.valid = false; }

static unsigned short cc_focus_col(bool foc, bool armed) { return foc ? (armed ? C_RED : THEME_ACC) : BG; }

// A key of the Control Center: flat on the Cardputer; on the panel raised - a darker lip at its bottom and a line of
// light along its top edge.
template <typename T> static void cc_raised(T *g, int x, int y, int w, int h, int r, unsigned short fill)
{
    if (!lr_large()) { g->fillRoundRect(x, y, w, h, r, fill); return; }
    g->fillRoundRect(x, y, w, h, r, mix565(fill, 0x0000, 0.30f));
    g->fillRoundRect(x, y, w, h - 3, r, fill);
    g->drawFastHLine(x + r, y + 1, w - 2 * r, mix565(fill, 0xFFFF, 0.28f));   // light along the top edge
}

// 2 px focus ring just outside an element box (never overlaps a neighbour's ring: see the geometry).
template <typename T> static void cc_ring(T *g, int x, int y, int w, int h, int r, unsigned short c)
{
    g->drawRoundRect(x - 1, y - 1, w + 2, h + 2, r + 1, c);
    g->drawRoundRect(x - 2, y - 2, w + 4, h + 4, r + 2, c);
}

// Status strip: clock · network · Wi-Fi gauge · battery. Every piece lands in a fixed cell and is drawn
// opaque, so the 1-minute tick never wipes the strip.
template <typename T> static void cc_strip(T *g, bool full)
{
    time_t now = time(NULL); struct tm tmv; localtime_r(&now, &tmv);
    bool sta = cc_online(), ap = nucleo_setup_ap_active(), synced = nucleo_setup_time_synced();
    int  bpct = nucleo_power_battery_pct(), rssi = nucleo_setup_rssi();
    const char *net = sta ? nucleo_setup_ssid() : ap ? nucleo_setup_ap_ssid() : "offline";
    uint32_t sig = 2166136261u;
    #define MIX(v) do { sig = (sig ^ (uint32_t)(v)) * 16777619u; } while (0)
    MIX(tmv.tm_hour); MIX(tmv.tm_min); MIX(synced); MIX(bpct); MIX(sta); MIX(ap); MIX(wifi_bars(rssi));
    for (const char *p = net; *p; p++) MIX(*p);
    #undef MIX
    if (!full && sig == s_ccs.strip) return;
    s_ccs.strip = sig;

    char t[8]; snprintf(t, sizeof t, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    g->setFont(&fonts::Font2);                                        // the big clock face Home uses
    g->setTextColor(synced ? FG : MUTED, BG);                         // muted = clock never set
    g->setCursor(6, 1); g->print(t);
    int cr = 6 + (int)g->textWidth(t);
    g->setFont(&fonts::Font0); g->setTextSize(1);
    if (!full && s_ccs.clock_r > cr) g->fillRect(cr, 0, s_ccs.clock_r - cr, CC_STRIP_H, BG);   // narrower digits
    s_ccs.clock_r = (int16_t)cr;

    int rx = W - 4;                                                   // right cluster, fixed cells
    if (lr_large()) {                                                 // the close button (the whole strip closes)
        const int xc = W - 12, yc = CC_STRIP_H / 2;
        g->fillRect(W - 24, 0, 24, CC_STRIP_H, BG);
        for (int k = 0; k < 2; k++) { g->drawLine(xc - 5 + k, yc - 5, xc + 4 + k, yc + 4, FG); g->drawLine(xc + 4 + k, yc - 5, xc - 5 + k, yc + 4, FG); }
        rx = W - 28;
    }
    char b[8]; if (bpct >= 0) snprintf(b, sizeof b, "%3d%%", bpct); else snprintf(b, sizeof b, "  --");
    g->setTextColor(FG, BG); g->setCursor(rx - 24, 5); g->print(b); rx -= 24 + 4;
    unsigned short bc = (bpct < 20) ? C_RED : (bpct < 50) ? C_YELLOW : C_GREEN;
    int bx = rx - 14, fw = bpct > 0 ? (10 * bpct) / 100 : 0; if (fw < 1 && bpct > 0) fw = 1; if (fw > 10) fw = 10;
    g->drawRoundRect(bx, 5, 12, 8, 1, MUTED); g->fillRect(bx + 12, 8, 2, 3, MUTED);
    if (fw > 0)  g->fillRect(bx + 1, 6, fw, 6, bc);
    if (fw < 10) g->fillRect(bx + 1 + fw, 6, 10 - fw, 6, BG);
    rx = bx - 8;
    int lvl = sta ? wifi_bars(rssi) : 1; if (lvl < 1) lvl = 1;
    unsigned short on = sta ? C_GREEN : ap ? C_YELLOW : C_RED;       // same colour code as the home bar
    for (int i = 0; i < 4; i++) { int bh = 2 + i * 2; g->fillRect(rx - 19 + i * 5, 4 + 9 - bh, 3, bh, i < lvl ? on : LINE); }
    rx -= 19 + 6;
    int cells = (rx - 58) / 6;                                        // network name: right-aligned fixed field
    if (cells > 0) {
        char nb[32]; if (cells > 30) cells = 30;
        char nm[31]; snprintf(nm, sizeof nm, "%.*s", cells, net);
        snprintf(nb, sizeof nb, "%*s", cells, nm);
        g->setTextColor(sta ? MUTED : ap ? C_YELLOW : C_RED, BG);
        g->setCursor(rx - cells * 6, 5); g->print(nb);
    }
    if (full) g->drawFastHLine(0, CC_STRIP_H, W, LINE);
}

// The viewport clip: the body rows this frame can draw. Pieces that need their own clip (the slider track)
// intersect with it and restore it, so nothing ever lands on the strip or below the viewport.
static int s_cc_vp0 = CC_BODY_Y, s_cc_vp1 = 135;
// A whole band inside the viewport? Text and caption icons are drawn only then: a scrolled edge may cut a
// tile or a track (that reads as "more this way"), never half a line of text.
static bool cc_band_visible(int y, int h) { return y >= s_cc_vp0 && y + h <= s_cc_vp1; }
template <typename T> static void cc_clip_body(T *g) { g->setClipRect(0, s_cc_vp0, W, s_cc_vp1 - s_cc_vp0); }
template <typename T> static void cc_clip_piece(T *g, int x, int y, int w, int h)
{
    int y0 = y > s_cc_vp0 ? y : s_cc_vp0, y1 = (y + h) < s_cc_vp1 ? (y + h) : s_cc_vp1;
    g->setClipRect(x, y0, w, y1 > y0 ? y1 - y0 : 0);
}

// One caption line: `l` left (from x), `r` right-aligned. Font2 when both fit the column, else Font0 — and
// if even that is too wide the right part is dropped, so a long translation never runs off the panel.
// Hash-gated: in direct mode a caption repaints only when its text or colour changes.
template <typename T> static void cc_caption(T *g, int x, int y, const char *l, const char *r,
                                             unsigned short lc, unsigned short rc, int slot, bool full)
{
    uint32_t h = 2166136261u;
    for (const char *p = l; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    h = (h ^ 0x7Cu) * 16777619u;
    for (const char *p = r; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    h = (h ^ (((uint32_t)lc << 16) | rc)) * 16777619u;
    if (!full && h == s_ccs.cap[slot]) return;
    if (!cc_band_visible(y, CC_CAP_H)) return;        // half a line of text is never drawn (scrolled edge)
    s_ccs.cap[slot] = h;
    int avail = CC_X0 + CC_XW - 2 - x;
    g->fillRect(x, y, CC_X0 + CC_XW - x, CC_CAP_H, BG);
    g->setTextSize(1);
    g->setFont(&fonts::Font2);
    int lw = (int)g->textWidth(l), rw = r[0] ? (int)g->textWidth(r) : 0, gap = r[0] ? 10 : 0;
    bool big = lw + rw + gap <= avail;
    if (!big) {
        g->setFont(&fonts::Font0);
        lw = (int)g->textWidth(l); rw = r[0] ? (int)g->textWidth(r) : 0; gap = r[0] ? 6 : 0;
        if (lw + rw + gap > avail) { r = ""; rw = 0; }
    }
    int ty = big ? y : y + 4;                                         // Font0 centred in the 16 px band
    g->setTextColor(lc, BG); g->setCursor(x, ty); g->print(l);
    if (r[0]) { g->setTextColor(rc, BG); g->setCursor(CC_X0 + CC_XW - 2 - rw, ty); g->print(r); }
    g->setFont(&fonts::Font0);
}

static const char *cc_enter(void) { return TR5("invio", "enter", "enter", "enter", "enter"); }

// What a card's caption says: the focused card names its focused control, its live state and the key that
// acts (red while a disruptive action is armed); an unfocused card shows its title (and its quick keys).
static void cc_caption_text(int line, char *l, int lc, char *r, int rc, unsigned short *col, unsigned short *rcol)
{
    bool foc = (s_cc_line == line);
    *col = foc ? FG : MUTED; *rcol = foc ? THEME_ACC : DIM;
    l[0] = r[0] = 0;
    #define SET(L, R) do { snprintf(l, lc, "%s", (L)); snprintf(r, rc, "%s", (R)); } while (0)
    if (line == CL_TILES) {
        if (!foc) { SET(TR5("Rapide", "Quick", "Rapidas", "Rapides", "Schnell"), "1-4"); return; }
        switch (s_cc_tile) {
            case TL_MUTE:
                if (nucleo_audio_is_muted()) SET(TR5("Audio muto", "Sound muted", "Sin sonido", "Son coupe", "Ton aus"),
                                                 TR5("invio riattiva", "enter unmute", "enter activa", "enter retablit", "enter Ton an"));
                else                         SET(TR5("Audio attivo", "Sound on", "Sonido activo", "Son actif", "Ton an"),
                                                 TR5("invio silenzia", "enter mute", "enter silencia", "enter coupe", "enter stumm"));
                return;
            case TL_TORCH:  SET(TR5("Torcia", "Torch", "Linterna", "Torche", "Lampe"),
                                TR5("invio accende", "enter on", "enter enciende", "enter allume", "enter an")); return;
            case TL_SCREEN: SET(TR5("Spegni schermo", "Screen off", "Apagar pantalla", "Eteindre ecran", "Display aus"), cc_enter()); return;
            default:
                if (!cc_hotspot_ok()) { SET(TR5("Hotspot non disponibile", "Hotspot unavailable", "Hotspot no disponible",
                                                "Hotspot indisponible", "Hotspot nicht verfuegbar"), ""); *col = MUTED; return; }
                if (s_cc_arm == CC_ARM_HOTSPOT) {
                    *col = *rcol = C_RED;
                    SET(nucleo_setup_ap_intended() ? TR5("Spegnere hotspot?", "Hotspot off?", "Apagar hotspot?", "Couper hotspot ?", "Hotspot aus?")
                                                   : TR5("Accendere hotspot?", "Hotspot on?", "Activar hotspot?", "Activer hotspot ?", "Hotspot an?"),
                        TR5("invio si", "enter yes", "enter si", "enter oui", "enter ja"));
                    return;
                }
                if (nucleo_setup_ap_intended()) { snprintf(l, lc, "%.16s", nucleo_setup_ap_ssid()); snprintf(r, rc, "192.168.4.1");
                                                  *col = *rcol = C_YELLOW; return; }
                SET(TR5("Hotspot spento", "Hotspot off", "Hotspot apagado", "Hotspot coupe", "Hotspot aus"),
                    TR5("invio x2", "enter x2", "enter x2", "enter x2", "enter x2"));
                return;
        }
    }
    if (line == CL_BRIGHT) {
        snprintf(l, lc, "%s", TR5("Luminosita", "Brightness", "Brillo", "Luminosite", "Helligkeit"));
        snprintf(r, rc, "%d%%", nucleo_app_brightness()); *rcol = *col; return;
    }
    if (line == CL_XBRIGHT) {                              // compact: ENTER calibrates (no pill there), said when focused
        if (foc && cc_cal_ok() && !lr_large())
            snprintf(l, lc, "%s", TR5("2o schermo  invio calibra", "Screen 2  enter calibrate", "Pant. 2  enter calibrar",
                                      "Ecran 2  enter calibrer", "Bild 2  enter kalibrieren"));
        else snprintf(l, lc, "%s", TR5("Secondo schermo", "Second screen", "Segunda pantalla", "Second ecran", "2. Bildschirm"));
        if (cc_xbri_ok()) snprintf(r, rc, "%d%%", nucleo_extdisp_brightness());
        *rcol = *col; return;
    }
    if (line == CL_VOLUME) {
        bool m = nucleo_audio_is_muted();
        if (!foc)   snprintf(l, lc, "%s", TR5("Volume", "Volume", "Volumen", "Volume", "Lautstaerke"));
        else if (m) snprintf(l, lc, "%s", TR5("Volume  invio riattiva", "Volume  enter unmute", "Volumen  enter activa",
                                              "Volume  enter retablit", "Lautst.  enter Ton an"));
        else        snprintf(l, lc, "%s", TR5("Volume  invio muto", "Volume  enter mute", "Volumen  enter mudo",
                                              "Volume  enter muet", "Lautst.  enter stumm"));
        if (m) snprintf(r, rc, "%s", TR5("muto", "muted", "mudo", "muet", "stumm"));
        else   snprintf(r, rc, "%d%%", nucleo_audio_volume());
        *rcol = *col; return;
    }
    if (!foc) { SET(TR5("Scorciatoie", "Shortcuts", "Atajos", "Raccourcis", "Kurzwahl"), "5-9"); return; }
    switch (s_cc_short) {
        case SC_SETTINGS: SET(TR5("5 Impostazioni", "5 Settings", "5 Ajustes", "5 Reglages", "5 Einstellungen"), cc_enter()); return;
        case SC_WEB: {
            const char *ip = cc_online() ? nucleo_setup_ip() : nucleo_setup_ap_active() ? "192.168.4.1" : "--";
            snprintf(l, lc, "6 Web %.15s", ip); snprintf(r, rc, "PIN %.8s", nucleo_auth_pin()); *rcol = FG; return;
        }
        case SC_USBKBD:   SET(TR5("7 Tastiera USB", "7 USB keyboard", "7 Teclado USB", "7 Clavier USB", "7 USB-Tastatur"), cc_enter()); return;
        case SC_USBDRIVE: SET(TR5("8 SD come disco USB", "8 SD as USB drive", "8 SD como disco USB", "8 SD en disque USB",
                                  "8 SD als USB-Laufwerk"), cc_enter()); return;
        default:
            if (s_cc_arm == CC_ARM_RESTART) {
                *col = *rcol = C_RED;
                SET(TR5("Riavviare ora?", "Restart now?", "Reiniciar ahora?", "Redemarrer ?", "Jetzt neu starten?"),
                    TR5("invio si", "enter yes", "enter si", "enter oui", "enter ja"));
                return;
            }
            SET(TR5("9 Riavvia", "9 Restart", "9 Reiniciar", "9 Redemarrer", "9 Neustart"),
                TR5("invio x2", "enter x2", "enter x2", "enter x2", "enter x2"));
            return;
    }
    #undef SET
}

template <typename T> static void cc_card_caption(T *g, int line, int x, int y, bool full)
{
    char l[48], r[24]; unsigned short lc, rc;
    cc_caption_text(line, l, sizeof l, r, sizeof r, &lc, &rc);
    cc_caption(g, x, y, l, r, lc, rc, line, full);
}

template <typename T> static void cc_tile(T *g, int i, int oy, bool full)
{
    int x = CC_X0 + i * CC_TILE_P, y = oy + CC_TILE_DY;
    bool on = cc_tile_on(i), foc = (s_cc_line == CL_TILES && s_cc_tile == i);
    bool off = (i == TL_HOTSPOT && !cc_hotspot_ok());                 // unavailable this boot
    uint8_t  fk   = (uint8_t)((on ? 1 : 0) | (off ? 2 : 0));
    unsigned short fill = on ? cc_tile_col(i) : LINE;
    unsigned short ink  = nucleo_theme_ink_on(fill, on ? INK : off ? DIM : (foc ? FG : MUTED));
    unsigned short ring = cc_focus_col(foc, i == TL_HOTSPOT && s_cc_arm == CC_ARM_HOTSPOT);
    if (full || ring != s_ccs.tile_ring[i]) { cc_ring(g, x, y, CC_TILE_W, CC_TILE_H, 8, ring); s_ccs.tile_ring[i] = ring; }
    bool refill = full || fk != s_ccs.tile_fill[i];
    if (refill) cc_raised(g, x, y, CC_TILE_W, CC_TILE_H, 8, fill);
    if (refill || ink != s_ccs.tile_ink[i]) {
        ui_glyph(g, cc_tile_glyph(i), x + CC_TILE_W / 2, y + CC_TILE_H / 2 + 1, CC_GLYPH_T, ink, fill);   // the big icon IS the label
        char k[2] = { (char)('1' + i), 0 };                           // quick-key badge
        g->setFont(&fonts::Font0); g->setTextSize(1);
        g->setTextColor(nucleo_theme_ink_on(fill, on ? INK : DIM), fill); g->setCursor(x + 5, y + 3); g->print(k);
    }
    s_ccs.tile_fill[i] = fk; s_ccs.tile_ink[i] = ink;
}

template <typename T> static void cc_slider(T *g, int k, int oy, bool full)
{
    int  line = k == 2 ? CL_XBRIGHT : k ? CL_VOLUME : CL_BRIGHT, y = oy, cy = y + CC_TRK_DY + CC_TRK_H / 2;
    bool foc = (s_cc_line == line), muted = k == 1 && nucleo_audio_is_muted();
    int  val = k == 2 ? nucleo_extdisp_brightness() : k ? nucleo_audio_volume() : nucleo_app_brightness();
    unsigned short sem  = k == 1 ? (muted ? DIM : C_GREEN) : C_YELLOW;
    unsigned short ink  = foc ? FG : MUTED;
    unsigned short ring = cc_focus_col(foc, false);
    uint8_t glyph = (uint8_t)(k == 2 ? UG_MONITOR : k ? (muted ? UG_MUTE : UG_SPEAKER) : UG_SUN);
    if (full || ring != s_ccs.sl_ring[k]) { cc_ring(g, CC_X0, y, CC_XW, cc_card_h(line), 7, ring); s_ccs.sl_ring[k] = ring; }
    if ((full || glyph != s_ccs.sl_glyph[k] || ink != s_ccs.sl_ink[k]) && cc_band_visible(y + 2, CC_CAP_H)) {
        if (!full && glyph != s_ccs.sl_glyph[k]) g->fillRect(CC_X0 + 2, y + 2, 17, CC_CAP_H, BG);   // a different icon SHAPE
        ui_glyph(g, glyph, CC_X0 + 10, y + 2 + CC_CAP_H / 2, 6, ink, BG);
    }
    cc_card_caption(g, line, CC_X0 + 22, y + 2, full);
    if (k == 2 && lr_large() && cc_cal_ok() && full && cc_band_visible(cy - 11, 22)) {   // [Calibra]: the panel's touch
        const int px = CC_X0 + CC_XW - 6 - CC_CAL_W;
        g->fillRoundRect(px, cy - 13, CC_CAL_W, 21, 10, LINE);
        g->setFont(&fonts::Font2); g->setTextSize(1); g->setTextColor(FG, LINE);
        const char *cl = TR5("Calibra", "Calibrate", "Calibrar", "Calibrer", "Kalibr.");
        g->setCursor(px + (CC_CAL_W - (int)g->textWidth(cl)) / 2, cy - 11); g->print(cl);
        g->setFont(&fonts::Font0);
    }
    if (k == 2 && !cc_xbri_ok()) { s_ccs.sl_glyph[k] = glyph; s_ccs.sl_ink[k] = ink; return; }   // LED on 5 V: no slider
    const int TW = cc_trk_w(line);
    int fw = val * TW / 100; if (fw < 0) fw = 0; if (fw > TW) fw = TW;
    int kr = lr_large() ? (foc ? 9 : 7) : (foc ? 6 : 4);             // the panel: a knob the finger finds, always
    int kx = CC_TRK_X + fw; if (kx < CC_TRK_X + 6) kx = CC_TRK_X + 6; if (kx > CC_TRK_X + TW - 6) kx = CC_TRK_X + TW - 6;
    if (full || val != s_ccs.sl_val[k] || kr != s_ccs.sl_kr[k] || kx != s_ccs.sl_kx[k] || sem != s_ccs.sl_col[k] || ink != s_ccs.sl_ink[k]) {
        if (!full) g->fillCircle(s_ccs.sl_kx[k], cy + (lr_large() ? 1 : 0), s_ccs.sl_kr[k] + (lr_large() ? 1 : 0), BG);   // lift the old knob (and its shadow)
        int ty = cy - CC_TRK_H / 2;
        if (fw < TW) { cc_clip_piece(g, CC_TRK_X + fw, ty, TW - fw, CC_TRK_H); g->fillRoundRect(CC_TRK_X, ty, TW, CC_TRK_H, 3, LINE); }
        if (fw > 0)  { cc_clip_piece(g, CC_TRK_X, ty, fw, CC_TRK_H);           g->fillRoundRect(CC_TRK_X, ty, TW, CC_TRK_H, 3, sem); }
        cc_clip_body(g);
        if (lr_large()) {                                                // raised: a shadow, a light face, a colour core
            g->fillCircle(kx, cy + 2, kr, mix565(BG, 0x0000, 0.6f));
            g->fillCircle(kx, cy, kr, foc ? FG : mix565(FG, BG, 0.15f));
            g->fillCircle(kx, cy, kr - 3, sem);
        } else g->fillCircle(kx, cy, kr, foc ? FG : sem);
    }
    s_ccs.sl_glyph[k] = glyph; s_ccs.sl_ink[k] = ink; s_ccs.sl_val[k] = (int16_t)val;
    s_ccs.sl_kx[k] = (int16_t)kx; s_ccs.sl_kr[k] = (uint8_t)kr; s_ccs.sl_col[k] = sem;
}

template <typename T> static void cc_short(T *g, int i, int oy, bool full)
{
    int x = CC_X0 + i * CC_SHORT_P, y = oy + CC_SHORT_DY;
    bool foc = (s_cc_line == CL_SHORT && s_cc_short == i);
    unsigned short ring = cc_focus_col(foc, i == SC_RESTART && s_cc_arm == CC_ARM_RESTART);
    unsigned short ink  = (i == SC_RESTART) ? C_RED : nucleo_theme_ink_on(LINE, foc ? FG : MUTED);   // red = semantic, kept
    if (full) cc_raised(g, x, y, CC_SHORT_W, CC_SHORT_H, 7, LINE);
    if (full || ring != s_ccs.sh_ring[i]) { cc_ring(g, x, y, CC_SHORT_W, CC_SHORT_H, 7, ring); s_ccs.sh_ring[i] = ring; }
    if (full || ink != s_ccs.sh_ink[i]) {
        ui_glyph(g, cc_short_glyph(i), x + CC_SHORT_W / 2 + 3, y + CC_SHORT_H / 2, CC_GLYPH_S, ink, LINE);
        char k[2] = { (char)('1' + CC_NTILES + i), 0 };               // quick-key badge 5-9
        g->setFont(&fonts::Font0); g->setTextSize(1);
        g->setTextColor(nucleo_theme_ink_on(LINE, DIM), LINE); g->setCursor(x + 4, y + 3); g->print(k);
        s_ccs.sh_ink[i] = ink;
    }
}

// Scroll knob on the right edge: where the viewport sits in the column (smartwatch position indicator).
template <typename T> static void cc_knob(T *g)
{
    int vph = s_cc_vp1 - s_cc_vp0;
    if (CC_COL_H <= vph) return;
    int kh = vph * vph / CC_COL_H, ky = s_cc_vp0 + s_cc_scroll * (vph - kh) / (CC_COL_H - vph);
    g->fillRect(CC_KNOB_X, s_cc_vp0, 2, vph, LINE);
    g->fillRect(CC_KNOB_X, ky, 2, kh, MUTED);
}

// Where the column must scroll: the focused card CENTRED in the viewport (a watch list keeps the selection
// in the middle), clamped to the column ends so the first and last cards still sit at the edges.
static int cc_scroll_target(int vph)
{
    if (!cc_line_on(s_cc_line)) s_cc_line = CL_BRIGHT;         // the panel's card went away (no module this boot)
    int t = cc_card_y(s_cc_line) + cc_card_h(s_cc_line) / 2 - vph / 2;
    int mx = CC_COL_H - vph; if (mx < 0) mx = 0;
    if (t > mx) t = mx;
    if (t < 0) t = 0;
    return t;
}

template <typename T> static void cc_paint(T *g, bool full, int vh)
{
    if (full) g->fillRect(0, 0, W, vh, BG);
    cc_strip(g, full);
    s_cc_vp0 = CC_BODY_Y; s_cc_vp1 = vh;
    cc_clip_body(g);
    for (int line = 0; line < CC_NLINES; line++) {
        if (!cc_line_on(line)) continue;
        int oy = CC_BODY_Y + cc_card_y(line) - s_cc_scroll;
        if (oy + cc_card_h(line) <= s_cc_vp0 || oy >= s_cc_vp1) continue;   // fully out of view
        switch (line) {
            case CL_TILES:
                for (int i = 0; i < CC_NTILES; i++) cc_tile(g, i, oy, full);
                cc_card_caption(g, CL_TILES, CC_X0 + 2, oy + CC_TILE_CAP, full);
                break;
            case CL_BRIGHT: cc_slider(g, 0, oy, full); break;
            case CL_VOLUME: cc_slider(g, 1, oy, full); break;
            case CL_XBRIGHT: cc_slider(g, 2, oy, full); break;
            default:
                for (int i = 0; i < CC_NSHORT; i++) cc_short(g, i, oy, full);
                cc_card_caption(g, CL_SHORT, CC_X0 + 2, oy + CC_SHORT_CAP, full);
                break;
        }
    }
    if (full) cc_knob(g);
    g->clearClipRect();
    if (full && lr_large()) {                                 // touch: a tile or a shortcut is its quick key 1..9
        nucleo_touch_clear();
        nucleo_touch_add(0, 0, W, CC_BODY_Y + 3, NK_BACK, 0);              // the strip (and its X): close
        int oy = CC_BODY_Y + cc_card_y(CL_TILES) - s_cc_scroll;
        for (int i = 0; i < CC_NTILES; i++)
            nucleo_touch_add(CC_X0 + i * CC_TILE_P, oy, CC_TILE_P, cc_card_h(CL_TILES), NK_CHAR, (char)('1' + i));
        if (cc_line_on(CL_XBRIGHT) && cc_cal_ok())                             // [Calibra] (before the card's field)
            nucleo_touch_add(CC_X0 + CC_XW - 10 - CC_CAL_W, CC_BODY_Y + cc_card_y(CL_XBRIGHT) - s_cc_scroll, CC_CAL_W + 10,
                             cc_card_h(CL_XBRIGHT), NK_CHAR, '0');
        for (int line = CL_BRIGHT; line <= CL_VOLUME; line++)                  // sliders: tap where the value goes
            if (cc_line_on(line)) nucleo_touch_add(CC_X0, CC_BODY_Y + cc_card_y(line) - s_cc_scroll, CC_XW, cc_card_h(line), NK_CHAR, NUCLEO_TOUCH_FIELD);
        oy = CC_BODY_Y + cc_card_y(CL_SHORT) - s_cc_scroll;
        for (int i = 0; i < CC_NSHORT; i++)
            nucleo_touch_add(CC_X0 + i * CC_SHORT_P, oy, CC_SHORT_P, cc_card_h(CL_SHORT), NK_CHAR, (char)('1' + CC_NTILES + i));
        nucleo_touch_commit();
    }
}

void launcher_render_control_center(void)
{
    s_cc_drawn_sig = cc_sig();
    M5Canvas *c = lr_large() ? nullptr : nucleo_screen();    // large: direct, repainting only what changed
    int vh = c ? c->height() : H; if (vh > H) vh = H;        // the rows THIS frame can draw
    int tgt = cc_scroll_target(vh - CC_BODY_Y);
    if (c) {                                                  // back-buffer: compose all, ONE blit
        int dl = tgt - s_cc_scroll;                           // ease toward the focused card, a step per frame
        s_cc_scroll = s_cc_snap ? tgt : s_cc_scroll + ((dl >= -2 && dl <= 2) ? dl : dl / 2);
        s_cc_anim = (s_cc_scroll != tgt);
        cc_paint(c, true, vh);
        c->pushSprite(0, 0);
        // A canvas fitted to a short heap block (240x130 on the ADV) cannot cover the panel's last rows: clear
        // them here, or they keep the previous frame (BG over BG on later frames — nothing visibly changes).
        if (vh < H) { d.startWrite(); d.fillRect(0, vh, W, H - vh, BG); d.endWrite(); }
    } else {                                                  // direct: only what changed
        // An animated scroll would repaint the whole body every frame straight on the panel (flicker): jump.
        bool full = !s_ccs.valid || tgt != s_cc_scroll || s_ccs.vh != vh;
        s_cc_scroll = tgt; s_cc_anim = false;
        d.startWrite(); cc_paint(&d, full, vh); d.endWrite();
    }
    s_cc_snap = false;
    s_ccs.vh = (int16_t)vh;
    s_ccs.valid = true;                                       // the screen now matches s_ccs
}

// ---- companion: the Cardputer's own panel while the launcher home lives on the external one ------------------
// Only what is worth a glance: the time, the date, the battery, the network (name + address to reach the web
// OS) and what is playing. Drawn straight to the panel, no buffer; launcher_render_companion_changed() keeps a
// signature so the run loop repaints it only when one of those values moved (about once a minute).
static uint32_t companion_sig(void)
{
    time_t now = time(NULL); struct tm *tm = localtime(&now);
    uint32_t h = 2166136261u;
    auto mix = [&](const char *p) { while (p && *p) h = (h ^ (uint8_t)*p++) * 16777619u; h = (h ^ 0xFF) * 16777619u; };
    char b[32];
    snprintf(b, sizeof b, "%d:%d:%d", tm ? tm->tm_hour : -1, tm ? tm->tm_min : -1, tm ? tm->tm_yday : -1); mix(b);
    snprintf(b, sizeof b, "%d", nucleo_power_battery_pct()); mix(b);
    mix(nucleo_setup_mode()); mix(nucleo_setup_ssid()); mix(nucleo_setup_ip());
    bool play = nucleo_audio_is_playing() && !nucleo_audio_is_paused();
    mix(play ? nucleo_audio_path() : "-");
    mix(nucleo_i18n_lang());
    return h;
}

static uint32_t s_comp_sig = 0;
bool launcher_render_companion_changed(void)
{
    uint32_t h = companion_sig();
    if (h == s_comp_sig) return false;
    s_comp_sig = h;
    return true;
}
void launcher_render_companion_invalidate(void) { s_comp_sig = 0; }

void launcher_render_companion(lgfx::LovyanGFX *g)
{
    const int w = g->width(), h = g->height();
    time_t now = time(NULL); struct tm *tm = localtime(&now);
    g->startWrite();
    g->fillScreen(BG);
    g->setTextDatum(lgfx::top_center);
    // The time, as big as the panel allows (7-segment, 48 px).
    char t[8]; snprintf(t, sizeof t, "%02d:%02d", tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0);
    g->setTextColor(FG, BG);
    g->drawString(t, w / 2, 6, &fonts::Font7);
    // Date, or what is playing (the track's file name, without folders or extension).
    char line[40];
    bool play = nucleo_audio_is_playing() && !nucleo_audio_is_paused();
    if (play) {
        const char *p = nucleo_audio_path(), *slash = strrchr(p, '/');
        snprintf(line, sizeof line, "%s", slash ? slash + 1 : p);
        char *dot = strrchr(line, '.'); if (dot && dot != line) *dot = 0;
        g->setTextColor(C_GREEN, BG);
    } else {
        day_month(line, sizeof line, now, tm);
        g->setTextColor(C_YELLOW, BG);
    }
    g->setFont(&fonts::Font2);
    while (line[0] && (int)g->textWidth(line) > w - 12) line[strlen(line) - 1] = 0;
    g->drawString(line, w / 2, 60, &fonts::Font2);
    g->drawFastHLine(8, 82, w - 16, LINE);
    // Battery left, network right; the address to reach the web OS under the network name.
    g->setTextDatum(lgfx::top_left);
    int bp = nucleo_power_battery_pct();
    char bt[12];
    if (bp >= 0) snprintf(bt, sizeof bt, "%d%%", bp); else snprintf(bt, sizeof bt, "--");
    g->setTextColor(bp >= 0 && bp < 20 ? C_RED : bp >= 0 && bp < 50 ? C_YELLOW : C_GREEN, BG);
    g->drawString(bt, 10, 92, &fonts::Font4);
    bool sta = !strcmp(nucleo_setup_mode(), "sta") && nucleo_setup_ip()[0];
    g->setTextDatum(lgfx::top_right);
    char net[24];
    snprintf(net, sizeof net, "%.20s", sta ? nucleo_setup_ssid() : !strcmp(nucleo_setup_mode(), "ap") ? "Hotspot" : "offline");
    g->setTextColor(sta ? FG : C_RED, BG);
    g->drawString(net, w - 10, 90, &fonts::Font2);
    g->setTextColor(MUTED, BG);
    g->drawString(sta ? nucleo_setup_ip() : !strcmp(nucleo_setup_mode(), "ap") ? "192.168.4.1" : "", w - 10, 110, &fonts::Font2);
    g->setTextDatum(lgfx::top_left);
    g->setFont(&fonts::Font0);
    g->endWrite();
    (void)h;
}
