// Calendar app (native): a day-focused agenda for the Cardputer.
//
// The home view centres on the CURRENT ("dying") day and flanks it with the day before
// and the day after — a smartwatch-style focus, but over dates. Move the focus with ‹/› or
// ↑/↓ (t = jump to today); Enter opens a scrollable, readable detail of that day's events
// (the shared focused-list widget, so long titles marquee just like Files/Notes).
//
// Reminders are NOT handled here: a device-wide background service (calendar_svc.cpp) owns
// detection + chime + the bus broadcast, so alerts fire even when this app is closed. This
// file is purely the viewer. Events are the SAME store the web Calendar writes:
// /sd/system/config/calendar.json — { "events": { "YYYY-MM-DD": [ {"time","text"} ] } }.
// Read-only on the device; creating/editing stays in the richer web companion.
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

#include "launcher_theme.h"   // W/H + themed palette (BG, FG, MUTED, DIM, LINE, INK, C_*)
#include "app_gfx.h"          // the `d` draw-target redirect (NB: never name a local `d`)
#include "nucleo_i18n.h"      // TR(it,en): hints follow the system language

#define CAL_PATH NUCLEO_SD_MOUNT "/system/config/calendar.json"
static const unsigned short ACC = C_GREEN;

// ---- state -----------------------------------------------------------------
enum { V_DAYS, V_DETAIL };
static int s_view = V_DAYS;
static int s_offset = 0;            // focused day, in days from today (0 = today)
static int s_sel = 0;              // selected row in the detail list
static cJSON *s_root = nullptr;    // parsed calendar.json (cached; reloaded periodically)
static int s_reload = 0;           // tick counter for lazy reload

struct Ev { char t[6]; char x[96]; };
#define MAX_EV 64
// The per-day event buffer (64*102 = 6.5 KB) is malloc'd while the Calendar app is OPEN and freed on
// leave(), so it does NOT sit in .bss starving the heap when the app is closed — that RAM goes to
// ANIMA (its L1 index + online worker) the moment you leave Calendar. Guarded: if the alloc fails the
// app simply shows no events (s_evn stays 0) instead of dereferencing NULL — never a crash.
static Ev *s_ev = nullptr;
static int s_evn = 0;

// ---- calendar.json (read-only here) ----------------------------------------
static void reload_root(void)
{
    if (s_root) { cJSON_Delete(s_root); s_root = nullptr; }
    FILE *f = fopen(CAL_PATH, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n > 0 && n < 256 * 1024) {
        char *buf = (char *)malloc(n + 1);
        if (buf) { size_t got = fread(buf, 1, n, f); buf[got] = 0; s_root = cJSON_Parse(buf); free(buf); }
    }
    fclose(f);
}

static cJSON *day_array(const char *key)
{
    if (!s_root) return nullptr;
    cJSON *evs = cJSON_GetObjectItem(s_root, "events");
    if (!cJSON_IsObject(evs)) return nullptr;
    cJSON *arr = cJSON_GetObjectItem(evs, key);
    return cJSON_IsArray(arr) ? arr : nullptr;
}
static int day_count(const char *key) { cJSON *a = day_array(key); return a ? cJSON_GetArraySize(a) : 0; }

// ---- date helpers ----------------------------------------------------------
// localtime for "today + off days", anchored at local noon so DST/edge shifts can't bump us
// into the wrong calendar day.
static struct tm day_tm(int off)
{
    time_t now = time(NULL);
    struct tm tm; localtime_r(&now, &tm);
    tm.tm_hour = 12; tm.tm_min = 0; tm.tm_sec = 0;
    time_t t = mktime(&tm) + (time_t)off * 86400;
    struct tm dd; localtime_r(&t, &dd);
    return dd;
}
static bool time_ready(void) { return time(NULL) >= 1672531200; }   // 2023-01-01: NTP has landed

static int ev_cmp(const void *a, const void *b) { return strcmp(((const Ev *)a)->t, ((const Ev *)b)->t); }

// Load every event of the focused day into s_ev, sorted by time.
static void load_day(int off)
{
    s_evn = 0; s_sel = 0;
    if (!s_ev) return;                                 // no buffer (alloc failed / app not entered) -> no events
    char key[12]; struct tm dd = day_tm(off); strftime(key, sizeof key, "%Y-%m-%d", &dd);
    cJSON *arr = day_array(key);
    if (!arr) return;
    cJSON *e;
    cJSON_ArrayForEach(e, arr) {
        if (s_evn >= MAX_EV) break;
        cJSON *t = cJSON_GetObjectItem(e, "time");
        cJSON *x = cJSON_GetObjectItem(e, "text");
        Ev *r = &s_ev[s_evn++];
        snprintf(r->t, sizeof r->t, "%s", cJSON_IsString(t) && t->valuestring[0] ? t->valuestring : "--:--");
        snprintf(r->x, sizeof r->x, "%s", cJSON_IsString(x) ? x->valuestring : "(event)");
    }
    qsort(s_ev, s_evn, sizeof(Ev), ev_cmp);
}

// ---- lifecycle -------------------------------------------------------------
static int s_last_min = -1;
static void set_view(int v)
{
    s_view = v;
    nucleo_app_set_hint(v == V_DETAIL ? TR("su/giu scorri   esc indietro", "up/dn scroll   esc back")
                                      : TR("</> o su/giu giorno   invio apri", "</> or up/dn day   enter open"));
}
static void enter(void)
{
    if (!s_ev) s_ev = (Ev *)malloc(sizeof(Ev) * MAX_EV);   // ~6.5 KB, only while the app is open
    reload_root();
    s_offset = 0; s_view = V_DAYS; s_reload = 0; s_last_min = -1;
    set_view(V_DAYS);
    nucleo_app_request_draw();
}
static void leave(void)
{
    if (s_root) { cJSON_Delete(s_root); s_root = nullptr; }
    if (s_ev)   { free(s_ev); s_ev = nullptr; s_evn = 0; }   // return the 6.5 KB to the heap for ANIMA
}

static void tick(void)
{
    bool need_draw = false;
    if (++s_reload >= 25) {           // ~5 s: pick up edits made from the web companion
        s_reload = 0; reload_root();
        if (s_view == V_DETAIL) load_day(s_offset);
        need_draw = true;
    }

    // Redraw only when the minute changes (clock in title) — no continuous redraws.
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_min != s_last_min) {
        s_last_min = tm_now.tm_min;
        need_draw = true;
    }

    if (need_draw) nucleo_app_request_draw();
}

static void on_key(int key, char ch)
{
    if (s_view == V_DAYS) {
        if (key == NK_LEFT || key == NK_UP)         { s_offset--; nucleo_app_request_draw(); }
        else if (key == NK_RIGHT || key == NK_DOWN) { s_offset++; nucleo_app_request_draw(); }
        else if (ch == 't')                         { s_offset = 0; nucleo_app_request_draw(); }   // jump to today
        else if (key == NK_ENTER)                   { load_day(s_offset); set_view(V_DETAIL); nucleo_app_request_draw(); }
        return;
    }
    // V_DETAIL
    if (key == NK_BACK || key == NK_DEL)            { set_view(V_DAYS); nucleo_app_request_draw(); }
    else if (app_ui_list_key(key, ch, &s_sel, s_evn,
             [](int i, void *) { return (const char *)s_ev[i].x; }, nullptr)) {
        nucleo_app_request_draw();
    }
}

// ---- drawing ---------------------------------------------------------------
static int dcenter(const char *s, int size) { return (240 - (int)strlen(s) * 6 * size) / 2; }

// one flanking (prev/next) day row: arrow + name + event count, dim.
static void draw_side(int yc, int off, bool below)
{
    struct tm dt = day_tm(off);
    char key[12]; strftime(key, sizeof key, "%Y-%m-%d", &dt);
    char name[24]; strftime(name, sizeof name, "%a %e %b", &dt);
    int n = day_count(key);

    d.setTextSize(1); d.setTextColor(DIM, BG);
    d.setCursor(10, yc - 3); d.print(below ? "v" : "^");
    d.setTextColor(MUTED, BG); d.setCursor(24, yc - 3); d.print(name);
    if (n > 0) {
        char r[16]; snprintf(r, sizeof r, "%d ev", n);
        d.setTextColor(ACC, BG); d.setCursor(238 - (int)strlen(r) * 6, yc - 3); d.print(r);
    } else {
        d.setTextColor(DIM, BG); d.setCursor(238 - 6, yc - 3); d.print("-");
    }
}

static void draw_days(void)
{
    struct tm focus = day_tm(s_offset);
    char hdr[20]; strftime(hdr, sizeof hdr, "%B %Y", &focus);
    char right[12];
    if (s_offset == 0) { time_t now = time(NULL); struct tm tn; localtime_r(&now, &tn); snprintf(right, sizeof right, "%02d:%02d", tn.tm_hour, tn.tm_min); }
    else snprintf(right, sizeof right, "%+dd", s_offset);
    int y0 = app_ui_title(hdr, ACC, time_ready() ? right : "no NTP");

    // ---- flanking previous day ----
    draw_side(y0 + 11, s_offset - 1, false);

    // ---- focused day card ----
    int cy = y0 + 24, ch = 50;
    d.fillRoundRect(6, cy, W - 12, ch, 8, s_offset == 0 ? ACC : LINE);
    unsigned short ink = s_offset == 0 ? INK : FG;

    char big[24]; strftime(big, sizeof big, "%a %e %b", &focus);
    d.setTextSize(2); d.setTextColor(ink, s_offset == 0 ? ACC : LINE);
    d.setCursor(14, cy + 5); d.print(big);
    if (s_offset == 0) { d.setTextSize(1); d.setCursor(W - 12 - 7 * 6, cy + 6); d.print("TODAY"); }

    // day-progress bar for the "dying" day (fraction of today already elapsed)
    if (s_offset == 0 && time_ready()) {
        time_t now = time(NULL); struct tm tn; localtime_r(&now, &tn);
        int secs = tn.tm_hour * 3600 + tn.tm_min * 60 + tn.tm_sec;
        int bw = (W - 28) * secs / 86400;
        d.fillRect(14, cy + 23, W - 28, 3, BG);
        d.fillRect(14, cy + 23, bw, 3, INK);
    }

    char key[12]; strftime(key, sizeof key, "%Y-%m-%d", &focus);
    cJSON *arr = day_array(key);
    int n = arr ? cJSON_GetArraySize(arr) : 0;
    d.setTextSize(1); d.setTextColor(ink, s_offset == 0 ? ACC : LINE);
    if (n == 0) {
        d.setCursor(14, cy + 32); d.print("No events");
    } else {
        // show the two earliest events; "+k more" if the day is busier.
        // IMPORTANT: do NOT use Ev tmp[MAX_EV] on the stack (= 6.5 KB → stack overflow → PANIC).
        // We only ever display 2 events, so we keep just the first 2 we find sorted by time.
        static Ev tmp[2]; int m = 0; cJSON *e;
        cJSON_ArrayForEach(e, arr) {
            if (m >= 2) { m++; continue; }   // count remaining for the "+k more" badge
            cJSON *t = cJSON_GetObjectItem(e, "time"); cJSON *x = cJSON_GetObjectItem(e, "text");
            snprintf(tmp[m].t, 6, "%s", cJSON_IsString(t) && t->valuestring[0] ? t->valuestring : "--:--");
            snprintf(tmp[m].x, sizeof tmp[m].x, "%s", cJSON_IsString(x) ? x->valuestring : "(event)");
            m++;
        }
        // Sort only the first min(m,2) entries we captured.
        int cap = m < 2 ? m : 2;
        if (cap == 2 && strcmp(tmp[0].t, tmp[1].t) > 0) { Ev sw = tmp[0]; tmp[0] = tmp[1]; tmp[1] = sw; }
        for (int i = 0; i < cap; i++) {
            char line[44]; snprintf(line, sizeof line, "%s %.30s", tmp[i].t, tmp[i].x);
            d.setCursor(14, cy + 30 + i * 10); d.print(line);
        }
        if (m > 2) { char more[16]; snprintf(more, sizeof more, "+%d more", m - 2);
            d.setCursor(W - 12 - (int)strlen(more) * 6, cy + 40); d.print(more); }
    }

    // ---- flanking next day ----
    draw_side(cy + ch + 13, s_offset + 1, true);
}

static const char *el_label(int i, void *) { return s_ev[i].x; }
static const char *el_right(int i, void *) { return s_ev[i].t; }
static unsigned short el_color(int i, void *) { (void)i; return ACC; }

static void draw_detail(void)
{
    struct tm dt = day_tm(s_offset);
    char title[24]; strftime(title, sizeof title, "%a %e %b", &dt);
    char cnt[16]; snprintf(cnt, sizeof cnt, "%d event%s", s_evn, s_evn == 1 ? "" : "s");
    int y0 = app_ui_title(title, ACC, cnt);

    if (s_evn == 0) {
        d.setTextSize(1); d.setTextColor(DIM, BG);
        d.setCursor(dcenter("No events this day", 1), y0 + 24); d.print("No events this day");
        return;
    }
    app_ui_list(y0, (H - HINT) - y0, s_evn, s_sel, el_label, el_right, el_color, nullptr);
}

static void draw(void)
{
    if (s_view == V_DETAIL) draw_detail(); else draw_days();
}

extern "C" void nucleo_register_calendar(void)
{
    static const nucleo_app_def_t app = {
        "calendar", "Calendar", "Office", "Day-focused agenda with reminders",
        'k', C_GREEN, enter, on_key, tick, draw, leave
    };
    nucleo_app_register(&app);
}
