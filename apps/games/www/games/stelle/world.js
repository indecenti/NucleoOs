// stelle/world.js — the WEB-ONLY visual layer of a Costellazioni star system, as pure data.
//
// Everything here is derived from (seed, sector, system) through DEDICATED hash domains (VD below),
// exactly like DOM.FLAVOR in constellations-gen.js: it never touches the numeric layer the firmware
// shares (prices, factions, beacons, missions). The same blueprint feeds the flight sim (asteroid and
// station collision volumes, battle anchors) and the Three.js renderer (star, planets, nebula, rocks,
// station modules), so what you see is what you collide with. No Three.js import: Node can test it.
import { hash3 } from '../constellations-gen.js';

const u32 = (x) => x >>> 0;
// Web visual domains: kept far from DOM.* (1..8) by a fixed high base, then one sub-domain per layer.
const VD = 0x57E11E00;
export const VDOM = { STAR: 1, PLANET: 2, NEB: 3, ROCK: 4, STATION: 5, LAYOUT: 6, MISSION: 7, TRAFFIC: 8, NAME: 9, SKY: 10, MOON: 11, GALAXY: 12, POI: 13 };
export const vhash = (seed, sector, sys, dom, salt) =>
  hash3(u32(seed ^ u32(VD + Math.imul(dom, 0x9E37))), u32(sector), u32(((sys & 0xff) << 16) | (salt & 0xffff)));

// mulberry32 stream seeded from a hash — deterministic sequences for one layer.
export function rng(seed) {
  let a = seed >>> 0;
  const f = () => {
    a = (a + 0x6D2B79F5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
  f.range = (lo, hi) => lo + (hi - lo) * f();
  f.int = (n) => Math.floor(f() * n);
  f.pick = (arr) => arr[Math.floor(f() * arr.length)];
  f.sign = () => (f() < 0.5 ? -1 : 1);
  return f;
}

// ---- colour helpers (linear-ish RGB triples in 0..1) ---------------------------------------------
export function hsl(h, s, l) {
  h = ((h % 1) + 1) % 1;
  const a = s * Math.min(l, 1 - l);
  const f = (n) => { const k = (n + h * 12) % 12; return l - a * Math.max(-1, Math.min(k - 3, 9 - k, 1)); };
  return [f(0), f(8), f(4)];
}
// Black-body-ish star tint for a temperature in kelvin (Tanner Helland fit, normalised).
export function kelvin(k) {
  const t = k / 100; let r, g, b;
  if (t <= 66) { r = 255; g = 99.47 * Math.log(t) - 161.12; b = t <= 19 ? 0 : 138.52 * Math.log(t - 10) - 305.04; }
  else { r = 329.7 * Math.pow(t - 60, -0.1332); g = 288.12 * Math.pow(t - 60, -0.0755); b = 255; }
  const c = (v) => Math.max(0, Math.min(255, v)) / 255;
  return [c(r), c(g), c(b)];
}
const norm = (v) => { const l = Math.hypot(v[0], v[1], v[2]) || 1; return [v[0] / l, v[1] / l, v[2] / l]; };
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
function randDir(r) { const z = r.range(-1, 1), a = r.range(0, Math.PI * 2), s = Math.sqrt(1 - z * z); return [s * Math.cos(a), z, s * Math.sin(a)]; }

// ---- star classes ---------------------------------------------------------------------------
// weight, temperature K, angular radius (deg), light intensity, flare activity, corona scale
const STARS = [
  { cls: 'M', w: 22, k: 3200, size: 2.2, lux: 2.3, flare: 0.8, corona: 1.6 },
  { cls: 'K', w: 22, k: 4400, size: 2.6, lux: 2.8, flare: 0.5, corona: 1.4 },
  { cls: 'G', w: 20, k: 5700, size: 2.8, lux: 3.1, flare: 0.4, corona: 1.3 },
  { cls: 'F', w: 13, k: 6800, size: 3.0, lux: 3.3, flare: 0.3, corona: 1.25 },
  { cls: 'A', w: 9, k: 9000, size: 3.3, lux: 3.6, flare: 0.25, corona: 1.2 },
  { cls: 'B', w: 6, k: 16000, size: 3.8, lux: 4.0, flare: 0.3, corona: 1.35 },
  { cls: 'RG', w: 5, k: 3000, size: 7.5, lux: 2.4, flare: 0.6, corona: 1.15 },   // red giant: huge, dim, swollen
  { cls: 'WD', w: 3, k: 12000, size: 0.9, lux: 2.6, flare: 0.1, corona: 2.4 },   // white dwarf: tiny, blinding
];
// Curated nebula harmonies (hues 0..1): two complementary veils + a dust tint. Picked per system,
// then nudged toward the star colour so the sky and the light agree.
const NEB = [
  [0.52, 0.86, 0.08], [0.74, 0.07, 0.62], [0.62, 0.12, 0.70], [0.98, 0.50, 0.58], [0.40, 0.78, 0.10],
  [0.92, 0.60, 0.70], [0.08, 0.68, 0.95], [0.45, 0.97, 0.55], [0.58, 0.04, 0.80], [0.80, 0.48, 0.02],
  [0.56, 0.62, 0.12], [0.03, 0.55, 0.85],
];
// Planet archetypes. ramp = 5 stops (hue, sat, light) from deep to high; ocean = sea level (0 = none).
export const PLANET_TYPES = ['rocky', 'desert', 'ocean', 'ice', 'jungle', 'volcanic', 'gas', 'crystal'];
const PT = {
  rocky: { ocean: 0.0, clouds: 0.25, atmo: 0.35, hab: 0.5, ramp: [[0.07, 0.15, 0.18], [0.08, 0.18, 0.30], [0.09, 0.15, 0.42], [0.10, 0.10, 0.55], [0.11, 0.06, 0.72]] },
  desert: { ocean: 0.0, clouds: 0.12, atmo: 0.45, hab: 0.4, ramp: [[0.05, 0.45, 0.30], [0.07, 0.55, 0.45], [0.09, 0.55, 0.58], [0.10, 0.45, 0.68], [0.11, 0.30, 0.80]] },
  ocean: { ocean: 0.62, clouds: 0.6, atmo: 0.8, hab: 0.9, ramp: [[0.60, 0.70, 0.14], [0.57, 0.65, 0.30], [0.25, 0.35, 0.35], [0.20, 0.30, 0.45], [0.12, 0.12, 0.80]] },
  ice: { ocean: 0.2, clouds: 0.35, atmo: 0.55, hab: 0.25, ramp: [[0.58, 0.40, 0.35], [0.56, 0.30, 0.55], [0.55, 0.22, 0.72], [0.55, 0.15, 0.84], [0.56, 0.10, 0.94]] },
  jungle: { ocean: 0.42, clouds: 0.55, atmo: 0.75, hab: 0.85, ramp: [[0.52, 0.60, 0.18], [0.33, 0.55, 0.20], [0.28, 0.60, 0.28], [0.22, 0.45, 0.38], [0.14, 0.25, 0.55]] },
  volcanic: { ocean: 0.0, clouds: 0.3, atmo: 0.35, hab: 0.1, ramp: [[0.02, 0.10, 0.06], [0.03, 0.15, 0.12], [0.05, 0.12, 0.20], [0.02, 0.85, 0.45], [0.08, 1.0, 0.62]] },
  gas: { ocean: 0.0, clouds: 0.0, atmo: 0.6, hab: 0.0, ramp: [[0.06, 0.45, 0.35], [0.08, 0.40, 0.55], [0.10, 0.30, 0.70], [0.04, 0.50, 0.45], [0.12, 0.20, 0.82]] },
  crystal: { ocean: 0.3, clouds: 0.15, atmo: 0.7, hab: 0.0, ramp: [[0.72, 0.60, 0.15], [0.70, 0.55, 0.32], [0.55, 0.70, 0.45], [0.50, 0.80, 0.62], [0.80, 0.50, 0.85]] },
};
const ROMAN = ['I', 'II', 'III', 'IV', 'V', 'VI', 'VII'];

// ---- the system blueprint ------------------------------------------------------------------------
// sys = genSystem(...) row ({ it, faction, econ, beacon, ... }); litBeacon = the save's bit for it.
export function systemBlueprint(seed, sector, sysIdx, sys, litBeacon = false) {
  const R = (dom, salt = 0) => rng(vhash(seed, sector, sysIdx, dom, salt));
  const fac = sys ? sys.faction : 0;
  const name = sys ? sys.it : 'Nova';

  // star
  const rs = R(VDOM.STAR);
  let tw = 0; for (const s of STARS) tw += s.w;
  let pick = rs() * tw, sc = STARS[0];
  for (const s of STARS) { pick -= s.w; if (pick <= 0) { sc = s; break; } }
  const k = sc.k * rs.range(0.92, 1.08);
  const sunDir = norm([rs.sign() * rs.range(0.65, 0.95), rs.range(0.12, 0.45), rs.range(0.25, 0.7)]);
  const star = { cls: sc.cls, kelvin: Math.round(k), color: kelvin(k), size: sc.size * rs.range(0.9, 1.15), lux: sc.lux,
    flare: sc.flare, corona: sc.corona, dir: sunDir, seed: u32(rs() * 4294967296) };

  // nebula palette (harmony nudged toward the star)
  const rn = R(VDOM.NEB);
  const H = NEB[rn.int(NEB.length)], hj = rn.range(-0.04, 0.04);
  const deep = sector > 5;   // deeper sectors are older, darker, stranger (lore: fewer stars)
  const nebula = {
    a: hsl(H[0] + hj, rn.range(0.55, 0.85), rn.range(0.38, 0.5)),
    b: hsl(H[1] + hj, rn.range(0.5, 0.8), rn.range(0.35, 0.5)),
    dust: hsl(H[2] + hj, rn.range(0.25, 0.5), rn.range(0.18, 0.3)),
    density: rn.range(0.45, 0.95) * (deep ? 0.8 : 1),
    warp: rn.range(0.6, 1.6), scale: rn.range(0.8, 1.6),
    axis: randDir(rn),                 // the galactic band direction (Milky-Way-like stripe)
    band: rn.range(0.35, 0.8),
    stars: Math.round(rn.range(0.7, 1.15) * (deep ? 0.75 : 1) * 1000) / 1000,
    seed: u32(rn() * 4294967296),
  };
  if (fac === 3) { nebula.a = hsl(0.52, 0.8, 0.45); nebula.b = hsl(0.76, 0.75, 0.42); }   // Echo space: cyan-violet lattice light

  // planets: one "home" world the station orbits (big in the sky) + 1..4 others
  const rp = R(VDOM.PLANET);
  const nPl = 2 + rp.int(4);
  const planets = [];
  const homeDir = norm([rp.range(-0.75, 0.25), rp.range(-0.45, 0.05), -1]);   // ahead of the start heading
  for (let i = 0; i < nPl; i++) {
    const home = i === 0;
    let type;
    const roll = rp();
    if (fac === 3 && roll < 0.45) type = 'crystal';
    else if (home && fac !== 3) type = ['ocean', 'jungle', 'rocky', 'desert', 'ice', 'ocean'][rp.int(6)];
    else type = PLANET_TYPES[Math.floor(roll * 7)];
    const def = PT[type];
    let dir;
    if (home) dir = homeDir;
    else {
      for (let tries = 0; tries < 20; tries++) {
        dir = randDir(rp);
        if (dot(dir, sunDir) < 0.75 && planets.every((p) => dot(p.dir, dir) < 0.9)) break;
      }
    }
    const angR = home ? rp.range(13, 21) : (type === 'gas' ? rp.range(4, 9) : rp.range(1.6, 5));
    const hj2 = rp.range(-0.05, 0.05);
    const ramp = def.ramp.map(([h, s, l]) => hsl(h + hj2, Math.min(1, s * rp.range(0.85, 1.15)), Math.min(0.95, l * rp.range(0.9, 1.1))));
    const atmoHue = type === 'desert' ? 0.07 : type === 'volcanic' ? 0.03 : type === 'crystal' ? 0.78 : type === 'gas' ? H[0] : 0.58;
    const p = {
      name: name + ' ' + ROMAN[i], type, dir, angR, seed: u32(rp() * 4294967296),
      ramp, ocean: def.ocean ? def.ocean + rp.range(-0.08, 0.08) : 0,
      oceanColor: hsl(type === 'crystal' ? 0.78 : 0.58 + rp.range(-0.04, 0.04), 0.7, type === 'ice' ? 0.55 : 0.2),
      clouds: def.clouds * rp.range(0.6, 1.3), cloudColor: hsl(atmoHue, 0.15, 0.92),
      atmo: def.atmo * rp.range(0.7, 1.2), atmoColor: hsl(atmoHue + rp.range(-0.03, 0.03), 0.65, 0.6),
      lights: fac !== 3 && home && rp() < def.hab ? 1 : 0,
      rot: rp.range(0.004, 0.02) * rp.sign(), tilt: rp.range(-0.45, 0.45),
      noise: rp.range(1.6, 3.2), bands: type === 'gas' ? rp.range(6, 16) : 0,
      rings: null, moonOf: -1,
    };
    if ((type === 'gas' && rp() < 0.7) || (type === 'ice' && rp() < 0.25) || (home && rp() < 0.18)) {
      p.rings = { inner: rp.range(1.35, 1.6), outer: rp.range(1.9, 2.6), color: hsl(rp.range(0.05, 0.13), 0.3, 0.68), tilt: rp.range(-0.5, 0.5), dens: rp.range(0.5, 0.95) };
    }
    planets.push(p);
  }
  // real places: the home world 40 km out, the others 70-190 km (same apparent size as before, so they are
  // physically bigger the farther they hang) — the cruise drive reaches any of them in seconds
  planets.forEach((p, i) => {
    p.dist = i === 0 ? 40000 : Math.round(70000 + rp() * 120000);
    p.radius = Math.round(Math.tan(p.angR * Math.PI / 180) * p.dist);
    p.pos = [p.dir[0] * p.dist, p.dir[1] * p.dist, p.dir[2] * p.dist];
    p.moons = moonsOf(seed, sector, sysIdx, i, p);
  });

  // battle space anchors (metres): the player starts at the origin heading -Z
  const rl = R(VDOM.LAYOUT);
  const inhabited = fac !== 3;
  const station = inhabited ? stationBlueprint(seed, sector, sysIdx, fac, rl) : null;
  const beacon = sys && sys.beacon ? { pos: [rl.range(-2600, -1400) * rl.sign(), rl.range(-300, 500), rl.range(-5200, -3800)], lit: !!litBeacon, height: 260, radius: 70 } : null;
  const fieldCenter = [rl.range(1900, 3000), rl.range(-500, 300), rl.range(-3600, -1800)];   // across the lane from the station
  const field = asteroidField(seed, sector, sysIdx, fieldCenter, rl.range(1500, 2300), sector);

  // where a jump drops you: at a lit beacon (the network holds you), else at the edge of the system, 13 km out,
  // looking in at the home world and the station
  const ra = R(VDOM.POI), toward = station ? station.pos : [0, 0, -2500];
  const edgeDir = norm([ra.range(-0.6, 0.6), ra.range(-0.12, 0.2), 1]);
  const edge = [toward[0] + edgeDir[0] * 13000, toward[1] + edgeDir[1] * 13000, toward[2] + edgeDir[2] * 13000];
  return { key: `${seed}:${sector}:${sysIdx}`, name, faction: fac, star, nebula, planets, station, beacon, field,
    sunDir, deep, inhabited, edge };
}

// ---- moons: 0-3 per world on a dedicated hash domain (orbit radius in planet radii, size, type) ------
const MOON_TYPES = ['rocky', 'ice', 'volcanic', 'desert', 'crystal'];
export function moonsOf(seed, sector, sysIdx, pi, p) {
  const r = rng(vhash(seed, sector, sysIdx, VDOM.MOON, pi));
  const n = p.type === 'gas' ? 1 + r.int(3) : r() < 0.55 ? 1 + (r() < 0.25 ? 1 : 0) : 0;
  const out = [], tilt = r.range(-0.35, 0.35);
  for (let k = 0; k < n; k++) {
    const type = p.type === 'crystal' ? 'crystal' : MOON_TYPES[r.int(p.type === 'gas' ? 4 : 2)];
    const def = PT[type], hj = r.range(-0.05, 0.05);
    out.push({ name: p.name + ' ' + String.fromCharCode(97 + k), type, seed: u32(r() * 4294967296),
      orbit: r.range(2.0, 3.4) + k * 1.1, phase: r.range(0, Math.PI * 2), tilt, size: r.range(0.1, type === 'volcanic' ? 0.2 : 0.27), spin: r.range(0.002, 0.006),
      ramp: def.ramp.map(([h, s, l]) => hsl(h + hj, s * 0.8, l)), ocean: 0, oceanColor: hsl(0.58, 0.5, 0.3), clouds: 0, atmo: type === 'volcanic' ? 0.25 : 0.05,
      atmoColor: hsl(type === 'volcanic' ? 0.04 : 0.58, 0.5, 0.6), lights: 0, noise: r.range(2.2, 3.6), bands: 0, rings: null, tiltAxis: r.range(-0.4, 0.4) });
  }
  return out;
}
// world-space centre of moon k of planet p (orbits are frozen: a sortie lasts minutes, an orbit days)
export function moonPos(p, m, out = [0, 0, 0]) {
  const a = m.phase, R = p.radius * m.orbit, ct = Math.cos(m.tilt), st = Math.sin(m.tilt);
  const x = Math.cos(a) * R, z = Math.sin(a) * R;
  out[0] = p.pos[0] + x; out[1] = p.pos[1] + z * st; out[2] = p.pos[2] + z * ct;
  return out;
}

// ---- station blueprint: a faction-styled set of modules (pure data, the renderer builds meshes) --
export function stationBlueprint(seed, sector, sysIdx, fac, rl) {
  const r = rng(vhash(seed, sector, sysIdx, VDOM.STATION, 0));
  const pos = rl ? [rl.range(-1700, -900), rl.range(-160, 160), rl.range(-3300, -2500)] : [-1200, 0, -2900];
  // Modules are pure data (stelle/kit.js builds the meshes from them). collide = conservative spheres over the
  // solid parts (local to pos); dock = the bay mouth p and its outward unit normal n: the lane p .. p + n*700 and
  // the bay ~25 m inside p stay clear of geometry and spheres. yaw turns the whole layout about Y (mods, spheres
  // and dock are built in the layout frame, then turned here so sim and renderer agree).
  const mods = [], collide = [], TAU = Math.PI * 2;
  const M = (kind, p, size, o = {}) => mods.push({ kind, p, size, ...o });
  const C = (x, y, z, rad) => collide.push([x, y, z, rad]);
  let yaw = 0, radius = 300, dock = null;
  if (fac === 2) {
    // Relitti: Gutterdeep, the hulk-town inside the freighter "Patience" — a bulging patched hull with lit
    // openings, cranes on its back, boats moored on cables; built along Z, turned so the bay faces +Z
    yaw = -Math.PI / 2;
    const Lh = r.range(400, 460), h2 = Lh / 2;
    const secs = [[h2, 26, 24, 6, 8, 7], [h2 - 25, 80, 70, 4, 24, 20], [h2 - 90, 116, 96, 2, 34, 28], [0, 126, 102, 0, 37, 30], [-h2 + 90, 114, 94, -2, 33, 28], [-h2 + 25, 72, 62, -4, 21, 18], [-h2, 28, 26, -6, 8, 7]];
    const at = (z) => { for (let i = 0; i < secs.length - 1; i++) { const a = secs[i], b = secs[i + 1]; if (z <= a[0] && z >= b[0]) { const t = (a[0] - z) / (a[0] - b[0]); return [a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t]; } } return [secs[6][1], secs[6][2]]; };
    const dz = r.range(-40, 40), D = 52, W = 46, H = 30;
    M('freighter', [0, 0, 0], [126, 102, Lh], { secs, dz });
    M('dockhouse', [63 + D, -10, dz], [W, H, D], { a: Math.PI / 2, style: 2, foot: 0 });
    for (let z = -h2 + 34; z <= h2 - 34; z += 44) { const s = at(z); C(0, 0, z, Math.max(s[0], s[1]) * 0.55); }
    C(63 + D / 2, -10 + H / 2 + 16, dz, 13); C(63 + D / 2, -10 - H / 2 - 16, dz, 13);
    for (let i = 0; i < 3; i++) { const z = -h2 * 0.6 + i * h2 * 0.6 + r.range(-20, 20), s = at(z); M('rcrane', [r.range(-20, 20), s[1] / 2 - 4, z], [r.range(50, 80), r.range(60, 100), 0], { a: r.range(0, TAU) }); }
    for (let i = 0; i < 4; i++) { const z = r.range(-h2 * 0.5, h2 * 0.5), s = at(z); M('stack', [r.range(-24, 24), s[1] / 2 - 3, z], [r.range(18, 30), r.range(14, 30), r.range(20, 36)]); }
    for (let i = 0; i < 6; i++) {
      const side = i % 2 ? 1 : -1;
      let z = r.range(-h2 + 70, h2 - 70); if (side > 0 && Math.abs(z - dz) < 90) z = dz + (z < dz ? -90 : 90);
      const s = at(z), x = side * (s[0] / 2 + r.range(26, 44)), y = r.range(-36, 30), L = r.range(24, 38);
      M('moored', [x, y, z], [L * 0.32, L * 0.24, L], { side, hull: [side * s[0] * 0.45, y * 0.5, z + r.range(-10, 10)] });
      C(x, y, z, L * 0.55);
    }
    dock = { p: [63 + D, -10, dz], n: [1, 0, 0] };
    radius = h2 + 40;
  } else if (fac === 1) {
    // Custodi: the Lamp Monastery — white-stone terraces, towers and a pagoda hall built inside the cracked
    // shell of the dead beacon Lumen, broken open towards +Z; candle lanterns float in the breach
    const Rs = r.range(155, 175), open = r.range(1.0, 1.1), fy = -Rs * 0.32;
    M('shell', [0, 0, 0], [Rs, 11, 0], { open });
    M('ledge', [0, fy, -10], [Rs, 60, 0]);
    M('terrace', [0, fy + 9, 0], [190, 18, 150]); M('terrace', [0, fy + 27, -14], [150, 18, 116]); M('terrace', [0, fy + 45, -26], [110, 18, 80]);
    M('pagoda', [0, fy + 54, -28], [72, 0, 56], { tiers: 3 });
    for (const sx of [-1, 1]) { M('hall', [sx * 72, fy + 18, -6], [28, 24, 52]); C(sx * 72, fy + 32, -6, 30); }
    const towers = [[-84, fy + 18, 60, r.range(46, 64)], [84, fy + 18, 60, r.range(46, 64)], [-64, fy + 36, -58, r.range(56, 80)], [64, fy + 36, -58, r.range(56, 80)], [-30, fy + 54, -62, r.range(40, 52)], [30, fy + 54, -62, r.range(40, 52)]];
    for (const [x, y, z, h] of towers) { M('ctower', [x, y, z], [12, h, 12]); C(x, y + h * 0.5, z, h * 0.5 + 4); }
    M('stairs', [0, fy + 18, 57], [30, 18, 26]);
    const mouth = [0, fy + 20, 119];
    M('dockhouse', mouth, [36, 22, 40], { a: 0, style: 1, foot: 12 });
    M('lanterns', [0, fy, 0], [Rs, 0, 0], { n: 40, lane: mouth });
    const N = 46, ga = Math.PI * (3 - Math.sqrt(5));
    for (let k = 0; k < N; k++) { const z = 1 - 2 * (k + 0.5) / N, q = Math.sqrt(1 - z * z), ph = k * ga; if (Math.acos(z) > open + 0.16) C(q * Math.cos(ph) * (Rs - 6), q * Math.sin(ph) * (Rs - 6), z * (Rs - 6), 46); }
    C(-70, fy - 40, -20, 55); C(70, fy - 40, -20, 55); C(0, fy - 45, 35, 55);
    C(-55, fy + 9, 30, 48); C(55, fy + 9, 30, 48); C(-55, fy + 9, -40, 48); C(55, fy + 9, -40, 48);
    C(0, fy + 50, -26, 64); C(0, fy + 47, 99, 12);
    dock = { p: mouth, n: [0, 0, 1] };
    radius = Rs * 1.3 + 12;
  } else {
    // Gilda: the Ardali Exchange — a brass-and-ivory ring city (domed districts, spires, blue window bands)
    // round a hub crowned by a golden dome, piers between them; the dock house juts from the ring towards +Z
    const R = r.range(240, 300), W = 44, H = 30, da = -Math.PI / 2, D = 54;
    M('gring', [0, 0, 0], [R, W, H], { n: 34 + r.int(6), da });
    M('ghub', [0, 0, 0], [64, 0, 0]);
    for (let i = 0; i < 6; i++) M('gpier', [0, 0, 0], [R - W / 2, 7, 9], { a: da + (i + 0.5) / 6 * TAU });
    const mouth = [0, 0, R + W / 2 + D];
    M('dockhouse', mouth, [40, 26, D], { a: 0, style: 0, foot: 0 });
    for (let k = 0; k < 32; k++) { const a = k / 32 * TAU; C(R * Math.cos(a), 0, -R * Math.sin(a), 34); if (k % 2 === 0) C(R * Math.cos(a), H / 2 + 24, -R * Math.sin(a), 24); }
    C(0, 0, 0, 70); C(0, 74, 0, 40); C(0, -46, 0, 34);
    C(0, 13 + 14, R + W / 2 + D / 2, 13); C(0, -13 - 14, R + W / 2 + D / 2, 13);
    dock = { p: mouth, n: [0, 0, 1] };
    radius = R + W / 2 + D + 76;
  }
  if (yaw) {   // turn spheres and dock from the layout frame into the station frame (same rotation as the meshes)
    const c = Math.cos(yaw), s = Math.sin(yaw), rot = (v) => [c * v[0] + s * v[2], v[1], -s * v[0] + c * v[2]];
    for (const k of collide) { const v = rot(k); k[0] = v[0]; k[2] = v[2]; }
    dock = { p: rot(dock.p), n: rot(dock.n).map((v) => (Math.abs(v) < 1e-9 ? 0 : v)) };
  }
  return { pos, faction: fac, mods, collide, radius, seed: u32(r() * 4294967296), lights: 30 + r.int(30), dock, yaw };
}

// ---- asteroid field: the collidable rocks (same list for sim and renderer) -----------------------
// Sizes follow a power law (many pebbles, a few mountains); positions cluster in a flattened lens
// with a few denser knots where Relitti ambushes hide.
export function asteroidField(seed, sector, sysIdx, center, radius, depth = 0) {
  const r = rng(vhash(seed, sector, sysIdx, VDOM.ROCK, 0));
  const N = 520 + r.int(260);
  const x = new Float32Array(N), y = new Float32Array(N), z = new Float32Array(N), rad = new Float32Array(N), spin = new Float32Array(N), shape = new Uint8Array(N);
  const knots = [];
  for (let i = 0; i < 4; i++) knots.push([r.range(-0.6, 0.6) * radius, r.range(-0.18, 0.18) * radius, r.range(-0.6, 0.6) * radius]);
  const tiltA = r.range(-0.4, 0.4), tiltB = r.range(-0.4, 0.4);
  const ca = Math.cos(tiltA), sa = Math.sin(tiltA), cb = Math.cos(tiltB), sb = Math.sin(tiltB);
  const tilt = (px, py, pz) => {   // tilt the lens (same transform for rocks and knots)
    let ty = py * ca - pz * sa; const tz = py * sa + pz * ca; py = ty; pz = tz;
    const tx = px * cb - py * sb; ty = px * sb + py * cb;
    return [center[0] + tx, center[1] + ty, center[2] + pz];
  };
  for (let i = 0; i < N; i++) {
    let px, py, pz;
    if (r() < 0.38) { const k = knots[r.int(4)], s = radius * 0.16; px = k[0] + r.range(-s, s); py = k[1] + r.range(-s, s) * 0.6; pz = k[2] + r.range(-s, s); }
    else { const a = r.range(0, Math.PI * 2), d = Math.sqrt(r()) * radius; px = Math.cos(a) * d; pz = Math.sin(a) * d; py = r.range(-1, 1) * radius * 0.16 * (1 - d / radius * 0.5); }
    const u = r(), t = tilt(px, py, pz);
    x[i] = t[0]; y[i] = t[1]; z[i] = t[2];
    rad[i] = 6 + Math.pow(u, 5.5) * 150 + r.range(0, 6);
    spin[i] = r.range(-0.25, 0.25);
    shape[i] = r.int(4);
  }
  return { center, radius, n: N, x, y, z, r: rad, spin, shape, knots: knots.map((k) => tilt(k[0], k[1], k[2])), seed: u32(r() * 4294967296), depth };
}
