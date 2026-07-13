// Video app v2 — a native on-device player for NucleoOS .nfv clips (MJPEG 240x136 + a
// sibling .mp3). The device cannot decode H.264/H.265 (no PSRAM, no hardware codec), so
// clips are pre-converted on a PC with tools/nfv/encode.py into per-frame JPEGs that M5GFX
// decodes for free, plus a mono low-bitrate MP3 played by the existing Helix task.
//
// Browser: a folder-aware fisheye list aligned with the Music app — folders first (amber, with
// a recursive clip count), then .nfv files (blue, with duration and a green "continue watching"
// mark). Enter descends / plays, Del goes up. Every row self-clears its own background so the
// list is correct AND flicker-free both when the shared canvas is buffered and when a decoder
// holds the RAM and we draw DIRECT (this was the v1 selection-page flicker: only the focused
// row was cleared, the rest relied on the canvas pre-fill that's absent in direct mode).
//
// Settings (TAB): a 4-tab sheet mirroring Music — PLAY / AUDIO / VIEW / LIBRARY — with a
// persistent tab bar, toggle pills, sliders and action rows. Adds Autoplay, Repeat (Off/One/All),
// Resume behaviour, start volume, brightness, sort order and "Continue Watching" management.
// Persisted to /system/config/video.json.
//
// Playback (play_nfv) is a blocking modal: it owns the screen + keyboard until the clip ends or
// the viewer backs out, frees the shared UI canvas to hand the contiguous block to the MP3
// decoder, and pets the task watchdog every frame. When a clip ends naturally and Autoplay/Repeat
// is on, the next clip in the folder starts automatically. See docs/media.md and ANTI-FLICKER.md.
#include "nucleo_app.h"
#include "app_ui.h"
#include "nucleo_i18n.h"        // TR(it,en): hints follow the system language
#include <M5GFX.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
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
#include "nucleo_exclusive.h"   // dedicated-mode RAM reclaim while a clip plays (Wi-Fi STA stays up)
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <errno.h>

static const char *V_TAG = "video";

// ---- Design tokens (RGB565), aligned with the Music app for a consistent Media look ----
static const unsigned short
    BG    = 0x0841,   // base background (void blue)
    SURF  = 0x10A2,   // raised surface / slider track
    CAP   = 0x1A8B,   // focused settings-row capsule
    FG    = 0xFFFF,   // primary text
    MUTED = 0x8C71,   // secondary text (readable grey)
    DIM   = 0x52CB,   // tertiary text / key hints
    LINE  = 0x2945,   // hairline separators
    ACC   = 0x4D9C,   // video accent (matches the launcher tile colour)
    GRN   = 0x8FF3,   // continue-watching / positive
    AMB   = 0xFE8C,   // folders / warm accent
    MUTERED = 0xF96B, // muted indicator (named MUTERED: M5GFX already defines a global 'RED')
    INK   = 0x0000;   // text on bright fills

#define VIDEO_DIR     NUCLEO_SD_MOUNT "/data/Videos"
#define SETTINGS_PATH NUCLEO_SD_MOUNT "/system/config/video.json"

#define MAXE   64               // entries shown in the current folder (folders + clips)
#define HEAD_H 20               // list context-header height

// NFV1 header — must match tools/nfv/encode.py (little-endian, 32 bytes).
#pragma pack(push, 1)
typedef struct {
    char     magic[4];        // "NFV1"
    uint8_t  version;
    uint8_t  flags;           // bit0 = sibling .mp3 exists, bit1 = inline seek index
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    uint16_t reserved0;       // v2: index_stride
    uint32_t frame_count;
    uint32_t duration_ms;
    uint32_t max_frame_bytes;
    uint8_t  reserved1[6];    // v2: [0:4] = index_offset
} nfv_header_t;
#pragma pack(pop)

struct VEntry { char name[72]; bool dir; uint16_t count; uint32_t dur; uint32_t resume; };

// The folder listing lives in one malloc'd block held only while the app is OPEN and freed on
// leave(), so the RAM (~5 KB) goes back to ANIMA (mirrors Files / Music). Guarded: if the alloc
// fails, st stays NULL / s_n stays 0 and every access is bounded, never a crash.
struct VState { VEntry e[MAXE]; };
static VState *st = nullptr;

static char s_path[192] = "/";  // current folder under VIDEO_DIR, always starts+ends in '/'
static int  s_n, s_sel, s_scroll;

// ---- persisted settings ------------------------------------------------------
static bool s_autoplay    = false;  // play the next clip in the folder when one ends
static int  s_repeat      = 0;      // 0=off 1=one 2=all
static int  s_resume_mode = 0;      // 0=ask 1=auto-resume 2=always start over
static int  s_vol_default = 0;      // 0=keep last; else set this volume at play-start
static int  s_sort        = 0;      // 0=name 1=duration (longest first)
static bool s_time_rem    = false;  // controller time field: false=elapsed/total, true=-remaining

// How a blocking playback modal ended — drives the auto-advance / skip loop in play_from().
enum { PR_STOP = 0, PR_ENDED, PR_NEXT, PR_PREV };

// ---- settings sheet state ----------------------------------------------------
static bool s_set_open = false;
static int  s_set_tab  = 0;     // 0=PLAY 1=AUDIO 2=VIEW 3=LIBRARY
static int  s_set_row  = 0;     // -1 = tab header, 0..n-1 = row
static bool s_set_edit = false; // a slider is in adjust mode (UP/DN/L/R change it)
static const int SET_ROWS[4] = { 3, 2, 3, 4 };   // LIBRARY gains a "Help" row
static const char *const TABS[4] = { "PLAY", "AUDIO", "VIEW", "LIBRARY" };

// ---- help / manual (bilingual, scrollable) -----------------------------------
static bool s_help_open  = false;
static int  s_help_scroll = 0;

static int s_hint_last = -1;
static void update_hint(void);

// ---- settings I/O ------------------------------------------------------------
static void load_settings(void)
{
    FILE *f = fopen(SETTINGS_PATH, "rb"); if (!f) return;
    char buf[224]; int n = (int)fread(buf, 1, sizeof buf - 1, f); fclose(f);
    if (n <= 0) return;
    buf[n] = 0;
    cJSON *root = cJSON_Parse(buf); if (!root) return;
    cJSON *v;
    if ((v = cJSON_GetObjectItem(root, "autoplay"))    && cJSON_IsBool(v))   s_autoplay    = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(root, "repeat"))      && cJSON_IsNumber(v)) s_repeat      = (int)v->valuedouble;
    if ((v = cJSON_GetObjectItem(root, "resume_mode")) && cJSON_IsNumber(v)) s_resume_mode = (int)v->valuedouble;
    if ((v = cJSON_GetObjectItem(root, "vol_default")) && cJSON_IsNumber(v)) s_vol_default = (int)v->valuedouble;
    if ((v = cJSON_GetObjectItem(root, "sort"))        && cJSON_IsNumber(v)) s_sort        = (int)v->valuedouble;
    if ((v = cJSON_GetObjectItem(root, "time_rem"))    && cJSON_IsBool(v))   s_time_rem    = cJSON_IsTrue(v);
    if (s_repeat < 0 || s_repeat > 2)           s_repeat = 0;
    if (s_resume_mode < 0 || s_resume_mode > 2) s_resume_mode = 0;
    if (s_vol_default < 0 || s_vol_default > 100) s_vol_default = 0;
    if (s_sort < 0 || s_sort > 1)               s_sort = 0;
    cJSON_Delete(root);
}

static void save_settings(void)
{
    char buf[224];
    snprintf(buf, sizeof buf,
        "{\"autoplay\":%s,\"repeat\":%d,\"resume_mode\":%d,\"vol_default\":%d,\"sort\":%d,\"time_rem\":%s}",
        s_autoplay ? "true" : "false", s_repeat, s_resume_mode, s_vol_default, s_sort,
        s_time_rem ? "true" : "false");
    mkdir(NUCLEO_SD_MOUNT "/system", 0775);
    mkdir(NUCLEO_SD_MOUNT "/system/config", 0775);
    FILE *f = fopen(SETTINGS_PATH, "wb");
    if (f) { fwrite(buf, 1, strlen(buf), f); fclose(f); }
}

// ---- helpers -----------------------------------------------------------------
static bool is_nfv(const char *n)
{
    const char *dot = strrchr(n, '.');
    return dot && !strcasecmp(dot, ".nfv");
}
// Folders first; then by name (sort 0) or duration longest-first (sort 1).
static int cmp(const void *a, const void *b)
{
    const VEntry *x = (const VEntry *)a, *y = (const VEntry *)b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    if (!x->dir && !y->dir && s_sort == 1 && x->dur != y->dur) return x->dur > y->dur ? -1 : 1;
    return strcasecmp(x->name, y->name);
}
static void fmt_mmss(char *b, size_t n, uint32_t s) { snprintf(b, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60)); }

// ---- resume store ("Continue watching") -------------------------------------
// A tiny text DB beside the clips: one "seconds|key" line each, where key is the clip's path
// relative to /data/Videos (just the file name at the root — backward-compatible). Hidden
// (leading dot) so it never appears in the list. We keep a clip's position when the viewer
// stops part-way and drop it once finished (or barely started) — Netflix-style resume.
#define RESUME_DB   VIDEO_DIR "/.nfv-resume"
#define RESUME_MAX  48
struct resume_ent { char name[80]; uint32_t sec; };

// Scratch for the resume DB, kept on the heap — NOT on the stack. resume_save() is reached from the
// deepest call nest (nucleo_app_run->on_key->play_from->play_nfv->resume_save); a 48*84B local array
// there overflowed the 8 KB main-task stack -> memory corruption -> the "other watchdog" reset on ESC.
// resume_save and resume_refresh both run on the UI task and never nest, so one shared buffer is safe.
// Heap-allocated on enter() and freed on leave() so it is ZERO RAM at boot when the app is closed.
static resume_ent *s_resume_scratch = nullptr;

static int resume_read_all(resume_ent *out, int max)
{
    FILE *f = fopen(RESUME_DB, "r");
    if (!f) return 0;
    int n = 0; char line[160];
    while (n < max && fgets(line, sizeof line, f)) {
        char *bar = strchr(line, '|'); if (!bar) continue;
        *bar = 0;
        char *nm = bar + 1, *nl = strpbrk(nm, "\r\n"); if (nl) *nl = 0;
        if (!nm[0]) continue;
        out[n].sec = (uint32_t)strtoul(line, NULL, 10);
        snprintf(out[n].name, sizeof out[n].name, "%s", nm);
        n++;
    }
    fclose(f);
    return n;
}

static void resume_save(const char *key, uint32_t sec, uint32_t total_sec)
{
    resume_ent *e = s_resume_scratch; if (!e) return;
    int n = resume_read_all(e, RESUME_MAX);   // heap scratch, not 4 KB of stack
    for (int i = 0; i < n; i++)                              // drop any existing entry for this clip
        if (!strcmp(e[i].name, key)) { e[i] = e[n - 1]; n--; break; }
    bool finished = total_sec && sec + 5 >= total_sec;
    if (sec >= 5 && !finished) {                            // worth remembering
        if (n >= RESUME_MAX) { for (int i = 1; i < n; i++) e[i - 1] = e[i]; n--; }  // evict oldest
        snprintf(e[n].name, sizeof e[n].name, "%s", key); e[n].sec = sec; n++;
    }
    FILE *f = fopen(RESUME_DB, "w");
    if (!f) return;
    for (int i = 0; i < n; i++) fprintf(f, "%u|%s\n", (unsigned)e[i].sec, e[i].name);
    fclose(f);
}

// Build a clip's resume key = path under /data/Videos (folder prefix + name; bare name at root).
static void rel_key(char *buf, size_t n, const char *name)
{
    const char *p = s_path; if (*p == '/') p++;             // drop leading '/'
    snprintf(buf, n, "%s%s", p, name);
}

// Peek a clip's duration (seconds) from its NFV header — version-aware (v3 moved the field).
static uint32_t nfv_duration_path(const char *vpath)
{
    FILE *f = fopen(vpath, "rb"); if (!f) return 0;
    uint8_t hb[64]; size_t got = fread(hb, 1, sizeof hb, f); fclose(f);
    if (got < 32 || memcmp(hb, "NFV1", 4)) return 0;
    auto le32 = [&](int o) { return (uint32_t)(hb[o] | (hb[o+1] << 8) | (hb[o+2] << 16) | ((uint32_t)hb[o+3] << 24)); };
    if (hb[4] == 3) return got >= 64 ? le32(24) / 1000 : 0;   // v3: duration_ms at offset 24
    return le32(18) / 1000;                                   // v1/v2: duration_ms at offset 18
}

// Count .nfv clips reachable under `abs`, descending a few levels; depth-capped so a deep tree
// can't stall the scan. The parent DIR is already closed, so the nested opendir here is safe.
static int count_clips(const char *abs, int depth)
{
    if (depth > 4) return 0;
    esp_task_wdt_reset();
    DIR *dir = opendir(abs);
    if (!dir) return 0;
    struct dirent *e; int total = 0;
    while ((e = readdir(dir)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (is_nfv(e->d_name)) { total++; continue; }
        char child[320]; snprintf(child, sizeof child, "%s/%s", abs, e->d_name);
        struct stat sb;
        if (stat(child, &sb) == 0 && S_ISDIR(sb.st_mode)) total += count_clips(child, depth + 1);
        if (total > 9999) break;
    }
    closedir(dir);
    return total;
}

// Refresh the per-clip resume position from the DB (no selection change). Returns the number of
// distinct in-progress clips recorded (for the LIBRARY tab's "Continue" counter).
static int resume_refresh(void)
{
    if (!st || !s_resume_scratch) return 0;
    resume_ent *e = s_resume_scratch; int n = resume_read_all(e, RESUME_MAX);   // heap scratch, not 4 KB of stack
    for (int i = 0; i < s_n; i++) {
        st->e[i].resume = 0;
        if (st->e[i].dir) continue;
        char key[200]; rel_key(key, sizeof key, st->e[i].name);
        for (int j = 0; j < n; j++)
            if (!strcmp(e[j].name, key)) { st->e[i].resume = e[j].sec; break; }
    }
    return n;
}

static void scan(void)
{
    s_n = 0; s_sel = 0; s_scroll = 0;
    if (!st) return;
    char base[208]; snprintf(base, sizeof base, "%s%s", VIDEO_DIR, s_path);   // ends in '/'
    DIR *dir = opendir(base);
    if (dir) {
        struct dirent *e;
        while ((e = readdir(dir)) != NULL && s_n < MAXE) {  // pass 1: gather folders + clips
            if (e->d_name[0] == '.') continue;
            char c[320]; snprintf(c, sizeof c, "%s%s", base, e->d_name);
            struct stat sb;
            if (stat(c, &sb) != 0) continue;
            bool isdir = S_ISDIR(sb.st_mode);
            if (!isdir && !is_nfv(e->d_name)) continue;     // hide non-clip files (.mp3 siblings, .idx, .mp4)
            VEntry *r = &st->e[s_n];
            snprintf(r->name, sizeof r->name, "%s", e->d_name);
            r->dir = isdir; r->count = 0; r->dur = 0; r->resume = 0;
            s_n++;
        }
        closedir(dir);                                      // close BEFORE recursing
    }
    for (int i = 0; i < s_n; i++) {                         // pass 2: folder counts + clip durations
        VEntry *r = &st->e[i];
        char c[320]; snprintf(c, sizeof c, "%s%s", base, r->name);
        if (r->dir) r->count = (uint16_t)count_clips(c, 0);
        else        r->dur = nfv_duration_path(c);
        esp_task_wdt_reset();
    }
    qsort(st->e, s_n, sizeof(VEntry), cmp);                 // sort AFTER durations are known
    resume_refresh();
}

// Re-sort the current listing in place (used when the Sort setting changes) — no rescan needed.
static void resort(void)
{
    if (!st || s_n <= 0) return;
    qsort(st->e, s_n, sizeof(VEntry), cmp);
    s_sel = 0; s_scroll = 0;
}

static void go_up(void)
{
    if (!strcmp(s_path, "/")) return;
    int l = strlen(s_path); if (l && s_path[l - 1] == '/') s_path[--l] = 0;
    char *slash = strrchr(s_path, '/');
    if (slash) slash[1] = 0; else strcpy(s_path, "/");
    scan();
}
static void descend(const char *name)
{
    int l = strlen(s_path);
    snprintf(s_path + l, sizeof(s_path) - l, "%s/", name);
    scan();
}

// ---- professional auto-hiding controller (mirrors web/device/apps/video.js) ------------
// Drawn ONLY in the bottom bar region; the video is clip-limited to the area above it so the
// two never overdraw each other -> no flicker on the bare panel.
static const int      BARH = 42, BARY = 135 - 42;
static const unsigned short CO_BG = 0x0861, CO_LINE = 0x2945, CO_TXT = 0xFFFF, CO_MUT = 0x8C71,
                           CO_SEG = 0x10A2, CO_TRK = 0x2A4B, CO_INK = 0x0420, CO_CHIP = 0x1926, CO_WARM = 0xFE8C;

// Tiny speaker / sun glyphs for the meters — clearer and tighter than the old "VOL"/"LUM" text.
static void glyph_vol(int x, int y, unsigned short c)
{
    d.fillRect(x, y + 3, 3, 3, c); d.fillTriangle(x + 3, y, x + 3, y + 9, x + 8, y + 4, c);   // cone
    d.drawFastVLine(x + 10, y + 2, 5, c);                                                      // one wave
}
static void glyph_sun(int x, int y, unsigned short c)
{
    d.fillCircle(x + 5, y + 4, 2, c);
    d.drawFastVLine(x + 5, y - 1, 2, c); d.drawFastVLine(x + 5, y + 7, 2, c);
    d.drawFastHLine(x + 1, y + 4, 2, c); d.drawFastHLine(x + 8, y + 4, 2, c);
    d.drawPixel(x + 2, y + 1, c); d.drawPixel(x + 8, y + 1, c); d.drawPixel(x + 2, y + 7, c); d.drawPixel(x + 8, y + 7, c);
}

// Glanceable 6-segment meter with a leading icon (0 = volume, 1 = brightness). No number — the value
// is read from the segments + the key you pressed; the icon is tighter than text, leaving room for the badge.
static void draw_meter(int x, int y, int icon, int val, unsigned short col)
{
    if (icon == 1) glyph_sun(x, y, CO_MUT); else glyph_vol(x, y, CO_MUT);
    const int bx = x + 16, segs = 6, sw = 8, gap = 2, on = (val * segs + 50) / 100;
    for (int i = 0; i < segs; i++)
        d.fillRoundRect(bx + i * (sw + gap), y, sw, 8, 1, i < on ? col : CO_SEG);
}

// Transport glyphs (10px tall) for the controller's left cluster.
static void glyph_prev(int x, int y, unsigned short c) { d.fillRect(x, y, 2, 10, c); d.fillTriangle(x + 3, y + 5, x + 9, y, x + 9, y + 10, c); }
static void glyph_next(int x, int y, unsigned short c) { d.fillTriangle(x, y, x, y + 10, x + 6, y + 5, c); d.fillRect(x + 7, y, 2, 10, c); }
static void glyph_speaker(int x, int y, unsigned short c)   // speaker cone + a red mute slash
{
    d.fillRect(x, y + 3, 3, 4, c); d.fillTriangle(x + 3, y, x + 3, y + 10, x + 8, y + 5, c);
    d.drawLine(x, y, x + 9, y + 10, MUTERED); d.drawLine(x + 1, y, x + 10, y + 10, MUTERED);
}

// Draw the professional controller bar. See ANTI-FLICKER.md: the bar background + the static
// transport glyphs are painted ONCE (full=true, when the overlay appears); on content changes only
// the small dynamic strips are cleared+repainted, never the whole bar — otherwise it blinks 1/s.
static void draw_controller(const char *title, const char *next_title, uint32_t cur_s, uint32_t tot_s,
                            bool paused, bool muted, int vol, int bri, bool full, bool seeking,
                            int sdir, int sstep, bool near_end)
{
    if (full) {
        d.fillRect(0, BARY, 240, BARH, CO_BG); d.drawFastHLine(0, BARY, 240, CO_LINE);
        glyph_prev(8, BARY + 6, CO_TXT);                    // |< previous clip (static)
        glyph_next(47, BARY + 6, CO_TXT);                   // >| next clip (static)
    }

    // play/pause chip with a real icon (triangle when paused, two bars when playing)
    d.fillRoundRect(24, BARY + 5, 16, 16, 4, paused ? CO_CHIP : ACC);
    unsigned short pc = paused ? 0xCE7F : CO_INK;
    if (paused) d.fillTriangle(30, BARY + 9, 30, BARY + 17, 37, BARY + 13, pc);                 // play
    else { d.fillRect(29, BARY + 9, 3, 8, pc); d.fillRect(35, BARY + 9, 3, 8, pc); }            // pause

    // title + time strip (cleared as one band)
    d.fillRect(60, BARY + 7, 178, 9, CO_BG);
    char tfield[14];
    if (s_time_rem) { uint32_t rem = tot_s > cur_s ? tot_s - cur_s : 0; char r[8]; fmt_mmss(r, sizeof r, rem); snprintf(tfield, sizeof tfield, "-%s", r); }
    else            { char te[8], td[8]; fmt_mmss(te, sizeof te, cur_s); fmt_mmss(td, sizeof td, tot_s); snprintf(tfield, sizeof tfield, "%s/%s", te, td); }
    int twpx = (int)strlen(tfield) * 6;
    d.setTextColor(CO_MUT, CO_BG); d.setCursor(238 - twpx, BARY + 8); d.print(tfield);

    int navail = (238 - twpx - 6) - 62; if (navail < 12) navail = 12;
    int maxc = navail / 6;
    if (seeking) {
        char bd[12]; snprintf(bd, sizeof bd, "%s %ds", sdir > 0 ? ">>" : "<<", sstep);
        d.setTextColor(ACC, CO_BG); d.setCursor(62, BARY + 8); d.print(bd);
    } else if (near_end && next_title && next_title[0]) {
        char up[40]; snprintf(up, sizeof up, "Up next: %.*s", maxc > 9 ? maxc - 9 : 1, next_title);
        d.setTextColor(GRN, CO_BG); d.setCursor(62, BARY + 8); d.print(up);
    } else {
        d.setTextColor(CO_TXT, CO_BG); d.setCursor(62, BARY + 8);
        for (int i = 0; i < maxc && title[i]; i++) d.print(title[i]);
    }

    // scrubber with chapter ticks (the 1-9 jump points) + playhead
    const int px = 6, pw = 228, py = BARY + 24;
    d.fillRect(px - 1, py - 4, pw + 8, 11, CO_BG);
    d.fillRoundRect(px, py, pw, 3, 1, CO_TRK);
    int fillw = tot_s ? (int)((int64_t)pw * cur_s / tot_s) : 0;
    if (fillw > 0) d.fillRoundRect(px, py, fillw, 3, 1, ACC);
    for (int i = 1; i < 10; i++) d.drawFastVLine(px + pw * i / 10, py - 2, 7, CO_SEG);          // 10% chapter marks
    d.fillCircle(px + fillw, py + 1, 3, CO_TXT);

    // meters band (cleared as one strip): VOL (or a MUTED chip) on the left, LUM on the right,
    // and a compact AP/repeat badge far right so the viewer knows it'll keep going.
    d.fillRect(0, BARY + 30, 240, 10, CO_BG);
    if (muted) {
        glyph_speaker(6, BARY + 31, CO_MUT);
        d.setTextSize(1); d.setTextColor(MUTERED, CO_BG); d.setCursor(18, BARY + 31); d.print("MUTE");
    } else {
        draw_meter(6, BARY + 31, 0, vol, GRN);
    }
    draw_meter(104, BARY + 31, 1, bri, CO_WARM);
    if (s_autoplay || s_repeat) {                            // tiny mode badge (fits in the right margin)
        const char *b = s_repeat == 1 ? "R1" : s_repeat == 2 ? "RA" : "AP";
        d.setTextSize(1); d.setTextColor(s_repeat ? ACC : GRN, CO_BG);
        d.setCursor(238 - (int)strlen(b) * 6, BARY + 31); d.print(b);
    }
}

// Full-screen "Comandi" cheat-sheet raised with TAB during playback: makes brightness and every key
// discoverable, and shows Luce/Volume live so you can adjust them right here and see the level change.
static void draw_video_help(int bri, int vol, bool muted)
{
    d.fillScreen(CO_BG);
    d.setTextSize(2); d.setTextColor(ACC, CO_BG); d.setCursor(10, 6); d.print("Comandi");
    d.drawFastHLine(10, 26, 220, CO_LINE);

    auto bar = [&](int y, const char *lbl, int pct, unsigned short col) {
        d.setTextSize(1); d.setTextColor(CO_TXT, CO_BG); d.setCursor(10, y); d.print(lbl);
        const int bx = 92, bw = 100, bh = 8;
        d.fillRoundRect(bx, y - 1, bw, bh, 3, CO_TRK);
        int w = pct * bw / 100; if (w < 0) w = 0; if (w > bw) w = bw;
        if (w) d.fillRoundRect(bx, y - 1, w, bh, 3, col);
        char v[8]; snprintf(v, sizeof v, "%d%%", pct);
        d.setTextColor(col, CO_BG); d.setCursor(198, y); d.print(v);
    };
    bar(34, "- =   Luce", bri, CO_WARM);
    if (muted) { d.setTextSize(1); d.setTextColor(MUTERED, CO_BG); d.setCursor(10, 48); d.print("; .   Volume   (MUTO: m)"); }
    else       bar(48, "; .   Volume", vol, GRN);

    static const char *const rk[6] = { ", /", "[ ]", "INVIO", "0-9", "m   t", "TAB" };
    static const char *const rd[6] = { "indietro / avanti", "clip prec / succ", "play / pausa",
                                       "salta al 10..90%", "muto / tempo restante", "chiudi   (ESC esci)" };
    int y = 66;
    for (int i = 0; i < 6; i++) {
        d.setTextSize(1); d.setTextColor(ACC, CO_BG); d.setCursor(10, y);  d.print(rk[i]);
        d.setTextColor(CO_TXT, CO_BG); d.setCursor(58, y); d.print(rd[i]);
        y += 11;
    }
}

// Netflix-style resume sheet. Returns the chosen start in ms, 0 to start over, or -1 if backed
// out. Blocking modal (owns the screen + keyboard) and pets the watchdog while it waits.
static int64_t resume_prompt(const char *vpath, const char *title, uint32_t resume_sec)
{
    uint32_t total = nfv_duration_path(vpath);
    char rs[8], ts[8]; fmt_mmss(rs, sizeof rs, resume_sec); fmt_mmss(ts, sizeof ts, total);

    int sel = 0, last = -1;                                   // 0 = Resume, 1 = Start over
    for (;;) {
        esp_task_wdt_reset();
        if (sel != last) {
            last = sel;
            d.fillScreen(BG);
            d.setTextSize(1); d.setTextColor(ACC, BG); d.setCursor(12, 10); d.print("Resume playback?");
            char tb[34]; snprintf(tb, sizeof tb, "%.32s", title);
            d.setTextColor(FG, BG); d.setCursor(12, 24); d.print(tb);

            int px = 12, pw = 216, py = 40;                  // a glance at how far in they were
            d.fillRoundRect(px, py, pw, 4, 2, CO_TRK);
            int fw = total ? (int)((int64_t)pw * resume_sec / total) : 0;
            if (fw > 0) d.fillRoundRect(px, py, fw, 4, 2, ACC);
            char line[40]; snprintf(line, sizeof line, "Stopped at %s / %s", rs, ts);
            d.setTextColor(CO_MUT, BG); d.setCursor(12, 50); d.print(line);

            const char *opt[2] = { "Resume", "Start over" };
            for (int b = 0; b < 2; b++) {
                int bx = 12 + b * 112;
                d.fillRoundRect(bx, 70, 104, 22, 6, sel == b ? ACC : CO_CHIP);
                d.setTextColor(sel == b ? CO_INK : FG, sel == b ? ACC : CO_CHIP);
                d.setCursor(bx + (104 - (int)strlen(opt[b]) * 6) / 2, 78); d.print(opt[b]);
            }
            d.setTextColor(CO_MUT, BG); d.setCursor(12, 110); d.print("< > select   enter ok   esc back");
        }
        nucleo_key_t k = nucleo_kbd_read();
        if      (k.key == NK_LEFT || k.key == NK_RIGHT || k.key == NK_UP || k.key == NK_DOWN) sel ^= 1;
        else if (k.key == NK_ENTER) return sel == 0 ? (int64_t)resume_sec * 1000 : 0;
        else if (k.key == NK_BACK || k.key == NK_DEL || k.ch == '`') return -1;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// One-time on-device seek index for v1 clips (no inline index). Walks the size-prefix chain ONCE
// to build a sparse frame->byte table, cached on SD as "<video>.idx" so later resume/seek is an
// instant O(1) jump. Returns an open FILE* to read offsets from (caller fcloses it), *base = byte
// pos of offsets[0], *stride = frames per entry; NULL if it can't build.
#define NFVX_STRIDE 48
static FILE *video_index_sidecar(const char *vpath, FILE *f, const nfv_header_t *H, long fsz,
                                 long *base, uint32_t *stride, bool *aborted)
{
    if (aborted) *aborted = false;
    if (!H->frame_count) return NULL;
    char ipath[288]; snprintf(ipath, sizeof ipath, "%s.idx", vpath);

    FILE *sf = fopen(ipath, "rb");
    if (sf) {                                                 // reuse a valid cache
        char m[4]; uint8_t ver = 0, pad = 0; uint16_t stp = 0; uint32_t fc = 0, cnt = 0;
        bool ok = fread(m, 1, 4, sf) == 4 && memcmp(m, "NFVX", 4) == 0 &&
                  fread(&ver, 1, 1, sf) == 1 && fread(&pad, 1, 1, sf) == 1 &&
                  fread(&stp, 1, 2, sf) == 2 && fread(&fc, 1, 4, sf) == 4 && fread(&cnt, 1, 4, sf) == 4 &&
                  ver == 1 && stp > 0 && fc == H->frame_count && cnt == (H->frame_count + stp - 1) / stp;
        if (ok) { *base = 16; *stride = stp; return sf; }
        fclose(sf);                                           // stale/corrupt -> rebuild
    }

    char tpath[296]; snprintf(tpath, sizeof tpath, "%s.idxtmp", vpath);
    FILE *w = fopen(tpath, "wb"); if (!w) { ESP_LOGW(V_TAG, "idx: cannot create %s (errno %d)", tpath, errno); return NULL; }
    uint16_t stp = NFVX_STRIDE; uint32_t fc = H->frame_count, cnt = (fc + stp - 1) / stp;
    uint8_t hdr[16]; memcpy(hdr, "NFVX", 4); hdr[4] = 1; hdr[5] = 0;
    memcpy(hdr + 6, &stp, 2); memcpy(hdr + 8, &fc, 4); memcpy(hdr + 12, &cnt, 4);
    fwrite(hdr, 1, 16, w);

    // draw the wait screen once — only the percentage chip refreshes in the loop below
    d.fillScreen(BG);
    d.setTextSize(2); d.setTextColor(ACC, BG);
    { const char *h = "Preparazione indice"; d.setCursor((240 - (int)strlen(h) * 12) / 2, 8); d.print(h); }
    d.drawFastHLine(0, 28, 240, DIM);
    d.setTextSize(1); d.setTextColor(DIM, BG);
    { const char *s1 = "Analisi fotogrammi in corso."; d.setCursor((240 - (int)strlen(s1) * 6) / 2, 38); d.print(s1); }
    { const char *s2 = "Prima apertura: puo' richiedere"; d.setCursor((240 - (int)strlen(s2) * 6) / 2, 50); d.print(s2); }
    { const char *s3 = "qualche minuto di attesa."; d.setCursor((240 - (int)strlen(s3) * 6) / 2, 60); d.print(s3); }
    d.drawFastHLine(0, 104, 240, DIM);
    { const char *es = "[ ESC ]  annulla"; d.setCursor((240 - (int)strlen(es) * 6) / 2, 110); d.print(es); }

    fseek(f, sizeof *H, SEEK_SET);
    long off = (long)sizeof *H; bool ok = true;
    for (uint32_t i = 0; i < fc; i++) {
        if ((i % stp) == 0) { uint32_t o = (uint32_t)off; fwrite(&o, 1, 4, w); }
        uint32_t s; if (fread(&s, 1, 4, f) != 4) { ok = false; break; }
        off += 4 + (long)s;
        if (off > fsz) { ok = false; break; }
        if (fseek(f, (long)s, SEEK_CUR) != 0) { ok = false; break; }
        if ((i & 0x3f) == 0) {                                // every 64 frames: pet WDT, poll ESC, update %
            esp_task_wdt_reset();
            for (int g = 0; g < 8; g++) {                     // drain the key queue so an ESC tap is never missed
                nucleo_key_t k = nucleo_kbd_read();
                if (k.key == NK_NONE && k.ch == 0) break;
                if (k.key == NK_BACK || k.key == NK_DEL || k.ch == '`' || k.ch == 'q') {
                    if (aborted) *aborted = true;
                    ok = false;
                    break;
                }
            }
            if (!ok) break;                                   // aborted -> bail out of the scan
            char pb[6]; snprintf(pb, sizeof pb, "%d%%", (int)((int64_t)i * 100 / fc));
            d.fillRect(0, 70, 240, 30, BG);
            d.setTextSize(3); d.setTextColor(FG, BG);
            d.setCursor((240 - (int)strlen(pb) * 18) / 2, 74); d.print(pb);
        }
    }
    fclose(w);
    if (!ok) { remove(tpath); return NULL; }                  // aborted or read error: no half cache
    remove(ipath); rename(tpath, ipath);                      // publish atomically
    ESP_LOGI(V_TAG, "idx: cached %u entries for %s", (unsigned)cnt, vpath);
    sf = fopen(ipath, "rb"); if (!sf) { ESP_LOGW(V_TAG, "idx: reopen failed (errno %d)", errno); return NULL; }
    *base = 16; *stride = stp; return sf;
}

// Diagnostic breadcrumb to SD — survives a freeze+watchdog-reboot, read back over WiFi via
// /api/fs/read /data/.vtrace. Each call appends "<tag> free=.. lrg=.. t=.." (internal heap free +
// largest contiguous block + ms). `reset` truncates (a new playback session). The last line written
// before a hang pinpoints where the exit wedged — our only window, the JTAG console isn't cabled.
static void vtrace(const char *tag, bool reset)
{
    FILE *f = fopen(NUCLEO_SD_MOUNT "/data/.vtrace", reset ? "w" : "a");
    if (!f) return;
    fprintf(f, "%s free=%u lrg=%u t=%lld\n", tag,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
            (long long)(esp_timer_get_time() / 1000));
    fclose(f);
}

// ===== NFV v3 (tile-delta) playback =========================================================
// v3 splits each frame into a grid of equal tiles and stores, per frame, ONLY the tiles that
// changed (see tools/nfv/nfv3.py). The previous frame already lives in the ST7789's own GRAM
// (we draw direct, no RAM framebuffer), so a partial update costs zero extra RAM — in fact the
// reassembly buffer here (~template + one tile) is far smaller than v2's full-frame buffer.
//
// Every tile is encoded with the SAME quant + Huffman tables and the SAME dimensions, so the
// JPEG header is identical for all of them: we store it ONCE (the "template") and each tile
// carries only its entropy scan. A tile's full JPEG is rebuilt as `template + scan + FFD9` —
// byte-identical to what the PC encoder produced, so M5GFX's baseline decoder accepts it (proven
// by a host round-trip in tools/nfv/test_nfv3.py).
//
// Because delta frames are NOT self-contained, the player can't byte-jump to an arbitrary frame:
// it seeks to the nearest preceding keyframe (indexed) and replays the few deltas up to the
// target. Forward catch-up just applies the intervening deltas (cheap). Audio is embedded as the
// file's last section (played via nucleo_audio_play_window) or a sibling .mp3 (fallback).
static int play_nfv3(const char *vpath, const char *title, const char *reskey, int64_t start_ms,
                     const char *next_title)
{
    FILE *f = fopen(vpath, "rb");
    if (!f) return PR_STOP;
    uint8_t hb[64];
    if (fread(hb, 1, 64, f) != 64 || memcmp(hb, "NFV1", 4) || hb[4] != 3) { fclose(f); return PR_STOP; }
    auto le16 = [&](int o) { return (uint16_t)(hb[o] | (hb[o+1] << 8)); };
    auto le32 = [&](int o) { return (uint32_t)(hb[o] | (hb[o+1] << 8) | (hb[o+2] << 16) | ((uint32_t)hb[o+3] << 24)); };

    const uint8_t  flags        = hb[5];
    uint16_t       fps          = le16(10); if (!fps) fps = 12;
    const uint8_t  tile_w       = hb[16], tile_h = hb[17], cols = hb[18], rows = hb[19];
    const uint32_t frame_count  = le32(20);
    const uint32_t duration_hdr = le32(24);
    uint32_t       max_tile     = le32(28);
    const uint32_t template_off = le32(32);
    const uint16_t template_len = le16(36);
    const uint32_t index_off    = le32(40);
    const uint32_t index_count  = le32(44);
    const uint32_t audio_off    = le32(48);
    const uint32_t audio_len    = le32(52);
    const int      tile_count   = (int)cols * (int)rows;

    if (!frame_count || !template_len || tile_count <= 0 || tile_count > 255) { fclose(f); return PR_STOP; }
    if (max_tile < 64)    max_tile = 64;
    if (max_tile > 60000) max_tile = 60000;            // u16 scan field caps it anyway

    // ONE reusable JPEG buffer: [ shared template | tile scan | FF D9 ]. The template prefix is
    // written once; only the scan region is refilled per tile. Far smaller than v2's frame buffer,
    // so the big freed block goes to the Helix MP3 decoder (silent-audio OOM avoided).
    nucleo_app_release_buffers();
    const size_t jcap = (size_t)template_len + max_tile + 2;
    uint8_t *jbuf = (uint8_t *)malloc(jcap);
    if (!jbuf) {
        d.fillScreen(0); d.setTextColor(CO_TXT, 0); d.setTextSize(1);
        d.setCursor(20, 60); d.print("Not enough memory to play");
        vTaskDelay(pdMS_TO_TICKS(1500)); fclose(f); return PR_STOP;
    }
    fseek(f, template_off, SEEK_SET);
    if (fread(jbuf, 1, template_len, f) != template_len) { free(jbuf); fclose(f); return PR_STOP; }
    const long frames_begin = (long)template_off + template_len;

    d.fillScreen(0);

    // ---- timing ----
    const uint32_t tot_s  = frame_count ? (frame_count - 1) / fps : 0;
    const uint32_t dur_ms = duration_hdr ? duration_hdr : (uint32_t)((uint64_t)frame_count * 1000 / fps);
    if (start_ms < 0) start_ms = 0;
    if (dur_ms && start_ms > (int64_t)dur_ms) start_ms = dur_ms;

    // ---- audio: embedded window (preferred) or sibling .mp3 (fallback) ----
    char apath[256]; snprintf(apath, sizeof apath, "%s", vpath);
    { char *dot = strrchr(apath, '.'); if (dot) snprintf(dot, 5, ".mp3"); }
    bool have_audio = false;
    if (audio_len) {
        nucleo_audio_play_window(vpath, (uint32_t)start_ms, dur_ms, audio_off, audio_len);
        have_audio = true;
    } else if ((flags & 1) == 0 && access(apath, R_OK) == 0) {
        nucleo_audio_play_at(apath, (uint32_t)start_ms, dur_ms);
        have_audio = true;
    }
    if (have_audio) {
        if (s_vol_default > 0) nucleo_audio_set_volume(s_vol_default);
        nucleo_audio_fade_in(180);
    }

    const int64_t AUDIO_LAT_MS = 70;
    int64_t t0 = esp_timer_get_time() - start_ms * 1000;
    const int64_t period = 1000000 / fps;

    // ---- frame engine ----------------------------------------------------------------------
    // Invariant: `cur` = last fully-rendered frame; the file is positioned to read frame cur+1
    // (== next_to_read). seek_keyframe() repositions both; render_one() paints one frame's tiles.
    int64_t cur = -1;
    int64_t next_to_read = 0;
    bool    force_resync = true;       // first paint must seek to a keyframe

    // WDT-safe far seek (same rationale as play_nfv's seek_far): hop in bounded steps, pet between,
    // so a multi-second cluster-chain walk on a big v3 clip can't trip the 8s task-WDT. Rewind base is
    // frames_begin (the v3 frame region start).
    auto seek_far = [&](long off) {
        if (off < frames_begin) off = frames_begin;
        long pos = ftell(f);
        if (pos < 0 || off < pos) { fseek(f, frames_begin, SEEK_SET); pos = frames_begin; }
        const long STEP = 2L * 1024 * 1024;
        while (off - pos > STEP) { fseek(f, STEP, SEEK_CUR); pos += STEP; esp_task_wdt_reset(); }
        fseek(f, off - pos, SEEK_CUR);
    };

    auto seek_keyframe = [&](int64_t target) {
        long off = frames_begin; uint32_t kf = 0;
        if (index_count) {
            int L = 0, R = (int)index_count - 1, a = 0;
            uint8_t eb[8];
            while (L <= R) {                                   // largest keyframe idx <= target
                int mid = (L + R) / 2;
                esp_task_wdt_reset();                          // index probes on a big file are slow
                fseek(f, (long)index_off + (long)mid * 8, SEEK_SET);
                if (fread(eb, 1, 8, f) != 8) break;
                uint32_t fi = (uint32_t)(eb[0] | (eb[1]<<8) | (eb[2]<<16) | ((uint32_t)eb[3]<<24));
                if (fi <= (uint32_t)target) { a = mid; L = mid + 1; } else { R = mid - 1; }
            }
            fseek(f, (long)index_off + (long)a * 8, SEEK_SET);
            if (fread(eb, 1, 8, f) == 8) {
                kf  = (uint32_t)(eb[0] | (eb[1]<<8) | (eb[2]<<16) | ((uint32_t)eb[3]<<24));
                off = (long)(eb[4] | (eb[5]<<8) | (eb[6]<<16) | ((uint32_t)eb[7]<<24));
            }
        }
        seek_far(off);                                         // WDT-safe positioning to the keyframe
        next_to_read = kf;
        return (int64_t)kf;
    };

    auto render_one = [&]() -> bool {                          // draw frame `next_to_read`'s tiles
        uint8_t nb;
        if (fread(&nb, 1, 1, f) != 1) return false;            // EOF
        for (int i = 0; i < (int)nb; i++) {
            uint8_t hdr3[3];
            if (fread(hdr3, 1, 3, f) != 3) return false;
            uint8_t  ti   = hdr3[0];
            uint16_t slen = (uint16_t)(hdr3[1] | (hdr3[2] << 8));
            if (slen > max_tile) { fseek(f, slen, SEEK_CUR); return false; }   // corrupt -> bail
            if (fread(jbuf + template_len, 1, slen, f) != slen) return false;
            jbuf[template_len + slen]     = 0xFF;
            jbuf[template_len + slen + 1] = 0xD9;
            if (ti < tile_count) {
                int tx = (ti % cols) * tile_w, ty = (ti / cols) * tile_h;
                d.drawJpg(jbuf, (size_t)template_len + slen + 2, tx, ty);
            }
        }
        next_to_read++;
        return true;
    };

    // Bring the screen to `target`. Re-seeks to a keyframe on a forced resync (seek / overlay
    // repaint) or a backward move; otherwise just applies the forward deltas.
    auto advance_to = [&](int64_t target) -> bool {
        if (target == cur && !force_resync) return true;
        // Re-seek to a keyframe on a backward jump OR a BIG forward gap: replaying thousands of deltas
        // frame-by-frame to catch a far target is a multi-second hang. <=4-frame backward jitter is
        // tolerated (audio resync) to avoid a costly re-seek every frame.
        if (force_resync || cur < 0 || target < cur - 4 || target > cur + 96) {
            cur = seek_keyframe(target) - 1;
            force_resync = false;
        }
        while (cur < target) {
            if (!render_one()) return false;
            cur++;
            if ((cur & 255) == 0) esp_task_wdt_reset();        // long delta replay can't starve the WDT
        }
        return true;
    };

    if (start_ms > 0) {                                        // wind to the resume point
        uint32_t sf = (uint32_t)((int64_t)start_ms * fps / 1000);
        if (frame_count && sf >= frame_count) sf = frame_count - 1;
        char ts[8]; fmt_mmss(ts, sizeof ts, (uint32_t)(start_ms / 1000));
        char rb[24]; snprintf(rb, sizeof rb, "Resuming %s...", ts);
        d.setTextColor(CO_TXT, 0); d.setTextSize(1);
        d.setCursor((240 - (int)strlen(rb) * 6) / 2, 62); d.print(rb);
    }

    int     bri = nucleo_app_brightness();
    int64_t overlay_until = esp_timer_get_time() + 2500000;
    bool    help = false, paused = false, stop = false, need_full = false, ov_prev = false, ctrl_full = true;
    int     result = PR_STOP;
    uint32_t ui_sig = 0xffffffff;
    int64_t  seek_hold_until = 0; int64_t hold_frame = 0;
    int64_t  resync_until = 0;
    int      seek_step_s = 10, seek_dir = 0;
    int64_t  last_seek_us = 0, badge_until = 0;

    auto seek_ms = [&](int64_t ms) {
        if (ms < 0) ms = 0;
        if (dur_ms && ms > (int64_t)dur_ms) ms = dur_ms;
        uint32_t tf = (uint32_t)(ms * fps / 1000);
        if (frame_count && tf >= frame_count) tf = frame_count - 1;
        if (have_audio) nucleo_audio_seek((uint32_t)ms);
        t0 = esp_timer_get_time() - ms * 1000;
        hold_frame = tf; seek_hold_until = esp_timer_get_time() + 250000;
        resync_until = esp_timer_get_time() + 1200000;        // free-run until the audio task applies the seek (elapsed_ms stale meanwhile) -> backward jump must not snap back
        force_resync = true;                                   // land on a keyframe, then replay
        overlay_until = esp_timer_get_time() + 3000000; ui_sig = 0xffffffff;
    };

    while (!stop) {
        esp_task_wdt_reset();

        nucleo_key_t k = nucleo_kbd_read();
        if (k.key != NK_NONE) {
            int64_t now = esp_timer_get_time();
            int64_t pos_ms = have_audio ? (int64_t)nucleo_audio_elapsed_ms() : (now - t0) / 1000;
            if (k.key == NK_BACK || k.key == NK_DEL || k.ch == 'q' || k.ch == '`') { result = PR_STOP; stop = true; }
            else if (k.ch == ']')       { result = PR_NEXT; stop = true; }
            else if (k.ch == '[')       { result = PR_PREV; stop = true; }
            else if (k.key == NK_ENTER || k.ch == ' ' || k.ch == 'p') { paused = !paused; if (have_audio) nucleo_audio_toggle_pause(); overlay_until = now + 3000000; ui_sig = 0xffffffff; }
            else if (k.key == NK_TAB)   { help = !help; if (!help) need_full = true; ui_sig = 0xffffffff; }   // TAB = pannello Comandi
            else if (k.ch == 'm')       { nucleo_audio_set_mute(!nucleo_audio_is_muted()); overlay_until = now + 3000000; ui_sig = 0xffffffff; }
            else if (k.ch == 't')       { s_time_rem = !s_time_rem; overlay_until = now + 3000000; ui_sig = 0xffffffff; }
            else if (k.key == NK_LEFT || k.key == NK_RIGHT) {
                int dir = (k.key == NK_RIGHT) ? 1 : -1;
                if (dir == seek_dir && now - last_seek_us < 1200000) seek_step_s = (seek_step_s < 30) ? 30 : 60;
                else seek_step_s = 10;
                seek_dir = dir; last_seek_us = now; badge_until = now + 900000;
                seek_ms(pos_ms + (int64_t)dir * seek_step_s * 1000);
            }
            else if (k.key == NK_UP)    { if (nucleo_audio_is_muted()) nucleo_audio_set_mute(false); nucleo_audio_set_volume(nucleo_audio_volume() + 10); overlay_until = now + 3000000; ui_sig = 0xffffffff; }
            else if (k.key == NK_DOWN)  { if (nucleo_audio_is_muted()) nucleo_audio_set_mute(false); nucleo_audio_set_volume(nucleo_audio_volume() - 10); overlay_until = now + 3000000; ui_sig = 0xffffffff; }
            else if (k.ch == '=' || k.ch == '+') { nucleo_app_set_brightness(nucleo_app_brightness() + 10); bri = nucleo_app_brightness(); overlay_until = now + 3000000; ui_sig = 0xffffffff; }
            else if (k.ch == '-' || k.ch == '_') { nucleo_app_set_brightness(nucleo_app_brightness() - 10); bri = nucleo_app_brightness(); overlay_until = now + 3000000; ui_sig = 0xffffffff; }
            // 1-9 chapter-jump REMOVED (2026-06-21): on a huge clip a far jump COMPLETES the reposition
            // (trace si_ok) but the draw/decode loop then hangs with the heap at ~6KB largest block (no
            // PSRAM) — a physical limit on big jumps. Resume + arrow seeks (small hops) are safe and stay.
        }
        if (stop) break;

        if (help) {                                            // TAB cheat-sheet: pause the video pass, show keys + live Luce/Volume
            if (ui_sig) { draw_video_help(bri, nucleo_audio_volume(), nucleo_audio_is_muted()); ui_sig = 0; }
            vTaskDelay(pdMS_TO_TICKS(16));
            continue;
        }

        int64_t now = esp_timer_get_time();
        bool overlay_now = now < overlay_until;

        int64_t target;
        if (now < seek_hold_until)         target = hold_frame;
        else if (paused)                   target = cur < 0 ? 0 : cur;
        else if (now < resync_until)       target = (now - t0) / period;   // post-seek free-run until the audio task applies the seek (elapsed_ms stale until then) -> backward jump must not snap back
        else if (have_audio && nucleo_audio_is_playing()) {
            int64_t ms = (int64_t)nucleo_audio_elapsed_ms() - AUDIO_LAT_MS; if (ms < 0) ms = 0;
            target = ms * fps / 1000;
        } else                             target = (now - t0) / period;
        if (frame_count && target >= (int64_t)frame_count) target = frame_count - 1;
        if (target < 0) target = 0;

        if (overlay_now != ov_prev) { if (!overlay_now) need_full = true; else ctrl_full = true; ov_prev = overlay_now; ui_sig = 0xffffffff; }

        if (need_full) { force_resync = true; need_full = false; }     // repaint covered region
        if (target != cur || force_resync) {
            d.startWrite();
            d.setClipRect(0, 0, 240, overlay_now ? BARY : 135);   // ALWAYS clip: coded height (144) > panel (135), else the 3rd tile row bleeds = vertical scroll
            bool ok = advance_to(target);
            d.clearClipRect();
            d.endWrite();
            if (!ok) { ESP_LOGW(V_TAG, "nfv3 end: cur=%u/%u next=%u", (unsigned)cur, (unsigned)frame_count, (unsigned)next_to_read); result = PR_ENDED; break; }
        }

        bool seeking = now < badge_until;
        if (overlay_now) {
            uint32_t el = (cur < 0 ? 0 : (uint32_t)cur) / fps;
            bool muted = nucleo_audio_is_muted();
            bool near_end = tot_s && el + 10 >= tot_s && next_title && next_title[0];
            uint32_t sig = (el & 0x1fff) | ((uint32_t)(nucleo_audio_volume() / 10) << 13)
                         | ((uint32_t)(bri / 10) << 17) | ((uint32_t)paused << 21)
                         | ((uint32_t)(seeking ? 1 : 0) << 22) | ((uint32_t)muted << 23)
                         | ((uint32_t)s_time_rem << 24) | ((uint32_t)near_end << 25);
            if (sig != ui_sig) {
                ui_sig = sig;
                d.startWrite();
                draw_controller(title, next_title, el, tot_s, paused, muted, nucleo_audio_volume(),
                                bri, ctrl_full, seeking, seek_dir, seek_step_s, near_end);
                d.endWrite();
                ctrl_full = false;
            }
        }

        if (have_audio && !paused && cur >= (int64_t)frame_count - 1 && nucleo_audio_elapsed_ms() > dur_ms) { result = PR_ENDED; break; }
        if (!have_audio && !paused && (now - t0) / period >= (int64_t)frame_count) { result = PR_ENDED; break; }   // silent clip end

        vTaskDelay((target != cur || seeking) ? 1 : pdMS_TO_TICKS(8));
    }

    esp_task_wdt_reset();                          // cleanup below (SD write + audio stop) must not starve the WDT
    vtrace("nfv3_loopend", false);
    if (have_audio) nucleo_audio_stop();           // stop the decoder BEFORE the SD write (shared SPI bus) — see play_nfv
    vtrace("nfv3_audiostop", false);
    uint32_t watched = fps ? (uint32_t)(cur < 0 ? 0 : cur) / fps : 0;
    resume_save(reskey, result == PR_ENDED ? tot_s : watched, tot_s);
    vtrace("nfv3_resumed", false);
    free(jbuf);
    fclose(f);
    return result;
}

// Blocking modal playback of clip at `vpath` (sibling .mp3 inferred), starting at `start_ms`
// (0 = top). `title` is shown in the bar; `reskey` is the resume DB key. The audio task is the
// master clock: the video maps the audio's elapsed ms to a frame and skips ahead if it falls
// behind, so A/V never drifts. Controls: enter/space play-pause · ,/ seek (taps accelerate
// 10->30->60s) · ;. volume · [ ] prev/next clip · -/= brightness · m mute · t time · 0-9 jump %
// · TAB hold · esc stop. `next_title` (or NULL) is shown as "Up next" near the end for autoplay.
// Returns a PR_* code: PR_ENDED (natural end -> caller may auto-advance), PR_NEXT/PR_PREV (skip),
// PR_STOP (viewer stopped / failed to start). Does NOT restore the list — the caller does, so a
// back-to-back auto-advance/skip never flashes the browser between clips.
static int play_nfv(const char *vpath, const char *title, const char *reskey, int64_t start_ms,
                    const char *next_title)
{
    char apath[256]; snprintf(apath, sizeof apath, "%s", vpath);
    char *dot = strrchr(apath, '.'); if (dot) snprintf(dot, 5, ".mp3");

    FILE *f = fopen(vpath, "rb");
    if (!f) return PR_STOP;
    nfv_header_t H;
    if (fread(&H, 1, sizeof H, f) != sizeof H || memcmp(H.magic, "NFV1", 4) != 0) { fclose(f); return PR_STOP; }
    if (H.version == 3) { fclose(f); return play_nfv3(vpath, title, reskey, start_ms, next_title); }   // tile-delta path

    uint16_t fps = H.fps ? H.fps : 12;
    uint32_t maxf = H.max_frame_bytes;
    if (maxf < 1024 || maxf > 32768) maxf = 32768;             // sanity clamp on the buffer
    // Free the 32 KB shared UI canvas, then malloc a RIGHT-SIZED JPEG frame buffer. Real frames
    // are ~8 KB, so this hands the big contiguous block to the Helix MP3 decoder — without it the
    // sibling audio's MP3InitDecoder OOMs and the clip plays SILENT.
    nucleo_app_release_buffers();
    size_t bufcap = maxf;
    uint8_t *buf = (uint8_t *)malloc(bufcap);
    if (!buf) {
        d.fillScreen(0); d.setTextColor(CO_TXT, 0); d.setTextSize(1);
        d.setCursor(20, 60); d.print("Not enough memory to play");
        vTaskDelay(pdMS_TO_TICKS(1500)); fclose(f); return PR_STOP;
    }
    uint32_t last_sz = 0;
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, sizeof H, SEEK_SET);

    // ---- Seek index: an EXACT frame->byte table so resume/seek never crawls the clip. ----
    FILE    *idxf = NULL;
    long     idx_base = 0;
    uint32_t idx_stride = 0;
    bool     idx_owned = false;
    {
        uint16_t stp = H.reserved0;
        uint32_t ioff = 0; memcpy(&ioff, H.reserved1, sizeof ioff);
        if ((H.flags & 2) && stp && (long)ioff >= (long)sizeof H && (long)ioff < fsz) {
            // Inline index: SHARE the frame handle. A separate fopen handle sat at byte 0 and paid a
            // maximal 0->EOF cluster-chain walk on its FIRST index read (= the freeze), plus a ~4KB
            // in-play FIL/sector-cache alloc. One handle + WDT-safe seeks (seek_far) is the right shape.
            idxf = f; idx_base = (long)ioff + 4; idx_stride = stp;
        }
    }

    d.fillScreen(0);
    if (!idxf) {                                              // v1 clip: build/load the SD sidecar
        long b; uint32_t stp; bool aborted = false;
        FILE *sf = video_index_sidecar(vpath, f, &H, fsz, &b, &stp, &aborted);
        if (aborted) { free(buf); fclose(f); return PR_STOP; }   // user pressed esc during "Preparing"
        if (sf) { idxf = sf; idx_base = b; idx_stride = stp; idx_owned = true; }
        fseek(f, sizeof H, SEEK_SET);
    }
    if (start_ms > 0) {
        char ts[8]; fmt_mmss(ts, sizeof ts, (uint32_t)(start_ms / 1000));
        char rb[24]; snprintf(rb, sizeof rb, "Resuming %s...", ts);
        d.setTextColor(CO_TXT, 0); d.setTextSize(1);
        d.setCursor((240 - (int)strlen(rb) * 6) / 2, 62); d.print(rb);
    }

    const int64_t period = 1000000 / fps;
    const uint32_t tot_s = H.frame_count ? (H.frame_count - 1) / fps : 0;
    const uint32_t dur_ms = H.duration_ms ? H.duration_ms : (uint32_t)((uint64_t)H.frame_count * 1000 / fps);
    if (start_ms < 0) start_ms = 0;
    if (dur_ms && start_ms > (int64_t)dur_ms) start_ms = dur_ms;

    bool have_audio = (H.flags & 1) && access(apath, R_OK) == 0;
    if (have_audio) {
        nucleo_audio_play_at(apath, (uint32_t)start_ms, dur_ms);
        if (s_vol_default > 0) nucleo_audio_set_volume(s_vol_default);
        nucleo_audio_fade_in(180);                            // pop-free start (see nucleo_audio)
    }

    const int64_t AUDIO_LAT_MS = 70;
    int64_t t0 = esp_timer_get_time() - start_ms * 1000;

    int     bri = nucleo_app_brightness();
    int64_t overlay_until = esp_timer_get_time() + 2500000;
    bool    help = false, paused = false, stop = false, need_full = false, ov_prev = false, ctrl_full = true;
    int     result = PR_STOP;
    uint32_t cur = 0, next_idx = 0;
    int64_t  cached_i = -1; long cached_foff = 0;   // 1-entry index cache: repeated 1-9 in the same stride bucket skips the walk+4B read
    uint32_t ui_sig = 0xffffffff;
    int64_t  seek_hold_until = 0; uint32_t hold_frame = 0;
    int64_t  resync_until = 0;
    int      seek_step_s = 10, seek_dir = 0;
    int64_t  last_seek_us = 0, badge_until = 0;
    // Video-stall self-heal: track when `cur` last advanced. If the audio clock keeps running but the
    // frame cursor is wedged (a mis-seek or an oversize/corrupt-frame bail leaves next_idx past target,
    // so want_load never fires again), the clip plays "audio, no video" — and on exit cur/fps==0 so the
    // resume point is lost too. We detect the stall below and force an exact index re-seek to re-lock.
    int64_t  cur_moved_us = esp_timer_get_time();
    uint32_t cur_seen = 0xffffffff;

    // WDT-safe far seek. FATFS without fast-seek walks the cluster chain O(distance) INSIDE one
    // blocking fseek(); on a 300-500MB clip that exceeds the 8s task-WDT and hard-freezes the system,
    // and a single fseek cannot be interrupted to pet the dog. So split a long move into bounded
    // forward hops and pet between them: a slow seek degrades to "takes a moment", never a freeze.
    // Backward targets restart the FATFS walk from cluster 0, so rewind to the header and hop forward.
    auto seek_far = [&](FILE *fp, long off) {
        if (off < (long)sizeof H) off = (long)sizeof H;
        long pos = ftell(fp);
        if (pos < 0 || off < pos) { fseek(fp, (long)sizeof H, SEEK_SET); pos = (long)sizeof H; }
        const long STEP = 2L * 1024 * 1024;                   // ~2MB/hop (=64 clusters), no single hop can approach the 8s WDT even under W5500+audio bus contention
        while (off - pos > STEP) { fseek(fp, STEP, SEEK_CUR); pos += STEP; esp_task_wdt_reset(); }
        fseek(fp, off - pos, SEEK_CUR);
    };

    auto load_frame = [&](uint32_t target) -> bool {
        while (next_idx < target) {
            uint32_t s; if (fread(&s, 1, 4, f) != 4) return false;
            fseek(f, s, SEEK_CUR); next_idx++;
            if ((next_idx & 255) == 0) esp_task_wdt_reset();  // a big forward skip can't starve the WDT
        }
        uint32_t s; if (fread(&s, 1, 4, f) != 4) return false;
        if (s > maxf) { fseek(f, s, SEEK_CUR); next_idx = target + 1; return false; }
        if (fread(buf, 1, s, f) != s) return false;
        last_sz = s; next_idx = target + 1; return true;
    };

    auto seek_index = [&](uint32_t tf) -> bool {
        if (!idxf || !idx_stride) return false;
        uint32_t i = tf / idx_stride;
        long foff;
        if ((int64_t)i == cached_i) {                         // same stride bucket as last seek: skip the index walk + 4B read
            foff = cached_foff;
        } else {
            seek_far(idxf, idx_base + (long)i * 4);            // WDT-safe (idxf shares f): read the offset...
            uint32_t fo;
            if (fread(&fo, 1, 4, idxf) != 4) return false;
            if ((long)fo < (long)sizeof H || (long)fo >= fsz) return false;
            foff = (long)fo; cached_i = (int64_t)i; cached_foff = foff;
        }
        seek_far(f, foff);                                    // ...then WDT-safe positioning to the frame
        next_idx = i * idx_stride;
        return true;
    };

    auto seek_approx = [&](uint32_t tf) -> bool {
        if (!H.frame_count || fsz <= (long)sizeof H) return false;
        long est = (long)sizeof H + (long)((int64_t)(fsz - (long)sizeof H) * tf / H.frame_count);
        if (est < (long)sizeof H) est = sizeof H;
        seek_far(f, est);                                     // WDT-safe (was a raw fseek to a far offset)
        int prev = -1; long pos = est, limit = (long)maxf * 4 + 64;
        for (long i = 0; i < limit; i++) {
            int c = fgetc(f); if (c < 0) break; pos++;
            if ((i & 8191) == 8191) esp_task_wdt_reset();     // the byte-scan fallback must not starve the WDT
            if (prev == 0xFF && c == 0xD8) { fseek(f, pos - 6, SEEK_SET); next_idx = tf; return true; }
            prev = c;
        }
        next_idx = tf;                                        // even on miss: don't leave load_frame to crawl from a stale next_idx
        return false;
    };

    auto seek_ms = [&](int64_t ms) {
        if (ms < 0) ms = 0;
        if (dur_ms && ms > (int64_t)dur_ms) ms = dur_ms;
        uint32_t tf = (uint32_t)(ms * fps / 1000);
        if (H.frame_count && tf >= H.frame_count) tf = H.frame_count - 1;
        if (have_audio) nucleo_audio_seek((uint32_t)ms);
        t0 = esp_timer_get_time() - ms * 1000;
        if (!seek_index(tf)) seek_approx(tf);
        hold_frame = tf; seek_hold_until = esp_timer_get_time() + 250000;
        resync_until = esp_timer_get_time() + 1200000;        // free-run from the seek target until the audio task applies the seek (elapsed_ms is stale meanwhile) -> a backward jump must not snap back to the old position
        cur = 0xffffffff; need_full = false;
        overlay_until = esp_timer_get_time() + 3000000; ui_sig = 0xffffffff;
    };

    if (start_ms > 0) {                                       // wind to the resume point in O(1)
        uint32_t sf = (uint32_t)((int64_t)start_ms * fps / 1000);
        if (H.frame_count && sf >= H.frame_count) sf = H.frame_count - 1;
        if (!seek_index(sf)) seek_approx(sf);
    }

    while (!stop) {
        esp_task_wdt_reset();

        nucleo_key_t k = nucleo_kbd_read();
        if (k.key != NK_NONE) {
            int64_t now = esp_timer_get_time();
            int64_t pos_ms = have_audio ? (int64_t)nucleo_audio_elapsed_ms() : (now - t0) / 1000;
            if (k.key == NK_BACK || k.key == NK_DEL || k.ch == 'q' || k.ch == '`') { result = PR_STOP; stop = true; }
            else if (k.ch == ']')       { result = PR_NEXT; stop = true; }   // skip to next clip
            else if (k.ch == '[')       { result = PR_PREV; stop = true; }   // skip to previous clip
            else if (k.key == NK_ENTER || k.ch == ' ' || k.ch == 'p') { paused = !paused; if (have_audio) nucleo_audio_toggle_pause(); overlay_until = now + 3000000; ui_sig = 0xffffffff; }
            else if (k.key == NK_TAB)   { help = !help; if (!help) need_full = true; ui_sig = 0xffffffff; }   // TAB = pannello Comandi
            else if (k.ch == 'm')       { nucleo_audio_set_mute(!nucleo_audio_is_muted()); overlay_until = now + 3000000; ui_sig = 0xffffffff; }   // mute toggle
            else if (k.ch == 't')       { s_time_rem = !s_time_rem; overlay_until = now + 3000000; ui_sig = 0xffffffff; }   // elapsed/remaining (persist via VIEW>Time; no SD write mid-play)
            else if (k.key == NK_LEFT || k.key == NK_RIGHT) {
                int dir = (k.key == NK_RIGHT) ? 1 : -1;
                if (dir == seek_dir && now - last_seek_us < 1200000) seek_step_s = (seek_step_s < 30) ? 30 : 60;
                else seek_step_s = 10;
                seek_dir = dir; last_seek_us = now; badge_until = now + 900000;
                seek_ms(pos_ms + (int64_t)dir * seek_step_s * 1000);
            }
            else if (k.key == NK_UP)    { if (nucleo_audio_is_muted()) nucleo_audio_set_mute(false); nucleo_audio_set_volume(nucleo_audio_volume() + 10); overlay_until = now + 3000000; ui_sig = 0xffffffff; }
            else if (k.key == NK_DOWN)  { if (nucleo_audio_is_muted()) nucleo_audio_set_mute(false); nucleo_audio_set_volume(nucleo_audio_volume() - 10); overlay_until = now + 3000000; ui_sig = 0xffffffff; }
            else if (k.ch == '=' || k.ch == '+') { nucleo_app_set_brightness(nucleo_app_brightness() + 10); bri = nucleo_app_brightness(); overlay_until = now + 3000000; ui_sig = 0xffffffff; }
            else if (k.ch == '-' || k.ch == '_') { nucleo_app_set_brightness(nucleo_app_brightness() - 10); bri = nucleo_app_brightness(); overlay_until = now + 3000000; ui_sig = 0xffffffff; }
            // 1-9 chapter-jump REMOVED (2026-06-21): on a huge clip a far jump COMPLETES the reposition
            // (trace si_ok) but the draw/decode loop then hangs with the heap at ~6KB largest block (no
            // PSRAM) — a physical limit on big jumps. Resume + arrow seeks (small hops) are safe and stay.
        }
        if (stop) break;

        if (help) {                                            // TAB cheat-sheet: pause the video pass, show keys + live Luce/Volume
            if (ui_sig) { draw_video_help(bri, nucleo_audio_volume(), nucleo_audio_is_muted()); ui_sig = 0; }
            vTaskDelay(pdMS_TO_TICKS(16));
            continue;
        }

        int64_t now = esp_timer_get_time();
        bool overlay_now = now < overlay_until;

        uint32_t target;
        if (now < seek_hold_until)         target = hold_frame;
        else if (paused)                   target = cur;
        else if (now < resync_until)       target = (uint32_t)((now - t0) / period);   // post-seek: free-run from the seek target until the audio decoder repositions (elapsed_ms is stale until its task runs poll_seek) -> stops a big/backward jump snapping back to the pre-seek position
        else if (have_audio && nucleo_audio_is_playing()) {
            int64_t ms = (int64_t)nucleo_audio_elapsed_ms() - AUDIO_LAT_MS; if (ms < 0) ms = 0;
            target = (uint32_t)(ms * fps / 1000);
        } else                             target = (uint32_t)((now - t0) / period);
        if (H.frame_count && target >= H.frame_count) target = H.frame_count - 1;

        // Self-heal a wedged frame cursor (see cur_moved_us decl). The stuck signature is: audio is the
        // live clock, the target has run ahead of cur, yet want_load is dead because next_idx overshot
        // target (target < next_idx). If that persists past a grace window, re-lock with an exact index
        // seek (approx fallback for index-less v1 clips) so load_frame draws the live frame again.
        if (cur != cur_seen) { cur_seen = cur; cur_moved_us = now; }
        else if (!paused && have_audio && nucleo_audio_is_playing()
                 && now >= seek_hold_until && now >= resync_until
                 && (int64_t)target > (int64_t)cur + 2 && (int64_t)target < (int64_t)next_idx
                 && now - cur_moved_us > 1200000) {
            if (!seek_index(target)) seek_approx(target);    // re-position f to a real frame boundary
            cur_moved_us = now;                              // fresh grace window for the re-seek
            ESP_LOGW(V_TAG, "nfv stall heal: cur=%u target=%u next_idx=%u", (unsigned)cur, (unsigned)target, (unsigned)next_idx);
        }

        if (overlay_now != ov_prev) { if (!overlay_now) need_full = true; else ctrl_full = true; ov_prev = overlay_now; ui_sig = 0xffffffff; }

        // FORWARD-ONLY. The loop holds (no backward walk) on small jitter — that killed the 1-9 lag.
        // BUT a BIG forward gap must never be crawled frame-by-frame: on a 120k-frame clip that's a
        // multi-MINUTE hang (load_frame pets the WDT, so it's not a reboot — it's a true freeze, the
        // "video non arriva"). If the target is far ahead of the read cursor, jump there via the index
        // in O(1) (seek_far is WDT-safe), so load_frame only skips the short remainder.
        bool want_load = (int64_t)target >= (int64_t)next_idx;
        if (want_load && idx_stride && (int64_t)target > (int64_t)next_idx + 4 * (int64_t)idx_stride) {
            seek_index(target);   // big forward gap -> O(1) index jump, never crawl frame-by-frame
        }
        if (want_load || (need_full && last_sz)) {
            bool have = want_load ? load_frame(target) : true;
            if (have) {
                d.startWrite();
                if (overlay_now) d.setClipRect(0, 0, 240, BARY);
                d.drawJpg(buf, last_sz, 0, 0);
                if (overlay_now) d.clearClipRect();
                d.endWrite();
                cur = target; need_full = false;
            } else if (next_idx >= H.frame_count) { result = PR_ENDED; break; }
            else { static int s_lf = 0; if (s_lf++ < 8) ESP_LOGW(V_TAG, "nfv loadfail: cur=%u target=%u next_idx=%u/%u maxf=%u", (unsigned)cur, (unsigned)target, (unsigned)next_idx, (unsigned)H.frame_count, (unsigned)maxf); }
        }

        bool seeking = now < badge_until;
        if (overlay_now) {
            uint32_t el = cur / fps;
            bool muted = nucleo_audio_is_muted();
            bool near_end = tot_s && el + 10 >= tot_s && next_title && next_title[0];
            uint32_t sig = (el & 0x1fff) | ((uint32_t)(nucleo_audio_volume() / 10) << 13)
                         | ((uint32_t)(bri / 10) << 17) | ((uint32_t)paused << 21)
                         | ((uint32_t)(seeking ? 1 : 0) << 22) | ((uint32_t)muted << 23)
                         | ((uint32_t)s_time_rem << 24) | ((uint32_t)near_end << 25);
            if (sig != ui_sig) {
                ui_sig = sig;
                d.startWrite();
                draw_controller(title, next_title, el, tot_s, paused, muted, nucleo_audio_volume(),
                                bri, ctrl_full, seeking, seek_dir, seek_step_s, near_end);
                d.endWrite();
                ctrl_full = false;
            }
        }

        if (have_audio && !paused && cur >= H.frame_count - 1 && nucleo_audio_elapsed_ms() > H.duration_ms) { result = PR_ENDED; break; }

        vTaskDelay((want_load || seeking) ? 1 : pdMS_TO_TICKS(8));
    }

    esp_task_wdt_reset();                          // cleanup below (SD write + audio stop) must not starve the WDT
    vtrace("nfv_loopend", false);
    ESP_LOGW(V_TAG, "nfv end: result=%d cur=%u/%u next_idx=%u maxf=%u audio=%d elapsed=%u dur=%u",
             result, (unsigned)cur, (unsigned)H.frame_count, (unsigned)next_idx, (unsigned)maxf,
             (int)have_audio, (unsigned)nucleo_audio_elapsed_ms(), (unsigned)dur_ms);
    // Stop the audio decoder before the SD work below: frees its RAM and nothing's left to play. (The
    // real exit-freeze cause was a STACK overflow in resume_save — its 48-entry array, now a heap
    // scratch buffer s_resume_scratch — not SD contention; this ordering is just good hygiene.)
    if (have_audio) nucleo_audio_stop();
    vtrace("nfv_audiostop", false);
    uint32_t watched = fps ? cur / fps : 0;
    resume_save(reskey, result == PR_ENDED ? tot_s : watched, tot_s);   // remember position (or clear if done)
    vtrace("nfv_resumed", false);
    if (idx_owned && idxf) fclose(idxf);
    free(buf);
    fclose(f);
    return result;
}

// Return to the list after the playback modal(s). play_nfv frees the shared UI canvas to hand the
// contiguous block to the MP3 decoder; the audio task frees its RAM on stop() asynchronously, so
// retry briefly — the canvas usually re-acquires within a frame or two, restoring the buffered
// (flicker-free) path immediately.
static void return_to_list(void)
{
    for (int i = 0; i < 8 && !nucleo_screen_acquire(); i++) { esp_task_wdt_reset(); vTaskDelay(pdMS_TO_TICKS(20)); }
    d.fillScreen(BG);
    nucleo_app_request_draw();
}

// ---- dense fisheye list rendering (mirrors the Music browser) -------------------------------
static void icon_folder(int x, int y, unsigned short c)
{
    d.fillRect(x + 1, y, 6, 2, c);
    d.fillRoundRect(x, y + 2, 12, 8, 2, c);
}
static void icon_film(int x, int y, unsigned short c)       // outlined frame + play triangle
{
    d.drawRoundRect(x, y, 12, 9, 2, c);
    d.fillTriangle(x + 4, y + 2, x + 4, y + 6, x + 8, y + 4, c);
}
static void mark_play(int x, int y, unsigned short c) { d.fillTriangle(x, y, x, y + 6, x + 5, y + 3, c); }

// Draw one list row, self-clearing its own background so it is correct in BOTH the buffered and
// the DIRECT render path (the latter has no canvas pre-fill — the v1 selection-page flicker).
static void draw_row(int i, int y, int h, bool focus)
{
    VEntry *r = &st->e[i];
    bool is_resume = !r->dir && r->resume > 0;               // "continue watching" -> green ▶ + time
    unsigned short acc = r->dir ? AMB : ACC;

    d.fillRect(0, y, 240, h, BG);                            // clear this row first

    // right-aligned meta: clip count for folders, resume time for in-progress clips, else duration
    char mb[12] = "";
    if (r->dir)         snprintf(mb, sizeof mb, "%u", (unsigned)r->count);
    else if (is_resume) fmt_mmss(mb, sizeof mb, r->resume);
    else if (r->dur)    fmt_mmss(mb, sizeof mb, r->dur);
    int mw = mb[0] ? (int)strlen(mb) * 6 : 0;

    char disp[72]; snprintf(disp, sizeof disp, "%s", r->name);
    if (!r->dir) { char *dt = strrchr(disp, '.'); if (dt) *dt = 0; }   // hide the .nfv extension

    int my = y + (h - 8) / 2;                                // meta text baseline
    if (focus) {
        d.fillRoundRect(4, y + 1, 232, h - 2, 7, acc);       // bright capsule, INK content
        int iy = y + (h - 12) / 2;
        if (r->dir) icon_folder(12, iy + 1, INK); else icon_film(12, iy + 1, INK);

        int meta_x = 230 - mw;
        if (mb[0]) {
            if (is_resume) { mark_play(meta_x - 8, my + 1, INK); meta_x -= 8; }
            d.setTextSize(1); d.setTextColor(INK, acc); d.setCursor(230 - mw, my); d.print(mb);
        }
        int nx = 30, navail = (mb[0] ? meta_x - 6 : 230) - nx; if (navail < 12) navail = 12;
        int maxc = navail / 12; if (maxc < 1) maxc = 1; if (maxc > 22) maxc = 22;
        char nb[24]; snprintf(nb, sizeof nb, "%.*s", maxc, disp);
        d.setTextSize(2); d.setTextColor(INK, acc); d.setCursor(nx, y + (h - 16) / 2); d.print(nb);
    } else {
        unsigned short namec = r->dir ? AMB : FG;
        unsigned short metac = is_resume ? GRN : MUTED;
        if (r->dir) icon_folder(9, y + (h - 10) / 2, AMB); else icon_film(9, y + (h - 9) / 2, ACC);

        int meta_x = 230 - mw;
        if (mb[0]) {
            if (is_resume) { mark_play(meta_x - 8, my + 1, GRN); meta_x -= 8; }
            d.setTextSize(1); d.setTextColor(metac, BG); d.setCursor(230 - mw, my); d.print(mb);
        }
        int nx = 24, navail = (mb[0] ? meta_x - 6 : 230) - nx; if (navail < 6) navail = 6;
        int maxc = navail / 12; if (maxc < 1) maxc = 1; if (maxc > 18) maxc = 18;
        char nb[20]; snprintf(nb, sizeof nb, "%.*s", maxc, disp);
        d.setTextSize(2); d.setTextColor(namec, BG); d.setCursor(nx, y + (h - 16) / 2); d.print(nb);
    }
}

static void draw_list(int y0, int region_h)
{
    if (s_sel < s_scroll) s_scroll = s_sel;
    int scan_y = 0;
    for (int i = s_scroll; i <= s_sel; i++) scan_y += (i == s_sel) ? 28 : 20;
    while (scan_y > region_h && s_scroll < s_sel) { scan_y -= (s_scroll == s_sel) ? 28 : 20; s_scroll++; }

    int y = y0;
    for (int i = s_scroll; i < s_n && y < y0 + region_h; i++) {
        int h = (i == s_sel) ? 28 : 20;
        if (y + h > y0 + region_h && i != s_scroll) break;
        draw_row(i, y, h, i == s_sel);
        y += h;
    }
    if (y < y0 + region_h) d.fillRect(0, y, 240, (y0 + region_h) - y, BG);   // clear the tail

    if (s_n > 1) {                                            // capsule scroll knob over a faint track
        int track = region_h - 6; if (track < 12) track = 12;
        int kh = track * region_h / (16 * s_n); if (kh < 12) kh = 12; if (kh > track) kh = track;
        int ky = y0 + 3 + (track - kh) * s_sel / (s_n - 1);
        d.fillRoundRect(236, y0 + 3, 2, track, 1, LINE);
        d.fillRoundRect(236, ky, 2, kh, 1, ACC);
    }
}

// Slim context header: accent title (folder / "Video") + clip count + accent rule.
static void draw_header(int top, int count)
{
    char ctx[24];
    if (strcmp(s_path, "/")) {                               // in a sub-folder -> show the path tail
        const char *p = s_path + 1;
        snprintf(ctx, sizeof ctx, "%s", p);
        int l = strlen(ctx); if (l && ctx[l - 1] == '/') ctx[--l] = 0;
        if (l > 15) memmove(ctx, ctx + (l - 15), 16);
    } else snprintf(ctx, sizeof ctx, "Video");

    d.fillRect(0, top, 240, HEAD_H, BG);
    char t[18]; snprintf(t, sizeof t, "%.15s", ctx);
    d.setTextSize(2); d.setTextColor(ACC, BG); d.setCursor(8, top + 1); d.print(t);
    if (count >= 0) {
        char c[8]; snprintf(c, sizeof c, "%d", count);
        int cw = (int)strlen(c) * 6;
        d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(234 - cw, top + 5); d.print(c);
    }
    d.drawFastHLine(8, top + HEAD_H - 2, 224, LINE);
    int uw = (int)strlen(t) * 12; if (uw > 150) uw = 150;
    d.fillRect(8, top + HEAD_H - 2, uw, 2, ACC);
}

// ---- settings sheet (TAB) — mirrors the Music app's 4-tab design ---------------------------
enum { SV_TEXT = 0, SV_TOGGLE, SV_SLIDER, SV_ACTION };

// Persistent segmented tab bar.
static void draw_tabbar(int active, bool hdr)
{
    d.fillRect(0, 0, 240, 24, BG);
    int seg = 240 / 4;
    for (int i = 0; i < 4; i++) {
        int x = i * seg, tw = (int)strlen(TABS[i]) * 6;
        if (i == active) {
            d.fillRoundRect(x + 3, 3, seg - 6, 17, 8, ACC);
            d.setTextSize(1); d.setTextColor(INK, ACC); d.setCursor(x + (seg - tw) / 2, 8); d.print(TABS[i]);
        } else {
            d.setTextSize(1); d.setTextColor(hdr ? MUTED : DIM, BG); d.setCursor(x + (seg - tw) / 2, 8); d.print(TABS[i]);
        }
    }
    d.drawFastHLine(0, 23, 240, LINE);
}

static void draw_set_row(int y, bool focus, const char *label, const char *val,
                         int kind, bool on, int slider_val)
{
    int h = focus ? 50 : 32;
    d.fillRoundRect(4, y, 232, h - 2, 9, focus ? CAP : BG);
    if (focus) d.fillRoundRect(4, y + 3, 5, h - 8, 2, ACC);   // accent rail

    d.setTextSize(2); d.setTextColor(focus ? FG : MUTED, focus ? CAP : BG);
    d.setCursor(16, y + (h - 16) / 2 - 1); d.print(label);

    if (kind == SV_SLIDER) {
        bool edit = focus && s_set_edit;
        int sw = focus ? 96 : 64, sh = 12, bx = 230 - sw, vy = y + (h - sh) / 2;
        d.fillRoundRect(bx, vy, sw, sh, sh / 2, SURF);
        int onw = slider_val * sw / 100; if (onw < 0) onw = 0; if (onw > sw) onw = sw;
        if (onw > 0) d.fillRoundRect(bx, vy, onw, sh, sh / 2, GRN);
        int kx = bx + onw; if (kx < bx + 6) kx = bx + 6; if (kx > bx + sw - 6) kx = bx + sw - 6;
        d.fillCircle(kx, vy + sh / 2, edit ? sh / 2 + 2 : sh / 2 + 1, FG);
        if (edit) d.drawRoundRect(bx - 2, vy - 2, sw + 4, sh + 4, (sh + 4) / 2, ACC);
        return;
    }
    if (kind == SV_TOGGLE) {
        int sw = 42, sh = 20, bx = 230 - sw, vy = y + (h - sh) / 2;
        d.fillRoundRect(bx, vy, sw, sh, sh / 2, on ? GRN : SURF);
        int kx = on ? bx + sw - sh / 2 - 1 : bx + sh / 2 + 1;
        d.fillCircle(kx, vy + sh / 2, sh / 2 - 3, on ? INK : MUTED);
        return;
    }
    if (kind == SV_ACTION) {
        int bw = 28, bh = 22, bx = 230 - bw, vy = y + (h - bh) / 2;
        d.fillRoundRect(bx, vy, bw, bh, 6, focus ? ACC : SURF);
        unsigned short ar = focus ? INK : MUTED;
        int ax = bx + bw / 2 - 2, ay = vy + bh / 2;
        d.fillTriangle(ax, ay - 4, ax, ay + 4, ax + 4, ay, ar);    // chevron
        return;
    }
    if (val && val[0]) {                                      // SV_TEXT value chip
        int vw = (int)strlen(val) * 12 + 14, vh = 22, bx = 230 - vw, vy = y + (h - vh) / 2;
        if (focus) d.fillRoundRect(bx, vy, vw, vh, 6, SURF);
        d.setTextSize(2); d.setTextColor(focus ? FG : MUTED, focus ? SURF : BG);
        d.setCursor(bx + 7, vy + 3); d.print(val);
    }
}

static const char *repeat_str(void) { return s_repeat == 0 ? "Off" : s_repeat == 1 ? "One" : "All"; }
static const char *resume_str(void) { return s_resume_mode == 0 ? "Ask" : s_resume_mode == 1 ? "Auto" : "Off"; }

struct SetItem { const char *label; char val[16]; int kind; bool on; int slider; };

static int build_items(SetItem *it)
{
    memset(it, 0, sizeof(SetItem) * 6);
    if (s_set_tab == 0) {                                     // PLAY
        it[0].label = "Autoplay"; it[0].kind = SV_TOGGLE; it[0].on = s_autoplay;
        it[1].label = "Repeat";   it[1].kind = SV_TEXT;   snprintf(it[1].val, 16, "%s", repeat_str());
        it[2].label = "Resume";   it[2].kind = SV_TEXT;   snprintf(it[2].val, 16, "%s", resume_str());
        return 3;
    }
    if (s_set_tab == 1) {                                     // AUDIO
        it[0].label = "Volume";    it[0].kind = SV_SLIDER; it[0].slider = nucleo_audio_volume();
        it[1].label = "Start Vol"; it[1].kind = SV_TEXT;   snprintf(it[1].val, 16, s_vol_default ? "%d%%" : "Last", s_vol_default);
        return 2;
    }
    if (s_set_tab == 2) {                                     // VIEW
        it[0].label = "Bright"; it[0].kind = SV_SLIDER; it[0].slider = nucleo_app_brightness();
        it[1].label = "Sort";   it[1].kind = SV_TEXT;   snprintf(it[1].val, 16, s_sort == 0 ? "Name" : "Length");
        it[2].label = "Time";   it[2].kind = SV_TEXT;   snprintf(it[2].val, 16, s_time_rem ? "Left" : "Total");
        return 3;
    }
    // LIBRARY
    it[0].label = "Continue"; it[0].kind = SV_TEXT; { int n = resume_refresh(); snprintf(it[0].val, 16, "%d", n); }
    it[1].label = "Clear All"; it[1].kind = SV_ACTION;
    it[2].label = "Rescan";    it[2].kind = SV_ACTION;
    it[3].label = "Help";      it[3].kind = SV_ACTION;
    return 4;
}

static void draw_settings(int ch)
{
    d.fillRect(0, 0, 240, ch, BG);
    bool hdr_mode = (s_set_row == -1);
    draw_tabbar(s_set_tab, hdr_mode);

    SetItem it[6]; int n = build_items(it);

    d.setClipRect(0, 24, 240, ch - 36);
    if (hdr_mode) {
        int y = 30;                                          // dimmed preview; DOWN to focus
        for (int i = 0; i < n && y < ch - 12; i++) {
            draw_set_row(y, false, it[i].label, it[i].val, it[i].kind, it[i].on, it[i].slider);
            y += 34;
        }
    } else {
        int cy = (28 + ch) / 2, f = s_set_row;
        for (int i = 0; i < n; i++) {
            int dist = i - f, h = (dist == 0) ? 50 : 32, y;
            if (dist == 0)     y = cy - h / 2;
            else if (dist < 0) y = cy - 25 + dist * 32;
            else               y = cy + 25 + (dist - 1) * 32;
            if (y + h > 24 && y < ch - 12)
                draw_set_row(y, i == f, it[i].label, it[i].val, it[i].kind, it[i].on, it[i].slider);
        }
    }
    d.clearClipRect();

    d.setTextSize(1); d.setTextColor(DIM, BG); d.setCursor(8, ch - 10);
    if (s_set_edit)     d.print("L/R adjust   ENTER done");
    else if (hdr_mode)  d.print("RIGHT tab   DOWN rows   ESC close");
    else                d.print("UP/DN row   RIGHT tab   ENTER ok");
}

// Adjust the focused slider (AUDIO>Volume or VIEW>Bright) by delta.
static void slider_adjust(int delta)
{
    if (s_set_tab == 1) nucleo_audio_set_volume(nucleo_audio_volume() + delta);
    else if (s_set_tab == 2) nucleo_app_set_brightness(nucleo_app_brightness() + delta);
}

static void on_tab(void)
{
    s_set_open = !s_set_open;
    s_set_tab = 0;            // PLAY is the entry tab
    s_set_row = -1;          // start in the tab header: RIGHT switches tab, DOWN enters rows
    s_set_edit = false;
    update_hint();
    nucleo_app_request_draw();
}

// Settings keys. NOTE: NK_LEFT and NK_BACK never arrive here — the framework routes them to the
// back handler (video_back). So tabs cycle with RIGHT and "back" is handled hierarchically.
static void settings_key(int key, char ch)
{
    (void)ch;
    if (s_set_edit) {
        if      (key == NK_RIGHT || key == NK_UP) slider_adjust(+5);
        else if (key == NK_DOWN)                  slider_adjust(-5);
        else if (key == NK_ENTER)                 s_set_edit = false;
        nucleo_app_request_draw(); return;
    }
    if (key == NK_RIGHT) {                                    // horizontal pager from anywhere
        s_set_tab = (s_set_tab + 1) % 4;
        if (s_set_row >= 0) s_set_row = 0;
        update_hint(); nucleo_app_request_draw(); return;
    }
    if (s_set_row == -1) { if (key == NK_DOWN) s_set_row = 0; nucleo_app_request_draw(); return; }

    int nrows = SET_ROWS[s_set_tab];
    if (key == NK_UP) {
        s_set_row = (s_set_row > 0) ? s_set_row - 1 : -1;     // row 0 -> back to the tab header
    } else if (key == NK_DOWN) {
        if (s_set_row < nrows - 1) s_set_row++;
    } else if (key == NK_ENTER) {
        if (s_set_tab == 0) {                                 // PLAY
            if      (s_set_row == 0) s_autoplay = !s_autoplay;
            else if (s_set_row == 1) s_repeat = (s_repeat + 1) % 3;
            else if (s_set_row == 2) s_resume_mode = (s_resume_mode + 1) % 3;
            save_settings();
        } else if (s_set_tab == 1) {                          // AUDIO
            if (s_set_row == 0) s_set_edit = true;            // volume -> adjust mode
            else if (s_set_row == 1) {
                static const int vols[] = { 0, 50, 75, 100 };
                int cur = 0; for (int i = 0; i < 4; i++) if (vols[i] == s_vol_default) { cur = i; break; }
                s_vol_default = vols[(cur + 1) % 4]; save_settings();
            }
        } else if (s_set_tab == 2) {                          // VIEW
            if (s_set_row == 0) s_set_edit = true;            // brightness -> adjust mode
            else if (s_set_row == 1) { s_sort = (s_sort + 1) % 2; save_settings(); resort(); }
            else if (s_set_row == 2) { s_time_rem = !s_time_rem; save_settings(); }
        } else {                                              // LIBRARY
            if      (s_set_row == 1) { remove(RESUME_DB); resume_refresh(); }
            else if (s_set_row == 2) { scan(); }
            else if (s_set_row == 3) { s_help_open = true; s_help_scroll = 0; }
        }
    }
    update_hint();
    nucleo_app_request_draw();
}

// Hierarchical Back/Left: the framework routes BOTH keys here and hands us the key code, so we can
// tell Left from Esc. Pop one level and return true to consume; return false only at the top so
// the app actually closes. In a slider, Left lowers the value (Right raises it).
static bool video_back(int key)
{
    if (!st) return false;
    if (s_help_open) {                                   // manual closes back to the LIBRARY tab
        (void)key; s_help_open = false; update_hint(); nucleo_app_request_draw(); return true;
    }
    if (s_set_open) {
        if (s_set_edit) {
            if (key == NK_LEFT) slider_adjust(-5);            // Left = value down
            else                s_set_edit = false;           // Esc = done adjusting
            nucleo_app_request_draw(); return true;
        }
        if (s_set_row >= 0) s_set_row = -1;                   // row -> tab header
        else               { s_set_open = false; update_hint(); }   // header -> close sheet
        nucleo_app_request_draw(); return true;
    }
    if (strcmp(s_path, "/") != 0) { go_up(); nucleo_app_request_draw(); return true; }
    return false;                                             // root -> let the framework close us
}

// ---- bilingual manual --------------------------------------------------------
// A line starting with '#' is a heading (accent, no marker); "" is a spacer.
static const char *const HELP_LINES[] = {
    "#RIPRODUCE (sul Cardputer)",
    "Solo file .nfv (v1/v2/v3).",
    "Dentro sono frame JPEG; il v3",
    "aggiorna solo le parti che",
    "cambiano + audio incluso.",
    "#ALTRI FORMATI?",
    "mp4, mkv, avi, mov, mjpeg e",
    "qualsiasi altro NON si aprono",
    "qui: convertili in .nfv con lo",
    "strumento NucleoOS (NFV Studio,",
    "run_studio.ps1 sul PC).",
    "#COMPATIBILITA'",
    "I clip vecchi (v1/v2) si vedono",
    "come prima: il player riconosce",
    "la versione automaticamente.",
    "#AUDIO",
    "Mono 16 kHz: lo speaker non",
    "rende sopra ~6 kHz - leggero,",
    "stesso suono.",
    "",
    "#=== ENGLISH ===",
    "#PLAYS (on the Cardputer)",
    "Only .nfv files (v1/v2/v3).",
    "Inside are JPEG frames; v3",
    "stores only the parts that",
    "change + embedded audio.",
    "#OTHER FORMATS?",
    "mp4, mkv, avi, mov, mjpeg and",
    "any other do NOT open here:",
    "convert them to .nfv with the",
    "NucleoOS tool (NFV Studio,",
    "run_studio.ps1 on a PC).",
    "#COMPATIBILITY",
    "Old clips (v1/v2) play just like",
    "before: the player detects the",
    "version automatically.",
    "#AUDIO",
    "Mono 16 kHz: the speaker is deaf",
    "above ~6 kHz - lighter, same",
    "sound.",
};
static const int HELP_N = (int)(sizeof HELP_LINES / sizeof HELP_LINES[0]);

static void draw_help(int ch)
{
    d.fillRect(0, 0, 240, ch, BG);
    d.fillRect(0, 0, 240, 24, BG);
    d.setTextSize(2); d.setTextColor(FG, BG); d.setCursor(10, 4); d.print("Guida / Help");
    d.drawFastHLine(0, 23, 240, LINE);

    const int LH = 11, top = 28, bot = ch - 12;
    int vis = (bot - top) / LH;
    int maxs = HELP_N - vis; if (maxs < 0) maxs = 0;
    if (s_help_scroll > maxs) s_help_scroll = maxs;
    if (s_help_scroll < 0) s_help_scroll = 0;

    d.setClipRect(0, 24, 240, bot - 24);
    int y = top;
    for (int i = s_help_scroll; i < HELP_N && y < bot; i++, y += LH) {
        const char *s = HELP_LINES[i];
        d.setTextSize(1);
        if (s[0] == '#') { d.setTextColor(ACC, BG); d.setCursor(8, y); d.print(s + 1); }
        else             { d.setTextColor(s[0] ? FG : DIM, BG); d.setCursor(8, y); d.print(s); }
    }
    d.clearClipRect();

    // scrollbar
    if (maxs > 0) {
        int trh = bot - top, kh = trh * vis / HELP_N; if (kh < 8) kh = 8;
        int ky = top + (trh - kh) * s_help_scroll / maxs;
        d.fillRoundRect(236, top, 2, trh, 1, SURF);
        d.fillRoundRect(236, ky, 2, kh, 1, ACC);
    }
    d.setTextSize(1); d.setTextColor(DIM, BG); d.setCursor(8, ch - 10);
    d.print(maxs > 0 ? "UP/DN scroll   ESC back" : "ESC back");
}

static void draw(void)
{
    if (!st) return;
    int ch = nucleo_app_content_height();
    int top = nucleo_app_content_top();
    if (s_help_open) { draw_help(ch); return; }
    if (s_set_open) { draw_settings(ch); return; }

    draw_header(top, s_n);
    int lt = top + HEAD_H, lh = ch - HEAD_H;

    if (s_n == 0) {
        d.fillRect(0, lt, 240, lh, BG);
        d.setTextSize(1); d.setTextColor(DIM, BG);
        if (strcmp(s_path, "/")) { d.setCursor(12, lt + 16); d.print("(empty folder)"); }
        else {
            d.setCursor(12, lt + 16); d.print("No .nfv clips in /data/Videos");
            d.setCursor(12, lt + 30); d.print("Convert on PC: tools/nfv/run_studio.ps1");
        }
        return;
    }
    draw_list(lt, lh);
}

static const char *row_label(int i, void *) { return st->e[i].name; }

// Auto-advance target after `idx`, honouring Repeat. -1 = stop (no next, or Repeat off at the end).
static int next_to_play(int idx)
{
    if (!st) return -1;
    if (s_repeat == 1) return idx;                                  // One: same clip
    for (int j = idx + 1; j < s_n; j++) if (!st->e[j].dir) return j;  // next clip in folder
    if (s_repeat == 2) for (int j = 0; j < s_n; j++) if (!st->e[j].dir) return j;  // All: wrap
    return -1;
}
// Manual skip: next / previous clip in the folder, always wrapping (idx itself if it's the only one).
static int next_clip(int idx)
{
    if (!st) return idx;
    for (int j = idx + 1; j < s_n; j++) if (!st->e[j].dir) return j;
    for (int j = 0; j < idx; j++)       if (!st->e[j].dir) return j;
    return idx;
}
static int prev_clip(int idx)
{
    if (!st) return idx;
    for (int j = idx - 1; j >= 0; j--)  if (!st->e[j].dir) return j;
    for (int j = s_n - 1; j > idx; j--) if (!st->e[j].dir) return j;
    return idx;
}

// Compose the absolute path, display title and resume key for an entry.
static void clip_paths(int idx, char *vpath, size_t vn, char *title, size_t tn, char *key, size_t kn)
{
    VEntry *r = &st->e[idx];
    snprintf(vpath, vn, "%s%s%s", VIDEO_DIR, s_path, r->name);
    snprintf(title, tn, "%s", r->name);
    char *td = strrchr(title, '.'); if (td) *td = 0;
    rel_key(key, kn, r->name);
}
static void clip_title(int idx, char *buf, size_t n)
{
    snprintf(buf, n, "%s", st->e[idx].name);
    char *td = strrchr(buf, '.'); if (td) *td = 0;
}

// Play `idx` (at start_ms), then loop on the result: auto-advance on natural end (Autoplay/Repeat),
// or honour the viewer's prev/next skip. ONE return_to_list() at the end -> no flash between clips.
static void play_from(int idx, int64_t start_ms)
{
    // The dedicated reclaim window (NX_NET_APP, ~70 KB; Wi-Fi STA stays up) is already held for the whole
    // app session by enter() — like the Music player. We do NOT toggle it per-play: the old per-play
    // enter/exit meant the FILE BROWSER ran at the ~5 KB idle heap, where scan()'s opendir/fopen OOM and
    // the list comes up EMPTY ("non vedo i file"). Holding it from enter() gives both browser and player
    // the headroom. (Do NOT add NX_WIFI: tearing the radio down+up is the ADV's fragile path — it left
    // audio dead + SD flaky for an extra ~30-50KB we don't need; an .nfv plays off SD, no network.)
    vtrace("play_enter", true);

    for (;;) {
        s_sel = idx;                                         // keep the list selection on the live clip
        char vpath[256], title[72], key[200], ntbuf[72];
        clip_paths(idx, vpath, sizeof vpath, title, sizeof title, key, sizeof key);
        const char *nt = NULL;                               // "Up next" preview for autoplay/repeat
        if (s_autoplay || s_repeat) { int an = next_to_play(idx); if (an >= 0 && an != idx) { clip_title(an, ntbuf, sizeof ntbuf); nt = ntbuf; } }

        int r = play_nfv(vpath, title, key, start_ms, nt);
        resume_refresh();
        start_ms = 0;                                        // any subsequent clip starts at the top

        if (r == PR_NEXT)      { idx = next_clip(idx); continue; }
        if (r == PR_PREV)      { idx = prev_clip(idx); continue; }
        if (r == PR_ENDED && (s_autoplay || s_repeat)) { int an = next_to_play(idx); if (an >= 0) { idx = an; continue; } }
        break;                                               // PR_STOP, or ended with nothing to advance to
    }
    vtrace("pf_before_rtl", false);
    return_to_list();         // re-acquire the 32 KB canvas; the reclaim window stays held until leave()
    vtrace("pf_done", false);
}

// Play a single .nfv chosen in Files ("open with") — it can live OUTSIDE VIDEO_DIR. The reclaim window
// is already held for the session by enter(); we just play the one clip, then return to the list.
static void play_external(const char *abs)
{
    const char *bn = strrchr(abs, '/'); bn = bn ? bn + 1 : abs;
    char title[72]; snprintf(title, sizeof title, "%s", bn);
    char *dt = strrchr(title, '.'); if (dt) *dt = 0;          // hide the .nfv extension in the bar
    play_nfv(abs, title, abs, 0, NULL);                       // reskey = abs path (unique resume key)
    resume_refresh();
    return_to_list();
}

static void on_key(int key, char ch)
{
    if (!st) return;
    if (s_help_open) {                                   // manual is a scrollable modal
        if      (key == NK_DOWN) s_help_scroll++;
        else if (key == NK_UP)   s_help_scroll--;
        nucleo_app_request_draw(); return;
    }
    if (s_set_open) { settings_key(key, ch); return; }

    if (app_ui_list_key(key, ch, &s_sel, s_n, row_label, nullptr)) {   // up/down + type-to-jump
        // handled
    }
    else if (key == NK_DEL) { go_up(); }
    else if (key == NK_ENTER && s_sel < s_n) {
        VEntry *r = &st->e[s_sel];
        if (r->dir) descend(r->name);
        else {
            int64_t start_ms = 0;
            if (r->resume > 0) {                            // seen before -> Ask / Auto / Off
                if (s_resume_mode == 0) {
                    char vpath[256], title[72], key2[200];
                    clip_paths(s_sel, vpath, sizeof vpath, title, sizeof title, key2, sizeof key2);
                    int64_t rr = resume_prompt(vpath, title, r->resume);
                    if (rr < 0) { d.fillScreen(BG); nucleo_app_request_draw(); return; }
                    start_ms = rr;
                } else if (s_resume_mode == 1) start_ms = (int64_t)r->resume * 1000;
            }
            play_from(s_sel, start_ms);
            return;                                          // play_from already requested a redraw
        }
    }
    else return;
    nucleo_app_request_draw();
}

// No continuous animation in the browser (the list redraws only on keypress/state change), so the
// tick is a no-op — the most flicker-proof posture on this PSRAM-less, no-double-buffer panel.
static void tick(void) {}

static void update_hint(void)
{
    int hs = s_set_open ? (s_set_edit ? 2 : 1) : 0;
    if (hs == s_hint_last) return;
    s_hint_last = hs;
    switch (hs) {
        case 1:  nucleo_app_set_hint(TR("DX scheda \xb7 SU/GIU riga \xb7 ESC chiudi", "RIGHT tab \xb7 UP/DN row \xb7 ESC close")); break;
        case 2:  nucleo_app_set_hint(TR("SX/DX regola \xb7 INVIO fatto", "L/R adjust \xb7 ENTER done")); break;
        default: nucleo_app_set_hint(TR("invio riproduci/apri \xb7 canc su \xb7 TAB menu", "enter play/open \xb7 del up \xb7 TAB menu"));
    }
}

static void enter(void)
{
    // RAM is handled DECLARATIVELY by exclusive_flags (NX_NET_APP | NX_SOLO) — the framework applies it
    // BEFORE this on_enter runs, so scan()'s opendir/fopen already have headroom. NX_SOLO reboots the
    // whole app into a FRESH, UNFRAGMENTED heap (services never started), which is what finally lets the
    // Helix MP3 decoder (~20 KB) + the JPEG frame buffer (~9 KB) + the file list coexist without OOM —
    // the inline ~70 KB reclaim alone left the decoder short and playback was SILENT.
    if (!st) st = (VState *)calloc(1, sizeof(VState));       // ~5 KB, only while the app is open
    if (!s_resume_scratch)                                   // ~4 KB resume scratch, only while open; before scan()
        s_resume_scratch = (resume_ent *)calloc(RESUME_MAX, sizeof *s_resume_scratch);
    load_settings();
    nucleo_audio_set_mute(false);                            // never inherit a mute from a previous session
    nucleo_app_set_tab_handler(on_tab);
    nucleo_app_set_back_handler(video_back);                 // hierarchical ESC/Left
    s_set_open = false; s_set_edit = false; s_set_tab = 0; s_set_row = -1;
    strcpy(s_path, "/");
    scan();
    s_hint_last = -1; update_hint();
    const char *of = nucleo_app_take_open_file();
    if (of && of[0]) play_external(of);                       // opened from Files -> play that clip, then the list
}
// Return the RAM to ANIMA and clear mute so it never leaks to Music/Radio after we close.
static void leave(void)
{
    if (nucleo_exclusive_active()) nucleo_exclusive_exit();   // safety net: never leave services suspended
    nucleo_audio_set_mute(false);
    if (st) { free(st); st = nullptr; }
    if (s_resume_scratch) { free(s_resume_scratch); s_resume_scratch = nullptr; }
    s_n = 0;
}

extern "C" void nucleo_register_video(void)
{
    static const nucleo_app_def_t app = {
        "video", "Video", "Media",
        ".nfv player — prev/next, mute, autoplay, resume. TAB=settings.",
        'V', 0x4D9C, enter, on_key, tick, draw, leave,
        NX_NET_APP | NX_SOLO   // SOLO: reboot into a fresh, unfragmented heap so MP3 decoder + JPEG buffer
                               // + file list coexist (inline reclaim left the decoder short -> silent video).
                               // NX_NET_APP bits are no-ops in Solo (services down at boot) but kept defensively
                               // for the non-Solo fallback path. Esc reboots back to the full OS.
    };
    nucleo_app_register(&app);
}
