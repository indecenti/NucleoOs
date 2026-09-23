// ANIMA shell: an on-device chat with the offline assistant, right on the Cardputer.
// Type a line, Enter asks ANIMA, the answer scrolls in a transcript. Mirrors what the web
// client gets from GET /api/anima (nucleo_httpd.c): it runs the same nucleo_anima_query()
// and resolves the SYSTEM {value} templates + LAUNCH/TOOL actions locally (system values,
// create_file, set_volume/brightness, add_event calendar reminders). A LAUNCH answer OFFERS the app
// ("Enter = open Music"): Enter hands its id over in RTC and reboots into the full OS, which opens it.
//
// Readability (smartwatch-style): the chat renders in a REAL anti-aliased proportional font
// (FreeSans, M5GFX's built-in GFX font) instead of the cramped 6x8 bitmap — the single biggest
// legibility win on this 240x135 panel. Messages are word-wrapped by PIXEL width and shown as
// chat bubbles: your questions right-aligned in blue, ANIMA's answers left with a colored rail,
// meta notes dim and small. "Compatto" in Settings swaps to the denser 16px Font2 for more lines.
// All visible strings are ASCII-folded on the way in so accented online text never shows tofu in
// the ASCII-only GFX font. A fresh/cleared chat shows a suggestion deck (fn+;/. + Invio) that both
// welcomes and showcases what ANIMA can do; TAB opens the IDEE tab — a drill-down catalog of every
// offline skill, where parametric entries (e.g. a multiplication) open a fill-in form for the values.
//
// Drawing: ANIMA frees the 32 KB shared canvas on enter (the L1 index + TLS need that RAM) and
// pins itself to DIRECT drawing, so it can't use the framework's off-screen composite. Per
// ANTI-FLICKER.md technique 2 everything paints IN PLACE: opaque text over the old pixels, only the
// leftovers cleared (box_text / pill_band), so a scroll, a page flip, a typewriter frame or a menu key
// never blanks a region; one clear happens only on a scene change. The caret blink toggles a single
// bar. The transcript is a word-wrapped row cache rebuilt from a small message ring only when the
// content changes, shown top-anchored: every answer lands on its FIRST row, fn+;/. flip pages, and Enter
// on an empty line opens a full-screen reader (docs/anima-native.md §8). Chrome follows the OS theme
// (THEME_* roles) and the text the OS language.
//
// Solo only: opening ANIMA from the full OS reboots into the dedicated ANIMA Solo boot (enter()), so
// every line below the Solo gate runs on the big `anima-solo` task. nucleo_anima_query() — which can
// reach the online tiers (a blocking HTTPS fetch, seconds) — runs INLINE on that task: the UI loop is
// blocked for the turn, so submit() paints the "thinking" state itself, unsubscribes the task WDT, and
// polls the keyboard during the reveal/voice (docs/anima-native.md §7). There is no worker task.
//
// Keyboard (the Notes-editor rule): the driver delivers ; . , / as arrows that CARRY their character.
// Wherever you write — chat, welcome deck, IDEE fill-in forms, file editor — they TYPE themselves, so
// "2.5", "ciao." or an Italian decimal comma just work and '/' starts a slash command. Their arrow
// meaning needs a modifier (the Cardputer's own arrow layer): fn+; / fn+. page the chat (a row while
// typing; move the deck / hop form fields), fn+/ accepts the ghost completion (on an empty line: cycles
// the online mode, or retries online after an offline "I don't know"), ctrl+; / ctrl+. walk the history. ',' reaches on_back (the launcher routes Left
// there) and is typed as a comma — it never leaves. Only Esc (backtick) leaves, behind a confirm. In
// the tabbed menu's lists (no text entry) the plain arrows still navigate and ',' pages the tabs.
#include "nucleo_app.h"
#include "app_gfx.h"
#include "app_ui.h"       // app_ui_ascii_fold: shared UTF-8 -> ASCII fold for the TFT fonts
#include "nucleo_i18n.h"   // the OS language (settings.json ui.language): ANIMA follows it, TR(it,en)
#include "nucleo_theme.h"  // THEME_* roles: the chrome follows the OS theme (docs/native-ui-kit.md)
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
#include <new>             // placement new: build the query result in place (no by-value copy)
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"   // pet il task-WDT prima della scrittura SD del calendario (anti-reboot)
#include "esp_attr.h"       // RTC_NOINIT_ATTR: carry the seeded question across the ANIMA Solo reboot
#include "esp_app_desc.h"   // esp_app_get_description(): real running-image version (single source of truth)
#include "esp_system.h"     // esp_reset_reason(): the "open <app>" handoff is honoured only after our own reboot
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
// Launcher display name of a native app in the OS language (flash table in launcher_render.cpp); NULL when
// the app keeps its own proper-noun name. Works in any boot — ANIMA Solo registers no other app.
const char *launcher_app_localized_name(const char *id);
}

// Chrome follows the OS theme (docs/native-ui-kit.md §1): the roles, never an RGB565 literal, so AMOLED /
// Hacker Green / Nano Banana recolour ANIMA like every other app. The focused row / active tab is the kit's
// one selection look — an ACC pill with INK text — (the old CAP/SURF capsule greys are gone).
#define BG    THEME_BG
#define FG    THEME_FG
#define MUTED THEME_MUTED
#define DIM   THEME_DIM
#define LINE  THEME_LINE
#define INK   THEME_INK
// Identity + content semantics (named, theme-independent — allowed by the kit): the app's registered
// accent, "live/ok" green, the user-echo blue and the "awaiting your reply" amber.
static const unsigned short ACC = 0x929F /* ANIMA violet */, GRN = 0x8FF3, USR = 0x6E1F, AMBER = 0xFD20;
// Transcript colour ROLE codes (Msg.col / Msg.accent): see col_role() / pal().
static const unsigned short K_FG = 1, K_MUTED = 2, K_DIM = 3, K_ACC = 4, K_GRN = 5, K_USR = 6, K_AMBER = 7;

#define A_INMAX  140           // max input length
#define MSG_TAG  12            // an answer's source + time tag ("web|12.3 s")
#define RECENT_N 8             // recent IDEE form values remembered for the slot ghost (recent_push)
#define TODAY_MAX 10           // OGGI tab: today's events + the next upcoming peek

// ---- session state: ONE heap block, allocated in enter(), freed in leave() -----------------------
// ANIMA only ever runs in its own Solo boot, but .bss is reserved in EVERY boot — the normal OS boots
// where ANIMA never opens paid ~5 KB for these buffers for nothing. So everything sized the app owns
// lives here instead. In the Solo boot it's ~neutral (the same bytes, taken once, early, from a fresh
// heap: no fragmentation). Every access happens after enter() allocated it: the framework callbacks
// (draw/tick/on_key/on_tab/on_back) bail while it is NULL, and the cross-app ask hook
// (nucleo_anima_app_ask) writes RTC memory instead. Keep it that way: no s_ses-> outside the session.
typedef struct {
    anima_result_t res;                // last query's result, built IN PLACE (never copied by value)
    char full[1024];                   // the CURRENT answer, whole (the ring keeps a clipped copy)
    char input[A_INMAX];               // the chat line being typed
    char req[A_INMAX];                 // what the engine is asked (after the calculator chain)
    char hist_draft[A_INMAX];          // the in-progress line, parked while browsing history
    char last_subject[48];             // deductive focus, surfaced in the STATO tab
    char last_num[40];                 // last numeric result (calculator chain)
    char recent[RECENT_N][40];         // recent IDEE form values (session-only)
    char slot[2][40];                  // the values typed into the open IDEE form's slots
    signed char focus_leaf[9];         // last focused leaf row per IDEE category (CAT_N == 9)
    char carry;                        // printable key that ended a turn early -> first char of the next line
    // Calendar cache (cal_refresh): parsed once per change of calendar.json size/mtime, day or language.
    char today[TODAY_MAX][72];         // OGGI lines "HH:MM  text" (folded) + the next upcoming peek
    char today_hdr[40];                // "Oggi, lun 8 giu"
    char complics[80];                 // deck glance line: the next reminder (empty if none)
    char agenda[200];                  // raw "HH:MM text; ..." of today's events (agenda readout)
    char cal_next[72];                 // raw next event today-or-later (deck glance source)
    int  agenda_n;                     // events today (array size, as the agenda readout counts them)
    long cal_size; time_t cal_mtime; char cal_key[12]; bool cal_en, cal_ok;   // cache stamp
    // Transcript view (reader). Top-anchored: vtop = first visible row. vmode FOLLOW pins the newest row to
    // the bottom (a new question), ANCHOR puts the first row of message `anchor` (the answer) at the top so
    // a long answer reads from its start, MANUAL is where the user paged to. Rows are re-wrapped on every
    // push, so the anchor is a ring slot, resolved to a row index at paint time.
    int  vtop;
    signed char vmode, anchor;
    bool reader;                       // full-screen reader: header, input and footer hidden, ~7 rows
    char launch[16];                   // native app id the last answer offers ("Enter = open Music"); "" = none
    char last_tag[MSG_TAG];            // the last answer's source + time (STATO tab)
    bool retry_online;                 // the last answer abstained offline while the network is up: fn+/ = retry online
    bool cloud_note;                   // the blocking turn may reach the cloud: the input row says how long it can take
    // Menu paint cache (flicker-free direct draw): the scene painted last (tab / IDEE level / form / language)
    // — a change = one clear + the static parts; within a scene only the list rows whose content moved repaint.
    int  mscene;
    uint32_t msig[8];                  // what each visible list-row slot shows
    short mleft, mpage;                // where the list's leftover band starts; the GUIDA page on the panel
    bool mfull;                        // this menu paint is a scene paint (static parts too)
    bool clear_confirm, clear_yes;     // "Clear chat" asks first: the kit's confirm card (focus starts on No)
    signed char body_kind;             // what the chat body currently shows on the panel (BK_*): a change = one clear
    unsigned rgen;                     // nucleo_app_repaint_gen() at the last paint: a bump = overlay residue, repaint all
} AnimaSession;
static AnimaSession *s_ses = nullptr;

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
// tag = the answer's source + time ("L1|0.4 s", '|' drawn as a dot), shown discreetly at the end of the answer.
// A meta line pushed with accent MSG_EPHEMERAL (a next-step hint) is display-only: never written to the SD.
#define MSG_EPHEMERAL 1
typedef struct { char text[MSG_TEXT]; unsigned short col, accent; unsigned char role; char tag[MSG_TAG]; } Msg;
// Heap-on-enter (was .bss): the transcript ring is ~5 KB and ANIMA is closed almost always, so keeping it
// resident cost the boot RAM budget for nothing. calloc'd in enter() (inside the exclusive reclaim window),
// freed in leave(); every access is null-guarded so an OOM-on-enter degrades to "no transcript", not a crash.
static Msg *s_msg = nullptr;
static int s_mhead, s_mcount;
// "Risposta corrente INTERA": il ring tiene copie accorciate a MSG_TEXT (cronologia, RAM bassa); l'ultima
// risposta di ANIMA si mostra invece per intero da s_ses->full (fino al cap del motore, 1 KB). s_full_idx
// = slot del ring di quel messaggio (-1 = nessuno) -> rebuild_rows wrappa quel messaggio da s_ses->full.
static int  s_full_idx = -1;
static int  s_reveal   = -1;   // typewriter: -1 = mostra tutto s_ses->full; >=0 = mostra solo i primi N byte (rivelazione graduale stile GPT)
static bool s_exit_confirm = false;   // modale conferma uscita (Esc nel chat base): true = mostra la modale a tutto schermo
extern void launcher_render_hint_bar(void);   // ridipinge il footer SUBITO (il loop framework e' bloccato durante la query inline)

// Wrapped display rows (derived). A row points into a message's text (valid until the next
// rebuild, which every push triggers after writing the message).
enum { F_SMALL = 0 /*Font0 6x8*/, F_MED = 1 /*Font2 16px*/, F_BIG = 2 /*FreeSans9pt7b*/, F_BOLD = 3 /*FreeSansBold9pt7b*/ };
#define ROW_MAX 120
// `first` carries flags: RF_FIRST = first row of its message (3 px gap above). `mi` = the message's ring
// slot, so the view can anchor on a message and the position indicator can count an answer's rows.
enum { RF_FIRST = 1, RF_TAG = 2 /* draw the message's tag at the right end of this row */,
       RF_TAGROW = 4 /* this row IS the tag (it did not fit on the last text row) */ };
typedef struct { const char *p; unsigned short len, col, accent; unsigned char role, font, first, mi; } Row;
static Row *s_row = nullptr;      // heap-on-enter (was .bss ~2 KB), paired with s_msg above
static int s_rown;
enum { V_FOLLOW = 0, V_ANCHOR, V_MANUAL };        // AnimaSession.vmode
enum { BK_NONE = 0, BK_DECK, BK_CHAT };           // AnimaSession.body_kind

// ---- dirty regions (flicker-free direct draw) -------------------------------
// Every chat region paints IN PLACE (opaque text + only the leftover pixels cleared), so a dirty flag costs
// no blank frame. s_d_clear is the one exception: a SCENE change (menu/editor/modal/reader closed, chat
// cleared) wipes the content area once before the regions paint.
static bool s_d_hdr, s_d_body, s_d_input, s_d_badge, s_d_clear;
static void mark_all_dirty(void) { s_d_hdr = s_d_body = s_d_input = s_d_clear = true; }

// ---- input + last-answer state (the text buffers live in s_ses) -------------
static int  s_ilen;
static int  s_last_conf;                         // confidence of the last answer (-1 = none)
static int  s_blink, s_spin;
static bool s_user_sent;                          // false until the first question -> show the deck
static int  s_sug_sel;                            // focused suggestion in the deck / IDEE tab
static bool s_awaiting;                            // last answer was a follow-up question -> input placeholder hints it
static int  s_clock_min = -1;                      // header clock: last minute painted (repaint only when it changes)

// ---- arrow-layer modifiers ----------------------------------------------------
// ; . , / TYPE while writing (see the header note); their arrow meaning needs a modifier. The driver's
// bit names are offset from the PRINTED legends (nucleo_kbd mod_for_xy is best-effort; same note as
// app_pinball.cpp): the key printed "fn" (row 3, left) reports NK_MOD_CTRL, the key printed "ctrl"
// (row 4, left) reports NK_MOD_FN. ANIMA names keys by what is printed on them — if a board ever
// reports them the other way round, swap these two helpers and every hint stays true.
static inline bool key_fn(void)   { return (nucleo_kbd_mods() & NK_MOD_CTRL) != 0; }   // printed "fn": scroll / move
static inline bool key_ctrl(void) { return (nucleo_kbd_mods() & NK_MOD_FN)   != 0; }   // printed "ctrl": history
static inline bool key_mod(void)  { return key_fn() || key_ctrl(); }                   // either: arrow meaning
static int64_t s_toast_until;   // >0 = the footer shows a transient status (mode switch); tick() restores the hint

// ---- command history (shell-style recall) -----------------------------------
// A small ring of the lines the user sent. ctrl+; walks back through them (ctrl+. forward); fn+;/.
// scroll the transcript. Every ANIMA open is a fresh Solo boot, so enter() rebuilds the ring from the
// user's own lines in the restored chat (hist_from_chat) — recall and ghost completion span sessions.
#define HIST_N   8
#define HIST_LEN 120
// Heap-on-enter (freed in leave): the input-history ring is session-only and the app is closed
// most of the time — no reason to hold ~1 KB of .bss at boot. Null-guarded everywhere.
static char (*s_hist)[HIST_LEN] = nullptr;
static int  s_hist_count;                           // entries stored (<= HIST_N)
static int  s_hist_head;                            // ring write index
static int  s_hist_nav = -1;                        // -1 = editing the live draft; 0 = newest, up = older

// nav 0 = most recent. Returns NULL out of range.
static const char *hist_at(int nav)
{
    if (!s_hist || nav < 0 || nav >= s_hist_count) return NULL;
    return s_hist[(s_hist_head - 1 - nav + HIST_N * 2) % HIST_N];
}
static void hist_push(const char *s)
{
    if (!s_hist || !s || !s[0]) return;
    if (s_hist_count) { const char *last = hist_at(0); if (last && !strncmp(last, s, HIST_LEN - 1)) { s_hist_nav = -1; return; } }
    snprintf(s_hist[s_hist_head], HIST_LEN, "%s", s);
    s_hist_head = (s_hist_head + 1) % HIST_N;
    if (s_hist_count < HIST_N) s_hist_count++;
    s_hist_nav = -1;
}

// ---- recent field values (smartwatch: re-typing the same city/number is the common case) ----------
// A tiny ring of distinct values typed into IDEE form slots; the form ghosts the best prefix match so
// you accept a past value with one fn+/ press instead of retyping it. Session-only (s_ses->recent).
static int  s_recent_count, s_recent_head;
static void recent_push(const char *v)
{
    if (!v || !v[0]) return;
    for (int i = 0; i < s_recent_count; i++)                       // skip if already remembered (any slot/order)
        if (!strcasecmp(s_ses->recent[(s_recent_head - 1 - i + RECENT_N * 2) % RECENT_N], v)) return;
    // v is a form slot of the same session block: a plain bounded memmove (snprintf's restrict trips
    // -Werror=restrict on two members of one object).
    size_t L = strlen(v); if (L > sizeof s_ses->recent[0] - 1) L = sizeof s_ses->recent[0] - 1;
    memmove(s_ses->recent[s_recent_head], v, L); s_ses->recent[s_recent_head][L] = 0;
    s_recent_head = (s_recent_head + 1) % RECENT_N;
    if (s_recent_count < RECENT_N) s_recent_count++;
}
static bool slot_autocomplete(const char *pfx, char *out, int cap)
{
    int pl = (int)strlen(pfx);
    if (pl < 1) return false;
    for (int n = 0; n < s_recent_count; n++) {
        const char *r = s_ses->recent[(s_recent_head - 1 - n + RECENT_N * 2) % RECENT_N];
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
static bool s_en    = false;                      // mirror of the OS language (nucleo_i18n_is_en), refreshed by load_settings
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
#define GUIDE_N 9                                 // cards in the GUIDA manual (also its "row" count)
static bool s_menu_open;                          // the tabbed menu is up (modal over the chat)
static int  s_tab;                                // active tab (TAB_*)
static int  s_mrow;                                // focused row: -1 = tab bar, 0..n-1 = content row
static int  s_list_scroll;                         // natural-scroll pixel offset for the active list tab
                                                   // (IDEE/OGGI/IA); only moves when the focus hits an edge
static bool s_edit;                               // a slider row (IA Volume/Luce) is in adjust mode

// ---- IDEE skill tree (smartwatch-style drill-down) --------------------------
// The IDEE tab is no longer a flat list: it's a two-level menu over a catalog of what ANIMA can do
// offline, grouped by skill. Level 0 = categories; ENTER drills into a category's leaves; a leaf
// either sends a ready prompt or — when it needs values (e.g. "Moltiplica" wants two numbers) —
// opens a tiny fill-in FORM so the device asks for each operand instead of guessing them. Esc/Left
// climb back up a level (form -> leaves -> categories -> tab bar), the watch-menu "back" gesture
// (inside a form Left is fn+, — a plain ',' types a decimal comma into the field).
static int  s_idee_cat = -1;                       // -1 = category list; >=0 = inside that category
static int  s_form_leaf = -1;                       // >=0 = a fill-in form is open for LEAVES[s_form_leaf]
static int  s_form_slot;                            // which slot the form is collecting (0/1); values in s_ses->slot
// Context memory (the watch "resume where you were"): diving into IDEE lands on the last category you
// used, and opening a category pre-focuses the last leaf you picked there (s_ses->focus_leaf) — so
// re-running a skill is a few presses. Session-only, reset on app enter.
static int  s_focus_cat;                            // category the tab-bar dive lands on

// ---- full-screen text editor (file creation from IDEE) ----------------------
// A simple full-screen textarea: type freely, Enter = newline, DEL = backspace, Ctrl+S saves to the
// SD path collected by the "Crea file" form (never over an existing file: name-2, name-3...), Esc
// cancels — behind the exit-style confirm when there is text. A failed save keeps the editor open with
// the text intact. Append-only edit (caret at the end) — a true mid-text cursor is overkill on this
// keyboard; this matches "una semplice textarea". Its buffers are allocated when the editor OPENS and
// freed when it closes (editor_close), so the ~1.6 KB is never held during a query's TLS handshake.
#define ED_BUF_CAP 1024
#define ED_LCAP    120                              // wrapped-line cap of draw_editor's layout
typedef struct {
    char  buf[ED_BUF_CAP];                          // the file content being typed
    char  path[80];                                 // absolute SD-relative path "/data/..." (from the form slot)
    short loff[ED_LCAP], llen[ED_LCAP];             // draw_editor's wrapped-line layout (was static .bss)
    short cx, cy;                                   // where the caret was painted (the blink toggles only it)
    bool  full, dirty;                              // full = scene paint; dirty = the text changed; else caret only
} AnimaEditor;
static AnimaEditor *s_ed = nullptr;                 // non-NULL exactly while s_ed_open
static bool s_ed_open;
static int  s_ed_len;
static int  s_ed_scroll;                            // wrapped-rows scrolled away above the viewport (0 = caret line visible)

// ---- calculator chain (continue from the last numeric answer, like a real calculator) -------
// The visible bubble stays exactly what the user typed; only the query sent to the engine is
// rewritten ("diviso 32" -> "2430 diviso 32"). Mirrors the web app's behind-the-scenes chaining.
static bool s_last_math;                           // last answer was a math intent -> a bare op continues it

// ---- Today/agenda tile + watch-face complications (read from the OS calendar) ----------------
// The text lives in s_ses (today / today_hdr / complics), filled by cal_refresh().
static int  s_today_n;                             // lines in s_ses->today (today's events + the next upcoming)
static int  s_today_count;                         // number of events TODAY (for the STATO tab)

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
static int  row_h(const Row *r)      { return font_h(r->font) + ((r->first & RF_FIRST) ? 3 : 0); }   // +gap before a new message
static int  input_h(void)            { return font_h(chat_font()) + 8; }
// Width of the first n bytes of s with the CURRENT font (textWidth needs a NUL-terminated string).
static int  meas(const char *s, int n) { char t[216]; if (n > 215) n = 215; memcpy(t, s, n); t[n] = 0; return (int)d.textWidth(t); }

// ---- in-place painters (ANTI-FLICKER.md technique 2, no back-buffer) ---------------------------------
// ANIMA draws DIRECT to the panel, so a fillRect-then-print blinks. These never clear under the text: the
// font paints its own cell background (M5GFX fills the glyph box when fg != bg), and only the pixels the
// glyphs won't cover are filled — so a box goes from its old content straight to the new one.
// Opaque text in the box [bx,bx+bw) x [by,by+bh) with the CURRENT font; returns the x after the text.
static int box_text(int bx, int by, int bw, int bh, int tx, int ty, const char *s, unsigned short fg, unsigned short bg)
{
    const int fh = (int)d.fontHeight(), w = (s && s[0]) ? (int)d.textWidth(s) : 0, bx1 = bx + bw, by1 = by + bh;
    if (tx < bx) tx = bx;
    const int gy0 = ty < by ? by : ty, gy1 = ty + fh > by1 ? by1 : ty + fh;   // glyph rows inside the box
    if (gy0 > by)  d.fillRect(bx, by, bw, gy0 - by, bg);
    if (gy1 < by1) d.fillRect(bx, gy1, bw, by1 - gy1, bg);
    if (gy1 > gy0) {
        if (tx > bx) d.fillRect(bx, gy0, tx - bx, gy1 - gy0, bg);
        if (tx + w < bx1) d.fillRect(tx + w, gy0, bx1 - tx - w, gy1 - gy0, bg);
    }
    if (w) { d.setTextColor(fg, bg); d.setCursor(tx, ty); d.print(s); }
    return tx + w;
}
// A focused-row pill (the kit's one selection look: accent fill, INK text) painted over whatever the band
// held: only the four corner squares outside the arc and the side margins [0,x) / [x+w,xr) go to BG first.
static void pill_band(int x, int y, int w, int h, int r, unsigned short col, int xr)
{
    if (x > 0) d.fillRect(0, y, x, h, BG);
    if (x + w < xr) d.fillRect(x + w, y, xr - x - w, h, BG);
    d.fillRect(x, y, r, r, BG);         d.fillRect(x + w - r, y, r, r, BG);
    d.fillRect(x, y + h - r, r, r, BG); d.fillRect(x + w - r, y + h - r, r, r, BG);
    d.fillRoundRect(x, y, w, h, r, col);
}
// A one-line Font0 field [x0,x1) at glyph row y: two coloured segments (a, then b) aligned left or right,
// everything else in the field painted BG. Header clusters, the reader's status strip.
static void seg_field(int x0, int x1, int y, bool right, const char *a, unsigned short ca, const char *b, unsigned short cb)
{
    d.setFont(&fonts::Font0); d.setTextSize(1);
    const int wa = (a && a[0]) ? (int)strlen(a) * 6 : 0, wb = (b && b[0]) ? (int)strlen(b) * 6 : 0;
    const int gap = (wa && wb) ? 12 : 0;
    int x = right ? x1 - (wa + gap + wb) : x0;
    if (x < x0) x = x0;
    if (x > x0) d.fillRect(x0, y, x - x0, 8, BG);
    if (wa) { d.setTextColor(ca, BG); d.setCursor(x, y); d.print(a); x += wa; }
    if (gap) { d.fillRect(x, y, gap, 8, BG); x += gap; }
    if (wb) { d.setTextColor(cb, BG); d.setCursor(x, y); d.print(b); x += wb; }
    if (x < x1) d.fillRect(x, y, x1 - x, 8, BG);
}
// Slim scroll rail in the column [235,240): track, thumb, track as three non-overlapping pieces, so the
// thumb moves without the column ever blanking. total/first/shown are in rows.
static void draw_vscroll(int ty0, int avail, int total, int first, int shown)
{
    if (total <= 0 || shown >= total) { d.fillRect(235, ty0, 5, avail, BG); return; }
    int th = avail * shown / total; if (th < 8) th = 8;
    if (th > avail) th = avail;
    int tyo = first * (avail - th) / (total - shown);
    if (tyo < 0) tyo = 0;
    if (tyo > avail - th) tyo = avail - th;
    const int bot = ty0 + tyo + th, rest = ty0 + avail - bot;
    if (tyo) { d.fillRect(235, ty0, 2, tyo, BG); d.drawFastVLine(237, ty0, tyo, LINE); d.fillRect(238, ty0, 2, tyo, BG); }
    d.fillRect(235, ty0 + tyo, 1, th, BG); d.fillRect(236, ty0 + tyo, 3, th, MUTED); d.fillRect(239, ty0 + tyo, 1, th, BG);
    if (rest > 0) { d.fillRect(235, bot, 2, rest, BG); d.drawFastVLine(237, bot, rest, LINE); d.fillRect(238, bot, 2, rest, BG); }
}

// ---- row cache: word-wrap the message ring by pixel width --------------------
static unsigned char s_wrap_mi;   // ring slot of the message wrap_msg is emitting (stamped on its rows)
static void emit_row(const char *p, int len, unsigned short col, unsigned short acc,
                     unsigned char role, unsigned char font, unsigned char first)
{
    if (!s_row) return;
    if (s_rown == ROW_MAX) { memmove(&s_row[0], &s_row[1], sizeof(Row) * (ROW_MAX - 1)); s_rown--; }
    Row *r = &s_row[s_rown++];
    r->p = p; r->len = (unsigned short)len; r->col = col; r->accent = acc;
    r->role = role; r->font = font; r->first = first; r->mi = s_wrap_mi;
}

// Source tags ("L1|0.4 s") are Font0; the '|' is drawn as a small centred dot (the ASCII font has no '·').
static int tag_w(const char *t) { return (int)strlen(t) * 6; }
static void draw_tag(int x, int y, const char *t, unsigned short col)
{
    d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextColor(col, BG);
    for (const char *p = t; *p; p++, x += 6) {
        if (*p == '|') { d.fillRect(x, y, 6, 8, BG); d.fillRect(x + 2, y + 3, 2, 2, col); }
        else { char c[2] = { *p, 0 }; d.setCursor(x, y); d.print(c); }
    }
}

// Greedy word-wrap one message into rows, measuring with its font. Honours '\n', hard-splits a word
// too long to ever fit, and guarantees at least one (possibly blank) row so the message stays visible.
static void wrap_msg(const Msg *m, const char *override_text)
{
    // ANIMA answers use a BOLD face in big mode (stronger on this low-DPI panel); questions stay regular.
    unsigned char font = (m->role == R_META)  ? (unsigned char)F_SMALL
                       : (m->role == R_ANIMA) ? (s_big ? (unsigned char)F_BOLD : (unsigned char)F_MED)
                       :                        chat_font();
    set_font(font);
    // User rows are right-aligned with a min-x of 22, so their usable width is 232-22=210; ANIMA rows
    // start at x=11 (210..225 region) so 214. Wrapping must match the render budget or a full line clips.
    const int availw = (m->role == R_META) ? 224 : (m->role == R_USER) ? 210 : 214;
    const char *text = override_text ? override_text : m->text;   // risposta corrente: testo pieno da s_ses->full
    s_wrap_mi = (unsigned char)(m - s_msg);
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
    // The source tag rides at the right end of the answer's last row when it fits there (no extra row),
    // else on a small row of its own. Not while the typewriter is still revealing the text.
    if (m->role == R_ANIMA && m->tag[0] && !(override_text && s_reveal >= 0)) {
        const Row *lr = &s_row[s_rown - 1];
        if (meas(lr->p, lr->len) <= 217 - tag_w(m->tag)) s_row[s_rown - 1].first |= RF_TAG;
        else emit_row(m->tag, (int)strlen(m->tag), K_DIM, 0, R_META, F_SMALL, RF_TAGROW);
    }
}

// Wrap the ring into rows from row index `k` on. from_cur=false: the whole ring (k = 0). from_cur=true:
// only the current answer (s_full_idx) and the messages queued after it — the typewriter's per-frame
// re-wrap, where every older message's rows are unchanged and are kept as they are.
static void wrap_ring(int k, bool from_cur)
{
    s_rown = k;
    if (!s_msg || !s_row) { s_rown = 0; return; }
    bool on = !from_cur;
    for (int i = 0; i < s_mcount; i++) { int idx = (s_mhead - s_mcount + i + MSG_MAX) % MSG_MAX;
        if (idx == s_full_idx) {                                  // slot corrente: wrappa dal testo pieno...
            on = true;
            int len = (int)strlen(s_ses->full);
            if (s_reveal >= 0 && s_reveal < len) {                // ...troncato a s_reveal byte durante il typewriter
                char saved = s_ses->full[s_reveal]; s_ses->full[s_reveal] = 0;
                wrap_msg(&s_msg[idx], s_ses->full);
                s_ses->full[s_reveal] = saved;
            } else wrap_msg(&s_msg[idx], s_ses->full);
        } else if (on) wrap_msg(&s_msg[idx], NULL); }
    d.setFont(&fonts::Font0); d.setTextSize(1);   // leave the global font at the framework default
    s_d_body = true;                              // the view mode (FOLLOW / ANCHOR / MANUAL) decides where it lands
}
static void rebuild_rows(void) { wrap_ring(0, false); }

// Transcript colours are stored as ROLES (small codes), resolved at paint time — a restored chat follows the
// active theme. Old chat files hold real RGB565 values (all far above the codes): those paint unchanged.
static unsigned short col_role(unsigned short c)
{
    return c == FG ? K_FG : c == USR ? K_USR : c == ACC ? K_ACC : c == AMBER ? K_AMBER : c == GRN ? K_GRN
         : c == MUTED ? K_MUTED : c == DIM ? K_DIM : c;
}
static unsigned short pal(unsigned short c)
{
    switch (c) {
        case K_FG: return FG;   case K_MUTED: return MUTED; case K_DIM: return DIM;     case K_ACC: return ACC;
        case K_GRN: return GRN; case K_USR: return USR;     case K_AMBER: return AMBER; default: return c;
    }
}
static void push_msg(unsigned char role, unsigned short col, unsigned short accent, const char *text)
{
    if (!s_msg) return;
    if (s_mhead == s_full_idx) s_full_idx = -1;   // lo slot del messaggio "intero" viene riusato -> torna accorciato
    Msg *m = &s_msg[s_mhead];
    app_ui_ascii_fold(text, m->text, MSG_TEXT);
    m->col = col_role(col); m->accent = role == R_ANIMA ? col_role(accent) : accent; m->role = role; m->tag[0] = 0;
    s_mhead = (s_mhead + 1) % MSG_MAX; if (s_mcount < MSG_MAX) s_mcount++;
    rebuild_rows();
}
static void push_meta(const char *t, unsigned short col) { push_msg(R_META, col, 0, t); }
// A new question pins the view to the newest row again (the answer then anchors itself, present_result).
static void push_user(const char *t)                     { if (s_ses) s_ses->vmode = V_FOLLOW; push_msg(R_USER, USR, 0, t); }
static void push_anima(const char *t, unsigned short acc) { push_msg(R_ANIMA, FG, acc, t); }

// ---- transcript view: top-anchored, page-wise (the reader) ------------------------------------------
// The row at the TOP of the viewport drops its 3 px message gap (it separates messages, not the view
// edge), so a page holds one more line. Geometry: chat body = content - 18 header - input - 1; the
// full-screen reader (fullscreen: no header, input or footer) = RD_BODY rows + a 9 px status strip.
#define RD_BODY 126
static int top_h(int i)  { return font_h(s_row[i].font); }                   // row i as the view's top row
static int chat_avail(void) { return nucleo_app_content_height() - 18 - input_h() - 1; }
static int first_row_of(int slot) { for (int i = 0; i < s_rown; i++) if (s_row[i].mi == slot) return i; return -1; }
// The last row index that fits fully when row t is at the top.
static int last_fit(int t, int avail)
{
    int used = 0, last = t;
    for (int i = t; i < s_rown; i++) {
        int hh = (i == t) ? top_h(i) : row_h(&s_row[i]);
        if (used + hh > avail) break;
        used += hh; last = i;
    }
    return last;
}
// The top row that shows the newest row at the bottom (the FOLLOW position, and the scroll limit).
static int max_top(int avail)
{
    int used = 0, t = s_rown - 1;
    for (int i = s_rown - 1; i >= 0; i--) {
        if (used + top_h(i) > avail) break;
        t = i; used += row_h(&s_row[i]);
    }
    return t < 0 ? 0 : t;
}
// Resolve the view mode to a top row for this viewport (and remember it: paging starts from there).
static int view_top(int avail)
{
    AnimaSession *S = s_ses;
    int mt = max_top(avail), t = mt;
    if (S->vmode == V_ANCHOR) { int a = first_row_of(S->anchor); if (a >= 0 && a < mt) t = a; }
    else if (S->vmode == V_MANUAL && S->vtop < mt) t = S->vtop;
    if (t < 0) t = 0;
    S->vtop = t;
    return t;
}
static void view_set(int t, int avail)
{
    int mt = max_top(avail);
    if (t > mt) t = mt;
    if (t < 0) t = 0;
    s_ses->vtop = t; s_ses->vmode = (t >= mt) ? V_FOLLOW : V_MANUAL;   // reaching the end pins the newest row
    s_d_body = s_d_badge = true;
}
// One PAGE (not a row): the rows below / above, keeping the last (first) visible row as context when a
// page shows 3+ rows. ~40 presses to read 1 KB became ~5.
static void view_page(int dir, int avail)
{
    if (s_rown <= 0) return;
    int t = view_top(avail), last = last_fit(t, avail), nt;
    if (dir > 0) nt = (last > t + 1) ? last : last + 1;
    else {
        int keep = (last > t + 1) ? t : t - 1;           // the old top ends up as the new page's last row
        if (keep < 0) keep = 0;
        nt = keep;
        for (int i = keep, used = 0; i >= 0; i--) {      // climb while the rows still fit above it
            if (used + top_h(i) > avail) break;
            nt = i; used += row_h(&s_row[i]);
        }
    }
    view_set(nt, avail);
}
static void view_scroll(int dir, int avail) { if (s_rown > 0) view_set(view_top(avail) + dir, avail); }
// "p/N" pages of the CURRENT answer when it overflows the viewport. False = it fits, or you are reading
// older history above it (then only the scrollbar shows where you are). Pages step like view_page.
static bool answer_pos(int avail, int *p, int *n)
{
    if (!s_ses || s_full_idx < 0 || s_rown <= 0) return false;
    int a = first_row_of(s_full_idx);
    if (a < 0) return false;
    int e = a; while (e + 1 < s_rown && s_row[e + 1].mi == s_full_idx) e++;
    int la = last_fit(a, avail);
    if (la >= e) return false;                                        // the whole answer fits one page
    int t = view_top(avail);
    if (t < a) return false;
    int vis = la - a + 1, step = vis > 2 ? vis - 1 : vis;
    int N = 1 + (e - la + step - 1) / step;
    int P = (last_fit(t, avail) >= e) ? N : 1 + (t - a + step - 1) / step;
    *p = P < 1 ? 1 : P > N ? N : P; *n = N;
    return true;
}

// Push online down to the assistant: master switch on unless Off; online-only when Only.
static void apply_online_mode(void)
{
    nucleo_anima_set_online(s_omode != OM_OFF);
    nucleo_anima_set_online_only(s_omode == OM_ONLY);
    // COMPACT (short ~160-char cloud answers, max_tok=110) ONLY when the LLM is NOT the sole brain: hybrid
    // answers from L1/wiki (already short) and offline doesn't call the cloud. In ONLINE-ONLY the user wants
    // the model's FULL answer (stories/explanations), so drop the clamp -> max_tok=900, shown whole + read in
    // chunks. The single source of truth for the mode, so it stays correct across live mode switches.
    nucleo_anima_set_compact_reply(s_omode != OM_ONLY);
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
                cJSON *o = cJSON_GetObjectItem(root, "online"), *b = cJSON_GetObjectItem(root, "big");
                if (cJSON_IsString(o))      s_omode = !strcmp(o->valuestring, "off")  ? OM_OFF
                                                    : !strcmp(o->valuestring, "only") ? OM_ONLY : OM_ON;
                else if (cJSON_IsBool(o))   s_omode = cJSON_IsTrue(o) ? OM_ON : OM_OFF;   // legacy bool
                if (cJSON_IsBool(b)) s_big = cJSON_IsTrue(b);
                cJSON_Delete(root);
            }
        }
    }
    s_en = nucleo_i18n_is_en();   // ANIMA speaks the OS language (the file's legacy "lang" key is only written back)
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
// The "-- ripresa DD/MM HH:MM --" separator load_chat() prepends is display-only: it is never written
// back (and legacy files that already piled several up are cleaned on load), so there is at most one.
static bool is_session_sep(const Msg *m)
{
    if (m->role == R_META && m->accent == MSG_EPHEMERAL) return true;   // next-step hints: display-only too
    if (m->role != R_META || strncmp(m->text, "-- ", 3) != 0) return false;
    size_t n = strlen(m->text);
    return n >= 6 && !strcmp(m->text + n - 3, " --");
}
static void save_chat(void)
{
    if (!s_msg) return;
    mkdir(NUCLEO_SD_MOUNT "/system", 0775);
    mkdir(NUCLEO_SD_MOUNT "/system/config", 0775);
    FILE *f = fopen(CHAT_PATH, "wb");
    if (!f) return;
    uint8_t cnt = 0;
    for (int i = 0; i < s_mcount; i++)
        if (!is_session_sep(&s_msg[(s_mhead - s_mcount + i + MSG_MAX) % MSG_MAX])) cnt++;
    fwrite("ACH1", 1, 4, f); fwrite(&cnt, 1, 1, f);
    for (int i = 0; i < s_mcount; i++) {
        int idx = (s_mhead - s_mcount + i + MSG_MAX) % MSG_MAX;
        Msg *m = &s_msg[idx];
        if (is_session_sep(m)) continue;                     // display-only, never persisted
        // A tagged answer is stored as "\x1f<tag>\x1f<text>" (same ACH1 record; old files have no tag).
        const uint16_t tl = (uint16_t)strlen(m->tag), len = (uint16_t)(strlen(m->text) + (tl ? tl + 2 : 0));
        fwrite(&m->role, 1, 1, f); fwrite(&m->col, 1, 2, f); fwrite(&m->accent, 1, 2, f);
        fwrite(&len, 1, 2, f);
        if (tl) { fputc(0x1f, f); fwrite(m->tag, 1, tl, f); fputc(0x1f, f); }
        fwrite(m->text, 1, strlen(m->text), f);
    }
    fclose(f);
}
static void load_chat(void)
{
    s_mhead = s_mcount = 0;
    if (!s_msg) return;
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
        char buf[MSG_TEXT + MSG_TAG + 2];
        const uint16_t rd = len < sizeof buf - 1 ? len : (uint16_t)(sizeof buf - 1);
        if (fread(buf, 1, rd, f) != rd) break;
        if (rd < len && fseek(f, len - rd, SEEK_CUR) != 0) break;   // longer than we keep: skip it, stay in sync
        buf[rd] = 0;
        const char *tx = buf; m->tag[0] = 0;
        if (buf[0] == 0x1f) {                                // "\x1f<tag>\x1f<text>": the answer's source tag
            const char *e = strchr(buf + 1, 0x1f);
            if (e) { int tl = (int)(e - buf - 1); if (tl > MSG_TAG - 1) tl = MSG_TAG - 1; memcpy(m->tag, buf + 1, tl); m->tag[tl] = 0; tx = e + 1; }
        }
        snprintf(m->text, MSG_TEXT, "%s", tx);
        if (is_session_sep(m)) continue;                     // legacy file: drop old separators (slot reused)
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
        m->role = R_META; m->col = K_DIM; m->accent = 0; m->tag[0] = 0;
        snprintf(m->text, MSG_TEXT, "%s", sep);
        s_mcount++;   // il ring ora include il meta come messaggio piu' vecchio
    }
}
// Seed the recall ring from the restored transcript (oldest -> newest, so the newest line is hist_at(0)).
// Only the user's own lines; "[nuovo file]"-style pseudo-bubbles are not something you'd re-send.
static void hist_from_chat(void)
{
    if (!s_msg || !s_hist) return;
    for (int i = 0; i < s_mcount; i++) {
        const Msg *m = &s_msg[(s_mhead - s_mcount + i + MSG_MAX * 2) % MSG_MAX];
        if (m->role == R_USER && m->text[0] && m->text[0] != '[') hist_push(m->text);
    }
    s_hist_nav = -1;
}

// ---- turn state -------------------------------------------------------------
// A turn (query -> answer -> voice) runs INLINE in submit() on the Solo task; s_busy is true for its
// duration (thinking dots in the input row, "pensa..." badge). No worker task, no cross-task handoff.
static bool           s_busy;
static const char    *ATAG = "anima.app";

// Cross-app seed: another app (notify action, ESP-NOW link command) stashes a question, then (or later)
// nucleo_app_launch_id("anima") opens us; the Solo enter() auto-submits it. This routes the question
// THROUGH ANIMA, so it transparently uses the online tier when available. It is called in the FULL OS,
// where the session block doesn't exist — so the text goes straight into RTC no-init RAM (which is what
// carries it across the warm reboot into Solo anyway); only a 1-byte "staged in this boot" flag stays in
// .bss. The full-OS enter() arms the magic only when staged, so a question staged in a boot where ANIMA
// is never opened (or during the Solo session itself) is dropped at the next reboot, exactly as before.
#define ANIMA_PRESET_MAGIC 0xA11A0A5Bu
RTC_NOINIT_ATTR static uint32_t s_rtc_preset_magic;
RTC_NOINIT_ATTR static char     s_rtc_preset[A_INMAX];
static bool s_preset_staged;
extern "C" void nucleo_anima_app_ask(const char *q)
{
    if (!q) return;
    snprintf(s_rtc_preset, sizeof s_rtc_preset, "%s", q);
    s_preset_staged = q[0] != 0;
}

// "Enter = open <app>" (the other direction). ANIMA Solo registers no other app, so it cannot open one
// itself: it stages the NATIVE app id in the same RTC no-init slot as the preset question — the magic word
// says which of the two the slot holds, so this costs zero extra RTC bytes — and reboots into the full OS.
// There the run loop asks for it ONCE (nucleo_anima_take_launch) and opens it like a launcher tap, so an
// NX_SOLO app (Music, Video, a game) still gets its own fresh-heap boot with its Wi-Fi/BLE flags.
// Only our own reboot (ESP_RST_SW) honours it: a crash or a cold power-on (garbage RTC) never opens anything.
// A stale handoff (an OS without the consumer hook) is cleared by the next ANIMA Solo enter().
#define ANIMA_LAUNCH_MAGIC 0xA11A0A6Cu
#define LAUNCH_ID_MAX 16
extern "C" const char *nucleo_anima_take_launch(void)
{
    if (s_rtc_preset_magic != ANIMA_LAUNCH_MAGIC) return nullptr;
    s_rtc_preset_magic = 0;                                     // consume once, even when refused below
    if (esp_reset_reason() != ESP_RST_SW) return nullptr;
    s_rtc_preset[LAUNCH_ID_MAX - 1] = 0;
    return s_rtc_preset[0] ? s_rtc_preset : nullptr;
}

// Voce on-device: pronuncia la risposta, MA non la conoscenza (tier remoto/L1/MOSAICO) ne' la
// calcolatrice (intent "calc") -> per quelle suona "leggila sullo schermo". Le risposte non
// interamente coperte da clip diventano comunque "leggila" dentro nucleo_tts_say().
static void fill_system_value(const char *arg, char *out, size_t n, bool en);   // definita piu' sotto

// Cap di lettura vocale: oltre questa lunghezza la voce NON recita (sarebbe un monologo) ma dice UNA
// sola volta "leggila sullo schermo". Sotto il cap legge frase per frase. Tiene la voce da chat reale.
#define VOICE_CAP 340

// ---- keys during a turn -------------------------------------------------------
// The inline turn owns the UI task, so the framework can't deliver keys until submit() returns; the
// reveal and the voice poll the keyboard themselves. Enter/Esc = STOP (skip the rest of the reveal and
// the voice), as always. A printable key (no fn/ctrl held — those are the arrow layer) used to be read
// and thrown away; now it ALSO ends the turn early and is kept (s_ses->carry) as the first character of
// the next question, which submit() types once the turn is over. Other keys are ignored, as before.
enum { TK_NONE = 0, TK_STOP, TK_TYPED };
static int turn_key_class(const nucleo_key_t &k)
{
    if (k.key == NK_ENTER || k.key == NK_BACK) return TK_STOP;
    if (k.ch >= 32 && k.ch < 127 && !key_mod()) { if (!s_ses->carry) s_ses->carry = k.ch; return TK_TYPED; }
    return TK_NONE;
}
static int turn_key(void)
{
    nucleo_key_t k = nucleo_kbd_read();
    return k.key == NK_NONE ? TK_NONE : turn_key_class(k);
}
// Keys that piled up while the query blocked the loop: Enter/Esc anywhere = STOP; the first printable
// one is kept (see above) and ends the drain, so anything typed after it stays queued for the normal
// loop (e.g. the Enter that sends the new line) instead of being eaten here.
static int drain_turn_keys(void)
{
    int res = TK_NONE;
    for (int i = 0; i < 32; i++) {
        nucleo_key_t k = nucleo_kbd_read();
        if (k.key == NK_NONE) break;
        int t = turn_key_class(k);
        if (t == TK_STOP) res = TK_STOP;
        else if (t == TK_TYPED) { if (res == TK_NONE) res = TK_TYPED; break; }
    }
    return res;
}
// Wait for the clip being played while polling the keyboard (every 40 ms): any stop/typing key hushes it
// at once. max_ms 0 = until it ends by itself. Returns the key class that interrupted it (TK_NONE = none).
static int voice_wait(uint32_t max_ms)
{
    for (uint32_t t = 0; nucleo_audio_playing() && (!max_ms || t < max_ms); t += 40) {
        int k = turn_key();
        if (k != TK_NONE) { nucleo_audio_stop(); return k; }
        if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();   // no-op on the inline turn (unsubscribed)
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    return TK_NONE;
}

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
        if (!nucleo_tts_enabled()) return;   // voice off -> skip the whole loop (no useless WAV-plan attempts)
        const char *full = nucleo_anima_long_reply();
        const char *p = (full && full[0]) ? full : r.reply;
        // CAP 340: una risposta lunga non si recita (monologo) — la voce dice UNA sola volta "leggila".
        if ((int)strlen(p) > VOICE_CAP) { nucleo_tts_read_hint(lang); return; }
        // Sotto il cap: leggi frase per frase ("un po' alla volta"), aspettando l'audio prima della
        // successiva. Se una frase NON e' coperta dal pool clip, di' "leggila" UNA volta e fermati: MAI
        // due "leggi" nella stessa risposta. Invio/Esc fermano la voce; un tasto stampabile pure (e resta
        // come primo carattere della prossima domanda: turn_key).
        bool hinted = false;
        for (int g = 0; *p && g < 24; g++) {
            if (turn_key() != TK_NONE) { nucleo_audio_stop(); return; }                          // Invio / digitazione = stop
            char sent[200]; int n = 0;
            while (p[n] && n < (int)sizeof(sent) - 1) { char c = p[n]; sent[n++] = c; if (c == '.' || c == '!' || c == '?') break; }
            sent[n] = 0; p += n;
            while (*p == ' ' || *p == '\n' || *p == '\t') p++;
            bool letter = false;   // skip a punctuation-only shard (e.g. ".)") that would just trigger "leggila"
            for (int i = 0; i < n; i++) { char c = sent[i]; if ((c|32) >= 'a' && (c|32) <= 'z') { letter = true; break; } if (c >= '0' && c <= '9') { letter = true; break; } }
            if (!letter) continue;
            // say_quiet parla la frase se coperta dal pool, altrimenti resta MUTO e ritorna false (niente
            // "leggila" interno). Il read_hint parte UNA volta sola sotto -> mai due.
            if (nucleo_tts_say_quiet(sent, lang)) {
                if (voice_wait(0) != TK_NONE) return;        // un tasto ferma la voce DURANTE il play (non solo tra le frasi)
            }
            else { if (!hinted) { nucleo_tts_read_hint(lang); hinted = true; } break; }          // scoperta -> una "leggila", poi stop
        }
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
            // MAI due "leggila": say_quiet resta muto su parola scoperta -> un solo read_hint se nessuna parte parla.
            bool spoke = false;
            if (p1[0] && nucleo_tts_say_quiet(p1, p1lang)) {
                spoke = true;
                if (voice_wait(2500) != TK_NONE) return;          // interrotta: niente seconda parte
            }
            if (nucleo_tts_say_quiet(tgt, tl)) spoke = true;
            if (!spoke) nucleo_tts_read_hint(lang);
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

// ---- OS calendar (calendar.json): ONE loader, ONE cap, ONE parse per change -----------------------
// Four consumers read the same file: the agenda readout ("che impegni ho oggi"), the OGGI tab, the deck's
// next-reminder glance and the add_event writer. They used to parse it separately — every TAB press
// re-read and fully re-parsed it — with caps of 32/64/200 KB: a 200 KB file meant a 200 KB malloc attempt
// on a ~50 KB heap. Now cal_load() is the single reader with one cap (a bigger file is refused and logged:
// the readers show "no events", the writer fails closed), and cal_refresh() parses at most once per change
// of the file's size/mtime (or of the day / UI language), caching everything the readers need in s_ses.
#define CAL_PATH      NUCLEO_SD_MOUNT "/system/config/calendar.json"
#define CAL_MAX_BYTES 32768     // the file AND its cJSON tree must fit the heap together (mirrors nucleo_httpd.c)
// Short weekday/month names for the glance card and the Today header (ASCII, both languages).
static const char *WD3_IT[] = { "dom", "lun", "mar", "mer", "gio", "ven", "sab" };
static const char *MO3_IT[] = { "gen", "feb", "mar", "apr", "mag", "giu", "lug", "ago", "set", "ott", "nov", "dic" };
static const char *WD3_EN[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *MO3_EN[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

// Parse calendar.json (NULL: missing, empty, over the cap, OOM or corrupt). *had_data = the file exists
// with content — the writer uses it to fail closed instead of overwriting a calendar it could not read.
static cJSON *cal_load(bool *had_data)
{
    if (had_data) *had_data = false;
    FILE *f = fopen(CAL_PATH, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (had_data) *had_data = sz > 0;
    cJSON *root = nullptr;
    if (sz >= CAL_MAX_BYTES) {
        ESP_LOGW(ATAG, "calendar.json is %ld B (cap %d): not loaded", sz, CAL_MAX_BYTES);
    } else if (sz > 0) {
        char *b = (char *)malloc((size_t)sz + 1);
        if (b) { size_t rd = fread(b, 1, (size_t)sz, f); b[rd] = 0; root = cJSON_Parse(b); free(b); }   // text freed before the tree is used
    }
    fclose(f);
    return root;
}
// Smallest date key of `evs` that has events and is >= `from` (strict: > `from`). "" if none.
static void cal_next_key(cJSON *evs, const char *from, bool strict, char *out, size_t n)
{
    out[0] = 0; cJSON *it;
    cJSON_ArrayForEach(it, evs) {
        const char *k = it->string;
        if (!k) continue;
        int c = strcmp(k, from);
        if (strict ? c <= 0 : c < 0) continue;
        if (!cJSON_IsArray(it) || cJSON_GetArraySize(it) == 0) continue;
        if (!out[0] || strcmp(k, out) < 0) snprintf(out, n, "%s", k);
    }
}
// Fill the calendar cache: the OGGI lines + today count + header, the agenda readout parts, and the raw
// next-event glance. No SD read at all while the stamp (size, mtime, day, language) is unchanged, so a
// menu open costs one stat(); force = parse regardless.
static void cal_refresh(bool force)
{
    AnimaSession *S = s_ses;
    time_t now = time(NULL); struct tm t; localtime_r(&now, &t);
    char key[16]; snprintf(key, sizeof key, "%04d-%02d-%02d", (t.tm_year + 1900) % 10000, (t.tm_mon + 1) % 100, t.tm_mday % 100);
    struct stat st; const bool exists = (stat(CAL_PATH, &st) == 0);
    const long sz = exists ? (long)st.st_size : -1; const time_t mt = exists ? st.st_mtime : 0;
    if (!force && S->cal_ok && S->cal_size == sz && S->cal_mtime == mt && S->cal_en == s_en && !strcmp(S->cal_key, key)) return;
    S->cal_ok = true; S->cal_size = sz; S->cal_mtime = mt; S->cal_en = s_en;
    snprintf(S->cal_key, sizeof S->cal_key, "%s", key);
    snprintf(S->today_hdr, sizeof S->today_hdr, s_en ? "Today, %s %d %s" : "Oggi, %s %d %s",
             (s_en ? WD3_EN : WD3_IT)[t.tm_wday], t.tm_mday, (s_en ? MO3_EN : MO3_IT)[t.tm_mon]);
    s_today_n = s_today_count = 0; S->agenda_n = 0; S->agenda[0] = 0; S->cal_next[0] = 0;
    cJSON *root = exists ? cal_load(nullptr) : nullptr;
    cJSON *evs = root ? cJSON_GetObjectItem(root, "events") : nullptr;
    cJSON *today = evs ? cJSON_GetObjectItem(evs, key) : nullptr;
    if (today && cJSON_IsArray(today)) {
        S->agenda_n = cJSON_GetArraySize(today);
        char *l = S->agenda; const size_t lc = sizeof S->agenda;
        cJSON *ev;
        cJSON_ArrayForEach(ev, today) {
            const cJSON *tmj = cJSON_GetObjectItem(ev, "time"), *tx = cJSON_GetObjectItem(ev, "text");
            const char *ts = cJSON_IsString(tmj) ? tmj->valuestring : "", *txs = cJSON_IsString(tx) ? tx->valuestring : "";
            // agenda readout: raw (UTF-8, spoken) "HH:MM text" joined with "; " up to 200 chars
            char one[96]; snprintf(one, sizeof one, "%s%s%s", ts, ts[0] ? " " : "", txs);
            if (l[0] && strlen(l) + strlen(one) + 3 < lc) strncat(l, "; ", lc - strlen(l) - 1);
            if (strlen(l) + strlen(one) + 1 < lc) strncat(l, one, lc - strlen(l) - 1);
            // OGGI tab: folded "HH:MM  text" lines (the double space splits time from title), TODAY_MAX max
            if (s_today_n < TODAY_MAX) {
                char line[72]; if (ts[0]) snprintf(line, sizeof line, "%s  %s", ts, txs); else snprintf(line, sizeof line, "%s", txs);
                app_ui_ascii_fold(line, S->today[s_today_n++], 72);
            }
        }
        s_today_count = s_today_n;
    }
    if (evs && cJSON_IsObject(evs)) {
        char bestk[16];
        cal_next_key(evs, key, false, bestk, sizeof bestk);             // deck glance: first event today-or-later
        if (bestk[0]) {
            cJSON *ev = cJSON_GetArrayItem(cJSON_GetObjectItem(evs, bestk), 0);
            const cJSON *tmj = cJSON_GetObjectItem(ev, "time"), *tx = cJSON_GetObjectItem(ev, "text");
            const char *ts = cJSON_IsString(tmj) ? tmj->valuestring : "", *txs = cJSON_IsString(tx) ? tx->valuestring : "";
            int yy, mm, dd;
            if (!strcmp(bestk, key)) snprintf(S->cal_next, sizeof S->cal_next, "%s%s%s", ts, ts[0] ? " " : "", txs);
            else if (sscanf(bestk, "%d-%d-%d", &yy, &mm, &dd) == 3) snprintf(S->cal_next, sizeof S->cal_next, "%d/%d %s", dd, mm, txs);
            else snprintf(S->cal_next, sizeof S->cal_next, "%s", txs);
        }
        if (s_today_n < TODAY_MAX) {                                   // OGGI: peek at the soonest FUTURE day
            cal_next_key(evs, key, true, bestk, sizeof bestk);
            if (bestk[0]) {
                cJSON *ev = cJSON_GetArrayItem(cJSON_GetObjectItem(evs, bestk), 0);
                const cJSON *tx = cJSON_GetObjectItem(ev, "text");
                const char *txs = cJSON_IsString(tx) ? tx->valuestring : "";
                int yy, mm, dd; char line[72];
                if (sscanf(bestk, "%d-%d-%d", &yy, &mm, &dd) == 3) snprintf(line, sizeof line, s_en ? "next %d/%d  %s" : "poi %d/%d  %s", dd, mm, txs);
                else snprintf(line, sizeof line, "%s", txs);
                app_ui_ascii_fold(line, S->today[s_today_n++], 72);
            }
        }
    }
    if (root) cJSON_Delete(root);
}
// Deck glance line: the next reminder (the date lives in the header, storage in the STATO tab). Updated at
// enter()/clear only — a glance, not a live readout. Empty when nothing is upcoming.
static void refresh_complications(void)
{
    cal_refresh(false);                                                   // enter(): empty cache -> one parse
    char nx[60]; app_ui_ascii_fold(s_ses->cal_next, nx, sizeof nx);
    snprintf(s_ses->complics, sizeof s_ses->complics, "%s", nx);
    if ((int)strlen(s_ses->complics) > 39) s_ses->complics[39] = 0;       // one Font0 line on the 240px panel
}
// OGGI tab / STATO count: called on every menu open — a stat() when nothing changed, never a re-parse.
static void load_today(void) { cal_refresh(false); }

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
        const esp_app_desc_t *desc = esp_app_get_description();
        snprintf(out, n, "NucleoOS %s", desc ? desc->version : "?");
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
        // Capabilities advertises only what ANIMA Solo can actually fulfil: device readouts, Wi-Fi/network,
        // weather, calendar, the solvers, conversions, spreadsheet formulas, the built-in file editor,
        // knowledge — and opening a device app (offered as "Enter = open X", then a reboot into the full OS).
        snprintf(out, n, en ? "I can give you time/date/space/RAM/battery, your Wi-Fi/network status, the weather of a city, the time in a world city, manage the calendar, set timers and alarms, work out the weekday of a date, days until a holiday, days in a month or age from a birth year, solve math/physics/geometry/vectors/Ohm, conversions, spreadsheet formulas, create and edit files, open the device's apps, and answer about NucleoOS/C/electronics"
                            : "Posso darti ora/data/spazio/RAM/batteria, lo stato del Wi-Fi/rete, il meteo di una citta, l'ora in una citta del mondo, gestire il calendario, impostare timer e sveglie, calcolare il giorno della settimana di una data, i giorni a una festa o in un mese, l'età da un anno di nascita, risolvere matematica/fisica/geometria/vettori/Ohm, conversioni, formule del foglio di calcolo, creare e modificare file, aprire le app del device, e rispondere su NucleoOS/C/elettronica");
    } else if (!strcmp(arg, "agenda") && tm) {
        snprintf(out, n, en ? "you have no events today" : "oggi non hai impegni");
        cal_refresh(false);                              // cached parse: re-read only if the file / day changed
        int c = s_ses->agenda_n;
        if (c > 0)
            snprintf(out, n, en ? "today you have %d %s: %s" : "oggi hai %d %s: %s",
                     c, en ? (c == 1 ? "event" : "events") : (c == 1 ? "impegno" : "impegni"), s_ses->agenda);
    }
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
    const char *path = CAL_PATH;
    bool had_data = false;
    cJSON *root = cal_load(&had_data);             // the one calendar reader (same 32 KB cap as before)
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
        s_ses->cal_ok = false;          // the calendar changed: the next reader re-parses it (cal_refresh)
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
    if (!s_last_math || !s_ses->last_num[0]) return false;
    const char *s = in; while (*s == ' ') s++;
    char c = *s;
    bool op = (c == '+' || c == '-' || c == '*' || c == '/' || c == '^' || c == '%');
    if (!op) {
        static const char *W[] = { "diviso", "fratto", "per", "piu", "meno", "volte", "x",
                                   "plus", "minus", "times", "over", "divided", "multiplied", nullptr };
        for (int i = 0; W[i] && !op; i++) if (cont_word(s, W[i])) op = true;
    }
    if (!op) {                                                         // unary power phrases
        if (!strcasecmp(s, "al quadrato") || !strcasecmp(s, "squared")) { snprintf(out, n, "%s ^ 2", s_ses->last_num); return true; }
        if (!strcasecmp(s, "al cubo")     || !strcasecmp(s, "cubed"))   { snprintf(out, n, "%s ^ 3", s_ses->last_num); return true; }
        return false;
    }
    snprintf(out, n, "%s %s", s_ses->last_num, in);
    return true;
}

static void draw(void);   // fwd: the inline Solo path paints the answer SYNCHRONOUSLY before speaking it

// ---- "open <app>" answers ------------------------------------------------------------------------------
// Native apps whose launcher name is a proper noun (no row in launcher_app_localized_name's table).
static const struct { const char *id, *it, *en; } APP_PROPER[] = {
    { "video", "Video", "Video" },       { "pomodoro", "Pomodoro", "Pomodoro" }, { "ssh", "SSH", "SSH" },
    { "ble", "BLE", "BLE" },             { "payloads", "Payloads", "Payloads" }, { "ethernet", "Ethernet", "Ethernet" },
    { "updates", "Aggiornamenti", "Updates" }, { "pong", "Pong", "Pong" },       { "poker", "Poker", "Poker" },
    { "yahtzee", "Yahtzee", "Yahtzee" }, { "gbemu", "Game Boy", "Game Boy" },    { "anima", "ANIMA", "ANIMA" },
};
// The NATIVE launcher id for an ANIMA/registry app id ("media-player" -> "music") and its display name in
// the OS language, or NULL when the app has no native screen (web-only: paint, spreadsheet, ...).
static const char *launch_target(const char *arg, char *nm, size_t n)
{
    if (!arg || !arg[0]) return nullptr;
    const char *nat = !strcmp(arg, "settings") ? "wifi" : nucleo_app_native_id(arg);   // native Settings is "wifi"
    if (!nat || !nat[0]) return nullptr;
    const char *loc = launcher_app_localized_name(nat);
    if (loc) { snprintf(nm, n, "%s", loc); return nat; }
    for (unsigned i = 0; i < sizeof APP_PROPER / sizeof APP_PROPER[0]; i++)
        if (!strcmp(nat, APP_PROPER[i].id)) { snprintf(nm, n, "%s", s_en ? APP_PROPER[i].en : APP_PROPER[i].it); return APP_PROPER[i].id; }
    return nullptr;
}

// The answer's source + time tag: calc (a solver), L0 (command/device readout), L1 (retrieval / reasoning),
// L2 (MOSAICO stitch), web (the online tiers). The time only when it is worth reading (>= 0.25 s).
static void make_tag(char *out, size_t n, const anima_result_t &r, int ms)
{
    const char *src = r.tier == ANIMA_TIER_NONE ? "" : is_math_intent(r.intent) ? "calc"
                    : r.tier == ANIMA_TIER_COMMAND ? "L0" : r.tier == ANIMA_TIER_FACT ? "L1"
                    : r.tier == ANIMA_TIER_STITCH ? "L2" : "web";
    if (!src[0]) out[0] = 0;                                   // an abstention gets a next step instead
    else if (ms >= 250) snprintf(out, n, "%s|%d.%d s", src, ms / 1000 % 100, (ms % 1000) / 100);
    else snprintf(out, n, "%s", src);
}

// Turn the just-returned result into transcript messages (and queue a launch if asked). `ms` = the turn time.
static void present_result(int ms)
{
    char reply[1024];   // pieno fino al cap del motore (s_ses->res.reply[1024]): la risposta corrente si mostra INTERA
    // NB: sullo stack di proposito, NON static — su ADV 1 KB di .bss in piu' spinge httpd_start oltre il filo
    // del rasoio dell'heap di boot (abort loop in main.c). La pressione sullo stack main 8 KB e' un rischio
    // teorico latente (mai un overflow osservato); l'heap di boot e' il vincolo reale. Vedi boot-ram-discipline.
    bool tool_ok = true;            // esito dell'operazione TOOL -> conferma vocale "Fatto"/"Errore"
    s_ses->launch[0] = 0;           // a new answer supersedes any "Enter = open <app>" offer
    bool _tool_write = s_ses->res.action == ANIMA_ACT_TOOL &&
        (!strcmp(s_ses->res.intent, "add_event") || !strcmp(s_ses->res.intent, "create_file"));
    // PIPELINE SEQUENZIALE (mai operazioni parallele): prima di scrivere il memo su SD, FERMA del tutto
    // l'audio in corso e attendi che il task player si sia smontato. Senza, la scrittura SD del calendario
    // correva IN PARALLELO con il task audio che legge/scrive la stessa SD (assemblaggio WAV/play della
    // voce di una risposta precedente) -> contesa FatFs/I2S che inchioda il device (era il freeze del
    // "ricordami/segna appuntamento"). nucleo_audio_stop e' bounded (~4.5s max) e pet-a il WDT. Cosi' la
    // sequenza e': capisci (query inline, gia' conclusa) -> [stop audio + libera] -> scrivi
    // memo -> SOLO DOPO sintetizza la voce di conferma (in coda, sotto). Una risorsa per volta.
    if (_tool_write) {
        nucleo_audio_stop();             // nessun task audio tocca la SD mentre scriviamo il memo
        nucleo_audio_wait_idle(200);     // margine: l'uscita I2S e' libera prima dell'I/O su SD
        ESP_LOGW(ATAG, "TOOL %s START free=%u largest=%u", s_ses->res.intent,
            (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    }
    if (s_ses->res.action == ANIMA_ACT_SYSTEM) {
        char value[384]; fill_system_value(s_ses->res.arg, value, sizeof(value), s_en);
        const char *ph = strstr(s_ses->res.reply, "{value}");
        if (ph) snprintf(reply, sizeof(reply), "%.*s%s%s", (int)(ph - s_ses->res.reply), s_ses->res.reply, value, ph + 7);
        else    snprintf(reply, sizeof(reply), "%s", s_ses->res.reply);
    } else if (s_ses->res.action == ANIMA_ACT_TOOL && !strcmp(s_ses->res.intent, "create_file") && s_ses->res.arg[0]
               && s_ses->res.arg[0] == '/' && !strstr(s_ses->res.arg, "..")) {
        // Physical user present -> no pairing PIN gate (unlike the web). Never overwrite. Guard: arg
        // must be an absolute SD path with no ".." (no traversal outside the mount).
        const char *bn = strrchr(s_ses->res.arg, '/'); bn = bn ? bn + 1 : s_ses->res.arg;
        char path[128]; snprintf(path, sizeof(path), NUCLEO_SD_MOUNT "%s", s_ses->res.arg);
        char dir[128]; snprintf(dir, sizeof(dir), "%s", path);
        char *slash = strrchr(dir, '/'); if (slash && slash != dir) { *slash = 0; mkdir(dir, 0775); }
        if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();   // pet il WDT prima del write SD (come apply_event)
        FILE *ex = fopen(path, "rb");
        if (ex) { fclose(ex); snprintf(reply, sizeof(reply), s_en ? "%s already exists: I won't overwrite it." : "%s esiste gia: non lo sovrascrivo.", bn); nucleo_anima_note_file(s_ses->res.arg); nucleo_anima_observe("create_file", true); }
        else {
            FILE *cf = fopen(path, "wb");
            if (cf) { const char *body = nucleo_anima_tool_content();
                      if (body && body[0]) { fwrite(body, 1, strlen(body), cf); snprintf(reply, sizeof(reply), s_en ? "I created %s with the content." : "Ho creato %s con il contenuto.", bn); }
                      else snprintf(reply, sizeof(reply), s_en ? "I created %s." : "Ho creato %s.", bn);
                      fclose(cf); nucleo_anima_note_file(s_ses->res.arg); nucleo_anima_observe("create_file", true); }
            else    { snprintf(reply, sizeof(reply), s_en ? "I can't create %s." : "Non riesco a creare %s.", bn); nucleo_anima_observe("create_file", false); tool_ok = false; }
        }
    } else if (s_ses->res.action == ANIMA_ACT_TOOL &&
               (!strcmp(s_ses->res.intent, "set_volume") || !strcmp(s_ses->res.intent, "set_brightness"))) {
        // arg is "<pct>" (absolute) or "+N"/"-N" (relative). Physical user present -> no PIN gate.
        bool vol = !strcmp(s_ses->res.intent, "set_volume");
        int cur  = vol ? nucleo_audio_volume() : nucleo_app_brightness();
        int want = (s_ses->res.arg[0] == '+' || s_ses->res.arg[0] == '-') ? cur + atoi(s_ses->res.arg) : atoi(s_ses->res.arg);
        if (want < 0) want = 0;
        if (want > 100) want = 100;
        if (vol) nucleo_audio_set_volume(want); else nucleo_app_set_brightness(want);
        nucleo_app_persist_prefs();   // a spoken "set volume/brightness" is a deliberate pref -> survives reboot
        // Bilingue: e' pronunciata PER INTERO (say_or sotto), quindi in EN deve uscire in inglese o la
        // voce EN non la coprirebbe. mathspeak rende "%" -> "per cento"/"percent".
        snprintf(reply, sizeof(reply), s_en ? (vol ? "Volume %d%%." : "Brightness %d%%.")
                                            : (vol ? "Volume al %d%%." : "Luminosita al %d%%."), want);
        nucleo_anima_observe(s_ses->res.intent, true);
    } else if (s_ses->res.action == ANIMA_ACT_TOOL && !strcmp(s_ses->res.intent, "add_event")) {
        // Calendar reminder: the spec is on the content channel; write it straight to the OS calendar.
        bool _w = apply_event(nucleo_anima_tool_content(), reply, sizeof(reply));
        ESP_LOGW(ATAG, "add_event WRITTEN ok=%d free=%u largest=%u", _w,
            (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        if (_w) nucleo_anima_observe("add_event", true);
        else { snprintf(reply, sizeof(reply), s_en ? "I couldn't add the reminder." : "Non sono riuscito ad aggiungere il promemoria."); nucleo_anima_observe("add_event", false); tool_ok = false; }
    } else if (s_ses->res.action == ANIMA_ACT_TOOL && !strcmp(s_ses->res.intent, "open_file")) {
        // Follow-up "aprilo" on a remembered file. In ANIMA Solo the assistant runs alone (no viewer apps),
        // so we can't hand the file off — point the user to the launcher instead of faking an "Opening...".
        const char *bn = strrchr(s_ses->res.arg, '/'); bn = bn ? bn + 1 : s_ses->res.arg;
        snprintf(reply, sizeof(reply), s_en ? "Press Esc to leave ANIMA, then open %s from the launcher."
                                            : "Premi Esc per uscire da ANIMA, poi apri %s dal launcher.", bn);
    } else if (s_ses->res.action == ANIMA_ACT_TOOL && !strcmp(s_ses->res.intent, "create_file")) {
        // create_file whose arg failed the absolute-path guard above -> honest error, never a silent
        // "success" via the generic reply (which would confirm a file that was never written).
        snprintf(reply, sizeof(reply), s_en ? "Invalid file path." : "Percorso file non valido.");
        nucleo_anima_observe("create_file", false); tool_ok = false;
    } else if (s_ses->res.action == ANIMA_ACT_LAUNCH) {
        // ANIMA Solo hosts no other app, so the answer OFFERS the launch: Enter on the empty line stages the
        // native id in RTC and reboots into the full OS, which opens it (launch_now / nucleo_anima_take_launch).
        char nm[32]; const char *nat = launch_target(s_ses->res.arg, nm, sizeof nm);
        if (nat && !strcmp(nat, "anima")) snprintf(reply, sizeof(reply), "%s", s_en ? "You are already in ANIMA." : "Sei gia' in ANIMA.");
        else if (nat) {
            snprintf(s_ses->launch, sizeof s_ses->launch, "%s", nat);
            snprintf(reply, sizeof(reply), s_en ? "Enter = open %s" : "Invio = apri %s", nm);
        } else snprintf(reply, sizeof(reply), s_en ? "%s is only in the web OS: open the device's IP in a browser."
                                                   : "%s c'e' solo nel web OS: apri l'IP del device nel browser.", s_ses->res.arg);
    } else {
        // Show the FULL cloud answer: grok_chat keeps anything over the 360-char on-card clip on the heap
        // overflow channel (nucleo_anima_long_reply); the engine clips s_ses->res.reply to 360. Prefer the overflow
        // so a long LLM answer is shown WHOLE (up to reply[1024]), read aloud in chunks. NULL for offline turns
        // (set_long_reply(NULL) at each query start), so those fall back to s_ses->res.reply unchanged.
        const char *full = nucleo_anima_long_reply();
        const char *body = (full && full[0]) ? full : (s_ses->res.reply[0] ? s_ses->res.reply : (s_en ? "I don't know." : "Non lo so."));
        snprintf(reply, sizeof(reply), "%s", body);
    }
    // La risposta CORRENTE si mostra INTERA: salva il testo pieno (foldato) in s_ses->full; quel messaggio verra'
    // wrappato da li' (vedi rebuild_rows). Nel ring va solo la copia accorciata qui sotto (cronologia, RAM bassa).
    app_ui_ascii_fold(reply, s_ses->full, sizeof s_ses->full);
    // Tiny screen: keep a long answer SHORT in the HISTORY ring (the current one shows full, scroll to read).
    // Clip at a clean boundary — the longest complete sentence within the limit, else a whole word; never mid-word.
    if ((int)strlen(reply) > NATIVE_REPLY_MAX) {
        int cut = 0;
        for (int i = 0; i < NATIVE_REPLY_MAX && reply[i]; i++)
            if (reply[i] == '.' || reply[i] == '!' || reply[i] == '?') cut = i + 1;
        if (cut == 0) { cut = NATIVE_REPLY_MAX; while (cut > 0 && reply[cut] != ' ') cut--; if (!cut) cut = NATIVE_REPLY_MAX; }
        while (cut > 0 && ((unsigned char)reply[cut] & 0xC0) == 0x80) cut--;
        reply[cut] = 0;
    }
    // The answer bubble: amber rail when ANIMA is asking a follow-up (awaiting a reply), else violet.
    s_full_idx = -1;                                  // il rebuild dentro push_anima NON deve applicare s_ses->full allo slot vecchio
    push_anima(reply, s_ses->res.awaiting ? AMBER : ACC);  // copia accorciata nel ring (cronologia)
    s_full_idx = (s_mhead - 1 + MSG_MAX) % MSG_MAX;   // marca lo slot appena scritto: mostralo INTERO da s_ses->full
    s_ses->vmode = V_ANCHOR; s_ses->anchor = (signed char)s_full_idx;   // reader: the answer's FIRST row at the top
    make_tag(s_msg[s_full_idx].tag, MSG_TAG, s_ses->res, ms);            // "L1 . 0.4 s" at the answer's end
    snprintf(s_ses->last_tag, sizeof s_ses->last_tag, "%s", s_msg[s_full_idx].tag);
    rebuild_rows();                                   // ri-wrappa quel messaggio dal testo pieno
    if (s_ses->res.corrected[0]) { char c[80]; snprintf(c, sizeof(c), s_en ? "(understood: %s)" : "(ho inteso: %s)", s_ses->res.corrected); push_meta(c, DIM); }
    // Reasoning trace (Claude-Code-style steps): only for genuine multi-step agent turns (those whose
    // trace has a step separator). Single-tier answers stay clean — the badge already shows tier+conf.
    if (strstr(s_ses->res.trace, " > ")) { char tr[120]; snprintf(tr, sizeof(tr), "|_ %s", s_ses->res.trace); push_meta(tr, DIM); }
    // An honest "I don't know" comes with ONE actionable next step (display-only, never persisted): the
    // network is up but ANIMA is offline -> fn+/ retries the question online; otherwise -> the IDEE catalog.
    s_ses->retry_online = false;
    if (s_ses->res.tier == ANIMA_TIER_NONE && !s_ses->res.awaiting) {
        const char *ip = nucleo_setup_ip();
        if (s_omode == OM_OFF && ip && ip[0]) {
            s_ses->retry_online = true;
            push_msg(R_META, GRN, MSG_EPHEMERAL, s_en ? "fn / : try online" : "fn / : prova online");
        } else push_msg(R_META, GRN, MSG_EPHEMERAL, s_en ? "TAB > IDEAS: what I can do" : "TAB > IDEE: cosa so fare");
    }

    s_last_conf = (s_ses->res.action == ANIMA_ACT_NONE) ? -1 : s_ses->res.confidence;
    snprintf(s_ses->last_subject, sizeof(s_ses->last_subject), "%s", s_ses->res.subject);
    s_awaiting = s_ses->res.awaiting;                  // drives the "rispondi..." input placeholder
    // Calculator chain: remember the number this answer produced so a bare "diviso 32" continues it.
    s_last_math = is_math_intent(s_ses->res.intent) && s_ses->res.action == ANIMA_ACT_ANSWER;
    if (s_last_math) extract_last_number(reply, s_ses->last_num, sizeof s_ses->last_num); else s_ses->last_num[0] = 0;

    // Conferma VOCALE delle operazioni (TOOL): se la frase esatta non e' pronunciabile (nomi file,
    // dettagli evento -> finirebbe in "leggila"), dice una conferma breve sull'ESITO. Le reply gia'
    // coperte (es. "Volume al 70 per cento") vengono dette tali e quali. I LANCIO non parlano:
    // l'app che si apre e' gia' il feedback. (Nel path low-mem speak_result salta TOOL: niente doppio.)
    if (s_ses->res.action == ANIMA_ACT_TOOL) {
        if (_tool_write) ESP_LOGW(ATAG, "TOOL %s SPEAK start largest=%u", s_ses->res.intent,
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        nucleo_tts_say_or(reply, tool_ok ? (s_en ? "Done" : "Fatto") : (s_en ? "Error" : "Errore"), s_en ? "en" : "it");
        if (_tool_write) ESP_LOGW(ATAG, "TOOL %s SPEAK done", s_ses->res.intent);
    }

    s_d_hdr = true;
    s_d_input = true;                              // ridipingi la riga input: toglie lo stato "sta scrivendo" quando s_busy si spegne
    save_chat();                                   // persist the transcript so re-entry shows this turn
}

static void clear_chat(void);
static void ask_clear(void);               // "Clear chat" behind the confirm card (defined with clear_chat)
static void submit(void);
static void chat_type(char ch);            // types one char into the chat line (defined with the key handlers)
static void refresh_complications(void);   // watch-face glance strip; used by clear_chat() above its definition
static const char *chat_hint(void);        // footer hint (usato da cancel_query/submit prima della sua def)

// Stop command (/stop, Enter/DEL while busy): hush any voice and free the UI. The inline turn itself is
// stopped from inside submit() (turn_key) — the loop can't deliver keys while it runs.
static void cancel_query(void)
{
    if (!s_busy) { push_meta(s_en ? "Nothing to stop." : "Niente da fermare.", DIM); return; }
    nucleo_audio_stop();                            // hush any sentence being read aloud right now
    s_busy = false; s_spin = 0;
    s_d_hdr = true; s_d_input = true;   // ridipingi la riga input: toglie i puntini "pensa" (anche path NK_DEL)
    nucleo_app_set_hint(chat_hint());   // ripristina il footer normale
    push_meta(s_en ? "(stopped)" : "(annullato)", DIM);
}

// Enter on the empty line after an "Enter = open <app>" answer: stage the app in RTC and leave. leave() saves
// the chat, close_app() reboots into the full OS, whose run loop opens the app (nucleo_anima_take_launch).
static void launch_now(void)
{
    char nm[32] = "", t[48];
    if (!launch_target(s_ses->launch, nm, sizeof nm)) snprintf(nm, sizeof nm, "%s", s_ses->launch);
    snprintf(s_rtc_preset, sizeof s_rtc_preset, "%s", s_ses->launch);
    s_rtc_preset_magic = ANIMA_LAUNCH_MAGIC;
    s_ses->launch[0] = 0;
    snprintf(t, sizeof t, s_en ? "Opening %s..." : "Apro %s...", nm);
    s_ses->vmode = V_FOLLOW; push_meta(t, GRN);                // kept in the chat: the history says what happened
    nucleo_app_set_hint(t); launcher_render_hint_bar();        // the reboot pause reads as intentional
    draw();
    nucleo_app_exit();                                         // Solo: saves + esp_restart(), never returns
}

// Effetto "scrittura" stile GPT/Claude per le risposte ONLINE: rivela la risposta corrente in al massimo
// TW_FRAMES frame (~0.35 s + render, qualunque sia la lunghezza) invece dei ~40-80 frame da 50 ms di prima,
// che aggiungevano 1.5-2.5 s a una risposta media. Ogni frame ri-wrappa SOLO la risposta corrente (+ le
// meta che la seguono): le righe dei messaggi precedenti non cambiano e restano come sono (wrap_ring).
// Le risposte offline (istantanee) non passano di qui: compaiono subito. Path inline (Solo): il loop UI e'
// bloccato dalla submit, qui animiamo noi. Ritorna TK_STOP (Invio/Esc: salta la voce), TK_TYPED (tasto
// stampabile: tenuto per la prossima domanda, salta la voce) o TK_NONE.
#define TW_FRAMES   10
#define TW_FRAME_MS 35
static int typewriter_reveal(void)
{
    if (s_full_idx < 0 || !s_ses->full[0]) { s_reveal = -1; return TK_NONE; }
    const int total = (int)strlen(s_ses->full);
    // First row of the answer in the row cache: present_result just wrapped it whole, and a revealed
    // prefix never needs more rows than the whole text, so the rows before it stay valid for every frame.
    int k = -1;
    for (int i = 0; i < s_rown; i++)
        if (s_row[i].p >= s_ses->full && s_row[i].p <= s_ses->full + total) { k = i; break; }
    const int step = (total + TW_FRAMES - 1) / TW_FRAMES;   // bytes per frame -> <= TW_FRAMES frames
    int res = TK_NONE, n = 0;
    while (n < total) {
        n += step; if (n > total) n = total;
        while (n < total && s_ses->full[n] != ' ') n++;        // finish the word: never cut one in half
        s_reveal = n;
        s_spin = (s_spin + 1) & 3;                             // anima i pallini "pensa" MENTRE scrive
        if (k >= 0) wrap_ring(k, true); else rebuild_rows();   // ri-wrappa solo il messaggio troncato
        s_d_input = true;                                      // ridipingi anche i pallini
        draw();
        if (n >= total) break;                                 // ultimo frame: gia' tutto a schermo
        res = turn_key();
        if (res != TK_NONE) break;                             // Invio/Esc = STOP; un tasto stampabile pure
        vTaskDelay(pdMS_TO_TICKS(TW_FRAME_MS));
    }
    s_reveal = -1;
    // Always re-wrap the whole text once more: an interrupted reveal shows the rest at once, and a complete
    // one gains the answer's source tag (skipped while revealing). The caller paints it right away.
    if (k >= 0) wrap_ring(k, true); else rebuild_rows();
    return res;
}

// The online mode's name in the OS language (chat lines, the /info readout, the mode toast, STATO).
static const char *mode_name(int m)
{
    return m == OM_OFF ? "Offline" : m == OM_ONLY ? TR("Solo online", "Online only") : TR("Ibrido", "Hybrid");
}
static void push_mode(void)
{
    char t[40]; snprintf(t, sizeof t, s_en ? "Mode: %s" : "Modalita: %s", mode_name(s_omode)); push_meta(t, GRN);
}
// Quick mode switch: Offline -> Ibrido -> Solo online -> ...
static void cycle_mode(void)
{
    s_omode = (s_omode + 1) % 3;
    apply_online_mode(); save_settings();
    push_mode();
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
    push_meta(s_en ? "/stop  /mode  /offline  /hybrid  /online" : "/stop  /modo  /offline  /ibrido  /online", DIM);
    push_meta(s_en ? "/l1  /clear  /info  /help" : "/l1  /cancella  /info  /aiuto", DIM);
    push_meta(s_en ? "/l1: offline brain AUTO/ON/OFF (RAM)" : "/l1: AI offline AUTO/ON/OFF (RAM)", DIM);
    push_meta(s_en ? "fn ;/. page the chat, ctrl ;/. history." : "fn ;/. pagina la chat, ctrl ;/. cronologia.", DIM);
    push_meta(s_en ? "Enter on an empty line: full-screen reader." : "Invio a riga vuota: lettore a schermo pieno.", DIM);
    push_meta(s_en ? "fn /: complete, or switch mode. TAB: menu." : "fn /: completa, o cambia modalita. TAB: menu.", DIM);
    push_meta(s_en ? "Hold G0: push-to-talk. Esc: leave." : "Tieni premuto G0: parla (push-to-talk). Esc: esci.", DIM);
}

static void push_info(void)
{
    char b[64];
    snprintf(b, sizeof b, s_en ? "Free RAM: %u KB" : "RAM libera: %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024)); push_meta(b, MUTED);
    const char *ssid = nucleo_setup_ssid(), *ip = nucleo_setup_ip();
    if (ip && ip[0]) snprintf(b, sizeof b, s_en ? "Network: %s  %s" : "Rete: %s  %s", (ssid && ssid[0]) ? ssid : "-", ip);
    else             snprintf(b, sizeof b, "%s", s_en ? "Network: not connected" : "Rete: non connesso");
    push_meta(b, MUTED);
    // ANIMA always runs in Solo: every query runs inline on the Solo task (there is no worker).
    snprintf(b, sizeof b, s_en ? "Mode: %s   Worker: inline (Solo)" : "Modalita: %s   Worker: inline (Solo)", mode_name(s_omode)); push_meta(b, MUTED);
    int l1m = nucleo_anima_l1_get_mode();
    snprintf(b, sizeof b, s_en ? "Offline AI (L1): %s  (%s)" : "AI offline (L1): %s  (%s)",
             l1m == 1 ? "ON" : l1m == 2 ? "OFF" : "AUTO",
             nucleo_anima_l1_serving() ? (s_en ? "active" : "attiva") : (s_en ? "stood down" : "a riposo"));
    push_meta(b, MUTED);
    if (nucleo_anima_solo_active())   // dedicated boot personality: make the (reboot-based) exit discoverable
        push_meta(s_en ? "Solo mode - Esc returns to the OS" : "Modalita Solo - Esc torna all'OS", ACC);
}

// Handle a "/command" typed in the chat. Returns true if it was a recognised command.
static bool handle_slash(const char *in)
{
    push_user(in);
    if      (!strcmp(in, "/stop"))                              cancel_query();
    else if (!strcmp(in, "/cancella") || !strcmp(in, "/clear")) ask_clear();
    else if (!strcmp(in, "/offline")) { s_omode = OM_OFF;  apply_online_mode(); save_settings(); push_mode(); }
    else if (!strcmp(in, "/ibrido") || !strcmp(in, "/hybrid")) { s_omode = OM_ON; apply_online_mode(); save_settings(); push_mode(); }
    else if (!strcmp(in, "/online"))  { s_omode = OM_ONLY; apply_online_mode(); save_settings(); push_mode(); }
    else if (!strcmp(in, "/modo") || !strcmp(in, "/mode")) cycle_mode();
    else if (!strcmp(in, "/l1"))      cycle_l1();
    else if (!strcmp(in, "/info") || !strcmp(in, "/stato")) push_info();
    else if (!strcmp(in, "/aiuto") || !strcmp(in, "/help")) push_help();
    else push_meta(s_en ? "Unknown command. /help for the list." : "Comando sconosciuto. /aiuto per la lista.", DIM);
    return true;
}

static void submit(void)
{
    if (s_ilen == 0) return;
    s_ses->input[s_ilen] = 0;
    hist_push(s_ses->input);                           // remember the typed line for ctrl+; recall + autocomplete
    s_user_sent = true;                           // first question -> the suggestion deck steps aside
    s_awaiting = false;                           // the user is answering; a new follow-up may re-arm it
    s_ses->launch[0] = 0;                         // a new line drops any pending "Enter = open <app>"
    s_ses->retry_online = false;
    if (s_ses->input[0] == '/') {                       // slash-commands run anytime (even while busy, e.g. /stop)
        handle_slash(s_ses->input);
        s_ilen = 0; s_ses->input[0] = 0; s_d_input = true; nucleo_app_request_draw(); return;
    }
    if (s_busy) return;                            // one query at a time
    push_user(s_ses->input);                            // the visible bubble is exactly what was typed
    char sendq[A_INMAX];                           // ...but the engine may get the calculator chain
    if (chain_math(s_ses->input, sendq, sizeof sendq)) snprintf(s_ses->req, sizeof(s_ses->req), "%s", sendq);
    else                                          snprintf(s_ses->req, sizeof(s_ses->req), "%s", s_ses->input);
    s_ilen = 0; s_ses->input[0] = 0;
    // ANIMA Solo runs the query INLINE on its dedicated big-stack task — NO separate 30 KB worker. Two
    // 30 KB stacks (UI + worker) plus the ~35 KB TLS handshake do NOT fit this PSRAM-less chip, which is
    // why "solo online" never reached Groq. One task owning everything (like USB-MSC) hands the whole heap
    // to the handshake -> online fits. The UI is frozen on the question for the turn (the assistant is the
    // only thing running anyway), then paints the answer. (ANIMA only ever runs in Solo: enter() reboots
    // into it, so this is the only query path.)
    s_busy = true;
    nucleo_app_set_hint(s_en ? "Enter to stop" : "Invio per fermare");   // footer: come fermare durante l'esecuzione
    launcher_render_hint_bar();                                          // ...dipinto SUBITO (il loop framework e' bloccato per tutto il turno inline)
    // CHAT-FEEL: la query inline BLOCCA questo task UI -> senza, lo schermo resta congelato sullo stato
    // pre-invio fino alla risposta. Dipingi SUBITO la bolla utente + i puntini "pensa".
    s_spin = 0; s_d_body = s_d_input = s_d_hdr = true; draw();
    // The FIRST query after the Solo reboot races the Wi-Fi reconnect (~5-8 s): if online is on but the
    // IP isn't up yet, online_available() is false -> the cascade stands UP L1 and answers offline even
    // though there IS connectivity ("la prima domanda risponde offline"). Wait briefly for the IP so the
    // first knowledge turn reaches the cloud. No-op once connected (later turns don't wait); WDT-fed,
    // bounded ~5 s.
    if (nucleo_anima_online_enabled()) {
        for (int i = 0; i < 50 && !nucleo_anima_online_available(); i++) {
            if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();
            if ((i & 3) == 0) { s_spin = (s_spin + 1) & 3; s_d_input = true; draw(); }   // anima "sta scrivendo..."
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    // The query below blocks this task (no key, no animation) — up to the engine's 12 s network budget when
    // a cloud tier may answer. Say so in the input row BEFORE freezing, so the still screen is explained.
    s_ses->cloud_note = nucleo_anima_online_available();
    if (s_ses->cloud_note) { s_d_input = true; draw(); }
    ESP_LOGW(ATAG, "inline query START online=%d free=%u largest=%u",   // Solo heap margin + connectivity at turn start (WARN so it shows in /api/logs + serial)
             (int)nucleo_anima_online_available(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    esp_task_wdt_delete(NULL);                 // an online turn can run ~30s; don't let the 8s task WDT reboot mid-handshake. KEPT deleted THROUGH the voice below: a knowledge answer's TTS render reads MANY clips from SD (fseek per clip) and the ADV's slow SD makes the cumulative SD time exceed the 8s WDT -> anima-solo TWDT reboot (real backtrace: speak_result->render->tts_index_find->fseek). Re-added only after speak_result.
    // Built IN PLACE: the ~1.4 KB result is constructed straight into the session block (guaranteed copy
    // elision: the callee writes through the hidden return pointer), never as a temporary in this frame and
    // then copied — that temporary sat on the 26 KB Solo stack for the whole query (TLS included).
    const int64_t t0 = esp_timer_get_time();
    ::new (static_cast<void *>(&s_ses->res)) anima_result_t(nucleo_anima_query(s_ses->req, s_en ? "en" : "it"));
    const int turn_ms = (int)((esp_timer_get_time() - t0) / 1000);
    s_ses->cloud_note = false;
    { const char *lr = nucleo_anima_long_reply();
      ESP_LOGW(ATAG, "inline query DONE tier=%d action=%d stack_hw=%u free=%u reply_len=%u long_len=%u",   // tier 4=REMOTE; reply_len=clip, long_len=full overflow
             (int)s_ses->res.tier, (int)s_ses->res.action, (unsigned)(uxTaskGetStackHighWaterMark(NULL)*sizeof(StackType_t)),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT), (unsigned)strlen(s_ses->res.reply), (unsigned)(lr ? strlen(lr) : 0)); }
    s_ses->carry = 0;
    present_result(turn_ms);
    // TEXT BEFORE VOICE: the inline query BLOCKS this UI task, so the launcher loop can't paint until we
    // return — meaning speak_result() (which blocks on TTS) would otherwise be HEARD before the answer is
    // SEEN. Paint it synchronously now (ANIMA is direct-draw), THEN speak. Only an ONLINE answer gets the
    // (capped) typewriter; an offline one is instant, so it appears at once — the reveal was pure latency.
    int stop = (s_ses->res.tier == ANIMA_TIER_REMOTE) ? typewriter_reveal() : TK_NONE;
    s_busy = false;        // scrittura finita -> pallini via, prompt normale, badge -> orologio
    s_d_input = s_d_hdr = true; draw();
    // Keys pressed during the query/reveal: Invio/Esc = STOP (salta la voce, il testo resta a schermo); un
    // tasto stampabile salta la voce E diventa il primo carattere della prossima domanda (s_ses->carry).
    if (stop == TK_NONE) stop = drain_turn_keys();
    if (stop == TK_STOP) push_meta(s_en ? "(stopped)" : "(annullato)", DIM);
    else if (stop == TK_NONE) speak_result(s_ses->res, s_en);   // voce on-device, interrompibile (turn_key)
    esp_task_wdt_add(NULL);                   // turn finito (query + render voce SD-lenta): ri-sottoscrivi il task WDT
    if (s_ses->carry) { char c = s_ses->carry; s_ses->carry = 0; chat_type(c); }   // start the next question
    nucleo_app_set_hint(chat_hint()); launcher_render_hint_bar();   // ripristina il footer normale
    s_d_input = true; s_d_hdr = true;
    nucleo_app_request_draw();
}

static void clear_chat(void)
{
    nucleo_anima_reset_session();
    s_mhead = s_mcount = 0; s_rown = 0; s_ilen = 0; s_ses->input[0] = 0;
    s_full_idx = -1; s_ses->vmode = V_FOLLOW; s_ses->vtop = 0; s_ses->launch[0] = 0; s_ses->retry_online = false;
    s_last_conf = -1; s_ses->last_tag[0] = 0; s_ses->last_subject[0] = 0;
    s_last_math = false; s_ses->last_num[0] = 0;
    s_user_sent = false; s_sug_sel = 0; s_awaiting = false;   // back to the suggestion deck
    remove(CHAT_PATH);                                        // forget the persisted conversation too
    refresh_complications();
    mark_all_dirty();
}

// "Clear chat" (IA row, /cancella, /clear) is destructive and final — it also deletes the saved chat file —
// so it asks with the kit's standard confirm card (app_ui_confirm), focus on No: a stray Enter is safe.
static void ask_clear(void)
{
    s_ses->clear_confirm = true; s_ses->clear_yes = false;
    nucleo_app_set_hint(s_en ? "</> pick   enter ok   esc back" : "</> scegli   invio ok   esc annulla");
    nucleo_app_request_draw();
}
static void menu_hint(void);               // footer hint for the menu (defined with the menu)
static void clear_confirm_done(bool yes)
{
    s_ses->clear_confirm = false;
    if (yes) { clear_chat(); s_menu_open = false; }       // cleared: back to the welcome deck
    if (s_menu_open) menu_hint(); else nucleo_app_set_hint(chat_hint());
    mark_all_dirty(); nucleo_app_request_draw();          // the card leaves: repaint the scene under it
}

// ---- suggestion deck (empty-state) ------------------------------------------
// A fresh/cleared chat shows starter prompts that exercise the breadth of ANIMA AND lean on everyday
// human life: time, weather, Wi-Fi/network, mental math, a calendar reminder, a unit conversion, a
// percentage (tip/discount), capabilities. fn+;/. pick, Invio runs. (No "open app" prompt here on purpose:
// opening an app leaves ANIMA with a reboot — it lives in IDEE > App e file, one confirm away.)
#define SUG_N 16   // deck espanso: 16 voci scrollabili (su/giu), copre piu' skill
static const char *SUG_IT[SUG_N] = {
    "Che ore sono", "Che giorno e oggi", "Meteo a Brescia",
    "Quanto spazio ho sulla SD", "A che rete sono connesso", "Quanto fa 18 x 24",
    "Quanto e il 15% di 80", "Ricordami la spesa domani alle 18", "Cosa sai fare",
    // nuovi suggerimenti
    "Radice quadrata di 144", "Converti 5 miglia in km",
    "Chi e Ada Lovelace", "Quanto fa 37 per 48",
    "In che stagione siamo", "Ricordami palestra domani alle 7",
    "Cos'e il DNA"
};
static const char *SUG_EN[SUG_N] = {
    "What time is it", "What's today's date", "Weather in London",
    "How much SD space is free", "What network am I on", "How much is 18 x 24",
    "What is 15% of 80", "Remind me groceries tomorrow at 6pm", "What can you do",
    // new suggestions
    "Square root of 144", "Convert 5 miles to km",
    "Who is Ada Lovelace", "How much is 37 times 48",
    "What season is it", "Remind me gym tomorrow at 7am",
    "What is DNA"
};
static const char *cur_sug(int i) { return (s_en ? SUG_EN : SUG_IT)[i < SUG_N ? i : 0]; }
static bool deck_active(void)     { return !s_user_sent && s_ilen == 0 && !s_menu_open; }

// Recall a history entry into the input. dir<0 = older (ctrl+;), dir>0 = newer (ctrl+.). The live
// draft is parked on first step back and restored when you walk forward past the newest entry.
static void hist_recall(int dir)
{
    if (s_hist_count == 0) return;
    if (s_hist_nav < 0) {                                  // entering history: stash the draft
        if (dir > 0) return;                               // already on the draft, nothing newer
        snprintf(s_ses->hist_draft, sizeof s_ses->hist_draft, "%s", s_ses->input);
        s_hist_nav = 0;
    } else {
        s_hist_nav += (dir < 0) ? 1 : -1;
    }
    if (s_hist_nav >= s_hist_count) s_hist_nav = s_hist_count - 1;
    const char *line = (s_hist_nav < 0) ? s_ses->hist_draft : hist_at(s_hist_nav);
    snprintf(s_ses->input, sizeof s_ses->input, "%s", line ? line : "");
    s_ilen = (int)strlen(s_ses->input);
    s_d_input = true;
}

// Inline autocomplete (fish-style ghost text): the best single completion of the typed prefix, drawn
// dimmed after the caret and accepted with fn+/ (a plain '/' just types). Sources, in priority: slash commands,
// your own history (most recent first), then the starter suggestions. Case-insensitive prefix match.
static const char *const SLASH_CMDS[] = { "/stop", "/cancella", "/clear", "/offline", "/ibrido", "/hybrid",
                                          "/online", "/modo", "/mode", "/l1", "/info", "/stato", "/aiuto", "/help" };
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
                                     "Agenda", "App e file", "Sapere", "Traduci" };
static const char *CAT_EN[CAT_N] = { "System", "Math", "Geometry", "Conversions", "Weather",
                                     "Agenda", "Apps & files", "Knowledge", "Translate" };
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
    { 0,1, "Giorno di...","Weekday of",  "Che giorno della settimana e il %s","What day of the week is %s","Data es 25 dic 2026","Date e.g. dec 25 2026",0,0 },
    { 0,1, "Giorni a...","Days until",   "Quanti giorni mancano a %s","How many days until %s",   "Data o festa","Date or holiday",0,0 },
    { 0,1, "Giorni nel mese","Days in month","Quanti giorni ha %s","How many days in %s",           "Mese es febbraio","Month e.g. february",0,0 },
    { 0,1, "Eta da anno","Age from year", "Quanti anni ha chi e nato nel %s","How old is someone born in %s","Anno nascita","Birth year",0,0 },
    { 0,1, "Ora nel mondo","World clock", "Che ore sono a %s","What time is it in %s",                 "Citta es Tokyo","City e.g. Tokyo",0,0 },
    { 0,1, "Segno zodiacale","Star sign", "Che segno e chi nasce il %s","What star sign is someone born on %s","Data es 5 agosto","Date e.g. august 5",0,0 },
    { 0,2, "Ore tra orari","Hours between","Quante ore da %s a %s","How many hours from %s to %s",   "Da HH:MM","From HH:MM","A HH:MM","To HH:MM" },
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
    { 5,1, "Timer","Timer",              "Timer di %s minuti","Timer for %s minutes",             "Minuti","Minutes",0,0 },
    { 5,1, "Sveglia","Alarm",            "Sveglia alle %s","Alarm at %s",                         "Ora HH:MM","Time HH:MM",0,0 },
    { 5,2, "Promem. domani","Tomorrow",  "Ricordami %s domani alle %s","Remind me %s tomorrow at %s","Cosa","What","Ora HH:MM","Time HH:MM" },
    { 5,2, "Promem. oggi","Today rem.",  "Ricordami %s oggi alle %s","Remind me %s today at %s",  "Cosa","What","Ora HH:MM","Time HH:MM" },
    // -- App e file (6): open a device app (the answer offers "Enter = open X": ANIMA reboots into the full
    // OS, which opens it — see launch_now), then ANIMA's OWN built-in editor (quick note + create file).
    { 6,1, "Apri app","Open app",        "Apri %s","Open %s",                                     "App es musica","App e.g. music",0,0 },
    { 6,0, "Musica","Music",             "Apri la musica","Open music",                           0,0,0,0 },
    { 6,0, "Impostazioni","Settings",    "Apri le impostazioni","Open settings",                  0,0,0,0 },
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
    const char *a = s_ses->slot[0][0] ? s_ses->slot[0] : (preview ? "?" : "");
    const char *b = s_ses->slot[1][0] ? s_ses->slot[1] : (preview ? "?" : "");
    if      (L->slots >= 2) snprintf(out, n, t, a, b);   // format is OUR constant; a/b are inert args
    else if (L->slots == 1) snprintf(out, n, t, a);
    else                    snprintf(out, n, "%s", t);
}

// ---- tabbed menu: labels + per-tab focusable-row count (Music/Video parity) -------------------
// Short ASCII labels so all five fit the 240/5 = 48px segments and never clip (worst case
// "STATUS" = 36px, centred with 6px margins). Localised IT/EN like the rest of the app.
static const char *const TABS_IT[TAB_N] = { "IDEE", "OGGI", "GUIDA", "IA", "STATO" };
static const char *const TABS_EN[TAB_N] = { "IDEAS", "TODAY", "GUIDE", "AI", "STATUS" };
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
    snprintf(s_ses->input, sizeof(s_ses->input), "%s", q);
    s_ilen = (int)strlen(s_ses->input);
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
static int  list_scroll_y0(int top, int avail, int n, int f);   // natural-scroll layout (used by draw_ia, defined later)
static void reader_open(void);                // full-screen reader (defined with the painters)
static void reader_close(void);

// Collapse the IDEE tree back to its category list (called when the menu (re)opens or pages away).
static void reset_idee(void) { s_idee_cat = -1; s_form_leaf = -1; s_form_slot = 0; s_list_scroll = 0; }

static void enter(void)
{
    // ANIMA Solo (see nucleo_app.h): from the full OS, opening the assistant reboots into a dedicated
    // personality where it owns a large, UNFRAGMENTED heap (httpd/mDNS/recorder/etc never start) — the
    // only way this PSRAM-less chip fits online TLS + L1 + voice at once. If another app seeded a
    // question (nucleo_anima_app_ask: notify / ESP-NOW), arm its RTC copy so the auto-ask fires after
    // the warm reboot. A cold power-on clears the RTC flags -> you always land in the full OS.
    if (!nucleo_anima_solo_active()) {
        if (s_preset_staged) s_rtc_preset_magic = ANIMA_PRESET_MAGIC;   // text already in s_rtc_preset
        nucleo_anima_solo_request();                              // set RTC flag + esp_restart() — NEVER returns
        return;
    }
    // From here on we are in the Solo boot, for the rest of it (the Solo flag is latched per boot).
    const bool preset = (s_rtc_preset_magic == ANIMA_PRESET_MAGIC);   // a question carried across the reboot
    s_rtc_preset_magic = 0;
    s_rtc_preset[A_INMAX - 1] = 0;

    // ANIMA draws DIRECT to the panel, so free the launcher's ~32 KB off-screen back-buffer the
    // moment we open: that RAM belongs to the assistant while it runs (L1 index + TLS).
    nucleo_screen_release();
    nucleo_app_set_direct_draw(true);
    nucleo_audio_stop();
    // In Solo httpd / L1 / mDNS were NEVER started — the heap is already wide open, so there is nothing
    // to suspend (and calling nucleo_exclusive_exit later would START services we deliberately kept off).
    // Just log the headroom we came up with for the boot-RAM trace.
    ESP_LOGI(ATAG, "solo enter: free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    nucleo_anima_set_compact_reply(true);   // small screen: cloud answers short & complete (off again in leave)
    load_settings();
    nucleo_anima_init(nucleo_i18n_lang());          // the OS language, as main.c's boot init (was a hard-coded "it")
    // Clear any "a browser LLM is serving" hint the web app may have left set: when the NATIVE app is
    // foreground the device itself is the brain, so AUTO must decide L1 purely on online-key availability.
    // (An explicit user /l1 ON/OFF override still wins — set_external_brain only affects AUTO.)
    nucleo_anima_l1_set_external_brain(false);
    // Session buffers on the heap (freed in leave), taken once, early, from the fresh Solo heap — they
    // hold ZERO .bss in any boot. s_ses first: every text buffer the UI touches lives there, so without it
    // the app only shows an out-of-memory notice (draw) and Esc leaves (on_back); nothing else runs.
    nucleo_app_set_tab_handler(on_tab);
    nucleo_app_set_back_handler(on_back);
    if (!s_ses) s_ses = (AnimaSession *)calloc(1, sizeof *s_ses);   // ~4.9 KB, was .bss in every boot
    if (!s_ses) {
        ESP_LOGE(ATAG, "session alloc (%u B) failed: free=%u largest=%u", (unsigned)sizeof(AnimaSession),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        nucleo_app_set_hint(s_en ? "esc exit" : "esc esci");
        nucleo_app_request_draw();
        return;
    }
    if (!s_hist)   s_hist   = (char (*)[HIST_LEN])calloc(HIST_N, sizeof *s_hist);
    if (!s_msg)    s_msg    = (Msg *)calloc(MSG_MAX, sizeof *s_msg);   // ~5 KB transcript ring — heap, not .bss
    if (!s_row)    s_row    = (Row *)calloc(ROW_MAX, sizeof *s_row);   // ~2 KB wrapped-row cache
    load_chat();                                   // restore the last conversation (empty ring if none)
    s_rown = 0; s_ilen = 0; s_full_idx = -1;
    s_ses->vmode = V_FOLLOW; s_ses->vtop = 0; s_ses->anchor = -1; s_ses->reader = false;   // restored chat: newest row
    s_ses->launch[0] = 0; s_ses->clear_confirm = false; s_ses->retry_online = false; s_ses->cloud_note = false;
    s_ses->body_kind = BK_NONE; s_ses->rgen = nucleo_app_repaint_gen(); s_ses->mscene = -1;
    s_ed_open = false; s_ed_len = 0; s_ed_scroll = 0;   // the editor block is allocated only when it opens
    s_busy = false;
    s_last_conf = -1; s_ses->last_tag[0] = 0; s_ses->last_subject[0] = 0;
    s_user_sent = (s_mcount > 0); s_sug_sel = 0; s_awaiting = false; s_clock_min = -1;
    s_hist_count = 0; s_hist_head = 0; s_hist_nav = -1; s_ses->hist_draft[0] = 0;
    hist_from_chat();                              // fresh Solo boot: recall your own lines from the restored chat
    s_toast_until = 0; s_exit_confirm = false;
    s_recent_count = 0; s_recent_head = 0;
    s_last_math = false; s_ses->last_num[0] = 0;
    s_today_n = s_today_count = 0; s_ses->today_hdr[0] = 0; s_ses->complics[0] = 0;
    s_ses->cal_ok = false; s_ses->carry = 0;        // enter always re-reads the calendar once (cal_refresh)
    s_menu_open = false; s_tab = TAB_IDEE; s_mrow = -1; s_edit = false; reset_idee();
    s_focus_cat = 0; memset(s_ses->focus_leaf, 0, sizeof s_ses->focus_leaf);
    refresh_complications();                       // watch-face glance card: parses calendar.json once (cache)
    if (s_mcount) rebuild_rows();                  // restored conversation -> wrap it for display
    if (preset && s_rtc_preset[0]) {               // seeded by another app (notify / ESP-NOW): auto-ask
        snprintf(s_ses->input, sizeof s_ses->input, "%s", s_rtc_preset); s_ilen = (int)strlen(s_ses->input);
        submit();                                  // runs the turn inline, like a typed question
    }
    nucleo_app_set_hint(chat_hint());              // deck or chat hint, whichever is up
    mark_all_dirty();
    nucleo_app_request_draw();
}

static void leave(void)
{
    save_chat();                                   // remember the conversation for next time
    nucleo_anima_set_compact_reply(false);         // web client (full screen) keeps long answers
    free(s_hist);   s_hist   = nullptr;            // session buffers back to zero RAM until next enter
    free(s_ed);     s_ed     = nullptr; s_ed_open = false;
    free(s_msg);    s_msg    = nullptr;            // transcript ring + row cache: ~7 KB of .bss reclaimed at boot
    free(s_row);    s_row    = nullptr;
    free(s_ses);    s_ses    = nullptr;            // callbacks bail on NULL from here on
    // Give the shared 32 KB canvas back before handing over (bounded ~700 ms; the lazy getter heals later).
    // In practice leave() only runs from close_app() in the Solo boot, right before its esp_restart().
    for (int i = 0; i < 35 && !nucleo_screen_acquire(); i++) vTaskDelay(pdMS_TO_TICKS(20));
    if (nucleo_exclusive_active()) nucleo_exclusive_exit();  // ripristina httpd/L1/mDNS/voce: canvas gia' ripristinata
    d.setFont(&fonts::Font0); d.setTextSize(1);    // restore the framework's default font for the next app
}

// The hint shown on the chat page: the deck, an empty line, or a line being written (<= 39 chars, the
// footer's cap). It teaches the arrow layer — plain ; . , / type, fn+key is the arrow (header note).
static const char *chat_hint(void)
{
    if (deck_active()) return s_en ? "fn ;/. pick  1-9 try  fn / mode" : "fn ;/. scegli  1-9 prova  fn / modo";
    if (s_ilen > 0)    return s_en ? "enter send  fn / complete  tab menu" : "invio invia  fn / completa  tab menu";
    if (s_ses && s_ses->launch[0]) return s_en ? "enter open app  fn ;. page  fn / mode" : "invio apri app  fn ;. pagina  fn / modo";
    if (s_rown > 0) return s_en ? "enter read  fn ;. page  fn / mode" : "invio leggi  fn ;. pagina  fn / modo";
    return s_en ? "esc exit  fn ;/. scroll  fn / mode" : "esc esci  fn ;/. scorri  fn / modo";
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
        if (LEAVES[s_form_leaf].slots < 2)                     // single field: nothing to hop between
            nucleo_app_set_hint(s_en ? "type  fn / fill  enter send" : "digita  fn / riempi  invio invia");
        else                                                   // ; . , / type; fn+;/. hop fields, fn+/ fills
            nucleo_app_set_hint(s_en ? (last ? "fn / fill  fn ;. field  enter send" : "fn / fill  fn ;. field  enter next")
                                     : (last ? "fn / riempi  fn ;. campo  invio invia" : "fn / riempi  fn ;. campo  invio avanti"));
        return;
    }
    if (s_mrow == -1) { nucleo_app_set_hint(s_en ? "l/r tab   down enter   esc" : "sx/dx scheda  giu entra  esc"); return; }
    switch (s_tab) {
        case TAB_IDEE:
            if (s_idee_cat < 0) nucleo_app_set_hint(s_en ? "up/dn  enter open  l/r tab" : "su giu  invio apri  sx/dx");
            else                nucleo_app_set_hint(s_en ? "up/dn  enter send  esc back" : "su giu  invio invia  esc su");
            break;
        case TAB_OGGI:  nucleo_app_set_hint(s_en ? "up/dn scroll   l/r tab"      : "su giu scorri  sx/dx scheda");  break;
        case TAB_GUIDA: nucleo_app_set_hint(s_en ? "up/dn page  1-9 jump  l/r"   : "su giu pag  1-9  sx/dx sch");   break;
        case TAB_IA:    nucleo_app_set_hint(s_en ? "up/dn  enter change  l/r tab" : "su giu  invio cambia sx/dx"); break;
        default:        nucleo_app_set_hint(s_en ? "l/r tab   esc close"          : "sx/dx scheda  esc chiudi");
    }
}

// TAB toggles the full-screen tabbed menu over the chat (Music/Video model: TAB open/close, LEFT/
// RIGHT page the tabs). Always opens on IDEE with the tab bar focused, so the first DOWN dives in.
static void on_tab(void)
{
    if (!s_ses || s_ed_open || s_exit_confirm || s_ses->clear_confirm) return;   // no session / the editor / a confirm owns the screen
    reader_close();                                        // TAB from the reader: straight to the menu
    s_menu_open = !s_menu_open;
    if (s_menu_open) { s_tab = TAB_IDEE; s_mrow = -1; s_edit = false; s_sug_sel = 0; reset_idee(); load_today(); menu_hint(); }
    else             { nucleo_app_set_hint(chat_hint()); }
    mark_all_dirty();
    nucleo_app_request_draw();
}

// A chat/deck key changed the line: repaint the input, swap the body when the welcome deck steps aside
// (first char) or comes back (last char deleted), and keep the footer hint in step. The hint is left
// alone while a turn runs (its "Enter to stop" belongs to the busy state).
static void chat_changed(bool was_deck)
{
    s_d_input = true;
    if (was_deck != deck_active()) s_d_body = true;
    if (!s_busy) nucleo_app_set_hint(chat_hint());         // idempotent: repaints the footer only on a change
    nucleo_app_request_draw();
}
// Append one character to the chat line — every printable key, INCLUDING ; . , / (they type; their
// arrow meaning needs fn). Typing snaps a scrolled-up transcript back to the newest line.
static void chat_type(char ch)
{
    bool was_deck = deck_active();
    if (s_ilen < A_INMAX - 1) { s_ses->input[s_ilen++] = ch; s_ses->input[s_ilen] = 0; }
    s_hist_nav = -1;
    chat_changed(was_deck);                          // the view stays where you are reading (the input row is always visible)
}
static void idee_form_key(int key, char ch);   // defined below (fill-in form keys)

// Esc/Back AND Left (= the ',' key) both route here with the key code, so we tell them apart. Wherever
// you write (chat, deck, fill-in form, editor) ',' is a COMMA — it never leaves nor throws a form away;
// only fn+, is the Left arrow. In the menu lists LEFT pages the tabs backward (the mirror of RIGHT in
// menu_key). ESC/Back stays hierarchical: slider adjust -> row -> tab bar -> close menu; from the chat
// base Esc opens the leave-confirm (leaving Solo = reboot, so never on a single stray key).
static bool on_back(int key)
{
    if (!s_ses) { if (key == NK_BACK) nucleo_app_exit(); return true; }   // OOM notice: Esc leaves (Solo -> reboot)
    if (s_exit_confirm) { s_exit_confirm = false; mark_all_dirty(); nucleo_app_request_draw(); return true; }  // Esc nel modale = annulla (resta)
    if (s_ses->clear_confirm) {                             // confirm card: Esc = No, ',' toggles the focus
        if (key == NK_BACK) clear_confirm_done(false);
        else { app_ui_confirm_key(NK_LEFT, 0, &s_ses->clear_yes); nucleo_app_request_draw(); }
        return true;
    }
    if (s_ed_open) {                                        // editor: the launcher routes ',' (Left) and Esc here
        if (key == NK_LEFT) {                               // ',' -> type a literal comma (textarea isn't comma-blind)
            if (s_ed_len < ED_BUF_CAP - 1) { s_ed->buf[s_ed_len++] = ','; s_ed->buf[s_ed_len] = 0; s_ed->dirty = true; nucleo_app_request_draw(); }
        } else if (s_ed_len > 0) {                          // Esc with text typed: ask before throwing it away
            s_exit_confirm = true; nucleo_app_request_draw();
        } else editor_cancel();                             // Esc on an empty editor: nothing to lose
        return true;
    }
    if (s_ses->reader) {                                    // reader: Esc closes it, ',' (Left) pages back
        if (key == NK_BACK) reader_close();
        else { view_page(-1, RD_BODY); nucleo_app_request_draw(); }
        return true;
    }
    if (!s_menu_open) {                                     // chat base + welcome deck
        if (key == NK_BACK) { s_exit_confirm = true; nucleo_app_request_draw(); return true; }  // Esc -> chiedi conferma, NON chiudere subito
        if (!key_mod()) chat_type(',');                     // ',' types a comma (was: closed ANIMA = Solo reboot)
        return true;                                        // fn+, (Left arrow): nothing to move on an append-only line
    }
    if (s_tab == TAB_IDEE && s_form_leaf >= 0 && key == NK_LEFT && !key_mod()) {   // form: ',' = decimal comma
        idee_form_key(NK_CHAR, ',');                        // (fn+, still climbs back, below)
        return true;
    }
    if (s_edit) {
        if (key == NK_LEFT) slider_adjust(-5);              // Left lowers the value
        else              { s_edit = false; nucleo_app_persist_prefs(); }   // Esc finishes adjusting -> save volume/brightness
        menu_hint(); nucleo_app_request_draw(); return true;
    }
    // IDEE drill-down: when inside a category or a fill-form, BOTH Esc and Left climb one level (the
    // watch "back" gesture) instead of paging tabs — only at the top category list does Left page.
    // (In a form Left means fn+, — a plain ',' was typed above.)
    if (s_tab == TAB_IDEE && (s_form_leaf >= 0 || s_idee_cat >= 0)) {
        if (s_form_leaf >= 0) s_form_leaf = -1;             // form -> back to its leaf list
        else { s_mrow = s_idee_cat; s_idee_cat = -1; s_list_scroll = 0; }   // leaves -> back to categories (land on it)
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
    int n = cat_leaf_count(c), row = s_ses->focus_leaf[c];          // resume the last leaf you used here
    if (row < 0 || row >= n) row = 0;
    s_idee_cat = c; s_mrow = row; s_form_leaf = -1; s_list_scroll = 0;   // fresh scroll for the new leaf list
    menu_hint(); nucleo_app_request_draw();
}
// Never overwrite: if the SD-relative path `rel` ("/dir/name.ext") exists, rewrite it in place to the
// first free "/dir/name-2.ext", "-3", ... Returns false when no free name fits (caller refuses to save).
static bool path_make_unique(char *rel, size_t cap)
{
    char full[180]; struct stat st;
    snprintf(full, sizeof full, NUCLEO_SD_MOUNT "%s", rel);
    if (stat(full, &st) != 0) return true;                  // free already
    char stem[80]; snprintf(stem, sizeof stem, "%s", rel);
    char *slash = strrchr(stem, '/'), *dot = strrchr(stem, '.');
    char ext[16] = "";
    if (slash && dot && dot > slash + 1 && strlen(dot) < sizeof ext) { snprintf(ext, sizeof ext, "%s", dot); *dot = 0; }
    for (int k = 2; k < 100; k++) {
        char cand[112];
        snprintf(cand, sizeof cand, "%s-%d%s", stem, k, ext);
        if (strlen(cand) >= cap) return false;
        snprintf(full, sizeof full, NUCLEO_SD_MOUNT "%s", cand);
        if (stat(full, &st) != 0) { snprintf(rel, cap, "%s", cand); return true; }
    }
    return false;
}
// A quick-note target: /data/note/YYYYMMDD_HHMMSS.txt (seconds, so two notes in one minute don't
// collide), or nota.txt before the clock is set — both made unique, so a note never lands on another.
static void quick_note_path(char *out, size_t n)
{
    mkdir(NUCLEO_SD_MOUNT "/data", 0775);
    mkdir(NUCLEO_SD_MOUNT "/data/note", 0775);
    time_t now = time(NULL); struct tm tmv;
    if (now > 1672531200 && localtime_r(&now, &tmv))
        snprintf(out, n, "/data/note/%04d%02d%02d_%02d%02d%02d.txt", tmv.tm_year + 1900, tmv.tm_mon + 1,
                 tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    else
        snprintf(out, n, "/data/note/nota.txt");
    path_make_unique(out, n);
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
            char path[80];
            quick_note_path(path, sizeof path);             // per-second name, never an existing file
            editor_open(path);
            return;
        }
        send_query(s_en ? L->t_en : L->t_it);
    } else {
        s_form_leaf = li; s_form_slot = 0; s_ses->slot[0][0] = s_ses->slot[1][0] = 0;
        menu_hint(); nucleo_app_request_draw();
    }
}
// Keys while a fill-in form is open: type into the active slot — ; . , / included, so "2.5", "3,75",
// "12/25" or "10:30" go in as typed — ENTER advances (or sends on the last slot), DEL backspaces (and
// steps back a slot when empty). The arrow layer: fn+; / fn+. hop between slots, fn+/ accepts the
// recent-value ghost (one-press fill). ',' arrives via on_back (Left) and is re-routed here.
static void idee_form_key(int key, char ch)
{
    const Leaf *L = &LEAVES[s_form_leaf];
    int sl = s_form_slot;
    if (key_mod() && (key == NK_UP || key == NK_DOWN || key == NK_RIGHT)) {
        char g[40];
        if      (key == NK_UP)   { if (sl > 0)            s_form_slot--; }
        else if (key == NK_DOWN) { if (sl < L->slots - 1) s_form_slot++; }
        else if (s_ses->slot[sl][0] && slot_autocomplete(s_ses->slot[sl], g, sizeof g)) snprintf(s_ses->slot[sl], sizeof s_ses->slot[sl], "%s", g);
        menu_hint(); nucleo_app_request_draw(); return;
    }
    if (key == NK_ENTER) {
        if (s_ses->slot[sl][0] == 0) { nucleo_app_request_draw(); return; }   // need a value before advancing
        if (sl < L->slots - 1) { s_form_slot++; }                        // -> next slot
        else {                                                           // last slot: assemble + send
            for (int i = 0; i < L->slots; i++) recent_push(s_ses->slot[i]);   // remember values for next time
            // EDITOR leaf (sentinel in p2): instead of sending a query, open the full-screen textarea on
            // the path just typed (slot 0). Content is written on Ctrl+S. (Crea file / Nota rapida.)
            if (L->p2_it && !strcmp(L->p2_it, "@editor")) { editor_open(s_ses->slot[0]); return; }
            char q[A_INMAX]; form_build_query(q, sizeof q, false);
            s_menu_open = false; reset_idee();
            nucleo_app_set_hint(chat_hint()); mark_all_dirty();
            send_query(q);
            return;
        }
    } else if (key == NK_DEL) {
        int n = (int)strlen(s_ses->slot[sl]);
        if (n > 0)        s_ses->slot[sl][n - 1] = 0;
        else if (sl > 0)  s_form_slot--;                                 // empty backspace -> previous slot
    } else if (ch >= 32 && ch < 127) {                                   // plain ; . / arrive here with their char
        int n = (int)strlen(s_ses->slot[sl]);
        if (n < (int)sizeof(s_ses->slot[0]) - 1) { s_ses->slot[sl][n] = ch; s_ses->slot[sl][n + 1] = 0; }
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
        if (ch >= '1' && ch <= '0' + n && ch <= '9') { s_ses->focus_leaf[s_idee_cat] = ch - '1'; activate_leaf(cat_leaf_at(s_idee_cat, ch - '1')); return; }
        if      (key == NK_UP)    s_mrow = (s_mrow > 0) ? s_mrow - 1 : -1;
        else if (key == NK_DOWN)  { if (s_mrow < n - 1) s_mrow++; }
        else if (key == NK_ENTER && s_mrow >= 0) { activate_leaf(cat_leaf_at(s_idee_cat, s_mrow)); return; }
        else if (is_alpha(ch)) {                                         // type-ahead over the leaf labels
            char lc = lc1(ch);
            for (int k = 1; k <= n; k++) { int i = ((s_mrow < 0 ? -1 : s_mrow) + k + n) % n; const Leaf *L = &LEAVES[cat_leaf_at(s_idee_cat, i)]; const char *lab = s_en ? L->l_en : L->l_it; if (lc1(lab[0]) == lc) { s_mrow = i; break; } }
        }
        else return;
        if (s_mrow >= 0) s_ses->focus_leaf[s_idee_cat] = s_mrow;             // remember where you were
    }
    menu_hint(); nucleo_app_request_draw();
}

// ---- full-screen text editor (file creation from IDEE) ----------------------
// Open the editor on the path the "Crea file" form collected (slot 0). Closes the menu so the editor
// owns the whole screen — a plain textarea, exactly what the user asked for.
static void editor_close(void) { s_ed_open = false; free(s_ed); s_ed = nullptr; }   // buffer back to the heap
static void editor_open(const char *path)
{
    if (!s_ed) s_ed = (AnimaEditor *)calloc(1, sizeof *s_ed);   // ~1.6 KB, only while the editor is open
    if (!s_ed) {                                     // no RAM for the buffer: say so instead of a dead key
        push_meta(s_en ? "Not enough memory for the editor." : "Memoria insufficiente per l'editor.", AMBER);
        mark_all_dirty(); nucleo_app_request_draw();
        return;
    }
    while (*path == ' ') path++;
    if (path[0] == '/') snprintf(s_ed->path, sizeof s_ed->path, "%s", path);
    else                snprintf(s_ed->path, sizeof s_ed->path, "/data/%s", path);   // default to /data/
    s_ed->buf[0] = 0; s_ed_len = 0; s_ed_scroll = 0; s_ed_open = true; s_ed->full = true;
    s_form_leaf = -1; s_menu_open = false; reset_idee();
    nucleo_app_set_hint(s_en ? "Enter=newline  Ctrl+S save  Esc cancel"
                             : "Invio=a capo  Ctrl+S salva  Esc annulla");
    mark_all_dirty(); nucleo_app_request_draw();
}
// Write the buffer to the SD path (guarded: absolute "/..." path, no ".."). NEVER overwrites: the
// editor starts empty, so writing over an existing file would silently wipe it — an existing name
// becomes name-2, name-3... Creates the parent dir, stops audio first (sequenziale: una risorsa per
// volta), pets the WDT around the SD I/O. Success: confirmation in the chat + editor closed. FAILURE:
// the editor STAYS OPEN with the text intact and the footer says so; a user path that can't be written
// (bad name, missing parent chain) is re-pointed to a fresh /data/note/ name — the title bar shows it —
// so the very next Ctrl+S can still rescue the text.
static void editor_save(void)
{
    bool ok = false;
    if (s_ed->path[0] == '/' && !strstr(s_ed->path, "..") && path_make_unique(s_ed->path, sizeof s_ed->path)) {
        char full[180]; snprintf(full, sizeof full, NUCLEO_SD_MOUNT "%s", s_ed->path);
        char dir[180];  snprintf(dir, sizeof dir, "%s", full);
        char *slash = strrchr(dir, '/'); if (slash && slash != dir) { *slash = 0; mkdir(dir, 0775); }
        nucleo_audio_stop();                                              // no audio while we touch the SD
        if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();
        FILE *f = fopen(full, "wb");
        if (f) {
            ok = (s_ed_len == 0 || fwrite(s_ed->buf, 1, (size_t)s_ed_len, f) == (size_t)s_ed_len);   // SD full = short write
            if (fclose(f) != 0) ok = false;
            if (!ok) remove(full);                                        // drop the half-written NEW file (unique name: ours)
        }
    }
    if (!ok) {                                                            // keep the editor + text; say what happened
        bool in_notes = !strncmp(s_ed->path, "/data/note/", 11);
        if (!in_notes) quick_note_path(s_ed->path, sizeof s_ed->path);     // rescue target for the retry
        nucleo_app_set_hint(in_notes ? (s_en ? "ERROR: not saved. Ctrl+S to retry" : "ERRORE: non salvato. Ctrl+S riprova")
                                     : (s_en ? "Not saved. Ctrl+S: save to note/" : "Non salvato. Ctrl+S: salva in note/"));
        mark_all_dirty(); nucleo_app_request_draw();
        return;
    }
    nucleo_anima_note_file(s_ed->path);
    char reply[120];
    const char *bn = strrchr(s_ed->path, '/'); bn = bn ? bn + 1 : s_ed->path;
    snprintf(reply, sizeof reply, s_en ? "Saved %s (%d chars)." : "Salvato %s (%d caratteri).", bn, s_ed_len);
    editor_close();                                  // frees s_ed: bn is not used past this point
    push_user(s_en ? "[new file]" : "[nuovo file]");
    push_anima(reply, ACC);
    s_user_sent = true;
    nucleo_app_set_hint(chat_hint());
    mark_all_dirty(); nucleo_app_request_draw();
    save_chat();
}
static void editor_cancel(void)
{
    editor_close();
    nucleo_app_set_hint(chat_hint());
    mark_all_dirty(); nucleo_app_request_draw();
}
// Editor keystrokes: printable -> append (; . / included); Enter -> newline; DEL -> backspace; Ctrl+S ->
// save (printed ctrl OR fn: the driver's bit names are offset, see key_fn). (Esc and the comma key arrive
// via on_back — the launcher intercepts them — and are handled there: Esc cancels (confirming first when
// there is text), Left/',' types a literal comma so the textarea isn't comma-blind.)
static void editor_key(int key, char ch)
{
    if (key_mod() && (ch == 's' || ch == 'S' || ch == 0x13)) { editor_save(); return; }
    if (key == NK_ENTER) {
        if (s_ed_len < ED_BUF_CAP - 1) { s_ed->buf[s_ed_len++] = '\n'; s_ed->buf[s_ed_len] = 0; }
    } else if (key == NK_DEL) {
        if (s_ed_len > 0) s_ed->buf[--s_ed_len] = 0;
    } else if (ch >= 32 && ch < 127) {
        if (s_ed_len < ED_BUF_CAP - 1) { s_ed->buf[s_ed_len++] = ch; s_ed->buf[s_ed_len] = 0; }
    } else return;
    s_ed->dirty = true;                                   // repaint the text (in place); the blink alone touches only the caret
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
        else if (key == NK_ENTER)               { s_edit = false; nucleo_app_persist_prefs(); }   // save volume/brightness on exit
        menu_hint(); nucleo_app_request_draw(); return;     // hint aggiorna il % della velocita' live
    }
    if      (key == NK_UP)   s_mrow = (s_mrow > 0) ? s_mrow - 1 : -1;   // row 0 -> back to the tab bar
    else if (key == NK_DOWN) { if (s_mrow < IA_ROWS - 1) s_mrow++; }
    else if (key == NK_ENTER) {
        switch (s_mrow) {
            case IA_ONLINE: s_omode = (s_omode + 1) % 3; apply_online_mode(); save_settings(); break;
            case IA_LANG:                                    // the OS language itself (settings.json ui.language)
                nucleo_i18n_set_en(!s_en); s_en = nucleo_i18n_is_en(); save_settings();
                if (s_ses->vmode == V_MANUAL) s_ses->vmode = V_FOLLOW;
                rebuild_rows(); break;                       // re-wrap + relabel
            case IA_TEXT:                                    // re-wrap the transcript: row indices change
                s_big = !s_big; save_settings();
                if (s_ses->vmode == V_MANUAL) s_ses->vmode = V_FOLLOW;
                rebuild_rows(); break;
            case IA_VOICE:  if (nucleo_tts_available()) nucleo_tts_set_enabled(!nucleo_tts_enabled()); break;
            case IA_SPEED:  s_edit = true; break;            // -> L/R adjust mode (velocita' voce)
            case IA_VOL:    s_edit = true; break;            // -> L/R adjust mode
            case IA_BRI:    s_edit = true; break;
            case IA_CLEAR:  ask_clear(); return;           // destructive: the confirm card first
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

// fn+/ on an empty line: cycle Offline -> Ibrido -> Solo online. The header label changes and a short
// footer toast names the new mode — on the welcome deck too, where it used to switch silently. In the
// chat the transcript also gets the usual "Modalita:" line; on the deck it doesn't (it would be
// invisible there, and a persisted meta line would replace the deck on the next open).
static void cycle_mode_key(void)
{
    if (deck_active()) { s_omode = (s_omode + 1) % 3; apply_online_mode(); save_settings(); }
    else cycle_mode();
    char t[40];
    snprintf(t, sizeof t, s_en ? "Mode: %s  (fn / next)" : "Modo: %s  (fn / cambia)", mode_name(s_omode));
    nucleo_app_set_hint(t);
    s_toast_until = esp_timer_get_time() + 2500000;         // tick() puts the key hint back after 2.5 s
    s_d_hdr = true; nucleo_app_request_draw();
}

// fn+/ right after an offline "I don't know" while the network is up: switch to Hybrid and ask the same
// question again (the next step the answer offered). The header label shows the new mode.
static void retry_online(void)
{
    s_ses->retry_online = false;
    s_omode = OM_ON; apply_online_mode(); save_settings();
    push_meta(s_en ? "Mode: Hybrid - asking online" : "Modo: Ibrido - chiedo online", GRN);
    // Straight into the input line (no local copy: this frame stays on the 26 KB Solo stack for the whole
    // blocking query). A bounded memmove — the source may be another member of the same session block.
    const char *h = hist_at(0), *src = h ? h : s_ses->req;
    size_t L = strlen(src); if (L > A_INMAX - 1) L = A_INMAX - 1;
    if (!L || src[0] == '/') return;
    memmove(s_ses->input, src, L); s_ses->input[L] = 0; s_ilen = (int)L;
    submit();
}

static void on_key(int key, char ch)
{
    if (!s_ses) return;                                     // no session block: only Esc (on_back) works
    if (s_exit_confirm) {                                   // modale conferma: Invio = conferma, altro = annulla
        s_exit_confirm = false;
        if (s_ed_open) {                                    // editor "discard the text?": Enter only (letters are
            if (key == NK_ENTER) editor_cancel();           // what you type — a stray 's' must never discard a note)
            else { mark_all_dirty(); nucleo_app_request_draw(); }
            return;
        }
        if (key == NK_ENTER || ch == 's' || ch == 'S' || ch == 'y' || ch == 'Y')
            nucleo_app_exit();                             // conferma -> chiude (in Solo = esp_restart, NON ritorna)
        else { mark_all_dirty(); nucleo_app_request_draw(); }   // annulla -> torna alla chat
        return;
    }
    if (s_ses->clear_confirm) {                             // the confirm card owns every key
        int r = app_ui_confirm_key(key, ch, &s_ses->clear_yes);
        if (r >= 0) clear_confirm_done(r == 1); else nucleo_app_request_draw();
        return;
    }
    if (s_ed_open)   { editor_key(key, ch); return; }       // the full-screen editor owns every key
    if (s_menu_open) { menu_key(key, ch); return; }
    if (s_ses->reader) {                                    // reader: no text entry, so the plain arrows page
        if (key == NK_ENTER || key == NK_DEL) { reader_close(); return; }
        if (key == NK_UP || key == NK_DOWN || key == NK_RIGHT || ch == ' ') {
            view_page(key == NK_UP ? -1 : +1, RD_BODY); nucleo_app_request_draw(); return;
        }
        if (ch > 32 && ch < 127 && !key_mod()) { reader_close(); chat_type(ch); }   // a letter: back to the chat, typed
        return;
    }

    // ---- chat + welcome deck. ; . / TYPE (see the header note); the arrow meaning needs a modifier:
    //      fn+; fn+.  scroll the transcript (on the deck: move the pick)
    //      ctrl+; ctrl+.  command history (from the deck too: it steps aside for the recalled line)
    //      fn+/  accept the ghost completion; on an empty line cycle the online mode
    const bool was_deck = deck_active();
    if ((key == NK_UP || key == NK_DOWN) && key_ctrl()) {
        hist_recall(key == NK_UP ? -1 : +1);
        chat_changed(was_deck); return;
    }
    if ((key == NK_UP || key == NK_DOWN) && key_fn()) {
        if (was_deck) {
            if (key == NK_UP)   { if (s_sug_sel > 0)         { s_sug_sel--; s_d_body = true; } }
            else                { if (s_sug_sel < SUG_N - 1) { s_sug_sel++; s_d_body = true; } }
        } else if (s_ilen == 0) view_page(key == NK_UP ? -1 : +1, chat_avail());   // empty line: a PAGE per press
        else                     view_scroll(key == NK_UP ? -1 : +1, chat_avail());  // while typing: a row
        nucleo_app_request_draw(); return;
    }
    if (key == NK_RIGHT && key_mod()) {
        if (s_ilen == 0) {
            if (s_ses->retry_online && s_omode == OM_OFF) retry_online(); else cycle_mode_key();
            return;
        }
        char g[HIST_LEN];
        if (autocomplete(s_ses->input, g, sizeof g)) {           // no ghost -> nothing (fn+/ never types a '/')
            snprintf(s_ses->input, sizeof s_ses->input, "%s", g); s_ilen = (int)strlen(s_ses->input);
            s_hist_nav = -1; chat_changed(was_deck);
        }
        return;
    }
    if (was_deck && (key == NK_ENTER || (ch >= '1' && ch <= '9' && ch <= '0' + SUG_N))) {   // the deck's own picks
        if (key != NK_ENTER) s_sug_sel = ch - '1';          // digit 1-9 jump+send (never punctuation past '9')
        run_suggestion();
        if (!s_busy) nucleo_app_set_hint(chat_hint());      // deck -> chat hint (a running turn keeps "stop")
        return;
    }

    if (key == NK_ENTER) {                                  // Invio mentre elabora = stop (the inline turn polls it itself: turn_key)
        if (!s_busy && s_ilen == 0) {                        // empty line: open the offered app, else the reader
            if (s_ses->launch[0]) launch_now(); else reader_open();
            return;
        }
        if (s_busy) cancel_query(); else submit();
        if (!s_busy && !s_ses->clear_confirm) nucleo_app_set_hint(chat_hint());   // line is empty again (a confirm card keeps its hint)
    }
    else if (key == NK_DEL)  {
        if (s_ilen > 0) { s_ses->input[--s_ilen] = 0; s_hist_nav = -1; chat_changed(was_deck); return; }   // deck returns on the last char
        else if (s_busy) cancel_query();
        else return;
    }
    else if (ch >= 32 && ch < 127) { chat_type(ch); return; }   // letters AND plain ; . / (the deck steps aside)
    else return;
    nucleo_app_request_draw();
}

static void tick(void)
{
    if (!s_ses) return;           // no session block (see enter): static notice, nothing to animate
    if (s_exit_confirm || s_ses->clear_confirm) return;   // a modal is up: static, no periodic redraws
    if (s_toast_until && esp_timer_get_time() >= s_toast_until) {   // mode toast expired -> the key hint returns
        s_toast_until = 0;
        if (!s_menu_open && !s_ed_open && !s_busy) nucleo_app_set_hint(chat_hint());
    }
    if (s_ed_open) { if ((++s_blink & 1) == 0) nucleo_app_request_draw(); return; }   // editor: blink the caret only
    if (s_menu_open || s_ses->reader) return;                               // the menu / the reader: no chat animation
    if (s_busy) { s_spin = (s_spin + 1) & 3; s_d_badge = true; s_d_input = true; nucleo_app_request_draw(); return; }   // anima sia "pensa..." (header) sia "sta scrivendo..." (input)
    time_t now = time(NULL); struct tm *tm = localtime(&now);               // header clock: repaint on a minute change
    int mn = tm ? tm->tm_min : -1;
    if (mn != s_clock_min) { s_clock_min = mn; s_d_hdr = true; nucleo_app_request_draw(); }
    else if ((++s_blink & 1) == 0 && !deck_active()) nucleo_app_request_draw();  // cursor blink (caret cell only)
}

// ---- tabbed menu: the kit's tab strip + settings rows (docs/native-ui-kit.md §3) -------------------
// The standard app_ui_tabs strip across the top 20px (active = ACC pill + INK, others LINE pills + MUTED);
// the content starts at 22. It fills its own band, so it is painted only on a menu scene change.
static void draw_tabbar(int active)
{
    d.setFont(&fonts::Font0); d.setTextSize(1);
    app_ui_tabs(0, s_en ? TABS_EN : TABS_IT, TAB_N, active, ACC);
    d.fillRect(0, 20, 240, 2, BG);
}
// A menu row band [y, y+h): focused = the accent pill (INK content), else BG (MUTED content). It paints the
// whole band, so repainting one row never needs a clear first; returns the content background.
static unsigned short row_band(int y, int h, bool focus)
{
    if (focus) { pill_band(4, y, 232, h - 2, 9, ACC, 240); d.fillRect(0, y + h - 2, 240, 2, BG); return ACC; }
    d.fillRect(0, y, 240, h, BG);
    return BG;
}
// Menu lists repaint per ROW: slot k (counted from the first visible row) is repainted only when what it
// shows changed (row, y, focus, value). A focus move then repaints two rows, not the screen.
#define MSIG_N 8
static uint32_t mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
static bool mrow_dirty(int k, uint32_t sig)
{
    sig |= 1u;                                          // 0 = "unknown" (forces a paint)
    if (k >= MSIG_N) return true;
    if (s_ses->msig[k] == sig) return false;
    s_ses->msig[k] = sig;
    return true;
}
// After a list: forget the slots below the last row and clear the band left below it (only when it moved).
static void mlist_tail(int k, int y, int ch)
{
    for (; k < MSIG_N; k++) s_ses->msig[k] = 0;
    if (y > ch) y = ch;
    if (y < ch && s_ses->mleft != y) d.fillRect(0, y, 240, ch - y, BG);
    s_ses->mleft = (short)y;
}

// One settings row — toggle pill / value chip / slider / action chevron — exactly like Music/Video.
// The focused row grows to 46px with an accent rail; neighbours are 30px. Label is size-2 Font0.
enum { SV_TEXT = 0, SV_TOGGLE, SV_SLIDER, SV_ACTION };
static void draw_set_row(int y, bool focus, const char *label, const char *val,
                         int kind, bool on, int slider_val)
{
    const int h = focus ? 46 : 30;
    const unsigned short bg = row_band(y, h, focus), ink = focus ? INK : MUTED;
    d.setFont(&fonts::Font0); d.setTextSize(2);
    d.setTextColor(ink, bg);
    d.setCursor(16, y + (h - 16) / 2 - 1); d.print(label);

    if (kind == SV_SLIDER) {                                        // track LINE, fill GRN, knob FG
        bool edit = focus && s_edit;
        int sw = focus ? 96 : 60, sh = 12, bx = 230 - sw, vy = y + (h - sh) / 2;
        d.fillRoundRect(bx, vy, sw, sh, sh / 2, LINE);
        int onw = slider_val * sw / 100; if (onw < 0) onw = 0; if (onw > sw) onw = sw;
        if (onw > 0) d.fillRoundRect(bx, vy, onw, sh, sh / 2, GRN);
        int kx = bx + onw; if (kx < bx + 6) kx = bx + 6; if (kx > bx + sw - 6) kx = bx + sw - 6;
        d.fillCircle(kx, vy + sh / 2, edit ? sh / 2 + 2 : sh / 2 + 1, FG);
        if (edit) d.drawRoundRect(bx - 2, vy - 2, sw + 4, sh + 4, (sh + 4) / 2, INK);
        return;
    }
    if (kind == SV_TOGGLE) {
        int sw = 42, sh = 20, bx = 230 - sw, vy = y + (h - sh) / 2;
        d.fillRoundRect(bx, vy, sw, sh, sh / 2, on ? GRN : LINE);
        int kx = on ? bx + sw - sh / 2 - 1 : bx + sh / 2 + 1;
        d.fillCircle(kx, vy + sh / 2, sh / 2 - 3, on ? INK : FG);
        return;
    }
    if (kind == SV_ACTION) {                                        // a chevron: Enter acts
        int ax = 214, ay = y + h / 2;
        d.fillTriangle(ax, ay - 5, ax, ay + 5, ax + 6, ay, ink);
        return;
    }
    if (val && val[0]) {                                            // SV_TEXT value, right-aligned
        d.setTextColor(ink, bg);
        d.setCursor(228 - (int)strlen(val) * 12, y + (h - 16) / 2 - 1); d.print(val);
    }
}

// Build the IA tab's eight rows: mode chip, language, text size, voice toggle, voice-SPEED slider,
// volume + screen sliders, and the clear-chat action. ASCII labels (Font0 has no glyphs for accents).
struct IAItem { const char *label; char val[14]; int kind; bool on; int slider; };
static int build_ia(IAItem *it)
{
    memset(it, 0, sizeof(IAItem) * IA_ROWS);
    it[IA_ONLINE].label = "Online"; it[IA_ONLINE].kind = SV_TEXT;
    snprintf(it[IA_ONLINE].val, 14, "%s", s_omode == OM_OFF ? "Off" : s_omode == OM_ONLY ? TR("Solo", "Only") : "On");
    it[IA_LANG].label = s_en ? "Lang" : "Lingua"; it[IA_LANG].kind = SV_TEXT;   // the OS language (Enter: IT <-> EN)
    snprintf(it[IA_LANG].val, 14, "%.2s", nucleo_i18n_lang());
    for (char *c = it[IA_LANG].val; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
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
static void wline(int x, int y, int lineh, const char *t, unsigned short col) { box_text(0, y, 240, lineh, x, y, t, col, BG); }
static int draw_wrapped(const char *text, int x, int y, int maxw, unsigned char font, unsigned short col, int lineh)
{
    set_font(font);
    char t[120];
    const char *ls = text, *p = text;
    while (*p) {
        if (*p == '\n') { int n = (int)(p - ls); if (n > 119) n = 119; memcpy(t, ls, n); t[n] = 0; wline(x, y, lineh, t, col); y += lineh; p++; ls = p; continue; }
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
                int n = take; memcpy(t, q, n); t[n] = 0; wline(x, y, lineh, t, col); y += lineh; q += take;
            }
            ls = we; p = we;
        } else {
            int n = (int)(p - ls); if (n > 119) n = 119; memcpy(t, ls, n); t[n] = 0; wline(x, y, lineh, t, col); y += lineh;
            while (*p == ' ') p++;
            ls = p;
        }
    }
    if (p > ls) { int n = (int)(p - ls); if (n > 119) n = 119; memcpy(t, ls, n); t[n] = 0; wline(x, y, lineh, t, col); y += lineh; }
    return y;
}

// ---- the navigable manual (GUIDA tab) ---------------------------------------
// A smartwatch-style card carousel: one topic per card, a BIG bold title + a few BIG FreeSans lines,
// with page dots at the foot showing position. UP/DOWN flip cards, 1-9 jump. Content is hand-verified
// against the real app (no invented features) and each body is hand-fit to <=3 short lines so the big
// type always clears the 240x99 body region — no clipped text.
typedef struct { const char *title, *body; } GuidePage;
// Each \n-segment is hand-kept short (<=~20 chars) so it stays one line in the big proportional FreeSans
// (~9-11px/char, 224px budget) and every card is <=3 lines — fits below the title rule, above the dots.
static const GuidePage GUIDE_IT[GUIDE_N] = {
    { "ANIMA",         "Assistente offline.\nScrivi e premi Invio.\nVa anche senza rete." },
    { "Tasti",         "fn ;/.  pagina la chat\nctrl ;/.  cronologia\nfn /  completa, modo" },
    { "Lettura",       "Invio a riga vuota:\nlettore a schermo\npieno, ;/. pagina." },
    { "Menu",          "TAB apre il menu.\nInvio apri, Esc su.\nDx/Sx cambia scheda." },
    { "Cosa chiedere", "Ora, meteo, calcoli,\npromemoria, traduzioni.\n\"Apri Musica\" + Invio." },
    { "IDEE",          "Catalogo di cio' che\nso fare. Invio entra.\n\"...\" chiede i dati." },
    { "Modalita",      "Offline: solo qui.\nIbrido: poi il cloud.\nSolo online: cloud." },
    { "Solo online",   "Usa un LLM nel cloud.\nServe Wi-Fi e una\nchiave API (da web)." },
    { "Voce",          "Legge le risposte.\nAmbra = aspetto una\ntua risposta." },
};
static const GuidePage GUIDE_EN[GUIDE_N] = {
    { "ANIMA",         "Offline assistant.\nType and press Enter.\nWorks with no network." },
    { "Keys",          "fn ;/.  page the chat\nctrl ;/.  history\nfn /  complete, mode" },
    { "Reading",       "Enter on empty line:\nfull-screen reader,\n;/. flips pages." },
    { "Menu",          "TAB opens the menu.\nEnter opens, Esc back.\nLeft/Right switch tab." },
    { "What to ask",   "Time, weather, math,\nreminders, translate.\n\"Open Music\" + Enter." },
    { "IDEAS",         "Catalog of all I\ncan do. Enter to open.\n\"...\" asks for input." },
    { "Modes",         "Offline: device only.\nHybrid: then cloud.\nOnline: cloud only." },
    { "Online only",   "Uses a cloud LLM.\nNeeds Wi-Fi and an\nAPI key (from web)." },
    { "Voice",         "Reads answers aloud.\nAmber = waiting for\nyour reply." },
};
// A guide card: BIG bold violet title under a hairline, BIG anti-aliased FreeSans body, and a row of
// carousel page dots at the foot (current = a fat ACC dot, the others small + MUTED) — the modern
// "page N of M" affordance that replaces the old cramped "1/5" counter.
static void draw_guide(int ch)
{
    int pg = s_mrow < 0 ? 0 : s_mrow;
    if (pg >= GUIDE_N) pg = GUIDE_N - 1;
    if (!s_ses->mfull && s_ses->mpage == pg) return;     // same card on the panel: nothing to repaint
    s_ses->mpage = (short)pg;
    const GuidePage *g = (s_en ? GUIDE_EN : GUIDE_IT) + pg;
    set_font(F_BOLD);                                     // big bold title (was a tiny 6x8), painted in place
    box_text(0, 22, 240, 21, 8, 22, g->title, ACC, BG);
    d.drawFastHLine(8, 43, 224, LINE);
    d.setClipRect(0, 45, 240, ch - 53);                  // body band, kept clear of the title and the dots
    int by = draw_wrapped(g->body, 8, 47, 224, F_BIG, FG, 18);   // big anti-aliased FreeSans body
    d.clearClipRect();
    if (by < ch - 8) d.fillRect(0, by, 240, ch - 8 - by, BG);
    int gap = 14, span = (GUIDE_N - 1) * gap, x0 = 120 - span / 2, dy = ch - 6;          // carousel page dots
    for (int i = 0; i < GUIDE_N; i++) {
        d.fillRect(x0 + i * gap - 3, dy - 3, 7, 7, BG);  // a dot's own 7x7 box: the only thing that clears
        d.fillCircle(x0 + i * gap, dy, i == pg ? 3 : 2, i == pg ? ACC : MUTED);
    }
    d.setFont(&fonts::Font0); d.setTextSize(1);          // leave the global font at the framework default
}

// ---- IA tab: the settings list (Music/Video parity) -------------------------
// Same natural-scroll list as the other tabs: the focused row enlarges in place and the list scrolls
// only when the focus reaches an edge (no centre-pinning). Clipped to the body region so nothing bleeds.
static void draw_ia(int ch)
{
    IAItem it[IA_ROWS]; int n = build_ia(it);
    const int top = 24, f = s_mrow;
    d.setClipRect(0, 22, 240, ch - 22);
    int yy = list_scroll_y0(top, ch - top, n, f), k = 0;
    for (int i = 0; i < n; i++) {
        int hh = (i == f) ? 46 : 30;
        if (yy + hh > 22 && yy < ch) {
            uint32_t sg = mix(mix(mix(mix(2166136261u, 0x1A00u + i), ((uint32_t)yy << 2) | (i == f) | ((i == f && s_edit) << 1)),
                                  (uint32_t)(it[i].slider * 2 + it[i].on)), (uint32_t)(it[i].val[0] | it[i].val[1] << 8 | it[i].val[2] << 16));
            if (mrow_dirty(k, sg)) draw_set_row(yy, i == f, it[i].label, it[i].val, it[i].kind, it[i].on, it[i].slider);
            k++;
        }
        yy += hh;
    }
    d.clearClipRect();
    mlist_tail(k, yy, ch);
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
    stato_row(y, TR("Rete", "Net"), v, (ip && ip[0]) ? GRN : MUTED);
    if (ip && ip[0] && ssid && ssid[0]) {                                // SSID as a small grey tag, right-aligned
        char sb[18]; snprintf(sb, sizeof sb, "%.16s", ssid);
        d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextColor(DIM, BG);
        d.setCursor(232 - (int)strlen(sb) * 6, y + 4); d.print(sb);
    }
    y += step;

    char lc[4]; snprintf(lc, sizeof lc, "%.2s", nucleo_i18n_lang()); for (char *c = lc; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    snprintf(v, sizeof v, "%s  %s", mode_name(s_omode), lc);
    // Plain FG: the old "amber if no worker" was a permanent false alarm — ANIMA always runs in Solo, where
    // queries run inline and a worker never exists.
    stato_row(y, s_en ? "Mode" : "Modo", v, FG); y += step;

    long up = (long)(esp_timer_get_time() / 1000000);
    int uh = (int)(up / 3600), um = (int)((up % 3600) / 60);
    char ut[16]; if (uh) snprintf(ut, sizeof ut, "%dh%dm", uh, um); else snprintf(ut, sizeof ut, "%dm", um);
    snprintf(v, sizeof v, s_en ? "%s  %d ev" : "%s  %d oggi", ut, s_today_count);
    stato_row(y, s_en ? "On" : "Acceso", v, FG); y += step;

    if (s_ses->last_tag[0] && y <= ymax) {                               // last answer's source + time + confidence
        char tg[MSG_TAG]; snprintf(tg, sizeof tg, "%s", s_ses->last_tag);
        char *bar = strchr(tg, '|'); if (bar) *bar = ' ';
        snprintf(v, sizeof v, "%s  %d%%", tg, s_last_conf < 0 ? 0 : s_last_conf);
        stato_row(y, s_en ? "Last" : "Ultima", v, ACC); y += step;
    }
}

// Truncate s in place so it fits `budget` px in the CURRENT font. When it actually has to cut it ends
// with a ".." ellipsis (the GFX ASCII fonts have no "…" glyph) — the modern "clipped, there's more"
// affordance instead of a stop mid-word. Never writes past the original NUL, so it's safe even on a
// full fixed buffer. Keeps a long prompt from bleeding past its capsule.
static void fit_w(char *s, int budget)
{
    int L = (int)strlen(s);
    if (L == 0 || (int)d.textWidth(s) <= budget) return;          // already fits — leave it untouched
    for (int n = L - 2; n > 0; n--) {                             // longest "<prefix>.." that fits, first
        s[n] = '.'; s[n + 1] = '.'; s[n + 2] = 0;                 // n+2 <= L: stays inside the buffer
        if ((int)d.textWidth(s) <= budget) return;
    }
    int n = (int)strlen(s);                                       // pathologically narrow budget: hard clip
    while (n > 0 && (int)d.textWidth(s) > budget) s[--n] = 0;
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

// One category row, framed like an IA settings row: the ACC pill (INK content) when focused. A subtle
// quick-pick index (1-9) on the far left, the identity glyph, the big size-2 label, and on the right a
// small leaf-count badge + the drill chevron. Everything is vertically centred for either row height.
static void draw_cat_row(int y, bool focus, int i)
{
    const int h = focus ? 46 : 30;
    const unsigned short bg = row_band(y, h, focus), ink = focus ? INK : MUTED;
    int cy = y + h / 2, ty = y + (h - 16) / 2;
    char num[4]; snprintf(num, sizeof num, "%d", i + 1);                     // quick-pick index, subtle
    d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextColor(focus ? INK : DIM, bg);
    d.setCursor(11, cy - 3); d.print(num);
    draw_cat_icon(i, 22, ty, ink);                                           // identity glyph
    d.setFont(&fonts::Font0); d.setTextSize(2); d.setTextColor(ink, bg);     // big label
    char line[40]; snprintf(line, sizeof line, "%s", cat_label(i));
    fit_w(line, 150);                                                        // ".." if longer; room for badge + chevron
    d.setCursor(44, ty - 1); d.print(line);
    char cnt[6]; snprintf(cnt, sizeof cnt, "%d", cat_leaf_count(i));         // leaf-count badge, right of the label
    d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextColor(ink, bg);
    d.setCursor(206 - (int)strlen(cnt) * 6, cy - 3); d.print(cnt);
    d.fillTriangle(222, cy - 4, 222, cy + 4, 227, cy, ink);                  // drill chevron
}

// One leaf row, framed like an IA settings row: a subtle quick-pick index on the left, the big size-2
// label, and — for leaves that open a fill-in form — a "..." badge on the right (the "needs input" cue).
static void draw_leaf_row(int y, bool focus, int c, int i)
{
    const int h = focus ? 46 : 30;
    const Leaf *L = &LEAVES[cat_leaf_at(c, i)];
    const unsigned short bg = row_band(y, h, focus), ink = focus ? INK : MUTED;
    bool form = L->slots > 0;
    int cy = y + h / 2, ty = y + (h - 16) / 2;
    char num[4]; snprintf(num, sizeof num, "%d", i + 1);                     // quick-pick index, subtle
    d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextColor(focus ? INK : DIM, bg);
    d.setCursor(11, cy - 3); d.print(num);
    d.setFont(&fonts::Font0); d.setTextSize(2); d.setTextColor(ink, bg);     // big label
    char line[44]; snprintf(line, sizeof line, "%s", s_en ? L->l_en : L->l_it);
    fit_w(line, form ? 152 : 188);                                           // ".." if longer; room for the form badge
    d.setCursor(34, ty - 1); d.print(line);
    if (form) { d.setFont(&fonts::Font0); d.setTextSize(2); d.setTextColor(ink, bg);
                d.setCursor(202, ty - 1); d.print("..."); }
}

// Natural list scroll shared by the IDEE/OGGI/IA lists. A persistent pixel offset (s_list_scroll) that
// only moves when the focused row would fall outside the viewport — so the selection scrolls like a
// normal list (riding the top/bottom edge), NOT pinned to the vertical centre. Rows are 30px, the
// focused one 46px. Returns row 0's y (top - scroll). f < 0 (tab bar focused) = anchored at the top.
static int list_scroll_y0(int top, int avail, int n, int f)
{
    if (f < 0) { s_list_scroll = 0; return top; }
    int content = n * 30 + 16;                               // the focused row is 46 (= 30 + 16)
    int maxs = content > avail ? content - avail : 0;
    int ftop = f * 30, fbot = ftop + 46;                     // focused row in content coords (rows above are 30)
    if (ftop - s_list_scroll < 0)     s_list_scroll = ftop;          // above the viewport -> reveal its top
    if (fbot - s_list_scroll > avail) s_list_scroll = fbot - avail;  // below the viewport -> reveal its bottom
    if (s_list_scroll < 0)    s_list_scroll = 0;
    if (s_list_scroll > maxs) s_list_scroll = maxs;
    return top - s_list_scroll;
}

// Categories: a top-packed list with natural scroll (the focused category enlarges in place; the list
// scrolls only when it reaches an edge). A partly-clipped row at an edge is itself the "more" cue.
static void draw_idee_cats(int ch)
{
    const int top = 24, f = s_mrow;
    d.setClipRect(0, 22, 240, ch - 22);
    int yy = list_scroll_y0(top, ch - top, CAT_N, f), k = 0;
    for (int i = 0; i < CAT_N; i++) {
        int hh = (i == f) ? 46 : 30;
        if (yy + hh > 22 && yy < ch) {
            if (mrow_dirty(k, mix(mix(2166136261u, 0xC000u + i), ((uint32_t)yy << 1) | (i == f)))) draw_cat_row(yy, i == f, i);
            k++;
        }
        yy += hh;
    }
    d.clearClipRect();
    mlist_tail(k, yy, ch);
}

// Leaves of the open category: a slim "[icon] < Category   N" breadcrumb, then the same naturally-
// scrolling list below it (focused leaf enlarged in place).
static void draw_idee_leaves(int ch)
{
    int c = s_idee_cat, n = cat_leaf_count(c);
    if (s_ses->mfull) {                           // the breadcrumb is static for the whole scene
        draw_cat_icon(c, 6, 22, ACC);             // breadcrumb icon carries the category identity down
        d.setFont(&fonts::Font0); d.setTextSize(1);
        char hd[40]; snprintf(hd, sizeof hd, "< %s", cat_label(c));
        d.setTextColor(ACC, BG); d.setCursor(28, 27); d.print(hd);
        char cc[6]; snprintf(cc, sizeof cc, "%d", n);
        d.setTextColor(MUTED, BG); d.setCursor(232 - (int)strlen(cc) * 6, 27); d.print(cc);
        d.drawFastHLine(8, 40, 224, LINE);
    }
    const int top = 44, f = s_mrow;
    d.setClipRect(0, 42, 240, ch - 42);
    int yy = list_scroll_y0(top, ch - top, n, f), k = 0;
    for (int i = 0; i < n; i++) {
        int hh = (i == f) ? 46 : 30;
        if (yy + hh > 42 && yy < ch) {
            if (mrow_dirty(k, mix(mix(2166136261u, 0x1E00u + i), ((uint32_t)yy << 1) | (i == f)))) draw_leaf_row(yy, i == f, c, i);
            k++;
        }
        yy += hh;
    }
    d.clearClipRect();
    mlist_tail(k, yy, ch);
}

// The fill-in form, redesigned as a full-screen FOCUS wizard (one field at a time, BIG type): a slim
// breadcrumb + step dots up top, then the field's question in a large font, a tall full-width value box
// with size-2 digits + a one-press recent-value ghost, and a live preview of the question taking shape.
// Showing a single slot huge (instead of cramming both small) is the readability win on this 240px panel.
static void draw_idee_form(int ch)
{
    const Leaf *L = &LEAVES[s_form_leaf];
    int s = s_form_slot;
    const int bx = 8, bw = 224, by = 64, bh = 38;

    // -- breadcrumb: icon + "Cat > Leaf" (left) + the value box frame: static for the whole form --
    if (s_ses->mfull) {
        draw_cat_icon(L->cat, 6, 22, ACC);
        d.setFont(&fonts::Font0); d.setTextSize(1);
        char hd[52]; snprintf(hd, sizeof hd, "%s > %s", cat_label(L->cat), s_en ? L->l_en : L->l_it);
        while (hd[0] && (int)d.textWidth(hd) > 150) hd[strlen(hd) - 1] = 0;
        d.setTextColor(ACC, BG); d.setCursor(28, 27); d.print(hd);
        d.drawFastHLine(8, 40, 224, LINE);
        d.drawRoundRect(bx, by, bw, bh, 8, ACC); d.drawRoundRect(bx + 1, by + 1, bw - 2, bh - 2, 7, ACC);   // the active field
        d.fillRect(bx + 4, by + 5, 4, bh - 10, ACC);            // active rail
    }
    // -- step "n/N" + dots (multi-field forms): a fixed field and each dot's own box --
    if (L->slots > 1) {
        char sp[12]; snprintf(sp, sizeof sp, "%d/%d", s + 1, L->slots);
        d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(186, 27); d.print(sp);
        for (int i = 0; i < L->slots; i++) {
            int dx = 224 - (L->slots - 1 - i) * 11, dy = 30;
            d.fillRect(dx - 4, dy - 4, 9, 9, BG);
            if      (i <  s) d.fillCircle(dx, dy, 3, GRN);
            else if (i == s) d.fillCircle(dx, dy, 4, ACC);
            else             d.drawCircle(dx, dy, 3, MUTED);
        }
    }
    // -- BIG question label (this slot's field), opaque in its band --
    const char *pl = (s == 0) ? (s_en ? L->p1_en : L->p1_it) : (s_en ? L->p2_en : L->p2_it);
    char lab[40]; snprintf(lab, sizeof lab, "%s", pl ? pl : "");
    d.setFont(&fonts::FreeSans9pt7b); d.setTextSize(1);
    while (lab[0] && (int)d.textWidth(lab) > 224) lab[strlen(lab) - 1] = 0;
    box_text(0, 42, 240, by - 42, 10, 44, lab, FG, BG);

    // -- the value, painted in place inside the frame: text, caret, recent-value ghost, then the leftover --
    const int ix0 = bx + 9, ix1 = bx + bw - 3, iy0 = by + 3, ih = bh - 6;
    const int vx = bx + 14, vy = by + (bh - 32) / 2;            // size-2 Font2 glyph is ~32px tall
    d.setFont(&fonts::Font2);
    d.setClipRect(ix0, iy0, ix1 - ix0, ih);                    // a long value never paints over the frame
    int cx = vx;
    if (s_ses->slot[s][0]) {
        d.setTextSize(2);
        const char *sv = s_ses->slot[s];                        // long value: show its tail, like the chat line
        while (sv[1] && (int)d.textWidth(sv) > ix1 - 6 - vx) sv++;
        d.fillRect(ix0, iy0, vx - ix0, ih, BG);
        d.setTextColor(FG, BG); d.setCursor(vx, vy); d.print(sv);
        cx = vx + (int)d.textWidth(sv); if (cx > ix1 - 5) cx = ix1 - 5;
    } else {
        d.setTextSize(1);
        cx = ix0 + 1;
        box_text(cx + 5, iy0, ix1 - cx - 5, ih, vx, by + (bh - 16) / 2, s_en ? "type..." : "scrivi...", DIM, BG);
        d.fillRect(ix0, iy0, 1, ih, BG);
    }
    d.fillRect(cx, iy0, 1, ih, BG); d.fillRect(cx + 1, vy, 3, 30, GRN); d.fillRect(cx + 4, iy0, 1, ih, BG);   // big caret
    if (vy + 30 < iy0 + ih) d.fillRect(cx + 1, vy + 30, 3, iy0 + ih - vy - 30, BG);
    if (s_ses->slot[s][0]) {
        char g[40]; int ge = cx + 5;                            // ghost: a past value with this prefix
        if (cx < bx + bw - 36 && slot_autocomplete(s_ses->slot[s], g, sizeof g)) {
            d.setTextSize(2);
            ge = box_text(cx + 5, iy0, ix1 - cx - 5, ih, cx + 6, vy, g + strlen(s_ses->slot[s]), DIM, BG);
        } else d.fillRect(cx + 5, iy0, ix1 - cx - 5, ih, BG);
        (void)ge;
    }
    d.clearClipRect();
    d.setTextSize(1);

    // -- live preview of the assembled question (fills as you type) --
    char q[A_INMAX]; form_build_query(q, sizeof q, true);
    char pv[96]; snprintf(pv, sizeof pv, "%s", q);
    d.setFont(&fonts::Font0); d.setTextSize(1);
    while (pv[0] && (int)d.textWidth(pv) > 224) pv[strlen(pv) - 1] = 0;
    int py = by + bh + 8; if (py > ch - 10) py = ch - 10;
    box_text(0, py, 240, 8, 8, py, pv, MUTED, BG);
}

static void draw_idee(int ch)
{
    if (s_form_leaf >= 0) { draw_idee_form(ch);   return; }
    if (s_idee_cat < 0)   { draw_idee_cats(ch);   return; }
    draw_idee_leaves(ch);
}

// ---- OGGI tab: today's agenda (data cached by cal_refresh) ------------------
static void today_key(int key, char ch)
{
    (void)ch;
    if      (key == NK_UP)   s_mrow = (s_mrow > 0) ? s_mrow - 1 : -1;   // off the top -> tab bar
    else if (key == NK_DOWN) { if (s_mrow < s_today_n - 1) s_mrow++; }
    else return;
    menu_hint(); nucleo_app_request_draw();
}

// One agenda row, framed like an IA settings row: the time/date in an accent colour, the title in big
// size-2 type — the modern agenda look (glanceable time, readable title). The focused event expands to
// two lines (time on top, full-width title below, less truncation); the rest are one compact line. The
// "next upcoming" peek (i >= s_today_count) is drawn dimmer. Splits "prefix  title" on the double space.
static void draw_event_row(int y, bool focus, int i)
{
    const int h = focus ? 46 : 30;
    bool future = (i >= s_today_count);
    const unsigned short bg = row_band(y, h, focus);
    unsigned short tcol = focus ? INK : (future ? DIM : MUTED);                  // title colour
    unsigned short acol = focus ? INK : (future ? DIM : ACC);                    // time/date accent
    const char *sep = strstr(s_ses->today[i], "  ");
    char pre[24] = ""; const char *title = s_ses->today[i];
    if (sep && sep != s_ses->today[i]) { int pl = (int)(sep - s_ses->today[i]); if (pl > 23) pl = 23;
                                    memcpy(pre, s_ses->today[i], pl); pre[pl] = 0; title = sep + 2; }
    char t[64]; snprintf(t, sizeof t, "%s", title);
    if (focus) {                                                                // expanded: time on top, BIG title below
        if (pre[0]) { d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextColor(acol, bg); d.setCursor(16, y + 6); d.print(pre); }
        d.setFont(&fonts::Font0); d.setTextSize(2); d.setTextColor(tcol, bg);
        fit_w(t, 214);                                                          // ".." if longer than the row
        d.setCursor(16, pre[0] ? y + 22 : y + (h - 16) / 2 - 1); d.print(t);
    } else {                                                                    // compact: small time accent + big title
        int x = 16;
        if (pre[0]) { d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextColor(acol, bg);
                      d.setCursor(x, y + (h - 8) / 2); d.print(pre); x += (int)d.textWidth(pre) + 8; }
        d.setFont(&fonts::Font0); d.setTextSize(2); d.setTextColor(tcol, bg);
        fit_w(t, 232 - x);                                                      // ".." if longer than the room left
        d.setCursor(x, y + (h - 16) / 2 - 1); d.print(t);
    }
}

// OGGI tab: today's agenda as a naturally-scrolling list (same look & motion as the IA/IDEE tabs) — the
// focused event enlarges in place and the list scrolls only when it reaches an edge. A bigger accented
// header + a friendly, readable empty state.
static void draw_today(int ch)
{
    const int top = 44, f = s_mrow;
    if (s_ses->mfull) {                                                         // header: static for the scene
        set_font(F_MED); d.setTextColor(ACC, BG);                               // bigger, accented date header
        d.setCursor(8, 24); d.print(s_ses->today_hdr[0] ? s_ses->today_hdr : (s_en ? "Today" : "Oggi"));
        if (s_today_count > 0) { char cb[6]; snprintf(cb, sizeof cb, "%d", s_today_count);
            d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextColor(MUTED, BG);
            d.setCursor(232 - (int)strlen(cb) * 6, 28); d.print(cb); }
        d.drawFastHLine(8, 42, 224, LINE);
    }
    if (s_today_n == 0) {                                                       // friendly, bigger empty state
        if (!s_ses->mfull) return;
        set_font(F_BIG); d.setTextColor(MUTED, BG);
        d.setCursor(10, top + 10); d.print(s_en ? "No events today" : "Niente in agenda oggi");
        d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextColor(DIM, BG);
        d.setCursor(10, top + 36); d.print(s_en ? "Try: remind me ..." : "Prova: ricordami ...");
        return;
    }
    d.setClipRect(0, top, 240, ch - top);
    int yy = list_scroll_y0(top, ch - top, s_today_n, f), k = 0;
    for (int i = 0; i < s_today_n; i++) {
        int hh = (i == f) ? 46 : 30;
        if (yy + hh > top && yy < ch) {
            if (mrow_dirty(k, mix(mix(2166136261u, 0x0D00u + i), ((uint32_t)yy << 1) | (i == f)))) draw_event_row(yy, i == f, i);
            k++;
        }
        yy += hh;
    }
    d.clearClipRect();
    mlist_tail(k, yy, ch);
}

// ---- chat: region painters (direct draw; each self-clears its own box) -------
// Right side of the header: the thinking pulse while busy, otherwise a live clock (the smartwatch
// staple). The last answer's tier/confidence is shown by the bubble rail colour + the STATO tab,
// so the header stays calm and glanceable. Clock hidden until the RTC is past 2023 (pre-NTP epoch).
// Right cluster of the header: the abbreviated date (warm amber) snug to the LEFT of the clock
// (grey), both right-aligned with the cluster edge at x=234. While busy the whole cluster is
// replaced by the "pensa..." pulse. Keeping the date next to the clock (not floating far left)
// and in a distinct colour is the uniformity the launcher header now shares.
// The right field [104,234): "thinking..." while busy; otherwise the answer's page "2/5" (when it overflows
// the body) or the date, then the clock. One opaque Font0 field — a tick or a page flip never blanks it.
static void put_badge(int top)
{
    char a[24] = "", b[8] = ""; unsigned short ca = AMBER;
    if (s_busy) {
        static const char *dots[] = { "", ".", "..", "..." };
        snprintf(a, sizeof a, "%s%s", s_en ? "thinking" : "pensa", dots[s_spin & 3]); ca = GRN;
    } else {
        time_t now = time(NULL); struct tm *tm = localtime(&now);
        const bool clk = tm && now > 1672531200;                // pre-NTP: no clock/date yet
        int p, n;
        if (answer_pos(chat_avail(), &p, &n)) { snprintf(a, sizeof a, "%d/%d", p, n); ca = ACC; }
        else if (clk) snprintf(a, sizeof a, "%s %d %s", (s_en ? WD3_EN : WD3_IT)[tm->tm_wday], tm->tm_mday,
                               (s_en ? MO3_EN : MO3_IT)[tm->tm_mon]);
        if (clk) snprintf(b, sizeof b, "%02d:%02d", tm->tm_hour, tm->tm_min);
    }
    seg_field(104, 234, top + 4, true, a, ca, b, MUTED);
}
static void draw_badge(int top) { put_badge(top); }

static void draw_header(int top)
{
    d.setFont(&fonts::Font0); d.setTextSize(1);
    d.setTextColor(ACC, BG); d.setCursor(8, top + 4); d.print("ANIMA");
    char ml[8]; snprintf(ml, sizeof ml, "%-7s", s_omode == OM_OFF ? "offline" : s_omode == OM_ONLY ? "online" : (s_en ? "hybrid" : "ibrido"));
    d.setTextColor(s_omode == OM_OFF ? DIM : s_omode == OM_ONLY ? GRN : ACC, BG);
    d.setCursor(60, top + 4); d.print(ml);                      // fixed 7-cell field: a shorter label erases the longer
    put_badge(top);                                             // page / date + clock, right-aligned cluster
    d.drawFastHLine(0, top + 15, 240, LINE);
}

static void draw_deck(int ty0, int avail)
{
    // Glance header: a time-of-day greeting (left) + clock (right) — like a watch's top card — then
    // the starter prompts. fn+;/. pick, Invio (or 1-9) sends, so the first ask needs no typing.
    // Painted in place (opaque text + leftovers): moving the pick repaints the rows, never a blank body.
    time_t now = time(NULL); struct tm *tm = localtime(&now);
    int hr = tm ? tm->tm_hour : 9;
    const char *greet = s_en ? (hr < 12 ? "Good morning" : hr < 18 ? "Good afternoon" : "Good evening")
                             : (hr < 12 ? "Buongiorno"   : hr < 18 ? "Buon pomeriggio" : "Buonasera");
    char hm[8] = "";
    if (tm && now > 1672531200) snprintf(hm, sizeof hm, "%02d:%02d", tm->tm_hour, tm->tm_min);
    d.setFont(&fonts::Font2); d.setTextSize(1);
    box_text(0, ty0, 196, 16, 8, ty0, greet, ACC, BG);
    d.setFont(&fonts::Font0);
    box_text(196, ty0, 44, 16, 232 - (int)strlen(hm) * 6, ty0 + 4, hm, MUTED, BG);
    // Glance line: the next reminder if there's one, otherwise a hint that TAB opens the full skill
    // catalog (so first-timers discover the IDEE tree beyond these quick picks). One dim Font0 line.
    bool reminder = s_ses->complics[0] != 0;
    const char *glance = reminder ? s_ses->complics : (s_en ? "TAB: full skill catalog" : "TAB: catalogo completo");
    box_text(0, ty0 + 16, 240, 12, 8, ty0 + 16, glance, reminder ? MUTED : DIM, BG);
    d.setFont(&fonts::Font2); d.setTextSize(1);
    if (s_sug_sel < 0) s_sug_sel = 0;
    if (s_sug_sel >= SUG_N) s_sug_sel = SUG_N - 1;
    const int rh = 16, top_y = ty0 + 28, end = ty0 + avail;
    int maxvis = (end - top_y) / rh; if (maxvis < 1) maxvis = 1;   // scroll so the selection stays on screen
    int first = (s_sug_sel >= maxvis) ? s_sug_sel - maxvis + 1 : 0;
    int yy = top_y;
    for (int i = first; i < SUG_N && i < first + maxvis; i++, yy += rh) {
        char line[48]; snprintf(line, sizeof line, "%s", cur_sug(i)); fit_w(line, 212);
        if (i == s_sug_sel) {                                     // the kit's selection look: accent pill, INK text
            pill_band(4, yy, 228, rh, 6, ACC, 235);
            d.setTextColor(INK, ACC); d.setCursor(12, yy); d.print(line);
        } else box_text(0, yy, 235, rh, 12, yy, line, FG, BG);
    }
    if (yy < end) d.fillRect(0, yy, 235, end - yy, BG);
    int shown = (SUG_N - first < maxvis) ? SUG_N - first : maxvis;
    draw_vscroll(top_y, maxvis * rh, SUG_N, first, shown);        // scroll cue when the deck overflows
}

// One transcript row, painted IN PLACE over the band [0,235) x [y, y+height): the 3 px message gap (dropped
// for the view's top row), the rail of an answer, then the opaque text via box_text — no clear under it, so
// a scroll, a page flip or a typewriter frame never shows a blank body.
static int render_row(int y, const Row *r, bool at_top)
{
    set_font(r->font);
    const int gap = ((r->first & RF_FIRST) && !at_top) ? 3 : 0, base = font_h(r->font), ty = y + gap;
    if (gap) d.fillRect(0, y, 235, gap, BG);
    char t[216]; int n = r->len; if (n > 215) n = 215; memcpy(t, r->p, n); t[n] = 0;
    if (r->first & RF_TAGROW) {                              // the answer's source tag on a row of its own
        const int tw = tag_w(t), tgx = 232 - tw;
        d.fillRect(0, ty, tgx, base, BG);
        d.fillRect(tgx, ty, tw, 1, BG); d.fillRect(tgx, ty + 9, tw, base - 9, BG);
        d.fillRect(tgx + tw, ty, 235 - tgx - tw, base, BG);
        draw_tag(tgx, ty + 1, t, pal(r->col));
        return gap + base;
    }
    int x0 = 0, tx = 8;
    if (r->role == R_USER) { tx = 232 - (int)d.textWidth(t); if (tx < 22) tx = 22; }
    else if (r->role == R_ANIMA) {                           // the message's left rail (violet / amber)
        d.fillRect(0, ty, 3, base, BG);
        d.fillRect(3, ty, 3, base - 2, pal(r->accent));
        d.fillRect(3, ty + base - 2, 3, 2, BG);
        x0 = 6; tx = 11;
    }
    if (r->first & RF_TAG) {                                 // text, then the tag right-aligned on the same row
        const char *tg = s_msg[r->mi].tag;
        const int tw = tag_w(tg), tgx = 232 - tw, gy = ty + base - 12;
        box_text(x0, ty, tgx - 4 - x0, base, tx, ty, t, pal(r->col), BG);
        d.fillRect(tgx - 4, ty, 239 - tgx, gy - ty, BG);
        d.fillRect(tgx - 4, gy + 8, 239 - tgx, ty + base - gy - 8, BG);
        d.fillRect(tgx - 4, gy, 4, 8, BG); d.fillRect(tgx + tw, gy, 235 - tgx - tw, 8, BG);
        draw_tag(tgx, gy, tg, DIM);
        return gap + base;
    }
    box_text(x0, ty, 235 - x0, base, tx, ty, t, pal(r->col), BG);
    return gap + base;
}
// Paint rows from `t` into [ty0, ty0+avail) and clear only what is left below the last one.
static void paint_rows(int ty0, int avail, int t)
{
    int y = ty0; const int end = ty0 + avail;
    d.setClipRect(0, ty0, 235, avail);                       // a partial last row never bleeds onto the input edge
    for (int i = t; i < s_rown && y < end; i++) y += render_row(y, &s_row[i], i == t);
    d.clearClipRect();
    if (y < end) d.fillRect(0, y, 235, end - y, BG);
    draw_vscroll(ty0, avail, s_rown, t, last_fit(t, avail) - t + 1);
}

static void draw_body(int top, int h)
{
    const int ty0 = top + 18, avail = h - input_h() - 1 - 18;
    const signed char kind = deck_active() ? BK_DECK : BK_CHAT;
    if (s_ses->body_kind != kind) {                          // deck <-> transcript: a scene change, one clear
        if (s_ses->body_kind != BK_NONE) d.fillRect(0, ty0, 240, avail, BG);
        s_ses->body_kind = kind;
    }
    if (kind == BK_DECK) { draw_deck(ty0, avail); return; }
    if (s_rown <= 0) { d.fillRect(0, ty0, 240, avail, BG); return; }
    paint_rows(ty0, avail, view_top(avail));
}

// ---- the full-screen reader ---------------------------------------------------------------------------
// Enter on an empty line (or when an answer needs it) opens it: header, input AND the footer go away
// (fullscreen), the transcript gets RD_BODY px (~7 big rows instead of ~4) and a 9 px status strip at the
// foot names where you are ("2/5"), the answer's source and the keys. ; . (and fn+; fn+., ',' '/', space)
// flip PAGES; Enter / Esc / DEL close it; any letter closes it and starts the next question.
static void reader_status(void)
{
    char a[24] = ""; int p, n;
    if (answer_pos(RD_BODY, &p, &n)) snprintf(a, sizeof a, "%d/%d", p, n);
    d.fillRect(0, RD_BODY, 240, 1, BG);
    seg_field(0, 120, RD_BODY + 1, false, a, ACC, "", MUTED);
    seg_field(120, 240, RD_BODY + 1, true, s_en ? ";/. page  esc" : ";/. pagina  esc", DIM, "", DIM);
}
static void draw_reader(void)
{
    if (s_rown <= 0) { d.fillRect(0, 0, 240, RD_BODY, BG); reader_status(); return; }
    paint_rows(0, RD_BODY, view_top(RD_BODY));
    reader_status();
}
static const char *chat_hint(void);
static void reader_open(void)
{
    if (!s_ses || s_rown <= 0 || s_ses->reader) return;
    s_ses->reader = true;
    nucleo_app_set_fullscreen(true);                          // reclaim the footer rows too: every pixel for the text
    mark_all_dirty(); nucleo_app_request_draw();
}
static void reader_close(void)
{
    if (!s_ses || !s_ses->reader) return;
    s_ses->reader = false;
    nucleo_app_set_fullscreen(false);                         // the framework repaints the footer
    nucleo_app_set_hint(chat_hint());
    mark_all_dirty(); nucleo_app_request_draw();
}

// Text start x of the input row: right after the ">" prompt with a fixed gap, measured in the live
// chat font (the old hard-coded 22 didn't track the proportional prompt width). The chat font must be
// set before calling. Single source so placeholder, text, caret and ghost all line up.
static const int PROMPT_X = 8;
static int input_x0(void) { return PROMPT_X + (int)d.textWidth(">") + (s_big ? 14 : 6); }

// The input row, painted in place: every keystroke overwrites the line (opaque glyphs) and clears only what
// is left to its right — the row never blanks while you type. Busy: the dot wave (its own small box is the
// one thing that clears, it animates anyway) and, when the turn may reach the cloud, how long it can take.
static void draw_input(int top, int h)
{
    const int inH = input_h(), in_top = top + h - inH, ty = in_top + 4, bot = in_top + inH;
    d.drawFastHLine(0, in_top, 240, LINE);
    set_font(chat_font());
    const int fh = (int)d.fontHeight();
    d.fillRect(0, in_top + 1, 240, ty - in_top - 1, BG);   // the margins around the glyph band (BG over BG: invisible)
    if (ty + fh < bot) d.fillRect(0, ty + fh, 240, bot - ty - fh, BG);
    if (s_busy && s_ilen == 0) {                            // pensa: onda di 4 pallini, il "picco" luminoso scorre
        const int cy = in_top + inH / 2, act = (int)(s_spin & 3);
        d.fillRect(0, ty, 72, fh, BG);
        for (int i = 0; i < 4; i++) {
            int dist = i - act; if (dist < 0) dist = -dist;
            int r = dist == 0 ? 4 : dist == 1 ? 3 : 2;
            unsigned short col = dist == 0 ? GRN : dist == 1 ? ACC : DIM;
            d.fillCircle(PROMPT_X + 8 + i * 14, cy, r, col);
        }
        const char *note = s_ses->cloud_note ? (s_en ? "cloud: up to 12 s" : "cloud: max 12 s") : "";
        set_font(F_MED);
        box_text(72, ty, 168, fh, 76, ty + (fh - (int)d.fontHeight()) / 2, note, MUTED, BG);
        return;
    }
    const int x0 = input_x0(), availw = 232 - x0;
    const int pe = box_text(0, ty, 8 + (int)d.textWidth(">"), fh, PROMPT_X, ty, ">", ACC, BG);
    if (x0 > pe) d.fillRect(pe, ty, x0 - pe, fh, BG);
    if (s_ilen == 0) {                                       // empty -> a dim placeholder cue (smartwatch style)
        box_text(x0, ty, 240 - x0, fh, x0, ty, s_awaiting ? (s_en ? "reply..." : "rispondi...") : (s_en ? "type..." : "scrivi..."), DIM, BG);
        return;
    }
    int startc = 0; while (s_ses->input[startc] && (int)d.textWidth(s_ses->input + startc) > availw) startc++;   // scroll to keep the caret visible
    const char *vis = s_ses->input + startc;
    const int te = x0 + (int)d.textWidth(vis);
    d.setTextColor(FG, BG); d.setCursor(x0, ty); d.print(vis);
    // Ghost completion: the dim tail of the best match, drawn after the caret. fn+/ accepts it.
    char ghost[HIST_LEN];
    if (te + 3 < 230 && autocomplete(s_ses->input, ghost, sizeof ghost)) box_text(te, ty, 240 - te, fh, te + 3, ty, ghost + s_ilen, DIM, BG);
    else if (te < 240) d.fillRect(te, ty, 240 - te, fh, BG);
}

// The caret is a thin bar after the visible input. Toggling just this cell lets the blink animate
// without touching anything else (no flicker). Hidden on the deck and while busy.
static void draw_caret(int top, int h)
{
    if (deck_active() || s_menu_open || s_busy) return;     // busy: la riga input mostra i pallini -> il caret non li tocca
    int inH = input_h(), in_top = top + h - inH, ty = in_top + 4;
    set_font(chat_font());
    const int x0 = input_x0(), availw = 232 - x0;
    int startc = 0; while (s_ses->input[startc] && (int)d.textWidth(s_ses->input + startc) > availw) startc++;
    int cx = x0 + (int)d.textWidth(s_ses->input + startc);
    int ch = s_big ? 16 : 13;
    bool show = (s_blink & 2);
    d.fillRect(cx + 1, ty, 2, ch, show ? GRN : BG);
}

// The tabbed menu. A SCENE change (menu opened, tab paged, IDEE level/form entered, language, an overlay
// painted over us) costs one clear + the tab strip + the static parts; inside a scene a key repaints only
// what moved (the list rows whose content changed, the form's value, the GUIDA card on a page flip).
static void draw_menu(int ch)
{
    AnimaSession *S = s_ses;
    const int scene = s_tab | ((s_idee_cat + 1) << 4) | ((s_form_leaf + 1) << 8) | (s_en ? 1 << 16 : 0);
    S->mfull = (S->mscene != scene);
    if (S->mfull) {
        S->mscene = scene;
        d.fillRect(0, 0, 240, ch, BG);
        draw_tabbar(s_tab);
        memset(S->msig, 0, sizeof S->msig); S->mleft = -1; S->mpage = -1;
    }
    switch (s_tab) {
        case TAB_IDEE:  draw_idee(ch);  break;
        case TAB_OGGI:  draw_today(ch); break;
        case TAB_GUIDA: draw_guide(ch); break;
        case TAB_IA:    draw_ia(ch);    break;
        case TAB_STATO: if (S->mfull) draw_stato(ch); break;   // a read-only readout: painted per scene
    }
}

// Full-screen text editor: a path title bar + the word-wrapped buffer (auto-scrolled so the caret/end
// stays visible) + a blinking caret. Direct-draw, repaints the whole content area each call (cheap: the
// buffer is <=1 KB). The bottom hint line is the framework's (set in editor_open).
static void draw_editor(int top, int h)
{
    AnimaEditor *E = s_ed;
    const int fh = font_h(F_MED);
    if (!E->full && !E->dirty) {                         // the blink: toggle ONLY the caret bar
        d.fillRect(E->cx, E->cy, 2, fh, (s_blink & 2) ? ACC : BG);
        return;
    }
    set_font(F_MED);
    if (E->full) {                                       // scene: clear once + the title bar with the path
        d.fillRect(0, top, 240, h, BG);
        d.fillRect(0, top, 240, 17, LINE);
        char pth[80]; snprintf(pth, sizeof pth, "%s", E->path[0] ? E->path : "(file)"); fit_w(pth, 176);
        d.setTextColor(FG, LINE); d.setCursor(4, top + 1); d.print(pth);
    }
    // Title bar: the live char count on the right, an opaque field (no clear).
    char cc[16]; snprintf(cc, sizeof cc, "%d", s_ed_len);
    box_text(184, top, 56, 17, 238 - (int)d.textWidth(cc), top + 1, cc, ACC, LINE);
    E->full = E->dirty = false;

    // Word-wrap the buffer into line segments [off,len), honoring '\n' and hard-splitting over-wide words.
    // The segment arrays live in the editor block (heap while the editor is open; were static .bss).
    const int availw = 232, LCAP = ED_LCAP, lh = font_h(F_MED) + 1;
    short *loff = s_ed->loff, *llen = s_ed->llen; int nl = 0;
    int ls = 0, i = 0, n = s_ed_len;
    while (i <= n && nl < LCAP) {
        if (i == n || s_ed->buf[i] == '\n') {
            if (ls == i) { loff[nl] = (short)ls; llen[nl] = 0; nl++; }     // blank line
            else {
                int seg = ls;
                while (seg < i && nl < LCAP) {
                    int take = i - seg;
                    while (take > 1 && meas(s_ed->buf + seg, take) > availw) take--;
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
    // Each visible line painted in place (opaque text + its leftover), then what is left below the last one.
    int y = by;
    E->cx = 5; E->cy = (short)by;
    for (int r = first; r < nl; r++) {
        char tmp[80]; int L = llen[r]; if (L > 79) L = 79;
        memcpy(tmp, s_ed->buf + loff[r], L); tmp[L] = 0;
        int te = box_text(0, y, 240, lh, 4, y, tmp, FG, BG);
        if (r == nl - 1) { E->cx = (short)(te + 1); E->cy = (short)y; }
        y += lh;
    }
    if (y < top + h) d.fillRect(0, y, 240, top + h - y, BG);
    d.fillRect(E->cx, E->cy, 2, fh, (s_blink & 2) ? ACC : BG);   // the caret at the end of the text
}

// Modale di conferma uscita: pannello a tutto schermo, font grandi ben visibili (REGOLA UI nativa).
// With the editor open the same modal asks "discard the typed text?" (Esc in the editor with text).
static void draw_exit_modal(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    const bool ed = s_ed_open;
    d.fillRect(0, top, 240, h, BG);
    int bw = 212, bh = 92, bx = (240 - bw) / 2, by = top + (h - bh) / 2;
    d.fillRoundRect(bx, by, bw, bh, 10, BG);
    d.drawRoundRect(bx, by, bw, bh, 10, ACC);
    d.drawRoundRect(bx + 1, by + 1, bw - 2, bh - 2, 9, ACC);          // doppio bordo = piu' marcato
    set_font(F_BIG); d.setTextColor(FG, BG);
    const char *q = ed ? (s_en ? "Discard the text?" : "Scartare il testo?") : (s_en ? "Leave ANIMA?" : "Uscire da ANIMA?");
    d.setCursor(120 - (int)d.textWidth(q) / 2, by + 14); d.print(q);
    set_font(F_MED);
    const char *yes = ed ? (s_en ? "Enter = Discard" : "Invio = Scarta") : (s_en ? "Enter = Exit" : "Invio = Esci");
    d.setTextColor(ed ? AMBER : GRN, BG); d.setCursor(120 - (int)d.textWidth(yes) / 2, by + 44); d.print(yes);
    const char *no = ed ? (s_en ? "Esc = Keep writing" : "Esc = Continua") : (s_en ? "Esc = Stay" : "Esc = Resta");
    d.setTextColor(MUTED, BG); d.setCursor(120 - (int)d.textWidth(no) / 2, by + 66); d.print(no);
    d.setFont(&fonts::Font0); d.setTextSize(1);
}

static void draw(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    if (!s_ses) {                                           // session block failed to allocate (see enter)
        d.fillRect(0, top, 240, h, BG);
        set_font(F_MED); d.setTextColor(AMBER, BG);
        d.setCursor(8, top + 20); d.print(s_en ? "ANIMA: out of memory." : "ANIMA: memoria esaurita.");
        d.setTextColor(MUTED, BG);
        d.setCursor(8, top + 44); d.print(s_en ? "Esc returns to the OS." : "Esc torna all'OS.");
        d.setFont(&fonts::Font0); d.setTextSize(1);
        return;
    }
    // An overlay (voice, notification banner, Control Center) painted over us, or a force_repaint(): what the
    // in-place painters believe is on the panel is stale -> one full repaint (ANTI-FLICKER.md, repaint_gen).
    const unsigned gen = nucleo_app_repaint_gen();
    if (gen != s_ses->rgen) { s_ses->rgen = gen; mark_all_dirty(); }
    if (s_exit_confirm) { draw_exit_modal(); return; }      // modale uscita sopra a tutto
    if (s_ses->clear_confirm) {                             // the kit's confirm card over the current scene
        if (s_d_clear) { d.fillRect(0, top, 240, h, BG); s_d_clear = false; }   // an overlay painted over: clean backdrop
        d.setFont(&fonts::Font0); d.setTextSize(1);
        app_ui_confirm(s_en ? "Clear the chat?" : "Pulire la chat?",
                       s_en ? "Deletes the saved conversation" : "Cancella la conversazione salvata", s_ses->clear_yes);
        d.setFont(&fonts::Font0); d.setTextSize(1);
        return;
    }
    // Safety net: if the framework ever hands us its (freshly cleared) off-screen canvas instead of
    // the direct path, repaint everything so nothing is left blank (the in-place painters assume the panel).
    if (nucleo_app_is_buffered()) mark_all_dirty();
    if (s_d_clear) {                                        // a scene change reaches whichever view paints next
        if (s_ed) s_ed->full = true;
        s_ses->mscene = -1;
    }
    if (s_ed_open || s_menu_open) s_d_clear = false;        // (they do their own one clear per scene)
    if (s_ed_open)   { draw_editor(top, h); d.setFont(&fonts::Font0); d.setTextSize(1); return; }
    if (s_menu_open) { draw_menu(h); d.setFont(&fonts::Font0); d.setTextSize(1); return; }

    if (s_ses->reader) {                                    // full-screen reader (fullscreen: h = the whole panel)
        if (s_d_clear) d.fillRect(0, 0, 240, h, BG);
        draw_reader();
        s_d_hdr = s_d_body = s_d_input = s_d_badge = s_d_clear = false;
        d.setFont(&fonts::Font0); d.setTextSize(1);
        return;
    }
    if (s_d_clear) { d.fillRect(0, top, 240, h, BG); s_ses->body_kind = BK_NONE; }   // scene change: the one clear
    if (s_d_body) s_d_badge = true;                         // the header's page indicator follows the body

    if (s_d_body)       draw_body(top, h);
    if (s_d_hdr)        draw_header(top);
    else if (s_d_badge) draw_badge(top);
    if (s_d_input)      draw_input(top, h);
    draw_caret(top, h);

    s_d_hdr = s_d_body = s_d_input = s_d_badge = s_d_clear = false;
    d.setFont(&fonts::Font0); d.setTextSize(1);    // leave the global font at the framework default
}

extern "C" void nucleo_register_anima(void)
{
    static const nucleo_app_def_t app = {
        "anima", "ANIMA", "Tools", "Offline assistant: ask in plain words",
        'a', ACC, enter, on_key, tick, draw, leave
    };
    nucleo_app_register(&app);
}
