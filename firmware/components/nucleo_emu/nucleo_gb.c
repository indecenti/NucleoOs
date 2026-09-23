// nucleo_gb — see include/nucleo_gb.h for why a Game Boy fits on this board at all.
//
// This file is the HOST half of Peanut-GB: the core asks for cartridge bytes, hands back finished
// scanlines, and reports errors; everything about where a ROM lives and how it reaches the panel is
// decided here. The core is vendored from upstream plus a short list of marked accuracy patches
// (vendor/README.md); everything else about a cartridge is decided here, not there.
#include "nucleo_gb.h"
#include "nucleo_board.h"
#include "nucleo_audio.h"   // raw PCM sink: the APU produces its own samples, we hand them to I2S
#include "esp_log.h"
#include "esp_attr.h"       // IRAM_ATTR: keep the innermost callbacks out of the flash cache
#include "esp_heap_caps.h"
#include "esp_timer.h"      // where a frame's time actually goes
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>   // save-state presence check
#include <time.h>       // MBC3 real-time clock seeded from the system clock

// Peanut-GB build switches — set BEFORE the include, they are compile-time for the whole core.
#define ENABLE_LCD   1
#define ENABLE_SOUND 1     // Peanut-GB ships no APU: it calls audio_read/audio_write, which we route
                           // to the vendored minigb_apu (MIT, context-based, ~2.2 KB per frame).
// ON. It reproduces the DMG's sprite priority rule (lower X wins where sprites overlap, then OAM
// order) instead of drawing in raw OAM order. It was off to save the per-line sort, and dmg-acid2
// showed the price: 40 wrong pixels exactly where sprites overlap — the flicker-and-swap artefacts
// players read as "the emulator draws the wrong thing on top". The sort is an insertion sort over
// at most ten entries per line; the jump-table dispatch (CMakeLists.txt) buys it back many times over.
#define PEANUT_GB_HIGH_LCD_ACCURACY 1
// OFF. It tags every emitted pixel with which palette produced it (OBJ0/OBJ1/BG) so a front-end
// can apply the different colour palettes a Game Boy Color gives a DMG game. This is a DMG core on a
// four-shade screen and our scanline callback masks the tag straight off again — so it is an extra OR
// per pixel, 23,000 a frame, plus masking through the sprite path, for information nobody reads.
#define PEANUT_GB_12_COLOUR 0

#ifndef MINIGB_APU_AUDIO_FORMAT_S16SYS
# define MINIGB_APU_AUDIO_FORMAT_S16SYS 1   // 16-bit signed: exactly what the I2S sink takes
#endif
#include "../vendor/minigb_apu.h"
// Peanut-GB reaches the APU through two BARE function names, so these shims are the bridge from its
// global-style contract to our per-session context. They must be declared before peanut_gb.h is
// included, and defined against the live session (S) below.
static uint8_t audio_read(const uint16_t addr);
static void    audio_write(const uint16_t addr, const uint8_t val);
#include "../vendor/peanut_gb.h"

static const char *TAG = "gb";

#define CART_RAM_MAX   (128 * 1024)     // largest battery RAM a cartridge can declare (MBC5, 16 x 8 KB)
// Battery RAM lives in 8 KB BANKS — the cartridge's own bank size — and a cartridge with more than
// RAM_RESIDENT of them keeps only that many in RAM; the rest wait in "<rom>.sav.swp" on the card.
//
// Why: in the emulator's Solo boot the heap has ~90 KB free and its largest block is 32 KB (device
// trace). Pokémon Red/Blue/Yellow/Gold/Silver carry 32 KB of SRAM. As one block it could not be
// allocated at all (the old code asked for it AFTER the greedy ROM cache: "not enough RAM", the game
// never started); as four resident banks it opens but starves the ROM cache to ~29 pages, and the
// host gate measures 3.9 SD reads per frame on a big cartridge — unplayable. Two resident banks give
// the cache 16 KB back. The games this matters for use SRAM in a narrow pattern: Pokémon decompresses
// sprites in bank 0 all the time, writes its save to bank 1, and touches banks 2-3 only when the
// player switches PC box — so a swap is an event (a box change), not a per-frame cost.
#define RAM_BANK       8192
#define RAM_BANKS      (CART_RAM_MAX / RAM_BANK)
#define RAM_RESIDENT   2

// ROM page cache. A cartridge rarely fits in RAM here (a 128 KB ROM would need 152 KB contiguous on
// a board whose largest block is 32 KB in the Solo boot), so the ROM is served from the SD card and this cache is
// what stands between the emulator and a 2-4 ms card read on the critical path.
//
// SMALL PAGES, MANY OF THEM. That is the whole design, and it was measured, not guessed — the host
// gate (tools/emu-host/gb_cache_test.c) sweeps the geometry against real cartridges. At a FIXED 40 KB
// budget, misses per frame on The Legend of Zelda / Metroid II / Kirby's Block Ball:
//
//     8 KB x  5 slots     6.0   5.5  12.9      <- unusable
//     4 KB x 10 slots     6.4   3.7   4.2      <- the first design; ~19 ms/frame of SD on Zelda
//     2 KB x 20 slots     0.1   0.4   3.0
//     1 KB x 40 slots     0.1   0.1   0.2      <- chosen
//
// Same RAM, sixty times fewer reads. A game's working set is SCATTERED — a few hundred live bytes in
// each of many places — so a big page spends most of its bulk on bytes nobody asked for, and a budget
// divided into few large pages covers few regions. Divide it finely and it covers many. Going finer
// still (512 B) buys nothing and doubles the bookkeeping; 1 KB is also two whole SD sectors, so a
// refill is one aligned FATFS read.
//
// Fully associative with LRU. Direct mapping would collide bank 0 — the vectors and the main loop,
// live every single frame — against every bank whose number shares its low bits.
#ifndef PG_BITS
# define PG_BITS  10                    // 1 KB pages
#endif
#ifndef PG_SLOTS
# define PG_SLOTS 96                    // CAP, not a target: open() allocates only what the heap
                                        // gives, and stops at the KEEP floor. 40 was measured right
                                        // for platformers, but Street Fighter II streams ~48-64 KB of
                                        // scattered sprite banks per fight — at 40 KB it thrashed
                                        // (1.1 misses/frame = ~3.4 ms of SD in a 16.7 ms budget); at
                                        // 64+ it drops to 0.1. The emulator runs in a Solo boot with
                                        // Wi-Fi stripped, so the heap really has this to give — the
                                        // fixed 40 was leaving it on the table. Must stay <= 127:
                                        // the hint table stores slot indices in an int8_t.
#endif
#define PG_SIZE   (1u << PG_BITS)
#define PG_MIN    8                     // fewer than this thrashes; refuse rather than crawl
#define PG_EMPTY  0xFFFFFFFFu
// Lookup accelerator. With forty slots a linear scan runs on every page CHANGE, not just every miss,
// and page changes are frequent by design. This is a direct-mapped hint from tag to slot: one probe
// answers almost every lookup, and because the hint is always VERIFIED against the slot's real tag a
// stale or colliding entry costs a fallback scan rather than a wrong byte.
#define PG_HINT   256                   // 256 bytes, indexed by the tag's low bits

// ── session state ───────────────────────────────────────────────────────────────────────────────
// One pointer, heap-allocated on open. Nothing here is static storage: while the app is closed this
// module must cost zero RAM (docs/memory-budget.md).
typedef struct {
    struct gb_s   gb;                   // ~17.2 KB — the whole console
    struct minigb_apu_ctx apu;          // Game Boy sound chip state (context-based, no globals)
    audio_sample_t audio[AUDIO_SAMPLES_TOTAL];   // one frame of stereo samples (~2.2 KB)
    bool          sound;                // false when the speaker could not be claimed

    FILE         *fp;                   // open ROM on the SD card
    uint32_t      rom_bytes;
    uint8_t      *rom_all;              // whole ROM, when it fits (fast path: no SD traffic at all)
    // Page cache, used whenever the whole ROM will not fit. See rom_read for why it is pages and
    // not banks.
    uint8_t      *pg[PG_SLOTS];         // 1 KB each, allocated as many as the heap allows
    uint32_t      pg_tag[PG_SLOTS];     // which 1 KB page each slot holds (PG_EMPTY = none)
    uint32_t      pg_age[PG_SLOTS];     // LRU stamp
    int           pg_n;                 // slots actually allocated
    uint32_t      pg_clock;             // monotonic stamp source
    int8_t        hint[PG_HINT];        // tag -> slot guess, always verified before use
    uint32_t      cur_tag;              // fast path: the page the last read hit...
    uint8_t      *cur_buf;              // ...and its buffer
    uint32_t      bank_misses;          // SD refills since open
    uint32_t      us_cpu, us_audio;     // where a frame's time goes

    // Battery RAM — see RAM_RESIDENT. Slots are the banks held in RAM; bank_slot maps a cartridge bank
    // to its slot, or -1 when it is on the card.
    size_t        cart_ram_bytes;       // 0 = cartridge has no RAM
    uint8_t      *ram_slot[RAM_BANKS];
    int8_t        bank_slot[RAM_BANKS];
    int8_t        slot_bank[RAM_BANKS];
    bool          slot_dirty[RAM_BANKS];
    uint32_t      slot_age[RAM_BANKS];
    uint8_t       ram_nb, ram_ns;       // banks the cartridge has / banks held in RAM
    uint32_t      ram_swaps;            // banks brought in from the card since open
    bool          cart_ram_dirty;
    uint32_t      ram_touch;            // frame of the last cart-RAM change (autosave waits for quiet)
    uint32_t      clock;                // frames since open — never reset, unlike `frames`

    // The four header bytes gb_init() decides from, as WE want it to see them (see header_fix).
    uint8_t       hdr_type, hdr_rom, hdr_ram, hdr_sum;

    uint32_t      core_errors;          // invalid opcodes/reads reported by the core since open/reset
    uint16_t      core_err_addr;
    uint8_t       core_err_kind;

    char          rom_path[300];        // the cartridge; .sav and .stN hang off it
    char          title[17];

    nucleo_gb_line_fn on_line;
    void             *user;

    size_t        heap_bytes;
    uint32_t      frames;
} gb_session_t;

static gb_session_t *S = NULL;

// ── cartridge access ────────────────────────────────────────────────────────────────────────────
// Peanut-GB passes a FLAT offset into the ROM image (it resolves banking itself), so this is a pure
// "give me byte N of the file" service. It is called for EVERY instruction fetch, so the resident
// paths are branch-light and the SD path is the exception, not the rule.
// IRAM. This runs on EVERY instruction fetch and every data read, several million times a second.
// Left in flash it is fetched through a 16 KB instruction cache that the core's own dispatch switch
// is already thrashing, so a fair share of those calls stall on an 80 MHz DIO flash read. It is a
// few hundred bytes; buying them out of the cache is the cheapest speed in the whole emulator. Same
// reasoning for the other four callbacks below.
static IRAM_ATTR uint8_t rom_read(struct gb_s *gb, const uint_fast32_t addr)
{
    // Context comes from the core's own private pointer, NEVER from the module global: gb_init()
    // calls this to read the cartridge header before open() has published S.
    gb_session_t *s = (gb_session_t *)gb->direct.priv;
    if (!s || addr >= s->rom_bytes) return 0xFF;

    if (s->rom_all) return s->rom_all[addr];            // fast path: whole ROM in RAM

    // FAST PATH — one shift and one compare. Consecutive fetches almost always land in the page the
    // previous one did, so this is what the vast majority of reads cost.
    uint32_t tag = (uint32_t)(addr >> PG_BITS);
    if (tag == s->cur_tag) return s->cur_buf[addr & (PG_SIZE - 1)];

    // One probe through the hint table answers almost every page change. The tag comparison is what
    // makes it safe: a stale hint simply fails it and falls through to the scan below.
    int h = (int)(tag & (PG_HINT - 1));
    int g = s->hint[h];
    if (g >= 0 && g < s->pg_n && s->pg_tag[g] == tag) {
        s->pg_age[g] = ++s->pg_clock;
        s->cur_tag = tag; s->cur_buf = s->pg[g];
        return s->pg[g][addr & (PG_SIZE - 1)];
    }

    // Hint missed (stale, or two live tags share the low bits). Scan, and re-point the hint.
    for (int i = 0; i < s->pg_n; i++) {
        if (s->pg_tag[i] == tag) {
            s->pg_age[i] = ++s->pg_clock;
            s->hint[h] = (int8_t)i;
            s->cur_tag = tag; s->cur_buf = s->pg[i];
            return s->pg[i][addr & (PG_SIZE - 1)];
        }
    }

    // Miss: evict the least recently used slot and refill it from the card.
    int v = 0;
    for (int i = 1; i < s->pg_n; i++) if (s->pg_age[i] < s->pg_age[v]) v = i;
    uint32_t off = tag << PG_BITS;
    if (fseek(s->fp, (long)off, SEEK_SET) != 0) return 0xFF;
    size_t want = PG_SIZE;
    if (off + want > s->rom_bytes) want = s->rom_bytes - off;
    if (fread(s->pg[v], 1, want, s->fp) != want) {
        // The slot now holds a half-read page. Drop it, and drop the fast-path shortcut too if it
        // happened to point here — a stale cur_buf would serve that garbage without a tag check.
        s->pg_tag[v] = PG_EMPTY;
        if (s->cur_buf == s->pg[v]) { s->cur_tag = PG_EMPTY; s->cur_buf = NULL; }
        return 0xFF;
    }
    if (want < PG_SIZE) memset(s->pg[v] + want, 0xFF, PG_SIZE - want);
    s->pg_tag[v] = tag;
    s->pg_age[v] = ++s->pg_clock;
    s->hint[h] = (int8_t)v;
    s->bank_misses++;
    s->cur_tag = tag; s->cur_buf = s->pg[v];
    return s->pg[v][addr & (PG_SIZE - 1)];
}

// APU bridge. Peanut-GB calls these for every read/write in the 0xFF10-0xFF3F sound range.
static IRAM_ATTR uint8_t audio_read(const uint16_t addr)
{
    return S ? minigb_apu_audio_read(&S->apu, addr) : 0xFF;
}
static IRAM_ATTR void audio_write(const uint16_t addr, const uint8_t val)
{
    if (S) minigb_apu_audio_write(&S->apu, addr, val);
}

static int ram_bank_in(gb_session_t *s, int bank);
static IRAM_ATTR uint8_t cart_ram_read(struct gb_s *gb, const uint_fast32_t addr)
{
    gb_session_t *s = (gb_session_t *)gb->direct.priv;
    if (!s || addr >= s->cart_ram_bytes) return 0xFF;
    int sl = s->bank_slot[addr >> 13];
    if (sl < 0 && (sl = ram_bank_in(s, (int)(addr >> 13))) < 0) return 0xFF;
    s->slot_age[sl] = s->clock;
    return s->ram_slot[sl][addr & (RAM_BANK - 1)];
}

static IRAM_ATTR void cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val)
{
    gb_session_t *s = (gb_session_t *)gb->direct.priv;
    if (!s || addr >= s->cart_ram_bytes) return;
    int sl = s->bank_slot[addr >> 13];
    if (sl < 0 && (sl = ram_bank_in(s, (int)(addr >> 13))) < 0) return;
    s->slot_age[sl] = s->clock;
    uint8_t *b = &s->ram_slot[sl][addr & (RAM_BANK - 1)];
    if (*b != val) { *b = val; s->slot_dirty[sl] = true; s->cart_ram_dirty = true; s->ram_touch = s->clock; }
}

// A core error is not fatal to the OS: count it and let the app decide. Halting here would take the
// whole device down for a bad ROM, which is exactly the wrong trade on an appliance (the vendored core
// is patched so that returning from this hook is defined — see vendor/README.md, P4).
static void gb_err(struct gb_s *gb, const enum gb_error_e err, const uint16_t addr)
{
    gb_session_t *s = (gb_session_t *)gb->direct.priv;
    if (!s) return;
    if (s->core_errors++ == 0) {
        s->core_err_addr = addr; s->core_err_kind = (uint8_t)err;
        ESP_LOGW(TAG, "core error %d at %04X", (int)err, addr);
    }
}

// Controllers the core has no model of. MMM01 is in its table as "ROM only", which boots the menu of
// a multicart and then crashes, so it is refused here with the rest rather than half-run.
static bool cart_unsupported(uint8_t t)
{
    switch (t) {
        case 0x0B: case 0x0C: case 0x0D: case 0x20: case 0x22: case 0xFC: case 0xFD: case 0xFE: return true;
        default: return false;
    }
}

// ── cartridge header, as the core should see it ─────────────────────────────────────────────────
// gb_init() decides everything from four header bytes and trusts them completely, which rejects
// cartridges a real Game Boy runs and indexes past its own tables on others:
//   0x147 type      HuC1 (0xFF) is an MBC1 with an IR port; mapping it to MBC1+RAM+BATTERY runs it.
//   0x148 ROM size  codes 0x52-0x54 (1.1/1.2/1.5 MB) and plain wrong values index past the core's
//                   bank table. The FILE is the truth: the smallest power of two that holds it.
//   0x149 RAM size  values above 5 index past its RAM table.
//   0x14D checksum  a mismatch refuses the cartridge outright. Prototypes, fan translations and
//                   homebrew routinely ship a stale one; recomputing it over the bytes above lets
//                   them boot, which is what every other emulator does.
// These are served ONLY while gb_init() runs (rom_read_init). The running game reads its real header,
// so a title that checks its own checksum still sees the original.
static void header_fix(gb_session_t *s, const uint8_t *h)
{
    s->hdr_type = h[0x147];
    if (s->hdr_type == 0xFF) s->hdr_type = 0x03;              // HuC1 -> MBC1 + RAM + battery

    uint8_t want = 0;                                         // 0 = 32 KB, each step doubles
    while (want < 8 && (32u * 1024u << want) < s->rom_bytes) want++;
    s->hdr_rom = h[0x148];
    if (s->hdr_rom > 8 || (32u * 1024u << s->hdr_rom) < s->rom_bytes) s->hdr_rom = want;

    s->hdr_ram = h[0x149] > 5 ? 3 : h[0x149];

    uint8_t x = 0;
    for (int i = 0x134; i <= 0x14C; i++) {
        uint8_t b = h[i];
        if (i == 0x147) b = s->hdr_type;
        if (i == 0x148) b = s->hdr_rom;
        if (i == 0x149) b = s->hdr_ram;
        x = (uint8_t)(x - b - 1);
    }
    if (x != h[0x14D]) ESP_LOGW(TAG, "header checksum %02X, expected %02X — accepted", h[0x14D], x);
    if (s->hdr_type != h[0x147] || s->hdr_rom != h[0x148] || s->hdr_ram != h[0x149])
        ESP_LOGW(TAG, "header fixed: type %02X->%02X rom %02X->%02X ram %02X->%02X",
                 h[0x147], s->hdr_type, h[0x148], s->hdr_rom, h[0x149], s->hdr_ram);
    s->hdr_sum = x;
}

static uint8_t rom_read_init(struct gb_s *gb, const uint_fast32_t addr)
{
    gb_session_t *s = (gb_session_t *)gb->direct.priv;
    switch (addr) {
        case 0x147: return s->hdr_type;
        case 0x148: return s->hdr_rom;
        case 0x149: return s->hdr_ram;
        case 0x14D: return s->hdr_sum;
        default:    return rom_read(gb, addr);
    }
}

// The MBC3 clock. Pokémon Gold/Silver and friends read it for day/night and timed events; left at
// zero it resets every session. Seed it from the system clock when that clock is plausibly set (the
// device keeps it across the Solo reboot) — otherwise leave the cartridge's own count alone.
//
// The DAY counter must only ever move forward: games store the day they last saw and act on the
// difference. The core's gb_set_rtc() uses tm_yday, which falls back to 0 every New Year — a game
// would see time run backwards. So the day is counted from a fixed epoch (1 Jan 2024) and wraps at
// the counter's own 512 days, the way a real cartridge left running would.
static void rtc_seed(gb_session_t *s)
{
    if (s->gb.mbc != 3) return;
    time_t now = time(NULL);
    if (now < 1704067200) return;                             // before 2024: the clock was never set
    struct tm tm; localtime_r(&now, &tm);
    long days = tm.tm_yday;
    for (int y = 2024; y < tm.tm_year + 1900; y++)
        days += ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 366 : 365;
    days %= 512;
    s->gb.rtc_real.bytes[0] = (uint8_t)tm.tm_sec;
    s->gb.rtc_real.bytes[1] = (uint8_t)tm.tm_min;
    s->gb.rtc_real.bytes[2] = (uint8_t)tm.tm_hour;
    s->gb.rtc_real.bytes[3] = (uint8_t)(days & 0xFF);
    s->gb.rtc_real.bytes[4] = (uint8_t)((s->gb.rtc_real.bytes[4] & 0x40) | ((days >> 8) & 1));  // keep HALT
}

// ── scanline out ────────────────────────────────────────────────────────────────────────────────
static IRAM_ATTR void lcd_line(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line)
{
    gb_session_t *s = (gb_session_t *)gb->direct.priv;
    if (s && s->on_line) s->on_line(pixels, (int)line, s->user);
}

// ── battery RAM banks ───────────────────────────────────────────────────────────────────────────
// On disk (.sav, .sav.swp, the RAM part of a state) the RAM is always ONE flat image, byte-compatible
// with the .sav every other emulator writes; only its residency in memory is banked.
static size_t ram_chunk(const gb_session_t *s) { return s->cart_ram_bytes < RAM_BANK ? s->cart_ram_bytes : RAM_BANK; }
static bool   ram_swapped(const gb_session_t *s) { return s->ram_nb > s->ram_ns; }
static void   swp_name(const gb_session_t *s, char *out, size_t n) { snprintf(out, n, "%s.sav.swp", s->rom_path); }

// One bank between a slot and the swap file. Opened per call: a swap is an event by design, and a
// handle held for the whole session would cost a FATFS object the ROM cache could use.
static bool swp_io(gb_session_t *s, int bank, uint8_t *buf, bool write)
{
    char p[320]; swp_name(s, p, sizeof p);
    FILE *f = fopen(p, write ? "r+b" : "rb");
    if (!f) return false;
    bool ok = fseek(f, (long)bank * RAM_BANK, SEEK_SET) == 0
           && (write ? fwrite(buf, 1, RAM_BANK, f) : fread(buf, 1, RAM_BANK, f)) == RAM_BANK;
    ok = (fclose(f) == 0) && ok;
    return ok;
}

// Bring a bank in from the card over the least recently used slot (written back first if dirty).
// Returns the slot, or -1 if the card refused — the access then reads open bus / is dropped, which is
// what a real cartridge with a bad contact does, rather than serving another bank's bytes.
static int ram_bank_in(gb_session_t *s, int bank)
{
    if (!ram_swapped(s) || bank >= s->ram_nb) return -1;
    int v = 0;
    for (int i = 1; i < s->ram_ns; i++) if (s->slot_age[i] < s->slot_age[v]) v = i;
    int old = s->slot_bank[v];
    if (old >= 0 && s->slot_dirty[v] && !swp_io(s, old, s->ram_slot[v], true)) {
        ESP_LOGW(TAG, "SRAM bank %d: write-back failed", old);
        return -1;
    }
    if (old >= 0) s->bank_slot[old] = -1;
    s->slot_dirty[v] = false;
    s->slot_bank[v] = -1;
    if (!swp_io(s, bank, s->ram_slot[v], false)) { ESP_LOGW(TAG, "SRAM bank %d: read failed", bank); return -1; }
    s->slot_bank[v] = (int8_t)bank;
    s->bank_slot[bank] = (int8_t)v;
    s->slot_age[v] = s->clock;
    s->ram_swaps++;
    return v;
}

static bool ram_alloc(gb_session_t *s, size_t need)
{
    s->cart_ram_bytes = need;
    s->ram_nb = (uint8_t)((need + RAM_BANK - 1) / RAM_BANK);
    s->ram_ns = s->ram_nb > RAM_RESIDENT ? RAM_RESIDENT : s->ram_nb;
    for (int i = 0; i < RAM_BANKS; i++) { s->bank_slot[i] = -1; s->slot_bank[i] = -1; }
    size_t c = ram_chunk(s);
    for (int i = 0; i < s->ram_ns; i++) {
        if (!(s->ram_slot[i] = (uint8_t *)calloc(1, c))) return false;
        s->slot_bank[i] = (int8_t)i; s->bank_slot[i] = (int8_t)i;
    }
    s->heap_bytes += c * s->ram_ns;
    return true;
}
static void ram_free(gb_session_t *s)
{
    for (int i = 0; i < RAM_BANKS; i++) { free(s->ram_slot[i]); s->ram_slot[i] = NULL; }
    if (s->cart_ram_bytes && ram_swapped(s)) { char p[320]; swp_name(s, p, sizeof p); remove(p); }
}

// Write back every dirty resident bank, so the swap file is the whole truth.
static bool ram_flush(gb_session_t *s)
{
    if (!ram_swapped(s)) return true;
    for (int i = 0; i < s->ram_ns; i++)
        if (s->slot_dirty[i] && s->slot_bank[i] >= 0) {
            if (!swp_io(s, s->slot_bank[i], s->ram_slot[i], true)) return false;
            s->slot_dirty[i] = false;
        }
    return true;
}
// Reload the resident slots from the swap file (after it was rewritten as a whole).
static bool ram_reload(gb_session_t *s)
{
    bool ok = true;
    for (int i = 0; i < s->ram_ns; i++) {
        int b = s->slot_bank[i] >= 0 ? s->slot_bank[i] : i;
        s->bank_slot[b] = (int8_t)i; s->slot_bank[i] = (int8_t)b; s->slot_dirty[i] = false;
        if (!swp_io(s, b, s->ram_slot[i], false)) ok = false;
    }
    return ok;
}

// Copy `n` bytes between two open files in 512-byte steps (a stack buffer, never a heap block);
// `in == NULL` writes zeros. Returns the bytes written.
static size_t copy_stream(FILE *out, FILE *in, size_t n)
{
    uint8_t buf[512];
    size_t done = 0;
    if (!in) memset(buf, 0, sizeof buf);
    while (done < n) {
        size_t want = n - done < sizeof buf ? n - done : sizeof buf;
        size_t got = in ? fread(buf, 1, want, in) : want;
        if (got < want) memset(buf + got, 0, want - got);       // short source: the rest is erased RAM
        if (fwrite(buf, 1, want, out) != want) break;
        done += want;
        if (in && got < want) in = NULL;
    }
    return done;
}

// The whole RAM image out to an open file. Returns the bytes written.
static size_t ram_export(gb_session_t *s, FILE *out)
{
    if (!s->cart_ram_bytes) return 0;
    if (!ram_swapped(s)) {
        size_t c = ram_chunk(s), done = 0;
        for (int i = 0; i < s->ram_nb; i++) {
            size_t n = fwrite(s->ram_slot[i], 1, c, out);
            done += n;
            if (n != c) break;
        }
        return done;
    }
    if (!ram_flush(s)) return 0;
    char p[320]; swp_name(s, p, sizeof p);
    FILE *in = fopen(p, "rb");
    if (!in) return 0;
    size_t done = copy_stream(out, in, s->cart_ram_bytes);
    fclose(in);
    return done;
}

// The whole RAM image in from an open file (`in == NULL` = erased RAM). Returns the bytes taken from
// the file; a short file leaves the rest erased. Clears every dirty mark: RAM now equals its source.
static size_t ram_import(gb_session_t *s, FILE *in)
{
    if (!s->cart_ram_bytes) return 0;
    size_t got = 0;
    if (!ram_swapped(s)) {
        size_t c = ram_chunk(s);
        for (int i = 0; i < s->ram_nb; i++) {
            size_t n = in ? fread(s->ram_slot[i], 1, c, in) : 0;
            if (n < c) memset(s->ram_slot[i] + n, 0, c - n);
            got += n;
            if (n < c) in = NULL;
            s->slot_dirty[i] = false;
        }
        return got;
    }
    char p[320]; swp_name(s, p, sizeof p);
    FILE *out = fopen(p, "wb");
    if (!out) return 0;
    long start = in ? ftell(in) : 0;
    copy_stream(out, in, s->cart_ram_bytes);
    if (in) got = (size_t)(ftell(in) - start);
    if (fclose(out) != 0 || !ram_reload(s)) return 0;
    return got;
}

static void ram_clear(gb_session_t *s) { ram_import(s, NULL); }

// ── save RAM ────────────────────────────────────────────────────────────────────────────────────
// "<rom>.sav" beside the cartridge. Written to "<rom>.sav.tmp" first and renamed over the old file:
// the device runs on a 120 mAh cell, and a save cut short by a flat battery must cost the last few
// seconds of play, never the whole save. FATFS cannot rename onto an existing name, so the old file
// is removed in between — and a load that finds no .sav falls back to a .tmp left by exactly that gap.
static void sav_name(const gb_session_t *s, char *out, size_t n, bool tmp)
{
    snprintf(out, n, "%s.sav%s", s->rom_path, tmp ? ".tmp" : "");
}

static void sav_load(gb_session_t *s)
{
    if (!s->cart_ram_bytes) return;
    char p[320];
    sav_name(s, p, sizeof p, false);
    FILE *f = fopen(p, "rb");
    if (!f) { sav_name(s, p, sizeof p, true); f = fopen(p, "rb"); }
    if (!f) { ram_clear(s); return; }         // no save yet: erased RAM (and, if banked, a fresh swap file)
    size_t got = ram_import(s, f);
    fclose(f);
    ESP_LOGI(TAG, "save loaded (%u B)", (unsigned)got);
}

void nucleo_gb_save(void)
{
    gb_session_t *s = S;
    if (!s || !s->cart_ram_bytes || !s->cart_ram_dirty) return;
    // A console that has executed an invalid opcode is running data as code, and whatever it wrote to
    // battery RAM since is garbage. Writing that over the player's save would turn a crash into lost
    // progress — so a crashed session never saves. Reset (or loading a state) clears the condition.
    if (s->core_errors) { ESP_LOGW(TAG, "save skipped: the console crashed"); return; }
    char tmp[320], fin[320];
    sav_name(s, tmp, sizeof tmp, true);
    sav_name(s, fin, sizeof fin, false);
    // If an earlier save died between removing .sav and renaming .tmp, the .tmp IS the save. Promote
    // it before a new .tmp is written over it.
    struct stat sb;
    if (stat(fin, &sb) != 0 && stat(tmp, &sb) == 0 && rename(tmp, fin) != 0) {
        ESP_LOGW(TAG, "cannot recover %s — not overwriting it", tmp);
        return;
    }
    FILE *f = fopen(tmp, "wb");
    if (!f) { ESP_LOGW(TAG, "cannot write %s", tmp); return; }
    size_t put = ram_export(s, f);
    bool ok = (put == s->cart_ram_bytes);
    ok = (fclose(f) == 0) && ok;            // FATFS flushes on close: a failed close is a failed write
    if (!ok) { remove(tmp); ESP_LOGW(TAG, "save write failed — previous save kept"); return; }
    remove(fin);
    if (rename(tmp, fin) != 0) { ESP_LOGW(TAG, "save rename failed — kept as %s", tmp); return; }
    s->cart_ram_dirty = false;
    ESP_LOGI(TAG, "save written (%u B)", (unsigned)put);
}

// Games write battery RAM when the player saves — and nothing reaches the card until the emulator
// closes, so a flat battery or a crash threw the save away. Write it once the game has left the RAM
// alone for ~1.5 s: a save screen finishes well inside that, and a game that uses its battery RAM as
// scratch every frame never goes quiet, so it cannot turn this into an SD write per second.
bool nucleo_gb_autosave(void)
{
    gb_session_t *s = S;
    if (!s || !s->cart_ram_dirty) return false;
    if (s->clock - s->ram_touch < 90) return false;
    nucleo_gb_save();
    return !s->cart_ram_dirty;
}

// ── lifecycle ───────────────────────────────────────────────────────────────────────────────────
static void session_free(gb_session_t *s)
{
    if (!s) return;
    // Give the I2S channel BACK. Opening it allocates a TX channel plus its DMA descriptors and
    // buffers, and nothing else was releasing them: quitting a cartridge left that RAM held for the
    // rest of the session, so the next ROM had a smaller heap to be cached in (and the idle speaker
    // kept its channel powered). Closing is a no-op if a real track has since taken the channel over.
    if (s->sound) { s->sound = false; nucleo_audio_pcm_close(); }
    if (s->fp) fclose(s->fp);
    for (int i = 0; i < s->pg_n; i++) free(s->pg[i]);
    free(s->rom_all); ram_free(s);
    free(s);
}

esp_err_t nucleo_gb_open(const char *rom_path, nucleo_gb_line_fn on_line, void *user)
{
    if (!rom_path || !*rom_path) return ESP_ERR_INVALID_ARG;
    nucleo_gb_close();

    gb_session_t *s = (gb_session_t *)calloc(1, sizeof(gb_session_t));
    if (!s) { ESP_LOGE(TAG, "no RAM for the session (%u B)", (unsigned)sizeof(gb_session_t)); return ESP_ERR_NO_MEM; }
    s->heap_bytes = sizeof(gb_session_t);
    s->on_line = on_line; s->user = user;

    s->fp = fopen(rom_path, "rb");
    if (!s->fp) { ESP_LOGE(TAG, "cannot open %s", rom_path); session_free(s); return ESP_ERR_NOT_FOUND; }
    fseek(s->fp, 0, SEEK_END);
    long sz = ftell(s->fp);
    fseek(s->fp, 0, SEEK_SET);
    if (sz < 0x150) { session_free(s); return ESP_ERR_INVALID_SIZE; }   // not even a header
    s->rom_bytes = (uint32_t)sz;
    snprintf(s->rom_path, sizeof s->rom_path, "%s", rom_path);

    // Decide from the header BEFORE any big allocation: a cartridge this core cannot run should be
    // refused in microseconds, not after the page cache has been carved out of the heap.
    {
        uint8_t hdr[0x150];
        if (fread(hdr, 1, sizeof hdr, s->fp) != sizeof hdr) { session_free(s); return ESP_ERR_INVALID_SIZE; }
        fseek(s->fp, 0, SEEK_SET);
        if (hdr[0x143] == 0xC0) {                 // CGB-only: this is a DMG core, it would draw garbage
            ESP_LOGW(TAG, "Game Boy Color-only cartridge");
            session_free(s);
            return ESP_ERR_INVALID_VERSION;
        }
        if (cart_unsupported(hdr[0x147])) {
            ESP_LOGW(TAG, "cartridge type %02X (%s) is not emulated", hdr[0x147], nucleo_gb_cart_name(hdr[0x147]));
            session_free(s);
            return ESP_ERR_NOT_SUPPORTED;
        }
        header_fix(s, hdr);
    }

    // Battery RAM FIRST. It is a single block of up to 32 KB, and the page cache below is greedy — it
    // takes 1 KB pages until only KEEP is left. Allocated after it, a 32 KB-RAM cartridge (Pokémon,
    // Zelda DX...) found no block big enough and failed with "not enough RAM" on a heap that had had
    // plenty. Sized from the (corrected) header exactly as the core sizes it: MBC2 carries 512 nibbles
    // inside the controller, everything else is the RAM-size code.
    {
        static const uint32_t ram_sizes[6] = { 0, 0x800, 0x2000, 0x8000, 0x20000, 0x10000 };
        size_t need = (s->hdr_type == 0x05 || s->hdr_type == 0x06) ? 0x200 : ram_sizes[s->hdr_ram];
        if (need > CART_RAM_MAX) {
            ESP_LOGW(TAG, "cart RAM %u B capped to %u B", (unsigned)need, (unsigned)CART_RAM_MAX);
            need = CART_RAM_MAX;
        }
        if (need) {
            if (!ram_alloc(s, need)) { ESP_LOGE(TAG, "no RAM for the %u B cart save", (unsigned)need); session_free(s); return ESP_ERR_NO_MEM; }
            sav_load(s);
        }
    }

    // Sound is BEST-EFFORT: if the speaker is busy (a track playing, the recorder holding the shared
    // mic pin) the game still runs, silently. Refusing to start a cartridge because audio was taken
    // would be the wrong trade on a games machine. It opens HERE, before the page cache, so its DMA
    // channel is already paid for when the greedy cache below decides how much of the heap to take.
    s->sound = (nucleo_audio_pcm_open(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS) == ESP_OK);
    if (!s->sound) ESP_LOGW(TAG, "speaker unavailable — playing silently");

    // Prefer holding the WHOLE ROM: it removes SD traffic from the hot path entirely, and the small
    // carts (32/64 KB) that make up much of the library fit easily. Only reach for the bank cache
    // when the ROM is too big — and leave real headroom, because the app still needs its own buffers.
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    const size_t HEADROOM = 24 * 1024;
    if (s->rom_bytes + HEADROOM <= largest) {
        s->rom_all = (uint8_t *)malloc(s->rom_bytes);
        if (s->rom_all) {
            if (fread(s->rom_all, 1, s->rom_bytes, s->fp) != s->rom_bytes) { free(s->rom_all); s->rom_all = NULL; }
            else { s->heap_bytes += s->rom_bytes; fclose(s->fp); s->fp = NULL; }   // file no longer needed
        }
    }
    if (!s->rom_all) {
        // Take as many 4 KB pages as the heap will give, keeping a floor of headroom for the app's
        // own buffers. More slots means fewer SD refills, and the refills that remain are small.
        s->cur_tag = PG_EMPTY;
        for (int i = 0; i < PG_SLOTS; i++) s->pg_tag[i] = PG_EMPTY;
        memset(s->hint, -1, sizeof s->hint);
        // The floor left for what a RUNNING game allocates on the side: the file handles of an
        // autosave, a save state or a trace flush (FILE + FATFS object + stdio buffer, a few KB, all
        // transient). The speaker's DMA channel is no longer part of it — it opened above, before the
        // cache — so 8 KB covers it; every KB above that is a ROM page, and on a 32 KB-SRAM cartridge
        // (Pokémon) the difference between 9 pages and 17 is the difference between 22 SD reads a
        // frame and a playable game.
        const size_t KEEP = 8 * 1024;
        for (int i = 0; i < PG_SLOTS; i++) {
            if (heap_caps_get_free_size(MALLOC_CAP_DEFAULT) < PG_SIZE + KEEP) break;
            uint8_t *b = (uint8_t *)malloc(PG_SIZE);
            if (!b) break;
            s->pg[s->pg_n++] = b;
            s->heap_bytes += PG_SIZE;
        }
        if (s->pg_n < PG_MIN) {
            ESP_LOGE(TAG, "only %d ROM pages fit (need %d) — largest block %u B",
                     s->pg_n, PG_MIN, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
            session_free(s);
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "ROM page cache: %d x %u B", s->pg_n, (unsigned)PG_SIZE);
    }

    // Publish the session BEFORE init: the APU shims (audio_read/audio_write) are bare functions with
    // no gb parameter, so the global is their only route, and gb_init writes the sound registers.
    S = s;
    minigb_apu_audio_init(&s->apu);

    // Init reads the header through rom_read_init (the corrected bytes, see header_fix); the running
    // game gets the plain reader, and the real header, from here on.
    enum gb_init_error_e err = gb_init(&s->gb, rom_read_init, cart_ram_read, cart_ram_write, gb_err, s);
    if (err != GB_INIT_NO_ERROR) {
        ESP_LOGE(TAG, "gb_init failed (%d), cartridge type %02X", (int)err, s->hdr_type);
        S = NULL;                       // never leave a dangling session published
        session_free(s);
        return ESP_ERR_NOT_SUPPORTED;
    }

    // The core's own idea of the save size must match what was allocated before the cache; if it does
    // not, the host arithmetic above has drifted from the core's table — say so loudly.
    size_t core_need = 0;
    if (gb_get_save_size_s(&s->gb, &core_need) != 0) core_need = 0;
    if (core_need > CART_RAM_MAX) core_need = CART_RAM_MAX;
    if (core_need != s->cart_ram_bytes)
        ESP_LOGW(TAG, "cart RAM size mismatch: core %u B, host %u B", (unsigned)core_need, (unsigned)s->cart_ram_bytes);
    s->gb.gb_rom_read = rom_read;       // header decisions are made: switch to the hot-path reader
    rtc_seed(s);

    gb_init_lcd(&s->gb, lcd_line);
    // Frame skipping is the core's own knob; we run every frame and let the app pace itself, so the
    // picture is never silently degraded without the app asking for it.
    s->gb.direct.frame_skip = 0;

    gb_get_rom_name(&s->gb, s->title);
    s->title[16] = '\0';

    ESP_LOGI(TAG, "'%s' %uKB %s | heap %u B | largest was %u B | sound %s",
             s->title, (unsigned)(s->rom_bytes / 1024),
             s->rom_all ? "resident" : "paged from SD",
             (unsigned)s->heap_bytes, (unsigned)largest, s->sound ? "on" : "off");
    return ESP_OK;
}

void nucleo_gb_close(void)
{
    if (!S) return;
    nucleo_gb_save();
    gb_session_t *s = S;
    S = NULL;                 // clear first: the callbacks read S and must not see a freed session
    session_free(s);
}

bool nucleo_gb_is_open(void) { return S != NULL; }

void nucleo_gb_run_frame(void)
{
    if (!S) return;
    int64_t t0 = esp_timer_get_time();
    gb_run_frame(&S->gb);
    int64_t t1 = esp_timer_get_time();
    S->us_cpu += (uint32_t)(t1 - t0);
    S->frames++;
    S->clock++;
    // One frame of sound, produced AFTER the frame that generated it and pushed straight out. The
    // write blocks on the I2S DMA, which is also what paces us to real speed — so a dropped frame
    // shows up as a click rather than as drift.
    if (S->sound) {
        minigb_apu_audio_callback(&S->apu, S->audio);
        nucleo_audio_pcm_write(S->audio, sizeof(S->audio));
        S->us_audio += (uint32_t)(esp_timer_get_time() - t1);
    }
}

// Restart the cartridge from the top of its ROM. Battery RAM (flushed first) and the page cache
// survive; the APU is re-initialised by hand because gb_reset() leaves it alone.
void nucleo_gb_reset(void)
{
    if (!S) return;
    if (S->core_errors) {
        // After a crash the battery RAM in memory is suspect: go back to what is on the card.
        S->cart_ram_dirty = false;
        sav_load(S);                          // erases first when there is no save
        S->core_errors = 0;
    } else {
        nucleo_gb_save();
    }
    gb_reset(&S->gb);
    minigb_apu_audio_init(&S->apu);
    rtc_seed(S);
}

void nucleo_gb_set_frameskip(bool on) { if (S) S->gb.direct.frame_skip = on; }
void nucleo_gb_set_interlace(bool on) { if (S) S->gb.direct.interlace  = on; }

void nucleo_gb_reset_counters(void)
{
    if (!S) return;
    S->us_cpu = S->us_audio = 0;
    S->bank_misses = 0;
    S->frames = 0;
}

// ── save states ─────────────────────────────────────────────────────────────────────────────────
// A state is the console struct byte for byte, followed by the cartridge RAM. The struct carries six
// function pointers (four cartridge accessors, the error hook, the scanline hook) plus our own
// session pointer, and those are the ONE thing that must not come from the file: a corrupt or
// hand-edited state would otherwise hand the CPU an arbitrary address to call. They are saved with
// everything else for simplicity and then overwritten from the live session on load.
#define STATE_MAGIC 0x3142474Eu   // 'NGB1'
// "<rom>.st<slot>". It used to be derived from the .sav path, which exists only for cartridges with
// battery RAM — so on Tetris, Super Mario Land and every other RAM-less cart the path came out as a
// bare ".st0" and save states silently failed. The session now keeps the ROM path itself.
static void state_path(char *out, size_t n, int slot)
{
    snprintf(out, n, "%s.st%d", S->rom_path, slot < 0 ? 0 : (slot > 9 ? 9 : slot));
}

bool nucleo_gb_state_exists(int slot)
{
    if (!S) return false;
    char p[320]; state_path(p, sizeof p, slot);
    struct stat sb;
    return stat(p, &sb) == 0 && sb.st_size > (long)sizeof(struct gb_s);
}

static esp_err_t state_write(const char *p)
{
    FILE *f = fopen(p, "wb");
    if (!f) { ESP_LOGW(TAG, "state: cannot write %s", p); return ESP_FAIL; }
    uint32_t magic = STATE_MAGIC, ram = (uint32_t)S->cart_ram_bytes;
    bool ok = fwrite(&magic, 1, 4, f) == 4
           && fwrite(&ram, 1, 4, f) == 4
           && fwrite(&S->gb, 1, sizeof S->gb, f) == sizeof S->gb
           && ram_export(S, f) == ram;
    ok = (fclose(f) == 0) && ok;
    if (!ok) { remove(p); ESP_LOGW(TAG, "state: short write %s", p); return ESP_FAIL; }
    return ESP_OK;
}

esp_err_t nucleo_gb_state_save(int slot)
{
    if (!S) return ESP_ERR_INVALID_STATE;
    char p[320]; state_path(p, sizeof p, slot);
    esp_err_t e = state_write(p);
    if (e == ESP_OK) ESP_LOGI(TAG, "state %d saved (%u B)", slot, (unsigned)(sizeof S->gb + S->cart_ram_bytes));
    return e;
}

// A state file this session could have written: right magic, right RAM size, right total length.
static bool state_valid(const char *p)
{
    struct stat sb;
    if (stat(p, &sb) != 0 || (size_t)sb.st_size != 8 + sizeof(struct gb_s) + S->cart_ram_bytes) return false;
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    uint32_t magic = 0, ram = 0;
    bool ok = fread(&magic, 1, 4, f) == 4 && fread(&ram, 1, 4, f) == 4
           && magic == STATE_MAGIC && ram == S->cart_ram_bytes;
    fclose(f);
    return ok;
}

// Read a state STRAIGHT INTO the live console. There is nowhere else to put it: the Solo task that
// runs every game has an 8 KB stack (the old `struct gb_s tmp` — 17 KB of it — overflowed that on
// every "Load state"), and while a game runs the heap's largest block is ~7 KB. Every pointer in the
// struct comes back from the LIVE session afterwards, never from the file — the serial and boot-ROM
// hooks included, which the core calls whenever they are non-NULL — and so do the front-end's own
// knobs (frame skip, interlace, the held buttons).
static bool state_read_live(const char *p)
{
    struct gb_s *g = &S->gb;
    uint8_t (*rom)(struct gb_s *, const uint_fast32_t) = g->gb_rom_read;
    uint8_t (*crr)(struct gb_s *, const uint_fast32_t) = g->gb_cart_ram_read;
    void (*crw)(struct gb_s *, const uint_fast32_t, const uint8_t) = g->gb_cart_ram_write;
    void (*err)(struct gb_s *, const enum gb_error_e, const uint16_t) = g->gb_error;
    void (*stx)(struct gb_s *, const uint8_t) = g->gb_serial_tx;
    enum gb_serial_rx_ret_e (*srx)(struct gb_s *, uint8_t *) = g->gb_serial_rx;
    uint8_t (*boot)(struct gb_s *, const uint_fast16_t) = g->gb_bootrom_read;
    void (*line)(struct gb_s *, const uint8_t *, const uint_fast8_t) = g->display.lcd_draw_line;
    void *priv = g->direct.priv;
    bool fs = g->direct.frame_skip, il = g->direct.interlace;
    uint8_t joy = g->direct.joypad;

    FILE *f = fopen(p, "rb");
    bool ok = f && fseek(f, 8, SEEK_SET) == 0
                && fread(g, 1, sizeof *g, f) == sizeof *g
                && ram_import(S, f) == S->cart_ram_bytes;
    if (f) fclose(f);

    g->gb_rom_read = rom; g->gb_cart_ram_read = crr; g->gb_cart_ram_write = crw; g->gb_error = err;
    g->gb_serial_tx = stx; g->gb_serial_rx = srx; g->gb_bootrom_read = boot;
    g->display.lcd_draw_line = line; g->direct.priv = priv;
    g->direct.frame_skip = fs; g->direct.interlace = il; g->direct.joypad = joy;
    return ok;
}

esp_err_t nucleo_gb_state_load(int slot)
{
    if (!S) return ESP_ERR_INVALID_STATE;
    char p[320]; state_path(p, sizeof p, slot);
    struct stat sb;
    if (stat(p, &sb) != 0) return ESP_ERR_NOT_FOUND;
    if (!state_valid(p)) { ESP_LOGW(TAG, "state %d: rejected", slot); return ESP_ERR_INVALID_CRC; }

    // Loading overwrites the live console in place, so a read that failed half way would leave half
    // of one game and half of another. The running console goes to "<rom>.stu" first; a failed load
    // puts it back exactly as it was. The file is removed once the load has succeeded.
    char undo[320];
    snprintf(undo, sizeof undo, "%s.stu", S->rom_path);
    bool have_undo = state_write(undo) == ESP_OK;

    if (!state_read_live(p)) {
        ESP_LOGW(TAG, "state %d: read failed half way — restoring the running game", slot);
        if (!(have_undo && state_read_live(undo))) {
            // Neither the state nor the undo could be read back: fall back to a clean power-on with the
            // battery RAM from the card, never a half-loaded console.
            sav_load(S);
            S->cart_ram_dirty = false;
            gb_reset(&S->gb);
            minigb_apu_audio_init(&S->apu);
        }
        if (have_undo) remove(undo);
        return ESP_ERR_INVALID_CRC;
    }
    if (have_undo) remove(undo);
    if (S->cart_ram_bytes) S->cart_ram_dirty = true;
    S->core_errors = 0;          // the restored console is the one from before any crash
    rtc_seed(S);                 // ...but real time has moved on since the state was written
    ESP_LOGI(TAG, "state %d loaded", slot);
    return ESP_OK;
}

void nucleo_gb_set_buttons(uint8_t mask)
{
    if (!S) return;
    // Peanut-GB uses ACTIVE-LOW joypad bits, and its bit order is the hardware's, not ours.
    uint8_t j = 0xFF;
    if (mask & NUCLEO_GB_A)      j &= ~JOYPAD_A;
    if (mask & NUCLEO_GB_B)      j &= ~JOYPAD_B;
    if (mask & NUCLEO_GB_SELECT) j &= ~JOYPAD_SELECT;
    if (mask & NUCLEO_GB_START)  j &= ~JOYPAD_START;
    if (mask & NUCLEO_GB_RIGHT)  j &= ~JOYPAD_RIGHT;
    if (mask & NUCLEO_GB_LEFT)   j &= ~JOYPAD_LEFT;
    if (mask & NUCLEO_GB_UP)     j &= ~JOYPAD_UP;
    if (mask & NUCLEO_GB_DOWN)   j &= ~JOYPAD_DOWN;
    S->gb.direct.joypad = j;
}

const char *nucleo_gb_title(void) { return S ? S->title : ""; }

void nucleo_gb_get_stats(nucleo_gb_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!S) return;
    out->heap_bytes   = S->heap_bytes;
    out->rom_bytes    = S->rom_bytes;
    out->us_cpu   = S->us_cpu;
    out->us_audio = S->us_audio;
    out->rom_resident = (S->rom_all != NULL);
    out->rom_paged    = (S->rom_all == NULL);
    out->rom_pages    = S->pg_n;
    out->bank_misses  = S->bank_misses;
    out->frames       = S->frames;
    out->core_errors  = S->core_errors;
    out->core_err_addr = S->core_err_addr;
    out->cart_type    = S->hdr_type;
    out->save_dirty   = S->cart_ram_dirty;
    out->ram_swaps    = S->ram_swaps;
    out->ram_banked   = S->cart_ram_bytes && ram_swapped(S);
}

void nucleo_gb_get_regs(nucleo_gb_regs_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!S) return;
    const struct cpu_registers_s *r = &S->gb.cpu_reg;
    out->pc = r->pc.reg; out->sp = r->sp.reg; out->r_a = r->a;
    out->r_b = r->bc.bytes.b; out->r_c = r->bc.bytes.c; out->r_d = r->de.bytes.d; out->r_e = r->de.bytes.e;
    out->r_h = r->hl.bytes.h; out->r_l = r->hl.bytes.l;
}

// Header only — no session, no allocation. Lets the shelf refuse a cartridge this core cannot run
// BEFORE it tears the UI down, and say why in words rather than "invalid ROM".
esp_err_t nucleo_gb_probe(const char *rom_path, nucleo_gb_info_t *out)
{
    if (!rom_path || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    FILE *f = fopen(rom_path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    uint8_t h[0x150];
    size_t got = fread(h, 1, sizeof h, f);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    if (got != sizeof h) return ESP_ERR_INVALID_SIZE;
    out->rom_bytes = sz > 0 ? (uint32_t)sz : 0;
    out->cart_type = h[0x147];
    out->cgb_flag  = h[0x143];
    if (out->cgb_flag == 0xC0) return ESP_ERR_INVALID_VERSION;
    return cart_unsupported(out->cart_type) ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
}

const char *nucleo_gb_cart_name(uint8_t type)
{
    switch (type) {
        case 0x0B: case 0x0C: case 0x0D: return "MMM01";
        case 0x20: return "MBC6";
        case 0x22: return "MBC7";
        case 0xFC: return "Pocket Camera";
        case 0xFD: return "TAMA5";
        case 0xFE: return "HuC3";
        default:   return "?";
    }
}
