// Launcher rendering. See launcher_render.h.
#include "launcher_render.h"
#include "launcher_menu.h"
#include "nucleo_kbd.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include "esp_system.h"      // esp_restart() for the Control Center's Restart shortcut
#include "esp_timer.h"       // esp_timer_get_time() for the frame-rate-independent carousel ease
#include "nucleo_i18n.h"     // system language (TR/it-en) for the Control Center labels

// The real display (defined in nucleo_ui.cpp). The launcher always draws to it directly;
// only the animated list band is composited off-screen first. We do NOT include app_gfx.h
// here on purpose — that header redefines `d` as the movable app-draw target, but the
// launcher chrome must always hit the physical screen.
extern M5GFX d;

// Shared off-screen back-buffer (nucleo_ui.cpp). The launcher list band composites into it and
// blits the band region with a clipped push; apps reuse the same canvas. See app_gfx.h.
M5Canvas *nucleo_screen(void);
void      nucleo_screen_release(void);

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
    bool app = cur && cur->kind == N_APP && strcmp(cur->id, "anima");
    const char *pinw = app ? (launcher_is_pinned(cur->id) ? TR("* togli", "* unpin") : TR("* fissa", "* pin")) : "";
    if (launcher_filter()[0]) {
        snprintf(s_hint, sizeof(s_hint), TR("cerca \"%.12s\"   esc azzera", "find \"%.12s\"   esc clear"), launcher_filter());
    } else if (launcher_depth() > 0) {
        if (app) snprintf(s_hint, sizeof(s_hint), TR("invio apri   %s   esc indietro", "enter open   %s   esc back"), pinw);
        else     snprintf(s_hint, sizeof(s_hint), "%s", TR("invio apri   esc indietro", "enter open   esc back"));
    } else {
        if (app) snprintf(s_hint, sizeof(s_hint), TR("invio apri   %s   tab rapide", "enter open   %s   tab quick"), pinw);
        else     snprintf(s_hint, sizeof(s_hint), "%s", TR("invio apri   digita cerca   tab rapide", "enter open   type to find   tab quick"));   // teach Spotlight + the Control Center
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
template <typename T> static void icon_line(T *g, float x0, float y0, float x1, float y1, float w, uint16_t c)
{
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) { g->fillCircle((int)lroundf(x0), (int)lroundf(y0), (int)lroundf(w / 2), c); return; }
    float px = -dy / len * (w / 2), py = dx / len * (w / 2);
    g->fillTriangle((int)lroundf(x0 + px), (int)lroundf(y0 + py), (int)lroundf(x0 - px), (int)lroundf(y0 - py), (int)lroundf(x1 + px), (int)lroundf(y1 + py), c);
    g->fillTriangle((int)lroundf(x1 + px), (int)lroundf(y1 + py), (int)lroundf(x1 - px), (int)lroundf(y1 - py), (int)lroundf(x0 - px), (int)lroundf(y0 - py), c);
}

template <typename T> static void ui_icon(T *g, int cx, int cy, int r, const char *id, char letter, uint16_t col, uint16_t bg)
{
    if (!id) id = "";
    const float s = (float)r, Tk = (s * 0.22f > 2.0f ? s * 0.22f : 2.0f);
    #define RI(v)              ((int)lroundf((float)(v)))
    #define CLR(rr,w,h)        RI(fminf((float)(rr), fminf((float)(w) / 2.0f, (float)(h) / 2.0f)))
    #define IBX(x,y,w,h,c)     g->fillRect(RI(x),RI(y),RI(w),RI(h),(c))
    #define IFC(x,y,rr,c)      g->fillCircle(RI(x),RI(y),RI(rr),(c))
    #define ITR(a,b,p,q,u,v,c) g->fillTriangle(RI(a),RI(b),RI(p),RI(q),RI(u),RI(v),(c))
    #define IRR(x,y,w,h,rr,c)  g->fillRoundRect(RI(x),RI(y),RI(w),RI(h),CLR(rr,w,h),(c))
    #define IL(a,b,p,q,w,c)    icon_line(g,(float)(a),(float)(b),(float)(p),(float)(q),(float)(w),(c))
    #define IEL(x,y,rx,ry,c)   do { g->drawEllipse(RI(x),RI(y),RI(rx),RI(ry),(c)); g->drawEllipse(RI(x),RI(y),RI((float)(rx)-1.0f),RI((float)(ry)-1.0f),(c)); } while (0)

    if (!strcmp(id, "clock")) {
        IFC(cx, cy, s, col); IFC(cx, cy, s - Tk, bg);
        IBX(cx - Tk / 2, cy - s * 0.6f, Tk, s * 0.6f, col); IBX(cx, cy - Tk / 2, s * 0.5f, Tk, col); IFC(cx, cy, Tk * 0.7f, col);
    } else if (!strcmp(id, "chrono")) {                                        // stopwatch: dial ring + top plunger + single hand
        IBX(cx - Tk * 0.4f, cy - s * 0.98f, Tk * 0.8f, s * 0.34f, col);        // plunger stem
        IFC(cx, cy - s * 0.98f, Tk * 0.6f, col);                              // plunger knob
        IFC(cx, cy + s * 0.14f, s * 0.80f, col); IFC(cx, cy + s * 0.14f, s * 0.80f - Tk, bg);   // dial body -> ring
        IBX(cx - Tk * 0.35f, cy + s * 0.14f - s * 0.50f, Tk * 0.7f, s * 0.50f, col);            // hand (points up)
        IFC(cx, cy + s * 0.14f, Tk * 0.8f, col);                              // centre hub
    } else if (!strcmp(id, "pomodoro")) {                                     // tomato: round body + leafy crown + shine
        IFC(cx, cy + s * 0.16f, s * 0.86f, col);                             // body
        IFC(cx - s * 0.34f, cy - s * 0.12f, s * 0.20f, bg);                  // shine cut-out
        IBX(cx - Tk * 0.4f, cy - s * 0.95f, Tk * 0.8f, s * 0.30f, col);      // stem
        ITR(cx, cy - s * 0.95f, cx - s * 0.42f, cy - s * 0.34f, cx + s * 0.42f, cy - s * 0.34f, col);          // centre leaf
        ITR(cx - s * 0.66f, cy - s * 0.30f, cx - s * 0.06f, cy - s * 0.44f, cx - s * 0.20f, cy - s * 0.02f, col); // left leaf
        ITR(cx + s * 0.66f, cy - s * 0.30f, cx + s * 0.06f, cy - s * 0.44f, cx + s * 0.20f, cy - s * 0.02f, col); // right leaf
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
    } else if (!strcmp(id, "files")) {
        IRR(cx - s, cy - s * 0.6f, s * 0.95f, s * 0.4f, Tk * 0.6f, col);
        IRR(cx - s, cy - s * 0.3f, 2 * s, s * 1.5f, s * 0.22f, col);
        IBX(cx - s * 0.7f, cy - s * 0.1f, s * 1.4f, Tk * 0.55f, bg);
    } else if (!strcmp(id, "calendar")) {
        IRR(cx - s, cy - s * 0.78f, 2 * s, s * 1.66f, s * 0.22f, col);
        IBX(cx - s, cy - s * 0.78f, 2 * s, s * 0.5f, col);
        IRR(cx - s + Tk, cy - s * 0.28f + Tk, 2 * s - 2 * Tk, s * 1.16f - 2 * Tk, 2, bg);
        IBX(cx - s * 0.55f, cy - s, Tk, s * 0.34f, col); IBX(cx + s * 0.55f - Tk, cy - s, Tk, s * 0.34f, col);
        IFC(cx, cy + s * 0.34f, Tk * 0.7f, col);
    } else if (!strcmp(id, "notepad")) {
        IRR(cx - s * 0.8f, cy - s, s * 1.6f, 2 * s, s * 0.2f, col);
        for (int i = 0; i < 3; i++) IBX(cx - s * 0.5f, cy - s * 0.45f + i * s * 0.5f, s * (1.0f - i * 0.18f), Tk * 0.55f, bg);
    } else if (!strcmp(id, "usb")) {
        IBX(cx - Tk, cy - s, 2 * Tk, s * 0.4f, col);
        IRR(cx - s * 0.5f, cy - s * 0.62f, s, s * 1.62f, s * 0.2f, col);
        IBX(cx - s * 0.5f, cy + s * 0.12f, s, Tk * 0.55f, bg);
    } else if (!strcmp(id, "usbweb")) {                                          // globe (web) over a plug stub (cable)
        IEL(cx, cy - s * 0.28f, s * 0.62f, s * 0.62f, col);
        IEL(cx, cy - s * 0.28f, s * 0.26f, s * 0.62f, col);
        g->drawFastHLine(RI(cx - s * 0.62f), RI(cy - s * 0.28f), RI(s * 1.24f), col);
        IBX(cx - Tk * 0.5f, cy + s * 0.34f, Tk, s * 0.4f, col);                  // cable
        IRR(cx - s * 0.3f, cy + s * 0.72f, s * 0.6f, s * 0.34f, 2, col);         // plug
    } else if (!strcmp(id, "usbkbd")) {
        IRR(cx - s, cy - s * 0.6f, 2 * s, s * 1.2f, s * 0.22f, col);
        for (int ry = 0; ry < 2; ry++) for (int cc = 0; cc < 4; cc++) IBX(cx - s * 0.72f + cc * s * 0.46f, cy - s * 0.32f + ry * s * 0.42f, Tk * 0.6f, Tk * 0.6f, bg);
        IBX(cx - s * 0.4f, cy + s * 0.34f, s * 0.8f, Tk * 0.55f, bg);
    } else if (!strcmp(id, "music")) {
        IFC(cx - s * 0.45f, cy + s * 0.55f, Tk * 1.05f, col);
        IBX(cx - s * 0.45f + Tk * 0.6f, cy - s * 0.82f, Tk * 0.8f, s * 1.45f, col);
        ITR(cx - s * 0.45f + Tk * 1.4f, cy - s * 0.82f, cx - s * 0.45f + Tk * 1.4f, cy - s * 0.12f, cx + s * 0.72f, cy - s * 0.45f, col);
    } else if (!strcmp(id, "gbemu")) {                                       // Game Boy: body, screen, d-pad, two buttons
        IRR(cx - s * 0.62f, cy - s, s * 1.24f, s * 2.0f, s * 0.26f, col);      // handheld body
        IBX(cx - s * 0.40f, cy - s * 0.78f, s * 0.80f, s * 0.62f, bg);         // screen well
        IBX(cx - s * 0.34f, cy + s * 0.14f, s * 0.10f, s * 0.34f, bg);         // d-pad vertical
        IBX(cx - s * 0.46f, cy + s * 0.26f, s * 0.34f, s * 0.10f, bg);         // d-pad horizontal
        IFC(cx + s * 0.30f, cy + s * 0.22f, s * 0.13f, bg);                    // B
        IFC(cx + s * 0.02f, cy + s * 0.42f, s * 0.13f, bg);                    // A
    } else if (!strcmp(id, "ggemu")) {                                       // Game Gear: landscape body, screen, d-pad, two buttons
        IRR(cx - s, cy - s * 0.62f, s * 2.0f, s * 1.24f, s * 0.40f, col);      // handheld body
        IBX(cx - s * 0.36f, cy - s * 0.42f, s * 0.72f, s * 0.84f, bg);         // screen well
        IBX(cx - s * 0.80f, cy - s * 0.06f, s * 0.30f, s * 0.12f, bg);         // d-pad horizontal
        IBX(cx - s * 0.71f, cy - s * 0.21f, s * 0.12f, s * 0.42f, bg);         // d-pad vertical
        IFC(cx + s * 0.56f, cy - s * 0.06f, s * 0.11f, bg);                    // 2
        IFC(cx + s * 0.80f, cy + s * 0.14f, s * 0.11f, bg);                    // 1
    } else if (!strcmp(id, "video")) {
        IRR(cx - s, cy - s * 0.75f, 2 * s, s * 1.5f, s * 0.22f, col);
        ITR(cx - s * 0.3f, cy - s * 0.42f, cx - s * 0.3f, cy + s * 0.42f, cx + s * 0.5f, cy, bg);
    } else if (!strcmp(id, "Web OS")) {
        // category: a globe (browser web OS) — outline circle + one meridian + one parallel.
        IEL(cx, cy, s * 0.92f, s * 0.92f, col);
        IEL(cx, cy, s * 0.4f, s * 0.92f, col);       // meridian
        g->drawFastHLine(RI(cx - s * 0.92f), RI(cy), RI(s * 1.84f), col);  // equator
    } else if (!strcmp(id, "Media")) {
        ITR(cx - s * 0.7f, cy - s, cx - s * 0.7f, cy + s, cx + s, cy, col);
    } else if (!strcmp(id, "photos")) {
        IRR(cx - s, cy - s, 2 * s, 2 * s, s * 0.22f, col);
        IRR(cx - s + Tk, cy - s + Tk, 2 * s - 2 * Tk, 2 * s - 2 * Tk, 2, bg);
        IFC(cx - s * 0.4f, cy - s * 0.4f, Tk * 0.8f, col);
        ITR(cx - s * 0.85f, cy + s * 0.62f, cx - s * 0.1f, cy - s * 0.15f, cx + s * 0.25f, cy + s * 0.62f, col);
        ITR(cx - s * 0.05f, cy + s * 0.62f, cx + s * 0.45f, cy + s * 0.05f, cx + s * 0.85f, cy + s * 0.62f, col);
    } else if (!strcmp(id, "recorder") || !strcmp(id, "voice")) {
        IRR(cx - Tk * 1.15f, cy - s, Tk * 2.3f, s * 1.25f, Tk * 1.15f, col);
        IL(cx - s * 0.58f, cy, cx - s * 0.58f, cy + s * 0.22f, Tk * 0.6f, col); IL(cx + s * 0.58f, cy, cx + s * 0.58f, cy + s * 0.22f, Tk * 0.6f, col);
        IL(cx - s * 0.58f, cy + s * 0.2f, cx, cy + s * 0.5f, Tk * 0.6f, col); IL(cx + s * 0.58f, cy + s * 0.2f, cx, cy + s * 0.5f, Tk * 0.6f, col);
        IBX(cx - Tk / 2, cy + s * 0.45f, Tk, s * 0.35f, col); IBX(cx - s * 0.45f, cy + s * 0.78f, s * 0.9f, Tk * 0.6f, col);
    } else if (!strcmp(id, "micspec")) {
        static const float mh[5] = { 0.55f, 1.0f, 0.4f, 0.85f, 0.6f };
        for (int i = 0; i < 5; i++) IBX(cx - s + i * (2 * s / 5.0f) + Tk * 0.2f, cy + s - 2 * s * mh[i], Tk, 2 * s * mh[i], col);
    } else if (!strcmp(id, "voicelab")) {
        IRR(cx - s, cy - s * 0.8f, 2 * s, s * 1.25f, s * 0.3f, col);
        ITR(cx - s * 0.5f, cy + s * 0.35f, cx - s * 0.5f, cy + s, cx, cy + s * 0.4f, col);
        for (int i = -1; i <= 1; i++) IFC(cx + i * s * 0.5f, cy - s * 0.18f, Tk * 0.5f, bg);
    } else if (!strcmp(id, "info") || !strcmp(id, "Connect")) {
        IFC(cx, cy + s * 0.7f, Tk * 0.85f, col);
        ITR(cx - s * 0.55f, cy + s * 0.15f, cx + s * 0.55f, cy + s * 0.15f, cx, cy + s * 0.55f, col);
        ITR(cx - s * 0.28f, cy + s * 0.33f, cx + s * 0.28f, cy + s * 0.33f, cx, cy + s * 0.55f, bg);
        ITR(cx - s, cy - s * 0.42f, cx + s, cy - s * 0.42f, cx, cy + s * 0.1f, col);
        ITR(cx - s * 0.62f, cy - s * 0.18f, cx + s * 0.62f, cy - s * 0.18f, cx, cy + s * 0.1f, bg);
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
    } else if (!strcmp(id, "radio")) {
        IRR(cx - s, cy - s * 0.3f, 2 * s, s * 1.25f, s * 0.18f, col);
        IL(cx + s * 0.4f, cy - s * 0.3f, cx + s * 0.85f, cy - s, Tk * 0.6f, col); IFC(cx + s * 0.85f, cy - s, Tk * 0.6f, col);
        IFC(cx - s * 0.45f, cy + s * 0.32f, s * 0.38f, bg); IFC(cx + s * 0.5f, cy + s * 0.32f, Tk * 0.8f, bg);
    } else if (!strcmp(id, "remote")) {
        IRR(cx - s, cy - s * 0.8f, 2 * s, s * 1.3f, s * 0.18f, col);
        IRR(cx - s + Tk, cy - s * 0.8f + Tk, 2 * s - 2 * Tk, s * 1.3f - 2 * Tk, 2, bg);
        IFC(cx - s * 0.55f, cy + s * 0.82f, Tk * 0.7f, col); ITR(cx - s * 0.78f, cy + s * 0.55f, cx - s * 0.32f, cy + s * 0.55f, cx - s * 0.55f, cy + s * 0.85f, col);
    } else if (!strcmp(id, "ir")) {
        IRR(cx - s * 0.5f, cy - s, s, 2 * s, s * 0.3f, col);
        IFC(cx - s * 0.02f, cy - s * 0.62f, Tk * 0.45f, bg); IBX(cx - s * 0.22f, cy - s * 0.12f, s * 0.4f, Tk * 0.45f, bg); IBX(cx - s * 0.22f, cy + s * 0.28f, s * 0.4f, Tk * 0.45f, bg);
        IL(cx + s * 0.6f, cy - s * 0.7f, cx + s, cy - s, Tk * 0.6f, col); IL(cx + s * 0.6f, cy - s * 0.35f, cx + s, cy - s * 0.55f, Tk * 0.6f, col);
    } else if (!strcmp(id, "qr")) {
        const float e = s * 0.6f; static const int o[3][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 } };
        for (int k = 0; k < 3; k++) { int ox = o[k][0], oy = o[k][1];
            IBX(cx + ox * e - e * 0.5f, cy + oy * e - e * 0.5f, e, e, col); IBX(cx + ox * e - e * 0.26f, cy + oy * e - e * 0.26f, e * 0.52f, e * 0.52f, bg); IFC(cx + ox * e, cy + oy * e, e * 0.16f, col); }
        static const float d[4][2] = { { 0.32f, 0.32f }, { 0.72f, 0.42f }, { 0.42f, 0.72f }, { 0.74f, 0.74f } };
        for (int k = 0; k < 4; k++) IBX(cx + d[k][0] * s, cy + d[k][1] * s, Tk * 0.55f, Tk * 0.55f, col);
    } else if (!strcmp(id, "notify")) {
        IRR(cx - s * 0.72f, cy - s * 0.7f, s * 1.44f, s * 1.2f, s * 0.7f, col);
        IBX(cx - s * 0.82f, cy + s * 0.34f, s * 1.64f, Tk * 0.65f, col);
        IBX(cx - Tk / 2, cy - s, Tk, Tk * 0.8f, col); IFC(cx, cy + s * 0.74f, Tk * 0.6f, col);
    } else if (!strcmp(id, "torch")) {
        IRR(cx - s, cy - s * 0.45f, s * 0.9f, s * 0.9f, s * 0.18f, col);
        ITR(cx - s * 0.12f, cy - s * 0.62f, cx + s * 0.38f, cy - s, cx + s * 0.38f, cy + s, col); ITR(cx - s * 0.12f, cy + s * 0.62f, cx + s * 0.38f, cy + s, cx + s * 0.38f, cy - s, col);
        IBX(cx + s * 0.34f, cy - s * 0.55f, Tk * 0.8f, s * 1.1f, col);
        IL(cx + s * 0.6f, cy - s * 0.5f, cx + s, cy - s * 0.8f, Tk * 0.55f, col); IL(cx + s * 0.6f, cy, cx + s, cy, Tk * 0.55f, col); IL(cx + s * 0.6f, cy + s * 0.5f, cx + s, cy + s * 0.8f, Tk * 0.55f, col);
    } else if (!strcmp(id, "theme")) {
        IFC(cx, cy, s, col); IBX(cx, cy - s, s + 1, 2 * s, bg); IEL(cx, cy, s, s, col);
    } else if (!strcmp(id, "wifi")) {
        for (int i = 0; i < 3; i++) { float yy = cy - s * 0.55f + i * s * 0.55f; IBX(cx - s, yy - Tk * 0.28f, 2 * s, Tk * 0.55f, col); IFC(cx - s * 0.5f + i * s * 0.5f, yy, Tk * 0.85f, col); }
    } else if (!strcmp(id, "mail")) {
        // envelope: filled body + V flap drawn in the background colour
        IRR(cx - s, cy - s * 0.66f, 2 * s, s * 1.32f, s * 0.18f, col);
        IL(cx - s * 0.86f, cy - s * 0.46f, cx, cy + s * 0.16f, Tk * 0.6f, bg);
        IL(cx + s * 0.86f, cy - s * 0.46f, cx, cy + s * 0.16f, Tk * 0.6f, bg);
    } else if (!strcmp(id, "Messaging")) {
        // category: speech bubble with a tail + three dots
        IRR(cx - s, cy - s * 0.8f, 2 * s, s * 1.28f, s * 0.34f, col);
        ITR(cx - s * 0.5f, cy + s * 0.3f, cx - s * 0.08f, cy + s * 0.3f, cx - s * 0.62f, cy + s, col);
        for (int i = -1; i <= 1; i++) IFC(cx + i * s * 0.42f, cy - s * 0.14f, Tk * 0.5f, bg);
    } else if (!strcmp(id, "link")) {
        float a = s * 0.78f;
        IL(cx - a, cy, cx + a, cy - a, Tk * 0.6f, col); IL(cx - a, cy, cx + a, cy + a, Tk * 0.6f, col);
        IFC(cx - a, cy, Tk * 1.05f, col); IFC(cx + a, cy - a, Tk * 1.05f, col); IFC(cx + a, cy + a, Tk * 1.05f, col);
    } else if (!strcmp(id, "swarm")) {
        // a hub with peers all around it — a mesh/swarm of devices (distinct from link's 3 nodes)
        const float R = s * 0.84f;
        static const float o[6][2] = { {1.0f,0.0f},{0.5f,0.87f},{-0.5f,0.87f},{-1.0f,0.0f},{-0.5f,-0.87f},{0.5f,-0.87f} };
        for (int k = 0; k < 6; k++) { float px = cx + R * o[k][0], py = cy + R * o[k][1];
            IL(cx, cy, px, py, Tk * 0.5f, col); IFC(px, py, Tk * 0.66f, col); }
        IFC(cx, cy, Tk * 1.05f, col);
    } else if (!strcmp(id, "ssh")) {
        IRR(cx - s, cy - s * 0.8f, 2 * s, s * 1.6f, s * 0.18f, col);
        IBX(cx - s, cy - s * 0.8f, 2 * s, s * 0.42f, col); IRR(cx - s + Tk * 0.6f, cy - s * 0.28f, 2 * s - Tk * 1.2f, s - Tk * 0.6f, 2, bg);
        ITR(cx - s * 0.5f, cy - s * 0.05f, cx - s * 0.5f, cy + s * 0.32f, cx - s * 0.08f, cy + s * 0.14f, col);
        IBX(cx, cy + s * 0.24f, s * 0.45f, Tk * 0.55f, col);
    } else if (!strcmp(id, "ethernet")) {
        IRR(cx - s * 0.72f, cy - s * 0.8f, s * 1.44f, s * 1.3f, s * 0.16f, col);
        for (int i = 0; i < 4; i++) IBX(cx - s * 0.5f + i * s * 0.32f, cy - s * 0.8f, Tk * 0.45f, s * 0.4f, bg);
        IBX(cx - Tk * 0.7f, cy + s * 0.5f, Tk * 1.4f, s * 0.5f, col);
    } else if (!strcmp(id, "beacon")) {
        ITR(cx - s * 0.8f, cy + s, cx + s * 0.8f, cy + s, cx, cy - s * 0.15f, col);
        IRR(cx - s * 0.5f, cy + s * 0.82f, s, Tk * 0.7f, 2, bg);
        IFC(cx, cy - s * 0.28f, Tk * 0.8f, col);
        IL(cx + s * 0.34f, cy - s * 0.55f, cx + s * 0.62f, cy - s * 0.78f, Tk * 0.5f, col); IL(cx + s * 0.34f, cy - s * 0.18f, cx + s * 0.66f, cy - s * 0.32f, Tk * 0.5f, col);
        IL(cx - s * 0.34f, cy - s * 0.55f, cx - s * 0.62f, cy - s * 0.78f, Tk * 0.5f, col); IL(cx - s * 0.34f, cy - s * 0.18f, cx - s * 0.66f, cy - s * 0.32f, Tk * 0.5f, col);
    } else if (!strcmp(id, "wifiatk")) {
        IFC(cx - s * 0.3f, cy + s * 0.58f, Tk * 0.75f, col);
        ITR(cx - s * 0.8f, cy + s * 0.05f, cx + s * 0.2f, cy + s * 0.05f, cx - s * 0.3f, cy + s * 0.5f, col); ITR(cx - s * 0.55f, cy + s * 0.22f, cx - s * 0.05f, cy + s * 0.22f, cx - s * 0.3f, cy + s * 0.5f, bg);
        IL(cx + s * 0.1f, cy - s, cx + s, cy - s * 0.2f, Tk * 0.85f, col); IL(cx + s, cy - s, cx + s * 0.1f, cy - s * 0.2f, Tk * 0.85f, col);
    } else if (!strcmp(id, "evilportal")) {
        IRR(cx - s, cy - s * 0.85f, 2 * s, s * 1.7f, s * 0.16f, col);
        IBX(cx - s, cy - s * 0.85f, 2 * s, s * 0.4f, col); IRR(cx - s + Tk * 0.6f, cy - s * 0.4f + Tk * 0.6f, 2 * s - Tk * 1.2f, s * 1.18f, 2, bg);
        IRR(cx - s * 0.4f, cy + s * 0.02f, s * 0.8f, s * 0.56f, 2, col); IEL(cx, cy - s * 0.05f, s * 0.26f, s * 0.26f, col);
    } else if (!strcmp(id, "sniffer")) {
        // radar: a source dot with concentric signal rings + a sweep line (listening to the air)
        IEL(cx, cy, s * 0.95f, s * 0.95f, col);
        IEL(cx, cy, s * 0.60f, s * 0.60f, col);
        IFC(cx, cy, Tk * 0.9f, col);
        IL(cx, cy, cx + s * 0.78f, cy - s * 0.55f, Tk * 0.55f, col);
    } else if (!strcmp(id, "weather")) {
        // a sun behind a cloud
        IFC(cx - s * 0.30f, cy - s * 0.38f, s * 0.34f, col);                                  // sun disc
        IL(cx - s * 0.30f, cy - s * 0.96f, cx - s * 0.30f, cy - s * 0.74f, Tk * 0.7f, col);   // ray up
        IL(cx - s * 0.84f, cy - s * 0.38f, cx - s * 0.62f, cy - s * 0.38f, Tk * 0.7f, col);   // ray left
        IL(cx - s * 0.68f, cy - s * 0.76f, cx - s * 0.50f, cy - s * 0.58f, Tk * 0.7f, col);   // ray nw
        IL(cx + s * 0.06f, cy - s * 0.76f, cx - s * 0.10f, cy - s * 0.58f, Tk * 0.7f, col);   // ray ne
        IFC(cx - s * 0.50f, cy + s * 0.30f, s * 0.34f, col);                                  // cloud puffs
        IFC(cx + s * 0.55f, cy + s * 0.30f, s * 0.40f, col);
        IFC(cx + s * 0.05f, cy + s * 0.06f, s * 0.46f, col);
        IRR(cx - s * 0.85f, cy + s * 0.26f, s * 1.7f, s * 0.55f, s * 0.27f, col);
    } else if (!strcmp(id, "ble")) {
        // Bluetooth rune: vertical stem + the two crossed right "wings"
        float a = s * 0.6f, q = s * 0.5f;
        IL(cx, cy - s, cx, cy + s, Tk * 0.7f, col);
        IL(cx, cy - s, cx + a, cy - q, Tk * 0.7f, col);
        IL(cx + a, cy - q, cx - a, cy + q, Tk * 0.7f, col);
        IL(cx - a, cy - q, cx + a, cy + q, Tk * 0.7f, col);
        IL(cx + a, cy + q, cx, cy + s, Tk * 0.7f, col);
    } else if (!strcmp(id, "payloads")) {
        // a lightning bolt — keystroke injection
        ITR(cx - s * 0.30f, cy - s, cx + s * 0.40f, cy - s * 0.2f, cx - s * 0.10f, cy - s * 0.2f, col);
        ITR(cx + s * 0.30f, cy + s, cx - s * 0.40f, cy + s * 0.2f, cx + s * 0.10f, cy + s * 0.2f, col);
    } else if (!strcmp(id, "sentinel")) {
        // a watching shield — defensive tracker detection (a shield with an eye)
        IRR(cx - s, cy - s, 2 * s, s, s * 0.32f, col);                                   // shield shoulders
        ITR(cx - s, cy - s * 0.5f, cx + s, cy - s * 0.5f, cx, cy + s, col);              // shield point
        IEL(cx, cy - s * 0.12f, s * 0.46f, s * 0.30f, bg);                              // eye outline (punched out)
        IFC(cx, cy - s * 0.12f, Tk * 0.75f, bg);                                        // pupil
    } else if (!strcmp(id, "fido")) {
        // a key — the security key / passkey
        IFC(cx - s * 0.45f, cy, s * 0.52f, col); IFC(cx - s * 0.45f, cy, s * 0.52f - Tk, bg);  // bow (ring)
        IBX(cx - s * 0.02f, cy - Tk * 0.45f, s * 0.98f, Tk * 0.9f, col);                 // shaft
        IBX(cx + s * 0.58f, cy, Tk * 0.8f, s * 0.5f, col);                              // tooth 1
        IBX(cx + s * 0.86f, cy, Tk * 0.8f, s * 0.32f, col);                             // tooth 2
    } else if (!strcmp(id, "airspace")) {
        // concentric signal rings + a centre dot — passively watching the air
        IEL(cx, cy, s * 0.95f, s * 0.95f, col);
        IEL(cx, cy, s * 0.62f, s * 0.62f, col);
        IEL(cx, cy, s * 0.30f, s * 0.30f, col);
        IFC(cx, cy, Tk * 0.9f, col);
    } else if (!strcmp(id, "Security")) {
        IRR(cx - s, cy - s, 2 * s, s, s * 0.32f, col);
        ITR(cx - s, cy - s * 0.5f, cx + s, cy - s * 0.5f, cx, cy + s, col);
    } else if (!strcmp(id, "Games")) {
        IRR(cx - s, cy - s * 0.5f, 2 * s, s * 1.05f, s * 0.5f, col);
        IBX(cx - s * 0.62f, cy - Tk * 0.28f, s * 0.5f, Tk * 0.55f, bg); IBX(cx - s * 0.42f, cy - s * 0.25f, Tk * 0.55f, s * 0.5f, bg);
        IFC(cx + s * 0.4f, cy - s * 0.12f, Tk * 0.55f, bg); IFC(cx + s * 0.66f, cy + s * 0.1f, Tk * 0.55f, bg); IFC(cx + s * 0.14f, cy + s * 0.1f, Tk * 0.55f, bg);
    } else if (!strcmp(id, "reactor")) {
        IEL(cx, cy, s, s * 0.42f, col); IEL(cx, cy, s * 0.42f, s, col);
        IFC(cx, cy, Tk * 1.05f, col); IFC(cx + s * 0.92f, cy, Tk * 0.55f, col); IFC(cx, cy - s * 0.92f, Tk * 0.55f, col);
    } else if (!strcmp(id, "pong")) {
        // two paddles + ball + a hint of the centre net
        IRR(cx - s * 0.92f, cy - s * 0.6f, s * 0.26f, s * 1.2f, 2, col);   // left paddle
        IRR(cx + s * 0.66f, cy - s * 0.15f, s * 0.26f, s * 1.2f, 2, col);  // right paddle (offset)
        IFC(cx + s * 0.06f, cy + s * 0.04f, s * 0.22f, col);              // ball
        IFC(cx, cy - s * 0.62f, Tk * 0.35f, col); IFC(cx, cy + s * 0.62f, Tk * 0.35f, col);  // net dots
    } else if (!strcmp(id, "tanks")) {
        // tank: hull + wheels + turret + raised barrel
        IRR(cx - s * 0.9f, cy + s * 0.12f, s * 1.8f, s * 0.5f, 3, col);
        IFC(cx - s * 0.55f, cy + s * 0.66f, Tk * 0.55f, col); IFC(cx, cy + s * 0.66f, Tk * 0.55f, col); IFC(cx + s * 0.55f, cy + s * 0.66f, Tk * 0.55f, col);
        IRR(cx - s * 0.34f, cy - s * 0.22f, s * 0.68f, s * 0.42f, 2, col);
        IL(cx + s * 0.2f, cy - s * 0.05f, cx + s, cy - s * 0.62f, Tk * 0.6f, col);
    } else if (!strcmp(id, "stelle")) {
        static const float p[5][2] = { { -0.78f, -0.5f }, { -0.05f, -0.15f }, { 0.7f, -0.6f }, { 0.4f, 0.6f }, { -0.55f, 0.7f } };
        static const int lk[4][2] = { { 0, 1 }, { 1, 2 }, { 1, 3 }, { 3, 4 } };
        for (int k = 0; k < 4; k++) IL(cx + p[lk[k][0]][0] * s, cy + p[lk[k][0]][1] * s, cx + p[lk[k][1]][0] * s, cy + p[lk[k][1]][1] * s, Tk * 0.35f, col);
        static const float sr[5] = { 0.55f, 0.9f, 0.5f, 0.62f, 0.45f };
        for (int i = 0; i < 5; i++) IFC(cx + p[i][0] * s, cy + p[i][1] * s, Tk * sr[i], col);
    } else if (!strcmp(id, "giardino")) {
        IRR(cx - s * 0.85f, cy + s * 0.55f, s * 1.7f, s * 0.45f, s * 0.16f, col);
        IBX(cx - Tk * 0.4f, cy - s * 0.25f, Tk * 0.8f, s * 0.85f, col);
        IFC(cx - s * 0.34f, cy - s * 0.28f, s * 0.34f, col); IFC(cx + s * 0.34f, cy - s * 0.28f, s * 0.34f, col); IFC(cx, cy - s * 0.62f, s * 0.3f, col);
    } else if (!strcmp(id, "slots")) {
        IRR(cx - s * 0.85f, cy - s * 0.7f, s * 1.7f, s * 1.5f, s * 0.16f, col);
        IRR(cx - s * 0.6f, cy - s * 0.45f, s * 1.2f, s * 0.9f, 2, bg);
        IBX(cx - s * 0.2f, cy - s * 0.45f, Tk * 0.45f, s * 0.9f, col); IBX(cx + s * 0.2f - Tk * 0.45f, cy - s * 0.45f, Tk * 0.45f, s * 0.9f, col);
        IBX(cx + s * 0.85f, cy - s * 0.6f, Tk * 0.7f, s * 0.7f, col); IFC(cx + s * 0.85f + Tk * 0.35f, cy - s * 0.6f, Tk * 0.7f, col);
    } else if (!strcmp(id, "Tools")) {
        for (int i = 0; i < 8; i++) { float a = i * 0.785398f; IBX(cx + cosf(a) * s * 0.84f - Tk * 0.45f, cy + sinf(a) * s * 0.84f - Tk * 0.45f, Tk * 0.9f, Tk * 0.9f, col); }
        IFC(cx, cy, s * 0.6f, col); IFC(cx, cy, s * 0.24f, bg);
    } else if (!strcmp(id, "Office")) {
        IRR(cx - s, cy - s * 0.4f, 2 * s, s * 1.3f, s * 0.16f, col);
        IRR(cx - s * 0.45f, cy - s * 0.8f, s * 0.9f, s * 0.5f, s * 0.16f, col); IRR(cx - s * 0.28f, cy - s * 0.62f, s * 0.56f, s * 0.4f, 2, bg);
        IBX(cx - s, cy + s * 0.06f, 2 * s, Tk * 0.65f, bg);
    } else if (!strcmp(id, "device")) {                                         // phone: body, screen, earpiece, home
        IRR(cx - s * 0.58f, cy - s, s * 1.16f, 2 * s, s * 0.22f, col);
        IRR(cx - s * 0.4f, cy - s * 0.68f, s * 0.8f, s * 1.25f, 2, bg);
        IBX(cx - s * 0.18f, cy - s * 0.85f, s * 0.36f, Tk * 0.4f, bg); IFC(cx, cy + s * 0.78f, Tk * 0.5f, bg);
    } else if (!strcmp(id, "poker")) {                                          // playing card with a heart pip
        IRR(cx - s * 0.68f, cy - s, s * 1.36f, 2 * s, s * 0.18f, col);
        IFC(cx - s * 0.2f, cy - s * 0.18f, s * 0.22f, bg); IFC(cx + s * 0.2f, cy - s * 0.18f, s * 0.22f, bg);
        ITR(cx - s * 0.41f, cy - s * 0.06f, cx + s * 0.41f, cy - s * 0.06f, cx, cy + s * 0.45f, bg);
        IFC(cx - s * 0.44f, cy - s * 0.76f, Tk * 0.42f, bg); IFC(cx + s * 0.44f, cy + s * 0.76f, Tk * 0.42f, bg);
    } else if (!strcmp(id, "pinball")) {                                        // portrait table: body, ball, two flippers
        IRR(cx - s * 0.62f, cy - s, s * 1.24f, 2 * s, s * 0.3f, col);
        IRR(cx - s * 0.42f, cy - s * 0.82f, s * 0.84f, s * 1.52f, 2, bg);
        IFC(cx + s * 0.12f, cy - s * 0.34f, Tk * 0.72f, col);                    // ball
        IL(cx - s * 0.3f, cy + s * 0.52f, cx + s * 0.02f, cy + s * 0.28f, Tk * 0.6f, col);   // left flipper
        IL(cx + s * 0.3f, cy + s * 0.52f, cx - s * 0.02f, cy + s * 0.28f, Tk * 0.6f, col);   // right flipper
    } else if (!strcmp(id, "level")) {                                          // spirit level: pill vial, bubble, two gauge ticks
        IRR(cx - s, cy - s * 0.42f, 2 * s, s * 0.84f, s * 0.42f, col);
        IRR(cx - s + Tk * 0.6f, cy - s * 0.42f + Tk * 0.6f, 2 * s - Tk * 1.2f, s * 0.84f - Tk * 1.2f, s * 0.36f, bg);
        IBX(cx - s * 0.34f, cy - s * 0.42f, Tk * 0.5f, s * 0.84f, col);
        IBX(cx + s * 0.34f - Tk * 0.5f, cy - s * 0.42f, Tk * 0.5f, s * 0.84f, col);
        IFC(cx, cy, Tk * 1.15f, col);                                           // the bubble
    } else if (!strcmp(id, "dice")) {                                           // die face: rounded square + 5 pips
        IRR(cx - s, cy - s, 2 * s, 2 * s, s * 0.28f, col);
        IFC(cx - s * 0.45f, cy - s * 0.45f, Tk * 0.6f, bg);
        IFC(cx + s * 0.45f, cy - s * 0.45f, Tk * 0.6f, bg);
        IFC(cx, cy, Tk * 0.6f, bg);
        IFC(cx - s * 0.45f, cy + s * 0.45f, Tk * 0.6f, bg);
        IFC(cx + s * 0.45f, cy + s * 0.45f, Tk * 0.6f, bg);
    } else if (!strcmp(id, "goniometer")) {                                     // protractor: top half-disc + pivot + angle arm
        IFC(cx, cy + s * 0.45f, s, col);                                        // full disc, dropped...
        IBX(cx - s - 1, cy + s * 0.45f, 2 * s + 2, s + 2, bg);                  // ...mask below centre -> half-disc (flat base down)
        IBX(cx - s * 0.5f, cy + s * 0.05f, s, s * 0.42f, bg);                   // inner protractor cut-out
        IL(cx, cy + s * 0.45f, cx + s * 0.66f, cy - s * 0.55f, Tk * 0.55f, col);// angle arm from the pivot
        IFC(cx, cy + s * 0.45f, Tk * 0.7f, col);                               // pivot dot
    } else if (!strcmp(id, "Measure")) {                                       // DIP microchip: body + side legs + pin-1 dot
        IRR(cx - s * 0.56f, cy - s * 0.72f, s * 1.12f, s * 1.44f, s * 0.12f, col);
        IRR(cx - s * 0.28f, cy - s * 0.44f, s * 0.56f, s * 0.9f, 2, bg);
        for (int i = -1; i <= 1; i++) {
            IBX(cx - s, cy + i * s * 0.42f - Tk * 0.28f, s * 0.44f, Tk * 0.56f, col);
            IBX(cx + s * 0.56f, cy + i * s * 0.42f - Tk * 0.28f, s * 0.44f, Tk * 0.56f, col);
        }
        IFC(cx - s * 0.28f, cy - s * 0.46f, Tk * 0.5f, col);                    // pin-1 notch dot
    } else if (!strcmp(id, "pedometer")) {                                      // footprint: ball + heel + a row of toes
        IFC(cx, cy - s * 0.05f, s * 0.52f, col);                                // ball of the foot
        IFC(cx + s * 0.12f, cy + s * 0.66f, s * 0.32f, col);                    // heel (smaller, offset)
        IFC(cx - s * 0.42f, cy - s * 0.55f, Tk * 0.34f, col);                   // toes...
        IFC(cx - s * 0.15f, cy - s * 0.72f, Tk * 0.38f, col);
        IFC(cx + s * 0.13f, cy - s * 0.74f, Tk * 0.36f, col);
        IFC(cx + s * 0.37f, cy - s * 0.62f, Tk * 0.32f, col);
    } else if (!strcmp(id, "alarm")) {                                          // alarm bell: dome body + base rim + clapper
        IFC(cx, cy - s * 0.6f, Tk * 0.4f, col);                                 // top knob
        ITR(cx, cy - s * 0.5f, cx - s * 0.6f, cy + s * 0.4f, cx + s * 0.6f, cy + s * 0.4f, col);  // bell body
        IFC(cx, cy - s * 0.42f, s * 0.3f, col);                                 // round the shoulders
        IRR(cx - s * 0.66f, cy + s * 0.36f, s * 1.32f, Tk * 0.5f, Tk * 0.2f, col);  // base rim
        IFC(cx, cy + s * 0.66f, Tk * 0.42f, col);                              // clapper
    } else if (!strcmp(id, "screensaver")) {                                    // crescent moon + 3 stars (sleep/idle)
        IFC(cx - s * 0.18f, cy + s * 0.08f, s * 0.76f, col);                   // moon disc
        IFC(cx + s * 0.22f, cy - s * 0.08f, s * 0.56f, bg);                    // bite out → crescent
        IFC(cx + s * 0.72f, cy - s * 0.64f, Tk * 0.65f, col);                  // star top-right
        IFC(cx + s * 0.80f, cy + s * 0.28f, Tk * 0.45f, col);                  // star mid-right
        IFC(cx - s * 0.18f, cy - s * 0.88f, Tk * 0.50f, col);                  // star top
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
    } else {
        g->setTextSize(r >= 12 ? 3 : (r >= 6 ? 2 : 1)); g->setTextColor(col);   // fallback: a bold letter
        int cw = (r >= 12 ? 18 : (r >= 6 ? 12 : 6)), ch = (r >= 12 ? 24 : (r >= 6 ? 16 : 8));
        g->setCursor(cx - cw / 2 + 1, cy - ch / 2); g->print(letter);
        g->setTextSize(1);                                                      // MUST reset: else it leaks into the title (Font4 x3 = huge)
    }
    #undef RI
    #undef CLR
    #undef IBX
    #undef IFC
    #undef ITR
    #undef IRR
    #undef IL
    #undef IEL
}

// Public wrappers so sibling launcher TUs (the Game front-end's procedural poster) can draw the real
// vector icons — to the off-screen canvas (M5Canvas) or straight to the display (M5GFX). The ui_icon
// template instantiates for each target.
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
    if (nucleo_evilportal_running())     { snprintf(buf, cap, "EVIL PORTAL  %d catt.", nucleo_evilportal_captures()); return buf; }
    if (nucleo_wifiatk_deauth_running()) { snprintf(buf, cap, "DEAUTH FLOOD  %lu fr", nucleo_wifiatk_frames());        return buf; }
    if (nucleo_wifiatk_beacon_running()) { snprintf(buf, cap, "BEACON SPAM  %d SSID", nucleo_wifiatk_beacon_count()); return buf; }
    return NULL;
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
        d.setTextSize(1); d.setTextColor(INK, C_RED); d.setCursor(18, 4); d.print(alert);
        d.drawFastHLine(0, BAR - 1, W, LINE);
        return;
    }
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
        d.fillRoundRect(2, 1, 14, 14, 3, node->color);
        ui_icon(&d, 9, 8, 6, node->id, node->icon, INK, node->color);
        d.setFont(&fonts::Font2); d.setTextColor(FG, INK); d.setCursor(20, 0);
        char b[16]; snprintf(b, sizeof(b), "%.14s", node_label(node)); d.print(b);
        d.setFont(&fonts::Font0); d.setTextSize(1);
        int rx = W - 2;
        draw_wifi(rx - 19, 4, sta, nucleo_setup_rssi());   rx -= 19 + 6;   // signal far right
        int cnt = node_child_count(node);
        if (cnt > 0) {
            char cc[12]; snprintf(cc, sizeof cc, "%d app", cnt);
            d.setTextColor(MUTED, INK); d.setCursor(rx - (int)strlen(cc) * 6, 4); d.print(cc);   // left of the gauges
        }
    } else {
        // Home (smartwatch face): a bold clock anchors the LEFT in the 16px-tall Font2 face; the
        // Wi-Fi gauge, the date (day + month) and the network name form ONE right-aligned cluster,
        // packed from the right edge inward. The date has PRIORITY over the SSID, so day/month is
        // shown whenever it fits. Drawn only when no filter chip claims the right.
        time_t now = time(NULL); struct tm *tm = localtime(&now);
        char t[8]; snprintf(t, sizeof(t), "%02d:%02d", tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0);
        d.setFont(&fonts::Font2); d.setTextColor(FG, INK); d.setCursor(6, 0); d.print(t);  // big white clock
        int clock_r = 6 + (int)d.textWidth(t);
        d.setFont(&fonts::Font0); d.setTextSize(1);

        if (!launcher_filter()[0]) {
            // Right cluster from the edge: [|||] Wi-Fi, then [day month], then [ssid] if it still
            // fits. Each piece is dropped if it would crowd the clock, so nothing overlaps the time.
            int rx = W - 2;
            draw_wifi(rx - 19, 4, sta, nucleo_setup_rssi());   rx -= 19 + 6;   // antenna gauge, far right
            int bpct = nucleo_power_battery_pct();
            if (bpct >= 0) { draw_battery_pip(rx - 14, 4, bpct); rx -= 14 + 6; }   // pip left of the gauge
            // Day + month follow the system language (Italian only for "it"; English is the floor, like TR).
            static const char *const WD_IT[7]  = { "dom","lun","mar","mer","gio","ven","sab" };
            static const char *const MO_IT[12] = { "gen","feb","mar","apr","mag","giu","lug","ago","set","ott","nov","dic" };
            static const char *const WD_EN[7]  = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
            static const char *const MO_EN[12] = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
            bool it = !strcmp(nucleo_i18n_lang(), "it");
            char dt[16] = "";
            if (tm && now > 1672531200) snprintf(dt, sizeof dt, "%s %d %s", (it ? WD_IT : WD_EN)[tm->tm_wday], tm->tm_mday, (it ? MO_IT : MO_EN)[tm->tm_mon]);
            int dw = (int)strlen(dt) * 6;
            if (dt[0] && rx - dw > clock_r + 6) {
                d.setTextColor(C_YELLOW, INK); d.setCursor(rx - dw, 4); d.print(dt);
                rx -= dw + 8;
            }
            // Connected -> the live SSID in muted grey; not connected -> a small red "offline" right
            // next to the gauge, so a missing/lost network reads instantly instead of a stale name.
            char net[12]; snprintf(net, sizeof net, "%.10s", sta ? nucleo_setup_ssid() : "offline");
            int nw = (int)strlen(net) * 6;
            if (rx - nw > clock_r + 8) {
                d.setTextColor(sta ? MUTED : C_RED, INK); d.setCursor(rx - nw, 4); d.print(net);
            }
        }
    }

    if (launcher_filter()[0]) {
        // Filter chip, right-aligned so it never collides with the title/clock.
        char fb[20]; snprintf(fb, sizeof fb, "/%.10s", launcher_filter());
        int fw = (int)strlen(fb) * 6;
        d.fillRect(W - fw - 4, 0, fw + 4, BAR - 1, 0x1926);
        d.setTextColor(C_GREEN, 0x1926); d.setCursor(W - fw - 1, 4); d.print(fb);
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
        d.setTextSize(1); d.setTextColor(INK, C_RED); d.setCursor(18, 4); d.print(alert);
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
    d.fillRect(0, 0, 56, BAR - 1, INK);                       // wipe the clock cell (Font2 is wider than the old 6x8)
    d.setFont(&fonts::Font2); d.setTextColor(FG, INK); d.setCursor(6, 0); d.print(t);
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
static const int CAR_SLOT    = 84;     // horizontal spacing between adjacent icons (left/right peek)
static const int CAR_R_C     = 32;     // centred badge radius (fills the band top-down)
static const int CAR_R_S     = 19;     // side badge radius (neighbours stay prominent)
static const int CAR_ICON_DY = 34;     // icon-row centre, pushed up to use the band top

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
    c->setTextColor(FG, INK);
    c->setCursor(cx - tw / 2, cy - 8); c->print(s);                                        // Font2 ~16px, centred
}

// Dot rail / position indicator, centred at (cx, y). Collapses to "k/n" past 13 items.
template <typename T> static void draw_dots(T *c, int n, int sel, int cx, int y, uint16_t accent)
{
    if (n <= 1) return;
    if (n > 13) {
        char b[12]; snprintf(b, sizeof b, "%d/%d", sel + 1, n);
        c->setFont(&fonts::Font0); c->setTextSize(1); c->setTextColor(MUTED, BG);
        c->setCursor(cx - (int)c->textWidth(b) / 2, y - 4); c->print(b);
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

// ---- the launcher carousel --------------------------------------------------
// Rendered into a band-local coordinate space (top = `base`): for the off-screen canvas base = 0 and
// the result is pushed at y = LIST_TOP; for the direct fallback base = LIST_TOP. The focused item is a
// big centred badge; neighbours peek smaller/dimmer; the rail slides via s_carousel (eased in
// launcher_render_step_scroll).
template <typename T> static void draw_list(T *c, int base)
{
    if (nucleo_theme_has_bg_image()) nucleo_theme_draw_bg_slice(c, 0, base, W, LIST_BAND_H);
    else                            c->fillRect(0, base, W, LIST_BAND_H, BG);

    int n = launcher_visible_count();
    if (n == 0) {
        c->setFont(&fonts::Font2); c->setTextColor(DIM, BG);
        c->setCursor(54, base + LIST_BAND_H / 2 - 8); c->print("Nessuna app");
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
        int cy = iconCY + (int)((1.0f - t) * 6.0f + 0.5f);              // neighbours drop a touch (arc/depth)
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
        ui_icon(c, x, cy, gr, it->id, it->icon, gcol, badge);
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
        c->setCursor(W / 2 - (int)c->textWidth(buf) / 2, base + 84 - fh / 2); c->print(buf);   // centre at base+84
        draw_dots(c, n, sel, W / 2, base + 102, cur->color);
    }
    c->setFont(&fonts::Font0); c->setTextSize(1);                       // leave the font at the framework default
}

// The list band composites into the shared back-buffer (see nucleo_ui.cpp). We draw the band
// into the top of that canvas and blit only the band region with a destination-clipped push,
// so the chrome below it is untouched. Decoder apps call nucleo_screen_release() directly to
// hand the canvas RAM to the codec; it re-acquires lazily here.
void launcher_render_list(void)
{
    M5Canvas *c = nucleo_screen();
    s_band_buffered = (c != nullptr);                          // tells step_scroll to snap (not animate) on the direct path
    if (c) {
        draw_list(c, 0);                                        // band-local: rows 0..LIST_BAND_H of the canvas
        d.setClipRect(0, LIST_TOP, W, LIST_BAND_H);            // land it in the band region only...
        c->pushSprite(0, LIST_TOP);                            // ...one blit -> no flicker (rows below are clipped)
        d.clearClipRect();
    } else {
        // No canvas (heap fragmented after a radio session): draw direct, batched into ONE SPI
        // transaction so the clear→rows repaint is as quick as possible. Paired with the snap-scroll
        // above (no per-frame animation) this keeps the menu clean instead of a flickery mess.
        d.startWrite();
        draw_list(&d, LIST_TOP);
        d.endWrite();
    }
}

// ---- Control Center overlay (one-screen quick panel) -------------------------
// Raised with TAB from anywhere. ONE screen, no tabs and no hidden pages: everything a quick panel is
// for is visible at once, top to bottom —
//
//   14:05  CasaNet                 |||  [=] 85%    status strip: clock · network · signal · battery
//   [1 Muto] [2 Torcia] [3 Spegni] [4 Hotspot]     toggle tiles (keys 1-4 fire them directly)
//   (sun)  =================o---------   70%       brightness  (LEFT/RIGHT adjust in place)
//   (spk)  ========o------------------   40%       volume      (ENTER toggles mute)
//   (gear) (web) (kbd) (usb) (power)               shortcuts: Settings · Web client · USB keyboard ·
//                                                  USB drive · Restart
//   Luminosita 70%   </> regola                    context line: what the focus does + its live value
//                                                  (on the Web shortcut: the IP and pairing PIN)
//
// Keys: UP/DOWN move between lines (wrap); LEFT/RIGHT move inside a line or adjust a slider; ENTER acts;
// Esc (or TAB) closes. The focus is REMEMBERED across opens (resume where you left off). Disruptive
// actions (Hotspot, which drops the Wi-Fi client link, and Restart) arm on the first ENTER and fire on
// the second; any other key disarms — the focus turns red and the context line says so.
//
// Colours are theme roles (BG/FG/MUTED/DIM/LINE/INK/THEME_ACC) plus the named semantic C_* accents only:
// the old sheet's private 0x10A2/0x1A8B surface tints ignored the theme (black-on-black on AMOLED).
// Language, theme, USB-drive and network details moved to the Settings app (the gear shortcut), so the
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
enum { CL_TILES = 0, CL_BRIGHT, CL_VOLUME, CL_SHORT, CC_NLINES };          // the four focusable lines
enum { TL_MUTE = 0, TL_TORCH, TL_SCREEN, TL_HOTSPOT, CC_NTILES };          // toggle tiles
enum { SC_SETTINGS = 0, SC_WEB, SC_USBKBD, SC_USBDRIVE, SC_RESTART, CC_NSHORT };   // shortcuts
#define CC_ARM_HOTSPOT  TL_HOTSPOT
#define CC_ARM_RESTART  (100 + SC_RESTART)

// Geometry (240x135, full screen). Every element keeps a 2 px focus-ring margin that never overlaps a
// neighbour's ring (tiles 4x55 + 4 px gaps, shortcuts 5x43 + 4 px gaps, lines 2 px apart), so a focus
// move only redraws two outlines. Rings: tiles y 20..57, sliders 59..77 / 78..96, shortcuts 98..119.
static const int CC_STRIP_H = 18;
static const int CC_TILE_Y = 22, CC_TILE_H = 34, CC_TILE_W = 55, CC_TILE_P = 59;
static const int CC_BRI_Y = 61, CC_VOL_Y = 80, CC_SL_H = 15, CC_SL_X = 4, CC_SL_W = 232;
static const int CC_TRK_X = 30, CC_TRK_W = 168, CC_TRK_H = 6;                        // slider track
static const int CC_SHORT_Y = 100, CC_SHORT_H = 18, CC_SHORT_W = 43, CC_SHORT_P = 47;
static const int CC_CTX_Y = 121;

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
static const char *cc_tile_label(int i)
{
    switch (i) {
        case TL_MUTE:   return TR("Muto", "Mute");
        case TL_TORCH:  return TR("Torcia", "Torch");
        case TL_SCREEN: return TR("Spegni", "Sleep");
        default:        return "Hotspot";
    }
}
static int cc_short_glyph(int i)
{
    static const int G[CC_NSHORT] = { UG_GEAR, UG_MONITOR, UG_KEYBOARD, UG_USB, UG_POWER };
    return G[i];
}

void launcher_render_control_center_invalidate(void);
void launcher_render_control_center_open(void)          // focus kept: resume; first paint is a full one
{
    s_cc_arm = -1; s_cc_prefs = false; launcher_render_control_center_invalidate();
}

void launcher_render_control_center_close(void)
{
    s_cc_arm = -1;
    if (s_cc_prefs) { s_cc_prefs = false; nucleo_app_persist_prefs(); }   // one settings.json write per visit
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
    if (key != NK_ENTER && !digit) s_cc_arm = -1;          // any other key disarms a pending action

    if (key == NK_BACK) return CC_CLOSE;
    if (digit) {                                           // 1-4: fire a tile directly (smartwatch quick keys)
        s_cc_line = CL_TILES; s_cc_tile = ch - '1';
        if (armed != s_cc_tile) { armed = -1; s_cc_arm = -1; }
        return cc_tile_act(s_cc_tile, armed);
    }
    if (key == NK_UP || key == NK_DOWN) {
        s_cc_line = (s_cc_line + (key == NK_DOWN ? 1 : CC_NLINES - 1)) % CC_NLINES;
        return CC_REDRAW;
    }
    if (key == NK_LEFT || key == NK_RIGHT) {
        int dir = (key == NK_RIGHT) ? 1 : -1;
        switch (s_cc_line) {
            case CL_TILES:  s_cc_tile  = (s_cc_tile  + dir + CC_NTILES) % CC_NTILES; break;
            case CL_SHORT:  s_cc_short = (s_cc_short + dir + CC_NSHORT) % CC_NSHORT; break;
            case CL_BRIGHT: nucleo_app_set_brightness(nucleo_app_brightness() + dir * 5); s_cc_prefs = true; break;
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
    #undef MIX
    return sig;
}
static uint32_t s_cc_drawn_sig = 0;    // latched by every draw (key-driven or tick-driven)

// 1 Hz: true only when the panel's content changed since it was last DRAWN — a static panel is never
// re-blitted, and a key-driven redraw is not repeated by the next tick (ANTI-FLICKER.md).
bool launcher_render_control_center_tick(void) { return cc_sig() != s_cc_drawn_sig; }

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
    uint32_t strip, ctx;
    uint8_t  tile_fill[CC_NTILES];
    uint16_t tile_ink[CC_NTILES], tile_ring[CC_NTILES];
    uint8_t  sl_glyph[2], sl_kr[2];
    uint16_t sl_ink[2], sl_ring[2], sl_col[2];
    int16_t  sl_val[2], sl_kx[2];
    uint16_t sh_ring[CC_NSHORT], sh_ink[CC_NSHORT];
    int16_t  clock_r;
};
static CcShown s_ccs;

void launcher_render_control_center_invalidate(void) { s_ccs.valid = false; }

static unsigned short cc_focus_col(bool foc, bool armed) { return foc ? (armed ? C_RED : THEME_ACC) : BG; }

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

template <typename T> static void cc_tile(T *g, int i, bool full)
{
    int x = 4 + i * CC_TILE_P, y = CC_TILE_Y;
    bool on = cc_tile_on(i), foc = (s_cc_line == CL_TILES && s_cc_tile == i);
    bool off = (i == TL_HOTSPOT && !cc_hotspot_ok());                 // unavailable this boot
    uint8_t  fk   = (uint8_t)((on ? 1 : 0) | (off ? 2 : 0));
    unsigned short fill = on ? cc_tile_col(i) : LINE;
    unsigned short ink  = on ? INK : off ? DIM : (foc ? FG : MUTED);
    unsigned short ring = cc_focus_col(foc, i == TL_HOTSPOT && s_cc_arm == CC_ARM_HOTSPOT);
    if (full || ring != s_ccs.tile_ring[i]) { cc_ring(g, x, y, CC_TILE_W, CC_TILE_H, 8, ring); s_ccs.tile_ring[i] = ring; }
    bool refill = full || fk != s_ccs.tile_fill[i];
    if (refill) g->fillRoundRect(x, y, CC_TILE_W, CC_TILE_H, 8, fill);
    if (refill || ink != s_ccs.tile_ink[i]) {
        ui_glyph(g, cc_tile_glyph(i), x + CC_TILE_W / 2, y + 12, 7, ink, fill);
        const char *lb = cc_tile_label(i);
        g->setTextSize(1); g->setTextColor(ink, fill);
        g->setCursor(x + (CC_TILE_W - (int)strlen(lb) * 6) / 2, y + 24); g->print(lb);
        char k[2] = { (char)('1' + i), 0 };                           // quick-key badge
        g->setTextColor(on ? INK : DIM, fill); g->setCursor(x + 4, y + 3); g->print(k);
    }
    s_ccs.tile_fill[i] = fk; s_ccs.tile_ink[i] = ink;
}

template <typename T> static void cc_slider(T *g, int k, bool full)
{
    int  line = k ? CL_VOLUME : CL_BRIGHT, y = k ? CC_VOL_Y : CC_BRI_Y, cy = y + CC_SL_H / 2;
    bool foc = (s_cc_line == line), muted = k && nucleo_audio_is_muted();
    int  val = k ? nucleo_audio_volume() : nucleo_app_brightness();
    unsigned short sem  = k ? (muted ? DIM : C_GREEN) : C_YELLOW;
    unsigned short ink  = foc ? FG : MUTED;
    unsigned short ring = cc_focus_col(foc, false);
    uint8_t glyph = (uint8_t)(k ? (muted ? UG_MUTE : UG_SPEAKER) : UG_SUN);
    if (full || ring != s_ccs.sl_ring[k]) { cc_ring(g, CC_SL_X, y, CC_SL_W, CC_SL_H, 7, ring); s_ccs.sl_ring[k] = ring; }
    if (full || glyph != s_ccs.sl_glyph[k] || ink != s_ccs.sl_ink[k]) {
        if (!full && glyph != s_ccs.sl_glyph[k]) g->fillRect(7, y, 17, CC_SL_H, BG);   // a different icon SHAPE
        ui_glyph(g, glyph, 15, cy, 6, ink, BG);
    }
    int fw = val * CC_TRK_W / 100; if (fw < 0) fw = 0; if (fw > CC_TRK_W) fw = CC_TRK_W;
    int kr = foc ? 5 : 3;
    int kx = CC_TRK_X + fw; if (kx < CC_TRK_X + 5) kx = CC_TRK_X + 5; if (kx > CC_TRK_X + CC_TRK_W - 5) kx = CC_TRK_X + CC_TRK_W - 5;
    if (full || val != s_ccs.sl_val[k] || kr != s_ccs.sl_kr[k] || kx != s_ccs.sl_kx[k] || sem != s_ccs.sl_col[k] || ink != s_ccs.sl_ink[k]) {
        if (!full) g->fillCircle(s_ccs.sl_kx[k], cy, s_ccs.sl_kr[k], BG);            // lift the old knob
        int ty = cy - CC_TRK_H / 2;
        if (fw < CC_TRK_W) { g->setClipRect(CC_TRK_X + fw, ty, CC_TRK_W - fw, CC_TRK_H); g->fillRoundRect(CC_TRK_X, ty, CC_TRK_W, CC_TRK_H, 3, LINE); }
        if (fw > 0)        { g->setClipRect(CC_TRK_X, ty, fw, CC_TRK_H);                g->fillRoundRect(CC_TRK_X, ty, CC_TRK_W, CC_TRK_H, 3, sem); }
        g->clearClipRect();
        g->fillCircle(kx, cy, kr, foc ? FG : sem);
        char b[8]; snprintf(b, sizeof b, "%3d%%", val);                // fixed 4-char field, opaque
        g->setTextSize(1); g->setTextColor(ink, BG);
        g->setCursor(CC_SL_X + CC_SL_W - 4 - 24, cy - 3); g->print(b);
    }
    s_ccs.sl_glyph[k] = glyph; s_ccs.sl_ink[k] = ink; s_ccs.sl_val[k] = (int16_t)val;
    s_ccs.sl_kx[k] = (int16_t)kx; s_ccs.sl_kr[k] = (uint8_t)kr; s_ccs.sl_col[k] = sem;
}

template <typename T> static void cc_short(T *g, int i, bool full)
{
    int x = 4 + i * CC_SHORT_P, y = CC_SHORT_Y;
    bool foc = (s_cc_line == CL_SHORT && s_cc_short == i);
    unsigned short ring = cc_focus_col(foc, i == SC_RESTART && s_cc_arm == CC_ARM_RESTART);
    unsigned short ink  = (i == SC_RESTART) ? C_RED : (foc ? FG : MUTED);
    if (full) g->fillRoundRect(x, y, CC_SHORT_W, CC_SHORT_H, 7, LINE);
    if (full || ring != s_ccs.sh_ring[i]) { cc_ring(g, x, y, CC_SHORT_W, CC_SHORT_H, 7, ring); s_ccs.sh_ring[i] = ring; }
    if (full || ink != s_ccs.sh_ink[i])   { ui_glyph(g, cc_short_glyph(i), x + CC_SHORT_W / 2, y + CC_SHORT_H / 2, 6, ink, LINE); s_ccs.sh_ink[i] = ink; }
}

// The one line of text on the panel: names the focused control, its live state and the key that acts
// (vocabulary of docs/native-ui-kit.md §5). Red while a disruptive action is armed.
static const char *cc_context_text(char *b, int cap, unsigned short *col)
{
    const char *s = b; *col = MUTED; b[0] = 0;
    if (s_cc_arm == CC_ARM_HOTSPOT) {
        *col = C_RED;
        return nucleo_setup_ap_intended() ? TR("invio spegne hotspot   esc annulla", "enter hotspot off   esc cancel")
                                          : TR("invio accende hotspot   esc annulla", "enter hotspot on   esc cancel");
    }
    if (s_cc_arm == CC_ARM_RESTART) { *col = C_RED; return TR("invio riavvia ora   esc annulla", "enter restart now   esc cancel"); }
    if (s_cc_line == CL_TILES) {
        switch (s_cc_tile) {
            case TL_MUTE:   return nucleo_audio_is_muted() ? TR("Audio muto   invio riattiva", "Sound muted   enter unmute")
                                                           : TR("Audio attivo   invio silenzia", "Sound on   enter mute");
            case TL_TORCH:  return TR("Torcia   invio accende", "Torch   enter turn on");
            case TL_SCREEN: return TR("Spegni schermo   un tasto riaccende", "Screen off   any key wakes it");
            default:
                if (!cc_hotspot_ok()) return TR("Hotspot non disponibile in questa app", "Hotspot unavailable in this app");
                if (nucleo_setup_ap_intended()) { snprintf(b, cap, "%.18s  192.168.4.1", nucleo_setup_ap_ssid()); *col = C_YELLOW; return s; }
                return TR("Hotspot spento   invio x2 accende", "Hotspot off   enter x2 turns on");
        }
    }
    if (s_cc_line == CL_BRIGHT) { snprintf(b, cap, TR("Luminosita %d%%   </> regola", "Brightness %d%%   </> adjust"), nucleo_app_brightness()); return s; }
    if (s_cc_line == CL_VOLUME) {
        if (nucleo_audio_is_muted()) snprintf(b, cap, TR("Volume %d%% muto   invio riattiva", "Volume %d%% muted   enter unmute"), nucleo_audio_volume());
        else                         snprintf(b, cap, TR("Volume %d%%   </> regola   invio muto", "Volume %d%%   </> adjust   enter mute"), nucleo_audio_volume());
        return s;
    }
    switch (s_cc_short) {
        case SC_SETTINGS: return TR("Tutte le impostazioni   invio apri", "All settings   enter open");
        case SC_WEB: {
            const char *ip = cc_online() ? nucleo_setup_ip() : nucleo_setup_ap_active() ? "192.168.4.1" : "--";
            snprintf(b, cap, "Web %.15s   PIN %.8s", ip, nucleo_auth_pin()); *col = FG; return s;
        }
        case SC_USBKBD:   return TR("Tastiera USB per il PC   invio apri", "USB keyboard for a PC   enter open");
        case SC_USBDRIVE: return TR("Scheda SD come disco USB   invio apri", "SD card as a USB drive   enter open");
        default:          return TR("Riavvia il dispositivo   invio x2", "Restart the device   enter x2");
    }
}

template <typename T> static void cc_context(T *g, bool full)
{
    char b[48]; unsigned short c; const char *s = cc_context_text(b, sizeof b, &c);
    char o[40]; memset(o, ' ', 39); o[39] = 0;                        // centred in a fixed 39-column field
    int len = (int)strlen(s); if (len > 39) len = 39;
    memcpy(o + (39 - len) / 2, s, len);
    uint32_t h = 2166136261u; for (const char *p = o; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    h ^= c;
    if (!full && h == s_ccs.ctx) return;
    s_ccs.ctx = h;
    if (full) g->drawFastHLine(0, CC_CTX_Y, W, LINE);
    g->setTextSize(1); g->setTextColor(c, BG); g->setCursor(3, CC_CTX_Y + 4); g->print(o);
}

template <typename T> static void cc_paint(T *g, bool full)
{
    if (full) g->fillScreen(BG);
    cc_strip(g, full);
    for (int i = 0; i < CC_NTILES; i++) cc_tile(g, i, full);
    cc_slider(g, 0, full);
    cc_slider(g, 1, full);
    for (int i = 0; i < CC_NSHORT; i++) cc_short(g, i, full);
    cc_context(g, full);
}

void launcher_render_control_center(void)
{
    s_cc_drawn_sig = cc_sig();
    M5Canvas *c = nucleo_screen();
    if (c) { cc_paint(c, true); c->pushSprite(0, 0); }                // back-buffer: compose all, ONE blit
    else   { d.startWrite(); cc_paint(&d, !s_ccs.valid); d.endWrite(); }   // direct: only what changed
    s_ccs.valid = true;                                               // the screen now matches s_ccs
}
