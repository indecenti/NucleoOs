# Native emulation on the Cardputer

What the device can emulate **itself**, in firmware C, with no browser involved — and why the list is
as short as it is. For the browser-side emulator (the Arcade app, EmulatorJS on the client), see
`apps/arcade/` and its `NOTICE.md`; the two answer different questions and should not be confused.

Status: **Game Boy (DMG) ships** as the native app `gbemu` — a patched Peanut-GB core held to the
blargg/mooneye/acid2 reference suites by a ratchet gate (§5). Everything else on this page is analysis.

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
| `gbemu` Solo boot, canvas released (`/gbemu_trace.txt`) | ~90,000 | **32,768** |

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
| Game Boy Color | Walnut-CGB | ~49 KB (one block) | MIT | 🟡 RAM shape + CPU — see §6 |
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

**Core.** Peanut-GB at `firmware/components/nucleo_emu/vendor/peanut_gb.h`: upstream (revision in
the README beside it) plus eight marked accuracy patches, `NUCLEO PATCH (P1–P8)`, each justified by a
test ROM that failed without it and listed in `vendor/README.md` with a re-appliable
`peanut_gb.nucleo.patch`. Three properties make the core fit:

- `sizeof(struct gb_s)` = **16,952 B** — 8 KB WRAM + 8 KB VRAM + 160 B OAM + 256 B HRAM/IO + state.
  Measured, not estimated; the host gate asserts it, and no patch changed it (P1 reuses two spare
  bits of an existing flag byte).
- **No framebuffer.** The PPU calls `lcd_draw_line(gb, pixels, line)` once per visible line, so the
  display costs a 160-byte line buffer instead of the 23–46 KB a frame would.
- **Zero mutable statics** → 0 bytes of `.bss`, which is exactly the heap-on-enter rule in
  `docs/memory-budget.md`.

### 4.1 Accuracy — what was wrong, and how it is held right

Measured against the reference suites (blargg, mooneye-test-suite, dmg-acid2 — §5). The core that
shipped before this work passed **48 of 119**; the patched core passes **66** — including the
dmg-acid2 picture pixel-for-pixel, blargg's `halt_bug`, and 8 of mooneye's 11 timer tests (0
before). The failures that are left are cycle-exact tests (M-cycle memory timing, OAM DMA timing, PPU mode-3
length) that a line-based, instruction-granular core cannot pass by construction; no game in the
library depends on them.

What the fixes mean on a real cartridge:

| Symptom a player saw | Cause | Fix |
|---|---|---|
| MBC2 games (Kirby's Pinball Land, Konami Golf, Final Fantasy Legend) never kept a save | init read the header's "0 RAM banks" — MBC2's RAM is inside the controller — and switched cart RAM **off** | P5 |
| Some games dropped to half speed in places, or missed an interrupt | `EI` took effect immediately, so `EI; HALT` with an interrupt pending serviced it *before* the HALT and then waited a whole extra interrupt | P2 |
| Rare lock-ups / wrong code paths after HALT | the HALT bug was not emulated | P3 |
| Music tempo / RNG drift in games that resync on DIV or TAC | DIV and TIMA were independent counters | P8 |
| 1–2 MB MBC1 cartridges crashed after the first level | the high bank bits were ignored in mode 1 | P6 |
| Sprites drawn on top of the wrong sprite (flicker-swap on overlaps) | DMG sprite priority was switched off for speed (`PEANUT_GB_HIGH_LCD_ACCURACY 0`) — dmg-acid2 showed 40 wrong pixels | switched **on**; the jump-table dispatch (§4.6) pays for it |
| A stray invalid opcode could take the device with it | upstream treats the error hook as `__builtin_unreachable()`; ours returns | P4, plus the crash menu (§4.4) |

**The header is corrected, not trusted** (`header_fix` in `nucleo_gb.c`, no core patch needed): a
stale header checksum no longer refuses the cartridge (prototypes, fan translations, homebrew — the
device trace shows *Adventures of Pinocchio (Proto)* failing exactly this way); an out-of-range
ROM-size byte is derived from the file size instead of indexing past the core's table; HuC1 runs as
the MBC1 it is. These corrected bytes are served **only while `gb_init()` runs** — the running game
reads its real header, so titles that verify their own checksum still see the original.

**Refused in words, before the UI is torn down.** `nucleo_gb_probe()` reads the header without
allocating anything, so the shelf refuses a Game Boy Color–only cartridge ("Game Boy Color only") or a
controller the core has no model of ("Unsupported cartridge: MBC7") instantly, instead of booting it
into garbage or answering "Invalid ROM". GBC-*enhanced* cartridges (header `0x80`) still run in their
DMG mode.

### 4.2 Memory — the heap has a shape, not a size

The emulator's own trace (`/gbemu_trace.txt`) is the ground truth, and it corrected an assumption this
page used to state: in the Solo boot, after the UI canvas is released, the heap has **~90 KB free but
its largest block is 32 KB**, not ~60 KB. Everything below is designed against that shape, and the host
gate now *models* it (§5) instead of trusting a PC heap that always has a 32 KB block to give.

Where the RAM goes while a game runs, and what changed:

| | before | now |
|---|---:|---:|
| core + APU + session | ~21 KB | ~21 KB |
| ROM page cache (1 KB pages, greedy) | 40 pages (device trace) | **53 pages** on the modelled heap (61 with no battery RAM) |
| battery RAM, 8 KB cart | 8 KB | 8 KB |
| battery RAM, 32 KB cart (Pokémon) | **could not start** — one 32 KB block, asked for after the cache | **2 × 8 KB resident**, 45 cache pages, 0.2 SD reads/frame |
| floor kept for runtime file handles | 16 KB | 8 KB (the speaker's DMA channel now opens *before* the cache) |
| shelf name list during play | ~6.8 KB resident | swapped to `/system/config/gbemu.shelf` |
| shelf title-scroller sprite during play | ~6.3 KB resident | freed |

**Battery RAM lives in 8 KB banks, and at most two of them in RAM.** A cartridge with more (Pokémon
Red/Blue/Yellow/Gold/Silver carry 32 KB; MBC5 allows 128 KB) keeps the other banks in
`<rom>.sav.swp`, brought in over the least-recently-used slot on a bank switch. That is cheap exactly
where it matters: Pokémon decompresses sprites in SRAM bank 0 constantly, writes its save to bank 1,
and touches banks 2–3 only when the player changes PC box — a swap is an event, not a per-frame cost.
mooneye's `mbc1/ram_256kb`, which walks all four banks, passes through this path on the modelled heap
(18 swaps). On disk the RAM is always one flat image, byte-compatible with every other emulator's
`.sav`.

**Page cache geometry** (unchanged, still measured): 1 KB pages, fully associative LRU, one-compare
fast path plus a 256-byte hint table. The sweep that chose it, at a fixed 40 KB budget:

| Geometry (all = 40 KB) | Zelda (512 KB) | Metroid II (256 KB) | Kirby's Block Ball (512 KB) |
|---|---|---|---|
| 8 KB × 5 slots  | 6.0 | 5.5 | 12.9 |
| 4 KB × 10 slots | 6.4 | 3.7 | 4.2 |
| 2 KB × 20 slots | 0.1 | 0.4 | 3.0 |
| **1 KB × 40 slots** | **0.1** | **0.1** | **0.2** |

A game's working set is scattered — a few hundred live bytes in many places — so small pages cover
more of it for the same RAM. The curve has a knee: Kirby's Block Ball reads 0.2 misses/frame at 45
pages and 3.9 at 29, which is why every recovered KB above was worth chasing.

### 4.3 Saves that survive a flat battery

- **Autosave.** Battery RAM used to reach the card only when the emulator closed — a crash or a flat
  120 mAh cell threw the session's progress away. It is now written once the game has left its RAM
  alone for ~1.5 s (i.e. a save screen has finished), and whenever the in-game menu opens (a pause,
  so the SD write costs nothing visible). A game that uses battery RAM as scratch every frame never
  goes quiet, so it cannot turn this into an SD write per second.
- **Atomic.** Written to `<rom>.sav.tmp`, closed (a failed `fclose` is a failed write — FATFS flushes
  there), then renamed over `.sav`. FATFS cannot rename onto an existing name, so the old file is
  removed in between; a `.tmp` left by exactly that gap is promoted on the next load or save, never
  overwritten.
- **A crashed console never saves.** After an invalid opcode the CPU is running data, and anything it
  wrote to battery RAM is garbage. Saving is suppressed; *Reset* reloads battery RAM from the card.

### 4.4 Save states, reset, crash

- **States work on every cartridge.** The state path was derived from the `.sav` path, which exists
  only for carts with battery RAM — Tetris, Super Mario Land and every RAM-less cart wrote to a bare
  `.st0` and silently failed. The session now keeps the ROM path.
- **Loading no longer overflows the stack.** Every game runs on the Solo task's **8 KB** stack, and the
  loader verified a state by reading it into a `struct gb_s` on the stack — 17 KB. It now validates
  the file (magic, RAM size, exact length), writes the running console to `<rom>.stu`, and reads the
  state straight into the live struct; a read that fails half way restores the `.stu`. Every function
  pointer in the struct — the serial and boot-ROM hooks included, which the core calls whenever they
  are non-NULL — comes from the live session, never from the file.
- **Reset** (menu) restarts the cartridge; battery RAM is kept (flushed first, or reloaded from the
  card after a crash). The APU is re-initialised by hand — `gb_reset()` leaves it alone.
- **Crash.** On the first invalid opcode the game pauses into the menu, titled
  "CPU crashed @PC — Reset?". The *Resume* row is preselected: a mashed button must never reset.
- **MBC3 clock.** Seeded from the system clock (kept across the Solo reboot) when it is plausibly set.
  The day counter runs from a fixed epoch, so it only ever moves forward — the core's own
  `gb_set_rtc()` uses the day-of-year, which falls back to 0 every New Year and would tell a game time
  ran backwards.

### 4.5 Screen

**Native width, no scaling horizontally.** 160 columns fit inside 240, so every column is one pixel.
Vertically 144 lines must reach 135: one line in sixteen has to go. By default it is **blended** into
the line below it — each pixel the exact midpoint of the two palette colours (`MIX_TX`, precomputed
per palette, panel byte order) — so a row of every 8×8 glyph, a 1 px ledge, the top of a sprite never
simply vanish. *Lines: Sharp* in the menu restores the pure drop for players who want only original
colours. The merged lines are 7, 23, … 135: every one has a successor, so there is no end case.
Nine rows a frame take the blend path; the other 126 stay on the flat lookup.

Palettes (Green calibrated for a backlit panel, DMG mint, Mono, Amber), *Screen: Filled* (240 px,
exact 2:3), the HUD pillars, the one-SPI-transaction-per-frame DMA band pipeline and the flicker rules
are unchanged from the first version — see the comments in `app_gbemu.cpp`.

### 4.6 Where the CPU goes

**The device is not instruction-bound, it is instruction-*cache*-bound.** `tools/emu-host/qemu-bench`
runs the core on Espressif's ESP32-S3 QEMU with `-icount` (one instruction = one virtual ns). Street
Fighter II costs **~0.8 M instructions per frame** — 3.4 ms at 240 MHz at one instruction per cycle —
while the device measures **15 ms** for the same frame. The difference is the 16 KB instruction cache
refilling from 80 MHz DIO flash (~450 CPU cycles per 32-byte line) under a hot path that barely fits
in it. Two consequences steer every change here: fewer instructions help, *smaller hot code* helps
more, and RAM spent on IRAM would be paid by every other app.

| Variant (6 cartridges, k-instructions/frame, summed) | total | `__gb_step_cpu` |
|---|---:|---:|
| as shipped before (upstream core, sprite priority off, no jump tables) | 7,960 | 14,452 B |
| patched core + sprite priority, no jump tables | 8,211 | ~14.4 KB |
| **patched core + sprite priority + jump tables (now)** | **7,394** | **12,465 B** |
| same at `-Os` | 8,350 | 10,601 B |

**Jump tables** are the lever. ESP-IDF adds `-fno-jump-tables -fno-tree-switch-conversion` to every
file (a table in flash would break code that runs from IRAM with the cache off), which turns the
256-way opcode switch, the CB-prefix switch and the memory-map switch in every read and write into
binary trees of ~8 compare-and-branches. `nucleo_emu/CMakeLists.txt` turns them back on for this
component only — nothing in it runs with the cache disabled. Result: −10 % instructions *and* −14 %
hot code, which is what pays for sprite priority and the accuracy patches with room to spare. `-O2`
stays: `-Os` saves 1.9 KB of hot code for +13 % instructions.

Also measured and kept: `-O2` scoped to the component, `IRAM_ATTR` on the five callbacks,
`PEANUT_GB_12_COLOUR 0`, the CPU pinned at 240 MHz while a game runs, one-way 30 fps relief.

**The on-card trace is aggregated**: one line per 10 s of play (average and minimum fps, cost
breakdown, SD misses, crash address) instead of an fopen/append/fclose every second — each of which
was a FAT update on the SD bus mid-frame and a card wake on a 120 mAh battery.

### 4.7 Controls, shelf, menu

Controls are built from the keyboard's live pressed set (holds and chords work): **E/S/A/D** or the
printed **`;` `.` `,` `/`** arrows, **K** = A, **J** = B, **Enter** = Start, **Space** = Select, **TAB**
held = fast-forward, **P** palette, **I** HUD, **−/=** volume, **M/Esc** menu. Keys still held when the
menu closes stay out of the game until released — picking *Resume* with Enter used to press Start.

The shelf filters ~4,700 cartridges live and holds at most 120 matches. Names are stored whole in a
packed pool: the old fixed 56-byte slots truncated long No-Intro names ("…Defender & Joust (USA,
Europe).g") and play() then looked for a file that did not exist — "ROM not found" on a cartridge
sitting right there (device trace). A failed start now stays on screen until the next key; before, the
shelf's own repaint wiped the box before anyone could read it.

Menu: Resume · Save state · Load state · Volume · Screen · **Lines (blended/sharp)** · On-screen info ·
Palette · Picture (30 fps relief) · **Reset game** · Quit. 1–9 pick rows directly.

## 5. Verifying it

Host-first, per `CLAUDE.md`:

```bash
npm run gb:test
```

Three parts:

1. **Render** — the vendored core with the firmware's switches boots ROMs from
   `tools/sd-sim/data/ROMs/gb|gbc`, asserts `sizeof(struct gb_s)`, and checks the PPU really emits
   non-blank lines (most of them — a game may switch the LCD off legitimately).
2. **The real module on a device-shaped heap** — `nucleo_gb.c` itself, compiled with
   `tools/emu-host/heap_model.h` force-included so its `malloc`/`calloc`/`free` go through a best-fit
   model of the Solo-boot heap (largest block 32 KB) and `heap_caps_*` read the same model. It opens
   the biggest cartridges, a synthesised 32 KB-SRAM variant (Pokémon's shape), and mooneye's
   `mbc1/ram_64kb` + `ram_256kb` through the banked-swapped SRAM path; it asserts misses/frame under
   1.4, that open/close returns every byte to the heap, that a GBC-only cartridge is refused, and that
   save states round-trip **bit-exactly** (replay the same 120 frames from the state and compare a hash
   of every pixel — "it loaded" tests nothing).
3. **Accuracy ratchet** — `npm run gb:accuracy` (also standalone) runs blargg `cpu_instrs`,
   `instr_timing`, `halt_bug`, dmg-acid2 (expected hash derived from the reference PNG, not from our
   output) and every DMG-compatible mooneye `acceptance/` and `emulator-only/mbc1,2,5` test. The ROMs
   are fetched once, SHA-256-pinned, into `tools/emu-host/testroms/` (gitignored). The gate fails if
   anything in `gb-accuracy-expected.json` stops passing; `--update` locks newly passing tests in.
   Currently **55 of 98** in that set.

And, for cost rather than correctness, `tools/emu-host/qemu-bench/run.ps1` (§4.6).

One invariant that looks obvious and is **wrong**: "144 scanlines per frame". A game may switch the
LCD off (LCDC bit 7) during boot logos or VRAM setup, and the PPU legitimately emits nothing while it
is off. The render gate requires most lines, not all.

## 6. Open questions

- **Flash mode is DIO, not QIO — now the biggest lever left.** §4.6 shows the emulator is bound by
  instruction-cache refills from flash, and QIO would roughly halve every refill for zero RAM. It
  needs the *bootloader* rebuilt and written over serial (OTA does not carry it), and a wrong guess
  about the module's flash is a serial-recovery session — a deliberate decision with a device on the
  bench, not a config change to make blind.
- **Instruction cache is 16 KB.** 32 KB would likely help more than anything in code, but costs 16 KB
  of SRAM for every app, always.
- **Measure on the device.** The instruction counts are exact; the cache effect is inferred from one
  device number (SF2: 15 ms vs 3.4 ms of instructions). The aggregated trace line reports `cpu=` per
  10 s — compare it before/after this change on the same cartridge.
- **Game Boy Color.** [Walnut-CGB](https://github.com/Mr-PauI/Walnut-CGB) (MIT) passes cgb-acid2 and
  is the only licence-clean CGB core found. Measured on the host gate it gives no DMG accuracy gain,
  and its console struct is ~49 KB with 32 KB WRAM + 16 KB VRAM in one block — on a heap whose largest
  block is 32 KB, and with double-speed CGB titles needing twice the CPU the DMG core already spends.
  It would need the WRAM/VRAM split into 4–8 KB banks (the SRAM banking above is the pattern) and the
  flash/cache work first. GBC cartridges are covered today by the browser Arcade.
- **The carousel entry shows the console, never a screenshot** — `gf_never_shot()` in `gamefront.cpp`.
  Its procedural poster is the console glyph alone: the title is printed once, under the cover, like
  every other game (it used to be printed a second time inside the poster).
- **Chip-8** is the obvious next core: ~5 KB, no licence question, same app scaffolding.
