// The Games gallery's bench (node tools/ui-host/gamefront-bench.mjs): the REAL gamefront.cpp driven the way the
// launcher's loop drives it on the device (a key when one is due, gamefront_step, a render when dirty, 20 ms), through
// a browse of a console: open, rest, scroll, rest, scroll back, a held key. Every card access is counted (the link
// wraps fopen / fread / fgets / stat / opendir / readdir) and every frame pushed to the screen (lg::frame /
// lg::frame_rect and their area), per phase, so a change to the gallery is measured, not guessed:
//   opens (missed)   files opened on the card (a missed one is a directory lookup for nothing)
//   KB read          bytes read from the card
//   stats / dirs     stat() calls / directory listings
//   whole / rects    whole frames / partial frames composed; Kpx = the pixels they pushed
//   ram              stops whose picture came from the RAM cache (the gallery says so on the PC)
//   cpu ms           the PC's time in gamefront_render (-O0: a proxy, the device is ~10-20x slower)
#include <M5GFX.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <chrono>
#include <sys/stat.h>
#include <dirent.h>
#include "host_state.h"
#include "nucleo_kbd.h"
#include "gamefront.h"

void host_display_begin(void);
extern M5GFX d;
extern "C" void nucleo_theme_init(void);
extern "C" bool nucleo_theme_set(const char *);
extern "C" void nucleo_i18n_set_lang(const char *);
extern "C" void nucleo_register_gamefront(void);

struct Io { long opens, misses, writes, reads, lines, stats, dirs, entries, whole, rects, ram; long long bytes, px; double cpu_ms; };
static Io io;

extern "C" {
FILE *__real_fopen(const char *, const char *);
FILE *__wrap_fopen(const char *p, const char *m)
{
    FILE *f = __real_fopen(p, m);
    if (m[0] == 'r') { io.opens++; if (!f) io.misses++; } else io.writes++;
    if (getenv("BENCH_TRACE")) fprintf(stderr, "open %s %s%s\n", m, p, f ? "" : "  MISS");
    return f;
}
size_t __real_fread(void *, size_t, size_t, FILE *);
size_t __wrap_fread(void *b, size_t s, size_t n, FILE *f) { const size_t r = __real_fread(b, s, n, f); io.reads++; io.bytes += (long long)(r * s); return r; }
char *__real_fgets(char *, int, FILE *);
char *__wrap_fgets(char *b, int n, FILE *f) { char *r = __real_fgets(b, n, f); if (r) { io.lines++; io.bytes += (long long)strlen(r); } return r; }
int __real_stat64i32(const char *, struct _stat64i32 *);
int __wrap_stat64i32(const char *p, struct _stat64i32 *s)
{
    io.stats++;
    if (getenv("BENCH_TRACE")) fprintf(stderr, "stat %s\n", p);
    return __real_stat64i32(p, s);
}
DIR *__real_opendir(const char *);
DIR *__wrap_opendir(const char *p) { io.dirs++; if (getenv("BENCH_TRACE")) fprintf(stderr, "opendir %s\n", p); return __real_opendir(p); }
struct dirent *__real_readdir(DIR *);
struct dirent *__wrap_readdir(DIR *d) { struct dirent *e = __real_readdir(d); if (e) io.entries++; return e; }
// lg::frame / lg::frame_rect (app_large.cpp), by their mangled names
bool __real__ZN2lg5frameEPFvPvES0_iib(void (*)(void *), void *, int, int, bool);
bool __wrap__ZN2lg5frameEPFvPvES0_iib(void (*p)(void *), void *u, int y0, int h, bool ex)
{
    io.whole++;
    io.px += (long long)d.width() * (h ? h : d.height() - y0);
    return __real__ZN2lg5frameEPFvPvES0_iib(p, u, y0, h, ex);
}
bool __real__ZN2lg10frame_rectEPFvPvES0_iiiib(void (*)(void *), void *, int, int, int, int, bool);
bool __wrap__ZN2lg10frame_rectEPFvPvES0_iiiib(void (*p)(void *), void *u, int x, int y, int w, int h, bool ex)
{
    io.rects++; io.px += (long long)w * h;
    return __real__ZN2lg10frame_rectEPFvPvES0_iiiib(p, u, x, y, w, h, ex);
}
}

// The device's loop, one 20 ms turn at a time.
static bool s_dirty;
static void loop_turn(int key = NK_NONE, char ch = 0)
{
    if (key != NK_NONE) {
        char id[40];
        const int r = gamefront_key(key, ch, id, sizeof id);
        if (r == GF_REDRAW) s_dirty = true;
    }
    if (gamefront_step()) s_dirty = true;
    if (s_dirty) {
        s_dirty = false;
        const auto t0 = std::chrono::steady_clock::now();
        gamefront_render();
        io.cpu_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    g_host.now_us += 20000;
}
static void idle(int ms) { for (int t = 0; t < ms; t += 20) loop_turn(); }
static void keys(int key, int n, int every_ms)          // n presses, one every every_ms
{
    for (int i = 0; i < n; i++) { loop_turn(key, key == NK_DOWN ? '.' : key == NK_UP ? ';' : 0); idle(every_ms - 20); }
}
static void phase(const char *name)
{
    fprintf(stderr, "PHASE %s %ld %ld %ld %lld %ld %ld %ld %ld %lld %.1f\n", name, io.opens, io.misses, io.writes, io.bytes,
            io.stats, io.dirs, io.whole, io.rects, io.px, io.cpu_ms);
    fflush(stderr);
    memset(&io, 0, sizeof io);
}

int main(int argc, char **argv)
{
    _putenv("TZ=UTC0"); _tzset();
    g_host.large = argc > 1 && !strcmp(argv[1], "large");
    g_host.two_screens = g_host.large;
    if (argc > 2 && !strcmp(argv[2], "nocanvas")) g_host.canvas_ok = false;   // the ADV with no 32 KB canvas: no cache, no scratch
    if (argc > 2 && !strcmp(argv[2], "tight")) g_host.largest_block = 9000;     // a fragmented heap: a small arena
    host_display_begin();
    nucleo_theme_init();
    nucleo_theme_set("classic");
    nucleo_i18n_set_lang("en");
    nucleo_register_gamefront();                    // the registry of gamefront_host.cpp
    memset(&io, 0, sizeof io);
    gamefront_open();
    while (gamefront_scanning()) gamefront_step();  // the first walk of the card, all at once
    gamefront_invalidate(); loop_turn(); s_dirty = true; loop_turn();
    phase("open");
    idle(3000);                                     phase("rest-systems");
    keys(NK_DOWN, 4, 300);                          phase("to-gameboy");
    loop_turn(NK_ENTER);                            phase("enter");
    idle(2500);                                     phase("rest");
    keys(NK_DOWN, 10, 180);                         phase("scroll-10");
    idle(2500);                                     phase("rest");
    keys(NK_UP, 10, 180);                           phase("back-10");
    idle(2500);                                     phase("rest");
    keys(NK_DOWN, 20, 60);                          phase("held-20");
    idle(2500);                                     phase("rest");
    loop_turn(NK_CHAR, 'l');                        phase("type-l");
    loop_turn(NK_CHAR, 'o');                        phase("type-o");
    loop_turn(NK_BACK);                             phase("esc");
    // Another visit, the card as the first one left it (its screenshots made): the everyday browse.
    loop_turn(NK_BACK); loop_turn(NK_BACK);         // out of the console, out of the gallery
    gamefront_open(); s_dirty = true; loop_turn();  phase("reopen");
    idle(1500);                                     phase("rest");
    loop_turn(NK_ENTER);                            phase("enter");   // the same console: the selection stayed on it
    idle(2500);                                     phase("rest");
    keys(NK_DOWN, 10, 180);                         phase("scroll-10");
    idle(2500);                                     phase("rest");
    keys(NK_UP, 10, 180);                           phase("back-10");
    return 0;
}
