// SD content self-install — the engine. See sdc_engine.h. Pure C + stdio (no ESP-IDF): compiled unchanged
// into the firmware and into the host gate, which drives it against a fake server with the device's TLS
// limit and injected faults (tools/anima-host/sdcontent-e2e.mjs).
#include "sdc_engine.h"
#include "content_policy.h"
#include "sdc_sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#define SDC_MKDIR(p) _mkdir(p)
#else
#define SDC_MKDIR(p) mkdir((p), 0775)
#endif

#define SDC_LINE_MAX 384          // a manifest line: 64 hex + size + pack + mode + a <=200-char path

typedef struct {
    const sdc_cfg_t *cfg;
    const sdc_io_t  *io;
    sdc_state_t     *st;
    uint8_t         *buf;         // one window, malloc'd for the run only (no permanent RAM in the full OS)
    uint64_t         recv;        // body bytes received this run
    uint64_t         total;       // payload bytes of the selected packs
    uint64_t         done;        // payload bytes of the files finished (installed or present)
    unsigned         windows;     // windows received (progress pacing)
    bool             payload;     // the fetch in flight is a payload file (counts toward done)
} eng_t;

static void tick(eng_t *e) { if (e->io->tick) e->io->tick(e->io->ctx); }
static void prog_at(eng_t *e, uint32_t in_flight)
{
    e->st->recv_kb = (int)(e->recv / 1024);
    if (e->total) {
        uint64_t d = e->done + in_flight;
        if (d > e->total) d = e->total;
        e->st->kb_done = (uint32_t)(d / 1024);
        if (e->st->phase == SDC_DOWNLOADING) e->st->pct = (int)(d * 100 / e->total);
    }
    if (e->io->progress) e->io->progress(e->io->ctx, e->st);
}
static void prog(eng_t *e)
{
    e->st->recv_kb = (int)(e->recv / 1024);
    if (e->io->progress) e->io->progress(e->io->ctx, e->st);
}

static bool fail(eng_t *e, sdc_code_t code, const char *msg, const char *detail)
{
    e->st->code = code;
    e->st->phase = SDC_FAILED;
    snprintf(e->st->err, sizeof e->st->err, "%s", msg);
    if (detail) snprintf(e->st->detail, sizeof e->st->detail, "%.63s", detail);
    prog(e);
    return false;
}

static bool pack_wanted(const char *packs, const char *pack)
{
    size_t pl = strlen(pack);
    for (const char *p = packs; p && *p; ) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len == pl && strncmp(p, pack, pl) == 0) return true;
        p += len; if (*p == ',') p++;
    }
    return false;
}

static void join(char *out, size_t cap, const char *root, const char *rel) { snprintf(out, cap, "%s/%s", root, rel); }

// base + path, percent-encoding anything outside RFC 3986 "unreserved" and '/' (a space or '#' in a file name
// must not break the request line).
static void url_join(char *out, size_t cap, const char *base, const char *path)
{
    int k = snprintf(out, cap, "%s", base);
    if (k < 0 || (size_t)k >= cap) { out[cap - 1] = 0; return; }
    for (const unsigned char *p = (const unsigned char *)path; *p && (size_t)k + 4 < cap; p++) {
        unsigned char ch = *p;
        bool keep = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                    ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '/';
        if (keep) out[k++] = (char)ch;
        else k += snprintf(out + k, cap - (size_t)k, "%%%02X", ch);
    }
    out[k] = 0;
}

// Create every directory on the way to `abspath` (the last component is the file).
static void mkdir_parents(const char *abspath)
{
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s", abspath);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; SDC_MKDIR(tmp); *p = '/'; }
    }
}

static bool file_size(const char *path, uint32_t *size)
{
    struct stat sb;
    if (stat(path, &sb) != 0 || !S_ISREG(sb.st_mode)) return false;
    *size = (uint32_t)sb.st_size;
    return true;
}

static bool file_sha(eng_t *e, const char *path, char hex[65])
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    sdc_sha256_t c; sdc_sha256_init(&c);
    size_t r;
    while ((r = fread(e->buf, 1, e->cfg->window, f)) > 0) { sdc_sha256_update(&c, e->buf, r); tick(e); }
    fclose(f);
    uint8_t d[32]; sdc_sha256_final(&c, d); sdc_sha256_hex(d, hex);
    return true;
}

// Stream `url` into `part` window by window. known=true: exactly `expect` bytes are requested (the last
// window is trimmed); known=false: windows until a short read / 416 marks the end. Every transport failure
// retries THAT window (a dropped keep-alive costs one window, not the file). On any error the .part is
// removed. Returns SDC_OK, SDC_E_FETCH (st->http set) or SDC_E_WRITE.
static sdc_code_t fetch_to_file(eng_t *e, const char *url, const char *part, bool known, uint32_t expect,
                                char sha_hex[65], uint32_t *out_bytes)
{
    FILE *f = fopen(part, "wb");
    if (!f) return SDC_E_WRITE;
    sdc_sha256_t c; sdc_sha256_init(&c);
    uint32_t off = 0;
    sdc_code_t rc = SDC_OK;
    for (;;) {
        uint32_t want = e->cfg->window;
        if (known) { if (off >= expect) break; if (expect - off < want) want = expect - off; }
        int status = 0, n = -1;
        for (int t = 0; t <= e->cfg->retries; t++) {
            tick(e);
            e->st->requests++;
            status = 0;
            n = e->io->get(e->io->ctx, url, off, want, e->buf, &status);
            if (n >= 0) break;
            if (status >= 400 && status < 500 && status != 408 && status != 429) break;   // permanent (404, 416, ...)
        }
        if (n < 0) {
            if (status == 416 && !known && off > 0) break;      // past the end at an exact window boundary
            e->st->http = status; rc = SDC_E_FETCH; break;
        }
        if ((uint32_t)n > want) { e->st->http = status; rc = SDC_E_FETCH; break; }   // transport broke the contract
        if (n > 0 && fwrite(e->buf, 1, (size_t)n, f) != (size_t)n) { rc = SDC_E_WRITE; break; }
        sdc_sha256_update(&c, e->buf, (size_t)n);
        off += (uint32_t)n;
        e->recv += (uint32_t)n;
        if ((++e->windows & 7) == 0) prog_at(e, e->payload ? off : 0);   // keep a big file's bar moving (~48 KB)
        if ((uint32_t)n < want) break;                          // short read: the resource ended
    }
    if (fclose(f) != 0 && rc == SDC_OK) rc = SDC_E_WRITE;
    if (rc != SDC_OK) { unlink(part); return rc; }
    uint8_t d[32]; sdc_sha256_final(&c, d); sdc_sha256_hex(d, sha_hex);
    *out_bytes = off;
    return SDC_OK;
}

// Download, verify (size + SHA-256) and atomically install one file; a mismatch is retried once.
static sdc_code_t install_file(eng_t *e, const sdc_file_t *f)
{
    char url[400], dst[300], part[306], got[65];
    url_join(url, sizeof url, e->cfg->base_url, f->path);
    join(dst, sizeof dst, e->cfg->root, f->path);
    snprintf(part, sizeof part, "%s.part", dst);
    mkdir_parents(dst);
    for (int attempt = 0; attempt < 2; attempt++) {
        uint32_t n = 0;
        e->payload = true;
        sdc_code_t rc = fetch_to_file(e, url, part, true, f->size, got, &n);
        e->payload = false;
        if (rc != SDC_OK) return rc;
        if (n == f->size && strncmp(got, f->sha, 64) == 0) {
            unlink(dst);                                        // FAT/Windows rename does not replace
            if (rename(part, dst) != 0) { unlink(part); return SDC_E_WRITE; }
            return SDC_OK;
        }
        unlink(part);
    }
    return SDC_E_VERIFY;
}

static const char *basename_of(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }

bool sdc_engine_run(const sdc_cfg_t *cfg, const sdc_io_t *io, sdc_state_t *st)
{
    memset(st, 0, sizeof *st);
    st->phase = SDC_CHECKING; st->pct = -1;
    eng_t E = { .cfg = cfg, .io = io, .st = st, .buf = NULL }, *e = &E;
    prog(e);

    // 1) The card: present AND writable (a real write/read/delete — a mounted card can still be read-only).
    struct stat sb;
    if (stat(cfg->root, &sb) != 0) return fail(e, SDC_E_NO_SD, "no SD card", cfg->root);
    char p1[300];
    join(p1, sizeof p1, cfg->root, "system"); SDC_MKDIR(p1);
    join(p1, sizeof p1, cfg->root, "system/content"); SDC_MKDIR(p1);
    join(p1, sizeof p1, cfg->root, "system/.sdc-write-test");
    FILE *pf = fopen(p1, "wb");
    if (!pf || fwrite("ok", 1, 2, pf) != 2) { if (pf) fclose(pf); unlink(p1); return fail(e, SDC_E_SD_RO, "SD not writable", NULL); }
    if (fclose(pf) != 0) { unlink(p1); return fail(e, SDC_E_SD_RO, "SD not writable", NULL); }
    unlink(p1);

    uint32_t w = cfg->window < 1024 ? 1024 : cfg->window > 16384 ? 16384 : cfg->window;
    sdc_cfg_t c2 = *cfg; c2.window = w; e->cfg = &c2;
    e->buf = (uint8_t *)malloc(w);
    if (!e->buf) return fail(e, SDC_E_NOMEM, "out of memory", NULL);
    bool ok = false;

    // 2) The manifest, windowed like everything else (it is 150 KB: one GET would be 16 KB TLS records).
    char murl[400], mpart[300], mfile[300], msha[65];
    url_join(murl, sizeof murl, cfg->base_url, "sd-manifest.txt");
    join(mpart, sizeof mpart, cfg->root, "system/content/manifest.part");
    join(mfile, sizeof mfile, cfg->root, "system/content/manifest.txt");
    uint32_t mbytes = 0;
    sdc_code_t rc = fetch_to_file(e, murl, mpart, false, 0, msha, &mbytes);
    if (rc != SDC_OK) { fail(e, rc == SDC_E_WRITE ? SDC_E_WRITE : SDC_E_MANIFEST,
                             rc == SDC_E_WRITE ? "SD write failed" : "manifest download failed", "sd-manifest.txt"); goto out; }
    unlink(mfile);
    if (rename(mpart, mfile) != 0) { unlink(mpart); fail(e, SDC_E_WRITE, "SD write failed", "manifest.txt"); goto out; }

    // 3) Header + count pass. Any malformed line or path outside the allow-list fails the run BEFORE a single
    //    payload byte is written (a hostile manifest must not write anywhere).
    FILE *mf = fopen(mfile, "r");
    if (!mf) { fail(e, SDC_E_WRITE, "SD read failed", "manifest.txt"); goto out; }
    char line[SDC_LINE_MAX];
    sdc_header_t hdr; bool have_hdr = false;
    int total = 0;
    while (fgets(line, sizeof line, mf)) {
        if (!have_hdr && sdc_parse_header(line, &hdr)) {
            have_hdr = true;
            if (!sdc_tag_matches(cfg->ver3, hdr.tag)) { fclose(mf); fail(e, SDC_E_TAG, "content is for another firmware", hdr.tag); goto out; }
            continue;
        }
        sdc_file_t f; bool cmt;
        if (sdc_parse_file(line, &f, &cmt)) { if (pack_wanted(cfg->packs, f.pack)) { total++; e->total += f.size; } }
        else if (!cmt) { line[strcspn(line, "\r\n")] = 0; fclose(mf); fail(e, SDC_E_MANIFEST_BAD, "invalid manifest", line); goto out; }
    }
    if (!have_hdr) { fclose(mf); fail(e, SDC_E_MANIFEST_BAD, "invalid manifest", "no header"); goto out; }
    sdc_content_write(cfg->root, cfg->ver3, false, false);   // "started": an interrupted run reads as PARTIAL
    st->files_total = total; st->kb_total = (uint32_t)(e->total / 1024);
    st->phase = SDC_DOWNLOADING; st->pct = 0; prog_at(e, 0);

    // 4) Install pass.
    rewind(mf);
    while (fgets(line, sizeof line, mf)) {
        sdc_file_t f; bool cmt;
        if (!sdc_parse_file(line, &f, &cmt)) continue;          // header / #pack / blank
        if (!pack_wanted(cfg->packs, f.pack)) continue;         // optional pack, not selected
        snprintf(st->cur, sizeof st->cur, "%.39s", basename_of(f.path));
        char dst[300]; join(dst, sizeof dst, cfg->root, f.path);
        uint32_t dsz = 0; bool on_disk = file_size(dst, &dsz);
        char dsha[65]; bool have_sha = on_disk && dsz == f.size && file_sha(e, dst, dsha);
        sdc_plan_t pl = sdc_plan(&f, on_disk, dsz, have_sha ? dsha : NULL);
        // MERGE (the app registry): written only when absent — an existing one is the device's live list.
        bool need = pl == SDC_PLAN_WRITE || (pl == SDC_PLAN_MERGE && !on_disk);
        if (need) {
            rc = install_file(e, &f);
            if (rc != SDC_OK) {
                fclose(mf);
                fail(e, rc, rc == SDC_E_VERIFY ? "file verification failed" : rc == SDC_E_WRITE ? "SD write failed" : "download failed", f.path);
                goto out;
            }
            st->files_written++;
        }
        st->files_done++;
        e->done += f.size;
        if (!e->total) st->pct = total > 0 ? (st->files_done * 100) / total : 100;   // an all-empty payload
        prog_at(e, 0);
    }
    fclose(mf);

    sdc_content_write(cfg->root, cfg->ver3, true, false);
    st->phase = SDC_DONE; st->pct = 100; st->code = SDC_OK; st->kb_done = st->kb_total;
    prog(e);
    ok = true;
out:
    free(e->buf);
    return ok;
}

// ---- content.json -----------------------------------------------------------------------------------

bool sdc_content_read(const char *root, char *tag, size_t tagcap, bool *complete, bool *declined)
{
    if (complete) *complete = false;
    if (declined) *declined = false;
    if (tag && tagcap) tag[0] = 0;
    char path[300]; join(path, sizeof path, root, "system/content.json");
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char buf[192]; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f);
    buf[n] = 0;
    const char *t = strstr(buf, "\"tag\"");
    if (t && tag && tagcap) { t = strchr(t, ':'); if (t) t = strchr(t, '"');
        if (t) { t++; size_t k = 0; while (t[k] && t[k] != '"' && k < tagcap - 1) { tag[k] = t[k]; k++; } tag[k] = 0; } }
    if (complete) *complete = strstr(buf, "\"complete\":true") != NULL;
    if (declined) *declined = strstr(buf, "\"declined\":true") != NULL;
    return true;
}

void sdc_content_write(const char *root, const char *tag, bool complete, bool declined)
{
    char path[300]; join(path, sizeof path, root, "system"); SDC_MKDIR(path);
    join(path, sizeof path, root, "system/content.json");
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "{\"tag\":\"%s\",\"complete\":%s,\"declined\":%s}\n",
            tag, complete ? "true" : "false", declined ? "true" : "false");
    fclose(f);
}

// The release named by root/system/content/manifest.txt's header ("" when absent / not a manifest).
static bool manifest_tag(const char *root, char *tag, size_t cap)
{
    char path[300]; join(path, sizeof path, root, "system/content/manifest.txt");
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[SDC_LINE_MAX]; bool ok = false;
    if (fgets(line, sizeof line, f)) {
        sdc_header_t h;
        if (sdc_parse_header(line, &h)) { snprintf(tag, cap, "%.23s", h.tag[0] == 'v' ? h.tag + 1 : h.tag); ok = true; }
    }
    fclose(f);
    return ok;
}

sdc_status_t sdc_content_status(const char *root, const char *ver3, char *tag, size_t tagcap)
{
    char dummy[24]; if (!tag || !tagcap) { tag = dummy; tagcap = sizeof dummy; }
    tag[0] = 0;
    char ctag[24]; bool complete = false, declined = false;
    bool have_cj = sdc_content_read(root, ctag, sizeof ctag, &complete, &declined);
    bool cj_mine = have_cj && sdc_tag_matches(ver3, ctag);
    if (cj_mine && complete)               { snprintf(tag, tagcap, "%s", ver3); return SDC_ST_COMPLETE; }
    if (cj_mine && !declined)              { snprintf(tag, tagcap, "%s", ver3); return SDC_ST_PARTIAL; }
    // The files themselves: is the web OS on the card (by hand, or a previous install)?
    char p[300]; uint32_t sz = 0;
    join(p, sizeof p, root, "www/shell/index.html");
    bool shell = file_size(p, &sz) && sz > 0;
    if (!shell) { join(p, sizeof p, root, "www/shell/index.html.gz"); shell = file_size(p, &sz) && sz > 0; }
    if (shell) {
        char mtag[24];
        if (manifest_tag(root, mtag, sizeof mtag)) {
            snprintf(tag, tagcap, "%s", mtag);
            return sdc_tag_matches(ver3, mtag) ? SDC_ST_MANUAL : SDC_ST_OUTDATED;
        }
        if (have_cj && complete)           { snprintf(tag, tagcap, "%s", ctag); return SDC_ST_OUTDATED; }
        return SDC_ST_UNKNOWN;
    }
    return cj_mine && declined ? SDC_ST_SKIPPED : SDC_ST_MISSING;
}

bool sdc_content_needed(const char *root, const char *ver3)
{
    sdc_status_t s = sdc_content_status(root, ver3, NULL, 0);
    return s == SDC_ST_MISSING || s == SDC_ST_PARTIAL || s == SDC_ST_OUTDATED;
}
