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
#include "nucleo_i18n.h"      // TR(it,en) + the 5-language OS switch
#include <M5GFX.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include <string.h>
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
void        nucleo_setup_start_ap(void);
void        nucleo_setup_stop_ap(void);
void        nucleo_setup_forget(void);
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
    R_NAME, R_PIN, R_SESSIONS, R_MODEL, R_VERSION, R_BATTERY, R_SD, R_RAM, R_UPTIME, R_UPDATES, R_RESTART,  // Device
    R_RST_SOFT, R_RST_HARD,                                                                 // Reset
    R_SAVED_NET, R_FORGET_ALL,                                                              // Saved networks
};

static const uint8_t ROWS_ROOT[]    = { R_S_WIFI, R_S_AP, R_S_BT, R_S_DISPLAY, R_S_SOUND, R_ANIMA, R_LANG, R_DATETIME, R_S_DEVICE, R_S_RESET };
static const uint8_t ROWS_WIFI[]    = { R_NETS, R_SAVED, R_HANDOFF };
static const uint8_t ROWS_AP[]      = { R_AP_ON, R_AP_STATE, R_AP_SSID, R_AP_PASS, R_AP_ADDR };
static const uint8_t ROWS_BT[]      = { R_BT_BOOT, R_BT_STATE, R_RESTART };
static const uint8_t ROWS_DISPLAY[] = { R_BRIGHT, R_THEME, R_SAVER_TIME, R_SAVER_STYLE };
static const uint8_t ROWS_SOUND[]   = { R_VOLUME, R_MUTE, R_TTS, R_TTS_SPEED, R_VOICE };
static const uint8_t ROWS_DEVICE[]  = { R_NAME, R_PIN, R_SESSIONS, R_MODEL, R_VERSION, R_BATTERY, R_SD, R_RAM, R_UPTIME, R_UPDATES, R_RESTART };
static const uint8_t ROWS_RESET[]   = { R_RST_SOFT, R_RST_HARD };
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
static int64_t s_adj_us = 0;                   // last slider step (hold-to-accelerate)
static int64_t s_cyc_us = 0;                   // last choice step (held arrows throttled: each step persists)
static int8_t  s_tts_ok = -1, s_bt_pref = -1;  // cached per visit: the TTS probe opens an SD index and the
                                               // BLE pref is an NVS read — never per painted frame

static char    s_q[17]; static int s_qn = 0;   // search query
static uint8_t s_hit_pg[18], s_hit_id[18];     // search results (page, row id)
static int     s_hits = 0;

enum { IM_NONE = 0, IM_PASS, IM_NAME, IM_APSSID, IM_APPASS };
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

enum { OP_NONE = 0, OP_SCAN, OP_JOIN };
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
    if (sec <= 0)           snprintf(b, cap, "%s", TR("Mai", "Never"));
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
    nucleo_theme_set(all[(idx + dir + cnt) % cnt].id);
}

static const int SAVER_STEPS[] = { 0, 15, 30, 60, 120, 300, 600 };
static void saver_time_cycle(int dir)
{
    int cur = nucleo_screensaver_timeout_s(), n = NROWS(SAVER_STEPS), idx = -1;
    if (dir > 0) { for (int i = 0; i < n; i++) if (SAVER_STEPS[i] > cur) { idx = i; break; } if (idx < 0) idx = 0; }
    else         { for (int i = n - 1; i >= 0; i--) if (SAVER_STEPS[i] < cur) { idx = i; break; } if (idx < 0) idx = n - 1; }
    nucleo_screensaver_set_timeout_s(SAVER_STEPS[idx]);
}
static const char *saver_style_name(int m)
{
    switch (m) {
        case 0:  return TR("Schermo nero", "Blank screen");
        case 2:  return TR("Stelle", "Stars");
        case 3:  return TR("Fuoco", "Fire");
        default: return TR("Orologio", "Clock");
    }
}
static const char *anima_mode_name(int m) { return m == 0 ? "Offline" : m == 2 ? TR("Solo online", "Online only") : TR("Ibrida", "Hybrid"); }
static const char *anima_mode_sub(int m)
{
    return m == 0 ? TR("Solo conoscenza sul device", "Knowledge on the device only")
         : m == 2 ? TR("Sempre il modello cloud", "Always the cloud model")
         :          TR("Prima offline, poi il cloud", "Offline first, then cloud");
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
        case PG_DEVICE:  *n = NROWS(ROWS_DEVICE);  return ROWS_DEVICE;
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
        case PG_DISPLAY: return TR("Schermo", "Display");
        case PG_SOUND:   return TR("Suono", "Sound");
        case PG_DEVICE:  return TR("Dispositivo", "Device");
        case PG_RESET:   return TR("Ripristino", "Reset");
        case PG_NETS:    return TR("Reti vicine", "Nearby networks");
        case PG_SAVED:   return TR("Reti salvate", "Saved networks");
        default:         return TR("Impostazioni", "Settings");
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
        if (sg == SG_CLOCK)     { r.label = TR("Imposta l'ora", "Set the clock");     r.glyph = UG_CLOCK;   sset(r, TR("L'orologio non e mai stato impostato", "The clock was never set")); }
        else if (sg == SG_BATT) { r.label = TR("Batteria bassa", "Battery low");      r.glyph = UG_BATTERY; sset(r, TR("Invio: luminosita al 40%", "Enter: brightness to 40%")); }
        else                    { r.label = TR("Collegati al Wi-Fi", "Join Wi-Fi");   r.glyph = UG_WIFI;    sset(r, TR("Nessuna rete: scegline una", "No network: pick one")); }
    } break;
    // -- root: sections with a preview of their value
    case R_S_WIFI:
        r.label = "Wi-Fi"; r.glyph = UG_WIFI;
        if (connected())  { vset(r, nucleo_setup_ssid()); r.col = GOOD;
                            snprintf(r.sub, sizeof r.sub, "%.16s  %d dBm", nucleo_setup_ssid(), nucleo_setup_rssi()); }
        else if (ap_up()) { vset(r, "Hotspot"); r.col = WARN; sset(r, TR("Solo hotspot attivo", "Hotspot only")); }
        else              { vset(r, TR("Offline", "Offline")); r.col = MUTED; }
        break;
    case R_S_AP:
        r.label = "Hotspot"; r.glyph = UG_HOTSPOT;
        if (ap_on())        { vset(r, "On"); r.col = WARN; snprintf(r.sub, sizeof r.sub, "%.18s  192.168.4.1", nucleo_setup_ap_ssid()); }
        else if (ap_resc()) { vset(r, TR("Soccorso", "Rescue")); r.col = WARN; sset(r, TR("Temporaneo, finche torna la rete", "Temporary, until Wi-Fi returns")); }
        else                { vset(r, "Off"); r.col = MUTED; }
        break;
    case R_S_BT: {
        bool pref = bt_pref(), live = nucleo_ble_radio_present();
        r.label = "Bluetooth"; r.glyph = UG_BLUETOOTH;
        vset(r, pref ? "On" : "Off"); r.col = pref ? ACC : MUTED;
        if (pref != live) sset(r, TR("Cambia al prossimo riavvio", "Changes at the next restart"));
    } break;
    case R_S_DISPLAY:
        r.label = TR("Schermo", "Display"); r.glyph = UG_SUN;
        snprintf(r.val, sizeof r.val, "%d%%", nucleo_app_brightness());
        snprintf(r.sub, sizeof r.sub, "%d%%  %s", nucleo_app_brightness(), theme_name());
        break;
    case R_S_SOUND:
        r.label = TR("Suono", "Sound"); r.glyph = muted ? UG_MUTE : UG_SPEAKER;
        if (muted) { vset(r, TR("Muto", "Muted")); r.col = BAD; }
        else snprintf(r.val, sizeof r.val, "%d%%", nucleo_audio_volume());
        break;
    case R_ANIMA: {
        int m = nucleo_anima_ui_online_mode();
        r.label = "ANIMA"; r.glyph = UG_STAR; r.kind = K_CYCLE; vset(r, anima_mode_name(m)); sset(r, anima_mode_sub(m));
    } break;
    case R_LANG: r.label = TR("Lingua", "Language"); r.glyph = UG_GLOBE; r.kind = K_CYCLE; vset(r, LANGS[lang_idx()].name); break;
    case R_DATETIME: {
        r.label = TR("Data e ora", "Date/time"); r.glyph = UG_CLOCK;
        time_t now = time(NULL); struct tm tmv; localtime_r(&now, &tmv);
        if (now >= DT_EPOCH_2023) {
            strftime(r.val, sizeof r.val, "%H:%M", &tmv);
            strftime(r.sub, sizeof r.sub, "%d/%m/%Y  %H:%M", &tmv);
            if (nucleo_setup_time_synced()) snprintf(r.sub + strlen(r.sub), sizeof r.sub - strlen(r.sub), "  NTP");
        } else { vset(r, TR("da impostare", "not set")); r.col = WARN; }
    } break;
    case R_S_DEVICE: r.label = TR("Dispositivo", "Device"); r.glyph = UG_CHIP; vset(r, nucleo_setup_device_name()); r.col = MUTED;
                     snprintf(r.sub, sizeof r.sub, "%.14s  PIN %s", nucleo_setup_device_name(), nucleo_auth_pin()); break;
    case R_S_RESET:  r.label = TR("Ripristino", "Reset"); r.glyph = UG_RESET; sset(r, TR("Azzera impostazioni o tutto", "Reset settings or everything")); break;
    // -- Wi-Fi
    case R_NETS: { r.label = TR("Reti vicine", "Nearby networks"); int c = nucleo_setup_scan_count();
                   if (c) { snprintf(r.val, sizeof r.val, "%d", c); }
                   sset(r, TR("Cerca e collegati", "Scan and join")); } break;
    case R_SAVED:   r.label = TR("Reti salvate", "Saved networks"); snprintf(r.val, sizeof r.val, "%d", nucleo_setup_net_count());
                    sset(r, TR("Preferite e da dimenticare", "Prefer or forget")); break;
    case R_HANDOFF: r.label = TR("Passaggio web", "Web handoff"); r.kind = K_TOGGLE; r.on = nucleo_remote_enabled();
                    sset(r, TR("Il browser prende lo schermo", "The browser takes the screen")); break;
    // -- Hotspot
    case R_AP_ON:    r.label = "Hotspot"; r.kind = K_TOGGLE; r.on = ap_on();
                     sset(r, r.on ? TR("Acceso: 192.168.4.1", "On: 192.168.4.1") : TR("Spento: usi la tua rete", "Off: using your Wi-Fi")); break;
    case R_AP_STATE:
        r.label = TR("Stato", "Status"); r.kind = K_INFO;
        if (!ap_up())       { vset(r, TR("Spento", "Off")); r.col = MUTED; }
        else if (ap_resc()) { vset(r, TR("Soccorso", "Rescue")); r.col = WARN; }
        else                { vset(r, TR("Attivo", "Active")); r.col = GOOD; }
        break;
    case R_AP_SSID: r.label = TR("Nome", "Name"); r.kind = K_EDIT; vset(r, nucleo_setup_ap_ssid()); break;
    case R_AP_PASS: r.label = "Password"; r.kind = K_EDIT;
                    vset(r, nucleo_setup_ap_secure() ? nucleo_setup_ap_pass() : TR("(aperta)", "(open)")); break;
    case R_AP_ADDR: r.label = TR("Indirizzo", "Address"); r.kind = K_INFO; vset(r, "192.168.4.1"); break;
    // -- Bluetooth (the radio's RAM is only reserved when it's on at boot)
    case R_BT_BOOT: r.label = "Bluetooth"; r.kind = K_TOGGLE; r.on = bt_pref();
                    sset(r, r.on ? TR("Pronto per le app BLE", "Ready for BLE apps") : TR("Spento: piu RAM per il resto", "Off: more RAM for everything else")); break;
    case R_BT_STATE: r.label = TR("Adesso", "Right now"); r.kind = K_INFO;
                     if (nucleo_ble_radio_present()) { vset(r, TR("Radio attiva", "Radio on")); r.col = GOOD; }
                     else                            { vset(r, TR("Radio spenta", "Radio off")); r.col = MUTED; }
                     if (bt_pref() != nucleo_ble_radio_present()) sset(r, TR("Riavvia per applicare", "Restart to apply"));
                     break;
    // -- Display
    case R_BRIGHT: r.label = TR("Luminosita", "Brightness"); r.kind = K_SLIDER; r.num = (int16_t)nucleo_app_brightness(); r.lo = 10; r.hi = 100; r.col = WARN; break;
    case R_THEME:  r.label = TR("Tema", "Theme"); r.kind = K_CYCLE; vset(r, theme_name()); break;
    case R_SAVER_TIME: r.label = TR("Salvaschermo", "Screensaver"); r.kind = K_CYCLE;
                       fmt_dur(b, sizeof b, nucleo_screensaver_timeout_s()); vset(r, b);
                       sset(r, TR("Dopo inattivita, dal menu", "After idle, from the menu")); break;
    case R_SAVER_STYLE: r.label = TR("Stile", "Style"); r.kind = K_CYCLE; vset(r, saver_style_name(nucleo_screensaver_mode())); break;
    // -- Sound
    case R_VOLUME: r.label = "Volume"; r.kind = K_SLIDER; r.num = (int16_t)nucleo_audio_volume(); r.lo = 0; r.hi = 100; r.col = muted ? DIM : GOOD; break;
    case R_MUTE:   r.label = TR("Muto", "Mute"); r.kind = K_TOGGLE; r.on = muted;
                   sset(r, muted ? TR("Nessun suono", "No sound") : TR("Audio attivo", "Sound on")); break;
    case R_TTS:    r.label = TR("Lettura vocale", "Read aloud"); r.kind = K_TOGGLE; r.on = nucleo_tts_enabled(); r.dis = !tts_ok();
                   sset(r, r.dis ? TR("Pacchetto voce assente su SD", "Voice pack missing on SD") : TR("Le risposte vengono lette", "Answers are spoken")); break;
    case R_TTS_SPEED: r.label = TR("Velocita voce", "Voice speed"); r.kind = K_SLIDER; r.num = (int16_t)nucleo_tts_speed(); r.lo = 70; r.hi = 160;
                      r.col = ACC; r.dis = !tts_ok(); break;
    case R_VOICE:  r.label = TR("Ascolto sempre", "Always listen"); r.kind = K_TOGGLE; r.on = nucleo_voice_always_on();
                   sset(r, TR("Parola chiave pronta (16 KB RAM)", "Wake word ready (16 KB RAM)")); break;
    // -- Device
    case R_NAME: r.label = TR("Nome", "Name"); r.kind = K_EDIT; vset(r, nucleo_setup_device_name()); break;
    case R_PIN: { const char *p = nucleo_auth_pin(); r.label = "PIN"; r.kind = K_INFO; vset(r, p[0] ? p : "------"); r.col = GOOD; r.vsz = 2;
                  sset(r, TR("Per collegare un browser", "To pair a browser")); } break;
    case R_SESSIONS: { int n = nucleo_auth_session_count(); r.label = TR("Sessioni web", "Web sessions");
                       r.kind = n > 0 ? K_DANGER : K_INFO; snprintf(r.val, sizeof r.val, "%d", n);
                       if (n > 0) { sset(r, TR("Invio: disconnetti tutti", "Enter: sign everyone out")); }
                       r.col = n ? WARN : MUTED; } break;
    case R_MODEL:   r.label = TR("Modello", "Model"); r.kind = K_INFO; vset(r, nucleo_ui_is_adv() ? "Cardputer ADV" : "Cardputer"); r.col = FG; break;
    case R_VERSION: r.label = TR("Versione", "Version"); r.kind = K_INFO; vset(r, esp_app_get_description()->version); r.col = FG; break;
    case R_BATTERY: {
        int p = nucleo_power_battery_pct(), mv = nucleo_power_battery_mv();
        r.label = TR("Batteria", "Battery"); r.kind = K_INFO;
        if (p < 0) { vset(r, TR("n/d", "n/a")); r.col = MUTED; }
        else { snprintf(r.val, sizeof r.val, "%d%%", p); r.col = p < 20 ? BAD : p < 50 ? WARN : GOOD;
               if (mv > 0) snprintf(r.sub, sizeof r.sub, "%d%%  %d.%02d V", p, mv / 1000, (mv % 1000) / 10); }
    } break;
    case R_SD: {
        r.label = "SD"; r.kind = K_INFO; r.col = FG;
        const nucleo_storage_info_t *st = nucleo_storage_info();
        if (st && st->mounted) {
            uint32_t f = (uint32_t)(st->free_bytes / (1024 * 1024)), t = (uint32_t)(st->total_bytes / (1024 * 1024));
            if (t >= 1000) snprintf(r.val, sizeof r.val, TR("%.1f GB liberi", "%.1f GB free"), f / 1024.0f);
            else           snprintf(r.val, sizeof r.val, TR("%u MB liberi", "%u MB free"), (unsigned)f);
            if (t >= 1000) snprintf(r.sub, sizeof r.sub, TR("%.1f di %.1f GB liberi", "%.1f of %.1f GB free"), f / 1024.0f, t / 1024.0f);
        } else { vset(r, TR("assente", "none")); r.col = BAD; }
    } break;
    case R_RAM:
        r.label = TR("RAM libera", "Free RAM"); r.kind = K_INFO; r.col = FG;
        snprintf(r.val, sizeof r.val, "%u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024));
        snprintf(r.sub, sizeof r.sub, TR("%u KB, blocco max %u KB", "%u KB, largest block %u KB"),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) / 1024));
        break;
    case R_UPTIME: {
        int64_t s = esp_timer_get_time() / 1000000; r.label = TR("Acceso da", "Uptime"); r.kind = K_INFO; r.col = FG;
        if (s < 3600) snprintf(r.val, sizeof r.val, "%d min", (int)(s / 60));
        else          snprintf(r.val, sizeof r.val, "%dh %02dm", (int)(s / 3600), (int)(s % 3600 / 60));
    } break;
    case R_UPDATES: r.label = TR("Aggiornamenti", "Updates"); r.glyph = UG_UPDATE; sset(r, TR("Cerca una nuova versione", "Check for a new version")); break;
    case R_RESTART: r.label = TR("Riavvia", "Restart"); r.kind = K_DANGER; sset(r, TR("Chiude tutto e riparte", "Closes everything and restarts")); break;
    // -- Reset (armed rows show how many ENTER presses are left)
    case R_RST_SOFT: case R_RST_HARD:
        r.label = (id == R_RST_SOFT) ? TR("Azzera config", "Reset settings") : TR("Reset totale", "Factory reset");
        r.kind = K_DANGER;
        if (s_rst_id == id) { snprintf(r.val, sizeof r.val, "x%d", s_rst_left + 1);
                              snprintf(r.sub, sizeof r.sub, TR("Invio ancora %d volte", "ENTER %d more times"), s_rst_left); }
        else sset(r, id == R_RST_SOFT ? TR("Rete, preferenze, log. File salvi", "Network, prefs, logs. Files kept")
                                      : TR("Anche chiavi e dati ANIMA", "Also keys and ANIMA data"));
        break;
    // -- Saved networks
    case R_SAVED_NET: {
        const char *ss = nucleo_setup_net_ssid(num);
        r.kind = K_INFO; app_ui_ascii_fold(ss, r.val, sizeof r.val); r.label = r.val;   // the SSID IS the label
        r.on = nucleo_setup_net_priority(num) > 0;
        bool cur = connected() && !strcmp(ss, nucleo_setup_ssid());
        r.col = cur ? GOOD : FG;
        sset(r, cur ? (r.on ? TR("In uso, preferita", "In use, preferred") : TR("In uso", "In use"))
                    : (r.on ? TR("Preferita: si collega per prima", "Preferred: joined first") : TR("Salvata", "Saved")));
    } break;
    case R_FORGET_ALL: r.label = TR("Dimentica tutte", "Forget all"); r.kind = K_DANGER;
                       snprintf(r.sub, sizeof r.sub, TR("%d reti e password", "%d networks and passwords"), nucleo_setup_net_count()); break;
    }
}

// Row `i` of page `pg`.
static void make_row(int pg, int i, Row &r)
{
    if (pg == PG_SAVED) { int k = nucleo_setup_net_count(); make_row_id(i < k ? R_SAVED_NET : R_FORGET_ALL, i, r); return; }
    if (pg == PG_SEARCH) {
        if (i < 0 || i >= s_hits) { memset(&r, 0, sizeof r); r.label = ""; return; }
        make_row_id(s_hit_id[i], 0, r);
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
static void search_run(void)
{
    s_hits = 0;
    static const uint8_t PAGES[] = { PG_ROOT, PG_WIFI, PG_AP, PG_BT, PG_DISPLAY, PG_SOUND, PG_DEVICE, PG_RESET };
    for (int p = 0; p < NROWS(PAGES) && s_hits < NROWS(s_hit_pg); p++) {
        int pg = PAGES[p], n; const uint8_t *ids = page_rows(pg, &n);
        for (int i = 0; i < n && s_hits < NROWS(s_hit_pg); i++) {
            if (pg == PG_ROOT && section_of(ids[i]) < 0 && ids[i] != R_ANIMA && ids[i] != R_LANG && ids[i] != R_DATETIME) continue;
            Row r; make_row_id(ids[i], i, r);
            if (ci_contains(r.label, s_q) || (pg != PG_ROOT && ci_contains(page_title(pg), s_q))) {
                s_hit_pg[s_hits] = (uint8_t)pg; s_hit_id[s_hits] = ids[i]; s_hits++;
            }
        }
    }
}

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
    char cnt[8], rb[8]; snprintf(cnt, sizeof cnt, "%d", s_hits); snprintf(rb, sizeof rb, "%5.5s", cnt);
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
    d.fillCircle(on ? x + sw - sh / 2 - 1 : x + sh / 2 + 1, cy, sh / 2 - 2, on ? INK : (onchip ? FG : MUTED));
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
    uint16_t pill = r.dis ? LINE : danger ? BAD : ACC, ink = r.dis ? MUTED : INK;
    d.fillRoundRect(3, y, W - 8, ROW_F - 1, 9, pill);
    if (icon) ui_glyph(&d, r.glyph, 14, y + 9, 6, ink, pill);
    int top_vx = rx;                                                        // right edge left for the name
    if (r.kind == K_TOGGLE) { draw_switch(rx, y + 9, r.on && !r.dis, true); top_vx = rx - 30; }
    else if (r.kind == K_NAV || (danger && r.id != R_RST_SOFT && r.id != R_RST_HARD)) { ui_glyph(&d, UG_NEXT, rx - 2, y + 9, 5, ink, pill); top_vx = rx - 12; }
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
            txt(10, top + 14, TR("Nessuna impostazione con", "No setting matches"), MUTED, BG, 1);
            char q[26]; snprintf(q, sizeof q, "\"%.20s\"", s_q); f2(10, top + 28, q, W - 20, FG, BG, false);
            txt(10, top + 52, TR("canc cancella   esc chiude", "del erase   esc closes"), DIM, BG, 1);
        } else {                                                     // only PG_SAVED can be empty
            txt(10, top + 12, TR("Nessuna rete salvata.", "No saved networks."), FG, BG, 1);
            txt(10, top + 26, TR("Unisciti a una da Reti vicine:", "Join one from Nearby networks:"), MUTED, BG, 1);
            txt(10, top + 38, TR("la password resta salvata qui.", "its password is kept here."), MUTED, BG, 1);
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
        f2(x, top + 19, ap_resc() ? TR("192.168.4.1  soccorso", "192.168.4.1  rescue") : "192.168.4.1  hotspot", mw, FG, BG, false);
    } else {
        txt_fit(x, top + 2, TR("Non connesso", "Not connected"), mw, MUTED, BG, 2);
        f2(x, top + 19, TR("Scegli una rete qui sotto", "Pick a network below"), mw, MUTED, BG, false);
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
        txt(26, y + 2, nucleo_setup_scan_count() ? TR("Cerca di nuovo", "Scan again") : TR("Cerca reti", "Scan"), ink, bg, 2);
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
    char name[34]; app_ui_ascii_fold(ss[0] ? ss : TR("(nascosta)", "(hidden)"), name, sizeof name);
    txt_fit(38, y + 2, name, W - 38 - 22, foc ? INK : (cur ? GOOD : FG), bg, 2);
    if (pref)       ui_glyph(&d, UG_STAR, W - 15, y + 9, 5, foc ? INK : GOOD, bg);
    else if (known) d.fillCircle(W - 15, y + 9, 3, foc ? INK : ACC);
    if (foc) {
        char det[48];
        snprintf(det, sizeof det, "%s  ch%d  %d dBm%s", nucleo_setup_scan_auth_label(k), nucleo_setup_scan_channel(k), rssi,
                 cur ? TR("  in uso", "  in use") : known ? TR("  salvata", "  saved") : "");
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
        if (s_full) { const char *m = TR("Cerco le reti...", "Scanning..."); txt(W / 2 - (int)strlen(m) * 6, top + 60, m, FG, BG, 2); }
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
        f2(10, top + ROW_F + 10, TR("Nessuna rete trovata.", "No networks found."), W - 20, MUTED, BG, false);
        txt(10, top + ROW_F + 30, TR("Invio sulla riga sopra per riprovare.", "ENTER on the row above to retry."), DIM, BG, 1);
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
        case 0: if (*v < 2020) *v = 2020; if (*v > 2099) *v = 2099; break;
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
    if (now >= DT_EPOCH_2023 && tmv.tm_year + 1900 >= 2020) {
        s_dtv[0] = tmv.tm_year + 1900; s_dtv[1] = tmv.tm_mon + 1; s_dtv[2] = tmv.tm_mday;
        s_dtv[3] = tmv.tm_hour;        s_dtv[4] = tmv.tm_min;
    } else { s_dtv[0] = 2026; s_dtv[1] = 1; s_dtv[2] = 1; s_dtv[3] = 12; s_dtv[4] = 0; }
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
    draw_header(TR("Data e ora", "Date/time"), nucleo_setup_time_synced() ? "NTP" : NULL, true);
    char b[8];
    int dw = 40, mw = 40, yw = 64, g = 14, x = (W - (dw + mw + yw + 2 * g)) / 2, y = 37, bh = 26;
    int hw = 56, g2 = 20, tx = (W - (2 * hw + g2)) / 2, ty = 84, th = 32;
    if (s_full) {                                                    // captions + separators: static
        f2(8, 20, TR("Giorno / mese / anno", "Day / month / year"), W - 16, MUTED, BG, false);
        txt(x + dw + 1, y + 5, "/", MUTED, BG, 2);
        txt(x + dw + g + mw + 1, y + 5, "/", MUTED, BG, 2);
        f2(8, 66, TR("Ore : minuti", "Hours : minutes"), W - 16, MUTED, BG, false);
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
                    : s_im == IM_APSSID ? TR("Nome rete", "Network")
                    : s_im == IM_APPASS ? "Password"
                    :                     TR("Nome", "Name");
    if (touched(iy, iy + 25)) {                                      // frame: first paint, or rows repainted under it
        d.fillRoundRect(4, iy, W - 8, 25, 7, LINE);
        d.drawRoundRect(4, iy, W - 8, 25, 7, ACC);
        txt(11, iy + 9, lab, ACC, LINE, 1);
    }
    bool mask = (s_im == IM_PASS);                                   // the join password stays hidden
    int vx = 11 + (int)strlen(lab) * 6 + 8, maxc = (W - 14 - vx) / 6 - 1;
    if (maxc > 36) maxc = 36;
    int from = s_ilen > maxc ? s_ilen - maxc : 0, k = 0;
    char sh[40]; for (int i = from; i < s_ilen && k < maxc; i++, k++) sh[k] = mask ? '*' : s_ibuf[i];
    for (int j = k; j <= maxc; j++) sh[j] = ' ';                    // pad: the field overwrites itself
    sh[maxc + 1] = 0;
    txt(vx, iy + 9, sh, FG, LINE, 1);
    d.fillRect(vx + k * 6 + 1, iy + 9, 2, 8, ACC);                   // caret (inside the text cell)
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
    const char *m = TR("Connessione a", "Connecting to");
    f2(W / 2 - 50, 68, m, 100, MUTED, BG, false);
    char name[34]; app_ui_ascii_fold(s_join_ssid, name, sizeof name);
    char b[20]; snprintf(b, sizeof b, "%.18s", name);
    txt(W / 2 - (int)strlen(b) * 6, 88, b, FG, BG, 2);
}

// ---- on_draw -------------------------------------------------------------------------------
// What forces a FULL paint on the direct path: anything that changes the layout of the whole screen.
static uint32_t scene_sig(void)
{
    uint32_t h = fnv(fnv(1u, s_page), s_dt ? 1u : 0u);
    h = fnv(h, (s_busy && s_op == OP_JOIN) ? 1u : 0u);
    h = fnv(h, (s_page == PG_NETS && s_busy && s_op == OP_SCAN) ? 1u : 0u);
    h = fnv(fnv(h, s_im != IM_NONE ? 1u : 0u), s_cf);
    h = fnv(fnv(fnv(h, THEME_BG), THEME_ACC), THEME_FG);
    h = fnv(h, nucleo_i18n_gen());
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
    if (s_dt)                            draw_datetime();
    else if (s_busy && s_op == OP_JOIN)  draw_joining();
    else if (s_page == PG_NETS)          draw_nets(ch);
    else                                 draw_page(ch);
    if (s_im != IM_NONE) draw_input(ch);
    uint32_t ov = fnv(s_cf, s_cf_yes ? 1u : 2u);
    if (s_cf != R_NONE && (touched(15, 106) || ov != s_ovl_sig)) {     // confirm card (y 15..105)
        s_ovl_sig = ov; mark(15, 106);
        char msg[36];
        switch (s_cf) {
        case R_RESTART: app_ui_confirm(TR("Riavviare?", "Restart?"), TR("Il dispositivo si riavvia ora.", "The device restarts now."), s_cf_yes); break;
        case R_FORGET_ALL:
            snprintf(msg, sizeof msg, TR("%d reti e le loro password.", "%d networks and passwords."), nucleo_setup_net_count());
            app_ui_confirm(TR("Dimenticare tutte?", "Forget all?"), msg, s_cf_yes); break;
        case R_AP_ON:
            if (ap_on()) app_ui_confirm(TR("Spegnere hotspot?", "Hotspot off?"), TR("Chi e collegato perde la rete.", "Connected devices will drop."), s_cf_yes);
            else         app_ui_confirm(TR("Accendere hotspot?", "Hotspot on?"), TR("Il Wi-Fi attuale si disconnette.", "Your Wi-Fi link will drop."), s_cf_yes);
            break;
        case R_SESSIONS: app_ui_confirm(TR("Disconnettere?", "Sign out all?"), TR("I browser rifaranno il pairing.", "Browsers must pair again."), s_cf_yes); break;
        default:
            app_ui_ascii_fold(s_cf_ssid, msg, sizeof msg);
            app_ui_confirm(TR("Dimenticare rete?", "Forget network?"), msg, s_cf_yes);
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
    if (s_im != IM_NONE)          { nucleo_app_set_hint(TR("invio salva   esc annulla", "enter save   esc cancel")); return; }
    if (s_cf != R_NONE)           { nucleo_app_set_hint(TR("</> scegli   invio conferma", "</> pick   enter confirm")); return; }
    if (s_dt)                     { nucleo_app_set_hint(TR("</> campo   su/giu valore   invio salva", "</> field   up/dn value   enter save")); return; }
    if (s_busy && s_op == OP_JOIN){ nucleo_app_set_hint(TR("connessione in corso...", "connecting...")); return; }
    const char *back = (s_page == PG_ROOT) ? TR("esc esci", "esc back") : s_page == PG_SEARCH ? TR("esc chiude", "esc close") : TR("esc indietro", "esc back");
    if (s_page == PG_NETS) {
        int sel = cur_sel();
        if (sel == 0 || (s_busy && s_op == OP_SCAN)) { snprintf(hb, sizeof hb, "%s   %s", TR("invio cerca", "enter scan"), back); nucleo_app_set_hint(hb); return; }
        bool known = nucleo_setup_net_is_known(nucleo_setup_scan_ssid(sel - 1));
        if (known) nucleo_app_set_hint(TR("invio connetti   p pref   canc elimina", "enter join   p prefer   del forget"));
        else { snprintf(hb, sizeof hb, "%s   %s", TR("invio connetti", "enter join"), back); nucleo_app_set_hint(hb); }
        return;
    }
    Row r;
    if (!focused_row(r)) { nucleo_app_set_hint(s_page == PG_SEARCH ? TR("digita   canc cancella   esc chiude", "type   del erase   esc close") : back); return; }
    if (r.id == R_SAVED_NET) { nucleo_app_set_hint(TR("invio preferita   canc elimina", "enter prefer   del forget")); return; }
    if (s_rst_id != R_NONE && r.id == s_rst_id) {
        snprintf(hb, sizeof hb, TR("invio ancora %d   altro tasto annulla", "enter %d more   other key cancels"), s_rst_left);
        nucleo_app_set_hint(hb); return;
    }
    const char *verb;
    switch (r.kind) {
        case K_TOGGLE: verb = TR("invio on/off", "enter on/off"); break;
        case K_SLIDER: verb = TR("</> regola", "</> adjust"); break;
        case K_CYCLE:  verb = TR("</> cambia", "</> change"); break;
        case K_EDIT:   verb = TR("invio modifica", "enter edit"); break;
        case K_INFO:   verb = TR("su/giu scegli", "up/dn pick"); break;
        case K_DANGER: verb = (r.id == R_RST_SOFT || r.id == R_RST_HARD) ? TR("invio 3 volte", "enter 3 times")
                                                                         : TR("invio conferma", "enter confirm"); break;
        default:       verb = TR("invio apri", "enter open"); break;
    }
    if (s_page == PG_ROOT && r.kind == K_NAV) snprintf(hb, sizeof hb, "%s   %s   %s", verb, TR("digita cerca", "type to find"), back);
    else snprintf(hb, sizeof hb, "%s   %s", verb, back);
    nucleo_app_set_hint(hb);
}

// ---- worker (scan / join off the UI loop) ------------------------------------------------
static void wifi_task(void *)
{
    if (s_op == OP_SCAN)      nucleo_setup_scan();
    else if (s_op == OP_JOIN) s_join_ok = nucleo_setup_join(s_join_ssid, s_join_pass);
    s_done = true; s_task = nullptr; vTaskDelete(nullptr);
}
static void start_op(int op)
{
    if (s_busy) return;
    s_op = op; s_busy = true; s_done = false; s_anim = 0;
    // 6 KB like the Wi-Fi supervisor: a successful join persists networks + setup (cJSON + LittleFS +
    // NVS + SD FAT writes) on this task; 4 KB was too tight for that call chain.
    if (xTaskCreate(wifi_task, "wifi", 6144, nullptr, tskIDLE_PRIORITY + 2, &s_task) != pdPASS) {
        s_busy = false; s_task = nullptr; toast(TR("Memoria insufficiente, riprova", "Out of memory, try again"));
    }
}

// ---- navigation ------------------------------------------------------------------------
static void flush_prefs(void) { if (s_prefs_dirty) { s_prefs_dirty = false; nucleo_app_persist_prefs(); } }

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
    if (s_im != IM_NONE || s_cf != R_NONE || s_dt || (s_busy && s_op == OP_JOIN)) return;
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
}

// LEFT/RIGHT on a slider or a choice. Returns false when the row isn't adjustable — toggles are
// deliberately NOT here: a stray LEFT meant as "back" must never switch the hotspot off. Held arrows
// accelerate (the key repeats every ~90 ms): a crown that spins faster the longer you turn it.
static bool adjust(const Row &r, int dir)
{
    if (r.dis) { toast(r.sub[0] ? r.sub : TR("Non disponibile", "Not available")); return true; }
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
        case R_TTS_SPEED: s_adj_us = now; nucleo_tts_set_speed(nucleo_tts_speed() + dir * step); return true;
        case R_LANG:      lang_cycle(dir);  return true;
        case R_THEME:     theme_cycle(dir); return true;
        case R_ANIMA:     nucleo_anima_ui_set_online_mode((nucleo_anima_ui_online_mode() + dir + 3) % 3); return true;
        case R_SAVER_TIME:  saver_time_cycle(dir); return true;
        case R_SAVER_STYLE: nucleo_screensaver_set_mode((nucleo_screensaver_mode() + dir + 4) % 4); return true;
        default: return false;
    }
}

// ENTER on a switch row. The hotspot switch drops/joins the Wi-Fi link, so it asks first (confirm card,
// same rule as the Control Center's arm-then-fire tile).
static void toggle(const Row &r)
{
    if (r.dis) { toast(r.sub[0] ? r.sub : TR("Non disponibile", "Not available")); return; }
    bool on = !r.on;
    switch (r.id) {
        case R_HANDOFF: nucleo_remote_set_enabled(on); toast_ok(on ? TR("Passaggio web attivo", "Web handoff on") : TR("Passaggio web spento", "Web handoff off")); break;
        case R_MUTE:    nucleo_audio_set_mute(on); nucleo_app_persist_prefs(); break;
        case R_TTS:     nucleo_tts_set_enabled(on); toast_ok(on ? TR("Lettura vocale attiva", "Read aloud on") : TR("Lettura vocale spenta", "Read aloud off")); break;
        case R_VOICE:   nucleo_voice_set_always_on(on); break;
        case R_BT_BOOT: nucleo_ble_set_pref(on); s_bt_pref = on ? 1 : 0; toast_ok(TR("Vale dal prossimo riavvio", "Applies after a restart")); break;
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

static void activate(const Row &r)
{
    int sec = section_of(r.id);
    if (sec >= 0) { s_qn = 0; s_q[0] = 0; open_section(sec); return; }
    switch (r.id) {
    case R_SUGGEST: {
        int sg = suggestion();
        if (sg == SG_CLOCK)     dt_open();
        else if (sg == SG_BATT) { nucleo_app_set_brightness(40); nucleo_app_persist_prefs(); toast_ok(TR("Luminosita al 40%", "Brightness set to 40%")); }
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
    case R_SESSIONS: if (r.kind == K_DANGER) { s_cf = R_SESSIONS; s_cf_yes = false; } break;
    case R_FORGET_ALL: s_cf = R_FORGET_ALL; s_cf_yes = false; break;
    case R_SAVED_NET: {
        const char *ss = nucleo_setup_net_ssid(r.num);
        bool pin = !r.on;
        nucleo_setup_net_set_priority(ss, pin ? WIFI_PIN_PRIO : 0);
        toast_ok(pin ? TR("Preferita: si collega per prima", "Preferred: joined first") : TR("Priorita normale", "Normal priority"));
    } break;
    case R_RST_SOFT: case R_RST_HARD:
        if (s_rst_id != r.id) { s_rst_id = r.id; s_rst_left = 2; }
        else if (--s_rst_left <= 0) {
            flush_prefs();
            // Soft: config + sessions + logs. Hard: also keys, ANIMA's learned data, backups, journal.
            static const char *const SOFT[] = { "/sd/system/config", "/sd/system/sessions", "/sd/system/log" };
            static const char *const HARD[] = { "/sd/system/keys", "/sd/data/anima/learned", "/sd/config", "/sd/backups", "/sd/journal" };
            static const char *const HARD_FILES[] = { "/sd/data/anima/teacher.json", "/sd/data/anima/telemetry.ndjson",
                                                      "/sd/data/anima/session.txt", "/sd/data/anima/sessions.json", "/sd/data/anima/workspace.json" };
            for (int i = 0; i < NROWS(SOFT); i++) rm_tree(SOFT[i]);
            if (r.id == R_RST_HARD) {
                for (int i = 0; i < NROWS(HARD); i++) rm_tree(HARD[i]);
                for (int i = 0; i < NROWS(HARD_FILES); i++) unlink(HARD_FILES[i]);
            }
            esp_restart();
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
    case R_FORGET_ALL: nucleo_setup_forget(); toast_ok(TR("Reti dimenticate: hotspot attivo", "Networks forgotten: hotspot on")); break;
    case R_SESSIONS:   nucleo_auth_revoke(NULL); toast_ok(TR("Tutti i browser disconnessi", "Every browser signed out")); break;
    case R_AP_ON: {
        bool on = !ap_on();
        if (on) nucleo_setup_start_ap(); else nucleo_setup_stop_ap();
        toast_ok(on ? TR("Hotspot acceso: 192.168.4.1", "Hotspot on: 192.168.4.1")
                    : TR("Hotspot spento: torno alla tua rete", "Hotspot off: rejoining your Wi-Fi"));
    } break;
    case R_SAVED_NET: {
        nucleo_setup_forget_ssid(s_cf_ssid); toast_ok(TR("Rete dimenticata", "Network forgotten"));
        int n = page_count(s_page); if (s_sel[s_page] >= n) s_sel[s_page] = (int8_t)(n > 0 ? n - 1 : 0);
    } break;
    }
}

// ---- inline text input ------------------------------------------------------------------------
// Every printable key types — including , ; . / which double as the arrows (Wi-Fi passwords use them).
static void input_char(char c) { if (c >= 32 && c < 127 && s_ilen < (int)sizeof(s_ibuf) - 1) { s_ibuf[s_ilen++] = c; s_ibuf[s_ilen] = 0; } }
static void input_close(void) { s_im = IM_NONE; memset(s_ibuf, 0, sizeof s_ibuf); s_ilen = 0; }
static void input_key(int k, char ch)
{
    if (k == NK_DEL) { if (s_ilen > 0) s_ibuf[--s_ilen] = 0; return; }
    if (k != NK_ENTER) { input_char(ch); return; }
    switch (s_im) {
    case IM_PASS:   snprintf(s_join_pass, sizeof s_join_pass, "%s", s_ibuf); input_close(); start_op(OP_JOIN); break;
    case IM_NAME:
        if (!s_ilen) { toast(TR("Il nome non puo essere vuoto", "The name can't be empty")); return; }
        nucleo_setup_set_device_name(s_ibuf); input_close(); toast_ok(TR("Nome salvato", "Name saved")); break;
    case IM_APSSID:
        if (!s_ilen) { toast(TR("Il nome non puo essere vuoto", "The name can't be empty")); return; }
        nucleo_setup_set_ap_ssid(s_ibuf); input_close(); toast_ok(TR("Nome hotspot salvato", "Hotspot name saved")); break;
    case IM_APPASS:                                  // empty = open hotspot; WPA2 needs >= 8 chars
        if (s_ilen && s_ilen < 8) { toast(TR("Minimo 8 caratteri (o vuota)", "At least 8 chars (or empty)")); return; }
        nucleo_setup_set_ap_pass(s_ibuf); toast_ok(s_ilen ? TR("Password salvata", "Password saved") : TR("Hotspot aperto", "Hotspot is open"));
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
    if (s_im != IM_NONE) { input_key(k, ch); }
    else if (s_cf != R_NONE) { int c = app_ui_confirm_key(k, ch, &s_cf_yes); if (c >= 0) confirm_done(c == 1); }
    else if (s_dt) {
        if (k == NK_UP)          dt_adjust(+1);
        else if (k == NK_DOWN)   dt_adjust(-1);
        else if (k == NK_RIGHT)  dt_step(+1);
        else if (k == NK_ENTER) { nucleo_setup_set_datetime(s_dtv[0], s_dtv[1], s_dtv[2], s_dtv[3], s_dtv[4]); s_dt = false; toast_ok(TR("Data e ora impostate", "Date and time set")); }
    }
    else if (s_busy && s_op == OP_JOIN) { return; }
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
                } else if (sel > 0 && !s_busy) {
                    const char *ss = nucleo_setup_scan_ssid(sel - 1);
                    if (nucleo_setup_net_is_known(ss)) {
                        if (k == NK_DEL) { snprintf(s_cf_ssid, sizeof s_cf_ssid, "%s", ss); s_cf = R_SAVED_NET; s_cf_yes = false; }
                        else if (ch == 'p' || ch == 'P') {
                            bool pin = prio_of(ss) == 0;
                            nucleo_setup_net_set_priority(ss, pin ? WIFI_PIN_PRIO : 0);
                            toast_ok(pin ? TR("Preferita: si collega per prima", "Preferred: joined first") : TR("Priorita normale", "Normal priority"));
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
    else if (s_busy && s_op == OP_JOIN) { /* swallow: the join finishes on its own */ }
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
    if (s_busy) {
        s_anim++;
        if (s_done) {
            s_busy = false; s_done = false; int op = s_op; s_op = OP_NONE;
            if (op == OP_JOIN) {
                memset(s_join_pass, 0, sizeof s_join_pass);
                if (s_join_ok) { toast_ok(TR("Connesso", "Connected")); set_page(PG_WIFI); }
                else toast(TR("Connessione non riuscita", "Could not connect"));
            } else if (op == OP_SCAN && s_page == PG_NETS) {
                s_sel[PG_NETS] = (int8_t)(nucleo_setup_scan_count() > 0 ? 1 : 0);   // focus the strongest network
                s_scroll = 0;
            }
            update_hint();
        }
        nucleo_app_request_draw(); return;
    }
    if (s_tickn % 5 == 0) {                                   // 1 Hz: repaint only when a live value changed
        uint32_t sig = live_sig();
        if (sig != s_live) { s_live = sig; update_hint(); nucleo_app_request_draw(); }
    }
}

// ---- lifecycle -----------------------------------------------------------------------------
static void enter(void)
{
    s_page = PG_ROOT; s_scroll = 0;                           // s_sel[] is kept: resume the last focus
    s_im = IM_NONE; s_cf = R_NONE; s_dt = false; s_rst_id = R_NONE; s_prefs_dirty = false;
    s_qn = 0; s_q[0] = 0; s_hits = 0; s_tts_ok = -1; s_bt_pref = -1;
    s_msg_t = 0; s_live = 0;
    if (!s_task) { s_busy = false; s_op = OP_NONE; s_done = false; }
    nucleo_app_set_tab_handler(on_tab);
    nucleo_app_set_back_handler(on_back);
    update_hint(); nucleo_app_request_draw();
}
static void leave(void)
{
    flush_prefs();
    strip_release();
    memset(s_join_pass, 0, sizeof s_join_pass);
    memset(s_ibuf, 0, sizeof s_ibuf); s_ilen = 0; s_im = IM_NONE;
}

extern "C" void nucleo_register_wifi(void)
{
    static const nucleo_app_def_t app = {
        "wifi", "Impostazioni", "System", "Wi-Fi, hotspot, Bluetooth, display, sound, ANIMA, language, device",
        'W', C_BLUE, enter, on_key, on_tick, on_draw, leave
    };
    nucleo_app_register(&app);
}
