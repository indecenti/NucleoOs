// nucleo_gg — see include/nucleo_gg.h for why this is our own machine around a vendored Z80.
//
// The Game Gear is a Master System with a 160x144 window onto the VDP's 256x192 picture, a 4096-colour
// palette and a Start button: Z80 @ 3.58 MHz, 8 KB work RAM, a TMS9918-derived VDP in "mode 4"
// (16 KB VRAM, 32 colours, 64 sprites, line interrupts), an SN76489 PSG and the Sega mapper. What is
// written here follows the public hardware notes (Charles MacDonald's "SMS/GG hardware notes" and
// "SMS VDP documentation", SMS Power!): no emulator's code was used.
//
// MASTER SYSTEM MODE comes with it: a Game Gear runs SMS cartridges natively (the whole 256x192
// picture, the SMS 64-colour palette, Start as Pause), and a few Game Gear releases are SMS games that
// ask for exactly that mode. So the same machine plays ".sms" files too.
//
// HOW IT FITS
//   console      one ~25 KB block (gg_machine_t): the save state is that struct, byte for byte
//   picture      one line at a time into a 256-pixel buffer, never a frame (a frame would be 45-96 KB)
//   cartridge    1 KB pages from the SD card, LRU — the gbemu cache, measured there (docs §4.2)
//   battery RAM  1 KB pages taken from the same pool, only the ones a game actually writes
//   EEPROM       the 128-byte 93C46 of the Sega sports cartridges, inside the machine struct
#include "nucleo_gg.h"
#include "nucleo_audio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// ── the CPU: z80emu, built inside this file so its memory accessors inline ──────────────────────────
#include "vendor/z80emu/z80emu.h"

static const char *TAG = "gg";

// NTSC timing. 228 CPU cycles per line, 262 lines, 192 of them active; the Game Gear shows lines
// 24..167 and columns 48..207 of that picture.
#define CYC_LINE    228
#define LINES       262
#define ACTIVE      192
#define FRAME_CYC   (CYC_LINE * LINES)          // 59,736 cycles = 16.688 ms
#define GG_X0       48
#define GG_Y0       24

// PSG → speaker. 32 kHz mono: the tiny speaker has one channel, and the chip's own tones sit far
// below 16 kHz. 534 samples per frame x 59.92 frames = 31,997 Hz, so the I2S drains at the rate the
// frames fill it.
#define PSG_RATE    32000
#define PSG_SPF     534
#define PSG_STEP    458188u                     // (3579545 / 16) / 32000 in 16.16 fixed point

// Cartridge pages. Same geometry the Game Boy cache measured best (1 KB x as many as fit, LRU,
// hint table) — see nucleo_gb.c. The pool also lends pages to battery RAM, pinned while in use.
#define PG_BITS     10
#define PG_SIZE     (1u << PG_BITS)
#define PG_SLOTS    96                          // cap; <= 127 (slot indices are int8_t)
#define PG_MIN      12                          // fewer ROM pages than this thrashes: refuse
#define PG_HINT     256
#define PG_EMPTY    0xFFFFFFFFu
#define PG_SRAM     0x80000000u                 // tag bit: this slot holds battery RAM, never evicted
#define SRAM_PAGES  32                          // 32 KB: two 16 KB banks, the most a Sega mapper maps
#define SRAM_BYTES  (SRAM_PAGES * PG_SIZE)
#define KEEP        (8 * 1024)                  // heap left for a running game's transient file handles

// ── the console ────────────────────────────────────────────────────────────────────────────────────
// Plain data only: a save state is this struct written out whole. The Z80's register-decoding tables
// are pointers into the struct itself, so they are rebuilt after every load (z80_fix) — never trusted
// from a file.
typedef struct {
    Z80_STATE z;
    uint8_t  wram[0x2000];
    uint8_t  vram[0x4000];
    uint8_t  cram[0x40];                        // 32 colours x 12 bits (GG), little-endian pairs
    uint8_t  reg[16];
    uint8_t  status;                            // b7 frame IRQ, b6 sprite overflow, b5 collision
    uint8_t  pending, code, rbuf, clatch;       // control-port byte latch, access code, read buffer, CRAM latch
    uint8_t  lc, line_irq, vscroll;             // line counter, its pending flag, V scroll latched per frame
    uint16_t addr;
    uint8_t  map[4];                            // Sega: FFFC-FFFF; Codemasters: map[1..3] = slots 0-2
    uint8_t  io, stereo, halted, ei_block;
    uint8_t  link[6];                           // GG ports 1-5: the link port's registers, read back as written
    int32_t  cyc;                               // CPU cycles into the frame (carries <1 instruction over)
    // SN76489
    uint16_t tone[4];                           // [3] = noise control
    uint8_t  vol[4], out[4];
    uint8_t  psg_latch, nflip;
    uint16_t lfsr;
    int32_t  cnt[4];
    uint32_t tick_acc;
    int32_t  dc;                                // DC blocker (the chip's output is unipolar)
    // 93C46 serial EEPROM (sports cartridges): 64 x 16 bits, and its serial-protocol state
    uint16_t ee[64];
    uint16_t ee_sr;
    uint8_t  ee_on, ee_cs, ee_clk, ee_do, ee_state, ee_bits, ee_we, ee_op, ee_addr;
    uint8_t  nmi;                               // Pause pressed (Master System mode): NMI at the next frame
} gg_machine_t;

typedef struct {
    gg_machine_t *m;

    // Z80 address space in 1 KB pages. rmap NULL = not resident (fault it in); wmap covers work RAM only.
    uint8_t      *rmap[64];
    uint8_t      *wmap[64];
    int8_t        rslot[64];                    // pool slot behind rmap[i], -1 = none / battery RAM

    // Page pool
    uint8_t      *pg[PG_SLOTS];
    uint32_t      pg_tag[PG_SLOTS];
    uint32_t      pg_age[PG_SLOTS];
    uint8_t       pg_mapped[PG_SLOTS];          // rmap entries pointing here — prefer evicting 0
    int           pg_n, pg_pinned;
    uint32_t      pg_clock, frame_clock;        // age stamps; frame_clock = pg_clock when this frame began
    int8_t        hint[PG_HINT];
    uint32_t      misses, faults;               // SD refills / page (re)mappings, since the last reset

    uint8_t      *sram[SRAM_PAGES];             // battery RAM pages (pool slots), NULL = all zero
    bool          sram_present;                 // the cartridge has used (or has a save of) battery RAM
    bool          sram_dirty, sram_full_warned;
    uint32_t      sram_touch;

    FILE         *fp;
    uint32_t      rom_bytes, rom_off;           // file size, and the 512-byte copier header if present
    uint32_t      banks;                        // 16 KB banks
    uint32_t      crc;                          // CRC-32 of the file: what the cartridge database is keyed on
    bool          codies, eeprom;

    // The picture. GG: lines 24-167, columns 48-207 of the VDP's 256x192; SMS mode: all of it.
    bool          sms;                          // Master System mode (a GG cartridge that asks for it, or .sms)
    int           x0, y0, w, h;
    uint32_t      idx[68];                      // one line's pixel flags, background space (bg_line):
                                                //   b0-3 colour, b4 palette, b5 bg priority, b6 sprite here
    uint32_t      cbuf[136];                    // ...and its colours, same space: what the line callback gets
    uint32_t      band[24][2];                  // sprites that can touch each 8-line band (spr_bands)
    bool          band_dirty;
    uint16_t      pal[32];                      // CRAM as panel-order RGB565

    int16_t       pcm[PSG_SPF];
    int           psg_pos;
    bool          sound;

    int           vline;
    uint8_t       buttons, pad;
    bool          frameskip;
    uint32_t      clock, frames;
    uint32_t      us_cpu, us_audio;
    size_t        heap_bytes;

    nucleo_gg_line_fn on_line;
    void             *user;
    char          rom_path[300];
    char          save_base[300];
    char          title[40];
} gg_session_t;

static gg_session_t *S = NULL;

// The ~25 KB console needs the heap's one 32 KB block (the zip window before it, unzip_rom); the
// session is meant to land in the smaller fragments beside it. A session that grows past this stops
// fitting them and starts eating that block — recheck the modelled heap (npm run gg:test) first.
_Static_assert(sizeof(gg_session_t) <= 7168, "gg_session_t grew: recheck the pre-unzip heap margin");

#ifdef NUCLEO_GG_PROFILE                        // bench only: where a frame's time goes, in µs
uint32_t gg_prof_render, gg_prof_spr, gg_prof_map;   // map stays 0: the colour pass is fused into bg_line
#endif

// ── cartridge pages ────────────────────────────────────────────────────────────────────────────────
static void map_clear(gg_session_t *s, int zp)
{
    int sl = s->rslot[zp];
    if (sl >= 0 && s->pg_mapped[sl]) s->pg_mapped[sl]--;
    s->rslot[zp] = -1;
    s->rmap[zp] = NULL;
}

// A bank switch forgets the 16 pages of that slot; the next access faults the new ones in.
static void slot_unmap(gg_session_t *s, int slot)
{
    for (int zp = slot * 16; zp < slot * 16 + 16; zp++) map_clear(s, zp);
}

// Victim for a new page. Never battery RAM, never a page mapped right now if there is any other.
// Among the rest: the oldest one this frame has not touched — plain LRU — but if THIS frame has
// touched them all, the pool is smaller than the frame's working set, and the pattern is a cycle (a
// game walks the same banks in the same order every frame). LRU is the worst possible policy for a
// cycle one page larger than the cache: it evicts exactly the page needed next, and every access
// misses. Evicting the most recently touched instead keeps the rest of the cycle resident, so the
// misses shrink to the overflow. Measured on Sonic 2 at 50 pages: 3.3 misses/frame with LRU alone.
static int pool_victim(gg_session_t *s)
{
    int v = -1;
    for (int i = 0; i < s->pg_n; i++) {
        if (s->pg_tag[i] != PG_EMPTY && (s->pg_tag[i] & PG_SRAM)) continue;
        if (v < 0) { v = i; continue; }
        bool mi = s->pg_mapped[i] != 0, mv = s->pg_mapped[v] != 0;
        if (mi != mv) { if (!mi) v = i; continue; }
        bool oi = s->pg_age[i] <= s->frame_clock, ov = s->pg_age[v] <= s->frame_clock;
        if (oi != ov) { if (oi) v = i; continue; }
        if (oi ? s->pg_age[i] < s->pg_age[v] : s->pg_age[i] > s->pg_age[v]) v = i;
    }
    if (v >= 0 && s->pg_mapped[v])
        for (int zp = 0; zp < 48; zp++) if (s->rslot[zp] == v) map_clear(s, zp);
    return v;
}

// Pool slot holding ROM page `tag` (file offset / 1 KB), refilled from the card on a miss.
static int page_get(gg_session_t *s, uint32_t tag)
{
    int h = (int)(tag & (PG_HINT - 1));
    int g = s->hint[h];
    if (g >= 0 && g < s->pg_n && s->pg_tag[g] == tag) return g;
    for (int i = 0; i < s->pg_n; i++)
        if (s->pg_tag[i] == tag) { s->hint[h] = (int8_t)i; return i; }

    int v = pool_victim(s);
    if (v < 0) return -1;
    uint32_t off = s->rom_off + (tag << PG_BITS);
    size_t want = PG_SIZE, got = 0;
    if (off < s->rom_bytes) {
        if (off + want > s->rom_bytes) want = s->rom_bytes - off;
        if (fseek(s->fp, (long)off, SEEK_SET) == 0) got = fread(s->pg[v], 1, want, s->fp);
    }
    if (got < PG_SIZE) memset(s->pg[v] + got, 0xFF, PG_SIZE - got);
    s->pg_tag[v] = tag;
    s->hint[h] = (int8_t)v;
    s->misses++;
    return v;
}

// Which 1 KB of the ROM file the Z80 page `zp` (0..47) shows right now.
static uint32_t rom_page(const gg_session_t *s, int zp)
{
    const gg_machine_t *m = s->m;
    int slot = zp >> 4;
    uint32_t bank = (!s->codies && zp == 0) ? 0 : m->map[1 + slot];   // Sega: 0000-03FF never pages
    return (bank % s->banks) * 16 + (uint32_t)(zp & 15);
}

static inline bool sram_mapped(const gg_session_t *s) { return !s->codies && !s->eeprom && (s->m->map[0] & 0x08); }

// ── 93C46 serial EEPROM ────────────────────────────────────────────────────────────────────────────
// World Series Baseball, The Majors and Pro Yakyuu GG League save to a 1-kbit Microwire EEPROM instead
// of battery RAM. FFFC bit 3 lays it over $8000 and bit 7 resets its interface; the game then bit-bangs
// it through that one address: a write drives DI (bit 0), CLK (bit 1) and CS (bit 2), a read returns DO
// on bit 0. Protocol per the 93C46 datasheet: start bit, 2-bit opcode, 6-bit address, 16 data bits.
enum { EE_START, EE_CMD, EE_DATA, EE_READ, EE_IDLE };

static void ee_clock(gg_session_t *s, int di)
{
    gg_machine_t *m = s->m;
    switch (m->ee_state) {
        case EE_START:
            if (di) { m->ee_state = EE_CMD; m->ee_bits = 0; m->ee_sr = 0; }
            break;
        case EE_CMD:
            m->ee_sr = (uint16_t)((m->ee_sr << 1) | di);
            if (++m->ee_bits < 8) break;
            m->ee_op = (uint8_t)(m->ee_sr >> 6);
            m->ee_addr = (uint8_t)(m->ee_sr & 0x3F);
            m->ee_bits = 0; m->ee_sr = 0;
            switch (m->ee_op) {
                case 2:                                         // READ: a dummy 0, then D15..D0, sequential
                    m->ee_state = EE_READ; m->ee_sr = m->ee[m->ee_addr]; m->ee_do = 0; break;
                case 1:                                         // WRITE: 16 data bits follow
                    m->ee_state = EE_DATA; break;
                case 3:                                         // ERASE
                    if (m->ee_we) { m->ee[m->ee_addr] = 0xFFFF; s->sram_dirty = true; s->sram_touch = s->clock; }
                    m->ee_state = EE_IDLE; m->ee_do = 1; break;
                default:
                    switch (m->ee_addr >> 4) {
                        case 0: m->ee_we = 0; m->ee_state = EE_IDLE; break;          // EWDS
                        case 1: m->ee_state = EE_DATA; break;                        // WRAL
                        case 2: if (m->ee_we) { for (int i = 0; i < 64; i++) m->ee[i] = 0xFFFF;   // ERAL
                                                s->sram_dirty = true; s->sram_touch = s->clock; }
                                m->ee_state = EE_IDLE; break;
                        default: m->ee_we = 1; m->ee_state = EE_IDLE; break;          // EWEN
                    }
                    m->ee_do = 1;
                    break;
            }
            break;
        case EE_DATA:
            m->ee_sr = (uint16_t)((m->ee_sr << 1) | di);
            if (++m->ee_bits < 16) break;
            if (m->ee_we) {
                if (m->ee_op == 0) for (int i = 0; i < 64; i++) m->ee[i] = m->ee_sr;
                else m->ee[m->ee_addr] = m->ee_sr;
                s->sram_dirty = true; s->sram_touch = s->clock;
            }
            m->ee_state = EE_IDLE; m->ee_do = 1;
            break;
        case EE_READ:
            m->ee_do = (uint8_t)(m->ee_sr >> 15);
            m->ee_sr <<= 1;
            if (++m->ee_bits == 16) { m->ee_bits = 0; m->ee_addr = (m->ee_addr + 1) & 63; m->ee_sr = m->ee[m->ee_addr]; }
            break;
        default:
            break;
    }
}

static void ee_write(gg_session_t *s, uint8_t v)
{
    gg_machine_t *m = s->m;
    int cs = (v >> 2) & 1, clk = (v >> 1) & 1;
    if (!cs) { m->ee_state = EE_START; m->ee_do = 1; }                 // deselected: standby, DO released
    else {
        if (!m->ee_cs) m->ee_state = EE_START;                          // CS rising: a new command
        if (clk && !m->ee_clk) ee_clock(s, v & 1);                      // data moves on the rising edge
    }
    m->ee_cs = (uint8_t)cs; m->ee_clk = (uint8_t)clk;
}

static inline uint8_t ee_read(const gg_machine_t *m) { return (uint8_t)((m->ee_cs << 2) | 0x02 | m->ee_do); }
static inline int  sram_page(const gg_session_t *s, unsigned a)
{
    return ((s->m->map[0] & 0x04) ? 16 : 0) + (int)((a >> 10) & 15);
}

// A battery-RAM page for the first write to it: a pool slot, pinned, zeroed. The ROM keeps at least
// PG_MIN pages whatever a game asks for — beyond that the write is dropped (said once in the log).
static uint8_t *sram_alloc(gg_session_t *s, int p)
{
    if (s->pg_n - s->pg_pinned <= PG_MIN) {
        if (!s->sram_full_warned) { s->sram_full_warned = true; ESP_LOGW(TAG, "battery RAM: pool exhausted"); }
        return NULL;
    }
    int v = pool_victim(s);
    if (v < 0) return NULL;
    s->pg_tag[v] = PG_SRAM | (uint32_t)p;
    memset(s->pg[v], 0, PG_SIZE);
    s->pg_pinned++;
    s->sram[p] = s->pg[v];
    return s->pg[v];
}

static uint8_t gg_fault(gg_session_t *s, unsigned a)
{
    int zp = (int)(a >> 10);
    if (zp == 32 && s->m->ee_on) {
        // The EEPROM answers at $8000 only; the rest of that 1 KB page is still ROM, so the page is
        // never mapped while the EEPROM is on and every access to it comes through here.
        if (a == 0x8000) return ee_read(s->m);
        int sl = page_get(s, rom_page(s, zp));
        return sl < 0 ? 0xFF : s->pg[sl][a & (PG_SIZE - 1)];
    }
    if (zp >= 32 && sram_mapped(s)) {
        uint8_t *p = s->sram[sram_page(s, a)];
        if (!p) return 0x00;                    // never written: erased RAM
        s->rmap[zp] = p;
        return p[a & (PG_SIZE - 1)];
    }
    int sl = page_get(s, rom_page(s, zp));
    if (sl < 0) return 0xFF;
    s->faults++;
    map_clear(s, zp);
    s->rmap[zp] = s->pg[sl];
    s->rslot[zp] = (int8_t)sl;
    s->pg_mapped[sl]++;
    s->pg_age[sl] = ++s->pg_clock;
    return s->pg[sl][a & (PG_SIZE - 1)];
}

// Everything derived from the machine struct that is not IN it: the page map, the sprite bands.
// After a power-on or a state load, both are rebuilt from scratch.
static void remap_all(gg_session_t *s)
{
    s->band_dirty = true;
    for (int zp = 0; zp < 48; zp++) map_clear(s, zp);
    for (int zp = 48; zp < 64; zp++) {
        s->rmap[zp] = s->wmap[zp] = s->m->wram + ((zp & 7) << PG_BITS);
        s->rslot[zp] = -1;
    }
}

static void mapper_write(gg_session_t *s, int r, uint8_t v)
{
    gg_machine_t *m = s->m;
    if (r == 0 && s->eeprom) {                  // EEPROM carts: FFFC is the EEPROM's control
        if (v & 0x80) { m->ee_state = EE_START; m->ee_do = 1; m->ee_cs = m->ee_clk = 0; }
        m->ee_on = (v & 0x08) != 0;
        map_clear(s, 32);
    }
    if (m->map[r] == v) return;
    m->map[r] = v;
    if (r == 0) slot_unmap(s, 2);               // RAM in/out of 8000-BFFF, or its bank changed
    else if (r != 3 || !sram_mapped(s)) slot_unmap(s, r - 1);
}

static void gg_wr_slow(gg_session_t *s, unsigned a, uint8_t v)
{
    if (a >= 0xC000) {
        s->m->wram[a & 0x1FFF] = v;
        if (a >= 0xFFFC && !s->codies) mapper_write(s, (int)(a & 3), v);
        return;
    }
    if (s->codies) {
        if ((a & 0x3FFF) == 0) mapper_write(s, 1 + (int)(a >> 14), v);
        return;
    }
    if (a == 0x8000 && s->m->ee_on) { ee_write(s, v); return; }
    if (a >= 0x8000 && sram_mapped(s)) {
        int p = sram_page(s, a);
        uint8_t *b = s->sram[p];
        if (!b) {
            if (!v) return;                     // zero onto erased RAM changes nothing
            if (!(b = sram_alloc(s, p))) return;
            s->rmap[a >> 10] = b;
        }
        s->sram_present = true;
        if (b[a & (PG_SIZE - 1)] != v) { b[a & (PG_SIZE - 1)] = v; s->sram_dirty = true; s->sram_touch = s->clock; }
    }
    // anything else is a write to ROM: ignored, as on the real cartridge
}

// ── VDP ────────────────────────────────────────────────────────────────────────────────────────────
// The frame flag rises on line 192, but the IRQ it causes is only seen from the NEXT line. On the
// hardware a game polling the status port (IN A,($BF) / JP P) catches the flag whenever it rises
// during the IN, before the CPU samples the interrupt; with an IRQ at the very edge the handler would
// read it first, every frame, and the poll would never end (Monster Truck Wars, Chicago Syndicate).
static inline bool irq_line(const gg_session_t *s)
{
    const gg_machine_t *m = s->m;
    return ((m->status & 0x80) && (m->reg[1] & 0x20) && s->vline != ACTIVE)
        || (m->line_irq && (m->reg[0] & 0x10));
}

static void pal_update(gg_session_t *s, int i)
{
    uint16_t v;
    if (s->sms) {                               // Master System: one byte, --BBGGRR
        static const uint8_t L5[4] = { 0, 10, 21, 31 }, L6[4] = { 0, 21, 42, 63 };
        uint8_t c = s->m->cram[i];
        v = (uint16_t)((L5[c & 3] << 11) | (L6[(c >> 2) & 3] << 5) | L5[(c >> 4) & 3]);
    } else {                                    // Game Gear: two bytes, ----BBBB GGGGRRRR
        const uint8_t *c = &s->m->cram[i * 2];
        unsigned r = c[0] & 15, g = c[0] >> 4, b = c[1] & 15;
        v = (uint16_t)(((r * 2 + (r >> 3)) << 11) | ((g * 4 + (g >> 2)) << 5) | (b * 2 + (b >> 3)));
    }
    s->pal[i] = (uint16_t)((v >> 8) | (v << 8));      // panel byte order
}

// Returns true when this write raised the IRQ line (the CPU must look at it after this instruction).
static bool vdp_ctrl_w(gg_session_t *s, uint8_t v)
{
    gg_machine_t *m = s->m;
    if (!m->pending) { m->addr = (uint16_t)((m->addr & 0x3F00) | v); m->pending = 1; return false; }
    m->pending = 0;
    m->code = v >> 6;
    m->addr = (uint16_t)(((v & 0x3F) << 8) | (m->addr & 0xFF));
    if (m->code == 0) {
        m->rbuf = m->vram[m->addr];
        m->addr = (m->addr + 1) & 0x3FFF;
    } else if (m->code == 2 && (v & 0x0F) < 11) {
        bool was = irq_line(s);
        m->reg[v & 0x0F] = (uint8_t)m->addr;
        if ((v & 0x0F) == 1 || (v & 0x0F) == 5) s->band_dirty = true;   // sprite size / table moved
        return !was && irq_line(s);
    }
    return false;
}

static void vdp_data_w(gg_session_t *s, uint8_t v)
{
    gg_machine_t *m = s->m;
    m->pending = 0;
    if (m->code == 3 && s->sms) {               // SMS CRAM: 32 single-byte colours
        m->cram[m->addr & 0x1F] = v & 0x3F;
        pal_update(s, m->addr & 0x1F);
    } else if (m->code == 3) {                  // GG CRAM: 12-bit colours, latched on the even byte
        if (m->addr & 1) {
            int i = m->addr & 0x3E;
            m->cram[i] = m->clatch;
            m->cram[i + 1] = v & 0x0F;
            pal_update(s, i >> 1);
        } else m->clatch = v;
    } else {
        m->vram[m->addr] = v;
        // A sprite's Y moved: the per-band sprite lists (spr_bands) are stale.
        if (((m->addr - ((m->reg[5] & 0x7E) << 7)) & 0x3FFF) < 64) s->band_dirty = true;
    }
    m->rbuf = v;
    m->addr = (m->addr + 1) & 0x3FFF;
}

static uint8_t vdp_data_r(gg_machine_t *m)
{
    m->pending = 0;
    uint8_t v = m->rbuf;
    m->rbuf = m->vram[m->addr];
    m->addr = (m->addr + 1) & 0x3FFF;
    return v;
}

static uint8_t vdp_status_r(gg_machine_t *m)
{
    uint8_t v = (uint8_t)((m->status & 0xE0) | 0x1F);
    m->status = 0;
    m->line_irq = 0;
    m->pending = 0;
    return v;
}

static inline uint8_t vcounter(const gg_session_t *s)
{
    int l = s->vline;
    return (uint8_t)(l <= 0xDA ? l : l - 6);    // NTSC 192-line: 00-DA, then D5-FF
}

static uint8_t hcounter(const gg_session_t *s, int elapsed)
{
    int c = s->m->cyc + elapsed - s->vline * CYC_LINE;
    if (c < 0) c = 0; else if (c >= CYC_LINE) c = CYC_LINE - 1;
    int h = c * 171 / CYC_LINE;                 // 342 pixels per line, counted in pairs
    return (uint8_t)(h > 0x93 ? h + 0x55 : h);  // 00-93, then E9-FF
}

// ── PSG (SN76489) ──────────────────────────────────────────────────────────────────────────────────
// 2 dB per attenuation step; four channels at full volume sum to 32,000.
static const int16_t VOL[16] = { 8000, 6355, 5048, 4009, 3185, 2530, 2009, 1596,
                                 1268, 1007,  800,  635,  505,  401,  318,    0 };

static void psg_run(gg_session_t *s, int upto)
{
    gg_machine_t *m = s->m;
    if (upto > PSG_SPF) upto = PSG_SPF;
    unsigned en = (m->stereo | (m->stereo >> 4)) & 15;     // one speaker: audible on either side
    for (; s->psg_pos < upto; s->psg_pos++) {
        m->tick_acc += PSG_STEP;
        int n = (int)(m->tick_acc >> 16);
        m->tick_acc &= 0xFFFF;
        int acc = 0;
        for (int c = 0; c < 3; c++) {
            int p = m->tone[c];
            // Period 0/1 toggles at >100 kHz: it is inaudible as a tone and is how games play samples
            // (hold the output high, write volumes). Treat it as a constant high.
            if (p <= 1) m->out[c] = 1;
            else { m->cnt[c] -= n; while (m->cnt[c] <= 0) { m->cnt[c] += p; m->out[c] ^= 1; } }
            if (m->out[c] && (en >> c & 1)) acc += VOL[m->vol[c]];
        }
        int np = (m->tone[3] & 3) == 3 ? (m->tone[2] ? m->tone[2] : 1) : (0x10 << (m->tone[3] & 3));
        m->cnt[3] -= n;
        while (m->cnt[3] <= 0) {
            m->cnt[3] += np;
            if ((m->nflip ^= 1)) {
                unsigned fb = (m->tone[3] & 4) ? ((m->lfsr ^ (m->lfsr >> 3)) & 1) : (m->lfsr & 1);
                m->lfsr = (uint16_t)((m->lfsr >> 1) | (fb << 15));
            }
        }
        if ((m->lfsr & 1) && (en >> 3 & 1)) acc += VOL[m->vol[3]];
        m->dc += (acc - m->dc) >> 7;            // ~40 Hz high-pass: centre the unipolar output
        int y = acc - m->dc;
        s->pcm[s->psg_pos] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
    }
}

// Bring the PSG up to the CPU's position before a register changes — sample playback lives on it.
static void psg_sync(gg_session_t *s, int elapsed)
{
    int c = s->m->cyc + elapsed;
    if (c > 0) psg_run(s, (int)((int64_t)c * PSG_SPF / FRAME_CYC));
}

static void psg_write(gg_machine_t *m, uint8_t v)
{
    if (v & 0x80) m->psg_latch = (v >> 4) & 7;
    int ch = m->psg_latch >> 1;
    if (m->psg_latch & 1) { m->vol[ch] = v & 15; return; }
    if (ch == 3) { m->tone[3] = v & 7; m->lfsr = 0x8000; return; }
    if (v & 0x80) m->tone[ch] = (uint16_t)((m->tone[ch] & 0x3F0) | (v & 15));
    else          m->tone[ch] = (uint16_t)((m->tone[ch] & 15) | ((v & 0x3F) << 4));
}

// ── the Z80's view of the machine (z80user.h calls these) ──────────────────────────────────────────
static inline uint8_t z80u_rd(void *ctx, unsigned a)
{
    gg_session_t *s = (gg_session_t *)ctx;
    const uint8_t *p = s->rmap[a >> 10];
    return p ? p[a & (PG_SIZE - 1)] : gg_fault(s, a);
}

static inline void z80u_wr(void *ctx, unsigned a, unsigned v)
{
    gg_session_t *s = (gg_session_t *)ctx;
    uint8_t *p = s->wmap[a >> 10];
    if (p && a < 0xFFFC) p[a & (PG_SIZE - 1)] = (uint8_t)v;
    else gg_wr_slow(s, a, (uint8_t)v);
}

static uint8_t z80u_in(void *ctx, unsigned port, int elapsed)
{
    gg_session_t *s = (gg_session_t *)ctx;
    gg_machine_t *m = s->m;
    port &= 0xFF;
    if (port <= 0x06 && !s->sms) {             // Game Gear system ports (absent in SMS mode)
        if (port == 0x00) return (uint8_t)(((s->buttons & NUCLEO_GB_START) ? 0x00 : 0x80) | 0x40);  // export, NTSC
        // The link port with no cable: its registers read back what was written (a game that turns
        // the serial transmitter on waits to see that bit — Pac-Attack), nothing is ever received.
        return port == 0x06 ? 0xFF : m->link[port];
    }
    switch (port & 0xC1) {
        case 0x40: return vcounter(s);
        case 0x41: return hcounter(s, elapsed);
        case 0x80: return vdp_data_r(m);
        case 0x81: return vdp_status_r(m);
        case 0xC0: return s->pad;
        case 0xC1: {                            // port B idle; TH lines echo the I/O control outputs
            uint8_t v = 0xFF;
            if (!(m->io & 0x02)) v = (uint8_t)((v & ~0x40) | ((m->io & 0x20) << 1));
            if (!(m->io & 0x08)) v = (uint8_t)((v & ~0x80) | (m->io & 0x80));
            return v;
        }
        default:   return 0xFF;
    }
}

static int z80u_out(void *ctx, unsigned port, unsigned v, int elapsed)
{
    gg_session_t *s = (gg_session_t *)ctx;
    gg_machine_t *m = s->m;
    port &= 0xFF;
    if (port <= 0x06 && !s->sms) {
        if (port == 0x06) { psg_sync(s, elapsed); m->stereo = (uint8_t)v; }
        else if (port == 0x05) m->link[5] = (uint8_t)(v & 0xF8);   // status bits 0-2: sent, nothing received
        else if (port != 0x04 && port != 0x00) m->link[port] = (uint8_t)v;
        return 0;
    }
    switch (port & 0xC1) {
        case 0x01: m->io = (uint8_t)v; return 0;
        case 0x40: case 0x41: psg_sync(s, elapsed); psg_write(m, (uint8_t)v); return 0;
        case 0x80: vdp_data_w(s, (uint8_t)v); return 0;
        case 0x81: return vdp_ctrl_w(s, (uint8_t)v);
        default:   return 0;
    }
}

#include "vendor/z80emu/z80emu.c"

// Register-decoding tables are pointers into the state itself: rebuild them, keep everything else.
static void z80_fix(Z80_STATE *z)
{
    Z80_STATE t = *z;
    Z80Reset(z);
    z->status = t.status;
    z->registers = t.registers;
    memcpy(z->alternates, t.alternates, sizeof z->alternates);
    z->i = t.i; z->r = t.r; z->pc = t.pc; z->iff1 = t.iff1; z->iff2 = t.iff2; z->im = t.im;
}

// Run the CPU to `until` cycles into the frame. The IRQ is a level held by the VDP, so it is looked
// at wherever it can have changed: before each run, after an EI has let one more instruction through,
// and after a port write that raised it (z80u_out stops the run).
static void run_cpu(gg_session_t *s, int32_t until)
{
    gg_machine_t *m = s->m;
    while (m->cyc < until) {
        if (m->ei_block) {
            m->ei_block = 0;
            m->cyc += Z80Emulate(&m->z, 1, s);
        } else {
            if (m->z.iff1 && irq_line(s)) {
                m->cyc += Z80Interrupt(&m->z, 0xFF, s);
                m->halted = 0;
            }
            if (m->halted) { m->cyc = until; break; }
            m->cyc += Z80Emulate(&m->z, until - m->cyc, s);
        }
        if (m->z.status == Z80_STATUS_HALT) m->halted = 1;
        else if (m->z.status == Z80_STATUS_EI) m->ei_block = 1;
    }
}

// ── the picture ────────────────────────────────────────────────────────────────────────────────────
// A tile row is four bit-planes; pixel k of the row is bit (7-k) of each. SPREAD[n] puts the four bits
// of a nibble into the low bit of four BYTES (leftmost first), so four lookups per nibble build four
// finished pixels at once and a whole tile row leaves as two aligned 32-bit stores — no per-pixel
// loop on the background. MIRROR is the same for horizontally flipped tiles. 128 bytes, not the 2 KB
// a byte-indexed table would cost.
static const uint32_t SPREAD[16] = {
    0x00000000, 0x01000000, 0x00010000, 0x01010000, 0x00000100, 0x01000100, 0x00010100, 0x01010100,
    0x00000001, 0x01000001, 0x00010001, 0x01010001, 0x00000101, 0x01000101, 0x00010101, 0x01010101,
};
static const uint32_t MIRROR[16] = {
    0x00000000, 0x00000001, 0x00000100, 0x00000101, 0x00010000, 0x00010001, 0x00010100, 0x00010101,
    0x01000000, 0x01000001, 0x01000100, 0x01000101, 0x01010000, 0x01010001, 0x01010100, 0x01010101,
};
// Pixels 0-3 (lo) and 4-7 (hi) of one tile row, one colour index 0..15 per byte.
static inline void tile_row(const uint8_t *t, bool flip, uint32_t *lo, uint32_t *hi)
{
    if (!flip) {
        *lo = SPREAD[t[0] >> 4] | SPREAD[t[1] >> 4] << 1 | SPREAD[t[2] >> 4] << 2 | SPREAD[t[3] >> 4] << 3;
        *hi = SPREAD[t[0] & 15] | SPREAD[t[1] & 15] << 1 | SPREAD[t[2] & 15] << 2 | SPREAD[t[3] & 15] << 3;
    } else {
        *lo = MIRROR[t[0] & 15] | MIRROR[t[1] & 15] << 1 | MIRROR[t[2] & 15] << 2 | MIRROR[t[3] & 15] << 3;
        *hi = MIRROR[t[0] >> 4] | MIRROR[t[1] >> 4] << 1 | MIRROR[t[2] >> 4] << 2 | MIRROR[t[3] >> 4] << 3;
    }
}
// Background attributes onto four pixels: palette select on all, priority only where the colour is not 0.
static inline uint32_t tile_attr(uint32_t px, uint32_t pal, bool prio)
{
    if (!prio) return px | pal;
    uint32_t nz = (px | px >> 1 | px >> 2 | px >> 3) & 0x01010101u;
    return px | pal | nz << 5;
}

// The background of one line into s->idx. The buffer is in BACKGROUND space: tile k of the line sits
// at byte 8 + 8k, whatever the fine scroll — so every store is aligned — and screen pixel x is byte
// 8 + x - shift (returned). The leftmost, partly scrolled-in tile is k = -1.
// Four pixels' colour indices (one per byte) -> four panel colours, as two 32-bit stores.
static inline void tile_colour(const uint16_t *pal, uint32_t px, uint32_t *c)
{
    c[0] = pal[px & 31] | (uint32_t)pal[(px >> 8) & 31] << 16;
    c[1] = pal[(px >> 16) & 31] | (uint32_t)pal[(px >> 24) & 31] << 16;
}

// The background of one line, straight to COLOUR (s->cbuf) — the sprites draw over it in place, so
// there is no separate index-to-colour pass over the line (it cost as much as decoding the tiles).
// s->idx keeps each pixel's flags for the sprites: b0-3 colour, b4 palette, b5 background priority,
// b6 a sprite is already here.
// Both buffers are in BACKGROUND space: tile k of the line sits at element 8 + 8k whatever the fine
// scroll, so every store is aligned, and screen pixel x is element 8 + x - shift (shift is returned).
// The leftmost, partly scrolled-in tile is k = -1. A tile row identical to the one before it (sky,
// walls, the flat half of any screen) is copied rather than decoded again.
static int bg_line(gg_session_t *s, int line)
{
    gg_machine_t *m = s->m;
    int hs = ((m->reg[0] & 0x40) && line < 16) ? 0 : m->reg[8];
    int shift = hs & 7, coarse = hs >> 3;
    int nt = (m->reg[2] & 0x0E) << 10;
    int v = (line + m->vscroll) % 224;
    // The name-table row and the row inside a tile, once per line — and once more for the right eight
    // columns when reg 0 bit 7 locks their vertical scroll.
    const uint8_t *nrow = &m->vram[nt + ((v >> 3) << 6)], *lrow = &m->vram[nt + ((line >> 3) << 6)];
    int k_lock = (m->reg[0] & 0x80) ? 24 : 99;
    uint32_t *fl = s->idx, *cw = s->cbuf;
    uint32_t last = 0xFFFFFFFFu, f0 = 0, f1 = 0, c0 = 0, c1 = 0, c2 = 0, c3 = 0;
    for (int k = (s->x0 - shift) >> 3, k1 = (s->x0 + s->w - 1 - shift) >> 3; k <= k1; k++) {
        bool lock = k >= k_lock;
        const uint8_t *e = (lock ? lrow : nrow) + (((k - coarse) & 31) << 1);
        uint32_t attr = e[0] | (e[1] << 8);
        uint32_t r = lock ? (uint32_t)(line & 7) : (uint32_t)(v & 7);
        uint32_t fy = (attr & 0x400) ? 7 - r : r;
        uint32_t key = (attr & 0x1FFF) | fy << 13;
        if (key != last) {
            last = key;
            uint32_t lo, hi;
            tile_row(&m->vram[((attr & 0x1FF) << 5) + (fy << 2)], attr & 0x200, &lo, &hi);
            uint32_t pal = (attr & 0x800) ? 0x10101010u : 0;
            f0 = tile_attr(lo, pal, attr & 0x1000);
            f1 = tile_attr(hi, pal, attr & 0x1000);
            uint32_t c[4];
            tile_colour(s->pal, f0, &c[0]);
            tile_colour(s->pal, f1, &c[2]);
            c0 = c[0]; c1 = c[1]; c2 = c[2]; c3 = c[3];
        }
        fl[2 + 2 * k] = f0; fl[2 + 2 * k + 1] = f1;
        uint32_t *o = &cw[4 + 4 * k];
        o[0] = c0; o[1] = c1; o[2] = c2; o[3] = c3;
    }
    return shift;
}

// Which sprites can touch each band of 8 lines, rebuilt only when the sprite table's Y bytes, its
// base (reg 5) or the sprite size (reg 1) change — once a frame at most, for a game that updates its
// sprites in the vertical blank. A line then looks only at the handful of sprites its band names,
// instead of walking all 64 table entries on every one of 192 lines.
static void spr_bands(gg_session_t *s)
{
    gg_machine_t *m = s->m;
    const uint8_t *sat = &m->vram[(m->reg[5] & 0x7E) << 7];
    int hz = ((m->reg[1] & 2) ? 16 : 8) << (m->reg[1] & 1);
    memset(s->band, 0, sizeof s->band);
    for (int i = 0; i < 64; i++) {
        if (sat[i] == 0xD0) break;              // end of the list in 192-line mode
        int y0 = sat[i] + 1;
        if (y0 > 240) y0 -= 256;                // wraps in from the top
        for (int b = (y0 < 0 ? 0 : y0) >> 3; b < 24 && b * 8 < y0 + hz; b++)
            s->band[b][i >> 5] |= 1u << (i & 31);
    }
    s->band_dirty = false;
}

// Up to eight sprites per line, the ninth sets the overflow flag; the first sprite to cover a pixel
// wins and a second one there sets the collision flag. Evaluated on every active line (the flags are
// the game's to read); drawn only where the Game Gear shows them.
static void spr_line(gg_session_t *s, int line, bool vis, int shift)
{
    gg_machine_t *m = s->m;
    const uint8_t *sat = &m->vram[(m->reg[5] & 0x7E) << 7];
    int zoom = m->reg[1] & 1;
    int h = (m->reg[1] & 2) ? 16 : 8;
    unsigned hz = (unsigned)(h << zoom);
    if (s->band_dirty) spr_bands(s);
    uint8_t list[8];
    int n = 0;
    // The band's candidates, lowest table index first (the order the VDP gives them priority).
    for (int w = 0; w < 2 && n <= 8; w++) {
        uint32_t bits = s->band[line >> 3][w];
        while (bits) {
            int i = (w << 5) + __builtin_ctz(bits);
            bits &= bits - 1;
            if (((unsigned)(line - sat[i] - 1) & 0xFF) >= hz) continue;
            if (n == 8) { m->status |= 0x40; n = 9; break; }
            list[n++] = (uint8_t)i;
        }
    }
    if (n > 8) n = 8;
    if (!vis) return;
    uint8_t *b = (uint8_t *)s->idx + 8 - shift;  // screen x -> background-space element
    uint16_t *cb = (uint16_t *)s->cbuf + 8 - shift;
    int xs = (m->reg[0] & 8) ? 8 : 0;
    int base = (m->reg[6] & 4) << 6;            // 0x100: sprite tiles from the upper half
    int xa = s->x0, xb = s->x0 + s->w;
    for (int j = 0; j < n; j++) {
        int i = list[j];
        int x = sat[0x80 + i * 2] - xs;
        if (x + (8 << zoom) <= xa || x >= xb) continue;
        int tile = sat[0x81 + i * 2] | base;
        if (h == 16) tile &= ~1;
        int r = ((line - sat[i] - 1) & 0xFF) >> zoom;
        uint32_t px[2];
        tile_row(&m->vram[(((tile + (r >> 3)) & 0x1FF) << 5) + ((r & 7) << 2)], false, &px[0], &px[1]);
        const uint8_t *pp = (const uint8_t *)px;
        for (int p = 0; p < 8; p++) {
            uint8_t c = pp[p];
            if (!c) continue;
            for (int z = 0; z <= zoom; z++) {
                int sx = x + (p << zoom) + z;
                if (sx < xa || sx >= xb) continue;
                uint8_t *o = &b[sx];
                if (*o & 0x40) { m->status |= 0x20; continue; }
                if (*o & 0x20) { *o |= 0x40; continue; }        // background priority: hidden, but there
                *o = (uint8_t)(c | 0x50);
                cb[sx] = s->pal[16 | c];
            }
        }
    }
}

static void render_line(gg_session_t *s, int line, bool draw)
{
    gg_machine_t *m = s->m;
    bool vis = draw && line >= s->y0 && line < s->y0 + s->h;
    uint16_t *o = (uint16_t *)s->cbuf + 8;
    if (!(m->reg[1] & 0x40)) {                  // display off: backdrop, no sprites
        if (!vis) return;
        uint16_t c = s->pal[16 + (m->reg[7] & 15)];
        for (int x = 0; x < s->w; x++) o[x] = c;
    } else {
        int shift = vis ? bg_line(s, line) : 0;
#ifdef NUCLEO_GG_PROFILE
        int64_t p0 = esp_timer_get_time();
        spr_line(s, line, vis, shift);
        gg_prof_spr += (uint32_t)(esp_timer_get_time() - p0);
#else
        spr_line(s, line, vis, shift);
#endif
        if (!vis) return;
        o += s->x0 - shift;                     // the line is already colour: point at its window
        // Master System: column 0 can be masked to the backdrop (games hide scroll-in garbage there).
        if (s->sms && (m->reg[0] & 0x20)) for (int x = 0; x < 8; x++) o[x] = s->pal[16 + (m->reg[7] & 15)];
    }
    if (s->on_line) s->on_line(o, line - s->y0, s->user);
}

// ── power-on ───────────────────────────────────────────────────────────────────────────────────────
static void machine_init(gg_session_t *s)
{
    gg_machine_t *m = s->m;
    uint16_t ee[64];                            // the EEPROM survives a reset, like battery RAM
    memcpy(ee, m->ee, sizeof ee);
    memset(m, 0, sizeof *m);
    memcpy(m->ee, ee, sizeof ee);
    m->ee_do = 1;
    Z80Reset(&m->z);
    // Power-on registers as the BIOS hands the cartridge over: SP=DFF0 (IX=IY=FFFF, AF=0040 as a Z80
    // leaves them). It matters more than it looks: some cartridges PUSH before they ever set SP — Ecco
    // the Dolphin and Evander Holyfield Boxing both use "PUSH IX / POP IX" as a delay in their VDP
    // init — so with the stack anywhere near FFFF those pushes land on the mapper registers at
    // FFFC-FFFF and page random banks over the code about to run. z80emu's SP=FFFF froze Evander;
    // MAME's SP=0000 froze Ecco (and "worked" for Evander only by luck). Inside RAM, both boot.
    m->z.registers.word[Z80_SP] = 0xDFF0;
    m->z.registers.word[Z80_IX] = 0xFFFF;
    m->z.registers.word[Z80_IY] = 0xFFFF;
    m->z.registers.word[Z80_AF] = 0x0040;
    // What the BIOS leaves in the VDP, for cartridges that never set every register themselves.
    static const uint8_t R[11] = { 0x36, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFB, 0x00, 0x00, 0x00, 0xFF };
    memcpy(m->reg, R, sizeof R);
    if (s->codies) { m->map[1] = 0; m->map[2] = 1; m->map[3] = 0; }
    else           { m->map[1] = 0; m->map[2] = 1; m->map[3] = 2; }
    m->io = 0xFF;
    m->stereo = 0xFF;
    static const uint8_t LINK[6] = { 0x00, 0x7F, 0xFF, 0x00, 0xFF, 0x00 };
    memcpy(m->link, LINK, sizeof LINK);
    m->lfsr = 0x8000;
    for (int i = 0; i < 4; i++) m->vol[i] = 15;
    for (int i = 0; i < 32; i++) pal_update(s, i);
    remap_all(s);
}

// ── where a cartridge's files live — "<root>/Saves/gg/<rom name>.*", as for the Game Boy ───────────
static void mkdir_one(const char *p)
{
#ifdef _WIN32
    mkdir(p);
#else
    mkdir(p, 0775);
#endif
}

// "<root>/Saves/<sys>/<rom name>" for "<root>/ROMs/<sys>/<rom name>.<ext>" into `base` (the folders
// are created), and the tidied name the menu shows into `title`. No session needed: it runs before
// anything is allocated (see nucleo_gg_open).
static void save_base_init(const char *path, char *base, size_t bn, char *title, size_t tn)
{
    const char *file = strrchr(path, '/');
    const char *bs = strrchr(path, '\\');
    if (!file || (bs && bs > file)) file = bs;
    file = file ? file + 1 : path;
    const char *roms = strstr(path, "/ROMs/");
    if (!roms) roms = strstr(path, "\\ROMs\\");
    char dir[260];
    if (roms && roms < file && file - 1 - (roms + 6) > 0 && file - 1 - (roms + 6) < 32) {
        int root_len = (int)(roms - path), sys_len = (int)(file - 1 - (roms + 6));
        snprintf(dir, sizeof dir, "%.*s/Saves", root_len, path);
        mkdir_one(dir);
        snprintf(dir, sizeof dir, "%.*s/Saves/%.*s", root_len, path, sys_len, roms + 6);
    } else {
        snprintf(dir, sizeof dir, "%.*s/saves", (int)(file - path) > 0 ? (int)(file - 1 - path) : 0, path);
    }
    mkdir_one(dir);
    const char *dot = strrchr(file, '.');
    int base_len = dot ? (int)(dot - file) : (int)strlen(file);
    snprintf(base, bn, "%s/%.*s", dir, base_len, file);

    // The title the menu shows: the file name without extension or the (region) [dump] tags.
    int tl = (int)tn - 1;
    snprintf(title, tn, "%.*s", base_len < tl ? base_len : tl, file);
    char *cut = strstr(title, " (");  if (cut) *cut = '\0';
    cut = strstr(title, " [");        if (cut) *cut = '\0';
}

// ── battery RAM ────────────────────────────────────────────────────────────────────────────────────
// On disk: one flat 32 KB image, byte-compatible with other SMS/GG emulators' .sav. In memory: only
// the non-zero pages, borrowed from the page pool.
static void sav_name(const gg_session_t *s, char *out, size_t n, bool tmp)
{
    snprintf(out, n, "%s.sav%s", s->save_base, tmp ? ".tmp" : "");
}

// Import `SRAM_BYTES` from an open file (NULL = all zero). Pages that stay zero cost nothing.
static bool sram_import(gg_session_t *s, FILE *f)
{
    uint8_t buf[512];
    bool ok = true;
    for (int p = 0; p < SRAM_PAGES; p++) {
        for (int half = 0; half < 2; half++) {
            size_t got = f ? fread(buf, 1, sizeof buf, f) : 0;
            if (got < sizeof buf) { memset(buf + got, 0, sizeof buf - got); if (f) ok = false; f = NULL; }
            bool nz = false;
            for (size_t i = 0; i < sizeof buf && !nz; i++) nz = buf[i] != 0;
            uint8_t *pg = s->sram[p];
            if (!pg && nz && !(pg = sram_alloc(s, p))) { ok = false; continue; }
            if (pg) memcpy(pg + half * 512, buf, 512);
        }
    }
    for (int zp = 32; zp < 48; zp++) if (sram_mapped(s)) map_clear(s, zp);
    return ok;
}

static size_t sram_export(const gg_session_t *s, FILE *f)
{
    uint8_t zero[256];
    memset(zero, 0, sizeof zero);
    size_t done = 0;
    for (int p = 0; p < SRAM_PAGES; p++) {
        if (s->sram[p]) { done += fwrite(s->sram[p], 1, PG_SIZE, f); continue; }
        for (int i = 0; i < 4; i++) done += fwrite(zero, 1, sizeof zero, f);
    }
    return done;
}

static void sav_load(gg_session_t *s)
{
    char p[320];
    sav_name(s, p, sizeof p, false);
    FILE *f = fopen(p, "rb");
    if (!f) { sav_name(s, p, sizeof p, true); f = fopen(p, "rb"); }
    if (!f) return;
    if (s->eeprom) { if (fread(s->m->ee, 1, sizeof s->m->ee, f) != sizeof s->m->ee) memset(s->m->ee, 0xFF, sizeof s->m->ee); }
    else sram_import(s, f);
    fclose(f);
    s->sram_present = true;
    ESP_LOGI(TAG, "%s loaded", s->eeprom ? "EEPROM" : "battery RAM");
}

void nucleo_gg_save(void)
{
    gg_session_t *s = S;
    if (!s || !s->sram_dirty || (!s->sram_present && !s->eeprom)) return;
    esp_task_wdt_reset();
    char tmp[320], fin[320];
    sav_name(s, tmp, sizeof tmp, true);
    sav_name(s, fin, sizeof fin, false);
    struct stat sb;
    if (stat(fin, &sb) != 0 && stat(tmp, &sb) == 0 && rename(tmp, fin) != 0) {
        ESP_LOGW(TAG, "cannot recover %s — not overwriting it", tmp);
        return;
    }
    FILE *f = fopen(tmp, "wb");
    if (!f) { ESP_LOGW(TAG, "cannot write %s", tmp); return; }
    // An EEPROM cartridge's .sav is the EEPROM itself, 128 bytes; battery RAM is the flat 32 KB image.
    bool ok = s->eeprom ? fwrite(s->m->ee, 1, sizeof s->m->ee, f) == sizeof s->m->ee
                        : sram_export(s, f) == SRAM_BYTES;
    ok = (fclose(f) == 0) && ok;
    if (!ok) { remove(tmp); ESP_LOGW(TAG, "save write failed — previous save kept"); return; }
    remove(fin);
    if (rename(tmp, fin) != 0) { ESP_LOGW(TAG, "save rename failed — kept as %s", tmp); return; }
    s->sram_dirty = false;
    s->sram_present = true;
    esp_task_wdt_reset();
}

bool nucleo_gg_autosave(void)
{
    gg_session_t *s = S;
    if (!s || !s->sram_dirty || s->clock - s->sram_touch < 90) return false;
    nucleo_gg_save();
    return !s->sram_dirty;
}

// ── lifecycle ──────────────────────────────────────────────────────────────────────────────────────
static void session_free(gg_session_t *s)
{
    if (!s) return;
    if (s->sound) { s->sound = false; nucleo_audio_pcm_close(); }
    if (s->fp) fclose(s->fp);
    for (int i = 0; i < s->pg_n; i++) free(s->pg[i]);
    free(s->m);
    free(s);
}

// ── cartridges that need more than the Sega mapper ───────────────────────────────────────────────────
// Keyed on the CRC-32 of the file, compiled from the public cartridge databases (SMS Power!, the Genesis
// Plus GX game list): Game Gear releases that are Master System games running in the GG's SMS mode,
// Codemasters boards whose header the heuristic below might miss, and the 93C46 EEPROM carts.
enum { CART_SMS = 1, CART_CODIES = 2, CART_EEPROM = 4 };
static const struct { uint32_t crc; uint8_t flags; } CARTS[] = {
    { 0x01A2D595, CART_SMS                   },   // Street Battle [Proto] [SMS-GG] (US)
    { 0x01EAB89D, CART_SMS                   },   // Out Run Europa [SMS-GG]
    { 0x10DBBEF4, CART_SMS                   },   // Super Kick Off [SMS-GG]
    { 0x152F0DCC, CART_CODIES                },   // Drop Zone
    { 0x1D93246E, CART_SMS                   },   // Olympic Gold [A][SMS-GG]
    { 0x2306AAF4, CART_CODIES                },   // Dinobasher - Starring Bignose the Caveman [GG] [Proto]
    { 0x29822980, CART_CODIES                },   // Cosmic Spacehead
    { 0x2DA8E943, CART_EEPROM                },   // Pro Yakyuu GG League
    { 0x311D2863, CART_SMS                   },   // Prince of Persia [A][SMS-GG]
    { 0x3382D73F, CART_SMS                   },   // Olympic Gold (TW) [SMS-GG]
    { 0x354BEE78, CART_SMS                   },   // Paperboy [v1] (TW) [SMS-GG]
    { 0x36EBCD6D, CART_EEPROM                },   // Majors Pro Baseball
    { 0x3ACE6335, CART_CODIES                },   // CJ Elephant Fugitive [Proto]
    { 0x3D8D0DD6, CART_EEPROM                },   // World Series Baseball [v0]
    { 0x44136A72, CART_SMS                   },   // Forgotten Worlds (TW) [SMS-GG]
    { 0x45F058D6, CART_SMS                   },   // Prince of Persia [B][SMS-GG]
    { 0x4762E022, CART_SMS                   },   // Kung Fu Kid (TW) [SMS-GG]
    { 0x55F929CE, CART_SMS                   },   // Choplifter (TW) [SMS-GG]
    { 0x56201996, CART_SMS                   },   // R.C. Grand Prix [SMS-GG]
    { 0x578A8A38, CART_EEPROM                },   // World Series Baseball '95
    { 0x59840FD6, CART_SMS                   },   // Castle of Illusion - Starring Mickey Mouse [SMS-GG]
    { 0x5E4B454E, CART_SMS                   },   // Argos no Juujiken (TW) [SMS-GG]
    { 0x5E53C7F7, CART_CODIES                },   // Ernie Els Golf
    { 0x5E7B18C8, CART_SMS                   },   // Wonder Kid [Proto]
    { 0x63A7F906, CART_SMS                   },   // Strider (TW) [SMS-GG]
    { 0x6630E5FD, CART_SMS                   },   // Aerial Assault (TW) [SMS-GG]
    { 0x6CAA625B, CART_CODIES                },   // Cosmic Spacehead [GG]
    { 0x6F8E46CF, CART_SMS                   },   // Alex Kidd in Miracle World (TW) [SMS-GG]
    { 0x6FE448A5, CART_SMS                   },   // Great Basketball (TW) [SMS-GG]
    { 0x72981057, CART_CODIES                },   // CJ Elephant Fugitive
    { 0x76C5BDFB, CART_SMS                   },   // Jang Pung II [SMS-GG] (KR)
    { 0x7BB81E3D, CART_SMS                   },   // Taito Chase H.Q (J) [SMS-GG]
    { 0x7D59283B, CART_SMS                   },   // Scramble Spirits (TW) [SMS-GG]
    { 0x7EAED675, CART_SMS                   },   // Lord of Sword (TW) [SMS-GG]
    { 0x8813514B, CART_CODIES                },   // Excellent Dizzy Collection, The [Proto]
    { 0x89EFCC22, CART_SMS                   },   // Secret Command (TW) [SMS-GG]
    { 0x96E16FE4, CART_SMS                   },   // E-SWAT [v1] (TW) [SMS-GG]
    { 0x98CF1254, CART_SMS                   },   // Thunder Blade (TW) [SMS-GG]
    { 0x98F64975, CART_SMS                   },   // Black Belt (TW) [SMS-GG]
    { 0x9942B69B, CART_SMS                   },   // Castle of Illusion - Starring Mickey Mouse (J) [SMS-GG]
    { 0x9C76FB3A, CART_SMS                   },   // Rastan Saga (J) [SMS-GG]
    { 0x9FA727A0, CART_SMS                   },   // Street Hero [Proto 0] [SMS-GG] (US)
    { 0xA2F9C7AF, CART_SMS                   },   // Olympic Gold [B][SMS-GG]
    { 0xA577CE46, CART_CODIES                },   // Micro Machines
    { 0xAA140C9C, CART_SMS | CART_CODIES     },   // Excellent Dizzy Collection, The [SMS-GG]
    { 0xAB67C6BD, CART_SMS                   },   // Shadow Dancer - The Secret Of Shinobi (TW) [SMS-GG]
    { 0xAC2EA669, CART_SMS                   },   // Shadow of the Beast (TW) [SMS-GG]
    { 0xAD9FF469, CART_SMS                   },   // Cyber Shinobi, The (TW) [SMS-GG]
    { 0xB6207F0D, CART_SMS                   },   // Hokuto no Ken (TW) [SMS-GG]
    { 0xB948752E, CART_SMS                   },   // Final Bubble Bobble (TW) [SMS-GG]
    { 0xB9664AE1, CART_CODIES                },   // Fantastic Dizzy
    { 0xBB38CFD7, CART_EEPROM                },   // World Series Baseball [v1]
    { 0xC1756BEE, CART_CODIES                },   // Pete Sampras Tennis
    { 0xC21E6CD0, CART_CODIES                },   // Micro Machines [GG] [Proto]
    { 0xC597BA5D, CART_CODIES                },   // Pete Sampras Tennis (US)
    { 0xC8381DEF, CART_SMS                   },   // Taito Chase H.Q [SMS-GG]
    { 0xC888222B, CART_SMS | CART_CODIES     },   // Fantastic Dizzy [SMS-GG]
    { 0xCACDF759, CART_SMS                   },   // Quartet (TW) [SMS-GG]
    { 0xCAFD2D83, CART_SMS                   },   // Prince of Persia (TW) [SMS-GG]
    { 0xCC521975, CART_SMS                   },   // Cave Dude [Proto] [SMS-GG]
    { 0xD0263024, CART_SMS                   },   // Seishun Scandal (TW) [SMS-GG]
    { 0xD282EF71, CART_SMS                   },   // Submarine Attack (TW) [SMS-GG]
    { 0xD9A7F170, CART_CODIES                },   // Man Overboard!
    { 0xDA8E95A9, CART_SMS                   },   // WWF Wrestlemania Steel Cage Challenge [SMS-GG]
    { 0xDBE8895C, CART_CODIES                },   // Micro Machines 2 - Turbo Tournament
    { 0xE532716F, CART_SMS                   },   // R-Type (TW) [SMS-GG]
    { 0xE5F789B9, CART_SMS                   },   // Predator 2 [SMS-GG]
    { 0xEA5C3A6F, CART_CODIES                },   // Dinobasher - Starring Bignose the Caveman [Proto]
    { 0xF037EC00, CART_SMS                   },   // Out Run Europa (US) [SMS-GG]
    { 0xF4F848C2, CART_SMS                   },   // Double Dragon (TW) [SMS-GG]
    { 0xF7C524F6, CART_CODIES                },   // Micro Machines [GG]
    { 0xFB163003, CART_SMS                   },   // Doki Doki Penguin Land - Uchuu-Daibouken (TW) [SMS-GG]
    { 0xFB481971, CART_SMS                   },   // Street Hero [Proto 1] [SMS-GG] (US)
};

static uint8_t cart_flags(uint32_t crc)
{
    for (size_t i = 0; i < sizeof CARTS / sizeof CARTS[0]; i++) if (CARTS[i].crc == crc) return CARTS[i].flags;
    return 0;
}

// CRC-32 (IEEE), a nibble at a time: a 64-byte table instead of 1 KB.
static uint32_t crc_step(uint32_t c, const uint8_t *p, size_t n)
{
    static const uint32_t T[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
    };
    while (n--) { c ^= *p++; c = (c >> 4) ^ T[c & 15]; c = (c >> 4) ^ T[c & 15]; }
    return c;
}

// The cartridge's CRC, read once per ROM and then remembered in "<save_base>.id" (size + CRC): hashing
// 512 KB over the SD bus is ~0.5 s, a cost worth paying on the first launch only.
static uint32_t cart_crc(gg_session_t *s)
{
    char p[320];
    snprintf(p, sizeof p, "%s.id", s->save_base);
    uint32_t id[2] = { 0, 0 };
    FILE *f = fopen(p, "rb");
    if (f) {
        bool ok = fread(id, 1, sizeof id, f) == sizeof id;
        fclose(f);
        if (ok && id[0] == s->rom_bytes) return id[1];
    }
    uint8_t *buf = (uint8_t *)malloc(4096);    // before the page pool exists, so there is room for it
    if (!buf) return 0;
    uint32_t c = 0xFFFFFFFFu;
    size_t got;
    fseek(s->fp, 0, SEEK_SET);
    while ((got = fread(buf, 1, 4096, s->fp)) > 0) { c = crc_step(c, buf, got); esp_task_wdt_reset(); }
    free(buf);
    c = ~c;
    id[0] = s->rom_bytes; id[1] = c;
    if ((f = fopen(p, "wb"))) { fwrite(id, 1, sizeof id, f); fclose(f); }
    return c;
}

static bool ext_is(const char *path, const char *ext)
{
    size_t n = strlen(path), e = strlen(ext);
    if (n < e) return false;
    for (size_t i = 0; i < e; i++) {
        char a = path[n - e + i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

// Codemasters cartridges carry a checksum at 7FE6 and its complement at 7FE8 instead of relying on
// the Sega mapper registers; their bank switches are writes to 0000/4000/8000.
static bool codies_header(FILE *f, uint32_t off, uint32_t size)
{
    uint8_t h[16];
    if (size < off + 0x8000 || fseek(f, (long)(off + 0x7FE0), SEEK_SET) != 0 || fread(h, 1, 16, f) != 16) return false;
    unsigned sum = h[6] | (h[7] << 8), inv = h[8] | (h[9] << 8);
    return sum != 0 && ((sum + inv) & 0xFFFF) == 0;
}

typedef struct { uint32_t method, csize, usize, lho; bool sms; } zip_entry_t;
static esp_err_t zip_find(FILE *f, zip_entry_t *e);

esp_err_t nucleo_gg_probe(const char *rom_path, nucleo_gg_info_t *out)
{
    if (!rom_path || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    FILE *f = fopen(rom_path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    if (ext_is(rom_path, ".zip")) {             // the cartridge inside, from the central directory
        zip_entry_t e;
        esp_err_t ze = zip_find(f, &e);
        fclose(f);
        if (ze != ESP_OK) return ze;
        out->rom_bytes = e.usize;
        out->sms = e.sms;
        return e.usize < 0x2000 ? ESP_ERR_INVALID_SIZE : ESP_OK;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    uint32_t off = (sz > 0 && (sz % 0x4000) == 512) ? 512 : 0;
    out->rom_bytes = sz > 0 ? (uint32_t)sz : 0;
    out->codemasters = sz >= 0x2000 && codies_header(f, off, (uint32_t)sz);
    out->sms = ext_is(rom_path, ".sms");        // SMS-mode .gg cartridges are known only after open (CRC)
    fclose(f);
    return sz < 0x2000 ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

// ── zipped cartridges ──────────────────────────────────────────────────────────────────────────────
// The library folders are shared with the browser Arcade, which reads .zip — so a cartridge may well
// be zipped. It is unpacked ONCE, beside its saves ("<save_base>.gg" / ".sms"), and played from there;
// a later launch finds that copy (the size the zip promises) and goes straight to it. Deflate needs a
// 32 KB window: it is allocated before the console exists, while the heap's one 32 KB block is free,
// and the inflater itself is the one in the ESP32-S3's mask ROM — no flash spent on it.
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

#ifdef NUCLEO_HOST_ZLIB
#include <zlib.h>
static bool inflate_to(FILE *in, uint32_t csize, FILE *out, uint32_t usize)
{
    z_stream z; memset(&z, 0, sizeof z);
    if (inflateInit2(&z, -15) != Z_OK) return false;
    uint8_t ib[4096], ob[4096];
    uint32_t left = csize, written = 0;
    int st = Z_OK;
    while (st != Z_STREAM_END) {
        if (!z.avail_in && left) {
            size_t want = left < sizeof ib ? left : sizeof ib, got = fread(ib, 1, want, in);
            if (!got) break;
            left -= (uint32_t)got; z.next_in = ib; z.avail_in = (uInt)got;
        }
        z.next_out = ob; z.avail_out = sizeof ob;
        st = inflate(&z, Z_NO_FLUSH);
        if (st != Z_OK && st != Z_STREAM_END) break;
        size_t n = sizeof ob - z.avail_out;
        if (n && fwrite(ob, 1, n, out) != n) break;
        written += (uint32_t)n;
    }
    inflateEnd(&z);
    return st == Z_STREAM_END && written == usize;
}
#else
#include "miniz.h"                              // tinfl, in the chip's ROM
static bool inflate_to(FILE *in, uint32_t csize, FILE *out, uint32_t usize)
{
    tinfl_decompressor *d = (tinfl_decompressor *)malloc(sizeof *d);
    uint8_t *dict = (uint8_t *)malloc(TINFL_LZ_DICT_SIZE);
    uint8_t *ib = (uint8_t *)malloc(1024);
    bool ok = false;
    if (!(d && dict && ib))
        ESP_LOGW(TAG, "inflate: no room for the 32 KB window (largest block %u, free %u)",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    if (d && dict && ib) {
        tinfl_init(d);
        size_t avail = 0, at = 0, dofs = 0;
        uint32_t left = csize, written = 0, turns = 0;
        for (;;) {
            if (!avail && left) {
                avail = fread(ib, 1, left < 1024 ? left : 1024, in);
                if (!avail) break;
                left -= (uint32_t)avail; at = 0;
            }
            size_t ic = avail, oc = TINFL_LZ_DICT_SIZE - dofs;
            tinfl_status st = tinfl_decompress(d, ib + at, &ic, dict, dict + dofs, &oc,
                                               left ? TINFL_FLAG_HAS_MORE_INPUT : 0);
            avail -= ic; at += ic;
            if (oc) {
                if (fwrite(dict + dofs, 1, oc, out) != oc) break;
                written += (uint32_t)oc;
                dofs = (dofs + oc) & (TINFL_LZ_DICT_SIZE - 1);
            }
            if (st == TINFL_STATUS_DONE) { ok = written == usize; break; }
            if (st < 0) break;
            if ((++turns & 63) == 0) esp_task_wdt_reset();
        }
    }
    free(ib); free(dict); free(d);
    return ok;
}
#endif

// Unpack the first Game Gear / Master System image in `zip`; `out` receives its path.
// The first Game Gear / Master System image listed in the zip's central directory. No allocation, so
// the shelf's header probe can use it too and refuse a zip with no cartridge in it up front.
static esp_err_t zip_find(FILE *f, zip_entry_t *e)
{
    uint8_t b[128];
    fseek(f, 0, SEEK_END);
    long zsz = ftell(f), tail = zsz < (long)sizeof b ? zsz : (long)sizeof b;
    int eo = -1;                                // the end-of-central-directory record, from the back
    if (tail >= 22 && fseek(f, zsz - tail, SEEK_SET) == 0 && fread(b, 1, (size_t)tail, f) == (size_t)tail)
        for (long i = tail - 22; i >= 0; i--) if (rd32(b + i) == 0x06054b50) { eo = (int)i; break; }
    if (eo < 0) return ESP_ERR_INVALID_SIZE;
    unsigned count = rd16(b + eo + 10);
    long pos = (long)rd32(b + eo + 16);
    for (unsigned i = 0; i < count; i++) {
        if (fseek(f, pos, SEEK_SET) != 0 || fread(b, 1, 46, f) != 46 || rd32(b) != 0x02014b50) break;
        e->method = rd16(b + 10); e->csize = rd32(b + 20); e->usize = rd32(b + 24); e->lho = rd32(b + 42);
        unsigned nlen = rd16(b + 28), skip = rd16(b + 30) + rd16(b + 32);
        char name[256];
        size_t take = nlen < sizeof name - 1 ? nlen : sizeof name - 1;
        if (fread(name, 1, take, f) != take) break;
        name[take] = '\0';
        if (ext_is(name, ".gg") || ext_is(name, ".sms")) {
            e->sms = ext_is(name, ".sms");
            return (e->method == 0 || e->method == 8) ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
        }
        pos += 46 + (long)nlen + (long)skip;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t unzip_rom(const char *save_base, const char *zip, char *out, size_t n)
{
    FILE *f = fopen(zip, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    zip_entry_t e;
    esp_err_t ze = zip_find(f, &e);
    if (ze != ESP_OK) { fclose(f); return ze; }
    uint32_t method = e.method, csize = e.csize, usize = e.usize, lho = e.lho;
    snprintf(out, n, "%s.%s", save_base, e.sms ? "sms" : "gg");
    struct stat sb;
    if (stat(out, &sb) == 0 && (uint32_t)sb.st_size == usize) { fclose(f); return ESP_OK; }   // unpacked before

    ESP_LOGI(TAG, "unpacking %s (%u -> %u B)", zip, (unsigned)csize, (unsigned)usize);
    uint8_t b[320];
    if (fseek(f, (long)lho, SEEK_SET) != 0 || fread(b, 1, 30, f) != 30 || rd32(b) != 0x04034b50 ||
        fseek(f, (long)lho + 30 + rd16(b + 26) + rd16(b + 28), SEEK_SET) != 0) { fclose(f); return ESP_ERR_INVALID_SIZE; }
    char tmp[340];
    snprintf(tmp, sizeof tmp, "%s.tmp", out);
    FILE *o = fopen(tmp, "wb");
    if (!o) { fclose(f); return ESP_FAIL; }
    bool ok;
    if (method == 8) ok = inflate_to(f, csize, o, usize);
    else {                                      // stored: a copy
        uint32_t left = usize;
        ok = true;
        while (left && ok) {
            size_t want = left < sizeof b ? left : sizeof b;
            ok = fread(b, 1, want, f) == want && fwrite(b, 1, want, o) == want;
            left -= (uint32_t)want;
            if ((left & 0x3FFF) < sizeof b) esp_task_wdt_reset();
        }
    }
    ok = (fclose(o) == 0) && ok;
    fclose(f);
    if (!ok) { remove(tmp); return ESP_ERR_NO_MEM; }  // the likeliest cause: no 32 KB block for the window
    remove(out);
    return rename(tmp, out) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t nucleo_gg_open(const char *rom_path, nucleo_gg_line_fn on_line, void *user)
{
    if (!rom_path || !*rom_path) return ESP_ERR_INVALID_ARG;
    nucleo_gg_close();

    // Saves follow the name in the library, zipped or not. Worked out on the stack: a zipped cartridge
    // is inflated BEFORE anything of ours is allocated, so its 32 KB window finds the heap's one 32 KB
    // block whole, whatever the session struct grows to.
    char base[300], title[40], rom[330];
    save_base_init(rom_path, base, sizeof base, title, sizeof title);
    if (ext_is(rom_path, ".zip")) {
        esp_err_t ze = unzip_rom(base, rom_path, rom, sizeof rom);
        if (ze != ESP_OK) { ESP_LOGW(TAG, "cannot unpack %s (0x%x)", rom_path, (unsigned)ze); return ze; }
    } else snprintf(rom, sizeof rom, "%s", rom_path);

    gg_session_t *s = (gg_session_t *)calloc(1, sizeof *s);
    if (!s) return ESP_ERR_NO_MEM;
    s->heap_bytes = sizeof *s;
    s->on_line = on_line; s->user = user;
    snprintf(s->rom_path, sizeof s->rom_path, "%s", rom_path);
    memcpy(s->save_base, base, sizeof base);
    memcpy(s->title, title, sizeof title);

    // The console next, while the heap's biggest block is still whole: it is the one ~25 KB piece.
    gg_machine_t *m = (gg_machine_t *)calloc(1, sizeof *m);
    if (!m) {
        ESP_LOGE(TAG, "no RAM for the console (%u B)", (unsigned)sizeof *m);
        session_free(s);
        return ESP_ERR_NO_MEM;
    }
    s->m = m;
    s->heap_bytes += sizeof *m;

    s->fp = fopen(rom, "rb");
    if (!s->fp) { session_free(s); return ESP_ERR_NOT_FOUND; }
    fseek(s->fp, 0, SEEK_END);
    long sz = ftell(s->fp);
    if (sz < 0x2000) { session_free(s); return ESP_ERR_INVALID_SIZE; }
    s->rom_bytes = (uint32_t)sz;
    s->rom_off = (sz % 0x4000) == 512 ? 512 : 0;
    s->banks = (s->rom_bytes - s->rom_off + 0x3FFF) / 0x4000;
    s->crc = cart_crc(s);
    uint8_t flags = cart_flags(s->crc);
    s->codies = (flags & CART_CODIES) || codies_header(s->fp, s->rom_off, s->rom_bytes);
    s->eeprom = (flags & CART_EEPROM) != 0;
    s->sms = (flags & CART_SMS) || ext_is(rom, ".sms");

    // The speaker before the pool, so its DMA channel is paid for before the pool takes what is left.
    s->sound = nucleo_audio_pcm_open(PSG_RATE, 1) == ESP_OK;
    if (!s->sound) ESP_LOGW(TAG, "speaker unavailable — playing silently");

    for (int i = 0; i < PG_SLOTS; i++) s->pg_tag[i] = PG_EMPTY;
    memset(s->hint, -1, sizeof s->hint);
    for (int i = 0; i < 64; i++) s->rslot[i] = -1;
    uint32_t rom_pages = (s->rom_bytes - s->rom_off + PG_SIZE - 1) / PG_SIZE;
    for (int i = 0; i < PG_SLOTS && (uint32_t)i < rom_pages + SRAM_PAGES; i++) {
        if (heap_caps_get_free_size(MALLOC_CAP_DEFAULT) < PG_SIZE + KEEP) break;
        uint8_t *b = (uint8_t *)malloc(PG_SIZE);
        if (!b) break;
        s->pg[s->pg_n++] = b;
        s->heap_bytes += PG_SIZE;
    }
    if (s->pg_n < PG_MIN) {
        ESP_LOGE(TAG, "only %d cartridge pages fit (need %d)", s->pg_n, PG_MIN);
        session_free(s);
        return ESP_ERR_NO_MEM;
    }

    s->pad = 0xFF;
    if (s->sms) { s->x0 = 0;     s->y0 = 0;     s->w = 256;         s->h = 192; }
    else        { s->x0 = GG_X0; s->y0 = GG_Y0; s->w = NUCLEO_GG_W; s->h = NUCLEO_GG_H; }

    S = s;
    memset(m->ee, 0xFF, sizeof m->ee);          // an erased EEPROM reads all ones
    machine_init(s);
    sav_load(s);
    ESP_LOGI(TAG, "'%s' %uKB crc %08X%s%s%s | %d pages | heap %u B | sound %s", s->title,
             (unsigned)(s->rom_bytes / 1024), (unsigned)s->crc, s->sms ? " sms-mode" : "",
             s->codies ? " codemasters" : "", s->eeprom ? " eeprom" : "",
             s->pg_n, (unsigned)s->heap_bytes, s->sound ? "on" : "off");
    return ESP_OK;
}

void nucleo_gg_close(void)
{
    if (!S) return;
    nucleo_gg_save();
    gg_session_t *s = S;
    S = NULL;
    session_free(s);
}

bool nucleo_gg_is_open(void) { return S != NULL; }

void nucleo_gg_run_frame(void)
{
    gg_session_t *s = S;
    if (!s) return;
    gg_machine_t *m = s->m;
    int64_t t0 = esp_timer_get_time();
    bool draw = !(s->frameskip && (s->clock & 1));
    s->psg_pos = 0;
    // Recency, not residency. A page stays in rmap for as long as its bank stays switched in, which
    // for the code in slots 0-1 is the whole game — so "mapped" says nothing about "used". Forgetting
    // the ROM mappings once a frame makes the next touch of each page a soft fault (a hint lookup, no
    // SD read) that stamps its age: eviction then sees what THIS frame actually used.
    for (int zp = 0; zp < 48; zp++) if (s->rslot[zp] >= 0) map_clear(s, zp);
    s->frame_clock = s->pg_clock;
    if (m->nmi) {                               // Pause (Master System mode) is the Z80's NMI
        m->nmi = 0;
        m->cyc += Z80NonMaskableInterrupt(&m->z, s);
        m->halted = 0;
    }
    for (int line = 0; line < LINES; line++) {
        s->vline = line;
        if (line == 0) m->vscroll = m->reg[9];  // V scroll is latched once per frame
#ifdef NUCLEO_GG_PROFILE                        // the QEMU bench's split (tools/emu-host/qemu-bench-gg)
        int64_t pr = esp_timer_get_time();
        if (line < ACTIVE) render_line(s, line, draw);
        gg_prof_render += (uint32_t)(esp_timer_get_time() - pr);
#else
        if (line < ACTIVE) render_line(s, line, draw);
#endif
        if (line <= ACTIVE) {                   // the line counter runs through the active display
            if (m->lc == 0) { m->lc = m->reg[10]; m->line_irq = 1; }
            else m->lc--;
        } else m->lc = m->reg[10];
        if (line == ACTIVE) m->status |= 0x80;
        run_cpu(s, (line + 1) * CYC_LINE);
    }
    m->cyc -= FRAME_CYC;
    int64_t t1 = esp_timer_get_time();
    s->us_cpu += (uint32_t)(t1 - t0);
    psg_run(s, PSG_SPF);
    if (s->sound) {
        nucleo_audio_pcm_write(s->pcm, sizeof s->pcm);
        s->us_audio += (uint32_t)(esp_timer_get_time() - t1);
    }
    s->frames++;
    s->clock++;
}

void nucleo_gg_reset(void)
{
    if (!S) return;
    nucleo_gg_save();
    machine_init(S);
}

void nucleo_gg_set_buttons(uint8_t b)
{
    if (!S) return;
    // Master System mode has no Start: the key is Pause, an edge (one NMI per press, not per frame held).
    if (S->sms && (b & NUCLEO_GB_START) && !(S->buttons & NUCLEO_GB_START)) S->m->nmi = 1;
    S->buttons = b;
    uint8_t p = 0xFF;
    if (b & NUCLEO_GB_UP)    p &= ~0x01;
    if (b & NUCLEO_GB_DOWN)  p &= ~0x02;
    if (b & NUCLEO_GB_LEFT)  p &= ~0x04;
    if (b & NUCLEO_GB_RIGHT) p &= ~0x08;
    if (b & NUCLEO_GB_B)     p &= ~0x10;        // button 1 (left thumb)
    if (b & NUCLEO_GB_A)     p &= ~0x20;        // button 2 (right thumb)
    S->pad = p;
}

const char *nucleo_gg_title(void) { return S ? S->title : ""; }
void nucleo_gg_geometry(int *w, int *h)
{
    if (w) *w = S ? S->w : NUCLEO_GG_W;
    if (h) *h = S ? S->h : NUCLEO_GG_H;
}
void nucleo_gg_set_frameskip(bool on) { if (S) S->frameskip = on; }

void nucleo_gg_reset_counters(void)
{
    if (!S) return;
    S->us_cpu = S->us_audio = 0;
    S->misses = 0;
    S->frames = 0;
}

void nucleo_gg_get_stats(nucleo_gb_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    gg_session_t *s = S;
    if (!s) return;
    uint32_t rom_pages = (s->rom_bytes - s->rom_off + PG_SIZE - 1) / PG_SIZE;
    out->heap_bytes   = s->heap_bytes;
    out->rom_bytes    = s->rom_bytes;
    out->rom_pages    = s->pg_n - s->pg_pinned;
    out->rom_resident = (uint32_t)out->rom_pages >= rom_pages;
    out->rom_paged    = !out->rom_resident;
    out->bank_misses  = s->misses;
    out->frames       = s->frames;
    out->us_cpu       = s->us_cpu;
    out->us_audio     = s->us_audio;
    out->cart_type    = s->codies ? 1 : 0;
    out->save_dirty   = s->sram_dirty;
}

// ── save states ────────────────────────────────────────────────────────────────────────────────────
// "NGG1", sizeof(gg_machine_t), battery-RAM bytes (0 or 32 KB), the machine, the battery RAM.
#define STATE_MAGIC 0x3147474Eu

static void state_path(char *out, size_t n, int slot)
{
    snprintf(out, n, "%s.st%d", S->save_base, slot < 0 ? 0 : (slot > 9 ? 9 : slot));
}

bool nucleo_gg_state_exists(int slot)
{
    if (!S) return false;
    char p[320]; state_path(p, sizeof p, slot);
    struct stat sb;
    return stat(p, &sb) == 0 && (size_t)sb.st_size >= 12 + sizeof(gg_machine_t);
}

static esp_err_t state_write(const char *p)
{
    esp_task_wdt_reset();                       // ~25-57 KB to the card: fed like the battery-RAM save
    FILE *f = fopen(p, "wb");
    if (!f) return ESP_FAIL;
    uint32_t hdr[3] = { STATE_MAGIC, (uint32_t)sizeof(gg_machine_t), (S->sram_present && !S->eeprom) ? SRAM_BYTES : 0 };
    bool ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr
           && fwrite(S->m, 1, sizeof(gg_machine_t), f) == sizeof(gg_machine_t);
    esp_task_wdt_reset();
    ok = ok && (!hdr[2] || sram_export(S, f) == SRAM_BYTES);
    ok = (fclose(f) == 0) && ok;
    esp_task_wdt_reset();
    if (!ok) { remove(p); return ESP_FAIL; }
    return ESP_OK;
}

esp_err_t nucleo_gg_state_save(int slot)
{
    if (!S) return ESP_ERR_INVALID_STATE;
    char p[320]; state_path(p, sizeof p, slot);
    return state_write(p);
}

// Header checks: right magic, a machine of THIS build's size, a length that adds up.
static bool state_header(const char *p, uint32_t *sram)
{
    struct stat sb;
    if (stat(p, &sb) != 0) return false;
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    uint32_t h[3] = { 0 };
    bool ok = fread(h, 1, sizeof h, f) == sizeof h && h[0] == STATE_MAGIC && h[1] == sizeof(gg_machine_t)
           && (h[2] == 0 || h[2] == SRAM_BYTES) && (size_t)sb.st_size == sizeof h + h[1] + h[2];
    fclose(f);
    *sram = h[2];
    return ok;
}

// Straight into the live console — there is no room for a second 25 KB copy, and the game task's
// stack is 8 KB. Everything derived (pointer tables, palette, page map) is rebuilt afterwards.
static bool state_read_live(const char *p, uint32_t sram)
{
    esp_task_wdt_reset();
    FILE *f = fopen(p, "rb");
    bool ok = f && fseek(f, 12, SEEK_SET) == 0 && fread(S->m, 1, sizeof(gg_machine_t), f) == sizeof(gg_machine_t);
    esp_task_wdt_reset();
    if (ok && sram) { ok = sram_import(S, f); S->sram_present = true; }
    if (f) fclose(f);
    esp_task_wdt_reset();
    z80_fix(&S->m->z);
    for (int i = 0; i < 32; i++) pal_update(S, i);
    remap_all(S);
    return ok;
}

esp_err_t nucleo_gg_state_load(int slot)
{
    if (!S) return ESP_ERR_INVALID_STATE;
    char p[320]; state_path(p, sizeof p, slot);
    struct stat sb;
    if (stat(p, &sb) != 0) return ESP_ERR_NOT_FOUND;
    uint32_t sram = 0;
    if (!state_header(p, &sram)) { ESP_LOGW(TAG, "state %d: rejected", slot); return ESP_ERR_INVALID_CRC; }

    // A read that fails half way would leave half of one game and half of another: keep the running
    // console in "<rom>.stu" first and put it back if the load does not complete.
    char undo[320];
    snprintf(undo, sizeof undo, "%s.stu", S->save_base);
    bool have_undo = state_write(undo) == ESP_OK;
    uint32_t undo_sram = (S->sram_present && !S->eeprom) ? SRAM_BYTES : 0;
    if (!state_read_live(p, sram)) {
        ESP_LOGW(TAG, "state %d: read failed — restoring the running game", slot);
        if (!(have_undo && state_read_live(undo, undo_sram))) { machine_init(S); sav_load(S); }
        if (have_undo) remove(undo);
        return ESP_ERR_INVALID_CRC;
    }
    if (have_undo) remove(undo);
    if (sram) S->sram_dirty = true;
    return ESP_OK;
}
