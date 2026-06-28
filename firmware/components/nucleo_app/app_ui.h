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

// Centralized navigation + Quick-Jump (alphabetical search). Call this from on_key().
// Returns true if the key was handled.
bool app_ui_list_key(int key, char ch, int *sel, int count, app_ui_text_fn label, void *ud);

// True while the last-drawn list still needs animating (smooth-scroll settling, or the focused
// label mid-marquee). Gate a list app's tick() redraw on this so an idle list stops repainting.
bool app_ui_list_animating(void);
