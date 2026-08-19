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

**Screen — native width, no scaling.** The Game Boy is 160×144 and the panel is 240×135. Horizontally
there is nothing to solve: 160 columns fit inside 240, so every column maps to exactly one pixel,
unfiltered. (An earlier version decimated to 150 wide by dropping every 16th column. That destroyed
real detail — thin sprites lost limbs — to solve a problem the panel did not have.)

Vertically there is no such luxury: 144 lines must reach a 135 px panel, and the alternatives to
dropping 9 of them are cropping or letterboxing a screen that is already small. Every 16th line is
dropped, and it is a **drop, not a blend** — no averaging, so surviving pixels keep exactly the colours
the PPU produced. Output lines are accumulated 15 at a time and pushed in one `pushImage`: **9 SPI
transactions per frame instead of 135.**

**Where the CPU goes.** Three settings matter more than any code in the app:

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
- **Game Boy Color.** Peanut-GB is DMG-only. `.gbc` files are listed and marked, and dual-mode carts
  run in their DMG fallback, but CGB-exclusive titles will not render correctly.
- **Chip-8** is the obvious next core: ~5 KB, no licence question, and it exercises the same app
  scaffolding.
- **Measured Solo-boot free DRAM** is still unrecorded for this app. That single number decides
  whether anything heavier than Game Boy is ever worth revisiting.
