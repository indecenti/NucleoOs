/* z80user.h — how z80emu reaches the Game Gear (replaces upstream's zextest example).
 *
 * Upstream: Copyright (c) 2016, 2017 Lin Ke-Fong. "This code is free, do whatever you want with it."
 *
 * z80emu.c is compiled INSIDE the file that includes it (nucleo_gg.c, or the host ZEX harness), which
 * defines these five functions first — so every memory access below inlines into the interpreter
 * instead of going through a function pointer. `context` is the void * handed to Z80Emulate().
 *
 *   z80u_rd(ctx, addr)                   byte read, addr already 16-bit
 *   z80u_wr(ctx, addr, v)                byte write
 *   z80u_in(ctx, port, elapsed)          port read; elapsed = cycles into this Z80Emulate() call
 *   z80u_out(ctx, port, v, elapsed)      port write; returns non-zero to stop after this instruction
 *                                        (the IRQ line may have just risen)
 */
#ifndef __Z80USER_INCLUDED__
#define __Z80USER_INCLUDED__

#define Z80_READ_BYTE(address, x)       { (x) = z80u_rd(context, (address) & 0xffff); }
#define Z80_FETCH_BYTE(address, x)      Z80_READ_BYTE((address), (x))
#define Z80_READ_WORD(address, x)                                               \
{                                                                               \
        (x) = z80u_rd(context, (address) & 0xffff)                              \
            | (z80u_rd(context, ((address) + 1) & 0xffff) << 8);                \
}
#define Z80_FETCH_WORD(address, x)      Z80_READ_WORD((address), (x))
#define Z80_WRITE_BYTE(address, x)      { z80u_wr(context, (address) & 0xffff, (x)); }
#define Z80_WRITE_WORD(address, x)                                              \
{                                                                               \
        z80u_wr(context, (address) & 0xffff, (x));                              \
        z80u_wr(context, ((address) + 1) & 0xffff, (x) >> 8);                   \
}
#define Z80_READ_WORD_INTERRUPT(address, x)     Z80_READ_WORD((address), (x))
#define Z80_WRITE_WORD_INTERRUPT(address, x)    Z80_WRITE_WORD((address), (x))
#define Z80_INPUT_BYTE(port, x)         { (x) = z80u_in(context, (port), elapsed_cycles); }
#define Z80_OUTPUT_BYTE(port, x)        { if (z80u_out(context, (port), (x), elapsed_cycles)) number_cycles = 0; }

#endif
