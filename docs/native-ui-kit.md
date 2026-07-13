# Native UI kit — the on-device look, one source of truth

Companion to `docs/device-ui.md` (which covers the launcher + navigation model). This file is
the **style contract every native app (`firmware/components/nucleo_app/app_*.cpp`) must follow**
so the OS looks like one product instead of 90 hand-rolled screens. When two apps disagree on a
gray, a capsule, a header or a hint, they are both wrong — the answer is here.

The primitives live in `app_ui.h` (draw helpers) and `nucleo_theme.h` (the palette). Adopt them;
do not fork them.

## 1. The golden rule: never hardcode a chrome color

The active palette is eight runtime globals in `nucleo_theme.h`. A theme switch (Settings ▸ Theme)
rewrites them live. **Any RGB565 literal you bake into a draw call ignores the theme** — the app
silently breaks under Hacker Green / AMOLED (e.g. a hardcoded `0x0841` text field renders
black-on-black). Use the role, not the number:

| Role | Global | Use for |
|---|---|---|
| Background | `THEME_BG` | the content fill, text backdrops |
| Foreground | `THEME_FG` | primary text |
| Muted | `THEME_MUTED` | secondary labels, units, timestamps |
| Dim | `THEME_DIM` | tertiary / disabled |
| Line | `THEME_LINE` | hairlines, gauge tracks, inactive-tab fill |
| Ink | `THEME_INK` | text **on** an accent fill (selected pill / active tab) |
| Accent | `THEME_ACC` | the theme's highlight |

`app_ui.cpp` already `#define`s `BG/FG/MUTED/DIM/LINE/INK/ACC` to these — copy that block into a
new app rather than re-picking grays. The classic-theme values (`0x8C71` muted, `0x2945` line,
`0x0841` bg…) are **not** magic constants to reuse; they are one theme's data.

**Allowed literals:** app-*content* colors with real semantics — weather-icon tints, gauge
per-metric colors, game sprites. Give them **named constants** (`SUN`, `RAIN`, `BAT_COL`), never a
bare `0x…` inside a `d.fill*()` call. Chrome (headers, tabs, rows, hints, panels) is theme-only.

## 2. Per-app accent (identity)

Every app is registered with an accent `color` (the `nucleo_app_def_t` field, shown in the
launcher). Reuse **that one color** as the app's in-screen accent — declare it once at file scope
(`static const uint16_t ACCENT = 0x5D9F;`) and pass it to `app_ui_title/tabs/row`. This keeps
layout identical across apps while preserving per-app identity, exactly like the launcher's
per-category accents. Do not scatter the accent as a literal across draw calls.

## 3. Shared widgets — the standard building blocks (`app_ui.h`)

| Widget | Signature | Replaces |
|---|---|---|
| **Header** | `int app_ui_title(text, accent, right)` | hand-drawn title + hairline rule |
| **Tab strip** | `int app_ui_tabs(top, names, n, active, accent)` | per-app `tabbar()` loops |
| **Scroll list** | `app_ui_list(...)` + `app_ui_list_key(...)` | hand-rolled scroll loops |
| **Selectable row** | `app_ui_row(y, h, label, focus, accent)` | one-off settings rows |
| **Gauge** | `app_ui_gauge(y, label, val, pct, col)` | copied battery/RAM/storage bars |
| **Confirm card** | `app_ui_confirm(...)` + `app_ui_confirm_key(...)` | "press D again" hints |

Selection look is **one look everywhere**: the focused row / active tab is an **accent-filled
rounded pill with `INK` text**; unfocused is `MUTED` text on `BG`. This matches the launcher and
`app_ui_list`. Do **not** invent a dark capsule tint (the old `0x1A8B` / `0x10A2` / `0x12B2`
divergence) — that role does not exist in the palette.

Blocking flows use the framework modals in `nucleo_ui.h` — `nucleo_ui_input` (text entry),
`nucleo_ui_menu`, `nucleo_ui_message`. Do not hand-roll a keyboard-read text-input loop.

## 4. Header + layout geometry

The framework owns the **status bar** (top, `BAR`=16px) and the **hint bar** (bottom, `HINT`=14px).
The app draws only `[nucleo_app_content_top(), +nucleo_app_content_height())`. Never draw into the
bars directly; set the footer with `nucleo_app_set_hint()`.

Standard vertical rhythm: header via `app_ui_title` (24px incl. rule) → content. Tabbed apps:
`app_ui_tabs` (20px) → content. List rows step 22px. Settings rows 22–24px.

## 5. Hint bar — one vocabulary, always translated

The hint bar is the most visible cross-app inconsistency. Rules:

1. **Always `TR(it, en)`** — never a bare Italian string literal. The repo is English-first with a
   live IT/EN switch; a bare literal breaks the language toggle *and* the i18n gate.
2. **Lowercase keys, canonical verbs, `"   "` (3-space) separators.** Vocabulary:

   | Action | IT | EN |
   |---|---|---|
   | navigate up/down | `su/giu` | `up/dn` |
   | left/right | `</>` | `</>` |
   | confirm | `invio <verb>` | `enter <verb>` |
   | close app | `esc esci` | `esc back` |
   | back one level | `esc indietro` | `esc back` |
   | open | `apri` | `open` |
   | choose | `scegli` | `pick` |

   Example: `TR("su/giu scegli   invio apri   esc esci", "up/dn pick   enter open   esc back")`.

## 6. Anti-flicker (unchanged, see `docs/ANTI-FLICKER.md`)

Default is canvas + `on_draw`/poll; the framework composites off-screen and blits once. Only pin
`nucleo_app_set_direct_draw(true)` when the app freed the 32 KB canvas for RAM, and then self-clear
only the regions you change. Static screens redraw on key input only — never repaint idle.

## 7. Adoption status & migration

Reference apps (already on-canon): **Files, Notes, Calendar** (list), **System Status**, **Meteo**
(tabbed, post-pilot). New apps start from `nucleo-new-app` / the `nucleo-native-app` skill, which
pull these primitives.

Migrating an existing app:
1. Delete the local gray/accent literals; `#define` the `THEME_*` block from `app_ui.cpp`.
2. Header → `app_ui_title`; tabs → `app_ui_tabs`; settings rows → `app_ui_row`; gauges →
   `app_ui_gauge`.
3. `TR()` every hint; apply the vocabulary above.
4. Keep genuine content colors as named constants.
5. Build (`ninja` single-obj compile-check) and confirm on device (native UIs aren't in the
   `web/device` simulator — pixels are only verifiable on hardware).

The **`npm run ui:lint`** gate (`tools/native-ui-lint.mjs`) flags raw `0x…` literals fed into draw
calls in `app_*.cpp` and bare (non-`TR`) `nucleo_app_set_hint` strings, so regressions fail the gate
rather than ship. It is green across the whole app set as of the homogenization campaign; run it
before adding or editing a native app.
