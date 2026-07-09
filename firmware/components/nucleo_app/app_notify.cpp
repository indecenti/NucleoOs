// Notification Center (native): the on-device twin of the web Notification Center.
//
// Reads the SAME history the backbone writes — /sd/system/notify.jsonl (see nucleo_notify.cpp) —
// and shows it in a tabbed, Win11-style list: Tutte / Calendario / Sistema / ANIMA. Enter runs the
// notification's action (open an app, ask ANIMA, open a file); TAB cycles the tab; up/down scroll.
//
// Cost discipline (the device's rule): the journal is parsed ONLY on enter (never on a timer), into
// a buffer that is malloc'd on open and FREED on leave — nothing sits in .bss when the app is closed.
// Parsing keeps just the newest MAX_N lines via a sliding window (CPU, not RAM). tick() repaints only
// while the list is still animating, so an idle screen never repaints. No task, no polling.
#include "nucleo_app.h"
#include "app_ui.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "cJSON.h"
extern "C" {
#include "nucleo_board.h"
}
#include "launcher_theme.h"   // W/H + RGB565 palette
#include "app_gfx.h"          // the `d` draw-target redirect (never name a local `d`)

extern "C" void nucleo_anima_app_ask(const char *q);   // seed ANIMA with a question (app_anima.cpp)

#define JRNL_PATH NUCLEO_SD_MOUNT "/system/notify.jsonl"
#define JRNL_BAK  NUCLEO_SD_MOUNT "/system/notify.jsonl.0"
static const unsigned short ACC = C_PURPLE;

enum { TAB_ALL, TAB_CAL, TAB_SYS, TAB_AI, TAB_COUNT };
static const char *TAB_NAME[TAB_COUNT] = { "Tutte", "Cal", "Sis", "AI" };

// One stored notification (display fields only; body/id dropped to keep RAM tight).
struct NItem { char src[12]; char lvl[10]; char ttl[60]; char act[28]; char tstr[8]; };
#define MAX_N 24
static NItem *s_items = nullptr;     // malloc on enter, free on leave (chronological: oldest..newest)
static int s_n = 0;
static int s_view[MAX_N];            // item indices matching the current tab (newest-first)
static int s_vn = 0;
static int s_cnt[TAB_COUNT];         // per-tab counts (the digest)
static int s_tab = TAB_ALL;
static int s_sel = 0;
static bool s_oom = false;           // true if the ~3 KB buffer couldn't be allocated on enter
static bool s_confirm_clr, s_clr_yes;   // pending "clear history?" confirm card

// ---- helpers ---------------------------------------------------------------
static int tab_of(const char *src)
{
    if (!strcmp(src, "calendar")) return TAB_CAL;
    if (!strcmp(src, "anima"))    return TAB_AI;
    return TAB_SYS;                                   // system/ota/recorder/voice/app
}
static unsigned short lvl_color(const char *lvl)
{
    if (!strcmp(lvl, "success"))  return C_GREEN;
    if (!strcmp(lvl, "warn"))     return C_YELLOW;
    if (!strcmp(lvl, "critical")) return C_RED;
    return C_BLUE;                                    // info
}
static void fmt_when(long ts, char *buf, int sz)
{
    if (ts <= 0) { snprintf(buf, sz, "--:--"); return; }
    time_t t = (time_t)ts, now = time(NULL);
    struct tm tm, tn; localtime_r(&t, &tm); localtime_r(&now, &tn);
    if (tm.tm_year == tn.tm_year && tm.tm_yday == tn.tm_yday) snprintf(buf, sz, "%02d:%02d", tm.tm_hour, tm.tm_min);
    else snprintf(buf, sz, "%02d/%02d", tm.tm_mday, tm.tm_mon + 1);
}

// Pull a string field from a parsed line into dst (bounded).
static void get_str(cJSON *o, const char *k, char *dst, int sz, const char *dflt)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    snprintf(dst, sz, "%s", (cJSON_IsString(v) && v->valuestring[0]) ? v->valuestring : dflt);
}

// Parse one JSON line into the sliding window (keep the newest MAX_N — drop the oldest by shifting,
// which is CPU we happily spend to avoid a bigger buffer).
static void ingest_line(const char *line)
{
    cJSON *o = cJSON_Parse(line);
    if (!o) return;
    NItem it;
    get_str(o, "src", it.src, sizeof it.src, "system");
    get_str(o, "lvl", it.lvl, sizeof it.lvl, "info");
    get_str(o, "ttl", it.ttl, sizeof it.ttl, "(notifica)");
    get_str(o, "act", it.act, sizeof it.act, "");
    cJSON *ts = cJSON_GetObjectItem(o, "ts");
    fmt_when(cJSON_IsNumber(ts) ? (long)ts->valuedouble : 0, it.tstr, sizeof it.tstr);
    cJSON_Delete(o);

    if (s_n < MAX_N) s_items[s_n++] = it;
    else { memmove(s_items, s_items + 1, (MAX_N - 1) * sizeof(NItem)); s_items[MAX_N - 1] = it; }
}

static void load_one_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) if (line[0] == '{') ingest_line(line);
    fclose(f);
}

static void load_journal(void)
{
    s_n = 0;
    if (!s_items) return;
    load_one_file(JRNL_BAK);     // older half first (so the newest end up last = freshest)
    load_one_file(JRNL_PATH);
    for (int t = 0; t < TAB_COUNT; t++) s_cnt[t] = 0;
    for (int i = 0; i < s_n; i++) { s_cnt[TAB_ALL]++; s_cnt[tab_of(s_items[i].src)]++; }
}

// Build the visible list for the current tab, newest-first.
static void build_view(void)
{
    s_vn = 0;
    for (int i = s_n - 1; i >= 0 && s_vn < MAX_N; i--)
        if (s_tab == TAB_ALL || tab_of(s_items[i].src) == s_tab) s_view[s_vn++] = i;
    if (s_sel >= s_vn) s_sel = s_vn ? s_vn - 1 : 0;
}

// ---- row providers (index into s_view) -------------------------------------
static const char *row_label(int i, void *) { return s_items[s_view[i]].ttl; }
static const char *row_right(int i, void *) { return s_items[s_view[i]].tstr; }
static unsigned short row_color(int i, void *) { return lvl_color(s_items[s_view[i]].lvl); }

// ---- actions ---------------------------------------------------------------
static void run_action(const char *act)
{
    if (!act || !act[0]) return;
    const char *colon = strchr(act, ':');
    if (!colon) return;
    int klen = (int)(colon - act);
    const char *arg = colon + 1;
    if (klen == 3 && !strncmp(act, "app", 3))       nucleo_app_launch_id(arg);
    else if (klen == 4 && !strncmp(act, "file", 4)) nucleo_app_launch_file(arg);
    else if (klen == 5 && !strncmp(act, "anima", 5)) { nucleo_anima_app_ask(arg); nucleo_app_launch_id("anima"); }
}

// ---- drawing ---------------------------------------------------------------
static void draw_tabs(int y)
{
    d.fillRect(0, y, W, 16, BG);                      // clear the strip (gap the widgets don't own)
    const int n = TAB_COUNT, gap = 4, x0 = 6;
    int tw = (W - x0 * 2 - gap * (n - 1)) / n;        // 4 equal pills
    for (int i = 0; i < n; i++) {
        int x = x0 + i * (tw + gap);
        bool act = (i == s_tab);
        if (act) d.fillRoundRect(x, y, tw, 14, 4, ACC);
        else     d.drawRoundRect(x, y, tw, 14, 4, LINE);
        char lbl[16]; snprintf(lbl, sizeof lbl, "%s %d", TAB_NAME[i], s_cnt[i]);
        d.setTextSize(1); d.setTextColor(act ? INK : MUTED, act ? ACC : BG);
        d.setCursor(x + (tw - (int)strlen(lbl) * 6) / 2, y + 3); d.print(lbl);
    }
}

static void draw(void)
{
    char right[16]; snprintf(right, sizeof right, "%d", s_cnt[s_tab]);
    int y0 = app_ui_title("Notifiche", ACC, right);

    int yt = y0;                                      // tab strip
    draw_tabs(yt);
    int yl = yt + 18;                                 // list band below the tabs

    if (s_oom) {                                       // buffer alloc failed — honest, don't deref
        d.fillRect(0, yl, W, (H - HINT) - yl, BG);
        const char *msg = "Memoria insufficiente";
        d.setTextSize(1); d.setTextColor(C_RED, BG);
        d.setCursor((W - (int)strlen(msg) * 6) / 2, yl + 24); d.print(msg);
        return;
    }
    if (s_vn == 0) {
        d.fillRect(0, yl, W, (H - HINT) - yl, BG);
        const char *msg = "Nessuna notifica";
        d.setTextSize(1); d.setTextColor(DIM, BG);
        d.setCursor((W - (int)strlen(msg) * 6) / 2, yl + 24); d.print(msg);
        return;
    }
    app_ui_list(yl, (H - HINT) - yl, s_vn, s_sel, row_label, row_right, row_color, nullptr);
    if (s_confirm_clr) app_ui_confirm("Cancella tutto?", "Elimina la cronologia notifiche", s_clr_yes);
}

// ---- input / lifecycle -----------------------------------------------------
static void cycle_tab(void)                           // TAB handler
{
    s_tab = (s_tab + 1) % TAB_COUNT; s_sel = 0;
    build_view(); nucleo_app_request_draw();
}

static void on_key(int key, char ch)
{
    if (s_confirm_clr) {                              // clear-history card is up: it owns every key
        int r = app_ui_confirm_key(key, ch, &s_clr_yes);
        if (r == 1) { remove(JRNL_PATH); remove(JRNL_BAK); load_journal(); build_view(); }
        if (r >= 0) s_confirm_clr = false;
        nucleo_app_request_draw();
        return;
    }
    if (key == NK_ENTER) {
        if (s_vn > 0) run_action(s_items[s_view[s_sel]].act);
        return;
    }
    if (ch == 'c' && s_vn > 0) {                      // clear history (device + the shared store)
        s_confirm_clr = true; s_clr_yes = false;
        nucleo_app_request_draw();
        return;
    }
    if (app_ui_list_key(key, ch, &s_sel, s_vn, row_label, nullptr)) nucleo_app_request_draw();
}

static void tick(void)
{
    if (app_ui_list_animating()) nucleo_app_request_draw();   // repaint only while settling
}

static void enter(void)
{
    if (!s_items) s_items = (NItem *)malloc(sizeof(NItem) * MAX_N);   // ~3 KB, only while open
    s_oom = (s_items == nullptr);                                     // degrade cleanly, never deref
    s_tab = TAB_ALL; s_sel = 0; s_confirm_clr = false;
    load_journal(); build_view();                                     // both no-op when s_items==NULL
    nucleo_app_set_tab_handler(cycle_tab);
    nucleo_app_set_hint(s_oom ? "memoria insufficiente   esc indietro"
                              : "su/giu scegli   enter apri   TAB scheda   c pulisci");
    nucleo_app_request_draw();
}
static void leave(void)
{
    if (s_items) { free(s_items); s_items = nullptr; s_n = 0; s_vn = 0; }   // return the RAM on exit
}

extern "C" void nucleo_register_notify(void)
{
    static const nucleo_app_def_t app = {
        "notify", "Notifiche", "System", "Notification center",
        'n', ACC, enter, on_key, tick, draw, leave
    };
    nucleo_app_register(&app);
}
