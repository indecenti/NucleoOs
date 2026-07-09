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
