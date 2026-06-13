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
#include "nucleo_exclusive.h"   // dedicated-mode RAM reclaim (~70KB) while a track plays — like the video player
#include "freertos/task.h"
extern "C" {
#include "nucleo_audio.h"
#include "nucleo_board.h"
#include "nucleo_kbd.h"
#include "cJSON.h"
}
#include "app_gfx.h"

// ---- Design tokens (RGB565) — tuned for legibility on the dark panel, aligned with Radio ----
#define BG    0x0841 // base background (void blue)
#define SURF  0x10A2 // raised surface / progress-track background
#define CAP   0x1A8B // focused capsule background (settings rows)
#define FG    0xFFFF // primary text (always legible on BG)
#define MUTED 0x8C71 // secondary text — readable grey (NOT the old near-black)
#define DIM   0x52CB // tertiary text / key hints
#define LINE  0x2945 // hairline separators
#define ACC   0x4DDF // primary accent — bright blue
#define GRN   0x8FF3 // playing / positive — mint green
#define AMB   0xFE8C // paused / folders / warm accent
#define RED   0xF96B // favourite / stop — warm red
#define INK   0x0000 // text on bright fills

#define MUSIC_DIR     NUCLEO_SD_MOUNT "/data/Music"
#define SETTINGS_PATH NUCLEO_SD_MOUNT "/system/config/player.json"
#define MAXE   96   // max entries in current folder
#define MAXQ   64   // max queued tracks
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

// ── Dedicated-mode RAM window for playback ─────────────────────────────────────────────────────────
// The Helix MP3 decoder needs a chunky contiguous working set; on this PSRAM-less, ~50%-fragmented heap
// the idle ~24 KB free wasn't enough and MP3InitDecoder OOM'd ("non riproduce più"). So while a track
// plays we take the SAME dedicated window the video player uses: nucleo_exclusive_enter(NX_NET_APP) frees
// ~70 KB by suspending httpd/L1/mDNS/voice (Wi-Fi STA stays up). HELD ACROSS track changes — entered in
// play_q, released only when playback FULLY stops (on_tick, when the queue clears playpath) — so there's
// no httpd stop/start churn between songs. Just browsing the library (not playing) keeps the network up.
static bool s_excl = false;
static void music_excl(bool on)
{
    if (on == s_excl) return;
    if (on) {
        nucleo_exclusive_info_t inf;
        nucleo_exclusive_enter(NX_NET_APP, &inf);
        ESP_LOGI("music", "excl: reclaim free %u->%u largest %u->%u",
                 (unsigned)inf.free_before, (unsigned)inf.free_after,
                 (unsigned)inf.largest_before, (unsigned)inf.largest_after);
    } else {
        nucleo_exclusive_exit();
    }
    s_excl = on;
}

static void play_q(int pos)
{
    if (!st || pos < 0 || pos >= st->qn) return;
    int ti = track_at(pos);
    char abs[300]; snprintf(abs, sizeof abs, "%s%s", st->qdir, st->q[ti]);

    music_excl(true);   // ~70 KB headroom so the Helix decoder never OOMs (held across tracks; freed in on_tick when stopped)

    // Hand the shared ~32 KB canvas back to the heap BEFORE the Helix decoder starts: every play
    // path funnels through here (browser autoplay/skip, queue advance, now_playing), so doing it
    // once here covers them all. Idempotent — re-acquire is lazy when the list repaints.
    nucleo_app_release_buffers();

    // Helix needs ~17 KB contiguous internal RAM; refuse early with honest feedback instead of a
    // muted failure when the largest free block can't hold the decoder.
    size_t blk = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (blk < 18 * 1024) {
        nucleo_app_set_hint("RAM insufficiente per riprodurre");
        return;
    }

    if (nucleo_audio_play(abs) == ESP_OK) {
        st->qidx = pos;
        snprintf(st->playpath, sizeof st->playpath, "%s", abs);
        if (s_vol_default > 0) nucleo_audio_set_volume(s_vol_default);
    } else {
        nucleo_app_set_hint("Riproduzione non riuscita");
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
        int maxc = navail / 6; if (maxc < 1) maxc = 1; if (maxc > 36) maxc = 36;
        char nb[40]; snprintf(nb, sizeof nb, "%.*s", maxc, bn);
        d.setTextSize(1); d.setTextColor(namec, BG); d.setCursor(nx, y + (h-8)/2); d.print(nb);
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
    for (int i = s_scroll; i <= s_sel; i++) scan_y += (i == s_sel) ? 28 : 15;
    while (scan_y > region_h && s_scroll < s_sel) {
        scan_y -= (s_scroll == s_sel) ? 28 : 15;
        s_scroll++;
    }

    int y = y0;
    for (int i = s_scroll; i < s_n && y < y0 + region_h; i++) {
        int h = (i == s_sel) ? 28 : 15;
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
    unsigned short c = paused ? AMB : GRN;
    int mid = y + 1 + (STRIP_H - 2) / 2;

    // Right side: optional mode badge, then a fixed-width time field to its left.
    char badge[12] = "";
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
    int h = large ? 50 : 32;
    d.fillRoundRect(4, y, 232, h - 2, 9, focus ? CAP : BG);
    if (focus) d.fillRoundRect(4, y + 3, 5, h - 8, 2, ACC);   // accent rail

    d.setTextSize(2); d.setTextColor(focus ? FG : MUTED, focus ? CAP : BG);
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
    if (kind == SV_ACTION) {
        int bw = 28, bh = 22, bx = 230 - bw, vy = y + (h - bh) / 2;
        d.fillRoundRect(bx, vy, bw, bh, 6, focus ? ACC : SURF);
        unsigned short ar = focus ? INK : MUTED;
        int ax = bx + bw / 2 - 2, ay = vy + bh / 2;
        d.fillTriangle(ax, ay - 4, ax, ay + 4, ax + 4, ay, ar);    // chevron
        return;
    }
    // SV_TEXT: value chip
    if (val && val[0]) {
        int vw = (int)strlen(val) * 12 + 14, vh = 22, bx = 230 - vw, vy = y + (h - vh) / 2;
        if (focus) d.fillRoundRect(bx, vy, vw, vh, 6, SURF);
        d.setTextSize(2); d.setTextColor(focus ? FG : MUTED, focus ? SURF : BG);
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
        n = 2;
        it[0].label = "Volume";    it[0].kind = SV_SLIDER; it[0].vol = nucleo_audio_volume();
        it[1].label = "Start Vol"; it[1].kind = SV_TEXT;   snprintf(it[1].val, 16, s_vol_default ? "%d%%" : "Last", s_vol_default);
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

// Settings keys. NOTE: NK_LEFT and NK_BACK never arrive here — the framework routes them to
// the back handler (player_back). So tabs cycle with RIGHT and "back" is handled hierarchically.
static void settings_key(int key, char ch)
{
    (void)ch;

    // Volume adjust mode: RIGHT raises, LEFT lowers (LEFT arrives via player_back). UP/DN also work.
    // ENTER leaves; Esc leaves via player_back.
    if (s_set_edit) {
        if      (key == NK_RIGHT || key == NK_UP) nucleo_audio_set_volume(nucleo_audio_volume() + 5);
        else if (key == NK_DOWN)                  nucleo_audio_set_volume(nucleo_audio_volume() - 5);
        else if (key == NK_ENTER)                 s_set_edit = false;
        nucleo_app_request_draw(); return;
    }

    // Horizontal pager: RIGHT cycles tabs from ANYWHERE (header or a row) — no need to climb
    // back to the top. Landing on row 0 keeps you reading the new tab's content immediately.
    if (key == NK_RIGHT) {
        int a[4], na = settings_avail(a);
        s_set_tab = a[(tab_index(a, na, s_set_tab) + 1) % na];
        if (s_set_row >= 0) s_set_row = 0;
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
        } else if (s_set_tab == 2) {                          // QUEUE
            if (s_set_row == 1 && st && st->qn > 0) rebuild_shuffle();
            else if (s_set_row == 2 && st) {
                nucleo_audio_stop(); st->qn = 0; st->qidx = 0; st->playpath[0] = 0;
                s_set_open = false; nucleo_app_request_draw(); return;
            }
        } else if (s_set_tab == 3) {                          // FIND
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
        case 1:  nucleo_app_set_hint("Type to search \xb7 ENTER find \xb7 ESC"); break;
        case 2:  nucleo_app_set_hint("UP/DN pick \xb7 ENTER choose \xb7 ESC"); break;
        case 3:  nucleo_app_set_hint("L/R tab \xb7 UP/DN row \xb7 ESC close"); break;
        default: nucleo_app_set_hint("UP/DN browse \xb7 ENTER play \xb7 TAB menu");
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
    // Release the dedicated RAM window once playback FULLY stops (queue ended / user stopped -> playpath
    // cleared). Held across track changes (playpath stays set), so no httpd stop/start churn between songs.
    if (!nucleo_audio_is_playing() && !nucleo_audio_is_paused() && (!st || st->playpath[0] == 0))
        music_excl(false);

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
    if (!st) st = (PState *)calloc(1, sizeof(PState));
    load_settings();
    music_db_init();
    nucleo_app_set_tab_handler(on_tab);
    nucleo_app_set_back_handler(player_back);   // hierarchical ESC/Left (else the framework closes us)
    memset(st, 0, sizeof(*st));
    st->filter_type = -1;
    strcpy(s_path, "/"); scan();
    // "Has music" = any audio anywhere under Music (a track at root, or a folder with songs).
    s_has_music = false;
    for (int i = 0; i < s_n; i++) if (!st->e[i].dir || st->e[i].count > 0) { s_has_music = true; break; }
    s_set_open = false; s_set_edit = false;
    s_strip_struct = -1; s_strip_el = -1; s_strip_pct = -1; s_groove_y = -1;
    s_hint_last = -1; update_hint();    // show the browser hint in the footer
}
static void leave(void)
{
    nucleo_audio_stop();
    music_excl(false);             // restore httpd/L1/mDNS/voice — never leave them suspended after the app closes
    np_mq_free();                  // safety net: now_playing frees on exit, but never leak the sprite
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
#define MQ_W 200
#define MQ_H 16
static M5Canvas *s_mq_cv     = nullptr;
static bool      s_mq_failed = false;   // createSprite failed this session -> direct fallback, no retry

static bool np_mq_acquire(void)
{
    if (s_mq_cv) return true;
    if (s_mq_failed) return false;
    M5Canvas *cv = new (std::nothrow) M5Canvas(nucleo_app_gfx());
    if (!cv) { s_mq_failed = true; return false; }
    cv->setColorDepth(8);
    cv->setPsram(false);   // PSRAM-less board: skip the doomed SPIRAM probe (the misleading "oom 3204 B caps=0x404") → DMA-internal directly
    if (!cv->createSprite(MQ_W, MQ_H)) { delete cv; s_mq_failed = true; return false; }
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

// Static title-band chrome: favourite heart + accent rule. Clears the whole band, so it runs
// ONLY when the title/fav/meta actually changes — never on the marquee cadence (np_marquee owns
// the scrolling text region and never lets the panel flash).
static void np_header_chrome(bool fav)
{
    d.fillRect(0, 2, 240, 19, BG);
    if (fav) icon_heart(220, 6, RED);
    d.drawFastHLine(8, 22, 224, LINE);
    int tw = (int)strlen(s_np_title) * 12;
    d.fillRect(8, 22, tw > 150 ? 150 : tw, 2, ACC);
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
        cv->pushSprite(8, 4);
    } else {                                             // no sprite RAM -> direct (may flicker)
        d.setClipRect(8, 4, avail, MQ_H);
        d.fillRect(8, 4, avail, MQ_H, BG);
        d.setTextSize(2); d.setTextColor(ACC, BG);
        int x = 8 - s_mq_off;
        d.setCursor(x, 4);        d.print(s_np_title);
        d.setCursor(x + span, 4); d.print(s_np_title);
        d.clearClipRect();
    }
}

// Circular progress ring with a play/pause core (the hero element).
static void np_ring(bool playing, bool paused)
{
    const int cx = 54, cy = 66, r0 = 24, r1 = 33;
    int pct = nucleo_audio_progress(); if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    unsigned short col = paused ? AMB : (playing ? GRN : MUTED);
    int a = 360 * pct / 100;
    if (a > 0)   d.fillArc(cx, cy, r0, r1, 0, a, col);     // elapsed
    if (a < 360) d.fillArc(cx, cy, r0, r1, a, 360, SURF);  // remaining
    d.fillCircle(cx, cy, r0 - 2, BG);                      // clear core
    if (playing) icon_pause(cx - 6, cy - 9, 18, col);
    else         icon_play(cx - 5, cy - 9, 18, col);
}

// Elapsed/duration readout only — the one part of the info column that ticks every second.
// Redrawn on its own so a passing second doesn't repaint the track #, metadata, volume bar,
// and mode badges that didn't change. Region is clear of the volume bar (y78) and meta (y39).
static void np_time(void)
{
    const int x = 100;
    d.fillRect(x, 52, 132, 16, BG);
    uint32_t el = nucleo_audio_elapsed(), du = nucleo_audio_duration_ms() / 1000;
    char te[8], td[8]; fmt_time(te, sizeof te, el); fmt_time(td, sizeof td, du);
    d.setTextSize(2); d.setTextColor(FG, BG); d.setCursor(x, 52); d.print(te);
    if (du) { d.setTextSize(1); d.setTextColor(MUTED, BG);
        char t2[10]; snprintf(t2, sizeof t2, "/ %s", td);
        d.setCursor(x + (int)strlen(te) * 12 + 6, 58); d.print(t2); }
}

// Right-hand info column: position, artist/genre, time, volume, mode badges.
static void np_info(void)
{
    const int x = 100;
    d.fillRect(x, 26, 140, 84, BG);

    char pos[16]; snprintf(pos, sizeof pos, "TRACK %d/%d", st->qidx + 1, st->qn);
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(x, 27); d.print(pos);

    if (s_np_meta[0]) { char m[24]; snprintf(m, sizeof m, "%.20s", s_np_meta);
        d.setTextColor(DIM, BG); d.setCursor(x, 39); d.print(m); }

    np_time();

    int vol = nucleo_audio_volume();
    const int vx = x, vy = 78, vw = 104, vh = 12;
    d.fillRoundRect(vx, vy, vw, vh, vh / 2, SURF);
    int fw = vw * vol / 100; if (fw > 0) d.fillRoundRect(vx, vy, fw, vh, vh / 2, GRN);
    char vb[8]; snprintf(vb, sizeof vb, "%d%%", vol);
    d.setTextSize(1); d.setTextColor(FG, BG); d.setCursor(vx + vw + 6, vy + 2); d.print(vb);

    int bx = x, by = 96;
    if (s_shuffle) { d.fillRoundRect(bx, by, 16, 12, 4, SURF); d.setTextColor(ACC, SURF); d.setCursor(bx + 5, by + 2); d.print("~"); bx += 20; }
    if (s_repeat)  { const char *rl = s_repeat == 1 ? "R1" : "RA"; d.fillRoundRect(bx, by, 22, 12, 4, SURF); d.setTextColor(ACC, SURF); d.setCursor(bx + 5, by + 2); d.print(rl); bx += 26; }
    if (!s_shuffle && !s_repeat) { d.setTextColor(DIM, BG); d.setCursor(x, by + 2); d.print("in order"); }
}

// "Next up" line + bottom key legend (drawn once and on track/mode change).
static void np_next(void)
{
    d.fillRect(0, 110, 240, 10, BG);
    int np = next_pos();
    if (np >= 0 && np != st->qidx) {
        int ti = track_at(np);
        const char *nm = st->q[ti]; const char *bn = strrchr(nm, '/'); bn = bn ? bn + 1 : nm;
        char b[42]; snprintf(b, sizeof b, "Next: %.32s", bn);
        d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(8, 111); d.print(b);
    }
}

static void np_legend(void)
{
    d.drawFastHLine(8, 121, 224, LINE);
    d.setTextSize(1); d.setTextColor(DIM, BG); d.setCursor(8, 125);
    d.print("L/R seek  U/D vol  [ ] track  f fav");
}

static void now_playing(void)
{
    // Release the shared canvas here too (play_q also releases) so we grab the small marquee sprite
    // while the heap is freshly freed and before the decoder takes its ~17 KB — that ordering keeps
    // the sprite from fragmenting the decoder's contiguous block (acquire order = stability).
    nucleo_app_release_buffers();
    s_mq_failed = false; np_mq_acquire();
    char target[300]; snprintf(target, sizeof target, "%s%s", st->qdir, st->q[track_at(st->qidx)]);
    if (strcmp(target, st->playpath) != 0 || !nucleo_audio_is_playing()) play_q(st->qidx);
    if (st->playpath[0] == 0) {  // failed to start
        np_mq_free();
        for (int i = 0; i < 8 && !nucleo_screen_acquire(); i++) vTaskDelay(pdMS_TO_TICKS(20));
        d.fillScreen(BG); nucleo_app_request_draw(); return;
    }

    d.fillScreen(BG);
    np_load_meta(); s_mq_off = 0;
    np_legend(); np_next();
    char cur[208]; snprintf(cur, sizeof cur, "%s", st->playpath);

    bool back = false, started = false;
    int last_meta = -1, last_el = -1, last_pct = -1, mq = 0;

    while (!back) {
        esp_task_wdt_reset();
        nucleo_key_t k = nucleo_kbd_read();
        if (k.key != NK_NONE) {
            if      (k.key==NK_BACK||k.key==NK_TAB||k.ch=='`') back = true;
            else if (k.ch=='s')                                { nucleo_audio_stop(); st->playpath[0]=0; back=true; }
            else if (k.key==NK_ENTER||k.ch==' '||k.ch=='p')    nucleo_audio_toggle_pause();
            else if (k.key==NK_LEFT)                            seek_rel(-10);
            else if (k.key==NK_RIGHT)                           seek_rel(+10);
            else if (k.key==NK_UP)                              nucleo_audio_set_volume(nucleo_audio_volume()+10);
            else if (k.key==NK_DOWN)                            nucleo_audio_set_volume(nucleo_audio_volume()-10);
            else if (k.ch=='[') { play_q(prev_pos()); started=false; }
            else if (k.ch==']') { int p=next_pos(); if(p>=0){play_q(p);started=false;}else{st->playpath[0]=0;back=true;} }
            else if (k.ch=='f') { const char *bn=strrchr(st->playpath,'/'); bn=bn?bn+1:st->playpath;
                                  music_db_set_fav(bn, !music_db_is_fav(bn)); last_meta=-1; }
            else if (k.ch=='r') { s_repeat=(s_repeat+1)%3; save_settings(); np_next(); last_meta=-1; }
            else if (k.ch>='0'&&k.ch<='9') {
                uint32_t dur=nucleo_audio_duration_ms();
                if (dur) nucleo_audio_seek((uint32_t)((int64_t)(k.ch-'0')*dur/10));
            }
        }
        if (!started && nucleo_audio_is_playing() && nucleo_audio_elapsed_ms() > 200) started = true;
        player_update_logic(&back);

        // Track changed (auto-advance or skip) -> refresh metadata + up-next.
        if (st->playpath[0] && strcmp(cur, st->playpath) != 0) {
            snprintf(cur, sizeof cur, "%s", st->playpath);
            np_load_meta(); s_mq_off = 0; np_next(); last_meta = -1;
        }

        bool playing = nucleo_audio_is_playing(), paused = nucleo_audio_is_paused();
        const char *bn = strrchr(st->playpath,'/'); bn = bn ? bn+1 : st->playpath;
        bool fav = music_db_is_fav(bn);

        // Split the repaint by what actually changed so a passing second is cheap:
        //  - meta (volume/play state/track/fav) -> full header + ring + info column
        //  - elapsed second                      -> just the time readout
        //  - ring progress percent               -> just the ring arc
        //  - otherwise                            -> advance the title marquee
        int meta = ((nucleo_audio_volume()/5)) | (paused?1<<7:0) | (playing?1<<8:0)
                 | (st->qidx<<9) | (fav?1<<20:0);
        int el  = (int)nucleo_audio_elapsed();
        int pct = nucleo_audio_progress();
        if (meta != last_meta) {
            last_meta = meta;
            np_header_chrome(fav); np_marquee(); np_ring(playing, paused); np_info();
            last_el = el; last_pct = pct;                // info just drew the current time/ring
        } else if (el != last_el || pct != last_pct) {
            if (el  != last_el)  { last_el  = el;  np_time(); }
            if (pct != last_pct) { last_pct = pct; np_ring(playing, paused); }
        } else if (++mq >= 3) {                          // ~150 ms marquee step
            mq = 0;
            if ((int)strlen(s_np_title) * 12 > MQ_W) {   // only long titles scroll
                s_mq_off += 3; if (s_mq_off >= (int)strlen(s_np_title) * 12 + 24) s_mq_off = 0;
                np_marquee();                            // sprite blit only — chrome stays put
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    np_mq_free();                                        // hand the sprite RAM back before we leave
    if (st->playpath[0] == 0)
        for (int i = 0; i < 8 && !nucleo_screen_acquire(); i++) vTaskDelay(pdMS_TO_TICKS(20));
    d.fillScreen(BG);
    nucleo_app_request_draw();
}

extern "C" void nucleo_register_player(void)
{
    static const nucleo_app_def_t app = {
        "music", "Music", "Media",
        "MP3/WAV browser — Shuffle, Repeat, Autoplay. TAB=settings.",
        'M', 0xFBB6, enter, on_key, tick, draw, leave
    };
    nucleo_app_register(&app);
}
