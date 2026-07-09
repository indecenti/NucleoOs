# App categorization — analysis & reorganization

Scope: the **native firmware launcher** taxonomy (the `category` field of each
`nucleo_app_def_t`, grouped by `get_or_create_cat` in
`firmware/components/nucleo_app/launcher_menu.cpp` and drawn as the Home category tiles).
This document records the inventory, how categories are defined **by use**, the reorganization
applied, and the work deliberately deferred.

## How categorization works (native)

- Each native app declares a `category` string in its `nucleo_app_def_t` literal (3rd field).
- `launcher_build_menu()` buckets apps by that string via `get_or_create_cat`, which also
  assigns the category tile its **colour** and **icon glyph** (a special glyph for known names,
  else the first letter). Unknown names still work — blue tile + letter glyph.
- **ANIMA is special**: hoisted straight onto Home (not shown inside a category), so its own
  `category` value is inert.
- Category tile order on Home = the order categories are first seen while registering apps
  (the `nucleo_register_*` call order in `nucleo_app.cpp`).
- Hard limits: `MAX_CATS = 10`, `MAX_APPS = 64` (both `.bss`-resident — do not raise casually,
  see the RAM notes in `launcher_menu.cpp`).

## Guiding principle — split by USE, not by implementation

A category answers "what is the user trying to **do**", not "what tech does it use". The clearest
line runs between apps that *reach another party* and apps that *bridge/​control a machine*:

| Category | Defining use | Apps |
|---|---|---|
| **Communication** | send/receive **messages** — to a person or another device | mail, link (Vicino), swarm (Sciame) — **+ LoRa messaging & other radios to come** |
| **Connect** | **bridge or remote-access** a PC/network — the device as endpoint or peripheral | remote, ssh, keydeck, usb, usbkbd |
| **Media** | play or capture **audio/video** | music, video, radio, photos, recorder, micspec |
| **Office** | **documents, data, time** — get work done | calc, notepad, calendar, clock, files |
| **Tools** | one-shot **gadgets/utilities** | qr, torch, weather, ir, alarm |
| **System** | **device settings, status, display upkeep** | wifi (Impostazioni), theme, info, sysmon, screensaver, pixel-fix, notify |
| **Hardware** | **IMU sensor** instruments | level, goniometer, pedometer |
| **Voice** | **voice-engine** training/diagnostics | voice, voicelab |
| **Security** | authorized **offensive/testing** tools | ble, beacon, evilportal, ethernet, payloads, sniffer, wifiatk |
| **Games** | play | (12 titles) — opens the GameFront front-end |

The **Communication vs Connect** split is the key distinction: "talk to someone/something"
(mail, ESP-NOW file/command exchange, mesh presence — and soon LoRa) vs "connect this device to a
computer/network" (remote-control mode, SSH client, USB drive, USB keyboard, KeyDeck).

## What changed (and why)

Two passes. First pass emptied an overloaded `Tools` and killed a 1-app category. Second pass
(this revision) corrected the comms grouping per the roadmap: **Communication is a dedicated,
growth category** — LoRa messaging and other comm systems will land here — so messaging apps were
pulled OUT of the generic `Connect` bridge category into it.

| App | From → To | Reason |
|---|---|---|
| mail | Communication (kept dedicated) | email = messaging a person |
| link (Vicino) | Connect → **Communication** | ESP-NOW file/command exchange with nearby devices = device messaging |
| swarm (Sciame) | Connect → **Communication** | mesh discovery/ping = device-to-device comms |
| usb | Tools → **Connect** | exposes SD to a PC = host bridge |
| usbkbd | Tools → **Connect** | USB HID keyboard = host bridge |
| calc, notepad, calendar, clock, files | Tools → **Office** | documents/data/time |
| notify | Tools → **System** | OS notification center = a system surface, not office work |
| screensaver, pixel-fix | Tools → **System** | display idle behaviour + panel maintenance |

`Communication` reuses its existing speech-bubble glyph + blue tile (already provisioned in
`get_or_create_cat` and `ui_icon`). `Office` reuses its pre-provisioned briefcase glyph.

## Inventory (after) — 10 categories, balanced, use-driven

| Category | Count | Apps |
|---|---:|---|
| Games | 12 | (unchanged) |
| Security | 7 | (unchanged) |
| System | 7 | info, sysmon, theme, wifi, screensaver, pixel-fix, notify |
| Media | 6 | music, video, radio, photos, recorder, micspec |
| Connect | 5 | remote, ssh, keydeck, usb, usbkbd |
| Office | 5 | calc, notepad, calendar, clock, files |
| Tools | 5 | qr, torch, weather, ir, alarm (+ anima, hoisted) |
| Communication | 3 | mail, link, swarm — **grows with LoRa & other radios** |
| Hardware | 3 | goniometer, level, pedometer |
| Voice | 2 | voice, voicelab |

Distinct categories: **10 = MAX_CATS (10)** — the launcher is now at its category cap.
**Zero `.bss` growth** (no new capacity; the taxonomy just uses all 10 slots). No single-app
categories; the largest non-Games group is 7.

### RAM / roadmap note

Future **LoRa messaging and other comm apps go INSIDE `Communication`** — they are new *apps*
(bounded by `MAX_APPS = 64`, currently 54 in use), not new categories, so they cost no category
slots. However, the launcher now sits at **exactly `MAX_CATS = 10`**: adding a *genuinely new
category* later requires bumping `MAX_CATS` (each +1 costs ≈ `(MAX_APPS+1)` pointers of `.bss` ≈
260 B, plus one `MenuNode`). Do that consciously if/when a new top-level use appears.

## UI/UX applied alongside the reorg

- **Localized category tiles.** Category ids stay English (bucketing + icon lookup), but the Home
  tile title and the breadcrumb now render in the user's language via `node_label()` in
  `launcher_render.cpp` — e.g. IT shows Strumenti / Sistema / Giochi / Comunicazione / **Sensori**
  instead of the raw English ids. Display-only, zero RAM (flash literals). This also renders
  `Hardware` as **Sensori / Sensors** without an id/glyph rename (see deferred, now resolved).
- **Type-to-search hint.** The Home hint bar now reads `invio apre · digita per cercare`, teaching
  the global Spotlight filter (previously undiscoverable).
- Prior launcher UX from earlier passes: pin-to-Home (`*`, yellow dot marker), battery pip in the
  status bar, frame-rate-independent carousel glide.

## Deliberately deferred

- **Native ↔ web taxonomy divergence.** The web side uses a *different* set — `schemas/manifest.schema.json`
  enumerates lowercase `system, productivity, media, tools, connectivity, games` (6), while native
  uses the 10 TitleCase names above. The two taxonomies have always been independent; this pass
  only touched native. Unifying them (map native→web, or one shared list) is a separate
  cross-cutting change (schema + every web manifest). Flagged so it is a conscious decision.
- **`alarm` placement.** Uses IMU + mic (sensor-like) but reads as a gadget; left in Tools.
- **`Voice` (2 apps).** Small but a distinct use (voice-engine trainer + lab); kept separate
  rather than folded into System/Tools.

**Resolved:** `Hardware → Sensori` — done at the display layer (localized label), so the category id
and chip glyph are unchanged while the tile reads "Sensori" (IT) / "Sensors" (EN).

## Verification

- No firmware logic keys off the changed category strings (only `get_or_create_cat`'s colour/icon
  map and `ui_icon`'s category glyphs reference category names; GameFront keys off `"Games"`,
  untouched). Confirmed by grep.
- Category edits are initializer strings — compiled clean; no link impact.
- Web `npm run validate` is unaffected (native `.cpp` categories are independent of the web
  manifest schema).
