# Anti-flicker rules for on-device UI (READ BEFORE DRAWING)

The Cardputer drives a bare ST7789 panel with **no hardware double-buffer**, and the board
has **no PSRAM** for a full off-screen framebuffer. Therefore **any `clear-then-draw` straight
to the panel shows the cleared state for one frame = visible flicker.** Every time we forgot
this, the UI flickered. The fix is always one of the techniques below — never "just redraw".

## The one cause
`d.fillScreen(...)` / `d.fillRect(<big area>, bg)` immediately followed by drawing content,
**on a repeating cadence** (per frame, or per 1 Hz update), makes the panel blink that area.

## The four sanctioned techniques

1. **Composite into an off-screen sprite, push once.** For anything that animates, build the
   frame in an `M5Canvas` and `pushSprite()` it in a single blit. The panel never sees an
   intermediate state.

   **The framework guarantees the canvas for you — apps need NO per-app acquire.** `open_app_def()`
   (nucleo_app.cpp) re-acquires the shared canvas right after `on_enter()` for any app that did not
   opt into direct draw (`!s_app_direct`), with the same ≤150 ms retry as `close_app()`. This is the
   single owner of the invariant "a buffered app always has its back-buffer", and it covers BOTH the
   launcher→app path and the app→app path (`launch_by_id`, e.g. ANIMA "apri tanks", which bypasses
   `close_app`'s re-acquire). Before this, a full-screen game launched after a media/voice app that had
   released the canvas would fall back to DIRECT draw and flicker (its whole-screen repaint clears the
   panel every frame) — every animated game (tanks/pong/pinball/constellations/brawler…) shared the
   bug; only yahtzee had bolted on its own acquire. Now none of them need to. Direct-draw apps
   (ANIMA/radio/video/SSH/recorder) opt out by setting `s_app_direct` in `on_enter` and are left alone.

   **There is ONE shared back-buffer** (`nucleo_screen()`, defined in nucleo_ui.cpp): a single
   240×(H−HINT) canvas reused by the launcher list band AND every foreground app — they are
   mutually exclusive on screen. It is allocated **once at boot in `nucleo_ui_init`, while the
   heap is still clean** (before Wi-Fi/HTTP/mDNS fragment it), and **kept** for the whole
   session. This matters: the old design allocated/freed a per-launcher band and a per-app
   `s_fb` on every transition, so under runtime fragmentation (largest free block ~31 KB) the
   re-allocation failed and the scrolling list silently dropped to a flickering direct draw.
   With one persistent buffer the buffered path never re-allocates → the scroll can't flicker.
   The band blits only its region with a destination-clipped `pushSprite` (the canvas is taller
   than the band); apps push the whole canvas at (0,0). Allocated at 8bpp (RGB332, ~28 KB):
   held permanently from boot, so a small predictable footprint beats colour depth — and
   networking already forced 8bpp almost always before, so this is the same look, just reliable.

   **The buffer is released for the decoders and re-acquired lazily.** `nucleo_screen_release()`
   frees the 32 KB canvas (deleteSprite) so an audio/media app can hand the contiguous block to
   the Helix MP3 decoder (~17 KB single alloc) — without this, MP3InitDecoder fails out-of-RAM
   and playback is silent on the PSRAM-less chip. On return the canvas re-acquires: explicitly in
   `close_app()`, or lazily in the `nucleo_screen()` getter with a ~400 ms timed retry (so a
   failed 32 KB createSprite isn't attempted every frame while a decoder still holds the RAM, but
   the UI self-heals within ~0.4 s of the RAM coming back). The canvas is 240×135 @ 8bpp =
   32 400 B — sized to re-acquire cleanly after a decoder ran (137 rows = 32 880 B did NOT fit
   the ~32 768 B contiguous block left behind, which is what caused the old "video → menu
   flickers" bug). Video allocates its OWN ~8 KB JPEG frame buffer; music `now_playing` and the
   Control Center composite straight into the shared canvas (they own the screen then).

2. **Static-once + dynamic-small-regions (the blocking-modal pattern).** When a sprite won't
   fit (a modal running the decoder / a video frame buffer):
   - Draw the **static layout exactly once** when the view appears (labels, rules, frames).
   - On updates, redraw **only the individual elements whose value changed**, each in its
     **smallest bounding box**, gated to that element actually changing (e.g. the seconds
     digit, the playhead band, a meter's segments). A small `fillRect`+draw at ~1 Hz over a
     few pixels is imperceptible; a full-bar/full-screen `fillRect` per update is not.
   - **Never** `fillScreen` or `fillRect` a whole bar/screen on the update path. That single
     line is what flickers. (`now_playing` in app_player.cpp is the reference implementation;
     `launcher_render_clock_tick` reprints just the HH:MM digits in place at 1 Hz instead of
     re-wiping the whole status bar via the old per-second `launcher_render_chrome`.)

3. **Continuously-scrolling text (marquee) → its own small sprite.** A marquee must repaint
   every frame, so a per-frame `fillRect`+print of its line flickers. Render the marquee into
   a small dedicated `M5Canvas` (e.g. the title line, ~220x18) and `pushSprite()` it each
   frame. Cheap (a few KB) and flicker-free.

4. **Opaque content draws direct.** A JPEG video frame (`drawJpg`) fully covers its area, so
   it has no clear-then-draw step and never flickers. Only the *overlay* on top of it needs
   techniques 2/3. Clip the opaque content so it never overdraws the overlay region.

## Technique 2, applied to an app that can lose the canvas (the Connection sheet)

An event-driven app is not automatically safe. The Connection app (`app_info.cpp`) is the reference
for the mixed case: it draws a key/value sheet that also carries LIVE values (free RAM, largest
block, battery, uptime), so it repaints ~1 Hz forever, and it is exactly the app you open right
after a web client / Remote took the 32 KB canvas — i.e. on the DIRECT path. Full-area
`fillRect` + redraw at 1 Hz, plus one frame per easing step of its smooth scroll, is technique 1's
cadence with technique 1 unavailable: it blinks. What it does now, and what any app in that shape
should copy:

- **Know which path you are on.** `nucleo_app_is_buffered()` is valid inside `on_draw`; latch it
  into a static so the poll handler can read it too.
- **Buffered:** draw the whole frame (the sprite is wiped for you). **Direct:** keep a per-visible-slot
  signature of what is on the panel and repaint only the slots whose content or y changed, each in
  its own row box. A 1 Hz repaint of the two rows that actually changed is invisible.
- **Do not animate on the direct path.** Ease the scroll only when buffered; snap otherwise — one
  repaint per keypress instead of ten full-area clears.
- **Gate the poll on real change.** Rebuild the model in the poll handler, hash it, and ask for a
  frame only if the hash moved. An idle sheet then costs zero frames on both paths.
- **Clip to the content band.** A partially scrolled row otherwise paints into the hint bar, which
  the app never clears — the smear stays there for the rest of the session.
- **Pin the font.** The layout math assumes Font0 cells; a previous app/overlay may have left another
  font on the target, which turns the sheet into overlapping text.
- **Watch `nucleo_app_repaint_gen()`.** It bumps on `force_repaint()` and whenever the app regains
  the screen from the launcher/an overlay (voice, notification banner, Control Center). An
  incremental direct-path cache MUST invalidate then, or overlay residue stays on screen. Buffered
  apps can ignore it — the run loop already forces a full blit in the same cases.

## Checklist when adding/touching a drawing routine
- Does it run on a repeating cadence? If yes, it must use one of the techniques above.
- Is there a `fillScreen`/large `fillRect` on that cadence? If yes, that's the bug — move it
  to a one-time static pass, or switch to a sprite.
- Does it scroll text continuously? Use a marquee sprite (technique 3).
- Is the redraw gated to an actual content change (a `sig` that only flips when something the
  user can see changes)? It must be.

## The measured constraint (2026-08): the back-buffer must FIT, then it must SURVIVE

Two numbers, both measured on the live ADV, explain every flicker report this month:

- **At boot (ui-init): largest contiguous block = 31,744 B — structurally, every boot, Solo
  included.** The full 240x135@8bpp canvas needs 32,400 B, so on the ADV it had NEVER allocated:
  the board silently direct-drew from day one, and every continuously-repainting app flickered
  (and felt slow — a full-frame SPI clear+redraw per frame hogs the UI task) while every
  buffered-path improvement changed nothing, because that path was never taken.
- **Mid-session: largest block ≈ 8-12 KB.** Once the canvas is released inline (voice PTT,
  a web client, a decoder), it does not come back until reboot.

What the framework does about it:

1. **Adaptive height** (`nucleo_screen_acquire`): full 240x135 first; if that fails, fit the
   canvas to the measured block — `(largest - margin) / width` rows, refused only below the app
   content area (H-HINT). The ADV lands at 240x131: content fully covered, buffered draw real.
   Consumers adapt via `cv->height()`; `set_fullscreen` blanks the few uncovered rows once.
2. **Continuously-repainting apps run in a SOLO boot** (`NX_SOLO | NX_WIFI`): instruments
   (level, protractor, mic analyzer), the USB-keyboard typing modal, every game. Fresh heap =
   guaranteed canvas AND working room for their own buffers. Event-driven apps may stay inline.
3. **A GO-hold while an exclusive app is foreground is ignored** — voice is down (NX_VOICE), and
   the old path freed the canvas before the engine could refuse.
4. **The run loop self-heals**: an app stuck on direct draw stands the ANIMA L1 index down
   (~24 KB, lazy-reloads from SD) and retries the canvas every ~2 s.
5. **Diagnosis is one serial line**: acquire failure logs the largest free block ("canvas ...
   alloc FAILED - largest free block N"), the fitted fallback logs its size, and apps show a
   small `!D` title marker while on the direct path. Before optimizing a "flickering" app,
   CHECK THE PATH FIRST — one 30 s capture of COM4 settles it.

One more lesson from the same hunt, for sensor-driven apps: **flicker can be CONTENT.** The mic
analyzer rendered perfectly and still "flickered" because its AGC, with no absolute floor,
normalized a silent room's noise into full-scale random bars at 31 fps. A self-calibrating
noise gate (learned floor EMA; open at 3x, close at 1.8x — ratios, so it survives the ADV's
~3x-hotter mic path) freezes the frame in silence, which the seq-gated poll then never
recomposites. If a live app dances when its input is idle, gate the INPUT, not the renderer.

