// SD content self-install — the app-registry merge. See sdc_registry.h.
// A tiny streaming JSON walker: it only needs to find the top-level "installed" array, step over its
// elements (strings and nesting handled, escapes respected) and read each object's own "id".
#include "sdc_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

#define SDC_REG_MAX_IDS 256      // apps on a device; ids are kept as 32-bit hashes (1 KB)

static uint32_t fnv1a(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h;
}

static bool is_ws(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Read up to and including the '[' that opens the top-level "installed" array; every byte read is copied to
// `echo` when given. false = no such array (or the file ended).
static bool seek_installed(FILE *f, FILE *echo)
{
    int depth = 0, c;
    bool str = false, esc = false, key_ready = false, want_array = false;
    char key[16]; size_t kl = 0; bool capturing = false;
    while ((c = fgetc(f)) != EOF) {
        if (echo) fputc(c, echo);
        if (str) {
            if (esc) { esc = false; if (capturing && kl < sizeof key - 1) key[kl++] = (char)c; continue; }
            if (c == '\\') { esc = true; continue; }
            if (c == '"') { str = false; if (capturing) { key[kl] = 0; key_ready = true; capturing = false; } continue; }
            if (capturing) { if (kl < sizeof key - 1) key[kl++] = (char)c; else capturing = false; }
            continue;
        }
        if (is_ws(c)) continue;
        if (c == '"') { str = true; capturing = depth == 1; kl = 0; key_ready = false; continue; }
        if (c == ':') { want_array = depth == 1 && key_ready && strcmp(key, "installed") == 0; key_ready = false; continue; }
        if (c == '[') { if (want_array && depth == 1) return true; depth++; want_array = false; continue; }
        if (c == '{') { depth++; want_array = false; continue; }
        if (c == '}' || c == ']') { depth--; want_array = false; if (depth < 0) return false; continue; }
        want_array = false; key_ready = false;   // a number / true / false / null / ','
    }
    return false;
}

// Positioned just inside the array: the next element. 1 = an object, copied into buf (len in *len);
// 2 = a non-object element (skipped); 0 = the array closed (its ']' consumed); -1 = malformed / too big.
static int next_entry(FILE *f, char *buf, size_t cap, size_t *len)
{
    int c;
    do { c = fgetc(f); } while (c != EOF && (is_ws(c) || c == ','));
    if (c == EOF) return -1;
    if (c == ']') return 0;
    bool obj = c == '{';
    size_t n = 0; int depth = 0; bool str = false, esc = false;
    for (;;) {
        if (obj) { if (n >= cap) return -1; buf[n++] = (char)c; }
        if (str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') { str = false; if (!obj && depth == 0) break; }
        } else if (c == '"') str = true;
        else if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') { depth--; if (depth == 0) break; if (depth < 0) return -1; }
        else if (!obj && depth == 0) {                       // a bare scalar ends at ',' / ']' / whitespace
            int d = fgetc(f);
            if (d == EOF) return -1;
            if (d == ',' || is_ws(d)) break;
            if (d == ']') { ungetc(d, f); break; }
            c = d; continue;
        }
        c = fgetc(f);
        if (c == EOF) return -1;
    }
    *len = n;
    return obj ? 1 : 2;
}

// The object's own top-level "id" string (escapes kept verbatim; ids are plain ASCII). false = none.
static bool entry_id(const char *s, size_t n, const char **id, size_t *idlen)
{
    int depth = 0; bool str = false, esc = false, key_is_id = false, take = false;
    size_t ks = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (str) {
            if (esc) { esc = false; continue; }
            if (c == '\\') { esc = true; continue; }
            if (c == '"') {
                str = false;
                if (take) { *id = s + ks; *idlen = i - ks; return true; }
                if (depth == 1) key_is_id = (i - ks == 2 && strncmp(s + ks, "id", 2) == 0);
            }
            continue;
        }
        if (c == '"') { str = true; ks = i + 1; continue; }
        if (c == ':') { take = depth == 1 && key_is_id; key_is_id = false; if (take) { size_t j = i + 1; while (j < n && is_ws((unsigned char)s[j])) j++; if (j >= n || s[j] != '"') take = false; } continue; }
        if (c == '{' || c == '[') { depth++; continue; }
        if (c == '}' || c == ']') { depth--; continue; }
        if (c == ',') { key_is_id = false; take = false; }
    }
    return false;
}

static bool has_hash(const uint32_t *h, int n, uint32_t v) { for (int i = 0; i < n; i++) if (h[i] == v) return true; return false; }

sdc_reg_result_t sdc_registry_merge(const char *device_path, const char *release_path,
                                    char *buf, size_t cap, int *added)
{
    *added = 0;
    char addp[310], outp[310];
    snprintf(addp, sizeof addp, "%s.add", device_path);
    snprintf(outp, sizeof outp, "%s.merge", device_path);
    uint32_t *ids = (uint32_t *)malloc(SDC_REG_MAX_IDS * sizeof *ids);
    if (!ids) return SDC_REG_IO;
    int nids = 0, nadd = 0, r;
    sdc_reg_result_t res = SDC_REG_UNCHANGED;
    FILE *dev = NULL, *rel = NULL, *add = NULL, *out = NULL;
    size_t len;

    // 1) The ids the device already has.
    if (!(dev = fopen(device_path, "rb")) || !seek_installed(dev, NULL)) { res = SDC_REG_DEVICE_BAD; goto done; }
    while ((r = next_entry(dev, buf, cap, &len)) > 0) {
        const char *id; size_t il;
        if (r == 1 && entry_id(buf, len, &id, &il) && nids < SDC_REG_MAX_IDS) ids[nids++] = fnv1a(id, il);
    }
    if (r < 0) { res = SDC_REG_DEVICE_BAD; goto done; }
    fclose(dev); dev = NULL;

    // 2) The release entries it lacks, length-prefixed into <device>.add.
    if (!(rel = fopen(release_path, "rb")) || !seek_installed(rel, NULL)) { res = SDC_REG_RELEASE_BAD; goto done; }
    if (!(add = fopen(addp, "wb"))) { res = SDC_REG_IO; goto done; }
    while ((r = next_entry(rel, buf, cap, &len)) > 0) {
        const char *id; size_t il;
        if (r != 1 || !entry_id(buf, len, &id, &il)) continue;
        uint32_t h = fnv1a(id, il);
        if (has_hash(ids, nids, h)) continue;
        uint32_t l32 = (uint32_t)len;
        if (fwrite(&l32, sizeof l32, 1, add) != 1 || fwrite(buf, 1, len, add) != len) { res = SDC_REG_IO; goto done; }
        if (nids < SDC_REG_MAX_IDS) ids[nids++] = h;            // a release listing an id twice adds it once
        nadd++;
    }
    if (r < 0) { res = SDC_REG_RELEASE_BAD; goto done; }
    fclose(rel); rel = NULL;
    if (fclose(add) != 0) { add = NULL; res = SDC_REG_IO; goto done; }
    add = NULL;
    if (nadd == 0) goto done;                                   // nothing new: the device file stays as it is

    // 3) Copy the device file, appending the new entries just before the array's ']' (after its last element,
    //    before the whitespace that precedes the bracket, so the file keeps its shape).
    if (!(dev = fopen(device_path, "rb")) || !(add = fopen(addp, "rb")) || !(out = fopen(outp, "wb"))) { res = SDC_REG_IO; goto done; }
    if (!seek_installed(dev, out)) { res = SDC_REG_DEVICE_BAD; goto done; }
    {
        char pend[64]; size_t pn = 0;
        bool has_elem = false, str = false, esc = false, closed = false;
        int depth = 0, c;
        while ((c = fgetc(dev)) != EOF) {
            if (!str && depth == 0) {
                if (is_ws(c)) { if (pn < sizeof pend) { pend[pn++] = (char)c; continue; } fwrite(pend, 1, pn, out); pn = 0; fputc(c, out); continue; }
                if (c == ']') {
                    for (int k = 0; k < nadd; k++) {
                        uint32_t l32;
                        if (fread(&l32, sizeof l32, 1, add) != 1 || l32 > cap || fread(buf, 1, l32, add) != l32) { res = SDC_REG_IO; goto done; }
                        fputs(has_elem || k > 0 ? ",\n    " : "\n    ", out);
                        fwrite(buf, 1, l32, out);
                    }
                    fwrite(pend, 1, pn, out); pn = 0;
                    fputc(']', out);
                    while ((c = fgetc(dev)) != EOF) fputc(c, out);   // the rest of the file, verbatim
                    closed = true;
                    break;
                }
                if (pn) { fwrite(pend, 1, pn, out); pn = 0; }
                if (c != ',') has_elem = true;
            }
            fputc(c, out);
            if (str) { if (esc) esc = false; else if (c == '\\') esc = true; else if (c == '"') str = false; }
            else if (c == '"') str = true;
            else if (c == '{' || c == '[') depth++;
            else if (c == '}' || c == ']') depth--;
        }
        if (!closed) { res = SDC_REG_DEVICE_BAD; goto done; }   // the array never closed: not a registry
    }
    fclose(dev); dev = NULL;
    if (fclose(out) != 0) { out = NULL; res = SDC_REG_IO; goto done; }
    out = NULL;
    // FAT/Windows rename does not replace, and removing the live list first would lose it if the rename then
    // failed: park it as .old, move the merged file in, put .old back on any failure.
    {
        char oldp[310];
        snprintf(oldp, sizeof oldp, "%s.old", device_path);
        unlink(oldp);
        if (rename(device_path, oldp) != 0) { res = SDC_REG_IO; goto done; }
        if (rename(outp, device_path) != 0) { rename(oldp, device_path); res = SDC_REG_IO; goto done; }
        unlink(oldp);
    }
    *added = nadd;
    res = SDC_REG_MERGED;

done:
    if (dev) fclose(dev);
    if (rel) fclose(rel);
    if (add) fclose(add);
    if (out) fclose(out);
    unlink(addp);
    if (res != SDC_REG_MERGED) unlink(outp);
    free(ids);
    return res;
}

sdc_reg_result_t sdc_registry_drop(const char *device_path, const char *id, char *buf, size_t cap, int *removed)
{
    *removed = 0;
    char outp[310], oldp[310];
    snprintf(outp, sizeof outp, "%s.drop", device_path);
    snprintf(oldp, sizeof oldp, "%s.old", device_path);
    size_t want = strlen(id);
    FILE *dev = fopen(device_path, "rb"), *out = NULL;
    sdc_reg_result_t res = SDC_REG_UNCHANGED;
    size_t len;
    int r, kept = 0;
    if (!dev) return SDC_REG_DEVICE_BAD;
    if (!(out = fopen(outp, "wb"))) { res = SDC_REG_IO; goto done; }
    if (!seek_installed(dev, out)) { res = SDC_REG_DEVICE_BAD; goto done; }
    // Re-emit every object element except the one with this id; non-object elements are not registry entries.
    while ((r = next_entry(dev, buf, cap, &len)) > 0) {
        const char *eid; size_t il;
        if (r != 1) continue;
        if (entry_id(buf, len, &eid, &il) && il == want && !strncmp(eid, id, il)) { (*removed)++; continue; }
        fputs(kept++ ? ",\n    " : "\n    ", out);
        fwrite(buf, 1, len, out);
    }
    if (r < 0) { res = SDC_REG_DEVICE_BAD; goto done; }
    fputs(kept ? "\n  ]" : "]", out);
    { int c; while ((c = fgetc(dev)) != EOF) fputc(c, out); }   // the rest of the file, verbatim
    fclose(dev); dev = NULL;
    if (fclose(out) != 0) { out = NULL; res = SDC_REG_IO; goto done; }
    out = NULL;
    if (!*removed) goto done;
    unlink(oldp);
    if (rename(device_path, oldp) != 0) { res = SDC_REG_IO; goto done; }
    if (rename(outp, device_path) != 0) { rename(oldp, device_path); res = SDC_REG_IO; goto done; }
    unlink(oldp);
    res = SDC_REG_MERGED;
done:
    if (dev) fclose(dev);
    if (out) fclose(out);
    unlink(outp);
    return res;
}
