# Native emulation on the Cardputer

What the device can emulate **itself**, in firmware C, with no browser involved — and why the list is
as short as it is. For the browser-side emulator (the Arcade app, EmulatorJS on the client), see
`apps/arcade/` and its `NOTICE.md`; the two answer different questions and should not be confused.

Status: **Game Boy (DMG) ships** as the native app `gbemu`. Everything else on this page is analysis.

---

## 1. The constraint is contiguous RAM, not free RAM

The board is an **ESP32-S3FN8**: dual-core LX7 @240 MHz, **512 KB SRAM, no PSRAM**, 8 MB flash. The
missing `R` in the part number *is* the "no PSRAM" marker (`R2`/`R8` would mean 2/8 MB); this holds
for the original Cardputer **and** the ADV — no variant ships PSRAM.

Free bytes are not the budget. The budget is the largest **contiguous** block, and the device's own
boot trace (`/sd/boot_trace.txt`) shows where it goes:

| boot stage | free | largest block |
|---|---:|---:|
| `boot-start` | 125,316 | **63,488** |
| `ui-init` | 86,400 | **31,744** ← the 32,400 B shared UI canvas lands here |
| `sd-mounted` | 74,260 | 31,744 |
| Solo boot over USB, no Wi-Fi | 65,728 | 31,744 |

Two things follow, and they drive every design decision below:

1. **The 32,400 B canvas (240×135 @ 8bpp) halves the arena and it never recovers.** Releasing it
   (`nucleo_app_release_buffers()` + `nucleo_screen_release()`) gives the block back.
2. **The runtime reclaim frees RAM but cannot defragment** — `nucleo_exclusive.h` says so explicitly.
   Only a **Solo boot** starts from a clean arena. That is why the emulator app declares
   `NX_SOLO`, and `NX_WIFI` on top (the radio costs ~48 KB *and* halves the largest block — an
   emulator has no use for a network, and leaving the app reboots into the full OS, so the radio
   returns without the fragile in-place restore that once broke audio + SD on the ADV).

Two facts that are **not** constraints, contrary to reasonable assumptions:

- **The SD does not share the SPI bus with the display.** Display is on `SPI3_HOST`, SD on
  `SPI2_HOST` (`nucleo_board.h`). No contention.
- **Display bandwidth is not the ceiling.** ST7789V2 at 80 MHz with DMA is ~154 fps for a full
  240×135 frame.

## 2. Flash

`partitions.csv` allocates the full 8 MB with nothing spare (`0x760000 + 0xA0000 = 0x800000`). The
app image leaves ~750 KB of headroom inside `ota_0`, which is plenty for a core (50–150 KB of code),
but there is **no room for a ROM partition** without shrinking the OTA banks — and the file's own
header warns that a partition-table change is not OTA-safe.

That rules out the trick the reference projects use (copy the ROM to a flash partition and
`esp_partition_mmap` it, so the cartridge costs zero RAM). Our cartridges therefore stream from the
SD card instead — see §4.

## 3. What is actually feasible

RAM figures are the emulator's working set; the licence column is what decides most of them for us.

| System | Core | RAM | Licence | Verdict |
|---|---|---:|---|---|
| **Game Boy (DMG)** | **Peanut-GB** | **~17 KB** | **MIT** | 🟢 **shipped** |
| Chip-8 | write our own | ~5 KB | ours | 🟢 feasible, trivial |
| ZX Spectrum 48K | (needs a permissive Z80) | ~48 KB | varies | 🟢 feasible |
| Master System / Game Gear | smsplus | ~30–40 KB | GPL-2.0 | 🔴 licence |
| NES | nofrendo | ~80 KB | LGPL-2.0 | 🟡 relink obligation |
| Game Boy Color | gnuboy | ~105 KB | GPL-2.0 | 🔴 licence |
| PC Engine | pce-go | ~115 KB | GPL-2.0 | 🔴 licence |
| Neo Geo Pocket | RACE | ~147 KB | GPL-2.0 | 🔴 licence |
| Mega Drive / WonderSwan | — | — | — | 🔴 too slow |
| SNES | — | — | — | 🔴 not enough RAM |

**The binding constraint above Game Boy is legal, not technical.** NucleoOS is **PolyForm
Noncommercial 1.0.0**, which imposes a use restriction that GPL-2.0 forbids adding — so the GPL
cores cannot be linked into this firmware, however well they would fit. nofrendo (LGPL) is linkable
in principle but static linking triggers the relink obligation, which is awkward for a project that
publishes binaries through GitHub Releases and a web flasher.

So Game Boy was not merely the best first target: it is the only clean one.

## 4. How the Game Boy app works

**Core.** Peanut-GB, vendored verbatim at `firmware/components/nucleo_emu/vendor/peanut_gb.h`
(revision recorded in the README beside it). Two properties make it fit:

- `sizeof(struct gb_s)` = **16,952 B** — 8 KB WRAM + 8 KB VRAM + 160 B OAM + 256 B HRAM/IO + state.
  Measured, not estimated; the host gate asserts it so a future update cannot quietly double it.
- **No framebuffer.** The PPU calls `lcd_draw_line(gb, pixels, line)` once per visible line, so the
  display costs a 160-byte line buffer instead of the 23–46 KB a frame would.
- **Zero mutable statics** → 0 bytes of `.bss`, which is exactly the heap-on-enter rule in
  `docs/memory-budget.md`.

**Cartridge — the page cache.** `gb_rom_read()` is a host callback, so the ROM need not be resident.
A 32 KB cartridge is loaded outright when the largest free block can take it with 24 KB of headroom;
everything bigger is **paged from the card**, because a 128 KB ROM would need 152 KB contiguous on a
board whose largest block is ~60 KB even after a Solo boot.

The geometry of that cache decides whether the emulator runs. It was **measured, not chosen** — the
host gate sweeps it against real cartridges at a fixed 40 KB budget. Misses per frame over 15 seconds
of emulated gameplay, on the three worst titles in the library:

| Geometry (all = 40 KB) | Zelda (512 KB) | Metroid II (256 KB) | Kirby's Block Ball (512 KB) |
|---|---|---|---|
| 8 KB × 5 slots  | 6.0 | 5.5 | 12.9 |
| 4 KB × 10 slots | 6.4 | 3.7 | 4.2 |
| 2 KB × 20 slots | 0.1 | 0.4 | 3.0 |
| **1 KB × 40 slots** | **0.1** | **0.1** | **0.2** |

Same RAM, sixty times fewer reads. A miss is one SD read at roughly 2–4 ms against a 16.7 ms frame,
so the 4 KB configuration spent **~19 ms per frame** waiting on the card in Zelda — more than a whole
frame, which is precisely what "the emulator lags" meant.

Why small pages win: a game's working set is *scattered* — a few hundred live bytes in each of many
places — so a large page spends most of its bulk on bytes nobody asked for, and a fixed budget cut
into few large pages covers few regions. Cut finely, it covers many. Below 1 KB the curve flattens
and the bookkeeping doubles; 1 KB is also two whole SD sectors, so a refill is one aligned read.

The cache is fully associative with LRU (direct mapping would collide bank 0 — live every frame —
against every bank sharing its low bits), fronted by a one-compare fast path for the current page and
a 256-byte direct-mapped hint table so a page *change* costs one probe rather than a 40-slot scan.
`nucleo_gb_get_stats()` reports the path taken, the slot count, and the miss count.

**Palette.** Four, and the default is the Game Boy's own green — *calibrated*, not copied. There are
two defensible "original" palettes and neither transplants cleanly onto this panel:

- `#9BBC0F #8BAC0F #306230 #0F380F` — the literal colours of the DMG's reflective LCD
- `#E0F8D0 #88C070 #346856 #081820` — what emulators have drawn a Game Boy as for decades

The literal set was designed to be read by **ambient light** bouncing off a passive panel. Emitted by
a backlit ST7789, its two dark shades land near black: mid-tones stop reading as green and the picture
goes muddy. The emulator-standard set fixes that by going mint, and stops looking like a Game Boy.

So the default keeps the two light shades exactly as the hardware had them — `#9BBC0F` is the colour
everyone pictures — and lifts only the two dark ones, which is precisely the work ambient light used
to do. Shade 1 is nudged down at the same time, because the real panel's top two shades are nearly
indistinguishable and a 135 px screen cannot afford to waste a whole shade. The alternatives are the
emulator-standard mint, maximum contrast for a bright room, and amber for night. `P` cycles them live.

**Screen — native width, no scaling.** The Game Boy is 160×144 and the panel is 240×135. Horizontally
there is nothing to solve: 160 columns fit inside 240, so every column maps to exactly one pixel,
unfiltered. (An earlier version decimated to 150 wide by dropping every 16th column. That destroyed
real detail — thin sprites lost limbs — to solve a problem the panel did not have.)

Vertically there is no such luxury: 144 lines must reach a 135 px panel, and the alternatives to
dropping 9 of them are cropping or letterboxing a screen that is already small. Every 16th line is
dropped, and it is a **drop, not a blend** — no averaging, so surviving pixels keep exactly the colours
the PPU produced. Output lines are accumulated 15 at a time and pushed in one `pushImage`: **9 SPI
transactions per frame instead of 135.**

**Controls — the "lag" that was not a frame-rate problem.** The first version built the button mask
from `nucleo_kbd_read()`, which reports a printable key **once per press and never repeats**. A held
direction therefore released itself on the very next frame: the character took one step and stopped,
and two buttons could never be down at once — no running jump, no diagonal. It read as the emulator
being slow. It was not; the D-pad was tapping itself.

The mask is now built from `nucleo_kbd_char_down()` — the live pressed set, rebuilt from the matrix on
every scan — so directions hold and chords work. Two d-pads are live at once: **E/S/A/D** as a movement
diamond under the left hand, and the **`;` `.` `,` `/`** cluster the Cardputer literally prints arrows
on under the right. **K** = A, **J** = B, **Enter** = Start, **Space** = Select, **TAB held** =
fast-forward, **P** = palette, **M** or **Esc** = the in-game menu (save state, load state, palette,
picture, quit).

Esc opening a *menu* rather than quitting outright is deliberate: leaving a game means losing the
session, because the app runs in a Solo boot and exiting reboots. The key a player hits by reflex must
not be the destructive one.

**Where the CPU goes.** Four settings matter more than any code in the app:

- **`-O2`, scoped to `nucleo_emu`.** The firmware builds at `-Os` because flash is the scarce resource
  across ~60 components, but the emulator is the one place with a hard real-time deadline. The
  exception is declared in the component's own `CMakeLists.txt` and costs a few KB of flash, not the
  ~250 KB a global `-O2` would.
- **`IRAM_ATTR` on the five core callbacks** (`rom_read`, the two APU shims, the two cart-RAM
  accessors, the scanline hand-off). These run millions of times a second; left in flash they are
  fetched through a 16 KB instruction cache that the core's own dispatch switch is already thrashing,
  at 80 MHz **DIO**.
- **`PEANUT_GB_HIGH_LCD_ACCURACY 0`.** It re-sorts sprites by X on every scanline to reproduce the
  DMG's priority rule — 144 sorts per frame for a difference most games never show.
- **`PEANUT_GB_12_COLOUR 0`.** It tags every emitted pixel with which palette produced it, so a
  front-end can give a DMG game the different colours a Game Boy Color would. This is a DMG core on a
  four-shade screen and our callback masks the tag straight off again: an extra OR per pixel, 23,000
  a frame, for information nobody reads.

**Adaptive relief, and why it is one-way.** If the picture still cannot be produced sixty times a
second, the app produces *less* of it rather than letting every frame arrive late: **30 fps
frame-skip**, the core's own lever, which the CPU runs through every frame so timing and input stay
exact — only the drawing thins out. The current level shows in the right pillar and can be raised by
hand from the in-game menu.

It engages after two consecutive slow seconds and then **stays**. It does *not* step back up
automatically, and that is not laziness — it is the fix for a flicker. Frame-skip halves the blit
work, so the moment it engages the measured fps jumps back up; a symmetric "recover when fps is high
again" rule therefore switches relief off, fps falls, relief re-engages, and the whole thing toggles
once a second — 60 fps and 30 fps in alternation, which the eye reads as flicker. The metric was
feeding back into the thing it measured. A one-way control has no such loop.

(The first version also had a second lever, **interlacing**, tried before frame-skip. It was removed:
the core emits alternate scanlines under interlace, but this renderer counts an output row per
scanline *received*, so with half the lines arriving the picture only reached the middle of the panel
and the bottom half stopped updating — a visible split, not relief.)

**No tearing, no clear-then-draw — the two flicker rules this app has to obey.** The board has no
PSRAM for a full framebuffer and the panel has no hardware double-buffer, so the picture is drawn
straight to the ST7789 (`docs`/`../firmware/components/nucleo_app/ANTI-FLICKER.md`). Two consequences:

- **One SPI transaction per frame.** `run_frame` calls the scanline callback 135 times and each pushes
  its band; the whole sweep is wrapped in a single `startWrite`/`endWrite`, so the nine bands stream to
  the panel with no bus hand-off between them instead of nine separately-arbitrated writes. The display
  is on SPI3 and the SD card on SPI2, so holding the display bus across a frame contends with nothing.
- **The marquee never clears its own box on the animating path.** A scrolling line overruns its box, so
  the clipped opaque glyphs already cover every pixel — the per-frame `fillRect` a naïve marquee does
  first would be a whole frame of bare background, which is exactly the clear-then-draw flicker. The
  `fillRect` survives only on the static (fits-in-the-box) path, where it runs once and never repeats.

**Scrolling text costs a sprite, not a repaint.** Getting the shelf's long game titles to scroll took
three attempts, and the two failures are the instructive part — both produced *the same pair of
symptoms*, "the list flickers" and "the text doesn't move", from one cause each:

1. Animating from `on_tick`. The tick runs with the gfx pointed at the **panel**, not at the shared
   canvas a buffered app's `on_draw` composites into — so the text landed in the wrong buffer at the
   wrong coordinates and the next push painted over it. Nothing moved.
2. Animating by calling `nucleo_app_force_repaint()` on a timer. That re-runs the whole `on_draw`, and
   in the Solo boot the shelf draws **direct to the panel** with no canvas — so five times a second the
   entire list was cleared and redrawn. That was the flicker, and the flashing masked the motion, which
   is why it still looked frozen.

The working answer is ANTI-FLICKER.md technique 3, the same one `app_player`'s Now-Playing title uses:
the scrolling line gets its **own small off-screen sprite**, blitted in one `pushSprite` over just its
row from a 50 Hz `nucleo_app_set_poll_handler`. The panel never sees a clear, nothing else on screen is
touched, and the motion is smooth instead of a 5 Hz stutter. Two copies of the text are drawn one gap
apart so the loop never shows a seam. A title that fits is drawn once, statically, and the poll ignores
it. **The general rule: never drive an animation by repainting a view — give the moving part a sprite.**

**Save states.** The console is one struct with no external references except our own callbacks, so a
state is a raw dump of it plus the cartridge RAM, written beside the ROM as `<rom>.st0`. Two things
matter and both are enforced: the six function pointers in the struct are restored from the **live
session**, never from the file (a state off an SD card must not be able to choose an address for the
CPU to call), and the cartridge RAM is read into scratch first so a truncated file leaves the running
game untouched instead of corrupting it. The host gate verifies the round-trip by replaying the
emulator against itself and comparing a hash of every pixel drawn — see below.

**The measurement is on screen.** The two 40 px pillars either side of a 160 px picture are not
padding; they are the only place a running emulator can speak without covering the game. Left: frame
rate, palette, the keys that are not guessable. Right: where the frame actually went — `c` CPU, `b`
blit, `a` audio, in tenths of a millisecond — plus the relief level. The same line goes to
`/gbemu_trace.txt` once a second, so a session can be diagnosed after the fact from the card.

**Saves.** Cartridge RAM is persisted to `<rom>.sav` beside the ROM, written on close.

**Speed.** The same core runs at >70 fps on a 133 MHz Cortex-M0+ (RP2040-GB), so a 240 MHz LX7 has
ample headroom *on paper*. In practice the budget is spent long before the CPU is: see "Where the CPU
goes" above, and the page cache before that. On the host gate the core runs at 6,000–11,000 fps.

**What the app switches off.** The emulator declares `NX_NET_APP | NX_SOLO | NX_WIFI`, which is not a
runtime reclaim but a *boot-time* decision: opening it reboots into a Solo session where `main.c`
never starts Wi-Fi (~48 KB, and it halves the largest contiguous block), httpd, mDNS, the recorder,
auth or IR — and the heap comes up unfragmented, which is the only reason 40 KB of page cache plus a
17 KB core fits at all. `nucleo_power_perf_begin()` additionally pins the CPU at 240 MHz for exactly
as long as a game runs, because DFS cannot tell a render loop from an idle launcher. The audio meter
in `nucleo_audio` is demand-gated, so it costs nothing unless a visualiser is actually on screen.

**The shelf.** The library on the card is ~4,700 Game Boy cartridges — 300 KB of names if held in
RAM, which is impossible. The browser therefore never holds the whole library: it re-walks the
directory against a live filter and keeps at most 120 matches, reporting `shown/total` honestly.
Type-to-filter is the navigation model, not a convenience, which is also the right answer on a device
with a real keyboard (see `docs/device-ui.md`). Digits 1–9 pick a visible row directly; the last
cartridge played is remembered and pre-selected on the next open.

## 5. Verifying it

Host-first, per `CLAUDE.md` — the core and the host callbacks are proven on the PC before anything
reaches a board:

```bash
npm run gb:test
```

It runs **two** gates. The first compiles the vendored core with the firmware's build switches,
asserts the struct size, then boots ROMs from `tools/sd-sim/data/ROMs/gb|gbc` and runs 180 frames of
each, checking that the PPU emits scanlines and that they are not all blank.

The second compiles **`nucleo_gb.c` itself** — the file that decides where a cartridge lives — and
drives its public API for 900 frames while mashing START and A to get past the title screen into real
gameplay. `NUCLEO_HOST_HEAP` shrinks the heap the module believes it has to the ~60 KB a Solo boot
actually offers, so the SD-paged path is the one under test, and it fails the build if any cartridge
exceeds 1.4 misses per frame.

It also verifies **save states by replaying the emulator against itself**: run to a fixed point, save,
run 120 more frames on a fixed input sequence while hashing every pixel of every scanline, load the
state, replay the identical 120 frames, and require the two hashes to match bit for bit. "It loaded
without crashing" tests nothing — a state that drops the PPU registers or a timer still loads, still
runs, and quietly plays a different game. Divergence in the picture is the only check that catches it.

That second gate exists because of a specific failure: the first one supplies its own callbacks, so
it never touched `nucleo_gb.c` at all — and it stayed green through a session in which the device
answered "invalid ROM" for every cartridge. A gate that cannot fail the way the product fails is not
a gate. It is also the rig that produced the geometry table above; the page size and slot count are
overridable at compile time *only* so it can sweep them.

One invariant that looks obvious and is **wrong**: "144 scanlines per frame". A game may switch the
LCD off (LCDC bit 7) during boot logos or VRAM setup, and the PPU legitimately emits nothing while it
is off. The gate therefore requires most of the expected lines, not all of them — the first version
demanded exactness and failed every healthy ROM.

## 6. Open questions

- **Flash mode is DIO, not QIO.** Every instruction-cache miss is a 2-bit-wide read at 80 MHz; QIO
  would double that bandwidth for free in RAM terms. It is not enabled because a wrong guess about the
  module's flash chip is a serial-recovery brick, so it needs a deliberate decision and a device to
  test on — not a silent config change.
- **Instruction cache is 16 KB.** 32 KB would help the core's large dispatch switch measurably, but
  the cache is carved out of the same internal SRAM the emulator is already fighting for.
- **The carousel entry shows the console, never a screenshot.** GameFront captures a cover per game,
  but an emulator's entry is not a game — it is a console with hundreds of cartridges behind it, so a
  picture of whatever ran last misrepresents the whole shelf and the next cartridge makes it wrong
  again. `gf_never_shot()` in `gamefront.cpp` routes those ids to the procedural poster (which draws
  the Game Boy icon) and makes cover capture *refuse* rather than write a file the renderer ignores.
- **Game Boy Color.** Peanut-GB is DMG-only. `.gbc` files are listed and marked, and dual-mode carts
  run in their DMG fallback, but CGB-exclusive titles will not render correctly.
- **Chip-8** is the obvious next core: ~5 KB, no licence question, and it exercises the same app
  scaffolding.
- **Measured Solo-boot free DRAM** is still unrecorded for this app. That single number decides
  whether anything heavier than Game Boy is ever worth revisiting.
