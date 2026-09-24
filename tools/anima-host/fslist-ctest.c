// Host test for the streaming /api/fs/list body (firmware/components/nucleo_fsapi/fslist.c).
// Unity build: includes the REAL fslist.c so its static helpers (JSON escaping) are testable
// too. Run from an empty scratch dir (fslist-check.mjs does): builds a fixture tree under ./sd
// (the host shim mounts the SD at "./sd"), streams listings to ./out/*.json for the node side to
// validate structurally, and self-checks what only C can see:
//   * a 16-byte chunk buffer yields byte-identical output to a 1 KB one (flush boundaries);
//   * a sink that fails mid-stream stops the stream and reports false;
//   * names are escaped exactly like cJSON (quotes, backslash, controls, raw UTF-8).
#include "fslist.c"
#include <sys/stat.h>
#include <direct.h>

static int passed, failed;
#define CHECK(cond, msg) do { if (cond) passed++; else { failed++; printf("  FAIL %s\n", msg); } } while (0)

typedef struct { char *p; size_t n, cap; size_t fail_after; } cap_t;

static bool cap_sink(void *ctx, const char *d, size_t len)
{
    cap_t *c = ctx;
    if (c->fail_after && c->n + len > c->fail_after) return false;
    if (c->n + len + 1 > c->cap) { c->cap = (c->n + len + 1) * 2; c->p = realloc(c->p, c->cap); }
    memcpy(c->p + c->n, d, len); c->n += len; c->p[c->n] = 0;
    return true;
}

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) { printf("  FAIL cannot create %s\n", path); failed++; return; }
    fputs(text, f); fclose(f);
}
static void put_n(const char *path, int n) { FILE *f = fopen(path, "wb"); if (f) { for (int i = 0; i < n; i++) fputc('x', f); fclose(f); } }
static void mk(const char *p) { _mkdir(p); }

// Stream `abs` with a `cap`-byte buffer into a capture; returns the stream's result.
static bool list_to(const char *abs, size_t cap, cap_t *out)
{
    DIR *d = opendir(abs);
    if (!d) return false;
    char *buf = malloc(cap);
    bool ok = fslist_stream(d, abs, buf, cap, cap_sink, out);
    free(buf); closedir(d);
    return ok;
}

static void dump(const char *abs, const char *name)
{
    cap_t big = {0}, tiny = {0};
    bool ok1 = list_to(abs, 1024, &big), ok2 = list_to(abs, 16, &tiny);
    char msg[160];
    snprintf(msg, sizeof msg, "%s: stream ok", name);                         CHECK(ok1 && ok2, msg);
    snprintf(msg, sizeof msg, "%s: 16 B chunks == 1 KB chunks", name);        CHECK(big.n == tiny.n && big.p && tiny.p && !memcmp(big.p, tiny.p, big.n), msg);
    char path[200]; snprintf(path, sizeof path, "out/%s.json", name);
    FILE *f = fopen(path, "wb"); if (f) { fwrite(big.p, 1, big.n, f); fclose(f); }
    free(big.p); free(tiny.p);
}

int main(void)
{
    mk("sd"); mk("sd/data"); mk("sd/data/ROMs"); mk("sd/data/ROMs/gb"); mk("sd/apps"); mk("sd/apps/demo");
    mk("sd/data/music"); mk("out");

    // A bundled-games leaf: manifest names (case/blank/comment/CRLF variants), a user import,
    // a listed folder with a nested folder, an unlisted folder with only a file.
    put("sd/data/ROMs/gb/.factory", "Tetris.gb\n  Zelda.GB \r\n# comment line\n\nSub\n");
    put_n("sd/data/ROMs/gb/Tetris.gb", 10);
    put_n("sd/data/ROMs/gb/zelda.gb", 20);
    put_n("sd/data/ROMs/gb/User.gb", 5);
    mk("sd/data/ROMs/gb/Sub"); mk("sd/data/ROMs/gb/Sub/inner");
    mk("sd/data/ROMs/gb/Empty"); put_n("sd/data/ROMs/gb/Empty/x.txt", 1);

    // A big protected folder: the case that used to 503 on the device.
    for (int i = 0; i < 300; i++) { char p[64]; snprintf(p, sizeof p, "sd/apps/demo/f%03d.js", i); put_n(p, i); }

    // User media with non-ASCII / punctuation names (raw UTF-8 must pass through).
    put_n("sd/data/music/canzone \xc3\xa8 bella.mp3", 7);
    put_n("sd/data/music/a&b 'x' [1].txt", 3);

    dump(NUCLEO_SD_MOUNT "/data/ROMs/gb", "roms");
    dump(NUCLEO_SD_MOUNT "/apps/demo", "apps");
    dump(NUCLEO_SD_MOUNT "/data/music", "music");

    // Sink failure mid-stream: stops, reports false, never writes past the failure.
    cap_t bad = { .fail_after = 200 };
    CHECK(!list_to(NUCLEO_SD_MOUNT "/apps/demo", 64, &bad), "failing sink -> stream returns false");
    CHECK(bad.n <= 200, "failing sink -> nothing accepted past the failure");
    free(bad.p);

    // Escaping, exactly cJSON's table.
    cap_t esc = {0};
    char eb[8];
    fsl_out_t o = { eb, sizeof eb, 0, cap_sink, &esc, true };
    out_json(&o, "a\"b\\c\n\t\x01\x1f\xc3\xa8z");
    out_flush(&o);
    const char *want = "a\\\"b\\\\c\\n\\t\\u0001\\u001f\xc3\xa8z";
    CHECK(esc.p && !strcmp(esc.p, want), "JSON escaping matches cJSON");
    free(esc.p);

    printf("fslist: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
