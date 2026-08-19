// Music player v4 — folder browser + MP3/WAV for Cardputer, watch-grade UI.
// Browser: fisheye list (focused row enlarged), legible white labels, amber folders,
//   playing-track indicator, favourite hearts, slim context header, mini now-playing pill.
// Now Playing: full-screen card with a circular progress ring + play/pause core, a marquee
//   title, favourite heart, artist/genre, big time, volume bar and an up-next line.
// Settings (TAB): 4-tab sheet with a persistent dotted tab bar, toggle pills and button rows.
// Shuffle, Repeat (Off/One/All), Autoplay, Start-Volume — persisted to /system/config/player.json.
// Discipline (see ANTI-FLICKER.md): nothing repaints on a clock cadence except the smallest
//   region that actually changed.
//   - Browser: while a track plays, a passing second repaints ONLY the mini strip (time +
//     progress) and the playing row's groove in place — never the whole list/header (technique 2).
//     The list/header repaint only on key/structural change.
//   - Now-Playing title marquee renders into its OWN small off-screen sprite and blits once
//     (technique 3): no per-frame band clear on the panel, so no flicker and no text ghosting.
#include "nucleo_app.h"
#include "nucleo_audio.h"
#include "app_ui.h"
#include "app_player_db.h"
#include <M5GFX.h>
#include <new>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nucleo_exclusive.h"   // NX_NET_APP: dedicated-mode RAM reclaim (~70KB) while a track plays
#include "freertos/task.h"
extern "C" {
#include "nucleo_audio.h"
#include "nucleo_board.h"
#include "nucleo_kbd.h"
#include "cJSON.h"
}
#include "app_gfx.h"
#include "nucleo_theme.h"       // THEME_*: chrome follows the live theme (docs/native-ui-kit.md)
#include "nucleo_i18n.h"        // TR(it,en): UI labels follow the system language

// ---- Design tokens — CHROME follows the LIVE theme (docs/native-ui-kit.md rule 1) ---------------
// These were baked RGB565 literals, so Music drew its own blue-on-void-blue whatever the user had
// chosen: under Hacker Green or AMOLED it was the one screen that ignored the Theme setting. A theme
// switch rewrites these globals at runtime, so the app now follows along for free.
#define BG    THEME_BG
#define FG    THEME_FG
#define MUTED THEME_MUTED
#define DIM   THEME_DIM
#define LINE  THEME_LINE
#define INK   THEME_INK
#define ACC   THEME_ACC
// Track/groove fill behind a bar: the kit assigns that role to LINE. The old private "raised surface"
// and "focused capsule" tints (0x10A2 / 0x1A8B) are explicitly NOT palette roles — across the OS a
// focused row is an ACC-filled pill with INK text, and this app now matches instead of inventing one.
#define SURF  THEME_LINE
#define CAP   THEME_ACC
// ---- Content colors: real semantics, allowed by the kit as named constants ----------------------
#define GRN   0x8FF3 // playing / positive — mint green
#define AMB   0xFE8C // paused / folders / warm accent
#define RED   0xF96B // favourite / stop — warm red

#define MUSIC_DIR     NUCLEO_SD_MOUNT "/data/Music"
#define SETTINGS_PATH NUCLEO_SD_MOUNT "/system/config/player.json"
// Tenuti PICCOLI come il Video app (VState ~5 KB) di proposito: PState va allocato in UN blocco
// contiguo all'apertura, e su questo heap frammentato senza PSRAM il blocco grande non c'e' (96+64
// voci facevano ~10,5 KB -> calloc falliva -> "RAM insufficiente" e Music non apriva). 48 voci per
// cartella + 32 in coda portano PState a ~5,3 KB, in linea col Video che apre senza problemi.
#define MAXE   48   // max entries in current folder
#define MAXQ   32   // max queued tracks
#define HEAD_H 20   // list context-header height
#define STRIP_H 22  // mini now-playing strip height

struct MEntry { char name[56]; bool dir; uint16_t count; uint32_t dur; };

struct PState {
    char playpath[208];
    int qidx, qn;
    char search_query[64];
    int filter_type; // -1=Folder, 0=Title, 1=Genre, 2=Artist, 3=Fav, 4=MostPlayed
    MEntry e[MAXE];
    char   qdir[208];         // queue folder (abs path)
    char   q[MAXQ][56];       // queued filenames (original order)
    int    shuf[MAXQ];        // shuffled position -> q[] index
};
static PState *st = nullptr;

static char s_path[192] = "/";  // current folder under MUSIC_DIR
static int  s_n, s_sel, s_scroll;
// Per-second strip update bookkeeping (browser view). A structural change (started/paused/
// stopped) forces one full repaint; a passing second/percent only refreshes the strip in place.
static int  s_strip_struct = -1;  // last structural playback state: 0 idle, 1 paused, 2 playing
static int  s_strip_el     = -1;  // last elapsed seconds painted into the strip
static int  s_strip_pct    = -1;  // last progress percent painted into the strip
static int  s_groove_y     = -1;  // panel-y of the focused playing row's groove, or -1 (none)

// ---- persisted settings ------------------------------------------------------
static bool s_shuffle     = false;
static int  s_repeat      = 0;    // 0=off 1=one 2=all
static bool s_autoplay    = false;
static int  s_vol_default = 0;    // 0=keep last; else set this volume at play-start

// ---- settings panel state ----------------------------------------------------
static bool s_set_open = false;
static int  s_set_tab  = 0;   // logical tab id: 0=Play 1=Audio 2=Queue 3=Find
static int  s_set_row  = 0;   // -1 = tab header, 0..n-1 = row
static bool s_set_edit = false;   // volume slider in adjust mode (UP/DN change it)
static const int SET_ROWS[] = {3, 2, 3, 6};   // indexed by logical tab id
static bool s_has_music = false;  // any audio anywhere under /data/Music (gates Find tab)

// Which settings tabs are available right now (Queue needs a queue, Find needs music).
static int settings_avail(int out[4])
{
    int n = 0;
    out[n++] = 0;                       // Play   (always)
    out[n++] = 1;                       // Audio  (always)
    if (st && st->qn > 0) out[n++] = 2; // Queue  (only with a queue)
    if (s_has_music)      out[n++] = 3; // Find   (only with music)
    return n;
}
static int tab_index(const int *a, int n, int id) { for (int i = 0; i < n; i++) if (a[i] == id) return i; return 0; }

static bool s_typing = false;
static bool s_sub_list = false;
static int s_sub_type = 0; 
static char **s_sub_items = NULL;
static int s_sub_count = 0;
static int s_sub_sel = 0;
static bool s_play_counted = false;

static int next_pos(void);

// forward declarations
static void on_tab(void);
static void now_playing(void);
static void np_mq_free(void);   // marquee sprite teardown (used by leave() before its definition)

// ---- settings I/O -----------------------------------------------------------
static void load_settings(void)
{
    FILE *f = fopen(SETTINGS_PATH, "rb"); if (!f) return;
    char buf[192]; int n = (int)fread(buf, 1, sizeof buf - 1, f); fclose(f);
    if (n <= 0) return;
    buf[n] = 0;
    cJSON *root = cJSON_Parse(buf); if (!root) return;
    cJSON *v;
    if ((v = cJSON_GetObjectItem(root, "shuffle"))     && cJSON_IsBool(v))   s_shuffle     = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(root, "repeat"))      && cJSON_IsNumber(v)) s_repeat      = (int)v->valuedouble;
    if ((v = cJSON_GetObjectItem(root, "autoplay"))    && cJSON_IsBool(v))   s_autoplay    = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(root, "vol_default")) && cJSON_IsNumber(v)) s_vol_default = (int)v->valuedouble;
    if (s_repeat < 0 || s_repeat > 2) s_repeat = 0;
    if (s_vol_default < 0 || s_vol_default > 100) s_vol_default = 0;
    cJSON_Delete(root);
}

static void save_settings(void)
{
    char buf[192];
    snprintf(buf, sizeof buf,
        "{\"shuffle\":%s,\"repeat\":%d,\"autoplay\":%s,\"vol_default\":%d}",
        s_shuffle ? "true" : "false", s_repeat,
        s_autoplay ? "true" : "false", s_vol_default);
    mkdir(NUCLEO_SD_MOUNT "/system", 0775);
    mkdir(NUCLEO_SD_MOUNT "/system/config", 0775);
    FILE *f = fopen(SETTINGS_PATH, "wb");
    if (f) { fwrite(buf, 1, strlen(buf), f); fclose(f); }
}

// ---- helpers ----------------------------------------------------------------
static bool is_audio(const char *n)
{
    const char *dot = strrchr(n, '.'); if (!dot) return false;
    return !strcasecmp(dot, ".mp3") || !strcasecmp(dot, ".wav");
}
static int cmp_entry(const void *a, const void *b)
{
    const MEntry *x = (const MEntry *)a, *y = (const MEntry *)b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}
static void fmt_time(char *b, size_t n, uint32_t s)
{ snprintf(b, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60)); }

// ---- duration estimation (header-read only, no decode) ----------------------
static uint32_t track_seconds(const char *abs)
{
    const char *dot = strrchr(abs, '.');
    bool wav = dot && !strcasecmp(dot, ".wav");
    FILE *f = fopen(abs, "rb"); if (!f) return 0;
    
    fseek(f, 0, SEEK_END);
    uint32_t fsize = (uint32_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    
    uint32_t secs = 0;
    if (wav) {
        uint8_t h[64]; size_t n = fread(h, 1, sizeof h, f);
        if (n >= 44 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) {
            uint32_t byterate = h[28]|(h[29]<<8)|(h[30]<<16)|((uint32_t)h[31]<<24);
            uint32_t datasz = 0;
            if (!memcmp(h + 36, "data", 4)) datasz = h[40]|(h[41]<<8)|(h[42]<<16)|((uint32_t)h[43]<<24);
            if (!datasz && fsize > 44) datasz = fsize - 44;
            if (byterate) secs = datasz / byterate;
        }
    } else {
        uint8_t b[10]; long pos = 0;
        if (fread(b, 1, 10, f) == 10 && b[0]=='I' && b[1]=='D' && b[2]=='3') {
            uint32_t id3 = ((b[6]&0x7f)<<21)|((b[7]&0x7f)<<14)|((b[8]&0x7f)<<7)|(b[9]&0x7f);
            pos = 10 + (long)id3;
        }
        fseek(f, pos, SEEK_SET);
        uint8_t buf[1024]; int n = (int)fread(buf, 1, sizeof buf, f), i = 0;
        for (; i + 4 <= n; i++) if (buf[i]==0xFF && (buf[i+1]&0xE0)==0xE0) break;
        if (i + 4 <= n) {
            uint8_t h1 = buf[i+1], h2 = buf[i+2];
            int ver = (h1>>3)&3, brIdx = (h2>>4)&0xF, srIdx = (h2>>2)&3, chan = (buf[i+3]>>6)&3;
            static const int br1[16]={0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
            static const int br2[16]={0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};
            static const int sr1[4]={44100,48000,32000,0},sr2[4]={22050,24000,16000,0},sr25[4]={11025,12000,8000,0};
            int bitrate = (ver==3)?br1[brIdx]:br2[brIdx];
            int sr = (ver==3)?sr1[srIdx]:(ver==2)?sr2[srIdx]:sr25[srIdx];
            int xoff = i+4+((ver==3)?(chan==3?17:32):(chan==3?9:17));
            uint32_t nframes = 0;
            if (xoff+12<=n && (!memcmp(buf+xoff,"Xing",4)||!memcmp(buf+xoff,"Info",4))) {
                uint32_t flags=((uint32_t)buf[xoff+4]<<24)|(buf[xoff+5]<<16)|(buf[xoff+6]<<8)|buf[xoff+7];
                if (flags&1) nframes=((uint32_t)buf[xoff+8]<<24)|(buf[xoff+9]<<16)|(buf[xoff+10]<<8)|buf[xoff+11];
            }
            int spf = (ver==3)?1152:576;
            if (nframes && sr) secs=(uint32_t)((uint64_t)nframes*spf/sr);
            else if (bitrate && fsize>(uint32_t)pos) secs=(uint32_t)((uint64_t)(fsize-pos)*8/((uint32_t)bitrate*1000));
        }
    }
    fclose(f); return secs;
}

// Usa stat() per determinare se un path è una directory.
// d_type è inaffidabile su FATFS (restituisce DT_UNKNOWN).
static bool is_dir_stat(const char *path)
{
    struct stat sb;
    if (stat(path, &sb) != 0) return false;
    return S_ISDIR(sb.st_mode);
}

static int count_songs(const char *abs, int depth)
{
    if (depth > 4) return 0;
    esp_task_wdt_reset();
    DIR *dir = opendir(abs); if (!dir) return 0;
    struct dirent *e; int total = 0;
    while ((e = readdir(dir)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char child[320]; snprintf(child, sizeof child, "%s/%s", abs, e->d_name);
        if (is_dir_stat(child)) {
            total += count_songs(child, depth+1);
        } else if (is_audio(e->d_name)) {
            total++;
        }
        if (total > 9999) break;
    }
    closedir(dir); return total;
}

static void scan(void)
{
    s_n = 0; s_sel = 0; s_scroll = 0;
    if (!st) return;
    
    if (st->filter_type >= 0) {
        struct TrackMeta *res = NULL;
        const char *q = st->filter_type == 0 ? st->search_query : (st->search_query[0] ? st->search_query : NULL);
        int count = music_db_search(q, st->filter_type, &res);
        if (res) {
            for (int i=0; i<count && i<MAXE; i++) {
                MEntry *r = &st->e[s_n];
                snprintf(r->name, sizeof r->name, "%s", res[i].path);
                r->dir = false; r->count = 0; r->dur = 0;
                s_n++;
            }
            free(res);
        }
        return;
    }
    
    char base[208]; snprintf(base, sizeof base, "%s%s", MUSIC_DIR, s_path);
    DIR *dir = opendir(base); if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && s_n < MAXE) {
        if (e->d_name[0] == '.') continue;
        char child[320]; snprintf(child, sizeof child, "%s/%s", base, e->d_name);
        bool isdir = is_dir_stat(child);
        if (!isdir && !is_audio(e->d_name)) continue;
        MEntry *r = &st->e[s_n];
        snprintf(r->name, sizeof r->name, "%s", e->d_name);
        r->dir = isdir; r->count = 0; r->dur = 0;
        s_n++;
    }
    closedir(dir);
    qsort(st->e, s_n, sizeof(MEntry), cmp_entry);
    for (int i = 0; i < s_n; i++) {
        MEntry *r = &st->e[i];
        char c[320]; snprintf(c, sizeof c, "%s%s", base, r->name);
        if (r->dir) r->count = (uint16_t)count_songs(c, 0);
        else        r->dur   = track_seconds(c);
        esp_task_wdt_reset();
    }
}

static void go_up(void)
{
    if (!strcmp(s_path, "/")) return;
    int l = strlen(s_path); if (l && s_path[l-1]=='/') s_path[--l]=0;
    char *slash = strrchr(s_path, '/');
    if (slash) slash[1]=0; else strcpy(s_path, "/");
    scan();
}
static void descend(const char *name)
{
    int l = strlen(s_path);
    snprintf(s_path+l, sizeof(s_path)-l, "%s/", name);
    scan();
}

static void rebuild_shuffle(void)
{
    if (!st) return;
    for (int i = 0; i < MAXQ; i++) st->shuf[i] = i;
    if (!s_shuffle || st->qn <= 1) return;
    srand((unsigned int)(esp_timer_get_time() & 0xFFFFFFFF));
    for (int i = st->qn - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = st->shuf[i]; st->shuf[i] = st->shuf[j]; st->shuf[j] = tmp;
    }
    int cur = st->qidx;
    for (int i = 0; i < st->qn; i++) {
        if (st->shuf[i] == cur) {
            int tmp = st->shuf[0]; st->shuf[0] = st->shuf[i]; st->shuf[i] = tmp;
            break;
        }
    }
    st->qidx = 0;
}

static int track_at(int pos)
{
    if (!st || pos < 0 || pos >= st->qn) return 0;
    return s_shuffle ? st->shuf[pos] : pos;
}

// ── Dedicated-mode RAM window ────────────────────────────────────────────────────────────────────────
// Music now declares NX_NET_APP | NX_SOLO in its app_def, exactly like the Video player, so the
// framework applies the window BEFORE on_enter runs and this file no longer manages it by hand.
//
// Why the runtime reclaim alone was NOT enough: it frees RAM but CANNOT DEFRAGMENT (see
// nucleo_exclusive.h). The Helix decoder needs ~20-24 KB CONTIGUOUS across its 8 allocations, and on a
// heap that has already hosted httpd + the L1 index + a dozen app sessions, the free total can be
// plentiful while no single block is big enough — MP3InitDecoder fails and the track plays SILENTLY.
// That is exactly the "out of RAM: MP3 decoder" verdict this app now reports. NX_SOLO reboots into the
// app on a fresh, unfragmented heap (httpd/mDNS/launcher never start), which is the only thing that
// actually produces a contiguous block that size — the same reason Video and the heavy games use it.
// Cost: opening Music reboots, and Esc reboots back to the full OS.

static void play_q(int pos)
{
    if (!st || pos < 0 || pos >= st->qn) return;
    int ti = track_at(pos);
    char abs[300]; snprintf(abs, sizeof abs, "%s%s", st->qdir, st->q[ti]);

    // Hand the shared ~32 KB canvas back to the heap BEFORE the Helix decoder starts: every play
    // path funnels through here (browser autoplay/skip, queue advance, now_playing), so doing it
    // once here covers them all. Idempotent — re-acquire is lazy when the list repaints.
    nucleo_app_release_buffers();

    // Give the FreeRTOS IDLE task a tick to reclaim the just-ended player task's stack (freed
    // DEFERRED after its vTaskDelete). On an auto-advance the previous "audio" task has only just
    // self-deleted, so its ~8 KB stack is still counted as in-use right here — long enough to drop
    // the largest free block under a rigid pre-check and SILENTLY kill the advance (the next track
    // never plays). Yield first, then let nucleo_audio's own reclaim-cb + retry (start_play_window)
    // make the final RAM call: it logs an honest error on real OOM and we surface it as the hint below.
    vTaskDelay(pdMS_TO_TICKS(20));

    if (nucleo_audio_play(abs) == ESP_OK) {
        st->qidx = pos;
        snprintf(st->playpath, sizeof st->playpath, "%s", abs);
        if (s_vol_default > 0) nucleo_audio_set_volume(s_vol_default);
    } else {
        nucleo_app_set_hint(TR("Riproduzione non riuscita", "Playback failed"));
    }
}

static int next_pos(void)
{
    if (!st) return -1;
    if (s_repeat == 1) return st->qidx;
    if (st->qidx < st->qn - 1) return st->qidx + 1;
    return (s_repeat == 2) ? 0 : -1;
}
static int prev_pos(void)
{
    if (!st) return 0;
    if (st->qidx > 0) return st->qidx - 1;
    return (s_repeat == 2) ? st->qn - 1 : 0;
}

// ---- Iconography (small vector glyphs; x,y = top-left of a ~12px cell) -------
static void icon_folder(int x, int y, unsigned short c)
{ d.fillRect(x, y+1, 6, 2, c); d.fillRoundRect(x, y+2, 12, 8, 2, c); }
static void icon_note(int x, int y, unsigned short c)
{ d.fillCircle(x+2, y+8, 2, c); d.fillRect(x+4, y+1, 2, 7, c); d.fillRect(x+4, y, 5, 2, c); }
static void icon_heart(int x, int y, unsigned short c)
{ d.fillCircle(x+2, y+2, 2, c); d.fillCircle(x+6, y+2, 2, c); d.fillTriangle(x, y+2, x+8, y+2, x+4, y+8, c); }
static void icon_play(int x, int y, int sz, unsigned short c)
{ d.fillTriangle(x, y, x, y+sz, x + sz*7/8, y + sz/2, c); }
static void icon_pause(int x, int y, int sz, unsigned short c)
{ int bw = sz/3; if (bw < 2) bw = 2; d.fillRect(x, y, bw, sz, c); d.fillRect(x + bw + sz/4, y, bw, sz, c); }
// Static 3-bar "now playing" mark (no animation in the list view).
static void icon_eq(int x, int y, unsigned short c)
{ d.fillRect(x, y+4, 2, 4, c); d.fillRect(x+3, y+1, 2, 7, c); d.fillRect(x+6, y+3, 2, 5, c); }

static bool row_is_playing(MEntry *r, const char *base)
{
    if (r->dir || !st || !st->playpath[0]) return false;
    char abs[300]; snprintf(abs, sizeof abs, "%s%s", base, r->name);
    return !strcmp(abs, st->playpath);
}

// Thin progress groove inside the focused playing row's capsule. Drawn during the list paint
// (which records s_groove_y) and re-drawn in place each second by tick() — INK on the capsule,
// a 2px region, so the per-second refresh is imperceptible (no row/list re-render).
static void draw_row_groove(void)
{
    if (s_groove_y < 0) return;
    int pct = nucleo_audio_progress(); if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    d.fillRect(8, s_groove_y, 224, 2, BG);
    d.fillRect(8, s_groove_y, 224 * pct / 100, 2, INK);
}

static void draw_row(int i, int y, int h, bool focus, const char *base)
{
    MEntry *r = &st->e[i];
    bool plays  = row_is_playing(r, base);
    bool paused = plays && nucleo_audio_is_paused();
    const char *bn = strrchr(r->name, '/'); bn = bn ? bn + 1 : r->name;
    bool fav = !r->dir && music_db_is_fav(bn);
    // per-row accent: folders=amber, playing=green (amber if paused), tracks=blue
    unsigned short acc = r->dir ? AMB : (plays ? (paused ? AMB : GRN) : ACC);

    d.fillRect(0, y, 240, h, BG);

    // right-aligned meta: duration for tracks, song count for folders
    char mb[12] = ""; int mw = 0;
    if (r->dir)      snprintf(mb, sizeof mb, "%u", (unsigned)r->count);
    else if (r->dur) fmt_time(mb, sizeof mb, r->dur);
    if (mb[0]) mw = (int)strlen(mb) * 6;

    if (focus) {
        // bright capsule, INK content
        d.fillRoundRect(4, y+1, 232, h-2, 7, acc);
        int iy = y + (h-12)/2;
        if (r->dir)     icon_folder(12, iy+1, INK);
        else if (plays) { if (paused) icon_pause(13, iy+1, 10, INK); else icon_eq(12, iy+2, INK); }
        else            icon_note(13, iy, INK);

        int meta_x = 230 - mw;
        if (mb[0]) { d.setTextSize(1); d.setTextColor(INK, acc); d.setCursor(meta_x, y + (h-8)/2); d.print(mb); }
        int heart_x = meta_x;
        if (fav) { heart_x = meta_x - 12; icon_heart(meta_x - 11, y + (h-8)/2, INK); }

        int nx = 30, navail = (heart_x - 8) - nx; if (navail < 12) navail = 12;
        int maxc = navail / 12; if (maxc < 1) maxc = 1; if (maxc > 22) maxc = 22;
        char nb[24]; snprintf(nb, sizeof nb, "%.*s", maxc, bn);
        d.setTextSize(2); d.setTextColor(INK, acc); d.setCursor(nx, y + (h-16)/2); d.print(nb);

        // thin progress groove inside the capsule for the playing track — remember its y so a
        // passing second can refresh just this 2px band (no full row repaint).
        if (plays && !r->dir) { s_groove_y = y + h - 4; draw_row_groove(); }
    } else {
        unsigned short namec = plays ? acc : (r->dir ? AMB : FG);
        if (r->dir)     icon_folder(9, y + (h-10)/2, AMB);
        else if (plays) icon_eq(10, y + (h-8)/2, acc);
        else            d.fillCircle(12, y + h/2, 2, MUTED);

        if (mb[0]) { d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(230 - mw, y + (h-8)/2); d.print(mb); }
        int heart_x = 230 - mw;
        if (fav) { heart_x = 230 - mw - 11; icon_heart(230 - mw - 10, y + (h-8)/2, RED); }

        int nx = 24, navail = (heart_x - 6) - nx; if (navail < 6) navail = 6;
        int maxc = navail / 12; if (maxc < 1) maxc = 1; if (maxc > 18) maxc = 18;
        char nb[20]; snprintf(nb, sizeof nb, "%.*s", maxc, bn);
        d.setTextSize(2); d.setTextColor(namec, BG); d.setCursor(nx, y + (h-16)/2); d.print(nb);
    }
}

// Slim context header: accent title (folder / filter / "Music") + item count + accent rule.
static void draw_header(int top, const char *ctx, int count)
{
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

static void draw_list(int y0, int region_h, const char *base)
{
    if (s_sel < s_scroll) s_scroll = s_sel;
    int scan_y = 0;
    for (int i = s_scroll; i <= s_sel; i++) scan_y += (i == s_sel) ? 28 : 20;
    while (scan_y > region_h && s_scroll < s_sel) {
        scan_y -= (s_scroll == s_sel) ? 28 : 20;
        s_scroll++;
    }

    int y = y0;
    for (int i = s_scroll; i < s_n && y < y0 + region_h; i++) {
        int h = (i == s_sel) ? 28 : 20;
        if (y + h > y0 + region_h && i != s_scroll) break;
        draw_row(i, y, h, i == s_sel, base);
        y += h;
    }

    if (y < y0 + region_h) d.fillRect(0, y, 240, (y0 + region_h) - y, BG);

    // Capsule scroll knob over a faint track (watch-style).
    if (s_n > 1) {
        int track = region_h - 6; if (track < 12) track = 12;
        int kh = track * region_h / (16 * s_n); if (kh < 12) kh = 12; if (kh > track) kh = track;
        int ky = y0 + 3 + (track - kh) * s_sel / (s_n - 1);
        d.fillRoundRect(236, y0 + 3, 2, track, 1, LINE);
        d.fillRoundRect(236, ky, 2, kh, 1, ACC);
    }
}

// Mini now-playing pill at the bottom of the browser: glanceable transport + progress.
// full=true  -> paint the whole pill (chrome, disc, title, badge) — on key/structural change.
// full=false -> refresh ONLY the per-second regions (elapsed time + progress line) in place,
//               without the whole-strip clear, so a passing second never flickers the bar.
// The static layout (badge -> time field -> title) is computed identically on both paths so the
// in-place time refresh lands exactly where the full paint put it. (ANTI-FLICKER technique 2.)
static void draw_mini_strip(int y, bool full)
{
    bool paused = nucleo_audio_is_paused();
    // A hard mute is device-wide and survives a reboot, so the strip must not paint a confident green
    // "playing" pill while the engine is writing silence. Muted takes the grey, and the badge says so.
    bool muted = nucleo_audio_is_muted();
    unsigned short c = muted ? MUTED : (paused ? AMB : GRN);
    int mid = y + 1 + (STRIP_H - 2) / 2;

    // Right side: optional mode badge, then a fixed-width time field to its left.
    char badge[12] = "";
    if (muted) strcat(badge, "MUTE ");
    if (s_shuffle) strcat(badge, "~");
    if (s_repeat == 1) strcat(badge, " R1");
    else if (s_repeat == 2) strcat(badge, " RA");
    int bw = badge[0] ? (int)strlen(badge) * 6 : 0;
    const int TF = 34;                                   // time field width (fits "10:00")
    int tf_r = badge[0] ? (226 - bw - 6) : 226;          // time field right edge
    int tf_l = tf_r - TF;

    if (full) {
        d.fillRect(0, y, 240, STRIP_H + 2, BG);
        d.fillRoundRect(8, y + 1, 224, STRIP_H - 2, 9, SURF);   // rounded pill
        d.fillCircle(22, mid, 8, c);                            // play/pause core
        if (paused) icon_pause(19, mid - 4, 8, INK);
        else        icon_play(20, mid - 4, 8, INK);
        d.setTextSize(1);
        if (badge[0]) { d.setTextColor(ACC, SURF); d.setCursor(226 - bw, y + 4); d.print(badge); }
        const char *bn = strrchr(st->playpath,'/') ? strrchr(st->playpath,'/') + 1 : st->playpath;
        int nx = 36, navail = (tf_l - 4) - nx; if (navail < 6) navail = 6;
        int maxc = navail / 6; if (maxc < 1) maxc = 1; if (maxc > 30) maxc = 30;
        char nb[32]; snprintf(nb, sizeof nb, "%.*s", maxc, bn ? bn : "");
        d.setTextColor(FG, SURF); d.setCursor(nx, y + 4); d.print(nb);
    }

    // Per-second region 1: elapsed time, right-aligned in its fixed field cleared to the pill.
    char te[8]; fmt_time(te, sizeof te, nucleo_audio_elapsed());
    int tw = (int)strlen(te) * 6;
    d.setTextSize(1);
    d.fillRect(tf_l, y + 3, TF, 9, SURF);
    d.setTextColor(c, SURF); d.setCursor(tf_r - tw, y + 4); d.print(te);

    // Per-second region 2: slim progress line along the bottom of the pill.
    int pct = nucleo_audio_progress(); if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    const int px = 34, pw = 192, py = y + STRIP_H - 4;
    d.fillRect(px, py, pw, 2, BG);
    d.fillRect(px, py, pw * pct / 100, 2, c);
}

// Persistent segmented tab bar — only the currently-available tabs are shown.
static void draw_tabbar(const int *avail, int na, int active, bool hdr)
{
    static const char *T[4] = { "PLAY", "AUDIO", "QUEUE", "FIND" };
    d.fillRect(0, 0, 240, 24, BG);
    int seg = 240 / (na < 1 ? 1 : na);
    for (int i = 0; i < na; i++) {
        int id = avail[i], x = i * seg, tw = (int)strlen(T[id]) * 6;
        if (id == active) {
            d.fillRoundRect(x + 3, 3, seg - 6, 17, 8, ACC);
            d.setTextSize(1); d.setTextColor(INK, ACC);
            d.setCursor(x + (seg - tw) / 2, 8); d.print(T[id]);
        } else {
            d.setTextSize(1); d.setTextColor(hdr ? MUTED : DIM, BG);
            d.setCursor(x + (seg - tw) / 2, 8); d.print(T[id]);
        }
    }
    d.drawFastHLine(0, 23, 240, LINE);
}

// Row value kinds.
enum { SV_TEXT = 0, SV_TOGGLE, SV_SLIDER, SV_ACTION };

static void draw_set_row_fs(int y, bool focus, const char *label, const char *val,
                            int kind, bool on, bool large, int vol)
{
    // Focus = accent-filled pill with INK text, the one selection look the whole OS uses (kit §3).
    // The old private dark tint + FG text was a fourth divergent capsule style.
    int h = large ? 50 : 32;
    d.fillRoundRect(4, y, 232, h - 2, 9, focus ? CAP : BG);

    d.setTextSize(2); d.setTextColor(focus ? INK : MUTED, focus ? CAP : BG);
    d.setCursor(16, y + (h - 16) / 2 - 1); d.print(label);

    if (kind == SV_SLIDER) {
        bool edit = focus && s_set_edit;
        int sw = large ? 96 : 64, sh = 12, bx = 230 - sw, vy = y + (h - sh) / 2;
        d.fillRoundRect(bx, vy, sw, sh, sh / 2, SURF);
        int onw = vol * sw / 100; if (onw < 0) onw = 0; if (onw > sw) onw = sw;
        if (onw > 0) d.fillRoundRect(bx, vy, onw, sh, sh / 2, GRN);
        int kx = bx + onw; if (kx < bx + 6) kx = bx + 6; if (kx > bx + sw - 6) kx = bx + sw - 6;
        d.fillCircle(kx, vy + sh / 2, edit ? sh / 2 + 2 : sh / 2 + 1, FG);
        if (edit) d.drawRoundRect(bx - 2, vy - 2, sw + 4, sh + 4, (sh + 4) / 2, ACC);   // adjust-mode outline
        return;
    }
    if (kind == SV_TOGGLE) {
        int sw = 42, sh = 20, bx = 230 - sw, vy = y + (h - sh) / 2;
        d.fillRoundRect(bx, vy, sw, sh, sh / 2, on ? GRN : SURF);
        int kx = on ? bx + sw - sh / 2 - 1 : bx + sh / 2 + 1;
        d.fillCircle(kx, vy + sh / 2, sh / 2 - 3, on ? INK : MUTED);
        return;
    }
    // The focused row is already an accent fill, so a value must sit ON it in INK — the old code drew
    // a second chip in the same accent (invisible) or a dark chip that fought the pill.
    if (kind == SV_ACTION) {
        int bw = 28, bh = 22, bx = 230 - bw, vy = y + (h - bh) / 2;
        if (!focus) d.fillRoundRect(bx, vy, bw, bh, 6, SURF);
        int ax = bx + bw / 2 - 2, ay = vy + bh / 2;
        d.fillTriangle(ax, ay - 4, ax, ay + 4, ax + 4, ay, focus ? INK : MUTED);    // chevron
        return;
    }
    // SV_TEXT: value chip
    if (val && val[0]) {
        int vw = (int)strlen(val) * 12 + 14, vh = 22, bx = 230 - vw, vy = y + (h - vh) / 2;
        d.setTextSize(2); d.setTextColor(focus ? INK : MUTED, focus ? CAP : BG);
        d.setCursor(bx + 7, vy + 3); d.print(val);
    }
}

static const char *repeat_str(void) {
    if (s_repeat == 0) return "Off";
    if (s_repeat == 1) return "One";
    return "All";
}

static void draw_settings_full(int ch)
{
    d.fillRect(0, 0, 240, ch, BG);
    bool hdr_mode = (s_set_row == -1);
    int avail[4], na = settings_avail(avail);
    if (tab_index(avail, na, s_set_tab) == 0 && avail[0] != s_set_tab) s_set_tab = avail[0];  // clamp to a live tab
    draw_tabbar(avail, na, s_set_tab, hdr_mode);

    struct SetItem { const char *label; char val[16]; int kind; bool on; int vol; };
    SetItem it[6] = {};
    int n = 0;

    if (s_set_tab == 0) {
        n = 3;
        it[0].label = "Shuffle";  it[0].kind = SV_TOGGLE; it[0].on = s_shuffle;
        it[1].label = "Repeat";   it[1].kind = SV_TEXT;   snprintf(it[1].val, 16, "%s", repeat_str());
        it[2].label = "Autoplay"; it[2].kind = SV_TOGGLE; it[2].on = s_autoplay;
    } else if (s_set_tab == 1) {
        n = 3;
        it[0].label = "Volume";    it[0].kind = SV_SLIDER; it[0].vol = nucleo_audio_volume();
        it[1].label = "Start Vol"; it[1].kind = SV_TEXT;   snprintf(it[1].val, 16, s_vol_default ? "%d%%" : "Last", s_vol_default);
        // Device-wide mute, exposed here because it is the one setting that can silence Music while
        // everything else looks correct: it lives in the audio engine (not in player.json), is set from
        // the launcher Control Center, and is restored from settings.json on every boot. Without a row
        // here the user has no way to see or clear it without leaving the app.
        it[2].label = "Mute";      it[2].kind = SV_TOGGLE; it[2].on = nucleo_audio_is_muted();
    } else if (s_set_tab == 2) {
        n = 3;
        it[0].label = "Queue";       it[0].kind = SV_TEXT;
        snprintf(it[0].val, 16, (st && st->qn) ? "%d/%d" : "empty", st ? st->qidx + 1 : 0, st ? st->qn : 0);
        it[1].label = "Reshuffle";   it[1].kind = SV_ACTION;
        it[2].label = "Clear Queue"; it[2].kind = SV_ACTION;
    } else {
        n = 6;
        it[0].label = "Find Track";  it[0].kind = SV_ACTION;
        it[1].label = "Genres";      it[1].kind = SV_ACTION;
        it[2].label = "Artists";     it[2].kind = SV_ACTION;
        it[3].label = "Favourites";  if (st && st->filter_type == 3) { it[3].kind = SV_TEXT; strcpy(it[3].val, "ON"); } else it[3].kind = SV_ACTION;
        it[4].label = "Most Played"; if (st && st->filter_type == 4) { it[4].kind = SV_TEXT; strcpy(it[4].val, "ON"); } else it[4].kind = SV_ACTION;
        it[5].label = "Clear Filter"; it[5].kind = SV_ACTION;
    }

    // Rows are clipped below the tab bar (and above the hint line).
    d.setClipRect(0, 24, 240, ch - 36);
    if (hdr_mode) {
        int y = 30;                                      // dimmed preview; DOWN to focus
        for (int i = 0; i < n && y < ch - 12; i++) {
            draw_set_row_fs(y, false, it[i].label, it[i].val, it[i].kind, it[i].on, false, it[i].vol);
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
                draw_set_row_fs(y, i == f, it[i].label, it[i].val, it[i].kind, it[i].on, i == f, it[i].vol);
        }
    }
    d.clearClipRect();

    d.setTextSize(1); d.setTextColor(DIM, BG); d.setCursor(8, ch - 10);
    bool on_slider = (!hdr_mode && s_set_tab == 1 && s_set_row == 0);   // AUDIO > Volume
    if (s_set_edit)     d.print("L/R adjust vol   ENTER done");
    else if (hdr_mode)  d.print("RIGHT tab   DOWN rows   ESC close");
    else if (on_slider) d.print("RIGHT tab   ENTER adjust vol");
    else                d.print("UP/DN row   RIGHT tab   ENTER ok");
}

// Build the music DB (genres/artists/fav/most-played index) LAZILY — never at app open. Spawning the
// indexer task at launch raced the UI canvas for the last few KB of the ~37 KB heap and rebooted the
// device (see enter()). The browser and playback don't need it; only the Find tab does, so we build it
// the first time Find is opened. Idempotent: the indexer no-ops if the JSONL already exists, and a
// failed task spawn (low RAM) is non-fatal — Find just shows empty until it can run.
static void music_db_ensure(void)
{
    static bool inited = false;
    if (inited) return;
    inited = true;
    music_db_init();
}

// Settings keys. NOTE: NK_LEFT and NK_BACK never arrive here — the framework routes them to
// the back handler (player_back). So tabs cycle with RIGHT and "back" is handled hierarchically.
static void settings_key(int key, char ch)
{
    (void)ch;

    // Volume adjust mode: RIGHT raises, LEFT lowers (LEFT arrives via player_back). UP/DN also work.
    // ENTER leaves; Esc leaves via player_back.
    if (s_set_edit) {
        // Moving the slider clears a hard mute (see the Now Playing volume keys): otherwise the value
        // climbs to 100% and the speaker still says nothing.
        if      (key == NK_RIGHT || key == NK_UP) { nucleo_audio_set_mute(false); nucleo_audio_set_volume(nucleo_audio_volume() + 5); }
        else if (key == NK_DOWN)                  { nucleo_audio_set_mute(false); nucleo_audio_set_volume(nucleo_audio_volume() - 5); }
        else if (key == NK_ENTER)                 s_set_edit = false;
        nucleo_app_request_draw(); return;
    }

    // Horizontal pager: RIGHT cycles tabs from ANYWHERE (header or a row) — no need to climb
    // back to the top. Landing on row 0 keeps you reading the new tab's content immediately.
    if (key == NK_RIGHT) {
        int a[4], na = settings_avail(a);
        s_set_tab = a[(tab_index(a, na, s_set_tab) + 1) % na];
        if (s_set_row >= 0) s_set_row = 0;
        if (s_set_tab == 3) music_db_ensure();   // entering Find -> build the index now (async) so it's ready when used
        nucleo_app_request_draw(); return;
    }

    // Tab header mode: DOWN drops into the rows.
    if (s_set_row == -1) {
        if (key == NK_DOWN) s_set_row = 0;
        nucleo_app_request_draw(); return;
    }

    int nrows = SET_ROWS[s_set_tab];
    if (key == NK_UP) {
        s_set_row = (s_set_row > 0) ? s_set_row - 1 : -1;     // row 0 -> back to the tab header
    } else if (key == NK_DOWN) {
        if (s_set_row < nrows - 1) s_set_row++;               // clamp at the last row
    } else if (key == NK_ENTER) {
        if (s_set_tab == 0) {                                 // PLAY
            if      (s_set_row == 0) { s_shuffle = !s_shuffle; rebuild_shuffle(); }
            else if (s_set_row == 1) { s_repeat = (s_repeat + 1) % 3; }
            else if (s_set_row == 2) { s_autoplay = !s_autoplay; }
            save_settings();
        } else if (s_set_tab == 1) {                          // AUDIO
            if (s_set_row == 0) { s_set_edit = true; }        // volume -> adjust mode
            else if (s_set_row == 1) {                        // start volume cycle
                static const int vols[] = {0, 50, 75, 100};
                int cur = 0; for (int i = 0; i < 4; i++) if (vols[i] == s_vol_default) { cur = i; break; }
                s_vol_default = vols[(cur + 1) % 4]; save_settings();
            }
            else if (s_set_row == 2) {                        // device-wide mute
                nucleo_audio_set_mute(!nucleo_audio_is_muted());
                nucleo_app_persist_prefs();                   // same store the Control Center writes
            }
        } else if (s_set_tab == 2) {                          // QUEUE
            if (s_set_row == 1 && st && st->qn > 0) rebuild_shuffle();
            else if (s_set_row == 2 && st) {
                nucleo_audio_stop(); st->qn = 0; st->qidx = 0; st->playpath[0] = 0;
                s_set_open = false; nucleo_app_request_draw(); return;
            }
        } else if (s_set_tab == 3) {                          // FIND
            music_db_ensure();                                // make sure the index is (being) built before we read it
            if      (s_set_row == 0) { s_typing = true; if (st) st->search_query[0] = 0; }
            else if (s_set_row == 1) { s_sub_type = 1; s_sub_count = music_db_get_unique(1, &s_sub_items); s_sub_sel = 0; s_sub_list = true; }
            else if (s_set_row == 2) { s_sub_type = 2; s_sub_count = music_db_get_unique(2, &s_sub_items); s_sub_sel = 0; s_sub_list = true; }
            else if (s_set_row == 3) { if (st) st->filter_type = 3; scan(); s_set_open = false; }
            else if (s_set_row == 4) { if (st) st->filter_type = 4; scan(); s_set_open = false; }
            else if (s_set_row == 5) { if (st) { st->filter_type = -1; st->search_query[0] = 0; } scan(); s_set_open = false; }
        }
    }
    nucleo_app_request_draw();
}

// Footer hint reflects the current mode; only re-sets when the mode actually changes.
static int s_hint_last = -1;   // reset to -1 in enter() to force a fresh set
static void update_hint(void)
{
    int hs = s_typing ? 1 : s_sub_list ? 2 : s_set_open ? 3 : 0;
    if (hs == s_hint_last) return;
    s_hint_last = hs;
    switch (hs) {
        case 1:  nucleo_app_set_hint(TR("scrivi cerca   invio trova   esc", "type search   enter find   esc")); break;
        case 2:  nucleo_app_set_hint(TR("su/giu scegli   invio conferma   esc", "up/dn pick   enter choose   esc")); break;
        case 3:  nucleo_app_set_hint(TR("</> scheda   su/giu riga   esc chiudi", "</> tab   up/dn row   esc close")); break;
        default: nucleo_app_set_hint(TR("su/giu sfoglia   invio riproduci   tab menu", "up/dn browse   enter play   tab menu"));
    }
}

static void on_tab(void)
{
    s_set_open = !s_set_open;
    s_set_tab = 0;          // PLAY is always available
    s_set_row = -1;         // start in the tab header: RIGHT switches tab, DOWN enters rows
    s_set_edit = false;
    update_hint();
    nucleo_app_request_draw();
}

// Hierarchical Back/Left: the framework routes BOTH keys here and now hands us the key code, so
// we can tell Left from Esc. Pop one level and return true to consume; return false only at the
// top so the app actually closes. In the volume slider, Left lowers the value (Right raises it).
static bool player_back(int key)
{
    if (!st) return false;
    if (s_typing) { s_typing = false; update_hint(); nucleo_app_request_draw(); return true; }
    if (s_sub_list) {
        s_sub_list = false;
        if (s_sub_items) { for (int i = 0; i < s_sub_count; i++) free(s_sub_items[i]); free(s_sub_items); s_sub_items = NULL; }
        update_hint(); nucleo_app_request_draw(); return true;
    }
    if (s_set_open) {
        if (s_set_edit) {
            if (key == NK_LEFT) nucleo_audio_set_volume(nucleo_audio_volume() - 5);  // Left = volume down
            else                s_set_edit = false;                                  // Esc = done adjusting
            nucleo_app_request_draw(); return true;
        }
        if (s_set_row >= 0) s_set_row = -1;              // row -> tab header
        else               s_set_open = false;          // header -> close sheet
        update_hint(); nucleo_app_request_draw(); return true;
    }
    if (st->filter_type >= 0) {                          // a filter/search is active -> clear it
        st->filter_type = -1; st->search_query[0] = 0; scan();
        nucleo_app_request_draw(); return true;
    }
    if (strcmp(s_path, "/") != 0) { go_up(); nucleo_app_request_draw(); return true; }
    return false;                                        // root, no filter -> let the framework close us
}

static const char *breadcrumb(void)
{
    static char b[26];
    if (!strcmp(s_path, "/")) { b[0]=0; return b; }
    snprintf(b, sizeof b, "%s", s_path+1);
    int l = strlen(b); if (l && b[l-1]=='/') b[--l]=0;
    if (l > 24) memmove(b, b+(l-24), 25);
    return b;
}

// Full-screen search-input sheet (covers the framework chrome).
static void draw_typing(void)
{
    d.fillScreen(BG);
    d.fillRect(0, 0, 240, 22, SURF);
    d.setTextSize(1); d.setTextColor(ACC, SURF); d.setCursor(10, 7); d.print("SEARCH");
    d.setTextColor(DIM, SURF); d.setCursor(204, 7); d.print("ESC");

    const char *sq = st ? st->search_query : "";
    d.fillRoundRect(10, 32, 220, 30, 7, SURF);
    d.drawRoundRect(10, 32, 220, 30, 7, ACC);
    char vis[20]; snprintf(vis, sizeof vis, "%.16s", sq);
    d.setTextSize(2); d.setTextColor(FG, SURF); d.setCursor(18, 40);
    if (vis[0]) d.print(vis);
    int cx = 18 + (int)strlen(vis) * 12;
    if (cx < 222) d.fillRect(cx, 40, 9, 16, ACC);   // caret block

    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(10, 74);
    d.print("Type to search \xb7 ENTER confirm");
    if (sq[0]) {
        d.setTextColor(DIM, BG); d.setCursor(10, 90);
        char h[40]; snprintf(h, sizeof h, "Find: \"%.26s\"", sq);
        d.print(h);
    }
}

// Full-screen genre / artist picker sheet.
static void draw_sublist(void)
{
    d.fillScreen(BG);
    d.fillRect(0, 0, 240, 22, SURF);
    d.setTextSize(1); d.setTextColor(ACC, SURF); d.setCursor(10, 7);
    d.print(s_sub_type == 1 ? "GENRES" : "ARTISTS");
    d.setTextColor(DIM, SURF); d.setCursor(204, 7); d.print("ESC");

    if (s_sub_count == 0) {
        d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(12, 42); d.print("Nothing here yet");
        return;
    }
    const int vis = 5, rh = 21, y0 = 28;
    int scroll = s_sub_sel - vis / 2;
    if (scroll > s_sub_count - vis) scroll = s_sub_count - vis;
    if (scroll < 0) scroll = 0;
    for (int i = 0; i < vis && scroll + i < s_sub_count; i++) {
        int idx = scroll + i, y = y0 + i * rh;
        bool focus = (idx == s_sub_sel);
        if (focus) {
            d.fillRoundRect(8, y, 224, rh - 1, 6, ACC);
            d.setTextSize(2); d.setTextColor(INK, ACC);
            char lbl[20]; snprintf(lbl, sizeof lbl, "%.17s", s_sub_items[idx]);
            d.setCursor(16, y + 2); d.print(lbl);
        } else {
            d.fillCircle(15, y + 10, 2, MUTED);
            d.setTextSize(1); d.setTextColor(FG, BG);
            char lbl[36]; snprintf(lbl, sizeof lbl, "%.32s", s_sub_items[idx]);
            d.setCursor(24, y + 6); d.print(lbl);
        }
    }
    if (s_sub_count > vis) {
        int track = vis * rh - 6, kh = track * vis / s_sub_count; if (kh < 12) kh = 12;
        int ky = y0 + 3 + (track - kh) * s_sub_sel / (s_sub_count - 1);
        d.fillRoundRect(235, y0 + 3, 3, track, 1, LINE);
        d.fillRoundRect(235, ky, 3, kh, 1, ACC);
    }
}

static void draw(void)
{
    if (!st) return;
    s_groove_y = -1;   // re-established below only if the focused row is the playing track
    if (s_typing)   { draw_typing();  return; }
    if (s_sub_list) { draw_sublist(); return; }

    int ch = nucleo_app_content_height();
    int top = nucleo_app_content_top();
    int cb  = top + ch;
    if (s_set_open) { draw_settings_full(ch); return; }

    bool active = nucleo_audio_is_playing() || nucleo_audio_is_paused();

    // Context title: filter name, else current folder, else "Music".
    char ctx[24];
    switch (st->filter_type) {
        case 0:  snprintf(ctx, sizeof ctx, "Search");      break;
        case 1:  snprintf(ctx, sizeof ctx, "Genre");       break;
        case 2:  snprintf(ctx, sizeof ctx, "Artist");      break;
        case 3:  snprintf(ctx, sizeof ctx, "Favourites");  break;
        case 4:  snprintf(ctx, sizeof ctx, "Top Played");  break;
        default: if (strcmp(s_path, "/")) snprintf(ctx, sizeof ctx, "%s", breadcrumb());
                 else                     snprintf(ctx, sizeof ctx, "Music");
    }
    draw_header(top, ctx, s_n);

    int lt = top + HEAD_H;
    int lh = ch - HEAD_H - (active ? STRIP_H : 0);

    if (s_n == 0) {
        d.fillRect(0, lt, 240, lh, BG);
        d.setTextSize(1); d.setTextColor(DIM, BG); d.setCursor(12, lt + 14);
        d.print(strcmp(s_path, "/") ? "(empty)" : "No music in /data/Music");
    } else {
        char base[208]; snprintf(base, sizeof base, "%s%s", MUSIC_DIR, s_path);
        draw_list(lt, lh, base);
    }

    if (active) draw_mini_strip(cb - STRIP_H, true);
}

static const char *row_label(int i, void *) { return st->e[i].name; }

static void build_queue(int sel_entry_idx)
{
    if (!st) return;
    char base[208]; snprintf(base, sizeof base, "%s%s", MUSIC_DIR, s_path);
    snprintf(st->qdir, sizeof st->qdir, "%s", base);
    st->qn = 0; int qi = 0;
    for (int i = 0; i < s_n && st->qn < MAXQ; i++) {
        if (st->e[i].dir) continue;
        if (i == sel_entry_idx) qi = st->qn;
        char *dst = st->q[st->qn++];
        const char *src = st->e[i].name;
        size_t k = 0; for (; k < 55 && src[k]; k++) dst[k] = src[k]; dst[k] = 0;
    }
    st->qidx = qi;
    rebuild_shuffle();
    if (s_shuffle) st->qidx = 0;
}

// Play a file chosen in Files ("open with"). It can live in ANY folder (not only under MUSIC_DIR),
// so we build the queue straight from the file's OWN directory: every audio sibling joins the queue
// (so next/prev still work), positioned on the clicked track. Falls back to a single-track queue.
static void play_external(const char *abs)
{
    if (!st || !abs || !abs[0]) return;
    const char *slash = strrchr(abs, '/');
    if (!slash) return;
    int dlen = (int)(slash - abs) + 1;                       // keep the trailing '/'
    if (dlen <= 0 || dlen >= (int)sizeof st->qdir) return;
    memcpy(st->qdir, abs, dlen); st->qdir[dlen] = 0;
    const char *fname = slash + 1;
    st->qn = 0; int qi = 0;
    DIR *dir = opendir(st->qdir);
    if (dir) {
        struct dirent *e;
        while ((e = readdir(dir)) != NULL && st->qn < MAXQ) {
            if (e->d_name[0] == '.' || !is_audio(e->d_name)) continue;
            if (!strcmp(e->d_name, fname)) qi = st->qn;
            char *dst = st->q[st->qn++];
            size_t k = 0; for (; k < 55 && e->d_name[k]; k++) dst[k] = e->d_name[k]; dst[k] = 0;
        }
        closedir(dir);
    }
    if (st->qn == 0) {                                       // dir unreadable: queue just the clicked file
        char *dst = st->q[0]; size_t k = 0; for (; k < 55 && fname[k]; k++) dst[k] = fname[k]; dst[k] = 0;
        st->qn = 1; qi = 0;
    }
    st->qidx = qi;
    rebuild_shuffle();
    if (s_shuffle) st->qidx = 0;
    play_q(st->qidx);
}

static void on_key(int key, char ch)
{
    if (!st) return;
    if (s_typing) {                                      // cancel = Back/Left (handled by player_back)
        if (key == NK_DEL || ch == 8 || ch == 127) {
            int len = strlen(st->search_query); if (len > 0) st->search_query[len-1] = 0;
        } else if (key == NK_ENTER) {
            s_typing = false;
            if (st->search_query[0]) { st->filter_type = 0; scan(); s_set_open = false; }
            update_hint();
        } else if (ch >= 32 && ch <= 126) {
            int len = strlen(st->search_query);
            if (len < 63) { st->search_query[len] = ch; st->search_query[len+1] = 0; }
        }
        nucleo_app_request_draw(); return;
    }
    if (s_sub_list) {                                    // cancel = Back/Left (handled by player_back)
        if (key == NK_UP)        { s_sub_sel--; if (s_sub_sel < 0) s_sub_sel = s_sub_count - 1; }
        else if (key == NK_DOWN) { s_sub_sel++; if (s_sub_sel >= s_sub_count) s_sub_sel = 0; }
        else if ((key == NK_ENTER || key == NK_RIGHT) && s_sub_count > 0) {
            st->filter_type = s_sub_type;
            snprintf(st->search_query, sizeof st->search_query, "%s", s_sub_items[s_sub_sel]);
            s_sub_list = false;
            if (s_sub_items) { for (int i = 0; i < s_sub_count; i++) free(s_sub_items[i]); free(s_sub_items); s_sub_items = NULL; }
            scan(); s_set_open = false; update_hint();
        }
        nucleo_app_request_draw(); return;
    }
    if (s_set_open) { settings_key(key, ch); return; }
    if (ch == 'f' && (nucleo_audio_is_playing() || nucleo_audio_is_paused())) {
        const char *bn = strrchr(st->playpath,'/'); bn = bn ? bn+1 : st->playpath;
        music_db_set_fav(bn, !music_db_is_fav(bn));
        nucleo_app_request_draw(); return;
    }
    if (app_ui_list_key(key, ch, &s_sel, s_n, row_label, nullptr)) {
    } else if (key == NK_DEL) { go_up(); }
    else if (key == NK_ENTER && s_sel < s_n) {
        MEntry *r = &st->e[s_sel];
        if (r->dir) {
            descend(r->name);
            if (s_autoplay && s_n > 0) {
                for (int i = 0; i < s_n; i++) {
                    if (!st->e[i].dir) { build_queue(i); play_q(0); break; }
                }
            }
        } else {
            build_queue(s_sel); now_playing();
        }
    } else if (key == NK_RIGHT && st->qn > 0 && (nucleo_audio_is_playing() || nucleo_audio_is_paused())) {
        now_playing();   // jump to the full-screen Now Playing for the current track
    } else if (ch == ' ') { nucleo_audio_toggle_pause(); }
    else if (ch == ']' && st->qn > 0) { int p = next_pos(); if (p >= 0) play_q(p); }
    else if (ch == '[' && st->qn > 0) { play_q(prev_pos()); }
    else { return; }
    nucleo_app_request_draw();
}

static void player_update_logic(bool *back_flag)
{
    if (!st || !st->playpath[0]) return;

    // Auto-advance: wait for real audio output before tracking end-of-track.
    static bool started = false;
    if (nucleo_audio_is_playing() && nucleo_audio_elapsed_ms() > 200) started = true;

    if (started && !s_play_counted) {
        uint32_t dur = nucleo_audio_duration_ms();
        if (dur > 0 && nucleo_audio_elapsed_ms() > dur / 2) {
            const char *bn = strrchr(st->playpath, '/'); bn = bn ? bn+1 : st->playpath;
            music_db_add_play(bn);
            s_play_counted = true;
        }
    }

    if (started && !nucleo_audio_is_playing() && !nucleo_audio_is_paused()) {
        // Track ended -> advance, or leave if the queue is done.
        int p = next_pos();
        if (p >= 0) { play_q(p); started = false; s_play_counted = false; }
        else { st->playpath[0]=0; if (back_flag) *back_flag = true; }
    }
}

static void tick(void)
{
    update_hint();
    player_update_logic(nullptr);
    // NB: the dedicated RAM window is NOT released here. It's held for the WHOLE session (entered in
    // enter(), released in leave()): the browser itself needs the reclaimed RAM to exist on this ~5 KB
    // heap, not just the decoder. Releasing between tracks would tear httpd back up mid-session.

    bool active = nucleo_audio_is_playing() || nucleo_audio_is_paused();
    int s = active ? (nucleo_audio_is_paused() ? 1 : 2) : 0;

    // Structural change (started / paused / stopped): the strip appears, disappears or swaps
    // its play/pause core and the list height changes -> one full, buffered repaint.
    if (s != s_strip_struct) {
        s_strip_struct = s;
        s_strip_el  = (int)nucleo_audio_elapsed();
        s_strip_pct = nucleo_audio_progress();
        nucleo_app_request_draw();
        return;
    }

    // Progress-only tick while the browser strip is on screen: refresh ONLY the strip's time +
    // progress (and the playing row's groove) in place — never re-render the list/header. This is
    // what kills the old "whole list redraws every second while a song plays" flicker: during
    // playback the shared back-buffer is on loan to the MP3 decoder, so a full repaint would draw
    // the entire list direct-to-panel (clear-then-draw = flicker). Here we touch a few pixels.
    bool strip_live = active && !s_set_open && !s_typing && !s_sub_list;
    if (!strip_live) return;
    int el = (int)nucleo_audio_elapsed(), pct = nucleo_audio_progress();
    if (el == s_strip_el && pct == s_strip_pct) return;
    s_strip_el = el; s_strip_pct = pct;
    int top = nucleo_app_content_top();
    int cb  = top + nucleo_app_content_height();
    draw_mini_strip(cb - STRIP_H, false);   // in-place: elapsed time + progress only
    draw_row_groove();                       // no-op unless the playing row is focused
}

static void enter(void)
{
    // The RAM window is DECLARATIVE now (NX_NET_APP | NX_SOLO in the app_def): the framework has already
    // rebooted us into a fresh heap with httpd/L1/mDNS/voice absent by the time this runs, so the state
    // below and the decoder both allocate against an unfragmented arena. No background indexer at open
    // either — the DB is built lazily on first Find use (music_db_ensure). See the note above play_q.
    if (!st) st = (PState *)calloc(1, sizeof(PState));
    if (!st) {                                  // OOM even after the reclaim: bail (don't deref NULL) and restore services
        size_t freeb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t blk   = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        ESP_LOGE("music", "enter OOM: PState=%u free=%u largest=%u",
                 (unsigned)sizeof(PState), (unsigned)freeb, (unsigned)blk);
        char h[64]; snprintf(h, sizeof h, TR("RAM bassa: libero %uk blocco %uk", "Low RAM: free %uk block %uk"),
                             (unsigned)(freeb / 1024), (unsigned)(blk / 1024));
        nucleo_app_set_hint(h);
        return;                                 // the framework owns the window; leave() releases it
    }
    memset(st, 0, sizeof(*st));                 // calloc zeroes a fresh block; this also re-zeroes a reused one
    load_settings();
    nucleo_app_set_tab_handler(on_tab);
    nucleo_app_set_back_handler(player_back);   // hierarchical ESC/Left (else the framework closes us)
    st->filter_type = -1;
    strcpy(s_path, "/"); scan();
    // "Has music" = any audio anywhere under Music (a track at root, or a folder with songs).
    s_has_music = false;
    for (int i = 0; i < s_n; i++) if (!st->e[i].dir || st->e[i].count > 0) { s_has_music = true; break; }
    s_set_open = false; s_set_edit = false;
    s_strip_struct = -1; s_strip_el = -1; s_strip_pct = -1; s_groove_y = -1;
    s_hint_last = -1; update_hint();    // show the browser hint in the footer
    const char *of = nucleo_app_take_open_file();
    if (of && of[0]) { play_external(of); now_playing(); }   // opened from Files -> play that track + jump to Now Playing
}
static void leave(void)
{
    nucleo_audio_stop();
    if (nucleo_exclusive_active()) nucleo_exclusive_exit();  // safety net like Video/Radio/SSH: never leave services suspended
    np_mq_free();                                            // never leak the now-playing marquee sprite
    // Free the Genres/Artists sub-list if the app is closed straight from that sheet (the back handler
    // frees it on a normal pop, but the framework can close us from anywhere — don't leak the strdups).
    if (s_sub_items) {
        for (int i = 0; i < s_sub_count; i++) free(s_sub_items[i]);
        free(s_sub_items); s_sub_items = NULL; s_sub_count = 0;
    }
    s_sub_list = false; s_typing = false; s_set_open = false; // reset modal flags so the next open starts clean
    if (st) { free(st); st = nullptr; }
    s_n = 0;
}

// ---- full-screen Now Playing controller -------------------------------------
static void seek_rel(int delta_s)
{
    uint32_t dur = nucleo_audio_duration_ms(); if (!dur) return;
    int64_t nw = (int64_t)nucleo_audio_elapsed_ms() + (int64_t)delta_s * 1000;
    if (nw < 0) nw = 0;
    if (nw > (int64_t)dur) nw = dur;
    nucleo_audio_seek((uint32_t)nw);
}

// ---- Now Playing state (title for marquee, metadata line, scroll offset) ----
static char s_np_title[64];
static char s_np_meta[48];
static int  s_mq_off = 0;

// Dedicated marquee sprite for the scrolling title (ANTI-FLICKER technique 3). Composited off
// screen and blitted in one pushSprite, so the panel never sees the per-frame clear (= no flicker,
// no text ghosting). ~3.6 KB at 8bpp; acquired up front in now_playing() — after the shared
// back-buffer is released but BEFORE the MP3 decoder grabs its ~17 KB — so it can't fragment the
// decoder's contiguous block. Falls back to a (clipped) direct draw if the sprite won't fit.
#define MQ_W 210
#define MQ_H 16
static M5Canvas *s_mq_cv     = nullptr;
static bool      s_mq_failed = false;   // createSprite failed this session -> direct fallback, no retry

// ── Now-Playing layout (240x135, fullscreen) — the "Tape Deck" ────────────────────────────────────
// The panel is a wide, short letterbox (240x135 ≈ the classic Winamp main window to within 2%), so the
// old circular ring — a square on a letterbox — wasted a third of the width. This layout is horizontal
// bands only: a full-BLEED spectrum visualiser is the hero, and everything continuous (progress, seek)
// runs the full width, the one axis this screen is rich in. Design language follows the loved Cardputer
// players (Winamp-style, MicroGroove) and car-HMI glanceability: one shape, read in ~1.5 s.
#define NP_MARGIN  8                                 // left/right gutter for text rows
#define NP_RIGHT   (240 - NP_MARGIN)                 // 232
#define VIZ_Y      33
#define VIZ_H      55
#define VIZ_BASE   (VIZ_Y + VIZ_H)                   // 88: bar baseline
#define VIZ_BARS   30                                // display bars: x = 1 + i*8, w=6 -> ends at 239 (full bleed)
#define VIZ_SRC    NUCLEO_AUDIO_BANDS_N              // real bands (6), interpolated up to VIZ_BARS
static uint8_t s_viz_cap[VIZ_BARS];                  // peak-hold caps (slow fall), in band units 0..255
static uint8_t s_viz_prev[VIZ_BARS];                 // last drawn bar height (px), for delta-only repaint
static uint8_t s_viz_capp[VIZ_BARS];                 // last drawn cap row (px) — kills the falling-cap ghost trail
static const int SEEK_Y = 93;                        // seek groove
static const int TIME_Y = 100;                       // elapsed / duration readouts
static const int BADGE_Y = 110;                      // volume + mode badges row

// Linear interpolation between two RGB565 colours (t = 0..255). Cheap per-channel lerp — used for the
// visualiser's height gradient so a bar shades bass-green -> peak-red instead of flat blocks.
static inline uint16_t rgb565_lerp(uint16_t a, uint16_t b, int t)
{
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    int r = ar + (br - ar) * t / 255, g = ag + (bg - ag) * t / 255, bl = ab + (bb - ab) * t / 255;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

static bool np_mq_acquire(void)
{
    if (s_mq_cv) return true;
    if (s_mq_failed) return false;
    M5Canvas *cv = new (std::nothrow) M5Canvas(nucleo_app_gfx());
    if (!cv) { s_mq_failed = true; return false; }
    cv->setColorDepth(8);
    cv->setPsram(false);   // PSRAM-less board: skip the doomed SPIRAM probe (the misleading "oom 3204 B caps=0x404") → DMA-internal directly
    if (!cv->createSprite(MQ_W, MQ_H)) { delete cv; s_mq_failed = true; return false; }
    cv->setTextWrap(false);   // CRUCIAL: M5GFX text-wrap defaults to ON, so a long title would wrap at the
                              // 200px sprite edge onto a 2nd (clipped) line instead of scrolling past it —
                              // only the first ~16 chars would ever move. The marquee must NOT wrap.
    s_mq_cv = cv;
    return true;
}
static void np_mq_free(void)
{
    if (s_mq_cv) { s_mq_cv->deleteSprite(); delete s_mq_cv; s_mq_cv = nullptr; }
}

// Load the playing track's display title + "Artist - Genre" line from the music DB.
static void np_load_meta(void)
{
    s_np_meta[0] = 0;
    const char *bn = strrchr(st->playpath, '/'); bn = bn ? bn + 1 : st->playpath;
    snprintf(s_np_title, sizeof s_np_title, "%s", bn);
    char *dot = strrchr(s_np_title, '.'); if (dot && dot != s_np_title) *dot = 0;  // drop extension

    struct TrackMeta *res = NULL;
    if (music_db_search(bn, 0, &res) > 0 && res) {
        if (res[0].title[0]) snprintf(s_np_title, sizeof s_np_title, "%s", res[0].title);
        const char *a = res[0].artist[0] ? res[0].artist : NULL;
        const char *g = res[0].genre[0]  ? res[0].genre  : NULL;
        if (a && g) snprintf(s_np_meta, sizeof s_np_meta, "%s \xb7 %s", a, g);
        else if (a) snprintf(s_np_meta, sizeof s_np_meta, "%s", a);
        else if (g) snprintf(s_np_meta, sizeof s_np_meta, "%s", g);
        free(res);
    }
}

// Colour for the current transport state — the whole screen keys off this one value so play/pause/idle
// read at a glance: mint when playing, amber when paused (desaturation cue), grey when stopped.
static inline uint16_t np_state_col(bool playing, bool paused)
{ return paused ? AMB : (playing ? GRN : MUTED); }

// Static title-band chrome: favourite heart only. The accent rule underneath is now the coarse
// progress line (np_progline), so it lives on the pct-change path, not here.
static void np_header_chrome(bool fav)
{
    d.fillRect(0, 2, 240, 19, BG);
    if (fav) icon_heart(NP_RIGHT - 8, 6, RED);
}

// Full-width progress underline at y22 — doubles as a coarse position bar you read without numbers
// (the fine one is the seek groove below the visualiser). Track in LINE, elapsed in the state colour.
static void np_progline(uint16_t col)
{
    int pct = nucleo_audio_progress(); if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    const int x = NP_MARGIN, w = NP_RIGHT - NP_MARGIN;   // 8..232 = 224
    d.drawFastHLine(x, 22, w, LINE);
    d.fillRect(x, 22, w * pct / 100, 2, col);
}

// Scrolling title text (size-2 accent). Short titles print once, opaque. Long titles render into
// the dedicated off-screen sprite and blit in ONE pushSprite (technique 3) — the panel never sees
// a clear, so the scroll is flicker-free and leaves no ghost. If the sprite can't be allocated we
// fall back to a clipped direct draw (the old behaviour: correct, just not as smooth).
static void np_marquee(void)
{
    const int avail = MQ_W;
    int tw = (int)strlen(s_np_title) * 12;
    if (tw <= avail) {                                   // fits -> static, no scroll
        d.setClipRect(8, 4, avail, MQ_H);
        d.setTextSize(2); d.setTextColor(ACC, BG);
        d.setCursor(8, 4); d.print(s_np_title);
        d.clearClipRect();
        return;
    }
    int span = tw + 24;                                  // gap between the wrap copies
    if (np_mq_acquire()) {
        M5Canvas *cv = s_mq_cv;
        cv->fillSprite(BG);
        cv->setTextSize(2); cv->setTextColor(ACC, BG);
        int x = -s_mq_off;                               // sprite-local; (0,0) maps to screen (8,4)
        cv->setCursor(x, 0);        cv->print(s_np_title);
        cv->setCursor(x + span, 0); cv->print(s_np_title);   // wrap copy for a seamless loop
        // Own the blit's clip: pushSprite -> LGFXBase::pushImage clips to d's CURRENT clip rect, and d
        // is a shared surface other paths leave a narrow clip on. Without this, a leaked clip shrinks
        // the blit so a step overwrites only PART of the band, leaving the previous offset's text under
        // it -> two superimposed copies. Clip to the exact footprint so every step fully repaints the
        // band (still ONE opaque blit — anti-flicker preserved), mirroring the direct branches below.
        d.setClipRect(8, 4, avail, MQ_H);
        cv->pushSprite(8, 4);
        d.clearClipRect();
    } else {                                             // no sprite RAM -> direct (may flicker)
        d.setClipRect(8, 4, avail, MQ_H);
        d.fillRect(8, 4, avail, MQ_H, BG);
        d.setTextSize(2); d.setTextColor(ACC, BG);
        d.setTextWrap(false);                            // same wrap hazard on the shared target: off for the
        int x = 8 - s_mq_off;                            // scroll, restored to the framework default below
        d.setCursor(x, 4);        d.print(s_np_title);
        d.setCursor(x + span, 4); d.print(s_np_title);
        d.setTextWrap(true);                             // M5GFX default is wrap=ON — restore so other UI text wraps as before
        d.clearClipRect();
    }
}

// Full-bleed spectrum visualiser — the hero. 30 bars across the whole 240px, fed by the REAL output
// bands (nucleo_audio_bands: what the speaker actually plays, post volume/mute), interpolated 6->30 for
// a smooth silhouette. Winamp-style peak-hold caps make a ~20fps analyser look fluid.
//
// GHOSTING: a column is only touched when its bar height OR its cap position changed, but when it IS
// touched the WHOLE strip above the bar is cleared to BG first. The earlier version cleared only the
// shrunk slice of the bar, which left the falling peak-cap hairline (it sits ABOVE the bar) as a trail
// of stale white lines. Clearing the full column above the bar erases the old cap in the same stroke,
// and tracking the cap's pixel row (s_viz_capp) makes the "cap just reached the floor" frame repaint
// once more instead of leaving its last hairline behind. A full-height clear of a 6px column is a few
// hundred pixels — negligible, and only for columns that actually changed.
//
// Colour gradient runs green (quiet/low) -> amber -> red (loud/peak) by BAR HEIGHT, which reads as
// energy. force=true repaints every bar (first draw / after a full-screen clear).
static void np_viz(bool force)
{
    uint8_t band[VIZ_SRC];
    int nb = nucleo_audio_bands(band, VIZ_SRC);
    if (nb < VIZ_SRC) for (int b = nb; b < VIZ_SRC; b++) band[b] = 0;

    for (int i = 0; i < VIZ_BARS; i++) {
        // interpolate this display bar from the two nearest source bands
        int fp = i * (VIZ_SRC - 1) * 256 / (VIZ_BARS - 1);   // fixed-point source position <<8
        int si = fp >> 8, fr = fp & 0xFF;
        int v = band[si]; if (si + 1 < VIZ_SRC) v += (band[si + 1] - band[si]) * fr / 256;
        if (v < 0) v = 0;
        if (v > 255) v = 255;

        int h = v * VIZ_H / 255;                             // bar height in px
        if (s_viz_cap[i] < v) s_viz_cap[i] = v;              // peak-hold: jump up instantly
        else if (s_viz_cap[i] > 4) s_viz_cap[i] -= 4;        // ...fall slowly
        else s_viz_cap[i] = 0;
        int caph = s_viz_cap[i] * VIZ_H / 255;               // cap position in px above the baseline

        // Redraw this column only when its silhouette actually changed (bar height OR cap row). The
        // cap-row term is what makes a falling cap animate AND get its final frame cleared.
        if (!force && h == s_viz_prev[i] && caph == s_viz_capp[i]) continue;
        s_viz_prev[i] = (uint8_t)h;
        s_viz_capp[i] = (uint8_t)caph;

        int x = 1 + i * 8;                                   // full bleed: 1..238
        int top = VIZ_BASE - h;
        // Full-column clear above the bar — erases the old bar top AND the old cap hairline in one go.
        if (top > VIZ_Y) d.fillRect(x, VIZ_Y, 6, top - VIZ_Y, BG);
        // the bar itself: green -> amber -> red by height
        if (h > 0) {
            uint16_t c = v < 128 ? rgb565_lerp(GRN, AMB, v * 2)
                                 : rgb565_lerp(AMB, RED, (v - 128) * 2);
            d.fillRect(x, top, 6, h, c);
        }
        // peak-hold cap: a bright hairline, only while it floats clear above the bar
        if (caph > h + 1) d.drawFastHLine(x, VIZ_BASE - caph, 6, FG);
    }
}

// Meta row (y24): artist/genre at the left, TRACK n/N right-aligned. Static until the track changes.
static void np_meta(void)
{
    d.fillRect(0, 24, 240, 8, BG);
    char pos[16]; snprintf(pos, sizeof pos, "%d/%d", st->qidx + 1, st->qn);
    int pw = (int)strlen(pos) * 6;
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(NP_RIGHT - pw, 24); d.print(pos);
    if (s_np_meta[0]) {
        int maxc = (NP_RIGHT - pw - 6 - NP_MARGIN) / 6; if (maxc > 30) maxc = 30; if (maxc < 1) maxc = 1;
        char m[32]; snprintf(m, sizeof m, "%.*s", maxc, s_np_meta);
        d.setTextColor(DIM, BG); d.setCursor(NP_MARGIN, 24); d.print(m);
    }
}

// Full-width seek groove (y93) + knob — the FINE position bar. 224px wide gives ~1px/second on a
// 4-minute track, real resolution the eye can use. Fill + knob in the state colour.
static void np_seek(uint16_t col)
{
    int pct = nucleo_audio_progress(); if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    const int x = NP_MARGIN, w = NP_RIGHT - NP_MARGIN, h = 5, cy = SEEK_Y + h / 2;
    d.fillRect(x, SEEK_Y - 3, w + 1, 10, BG);                 // clear groove + knob only (stops above the times row)
    d.fillRoundRect(x, SEEK_Y, w, h, h / 2, SURF);
    int fw = w * pct / 100;
    if (fw > 0) d.fillRoundRect(x, SEEK_Y, fw, h, h / 2, col);
    int kx = x + fw; if (kx < x + 3) kx = x + 3; if (kx > x + w - 3) kx = x + w - 3;
    d.fillCircle(kx, cy, 4, FG); d.fillCircle(kx, cy, 2, col); // Winamp/head-unit knob
}

// Elapsed (left) / duration (right) readouts at y100 — the row that ticks each second.
static void np_times(void)
{
    d.fillRect(0, TIME_Y, 240, 9, BG);
    uint32_t el = nucleo_audio_elapsed(), du = nucleo_audio_duration_ms() / 1000;
    char te[8], td[8]; fmt_time(te, sizeof te, el); fmt_time(td, sizeof td, du);
    d.setTextSize(1); d.setTextColor(FG, BG); d.setCursor(NP_MARGIN, TIME_Y); d.print(te);
    if (du) { d.setTextColor(MUTED, BG); int dw = (int)strlen(td) * 6;
              d.setCursor(NP_RIGHT - dw, TIME_Y); d.print(td); }
}

// Bottom row (y110): a compact volume segment bar on the left, mode badges on the right. The volume
// bar reads MUTE (not a percent) when the engine's hard mute is set — that flag survives a reboot and
// is invisible everywhere else, so a muted device would otherwise look broken.
static void np_badges(void)
{
    d.fillRect(0, BADGE_Y, 240, 9, BG);
    int vol = nucleo_audio_volume();
    bool muted = nucleo_audio_is_muted();
    // 10 segments, 6px each + 1 gap = 70px — countable at a glance (a 1px smooth step is invisible).
    const int vx = NP_MARGIN, vy = BADGE_Y + 1, seg = 10, on = muted ? 0 : (vol + 5) / 10;
    for (int i = 0; i < seg; i++)
        d.fillRect(vx + i * 7, vy, 6, 6, i < on ? (uint16_t)GRN : (uint16_t)SURF);
    d.setTextSize(1);
    d.setTextColor(muted ? RED : MUTED, BG);
    d.setCursor(vx + seg * 7 + 6, BADGE_Y);
    if (muted) d.print("MUTE"); else { char vb[6]; snprintf(vb, sizeof vb, "%d%%", vol); d.print(vb); }

    // Mode badges, right-aligned: shuffle (~), repeat one/all (R1/RA). INK on the app accent.
    int bx = NP_RIGHT;
    if (s_repeat)  { const char *rl = s_repeat == 1 ? "R1" : "RA"; bx -= 22;
                     d.fillRoundRect(bx, BADGE_Y - 1, 20, 11, 3, ACC); d.setTextColor(INK, ACC); d.setCursor(bx + 4, BADGE_Y + 1); d.print(rl); bx -= 4; }
    if (s_shuffle) { bx -= 14;
                     d.fillRoundRect(bx, BADGE_Y - 1, 12, 11, 3, ACC); d.setTextColor(INK, ACC); d.setCursor(bx + 3, BADGE_Y + 1); d.print("~"); }
}

static void np_legend(void)
{
    d.drawFastHLine(NP_MARGIN, 121, NP_RIGHT - NP_MARGIN, LINE);
    d.setTextSize(1); d.setTextColor(DIM, BG); d.setCursor(NP_MARGIN, 125);
    d.print("L/R seek  U/D vol  [ ] track  f fav");
}

static void now_playing(void)
{
    // Release the shared ~32 KB canvas, then START THE TRACK BEFORE grabbing the marquee sprite.
    // The old order acquired the 3200-byte sprite first, "so it wouldn't fragment the decoder's block" —
    // but that is backwards: taking 3200 bytes out of the just-freed region is precisely what can split
    // it below the ~24 KB contiguous run Helix needs, and the decoder's failure is SILENT while the
    // sprite's is not (s_mq_failed already falls back to direct drawing). Biggest, least-recoverable
    // allocation first; the cosmetic one takes what is left.
    nucleo_app_release_buffers();
    char target[300]; snprintf(target, sizeof target, "%s%s", st->qdir, st->q[track_at(st->qidx)]);
    if (strcmp(target, st->playpath) != 0 || !nucleo_audio_is_playing()) play_q(st->qidx);
    // nucleo_audio_play() only SPAWNS the decode task, so MP3InitDecoder has not run yet when it
    // returns — grabbing the sprite right here would still race it for the block. Wait (bounded) until
    // the decoder reports its verdict, then take what is left. ~40 ms typical, capped at 400 ms so a
    // failed start can never hang the UI.
    for (int i = 0; i < 20 && nucleo_audio_dbg_init() == 0 && nucleo_audio_is_playing(); i++)
        vTaskDelay(pdMS_TO_TICKS(20));
    s_mq_failed = false; np_mq_acquire();
    if (st->playpath[0] == 0) {  // failed to start
        np_mq_free();
        for (int i = 0; i < 8 && !nucleo_screen_acquire(); i++) vTaskDelay(pdMS_TO_TICKS(20));
        d.fillScreen(BG); nucleo_app_force_repaint(); return;   // now_playing drew direct-to-panel: force a full re-blit or the list stays black (stale band-hash)
    }

    d.fillScreen(BG);
    np_load_meta(); s_mq_off = 0;
    memset(s_viz_prev, 0, sizeof s_viz_prev); memset(s_viz_cap, 0, sizeof s_viz_cap);
    memset(s_viz_capp, 0, sizeof s_viz_capp);            // fresh visualiser (no stale caps)
    d.drawFastHLine(1, VIZ_BASE, 238, LINE);            // spectrum floor: silent bars still read as a baseline
    np_legend();
    char cur[208]; snprintf(cur, sizeof cur, "%s", st->playpath);

    bool back = false, started = false;
    int last_meta = -1, last_el = -1, last_pct = -1;
    int64_t last_mq_ms = 0;                          // wall-clock ms of the last marquee step (esp_timer)
    // Silent-playback watchdog. The engine already recorded WHY a play made no sound, but nothing
    // ever read those counters, so a silent track was indistinguishable from a broken speaker — and
    // there is no serial console on this device to check. Give the decoder a grace period, then, if
    // it still has not emitted a single PCM frame, put the engine's own verdict on the panel.
    int64_t play_t0 = esp_timer_get_time() / 1000;
    bool silent_shown = false;

    while (!back) {
        esp_task_wdt_reset();
        nucleo_key_t k = nucleo_kbd_read();
        if (k.key != NK_NONE) {
            if      (k.key==NK_BACK||k.key==NK_TAB||k.ch=='`') back = true;
            else if (k.ch=='s')                                { nucleo_audio_stop(); st->playpath[0]=0; back=true; }
            else if (k.key==NK_ENTER||k.ch==' '||k.ch=='p')    nucleo_audio_toggle_pause();
            else if (k.key==NK_LEFT)                            seek_rel(-10);
            else if (k.key==NK_RIGHT)                           seek_rel(+10);
            // Touching the volume clears a hard mute first (same rule as the video player): reaching
            // for the volume IS the user asking to hear something, and without this the bar moves
            // while the output stays silent.
            else if (k.key==NK_UP)   { nucleo_audio_set_mute(false); nucleo_audio_set_volume(nucleo_audio_volume()+10); }
            else if (k.key==NK_DOWN) { nucleo_audio_set_mute(false); nucleo_audio_set_volume(nucleo_audio_volume()-10); }
            else if (k.ch=='[') { play_q(prev_pos()); started=false; }
            else if (k.ch==']') { int p=next_pos(); if(p>=0){play_q(p);started=false;}else{st->playpath[0]=0;back=true;} }
            else if (k.ch=='f') { const char *bn=strrchr(st->playpath,'/'); bn=bn?bn+1:st->playpath;
                                  music_db_set_fav(bn, !music_db_is_fav(bn)); last_meta=-1; }
            else if (k.ch=='r') { s_repeat=(s_repeat+1)%3; save_settings(); last_meta=-1; }
            else if (k.ch>='0'&&k.ch<='9') {
                uint32_t dur=nucleo_audio_duration_ms();
                if (dur) nucleo_audio_seek((uint32_t)((int64_t)(k.ch-'0')*dur/10));
            }
        }
        if (!started && nucleo_audio_is_playing() && nucleo_audio_elapsed_ms() > 200) started = true;
        player_update_logic(&back);

        // Track changed (auto-advance or skip) -> refresh metadata.
        if (st->playpath[0] && strcmp(cur, st->playpath) != 0) {
            snprintf(cur, sizeof cur, "%s", st->playpath);
            np_load_meta(); s_mq_off = 0; last_meta = -1;
            play_t0 = esp_timer_get_time() / 1000; silent_shown = false;   // re-arm the watchdog
        }

        // After the grace period, ask the engine whether anything is blocking sound and, if so, show
        // its verdict in place of the "Next:" line (2 s covers the SD open + Helix init + the first
        // frame even on a cold card). Deliberately NOT gated on "zero frames decoded": the decoder
        // counts a frame BEFORE the I2S sink is checked (nucleo_audio_mp3.c), so a failed i2s_open
        // yields frames > 0 with a NULL TX handle — decoding happily into the void. Gating on the
        // frame count would hide exactly that case. why_silent() returns false when audio is really
        // flowing, so calling it unconditionally costs one cheap check per track.
        if (!silent_shown && esp_timer_get_time() / 1000 - play_t0 > 2000) {
            char why[64];
            if (nucleo_audio_why_silent(why, sizeof why)) {
                silent_shown = true;
                d.fillRect(0, BADGE_Y, 240, 9, BG);      // takes over the badge row while it stands
                char msg[72]; snprintf(msg, sizeof msg, "SILENT: %.56s", why);
                d.setTextSize(1); d.setTextColor(RED, BG); d.setCursor(NP_MARGIN, BADGE_Y); d.print(msg);
            }
        } else if (silent_shown && nucleo_audio_dbg_frames() > 0 && !nucleo_audio_is_muted()) {
            // Audio recovered (e.g. the user unmuted): drop the verdict and force a full repaint so the
            // badge row comes back over it. Without this the red SILENT line stuck forever.
            silent_shown = false; last_meta = -1;
        }

        bool playing = nucleo_audio_is_playing(), paused = nucleo_audio_is_paused();
        const char *bn = strrchr(st->playpath,'/'); bn = bn ? bn+1 : st->playpath;
        bool fav = music_db_is_fav(bn);
        uint16_t col = np_state_col(playing, paused);

        // Repaint split by what actually changed so a passing second is cheap:
        //  - meta (volume/play state/track/fav) -> title chrome + marquee + meta row + badges
        //  - elapsed second                      -> times row
        //  - progress percent                    -> progress underline + seek groove
        //  - EVERY frame                         -> the visualiser (delta-only, ~a few rects)
        //  - marquee scroll                      -> time-based, its own band
        int meta = ((nucleo_audio_volume()/5)) | (paused?1<<7:0) | (playing?1<<8:0)
                 | (nucleo_audio_is_muted()?1<<9:0) | (st->qidx<<10) | (fav?1<<21:0);
        int el  = (int)nucleo_audio_elapsed();
        int pct = nucleo_audio_progress();
        if (meta != last_meta) {
            last_meta = meta;
            np_header_chrome(fav); np_marquee(); np_meta();
            np_progline(col); np_seek(col);
            if (!silent_shown) np_badges();              // don't clobber the silent verdict
            last_pct = pct;
        } else if (pct != last_pct) {
            last_pct = pct; np_progline(col); np_seek(col);
        }
        if (el != last_el) { last_el = el; np_times(); }

        // The visualiser runs every frame — it is what makes the screen feel alive. Delta-only, so a
        // steady passage costs only the bars that actually moved.
        np_viz(false);

        // Marquee scroll: time-based and independent of the edges above so a passing second can't
        // starve it. Held still while paused. Its band is disjoint from everything else redrawn here.
        if ((int)strlen(s_np_title) * 12 > MQ_W && !paused) {   // only long titles scroll
            int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms - last_mq_ms >= 150) {
                last_mq_ms = now_ms;
                s_mq_off += 3; if (s_mq_off >= (int)strlen(s_np_title) * 12 + 24) s_mq_off = 0;
                np_marquee();                            // one opaque blit over its band; chrome stays put
            }
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    np_mq_free();                                        // hand the sprite RAM back before we leave
    if (st->playpath[0] == 0)
        for (int i = 0; i < 8 && !nucleo_screen_acquire(); i++) vTaskDelay(pdMS_TO_TICKS(20));
    d.fillScreen(BG);
    nucleo_app_force_repaint();   // now_playing drew direct-to-panel: force a full re-blit or the browser list stays black
}

extern "C" void nucleo_register_player(void)
{
    static const nucleo_app_def_t app = {
        "music", "Music", "Media",
        "MP3/WAV browser — Shuffle, Repeat, Autoplay. TAB=settings.",
        'M', 0xFBB6, enter, on_key, tick, draw, leave,
        NX_NET_APP | NX_SOLO | NX_WIFI
            // SOLO: reboot into a fresh, UNFRAGMENTED heap. The runtime reclaim frees RAM but cannot
            // defragment, and Helix needs ~24 KB contiguous — see the note above play_q.
            // WIFI: with NX_SOLO this means "never start the radio for this boot". Wi-Fi costs ~48 KB
            // and halves the largest contiguous block (31 KB -> 15 KB), which is BELOW what the decoder
            // needs — so leaving it on would undo the whole point of the Solo reboot. Music is a purely
            // local player (files come off the SD), and leaving the app reboots into the full OS, so the
            // radio comes back on its own without the fragile in-place restore.
    };
    nucleo_app_register(&app);
}
