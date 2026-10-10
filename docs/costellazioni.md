# Costellazioni — design

One campaign, two windows on it. On the **Cardputer** (native, `app_constellations.cpp`) Costellazioni is a
compact space trader/fighter you play anywhere. In the **web OS** (Game Center, `apps/games/www/games/
constellations*.js`) the same run opens up into an immersive 3D game: an endless procedural galaxy to fly
through, planets to descend onto, and space battles that feel like a modern Star Wars dogfight. Progress made
in one is there in the other — the save is shared (`/sd/data/costellazioni/save.bin`, JSON at
`/api/game/costellazioni/save`, see `constellations-save.js`).

Status: v1 web (rail dogfight + hub) and native exist. This document is the target for the rebuild started
2026-10-10. Milestones at the end. M1 (space combat) and M2 (galaxy, travel, docking, relight) are in the web game;
see "What the web game shows today (M2)" below.

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

## What the web game shows today (M2)
- **Hub** (docked): six screens — Bridge (hall illustration cropped on its focal point, faction emblem, the
  station's contact), 3D galaxy map, Market, Shipyard, Missions, Codex. **Launch** undocks into free flight.
- **Galaxy map** (`stelle/galaxy.js`): a spiral galaxy; the current sector's ten systems in 3D (shared x/y, web-only
  height on hash domain `VDOM.GALAXY`), star colour by class, faction territories, beacons as crystal spires, golden
  threads only between two lit beacons (dim, broken otherwise), routes in jump range, the jump-range ring, charted /
  uncharted from the web save, neighbouring sectors along the arm (past ones keep the threads you lit). Drag, wheel,
  pinch; click picks a system. A hub jump plays as a drive burn, the tunnel and an arrival at the new station.
- **Free flight** (`sim.js` kind `explore`): points of interest (station, beacon, planets, moons, the field); `N`
  cycles the destination, the cruise drive scales with the distance (up to ~40 km/s) and drops out at the target;
  planets sit at their real distance (40–190 km, far camera rides with the player) and are surveyed from orbit
  (codex: worlds). `L` requests docking (clearance, a lane along the station's `dock` normal, a tractor slide-in,
  fade) or seats the relic at a dark beacon; `P` opens the galaxy map over the paused flight, `K` spools the jump
  drive (mass lock, 4.2 s), the tunnel, then an arrival fly-in in the target system. Random encounters: raiders, or
  the Echo's drones in Echo space.
- **Relight**: carry a relic to the beacon; it charges for about a minute while raiders come for the crystal and the
  Keepers' watch (a Censer and two Votives) holds near the spire; the light bursts out (pillar, golden threads to the
  other lit beacons) and the shared save gets the relight (bit, relic, 300 cr, Keepers +12). The Echo answers the
  first beacon of a sector by measuring you (lattice ships circle, the Voice speaks, they leave) and every further one
  by fighting (Lattice interceptors that phase out of your fire and blink sideways, a Choir that re-shields its kin
  through links and sings chords of slow bolts, Shards).
- **Rosters added**: Custodi Censer (stand-off bomber, slow homing torpedoes you can shoot down) and Reliquary barge
  (capital, point defence, bell-tower bridge); Echo Lattice and Choir.
- **Codex** (`constellations-codex.js`): 57 entries in six categories (lore, factions, places, worlds, ships, contacts)
  with the lore / station / world illustrations and the cast portraits; ship dossiers turn the live model in the
  hangar. Unlocked by play (systems visited, docking, surveys, kills, the Voice, relights, sector depth).
- **Web save** (`constellations-web.js`, `/sd/data/costellazioni/web.json` + a localStorage mirror, keyed by the run
  seed): visited systems per sector, relit beacons (kept after the sector advances), surveys, codex unlocks. The
  shared save contract and the generator output are unchanged.

## What the web game shows today (M3 — worlds)
Every landable world of every system is a whole planet you can fly down to, with no loading screen.
New modules under `apps/games/www/games/stelle/`: `noise.js`, `planet.js`, `chunk.js`, `terrain-worker.js`,
`terrain.js`, `atmo.js`, `props.js`, `surface.js` (about 170 KB of source, 57 KB gzipped).

- **The worlds move** (`world.js`): universe time is `Date.now()/1000 − 1767225600` (tests pin it with `__czClock`).
  Planets orbit their star in 3–9 days and spin; moons orbit their planet; the home planet keeps its place (the
  station's frame). Inside a world's sphere of influence its frame carries everything (ships, bolts, missiles,
  flares, pickups in the sim; particles, debris and trails in the renderer), so a landed ship stays landed.
- **Descent** (`surface.js`, one state machine in the 60 Hz sim): `SPACE → DESCENT → ENTRY → FLIGHT → LANDING →
  LANDED → TAKEOFF → FLIGHT → ASCENT → SPACE`. From orbit `L` ("Descend to …", "Dive into the clouds of …" on a gas
  giant) starts an automatic dive: the cruise drops out, the ship brakes, dives, heats (hull heat bar, a plasma
  sheath, embers, the hull glows), the air bleeds the speed, and below the clouds "you have the helm". Flying in
  fast by hand is an entry too (drag and heat, yours to steer). The cruise drive is refused low in the air and the
  jump drive and docking anywhere in it. `L` again lands (gear, a flat spot picked off any ruin's own ground, dust
  or spray), takes off, or leaves the atmosphere (an automatic climb to orbit).
- **Terrain**: a cube-sphere quadtree. Each chunk is 33×33 vertices plus skirts (1 221), built in module workers
  from pooled typed arrays (ping-pong transfer, no per-frame allocation) and kept in an LRU slot pool. The lattice
  is an exact binary fraction of each face, so neighbouring chunks compute the same height at the same vertex
  (bit-identical seams, checked by the host test); CDLOD geomorphing hides level changes; frustum and horizon
  culling. Height comes from an integer-hash gradient noise that is bit-identical in JS and GLSL (`noise.js`), on
  web-only hash domains (`VDOM.SURF`, `FLORA`, `SITE`, `WEATHER`, `ORBIT`), seeded from the world; the orbital view
  bakes the same macro shape and colours, so the planet seen from orbit is the one you land on. The shared
  generator output is unchanged (the host test checks its digest).
- **Biomes** (one rule set per type in `planet.js`, one shading branch per type in `terrain.js`): rocky — mesas,
  terraces and canyons in banded red stone, snow on the highest peaks; desert — dune seas, wind-cut rock, salt
  flats; ocean — islands with beaches, grass and dark cliffs, water with waves, sky reflection, sun glint and
  shore foam; ice — packed snow, blue ice walls, crevasses, a frozen sea; jungle — river valleys, moss and mud,
  mossy rock; volcanic — basalt, ash, cones, glowing fissures and a lava sea; crystal — violet ground, glassy
  facets, cyan veins and glowing lakes; gas giant — no ground: three cloud decks coloured by its own bands,
  the lowest one a pressure floor ("refuses gracefully": no landing, the pressure warning pushes you up). Detail
  is triplanar procedural texture at four scales with slope/height blending and per-pixel relief; cloud shadows,
  the ship's shadow, a headlight at night.
- **Sky and weather** (`atmo.js`): single scattering (Rayleigh + Mie with an absorbing dust term, soft planet shadow)
  drawn as a full-screen pass, with aerial perspective on terrain, flora and ship hulls; the sun's colour, the
  sky light on the hulls, the exposure and the stars (hidden by day) all come from the same model (a JS twin of the
  shader). A cloud shell plus billboard cloud puffs you fly through; weather per type (dust, rain, snow, ash, motes,
  wind) with storms.
- **Flora and props** (`props.js`): instanced kinds per biome — hoodoos and shrubs; fossil ribs and rock spires;
  palms and coral; ice spires and frost; spiral trees with glowing fruit, ferns and glow pods; basalt columns, dead
  trees and embers; crystal shards and lattices — scattered deterministically per ~300 m cell, faded with
  distance (big kinds seen further), density and range by quality tier.
- **Sites** (placed per world from its seed): Costellatori ruins (the Gate of Threads, the Silent Archive, the Last
  Observatory on the highest ground), relic shrines, crashed ships, faction outposts. A passive scanner ping every
  8 s in the air shows signals within 2.6 km; `Y` pings 7 km; `N` cycles the signals. A site is found by flying low
  over it (under 240 m, within 320 m, 2.4 s) or landing near it: a banner, a radio line, credits and reputation,
  the codex (the ruin entries and the Costellatori; the Archive brings the novice; the world type's entry on
  arrival). Landing beside a found shrine (or a ruin that holds one) recovers a relic (cargo, or 220 cr when the
  hold is full) and the Echo's sentinels rise; raiders come for a guarded wreck.
- **Surface flight**: ground effect, speed lines, a terrain assist (`U`: levels off, pulls up before the ground),
  terrain collision, bolts strike the ground, the AI keeps off it.
- **Planet HUD** (`hud.js`): pitch ladder and horizon, flight-path marker, heading tape (planet north), altitude
  above ground and sea, climb, ground speed, gear and assist, site markers with distance (signals until found),
  the ping ring, survey and recovery bars, `PULL UP`, hull heat, pressure, landing line, context prompts. All text
  at least 13 px; on phones the altitude panel stacks above the prompt and the speed panel, clear of the touch
  buttons, the scanner has its own `SCN` button and the missile / flare counts ride on their buttons. Gamepad: R3
  context action, L3 scan in the air; Back/B/Start close the jump map. Five languages.
- **Web save**: `found` and `looted` bitmasks per `sector:system:world` in `/sd/data/costellazioni/web.json`
  (never in the shared struct).
- **Quality tiers** (terrain LOD distance `K`, finest vertex spacing, chunk pool, flora density and range, workers,
  cloud puffs, weather particles): low 2.1 / 4.2 m / 300 / 0.32 × 380 m / 1 / 48 / 700; medium 2.5 / 2.6 m / 420 /
  0.62 × 650 m / 2 / 90 / 1300; high 2.9 / 1.7 m / 540 / 1.0 × 950 m / 2 / 140 / 2200. Big flora kinds switch to a
  light far mesh beyond ~320 m (the spiral tree: 1 031 → 288 triangles) and thin out with distance; the instance caps
  fill from the camera out. The dynamic resolution steps down past 18 ms a frame (to 60 % of the tier's scale) and
  back up under 12.5 ms.
- **Measured** (1920×1080, 100 m over the jungle in rain, `E2E_GPU=1`, after the dynamic resolution settles;
  frame interval avg / p95, then the frame's cost with the GPU waited for): Intel Arc 140T iGPU — auto (= medium,
  scale 0.78) 15.5 / 16.9 ms, 16.5 ms; low 8.4 / 9.2 ms, 10.1 ms; medium (0.92) 16.0 / 16.8 ms, 18.0 ms; high (0.90)
  15.2 / 16.0 ms, 18.9 ms. RTX 5070 Laptop — every tier at the 120 Hz cap (8.3 / 8.5 ms); cost auto (= high, 1.35)
  9.6 ms, high 9.7 ms, medium 6.8 ms, low 4.9 ms.
- **Tests**: `tools/games-host/test-costellazioni-worlds.mjs` (noise JS↔GLSL twin, terrain determinism and a digest
  of the generated worlds — update `GOLDEN` deliberately when a surface rule changes —, chunk seams, POI
  placement, flora, atmosphere, discovery persistence, the descent / landing / take-off state machine);
  `tools/web-e2e/costellazioni.e2e.mjs` (a world visited end to end with a discovery and the web save, a phone
  with touch emulation, the gamepad and the jump map, `E2E_GPU=1` frame times over a world per tier on the
  discrete and the integrated GPU — `launchBrowser({ gpu: 'low-power' })`, `__czSync` for the GPU-waited cost);
  `tools/web-e2e/costellazioni-shots.mjs --scenes worlds,entry,ruin,groundfight,worldhud --gpu` for review shots
  (dev hooks `__cz.dev.overWorld(world, alt, sunEl, az, v)` and `__cz.dev.nearSite(world, kinds, dist, alt, v,
  sunEl)`, flags `__czAutoLand`, `__czAutoSites`, `__czClock`).

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
   take-off. Shipped on the web: see "What the web game shows today (M3 — worlds)".
4. **M4 Story and sound** — codex and lore fragments, contacts and aces with portraits, the four tracks with a
   dynamic music system, Eco and Custodi rosters, the boss, polish.

Each milestone ends green in the e2e suite, under the asset budget, at 60 fps on a mid laptop.
