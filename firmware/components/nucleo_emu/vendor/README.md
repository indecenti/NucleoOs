# Vendored emulator cores

## peanut_gb.h — Game Boy (DMG)

- Upstream: <https://github.com/deltabeard/Peanut-GB>
- Base revision: `d0bcca771c83a2638c93a9ae61f3d51f226dc905` (2026-09-21, includes the JOYP bit 6/7 fix)
- Licence: MIT (see the header block inside the file)
- **Upstream + a short list of local patches**, every one marked in the source with
  `NUCLEO PATCH (Pn)` and collected in [`peanut_gb.nucleo.patch`](peanut_gb.nucleo.patch).
  To update: take the new upstream file, `git apply` the patch (or re-apply by hand where it
  no longer fits), then run `npm run gb:test` — the accuracy gate below tells you whether a
  patch became unnecessary or broke.

Configuration still happens through the `#define`s the including translation unit sets before
`#include` and through the callbacks the host installs (`../nucleo_gb.c`). The patches change
behaviour only where upstream is measurably wrong, and none of them changes `sizeof(struct gb_s)`
(16,952 B — the host gate asserts it).

### Local patches

Each one exists because a test ROM or a real cartridge failed without it. "Before → after" is the
host accuracy gate (`tools/emu-host/`, blargg + mooneye + dmg-acid2).

| # | What | Why | Evidence |
|---|---|---|---|
| P1 | two spare bits (`halt_bug`, `ime_delay`) in the existing flag byte | state for P2/P3 without growing the struct | `sizeof` unchanged |
| P2 | **EI takes effect after the next instruction**; DI cancels a pending EI | `EI; HALT` with an interrupt already pending must enter the HALT first — otherwise the handler runs, HALT then waits a *whole extra* interrupt, and wait-for-VBlank loops drop frames | mooneye `ei_sequence`, `ei_timing`, `rapid_di_ei`, `reti_intr_timing`: fail → pass |
| P3 | **HALT bug**: HALT with IME=0 and an interrupt pending does not halt, and the next opcode byte is fetched twice | real DMG behaviour some games trip over | blargg `halt_bug`: fail → pass |
| P4 | the error hook may **return** (invalid opcode → treated as a no-op; impossible read → 0xFF) | upstream marks the path `__builtin_unreachable()`; on an appliance the hook returns, so that was undefined behaviour at exactly the moment a game goes wrong | device no longer depends on UB after a crash; the app shows "CPU crashed — Reset?" |
| P5 | **MBC2 cartridge RAM kept enabled**; its upper nibble reads as 1 | the header declares 0 RAM banks for MBC2 (the RAM is inside the controller), and init used that to switch cart RAM **off** — MBC2 games could not save at all | mooneye `mbc2/ram`, `mbc2/bits_ramg`: fail → pass |
| P6 | **MBC1 above 512 KB**: BANK2 applies to 4000–7FFF in both modes; mode 1 maps bank `BANK2<<5` at 0000–3FFF | upstream used only the low 5 bits in mode 1 and never remapped bank 0 | mooneye `mbc1/rom_8Mb`, `mbc1/rom_16Mb`: fail → pass |
| P7 | per-line sprite list sized `MAX_SPRITES_LINE + 1` | upstream PR #153: the insertion sort writes one slot past the end (stack corruption in Donkey Kong Land, Pokémon) — required now that sprite priority (`PEANUT_GB_HIGH_LCD_ACCURACY`) is on | — |
| P8 | **timer = taps of one internal counter**: a DIV write restarts TIMA's phase (and clocks it on a falling edge); a TAC write re-derives the phase and clocks TIMA when the tapped bit falls | games that write DIV/TAC to resync music or RNG drifted | mooneye `div_timing`, `div_write`, `tim00`, `tim01_div_trigger`, `tim10`, `tim10_div_trigger`, `tim11`, `rapid_toggle`: fail → pass |

Header problems that do **not** need the core touched are handled host-side in `nucleo_gb.c`
(`header_fix`): a stale header checksum is recomputed (prototypes, translations and homebrew
boot), an out-of-range ROM-size byte is derived from the file size, HuC1 runs as MBC1, and
cartridges the core has no model of (MBC6/7, camera, TAMA5, HuC3, MMM01) are refused with a name.

### Why this core

Chosen for one measured reason: `struct gb_s` is **16,952 B** (8 KB WRAM + 8 KB VRAM +
160 B OAM + 256 B HRAM/IO + a few hundred bytes of CPU/PPU state) and there is **no
framebuffer** — the PPU hands the host one scanline at a time through
`lcd_draw_line(gb, pixels, line)`, so the display cost is a 160-byte line buffer instead
of the 23–46 KB a full frame would take.

That matters on this board because RAM here is not measured in totals but in *contiguous*
blocks: in the emulator's own Solo boot the heap has ~90 KB free but its largest block is
**32 KB** (device trace, `/gbemu_trace.txt`). 17 KB fits that; a core that wanted a
framebuffer would not.

The cartridge is reached through `gb_rom_read()`, another host callback, so the ROM does
not have to be resident either — it streams from the SD card through a page cache.

### Game Boy Color — evaluated, not adopted

[Walnut-CGB](https://github.com/Mr-PauI/Walnut-CGB) (MIT) is a Peanut-GB fork with CGB
support and a PPU that passes cgb-acid2. It was measured on the same host gate: on DMG test
ROMs it passes exactly what upstream Peanut-GB passes (no accuracy gain), and with CGB enabled
its console struct grows to ~49 KB — 32 KB WRAM + 16 KB VRAM as **one** block, on a heap whose
largest block is 32 KB. See `docs/native-emulation.md` §6 for what it would take.

## minigb_apu.c / .h — Game Boy sound

- MIT (`minigb_apu.LICENSE`), vendored verbatim. Context-based (no globals), ~2.2 KB per frame of
  stereo samples.

## z80emu/ — Zilog Z80 (the Game Gear's CPU)

- Upstream: <https://github.com/anotherlin/z80emu> (Lin Ke-Fong)
- Base revision: `1c418fa0d719abab9273131113defbe276101d95` (2017-09-18)
- Licence: "This code is free, do whatever you want with it." (header of every file)
- `z80emu.c`, `z80emu.h`, `instructions.h`, `macros.h`, `tables.h` are **verbatim**. `z80config.h`
  and `z80user.h` are the two files upstream says to replace, and ours replace them:
  - `z80config.h` catches HALT and EI, so the host can take the Game Gear's level-triggered IRQ at
    exactly the instruction boundaries where it can change. It also bridges an upstream naming
    mismatch: `z80emu.c` uses `Z80_STATUS_FLAG_*`, `z80emu.h` declares `Z80_STATUS_*`, so enabling
    any `Z80_CATCH_*` does not compile as shipped.
  - `z80user.h` routes memory and I/O to five functions `nucleo_gg.c` defines before it
    `#include`s `z80emu.c`, so every memory access inlines into the interpreter.
- Verified: built with OUR `z80config.h`, it passes `zexall` and `zexdoc` (all 67 groups) —
  `npm run gg:test` runs zexall every time (`tools/emu-host/zex_test.c`, a minimal CP/M). The machine
  around it is held to real cartridges by the same gate and to SMS Plus by the differential runs
  described in `docs/native-emulation.md` §7.

### Why this CPU and not SMS Plus

Every small SMS/GG emulator (SMS Plus and its forks: retro-go, pico-smsplus, the PocketSprite port)
carries MAME's Z80 by Juergen Buchmueller, "freeware for non-commercial purposes", whose terms the
author reserves the right to change retroactively — unusable in a project that ships binaries under
any open licence. They also keep the 16 KB VRAM in `.bss`, read the ROM through a flat pointer, and
want 32–320 KB of tile caches or look-up tables. The Game Gear machine around the CPU (`nucleo_gg.c`:
VDP, PSG, mappers) is our own, written from the public hardware notes; z80emu is the one part worth
not rewriting.
