# Sprite catalog (CC0 Kenney) — indices + game atlas

Full 12-column tilesheets (index = `row*12 + col`), plus the curated **Orde** game atlas built from
them. Visual reference (6× upscale) in `catalog/dungeon_x6.png` and `catalog/town_x6.png`.

## Tiny Dungeon (`tiny-dungeon/`) — 132 tiles, 16×16
Layout by band (12 cols):
- **rows 0–3** — stone walls, arches, doors, chests, barrels, skulls, levers, fountains.
- **row 4** — tan dungeon floor (cols 0–4), a **diamond/gem** (col 8), stone blocks.
- **rows 5–6** — planks, crates, ladders, a bordered pit frame.
- **rows 7–8** — **heroes**: wizard(84), adventurer(85), old-man(86), viking(87), ranger(88);
  knight(96), knight2(97), kid(98), princess(99), grey-mage(100).
- **row 9** — **monsters**: green-ghost(108), barbarian(109), red-demon(110), ranger(111), ranger(112).
- **row 10** — **beasts**: orange-blob(120), grey-wraith(121), **spider(122)**, rat(123), **wolf(124)**.
- **rows 8–10 right** — swords, shields, potions (green/red/blue), axes, hammer, keys.

## Tiny Town (`tiny-town/`) — 132 tiles, 16×16
- **rows 0–3** — **grass**(0,1), flower/sparkle grass(2), **dirt/sand**(12,13,24,25), grass↔dirt
  edges; **trees** (pine green 4/16/28, autumn-orange 3/15/27), **bush**(5), **mushrooms**(29).
- **rows 4–7** — building walls (blue-stone, red-brick, wood, grey), roofs, windows, doors.
- **rows 8–10** — castle walls, archways, fences, signs, and item icons (bread, chest, bomb,
  pickaxe, pitchfork, key, bow, hammer, shovel, bucket).

## Orde game atlas — `orde_atlas.bin` (30 tiles, 8bpp RGB332, 256 B/tile)
The first 23 tiles were cropped ad-hoc (see `Bash` recipe in git history / `catalog/atlas/`) — their
exact (col,row) crops aren't independently re-derivable, so **never re-crop them**; the table below is
for reference, not a rebuild recipe. Repacked to 8bpp RGB332 by `repack_orde_atlas8.mjs` (matches the
device canvas's own depth and Cardler's atlas format — shared blitters in `tile_blit.h`). Tiles 23-29
were added later by `append_orde_tiles.mjs`, which crops fresh from the same Kenney sheets Cardler uses
and appends past the existing tiles — that script IS the rebuild recipe for anything past index 22.

| Atlas idx | Sprite | Source pack | (col,row) | Role |
|---|---|---|---|---|
| 0 | grass | town | 0,0 | ground base |
| 1 | grass tufts | town | 1,0 | ground variant |
| 2 | flower grass | town | 2,0 | ground detail |
| 3 | dirt | town | 0,1 | ground patch |
| 4 | bush | town | 5,0 | decoration |
| 5 | mushrooms | town | 5,2 | decoration |
| 6 | knight | dungeon | 0,8 | **player** |
| 7 | green ghost | dungeon | 0,9 | **enemy type 0** |
| 8 | red demon | dungeon | 2,9 | **enemy type 1** |
| 9 | spider | dungeon | 2,10 | **enemy type 2** |
| 10 | wolf | dungeon | 4,10 | **enemy type 3** |
| 11 | diamond | dungeon | 8,4 | **xp gem** |
| 12-22 | heroes (knight/wizard/ranger), item icons (sword/axe/dagger/shield/potions), 2-tall tree top+bot | town/dungeon | — | see `app_vs.cpp` `T_N` enum |
| 23 | grass clover | town | 1,0 | ground variant (new) |
| 24 | grass pebble | town | 7,3 | ground variant (new) |
| 25 | sand | town | 3,3 | rare zone tile (new) |
| 26 | sign | town | 11,6 | camp-debris landmark (new) |
| 27 | barrel | town | 11,8 | camp-debris landmark (new) |
| 28 | crate | dungeon | 6,5 | camp-debris landmark (new) |
| 29 | chest | dungeon | 5,7 | camp-debris landmark (new, decorative only — not a pickup) |

Deploy target on device: `/sd/data/Orde/atlas.bin` (loaded once in `on_enter` straight into RAM, no
decode — see `load_atlas()` in `app_vs.cpp`).

## Sounds
See `assets/audio/README.md` — 345 CC0 WAV (16 kHz mono 16-bit), cue→event map for this game.
