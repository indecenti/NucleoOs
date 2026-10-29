// nucleo_pce — the PC Engine: see include/nucleo_pce.h.
//
// HOW IT FITS (the same rules as the other consoles here: contiguous RAM is the budget)
//   CPU          MAME's HuC6280 (BSD-3, vendor/h6280/h6280_core.h, generated), its memory through 1 KB windows:
//                each 1 KB of the CPU's 64 KB space points straight at work RAM or at a cartridge page in the
//                cache, so an instruction fetch is an index and a load; I/O and unmapped pages take the slow path
//   video        the HuC6270 VDC (64 KB of VRAM as two 32 KB blocks: the Solo boot has no 64 KB block) and the
//                HuC6260 VCE (512 colours), drawn a scanline at a time against the VDC's registers as they are when
//                the line starts; the vertical state machine, raster counter, RCR / vblank / DMA / sprite-overflow /
//                collision interrupts follow MAME's huc6270 (BSD-3) at line resolution
//   sound        the HuC6280's PSG: six wave channels (32 x 5-bit), noise on 4-5, DDA, the LFO; every register write
//                is logged with its cycle in the frame and replayed while the frame's 32 kHz samples are made, so
//                sampled voices (DDA written from the timer interrupt) play at their own pace
//   cartridge    1 KB pages from the SD card (as nucleo_nes.c), or the whole file when it fits with room to spare;
//                384 / 512 / 768 KB HuCards mirrored as on the console, Street Fighter II's bank register
//   timing       263 lines of 455 CPU cycles (7.16 MHz), 59.83 Hz
//   speed        waiting loops are skipped exactly (idle_after: a loop that comes back to the same state without a
//                write or an I/O read only ends at an interrupt; 40-80% of a frame's CPU cycles in most games); the
//                pixels are drawn on the S3's other core, eight lines a hand-over, from each line's registers captured
//                as it starts (nucleo_cowork; the lines go to the screen from this core, in order); behind the
//                60 Hz budget, frames run unseen (three at most in a row) and the late frame's sound is made long enough
//                that the speaker never runs dry (as nucleo_nes.c). The profile goes to the trace on close.
#include "nucleo_pce.h"
#include "nucleo_audio.h"
// The second core (firmware nucleo_app/nucleo_cowork.h, ABI level 6), declared here: nucleo_emu sits below
// nucleo_app in the component graph.
bool nucleo_cowork_run(void (*fn)(void *arg), void *arg);
void nucleo_cowork_wait(void);
void nucleo_cowork_stop(void);
#include "emu_romstore.h"                            // the cartridge from internal flash (ABI level 7)
#include <stdint.h>
#include "emu_audio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Every write and every I/O read the CPU makes is counted: an instruction loop that does neither and comes back to
// the same state is waiting (see idle_after). The core calls the hook after each instruction.
static void idle_after(void);
#define PCE_AFTER_OP() do { if (m_pc.w.l < m_ppc.w.l) idle_after(); } while (0)   // only a backward jump can close a loop
#ifdef NAP_MODULE
#define PCE_HOT    __attribute__((section(".text.nap_hot"), noinline))   // the run loop, locked in the cache (hot_lock)
#define PCE_HOT_FN __attribute__((section(".text.nap_hot")))             // ...and what it calls every few instructions
#else
#define PCE_HOT_FN
#endif
#include "../vendor/h6280/h6280_core.h"

static const char *TAG = "pce";

// The cartridge cache's page, which is also the CPU's window: 256 bytes. The big HuCards read a little of many places
// a frame (Street Fighter II, Bonk III: the code and the graphics of a whole stage); with 1 KB pages the same 52 KB of
// RAM held a quarter of what they use, and the card was read 34 times a frame. With 256-byte pages: under 2. The small
// games pay a few more reads while the cache fills, then nothing (40 cartridges measured, docs/native-emulation.md).
#ifndef PG_BITS
#define PG_BITS   8
#endif
#define PG_SIZE   (1u << PG_BITS)
#define PG_CHUNK  1024u                                // pages come from the heap in 1 KB blocks (one header per 4)
#define PPC       (PG_CHUNK / PG_SIZE)
#define PG_SLOTS  (128 * 1024 / PG_SIZE)
#define PG_MIN    (16 * 1024 / PG_SIZE)
#ifndef PG_AHEAD
#define PG_AHEAD  3                                    // pages read after a missed one (see rom_page)
#endif
#define NWIN      (0x10000 >> PG_BITS)                 // windows over the CPU's 64 KB
#define WPB       (0x2000 >> PG_BITS)                  // ...and in one 8 KB bank

// What every memory access reads, in ONE struct (one base address in a register, as the CPU's state H6280): the
// windows, and the write / I/O-read counters of the idle-loop detector.
static struct {
    const uint8_t *rd[NWIN];                         // CPU windows, a page each (NULL: the slow path)
    uint8_t       *wr[NWIN];
    uint32_t       writes, io_reads;
} MEM;
#define g_writes   MEM.writes
#define g_io_reads MEM.io_reads
#define PG_HINT   512
#define PG_EMPTY  0xFFFFu
#define KEEP      (8 * 1024)

#define LINES     263
#define LINE_CYC  455                                  // 1365 master clocks / 3
#define HSYNC_CYC 376                                  // the falling edge of HSYNC: the raster-compare interrupt
#define FIRST_Y   16                                   // the line the usual 240-line display starts on
#define APU_RATE  32000
#define APU_SPF   535                                  // 32000 / 59.826
#define PSG_HZ    3579545
#define FRAME_CYC (LINES * LINE_CYC)

// VDC status bits and registers (MAME huc6270)
#define ST_CR 0x01
#define ST_OR 0x02
#define ST_RR 0x04
#define ST_DS 0x08
#define ST_DV 0x10
#define ST_VD 0x20
enum { R_MAWR = 0, R_MARR = 1, R_VXR = 2, R_CR = 5, R_RCR = 6, R_BXR = 7, R_BYR = 8, R_MWR = 9, R_HSR = 10, R_HDR = 11,
       R_VPR = 12, R_VDW = 13, R_VCR = 14, R_DCR = 15, R_SOUR = 16, R_DESR = 17, R_LENR = 18, R_DVSSR = 19 };
enum { V_VSW, V_VDS, V_VDW, V_VCR };
static const uint8_t VRAM_INC[4] = { 1, 32, 64, 128 };

typedef struct {
    uint16_t frequency;
    uint8_t  control, balance, waveform[32], index, dda, noise_control;
    int32_t  noise_counter;
    uint32_t noise_seed;
    uint32_t acc;                                    // PSG clocks owed to the waveform step, 16.16
} psg_ch_t;

// Everything a save state holds besides the CPU and VRAM: one block, written as is.
typedef struct {
    uint8_t  wram[8192];
    uint16_t sat[256];
    // VDC
    uint16_t mawr, marr, vrr, vwr, cr, rcr, bxr, byr, mwr, hsr, hdr, vpr, vdw, vcr, dcr, sour, desr, lenr, dvssr;
    uint8_t  reg_index, status;
    bool     dvssr_written, dma_enabled, vd_triggered;
    int      satb_countdown, raster_count, vert_state, vert_to_go;
    uint16_t byr_latched;
    // VCE
    uint16_t pal[512];
    uint16_t vce_addr;
    uint8_t  vce_ctrl;
    // PSG
    psg_ch_t ch[6];
    uint8_t  psg_select, psg_balance, lfo_freq, lfo_ctrl, psg_enabled;
    // pad, mapper
    uint8_t  pad_sel, pad_clr;
    uint32_t sf2_off;
} pce_core_t;


// A display line as described when it starts (see vdc_line), and the batches the other core draws them in.
typedef struct pce_line_s {
    int16_t  y;                                      // the output row
    uint16_t width;                                  // 0: a blank line (outside the display window)
    uint16_t cr, mwr, bxr, byr, raster;
} pce_line_t;
#define PCE_BATCH 4                                  // 2 x 2 KB of pixels: RAM is the cartridge cache
typedef struct {
    int        n;
    pce_line_t line[PCE_BATCH];
    uint16_t   px[PCE_BATCH][NUCLEO_PCE_W];
} pce_batch_t;
// A line drawer's working rows: one for each core.
typedef struct {
    uint8_t  bg[528];
    uint16_t spr[512];
    uint16_t line[512];
} pce_scratch_t;

#define VRAM_BLK  8                                  // 4 K words each
typedef struct {
    pce_core_t m;
    uint16_t  *vram[VRAM_BLK];                       // 64 KB in 8 KB blocks (any fragmented heap has those)
    FILE      *fp;
    uint32_t   rom_base, rom_bytes, data_bytes;      // the HuCard data starts at rom_base (a 512-byte copier header)
    uint8_t   *rom_all;
    bool       rom_mapped;                           // rom_all is the cartridge mapped from flash (nucleo_romstore)
    uint8_t   *chunk[PG_SLOTS / PPC];                // the pages' 1 KB blocks
    uint16_t   pg_tag[PG_SLOTS], pg_age[PG_SLOTS], pg_clock;   // 16 bits: the tables stay small next to the pages
    int        pg_n;
    int16_t    hint[PG_HINT];
    uint32_t   cur_tag;
    uint8_t   *cur_buf;
    int16_t    cur_pg;
    uint32_t   misses;
    uint32_t   bank_off[128];                        // HuCard bank -> offset in the data (the console's mirroring)
    bool       sf2;
    bool       tg16;                                 // a TurboGrafx-16 card (its pad port reads "not Japanese")
    int16_t    win_pg[NWIN];                           // the cache page each window shows (-1: RAM, open bus, I/O)
    int16_t    last_pg;                              // the page rom_page returned last (-1: not a cache page)
    uint16_t   rgb[512];                             // the palette in the panel's RGB565 order
    uint16_t   out[NUCLEO_PCE_W];                    // a line drawn on this core
    pce_batch_t *batch[2];                           // lines for the other core: one filling, one being drawn
    int        bi, pending;                          // the batch being filled; the one handed over (-1: none)
    bool       busy;                                 // lines described and not yet on the screen
    uint16_t   *spr_here;                            // this core's sprite row (the interrupts it raises)
    pce_scratch_t *kw;                               // the other core's drawer
    // (each its own allocation, a few KB: after the VRAM the heap has no block for one big session)
    nucleo_pce_line_fn on_line;
    void      *user;
    uint8_t    buttons;
    uint32_t   leg_base, leg_len;                    // where the CPU is in the frame (the PSG's writes)
    int        psg_n, psg_i;                         // this frame's samples, and how many are made
    int16_t    pcm[2 * APU_SPF];                     // a late frame makes up to twice a frame's samples
    uint16_t   vol[32];
    emu_afilt_t afilt;
    bool       sound, frameskip, skip;
    // Pacing (as nucleo_nes.c): how far behind the console is, from work only; frames run unseen while it is.
    int64_t    last_t0, last_block;
    int32_t    debt;
    uint8_t    skipped;
    // Where the time goes, in CPU cycles (the S3's counter), written to the trace when the game closes.
    uint64_t   prof_cpu, prof_vdc, prof_line, prof_psg, prof_idle, prof_wait;
    uint32_t   prof_ahead, prof_misses, prof_frames, prof_drawn, prof_lines_here, prof_lines_other;
    char       trace_path[160];
    char       save_base[300];
    char       title[48];
    size_t     heap_bytes;
    uint32_t   frames, us_cpu, us_audio, us_miss, clock;
} pce_t;

static pce_t *PCE;
static inline void lines_touch(void);           // described lines drawn first: VRAM, sprite table or palette changes

static inline uint32_t pce_ccount(void)
{
#if defined(__XTENSA__)
    uint32_t c;
    __asm__ volatile("rsr %0, ccount" : "=r"(c));
    return c;
#elif defined(__x86_64__) || defined(__i386__)
    return (uint32_t)__builtin_ia32_rdtsc();
#else
    return 0;
#endif
}

// ── the cartridge, paged (as nucleo_nes.c) ────────────────────────────────────────────────────────────────
static const uint8_t PG_OPEN[PG_SIZE] = { [0 ... PG_SIZE - 1] = 0xFF };
static void windows_drop(void)
{
    memset(MEM.rd, 0, sizeof MEM.rd); memset(MEM.wr, 0, sizeof MEM.wr);
    memset(PCE->win_pg, 0xff, sizeof PCE->win_pg);
}

static inline uint8_t *pg_buf(const pce_t *s, int i) { return s->chunk[i / PPC] + (uint32_t)(i % PPC) * PG_SIZE; }

// The page holding cartridge page `tag`, or -1 (the hint table first, then a scan).
static int find_page(pce_t *s, uint32_t tag)
{
    const int g = s->hint[tag & (PG_HINT - 1)];
    if (g >= 0 && g < s->pg_n && s->pg_tag[g] == tag) return g;
    for (int i = 0; i < s->pg_n; i++) if (s->pg_tag[i] == tag) { s->hint[tag & (PG_HINT - 1)] = (int16_t)i; return i; }
    return -1;
}
// The least recently used page that no window shows and is not already taken (`taken`), else the oldest of all.
static int pick_victim(pce_t *s, uint8_t *taken)
{
    int v = -1;
    for (int i = 0; i < s->pg_n; i++) if (!taken[i] && (v < 0 || s->pg_age[i] < s->pg_age[v])) v = i;
    if (v < 0) { v = 0; for (int i = 1; i < s->pg_n; i++) if (s->pg_age[i] < s->pg_age[v]) v = i; }
    taken[v] = 1;
    for (int k = 0; k < NWIN; k++)                     // only the windows on the page being replaced (none, usually)
        if (s->win_pg[k] == v) { MEM.rd[k] = NULL; MEM.wr[k] = NULL; s->win_pg[k] = -1; }
    return v;
}

static const uint8_t *rom_page(uint32_t data_off)
{
    pce_t *s = PCE;
    s->last_pg = -1;
    if (data_off >= s->data_bytes) return PG_OPEN;
    const uint32_t tag = data_off >> PG_BITS;
    if (s->rom_all) return s->rom_all + s->rom_base + (tag << PG_BITS);
    if (tag == s->cur_tag) { s->last_pg = s->cur_pg; return s->cur_buf; }
    int g = find_page(s, tag);
    if (g < 0) {
        // The victim: the least recently used page that no window shows. A page the CPU reads through a window is
        // never "used" again as far as the ages know (a window is a pointer: no call, no clock), so plain LRU threw out
        // exactly the hottest pages, then every window with them, and read them back at once.
        uint8_t taken[PG_SLOTS];
        memset(taken, 0, sizeof taken);
        for (int k = 0; k < NWIN; k++) if (s->win_pg[k] >= 0) taken[s->win_pg[k]] = 1;
        g = pick_victim(s, taken);
        const int64_t m0 = esp_timer_get_time();
        size_t got = 0;
        if (fseek(s->fp, (long)(s->rom_base + (tag << PG_BITS)), SEEK_SET) == 0) got = fread(pg_buf(s, g), 1, PG_SIZE, s->fp);
        if (got < PG_SIZE) memset(pg_buf(s, g) + got, 0xFF, PG_SIZE - got);
        s->pg_tag[g] = (uint16_t)tag;
        s->hint[tag & (PG_HINT - 1)] = (int16_t)g;
        // Read-ahead: what a seek costs on the card is the seek, not the bytes, and the big HuCards read on (the
        // next page is wanted soon after in 60% of Street Fighter II's misses). The pages after this one, as long
        // as they are not cached already, come in the same read, as the oldest pages: unused, they are the first
        // to go; used, they are aged like any other.
        for (uint32_t t2 = tag + 1; t2 <= tag + PG_AHEAD && got == PG_SIZE; t2++) {
            if ((t2 << PG_BITS) >= s->data_bytes || find_page(s, t2) >= 0) break;
            const int v2 = pick_victim(s, taken);
            if (v2 == g) break;
            got = fread(pg_buf(s, v2), 1, PG_SIZE, s->fp);
            if (got < PG_SIZE) memset(pg_buf(s, v2) + got, 0xFF, PG_SIZE - got);
            s->pg_tag[v2] = (uint16_t)t2;
            s->pg_age[v2] = 1;
            s->hint[t2 & (PG_HINT - 1)] = (int16_t)v2;
            s->prof_ahead++;
        }
        s->us_miss += (uint32_t)(esp_timer_get_time() - m0);
        s->misses++;
        s->prof_misses++;
    }
    if (++s->pg_clock == 0xFFFF) {                    // the 16-bit clock wraps: halve every age, the order stays
        for (int i = 0; i < s->pg_n; i++) s->pg_age[i] >>= 1;
        s->pg_clock = 0x8000;
    }
    s->pg_age[g] = s->pg_clock;
    s->cur_tag = tag; s->cur_buf = pg_buf(s, g); s->cur_pg = (int16_t)g;
    s->last_pg = (int16_t)g;
    return s->cur_buf;
}

// The console's HuCard mirroring (banks 0x00-0x7F of the 1 MB card space).
static void map_banks(void)
{
    pce_t *s = PCE;
    const uint32_t banks = (s->data_bytes + 0x1FFF) / 0x2000;
    for (int x = 0; x < 128; x++) {
        uint32_t b;
        if (banks == 0x30)      b = x < 64 ? (uint32_t)(x & 0x1F) : (uint32_t)((x & 0x0F) + 0x20);
        else if (banks == 0x40) b = x < 64 ? (uint32_t)(x & 0x3F) : (uint32_t)((x & 0x1F) + 0x20);
        else if (banks == 0x60) b = x < 64 ? (uint32_t)(x & 0x3F) : (uint32_t)((x & 0x1F) + 0x40);
        else                    b = banks ? (uint32_t)x % banks : 0;
        s->bank_off[x] = b * 0x2000;
    }
}

// ── the CPU's memory ───────────────────────────────────────────────────────────────────────────────────────
static uint8_t io_read(uint16_t off);
static void io_write(uint16_t off, uint8_t v);

// Where 1 KB window `slot` of the CPU's space lands now (NULL for the I/O page).
static PCE_HOT_FN const uint8_t *window(int slot)
{
    pce_t *s = PCE;
    const uint8_t bank = m_mmr[slot / WPB];
    const uint32_t k = (uint32_t)(slot % WPB) << PG_BITS;
    const uint8_t *r = NULL;
    uint8_t *w = NULL;
    int16_t pg = -1;
    if (bank < 0x80) {
        uint32_t off = (s->sf2 && bank >= 0x40) ? (uint32_t)bank * 0x2000 + s->m.sf2_off : s->bank_off[bank];
        r = rom_page(off + k);
        pg = s->last_pg;
    } else if (bank >= 0xF8 && bank <= 0xFB) {
        r = w = s->m.wram + k;
    } else if (bank != 0xFF) {
        r = PG_OPEN;
    }
    MEM.rd[slot] = r;                                 // after rom_page: a miss there drops the windows on its victim
    MEM.wr[slot] = w;
    s->win_pg[slot] = pg;
    return r;
}

static void pce_mpr_changed(int page)
{
    g_writes++;
    for (int k = 0; k < WPB; k++) { MEM.rd[page * WPB + k] = NULL; MEM.wr[page * WPB + k] = NULL; PCE->win_pg[page * WPB + k] = -1; }
}

static inline uint8_t mem_read(uint16_t addr)
{
    const uint8_t *w = MEM.rd[addr >> PG_BITS];
    if (!w && !(w = window(addr >> PG_BITS))) return io_read(addr & 0x1fff);
    return w[addr & (PG_SIZE - 1)];
}
static inline void mem_write(uint16_t addr, uint8_t v)
{
    g_writes++;
    uint8_t *w = MEM.wr[addr >> PG_BITS];
    if (w) { w[addr & (PG_SIZE - 1)] = v; return; }
    const uint8_t bank = m_mmr[addr >> 13];
    if (bank >= 0xF8 && bank <= 0xFB) { window(addr >> PG_BITS); MEM.wr[addr >> PG_BITS][addr & (PG_SIZE - 1)] = v; return; }
    if (bank == 0xFF) { io_write(addr & 0x1fff, v); return; }
    if (bank < 0x80 && PCE->sf2 && (addr & 0x1FF0) == 0x1FF0) {           // Street Fighter II's bank register
        const uint32_t blocks = PCE->data_bytes > 0x80000 ? (PCE->data_bytes - 0x80000) / 0x80000 : 1;
        PCE->m.sf2_off = ((addr & 0x0F) % (blocks ? blocks : 1)) * 0x80000;
        windows_drop();
    }
}

// The core's memory interface (vendor/h6280/h6280_core.h). The VDC/VCE penalty cycle is the core's own.
static inline uint32_t translated(uint16_t addr);
static inline void check_vdc_vce_penalty(uint16_t addr);
// A window is never the I/O page (bank 0xFF has none), and the VDC / VCE penalty cycle is only for I/O: an access
// through a window skips the check, every other one takes it as before.
static uint8_t program_read8(uint16_t addr)
{
    const uint8_t *w = MEM.rd[addr >> PG_BITS];
    if (w) return w[addr & (PG_SIZE - 1)];
    check_vdc_vce_penalty(addr);
    return mem_read(addr);
}
static PCE_HOT_FN void program_write8(uint16_t addr, uint8_t v)
{
    uint8_t *w = MEM.wr[addr >> PG_BITS];
    if (w) { g_writes++; w[addr & (PG_SIZE - 1)] = v; return; }
    check_vdc_vce_penalty(addr);
    mem_write(addr, v);
}
static uint8_t  program_read8z(uint16_t addr)           { return mem_read((uint16_t)(0x2000 | (addr & 0x1fff))); }
static void     program_write8z(uint16_t addr, uint8_t v) { mem_write((uint16_t)(0x2000 | (addr & 0x1fff)), v); }
static uint16_t program_read16(uint16_t addr)           { return (uint16_t)(mem_read(addr) | mem_read((uint16_t)(addr + 1)) << 8); }
static uint16_t program_read16z(uint16_t addr)
{
    const uint16_t hi = (addr & 0xff) == 0xff ? (uint16_t)(addr - 0xff) : (uint16_t)(addr + 1);
    return (uint16_t)(program_read8z(addr) | program_read8z(hi) << 8);
}
static PCE_HOT_FN void push(uint8_t v)                  { program_write8z((uint16_t)m_sp.d, v); m_sp.b.l--; }
static uint8_t  pull_(void)                             { m_sp.b.l++; return program_read8z((uint16_t)m_sp.d); }
static uint8_t  read_opcode(void)                       { return mem_read(m_pc.w.l); }
static uint8_t  read_opcode_arg(void)                   { return mem_read(m_pc.w.l); }

// ── the PSG: registers now, sound later (from the log) ────────────────────────────────────────────────────
static void psg_reg(uint8_t reg, uint8_t data)
{
    pce_core_t *m = &PCE->m;
    psg_ch_t *c = &m->ch[m->psg_select < 6 ? m->psg_select : 5];
    switch (reg & 0x0f) {
        case 0x00: m->psg_select = data & 7; break;
        case 0x01: m->psg_balance = data; break;
        case 0x02: c->frequency = (uint16_t)((c->frequency & 0x0f00) | data); break;
        case 0x03: c->frequency = (uint16_t)((c->frequency & 0x00ff) | ((data << 8) & 0x0f00)); break;
        case 0x04:
            if ((c->control & 0x40) && !(data & 0x40)) c->index = 0;
            c->control = data;
            if (m->psg_select < 6) { m->psg_enabled &= (uint8_t)~(1 << m->psg_select); m->psg_enabled |= (uint8_t)((data >> 7) << m->psg_select); }
            break;
        case 0x05: c->balance = data; break;
        case 0x06:
            if (c->control & 0x40) c->dda = data & 0x1f;
            else { c->waveform[c->index & 0x1f] = data & 0x1f; if (!(c->control & 0x80)) c->index = (c->index + 1) & 0x1f; }
            break;
        case 0x07: c->noise_control = data; break;
        case 0x08: m->lfo_freq = data; break;
        case 0x09: m->lfo_ctrl = data; break;
    }
}
// One sample, the channels stepped in PSG clocks.
static void psg_sample(pce_t *s, int i)
{
    pce_core_t *m = &s->m;
    static const uint8_t SCALE[16] = { 0x00, 0x03, 0x05, 0x07, 0x09, 0x0b, 0x0d, 0x0f, 0x10, 0x13, 0x15, 0x17, 0x19, 0x1b, 0x1d, 0x1f };
    const uint32_t cps = (uint32_t)(((uint64_t)PSG_HZ << 16) / APU_RATE);   // PSG clocks per sample, 16.16
    {
        const uint8_t lmal = SCALE[(m->psg_balance >> 4) & 15], rmal = SCALE[m->psg_balance & 15];
        int32_t out = 0;
        for (int chn = 0; chn < 6; chn++) {
            if (!((m->psg_enabled >> chn) & 1)) continue;
            psg_ch_t *c = &m->ch[chn];
            const uint8_t lal = SCALE[(c->balance >> 4) & 15], ral = SCALE[c->balance & 15], al = c->control & 0x1f;
            int vll = (0x1f - lmal) + (0x1f - al) + (0x1f - lal); if (vll > 0x1f) vll = 0x1f;
            int vlr = (0x1f - rmal) + (0x1f - al) + (0x1f - ral); if (vlr > 0x1f) vlr = 0x1f;
            const int vol = (s->vol[vll] + s->vol[vlr]) >> 1;
            int data;
            if (chn >= 4 && (c->noise_control & 0x80)) {
                const int32_t step = (int32_t)(((c->noise_control & 0x1f) ^ 0x1f) << 6);
                data = (c->noise_seed & 1) ? 0x1f : 0;
                c->noise_counter -= (int32_t)(cps >> 16);
                while (c->noise_counter <= 0) {
                    c->noise_counter += step ? step : 64;
                    const uint32_t seed = c->noise_seed;
                    c->noise_seed = (seed >> 1) | ((((seed) ^ (seed >> 1) ^ (seed >> 11) ^ (seed >> 12) ^ (seed >> 17)) & 1) << 17);
                }
            } else if (c->control & 0x40) {
                data = c->dda;
            } else {
                uint32_t step = c->frequency ? c->frequency : 0x1000;
                if (chn == 0 && (m->lfo_ctrl & 3)) {           // channel 1 modulates channel 0's frequency
                    const psg_ch_t *l = &m->ch[1];
                    step = (uint32_t)((int32_t)step + ((l->waveform[l->index] - 16) << (((m->lfo_ctrl & 3) - 1) << 1)));
                    if ((int32_t)step < 1) step = 1;
                }
                data = c->waveform[c->index];
                c->acc += cps;
                const uint32_t adv = c->acc / (step << 16);
                if (adv) { c->index = (uint8_t)((c->index + adv) & 0x1f); c->acc -= adv * (step << 16); }
            }
            out += vol * (data - 16);
        }
        if (out > 32767) out = 32767; else if (out < -32768) out = -32768;
        s->pcm[i] = (int16_t)out;
    }
}
// The frame's samples that come before CPU cycle `cyc` of it (sample i sits at cycle i * FRAME_CYC / n). Made as the
// CPU gets there, so a register write lands between exactly the samples it fell between — the same sound as logging
// every write with its cycle and replaying the log after the frame, without the 8 KB the log took (and with no
// limit on writes a frame: sampled voices write the PSG from a timer interrupt thousands of times a second).
static void psg_upto(pce_t *s, uint32_t cyc)
{
    while (s->psg_i < s->psg_n && (uint32_t)s->psg_i * FRAME_CYC / (uint32_t)s->psg_n < cyc) {   // fits 32 bits
        psg_sample(s, s->psg_i);
        s->psg_i++;
    }
}
static void pce_psg_write(int reg, uint8_t v)
{
    pce_t *s = PCE;
    psg_upto(s, s->leg_base + (uint32_t)(s->leg_len - (m_icount > 0 ? m_icount : 0)));
    psg_reg((uint8_t)reg, v);
}

// ── the VCE ──────────────────────────────────────────────────────────────────────────────────────────────
static uint16_t rgb_of(uint16_t c)                     // 9-bit GGGRRRBBB -> RGB565, panel byte order
{
    const unsigned g = (c >> 6) & 7, r = (c >> 3) & 7, b = c & 7;
    const uint16_t v = (uint16_t)(((r * 31 / 7) << 11) | ((g * 63 / 7) << 5) | (b * 31 / 7));
    return (uint16_t)((v >> 8) | (v << 8));
}
static void vce_write(uint16_t off, uint8_t v)
{
    pce_core_t *m = &PCE->m;
    if ((off & 6) == 4) lines_touch();                 // a colour changes: the queued lines are drawn with the old one
    switch (off & 7) {
        case 0: m->vce_ctrl = v; break;
        case 2: m->vce_addr = (uint16_t)((m->vce_addr & 0x100) | v); break;
        case 3: m->vce_addr = (uint16_t)((m->vce_addr & 0xff) | ((v & 1) << 8)); break;
        case 4: m->pal[m->vce_addr] = (uint16_t)((m->pal[m->vce_addr] & 0x100) | v);
                PCE->rgb[m->vce_addr] = rgb_of(m->pal[m->vce_addr]); break;
        case 5: m->pal[m->vce_addr] = (uint16_t)((m->pal[m->vce_addr] & 0xff) | ((v & 1) << 8));
                PCE->rgb[m->vce_addr] = rgb_of(m->pal[m->vce_addr]);
                m->vce_addr = (m->vce_addr + 1) & 0x1ff; break;
    }
}
static uint8_t vce_read(uint16_t off)
{
    pce_core_t *m = &PCE->m;
    switch (off & 7) {
        case 4: return (uint8_t)(m->pal[m->vce_addr] & 0xff);
        case 5: { uint8_t d = (uint8_t)(0xfe | (m->pal[m->vce_addr] >> 8)); m->vce_addr = (m->vce_addr + 1) & 0x1ff; return d; }
    }
    return 0xff;
}

// ── the VDC ──────────────────────────────────────────────────────────────────────────────────────────────
static inline uint16_t vram_rd(uint32_t a) { a &= 0xffff; return a <= 0x7fff ? PCE->vram[a >> 12][a & 0xfff] : 0; }
static inline void vram_wr(uint32_t a, uint16_t v) { lines_touch(); a &= 0xffff; if (a <= 0x7fff) PCE->vram[a >> 12][a & 0xfff] = v; }
static void vdc_irq(uint8_t bit) { PCE->m.status |= bit; set_irq_line(0, ASSERT_LINE); }

static void vdc_regs_w(int r, uint16_t data, uint16_t mask)
{
    pce_core_t *m = &PCE->m;
#define COMBINE(x) (x) = (uint16_t)(((x) & ~mask) | (data & mask))
    switch (r) {
        case R_MAWR: COMBINE(m->mawr); break;
        case R_MARR: COMBINE(m->marr); m->vrr = vram_rd(m->marr); break;
        case R_VXR:
            COMBINE(m->vwr);
            if (mask & 0xff00) { vram_wr(m->mawr, m->vwr); m->mawr = (uint16_t)(m->mawr + VRAM_INC[(m->cr >> 11) & 3]); }
            break;
        case R_CR:   COMBINE(m->cr); break;
        case R_RCR:  COMBINE(m->rcr); m->rcr &= 0x3ff; break;
        case R_BXR:  COMBINE(m->bxr); m->bxr &= 0x3ff; break;
        case R_BYR:  COMBINE(m->byr); m->byr &= 0x1ff; m->byr_latched = m->byr; break;
        case R_MWR:  COMBINE(m->mwr); break;
        case R_HSR:  COMBINE(m->hsr); break;
        case R_HDR:  COMBINE(m->hdr); break;
        case R_VPR:  COMBINE(m->vpr); break;
        case R_VDW:  COMBINE(m->vdw); break;
        case R_VCR:  COMBINE(m->vcr); break;
        case R_DCR:  COMBINE(m->dcr); break;
        case R_SOUR: COMBINE(m->sour); break;
        case R_DESR: COMBINE(m->desr); break;
        case R_LENR: COMBINE(m->lenr); if (mask & 0xff00) m->dma_enabled = true; break;
        case R_DVSSR: COMBINE(m->dvssr); m->dvssr_written = true; break;
    }
#undef COMBINE
}
static void vdc_write(uint16_t off, uint8_t v)
{
    switch (off & 3) {
        case 0: PCE->m.reg_index = v & 0x1f; break;
        case 2: vdc_regs_w(PCE->m.reg_index, v, 0x00ff); break;
        case 3: vdc_regs_w(PCE->m.reg_index, (uint16_t)(v << 8), 0xff00); break;
    }
}
static uint8_t vdc_read(uint16_t off)
{
    pce_core_t *m = &PCE->m;
    switch (off & 3) {
        case 0: {
            const uint8_t d = m->status;
            m->status &= (uint8_t)~(ST_VD | ST_DV | ST_RR | ST_CR | ST_OR | ST_DS);
            set_irq_line(0, CLEAR_LINE);
            return d;
        }
        case 2: return (uint8_t)(m->vrr & 0xff);
        case 3: {
            const uint8_t d = (uint8_t)(m->vrr >> 8);
            if (m->reg_index == R_VXR) { m->marr = (uint16_t)(m->marr + VRAM_INC[(m->cr >> 11) & 3]); m->vrr = vram_rd(m->marr); }
            return d;
        }
    }
    return 0;
}
static void pce_vdc_st(int port, uint8_t v) { g_writes++; vdc_write((uint16_t)port, v); }

static void vdc_vblank(void)
{
    pce_core_t *m = &PCE->m;
    if (m->vd_triggered) return;
    if (m->cr & 0x08) vdc_irq(ST_VD);
    if (m->dvssr_written || (m->dcr & 0x10)) {        // VRAM -> SAT DMA
        lines_touch();
        for (int i = 0; i < 256; i += 4) {
            m->sat[i + 0] = vram_rd(m->dvssr + i + 0) & 0x03ff;
            m->sat[i + 1] = vram_rd(m->dvssr + i + 1) & 0x03ff;
            m->sat[i + 2] = vram_rd(m->dvssr + i + 2) & 0x07ff;
            m->sat[i + 3] = vram_rd(m->dvssr + i + 3);
        }
        m->dvssr_written = false;
        if (m->dcr & 0x01) m->satb_countdown = 4;
    }
    m->vd_triggered = true;
}
static void vdc_next_vert(void)
{
    pce_core_t *m = &PCE->m;
    switch (m->vert_state) {
        case V_VSW: m->vert_state = V_VDS; m->vert_to_go = ((m->vpr >> 8) & 0xff) + 2; break;
        case V_VDS: m->vert_state = V_VDW; m->vert_to_go = (m->vdw & 0x1ff) + 1; m->byr_latched = m->byr; m->vd_triggered = false; break;
        case V_VDW: m->vert_state = V_VCR; m->vert_to_go = m->vcr & 0xff; vdc_vblank(); break;
        default:    m->vert_state = V_VSW; m->vert_to_go = (m->vpr & 0x1f) + 1; break;
    }
}
// The rising edge of HSYNC: the line starts (MAME huc6270 hsync_changed(1), at line resolution).
static void vdc_hsync(void)
{
    pce_core_t *m = &PCE->m;
    if (m->satb_countdown && --m->satb_countdown == 0) vdc_irq(ST_DS);
    m->byr_latched++;
    m->raster_count++;
    if (m->vert_to_go == 2 && m->vert_state == V_VDS) m->raster_count = 0x40;
    m->vert_to_go--;
    int guard = 8;
    while (m->vert_to_go <= 0 && guard--) vdc_next_vert();
    if (m->dma_enabled) {                             // VRAM -> VRAM DMA
        const int di = (m->dcr & 8) ? -1 : 1, si = (m->dcr & 4) ? -1 : 1;
        do {
            vram_wr(m->desr, vram_rd(m->sour));
            m->sour = (uint16_t)(m->sour + si);
            m->desr = (uint16_t)(m->desr + di);
            m->lenr--;
        } while (m->lenr != 0xffff);
        if (m->dcr & 0x02) vdc_irq(ST_DV);
        m->dma_enabled = false;
    }
}
static void vdc_vsync(int state)                       // the VCE's VSYNC: low resets the vertical counter, high ends it
{
    pce_core_t *m = &PCE->m;
    if (!state) {
        m->vert_state = V_VCR;
        m->vert_to_go = 0;
        int guard = 8;
        while (m->vert_to_go <= 0 && guard--) vdc_next_vert();
    } else {
        vdc_vblank();
    }
}

// ── a line: described on the CPU's core, drawn on the other one ─────────────────────────────────────────────
// What a display line is made of, captured when it starts: the registers it is drawn with. The VRAM, the sprite
// table and the palette it also reads are the live ones: nothing changes them while a described line is still to be
// drawn (vram_wr, the DMAs and the VCE settle the lines first).

// The sprites of a line into `row` (index 0 = screen x 0): MAME's select_sprites / add_sprite. Returns the 16-cell
// overflow; a collision with sprite 0 is marked 0x8000 in the row.
static bool line_sprites(const pce_line_t *L, uint16_t *row)
{
    const uint16_t *sat = PCE->m.sat;
    static const int CGY[4] = { 16, 32, 64, 64 };
    const int width = L->width;
    memset(row, 0, (size_t)width * sizeof *row);
    int cells = 0;
    for (int i = 0; i < 256 && cells < 16; i += 4) {
        const int cgy = (sat[i + 3] >> 12) & 3, height = CGY[cgy];
        int line = L->raster - 1 - sat[i];
        if (line < 0 || line >= height) continue;
        const int xpos = sat[i + 1];
        int pattern = sat[i + 2] >> 1;
        const bool lsb = sat[i + 2] & 1, prio = (sat[i + 3] >> 7) & 1, cgx = (sat[i + 3] >> 8) & 1;
        const int pal = sat[i + 3] & 0x0f;
        if (cgy & 1) pattern &= ~0x0002;
        if (cgy & 2) pattern &= ~0x0006;
        if (sat[i + 3] & 0x8000) line = (height - 1) - line;
        const bool fx = (sat[i + 3] >> 11) & 1;
        const int n = cgx ? 2 : 1;
        if (cgx) pattern &= ~0x0001;
        for (int part = 0; part < n && cells < 16; part++) {
            const int pat = pattern + (cgx ? (fx ? 1 - part : part) : 0) + ((line >> 4) << 1);
            const uint32_t pa = ((uint32_t)pat << 6) + (line & 0x0f);
            uint32_t b0, b1, b2 = 0, b3 = 0;
            if ((L->mwr & 0x0c) == 0x04) {
                b0 = vram_rd(pa + (lsb ? 0x20 : 0x00)); b1 = vram_rd(pa + (lsb ? 0x30 : 0x10)) << 1;
            } else {
                b0 = vram_rd(pa); b1 = vram_rd(pa + 0x10) << 1; b2 = vram_rd(pa + 0x20) << 2; b3 = vram_rd(pa + 0x30) << 3;
            }
            const int x0 = xpos + part * 16 - 32;          // the sprite row starts 32 pixels left of the screen
            for (int j = 15; j >= 0; j--) {
                const uint8_t d = (uint8_t)((b3 & 8) | (b2 & 4) | (b1 & 2) | (b0 & 1));
                b0 >>= 1; b1 >>= 1; b2 >>= 1; b3 >>= 1;
                if (!d) continue;
                const int x = x0 + (j ^ (fx ? 15 : 0));
                if (x < 0 || x >= width) continue;
                if (!row[x]) row[x] = (uint16_t)((prio ? 0x4000 : 0) | ((i >> 2) << 8) | 0x2000 | (pal << 4) | d);
                else if (!(row[x] & 0x1f00)) row[x] |= 0x8000;   // over sprite 0: a collision
            }
            cells++;
        }
    }
    return cells >= 16;
}

// A byte of one bit plane spread to 8 nibbles, pixel 0 (the plane's bit 7) in the low nibble: four lookups and
// three shifts give a tile row's 8 colour indices at once, instead of 32 bit extractions.
static uint32_t PLANE[256];
static void plane_init(void)
{
    if (PLANE[0x80]) return;
    for (int b = 0; b < 256; b++) {
        uint32_t v = 0;
        for (int j = 0; j < 8; j++) if (b & (0x80 >> j)) v |= 1u << (j * 4);
        PLANE[b] = v;
    }
}

// The pixels of a described line, 256 of them into `o` (narrower modes centred on the overscan colour, wider ones
// resampled). Reads VRAM, the sprite table and the palette; writes only `o` and the drawer's scratch. Returns the
// sprite collision (bit 0) and overflow (bit 1), for a line drawn on the CPU's core.
static int line_draw(const pce_line_t *L, uint16_t *o, pce_scratch_t *k)
{
    const uint16_t *rgb = PCE->rgb;
    const uint16_t over = rgb[0x100];
    const int width = L->width;
    if (!width) {
        for (int x = 0; x < NUCLEO_PCE_W; x++) o[x] = over;
        return 0;
    }
    const bool bg_on = (L->cr & 0x80) != 0, spr_on = (L->cr & 0x40) != 0;
    uint16_t *out = width == NUCLEO_PCE_W ? o : k->line;    // the common 256-pixel mode: composed in place
    if (bg_on) {
        static const int WS[4] = { 5, 6, 7, 7 };
        const int ws = WS[(L->mwr >> 4) & 3];
        const uint32_t amask = (1u << ws) - 1;
        const uint16_t v = L->byr & ((L->mwr & 0x40) ? 0x1ff : 0xff);
        const uint32_t base = (uint32_t)(v >> 3) << ws, row = v & 7;
        uint32_t col = (L->bxr >> 3) & amask;
        uint8_t *dst = k->bg;
        for (int t = 0; t <= width / 8; t++, dst += 8, col = (col + 1) & amask) {
            const uint16_t bat = vram_rd(base | col);
            const uint8_t pal = (uint8_t)((bat >> 8) & 0xf0);
            const uint32_t ta = ((uint32_t)(bat & 0x0fff) << 4) + row;
            const uint16_t d1 = vram_rd(ta), d3 = vram_rd(ta + 8);
            if (!(d1 | d3)) { memset(dst, 0, 8); continue; }
            uint32_t px = PLANE[d1 & 0xff] | PLANE[d1 >> 8] << 1 | PLANE[d3 & 0xff] << 2 | PLANE[d3 >> 8] << 3;
            for (int j = 0; j < 8; j++, px >>= 4) { const uint8_t c = (uint8_t)(px & 15); dst[j] = c ? (uint8_t)(c | pal) : 0; }
        }
    }
    int flags = 0;
    if (spr_on && line_sprites(L, k->spr)) flags |= 2;
    const uint8_t *bg = k->bg + (L->bxr & 7);
    const uint16_t *spr = k->spr;
    if (bg_on && !spr_on) {
        for (int x = 0; x < width; x++) out[x] = rgb[bg[x]];
    } else if (bg_on) {
        for (int x = 0; x < width; x++) {
            const uint16_t sp = spr[x];
            uint16_t data = bg[x];
            if (sp) {
                const uint8_t sd = (uint8_t)sp;
                if (sd && ((sp & 0x4000) || !(data & 0x0f))) data = (uint16_t)(0x100 | sd);
                if (sp & 0x8000) flags |= 1;
            }
            out[x] = rgb[data];
        }
    } else if (spr_on) {
        for (int x = 0; x < width; x++) {
            const uint16_t sp = spr[x];
            if (sp & 0x8000) flags |= 1;
            out[x] = rgb[0x100 | (sp & 0xff)];
        }
    } else {
        for (int x = 0; x < width; x++) out[x] = over;   // nothing drawn: the sprite palette's colour 0
    }
    if (width < NUCLEO_PCE_W) {
        const int pad = (NUCLEO_PCE_W - width) / 2;
        for (int x = 0; x < pad; x++) o[x] = over;
        memcpy(o + pad, out, (size_t)width * sizeof *o);
        for (int x = pad + width; x < NUCLEO_PCE_W; x++) o[x] = over;
    } else if (width > NUCLEO_PCE_W) {
        for (int x = 0; x < NUCLEO_PCE_W; x++) o[x] = out[x * width / NUCLEO_PCE_W];
    }
    return flags;
}

// ── the other core ───────────────────────────────────────────────────────────────────────────────────────────
// Four lines a hand-over, two batches: the other core draws one while this core runs the CPU and fills the other.
// The other core only computes (no bus, no driver, no file: the panel shares the SD card's bus); the lines go to the
// screen from this core, in order.
static void line_job(void *arg)
{
    pce_t *s = PCE;
    pce_batch_t *b = s->batch[(int)(intptr_t)arg];
    for (int i = 0; i < b->n; i++) line_draw(&b->line[i], b->px[i], s->kw);
}
static void lines_emit(pce_t *s, int bi)
{
    pce_batch_t *b = s->batch[bi];
    const uint32_t c0 = pce_ccount();
    for (int i = 0; i < b->n; i++) if (s->on_line) s->on_line(b->px[i], b->line[i].y, s->user);
    s->prof_line += pce_ccount() - c0;
    b->n = 0;
}
static void lines_flush(pce_t *s)
{
    const int bi = s->bi;
    if (!s->batch[bi]->n) return;
    const uint32_t c0 = pce_ccount();
    const bool other = nucleo_cowork_run(line_job, (void *)(intptr_t)bi);   // waits for the previous batch first
    s->prof_wait += pce_ccount() - c0;
    if (!other) {                                      // no second core: drawn and sent here
        const uint32_t c1 = pce_ccount();
        line_job((void *)(intptr_t)bi);
        s->prof_vdc += pce_ccount() - c1;
        lines_emit(s, bi);
        return;
    }
    if (s->pending >= 0) lines_emit(s, s->pending);
    s->pending = bi;
    s->bi = bi ^ 1;
}
// Every described line drawn and on the screen: before VRAM, the sprite table or the palette change, and before the
// frame goes back to the front-end.
static void lines_settle(pce_t *s)
{
    if (!s->batch[s->bi]->n && s->pending < 0) return;
    lines_flush(s);
    const uint32_t c0 = pce_ccount();
    nucleo_cowork_wait();
    s->prof_wait += pce_ccount() - c0;
    if (s->pending >= 0) { lines_emit(s, s->pending); s->pending = -1; }
}
static inline void lines_touch(void) { pce_t *s = PCE; if (s->busy) { lines_settle(s); s->busy = false; } }

// A display line at its start; y is the output row (or -1: not shown).
static void vdc_line(int y)
{
    pce_t *s = PCE;
    pce_core_t *m = &s->m;
    int width = ((m->hdr & 0x7f) + 1) * 8;
    if (width > 512) width = 512;
    const pce_line_t L = { (int16_t)y, (uint16_t)width, m->cr, m->mwr, m->bxr, m->byr_latched, (uint16_t)m->raster_count };
    const bool spr_irq = (m->cr & 0x40) && (m->cr & 0x03);
    if (y < 0 || s->skip) {
        // Not shown: only what the CPU can see. The background never raises anything; the sprites do (sprite 0's
        // collision, the 16-cell overflow) and only when their interrupt is on.
        if (spr_irq) {
            const bool ov = line_sprites(&L, s->spr_here);
            if (ov && (m->cr & 0x02)) vdc_irq(ST_OR);
            if (m->cr & 0x01) for (int x = 0; x < width; x++) if (s->spr_here[x] & 0x8000) { vdc_irq(ST_CR); break; }
        }
        return;
    }
    if (spr_irq) {
        // The CPU reads this line's sprite interrupts as it starts: the sprites are evaluated here, now (the table
        // and VRAM they read only change after the queued lines are drawn); the pixels still go to the other core.
        s->prof_lines_here++;
        const uint32_t c0 = pce_ccount();
        const bool ov = line_sprites(&L, s->spr_here);
        if (ov && (m->cr & 0x02)) vdc_irq(ST_OR);
        if (m->cr & 0x01) for (int x = 0; x < width; x++) if (s->spr_here[x] & 0x8000) { vdc_irq(ST_CR); break; }
        s->prof_vdc += pce_ccount() - c0;
    }
    if (!spr_irq) s->prof_lines_other++;
    pce_batch_t *b = s->batch[s->bi];
    b->line[b->n++] = L;
    s->busy = true;
    if (b->n == PCE_BATCH) lines_flush(s);
}
static void blank_line(int y)
{
    pce_t *s = PCE;
    if (y < 0 || s->skip || !s->on_line) return;
    const pce_line_t L = { (int16_t)y, 0, 0, 0, 0, 0, 0 };
    pce_batch_t *b = s->batch[s->bi];
    b->line[b->n++] = L;
    s->busy = true;
    if (b->n == PCE_BATCH) lines_flush(s);
}

// ── the I/O page ─────────────────────────────────────────────────────────────────────────────────────────
static uint8_t pce_port_read(void)
{
    pce_t *s = PCE;
    uint8_t nib = 0x0f;
    if (s->m.pad_clr) nib = 0;
    else if (s->m.pad_sel)                              // the d-pad: up, right, down, left
        nib = (uint8_t)(0x0f & ~(((s->buttons & NUCLEO_GB_UP) ? 1 : 0) | ((s->buttons & NUCLEO_GB_RIGHT) ? 2 : 0)
                               | ((s->buttons & NUCLEO_GB_DOWN) ? 4 : 0) | ((s->buttons & NUCLEO_GB_LEFT) ? 8 : 0)));
    else                                                // I, II, SELECT, RUN
        nib = (uint8_t)(0x0f & ~(((s->buttons & NUCLEO_GB_A) ? 1 : 0) | ((s->buttons & NUCLEO_GB_B) ? 2 : 0)
                               | ((s->buttons & NUCLEO_GB_SELECT) ? 4 : 0) | ((s->buttons & NUCLEO_GB_START) ? 8 : 0)));
    return (uint8_t)((s->tg16 ? 0xb0 : 0xf0) | nib);  // no CD-ROM; bit 6: a Japanese PC Engine, or a TurboGrafx
}
static void pce_port_write(uint8_t v) { PCE->m.pad_sel = v & 1; PCE->m.pad_clr = (v >> 1) & 1; }

static uint8_t io_read(uint16_t off)
{
    g_io_reads++;
    switch (off >> 10) {
        case 0: return vdc_read(off);
        case 1: return vce_read(off);
        case 2: return io_buffer_r();
        case 3: return timer_r();
        case 4: m_io_buffer = pce_port_read(); return m_io_buffer;
        case 5: return irq_status_r(off & 3);
    }
    return 0xff;
}
static void io_write(uint16_t off, uint8_t v)
{
    switch (off >> 10) {
        case 0: vdc_write(off, v); break;
        case 1: vce_write(off, v); break;
        case 2: psg_w(off & 0x0f, v); break;
        case 3: timer_w(off & 1, v); break;
        case 4: port_w(v); break;
        case 5: irq_status_w(off & 3, v); break;
    }
}

// ── waiting loops, skipped exactly ─────────────────────────────────────────────────────────────────────────
// Most games spend much of a frame in a loop waiting for an interrupt (`lda flag / beq loop`, a `bbr` on a RAM bit).
// When the CPU jumps back to an address it was at before with every register, flag, stack pointer and speed the
// same, and in between wrote nothing and read no I/O port, the memory it reads is unchanged too: until something from
// outside the CPU happens it will go round the same instructions forever. Inside a leg of run_cpu nothing outside
// happens but the CPU's own timer, so the whole iterations left before the leg ends (or before the timer would reach
// zero) are skipped: their cycles are taken off the count and the timer, and the last partial iteration and the
// interrupt itself run as always. The result is the same machine state, cycle for cycle. The candidate is checked at
// backward jumps, Brent-style (the snapshot moves at powers of two), so a loop with a delay loop inside is found too.
static struct {
    uint16_t pc;
    uint8_t  a, x, y, p, sp, cpc;
    uint32_t writes, io;
    int      icount;
    uint16_t power, lam;
    bool     armed;
} g_idle;

static PCE_HOT_FN void idle_after(void)
{
    const uint16_t pc = m_pc.w.l;
    if (pc >= m_ppc.w.l) return;                       // only a backward jump closes a loop
    if (g_idle.armed && g_idle.pc == pc && g_idle.a == m_a && g_idle.x == m_x && g_idle.y == m_y && g_idle.p == m_p
        && g_idle.sp == m_sp.b.l && g_idle.cpc == m_clocks_per_cycle && g_idle.writes == g_writes && g_idle.io == g_io_reads
        && !m_irq_pending) {
        const int period = g_idle.icount - m_icount;
        if (period > 0) {
            int k = (m_icount - 1) / period;          // whole iterations that end before the leg does
            if (m_timer_status) {
                const int t = (m_timer_value - 1) / period;   // ...and before the timer reaches zero
                if (t < k) k = t;
            }
            if (k > 0) {
                const int c = k * period;
                m_icount -= c;
                m_timer_value -= c;
                PCE->prof_idle += (uint32_t)c;
            }
        }
        g_idle.icount = m_icount;
        return;
    }
    if (++g_idle.lam >= g_idle.power || !g_idle.armed) {
        g_idle.pc = pc; g_idle.a = m_a; g_idle.x = m_x; g_idle.y = m_y; g_idle.p = m_p; g_idle.sp = m_sp.b.l;
        g_idle.cpc = m_clocks_per_cycle; g_idle.writes = g_writes; g_idle.io = g_io_reads; g_idle.icount = m_icount;
        g_idle.armed = true;
        g_idle.lam = 0;
        g_idle.power = g_idle.power >= 128 ? 1 : (uint16_t)(g_idle.power * 2);
    }
}

// ── the run loop locked in the instruction cache ─────────────────────────────────────────────────────────
// A store module runs from flash through the S3's 16 KB instruction cache, shared by both cores, and a line missed
// costs ~2 us (DIO flash at 80 MHz): the CPU loop (~7.4 KB), the line drawer on the other core and the firmware's
// panel and audio code evict each other all frame long. The ESP32-S3's cache can LOCK lines: once loaded, a locked
// line is never replaced. The loop is preloaded and locked for as long as a game runs, so it executes as if it were
// in IRAM, and costs no RAM (IRAM would take it from the heap; MEMPROT forbids code in RAM anyway). The rest of the
// code shares what is left of the cache. ROM functions (esp32s3.rom.ld), the same on every S3.
#ifdef NAP_MODULE
#define ROM_ICACHE_PRELOAD      ((uint32_t (*)(uint32_t, uint32_t, uint32_t))0x4000174c)
#define ROM_ICACHE_PRELOAD_DONE ((uint32_t (*)(void))0x40001758)
#define ROM_ICACHE_PRELOAD_END  ((void (*)(uint32_t))0x40001764)
#define ROM_ICACHE_LOCK         ((void (*)(uint32_t, uint32_t))0x4000180c)
#define ROM_ICACHE_UNLOCK       ((void (*)(uint32_t, uint32_t))0x40001818)
#define ICACHE_LINE   32u
#define LOCK_MAX      (12u * 1024u)                    // three quarters of the cache at most: the rest needs a quarter
static uint32_t s_lock_at, s_lock_lines, s_hot_a, s_hot_end;   // s_hot_*: the range asked for (the trace says it)
static void hot_unlock(void)
{
    if (s_lock_lines) ROM_ICACHE_UNLOCK(s_lock_at, s_lock_lines);
    s_lock_lines = 0;
}
// The loop and its literal pool, as the module's linker script places them (sdk/native/nap.ld: .text.nap_hot).
extern const char _nap_hot_start[] __attribute__((visibility("hidden")));
extern const char _nap_hot_end[] __attribute__((visibility("hidden")));
// bytes: 0 = the whole hot section, else that many bytes from its start.
static void hot_lock(uint32_t bytes)
{
    hot_unlock();
    const uint32_t a = (uint32_t)(uintptr_t)_nap_hot_start & ~(ICACHE_LINE - 1);
    const uint32_t end = bytes ? a + bytes : ((uint32_t)(uintptr_t)_nap_hot_end + ICACHE_LINE - 1) & ~(ICACHE_LINE - 1);
    s_hot_a = a; s_hot_end = end;
    // Only code on the instruction bus from flash (0x42000000..), and never most of the cache.
    if (a < 0x42000000u || a >= 0x44000000u || end <= a || end - a > LOCK_MAX) {
        ESP_LOGW(TAG, "run loop not locked: %08x..%08x", (unsigned)a, (unsigned)end);
        return;
    }
    const uint32_t autoload = ROM_ICACHE_PRELOAD(a, end - a, 0);
    for (int i = 0; i < 100000 && !ROM_ICACHE_PRELOAD_DONE(); i++) { }
    ROM_ICACHE_PRELOAD_END(autoload);
    s_lock_at = a; s_lock_lines = (end - a) / ICACHE_LINE;
    ROM_ICACHE_LOCK(s_lock_at, s_lock_lines);
    ESP_LOGI(TAG, "run loop locked in the cache: %u B at %08x", (unsigned)(end - a), (unsigned)a);
}
#else
static void hot_unlock(void) { }
static void hot_lock(uint32_t bytes) { (void)bytes; }
#endif

// ── the frame ────────────────────────────────────────────────────────────────────────────────────────────
static PCE_HOT_FN void run_cpu(uint32_t cycles)
{
    pce_t *s = PCE;
    s->leg_len = cycles;
    m_icount += (int)cycles;
    g_idle.armed = false;                              // a new leg: the cycle count restarts
    g_idle.power = 1; g_idle.lam = 0;
    const uint32_t c0 = pce_ccount();
    if (m_icount > 0) execute_run();
    s->prof_cpu += pce_ccount() - c0;
    s->leg_base += cycles;
}

void nucleo_pce_run_frame(void)
{
    pce_t *s = PCE;
    if (!s) return;
    const int64_t t0 = esp_timer_get_time();
    // How far behind the console is, from WORK only (as nucleo_nes.c): the time since the last frame began minus
    // what that frame spent waiting for room in the speaker's queue (a full queue is the pace, not a cost).
    int over = 0;
    if (s->last_t0) {
        const int64_t cycle = t0 - s->last_t0 - s->last_block;
        if (cycle > 0 && cycle < 250000) over = (int)cycle - 16715;
    }
    s->last_t0 = t0;
    s->debt += over;
    if (s->debt < -4000) s->debt = -4000;              // a little credit, not a bank
    if (s->debt > 100000) s->debt = 0;                 // a pause (menu, card): start over
    s->skip = emu_bench_knob[1] == 1;                  // a bench: every frame unseen (the CPU's cost alone)
    if (s->frameskip && !s->skip) {
        s->skip = s->debt > 0 && s->skipped < 3;       // behind: catch up unseen, three at most in a row
        s->skipped = s->skip ? (uint8_t)(s->skipped + 1) : 0;
    }
    int n = APU_SPF;                                   // a frame that overran by real work: its samples too, so the
    if (over > 1000) n += (int)((uint32_t)over * 4u / 125u);   // speaker never runs dry (32 kHz = 4/125 a us)
    if (n > 2 * APU_SPF) n = 2 * APU_SPF;
    s->psg_n = n; s->psg_i = 0;
    s->leg_base = 0;
    for (int v = 0; v < LINES; v++) {
        vdc_hsync();
        const int y = v - FIRST_Y;
        const int shown = (y >= 0 && y < NUCLEO_PCE_H) ? y : -1;
        if (s->m.vert_state == V_VDW) vdc_line(shown);
        else blank_line(shown);
        run_cpu(HSYNC_CYC);
        if (s->m.raster_count == s->m.rcr && (s->m.cr & 0x04)) vdc_irq(ST_RR);   // the falling edge of HSYNC
        if (v == LINES - 4) vdc_vsync(0);
        else if (v == LINES - 1) vdc_vsync(1);
        run_cpu(LINE_CYC - HSYNC_CYC);
    }
    lines_settle(s);                                   // the front-end gets the frame whole
    const int64_t t1 = esp_timer_get_time();
    s->us_cpu += (uint32_t)(t1 - t0);
    const uint32_t c0 = pce_ccount();
    psg_upto(s, FRAME_CYC + 1);                        // the rest of the frame's samples
    s->last_block = 0;
    if (s->sound) {
        emu_afilt_run(&s->afilt, s->pcm, n, 1, 1);
        const int64_t w0 = esp_timer_get_time();
        nucleo_audio_pcm_write(s->pcm, (size_t)n * sizeof s->pcm[0]);
        const int64_t w1 = esp_timer_get_time();
        s->last_block = w1 - w0;
        s->us_audio += (uint32_t)(w0 - t1);
    }
    s->prof_psg += pce_ccount() - c0;
    s->prof_frames++;
    if (!s->skip) s->prof_drawn++;
    s->frames++;
    s->clock++;
}

// ── files ────────────────────────────────────────────────────────────────────────────────────────────────
static void mkdir_one(const char *p)
{
#ifdef _WIN32
    mkdir(p);
#else
    mkdir(p, 0775);
#endif
}
static void save_base_init(pce_t *s, const char *path)
{
    const char *file = strrchr(path, '/');
    file = file ? file + 1 : path;
    const char *roms = strstr(path, "/ROMs/");
    char dir[260];
    if (roms && roms < file && file - 1 - (roms + 6) > 0 && file - 1 - (roms + 6) < 32) {
        int root_len = (int)(roms - path), sys_len = (int)(file - 1 - (roms + 6));
        snprintf(dir, sizeof dir, "%.*s/Saves", root_len, path);
        mkdir_one(dir);
        snprintf(dir, sizeof dir, "%.*s/Saves/%.*s", root_len, path, sys_len, roms + 6);
    } else snprintf(dir, sizeof dir, "%.*s/saves", (int)(file - path) > 0 ? (int)(file - 1 - path) : 0, path);
    mkdir_one(dir);
    const char *dot = strrchr(file, '.');
    int base_len = dot ? (int)(dot - file) : (int)strlen(file);
    snprintf(s->save_base, sizeof s->save_base, "%s/%.*s", dir, base_len, file);
    snprintf(s->title, sizeof s->title, "%.*s", base_len < 47 ? base_len : 47, file);
    char *cut = strstr(s->title, " (");  if (cut) *cut = '\0';
    cut = strstr(s->title, " [");        if (cut) *cut = '\0';
}

// ── open / close ─────────────────────────────────────────────────────────────────────────────────────────
static void noop_job(void *arg) { (void)arg; }
static void session_free(pce_t *s)
{
    if (!s) return;
    if (s->sound) { s->sound = false; nucleo_audio_pcm_close(); }
    if (s->fp) fclose(s->fp);
    for (int i = 0; i < s->pg_n; i += PPC) free(s->chunk[i / PPC]);
    if (s->rom_mapped) nucleo_rom_unmap(); else free(s->rom_all);
    for (int i = 0; i < VRAM_BLK; i++) free(s->vram[i]);
    free(s->batch[0]); free(s->batch[1]); free(s->spr_here); free(s->kw);
    free(s);
}

esp_err_t nucleo_pce_probe(const char *rom_path)
{
    struct stat sb;
    if (stat(rom_path, &sb) != 0) return ESP_ERR_NOT_FOUND;
    const long data = (long)sb.st_size - ((sb.st_size & 0x1fff) == 512 ? 512 : 0);
    return (data >= 0x2000 && data <= 0x280000) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static void cpu_reset(void)
{
    // MAME h6280 device_reset: high speed off, MPR7 = bank 0 (the reset vector's page), I set, timer stopped.
    m_ppc.d = 0; m_pc.d = 0; m_zp.d = 0; m_ea.d = 0;
    m_a = m_x = m_y = 0;
    m_p = 0x14;                                        // I | B
    m_sp.d = 0x01ff;
    m_mmr[7] = 0x00;
    m_mmr[6] = m_mmr[5] = m_mmr[4] = m_mmr[3] = m_mmr[2] = m_mmr[1] = m_mmr[0] = 0x00;
    m_irq_mask = m_timer_status = m_timer_ack = 0;
    m_timer_value = 0; m_timer_load = 128 * 1024;
    m_nmi_state = 0; m_irq_state[0] = m_irq_state[1] = m_irq_state[2] = 0;
    m_irq_pending = 0; m_io_buffer = 0;
    m_clocks_per_cycle = 4;
    m_icount = 0;
    windows_drop();
    m_pc.b.l = program_read8(H6280_RESET_VEC);
    m_pc.b.h = program_read8(H6280_RESET_VEC + 1);
}

static void machine_reset(void)
{
    pce_core_t *m = &PCE->m;
    memset(m, 0, sizeof *m);
    m->hsr = 0x0202; m->hdr = 0x041f; m->vpr = 0x0f02; m->vdw = 0x00ef; m->vcr = 0x0004;
    m->raster_count = 0x4000;
    m->vert_state = V_VSW;
    m->pad_sel = 1; m->pad_clr = 1;
    m->ch[4].noise_seed = m->ch[5].noise_seed = 1;
    for (int i = 0; i < 512; i++) PCE->rgb[i] = rgb_of(0);
    for (int i = 0; i < VRAM_BLK; i++) memset(PCE->vram[i], 0, 0x2000);
    cpu_reset();
}

esp_err_t nucleo_pce_open(const char *rom_path, nucleo_pce_line_fn line_cb, void *user)
{
    nucleo_pce_close();
    esp_err_t pe = nucleo_pce_probe(rom_path);
    if (pe != ESP_OK) return pe;
    // The session first (work RAM inside it: the one block of ~18 KB), then the VRAM in 8 KB blocks. Two 32 KB blocks
    // were asked for once: a heap with 148 KB free but its biggest block at 47 KB (the Games gallery's firmware)
    // refused Street Fighter II with "not enough RAM".
    pce_t *s = (pce_t *)calloc(1, sizeof *s);
    if (!s) return ESP_ERR_NO_MEM;
    for (int i = 0; i < VRAM_BLK; i++)
        if (!(s->vram[i] = (uint16_t *)malloc(0x2000))) { session_free(s); return ESP_ERR_NO_MEM; }
    s->heap_bytes = sizeof *s + 0x10000;
    s->batch[0] = (pce_batch_t *)calloc(1, sizeof(pce_batch_t));
    s->batch[1] = (pce_batch_t *)calloc(1, sizeof(pce_batch_t));
    s->spr_here = (uint16_t *)malloc(512 * sizeof(uint16_t));
    s->kw = (pce_scratch_t *)malloc(sizeof(pce_scratch_t));
    if (!s->batch[0] || !s->batch[1] || !s->spr_here || !s->kw) { session_free(s); return ESP_ERR_NO_MEM; }
    s->heap_bytes += 2 * sizeof(pce_batch_t) + 512 * sizeof(uint16_t) + sizeof(pce_scratch_t);
    s->on_line = line_cb; s->user = user;
    s->pending = -1;
    s->cur_tag = 0xFFFFFFFFu;
    memset(s->win_pg, 0xff, sizeof s->win_pg);
    for (int i = 0; i < PG_SLOTS; i++) s->pg_tag[i] = PG_EMPTY;
    memset(s->hint, -1, sizeof s->hint);
    save_base_init(s, rom_path);
    const char *roms = strstr(rom_path, "/data/ROMs/");
    if (roms) snprintf(s->trace_path, sizeof s->trace_path, "%.*s/pceemu_trace.txt", (int)(roms - rom_path), rom_path);
    plane_init();
    s->fp = fopen(rom_path, "rb");
    if (!s->fp) { session_free(s); return ESP_ERR_NOT_FOUND; }
    fseek(s->fp, 0, SEEK_END);
    const long sz = ftell(s->fp);
    fseek(s->fp, 0, SEEK_SET);
    s->rom_bytes = (uint32_t)sz;
    s->rom_base = (sz & 0x1fff) == 512 ? 512 : 0;      // a copier's header
    s->data_bytes = s->rom_bytes - s->rom_base;
    s->sf2 = s->data_bytes > 0x100000;                 // only Street Fighter II is bigger than the 1 MB card space
    // The region from the file's name (No-Intro: "(USA)"): American cards refuse a Japanese console and vice versa.
    s->tg16 = strstr(rom_path, "(USA") != NULL || strstr(rom_path, "(U)") != NULL;

    s->sound = nucleo_audio_pcm_open_ex(APU_RATE, 1, EMU_AUDIO_QUEUE_FRAMES * APU_SPF) == ESP_OK;
    if (!s->sound) ESP_LOGW(TAG, "speaker unavailable — playing silently");

#ifdef NUCLEO_HOST_TEST
    const char *tp = getenv("PCE_PAGES");               // the host harness: the device's page cache, by number
    const int test_pages = tp ? atoi(tp) : 0;
#else
    const int test_pages = 0;
#endif
    if (!test_pages && (size_t)sz + 24 * 1024 <= heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)) {
        s->rom_all = (uint8_t *)malloc((size_t)sz);
        if (s->rom_all && fread(s->rom_all, 1, (size_t)sz, s->fp) != (size_t)sz) { free(s->rom_all); s->rom_all = NULL; }
        if (s->rom_all) { s->heap_bytes += (size_t)sz; fclose(s->fp); s->fp = NULL; }
    }
    // Too big for RAM: the cartridge mapped from the internal flash, read like RAM (copied there the first time).
    // No page cache, no card reads while playing, and the RAM the cache took stays with the game.
    if (!s->rom_all && !test_pages) {
        uint32_t msz = 0;
        const uint8_t *m = nucleo_rom_map(rom_path, &msz, emu_rom_progress, NULL);
        if (m && msz == (uint32_t)sz) {
            s->rom_all = (uint8_t *)m;                   // never written: the windows only read it
            s->rom_mapped = true;
            fclose(s->fp); s->fp = NULL;
        } else if (m) {
            nucleo_rom_unmap();
        }
    }
    // The other core's worker (its 4 KB stack) before the cartridge pages take the heap down to KEEP.
    nucleo_cowork_run(noop_job, NULL);
    nucleo_cowork_wait();
    if (!s->rom_all) {
        const uint32_t pages = (s->data_bytes + PG_SIZE - 1) / PG_SIZE;
        while (s->pg_n + (int)PPC <= PG_SLOTS && (uint32_t)s->pg_n < pages && (!test_pages || s->pg_n < test_pages)) {
            if (heap_caps_get_free_size(MALLOC_CAP_DEFAULT) < PG_CHUNK + KEEP) break;
            uint8_t *b = (uint8_t *)malloc(PG_CHUNK);
            if (!b) break;
            s->chunk[s->pg_n / PPC] = b;
            s->pg_n += PPC;
            s->heap_bytes += PG_CHUNK;
        }
        if (s->pg_n < PG_MIN) { ESP_LOGE(TAG, "only %d cartridge pages fit", s->pg_n); session_free(s); return ESP_ERR_NO_MEM; }
    }
    // The PSG's 48 dB over 32 steps (MAME c6280's table, without doubles).
    float level = 65535.0f / 6.0f / 32.0f;
    for (int i = 0; i < 31; i++) { s->vol[i] = (uint16_t)level; level *= 0.841395f; }
    s->vol[31] = 0;
    PCE = s;
    map_banks();
    machine_reset();
    if (emu_bench_knob[0] >= 0) hot_lock((uint32_t)emu_bench_knob[0]);   // a bench may turn it off (-1) or size it
    ESP_LOGI(TAG, "'%s' %u KB%s, %s, heap %u B, sound %s", s->title, (unsigned)(s->data_bytes / 1024), s->sf2 ? " (SF2 mapper)" : "",
             s->rom_mapped ? "mapped from flash" : s->rom_all ? "resident" : "paged", (unsigned)s->heap_bytes, s->sound ? "on" : "off");
    return ESP_OK;
}

#ifdef NAP_MODULE
static uint32_t lock_bytes(void) { return s_lock_lines * ICACHE_LINE; }
static uint32_t hot_a(void) { return s_hot_a; }
static uint32_t hot_end(void) { return s_hot_end; }
#else
static uint32_t lock_bytes(void) { return 0; }
static uint32_t hot_a(void) { return 0; }
static uint32_t hot_end(void) { return 0; }
#endif

// The VRAM in a save state: its blocks in order (the file is the same as when it was two halves).
static bool vram_io(FILE *f, bool out)
{
    for (int i = 0; i < VRAM_BLK; i++)
        if ((out ? fwrite(PCE->vram[i], 1, 0x2000, f) : fread(PCE->vram[i], 1, 0x2000, f)) != 0x2000) return false;
    return true;
}

// Where the frames' time went, once per game, appended to the emulator's trace: tenths of a millisecond per frame at
// 240 MHz (1024-cycle units by a shift: no 64-bit division in a module; k * 1024 / 24000 = k * 16 / 375).
static void profile_write(pce_t *s)
{
    if (!s->prof_frames || !s->trace_path[0] || !s->prof_cpu) return;
    const uint32_t n = s->prof_frames;
    const unsigned c = (unsigned)((uint32_t)(s->prof_cpu >> 10) / n * 16 / 375), v = (unsigned)((uint32_t)(s->prof_vdc >> 10) / n * 16 / 375),
                   l = (unsigned)((uint32_t)(s->prof_line >> 10) / n * 16 / 375), a = (unsigned)((uint32_t)(s->prof_psg >> 10) / n * 16 / 375),
                   w = (unsigned)((uint32_t)(s->prof_wait >> 10) / n * 16 / 375);
    FILE *f = fopen(s->trace_path, "a");
    if (!f) return;
    fprintf(f, "  pce profile: %u frames (%u drawn), ms/frame: huc6280 %u.%u  vdc %u.%u  panel lines %u.%u  wait 2nd core %u.%u  psg+speaker %u.%u, idle skipped %u%%, lines with sprite irq evaluated here %u%%, cartridge %s, misses %u, run loop locked %u B (hot %08x..%08x), bench knobs %d %d\n",
            (unsigned)n, (unsigned)s->prof_drawn, c / 10, c % 10, v / 10, v % 10, l / 10, l % 10, w / 10, w % 10, a / 10, a % 10,
            (unsigned)((uint32_t)(s->prof_idle >> 10) / n * 1024u / (LINES * LINE_CYC / 100)),   // no 64-bit division
            (unsigned)(s->prof_lines_here * 100u / (s->prof_lines_other + s->prof_lines_here + 1)),
            s->rom_mapped ? "mapped from flash" : s->rom_all ? "in RAM" : "paged from the card", (unsigned)s->prof_misses,
            (unsigned)(lock_bytes()), (unsigned)hot_a(), (unsigned)hot_end(), emu_bench_knob[0], emu_bench_knob[1]);
    fclose(f);
}

void nucleo_pce_close(void)
{
    if (!PCE) return;
    lines_settle(PCE);
    nucleo_cowork_stop();
    profile_write(PCE);                                // (it reports the lock)
    hot_unlock();
    pce_t *s = PCE;
    PCE = NULL;
    session_free(s);
}

void nucleo_pce_reset(void) { if (PCE) machine_reset(); }
void nucleo_pce_set_buttons(uint8_t m) { if (PCE) PCE->buttons = m; }
const char *nucleo_pce_title(void) { return PCE ? PCE->title : ""; }
// The front-end's relief lever (as the NES): on, a frame is drawn only while the console is not behind; never more
// than three unseen in a row. Off, every frame is drawn.
void nucleo_pce_set_frameskip(bool on) { if (PCE) { PCE->frameskip = on; PCE->debt = 0; PCE->skipped = 0; } }
void nucleo_pce_geometry(int *w, int *h) { *w = NUCLEO_PCE_W; *h = NUCLEO_PCE_H; }
void nucleo_pce_save(void) {}
bool nucleo_pce_autosave(void) { return false; }

// ── save states: "PCS1", the CPU, the machine, VRAM ────────────────────────────────────────────────────────
#define STATE_MAGIC 0x31534350u
typedef struct {
    PAIR ppc, pc, sp, zp, ea;
    uint8_t a, x, y, p, mmr[8], irq_mask, timer_status, timer_ack, clocks_per_cycle;
    int32_t timer_value, timer_load;
    uint8_t nmi_state, irq_state[3], irq_pending, io_buffer;
    int32_t icount;
} cpu_state_t;
static void cpu_get(cpu_state_t *c)
{
    c->ppc = m_ppc; c->pc = m_pc; c->sp = m_sp; c->zp = m_zp; c->ea = m_ea;
    c->a = m_a; c->x = m_x; c->y = m_y; c->p = m_p; memcpy(c->mmr, m_mmr, 8);
    c->irq_mask = m_irq_mask; c->timer_status = m_timer_status; c->timer_ack = m_timer_ack; c->clocks_per_cycle = m_clocks_per_cycle;
    c->timer_value = m_timer_value; c->timer_load = m_timer_load;
    c->nmi_state = m_nmi_state; memcpy(c->irq_state, m_irq_state, 3); c->irq_pending = m_irq_pending; c->io_buffer = m_io_buffer;
    c->icount = m_icount;
}
static void cpu_set(const cpu_state_t *c)
{
    m_ppc = c->ppc; m_pc = c->pc; m_sp = c->sp; m_zp = c->zp; m_ea = c->ea;
    m_a = c->a; m_x = c->x; m_y = c->y; m_p = c->p; memcpy(m_mmr, c->mmr, 8);
    m_irq_mask = c->irq_mask; m_timer_status = c->timer_status; m_timer_ack = c->timer_ack; m_clocks_per_cycle = c->clocks_per_cycle;
    m_timer_value = c->timer_value; m_timer_load = c->timer_load;
    m_nmi_state = c->nmi_state; memcpy(m_irq_state, c->irq_state, 3); m_irq_pending = c->irq_pending; m_io_buffer = c->io_buffer;
    m_icount = c->icount;
}
static void state_path(char *out, size_t n, int slot) { snprintf(out, n, "%s.st%d", PCE->save_base, slot < 0 ? 0 : slot > 9 ? 9 : slot); }
static long state_bytes(void) { return (long)(4 + sizeof(cpu_state_t) + sizeof(pce_core_t) + 0x10000); }

bool nucleo_pce_state_exists(int slot)
{
    if (!PCE) return false;
    char p[320]; state_path(p, sizeof p, slot);
    struct stat sb;
    return stat(p, &sb) == 0 && sb.st_size == state_bytes();
}
esp_err_t nucleo_pce_state_save(int slot)
{
    if (!PCE) return ESP_ERR_INVALID_STATE;
    char p[320]; state_path(p, sizeof p, slot);
    FILE *f = fopen(p, "wb");
    if (!f) return ESP_FAIL;
    cpu_state_t c; cpu_get(&c);
    const uint32_t magic = STATE_MAGIC;
    bool ok = fwrite(&magic, 1, 4, f) == 4 && fwrite(&c, 1, sizeof c, f) == sizeof c
           && fwrite(&PCE->m, 1, sizeof PCE->m, f) == sizeof PCE->m
           && vram_io(f, true);
    ok = fclose(f) == 0 && ok;
    if (!ok) { remove(p); return ESP_FAIL; }
    return ESP_OK;
}
esp_err_t nucleo_pce_state_load(int slot)
{
    if (!PCE) return ESP_ERR_INVALID_STATE;
    if (!nucleo_pce_state_exists(slot)) return ESP_ERR_NOT_FOUND;
    char p[320]; state_path(p, sizeof p, slot);
    FILE *f = fopen(p, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    cpu_state_t c;
    uint32_t magic = 0;
    bool ok = fread(&magic, 1, 4, f) == 4 && magic == STATE_MAGIC && fread(&c, 1, sizeof c, f) == sizeof c
           && fread(&PCE->m, 1, sizeof PCE->m, f) == sizeof PCE->m
           && vram_io(f, false);
    fclose(f);
    if (!ok) { machine_reset(); return ESP_ERR_INVALID_CRC; }
    cpu_set(&c);
    for (int i = 0; i < 512; i++) PCE->rgb[i] = rgb_of(PCE->m.pal[i]);
    windows_drop();
    return ESP_OK;
}

void nucleo_pce_get_stats(nucleo_gb_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    pce_t *s = PCE;
    if (!s) return;
    out->heap_bytes = s->heap_bytes;
    out->rom_bytes = s->data_bytes;
    out->rom_resident = s->rom_all != NULL;
    out->rom_paged = s->rom_all == NULL;
    out->rom_pages = s->pg_n;
    out->bank_misses = s->misses;
    out->frames = s->frames;
    out->us_cpu = s->us_cpu;
    out->us_audio = s->us_audio;
    out->us_miss = s->us_miss;
}
void nucleo_pce_reset_counters(void)
{
    if (!PCE) return;
    PCE->us_cpu = PCE->us_audio = PCE->us_miss = 0;
    PCE->misses = 0;
    PCE->frames = 0;
}
