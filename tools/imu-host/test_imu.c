// Host verification for nucleo_imu_motion — the real firmware motion classifier, compiled on the PC.
// Validates the MATH: gravity tracking, energy EMA, the still/hand/move thresholds and the
// hysteresis. What this CANNOT prove: that the BMI270 actually streams on a given ADV unit, or that
// the board axes are mapped right — that proof is the on-device test on a real Cardputer ADV.
#include "../../firmware/components/nucleo_imu/include/nucleo_imu_motion.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define G 16384.0f   // 1g in LSB at the +/-2g range nucleo_imu configures

// Feed `n` samples of (bx,by,bz) plus uniform jitter of +/-jit on each axis; return the final state.
static nk_motion_t feed(nk_motion_state *m, int n, float bx, float by, float bz, float jit)
{
    nk_motion_t s = NK_MOTION_UNKNOWN;
    for (int i = 0; i < n; i++) {
        float jx = jit ? ((rand() % 2001) - 1000) / 1000.0f * jit : 0.0f;
        float jy = jit ? ((rand() % 2001) - 1000) / 1000.0f * jit : 0.0f;
        float jz = jit ? ((rand() % 2001) - 1000) / 1000.0f * jit : 0.0f;
        s = nk_motion_push(m, bx + jx, by + jy, bz + jz);
    }
    return s;
}

int main(void)
{
    srand(12345);
    nk_motion_state m;

    // 1) reset -> UNKNOWN, not seeded
    nk_motion_reset(&m);
    CHECK(m.state == NK_MOTION_UNKNOWN, "reset must yield UNKNOWN, got %s", nk_motion_name(m.state));
    CHECK(m.seeded == 0, "reset must clear seeded");

    // 2) flat on a desk (gravity on Z, no jitter) -> STILL
    nk_motion_reset(&m);
    nk_motion_t s = feed(&m, 60, 0, 0, G, 0.0f);
    CHECK(s == NK_MOTION_STILL, "constant gravity must read STILL, got %s", nk_motion_name(s));
    CHECK(m.energy < 0.01f, "still energy must be near zero, got %.4f", m.energy);

    // 3) held in hand: gravity + small tremor -> HAND (not STILL, not MOVE)
    nk_motion_reset(&m);
    s = feed(&m, 80, 0, 0, G, 0.06f * G);   // ~6% g tremor
    CHECK(s == NK_MOTION_HAND, "hand tremor must read HAND, got %s", nk_motion_name(s));

    // 4) walking / shaking: large swings -> MOVE
    nk_motion_reset(&m);
    s = NK_MOTION_UNKNOWN;
    for (int i = 0; i < 40; i++) s = nk_motion_push(&m, (i & 1) ? G : -G, 0, 0);  // full +/-1g flips
    CHECK(s == NK_MOTION_MOVE, "large swings must read MOVE, got %s", nk_motion_name(s));

    // 5) monotonic: more agitation never reports a CALMER state than less
    nk_motion_state a, b, c;
    nk_motion_reset(&a); nk_motion_reset(&b); nk_motion_reset(&c);
    nk_motion_t sa = feed(&a, 80, 0, 0, G, 0.0f);
    nk_motion_t sb = feed(&b, 80, 0, 0, G, 0.06f * G);
    nk_motion_t sc = feed(&c, 80, 0, 0, G, 0.40f * G);
    CHECK(sa <= sb && sb <= sc, "states must be monotone in agitation: %s %s %s",
          nk_motion_name(sa), nk_motion_name(sb), nk_motion_name(sc));

    // 6) hysteresis: from MOVE, settling to perfect rest decays back through to STILL (eventually)
    nk_motion_reset(&m);
    for (int i = 0; i < 40; i++) nk_motion_push(&m, (i & 1) ? G : -G, 0, 0);   // drive to MOVE
    CHECK(m.state == NK_MOTION_MOVE, "precondition: should be MOVE");
    s = feed(&m, 120, 0, 0, G, 0.0f);   // then perfectly still for a while
    CHECK(s == NK_MOTION_STILL, "MOVE must decay back to STILL once at rest, got %s", nk_motion_name(s));

    // 7) labels
    CHECK(strcmp(nk_motion_name(NK_MOTION_STILL), "still") == 0, "label still");
    CHECK(strcmp(nk_motion_name(NK_MOTION_HAND),  "hand")  == 0, "label hand");
    CHECK(strcmp(nk_motion_name(NK_MOTION_MOVE),  "move")  == 0, "label move");
    CHECK(strcmp(nk_motion_name(NK_MOTION_UNKNOWN), "?")   == 0, "label unknown");

    // 8) orientation from a gravity vector (Z = screen normal, +Z = screen up)
    CHECK(nk_orient_classify(0, 0,  G) == NK_ORIENT_FLAT_UP,   "screen-up gravity => flat-up");
    CHECK(nk_orient_classify(0, 0, -G) == NK_ORIENT_FLAT_DOWN, "screen-down gravity => flat-down");
    CHECK(nk_orient_classify(G, 0,  0) == NK_ORIENT_UPRIGHT,   "gravity in plane (X) => upright");
    CHECK(nk_orient_classify(0, G,  0) == NK_ORIENT_UPRIGHT,   "gravity in plane (Y) => upright");
    CHECK(nk_orient_classify(0, 0.6f * G, 0.8f * G) == NK_ORIENT_TILTED, "45-ish => tilted");
    CHECK(nk_orient_classify(0, 0, 0.4f) == NK_ORIENT_UNKNOWN, "tiny vector => unknown");
    CHECK(strcmp(nk_orient_name(NK_ORIENT_FLAT_UP),   "flat-up")   == 0, "label flat-up");
    CHECK(strcmp(nk_orient_name(NK_ORIENT_FLAT_DOWN), "flat-down") == 0, "label flat-down");
    CHECK(strcmp(nk_orient_name(NK_ORIENT_UPRIGHT),   "upright")   == 0, "label upright");
    CHECK(strcmp(nk_orient_name(NK_ORIENT_TILTED),    "tilted")    == 0, "label tilted");

    // 9) bubble level math (lx/ly = screen-plane gravity, deg = tilt from flat)
    float lx, ly, deg;
    CHECK(nk_level(0, 0,  G, &lx, &ly, &deg) == 1 && deg < 1.0f, "flat => ~0deg, got %.2f", deg);
    CHECK(nk_level(0, 0, -G, &lx, &ly, &deg) == 1 && deg < 1.0f, "flat face-down => ~0deg, got %.2f", deg);
    // 30 deg tilt about the Y axis: gx = sin30*G, gz = cos30*G
    CHECK(nk_level(0.5f * G, 0, 0.8660254f * G, &lx, &ly, &deg) == 1 && deg > 28.0f && deg < 32.0f,
          "30deg tilt => ~30, got %.2f", deg);
    CHECK(fabsf(lx - 0.5f) < 0.02f, "30deg tilt => lx ~0.5, got %.3f", lx);
    CHECK(nk_level(G, 0, 0, &lx, &ly, &deg) == 1 && deg > 88.0f, "on edge => ~90, got %.2f", deg);
    CHECK(nk_level(0, 0, 0.4f, &lx, &ly, &deg) == 0, "tiny vector => 0 (untrusted)");

    // 10) pedometer: synthetic walking = a 2 Hz oscillation on the vertical axis -> ~1 step per cycle
    {
        nk_step_state st; nk_step_reset(&st);
        int fs = 50, secs = 15; float freq = 2.0f, amp = 5000.0f;   // 2 steps/s for 15 s -> ~30 steps
        unsigned last = 0;
        for (int n = 0; n < fs * secs; n++) {
            float t = (float)n / fs;
            float az = G + amp * sinf(2.0f * (float)M_PI * freq * t);
            last = nk_step_push(&st, 0.0f, 0.0f, az);
        }
        CHECK(last >= 26 && last <= 34, "walking 2Hz/15s => ~30 steps, got %u", last);

        nk_step_reset(&st);                                          // 11) at rest -> no false steps
        for (int n = 0; n < fs * 5; n++) {
            float j = ((rand() % 201) - 100) / 100.0f * 60.0f;      // tiny noise only
            last = nk_step_push(&st, 0.0f, 0.0f, G + j);
        }
        CHECK(last == 0, "still => 0 steps, got %u", last);
    }

    printf("\nnucleo_imu_motion host test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
