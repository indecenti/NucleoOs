# Two screens: the native large view (320x240 touch panel)

With the external 320x240 ILI9341 + XPT2046 panel attached, the panel is the **primary** screen: every app
draws its whole UI there natively (`on_draw_large`) and the Cardputer's own 240x135 screen becomes a
**companion** (state, selection details, key hints). Rule: `CLAUDE.md`, "Two screens: never a mirror".

- **Native, not scaled.** Each screen is laid out for 320x240. Pushing the compact 240x135 frame scaled up
  (`lg::game`, `pushRotateZoom`) is still a mirror.
- **Touch is direct.** You tap or drag the content itself: a row, a card, a system on a map, a board cell,
  the playfield, the target, a gauge used as a slider. No rows of buttons that re-send keys (Up / Down / OK /
  Back / Fire / a D-pad). Buttons exist only where they ARE the content (calculator keys, a macro pad, an IR
  remote, bet chips, a confirm card).
- **Gestures:** tap = pick, tap again (or tap the card) = open / confirm, drag = scroll or aim / steer, the
  header chevron (top left) = Back. The keyboard keeps working exactly as before.

## The kit (`firmware/components/nucleo_app/app_large.h`, namespace `lg`)

| Helper | What it is for |
|---|---|
| `lg::frame(paint, ud)` | Compose a whole 320x240 frame off-screen and blit it strip by strip: no flicker, no extra RAM. Use it for every large screen, animated or not. |
| `lg::field(x, y, w, h)` + `lg::tap(k, ch, &x, &y)` | One touch target whose taps the app hit-tests itself (a map, a board, a playfield, a list with columns). |
| row targets `NUCLEO_TOUCH_ROW + i`, `lg::row_tap` | Lists: tap = select, tap again = open. `lg::list` / `lg::text_list` add scrolling. |
| `nucleo_touch_add_pad(x, y, w, h, step)` | A drag pad: the drag delivers arrows in the FINGER's direction every `step` px (aim, steer, slide a paddle). |
| `nucleo_touch_add(..., NK_BACK, 0)` | The header chevron = Back. |
| `lg::header`, `lg::panel`, `lg::kv`, `lg::bar`, `lg::message`, `lg::font`, `lg::fit`, `lg::wrap` | Theme-coloured building blocks for tool apps; games usually draw their own palette (see `lt / ltf / lhead / lrow` in `app_constellations.cpp`). |
| `lg::comp_head`, `lg::comp_keys` | The Cardputer companion's head line and key hints. |

Fonts on the panel (`lg::font`): 0 = Font2 16 px (body, rows), 1 = FreeSansBold 9 pt (titles),
2 = FreeSansBold 12 pt (big values), 3 = FreeSansBold 18 pt (hero numbers), 4 = Font0 8 px (captions only).

### How `lg::frame` works, and its two rules

The shared 32 KB canvas, seen 320 px wide, holds ~100 rows. `lg::frame` calls `paint()` once per strip with
`d` = a window onto the canvas in PANEL coordinates (a sprite whose buffer starts `oy` rows before the
canvas, clipped to the strip), then pushes the finished strip. So `paint()` draws the whole frame each time,
fills its background first, and registers its touch targets inside (the set is cleared before every strip).

1. Inside `paint()` never call `clearClipRect()`, and never use `drawWideLine` / `drawWedgeLine` /
   `drawGradientLine`: they replace the clip with the whole sprite and **write outside the canvas** (heap
   corruption; it crashed the gallery for slots, reactor, sandgarden and pinball before the fix). `fillSmooth*`
   are safe (they go through clipped `fillRect` / `fillRectAlpha`). A narrower `setClipRect` is fine if the
   strip's own clip is restored afterwards.
2. Inside `paint()` never use `d.height()` or `nucleo_app_content_height()`: they report the strip or the
   compact screen. Use 320x240 constants.

### Touch on the large surface (what the hardware delivers)

`nucleo_extdisp.cpp` turns XPT2046 samples into keys. A tap is delivered on release; there is no
"finger down" or "hold" event. A tap inside a registered target delivers that target's key and records its
position (`nucleo_touch_last`). A drag inside a scroll area scrolls with a fling; inside a pad it emits
finger-direction arrows; anywhere else it emits natural-scroll arrows (drag left = Right), and a long press is
Back. Limits per frame: 16 targets, 2 scroll areas + pads (first registered wins on overlap).

### Traps

- In two-screens mode the compact draw is **not called** (`on_draw` draws the companion). Logic that lived in
  draw code (a cinematic ending on time, caches filled while drawing) must run from `on_tick` / the poll
  handler too. Example: Costellazioni's cinematics now end in `poll`.
- Views that reuse a compact renderer at full resolution parametrize it (`s_vw / s_vh / s_vk`, `view_set()`,
  a local `const int W = s_vw;` shadowing the global 240) instead of duplicating it. Hit-testing taps must use
  the same mapping as the painter.
- `-Werror=format` on xtensa: `uint32_t` is `long unsigned int`, so `%u` on it fails on the device but not on
  the PC. Cast to `(unsigned)`. Only `idf.py build` catches it.
- Flash budget: the app partition (3.5 MB) has ~29 KB left after the 14 games (image 0x378DA0). Reuse
  renderers; delete replaced code.

## Verifying on the PC

`node tools/ui-host/apps.mjs --app <key>` renders every scene of `tools/ui-host/apps/<key>.txt` from the real
source into `build/ui-host/gallery/sheets/<key>.png` (compact | companion | large | large with touch targets:
magenta = targets, cyan = scroll areas / pads). Scene tokens include `row:<n>` (a tapped row target) and
`tap:<x>,<y>` (a field tap at panel pixels). Drags cannot be simulated: test them on the device. Then always
finish with `idf.py --no-ccache build`. Known gallery limit: games open `/sd/...` on the drive root
(`D:/sd/data`), so saved settings and scores leak between scenes.

## The 14 native games (2026-10-27)

| Game | Panel | Touch |
|---|---|---|
| Costellazioni | Every screen native: title, settings, cinematics, star map with target card, dock as 6 cards, market with buy/sell columns, shipyard, bridge, events, missions, briefing, debrief; the dogfight rendered at 320x240 through the view-parametric 3D renderer with its own HUD. | Rows tap/tap-again; map: tap a system, tap again or its card to jump; market: tap the price = buy, the hold = sell; combat: tap a foe = aim + fire, drag = steer the reticle, tap the missile rack / boost gauge. |
| Poker | Table with 56x96 cards, chips, deck. | Tap a card = hold; chip = bet; deck = deal / draw. |
| Slots | Reels, drawn lever, line tabs, bet coins, AUTO lamp. | Tap reels / lever = spin (again = stop); tab = lines; coin = bet. |
| Cardler | World at 20x15 tiles with a top bar. | Tap ground = walk there; tap a villager / beast / chest = go and act; drag = steer. |
| Reactor | Power and core rings, six rods as sliders. | Tap / drag a rod to set it; tap the core ring = SCRAM. |
| Sand garden | 230 px board with a side column (score, best plant, Undo card). | Swipe on the board = one move; tap a board side = slide there; tap Undo. |
| Snake | 32x21 cells of 10 px, HUD band with minimap. | Drag = steer; tap = turn toward the point. |
| Pong | Field mapped to the panel, compact physics (network-compatible). | Drag = paddle follows; tap = paddle glides there. |
| Pinball | Portrait table 1:1 at full height + plunger lane + native side card. | Tap left / right half = flipper; pull the plunger down and release = launch. |
| Brawler | Stage at 320x180 under a 60 px HUD band. | Tap an enemy = walk + punch (again = combo); tap the road = walk; tap the hero = jump; drag = step. |
| Tank Duel | Arena at full resolution with a 28 px HUD. | Drag = drive; tap = fire toward the point; tap the chip = power-up. |
| Tanks | Battlefield 1:1 in a 320x240 view, HUD with weapon card, angle, power bar. | Tap the field = aim there; drag = angle / power; tap your tank = fire; tap weapon card / power bar. |
| Orde (vs) | World at native resolution, hero select cards, level-up cards. | Tap = walk there; drag = steer; tap a card = pick, again = take. |

Not verified on the PC (need the device): drags and flings, the SD sprite atlases (Cardler, Orde), the
co-op / network guest paths (touch moves only the local player), a few end screens no scene reaches.

## The other apps (2026-10-27)

42 apps redone with the panel as the primary screen (commit "Apps: the external panel as the primary
screen"): the 12 that were still mirrored (torch, qr, level, goniometer, pedometer, pixelfix, chrono,
pomodoro, alarm, dice, yahtzee, micspec) got their own view; the rest lost their generic key bars and use
the space better (calendar month grid + agenda, notepad with a tap-placed cursor, files detail pane, Wi-Fi
networks + detail, scrub bars and volume sliders in the media apps, swipe-to-archive notifications, ...).
The Security apps are out of scope and were not touched.

Known limits / follow-ups:
- `lg::frame` composes in the shared 8-bit (RGB332) canvas: dark theme greys such as `lg::card()` turn black
  or olive there. Apps with themed cards draw direct (or use a colour that survives, e.g. 0x212A). A 16-bit
  mode for `lg::frame` (5 strips of 48 rows) would fix it at some speed cost.
- micspec recomposes the whole panel at its frame rate; on the device it may be bound by the SPI push
  (~30 ms per 320x240 frame) — check it there.
- Notify's swipe-to-archive rewrites `notify.jsonl`: a notification arriving in those few ms is lost.
- Flash: 18 KB left in the app slot (image 0x37B890). Any further panel work must pay for itself.
