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
#include "nucleo_i18n.h"      // TR(it,en): chrome follows the system language live
#include "app_large.h"        // two screens: the list + detail on the 320x240 panel, the Cardputer as companion
#include "nucleo_touch.h"

extern "C" void nucleo_anima_app_ask(const char *q);   // seed ANIMA with a question (app_anima.cpp)

#define JRNL_PATH NUCLEO_SD_MOUNT "/system/notify.jsonl"
#define JRNL_BAK  NUCLEO_SD_MOUNT "/system/notify.jsonl.0"
static const unsigned short ACC = C_PURPLE;

enum { TAB_ALL, TAB_CAL, TAB_SYS, TAB_AI, TAB_COUNT };
static const char *TAB_NAME[TAB_COUNT]    = { "Tutte", "Cal", "Sis", "AI" };
static const char *TAB_NAME_EN[TAB_COUNT] = { "All",   "Cal", "Sys", "AI" };   // picked at paint time

// One stored notification (display fields only; body/id dropped to keep RAM tight).
struct NItem { char src[12]; char lvl[10]; char ttl[60]; char act[28]; char tstr[8]; long ts; };
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
static bool s_drop;                     // ...for the selected notification only (dismiss one)

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
    NItem it = {};   // zero the tail bytes get_str leaves past each string's NUL (the whole struct is copied)
    get_str(o, "src", it.src, sizeof it.src, "system");
    get_str(o, "lvl", it.lvl, sizeof it.lvl, "info");
    get_str(o, "ttl", it.ttl, sizeof it.ttl, "");   // untitled -> localised placeholder at paint time
    get_str(o, "act", it.act, sizeof it.act, "");
    cJSON *ts = cJSON_GetObjectItem(o, "ts");
    it.ts = cJSON_IsNumber(ts) ? (long)ts->valuedouble : 0;
    fmt_when(it.ts, it.tstr, sizeof it.tstr);
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

// Dismiss one notification: rewrite both journal files without its line. The backbone writes every line as
// {..."ttl":"<title>"...,"ts":<n>} (nucleo_notify.cpp), so "ts":<n>} plus the title identify it.
static void drop_item(const NItem *it)
{
    char ts[24]; snprintf(ts, sizeof ts, "\"ts\":%ld}", it->ts);
    static const char *const paths[2] = { JRNL_BAK, JRNL_PATH };
    for (int p = 0; p < 2; p++) {
        FILE *f = fopen(paths[p], "r");
        if (!f) continue;
        char tmp[64]; snprintf(tmp, sizeof tmp, "%s.t", paths[p]);
        FILE *o = fopen(tmp, "w");
        if (!o) { fclose(f); continue; }
        char line[256];
        while (fgets(line, sizeof line, f))
            if (!strstr(line, ts) || (it->ttl[0] && !strstr(line, it->ttl))) fputs(line, o);
        fclose(f); fclose(o);
        remove(paths[p]); rename(tmp, paths[p]);
    }
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
static const char *row_label(int i, void *) {
    const char *t = s_items[s_view[i]].ttl;
    return t[0] ? t : TR("(notifica)", "(notification)");
}
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
        char lbl[16]; snprintf(lbl, sizeof lbl, "%s %d", TR(TAB_NAME[i], TAB_NAME_EN[i]), s_cnt[i]);
        d.setTextSize(1); d.setTextColor(act ? INK : MUTED, act ? ACC : BG);
        d.setCursor(x + (tw - (int)strlen(lbl) * 6) / 2, y + 3); d.print(lbl);
    }
}

static void draw_companion(void);   // two-screen companion, defined below

static void draw(void)
{
    if (nucleo_app_two_screens()) { draw_companion(); return; }
    char right[16]; snprintf(right, sizeof right, "%d", s_cnt[s_tab]);
    int y0 = app_ui_title(TR("Notifiche", "Notifications"), ACC, right);

    int yt = y0;                                      // tab strip
    draw_tabs(yt);
    int yl = yt + 18;                                 // list band below the tabs

    if (s_oom || s_vn == 0) {                          // buffer alloc failed (honest, don't deref) / nothing here
        d.fillRect(0, yl, W, (H - HINT) - yl, BG);
        const char *msg = s_oom ? TR("Memoria insufficiente", "Out of memory") : TR("Nessuna notifica", "No notifications");
        d.setTextSize(1); d.setTextColor(s_oom ? C_RED : DIM, BG);
        d.setCursor((W - (int)strlen(msg) * 6) / 2, yl + 24); d.print(msg);
        return;
    }
    app_ui_list(yl, (H - HINT) - yl, s_vn, s_sel, row_label, row_right, row_color, nullptr);
    if (s_confirm_clr) app_ui_confirm(s_drop ? TR("Archiviare?", "Dismiss?") : TR("Cancella tutto?", "Clear all?"),
                                      s_drop ? row_label(s_sel, nullptr) : TR("Elimina la cronologia notifiche", "Delete notification history"), s_clr_yes);
}

// ===== two screens ==============================================================================================
// The external panel carries the Notification Center full-size: the filter tabs across the top, the list on the
// left, and the selected notification as a card on the right (title, source/level/time, the action it runs).
// The Cardputer is the companion: the active tab, the picked notification and the keys. Touch: the tabs filter,
// a row taps to pick and again to open; the card itself taps to open, a swipe right on it dismisses it (after a
// confirm card) and a swipe up / down on it walks the list.
static const int N_LISTW = 168;
static int s_lg_scroll = 0;                           // the panel list's first visible row
static const char NTABKEYS[TAB_COUNT + 1] = { 1, 2, 3, 4, 0 };   // a control char per tab (no collision with typing)

static void lg_detail(int x, int y, int w, int h)
{
    lg::panel(x, y, w, h);
    const unsigned short cb = lg::card();
    const NItem *it = &s_items[s_view[s_sel]];
    const unsigned short lc = lvl_color(it->lvl);
    d.fillRoundRect(x, y + 8, 4, h - 16, 2, lc);      // the level stripe
    lg::font(1); d.setTextColor(lc, cb);
    const int nl = lg::wrap(row_label(s_sel, nullptr), x + 12, y + 10, w - 22, 22, 4);
    const int yy = y + 12 + (nl < 1 ? 1 : nl) * 22 + 6;
    char meta[56]; snprintf(meta, sizeof meta, "%s  %s", it->src, it->tstr);
    lg::font(0); d.setTextColor(MUTED, cb);
    char mf[56]; lg::fit(mf, sizeof mf, meta, w - 22); d.drawString(mf, x + 12, yy);
    d.setTextColor(it->act[0] ? ACC : DIM, cb);
    lg::fit(mf, sizeof mf, it->act[0] ? it->act : TR("Nessuna azione", "No action"), w - 22); d.drawString(mf, x + 12, y + h - 42);
    lg::font(4); d.setTextColor(DIM, cb);
    d.drawString(TR("scorri >: archivia", "swipe >: dismiss"), x + 12, y + h - 18);
    if (it->act[0]) nucleo_touch_add(x, y, w, h, NK_ENTER, 0);   // the card opens what it announces
    nucleo_touch_add_pad(x, y, w, h, 72);              // swipe right = dismiss, up / down = the next card
}

static void lg_clear_card(void)
{
    const int cw = 248, cx = 36, chh = 130, cy = 55;
    const unsigned short cb = lg::card();
    lg::panel(cx, cy, cw, chh); d.drawRoundRect(cx, cy, cw, chh, 8, C_RED);
    lg::font(1); d.setTextColor(C_RED, cb); d.drawString(s_drop ? TR("Archiviare?", "Dismiss?") : TR("Cancella tutto?", "Clear all?"), cx + 16, cy + 14);
    lg::font(0); d.setTextColor(FG, cb);
    char t[64]; lg::fit(t, sizeof t, s_drop ? row_label(s_sel, nullptr) : TR("Elimina la cronologia notifiche", "Delete notification history"), cw - 32);
    d.drawString(t, cx + 16, cy + 44);
    const int bw = (cw - 48) / 2;
    lg::button(cx + 16, cy + chh - 48, bw, 36, "No", s_clr_yes ? LINE : ACC, s_clr_yes ? FG : INK, NK_CHAR, 'n');
    lg::button(cx + 32 + bw, cy + chh - 48, bw, 36, s_drop ? TR("Archivia", "Dismiss") : TR("Elimina", "Delete"), s_clr_yes ? C_RED : LINE, s_clr_yes ? INK : FG, NK_CHAR, 'y');
}

static void draw_large(void)
{
    nucleo_touch_clear();
    char r[16]; snprintf(r, sizeof r, "%d", s_cnt[TAB_ALL]);
    int y = lg::header(TR("Notifiche", "Notifications"), r, ACC);
    if (s_confirm_clr) { d.fillRect(0, y, 320, 240 - y, BG); lg_clear_card(); nucleo_touch_commit(); return; }   // the card owns the panel
    static char tn[TAB_COUNT][16]; const char *names[TAB_COUNT];
    for (int i = 0; i < TAB_COUNT; i++) { snprintf(tn[i], sizeof tn[i], "%s %d", TR(TAB_NAME[i], TAB_NAME_EN[i]), s_cnt[i]); names[i] = tn[i]; }
    y = lg::tabs(y, names, TAB_COUNT, s_tab, ACC, NTABKEYS);
    if (s_oom || s_vn == 0) {
        lg::message(0, y, 320, 240 - y, s_oom ? TR("Memoria insufficiente", "Out of memory") : TR("Nessuna notifica", "No notifications"),
                    s_oom ? nullptr : TR("Niente in questa scheda.", "Nothing in this tab."), s_oom ? C_RED : DIM);
        nucleo_touch_commit(); return;
    }
    lg::text_list(0, y + 2, N_LISTW, 240 - y - 4, s_vn, s_sel, &s_lg_scroll, row_label, row_right, row_color, nullptr, ACC);
    d.fillRect(N_LISTW, y, 320 - N_LISTW, 240 - y, BG);
    lg_detail(N_LISTW + 4, y + 4, 320 - N_LISTW - 10, 240 - y - 8);
    nucleo_touch_commit();
}

static void draw_companion(void)
{
    const int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, W, h, BG);
    const int y = lg::comp_head("NOTIFICHE", ACC);
    char tb[40]; snprintf(tb, sizeof tb, "%s  %d", TR(TAB_NAME[s_tab], TAB_NAME_EN[s_tab]), s_cnt[s_tab]);
    d.setFont(&fonts::Font0); d.setTextColor(ACC, BG); d.drawString(tb, 8, y + 2);
    if (s_oom || s_vn == 0) {
        d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(s_oom ? C_RED : MUTED, BG);
        d.drawString(s_oom ? TR("Memoria insufficiente", "Out of memory") : TR("Nessuna notifica", "No notifications"), 8, y + 22);
        lg::comp_keys(TR("TAB scheda  ESC esci", "TAB tab  ESC exit")); return;
    }
    const NItem *it = &s_items[s_view[s_sel]];
    d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(lvl_color(it->lvl), BG);
    const int nl = lg::wrap(it->ttl[0] ? it->ttl : TR("(notifica)", "(notification)"), 8, y + 18, 224, 20, 2);
    d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG);
    char meta[56]; snprintf(meta, sizeof meta, "%s  %s  %s", it->src, it->lvl, it->tstr);
    d.drawString(meta, 8, y + 20 + nl * 20);
    char pos[24]; snprintf(pos, sizeof pos, "%d / %d", s_sel + 1, s_vn);
    d.setTextColor(DIM, BG); d.drawString(pos, 8, y + 20 + nl * 20 + 14);
    lg::comp_keys(s_confirm_clr ? TR("y conferma  n annulla", "y confirm  n cancel")
                                : TR("INVIO apri  > archivia  TAB  c pulisci", "ENTER open  > dismiss  TAB  c clear"));
}

// ---- input / lifecycle -----------------------------------------------------
static void cycle_tab(void)                           // TAB handler
{
    s_tab = (s_tab + 1) % TAB_COUNT; s_sel = 0;
    build_view(); nucleo_app_request_draw();
}

static void on_key(int key, char ch)
{
    if (s_confirm_clr) {                              // the confirm card is up: it owns every key
        int r = app_ui_confirm_key(key, ch, &s_clr_yes);
        if (r == 1) { if (s_drop) drop_item(&s_items[s_view[s_sel]]); else { remove(JRNL_PATH); remove(JRNL_BAK); } load_journal(); build_view(); }
        if (r >= 0) s_confirm_clr = false;
        nucleo_app_request_draw();
        return;
    }
    if (key == NK_CHAR && ch >= 1 && ch <= TAB_COUNT) {   // a panel tab tap: filter to that tab
        s_tab = ch - 1; s_sel = 0; build_view(); nucleo_app_request_draw();
        return;
    }
    if (nucleo_app_two_screens()) {                       // panel: tap a row to pick, tap again to run it
        int t = lg::row_tap(key, ch, s_lg_scroll, s_vn, &s_sel);
        if (t == 2) { if (s_vn > 0) run_action(s_items[s_view[s_sel]].act); return; }
        if (t == 1) { nucleo_app_request_draw(); return; }
    }
    if (key == NK_ENTER) {
        if (s_vn > 0) run_action(s_items[s_view[s_sel]].act);
        return;
    }
    if ((ch == 'c' || key == NK_RIGHT) && s_vn > 0) {  // c = clear history, RIGHT / a swipe on the card = dismiss one
        s_confirm_clr = true; s_clr_yes = false; s_drop = key == NK_RIGHT;
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
    nucleo_app_set_hint(s_oom ? TR("memoria insufficiente   esc indietro", "out of memory   esc back")
                              : TR("su/giu  enter apri  TAB sched  c pul", "up/dn  enter open  TAB tab  c clear"));
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
        'n', ACC, enter, on_key, tick, draw, leave, 0, nullptr, draw_large
    };
    nucleo_app_register(&app);
}
