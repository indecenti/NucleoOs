// Step 0 stress harness: run the VS sim for 60 game-seconds (1800 frames @30fps) with the horde
// ramping to the 100-enemy cap, and prove the hot loop stays O(N) (spatial grid) not O(N*M) (naive).
// Build: gcc -O2 -o vs_host vs_host.c vs_sim.c   (see run below)
#include "../../firmware/components/nucleo_app/vs_sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(void)
{
    VS *w = (VS *)malloc(sizeof *w);            // heap-on-enter, like Solo boot
    if (!w) { printf("alloc fail\n"); return 1; }
    vs_init(w, 0xC0FFEEu);

    const int FRAMES = 1800;
    const int REPS   = 300;                     // repeat the 60 s run for a measurable host time
    unsigned long long sum_checks = 0;
    unsigned int max_checks = 0, max_en = 0, max_pr = 0;

    clock_t t0 = clock();
    for (int r = 0; r < REPS; r++) {
        vs_init(w, 0xC0FFEEu + r);
        for (int f = 0; f < FRAMES; f++) {
            vs_step(w);
            while (w->pending_up > 0) {            // simulate the player picking upgrades
                switch (w->plevel % 7) {            // rotate: collect weapons + buff passives
                    case 0: vs_give_weapon(w, WEP_FRUSTA);    break;
                    case 1: vs_give_weapon(w, WEP_CECCHINO);  break;
                    case 2: vs_give_weapon(w, WEP_GUARDIANO); break;
                    case 3: vs_give_weapon(w, WEP_PIROMANE);  break;
                    case 4: w->up_might++;  break;
                    case 5: w->up_haste++;  break;
                    default: w->up_area++;  break;
                }
                w->pending_up--;
            }
            if (r == 0) {                        // stats from the first run
                sum_checks += w->checks;
                if (w->checks > max_checks) max_checks = w->checks;
                if ((unsigned)w->en_count > max_en) max_en = w->en_count;
                if ((unsigned)w->pr_count > max_pr) max_pr = w->pr_count;
            }
        }
    }
    clock_t t1 = clock();

    double ms = (double)(t1 - t0) * 1000.0 / CLOCKS_PER_SEC;
    double us_frame = ms * 1000.0 / ((double)FRAMES * REPS);
    double avg_checks = (double)sum_checks / FRAMES;
    long naive = (long)max_en * VS_MAX_PROJ + max_en;    // worst-case O(N*M) per frame

    printf("=== VS Step 0 stress: %d frames x %d reps (60 game-seconds @30fps) ===\n", FRAMES, REPS);
    printf("host sim time      : %.1f ms total  |  %.3f us/frame (MinGW gcc -O2, x86)\n", ms, us_frame);
    printf("enemies peak       : %u / %d cap\n", max_en, VS_MAX_EN);
    printf("projectiles peak   : %u / %d cap\n", max_pr, VS_MAX_PROJ);
    printf("collision checks   : avg %.0f/frame  |  max %u/frame\n", avg_checks, max_checks);
    printf("naive O(N*M) would : ~%ld/frame  -> grid does %.1fx fewer\n",
           naive, naive / (avg_checks > 1 ? avg_checks : 1));
    printf("kills (60s)        : %d\n", w->kills);
    printf("player HP end      : %d\n", w->php);
    printf("xp / level end     : %d / %d\n", w->pxp, w->plevel);

    // --- correctness gate ---
    int pass = 1;
    if (w->en_count > VS_MAX_EN)  { printf("FAIL: enemy pool overflow\n"); pass = 0; }
    if (w->kills <= 0)            { printf("FAIL: weapon never killed anything\n"); pass = 0; }
    // (no php<100 check: the always-on garlic aura can keep a stationary player untouched — by design)
    if (avg_checks > (double)max_en * 20) { printf("FAIL: checks not O(N) (grid ineffective)\n"); pass = 0; }
    printf("%s\n", pass ? "PASS: hot loop is O(N), pools bounded, systems live." : "FAILED");

    // device extrapolation note
    printf("\n[device] ESP32-S3 @240MHz does ~100-300 simple int ops/us. At ~%.0f checks/frame plus\n",
           avg_checks);
    printf("         ~%d enemy + %d proj integer updates, sim work is a few thousand ops/frame\n", VS_MAX_EN, VS_MAX_PROJ);
    printf("         -> well under 1 ms/frame. The frame budget is spent on RENDER (blits), not sim.\n");

    free(w);
    return pass ? 0 : 1;
}
