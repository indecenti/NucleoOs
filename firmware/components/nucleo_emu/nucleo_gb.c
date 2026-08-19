// nucleo_gb — see include/nucleo_gb.h for why a Game Boy fits on this board at all.
//
// This file is the HOST half of Peanut-GB: the core asks for cartridge bytes, hands back finished
// scanlines, and reports errors; everything about where a ROM lives and how it reaches the panel is
// decided here. The core itself is vendored verbatim and never edited.
#include "nucleo_gb.h"
#include "nucleo_board.h"
#include "nucleo_audio.h"   // raw PCM sink: the APU produces its own samples, we hand them to I2S
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Peanut-GB build switches — set BEFORE the include, they are compile-time for the whole core.
#define ENABLE_LCD   1
#define ENABLE_SOUND 1     // Peanut-GB ships no APU: it calls audio_read/audio_write, which we route
                           // to the vendored minigb_apu (MIT, context-based, ~2.2 KB per frame).
#define PEANUT_GB_HIGH_LCD_ACCURACY 1   // sprite priority/window edge cases; costs a little CPU, and
                                        // without it a visible minority of games render subtly wrong.

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

#define ROM_BANK_BYTES 0x4000u          // 16 KB — the Game Boy's own bank granularity
#define CART_RAM_MAX   (32 * 1024)      // largest battery RAM we host (MBC3/5 4x8 KB)

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
    uint8_t      *bank0;                // else: the fixed first 16 KB, always resident
    uint8_t      *window;               // ...plus ONE cached switchable bank
    uint32_t      window_bank;          // which bank `window` holds
    uint8_t      *page;                 // last resort: ONE 4 KB page, direct-mapped over the whole ROM
    uint32_t      page_no;              // which 4 KB page `page` holds
    uint32_t      bank_misses;

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
static uint8_t rom_read(struct gb_s *gb, const uint_fast32_t addr)
{
    (void)gb;
    gb_session_t *s = S;
    if (!s || addr >= s->rom_bytes) return 0xFF;

    if (s->rom_all) return s->rom_all[addr];            // fast path: whole ROM in RAM

    // Tier 3: a single 4 KB page. Slower than the bank cache (bank 0 is no longer pinned, so hot code
    // and far data can fight over the one page) but it needs 4 KB instead of 32, which is the
    // difference between the cartridge starting and not starting on a tight heap.
    if (s->page) {
        uint32_t pg = addr >> 12;
        if (pg != s->page_no) {
            if (fseek(s->fp, (long)(pg << 12), SEEK_SET) != 0) return 0xFF;
            size_t want = 4096;
            if ((pg << 12) + want > s->rom_bytes) want = s->rom_bytes - (pg << 12);
            size_t got = fread(s->page, 1, want, s->fp);
            if (got != want) return 0xFF;
            if (want < 4096) memset(s->page + want, 0xFF, 4096 - want);
            s->page_no = pg;
            s->bank_misses++;
        }
        return s->page[addr & 0xFFF];
    }

    if (addr < ROM_BANK_BYTES) return s->bank0[addr];   // fixed bank, always resident

    uint32_t bank = (uint32_t)(addr / ROM_BANK_BYTES);
    if (bank != s->window_bank) {
        // Miss: pull the 16 KB bank in. Banks switch rarely inside a frame for most games, but a
        // bank-thrashing title will feel it — hence the resident fast path above, and the stats.
        if (fseek(s->fp, (long)(bank * ROM_BANK_BYTES), SEEK_SET) != 0) return 0xFF;
        size_t want = ROM_BANK_BYTES;
        if (bank * ROM_BANK_BYTES + want > s->rom_bytes) want = s->rom_bytes - bank * ROM_BANK_BYTES;
        if (fread(s->window, 1, want, s->fp) != want) return 0xFF;
        s->window_bank = bank;
        s->bank_misses++;
    }
    return s->window[addr - s->window_bank * ROM_BANK_BYTES];
}

// APU bridge. Peanut-GB calls these for every read/write in the 0xFF10-0xFF3F sound range.
static uint8_t audio_read(const uint16_t addr)
{
    return S ? minigb_apu_audio_read(&S->apu, addr) : 0xFF;
}
static void audio_write(const uint16_t addr, const uint8_t val)
{
    if (S) minigb_apu_audio_write(&S->apu, addr, val);
}

static uint8_t cart_ram_read(struct gb_s *gb, const uint_fast32_t addr)
{
    (void)gb;
    gb_session_t *s = S;
    if (!s || !s->cart_ram || addr >= s->cart_ram_bytes) return 0xFF;
    return s->cart_ram[addr];
}

static void cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val)
{
    (void)gb;
    gb_session_t *s = S;
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
static void lcd_line(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line)
{
    (void)gb;
    gb_session_t *s = S;
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
    free(s->rom_all); free(s->bank0); free(s->window); free(s->page); free(s->cart_ram);
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
        // Tier 2: pin the fixed bank and cache one switchable bank — 32 KB, the fastest layout short
        // of holding the whole cartridge.
        s->bank0  = (uint8_t *)malloc(ROM_BANK_BYTES);
        s->window = (uint8_t *)malloc(ROM_BANK_BYTES);
        if (s->bank0 && s->window) {
            s->heap_bytes += 2 * ROM_BANK_BYTES;
            if (fread(s->bank0, 1, ROM_BANK_BYTES, s->fp) != ROM_BANK_BYTES) { session_free(s); return ESP_FAIL; }
            s->window_bank = 0xFFFFFFFFu;   // 0 is a real bank, so it cannot be the "nothing cached" sentinel
        } else {
            // Tier 3: 32 KB was not there. Rather than refuse to start — which is what the user
            // actually sees, a game that simply does not launch — fall back to ONE 4 KB page. It is
            // slower, and the stats say so, but a cartridge that runs beats a cartridge that does not.
            free(s->bank0); free(s->window); s->bank0 = s->window = NULL;
            s->page = (uint8_t *)malloc(4096);
            if (!s->page) {
                ESP_LOGE(TAG, "no RAM for even a 4 KB ROM page (largest %u B)",
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
                session_free(s);
                return ESP_ERR_NO_MEM;
            }
            s->heap_bytes += 4096;
            s->page_no = 0xFFFFFFFFu;
            ESP_LOGW(TAG, "ROM cache degraded to a single 4 KB page — expect slower loading");
        }
    }

    enum gb_init_error_e err = gb_init(&s->gb, rom_read, cart_ram_read, cart_ram_write, gb_err, NULL);
    if (err != GB_INIT_NO_ERROR) {
        ESP_LOGE(TAG, "gb_init failed (%d) — not a Game Boy ROM?", (int)err);
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
        if (!s->cart_ram) { ESP_LOGE(TAG, "no RAM for the %u B cart save", (unsigned)need); session_free(s); return ESP_ERR_NO_MEM; }
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

    S = s;                                  // the APU shims read S, so publish before touching sound

    // Sound is BEST-EFFORT: if the speaker is busy (a track playing, the recorder holding the shared
    // mic pin) the game still runs, silently. Refusing to start a cartridge because audio was taken
    // would be the wrong trade on a games machine.
    minigb_apu_audio_init(&s->apu);
    s->sound = (nucleo_audio_pcm_open(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS) == ESP_OK);
    if (!s->sound) ESP_LOGW(TAG, "speaker unavailable — playing silently");

    ESP_LOGI(TAG, "'%s' %uKB %s | heap %u B | largest was %u B | sound %s",
             s->title, (unsigned)(s->rom_bytes / 1024),
             s->rom_all ? "resident" : (s->page ? "4 KB page cache" : "banked from SD"),
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
    gb_run_frame(&S->gb);
    S->frames++;
    // One frame of sound, produced AFTER the frame that generated it and pushed straight out. The
    // write blocks on the I2S DMA, which is also what paces us to real speed — so a dropped frame
    // shows up as a click rather than as drift.
    if (S->sound) {
        minigb_apu_audio_callback(&S->apu, S->audio);
        nucleo_audio_pcm_write(S->audio, sizeof(S->audio));
    }
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
    out->rom_resident = (S->rom_all != NULL);
    out->rom_paged    = (S->page != NULL);
    out->bank_misses  = S->bank_misses;
    out->frames       = S->frames;
}
