// game_text — see game_text.h. One shared instance: only one game is ever in the foreground.
//
// Pack format (little endian), written by tools/game-i18n/build.mjs:
//   "GTX1"  u16 count  then count x { u32 fnv1a(english), u16 offset }  sorted by hash,
//   then the NUL-terminated translations the offsets point at (offsets are from the start of the file).
#include "game_text.h"
#include "nucleo_i18n.h"
#include "nucleo_board.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define GT_MAX_PACK 12288                  // a whole game's text in one language is a few KB; refuse anything odd

static uint8_t *s_pack;                    // heap, only while a game is open and its language has a pack
static uint16_t s_n;
static uint32_t s_gen = UINT32_MAX;        // nucleo_i18n_gen() the pack was loaded for (a change reloads it)
static char s_game[16];

static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void drop(void) { free(s_pack); s_pack = NULL; s_n = 0; }

static void load(void)
{
    drop();
    s_gen = nucleo_i18n_gen();
    const char *l = nucleo_i18n_lang();
    if (!s_game[0] || !strcmp(l, "it") || !strcmp(l, "en")) return;   // the two flash languages need no pack
    char path[72];
    snprintf(path, sizeof path, NUCLEO_SD_MOUNT "/system/i18n/games/%s.%.3s", s_game, l);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    long sz = (fseek(f, 0, SEEK_END) == 0) ? ftell(f) : -1;
    if (sz < 6 || sz > GT_MAX_PACK || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return; }
    uint8_t *p = (uint8_t *)malloc((size_t)sz);
    bool ok = p && fread(p, 1, (size_t)sz, f) == (size_t)sz;
    fclose(f);
    // validate everything once here, so game_text() can index without checks: header, table inside the
    // file, every offset inside the string area, and the file ending in a NUL (every string terminates)
    uint16_t n = ok ? rd16(p + 4) : 0;
    ok = ok && !memcmp(p, "GTX1", 4) && 6 + (long)n * 6 <= sz && p[sz - 1] == 0;
    for (uint16_t i = 0; ok && i < n; i++) ok = rd16(p + 6 + i * 6 + 4) >= 6 + n * 6 && rd16(p + 6 + i * 6 + 4) < sz;
    if (!ok) { free(p); return; }
    s_pack = p; s_n = n;
}

void game_text_open(const char *game)
{
    snprintf(s_game, sizeof s_game, "%s", game ? game : "");
    load();
}

void game_text_close(void)
{
    drop();
    s_game[0] = 0;
    s_gen = UINT32_MAX;
}

const char *game_text(const char *it, const char *en)
{
    if (s_game[0] && s_gen != nucleo_i18n_gen()) load();             // the OS language changed while playing
    const char *l = nucleo_i18n_lang();
    if (!strcmp(l, "it")) return it;
    if (!s_pack || !strcmp(l, "en")) return en;
    uint32_t h = fnv1a(en);
    int lo = 0, hi = (int)s_n - 1;
    while (lo <= hi) {                                                // binary search: the table is sorted by hash
        int mid = (lo + hi) / 2;
        uint32_t m = rd32(s_pack + 6 + mid * 6);
        if (m == h) return (const char *)s_pack + rd16(s_pack + 6 + mid * 6 + 4);
        if (m < h) lo = mid + 1; else hi = mid - 1;
    }
    return en;
}
