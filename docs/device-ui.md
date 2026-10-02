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

## Launcher (smartwatch carousel)

A horizontal **icon carousel**: the focused app/category is a big centred badge with its title
underneath; the neighbours peek in smaller and dimmer on each side; a dot rail (or `k/n` past 13
items) marks the position. The status bar carries the breadcrumb (category + app count) or the
clock, date (in the OS language), SSID, Wi-Fi bars and battery; the hint bar names the keys.

- Every arrow (`;` `.` `,` `/`) steps the focus by one, with wrap-around; hold to repeat.
- `Enter` opens a category / launches an app. The **Games** tile opens the GameFront carousel.
- **Spotlight**: typing letters or digits filters. At Home the search is **global** (every app,
  flat); inside a category it narrows that category. An app matches by its registered label **or
  its title in any of the five languages** — "wetter", "meteo" or "weather" all find Weather,
  whatever the OS language. Results are **ranked**: titles that start with the query, then titles
  with a word that starts with it, then the rest (registration order inside each tier), so the
  focus lands on the likeliest app. `Del` edits the query; the status bar shows it (`/query`).
- **Spotlight reaches into Settings**: when the query also matches settings rows, one last result
  "N settings" (Settings glyph) opens Settings already searching for that query.
- `*` pins / unpins the focused app to Home (up to 6, oldest evicted, persisted in
  `/cfg/config/pins.txt`); pinned apps ride right after ANIMA, which always leads Home.
- **Recent**: one fixed Home tile (after the pins, clock glyph) lists the last 5 apps opened, newest
  first, from any path (launcher, ANIMA, Control Center, open-with). System launches (screensaver,
  web handoff, the boot update dialog) are not recorded. It is a single tile so the categories never
  shift; persisted in `/cfg/config/recents.txt`, written only when the order changes.
- `Esc`/`` ` `` clears the query if any, otherwise goes back one level. `TAB` opens the Control Center.
- **Return cursor**: leaving a Solo app (it warm-reboots back to the OS) lands on the same frame —
  Home or category, query and focused row — via an RTC snapshot that a cold boot ignores.
- Digits are **query characters**, not quick-select: a 1–9 launcher shortcut used to steal them
  from Spotlight and was removed. (The in-app list widget below still has 1–9.)

### Menu tree

`launcher_menu.cpp` builds the tree from the registered apps at boot: Home = ANIMA + pinned apps
+ Recent (once something was opened) + one node per category (in registration order); each category
lists its apps in registration order. After a rebuild (pin, launch) the focus follows the same node. Capacity is **`NUCLEO_APP_MAX`** (`nucleo_app.h`, currently 80) for the registry and the
tree alike; an app past it is dropped with an `ESP_LOGE`. The per-category lists share one flat
pointer pool, so raising the cap costs ~68 B per slot, not a full matrix row per category.

### Verify on the PC (no device)

The C code is the source of truth; two host gates compile it unchanged:

- **`npm run launcher:test`** (`tools/launcher-host/`) — the REAL `launcher_menu.cpp` against the
  real app table extracted from the firmware sources (`tools/launcher-host/apps.mjs`): capacity per
  board (original / ADV) vs `NUCLEO_APP_MAX`, every app reachable exactly once, Spotlight (5-language
  titles, ranking, 15-char cap, the Settings result), Recent (order, cap, exclusions, persistence,
  focus), pins (persist, evict, focus) and the Solo return cursor. Part of
  `npm run anima:gate`. It would have caught v0.4.0 silently dropping apps at the old 64 cap.
- **`npm run ui:shots`** (`tools/ui-host/`) — renders the REAL `launcher_render.cpp`, `app_ui.cpp`,
  `app_wifi.cpp` (Settings), `nucleo_theme.cpp` and `nucleo_i18n.c` with the REAL LovyanGFX core into
  an in-memory 240×135 display: pixel-identical to the panel (same fonts, same 8bpp back-buffer, and
  the ADV direct-draw path). Every scene × 5 languages × 4 themes → `build/ui-host/shots/*.png`, one
  sheet per scene in `build/ui-host/sheets/` and a contact sheet `build/ui-host/index.html`. It fails
  on a hint longer than the bar (39 glyphs) and on any pixel change vs `tools/ui-host/golden.json`;
  review the PNGs, then accept with `npm run ui:shots -- --update`. `--only <substring>` renders a subset.

`web/device/` (nav.js + the canvas simulator) is an older JS re-implementation kept for web-side UX
sketches; it does NOT track the firmware (no carousel, no pins, no multilingual search). Trust the
two gates above for the native UI.

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
| `1`–`9` | jump to the n-th row in list apps; in the launcher digits are Spotlight query characters |
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
- **`1`–`9` jump to the n-th row** (the launcher does not do this: there digits feed Spotlight).
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

TAB from anywhere (unless the foreground app claims it) raises the quick panel — smartwatch quick
settings: a status strip over a **scrolling column of four cards**.

```
┌ 14:05  CasaNet              ▮▮▮ ▭ 85% ┐  status strip: clock · network · signal · battery
│ [1 🔇] [2 🔦] [3 ☾] [4 📶]            ▌│  icon tiles — keys 1-4 fire them directly
│ Audio attivo           invio silenzia ▌│  caption (Font2): the focused tile + what ENTER does
│ ☀ Luminosita                     70%  ▌│  brightness card (LEFT/RIGHT adjust)
│ ━━━━━━━━━━━━━━━━━━●──────────────      │
│ ♪ Volume  invio muto             40%   │  volume card (LEFT/RIGHT adjust, ENTER = mute)
│ [5 ⚙] [6 ▭] [7 ⌨] [8 ▯] [9 ⏻]         │  shortcuts — keys 5-9 fire them directly
└ 6 Web 192.168.1.42        PIN 314159   ┘  Settings · Web client · USB keyboard · USB drive · Restart
```

- UP/DOWN move between cards (wrap), LEFT/RIGHT move inside a card or adjust a slider, ENTER acts,
  **1-9 select and fire** the n-th tile/shortcut, Esc or TAB closes. The focus — and the scroll — are
  **remembered** across opens; the focus is a 2 px **ring** around the element (red while armed).
- The column scrolls so the **focused card is always fully in view** (eased when the back-buffer is
  available, a jump when drawing direct); a knob on the right edge shows the position. A scrolled edge
  may cut a tile or a track (it reads as "more this way") but **never half a line of text**: a caption
  is drawn only when its whole line is in view.
- Every caption is **Font2** when it fits the column and falls back to Font0 when a translation does
  not, so no text runs off the panel in any of the five languages. Unfocused cards show their title
  (and quick keys); the focused card names the control, its live state and the key that acts. On the
  Web shortcut it shows the **IP and pairing PIN** (what you need to open the web OS).
- Disruptive actions — Hotspot (it drops the Wi-Fi client link) and Restart — **arm** on the first
  ENTER/digit and fire on the second; any other key disarms. In a Wi-Fi-skipped Solo boot (BLE suite /
  Sentinel, NX_WIFI apps, USB-web) the setup config was never loaded, so the Hotspot tile is drawn
  disabled and `nucleo_setup_start_ap/stop_ap` refuse (`nucleo_setup_config_loaded()`).
- Brightness/volume changes are persisted once, when the panel closes. Screen-off turns the
  backlight fully off; the next key wakes it (and is swallowed).
- **Short back-buffer.** On the ADV the heap's largest block is ~31.7 KB, so
  `nucleo_screen_acquire()` fits the canvas to **240x130** instead of 240x135. The panel's viewport
  ends at the canvas height and the rows below are cleared on the panel, so they never keep the
  previous frame (the launcher footer used to show through, half-cut). `npm run ui:shots` renders
  every scene with a 130-row canvas too (`*.short`) and fails on any panel row left unrepainted.
- **Flicker-free without the back-buffer.** When the canvas cannot be allocated at all the panel paints
  straight to the display and repaints only what changed (`CcShown s_ccs`): focus moves redraw two
  ring outlines, a slider lifts only its old knob and repaints the track as two non-overlapping
  pieces, captions repaint only when their text changes. A scroll there is a single full repaint.

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
  *act like the real rows* (flip, adjust with LEFT/RIGHT, open) and wear their section's glyph. The
  header counts the hits (`18+` past the 18 listed). DEL erases, Esc closes. The launcher's Spotlight
  opens Settings straight on this search (`nucleo_settings_search_preset`).
- **Crown acceleration.** Holding LEFT/RIGHT on a slider steps 5 → 10 once the key repeats.
- Same keys on every screen: UP/DOWN move (wrap) · **1-9** jump (on the root they open the section)
  · ENTER acts · LEFT/RIGHT adjust (elsewhere RIGHT opens, LEFT goes back — LEFT never closes the
  app) · Esc back · **TAB next section**. Focus is remembered per page. Confirmations use a tick toast.
- Toggles flip only on ENTER; the Hotspot switch and "sign everyone out" ask with a confirm card.
- Nearby networks scans when opened; ENTER joins (asks the password only for a secured network
  without a saved one), `p` marks a saved network preferred, `Del` forgets it (confirm card, by SSID).
- Getters that hit storage (TTS voice-pack probe, BLE NVS pref) are cached per visit, never per paint.
- The chip is the **theme accent**, so cycling the theme in Display recolours the screen live
  (`nucleo_theme_preview`, no write). Theme, screensaver time/style, voice speed, brightness and
  volume are held in RAM while you dial them and **saved once** when the focus leaves the row or the
  app (`flush_prefs`), so a held arrow never hammers flash or the SD card.
- **Five languages**: every string is `TR5` (it/en/es/fr/de, ASCII, sized for the 240 px rows);
  `npm run ui:shots` renders each screen in all of them.
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
