// brawler_scene.cpp — SCORRIBANDA: belt depth plane + MINIMAL backdrop on white paper.
//
// Art direction: no dark noir. Warm-white "paper" background (BR_PAPER), LINE-ONLY shapes in
// grey (outlines, not filled boxes), lots of white space, MULTI-LAYER parallax and a few small
// idle animations. No ground grid: depth reads from the fighters' SCALE.
// Characters stay as clean black silhouettes; blood is the only red. Everything goes through the
// reusable fxscene primitives (ridge/props/clean_floor) — only backdrop greys, no other color here.
//
// Owns: BR_BELT (the depth plane every module uses to measure feet/scale) + scene_draw().

#include "brawler.h"
#include <math.h>

// Belt depth mapping: z=0 (far, small, high) .. z=1 (near, large, low).
//   horizonY = 60  : horizon line, where the skyline rests and the ground begins.
//   frontY   = 130 : near edge of the belt (leaves breathing room for the HUD below, on 135px).
//   farS/nearS = 12/42 : figure scale per unit, far .. near. WIDE RANGE -> anyone heading to the back
//                        visibly shrinks (pronounced depth, per user request).
const fxfig::Belt BR_BELT = { 60.0f, 130.0f, 12.0f, 42.0f };

// Feet (screen-y) for a given depth — used by blood/shadows/draw ordering.
float scene_floor_y(float z) { return fxfig::belt_y(BR_BELT, z); }

// Figure scale for a given depth.
float scene_scale(float z) { return fxfig::belt_s(BR_BELT, z); }

// Backdrop: white paper -> far ridge -> mid props -> near props -> clean ground (no grid).
// Back-to-front composition, parallax driven by g.camx, animation from br_now_ms()/g.floorscroll.
// Per-level variety via the seeds (g.level) and a grey tint from the LevelDef fields (now greys).
void scene_draw(void)
{
    const LevelDef *L = brawler_level(g.level);
    int hy = (int)BR_BELT.horizonY;

    // Timer for idle animations: slow skyline drift + sway of the near props.
    float t = br_now_ms() * 0.001f;

    // (1) base: white paper across the whole sky (above the horizon). Lots of white by design.
    d.fillRect(0, 0, BR_SW, hy, BR_PAPER);

    // The backdrop greys come from the LevelDef (filled in brawler_levels): far->near get darker by
    // a touch going up a level -> each stage has its own shade while staying within the palette.
    uint16_t c_far  = L->build_far;     // far ridge + mid prop base
    uint16_t c_near = L->build_near;     // mid ridge
    uint16_t c_line = L->floor_line;     // near props + horizon line (bolder grey)

    // (2) FAR ridge: low profile, tight spacing, slow parallax + slight drift over time.
    //     Soft fill (a touch darker than the paper) so the distant mass reads softly.
    uint16_t far_fill = fx3d::mix(BR_PAPER, c_far, 60);
    fxscene::ridge(hy, /*minH*/8, /*maxH*/26, /*spacing*/22, c_far, far_fill,
                   /*parallax*/0.10f, g.camx, /*seed*/0x51u + g.level,
                   /*drift*/sinf(t * 0.15f) * 6.0f);

    // (2b) MID-FAR ridge: a second city band for depth, intermediate spacing and parallax.
    fxscene::ridge(hy, /*minH*/12, /*maxH*/34, /*spacing*/28, fx3d::mix(c_far, c_near, 90), 0,
                   /*parallax*/0.16f, g.camx, /*seed*/0x77u + g.level * 11u,
                   /*drift*/sinf(t * 0.11f + 1.0f) * 4.0f);

    // (3) MID ridge: taller and sparser, medium parallax, no fill (line only).
    fxscene::ridge(hy, /*minH*/16, /*maxH*/44, /*spacing*/34, c_near, 0,
                   /*parallax*/0.24f, g.camx, /*seed*/0xA3u + g.level * 7u, /*drift*/0.0f);

    // (4) MID props: shopfronts/arches in outline, medium parallax, static (no motion).
    fxscene::props(hy, /*spacing*/64, c_near, /*parallax*/0.40f, g.camx,
                   /*seed*/0x1Du + g.level * 3u, /*phase*/0.0f, /*kind*/2 /*shopfronts*/);

    // (5) NEAR props: lampposts and foliage in outline, fast parallax, slight sway.
    fxscene::props(hy, /*spacing*/92, c_line, /*parallax*/0.70f, g.camx,
                   /*seed*/0xC7u + g.level * 5u, /*phase*/t * 1.6f, /*kind*/-1 /*mixed*/);

    // (5b) FOREGROUND props: a few large elements very close, parallax almost 1:1, wider sway.
    fxscene::props(hy + 6, /*spacing*/140, c_line, /*parallax*/0.95f, g.camx,
                   /*seed*/0x2Fu + g.level * 13u, /*phase*/t * 2.1f, /*kind*/-1 /*mixed*/);

    // (6) CLEAN ground: no grid. A grey band just below the paper + a single horizon line,
    //     both from the level palette -> variety without leaving the greys.
    fxscene::clean_floor(BR_BELT, L->floor_near, c_line);
}
