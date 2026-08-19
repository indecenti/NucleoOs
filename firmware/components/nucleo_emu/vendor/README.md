# Vendored emulator cores

## peanut_gb.h — Game Boy (DMG)

- Upstream: <https://github.com/deltabeard/Peanut-GB>
- Revision: `8e656982f08663785794b84823d3e27f856fdb7f` (2026-01-23)
- Licence: MIT (see the header block inside the file)
- Vendored verbatim — **do not edit**. Configuration happens through the `#define`s the
  including translation unit sets before `#include`, and through the callbacks the host
  installs. Keeping it byte-identical to upstream means a future update is a diff, not an
  archaeology exercise.

### Why this core

Chosen for one measured reason: `struct gb_s` is **~17.2 KB** (8 KB WRAM + 8 KB VRAM +
160 B OAM + 256 B HRAM/IO + a few hundred bytes of CPU/PPU state) and there is **no
framebuffer** — the PPU hands the host one scanline at a time through
`lcd_draw_line(gb, pixels, line)`, so the display cost is a 160-byte line buffer instead
of the 23–46 KB a full frame would take.

That matters on this board because RAM here is not measured in totals but in *contiguous*
blocks: the boot trace shows the largest free block collapsing to ~31.7 KB once the shared
32,400-byte UI canvas is allocated, and only a Solo boot (fresh, unfragmented heap) with
Wi-Fi skipped and the canvas released gets it back toward ~60 KB. 17.2 KB fits that with
room to spare; a core that wanted a framebuffer would not.

The cartridge is reached through `gb_rom_read()`, another host callback, so the ROM does
not have to be resident either — it streams from the SD card through a bank cache.
