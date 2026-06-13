# On-device UI (device-first)

NucleoOS is **device-first**: the OS home is the Cardputer screen (240×135) driven by the
physical keyboard — no mouse. The browser is an optional remote companion, not the OS.
The on-device UI is **minimal, highly readable, single-app-foreground**, in the spirit of a
Google Wear OS launcher.

## Screen chrome (always present)

```
┌ 12:34            85%🔋  WiFi ┐   status bar  (~14px): time · battery · radio
│                              │
│      (app content area)      │   ~107px: full width for the active app
│                              │
├ ;/. move · ⏎ open · esc back ┤   hint bar   (~14px): contextual key hints
└──────────────────────────────┘
```

## Launcher (Wear OS-style focused menu)

A vertical list where the **selected row is a large rounded pill** (icon in a colored
circle + big label); neighbours are smaller and dimmed. Few items per screen, generous
spacing, a breadcrumb at the top and a one-line **instruction** above the hint bar.

- `;` up · `.` down (hold to repeat), with wrap-around.
- **Number keys `1`–`9` activate directly** (no scrolling) — exploits the physical keyboard.
- **Type-to-filter**: typing letters narrows the current menu (case-insensitive substring).
- `Enter` opens the focused menu / launches the focused app.
- `/` (right) opens the focused app's **context submenu** (Open · Pin to Home · App Info).
- `Esc`/`` ` `` clears the filter if any, otherwise goes back one menu level.

### Menu hierarchy (menus, submenus, icons)

The launcher is a hierarchical tree, not a flat list. The root shows **categories**; each
descends into its apps; each app has a small context submenu. Every node has a glyph icon, a
per-category accent color, and a clear English `desc` shown on the instruction line.

```
Home
├─ ♪ Media     → Voice Recorder · Music · Photos
├─ ⚙ Tools     → Calculator · Clock · Files · IR Remote
├─ ◆ System    → System Status · Network · Settings · About
└─ ⇄ Connect   → Companion App · Swarm
                   └─ (per app) ▶ Open · ★ Pin to Home · ⓘ App Info
```

### Simulator & tests (verify before flashing)

The navigation logic lives in **`web/device/nav.js`** as a pure, render-free state machine —
the single source of truth that the firmware mirrors in C (`nucleo_app.cpp`). It is:

- **Unit-tested**: `tools/device-ui.test.mjs` (run `npm test`) covers nav, wrap-around,
  filter, back/clear, quick keys, context menu and the contextual hint/instruction text.
- **Previewed**: `web/device/` is a faithful **240×135 pixel simulator** (canvas, integer-
  scaled, keyboard-driven), served at `/device/` by `tools/serve-shell.mjs`. Use it to design
  and verify the on-device UX in a browser before committing it to firmware.

## Foreground model ("one closes the other")

Exactly one app runs at a time, full-screen. Opening an app calls its `onEnter`; pressing
**Esc** calls `onExit` and returns to the launcher, freeing its RAM. No windows. This suits
240×135 and 512 KB.

## Keymap (Cardputer)

| Key | Action |
|---|---|
| `;` / `.` | up / down (nav) |
| `,` / `/` | left / right (where an app uses it) |
| `Enter` | select / confirm |
| `Esc` (top-left key) | back / close app → launcher |
| `Del` | backspace (text) |
| `1`–`9` | direct launch in launcher; app-specific elsewhere |
| printable | type (filter, text, calculator) |

(Verify key codes on hardware; M5Cardputer exposes `keysState()` with `enter`/`del` flags
and a `word` list; the top-left key is treated as Esc.)

## UI/UX tricks (readability on a tiny screen)

1. **Number-key direct launch** and **type-to-filter** — keyboard-first speed.
2. **Auto-fit font**: Clock/Calculator scale digits to fill the width.
3. **Marquee**: long titles/filenames scroll horizontally.
4. **Inverted high-contrast selection**, scroll indicator on the right edge.
5. **Thin-line gauges** for battery/RAM/storage — cheap to draw, instantly readable.
6. **Hold-to-repeat** navigation; light **slide** transition launcher↔app.
7. **Per-category accent color**; dark high-contrast theme; generous spacing (≤5 items/screen).

## Native app set (few, readable)

| App | Notes |
|---|---|
| **Setup Wizard** | First boot only (already implemented) |
| **Music** | MP3 via on-device decode → I2S speaker (audio is feasible without PSRAM) |
| **Clock** | Big auto-fit time + date |
| **Calculator** | Big display; types directly on the keyboard |
| **Files** | SD browser (list, open text), breadcrumb path |
| **System status** | Battery, RAM, storage, network — thin gauges |

## Framework

`nucleo_app` provides the app descriptor (`onEnter/onKey/onTick/onDraw/onExit`), the
single-foreground switcher, the shared chrome (status + hint bars), and the launcher. Native
apps register at boot; the launcher lists them. Built on M5GFX/M5Unified (display + keyboard
+ fonts), like the setup wizard.
