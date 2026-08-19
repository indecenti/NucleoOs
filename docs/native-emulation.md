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

**Cartridge.** `gb_rom_read()` is a host callback, so the ROM need not be resident. The app picks a
strategy per cartridge: if the whole ROM fits in the largest free block with 24 KB of headroom it is
loaded outright (no SD traffic at all while playing); otherwise it keeps the fixed 16 KB bank 0
resident plus one 16 KB switchable-bank window, refilled from SD on a miss. `nucleo_gb_get_stats()`
reports which path was taken and how many misses occurred.

**Screen.** The Game Boy is 160×144 and the panel is 240×135, so no integer scale fits. The app
decimates by exactly **15/16 on both axes** — drop every 16th column and every 16th line — giving
150×135 with the aspect preserved to within a pixel, and the test is one bitmask rather than a divide
per pixel. Output lines are accumulated 15 at a time and pushed in one `pushImage`: **9 SPI
transactions per frame instead of 135.**

**Saves.** Cartridge RAM is persisted to `<rom>.sav` beside the ROM, written on close.

**Speed.** The same core runs at >70 fps on a 133 MHz Cortex-M0+ (RP2040-GB), so a 240 MHz LX7 has
ample headroom. On the host gate it runs at 6,000–11,000 fps. There is **no audio yet**: Peanut-GB
ships no APU.

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

It compiles the vendored core with the firmware's build switches, asserts the struct size, then boots
every ROM it finds in `tools/sd-sim/data/ROMs/gb|gbc` and runs 180 frames of each, checking that the
PPU emits scanlines and that they are not all blank.

One invariant that looks obvious and is **wrong**: "144 scanlines per frame". A game may switch the
LCD off (LCDC bit 7) during boot logos or VRAM setup, and the PPU legitimately emits nothing while it
is off. The gate therefore requires most of the expected lines, not all of them — the first version
demanded exactness and failed every healthy ROM.

## 6. Open questions

- **Audio.** Peanut-GB has no APU. Adding one (minigb_apu or equivalent) needs its own buffers and an
  I2S owner, and must be checked against the RAM budget above.
- **Game Boy Color.** Peanut-GB is DMG-only. `.gbc` files are listed and marked, and dual-mode carts
  run in their DMG fallback, but CGB-exclusive titles will not render correctly.
- **Chip-8** is the obvious next core: ~5 KB, no licence question, and it exercises the same app
  scaffolding.
- **Measured Solo-boot free DRAM** is still unrecorded for this app. That single number decides
  whether anything heavier than Game Boy is ever worth revisiting.
