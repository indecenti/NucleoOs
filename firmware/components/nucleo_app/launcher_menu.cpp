// Launcher menu model + navigation. See launcher_menu.h.
#include "launcher_menu.h"
#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include <string.h>
#include <ctype.h>

// Registered foreground apps, owned by nucleo_app.cpp; read here to build the tree.
int                       nucleo_app_count(void);
const nucleo_app_def_t   *nucleo_app_at(int i);

#define MAX_APPS 40
#define MAX_CATS 8
#define MAX_DEPTH 6

// ---- shared per-app context actions ----------------------------------------
static const MenuNode A_OPEN = { "open", "Open",        '>', C_GREEN,  N_ACTION, "Launch this app full-screen", nullptr };
static const MenuNode A_PIN  = { "pin",  "Pin to Home", '*', C_YELLOW, N_ACTION, "Show this app at the top of Home", nullptr };
static const MenuNode A_INFO = { "info", "App Info",    'i', C_GREY,   N_ACTION, "Version, permissions, storage", nullptr };
static const MenuNode *ACTIONS[] = { &A_OPEN, &A_PIN, &A_INFO, nullptr };
static const MenuNode ACTIONS_MENU = { "ctx", "", ' ', C_BLUE, N_MENU, nullptr, ACTIONS };

// ---- dynamically built tree -------------------------------------------------
static MenuNode        s_dyn_cats[MAX_CATS];
static MenuNode        s_dyn_apps[MAX_APPS];
static const MenuNode *s_cat_items[MAX_CATS][MAX_APPS + 1];
static const MenuNode *s_root_items[MAX_CATS + 2];   // +1 slot: ANIMA is hoisted onto Home next to the categories
static MenuNode        ROOT;
static int             s_cat_count = 0;

// ---- navigation state -------------------------------------------------------
struct Frame { const MenuNode *node; int sel; char filter[16]; };
static Frame s_stack[MAX_DEPTH];
static int   s_top = 0;
static const MenuNode *s_ctx_owner = nullptr;

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
        else if (!strcmp(name, "Connect")) c = C_PURPLE;
        else if (!strcmp(name, "Security")) c = C_RED;
        else if (!strcmp(name, "Games"))   c = C_RED;
        s_dyn_cats[i].color = c;
        s_dyn_cats[i].kind = N_MENU;
        s_dyn_cats[i].desc = "";
        s_dyn_cats[i].items = s_cat_items[i];
        s_cat_items[i][0] = nullptr;
        return i;
    }
    return 0;
}

void launcher_build_menu(void)
{
    s_cat_count = 0;
    const MenuNode *anima_node = nullptr;        // hoisted straight onto Home, not buried in a category
    int n = nucleo_app_count();
    if (n > MAX_APPS) n = MAX_APPS;
    for (int i = 0; i < n; i++) {
        const nucleo_app_def_t *a = nucleo_app_at(i);
        s_dyn_apps[i].id = a->id;
        s_dyn_apps[i].label = a->name;
        s_dyn_apps[i].icon = a->icon;
        s_dyn_apps[i].color = a->color;
        s_dyn_apps[i].kind = N_APP;
        s_dyn_apps[i].desc = a->desc ? a->desc : "";
        s_dyn_apps[i].items = nullptr;

        if (!strcmp(a->id, "anima")) { anima_node = &s_dyn_apps[i]; continue; }  // ANIMA -> top-level Home entry

        int c = get_or_create_cat(a->category);
        int j = 0;
        while (s_cat_items[c][j] != nullptr) j++;
        s_cat_items[c][j] = &s_dyn_apps[i];
        s_cat_items[c][j + 1] = nullptr;
    }
    int r = 0;
    if (anima_node) s_root_items[r++] = anima_node;            // ANIMA leads Home, above the categories
    for (int i = 0; i < s_cat_count; i++) s_root_items[r++] = &s_dyn_cats[i];
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
    s_ctx_owner = nullptr;
}

// ---- filtering + visible-row queries ---------------------------------------
static bool match_filter(const MenuNode *n, const char *filter)
{
    if (!filter[0]) return true;
    char a[40], b[40];
    int i = 0; for (; n->label[i] && i < 39; i++) a[i] = (char)tolower((unsigned char)n->label[i]); a[i] = 0;
    int j = 0; for (; filter[j] && j < 39; j++)   b[j] = (char)tolower((unsigned char)filter[j]);  b[j] = 0;
    return strstr(a, b) != nullptr;
}

const MenuNode *launcher_nth_visible(int idx)
{
    const MenuNode *const *it = top().node->items;
    const MenuNode *found = nullptr; int seen = 0;
    for (int i = 0; it && it[i]; i++) {
        if (!match_filter(it[i], top().filter)) continue;
        if (seen == idx) found = it[i];
        seen++;
    }
    return found;
}

int launcher_visible_count(void)
{
    const MenuNode *const *it = top().node->items;
    int total = 0;
    for (int i = 0; it && it[i]; i++)
        if (match_filter(it[i], top().filter)) total++;
    return total;
}

const MenuNode *launcher_focused(void) { return launcher_nth_visible(top().sel); }

// ---- accessors --------------------------------------------------------------
const MenuNode *launcher_node(void)   { return top().node; }
int             launcher_depth(void)  { return s_top; }
int             launcher_sel(void)    { return top().sel; }
const char     *launcher_filter(void) { return top().filter; }

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

void launcher_open_context(void)
{
    const MenuNode *cur = launcher_focused();
    if (!cur || cur->kind != N_APP) return;
    s_ctx_owner = cur;
    nav_push(&ACTIONS_MENU);
}

const MenuNode *launcher_enter(void)
{
    const MenuNode *cur = launcher_focused();
    if (!cur) return nullptr;
    if (cur->kind == N_MENU) { nav_push(cur); return nullptr; }
    if (cur->kind == N_ACTION) {
        const MenuNode *owner = s_ctx_owner;
        launcher_back();                                   // close the context submenu
        if (owner && !strcmp(cur->id, "open")) return owner;
        return nullptr;                                    // pin/info: no state change here
    }
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
