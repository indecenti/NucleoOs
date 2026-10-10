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
- Post-processing (`stelle/post.js`, medium / high): the far and near passes into one multisampled half-float target
  (resolved once), a dual-filter bloom pyramid from half resolution down to 1/32, sun shafts, then one final pass
  (bloom, grade, ACES tone mapping, sRGB) straight to the canvas — about a third of the old EffectComposer chain on
  an integrated GPU. Low draws straight to the canvas. Quality tiers auto (integrated GPU / phone vs discrete) with a
  manual override.
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
`terrain.js`, `matgen.js`, `atmo.js`, `props.js`, `surface.js`, `post.js`.

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
  (bit-identical seams, checked by the host test); CDLOD geomorphing hides level changes; horizon culling, and the
  split distance of what lies outside the view counts double (coarser behind you, refined in a few frames when you
  turn) so the pool holds the near ground at its finest. A drawn-over ancestor stays cached but is recycled first.
  Height comes from an integer-hash gradient noise that is bit-identical in JS and GLSL (`noise.js`), on web-only
  hash domains (`VDOM.SURF`, `FLORA`, `SITE`, `WEATHER`, `ORBIT`), seeded from the world; the orbital view bakes the
  same macro shape and colours, so the planet seen from orbit is the one you land on. Everything finer than the
  macro field is JS-only and **metric** (wavelengths in metres, whatever the world's size): a domain warp, an
  "eroded" fractal (`efbm`: each octave damped by the slope the coarser ones built — wide soft valleys, crisp
  crests; `noise3d` gives the analytic gradient) and a cellular noise (`cell3`) for buttes, cones, karst towers and
  crystal mesas. Each chunk vertex also carries `aEx` (four bytes): open sky ↔ crevice, wetness (on volcanic worlds:
  the lava's light), a second type mask and convexity. The shared generator output is unchanged (the host test
  checks its digest).
  Water worlds (and the crystal lakes) keep their **seabed** in the mesh — only lava and frozen seas are flattened to
  sea level — and a fourth vertex attribute, `aDun` (the dune sea's unwrapped wave phase and its height), lets the
  shader redraw every dune brink per pixel at any distance (below).
- **Biomes** (one rule set per type in `planet.js`, one material table and shading branch per type in `terrain.js`):
  - rocky — three levels (basin, plateau, high mesa) with ~150 m escarpments and talus aprons, ragged buttes standing
    in the basins, canyons cut in two steps with sand floors, ridged ranges; banded sandstone cliffs (some bands pale
    caliche, some purple-grey shale, desert varnish streaking down the faces), red dust and scree, sage scrub in
    clumps, boulder fields, snow on the highest peaks;
  - desert — dune seas across a wind axis fixed per world: a long windward rise that still climbs at a **sharp brink**,
    then the slip face dropping steeply and easing into the trough (`duneProf`, crests 10–36 m that meander, a second
    small set at an angle). The mesh rounds a brink off between its vertices, so the terrain shader takes out the slope
    the mesh already shows (averaged over its own vertex spacing) and puts the analytic one back from `aDun`: a crisp
    lit side and shaded side from 50 m to the horizon. Fresh slip faces a shade deeper and warmer, pale brinks,
    coarser grey interdunes; wind ripples (0.85 m) and big ripples (6.5 m, readable from the air) across the wind on the
    windward slopes and flats, none on the slip faces; warm sand in three macro shades; wind-cut mesas, pale salt
    playas, sandstone outcrops, boulder fields, fossil skeletons and wrecks half buried; dust devils by day and a heat
    shimmer low over the hot ground;
  - ocean — islands with flat sand beaches, eroded hills and, on some coasts, sea cliffs; shallow shelves; under the
    water a seabed you can see and dive to: eroded reefs and ridges on the shelf, sand waves across the shallows,
    seamounts further out (never breaking the surface);
  - ice — glacier shelves stepping down in sheer blue ice walls, sharp ridged ranges, long crevasses across the
    ice's flow in fields; wind-packed snow, scoured blue ice, glitter in the sun, a frozen sea;
  - jungle — eroded hills, river valleys with wet mud banks, and in places karst towers (pale limestone, grass caps)
    rising out of the canopy;
  - volcanic — basalt shelves, cones with craters (some still glowing), lava rivers down to a lava sea under a
    drifting crust, ash drifts; the rock next to the lava takes its light;
  - crystal — faceted terraces, knife ridges, crystal mesas; violet facets with thin cyan veins that pulse;
  - gas giant — no ground: three cloud decks coloured by its own bands, the lowest one a pressure floor ("refuses
    gracefully": no landing, the pressure warning pushes you up), lightning in the decks.
  A take-off that faces a cliff rises straight up until the way is clear.
- **Ground materials** (`matgen.js`): ten tileable layers generated once per session on the GPU into a texture array
  (512² on medium / high, 256² on low; normal xy, height, tone; mipmapped; one small program per layer plus a normal
  pass, compiled off the main thread, one layer a frame; a neutral layer stands in until then): rock, sand ripples,
  grass and moss, gravel, snow with sastrugi, ash crust, ice fractures, crystal facets, dirt, weathered sandstone. Each
  world paints four of them with its own palette (two colours per slot, mixed by the tone): a flat ground, a second
  ground, the cliff rock and a special one (canyon sand, salt, beach, blue ice walls, karst, lava crust, facets), chosen by
  slope, elevation, moisture and the worker's masks and **height-blended** (sand fills the cracks, stones poke
  through). Biplanar projection (the two dominant planes of the normal) at two scales (a wide one always, the near
  one fading out by ~240 m) plus a 410 m patch field: no visible tiling from the air. Per-pixel detail normals,
  cavity and valley occlusion, strata bands by elevation on the cliffs, wet ground darkened and glossy, snow on the
  peaks, emissive lava cracks and crystal veins. Far away the ground blends into the orbital colour ramp.
  **From the air** (70–300 m and up), three things keep the land from reading as a tile: macro grounds — every world
  has three tints (rocky red / purple-grey / pale ochre, desert gold / orange / pale, jungle yellow-green / blue-green
  / ochre…) mixed by a tileable **3D noise volume** (64³, generated once on the GPU in `matgen.js`, shared with the
  clouds; no projection plane, no seam) over 0.6–2.3 km; **clumps** at 18–38 m per world (shrubs and growth, stone
  clusters, scoured hollows) with their own colour and an emboss toward the sun (a lit side, a shaded side) — the
  ground cover grows denser and taller in them; boulders that stay visible from altitude (below). The **seabed** has
  its own materials: sand in the shallows and between the reefs, rock on the reefs and walls, sea-grass meadows a few
  metres down, silt in the deep, lit by a sun that dims with the depth and is focused into drifting **caustics**.
- **Light**: the sun through the air and the cloud shadows, plus **two shadow-map cascades** that follow the camera, side
  by side in one depth atlas: the sharp one over the nearest hundred-odd metres (every caster; medium 1024² over
  ±150 m, high 2048² over ±230 m, drawn every frame) and a **wide one** out to a kilometre or two (terrain, sites, ship
  hulls and, on high, the far meshes of the trees; medium 1024² over ±1.1 km, high 2048² over ±1.7 km, drawn every
  other frame — it lives in world space, so a frame-old map still lines up): mesas and ridges throw their shadows across
  the basins, valleys lie in shade. Texel-snapped, normal-offset PCF in the sharp one, a single hardware 2×2 compare in
  the wide one (its texels are metres wide), a soft hand-over at the sharp one's border; the compares use explicit LOD
  (the D3D compiler behind ANGLE flattens branches round implicit-derivative fetches). One atlas, not two textures,
  and it is cleared once at creation: a shadow sampler bound to a depth texture that was never a render target has no
  storage behind it and every draw that declares it is silently dropped (the ground vanished at night in early tests
  of this pass — and a wrong "cheap" perf number came from exactly that). The hull material reads the same maps for
  the sun light, so trees, ruins and the nacelles shade the ship. A sky-coloured hemisphere — the zenith on what faces
  up, the bright horizon band mostly on what faces sideways — with the ground's own bounce for what faces down; aerial
  perspective and a low **height fog** (valley mist, dust, ash; denser at dawn, dusk and in rain) lit by the same sky;
  the sky itself is a **sky-view table** (192×108, the UE4 parameterisation, rendered each frame from the same
  scattering model) that the sky pass and the water read in one fetch. Sun shafts: a radial blur of the bloom's bright
  level (clouds included) toward the sun when it is on screen in the air.
- **Water** (a separate pass over the seabed on water worlds and crystal lakes): the chunks that hold sea are drawn a
  second time with the water material, projected onto the sea-level sphere (their odd vertices sink onto a coarser
  neighbour's chords as the geomorph runs out, so no crack shows from either side; skirts hang only when seen from
  above). Four Gerstner waves along the world's wind (the sea state from its weather), displaced near the camera and
  shaded with analytic normals plus scrolling ripples, both in a tangent frame anchored near the camera (re-anchored
  with a cross-fade every 2 km); each wave fades out where it would be smaller than a few pixels. **From above** the
  surface blends over the seabed by its Fresnel term — premultiplied: what it reflects (the sky table, the cloud deck,
  the sun's glitter core, sheen and sparkles), foam bands up the shore and whitecaps in a blow, light through the
  crests — while the seabed shader has already put the water between itself and the eye (absorption per metre, red
  first, along the refracted path, and the water's own scattered colour lit by the sun and the sky): turquoise
  shallows over sand and coral, then the deep, which turns opaque past ~60 m (chunks wholly that deep skip the seabed
  draw). **Under the surface** (you can dive: below) everything takes the same water — terrain, flora, the surface
  itself — by distance, darker the deeper the camera; the surface seen from below is **Snell's window**: the sky packed
  into a bright disc overhead (radiance ×n²), the edge rippling with the swell, a mirror of the deep outside it.
  Rain rings land on the water over a seabed. The lava sea glows only in the open cracks of its crust; the frozen sea
  shows fractures and a gloss.
- **Ground cover**: on the finest chunks near the camera (medium 70 m, high 115 m), one tuft per terrain quad built on
  the GPU straight from the chunk's own vertex buffers (no CPU work, nothing streamed): 6–9 blades placed, sized and
  coloured by hash and by the masks (lush grass, dry tufts, frost needles, ash with cinders, glowing crystal needles),
  bent by a gust field that travels over the ground, shrunk to nothing at the range; in rain the same buffers feed
  the splash rings.
  A **far ring** of fewer, broader, taller tufts (3 blades on medium, 4 on high) grows in on the next coarser chunks
  where the near ring shrinks away and carries the cover to 150 m (medium) / 240 m (high); both follow the clumps.
- **Sky and weather** (`atmo.js`): single scattering (Rayleigh + Mie with an absorbing dust term, soft planet shadow),
  with aerial perspective on terrain, flora and ship hulls; the sun's colour, the sky light on the hulls, the
  exposure and the stars (hidden by day) all come from the same model (a JS twin of the shader). **Volumetric clouds**
  on medium / high (`terrain.js` VOL_FRAG, drawn by the post chain at half resolution over the scene's resolved
  depth): a slab from the deck's base to its tops (650–1500 m thick), its coverage the orbital bake's (the clouds you
  saw from space), its shape a Perlin-Worley 3D noise eroded by a finer Worley one — flat bases, tops that climb where
  the cover is thick —, marched front to back with long strides through clear air, short ones in cloud and short ones
  that grow with the distance when the camera is inside the slab (you fly through them on the way down), a static
  interleaved-gradient jitter per pixel and per stride (no flicker from frame to frame, no slicing bands), lit by the
  sun through up to three taps toward it (shadowed bellies, lit crowns, a silver lining against the light; a share of
  multiple scattering that never darkens fully) and by the sky above and the ground below; the far clouds fade into
  the air by 24 km. A 3×3 depth-aware tent smooths the half-resolution result, a depth-aware 4-tap upsample brings it
  to full resolution without halos round ridges and hulls, and the bloom sees it. Low keeps the shell deck (lit as a
  volume would be) and its billboard puffs. A thin high veil of streaks, thinned edge-on; weather per type (dust, rain,
  snow, ash, motes, wind)
  with storms: rain rings on the ground, **lightning** in storms and on gas giants (a jagged bolt, the flash in the
  sky, the clouds and the ground, thunder by distance), **aurora** curtains on ice worlds at night.
  **Dust devils** on the sand and rock worlds by day (2 / 4 / 6 by tier): a funnel that widens as it climbs, leans
  with the wind and sways, a skirt of dust at its foot and streaks spiralling up it; each lives about a minute and
  wanders downwind, its foot following the ground. **Heat shimmer** (post chain, final pass): over hot ground by day
  (desert, volcanic, a little on rocky worlds) the view of whatever lies a few hundred metres off wobbles — never the
  sky, never the near ground (it reads the depth).
- **Flora and props** (`props.js`): instanced kinds per biome, now several species each — rocky: hoodoos, gnarled
  junipers, boulders, shrubs; desert: fossil ribs, rock spires, columnar cacti with glowing buds, boulders; ocean:
  palms, mangroves on stilt roots, blue coral spires with glowing tips, kelp at the water line, coral, bushes;
  ice: ice spires, clusters of hexagonal ice crystals, frost, snow-capped boulders; jungle: umbrella canopy trees
  (18–32 m, glowing pods), spiral trees, tree ferns, ferns, bushes, glow pods, bioluminescent mushrooms; volcanic:
  basalt columns, ash-covered dead trees with ember cracks, obsidian shards, embers; crystal: crystal trees,
  shards, lattices, glowing geodes. Scattered deterministically per ~300 m cell; size and hue vary per plant; what
  faces the sky takes the world's cover (snow, moss, ash, dust); occlusion toward the root, light through the
  leaves, the shadow map. Small kinds end at 60–70 % of the range; big kinds switch to a light far mesh (150 / 220
  / 300 m by tier) and, beyond the flora range, become **impostor cards** (painted once per world from the far mesh
  into an atlas) out to 3.4 km (medium) / 5 km (high), so the forests reach the horizon.
  This pass adds, on the dry worlds: **sandstone outcrops** (beds stacked with ledges and overhangs, each its own
  shade, a rubble apron), **fossil skeletons** (a spine arching over a ribcage, a skull, the tail under the sand — on
  the dunes) and **wrecks** from the Dimming (a torn hull section nosed into the ground, bare frames, a fallen nacelle,
  plates); **boulder fields** (boulders and rocks gather in patches ~260 m across, rare between them); boulders are a
  big kind now (far mesh and impostors: they read from altitude). On water worlds coral heads and kelp forests grow
  down the shelf (coral to −24 m, kelp to −30 m) and take the water's light.
- **Sites** (placed per world from its seed): Costellatori ruins (the Gate of Threads — on about half the worlds with a
  broken arch —, the Silent Archive, the Last Observatory on the highest ground), relic shrines, crashed ships,
  faction outposts, all carved: bevelled weathered blocks, recessed panels, gold star-map glyphs on verdigris plates,
  fallen blocks and column drums, debris fields out to ~45 m, sized to read from the air. A passive scanner ping every
  8 s in the air shows signals within 2.6 km; `Y` pings 7 km; `N` cycles the signals. A site is found by flying low
  over it (under 240 m, within 320 m, 2.4 s) or landing near it: a banner, a radio line, credits and reputation,
  the codex (the ruin entries and the Costellatori; the Archive brings the novice; the world type's entry on
  arrival). Landing beside a found shrine (or a ruin that holds one) recovers a relic (cargo, or 220 cr when the
  hold is full) and the Echo's sentinels rise; raiders come for a guarded wreck.
- **Sound** (`constellations-sfx.js`, all procedural): a world's ambience under the music — wind that rises with the
  speed and the air's density, the roar and crackle of an entry, rain, snow hush, grit; a bed per biome (jungle insects
  and calls, frogs at night; surf; lava rumble and bubbling; ice wind and cracks; dry wind and rockfall; crystal
  chimes; the roar of a gas giant), thunder after each strike at the speed of sound. No allocation per frame: the
  renderer calls `ambSet` every frame, the bed updates at ~15 Hz.
  **Music is streamed**: each track plays from an `<audio>` element (its stored bytes as a Blob URL, made once per
  track) routed through a `MediaElementSource` into its gain, so the browser decodes a few seconds ahead instead of
  holding the whole track as PCM — `decodeAudioData` kept 76 MB per track; the renderer process went from 160 to 96 MB
  after a session of five track changes (the JS heap is unchanged, 7.9 MB: decoded audio never counted there). Players
  loop natively; a change fades the old one over 1.2 s while the new one rises over 2 s (up to three players cover a
  change in mid-fade); a play refused before the first gesture (iOS, Android Chrome) is retried by the unlock, which
  also gives every player its gesture-blessed play(). Under the sea the whole mix goes through a low-pass (480 Hz);
  a splash going in and out.
- **Engines**: the nozzle sprite is a small hot heart in the engine's own colour (white only at its tightest point),
  not a lamp, so the chase view over a world shows the exhaust, not two white discs.
- **Surface flight**: ground effect, speed lines, a terrain assist (`U`: levels off, pulls up before the ground),
  terrain collision, bolts strike the ground, the AI keeps off it. On water worlds the sea can be **dived into**: a
  splash, the water takes the speed (60 m/s, harder the deeper), the guns go safe and the boost is off, bubbles
  stream from the engines and rise; the assist and the pull-up warning watch the seabed, not the surface (no nag below
  18 m/s); the chase camera follows under the surface and stays there (over the seabed) until she breaks it again.
  The AI, the bolts and the landing gear keep to the surface (no landing on, or under, open water).
- **Planet HUD** (`hud.js`): pitch ladder and horizon, flight-path marker, heading tape (planet north), altitude
  above ground and sea (under water: the depth and the clearance over the seabed), climb, ground speed, gear and
  assist, site markers with distance (signals until found), the ping ring, survey and recovery bars, `PULL UP`, hull heat, pressure, landing line, context prompts. All text
  at least 13 px; on phones the altitude panel stacks above the prompt and the speed panel, clear of the touch
  buttons, the scanner has its own `SCN` button and the missile / flare counts ride on their buttons. Gamepad: R3
  context action, L3 scan in the air; Back/B/Start close the jump map. Five languages.
- **Web save**: `found` and `looted` bitmasks per `sector:system:world` in `/sd/data/costellazioni/web.json`
  (never in the shared struct).
- **Quality tiers** (terrain LOD distance `K`, finest vertex spacing, chunk pool, flora density and range, workers,
  cloud puffs, weather particles; shadow maps; ground cover; impostors; clouds; dust devils): low 2.1 / 4.2 m / 380 /
  0.32 × 380 m / 1 / 48 / 700, no shadow map, no ground cover, no impostors, 256² ground layers, no high veil, the
  cloud shell and its puffs, 2 dust devils, no post chain (scale 0.72); medium 2.5 / 2.6 m / 640 / 0.62 × 650 m / 2 /
  90 / 1300, shadows 1024² over ±150 m + 1024² over ±1.1 km (terrain, sites, hulls), cover to 70 m (6 blades a quad)
  and its far ring to 150 m (3), impostors to 3.4 km (16 000 cards), volumetric clouds (14 strides, 2 light taps), 4
  dust devils; high 2.9 / 1.7 m / 860 / 1.0 × 950 m / 2 / 140 / 2200, shadows 2048² over ±230 m + 2048² over ±1.7 km
  (and the far trees), cover to 115 m (9 blades) and to 240 m (4), impostors to 5 km (30 000 cards), volumetric clouds
  (22 strides, 3 light taps), 6 dust devils. Big flora kinds switch to their far mesh beyond 150 / 220 / 300 m and thin
  out with distance; small kinds end at 60 % (70 % on high) of the flora range; the instance caps fill from the camera
  out. The dynamic resolution steps down past 18 ms a frame (to 60 % of the tier's scale) and back up under 12.5 ms
  (`__czNoDynRes` pins it for probes).
- **Measured** (1920×1080, after the dynamic resolution settles; frame interval avg / p95, then in brackets the
  frame's cost with the GPU waited for). Jungle in rain at 100 m (`E2E_GPU=1`): Intel Arc 140T iGPU — auto (= medium,
  scale 0.92) 13.8 / 15.4 ms (16.2); low 8.3 / 8.5 ms, the 120 Hz cap (8.3); medium (1.0) 13.3 / 14.4 ms (15.3); high
  (0.89) 17.5 / 19.6 ms (20.3). RTX 5070 Laptop — every tier at the 120 Hz cap (8.3 / 8.5 ms); cost auto (= high,
  1.34) 11.6 ms, high 11.7, medium 6.9, low 4.3. The **ocean world is now the heaviest** (the water is a second draw
  over the seabed; 100 m over the reef at 150 m/s, the perf probe): iGPU low 8.3 / 8.5 ms (8.0), medium (1.0) 15.3 /
  16.7 ms (17.1 — the water about 1.7 ms of it), high (0.89) 19.8 / 23.7 ms (23.5); RTX at the cap on every tier, cost
  low 4.5, medium 8.0, high (1.35) 13.3 ms. The desert: iGPU low 8.3 / 8.5 (7.4), medium 10.6 / 11.7 (12.3), high
  (1.14) 17.1 / 19.1 (17.4); RTX cost 3.3 / 4.8 / 7.7 ms. Main thread on medium 3.0 ms (desert) to 3.9 ms (ocean).
  GPU timer queries on the iGPU (jungle, 150 m/s, scale pinned at 1.0 with `__czNoDynRes`): medium about 12.3 ms a
  frame — far 0.8, sharp cascade 0.9, wide cascade 0.2 (averaged: it is drawn every other frame), sky LUT 0.1, near
  8.7, the half-resolution passes (volumetric clouds, bright pass, blur) 1.1, bloom 0.1, final pass with the heat
  shimmer 0.4 — up from 10.4 ms before the clouds, the wide cascade, the far ground cover and the water. So high leans
  on the dynamic resolution on the iGPU, and medium stays at full scale near 16 ms over the sea. Entering a world for
  the first time costs one frame of about 150 ms (the world's programs are compiled off the main thread before it
  shows).
- **Tests**: `tools/games-host/test-costellazioni-worlds.mjs` (noise JS↔GLSL twin, terrain determinism and a digest
  of the generated worlds — `GOLDEN` was updated again for the dunes' sharp brink and the seabed of the water worlds;
  update it deliberately when a surface rule changes —, chunk seams, a water world's seabed at its true depth and the
  lava sea flat, the dune phase per vertex, POI placement, flora and its far meshes, atmosphere, discovery
  persistence, the descent / landing / take-off state machine — a take-off under a cliff rises first, the autopilot
  lands beside a site it has just flown over —, the solid ground and the dive: a splash, the water holds her under
  75 m/s, the guns go safe, the seabed is solid); `tools/web-e2e/costellazioni.e2e.mjs` (a world visited end to end
  with a discovery and the web save, a phone with touch emulation, the gamepad and the jump map, `E2E_GPU=1` frame
  times over a world per tier on the discrete and the integrated GPU — `launchBrowser({ gpu: 'low-power' })`,
  `__czSync` for the GPU-waited cost; its `toHub` presses Enter only on a modal that offers a choice — one pressed on
  the "syncing your run" card carried into the hub and launched a flight); `tools/web-e2e/costellazioni-shots.mjs
  --scenes worlds,underwater,clouds,shadows,dunes --gpu` for review shots (`--suffix -v3` keeps a before / after pair
  with `-v2`; the worlds scene shoots orbit, descent, 300 m and 70 m, the low ones in a raking light (`LIGHT`: the sun
  across the view, not behind the camera) with the nose a little down; `underwater` a reef from above, among it, and
  the surface from below; `clouds` over the tops and inside the layer; `shadows` a low sun over the mesas with and
  without the wide cascade; `dunes` the dune sea and a dust devil; dev hooks `__cz.dev.overWorld(world, alt, sunEl, az,
  v, hdg)`, `__cz.dev.nearSite(world, kinds, dist, alt, v, sunEl)`, `__cz.r3d.world.devilAhead(dist, deg)`,
  `world.shadow2Off`, flags `__czAutoLand`, `__czAutoSites`, `__czClock`, `__czNoDynRes`; `__cz.perf.cpu` is the main
  thread's share of a frame).

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
