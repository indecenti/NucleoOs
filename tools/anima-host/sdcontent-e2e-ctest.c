// End-to-end host test of the SD-content installer ENGINE (firmware/components/nucleo_sdcontent/sdc_engine.c,
// the exact code the device runs). A fake "GitHub Pages" serves a fixture tree over the same get() contract
// the device transport implements, and models the device's TLS limit: a response body over MAX_RESP bytes
// fails like the Cardputer's 8 KB mbedTLS record buffer does (the bug that shipped: a plain GET died right
// after HTTP 200). The SD is a temp directory with user files that must never change.
// Run by tools/anima-host/sdcontent-e2e.mjs (`npm run sdcontent:e2e`, part of the ANIMA gate).
#include "sdc_engine.h"
#include "sdc_sha256.h"
#include "content_policy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir((p), 0775)
#endif

static int checks = 0, fails = 0;
#define OK(c, m) do { checks++; if (!(c)) { fails++; printf("  FAIL: %s  (%s:%d)\n", m, __FILE__, __LINE__); } } while (0)

#define W        "build/sdc-e2e"
#define SD       W "/sd"
#define SRV      W "/srv"        // the good release
#define SRV_TAG  W "/srv-tag"    // a manifest for another firmware
#define SRV_BAD  W "/srv-bad"    // a manifest with a hostile path
#define PREFIX   "fake://pages/sd/0.5.0/"
#define WINDOW   6144u
#define MAX_RESP 7168u           // device model: 8192-byte TLS record cap minus ~1 KB of GitHub headers

// ---- fs helpers --------------------------------------------------------------------------------------
static void mkdir_p(const char *path)
{
    char t[512]; snprintf(t, sizeof t, "%s", path);
    for (char *p = t + 1; *p; p++) if (*p == '/') { *p = 0; MKDIR(t); *p = '/'; }
    MKDIR(t);
}
static void put(const char *root, const char *rel, const void *data, size_t n)
{
    char full[512]; snprintf(full, sizeof full, "%s/%s", root, rel);
    char dir[512]; snprintf(dir, sizeof dir, "%s", full); char *s = strrchr(dir, '/'); if (s) { *s = 0; mkdir_p(dir); }
    FILE *f = fopen(full, "wb"); if (!f) { printf("cannot write %s\n", full); exit(2); }
    if (n) fwrite(data, 1, n, f);
    fclose(f);
}
static long slurp(const char *path, uint8_t **out)
{
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc(n ? (size_t)n : 1); if (n) fread(b, 1, (size_t)n, f); fclose(f);
    *out = b; return n;
}
static bool exists(const char *path) { struct stat sb; return stat(path, &sb) == 0; }
static bool same_file(const char *a, const char *b)
{
    uint8_t *x, *y; long nx = slurp(a, &x), ny = slurp(b, &y);
    bool eq = nx >= 0 && nx == ny && (nx == 0 || memcmp(x, y, (size_t)nx) == 0);
    if (nx >= 0) free(x);
    if (ny >= 0) free(y);
    return eq;
}
static bool file_is(const char *path, const char *text)
{
    uint8_t *x; long n = slurp(path, &x); if (n < 0) return false;
    bool eq = (size_t)n == strlen(text) && memcmp(x, text, (size_t)n) == 0; free(x); return eq;
}
static void rm_rf(const char *path)
{
    struct stat sb; if (stat(path, &sb) != 0) return;
    if (S_ISDIR(sb.st_mode)) {
        DIR *d = opendir(path); struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char c[512]; snprintf(c, sizeof c, "%s/%s", path, e->d_name); rm_rf(c);
        }
        if (d) closedir(d);
        rmdir(path);
    } else unlink(path);
}
static int count_parts(const char *path)   // leftover *.part files anywhere under path
{
    struct stat sb; if (stat(path, &sb) != 0) return 0;
    if (!S_ISDIR(sb.st_mode)) { size_t l = strlen(path); return l > 5 && !strcmp(path + l - 5, ".part"); }
    int n = 0; DIR *d = opendir(path); struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[512]; snprintf(c, sizeof c, "%s/%s", path, e->d_name); n += count_parts(c);
    }
    if (d) closedir(d);
    return n;
}
static void sha_hex_of(const void *d, size_t n, char hex[65])
{ sdc_sha256_t c; sdc_sha256_init(&c); sdc_sha256_update(&c, d, n); uint8_t g[32]; sdc_sha256_final(&c, g); sdc_sha256_hex(g, hex); }

// ---- the fixture release -----------------------------------------------------------------------------
typedef struct { const char *path, *pack; char mode; size_t size; uint32_t seed; const char *text; } fx_t;
static const fx_t FX[] = {
    { "www/shell/index.html",                 "core",   'w', 0,     0, "<html>NucleoOS</html>\n" },
    { "www/shell/app.js",                     "core",   'w', 20000, 11, NULL },   // 4 windows
    { "apps/calc/www/app.js",                 "core",   'w', 12288, 22, NULL },   // exactly 2 windows
    { "data/anima/anima-it-akb5.bin",         "core",   'w', 70001, 33, NULL },   // 12 windows, odd tail
    { "data/anima/learned/facets.it.jsonl",   "core",   'c', 0,     0, "RELEASE-SEED-IT\n" },  // user copy exists
    { "data/anima/learned/facets.en.jsonl",   "core",   'c', 0,     0, "RELEASE-SEED-EN\n" },  // absent -> created
    { "system/registry/apps.json",            "core",   'm', 0,     0, "{\"installed\":[\"release\"]}\n" },
    { "wallpapers/empty.txt",                 "core",   'w', 0,     0, "" },      // 0 bytes
    { "www/shell/a b#c.txt",                  "core",   'w', 0,     0, "needs url encoding\n" },
    { "apps/arcade/www/core.wasm",            "arcade", 'w', 9000,  44, NULL },   // optional pack
};
#define NFX ((int)(sizeof FX / sizeof FX[0]))

static size_t fx_bytes(const fx_t *f, uint8_t *out)
{
    if (f->text) { size_t n = strlen(f->text); memcpy(out, f->text, n); return n; }
    uint32_t x = f->seed * 2654435761u + 1;
    for (size_t i = 0; i < f->size; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; out[i] = (uint8_t)x; }
    return f->size;
}

static void write_release(const char *srv, const char *tag, const char *extra_line)
{
    rm_rf(srv); mkdir_p(srv);
    static uint8_t buf[80000];
    char man[8192]; size_t m = 0; uint64_t total = 0;
    char lines[4096]; size_t ln = 0;
    for (int i = 0; i < NFX; i++) {
        size_t n = fx_bytes(&FX[i], buf);
        put(srv, FX[i].path, buf, n);
        char hex[65]; sha_hex_of(buf, n, hex);
        ln += (size_t)snprintf(lines + ln, sizeof lines - ln, "%s %u %s %c %s\n", hex, (unsigned)n, FX[i].pack, FX[i].mode, FX[i].path);
        total += n;
    }
    if (extra_line) ln += (size_t)snprintf(lines + ln, sizeof lines - ln, "%s\n", extra_line);
    m += (size_t)snprintf(man + m, sizeof man - m, "#nucleoos-sd 1 %s %d %llu\n#pack arcade 1 9000\n#pack core %d %llu\n",
                          tag, NFX, (unsigned long long)total, NFX - 1, (unsigned long long)(total - 9000));
    m += (size_t)snprintf(man + m, sizeof man - m, "%s", lines);
    put(srv, "sd-manifest.txt", man, m);
}

// ---- the fake server ---------------------------------------------------------------------------------
typedef struct {
    const char *srv;
    int requests, fail_every, injected, corrupt_left, max_len;
    const char *corrupt_path;
    const char *notfound_path;     // 404 this one path
    const char *cut_path;          // the link dies for this path from cut_off on (every retry)
    uint32_t cut_off;
    bool manifest_404;
} fake_t;

static void url_decode(const char *in, char *out, size_t cap)
{
    size_t k = 0;
    for (const char *p = in; *p && k + 1 < cap; p++) {
        if (*p == '%' && p[1] && p[2]) { char h[3] = { p[1], p[2], 0 }; out[k++] = (char)strtol(h, NULL, 16); p += 2; }
        else out[k++] = *p;
    }
    out[k] = 0;
}

static int fake_get(void *ctx, const char *url, uint32_t off, uint32_t len, uint8_t *buf, int *status)
{
    fake_t *f = (fake_t *)ctx;
    f->requests++;
    if ((int)len > f->max_len) f->max_len = (int)len;
    *status = 0;
    if (f->fail_every && f->requests % f->fail_every == 0) { f->injected++; return -1; }   // dropped connection
    size_t pl = strlen(PREFIX);
    if (strncmp(url, PREFIX, pl) != 0) { *status = 404; return -1; }
    char rel[400]; url_decode(url + pl, rel, sizeof rel);
    if (f->manifest_404 && !strcmp(rel, "sd-manifest.txt")) { *status = 404; return -1; }
    if (f->notfound_path && !strcmp(rel, f->notfound_path)) { *status = 404; return -1; }
    if (f->cut_path && !strcmp(rel, f->cut_path) && off >= f->cut_off) { *status = 0; return -1; }
    char full[512]; snprintf(full, sizeof full, "%s/%s", f->srv, rel);
    uint8_t *data; long size = slurp(full, &data);
    if (size < 0) { *status = 404; return -1; }
    if ((long)off >= size) { free(data); *status = 416; return -1; }
    uint32_t n = (uint32_t)(size - (long)off); if (n > len) n = len;
    if (n > MAX_RESP) { free(data); *status = 0; return -1; }      // the device's TLS layer dies on this response
    memcpy(buf, data + off, n); free(data);
    if (f->corrupt_path && !strcmp(rel, f->corrupt_path) && f->corrupt_left != 0 && n > 0) {
        buf[n / 2] ^= 0x5a; if (f->corrupt_left > 0) f->corrupt_left--;
    }
    *status = 206;
    return (int)n;
}

// What the progress screen sees: the percentage must never go back, must move INSIDE a big file (not only
// between files), and kb_done must stay within kb_total.
static struct { int calls, last_pct, mid_file, inflight; bool monotonic, bounded; char last_cur[40]; } PR;
static void prog_rec(void *ctx, const sdc_state_t *st)
{
    (void)ctx;
    PR.calls++;
    if (st->phase != SDC_DOWNLOADING && st->phase != SDC_DONE) return;
    if (st->pct >= 0) { if (st->pct < PR.last_pct) PR.monotonic = false; PR.last_pct = st->pct; }
    if (st->kb_done > st->kb_total) PR.bounded = false;
    if (st->phase == SDC_DOWNLOADING && !strcmp(st->cur, PR.last_cur)) PR.inflight++;   // a repeat = mid-file update
    if (st->phase == SDC_DOWNLOADING && st->pct > 0 && st->pct < 100) PR.mid_file++;
    snprintf(PR.last_cur, sizeof PR.last_cur, "%s", st->cur);
}
static bool run(fake_t *f, const char *root, uint32_t window, const char *packs, sdc_state_t *st)
{
    sdc_cfg_t cfg = { .root = root, .base_url = PREFIX, .ver3 = "0.5.0", .packs = packs, .window = window, .retries = 3 };
    memset(&PR, 0, sizeof PR); PR.monotonic = PR.bounded = true;
    sdc_io_t io = { .ctx = f, .get = fake_get, .progress = prog_rec, .tick = NULL };
    return sdc_engine_run(&cfg, &io, st);
}

static void fresh_sd(void)
{
    rm_rf(SD); mkdir_p(SD);
    put(SD, "system/config/setup.json", "USER-SETUP", 10);
    put(SD, "data/Documents/note.txt", "USER-NOTE", 9);
    put(SD, "apps/theme.cfg", "USER-THEME", 10);
    put(SD, "data/anima/learned/facets.it.jsonl", "USER-SEED", 9);   // a 'c' (create-only) seed the user edited
}

static void check_user_state(const char *label)
{
    char m[160];
    snprintf(m, sizeof m, "%s: user setup.json untouched", label);  OK(file_is(SD "/system/config/setup.json", "USER-SETUP"), m);
    snprintf(m, sizeof m, "%s: user document untouched", label);   OK(file_is(SD "/data/Documents/note.txt", "USER-NOTE"), m);
    snprintf(m, sizeof m, "%s: apps/theme.cfg untouched", label);   OK(file_is(SD "/apps/theme.cfg", "USER-THEME"), m);
    snprintf(m, sizeof m, "%s: create-only seed kept", label);      OK(file_is(SD "/data/anima/learned/facets.it.jsonl", "USER-SEED"), m);
    snprintf(m, sizeof m, "%s: no .part left", label);              OK(count_parts(SD) == 0, m);
}

static void check_core_installed(const char *label, bool arcade)
{
    char m[200], a[512], b[512];
    for (int i = 0; i < NFX; i++) {
        snprintf(a, sizeof a, SD "/%s", FX[i].path); snprintf(b, sizeof b, SRV "/%s", FX[i].path);
        if (!strcmp(FX[i].path, "data/anima/learned/facets.it.jsonl")) continue;   // user copy, checked separately
        bool want = strcmp(FX[i].pack, "core") == 0 || arcade;
        snprintf(m, sizeof m, "%s: %s %s", label, FX[i].path, want ? "installed byte-exact" : "NOT installed (pack off)");
        OK(want ? same_file(a, b) : !exists(a), m);
    }
}

static void test_sha_vectors(void)
{
    char h[65];
    sha_hex_of("", 0, h);    OK(!strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "sha256('')");
    sha_hex_of("abc", 3, h); OK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256('abc')");
    const char *s56 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha_hex_of(s56, strlen(s56), h); OK(!strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "sha256(448-bit)");
    sdc_sha256_t c; sdc_sha256_init(&c); static uint8_t a[1000]; memset(a, 'a', sizeof a);
    for (int i = 0; i < 1000; i++) sdc_sha256_update(&c, a, sizeof a);   // 1,000,000 x 'a', fed in odd chunks
    uint8_t g[32]; sdc_sha256_final(&c, g); sdc_sha256_hex(g, h);
    OK(!strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "sha256(1M x 'a')");
}

// The real release: every manifest line passes the device parser, and our SHA-256 agrees with the hashes
// Python's hashlib wrote (an independent implementation) on a spread of real files.
static void test_real_release(void)
{
    FILE *mf = fopen("dist/sd-manifest.txt", "r");
    if (!mf) { printf("  (skip real-release checks: no dist/sd-manifest.txt — run tools/package-release.mjs)\n"); return; }
    char line[512]; int ok = 0, bad = 0, hashed = 0, hash_ok = 0, idx = 0; sdc_header_t h; bool hdr = false;
    while (fgets(line, sizeof line, mf)) {
        if (!hdr && sdc_parse_header(line, &h)) { hdr = true; continue; }
        sdc_file_t f; bool cmt;
        if (sdc_parse_file(line, &f, &cmt)) {
            ok++;
            if ((idx++ % 37) == 0 && hashed < 40) {
                char p[512]; snprintf(p, sizeof p, "dist/sd/%s", f.path);
                uint8_t *d; long n = slurp(p, &d);
                if (n >= 0) { char hx[65]; sha_hex_of(d, (size_t)n, hx); free(d); hashed++;
                              if (!strcmp(hx, f.sha) && (uint32_t)n == f.size) hash_ok++; }
            }
        } else if (!cmt) bad++;
    }
    fclose(mf);
    OK(hdr, "real manifest has a header");
    OK(ok > 1000 && bad == 0, "every real manifest line parses (0 refused)");
    if (hashed) OK(hash_ok == hashed, "sdc_sha256 == hashlib on real release files");
    printf("  real release: %d lines parsed, %d refused, %d/%d files hash-identical\n", ok, bad, hash_ok, hashed);
}

int main(void)
{
    mkdir_p(W);
    test_sha_vectors();
    test_real_release();
    write_release(SRV, "v0.5.0", NULL);
    write_release(SRV_TAG, "v0.6.0", NULL);
    { char hostile[200]; char hx[65]; sha_hex_of("x", 1, hx);
      snprintf(hostile, sizeof hostile, "%s 1 core w ../system/keys/evil", hx);
      write_release(SRV_BAD, "v0.5.0", hostile); }
    sdc_state_t st; bool ok;

    // S1 fresh install
    fresh_sd(); fake_t f1 = { .srv = SRV, .corrupt_left = 0 };
    ok = run(&f1, SD, WINDOW, "core", &st);
    OK(ok && st.code == SDC_OK && st.phase == SDC_DONE, "S1 fresh install succeeds");
    OK(st.files_total == NFX - 1, "S1 counts only the core pack");
    OK(st.files_written == NFX - 2, "S1 writes everything except the existing create-only seed");
    OK(f1.max_len <= (int)WINDOW, "S1 never asks for more than one window");
    check_core_installed("S1", false); check_user_state("S1");
    { uint64_t core = 0; for (int i = 0; i < NFX; i++) if (!strcmp(FX[i].pack, "core")) { static uint8_t b[80000]; core += fx_bytes(&FX[i], b); }
      OK(st.kb_total == (uint32_t)(core / 1024), "S1 kb_total is the core payload (the bar measures bytes, not files)");
      OK(st.kb_done == st.kb_total && st.pct == 100, "S1 the bar ends full"); }
    OK(PR.monotonic && PR.bounded, "S1 progress never goes back, never past the total");
    OK(PR.inflight > 0, "S1 a big file updates the bar while it downloads");
    { char tag[24]; bool c = false, d = false; OK(sdc_content_read(SD, tag, sizeof tag, &c, &d) && c && !d && !strcmp(tag, "0.5.0"), "S1 content.json complete for 0.5.0"); }
    OK(file_is(SD "/system/registry/apps.json", "{\"installed\":[\"release\"]}\n"), "S1 registry created when absent");
    OK(!sdc_content_needed(SD, "0.5.0"), "S1 nothing more to offer for 0.5.0");
    OK(sdc_content_needed(SD, "0.6.0"), "S1 a newer firmware is offered again");

    // S2 rerun: nothing downloaded again (only the manifest windows)
    fake_t f2 = { .srv = SRV };
    ok = run(&f2, SD, WINDOW, "core", &st);
    OK(ok && st.files_written == 0, "S2 rerun downloads no file");
    OK(f2.requests <= 2, "S2 rerun only fetches the manifest");
    check_user_state("S2");

    // S3 flaky network: every 4th request dropped
    fresh_sd(); fake_t f3 = { .srv = SRV, .fail_every = 4 };
    ok = run(&f3, SD, WINDOW, "core", &st);
    OK(ok && f3.injected > 5, "S3 completes through dropped connections");
    check_core_installed("S3", false); check_user_state("S3");

    // S4 one corrupted window: the file is re-downloaded and verified
    fresh_sd(); fake_t f4 = { .srv = SRV, .corrupt_path = "www/shell/app.js", .corrupt_left = 1 };
    ok = run(&f4, SD, WINDOW, "core", &st);
    OK(ok, "S4 a corrupted window is caught by SHA-256 and re-fetched");
    check_core_installed("S4", false);

    // S5 a file that is always corrupted: never installed, run fails clearly
    fresh_sd(); fake_t f5 = { .srv = SRV, .corrupt_path = "apps/calc/www/app.js", .corrupt_left = -1 };
    ok = run(&f5, SD, WINDOW, "core", &st);
    OK(!ok && st.code == SDC_E_VERIFY && !strcmp(st.detail, "apps/calc/www/app.js"), "S5 persistent corruption -> VERIFY with the path");
    OK(!exists(SD "/apps/calc/www/app.js"), "S5 the corrupted file is not installed");
    check_user_state("S5");

    // S6 manifest not published
    fresh_sd(); fake_t f6 = { .srv = SRV, .manifest_404 = true };
    ok = run(&f6, SD, WINDOW, "core", &st);
    OK(!ok && st.code == SDC_E_MANIFEST && st.http == 404, "S6 manifest 404 -> MANIFEST/404");
    OK(!exists(SD "/www"), "S6 nothing installed");

    // S7 manifest for another firmware
    fresh_sd(); fake_t f7 = { .srv = SRV_TAG };
    ok = run(&f7, SD, WINDOW, "core", &st);
    OK(!ok && st.code == SDC_E_TAG, "S7 other firmware -> TAG");
    OK(!exists(SD "/www"), "S7 nothing installed");

    // S8 hostile manifest line: refused before a single payload byte is written
    fresh_sd(); fake_t f8 = { .srv = SRV_BAD };
    ok = run(&f8, SD, WINDOW, "core", &st);
    OK(!ok && st.code == SDC_E_MANIFEST_BAD, "S8 path outside the allow-list -> MANIFEST_BAD");
    OK(!exists(SD "/www") && !exists(W "/system/keys/evil") && !exists(SD "/../system"), "S8 nothing written anywhere");
    check_user_state("S8");

    // S9 no card
    fake_t f9 = { .srv = SRV };
    ok = run(&f9, W "/no-such-card", WINDOW, "core", &st);
    OK(!ok && st.code == SDC_E_NO_SD && f9.requests == 0, "S9 no SD -> NO_SD, no network used");

    // S10 the device's own app registry is never overwritten
    fresh_sd(); put(SD, "system/registry/apps.json", "DEVICE-REG", 10);
    fake_t f10 = { .srv = SRV };
    ok = run(&f10, SD, WINDOW, "core", &st);
    OK(ok && file_is(SD "/system/registry/apps.json", "DEVICE-REG"), "S10 existing registry kept (merge = create-only)");

    // S11 the guard itself: a window over the device's TLS cap fails like the real bug did
    fresh_sd(); fake_t f11 = { .srv = SRV };
    ok = run(&f11, SD, 8192, "core", &st);
    OK(!ok && st.code == SDC_E_FETCH && st.http == 0, "S11 8 KB requests on a 20 KB file hit the modelled TLS cap (proves the model)");

    // S12 the card cannot take a path (a FILE where a directory must go)
    fresh_sd(); put(SD, "www", "NOT-A-DIR", 9);
    fake_t f12 = { .srv = SRV };
    ok = run(&f12, SD, WINDOW, "core", &st);
    OK(!ok && st.code == SDC_E_WRITE, "S12 write failure -> WRITE");
    OK(count_parts(SD) == 0, "S12 no .part left");

    // S13 optional pack selected
    fresh_sd(); fake_t f13 = { .srv = SRV };
    ok = run(&f13, SD, WINDOW, "core,arcade", &st);
    OK(ok && st.files_total == NFX, "S13 core+arcade counts both packs");
    check_core_installed("S13", true);

    // S14 interrupted (a file 404s mid-run), then resumed: installed files are not fetched again
    fresh_sd(); fake_t f14a = { .srv = SRV, .notfound_path = "data/anima/anima-it-akb5.bin" };
    ok = run(&f14a, SD, WINDOW, "core", &st);
    int done_before = st.files_written;
    OK(!ok && st.code == SDC_E_FETCH && st.http == 404, "S14 interruption -> FETCH/404");
    OK(sdc_content_needed(SD, "0.5.0"), "S14 an interrupted install is offered again");
    OK(count_parts(SD) == 0 && !exists(SD "/data/anima/anima-it-akb5.bin"), "S14 the failed file left no .part and no partial file");
    fake_t f14b = { .srv = SRV };
    ok = run(&f14b, SD, WINDOW, "core", &st);
    OK(ok && st.files_written == (NFX - 2) - done_before, "S14 resume fetches only what is missing");
    check_core_installed("S14", false); check_user_state("S14");

    // S16 the link dies MID-FILE after some windows were written (the 70 KB brain file, from 24 KB on)
    fresh_sd(); fake_t f16 = { .srv = SRV, .cut_path = "data/anima/anima-it-akb5.bin", .cut_off = 4 * WINDOW };
    ok = run(&f16, SD, WINDOW, "core", &st);
    OK(!ok && st.code == SDC_E_FETCH && st.http == 0 && !strcmp(st.detail, "data/anima/anima-it-akb5.bin"), "S16 mid-file link loss -> FETCH with the path");
    OK(count_parts(SD) == 0 && !exists(SD "/data/anima/anima-it-akb5.bin"), "S16 no .part and no half file left");
    // 1 manifest + index.html 1 + app.js 4 + calc 2 + 4 good brain windows + 4 attempts (1 + 3 retries) on the cut one
    OK(f16.requests == 1 + 1 + 4 + 2 + 4 + 4, "S16 exactly the windows up to the cut, the cut one tried 1+3 times");
    check_user_state("S16");

    // S15 "Skip" is remembered for this firmware only
    fresh_sd();
    OK(sdc_content_needed(SD, "0.5.0"), "S15 a blank card is offered the download");
    sdc_content_write(SD, "0.5.0", false, true);
    OK(!sdc_content_needed(SD, "0.5.0"), "S15 skipped -> not offered again for 0.5.0");
    OK(sdc_content_needed(SD, "0.6.0"), "S15 a new firmware offers it again");

    // S17 a card filled BY HAND from the release's -sd.zip (which carries its manifest): recognised, never
    //     re-offered; a newer firmware sees it as OUTDATED and the update fetches only what changed.
    {
        fresh_sd();
        static uint8_t b[80000];
        for (int i = 0; i < NFX; i++) {                                  // the user's edited seed stays (like a careful copy)
            if (strcmp(FX[i].pack, "core") || (FX[i].mode == 'c' && !strcmp(FX[i].path, "data/anima/learned/facets.it.jsonl"))) continue;
            put(SD, FX[i].path, b, fx_bytes(&FX[i], b));
        }
        uint8_t *m; long mn = slurp(SRV "/sd-manifest.txt", &m);
        put(SD, "system/content/manifest.txt", m, (size_t)mn); free(m);
        char tag[24];
        OK(sdc_content_status(SD, "0.5.0", tag, sizeof tag) == SDC_ST_MANUAL && !strcmp(tag, "0.5.0"), "S17 a hand copy of this release is MANUAL (0.5.0)");
        OK(!sdc_content_needed(SD, "0.5.0"), "S17 a hand copy is not offered the download");
        OK(sdc_content_status(SD, "0.6.0", tag, sizeof tag) == SDC_ST_OUTDATED && !strcmp(tag, "0.5.0"), "S17 for a newer firmware it is OUTDATED (from 0.5.0)");
        OK(sdc_content_needed(SD, "0.6.0"), "S17 an outdated card is offered the update");
        sdc_content_write(SD, "0.5.0", false, true);                   // an old "Later" must not hide real files
        OK(sdc_content_status(SD, "0.5.0", NULL, 0) == SDC_ST_MANUAL, "S17 a stale 'Later' does not mask a hand copy");
        unlink(SD "/system/content.json");
        // "verify and repair" over the hand copy: nothing to fetch, then recorded as COMPLETE
        fake_t fa = { .srv = SRV };
        ok = run(&fa, SD, WINDOW, "core", &st);
        OK(ok && st.files_written == 0, "S17 verifying a correct hand copy downloads no file");
        OK(sdc_content_status(SD, "0.5.0", NULL, 0) == SDC_ST_COMPLETE, "S17 ... and records it COMPLETE");
        // an update where one file changed: exactly that file is fetched
        put(SD, "www/shell/app.js", "OLD-RELEASE", 11);
        fake_t fb = { .srv = SRV };
        ok = run(&fb, SD, WINDOW, "core", &st);
        OK(ok && st.files_written == 1, "S17 an update fetches only the changed file");
        check_core_installed("S17", false); check_user_state("S17");
    }
    // S18 the web OS without any manifest (a dev sync, an old zip): UNKNOWN, not nagged
    fresh_sd(); put(SD, "www/shell/index.html.gz", "gz", 2);
    OK(sdc_content_status(SD, "0.5.0", NULL, 0) == SDC_ST_UNKNOWN && !sdc_content_needed(SD, "0.5.0"), "S18 web OS of unknown release: UNKNOWN, not offered");
    // S19 an interrupted run reads as PARTIAL (resume), not as a hand copy, although its manifest is on the card
    fresh_sd(); fake_t f19 = { .srv = SRV, .cut_path = "data/anima/anima-it-akb5.bin", .cut_off = 2 * WINDOW };
    ok = run(&f19, SD, WINDOW, "core", &st);
    OK(!ok && sdc_content_status(SD, "0.5.0", NULL, 0) == SDC_ST_PARTIAL && sdc_content_needed(SD, "0.5.0"), "S19 interrupted -> PARTIAL, offered (resume)");
    // S20 a blank card, then "Later": SKIPPED, not offered
    fresh_sd();
    OK(sdc_content_status(SD, "0.5.0", NULL, 0) == SDC_ST_MISSING, "S20 a blank card is MISSING");
    sdc_content_write(SD, "0.5.0", false, true);
    OK(sdc_content_status(SD, "0.5.0", NULL, 0) == SDC_ST_SKIPPED, "S20 'Later' on a blank card is SKIPPED");

    printf("sdcontent-e2e: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
