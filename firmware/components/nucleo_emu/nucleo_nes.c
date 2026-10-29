// nucleo_nes — the host half of the NES: see include/nucleo_nes.h.
//
// HOW IT FITS (the same rules as the Game Boy and the Game Gear: contiguous RAM is the budget)
//   console      agnes_t, ~23 KB in one block (2 KB work RAM, 4 KB name tables, the mapper's 8 KB PRG and 8 KB
//                CHR RAM); with the radio off the Solo boot's heap has a 45 KB block for it
//   picture      one 256-pixel scanline at a time (patch A2): no 61 KB frame buffer
//   cartridge    1 KB pages from the SD card, LRU with a direct-mapped hint (as nucleo_gb.c), or the whole file
//                when it fits with room to spare (most NROM games are 24-40 KB). Pages are counted from the PRG
//                data (after the header and any trainer), so each 1 KB the CPU or PPU sees is exactly one page:
//                agnes keeps direct pointers to them (patch A5) and reads the cartridge without calling us
//   speed        agnes draws a scanline at a time (patch A4), not a dot at a time
//   battery RAM  the MMC1 / MMC3 8 KB PRG RAM, "<root>/Saves/nes/<rom name>.sav", byte-compatible with other
//                emulators' saves
//   sound        our APU: agnes has none. Two pulses, triangle, noise and the DMC's direct output level, with
//                the frame sequencer, envelopes, sweeps and length counters, rendered a frame at a time at
//                32 kHz and mixed through the NES's own non-linear mixer curves
#include "nucleo_nes.h"
#include "nucleo_audio.h"
#include "emu_audio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define AGNES_IMPLEMENTATION
#include "../vendor/agnes/agnes.c"

static const char *TAG = "nes";

#define PG_BITS   10
#define PG_SIZE   (1u << PG_BITS)
#define PG_SLOTS  120
#define PG_MIN    12
#define PG_HINT   256
#define PG_EMPTY  0xFFFFFFFFu
#define KEEP      (8 * 1024)

#define APU_RATE  32000
#define APU_SPF   533                           // 32000 / 60.0988 (NTSC)
#define CPU_HZ    1789773

// ── the APU ────────────────────────────────────────────────────────────────────────────────────────
typedef struct {
    uint8_t  duty, vol, const_vol, halt, env_start, env_div, env_vol;
    uint8_t  sweep_en, sweep_per, sweep_neg, sweep_shift, sweep_div, sweep_reload;
    uint16_t timer, timer_cnt;
    uint8_t  seq, len, on;
} nes_pulse_t;
typedef struct {
    uint8_t  ctrl, lin_reload, lin_cnt, lin_flag, len, seq, on;
    uint16_t timer, timer_cnt;
} nes_tri_t;
typedef struct {
    uint8_t  vol, const_vol, halt, env_start, env_div, env_vol, mode, len, on;
    uint16_t period, timer_cnt, lfsr;
} nes_noise_t;
typedef struct {
    nes_pulse_t p[2];
    nes_tri_t   t;
    nes_noise_t n;
    uint8_t     dmc;                            // the DMC's 7-bit output level ($4011): sample players write it
    uint8_t     five_step;
    uint32_t    acc;                            // CPU cycles owed to the channels, 16.16
} nes_apu_t;

static const uint8_t LEN[32] = { 10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
                                 12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30 };
static const uint16_t NOISE_PER[16] = { 4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068 };
static const uint8_t DUTY[4] = { 0x40, 0x60, 0x78, 0x9F };      // the 8-step sequences, MSB first
static const uint8_t TRI[32] = { 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
                                 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

typedef struct {
    agnes_t      *a;
    FILE         *fp;
    uint32_t      rom_bytes;
    uint32_t      rom_base;                     // where the PRG data starts in the file: page 0
    uint8_t      *rom_all;
    uint8_t      *pg[PG_SLOTS];
    uint32_t      pg_tag[PG_SLOTS], pg_age[PG_SLOTS], pg_clock;
    int           pg_n;
    int8_t        hint[PG_HINT];
    uint32_t      cur_tag;
    uint8_t      *cur_buf;
    uint32_t      misses;

    nucleo_nes_line_fn on_line;
    void         *user;
    uint16_t      pal[64];                      // the NES palette, RGB565 in the panel's byte order

    nes_apu_t     apu;
    int16_t       pcm[APU_SPF];
    emu_afilt_t   afilt;
    uint16_t      mix_pulse[31], mix_tnd[203];  // the NES mixer's two curves, as 16-bit levels
    bool          sound;
    bool          frameskip;
    int64_t       due;                          // frame-skip: when this frame should start (0 = not yet known)
    uint8_t       skipped;                      // ...unseen frames in a row

    bool          battery;                      // the header says the PRG RAM keeps a save
    uint32_t      sav_sum;                      // checksum of what is on the card (autosave on change)
    char          save_base[300];
    char          title[48];
    uint8_t       mapper;
    size_t        heap_bytes;
    uint32_t      frames, us_cpu, us_audio;
    uint64_t      prof_apu;                     // cycles in the APU and the speaker (agnes_profile has the rest)
    uint32_t      prof_frames, prof_drawn;
    char          trace_path[96];               // "<card>/nesemu_trace.txt": the profile line on close
    uint32_t      clock;
} nes_session_t;

static nes_session_t *S;

// ── the APU ────────────────────────────────────────────────────────────────────────────────────────
static void env_clock(uint8_t *start, uint8_t *div, uint8_t *v, uint8_t period, uint8_t loop)
{
    if (*start) { *start = 0; *v = 15; *div = period; return; }
    if (*div) { (*div)--; return; }
    *div = period;
    if (*v) (*v)--; else if (loop) *v = 15;
}
static uint16_t sweep_target(const nes_pulse_t *p, int ch)
{
    uint16_t c = p->timer >> p->sweep_shift;
    if (!p->sweep_neg) return (uint16_t)(p->timer + c);
    return (uint16_t)(p->timer - c - (ch == 0));    // pulse 1 negates with one's complement
}
static void apu_quarter(nes_apu_t *a)
{
    for (int i = 0; i < 2; i++) env_clock(&a->p[i].env_start, &a->p[i].env_div, &a->p[i].env_vol, a->p[i].vol, a->p[i].halt);
    env_clock(&a->n.env_start, &a->n.env_div, &a->n.env_vol, a->n.vol, a->n.halt);
    if (a->t.lin_flag) a->t.lin_cnt = a->t.lin_reload; else if (a->t.lin_cnt) a->t.lin_cnt--;
    if (!(a->t.ctrl & 0x80)) a->t.lin_flag = 0;
}
static void apu_half(nes_apu_t *a)
{
    for (int i = 0; i < 2; i++) {
        nes_pulse_t *p = &a->p[i];
        if (!p->halt && p->len) p->len--;
        uint16_t tgt = sweep_target(p, i);
        if (!p->sweep_div && p->sweep_en && p->sweep_shift && p->timer >= 8 && tgt <= 0x7FF) p->timer = tgt;
        if (!p->sweep_div || p->sweep_reload) { p->sweep_div = p->sweep_per; p->sweep_reload = 0; } else p->sweep_div--;
    }
    if (!(a->t.ctrl & 0x80) && a->t.len) a->t.len--;
    if (!a->n.halt && a->n.len) a->n.len--;
}

static void apu_write(void *host, uint16_t addr, uint8_t v)
{
    nes_session_t *s = (nes_session_t *)host;
    nes_apu_t *a = &s->apu;
    if (addr < 0x4008) {
        nes_pulse_t *p = &a->p[(addr >> 2) & 1];
        switch (addr & 3) {
            case 0: p->duty = v >> 6; p->halt = (v >> 5) & 1; p->const_vol = (v >> 4) & 1; p->vol = v & 15; break;
            case 1: p->sweep_en = v >> 7; p->sweep_per = (v >> 4) & 7; p->sweep_neg = (v >> 3) & 1; p->sweep_shift = v & 7;
                    p->sweep_reload = 1; break;
            case 2: p->timer = (uint16_t)((p->timer & 0x700) | v); break;
            case 3: p->timer = (uint16_t)((p->timer & 0xFF) | (v & 7) << 8);
                    if (p->on) p->len = LEN[v >> 3];
                    p->seq = 0; p->env_start = 1; break;
        }
    } else if (addr < 0x400C) {
        nes_tri_t *t = &a->t;
        switch (addr & 3) {
            case 0: t->ctrl = v; t->lin_reload = v & 0x7F; break;
            case 2: t->timer = (uint16_t)((t->timer & 0x700) | v); break;
            case 3: t->timer = (uint16_t)((t->timer & 0xFF) | (v & 7) << 8);
                    if (t->on) t->len = LEN[v >> 3];
                    t->lin_flag = 1; break;
        }
    } else if (addr < 0x4010) {
        nes_noise_t *n = &a->n;
        switch (addr & 3) {
            case 0: n->halt = (v >> 5) & 1; n->const_vol = (v >> 4) & 1; n->vol = v & 15; break;
            case 2: n->mode = v >> 7; n->period = NOISE_PER[v & 15]; break;
            case 3: if (n->on) n->len = LEN[v >> 3]; n->env_start = 1; break;
        }
    } else if (addr == 0x4011) {
        a->dmc = v & 0x7F;
    } else if (addr == 0x4015) {
        a->p[0].on = v & 1; a->p[1].on = (v >> 1) & 1; a->t.on = (v >> 2) & 1; a->n.on = (v >> 3) & 1;
        if (!a->p[0].on) a->p[0].len = 0;
        if (!a->p[1].on) a->p[1].len = 0;
        if (!a->t.on) a->t.len = 0;
        if (!a->n.on) a->n.len = 0;
    } else if (addr == 0x4017) {
        a->five_step = v >> 7;
        if (a->five_step) { apu_quarter(a); apu_half(a); }
    }
}
static uint8_t apu_read(void *host)
{
    nes_apu_t *a = &((nes_session_t *)host)->apu;
    return (uint8_t)((a->p[0].len > 0) | (a->p[1].len > 0) << 1 | (a->t.len > 0) << 2 | (a->n.len > 0) << 3);
}

// One frame of sound. The registers are those the game left during the frame (most games write them once,
// in their NMI); the frame sequencer runs its four quarter steps across the frame, as on the console.
static void apu_render(nes_session_t *s)
{
    nes_apu_t *a = &s->apu;
    const uint32_t step = (uint32_t)(((uint64_t)CPU_HZ << 16) / APU_RATE);   // CPU cycles per sample, 16.16
    for (int i = 0; i < APU_SPF; i++) {
        const int q = i * 4 / APU_SPF;
        if (i == q * APU_SPF / 4 && !(a->five_step && q == 3)) { apu_quarter(a); if (q & 1) apu_half(a); }
        a->acc += step;
        const int cyc = (int)(a->acc >> 16);
        a->acc &= 0xFFFF;
        int out_p = 0;
        for (int c = 0; c < 2; c++) {
            nes_pulse_t *p = &a->p[c];
            // The pulse timer counts APU cycles (half the CPU's): the sequencer steps every (timer + 1) of them.
            int t = p->timer_cnt - cyc;
            while (t < 0) { t += (p->timer + 1) * 2; p->seq = (p->seq + 1) & 7; }
            p->timer_cnt = (uint16_t)t;
            const bool mute = !p->len || p->timer < 8 || (!p->sweep_neg && sweep_target(p, c) > 0x7FF);
            if (!mute && ((DUTY[p->duty] << p->seq) & 0x80)) out_p += p->const_vol ? p->vol : p->env_vol;
        }
        nes_tri_t *t = &a->t;
        if (t->len && t->lin_cnt && t->timer > 2) {     // an ultrasonic period is silenced, not aliased
            int tc = t->timer_cnt - cyc;
            while (tc < 0) { tc += t->timer + 1; t->seq = (t->seq + 1) & 31; }
            t->timer_cnt = (uint16_t)tc;
        }
        nes_noise_t *n = &a->n;
        int nc = n->timer_cnt - cyc;
        while (nc < 0) {
            nc += n->period ? n->period : 4;
            uint16_t fb = (n->lfsr ^ (n->lfsr >> (n->mode ? 6 : 1))) & 1;
            n->lfsr = (uint16_t)((n->lfsr >> 1) | fb << 14);
        }
        n->timer_cnt = (uint16_t)nc;
        const int out_n = (n->len && !(n->lfsr & 1)) ? (n->const_vol ? n->vol : n->env_vol) : 0;
        const int tnd = 3 * TRI[t->seq] + 2 * out_n + a->dmc;
        s->pcm[i] = (int16_t)(s->mix_pulse[out_p] + s->mix_tnd[tnd > 202 ? 202 : tnd] - 16384);
    }
}

static void mixer_init(nes_session_t *s)
{
    // nesdev "APU Mixer": pulse = 95.88 / (8128 / n + 100), tnd ~= 163.67 / (24329 / n + 100); full scale ~1.0.
    for (int n = 0; n < 31; n++) s->mix_pulse[n] = n ? (uint16_t)(95.88f / (8128.0f / n + 100) * 26000) : 0;
    for (int n = 0; n < 203; n++) s->mix_tnd[n] = n ? (uint16_t)(163.67f / (24329.0f / n + 100) * 26000) : 0;
}

// ── the cartridge, paged ─────────────────────────────────────────────────────────────────────────────
static const uint8_t PG_OPEN[PG_SIZE] = { [0 ... PG_SIZE - 1] = 0xFF };   // past the end of the file

// The page holding file offset `off` (pages counted from rom_base), from the cache or the card. A miss evicts the
// least recently used page, so agnes's direct pointers (patch A5) are dropped with it.
static const uint8_t *rom_page(void *host, uint32_t off)
{
    nes_session_t *s = (nes_session_t *)host;
    if (off < s->rom_base || off >= s->rom_bytes) return PG_OPEN;
    const uint32_t tag = (off - s->rom_base) >> PG_BITS;
    if (s->rom_all) {
        const uint32_t at = s->rom_base + (tag << PG_BITS);
        return at + PG_SIZE <= s->rom_bytes ? s->rom_all + at : PG_OPEN;   // a truncated file's last page: open bus
    }
    if (tag == s->cur_tag) return s->cur_buf;
    int h = (int)(tag & (PG_HINT - 1)), g = s->hint[h];
    if (g < 0 || g >= s->pg_n || s->pg_tag[g] != tag) {
        g = -1;
        for (int i = 0; i < s->pg_n; i++) if (s->pg_tag[i] == tag) { g = i; break; }
        if (g < 0) {                                 // a miss: the least recently used page takes it
            int v = 0;
            for (int i = 1; i < s->pg_n; i++) if (s->pg_age[i] < s->pg_age[v]) v = i;
            size_t got = 0;
            if (fseek(s->fp, (long)(s->rom_base + (tag << PG_BITS)), SEEK_SET) == 0) got = fread(s->pg[v], 1, PG_SIZE, s->fp);
            if (got < PG_SIZE) memset(s->pg[v] + got, 0xFF, PG_SIZE - got);
            s->pg_tag[v] = tag;
            s->misses++;
            g = v;
            agnes_drop_windows(s->a);                // one of them may have pointed at the page just replaced
        }
        s->hint[h] = (int8_t)g;
    }
    s->pg_age[g] = ++s->pg_clock;
    s->cur_tag = tag; s->cur_buf = s->pg[g];
    return s->cur_buf;
}
static uint8_t rom_read(void *host, uint32_t off)
{
    nes_session_t *s = (nes_session_t *)host;
    if (off < s->rom_base) return s->rom_all ? s->rom_all[off] : 0xFF;   // the header: agnes never reads it here
    return rom_page(host, off)[(off - s->rom_base) & (PG_SIZE - 1)];
}

// agnes already colours the line with s->pal (patch A2): straight to the front-end.
static void on_line(void *host, const uint16_t *line, int y)
{
    nes_session_t *s = (nes_session_t *)host;
    if (s->on_line) s->on_line(line, y, s->user);
}

// ── files: "<root>/Saves/nes/<rom name>.*" ─────────────────────────────────────────────────────────
static void mkdir_one(const char *p)
{
#ifdef _WIN32
    mkdir(p);
#else
    mkdir(p, 0775);
#endif
}
static void save_base_init(nes_session_t *s, const char *path)
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

// The PRG RAM agnes keeps inside its mapper (MMC1, MMC3): what a battery saves.
static uint8_t *prg_ram(nes_session_t *s)
{
    switch (s->a->gamepack.mapper) {
        case 1: return s->a->mapper.m1.prg_ram;
        case 4: return s->a->mapper.m4.prg_ram;
        default: return NULL;
    }
}
static uint32_t sum8k(const uint8_t *p) { uint32_t h = 2166136261u; for (int i = 0; i < 8192; i++) h = (h ^ p[i]) * 16777619u; return h; }
static void sav_load(nes_session_t *s)
{
    uint8_t *r = prg_ram(s);
    if (!r || !s->battery) return;
    char p[320]; snprintf(p, sizeof p, "%s.sav", s->save_base);
    FILE *f = fopen(p, "rb");
    if (f) { if (fread(r, 1, 8192, f) != 8192) ESP_LOGW(TAG, "short save %s", p); fclose(f); }
    s->sav_sum = sum8k(r);
}

void nucleo_nes_save(void)
{
    nes_session_t *s = S;
    if (!s || !s->battery) return;
    uint8_t *r = prg_ram(s);
    if (!r) return;
    uint32_t h = sum8k(r);
    if (h == s->sav_sum) return;
    char p[320], t[330];
    snprintf(p, sizeof p, "%s.sav", s->save_base);
    snprintf(t, sizeof t, "%s.tmp", p);
    FILE *f = fopen(t, "wb");
    if (!f) return;
    bool ok = fwrite(r, 1, 8192, f) == 8192;
    ok = fclose(f) == 0 && ok;
    if (ok) { remove(p); ok = rename(t, p) == 0; }
    if (ok) s->sav_sum = h; else remove(t);
}
// Every ~2 s while the game runs: cheap (one pass over 8 KB), and only a change reaches the card.
bool nucleo_nes_autosave(void)
{
    if (!S || !S->battery || S->clock % 120) return false;
    uint32_t before = S->sav_sum;
    nucleo_nes_save();
    return S->sav_sum != before;
}

// ── open / close ───────────────────────────────────────────────────────────────────────────────────
static void session_free(nes_session_t *s)
{
    if (!s) return;
    if (s->sound) { s->sound = false; nucleo_audio_pcm_close(); }
    if (s->fp) fclose(s->fp);
    for (int i = 0; i < s->pg_n; i++) free(s->pg[i]);
    free(s->rom_all);
    agnes_destroy(s->a);                          // with the mapper's RAM blocks (patch A8)
    free(s);
}

esp_err_t nucleo_nes_probe(const char *rom_path, int *mapper)
{
    FILE *f = fopen(rom_path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    uint8_t h[16];
    size_t got = fread(h, 1, sizeof h, f);
    fclose(f);
    if (got != sizeof h || memcmp(h, "NES\x1A", 4)) return ESP_ERR_INVALID_SIZE;
    int m = (h[6] >> 4) | (h[7] & 0xF0);
    if (mapper) *mapper = m;
    return (m == 0 || m == 1 || m == 2 || m == 4) ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nucleo_nes_open(const char *rom_path, nucleo_nes_line_fn line_cb, void *user)
{
    nucleo_nes_close();
    int mapper = 0;
    esp_err_t pe = nucleo_nes_probe(rom_path, &mapper);
    if (pe != ESP_OK) return pe;
    // The console first: ~23 KB in one block, before anything could carve the heap's big block up.
    agnes_t *a = agnes_make();
    if (!a) return ESP_ERR_NO_MEM;
    nes_session_t *s = (nes_session_t *)calloc(1, sizeof *s);
    if (!s) { agnes_destroy(a); return ESP_ERR_NO_MEM; }
    s->a = a; s->heap_bytes = sizeof *s + sizeof *a;
    s->on_line = line_cb; s->user = user; s->mapper = (uint8_t)mapper;
    s->cur_tag = PG_EMPTY;
    for (int i = 0; i < PG_SLOTS; i++) s->pg_tag[i] = PG_EMPTY;
    memset(s->hint, -1, sizeof s->hint);
    save_base_init(s, rom_path);
    const char *roms = strstr(rom_path, "/data/ROMs/");
    if (roms) snprintf(s->trace_path, sizeof s->trace_path, "%.*s/nesemu_trace.txt", (int)(roms - rom_path), rom_path);
    { uint64_t x, y, z; agnes_profile(&x, &y, &z); }  // a fresh count for this game

    s->fp = fopen(rom_path, "rb");
    if (!s->fp) { session_free(s); return ESP_ERR_NOT_FOUND; }
    fseek(s->fp, 0, SEEK_END);
    long sz = ftell(s->fp);
    fseek(s->fp, 0, SEEK_SET);
    uint8_t hdr[16];
    if (sz < 16 + 16384 || fread(hdr, 1, 16, s->fp) != 16) { session_free(s); return ESP_ERR_INVALID_SIZE; }
    s->rom_bytes = (uint32_t)sz;
    s->battery = (hdr[6] >> 1) & 1;

    // The mapper's RAM blocks next, while the heap still has them: 8 KB of CHR RAM when the cartridge has no CHR
    // ROM, 8 KB of PRG RAM for MMC1 / MMC3 (patch A8; agnes_load_ines_data keeps what is already there).
    a->gamepack.mapper = (unsigned char)mapper;
    {
        uint8_t **chr, **prg;
        agnes_rams(a, &chr, &prg);
        if (chr && hdr[5] == 0 && !(*chr = (uint8_t *)calloc(1, 8192))) { session_free(s); return ESP_ERR_NO_MEM; }
        if (prg && !(*prg = (uint8_t *)calloc(1, 8192))) { session_free(s); return ESP_ERR_NO_MEM; }
    }

    // Sound before the cache, so its DMA buffers are paid for first.
    s->sound = nucleo_audio_pcm_open_ex(APU_RATE, 1, EMU_AUDIO_QUEUE_FRAMES * APU_SPF) == ESP_OK;
    if (!s->sound) ESP_LOGW(TAG, "speaker unavailable — playing silently");

    // The whole cartridge when it fits with room to spare; else the page cache.
    if ((size_t)sz + 24 * 1024 <= heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)) {
        s->rom_all = (uint8_t *)malloc((size_t)sz);
        if (s->rom_all && (fseek(s->fp, 0, SEEK_SET) != 0 || fread(s->rom_all, 1, (size_t)sz, s->fp) != (size_t)sz)) {
            free(s->rom_all); s->rom_all = NULL;
        }
        if (s->rom_all) { s->heap_bytes += (size_t)sz; fclose(s->fp); s->fp = NULL; }
    }
    if (!s->rom_all) {
        uint32_t pages = (s->rom_bytes + PG_SIZE - 1) / PG_SIZE;
        for (int i = 0; i < PG_SLOTS && (uint32_t)i < pages; i++) {
            if (heap_caps_get_free_size(MALLOC_CAP_DEFAULT) < PG_SIZE + KEEP) break;
            uint8_t *b = (uint8_t *)malloc(PG_SIZE);
            if (!b) break;
            s->pg[s->pg_n++] = b;
            s->heap_bytes += PG_SIZE;
        }
        if (s->pg_n < PG_MIN) { ESP_LOGE(TAG, "only %d cartridge pages fit", s->pg_n); session_free(s); return ESP_ERR_NO_MEM; }
    }

    a->rom_read = rom_read; a->rom_page = rom_page; a->on_line = on_line; a->apu_write = apu_write; a->apu_read = apu_read;
    a->host = s;
    s->rom_base = (hdr[6] & 4) ? 16 + 512 : 16;      // the PRG data: after the header and any trainer
    // agnes reads only the header through `data` (patch A1); the size it checks against is the file's.
    if (!agnes_load_ines_data(a, hdr, (size_t)sz)) { session_free(s); return ESP_ERR_NOT_SUPPORTED; }
    if (a->gamepack.prg_rom_offset != s->rom_base) { session_free(s); return ESP_ERR_NOT_SUPPORTED; }
    { uint8_t **chr, **prg; agnes_rams(a, &chr, &prg);
      s->heap_bytes += (chr && *chr ? 8192 : 0) + (prg && *prg ? 8192 : 0); }

    const agnes_color_t *pc = agnes_palette();
    for (int i = 0; i < 64; i++) {
        uint16_t c = (uint16_t)(((pc[i].r & 0xF8) << 8) | ((pc[i].g & 0xFC) << 3) | (pc[i].b >> 3));
        s->pal[i] = (uint16_t)((c >> 8) | (c << 8));
    }
    a->rgb = s->pal;
    mixer_init(s);
    s->apu.n.lfsr = 1;
    S = s;
    sav_load(s);
    ESP_LOGI(TAG, "'%s' mapper %d, %u KB, %s, heap %u B, sound %s", s->title, mapper, (unsigned)(sz / 1024),
             s->rom_all ? "resident" : "paged", (unsigned)s->heap_bytes, s->sound ? "on" : "off");
    return ESP_OK;
}

// Where the frames' time went, once per game, appended to the emulator's trace: tenths of a millisecond per frame
// at 240 MHz, from the CPU's cycle counter (integers only: no double helpers in a module).
static void profile_write(nes_session_t *s)
{
    uint64_t cpu, ppu, line;
    agnes_profile(&cpu, &ppu, &line);
    if (!s->prof_frames || !s->trace_path[0] || (!cpu && !ppu)) return;
    // In 1024-cycle units (a shift, not a 64-bit division: libgcc's is not in the module ABI), averaged per frame,
    // then tenths of a millisecond at 240 MHz: k * 1024 / 24000 = k * 16 / 375.
    const uint32_t n = s->prof_frames;
    const unsigned c = (unsigned)((uint32_t)(cpu >> 10) / n * 16 / 375), p = (unsigned)((uint32_t)((ppu - line) >> 10) / n * 16 / 375),
                   l = (unsigned)((uint32_t)(line >> 10) / n * 16 / 375), a = (unsigned)((uint32_t)(s->prof_apu >> 10) / n * 16 / 375);
    FILE *f = fopen(s->trace_path, "a");
    if (!f) return;
    fprintf(f, "  nes profile: %u frames (%u drawn), ms/frame: 6502 %u.%u  ppu %u.%u  panel lines %u.%u  apu %u.%u\n",
            (unsigned)s->prof_frames, (unsigned)s->prof_drawn, c / 10, c % 10, p / 10, p % 10, l / 10, l % 10, a / 10, a % 10);
    fclose(f);
}

void nucleo_nes_close(void)
{
    if (!S) return;
    profile_write(S);
    nucleo_nes_save();
    nes_session_t *s = S;
    S = NULL;
    session_free(s);
}

void nucleo_nes_reset(void)
{
    if (!S) return;
    nucleo_nes_save();
    agnes_t *a = S->a;
    cpu_init(&a->cpu, a);                         // the console restarts from its reset vector; the cartridge stays
    ppu_init(&a->ppu, a);
    memset(&S->apu, 0, sizeof S->apu);
    S->apu.n.lfsr = 1;
}

void nucleo_nes_run_frame(void)
{
    nes_session_t *s = S;
    if (!s) return;
    int64_t t0 = esp_timer_get_time();
    bool skip = false;
    if (s->frameskip) {
        const int64_t now = esp_timer_get_time();
        if (!s->due || now - s->due > 100000) s->due = now;          // the first frame, or far behind: start over
        skip = now - s->due > 2000 && s->skipped < 3;                // more than 2 ms late: catch up unseen
        s->skipped = skip ? (uint8_t)(s->skipped + 1) : 0;
        s->due += 16639;                                              // 60.0988 Hz
    }
    s->a->skip_draw = skip;
    agnes_next_frame(s->a);
    int64_t t1 = esp_timer_get_time();
    s->us_cpu += (uint32_t)(t1 - t0);
    const uint32_t c0 = agnes_ccount();
    apu_render(s);
    if (s->sound) {
        emu_afilt_run(&s->afilt, s->pcm, APU_SPF, 1, 1);
        nucleo_audio_pcm_write(s->pcm, sizeof s->pcm);
        s->us_audio += (uint32_t)(esp_timer_get_time() - t1);
    }
    s->prof_apu += (uint32_t)(agnes_ccount() - c0);
    s->prof_frames++;
    if (!s->a->skip_draw) s->prof_drawn++;
    s->frames++;
    s->clock++;
}

void nucleo_nes_set_buttons(uint8_t m)
{
    if (!S) return;
    agnes_input_t in = { (m & NUCLEO_GB_A) != 0, (m & NUCLEO_GB_B) != 0, (m & NUCLEO_GB_SELECT) != 0, (m & NUCLEO_GB_START) != 0,
                         (m & NUCLEO_GB_UP) != 0, (m & NUCLEO_GB_DOWN) != 0, (m & NUCLEO_GB_LEFT) != 0, (m & NUCLEO_GB_RIGHT) != 0 };
    agnes_set_input(S->a, &in, NULL);
}

const char *nucleo_nes_title(void) { return S ? S->title : ""; }
// The front-end's relief lever. On, a frame is drawn only when the console is on time: a frame that starts late
// (the previous ones overran, a busy panel, a page from the card) is run unseen — agnes then resolves only the
// sprite-0 hit per line and the panel gets nothing — so the console itself stays at 60 frames a second and the
// sound never runs dry. Never more than three unseen in a row: the picture keeps moving at 15 fps at worst.
// Off, every frame is drawn.
void nucleo_nes_set_frameskip(bool on) { if (S) { S->frameskip = on; S->due = 0; S->skipped = 0; } }
void nucleo_nes_geometry(int *w, int *h) { *w = NUCLEO_NES_W; *h = NUCLEO_NES_H; }

// ── save states: "NNS2", the console (agnes_t), the APU, then the mapper's CHR / PRG RAM if it has them ──
#define STATE_MAGIC 0x32534E4Eu
static long state_bytes(void)
{
    uint8_t **chr, **prg;
    agnes_rams(S->a, &chr, &prg);
    return (long)(4 + sizeof(agnes_t) + sizeof(nes_apu_t)) + (chr && *chr ? 8192 : 0) + (prg && *prg ? 8192 : 0);
}
static void state_path(char *out, size_t n, int slot) { snprintf(out, n, "%s.st%d", S->save_base, slot < 0 ? 0 : slot > 9 ? 9 : slot); }

bool nucleo_nes_state_exists(int slot)
{
    if (!S) return false;
    char p[320]; state_path(p, sizeof p, slot);
    struct stat sb;
    return stat(p, &sb) == 0 && sb.st_size == state_bytes();
}

esp_err_t nucleo_nes_state_save(int slot)
{
    if (!S) return ESP_ERR_INVALID_STATE;
    char p[320]; state_path(p, sizeof p, slot);
    FILE *f = fopen(p, "wb");
    if (!f) return ESP_FAIL;
    uint32_t magic = STATE_MAGIC;
    bool ok = fwrite(&magic, 1, 4, f) == 4 && fwrite(S->a, 1, sizeof(agnes_t), f) == sizeof(agnes_t)
           && fwrite(&S->apu, 1, sizeof S->apu, f) == sizeof S->apu;
    uint8_t **chr, **prg;
    agnes_rams(S->a, &chr, &prg);
    if (ok && chr && *chr) ok = fwrite(*chr, 1, 8192, f) == 8192;
    if (ok && prg && *prg) ok = fwrite(*prg, 1, 8192, f) == 8192;
    ok = fclose(f) == 0 && ok;
    if (!ok) { remove(p); return ESP_FAIL; }
    return ESP_OK;
}

// Straight into the live console (no second 23 KB block exists), then every pointer comes back from this
// session — agnes_restore_state's own fix-ups, plus the host's hooks it keeps (patch A1-A3).
esp_err_t nucleo_nes_state_load(int slot)
{
    if (!S) return ESP_ERR_INVALID_STATE;
    if (!nucleo_nes_state_exists(slot)) return ESP_ERR_NOT_FOUND;
    char p[320]; state_path(p, sizeof p, slot);
    FILE *f = fopen(p, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    agnes_t *a = S->a;
    uint8_t (*rr)(void *, uint32_t) = a->rom_read;
    void (*ol)(void *, const uint16_t *, int) = a->on_line;
    void (*aw)(void *, uint16_t, uint8_t) = a->apu_write;
    uint8_t (*ar)(void *) = a->apu_read;
    uint8_t **chr, **prg, *chr_p = NULL, *prg_p = NULL;         // this session's RAM blocks, not the file's pointers
    agnes_rams(a, &chr, &prg);
    if (chr) chr_p = *chr;
    if (prg) prg_p = *prg;
    uint32_t magic = 0;
    bool ok = fread(&magic, 1, 4, f) == 4 && magic == STATE_MAGIC
           && fread(a, 1, sizeof(agnes_t), f) == sizeof(agnes_t)
           && fread(&S->apu, 1, sizeof S->apu, f) == sizeof S->apu;
    agnes_rams(a, &chr, &prg);
    if (chr) *chr = chr_p;
    if (prg) *prg = prg_p;
    if (ok && chr_p) ok = fread(chr_p, 1, 8192, f) == 8192;
    if (ok && prg_p) ok = fread(prg_p, 1, 8192, f) == 8192;
    fclose(f);
    a->rom_read = rr; a->on_line = ol; a->apu_write = aw; a->apu_read = ar; a->host = S;
    a->rom_page = rom_page;
    a->rgb = S->pal;
    agnes_drop_windows(a);
    a->gamepack.data = NULL;
    a->cpu.agnes = a; a->ppu.agnes = a;
    switch (a->gamepack.mapper) {
        case 0: a->mapper.m0.agnes = a; break;
        case 1: a->mapper.m1.agnes = a; break;
        case 2: a->mapper.m2.agnes = a; break;
        case 4: a->mapper.m4.agnes = a; break;
    }
    if (!ok) { nucleo_nes_reset(); return ESP_ERR_INVALID_CRC; }
    return ESP_OK;
}

void nucleo_nes_get_stats(nucleo_gb_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    nes_session_t *s = S;
    if (!s) return;
    out->heap_bytes = s->heap_bytes;
    out->rom_bytes = s->rom_bytes;
    out->rom_resident = s->rom_all != NULL;
    out->rom_paged = s->rom_all == NULL;
    out->rom_pages = s->pg_n;
    out->bank_misses = s->misses;
    out->frames = s->frames;
    out->us_cpu = s->us_cpu;
    out->us_audio = s->us_audio;
    out->cart_type = s->mapper;
    out->save_dirty = s->battery && prg_ram(s) && sum8k(prg_ram(s)) != s->sav_sum;
}

void nucleo_nes_reset_counters(void)
{
    if (!S) return;
    S->us_cpu = S->us_audio = 0;
    S->misses = 0;
    S->frames = 0;
}
