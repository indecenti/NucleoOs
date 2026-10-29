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
#include "nucleo_pce.h"
#include "nucleo_audio.h"
#include "emu_audio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../vendor/h6280/h6280_core.h"

static const char *TAG = "pce";

#define PG_BITS   10
#define PG_SIZE   (1u << PG_BITS)
#define PG_SLOTS  160
#define PG_MIN    16
#define PG_HINT   256
#define PG_EMPTY  0xFFFFFFFFu
#define KEEP      (8 * 1024)

#define LINES     263
#define LINE_CYC  455                                  // 1365 master clocks / 3
#define HSYNC_CYC 376                                  // the falling edge of HSYNC: the raster-compare interrupt
#define FIRST_Y   16                                   // the line the usual 240-line display starts on
#define APU_RATE  32000
#define APU_SPF   535                                  // 32000 / 59.826
#define PSG_HZ    3579545
#define LOG_MAX   1024

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

typedef struct {
    uint32_t cyc;                                    // cycle in the frame
    uint8_t  reg, val;
} psg_log_t;

typedef struct {
    pce_core_t m;
    uint16_t  *vram[2];                              // 2 x 16 K words
    FILE      *fp;
    uint32_t   rom_base, rom_bytes, data_bytes;      // the HuCard data starts at rom_base (a 512-byte copier header)
    uint8_t   *rom_all;
    uint8_t   *pg[PG_SLOTS];
    uint32_t   pg_tag[PG_SLOTS], pg_age[PG_SLOTS], pg_clock;
    int        pg_n;
    int8_t     hint[PG_HINT];
    uint32_t   cur_tag;
    uint8_t   *cur_buf;
    uint32_t   misses;
    uint32_t   bank_off[128];                        // HuCard bank -> offset in the data (the console's mirroring)
    bool       sf2;
    bool       tg16;                                 // a TurboGrafx-16 card (its pad port reads "not Japanese")
    const uint8_t *rd[64];                           // CPU 1 KB windows (NULL: the slow path)
    uint8_t   *wr[64];
    uint16_t   rgb[512];                             // the palette in the panel's RGB565 order
    uint16_t   line[512];                            // a drawn line, up to the VDC's widest
    uint16_t   out[NUCLEO_PCE_W];
    uint8_t    bg[528];
    uint16_t   spr[512];
    nucleo_pce_line_fn on_line;
    void      *user;
    uint8_t    buttons;
    psg_log_t  log[LOG_MAX];
    int        log_n;
    uint32_t   frame_cyc, leg_base, leg_len;         // where the CPU is in the frame (for the PSG log)
    int16_t    pcm[APU_SPF];
    uint16_t   vol[32];
    emu_afilt_t afilt;
    bool       sound, frameskip, skip;
    char       save_base[300];
    char       title[48];
    size_t     heap_bytes;
    uint32_t   frames, us_cpu, us_audio, clock;
} pce_t;

static pce_t *PCE;

// ── the cartridge, paged (as nucleo_nes.c) ────────────────────────────────────────────────────────────────
static const uint8_t PG_OPEN[PG_SIZE] = { [0 ... PG_SIZE - 1] = 0xFF };
static void windows_drop(void) { memset(PCE->rd, 0, sizeof PCE->rd); memset(PCE->wr, 0, sizeof PCE->wr); }

static const uint8_t *rom_page(uint32_t data_off)
{
    pce_t *s = PCE;
    if (data_off >= s->data_bytes) return PG_OPEN;
    const uint32_t tag = data_off >> PG_BITS;
    if (s->rom_all) return s->rom_all + s->rom_base + (tag << PG_BITS);
    if (tag == s->cur_tag) return s->cur_buf;
    int h = (int)(tag & (PG_HINT - 1)), g = s->hint[h];
    if (g < 0 || g >= s->pg_n || s->pg_tag[g] != tag) {
        g = -1;
        for (int i = 0; i < s->pg_n; i++) if (s->pg_tag[i] == tag) { g = i; break; }
        if (g < 0) {
            int v = 0;
            for (int i = 1; i < s->pg_n; i++) if (s->pg_age[i] < s->pg_age[v]) v = i;
            size_t got = 0;
            if (fseek(s->fp, (long)(s->rom_base + (tag << PG_BITS)), SEEK_SET) == 0) got = fread(s->pg[v], 1, PG_SIZE, s->fp);
            if (got < PG_SIZE) memset(s->pg[v] + got, 0xFF, PG_SIZE - got);
            s->pg_tag[v] = tag;
            s->misses++;
            g = v;
            windows_drop();                          // a window may have pointed at the page just replaced
        }
        s->hint[h] = (int8_t)g;
    }
    s->pg_age[g] = ++s->pg_clock;
    s->cur_tag = tag; s->cur_buf = s->pg[g];
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
static const uint8_t *window(int slot)
{
    pce_t *s = PCE;
    const uint8_t bank = m_mmr[slot >> 3];
    const uint32_t k = (uint32_t)(slot & 7) << 10;
    const uint8_t *r = NULL;
    uint8_t *w = NULL;
    if (bank < 0x80) {
        uint32_t off = (s->sf2 && bank >= 0x40) ? (uint32_t)bank * 0x2000 + s->m.sf2_off : s->bank_off[bank];
        r = rom_page(off + k);
    } else if (bank >= 0xF8 && bank <= 0xFB) {
        r = w = s->m.wram + k;
    } else if (bank != 0xFF) {
        r = PG_OPEN;
    }
    s->rd[slot] = r;                                 // after rom_page: a miss there drops every window
    s->wr[slot] = w;
    return r;
}

static void pce_mpr_changed(int page)
{
    for (int k = 0; k < 8; k++) { PCE->rd[page * 8 + k] = NULL; PCE->wr[page * 8 + k] = NULL; }
}

static inline uint8_t mem_read(uint16_t addr)
{
    const uint8_t *w = PCE->rd[addr >> 10];
    if (!w && !(w = window(addr >> 10))) return io_read(addr & 0x1fff);
    return w[addr & 0x3ff];
}
static inline void mem_write(uint16_t addr, uint8_t v)
{
    uint8_t *w = PCE->wr[addr >> 10];
    if (w) { w[addr & 0x3ff] = v; return; }
    const uint8_t bank = m_mmr[addr >> 13];
    if (bank >= 0xF8 && bank <= 0xFB) { window(addr >> 10); PCE->wr[addr >> 10][addr & 0x3ff] = v; return; }
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
static uint8_t  program_read8(uint16_t addr)            { check_vdc_vce_penalty(addr); return mem_read(addr); }
static void     program_write8(uint16_t addr, uint8_t v) { check_vdc_vce_penalty(addr); mem_write(addr, v); }
static uint8_t  program_read8z(uint16_t addr)           { return mem_read((uint16_t)(0x2000 | (addr & 0x1fff))); }
static void     program_write8z(uint16_t addr, uint8_t v) { mem_write((uint16_t)(0x2000 | (addr & 0x1fff)), v); }
static uint16_t program_read16(uint16_t addr)           { return (uint16_t)(mem_read(addr) | mem_read((uint16_t)(addr + 1)) << 8); }
static uint16_t program_read16z(uint16_t addr)
{
    const uint16_t hi = (addr & 0xff) == 0xff ? (uint16_t)(addr - 0xff) : (uint16_t)(addr + 1);
    return (uint16_t)(program_read8z(addr) | program_read8z(hi) << 8);
}
static void     push(uint8_t v)                         { program_write8z((uint16_t)m_sp.d, v); m_sp.b.l--; }
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
static void pce_psg_write(int reg, uint8_t v)
{
    pce_t *s = PCE;
    if (s->log_n < LOG_MAX) {
        s->log[s->log_n].cyc = s->leg_base + (uint32_t)(s->leg_len - (m_icount > 0 ? m_icount : 0));
        s->log[s->log_n].reg = (uint8_t)reg;
        s->log[s->log_n].val = v;
        s->log_n++;
    } else {
        psg_reg((uint8_t)reg, v);                     // a full log: apply at once (tone kept, timing lost)
    }
}

// One frame of sound: the logged writes applied at their moment, the channels stepped in PSG clocks.
static void psg_render(pce_t *s)
{
    pce_core_t *m = &s->m;
    static const uint8_t SCALE[16] = { 0x00, 0x03, 0x05, 0x07, 0x09, 0x0b, 0x0d, 0x0f, 0x10, 0x13, 0x15, 0x17, 0x19, 0x1b, 0x1d, 0x1f };
    const uint32_t cps = (uint32_t)(((uint64_t)PSG_HZ << 16) / APU_RATE);   // PSG clocks per sample, 16.16
    const uint32_t frame_cyc = LINES * LINE_CYC;
    int li = 0;
    for (int i = 0; i < APU_SPF; i++) {
        const uint32_t at = (uint32_t)((uint64_t)i * frame_cyc / APU_SPF);
        while (li < s->log_n && s->log[li].cyc <= at) { psg_reg(s->log[li].reg, s->log[li].val); li++; }
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
    while (li < s->log_n) { psg_reg(s->log[li].reg, s->log[li].val); li++; }
    s->log_n = 0;
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
static inline uint16_t vram_rd(uint32_t a) { a &= 0xffff; return a <= 0x7fff ? PCE->vram[a >> 14][a & 0x3fff] : 0; }
static inline void vram_wr(uint32_t a, uint16_t v) { a &= 0xffff; if (a <= 0x7fff) PCE->vram[a >> 14][a & 0x3fff] = v; }
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
static void pce_vdc_st(int port, uint8_t v) { vdc_write((uint16_t)port, v); }

static void vdc_vblank(void)
{
    pce_core_t *m = &PCE->m;
    if (m->vd_triggered) return;
    if (m->cr & 0x08) vdc_irq(ST_VD);
    if (m->dvssr_written || (m->dcr & 0x10)) {        // VRAM -> SAT DMA
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

// The sprites on this line into PCE->spr (index 0 = screen x 0): MAME's select_sprites / add_sprite.
static void vdc_sprites(int width)
{
    pce_core_t *m = &PCE->m;
    static const int CGY[4] = { 16, 32, 64, 64 };
    uint16_t *row = PCE->spr;
    memset(row, 0, (size_t)width * sizeof *row);
    int cells = 0;
    for (int i = 0; i < 256 && cells < 16; i += 4) {
        const int cgy = (m->sat[i + 3] >> 12) & 3, height = CGY[cgy];
        int line = m->raster_count - 1 - m->sat[i];
        if (line < 0 || line >= height) continue;
        const int xpos = m->sat[i + 1];
        int pattern = m->sat[i + 2] >> 1;
        const bool lsb = m->sat[i + 2] & 1, prio = (m->sat[i + 3] >> 7) & 1, cgx = (m->sat[i + 3] >> 8) & 1;
        const int pal = m->sat[i + 3] & 0x0f;
        if (cgy & 1) pattern &= ~0x0002;
        if (cgy & 2) pattern &= ~0x0006;
        if (m->sat[i + 3] & 0x8000) line = (height - 1) - line;
        const bool fx = (m->sat[i + 3] >> 11) & 1;
        const int n = cgx ? 2 : 1;
        if (cgx) pattern &= ~0x0001;
        for (int part = 0; part < n && cells < 16; part++) {
            const int pat = pattern + (cgx ? (fx ? 1 - part : part) : 0) + ((line >> 4) << 1);
            const uint32_t pa = ((uint32_t)pat << 6) + (line & 0x0f);
            uint32_t b0, b1, b2 = 0, b3 = 0;
            if ((m->mwr & 0x0c) == 0x04) {
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
    if (cells >= 16 && (m->cr & 0x02)) vdc_irq(ST_OR);
}

// One display line, drawn whole from the registers as they are now; y is the output row (or -1: not shown).
static void vdc_line(int y)
{
    pce_t *s = PCE;
    pce_core_t *m = &s->m;
    int width = ((m->hdr & 0x7f) + 1) * 8;
    if (width > 512) width = 512;
    uint16_t *out = s->line;
    const uint16_t over = s->rgb[0x100];
    const bool bg_on = (m->cr & 0x80) != 0, spr_on = (m->cr & 0x40) != 0;
    if (bg_on) {
        static const int WS[4] = { 5, 6, 7, 7 };
        const int ws = WS[(m->mwr >> 4) & 3];
        const uint32_t amask = (1u << ws) - 1;
        const uint16_t v = m->byr_latched & ((m->mwr & 0x40) ? 0x1ff : 0xff);
        const uint32_t base = (uint32_t)(v >> 3) << ws, row = v & 7;
        uint32_t col = (m->bxr >> 3) & amask;
        uint8_t *dst = s->bg;
        for (int t = 0; t <= width / 8; t++, dst += 8, col = (col + 1) & amask) {
            const uint16_t bat = vram_rd(base | col);
            const uint8_t pal = (uint8_t)((bat >> 8) & 0xf0);
            const uint32_t ta = ((uint32_t)(bat & 0x0fff) << 4) + row;
            const uint16_t d1 = vram_rd(ta), d3 = vram_rd(ta + 8);
            if (!(d1 | d3)) { memset(dst, 0, 8); continue; }
            for (int j = 0; j < 8; j++) {
                const int b = 7 - j;
                const uint8_t c = (uint8_t)(((d1 >> b) & 1) | (((d1 >> (b + 8)) & 1) << 1) | (((d3 >> b) & 1) << 2) | (((d3 >> (b + 8)) & 1) << 3));
                dst[j] = c ? (uint8_t)(c | pal) : 0;
            }
        }
    }
    if (spr_on) vdc_sprites(width);
    if (y < 0 || s->skip) {                             // not shown: only what the CPU can see (collision)
        if (spr_on && bg_on && (m->cr & 0x01)) {
            for (int x = 0; x < width; x++) if (s->spr[x] & 0x8000) { vdc_irq(ST_CR); break; }
        }
        return;
    }
    const uint8_t *bg = s->bg + (m->bxr & 7);
    bool coll = false;
    for (int x = 0; x < width; x++) {
        uint16_t data = 0x100;                         // nothing drawn: the sprite palette's colour 0
        const uint16_t sp = spr_on ? s->spr[x] : 0;
        const uint8_t sd = (uint8_t)(sp & 0xff);
        if (bg_on) {
            data = bg[x];
            if (sd && ((sp & 0x4000) || !(data & 0x0f))) data = (uint16_t)(0x100 | sd);
        } else if (spr_on) {
            data = (uint16_t)(0x100 | sd);
        }
        if (sp & 0x8000) coll = true;
        out[x] = s->rgb[data & 0x1ff];
    }
    if (coll && (m->cr & 0x01)) vdc_irq(ST_CR);
    // To the 256 columns the front-end takes: narrower lines centred on the overscan colour, wider ones resampled.
    uint16_t *o = s->out;
    if (width <= NUCLEO_PCE_W) {
        const int pad = (NUCLEO_PCE_W - width) / 2;
        for (int x = 0; x < pad; x++) o[x] = over;
        memcpy(o + pad, out, (size_t)width * sizeof *o);
        for (int x = pad + width; x < NUCLEO_PCE_W; x++) o[x] = over;
    } else {
        for (int x = 0; x < NUCLEO_PCE_W; x++) o[x] = out[x * width / NUCLEO_PCE_W];
    }
    if (s->on_line) s->on_line(o, y, s->user);
}
static void blank_line(int y)
{
    pce_t *s = PCE;
    if (y < 0 || s->skip || !s->on_line) return;
    for (int x = 0; x < NUCLEO_PCE_W; x++) s->out[x] = s->rgb[0x100];
    s->on_line(s->out, y, s->user);
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

// ── the frame ────────────────────────────────────────────────────────────────────────────────────────────
static void run_cpu(uint32_t cycles)
{
    pce_t *s = PCE;
    s->leg_len = cycles;
    m_icount += (int)cycles;
    if (m_icount > 0) execute_run();
    s->leg_base += cycles;
}

void nucleo_pce_run_frame(void)
{
    pce_t *s = PCE;
    if (!s) return;
    const int64_t t0 = esp_timer_get_time();
    s->skip = s->frameskip && (s->clock & 1);
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
    const int64_t t1 = esp_timer_get_time();
    s->us_cpu += (uint32_t)(t1 - t0);
    psg_render(s);
    if (s->sound) {
        emu_afilt_run(&s->afilt, s->pcm, APU_SPF, 1, 1);
        nucleo_audio_pcm_write(s->pcm, sizeof s->pcm);
        s->us_audio += (uint32_t)(esp_timer_get_time() - t1);
    }
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
static void session_free(pce_t *s)
{
    if (!s) return;
    if (s->sound) { s->sound = false; nucleo_audio_pcm_close(); }
    if (s->fp) fclose(s->fp);
    for (int i = 0; i < s->pg_n; i++) free(s->pg[i]);
    free(s->rom_all);
    free(s->vram[0]); free(s->vram[1]);
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
    memset(PCE->vram[0], 0, 0x8000); memset(PCE->vram[1], 0, 0x8000);
    PCE->log_n = 0;
    cpu_reset();
}

esp_err_t nucleo_pce_open(const char *rom_path, nucleo_pce_line_fn line_cb, void *user)
{
    nucleo_pce_close();
    esp_err_t pe = nucleo_pce_probe(rom_path);
    if (pe != ESP_OK) return pe;
    // VRAM first, while the heap still has two 32 KB blocks; then the session (work RAM inside it).
    uint16_t *v0 = (uint16_t *)malloc(0x8000), *v1 = v0 ? (uint16_t *)malloc(0x8000) : NULL;
    if (!v0 || !v1) { free(v0); return ESP_ERR_NO_MEM; }
    pce_t *s = (pce_t *)calloc(1, sizeof *s);
    if (!s) { free(v0); free(v1); return ESP_ERR_NO_MEM; }
    s->vram[0] = v0; s->vram[1] = v1;
    s->heap_bytes = sizeof *s + 0x10000;
    s->on_line = line_cb; s->user = user;
    s->cur_tag = PG_EMPTY;
    for (int i = 0; i < PG_SLOTS; i++) s->pg_tag[i] = PG_EMPTY;
    memset(s->hint, -1, sizeof s->hint);
    save_base_init(s, rom_path);
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

    if ((size_t)sz + 24 * 1024 <= heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)) {
        s->rom_all = (uint8_t *)malloc((size_t)sz);
        if (s->rom_all && fread(s->rom_all, 1, (size_t)sz, s->fp) != (size_t)sz) { free(s->rom_all); s->rom_all = NULL; }
        if (s->rom_all) { s->heap_bytes += (size_t)sz; fclose(s->fp); s->fp = NULL; }
    }
    if (!s->rom_all) {
        const uint32_t pages = (s->data_bytes + PG_SIZE - 1) / PG_SIZE;
        for (int i = 0; i < PG_SLOTS && (uint32_t)i < pages; i++) {
            if (heap_caps_get_free_size(MALLOC_CAP_DEFAULT) < PG_SIZE + KEEP) break;
            uint8_t *b = (uint8_t *)malloc(PG_SIZE);
            if (!b) break;
            s->pg[s->pg_n++] = b;
            s->heap_bytes += PG_SIZE;
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
    ESP_LOGI(TAG, "'%s' %u KB%s, %s, heap %u B, sound %s", s->title, (unsigned)(s->data_bytes / 1024), s->sf2 ? " (SF2 mapper)" : "",
             s->rom_all ? "resident" : "paged", (unsigned)s->heap_bytes, s->sound ? "on" : "off");
    return ESP_OK;
}

void nucleo_pce_close(void)
{
    if (!PCE) return;
    pce_t *s = PCE;
    PCE = NULL;
    session_free(s);
}

void nucleo_pce_reset(void) { if (PCE) machine_reset(); }
void nucleo_pce_set_buttons(uint8_t m) { if (PCE) PCE->buttons = m; }
const char *nucleo_pce_title(void) { return PCE ? PCE->title : ""; }
void nucleo_pce_set_frameskip(bool on) { if (PCE) PCE->frameskip = on; }
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
           && fwrite(PCE->vram[0], 1, 0x8000, f) == 0x8000 && fwrite(PCE->vram[1], 1, 0x8000, f) == 0x8000;
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
           && fread(PCE->vram[0], 1, 0x8000, f) == 0x8000 && fread(PCE->vram[1], 1, 0x8000, f) == 0x8000;
    fclose(f);
    if (!ok) { machine_reset(); return ESP_ERR_INVALID_CRC; }
    cpu_set(&c);
    for (int i = 0; i < 512; i++) PCE->rgb[i] = rgb_of(PCE->m.pal[i]);
    PCE->log_n = 0;
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
}
void nucleo_pce_reset_counters(void)
{
    if (!PCE) return;
    PCE->us_cpu = PCE->us_audio = 0;
    PCE->misses = 0;
    PCE->frames = 0;
}
