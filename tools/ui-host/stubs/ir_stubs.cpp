// Host stub for app_ir.cpp: a small FICTIONAL IR catalog so the large layouts render with real data.
// The real catalog is a packed binary on the SD card (presets.bin); a return-0 stub would just show
// "No catalog on SD". These functions stand in for nucleo_ir_pack.h / nucleo_ir_proto.h with invented
// remotes, buttons and TV-power codes. Nothing here touches hardware or files.
#include "nucleo_ir_pack.h"
#include "nucleo_ir_proto.h"
#include <string.h>

extern "C" {

// ---- fictional remote catalog, grouped by category (as the real pack is packed sorted) ----
struct FRemote { const char *name; uint8_t proto; uint8_t nbtn; const char *cat; };
static const FRemote REMS[] = {
    { "Samsung TV",    NIR_PROTO_SAMSUNG, 8, "tv" },
    { "Sony Bravia",   NIR_PROTO_SONY12,  8, "tv" },
    { "LG TV",         NIR_PROTO_NEC,     6, "tv" },
    { "Yamaha Amp",    NIR_PROTO_NEC,     5, "audio" },
    { "Bose Soundbar", NIR_PROTO_RC5,     4, "audio" },
    { "Daikin AC",     NIR_PROTO_NECEXT,  4, "aircon" },
};
static const int N_REM = (int)(sizeof(REMS) / sizeof(REMS[0]));
static const char *const BTNS[] = { "power", "vol+", "vol-", "ch+", "ch-", "mute", "menu", "ok" };

struct FTvp { const char *brand; uint8_t region; uint8_t proto; };
static const FTvp TVP[] = {
    { "Samsung", 1, NIR_PROTO_SAMSUNG }, { "Sony", 2, NIR_PROTO_SONY12 }, { "LG", 1, NIR_PROTO_NEC },
    { "Philips", 2, NIR_PROTO_RC5 },     { "Panasonic", 3, NIR_PROTO_PANASONIC }, { "Toshiba", 0, NIR_PROTO_NEC },
    { "Sharp", 3, NIR_PROTO_NEC },       { "Vizio", 1, NIR_PROTO_NEC }, { "Grundig", 2, NIR_PROTO_RC5 },
    { "Hisense", 3, NIR_PROTO_NEC },     { "TCL", 0, NIR_PROTO_NEC }, { "JVC", 0, NIR_PROTO_JVC },
};
static const int N_TVP = (int)(sizeof(TVP) / sizeof(TVP[0]));

bool ir_pack_open(ir_pack_t *p, const char *path)
{
    (void)path;
    if (!p) return false;
    memset(p, 0, sizeof *p);
    p->version = IRPACK_VERSION;
    p->rem_size = IRPACK_REMOTE_SIZE;
    p->n_remotes = (uint32_t)N_REM;
    p->n_tvpower = (uint32_t)N_TVP;
    return true;
}
void ir_pack_close(ir_pack_t *p) { if (p) p->f = nullptr; }

bool ir_pack_remote(ir_pack_t *p, uint32_t i, ir_pack_remote_t *out)
{
    (void)p;
    if (!out || (int)i >= N_REM) return false;
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", REMS[i].name);
    out->proto = REMS[i].proto;
    out->region = 0;
    out->nbtn = REMS[i].nbtn;
    out->addr = 0x10 + i;
    out->data_off = i;
    snprintf(out->category, sizeof out->category, "%s", REMS[i].cat);
    return true;
}
bool ir_pack_button(ir_pack_t *p, const ir_pack_remote_t *r, uint32_t j, ir_pack_btn_t *out)
{
    (void)p;
    if (!out || !r || j >= r->nbtn) return false;
    memset(out, 0, sizeof *out);
    snprintf(out->key, sizeof out->key, "%s", BTNS[j % (sizeof(BTNS) / sizeof(BTNS[0]))]);
    out->cmd = (uint32_t)(j + 1);
    return true;
}
bool ir_pack_tvpower(ir_pack_t *p, uint32_t i, ir_pack_tvp_t *out)
{
    (void)p;
    if (!out || (int)i >= N_TVP) return false;
    memset(out, 0, sizeof *out);
    snprintf(out->brand, sizeof out->brand, "%s", TVP[i].brand);
    out->region = TVP[i].region;
    out->proto = TVP[i].proto;
    out->addr = (uint16_t)(0x20 + i);
    out->cmd = (uint16_t)(i + 1);
    return true;
}
int ir_pack_raw(ir_pack_t *p, uint32_t off, uint16_t *carrier, uint16_t *dur, int cap)
{
    (void)p; (void)off; (void)dur; (void)cap;
    if (carrier) *carrier = 38000;
    return 0;
}

const char *nir_proto_name(nir_proto_t proto)
{
    static const char *const N[] = { "raw", "nec", "necext", "samsung", "sony12", "sony15",
                                     "sony20", "rc5", "jvc", "panasonic" };
    return (proto >= 0 && proto < NIR_PROTO__COUNT) ? N[proto] : "?";
}
nir_proto_t nir_proto_from_name(const char *name) { (void)name; return NIR_PROTO_NEC; }

}  // extern "C"
