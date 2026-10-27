// Host driver for the native-app loader gate (tools/native/test.mjs). Runs the REAL nap_loader.c.
//   nap_test relocate <file.nap> <exports.txt> <layout hex> <id> <out.flash> <out.data>
//       inspect + relocate the whole module with fixed bases; writes the flash image and the data segment
//   nap_test reject <file.nap> <exports.txt> <layout hex> <id>
//       targeted corruptions, each must be refused with the right code; then random mutations, none may crash
// exports.txt: one name per line, already in strcmp order; export i gets address 0x40000000 + 16 * i.
#include "nap_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IBUS 0x42010000u
#define DBUS 0x3c010000u
#define RAMB 0x3fc90000u

static uint8_t *g_file;
static uint32_t g_size;
static nap_export_t *g_ex;
static uint32_t g_nex;

static int mem_read(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    const uint8_t *f = ctx;
    if (off > g_size || len > g_size - off) return -1;
    memcpy(buf, f + off, len);
    return 0;
}

static uint8_t *slurp(const char *p, uint32_t *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", p); exit(2); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)sz + 1);
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) exit(2);
    fclose(f);
    *n = (uint32_t)sz;
    return b;
}

static void load_exports(const char *p)
{
    uint32_t n;
    char *t = (char *)slurp(p, &n);
    t[n] = 0;
    g_ex = calloc(1024, sizeof *g_ex);
    for (char *l = strtok(t, "\r\n"); l && g_nex < 1024; l = strtok(NULL, "\r\n")) {
        g_ex[g_nex].name = l;
        g_ex[g_nex].addr = 0x40000000u + 16u * g_nex;
        g_nex++;
    }
}

static nap_src_t src_of(const uint8_t *file, uint32_t size, uint32_t layout, const char *id)
{
    nap_src_t s = { 0 };
    s.ctx = (void *)file; s.read = mem_read; s.size = size;
    s.exports = g_ex; s.n_exports = g_nex; s.abi_max = 1; s.layout = layout; s.expect_id = id;
    s.flash_max = 252 * 1024; s.ram_max = 48 * 1024;
    return s;
}

static int g_cache_relocs;     // 1: relocation tables read once into RAM (the device path)
static uint8_t *g_relocs;
static int g_ticks;
static void count_tick(void *ctx) { (void)ctx; g_ticks++; }

static nap_rc_t full_load(const uint8_t *file, uint32_t size, uint32_t layout, const char *id, nap_image_t *img,
                          uint8_t **flash, uint8_t **ram, char *detail)
{
    uint32_t keep = g_size;
    g_size = size;
    nap_src_t s = src_of(file, size, layout, id);
    s.tick = count_tick;
    nap_rc_t rc = nap_inspect(&s, img, detail, 64);
    if (rc == NAP_OK && g_cache_relocs) {
        free(g_relocs);
        g_relocs = malloc(nap_relocs_bytes(img) + 4);
        rc = nap_cache_relocs(&s, img, g_relocs, nap_relocs_bytes(img));
    }
    if (rc == NAP_OK) {
        nap_bases_t b = { IBUS, DBUS, RAMB - img->data_start };
        *flash = calloc(1, img->image_size + 4);
        *ram = calloc(1, img->data_memsz + 4);
        for (uint32_t off = 0; rc == NAP_OK && off < img->image_size; off += 4096) {
            uint32_t n = img->image_size - off < 4096 ? img->image_size - off : 4096;
            rc = nap_relocate_flash(&s, img, &b, off, *flash + off, n, detail, 64);
        }
        if (rc == NAP_OK) rc = nap_relocate_data(&s, img, &b, *ram, detail, 64);
    }
    g_size = keep;
    return rc;
}

static int fails;
static void expect(const char *what, nap_rc_t got, nap_rc_t want)
{
    if (got != want) { printf("FAIL %s: got %s, want %s\n", what, nap_rc_name(got), nap_rc_name(want)); fails++; }
    else printf("ok   %s -> %s\n", what, nap_rc_name(got));
}

static void wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static nap_rc_t try_mut(uint32_t layout, const char *id, void (*mut)(uint8_t *, uint32_t *, const nap_image_t *),
                        const nap_image_t *good)
{
    uint8_t *m = malloc(g_size + 64);
    memcpy(m, g_file, g_size);
    uint32_t sz = g_size;
    mut(m, &sz, good);
    nap_image_t img; uint8_t *fl = NULL, *ra = NULL; char d[64];
    nap_rc_t rc = full_load(m, sz, layout, id, &img, &fl, &ra, d);
    free(fl); free(ra); free(m);
    return rc;
}

// Targeted corruptions.
static void m_magic(uint8_t *f, uint32_t *n, const nap_image_t *g) { (void)n; (void)g; f[1] = 'X'; }
static void m_machine(uint8_t *f, uint32_t *n, const nap_image_t *g) { (void)n; (void)g; f[18] = 3; }
static void m_exec(uint8_t *f, uint32_t *n, const nap_image_t *g) { (void)n; (void)g; f[16] = 2; }   // ET_EXEC
static void m_trunc(uint8_t *f, uint32_t *n, const nap_image_t *g) { (void)f; *n = g->ro_start + 8; }
static void m_tiny(uint8_t *f, uint32_t *n, const nap_image_t *g) { (void)f; (void)g; *n = 40; }
static uint32_t phdr(const uint8_t *f, int i) { return rd32(f + 28) + 32u * (uint32_t)i; }
static void m_flags(uint8_t *f, uint32_t *n, const nap_image_t *g) { (void)n; (void)g; wr32(f + phdr(f, 1) + 24, 7); }  // rodata RWX
static void m_seg_off(uint8_t *f, uint32_t *n, const nap_image_t *g) { (void)n; (void)g; wr32(f + phdr(f, 2) + 4, rd32(f + phdr(f, 2) + 4) + 4); }
static void m_seg_huge(uint8_t *f, uint32_t *n, const nap_image_t *g) { (void)n; (void)g; wr32(f + phdr(f, 2) + 20, 0x7fffffff); }
static uint32_t dyn_find(const uint8_t *f, uint32_t tag)
{
    for (int i = 0; i < 8; i++) if (rd32(f + phdr(f, i)) == 2) {
        uint32_t d = rd32(f + phdr(f, i) + 4);
        for (uint32_t p = d; rd32(f + p); p += 8) if (rd32(f + p) == tag) return p;
    }
    return 0;
}
static void m_needed(uint8_t *f, uint32_t *n, const nap_image_t *g) { (void)n; (void)g; uint32_t p = dyn_find(f, 6); wr32(f + p, 1); }  // SYMTAB -> NEEDED
static void m_relaent(uint8_t *f, uint32_t *n, const nap_image_t *g) { (void)n; (void)g; uint32_t p = dyn_find(f, 9); wr32(f + p + 4, 8); }
static void m_relasz(uint8_t *f, uint32_t *n, const nap_image_t *g) { (void)n; (void)g; uint32_t p = dyn_find(f, 8); wr32(f + p + 4, 0x7ffffff8); }
static void m_reloc_type(uint8_t *f, uint32_t *n, const nap_image_t *g) { (void)n; f[g->rela + 4] = 20; }
static void m_reloc_off(uint8_t *f, uint32_t *n, const nap_image_t *g) { (void)n; wr32(f + g->rela, 0x00ffff00); }
static void m_reloc_wrap(uint8_t *f, uint32_t *n, const nap_image_t *g) { (void)n; wr32(f + g->rela, 0xfffffffcu); }
static void m_reloc_odd(uint8_t *f, uint32_t *n, const nap_image_t *g) { (void)n; wr32(f + g->rela, rd32(f + g->rela) + 1); }
static void m_reloc_sym(uint8_t *f, uint32_t *n, const nap_image_t *g) { (void)n; uint32_t r = g->jmprel ? g->jmprel : g->rela; wr32(f + r + 4, (rd32(f + r + 4) & 0xff) | 0xffff00u); }
static void m_rel_target(uint8_t *f, uint32_t *n, const nap_image_t *g)
{   // a RELATIVE pointing far outside the module
    (void)n;
    for (uint32_t i = 0; i < g->nrela; i++) {
        uint8_t *r = f + g->rela + 12 * i;
        if (r[4] == 5) { wr32(r + 8, 0x00f00000); return; }
    }
}
static void m_id(uint8_t *f, uint32_t *n, const nap_image_t *g) { (void)n; (void)g; (void)f; }
static int g_info_off;
static void m_info_magic(uint8_t *f, uint32_t *n, const nap_image_t *g) { (void)n; (void)g; f[g_info_off] ^= 1; }
static void m_info_abi(uint8_t *f, uint32_t *n, const nap_image_t *g) { (void)n; (void)g; f[g_info_off + 4] = 9; }
static void m_info_noterm(uint8_t *f, uint32_t *n, const nap_image_t *g) { (void)n; (void)g; memset(f + g_info_off + 16, 'a', 33); }
static void m_strsz(uint8_t *f, uint32_t *n, const nap_image_t *g) { (void)n; (void)g; uint32_t p = dyn_find(f, 10); wr32(f + p + 4, 0x7fffffff); }

int main(int argc, char **argv)
{
    if (argc < 6) { fprintf(stderr, "usage: see header\n"); return 2; }
    g_file = slurp(argv[2], &g_size);
    load_exports(argv[3]);
    uint32_t layout = (uint32_t)strtoul(argv[4], NULL, 16);
    const char *id = argv[5];
    nap_image_t img; uint8_t *fl = NULL, *ra = NULL; char d[64] = "";
    nap_rc_t rc = full_load(g_file, g_size, layout, id, &img, &fl, &ra, d);
    if (rc != NAP_OK) { printf("FAIL load: %s (%s)\n", nap_rc_name(rc), d); return 1; }

    if (!strcmp(argv[1], "relocate")) {
        if (argc < 8) return 2;
        // The device path (tables cached in RAM) must give exactly the same bytes as reading them from the file.
        g_cache_relocs = 1;
        nap_image_t im2; uint8_t *fl2 = NULL, *ra2 = NULL; char d2[64] = "";
        g_ticks = 0;
        if (full_load(g_file, g_size, layout, id, &im2, &fl2, &ra2, d2) != NAP_OK ||
            memcmp(fl, fl2, img.image_size) || memcmp(ra, ra2, img.data_memsz)) { printf("FAIL cached relocations differ\n"); return 1; }
        if (!g_ticks) { printf("FAIL no tick during a load\n"); return 1; }
        g_cache_relocs = 0;
        FILE *o = fopen(argv[6], "wb"); fwrite(fl, 1, img.image_size, o); fclose(o);
        o = fopen(argv[7], "wb"); fwrite(ra, 1, img.data_memsz, o); fclose(o);
        printf("text 0x%x rodata 0x%x-0x%x data 0x%x+%u/%u relocs %u+%u entry 0x%x init %u fini %u id %s abi %u\n",
               img.text_end, img.ro_start, img.ro_end, img.data_start, img.data_filesz, img.data_memsz,
               img.nrela, img.njmprel, img.entry, img.n_init, img.n_fini, img.info.id, img.info.abi);
        return 0;
    }

    // reject: where nucleo_native_info lives in the file (offset == vaddr).
    for (uint32_t o = img.ro_start; o + 4 <= img.ro_end; o += 4) if (rd32(g_file + o) == 0x3150414eu) { g_info_off = (int)o; break; }
    struct { const char *what; void (*m)(uint8_t *, uint32_t *, const nap_image_t *); nap_rc_t want; } T[] = {
        { "bad ELF magic", m_magic, NAP_E_FORMAT },          { "other machine", m_machine, NAP_E_FORMAT },
        { "not a shared object", m_exec, NAP_E_FORMAT },     { "truncated file", m_trunc, NAP_E_SEGMENTS },
        { "40-byte file", m_tiny, NAP_E_IO },                { "rodata made executable", m_flags, NAP_E_SEGMENTS },
        { "data offset != vaddr", m_seg_off, NAP_E_SEGMENTS }, { "data segment 2 GB", m_seg_huge, NAP_E_TOOBIG },
        { "DT_NEEDED (a library)", m_needed, NAP_E_DYNAMIC }, { "RELAENT 8", m_relaent, NAP_E_DYNAMIC },
        { "RELASZ past the file", m_relasz, NAP_E_FORMAT },  { "unknown reloc type", m_reloc_type, NAP_E_RELOC },
        { "reloc outside the module", m_reloc_off, NAP_E_RELOC }, { "unaligned reloc", m_reloc_odd, NAP_E_RELOC },
        { "reloc offset that wraps to 0", m_reloc_wrap, NAP_E_RELOC },
        { "reloc symbol index out of range", m_reloc_sym, NAP_E_FORMAT }, { "RELATIVE target outside", m_rel_target, NAP_E_RELOC },
        { "info magic", m_info_magic, NAP_E_INFO },          { "info abi 9 > 1", m_info_abi, NAP_E_ABI },
        { "info id unterminated", m_info_noterm, NAP_E_INFO }, { "STRSZ past the file", m_strsz, NAP_E_FORMAT },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) expect(T[i].what, try_mut(layout, id, T[i].m, &img), T[i].want);
    expect("other layout fingerprint", try_mut(layout ^ 1, id, m_id, &img), NAP_E_LAYOUT);
    expect("installed under another id", try_mut(layout, "other", m_id, &img), NAP_E_ID);
    {   // an import the firmware does not export
        uint32_t keep = g_nex; g_nex = 0;
        expect("firmware without the imports", try_mut(layout, id, m_id, &img), NAP_E_IMPORT);
        g_nex = keep;
    }
    // Random mutations: any result is fine, a crash (sanitizer trap) is not.
    srand(1234);
    int refused = 0;
    for (int it = 0; it < 4000; it++) {
        uint8_t *m = malloc(g_size);
        memcpy(m, g_file, g_size);
        int k = 1 + rand() % 8;
        for (int j = 0; j < k; j++) {
            uint32_t at = (uint32_t)rand() % (it & 1 ? g_size : (img.ro_start < 4096 ? img.ro_start : 4096));
            m[at] = (uint8_t)rand();
        }
        nap_image_t im; uint8_t *f2 = NULL, *r2 = NULL; char dd[64];
        if (full_load(m, g_size, layout, id, &im, &f2, &r2, dd) != NAP_OK) refused++;
        free(f2); free(r2); free(m);
    }
    printf("ok   4000 random mutations: %d refused, no crash\n", refused);
    return fails ? 1 : 0;
}
