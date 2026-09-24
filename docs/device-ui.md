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

## Shared in-app list widget (`app_ui.h`)

Apps with a scrollable list should route keys through `app_ui_list_key()` and draw with
`app_ui_list()` instead of hand-rolling a scroll loop. This gives every list the same
smartwatch UX for free:
- `;` / `.` move with wrap-around; a right-edge scroll knob shows position.
- **`1`–`9` jump to the n-th row** (same shortcut as the launcher).
- **Type-ahead**: typing letters does a time-windowed prefix search (`ra` → first `Ra…`
  row); tapping the same single key again cycles through items starting with it.

Callers today: Files, Calendar, Notes, Photos, Notify, Player, Recorder, Video, Voice,
VoiceLab, IR. New list apps should adopt it rather than forking the widget.

For destructive actions use the shared confirm card `app_ui_confirm(title, msg, yes_focus)`
+ `app_ui_confirm_key()` instead of hand-rolled "press D again" hints. It defaults focus to
**No** so a stray Enter can't delete. Adopted in Notes (delete note), Files (delete file —
replaced the old "press D twice" arm), Notifications (clear history) and Voice (delete trained
template). Returns 1 = confirmed, 0 = cancelled, -1 = still open.

Settings uses it too (Restart, Forget all networks, forget one network). Deliberately **not**
converted: flows that already gate destructive actions their own way — Recorder's delete uses the
blocking `nucleo_ui_menu` Cancel/Delete modal, and Settings ▸ Reset keeps its triple-press
countdown (high-friction on purpose for a whole-OS wipe; the row shows the presses left).

## Control Center (TAB)

TAB from anywhere (unless the foreground app claims it) raises a **one-screen quick panel** — no
tabs, no hidden pages. Top to bottom:

```
┌ 14:05  CasaNet              ▮▮▮ ▭ 85% ┐  status strip: clock · network · signal · battery
│ [1 Mute] [2 Torch] [3 Sleep] [4 Hotspot]│  toggle tiles — keys 1-4 fire them directly
│ ☀ ━━━━━━━━━━━━━━●──────────      70%   │  brightness (LEFT/RIGHT adjust in place)
│ ♪ ━━━━━━━●─────────────────      40%   │  volume     (LEFT/RIGHT adjust, ENTER = mute)
│ (⚙) (▭) (⌨) (▯) (⏻)                    │  Settings · Web client · USB keyboard · USB drive · Restart
└ Brightness 70%   </> adjust            ┘  context line: what the focus does + its live value
```

- UP/DOWN move between lines (wrap), LEFT/RIGHT move inside a line or adjust a slider, ENTER acts,
  Esc or TAB closes. The focus is **remembered** across opens and drawn as a 2 px **ring** around
  the element (red while a disruptive action is armed).
- The context line names the focused control and its state; on the Web-client shortcut it shows
  the **IP and pairing PIN** (what you need to open the web OS).
- Disruptive actions — Hotspot (it drops the Wi-Fi client link) and Restart — **arm** on the first
  ENTER and fire on the second; any other key disarms. In a Wi-Fi-skipped Solo boot (BLE suite /
  Sentinel, NX_WIFI apps, USB-web) the setup config was never loaded, so the Hotspot tile is drawn
  disabled and `nucleo_setup_start_ap/stop_ap` refuse (`nucleo_setup_config_loaded()`).
- Brightness/volume changes are persisted once, when the panel closes. Screen-off turns the
  backlight fully off; the next key wakes it (and is swallowed).
- **Flicker-free without the back-buffer.** On the ADV the 32 KB canvas often can't be allocated
  after Wi-Fi comes up, so the panel paints straight to the display. It then repaints only the
  elements whose state changed (`CcShown s_ccs`) and never clears first: focus moves redraw two
  ring outlines, colour changes redraw a glyph over its own fill, a slider lifts only its old knob
  and repaints the track as two non-overlapping pieces, and every text line is a fixed-width field
  drawn opaque. Only opening the panel — or an overlay (torch, voice, reminder) having painted over
  it (`launcher_render_control_center_invalidate()`) — costs one full paint.

Code: `launcher_render.cpp` (§ Control Center). Theme roles only + named `C_*` semantics.

## Settings app (native, id `wifi`)

A smartwatch-style settings app: a **root list of sections**, each row previewing its live value,
then one page per section. Rows are built on demand from an id (no row arrays in RAM).

```
Settings
├─ [suggestion]  (only when needed: set the clock · battery low → dim · join Wi-Fi)
├─ Wi-Fi        (SSID)     → status card · Nearby networks · Saved networks · Web handoff
├─ Hotspot      (On/Off)   → Hotspot · Status · Name · Password · Address
├─ Bluetooth    (On/Off)   → On at boot (NVS, applies after restart) · Right now · Restart
├─ Display      (70%)      → Brightness · Theme · Screensaver timeout (Never…10 min) · Style
├─ Sound        (40%)      → Volume · Mute · Read aloud (TTS) · Voice speed · Always listen
├─ ANIMA        (inline)   → Offline / Hybrid / Online only (anima_ui.json + live flags)
├─ Language     (inline)   → it / en / es / fr / de
├─ Date/time    (14:05)    → no-typing field editor (day/month/year · hours:minutes)
├─ Device       (name)     → Name · PIN · Web sessions (sign everyone out) · Model · Version ·
│                             Battery · SD · Free RAM · Uptime · Updates · Restart
└─ Reset                   → Reset settings · Factory reset (ENTER ×3, the chip says what it erases)
```

- **Fisheye chips.** Compact rows (19 px) show the name and the value in the proportional
  **Font2** (16 px, full colour); the focused row expands into a two-line accent chip: the name,
  then a readable second line — a slider bar, `< choice >`, the value, or what the setting does.
- **Type to search.** Any letter on the root opens a live search across every section; results
  *act like the real rows* (flip, adjust with LEFT/RIGHT, open). DEL erases, Esc closes.
- **Crown acceleration.** Holding LEFT/RIGHT on a slider steps 5 → 10 once the key repeats.
- Same keys on every screen: UP/DOWN move (wrap) · **1-9** jump (on the root they open the section)
  · ENTER acts · LEFT/RIGHT adjust (elsewhere RIGHT opens, LEFT goes back — LEFT never closes the
  app) · Esc back · **TAB next section**. Focus is remembered per page. Confirmations use a tick toast.
- Toggles flip only on ENTER; the Hotspot switch and "sign everyone out" ask with a confirm card.
- Nearby networks scans when opened; ENTER joins (asks the password only for a secured network
  without a saved one), `p` marks a saved network preferred, `Del` forgets it (confirm card, by SSID).
- Getters that hit storage (TTS voice-pack probe, BLE NVS pref) are cached per visit, never per paint.
- The chip is the **theme accent**, so cycling the theme in Display recolours the screen live.
- **Flicker-free without the back-buffer** (the usual ADV state): only changed boxes repaint, each
  rendered in a strip sprite (as tall as the heap allows: a whole 34 px chip, 19 or 12 rows) and pushed
  atomically; the list scrolls by JUMPS (chip to the top going down, to the bottom going up) so most
  keys repaint two rows, not the whole list. See `ANTI-FLICKER.md` ("atomic box repaint").

Only settings with a real firmware backend are listed. Web-only keys in `settings.json` with no
firmware reader (`device.timezone`, `power.profile`, `power.sleep_timeout_s`, `network.ble.enabled`…)
are deliberately absent: nothing on the device reads them (time zone and NTP are compile-time,
screen-off-while-remote is a `#define`, the real BLE switch is NVS `ble/on`).

Both surfaces draw their icons from the shared system glyph set `ui_glyph.h` (Wi-Fi fan, sun,
speaker, power, gear…), separate from the launcher app-icon set so it never touches `icons:gate`.

## Framework

`nucleo_app` provides the app descriptor (`onEnter/onKey/onTick/onDraw/onExit`), the
single-foreground switcher, the shared chrome (status + hint bars), and the launcher. Native
apps register at boot; the launcher lists them. Built on M5GFX/M5Unified (display + keyboard
+ fonts), like the setup wizard.
