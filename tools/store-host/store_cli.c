// Host driver for the REAL app-store engine (firmware/components/nucleo_store/store_engine.c + store_verify.c,
// mbedTLS built from ESP-IDF's sources). A fake "GitHub Pages" serves a directory with the device's rules
// (Range windows, 416 past the end, injectable faults); a temp directory is the SD card. run.mjs drives it.
//
//   store_cli <public dir> <sd root> refresh [now]
//   store_cli <public dir> <sd root> install <id>
//   store_cli <public dir> <sd root> remove <id>
//   store_cli <public dir> <sd root> list
//   store_cli <public dir> <sd root> installed <id>
//   store_cli <public dir> <sd root> paths
// Env FAULT: "drop" (every 3rd request fails at the transport, then retried), "dead" (every request fails),
// "404". Prints "rc=<st_code_t>" last.
#include "store_engine.h"
#include "store_verify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *s_pub;
static int s_req;
static const char *s_fault;

static int fake_get(void *ctx, const char *url, uint32_t off, uint32_t len, uint8_t *buf, int *status)
{
    (void)ctx;
    s_req++;
    *status = 0;
    if (s_fault && !strcmp(s_fault, "dead")) return -1;
    if (s_fault && !strcmp(s_fault, "drop") && s_req % 3 == 0) return -1;
    const char *base = "https://store.test/";
    if (strncmp(url, base, strlen(base))) { *status = 404; return -1; }
    char path[512];
    snprintf(path, sizeof path, "%s/%s", s_pub, url + strlen(base));
    FILE *f = fopen(path, "rb");
    if (!f || (s_fault && !strcmp(s_fault, "404"))) { if (f) fclose(f); *status = 404; return -1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if ((long)off >= size && size > 0) { fclose(f); *status = 416; return -1; }
    fseek(f, (long)off, SEEK_SET);
    int n = (int)fread(buf, 1, len, f);
    fclose(f);
    *status = 206;
    return n;
}

static bool host_verify(void *ctx, const char *key, const uint8_t hash[32], const uint8_t *sig, size_t n)
{
    (void)ctx;
    return st_verify(key, hash, sig, n);
}

static uint32_t s_now;
static uint32_t host_now(void *ctx) { (void)ctx; return s_now; }

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: store_cli <public> <sd> <cmd> [arg]\n"); return 2; }
    s_pub = argv[1];
    const char *sd = argv[2], *cmd = argv[3], *arg = argc > 4 ? argv[4] : "";
    s_fault = getenv("FAULT");
    static uint8_t buf[6144];
    st_cfg_t cfg = { .root = sd, .base_url = "https://store.test/", .buf = buf, .window = sizeof buf, .retries = 3 };
    st_io_t io = { .ctx = NULL, .get = fake_get, .verify = host_verify, .now = host_now };
    st_state_t st = { 0 };
    st_code_t rc = ST_OK;
    if (!strcmp(cmd, "refresh")) {
        s_now = arg[0] ? (uint32_t)strtoul(arg, NULL, 10) : 0;
        rc = st_refresh(&cfg, &io, &st);
        printf("serial=%lu\n", (unsigned long)st_catalog_serial(sd));
    } else if (!strcmp(cmd, "install")) {
        st_app_t a;
        if (!st_find(sd, arg, &a)) rc = ST_E_NOTFOUND;
        else rc = st_install(&cfg, &io, &st, &a);
        if (rc == ST_OK) printf("files=%d bytes=%lu\n", st.files_done, (unsigned long)st.done);
    } else if (!strcmp(cmd, "remove")) {
        rc = st_remove(sd, arg);
    } else if (!strcmp(cmd, "list")) {
        FILE *f = st_catalog_open(sd);
        st_app_t a;
        int n = 0;
        while (f && st_catalog_next(f, &a)) { printf("app %s %s %lu %s\n", a.id, a.kind, (unsigned long)a.code, a.name[1]); n++; }
        if (f) fclose(f);
        printf("count=%d\n", n);
    } else if (!strcmp(cmd, "installed")) {
        bool sys = false;
        printf("installed=%lu system=%d\n", (unsigned long)st_installed(sd, arg, &sys), sys);
    } else if (!strcmp(cmd, "paths")) {
        static const char *good[] = { "a", "www/index.html", "a/b/c/d.js", "x_y-z.1" };
        static const char *bad[] = { "", "/a", "a/", "..", "../a", "a/../b", ".hidden", "a/.b", "a//b", "a/b/c/d/e",
                                      "a\\b", "a b", "a:b" };
        int errs = 0;
        for (unsigned i = 0; i < sizeof good / sizeof *good; i++) if (!st_path_ok(good[i])) { printf("good refused: %s\n", good[i]); errs++; }
        for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) if (st_path_ok(bad[i])) { printf("bad accepted: %s\n", bad[i]); errs++; }
        if (st_ver_cmp("0.8.0", "0.7.5") <= 0 || st_ver_cmp("0.7.5+3.gabc", "0.7.5") != 0 || st_ver_cmp("1.0", "1.0.1") >= 0) { printf("ver_cmp\n"); errs++; }
        rc = errs ? ST_E_BAD : ST_OK;
    } else { fprintf(stderr, "unknown command %s\n", cmd); return 2; }
    if (rc != ST_OK) printf("detail=%s http=%d\n", st.detail, st.http);
    printf("requests=%d\nrc=%d\n", s_req, rc);
    return 0;
}
