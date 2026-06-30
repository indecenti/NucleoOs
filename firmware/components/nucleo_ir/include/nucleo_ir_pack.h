// nucleo_ir_pack — low-RAM reader for the packed IR preset catalog ("IRPK" v1).
//
// Why a binary pack: the human-editable source of truth is apps/ir-remote/www/presets.json, which
// the browser loads whole (it has RAM to spare). The device does NOT — the ~18 KB heap can't hold a
// large JSON DOM. So tools/ir-pack.mjs compiles that JSON into a fixed-width binary the firmware
// reads with fseek/fread: every list row and every button is fetched on demand, so RAM stays O(one
// record) no matter how many thousands of remotes the catalog holds. Pure C (stdio only) so the
// host gate (`npm run irpack:test`) can compile and exercise it on the PC, exactly like the encoder.
//
// On-disk layout (little-endian, all offsets from file start):
//   Header (32 B):  "IRPK" | u16 version | u16 flags | u32 n_remotes | u32 remotes_off
//                          | u32 n_tvpower | u32 tvpower_off | u32 raw_off | u32 raw_size
//   Remote index   (40 B each, at remotes_off):  char name[28] | u8 proto | u8 region
//                          | u16 nbtn | u32 addr | u32 data_off
//   Button block   (16 B each, at a remote's data_off):  char key[12] | u32 cmd
//   TV-power table (28 B each, at tvpower_off):  char brand[20] | u8 region | u8 proto
//                          | u16 addr | u16 cmd | u16 pad
//   RAW pool       (at raw_off):  a sequence of entries, each = u16 carrier | u16 count | count*u16 µs
// region: 0=all 1=us 2=eu 3=asia. proto: nir_proto_t value (0 == RAW).
//
// RAW codes (the universal escape hatch — any captured signal, any protocol): a record whose proto
// is 0 carries a RAW reference instead of address/command. For a BUTTON the u32 cmd field IS the
// byte offset of its RAW entry; for a TV-power row the offset is (addr<<16 | cmd). The carrier lives
// inside the RAW entry. A remote is uniformly RAW or protocol (remote.proto == 0 => raw buttons).
#ifndef NUCLEO_IR_PACK_H
#define NUCLEO_IR_PACK_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IRPACK_MAGIC    "IRPK"
#define IRPACK_VERSION  2     // v2 adds the RAW pool; the reader still accepts a v1 (raw-less) pack
#define IRPACK_PROTO_RAW 0    // proto code that marks a RAW record

#define IRPACK_NAME_LEN  28
#define IRPACK_KEY_LEN   12
#define IRPACK_BRAND_LEN 20

#define IRPACK_HDR_SIZE     32
#define IRPACK_REMOTE_SIZE  40
#define IRPACK_BTN_SIZE     16
#define IRPACK_TVP_SIZE     28

typedef struct {
    FILE    *f;
    uint16_t version;
    uint32_t n_remotes, remotes_off;
    uint32_t n_tvpower, tvpower_off;
    uint32_t raw_off, raw_size;       // RAW pool (0/0 on a v1 pack)
} ir_pack_t;

typedef struct {
    char     name[IRPACK_NAME_LEN];   // NUL-terminated
    uint8_t  proto;                   // nir_proto_t
    uint8_t  region;                  // 0=all 1=us 2=eu 3=asia
    uint16_t nbtn;
    uint32_t addr;
    uint32_t data_off;
} ir_pack_remote_t;

typedef struct { char key[IRPACK_KEY_LEN]; uint32_t cmd; } ir_pack_btn_t;

typedef struct {
    char     brand[IRPACK_BRAND_LEN]; // NUL-terminated
    uint8_t  region, proto;
    uint16_t addr, cmd;
} ir_pack_tvp_t;

// Open a pack file. Reads + validates the 32-byte header. Returns false (and leaves p->f NULL)
// on a missing file or a bad magic/version. The FILE stays open until ir_pack_close().
bool ir_pack_open(ir_pack_t *p, const char *path);
void ir_pack_close(ir_pack_t *p);

// Random-access fetch of one record (seek + read). Index bounds-checked against the header.
bool ir_pack_remote(ir_pack_t *p, uint32_t i, ir_pack_remote_t *out);
bool ir_pack_button(ir_pack_t *p, const ir_pack_remote_t *r, uint32_t j, ir_pack_btn_t *out);
bool ir_pack_tvpower(ir_pack_t *p, uint32_t i, ir_pack_tvp_t *out);

// Read a RAW timing entry at absolute byte offset `off`: fills *carrier and up to `cap` durations
// (µs). Returns the duration count (>0), or 0 on a bad offset / empty entry. RAW offsets come from
// a RAW button's `cmd` field or a RAW tv-power row's (addr<<16 | cmd) — see the header note.
int  ir_pack_raw(ir_pack_t *p, uint32_t off, uint16_t *carrier, uint16_t *dur, int cap);

// Convenience: the RAW-pool offset carried by a RAW tv-power row (proto == IRPACK_PROTO_RAW).
static inline uint32_t ir_pack_tvp_raw_off(const ir_pack_tvp_t *t) {
    return ((uint32_t)t->addr << 16) | t->cmd;
}

// region name <-> code, shared with the packer's mapping. Unknown name -> 0 ("all").
uint8_t     ir_pack_region_code(const char *name);
const char *ir_pack_region_name(uint8_t code);

#ifdef __cplusplus
}
#endif
#endif // NUCLEO_IR_PACK_H
