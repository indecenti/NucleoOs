// App store client — the engine. See store_engine.h, and docs/format.md in the store repository (the two
// must stay in step: the store's tools/store.py writes exactly what this reads).
#include "store_engine.h"
#include "sdc_sha256.h"
#include "sdc_registry.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define INDEX_MAGIC "nucleoos-cardputer-index-v2"
#define PKG_MAGIC   "nucleoos-cardputer-pkg-v1"
#define SIG_MAGIC   "nucleoos-cardputer-sig-v1"
#define MAX_INDEX   (4u * 1024 * 1024)
#define MAX_SIGFILE 1024u
#define MAX_PKGSIG  (32u * 1024)
#define MAX_FILE    (4u * 1024 * 1024)
#define MAX_FILES   64
#define LINE_MAX_   1280
#define CLOCK_SET   1700000000u     // before this the device clock is not set: expiry is not checked

static char s_line[LINE_MAX_];      // one catalog / package line (the engine runs on one task at a time)
static char s_cat_channel[8];       // channel of the catalog st_catalog_open last opened

const char *st_kind_channel(const char *kind)
{
    if (!strcmp(kind, "native")) return ST_CH_NATIVE;
    if (!strcmp(kind, "web") || !strcmp(kind, "data")) return ST_CH_WEB;
    return NULL;
}

static bool channel_ok(const char *ch) { return ch && (!strcmp(ch, ST_CH_NATIVE) || !strcmp(ch, ST_CH_WEB)); }

// ── small helpers ─────────────────────────────────────────────────────────────
static bool is_dir(const char *p) { struct stat s; return stat(p, &s) == 0 && S_ISDIR(s.st_mode); }
static bool exists(const char *p) { struct stat s; return stat(p, &s) == 0; }

static int mkdir1(const char *p)
{
#ifdef _WIN32
    return (mkdir)(p);          // parenthesised: a host shim may define mkdir(p, m) as a macro
#else
    return mkdir(p, 0775);
#endif
}

// mkdir -p for the directory part of `path` (or `path` itself when dir_too).
static bool mkdirs(const char *path, bool dir_too)
{
    char tmp[300];
    size_t n = strlen(path);
    if (n >= sizeof tmp) return false;
    memcpy(tmp, path, n + 1);
    if (!dir_too) { char *s = strrchr(tmp, '/'); if (!s) return true; *s = 0; }
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (!is_dir(tmp) && mkdir1(tmp) != 0 && errno != EEXIST) return false;
        *p = '/';
    }
    return is_dir(tmp) || mkdir1(tmp) == 0 || errno == EEXIST;
}

static void rmtree(const char *path)
{
    DIR *d = opendir(path);
    if (!d) { unlink(path); return; }
    struct dirent *e;
    char child[300];
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        if (is_dir(child)) rmtree(child); else unlink(child);
    }
    closedir(d);
    rmdir(path);
}

static bool hex64(const char *s)
{
    for (int i = 0; i < 64; i++) if (!isxdigit((unsigned char)s[i]) || isupper((unsigned char)s[i])) return false;
    return s[64] == 0;
}

bool st_id_ok(const char *id)
{
    size_t n = strlen(id);
    if (n < 2 || n > ST_ID_MAX || !islower((unsigned char)id[0])) return false;
    for (size_t i = 0; i < n; i++)
        if (!(islower((unsigned char)id[i]) || isdigit((unsigned char)id[i]) || id[i] == '-')) return false;
    return true;
}

// Up to four segments of [A-Za-z0-9._-], none empty, none starting with '.' (so never "." or "..").
bool st_path_ok(const char *path)
{
    int segs = 0;
    const char *p = path;
    if (!*p || strlen(p) > 160) return false;
    while (*p) {
        if (*p == '.' || *p == '/') return false;
        segs++;
        while (*p && *p != '/') {
            char c = *p++;
            if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) return false;
        }
        if (*p == '/') { p++; if (!*p) return false; }
    }
    return segs <= 4;
}

int st_ver_cmp(const char *a, const char *b)
{
    for (int i = 0; i < 3; i++) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

static void fail_detail(st_state_t *st, st_code_t code, const char *detail)
{
    st->code = code;
    st->phase = ST_PH_FAILED;
    snprintf(st->detail, sizeof st->detail, "%s", detail ? detail : "");
}

static void progress(const st_io_t *io, st_state_t *st)
{
    if (st->total) st->pct = (int)((uint64_t)st->done * 100 / st->total);
    if (io->progress) io->progress(io->ctx, st);
}

static int b64val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int b64decode(const char *s, uint8_t *out, int cap)
{
    int n = 0, bits = 0;
    uint32_t acc = 0;
    for (; *s && *s != '='; s++) {
        int v = b64val((unsigned char)*s);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; if (n >= cap) return -1; out[n++] = (uint8_t)(acc >> bits); }
    }
    return n;
}

// "key <16 hex>\nsig <b64>\n" (the lines after a package's "--", or after the sig magic).
static bool read_sig_block(FILE *f, char key[17], uint8_t *der, int *derlen)
{
    char kl[32], sl[160];
    if (!fgets(kl, sizeof kl, f) || !fgets(sl, sizeof sl, f)) return false;
    size_t k = strlen(kl), s = strlen(sl);
    if (k != 21 || strncmp(kl, "key ", 4) || kl[20] != '\n') return false;
    if (s < 6 || strncmp(sl, "sig ", 4) || sl[s - 1] != '\n') return false;
    memcpy(key, kl + 4, 16); key[16] = 0;
    for (int i = 0; i < 16; i++) if (!isxdigit((unsigned char)key[i])) return false;
    sl[s - 1] = 0;
    *derlen = b64decode(sl + 4, der, 80);
    return *derlen >= 8;
}

// ── download ──────────────────────────────────────────────────────────────────
// GET url into `dest` via `dest`.part in Range windows; refuses more than `max` bytes, and anything but
// exactly `want` bytes when want >= 0. sha (optional) receives the SHA-256 of what was written.
static st_code_t fetch(const st_cfg_t *cfg, const st_io_t *io, st_state_t *st, const char *url,
                       const char *dest, uint32_t max, int64_t want, uint8_t sha[32])
{
    static char part[300];          // static: the engine runs on one task, and its stack also carries TLS
    snprintf(part, sizeof part, "%s.part", dest);
    if (!mkdirs(dest, false)) { fail_detail(st, ST_E_WRITE, dest); return ST_E_WRITE; }
    FILE *f = fopen(part, "wb");
    if (!f) { fail_detail(st, ST_E_WRITE, dest); return ST_E_WRITE; }
    sdc_sha256_t h;
    sdc_sha256_init(&h);
    uint32_t off = 0;
    st_code_t rc = ST_OK;
    for (;;) {
        int status = 0, n = -1;
        for (int a = 0; a <= cfg->retries; a++) {
            n = io->get(io->ctx, url, off, cfg->window, cfg->buf, &status);
            if (n >= 0 || status != 0) break;          // only a transport failure is worth retrying
            if (io->tick) io->tick(io->ctx);
        }
        if (n < 0 && status == 416 && off > 0) break;   // the body ended exactly on a window edge
        if (n < 0) { rc = ST_E_NET; st->http = status; break; }
        if (off + (uint32_t)n > max || (want >= 0 && off + (uint32_t)n > (uint64_t)want)) { rc = ST_E_VERIFY; break; }
        if (n && fwrite(cfg->buf, 1, (size_t)n, f) != (size_t)n) { rc = ST_E_WRITE; break; }
        sdc_sha256_update(&h, cfg->buf, (size_t)n);
        off += (uint32_t)n;
        st->done += (uint32_t)n;
        progress(io, st);
        if (io->tick) io->tick(io->ctx);
        if ((uint32_t)n < cfg->window || status == 200) break;   // short window (or a whole small body): the end
    }
    if (fclose(f) != 0 && rc == ST_OK) rc = ST_E_WRITE;
    if (rc == ST_OK && want >= 0 && off != (uint64_t)want) rc = ST_E_VERIFY;
    if (rc == ST_OK) {
        if (sha) sdc_sha256_final(&h, sha);
        unlink(dest);
        if (rename(part, dest) != 0) rc = ST_E_WRITE;
    }
    if (rc != ST_OK) { unlink(part); fail_detail(st, rc, strrchr(url, '/') ? strrchr(url, '/') + 1 : url); }
    return rc;
}

// ── catalog ───────────────────────────────────────────────────────────────────
static void store_dir(const char *root, const char *channel, char *out, size_t n)
{
    snprintf(out, n, "%s/apps/.store/%s", root, channel);
}

uint32_t st_catalog_serial(const char *root, const char *channel)
{
    char p[160]; store_dir(root, channel, p, sizeof p);
    strncat(p, "/serial", sizeof p - strlen(p) - 1);
    FILE *f = fopen(p, "rb");
    unsigned long v = 0;
    if (f) { if (fscanf(f, "%lu", &v) != 1) v = 0; fclose(f); }
    return (uint32_t)v;
}

// The header of a downloaded catalog: magic, channel, serial, expires, apps.
static bool read_header(FILE *f, const char *channel, uint32_t *serial, uint32_t *expires)
{
    char l[64], want[24];
    unsigned long s, e, n;
    snprintf(want, sizeof want, "channel %s\n", channel);
    if (!fgets(l, sizeof l, f) || strcmp(l, INDEX_MAGIC "\n")) return false;
    if (!fgets(l, sizeof l, f) || strcmp(l, want)) return false;
    if (!fgets(l, sizeof l, f) || sscanf(l, "serial %lu", &s) != 1) return false;
    if (!fgets(l, sizeof l, f) || sscanf(l, "expires %lu", &e) != 1) return false;
    if (!fgets(l, sizeof l, f) || sscanf(l, "apps %lu", &n) != 1) return false;
    *serial = (uint32_t)s; *expires = (uint32_t)e;
    return true;
}

// ── store art ─────────────────────────────────────────────────────────────────
// Optional and cosmetic: public/<channel>/art.bin (the apps' colour icons, raw RGB565: the device draws them a row at
// a time, no decoder) signed by art.sig exactly like the catalog. Fetched after a good catalog, only when art.sig
// changed; a store without art (404), a failed download or a bad signature leave the previous art (or none) and
// NEVER fail the refresh. Older firmware and the web shells never ask for it.
#define MAX_ART (512u * 1024)

static bool same_file(const char *a, const char *b)
{
    FILE *x = fopen(a, "rb"), *y = x ? fopen(b, "rb") : NULL;
    bool same = x && y;
    while (same) {
        int c = fgetc(x), d = fgetc(y);
        if (c != d) same = false;
        if (c == EOF || d == EOF) break;
    }
    if (x) fclose(x);
    if (y) fclose(y);
    return same;
}

// The five paths are st_refresh's own buffers (each 200 B), done with by then: no RAM of its own.
static void refresh_art(const st_cfg_t *cfg, const st_io_t *io, st_state_t *st, const char *dir,
                        char *sig, char *sig_new, char *art, char *art_new, char *url)
{
    const st_state_t keep = *st;                     // the art's own failures are not the refresh's
    snprintf(sig, 200, "%s/art.sig", dir);
    snprintf(sig_new, 200, "%s/art.sig.new", dir);
    snprintf(art, 200, "%s/art.bin", dir);
    snprintf(art_new, 200, "%s/art.new", dir);
    snprintf(url, 200, "%s%s/art.sig", cfg->base_url, cfg->channel);
    if (fetch(cfg, io, st, url, sig_new, MAX_SIGFILE, -1, NULL) == ST_OK && !same_file(sig_new, sig)) {
        uint8_t hash[32];
        snprintf(url, 200, "%s%s/art.bin", cfg->base_url, cfg->channel);
        if (fetch(cfg, io, st, url, art_new, MAX_ART, -1, hash) == ST_OK) {
            FILE *f = fopen(sig_new, "rb");
            char l[40], key[17];
            uint8_t der[80];
            int dl = 0;
            bool ok = f && fgets(l, sizeof l, f) && !strcmp(l, SIG_MAGIC "\n") && read_sig_block(f, key, der, &dl);
            if (f) fclose(f);
            if (ok && io->verify(io->ctx, key, hash, der, (size_t)dl)) {
                unlink(art); unlink(sig);
                if (rename(art_new, art) != 0 || rename(sig_new, sig) != 0) { unlink(art); unlink(sig); }
            }
        }
    }
    unlink(sig_new);
    unlink(art_new);
    *st = keep;
}

FILE *st_art_open(const char *root, const char *channel)
{
    char p[200];
    store_dir(root, channel, p, sizeof p);
    strncat(p, "/art.bin", sizeof p - strlen(p) - 1);
    FILE *f = fopen(p, "rb");
    uint8_t h[16];
    if (f && (fread(h, 1, sizeof h, f) != sizeof h || memcmp(h, "NART", 4) || h[4] != 1)) { fclose(f); f = NULL; }
    return f;
}

bool st_art_find(FILE *f, const char *id, uint32_t *off, int *size)
{
    uint8_t h[16], e[ST_ART_ENTRY];
    if (!f || fseek(f, 0, SEEK_SET) || fread(h, 1, sizeof h, f) != sizeof h) return false;
    const int n = h[6] | h[7] << 8, sz = h[5];
    for (int i = 0; i < n && fread(e, 1, sizeof e, f) == sizeof e; i++) {
        if (strncmp((const char *)e, id, ST_ART_ENTRY - 4) || e[strlen(id)] != 0) continue;
        uint32_t o = (uint32_t)e[ST_ART_ENTRY - 4] | (uint32_t)e[ST_ART_ENTRY - 3] << 8 |
                     (uint32_t)e[ST_ART_ENTRY - 2] << 16 | (uint32_t)e[ST_ART_ENTRY - 1] << 24;
        if (o < 16u + (uint32_t)n * ST_ART_ENTRY) return false;
        *off = o; *size = sz;
        return sz > 0;
    }
    return false;
}


st_code_t st_refresh(const st_cfg_t *cfg, const st_io_t *io, st_state_t *st)
{
    memset(st, 0, sizeof *st);
    st->phase = ST_PH_CATALOG;
    if (!channel_ok(cfg->channel)) { fail_detail(st, ST_E_BAD, "channel"); return ST_E_BAD; }
    if (!is_dir(cfg->root)) { fail_detail(st, ST_E_NO_SD, cfg->root); return ST_E_NO_SD; }
    static char dir[160], idx_new[200], sig_new[200], idx[200], ser[200], url[200];   // static: see st_install
    store_dir(cfg->root, cfg->channel, dir, sizeof dir);
    if (!mkdirs(dir, true)) { fail_detail(st, ST_E_WRITE, dir); return ST_E_WRITE; }
    snprintf(idx_new, sizeof idx_new, "%s/index.new", dir);
    snprintf(sig_new, sizeof sig_new, "%s/index.sig.new", dir);
    snprintf(idx, sizeof idx, "%s/index.txt", dir);
    snprintf(ser, sizeof ser, "%s/serial", dir);

    uint8_t hash[32];
    snprintf(url, sizeof url, "%s%s/index.txt", cfg->base_url, cfg->channel);
    st_code_t rc = fetch(cfg, io, st, url, idx_new, MAX_INDEX, -1, hash);
    if (rc == ST_OK) {
        snprintf(url, sizeof url, "%s%s/index.sig", cfg->base_url, cfg->channel);
        rc = fetch(cfg, io, st, url, sig_new, MAX_SIGFILE, -1, NULL);
    }
    if (rc != ST_OK) goto out;

    // The signature, then the header.
    {
        FILE *f = fopen(sig_new, "rb");
        char l[40], key[17];
        uint8_t der[80];
        int dl = 0;
        bool ok = f && fgets(l, sizeof l, f) && !strcmp(l, SIG_MAGIC "\n") && read_sig_block(f, key, der, &dl);
        if (f) fclose(f);
        if (!ok) { rc = ST_E_BAD; fail_detail(st, rc, "index.sig"); goto out; }
        if (!io->verify(io->ctx, key, hash, der, (size_t)dl)) { rc = ST_E_SIG; fail_detail(st, rc, "index.txt"); goto out; }
    }
    {
        FILE *f = fopen(idx_new, "rb");
        uint32_t serial = 0, expires = 0;
        bool ok = f && read_header(f, cfg->channel, &serial, &expires);
        if (f) fclose(f);
        if (!ok) { rc = ST_E_BAD; fail_detail(st, rc, "index.txt"); goto out; }
        if (serial < st_catalog_serial(cfg->root, cfg->channel)) { rc = ST_E_ROLLBACK; fail_detail(st, rc, "serial"); goto out; }
        uint32_t now = io->now ? io->now(io->ctx) : 0;
        if (now >= CLOCK_SET && now > expires) { rc = ST_E_EXPIRED; fail_detail(st, rc, "expires"); goto out; }
        unlink(idx);
        if (rename(idx_new, idx) != 0) { rc = ST_E_WRITE; fail_detail(st, rc, "index.txt"); goto out; }
        FILE *s = fopen(ser, "wb");
        if (!s || fprintf(s, "%lu\n", (unsigned long)serial) < 0) { if (s) fclose(s); rc = ST_E_WRITE; fail_detail(st, rc, "serial"); goto out; }
        fclose(s);
    }
    // The catalog is in place, its buffers free. sig_new / idx_new become the art's temporaries (unlinked at out:).
    refresh_art(cfg, io, st, dir, ser, sig_new, idx, idx_new, url);
    st->phase = ST_PH_DONE;
out:
    unlink(idx_new);
    unlink(sig_new);
    return rc;
}

// Split `s` in place at `sep` into at most n fields; returns the count.
static int split(char *s, char sep, char **f, int n)
{
    int k = 0;
    f[k++] = s;
    for (char *p = s; *p && k < n; p++) if (*p == sep) { *p = 0; f[k++] = p + 1; }
    return k;
}

static bool copy_field(char *dst, size_t cap, const char *src, size_t maxlen)
{
    size_t n = strlen(src);
    if (n == 0 || n > maxlen || n >= cap) return false;
    for (size_t i = 0; i < n; i++) if ((unsigned char)src[i] < 0x20 || (unsigned char)src[i] > 0x7e) return false;
    memcpy(dst, src, n + 1);
    return true;
}

bool st_parse_line(const char *line, st_app_t *a)
{
    char buf[LINE_MAX_];
    size_t n = strlen(line);
    if (n >= sizeof buf) return false;
    memcpy(buf, line, n + 1);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    // 15 fields: ... updated, author, abi; anything after is a future field (ignored).
    char *f[16];
    int nf = split(buf, '\t', f, 16);
    if (nf < 15 || strcmp(f[0], "app")) return false;
    memset(a, 0, sizeof *a);
    char *end;
    if (!copy_field(a->id, sizeof a->id, f[1], ST_ID_MAX) || !st_id_ok(a->id)) return false;
    if (!copy_field(a->kind, sizeof a->kind, f[2], 7)) return false;
    unsigned long code = strtoul(f[3], &end, 10);
    if (*end || code == 0 || code > 0x7fffffffUL) return false;
    a->code = (uint32_t)code;
    if (!copy_field(a->version, sizeof a->version, f[4], 15) || !copy_field(a->min_os, sizeof a->min_os, f[5], 15)) return false;
    if (!copy_field(a->category, sizeof a->category, f[6], 11)) return false;
    unsigned long size = strtoul(f[7], &end, 10);
    if (*end) return false;
    a->size = (uint32_t)size;
    if (!copy_field(a->sig_sha, sizeof a->sig_sha, f[8], 64) || !hex64(a->sig_sha)) return false;
    if (strcmp(f[9], "-") && !copy_field(a->perms, sizeof a->perms, f[9], 63)) return false;
    char *names[ST_LANGS + 1], *descs[ST_LANGS + 1];
    if (split(f[10], '|', names, ST_LANGS + 1) != ST_LANGS || split(f[11], '|', descs, ST_LANGS + 1) != ST_LANGS) return false;
    for (int i = 0; i < ST_LANGS; i++)
        if (!copy_field(a->name[i], sizeof a->name[i], names[i], ST_NAME_MAX) ||
            !copy_field(a->desc[i], sizeof a->desc[i], descs[i], ST_DESC_MAX)) return false;
    unsigned long u = strtoul(f[12], &end, 10);
    if (*end) return false;
    a->updated = (uint32_t)u;
    if (!copy_field(a->author, sizeof a->author, f[13], sizeof a->author - 1)) return false;
    if (!st_kind_channel(a->kind)) return false;
    if (!strcmp(a->kind, "native")) {                       // a native app names the ABI level it was built for
        unsigned long abi = strtoul(f[14], &end, 10);
        if (*end || abi == 0 || abi > 0xffff) return false;
        a->abi = (uint32_t)abi;
    } else if (strcmp(f[14], "-")) return false;
    return true;
}

FILE *st_catalog_open(const char *root, const char *channel)
{
    if (!channel_ok(channel)) return NULL;
    char p[200]; store_dir(root, channel, p, sizeof p);
    strncat(p, "/index.txt", sizeof p - strlen(p) - 1);
    FILE *f = fopen(p, "rb");
    uint32_t s, e;
    if (f && !read_header(f, channel, &s, &e)) { fclose(f); return NULL; }
    if (f) snprintf(s_cat_channel, sizeof s_cat_channel, "%s", channel);
    return f;
}

bool st_catalog_next(FILE *f, st_app_t *out)
{
    while (fgets(s_line, sizeof s_line, f)) {
        size_t n = strlen(s_line);
        if (n && s_line[n - 1] != '\n' && !feof(f)) {           // longer than any valid line: skip the rest
            int c; while ((c = fgetc(f)) != EOF && c != '\n') {}
            continue;
        }
        if (st_parse_line(s_line, out) && !strcmp(st_kind_channel(out->kind), s_cat_channel)) return true;
    }
    return false;
}

bool st_find(const char *root, const char *channel, const char *id, st_app_t *out)
{
    FILE *f = st_catalog_open(root, channel);
    if (!f) return false;
    bool found = false;
    while (!found && st_catalog_next(f, out)) found = !strcmp(out->id, id);
    fclose(f);
    return found;
}

// ── install ───────────────────────────────────────────────────────────────────
uint32_t st_installed(const char *root, const char *id, bool *system)
{
    char p[200];
    snprintf(p, sizeof p, "%s/apps/%s/.store-version", root, id);
    if (system) *system = false;
    FILE *f = fopen(p, "rb");
    if (!f) {
        snprintf(p, sizeof p, "%s/apps/%s", root, id);
        if (system) *system = exists(p);
        return 0;
    }
    unsigned long v = 0;
    if (fscanf(f, "code %lu", &v) != 1) v = 0;
    fclose(f);
    return (uint32_t)v;
}

// One package line: "<sha256> <size> <path>".
static bool parse_pkg_file(char *l, char sha[65], uint32_t *size, char **path)
{
    size_t n = strlen(l);
    if (n && l[n - 1] == '\n') l[--n] = 0;
    if (n < 68 || l[64] != ' ') return false;
    memcpy(sha, l, 64); sha[64] = 0;
    if (!hex64(sha)) return false;
    char *end;
    unsigned long sz = strtoul(l + 65, &end, 10);
    if (end == l + 65 || *end != ' ' || sz > MAX_FILE) return false;
    *size = (uint32_t)sz;
    *path = end + 1;
    return st_path_ok(*path);
}

// Reads <sigpath> (a downloaded package.sig): checks its header against the catalog line, hashes the signed
// body, verifies the signature and counts the files. Nothing is downloaded before this says yes.
static st_code_t check_package(const st_io_t *io, st_state_t *st, const st_app_t *a, const char *sigpath,
                               int *nfiles, uint32_t *total)
{
    FILE *f = fopen(sigpath, "rb");
    if (!f) { fail_detail(st, ST_E_WRITE, "package.sig"); return ST_E_WRITE; }
    sdc_sha256_t h;
    sdc_sha256_init(&h);
    int line = 0, files = 0;
    uint32_t sum = 0;
    bool body_end = false, ok = true;
    char code[16];
    snprintf(code, sizeof code, "%lu", (unsigned long)a->code);
    while (ok && fgets(s_line, sizeof s_line, f)) {
        size_t n = strlen(s_line);
        if (!n || s_line[n - 1] != '\n') { ok = false; break; }
        if (!strcmp(s_line, "--\n")) { body_end = true; break; }
        sdc_sha256_update(&h, s_line, n);
        s_line[n - 1] = 0;
        switch (line++) {
            case 0: ok = !strcmp(s_line, PKG_MAGIC); break;
            case 1: ok = !strcmp(s_line, a->id); break;
            case 2: ok = !strcmp(s_line, code); break;
            case 3: ok = !strcmp(s_line, a->kind); break;
            default: {
                char sha[65], *path; uint32_t sz;
                strcat(s_line, "\n");
                ok = parse_pkg_file(s_line, sha, &sz, &path) && ++files <= MAX_FILES;
                sum += sz;
            }
        }
    }
    char key[17];
    uint8_t der[80], hash[32];
    int dl = 0;
    ok = ok && body_end && line >= 5 && read_sig_block(f, key, der, &dl);
    fclose(f);
    if (!ok) { fail_detail(st, ST_E_BAD, "package.sig"); return ST_E_BAD; }
    sdc_sha256_final(&h, hash);
    if (!io->verify(io->ctx, key, hash, der, (size_t)dl)) { fail_detail(st, ST_E_SIG, "package.sig"); return ST_E_SIG; }
    *nfiles = files;
    *total = sum;
    return ST_OK;
}

// The app's own "permissions" array, copied verbatim from its manifest.json when it is plain and short.
static void manifest_perms(const char *mpath, char *out, size_t cap, uint8_t *buf, size_t bufcap)
{
    snprintf(out, cap, "[]");
    FILE *f = fopen(mpath, "rb");
    if (!f) return;
    size_t n = fread(buf, 1, bufcap - 1, f);
    fclose(f);
    buf[n] = 0;
    const char *k = strstr((const char *)buf, "\"permissions\"");
    if (!k) return;
    const char *o = strchr(k, '['), *c = o ? strchr(o, ']') : NULL;
    if (!o || !c || (size_t)(c - o + 2) > cap) return;
    for (const char *p = o + 1; p < c; p++)
        if (!(isalnum((unsigned char)*p) || strchr("._-\", \n\r\t", *p))) return;
    memcpy(out, o, (size_t)(c - o + 1));
    out[c - o + 1] = 0;
}

static void register_web_app(const st_cfg_t *cfg, const st_app_t *a)
{
    static char reg[200], add[200], man[200], perms[220];   // static: see st_install
    snprintf(reg, sizeof reg, "%s/system/registry/apps.json", cfg->root);
    if (!exists(reg)) return;                                   // no web OS on this card: nothing to list it in
    snprintf(man, sizeof man, "%s/apps/%s/manifest.json", cfg->root, a->id);
    manifest_perms(man, perms, sizeof perms, cfg->buf, cfg->window);
    snprintf(add, sizeof add, "%s/apps/.store/reg-add.json", cfg->root);
    FILE *f = fopen(add, "wb");
    if (!f) return;
    fprintf(f, "{\"installed\":[{\"id\":\"%s\",\"version\":\"%s\",\"path\":\"/apps/%s\",\"enabled\":true,"
               "\"autostart\":false,\"permissions\":%s}]}\n", a->id, a->version, a->id, perms);
    fclose(f);
    int added = 0;
    sdc_registry_merge(reg, add, (char *)cfg->buf, cfg->window, &added);
    unlink(add);
}

st_code_t st_install(const st_cfg_t *cfg, const st_io_t *io, st_state_t *st, const st_app_t *a)
{
    memset(st, 0, sizeof *st);
    st->phase = ST_PH_PACKAGE;
    snprintf(st->cur, sizeof st->cur, "%s", a->id);
    if (!is_dir(cfg->root)) { fail_detail(st, ST_E_NO_SD, cfg->root); return ST_E_NO_SD; }
    if (!st_id_ok(a->id)) { fail_detail(st, ST_E_BAD, a->id); return ST_E_BAD; }
    const char *ch = st_kind_channel(a->kind);
    if (!ch || !channel_ok(cfg->channel) || strcmp(ch, cfg->channel)) { fail_detail(st, ST_E_UNSUPPORTED, a->kind); return ST_E_UNSUPPORTED; }
    if (!strcmp(a->kind, "native")) {
        if (!cfg->native_abi) { fail_detail(st, ST_E_UNSUPPORTED, a->kind); return ST_E_UNSUPPORTED; }
        if (a->abi > cfg->native_abi) { fail_detail(st, ST_E_ABI, a->id); return ST_E_ABI; }
    }
    bool system = false;
    st_installed(cfg->root, a->id, &system);
    if (system) { fail_detail(st, ST_E_CONFLICT, a->id); return ST_E_CONFLICT; }

    // Static, not on the stack: the store task's stack also carries the TLS handshake and the signature check
    // (a 6 KB task overflowed with these 1.4 KB on it). The engine runs one operation at a time.
    static char tmp[200], sig[200], live[200], old[200], url[260], path[300];
    snprintf(tmp, sizeof tmp, "%s/apps/.tmp/%s", cfg->root, a->id);
    snprintf(sig, sizeof sig, "%s/apps/.tmp/%s.sig", cfg->root, a->id);
    snprintf(live, sizeof live, "%s/apps/%s", cfg->root, a->id);
    snprintf(old, sizeof old, "%s/apps/.old/%s", cfg->root, a->id);
    rmtree(tmp);
    unlink(sig);

    // 1. package.sig: must be the one the signed catalog names, and must itself verify.
    uint8_t sha[32];
    char hex[65];
    snprintf(url, sizeof url, "%s%s/apps/%s/%lu/package.sig", cfg->base_url, cfg->channel, a->id, (unsigned long)a->code);
    st_code_t rc = fetch(cfg, io, st, url, sig, MAX_PKGSIG, -1, sha);
    if (rc != ST_OK) goto out;
    sdc_sha256_hex(sha, hex);
    if (strcmp(hex, a->sig_sha)) { rc = ST_E_VERIFY; fail_detail(st, rc, "package.sig"); goto out; }
    int nfiles = 0;
    uint32_t total = 0;
    if ((rc = check_package(io, st, a, sig, &nfiles, &total)) != ST_OK) goto out;

    // 2. Every file, each checked against its signed size and hash.
    st->phase = ST_PH_FILES;
    st->files_total = nfiles;
    st->done = 0;
    st->total = total;
    {
        FILE *f = fopen(sig, "rb");
        if (!f) { rc = ST_E_WRITE; fail_detail(st, rc, "package.sig"); goto out; }
        for (int i = 0; i < 4; i++) if (!fgets(s_line, sizeof s_line, f)) break;
        while (rc == ST_OK && fgets(s_line, sizeof s_line, f) && strcmp(s_line, "--\n")) {
            char want[65], *rel;
            uint32_t sz;
            if (!parse_pkg_file(s_line, want, &sz, &rel)) { rc = ST_E_BAD; fail_detail(st, rc, "package.sig"); break; }
            snprintf(path, sizeof path, "%s/%s", tmp, rel);
            snprintf(url, sizeof url, "%s%s/apps/%s/%lu/%s", cfg->base_url, cfg->channel, a->id, (unsigned long)a->code, rel);
            const char *base = strrchr(rel, '/');
            snprintf(st->cur, sizeof st->cur, "%s", base ? base + 1 : rel);
            if ((rc = fetch(cfg, io, st, url, path, MAX_FILE, sz, sha)) != ST_OK) break;
            sdc_sha256_hex(sha, hex);
            if (strcmp(hex, want)) { rc = ST_E_VERIFY; fail_detail(st, rc, rel); break; }
            st->files_done++;
            progress(io, st);
        }
        fclose(f);
        if (rc != ST_OK) goto out;
    }

    // 3. Mark it as the store's, then swap it in: the old version is parked, never lost until the new one is in.
    st->phase = ST_PH_INSTALL;
    snprintf(path, sizeof path, "%s/.store-version", tmp);
    {
        FILE *v = fopen(path, "wb");
        // What the launcher needs to list it without the catalog: category and its name in every language.
        // "perm" lets the launcher decide a native game's Solo boot before the module is loaded: no network
        // permission = the radio stays off for it (~48 KB and twice the largest block, store_apps.cpp).
        if (!v || fprintf(v, "code %lu\nversion %s\nkind %s\nchannel %s\nabi %lu\ncategory %s\nname %s|%s|%s|%s|%s\nperm %s\n",
                          (unsigned long)a->code, a->version, a->kind, cfg->channel, (unsigned long)a->abi, a->category,
                          a->name[0], a->name[1], a->name[2], a->name[3], a->name[4], a->perms) < 0) {
            if (v) fclose(v);
            rc = ST_E_WRITE; fail_detail(st, rc, ".store-version"); goto out;
        }
        if (fclose(v) != 0) { rc = ST_E_WRITE; fail_detail(st, rc, ".store-version"); goto out; }
    }
    // The signed package travels with the app: st_verify_installed re-checks its files against it later.
    snprintf(path, sizeof path, "%s/.package.sig", tmp);
    if (rename(sig, path) != 0) { rc = ST_E_WRITE; fail_detail(st, rc, ".package.sig"); goto out; }
    rmtree(old);
    bool had = is_dir(live);
    if (had && (!mkdirs(old, false) || rename(live, old) != 0)) { rc = ST_E_WRITE; fail_detail(st, rc, a->id); goto out; }
    if (rename(tmp, live) != 0) {
        if (had) rename(old, live);
        rc = ST_E_WRITE; fail_detail(st, rc, a->id); goto out;
    }
    rmtree(old);
    if (!strcmp(a->kind, "web")) register_web_app(cfg, a);
    st->phase = ST_PH_DONE;
    st->pct = 100;
    progress(io, st);
out:
    unlink(sig);
    if (rc != ST_OK) rmtree(tmp);
    return rc;
}

st_code_t st_remove(const char *root, const char *id)
{
    if (!st_id_ok(id)) return ST_E_BAD;
    bool system = false;
    if (!st_installed(root, id, &system)) return system ? ST_E_CONFLICT : ST_E_NOTFOUND;
    char p[200], reg[200];
    snprintf(p, sizeof p, "%s/apps/%s", root, id);
    rmtree(p);
    if (is_dir(p)) return ST_E_WRITE;
    snprintf(reg, sizeof reg, "%s/system/registry/apps.json", root);
    if (exists(reg)) {
        static char buf[2048];
        int removed = 0;
        sdc_registry_drop(reg, id, buf, sizeof buf, &removed);
    }
    return ST_OK;
}

st_code_t st_verify_installed(const char *root, const char *id, const char *rel, const st_io_t *io,
                              uint8_t *buf, size_t buflen, uint8_t sha_out[32])
{
    if (!st_id_ok(id) || !st_path_ok(rel) || !buf || buflen < 64) return ST_E_BAD;
    char p[200];
    snprintf(p, sizeof p, "%s/apps/%s/.package.sig", root, id);
    FILE *f = fopen(p, "rb");
    if (!f) return ST_E_NOTFOUND;
    char *ln = malloc(LINE_MAX_);                   // not s_line: this runs on the caller's task, the store on its own
    if (!ln) { fclose(f); return ST_E_NOMEM; }
    sdc_sha256_t h;
    sdc_sha256_init(&h);
    char want[65] = "";
    uint32_t want_size = 0;
    int line = 0;
    bool body_end = false, ok = true;
    while (ok && fgets(ln, LINE_MAX_, f)) {
        size_t n = strlen(ln);
        if (!n || ln[n - 1] != '\n') { ok = false; break; }
        if (!strcmp(ln, "--\n")) { body_end = true; break; }
        sdc_sha256_update(&h, ln, n);
        if (line == 0) ok = !strcmp(ln, PKG_MAGIC "\n");
        else if (line == 1) { ln[n - 1] = 0; ok = !strcmp(ln, id); }
        else if (line >= 4) {
            char sha[65], *path; uint32_t sz;
            ok = parse_pkg_file(ln, sha, &sz, &path);
            if (ok && !strcmp(path, rel)) { memcpy(want, sha, sizeof want); want_size = sz; }
        }
        line++;
    }
    char key[17];
    uint8_t der[80], hash[32];
    int dl = 0;
    ok = ok && body_end && line >= 5 && read_sig_block(f, key, der, &dl);
    fclose(f);
    free(ln);
    if (!ok) return ST_E_BAD;
    sdc_sha256_final(&h, hash);
    if (!io->verify(io->ctx, key, hash, der, (size_t)dl)) return ST_E_SIG;
    if (!want[0]) return ST_E_NOTFOUND;

    snprintf(p, sizeof p, "%s/apps/%s/%s", root, id, rel);
    if (!(f = fopen(p, "rb"))) return ST_E_NOTFOUND;
    sdc_sha256_init(&h);
    uint32_t total = 0;
    size_t n;
    while ((n = fread(buf, 1, buflen, f)) > 0) {
        sdc_sha256_update(&h, buf, n);
        total += (uint32_t)n;
        if (io->tick) io->tick(io->ctx);
    }
    fclose(f);
    char hex[65];
    sdc_sha256_final(&h, hash);
    sdc_sha256_hex(hash, hex);
    if (total != want_size || strcmp(hex, want)) return ST_E_VERIFY;
    if (sha_out) memcpy(sha_out, hash, 32);
    return ST_OK;
}
