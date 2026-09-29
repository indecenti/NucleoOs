// Launcher menu model + navigation (source of truth; host-tested by `npm run launcher:test`).
//
// Owns the hierarchical menu tree (Home -> categories -> apps, plus pinned apps on Home), the
// navigation stack with type-to-filter, and all the queries the renderer/run-loop need.
// It deliberately knows NOTHING about drawing or about launching apps: navigation that
// would start an app returns the target node to the caller (launcher_enter), so this
// module has no dependency on the app-lifecycle code — which is what lets tools/launcher-host
// compile it unchanged on the PC against the real app table.
#pragma once
#include "launcher_theme.h"

enum node_kind_t { N_MENU, N_APP };

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

// Return-cursor across a Solo (NX_SOLO) warm reboot. capture() snapshots the current launch frame
// (Home vs category + type-to-filter + focused row) into RTC just before the Solo reboot; apply()
// re-enters it after the return boot (call AFTER build_menu()+reset(), only in the full-OS boot).
void launcher_capture_return(void);
void launcher_apply_return(void);

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
// Apply Enter to the focused row. Pushes submenus internally;
// returns the app node to launch (caller's job) or NULL when handled here.
const MenuNode *launcher_enter(void);
void            launcher_filter_push(char c);  // append a char to the filter, reset focus
void            launcher_filter_backspace(void);

// ---- recents -------------------------------------------------------------------
// Home carries one fixed "Recent" tile (id LAUNCHER_RECENT_ID) listing the last apps opened, newest
// first; it appears after the first launch. Call on EVERY app launch (launcher, ANIMA, CC, open-with).
#define LAUNCHER_RECENT_ID "recent"
void            launcher_note_launch(const char *id);

// ---- Spotlight -> Settings --------------------------------------------------------
// The last Spotlight result when the query matches settings: launching it opens Settings searching for
// the query (nucleo_app.cpp routes the id). hits() = settings found by the active Home query (0 = none).
#define LAUNCHER_SETTINGS_SEARCH_ID "settings-search"
int             launcher_settings_hits(void);

// ---- pin to Home ------------------------------------------------------------
bool            launcher_is_pinned(const char *id);   // true if this app id rides the top of Home
void            launcher_toggle_pin(const char *id);  // pin/unpin + persist + rebuild the tree
