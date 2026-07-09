# Co-op RPG — asset library (CC0)

Source art for the native Cardputer co-op RPG concept. **All CC0** (Kenney) — public domain,
commercial use OK, no attribution required. Keep `License.txt` alongside each pack.

## Packs

| Pack | Tiles | Tile size | Packed sheet | Use |
|---|---|---|---|---|
| `tiny-dungeon/` | 132 | 16×16 | `Tilemap/tilemap_packed.png` 192×176 (12×11) | Interiors / closed rooms: walls, floors, doors, chests, adventurer + enemy chars, weapons/items |
| `tiny-town/`    | 132 | 16×16 | `Tilemap/tilemap_packed.png` 192×176 (12×11) | Overworld: grass, water, trees, paths, houses, fences, townsfolk NPCs |

Each pack also ships 132 individual `Tiles/tile_NNNN.png` (same art, one file per tile) and a
`Tilesheet.txt`. `tiny-dungeon` additionally has a `Tiled/` project (for the Tiled map editor).

## Fit for Cardputer (240×135, 8bpp RGB332 canvas, no PSRAM)

- **Tile size 16×16 is correct — NO resizing for the overworld.** Screen shows ~15×8 tiles
  (GB showed 10×9), a good Pokémon-style viewport.
- **Full atlas RAM cost** (whole 132-tile sheet held in a RAM sprite):
  - **33.8 KB @ 8bpp (RGB332)** — matches the device canvas, cheapest.
  - 67.6 KB @ RGB565 — better colour, still affordable in Solo boot.
- Games run in **Solo boot** (fresh ~200 KB heap, minimal profile), so atlas (34 KB @8bpp) +
  screen canvas (32 KB @8bpp) ≈ 66 KB coexist comfortably. **Verdict: not too big.**
- **8bpp is the smart choice**: matches the canvas and dodges the RGB332 banding seen elsewhere;
  Kenney "Tiny" palette is limited so it survives the quantisation well.
- **Battle boss sprites**: source chars/creatures are 16×16 → upscale ×2/×3 (integer, cheap) at
  draw time for the battle view. No pre-scaled assets needed.

## Pipeline (planned)

Two options, both keep the art on SD (device-load discipline):
1. **Decode-once**: load `tilemap_packed.png` into a RAM sprite at zone load (M5GFX PNG decode,
   paid once), then `pushImage` 16×16 regions to the screen canvas per frame. No converter needed.
2. **Pre-baked atlas**: PC converter PNG → raw RGB565/8bpp `.atlas` on SD for instant load (no
   decode). Faster zone loads; build it if PNG decode latency is an issue.

## Not included / TODO

- Monster roster for battles: reuse enemy chars from `tiny-dungeon` (upscaled), or add a CC0
  creature pack (e.g. OpenGameArt "Tiny Creatures") after license check.
- No Nintendo/Pokémon assets — IP; the "Pokémon feel" comes from structure, not stolen sprites.
