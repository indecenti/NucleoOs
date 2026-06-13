// Launcher menu model + navigation (the C mirror of web/device/nav.js).
//
// Owns the hierarchical menu tree (categories -> apps -> per-app context actions), the
// navigation stack with type-to-filter, and all the queries the renderer/run-loop need.
// It deliberately knows NOTHING about drawing or about launching apps: navigation that
// would start an app returns the target node to the caller (launcher_enter), so this
// module has no dependency on the app-lifecycle code. Keep in sync with nav.js, which is
// unit-tested (tools/device-ui.test.mjs) and previewed in web/device/.
#pragma once
#include "launcher_theme.h"

enum node_kind_t { N_MENU, N_APP, N_ACTION };

struct MenuNode {
    const char *id;
    const char *label;
    char icon;
    unsigned short color;
    node_kind_t kind;
    const char *desc;
    const MenuNode *const *items;   // NULL-terminated; menus only
};

// Build the dynamic category->app tree from the registered apps, then reset to the root.
void launcher_build_menu(void);
void launcher_reset(void);

// ---- queries (current frame) -----------------------------------------------
const MenuNode *launcher_node(void);          // the menu currently shown
int             launcher_depth(void);         // 0 at the root, >0 inside a submenu
int             launcher_sel(void);           // index of the focused visible row
void            launcher_set_sel(int sel);    // clamp + set focused row
const char     *launcher_filter(void);        // active type-to-filter string ("" if none)
const MenuNode *launcher_focused(void);        // focused visible node (NULL if none)
int             launcher_visible_count(void);  // rows visible under the active filter
const MenuNode *launcher_nth_visible(int idx); // nth visible row (NULL if out of range)

// ---- navigation ------------------------------------------------------------
bool            launcher_back(void);           // clear filter, else pop a frame; false at root
void            launcher_open_context(void);   // open the focused app's context submenu
// Apply Enter to the focused row. Pushes submenus / resolves context actions internally;
// returns the app node to launch (caller's job) or NULL when handled here.
const MenuNode *launcher_enter(void);
void            launcher_filter_push(char c);  // append a char to the filter, reset focus
void            launcher_filter_backspace(void);
