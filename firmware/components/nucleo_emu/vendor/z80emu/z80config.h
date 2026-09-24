/* z80config.h — z80emu build switches for NucleoOS (replaces upstream's example; see ../README.md).
 *
 * Upstream: Copyright (c) 2016, 2017 Lin Ke-Fong. "This code is free, do whatever you want with it."
 */
#ifndef __Z80CONFIG_INCLUDED__
#define __Z80CONFIG_INCLUDED__

/* The Game Gear's IRQ is a LEVEL: the VDP holds it until the status port is read. The host accepts it
 * between Z80Emulate() calls, so it needs the emulator to stop where the answer can change:
 *   HALT — the CPU sleeps until an interrupt; the host burns the rest of the line instead of spinning.
 *   EI   — no interrupt may be taken right after EI; the host runs exactly one more instruction first.
 */
#define Z80_CATCH_HALT
#define Z80_CATCH_EI

/* Upstream's z80emu.c names the status values Z80_STATUS_FLAG_* while z80emu.h declares them as
 * Z80_STATUS_*, so enabling any Z80_CATCH_* above does not compile as shipped. Bridged here rather
 * than by patching the vendored file. */
#define Z80_STATUS_FLAG_HALT            Z80_STATUS_HALT
#define Z80_STATUS_FLAG_DI              Z80_STATUS_DI
#define Z80_STATUS_FLAG_EI              Z80_STATUS_EI
#define Z80_STATUS_FLAG_RETI            Z80_STATUS_RETI
#define Z80_STATUS_FLAG_RETN            Z80_STATUS_RETN
#define Z80_STATUS_FLAG_ED_UNDEFINED    Z80_STATUS_ED_UNDEFINED

#endif
