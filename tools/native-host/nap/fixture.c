// A small but complete native module for the loader gate (npm run native:test): imports (functions and data),
// pointers in every segment (code -> rodata, rodata -> code, data -> data), a constructor and a destructor,
// a jump table, and the two exports every .nap has. Built with the SDK's own flags and linker script.
#include <stdint.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    uint32_t magic; uint16_t abi; uint16_t flags; uint32_t layout; uint32_t ram; char id[33]; uint8_t reserved[3];
} nap_info_t;                                   // == sdk/native/include/nucleo_native.h (C view)

#define EXPORT __attribute__((visibility("default"), used))

typedef struct { const char *id; void (*draw)(void); } def_t;
extern void nucleo_app_register(const def_t *d);
extern const uint16_t THEME_FG;                 // a data import

static int counter = 7;                         // .data
static int *counter_ptr = &counter;             // data -> data
static char scratch[64];                        // .bss
static const char *const words[] = { "zero", "one", "two", "three" };   // rodata/data.rel.ro -> rodata

static void draw(void)
{
    snprintf(scratch, sizeof scratch, "%s %d %u", words[*counter_ptr & 3], *counter_ptr, (unsigned)THEME_FG);
}

static int pick(int k)                          // a switch dense enough for a jump table
{
    switch (k) {
        case 0: return 11; case 1: return 23; case 2: return 37; case 3: return 41;
        case 4: return 53; case 5: return 67; case 6: return 71; default: return 0;
    }
}

static const def_t DEF = { "fixture", draw };   // rodata -> code + rodata

__attribute__((constructor)) static void init(void) { counter += pick(counter & 7); }
__attribute__((destructor))  static void fini(void) { memset(scratch, 0, sizeof scratch); }

EXPORT const nap_info_t nucleo_native_info = { 0x3150414eu, 1, 0, 0x12345678u, 0, "fixture", { 0, 0, 0 } };
EXPORT void nucleo_native_main(void) { nucleo_app_register(&DEF); }
