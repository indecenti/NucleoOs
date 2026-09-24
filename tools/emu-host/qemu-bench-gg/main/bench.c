// ggbench — what one emulated Game Gear frame costs the ESP32-S3 core, under QEMU -icount
// (1 instruction = 1 virtual ns, so esp_timer microseconds = thousands of instructions).
// The REAL nucleo_gg.c is compiled in; only its file access is redirected to embedded ROM images.
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "nucleo_audio.h"
#ifndef BENCH_SKIP
#define BENCH_SKIP 0
#endif

esp_err_t nucleo_audio_pcm_open(int rate, int channels) { (void)rate; (void)channels; return ESP_OK; }
esp_err_t nucleo_audio_pcm_write(const int16_t *pcm, size_t bytes) { (void)pcm; (void)bytes; return ESP_OK; }
void nucleo_audio_pcm_close(void) { }
esp_err_t esp_task_wdt_reset(void) { return ESP_OK; }     // the bench disables the task watchdog

static const uint8_t *g_rom; static size_t g_rom_len;
static FILE *bench_fopen(const char *p, const char *mode)
{
    size_t n = strlen(p);
    if (n > 3 && !strcmp(p + n - 3, ".gg") && mode[0] == 'r') return fmemopen((void *)g_rom, g_rom_len, "rb");
    return NULL;                                          // saves, states: no card here
}
#include <sys/stat.h>
static int bench_mkdir(const char *p, mode_t m) { (void)p; (void)m; return 0; }
#define fopen(p, m) bench_fopen(p, m)
#define mkdir bench_mkdir
#include "nucleo_gg.c"
#undef fopen

#define ROM(n) extern const uint8_t _binary_##n##_gg_start[], _binary_##n##_gg_end[];
ROM(sonic) ROM(sonic2) ROM(chaos) ROM(mm2) ROM(ewj) ROM(sor2)
static const struct { const char *name; const uint8_t *s, *e; } roms[] = {
    { "sonic",   _binary_sonic_gg_start,   _binary_sonic_gg_end },
    { "sonic2",  _binary_sonic2_gg_start,  _binary_sonic2_gg_end },
    { "chaos",   _binary_chaos_gg_start,   _binary_chaos_gg_end },
    { "mm2",     _binary_mm2_gg_start,     _binary_mm2_gg_end },
    { "ewj",     _binary_ewj_gg_start,     _binary_ewj_gg_end },
    { "sor2", _binary_sor2_gg_start, _binary_sor2_gg_end },
};
static uint16_t sink[160];
static void on_line(const uint16_t *px, int line, void *user) { (void)line; (void)user; memcpy(sink, px, sizeof sink); }
static uint8_t btn(int f)
{
    if (f < 180) return 0;
    int ph = (f / 20) % 6;
    if (f < 900) return ph == 0 ? NUCLEO_GB_START : ph == 3 ? NUCLEO_GB_A : 0;
    return (uint8_t)(NUCLEO_GB_RIGHT | ((f / 30) % 3 == 0 ? NUCLEO_GB_B : 0) | (ph == 5 ? NUCLEO_GB_A : 0));
}
void app_main(void)
{
    const int FR = 1500, SKIP = 900;          // measure gameplay, not the title screen
    printf("\n@@BENCH gg\n");
    uint64_t tot = 0;
    for (int r = 0; r < sizeof roms / sizeof roms[0]; r++) {
        g_rom = roms[r].s; g_rom_len = roms[r].e - roms[r].s;
        char path[32]; snprintf(path, sizeof path, "/rom/%s.gg", roms[r].name);
        if (nucleo_gg_open(path, on_line, NULL) != ESP_OK) { printf("open fail %s\n", roms[r].name); continue; }
        nucleo_gg_set_frameskip(BENCH_SKIP);      // 1: render every other frame -> isolates the picture's cost
        int64_t cpu = 0, mx = 0;
        for (int f = 0; f < FR; f++) {
            if (f == SKIP) gg_prof_render = gg_prof_spr = gg_prof_map = 0;
            nucleo_gg_set_buttons(btn(f));
            int64_t t0 = esp_timer_get_time();
            nucleo_gg_run_frame();
            int64_t t = esp_timer_get_time() - t0;
            if (f >= SKIP) { cpu += t; if (t > mx) mx = t; }
        }
        nucleo_gb_stats_t st; nucleo_gg_get_stats(&st);
        int n = FR - SKIP;
        (void)st;
        printf("@@ %-8s avg=%lld kinsn/frame  max=%lld | render=%u (bg=%u spr=%u map=%u) cpu+psg=%lld\n", roms[r].name,
               cpu / n, mx, (unsigned)(gg_prof_render / n),
               (unsigned)((gg_prof_render - gg_prof_spr - gg_prof_map) / n), (unsigned)(gg_prof_spr / n),
               (unsigned)(gg_prof_map / n), cpu / n - gg_prof_render / n);
        tot += cpu / (FR - SKIP);
        nucleo_gg_close();
    }
    printf("@@ TOTAL avg=%llu\n@@END\n", tot);
    fflush(stdout);
    for (;;) { }
}
