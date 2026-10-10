# Costellazioni — design

One campaign, two windows on it. On the **Cardputer** (native, `app_constellations.cpp`) Costellazioni is a
compact space trader/fighter you play anywhere. In the **web OS** (Game Center, `apps/games/www/games/
constellations*.js`) the same run opens up into an immersive 3D game: an endless procedural galaxy to fly
through, planets to descend onto, and space battles that feel like a modern Star Wars dogfight. Progress made
in one is there in the other — the save is shared (`/sd/data/costellazioni/save.bin`, JSON at
`/api/game/costellazioni/save`, see `constellations-save.js`).

Status: v1 web (rail dogfight + hub) and native exist. This document is the target for the rebuild started
2026-10-10. Milestones at the end.

## Pillars
1. **Wonder** — every system looks different and worth seeing: a star you can feel, planets with weather and
   rings, nebulae, stations that glitter, wrecks with stories. Infinite, but never samey.
2. **Combat that hits** — fast, readable 6DOF dogfights: power juggling, lock-on, shields that flare, ships that
   break apart, capital ships with turrets and subsystems, squads that fight as squads.
3. **A world with a past** — a lore you uncover in fragments: beacons, ruins, logs, factions with motives.
4. **Your run, everywhere** — the Cardputer and the browser are one campaign; nothing done in either is lost.
5. **Light on the wire** — served from an ESP32's SD over Wi-Fi: it starts fast and streams the rest. The worlds
   are generated, not downloaded.

## Lore — "The Lit Network"
Long before the Guild, the stars were linked by the **Beacons** — lattice lighthouses of a vanished people the
Keepers call the *Costellatori*. A beacon is not a lamp: it is a held breath of space, a fixed point that lets a
ship jump between systems without drifting into the dark. When the Costellatori vanished, the beacons went out
one by one. What was a network became islands. Trade became smuggling, borders became superstition.

- **Gilda (the Guild)** — the merchant houses that survived the Dimming by owning the few routes still lit.
  Pragmatic, rich, lawful when the law pays. Their convoys are the arteries of the sector; their patrols keep
  them flowing. They want the beacons relit — and *owned*.
- **Custodi (the Keepers)** — a monastic order that has tended the dead beacons for nine centuries without
  understanding them. They read the Costellatori script badly and pray to it well. They protect relics and
  pay for them; they distrust anyone who sells them.
- **Relitti (the Wrecks)** — salvagers, pirates, and the families who live in the hulks of the Dimming. Not
  evil: free. They strip what the Guild abandons, raid what the Guild hoards, and know paths no chart shows.
- **Eco (the Echo)** — something answers when a beacon is relit. First as interference, then as voices in the
  comms, then as ships made of the same lattice as the beacons. The Echo is the Costellatori's immune system,
  or their ghost, or their jailer. It is not hostile at first. It becomes hostile to whoever relights too much,
  too fast, for the wrong reasons.

The player is an independent pilot with an old courier and a debt. The campaign arc is **relighting the network
sector by sector** (the save's `beacon_lit` per sector and `sector` depth): every relit beacon opens a route,
shifts the factions, and wakes the Echo a little more. Deeper sectors are older, stranger, and lit by fewer
stars. The truth about the Costellatori is in the ruins on the worlds — and in what the Echo is trying to say.

Tone: wonder first, melancholy second, danger third. Names and text in five languages (it/en/es/fr/de).

## The game loop (web)
**Fly → discover → fight or trade → relight.**
- **System space**: arrive by jump at a beacon (or at the system edge if unlit). The star, 2–7 planets
  (procedural), asteroid belts, a station per inhabited system, points of interest (wrecks, signal sources,
  Echo anomalies). Free flight with an optional cruise drive between them.
- **Stations**: dock (short automated approach) → the hub: trade (existing economy), outfit (shop), repair,
  missions (existing generator + flavor), faction standing, codex. Stations look like their faction.
- **Missions** (existing types, staged in 3D): patrol (sweep waypoints, ambushes), bounty (named ace + wing),
  escort (protect a convoy through an attack), defend (hold a station/beacon against waves, a capital ship).
- **Planets**: descend from orbit through the atmosphere (a transition, not a loading screen) to a surface
  zone: terrain, biome, weather, flora, ruins of the Costellatori with relics and lore, salvage, surface
  threats. Take off again to orbit.
- **Beacons**: in each sector a few beacons can be relit (`beaconsPerSector`): reach it, defend it while it
  charges (an Echo response scales with how many you have lit), and the route opens.
- **Jump**: a hyperspace sequence to another system in range (`jump_range`, fuel) or, from a lit beacon, to
  the next sector.

## Combat
Arcade-sim, Star Wars Squadrons as the feel reference (not a copy).
- **Flight**: pitch/yaw/roll, throttle with a sweet spot for best turning, boost, drift. Mouse+keyboard,
  gamepad (existing `gamepad.js`), touch fallback.
- **Power**: three pips between engines / weapons / shields (keys 1/2/3, reset 4). Overcharge trades.
- **Weapons**: lasers (bolts with travel time, convergence, heat), missiles (lock time, evadable), torpedoes
  vs capital ships, countermeasures (flares). Upgrades from the shop map to the save (`weapon`, `shield_max`,
  `sensors`, `hull_max`).
- **Shields** front/back balance; hull damage shows (smoke, sparks, a flickering HUD).
- **Targeting**: cycle targets, target attacker, target ahead; lead pip; off-screen indicators.
- **Feel**: hit-stop on kills, screen shake scaled by impact, shield flare at the hit point, debris that keeps
  the ship's momentum, a shockwave ring, cockpit glass shake, audio ducking on big explosions.

### Enemy roster (each faction distinct in silhouette, colour and behaviour)
| Faction | Units | Behaviour |
|---|---|---|
| Gilda | Lancer (interceptor), Bastion (heavy fighter), Warden gunship (corvette), Convoy hauler | Disciplined wings, formation flying, shield discipline; call reinforcements |
| Custodi | Votive (light fighter), Censer (bomber, slow torpedoes), Reliquary barge (capital with point defence) | Defensive, protect relics and their barge; never pursue far |
| Relitti | Scrapwing (cheap swarm fighter), Harpoon (tractor tether), Gutter (boarding craft), Hulk (converted freighter with turrets) | Ambushes from asteroids and wrecks, hit-and-run, tether-and-board |
| Eco | Shard (drone swarm), Lattice (morphing interceptor, phases), Choir (support that shields others), Warden of the Dark (boss: a beacon-sized lattice entity with weak points) | Alien: patterns, swarm logic, phase shifts, attacks that read as music |
| Surface | Sentinel drones (Echo), feral fauna (flight-capable), Relitti ground skiffs | Short, readable threats during exploration |

Named aces (bounty targets) get a generated name/portrait and one signature trick.

## Procedural universe (infinite, deterministic)
Everything derives from `(seed, sector, system, …)` with the SHARED generator (`constellations-gen.js`, byte-
identical to the firmware for the numeric layer). The web adds *visual* layers from dedicated hash domains (the
`DOM.FLAVOR` pattern) so they never perturb the numbers both sides agree on.
- **Galaxy map**: sectors laid out on a spiral, each a cluster of 10 systems; the map shows lit routes.
- **Star**: class (spectral colour, size, flares), corona shader, lens glare.
- **Planets**: type (rocky, desert, ocean, ice, jungle, volcanic, gas giant, crystal/Echo-touched), generated
  in shaders from noise: height, biome colour ramps, clouds layer, atmosphere rim (scattering approximation),
  night-side city lights on inhabited worlds, rings, moons. No downloaded planet textures.
- **Space**: procedural nebula skybox per system (fbm noise, palette from the star), dense starfield, dust.
- **Stations**: assembled from a kit of procedural modules by faction (rings, spires, docks, lights).
- **Surfaces**: a local terrain patch around the landing zone, chunked LOD heightfield from layered noise,
  biome materials (triplanar, procedural), instanced flora/rocks, sky and fog matched to the atmosphere,
  ruins placed by the generator.

## Presentation and assets
Budget (user decision): **35–40 MB total**, at most **4 music tracks**, audio compressed. Loaded progressively:
the game is playable after the code + first track; images stream in the background.

| Asset | Source | Format | Budget |
|---|---|---|---|
| Code (Three.js r160 already shipped) | repo | ES modules, .gz twins | ~1.5 MB |
| Ship models | procedural kit + existing GLB | geometry in code | ~0.5 MB |
| Illustrations (title, lore codex, faction emblems, station interiors/exteriors, briefings, battles, planet/ruin vistas, ships) | **Qwen-Image 2.1 base** local (ComfyUI), 20 steps | AVIF q54, 1280x720 / 1024x576, emblems 384x384 | 54 images, ~4 MB |
| Portraits (the recurring cast of `tools/costellazioni-assets/lore.md`) | Qwen-Image 2.1 base | AVIF q62, 384x384 | 12, ~0.2 MB |
| Music: 1 main theme, 2 exploration (calm / deep), 1 combat | **ACE-Step 1.5** local (`music-spec.json`) | Ogg Opus 64 kbps VBR stereo | 4 tracks, ~6 MB |
| SFX | tools/sfx-gen + layered procedural | Opus / WAV small | ~1.5 MB |

Art direction: painterly sci-fi concept art with a consistent palette per faction (Gilda: brass/ivory/blue;
Custodi: verdigris/candle gold/white stone; Relitti: rust/sodium orange/oil black; Eco: cyan-violet lattice
light on black), cinematic light, no text in images. Prompts and seeds are versioned in
`tools/costellazioni-assets/` so every image can be regenerated:
- `prompts.json` (prompt, seed, size, style keys) -> `gen-images.mjs` (ComfyUI API; the **base** model at 20 steps,
  cfg 1, euler/simple — the turbo distillate over-sharpens skin and texture past its native 8 steps) -> `raw/`
  (git-ignored). The cast portraits are generated first; every scene with a cast member passes those portraits as
  reference images (`refs`, written `<image1>`... in the prompt), so faces, clothes and implants stay the same.
- `build-assets.py` -> AVIF (about 27% smaller than WebP q72 at the same look) with a content hash in the file name,
  plus `assets/manifest.json` (size, kind, load priority, two-colour placeholder gradient).
- `games/stelle/assets.js` is the only loader: IndexedDB blob store keyed by the hashed name (see caching below). Licence: Qwen-Image 2.1 weights are under
the Qwen licence (research/non-commercial terms flagged by some guides) — NucleoOS is non-commercial; the
generated images are credited in the codex and the asset manifest.

Music (ACE-Step, see the `ace-step-songwriting` skill): **Theme "Costellazioni"** (wide synth-orchestral,
the beacon motif), **"Islands of Light"** (calm exploration), **"The Dimming"** (deep-sector, sparse,
melancholic), **"Lattice Storm"** (combat: driving percussion, brass-like synths). Instrumental, loopable
edges, mastered to the same loudness. The beacon motif recurs in all four.

## Technical architecture (web)
- Three.js r160 (vendored), WebGL2, no build step: plain ES modules under `apps/games/www/games/stelle/…`
  (split by concern: flight, combat, ai, galaxy, planet shaders, surface, ui, audio, assets loader).
  Every shipped file gets its `.gz` twin (`npm run gz:check`).
- Post-processing: bloom (vendored UnrealBloomPass), tone mapping ACES, optional FXAA; quality tiers auto
  (integrated GPU / phone vs discrete) with a manual override.
- Performance: instancing for debris/asteroids/flora, LOD for planets/terrain, pooled particles, fixed-step
  sim at 60 Hz decoupled from render, no per-frame allocation.
- Assets: a manifest with sizes and priorities; a loader that streams after first paint.
- **Downloaded once, never again on reload** (user requirement). The device serves the web OS over plain
  `http://`, where browsers allow neither service workers nor the Cache API — but IndexedDB works:
  - every heavy asset (images, music, models) has a content-hash file name from the manifest and is stored in an
    IndexedDB blob store keyed by that hash; a reload reads it from there, an update downloads only the
    hashes that changed, and stale entries are pruned against the manifest;
  - the device already sends `Cache-Control: public, max-age=604800` for images/audio and an ETag
    (304 revalidation, no body) for code, so the HTTP cache covers the first week and the code is never
    re-sent unchanged;
  - the game code ships as a few bundled modules (not dozens), so a reload costs a handful of 304s.
- Save: `constellations-save.js` contract unchanged; new web-only progress (codex entries, surface discoveries)
  lives in a separate web save next to it (`/sd/data/costellazioni/web.json`), never in the shared struct.
- Tests: `tools/web-e2e/` (headless Chrome against `tools/serve-shell.mjs`): boot, fly, fight, dock, trade,
  jump, descend, save round-trip, five languages, no console errors, frame-time budget.

## Native (Cardputer) — kept in step
The native game keeps the shared numeric layer and save. It gets the same lore names and faction identities,
a richer 8bpp look (dithered nebulae, lit planets, outlined ships, juicy combat) and the same mission types —
see `app_constellations.cpp` and the native harness `tools/native-host/games/stelle.cpp`.

What the native game shows today:
- **Combat**: a dithered nebula in the system faction's colours, parallax stars and boost streaks, the system's lit
  planet and (when there is one) its beacon spire, lit or dark. Each faction has its own outlined ship kit
  (Gilda ivory/blue needles and H-heavies, Relitti lopsided rust, Custodi white-gold with a halo, Eco cyan
  lattice). Enemies telegraph before firing (closing red ring, engine blaze before a dive) and their shots
  leave the shooter; the locked target has a lead pip (+50% damage, a pip hit cancels a charged shot).
  Explosions are fireball + smoke + shock ring + debris; shield ripple, damage-direction arc, a small radar.
- **Aces** are the cast (Dax Oren, Sister Vigil, Scarlet Gutter, One-Eye Bram): weave, charge, burst, evade;
  the Keeper raises a shield, the Echo ace phases out. Radio lines name them in the HUD.
- **Ambushes**: Echo ships in Echo space; a faction at reputation -25 or worse sends its own patrols. This is
  native-only (no save field, no generator change) — the web game may choose differently.
- **HUD**: icon bars, wave/kill counters, an objective line that becomes the ace's radio line and health bar.
- **Map**: beacons are crystal spires; a gold thread with a running light joins two beacons only when both are
  lit, otherwise it is dim and broken; the jump plays as a tunnel with the destination name.
- **Stations and title**: faction-coloured headers with the emblem; contracts come from Vesna Ardali,
  Mother Ilse or Mara "Rustmother"; the title is a painted 8bpp scene.
Budget: about 74 KB flash, 749 B static RAM, deepest frame 768 B (`npm run games:ram`).

## Milestones
1. **M1 Space combat** — new flight model, power, weapons, shields, AI squads, the roster above (Gilda/Relitti
   first), explosions and feel, procedural ship kit, system space with star/planets/nebula, missions staged in
   3D. Replaces the rail dogfight.
2. **M2 Galaxy** — galaxy map, jumps and hyperspace, procedural planets (shaders) with atmosphere/rings/moons,
   stations by faction, docking into the hub, beacons relighting with Echo response.
3. **M3 Worlds** — orbital descent, surface patch terrain + biomes + flora, ruins and relics, surface threats,
   take-off.
4. **M4 Story and sound** — codex and lore fragments, contacts and aces with portraits, the four tracks with a
   dynamic music system, Eco and Custodi rosters, the boss, polish.

Each milestone ends green in the e2e suite, under the asset budget, at 60 fps on a mid laptop.
