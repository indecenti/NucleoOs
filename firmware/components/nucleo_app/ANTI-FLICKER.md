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

## Checklist when adding/touching a drawing routine
- Does it run on a repeating cadence? If yes, it must use one of the techniques above.
- Is there a `fillScreen`/large `fillRect` on that cadence? If yes, that's the bug — move it
  to a one-time static pass, or switch to a sprite.
- Does it scroll text continuously? Use a marquee sprite (technique 3).
- Is the redraw gated to an actual content change (a `sig` that only flips when something the
  user can see changes)? It must be.
