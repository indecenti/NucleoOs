# Mini Vampire-Survivors — native game plan (ultra-optimised)

A real-time horde survivor for the Cardputer (240×135, ESP32-S3, **no PSRAM**). The genre is
"hundreds of cheap dumb entities" — a perfect fit *if* built like a 16-bit console, not like a
generic engine. This doc is the build plan + the optimisation contract.

## Design pillars
- 5 heroes, each a distinct **auto-attack** playstyle (melee / homing / orbit / AoE / line).
- Mobile camera over a large (effectively infinite) field.
- Waves ("hordes") that escalate on the survival timer; a boss each minute.
- **Evolved-but-minimal HUD**: thin gauges, maximum play area.
- Hero pick via the existing **`gamefront` carousel** (art + name + power).
- Runs in **Solo boot** (fresh ~200 KB heap, minimal profile) → RAM headroom for pools + atlas.

## Hard budget (the contract)
| Resource | Cap |
|---|---|
| Enemies | 100 (pool) |
| Projectiles | 48 |
| XP gems / pickups | 96 |
| Concurrent SFX voices | 6 (priority + steal) |
| Target frame | 25–30 fps (adaptive: director throttles spawns if frame time spikes) |
| Canvas | 240×135 @ 8bpp (RGB332) = 32 KB |
| Sprite atlas (RAM) | ~16–24 KB (8×8 / 16×16 sprites) |
| Entity pools (SoA) | ~3 KB |
| Spatial grid | ~2–4 KB |

All allocated **heap-on-enter** (Solo), freed on exit. No file-scope `.bss` for these (boot-RAM
discipline). No `malloc` in the frame loop.

## 16-bit-era optimisations (the point of the exercise)
The whole game is built on tricks the SNES/Genesis used because they had the same constraints:

1. **Fixed-point math, no floats in the hot loop.** Positions/velocities as **8.8 fixed** (`int16`
   whole + `uint8` frac, or `int32` 16.16 where range needs it). Integer add/shift only.
2. **Precomputed LUTs.** `sin`/`cos` tables (256-entry, `int8`/`int16`) for orbit weapons, aim,
   knockback, spawn rings. **No `sqrt`** — compare *squared* distances; normalise headings with a
   small direction LUT (octant + lerp) instead of `atan2`.
3. **Sprite/tile atlas + metasprites.** One RAM atlas sprite; draw with `pushImage` region blits
   (color-key transparency). Big things (player, boss) = **metasprites** composed from 8×8 tiles.
4. **Hardware-sprite discipline → caps + culling.** The SNES capped on-screen sprites (128 total,
   32/line); we cap pools and **cull off-camera entities before draw/update**. Stable frame rate
   beats raw entity count.
5. **Tiled scrolling background.** The field is a repeating tile map (like a BG layer with a scroll
   register). Camera = a scroll offset; draw only the tiles under the viewport. Optional cheap
   **parallax** star/fog layer.
6. **Palette-swap animation & effects.** 8bpp RGB332 *is* an indexed-ish palette. Animate hit-flash,
   low-HP pulse, power-up glow by **swapping the sprite's colour**, not redrawing frames. Classic
   palette-cycle for lava/water/aura at ~zero cost.
7. **No enemy↔enemy collision.** Survivors enemies overlap freely — dropping the O(N²) pass is the
   single biggest win. Enemies just steer toward the player (LUT heading).
8. **Uniform-grid spatial hash** (16 px cells over camera+margin). Projectile↔enemy and
   player↔enemy queries scan only local cells → **O(N)**. Rebuilt once per frame from the enemy pool.
9. **Sampled SFX with a fixed voice count.** Deploy the CC0 WAV pack as a `game_sfx` arcade pack
   (`pack/<name>.wav`, zero synth CPU). A tiny **6-voice mixer** with priority + voice-stealing, and
   **rate-limited** high-frequency cues (one enemy-hit per ~40 ms) — the SPC700 8-channel model.
10. **Single canvas blit per frame** (anti-flicker: canvas+poll, one DMA push). Full redraw each
    frame (fillScreen bg + culled sprite blits) — cheaper and simpler than dirty-rect with hordes.
11. **Structure-of-Arrays pools.** Enemies/projectiles/gems as parallel arrays (`x[]`, `y[]`,
    `hp[]`, `type[]`, `alive[]`) — cache-friendly tight loops, free-list index reuse, no per-entity
    alloc.

## The 5 heroes (distinct auto-weapons)
| Hero | Weapon | Feel | Evolution |
|---|---|---|---|
| **Frusta** | Melee arc in move direction | close, high rate | double arc |
| **Mago** | Homing bolts at nearest enemy | ranged pick | +projectiles |
| **Guardiano** | Orbiting orbs (contact) | anti-swarm | +orbs / speed |
| **Piromane** | Periodic AoE nova ring | crowd clear | +radius |
| **Cecchino** | Piercing line shot + knockback | single-target | +pierce |

Each: signature weapon (3–4 upgrade levels) + 2 passives (move speed, magnet, max HP). Small tree —
device-appropriate, not the full VS matrix.

## Loop & systems
- **Director**: wave table over the timer (swarm → encircle → elites → minute boss); spawns just
  outside the camera; ramps difficulty; throttles on frame-time pressure.
- **XP/level**: gems drop → magnet radius pulls them → XP bar → level-up **pauses** into a 3-card
  chooser (reuse carousel styling).
- **HUD (minimal)**: top thin HP + XP + timer + kill count; low-HP palette pulse; nothing else in
  the play area.

## Audio
Library ready: `assets/audio/` (CC0, converted to 16-bit/16 kHz/mono WAV). Cue map in
`assets/audio/README.md`. Deploy the curated subset to the game's SD `pack/` dir.

## Build plan (vertical slices — stop/verify each)
1. **Core proof**: SoA pools + spawn + steer-to-player + spatial grid + 1 weapon (Mago) + timer.
   Prove **100 enemies hold a stable frame** (host-sim the logic where possible; measure on device).
2. **Render**: 8×8 atlas + camera + tiled BG + culling + single blit.
3. **Progression**: gems/XP/magnet + level-up 3-card + the other 4 weapons.
4. **Content**: director waves + minute boss + minimal HUD + 6-voice SFX mixer (WAV pack).
5. **Frontend**: hero-select carousel (gamefront) + run summary.

Gate 1 is the make-or-break: if the grid + SoA + fixed-point hold 100 enemies at frame, the rest is
content. If not, drop caps before adding features.

## Implemented (native app `app_vs.cpp` "Orde", Solo boot)
- **Core** (`vs_sim.c`, portable, host-proven): 100 enemies at **~0.68 µs/frame**, collisions **~750× under
  naive** via the spatial grid, **118 kills / 60 s** at current tuning (`tools/vs-host/vs_host.c`, build
  with `PATH=/c/msys64/mingw64/bin` — the linker subprocs need MinGW's own bin on PATH). Grid scans skip
  enemies killed earlier in the frame (else a pierced/multi-shot double-kill desynced `en_count`).
- **Multi-weapon system** (the Vampire-Survivors core, `vs_sim.c`): the player carries up to `VS_MAX_WEP`
  (6) weapons that **all auto-fire simultaneously**, each on its own cooldown + auto-target. All 5
  archetypes are live: **Mago** (homing bolt fan), **Frusta** (instant melee arc in the facing dir,
  double-sided at Lv3+), **Guardiano** (persistent orbiting orbs — drawn by the app from the *same* LUT
  formula the sim ticks damage on, so visual == hitbox), **Piromane** (periodic AoE nova burst),
  **Cecchino** (fast high-pierce lance). Each levels 1..5. `PK_*` projectile kinds render distinctly;
  `pdmg`/`pkind`/pool raised to 64. Global **passives** (`up_might/haste/area/speed` + magnet/regen/
  maxhp/garlic) buff the *whole* arsenal from one card. Helpers `vs_give_weapon/has_weapon/weapon_maxed`.
- **Heroes → starting kit**: each of the 4 heroes now starts with a different signature weapon (Monk =
  Guardiano+aura, Mago = Mago, Cavaliere = Frusta+tanky, Ranger = Cecchino+haste); everything else is
  earned through level-ups, so two runs of one hero diverge into different builds.
- **Level-up chooser is dynamic** (`roll_ups`/`apply_offer` in `app_vs.cpp`): each level-up builds a
  candidate pool of *learn-a-new-weapon* / *level-an-owned-weapon* / *take-a-passive* offers, Fisher-Yates
  picks 3. Cards show a NUOVA ARMA / Lv-N tag so you know if you're widening or deepening the build.
- **Elites + pickups**: an **elite/miniboss** (`eelite`) spawns every ~30 s — big, tanky (6×+24 HP),
  slower, drawn larger with a purple threat ring + HP bar; it always drops a **fat xp gem** (`gval`, blue,
  worth 6) and a **pickup** — a heal (chicken, +25 HP) or, every 3rd elite, a **bomb** that clears the
  screen (`want_bomb` → `hit_radius` 300 px + a white on-screen flash).
- **Minute boss + juice** (`eboss`): every 60 s a **BOSS** spawns — enormous HP (scales per minute), a
  huge pulsing aura, a **radial bullet-hell ring** every 75 f, an on-screen boss HP bar, and a **12-gem +
  heal jackpot** on death. Plus game-feel: enemies **flash white** on hit (`ehflash`), and **screen shake**
  (`shake`, camera jitter) fires on player hits and boss death. All still host-proven O(N), pools bounded
  (20/64 proj peak), ~0.65 µs/frame.
- **Sprites**: curated CC0 Kenney atlas → deployed as a raw **RGB565** strip (`/sd/data/Orde/atlas.bin`,
  hard alpha-keyed to magenta → clean cutouts, no PNG decode on device). Baked **2× (32×32)** with a
  mirrored copy at load for big readable sprites.
- **Animation**: Kenney "Tiny" characters are single front frames (no walk frames in-pack), so — the
  documented single-sprite top-down technique — entities **flip L/R to face the player** (player faces
  last move dir) plus a **1 px vertical bob**. Coherent, zero extra frames, ~zero cost.
- **World**: tiled grass with coarse dirt patches, scattered tufts, rare flowers + sparse bush/mushroom
  decoration (deterministic per-cell hash). 2-tall trees check the neighbor cell their canopy lands on
  (same hash formula, one row up) before planting, so a tree never silently overdraws a bush/mushroom/
  another tree the cell above already claimed for itself (Cardler's `tallTree()` "is it free" guard,
  ported to the hash-based world instead of Cardler's static map).
- **RAM/CPU discipline**: VS world (~7 KB) + ground tiles (~6 KB) + 2×/mirrored entity sprites (~24 KB)
  are **heap-on-enter**, freed on exit; opaque ground blit (no key scan); **strong camera culling** —
  the sim skips steering and the renderer skips drawing any enemy beyond ~640 px from the player, so no
  CPU is spent on off-camera entities.

- **Menus + progression**: START title, hero-select carousel, the dynamic 3-card level-up chooser (above),
  and a full run-summary GAME OVER screen (hero, survival time, level, kills, plus the persisted best-run
  record — `best.json` — and a NUOVO RECORD banner). State machine START → PLAY → LEVELUP → OVER. All 3
  upgrade cards stay visible at once (compare-at-a-glance, not a one-at-a-time carousel).
- **HUD**: HP/XP bars sit a few px off the literal top edge and are thicker than before — a hairline glued
  to row 0 was easy to miss on real hardware. A numeric `HP xx/xx` readout rides the timer/level text row
  (already proven visible) as a zero-risk backup to the graphic bar, colour-matched to the same
  green/yellow/red/blink danger read.
- **Waves**: besides the sustained count, a periodic **ENCIRCLE wave** spawns a full ring of enemies
  around the player (`spawn_ring`, LUT-placed) every ~16 s, count `10+level`. Ring radius is kept near
  the *screen*, not the 512 px sim window (150 px, widening to 230 px with level) — the ring reads as
  "player at the centre, enemies closing in" the instant it spawns, plus a red/black border flash
  (`s_wave_flash`) telegraphs the ambush even where part of the ring is just off-screen. Ring-wave
  enemies are **ranged** (`eranged`/`efire_cd` per-enemy): they close in like the regular horde but also
  fire a bolt at the player on their own staggered cooldown (~3-4 s). Enemy bolts are a distinct
  projectile pool entry (`powner=1`) that only ever threatens the player (O(1) distance check, no grid
  scan) — colour-coded red/orange vs. the player's green bolts, with a blinking red pip over armed
  enemies.
- **Difficulty scaling**: enemy HP (`enemy_hp()`) now carries a level-linked bonus (`plevel/4`, capped
  +10) on top of the type-based base, so the horde keeps pace with the player's own level-scaled Mago
  instead of staying flat once all 4 types unlock (~90 s in). The sustained-count ramp was widened too
  — was `20 + frame/60` capped at 55 (maxed out after ~70 s and then plateaued for the rest of the
  run); now `20 + frame/90 + level` capped at 85, so pressure keeps climbing with both survival time
  and level instead of flatlining early.
- **Sound**: CC0 Kenney WAV cues deployed to `/sd/data/Orde/pack/*.wav` and played via the shared
  `game_sfx` engine (single-channel, non-important cues skip while busy = free rate-limiting; degrades to
  a synth tone if a WAV is missing). Events: die / hurt / level-up / wave / start / over / select.

Next (not yet done): **weapon evolutions** (max a weapon + hold the right passive → an evolved form, the
signature VS hook), a **1-minute boss** distinct from the periodic elite, treasure chests, and a coin /
meta-currency between runs. The weapon/passive/offer scaffolding is all in place for evolutions now.
