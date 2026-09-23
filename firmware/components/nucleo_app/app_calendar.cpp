// Calendar app (native): a complete day agenda for the Cardputer — read, add, edit, delete.
//
// Home is the focused day's agenda in big type (today on open): ‹/› flip days, ↑/↓ (or 1-9) pick an
// event, Enter reads it in full, n adds, e edits, d deletes, m opens a month grid, TAB jumps to the
// next day that has events, t returns to today. On today the agenda pre-selects the NEXT event, dims
// the past ones and the header counts down to it. The add/edit form is keyboard-first: digits type the
// time ("930" -> 09:30; ↑/↓ ±15 min, ‹/› ±1 h, DEL = all day) and the title autocompletes from past
// events (TAB accepts). Every primary line is size-2 type; size 1 is only for secondary metadata.
//
// Reminders are NOT handled here: a device-wide background service (calendar_svc.cpp) owns detection +
// chime + the bus broadcast, and watches the file, so events added here chime too.
// Store: the SAME file the web Calendar writes — /sd/system/config/calendar.json,
// { "events": { "YYYY-MM-DD": [ {"id","time","text"} ] } } ("time" = "HH:MM" or "" for all day).
// Writes are a read-modify-write of the whole doc (temp + rename) and publish fs.changed, so an open
// web Calendar refreshes live.
#include "nucleo_app.h"
#include "app_ui.h"
#include <M5GFX.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include "cJSON.h"
#include "esp_random.h"
extern "C" {
#include "nucleo_board.h"
}

#include "launcher_theme.h"   // W/H + themed palette (BG, FG, MUTED, DIM, LINE, INK, C_*)
#include "app_gfx.h"          // the `d` draw-target redirect (NB: never name a local `d`)
#include "nucleo_i18n.h"      // TR(it,en) / TR5: hints + names follow the system language

extern "C" unsigned int nucleo_event_publish(const char *topic, const char *payload);

#define CAL_PATH NUCLEO_SD_MOUNT "/system/config/calendar.json"
#define CAL_TMP  CAL_PATH ".tmp"
static const unsigned short ACC = C_GREEN;
static const unsigned short ERR = C_RED;
#define BODY_Y 24                    // below the 24px header
#define BODY_H ((H - HINT) - BODY_Y)

// ---- state -----------------------------------------------------------------
enum { V_DAY, V_EVENT, V_MONTH, V_FORM };
static int  s_view = V_DAY;
static int  s_offset = 0;            // focused day, in days from today (0 = today)
static int  s_sel = 0;               // selected event (agenda row / reader page)
static int  s_moff = 0;              // month-grid cursor, days from today
static bool s_confirm, s_yes;        // delete confirm card up / its focused button
static int  s_reload = 0;            // tick counter for the change check
static int  s_last_min = -1;

static cJSON *s_root = nullptr;      // parsed calendar.json (kept while open; reparsed only on change)
static time_t s_mtime = 0;           // stamp of the file the tree came from
static long   s_fsize = -1;
static int    s_fail  = 0;           // consecutive parse failures (web write in flight, or RAM)
static bool   s_bad   = false;       // the file exists but no tree could be built: never pretend "no events"

struct Ev { char t[6]; char x[84]; short src; };   // t = "HH:MM" | "" (all day); x ASCII-folded; src = index in the day's array
#define MAX_EV 64
// The FOCUSED day's events, sorted (all-day first, then by time), sized to the day and rebuilt on every
// day change, so the agenda, the reader and the form always agree.
static Ev  *s_ev = nullptr;
static int  s_evn = 0;
static bool s_ev_oom = false;        // the day has events but its buffer didn't fit
static char s_ev_key[12] = "";       // which day s_ev holds (catches the midnight rollover)

// ---- add/edit form ----
enum { F_DAY, F_TIME, F_TEXT, F_N };
static int  s_field;
static int  s_form_from;             // view to return to
static bool s_editing;               // false = new event
static char s_edkey[12];             // edited event: its day key + index in that day's array
static int  s_edsrc;
static int  s_foff;                  // form day, days from today
static int  s_fmin;                  // form time in minutes, -1 = all day
static char s_tdig[5]; static int s_tdn;   // digits typed into the time field
static char s_ftext[81]; static int s_flen;
static bool s_ftext_dirty;           // text touched (else an edit keeps the original, possibly accented, text)
static char s_sugg[84];              // autocomplete candidate (full text) or ""
static const char *s_msg = nullptr;  // one-shot form error, shown in the title

// ---- localized names (ASCII: the TFT font has no accented glyphs) ------------
static int lang_col(void) { return TR5("0", "1", "2", "3", "4")[0] - '0'; }   // it,en,es,fr,de (en floor)
static const char *wday_short(int w)
{
    static const char *const N[5][7] = {
        { "Dom", "Lun", "Mar", "Mer", "Gio", "Ven", "Sab" },
        { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" },
        { "Dom", "Lun", "Mar", "Mie", "Jue", "Vie", "Sab" },
        { "Dim", "Lun", "Mar", "Mer", "Jeu", "Ven", "Sam" },
        { "So",  "Mo",  "Di",  "Mi",  "Do",  "Fr",  "Sa"  },
    };
    return N[lang_col()][(w % 7 + 7) % 7];
}
static const char *month_name(int m, bool full)
{
    static const char *const F[5][12] = {
        { "Gennaio", "Febbraio", "Marzo", "Aprile", "Maggio", "Giugno", "Luglio", "Agosto", "Settembre", "Ottobre", "Novembre", "Dicembre" },
        { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" },
        { "Enero", "Febrero", "Marzo", "Abril", "Mayo", "Junio", "Julio", "Agosto", "Septiembre", "Octubre", "Noviembre", "Diciembre" },
        { "Janvier", "Fevrier", "Mars", "Avril", "Mai", "Juin", "Juillet", "Aout", "Septembre", "Octobre", "Novembre", "Decembre" },
        { "Januar", "Februar", "Maerz", "April", "Mai", "Juni", "Juli", "August", "September", "Oktober", "November", "Dezember" },
    };
    static const char *const S[5][12] = {
        { "Gen", "Feb", "Mar", "Apr", "Mag", "Giu", "Lug", "Ago", "Set", "Ott", "Nov", "Dic" },
        { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" },
        { "Ene", "Feb", "Mar", "Abr", "May", "Jun", "Jul", "Ago", "Sep", "Oct", "Nov", "Dic" },
        { "Jan", "Fev", "Mar", "Avr", "Mai", "Jun", "Jul", "Aou", "Sep", "Oct", "Nov", "Dec" },
        { "Jan", "Feb", "Mrz", "Apr", "Mai", "Jun", "Jul", "Aug", "Sep", "Okt", "Nov", "Dez" },
    };
    m = (m % 12 + 12) % 12;
    return full ? F[lang_col()][m] : S[lang_col()][m];
}
static const char *all_day_label(void) { return TR5("Tutto il g.", "All day", "Todo el dia", "Journee", "Ganztags"); }

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
static void day_key(int off, char *key) { struct tm dd = day_tm(off); strftime(key, 12, "%Y-%m-%d", &dd); }
// "Mer 23 Set"; another year trades the weekday for it ("23 Set '27") so it stays <= 11 chars.
static void fmt_day(const struct tm &t, char *out, int cap)
{
    time_t n = time(NULL); struct tm now; localtime_r(&n, &now);
    if (t.tm_year != now.tm_year) snprintf(out, cap, "%d %s '%02d", t.tm_mday, month_name(t.tm_mon, false), t.tm_year % 100);
    else snprintf(out, cap, "%s %d %s", wday_short(t.tm_wday), t.tm_mday, month_name(t.tm_mon, false));
}
static bool time_ready(void) { return time(NULL) >= 1672531200; }   // 2023-01-01: NTP (or a manual set) has landed
static void now_hhmm(char *out) { time_t n = time(NULL); struct tm t; localtime_r(&n, &t); snprintf(out, 6, "%02d:%02d", t.tm_hour, t.tm_min); }
static int  now_min(void) { time_t n = time(NULL); struct tm t; localtime_r(&n, &t); return t.tm_hour * 60 + t.tm_min; }
// Days from today to a "YYYY-MM-DD" key.
static int key_offset(const char *key)
{
    struct tm k = {}; int y = 0, m = 0, dd = 0;
    if (sscanf(key, "%d-%d-%d", &y, &m, &dd) != 3) return 0;
    k.tm_year = y - 1900; k.tm_mon = m - 1; k.tm_mday = dd; k.tm_hour = 12; k.tm_isdst = -1;
    struct tm t0 = day_tm(0); t0.tm_isdst = -1;
    long diff = (long)(mktime(&k) - mktime(&t0));
    return (int)((diff + (diff >= 0 ? 43200 : -43200)) / 86400);
}

// ---- calendar.json ---------------------------------------------------------
static cJSON *parse_file(long n)
{
    if (n <= 0 || n >= 256 * 1024) return nullptr;
    FILE *f = fopen(CAL_PATH, "rb");
    if (!f) return nullptr;
    cJSON *r = nullptr;
    char *buf = (char *)malloc(n + 1);
    if (buf) { size_t got = fread(buf, 1, n, f); buf[got] = 0; r = cJSON_Parse(buf); free(buf); }
    fclose(f);
    return r;
}

// Reparse only when the file changed. A failed parse KEEPS the previous tree (the web companion may be
// mid-write) and leaves the stamp stale so the next check retries; only a second failure in a row drops
// it to retry with that RAM. Returns true when what's on screen may have changed.
static bool reload_root(void)
{
    struct stat st;
    if (stat(CAL_PATH, &st) != 0) {                         // no calendar yet: nothing to show, nothing wrong
        bool had = s_root || s_bad;
        if (s_root) { cJSON_Delete(s_root); s_root = nullptr; }
        s_mtime = 0; s_fsize = -1; s_fail = 0; s_bad = false;
        return had;
    }
    if (!s_fail && st.st_mtime == s_mtime && (long)st.st_size == s_fsize) return false;
    cJSON *nr = parse_file((long)st.st_size);
    bool dropped = false;
    if (!nr && s_root && s_fail >= 1) {                     // failed twice: maybe RAM -> retry without the old tree
        cJSON_Delete(s_root); s_root = nullptr; dropped = true;
        nr = parse_file((long)st.st_size);
    }
    if (!nr) {
        s_fail++;
        bool was_bad = s_bad; s_bad = (s_root == nullptr);
        return dropped || s_bad != was_bad;
    }
    if (s_root) cJSON_Delete(s_root);
    s_root = nr; s_mtime = st.st_mtime; s_fsize = (long)st.st_size; s_fail = 0; s_bad = false;
    return true;
}

static cJSON *events_obj(bool create)
{
    if (!s_root) {
        if (!create || s_bad) return nullptr;                // never rebuild over a file we couldn't read
        s_root = cJSON_CreateObject();
        if (!s_root) return nullptr;
    }
    cJSON *evs = cJSON_GetObjectItem(s_root, "events");
    if (!cJSON_IsObject(evs)) {
        if (!create) return nullptr;
        cJSON_DeleteItemFromObject(s_root, "events");
        evs = cJSON_AddObjectToObject(s_root, "events");
    }
    return evs;
}
static cJSON *day_array(const char *key)
{
    cJSON *evs = events_obj(false);
    cJSON *arr = evs ? cJSON_GetObjectItem(evs, key) : nullptr;
    return cJSON_IsArray(arr) ? arr : nullptr;
}
static int day_count(const char *key) { cJSON *a = day_array(key); return a ? cJSON_GetArraySize(a) : 0; }

// First day AFTER `key` that has events (keys are ISO dates: string order = date order), or "".
static void next_busy(const char *key, char *out)
{
    out[0] = 0;
    cJSON *evs = events_obj(false), *day;
    if (!evs) return;
    cJSON_ArrayForEach(day, evs) {
        if (!day->string || !cJSON_IsArray(day) || cJSON_GetArraySize(day) == 0) continue;
        if (strcmp(day->string, key) > 0 && (!out[0] || strcmp(day->string, out) < 0)) snprintf(out, 12, "%s", day->string);
    }
}

// Write the whole doc atomically-ish (temp + rename), then tell web clients.
static bool save_root(void)
{
    if (!s_root) return false;
    char *out = cJSON_PrintUnformatted(s_root);
    if (!out) return false;
    mkdir(NUCLEO_SD_MOUNT "/system", 0775);
    mkdir(NUCLEO_SD_MOUNT "/system/config", 0775);
    size_t len = strlen(out);
    FILE *f = fopen(CAL_TMP, "wb");
    bool ok = f && fwrite(out, 1, len, f) == len;
    if (f && fclose(f) != 0) ok = false;
    cJSON_free(out);
    if (!ok) { remove(CAL_TMP); return false; }
    remove(CAL_PATH);                                        // FATFS rename won't overwrite
    if (rename(CAL_TMP, CAL_PATH) != 0) return false;
    struct stat st;
    if (stat(CAL_PATH, &st) == 0) { s_mtime = st.st_mtime; s_fsize = (long)st.st_size; }   // our own write: no reparse
    s_fail = 0; s_bad = false;
    nucleo_event_publish("fs.changed", "{\"op\":\"write\",\"path\":\"/system/config/calendar.json\"}");
    return true;
}
// A failed save left the in-RAM tree ahead of the disk: re-read the disk so both agree again.
static void resync_from_disk(void) { s_fsize = -1; s_fail = 0; reload_root(); }

// ---- the focused day's events ------------------------------------------------
static bool valid_hhmm(const char *s)
{
    return s && isdigit((unsigned char)s[0]) && isdigit((unsigned char)s[1]) && s[2] == ':' &&
           isdigit((unsigned char)s[3]) && isdigit((unsigned char)s[4]) && !s[5];
}
static int ev_cmp(const void *a, const void *b)
{
    int c = strcmp(((const Ev *)a)->t, ((const Ev *)b)->t);             // "" (all day) first
    return c ? c : ((const Ev *)a)->src - ((const Ev *)b)->src;          // stable: insertion order
}
static void load_day(void)
{
    free(s_ev); s_ev = nullptr; s_evn = 0; s_ev_oom = false;
    day_key(s_offset, s_ev_key);
    cJSON *arr = day_array(s_ev_key);
    int n = arr ? cJSON_GetArraySize(arr) : 0;
    if (n > MAX_EV) n = MAX_EV;
    if (n <= 0) return;
    s_ev = (Ev *)malloc(sizeof(Ev) * n);
    if (!s_ev) { s_ev_oom = true; return; }
    cJSON *e; int idx = 0;
    cJSON_ArrayForEach(e, arr) {
        if (s_evn >= n) break;
        cJSON *t = cJSON_GetObjectItem(e, "time");
        cJSON *x = cJSON_GetObjectItem(e, "text");
        Ev *r = &s_ev[s_evn++];
        r->src = (short)idx++;
        snprintf(r->t, sizeof r->t, "%s", cJSON_IsString(t) && valid_hhmm(t->valuestring) ? t->valuestring : "");
        app_ui_ascii_fold(cJSON_IsString(x) ? x->valuestring : "", r->x, sizeof r->x);   // web text is UTF-8
        if (!r->x[0]) snprintf(r->x, sizeof r->x, "(...)");
    }
    qsort(s_ev, s_evn, sizeof(Ev), ev_cmp);
}
static int find_src(int src) { for (int i = 0; i < s_evn; i++) if (s_ev[i].src == src) return i; return 0; }

// Today's next event (time >= now), or -1 (not today / all past / only all-day left).
static int next_index(void)
{
    if (s_offset != 0) return -1;
    char now[6]; now_hhmm(now);
    for (int i = 0; i < s_evn; i++) if (s_ev[i].t[0] && strcmp(s_ev[i].t, now) >= 0) return i;
    return -1;
}
static bool is_past(int i)
{
    if (s_offset < 0) return true;
    if (s_offset > 0 || !s_ev[i].t[0]) return false;
    char now[6]; now_hhmm(now);
    return strcmp(s_ev[i].t, now) < 0;
}
static int default_sel(void) { int n = next_index(); return n >= 0 ? n : 0; }   // resume at what's next

static void fmt_countdown(const char *hhmm, char *out, int cap)
{
    int m = (atoi(hhmm) * 60 + atoi(hhmm + 3)) - now_min();
    const char *in = TR5("tra", "in", "en", "dans", "in");
    if (m <= 0)      snprintf(out, cap, "%s", TR5("ORA", "NOW", "AHORA", "MAINT.", "JETZT"));
    else if (m < 60) snprintf(out, cap, "%s %dm", in, m);
    else             snprintf(out, cap, "%s %dh%02d", in, m / 60, m % 60);
}

// ---- views + hints -----------------------------------------------------------
static void form_hint(void)
{
    if (s_field == F_DAY)       nucleo_app_set_hint(TR("</> giorno  su/giu settimana  TAB campo", "</> day  up/dn week  TAB field"));
    else if (s_field == F_TIME) nucleo_app_set_hint(TR("cifre o su/giu  CANC = tutto il g.", "digits or up/dn  DEL = all day"));
    else                        nucleo_app_set_hint(TR("TAB completa/campo  INVIO salva", "TAB complete/field  ENTER save"));
}
static void set_view(int v)
{
    s_view = v;
    if (v == V_FORM)        form_hint();
    else if (v == V_EVENT)  nucleo_app_set_hint(TR("su/giu evento  e modifica  d elimina", "up/dn event  e edit  d delete"));
    else if (v == V_MONTH)  nucleo_app_set_hint(TR("frecce giorno  INVIO apri  TAB pross.", "arrows day  ENTER open  TAB next"));
    else                    nucleo_app_set_hint(TR("</> giorno  n nuovo  m mese  d elimina", "</> day  n new  m month  d delete"));
}
static void goto_day(int off)
{
    s_offset = off;
    load_day();
    s_sel = default_sel();
    nucleo_app_request_draw();
}

// ---- form ------------------------------------------------------------------
static int default_time(void)
{
    if (s_offset != 0) return 9 * 60;
    int h = now_min() / 60 + 1;                              // today: the next full hour
    return h < 24 ? h * 60 : -1;
}
static void form_open(bool edit)
{
    s_form_from = s_view;
    s_editing = edit; s_field = F_TEXT; s_foff = s_offset;
    s_tdn = 0; s_tdig[0] = 0; s_sugg[0] = 0; s_msg = nullptr;
    if (edit && s_sel < s_evn) {
        const Ev *e = &s_ev[s_sel];
        s_fmin = e->t[0] ? atoi(e->t) * 60 + atoi(e->t + 3) : -1;
        snprintf(s_ftext, sizeof s_ftext, "%s", e->x); s_flen = (int)strlen(s_ftext);
        snprintf(s_edkey, sizeof s_edkey, "%s", s_ev_key); s_edsrc = e->src;
        s_ftext_dirty = false;
    } else {
        s_editing = false;
        s_fmin = default_time();
        s_ftext[0] = 0; s_flen = 0; s_ftext_dirty = true;
    }
    set_view(V_FORM);
    nucleo_app_request_draw();
}

// Autocomplete: the most recent past event whose title starts with what's typed (history as input).
static void update_sugg(void)
{
    s_sugg[0] = 0;
    if (s_flen < 2) return;
    cJSON *evs = events_obj(false), *day;
    if (!evs) return;
    char best[12] = "", f[84];
    cJSON_ArrayForEach(day, evs) {
        if (!day->string || !cJSON_IsArray(day) || strcmp(day->string, best) <= 0) continue;
        cJSON *e;
        cJSON_ArrayForEach(e, day) {
            cJSON *x = cJSON_GetObjectItem(e, "text");
            if (!cJSON_IsString(x)) continue;
            app_ui_ascii_fold(x->valuestring, f, sizeof f);
            if ((int)strlen(f) > s_flen && (int)strlen(f) < (int)sizeof s_ftext && !strncasecmp(f, s_ftext, s_flen)) {
                snprintf(best, sizeof best, "%s", day->string);
                snprintf(s_sugg, sizeof s_sugg, "%s", f);
                break;
            }
        }
    }
}
static void text_put(char c)
{
    if (s_flen < (int)sizeof s_ftext - 1) { s_ftext[s_flen++] = c; s_ftext[s_flen] = 0; s_ftext_dirty = true; }
    update_sugg();
}
static void time_digit(char c)
{
    if (s_tdn >= 4) s_tdn = 0;                               // a 5th digit starts a new time
    s_tdig[s_tdn++] = c; s_tdig[s_tdn] = 0;
    int v = atoi(s_tdig), h = s_tdn <= 2 ? v : v / 100, m = s_tdn <= 2 ? 0 : v % 100;   // "9" "14" "930" "1415"
    if (h < 24 && m < 60) s_fmin = h * 60 + m;
}
static void time_step(int delta)                            // ±15 snaps to the quarter; ±60 keeps minutes
{
    s_tdn = 0;
    int m = s_fmin < 0 ? (default_time() < 0 ? 12 * 60 : default_time()) : s_fmin;
    if (delta == 15)       m = (m / 15 + 1) * 15;
    else if (delta == -15) m = ((m + 14) / 15 - 1) * 15;
    else                   m += delta;
    s_fmin = ((m % 1440) + 1440) % 1440;
}
static void set_str(cJSON *o, const char *k, const char *v) { cJSON_DeleteItemFromObject(o, k); cJSON_AddStringToObject(o, k, v); }
static cJSON *day_array_mk(cJSON *evs, const char *key)
{
    cJSON *arr = cJSON_GetObjectItem(evs, key);
    if (!cJSON_IsArray(arr)) { cJSON_DeleteItemFromObject(evs, key); arr = cJSON_AddArrayToObject(evs, key); }
    return arr;
}
static void make_id(char *out, int cap)                      // same shape as the web: 'e' + base36(ms) + 4 random
{
    static const char B[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char tmp[16]; int n = 0, o = 0;
    unsigned long long v = (unsigned long long)time(NULL) * 1000ULL + (esp_random() % 1000);
    do { tmp[n++] = B[v % 36]; v /= 36; } while (v && n < (int)sizeof tmp);
    out[o++] = 'e';
    while (n && o < cap - 5) out[o++] = tmp[--n];
    uint32_t r = esp_random();
    for (int i = 0; i < 4 && o < cap - 1; i++) { out[o++] = B[r % 36]; r /= 36; }
    out[o] = 0;
}
static bool form_commit(void)
{
    if (s_ftext_dirty && s_flen == 0) { s_msg = TR5("Scrivi un titolo", "Type a title", "Escribe un titulo", "Ecris un titre", "Titel eingeben"); s_field = F_TEXT; form_hint(); return false; }
    cJSON *evs = events_obj(true);
    if (!evs) { s_msg = TR("File illeggibile", "File unreadable"); return false; }
    char key[12]; day_key(s_foff, key);
    char tbuf[6] = "";
    if (s_fmin >= 0) snprintf(tbuf, sizeof tbuf, "%02d:%02d", s_fmin / 60, s_fmin % 60);

    cJSON *obj = nullptr; int newsrc = 0;
    if (s_editing) {
        cJSON *old = cJSON_GetObjectItem(evs, s_edkey);
        obj = cJSON_IsArray(old) ? cJSON_GetArrayItem(old, s_edsrc) : nullptr;
        if (!obj) { s_msg = TR("Evento sparito", "Event gone"); return false; }
        newsrc = s_edsrc;
        if (strcmp(key, s_edkey) != 0) {                     // moved to another day: re-home it
            obj = cJSON_DetachItemFromArray(old, s_edsrc);
            if (cJSON_GetArraySize(old) == 0) cJSON_DeleteItemFromObject(evs, s_edkey);
            cJSON *dst = day_array_mk(evs, key);
            if (!dst) { cJSON_Delete(obj); resync_from_disk(); s_msg = TR("RAM insufficiente", "Not enough RAM"); return false; }
            newsrc = cJSON_GetArraySize(dst);
            cJSON_AddItemToArray(dst, obj);
        }
    } else {
        cJSON *dst = day_array_mk(evs, key);
        obj = cJSON_CreateObject();
        if (!dst || !obj) { cJSON_Delete(obj); resync_from_disk(); s_msg = TR("RAM insufficiente", "Not enough RAM"); return false; }
        char id[24]; make_id(id, sizeof id);
        cJSON_AddStringToObject(obj, "id", id);
        newsrc = cJSON_GetArraySize(dst);
        cJSON_AddItemToArray(dst, obj);
    }
    set_str(obj, "time", tbuf);
    if (s_ftext_dirty) set_str(obj, "text", s_ftext);        // untouched edit: keep the original (accented) text
    if (!save_root()) { resync_from_disk(); s_msg = TR5("Salvataggio fallito", "Save failed", "Error al guardar", "Echec sauvegarde", "Speichern fehlg."); return false; }

    s_offset = s_foff; load_day(); s_sel = find_src(newsrc);  // land on the saved event
    return true;
}

static bool delete_sel(void)
{
    if (s_sel < 0 || s_sel >= s_evn) return false;
    cJSON *evs = events_obj(false);
    cJSON *arr = evs ? cJSON_GetObjectItem(evs, s_ev_key) : nullptr;
    if (!cJSON_IsArray(arr)) return false;
    cJSON_DeleteItemFromArray(arr, s_ev[s_sel].src);
    if (cJSON_GetArraySize(arr) == 0) cJSON_DeleteItemFromObject(evs, s_ev_key);
    bool ok = save_root();
    if (!ok) resync_from_disk();
    int sel = s_sel; load_day();
    s_sel = sel < s_evn ? sel : (s_evn > 0 ? s_evn - 1 : 0);
    return ok;
}

// ---- lifecycle -------------------------------------------------------------
// The framework routes LEFT/BACK here (never to on_key) and closes the app unless we consume them.
static bool cal_back(int key)
{
    bool left = (key == NK_LEFT);
    if (s_confirm) { if (left) s_yes = !s_yes; else s_confirm = false; nucleo_app_request_draw(); return true; }
    if (s_view == V_FORM) {
        s_msg = nullptr;
        if (!left) { set_view(s_form_from); nucleo_app_request_draw(); return true; }   // Esc: cancel
        if (s_field == F_TEXT)      text_put(',');           // ',' is the LEFT key: in text it's a comma
        else if (s_field == F_DAY)  s_foff--;
        else                        time_step(-60);
        nucleo_app_request_draw(); return true;
    }
    if (s_view == V_MONTH) {
        if (left) s_moff--; else set_view(V_DAY);
        nucleo_app_request_draw(); return true;
    }
    if (s_view == V_EVENT)  { set_view(V_DAY); nucleo_app_request_draw(); return true; }
    if (left)               { goto_day(s_offset - 1); return true; }   // agenda: previous day
    return false;                                                         // BACK on the agenda -> close
}
static void on_key(int key, char ch);
// The framework never hands TAB to on_key: unclaimed, it toggles the Control Center. Claim it.
static void cal_tab(void) { on_key(NK_TAB, 0); }
static void enter(void)
{
    nucleo_app_set_back_handler(cal_back);
    nucleo_app_set_tab_handler(cal_tab);
    s_mtime = 0; s_fsize = -1; s_fail = 0; s_bad = false;
    reload_root();
    s_reload = 0; s_last_min = -1; s_confirm = false;
    goto_day(0);
    set_view(V_DAY);
}
static void leave(void)
{
    if (s_root) { cJSON_Delete(s_root); s_root = nullptr; }
    free(s_ev); s_ev = nullptr; s_evn = 0;                                // RAM back to the heap for ANIMA
}

static void tick(void)
{
    bool need_draw = false;
    // ~5 s: pick up edits made from the web companion — never under an open form/confirm, whose indices
    // point into the current tree.
    if (++s_reload >= 25 && s_view != V_FORM && !s_confirm) {
        s_reload = 0;
        if (reload_root()) {
            int sel = s_sel; load_day();
            s_sel = sel < s_evn ? sel : (s_evn > 0 ? s_evn - 1 : 0);
            if (s_view == V_EVENT && s_evn == 0) set_view(V_DAY);
            need_draw = true;
        }
    }
    // Minute change: clock, countdown, past-event dimming. Midnight also moves "today".
    time_t now = time(NULL);
    struct tm tm_now; localtime_r(&now, &tm_now);
    if (tm_now.tm_min != s_last_min) {
        s_last_min = tm_now.tm_min;
        char k[12]; day_key(s_offset, k);
        if (s_view != V_FORM && !s_confirm && strcmp(k, s_ev_key) != 0) { int sel = s_sel; load_day(); s_sel = sel < s_evn ? sel : 0; }
        need_draw = true;
    }
    if (need_draw) nucleo_app_request_draw();
}

static const char *el_label(int i, void *) { return s_ev[i].x; }
static const char *el_right(int i, void *) { return s_ev[i].t[0] ? s_ev[i].t : "--:--"; }
static unsigned short el_color(int i, void *) { return is_past(i) ? MUTED : ACC; }

static void on_key(int key, char ch)
{
    if (s_confirm) {
        int r = app_ui_confirm_key(key, ch, &s_yes);
        if (r == 1) { delete_sel(); if (s_view == V_EVENT && s_evn == 0) set_view(V_DAY); }
        if (r >= 0) s_confirm = false;
        nucleo_app_request_draw();
        return;
    }
    if (s_view == V_FORM) {
        s_msg = nullptr;
        if (key == NK_ENTER) { if (form_commit()) set_view(V_DAY); }
        else if (key == NK_TAB) {
            if (s_field == F_TEXT && s_sugg[0]) { snprintf(s_ftext, sizeof s_ftext, "%s", s_sugg); s_flen = (int)strlen(s_ftext); s_ftext_dirty = true; s_sugg[0] = 0; }
            else { s_field = (s_field + 1) % F_N; s_tdn = 0; form_hint(); }
        }
        else if (s_field == F_TEXT) {
            if (key == NK_DEL) { if (s_flen > 0) { s_ftext[--s_flen] = 0; s_ftext_dirty = true; } update_sugg(); }
            else if (ch >= 32 && ch < 127) text_put(ch);     // ';' '.' '/' arrive as arrows WITH their char
            else return;
        }
        else if (s_field == F_DAY) {
            if (key == NK_RIGHT)      s_foff++;
            else if (key == NK_UP)    s_foff -= 7;
            else if (key == NK_DOWN)  s_foff += 7;
            else if (ch == 't' || ch == 'T') s_foff = 0;
            else return;
        }
        else {                                               // F_TIME
            if (ch >= '0' && ch <= '9') time_digit(ch);
            else if (key == NK_UP)    time_step(15);
            else if (key == NK_DOWN)  time_step(-15);
            else if (key == NK_RIGHT) time_step(60);
            else if (key == NK_DEL)   { if (s_tdn > 0) { s_tdig[--s_tdn] = 0; if (s_tdn) { char c = s_tdig[s_tdn - 1]; s_tdn--; time_digit(c); } } else s_fmin = -1; }
            else return;
        }
        nucleo_app_request_draw();
        return;
    }
    if (s_view == V_MONTH) {
        if (key == NK_RIGHT)      s_moff++;
        else if (key == NK_UP)    s_moff -= 7;
        else if (key == NK_DOWN)  s_moff += 7;
        else if (ch == 't' || ch == 'T') s_moff = 0;
        else if (key == NK_TAB) { char k[12], nb[12]; day_key(s_moff, k); next_busy(k, nb); if (nb[0]) s_moff = key_offset(nb); }
        else if (key == NK_ENTER) { set_view(V_DAY); goto_day(s_moff); return; }
        else return;
        nucleo_app_request_draw();
        return;
    }
    if (s_view == V_EVENT) {                                 // reader: flip through the day's events
        if (key == NK_UP)        { if (s_sel > 0) s_sel--; }
        else if (key == NK_DOWN) { if (s_sel < s_evn - 1) s_sel++; }
        else if (key == NK_ENTER || ch == 'e' || ch == 'E') { form_open(true); return; }
        else if ((ch == 'd' || ch == 'D') && s_evn > 0) { s_confirm = true; s_yes = false; }
        else if (key == NK_DEL) set_view(V_DAY);
        else return;
        nucleo_app_request_draw();
        return;
    }
    // V_DAY (agenda)
    if (key == NK_RIGHT)                         goto_day(s_offset + 1);          // LEFT: cal_back
    else if (key == NK_ENTER)                    { if (s_evn > 0) { set_view(V_EVENT); nucleo_app_request_draw(); } else if (time_ready() && !s_bad) form_open(false); }
    else if (ch == 'n' || ch == 'N')             { if (time_ready()) form_open(false); }
    else if ((ch == 'e' || ch == 'E') && s_evn)  form_open(true);
    else if ((ch == 'd' || ch == 'D') && s_evn)  { s_confirm = true; s_yes = false; nucleo_app_request_draw(); }
    else if (ch == 't' || ch == 'T')             goto_day(0);
    else if (ch == 'm' || ch == 'M')             { s_moff = s_offset; set_view(V_MONTH); nucleo_app_request_draw(); }
    else if (key == NK_TAB) {                                                      // jump to the next busy day
        char nb[12]; next_busy(s_ev_key, nb);
        if (nb[0]) goto_day(key_offset(nb));
    }
    else if (key == NK_UP || key == NK_DOWN || (ch >= '1' && ch <= '9')) {         // no type-ahead: letters are commands
        if (app_ui_list_key(key, ch, &s_sel, s_evn, el_label, nullptr)) nucleo_app_request_draw();
    }
}

// ---- drawing ---------------------------------------------------------------
// Every view clears its own body: on the direct-draw path (no 32 KB canvas on a tight heap) the
// framework does NOT pre-clear.
static void clear_body(void) { d.fillRect(0, BODY_Y, W, BODY_H, BG); }
static void print_center(const char *s, int y, int size, unsigned short col)
{
    char b[24]; int maxc = 228 / (6 * size); snprintf(b, sizeof b, "%.*s", maxc < 23 ? maxc : 23, s);
    d.setTextSize(size); d.setTextColor(col, BG);
    d.setCursor((W - (int)strlen(b) * 6 * size) / 2, y); d.print(b);
}

// Agenda header: big date left; relative day + countdown/count right; the rule doubles as today's
// elapsed-time bar.
static void draw_day_header(void)
{
    d.fillRect(0, 0, W, BODY_Y, BG);
    struct tm t = day_tm(s_offset);
    char date[24];
    if (time_ready()) fmt_day(t, date, sizeof date); else snprintf(date, sizeof date, "--");
    d.setTextSize(2); d.setTextColor(s_offset == 0 ? ACC : FG, BG);
    d.setCursor(8, 3); d.print(date);

    char rel[16];
    if (s_offset == 0)       snprintf(rel, sizeof rel, "%s", TR5("OGGI", "TODAY", "HOY", "AUJ.", "HEUTE"));
    else if (s_offset == 1)  snprintf(rel, sizeof rel, "%s", TR5("DOMANI", "TOMORROW", "MANANA", "DEMAIN", "MORGEN"));
    else if (s_offset == -1) snprintf(rel, sizeof rel, "%s", TR5("IERI", "YESTERDAY", "AYER", "HIER", "GESTERN"));
    else                     snprintf(rel, sizeof rel, "%+d%s", s_offset, TR5("g", "d", "d", "j", "T"));
    char sub[16] = "";
    int nx = next_index();
    if (nx >= 0) fmt_countdown(s_ev[nx].t, sub, sizeof sub);
    else if (s_evn > 0) snprintf(sub, sizeof sub, "%d %s", s_evn, s_evn == 1 ? TR5("evento", "event", "evento", "evt", "Termin") : TR5("eventi", "events", "eventos", "evts", "Termine"));
    d.setTextSize(1);
    d.setTextColor(s_offset == 0 ? ACC : MUTED, BG); d.setCursor(W - 6 - (int)strlen(rel) * 6, 2);  d.print(rel);
    d.setTextColor(nx >= 0 ? FG : MUTED, BG);        d.setCursor(W - 6 - (int)strlen(sub) * 6, 12); d.print(sub);

    d.fillRect(0, 21, W, 2, LINE);
    if (s_offset == 0 && time_ready()) {
        time_t now = time(NULL); struct tm tn; localtime_r(&now, &tn);
        d.fillRect(0, 21, W * (tn.tm_hour * 3600 + tn.tm_min * 60 + tn.tm_sec) / 86400, 2, ACC);
    }
}

// Big, centered empty/error state: three size-2 lines (what, what to do, where to go).
static void empty_state(const char *l1, unsigned short c1, const char *l2, const char *l3)
{
    clear_body();
    if (l1) print_center(l1, BODY_Y + 12, 2, c1);
    if (l2) print_center(l2, BODY_Y + 38, 2, ACC);
    if (l3) print_center(l3, BODY_Y + 64, 2, MUTED);
}

static void draw_day(void)
{
    draw_day_header();
    if (!time_ready()) { empty_state(TR5("Ora non impostata", "Clock not set", "Hora no fijada", "Heure non reglee", "Uhr nicht gestellt"), ERR, TR("App WiFi > SYS", "WiFi app > SYS"), nullptr); return; }
    if (s_bad)         { empty_state(TR("File illeggibile", "File unreadable"), ERR, TR("Correggi dal web", "Fix it from the web"), nullptr); return; }
    if (s_ev_oom)      { empty_state(TR("RAM insufficiente", "Not enough RAM"), ERR, TR("Esci e riprova", "Exit and retry"), nullptr); return; }
    if (s_evn == 0) {
        char nb[12], line3[28] = "";
        next_busy(s_ev_key, nb);
        if (nb[0]) { struct tm t = day_tm(key_offset(nb)); char ds[20]; fmt_day(t, ds, sizeof ds); snprintf(line3, sizeof line3, "TAB %s", ds); }
        empty_state(TR5("Nessun evento", "No events", "Sin eventos", "Aucun evenement", "Keine Termine"), FG,
                    TR5("INVIO = nuovo", "ENTER = new", "INTRO = nuevo", "ENTREE = nouveau", "ENTER = neu"), line3[0] ? line3 : nullptr);
        return;
    }
    app_ui_list(BODY_Y, BODY_H, s_evn, s_sel, el_label, el_right, el_color, nullptr);   // fills its band
}

// Greedy word wrap of ASCII `s` into rows of <= cols chars; a word longer than a row is hard-split.
// Returns the rows needed (only the first maxr are stored).
static int wrap_text(const char *s, int cols, char (*rows)[40], int maxr)
{
    int n = 0;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        int len = (int)strlen(s), take = len <= cols ? len : cols;
        if (len > cols) { int sp = cols; while (sp > 0 && s[sp] != ' ') sp--; if (sp > 0) take = sp; }
        if (n < maxr) { memcpy(rows[n], s, take); rows[n][take] = 0; }
        n++; s += take;
    }
    return n;
}

// Full-text reader for one event, in big type.
static void draw_event(void)
{
    if (s_sel < 0 || s_sel >= s_evn) { set_view(V_DAY); draw_day(); return; }
    const Ev *e = &s_ev[s_sel];
    bool past = is_past(s_sel);
    char pos[12]; snprintf(pos, sizeof pos, "%d/%d", s_sel + 1, s_evn);
    app_ui_title(e->t[0] ? e->t : all_day_label(), past ? MUTED : ACC, pos);
    clear_body();

    static char rows[9][40];                                // static: keep the draw path off the small stack
    int n = wrap_text(e->x, 19, rows, 9);                   // size 2: 19 cols x 16 px rows
    int y = BODY_Y + 1, sz = 2, step = 16;                  // 6 size-2 rows fit exactly (25 + 96 = 121)
    if (n <= 5) {                                           // room for the date line under the title
        struct tm dt = day_tm(s_offset);
        char sub[40]; fmt_day(dt, sub, sizeof sub);
        if (s_offset == 0 && e->t[0] && !past) { char cd[16]; fmt_countdown(e->t, cd, sizeof cd); size_t l = strlen(sub); snprintf(sub + l, sizeof sub - l, "  %s", cd); }
        d.setTextSize(1); d.setTextColor(past ? DIM : MUTED, BG); d.setCursor(10, y); d.print(sub);
        y += 13;
    } else if (n > 6) { n = wrap_text(e->x, 38, rows, 9); sz = 1; step = 10; }   // only very long titles drop to size 1
    int maxr = (H - HINT - y) / step; if (n > maxr) n = maxr;
    d.setTextSize(sz); d.setTextColor(past ? MUTED : FG, BG);
    for (int i = 0; i < n; i++) { d.setCursor(10, y + i * step); d.print(rows[i]); }
}

// Month grid: size-2 day numbers, Monday first. Days with events in ACC, today outlined, cursor filled.
static void draw_month(void)
{
    struct tm cur = day_tm(s_moff);
    int y = cur.tm_year + 1900, m = cur.tm_mon;
    struct tm first = {}; first.tm_year = cur.tm_year; first.tm_mon = m; first.tm_mday = 1; first.tm_hour = 12; first.tm_isdst = -1;
    mktime(&first);
    struct tm last = {}; last.tm_year = cur.tm_year; last.tm_mon = m + 1; last.tm_mday = 0; last.tm_hour = 12; last.tm_isdst = -1;
    mktime(&last);
    int ndays = last.tm_mday, lead = (first.tm_wday + 6) % 7;
    struct tm today = day_tm(0);
    bool this_month = (today.tm_year == cur.tm_year && today.tm_mon == m);

    int total = 0; char key[12];
    for (int dd = 1; dd <= ndays; dd++) { snprintf(key, sizeof key, "%04d-%02d-%02d", y, m + 1, dd); total += day_count(key); }

    d.fillRect(0, 0, W, H - HINT, BG);
    char hdr[24]; snprintf(hdr, sizeof hdr, "%s %d", month_name(m, true), y);
    d.setTextSize(2); d.setTextColor(ACC, BG); d.setCursor(6, 0); d.print(hdr);
    char rt[16]; snprintf(rt, sizeof rt, "%d ev", total);
    d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(W - 4 - (int)strlen(rt) * 6, 4); d.print(rt);

    const int CW = 34, X0 = 1, Y0 = 25, RH = 16;
    for (int c = 0; c < 7; c++) {                            // weekday initials, Monday first
        const char *wn = wday_short((c + 1) % 7);
        d.setTextColor(c >= 5 ? DIM : MUTED, BG); d.setCursor(X0 + c * CW + (CW - 12) / 2, 16); d.write(wn[0]); d.write(wn[1]);
    }
    for (int dd = 1; dd <= ndays; dd++) {
        int cell = lead + dd - 1, cx = X0 + (cell % 7) * CW, cy = Y0 + (cell / 7) * RH;
        snprintf(key, sizeof key, "%04d-%02d-%02d", y, m + 1, dd);
        bool has = day_count(key) > 0, sel = (dd == cur.tm_mday), now = this_month && dd == today.tm_mday;
        unsigned short fg = has ? ACC : ((cell % 7) >= 5 ? MUTED : FG), bg = BG;
        if (sel)      { d.fillRoundRect(cx + 1, cy, CW - 2, RH, 4, ACC); fg = INK; bg = ACC; }
        else if (now) d.drawRoundRect(cx + 1, cy, CW - 2, RH, 4, ACC);
        char num[4]; snprintf(num, sizeof num, "%d", dd);
        d.setTextSize(2); d.setTextColor(fg, bg);
        d.setCursor(cx + (CW - (int)strlen(num) * 12) / 2, cy + 1); d.print(num);
    }
}

// Add/edit form: two value rows (day, time) and a 3-line text box, all size 2.
static void form_row(int y, int field, const char *label, const char *value)
{
    bool f = (s_field == field);
    d.fillRect(0, y, W, 21, BG);
    if (f) d.fillRoundRect(3, y, W - 6, 20, 5, ACC);
    d.setTextSize(2);
    d.setTextColor(f ? INK : MUTED, f ? ACC : BG); d.setCursor(9, y + 2);  d.print(label);
    d.setTextColor(f ? INK : FG,    f ? ACC : BG); d.setCursor(93, y + 2); d.print(value);
}
static void draw_form(void)
{
    const char *title = s_msg ? s_msg : (s_editing ? TR5("Modifica", "Edit event", "Editar", "Modifier", "Bearbeiten")
                                                   : TR5("Nuovo evento", "New event", "Nuevo evento", "Nouvel evt", "Neuer Termin"));
    app_ui_title(title, s_msg ? ERR : ACC, "");
    clear_body();

    struct tm fd = day_tm(s_foff); char ds[24]; fmt_day(fd, ds, sizeof ds);
    form_row(BODY_Y, F_DAY, TR5("Giorno", "Day", "Dia", "Jour", "Tag"), ds);
    char ts[16];
    if (s_fmin < 0) snprintf(ts, sizeof ts, "%s", all_day_label());
    else snprintf(ts, sizeof ts, "%02d:%02d", s_fmin / 60, s_fmin % 60);
    form_row(BODY_Y + 22, F_TIME, TR5("Ora", "Time", "Hora", "Heure", "Zeit"), ts);

    // text box: char-wrapped, the cursor line kept visible; the autocomplete tail drawn DIM after it
    const int BY = BODY_Y + 45, BH = (H - HINT) - BY, COLS = 18, LINES = 3;
    bool f = (s_field == F_TEXT);
    d.drawRoundRect(3, BY, W - 6, BH, 5, f ? ACC : LINE);
    const char *ghost = (f && s_sugg[0]) ? s_sugg + s_flen : "";
    int glen = (int)strlen(ghost), cur_line = s_flen / COLS;
    int first = cur_line - (LINES - 1); if (first < 0) first = 0;
    d.setTextSize(2);
    if (s_flen == 0 && !glen) {
        d.setTextColor(DIM, BG); d.setCursor(9, BY + 2); d.print(TR5("Titolo...", "Title...", "Titulo...", "Titre...", "Titel..."));
    }
    for (int l = first; l < first + LINES; l++) {
        for (int c = 0; c < COLS; c++) {
            int i = l * COLS + c;
            char ch = i < s_flen ? s_ftext[i] : (i - s_flen < glen ? ghost[i - s_flen] : 0);
            if (!ch) break;
            d.setTextColor(i < s_flen ? FG : DIM, BG);
            d.setCursor(9 + c * 12, BY + 2 + (l - first) * 16); d.write(ch);
        }
    }
    if (f) {                                                // cursor bar
        int cl = cur_line - first, cc = s_flen % COLS;
        d.fillRect(9 + cc * 12, BY + 2 + cl * 16 + 14, 11, 2, ACC);   // last row: 69+2+32+14 = 117 < 121
    }
}

static void draw(void)
{
    if (s_view == V_FORM)       draw_form();
    else if (s_view == V_MONTH) draw_month();
    else if (s_view == V_EVENT) draw_event();
    else                        draw_day();
    if (s_confirm && s_sel < s_evn)
        app_ui_confirm(TR5("Eliminare?", "Delete?", "Eliminar?", "Supprimer?", "Loeschen?"), s_ev[s_sel].x, s_yes);
}

extern "C" void nucleo_register_calendar(void)
{
    static const nucleo_app_def_t app = {
        "calendar", "Calendar", "Office", "Day agenda: add, edit, remind",
        'k', C_GREEN, enter, on_key, tick, draw, leave
    };
    nucleo_app_register(&app);
}
