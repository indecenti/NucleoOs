// nucleo_gb — see include/nucleo_gb.h for why a Game Boy fits on this board at all.
//
// This file is the HOST half of Peanut-GB: the core asks for cartridge bytes, hands back finished
// scanlines, and reports errors; everything about where a ROM lives and how it reaches the panel is
// decided here. The core itself is vendored verbatim and never edited.
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

// Peanut-GB build switches — set BEFORE the include, they are compile-time for the whole core.
#define ENABLE_LCD   1
#define ENABLE_SOUND 1     // Peanut-GB ships no APU: it calls audio_read/audio_write, which we route
                           // to the vendored minigb_apu (MIT, context-based, ~2.2 KB per frame).
// OFF deliberately. It re-sorts sprites by X coordinate on every scanline to reproduce the DMG's
// priority rule, which is 144 sorts per frame for a difference most games never show. On a board
// with a 16.7 ms budget and no cycles to spare that is the wrong trade: correctness nobody sees,
// paid for with frames everybody feels.
#define PEANUT_GB_HIGH_LCD_ACCURACY 0
// Also off. It tags every emitted pixel with which palette produced it (OBJ0/OBJ1/BG) so a front-end
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

#define CART_RAM_MAX   (32 * 1024)      // largest battery RAM we host (MBC3/5 4x8 KB)

// ROM page cache. A cartridge rarely fits in RAM here (a 128 KB ROM would need 152 KB contiguous on
// a board whose largest block is ~60 KB), so the ROM is served from the SD card and this cache is
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
# define PG_SLOTS 40                    // 40 KB total
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
    uint8_t      *pg[PG_SLOTS];         // 4 KB each, allocated as many as the heap allows
    uint32_t      pg_tag[PG_SLOTS];     // which 4 KB page each slot holds (PG_EMPTY = none)
    uint32_t      pg_age[PG_SLOTS];     // LRU stamp
    int           pg_n;                 // slots actually allocated
    uint32_t      pg_clock;             // monotonic stamp source
    int8_t        hint[PG_HINT];        // tag -> slot guess, always verified before use
    uint32_t      cur_tag;              // fast path: the page the last read hit...
    uint8_t      *cur_buf;              // ...and its buffer
    uint32_t      bank_misses;          // SD refills since open
    uint32_t      us_cpu, us_audio;     // where a frame's time goes

    uint8_t      *cart_ram;
    size_t        cart_ram_bytes;
    bool          cart_ram_dirty;

    char          sav_path[300];
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

static IRAM_ATTR uint8_t cart_ram_read(struct gb_s *gb, const uint_fast32_t addr)
{
    gb_session_t *s = (gb_session_t *)gb->direct.priv;
    if (!s || !s->cart_ram || addr >= s->cart_ram_bytes) return 0xFF;
    return s->cart_ram[addr];
}

static IRAM_ATTR void cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val)
{
    gb_session_t *s = (gb_session_t *)gb->direct.priv;
    if (!s || !s->cart_ram || addr >= s->cart_ram_bytes) return;
    if (s->cart_ram[addr] != val) { s->cart_ram[addr] = val; s->cart_ram_dirty = true; }
}

// A core error is not fatal to the OS: log it and let the app decide. Halting here would take the
// whole device down for a bad ROM, which is exactly the wrong trade on an appliance.
static void gb_err(struct gb_s *gb, const enum gb_error_e err, const uint16_t addr)
{
    (void)gb;
    ESP_LOGW(TAG, "core error %d at %04X", (int)err, addr);
}

// ── scanline out ────────────────────────────────────────────────────────────────────────────────
static IRAM_ATTR void lcd_line(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line)
{
    gb_session_t *s = (gb_session_t *)gb->direct.priv;
    if (s && s->on_line) s->on_line(pixels, (int)line, s->user);
}

// ── save RAM ────────────────────────────────────────────────────────────────────────────────────
static void sav_load(gb_session_t *s)
{
    if (!s->cart_ram || !s->cart_ram_bytes) return;
    FILE *f = fopen(s->sav_path, "rb");
    if (!f) return;
    size_t got = fread(s->cart_ram, 1, s->cart_ram_bytes, f);
    fclose(f);
    ESP_LOGI(TAG, "save loaded (%u B)", (unsigned)got);
}

void nucleo_gb_save(void)
{
    gb_session_t *s = S;
    if (!s || !s->cart_ram || !s->cart_ram_bytes || !s->cart_ram_dirty) return;
    FILE *f = fopen(s->sav_path, "wb");
    if (!f) { ESP_LOGW(TAG, "cannot write %s", s->sav_path); return; }
    size_t put = fwrite(s->cart_ram, 1, s->cart_ram_bytes, f);
    fclose(f);
    s->cart_ram_dirty = (put != s->cart_ram_bytes);
    ESP_LOGI(TAG, "save written (%u B)", (unsigned)put);
}

// ── lifecycle ───────────────────────────────────────────────────────────────────────────────────
static void session_free(gb_session_t *s)
{
    if (!s) return;
    if (s->fp) fclose(s->fp);
    for (int i = 0; i < s->pg_n; i++) free(s->pg[i]);
    free(s->rom_all); free(s->cart_ram);
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
    if (sz <= 0) { session_free(s); return ESP_ERR_INVALID_SIZE; }
    s->rom_bytes = (uint32_t)sz;

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
        const size_t KEEP = 12 * 1024;                     // leave room for the app's band buffer etc.
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

    enum gb_init_error_e err = gb_init(&s->gb, rom_read, cart_ram_read, cart_ram_write, gb_err, s);
    if (err != GB_INIT_NO_ERROR) {
        ESP_LOGE(TAG, "gb_init failed (%d) — not a Game Boy ROM?", (int)err);
        S = NULL;                       // never leave a dangling session published
        session_free(s);
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Cartridge RAM: sized from the header by the core. Allocate only what the cart declares.
    // The _s form is the non-deprecated one: it reports a bad header instead of returning a size.
    size_t need = 0;
    if (gb_get_save_size_s(&s->gb, &need) != 0) need = 0;
    if (need > CART_RAM_MAX) need = CART_RAM_MAX;
    if (need) {
        s->cart_ram = (uint8_t *)calloc(1, need);
        if (!s->cart_ram) { ESP_LOGE(TAG, "no RAM for the %u B cart save", (unsigned)need); S = NULL; session_free(s); return ESP_ERR_NO_MEM; }
        s->cart_ram_bytes = need;
        s->heap_bytes += need;
        snprintf(s->sav_path, sizeof s->sav_path, "%s.sav", rom_path);
        sav_load(s);
    }

    gb_init_lcd(&s->gb, lcd_line);
    // Frame skipping is the core's own knob; we run every frame and let the app pace itself, so the
    // picture is never silently degraded without the app asking for it.
    s->gb.direct.frame_skip = 0;

    gb_get_rom_name(&s->gb, s->title);
    s->title[16] = '\0';

    // Sound is BEST-EFFORT: if the speaker is busy (a track playing, the recorder holding the shared
    // mic pin) the game still runs, silently. Refusing to start a cartridge because audio was taken
    // would be the wrong trade on a games machine.
    s->sound = (nucleo_audio_pcm_open(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS) == ESP_OK);
    if (!s->sound) ESP_LOGW(TAG, "speaker unavailable — playing silently");

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
    // One frame of sound, produced AFTER the frame that generated it and pushed straight out. The
    // write blocks on the I2S DMA, which is also what paces us to real speed — so a dropped frame
    // shows up as a click rather than as drift.
    if (S->sound) {
        minigb_apu_audio_callback(&S->apu, S->audio);
        nucleo_audio_pcm_write(S->audio, sizeof(S->audio));
        S->us_audio += (uint32_t)(esp_timer_get_time() - t1);
    }
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
static void state_path(char *out, size_t n, int slot)
{
    // sav_path is "<rom>.sav" and is the only copy of the ROM path we keep; drop the four-character
    // suffix to get back to the cartridge and hang the slot off that.
    size_t l = strlen(S->sav_path);
    if (l > 4) l -= 4;
    if (l >= n) l = n - 1;
    memcpy(out, S->sav_path, l);
    out[l] = ' ';
    snprintf(out + l, n - l, ".st%d", slot < 0 ? 0 : (slot > 9 ? 9 : slot));
}

bool nucleo_gb_state_exists(int slot)
{
    if (!S) return false;
    char p[300]; state_path(p, sizeof p, slot);
    struct stat sb;
    return stat(p, &sb) == 0 && sb.st_size > (long)sizeof(struct gb_s);
}

esp_err_t nucleo_gb_state_save(int slot)
{
    if (!S) return ESP_ERR_INVALID_STATE;
    char p[300]; state_path(p, sizeof p, slot);
    FILE *f = fopen(p, "wb");
    if (!f) { ESP_LOGW(TAG, "state %d: cannot write %s", slot, p); return ESP_FAIL; }
    uint32_t magic = STATE_MAGIC, ram = S->cart_ram_bytes;
    bool ok = fwrite(&magic, 1, 4, f) == 4
           && fwrite(&ram, 1, 4, f) == 4
           && fwrite(&S->gb, 1, sizeof S->gb, f) == sizeof S->gb;
    if (ok && ram) ok = fwrite(S->cart_ram, 1, ram, f) == ram;
    fclose(f);
    if (!ok) { remove(p); ESP_LOGW(TAG, "state %d: short write", slot); return ESP_FAIL; }
    ESP_LOGI(TAG, "state %d saved (%u B)", slot, (unsigned)(sizeof S->gb + ram));
    return ESP_OK;
}

esp_err_t nucleo_gb_state_load(int slot)
{
    if (!S) return ESP_ERR_INVALID_STATE;
    char p[300]; state_path(p, sizeof p, slot);
    FILE *f = fopen(p, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;

    uint32_t magic = 0, ram = 0;
    struct gb_s tmp;
    bool ok = fread(&magic, 1, 4, f) == 4
           && fread(&ram, 1, 4, f) == 4
           && magic == STATE_MAGIC
           && ram == S->cart_ram_bytes
           && fread(&tmp, 1, sizeof tmp, f) == sizeof tmp;
    // Read the cartridge RAM into a scratch copy first: a state that fails half way through must
    // leave the RUNNING game untouched rather than corrupt it.
    uint8_t *ramtmp = NULL;
    if (ok && ram) {
        ramtmp = (uint8_t *)malloc(ram);
        ok = ramtmp && fread(ramtmp, 1, ram, f) == ram;
    }
    fclose(f);
    if (!ok) { free(ramtmp); ESP_LOGW(TAG, "state %d: rejected", slot); return ESP_ERR_INVALID_CRC; }

    // Everything is verified — commit. The pointers come from the LIVE session, never from the file.
    tmp.gb_rom_read       = S->gb.gb_rom_read;
    tmp.gb_cart_ram_read  = S->gb.gb_cart_ram_read;
    tmp.gb_cart_ram_write = S->gb.gb_cart_ram_write;
    tmp.gb_error          = S->gb.gb_error;
    tmp.display.lcd_draw_line = S->gb.display.lcd_draw_line;
    tmp.direct.priv       = S->gb.direct.priv;
    S->gb = tmp;
    if (ram) { memcpy(S->cart_ram, ramtmp, ram); S->cart_ram_dirty = true; }
    free(ramtmp);
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
}
