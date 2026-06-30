// nucleo_ir_pack — see nucleo_ir_pack.h. Pure stdio so it compiles on host and device alike.
// All multi-byte fields are read with explicit little-endian helpers (never struct-overlay) so the
// format is byte-identical no matter the target's packing/endianness.
#include "nucleo_ir_pack.h"
#include <string.h>

static uint16_t rd16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static uint32_t rd32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

// Read exactly `n` bytes from absolute offset `off`. Returns false on short read / seek error.
static bool read_at(FILE *f, uint32_t off, void *dst, size_t n) {
    if (fseek(f, (long)off, SEEK_SET) != 0) return false;
    return fread(dst, 1, n, f) == n;
}

// region table — KEEP IN SYNC with tools/ir-pack.mjs REGIONS.
static const char *const REGIONS[] = { "all", "us", "eu", "asia" };
#define NREGION ((uint8_t)(sizeof REGIONS / sizeof REGIONS[0]))

uint8_t ir_pack_region_code(const char *name) {
    if (!name) return 0;
    for (uint8_t i = 0; i < NREGION; i++) if (strcmp(name, REGIONS[i]) == 0) return i;
    return 0;
}
const char *ir_pack_region_name(uint8_t code) { return code < NREGION ? REGIONS[code] : "all"; }

bool ir_pack_open(ir_pack_t *p, const char *path) {
    if (!p || !path) return false;
    memset(p, 0, sizeof *p);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t h[IRPACK_HDR_SIZE];
    if (fread(h, 1, sizeof h, f) != sizeof h || memcmp(h, IRPACK_MAGIC, 4) != 0) { fclose(f); return false; }
    uint16_t ver = rd16(h + 4);
    if (ver != 1 && ver != 2) { fclose(f); return false; }   // accept v1 (raw-less) and v2
    p->f           = f;
    p->version     = ver;
    p->n_remotes   = rd32(h + 8);
    p->remotes_off = rd32(h + 12);
    p->n_tvpower   = rd32(h + 16);
    p->tvpower_off = rd32(h + 20);
    p->raw_off     = (ver >= 2) ? rd32(h + 24) : 0;
    p->raw_size    = (ver >= 2) ? rd32(h + 28) : 0;
    return true;
}

void ir_pack_close(ir_pack_t *p) {
    if (p && p->f) { fclose(p->f); p->f = NULL; }
}

bool ir_pack_remote(ir_pack_t *p, uint32_t i, ir_pack_remote_t *out) {
    if (!p || !p->f || !out || i >= p->n_remotes) return false;
    uint8_t r[IRPACK_REMOTE_SIZE];
    if (!read_at(p->f, p->remotes_off + i * IRPACK_REMOTE_SIZE, r, sizeof r)) return false;
    memcpy(out->name, r, IRPACK_NAME_LEN);
    out->name[IRPACK_NAME_LEN - 1] = 0;
    out->proto    = r[28];
    out->region   = r[29];
    out->nbtn     = rd16(r + 30);
    out->addr     = rd32(r + 32);
    out->data_off = rd32(r + 36);
    return true;
}

bool ir_pack_button(ir_pack_t *p, const ir_pack_remote_t *rm, uint32_t j, ir_pack_btn_t *out) {
    if (!p || !p->f || !rm || !out || j >= rm->nbtn) return false;
    uint8_t b[IRPACK_BTN_SIZE];
    if (!read_at(p->f, rm->data_off + j * IRPACK_BTN_SIZE, b, sizeof b)) return false;
    memcpy(out->key, b, IRPACK_KEY_LEN);
    out->key[IRPACK_KEY_LEN - 1] = 0;
    out->cmd = rd32(b + 12);
    return true;
}

int ir_pack_raw(ir_pack_t *p, uint32_t off, uint16_t *carrier, uint16_t *dur, int cap) {
    if (!p || !p->f || off == 0 || cap <= 0) return 0;
    // Entry header: u16 carrier, u16 count. Reject offsets outside the RAW pool.
    if (p->raw_off && off < p->raw_off) return 0;
    uint8_t hdr[4];
    if (!read_at(p->f, off, hdr, sizeof hdr)) return 0;
    uint16_t car = rd16(hdr), n = rd16(hdr + 2);
    if (n == 0) return 0;
    if ((int)n > cap) n = (uint16_t)cap;          // clamp to the caller's buffer
    if (carrier) *carrier = car;
    for (uint16_t k = 0; k < n; k++) {
        uint8_t b[2];
        if (!read_at(p->f, off + 4 + (uint32_t)k * 2, b, 2)) return k ? (int)k : 0;
        dur[k] = rd16(b);
    }
    return (int)n;
}

bool ir_pack_tvpower(ir_pack_t *p, uint32_t i, ir_pack_tvp_t *out) {
    if (!p || !p->f || !out || i >= p->n_tvpower) return false;
    uint8_t t[IRPACK_TVP_SIZE];
    if (!read_at(p->f, p->tvpower_off + i * IRPACK_TVP_SIZE, t, sizeof t)) return false;
    memcpy(out->brand, t, IRPACK_BRAND_LEN);
    out->brand[IRPACK_BRAND_LEN - 1] = 0;
    out->region = t[20];
    out->proto  = t[21];
    out->addr   = rd16(t + 22);
    out->cmd    = rd16(t + 24);
    return true;
}
