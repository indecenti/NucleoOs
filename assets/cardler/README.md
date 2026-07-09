# Cardler — procedural top-down RPG (native, Solo boot)

A top-down RPG built on the **same sandbox as Orde** (`app_vs`): an SD raw **8bpp RGB332** atlas (matches
the device canvas's own depth — half the RAM of RGB565 and a straight memcpy blit, no per-pixel convert),
a per-pixel magenta colour-key blitter, a camera over a tile world, held-key movement. The three blitters
(`blit_op`/`blit_key`/`blit_sz`) live in the shared `firmware/components/nucleo_app/tile_blit.h`, used by
both Cardler and Orde.

The **whole world is procedural and data-driven** — no hand-authored maps or hand-written tile switches.

## Pipeline (re-run to reshape / grow / reseed the world)
```
node assets/cardler/build_atlas.mjs           # crop tiles -> cardler_atlas.bin (+ atlas_v.rgba, atlas_enum.json)
node assets/cardler/worldgen.mjs [seed] [W] [H]  # -> firmware/components/nucleo_app/cardler_world.h
node assets/cardler/render_preview.mjs        # host QA: render the generated world to _worldmap.rgba
node assets/cardler/test_scene.mjs            # host QA: a reference scene (houses/castle/props/chars)
```
Then rebuild the firmware and deploy `cardler_atlas.bin` (8bpp RGB332) -> `/sd/data/Cardler/atlas.bin`.

## build_atlas.mjs — the tile set
Crops 16×16 tiles from the **individual** CC0 Kenney files `assets/coop-rpg/tiny-{town,dungeon}/Tiles/tile_NNNN.png`
(those are TRUE RGBA; the packed `tilemap.png` is a **palette** PNG with no per-pixel alpha → cropping it
yields opaque-black backgrounds). Packs to raw **8bpp RGB332** with an exact magenta key (`alpha<128 → 0xE3`;
a real-magenta opaque pixel is nudged to `0xE7` so it's never keyed). `TILES[]` is the single source of
truth for the atlas order and the `C_*` enum (`atlas_enum.json`). Every tile cataloged in `TILES[]` is
actually **placed** by worldgen — no dead art (e.g. LADDER, the arch/gate/courtyard castle pieces were
dropped as never-placeable, reclaiming RAM). Opaque base tiles that need a mirror (`WALLE`/`WWALLE`) bake
their `_R` flip at build time (`blit_op` has no runtime flip).

Correct Kenney autotile pieces are used: red/grey **roof L/M/R ridge + eave**, dedicated **wall edge +
fill** with framed **window** + **door**, a grass↔tan-ground **9-slice** (paths, plazas, plowed fields
AND the beach coast all share it — identical tan fill, seamless), fence + minecart-rail corners, and a
single closed manor-**castle** with corner turrets/merlons (the old arch/gate/courtyard was a darker
sub-palette that never blended, so it's gone).

## worldgen.mjs — the world
Seeded PRNG (mulberry32). Places zones (forest belt, village, castle, faro peninsula + beach, farm),
stamps multi-tile structures with footprint checks, carves a road network to every door, lays a rail line,
scatters decoration, and spawns NPCs + roaming beasts. Emits `cardler_world.h`:
- `enum { C_* }` + `MAP_W/H` + `MAP[]`.
- **`TID/OVL/SOL/FLP[128]`** — per-ASCII lookup: opaque base tile, keyed overlay (255=none), solid flag,
  overlay mirror (bit0 H / bit1 V). The firmware needs **no hand-written tile switches**. A guard
  auto-demotes any transparent base tile to a GRASS ground + keyed overlay (prevents black/magenta boxes).
- `ENT[NENT]` — NPCs (kind 0, block + dialogue) and beasts (kind 1, contact damage), with patrol spans.

World-richness passes (all reuse existing tiles, no new art):
- **Coherent beach** — the faro coast is a per-row-jittered coastline (not the old 85%-chance swiss
  cheese); the grass↔sand boundary is grass-fringed by the shared tan 9-slice, filled mostly with plain
  PATH (SAND's baked-in grass sprout tiles into an ugly grid as a majority fill) + sparse dune tufts.
- **Working farmland** — open plowed `field()`s (tilled earth + alternating crop rows, `G` = a plant on
  dirt that never mis-fringes the furrows) fill the once-bare farm zone, alongside the fenced flower gardens.
- **Flower meadows** — clustered flower/clover blobs in the open country (uniform per-tile scatter never
  reads as a "field of flowers"; a blob does).
- **Guaranteed sprite variety** — NPC/beast spawns draw from a **shuffle-bag** (without replacement) so
  every cataloged villager (VILL/KID/PRIN/OLD/WIZ/VIKING/WITCH) and beast (per zone) is guaranteed to
  appear, instead of some being left out by the luck of independent random picks.

## Firmware (`app_cardler.cpp`, NX_SOLO)
Includes `cardler_world.h`. Draws each cell `blit_op(TID[c])` then `blit_key(OVL[c])`; collision from
`SOL[]` (feet-box, per-axis slide); chests open to gold (looted set in RAM, MAP is const flash); beasts
deal contact damage with i-frames. Registered in `nucleo_app.cpp`, listed in `CMakeLists.txt`.

Reference: the Kenney **Tiled** `sampleMap.tmx` (dungeon) documents flip-flag autotiling — tile GID high
bits `0x80000000` H, `0x40000000` V, `0x20000000` diagonal; id = `gid & 0x1FFFFFFF`. Kenney builds all four
corners/edges from ONE tile flipped. Cardler uses explicit corner tiles today; the blitter already
H-mirrors (`f`), so flip-based autotiling is a future size optimisation.
