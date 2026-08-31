// update_policy.c — pure decision core for the native release-update flow. See update_policy.h.
// Compiled both into the firmware and, unmodified, by the host gate (npm run update:test).
#include "update_policy.h"
#include <string.h>
#include <ctype.h>

bool upd_parse_semver(const char *s, upd_semver_t *out)
{
    if (!s) return false;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == 'v' || *s == 'V') s++;
    int part[3];
    for (int i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)*s)) return false;
        long v = 0;
        while (isdigit((unsigned char)*s)) {
            v = v * 10 + (*s - '0');
            if (v > 1000000) return false;          // absurd component = not a version
            s++;
        }
        part[i] = (int)v;
        if (i < 2) { if (*s != '.') return false; s++; }
    }
    // Whatever follows the triplet (+build.g<hash>, .<build>, '*') is metadata — ignored.
    if (out) { out->maj = part[0]; out->min = part[1]; out->pat = part[2]; }
    return true;
}

int upd_cmp(const char *a, const char *b)
{
    upd_semver_t va, vb;
    if (!upd_parse_semver(a, &va) || !upd_parse_semver(b, &vb)) return 0;
    if (va.maj != vb.maj) return va.maj < vb.maj ? -1 : 1;
    if (va.min != vb.min) return va.min < vb.min ? -1 : 1;
    if (va.pat != vb.pat) return va.pat < vb.pat ? -1 : 1;
    return 0;
}

bool upd_extract_tag(const char *json, char *out, size_t cap)
{
    if (!json || !out || cap < 2) return false;
    const char *k = strstr(json, "\"tag\"");
    if (!k) return false;
    k += 5;
    while (*k == ' ' || *k == '\t' || *k == '\r' || *k == '\n') k++;
    if (*k != ':') return false;
    k++;
    while (*k == ' ' || *k == '\t' || *k == '\r' || *k == '\n') k++;
    if (*k != '"') return false;
    k++;
    size_t n = 0;
    while (k[n] && k[n] != '"') {
        if (n + 1 >= cap) return false;            // would truncate: refuse, don't mangle
        out[n] = k[n];
        n++;
    }
    if (k[n] != '"' || n == 0) return false;
    out[n] = 0;
    return true;
}

bool upd_find_sha256(const char *sums, const char *name, char *out_hex65)
{
    if (!sums || !name || !name[0] || !out_hex65) return false;
    const size_t nlen = strlen(name);
    const char *p = sums;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
        while (linelen && (p[linelen - 1] == '\r' || p[linelen - 1] == ' ')) linelen--;
        // "<64 hex><spaces>[*]<name>"
        size_t h = 0;
        while (h < linelen && isxdigit((unsigned char)p[h])) h++;
        if (h == 64) {
            size_t q = h;
            while (q < linelen && (p[q] == ' ' || p[q] == '\t')) q++;
            if (q < linelen && p[q] == '*') q++;
            if (linelen - q == nlen && strncmp(p + q, name, nlen) == 0) {
                for (int i = 0; i < 64; i++) out_hex65[i] = (char)tolower((unsigned char)p[i]);
                out_hex65[64] = 0;
                return true;
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    return false;
}

bool upd_should_show(const char *current, const char *latest, const char *dismissed)
{
    if (!latest || !latest[0]) return false;
    upd_semver_t tmp;
    if (!upd_parse_semver(latest, &tmp) || !upd_parse_semver(current, &tmp)) return false;
    if (upd_cmp(current, latest) >= 0) return false;    // equal or dev-build-ahead: quiet
    if (dismissed && dismissed[0] && strcmp(dismissed, latest) == 0) return false;
    return true;
}

bool upd_check_due(uint32_t now_s, uint32_t last_s, uint32_t interval_s)
{
    if (last_s == 0) return true;
    if (now_s < last_s) return true;                    // clock went backwards: never wedge
    return (now_s - last_s) >= interval_s;
}
