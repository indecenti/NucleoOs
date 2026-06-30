// Host dumper for the IRPK pack reader (nucleo_ir_pack.c). Opens the .bin passed as argv[1] and
// prints every record in a stable tab-delimited form so ir-pack-check.mjs can diff the C reader's
// view against the JS packer's own unpack() — a cross-implementation round-trip. Proto/region are
// printed as their numeric codes (impl-agnostic). See ir-pack-check.mjs for the harness.
#include "nucleo_ir_pack.h"
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 2) { printf("NOARG\n"); return 2; }
    ir_pack_t p;
    if (!ir_pack_open(&p, argv[1])) { printf("OPEN_FAIL\n"); return 0; }  // JS asserts this on a junk file
    printf("H\t%u\t%u\n", (unsigned)p.n_remotes, (unsigned)p.n_tvpower);

    for (uint32_t i = 0; i < p.n_remotes; i++) {
        ir_pack_remote_t r;
        if (!ir_pack_remote(&p, i, &r)) { printf("RFAIL\t%u\n", (unsigned)i); continue; }
        printf("R\t%u\t%s\t%u\t%u\t%u\t%u\n",
               (unsigned)i, r.name, r.proto, r.region, (unsigned)r.nbtn, (unsigned)r.addr);
        for (uint32_t j = 0; j < r.nbtn; j++) {
            ir_pack_btn_t b;
            if (!ir_pack_button(&p, &r, j, &b)) { printf("BFAIL\t%u\t%u\n", (unsigned)i, (unsigned)j); continue; }
            printf("B\t%u\t%u\t%s\t%u\n", (unsigned)i, (unsigned)j, b.key, (unsigned)b.cmd);
            if (r.proto == IRPACK_PROTO_RAW) {   // RAW button: cmd field is the raw-pool offset
                uint16_t car = 0, dur[400]; int nd = ir_pack_raw(&p, b.cmd, &car, dur, 400);
                printf("RB\t%u\t%u\t%u\t%d", (unsigned)i, (unsigned)j, car, nd);
                for (int k = 0; k < nd; k++) printf("\t%u", dur[k]);
                printf("\n");
            }
        }
    }
    for (uint32_t i = 0; i < p.n_tvpower; i++) {
        ir_pack_tvp_t t;
        if (!ir_pack_tvpower(&p, i, &t)) { printf("TFAIL\t%u\n", (unsigned)i); continue; }
        printf("T\t%u\t%s\t%u\t%u\t%u\t%u\n",
               (unsigned)i, t.brand, t.region, t.proto, (unsigned)t.addr, (unsigned)t.cmd);
        if (t.proto == IRPACK_PROTO_RAW) {       // RAW row: offset = (addr<<16 | cmd)
            uint16_t car = 0, dur[400]; int nd = ir_pack_raw(&p, ir_pack_tvp_raw_off(&t), &car, dur, 400);
            printf("TR\t%u\t%u\t%d", (unsigned)i, car, nd);
            for (int k = 0; k < nd; k++) printf("\t%u", dur[k]);
            printf("\n");
        }
    }
    // bounds guard: one-past-the-end must be rejected (prints "0\t0")
    ir_pack_remote_t r; ir_pack_tvp_t t;
    printf("OOB\t%d\t%d\n", ir_pack_remote(&p, p.n_remotes, &r) ? 1 : 0,
                            ir_pack_tvpower(&p, p.n_tvpower, &t) ? 1 : 0);
    ir_pack_close(&p);
    return 0;
}
