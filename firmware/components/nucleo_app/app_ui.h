// Shared Wear OS-style list widget for the native apps (Files / Music / Photos / Notes).
// The focused row is a size-2 accent pill; the rows adjacent to it stay full-size + readable
// (size 2, white), and rows further away shrink + dim for depth. Mirrors the
// simulator helper web/device/apps/_list.js so the look matches what was verified there.
#pragma once
// C++ only (all callers are the app_*.cpp files); kept out of extern "C" so the row-
// provider callbacks can be ordinary C++ statics without a language-linkage mismatch.

// Per-row data providers. `right` and `color` may be NULL.
typedef const char *(*app_ui_text_fn)(int i, void *ud);
typedef unsigned short (*app_ui_color_fn)(int i, void *ud);

// Consistent, readable app header: accent title (size 2) + a hairline rule, with an
// optional muted right-aligned string. Returns the first usable content y below it.
int app_ui_title(const char *text, unsigned short accent, const char *right);

// Standard tab strip (see docs/native-ui-kit.md). Draws a row of pill tabs at `top`: the active
// one is an `accent` fill with INK text, the rest are a THEME_LINE fill with MUTED text — the same
// selection look as app_ui_list / the launcher. Fills its own 20px band (theme BG) first, so the
// caller need not pre-clear it. Returns the first content y below the strip (top + 20). Replaces
// every app's hand-rolled tabbar(). Tabs that would overflow 240px are dropped rather than clipped.
int app_ui_tabs(int top, const char *const *names, int n, int active, unsigned short accent);

// One selectable settings row in the band [y, y+h): focused = `accent` pill + INK text, else MUTED
// on BG — matching app_ui_list's focused row. For a scrollable list prefer app_ui_list(); use this
// for the handful of fixed rows a settings/tab screen shows. Does NOT clear the whole band (caller
// fills the content area once); it only paints the pill + label.
void app_ui_row(int y, int h, const char *label, bool focus, unsigned short accent);

// Labelled value bar (battery / RAM / storage style): muted `label` (size 1) left, `val` (size 2,
// `col`) right-aligned, and a thin `col` gauge filled to `pct` (0..100) over a THEME_LINE track.
// Self-clears just the value field + bar interior so a shrinking value/bar leaves no trail — safe to
// call every tick without a full-screen wipe. Lay out rows ~26px apart.
void app_ui_gauge(int y, const char *label, const char *val, int pct, unsigned short col);

// Draw `count` rows in the band [top, top+h), highlighting `sel`. Fills the band first.
void app_ui_list(int top, int h, int count, int sel,
                 app_ui_text_fn label, app_ui_text_fn right, app_ui_color_fn color, void *ud);

// Centralized list navigation. Call this from on_key(); returns true if the key was handled:
//   ; / .  (NK_UP/NK_DOWN) move the selection with wrap-around
//   1-9    jump directly to the n-th row (launcher-consistent quick-select)
//   letters type-ahead: a time-windowed prefix search (type "ra" -> first "Ra..." row);
//          tapping the same single key again cycles through the items starting with it.
bool app_ui_list_key(int key, char ch, int *sel, int count, app_ui_text_fn label, void *ud);

// True while the last-drawn list still needs animating (smooth-scroll settling, or the focused
// label mid-marquee). Gate a list app's tick() redraw on this so an idle list stops repainting.
bool app_ui_list_animating(void);

// Shared yes/no confirm card, centered over the content area — for destructive actions (delete,
// wipe, reset) so apps stop hand-rolling "press D again" hints. The app owns a `bool yes_focus`
// state: draw the card each frame while a confirm is pending, and route keys through the _key
// handler. `yes_focus` starts false (No) so an accidental Enter is safe.
void app_ui_confirm(const char *title, const char *msg, bool yes_focus);

// Route a key to a pending confirm card. `;`/`.`/`,`/`/` toggle the focus; Enter picks it;
// `y`/`n` are direct shortcuts. Returns 1 = confirmed, 0 = cancelled, -1 = still open (redraw).
int app_ui_confirm_key(int key, char ch, bool *yes_focus);
