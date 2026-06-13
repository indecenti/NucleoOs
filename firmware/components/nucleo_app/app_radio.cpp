// Radio Index app: a multi-station live-MP3 tuner for the Cardputer.
//
// The station list is SHARED with the web app. It lives on the SD card at
//   /system/config/radio.json   (schema 2: { "default": <id>, "stations": [ {id,name,genre,stream}, ... ] })
// The web "Radio Index" app writes that file via /api/fs/write; this native app reads it on open,
// so stations added/edited/reordered in the browser show up on the device with NO reflash. The host
// tool tools/radio-check.mjs validates the same file. A legacy schema-1 file ({ "stream", "name" })
// is still accepted, and if the card has no config at all we fall back to a single built-in
// "Radio Index" station so the app always works.
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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
extern "C" {
#include "nucleo_audio.h"
#include "nucleo_board.h"
#include "nucleo_kbd.h"
#include "cJSON.h"
}

#include "app_gfx.h"
static const unsigned short BG = 0x0841, FG = 0xFFFF, MUTED = 0x8C71, DIM = 0x4410, HLINE = 0x2945,
                            ACC = 0x4DDF, ONAIR = 0xF96B, GRN = 0x8FF3, WARM = 0xFE8C, INK = 0x0000;

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
static void seed_default(void) { seed_radio_index(); s_count = 1; s_default = 0; }  // fallback if alloc fails

static void load_config(void)
{
    // Slot 0 is always Radio Index (pinned, never overwritten by the JSON list).
    seed_radio_index();
    s_count = 1; s_default = 0;                              // start with Radio Index in slot 0

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

// status: 0 = connecting, 1 = live (audio flowing), 2 = offline (gave up).
static void draw_status(int status)
{
    const char *lbl = (status == 1) ? "ON AIR" : (status == 2) ? "OFFLINE" : "TUNING";
    unsigned short col = (status == 1) ? ONAIR : (status == 2) ? MUTED : WARM;
    d.fillCircle(18, ST_Y + 12, 6, col);
    d.fillRect(ST_X, ST_Y, EQ_X - ST_X - 4, 24, BG);
    d.setTextSize(3); d.setTextColor(col, BG);
    d.setCursor(ST_X, ST_Y); d.print(lbl);
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
    d.fillRoundRect(VB_X, VB_Y, VB_W, VB_H, 5, 0x10A2);              // track
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

static void listen(const station_t *st)
{
    nucleo_app_release_buffers();                 // reclaim RAM for the decoder + HTTP client
    nucleo_audio_play_url(st->stream);
    radio_static(st);

    int64_t start = esp_timer_get_time() / 1000;
    bool back = false;
    int last_status = -1, last_vol = -1, last_eq = -1;
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
        bool flowing = nucleo_audio_is_playing() && nucleo_audio_elapsed_ms() > 0;
        int status = flowing ? 1 : ((now - start > 14000) ? 2 : 0);    // 14 s with no audio -> offline
        int vol    = nucleo_audio_volume();
        int eq_slot = (int)(now / 120);                                // ~8 fps equalizer

        if (status != last_status) { last_status = status; draw_status(status); draw_eq(status == 1); last_eq = eq_slot; }
        else if (status == 1 && eq_slot != last_eq) { last_eq = eq_slot; draw_eq(true); }
        if (vol != last_vol) { last_vol = vol; draw_volume(); }
        vTaskDelay(pdMS_TO_TICKS(60));
    }
    nucleo_audio_stop();                          // terminate the stream task before leaving the modal
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

static void enter(void)
{
    if (!s_st) { s_st = (station_t *)malloc(sizeof(station_t) * RADIO_MAX); s_cap = s_st ? RADIO_MAX : 0; }
    if (!s_st) { s_st = &s_fallback; s_cap = 1; }                   // malloc failed -> single static slot
    load_config();
    s_sel = (s_default < s_count) ? s_default : 0;
    s_top = 0; clamp_scroll();
    nucleo_app_set_direct_draw(true);              // run DIRECT: no 32 KB canvas, no reacquire flicker
    nucleo_app_release_buffers();                  // hand that RAM to the decoder right away
    nucleo_app_set_hint("up/down  pick      enter  listen");
}

// Always tear everything down — audio task (decoder + WiFi buffers) and the station array — so the
// app leaves no RAM behind, whether or not it ever streamed.
static void leave(void)
{
    nucleo_audio_stop();
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
        'R', 0x4DDF, enter, on_key, tick, draw, leave
    };
    nucleo_app_register(&app);
}
