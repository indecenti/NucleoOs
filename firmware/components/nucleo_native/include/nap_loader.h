// Native app loader — the PORTABLE half: validates a .nap (ELF32 Xtensa shared object, sdk/native/nap.ld layout)
// and relocates it, reading the file through a callback in small pieces. Pure C, no ESP-IDF: the device glue
// (nucleo_native.cpp) writes the relocated image to the flash scratch region and maps it; the host gate
// (tools/native-host/nap) runs the same code over real modules and over corrupted ones. Spec: docs/native-apps.md.
//
// Nothing here trusts the file: every offset, size, table, relocation and import is range-checked, and
// nap_inspect() rejects anything outside the one layout the SDK produces before a single byte is executed.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "nucleo_native.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { const char *name; uint32_t addr; } nap_export_t;     // sorted by strcmp(name)

typedef enum {
    NAP_OK = 0,
    NAP_E_IO,          // the file could not be read
    NAP_E_FORMAT,      // not an ELF32 Xtensa shared object, or a table / offset out of range
    NAP_E_SEGMENTS,    // not the text / rodata / data layout of sdk/native/nap.ld
    NAP_E_DYNAMIC,     // a dynamic tag this loader does not implement (NEEDED, INIT, TLS...)
    NAP_E_RELOC,       // a relocation type or target outside the module
    NAP_E_IMPORT,      // imports a name this firmware does not export (detail = the name)
    NAP_E_INFO,        // no valid nucleo_native_info / nucleo_native_main
    NAP_E_ABI,         // built for a newer ABI level than this firmware has
    NAP_E_LAYOUT,      // built against differently shaped firmware types (layout fingerprint)
    NAP_E_ID,          // its id is not the folder it was installed in
    NAP_E_TOOBIG,      // larger than the flash scratch region or the RAM budget
} nap_rc_t;

typedef struct {
    void    *ctx;
    int    (*read)(void *ctx, uint32_t off, void *buf, uint32_t len);   // 0 = all len bytes read
    uint32_t size;                    // file size
    const nap_export_t *exports;      // the firmware's ABI table
    uint32_t n_exports;
    uint32_t abi_max;                 // highest ABI level this firmware exports
    uint32_t layout;                  // this firmware's nucleo_native_layout()
    const char *expect_id;            // the store id it was installed as (NULL = do not check)
    uint32_t flash_max;               // bytes of the scratch region usable for text + rodata
    uint32_t ram_max;                 // largest data segment accepted (.data + .bss)
    void   (*tick)(void *ctx);        // optional: called between blocks of work (feed a watchdog, yield)
} nap_src_t;

typedef struct {
    uint32_t text_end;                // text: [0, text_end)       instruction bus
    uint32_t ro_start, ro_end;        // rodata: [ro_start, ro_end) data bus, same flash pages
    uint32_t data_start, data_filesz, data_memsz;  // data: RAM
    uint32_t image_size;              // bytes that go to flash: [0, ro_end)
    uint32_t symtab, strtab, strsz, nsyms;
    uint32_t rela, nrela, jmprel, njmprel;
    uint32_t init_array, n_init, fini_array, n_fini;
    uint32_t entry;                   // vaddr of nucleo_native_main
    nap_info_t info;
    const uint8_t *rela_mem;          // nap_cache_relocs(): both tables in RAM (NULL = read from the file)
    const uint8_t *jmprel_mem;
} nap_image_t;

// Runtime addresses of vaddr 0 for each part: text on the instruction bus, rodata on the data bus (the same
// flash pages seen through the other bus), data in RAM (address of the buffer minus data_start).
typedef struct { uint32_t ibus, dbus, ram; } nap_bases_t;

// Full validation; fills *img. detail (optional) receives a short reason ("needs fopen64", "abi 2 > 1").
nap_rc_t nap_inspect(const nap_src_t *src, nap_image_t *img, char *detail, size_t detail_cap);

// Bytes nap_cache_relocs() needs, and the call itself: reads both relocation tables into `mem` once, so the
// passes below never re-read them from the SD card. Optional; without it they are read from the file.
uint32_t nap_relocs_bytes(const nap_image_t *img);
nap_rc_t nap_cache_relocs(const nap_src_t *src, nap_image_t *img, uint8_t *mem, uint32_t cap);

// The relocated bytes of the flash image [off, off + len): off and len multiples of 4, inside image_size.
nap_rc_t nap_relocate_flash(const nap_src_t *src, const nap_image_t *img, const nap_bases_t *b,
                            uint32_t off, uint8_t *buf, uint32_t len, char *detail, size_t detail_cap);

// The data segment, relocated, into ram (data_memsz bytes; .bss zeroed).
nap_rc_t nap_relocate_data(const nap_src_t *src, const nap_image_t *img, const nap_bases_t *b,
                           uint8_t *ram, char *detail, size_t detail_cap);

// The runtime address of a module vaddr (0 when it is outside every segment).
uint32_t nap_addr(const nap_image_t *img, const nap_bases_t *b, uint32_t vaddr);

// Binary search in a sorted export table (NULL when absent).
const nap_export_t *nap_find_export(const nap_export_t *tab, uint32_t n, const char *name);

const char *nap_rc_name(nap_rc_t rc);

#ifdef __cplusplus
}
#endif
