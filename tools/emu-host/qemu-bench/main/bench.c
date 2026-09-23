// gbbench — count what one emulated Game Boy frame costs on the ESP32-S3 core, under QEMU -icount
// (1 instruction = 1 virtual ns, so esp_timer microseconds = thousands of instructions).
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "esp_attr.h"
#define ENABLE_LCD 1
#define ENABLE_SOUND 1
#define PEANUT_GB_HIGH_LCD_ACCURACY HILCD
#define PEANUT_GB_12_COLOUR 0
#include "minigb_apu.h"
static struct minigb_apu_ctx APU;
static uint8_t audio_read(const uint16_t a) { return minigb_apu_audio_read(&APU, a); }
static void audio_write(const uint16_t a, const uint8_t v) { minigb_apu_audio_write(&APU, a, v); }
#include CORE_H

#define ROM(n) extern const uint8_t _binary_##n##_gb_start[], _binary_##n##_gb_end[];
ROM(tetris) ROM(kirby) ROM(zelda) ROM(sf2) ROM(tetris2) ROM(mm5)
static const struct { const char *name; const uint8_t *s, *e; } roms[] = {
    { "tetris",  _binary_tetris_gb_start,  _binary_tetris_gb_end },
    { "kirby",   _binary_kirby_gb_start,   _binary_kirby_gb_end },
    { "zelda",   _binary_zelda_gb_start,   _binary_zelda_gb_end },
    { "sf2",     _binary_sf2_gb_start,     _binary_sf2_gb_end },
    { "tetris2", _binary_tetris2_gb_start, _binary_tetris2_gb_end },
    { "mm5",     _binary_mm5_gb_start,     _binary_mm5_gb_end },
};
static const uint8_t *rom; static uint32_t rom_len;
static uint32_t cur_tag = 0xFFFFFFFF; static const uint8_t *cur_buf;
static uint8_t cram[32768];
static uint16_t row[160];
static const uint16_t pal[4] = { 0xE19D, 0xE27C, 0x8543, 0x231A };
static audio_sample_t abuf[AUDIO_SAMPLES_TOTAL];

// Same shape as nucleo_gb.c's hit path: bounds, one-compare page test, indexed load.
static IRAM_ATTR uint8_t rom_read(struct gb_s *gb, const uint_fast32_t addr)
{
    if (addr >= rom_len) return 0xFF;
    uint32_t tag = (uint32_t)(addr >> 10);
    if (tag == cur_tag) return cur_buf[addr & 1023];
    cur_tag = tag; cur_buf = rom + (tag << 10);
    return cur_buf[addr & 1023];
}
static IRAM_ATTR uint8_t rr(struct gb_s *gb, const uint_fast32_t a) { return a < sizeof cram ? cram[a] : 0xFF; }
static IRAM_ATTR void rw(struct gb_s *gb, const uint_fast32_t a, const uint8_t v) { if (a < sizeof cram) cram[a] = v; }
static void er(struct gb_s *gb, const enum gb_error_e e, const uint16_t a) { }
static IRAM_ATTR void ln(struct gb_s *gb, const uint8_t *px, const uint_fast8_t line)
{
    for (int x = 0; x < 160; x++) row[x] = pal[px[x] & 3];
}
static uint8_t btn(int f)
{
    int ph = (f / 20) % 4;
    if (f < 120) return 0xFF;
    if (ph == 0) return (uint8_t)~JOYPAD_START;
    if (ph == 2) return (uint8_t)~JOYPAD_A;
    return 0xFF;
}
static struct gb_s gb;
void app_main(void)
{
    const int FR = 600;
    printf("\n@@BENCH core=%s hilcd=%d\n", CORE_H, HILCD);
    uint64_t tot = 0;
    for (int r = 0; r < sizeof roms / sizeof roms[0]; r++) {
        rom = roms[r].s; rom_len = roms[r].e - roms[r].s; cur_tag = 0xFFFFFFFF;
        memset(cram, 0, sizeof cram);
        minigb_apu_audio_init(&APU);
        if (gb_init(&gb, rom_read, rr, rw, er, NULL) != GB_INIT_NO_ERROR) { printf("init fail %s\n", roms[r].name); continue; }
        gb_init_lcd(&gb, ln);
        int64_t cpu = 0, aud = 0;
        for (int f = 0; f < FR; f++) {
            gb.direct.joypad = btn(f);
            int64_t t0 = esp_timer_get_time();
            gb_run_frame(&gb);
            int64_t t1 = esp_timer_get_time();
            minigb_apu_audio_callback(&APU, abuf);
            int64_t t2 = esp_timer_get_time();
            if (f >= 120) { cpu += t1 - t0; aud += t2 - t1; }
        }
        printf("@@ %-8s cpu=%lld kinsn/frame  apu=%lld kinsn/frame\n", roms[r].name, cpu / (FR - 120), aud / (FR - 120));
        tot += cpu / (FR - 120);
    }
    printf("@@ TOTAL cpu=%llu\n@@END\n", tot);
    fflush(stdout);
    for (;;) { }
}
