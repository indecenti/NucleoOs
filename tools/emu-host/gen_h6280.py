#!/usr/bin/env python3
"""Vendor MAME's HuC6280 CPU core (BSD-3-Clause, Bryan McPhail et al.) as plain C for nucleo_pce.c.

    python tools/emu-host/gen_h6280.py <mame>/src/devices/cpu/h6280/h6280.cpp

writes firmware/components/nucleo_emu/vendor/h6280/h6280_core.h. The transformation is mechanical and the only
way the file changes: the class's members become file-scope statics (same names), its methods static functions,
the opcode table an array of function pointers. MAME's memory-space code is dropped; the including file supplies
what the core calls instead (all `pce_*`, defined before the include):

    uint8_t  program_read8(uint16_t a)   / program_write8(uint16_t a, uint8_t v)     through the MPR banks
    uint8_t  program_read8z(uint16_t a)  / program_write8z(uint16_t a, uint8_t v)    zero page (MPR1)
    uint16_t program_read16(uint16_t a)  / program_read16z(uint16_t a)
    uint8_t  read_opcode(void)           / read_opcode_arg(void)
    void     pce_vdc_st(int port, uint8_t v)      ST0 / ST1 / ST2 (the VDC's address, data low, data high)
    void     pce_mpr_changed(int page)            after TAM wrote MPR<page> (the host drops its windows)
    void     pce_psg_write(int reg, uint8_t v)    $0800-$0BFF (the PSG)
    uint8_t  pce_port_read(void) / pce_port_write(uint8_t v)                         the joypad port
    PCE_AFTER_OP()                       optional macro, after every instruction (the host's idle-loop detector)
"""
import os
import re
import sys

src = open(sys.argv[1], encoding='utf-8').read().replace('\r\n', '\n')
out_path = 'firmware/components/nucleo_emu/vendor/h6280/h6280_core.h'

lic = src[:src.index('#include "emu.h"')]

# ── the parts kept: flags + shortcuts, the helpers and instructions, the opcode table, the run loop, IRQ / timer / io ─
def between(a, b):
    i = src.index(a)
    j = src.index(b, i)
    return src[i:j]

flags = between('/* 6280 flags */', 'void h6280_device::internal_map')
table = between('const h6280_device::ophandler h6280_device::s_opcodetable[256] =', '};\n') + '};\n'
helpers = between('inline uint32_t h6280_device::translated', 'void h6280_device::state_string_export')
run = src[src.index('void h6280_device::execute_run()'):]

s = flags + '\n' + helpers + '\n' + run

# MAME-only lines
s = s.replace('debugger_instruction_hook(PCW);', '')
s = re.sub(r'standard_irq_callback\([^;]*\);', '', s)
s = s.replace('logerror("BRK %04x\\n",PCW);', '')
# The dispatch: every opcode through the factored interpreter below. `--hot N` puts the first N of h6280_hot.txt
# (most executed first) in a switch ahead of it, their handlers inlined. Measured on the ADV (Street Fighter II,
# the LX7's counters): 64 inlined 19.4 ms a frame (1.53 M fetch-stall cycles), 16 inlined 18.8 ms, none 17.3 ms
# (0.60 M): every opcode in one small loop beats a fast path for some of them. So none by default.
HOT = [int(x, 16) for x in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'h6280_hot.txt'))
       .read().split('\n')[-2].split()]
HOT = HOT[:int(sys.argv[sys.argv.index('--hot') + 1])] if '--hot' in sys.argv else []
DISPATCH = ('switch (in) {\n' + ''.join(f'\t\tcase 0x{i:02x}: op_{i:02x}(); break;\n' for i in HOT)
            + '\t\tdefault: /*FACTORED*/ break;\n\t\t}\n\t\t')
s = s.replace('(this->*m_opcode[in])();', DISPATCH + 'PCE_AFTER_OP();')
# ST0/ST1/ST2: the VDC through the io space -> the host
s = s.replace('m_io.write_byte(0x0000,tmp);', 'pce_vdc_st(0, tmp);')
s = s.replace('m_io.write_byte(0x0002,tmp);', 'pce_vdc_st(2, tmp);')
s = s.replace('m_io.write_byte(0x0003,tmp);', 'pce_vdc_st(3, tmp);')
# TAM: the host maps the new bank
for i in range(8):
    s = s.replace(f'if (tmp&0x{1 << i:02x}) m_mmr[{i}] = A;', f'if (tmp&0x{1 << i:02x}) {{ m_mmr[{i}] = A; pce_mpr_changed({i}); }}')
# pull(uint8_t &v): a macro over a value-returning function
s = s.replace('void h6280_device::pull(uint8_t &value)', 'static uint8_t pull_(void)')
# methods -> static functions; OP() -> static void op_xx(void)
s = re.sub(r'inline\s+([\w ]+?)\s+h6280_device::(\w+)\(', r'static inline \1 \2(', s)
s = re.sub(r'\n(\w[\w ]*?)\s+h6280_device::(\w+)\(', r'\nstatic \1 \2(', s)
s = s.replace('#define OP(prefix,opcode)  void h6280_device::prefix##_##opcode()',
              '#define OP(prefix,opcode)  static __attribute__((noinline, cold)) void prefix##_##opcode(void)\n'
              '#define OPH(prefix,opcode) static inline __attribute__((always_inline)) void prefix##_##opcode(void)')
for i in HOT:                                      # the hot handlers: inlined into the switch
    s = s.replace(f'OP(op,{i:02x}) {{', f'OPH(op,{i:02x}) {{')

# ── every other opcode: one factored interpreter, built from the same bodies ─────────────────────────────────────
# A cold opcode used to be a function of its own: ~190 of them, ~40 KB of flash, and each call an instruction-cache
# miss (~2 us from DIO flash). Street Fighter II runs 17% of its instructions there: ~3,000 misses a frame, 37% of the
# CPU loop's cycles stalled on fetch (the LX7's counters, on the ADV). MAME's bodies are regular: the op's cycles,
# then an operation on an addressing mode (`h6280_cycles(4); adc(rd_zpg());`, `wr_abs(sta())`, `wb_eaz(asl(...))`).
# So each of the 10 read modes, 9 write modes and ~110 operations is written ONCE: the opcode indexes a 4-byte entry
# {cycles, read mode, operation, write mode | bit}, one switch reads the operand, one does the operation, one writes.
# The same calls in the same order as each body, so the same machine; all 256 opcodes in a few KB that stay cached.
OPS = {int(m.group(1), 16): m.group(2).strip()
       for m in re.finditer(r'^OPH?\(op,([0-9a-f]{2})\) \{(.*?)\}\s*//', s, re.M)}
assert len(OPS) == 256, len(OPS)
RMODES = ['imm', 'zpg', 'zpx', 'zpy', 'abs', 'abx', 'aby', 'zpi', 'idx', 'idy']
F_TST = 0x80                                       # the entry's last byte: TST (its immediate before the operand)
cases, dec = {}, {}                                # case body -> case number; opcode -> (cycles, rmode, case, last)


def case_of(body):
    return cases.setdefault(body, len(cases))


for o in range(256):
    if o in HOT:
        continue
    b = OPS[o]
    if m := re.match(r'^h6280_cycles\((\d+)\); (\w+)\(rd_(\w+)\(\)\);$', b):           # R: op(rd_mode())
        dec[o] = (int(m[1]), RMODES.index(m[3]) + 1, case_of(f'{m[2]}(t);'), 0)
    elif m := re.match(r'^h6280_cycles\((\d+)\); wr_(\w+)\((\w+)\(\)\);$', b):         # W: wr_mode(op())
        dec[o] = (int(m[1]), 0, case_of(f'v = {m[3]}(); goto wr;'), RMODES.index(m[2]) + 1)
    elif m := re.match(r'^h6280_cycles\((\d+)\); wb_(eaz?)\((\w+)\(rd_(\w+)\(\)\)\);$', b):   # RMW
        dec[o] = (int(m[1]), RMODES.index(m[4]) + 1, case_of(f'v = {m[3]}(t); goto wb_{m[2]};'), 0)
    elif m := re.match(r'^h6280_cycles\((\d+)\); wb_(eaz?)\((\w+)\((\d), rd_(\w+)\(\)\)\);$', b):   # RMB / SMB n
        dec[o] = (int(m[1]), RMODES.index(m[5]) + 1, case_of(f'v = {m[3]}(n, t); goto wb_{m[2]};'), int(m[4]))
    elif m := re.match(r'^h6280_cycles\((\d+)\); (bb[rs])\((\d), rd_(\w+)\(\)\);$', b):   # BBR / BBS n
        dec[o] = (int(m[1]), RMODES.index(m[4]) + 1, case_of(f'{m[2]}(n, t);'), int(m[3]))
    elif m := re.match(r'^h6280_cycles\((\d+)\); int imm = rd_imm\(\); tst\(imm, rd_(\w+)\(\)\);$', b):   # TST
        dec[o] = (int(m[1]), RMODES.index(m[2]) + 1, case_of('tst(imm, t);'), F_TST)
    elif m := re.match(r'^h6280_cycles\((\d+)\); (\w+)\(\);$', b):                     # implied
        dec[o] = (int(m[1]), 0, case_of(f'{m[2]}();'), 0)
    elif m := re.match(r'^h6280_cycles\((\d+)\); A = (\w+)\(A\);$', b):                # accumulator
        dec[o] = (int(m[1]), 0, case_of(f'A = {m[2]}(A);'), 0)
    else:                                                                              # the rest, verbatim
        dec[o] = (0, 0, case_of(b), 0)
assert len(cases) < 256
WMODE = ''.join(f'\t\t\tcase {i + 1}: wr_{w}(v); break;\n' for i, w in enumerate(RMODES) if w != 'imm')
RMODE = ''.join(f'\t\t\tcase {i + 1}: t = rd_{r}(); break;\n' for i, r in enumerate(RMODES))
CASES = ''.join(f'\t\t\tcase {n}: {{ {body} }} break;\n' for body, n in cases.items())
FACTORED = ('{\n'
            '\t\t\tconst uint8_t *f = s_fdec[in];   /* {cycles, read mode, operation, write mode | bit | TST} */\n'
            '\t\t\tuint8_t t = 0, v = 0, imm = 0; const int n = f[3] & 7; (void)n; (void)imm;\n'
            '\t\t\tif (f[0]) h6280_cycles(f[0]);\n'
            '\t\t\tif (f[3] & 0x80) imm = rd_imm();\n'
            '\t\t\tswitch (f[1]) {\n' + RMODE + '\t\t\tdefault: break;\n\t\t\t}\n'
            '\t\t\tswitch (f[2]) {\n' + CASES + '\t\t\tdefault: break;\n\t\t\t}\n'
            '\t\t\tbreak;\n'
            '\t\twr:\n'
            '\t\t\tswitch (f[3]) {\n' + WMODE + '\t\t\tdefault: break;\n\t\t\t}\n'
            '\t\t\tbreak;\n'
            '\t\twb_eaz:                         /* read-modify-write: the result back where it was read */\n'
            '\t\t\twb_eaz(v);\n'
            '\t\t\tbreak;\n'
            '\t\twb_ea:\n'
            '\t\t\twb_ea(v);\n'
            '\t\t\t}')
s = s.replace('/*FACTORED*/', FACTORED)
FDEC = ('/* the factored interpreter\'s entries (see the generator): {cycles, read mode, operation, write mode | bit | TST} */\n'
        'static const uint8_t s_fdec[256][4] = {\n' +
        ''.join(f'\t/* {o:02x} */ {{ {d[0]}, {d[1]}, {d[2]}, 0x{d[3]:02x} }},\n' if (d := dec.get(o)) else f'\t/* {o:02x} */ {{ 0 }},\n'
                for o in range(256)) + '};\n'
        '/* N and Z of a result (set_nz) */\n'
        'static const uint8_t s_nz[256] = {' +
        ','.join(('\n\t' if v % 16 == 0 else ' ') + f'0x{(v & 0x80) | (0x02 if v == 0 else 0):02x}' for v in range(256)) + '\n};\n')
# The T flag (the ALU working on zero page (X) instead of A) and decimal mode are rare: out of line and cold, so the
# common ADC / SBC / AND / EOR / ORA stay a few instructions the hot handlers inline (adc was 718 bytes, called).
for f in ('tadc', 'tand', 'teor', 'tora', 'tsbc'):
    s = s.replace(f'static inline void {f}(uint8_t tmp)', f'static __attribute__((noinline, cold)) void {f}(uint8_t tmp)')
# The block transfers (their byte loop is the cost, not the call), BRK and the speed switches: out of line too.
for f in ('tai', 'tdd', 'tia', 'tii', 'tin', 'brk', 'csh', 'csl'):
    f0 = f'static inline void {f}()'
    assert s.count(f0) == 1, f0
    s = s.replace(f0, f'static __attribute__((noinline)) void {f}()')
for f in ('adc', 'sbc'):
    i = s.index(f'static inline void {f}(uint8_t tmp)\n{{')
    j = s.index('\n}\n', i) + 3
    fn = s[i:j]
    m = re.search(r'\t\tif \(P & _fD\)\n\t\t\{\n(.*?)\n\t\t\}\n\t\telse\n', fn, re.S)
    body = '\n'.join(l[2:] for l in m.group(1).split('\n'))
    bcd = f'static __attribute__((noinline, cold)) void {f}_bcd(uint8_t tmp)\n{{\n{body}\n}}\n\n'
    fn = fn.replace(m.group(0), f'\t\tif (P & _fD)\n\t\t\t{f}_bcd(tmp);\n\t\telse\n')
    s = s[:i] + bcd + fn + s[j:]
# Helpers the hot handlers call (when not inlined): next to the run loop (PCE_HOT_FN, the including file's section).
for proto in ('static inline void bra(bool cond)', 'static inline uint8_t rd_zpg(void)', 'static inline uint8_t rd_idy(void)',
              'static inline void adc(uint8_t tmp)', 'static inline void sbc(uint8_t tmp)', 'static inline void and_a(uint8_t tmp)',
              'static inline void eor(uint8_t tmp)', 'static inline void ora(uint8_t tmp)'):
    proto = proto if proto in s else proto.replace('(void)', '()')   # MAME's (), made (void) further down
    assert s.count(proto) == 1, proto
    s = s.replace(proto, proto.replace('static inline ', 'static inline PCE_HOT_FN '))
s = re.sub(r'static (inline )?(\w[\w ]*) (\w+)\(\)', r'static \1\2 \3(void)', s)
# The run loop's placement is the including file's (PCE_HOT: a store module puts it in the section it locks in the
# instruction cache).
s = s.replace('static void execute_run(void)', 'static PCE_HOT void execute_run(void)')

# ── the run loop driven by events, exactly ───────────────────────────────────────────────────────────────────
# MAME checks, after every instruction, a pending interrupt and the timer (decremented with every instruction's
# cycles). PCE-Go checks them once a scanline, which is cheaper and inexact. Here an inner loop runs instructions with
# ONE test, `icount > stop`, where stop is the end of the leg, the cycle the timer reaches zero, or "now" when an
# interrupt is pending (run_stop); the timer is derived from the cycle counter. MAME's checks then run after exactly
# the instructions where they could do something, so the machine is the same, cycle for cycle.
def sub(a, b, n=1):
    global s
    assert s.count(a) == n, (a[:50], s.count(a))
    s = s.replace(a, b)


sub('\t/* Execute instructions */\n\tdo\n\t{\n', '\t/* Execute instructions */\n\trun_stop();\n\tdo\n\t{\n\t\tdo {\n')
sub('PCE_AFTER_OP();\n', 'PCE_AFTER_OP();\n\t\t} while (m_icount > m_stop);   /* the fast path: nothing to check until stop */\n'
    '\t\ttimer_sync();\n')
sub('\t} while (m_icount > 0);\n}', '\t\trun_stop();\n\t} while (m_icount > 0);\n}')
sub('\t\t/* Check internal timer */\n', '\t\t/* Check internal timer */\n\t\ttimer_sync();                  /* (an interrupt taken just above spent cycles) */\n')
sub('\tm_timer_value -= ((cyc) * m_clocks_per_cycle);\n', '')                      # derived from icount now
sub('\tm_icount -= ((cyc) * m_clocks_per_cycle);\n', '\tm_icount -= ((cyc) * m_clocks_per_cycle);   /* (the timer follows: timer_now) */\n')
sub('\tif (!m_irq_pending)\n\t\tm_irq_pending = 2;\n}', '\tif (!m_irq_pending)\n\t\tm_irq_pending = 2;\n\tm_stop = INT32_MAX;                            /* the run loop looks at it after this instruction */\n}')
sub('\t/* only returns countdown */\n', '\t/* only returns countdown */\n\ttimer_sync();\n')
sub('static void timer_w(offs_t offset, uint8_t data)\n{\n', 'static void timer_w(offs_t offset, uint8_t data)\n{\n\ttimer_sync();\n\tm_stop = INT32_MAX;                            /* a start, a stop: the loop recomputes where to stop */\n')
# N and Z from a table: one load instead of a compare and the bit games (the same flags)
sub('''	P = (P & ~(_fN|_fT|_fZ)) |
		(n & _fN) |
		((n == 0) ? _fZ : 0);''', '''	P = (P & ~(_fN|_fT|_fZ)) | s_nz[n];          /* (n & _fN) | (n == 0 ? _fZ : 0), from a table */''')

# the io pages' callbacks -> the host; MAME's address-space translation goes
s = s.replace('''	if (!m_port_in_cb.isunset())
		return m_port_in_cb();
	else
		return m_io_buffer;''', '''	return pce_port_read();''')
s = s.replace('	m_port_out_cb(data);', '	pce_port_write(data);')
s = s.replace('	m_psg->c6280_w(offset, data);', '	pce_psg_write((int)offset, data);')
i = s.index('static bool memory_translate(')
j = s.index('static uint8_t io_get_buffer(void)')
s = s[:i] + s[j:]

tab = table.replace('const h6280_device::ophandler h6280_device::s_opcodetable[256] =',
                    'static void (*const s_opcodetable[256])(void) =')
tab = tab.replace('&h6280_device::', '')


head = '''/* GENERATED by tools/emu-host/gen_h6280.py from MAME src/devices/cpu/h6280/h6280.cpp — do not edit by hand.
 * MAME's HuC6280 core, plain C: members are file-scope statics, methods static functions. See the generator for
 * what the including file supplies. Original notice: */
''' + lic + '''
#include <stdint.h>
#include <stdbool.h>
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#ifndef PCE_AFTER_OP
#define PCE_AFTER_OP() ((void)0)                   /* the including file's hook after every instruction */
#endif
#ifndef PCE_HOT
#define PCE_HOT                                    /* the including file's placement of the run loop */
#endif
#ifndef PCE_HOT_FN
#define PCE_HOT_FN                                 /* ...and of the helpers the hot handlers call */
#endif

typedef union { struct { uint8_t l, h, h2, h3; } b; struct { uint16_t l, h; } w; uint32_t d; } PAIR;
#define CLEAR_LINE  0
#define ASSERT_LINE 1
#define INPUT_LINE_NMI 32
#define H6280_LAZY_FLAGS 0
#define H6280_IRQ_MASK 7
enum { H6280_RESET_VEC = 0xfffe, H6280_NMI_VEC = 0xfffc, H6280_TIMER_VEC = 0xfffa, H6280_IRQ1_VEC = 0xfff8,
       H6280_IRQ2_VEC = 0xfff6 };

/* The CPU's state in ONE struct, the members under their MAME names by macro. A store module is position-independent
 * code: every separate global costs a literal (its address, fetched through the instruction cache from flash) and a
 * load. One struct is one base address the compiler keeps in a register, and each member a single load. */
static struct {
    PAIR ppc, pc, sp, zp, ea;
    int icount;
    int32_t timer_value, timer_load;
    int32_t tsync, stop;                          /* the timer as of icount == tsync; where the fast loop stops */
    uint8_t a, x, y, p, mmr[8], irq_mask, timer_status, timer_ack, clocks_per_cycle;
    uint8_t nmi_state, irq_state[3], irq_pending, io_buffer, timer_scale;
} H6280 = { .timer_scale = 1 };
#define m_tsync H6280.tsync
#define m_stop H6280.stop
/* The timer counts down with the cycles: it is not decremented with every instruction but derived from the cycle
 * counter (timer_value as of icount == tsync). timer_sync() makes timer_value current; the including file calls it
 * before reading or writing the timer, and moves tsync with icount when it gives the CPU more cycles. */
static inline void timer_sync(void) { H6280.timer_value -= H6280.tsync - H6280.icount; H6280.tsync = H6280.icount; }
static inline int32_t timer_now(void) { return H6280.timer_value - (H6280.tsync - H6280.icount); }
/* Where the run loop's fast path stops: at the end of the leg (0), at the cycle the timer reaches zero, or after this
 * instruction when an interrupt is pending (INT32_MAX). Anything that may make an interrupt pending sets it so. */
static inline void run_stop(void)
{
    if (H6280.irq_pending) { H6280.stop = INT32_MAX; return; }
    const int32_t t = H6280.timer_status ? H6280.icount - timer_now() : 0;
    H6280.stop = t > 0 ? t : 0;
}
#define m_ppc H6280.ppc
#define m_pc H6280.pc
#define m_sp H6280.sp
#define m_zp H6280.zp
#define m_ea H6280.ea
#define m_icount H6280.icount
#define m_timer_value H6280.timer_value
#define m_timer_load H6280.timer_load
#define m_a H6280.a
#define m_x H6280.x
#define m_y H6280.y
#define m_p H6280.p
#define m_mmr H6280.mmr
#define m_irq_mask H6280.irq_mask
#define m_timer_status H6280.timer_status
#define m_timer_ack H6280.timer_ack
#define m_clocks_per_cycle H6280.clocks_per_cycle
#define m_nmi_state H6280.nmi_state
#define m_irq_state H6280.irq_state
#define m_irq_pending H6280.irq_pending
#define m_io_buffer H6280.io_buffer
#define m_timer_scale H6280.timer_scale

/* the including file's memory, before the core */
static uint8_t  program_read8(uint16_t addr);
static void     program_write8(uint16_t addr, uint8_t data);
static uint8_t  program_read8z(uint16_t addr);
static void     program_write8z(uint16_t addr, uint8_t data);
static uint16_t program_read16(uint16_t addr);
static uint16_t program_read16z(uint16_t addr);
static void     push(uint8_t value);
static uint8_t  pull_(void);
#define pull(v) ((v) = pull_())
static uint8_t  read_opcode(void);
static uint8_t  read_opcode_arg(void);
static void     set_irq_line(int irqline, int state);
static void pce_vdc_st(int port, uint8_t v);
static void pce_mpr_changed(int page);
static uint8_t pce_port_read(void);
static void pce_port_write(uint8_t v);
static void pce_psg_write(int reg, uint8_t v);
typedef uint32_t offs_t;
'''

# forward declarations of every op (the table names them before their bodies) and of every helper (C needs a
# declaration before a use; MAME's class had them all in its header)
ops = sorted(set(re.findall(r'OPH?\(op,([0-9a-f]{2})\)', s)))
fwd = '\n'.join(f'static void op_{o}(void);' for o in ops)
protos = []
for m in re.finditer(r'^static (inline )?([\w ]+?)\s+(\w+)\(([^)]*)\)\s*\n\{', s, re.M):
    if m.group(3) == 'execute_run' or m.group(3).startswith('op_'):
        continue
    protos.append(f'static {m.group(1) or ""}{m.group(2)} {m.group(3)}({m.group(4)});')
fwd = '\n'.join(protos) + '\n' + fwd

out = head + '\n' + fwd + '\n' + FDEC + '\n' + s   # (MAME's table of handlers is no longer needed)
open(out_path, 'w', encoding='utf-8', newline='\n').write(out)
print('wrote', out_path, len(out.splitlines()), 'lines,', len(ops), 'opcodes')
