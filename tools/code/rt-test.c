// rt-test.c: host gate for Nucleo Code's runtime (firmware/components/nucleo_code/code_rt.c, the code the
// device runs): output capture, errors with file:line, the time limit, out-of-memory, workspace
// confinement, and the memory typical programs need. Built and run by tools/code/rt-check.mjs.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "code_rt.h"
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
static int64_t now_ms(void) { return (int64_t)GetTickCount64(); }
static void sleep_ms(uint32_t ms) { Sleep(ms); }
#define MKDIR(p) _mkdir(p)
#else
#include <unistd.h>
static int64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static void sleep_ms(uint32_t ms) { usleep(ms * 1000); }
#define MKDIR(p) mkdir(p, 0777)
#endif

static char g_out[8192]; static size_t g_len;
static void out(void *op, const char *s, size_t n) { (void)op; if (g_len + n < sizeof g_out) { memcpy(g_out + g_len, s, n); g_len += n; g_out[g_len] = 0; } }
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static code_result_t run(size_t arena_kb, const char *src, uint32_t timeout)
{
    static code_env_t env;
    env.root = "build/code-rt/ws"; env.out = out; env.op = NULL; env.now_ms = now_ms; env.sleep_ms = sleep_ms; env.cancel = NULL; env.timeout_ms = timeout;
    void *arena = malloc(arena_kb * 1024);
    code_result_t r; g_len = 0; g_out[0] = 0;
    code_run(arena, arena_kb * 1024, src, strlen(src), "main.js", &env, &r);
    free(arena);
    return r;
}

// A run with a screen: rows blitted are counted, keys come from a list, the cancel fires after 400 ms.
static int g_blit_rows, g_blit_maxw; static uint16_t g_first_px; static int g_kpos; static const char **g_keys; static int64_t g_t0;
static void blit(void *op, int x, int y, int w, int h, const uint16_t *px)
{
    (void)op; (void)x;
    if (y == 0 && !g_blit_rows) g_first_px = px[0];
    g_blit_rows += h; if (w > g_blit_maxw) g_blit_maxw = w;
}
static const char *key(void *op) { (void)op; return g_keys && g_keys[g_kpos] ? g_keys[g_kpos++] : NULL; }
static bool cancel_late(void *op) { (void)op; return now_ms() - g_t0 > 400; }
static code_result_t run_screen(int w, int h, const char *src, uint32_t timeout, const char **keys, const char *shot)
{
    static code_env_t env;
    memset(&env, 0, sizeof env);
    env.root = "build/code-rt/ws"; env.out = out; env.now_ms = now_ms; env.sleep_ms = sleep_ms; env.timeout_ms = timeout;
    env.scr_w = w; env.scr_h = h; env.blit = blit; env.key = key; env.shot = shot;
    env.cancel = timeout == CODE_NO_LIMIT ? cancel_late : NULL;
    g_blit_rows = g_blit_maxw = 0; g_first_px = 0; g_keys = keys; g_kpos = 0; g_t0 = now_ms();
    void *arena = malloc(32 * 1024);
    code_result_t r; g_len = 0; g_out[0] = 0;
    code_run(arena, 32 * 1024, src, strlen(src), "app.js", &env, &r);
    free(arena);
    return r;
}

// Programs like the ones the agent writes (a few hundred lines at most), to size the default arena.
static const char *P_FIZZ = "for (var i = 1; i <= 30; i++) print(i % 15 == 0 ? 'FizzBuzz' : i % 3 == 0 ? 'Fizz' : i % 5 == 0 ? 'Buzz' : i);";
static const char *P_SORT =
    "function qs(a){ if (a.length < 2) return a; var p = a[0], l = [], r = [];"
    " for (var i = 1; i < a.length; i++) (a[i] < p ? l : r).push(a[i]); return qs(l).concat([p], qs(r)); }"
    "var a = []; for (var i = 0; i < 400; i++) a.push((i * 7919) % 1000);"
    "var s = qs(a); print(s[0], s[399], s.length);";
static const char *P_TEXT =
    "var words = {}; var t = 'the quick brown fox jumps over the lazy dog the fox the end';"
    "for (var k = 0; k < 50; k++) { var w = t.split(' '); for (var i = 0; i < w.length; i++) words[w[i]] = (words[w[i]] || 0) + 1; }"
    "var keys = Object.keys(words).sort(function(a,b){ return words[b] - words[a]; });"
    "print(keys[0], words[keys[0]]); print(JSON.stringify({top: keys.slice(0,3)}));";
static const char *P_GAME =
    "var W = 30, H = 12, grid = []; for (var y = 0; y < H; y++) { var row = []; for (var x = 0; x < W; x++) row.push(((x*3+y*5)%7)==0 ? 1 : 0); grid.push(row); }"
    "function step(g){ var n = []; for (var y = 0; y < H; y++){ var r = []; for (var x = 0; x < W; x++){ var c = 0;"
    " for (var dy = -1; dy <= 1; dy++) for (var dx = -1; dx <= 1; dx++) if (dx||dy) c += g[(y+dy+H)%H][(x+dx+W)%W];"
    " r.push(c == 3 || (c == 2 && g[y][x]) ? 1 : 0); } n.push(r); } return n; }"
    "for (var s = 0; s < 20; s++) grid = step(grid); var alive = 0; for (var y = 0; y < H; y++) for (var x = 0; x < W; x++) alive += grid[y][x]; print('alive', alive);";

int main(void)
{
    MKDIR("build"); MKDIR("build/code-rt"); MKDIR("build/code-rt/ws");

    code_result_t r = run(24, "print('ciao', 1 + 2, [1,2], {a:1}); console.log('ok');", 2000);
    CHECK(r.ok, "hello: %s", r.error);
    CHECK(strstr(g_out, "ciao 3") && strstr(g_out, "ok\n"), "hello output: [%s]", g_out);

    r = run(24, "var a = 1;\nvar b = 2;\nnull.x;\n", 2000);
    CHECK(!r.ok && strstr(r.error, "TypeError"), "error kind: %s", r.error);
    CHECK(!strcmp(r.where, "main.js:3"), "error where: [%s]", r.where);

    r = run(24, "var x = 0; while (true) x++;", 300);
    CHECK(!r.ok && r.timeout && r.ms < 1500, "timeout: ok=%d timeout=%d ms=%u", r.ok, r.timeout, r.ms);

    r = run(16, "var a = []; while (true) a.push('xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx' + a.length);", 5000);
    CHECK(!r.ok && r.oom, "oom: ok=%d oom=%d err=%s", r.ok, r.oom, r.error);

    r = run(24, "print(fs.write('a.txt', 'riga1\\n')); fs.append('a.txt', 'riga2'); print(fs.read('a.txt')); print(fs.list('.').length >= 1); print(fs.exists('a.txt'));", 2000);
    CHECK(r.ok && strstr(g_out, "true\nriga1\nriga2\ntrue\ntrue"), "fs: %s [%s]", r.error, g_out);
    r = run(24, "fs.read('../secret.txt');", 2000);
    CHECK(!r.ok && strstr(r.error, "outside the workspace"), "confine ..: %s", r.error);
    r = run(24, "fs.read('/system/x');", 2000);
    CHECK(!r.ok, "confine abs: %s", r.error);

    r = run(24, "var t = Date.now(); sys.sleep(60); print(Date.now() - t >= 50); print(sys.mem().used > 0);", 2000);
    CHECK(r.ok && strstr(g_out, "true\ntrue"), "sys: %s [%s]", r.error, g_out);

    // Memory: the smallest arena each program runs in, and what is live at most.
    const struct { const char *name, *src; } P[] = { { "fizzbuzz", P_FIZZ }, { "quicksort400", P_SORT }, { "wordcount", P_TEXT }, { "life30x12", P_GAME } };
    printf("program        min-arena  live-max  ms\n");
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++) {
        size_t lo = 0;
        for (size_t kb = 8; kb <= 96; kb += 2) { r = run(kb, P[i].src, 5000); if (r.ok) { lo = kb; break; } }
        code_result_t m = run(96, P[i].src, 5000);
        printf("%-14s %6u KB  %6u B  %u\n", P[i].name, (unsigned)lo, (unsigned)m.live_max, m.ms);
        CHECK(lo && lo <= 64, "%s needs more than 64 KB on the host", P[i].name);
    }
    // the model's markdown made plain for the device's fonts
    {
        char md[512];
        code_md_plain("# Fatto\nHo scritto **calc.js** con `evaluate()`.\n```js\nprint(1)\n```\nVedi [la guida](http://x).", md, sizeof md);
        CHECK(!strcmp(md, "Fatto\nHo scritto calc.js con evaluate().\n  print(1)\nVedi la guida."), "md_plain: [%s]", md);
        code_md_plain("**a** __b__ c", md, 4);   // bounded
        CHECK(strlen(md) < 4, "md_plain bound: [%s]", md);
    }
    // the compatibility chunks load only when named, and work
    r = run(24, "print('ab'.includes('b'), [1,2].find(function(v){return v>1}), Object.values({a:7})[0], '5'.padStart(2,'0'), new Map([['k',1]]).get('k'));", 2000);
    CHECK(r.ok && strstr(g_out, "true 2 7 05 1"), "compat: %s [%s]", r.error, g_out);
    r = run(24, "gc(); print(sys.mem().used);", 2000);
    { int plain = atoi(g_out); CHECK(plain > 0 && plain < 6000, "no compat cost for a plain script: %d", plain); }
    // the screen (docs/nucleo-code.md 4.5): no screen -> a clear error; a scene -> rows blitted and a PNG (rt-check.mjs
    // decodes it and checks its pixels); scripted keys; an app loop with no time limit, stopped by the cancel
    r = run(24, "gfx.clear(0);", 2000);
    CHECK(!r.ok && strstr(r.error, "no screen"), "gfx without a screen: %s", r.error);
    r = run_screen(240, 135, "var W = gfx.width(), H = gfx.height(); gfx.clear(gfx.DARKBLUE); gfx.rect(0, 0, 20, 20, gfx.RED);"
                   "gfx.frame(30, 10, 40, 30, gfx.WHITE); gfx.line(0, H - 1, W - 1, 0, gfx.YELLOW); gfx.disc(120, 70, 15, gfx.GREEN);"
                   "gfx.circle(120, 70, 25, gfx.CYAN); var w = gfx.text('Ciao!', 150, 100, gfx.WHITE, 2); gfx.pixel(239, 134, gfx.PINK);"
                   "print(W, H, w, gfx.get(5, 5), gfx.get(239, 134), gfx.get(-1, 0)); gfx.flush();", 2000, NULL, "build/code-rt/shot.png");
    CHECK(r.ok && strstr(g_out, "240 135 60 4 13 -1"), "gfx scene: %s [%s]", r.error, g_out);
    CHECK(r.drew && r.shot_ok && r.frames == 1 && r.scr_w == 240, "gfx result: drew=%d shot=%d frames=%u", r.drew, r.shot_ok, (unsigned)r.frames);
    CHECK(g_blit_rows >= 135 && g_blit_maxw == 240, "blit: %d rows, width %d", g_blit_rows, g_blit_maxw);
    CHECK(g_first_px == 0xE1C7, "rgb565 of red at (0,0): %04x", (unsigned)g_first_px);
    static const char *KEYS[] = { "right", "right", "enter", "a", NULL };
    r = run_screen(240, 135, "var k, seen = []; while ((k = key.get()) !== null) seen.push(k); print(seen.join(','), key.down('a'), touch.poll());",
                   2000, KEYS, NULL);
    CHECK(r.ok && strstr(g_out, "right,right,enter,a false false"), "keys: %s [%s]", r.error, g_out);
    r = run_screen(320, 240, "var x = 0; while (true) { gfx.clear(0); gfx.rect(x % 300, 100, 20, 20, gfx.ORANGE); gfx.flush(); x += 4; sys.sleep(5); }",
                   CODE_NO_LIMIT, NULL, "build/code-rt/shot-large.png");
    CHECK(!r.ok && r.cancelled && r.drew && r.frames > 3 && r.shot_ok && r.scr_w == 320, "app loop: cancelled=%d frames=%u shot=%d %s",
          r.cancelled, (unsigned)r.frames, r.shot_ok, r.error);
    // an app loop is stopped on time even though it lives in C (flush + sleep): the 300 ms limit, not seconds later
    r = run_screen(240, 135, "while (true) { gfx.clear(1); gfx.flush(); sys.sleep(20); }", 300, NULL, NULL);
    CHECK(r.timeout && r.drew && r.ms < 450, "app loop limit: timeout=%d ms=%u", r.timeout, (unsigned)r.ms);
    r = run_screen(240, 135, "while (true) { try { gfx.flush(); sys.sleep(10); } catch (e1) { print('caught'); } }", 300, NULL, NULL);
    CHECK(r.timeout && !strstr(g_out, "caught"), "the stop is not catchable: [%s]", g_out);
    printf(fails ? "FAILED %d\n" : "ALL OK\n", fails);
    return fails != 0;
}
