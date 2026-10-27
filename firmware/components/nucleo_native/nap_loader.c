// Native app loader — portable half. See nap_loader.h and docs/native-apps.md.
#include "nap_loader.h"

#include <stdio.h>
#include <string.h>

// ── ELF32 (only what a .nap may contain) ─────────────────────────────────────
#define EM_XTENSA        94
#define ET_DYN           3
#define PT_NULL          0
#define PT_LOAD          1
#define PT_DYNAMIC       2
#define PT_GNU_STACK     0x6474e551u
#define PF_X 1
#define PF_W 2
#define PF_R 4

#define DT_NULL          0
#define DT_NEEDED        1
#define DT_PLTRELSZ      2
#define DT_PLTGOT        3
#define DT_HASH          4
#define DT_STRTAB        5
#define DT_SYMTAB        6
#define DT_RELA          7
#define DT_RELASZ        8
#define DT_RELAENT       9
#define DT_STRSZ         10
#define DT_SYMENT        11
#define DT_SONAME        14
#define DT_PLTREL        20
#define DT_TEXTREL       22
#define DT_JMPREL        23
#define DT_INIT_ARRAY    25
#define DT_FINI_ARRAY    26
#define DT_INIT_ARRAYSZ  27
#define DT_FINI_ARRAYSZ  28
#define DT_FLAGS         30
#define DT_RELACOUNT     0x6ffffff9u
#define DT_XTENSA_GOT_LOC_OFF 0x70000000u
#define DT_XTENSA_GOT_LOC_SZ  0x70000001u

#define R_XTENSA_NONE     0
#define R_XTENSA_32       1
#define R_XTENSA_RTLD     2
#define R_XTENSA_GLOB_DAT 3
#define R_XTENSA_JMP_SLOT 4
#define R_XTENSA_RELATIVE 5

#define STB_LOCAL  0
#define STT_FUNC   2
#define SHN_UNDEF  0

#define EHDR_SIZE  52
#define PHDR_SIZE  32
#define SYM_SIZE   16
#define RELA_SIZE  12
#define NAME_MAX_  96          // longest import name accepted (mangled C++ included)
#define RELA_BLOCK 32          // relocations read per call (384 B of stack)

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

#define DETAIL(...) do { if (detail && detail_cap) snprintf(detail, detail_cap, __VA_ARGS__); } while (0)

// [off, off+len) inside [0, limit) without overflow.
static bool in_range(uint32_t off, uint32_t len, uint32_t limit) { return off <= limit && len <= limit - off; }

static bool rd(const nap_src_t *s, uint32_t off, void *buf, uint32_t len)
{
    return in_range(off, len, s->size) && s->read(s->ctx, off, buf, len) == 0;
}

const char *nap_rc_name(nap_rc_t rc)
{
    static const char *const n[] = { "ok", "io", "format", "segments", "dynamic", "reloc", "import", "info", "abi",
                                     "layout", "id", "too big" };
    return (unsigned)rc < sizeof n / sizeof n[0] ? n[rc] : "?";
}

const nap_export_t *nap_find_export(const nap_export_t *tab, uint32_t n, const char *name)
{
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int c = strcmp(name, tab[mid].name);
        if (c == 0) return &tab[mid];
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}

static bool in_module(const nap_image_t *img, uint32_t v)
{
    return v < img->text_end || (v >= img->ro_start && v < img->ro_end) ||
           (v >= img->data_start && v <= img->data_start + img->data_memsz);
}

uint32_t nap_addr(const nap_image_t *img, const nap_bases_t *b, uint32_t v)
{
    if (!in_module(img, v)) return 0;
    if (v < img->text_end) return b->ibus + v;
    if (v >= img->ro_start && v < img->ro_end) return b->dbus + v;
    if (v >= img->data_start && v <= img->data_start + img->data_memsz) return b->ram + v;
    return 0;
}

// A NUL-terminated string at strtab + off (at most cap - 1 chars), inside the string table.
static bool read_str(const nap_src_t *s, const nap_image_t *img, uint32_t off, char *out, size_t cap)
{
    if (off >= img->strsz) return false;
    uint32_t avail = img->strsz - off;
    uint32_t n = avail < cap ? avail : (uint32_t)cap;
    if (!rd(s, img->strtab + off, out, n)) return false;
    for (uint32_t i = 0; i < n; i++) if (!out[i]) return true;
    return false;                                   // unterminated, or longer than cap - 1
}

typedef struct { uint32_t name, value, size; uint8_t info, other; uint16_t shndx; } sym_t;

static bool read_sym(const nap_src_t *s, const nap_image_t *img, uint32_t idx, sym_t *o)
{
    uint8_t b[SYM_SIZE];
    if (idx >= img->nsyms || !rd(s, img->symtab + idx * SYM_SIZE, b, SYM_SIZE)) return false;
    o->name = rd32(b); o->value = rd32(b + 4); o->size = rd32(b + 8);
    o->info = b[12]; o->other = b[13]; o->shndx = rd16(b + 14);
    return true;
}

// The runtime value of symbol idx: an import from the export table, or a module address.
static nap_rc_t sym_value(const nap_src_t *s, const nap_image_t *img, const nap_bases_t *b, uint32_t idx,
                          uint32_t *out, char *detail, size_t detail_cap)
{
    sym_t sy;
    if (!read_sym(s, img, idx, &sy)) { DETAIL("symbol %lu", (unsigned long)idx); return NAP_E_FORMAT; }
    if (sy.shndx != SHN_UNDEF) {
        if (!in_module(img, sy.value)) { DETAIL("symbol %lu outside the module", (unsigned long)idx); return NAP_E_RELOC; }
        *out = nap_addr(img, b, sy.value);
        return NAP_OK;
    }
    char name[NAME_MAX_];
    if (!read_str(s, img, sy.name, name, sizeof name)) { DETAIL("symbol name %lu", (unsigned long)idx); return NAP_E_FORMAT; }
    const nap_export_t *e = nap_find_export(s->exports, s->n_exports, name);
    if (!e) { DETAIL("needs %s", name); return NAP_E_IMPORT; }
    *out = e->addr;
    return NAP_OK;
}

// ── inspect ──────────────────────────────────────────────────────────────────
static bool find_sym(const nap_src_t *s, const nap_image_t *img, const char *want, sym_t *o)
{
    char name[40];
    for (uint32_t i = 1; i < img->nsyms; i++) {
        if (!read_sym(s, img, i, o) || o->shndx == SHN_UNDEF || (o->info >> 4) == STB_LOCAL) continue;
        if (read_str(s, img, o->name, name, sizeof name) && !strcmp(name, want)) return true;
    }
    return false;
}

// A block of relocation entries: from the RAM copy when there is one, else from the file.
static bool rela_block(const nap_src_t *s, const nap_image_t *img, uint32_t tab, uint32_t first, uint32_t k, uint8_t *blk)
{
    const uint8_t *mem = tab == img->rela ? img->rela_mem : tab == img->jmprel ? img->jmprel_mem : NULL;
    if (s->tick) s->tick(s->ctx);
    if (mem) { memcpy(blk, mem + first * RELA_SIZE, k * RELA_SIZE); return true; }
    return rd(s, tab + first * RELA_SIZE, blk, k * RELA_SIZE);
}

uint32_t nap_relocs_bytes(const nap_image_t *img) { return (img->nrela + img->njmprel) * RELA_SIZE; }

nap_rc_t nap_cache_relocs(const nap_src_t *s, nap_image_t *img, uint8_t *mem, uint32_t cap)
{
    if (nap_relocs_bytes(img) > cap) return NAP_E_TOOBIG;
    if ((img->nrela && !rd(s, img->rela, mem, img->nrela * RELA_SIZE)) ||
        (img->njmprel && !rd(s, img->jmprel, mem + img->nrela * RELA_SIZE, img->njmprel * RELA_SIZE)))
        return NAP_E_IO;
    img->rela_mem = mem;
    img->jmprel_mem = mem + img->nrela * RELA_SIZE;
    return NAP_OK;
}

// Every relocation of one table: type, alignment, target segment, and (with resolve) its symbol.
static nap_rc_t check_relocs(const nap_src_t *s, const nap_image_t *img, uint32_t tab, uint32_t n, bool plt,
                             char *detail, size_t detail_cap)
{
    uint8_t blk[RELA_BLOCK * RELA_SIZE];
    nap_bases_t probe = { 0, 0, 0 };
    for (uint32_t i = 0; i < n; i += RELA_BLOCK) {
        uint32_t k = n - i < RELA_BLOCK ? n - i : RELA_BLOCK;
        if (!rela_block(s, img, tab, i, k, blk)) return NAP_E_FORMAT;
        for (uint32_t j = 0; j < k; j++) {
            const uint8_t *r = blk + j * RELA_SIZE;
            uint32_t off = rd32(r), info = rd32(r + 4), type = info & 0xff, sym = info >> 8;
            if (off & 3) { DETAIL("reloc %lu unaligned", (unsigned long)(i + j)); return NAP_E_RELOC; }
            // in_range(): no `off + 4` that could wrap around to 0 and pass.
            bool in_flash = in_range(off, 4, img->text_end) ||
                            (off >= img->ro_start && in_range(off - img->ro_start, 4, img->ro_end - img->ro_start));
            bool in_data = off >= img->data_start && in_range(off - img->data_start, 4, img->data_memsz);
            if (!in_flash && !in_data) { DETAIL("reloc %lu at 0x%lx", (unsigned long)(i + j), (unsigned long)off); return NAP_E_RELOC; }
            switch (type) {
                case R_XTENSA_NONE: case R_XTENSA_RTLD: continue;
                case R_XTENSA_RELATIVE: if (plt || sym) goto bad; continue;
                case R_XTENSA_JMP_SLOT: if (!plt) goto bad; break;
                case R_XTENSA_32: case R_XTENSA_GLOB_DAT: if (plt) goto bad; break;
                default: goto bad;
            }
            if (!sym) goto bad;
            uint32_t v;
            nap_rc_t rc = sym_value(s, img, &probe, sym, &v, detail, detail_cap);
            if (rc != NAP_OK) return rc;
            continue;
        bad:
            DETAIL("reloc %lu type %lu", (unsigned long)(i + j), (unsigned long)type);
            return NAP_E_RELOC;
        }
    }
    return NAP_OK;
}

nap_rc_t nap_inspect(const nap_src_t *s, nap_image_t *img, char *detail, size_t detail_cap)
{
    memset(img, 0, sizeof *img);
    if (detail && detail_cap) detail[0] = 0;
    uint8_t eh[EHDR_SIZE];
    if (s->size < EHDR_SIZE || !rd(s, 0, eh, EHDR_SIZE)) return NAP_E_IO;
    if (memcmp(eh, "\x7f" "ELF", 4) || eh[4] != 1 || eh[5] != 1 || eh[6] != 1) { DETAIL("not ELF32 LE"); return NAP_E_FORMAT; }
    if (rd16(eh + 16) != ET_DYN || rd16(eh + 18) != EM_XTENSA || rd32(eh + 20) != 1) { DETAIL("not an Xtensa module"); return NAP_E_FORMAT; }
    uint32_t phoff = rd32(eh + 28);
    uint16_t phentsize = rd16(eh + 42), phnum = rd16(eh + 44);
    if (phentsize != PHDR_SIZE || phnum == 0 || phnum > 8 || !in_range(phoff, (uint32_t)phnum * PHDR_SIZE, s->size)) return NAP_E_FORMAT;

    // Program headers: exactly text(R+X) rodata(R) data(RW) in that order, file offset == vaddr, + one DYNAMIC.
    uint32_t dyn = 0, dynsz = 0;
    int loads = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        uint8_t ph[PHDR_SIZE];
        if (!rd(s, phoff + i * PHDR_SIZE, ph, PHDR_SIZE)) return NAP_E_IO;
        uint32_t type = rd32(ph), off = rd32(ph + 4), va = rd32(ph + 8), fsz = rd32(ph + 16), msz = rd32(ph + 20), fl = rd32(ph + 24);
        if (type == PT_NULL || type == PT_GNU_STACK) continue;
        if (type == PT_DYNAMIC) { if (dyn) return NAP_E_SEGMENTS; dyn = va; dynsz = msz; continue; }
        if (type != PT_LOAD) { DETAIL("segment type 0x%lx", (unsigned long)type); return NAP_E_SEGMENTS; }
        if (off != va || fsz > msz || !in_range(off, fsz, s->size)) return NAP_E_SEGMENTS;
        switch (loads++) {
            case 0: if (fl != (PF_R | PF_X) || va != 0 || fsz != msz) return NAP_E_SEGMENTS;
                    img->text_end = msz; break;
            case 1: if (fl != PF_R || va < img->text_end || fsz != msz) return NAP_E_SEGMENTS;
                    img->ro_start = va; img->ro_end = va + msz; break;
            case 2: if (fl != (PF_R | PF_W) || va < img->ro_end || (va & 3)) return NAP_E_SEGMENTS;
                    img->data_start = va; img->data_filesz = fsz; img->data_memsz = msz; break;
            default: return NAP_E_SEGMENTS;
        }
    }
    if (loads != 3 || !dyn || dyn < img->data_start || !in_range(dyn - img->data_start, dynsz, img->data_filesz) || (dynsz & 7))
        { DETAIL("segment layout"); return NAP_E_SEGMENTS; }
    img->image_size = img->ro_end;
    if (img->image_size > s->flash_max || img->data_memsz > s->ram_max)
        { DETAIL("%lu B code, %lu B data", (unsigned long)img->image_size, (unsigned long)img->data_memsz); return NAP_E_TOOBIG; }

    // Dynamic section: only the tags the SDK's link produces.
    uint32_t hash = 0, relasz = 0, pltrelsz = 0, init_sz = 0, fini_sz = 0;
    for (uint32_t p = dyn; p + 8 <= dyn + dynsz; p += 8) {
        uint8_t e[8];
        if (!rd(s, p, e, 8)) return NAP_E_IO;
        uint32_t tag = rd32(e), val = rd32(e + 4);
        if (tag == DT_NULL) break;
        switch (tag) {
            case DT_HASH:         hash = val; break;
            case DT_STRTAB:       img->strtab = val; break;
            case DT_SYMTAB:       img->symtab = val; break;
            case DT_STRSZ:        img->strsz = val; break;
            case DT_SYMENT:       if (val != SYM_SIZE) return NAP_E_DYNAMIC; break;
            case DT_RELA:         img->rela = val; break;
            case DT_RELASZ:       relasz = val; break;
            case DT_RELAENT:      if (val != RELA_SIZE) return NAP_E_DYNAMIC; break;
            case DT_JMPREL:       img->jmprel = val; break;
            case DT_PLTRELSZ:     pltrelsz = val; break;
            case DT_PLTREL:       if (val != DT_RELA) return NAP_E_DYNAMIC; break;
            case DT_INIT_ARRAY:   img->init_array = val; break;
            case DT_INIT_ARRAYSZ: init_sz = val; break;
            case DT_FINI_ARRAY:   img->fini_array = val; break;
            case DT_FINI_ARRAYSZ: fini_sz = val; break;
            case DT_PLTGOT: case DT_TEXTREL: case DT_FLAGS: case DT_RELACOUNT: case DT_SONAME:
            case DT_XTENSA_GOT_LOC_OFF: case DT_XTENSA_GOT_LOC_SZ: break;
            default: DETAIL("dynamic tag 0x%lx", (unsigned long)tag); return NAP_E_DYNAMIC;
        }
    }
    // The tables live in the text segment (sdk/native/nap.ld): read from the file, never mapped.
    uint8_t h[8];
    if (!hash || !img->symtab || !img->strtab || !in_range(hash, 8, img->text_end) || !rd(s, hash, h, 8)) return NAP_E_FORMAT;
    img->nsyms = rd32(h + 4);
    if (!img->nsyms || img->nsyms > 8192 || !in_range(img->symtab, img->nsyms * SYM_SIZE, img->text_end) ||
        !in_range(img->strtab, img->strsz, img->text_end) || relasz % RELA_SIZE || pltrelsz % RELA_SIZE ||
        (relasz && !in_range(img->rela, relasz, img->text_end)) || (pltrelsz && !in_range(img->jmprel, pltrelsz, img->text_end)))
        { DETAIL("tables"); return NAP_E_FORMAT; }
    img->nrela = relasz / RELA_SIZE;
    img->njmprel = pltrelsz / RELA_SIZE;
    if (init_sz % 4 || fini_sz % 4) return NAP_E_FORMAT;
    img->n_init = init_sz / 4;
    img->n_fini = fini_sz / 4;
    uint32_t dend = img->data_start + img->data_memsz;
    if ((img->n_init && (img->init_array < img->data_start || !in_range(img->init_array - img->data_start, init_sz, img->data_filesz))) ||
        (img->n_fini && (img->fini_array < img->data_start || !in_range(img->fini_array - img->data_start, fini_sz, img->data_filesz))) ||
        img->n_init > 64 || img->n_fini > 64 || dend < img->data_start)
        return NAP_E_FORMAT;

    // The two exports every module has.
    sym_t info, main_;
    if (!find_sym(s, img, "nucleo_native_info", &info) || !find_sym(s, img, "nucleo_native_main", &main_))
        { DETAIL("no nucleo_native_info/main"); return NAP_E_INFO; }
    if (info.value < img->ro_start || !in_range(info.value - img->ro_start, sizeof(nap_info_t), img->ro_end - img->ro_start) ||
        info.size != sizeof(nap_info_t) || (main_.info & 0xf) != STT_FUNC || main_.value >= img->text_end)
        { DETAIL("bad nucleo_native_info/main"); return NAP_E_INFO; }
    img->entry = main_.value;
    if (!rd(s, info.value, &img->info, sizeof img->info)) return NAP_E_IO;
    const nap_info_t *ni = &img->info;
    if (ni->magic != NAP_MAGIC || memchr(ni->id, 0, sizeof ni->id) == NULL) return NAP_E_INFO;
    if (ni->abi == 0 || ni->abi > s->abi_max) { DETAIL("abi %lu > %lu", (unsigned long)ni->abi, (unsigned long)s->abi_max); return NAP_E_ABI; }
    if (ni->layout != s->layout) { DETAIL("layout %08lx != %08lx", (unsigned long)ni->layout, (unsigned long)s->layout); return NAP_E_LAYOUT; }
    if (s->expect_id && strcmp(ni->id, s->expect_id)) { DETAIL("id %s", ni->id); return NAP_E_ID; }

    // Every relocation and import, before anything is written anywhere.
    nap_rc_t rc = check_relocs(s, img, img->rela, img->nrela, false, detail, detail_cap);
    if (rc == NAP_OK) rc = check_relocs(s, img, img->jmprel, img->njmprel, true, detail, detail_cap);
    return rc;
}

// ── relocate ─────────────────────────────────────────────────────────────────
// Apply the relocations of one table that fall in [lo, hi) to buf (which holds the bytes of [lo, hi)).
static nap_rc_t apply(const nap_src_t *s, const nap_image_t *img, const nap_bases_t *b, uint32_t tab, uint32_t n,
                      uint32_t lo, uint32_t hi, uint8_t *buf, char *detail, size_t detail_cap)
{
    uint8_t blk[RELA_BLOCK * RELA_SIZE];
    for (uint32_t i = 0; i < n; i += RELA_BLOCK) {
        uint32_t k = n - i < RELA_BLOCK ? n - i : RELA_BLOCK;
        if (!rela_block(s, img, tab, i, k, blk)) return NAP_E_IO;
        for (uint32_t j = 0; j < k; j++) {
            const uint8_t *r = blk + j * RELA_SIZE;
            uint32_t off = rd32(r), info = rd32(r + 4), add = rd32(r + 8), type = info & 0xff;
            if (off < lo || !in_range(off - lo, 4, hi - lo)) continue;
            uint8_t *p = buf + (off - lo);
            uint32_t v;
            nap_rc_t rc;
            switch (type) {
                case R_XTENSA_NONE: break;
                case R_XTENSA_RTLD: wr32(p, 0); break;           // lazy binding is never used: all bound now
                case R_XTENSA_RELATIVE: {
                    uint32_t target = rd32(p) + add;
                    if (!in_module(img, target)) { DETAIL("relative 0x%lx -> 0x%lx", (unsigned long)off, (unsigned long)target); return NAP_E_RELOC; }
                    wr32(p, nap_addr(img, b, target));
                    break;
                }
                case R_XTENSA_GLOB_DAT: case R_XTENSA_JMP_SLOT:
                    if ((rc = sym_value(s, img, b, info >> 8, &v, detail, detail_cap)) != NAP_OK) return rc;
                    wr32(p, v + add);
                    break;
                case R_XTENSA_32:
                    if ((rc = sym_value(s, img, b, info >> 8, &v, detail, detail_cap)) != NAP_OK) return rc;
                    wr32(p, rd32(p) + v + add);
                    break;
                default: return NAP_E_RELOC;
            }
        }
    }
    return NAP_OK;
}

nap_rc_t nap_relocate_flash(const nap_src_t *s, const nap_image_t *img, const nap_bases_t *b,
                            uint32_t off, uint8_t *buf, uint32_t len, char *detail, size_t detail_cap)
{
    if ((off | len) & 3 || !in_range(off, len, img->image_size)) return NAP_E_FORMAT;
    if (!rd(s, off, buf, len)) return NAP_E_IO;
    // The gap between text and rodata (alignment padding) is not part of either segment: zero it.
    for (uint32_t a = off; a < off + len; a++) if (a >= img->text_end && a < img->ro_start) buf[a - off] = 0;
    nap_rc_t rc = apply(s, img, b, img->rela, img->nrela, off, off + len, buf, detail, detail_cap);
    if (rc == NAP_OK) rc = apply(s, img, b, img->jmprel, img->njmprel, off, off + len, buf, detail, detail_cap);
    return rc;
}

nap_rc_t nap_relocate_data(const nap_src_t *s, const nap_image_t *img, const nap_bases_t *b,
                           uint8_t *ram, char *detail, size_t detail_cap)
{
    memset(ram, 0, img->data_memsz);
    if (img->data_filesz && !rd(s, img->data_start, ram, img->data_filesz)) return NAP_E_IO;
    uint32_t lo = img->data_start, hi = img->data_start + img->data_memsz;
    nap_rc_t rc = apply(s, img, b, img->rela, img->nrela, lo, hi, ram, detail, detail_cap);
    if (rc == NAP_OK) rc = apply(s, img, b, img->jmprel, img->njmprel, lo, hi, ram, detail, detail_cap);
    return rc;
}
