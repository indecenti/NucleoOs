// ANIMA shell: an on-device chat with the offline assistant, right on the Cardputer.
// Type a line, Enter asks ANIMA, the answer scrolls in a transcript. Mirrors what the web
// client gets from GET /api/anima (nucleo_httpd.c): it runs the same nucleo_anima_query()
// and resolves the SYSTEM {value} templates + LAUNCH/TOOL actions locally (system values,
// create_file, set_volume/brightness, add_event calendar reminders, and app launching).
//
// Readability (smartwatch-style): the chat renders in a REAL anti-aliased proportional font
// (FreeSans, M5GFX's built-in GFX font) instead of the cramped 6x8 bitmap — the single biggest
// legibility win on this 240x135 panel. Messages are word-wrapped by PIXEL width and shown as
// chat bubbles: your questions right-aligned in blue, ANIMA's answers left with a colored rail,
// meta notes dim and small. "Compatto" in Settings swaps to the denser 16px Font2 for more lines.
// All visible strings are ASCII-folded on the way in so accented online text never shows tofu in
// the ASCII-only GFX font. A fresh/cleared chat shows a suggestion deck (su/giu + Invio) that both
// welcomes and showcases what ANIMA can do; TAB opens the IDEE tab — a drill-down catalog of every
// offline skill, where parametric entries (e.g. a multiplication) open a fill-in form for the values.
//
// Drawing: ANIMA frees the 32 KB shared canvas on enter (its index + worker need that RAM) and
// pins itself to DIRECT drawing, so it can't use the framework's off-screen composite. Per
// ANTI-FLICKER.md technique 2, draw() repaints only the region that changed (header badge /
// transcript / input); the caret blink toggles a single bar and the spinner repaints just the
// badge rect. The transcript is a word-wrapped row cache rebuilt from a small message ring only
// when the content changes (so toggling text size re-wraps cleanly without losing history).
//
// Why a worker task: nucleo_anima_query() can reach the online tiers (entity/live/teacher in
// docs/anima-online.md), which do a blocking HTTPS fetch — seconds, not microseconds. Calling it
// straight from the UI loop would freeze the launcher and trip the 8 s task watchdog. So the query
// runs on a side task and we poll the result in tick(), showing a "thinking" spinner.
//
// Keyboard note (matrix limitation, shared with Notes): ; . / type as themselves while writing,
// but with the input empty Up/Down scroll the transcript (or move the suggestion deck). The
// backtick (Esc/Back) and ',' (Left) are intercepted by the launcher loop and handed to on_back: in
// the menu Esc steps back a level and Left pages the tabs (the mirror of Right); from the chat base
// both leave the app — so a query can't contain a comma.
#include "nucleo_app.h"
#include "app_gfx.h"
#include <M5GFX.h>
#include "nucleo_anima.h"
#include "nucleo_tts.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"   // pet il task-WDT prima della scrittura SD del calendario (anti-reboot)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
extern "C" {
#include "nucleo_board.h"
#include "nucleo_storage.h"
#include "nucleo_registry.h"
#include "nucleo_audio.h"
#include "nucleo_exclusive.h"   // dedicated-mode RAM reclaim (~70KB, Wi-Fi stays) for the online-only cloud TLS
#include "cJSON.h"
// Live Wi-Fi state (resolved at link; same source the status API uses). No component dep:
// nucleo_setup REQUIRES nucleo_app, so a REQUIRES the other way would cycle — extern it.
const char *nucleo_setup_mode(void);
const char *nucleo_setup_ssid(void);
const char *nucleo_setup_ip(void);
// ANIMA online master switch (nucleo_anima_online.c) + online-only mode (nucleo_anima.c).
void nucleo_anima_set_online(bool on);
bool nucleo_anima_online_enabled(void);
void nucleo_anima_set_online_only(bool on);
// Compact-reply: while the NATIVE app is foreground (small screen), the cloud chat answers short and
// COMPLETE so the reply fits without the render clip cutting a sentence. On at enter, off at leave.
void nucleo_anima_set_compact_reply(bool on);
// Audio decoder: stop any background playback so its ~17-30 KB Helix decoder block returns to the
// heap the moment ANIMA opens — the assistant needs that RAM. Idempotent.
void nucleo_audio_stop(void);
// Aspetta che la voce/audio in corso finisca DA SOLA (no-op se niente suona, cap ms). Serializza le
// operazioni pesanti in RAM (es. la scrittura del calendario) con la riproduzione: niente picchi sommati.
void nucleo_audio_wait_idle(uint32_t max_ms);
// Event bus: publish a calendar.changed event after a native add_event write (so the web/calendar
// service refresh). Defined in nucleo_eventbus; linked into this component (calendar_svc uses it).
uint32_t nucleo_event_publish(const char *topic, const char *payload_json);
}

// Native app table read-access (C++ linkage; defined in nucleo_app.cpp) — used to resolve an app
// id ANIMA wants to open into its human name for the "Apro <name>..." line.
int                     nucleo_app_count(void);
const nucleo_app_def_t *nucleo_app_at(int i);

static const unsigned short BG = 0x0841, FG = 0xFFFF, MUTED = 0x8C71, DIM = 0x4410,
                            ACC = 0x929F /* ANIMA violet */, GRN = 0x8FF3, LINE = 0x2945,
                            INK = 0x0000, USR = 0x6E1F /* user echo blue */,
                            AMBER = 0xFD20,
                            SURF = 0x10A2 /* raised surface: slider track / value chip */,
                            CAP  = 0x1A8B /* focused settings-row capsule (Music/Video parity) */;

#define A_INMAX  140           // max input length

// ---- message ring (source of truth) -----------------------------------------
// Each message is one logical turn (a question, an answer, or a meta note). The transcript shown on
// screen is a word-wrapped ROW cache derived from this ring (rebuild_rows). Keeping the originals
// lets us re-wrap on a text-size/language change without losing history. All static (no heap): the
// assistant's heap is precious on this PSRAM-less chip, so the UI never allocates.
enum { R_META = 0, R_USER = 1, R_ANIMA = 2 };
#define MSG_MAX  16    // transcript history depth (~8 Q&A turns — ample on this screen). Trimmed from 22 to
                       // reclaim the .bss the wider MSG_TEXT below costs, so the online TLS heap budget stays fat.
#define MSG_TEXT 320   // holds a FULL offline answer: L1 cards (<=250) whole, and most MOSAICO L2 (join cap
                       // ~360). Sized DOWN from 384 to give the online TLS handshake back ~1 KB of heap
                       // margin — at 384 the idle free sat right on the NUCLEO_TLS_MIN_FREE gate and online
                       // turns flip-flopped by ~100 bytes after fragmentation. (Online replies are steered
                       // compact, well under this.) The rare >320 L2 answer clips at a sentence boundary.
typedef struct { char text[MSG_TEXT]; unsigned short col, accent; unsigned char role; } Msg;
static Msg s_msg[MSG_MAX];
static int s_mhead, s_mcount;

// Wrapped display rows (derived). A row points into a message's text (valid until the next
// rebuild, which every push triggers after writing the message).
enum { F_SMALL = 0 /*Font0 6x8*/, F_MED = 1 /*Font2 16px*/, F_BIG = 2 /*FreeSans9pt7b*/, F_BOLD = 3 /*FreeSansBold9pt7b*/ };
#define ROW_MAX 120
typedef struct { const char *p; unsigned short len, col, accent; unsigned char role, font, first; } Row;
static Row s_row[ROW_MAX];
static int s_rown;
static int s_scroll;            // rows scrolled up from the bottom (0 = newest)

// ---- dirty regions (flicker-free direct draw) -------------------------------
static bool s_d_hdr, s_d_body, s_d_input, s_d_badge;
static void mark_all_dirty(void) { s_d_hdr = s_d_body = s_d_input = true; }

// ---- input + last-answer state ----------------------------------------------
static char s_input[A_INMAX];
static int  s_ilen;
static int  s_last_conf;                         // confidence of the last answer (-1 = none)
static const char *s_last_tier = "";             // "L0"/"L1"/"web" label of the last answer
static char s_last_subject[48];                  // deductive focus, surfaced in the STATO tab
static int  s_blink, s_spin;
static bool s_user_sent;                          // false until the first question -> show the deck
static int  s_sug_sel;                            // focused suggestion in the deck / IDEE tab
static bool s_awaiting;                            // last answer was a follow-up question -> input placeholder hints it
static int  s_clock_min = -1;                      // header clock: last minute painted (repaint only when it changes)

// ---- command history (shell-style recall) -----------------------------------
// A small ring of the lines the user sent. Ctrl+Up walks back through them (Ctrl+Down forward),
// leaving plain Up/Down free to scroll the transcript. Session-only (.bss, reset on enter).
#define HIST_N   8
#define HIST_LEN 120
static char s_hist[HIST_N][HIST_LEN];
static int  s_hist_count;                           // entries stored (<= HIST_N)
static int  s_hist_head;                            // ring write index
static int  s_hist_nav = -1;                        // -1 = editing the live draft; 0 = newest, up = older
static char s_hist_draft[A_INMAX];                  // the in-progress line, parked while browsing history

// nav 0 = most recent. Returns NULL out of range.
static const char *hist_at(int nav)
{
    if (nav < 0 || nav >= s_hist_count) return NULL;
    return s_hist[(s_hist_head - 1 - nav + HIST_N * 2) % HIST_N];
}
static void hist_push(const char *s)
{
    if (!s || !s[0]) return;
    if (s_hist_count) { const char *last = hist_at(0); if (last && !strncmp(last, s, HIST_LEN - 1)) { s_hist_nav = -1; return; } }
    snprintf(s_hist[s_hist_head], HIST_LEN, "%s", s);
    s_hist_head = (s_hist_head + 1) % HIST_N;
    if (s_hist_count < HIST_N) s_hist_count++;
    s_hist_nav = -1;
}

// ---- recent field values (smartwatch: re-typing the same city/number is the common case) ----------
// A tiny ring of distinct values typed into IDEE form slots; the form ghosts the best prefix match so
// you accept a past value with one Right press instead of retyping it. Session-only (.bss).
#define RECENT_N 8
static char s_recent[RECENT_N][40];
static int  s_recent_count, s_recent_head;
static void recent_push(const char *v)
{
    if (!v || !v[0]) return;
    for (int i = 0; i < s_recent_count; i++)                       // skip if already remembered (any slot/order)
        if (!strcasecmp(s_recent[(s_recent_head - 1 - i + RECENT_N * 2) % RECENT_N], v)) return;
    snprintf(s_recent[s_recent_head], 40, "%s", v);
    s_recent_head = (s_recent_head + 1) % RECENT_N;
    if (s_recent_count < RECENT_N) s_recent_count++;
}
static bool slot_autocomplete(const char *pfx, char *out, int cap)
{
    int pl = (int)strlen(pfx);
    if (pl < 1) return false;
    for (int n = 0; n < s_recent_count; n++) {
        const char *r = s_recent[(s_recent_head - 1 - n + RECENT_N * 2) % RECENT_N];
        if ((int)strlen(r) > pl && !strncasecmp(r, pfx, pl)) { snprintf(out, cap, "%s", r); return true; }
    }
    return false;
}

// ---- settings (persisted to SD) ---------------------------------------------
// Online is 3-state: Off = offline only; On = hybrid (offline first, online on a miss); Only = use
// ONLY the online Grok model, skipping the offline cascade.
enum { OM_OFF = 0, OM_ON = 1, OM_ONLY = 2 };
static int  s_omode = OM_ON;
static bool s_big   = true;                       // chat text size: true = FreeSans (Grande), false = Font2 (Compatto)
static bool s_en    = false;                      // language: false = it, true = en
// TAB opens a full-screen tabbed MENU over the chat — the exact persistent tab-bar + carousel
// pattern the Music/Video apps use (draw_tabbar + draw_set_row). The chat is the base; the menu is
// a modal overlay (not a page in a ring). RIGHT cycles tabs; UP/DOWN walk the rows of the live tab;
// row -1 means the tab bar itself is focused (DOWN dives into the content, ESC closes). LEFT also
// cycles tabs (backward), so the two arrows page the carousel symmetrically.
enum { TAB_IDEE = 0, TAB_OGGI = 1, TAB_GUIDA = 2, TAB_IA = 3, TAB_STATO = 4 };
#define TAB_N 5
// IA (settings) tab rows — fixed order; SLIDER rows (Velocita voce/Volume/Luce) entrano in L/R adjust.
enum { IA_ONLINE = 0, IA_LANG, IA_TEXT, IA_VOICE, IA_SPEED, IA_VOL, IA_BRI, IA_CLEAR };
#define IA_ROWS 8
#define GUIDE_N 5                                 // pages in the GUIDA manual (also its "row" count)
static bool s_menu_open;                          // the tabbed menu is up (modal over the chat)
static int  s_tab;                                // active tab (TAB_*)
static int  s_mrow;                                // focused row: -1 = tab bar, 0..n-1 = content row
static bool s_edit;                               // a slider row (IA Volume/Luce) is in adjust mode

// ---- IDEE skill tree (smartwatch-style drill-down) --------------------------
// The IDEE tab is no longer a flat list: it's a two-level menu over a catalog of what ANIMA can do
// offline, grouped by skill. Level 0 = categories; ENTER drills into a category's leaves; a leaf
// either sends a ready prompt or — when it needs values (e.g. "Moltiplica" wants two numbers) —
// opens a tiny fill-in FORM so the device asks for each operand instead of guessing them. Esc/Left
// climb back up a level (form -> leaves -> categories -> tab bar), the watch-menu "back" gesture.
static int  s_idee_cat = -1;                       // -1 = category list; >=0 = inside that category
static int  s_form_leaf = -1;                       // >=0 = a fill-in form is open for LEAVES[s_form_leaf]
static int  s_form_slot;                            // which slot the form is collecting (0/1)
static char s_slot[2][40];                          // the values typed into the form's slots
// Context memory (the watch "resume where you were"): diving into IDEE lands on the last category you
// used, and opening a category pre-focuses the last leaf you picked there — so re-running a skill is a
// few presses. Session-only (.bss, reset on app enter); 9 categories so the per-cat array is tiny.
static int  s_focus_cat;                            // category the tab-bar dive lands on
static signed char s_focus_leaf[9];                 // last focused leaf row per category (CAT_N == 9)

// ---- full-screen text editor (file creation from IDEE) ----------------------
// A simple full-screen textarea: type freely, Enter = newline, DEL = backspace, Ctrl+S saves to the
// SD path collected by the "Crea file" form, Esc cancels. Append-only edit (caret at the end) — a true
// mid-text cursor is overkill on this keyboard; this matches "una semplice textarea". Static (.bss).
static bool s_ed_open;
static char s_ed_path[80];                          // absolute SD-relative path "/data/..." (from the form slot)
static char s_ed_buf[1024];                         // the file content being typed
static int  s_ed_len;
static int  s_ed_scroll;                            // wrapped-rows scrolled away above the viewport (0 = caret line visible)

// ---- calculator chain (continue from the last numeric answer, like a real calculator) -------
// The visible bubble stays exactly what the user typed; only the query sent to the engine is
// rewritten ("diviso 32" -> "2430 diviso 32"). Mirrors the web app's behind-the-scenes chaining.
static char s_last_num[40];                        // last numeric result (extracted from the answer)
static bool s_last_math;                           // last answer was a math intent -> a bare op continues it

// ---- Today/agenda tile + watch-face complications (read from the OS calendar) ----------------
#define TODAY_MAX 10
static char s_today[TODAY_MAX][72];                // formatted "HH:MM text" lines (.bss, no heap)
static int  s_today_n;                             // lines in s_today (today's events + the next upcoming)
static int  s_today_count;                         // number of events TODAY (for the STATO tab)
static char s_today_hdr[40];                       // "Oggi, lun 8 giu"
static char s_complics[80];                        // deck glance line: the next reminder (empty if none)

// ---- fonts / metrics --------------------------------------------------------
// The chat font is a real anti-aliased GFX font (FreeSans) — far more legible than the scaled 6x8.
// On the direct-draw path M5GFX blends the glyph alpha against the text BACKGROUND color we set
// (no panel read-back), so on our solid region backgrounds it stays crisp. Always pair fg+bg.
static void set_font(unsigned char f)
{
    if      (f == F_BIG)  d.setFont(&fonts::FreeSans9pt7b);
    else if (f == F_BOLD) d.setFont(&fonts::FreeSansBold9pt7b);
    else if (f == F_MED)  d.setFont(&fonts::Font2);
    else                  d.setFont(&fonts::Font0);
    d.setTextSize(1);
}
static unsigned char chat_font(void) { return s_big ? F_BIG : F_MED; }
static int  font_h(unsigned char f)  { return (f == F_BIG || f == F_BOLD) ? 18 : f == F_MED ? 15 : 11; }
static int  row_h(const Row *r)      { return font_h(r->font) + (r->first ? 3 : 0); }   // +gap before a new message
static int  input_h(void)            { return font_h(chat_font()) + 8; }
// Width of the first n bytes of s with the CURRENT font (textWidth needs a NUL-terminated string).
static int  meas(const char *s, int n) { char t[216]; if (n > 215) n = 215; memcpy(t, s, n); t[n] = 0; return (int)d.textWidth(t); }

// Fold UTF-8 (accents, smart quotes, dashes) to ASCII and drop anything else, so the ASCII-only
// GFX chat font never renders tofu boxes for online answers / accented city names. The offline
// corpus is already ASCII-folded, so this is mostly a safety net for the online tiers.
static void ascii_fold(const char *src, char *dst, int cap)
{
    int o = 0; const unsigned char *s = (const unsigned char *)src;
    while (*s && o < cap - 1) {
        unsigned char c = *s;
        if (c < 0x80) { dst[o++] = (char)c; s++; continue; }
        if (c == 0xC3 && s[1]) {                              // Latin-1 supplement (accented letters)
            char r = 0; unsigned char d2 = s[1];
            if      (d2 >= 0x80 && d2 <= 0x85) r = 'A'; else if (d2 >= 0xA0 && d2 <= 0xA5) r = 'a';
            else if (d2 >= 0x88 && d2 <= 0x8B) r = 'E'; else if (d2 >= 0xA8 && d2 <= 0xAB) r = 'e';
            else if (d2 >= 0x8C && d2 <= 0x8F) r = 'I'; else if (d2 >= 0xAC && d2 <= 0xAF) r = 'i';
            else if (d2 >= 0x92 && d2 <= 0x96) r = 'O'; else if (d2 >= 0xB2 && d2 <= 0xB6) r = 'o';
            else if (d2 >= 0x99 && d2 <= 0x9C) r = 'U'; else if (d2 >= 0xB9 && d2 <= 0xBC) r = 'u';
            else if (d2 == 0x87) r = 'C'; else if (d2 == 0xA7) r = 'c';
            else if (d2 == 0x91) r = 'N'; else if (d2 == 0xB1) r = 'n';
            else if (d2 == 0x97) r = 'x';                     // multiplication sign
            if (r) dst[o++] = r;
            s += 2; continue;
        }
        if (c == 0xE2 && s[1] == 0x80 && s[2]) {              // general punctuation
            unsigned char d3 = s[2];
            if      (d3 == 0x98 || d3 == 0x99) dst[o++] = '\'';
            else if (d3 == 0x9C || d3 == 0x9D) dst[o++] = '"';
            else if (d3 == 0x93 || d3 == 0x94) dst[o++] = '-';
            else if (d3 == 0xA6 && o < cap - 3) { dst[o++] = '.'; dst[o++] = '.'; dst[o++] = '.'; }
            s += 3; continue;
        }
        s++; while ((*s & 0xC0) == 0x80) s++;                 // unknown: skip the whole codepoint
    }
    dst[o] = 0;
}

// ---- row cache: word-wrap the message ring by pixel width --------------------
static void emit_row(const char *p, int len, unsigned short col, unsigned short acc,
                     unsigned char role, unsigned char font, unsigned char first)
{
    if (s_rown == ROW_MAX) { memmove(&s_row[0], &s_row[1], sizeof(Row) * (ROW_MAX - 1)); s_rown--; }
    Row *r = &s_row[s_rown++];
    r->p = p; r->len = (unsigned short)len; r->col = col; r->accent = acc;
    r->role = role; r->font = font; r->first = first;
}

// Greedy word-wrap one message into rows, measuring with its font. Honours '\n', hard-splits a word
// too long to ever fit, and guarantees at least one (possibly blank) row so the message stays visible.
static void wrap_msg(const Msg *m)
{
    // ANIMA answers use a BOLD face in big mode (stronger on this low-DPI panel); questions stay regular.
    unsigned char font = (m->role == R_META)  ? (unsigned char)F_SMALL
                       : (m->role == R_ANIMA) ? (s_big ? (unsigned char)F_BOLD : (unsigned char)F_MED)
                       :                        chat_font();
    set_font(font);
    // User rows are right-aligned with a min-x of 22, so their usable width is 232-22=210; ANIMA rows
    // start at x=11 (210..225 region) so 214. Wrapping must match the render budget or a full line clips.
    const int availw = (m->role == R_META) ? 224 : (m->role == R_USER) ? 210 : 214;
    const char *text = m->text;
    int before = s_rown, first = 1;
    if (!text[0]) { emit_row(text, 0, m->col, m->accent, m->role, font, 1); return; }
    const char *ls = text, *p = text;
    while (*p) {
        if (*p == '\n') { emit_row(ls, (int)(p - ls), m->col, m->accent, m->role, font, first); first = 0; p++; ls = p; continue; }
        if (*p == ' ' && p == ls) { p++; ls = p; continue; }       // drop leading spaces on a fresh line
        const char *we = p; while (*we == ' ') we++; while (*we && *we != ' ' && *we != '\n') we++;  // [p..we) = spaces + next word
        if (meas(ls, (int)(we - ls)) <= availw) { p = we; continue; }   // fits: extend the line, keep scanning
        if (p == ls) {                                              // single word longer than a line -> hard split
            const char *q = ls;
            while (q < we) {
                int take = 1; while (q + take <= we && meas(q, take) <= availw) take++; take--; if (take < 1) take = 1;
                emit_row(q, take, m->col, m->accent, m->role, font, first); first = 0; q += take;
            }
            ls = we; p = we;
        } else {                                                   // break before the overflowing word
            emit_row(ls, (int)(p - ls), m->col, m->accent, m->role, font, first); first = 0;
            while (*p == ' ') p++;
            ls = p;
        }
    }
    if (p > ls) emit_row(ls, (int)(p - ls), m->col, m->accent, m->role, font, first);
    if (s_rown == before) emit_row(text, 0, m->col, m->accent, m->role, font, 1);   // all-spaces -> keep a blank row
}

static void rebuild_rows(void)
{
    s_rown = 0;
    for (int i = 0; i < s_mcount; i++) { int idx = (s_mhead - s_mcount + i + MSG_MAX) % MSG_MAX; wrap_msg(&s_msg[idx]); }
    d.setFont(&fonts::Font0); d.setTextSize(1);   // leave the global font at the framework default
    s_scroll = 0; s_d_body = true;                // any new content snaps the view to the bottom
}

static void push_msg(unsigned char role, unsigned short col, unsigned short accent, const char *text)
{
    Msg *m = &s_msg[s_mhead];
    ascii_fold(text, m->text, MSG_TEXT);
    m->col = col; m->accent = accent; m->role = role;
    s_mhead = (s_mhead + 1) % MSG_MAX; if (s_mcount < MSG_MAX) s_mcount++;
    rebuild_rows();
}
static void push_meta(const char *t, unsigned short col) { push_msg(R_META, col, 0, t); }
static void push_user(const char *t)                     { push_msg(R_USER, USR, 0, t); }
static void push_anima(const char *t, unsigned short acc) { push_msg(R_ANIMA, FG, acc, t); }

// Push online down to the assistant: master switch on unless Off; online-only when Only.
static void apply_online_mode(void)
{
    nucleo_anima_set_online(s_omode != OM_OFF);
    nucleo_anima_set_online_only(s_omode == OM_ONLY);
    s_d_hdr = true;
}

#define SETTINGS_PATH NUCLEO_SD_MOUNT "/system/config/anima_ui.json"

static void load_settings(void)
{
    FILE *f = fopen(SETTINGS_PATH, "rb");
    if (f) {
        char buf[160]; int n = (int)fread(buf, 1, sizeof(buf) - 1, f); fclose(f);
        if (n > 0) {
            buf[n] = 0;
            cJSON *root = cJSON_Parse(buf);
            if (root) {
                cJSON *o = cJSON_GetObjectItem(root, "online"), *b = cJSON_GetObjectItem(root, "big"),
                      *l = cJSON_GetObjectItem(root, "lang");
                if (cJSON_IsString(o))      s_omode = !strcmp(o->valuestring, "off")  ? OM_OFF
                                                    : !strcmp(o->valuestring, "only") ? OM_ONLY : OM_ON;
                else if (cJSON_IsBool(o))   s_omode = cJSON_IsTrue(o) ? OM_ON : OM_OFF;   // legacy bool
                if (cJSON_IsBool(b)) s_big = cJSON_IsTrue(b);
                if (cJSON_IsString(l)) s_en = !strcmp(l->valuestring, "en");
                cJSON_Delete(root);
            }
        }
    }
    apply_online_mode();
}

static void save_settings(void)
{
    mkdir(NUCLEO_SD_MOUNT "/system", 0775);
    mkdir(NUCLEO_SD_MOUNT "/system/config", 0775);
    FILE *f = fopen(SETTINGS_PATH, "wb");
    if (!f) return;
    const char *om = s_omode == OM_OFF ? "off" : s_omode == OM_ONLY ? "only" : "on";
    fprintf(f, "{\"online\":\"%s\",\"big\":%s,\"lang\":\"%s\"}\n",
            om, s_big ? "true" : "false", s_en ? "en" : "it");
    fclose(f);
}

// ---- transcript persistence: show the last conversation on re-entry --------
// The chat ring is .bss (reset on enter); persist it to SD so reopening ANIMA restores where you left
// off. Binary + length-prefixed (messages may contain '\n'). Bounded by MSG_MAX. Best-effort: a failed
// read/write just yields a fresh chat. Saved at the end of each turn and on leave.
#define CHAT_PATH NUCLEO_SD_MOUNT "/system/config/anima_chat.dat"
static void save_chat(void)
{
    mkdir(NUCLEO_SD_MOUNT "/system", 0775);
    mkdir(NUCLEO_SD_MOUNT "/system/config", 0775);
    FILE *f = fopen(CHAT_PATH, "wb");
    if (!f) return;
    uint8_t cnt = (uint8_t)s_mcount;
    fwrite("ACH1", 1, 4, f); fwrite(&cnt, 1, 1, f);
    for (int i = 0; i < s_mcount; i++) {
        int idx = (s_mhead - s_mcount + i + MSG_MAX) % MSG_MAX;
        Msg *m = &s_msg[idx];
        uint16_t len = (uint16_t)strlen(m->text);
        fwrite(&m->role, 1, 1, f); fwrite(&m->col, 1, 2, f); fwrite(&m->accent, 1, 2, f);
        fwrite(&len, 1, 2, f); fwrite(m->text, 1, len, f);
    }
    fclose(f);
}
static void load_chat(void)
{
    s_mhead = s_mcount = 0;
    FILE *f = fopen(CHAT_PATH, "rb");
    if (!f) return;
    char magic[4]; uint8_t cnt = 0;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "ACH1", 4) != 0 || fread(&cnt, 1, 1, f) != 1) { fclose(f); return; }
    if (cnt > MSG_MAX) cnt = MSG_MAX;
    for (int i = 0; i < cnt; i++) {
        Msg *m = &s_msg[s_mhead];
        uint16_t len = 0;
        if (fread(&m->role, 1, 1, f) != 1 || fread(&m->col, 1, 2, f) != 2 ||
            fread(&m->accent, 1, 2, f) != 2 || fread(&len, 1, 2, f) != 2) break;
        if (len >= MSG_TEXT) len = MSG_TEXT - 1;
        if (fread(m->text, 1, len, f) != len) break;
        m->text[len] = 0;
        s_mhead = (s_mhead + 1) % MSG_MAX; if (s_mcount < MSG_MAX) s_mcount++;
    }
    fclose(f);
    // Separatore visivo in TESTA alla chat ripristinata: "── ripresa GG/MM HH:MM ──".
    // Inserisce nel ring PRIMA del primo messaggio (slot libero a sinistra della testa logica);
    // s_en e' gia' caricato da load_settings() che precede load_chat() in enter().
    if (s_mcount > 0 && s_mcount < MSG_MAX) {
        char sep[48];
        struct stat st;
        if (stat(CHAT_PATH, &st) == 0) {
            struct tm *tm = localtime(&st.st_mtime);
            if (tm) snprintf(sep, sizeof sep, s_en ? "-- session %02d/%02d %02d:%02d --"
                                                    : "-- ripresa %02d/%02d %02d:%02d --",
                             tm->tm_mday, tm->tm_mon + 1, tm->tm_hour, tm->tm_min);
            else    snprintf(sep, sizeof sep, s_en ? "-- restored --" : "-- ripresa --");
        } else {
            snprintf(sep, sizeof sep, s_en ? "-- restored --" : "-- ripresa --");
        }
        // Slot libero prima della testa logica: (mhead - mcount - 1) % MSG_MAX
        int slot = (s_mhead - s_mcount - 1 + MSG_MAX * 2) % MSG_MAX;
        Msg *m = &s_msg[slot];
        m->role = R_META; m->col = DIM; m->accent = 0;
        snprintf(m->text, MSG_TEXT, "%s", sep);
        s_mcount++;   // il ring ora include il meta come messaggio piu' vecchio
    }
}

// ---- worker task: runs the (possibly blocking) query off the UI loop --------
static TaskHandle_t      s_worker;
static volatile uint32_t s_worker_epoch;         // bumped by stop_worker: a worker with a stale epoch
                                                 // self-deletes instead of being vTaskDelete'd mid-query
static volatile bool     s_busy, s_done;
static volatile uint32_t s_gen;                  // bumped per request AND on (re)enter; a result whose
static volatile uint32_t s_done_gen;             // s_done_gen != s_gen is stale -> dropped
static char           s_req[A_INMAX];
static anima_result_t s_res;
static char           s_pending_launch[24];      // native app id to open once the answer is shown
static char           s_launch_web[24];          // the web id ANIMA returned (for the "web only" note)
static int            s_launch_wait;             // >0 = a launch is queued; tick() defers it this many ticks so
                                                 // ANIMA's RAM (worker stack + L1) frees before the target app loads
static const char    *ATAG = "anima.app";

// Cross-app seed: another native app (e.g. the Wi-Fi app's "Diagnostica con ANIMA") stashes a question
// here, then nucleo_app_launch_id("anima") opens us; enter() auto-submits it once the worker is ready.
// This routes the diagnostic THROUGH ANIMA, so it transparently uses the online tier when available.
static char           s_preset[A_INMAX];
extern "C" void nucleo_anima_app_ask(const char *q) { if (q) snprintf(s_preset, sizeof s_preset, "%s", q); }

// Voce on-device: pronuncia la risposta, MA non la conoscenza (tier remoto/L1/MOSAICO) ne' la
// calcolatrice (intent "calc") -> per quelle suona "leggila sullo schermo". Le risposte non
// interamente coperte da clip diventano comunque "leggila" dentro nucleo_tts_say().
static void fill_system_value(const char *arg, char *out, size_t n, bool en);   // definita piu' sotto

static void speak_result(const anima_result_t &r, bool en)
{
    if (!r.reply[0]) return;
    // LAUNCH e TOOL li vocalizza present_result: conosce il nome NATIVO dell'app (non l'id "media-player"
    // grezzo, che non e' coperto -> "leggila") e l'ESITO dell'operazione (-> conferma "Fatto"). Qui niente,
    // cosi' non si doppia la voce ne' si legge un id come fosse parlato.
    if (r.action == ANIMA_ACT_LAUNCH || r.action == ANIMA_ACT_TOOL) return;
    const char *lang = en ? "en" : "it";
    bool knowledge = r.tier == ANIMA_TIER_FACT || r.tier == ANIMA_TIER_STITCH || r.tier == ANIMA_TIER_REMOTE;
    if (knowledge) {
        // Voce a mosaico: invece di "leggila" prova a dire il GIST BREVE (la prima frase, di solito la
        // definizione). say() la pronuncia se il pool la copre, altrimenti ripiega su "leggila": cosi' i
        // topic a parole comuni vengono detti brevi, i nomi/entita' rari (non coperti) restano letti.
        char gist[160]; nucleo_tts_first_sentence(r.reply, gist, sizeof gist);
        nucleo_tts_say(gist, lang);
        return;
    }
    // Il CALCOLO non e' piu' instradato a "leggila": nucleo_tts_say() ora "parlabilizza" = % ^ (mathspeak),
    // cosi' "Fa 16", "Il 20% di 150 = 30", "5^3 = 125" si pronunciano; cio' che resta scoperto (geometria
    // simbolo-densa, numeri romani) cade comunque in "leggila" dentro say(). Era il bug "Fa 16 muto".

    // TRADUTTORE — voce BILINGUE in due tempi: la CORNICE "cane in inglese" (pausa) poi la TRADUZIONE "dog".
    // La reply e' `"<src>" in <lingua>: <tgt>[, sinonimi].` -> la cornice = tutto prima del ": " (virgolette
    // tolte), detta nella lingua UI quando la sorgente E' nella lingua UI (caso comune: parola italiana in
    // modo IT -> "cane in inglese" tutto coperto dall'indice IT). Se la sorgente e' STRANIERA (parola inglese
    // in modo IT) la cornice "in italiano" mischierebbe le lingue: ripiego sulla sola parola sorgente nella
    // sua lingua ("dog" -> "cane"). La pausa e' il gap del wait_idle. I due render usano indici mono-lingua.
    if (!strcmp(r.intent, "translate")) {
        char tw[80], tl[8];
        if (nucleo_tts_translate_word(r.reply, tw, sizeof tw, tl, sizeof tl)) {
            char *cm = strchr(tw, ','); if (cm) *cm = 0;          // solo la prima traduzione (non l'elenco sinonimi)
            char *tgt = tw; while (*tgt == ' ') tgt++;
            const char *src_lang = (tl[0] == 'e' && tl[1] == 'n') ? "it" : "en";   // sorgente = lingua opposta al target
            char p1[120]; int o = 0; const char *p1lang;
            if (!strcmp(src_lang, en ? "en" : "it")) {            // sorgente nella lingua UI -> cornice intera
                const char *colon = strstr(r.reply, ": ");
                for (const char *p = r.reply; *p && (!colon || p < colon) && o < (int)sizeof(p1) - 1; p++)
                    if (*p != '"') p1[o++] = *p;                  // "cane" in inglese -> cane in inglese
                p1lang = lang;
            } else {                                              // sorgente straniera -> solo la parola, sua lingua
                const char *q1 = strchr(r.reply, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
                if (q1 && q2) for (const char *p = q1 + 1; p < q2 && o < (int)sizeof(p1) - 1; p++) p1[o++] = *p;
                p1lang = src_lang;
            }
            p1[o] = 0;
            if (p1[0]) { nucleo_tts_say(p1, p1lang); nucleo_audio_wait_idle(2500); }
            nucleo_tts_say(tgt, tl);
            return;
        }
    }

    // Risolvi il template {value} (stato: ora/batteria/data/spazio/...) PRIMA di parlare. Senza, si
    // vocalizzerebbe il template GREZZO "{value}." e le graffe farebbero scattare la guardia "sa di
    // codice" -> "leggila", mentre lo schermo (che sostituisce in present_result) mostra il valore
    // giusto: era questo il "ora a schermo ma non la pronuncia". Stessa sostituzione dello schermo.
    const char *ph = (r.action == ANIMA_ACT_SYSTEM) ? strstr(r.reply, "{value}") : NULL;
    if (ph) {
        char value[384]; fill_system_value(r.arg, value, sizeof value, en);
        char spoken[416];
        snprintf(spoken, sizeof spoken, "%.*s%s%s", (int)(ph - r.reply), r.reply, value, ph + 7);
        if (!strcmp(r.arg, "agenda")) {
            // L'elenco eventi (orari/testi) e' variabile -> se l'intero non e' pronunciabile, di' almeno
            // il CONTEGGIO ("oggi hai 3 impegni"), troncando ai due punti. Conteggio detto > "leggila".
            char count[80]; snprintf(count, sizeof count, "%s", spoken);
            char *colon = strchr(count, ':'); if (colon) *colon = 0;
            nucleo_tts_say_or(spoken, count, lang);
        } else {
            nucleo_tts_say(spoken, lang);
        }
    } else if (nucleo_tts_has_mathtypo(r.reply)) {
        // FORMULA densa (geometria/fisica: "Area = π·5² = 78.5398") -> la voce non sa dire i simboli; se
        // c'e' un RISULTATO numerico pulito dopo l'ultimo "=", dillo ("Il risultato e' 78.5398") invece
        // di "leggila". Altrimenti (formula senza numero, es. "A = π·r²") -> say normale -> "leggila".
        char res[48];
        if (nucleo_tts_eq_result(r.reply, res, sizeof res)) {
            char spoken[80]; snprintf(spoken, sizeof spoken, en ? "The result is %s." : "Il risultato e' %s.", res);
            nucleo_tts_say(spoken, lang);
        } else nucleo_tts_say(r.reply, lang);
    } else {
        nucleo_tts_say(r.reply, lang);
    }
}

static void anima_worker(void *arg)
{
    const uint32_t epoch = (uint32_t)(uintptr_t)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (epoch != s_worker_epoch) break;          // orphaned by stop_worker (idle, or drained after busy)
        uint32_t g = s_gen;
        char req[A_INMAX]; snprintf(req, sizeof(req), "%s", s_req);
        ESP_LOGI(ATAG, "query START q='%s' gen=%u omode=%d heap=%u largest=%u", req, (unsigned)g, s_omode,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        // ONLINE-ONLY: the cloud TLS handshake needs a large CONTIGUOUS block this PSRAM-less chip can't
        // spare while httpd/L1/mDNS/voice hold heap — it OOMs (~24 KB peak vs ~16 KB largest, measured) and
        // "the online model doesn't answer". Reclaim ~70 KB the SAME way the recorder's ai_task does
        // (nucleo_exclusive_enter; Wi-Fi STA stays) so the cloud ACTUALLY responds. Entered BEFORE the spine
        // lock: httpd_stop()'s task-join must NOT run while we hold the non-recursive lock that a web handler
        // also wants (that deadlocks). With httpd down the spine lock is then uncontended. Hybrid/offline keep
        // their heap (L1 answers there); only the pure-cloud turn pays the window. The query's own online-only
        // branch still falls back to a labelled offline answer if the cloud misses even with the freed heap.
        bool nx = false;
        if (s_omode == OM_ONLY && nucleo_anima_online_available()) {
            nucleo_exclusive_info_t inf;
            nx = nucleo_exclusive_enter(NX_NET_APP, &inf);
            ESP_LOGI(ATAG, "online-only exclusive=%d post-reclaim free=%u largest=%u",
                     (int)nx, (unsigned)inf.free_after, (unsigned)inf.largest_after);
        }
        // Spine gate: wait (poll) up to ~8s for the cascade to be free — a concurrent web /api/anima
        // holds it only briefly. If still busy, answer "busy" rather than racing the shared L1 state.
        bool a_locked = false;
        for (int i = 0; i < 400 && !(a_locked = nucleo_anima_try_lock()); i++) vTaskDelay(pdMS_TO_TICKS(20));
        anima_result_t r;
        if (!a_locked) {
            memset(&r, 0, sizeof r); r.tier = ANIMA_TIER_NONE; r.action = ANIMA_ACT_NONE;
            snprintf(r.reply, sizeof r.reply, s_en ? "Busy — try again in a moment." : "Occupato — riprova tra un istante.");
        } else {
            r = nucleo_anima_query(req, s_en ? "en" : "it");
            nucleo_anima_unlock();
        }
        if (nx) nucleo_exclusive_exit();   // restore httpd/L1/mDNS/voice — runs on busy AND answered paths, before any epoch-break below
        ESP_LOGI(ATAG, "query DONE gen=%u tier=%d action=%d conf=%d reply='%.48s'",
                 (unsigned)g, (int)r.tier, (int)r.action, r.confidence, r.reply);
        if (epoch != s_worker_epoch) break;          // orphaned mid-query: the app state belongs to a
                                                     // newer session now — exit without touching it
        // Pubblica PRIMA il risultato (UI puo' mostrarlo subito), poi parla. speak_result() e' SINCRONO e
        // SD/audio-pesante (nucleo_audio_stop aspetta fino a ~4.5s + assembla _say.wav): tenuto prima di
        // s_done teneva la app su "busy" per tutto il render -> sembrava freezata (tipico sul "ricorda",
        // la cui conferma ripiega quasi sempre su read_it = un secondo render). L'audio resta async.
        s_res     = r;
        s_done_gen = g;
        __sync_synchronize();
        s_done = true;
        if (g == s_gen) s_busy = false;   // a /stop or a newer request already moved on -> don't clear its busy flag
        // Parla la risposta on-device (no-op se la voce e' disattiva). Questo e' il path del Cardputer;
        // il path web passa da nucleo_httpd, quindi resta escluso (usa speechSynthesis del browser).
        if (g == s_gen) speak_result(r, s_en);
    }
    vTaskDelete(NULL);
}

static void stop_worker(void)
{
    if (!s_worker) return;
    // NEVER vTaskDelete a worker that may be mid-online-query: it can hold the ANIMA spine lock and an
    // arb token (HTTP timeout up to 30 s) — deleting it leaked both until reboot, plus the HTTP client
    // heap. Orphan it instead: bump the epoch and wake it; an idle worker exits immediately, a busy one
    // finishes its query (releasing locks normally), skips the stale state writes, and self-deletes.
    s_worker_epoch = s_worker_epoch + 1;
    __sync_synchronize();
    xTaskNotifyGive(s_worker);
    s_worker = nullptr; s_busy = false; s_done = false;
}

// ---- live SYSTEM value resolver (mirrors anima_get() in nucleo_httpd.c) ------
// BILINGUE: i valori (giorni/mesi/stagioni/ora/spazio/uptime/agenda) escono nella lingua della
// sessione (en). Senza, in modalita' inglese uscivano in italiano e la voce EN non li copriva
// -> "leggila" (era il "l'inglese non risponde all'ora"). I template wrapper ({value}) li sceglie
// gia' INTENTS[] per lingua; qui produciamo il VALORE coerente. Uptime per esteso (no "2g 3h": la
// voce direbbe le lettere) -> stesso testo a schermo e a voce.
static void fill_system_value(const char *arg, char *out, size_t n, bool en)
{
    snprintf(out, n, en ? "not available" : "non disponibile");
    time_t now = time(NULL); struct tm *tm = localtime(&now);
    if (!strcmp(arg, "time")) {
        if (tm && now > 1672531200) nucleo_tts_speak_time(out, (int)n, tm->tm_hour, tm->tm_min, en ? "en" : "it");
        else                        snprintf(out, n, en ? "I don't know the time: the clock isn't set" : "Non conosco l'ora: l'orologio non e' impostato");
    } else if (!strcmp(arg, "storage")) {
        nucleo_storage_refresh();
        const nucleo_storage_info_t *st = nucleo_storage_info();
        if (st->mounted) snprintf(out, n, en ? "%.1f GB free of %.1f GB" : "%.1f GB liberi su %.1f GB", st->free_bytes / 1e9, st->total_bytes / 1e9);
    } else if (!strcmp(arg, "date") && tm) {
        static const char *WD_IT[] = {"domenica","lunedi","martedi","mercoledi","giovedi","venerdi","sabato"};
        static const char *MO_IT[] = {"gennaio","febbraio","marzo","aprile","maggio","giugno","luglio","agosto","settembre","ottobre","novembre","dicembre"};
        static const char *WD_EN[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
        static const char *MO_EN[] = {"January","February","March","April","May","June","July","August","September","October","November","December"};
        if (en) snprintf(out, n, "Today is %s %d %s %d", WD_EN[tm->tm_wday], tm->tm_mday, MO_EN[tm->tm_mon], tm->tm_year + 1900);
        else    snprintf(out, n, "Oggi e %s %d %s %d", WD_IT[tm->tm_wday], tm->tm_mday, MO_IT[tm->tm_mon], tm->tm_year + 1900);
    } else if (!strcmp(arg, "year") && tm) {
        snprintf(out, n, "%d", tm->tm_year + 1900);
    } else if (!strcmp(arg, "season") && tm) {
        static const char *SE_IT[] = {"inverno","primavera","estate","autunno"};   // astronomical, N. hemisphere
        static const char *SE_EN[] = {"winter","spring","summer","autumn"};
        int mo = tm->tm_mon, dy = tm->tm_mday;
        int s = ((mo == 2 && dy >= 20) || mo == 3 || mo == 4 || (mo == 5 && dy <= 20)) ? 1
              : ((mo == 5 && dy >= 21) || mo == 6 || mo == 7 || (mo == 8 && dy <= 22)) ? 2
              : ((mo == 8 && dy >= 23) || mo == 9 || mo == 10 || (mo == 11 && dy <= 20)) ? 3 : 0;
        snprintf(out, n, "%s", en ? SE_EN[s] : SE_IT[s]);
    } else if (!strcmp(arg, "version")) {
        snprintf(out, n, "NucleoOS 0.1.0");
    } else if (!strcmp(arg, "uptime")) {
        long s = (long)(esp_timer_get_time() / 1000000);
        int dd = (int)(s / 86400), hh = (int)((s % 86400) / 3600), mm = (int)((s % 3600) / 60);
        if (dd)      snprintf(out, n, en ? "%d days and %d hours"    : "%d giorni e %d ore",  dd, hh);
        else if (hh) snprintf(out, n, en ? "%d hours and %d minutes" : "%d ore e %d minuti",  hh, mm);
        else         snprintf(out, n, en ? "%d minutes"             : "%d minuti", mm);
    } else if (!strcmp(arg, "ram")) {
        snprintf(out, n, en ? "%u KB of RAM free" : "%u KB di RAM liberi", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024));
    } else if (!strcmp(arg, "network")) {
        const char *mode = nucleo_setup_mode(), *ssid = nucleo_setup_ssid(), *ip = nucleo_setup_ip();
        if (mode && !strcmp(mode, "ap")) snprintf(out, n, en ? "I'm a Wi-Fi hotspot \"%s\", IP %s" : "Sono un hotspot Wi-Fi \"%s\", IP %s", ssid, ip);
        else if (ssid && ssid[0])        snprintf(out, n, en ? "connected to \"%s\", IP %s" : "connesso a \"%s\", IP %s", ssid, ip);
        else                             snprintf(out, n, en ? "not connected" : "non connesso");
    } else if (!strcmp(arg, "capabilities")) {
        int cnt = nucleo_registry_count(); const nucleo_app_t *apps = nucleo_registry_apps();
        char list[80] = ""; int shown = 0;
        for (int i = 0; i < cnt && shown < 5; i++) {
            const char *nm = apps[i].name[0] ? apps[i].name : apps[i].id;
            if (list[0] && strlen(list) + strlen(nm) + 3 < sizeof(list)) strncat(list, ", ", sizeof(list) - strlen(list) - 1);
            if (strlen(list) + strlen(nm) + 1 < sizeof(list)) { strncat(list, nm, sizeof(list) - strlen(list) - 1); shown++; }
        }
        snprintf(out, n, en ? "I can open apps (%s...), give you time/date/space/battery, the weather of a city, manage the calendar, solve math/physics/geometry/vectors/Ohm, conversions, spreadsheet formulas, create files, and answer about NucleoOS/C/electronics"
                            : "Posso aprire le app (%s...), darti ora/data/spazio/batteria, il meteo di una citta, gestire il calendario, risolvere matematica/fisica/geometria/vettori/Ohm, conversioni, formule del foglio di calcolo, creare file, e rispondere su NucleoOS/C/elettronica", list);
    } else if (!strcmp(arg, "agenda") && tm) {
        snprintf(out, n, en ? "you have no events today" : "oggi non hai impegni");
        FILE *f = fopen(NUCLEO_SD_MOUNT "/system/config/calendar.json", "rb");
        if (f) {
            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
            char *buf = (sz > 0 && sz < 65536) ? (char *)malloc(sz + 1) : nullptr;
            if (buf && fread(buf, 1, sz, f) == (size_t)sz) {
                buf[sz] = 0;
                char key[16]; snprintf(key, sizeof(key), "%04d-%02d-%02d", (tm->tm_year + 1900) % 10000, (tm->tm_mon + 1) % 100, tm->tm_mday % 100);
                cJSON *root = cJSON_Parse(buf);
                cJSON *evs = root ? cJSON_GetObjectItem(root, "events") : nullptr;
                cJSON *today = evs ? cJSON_GetObjectItem(evs, key) : nullptr;
                int c = (today && cJSON_IsArray(today)) ? cJSON_GetArraySize(today) : 0;
                if (c > 0) {
                    char l[200] = ""; cJSON *ev;
                    cJSON_ArrayForEach(ev, today) {
                        const cJSON *t = cJSON_GetObjectItem(ev, "time"), *tx = cJSON_GetObjectItem(ev, "text");
                        const char *ts = cJSON_IsString(t) ? t->valuestring : "", *txs = cJSON_IsString(tx) ? tx->valuestring : "";
                        char one[96]; snprintf(one, sizeof(one), "%s%s%s", ts, ts[0] ? " " : "", txs);
                        if (l[0] && strlen(l) + strlen(one) + 3 < sizeof(l)) strncat(l, "; ", sizeof(l) - strlen(l) - 1);
                        if (strlen(l) + strlen(one) + 1 < sizeof(l)) strncat(l, one, sizeof(l) - strlen(l) - 1);
                    }
                    snprintf(out, n, en ? "today you have %d %s: %s" : "oggi hai %d %s: %s",
                             c, en ? (c == 1 ? "event" : "events") : (c == 1 ? "impegno" : "impegni"), l);
                }
                if (root) cJSON_Delete(root);
            }
            free(buf); fclose(f);
        }
    }
}

// ---- app launching ----------------------------------------------------------
// ANIMA returns WEB registry ids (app-aliases.json); map them to the native launcher ids. Apps that
// exist only in the web shell get a friendly note instead of a dead "I don't have that app".
static const char *native_app_id(const char *anima_id) { return nucleo_app_native_id(anima_id); }  // fonte unica in nucleo_app
static bool is_web_only(const char *web)
{
    static const char *W[] = { "paint", "spreadsheet", "terminal", "settings", "browser", "tasks",
                               "log-viewer", "swarm", "automation-studio", "recycle-bin", "updates", "dosbox", nullptr };
    for (int i = 0; W[i]; i++) if (!strcmp(W[i], web)) return true;
    return false;
}
static const char *native_name(const char *id)
{
    int n = nucleo_app_count();
    for (int i = 0; i < n; i++) { const nucleo_app_def_t *a = nucleo_app_at(i); if (!strcmp(a->id, id)) return a->name[0] ? a->name : id; }
    return id;
}
// Map a file path's extension to the native viewer that opens it (mirrors nucleo_app_launch_file's
// table). Used by the "aprilo" file follow-up so it routes through the deferred-launch path. NULL = none.
static const char *file_app(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return nullptr;
    if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg") || !strcasecmp(ext, ".png") ||
        !strcasecmp(ext, ".bmp") || !strcasecmp(ext, ".gif")) return "photos";
    if (!strcasecmp(ext, ".mp3") || !strcasecmp(ext, ".wav")) return "music";
    if (!strcasecmp(ext, ".txt") || !strcasecmp(ext, ".md")  || !strcasecmp(ext, ".json") ||
        !strcasecmp(ext, ".log") || !strcasecmp(ext, ".csv") || !strcasecmp(ext, ".ini")) return "notepad";
    return nullptr;
}

// Append an ANIMA-scheduled reminder to the OS calendar (mirrors anima_apply_event in nucleo_httpd.c).
// spec is the add_event content-channel payload "off=<days>;time=<HH:MM|>;text=<...>"; the day offset
// resolves against the RTC. Reads + appends + writes atomically (temp+rename) and fills a confirmation.
static bool apply_event(const char *spec, char *reply, size_t rcap)
{
    if (!spec || !spec[0]) return false;
    int off = 0; char tm[8] = ""; const char *text = "";
    const char *p  = strstr(spec, "off=");   if (p)  off = atoi(p + 4);
    const char *pt = strstr(spec, ";time="); if (pt) { pt += 6; int i = 0; while (pt[i] && pt[i] != ';' && i < 7) { tm[i] = pt[i]; i++; } tm[i] = 0; }
    const char *px = strstr(spec, ";text="); if (px) text = px + 6;
    if (!text[0]) return false;

    time_t now = time(NULL); struct tm t; localtime_r(&now, &t);
    t.tm_mday += off; t.tm_hour = 12; t.tm_min = 0; t.tm_sec = 0;   // noon: dodge a DST edge in mktime
    mktime(&t);
    char date[16]; strftime(date, sizeof(date), "%Y-%m-%d", &t);

    // Azzera il task-WDT (8s) PRIMA dell'I/O su SD: questa funzione gira sulla UI task (watchdog-watched)
    // e una scrittura su SD lenta/contesa puo' prendere secondi -> senza questo il WDT resetta il chip a
    // meta' scrittura (era il "i promemoria fanno riavviare"). No-op se la task non e' iscritta al WDT.
    if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();
    const char *path = NUCLEO_SD_MOUNT "/system/config/calendar.json";
    cJSON *root = nullptr;
    bool had_data = false;
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END); long nn = ftell(f); fseek(f, 0, SEEK_SET);
        had_data = nn > 0;
        // 32 KB cap, sized to the device heap (file + cJSON tree must coexist) — mirrors nucleo_httpd.c.
        if (nn > 0 && nn < 32768) { char *b = (char *)malloc((size_t)nn + 1); if (b) { size_t rd = fread(b, 1, (size_t)nn, f); b[rd] = 0; root = cJSON_Parse(b); free(b); } }
        fclose(f);
    }
    // Fail-closed: an existing calendar that can't be loaded (oversized/OOM/corrupt) must NOT be
    // rewritten with only the new event — that erased the whole calendar and still said "Added".
    if (had_data && !root) return false;
    if (!root) root = cJSON_CreateObject();
    cJSON *events = cJSON_GetObjectItem(root, "events");
    if (!cJSON_IsObject(events)) { cJSON_DeleteItemFromObject(root, "events"); events = cJSON_AddObjectToObject(root, "events"); }
    cJSON *day = cJSON_GetObjectItem(events, date);
    if (!cJSON_IsArray(day)) { cJSON_DeleteItemFromObject(events, date); day = cJSON_AddArrayToObject(events, date); }
    cJSON *ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "time", tm);
    cJSON_AddStringToObject(ev, "text", text);
    cJSON_AddItemToArray(day, ev);

    char *outc = cJSON_PrintUnformatted(root); cJSON_Delete(root);
    bool ok = false;
    if (outc) {
        if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();   // read+parse fatti: ripeti pet prima del write
        mkdir(NUCLEO_SD_MOUNT "/system", 0775); mkdir(NUCLEO_SD_MOUNT "/system/config", 0775);
        char tmp[160]; snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        FILE *o = fopen(tmp, "wb");
        if (o) { fwrite(outc, 1, strlen(outc), o); fclose(o); remove(path); ok = (rename(tmp, path) == 0); if (!ok) remove(tmp); }
        cJSON_free(outc);
    }
    if (ok) {
        // NIENTE event_publish qui: on-device (ANIMA nativa) NON c'e' MAI un client web da refreshare, e il
        // publish prende il mutex del bus eventi con portMAX_DELAY + scrive il journal su SD MENTRE apply_event
        // sta gia' usando la SD -> se quella scrittura si contende/blocca, il mutex resta preso all'infinito e
        // ogni task che usa il bus (httpd incluso) si blocca = FREEZE TOTALE del device. Il reminder deve SOLO
        // scrivere il file. (Il path web in nucleo_httpd.c pubblica ancora l'evento, li' un client puo' esserci.)
        if (tm[0]) snprintf(reply, rcap, s_en ? "Added \"%s\" on %s at %s." : "Aggiunto \"%s\" il %s alle %s.", text, date, tm);
        else       snprintf(reply, rcap, s_en ? "Added \"%s\" on %s."       : "Aggiunto \"%s\" il %s.",       text, date);
    }
    return ok;
}

#define NATIVE_REPLY_MAX 312   // longest reply kept verbatim before the boundary clip (<= MSG_TEXT-1). Sized to
                               // show OFFLINE answers whole (L1 <=250 always; MOSAICO up to ~312), never
                               // mid-sentence — the transcript scrolls (up/down) to read past the screen.
                               // Online replies are steered compact (~160), so for them this never binds.

// ---- calculator chaining helpers --------------------------------------------
// An answer counts as "math" (chainable) when its intent is one of the solver families.
static bool is_math_intent(const char *it)
{
    if (!it || !it[0]) return false;
    static const char *M[] = { "calc", "percent", "convert", "ohm", "base", "prime", "roman", "geo", "phys", nullptr };
    for (int i = 0; M[i]; i++) if (!strcmp(it, M[i])) return true;
    return false;
}
// Pull the LAST numeric token out of an answer ("Fa 2430." / "18 * 24 = 432." -> the result).
static void extract_last_number(const char *s, char *out, size_t n)
{
    out[0] = 0; const char *best = nullptr; int bestlen = 0;
    for (const char *p = s; *p; ) {
        bool neg = (*p == '-' && p[1] >= '0' && p[1] <= '9');
        if ((*p >= '0' && *p <= '9') || neg) {
            const char *q = p; if (*q == '-') q++;
            while ((*q >= '0' && *q <= '9') || *q == '.') q++;
            const char *e = q; while (e > p && e[-1] == '.') e--;     // trim trailing dots
            best = p; bestlen = (int)(e - p); p = q;
        } else p++;
    }
    if (best && bestlen > 0) { int l = bestlen; if (l > (int)n - 1) l = (int)n - 1; memcpy(out, best, l); out[l] = 0; }
}
// A word-operator at the start of the line, honouring a word boundary (next char space/digit/end).
static bool cont_word(const char *s, const char *w)
{
    size_t l = strlen(w);
    if (strncasecmp(s, w, l)) return false;
    char nx = s[l]; return nx == 0 || nx == ' ' || (nx >= '0' && nx <= '9');
}
// If `in` is a bare continuation ("+ 5", "diviso 32", "x 2", "al quadrato") AND the last answer was a
// number, build "<lastnum> <in>" for the engine. Returns true (out filled) when it rewrote the query.
static bool chain_math(const char *in, char *out, size_t n)
{
    if (!s_last_math || !s_last_num[0]) return false;
    const char *s = in; while (*s == ' ') s++;
    char c = *s;
    bool op = (c == '+' || c == '-' || c == '*' || c == '/' || c == '^' || c == '%');
    if (!op) {
        static const char *W[] = { "diviso", "fratto", "per", "piu", "meno", "volte", "x",
                                   "plus", "minus", "times", "over", "divided", "multiplied", nullptr };
        for (int i = 0; W[i] && !op; i++) if (cont_word(s, W[i])) op = true;
    }
    if (!op) {                                                         // unary power phrases
        if (!strcasecmp(s, "al quadrato") || !strcasecmp(s, "squared")) { snprintf(out, n, "%s ^ 2", s_last_num); return true; }
        if (!strcasecmp(s, "al cubo")     || !strcasecmp(s, "cubed"))   { snprintf(out, n, "%s ^ 3", s_last_num); return true; }
        return false;
    }
    snprintf(out, n, "%s %s", s_last_num, in);
    return true;
}

// Turn the just-returned result into transcript messages (and queue a launch if asked).
static void present_result(void)
{
    char reply[416];
    bool launched = false;
    bool tool_ok = true;            // esito dell'operazione TOOL -> conferma vocale "Fatto"/"Errore"
    bool _tool_write = s_res.action == ANIMA_ACT_TOOL &&
        (!strcmp(s_res.intent, "add_event") || !strcmp(s_res.intent, "create_file"));
    // PIPELINE SEQUENZIALE (mai operazioni parallele): prima di scrivere il memo su SD, FERMA del tutto
    // l'audio in corso e attendi che il task player si sia smontato. Senza, la scrittura SD del calendario
    // correva IN PARALLELO con il task audio che legge/scrive la stessa SD (assemblaggio WAV/play della
    // voce di una risposta precedente) -> contesa FatFs/I2S che inchioda il device (era il freeze del
    // "ricordami/segna appuntamento"). nucleo_audio_stop e' bounded (~4.5s max) e pet-a il WDT. Cosi' la
    // sequenza e': capisci (worker, gia' concluso e lock rilasciato) -> [stop audio + libera] -> scrivi
    // memo -> SOLO DOPO sintetizza la voce di conferma (in coda, sotto). Una risorsa per volta.
    if (_tool_write) {
        nucleo_audio_stop();             // nessun task audio tocca la SD mentre scriviamo il memo
        nucleo_audio_wait_idle(200);     // margine: l'uscita I2S e' libera prima dell'I/O su SD
        ESP_LOGW(ATAG, "TOOL %s START free=%u largest=%u", s_res.intent,
            (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    }
    if (s_res.action == ANIMA_ACT_SYSTEM) {
        char value[384]; fill_system_value(s_res.arg, value, sizeof(value), s_en);
        const char *ph = strstr(s_res.reply, "{value}");
        if (ph) snprintf(reply, sizeof(reply), "%.*s%s%s", (int)(ph - s_res.reply), s_res.reply, value, ph + 7);
        else    snprintf(reply, sizeof(reply), "%s", s_res.reply);
    } else if (s_res.action == ANIMA_ACT_TOOL && !strcmp(s_res.intent, "create_file") && s_res.arg[0]
               && s_res.arg[0] == '/' && !strstr(s_res.arg, "..")) {
        // Physical user present -> no pairing PIN gate (unlike the web). Never overwrite. Guard: arg
        // must be an absolute SD path with no ".." (no traversal outside the mount).
        const char *bn = strrchr(s_res.arg, '/'); bn = bn ? bn + 1 : s_res.arg;
        char path[128]; snprintf(path, sizeof(path), NUCLEO_SD_MOUNT "%s", s_res.arg);
        char dir[128]; snprintf(dir, sizeof(dir), "%s", path);
        char *slash = strrchr(dir, '/'); if (slash && slash != dir) { *slash = 0; mkdir(dir, 0775); }
        if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();   // pet il WDT prima del write SD (come apply_event)
        FILE *ex = fopen(path, "rb");
        if (ex) { fclose(ex); snprintf(reply, sizeof(reply), "%s esiste gia: non lo sovrascrivo.", bn); nucleo_anima_note_file(s_res.arg); nucleo_anima_observe("create_file", true); }
        else {
            FILE *cf = fopen(path, "wb");
            if (cf) { const char *body = nucleo_anima_tool_content();
                      if (body && body[0]) { fwrite(body, 1, strlen(body), cf); snprintf(reply, sizeof(reply), "Ho creato %s con il contenuto.", bn); }
                      else snprintf(reply, sizeof(reply), "Ho creato %s.", bn);
                      fclose(cf); nucleo_anima_note_file(s_res.arg); nucleo_anima_observe("create_file", true); }
            else    { snprintf(reply, sizeof(reply), "Non riesco a creare %s.", bn); nucleo_anima_observe("create_file", false); tool_ok = false; }
        }
    } else if (s_res.action == ANIMA_ACT_TOOL &&
               (!strcmp(s_res.intent, "set_volume") || !strcmp(s_res.intent, "set_brightness"))) {
        // arg is "<pct>" (absolute) or "+N"/"-N" (relative). Physical user present -> no PIN gate.
        bool vol = !strcmp(s_res.intent, "set_volume");
        int cur  = vol ? nucleo_audio_volume() : nucleo_app_brightness();
        int want = (s_res.arg[0] == '+' || s_res.arg[0] == '-') ? cur + atoi(s_res.arg) : atoi(s_res.arg);
        if (want < 0) want = 0;
        if (want > 100) want = 100;
        if (vol) nucleo_audio_set_volume(want); else nucleo_app_set_brightness(want);
        // Bilingue: e' pronunciata PER INTERO (say_or sotto), quindi in EN deve uscire in inglese o la
        // voce EN non la coprirebbe. mathspeak rende "%" -> "per cento"/"percent".
        snprintf(reply, sizeof(reply), s_en ? (vol ? "Volume %d%%." : "Brightness %d%%.")
                                            : (vol ? "Volume al %d%%." : "Luminosita al %d%%."), want);
        nucleo_anima_observe(s_res.intent, true);
    } else if (s_res.action == ANIMA_ACT_TOOL && !strcmp(s_res.intent, "add_event")) {
        // Calendar reminder: the spec is on the content channel; write it straight to the OS calendar.
        bool _w = apply_event(nucleo_anima_tool_content(), reply, sizeof(reply));
        ESP_LOGW(ATAG, "add_event WRITTEN ok=%d free=%u largest=%u", _w,
            (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        if (_w) nucleo_anima_observe("add_event", true);
        else { snprintf(reply, sizeof(reply), s_en ? "I couldn't add the reminder." : "Non sono riuscito ad aggiungere il promemoria."); nucleo_anima_observe("add_event", false); tool_ok = false; }
    } else if (s_res.action == ANIMA_ACT_TOOL && !strcmp(s_res.intent, "open_file")) {
        // Follow-up "aprilo" on a remembered file -> open it in the matching native viewer by extension,
        // reusing the deferred-launch path (s_pending_launch). No mapped viewer -> honest message.
        const char *bn = strrchr(s_res.arg, '/'); bn = bn ? bn + 1 : s_res.arg;
        const char *id = (s_res.arg[0] == '/') ? file_app(s_res.arg) : nullptr;
        if (id) { snprintf(s_pending_launch, sizeof(s_pending_launch), "%s", id);
                  snprintf(reply, sizeof(reply), s_en ? "Opening %s..." : "Apro %s...", bn); launched = true; }
        else      snprintf(reply, sizeof(reply), s_en ? "I can't open %s on the device." : "Non posso aprire %s sul dispositivo.", bn);
    } else if (s_res.action == ANIMA_ACT_TOOL && !strcmp(s_res.intent, "create_file")) {
        // create_file whose arg failed the absolute-path guard above -> honest error, never a silent
        // "success" via the generic reply (which would confirm a file that was never written).
        snprintf(reply, sizeof(reply), s_en ? "Invalid file path." : "Percorso file non valido.");
        nucleo_anima_observe("create_file", false); tool_ok = false;
    } else if (s_res.action == ANIMA_ACT_LAUNCH) {
        if (is_web_only(s_res.arg)) {
            snprintf(reply, sizeof(reply), s_en ? "%s is only available in the web app." : "%s e disponibile solo nell'app web.", s_res.arg);
        } else {
            const char *nat = native_app_id(s_res.arg);
            ESP_LOGI(ATAG, "ANIMA launch: arg='%s' -> native='%s'", s_res.arg, nat);   // DIAGNOSI: cosa apre davvero
            snprintf(s_pending_launch, sizeof(s_pending_launch), "%s", nat);
            snprintf(s_launch_web, sizeof(s_launch_web), "%s", s_res.arg);
            snprintf(reply, sizeof(reply), s_en ? "Opening %s..." : "Apro %s...", native_name(nat));
            launched = true;
        }
    } else {
        snprintf(reply, sizeof(reply), "%s", s_res.reply[0] ? s_res.reply : (s_en ? "I don't know." : "Non lo so."));
    }
    // Tiny screen: keep a long online answer SHORT on the device (the web shows it in full). Clip at a
    // clean boundary — the longest complete sentence within the limit, else a whole word; never mid-word.
    if ((int)strlen(reply) > NATIVE_REPLY_MAX) {
        int cut = 0;
        for (int i = 0; i < NATIVE_REPLY_MAX && reply[i]; i++)
            if (reply[i] == '.' || reply[i] == '!' || reply[i] == '?') cut = i + 1;
        if (cut == 0) { cut = NATIVE_REPLY_MAX; while (cut > 0 && reply[cut] != ' ') cut--; if (!cut) cut = NATIVE_REPLY_MAX; }
        while (cut > 0 && ((unsigned char)reply[cut] & 0xC0) == 0x80) cut--;
        reply[cut] = 0;
    }
    // The answer bubble: amber rail when ANIMA is asking a follow-up (awaiting a reply), else violet.
    push_anima(reply, s_res.awaiting ? AMBER : ACC);
    if (s_res.corrected[0] && !launched) { char c[80]; snprintf(c, sizeof(c), s_en ? "(understood: %s)" : "(ho inteso: %s)", s_res.corrected); push_meta(c, DIM); }
    // Reasoning trace (Claude-Code-style steps): only for genuine multi-step agent turns (those whose
    // trace has a step separator). Single-tier answers stay clean — the badge already shows tier+conf.
    if (strstr(s_res.trace, " > ")) { char tr[120]; snprintf(tr, sizeof(tr), "|_ %s", s_res.trace); push_meta(tr, DIM); }

    s_last_conf = (s_res.action == ANIMA_ACT_NONE) ? -1 : s_res.confidence;
    s_last_tier = s_res.tier == ANIMA_TIER_COMMAND ? "L0" :
                  s_res.tier == ANIMA_TIER_FACT    ? "L1" :
                  s_res.tier == ANIMA_TIER_REMOTE  ? "web" : "";
    snprintf(s_last_subject, sizeof(s_last_subject), "%s", s_res.subject);
    s_awaiting = s_res.awaiting;                  // drives the "rispondi..." input placeholder
    // Calculator chain: remember the number this answer produced so a bare "diviso 32" continues it.
    s_last_math = is_math_intent(s_res.intent) && s_res.action == ANIMA_ACT_ANSWER;
    if (s_last_math) extract_last_number(reply, s_last_num, sizeof s_last_num); else s_last_num[0] = 0;

    // Conferma VOCALE delle operazioni (TOOL): se la frase esatta non e' pronunciabile (nomi file,
    // dettagli evento -> finirebbe in "leggila"), dice una conferma breve sull'ESITO. Le reply gia'
    // coperte (es. "Volume al 70 per cento") vengono dette tali e quali. I LANCIO non parlano:
    // l'app che si apre e' gia' il feedback. (Nel path low-mem speak_result salta TOOL: niente doppio.)
    if (s_res.action == ANIMA_ACT_TOOL && !launched) {
        if (_tool_write) ESP_LOGW(ATAG, "TOOL %s SPEAK start largest=%u", s_res.intent,
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        nucleo_tts_say_or(reply, tool_ok ? (s_en ? "Done" : "Fatto") : (s_en ? "Error" : "Errore"), s_en ? "en" : "it");
        if (_tool_write) ESP_LOGW(ATAG, "TOOL %s SPEAK done", s_res.intent);
    }

    s_d_hdr = true;
    save_chat();                                   // persist the transcript so re-entry shows this turn
}

static void clear_chat(void);
static void submit(void);
static void refresh_complications(void);   // watch-face glance strip; used by clear_chat() above its definition

// Cancel the in-flight query: bump the generation so the worker's result is dropped, free the UI now.
static void cancel_query(void)
{
    if (!s_busy) { push_meta(s_en ? "Nothing to stop." : "Niente da fermare.", DIM); return; }
    s_gen = s_gen + 1;
    s_busy = false; s_spin = 0;
    s_d_hdr = true;
    push_meta(s_en ? "(stopped)" : "(annullato)", DIM);
}

// Quick mode switch: Offline -> Ibrido -> Solo online -> ...
static void cycle_mode(void)
{
    s_omode = (s_omode + 1) % 3;
    apply_online_mode(); save_settings();
    const char *m = s_omode == OM_OFF ? "Offline" : s_omode == OM_ONLY ? "Solo online" : "Ibrido";
    char t[40]; snprintf(t, sizeof t, "Modalita: %s", m); push_meta(t, GRN);
}

// Cycle the offline L1 brain policy: AUTO -> ON (forced) -> OFF -> AUTO. AUTO already stands L1 down
// whenever a cloud teacher (key+online) answers; ON forces the offline brain back on, OFF kills it.
static void cycle_l1(void)
{
    // set_mode can free the L1 index (OFF / AUTO with a cloud brain): only under the spine gate, or a
    // worker mid-query reads freed buffers. Busy = tell the user to retry, don't corrupt.
    if (!nucleo_anima_try_lock()) { push_meta(s_en ? "ANIMA busy, retry" : "ANIMA occupata, riprova", AMBER); return; }
    int md = (nucleo_anima_l1_get_mode() + 1) % 3;     // 0 AUTO, 1 ON, 2 OFF
    nucleo_anima_l1_set_mode(md);
    nucleo_anima_unlock();
    const char *s = md == 1 ? (s_en ? "Offline AI (L1): ON (forced)"  : "AI offline (L1): ON (forzata)")
                  : md == 2 ? (s_en ? "Offline AI (L1): OFF"          : "AI offline (L1): OFF")
                  :           (s_en ? "Offline AI (L1): AUTO"         : "AI offline (L1): AUTO");
    push_meta(s, GRN);
}

static void push_help(void)
{
    push_meta(s_en ? "Commands:" : "Comandi:", ACC);
    push_meta("/stop  /modo  /offline  /ibrido  /online", DIM);
    push_meta("/l1  /cancella  /info  /aiuto", DIM);
    push_meta(s_en ? "/l1: offline brain AUTO/ON/OFF (RAM)" : "/l1: AI offline AUTO/ON/OFF (RAM)", DIM);
    push_meta(s_en ? "Right: switch mode. TAB: menu." : "Freccia destra: modalita. TAB: menu.", DIM);
    push_meta(s_en ? "Hold G0 1.5s: open ANIMA anywhere." : "Tieni G0 1.5s: apri ANIMA ovunque.", DIM);
}

static void push_info(void)
{
    char b[64];
    snprintf(b, sizeof b, "RAM libera: %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024)); push_meta(b, MUTED);
    const char *ssid = nucleo_setup_ssid(), *ip = nucleo_setup_ip();
    if (ip && ip[0]) snprintf(b, sizeof b, "Rete: %s  %s", (ssid && ssid[0]) ? ssid : "-", ip);
    else             snprintf(b, sizeof b, "Rete: non connesso");
    push_meta(b, MUTED);
    const char *m = s_omode == OM_OFF ? "Offline" : s_omode == OM_ONLY ? "Solo online" : "Ibrido";
    snprintf(b, sizeof b, "Modalita: %s   Worker: %s", m, s_worker ? "ok" : "NON pronto"); push_meta(b, MUTED);
    int l1m = nucleo_anima_l1_get_mode();
    snprintf(b, sizeof b, "AI offline (L1): %s  (%s)",
             l1m == 1 ? "ON" : l1m == 2 ? "OFF" : "AUTO",
             nucleo_anima_l1_serving() ? (s_en ? "active" : "attiva") : (s_en ? "stood down" : "a riposo"));
    push_meta(b, MUTED);
}

// Handle a "/command" typed in the chat. Returns true if it was a recognised command.
static bool handle_slash(const char *in)
{
    push_user(in);
    if      (!strcmp(in, "/stop"))                              cancel_query();
    else if (!strcmp(in, "/cancella") || !strcmp(in, "/clear")) clear_chat();
    else if (!strcmp(in, "/offline")) { s_omode = OM_OFF;  apply_online_mode(); save_settings(); push_meta("Modalita: Offline", GRN); }
    else if (!strcmp(in, "/ibrido"))  { s_omode = OM_ON;   apply_online_mode(); save_settings(); push_meta("Modalita: Ibrido", GRN); }
    else if (!strcmp(in, "/online"))  { s_omode = OM_ONLY; apply_online_mode(); save_settings(); push_meta("Modalita: Solo online", GRN); }
    else if (!strcmp(in, "/modo"))    cycle_mode();
    else if (!strcmp(in, "/l1"))      cycle_l1();
    else if (!strcmp(in, "/info") || !strcmp(in, "/stato")) push_info();
    else if (!strcmp(in, "/aiuto") || !strcmp(in, "/help")) push_help();
    else push_meta(s_en ? "Unknown command. /help for the list." : "Comando sconosciuto. /aiuto per la lista.", DIM);
    return true;
}

static void submit(void)
{
    if (s_ilen == 0) return;
    s_input[s_ilen] = 0;
    hist_push(s_input);                           // remember the typed line for Ctrl+Up recall + autocomplete
    s_user_sent = true;                           // first question -> the suggestion deck steps aside
    s_awaiting = false;                           // the user is answering; a new follow-up may re-arm it
    if (s_input[0] == '/') {                       // slash-commands run anytime (even while busy, e.g. /stop)
        handle_slash(s_input);
        s_ilen = 0; s_input[0] = 0; s_d_input = true; nucleo_app_request_draw(); return;
    }
    if (s_busy) return;                            // one query at a time
    push_user(s_input);                            // the visible bubble is exactly what was typed
    char sendq[A_INMAX];                           // ...but the engine may get the calculator chain
    if (chain_math(s_input, sendq, sizeof sendq)) snprintf(s_req, sizeof(s_req), "%s", sendq);
    else                                          snprintf(s_req, sizeof(s_req), "%s", s_input);
    s_ilen = 0; s_input[0] = 0;
    s_gen = s_gen + 1;
    if (s_worker) {                                // normal path: run the (possibly online) query off-loop
        s_busy = true; s_done = false; s_spin = 0;
        xTaskNotifyGive(s_worker);
    } else {
        // Low memory: the 30 KB online worker couldn't be created. Run INLINE and FORCED OFFLINE so it
        // still answers (L0/L1 are fast, no watchdog risk) and we skip the blocking online tiers.
        bool saved_online = nucleo_anima_online_enabled();
        nucleo_anima_set_online(false);
        s_res = nucleo_anima_query(s_req, s_en ? "en" : "it");
        nucleo_anima_set_online(saved_online);
        s_done_gen = s_gen;
        present_result();
        speak_result(s_res, s_en);   // voce on-device (gated: no conoscenza/calc)
        if (s_pending_launch[0]) {
            s_launch_wait = 3;                             // defer via tick() so L1 frees + the heap settles first
        } else if (s_res.action != ANIMA_ACT_LAUNCH) {     // a web-only launch already showed its own note
            push_meta(s_en ? "(offline: low memory for the online model)"
                           : "(offline: memoria insufficiente per l'online)", DIM);
        }
    }
    s_d_input = true; s_d_hdr = true;
    nucleo_app_request_draw();
}

static void clear_chat(void)
{
    nucleo_anima_reset_session();
    s_mhead = s_mcount = 0; s_rown = 0; s_scroll = 0; s_ilen = 0; s_input[0] = 0;
    s_last_conf = -1; s_last_tier = ""; s_last_subject[0] = 0;
    s_last_math = false; s_last_num[0] = 0;
    s_user_sent = false; s_sug_sel = 0; s_awaiting = false;   // back to the suggestion deck
    remove(CHAT_PATH);                                        // forget the persisted conversation too
    refresh_complications();
    mark_all_dirty();
}

// ---- suggestion deck (empty-state) ------------------------------------------
// A fresh/cleared chat shows starter prompts that exercise the breadth of ANIMA AND lean on everyday
// human life: time, weather, app launch, mental math, a calendar reminder, a unit conversion, a
// percentage (tip/discount), capabilities. su/giu pick, Invio runs.
#define SUG_N 16   // deck espanso: 16 voci scrollabili (su/giu), copre piu' skill
static const char *SUG_IT[SUG_N] = {
    "Che ore sono", "Che giorno e oggi", "Meteo a Brescia",
    "Quanto spazio ho sulla SD", "Apri la musica", "Quanto fa 18 x 24",
    "Quanto e il 15% di 80", "Ricordami la spesa domani alle 18", "Cosa sai fare",
    // nuovi suggerimenti
    "Radice quadrata di 144", "Converti 5 miglia in km",
    "Chi e Ada Lovelace", "Quanto fa 37 per 48",
    "In che stagione siamo", "Ricordami palestra domani alle 7",
    "Cos'e il DNA"
};
static const char *SUG_EN[SUG_N] = {
    "What time is it", "What's today's date", "Weather in London",
    "How much SD space is free", "Open the music", "How much is 18 x 24",
    "What is 15% of 80", "Remind me groceries tomorrow at 6pm", "What can you do",
    // new suggestions
    "Square root of 144", "Convert 5 miles to km",
    "Who is Ada Lovelace", "How much is 37 times 48",
    "What season is it", "Remind me gym tomorrow at 7am",
    "What is DNA"
};
static const char *cur_sug(int i) { return (s_en ? SUG_EN : SUG_IT)[i < SUG_N ? i : 0]; }
static bool deck_active(void)     { return !s_user_sent && s_ilen == 0 && !s_menu_open; }

// Recall a history entry into the input. dir<0 = older (Ctrl+Up), dir>0 = newer (Ctrl+Down). The live
// draft is parked on first step back and restored when you walk forward past the newest entry.
static void hist_recall(int dir)
{
    if (s_hist_count == 0) return;
    if (s_hist_nav < 0) {                                  // entering history: stash the draft
        if (dir > 0) return;                               // already on the draft, nothing newer
        snprintf(s_hist_draft, sizeof s_hist_draft, "%s", s_input);
        s_hist_nav = 0;
    } else {
        s_hist_nav += (dir < 0) ? 1 : -1;
    }
    if (s_hist_nav >= s_hist_count) s_hist_nav = s_hist_count - 1;
    const char *line = (s_hist_nav < 0) ? s_hist_draft : hist_at(s_hist_nav);
    snprintf(s_input, sizeof s_input, "%s", line ? line : "");
    s_ilen = (int)strlen(s_input);
    s_d_input = true;
}

// Inline autocomplete (fish-style ghost text): the best single completion of the typed prefix, drawn
// dimmed after the caret and accepted with the Right arrow. Sources, in priority: slash commands,
// your own history (most recent first), then the starter suggestions. Case-insensitive prefix match.
static const char *const SLASH_CMDS[] = { "/stop", "/cancella", "/clear", "/offline", "/ibrido",
                                          "/online", "/modo", "/l1", "/info", "/stato", "/aiuto", "/help" };
static bool ready_leaf_complete(const char *pfx, char *out, int cap, int pl);   // scans the ready (slots==0) skills
static bool autocomplete(const char *pfx, char *out, int cap)
{
    int pl = (int)strlen(pfx);
    if (pl < 2) return false;                              // wait for 2+ chars so it isn't noisy
    if (pfx[0] == '/') {
        for (unsigned i = 0; i < sizeof SLASH_CMDS / sizeof *SLASH_CMDS; i++)
            if ((int)strlen(SLASH_CMDS[i]) > pl && !strncasecmp(SLASH_CMDS[i], pfx, pl)) { snprintf(out, cap, "%s", SLASH_CMDS[i]); return true; }
        return false;
    }
    for (int n = 0; n < s_hist_count; n++) {              // 1) your own past lines (most recent first)
        const char *h = hist_at(n);
        if (h && h[0] && h[0] != '/' && (int)strlen(h) > pl && !strncasecmp(h, pfx, pl)) { snprintf(out, cap, "%s", h); return true; }
    }
    for (int i = 0; i < SUG_N; i++) {                     // 2) the curated starter prompts
        const char *s = cur_sug(i);
        if ((int)strlen(s) > pl && !strncasecmp(s, pfx, pl)) { snprintf(out, cap, "%s", s); return true; }
    }
    return ready_leaf_complete(pfx, out, cap, pl);        // 3) every ready-to-send device skill (the IDEE catalog)
}

// ---- the skill catalog: categories + leaves (drives the IDEE tree) ----------
// One row per thing ANIMA can do, grouped by skill. A leaf with slots==0 sends its template verbatim;
// slots 1/2 means the template has that many %s placeholders the form fills from user input (the
// "ask for the numbers instead of inventing 4x4" behaviour). Stored const -> lives in flash, not RAM.
#define CAT_N 9
static const char *CAT_IT[CAT_N] = { "Sistema", "Calcolo", "Geometria", "Conversioni", "Meteo",
                                     "Agenda", "App e File", "Sapere", "Traduci" };
static const char *CAT_EN[CAT_N] = { "System", "Math", "Geometry", "Conversions", "Weather",
                                     "Agenda", "Apps & Files", "Knowledge", "Translate" };
static const char *cat_label(int c) { return (s_en ? CAT_EN : CAT_IT)[c]; }

typedef struct {
    unsigned char cat, slots;        // parent category; number of %s the template needs (0/1/2)
    const char *l_it, *l_en;         // leaf label (short, shown in the list)
    const char *t_it, *t_en;         // query template (contains exactly `slots` %s; %% = literal %)
    const char *p1_it, *p1_en;       // slot-1 field label (NULL when slots==0)
    const char *p2_it, *p2_en;       // slot-2 field label (NULL when slots<2)
} Leaf;

// Grouped by category in order, so cat_leaf_at() can scan linearly. Templates phrase the query the way
// the offline cascade already understands (mirrors the corpus + solver intents).
static const Leaf LEAVES[] = {
    // -- Sistema (0): live device readouts, all ready-to-send --
    { 0,0, "Ora","Time",                 "Che ore sono","What time is it",                       0,0,0,0 },
    { 0,0, "Data","Date",                "Che giorno e oggi","What is today's date",             0,0,0,0 },
    { 0,0, "Anno","Year",                "In che anno siamo","What year is it",                  0,0,0,0 },
    { 0,0, "Stagione","Season",          "In che stagione siamo","What season is it",            0,0,0,0 },
    { 0,0, "Spazio SD","SD space",       "Quanto spazio ho sulla SD","How much SD space is free",0,0,0,0 },
    { 0,0, "RAM libera","Free RAM",      "Quanta RAM libera ho","How much free RAM do I have",    0,0,0,0 },
    { 0,0, "Rete","Network",             "A che rete sono connesso","What network am I on",       0,0,0,0 },
    { 0,0, "Acceso da","Uptime",         "Da quanto sei acceso","What is your uptime",           0,0,0,0 },
    { 0,0, "Versione","Version",         "Che versione di NucleoOS e","What NucleoOS version is this", 0,0,0,0 },
    { 0,0, "Cosa sai fare","Skills",     "Cosa sai fare","What can you do",                       0,0,0,0 },
    // -- Calcolo (1): the calculator + finance helpers --
    { 1,2, "Moltiplica","Multiply",      "Quanto fa %s per %s","How much is %s times %s",         "Primo numero","First number","Secondo numero","Second number" },
    { 1,2, "Dividi","Divide",            "Quanto fa %s diviso %s","How much is %s divided by %s", "Dividendo","Dividend","Divisore","Divisor" },
    { 1,2, "Somma","Add",                "Quanto fa %s piu %s","How much is %s plus %s",          "Primo numero","First number","Secondo numero","Second number" },
    { 1,2, "Sottrai","Subtract",         "Quanto fa %s meno %s","How much is %s minus %s",        "Primo numero","First number","Secondo numero","Second number" },
    { 1,2, "Percentuale","Percentage",   "Quanto e il %s%% di %s","What is %s%% of %s",           "Percentuale","Percent","Totale","Total" },
    { 1,2, "Sconto","Discount",          "Quanto costa %s euro con sconto del %s%%","What is %s euro with %s%% discount","Prezzo euro","Price euro","Sconto %%","Discount %%" },
    { 1,2, "IVA","VAT",                  "Quanto e il %s%% di IVA su %s euro","What is %s%% VAT on %s euro","IVA %%","VAT %%","Imponibile","Net amount" },
    { 1,2, "Potenza","Power",            "Quanto fa %s elevato a %s","What is %s to the power %s","Base","Base","Esponente","Exponent" },
    { 1,1, "Radice","Square root",       "Radice quadrata di %s","Square root of %s",             "Numero","Number",0,0 },
    { 1,2, "Logaritmo","Logarithm",      "Logaritmo in base %s di %s","Logarithm base %s of %s",  "Base","Base","Numero","Number" },
    { 1,2, "MCD","GCD",                  "Massimo comun divisore di %s e %s","Greatest common divisor of %s and %s","Primo","First","Secondo","Second" },
    { 1,2, "mcm","LCM",                  "Minimo comune multiplo di %s e %s","Least common multiple of %s and %s","Primo","First","Secondo","Second" },
    { 1,1, "Fattoriale","Factorial",     "Fattoriale di %s","Factorial of %s",                    "Numero","Number",0,0 },
    { 1,1, "Numero primo?","Is prime?",  "Il numero %s e primo","Is %s a prime number",           "Numero","Number",0,0 },
    { 1,1, "In romano","To roman",       "Scrivi %s in numeri romani","Write %s in roman numerals","Numero","Number",0,0 },
    { 1,1, "In binario","To binary",     "Converti %s in binario","Convert %s to binary",         "Numero","Number",0,0 },
    // -- Geometria (2): geometry + physics solvers --
    { 2,1, "Area cerchio","Circle area", "Area di un cerchio di raggio %s","Area of a circle radius %s","Raggio","Radius",0,0 },
    { 2,1, "Circonferenza","Perimeter",  "Circonferenza di un cerchio di raggio %s","Circumference of a circle radius %s","Raggio","Radius",0,0 },
    { 2,2, "Area rettang.","Rect area",  "Area di un rettangolo %s per %s","Area of a rectangle %s by %s","Base","Width","Altezza","Height" },
    { 2,2, "Area triangolo","Triangle",  "Area di un triangolo base %s altezza %s","Area of a triangle base %s height %s","Base","Base","Altezza","Height" },
    { 2,2, "Ipotenusa","Hypotenuse",     "Ipotenusa con cateti %s e %s","Hypotenuse with legs %s and %s","Cateto 1","Leg 1","Cateto 2","Leg 2" },
    { 2,1, "Volume cubo","Cube vol.",    "Volume di un cubo di lato %s","Volume of a cube side %s",  "Lato","Side",0,0 },
    { 2,1, "Volume sfera","Sphere vol.", "Volume di una sfera di raggio %s","Volume of a sphere radius %s","Raggio","Radius",0,0 },
    { 2,2, "Legge di Ohm","Ohm law",     "Tensione con %s ohm e %s ampere","Voltage with %s ohm and %s amp","Resistenza ohm","Resistance ohm","Corrente A","Current A" },
    { 2,2, "Potenza el.","Elec power",   "Potenza elettrica con %s volt e %s ampere","Electrical power with %s volt and %s amp","Tensione V","Voltage V","Corrente A","Current A" },
    { 2,2, "Velocita","Speed",           "Velocita per %s km in %s ore","Speed for %s km in %s hours","Distanza km","Distance km","Tempo h","Time h" },
    // -- Conversioni (3) --
    { 3,1, "Miglia>km","Miles>km",       "Quanti km sono %s miglia","How many km is %s miles",    "Miglia","Miles",0,0 },
    { 3,1, "km>miglia","km>miles",       "Quante miglia sono %s km","How many miles is %s km",    "Chilometri","Kilometers",0,0 },
    { 3,1, "Pollici>cm","Inch>cm",       "Quanti cm sono %s pollici","How many cm is %s inches",   "Pollici","Inches",0,0 },
    { 3,1, "C>F","C>F",                  "Quanti gradi F sono %s gradi C","How many F is %s C",    "Gradi C","Degrees C",0,0 },
    { 3,1, "F>C","F>C",                  "Quanti gradi C sono %s gradi F","How many C is %s F",    "Gradi F","Degrees F",0,0 },
    { 3,1, "kg>libbre","kg>lb",          "Quante libbre sono %s kg","How many pounds is %s kg",   "Chilogrammi","Kilograms",0,0 },
    { 3,1, "Byte>KB/MB","Bytes>KB/MB",   "Converti %s byte in kilobyte e megabyte","Convert %s bytes to KB and MB","Byte","Bytes",0,0 },
    { 3,1, "Ettari>m2","Hectares>m2",    "Quanti metri quadri sono %s ettari","How many square meters is %s hectares","Ettari","Hectares",0,0 },
    { 3,2, "Generica","Generic",         "Converti %s in %s","Convert %s to %s",                  "Quantita es 5 m","Amount e.g. 5 m","Unita es cm","Unit e.g. cm" },
    // -- Meteo (4) --
    { 4,1, "Citta","City",               "Meteo a %s","Weather in %s",                            "Citta","City",0,0 },
    { 4,1, "Domani","Tomorrow",          "Che tempo fa domani a %s","Weather tomorrow in %s",     "Citta","City",0,0 },
    // -- Agenda (5) --
    { 5,0, "Impegni oggi","Today",       "Che impegni ho oggi","What are my events today",        0,0,0,0 },
    { 5,2, "Promem. domani","Tomorrow",  "Ricordami %s domani alle %s","Remind me %s tomorrow at %s","Cosa","What","Ora HH:MM","Time HH:MM" },
    { 5,2, "Promem. oggi","Today rem.",  "Ricordami %s oggi alle %s","Remind me %s today at %s",  "Cosa","What","Ora HH:MM","Time HH:MM" },
    // -- App e File (6) --
    { 6,0, "Apri Musica","Open Music",   "Apri la musica","Open the music",                       0,0,0,0 },
    { 6,0, "Apri Radio","Open Radio",    "Apri la radio","Open the radio",                         0,0,0,0 },
    { 6,0, "Apri Foto","Open Photos",    "Apri le foto","Open the photos",                        0,0,0,0 },
    { 6,0, "Apri Calend.","Open Cal.",   "Apri il calendario","Open the calendar",               0,0,0,0 },
    { 6,0, "Apri Note","Open Notes",     "Apri le note","Open notes",                             0,0,0,0 },
    { 6,0, "Apri SSH","Open SSH",        "Apri SSH","Open SSH",                                   0,0,0,0 },
    // NOTA RAPIDA: sentinella @note in p1 (slots=0) -> activate_leaf apre l'editor con path timestamp
    { 6,0, "Nota rapida","Quick note",   0,0,  "@note",0,  0,0 },
    // EDITOR leaf: slots=1 collects the PATH, then "@editor" (sentinel in p2) opens the full-screen
    // textarea instead of sending a query; the typed content is written to that path on Ctrl+S.
    { 6,1, "Crea file","Create file",    "Crea il file %s","Create the file %s",                  "Percorso /data/..","Path /data/..","@editor","@editor" },
    // -- Sapere (7): knowledge Q&A --
    { 7,1, "Chi e","Who is",             "Chi e %s","Who is %s",                                  "Nome","Name",0,0 },
    { 7,1, "Cos'e","What is",            "Cos'e %s","What is %s",                                 "Argomento","Topic",0,0 },
    { 7,1, "Spiega","Explain",           "Spiegami %s","Explain %s",                              "Argomento","Topic",0,0 },
    { 7,1, "Capitale di","Capital of",   "Qual e la capitale di %s","What is the capital of %s",  "Nazione","Country",0,0 },
    { 7,1, "Formula chim.","Chem form.", "Formula chimica di %s","Chemical formula of %s",        "Sostanza","Substance",0,0 },
    { 7,1, "Capo di stato","Head of st.","Chi e il capo di stato di %s","Who is the head of state of %s","Nazione","Country",0,0 },
    { 7,1, "Esempio codice","Code ex.",  "Scrivimi un esempio di codice %s","Write a code example in %s","Linguaggio","Language",0,0 },
    { 7,0, "Su NucleoOS","About OS",     "Cos'e NucleoOS","What is NucleoOS",                     0,0,0,0 },
    // -- Traduci (8) -- la PAROLA va PRIMA della lingua: il parser del traduttore estrae il residuo tra
    // "traduci"/"translate" e "in inglese"/"to english". Col vecchio "Traduci in inglese: <parola>" la
    // parola finiva dopo i due punti e NON veniva estratta -> "Cosa traduco?". Ora "traduci <parola> in ...".
    { 8,1, "In inglese","To English",    "traduci %s in inglese","translate %s to english",     "Testo","Text",0,0 },
    { 8,1, "In italiano","To Italian",   "traduci %s in italiano","translate %s to italian",     "Testo","Text",0,0 },
};
#define LEAF_N ((int)(sizeof(LEAVES) / sizeof(LEAVES[0])))

// Autocomplete source #3: the ready-to-send skills (slots==0) of the IDEE catalog, so typing "apri"
// ghosts "Apri la musica", "cos" -> "Cosa sai fare", etc. — the device's whole repertoire at your finger.
static bool ready_leaf_complete(const char *pfx, char *out, int cap, int pl)
{
    for (int i = 0; i < LEAF_N; i++) {
        if (LEAVES[i].slots) continue;
        const char *t = s_en ? LEAVES[i].t_en : LEAVES[i].t_it;
        if (!t) continue;
        if ((int)strlen(t) > pl && !strncasecmp(t, pfx, pl)) { snprintf(out, cap, "%s", t); return true; }
    }
    return false;
}

// Leaves are grouped by category, so a linear scan resolves count + the k-th leaf of a category.
static int cat_leaf_count(int c) { int n = 0; for (int i = 0; i < LEAF_N; i++) if (LEAVES[i].cat == c) n++; return n; }
static int cat_leaf_at(int c, int k) { for (int i = 0; i < LEAF_N; i++) if (LEAVES[i].cat == c) { if (k == 0) return i; k--; } return -1; }

// Assemble the form's query (or a live preview) by filling the template's %s with the typed slots.
// preview=true shows unfilled slots as "?" so the bottom line reads as the question taking shape.
static void form_build_query(char *out, size_t n, bool preview)
{
    if (s_form_leaf < 0) { out[0] = 0; return; }
    const Leaf *L = &LEAVES[s_form_leaf];
    const char *t = s_en ? L->t_en : L->t_it;
    const char *a = s_slot[0][0] ? s_slot[0] : (preview ? "?" : "");
    const char *b = s_slot[1][0] ? s_slot[1] : (preview ? "?" : "");
    if      (L->slots >= 2) snprintf(out, n, t, a, b);   // format is OUR constant; a/b are inert args
    else if (L->slots == 1) snprintf(out, n, t, a);
    else                    snprintf(out, n, "%s", t);
}

// ---- tabbed menu: labels + per-tab focusable-row count (Music/Video parity) -------------------
// Short ASCII labels so all five fit the 240/5 = 48px segments and never clip (worst case
// "STATUS" = 36px, centred with 6px margins). Localised IT/EN like the rest of the app.
static const char *const TABS_IT[TAB_N] = { "IDEE", "OGGI", "GUIDA", "IA", "STATO" };
static const char *const TABS_EN[TAB_N] = { "IDEAS", "TODAY", "GUIDE", "AI", "STATUS" };
static const char *tab_label(int i) { return (s_en ? TABS_EN : TABS_IT)[i]; }
// Number of UP/DOWN-navigable rows in a tab. STATO is a read-only readout (0 rows); OGGI is the
// live agenda length (0 => empty state, DOWN does nothing). s_today_n is the cached agenda count.
static int tab_rows(int t)
{
    switch (t) {
        case TAB_IDEE:  return s_idee_cat < 0 ? CAT_N : cat_leaf_count(s_idee_cat);
        case TAB_OGGI:  return s_today_n;
        case TAB_GUIDA: return GUIDE_N;
        case TAB_IA:    return IA_ROWS;
        default:        return 0;            // TAB_STATO: read-only
    }
}
// Send a ready-made query (a deck pick, a leaf prompt, or an assembled form). The visible bubble is
// exactly this text — submit() still applies the calculator chain to what the engine receives.
static void send_query(const char *q)
{
    if (s_busy) { push_meta(s_en ? "One at a time, wait..." : "Una alla volta, attendi...", DIM); return; }   // don't strand pre-filled text
    snprintf(s_input, sizeof(s_input), "%s", q);
    s_ilen = (int)strlen(s_input);
    submit();
}
static void run_suggestion(void) { send_query(cur_sug(s_sug_sel)); }

// ---- lifecycle --------------------------------------------------------------
static void on_tab(void);
static bool on_back(int key);
static const char *chat_hint(void);           // chat-page hint string (used by enter() before its def)
static void editor_open(const char *path);     // full-screen text editor (file creation) — defined below
static void editor_cancel(void);
static void load_today(void);                 // (re)read today's agenda from the OS calendar
static void today_key(int key, char ch);      // OGGI tab: agenda scroll keys
static void draw_today(int ch);               // OGGI tab painter
static void refresh_complications(void);      // rebuild the watch-face complication strip
static void slider_adjust(int delta);         // nudge the focused IA slider (Volume / Luce)
static void menu_key(int key, char ch);       // route a key while the tabbed menu is open
static void menu_hint(void);                  // footer hint for the current menu tab/row
static void draw_menu(int ch);                // paint the tabbed menu (tab bar + active tab body)

// Collapse the IDEE tree back to its category list (called when the menu (re)opens or pages away).
static void reset_idee(void) { s_idee_cat = -1; s_form_leaf = -1; s_form_slot = 0; }

static void enter(void)
{
    // ANIMA draws DIRECT to the panel, so free the launcher's ~32 KB off-screen back-buffer the
    // moment we open: that RAM belongs to the assistant while it runs (L1 index + 30 KB worker + TLS).
    nucleo_screen_release();
    nucleo_app_set_direct_draw(true);
    nucleo_audio_stop();
    nucleo_anima_set_compact_reply(true);   // small screen: cloud answers short & complete (off again in leave)
    load_settings();
    nucleo_anima_init("it");
    // Clear any "a browser LLM is serving" hint the web app may have left set: when the NATIVE app is
    // foreground the device itself is the brain, so AUTO must decide L1 purely on online-key availability.
    // (An explicit user /l1 ON/OFF override still wins — set_external_brain only affects AUTO.)
    nucleo_anima_l1_set_external_brain(false);
    load_chat();                                   // restore the last conversation (empty ring if none)
    s_rown = 0; s_scroll = 0; s_ilen = 0; s_input[0] = 0;
    s_ed_open = false; s_ed_len = 0; s_ed_buf[0] = 0; s_ed_path[0] = 0; s_ed_scroll = 0;
    s_busy = false; s_done = false; s_pending_launch[0] = 0; s_launch_web[0] = 0; s_launch_wait = 0;
    s_last_conf = -1; s_last_tier = ""; s_last_subject[0] = 0;
    s_user_sent = (s_mcount > 0); s_sug_sel = 0; s_awaiting = false; s_clock_min = -1;
    s_hist_count = 0; s_hist_head = 0; s_hist_nav = -1; s_hist_draft[0] = 0;
    s_recent_count = 0; s_recent_head = 0;
    s_last_math = false; s_last_num[0] = 0;
    s_today_n = s_today_count = 0; s_today_hdr[0] = 0; s_complics[0] = 0;
    s_gen = s_gen + 1;
    s_menu_open = false; s_tab = TAB_IDEE; s_mrow = -1; s_edit = false; reset_idee();
    s_focus_cat = 0; memset(s_focus_leaf, 0, sizeof s_focus_leaf);
    nucleo_app_set_tab_handler(on_tab);
    nucleo_app_set_back_handler(on_back);
    if (!s_worker) {
        // 30 KB stack: the offline L1 cascade (anima_query->try_cascade->l1_query->l1_encode, acc[1KB]+w
        // on the stack) overflowed the old 16 KB and panicked on every knowledge query — same root cause
        // fixed on the httpd task. Online HTTPS + mbedTLS cert-chain recursion needs the headroom too.
        // Heap-backed; released when the worker self-deletes (right away on leave if idle, at the end of
        // an in-flight query if busy — see stop_worker), so it only outlives the app by that drain.
        BaseType_t ok = xTaskCreate(anima_worker, "anima_sh", 30720, (void *)(uintptr_t)s_worker_epoch,
                                    tskIDLE_PRIORITY + 2, &s_worker);
        ESP_LOGI(ATAG, "worker %s; heap=%u largest=%u", ok == pdPASS ? "ready" : "CREATE FAILED",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        if (ok != pdPASS) s_worker = nullptr;
    }
    refresh_complications();                       // watch-face glance card (date / SD / next event)
    if (s_mcount) rebuild_rows();                  // restored conversation -> wrap it for display
    if (s_preset[0]) {                             // seeded by another app (e.g. Wi-Fi diagnostics): auto-ask
        snprintf(s_input, sizeof s_input, "%s", s_preset); s_ilen = (int)strlen(s_input); s_preset[0] = 0;
        if (s_worker) submit();
    }
    nucleo_app_set_hint(s_user_sent ? chat_hint()
                                    : (s_en ? "up/dn  1-9 try   tab menu" : "su giu  1-9 prova  tab menu"));
    mark_all_dirty();
    nucleo_app_request_draw();
}

static void leave(void)
{
    save_chat();                                   // remember the conversation for next time
    nucleo_anima_set_compact_reply(false);         // web client (full screen) keeps long answers
    stop_worker();                                 // free the worker's 30 KB heap stack for L1
    d.setFont(&fonts::Font0); d.setTextSize(1);    // restore the framework's default font for the next app
}

// The hint shown on the chat page (depends on whether the welcome deck is up).
static const char *chat_hint(void)
{
    if (deck_active()) return s_en ? "up/dn  1-9 try   tab menu" : "su giu  1-9 prova  tab menu";
    return s_en ? "enter send  -> complete  tab menu" : "invio invia  -> completa  tab menu";
}

// Footer hint for the live menu state: slider-adjust, tab-bar (row -1), or a per-tab row hint.
static void menu_hint(void)
{
    if (s_edit) {
        if (s_tab == TAB_IA && s_mrow == IA_SPEED) {   // mostra il valore % mentre si regola la velocita' voce
            char h[40]; snprintf(h, sizeof h, s_en ? "speed %d%%   l/r   enter ok" : "vel %d%%   sx/dx   invio ok", nucleo_tts_speed());
            nucleo_app_set_hint(h); return;
        }
        nucleo_app_set_hint(s_en ? "l/r adjust   enter ok" : "sx/dx regola   invio ok"); return;
    }
    if (s_tab == TAB_IDEE && s_form_leaf >= 0) {               // adapt: last slot sends, earlier ones advance
        bool last = s_form_slot >= LEAVES[s_form_leaf].slots - 1;
        nucleo_app_set_hint(s_en ? (last ? "type  -> fill  enter send" : "type  -> fill  enter next")
                                 : (last ? "digita  -> riempi  invio invia" : "digita  -> riempi  invio avanti"));
        return;
    }
    if (s_mrow == -1) { nucleo_app_set_hint(s_en ? "l/r tab   down enter   esc" : "sx/dx scheda  giu entra  esc"); return; }
    switch (s_tab) {
        case TAB_IDEE:
            if (s_idee_cat < 0) nucleo_app_set_hint(s_en ? "up/dn  enter open  l/r tab" : "su giu  invio apri  sx/dx");
            else                nucleo_app_set_hint(s_en ? "up/dn  enter send  esc back" : "su giu  invio invia  esc su");
            break;
        case TAB_OGGI:  nucleo_app_set_hint(s_en ? "up/dn scroll   l/r tab"      : "su giu scorri  sx/dx scheda");  break;
        case TAB_GUIDA: nucleo_app_set_hint(s_en ? "up/dn page  1-5 jump  l/r"   : "su giu pag  1-5  sx/dx sch");   break;
        case TAB_IA:    nucleo_app_set_hint(s_en ? "up/dn  enter change  l/r tab" : "su giu  invio cambia sx/dx"); break;
        default:        nucleo_app_set_hint(s_en ? "l/r tab   esc close"          : "sx/dx scheda  esc chiudi");
    }
}

// TAB toggles the full-screen tabbed menu over the chat (Music/Video model: TAB open/close, LEFT/
// RIGHT page the tabs). Always opens on IDEE with the tab bar focused, so the first DOWN dives in.
static void on_tab(void)
{
    s_menu_open = !s_menu_open;
    if (s_menu_open) { s_tab = TAB_IDEE; s_mrow = -1; s_edit = false; s_sug_sel = 0; reset_idee(); load_today(); menu_hint(); }
    else             { nucleo_app_set_hint(chat_hint()); }
    mark_all_dirty();
    nucleo_app_request_draw();
}

// Esc/Back AND Left both route here with the key code, so we tell them apart. LEFT pages the tabs
// backward (the mirror of RIGHT in menu_key) instead of leaving — pressing Left in the menu used to
// close the whole sheet, which read as "the tab system closes". ESC/Back stays hierarchical: slider
// adjust -> row -> tab bar -> close menu. From the chat base, return false so the framework closes ANIMA.
static bool on_back(int key)
{
    if (s_ed_open) {                                        // editor: the launcher routes ',' (Left) and Esc here
        if (key == NK_LEFT) {                               // ',' -> type a literal comma (textarea isn't comma-blind)
            if (s_ed_len < (int)sizeof(s_ed_buf) - 1) { s_ed_buf[s_ed_len++] = ','; s_ed_buf[s_ed_len] = 0; nucleo_app_request_draw(); }
        } else editor_cancel();                             // Esc/backtick -> cancel the editor
        return true;
    }
    if (!s_menu_open) return false;                         // chat base: let the framework close us
    if (s_edit) {
        if (key == NK_LEFT) slider_adjust(-5);              // Left lowers the value
        else                s_edit = false;                // Esc finishes adjusting
        menu_hint(); nucleo_app_request_draw(); return true;
    }
    // IDEE drill-down: when inside a category or a fill-form, BOTH Esc and Left climb one level (the
    // watch "back" gesture) instead of paging tabs — only at the top category list does Left page.
    if (s_tab == TAB_IDEE && (s_form_leaf >= 0 || s_idee_cat >= 0)) {
        if (s_form_leaf >= 0) s_form_leaf = -1;             // form -> back to its leaf list
        else { s_mrow = s_idee_cat; s_idee_cat = -1; }      // leaves -> back to categories (land on it)
        menu_hint(); nucleo_app_request_draw(); return true;
    }
    if (key == NK_LEFT) {                                   // Left = page tabs backward (mirror of Right)
        s_tab = (s_tab + TAB_N - 1) % TAB_N; reset_idee();
        if (s_mrow >= 0) s_mrow = (tab_rows(s_tab) > 0) ? 0 : -1;
        menu_hint(); nucleo_app_request_draw(); return true;
    }
    if (s_mrow >= 0) { s_mrow = -1; menu_hint(); nucleo_app_request_draw(); return true; }   // Esc: row -> tab bar
    s_menu_open = false;                                    // Esc on the tab bar -> close back to the chat
    nucleo_app_set_hint(chat_hint());
    mark_all_dirty();
    nucleo_app_request_draw();
    return true;
}

// Adjust the focused IA slider (Volume / Luce) by delta; the setters clamp to their valid range.
static void slider_adjust(int delta)
{
    if (s_tab != TAB_IA) return;
    if      (s_mrow == IA_VOL)   nucleo_audio_set_volume(nucleo_audio_volume() + delta);
    else if (s_mrow == IA_BRI)   nucleo_app_set_brightness(nucleo_app_brightness() + delta);
    else if (s_mrow == IA_SPEED) nucleo_tts_set_speed(nucleo_tts_speed() + delta);   // velocita' voce ±5%
}

// IDEE tab: the skill catalog, a two-level drill-down. Categories -> ENTER drills into a category's
// leaves -> ENTER on a leaf either sends it (slots==0) or opens a fill-in form (slots>0). Digit keys
// jump within the current level. Esc/Left climb back (handled in on_back).
static void enter_cat(int c)
{
    if (c < 0 || c >= CAT_N) return;
    s_focus_cat = c;
    int n = cat_leaf_count(c), row = s_focus_leaf[c];          // resume the last leaf you used here
    if (row < 0 || row >= n) row = 0;
    s_idee_cat = c; s_mrow = row; s_form_leaf = -1;
    menu_hint(); nucleo_app_request_draw();
}
// Fire a leaf: ready prompts send immediately and close the menu; parametric ones open the form so
// the device ASKS for the values (e.g. the two operands of a multiplication) rather than inventing them.
static void activate_leaf(int li)
{
    if (li < 0) return;
    const Leaf *L = &LEAVES[li];
    if (L->slots == 0) {
        // mark_all_dirty() before send so the chat header fully repaints even if the send is deferred
        // (busy -> only a meta line, which alone wouldn't clear the old tab bar).
        s_menu_open = false; reset_idee();
        nucleo_app_set_hint(chat_hint()); mark_all_dirty();
        // NOTA RAPIDA: sentinella @note in p1_it -> genera path con timestamp e apre l'editor direttamente.
        // Non invia nessuna query; l'utente scrive il contenuto e Ctrl+S salva il file.
        if (L->p1_it && !strcmp(L->p1_it, "@note")) {
            time_t now = time(NULL); struct tm *tm = localtime(&now);
            char path[80];
            if (tm && now > 1672531200)
                snprintf(path, sizeof path, "/data/note/%04d%02d%02d_%02d%02d.txt",
                         tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                         tm->tm_hour, tm->tm_min);
            else
                snprintf(path, sizeof path, "/data/note/nota.txt");
            mkdir(NUCLEO_SD_MOUNT "/data", 0775);
            mkdir(NUCLEO_SD_MOUNT "/data/note", 0775);
            editor_open(path);
            return;
        }
        send_query(s_en ? L->t_en : L->t_it);
    } else {
        s_form_leaf = li; s_form_slot = 0; s_slot[0][0] = s_slot[1][0] = 0;
        menu_hint(); nucleo_app_request_draw();
    }
}
// Keys while a fill-in form is open: type into the active slot, ENTER advances (or sends on the last
// slot), DEL backspaces (and steps back a slot when empty), UP/DOWN hop between slots.
static void idee_form_key(int key, char ch)
{
    const Leaf *L = &LEAVES[s_form_leaf];
    int sl = s_form_slot;
    // Right accepts the recent-value ghost (one-press fill); with no ghost it falls through to type '/'.
    if (key == NK_RIGHT && s_slot[sl][0]) {
        char g[40];
        if (slot_autocomplete(s_slot[sl], g, sizeof g)) { snprintf(s_slot[sl], sizeof s_slot[sl], "%s", g); menu_hint(); nucleo_app_request_draw(); return; }
    }
    if (key == NK_ENTER) {
        if (s_slot[sl][0] == 0) { nucleo_app_request_draw(); return; }   // need a value before advancing
        if (sl < L->slots - 1) { s_form_slot++; }                        // -> next slot
        else {                                                           // last slot: assemble + send
            for (int i = 0; i < L->slots; i++) recent_push(s_slot[i]);   // remember values for next time
            // EDITOR leaf (sentinel in p2): instead of sending a query, open the full-screen textarea on
            // the path just typed (slot 0). Content is written on Ctrl+S. (Crea file / Nota rapida.)
            if (L->p2_it && !strcmp(L->p2_it, "@editor")) { editor_open(s_slot[0]); return; }
            char q[A_INMAX]; form_build_query(q, sizeof q, false);
            s_menu_open = false; reset_idee();
            nucleo_app_set_hint(chat_hint()); mark_all_dirty();
            send_query(q);
            return;
        }
    } else if (key == NK_DEL) {
        int n = (int)strlen(s_slot[sl]);
        if (n > 0)        s_slot[sl][n - 1] = 0;
        else if (sl > 0)  s_form_slot--;                                 // empty backspace -> previous slot
    } else if (key == NK_UP)   { if (sl > 0)              s_form_slot--; }
    else if (key == NK_DOWN)   { if (sl < L->slots - 1)   s_form_slot++; }
    else if (ch >= 32 && ch < 127) {
        int n = (int)strlen(s_slot[sl]);
        if (n < (int)sizeof(s_slot[0]) - 1) { s_slot[sl][n] = ch; s_slot[sl][n + 1] = 0; }
    } else return;
    menu_hint(); nucleo_app_request_draw();
}
static inline char lc1(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static inline bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

static void idee_key(int key, char ch)
{
    if (s_form_leaf >= 0) { idee_form_key(key, ch); return; }
    if (s_idee_cat < 0) {                                                // -- category list --
        if (ch >= '1' && ch <= '0' + CAT_N) { enter_cat(ch - '1'); return; }
        if      (key == NK_UP)    s_mrow = (s_mrow > 0) ? s_mrow - 1 : -1;
        else if (key == NK_DOWN)  { if (s_mrow < CAT_N - 1) s_mrow++; }
        else if (key == NK_ENTER && s_mrow >= 0) { enter_cat(s_mrow); return; }
        else if (is_alpha(ch)) {                                         // type-ahead: jump to next match, cycling
            char lc = lc1(ch);
            for (int k = 1; k <= CAT_N; k++) { int i = ((s_mrow < 0 ? -1 : s_mrow) + k + CAT_N) % CAT_N; if (lc1(cat_label(i)[0]) == lc) { s_mrow = i; break; } }
        }
        else return;
    } else {                                                            // -- leaf list of a category --
        int n = cat_leaf_count(s_idee_cat);
        if (ch >= '1' && ch <= '0' + n && ch <= '9') { s_focus_leaf[s_idee_cat] = ch - '1'; activate_leaf(cat_leaf_at(s_idee_cat, ch - '1')); return; }
        if      (key == NK_UP)    s_mrow = (s_mrow > 0) ? s_mrow - 1 : -1;
        else if (key == NK_DOWN)  { if (s_mrow < n - 1) s_mrow++; }
        else if (key == NK_ENTER && s_mrow >= 0) { activate_leaf(cat_leaf_at(s_idee_cat, s_mrow)); return; }
        else if (is_alpha(ch)) {                                         // type-ahead over the leaf labels
            char lc = lc1(ch);
            for (int k = 1; k <= n; k++) { int i = ((s_mrow < 0 ? -1 : s_mrow) + k + n) % n; const Leaf *L = &LEAVES[cat_leaf_at(s_idee_cat, i)]; const char *lab = s_en ? L->l_en : L->l_it; if (lc1(lab[0]) == lc) { s_mrow = i; break; } }
        }
        else return;
        if (s_mrow >= 0) s_focus_leaf[s_idee_cat] = s_mrow;             // remember where you were
    }
    menu_hint(); nucleo_app_request_draw();
}

// ---- full-screen text editor (file creation from IDEE) ----------------------
// Open the editor on the path the "Crea file" form collected (slot 0). Closes the menu so the editor
// owns the whole screen — a plain textarea, exactly what the user asked for.
static void editor_open(const char *path)
{
    while (*path == ' ') path++;
    if (path[0] == '/') snprintf(s_ed_path, sizeof s_ed_path, "%s", path);
    else                snprintf(s_ed_path, sizeof s_ed_path, "/data/%s", path);   // default to /data/
    s_ed_buf[0] = 0; s_ed_len = 0; s_ed_scroll = 0; s_ed_open = true;
    s_form_leaf = -1; s_menu_open = false; reset_idee();
    nucleo_app_set_hint(s_en ? "Enter=newline  Ctrl+S save  Esc cancel"
                             : "Invio=a capo  Ctrl+S salva  Esc annulla");
    mark_all_dirty(); nucleo_app_request_draw();
}
// Write the buffer to the SD path (guarded: absolute "/..." path, no ".."). Creates the parent dir,
// overwrites (an explicit editor save). Stops audio first (sequenziale: una risorsa per volta), pets
// the WDT around the SD I/O, then pushes a confirmation to the chat and closes the editor.
static void editor_save(void)
{
    char reply[120]; bool ok = false;
    if (s_ed_path[0] == '/' && !strstr(s_ed_path, "..")) {
        char full[180]; snprintf(full, sizeof full, NUCLEO_SD_MOUNT "%s", s_ed_path);
        char dir[180];  snprintf(dir, sizeof dir, "%s", full);
        char *slash = strrchr(dir, '/'); if (slash && slash != dir) { *slash = 0; mkdir(dir, 0775); }
        nucleo_audio_stop();                                              // no audio while we touch the SD
        if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();
        FILE *f = fopen(full, "wb");
        if (f) { if (s_ed_len) fwrite(s_ed_buf, 1, (size_t)s_ed_len, f); fclose(f); ok = true; nucleo_anima_note_file(s_ed_path); }
    }
    const char *bn = strrchr(s_ed_path, '/'); bn = bn ? bn + 1 : s_ed_path;
    if (ok) snprintf(reply, sizeof reply, s_en ? "Saved %s (%d chars)." : "Salvato %s (%d caratteri).", bn, s_ed_len);
    else    snprintf(reply, sizeof reply, s_en ? "Couldn't save %s (bad path?)." : "Non riesco a salvare %s (percorso?).", bn);
    s_ed_open = false;
    push_user(s_en ? "[new file]" : "[nuovo file]");
    push_anima(reply, ACC);
    s_user_sent = true;
    nucleo_app_set_hint(chat_hint());
    mark_all_dirty(); nucleo_app_request_draw();
    save_chat();
}
static void editor_cancel(void)
{
    s_ed_open = false;
    nucleo_app_set_hint(chat_hint());
    mark_all_dirty(); nucleo_app_request_draw();
}
// Editor keystrokes: printable -> append; Enter -> newline; DEL -> backspace; Ctrl+S -> save. (Esc and
// the comma key arrive via on_back — the launcher intercepts them — and are handled there: Esc cancels,
// Left/',' types a literal comma so the textarea isn't comma-blind.)
static void editor_key(int key, char ch)
{
    if ((nucleo_kbd_mods() & NK_MOD_CTRL) && (ch == 's' || ch == 'S' || ch == 0x13)) { editor_save(); return; }
    if (key == NK_ENTER) {
        if (s_ed_len < (int)sizeof(s_ed_buf) - 1) { s_ed_buf[s_ed_len++] = '\n'; s_ed_buf[s_ed_len] = 0; }
    } else if (key == NK_DEL) {
        if (s_ed_len > 0) s_ed_buf[--s_ed_len] = 0;
    } else if (ch >= 32 && ch < 127) {
        if (s_ed_len < (int)sizeof(s_ed_buf) - 1) { s_ed_buf[s_ed_len++] = ch; s_ed_buf[s_ed_len] = 0; }
    } else return;
    nucleo_app_request_draw();
}

// GUIDA tab: the manual. The focused "row" IS the page (0..GUIDE_N-1); a number jumps to a page.
static void guida_key(int key, char ch)
{
    if      (ch >= '1' && ch <= '0' + GUIDE_N) s_mrow = ch - '1';
    else if (key == NK_UP)   s_mrow = (s_mrow > 0) ? s_mrow - 1 : -1;
    else if (key == NK_DOWN) { if (s_mrow < GUIDE_N - 1) s_mrow++; }
    else return;
    menu_hint(); nucleo_app_request_draw();
}

// IA (settings) tab: the carousel of toggles/chips/sliders/action — same widgets as Music/Video.
static void ia_key(int key)
{
    if (s_edit) {                                           // slider adjust mode
        if      (key == NK_RIGHT || key == NK_UP) slider_adjust(+5);
        else if (key == NK_DOWN)                  slider_adjust(-5);
        else if (key == NK_ENTER)                 s_edit = false;
        menu_hint(); nucleo_app_request_draw(); return;     // hint aggiorna il % della velocita' live
    }
    if      (key == NK_UP)   s_mrow = (s_mrow > 0) ? s_mrow - 1 : -1;   // row 0 -> back to the tab bar
    else if (key == NK_DOWN) { if (s_mrow < IA_ROWS - 1) s_mrow++; }
    else if (key == NK_ENTER) {
        switch (s_mrow) {
            case IA_ONLINE: s_omode = (s_omode + 1) % 3; apply_online_mode(); save_settings(); break;
            case IA_LANG:   s_en = !s_en; save_settings(); rebuild_rows(); break;   // re-wrap + relabel
            case IA_TEXT:   s_big = !s_big; save_settings(); rebuild_rows(); break; // re-wrap the transcript
            case IA_VOICE:  if (nucleo_tts_available()) nucleo_tts_set_enabled(!nucleo_tts_enabled()); break;
            case IA_SPEED:  s_edit = true; break;            // -> L/R adjust mode (velocita' voce)
            case IA_VOL:    s_edit = true; break;            // -> L/R adjust mode
            case IA_BRI:    s_edit = true; break;
            case IA_CLEAR:  clear_chat(); s_menu_open = false; nucleo_app_set_hint(chat_hint()); nucleo_app_request_draw(); return;
        }
    } else return;
    menu_hint(); nucleo_app_request_draw();
}

// Route a key while the tabbed menu is up. Digit shortcuts fire from anywhere; RIGHT pages the
// tabs forward (LEFT pages backward, but the framework routes it to on_back); with the tab bar
// focused (row -1) DOWN/ENTER dives into the content.
static void menu_key(int key, char ch)
{
    if (s_edit) { ia_key(key); return; }                    // slider adjust owns every key
    if (s_tab == TAB_IDEE && s_form_leaf >= 0) { idee_form_key(key, ch); return; }  // form owns every key
    if (ch >= '1' && ch <= '9') {                           // IDEE picks item N; GUIDA jumps to page N
        if (s_tab == TAB_IDEE)  { idee_key(key, ch);  return; }
        if (s_tab == TAB_GUIDA) { guida_key(key, ch); return; }
    }
    if (key == NK_RIGHT) {                                   // horizontal pager from anywhere
        s_tab = (s_tab + 1) % TAB_N; reset_idee();
        if (s_mrow >= 0) s_mrow = (tab_rows(s_tab) > 0) ? 0 : -1;
        menu_hint(); nucleo_app_request_draw(); return;
    }
    if (s_mrow == -1) {                                       // tab bar focused
        if ((key == NK_DOWN || key == NK_ENTER) && tab_rows(s_tab) > 0)   // dive into the content
            s_mrow = (s_tab == TAB_IDEE && s_idee_cat < 0) ? s_focus_cat : 0;   // IDEE resumes last category
        menu_hint(); nucleo_app_request_draw(); return;
    }
    switch (s_tab) {
        case TAB_IDEE:  idee_key(key, ch);  break;
        case TAB_OGGI:  today_key(key, ch); break;
        case TAB_GUIDA: guida_key(key, ch); break;
        case TAB_IA:    ia_key(key);        break;
        default:        break;                              // TAB_STATO: read-only
    }
}

static void on_key(int key, char ch)
{
    if (s_ed_open)   { editor_key(key, ch); return; }       // the full-screen editor owns every key
    if (s_menu_open) { menu_key(key, ch); return; }

    if (deck_active()) {                                    // suggestion deck owns the keys when idle
        if      (key == NK_UP)    { if (s_sug_sel > 0)         { s_sug_sel--; s_d_body = true; } }
        else if (key == NK_DOWN)  { if (s_sug_sel < SUG_N - 1) { s_sug_sel++; s_d_body = true; } }
        else if (key == NK_ENTER) { run_suggestion(); return; }
        else if (key == NK_RIGHT) cycle_mode();
        else if (ch >= '1' && ch <= '0' + SUG_N) { s_sug_sel = ch - '1'; run_suggestion(); return; }  // 1-9 jump+send
        else if (ch >= 32 && ch < 127) {                   // start typing -> the deck steps aside
            if (s_ilen < A_INMAX - 1) { s_input[s_ilen++] = ch; s_input[s_ilen] = 0; }
            s_d_input = true; s_d_body = true;
        } else return;
        nucleo_app_request_draw(); return;
    }

    // Up/Down: walk the command history (Linux style).
    // Ctrl+Up/Down: scroll the chat view.
    if (key == NK_UP || key == NK_DOWN) {
        if (nucleo_kbd_mods() & NK_MOD_CTRL) {
            if (key == NK_UP)   { if (s_scroll < s_rown - 1) { s_scroll++; s_d_body = true; } }
            if (key == NK_DOWN) { if (s_scroll > 0)          { s_scroll--; s_d_body = true; } }
        } else {
            hist_recall(key == NK_UP ? -1 : +1);
        }
        nucleo_app_request_draw(); return;
    }
    // Right with text: accept the ghost completion if there is one, else fall through (types '/').
    if (key == NK_RIGHT && s_ilen > 0) {
        char g[HIST_LEN];
        if (autocomplete(s_input, g, sizeof g)) {
            snprintf(s_input, sizeof s_input, "%s", g); s_ilen = (int)strlen(s_input);
            s_hist_nav = -1; s_d_input = true; nucleo_app_request_draw(); return;
        }
    }

    if (key == NK_ENTER)      submit();
    else if (key == NK_DEL)  {
        if (s_ilen > 0) { s_input[--s_ilen] = 0; s_d_input = true; s_hist_nav = -1; if (s_ilen == 0 && !s_user_sent) s_d_body = true; }
        else if (s_busy) cancel_query();
    }
    else if (s_ilen == 0 && key == NK_RIGHT) cycle_mode();
    else if (ch >= 32 && ch < 127) {
        if (s_ilen < A_INMAX - 1) { s_input[s_ilen++] = ch; s_input[s_ilen] = 0; s_d_input = true; s_hist_nav = -1; }
        if (s_scroll) { s_scroll = 0; s_d_body = true; }
    }
    else return;
    nucleo_app_request_draw();
}

static void tick(void)
{
    // Deferred app hand-off. Opening a RAM-heavy app (music/recorder) the instant the answer arrived
    // OOMs on this PSRAM-less chip: ANIMA still pins a 30 KB worker stack (+ maybe the ~18 KB L1 index)
    // while the target app needs the 32 KB launcher canvas + its decoder — and the largest free block
    // is only ~21 KB. The transition wedged the launcher task -> TASK_WDT reboot (see /api/logs). So we
    // free ANIMA's heavy RAM FIRST (stop_worker below orphans the worker; the idle task reclaims its
    // stack over the next ticks) and only hand off once the heap has coalesced — or after a short cap.
    if (s_pending_launch[0] && s_launch_wait > 0) {
        nucleo_anima_l1_unload_if_idle();                       // drop the offline index if it was resident
        // FIXED settle (not a heap-size gate): on the 2nd launch the canvas is already freed, so the
        // largest free block can read >=30 KB BEFORE the orphaned worker's 30 KB stack is actually
        // reclaimed by the idle task -> launching then leaves both allocated and the heavy app OOMs/
        // freezes. Counting down a few ticks guarantees the idle task has reclaimed the stack first.
        if (--s_launch_wait == 0) {
            char id[24]; snprintf(id, sizeof(id), "%s", s_pending_launch); s_pending_launch[0] = 0;
            ESP_LOGW(ATAG, "launch fire '%s' largest=%u", id,                  // DIAG: visible in /api/logs
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
            d.setFont(&fonts::Font0); d.setTextSize(1);
            if (!nucleo_app_launch_id(id)) { push_meta(s_en ? "App not on the device." : "Non ho quell'app sul dispositivo.", DIM); s_last_conf = -1; }
        }
        nucleo_app_request_draw();
        return;
    }

    if (s_done) {                                  // worker finished -> show the answer
        s_done = false;
        if (s_done_gen != s_gen) return;           // stale result: drop it
        if (s_menu_open) {                         // pop back to the chat so the reply/launch is visible
            s_menu_open = false; s_edit = false;
            nucleo_app_set_hint(chat_hint()); mark_all_dirty();
        }
        present_result();
        nucleo_app_request_draw();
        if (s_pending_launch[0]) {                 // a LAUNCH intent: free ANIMA's RAM, then DEFER the hand-off
            nucleo_anima_set_compact_reply(false);  // launch bypasses leave() -> clear the flag so the web client isn't left compact
            stop_worker();                          // launch bypasses on_exit -> orphan the worker (idle frees its 30 KB)
            s_launch_wait = 3;                      // ~0.6 s settle so the orphaned worker's stack is reclaimed first
            ESP_LOGW(ATAG, "launch armed '%s' largest=%u", s_pending_launch,   // DIAG: visible in /api/logs
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        }
        return;
    }
    if (s_ed_open) { if ((++s_blink & 1) == 0) nucleo_app_request_draw(); return; }   // editor: blink the caret only
    if (s_menu_open) return;                                                // the menu is up: no chat animation
    if (s_busy) { s_spin = (s_spin + 1) & 3; s_d_badge = true; nucleo_app_request_draw(); return; }
    time_t now = time(NULL); struct tm *tm = localtime(&now);               // header clock: repaint on a minute change
    int mn = tm ? tm->tm_min : -1;
    if (mn != s_clock_min) { s_clock_min = mn; s_d_hdr = true; nucleo_app_request_draw(); }
    else if ((++s_blink & 1) == 0 && !deck_active()) nucleo_app_request_draw();  // cursor blink (caret cell only)
}

// ---- tabbed menu: the Music/Video tab-bar + settings widgets (ported, ANIMA violet) ----------
// Persistent segmented tab bar across the top 22px. The active tab is a filled ACC capsule with
// INK text; the rest are MUTED (always readable — never the near-invisible DIM). 240/5 = 48px segs.
static void draw_tabbar(int active)
{
    d.fillRect(0, 0, 240, 22, BG);
    d.setFont(&fonts::Font0); d.setTextSize(1);
    const int seg = 240 / TAB_N;                       // 48px per tab
    for (int i = 0; i < TAB_N; i++) {
        const char *t = tab_label(i);
        int x = i * seg, tx = x + (seg - (int)strlen(t) * 6) / 2;
        if (i == active) {
            d.fillRoundRect(x + 2, 2, seg - 4, 17, 8, ACC);
            d.setTextColor(INK, ACC);
        } else {
            d.setTextColor(MUTED, BG);
        }
        d.setCursor(tx, 7); d.print(t);
    }
    d.drawFastHLine(0, 21, 240, LINE);
}

// One settings row — toggle pill / value chip / slider / action chevron — exactly like Music/Video.
// The focused row grows to 46px with an accent rail; neighbours are 30px. Label is size-2 Font0.
enum { SV_TEXT = 0, SV_TOGGLE, SV_SLIDER, SV_ACTION };
static void draw_set_row(int y, bool focus, const char *label, const char *val,
                         int kind, bool on, int slider_val)
{
    int h = focus ? 46 : 30;
    d.fillRoundRect(4, y, 232, h - 2, 9, focus ? CAP : BG);
    if (focus) d.fillRoundRect(4, y + 3, 5, h - 8, 2, ACC);          // accent rail
    d.setFont(&fonts::Font0); d.setTextSize(2);
    d.setTextColor(focus ? FG : MUTED, focus ? CAP : BG);
    d.setCursor(16, y + (h - 16) / 2 - 1); d.print(label);

    if (kind == SV_SLIDER) {
        bool edit = focus && s_edit;
        int sw = focus ? 96 : 60, sh = 12, bx = 230 - sw, vy = y + (h - sh) / 2;
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
        d.fillTriangle(ax, ay - 4, ax, ay + 4, ax + 4, ay, ar);     // chevron
        return;
    }
    if (val && val[0]) {                                            // SV_TEXT value chip
        int vw = (int)strlen(val) * 12 + 14, vh = 22, bx = 230 - vw, vy = y + (h - vh) / 2;
        if (focus) d.fillRoundRect(bx, vy, vw, vh, 6, SURF);
        d.setTextColor(focus ? FG : MUTED, focus ? SURF : BG);
        d.setCursor(bx + 7, vy + 3); d.print(val);
    }
}

// Build the IA tab's eight rows: mode chip, language, text size, voice toggle, voice-SPEED slider,
// volume + screen sliders, and the clear-chat action. ASCII labels (Font0 has no glyphs for accents).
struct IAItem { const char *label; char val[14]; int kind; bool on; int slider; };
static int build_ia(IAItem *it)
{
    memset(it, 0, sizeof(IAItem) * IA_ROWS);
    it[IA_ONLINE].label = "Online"; it[IA_ONLINE].kind = SV_TEXT;
    snprintf(it[IA_ONLINE].val, 14, "%s", s_omode == OM_OFF ? "Off" : s_omode == OM_ONLY ? "Solo" : "On");
    it[IA_LANG].label = s_en ? "Lang" : "Lingua"; it[IA_LANG].kind = SV_TEXT;
    snprintf(it[IA_LANG].val, 14, "%s", s_en ? "EN" : "IT");
    it[IA_TEXT].label = s_en ? "Text" : "Testo"; it[IA_TEXT].kind = SV_TEXT;
    snprintf(it[IA_TEXT].val, 14, "%s", s_big ? (s_en ? "Big" : "Grande") : (s_en ? "Small" : "Piccolo"));
    it[IA_VOICE].label = s_en ? "Voice" : "Voce"; it[IA_VOICE].kind = SV_TOGGLE;
    it[IA_VOICE].on = nucleo_tts_available() && nucleo_tts_enabled();
    // Velocita' di lettura: slider mappato dall'intervallo % [MIN..MAX] a 0..100 per il disegno della barra.
    it[IA_SPEED].label = s_en ? "Speed" : "Velocita"; it[IA_SPEED].kind = SV_SLIDER;
    it[IA_SPEED].slider = (nucleo_tts_speed() - TTS_SPEED_MIN) * 100 / (TTS_SPEED_MAX - TTS_SPEED_MIN);
    it[IA_VOL].label = "Volume"; it[IA_VOL].kind = SV_SLIDER; it[IA_VOL].slider = nucleo_audio_volume();
    it[IA_BRI].label = s_en ? "Light" : "Luce"; it[IA_BRI].kind = SV_SLIDER; it[IA_BRI].slider = nucleo_app_brightness();
    it[IA_CLEAR].label = s_en ? "Clear chat" : "Pulisci"; it[IA_CLEAR].kind = SV_ACTION;
    return IA_ROWS;
}

// Word-wrap `text` (honouring '\n') from (x,y) within maxw px and render it. Returns the y after the
// last line. Shared by the manual; measures with the chosen font so it never clips on either language.
static int draw_wrapped(const char *text, int x, int y, int maxw, unsigned char font, unsigned short col, int lineh)
{
    set_font(font); d.setTextColor(col, BG);
    char t[120];
    const char *ls = text, *p = text;
    while (*p) {
        if (*p == '\n') { int n = (int)(p - ls); if (n > 119) n = 119; memcpy(t, ls, n); t[n] = 0; d.setCursor(x, y); d.print(t); y += lineh; p++; ls = p; continue; }
        if (*p == ' ' && p == ls) { p++; ls = p; continue; }
        const char *we = p; while (*we == ' ') we++; while (*we && *we != ' ' && *we != '\n') we++;
        if (meas(ls, (int)(we - ls)) <= maxw) { p = we; continue; }
        if (p == ls) {                                       // a single word longer than the line -> hard split
            const char *q = ls;
            while (q < we) {
                int take = 1;
                while (q + take <= we && meas(q, take) <= maxw) take++;
                take--;
                if (take < 1) take = 1;
                int n = take; memcpy(t, q, n); t[n] = 0; d.setCursor(x, y); d.print(t); y += lineh; q += take;
            }
            ls = we; p = we;
        } else {
            int n = (int)(p - ls); if (n > 119) n = 119; memcpy(t, ls, n); t[n] = 0; d.setCursor(x, y); d.print(t); y += lineh;
            while (*p == ' ') p++;
            ls = p;
        }
    }
    if (p > ls) { int n = (int)(p - ls); if (n > 119) n = 119; memcpy(t, ls, n); t[n] = 0; d.setCursor(x, y); d.print(t); y += lineh; }
    return y;
}

// ---- the navigable manual (GUIDA tab) ---------------------------------------
// A compact handbook: intro, keys, what to ask, modes, tricks. UP/DOWN flip pages, 1-5 jump. Every
// body is hand-fit to <=5 lines so it always clears the 240x97 body region — no clipped text.
typedef struct { const char *title, *body; } GuidePage;
// Each line is hand-kept <=21 chars so it never wraps in proportional Font2 (~9-11px/char, 224px
// budget) and every page stays <=5 lines — fits y in [42,118), below the title rule, above the footer.
static const GuidePage GUIDE_IT[GUIDE_N] = {
    { "ANIMA",         "Assistente offline.\nScrivi e premi Invio.\nRisponde senza rete.\nIbrido/Online: piu'." },
    { "Tasti",         "Invio invia\n-> completa la riga\nSu/Giu scorre chat\nCtrl+Su = storico\nTAB menu  Esc esci" },
    { "Cosa chiedere", "Ora, meteo, apri app,\ncalcoli, promemoria.\nNon sai? Apri IDEE:\nil catalogo di tutto\ncio' che so fare." },
    { "Modalita",      "Offline: solo qui.\nIbrido: offline poi\nonline se serve.\nOnline: solo rete.\nCambi con Destra." },
    { "Trucchi",       "TAB: catalogo skill.\nInvio entra, Esc su.\n\"...\" chiede i dati.\nG0 1.5s: ovunque.\nAmbra = attendo te." },
};
static const GuidePage GUIDE_EN[GUIDE_N] = {
    { "ANIMA",        "Offline assistant.\nType and press Enter.\nWorks with no net.\nHybrid/Online: more." },
    { "Keys",         "Enter sends\n-> complete the line\nUp/Down scroll chat\nCtrl+Up = history\nTAB menu  Esc back" },
    { "What to ask",  "Time, weather, apps,\nmath, reminders.\nUnsure? Open IDEAS:\nthe catalog of all\nI can do." },
    { "Modes",        "Offline: device only.\nHybrid: offline then\nonline if needed.\nOnline: net only.\nSwitch with Right." },
    { "Tips",         "TAB: skill catalog.\nEnter opens, Esc up.\n\"...\" asks for input.\nG0 1.5s: anywhere.\nAmber = waiting." },
};
static void draw_guide(int ch)
{
    int pg = s_mrow < 0 ? 0 : s_mrow;
    if (pg >= GUIDE_N) pg = GUIDE_N - 1;
    const GuidePage *g = (s_en ? GUIDE_EN : GUIDE_IT) + pg;
    d.setFont(&fonts::Font0); d.setTextSize(1);
    d.setTextColor(ACC, BG); d.setCursor(8, 27); d.print(g->title);
    char pgs[8]; snprintf(pgs, sizeof pgs, "%d/%d", pg + 1, GUIDE_N);
    d.setTextColor(MUTED, BG); d.setCursor(232 - (int)strlen(pgs) * 6, 27); d.print(pgs);
    d.drawFastHLine(8, 38, 224, LINE);
    d.setClipRect(0, 22, 240, ch - 22);                  // never bleed past the body region
    draw_wrapped(g->body, 8, 42, 224, F_MED, FG, 15);
    d.clearClipRect();
    d.setFont(&fonts::Font0); d.setTextSize(1);
}

// ---- IA tab: the settings carousel (Music/Video parity) ---------------------
// Tab bar focused (row -1): a dimmed top-anchored preview, DOWN to engage. Engaged: the focused row
// is centred and enlarged, neighbours peek above/below. Clipped to the body region so nothing bleeds.
static void draw_ia(int ch)
{
    IAItem it[IA_ROWS]; int n = build_ia(it);
    d.setClipRect(0, 22, 240, ch - 22);
    if (s_mrow == -1) {
        int y = 26;
        for (int i = 0; i < n && y < ch - 4; i++) {
            draw_set_row(y, false, it[i].label, it[i].val, it[i].kind, it[i].on, it[i].slider);
            y += 32;
        }
    } else {
        int cy = (24 + ch) / 2, f = s_mrow;
        for (int i = 0; i < n; i++) {
            int dist = i - f, h = (dist == 0) ? 46 : 30, y;
            if (dist == 0)     y = cy - h / 2;
            else if (dist < 0) y = cy - 23 + dist * 30;
            else               y = cy + 23 + (dist - 1) * 30;
            if (y + h > 22 && y < ch)
                draw_set_row(y, i == f, it[i].label, it[i].val, it[i].kind, it[i].on, it[i].slider);
        }
    }
    d.clearClipRect();
}

// ---- STATO tab: read-only on-device diagnostics -----------------------------
// A glanceable readout under the tab bar; 13px lines from y=30 keep all seven inside the body.
// One STATO line: small grey label (Font0) on the left, the value BIG (Font2) so it reads at a glance.
static void stato_row(int y, const char *lbl, const char *val, unsigned short vc)
{
    d.setFont(&fonts::Font0); d.setTextSize(1);
    d.setTextColor(MUTED, BG); d.setCursor(12, y + 4); d.print(lbl);
    d.setFont(&fonts::Font2); d.setTextSize(1);
    char v[48]; snprintf(v, sizeof v, "%s", val);
    while (v[0] && (int)d.textWidth(v) > 232 - 88) v[strlen(v) - 1] = 0;   // clip to the right edge
    d.setTextColor(vc, BG); d.setCursor(88, y); d.print(v);
}
// STATO tab: a watch "system face" — readable Font2 values with status colours, using the full body.
static void draw_stato(int ch)
{
    (void)ch;
    const int step = 16, ymax = 104;
    int y = 24;
    char v[48];

    unsigned ram = (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024);
    snprintf(v, sizeof v, "%u KB", ram);
    stato_row(y, "RAM", v, ram >= 40 ? GRN : AMBER); y += step;          // colour flags a tight heap

    nucleo_storage_refresh(); const nucleo_storage_info_t *st = nucleo_storage_info();
    if (st && st->mounted) snprintf(v, sizeof v, "%.1f GB", st->free_bytes / 1e9);
    else                   snprintf(v, sizeof v, "%s", s_en ? "n/a" : "n/d");
    stato_row(y, "SD", v, FG); y += step;

    const char *ssid = nucleo_setup_ssid(), *ip = nucleo_setup_ip();
    if (ip && ip[0]) snprintf(v, sizeof v, "%s", ip);
    else             snprintf(v, sizeof v, "%s", s_en ? "offline" : "non conn.");
    stato_row(y, "Rete", v, (ip && ip[0]) ? GRN : MUTED);
    if (ip && ip[0] && ssid && ssid[0]) {                                // SSID as a small grey tag, right-aligned
        char sb[18]; snprintf(sb, sizeof sb, "%.16s", ssid);
        d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextColor(DIM, BG);
        d.setCursor(232 - (int)strlen(sb) * 6, y + 4); d.print(sb);
    }
    y += step;

    const char *m = s_omode == OM_OFF ? "Offline" : s_omode == OM_ONLY ? (s_en ? "Online" : "Solo online") : (s_en ? "Hybrid" : "Ibrido");
    snprintf(v, sizeof v, "%s  %s", m, s_en ? "EN" : "IT");
    stato_row(y, s_en ? "Mode" : "Modo", v, s_worker ? FG : AMBER); y += step;   // amber if the worker is down

    long up = (long)(esp_timer_get_time() / 1000000);
    int uh = (int)(up / 3600), um = (int)((up % 3600) / 60);
    char ut[16]; if (uh) snprintf(ut, sizeof ut, "%dh%dm", uh, um); else snprintf(ut, sizeof ut, "%dm", um);
    snprintf(v, sizeof v, s_en ? "%s  %d ev" : "%s  %d oggi", ut, s_today_count);
    stato_row(y, s_en ? "On" : "Acceso", v, FG); y += step;

    if (s_last_tier[0] && y <= ymax) {                                   // last answer's tier + confidence
        snprintf(v, sizeof v, "%s  %d%%", s_last_tier, s_last_conf < 0 ? 0 : s_last_conf);
        stato_row(y, s_en ? "Last" : "Ultima", v, ACC); y += step;
    }
}

// Truncate s in place so it fits `budget` px in the CURRENT font (drops trailing chars). Keeps a long
// prompt from bleeding past its capsule.
static void fit_w(char *s, int budget)
{
    int n = (int)strlen(s);
    while (n > 0 && (int)d.textWidth(s) > budget) s[--n] = 0;
}

// Focused list-row look, shared by the suggestion deck / IDEE / OGGI lists so a selected line reads
// like the settings rows (a raised CAP capsule + violet rail + bright FG text) instead of the old
// flat violet capsule with hard-to-read black text. Sets the text colour for the row; callers print
// the label at x>=12 so it clears the rail. `unfocused_col` is the normal (non-selected) text colour.
static void list_row_focus(int y, int rh, bool foc, unsigned short unfocused_col)
{
    if (foc) {
        d.fillRoundRect(4, y - 1, 232, rh, 4, CAP);     // raised capsule (matches the IA settings rows)
        d.fillRect(6, y + 1, 3, rh - 4, ACC);           // violet accent rail
        d.setTextColor(FG, CAP);
    } else {
        d.setTextColor(unfocused_col, BG);
    }
}

// ---- IDEE tab: the skill catalog, a smartwatch-style drill-down -------------
// Level 0 paints the categories (icon + leaf count + drill chevron); level 1 paints a category's leaves
// (a "..." badge marks the ones that open a fill-in form); the form collects each value with a labelled
// field + caret + step dots and previews the question. Each list shows a scroll rail when it overflows.

// A 16x16 mono-line glyph per category, drawn from primitives (the ASCII font has no icons, but the
// panel does — icons are the single biggest glance-ability win on a watch menu). (x,y) = box top-left.
static void draw_cat_icon(int c, int x, int y, unsigned short col)
{
    switch (c) {
        case 0:  // Sistema: an info "i" in a ring
            d.drawCircle(x + 8, y + 8, 6, col);
            d.fillRect(x + 7, y + 4, 2, 2, col); d.fillRect(x + 7, y + 7, 2, 5, col); break;
        case 1:  // Calcolo: a calculator (screen + 2x2 keys)
            d.drawRoundRect(x + 2, y + 1, 12, 14, 2, col); d.drawFastHLine(x + 4, y + 4, 8, col);
            d.fillRect(x + 4, y + 8, 2, 2, col); d.fillRect(x + 9, y + 8, 2, 2, col);
            d.fillRect(x + 4, y + 11, 2, 2, col); d.fillRect(x + 9, y + 11, 2, 2, col); break;
        case 2:  // Geometria: a right-triangle (set square)
            d.drawTriangle(x + 2, y + 13, x + 13, y + 13, x + 2, y + 3, col);
            d.drawRect(x + 2, y + 11, 3, 2, col); break;                       // right-angle tick
        case 3:  // Conversioni: two opposed arrows (swap)
            d.drawFastHLine(x + 3, y + 5, 8, col); d.fillTriangle(x + 11, y + 3, x + 11, y + 7, x + 14, y + 5, col);
            d.drawFastHLine(x + 5, y + 10, 8, col); d.fillTriangle(x + 5, y + 8, x + 5, y + 12, x + 2, y + 10, col); break;
        case 4:  // Meteo: a cloud
            d.fillCircle(x + 6, y + 9, 3, col); d.fillCircle(x + 10, y + 8, 4, col);
            d.fillCircle(x + 13, y + 9, 2, col); d.fillRect(x + 5, y + 10, 9, 2, col); break;
        case 5:  // Agenda: a calendar
            d.drawRoundRect(x + 2, y + 3, 12, 11, 1, col); d.fillRect(x + 2, y + 3, 12, 3, col);
            d.fillRect(x + 5, y + 1, 2, 3, col); d.fillRect(x + 9, y + 1, 2, 3, col);
            d.fillRect(x + 5, y + 9, 2, 2, col); break;                        // a marked day
        case 6:  // App e File: a 2x2 app grid
            d.fillRect(x + 3, y + 3, 4, 4, col); d.fillRect(x + 9, y + 3, 4, 4, col);
            d.fillRect(x + 3, y + 9, 4, 4, col); d.fillRect(x + 9, y + 9, 4, 4, col); break;
        case 7:  // Sapere: a lightbulb
            d.drawCircle(x + 8, y + 6, 5, col);
            d.drawFastHLine(x + 6, y + 12, 5, col); d.drawFastHLine(x + 7, y + 14, 3, col); break;
        default: // Traduci: a globe
            d.drawCircle(x + 8, y + 8, 6, col);
            d.drawFastHLine(x + 2, y + 8, 13, col); d.drawFastVLine(x + 8, y + 2, 13, col); break;
    }
}

// Slim scroll rail on the right when a list overflows the viewport (the watch "there's more" cue).
static void draw_list_scroll(int ty0, int avail, int total, int first, int shown)
{
    if (shown >= total) return;
    int th = avail * shown / total; if (th < 8) th = 8;
    int tyo = (total > shown) ? first * (avail - th) / (total - shown) : 0;
    d.drawFastVLine(237, ty0, avail, LINE);
    d.fillRect(236, ty0 + tyo, 3, th, MUTED);
}

// Categories: "[icon] N  Label   <count> >". Icon = identity, count + chevron = "drillable".
static void draw_idee_cats(int ch)
{
    d.setFont(&fonts::Font0); d.setTextSize(1);
    const int rh = 24, y0 = 28;
    int maxvis = (ch - y0) / rh; if (maxvis < 1) maxvis = 1;
    int sel = s_mrow, anchor = sel < 0 ? 0 : sel;
    int first = (anchor >= maxvis) ? anchor - maxvis + 1 : 0;
    int shown = (CAT_N - first < maxvis) ? CAT_N - first : maxvis;
    d.setClipRect(0, 22, 240, ch - 22);
    int y = y0;
    for (int i = first; i < CAT_N && i < first + maxvis; i++) {
        bool foc = (i == sel);
        list_row_focus(y, rh, foc, FG);
        draw_cat_icon(i, 8, y + 4, foc ? ACC : MUTED);          // per-category glyph
        char line[40]; snprintf(line, sizeof line, "%d  %s", i + 1, cat_label(i));
        fit_w(line, 158);                                       // x 30..188; room for count + chevron
        d.setTextSize(foc ? 2 : 1);
        d.setTextColor(FG, foc ? CAP : BG); d.setCursor(30, foc ? y + 4 : y + 8); d.print(line);
        char cnt[6]; snprintf(cnt, sizeof cnt, "%d", cat_leaf_count(i));
        d.setTextSize(1);
        d.setTextColor(foc ? FG : MUTED, foc ? CAP : BG);
        d.setCursor(210 - (int)strlen(cnt) * 6, y + 8); d.print(cnt);
        int axx = 224, ayy = y + rh / 2;
        d.fillTriangle(axx, ayy - 4, axx, ayy + 4, axx + 5, ayy, foc ? ACC : MUTED);
        y += rh;
    }
    d.clearClipRect();
    draw_list_scroll(y0, maxvis * rh, CAT_N, first, shown);
}

// Leaves of the open category, under a "[icon] < Category   N" breadcrumb (the icon carries down so the
// drill feels continuous). A trailing "..." marks leaves that open a fill-in form.
static void draw_idee_leaves(int ch)
{
    int c = s_idee_cat, n = cat_leaf_count(c);
    draw_cat_icon(c, 6, 22, ACC);                 // breadcrumb icon carries the category identity down
    d.setFont(&fonts::Font0); d.setTextSize(1);
    char hd[40]; snprintf(hd, sizeof hd, "< %s", cat_label(c));
    d.setTextColor(ACC, BG); d.setCursor(28, 27); d.print(hd);
    char cc[6]; snprintf(cc, sizeof cc, "%d", n);
    d.setTextColor(MUTED, BG); d.setCursor(232 - (int)strlen(cc) * 6, 27); d.print(cc);
    d.drawFastHLine(8, 40, 224, LINE);

    d.setFont(&fonts::Font0); d.setTextSize(1);
    const int rh = 24, y0 = 44;
    int maxvis = (ch - y0) / rh; if (maxvis < 1) maxvis = 1;
    int sel = s_mrow, anchor = sel < 0 ? 0 : sel;
    int first = (anchor >= maxvis) ? anchor - maxvis + 1 : 0;
    int shown = (n - first < maxvis) ? n - first : maxvis;
    d.setClipRect(0, 42, 240, ch - 42);
    int y = y0;
    for (int i = first; i < n && i < first + maxvis; i++) {
        const Leaf *L = &LEAVES[cat_leaf_at(c, i)];
        bool foc = (i == sel);
        list_row_focus(y, rh, foc, FG);
        char line[44]; snprintf(line, sizeof line, "%d  %s%s", i + 1, s_en ? L->l_en : L->l_it, L->slots ? " ..." : "");
        fit_w(line, 216);
        d.setTextSize(foc ? 2 : 1);
        d.setTextColor(FG, foc ? CAP : BG); d.setCursor(12, foc ? y + 4 : y + 8); d.print(line);
        y += rh;
    }
    d.clearClipRect();
    draw_list_scroll(y0, maxvis * rh, n, first, shown);
}

// The fill-in form, redesigned as a full-screen FOCUS wizard (one field at a time, BIG type): a slim
// breadcrumb + step dots up top, then the field's question in a large font, a tall full-width value box
// with size-2 digits + a one-press recent-value ghost, and a live preview of the question taking shape.
// Showing a single slot huge (instead of cramming both small) is the readability win on this 240px panel.
static void draw_idee_form(int ch)
{
    const Leaf *L = &LEAVES[s_form_leaf];
    int s = s_form_slot;

    // -- breadcrumb: icon + "Cat > Leaf" (left), step dots (right) --
    draw_cat_icon(L->cat, 6, 22, ACC);
    d.setFont(&fonts::Font0); d.setTextSize(1);
    char hd[52]; snprintf(hd, sizeof hd, "%s > %s", cat_label(L->cat), s_en ? L->l_en : L->l_it);
    while (hd[0] && (int)d.textWidth(hd) > 150) hd[strlen(hd) - 1] = 0;
    d.setTextColor(ACC, BG); d.setCursor(28, 27); d.print(hd);
    if (L->slots > 1) {                                          // step dots only matter for multi-field forms
        char sp[12]; snprintf(sp, sizeof sp, "%d/%d", s + 1, L->slots);
        d.setTextColor(MUTED, BG); d.setCursor(186, 27); d.print(sp);
        for (int i = 0; i < L->slots; i++) {
            int dx = 224 - (L->slots - 1 - i) * 11, dy = 30;
            if      (i <  s) d.fillCircle(dx, dy, 3, GRN);
            else if (i == s) d.fillCircle(dx, dy, 4, ACC);
            else             d.drawCircle(dx, dy, 3, MUTED);
        }
    }
    d.drawFastHLine(8, 40, 224, LINE);

    // -- BIG question label (this slot's field) --
    const char *pl = (s == 0) ? (s_en ? L->p1_en : L->p1_it) : (s_en ? L->p2_en : L->p2_it);
    char lab[40]; snprintf(lab, sizeof lab, "%s", pl ? pl : "");
    d.setFont(&fonts::FreeSans9pt7b);
    while (lab[0] && (int)d.textWidth(lab) > 224) lab[strlen(lab) - 1] = 0;
    d.setTextColor(FG, BG); d.setCursor(10, 44); d.print(lab);

    // -- tall full-width value box with size-2 input + recent-value ghost --
    const int bx = 8, bw = 224, by = 64, bh = 38;
    d.fillRoundRect(bx, by, bw, bh, 8, CAP);
    d.fillRect(bx + 3, by + 5, 4, bh - 10, ACC);                // active rail
    const int vx = bx + 14, vy = by + (bh - 32) / 2;            // size-2 Font2 glyph is ~32px tall
    d.setFont(&fonts::Font2);
    int cx = vx;
    if (s_slot[s][0]) {
        d.setTextSize(2); d.setTextColor(FG, CAP); d.setCursor(vx, vy); d.print(s_slot[s]);
        cx = vx + (int)d.textWidth(s_slot[s]); if (cx > bx + bw - 8) cx = bx + bw - 8;
        char g[40];                                             // ghost: a past value with this prefix
        if (slot_autocomplete(s_slot[s], g, sizeof g) && cx < bx + bw - 36) {
            d.setTextColor(DIM, CAP); d.setCursor(cx + 4, vy); d.print(g + strlen(s_slot[s]));
        }
    } else {
        d.setTextSize(1); d.setTextColor(DIM, CAP);
        d.setCursor(vx, by + (bh - 16) / 2); d.print(s_en ? "type..." : "scrivi...");
    }
    d.fillRect(cx + 2, vy, 3, 30, GRN);                          // big caret

    // -- live preview of the assembled question (fills as you type) --
    char q[A_INMAX]; form_build_query(q, sizeof q, true);
    char pv[96]; snprintf(pv, sizeof pv, "%s", q);
    d.setFont(&fonts::Font0); d.setTextSize(1);
    while (pv[0] && (int)d.textWidth(pv) > 224) pv[strlen(pv) - 1] = 0;
    int py = by + bh + 8; if (py > ch - 10) py = ch - 10;
    d.setTextColor(MUTED, BG); d.setCursor(8, py); d.print(pv);
    d.setTextSize(1);                                            // leave global size at the default
}

static void draw_idee(int ch)
{
    if (s_form_leaf >= 0) { draw_idee_form(ch);   return; }
    if (s_idee_cat < 0)   { draw_idee_cats(ch);   return; }
    draw_idee_leaves(ch);
}

// ---- Today/agenda + watch-face complications (OS calendar integration) -------
// Short weekday/month names for the glance card and the Today header (ASCII, both languages).
static const char *WD3_IT[] = { "dom", "lun", "mar", "mer", "gio", "ven", "sab" };
static const char *MO3_IT[] = { "gen", "feb", "mar", "apr", "mag", "giu", "lug", "ago", "set", "ott", "nov", "dic" };
static const char *WD3_EN[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *MO3_EN[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

// Soonest event today-or-later, as a short "HH:MM text" (today) or "DD/MM text" (future). For the
// complication strip. Transient malloc of the calendar file (freed before return), same as agenda.
static bool next_event(char *out, size_t n)
{
    out[0] = 0;
    time_t now = time(NULL); struct tm t; localtime_r(&now, &t);
    char today[16]; snprintf(today, sizeof today, "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    FILE *f = fopen(NUCLEO_SD_MOUNT "/system/config/calendar.json", "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = (sz > 0 && sz < 200000) ? (char *)malloc((size_t)sz + 1) : nullptr;
    bool ok = false;
    if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz) {
        buf[sz] = 0;
        cJSON *root = cJSON_Parse(buf);
        cJSON *evs = root ? cJSON_GetObjectItem(root, "events") : nullptr;
        if (evs && cJSON_IsObject(evs)) {
            char bestk[16] = ""; cJSON *it;
            cJSON_ArrayForEach(it, evs) {                                 // pick the smallest key >= today with events
                const char *k = it->string;
                if (!k || strcmp(k, today) < 0) continue;
                if (!cJSON_IsArray(it) || cJSON_GetArraySize(it) == 0) continue;
                if (!bestk[0] || strcmp(k, bestk) < 0) snprintf(bestk, sizeof bestk, "%s", k);
            }
            if (bestk[0]) {
                cJSON *day = cJSON_GetObjectItem(evs, bestk);
                cJSON *ev  = cJSON_GetArrayItem(day, 0);
                const cJSON *tmj = cJSON_GetObjectItem(ev, "time"), *tx = cJSON_GetObjectItem(ev, "text");
                const char *ts = cJSON_IsString(tmj) ? tmj->valuestring : "", *txs = cJSON_IsString(tx) ? tx->valuestring : "";
                char raw[72];
                if (!strcmp(bestk, today)) snprintf(raw, sizeof raw, "%s%s%s", ts, ts[0] ? " " : "", txs);
                else { int yy, mm, dd; if (sscanf(bestk, "%d-%d-%d", &yy, &mm, &dd) == 3) snprintf(raw, sizeof raw, "%d/%d %s", dd, mm, txs); else snprintf(raw, sizeof raw, "%s", txs); }
                ascii_fold(raw, out, (int)n); ok = out[0] != 0;
            }
        }
        if (root) cJSON_Delete(root);
    }
    free(buf); fclose(f);
    return ok;
}

// Rebuild the deck glance line: just the next reminder now (the date moved to the header and the
// free-SD readout was dropped as clutter — see the STATO tab for storage). Computed at enter()/clear()
// only (one SD read), not per frame — it's a glance, not a live readout. Empty when nothing's upcoming.
static void refresh_complications(void)
{
    char nx[60] = ""; next_event(nx, sizeof nx);
    snprintf(s_complics, sizeof s_complics, "%s", nx);
    if ((int)strlen(s_complics) > 39) s_complics[39] = 0;                 // one Font0 line on the 240px panel
}

// (Re)load today's events (+ the next upcoming) into the Today tile cache. Called when the tile or the
// STATO tab opens, so it's always fresh and never reads the SD during a repaint.
static void load_today(void)
{
    s_today_n = 0; s_today_count = 0; s_today_hdr[0] = 0;
    time_t now = time(NULL); struct tm t; localtime_r(&now, &t);
    char key[16]; snprintf(key, sizeof key, "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    snprintf(s_today_hdr, sizeof s_today_hdr, s_en ? "Today, %s %d %s" : "Oggi, %s %d %s",
             (s_en ? WD3_EN : WD3_IT)[t.tm_wday], t.tm_mday, (s_en ? MO3_EN : MO3_IT)[t.tm_mon]);
    FILE *f = fopen(NUCLEO_SD_MOUNT "/system/config/calendar.json", "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = (sz > 0 && sz < 200000) ? (char *)malloc((size_t)sz + 1) : nullptr;
    if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz) {
        buf[sz] = 0;
        cJSON *root = cJSON_Parse(buf);
        cJSON *evs = root ? cJSON_GetObjectItem(root, "events") : nullptr;
        cJSON *today = evs ? cJSON_GetObjectItem(evs, key) : nullptr;
        if (today && cJSON_IsArray(today)) {
            cJSON *ev;
            cJSON_ArrayForEach(ev, today) {
                if (s_today_n >= TODAY_MAX) break;
                const cJSON *tmj = cJSON_GetObjectItem(ev, "time"), *tx = cJSON_GetObjectItem(ev, "text");
                const char *ts = cJSON_IsString(tmj) ? tmj->valuestring : "", *txs = cJSON_IsString(tx) ? tx->valuestring : "";
                char line[72]; if (ts[0]) snprintf(line, sizeof line, "%s  %s", ts, txs); else snprintf(line, sizeof line, "%s", txs);
                ascii_fold(line, s_today[s_today_n++], 72);
            }
            s_today_count = s_today_n;
        }
        if (evs && cJSON_IsObject(evs) && s_today_n < TODAY_MAX) {        // append the soonest FUTURE event as a peek
            char bestk[16] = ""; cJSON *it;
            cJSON_ArrayForEach(it, evs) {
                const char *k = it->string;
                if (!k || strcmp(k, key) <= 0) continue;
                if (!cJSON_IsArray(it) || cJSON_GetArraySize(it) == 0) continue;
                if (!bestk[0] || strcmp(k, bestk) < 0) snprintf(bestk, sizeof bestk, "%s", k);
            }
            if (bestk[0]) {
                cJSON *day = cJSON_GetObjectItem(evs, bestk);
                cJSON *ev  = cJSON_GetArrayItem(day, 0);
                const cJSON *tx = cJSON_GetObjectItem(ev, "text");
                const char *txs = cJSON_IsString(tx) ? tx->valuestring : "";
                int yy, mm, dd; char line[72];
                if (sscanf(bestk, "%d-%d-%d", &yy, &mm, &dd) == 3) snprintf(line, sizeof line, s_en ? "next %d/%d  %s" : "poi %d/%d  %s", dd, mm, txs);
                else snprintf(line, sizeof line, "%s", txs);
                ascii_fold(line, s_today[s_today_n++], 72);
            }
        }
        if (root) cJSON_Delete(root);
    }
    free(buf); fclose(f);
}

static void today_key(int key, char ch)
{
    (void)ch;
    if      (key == NK_UP)   s_mrow = (s_mrow > 0) ? s_mrow - 1 : -1;   // off the top -> tab bar
    else if (key == NK_DOWN) { if (s_mrow < s_today_n - 1) s_mrow++; }
    else return;
    menu_hint(); nucleo_app_request_draw();
}

// OGGI tab: the OS calendar at a glance — today's events + the next upcoming one, under the tab bar.
static void draw_today(int ch)
{
    d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextColor(MUTED, BG);
    d.setCursor(8, 27); d.print(s_today_hdr[0] ? s_today_hdr : (s_en ? "Today" : "Oggi"));
    d.drawFastHLine(8, 38, 224, LINE);
    const int y0 = 42;
    if (s_today_n == 0) {
        d.setFont(&fonts::Font2); d.setTextSize(1); d.setTextColor(MUTED, BG);
        d.setCursor(10, y0 + 8); d.print(s_en ? "No events today" : "Nessun impegno oggi");
        d.setTextColor(DIM, BG); d.setCursor(10, y0 + 30);
        d.print(s_en ? "Ask: remind me ..." : "Chiedi: ricordami ...");
        return;
    }
    d.setFont(&fonts::Font2); d.setTextSize(1);
    const int rh = 18; int sel = s_mrow, maxvis = (ch - y0) / rh; if (maxvis < 1) maxvis = 1;
    int anchor = sel < 0 ? 0 : sel;
    int first = (anchor >= maxvis) ? anchor - maxvis + 1 : 0;
    int y = y0;
    for (int i = first; i < s_today_n && i < first + maxvis; i++) {
        bool foc = (i == sel);
        list_row_focus(y, rh, foc, i >= s_today_count ? MUTED : FG);          // the "next" peek is dimmer
        d.setCursor(12, y + 1); d.print(s_today[i]);
        y += rh;
    }
}

// ---- chat: region painters (direct draw; each self-clears its own box) -------
// Right side of the header: the thinking pulse while busy, otherwise a live clock (the smartwatch
// staple). The last answer's tier/confidence is shown by the bubble rail colour + the STATO tab,
// so the header stays calm and glanceable. Clock hidden until the RTC is past 2023 (pre-NTP epoch).
// Right cluster of the header: the abbreviated date (warm amber) snug to the LEFT of the clock
// (grey), both right-aligned with the cluster edge at x=234. While busy the whole cluster is
// replaced by the "pensa..." pulse. Keeping the date next to the clock (not floating far left)
// and in a distinct colour is the uniformity the launcher header now shares.
static void put_badge(int top)
{
    d.setFont(&fonts::Font0); d.setTextSize(1);
    if (s_busy) {
        static const char *dots[] = { "", ".", "..", "..." };
        char s[16]; snprintf(s, sizeof(s), "pensa%s", dots[s_spin]);
        d.setTextColor(GRN, BG); d.setCursor(234 - (int)strlen(s) * 6, top + 4); d.print(s);
        return;
    }
    time_t now = time(NULL); struct tm *tm = localtime(&now);
    if (!tm || now <= 1672531200) return;                       // pre-NTP: no clock/date yet
    char clk[8]; snprintf(clk, sizeof clk, "%02d:%02d", tm->tm_hour, tm->tm_min);
    int clkx = 234 - (int)strlen(clk) * 6;
    d.setTextColor(MUTED, BG); d.setCursor(clkx, top + 4); d.print(clk);
    char dt[20]; snprintf(dt, sizeof dt, "%s %d %s",
                          (s_en ? WD3_EN : WD3_IT)[tm->tm_wday], tm->tm_mday, (s_en ? MO3_EN : MO3_IT)[tm->tm_mon]);
    d.setTextColor(AMBER, BG);                                   // distinct from the grey clock
    d.setCursor(clkx - 6 - (int)strlen(dt) * 6, top + 4); d.print(dt);
}
// Clear covers the whole date+clock cluster (worst case ~96px) so neither half-erases on a tick.
static void draw_badge(int top) { d.fillRect(112, top, 128, 13, BG); put_badge(top); }

static void draw_header(int top)
{
    d.fillRect(0, top, 240, 18, BG);
    d.setFont(&fonts::Font0); d.setTextSize(1);
    d.setTextColor(ACC, BG); d.setCursor(8, top + 4); d.print("ANIMA");
    const char *ml = s_omode == OM_OFF ? "offline" : s_omode == OM_ONLY ? "online" : "ibrido";
    d.setTextColor(s_omode == OM_OFF ? DIM : s_omode == OM_ONLY ? GRN : ACC, BG);
    d.setCursor(60, top + 4); d.print(ml);
    put_badge(top);                                             // date + clock, right-aligned cluster
    d.drawFastHLine(0, top + 15, 240, LINE);
}

static void draw_deck(int ty0, int avail)
{
    // Glance header: a time-of-day greeting (left) + clock (right) — like a watch's top card — then
    // the starter prompts. su/giu pick, Invio (or 1-9) sends, so the first ask needs no typing.
    time_t now = time(NULL); struct tm *tm = localtime(&now);
    int hr = tm ? tm->tm_hour : 9;
    const char *greet = s_en ? (hr < 12 ? "Good morning" : hr < 18 ? "Good afternoon" : "Good evening")
                             : (hr < 12 ? "Buongiorno"   : hr < 18 ? "Buon pomeriggio" : "Buonasera");
    d.setFont(&fonts::Font2); d.setTextSize(1); d.setTextColor(ACC, BG);
    d.setCursor(8, ty0); d.print(greet);
    if (tm && now > 1672531200) { char hm[8]; snprintf(hm, sizeof hm, "%02d:%02d", tm->tm_hour, tm->tm_min);
                                  d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG);
                                  d.setCursor(232 - (int)strlen(hm) * 6, ty0 + 4); d.print(hm); }
    // Glance line: the next reminder if there's one, otherwise a hint that TAB opens the full skill
    // catalog (so first-timers discover the IDEE tree beyond these quick picks). One dim Font0 line.
    bool reminder = s_complics[0] != 0;
    const char *glance = reminder ? s_complics : (s_en ? "TAB: full skill catalog" : "TAB: catalogo completo");
    d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextColor(reminder ? MUTED : DIM, BG);
    d.setCursor(8, ty0 + 16); d.print(glance);
    d.setFont(&fonts::Font2); d.setTextSize(1);
    if (s_sug_sel < 0) s_sug_sel = 0;
    if (s_sug_sel >= SUG_N) s_sug_sel = SUG_N - 1;
    int rh = 16, top_y = ty0 + 28;
    int maxvis = (ty0 + avail - top_y) / rh; if (maxvis < 1) maxvis = 1;   // scroll so the selection stays on screen
    int first = (s_sug_sel >= maxvis) ? s_sug_sel - maxvis + 1 : 0;
    int yy = top_y;
    for (int i = first; i < SUG_N && i < first + maxvis; i++) {
        bool foc = (i == s_sug_sel);
        list_row_focus(yy, rh, foc, FG);
        char line[48]; snprintf(line, sizeof line, "%s", cur_sug(i)); fit_w(line, 218);
        d.setCursor(12, yy + 1); d.print(line);
        yy += rh;
    }
}

static void render_row(int y, const Row *r)
{
    set_font(r->font);
    int gap = r->first ? 3 : 0, ty = y + gap, base = font_h(r->font);
    char t[216]; int n = r->len; if (n > 215) n = 215; memcpy(t, r->p, n); t[n] = 0;
    if (r->role == R_USER) {
        int w = (int)d.textWidth(t); int x = 232 - w; if (x < 22) x = 22;
        d.setTextColor(r->col, BG); d.setCursor(x, ty); d.print(t);
    } else if (r->role == R_ANIMA) {
        d.fillRect(3, ty, 3, base - 2, r->accent);          // the message's left rail (violet / amber)
        d.setTextColor(r->col, BG); d.setCursor(11, ty); d.print(t);
    } else {
        d.setTextColor(r->col, BG); d.setCursor(8, ty); d.print(t);
    }
}

static void draw_body(int top, int h)
{
    int inH = input_h();
    int ty0 = top + 18, body_bottom = top + h - inH - 1, avail = body_bottom - ty0;
    d.fillRect(0, ty0, 240, avail, BG);
    if (deck_active()) { draw_deck(ty0, avail); return; }
    if (s_rown <= 0) return;
    int maxscroll = s_rown - 1;
    if (s_scroll > maxscroll) s_scroll = maxscroll;
    if (s_scroll < 0) s_scroll = 0;
    int bottom = s_rown - 1 - s_scroll;
    int used = 0, start = bottom;
    for (int i = bottom; i >= 0; i--) {
        int hh = row_h(&s_row[i]);
        if (i != bottom && used + hh > avail) break;        // always keep the bottom row
        used += hh; start = i;
        if (used >= avail) break;
    }
    int y = ty0;
    d.setClipRect(0, ty0, 240, avail);                       // keep the bottom row from bleeding onto the input's top edge
    for (int i = start; i <= bottom; i++) { render_row(y, &s_row[i]); y += row_h(&s_row[i]); }
    d.clearClipRect();
    int shown = bottom - start + 1;                          // slim scrollbar when history overflows
    if (shown < s_rown) {
        int th = avail * shown / s_rown; if (th < 8) th = 8;
        int tymax = avail - th;
        int tyo = (s_rown - 1 - bottom) * tymax / (s_rown - shown);
        d.drawFastVLine(237, ty0, avail, LINE);
        d.fillRect(236, body_bottom - th - tyo, 3, th, MUTED);
    }
}

// Text start x of the input row: right after the ">" prompt with a fixed gap, measured in the live
// chat font (the old hard-coded 22 didn't track the proportional prompt width). The chat font must be
// set before calling. Single source so placeholder, text, caret and ghost all line up.
static const int PROMPT_X = 8;
static int input_x0(void) { return PROMPT_X + (int)d.textWidth(">") + (s_big ? 14 : 6); }

static void draw_input(int top, int h)
{
    int inH = input_h(), in_top = top + h - inH;
    d.fillRect(0, in_top, 240, inH, BG);
    d.drawFastHLine(0, in_top, 240, LINE);
    set_font(chat_font());
    int ty = in_top + 4;
    d.setTextColor(ACC, BG); d.setCursor(PROMPT_X, ty); d.print(">");
    const int x0 = input_x0(), availw = 232 - x0;
    if (s_ilen == 0) {                                       // empty -> a dim placeholder cue (smartwatch style)
        d.setTextColor(DIM, BG); d.setCursor(x0, ty);
        d.print(s_awaiting ? (s_en ? "reply..." : "rispondi...") : (s_en ? "type..." : "scrivi..."));
        return;
    }
    int startc = 0; while (s_input[startc] && (int)d.textWidth(s_input + startc) > availw) startc++;   // scroll to keep the caret visible
    d.setTextColor(FG, BG); d.setCursor(x0, ty); d.print(s_input + startc);
    // Ghost completion: the dim tail of the best match, drawn after the caret. Right accepts it.
    char ghost[HIST_LEN];
    if (autocomplete(s_input, ghost, sizeof ghost)) {
        int cx = x0 + (int)d.textWidth(s_input + startc) + 3;
        if (cx < 230) { d.setTextColor(DIM, BG); d.setCursor(cx, ty); d.print(ghost + s_ilen); }
    }
}

// The caret is a thin bar after the visible input. Toggling just this cell lets the blink animate
// without touching anything else (no flicker). Hidden on the deck and while busy.
static void draw_caret(int top, int h)
{
    if (deck_active() || s_menu_open) return;
    int inH = input_h(), in_top = top + h - inH, ty = in_top + 4;
    set_font(chat_font());
    const int x0 = input_x0(), availw = 232 - x0;
    int startc = 0; while (s_input[startc] && (int)d.textWidth(s_input + startc) > availw) startc++;
    int cx = x0 + (int)d.textWidth(s_input + startc);
    int ch = s_big ? 16 : 13;
    bool show = !s_busy && (s_blink & 2);
    d.fillRect(cx + 1, ty, 2, ch, show ? GRN : BG);
}

// Paint the whole tabbed menu: clear the body, the persistent tab bar, then the active tab.
static void draw_menu(int ch)
{
    d.fillRect(0, 0, 240, ch, BG);
    draw_tabbar(s_tab);
    switch (s_tab) {
        case TAB_IDEE:  draw_idee(ch);  break;
        case TAB_OGGI:  draw_today(ch); break;
        case TAB_GUIDA: draw_guide(ch); break;
        case TAB_IA:    draw_ia(ch);    break;
        case TAB_STATO: draw_stato(ch); break;
    }
}

// Full-screen text editor: a path title bar + the word-wrapped buffer (auto-scrolled so the caret/end
// stays visible) + a blinking caret. Direct-draw, repaints the whole content area each call (cheap: the
// buffer is <=1 KB). The bottom hint line is the framework's (set in editor_open).
static void draw_editor(int top, int h)
{
    d.fillRect(0, top, 240, h, BG);
    // Title bar: the target path, with a live char count on the right.
    d.fillRect(0, top, 240, 17, CAP);
    set_font(F_MED); d.setTextColor(FG, CAP);
    d.setCursor(4, top + 1); d.print(s_ed_path[0] ? s_ed_path : "(file)");
    char cc[16]; snprintf(cc, sizeof cc, "%d", s_ed_len);
    int cw = (int)d.textWidth(cc); d.setTextColor(MUTED, CAP); d.setCursor(238 - cw, top + 1); d.print(cc);

    // Word-wrap the buffer into line segments [off,len), honoring '\n' and hard-splitting over-wide words.
    const int availw = 232, LCAP = 120, lh = font_h(F_MED) + 1;
    static short loff[LCAP], llen[LCAP]; int nl = 0;
    int ls = 0, i = 0, n = s_ed_len;
    while (i <= n && nl < LCAP) {
        if (i == n || s_ed_buf[i] == '\n') {
            if (ls == i) { loff[nl] = (short)ls; llen[nl] = 0; nl++; }     // blank line
            else {
                int seg = ls;
                while (seg < i && nl < LCAP) {
                    int take = i - seg;
                    while (take > 1 && meas(s_ed_buf + seg, take) > availw) take--;
                    loff[nl] = (short)seg; llen[nl] = (short)take; nl++; seg += take;
                }
            }
            if (i == n) break;
            i++; ls = i;
        } else i++;
    }
    // Show the LAST rows that fit (caret-follows-bottom). Empty buffer -> just the caret.
    int by = top + 19, bh = h - 19, maxrows = bh / lh; if (maxrows < 1) maxrows = 1;
    int first = nl > maxrows ? nl - maxrows : 0;
    int y = by; d.setTextColor(FG, BG);
    for (int r = first; r < nl; r++) {
        char tmp[80]; int L = llen[r]; if (L > 79) L = 79;
        memcpy(tmp, s_ed_buf + loff[r], L); tmp[L] = 0;
        d.setCursor(4, y); d.print(tmp);
        if (r == nl - 1 && (s_blink & 1)) { int tw = (int)d.textWidth(tmp); d.fillRect(5 + tw, y, 2, font_h(F_MED), ACC); }
        y += lh;
    }
    if (nl == 0 && (s_blink & 1)) d.fillRect(5, by, 2, font_h(F_MED), ACC);
}

static void draw(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    if (s_ed_open)   { draw_editor(top, h); d.setFont(&fonts::Font0); d.setTextSize(1); return; }
    if (s_menu_open) { draw_menu(h); d.setFont(&fonts::Font0); d.setTextSize(1); return; }

    // Safety net: if the framework ever hands us its (freshly cleared) off-screen canvas instead of
    // the direct path, repaint everything so nothing is left blank.
    if (nucleo_app_is_buffered()) mark_all_dirty();

    if (s_d_hdr)        draw_header(top);
    else if (s_d_badge) draw_badge(top);
    if (s_d_body)       draw_body(top, h);
    if (s_d_input)      draw_input(top, h);
    draw_caret(top, h);

    s_d_hdr = s_d_body = s_d_input = s_d_badge = false;
    d.setFont(&fonts::Font0); d.setTextSize(1);    // leave the global font at the framework default
}

extern "C" void nucleo_register_anima(void)
{
    static const nucleo_app_def_t app = {
        "anima", "ANIMA", "Tools", "Offline assistant: ask in plain Italian",
        'a', ACC, enter, on_key, tick, draw, leave
    };
    nucleo_app_register(&app);
}
