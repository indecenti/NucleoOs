// app_wifi.cpp — NucleoOS Settings (launcher id "wifi", title "Impostazioni" / "Settings").
//
// A modern smartwatch settings app on a 240x135 screen with a real keyboard:
//
//   ROOT   one list; every row = glyph + name + a live PREVIEW of its value. A SUGGESTION chip rides on
//          top when something needs attention (clock never set, no network, battery low + bright screen).
//            Wi-Fi · Hotspot · Bluetooth · Display · Sound · ANIMA (inline) · Language (inline)
//            · Date/time · Device · Reset
//   PAGE   the rows of one section. Sliders / choices adjust IN PLACE with LEFT/RIGHT (held = faster,
//          like a watch crown), switches flip on ENTER, text fields open the inline editor, disruptive
//          actions ask first (confirm card) or need ENTER x3 (resets).
//   SEARCH type any letter on the root: every setting of every section is filtered as you type and the
//          results ACT like the real rows (flip, adjust, open) — the keyboard is the Cardputer's crown.
//   SUB    Nearby networks (scan + join) · Saved networks (prefer / forget) · date-time field editor.
//
// Look (docs/native-ui-kit.md): a fisheye list — the focused row is an expanded accent CHIP on two lines
// (name + a readable secondary line: its value, a slider bar, "< choice >" or what it does), the other
// rows are compact with their value in the proportional Font2 (16 px, full colour — not tiny grey
// 6x8). Theme roles only; the chip is THEME_ACC, so changing the theme recolours this screen live.
// Keys, same everywhere: UP/DOWN move (wrap) · 1-9 jump (root: open) · ENTER act · LEFT/RIGHT adjust
// (elsewhere RIGHT opens, LEFT goes back; LEFT never closes the app) · Esc back · TAB next section.
// Focus is remembered per page (resume). Rows are built on demand: a few hundred bytes of .bss total.

#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "launcher_theme.h"   // W/H + themed BG/FG/MUTED/DIM/LINE/INK + C_* accents
#include "app_gfx.h"
#include "app_ui.h"           // app_ui_confirm / app_ui_ascii_fold
#include "ui_glyph.h"         // system glyph set (shared with the Control Center)
#include "nucleo_i18n.h"      // TR5(it,en,es,fr,de) + the 5-language OS switch
#include "nucleo_guest.h"     // installed by M5Launcher: Device page gains "Back to M5Launcher"
#include <M5GFX.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include <string.h>
#include <strings.h>          // strcasecmp: the reset's keep-list match (FAT names fold case)
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>

extern "C" {
#include "nucleo_storage.h"
#include "nucleo_voice.h"
}

// ---- platform hooks (resolved at link) ---------------------------------------
extern "C" {
const char *nucleo_setup_mode(void);
const char *nucleo_setup_ssid(void);
const char *nucleo_setup_ip(void);
int         nucleo_setup_rssi(void);
int         nucleo_setup_channel(void);
const char *nucleo_setup_device_name(void);
bool        nucleo_setup_time_synced(void);
void        nucleo_setup_set_datetime(int year, int mon, int day, int hour, int min);
int         nucleo_setup_scan(void);
int         nucleo_setup_scan_count(void);
const char *nucleo_setup_scan_ssid(int i);
int         nucleo_setup_scan_rssi(int i);
int         nucleo_setup_scan_channel(int i);
int         nucleo_setup_scan_secure(int i);
const char *nucleo_setup_scan_auth_label(int i);
bool        nucleo_setup_join(const char *ssid, const char *pass);
bool        nucleo_setup_onboarding(void);                // first boot: the network step is ours (see enter())
bool        nucleo_setup_onboard_finish(void);
void        nucleo_setup_start_ap(void);
void        nucleo_setup_stop_ap(void);
void        nucleo_setup_forget(void);
bool        nucleo_setup_factory_reset(void);             // Reset rows: every tier of setup/networks + esp_wifi creds
bool        nucleo_setup_net_is_known(const char *ssid);
bool        nucleo_setup_net_has_password(const char *ssid);
void        nucleo_setup_forget_ssid(const char *ssid);
int         nucleo_setup_net_count(void);
const char *nucleo_setup_net_ssid(int i);
int         nucleo_setup_net_priority(int i);
void        nucleo_setup_net_set_priority(const char *ssid, int prio);
void        nucleo_setup_set_device_name(const char *name);
const char *nucleo_setup_ap_ssid(void);
const char *nucleo_setup_ap_pass(void);
bool        nucleo_setup_ap_secure(void);
bool        nucleo_setup_ap_intended(void);
bool        nucleo_setup_ap_active(void);
bool        nucleo_setup_ap_rescue(void);
void        nucleo_setup_set_ap_ssid(const char *ssid);
void        nucleo_setup_set_ap_pass(const char *pass);
const char *nucleo_auth_pin(void);
int         nucleo_auth_revoke(const char *keep_token);   // NULL = every web session
int         nucleo_auth_session_count(void);
bool        nucleo_auth_factory_reset(void);              // Factory reset: PIN + sessions from /cfg and NVS
bool        nucleo_mailcfg_erase_all(void);               // Factory reset: every SMTP account (nucleo_smtp)
bool        nucleo_keydeck_forget(void);                  // Factory reset: Key deck server address + PIN
int         nucleo_audio_volume(void);
void        nucleo_audio_set_volume(int pct);
void        nucleo_audio_set_mute(bool muted);
bool        nucleo_audio_is_muted(void);
bool        nucleo_remote_enabled(void);                  // app_remote.cpp: auto web handoff (NVS)
void        nucleo_remote_set_enabled(bool on);
bool        nucleo_tts_available(void);                   // voice pack present on the SD
bool        nucleo_tts_enabled(void);                     // speak answers aloud (SD speak.cfg)
void        nucleo_tts_set_enabled(bool on);
int         nucleo_tts_speed(void);                       // 70..160 %
void        nucleo_tts_set_speed(int pct);
int         nucleo_tts_speed_clamp(int pct);             // [70,160], what set_speed would store
bool        nucleo_ble_radio_present(void);               // radio alive this boot
bool        nucleo_ble_pref_enabled(void);                // on at boot (NVS), applied on the next boot
void        nucleo_ble_set_pref(bool on);
int         nucleo_screensaver_timeout_s(void);           // app_screensaver.cpp (0 = never)
void        nucleo_screensaver_set_timeout_s(int sec);
int         nucleo_screensaver_mode(void);                // 0 screen off, 1 clock, 2 stars, 3 fire
void        nucleo_screensaver_set_mode(int mode);
int         nucleo_anima_ui_online_mode(void);            // app_anima.cpp: 0 offline, 1 hybrid, 2 online only
void        nucleo_anima_ui_set_online_mode(int mode);
int         nucleo_power_battery_pct(void);               // -1 = unavailable
int         nucleo_power_battery_mv(void);
bool        nucleo_ui_is_adv(void);
}

// ---- palette: theme roles + named semantics ---------------------------------
#define ACC THEME_ACC                          // chip + values: the theme accent (recolours live)
static const uint16_t GOOD = C_GREEN, WARN = C_YELLOW, BAD = C_RED;

#define DT_EPOCH_2023 1672531200               // the clock is "real" (NTP/manual) at/after 2023-01-01
#define DT_YEAR_MIN   2023                     // the date editor's floor: an earlier year would read as "not set"
#define WIFI_PIN_PRIO 5                        // "preferred" priority (the store clamps 0..9)

// ---- geometry ---------------------------------------------------------------
static const int HDR   = 18;                   // header band: title + hairline
static const int ROW_H = 19;                   // compact row
static const int ROW_F = 34;                   // focused row: an expanded two-line chip

// ---- pages + rows -----------------------------------------------------------
enum { PG_ROOT = 0, PG_WIFI, PG_AP, PG_BT, PG_DISPLAY, PG_SOUND, PG_DEVICE, PG_RESET, PG_NETS, PG_SAVED, PG_SEARCH, PG_N };
enum { K_NAV = 0, K_INFO, K_TOGGLE, K_SLIDER, K_CYCLE, K_EDIT, K_DANGER };
enum {
    R_NONE = 0,
    R_SUGGEST,                                                                              // root chip
    R_S_WIFI, R_S_AP, R_S_BT, R_S_DISPLAY, R_S_SOUND, R_ANIMA, R_LANG, R_DATETIME, R_S_DEVICE, R_S_RESET,
    R_NETS, R_SAVED, R_HANDOFF,                                                             // Wi-Fi
    R_AP_ON, R_AP_STATE, R_AP_SSID, R_AP_PASS, R_AP_ADDR,                                   // Hotspot
    R_BT_BOOT, R_BT_STATE,                                                                  // Bluetooth
    R_BRIGHT, R_THEME, R_SAVER_TIME, R_SAVER_STYLE,                                         // Display
    R_VOLUME, R_MUTE, R_TTS, R_TTS_SPEED, R_VOICE,                                          // Sound
    R_NAME, R_PIN, R_SESSIONS, R_MODEL, R_VERSION, R_BATTERY, R_SD, R_RAM, R_UPTIME, R_UPDATES, R_RESTART, R_LAUNCHER,  // Device
    R_RST_SOFT, R_RST_HARD, R_RST_FMT, R_RST_SD,                                            // Reset
    R_SAVED_NET, R_FORGET_ALL,                                                              // Saved networks
    R_OB_SKIP,                                                                              // first boot: skip Wi-Fi (confirm only)
};

static const uint8_t ROWS_ROOT[]    = { R_S_WIFI, R_S_AP, R_S_BT, R_S_DISPLAY, R_S_SOUND, R_ANIMA, R_LANG, R_DATETIME, R_S_DEVICE, R_S_RESET };
static const uint8_t ROWS_WIFI[]    = { R_NETS, R_SAVED, R_HANDOFF };
static const uint8_t ROWS_AP[]      = { R_AP_ON, R_AP_STATE, R_AP_SSID, R_AP_PASS, R_AP_ADDR };
static const uint8_t ROWS_BT[]      = { R_BT_BOOT, R_BT_STATE, R_RESTART };
static const uint8_t ROWS_DISPLAY[] = { R_BRIGHT, R_THEME, R_SAVER_TIME, R_SAVER_STYLE };
static const uint8_t ROWS_SOUND[]   = { R_VOLUME, R_MUTE, R_TTS, R_TTS_SPEED, R_VOICE };
static const uint8_t ROWS_DEVICE[]  = { R_NAME, R_PIN, R_SESSIONS, R_MODEL, R_VERSION, R_BATTERY, R_SD, R_RAM, R_UPTIME, R_UPDATES, R_RESTART };
// Same page when installed by M5Launcher (runtime-detected; stand-alone never shows the extra row).
static const uint8_t ROWS_DEVICE_HOSTED[] = { R_NAME, R_PIN, R_SESSIONS, R_MODEL, R_VERSION, R_BATTERY, R_SD, R_RAM, R_UPTIME, R_UPDATES, R_RESTART, R_LAUNCHER };
static const uint8_t ROWS_RESET[]   = { R_RST_SOFT, R_RST_HARD, R_RST_FMT, R_RST_SD };
static bool is_rst(uint8_t id) { return id == R_RST_SOFT || id == R_RST_HARD || id == R_RST_FMT || id == R_RST_SD; }   // ENTER x3 rows
static const uint8_t SECTIONS[]     = { PG_WIFI, PG_AP, PG_BT, PG_DISPLAY, PG_SOUND, PG_DEVICE, PG_RESET };   // TAB order
#define NROWS(a) ((int)(sizeof(a) / sizeof((a)[0])))

// One row, built on demand from its id (never stored).
struct Row {
    const char *label;
    char     val[24];        // short value: right side of a compact row
    char     sub[40];        // secondary line of the focused chip (falls back to val)
    uint8_t  id, kind, glyph, vsz;
    bool     on, dis;        // switch state · unavailable (drawn dim, ENTER explains)
    int16_t  num, lo, hi;    // slider value + range
    uint16_t col;            // value colour
};

// ---- state ------------------------------------------------------------------
static uint8_t s_page = PG_ROOT;
static int8_t  s_sel[PG_N];                    // focus per page — kept across visits (resume)
static int     s_scroll = 0;                   // pixel scroll of the current (variable-height) list
static bool    s_prefs_dirty = false;          // brightness/volume moved: persist when the focus leaves
static bool    s_theme_pend = false;           // theme previewed while cycling; saved once on flush
static int     s_saver_t_pend = -1, s_saver_m_pend = -1;   // screensaver timeout/style being picked (-1 = none)
static int     s_tts_pend = -1;                // voice speed being dialled (RAM only); -1 = none. Committed on
                                               // flush: nucleo_tts_set_speed writes the SD AND purges the WAV cache
static int64_t s_adj_us = 0;                   // last slider step (hold-to-accelerate)
static int64_t s_cyc_us = 0;                   // last choice step (held arrows throttled: each step persists)
static int8_t  s_tts_ok = -1, s_bt_pref = -1;  // cached per visit: the TTS probe opens an SD index and the
                                               // BLE pref is an NVS read — never per painted frame

static char    s_q[17]; static int s_qn = 0;   // search query
static uint8_t s_hit_pg[18], s_hit_id[18];     // search results (page, row id)
static int     s_hits = 0;

enum { IM_NONE = 0, IM_PASS, IM_NAME, IM_APSSID, IM_APPASS };
// The join password is no longer typed blind: like a phone, the character just typed shows for 1.5 s,
// and TAB shows / hides the whole password. On the Cardputer's tiny keys a wrong letter behind a row of
// asterisks was the usual reason a join "failed".
static bool    s_reveal = false;
static int64_t s_ikey_us = 0;                  // when the last character was typed (0 = none showing)
static const int64_t PEEK_US = 1500000;
static int  s_im = IM_NONE;
static char s_ibuf[80]; static int s_ilen = 0;
static char s_join_ssid[33];

static uint8_t s_cf = R_NONE;                  // pending confirm card (row id of the action)
static char    s_cf_ssid[33];                  // its target network, captured at arm time (never an index:
                                               // the web API can reorder the saved list meanwhile)
static bool    s_cf_yes = false;               // card focus (starts on No)
static uint8_t s_rst_id = R_NONE;              // factory-reset countdown: armed row + presses left
static int     s_rst_left = 0;

static bool s_dt = false;                      // date/time editor
static int  s_dtf = 0;                         // focused field 0=Y 1=M 2=D 3=h 4=m
static int  s_dtv[5];

enum { OP_NONE = 0, OP_SCAN, OP_JOIN, OP_FINISH };   // FINISH: end the first-boot network step (hotspot + persist)
static volatile int  s_op = OP_NONE;
static volatile bool s_busy = false, s_done = false, s_join_ok = false;
static char s_join_pass[80];
static TaskHandle_t  s_task = nullptr;
static int  s_anim = 0, s_tickn = 0;
static uint32_t s_live = 0;                    // signature of the live values on screen (1 Hz redraw gate)

static char s_msg[48]; static int s_msg_t = 0; static bool s_msg_ok = false;
static void toast(const char *m)    { snprintf(s_msg, sizeof s_msg, "%s", m); s_msg_t = 12; s_msg_ok = false; }
static void toast_ok(const char *m) { toast(m); s_msg_ok = true; }   // with a tick: the watch "done" beat

// ---- small helpers ------------------------------------------------------------
static bool tts_ok(void)  { if (s_tts_ok < 0) s_tts_ok = nucleo_tts_available() ? 1 : 0; return s_tts_ok == 1; }
static bool bt_pref(void) { if (s_bt_pref < 0) s_bt_pref = nucleo_ble_pref_enabled() ? 1 : 0; return s_bt_pref == 1; }
static int  squality(int rssi) { if (!rssi) return 0; int q = 2 * (rssi + 100); return q < 0 ? 0 : q > 100 ? 100 : q; }
static uint16_t qcol(int q)    { return q >= 60 ? GOOD : q >= 33 ? WARN : BAD; }
static bool connected(void)    { return !strcmp(nucleo_setup_mode(), "sta") && nucleo_setup_ip()[0]; }
static bool ap_on(void)        { return nucleo_setup_ap_intended(); }   // the user's choice (not a rescue fallback)
static bool ap_up(void)        { return nucleo_setup_ap_active(); }     // reachable right now (chosen OR rescue)
static bool ap_resc(void)      { return nucleo_setup_ap_rescue(); }     // up only as a temporary STA fallback

static int prio_of(const char *ssid)
{
    if (!ssid || !ssid[0]) return 0;
    int n = nucleo_setup_net_count();
    for (int i = 0; i < n; i++) if (!strcmp(nucleo_setup_net_ssid(i), ssid)) return nucleo_setup_net_priority(i);
    return 0;
}

static void txt(int x, int y, const char *s, uint16_t fg, uint16_t bg, int sz)
{
    d.setTextSize(sz); d.setTextColor(fg, bg); d.setCursor(x, y); d.print(s);
}
// Default-font text truncated to `maxw` px (6*sz per glyph).
static void txt_fit(int x, int y, const char *s, int maxw, uint16_t fg, uint16_t bg, int sz)
{
    int mc = maxw / (6 * sz); if (mc < 1) return; if (mc > 38) mc = 38;
    char b[40]; snprintf(b, sizeof b, "%.*s", mc, s ? s : "");
    txt(x, y, b, fg, bg, sz);
}
// Font2 (16 px proportional) — the readable face for values and secondary lines. Cut with ".." to fit
// `maxw`; `right` anchors the text's right edge at x. Returns the drawn width. Restores Font0.
static int f2(int x, int y, const char *s, int maxw, uint16_t fg, uint16_t bg, bool right)
{
    if (!s || !s[0] || maxw < 10) return 0;
    d.setFont(&fonts::Font2); d.setTextSize(1);
    char b[48]; snprintf(b, sizeof b, "%s", s);
    int w = (int)d.textWidth(b);
    if (w > maxw) {
        char t[52]; snprintf(t, sizeof t, "%.1s..", b);            // fallback when even one glyph overflows
        for (int n = (int)strlen(b) - 1; n > 0; n--) {
            b[n] = 0; snprintf(t, sizeof t, "%s..", b);
            if ((int)d.textWidth(t) <= maxw) break;
        }
        snprintf(b, sizeof b, "%s", t);
        w = (int)d.textWidth(b);
    }
    d.setTextColor(fg, bg); d.setCursor(right ? x - w : x, y); d.print(b);
    d.setFont(&fonts::Font0);
    return w;
}
static void vset(Row &r, const char *s) { snprintf(r.val, sizeof r.val, "%s", s ? s : ""); }
static void sset(Row &r, const char *s) { snprintf(r.sub, sizeof r.sub, "%s", s ? s : ""); }
static void fmt_dur(char *b, int cap, int sec)
{
    if (sec <= 0)           snprintf(b, cap, "%s", TR5("Mai", "Never", "Nunca", "Jamais", "Nie"));
    else if (sec < 60)      snprintf(b, cap, "%d s", sec);
    else if (sec % 60 == 0) snprintf(b, cap, "%d min", sec / 60);
    else                    snprintf(b, cap, "%dm %ds", sec / 60, sec % 60);
}

// ---- choice lists -------------------------------------------------------------
static const struct { const char *code, *name; } LANGS[] = {
    { "it", "Italiano" }, { "en", "English" }, { "es", "Espanol" }, { "fr", "Francais" }, { "de", "Deutsch" },
};
static int lang_idx(void)
{
    const char *l = nucleo_i18n_lang();
    for (int i = 0; i < NROWS(LANGS); i++) if (l && !strcmp(LANGS[i].code, l)) return i;
    return 1;
}
static void lang_cycle(int dir) { nucleo_i18n_set_lang(LANGS[(lang_idx() + dir + NROWS(LANGS)) % NROWS(LANGS)].code); }

static const char *theme_name(void)
{
    int cnt = 0; const nucleo_theme_t *all = nucleo_theme_get_all(&cnt);
    const char *cur = nucleo_theme_get_current();
    for (int i = 0; i < cnt; i++) if (all[i].id && cur && !strcmp(all[i].id, cur)) return all[i].name;
    return cur ? cur : "?";
}
static void theme_cycle(int dir)
{
    int cnt = 0; const nucleo_theme_t *all = nucleo_theme_get_all(&cnt);
    if (cnt <= 0) return;
    const char *cur = nucleo_theme_get_current(); int idx = 0;
    for (int i = 0; i < cnt; i++) if (all[i].id && cur && !strcmp(all[i].id, cur)) { idx = i; break; }
    nucleo_theme_preview(all[(idx + dir + cnt) % cnt].id);        // live; persisted once by flush_prefs
    s_theme_pend = true; s_prefs_dirty = true;
}

static const int SAVER_STEPS[] = { 0, 15, 30, 60, 120, 300, 600 };
static void saver_time_cycle(int dir)
{
    int cur = s_saver_t_pend >= 0 ? s_saver_t_pend : nucleo_screensaver_timeout_s(), n = NROWS(SAVER_STEPS), idx = -1;
    if (dir > 0) { for (int i = 0; i < n; i++) if (SAVER_STEPS[i] > cur) { idx = i; break; } if (idx < 0) idx = 0; }
    else         { for (int i = n - 1; i >= 0; i--) if (SAVER_STEPS[i] < cur) { idx = i; break; } if (idx < 0) idx = n - 1; }
    s_saver_t_pend = SAVER_STEPS[idx]; s_prefs_dirty = true;         // saved once by flush_prefs
}
static const char *saver_style_name(int m)
{
    switch (m) {
        case 0:  return TR5("Schermo nero", "Blank screen", "Pantalla negra", "Ecran noir", "Schwarzbild");
        case 2:  return TR5("Stelle", "Stars", "Astros", "Etoiles", "Sterne");
        case 3:  return TR5("Fuoco", "Fire", "Fuego", "Feu", "Feuer");
        default: return TR5("Orologio", "Clock", "Reloj", "Horloge", "Uhr");
    }
}
static const char *anima_mode_name(int m)
{
    return m == 0 ? "Offline"
         : m == 2 ? TR5("Solo online", "Online only", "Solo online", "En ligne seul", "Nur online")
         :          TR5("Ibrida", "Hybrid", "Hibrido", "Hybride", "Hybrid");
}
static const char *anima_mode_sub(int m)
{
    return m == 0 ? TR5("Solo conoscenza sul device", "Knowledge on the device only", "Solo conocimiento local",
                        "Savoir local uniquement", "Nur lokales Wissen")
         : m == 2 ? TR5("Sempre il modello cloud", "Always the cloud model", "Siempre el modelo cloud",
                        "Toujours le modele cloud", "Immer das Cloud-Modell")
         :          TR5("Prima offline, poi il cloud", "Offline first, then cloud", "Primero offline, luego cloud",
                        "Offline d'abord, puis cloud", "Erst offline, dann Cloud");
}

// ---- suggestion chip (root, first row) -----------------------------------------
enum { SG_NONE = 0, SG_CLOCK, SG_BATT, SG_WIFI };
static int suggestion(void)
{
    if (time(NULL) < DT_EPOCH_2023) return SG_CLOCK;
    int b = nucleo_power_battery_pct();
    if (b >= 0 && b <= 15 && nucleo_app_brightness() > 40) return SG_BATT;
    if (!connected() && !ap_on()) return SG_WIFI;                  // offline (or only the rescue hotspot)
    return SG_NONE;
}

// ---- page model -----------------------------------------------------------------
static const uint8_t *page_rows(int pg, int *n)
{
    switch (pg) {
        case PG_ROOT:    *n = NROWS(ROWS_ROOT);    return ROWS_ROOT;
        case PG_WIFI:    *n = NROWS(ROWS_WIFI);    return ROWS_WIFI;
        case PG_AP:      *n = NROWS(ROWS_AP);      return ROWS_AP;
        case PG_BT:      *n = NROWS(ROWS_BT);      return ROWS_BT;
        case PG_DISPLAY: *n = NROWS(ROWS_DISPLAY); return ROWS_DISPLAY;
        case PG_SOUND:   *n = NROWS(ROWS_SOUND);   return ROWS_SOUND;
        case PG_DEVICE:  if (nucleo_guest_hosted()) { *n = NROWS(ROWS_DEVICE_HOSTED); return ROWS_DEVICE_HOSTED; }
                         *n = NROWS(ROWS_DEVICE);  return ROWS_DEVICE;
        case PG_RESET:   *n = NROWS(ROWS_RESET);   return ROWS_RESET;
        default:         *n = 0;                   return nullptr;
    }
}
static int page_count(int pg)
{
    if (pg == PG_NETS)   return nucleo_setup_scan_count() + 1;             // row 0 = scan again
    if (pg == PG_SAVED)  { int k = nucleo_setup_net_count(); return k ? k + 1 : 0; }   // + "forget all"
    if (pg == PG_SEARCH) return s_hits;
    int n; page_rows(pg, &n);
    if (pg == PG_ROOT && suggestion() != SG_NONE) n++;
    return n;
}
static int page_parent(int pg) { return (pg == PG_NETS || pg == PG_SAVED) ? PG_WIFI : PG_ROOT; }
static const char *page_title(int pg)
{
    switch (pg) {
        case PG_WIFI:    return "Wi-Fi";
        case PG_AP:      return "Hotspot";
        case PG_BT:      return "Bluetooth";
        case PG_DISPLAY: return TR5("Schermo", "Display", "Pantalla", "Ecran", "Anzeige");
        case PG_SOUND:   return TR5("Suono", "Sound", "Sonido", "Son", "Ton");
        case PG_DEVICE:  return TR5("Dispositivo", "Device", "Dispositivo", "Appareil", "Geraet");
        case PG_RESET:   return TR5("Ripristino", "Reset", "Restablecer", "Restauration", "Ruecksetzen");
        case PG_NETS:    return TR5("Reti vicine", "Nearby networks", "Redes cercanas", "Reseaux proches", "Netze in Naehe");
        case PG_SAVED:   return TR5("Reti salvate", "Saved networks", "Redes guardadas", "Reseaux connus", "Bekannte Netze");
        default:         return TR5("Impostazioni", "Settings", "Ajustes", "Reglages", "Einstellungen");
    }
}
static int section_of(uint8_t id)
{
    switch (id) {
        case R_S_WIFI: return PG_WIFI;   case R_S_AP: return PG_AP;          case R_S_BT: return PG_BT;
        case R_S_DISPLAY: return PG_DISPLAY; case R_S_SOUND: return PG_SOUND; case R_S_DEVICE: return PG_DEVICE;
        case R_S_RESET: return PG_RESET;
        default: return -1;
    }
}

// Fill `r` for the row with id `id` (live values read here, at paint/key time).
static void make_row_id(uint8_t id, int num, Row &r)
{
    memset(&r, 0, sizeof r);
    r.id = id; r.num = (int16_t)num; r.kind = K_NAV; r.col = ACC; r.vsz = 1; r.label = "";
    bool muted = nucleo_audio_is_muted();
    char b[32];
    switch (id) {
    case R_SUGGEST: {
        int sg = suggestion(); r.col = WARN;
        if (sg == SG_CLOCK) {
            r.label = TR5("Imposta l'ora", "Set the clock", "Poner la hora", "Regler l'heure", "Uhr stellen"); r.glyph = UG_CLOCK;
            sset(r, TR5("L'orologio non e mai stato impostato", "The clock was never set", "El reloj nunca se ajusto",
                        "Horloge jamais reglee", "Uhr wurde nie gestellt"));
        } else if (sg == SG_BATT) {
            r.label = TR5("Batteria bassa", "Battery low", "Bateria baja", "Batterie faible", "Akku schwach"); r.glyph = UG_BATTERY;
            sset(r, TR5("Invio: luminosita al 40%", "Enter: brightness to 40%", "Enter: brillo al 40%",
                        "Enter: luminosite a 40%", "Enter: Helligkeit 40%"));
        } else {
            r.label = TR5("Collegati al Wi-Fi", "Join Wi-Fi", "Conectar Wi-Fi", "Connexion Wi-Fi", "Wi-Fi verbinden"); r.glyph = UG_WIFI;
            sset(r, TR5("Nessuna rete: scegline una", "No network: pick one", "Sin red: elegir una",
                        "Pas de reseau: choisir", "Kein Netz: eines waehlen"));
        }
    } break;
    // -- root: sections with a preview of their value
    case R_S_WIFI:
        r.label = "Wi-Fi"; r.glyph = UG_WIFI;
        if (connected())  { vset(r, nucleo_setup_ssid()); r.col = GOOD;
                            snprintf(r.sub, sizeof r.sub, "%.16s  %d dBm", nucleo_setup_ssid(), nucleo_setup_rssi()); }
        else if (ap_up()) { vset(r, "Hotspot"); r.col = WARN;
                            sset(r, TR5("Solo hotspot attivo", "Hotspot only", "Solo hotspot activo", "Hotspot seul actif", "Nur Hotspot aktiv")); }
        else              { vset(r, TR5("Offline", "Offline", "Offline", "Offline", "Offline")); r.col = MUTED; }
        break;
    case R_S_AP:
        r.label = "Hotspot"; r.glyph = UG_HOTSPOT;
        if (ap_on())        { vset(r, "On"); r.col = WARN; snprintf(r.sub, sizeof r.sub, "%.18s  192.168.4.1", nucleo_setup_ap_ssid()); }
        else if (ap_resc()) { vset(r, TR5("Soccorso", "Rescue", "Rescate", "Secours", "Notfall")); r.col = WARN;
                              sset(r, TR5("Temporaneo, finche torna la rete", "Temporary, until Wi-Fi returns", "Temporal, hasta tener Wi-Fi",
                                          "Temporaire, en attente du Wi-Fi", "Vorlaeufig, bis Wi-Fi zurueck")); }
        else                { vset(r, "Off"); r.col = MUTED; }
        break;
    case R_S_BT: {
        bool pref = bt_pref(), live = nucleo_ble_radio_present();
        r.label = "Bluetooth"; r.glyph = UG_BLUETOOTH;
        vset(r, pref ? "On" : "Off"); r.col = pref ? ACC : MUTED;
        if (pref != live) sset(r, TR5("Cambia al prossimo riavvio", "Changes at the next restart", "Se aplica al reiniciar",
                                      "Change au redemarrage", "Gilt ab dem Neustart"));
    } break;
    case R_S_DISPLAY:
        r.label = TR5("Schermo", "Display", "Pantalla", "Ecran", "Anzeige"); r.glyph = UG_SUN;
        snprintf(r.val, sizeof r.val, "%d%%", nucleo_app_brightness());
        snprintf(r.sub, sizeof r.sub, "%d%%  %s", nucleo_app_brightness(), theme_name());
        break;
    case R_S_SOUND:
        r.label = TR5("Suono", "Sound", "Sonido", "Son", "Ton"); r.glyph = muted ? UG_MUTE : UG_SPEAKER;
        if (muted) { vset(r, TR5("Muto", "Muted", "Mudo", "Muet", "Stumm")); r.col = BAD; }
        else snprintf(r.val, sizeof r.val, "%d%%", nucleo_audio_volume());
        break;
    case R_ANIMA: {
        int m = nucleo_anima_ui_online_mode();
        r.label = "ANIMA"; r.glyph = UG_STAR; r.kind = K_CYCLE; vset(r, anima_mode_name(m)); sset(r, anima_mode_sub(m));
    } break;
    case R_LANG: r.label = TR5("Lingua", "Language", "Idioma", "Langue", "Sprache"); r.glyph = UG_GLOBE; r.kind = K_CYCLE;
                 vset(r, LANGS[lang_idx()].name); break;
    case R_DATETIME: {
        r.label = TR5("Data e ora", "Date/time", "Fecha y hora", "Date/heure", "Datum/Zeit"); r.glyph = UG_CLOCK;
        time_t now = time(NULL); struct tm tmv; localtime_r(&now, &tmv);
        if (now >= DT_EPOCH_2023) {
            strftime(r.val, sizeof r.val, "%H:%M", &tmv);
            strftime(r.sub, sizeof r.sub, "%d/%m/%Y  %H:%M", &tmv);
            if (nucleo_setup_time_synced()) snprintf(r.sub + strlen(r.sub), sizeof r.sub - strlen(r.sub), "  NTP");
        } else { vset(r, TR5("da impostare", "not set", "sin ajustar", "non reglee", "nicht gesetzt")); r.col = WARN; }
    } break;
    case R_S_DEVICE: r.label = TR5("Dispositivo", "Device", "Dispositivo", "Appareil", "Geraet"); r.glyph = UG_CHIP;
                     vset(r, nucleo_setup_device_name()); r.col = MUTED;
                     snprintf(r.sub, sizeof r.sub, "%.14s  PIN %s", nucleo_setup_device_name(), nucleo_auth_pin()); break;
    case R_S_RESET:  r.label = TR5("Ripristino", "Reset", "Restablecer", "Restauration", "Ruecksetzen"); r.glyph = UG_RESET;
                     sset(r, TR5("Azzera impostazioni o tutto", "Reset settings or everything", "Borrar ajustes o todo",
                                 "Effacer reglages ou tout", "Einstellungen oder alles")); break;
    // -- Wi-Fi
    case R_NETS: { r.label = TR5("Reti vicine", "Nearby networks", "Redes cercanas", "Reseaux proches", "Netze in Naehe");
                   int c = nucleo_setup_scan_count();
                   if (c) { snprintf(r.val, sizeof r.val, "%d", c); }
                   sset(r, TR5("Cerca e collegati", "Scan and join", "Buscar y conectar", "Scan et connexion", "Suchen, verbinden")); } break;
    case R_SAVED:   r.label = TR5("Reti salvate", "Saved networks", "Redes guardadas", "Reseaux connus", "Bekannte Netze");
                    snprintf(r.val, sizeof r.val, "%d", nucleo_setup_net_count());
                    sset(r, TR5("Preferite e da dimenticare", "Prefer or forget", "Preferir u olvidar",
                                "Preferer ou oublier", "Bevorzugen, vergessen")); break;
    case R_HANDOFF: r.label = TR5("Passaggio web", "Web handoff", "Traspaso web", "Relais web", "Web-Uebergabe");
                    r.kind = K_TOGGLE; r.on = nucleo_remote_enabled();
                    sset(r, TR5("Il browser prende lo schermo", "The browser takes the screen", "El navegador toma la pantalla",
                                "Le navigateur prend l'ecran", "Browser uebernimmt Anzeige")); break;
    // -- Hotspot
    case R_AP_ON:    r.label = "Hotspot"; r.kind = K_TOGGLE; r.on = ap_on();
                     sset(r, r.on ? TR5("Acceso: 192.168.4.1", "On: 192.168.4.1", "Activo: 192.168.4.1",
                                        "Actif: 192.168.4.1", "An: 192.168.4.1")
                                  : TR5("Spento: usi la tua rete", "Off: using your Wi-Fi", "Apagado: usa tu Wi-Fi",
                                        "Eteint: Wi-Fi habituel", "Aus: eigenes Wi-Fi")); break;
    case R_AP_STATE:
        r.label = TR5("Stato", "Status", "Estado", "Etat", "Status"); r.kind = K_INFO;
        if (!ap_up())       { vset(r, TR5("Spento", "Off", "Apagado", "Eteint", "Aus")); r.col = MUTED; }
        else if (ap_resc()) { vset(r, TR5("Soccorso", "Rescue", "Rescate", "Secours", "Notfall")); r.col = WARN; }
        else                { vset(r, TR5("Attivo", "Active", "Activo", "Actif", "Aktiv")); r.col = GOOD; }
        break;
    case R_AP_SSID: r.label = TR5("Nome", "Name", "Nombre", "Nom", "Name"); r.kind = K_EDIT; vset(r, nucleo_setup_ap_ssid()); break;
    case R_AP_PASS: r.label = "Password"; r.kind = K_EDIT;
                    vset(r, nucleo_setup_ap_secure() ? nucleo_setup_ap_pass() : TR5("(aperta)", "(open)", "(abierta)", "(ouvert)", "(offen)")); break;
    case R_AP_ADDR: r.label = TR5("Indirizzo", "Address", "Direccion", "Adresse", "Adresse"); r.kind = K_INFO; vset(r, "192.168.4.1"); break;
    // -- Bluetooth (the radio's RAM is only reserved when it's on at boot)
    case R_BT_BOOT: r.label = "Bluetooth"; r.kind = K_TOGGLE; r.on = bt_pref();
                    sset(r, r.on ? TR5("Pronto per le app BLE", "Ready for BLE apps", "Listo para apps BLE",
                                       "Pret pour les apps BLE", "Bereit fuer BLE-Apps")
                                 : TR5("Spento: piu RAM per il resto", "Off: more RAM for everything else", "Apagado: mas RAM para el resto",
                                       "Eteint: plus de RAM ailleurs", "Aus: mehr RAM fuer den Rest")); break;
    case R_BT_STATE: r.label = TR5("Adesso", "Right now", "Ahora", "Maintenant", "Jetzt"); r.kind = K_INFO;
                     if (nucleo_ble_radio_present()) { vset(r, TR5("Radio attiva", "Radio on", "Radio activa", "Radio active", "Funk aktiv")); r.col = GOOD; }
                     else                            { vset(r, TR5("Radio spenta", "Radio off", "Radio apagada", "Radio coupee", "Funk aus")); r.col = MUTED; }
                     if (bt_pref() != nucleo_ble_radio_present()) sset(r, TR5("Riavvia per applicare", "Restart to apply", "Reiniciar para aplicar",
                                                                            "Redemarrer pour activer", "Neustart zum Anwenden"));
                     break;
    // -- Display
    case R_BRIGHT: r.label = TR5("Luminosita", "Brightness", "Brillo", "Luminosite", "Helligkeit"); r.kind = K_SLIDER;
                   r.num = (int16_t)nucleo_app_brightness(); r.lo = 10; r.hi = 100; r.col = WARN; break;
    case R_THEME:  r.label = TR5("Tema", "Theme", "Tema", "Theme", "Design"); r.kind = K_CYCLE; vset(r, theme_name()); break;
    case R_SAVER_TIME: r.label = TR5("Salvaschermo", "Screensaver", "Salvapantallas", "Economiseur", "Screensaver"); r.kind = K_CYCLE;
                       fmt_dur(b, sizeof b, s_saver_t_pend >= 0 ? s_saver_t_pend : nucleo_screensaver_timeout_s()); vset(r, b);
                       sset(r, TR5("Dopo inattivita, dal menu", "After idle, from the menu", "Tras inactividad, en menu",
                                   "Apres inactivite, au menu", "Nach Leerlauf, im Menue")); break;
    case R_SAVER_STYLE: r.label = TR5("Stile", "Style", "Estilo", "Style", "Stil"); r.kind = K_CYCLE;
                        vset(r, saver_style_name(s_saver_m_pend >= 0 ? s_saver_m_pend : nucleo_screensaver_mode())); break;
    // -- Sound
    case R_VOLUME: r.label = "Volume"; r.kind = K_SLIDER; r.num = (int16_t)nucleo_audio_volume(); r.lo = 0; r.hi = 100; r.col = muted ? DIM : GOOD; break;
    case R_MUTE:   r.label = TR5("Muto", "Mute", "Mudo", "Muet", "Stumm"); r.kind = K_TOGGLE; r.on = muted;
                   sset(r, muted ? TR5("Nessun suono", "No sound", "Sin sonido", "Aucun son", "Kein Ton")
                                 : TR5("Audio attivo", "Sound on", "Sonido activo", "Son actif", "Ton an")); break;
    case R_TTS:    r.label = TR5("Lettura vocale", "Read aloud", "Lectura por voz", "Lecture vocale", "Vorlesen"); r.kind = K_TOGGLE;
                   r.on = nucleo_tts_enabled(); r.dis = !tts_ok();
                   sset(r, r.dis ? TR5("Pacchetto voce assente su SD", "Voice pack missing on SD", "Falta el paquete de voz en SD",
                                       "Pack voix absent de la SD", "Sprachpaket fehlt auf SD")
                                 : TR5("Le risposte vengono lette", "Answers are spoken", "Las respuestas se leen",
                                       "Les reponses sont lues", "Antworten werden vorgelesen")); break;
    case R_TTS_SPEED: r.label = TR5("Velocita voce", "Voice speed", "Ritmo de voz", "Debit de voix", "Sprechtempo"); r.kind = K_SLIDER;
                      r.num = (int16_t)(s_tts_pend >= 0 ? s_tts_pend : nucleo_tts_speed()); r.lo = 70; r.hi = 160;
                      r.col = ACC; r.dis = !tts_ok(); break;
    case R_VOICE:  r.label = TR5("Ascolto sempre", "Always listen", "Escuchar siempre", "Ecoute continue", "Immer zuhoeren"); r.kind = K_TOGGLE;
                   r.on = nucleo_voice_always_on();
                   sset(r, TR5("Parola chiave pronta (16 KB RAM)", "Wake word ready (16 KB RAM)", "Palabra clave lista (16 KB RAM)",
                               "Mot-cle pret (16 KB RAM)", "Weckwort bereit (16 KB RAM)")); break;
    // -- Device
    case R_NAME: r.label = TR5("Nome", "Name", "Nombre", "Nom", "Name"); r.kind = K_EDIT; vset(r, nucleo_setup_device_name()); break;
    case R_PIN: { const char *p = nucleo_auth_pin(); r.label = "PIN"; r.kind = K_INFO; vset(r, p[0] ? p : "------"); r.col = GOOD; r.vsz = 2;
                  sset(r, TR5("Per collegare un browser", "To pair a browser", "Vincular navegador", "Lier un navigateur", "Browser koppeln")); } break;
    case R_SESSIONS: { int n = nucleo_auth_session_count(); r.label = TR5("Sessioni web", "Web sessions", "Sesiones web", "Sessions web", "Web-Sitzungen");
                       r.kind = n > 0 ? K_DANGER : K_INFO; snprintf(r.val, sizeof r.val, "%d", n);
                       if (n > 0) { sset(r, TR5("Invio: disconnetti tutti", "Enter: sign everyone out", "Enter: cerrar todas",
                                                "Enter: tout deconnecter", "Enter: alle abmelden")); }
                       r.col = n ? WARN : MUTED; } break;
    case R_MODEL:   r.label = TR5("Modello", "Model", "Modelo", "Modele", "Modell"); r.kind = K_INFO;
                    vset(r, nucleo_ui_is_adv() ? "Cardputer ADV" : "Cardputer"); r.col = FG; break;
    case R_VERSION: r.label = TR5("Versione", "Version", "Version", "Version", "Version"); r.kind = K_INFO;
                    vset(r, esp_app_get_description()->version); r.col = FG; break;
    case R_BATTERY: {
        int p = nucleo_power_battery_pct(), mv = nucleo_power_battery_mv();
        r.label = TR5("Batteria", "Battery", "Bateria", "Batterie", "Akku"); r.kind = K_INFO;
        if (p < 0) { vset(r, TR5("n/d", "n/a", "n/d", "n/d", "k.A.")); r.col = MUTED; }
        else { snprintf(r.val, sizeof r.val, "%d%%", p); r.col = p < 20 ? BAD : p < 50 ? WARN : GOOD;
               if (mv > 0) snprintf(r.sub, sizeof r.sub, "%d%%  %d.%02d V", p, mv / 1000, (mv % 1000) / 10); }
    } break;
    case R_SD: {
        r.label = "SD"; r.kind = K_INFO; r.col = FG;
        const nucleo_storage_info_t *st = nucleo_storage_info();
        if (st && st->mounted) {
            uint32_t f = (uint32_t)(st->free_bytes / (1024 * 1024)), t = (uint32_t)(st->total_bytes / (1024 * 1024));
            if (t >= 1000) snprintf(r.val, sizeof r.val, TR5("%.1f GB liberi", "%.1f GB free", "%.1f GB libres",
                                                             "%.1f GB libres", "%.1f GB frei"), f / 1024.0f);
            else           snprintf(r.val, sizeof r.val, TR5("%u MB liberi", "%u MB free", "%u MB libres",
                                                             "%u MB libres", "%u MB frei"), (unsigned)f);
            if (t >= 1000) snprintf(r.sub, sizeof r.sub, TR5("%.1f di %.1f GB liberi", "%.1f of %.1f GB free", "%.1f de %.1f GB libres",
                                                             "%.1f sur %.1f GB libres", "%.1f von %.1f GB frei"), f / 1024.0f, t / 1024.0f);
        } else { vset(r, TR5("assente", "none", "ninguna", "absente", "fehlt")); r.col = BAD; }
    } break;
    case R_RAM:
        r.label = TR5("RAM libera", "Free RAM", "RAM libre", "RAM libre", "Freier RAM"); r.kind = K_INFO; r.col = FG;
        snprintf(r.val, sizeof r.val, "%u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024));
        snprintf(r.sub, sizeof r.sub, TR5("%u KB, blocco max %u KB", "%u KB, largest block %u KB", "%u KB, bloque max %u KB",
                                          "%u KB, bloc max %u KB", "%u KB, max. Block %u KB"),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) / 1024));
        break;
    case R_UPTIME: {
        int64_t s = esp_timer_get_time() / 1000000; r.label = TR5("Acceso da", "Uptime", "Encendido", "En marche", "Laufzeit"); r.kind = K_INFO; r.col = FG;
        if (s < 3600) snprintf(r.val, sizeof r.val, "%d min", (int)(s / 60));
        else          snprintf(r.val, sizeof r.val, "%dh %02dm", (int)(s / 3600), (int)(s % 3600 / 60));
    } break;
    case R_UPDATES: r.label = TR5("Aggiornamenti", "Updates", "Actualizaciones", "Mises a jour", "Updates"); r.glyph = UG_UPDATE;
                    sset(r, TR5("Cerca una nuova versione", "Check for a new version", "Buscar una nueva version",
                                "Chercher une mise a jour", "Nach neuer Version suchen")); break;
    case R_RESTART: r.label = TR5("Riavvia", "Restart", "Reiniciar", "Relancer", "Neustart"); r.kind = K_DANGER;
                    sset(r, TR5("Chiude tutto e riparte", "Closes everything and restarts", "Cierra todo y reinicia",
                                "Ferme tout et redemarre", "Schliesst alles, startet neu")); break;
    case R_LAUNCHER: {
        r.label = TR5("Torna a M5Launcher", "Back to M5Launcher", "Volver a M5Launcher", "Retour a M5Launcher", "Zurueck zu M5Launcher");
        r.kind = K_DANGER;
        guest_return_t m = nucleo_guest_return_mode();
        if (m == GUEST_RET_DEEP_SLEEP)
            sset(r, TR5("Poi INVIO sulla sua schermata", "Then ENTER on its splash", "Luego ENTER en su pantalla",
                        "Puis ENTREE sur son ecran", "Dann ENTER im Startbild"));
        else if (m == GUEST_RET_POWER_CYCLE)
            sset(r, TR5("Spegni e riaccendi", "Switch off and on", "Apaga y enciende", "Eteindre et rallumer", "Aus- und einschalten"));
        else
            sset(r, TR5("Tieni il suo tasto e riavvia", "Hold its key and restart", "Manten su tecla y reinicia",
                        "Maintenir sa touche, relancer", "Seine Taste halten, Neustart"));
    } break;
    // -- Reset (armed rows show how many ENTER presses are left)
    case R_RST_SOFT: case R_RST_HARD: case R_RST_FMT: case R_RST_SD: {
        // Four rows, each named for exactly what it erases: config only · factory reset (internal stores +
        // the SD's state folders) · the SD card only · both ("from scratch": a blank device + the wizard).
        const bool card = id == R_RST_FMT || id == R_RST_SD;
        r.label = (id == R_RST_SOFT) ? TR5("Azzera config", "Reset settings", "Borrar ajustes", "Effacer reglages", "Konfig loeschen")
                : (id == R_RST_HARD) ? TR5("Reset totale", "Factory reset", "Borrado total", "Tout effacer", "Werksreset")
                : (id == R_RST_FMT)  ? TR5("Formatta SD", "Erase SD card", "Formatear SD", "Formater la SD", "SD formatieren")
                                     : TR5("Tutto da zero", "Wipe everything", "Borrar todo", "Tout a zero", "Alles loeschen");
        r.kind = K_DANGER;
        if (card) r.dis = !nucleo_storage_info()->mounted;
        if (s_rst_id == id) { snprintf(r.val, sizeof r.val, "x%d", s_rst_left + 1);
                              // A card shared with M5Launcher loses the Launcher's files too: said where the user confirms.
                              snprintf(r.sub, sizeof r.sub, card && nucleo_guest_hosted()
                                           ? TR5("Anche M5Launcher: invio ancora %d", "M5Launcher too: ENTER %d more", "M5Launcher incl.: ENTER %d mas",
                                                 "M5Launcher aussi: ENTREE %d fois", "Auch M5Launcher: ENTER %dx")
                                           : TR5("Invio ancora %d volte", "ENTER %d more times", "ENTER %d veces mas",
                                                 "ENTER encore %d fois", "ENTER noch %d-mal"), s_rst_left); }
        else if (card && r.dis) sset(r, TR5("Nessuna scheda SD", "No SD card", "Sin tarjeta SD", "Pas de carte SD", "Keine SD-Karte"));
        else if (id == R_RST_FMT) sset(r, TR5("Solo la SD. Wi-Fi e PIN restano", "SD only. Wi-Fi and PIN kept", "Solo la SD. Wi-Fi y PIN quedan",
                                             "SD seule. Wi-Fi et PIN gardes", "Nur SD. WLAN und PIN bleiben"));
        else if (id == R_RST_SD)  sset(r, TR5("Reset totale + formatta SD", "Factory reset + erase SD", "Borrado total + formatear SD",
                                             "Tout effacer + formater SD", "Werksreset + SD formatieren"));
        else sset(r, id == R_RST_SOFT ? TR5("Rete, preferenze, log. File salvi", "Network, prefs, logs. Files kept", "Red, ajustes, logs (no archivos)",
                                            "Reseau, prefs, logs (sauf fichiers)", "Netz, Setup, Logs. Dateien bleiben")
                                      : TR5("Anche chiavi e dati ANIMA", "Also keys and ANIMA data", "Tambien claves, datos ANIMA",
                                            "Aussi cles et donnees ANIMA", "Auch Keys und ANIMA-Daten"));
    } break;
    // -- Saved networks
    case R_SAVED_NET: {
        const char *ss = nucleo_setup_net_ssid(num);
        r.kind = K_INFO; app_ui_ascii_fold(ss, r.val, sizeof r.val); r.label = r.val;   // the SSID IS the label
        r.on = nucleo_setup_net_priority(num) > 0;
        bool cur = connected() && !strcmp(ss, nucleo_setup_ssid());
        r.col = cur ? GOOD : FG;
        sset(r, cur ? (r.on ? TR5("In uso, preferita", "In use, preferred", "En uso, preferida", "Utilise, prefere", "Aktiv, bevorzugt")
                            : TR5("In uso", "In use", "En uso", "Utilise", "Aktiv"))
                    : (r.on ? TR5("Preferita: si collega per prima", "Preferred: joined first", "Preferida: se une primero",
                                  "Prefere: rejoint en premier", "Bevorzugt: zuerst verbunden")
                            : TR5("Salvata", "Saved", "Guardada", "Connu", "Bekannt")));
    } break;
    case R_FORGET_ALL: r.label = TR5("Dimentica tutte", "Forget all", "Olvidar todas", "Tout oublier", "Alle vergessen"); r.kind = K_DANGER;
                       snprintf(r.sub, sizeof r.sub, TR5("%d reti e password", "%d networks and passwords", "%d redes y claves",
                                                         "%d reseaux et mots de passe", "%d Netze und Passwoerter"), nucleo_setup_net_count()); break;
    }
}

// Row `i` of page `pg`.
static void make_row(int pg, int i, Row &r)
{
    if (pg == PG_SAVED) { int k = nucleo_setup_net_count(); make_row_id(i < k ? R_SAVED_NET : R_FORGET_ALL, i, r); return; }
    if (pg == PG_SEARCH) {
        if (i < 0 || i >= s_hits) { memset(&r, 0, sizeof r); r.label = ""; return; }
        make_row_id(s_hit_id[i], 0, r);
        // A sub-page row has no glyph of its own (0 would draw the Wi-Fi fan): wear its section's.
        static const uint8_t PAGE_GLYPH[PG_N] = { 0, UG_WIFI, UG_HOTSPOT, UG_BLUETOOTH, UG_SUN, UG_SPEAKER, UG_CHIP, UG_RESET, UG_WIFI, UG_WIFI, 0 };
        if (!r.glyph && s_hit_pg[i] != PG_ROOT) r.glyph = PAGE_GLYPH[s_hit_pg[i]];
        // show where it lives: "Sound  40%"
        char loc[40]; snprintf(loc, sizeof loc, "%s  %s", page_title(s_hit_pg[i]), r.sub[0] ? r.sub : r.val);
        sset(r, loc);
        return;
    }
    if (pg == PG_ROOT && suggestion() != SG_NONE) { if (i == 0) { make_row_id(R_SUGGEST, 0, r); return; } i--; }
    int n; const uint8_t *ids = page_rows(pg, &n);
    if (!ids || i < 0 || i >= n) { memset(&r, 0, sizeof r); r.label = ""; return; }
    make_row_id(ids[i], i, r);
}

static int  cur_sel(void)  { int n = page_count(s_page); int s = s_sel[s_page]; if (n <= 0) return 0; return s < 0 ? 0 : s >= n ? n - 1 : s; }
static bool focused_row(Row &r)
{
    if (s_page == PG_NETS || page_count(s_page) <= 0) return false;
    make_row(s_page, cur_sel(), r); return true;
}
static int root_index_of(uint8_t id)
{
    int off = suggestion() != SG_NONE ? 1 : 0;
    for (int i = 0; i < NROWS(ROWS_ROOT); i++) if (ROWS_ROOT[i] == id) return i + off;
    return 0;
}

// ---- search (type on the root) ---------------------------------------------------------------------
static bool ci_contains(const char *hay, const char *needle)
{
    if (!hay || !needle || !needle[0]) return false;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
        if (!*b) return true;
    }
    return false;
}
// Every searchable row matching q (its label, or its page title), in page order. Fills up to `cap`
// results when pg_out/id_out are given; returns the TOTAL match count either way (for "+N").
static int search_scan(const char *q, uint8_t *pg_out, uint8_t *id_out, int cap)
{
    int hits = 0;
    static const uint8_t PAGES[] = { PG_ROOT, PG_WIFI, PG_AP, PG_BT, PG_DISPLAY, PG_SOUND, PG_DEVICE, PG_RESET };
    for (int p = 0; p < NROWS(PAGES); p++) {
        int pg = PAGES[p], n; const uint8_t *ids = page_rows(pg, &n);
        for (int i = 0; i < n; i++) {
            if (pg == PG_ROOT && section_of(ids[i]) < 0 && ids[i] != R_ANIMA && ids[i] != R_LANG && ids[i] != R_DATETIME) continue;
            Row r; make_row_id(ids[i], i, r);
            if (ci_contains(r.label, q) || (pg != PG_ROOT && ci_contains(page_title(pg), q))) {
                if (pg_out && hits < cap) { pg_out[hits] = (uint8_t)pg; id_out[hits] = ids[i]; }
                hits++;
            }
        }
    }
    return hits;
}
static int s_hit_total = 0;                    // every match, even past the 18 listed (header shows "18+")
static void search_run(void)
{
    s_hit_total = search_scan(s_q, s_hit_pg, s_hit_id, NROWS(s_hit_pg));
    s_hits = s_hit_total < NROWS(s_hit_pg) ? s_hit_total : NROWS(s_hit_pg);
}

// Launcher Spotlight -> Settings. count(): how many settings a query finds (0 = don't offer the tile).
// preset(): the query the NEXT open of Settings starts on, as a search. One-shot, 16 chars, no heap.
static char s_preset[17];
extern "C" int nucleo_settings_search_count(const char *q)
{
    return (q && q[0]) ? search_scan(q, nullptr, nullptr, 0) : 0;
}
extern "C" void nucleo_settings_search_preset(const char *q)
{
    snprintf(s_preset, sizeof s_preset, "%s", q ? q : "");
}

// First boot: the wizard did the language; the NETWORK step is this app (Nearby networks — scan, signal,
// password editor, join). onboard(): the next open starts there. A join ends it on the Wi-Fi card; Esc
// ends it on the Hotspot page, which shows the hotspot name + password the phone needs.
// First-boot network step, ON RAILS: it opens on Nearby networks and only two exits exist — a join, or Esc
// confirmed as "use the hotspot" — and both land on the "All set" page (address + PIN), whose ENTER opens the
// launcher. Tab/LEFT/other pages are locked meanwhile; if the app is closed anyway (it never should be), the
// launcher re-opens this step until it is finished (nucleo_app.cpp), so the wizard always reaches its end.
enum { OB_NONE = 0, OB_NETS, OB_DONE };
static bool    s_onboard_req = false;
static uint8_t s_onboard = OB_NONE;
static bool    s_done_sta = false;                 // the "All set" page: joined a network (else: hotspot)
static bool    s_ob_finish_pend = false;           // skip confirmed while a scan still owns the radio: finish after it
extern "C" void nucleo_settings_onboard(void) { s_onboard_req = true; }
static void onboard_end(void);

// ---- drawing ------------------------------------------------------------------------------------
// Two paths (docs/ANTI-FLICKER.md, technique 2 - "an app that can lose the canvas"):
//  - BUFFERED: the framework hands us the shared canvas, wiped: draw the whole frame, one blit.
//  - DIRECT: no canvas (the usual state on the ADV once Wi-Fi is up): we paint straight onto the
//    panel, so every paint is INCREMENTAL. The screen is described by signatures (header title, Wi-Fi
//    card, one per visible list slot, scrollbar, date boxes, overlays) and only boxes whose signature
//    changed are repainted, each in its own box; value fields are fixed-width and drawn opaque. A full
//    paint happens only when the SCENE changes (page, an editor/confirm/date overlay opening or
//    closing, theme, language) or when the app regains the screen (nucleo_app_repaint_gen()).
struct Slot { int16_t y, h; uint32_t sig; };
static Slot     s_slot[10];                    // visible list rows as painted
static int      s_nslot = 0, s_list_bot = 0;   // ... and the bottom edge of the painted list
static uint32_t s_scene = 0, s_hdr_sig = 0, s_card_sig = 0, s_sbar_sig = 0, s_ovl_sig = 0, s_toast_sig = 0;
static uint32_t s_dtbox[5];
static unsigned s_rgen = ~0u;
static bool     s_full = true;                 // this on_draw is a full paint
static bool     s_toast_on = false;            // a toast is on the panel ...
static int      s_toast_y = 0;                 // ... at this y
static int      s_rep_lo = 9999, s_rep_hi = -1;   // y-range repainted this frame (overlays redraw over it)

static uint32_t fnv(uint32_t h, uint32_t v)        { return (h ^ v) * 16777619u; }
static uint32_t fnv_s(uint32_t h, const char *str) { if (str) for (; *str; str++) h = fnv(h, (uint8_t)*str); return fnv(h, 0xFFu); }
static void mark(int y0, int y1)    { if (y0 < s_rep_lo) s_rep_lo = y0; if (y1 > s_rep_hi) s_rep_hi = y1; }
static bool touched(int y0, int y1) { return s_full || (s_rep_lo < y1 && s_rep_hi > y0); }

// Header: back chevron + title (repainted only when it changes) + a fixed 5-column right field drawn
// opaque, so a moving "3/11" or the clock never wipes the band.
static void draw_header(const char *title, const char *right, bool back)
{
    uint32_t ts = fnv_s(back ? 1u : 2u, title);
    if (s_full || ts != s_hdr_sig) {
        if (!s_full) d.fillRect(0, 0, W - 36, HDR - 1, BG);
        int x = 6;
        if (back) { ui_glyph(&d, UG_PREV, 7, 8, 6, ACC, BG); x = 15; }
        txt_fit(x, 1, title, W - x - 38, ACC, BG, 2);
        d.drawFastHLine(0, HDR - 1, W, LINE);
        s_hdr_sig = ts; mark(0, HDR);
    }
    char rb[8]; snprintf(rb, sizeof rb, "%5.5s", right ? right : "");
    txt(W - 4 - 30, 5, rb, MUTED, BG, 1);
}

// Search header: magnifier + the query as a fixed 14-column opaque field (+ the hit count).
static void draw_search_header(void)
{
    if (s_full || s_hdr_sig != 0x5EA2C4u) {
        if (!s_full) d.fillRect(0, 0, W, HDR - 1, BG);
        ui_glyph(&d, UG_SEARCH, 9, 8, 6, ACC, BG);
        d.drawFastHLine(0, HDR - 1, W, LINE);
        s_hdr_sig = 0x5EA2C4u; mark(0, HDR);
    }
    char q[24]; snprintf(q, sizeof q, "%s_", s_q);
    int l = (int)strlen(q); const char *tail = l > 14 ? q + l - 14 : q;
    char qb[16]; snprintf(qb, sizeof qb, "%-14.14s", tail);
    txt(20, 1, qb, FG, BG, 2);
    char cnt[8], rb[8]; snprintf(cnt, sizeof cnt, s_hit_total > s_hits ? "%d+" : "%d", s_hits); snprintf(rb, sizeof rb, "%5.5s", cnt);
    txt(W - 4 - 30, 5, rb, MUTED, BG, 1);
}

// Atomic box repaint on the direct path. A changed box is rendered into a small 240x12 16-bpp strip
// sprite (~5.6 KB, allocated on the first direct paint of the visit, freed on leave) one strip at a
// time and each strip is pushed in ONE transfer, clipped to the box: the panel goes from the old box
// straight to the new one, never through a cleared box. `fn` draws the whole box with its top at the
// y it is given (x is absolute), so it is simply called once per strip, shifted. Same colours as the
// direct draw (16 bpp). If the strip can't be allocated: clear + draw (technique 2 fallback).
typedef void (*box_fn)(int y, const void *ctx);
static M5Canvas *s_strip = nullptr;
static bool      s_strip_tried = false;
static int       s_strip_h = 12;             // tallest strip the heap allowed (a whole chip = one push)
static void strip_release(void)
{
    if (s_strip) { s_strip->deleteSprite(); delete s_strip; s_strip = nullptr; }
    s_strip_tried = false;
}
static void paint_box(int x, int w, int y, int h, box_fn fn, const void *ctx)
{
    if (s_full) { fn(y, ctx); return; }                              // screen already cleared
    if (!s_strip && !s_strip_tried) {
        s_strip_tried = true;
        // Tallest strip that fits with a margin: a whole chip (34 rows) or a compact row (19) in ONE
        // push makes a list repaint near-instant; 12 rows is the floor. Never try what can't fit.
        static const int HS[] = { ROW_F, ROW_H, 12 };
        size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        for (int i = 0; i < NROWS(HS) && !s_strip; i++) {
            if ((size_t)(W * HS[i] * 2 + 1024) > big) continue;
            s_strip = new M5Canvas();
            s_strip->setColorDepth(16);
            if (s_strip->createSprite(W, HS[i])) s_strip_h = HS[i];
            else { delete s_strip; s_strip = nullptr; }
        }
    }
    int32_t cx, cy, cw, chh; d.getClipRect(&cx, &cy, &cw, &chh);    // the caller's band clip
    if (!s_strip) { d.fillRect(x, y, w, h, BG); fn(y, ctx); return; }
    LovyanGFX *panel = nucleo_app_gfx();
    for (int off = 0; off < h; off += s_strip_h) {
        int sy = y + off, sh = (h - off < s_strip_h) ? h - off : s_strip_h;
        int y0 = sy > cy ? sy : (int)cy, y1 = (sy + sh < cy + chh) ? sy + sh : (int)(cy + chh);
        if (y1 <= y0) continue;
        s_strip->fillSprite(BG);
        nucleo_app_set_gfx(s_strip);
        fn(y - sy, ctx);                                             // this strip lands at the sprite's row 0
        nucleo_app_set_gfx(nullptr);
        panel->setClipRect(x, y0, w, y1 - y0);
        s_strip->pushSprite(panel, 0, sy);
        // pushSprite is ASYNCHRONOUS here: M5GFX DMAs straight out of a sprite in internal RAM (always, on
        // this PSRAM-less chip) and returns while the strip is still on the wire. The next iteration — or
        // the next box — refills this same strip, so without the wait the panel received a mix of two
        // strips: half-drawn rows and a focus chip whose label vanished (ADV, 2026-10-01). The host
        // renderer has no DMA, which is why ui:shots never showed it.
        panel->waitDMA();
    }
    panel->setClipRect(cx, cy, cw, chh);
}

// Direct-path list slots: slot k at (y,h) with signature sg must be repainted only when it differs from
// what is on the panel. Returns true when the caller has to paint it (via paint_box).
static bool slot_begin(int k, int y, int h, uint32_t sg, Slot *next)
{
    next[k].y = (int16_t)y; next[k].h = (int16_t)h; next[k].sig = sg;
    if (!s_full && k < s_nslot && s_slot[k].y == y && s_slot[k].h == h && s_slot[k].sig == sg) return false;
    mark(y, y + h);
    return true;
}
static void slots_end(Slot *next, int k, int bot)
{
    if (!s_full && bot < s_list_bot) { d.fillRect(0, bot, W - 3, s_list_bot - bot, BG); mark(bot, s_list_bot); }
    memcpy(s_slot, next, sizeof(Slot) * k); s_nslot = k; s_list_bot = bot;
}
static uint32_t row_sig(const Row &r, bool foc, bool icon)
{
    uint32_t h = fnv_s(2166136261u, r.label); h = fnv_s(h, r.val); h = fnv_s(h, r.sub);
    h = fnv(h, (uint32_t)r.kind | (uint32_t)r.glyph << 8 | (uint32_t)r.vsz << 16 | (r.on ? 1u << 24 : 0) |
               (r.dis ? 1u << 25 : 0) | (foc ? 1u << 26 : 0) | (icon ? 1u << 27 : 0));
    return fnv(fnv(h, (uint16_t)r.num), r.col);
}

// Switch (on = green track, knob right). `onchip` = drawn inside the focused accent chip.
static void draw_switch(int rx, int cy, bool on, bool onchip)
{
    const int sw = 26, sh = 13;
    int x = rx - sw;
    d.fillRoundRect(x, cy - sh / 2, sw, sh, sh / 2, on ? GOOD : (onchip ? BG : LINE));
    d.fillCircle(on ? x + sw - sh / 2 - 1 : x + sh / 2 + 1, cy, sh / 2 - 2, on ? INK : (onchip ? FG : nucleo_theme_ink_on(LINE, MUTED)));   // themes may alias LINE==MUTED
}

static int slider_pct(const Row &r) { int span = r.hi - r.lo; if (span <= 0) return 0; int p = (r.num - r.lo) * 100 / span; return p < 0 ? 0 : p > 100 ? 100 : p; }

// One list row. Compact (19 px): name + value (Font2, full colour) or its switch. Focused (34 px): an
// expanded accent chip — the name, then a readable second line with the live control or what it does.
static void draw_row(int y, const Row &r, bool foc, bool icon)
{
    const int rx = W - 9;
    int lx = icon ? 26 : 8;
    bool danger = (r.kind == K_DANGER);
    if (!foc) {
        const int cy = y + ROW_H / 2;
        if (icon) ui_glyph(&d, r.glyph, 14, cy, 6, r.dis ? DIM : (r.id == R_SUGGEST ? WARN : ACC), BG);
        int vx = rx;
        switch (r.kind) {
        case K_TOGGLE: draw_switch(rx, cy, r.on && !r.dis, false); vx = rx - 30; break;
        case K_SLIDER: { char b[8]; snprintf(b, sizeof b, "%d%%", r.num); vx = rx - f2(rx, y + 2, b, 60, r.dis ? DIM : r.col, BG, true) - 6; } break;
        default:
            if (r.id == R_SAVED_NET) { if (r.on) { ui_glyph(&d, UG_STAR, rx - 5, cy, 5, GOOD, BG); vx = rx - 14; } break; }
            if (r.val[0]) {
                int room = rx - (lx + 64);                                   // the name keeps >= ~5 glyphs
                if (r.vsz == 2 && (int)strlen(r.val) * 12 <= room) { int w = (int)strlen(r.val) * 12; txt(rx - w, y + 2, r.val, r.col, BG, 2); vx = rx - w - 6; }
                else vx = rx - f2(rx, y + 2, r.val, room, r.dis ? DIM : r.col, BG, true) - 6;
            }
            break;
        }
        uint16_t lc = r.dis ? DIM : danger ? BAD : r.id == R_SUGGEST ? WARN : r.id == R_SAVED_NET ? r.col : FG;
        txt_fit(lx, y + 2, r.label, vx - lx, lc, BG, 2);
        return;
    }

    // ---- focused: expanded chip -----------------------------------------------------------------
    uint16_t pill = r.dis ? LINE : danger ? BAD : ACC, ink = r.dis ? nucleo_theme_ink_on(LINE, MUTED) : INK;
    d.fillRoundRect(3, y, W - 8, ROW_F - 1, 9, pill);
    if (icon) ui_glyph(&d, r.glyph, 14, y + 9, 6, ink, pill);
    int top_vx = rx;                                                        // right edge left for the name
    if (r.kind == K_TOGGLE) { draw_switch(rx, y + 9, r.on && !r.dis, true); top_vx = rx - 30; }
    else if (r.kind == K_NAV || (danger && !is_rst(r.id))) { ui_glyph(&d, UG_NEXT, rx - 2, y + 9, 5, ink, pill); top_vx = rx - 12; }
    else if (r.kind == K_DANGER && r.val[0]) { top_vx = rx - f2(rx, y + 1, r.val, 40, ink, pill, true) - 6; }
    txt_fit(lx, y + 2, r.label, top_vx - lx, ink, pill, 2);

    const int ly = y + 17;                                                  // second line (Font2, 16 px)
    int x2 = icon ? lx : 12, w2 = rx - x2;
    switch (r.kind) {
    case K_SLIDER: {
        char b[8]; snprintf(b, sizeof b, "%d%%", r.num);
        int vw = f2(rx, ly, b, 50, ink, pill, true);
        int bx = x2, bw = rx - vw - 10 - bx, by = ly + 5, bh = 6;
        d.fillRoundRect(bx, by, bw, bh, 3, BG);
        int fw = slider_pct(r) * bw / 100;
        if (fw > 0) d.fillRoundRect(bx, by, fw, bh, 3, r.dis ? DIM : FG);
        int kx = bx + fw; if (kx < bx + 4) kx = bx + 4; if (kx > bx + bw - 4) kx = bx + bw - 4;
        d.fillCircle(kx, by + bh / 2, 5, r.dis ? DIM : FG);
    } break;
    case K_CYCLE: {                                                          // < value >
        ui_glyph(&d, UG_PREV, x2 + 3, ly + 8, 5, ink, pill);
        int w = f2(x2 + 12, ly, r.val, w2 - 30, ink, pill, false);
        ui_glyph(&d, UG_NEXT, x2 + 12 + w + 8, ly + 8, 5, ink, pill);
        if (r.sub[0] && x2 + 12 + w + 20 < rx - 40) f2(rx, ly, r.sub, rx - (x2 + 12 + w + 20), ink, pill, true);
    } break;
    default:
        if (r.vsz == 2 && r.val[0] && !r.sub[0]) txt(x2, ly, r.val, ink, pill, 2);
        else if (r.kind == K_INFO && r.vsz == 2) { txt(x2, ly, r.val, ink, pill, 2); if (r.sub[0]) f2(rx, ly, r.sub, rx - x2 - (int)strlen(r.val) * 12 - 10, ink, pill, true); }
        else f2(x2, ly, r.sub[0] ? r.sub : r.val, w2, ink, pill, false);
        break;
    }
}

// Pixel layout of a variable-height list: keeps the focused chip in view with a row of context above.
// The chip keeps a row of context around it. When it would leave the view the list JUMPS instead of
// creeping one row per key: going down the chip lands near the top, going up near the bottom, so the
// next few moves only move the chip. On the direct path a scroll step repaints every visible row while
// a chip move repaints two — creeping made each key a full-list repaint that smeared on the slow LCD.
static int list_layout(int n, int sel, int band, int *total)
{
    int t = n > 0 ? (n - 1) * ROW_H + ROW_F : 0, st = sel * ROW_H;
    if (total) *total = t;
    int want_top = st - (sel > 0 ? ROW_H : 0), want_bot = st + ROW_F + (sel < n - 1 ? ROW_H / 2 : 0);
    if (want_bot > s_scroll + band)  s_scroll = want_top;             // off the bottom: chip to the top
    else if (want_top < s_scroll)    s_scroll = want_bot - band;      // off the top: chip to the bottom
    if (s_scroll > t - band) s_scroll = t - band;
    if (s_scroll < 0) s_scroll = 0;
    return s_scroll;
}
static void draw_scrollbar(int top, int band, int scroll, int total)
{
    uint32_t sg = fnv(fnv(fnv(fnv(7u, top), band), scroll), total);
    if (!s_full && sg == s_sbar_sig) return;                          // nothing else paints this column
    s_sbar_sig = sg;
    if (total <= band || band <= 8) { if (!s_full) d.fillRect(W - 3, top, 2, band, BG); return; }
    int kh = band * band / total; if (kh < 8) kh = 8;
    int ky = top + (band - kh) * scroll / (total - band);
    if (ky > top)                d.fillRect(W - 3, top, 2, ky - top, LINE);   // track / knob / track:
    d.fillRect(W - 3, ky, 2, kh, ACC);                                         // each pixel painted once
    if (ky + kh < top + band)    d.fillRect(W - 3, ky + kh, 2, top + band - ky - kh, LINE);
}

struct RowBox { const Row *r; bool foc, icon; };
static void row_box(int y, const void *ctx) { const RowBox *b = (const RowBox *)ctx; draw_row(y, *b->r, b->foc, b->icon); }

static int draw_wifi_card(int top);
static void draw_page(int ch)
{
    int n = page_count(s_page), sel = cur_sel();
    int top = HDR + 1;
    char pos[18] = "";
    if (s_page == PG_SEARCH) draw_search_header();
    else {
        if (s_page == PG_WIFI) top = draw_wifi_card(top);
        if (n > 5) snprintf(pos, sizeof pos, "%d/%d", sel + 1, n);
        else if (s_page == PG_ROOT) {
            time_t now = time(NULL); struct tm tmv; localtime_r(&now, &tmv);
            if (now >= DT_EPOCH_2023) snprintf(pos, sizeof pos, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
        }
        draw_header(page_title(s_page), pos, s_page != PG_ROOT);
    }
    int band = ch - 1 - top;
    if (n <= 0) {                                                    // "empty" is part of the scene: full paints only
        if (!s_full) return;
        if (s_page == PG_SEARCH) {
            txt(10, top + 14, TR5("Nessuna impostazione con", "No setting matches", "Ningun ajuste coincide con",
                                  "Aucun reglage pour", "Keine Einstellung passt zu"), MUTED, BG, 1);
            char q[26]; snprintf(q, sizeof q, "\"%.20s\"", s_q); f2(10, top + 28, q, W - 20, FG, BG, false);
            txt(10, top + 52, TR5("canc cancella   esc chiude", "del erase   esc closes", "del borrar   esc cerrar",
                                  "del effacer   esc fermer", "del loeschen   esc zurueck"), DIM, BG, 1);
        } else {                                                     // only PG_SAVED can be empty
            txt(10, top + 12, TR5("Nessuna rete salvata.", "No saved networks.", "No hay redes guardadas.",
                                  "Aucun reseau connu.", "Keine bekannten Netze."), FG, BG, 1);
            txt(10, top + 26, TR5("Unisciti a una da Reti vicine:", "Join one from Nearby networks:", "Conectar una en Redes cercanas:",
                                  "Rejoindre via Reseaux proches:", "Via Netze in Naehe verbinden:"), MUTED, BG, 1);
            txt(10, top + 38, TR5("la password resta salvata qui.", "its password is kept here.", "su clave queda guardada aqui.",
                                  "son mot de passe reste ici.", "das Passwort bleibt hier."), MUTED, BG, 1);
        }
        return;
    }
    int total, sc = list_layout(n, sel, band, &total);
    bool icon = (s_page == PG_ROOT || s_page == PG_SEARCH);
    Slot next[10]; int k = 0;
    d.setClipRect(0, top, W, band);
    int y = top - sc;
    for (int i = 0; i < n && k < 10; i++) {
        int h = (i == sel) ? ROW_F : ROW_H;
        if (y + h > top && y < top + band) {
            Row r; make_row(s_page, i, r);
            if (slot_begin(k, y, h, fnv(row_sig(r, i == sel, icon), ACC), next)) {
                RowBox rb = { &r, i == sel, icon };
                paint_box(0, W - 3, y, h, row_box, &rb);
            }
            k++;
        }
        y += h;
    }
    slots_end(next, k, y < top + band ? y : top + band);
    d.clearClipRect();
    draw_scrollbar(top, band, sc, total);
}

// Wi-Fi status card: signal ring + network name + address line. Returns the y below it.
static void card_box(int top, const void *);
static int draw_wifi_card(int top)
{
    const int h = 38;
    int rssi = nucleo_setup_rssi(), q = squality(rssi);
    bool sta = connected(), ap = ap_up();
    uint32_t sg = fnv_s(fnv_s(fnv_s(3u, nucleo_setup_ssid()), nucleo_setup_ip()), nucleo_setup_ap_ssid());
    sg = fnv(fnv(fnv(sg, (sta ? 1u : 0) | (ap ? 2u : 0) | (ap_resc() ? 4u : 0)), q / 5), rssi / 3);
    if (!s_full && sg == s_card_sig) return top + h + 1;              // unchanged: leave it on the panel
    s_card_sig = sg;
    mark(top, top + h);
    paint_box(0, W, top, h, card_box, nullptr);
    return top + h + 1;
}
static void card_box(int top, const void *)
{
    const int h = 38, cx = 21, cy = top + h / 2 - 1;
    int rssi = nucleo_setup_rssi(), q = squality(rssi);
    bool sta = connected(), ap = ap_up();
    d.fillArc(cx, cy, 11, 15, 135.0f, 405.0f, LINE);
    if (sta) d.fillArc(cx, cy, 11, 15, 135.0f, 135.0f + q * 2.7f, qcol(q));
    ui_glyph(&d, ap && !sta ? UG_HOTSPOT : UG_WIFI, cx, cy + 1, 6, sta ? qcol(q) : ap ? WARN : DIM, BG);
    const int x = 42, mw = W - x - 6;
    char b[40];
    if (sta) {
        txt_fit(x, top + 2, nucleo_setup_ssid(), mw, GOOD, BG, 2);
        snprintf(b, sizeof b, "%s  %d dBm", nucleo_setup_ip(), rssi);
        f2(x, top + 19, b, mw, FG, BG, false);
    } else if (ap) {
        txt_fit(x, top + 2, nucleo_setup_ap_ssid(), mw, WARN, BG, 2);
        f2(x, top + 19, ap_resc() ? TR5("192.168.4.1  soccorso", "192.168.4.1  rescue", "192.168.4.1  rescate",
                                        "192.168.4.1  secours", "192.168.4.1  Notfall") : "192.168.4.1  hotspot", mw, FG, BG, false);
    } else {
        txt_fit(x, top + 2, TR5("Non connesso", "Not connected", "Sin conexion", "Non connecte", "Nicht verbunden"), mw, MUTED, BG, 2);
        f2(x, top + 19, TR5("Scegli una rete qui sotto", "Pick a network below", "Elegir una red abajo",
                            "Choisir un reseau en bas", "Netz unten auswaehlen"), mw, MUTED, BG, false);
    }
    d.drawFastHLine(8, top + h - 1, W - 16, LINE);
}

// Nearby networks: row 0 = scan again, then one row per network; the focused one grows to show its
// security / channel / signal on a readable second line.
static void draw_net_row(int i, int y, int h, bool foc)
{
    uint16_t bg = foc ? ACC : BG, ink = foc ? INK : FG;
    if (foc) d.fillRoundRect(3, y, W - 8, h - 1, 9, ACC);
    if (i == 0) {
        ui_glyph(&d, UG_RESET, 14, y + 9, 6, foc ? INK : ACC, bg);
        txt(26, y + 2, nucleo_setup_scan_count() ? TR5("Cerca di nuovo", "Scan again", "Buscar de nuevo", "Chercher encore", "Erneut suchen")
                                                 : TR5("Cerca reti", "Scan", "Buscar redes", "Chercher", "Suchen"), ink, bg, 2);
        return;
    }
    int k = i - 1;
    const char *ss = nucleo_setup_scan_ssid(k);
    int rssi = nucleo_setup_scan_rssi(k), q = squality(rssi);
    bool cur = connected() && !strcmp(ss, nucleo_setup_ssid());
    bool known = nucleo_setup_net_is_known(ss), pref = known && prio_of(ss) > 0;
    int lit = q >= 75 ? 4 : q >= 50 ? 3 : q >= 25 ? 2 : q > 0 ? 1 : 0;
    for (int b = 0; b < 4; b++) {
        int bh = 3 + b * 3;
        if (b < lit || !foc) d.fillRect(7 + b * 4, y + 15 - bh, 3, bh, b < lit ? (foc ? INK : qcol(q)) : LINE);
    }
    if (nucleo_setup_scan_secure(k)) ui_glyph(&d, UG_LOCK, 29, y + 9, 4, foc ? INK : MUTED, bg);
    char name[34]; app_ui_ascii_fold(ss[0] ? ss : TR5("(nascosta)", "(hidden)", "(oculta)", "(masque)", "(versteckt)"), name, sizeof name);
    txt_fit(38, y + 2, name, W - 38 - 22, foc ? INK : (cur ? GOOD : FG), bg, 2);
    if (pref)       ui_glyph(&d, UG_STAR, W - 15, y + 9, 5, foc ? INK : GOOD, bg);
    else if (known) d.fillCircle(W - 15, y + 9, 3, foc ? INK : ACC);
    if (foc) {
        char det[48];
        snprintf(det, sizeof det, "%s  ch%d  %d dBm%s", nucleo_setup_scan_auth_label(k), nucleo_setup_scan_channel(k), rssi,
                 cur   ? TR5("  in uso", "  in use", "  en uso", "  utilise", "  aktiv")
                 : known ? TR5("  salvata", "  saved", "  guardada", "  connu", "  bekannt") : "");
        f2(38, y + 17, det, W - 38 - 12, INK, bg, false);
    }
}

// Busy spinner: a LINE ring with a rotating accent arc. The ring repaints the arc's old position in
// place (no clear), so it animates on the direct path without blinking.
static void draw_spinner(int cx, int cy)
{
    int a = (s_anim * 36) % 360;
    d.fillArc(cx, cy, 10, 14, 0.0f, 360.0f, LINE);
    d.fillArc(cx, cy, 10, 14, (float)a, (float)(a + 270), ACC);
}

static uint32_t net_sig(int i, bool foc)
{
    if (i == 0) return fnv(fnv(11u, foc), nucleo_setup_scan_count() ? 1u : 0u);
    int k = i - 1; const char *ss = nucleo_setup_scan_ssid(k);
    int rssi = nucleo_setup_scan_rssi(k), q = squality(rssi);
    int lit = q >= 75 ? 4 : q >= 50 ? 3 : q >= 25 ? 2 : q > 0 ? 1 : 0;
    bool known = nucleo_setup_net_is_known(ss), pref = known && prio_of(ss) > 0;
    bool cur = connected() && !strcmp(ss, nucleo_setup_ssid());
    uint32_t h = fnv(fnv_s(13u, ss), (uint32_t)lit | (nucleo_setup_scan_secure(k) ? 8u : 0) | (known ? 16u : 0) |
                                       (pref ? 32u : 0) | (cur ? 64u : 0) | (foc ? 128u : 0));
    if (foc) h = fnv(fnv(h, (uint32_t)rssi), (uint32_t)nucleo_setup_scan_channel(k));
    return h;
}

struct NetBox { int i, h; bool foc; };
static void net_box(int y, const void *ctx) { const NetBox *b = (const NetBox *)ctx; draw_net_row(b->i, y, b->h, b->foc); }

static void draw_nets(int ch)
{
    int n = nucleo_setup_scan_count(), total_n = n + 1, sel = cur_sel();
    char pos[8] = ""; if (n && !(s_busy && s_op == OP_SCAN)) snprintf(pos, sizeof pos, "%d", n);
    draw_header(page_title(PG_NETS), pos, true);
    int top = HDR + 1, band = ch - 1 - top;
    if (s_busy && s_op == OP_SCAN) {                                 // scanning is its own scene
        draw_spinner(W / 2, top + 34);
        if (s_full) { const char *m = TR5("Cerco le reti...", "Scanning...", "Buscando redes...", "Recherche...", "Suche Netze...");
                      txt(W / 2 - (int)strlen(m) * 6, top + 60, m, FG, BG, 2); }
        return;
    }
    int total, sc = list_layout(total_n, sel, band, &total);
    Slot next[10]; int k = 0;
    d.setClipRect(0, top, W, band);
    int y = top - sc;
    for (int i = 0; i < total_n && k < 10; i++) {
        int h = (i == sel) ? ROW_F : ROW_H;
        if (y + h > top && y < top + band) {
            if (slot_begin(k, y, h, fnv(net_sig(i, i == sel), ACC), next)) {
                NetBox nb = { i, h, i == sel };
                paint_box(0, W - 3, y, h, net_box, &nb);
            }
            k++;
        }
        y += h;
    }
    slots_end(next, k, y < top + band ? y : top + band);
    d.clearClipRect();
    draw_scrollbar(top, band, sc, total);
    if (!n && s_full) {                                              // "no networks" is part of the scene
        f2(10, top + ROW_F + 10, TR5("Nessuna rete trovata.", "No networks found.", "Ninguna red encontrada.",
                                     "Aucun reseau trouve.", "Keine Netze gefunden."), W - 20, MUTED, BG, false);
        txt(10, top + ROW_F + 30, TR5("Invio sulla riga sopra per riprovare.", "ENTER on the row above to retry.", "ENTER arriba para reintentar.",
                                      "ENTER en haut pour reessayer.", "ENTER oben fuer neuen Versuch."), DIM, BG, 1);
    }
}

// ---- date/time editor ----------------------------------------------------------------
// No typing: LEFT/RIGHT pick the field, UP/DOWN roll it (held = fast), ENTER saves. Lets an OFFLINE
// unit (no NTP) get a correct wall clock — the firmware setter takes local time and persists it.
static int dt_days(int y, int m)
{
    static const int dim[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };   // NB: `d` is the draw-target macro
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return (m >= 1 && m <= 12) ? dim[m - 1] : 31;
}
static void dt_adjust(int dir)
{
    int *v = &s_dtv[s_dtf];
    *v += dir;
    switch (s_dtf) {
        case 0: if (*v < DT_YEAR_MIN) *v = DT_YEAR_MIN; if (*v > 2099) *v = 2099; break;   // below = "clock not set"
        case 1: if (*v < 1) *v = 12; if (*v > 12) *v = 1; break;
        case 2: { int dm = dt_days(s_dtv[0], s_dtv[1]); if (*v < 1) *v = dm; if (*v > dm) *v = 1; } break;
        case 3: if (*v < 0) *v = 23; if (*v > 23) *v = 0; break;
        case 4: if (*v < 0) *v = 59; if (*v > 59) *v = 0; break;
    }
    int dm = dt_days(s_dtv[0], s_dtv[1]); if (s_dtv[2] > dm) s_dtv[2] = dm;
}
static void dt_open(void)
{
    time_t now = time(NULL); struct tm tmv; localtime_r(&now, &tmv);
    if (now >= DT_EPOCH_2023) {
        s_dtv[0] = tmv.tm_year + 1900; s_dtv[1] = tmv.tm_mon + 1; s_dtv[2] = tmv.tm_mday;
        s_dtv[3] = tmv.tm_hour;        s_dtv[4] = tmv.tm_min;
    } else {                                                         // never set: start from the build date,
        static const char MON[] = "JanFebMarAprMayJunJulAugSepOctNovDec";   // a few keypresses from today
        const char *bd = __DATE__;                                   // "Sep 29 2026"
        char mon[4] = { bd[0], bd[1], bd[2], 0 };
        const char *m = strstr(MON, mon);
        s_dtv[0] = atoi(bd + 7); s_dtv[1] = m ? (int)(m - MON) / 3 + 1 : 1; s_dtv[2] = atoi(bd + 4);
        s_dtv[3] = 12; s_dtv[4] = 0;
        if (s_dtv[0] < DT_YEAR_MIN) s_dtv[0] = DT_YEAR_MIN;
    }
    s_dtf = 2; s_dt = true;                                          // start on the day (leftmost field)
}
static void dt_box(int x, int y, int w, int h, const char *s, int sz, bool sel)
{
    d.fillRoundRect(x, y, w, h, 6, sel ? ACC : LINE);
    int tw = (int)strlen(s) * 6 * sz;
    txt(x + (w - tw) / 2, y + (h - 8 * sz) / 2, s, sel ? INK : FG, sel ? ACC : LINE, sz);
}
// One field box, repainted only when its value or focus changed (index = field id 0..4).
struct DtBox { int x, w, h, sz; const char *str; bool sel; };
static void dt_box_fn(int y, const void *ctx) { const DtBox *b = (const DtBox *)ctx; dt_box(b->x, y, b->w, b->h, b->str, b->sz, b->sel); }
static void dt_box_c(int f, int x, int y, int w, int h, const char *str, int sz)
{
    uint32_t sg = fnv(fnv_s(5u, str), s_dtf == f ? 1u : 2u);
    if (!s_full && s_dtbox[f] == sg) return;
    s_dtbox[f] = sg; mark(y, y + h);
    DtBox b = { x, w, h, sz, str, s_dtf == f };
    paint_box(x, w, y, h, dt_box_fn, &b);
}
static void draw_datetime(void)
{
    draw_header(TR5("Data e ora", "Date/time", "Fecha y hora", "Date/heure", "Datum/Zeit"), nucleo_setup_time_synced() ? "NTP" : NULL, true);
    char b[8];
    int dw = 40, mw = 40, yw = 64, g = 14, x = (W - (dw + mw + yw + 2 * g)) / 2, y = 37, bh = 26;
    int hw = 56, g2 = 20, tx = (W - (2 * hw + g2)) / 2, ty = 84, th = 32;
    if (s_full) {                                                    // captions + separators: static
        f2(8, 20, TR5("Giorno / mese / anno", "Day / month / year", "Dia / mes / anio",
                      "Jour / mois / annee", "Tag / Monat / Jahr"), W - 16, MUTED, BG, false);
        txt(x + dw + 1, y + 5, "/", MUTED, BG, 2);
        txt(x + dw + g + mw + 1, y + 5, "/", MUTED, BG, 2);
        f2(8, 66, TR5("Ore : minuti", "Hours : minutes", "Horas : minutos", "Heures : minutes", "Stunden : Minuten"), W - 16, MUTED, BG, false);
        txt(tx + hw + 1, ty + 4, ":", FG, BG, 3);
    }
    snprintf(b, sizeof b, "%02d", s_dtv[2]); dt_box_c(2, x, y, dw, bh, b, 2); x += dw + g;
    snprintf(b, sizeof b, "%02d", s_dtv[1]); dt_box_c(1, x, y, mw, bh, b, 2); x += mw + g;
    snprintf(b, sizeof b, "%04d", s_dtv[0]); dt_box_c(0, x, y, yw, bh, b, 2);
    snprintf(b, sizeof b, "%02d", s_dtv[3]); dt_box_c(3, tx, ty, hw, th, b, 3);
    snprintf(b, sizeof b, "%02d", s_dtv[4]); dt_box_c(4, tx + hw + g2, ty, hw, th, b, 3);
}
static const int DT_ORDER[5] = { 2, 1, 0, 3, 4 };                   // on-screen order: D M Y h m
static void dt_step(int dir)
{
    int p = 0; for (int i = 0; i < 5; i++) if (DT_ORDER[i] == s_dtf) p = i;
    s_dtf = DT_ORDER[(p + dir + 5) % 5];
}

// ---- overlays: inline editor + toast ----------------------------------------------------
static void draw_input(int ch)
{
    int iy = ch - 28;
    const char *lab = s_im == IM_PASS   ? "Password"
                    : s_im == IM_APSSID ? TR5("Nome rete", "Network", "Red", "Reseau", "Netzname")
                    : s_im == IM_APPASS ? "Password"
                    :                     TR5("Nome", "Name", "Nombre", "Nom", "Name");
    if (touched(iy, iy + 25)) {                                      // frame: first paint, or rows repainted under it
        d.fillRoundRect(4, iy, W - 8, 25, 7, LINE);
        d.drawRoundRect(4, iy, W - 8, 25, 7, ACC);
        txt(11, iy + 9, lab, ACC, LINE, 1);
    }
    // The value at size 2 (12 px glyphs, readable on the 240 px panel). A password is masked except the
    // character just typed (PEEK_US) or all of it while revealed (TAB).
    bool mask = (s_im == IM_PASS) && !s_reveal;
    bool peek = mask && s_ikey_us && esp_timer_get_time() - s_ikey_us < PEEK_US;
    int vx = 11 + (int)strlen(lab) * 6 + 8, maxc = (W - 14 - vx) / 12 - 1;
    if (maxc > 36) maxc = 36;
    int from = s_ilen > maxc ? s_ilen - maxc : 0, k = 0;
    char sh[40];
    for (int i = from; i < s_ilen && k < maxc; i++, k++) sh[k] = (mask && !(peek && i == s_ilen - 1)) ? '*' : s_ibuf[i];
    for (int j = k; j <= maxc; j++) sh[j] = ' ';                    // pad: the field overwrites itself
    sh[maxc + 1] = 0;
    txt(vx, iy + 5, sh, FG, LINE, 2);
    d.fillRect(vx + k * 12 + 1, iy + 5, 2, 16, ACC);                 // caret (inside the text cell)
}
static void draw_toast(int ch)
{
    int iy = (s_im != IM_NONE) ? ch - 52 : ch - 23;
    uint32_t sg = fnv(fnv_s(9u, s_msg), s_msg_ok ? 1u : 2u);
    if (s_toast_on && sg == s_toast_sig && iy == s_toast_y && !touched(iy, iy + 20)) return;
    s_toast_sig = sg; s_toast_y = iy;
    d.fillRoundRect(4, iy, W - 8, 20, 7, LINE);
    ui_glyph(&d, s_msg_ok ? UG_CHECK : UG_INFO, 14, iy + 10, 6, s_msg_ok ? GOOD : ACC, LINE);
    txt_fit(26, iy + 6, s_msg, W - 34, FG, LINE, 1);
}

static void draw_joining(void)
{
    draw_header("Wi-Fi", NULL, true);
    draw_spinner(W / 2, 48);
    if (!s_full) return;                                             // the rest is static
    const char *m = TR5("Connessione a", "Connecting to", "Conectando a", "Connexion a", "Verbinde mit");
    f2(W / 2 - 50, 68, m, 100, MUTED, BG, false);
    char name[34]; app_ui_ascii_fold(s_join_ssid, name, sizeof name);
    char b[20]; snprintf(b, sizeof b, "%.18s", name);
    txt(W / 2 - (int)strlen(b) * 6, 88, b, FG, BG, 2);
}

// "All set": the end of the first-boot wizard. Where the device is reachable now and the pairing PIN —
// everything needed to open the web OS — at sizes readable at arm's length.
static void draw_done(void)
{
    if (!s_full) return;                                             // static page
    const char *t = TR5("Tutto pronto!", "All set!", "Todo listo!", "Tout est pret !", "Alles bereit!");
    int tw = (int)strlen(t) * 12, tx = (W - tw - 18) / 2;
    ui_glyph(&d, UG_CHECK, tx + 6, 12, 7, GOOD, BG);
    txt(tx + 18, 4, t, GOOD, BG, 2);
    d.drawFastHLine(0, 23, W, LINE);
    char b[48], name[34];
    int y = 28;
    if (s_done_sta) {
        app_ui_ascii_fold(nucleo_setup_ssid(), name, sizeof name);
        snprintf(b, sizeof b, TR5("Connesso a %s", "Connected to %s", "Conectado a %s", "Connecte a %s", "Verbunden mit %s"), name);
        f2(8, y, b, W - 16, FG, BG, false); y += 20;
    } else {
        app_ui_ascii_fold(nucleo_setup_ap_ssid(), name, sizeof name);
        snprintf(b, sizeof b, "Hotspot: %s", name);
        f2(8, y, b, W - 16, FG, BG, false); y += 17;
        if (nucleo_setup_ap_secure()) { snprintf(b, sizeof b, "Password: %s", nucleo_setup_ap_pass()); f2(8, y, b, W - 16, FG, BG, false); }
        y += 20;
    }
    int lw = f2(8, y + 1, TR5("Apri dal browser", "Open in a browser", "Abre en el navegador", "Ouvre dans un navigateur", "Im Browser oeffnen"),
                W - 16, MUTED, BG, false);
    (void)lw; y += 17;
    txt_fit(8, y, s_done_sta ? nucleo_setup_ip() : "192.168.4.1", W - 16, ACC, BG, 2); y += 22;
    const char *pin = nucleo_auth_pin();
    snprintf(b, sizeof b, "PIN %s", pin && pin[0] ? pin : "------");
    txt_fit(8, y, b, W - 16, FG, BG, 2);
}

// The first-boot step closing (hotspot up + setup saved): a second or two, never a frozen list.
static void draw_finishing(void)
{
    draw_header("Wi-Fi", NULL, false);
    draw_spinner(W / 2, 52);
    if (!s_full) return;
    const char *m = TR5("Attivo l'hotspot...", "Starting the hotspot...", "Activando el hotspot...",
                        "Demarrage du hotspot...", "Hotspot startet...");
    f2(W / 2 - (int)strlen(m) * 7 / 2, 76, m, W - 16, MUTED, BG, false);
}

// ---- on_draw -------------------------------------------------------------------------------
// What forces a FULL paint on the direct path: anything that changes the layout of the whole screen.
static uint32_t scene_sig(void)
{
    uint32_t h = fnv(fnv(1u, s_page), s_dt ? 1u : 0u);
    h = fnv(h, (s_busy && s_op == OP_JOIN) ? 1u : 0u);
    h = fnv(h, ((s_busy && s_op == OP_FINISH) || s_ob_finish_pend) ? 1u : 0u);
    h = fnv(h, (s_page == PG_NETS && s_busy && s_op == OP_SCAN) ? 1u : 0u);
    h = fnv(fnv(h, s_im != IM_NONE ? 1u : 0u), s_cf);
    h = fnv(fnv(fnv(h, THEME_BG), THEME_ACC), THEME_FG);
    h = fnv(h, nucleo_i18n_gen());
    h = fnv(h, s_onboard);
    h = fnv(h, page_count(s_page) == 0 ? 1u : 0u);
    h = fnv(h, (s_page == PG_NETS && nucleo_setup_scan_count() == 0) ? 1u : 0u);
    return h;
}

static void on_draw(void)
{
    int ch = nucleo_app_content_height();
    bool toast = s_msg_t > 0, toast_gone = s_toast_on && !toast;
    uint32_t scene = scene_sig();
    unsigned gen = nucleo_app_repaint_gen();
    // Buffered frames are always full (the framework wiped the canvas); direct frames are full only
    // on a scene change, when the app regained the screen, or when a toast leaves a non-list screen.
    s_full = nucleo_app_is_buffered() || scene != s_scene || gen != s_rgen ||
             (toast_gone && (s_dt || (s_busy && s_op == OP_JOIN) || s_page == PG_NETS));
    s_scene = scene; s_rgen = gen; s_rep_lo = 9999; s_rep_hi = -1;
    if (nucleo_app_is_buffered() && s_strip) strip_release();        // back on the canvas: hand the strip back
    d.setFont(&fonts::Font0); d.setTextSize(1);
    if (s_full) {
        d.fillRect(0, 0, W, ch, BG);
        s_nslot = 0; s_list_bot = 0; s_hdr_sig = s_card_sig = s_sbar_sig = s_ovl_sig = 0;
        memset(s_dtbox, 0, sizeof s_dtbox);
    } else if (toast_gone) {                                         // give the toast's band back to the rows
        for (int i = 0; i < s_nslot; i++)                            // the rows under it repaint (atomically)
            if (s_slot[i].y < s_toast_y + 20 && s_slot[i].y + s_slot[i].h > s_toast_y) s_slot[i].sig = 0;
        int y0 = s_list_bot > s_toast_y ? s_list_bot : s_toast_y;   // below the list: plain background
        if (y0 < s_toast_y + 20) d.fillRect(0, y0, W - 3, s_toast_y + 20 - y0, BG);
        mark(s_toast_y, s_toast_y + 20);
    }
    if (s_onboard == OB_DONE)            draw_done();
    else if (s_dt)                       draw_datetime();
    else if (s_busy && s_op == OP_JOIN)  draw_joining();
    else if ((s_busy && s_op == OP_FINISH) || s_ob_finish_pend) draw_finishing();
    else if (s_page == PG_NETS)          draw_nets(ch);
    else                                 draw_page(ch);
    if (s_im != IM_NONE) draw_input(ch);
    uint32_t ov = fnv(s_cf, s_cf_yes ? 1u : 2u);
    if (s_cf != R_NONE && (touched(15, 106) || ov != s_ovl_sig)) {     // confirm card (y 15..105)
        s_ovl_sig = ov; mark(15, 106);
        char msg[36];
        switch (s_cf) {
        case R_RESTART:
            app_ui_confirm(TR5("Riavviare?", "Restart?", "Reiniciar?", "Redemarrer?", "Neustarten?"),
                           TR5("Il dispositivo si riavvia ora.", "The device restarts now.", "El equipo se reinicia ahora.",
                               "L'appareil redemarre.", "Das Geraet startet jetzt neu."), s_cf_yes); break;
        case R_LAUNCHER:
            // Launcher's splash auto-boots the selected app (us) after its timer: ENTER there opens its menu.
            app_ui_confirm(TR5("Tornare a M5Launcher?", "Back to M5Launcher?", "Volver a M5Launcher?", "Retour a M5Launcher?", "Zu M5Launcher?"),
                           TR5("Premi INVIO sul suo avvio.", "Press ENTER on its splash.", "Pulsa ENTER en su inicio.",
                               "ENTREE sur son demarrage.", "ENTER im Startbild druecken."), s_cf_yes); break;
        case R_FORGET_ALL:
            snprintf(msg, sizeof msg, TR5("%d reti e le loro password.", "%d networks and passwords.", "%d redes y sus claves.",
                                          "%d reseaux et mots de passe.", "%d Netze und Passwoerter."), nucleo_setup_net_count());
            app_ui_confirm(TR5("Dimenticare tutte?", "Forget all?", "Olvidar todas?", "Tout oublier?", "Alle vergessen?"), msg, s_cf_yes); break;
        case R_AP_ON:
            if (ap_on()) app_ui_confirm(TR5("Spegnere hotspot?", "Hotspot off?", "Apagar hotspot?", "Couper hotspot?", "Hotspot aus?"),
                                        TR5("Chi e collegato perde la rete.", "Connected devices will drop.", "Los conectados pierden la red.",
                                            "Les clients perdent le reseau.", "Verbundene verlieren das Netz."), s_cf_yes);
            else         app_ui_confirm(TR5("Accendere hotspot?", "Hotspot on?", "Activar hotspot?", "Activer hotspot?", "Hotspot an?"),
                                        TR5("Il Wi-Fi attuale si disconnette.", "Your Wi-Fi link will drop.", "El Wi-Fi actual se desconecta.",
                                            "Le Wi-Fi actuel se deconnecte.", "Das aktuelle Wi-Fi trennt sich."), s_cf_yes);
            break;
        case R_OB_SKIP:
            app_ui_confirm(TR5("Saltare il Wi-Fi?", "Skip Wi-Fi?", "Omitir el Wi-Fi?", "Sans Wi-Fi ?", "Ohne WLAN?"),
                           TR5("Userai il suo hotspot.", "You will use its hotspot.", "Usaras su hotspot.",
                               "Tu utiliseras son hotspot.", "Du nutzt seinen Hotspot."), s_cf_yes); break;
        case R_SESSIONS:
            app_ui_confirm(TR5("Disconnettere?", "Sign out all?", "Cerrar sesiones?", "Deconnecter?", "Alle abmelden?"),
                           TR5("I browser rifaranno il pairing.", "Browsers must pair again.", "Hay que vincular de nuevo.",
                               "Il faudra associer a nouveau.", "Browser muessen neu koppeln."), s_cf_yes); break;
        default:
            app_ui_ascii_fold(s_cf_ssid, msg, sizeof msg);
            app_ui_confirm(TR5("Dimenticare rete?", "Forget network?", "Olvidar red?", "Oublier reseau?", "Netz vergessen?"), msg, s_cf_yes);
            break;
        }
    }
    if (toast) draw_toast(ch);
    s_toast_on = toast;
}

// ---- hint bar (<=39 chars, docs/native-ui-kit.md §5 vocabulary) ------------------------------
static void update_hint(void)
{
    static char hb[48];
    if (s_onboard == OB_DONE) { nucleo_app_set_hint(TR5("invio inizia", "enter start", "enter empezar", "enter commencer", "enter starten")); return; }
    if (s_im == IM_PASS) { nucleo_app_set_hint(s_reveal
                                      ? TR5("invio ok  tab nascondi  esc annulla", "enter ok  tab hide  esc cancel", "enter ok  tab ocultar  esc anular",
                                            "enter ok  tab cacher  esc annuler", "enter ok  tab verbergen  esc Abbr.")
                                      : TR5("invio ok  tab mostra  esc annulla", "enter ok  tab show  esc cancel", "enter ok  tab mostrar  esc anular",
                                            "enter ok  tab voir  esc annuler", "enter ok  tab zeigen  esc Abbr.")); return; }
    if (s_im != IM_NONE)          { nucleo_app_set_hint(TR5("invio salva   esc annulla", "enter save   esc cancel", "enter guardar   esc anular",
                                                            "enter valider   esc annuler", "enter sichern   esc Abbruch")); return; }
    if (s_cf != R_NONE)           { nucleo_app_set_hint(TR5("</> scegli   invio conferma", "</> pick   enter confirm", "</> elegir   enter confirmar",
                                                            "</> choisir   enter valider", "</> Wahl   enter bestaetigen")); return; }
    if (s_dt)                     { nucleo_app_set_hint(TR5("</> campo   su/giu valore   invio salva", "</> field   up/dn value   enter save",
                                                            "</> campo  up/dn valor  enter fijar", "</> champ  up/dn valeur  enter fixer",
                                                            "</> Feld   up/dn Wert   enter setzen")); return; }
    if (s_busy && s_op == OP_JOIN){ nucleo_app_set_hint(TR5("connessione in corso...", "connecting...", "conectando...",
                                                            "connexion...", "verbinde...")); return; }
    if ((s_busy && s_op == OP_FINISH) || s_ob_finish_pend) { nucleo_app_set_hint(TR5("un attimo...", "one moment...", "un momento...",
                                                            "un instant...", "einen Moment...")); return; }
    const char *back = s_onboard ? TR5("esc salta", "esc skip", "esc omitir", "esc passer", "esc ueberspringen")
                     : (s_page == PG_ROOT) ? TR5("esc esci", "esc back", "esc salir", "esc sortir", "esc Ende")
                     : s_page == PG_SEARCH ? TR5("esc chiude", "esc close", "esc cerrar", "esc fermer", "esc zurueck")
                     :                       TR5("esc indietro", "esc back", "esc atras", "esc retour", "esc zurueck");
    if (s_page == PG_NETS) {
        int sel = cur_sel();
        if (sel == 0 || (s_busy && s_op == OP_SCAN)) {
            snprintf(hb, sizeof hb, "%s   %s", TR5("invio cerca", "enter scan", "enter buscar", "enter scanner", "enter suchen"), back);
            nucleo_app_set_hint(hb); return;
        }
        bool known = nucleo_setup_net_is_known(nucleo_setup_scan_ssid(sel - 1));
        if (known) nucleo_app_set_hint(TR5("invio connetti   p pref   canc elimina", "enter join   p prefer   del forget",
                                           "enter conectar   p pref   del olvidar", "enter connecter   p pref   del oublier",
                                           "enter verbinden  p pref  del vergessen"));
        else { snprintf(hb, sizeof hb, "%s   %s", TR5("invio connetti", "enter join", "enter conectar", "enter connecter", "enter verbinden"), back);
               nucleo_app_set_hint(hb); }
        return;
    }
    Row r;
    if (!focused_row(r)) {
        nucleo_app_set_hint(s_page == PG_SEARCH ? TR5("digita   canc cancella   esc chiude", "type   del erase   esc close",
                                                      "teclear   del borrar   esc cerrar", "taper   del effacer   esc fermer",
                                                      "tippen   del loeschen   esc zurueck") : back);
        return;
    }
    if (r.id == R_SAVED_NET) { nucleo_app_set_hint(TR5("invio preferita   canc elimina", "enter prefer   del forget", "enter preferir   del olvidar",
                                                       "enter preferer   del oublier", "enter bevorzugen   del vergessen")); return; }
    if (s_rst_id != R_NONE && r.id == s_rst_id) {
        snprintf(hb, sizeof hb, TR5("invio ancora %d   altro tasto annulla", "enter %d more   other key cancels", "enter %d mas   otra tecla anula",
                                    "enter encore %d   autre touche annule", "enter noch %dx   sonst Abbruch"), s_rst_left);
        nucleo_app_set_hint(hb); return;
    }
    const char *verb;
    switch (r.kind) {
        case K_TOGGLE: verb = TR5("invio on/off", "enter on/off", "enter on/off", "enter on/off", "enter an/aus"); break;
        case K_SLIDER: verb = TR5("</> regola", "</> adjust", "</> ajustar", "</> regler", "</> regeln"); break;
        case K_CYCLE:  verb = TR5("</> cambia", "</> change", "</> cambiar", "</> changer", "</> aendern"); break;
        case K_EDIT:   verb = TR5("invio modifica", "enter edit", "enter editar", "enter modifier", "enter bearbeiten"); break;
        case K_INFO:   verb = TR5("su/giu scegli", "up/dn pick", "up/dn elegir", "up/dn choisir", "up/dn waehlen"); break;
        case K_DANGER: verb = is_rst(r.id)
                            ? TR5("invio 3 volte", "enter 3 times", "enter 3 veces", "enter 3 fois", "enter 3-mal")
                            : TR5("invio conferma", "enter confirm", "enter confirmar", "enter valider", "enter ausfuehren"); break;
        default:       verb = TR5("invio apri", "enter open", "enter abrir", "enter ouvrir", "enter zeigen"); break;
    }
    if (s_page == PG_ROOT && r.kind == K_NAV)
        snprintf(hb, sizeof hb, "%s   %s   %s", verb, TR5("digita cerca", "type to find", "a-z buscar", "a-z filtre", "a-z suchen"), back);
    else
        snprintf(hb, sizeof hb, "%s   %s", verb, back);
    nucleo_app_set_hint(hb);
}

// ---- worker (scan / join off the UI loop) ------------------------------------------------
static void wipe(char *p, size_t n) { volatile char *v = p; while (n--) *v++ = 0; }   // not elided like memset
static void wifi_task(void *)
{
    if (s_op == OP_SCAN) nucleo_setup_scan();
    else if (s_op == OP_JOIN) {
        // The task owns its copy of the credentials: the app can be force-switched mid-handshake
        // (screensaver, ANIMA, web handoff) and leave() must never zero a password still being read.
        char ssid[sizeof s_join_ssid], pass[sizeof s_join_pass];
        memcpy(ssid, s_join_ssid, sizeof ssid); memcpy(pass, s_join_pass, sizeof pass);
        ssid[sizeof ssid - 1] = 0; pass[sizeof pass - 1] = 0;
        wipe(s_join_pass, sizeof s_join_pass);
        s_join_ok = nucleo_setup_join(ssid, pass);
        wipe(pass, sizeof pass);
    }
    // Ending the first-boot step may bring the hotspot up and persists setup to /cfg + NVS + the SD: blocking
    // driver calls and the deep store chain, so it runs HERE, never on the launcher's (main) task — where it
    // overflowed the stack and rebooted the device on "Skip Wi-Fi" (back to M5Launcher's splash).
    else if (s_op == OP_FINISH) s_done_sta = nucleo_setup_onboard_finish();
    s_done = true; s_task = nullptr; vTaskDelete(nullptr);
}
static void start_op(int op)
{
    if (s_busy) return;
    s_op = op; s_busy = true; s_done = false; s_anim = 0;
    // 6 KB like the Wi-Fi supervisor: a successful join persists networks + setup (cJSON + LittleFS +
    // NVS + SD FAT writes) on this task; 4 KB was too tight for that call chain.
    if (xTaskCreate(wifi_task, "wifi", 6144, nullptr, tskIDLE_PRIORITY + 2, &s_task) != pdPASS) {
        s_busy = false; s_task = nullptr;
        toast(TR5("Memoria insufficiente, riprova", "Out of memory, try again", "Sin memoria, reintentar",
                  "Memoire pleine, reessayer", "Speicher voll, erneut versuchen"));
    }
}

// ---- navigation ------------------------------------------------------------------------
static void flush_prefs(void)
{
    if (!s_prefs_dirty) return;
    s_prefs_dirty = false;
    nucleo_app_persist_prefs();
    if (s_tts_pend >= 0) { nucleo_tts_set_speed(s_tts_pend); s_tts_pend = -1; }   // one SD write + one cache purge per edit
    if (s_theme_pend) {                                            // one LittleFS + one SD write per theme pick
        s_theme_pend = false;
        char id[24]; snprintf(id, sizeof id, "%s", nucleo_theme_get_current());
        nucleo_theme_set(id);
    }
    if (s_saver_t_pend >= 0) { nucleo_screensaver_set_timeout_s(s_saver_t_pend); s_saver_t_pend = -1; }
    if (s_saver_m_pend >= 0) { nucleo_screensaver_set_mode(s_saver_m_pend); s_saver_m_pend = -1; }
}

static void set_page(int pg)
{
    flush_prefs();
    s_rst_id = R_NONE; s_page = (uint8_t)pg; s_scroll = 0;
    int n = page_count(pg);
    if (s_sel[pg] >= n) s_sel[pg] = (int8_t)(n > 0 ? n - 1 : 0);
    if (s_sel[pg] < 0) s_sel[pg] = 0;
    if (pg == PG_NETS && nucleo_setup_scan_count() == 0 && !s_busy) start_op(OP_SCAN);   // opening it IS the request
}
static void open_section(int pg)
{
    for (int i = 0; i < NROWS(ROWS_ROOT); i++) if (section_of(ROWS_ROOT[i]) == pg) s_sel[PG_ROOT] = (int8_t)root_index_of(ROWS_ROOT[i]);
    set_page(pg);
}
static void go_back(void)
{
    if (s_page == PG_SEARCH) { s_qn = 0; s_q[0] = 0; set_page(PG_ROOT); return; }
    set_page(page_parent(s_page));
}

// TAB: jump to the next section (from the root: the focused one, or Wi-Fi).
static void on_tab(void)
{
    if (s_im == IM_PASS) { s_reveal = !s_reveal; update_hint(); nucleo_app_request_draw(); return; }   // show / hide the password
    if (s_im != IM_NONE || s_cf != R_NONE || s_dt || (s_busy && s_op == OP_JOIN) || s_onboard) return;   // first boot: locked
    int cur = (s_page == PG_NETS || s_page == PG_SAVED) ? (int)PG_WIFI : (int)s_page, next = PG_WIFI;
    if (cur == PG_ROOT || cur == PG_SEARCH) {
        Row r; focused_row(r);
        int sec = section_of(r.id); next = sec >= 0 ? sec : PG_WIFI;
    } else {
        for (int i = 0; i < NROWS(SECTIONS); i++) if (SECTIONS[i] == cur) next = SECTIONS[(i + 1) % NROWS(SECTIONS)];
    }
    s_qn = 0; s_q[0] = 0;
    open_section(next);
    update_hint(); nucleo_app_request_draw();
}

// ---- actions -------------------------------------------------------------------------
static void open_editor(int mode, const char *init)
{
    s_im = mode; snprintf(s_ibuf, sizeof s_ibuf, "%s", init ? init : ""); s_ilen = (int)strlen(s_ibuf);
    s_reveal = false; s_ikey_us = 0;
}

// LEFT/RIGHT on a slider or a choice. Returns false when the row isn't adjustable — toggles are
// deliberately NOT here: a stray LEFT meant as "back" must never switch the hotspot off. Held arrows
// accelerate (the key repeats every ~90 ms): a crown that spins faster the longer you turn it.
static bool adjust(const Row &r, int dir)
{
    if (r.dis) { toast(r.sub[0] ? r.sub : TR5("Non disponibile", "Not available", "No disponible", "Indisponible", "Nicht verfuegbar")); return true; }
    int64_t now = esp_timer_get_time();
    int step = (now - s_adj_us < 220000) ? 10 : 5;
    // Choices persist inside their setters (settings.json / theme.json / SD), so a held arrow must not
    // hammer the flash every ~90 ms repeat: at most one step per 350 ms (a tap is always immediate).
    if (r.kind == K_CYCLE) {
        if (now - s_cyc_us < 350000) return true;
        s_cyc_us = now;
    }
    switch (r.id) {
        case R_BRIGHT:    s_adj_us = now; nucleo_app_set_brightness(nucleo_app_brightness() + dir * step); s_prefs_dirty = true; return true;
        case R_VOLUME:    s_adj_us = now; nucleo_audio_set_volume(nucleo_audio_volume() + dir * step);     s_prefs_dirty = true; return true;
        case R_TTS_SPEED: s_adj_us = now; s_tts_pend = nucleo_tts_speed_clamp((s_tts_pend >= 0 ? s_tts_pend : nucleo_tts_speed()) + dir * step);
                          s_prefs_dirty = true; return true;
        case R_LANG:      lang_cycle(dir);  return true;
        case R_THEME:     theme_cycle(dir); return true;
        case R_ANIMA:     nucleo_anima_ui_set_online_mode((nucleo_anima_ui_online_mode() + dir + 3) % 3); return true;
        case R_SAVER_TIME:  saver_time_cycle(dir); return true;
        case R_SAVER_STYLE: s_saver_m_pend = ((s_saver_m_pend >= 0 ? s_saver_m_pend : nucleo_screensaver_mode()) + dir + 4) % 4;
                            s_prefs_dirty = true; return true;
        default: return false;
    }
}

// ENTER on a switch row. The hotspot switch drops/joins the Wi-Fi link, so it asks first (confirm card,
// same rule as the Control Center's arm-then-fire tile).
static void toggle(const Row &r)
{
    if (r.dis) { toast(r.sub[0] ? r.sub : TR5("Non disponibile", "Not available", "No disponible", "Indisponible", "Nicht verfuegbar")); return; }
    bool on = !r.on;
    switch (r.id) {
        case R_HANDOFF: nucleo_remote_set_enabled(on);
                        toast_ok(on ? TR5("Passaggio web attivo", "Web handoff on", "Traspaso web activo", "Relais web actif", "Web-Uebergabe an")
                                    : TR5("Passaggio web spento", "Web handoff off", "Traspaso web apagado", "Relais web coupe", "Web-Uebergabe aus")); break;
        case R_MUTE:    nucleo_audio_set_mute(on); nucleo_app_persist_prefs(); break;
        case R_TTS:     nucleo_tts_set_enabled(on);
                        toast_ok(on ? TR5("Lettura vocale attiva", "Read aloud on", "Lectura por voz activa", "Lecture vocale active", "Vorlesen an")
                                    : TR5("Lettura vocale spenta", "Read aloud off", "Lectura por voz apagada", "Lecture vocale coupee", "Vorlesen aus")); break;
        case R_VOICE:   nucleo_voice_set_always_on(on); break;
        case R_BT_BOOT: nucleo_ble_set_pref(on); s_bt_pref = on ? 1 : 0;
                        toast_ok(TR5("Vale dal prossimo riavvio", "Applies after a restart", "Se aplica al reiniciar",
                                     "Actif apres redemarrage", "Gilt nach Neustart")); break;
        case R_AP_ON:   s_cf = R_AP_ON; s_cf_yes = false; break;
    }
}

// Recursive delete for the reset rows (depth-first; a plain file is unlinked).
static void rm_tree(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) { unlink(path); return; }
    struct dirent *e;
    while ((e = readdir(dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[256]; snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        rm_tree(sub);
    }
    closedir(dir); rmdir(path);
}

// Empty a directory but keep the named top-level entries (user content that lives beside the config).
static void rm_children_except(const char *path, const char *const *keep, int nkeep)
{
    DIR *dir = opendir(path);
    if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        bool kept = false;
        for (int i = 0; i < nkeep && !kept; i++) kept = !strcasecmp(e->d_name, keep[i]);   // FAT names fold case
        if (kept) continue;
        char sub[256]; snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        rm_tree(sub);
    }
    closedir(dir);
}

static void activate(const Row &r)
{
    int sec = section_of(r.id);
    if (sec >= 0) { s_qn = 0; s_q[0] = 0; open_section(sec); return; }
    switch (r.id) {
    case R_SUGGEST: {
        int sg = suggestion();
        if (sg == SG_CLOCK)     dt_open();
        else if (sg == SG_BATT) { nucleo_app_set_brightness(40); nucleo_app_persist_prefs();
                                  toast_ok(TR5("Luminosita al 40%", "Brightness set to 40%", "Brillo al 40%", "Luminosite a 40%", "Helligkeit auf 40%")); }
        else                    { s_sel[PG_WIFI] = 0; open_section(PG_WIFI); set_page(PG_NETS); }
    } break;
    case R_LANG: case R_THEME: case R_ANIMA: case R_SAVER_TIME: case R_SAVER_STYLE: adjust(r, +1); break;
    case R_DATETIME: dt_open(); break;
    case R_NETS:     set_page(PG_NETS); break;
    case R_SAVED:    set_page(PG_SAVED); break;
    case R_HANDOFF: case R_AP_ON: case R_MUTE: case R_VOICE: case R_TTS: case R_BT_BOOT: toggle(r); break;
    case R_VOLUME:   nucleo_audio_set_mute(!nucleo_audio_is_muted()); nucleo_app_persist_prefs(); break;   // ENTER on volume = mute
    case R_AP_SSID:  open_editor(IM_APSSID, nucleo_setup_ap_ssid()); break;
    case R_AP_PASS:  open_editor(IM_APPASS, nucleo_setup_ap_pass()); break;
    case R_NAME:     open_editor(IM_NAME, nucleo_setup_device_name()); break;
    case R_UPDATES:  flush_prefs(); nucleo_app_launch_id("updates"); return;
    case R_RESTART:  s_cf = R_RESTART; s_cf_yes = false; break;
    case R_LAUNCHER:
        if (nucleo_guest_return_mode() == GUEST_RET_DEEP_SLEEP) { s_cf = R_LAUNCHER; s_cf_yes = false; }
        else toast(r.sub);   // Launcher's own settings forbid a software hand-off: the row says what to do
        break;
    case R_SESSIONS: if (r.kind == K_DANGER) { s_cf = R_SESSIONS; s_cf_yes = false; } break;
    case R_FORGET_ALL: s_cf = R_FORGET_ALL; s_cf_yes = false; break;
    case R_SAVED_NET: {
        const char *ss = nucleo_setup_net_ssid(r.num);
        bool pin = !r.on;
        nucleo_setup_net_set_priority(ss, pin ? WIFI_PIN_PRIO : 0);
        toast_ok(pin ? TR5("Preferita: si collega per prima", "Preferred: joined first", "Preferida: se une primero",
                           "Prefere: rejoint en premier", "Bevorzugt: zuerst verbunden")
                     : TR5("Priorita normale", "Normal priority", "Prioridad normal", "Priorite normale", "Normale Prioritaet"));
    } break;
    case R_RST_SOFT: case R_RST_HARD: case R_RST_FMT: case R_RST_SD:
        if ((r.id == R_RST_FMT || r.id == R_RST_SD) && r.dis) { toast(TR5("Nessuna scheda SD", "No SD card", "Sin tarjeta SD", "Pas de carte SD", "Keine SD-Karte")); break; }
        if (s_rst_id != r.id) { s_rst_id = r.id; s_rst_left = 2; }
        else if (--s_rst_left <= 0 && r.id == R_RST_FMT) {
            // The card only: armed, run at the next boot before anything opens a file on it (see
            // nucleo_storage_format_arm). Wi-Fi, PIN and sessions live on /cfg + NVS and stay.
            if (nucleo_storage_format_arm(nucleo_i18n_lang())) esp_restart();
            s_rst_id = R_NONE;
            toast(TR5("Formattazione non avviata", "Erase could not start", "No se pudo formatear", "Formatage impossible", "Formatieren nicht gestartet"));
        }
        else if (s_rst_left <= 0) {
            flush_prefs();
            // The brick-class config lives on internal flash (/cfg LittleFS + NVS), with the SD only a
            // mirror that /cfg and NVS heal back — so each owner erases EVERY tier of its store and seals
            // it (no background save lands before the reboot). SD paths alone never re-armed the wizard.
            // Soft: network + setup (networks, hotspot, device name, wizard re-runs), every Settings pref
            //       (settings.json, theme, read-aloud, web handoff, BT at boot), logs. User content that
            //       lives beside the config (calendar events, alarms) is kept: "Files kept".
            //       Pairing is KEPT: it is not network config, and the "Web sessions" row revokes it alone.
            // Hard: also pairing (PIN + every session), SMTP accounts, the Key deck server PIN, the rest of
            //       /cfg (launcher pins/recents), keys, the sent-mail log, ANIMA's learned data, backups,
            //       journal. FIDO passkeys stay (the Passkeys app resets those, with its own warning).
            bool ok = nucleo_setup_factory_reset();
            nucleo_remote_set_enabled(true); nucleo_ble_set_pref(false);   // prefs kept in NVS: factory defaults
            static const char *const KEEP[] = { "calendar.json", "alarm.json" };
            static const char *const SOFT[] = { "/sd/system/sessions", "/sd/system/log", "/sd/system/logs" };
            static const char *const SOFT_FILES[] = { "/cfg/config/theme.json", "/sd/apps/theme.cfg", "/sd/data/tts/speak.cfg",
                                                      "/sd/net_trace.txt", "/sd/boot_trace.txt" };   // prefs off /sd/system/config + logs
            static const char *const HARD[] = { "/cfg/config", "/sd/system/keys", "/sd/system/mail", "/sd/data/anima/learned",
                                                "/sd/config", "/sd/backups", "/sd/journal" };
            static const char *const HARD_FILES[] = { "/sd/data/anima/teacher.json", "/sd/data/anima/telemetry.ndjson",
                                                      "/sd/data/anima/session.txt", "/sd/data/anima/sessions.json", "/sd/data/anima/workspace.json" };
            // Erase SD: the factory reset's internal tiers, then the WHOLE card is formatted at the next boot
            // (armed, see nucleo_storage_format_arm) — so its SD paths are skipped here, the format takes them.
            const bool sd = r.id == R_RST_SD;
            if (!sd) {
                rm_children_except("/sd/system/config", KEEP, NROWS(KEEP));
                for (int i = 0; i < NROWS(SOFT); i++) rm_tree(SOFT[i]);
            }
            for (int i = 0; i < NROWS(SOFT_FILES); i++) if (!sd || !strncmp(SOFT_FILES[i], "/cfg/", 5)) unlink(SOFT_FILES[i]);
            if (r.id != R_RST_SOFT) {
                ok = nucleo_auth_factory_reset() && ok;
                ok = nucleo_mailcfg_erase_all() && ok;     // SMTP app passwords
                ok = nucleo_keydeck_forget() && ok;        // a remote device's address + PIN
                for (int i = 0; i < NROWS(HARD); i++) if (!sd || !strncmp(HARD[i], "/cfg/", 5)) rm_tree(HARD[i]);
                if (!sd) for (int i = 0; i < NROWS(HARD_FILES); i++) unlink(HARD_FILES[i]);
            }
            if (sd) ok = nucleo_storage_format_arm(nucleo_i18n_lang()) && ok;
            if (ok) esp_restart();
            // A tier survived (logged): don't reboot into a device that would heal it back and look reset.
            // Every step is idempotent and the stores stay sealed, so ENTER x3 again simply retries.
            s_rst_id = R_NONE;
            toast(TR5("Reset incompleto: riprova", "Reset incomplete: try again", "Reset incompleto: reintenta",
                      "Reset incomplet: reessayez", "Reset unvollstaendig: nochmal"));
        }
        break;
    default: break;                                 // info rows: nothing to do
    }
}

static void confirm_done(bool yes)
{
    uint8_t what = s_cf; s_cf = R_NONE;
    if (!yes) return;
    switch (what) {
    case R_RESTART:    flush_prefs(); esp_restart(); break;
    case R_LAUNCHER:   flush_prefs(); nucleo_guest_return_to_launcher();   // never returns when it can act
                       toast(TR5("Spegni e riaccendi", "Switch off and on", "Apaga y enciende", "Eteindre et rallumer", "Aus- und einschalten")); break;
    case R_FORGET_ALL: nucleo_setup_forget();
                       toast_ok(TR5("Reti dimenticate: hotspot attivo", "Networks forgotten: hotspot on", "Redes olvidadas: hotspot activo",
                                    "Reseaux oublies: hotspot actif", "Netze vergessen: Hotspot an")); break;
    case R_OB_SKIP:    onboard_end(); break;
    case R_SESSIONS:   nucleo_auth_revoke(NULL);
                       toast_ok(TR5("Tutti i browser disconnessi", "Every browser signed out", "Navegadores desconectados",
                                    "Navigateurs deconnectes", "Alle Browser abgemeldet")); break;
    case R_AP_ON: {
        bool on = !ap_on();
        if (on) nucleo_setup_start_ap(); else nucleo_setup_stop_ap();
        toast_ok(on ? TR5("Hotspot acceso: 192.168.4.1", "Hotspot on: 192.168.4.1", "Hotspot activo: 192.168.4.1",
                          "Hotspot actif: 192.168.4.1", "Hotspot an: 192.168.4.1")
                    : TR5("Hotspot spento: torno alla tua rete", "Hotspot off: rejoining your Wi-Fi", "Hotspot apagado: vuelvo a tu Wi-Fi",
                          "Hotspot coupe: retour au Wi-Fi", "Hotspot aus: zurueck zum Wi-Fi"));
    } break;
    case R_SAVED_NET: {
        nucleo_setup_forget_ssid(s_cf_ssid); toast_ok(TR5("Rete dimenticata", "Network forgotten", "Red olvidada", "Reseau oublie", "Netz vergessen"));
        int n = page_count(s_page); if (s_sel[s_page] >= n) s_sel[s_page] = (int8_t)(n > 0 ? n - 1 : 0);
    } break;
    }
}

// ---- inline text input ------------------------------------------------------------------------
// Every printable key types — including , ; . / which double as the arrows (Wi-Fi passwords use them).
static void input_char(char c) { if (c >= 32 && c < 127 && s_ilen < (int)sizeof(s_ibuf) - 1) { s_ibuf[s_ilen++] = c; s_ibuf[s_ilen] = 0; s_ikey_us = esp_timer_get_time(); } }
static void input_close(void) { s_im = IM_NONE; memset(s_ibuf, 0, sizeof s_ibuf); s_ilen = 0; }
static void input_key(int k, char ch)
{
    if (k == NK_DEL) { if (s_ilen > 0) s_ibuf[--s_ilen] = 0; return; }
    if (k != NK_ENTER) { input_char(ch); return; }
    switch (s_im) {
    case IM_PASS:   snprintf(s_join_pass, sizeof s_join_pass, "%s", s_ibuf); input_close(); start_op(OP_JOIN); break;
    case IM_NAME:
        if (!s_ilen) { toast(TR5("Il nome non puo essere vuoto", "The name can't be empty", "El nombre no puede estar vacio",
                                 "Le nom ne peut pas etre vide", "Name darf nicht leer sein")); return; }
        nucleo_setup_set_device_name(s_ibuf); input_close(); toast_ok(TR5("Nome salvato", "Name saved", "Renombrado", "Renomme", "Umbenannt")); break;
    case IM_APSSID:
        if (!s_ilen) { toast(TR5("Il nome non puo essere vuoto", "The name can't be empty", "El nombre no puede estar vacio",
                                 "Le nom ne peut pas etre vide", "Name darf nicht leer sein")); return; }
        nucleo_setup_set_ap_ssid(s_ibuf); input_close();
        toast_ok(TR5("Nome hotspot salvato", "Hotspot name saved", "Hotspot renombrado", "Hotspot renomme", "Hotspot umbenannt")); break;
    case IM_APPASS:                                  // empty = open hotspot; WPA2 needs >= 8 chars
        if (s_ilen && s_ilen < 8) { toast(TR5("Minimo 8 caratteri (o vuota)", "At least 8 chars (or empty)", "Minimo 8 caracteres (o vacia)",
                                              "Au moins 8 car. (ou vide)", "Mind. 8 Zeichen (oder leer)")); return; }
        nucleo_setup_set_ap_pass(s_ibuf);
        toast_ok(s_ilen ? TR5("Password salvata", "Password saved", "Clave guardada", "MdP enregistre", "Passwort gesichert")
                        : TR5("Hotspot aperto", "Hotspot is open", "Hotspot abierto", "Hotspot ouvert", "Hotspot offen"));
        input_close(); break;
    }
}

// ---- search input ----------------------------------------------------------------------
static void search_set(const char *q)
{
    snprintf(s_q, sizeof s_q, "%s", q); s_qn = (int)strlen(s_q);
    search_run(); s_sel[PG_SEARCH] = 0; s_scroll = 0;
}

// ---- on_key -----------------------------------------------------------------------------
// LEFT and Esc never arrive here: the framework routes both to on_back().
static void on_key(int k, char ch)
{
    if (s_onboard == OB_DONE) {                                      // "All set": ENTER opens the launcher
        if (k == NK_ENTER) { s_onboard = OB_NONE; nucleo_app_exit(); }
        return;
    }
    if (s_onboard && s_im == IM_NONE && s_cf == R_NONE && s_page != PG_NETS) set_page(PG_NETS);   // rails
    if (s_im != IM_NONE) { input_key(k, ch); }
    else if (s_cf != R_NONE) { int c = app_ui_confirm_key(k, ch, &s_cf_yes); if (c >= 0) confirm_done(c == 1); }
    else if (s_dt) {
        if (k == NK_UP)          dt_adjust(+1);
        else if (k == NK_DOWN)   dt_adjust(-1);
        else if (k == NK_RIGHT)  dt_step(+1);
        else if (k == NK_ENTER) {
            nucleo_setup_set_datetime(s_dtv[0], s_dtv[1], s_dtv[2], s_dtv[3], s_dtv[4]); s_dt = false;
            toast_ok(TR5("Data e ora impostate", "Date and time set", "Fecha y hora fijadas", "Date et heure reglees", "Datum und Zeit gesetzt"));
        }
    }
    else if (s_busy && (s_op == OP_JOIN || s_op == OP_FINISH)) { return; }
    else if (s_ob_finish_pend) { return; }                         // skip confirmed: waiting for the scan to end
    else {
        if (k != NK_ENTER) s_rst_id = R_NONE;                        // any other key disarms a reset
        // Search: on the root any letter opens it; inside it, printable keys type and DEL erases.
        bool printable = (k == NK_CHAR && ch > ' ' && ch < 127);
        if (s_page == PG_ROOT && printable && isalpha((unsigned char)ch)) {
            char q[2] = { ch, 0 }; s_page = PG_SEARCH; search_set(q);
            update_hint(); nucleo_app_request_draw(); return;
        }
        if (s_page == PG_SEARCH && (printable || (k == NK_CHAR && ch == ' ') || k == NK_DEL)) {
            char q[17]; snprintf(q, sizeof q, "%s", s_q);
            int l = (int)strlen(q);
            if (k == NK_DEL) { if (l) q[l - 1] = 0; }
            else if (l < 16) { q[l] = ch; q[l + 1] = 0; }
            if (!q[0]) go_back(); else search_set(q);
            update_hint(); nucleo_app_request_draw(); return;
        }
        int n = page_count(s_page), sel = cur_sel();
        if (n > 0) {
            if (k == NK_UP || k == NK_DOWN) {
                s_sel[s_page] = (int8_t)((sel + (k == NK_DOWN ? 1 : n - 1)) % n);
                flush_prefs();
            } else if (k == NK_CHAR && ch >= '1' && ch <= '9') {
                int t = ch - '1';
                if (t < n) {
                    s_sel[s_page] = (int8_t)t;
                    if (s_page == PG_ROOT) { Row r; make_row(PG_ROOT, t, r); if (r.kind == K_NAV) activate(r); }   // root: like the launcher
                }
            } else if (s_page == PG_NETS) {
                if (k == NK_ENTER && !s_busy) {
                    if (sel == 0) start_op(OP_SCAN);
                    else {
                        snprintf(s_join_ssid, sizeof s_join_ssid, "%s", nucleo_setup_scan_ssid(sel - 1));
                        // Ask for a password only for a secured network we don't already hold one for.
                        if (nucleo_setup_scan_secure(sel - 1) && !nucleo_setup_net_has_password(s_join_ssid)) open_editor(IM_PASS, "");
                        else { s_join_pass[0] = 0; start_op(OP_JOIN); }
                    }
                } else if (sel > 0 && !s_busy && !s_onboard) {               // forget / prefer: not on first boot
                    const char *ss = nucleo_setup_scan_ssid(sel - 1);
                    if (nucleo_setup_net_is_known(ss)) {
                        if (k == NK_DEL) { snprintf(s_cf_ssid, sizeof s_cf_ssid, "%s", ss); s_cf = R_SAVED_NET; s_cf_yes = false; }
                        else if (ch == 'p' || ch == 'P') {
                            bool pin = prio_of(ss) == 0;
                            nucleo_setup_net_set_priority(ss, pin ? WIFI_PIN_PRIO : 0);
                            toast_ok(pin ? TR5("Preferita: si collega per prima", "Preferred: joined first", "Preferida: se une primero",
                                               "Prefere: rejoint en premier", "Bevorzugt: zuerst verbunden")
                                         : TR5("Priorita normale", "Normal priority", "Prioridad normal", "Priorite normale", "Normale Prioritaet"));
                        }
                    }
                }
            } else {
                Row r; make_row(s_page, sel, r);
                if (k == NK_ENTER) activate(r);
                else if (k == NK_RIGHT) { if (!adjust(r, +1) && r.kind == K_NAV) activate(r); }
                else if (k == NK_DEL && r.id == R_SAVED_NET) {
                    snprintf(s_cf_ssid, sizeof s_cf_ssid, "%s", nucleo_setup_net_ssid(r.num)); s_cf = R_SAVED_NET; s_cf_yes = false;
                }
                else if ((ch == 'p' || ch == 'P') && r.id == R_SAVED_NET) activate(r);
            }
        }
    }
    update_hint(); nucleo_app_request_draw();
}

// ---- back (Esc) / LEFT ----------------------------------------------------------------
// LEFT adjusts the focused slider/choice, otherwise goes back like Esc — but LEFT never closes the app
// (only Esc at the root returns to the launcher).
static bool on_back(int key)
{
    bool left = (key == NK_LEFT);
    if (s_im != IM_NONE)      { if (left) input_char(','); else input_close(); }
    else if (s_cf != R_NONE)  { if (left) app_ui_confirm_key(NK_LEFT, 0, &s_cf_yes); else s_cf = R_NONE; }
    else if (s_dt)            { if (left) dt_step(-1); else s_dt = false; }
    else if (s_busy && (s_op == OP_JOIN || s_op == OP_FINISH)) { /* swallow: it finishes on its own */ }
    else if (s_ob_finish_pend) { /* skip confirmed: waiting for the scan to end */ }
    else if (s_onboard == OB_DONE) { if (left) return true; s_onboard = OB_NONE; return false; }   // Esc: launcher, like ENTER
    else if (s_onboard) { if (!left) { s_cf = R_OB_SKIP; s_cf_yes = false; } }   // first boot: Esc asks "use the hotspot?"
    else {
        s_rst_id = R_NONE;
        Row r;
        bool adjusted = left && focused_row(r) && adjust(r, -1);
        if (!adjusted) {
            if (s_page == PG_ROOT) { if (!left) { flush_prefs(); return false; } }   // Esc at the root: close
            else go_back();
        }
    }
    update_hint(); nucleo_app_request_draw();
    return true;
}

// ---- on_tick (5 Hz) ---------------------------------------------------------------------------
static uint32_t live_sig(void)
{
    uint32_t h = 2166136261u;
    #define MIX(v) do { h = (h ^ (uint32_t)(v)) * 16777619u; } while (0)
    for (const char *p = nucleo_setup_ip(); *p; p++) MIX(*p);
    for (const char *p = nucleo_setup_ssid(); *p; p++) MIX(*p);
    MIX(nucleo_setup_mode()[0]); MIX(squality(nucleo_setup_rssi()) / 10);
    MIX(ap_up()); MIX(ap_on()); MIX(ap_resc());
    MIX(nucleo_setup_scan_count()); MIX(nucleo_setup_net_count()); MIX(nucleo_setup_time_synced());
    MIX(time(NULL) / 60); MIX(nucleo_power_battery_pct()); MIX(suggestion());
    if (s_page == PG_DEVICE) { MIX(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024); MIX(nucleo_auth_session_count()); }
    #undef MIX
    return h;
}

static void on_tick(void)
{
    s_tickn++;
    if (s_msg_t > 0 && --s_msg_t == 0) nucleo_app_request_draw();
    if (s_ikey_us && esp_timer_get_time() - s_ikey_us >= PEEK_US) { s_ikey_us = 0; if (s_im == IM_PASS) nucleo_app_request_draw(); }   // re-mask the peeked char
    if (s_busy) {
        s_anim++;
        if (s_done) {
            s_busy = false; s_done = false; int op = s_op; s_op = OP_NONE;
            if (op == OP_JOIN) {
                memset(s_join_pass, 0, sizeof s_join_pass);
                if (s_join_ok && s_onboard) { update_hint(); onboard_end(); nucleo_app_request_draw(); return; }
                else if (s_join_ok) { toast_ok(TR5("Connesso", "Connected", "Conectado", "Connecte", "Verbunden")); set_page(PG_WIFI); }
                else toast(s_onboard ? TR5("Non riuscita: controlla la password", "Failed: check the password", "Fallo: revisa la clave",
                                           "Echec : verifie le mot de passe", "Fehler: Passwort pruefen")
                                     : TR5("Connessione non riuscita", "Could not connect", "No se pudo conectar", "Echec de connexion", "Verbindung fehlgeschlagen"));
            } else if (op == OP_FINISH) {
                s_onboard = OB_DONE; s_msg_t = 0;                    // "All set"
            } else if (op == OP_SCAN && s_ob_finish_pend) {
                s_ob_finish_pend = false; update_hint(); onboard_end(); nucleo_app_request_draw(); return;
            } else if (op == OP_SCAN && s_page == PG_NETS) {
                s_sel[PG_NETS] = (int8_t)(nucleo_setup_scan_count() > 0 ? 1 : 0);   // focus the strongest network
                s_scroll = 0;
            }
            update_hint();
        }
        nucleo_app_request_draw(); return;
    }
    if (s_onboard == OB_NETS && !nucleo_setup_onboarding()) { onboard_end(); update_hint(); nucleo_app_request_draw(); return; }   // joined from the web
    if (s_tickn % 5 == 0) {                                   // 1 Hz: repaint only when a live value changed
        uint32_t sig = live_sig();
        if (sig != s_live) { s_live = sig; update_hint(); nucleo_app_request_draw(); }
    }
}

// ---- lifecycle -----------------------------------------------------------------------------
static void enter(void)
{
    s_page = PG_ROOT; s_scroll = 0;                           // s_sel[] is kept: resume the last focus
    s_im = IM_NONE; s_cf = R_NONE; s_dt = false; s_rst_id = R_NONE; s_prefs_dirty = false; s_tts_pend = -1;
    s_theme_pend = false; s_saver_t_pend = -1; s_saver_m_pend = -1;
    s_qn = 0; s_q[0] = 0; s_hits = 0; s_tts_ok = -1; s_bt_pref = -1;
    s_msg_t = 0; s_live = 0;
    if (!s_task) { s_busy = false; s_op = OP_NONE; s_done = false; }
    nucleo_app_set_tab_handler(on_tab);
    nucleo_app_set_back_handler(on_back);
    if (s_preset[0]) { s_page = PG_SEARCH; search_set(s_preset); s_preset[0] = 0; }   // opened from launcher Spotlight
    if (s_onboard_req) { s_onboard_req = false; s_onboard = nucleo_setup_onboarding() ? OB_NETS : OB_NONE; if (s_onboard) set_page(PG_NETS); }
    update_hint(); nucleo_app_request_draw();
}
// Ends the network step (a join, or the confirmed skip -> hotspot) and shows "All set".
static void onboard_end(void)
{
    if (s_onboard != OB_NETS) return;
    if (s_busy) { s_ob_finish_pend = (s_op == OP_SCAN); return; }   // a scan owns the radio: finish right after it
    start_op(OP_FINISH);                                            // OOM: start_op says so, the list stays, Esc retries
}
static void leave(void)
{
    s_onboard = OB_NONE; s_ob_finish_pend = false;   // an unfinished step stays pending in nucleo_setup: the launcher re-opens it
    flush_prefs();
    strip_release();
    if (!(s_busy && s_op == OP_JOIN)) wipe(s_join_pass, sizeof s_join_pass);   // an in-flight join wipes its own
    wipe(s_ibuf, sizeof s_ibuf); s_ilen = 0; s_im = IM_NONE;
}

extern "C" void nucleo_register_wifi(void)
{
    static const nucleo_app_def_t app = {
        "wifi", "Impostazioni", "System", "Wi-Fi, hotspot, Bluetooth, display, sound, ANIMA, language, device",
        'W', C_BLUE, enter, on_key, on_tick, on_draw, leave
    };
    nucleo_app_register(&app);
}
