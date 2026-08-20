// Radio Index app: a multi-station live-MP3 tuner for the Cardputer.
//
// The station list is SHARED with the web app. It lives on the SD card at
//   /system/config/radio.json   (schema 2: { "default": <id>, "stations": [ {id,name,genre,stream}, ... ] })
// The web "Radio Index" app writes that file via /api/fs/write; this native app reads it on open,
// so stations added/edited/reordered in the browser show up on the device with NO reflash. The host
// tool tools/radio-check.mjs validates the same file. A legacy schema-1 file ({ "stream", "name" })
// is still accepted, and if the card has no usable config we fall back to the FULL curated catalog
// (BUILTIN[] below — the same stations the web app seeds), so the dial is rich out of the box and
// never collapses to a single entry.
//
// RAM / SOLO: Radio opens via a Solo warm-reboot (NX_NET_APP | NX_SOLO — see the note at the
// registration for the measured numbers). Inline on the full OS the Helix decoder cannot come up at
// all (largest block ~13 KB vs ~20+ needed — the reported "no RAM" bug); the Solo boot provides
// ~60 KB free / ~23 KB largest and the full chain was measured alive there. The session is lean
// either way: station list freed during listen (~5.5 KB), I2S pre-opened BEFORE the jitter ring,
// ring sized to the real heap, decoder retried after a reclaim, EAGAIN treated as a wait.
//
// Audio: http://<host>/stream  (MP3, decoded by the Helix task via nucleo_audio_play_url ->
//        nucleo_audio_http.c). PLAIN HTTP, no TLS (this chip has no PSRAM; TLS would not fit beside
//        the real-time decoder). Keep stream URLs http:// and direct-200 (no redirects) or it stalls.
//
// ZERO-FLICKER DISCIPLINE (the whole point of this file's structure):
//   * The app runs DIRECT (no 32 KB shared canvas): set_direct_draw + release_buffers in enter().
//     That removes the canvas-reacquire churn that, after WiFi streaming fragments the heap, used to
//     force full-screen direct redraws (= flicker), and it returns ~32 KB to the decoder.
//   * The station list is a STATIC, snapping list — NOT the animated app_ui_list. Moving the
//     selection repaints ONLY the two rows that changed (old + new) plus the small header genre
//     field; nothing animates, so tick() is a no-op and there is no per-frame redraw at all.
//   * Only a scroll past the visible window repaints the list band, and only once.
//   * The ON AIR screen paints its chrome once; afterwards only the status badge, the bounded
//     equalizer and the volume bar repaint, each on its own change.
//
// ON EXIT everything is torn down: leave() always stops the audio task (frees the decoder + the WiFi
// RX buffers) and frees the station array, every time — even if the app is closed straight from the
// list without ever streaming.
#include "nucleo_app.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_log.h"            // WARN-level breadcrumbs (INFO is compiled out in release builds)
#include "esp_heap_caps.h"      // heap stats in the listen breadcrumb
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
extern "C" {
#include "nucleo_audio.h"
#include "nucleo_exclusive.h"   // dedicated-mode reclaim (~70KB) while streaming — like music/video; NX_VOICE frees the mic
#include "nucleo_board.h"
#include "nucleo_kbd.h"
#include "cJSON.h"
}

#include "app_gfx.h"
#include "nucleo_theme.h"       // themed chrome; was hardcoded classic literals -> ignored theme switches
#include "nucleo_i18n.h"        // TR(it,en): hint follows the system language

// Live STA link state (extern like app_wifi.cpp — a header include would cycle nucleo_setup->nucleo_app).
// The radio is the one app whose whole existence depends on the uplink, so it treats the link as a
// first-class state instead of "assume connected and time out into a generic OFFLINE".
extern "C" const char *nucleo_setup_ip(void);     // "" until the STA join lands
extern "C" const char *nucleo_setup_ssid(void);
extern "C" int         nucleo_setup_rssi(void);   // dBm, 0 = not associated
// Chrome follows the active theme; ACC is the app identity accent, ONAIR/GRN/WARM are content colors.
#define BG    THEME_BG
#define FG    THEME_FG
#define MUTED THEME_MUTED
#define DIM   THEME_DIM
#define HLINE THEME_LINE
#define INK   THEME_INK
static const unsigned short ACC = 0x4DDF, ONAIR = 0xF96B, GRN = 0x8FF3, WARM = 0xFE8C;

// ---- station list (shared with the web app via /system/config/radio.json) -------------------
#define RADIO_MAX 24
typedef struct { char name[40]; char genre[28]; char stream[160]; } station_t;
static const char *DEFAULT_URL = "http://radioindex-130-110-12-237.sslip.io/stream";
static station_t  s_fallback;            // 1-slot static safety net (~0.2 KB) if malloc fails
static station_t *s_st = nullptr;        // malloc'd while the app is OPEN, freed on leave()
static int s_cap = 0, s_count = 0, s_sel = 0, s_default = 0, s_top = 0;

// Always slot 0: the built-in Radio Index (pinned first so it is always in evidence).
static void seed_radio_index(void)
{
    snprintf(s_st[0].name,   sizeof s_st[0].name,   "%s", "Radio Index");
    snprintf(s_st[0].genre,  sizeof s_st[0].genre,  "%s", "Signature");
    snprintf(s_st[0].stream, sizeof s_st[0].stream, "%s", DEFAULT_URL);
}

// Curated free stations (plain-HTTP MP3, direct-200, verified by tools/radio-check.mjs). The SAME
// list the web Radio app seeds, mirrored here so a card never written by the web app still shows a
// full dial. SD /system/config/radio.json, when present, overrides this. Keep in lock-step with the
// SEED in apps/radio/www/index.html and tools/sd-sim/system/config/radio.json.
typedef struct { const char *name; const char *genre; const char *stream; } builtin_t;
static const builtin_t BUILTIN[] = {
    { "Groove Salad",     "Ambient / Downtempo",  "http://ice1.somafm.com/groovesalad-128-mp3" },
    { "Drone Zone",       "Ambient / Space",      "http://ice1.somafm.com/dronezone-128-mp3" },
    { "Lush",             "Chill / Vocals",       "http://ice1.somafm.com/lush-128-mp3" },
    { "Secret Agent",     "Lounge / Spy Jazz",    "http://ice1.somafm.com/secretagent-128-mp3" },
    { "Indie Pop Rocks!", "Indie Pop",            "http://ice1.somafm.com/indiepop-128-mp3" },
    { "Underground 80s",  "Synth / New Wave",     "http://ice1.somafm.com/u80s-128-mp3" },
    { "Beat Blender",     "Deep House",           "http://ice1.somafm.com/beatblender-128-mp3" },
    { "Fluid",            "Instrumental Hip-Hop", "http://ice1.somafm.com/fluid-128-mp3" },
    { "Boot Liquor",      "Americana",            "http://ice1.somafm.com/bootliquor-128-mp3" },
    { "PopTron",          "Electro Pop",          "http://ice1.somafm.com/poptron-128-mp3" },
    { "DEF CON Radio",    "Hacker / Electro",     "http://ice1.somafm.com/defcon-128-mp3" },
    { "Folk Forward",     "Folk / Acoustic",      "http://ice1.somafm.com/folkfwd-128-mp3" },
    { "Left Coast 70s",   "70s / Mellow",         "http://ice1.somafm.com/seventies-128-mp3" },
    { "Seven Inch Soul",  "Vintage Soul",         "http://ice1.somafm.com/7soul-128-mp3" },
    { "Metal Detector",   "Heavy Metal",          "http://ice1.somafm.com/metal-128-mp3" },
    { "Space Station",    "Space / Ambient",      "http://ice1.somafm.com/spacestation-128-mp3" },
    { "The Trip",         "Prog / Trip-Hop",      "http://ice1.somafm.com/thetrip-128-mp3" },
    { "Sonic Universe",   "Avant-Jazz",           "http://ice1.somafm.com/sonicuniverse-128-mp3" },
    { "ThistleRadio",     "Celtic / World",       "http://ice1.somafm.com/thistle-128-mp3" },
};

// Append the curated list after the pinned Radio Index — the device's built-in dial when the SD card
// has no usable radio.json yet (mirrors the web app's seed so both show the same stations).
static void seed_builtin_list(void)
{
    for (int i = 0; i < (int)(sizeof BUILTIN / sizeof BUILTIN[0]) && s_count < s_cap; i++) {
        station_t *s = &s_st[s_count];
        snprintf(s->name,   sizeof s->name,   "%s", BUILTIN[i].name);
        snprintf(s->genre,  sizeof s->genre,  "%s", BUILTIN[i].genre);
        snprintf(s->stream, sizeof s->stream, "%s", BUILTIN[i].stream);
        s_count++;
    }
}

// Read SD /system/config/radio.json and APPEND its stations after the pinned Radio Index. Leaves the
// list untouched on any miss (no file, no RAM, bad/empty JSON) so load_config() can fall back.
static void load_sd_stations(void)
{
    FILE *f = fopen(NUCLEO_SD_MOUNT "/system/config/radio.json", "rb");
    if (!f) return;
    char *buf = (char *)malloc(8192);
    if (!buf) { fclose(f); return; }
    int n = (int)fread(buf, 1, 8191, f); fclose(f);
    if (n <= 0) { free(buf); return; }
    buf[n] = 0;
    cJSON *root = cJSON_Parse(buf); free(buf);
    if (!root) return;

    cJSON *arr = cJSON_GetObjectItem(root, "stations");
    if (cJSON_IsArray(arr) && cJSON_GetArraySize(arr) > 0) {
        cJSON *defv = cJSON_GetObjectItem(root, "default");
        const char *def_id = cJSON_IsString(defv) ? defv->valuestring : nullptr;
        cJSON *it;
        cJSON_ArrayForEach(it, arr) {
            if (s_count >= s_cap) break;
            cJSON *st = cJSON_GetObjectItem(it, "stream");
            if (!cJSON_IsString(st) || !st->valuestring[0]) continue;
            // Skip if it's the same stream as the pinned Radio Index (avoid duplicate).
            if (!strcmp(st->valuestring, DEFAULT_URL)) continue;
            cJSON *nm = cJSON_GetObjectItem(it, "name");
            cJSON *ge = cJSON_GetObjectItem(it, "genre");
            cJSON *id = cJSON_GetObjectItem(it, "id");
            station_t *s = &s_st[s_count];
            snprintf(s->stream, sizeof s->stream, "%s", st->valuestring);
            snprintf(s->name,   sizeof s->name,   "%s", cJSON_IsString(nm) ? nm->valuestring : "Station");
            snprintf(s->genre,  sizeof s->genre,  "%s", cJSON_IsString(ge) ? ge->valuestring : "");
            // If the JSON marks this as default, honour it — but Radio Index at 0 stays pinned.
            if (def_id && cJSON_IsString(id) && !strcmp(id->valuestring, def_id)) s_default = s_count;
            s_count++;
        }
    } else {
        // Legacy schema 1: single top-level { "stream", "name" } — add as slot 1 if different.
        cJSON *st = cJSON_GetObjectItem(root, "stream"), *nm = cJSON_GetObjectItem(root, "name");
        if (cJSON_IsString(st) && st->valuestring[0] && strcmp(st->valuestring, DEFAULT_URL) != 0) {
            if (s_count < s_cap) {
                snprintf(s_st[s_count].stream, sizeof s_st[s_count].stream, "%s", st->valuestring);
                if (cJSON_IsString(nm)) snprintf(s_st[s_count].name, sizeof s_st[s_count].name, "%s", nm->valuestring);
                s_st[s_count].genre[0] = 0;
                s_count++;
            }
        }
    }
    cJSON_Delete(root);
    if (s_default >= s_count) s_default = 0;
}

static void load_config(void)
{
    // Slot 0 is always Radio Index (pinned, never overwritten by the JSON list).
    seed_radio_index();
    s_count = 1; s_default = 0;                              // start with Radio Index in slot 0

    load_sd_stations();                                     // SD list, when present, fills the dial

    // Nothing usable on the card (missing file, parse error, or an empty list) — fall back to the full
    // curated catalog so the native dial is as rich as the web app, never just Radio Index.
    if (s_count <= 1) seed_builtin_list();
}

// ================= ON AIR listening screen (blocking modal) =====================================
// Painted once by radio_static(); afterwards only three small regions ever repaint, each on its own
// trigger — no per-frame full redraw. (See the zero-flicker note at the top.)
static const int ST_X = 36, ST_Y = 40;                            // status label (size 3, 24 px tall)
static const int EQ_X = 168, EQ_Y = 40, EQ_W = 60, EQ_H = 24;     // equalizer box
static const int VB_X = 10, VB_Y = 104, VB_W = 196, VB_H = 16;    // volume track (leaves room for %)

static void radio_static(const station_t *st)
{
    d.fillScreen(BG);
    char nm[20]; snprintf(nm, sizeof nm, "%.18s", st->name);
    d.setTextSize(2); d.setTextColor(ACC, BG); d.setCursor(10, 6); d.print(nm);
    d.drawFastHLine(10, 28, 220, HLINE);
    if (st->genre[0]) {
        char g[34]; snprintf(g, sizeof g, "%.32s", st->genre);
        d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, 72); d.print(g);
    }
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, 90); d.print("VOLUME");
    // Clear, explicit key legend (the Cardputer's ';' / '.' are the up/down keys).
    d.setTextColor(DIM, BG); d.setCursor(10, 125); d.print(";  vol +     .  vol -     ESC  stop");
}

// status: 0 = tuning (WiFi up, stream connecting), 1 = live (audio flowing),
//         2 = offline (gave up), 3 = waiting for the WiFi join itself.
static void draw_status(int status)
{
    const char *lbl = (status == 1) ? "ON AIR" : (status == 2) ? "OFFLINE"
                    : (status == 3) ? "WIFI..." : "TUNING";
    unsigned short col = (status == 1) ? ONAIR : (status == 2) ? MUTED : WARM;
    d.fillCircle(18, ST_Y + 12, 6, col);
    d.fillRect(ST_X, ST_Y, EQ_X - ST_X - 4, 24, BG);
    d.setTextSize(3); d.setTextColor(col, BG);
    d.setCursor(ST_X, ST_Y); d.print(lbl);
}

// Link line under the status: SSID + signal while joined, an honest explanation while not. Its own
// small band (y=72..80, where the genre used to sit alone) — repaints only when the text changes.
static char s_netline_last[48];                             // reset on every listen() entry
static void draw_netline(const station_t *st)
{
    char line[48];
    const char *ip = nucleo_setup_ip();
    if (ip[0]) {
        int r = nucleo_setup_rssi();
        snprintf(line, sizeof line, "%.20s  %d dBm", nucleo_setup_ssid(), r);
    } else {
        snprintf(line, sizeof line, "%s", TR("attendo rete WiFi...", "waiting for WiFi..."));
    }
    if (!strcmp(line, s_netline_last)) return;
    snprintf(s_netline_last, sizeof s_netline_last, "%s", line);
    d.fillRect(120, 72, 120, 10, BG);                       // right half; genre keeps the left
    int w = (int)strlen(line) * 6;
    d.setTextSize(1); d.setTextColor(ip[0] ? MUTED : WARM, BG);
    d.setCursor(238 - w, 72); d.print(line);
}

// Time-driven bars (no RNG): each bar is a triangle wave at its own period. Bounded 60x24 box; this
// is the single intentional animation and it stops the instant audio stops.
static void draw_eq(bool live)
{
    const int floor_y = EQ_Y + EQ_H, bw = 9, gap = 6;
    d.fillRect(EQ_X, EQ_Y, EQ_W, EQ_H, BG);
    if (!live) { for (int i = 0; i < 4; i++) d.fillRect(EQ_X + i * (bw + gap), floor_y - 3, bw, 3, DIM); return; }
    static const int per[4] = { 220, 310, 180, 260 };
    int64_t t = esp_timer_get_time() / 1000;
    for (int i = 0; i < 4; i++) {
        long p = per[i], ph = (t + (long)i * 37) % p, tri = (ph < p / 2) ? ph : p - ph;
        int h = 4 + (int)(tri * 20 / (p / 2)); if (h > EQ_H) h = EQ_H;
        d.fillRect(EQ_X + i * (bw + gap), floor_y - h, bw, h, GRN);
    }
}

// Volume bar + percentage. Text is vertically centered on the bar (not drawn above it).
static void draw_volume(void)
{
    int vol = nucleo_audio_volume();
    d.fillRoundRect(VB_X, VB_Y, VB_W, VB_H, 5, HLINE);              // track
    int fw = VB_W * vol / 100; if (fw < 0) fw = 0; if (fw > VB_W) fw = VB_W;
    if (fw > 0) d.fillRoundRect(VB_X, VB_Y, fw, VB_H, 5, GRN);       // fill
    char vb[8]; snprintf(vb, sizeof vb, "%d%%", vol);
    // Clear and redraw the % label in the gap to the right of the bar, vertically centered.
    int lx = VB_X + VB_W + 4, lw = 240 - lx;
    d.fillRect(lx, VB_Y, lw, VB_H, BG);
    // size-1 glyph = 8px tall; center inside VB_H.
    int ty = VB_Y + (VB_H - 8) / 2;
    d.setTextSize(1); d.setTextColor(FG, BG);
    d.setCursor(lx, ty); d.print(vb);
}

static void clamp_scroll(void);                   // defined with the list UI below
static void ensure_stations(void);                // defined with enter() below

static void listen(const station_t *stp)
{
    // Local copy of THIS station (228 B on the UI task's stack), then free the whole list for the
    // duration of the listen: its ~5.5 KB goes exactly where the session is tightest — the Helix
    // decoder, the jitter ring and the Wi-Fi RX pool all carve the same small arena (measured:
    // free was ~13.5 KB at stream start). ensure_stations() rebuilds it on the way back.
    station_t stv = *stp; const station_t *st = &stv;
    if (s_st && s_st != &s_fallback) free(s_st);
    s_st = nullptr; s_cap = 0; s_count = 0;
    ESP_LOGW("radio", "listen '%s' free=%u largest=%u", st->name,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    nucleo_app_release_buffers();                 // reclaim RAM for the decoder + HTTP client
    // Dedicated mode while streaming: NX_NET_APP frees ~70KB (Wi-Fi STA stays for the stream) so the Helix
    // MP3 decoder never OOMs, and NX_VOICE drops the mic so it can't collide with the speaker on GPIO43.
    // Skipped in Solo boot: httpd/mDNS/L1/voice never started, and the in-place enter/exit is the fragile
    // ADV path — the clean heap already has the contiguous block the decoder needs.
    if (!nucleo_anima_solo_active()) nucleo_exclusive_enter(NX_NET_APP, nullptr);
    radio_static(st);
    s_netline_last[0] = 0;                        // fresh netline for this session
    draw_netline(st);                             // show the link state from the very first frame

    // LINK-AWARE start. In the Radio Solo boot the STA join runs in the BACKGROUND (the wifi
    // supervisor fires ~2 s after boot and may take several more to land), so the app's old
    // "play immediately, call it OFFLINE after a blind 14 s" raced the join it depended on — the
    // user saw OFFLINE while the WiFi was still coming up. Now the link is an explicit state:
    // wait for the IP first (WIFI... on screen, keys live), start the stream only once the
    // network exists, and only THEN arm the no-audio timeout.
    const int64_t WIFI_WAIT_MS = 30000;           // supervisor scan+join worst case, with margin
    const int64_t TUNE_WAIT_MS = 14000;           // stream connect+prebuffer once the link is up
    int64_t t0 = esp_timer_get_time() / 1000;
    int64_t start = -1;                           // stream start (armed when the join lands)
    bool back = false, playing = false;
    int last_status = -1, last_vol = -1, last_eq = -1;
    uint32_t last_elapsed = 0; int64_t last_flow_ms = 0;    // stall detector (reconnect feedback)
    while (!back) {
        esp_task_wdt_reset();
        nucleo_key_t k = nucleo_kbd_read();
        if (k.key != NK_NONE) {
            if      (k.key == NK_BACK || k.key == NK_TAB || k.ch == '`') back = true;
            else if (k.key == NK_ENTER || k.ch == ' ' || k.ch == 's')   back = true;   // stop -> back
            else if (k.key == NK_UP)   nucleo_audio_set_volume(nucleo_audio_volume() + 10);
            else if (k.key == NK_DOWN) nucleo_audio_set_volume(nucleo_audio_volume() - 10);
        }
        int64_t now = esp_timer_get_time() / 1000;
        bool link = nucleo_setup_ip()[0] != 0;

        if (!playing && link) {                   // join landed -> start the stream, arm the tuner
            esp_err_t pe = nucleo_audio_play_url(st->stream);
            playing = true;
            start = now;
            if (pe != ESP_OK) {                   // task/mic refusal: honest OFFLINE now, not in 14 s
                ESP_LOGW("radio", "play_url failed: %s", esp_err_to_name(pe));
                start = now - TUNE_WAIT_MS - 1;
            }
        }

        int status;
        if (!playing) {                           // still waiting for the WiFi join
            status = (now - t0 > WIFI_WAIT_MS) ? 2 : 3;
        } else {
            uint32_t el = nucleo_audio_elapsed_ms();
            bool flowing = nucleo_audio_is_playing() && el > 0;
            if (flowing && el != last_elapsed) { last_elapsed = el; last_flow_ms = now; }
            // elapsed frozen = decoder starved (stream stall / reconnect in progress): show TUNING
            // again instead of a false ON AIR, and flip back the moment samples move.
            bool stalled = flowing && (now - last_flow_ms > 3000);
            // While the link is up and the player task is alive the producer NEVER stops retrying,
            // so a hard OFFLINE before it succeeds is a lie — the old blind "14 s -> OFFLINE" fired
            // exactly while a slow first connect was still in progress. OFFLINE now means something
            // real: the player task itself gave up (play refused / decoder OOM), or a very long
            // barren wait (60 s) with nothing ever decoded.
            bool player_dead = !nucleo_audio_is_playing();
            status = (flowing && !stalled) ? 1
                   : ((player_dead || (now - start > 60000 && el == 0)) ? 2 : 0);
            (void)TUNE_WAIT_MS;
        }
        int vol    = nucleo_audio_volume();
        int eq_slot = (int)(now / 120);                                // ~8 fps equalizer

        if (status != last_status) { last_status = status; draw_status(status); draw_eq(status == 1); last_eq = eq_slot; }
        else if (status == 1 && eq_slot != last_eq) { last_eq = eq_slot; draw_eq(true); }
        if (vol != last_vol) { last_vol = vol; draw_volume(); }
        static int nl_div = 0;
        if (++nl_div >= 16) { nl_div = 0; draw_netline(st); }          // SSID/RSSI ~every second
        vTaskDelay(pdMS_TO_TICKS(60));
    }
    nucleo_audio_stop();                          // terminate the stream task before leaving the modal
    if (!nucleo_anima_solo_active()) nucleo_exclusive_exit();   // restore httpd/mDNS/voice/L1 (Solo: never suspended; Esc reboots out)
    ensure_stations();                            // rebuild the list we freed for the stream
    clamp_scroll();
    d.fillScreen(BG);
    nucleo_app_request_draw();                    // full list redraw on the way back
}

// ================= station list (idle) — static, snapping, partial-repaint ======================
#define ROW_H 24
static int list_top(void)  { return nucleo_app_content_top() + 26; }
static int list_rows(void) { int v = (nucleo_app_content_height() - 26) / ROW_H; return v < 1 ? 1 : v; }

static void clamp_scroll(void)
{
    int vis = list_rows();
    if (s_sel < s_top) s_top = s_sel;
    if (s_sel >= s_top + vis) s_top = s_sel - vis + 1;
    if (s_top < 0) s_top = 0;
    if (s_top > s_count - vis) s_top = (s_count > vis) ? s_count - vis : 0;
}

// One row at its slot. Focused = a filled pill with a big (size-2) name; others = a colour dot, the
// name (size-1) and the genre, dimmed. Each call clears only its own row band -> no flicker.
static void draw_row(int i)
{
    int y = list_top() + (i - s_top) * ROW_H;
    bool focus = (i == s_sel);
    unsigned short col = (i == s_default) ? GRN : ACC;
    d.fillRect(0, y, 234, ROW_H, BG);                               // leave x>=236 (scroll knob) untouched
    if (focus) {
        d.fillRoundRect(4, y + 1, 230, ROW_H - 2, 6, col);          // green pill = default, blue = other
        char nm[20]; snprintf(nm, sizeof nm, "%.18s", s_st[i].name);
        d.setTextSize(2); d.setTextColor(INK, col); d.setCursor(12, y + 4); d.print(nm);
    } else {
        d.fillCircle(12, y + ROW_H / 2, 3, col);
        int gw = (int)strlen(s_st[i].genre) * 6;
        int name_max = (228 - (gw ? gw + 10 : 0) - 22) / 6; if (name_max < 4) name_max = 4;
        char nm[40]; snprintf(nm, sizeof nm, "%.*s", name_max, s_st[i].name);
        d.setTextSize(1); d.setTextColor(FG, BG); d.setCursor(22, y + ROW_H / 2 - 4); d.print(nm);
        if (gw) { d.setTextColor(DIM, BG); d.setCursor(232 - gw, y + ROW_H / 2 - 4); d.print(s_st[i].genre); }
    }
}

// Repaint the selected station's genre in the header's right field (small, isolated region).
static void draw_header_genre(void)
{
    int cy = nucleo_app_content_top();
    d.fillRect(96, cy + 2, 142, 16, BG);
    if (s_count > 0 && s_st[s_sel].genre[0]) {
        char g[28]; snprintf(g, sizeof g, "%.23s", s_st[s_sel].genre);
        int gw = (int)strlen(g) * 6;
        d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(236 - gw, cy + 7); d.print(g);
    }
}

// Repaint just the list band (used on scroll): clear once, draw the visible rows + a scroll knob.
static void draw_list_band(void)
{
    int lt = list_top(), vis = list_rows(), lh = vis * ROW_H;
    d.fillRect(0, lt, 240, nucleo_app_content_height() - 26, BG);
    for (int i = s_top; i < s_top + vis && i < s_count; i++) draw_row(i);
    if (s_count > vis) {                                            // slim scroll indicator
        int track = lh - 6, kh = track * vis / s_count; if (kh < 10) kh = 10;
        int ky = lt + 3 + (track - kh) * s_sel / (s_count - 1);
        d.fillRoundRect(236, lt + 3, 2, track, 1, HLINE);
        d.fillRoundRect(236, ky, 2, kh, 1, (s_sel == s_default) ? GRN : ACC);
    }
}

// Allocate + fill the station list. Shared by enter() and the return-from-listen reload (the list is
// FREED for the whole listen so its ~5.5 KB serves the decoder + Wi-Fi RX instead — see listen()).
static void ensure_stations(void)
{
    if (!s_st) {
        s_st = (station_t *)malloc(sizeof(station_t) * RADIO_MAX); s_cap = s_st ? RADIO_MAX : 0;
        if (!s_st) { s_st = (station_t *)malloc(sizeof(station_t) * 12); s_cap = s_st ? 12 : 0; }  // degrade to a dozen, don't collapse to 1
    }
    if (!s_st) { s_st = &s_fallback; s_cap = 1; }                   // last resort: single static slot
    load_config();
    if (s_sel >= s_count) s_sel = 0;                                // keep the cursor valid after a reload
}

static void enter(void)
{
    // Free the 32 KB shared canvas FIRST so the station list (228 B * 24 = ~5.5 KB CONTIGUOUS) has room.
    // Order matters on the ADV: with the canvas still held the heap is fragmented and the malloc fell back
    // to the 1-slot static net -> "only Radio Index" in the dial. Release, then allocate.
    nucleo_app_set_direct_draw(true);              // run DIRECT: no 32 KB canvas, no reacquire flicker
    nucleo_app_release_buffers();                  // hand that RAM back before we ask for the contiguous block
    ensure_stations();
    s_sel = (s_default < s_count) ? s_default : 0;
    s_top = 0; clamp_scroll();
    nucleo_app_set_hint(TR("su/giu scegli   invio ascolta", "up/dn pick   enter listen"));
}

// Always tear everything down — audio task (decoder + WiFi buffers) and the station array — so the
// app leaves no RAM behind, whether or not it ever streamed.
static void leave(void)
{
    nucleo_audio_stop();
    if (nucleo_exclusive_active()) nucleo_exclusive_exit();   // safety net: never leave services suspended
    if (s_st && s_st != &s_fallback) free(s_st);
    s_st = nullptr; s_cap = 0; s_count = 0; s_sel = 0; s_top = 0;
}

static void on_key(int key, char ch)
{
    if (s_count <= 0) return;

    if (key == NK_UP || key == NK_DOWN) {
        int old = s_sel, old_top = s_top;
        s_sel = (key == NK_UP) ? (s_sel + s_count - 1) % s_count : (s_sel + 1) % s_count;
        clamp_scroll();
        if (s_top != old_top) draw_list_band();                    // scrolled/wrapped -> one band repaint
        else { draw_row(old); draw_row(s_sel); }                   // same window -> only the two rows
        draw_header_genre();                                       // isolated header field
        // NB: no request_draw — we painted directly, so the framework won't trigger a full redraw.
    } else if (key == NK_CHAR && ch > ' ') {                       // type-to-jump to the next matching name
        char want = (char)tolower((unsigned char)ch);
        for (int n = 1; n <= s_count; n++) {
            int idx = (s_sel + n) % s_count;
            if (tolower((unsigned char)s_st[idx].name[0]) == want) {
                s_sel = idx; clamp_scroll(); draw_list_band(); draw_header_genre(); break;
            }
        }
    } else if (key == NK_ENTER) {
        listen(&s_st[s_sel]);                                       // blocking; requests a full redraw on return
    }
}

static void tick(void) { }                         // static screen: nothing animates -> never repaints

static void draw(void)                             // full render: framework calls this on open and after listen()
{
    int cy = nucleo_app_content_top(), ch = nucleo_app_content_height();
    d.fillRect(0, cy, 240, ch, BG);
    d.setTextSize(2); d.setTextColor(ACC, BG); d.setCursor(10, cy + 2); d.print("Radio");
    d.drawFastHLine(10, cy + 21, 220, HLINE);
    d.fillRect(10, cy + 21, 60, 2, ACC);
    draw_header_genre();
    if (s_count == 0) { d.setTextColor(DIM, BG); d.setCursor(12, cy + 34); d.print("(no stations)"); return; }
    draw_list_band();
}

extern "C" void nucleo_register_radio(void)
{
    static const nucleo_app_def_t app = {
        "radio", "Radio Index", "Media", "Tune into free live radio (list shared with the web app)",
        'R', 0x4DDF, enter, on_key, tick, draw, leave,
        NX_NET_APP | NX_SOLO
            // SOLO, and this time MEASURED on both sides. Inline (full OS + declarative reclaim) the
            // decoder is dead on arrival: free 24 KB, largest 13 KB — Helix never comes up (the very
            // "no RAM" this app was reported for). The Solo boot gives free ~60 KB / largest ~23 KB and
            // the serial log shows the whole chain alive there: join ~9 s, HTTP 200, bytes flowing,
            // frames decoded. What USED to fail in Solo were two session bugs, both fixed since:
            // EAGAIN-as-fatal tore down live connections, and the I2S sink opened last and found no
            // DMA block (now it pre-opens before the ring). STA-only + PS_NONE keep the fresh join
            // usable. NO NX_WIFI: the stream needs the radio up. Esc reboots back to the full OS.
    };
    nucleo_app_register(&app);
}
