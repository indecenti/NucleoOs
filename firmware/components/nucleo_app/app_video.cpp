// Video app — a native on-device player for NucleoOS .nfv v4 clips: whole-frame baseline JPEGs and IMA ADPCM
// sound interleaved in one file (tools/nfv/nfv4.py, or Video Studio in the browser). The device cannot decode
// H.264/H.265 (no PSRAM, no hardware codec), so clips are pre-converted. Older .nfv (v1-v3: frames + a sibling
// .mp3, or tile-delta) are no longer played — the list shows them as such and tools/nfv/migrate.py carries a
// v1/v2 clip over without its source video.
//
// Browser: a folder-aware fisheye list aligned with the Music app — folders first (amber, with
// a recursive clip count), then .nfv files (blue, with duration and a green "continue watching"
// mark). Enter descends / plays, Del goes up. Every row self-clears its own background so the
// list is correct AND flicker-free both when the shared canvas is buffered and when a decoder
// holds the RAM and we draw DIRECT.
//
// Settings (TAB): a 4-tab sheet mirroring Music — PLAY / AUDIO / VIEW / LIBRARY — with a
// persistent tab bar, toggle pills, sliders and action rows. Adds Autoplay, Repeat (Off/One/All),
// Resume behaviour, start volume, brightness, sort order and "Continue Watching" management.
// Persisted to /system/config/video.json.
//
// Playback (play_nfv -> play_nfv4) is a blocking modal: it owns the screen + keyboard until the clip ends or
// the viewer backs out, and pets the task watchdog every frame. When a clip ends naturally and Autoplay/Repeat
// is on, the next clip in the folder starts automatically. See docs/media.md and ANTI-FLICKER.md.
#include "nucleo_app.h"
#include "app_ui.h"
#include "app_large.h"          // two screens: the clip list + card on the 320x240 panel
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
#include "nucleo_surface.h"     // two screens: the film plays on the external panel
#include "nucleo_extdisp.h"     // ...and the Cardputer becomes its remote
#include "nucleo_touch.h"       // taps: seek / pause / jump; a vertical drag = volume
#include "nucleo_power.h"       // the perf lock: 240 MHz while a film plays
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


struct VEntry { char name[72]; bool dir, old; uint16_t count, vw, vh; uint32_t dur; uint32_t resume; };   // old = pre-v4 clip; vw x vh = picture

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
// The scratch exists only around a read/rewrite of the DB (~4 KB): while a film plays it is the frame queue's.
static bool resume_scratch_take(void)
{
    if (!s_resume_scratch) s_resume_scratch = (resume_ent *)calloc(RESUME_MAX, sizeof *s_resume_scratch);
    return s_resume_scratch != nullptr;
}
static void resume_scratch_drop(void) { free(s_resume_scratch); s_resume_scratch = nullptr; }

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
    if (!resume_scratch_take()) return;
    resume_ent *e = s_resume_scratch;
    int n = resume_read_all(e, RESUME_MAX);   // heap scratch, not 4 KB of stack
    for (int i = 0; i < n; i++)                              // drop any existing entry for this clip
        if (!strcmp(e[i].name, key)) { e[i] = e[n - 1]; n--; break; }
    bool finished = total_sec && sec + 5 >= total_sec;
    if (sec >= 5 && !finished) {                            // worth remembering
        if (n >= RESUME_MAX) { for (int i = 1; i < n; i++) e[i - 1] = e[i]; n--; }  // evict oldest
        snprintf(e[n].name, sizeof e[n].name, "%s", key); e[n].sec = sec; n++;
    }
    FILE *f = fopen(RESUME_DB, "w");
    if (f) {
        for (int i = 0; i < n; i++) fprintf(f, "%u|%s\n", (unsigned)e[i].sec, e[i].name);
        fclose(f);
    }
    resume_scratch_drop();
}

// Build a clip's resume key = path under /data/Videos (folder prefix + name; bare name at root).
static void rel_key(char *buf, size_t n, const char *name)
{
    const char *p = s_path; if (*p == '/') p++;             // drop leading '/'
    snprintf(buf, n, "%s%s", p, name);
}

// Peek a clip's duration (seconds) from its v4 header; *old (optional) = an .nfv of an older version;
// *vw x *vh (optional) = the visible picture size.
static uint32_t nfv_duration_path(const char *vpath, bool *old = nullptr, uint16_t *vw = nullptr, uint16_t *vh = nullptr)
{
    if (old) *old = false;
    FILE *f = fopen(vpath, "rb"); if (!f) return 0;
    uint8_t hb[64]; size_t got = fread(hb, 1, sizeof hb, f); fclose(f);
    if (got < 32 || memcmp(hb, "NFV1", 4)) return 0;
    auto le32 = [&](int o) { return (uint32_t)(hb[o] | (hb[o+1] << 8) | (hb[o+2] << 16) | ((uint32_t)hb[o+3] << 24)); };
    if (hb[4] != 4) { if (old) *old = true; return 0; }       // v1-v3: no longer played
    if (vw) *vw = (uint16_t)(le32(40) ? le32(40) : (hb[6] | (hb[7] << 8)));
    if (vh) *vh = (uint16_t)(le32(44) ? le32(44) : (hb[8] | (hb[9] << 8)));
    return le32(20) / 1000;                                   // v4: duration_ms at offset 20
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
    if (!st || !resume_scratch_take()) return 0;
    resume_ent *e = s_resume_scratch; int n = resume_read_all(e, RESUME_MAX);   // heap scratch, not 4 KB of stack
    for (int i = 0; i < s_n; i++) {
        st->e[i].resume = 0;
        if (st->e[i].dir) continue;
        char key[200]; rel_key(key, sizeof key, st->e[i].name);
        for (int j = 0; j < n; j++)
            if (!strcmp(e[j].name, key)) { st->e[i].resume = e[j].sec; break; }
    }
    resume_scratch_drop();
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
            r->dir = isdir; r->old = false; r->count = 0; r->vw = r->vh = 0; r->dur = 0; r->resume = 0;
            s_n++;
        }
        closedir(dir);                                      // close BEFORE recursing
    }
    for (int i = 0; i < s_n; i++) {                         // pass 2: folder counts + clip durations
        VEntry *r = &st->e[i];
        char c[320]; snprintf(c, sizeof c, "%s%s", base, r->name);
        if (r->dir) r->count = (uint16_t)count_clips(c, 0);
        else        r->dur = nfv_duration_path(c, &r->old, &r->vw, &r->vh);
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
    d.setTextSize(2); d.setTextColor(ACC, CO_BG); d.setCursor(10, 6); d.print(TR("Comandi", "Controls"));
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
    bar(34, TR("- =   Luce", "- =   Light"), bri, CO_WARM);
    if (muted) { d.setTextSize(1); d.setTextColor(MUTERED, CO_BG); d.setCursor(10, 48); d.print(TR("; .   Volume   (MUTO: m)", "; .   Volume   (MUTED: m)")); }
    else       bar(48, "; .   Volume", vol, GRN);

    static const char *const rk[6]    = { ", /", "[ ]", "INVIO", "0-9", "m   t", "TAB" };
    static const char *const rk_en[6] = { ", /", "[ ]", "ENTER", "0-9", "m   t", "TAB" };
    static const char *const rd[6]    = { "indietro / avanti", "clip prec / succ", "play / pausa",
                                          "salta al 10..90%", "muto / tempo restante", "chiudi   (ESC esci)" };
    static const char *const rd_en[6] = { "back / forward", "prev / next clip", "play / pause",
                                          "jump to 10..90%", "mute / time left", "close   (ESC exit)" };
    int y = 66;
    for (int i = 0; i < 6; i++) {
        d.setTextSize(1); d.setTextColor(ACC, CO_BG); d.setCursor(10, y);  d.print(TR(rk[i], rk_en[i]));
        d.setTextColor(CO_TXT, CO_BG); d.setCursor(58, y); d.print(TR(rd[i], rd_en[i]));
        y += 11;
    }
}

// Netflix-style resume sheet. Returns the chosen start in ms, 0 to start over, or -1 if backed
// out. Blocking modal (owns the screen + keyboard) and pets the watchdog while it waits.
static int64_t resume_prompt(const char *vpath, const char *title, uint32_t resume_sec)
{
    uint32_t total = nfv_duration_path(vpath);
    char rs[8], ts[8]; fmt_mmss(rs, sizeof rs, resume_sec); fmt_mmss(ts, sizeof ts, total);
    // Sized to whichever screen `d` is: the Cardputer, or the external panel when the browser lives there (then
    // the two choices are big touch buttons too: 'r' = resume, 's' = start over).
    const int W = d.width(), H = d.height();
    const bool big = W >= 320;
    const char *opt[2] = { TR("Riprendi", "Resume"), TR("Da capo", "Start over") };

    int sel = 0, last = -1;                                   // 0 = Resume, 1 = Start over
    for (;;) {
        esp_task_wdt_reset();
        if (sel != last) {
            last = sel;
            d.fillScreen(BG);
            const int m = big ? 20 : 12, cw = W - 2 * m;
            int y = big ? 18 : 8;
            d.setTextDatum(top_left);
            d.setFont(big ? (const lgfx::IFont *)&fonts::FreeSansBold9pt7b : (const lgfx::IFont *)&fonts::Font0); d.setTextColor(ACC, BG);
            d.drawString(TR("Riprendere la visione?", "Resume playback?"), m, y);
            y += big ? 28 : 14;
            d.setFont(big ? (const lgfx::IFont *)&fonts::FreeSans9pt7b : (const lgfx::IFont *)&fonts::Font2); d.setTextColor(FG, BG);
            y += (big ? 22 : 16) * lg::wrap(title, m, y, cw, big ? 22 : 16, 2) + 4;
            const int fw = total ? (int)((int64_t)cw * resume_sec / total) : 0;   // how far in they were
            d.fillRoundRect(m, y, cw, big ? 6 : 4, 2, CO_TRK);
            if (fw > 0) d.fillRoundRect(m, y, fw, big ? 6 : 4, 2, GRN);
            y += big ? 12 : 8;
            char line[48]; snprintf(line, sizeof line, TR("Interrotto a %s di %s", "Stopped at %s of %s"), rs, ts);
            d.setFont(big ? (const lgfx::IFont *)&fonts::Font2 : (const lgfx::IFont *)&fonts::Font0); d.setTextColor(CO_MUT, BG); d.drawString(line, m, y);
            const int bh = big ? 52 : 22, by = big ? H - bh - 22 : 84, bw = (cw - (big ? 16 : 8)) / 2;
            if (big) nucleo_touch_clear();
            for (int b = 0; b < 2; b++) {
                const int bx = m + b * (bw + (big ? 16 : 8));
                const unsigned short fill = sel == b ? ACC : CO_CHIP;
                d.fillRoundRect(bx, by, bw, bh, big ? 10 : 6, fill);
                d.setFont(big ? (const lgfx::IFont *)&fonts::FreeSansBold9pt7b : (const lgfx::IFont *)&fonts::Font0); d.setTextDatum(middle_center);
                d.setTextColor(sel == b ? CO_INK : FG, fill); d.drawString(opt[b], bx + bw / 2, by + bh / 2);
                if (big) nucleo_touch_add(bx, by, bw, bh, NK_CHAR, b == 0 ? 'r' : 's');
            }
            if (big) nucleo_touch_commit();
            d.setTextDatum(top_left); d.setFont(&fonts::Font0); d.setTextColor(CO_MUT, BG);
            if (!big) d.drawString(TR("< > scegli   invio ok   esc indietro", "< > select   enter ok   esc back"), m, 112);
        }
        nucleo_key_t k = nucleo_kbd_read();
        if      (k.key == NK_LEFT || k.key == NK_RIGHT || k.key == NK_UP || k.key == NK_DOWN) sel ^= 1;
        else if (k.key == NK_ENTER) return sel == 0 ? (int64_t)resume_sec * 1000 : 0;
        else if (k.key == NK_CHAR && k.ch == 'r') return (int64_t)resume_sec * 1000;
        else if (k.key == NK_CHAR && k.ch == 's') return 0;
        else if (k.key == NK_BACK || k.key == NK_DEL || k.ch == '`') return -1;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
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

// ---- two screens --------------------------------------------------------------------------------------------
// With an external panel the film plays THERE, native (a 320x240 clip 1:1, a 240-wide one centred). A 320-wide
// clip on the Cardputer alone is scaled 3/4 (every fourth row and column dropped). The external panel's controller is a 30 px bar
// at its foot (in the letterbox band when there is one, else over the picture while it shows); the Cardputer
// becomes the remote: title, a big clock, the usual controller bar. Touch: the picture's left / centre /
// right third = -10 s / pause / +10 s (drawn on the still frame while paused), a vertical drag = volume, the bottom
// strip is the scrub track: a tap jumps to that point of the film.
struct VGeom { bool big, switched; float sc; int oy, oh; };
static const int VBAR = 30;                                  // the external panel's controller bar
static VGeom vgeom_begin(int vis_w, int vis_h)
{
    VGeom g = {};
    if (nucleo_surface_large_ok()) {
        if (!nucleo_surface_large()) g.switched = nucleo_surface_set_large(true);
        g.big = nucleo_surface_large();
    }
    const int pw = d.width(), ph = d.height();
    g.sc = (float)pw / (vis_w > 0 ? vis_w : 240);
    g.oh = (int)((vis_h > 0 ? vis_h : 135) * g.sc + 0.5f);
    g.oy = g.oh >= ph ? (ph - g.oh) / 2 : (g.big ? (ph - g.oh) / 2 : 0);   // compact keeps the picture on top
    return g;
}
static void vgeom_end(const VGeom &g)
{
    nucleo_app_set_gfx(nullptr);
    nucleo_touch_clear(); nucleo_touch_commit();
    if (g.switched) nucleo_surface_set_large(false);
}
static int s_vseek_w = 240;                                  // the scrub strip's width (the tap's x over it = the point)
static void vtouch(const VGeom &g)
{
    nucleo_touch_clear();
    const int pw = d.width(), ph = d.height();
    const int by = g.big ? ph - VBAR : BARY;                 // the bottom strip (the controller bar)
    const int py = g.oy > 0 ? g.oy : 0, pb = by;             // the picture above it
    nucleo_touch_add(0, py, pw / 3, pb - py, NK_LEFT, ',');
    nucleo_touch_add(pw / 3, py, pw - 2 * (pw / 3), pb - py, NK_ENTER, 0);
    nucleo_touch_add(pw - pw / 3, py, pw / 3, pb - py, NK_RIGHT, '/');
    nucleo_touch_add(0, by, pw, ph - by, NK_CHAR, NUCLEO_TOUCH_FIELD); s_vseek_w = pw;
    if (g.big && g.oy >= 16) nucleo_touch_add(0, 0, pw / 4, g.oy, NK_BACK, 0);   // the top band's left: stop
    nucleo_touch_add_scroll(0, py, pw, pb - py, 24, NK_DOWN, 0, NK_UP, 0);         // drag up = louder
    nucleo_touch_commit();
}
// The external panel's bar: title, time, progress. `full` repaints its background (else in place).
static void vbar_big(const char *title, uint32_t el, uint32_t tot, bool paused, bool full)
{
    const int W = d.width(), y = d.height() - VBAR;
    if (full) { d.fillRect(0, y, W, VBAR, CO_BG); d.drawFastHLine(0, y, W, CO_LINE); }
    d.setFont(&fonts::Font2); d.setTextSize(1);
    char tm[24], a[8], b[8]; fmt_mmss(a, sizeof a, el); fmt_mmss(b, sizeof b, tot);
    snprintf(tm, sizeof tm, "%s%s / %s", paused ? "|| " : "", a, b);
    const int tw = (int)d.textWidth(tm);
    char t[64]; snprintf(t, sizeof t, "%s", title ? title : "");
    while (t[0] && (int)d.textWidth(t) > W - tw - 24) t[strlen(t) - 1] = 0;
    d.setTextColor(CO_TXT, CO_BG); d.setCursor(8, y + 3); d.print(t);
    const int te = 8 + (int)d.textWidth(t);
    d.fillRect(te, y + 3, W - tw - 8 - te, 16, CO_BG);
    d.setTextColor(paused ? ACC : CO_MUT, CO_BG); d.setCursor(W - 8 - tw, y + 3); d.print(tm);
    const int px = 8, pw = W - 16, fill = tot ? (int)((int64_t)pw * el / tot) : 0;
    d.fillRect(px, y + 22, fill, 4, ACC);
    d.fillRect(px + fill, y + 22, pw - fill, 4, CO_CHIP);
    d.setFont(&fonts::Font0);
}
// The Cardputer as the remote while the film is on the external panel.
static void vremote(const char *title, const char *next_title, uint32_t el, uint32_t tot, bool paused, bool muted,
                    int vol, int bri, bool full, bool seeking, int sdir, int sstep, bool near_end)
{
    LovyanGFX *ig = (LovyanGFX *)nucleo_extdisp_internal_gfx();
    if (!ig) return;
    nucleo_app_set_gfx(ig);
    d.startWrite();
    if (full) {
        d.fillRect(0, 0, 240, BARY, CO_BG);
        d.setFont(&fonts::Font2); d.setTextColor(CO_MUT, CO_BG);
        char t[64]; snprintf(t, sizeof t, "%s", title ? title : "");
        while (t[0] && (int)d.textWidth(t) > 224) t[strlen(t) - 1] = 0;
        d.setCursor(8, 4); d.print(t);
    }
    char a[8], b[12]; fmt_mmss(a, sizeof a, s_time_rem && tot > el ? tot - el : el);
    d.setFont(&fonts::Font7); d.setTextColor(paused ? CO_MUT : CO_TXT, CO_BG);
    d.setCursor(8, 26); d.print(a);
    d.setFont(&fonts::Font2); d.setTextColor(CO_MUT, CO_BG);
    snprintf(b, sizeof b, "/ %s", ""); fmt_mmss(b + 2, sizeof b - 2, tot);
    d.setCursor(240 - 8 - (int)d.textWidth(b), 58); d.print(b);
    d.setFont(&fonts::Font0);
    draw_controller(title, next_title, el, tot, paused, muted, vol, bri, full, seeking, sdir, sstep, near_end);
    d.endWrite();
    nucleo_app_set_gfx(nullptr);
}

#define V4_RDBUF_MIN 4096            // smallest stdio buffer the reader falls back to

// ===== NFV v4 (interleaved MJPEG + IMA ADPCM) — the player ====================================================
// Format and rationale: tools/nfv/nfv4.py. In short: ONE file, ONE reader (this loop, front to back, nothing
// else on the card — no MP3 task, so no file-system / SPI-bus deadlock), whole-frame baseline JPEGs decoded by
// esp_new_jpeg (SIMD on the S3) in 16-row strips that go straight to the panel, and IMA ADPCM audio written to
// the I2S. The I2S queue is the clock: a frame that would be late is dropped (every frame stands alone), the
// sound never stops.
//
// Audio pump. The driver cannot say how much room its DMA queue has, and a blocking write would hold the reader
// back, so the queue is modelled: queued = samples written - samples played (wall clock x rate). A block is
// written only when it fits (it never blocks), the queue is topped up between the strips of every frame and
// between the chunks the reader takes, and an underrun re-anchors the model. The I2S queue is opened long
// (nucleo_audio_pcm_open_ex: ~200 ms instead of the driver's 90 ms), so a slow card read or a heavy frame no
// longer runs it dry.
//
// Read-ahead. The file carries each second's sound ~0.25 s before its pictures, and a reader that holds ONE
// frame stops there: while that frame waits for its turn, no more sound comes in, and a slow read right then is
// an audible gap. So the player keeps a queue of compressed frames (as many as the heap affords, up to V4_VQ)
// and a long ADPCM queue: the sound then runs up to ~0.5 s ahead of the picture.
//
// RAM while playing: the decoder (~10 KB), one 16-row strip (W x 16 x 2 = 10 KB at 320), V4_VQ compressed
// frames at most (the header's max_jpeg each, <= 24 KB by the encoder's cap; fewer when the heap is short), up
// to V4_AQ_MAX ADPCM blocks (6 KB) + one PCM block, the stdio buffer, the I2S DMA. Separate blocks, each well
// under the ~32 KB contiguous a Solo boot leaves.
#include "esp_jpeg_dec.h"

static const int8_t  IMA_IDX[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
static const int16_t IMA_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767 };
// One mono IMA ADPCM block (WAV layout: i16 predictor, u8 step index, u8 0, then nibbles low first) -> PCM.
static int ima_block(const uint8_t *b, int len, int16_t *out)
{
    if (len < 4) return 0;
    int pred = (int16_t)(b[0] | (b[1] << 8)), idx = b[2] > 88 ? 88 : b[2], n = 0;
    out[n++] = (int16_t)pred;
    for (int i = 4; i < len; i++) {
        for (int s = 0; s < 2; s++) {
            const int nib = s ? (b[i] >> 4) : (b[i] & 15), step = IMA_STEP[idx];
            int diff = step >> 3;
            if (nib & 1) diff += step >> 2;
            if (nib & 2) diff += step >> 1;
            if (nib & 4) diff += step;
            pred += (nib & 8) ? -diff : diff;
            if (pred > 32767) pred = 32767;
            if (pred < -32768) pred = -32768;
            idx += IMA_IDX[nib & 7];
            if (idx < 0) idx = 0;
            if (idx > 88) idx = 88;
            out[n++] = (int16_t)pred;
        }
    }
    return n;
}


#define V4_AQ_MAX 24                 // ADPCM blocks held ahead of the I2S queue (~0.75 s at 256 B / 16 kHz)
#define V4_QCAP   (6 * 240)          // the driver's default I2S queue, in samples, when the long one is refused
#define V4_QWANT  3200               // the I2S queue asked for (200 ms at 16 kHz)
#define V4_VQ     5                  // compressed frames read ahead, at most
#define V4_RESERVE (22 * 1024)       // heap left for the I2S DMA, stdio and the panel after the frame queue
static int play_nfv4(const char *vpath, const char *title, const char *reskey, int64_t start_ms,
                     const char *next_title)
{
    FILE *f = fopen(vpath, "rb");
    if (!f) return PR_STOP;
    // One SD read per 16 KB (about two frames): fewer, longer transfers on the bus the card shares with the
    // external panel. The media-center boot (no radio) leaves the RAM for it; 4 KB if it does not.
    size_t rdsz = 32768;                                 // ~3 frames per card read (157 KB free in the Solo boot)
    char *rdbuf = (char *)malloc(rdsz);
    if (!rdbuf) { rdsz = 16384; rdbuf = (char *)malloc(rdsz); }
    if (!rdbuf) { rdsz = V4_RDBUF_MIN; rdbuf = (char *)malloc(rdsz); }
    if (rdbuf) setvbuf(f, rdbuf, _IOFBF, rdsz);
    uint8_t hb[64];
    if (fread(hb, 1, 64, f) != 64 || memcmp(hb, "NFV1", 4) || hb[4] != 4) { fclose(f); free(rdbuf); return PR_STOP; }
    auto le16 = [&](int o) { return (uint32_t)(hb[o] | (hb[o + 1] << 8)); };
    auto le32 = [&](int o) { return (uint32_t)(hb[o] | (hb[o + 1] << 8) | (hb[o + 2] << 16) | ((uint32_t)hb[o + 3] << 24)); };
    const int W = (int)le16(6), Hc = (int)le16(8), fps = le16(10) ? (int)le16(10) : 15, rate = (int)le16(12);
    const bool has_audio = (hb[5] & 1) && hb[14] == 1 && rate > 0;
    const bool has_mask = (hb[5] & 2) != 0;              // video chunks say which 16-row strips changed
    const uint32_t nfr = le32(16), dur_ms = le32(20), max_jpeg = le32(24);
    const int ablock = (int)le16(28), aspb = (int)le16(30);
    const uint32_t index_off = le32(32), index_count = le32(36);
    const int vis_w = le32(40) ? (int)le32(40) : W, vis_h = le32(44) ? (int)le32(44) : Hc;
    if (W <= 0 || W > 320 || (W & 15) || Hc <= 0 || Hc > 320 || (Hc & 15) || !max_jpeg || max_jpeg > 48000 ||
        (has_audio && (ablock < 8 || ablock > 2048 || aspb < 1 || aspb > 4096))) {
        fclose(f); free(rdbuf); return PR_STOP;
    }

    nucleo_app_release_buffers();
    uint16_t *strip = (uint16_t *)jpeg_calloc_align((size_t)W * 16 * 2, 16);
    int aq_cap = V4_AQ_MAX;                               // the long ADPCM queue, halved until it fits
    uint8_t *aq = nullptr;
    if (has_audio) while (aq_cap >= 8 && !(aq = (uint8_t *)malloc((size_t)aq_cap * ablock))) aq_cap /= 2;
    int16_t  *pcm   = has_audio ? (int16_t *)malloc((size_t)aspb * 2) : nullptr;
    jpeg_dec_handle_t dec = nullptr;
    jpeg_dec_config_t dcfg = DEFAULT_JPEG_DEC_CONFIG();
    dcfg.output_type = JPEG_PIXEL_FORMAT_RGB565_BE;       // the panel's own byte order: a raw, unconverted push
    dcfg.block_enable = true;                             // 16 rows at a time (4:2:0 MCU), never a whole frame
    const bool dec_ok = jpeg_dec_open(&dcfg, &dec) == JPEG_ERR_OK;
    // The frame queue takes what is left: one slot always, more while the heap keeps V4_RESERVE free.
    uint8_t *vq[V4_VQ] = {}; int vq_cap = 0;
    while (vq_cap < V4_VQ) {
        if (vq_cap && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < max_jpeg + V4_RESERVE) break;
        if (!(vq[vq_cap] = (uint8_t *)malloc(max_jpeg))) break;
        vq_cap++;
    }
    uint8_t *jin = vq[0];                                 // the frame being decoded (the queue's head)
    const bool ok_mem = vq_cap > 0 && strip && (!has_audio || (aq && pcm)) && dec_ok;
    {
        char t[128];
        snprintf(t, sizeof t, "nfv4_open %dx%d fps=%d audio=%d/%d max_jpeg=%u ok=%d frames=%d ablocks=%d", W, Hc, fps,
                 (int)has_audio, rate, (unsigned)max_jpeg, (int)ok_mem, vq_cap, has_audio ? aq_cap : 0);
        vtrace(t, false);
    }
    auto cleanup = [&]() {
        if (dec) jpeg_dec_close(dec);
        for (int i = 0; i < vq_cap; i++) free(vq[i]);
        if (strip) jpeg_free_align(strip);
        free(aq); free(pcm);
        fclose(f); free(rdbuf);
    };
    if (!ok_mem) {
        d.fillScreen(0); d.setTextColor(CO_TXT, 0); d.setTextSize(1);
        d.setCursor(20, 60); d.print("Not enough memory to play");
        vTaskDelay(pdMS_TO_TICKS(1500));
        cleanup();
        return PR_STOP;
    }

    nucleo_power_perf_begin();                            // full clock while a film plays (DFS idles at 80 MHz)
    const VGeom G = vgeom_begin(vis_w, vis_h);
    const int pw = d.width(), ph = d.height();
    // Placement: native when the panel is wide enough (centred), else 3/4 (a 320 clip on the Cardputer: every
    // fourth row and column dropped, in place, inside the strip).
    const bool down = W > pw;
    const int OW = down ? W * 3 / 4 : W, OH = down ? Hc * 3 / 4 : Hc;
    const int ox = (pw - OW) / 2, oy = (ph - OH) / 2;
    d.setSwapBytes(false);                               // the strips are already in panel order
    d.fillScreen(0);
    vtouch(G);
    const bool audio_ok = has_audio && nucleo_audio_pcm_open_ex(rate, 1, V4_QWANT) == ESP_OK;
    const int qcap = audio_ok && nucleo_audio_pcm_queue_frames() > 0 ? nucleo_audio_pcm_queue_frames() : V4_QCAP;
    {
        char t[96];
        snprintf(t, sizeof t, "nfv4_geom panel=%dx%d big=%d down=%d at=%d,%d audio_ok=%d i2s=%d", pw, ph, (int)G.big,
                 (int)down, ox, oy, (int)audio_ok, qcap);
        vtrace(t, false);
    }
    if (audio_ok && s_vol_default > 0) nucleo_audio_set_volume(s_vol_default);
    if (audio_ok) nucleo_audio_fade_in(250);             // no click on the first sample

    // ---- clocks ---------------------------------------------------------------------------------------------
    int64_t a_t0 = 0;                    // when the audio anchor's first sample started playing (0 = not yet)
    uint64_t a_written = 0;              // samples written since the anchor
    int64_t a_base_ms = 0;               // clip time of the anchor
    int64_t v_t0 = esp_timer_get_time(); // wall-clock anchor when there is no audio (yet)
    int64_t v_base_ms = 0;
    uint32_t underruns = 0;
    auto pos_ms = [&]() -> int64_t {
        const int64_t now = esp_timer_get_time();
        if (audio_ok && a_t0) {
            int64_t played = (now - a_t0) * rate / 1000000;
            if (played > (int64_t)a_written) played = (int64_t)a_written;
            return a_base_ms + played * 1000 / rate;
        }
        return v_base_ms + (now - v_t0) / 1000;
    };
    // ---- audio queue ----------------------------------------------------------------------------------------
    int aq_len[V4_AQ_MAX] = {}; uint32_t aq_stamp[V4_AQ_MAX] = {}; int aq_head = 0, aq_n = 0;
    uint32_t a_skip_until = 0;           // after a seek: samples before this are dropped
    auto pump_audio = [&]() {
        if (!audio_ok) { aq_n = 0; return; }
        while (aq_n > 0) {
            const int64_t now = esp_timer_get_time();
            if (a_t0) {
                int64_t queued = (int64_t)a_written - (now - a_t0) * rate / 1000000;
                if (queued < 0) {                        // ran dry: re-anchor (the gap was silence)
                    a_base_ms += (int64_t)a_written * 1000 / rate;
                    a_t0 = now; a_written = 0; underruns++; queued = 0;
                }
                if (queued + aspb > qcap) break;         // no room: writing now would block
            }
            const int slot = aq_head;
            int n = ima_block(aq + (size_t)slot * ablock, aq_len[slot], pcm);
            int drop = 0;
            if (a_skip_until > aq_stamp[slot]) drop = (int)(a_skip_until - aq_stamp[slot]);
            if (drop > n) drop = n;
            if (n - drop > 0) {
                if (!a_t0) { a_t0 = now; a_written = 0; a_base_ms = (int64_t)(aq_stamp[slot] + drop) * 1000 / rate; }
                nucleo_audio_pcm_write(pcm + drop, (size_t)(n - drop) * 2);
                a_written += (uint64_t)(n - drop);
            }
            a_skip_until = 0;
            aq_head = (aq_head + 1) % aq_cap; aq_n--;
        }
    };
    // ---- picture ----------------------------------------------------------------------------------------------
    auto push_strip = [&](int y0, int rows) {
        if (!down) { d.pushImage(ox, oy + y0, W, rows, strip); return; }
        int outr = 0;                                    // 3/4 nearest, in place (destinations never pass sources)
        for (int r = 0; r < rows; r++) {
            if (((y0 + r) & 3) == 3) continue;
            const uint16_t *src = strip + (size_t)r * W;
            uint16_t *dst = strip + (size_t)outr * OW;
            for (int c = 0, o = 0; c < W; c++) if ((c & 3) != 3) dst[o++] = src[c];
            outr++;
        }
        d.pushImage(ox, oy + y0 * 3 / 4, OW, outr, strip);
    };
    uint32_t pf_dec = 0, pf_rd = 0, pf_shown = 0, pf_late = 0; int64_t pf_t0 = esp_timer_get_time();
    // Only the strips that changed go over the bus (the bus the card shares is the bottleneck: ~31 ms for a
    // whole 320x240 frame at 40 MHz). Frames dropped as late leave their changes in `pend_mask` for the next
    // one; `full` repaints everything (start, seek, the bar leaving the picture, the help sheet closing).
    uint16_t v_mask = 0xFFFF, pend_mask = 0;
    bool full = true;
    uint32_t pf_jdec = 0, pf_strips = 0;
    uint32_t pf_iter = 0, pf_slow = 0, pf_keys = 0; int32_t pf_lateby = 0; int64_t pf_prev = 0;   // loop health
    auto decode_frame = [&](uint32_t len) -> bool {
        jpeg_dec_io_t io = {};
        io.inbuf = jin; io.inbuf_len = (int)len;
        jpeg_dec_header_info_t info;
        if (jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK || info.width != W) return false;
        int cnt = 0, olen = 0;
        jpeg_dec_get_process_count(dec, &cnt);
        jpeg_dec_get_outbuf_len(dec, &olen);
        if (cnt <= 0 || olen > W * 16 * 2) return false;
        int y = 0;
        const uint16_t send = (full || !has_mask) ? 0xFFFF : (uint16_t)(v_mask | pend_mask);
        for (int k = 0; k < cnt; k++) {
            io.outbuf = (uint8_t *)strip;
            const int64_t j0 = esp_timer_get_time();
            if (jpeg_dec_process(dec, &io) != JPEG_ERR_OK) return false;
            pf_jdec += (uint32_t)(esp_timer_get_time() - j0);
            const int rows = io.out_size / (W * 2);
            if (k >= 16 || (send >> k) & 1) { push_strip(y, rows); pf_strips++; }
            y += rows;
            pump_audio();                                // keep the sound fed while the picture paints
        }
        return true;
    };

    // ---- the stream ---------------------------------------------------------------------------------------------
    long fpos = 64;                                      // file position, tracked (no ftell per chunk)
    bool eof = false;
    uint32_t v_skip_until = 0;
    uint32_t vq_len[V4_VQ] = {}, vq_stamp[V4_VQ] = {}; uint16_t vq_mask[V4_VQ] = {};
    int vq_head = 0, vq_n = 0;                           // frames read and waiting for their turn
    auto vq_pop = [&]() { vq_head = (vq_head + 1) % vq_cap; vq_n--; };
    int64_t cur_frame = -1;
    auto skip_bytes = [&](uint32_t n) -> bool {          // read through (a seek would drop the stdio buffer)
        uint8_t *sb = vq[(vq_head + vq_n) % vq_cap];      // a free slot (the reader runs only while one is free)
        while (n) { const uint32_t k = n > max_jpeg ? max_jpeg : n; if (fread(sb, 1, k, f) != k) return false; n -= k; }
        return true;
    };
    auto read_chunk = [&]() -> bool {                    // one chunk into the audio queue or the frame slot
        uint8_t ch[12];
        if ((uint32_t)fpos + 12 > index_off || fread(ch, 1, 12, f) != 12) { eof = true; return false; }
        const uint32_t len = ch[4] | (ch[5] << 8) | (ch[6] << 16) | ((uint32_t)ch[7] << 24);
        const uint32_t stamp = ch[8] | (ch[9] << 8) | (ch[10] << 16) | ((uint32_t)ch[11] << 24);
        fpos += 12 + (long)len;
        if (ch[0] == 2 && len <= (uint32_t)ablock && audio_ok) {
            if (a_skip_until && stamp + (uint32_t)aspb <= a_skip_until) return skip_bytes(len);
            const int slot = (aq_head + aq_n) % aq_cap;
            if (fread(aq + (size_t)slot * ablock, 1, len, f) != len) { eof = true; return false; }
            aq_len[slot] = (int)len; aq_stamp[slot] = stamp; aq_n++;
            return true;
        }
        if (ch[0] == 1 && len <= max_jpeg) {
            const uint16_t m = (uint16_t)(ch[2] | (ch[3] << 8));
            if (stamp < v_skip_until) { pend_mask |= m; return skip_bytes(len); }
            const int slot = (vq_head + vq_n) % vq_cap;
            if (fread(vq[slot], 1, len, f) != len) { eof = true; return false; }
            vq_len[slot] = len; vq_stamp[slot] = stamp; vq_mask[slot] = m; vq_n++;
            return true;
        }
        return skip_bytes(len);
    };
    auto seek_far = [&](long off) {                      // WDT-safe hops (FATFS walks the cluster chain)
        long pos = ftell(f);
        if (pos < 0 || off < pos) { fseek(f, 64, SEEK_SET); pos = 64; }
        const long STEP = 2L * 1024 * 1024;
        while (off - pos > STEP) { fseek(f, STEP, SEEK_CUR); pos += STEP; esp_task_wdt_reset(); }
        fseek(f, off - pos, SEEK_CUR);
    };
    auto seek_ms = [&](int64_t ms) {
        if (ms < 0) ms = 0;
        if (dur_ms && ms > (int64_t)dur_ms - 500) ms = (int64_t)dur_ms - 500;
        if (ms < 0) ms = 0;
        const uint32_t tf = (uint32_t)(ms * fps / 1000);
        long off = 64; uint32_t kf = 0;
        if (index_count) {                               // the last seek point at or before the target
            int L = 0, R = (int)index_count - 1, a = 0; uint8_t eb[8];
            while (L <= R) {
                const int mid = (L + R) / 2;
                esp_task_wdt_reset();
                fseek(f, (long)index_off + (long)mid * 8, SEEK_SET);
                if (fread(eb, 1, 8, f) != 8) break;
                const uint32_t fi = eb[0] | (eb[1] << 8) | (eb[2] << 16) | ((uint32_t)eb[3] << 24);
                if (fi <= tf) { a = mid; L = mid + 1; } else R = mid - 1;
            }
            fseek(f, (long)index_off + (long)a * 8, SEEK_SET);
            if (fread(eb, 1, 8, f) == 8) {
                kf  = eb[0] | (eb[1] << 8) | (eb[2] << 16) | ((uint32_t)eb[3] << 24);
                off = (long)(eb[4] | (eb[5] << 8) | (eb[6] << 16) | ((uint32_t)eb[7] << 24));
            }
        }
        (void)kf;
        seek_far(off); fpos = off;
        vq_n = 0; vq_head = 0; eof = false; aq_n = 0; aq_head = 0;
        v_skip_until = tf;
        a_skip_until = audio_ok ? (uint32_t)(ms * rate / 1000) : 0;
        a_t0 = 0; a_written = 0; a_base_ms = ms;
        v_t0 = esp_timer_get_time(); v_base_ms = ms;
        full = true; pend_mask = 0;                      // a new place in the film: paint it whole
    };
    if (start_ms > 0) seek_ms(start_ms);

    // ---- the loop (keys and chrome) ----------------------------------------------------------------
    const uint32_t tot_s = dur_ms / 1000;
    int     bri = nucleo_app_brightness();
    int64_t overlay_until = esp_timer_get_time() + 2500000;
    bool    help = false, paused = false, stop = false, ov_prev = false, ctrl_full = true, remote_full = true;
    int     result = PR_STOP;
    uint32_t ui_sig = 0xffffffff, remote_sig = 0xffffffff;
    int      seek_step_s = 10, seek_dir = 0;
    int64_t  last_seek_us = 0, badge_until = 0, paused_ms = 0;
    while (!stop) {
        esp_task_wdt_reset();
        const int64_t now = esp_timer_get_time();
        if (pf_prev && now - pf_prev > (int64_t)pf_slow * 1000) pf_slow = (uint32_t)((now - pf_prev) / 1000);
        pf_prev = now; pf_iter++;
        nucleo_key_t k = nucleo_kbd_read();
        if (k.key != NK_NONE) {
            pf_keys++;
            const int64_t p = paused ? paused_ms : pos_ms();
            if (k.key == NK_BACK || k.key == NK_DEL || k.ch == 'q' || k.ch == '`') { result = PR_STOP; stop = true; }
            else if (k.ch == ']') { result = PR_NEXT; stop = true; }
            else if (k.ch == '[') { result = PR_PREV; stop = true; }
            else if (nucleo_touch_is_field(k.key, k.ch)) {           // the scrub strip: jump to the tapped point
                int tx, ty;
                if (dur_ms && nucleo_touch_last(&tx, &ty)) { seek_ms((int64_t)dur_ms * tx / (s_vseek_w - 1)); if (paused) paused_ms = pos_ms(); }
                overlay_until = now + 3000000; ui_sig = 0xffffffff;
            }
            else if (k.key == NK_ENTER || k.ch == ' ' || k.ch == 'p') {
                paused = !paused;
                if (paused) {
                    paused_ms = p;
                    // A pause badge on the picture (the next frame paints over it when playback resumes). On the
                    // panel the still frame carries the transport instead, on the thirds that are its touch
                    // targets: -10 s, play, +10 s.
                    const int bx = (ox > 0 ? ox : 0) + 10, by = (oy > 0 ? oy : 0) + 10;
                    d.startWrite();
                    if (G.big) {
                        const int cy = ((oy > 0 ? oy : 0) + ph - VBAR) / 2;
                        for (int c = 0; c < 3; c++) {
                            const int cx = pw * (2 * c + 1) / 6;
                            d.fillCircle(cx, cy, c == 1 ? 30 : 22, c == 1 ? ACC : CO_BG); d.drawCircle(cx, cy, c == 1 ? 30 : 22, CO_LINE);
                            if (c == 1) d.fillTriangle(cx - 9, cy - 14, cx - 9, cy + 14, cx + 15, cy, CO_INK);
                            else for (int s = -6; s <= 6; s += 12) {                    // a double arrow: back / forward
                                const int ax = cx + s, dir = c ? 1 : -1;
                                d.fillTriangle(ax - 6 * dir, cy - 8, ax - 6 * dir, cy + 8, ax + 6 * dir, cy, CO_TXT);
                            }
                        }
                    } else {
                        d.fillRoundRect(bx, by, 34, 30, 8, CO_BG);
                        d.drawRoundRect(bx, by, 34, 30, 8, CO_LINE);
                        d.fillRect(bx + 11, by + 8, 4, 14, CO_TXT); d.fillRect(bx + 19, by + 8, 4, 14, CO_TXT);
                    }
                    d.endWrite();
                }
                else seek_ms(paused_ms);                 // resume exactly where it stopped (clocks re-anchored)
                overlay_until = now + 3000000; ui_sig = 0xffffffff;
            }
            else if (k.key == NK_TAB) { help = !help; if (!help) remote_full = ctrl_full = full = true; ui_sig = 0xffffffff; }
            else if (k.ch == 'm') { nucleo_audio_set_mute(!nucleo_audio_is_muted()); overlay_until = now + 3000000; ui_sig = 0xffffffff; }
            else if (k.ch == 't') { s_time_rem = !s_time_rem; overlay_until = now + 3000000; ui_sig = 0xffffffff; }
            else if (k.key == NK_LEFT || k.key == NK_RIGHT) {
                const int dir = (k.key == NK_RIGHT) ? 1 : -1;
                if (dir == seek_dir && now - last_seek_us < 1200000) seek_step_s = (seek_step_s < 30) ? 30 : 60;
                else seek_step_s = 10;
                seek_dir = dir; last_seek_us = now; badge_until = now + 900000;
                seek_ms(p + (int64_t)dir * seek_step_s * 1000);
                if (paused) paused_ms = pos_ms();
                overlay_until = now + 3000000; ui_sig = 0xffffffff;
            }
            else if (k.key == NK_UP)   { if (nucleo_audio_is_muted()) nucleo_audio_set_mute(false); nucleo_audio_set_volume(nucleo_audio_volume() + 10); overlay_until = now + 3000000; ui_sig = 0xffffffff; }
            else if (k.key == NK_DOWN) { if (nucleo_audio_is_muted()) nucleo_audio_set_mute(false); nucleo_audio_set_volume(nucleo_audio_volume() - 10); overlay_until = now + 3000000; ui_sig = 0xffffffff; }
            else if (k.ch == '=' || k.ch == '+') { nucleo_app_set_brightness(nucleo_app_brightness() + 10); bri = nucleo_app_brightness(); overlay_until = now + 3000000; ui_sig = 0xffffffff; }
            else if (k.ch == '-' || k.ch == '_') { nucleo_app_set_brightness(nucleo_app_brightness() - 10); bri = nucleo_app_brightness(); overlay_until = now + 3000000; ui_sig = 0xffffffff; }
        }
        if (stop) break;
        if (help) {
            if (ui_sig) {
                LovyanGFX *ig = G.big ? (LovyanGFX *)nucleo_extdisp_internal_gfx() : nullptr;
                if (ig) nucleo_app_set_gfx(ig);
                draw_video_help(bri, nucleo_audio_volume(), nucleo_audio_is_muted()); ui_sig = 0;
                nucleo_app_set_gfx(nullptr);
            }
            vTaskDelay(pdMS_TO_TICKS(16));
            continue;
        }

        const bool overlay_now = now < overlay_until;
        if (overlay_now != ov_prev) { if (overlay_now) ctrl_full = true; else full = true; ov_prev = overlay_now; ui_sig = 0xffffffff; }
        const bool bar_over = G.big && oy + OH > ph - VBAR;

        bool worked = false;
        if (!paused) {
            pump_audio();
            // Read ahead: fill the audio queue and the frame queue, a few chunks per pass with the sound topped
            // up after each, so a long refill never starves the I2S.
            int64_t r0 = esp_timer_get_time();
            for (int c = 0; c < 6 && !eof && vq_n < vq_cap && aq_n < aq_cap; c++) {
                const int had = vq_n;
                read_chunk();
                pump_audio();
                if (vq_n > had && vq_n > 1) break;           // one more frame banked per pass is enough
            }
            pf_rd += (uint32_t)(esp_timer_get_time() - r0);
            pump_audio();
            if (vq_n > 0) {
                const uint32_t v_stamp = vq_stamp[vq_head], v_len = vq_len[vq_head];
                v_mask = vq_mask[vq_head]; jin = vq[vq_head];
                const int64_t due = (int64_t)v_stamp * 1000 / fps, p = pos_ms();
                if (p > due + 1000 / fps) {                                                 // late: drop it
                    if ((int32_t)(p - due) > pf_lateby) pf_lateby = (int32_t)(p - due);
                    vq_pop(); pend_mask |= v_mask; pf_late++; worked = true;
                }
                else if (p + 3 >= due) {                                                   // its turn: paint it
                    const int64_t d0 = esp_timer_get_time();
                    d.startWrite();
                    if (G.big && overlay_now && bar_over) d.setClipRect(0, 0, pw, ph - VBAR);
                    else if (!G.big && overlay_now)      d.setClipRect(0, 0, pw, BARY);
                    else                                 d.setClipRect(0, 0, pw, ph);
                    if (!decode_frame(v_len)) {
                        static int s_bad = 0;
                        if (s_bad++ < 3) { char t[64]; snprintf(t, sizeof t, "nfv4_decode_fail frame=%u len=%u", (unsigned)v_stamp, (unsigned)v_len); vtrace(t, false); }
                    }
                    d.clearClipRect();
                    d.endWrite();
                    pf_dec += (uint32_t)(esp_timer_get_time() - d0);
                    cur_frame = v_stamp; vq_pop(); pf_shown++; worked = true;
                    full = false; pend_mask = 0;
                }
            } else if (eof && aq_n == 0) {
                if (!audio_ok || pos_ms() >= a_base_ms + (int64_t)a_written * 1000 / rate) { result = PR_ENDED; break; }
            }
        }

        // ---- chrome: the external bar / the Cardputer's controller, and the remote ----
        const uint32_t el = (uint32_t)((paused ? paused_ms : pos_ms()) / 1000);
        const bool seeking = now < badge_until;
        if (overlay_now) {
            const bool muted = nucleo_audio_is_muted();
            const bool near_end = tot_s && el + 10 >= tot_s && next_title && next_title[0];
            const uint32_t sig = (el & 0x1fff) | ((uint32_t)(nucleo_audio_volume() / 10) << 13) | ((uint32_t)(bri / 10) << 17)
                               | ((uint32_t)paused << 21) | ((uint32_t)seeking << 22) | ((uint32_t)muted << 23)
                               | ((uint32_t)s_time_rem << 24) | ((uint32_t)near_end << 25);
            if (sig != ui_sig) {
                ui_sig = sig;
                d.startWrite();
                if (G.big) vbar_big(title, el, tot_s, paused, ctrl_full);
                else draw_controller(title, next_title, el, tot_s, paused, muted, nucleo_audio_volume(), bri, ctrl_full,
                                     seeking, seek_dir, seek_step_s, near_end);
                d.endWrite();
                ctrl_full = false;
            }
        } else if (G.big && !bar_over && ui_sig != 0) {
            ui_sig = 0;
            d.fillRect(0, ph - VBAR, pw, VBAR, 0);
        }
        if (G.big) {
            const bool near_end = tot_s && el + 10 >= tot_s && next_title && next_title[0];
            const uint32_t rs = (el & 0x1fff) | ((uint32_t)(nucleo_audio_volume() / 10) << 13) | ((uint32_t)(bri / 10) << 17)
                              | ((uint32_t)paused << 21) | ((uint32_t)seeking << 22) | ((uint32_t)nucleo_audio_is_muted() << 23)
                              | ((uint32_t)s_time_rem << 24) | ((uint32_t)near_end << 25);
            if (rs != remote_sig) {
                remote_sig = rs;
                vremote(title, next_title, el, tot_s, paused, nucleo_audio_is_muted(), nucleo_audio_volume(), bri,
                        remote_full, seeking, seek_dir, seek_step_s, near_end);
                remote_full = false;
            }
        }
        if (now - pf_t0 >= 5000000) {
            char t[192];
            snprintf(t, sizeof t, "nfv4_perf shown=%u late=%u rd=%ums jpeg=%ums dec+push=%ums strips=%u under=%u pos=%u "
                     "iter=%u slow=%ums keys=%u lateby=%dms aq=%d",
                     (unsigned)pf_shown, (unsigned)pf_late, (unsigned)(pf_rd / 1000), (unsigned)(pf_jdec / 1000),
                     (unsigned)(pf_dec / 1000), (unsigned)pf_strips, (unsigned)underruns, (unsigned)el,
                     (unsigned)pf_iter, (unsigned)pf_slow, (unsigned)pf_keys, (int)pf_lateby, aq_n);
            vtrace(t, false);
            ESP_LOGW(V_TAG, "%s", t);                   // live on the serial console too
            pf_shown = pf_late = pf_rd = pf_dec = pf_jdec = pf_strips = 0; pf_t0 = now;
            pf_iter = pf_slow = pf_keys = 0; pf_lateby = 0;
        }
        if (!worked) vTaskDelay(1);                      // nothing due yet: give the core back
    }

    esp_task_wdt_reset();
    if (audio_ok) nucleo_audio_pcm_close();
    const uint32_t watched = (uint32_t)((paused ? paused_ms : pos_ms()) / 1000);
    (void)cur_frame; (void)nfr;
    cleanup();                                           // the frame queue's RAM back first: the save needs a little
    resume_save(reskey, result == PR_ENDED ? tot_s : watched, tot_s);
    vgeom_end(G);
    nucleo_power_perf_end();
    return result;
}

// Blocking modal playback of the clip at `vpath`, from `start_ms` (0 = top). `title` is shown in the bar;
// `reskey` is the resume DB key; `next_title` (or NULL) is shown as "Up next" near the end for autoplay.
// Returns a PR_* code: PR_ENDED (natural end -> the caller may auto-advance), PR_NEXT/PR_PREV (skip),
// PR_STOP (stopped, or the clip cannot play). Does NOT restore the list — the caller does, so a back-to-back
// auto-advance/skip never flashes the browser between clips. Only v4 plays; an older clip says how to convert it.
static int play_nfv(const char *vpath, const char *title, const char *reskey, int64_t start_ms,
                    const char *next_title)
{
    uint8_t hb[8] = {};
    FILE *f = fopen(vpath, "rb");
    if (!f) return PR_STOP;
    const size_t got = fread(hb, 1, sizeof hb, f);
    fclose(f);
    if (got == sizeof hb && !memcmp(hb, "NFV1", 4) && hb[4] == 4)
        return play_nfv4(vpath, title, reskey, start_ms, next_title);
    char ver[24];
    if (got == sizeof hb && !memcmp(hb, "NFV1", 4)) snprintf(ver, sizeof ver, ".nfv v%u", (unsigned)hb[4]);
    else snprintf(ver, sizeof ver, "?");
    const char *lines[5] = { TR("Formato vecchio", "Old format"), ver,
                             TR("non piu' riprodotto.", "no longer played."),
                             TR("Riconvertilo con Video Studio", "Convert it with Video Studio"),
                             TR("o tools/nfv/migrate.py", "or tools/nfv/migrate.py") };
    d.fillScreen(BG);
    for (int i = 0; i < 5; i++) {
        const bool big = i < 2;
        d.setTextSize(big ? 2 : 1); d.setTextColor(big ? ACC : FG, BG);
        d.setCursor((d.width() - (int)strlen(lines[i]) * (big ? 12 : 6)) / 2, big ? 14 + i * 22 : 66 + (i - 2) * 14);
        d.print(lines[i]);
    }
    for (const int64_t until = esp_timer_get_time() + 8000000; esp_timer_get_time() < until;) {   // any key, or 8 s
        const nucleo_key_t k = nucleo_kbd_read();
        if (k.key != NK_NONE || k.ch) break;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    return PR_STOP;
}

// Return to the list after the playback modal(s). play_nfv frees the shared UI canvas to hand the
// contiguous block to the MP3 decoder; the audio task frees its RAM on stop() asynchronously, so
// retry briefly — the canvas usually re-acquires within a frame or two, restoring the buffered
// (flicker-free) path immediately.
static void return_to_list(void)
{
    for (int i = 0; i < 8 && !nucleo_screen_acquire(); i++) { esp_task_wdt_reset(); vTaskDelay(pdMS_TO_TICKS(20)); }
    d.fillScreen(BG);
    // Playback drew DIRECT to the panel while the canvas was freed, so the run loop's per-band hash
    // cache still describes the pre-playback list. A plain request_draw would repaint the canvas but
    // the diff-blit would skip every band that matches that stale hash, leaving the black video frame
    // on the panel (only the row that changed — the new resume mark — would show). Force a full blit.
    nucleo_app_force_repaint();
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
    else if (r->old)    snprintf(mb, sizeof mb, "%s", TR("vecchio", "old"));   // pre-v4: opening it says how to convert
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

// Touch: the rows draw_list put on the panel (slot -> entry index).
static int s_vtap[12], s_vtap_n;
static void draw_list(int y0, int region_h)
{
    if (s_sel < s_scroll) s_scroll = s_sel;
    int scan_y = 0;
    for (int i = s_scroll; i <= s_sel; i++) scan_y += (i == s_sel) ? 28 : 20;
    while (scan_y > region_h && s_scroll < s_sel) { scan_y -= (s_scroll == s_sel) ? 28 : 20; s_scroll++; }

    int y = y0;
    s_vtap_n = 0;
    for (int i = s_scroll; i < s_n && y < y0 + region_h; i++) {
        int h = (i == s_sel) ? 28 : 20;
        if (y + h > y0 + region_h && i != s_scroll) break;
        draw_row(i, y, h, i == s_sel);
        if (s_vtap_n < 12) {                                  // touch: tap picks, tap again plays / opens
            nucleo_touch_add(0, y, 240, h, NK_CHAR, (char)(NUCLEO_TOUCH_ROW + s_vtap_n));
            s_vtap[s_vtap_n++] = i;
        }
        y += h;
    }
    nucleo_touch_add_scroll(0, y0, 240, region_h, 20, NK_UP, 0, NK_DOWN, 0);   // a drag walks the list
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
    "Solo file .nfv v4: immagine",
    "(JPEG) e audio in un file.",
    "Il film va anche sullo schermo",
    "esterno 320x240, se collegato.",
    "#ALTRI FORMATI?",
    "mp4, mkv, avi, mov, mjpeg e",
    "qualsiasi altro NON si aprono",
    "qui: convertili in .nfv con",
    "Video Studio (app web) o con",
    "tools/nfv/nfv4.py sul PC.",
    "#CLIP VECCHI (v1/v2/v3)",
    "Nella lista: 'vecchio'. Non si",
    "riproducono piu': riconvertili,",
    "o usa tools/nfv/migrate.py.",
    "#AUDIO",
    "Mono 16 kHz: lo speaker non",
    "rende sopra ~6 kHz - leggero,",
    "stesso suono.",
};
static const char *const HELP_LINES_EN[] = {
    "#PLAYS (on the Cardputer)",
    "Only .nfv v4 files: picture",
    "(JPEG) and sound in one file.",
    "The film also plays on the",
    "external 320x240 panel.",
    "#OTHER FORMATS?",
    "mp4, mkv, avi, mov, mjpeg and",
    "any other do NOT open here:",
    "convert them to .nfv with",
    "Video Studio (web app) or",
    "tools/nfv/nfv4.py on a PC.",
    "#OLD CLIPS (v1/v2/v3)",
    "Listed as 'old'. They no longer",
    "play: convert them again, or",
    "use tools/nfv/migrate.py.",
    "#AUDIO",
    "Mono 16 kHz: the speaker is deaf",
    "above ~6 kHz - lighter, same",
    "sound.",
};
static const int HELP_N_IT = (int)(sizeof HELP_LINES / sizeof HELP_LINES[0]);
static const int HELP_N_EN = (int)(sizeof HELP_LINES_EN / sizeof HELP_LINES_EN[0]);

static void draw_help(int ch)
{
    d.fillRect(0, 0, 240, ch, BG);
    d.fillRect(0, 0, 240, 24, BG);
    d.setTextSize(2); d.setTextColor(FG, BG); d.setCursor(10, 4); d.print(TR("Guida", "Help"));
    // The manual half for the active OS language (Italian only for "it", like TR()).
    const bool help_en = nucleo_tr("it", "en")[0] == 'e';
    const char *const *HL = help_en ? HELP_LINES_EN : HELP_LINES;
    const int HELP_N = help_en ? HELP_N_EN : HELP_N_IT;
    d.drawFastHLine(0, 23, 240, LINE);

    const int LH = 11, top = 28, bot = ch - 12;
    int vis = (bot - top) / LH;
    int maxs = HELP_N - vis; if (maxs < 0) maxs = 0;
    if (s_help_scroll > maxs) s_help_scroll = maxs;
    if (s_help_scroll < 0) s_help_scroll = 0;

    d.setClipRect(0, 24, 240, bot - 24);
    int y = top;
    for (int i = s_help_scroll; i < HELP_N && y < bot; i++, y += LH) {
        const char *s = HL[i];
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

// ===== two screens ==============================================================================================
// With the external panel the browser lives THERE, designed for 320x240 + touch: the clip list on the left (the
// full height), the picked clip's card on the right (length, picture, where you stopped). Touch is the content
// itself: tap a row to pick it, tap the picked row or the card to play/open, drag the list to scroll, the header's
// back chevron leaves a folder and its menu button opens the settings.
// The Cardputer becomes the companion: the picked title in large type, its details and the keys. Settings and
// the manual stay on the Cardputer (keyboard work) while the panel keeps the list.
static const int LG_LISTW = 198;

static void lg_title(int idx, char *buf, size_t n)            // display name: no .nfv, trailing '/' off folders
{
    snprintf(buf, n, "%s", st->e[idx].name);
    if (!st->e[idx].dir) { char *dt = strrchr(buf, '.'); if (dt) *dt = 0; }
}

// A series in one folder ("Wallace e Gromit - Una Fantastica Gita", "Wallace e Gromit - I Pantaloni...") reads as a
// column of identical starts once cut to the row. A clip whose "<series> - " start is shared with another clip of
// the folder shows only what follows it (the card and the Cardputer keep the full title). 0 = show it whole.
static int lg_series_len(int i)
{
    if (st->e[i].dir) return 0;
    const char *n = st->e[i].name, *sep = strstr(n, " - ");
    if (!sep || sep == n) return 0;
    const int k = (int)(sep - n) + 3;
    for (int j = 0; j < s_n; j++)
        if (j != i && !st->e[j].dir && !strncmp(st->e[j].name, n, k)) return k;
    return 0;
}

// One row of lg::list: icon, title, length / resume time. It also records which entry each touch slot is.
static void lg_row(int i, int x, int y, int w, int h, bool focus, unsigned short bg, void *)
{
    const int slot = i - s_scroll;
    if (slot >= 0 && slot < 12) { s_vtap[slot] = i; if (slot >= s_vtap_n) s_vtap_n = slot + 1; }
    const int strip = lg_series_len(i);
    const VEntry *r = &st->e[i];
    const bool is_resume = !r->dir && r->resume > 0;
    const int iy = y + (h - 10) / 2;
    if (r->dir) icon_folder(x + 7, iy, focus ? INK : AMB); else icon_film(x + 7, iy, focus ? INK : ACC);
    char mb[12] = "";
    if (r->dir)         snprintf(mb, sizeof mb, "%u", (unsigned)r->count);
    else if (is_resume) fmt_mmss(mb, sizeof mb, r->resume);
    else if (r->old)    snprintf(mb, sizeof mb, "%s", TR("vecchio", "old"));
    else if (r->dur)    fmt_mmss(mb, sizeof mb, r->dur);
    lg::font(0);
    const int mw = mb[0] ? d.textWidth(mb) + 12 : 0;
    if (is_resume) mark_play(x + w - 8 - mw - 4, y + h / 2 - 3, focus ? INK : GRN);
    char t[72], f[80]; lg_title(i, t, sizeof t);
    const char *shown = (!r->dir && strip && (int)strlen(t) > strip) ? t + strip : t;
    lg::fit(f, sizeof f, shown, w - 22 - 16 - mw - (is_resume ? 10 : 0));
    lg::row_text(x + 22, y, w - 22, h, focus, bg, f, mb, r->dir ? AMB : FG);
}

static void lg_card(int x, int y, int w, int h)
{
    lg::panel(x, y, w, h);
    if (s_sel >= s_n) return;
    const unsigned short cb = lg::card();
    const VEntry *r = &st->e[s_sel];
    // the action, at the card's foot: what a tap on the card does
    if (!r->old) {
        const int bh = 30;
        lg::button(x + 8, y + h - bh - 8, w - 16, bh, r->dir ? TR("Apri", "Open") : r->resume > 0 ? TR("Riprendi", "Resume") : TR("Riproduci", "Play"),
                   r->dir ? AMB : (r->resume > 0 ? GRN : ACC), INK, NK_ENTER, 0);
        h -= bh + 8;                                          // the details live above it
    }
    char t[72]; lg_title(s_sel, t, sizeof t);
    int cy = y + 8;
    // the big line: the clip length (or the folder's clip count)
    char big[16];
    if (r->dir) snprintf(big, sizeof big, "%u", (unsigned)r->count); else if (r->dur) fmt_mmss(big, sizeof big, r->dur); else snprintf(big, sizeof big, "--");
    lg::font(2); d.setTextDatum(top_left); d.setTextColor(r->dir ? AMB : ACC, cb);
    d.drawString(big, x + 10, cy);
    lg::font(4); d.setTextColor(MUTED, cb);
    d.drawString(r->dir ? "video" : TR("durata", "length"), x + 10, cy + 26);
    cy += 42;
    d.drawFastHLine(x + 10, cy, w - 20, LINE);
    cy += 7;
    lg::font(0); d.setTextColor(FG, cb);
    cy += 17 * lg::wrap(t, x + 10, cy, w - 20, 17, 4) + 4;
    lg::font(4);
    if (r->old) {
        d.setTextColor(AMB, cb);
        d.drawString(TR("Formato vecchio:", "Old format:"), x + 10, cy);
        d.drawString(TR("riconvertilo.", "convert it again."), x + 10, cy + 11);
    } else if (!r->dir) {
        char m[32];
        if (r->vw) { snprintf(m, sizeof m, "%ux%u", (unsigned)r->vw, (unsigned)r->vh); d.setTextColor(MUTED, cb); d.drawString(m, x + 10, cy); cy += 12; }
        if (r->resume > 0 && r->dur) {                        // where you stopped, as a bar
            char rs[8]; fmt_mmss(rs, sizeof rs, r->resume);
            snprintf(m, sizeof m, "%s %s", TR("riprendi da", "resume at"), rs);
            d.setTextColor(GRN, cb); d.drawString(m, x + 10, cy);
            lg::bar(x + 10, cy + 12, w - 20, 4, (int)((int64_t)100 * r->resume / r->dur), GRN);
        }
    }
}

static void draw_large(void)
{
    if (!st) return;
    const int W = d.width(), H = d.height();
    nucleo_touch_clear();
    // header: where we are + how many; the back chevron leaves a folder, the menu opens the settings
    const bool sub = strcmp(s_path, "/") != 0;
    char ctx[48];
    if (sub) { snprintf(ctx, sizeof ctx, "%s", s_path + 1); size_t l = strlen(ctx); if (l && ctx[l - 1] == '/') ctx[l - 1] = 0; }
    else snprintf(ctx, sizeof ctx, "Video");
    char cnt[24]; snprintf(cnt, sizeof cnt, "%d %s", s_n, s_n == 1 ? TR("elemento", "item") : TR("elementi", "items"));
    const int body = lg::header(ctx, cnt, ACC, sub ? NK_DEL : NK_BACK, NK_TAB), bh = H - body;
    s_vtap_n = 0;
    if (s_set_open || s_help_open) {                         // keyboard work happens on the Cardputer
        lg::message(0, body, W, bh, s_help_open ? TR("Guida aperta sul Cardputer", "Manual open on the Cardputer")
                                                : TR("Impostazioni sul Cardputer", "Settings on the Cardputer"), nullptr, MUTED);
        nucleo_touch_commit();
        return;
    }
    if (s_n == 0) {
        lg::message(0, body, W, bh, strcmp(s_path, "/") ? TR("Cartella vuota", "Empty folder") : TR("Nessun video in /data/Videos", "No videos in /data/Videos"),
                    TR("Convertili con l'app web Video Studio", "Convert them with the Video Studio web app"), FG);
        nucleo_touch_commit();
        return;
    }
    // the list (left), the picked clip's card (right): tap the card = play / open
    d.fillRect(0, body, LG_LISTW, 2, BG);
    lg::list(0, body + 2, LG_LISTW, bh - 4, lg::ROW, s_n, s_sel, &s_scroll, lg_row, nullptr, ACC);
    d.fillRect(0, H - 2, LG_LISTW, 2, BG);
    d.fillRect(LG_LISTW, body, W - LG_LISTW, bh, BG);
    lg_card(LG_LISTW + 4, body + 4, W - LG_LISTW - 10, bh - 8);
    nucleo_touch_add(LG_LISTW, body, W - LG_LISTW, bh, NK_ENTER, 0);
    nucleo_touch_commit();
}

// The Cardputer while the panel shows the list: the picked title, large, and the keys.
static void draw_companion(int top, int ch)
{
    d.fillRect(0, top, 240, ch, BG);
    d.setTextDatum(top_left);
    d.setFont(&fonts::Font0); d.setTextColor(ACC, BG);
    d.drawString(TR("VIDEO  -  sul secondo schermo", "VIDEO  -  on the second screen"), 8, top + 4);
    if (s_sel < s_n) {
        const VEntry *r = &st->e[s_sel];
        char t[72]; lg_title(s_sel, t, sizeof t);
        d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(r->dir ? AMB : FG, BG);
        const int nl = lg::wrap(t, 8, top + 18, 224, 20, 3);
        d.setFont(&fonts::Font2);
        char m[40] = "";
        if (r->dir) snprintf(m, sizeof m, "%u video", (unsigned)r->count);
        else if (r->old) snprintf(m, sizeof m, "%s", TR("formato vecchio", "old format"));
        else if (r->resume > 0) { char a[8], b[8]; fmt_mmss(a, sizeof a, r->resume); fmt_mmss(b, sizeof b, r->dur); snprintf(m, sizeof m, "%s %s / %s", TR("riprendi", "resume"), a, b); }
        else if (r->dur) fmt_mmss(m, sizeof m, r->dur);
        int my = top + 22 + nl * 20;
        d.setTextColor(r->resume > 0 && !r->dir ? GRN : MUTED, BG);
        d.drawString(m, 8, my);
        if (!r->dir && !r->old && r->vw) {                    // the picture, right-aligned on the same line
            char res[16]; snprintf(res, sizeof res, "%ux%u", (unsigned)r->vw, (unsigned)r->vh);
            d.setTextDatum(top_right); d.setTextColor(DIM, BG); d.drawString(res, 232, my); d.setTextDatum(top_left);
        }
        my += 20;
        if (!r->dir && r->resume > 0 && r->dur) {             // where you stopped
            const int fill = (int)((int64_t)224 * r->resume / r->dur);
            d.fillRoundRect(8, my, 224, 4, 2, LINE); d.fillRoundRect(8, my, fill < 4 ? 4 : fill, 4, 2, GRN);
            my += 10;
        }
        // where the pick sits in the folder: a dot per entry (a bar past 24)
        const int bx = 8, bw = 224, by = top + ch - 8;
        if (s_n <= 24) for (int i = 0; i < s_n; i++) {
            const int cx = bx + (s_n == 1 ? bw / 2 : i * bw / (s_n - 1));
            d.fillCircle(cx, by, i == s_sel ? 3 : 2, i == s_sel ? ACC : LINE);
        } else {
            d.fillRect(bx, by - 1, bw, 2, LINE);
            d.fillRect(bx + (bw - 12) * s_sel / (s_n - 1), by - 2, 12, 4, ACC);
        }
    }
    d.setFont(&fonts::Font0);
}

static void draw(void)
{
    if (!st) return;
    int ch = nucleo_app_content_height();
    int top = nucleo_app_content_top();
    if (s_help_open) { draw_help(ch); return; }
    if (s_set_open) { draw_settings(ch); return; }
    if (nucleo_app_two_screens()) { draw_companion(top, ch); return; }

    draw_header(top, s_n);
    int lt = top + HEAD_H, lh = ch - HEAD_H;

    if (s_n == 0) {
        d.fillRect(0, lt, 240, lh, BG);
        d.setTextSize(1); d.setTextColor(DIM, BG);
        if (strcmp(s_path, "/")) { d.setCursor(12, lt + 16); d.print("(empty folder)"); }
        else {
            d.setCursor(12, lt + 16); d.print("No .nfv clips in /data/Videos");
            d.setCursor(12, lt + 30); d.print(TR("Converti: app web Video Studio", "Convert: Video Studio web app"));
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
    // the list comes up EMPTY ("I don't see any files"). Holding it from enter() gives both browser and player
    // the headroom. (Do NOT add NX_WIFI: tearing the radio down+up is the ADV's fragile path — it left
    // audio dead + SD flaky for an extra ~30-50KB we don't need; an .nfv plays off SD, no network.)
    vtrace("play_enter", true);

    for (;;) {
        s_sel = idx;                                         // keep the list selection on the live clip
        char vpath[256], title[72], key[200], ntbuf[72];
        clip_paths(idx, vpath, sizeof vpath, title, sizeof title, key, sizeof key);
        const char *nt = NULL;                               // "Up next" preview for autoplay/repeat
        if (s_autoplay || s_repeat) { int an = next_to_play(idx); if (an >= 0 && an != idx) { clip_title(an, ntbuf, sizeof ntbuf); nt = ntbuf; } }

        nucleo_app_display_hold(true);                       // a film is watched, not touched: stay lit
        int r = play_nfv(vpath, title, key, start_ms, nt);
        nucleo_app_display_hold(false);
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
    nucleo_app_display_hold(true);
    play_nfv(abs, title, abs, 0, NULL);                       // reskey = abs path (unique resume key)
    nucleo_app_display_hold(false);
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

    if (nucleo_touch_is_row(key, ch)) {                       // a tapped row: pick it, or open it if picked
        int slot = ch - NUCLEO_TOUCH_ROW;
        if (slot >= s_vtap_n) return;
        if (s_vtap[slot] != s_sel) { s_sel = s_vtap[slot]; nucleo_app_request_draw(); return; }
        key = NK_ENTER; ch = 0;
    }
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
                    if (rr < 0) { d.fillScreen(BG); nucleo_app_force_repaint(); return; }   // resume_prompt drew direct-to-panel: force a full re-blit (same as return_to_list)
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
        case 1:  nucleo_app_set_hint(TR5("DX scheda \xb7 SU/GIU riga \xb7 ESC chiudi", "RIGHT tab \xb7 UP/DN row \xb7 ESC close", "DER tab \xb7 AR/AB fila \xb7 ESC cierra", "DR. onglet \xb7 H/B ligne \xb7 ESC ferme", "RECHTS Tab \xb7 AUF/AB Zeile \xb7 ESC zu")); break;
        case 2:  nucleo_app_set_hint(TR5("SX/DX regola \xb7 INVIO fatto", "L/R adjust \xb7 ENTER done", "IZQ/DER ajusta \xb7 INTRO ok", "G/D regle \xb7 ENTREE ok", "L/R regeln \xb7 ENTER ok")); break;
        default: nucleo_app_set_hint(TR5("invio apri \xb7 canc su \xb7 TAB menu", "enter play/open \xb7 del up \xb7 TAB menu", "intro abre \xb7 supr subir \xb7 TAB menu", "entree lire \xb7 suppr haut \xb7 TAB menu", "enter Start \xb7 entf hoch \xb7 TAB Menue"));
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
        NX_NET_APP | NX_SOLO | NX_WIFI,  // SOLO + WIFI: a media center — the Solo boot starts no radio at all
                               // (no Wi-Fi, no BLE, no httpd/voice/L1): ~48 KB more heap, no radio interrupts.
                               // SOLO: reboot into a fresh, unfragmented heap so MP3 decoder + JPEG buffer
                               // + file list coexist (inline reclaim left the decoder short -> silent video).
                               // NX_NET_APP bits are no-ops in Solo (services down at boot) but kept defensively
                               // for the non-Solo fallback path. Esc reboots back to the full OS.
        nullptr,                       // no per-app RAM block
        draw_large,                    // two screens: the browser on the 320x240 panel, the Cardputer as companion
    };
    nucleo_app_register(&app);
}
