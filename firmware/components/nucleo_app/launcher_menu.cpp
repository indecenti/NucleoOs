// Launcher menu model + navigation. See launcher_menu.h.
#include "launcher_menu.h"
#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "nucleo_board.h"       // NUCLEO_CFG_MOUNT for the pinned-apps store
#include "esp_attr.h"           // RTC_NOINIT_ATTR: the return-cursor survives the Solo warm reboot
#include "esp_system.h"         // esp_reset_reason(): only a warm reboot is a real return-from-Solo
#include <string.h>
#include <ctype.h>
#include <stdio.h>

// Registered foreground apps, owned by nucleo_app.cpp; read here to build the tree.
int                       nucleo_app_count(void);
const nucleo_app_def_t   *nucleo_app_at(int i);

// s_dyn_apps[MAX_APPS] + s_cat_pool + s_apps[MAX_APPS] (nucleo_app.cpp) are ALWAYS-resident .bss on a
// no-PSRAM heap (~16.9 KB free measured). The per-category lists share ONE flat pointer pool (each
// category a contiguous NULL-terminated run) instead of a MAX_CATS x (MAX_APPS+1) matrix: 80 slots here
// cost ~1.1 KB LESS .bss than the old 64-slot matrix did.
#define MAX_APPS NUCLEO_APP_MAX
#define MAX_CATS 10
#define MAX_DEPTH 6
#define MAX_PIN  6            // apps pinned to Home. Store = MAX_PIN*24 = 144 B .bss (owns the id strings)
#define MAX_REC  5            // "Recent" tile: the last apps opened. Store = MAX_REC*24 = 120 B .bss

// ---- dynamically built tree -------------------------------------------------
static MenuNode        s_dyn_cats[MAX_CATS];
static MenuNode        s_dyn_apps[MAX_APPS];
static const MenuNode *s_cat_pool[MAX_APPS + MAX_CATS];         // every category's items, one run each
static const MenuNode *s_root_items[MAX_CATS + MAX_PIN + 3];   // ANIMA + pins + Recent + categories, NULL-terminated
static MenuNode        ROOT;
static int             s_cat_count = 0;
static int             s_app_n     = 0;   // number of app nodes in s_dyn_apps (for the flat "Spotlight" search)
static unsigned        s_tree_gen  = 0;   // bumped on every rebuild: invalidates the Spotlight rank cache

// ---- pinned-to-Home store ---------------------------------------------------
// App ids the user pinned to the top of Home (after ANIMA), persisted to LittleFS so they survive a
// reboot. Owns its own copies of the id strings (not app-def pointers), so a load before the apps are
// registered is still safe. Newest pin last. Zero heap: a fixed .bss array.
static char s_pin_buf[MAX_PIN][24];
static int  s_pin_n = 0;

#define PIN_STORE_PATH  NUCLEO_CFG_MOUNT "/config/pins.txt"
#define REC_STORE_PATH  NUCLEO_CFG_MOUNT "/config/recents.txt"

// One id per line; pins and recents share the format (and these two helpers).
static int ids_load(const char *path, char (*buf)[24], int cap)
{
    int n = 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[24];
    while (n < cap && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0]) snprintf(buf[n++], sizeof buf[0], "%s", line);
    }
    fclose(f);
    return n;
}
static void ids_save(const char *path, char (*buf)[24], int n)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < n; i++) fprintf(f, "%s\n", buf[i]);
    fclose(f);
}
static void pins_load(void) { s_pin_n = ids_load(PIN_STORE_PATH, s_pin_buf, MAX_PIN); }
static void pins_save(void) { ids_save(PIN_STORE_PATH, s_pin_buf, s_pin_n); }

// ---- "Recent" tile ----------------------------------------------------------
// The last MAX_REC apps opened, newest first, as ONE fixed Home tile right after the pins (so the
// categories never shift under the thumb). Same zero-heap id store as the pins; written only when the
// order actually changes, so re-opening the newest app costs no flash write.
static char            s_rec_buf[MAX_REC][24];
static int             s_rec_n = 0;
static const MenuNode *s_rec_items[MAX_REC + 1];
static MenuNode        REC;

// ---- navigation state -------------------------------------------------------
struct Frame { const MenuNode *node; int sel; char filter[16]; };
static Frame s_stack[MAX_DEPTH];
static int   s_top = 0;

static Frame &top() { return s_stack[s_top]; }

static int get_or_create_cat(const char *name)
{
    if (!name) name = "Apps";
    for (int i = 0; i < s_cat_count; i++)
        if (!strcmp(s_dyn_cats[i].id, name)) return i;
    if (s_cat_count < MAX_CATS) {
        int i = s_cat_count++;
        s_dyn_cats[i].id = name;
        s_dyn_cats[i].label = name;
        s_dyn_cats[i].icon = (char)toupper((unsigned char)name[0]);
        unsigned short c = C_BLUE;
        if      (!strcmp(name, "Media"))   c = C_PINK;
        else if (!strcmp(name, "Office"))  c = C_BLUE;
        else if (!strcmp(name, "Tools"))   c = C_YELLOW;
        else if (!strcmp(name, "System"))  c = C_GREY;
        else if (!strcmp(name, "Web OS"))  { c = C_PURPLE; s_dyn_cats[i].icon = 'W'; }  // the browser web OS: cable (usb) or LAN (remote)
        else if (!strcmp(name, "Connect")) c = C_PURPLE;
        else if (!strcmp(name, "Messaging")) { c = C_BLUE; s_dyn_cats[i].icon = '@'; }  // was "Communication"; '@' distinct from Connect's 'C'
        else if (!strcmp(name, "Security")) c = C_RED;
        else if (!strcmp(name, "Measure")) c = C_GREEN;  // was "Hardware": IMU measuring instruments, not device settings
        else if (!strcmp(name, "Games"))   c = C_RED;
        s_dyn_cats[i].color = c;
        s_dyn_cats[i].kind = N_MENU;
        s_dyn_cats[i].desc = "";
        s_dyn_cats[i].items = nullptr;           // pointed into s_cat_pool once every app is counted
        return i;
    }
    return 0;
}

void launcher_build_menu(void)
{
    s_tree_gen++;
    s_cat_count = 0;
    const MenuNode *anima_node = nullptr;        // hoisted straight onto Home, not buried in a category
    int n = nucleo_app_count();
    if (n > MAX_APPS) n = MAX_APPS;
    signed char cat_of[MAX_APPS];                // category index per app (-1 = ANIMA, not in a category)
    int cat_n[MAX_CATS] = { 0 };
    for (int i = 0; i < n; i++) {
        const nucleo_app_def_t *a = nucleo_app_at(i);
        s_dyn_apps[i].id = a->id;
        s_dyn_apps[i].label = a->name;
        s_dyn_apps[i].icon = a->icon;
        s_dyn_apps[i].color = a->color;
        s_dyn_apps[i].kind = N_APP;
        s_dyn_apps[i].desc = a->desc ? a->desc : "";
        s_dyn_apps[i].items = nullptr;

        if (!strcmp(a->id, "anima")) { anima_node = &s_dyn_apps[i]; cat_of[i] = -1; continue; }  // ANIMA -> top-level Home entry
        int c = get_or_create_cat(a->category);
        cat_of[i] = (signed char)c;
        cat_n[c]++;
    }
    // Carve the pool: category c owns cat_n[c] slots + its NULL terminator, in category order; apps keep
    // registration order inside their category. Total <= n + s_cat_count <= MAX_APPS + MAX_CATS.
    int fill[MAX_CATS];
    for (int c = 0, off = 0; c < s_cat_count; c++) {
        s_dyn_cats[c].items = &s_cat_pool[off];
        fill[c] = off;
        off += cat_n[c];
        s_cat_pool[off++] = nullptr;
    }
    for (int i = 0; i < n; i++)
        if (cat_of[i] >= 0) s_cat_pool[fill[(int)cat_of[i]]++] = &s_dyn_apps[i];
    s_app_n = n;                                                // every app node lives in s_dyn_apps[0..n) for Spotlight
    pins_load();                                                // refresh the pin set from the store before assembling Home
    s_rec_n = ids_load(REC_STORE_PATH, s_rec_buf, MAX_REC);
    int rn = 0;
    for (int k = 0; k < s_rec_n; k++)
        for (int i = 0; i < n; i++)
            if (!strcmp(s_dyn_apps[i].id, s_rec_buf[k]) && strcmp(s_rec_buf[k], "anima")) { s_rec_items[rn++] = &s_dyn_apps[i]; break; }
    s_rec_items[rn] = nullptr;
    REC.id = LAUNCHER_RECENT_ID; REC.label = "Recent"; REC.icon = 'R'; REC.color = C_GREEN;
    REC.kind = N_MENU; REC.desc = ""; REC.items = s_rec_items;
    int r = 0;
    if (anima_node) s_root_items[r++] = anima_node;            // ANIMA leads Home, above the categories
    // Pinned apps ride at the top of Home (after ANIMA), in pin order. Each still lives in its own
    // category too (Library model), so a pin adds a shortcut without moving the app.
    for (int p = 0; p < s_pin_n && r < MAX_CATS + MAX_PIN + 1; p++) {
        if (!strcmp(s_pin_buf[p], "anima")) continue;          // already hoisted — never double it
        for (int i = 0; i < n; i++)
            if (!strcmp(s_dyn_apps[i].id, s_pin_buf[p])) { s_root_items[r++] = &s_dyn_apps[i]; break; }
    }
    if (rn > 0) s_root_items[r++] = &REC;                      // one fixed tile: the categories keep their slots
    for (int i = 0; i < s_cat_count && r < MAX_CATS + MAX_PIN + 2; i++) s_root_items[r++] = &s_dyn_cats[i];
    s_root_items[r] = nullptr;

    ROOT.id = "home";
    ROOT.label = "Home";
    ROOT.icon = 'H';
    ROOT.color = C_BLUE;
    ROOT.kind = N_MENU;
    ROOT.desc = "NucleoOS home";
    ROOT.items = s_root_items;
}

void launcher_reset(void)
{
    s_stack[0].node = &ROOT;
    s_stack[0].sel = 0;
    s_stack[0].filter[0] = 0;
    s_top = 0;
}

// ---- return-cursor across a Solo warm reboot --------------------------------
// A NX_SOLO app (game / Music / Video / Radio) opens by warm-rebooting into a fresh heap and Esc
// warm-reboots back to the full OS — a brand-new boot, so launcher_reset() lands on Home/ANIMA and
// the user loses their place (and their type-to-search context). We snapshot the launch frame into
// RTC (survives the warm reboot, garbage after a real power-loss) right before the Solo reboot, and
// re-apply it once we come back. Restores the exact frame: Home vs a category, the type-to-filter
// string, and the focused row — so returning drops you back where you launched from.
#define RET_MAGIC 0x52544E43u   // 'RTNC'
struct RetCtx { uint32_t magic; int depth; int sel0; char filt0[16]; char cat_id[24]; int sel1; char filt1[16]; };
RTC_NOINIT_ATTR static RetCtx s_ret;
static void nav_push(const MenuNode *node);   // defined below; apply_return re-enters a category with it
static void clamp_sel(void);                  // defined below; keeps the focused row inside the visible list

void launcher_capture_return(void)
{
    s_ret.magic = RET_MAGIC;
    s_ret.depth = (s_top >= 1) ? 1 : 0;             // Home (0) or one category deep (1) — the only launch frames
    s_ret.sel0  = s_stack[0].sel;
    snprintf(s_ret.filt0, sizeof s_ret.filt0, "%s", s_stack[0].filter);
    s_ret.cat_id[0] = 0; s_ret.sel1 = 0; s_ret.filt1[0] = 0;
    if (s_ret.depth == 1 && s_stack[1].node && s_stack[1].node->id) {
        snprintf(s_ret.cat_id, sizeof s_ret.cat_id, "%s", s_stack[1].node->id);
        s_ret.sel1 = s_stack[1].sel;
        snprintf(s_ret.filt1, sizeof s_ret.filt1, "%s", s_stack[1].filter);
    }
}

// Re-apply the captured frame after the return boot. Caller runs this AFTER launcher_build_menu() +
// launcher_reset(), and ONLY when this boot is the full OS (not the Solo boot itself). Consumes the
// snapshot once. Gated on a warm reset so a cold power-on with garbage RTC can't teleport the cursor.
void launcher_apply_return(void)
{
    if (s_ret.magic != RET_MAGIC) return;
    s_ret.magic = 0;                                 // consume once, even if we bail below
    if (esp_reset_reason() != ESP_RST_SW) return;    // only our own esp_restart() (Solo) — never a crash/power-on

    s_top = 0;
    s_stack[0].node = &ROOT;
    s_stack[0].sel  = s_ret.sel0;
    snprintf(s_stack[0].filter, sizeof s_stack[0].filter, "%s", s_ret.filt0);

    if (s_ret.depth == 1 && s_ret.cat_id[0]) {       // re-enter the same category by id (pointers didn't survive)
        for (const MenuNode *const *it = ROOT.items; it && *it; ++it) {
            if ((*it)->kind == N_MENU && (*it)->id && !strcmp((*it)->id, s_ret.cat_id)) {
                nav_push(*it);                        // sets sel=0/filter="" — override with the snapshot below
                snprintf(top().filter, sizeof top().filter, "%s", s_ret.filt1);
                top().sel = s_ret.sel1;
                break;
            }
        }
    }
    clamp_sel();                                     // the app set is identical across the reboot, but be safe
}

// ---- filtering + visible-row queries ---------------------------------------
extern "C" const char *launcher_app_title(const char *id, int lang);   // launcher_render.cpp (5-language titles)
extern "C" int nucleo_settings_search_count(const char *q);             // app_wifi.cpp: settings a query finds

// Spotlight also reaches INSIDE Settings: when the query matches settings rows, one extra result (after
// every app) opens Settings already searching for it. Its title says how many (launcher_render.cpp).
static MenuNode SETQ = { LAUNCHER_SETTINGS_SEARCH_ID, "Settings", 'W', C_BLUE, N_APP, "", nullptr };
static char s_setq_for[16];                    // query the cached count belongs to
static int  s_setq_n = 0;
static int settings_hits(const char *q)
{
    if (strcmp(q, s_setq_for)) { snprintf(s_setq_for, sizeof s_setq_for, "%s", q); s_setq_n = nucleo_settings_search_count(q); }
    return s_setq_n;
}

// How well `text` matches the lower-case query `q`: 0 = not at all, 1 = the text starts with it,
// 2 = one of its words does ("pc" in "USB keyboard for a PC"), 3 = somewhere inside a word.
static int text_score(const char *text, const char *q)
{
    char a[40];
    int i = 0; for (; text[i] && i < 39; i++) a[i] = (char)tolower((unsigned char)text[i]); a[i] = 0;
    const char *hit = strstr(a, q);
    if (!hit) return 0;
    if (hit == a) return 1;
    for (const char *p = hit; p; p = strstr(p + 1, q))
        if (!isalnum((unsigned char)p[-1])) return 2;
    return 3;
}

// Best (lowest non-zero) score of a node against the query: its registered label and, for apps, its
// title in all five languages ("wetter", "meteo" and "weather" all find Weather). 0 = no match.
static int match_score(const MenuNode *n, const char *filter)
{
    if (!filter[0]) return 1;
    char q[16];
    int i = 0; for (; filter[i] && i < 15; i++) q[i] = (char)tolower((unsigned char)filter[i]); q[i] = 0;
    int best = text_score(n->label, q);
    for (int l = 0; n->kind == N_APP && l < 5 && best != 1; l++) {
        const char *t = launcher_app_title(n->id, l);
        int sc = t ? text_score(t, q) : 0;
        if (sc && (!best || sc < best)) best = sc;
    }
    return best;
}

// Spotlight: at Home with a filter, search becomes GLOBAL across every app (flat) — so any of the
// ~70 apps is reachable in a couple of keystrokes without digging into a category. Inside a category
// the filter still narrows that category. The keyboard is the Cardputer's edge over a watch.
static bool search_mode(void) { return s_top == 0 && s_stack[0].filter[0]; }
int launcher_settings_hits(void) { return search_mode() ? settings_hits(s_stack[0].filter) : 0; }

// Walk the visible rows in RANK order — prefix matches first, then word starts, then the rest, each
// tier in registration order — and return the want-th (want < 0: none; *total gets the count).
// The renderer asks several times per frame (count, each carousel slot, the focus) and the answer only
// changes with the frame, the query or the tree, so the scores are cached on exactly that key: the
// string matching runs once per keystroke, not once per call. 96 B, no heap.
static unsigned char    s_rank_sc[MAX_APPS + MAX_CATS + MAX_PIN];
static int              s_rank_n = -1;
static const MenuNode  *s_rank_node;
static char             s_rank_f[16];
static unsigned         s_rank_gen;
static bool             s_rank_global;
static const MenuNode *rank_walk(int want, int *total)
{
    bool global = search_mode();
    const char *f = global ? s_stack[0].filter : top().filter;
    const MenuNode *const *items = top().node ? top().node->items : nullptr;   // NULL before launcher_reset()
    if (s_rank_n < 0 || s_rank_gen != s_tree_gen || s_rank_global != global || s_rank_node != top().node || strcmp(s_rank_f, f)) {
        int n = 0;
        if (global) n = s_app_n;
        else while (items && items[n] && n < MAX_APPS + MAX_CATS + MAX_PIN) n++;
        for (int i = 0; i < n; i++) s_rank_sc[i] = (unsigned char)match_score(global ? &s_dyn_apps[i] : items[i], f);
        s_rank_n = n; s_rank_gen = s_tree_gen; s_rank_global = global; s_rank_node = top().node;
        snprintf(s_rank_f, sizeof s_rank_f, "%s", f);
    }
    const int n = s_rank_n;
    const unsigned char *sc = s_rank_sc;
    int seen = 0;
    for (int tier = 1; tier <= 3; tier++)
        for (int i = 0; i < n; i++) {
            if (sc[i] != tier) continue;
            if (seen == want) return global ? &s_dyn_apps[i] : items[i];
            seen++;
        }
    if (global && settings_hits(f) > 0) {         // last: "N settings" -> Settings, searching
        if (seen == want) return &SETQ;
        seen++;
    }
    if (total) *total = seen;
    return nullptr;
}

const MenuNode *launcher_nth_visible(int idx) { return idx < 0 ? nullptr : rank_walk(idx, nullptr); }

int launcher_visible_count(void)
{
    int total = 0;
    rank_walk(-1, &total);
    return total;
}

const MenuNode *launcher_focused(void) { return launcher_nth_visible(top().sel); }

// ---- accessors --------------------------------------------------------------
const MenuNode *launcher_node(void)   { return top().node; }
int             launcher_depth(void)  { return s_top; }
int             launcher_sel(void)    { return top().sel; }
const char     *launcher_filter(void) { return top().filter; }

// Clamp (not wrap) the focused row after the list under it changed size: a rebuild (pin/unpin) or a
// restored snapshot. Wrapping is for arrow keys only (launcher_set_sel).
static void clamp_sel(void)
{
    int n = launcher_visible_count();
    if (n <= 0 || top().sel < 0) top().sel = 0;
    else if (top().sel >= n)     top().sel = n - 1;
}

// After a rebuild, keep the focus on the SAME node when it is still listed (it may have moved: a pin
// shifts Home, a launch reorders Recent), else clamp.
static void refocus(const MenuNode *prev)
{
    int n = launcher_visible_count();
    for (int i = 0; prev && i < n; i++) if (launcher_nth_visible(i) == prev) { top().sel = i; return; }
    clamp_sel();
}

void launcher_set_sel(int sel)
{
    int n = launcher_visible_count();
    if (n <= 0) { top().sel = 0; return; }
    top().sel = ((sel % n) + n) % n;     // wrap, never out of range
}

// ---- navigation ops ---------------------------------------------------------
static void nav_push(const MenuNode *node)
{
    if (s_top + 1 < MAX_DEPTH) { s_top++; top().node = node; top().sel = 0; top().filter[0] = 0; }
}

bool launcher_back(void)
{
    if (top().filter[0]) { top().filter[0] = 0; top().sel = 0; return true; }
    if (s_top > 0) { s_top--; return true; }
    return false;
}

const MenuNode *launcher_enter(void)
{
    const MenuNode *cur = launcher_focused();
    if (!cur) return nullptr;
    if (cur->kind == N_MENU) { nav_push(cur); return nullptr; }
    return cur;                                            // a leaf app
}

void launcher_filter_push(char c)
{
    int l = strlen(top().filter);
    if (l < (int)sizeof(top().filter) - 1) {
        top().filter[l] = (char)tolower((unsigned char)c);
        top().filter[l + 1] = 0;
        top().sel = 0;
    }
}

void launcher_filter_backspace(void)
{
    int l = strlen(top().filter);
    if (l) { top().filter[l - 1] = 0; top().sel = 0; }
}

// ---- pin to Home ------------------------------------------------------------
bool launcher_is_pinned(const char *id)
{
    if (!id) return false;
    for (int i = 0; i < s_pin_n; i++) if (!strcmp(s_pin_buf[i], id)) return true;
    return false;
}

// Pin/unpin an app id at the top of Home, persist, and rebuild the tree. ANIMA is ignored (it already
// lives at Home). A full store drops the oldest pin to make room for the new one.
void launcher_toggle_pin(const char *id)
{
    if (!id || !id[0] || !strcmp(id, "anima") || !strcmp(id, LAUNCHER_SETTINGS_SEARCH_ID)) return;
    for (int i = 0; i < s_pin_n; i++) {                          // already pinned -> unpin
        if (!strcmp(s_pin_buf[i], id)) {
            for (int j = i; j < s_pin_n - 1; j++) memcpy(s_pin_buf[j], s_pin_buf[j + 1], sizeof s_pin_buf[0]);
            s_pin_n--;
            const MenuNode *prev = launcher_focused();
            pins_save(); launcher_build_menu(); refocus(prev);   // Home just lost a row: keep focus on it
            return;
        }
    }
    if (s_pin_n >= MAX_PIN) {                                    // full -> evict the oldest
        for (int j = 0; j < MAX_PIN - 1; j++) memcpy(s_pin_buf[j], s_pin_buf[j + 1], sizeof s_pin_buf[0]);
        s_pin_n = MAX_PIN - 1;
    }
    snprintf(s_pin_buf[s_pin_n++], sizeof s_pin_buf[0], "%s", id);
    const MenuNode *prev = launcher_focused();
    pins_save(); launcher_build_menu(); refocus(prev);
}

// ---- recents ------------------------------------------------------------------
void launcher_note_launch(const char *id)
{
    if (!id || !id[0] || !strcmp(id, "anima")) return;          // ANIMA already leads Home
    // Opened BY the system, not by the user: the idle screensaver, the web-handoff screen, the boot
    // update dialog. Recording them would push the user's real apps out of the tile.
    static const char *const AUTO[] = { "screensaver", "remote", "updates" };
    for (const char *a : AUTO) if (!strcmp(id, a)) return;
    if (s_rec_n > 0 && !strcmp(s_rec_buf[0], id)) return;       // already the newest: nothing changes, no write
    int at = s_rec_n < MAX_REC ? s_rec_n : MAX_REC - 1;         // slot to drop: the id itself, else the oldest
    for (int i = 0; i < s_rec_n; i++) if (!strcmp(s_rec_buf[i], id)) { at = i; break; }
    for (int j = at; j > 0; j--) memcpy(s_rec_buf[j], s_rec_buf[j - 1], sizeof s_rec_buf[0]);
    snprintf(s_rec_buf[0], sizeof s_rec_buf[0], "%s", id);
    if (s_rec_n < MAX_REC && at == s_rec_n) s_rec_n++;
    ids_save(REC_STORE_PATH, s_rec_buf, s_rec_n);
    const MenuNode *prev = launcher_focused();
    launcher_build_menu(); refocus(prev);
}
