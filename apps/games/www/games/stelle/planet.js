// stelle/planet.js — the SURFACE of a Costellazioni world (web-only), as pure deterministic math.
//
// One description per world, derived from the system blueprint (stelle/world.js) on dedicated hash domains
// (VDOM.SURF / SITE / FLORA / WEATHER), so the shared generator's numbers are never touched. It feeds:
//   · the terrain worker (stelle/terrain-worker.js) — chunk meshes, flora;
//   · the flight sim (stelle/surface.js) — the ground you collide with, landing spots, the sites you discover;
//   · the GPU — MACRO_GLSL is the same continent / mountain / moisture field and colour ramp the orbital bake
//     paints (stelle/space.js), so the planet you see from orbit is the one you land on.
// Units: metres; a direction n is a unit vector in the planet's own frame (before its tilt and spin).
// Sea level is elevation 0 on worlds with a sea (water, ice, lava, crystal lakes); dry worlds have none.
import { rng, vhash, VDOM } from './world.js';
import { fbm, ridged, noise3, NOISE_GLSL } from './noise.js';

const u32 = (x) => x >>> 0;
const clamp = (x, a, b) => (x < a ? a : x > b ? b : x);
const sstep = (a, b, x) => { const t = clamp((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); };
const lin = (c) => [Math.pow(c[0], 2.2), Math.pow(c[1], 2.2), Math.pow(c[2], 2.2)];
export const TYPE_ID = { rocky: 0, desert: 1, ocean: 2, ice: 3, jungle: 4, volcanic: 5, gas: 6, crystal: 7 };
export const landable = (type) => type !== 'gas';

// per world type: relief (fraction of the radius), mountain weight, what fills the low ground, the haze, the
// sky profile, the weather and the flora kit. Read with the world_* illustrations next to it.
const KIND = {
  rocky: { relief: 0.05, mnt: 0.9, liquid: 0, haze: 0.12, abs: 1.4, sky: [0.42, 0.55, 0.9], dust: [1.0, 0.8, 0.64], weather: 'dust', wk: 0.3,
    flora: [['hoodoo', 0.00006, 6, 18], ['rock', 0.0009, 0.8, 3.4], ['shrub', 0.0016, 0.6, 1.6]] },
  desert: { relief: 0.032, mnt: 0.6, liquid: 0, haze: 0.3, abs: 2.4, sky: [0.55, 0.55, 0.8], dust: [1.0, 0.72, 0.46], weather: 'dust', wk: 0.75,
    flora: [['rib', 0.000012, 18, 34], ['spire', 0.00005, 8, 22], ['rock', 0.0005, 0.8, 3.0], ['shrub', 0.0005, 0.5, 1.2]] },
  ocean: { relief: 0.045, mnt: 0.5, liquid: 1, haze: 0.07, sky: [0.2, 0.52, 1.0], dust: [0.9, 0.95, 1.0], weather: 'rain', wk: 0.55,
    flora: [['palm', 0.0012, 6, 13], ['coral', 0.0016, 0.8, 3.2], ['rock', 0.0006, 0.8, 2.6]] },
  ice: { relief: 0.04, mnt: 0.8, liquid: 2, haze: 0.05, sky: [0.22, 0.5, 1.0], dust: [0.95, 0.97, 1.0], weather: 'snow', wk: 0.65,
    flora: [['icespire', 0.00012, 5, 26], ['rock', 0.0006, 0.8, 3.0], ['frost', 0.0009, 0.4, 1.2]] },
  jungle: { relief: 0.036, mnt: 0.6, liquid: 1, haze: 0.09, sky: [0.26, 0.56, 1.0], dust: [0.86, 1.0, 0.9], weather: 'rain', wk: 0.4,
    flora: [['spiral', 0.0016, 9, 24], ['fern', 0.0045, 0.8, 2.6], ['glowpod', 0.0018, 0.4, 1.2], ['rock', 0.0003, 0.8, 2.6]] },
  volcanic: { relief: 0.05, mnt: 1.0, liquid: 3, seaLvl: 0.3, haze: 0.3, abs: 2.6, sky: [0.62, 0.5, 0.52], dust: [0.42, 0.3, 0.26], weather: 'ash', wk: 0.8,
    flora: [['basalt', 0.00022, 3, 12], ['deadtree', 0.0005, 4, 10], ['ember', 0.0004, 0.6, 1.8], ['rock', 0.0007, 0.8, 3.0]] },
  gas: { relief: 0, mnt: 0, liquid: 0, haze: 0.2, sky: [0.75, 0.62, 0.42], dust: [1.0, 0.9, 0.7], weather: 'wind', wk: 0.5, flora: [] },
  crystal: { relief: 0.045, mnt: 0.8, liquid: 4, haze: 0.08, sky: [0.62, 0.32, 1.0], dust: [0.7, 0.8, 1.0], weather: 'motes', wk: 0.55,
    flora: [['shard', 0.0011, 1.2, 9], ['lattice', 0.0005, 6, 16], ['rock', 0.0004, 0.8, 2.6]] },
};
export const FLORA_KINDS = ['rock', 'shrub', 'hoodoo', 'rib', 'spire', 'palm', 'coral', 'icespire', 'frost', 'spiral', 'fern', 'glowpod', 'basalt', 'deadtree', 'ember', 'shard', 'lattice'];
export const SITE_KINDS = ['gate', 'archive', 'observatory', 'relic', 'wreck', 'outpost'];

// ---- the surface description (plain data: it crosses into the worker) --------------------------------------------
// pl = blueprint planet (or moon), pi = its index; returns null for nothing (no planet)
export function surfaceOf(pl, seed, sector, sysIdx, pi, faction = 0) {
  if (!pl) return null;
  const K = KIND[pl.type] || KIND.rocky, r = rng(vhash(seed, sector, sysIdx, VDOM.SURF, pi));
  const R = pl.radius, seeds = [];
  for (let i = 0; i < 8; i++) seeds.push(u32(r() * 4294967296));
  const relief = K.relief ? clamp(R * K.relief * r.range(0.85, 1.15), 160, 900) : 0;
  const liquid = K.liquid;
  const sea = liquid === 3 ? K.seaLvl : liquid === 2 ? clamp(Math.max(0.33, pl.ocean || 0), 0.33, 0.5) : liquid ? clamp(pl.ocean || 0, 0.12, 0.72) : 0;
  // atmosphere: top, scale heights, scattering (Rayleigh tint from the type, Mie haze); the zenith optical
  // depths are what make a sky — scaled to the world's thin shell so a small planet still has a deep blue
  const top = clamp(R * 0.36, 2000, 7000), atk = clamp(pl.atmo || 0.5, 0.25, 1.1);
  // the air you fly through is set per metre (how far you see does not depend on the size of the world: ~9 km in
  // blue light, less in dust and ash); scale heights are a good fraction of the shell so the sky is already blue below
  // the clouds; a small world's thinner column gets a brighter sky gain instead of a hazier air
  const HR = top / 2.4, HM = top / 6;
  const sk = K.sky, skm = Math.max(sk[0], sk[1], sk[2]), bR0 = 1.15e-4 * atk / 0.6;
  const betaR = sk.map((v) => (v / skm) * bR0);
  const betaM = K.dust.map((v) => v * K.haze * 5.5e-4 * (0.85 + 0.3 * r()));
  const gainK = clamp(0.25 / (bR0 * HR), 1, 2.5);
  const cloudAlt = top * r.range(0.3, 0.42), cover = clamp((pl.clouds || 0) * 1.25, 0, 0.95);
  const rw = rng(vhash(seed, sector, sysIdx, VDOM.WEATHER, pi));
  const S = {
    key: `${seed}:${sector}:${sysIdx}:${pi}`, type: pl.type, ti: TYPE_ID[pl.type] | 0, name: pl.name, pi, fac: faction,
    R, A: relief, F: pl.noise || 2.2, seeds, sea, liquid, depth: relief * 0.9, mntK: K.mnt,
    atmo: { top, HR, HM, betaR, betaM, abs: K.abs || 0, gainK, g: pl.type === 'desert' || pl.type === 'volcanic' ? 0.68 : 0.76, k: atk },
    cloud: { alt: cloudAlt, cover, thick: r.range(120, 260) },
    weather: { kind: K.weather, k: clamp(K.wk * rw.range(0.5, 1.3), 0, 1), wind: [rw.range(-1, 1), rw.range(-1, 1)], storm: rw() < 0.3 },
    ramp: (pl.ramp || []).map(lin), ocean: lin(pl.oceanColor || [0.1, 0.2, 0.4]), lights: pl.lights || 0,
    flora: K.flora.map(([kind, dens, s0, s1]) => ({ kind, dens, s0, s1, id: FLORA_KINDS.indexOf(kind) })),
    sites: null,
  };
  S.sites = placeSites(S, seed, sector, sysIdx, pi, faction);
  return S;
}

// the same description for the renderer and the sim, built once per blueprint world (bp.key = "seed:sector:sys")
const SCACHE = new WeakMap();
export function surfaceFor(bp, pi) {
  const pl = bp && bp.planets && bp.planets[pi]; if (!pl) return null;
  let S = SCACHE.get(pl); if (S) return S;
  const k = String(bp.key || '0:0:0').split(':').map(Number);
  S = surfaceOf(pl, k[0] >>> 0, k[1] >>> 0, k[2] | 0, pi, bp.faction | 0);
  SCACHE.set(pl, S); return S;
}

// ---- the macro field: continents, mountains, moisture (identical in MACRO_GLSL) -----------------------------------
const MAC = { h: 0, c: 0, mn: 0, mo: 0, qx: 0, qy: 0, qz: 0 };
export function macro(S, x, y, z, out = MAC) {
  const F = S.F, sd = S.seeds, px = x * F, py = y * F, pz = z * F;
  const qx = fbm(px * 0.8, py * 0.8, pz * 0.8, 3, sd[0]);
  const qy = fbm(px * 0.8 + 5.2, py * 0.8 + 1.3, pz * 0.8 + 2.8, 3, sd[0] ^ 0x51);
  const qz = fbm(px * 0.8 + 9.7, py * 0.8 + 4.1, pz * 0.8 + 6.3, 3, sd[0] ^ 0xA3);
  const c = fbm(px + qx * 0.9, py + qy * 0.9, pz + qz * 0.9, 6, sd[1]);
  const mn = ridged(px * 1.9 + qx * 0.6, py * 1.9 + qy * 0.6, pz * 1.9 + qz * 0.6, 5, sd[2]);
  let h = 0.5 + c * 0.75;
  h += mn * S.mntK * sstep(0.48, 0.78, h) * 0.32;
  out.h = clamp(h, 0, 1); out.c = c; out.mn = mn; out.qx = qx; out.qy = qy; out.qz = qz;
  out.mo = fbm(px * 1.3 + 11, py * 1.3 + 3, pz * 1.3 + 7, 4, sd[3]) * 0.5 + 0.5;
  return out;
}

// ---- elevation (metres): the macro field shaped per world type, plus detail -------------------------------------------
// ex (optional) receives the shading masks the terrain shader uses: m1 (type mask), m2 (rockiness), h, mo
const EX = { h: 0, mo: 0, m1: 0, m2: 0 };
const terrace = (x, steps, k) => { const s = x * steps, f = s - Math.floor(s); return (Math.floor(s) + sstep(0.5 - k, 0.5 + k, f)) / steps; };
export function elevationRaw(S, x, y, z, ex = EX) {
  const M = macro(S, x, y, z), A = S.A, sd = S.seeds, F = S.F;
  if (!A) { ex.h = M.h; ex.mo = M.mo; ex.m1 = 0; ex.m2 = 0; return 0; }
  const px = x * F, py = y * F, pz = z * F, sea = S.sea;
  const hills = fbm(px * 18 + M.qx, py * 18 + M.qy, pz * 18 + M.qz, 5, sd[4]);
  const fine = fbm(px * 900, py * 900, pz * 900, 3, sd[5]);
  let e, m1 = 0, m2 = clamp(0.5 + hills * 1.6, 0, 1);
  const land = sea > 0 ? (M.h - sea) / (1 - sea) : M.h;
  switch (S.ti) {
    case 0: {   // rocky: mesas cut by canyons, ridged ranges
      const base = Math.pow(clamp(land, 0, 1), 1.1);
      const t = terrace(base * 0.9 + hills * 0.1, 4.5, 0.08);
      const cn = Math.abs(fbm(px * 4.2 + M.qx, py * 4.2 + M.qy, pz * 4.2 + M.qz, 4, sd[6]));
      const cut = sstep(0.02, 0.085, cn);
      e = A * (0.3 * t + 0.62 * t * cut + 0.5 * M.mn * M.mn * sstep(0.55, 0.85, M.h)) + A * 0.035 * hills;
      m1 = 1 - cut; break;
    }
    case 1: {   // desert: sand seas with dune crests, rock plateaus, flat playas
      const sand = sstep(0.6, 0.25, land) * (1 - sstep(0.15, 0.4, M.mn));
      const dn = 1 - Math.abs(noise3(px * 26 + M.qx * 2, py * 70, pz * 26 + M.qz * 2, sd[6]));
      const dn2 = 1 - Math.abs(noise3(px * 61, py * 160, pz * 61, sd[7]));
      const dune = dn * dn * dn * 30 + dn2 * dn2 * 7;
      e = A * (0.75 * land + 0.18 * M.mn * M.mn) + A * 0.03 * hills * (1 - sand) + dune * sand;
      if (M.c < -0.18) { const k = sstep(-0.18, -0.3, M.c); e = e * (1 - k) + A * 0.06 * k; }
      m1 = sand; break;
    }
    case 2: {   // ocean: steep islands on shallow shelves
      e = land > 0 ? A * Math.pow(land, 0.62) * (0.72 + 0.28 * hills) : 0;
      m1 = sstep(0.06, 0.0, land); break;
    }
    case 3: {   // ice: glacier plains, crevasse fields, sharp ranges
      e = A * (0.5 * land + 0.5 * land * (0.55 + M.mn * 0.45)) + A * 0.02 * hills;
      const cr = Math.abs(noise3(px * 95, py * 95, pz * 95, sd[6]));
      const crev = (1 - sstep(0.0, 0.05, cr)) * sstep(0.08, 0.3, land);
      e -= crev * 7; m1 = crev; break;
    }
    case 4: {   // jungle: rolling hills carved by river valleys
      e = A * (0.5 * land + 0.22 * (hills + 0.4) + 0.3 * M.mn * M.mn * sstep(0.5, 0.8, M.h));
      const rv = Math.abs(fbm(px * 3.1 + M.qx * 0.7, py * 3.1 + M.qy * 0.7, pz * 3.1 + M.qz * 0.7, 3, sd[6]));
      const vall = sstep(0.0, 0.07, rv);
      if (land > 0) e = e * vall + (e * 0.12 - 6) * (1 - vall);
      m1 = 1 - vall; break;
    }
    case 5: {   // volcanic: basalt shelves, cones with craters, lava below the lava line
      e = A * (0.55 * land + 0.45 * Math.pow(M.mn, 1.5) * sstep(0.4, 0.8, M.h));
      const k = noise3(px * 5, py * 5, pz * 5, sd[6]);
      if (k > 0.3) { const cone = sstep(0.3, 0.72, k); e += A * 0.55 * cone * cone - A * 0.35 * sstep(0.66, 0.8, k); }
      const cr = Math.abs(noise3(px * 38 + M.qx, py * 38 + M.qy, pz * 38 + M.qz, sd[7]));
      m1 = (1 - sstep(0.0, 0.028, cr)) * sstep(0.22, 0.02, land); break;
    }
    case 7: {   // crystal: faceted terraces and knife ridges
      const fct = 1 - Math.abs(noise3(px * 7 + M.qx, py * 7 + M.qy, pz * 7 + M.qz, sd[6]));
      const t = terrace(clamp(land, 0, 1) * 0.7 + fct * fct * 0.3, 6, 0.03);
      e = A * t + A * 0.02 * hills;
      const vn = Math.abs(ridged(px * 12, py * 12, pz * 12, 2, sd[7]) - 0.62);
      m1 = 1 - sstep(0.0, 0.035, vn); break;
    }
    default: e = 0;
  }
  if (sea > 0 && land < 0) {   // under the sea: a shelf near the coast, then the deep
    const d = clamp(-land * (1 - sea) / Math.max(sea, 0.05), 0, 1);
    e = -S.depth * Math.pow(d, 1.25) - 0.5;
  }
  e += fine * 1.2;
  ex.h = M.h; ex.mo = M.mo; ex.m1 = m1; ex.m2 = m2;
  return e;
}
// the ground as built: sites sit on levelled pads that blend back into the land
export function elevation(S, x, y, z, ex = EX) {
  let e = elevationRaw(S, x, y, z, ex);
  const st = S.sites;
  if (st) for (let i = 0; i < st.length; i++) {
    const s = st[i], c = x * s.dir[0] + y * s.dir[1] + z * s.dir[2];
    if (c < s.cos1) continue;
    const d = Math.acos(Math.min(1, c)) * S.R;
    const k = sstep(s.r1, s.r0, d);
    e = e + (s.e - e) * k;
  }
  return e;
}
// the solid surface: the ground, or the sea surface over it (ships skim water, lava and ice alike)
export function surfaceElevation(S, x, y, z) { const e = elevation(S, x, y, z); return S.sea > 0 ? Math.max(e, 0) : e; }

// ---- cube-sphere: face f, (u, v) in [-1, 1] -> unit direction (exact on the shared edges) ----------------------------
// faces: 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z; (u, v) runs along fixed axes so neighbouring faces meet bit-exactly
const FACE = [
  [1, 0, 0, 0, 0, -1, 0, 1, 0], [-1, 0, 0, 0, 0, 1, 0, 1, 0],
  [0, 1, 0, 1, 0, 0, 0, 0, -1], [0, -1, 0, 1, 0, 0, 0, 0, 1],
  [0, 0, 1, 1, 0, 0, 0, 1, 0], [0, 0, -1, -1, 0, 0, 0, 1, 0],
];
export const FACES = FACE;
export function cubeDir(f, u, v, out) {
  const F = FACE[f];
  const x = F[0] + u * F[3] + v * F[6], y = F[1] + u * F[4] + v * F[7], z = F[2] + u * F[5] + v * F[8];
  // the "spherified cube" map: more even cells than a plain normalise
  const x2 = x * x, y2 = y * y, z2 = z * z;
  out[0] = x * Math.sqrt(Math.max(0, 1 - y2 / 2 - z2 / 2 + y2 * z2 / 3));
  out[1] = y * Math.sqrt(Math.max(0, 1 - z2 / 2 - x2 / 2 + z2 * x2 / 3));
  out[2] = z * Math.sqrt(Math.max(0, 1 - x2 / 2 - y2 / 2 + x2 * y2 / 3));
  return out;
}
// inverse: a direction -> face and (u, v) (the face whose axis dominates; u, v of the plain projection, refined)
export function dirFace(x, y, z, out) {
  const ax = Math.abs(x), ay = Math.abs(y), az = Math.abs(z);
  let f; if (ax >= ay && ax >= az) f = x > 0 ? 0 : 1; else if (ay >= az) f = y > 0 ? 2 : 3; else f = z > 0 ? 4 : 5;
  const F = FACE[f], dn = x * F[0] + y * F[1] + z * F[2];
  let u = (x * F[3] + y * F[4] + z * F[5]) / dn, v = (x * F[6] + y * F[7] + z * F[8]) / dn;
  // Newton steps on the spherified map so (u, v) lands on the same lattice cubeDir walks
  const d = [0, 0, 0];
  for (let it = 0; it < 4; it++) {
    cubeDir(f, u, v, d);
    const eu = (x - d[0]) * F[3] + (y - d[1]) * F[4] + (z - d[2]) * F[5], ev = (x - d[0]) * F[6] + (y - d[1]) * F[7] + (z - d[2]) * F[8];
    u = clamp(u + eu * 1.25, -1, 1); v = clamp(v + ev * 1.25, -1, 1);
  }
  out[0] = f; out[1] = u; out[2] = v; return out;
}

// ---- the quadtree's geometry rules (shared by the renderer, the worker and the tests) ----------------------------------
export const GRID = 32;                                   // quads per chunk side (33 x 33 vertices + skirts)
export const nodeSize = (S, level) => S.R * Math.PI / 2 / (1 << level);
export function maxLevel(S, spacing) { return clamp(Math.ceil(Math.log2(S.R * Math.PI / 2 / GRID / spacing)), 3, 14); }

// ---- sites: ruins of the Costellatori, relic shrines, wrecks, outposts — placed where they make sense ---------------
const SITE_R = { gate: 70, archive: 80, observatory: 60, relic: 34, wreck: 90, outpost: 70 };
function placeSites(S, seed, sector, sysIdx, pi, fac) {
  if (!S.A) return [];
  const r = rng(vhash(seed, sector, sysIdx, VDOM.SITE, pi));
  const plan = [];
  const ruinN = 1 + r.int(2) + (S.type === 'rocky' || S.type === 'crystal' ? 1 : 0);
  const ruins = ['gate', 'archive', 'observatory'];
  for (let i = 0; i < ruinN; i++) plan.push(ruins[(r.int(3) + i) % 3]);
  if (r() < 0.6) plan.push('relic');
  for (let i = 0, n = 1 + r.int(2); i < n; i++) plan.push('wreck');
  if (fac !== 3) for (let i = 0, n = 1 + r.int(2); i < n; i++) plan.push('outpost');
  const out = [], d = [0, 0, 0], tmp = { h: 0, mo: 0, m1: 0, m2: 0 }, minSep = Math.min(0.5, 1600 / S.R);
  const slopeAt = (x, y, z, e0) => {   // worst rise over 30 m in four directions
    let ux = -z, uy = 0, uz = x; if (Math.abs(y) > 0.9) { ux = 1; uy = 0; uz = 0; }
    let l = Math.hypot(ux, uy, uz); ux /= l; uy /= l; uz /= l;
    let vx = y * uz - z * uy, vy = z * ux - x * uz, vz = x * uy - y * ux;
    const k = 30 / S.R; let s = 0;
    for (const [a, b] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
      let qx = x + (ux * a + vx * b) * k, qy = y + (uy * a + vy * b) * k, qz = z + (uz * a + vz * b) * k; l = Math.hypot(qx, qy, qz);
      s = Math.max(s, Math.abs(elevationRaw(S, qx / l, qy / l, qz / l, tmp) - e0) / 30);
    }
    return s;
  };
  for (let k = 0; k < plan.length; k++) {
    const kind = plan[k];
    let best = null, bestScore = -1e9;
    const tries = kind === 'observatory' ? 28 : 14;
    for (let t = 0; t < tries; t++) {
      const zz = r.range(-0.92, 0.92), a = r.range(0, Math.PI * 2), q = Math.sqrt(1 - zz * zz);
      d[0] = q * Math.cos(a); d[1] = zz; d[2] = q * Math.sin(a);
      if (out.some((s) => Math.acos(Math.min(1, s.dir[0] * d[0] + s.dir[1] * d[1] + s.dir[2] * d[2])) < minSep)) continue;
      const e = elevationRaw(S, d[0], d[1], d[2], tmp);
      if (S.sea > 0 && e < 4) continue;
      const sl = slopeAt(d[0], d[1], d[2], e);
      if (sl > (kind === 'observatory' ? 0.9 : 0.45)) continue;
      const score = kind === 'observatory' ? e : -sl * 100 + r();
      if (score > bestScore) { bestScore = score; best = [d[0], d[1], d[2], e]; }
      if (kind !== 'observatory' && sl < 0.18) break;
    }
    if (!best) continue;
    const R0 = SITE_R[kind] || 60;
    const loot = { cr: 0, rep: -1, repN: 0, relic: false };
    if (kind === 'gate' || kind === 'archive' || kind === 'observatory') { loot.cr = 180 + r.int(5) * 40; loot.rep = 1; loot.repN = 3; loot.relic = r() < 0.35; }
    else if (kind === 'relic') { loot.cr = 60; loot.rep = 1; loot.repN = 2; loot.relic = true; }
    else if (kind === 'wreck') { loot.cr = 120 + r.int(6) * 30; loot.rep = 2; loot.repN = 1; }
    else { loot.cr = 90 + r.int(4) * 20; loot.rep = fac; loot.repN = 2; }
    const guard = kind === 'wreck' ? (r() < 0.55 ? 'raider' : null) : (loot.relic || kind === 'relic') ? 'sentinel' : (r() < 0.25 ? 'sentinel' : null);
    out.push({ i: out.length, kind, dir: [best[0], best[1], best[2]], e: best[3] + (kind === 'wreck' ? -1.5 : 0.2), yaw: r.range(0, Math.PI * 2), r0: R0, r1: R0 * 2.4,
      cos1: Math.cos(R0 * 2.4 / S.R), loot, guard, fac: kind === 'outpost' ? fac : kind === 'wreck' ? 2 : 1, seed: u32(r() * 4294967296) });
  }
  return out;
}

// ---- flora: deterministic scatter per quadtree cell (face, level, x, y) ------------------------------------------------
// Returns one Float32Array of instances [x, y, z, scale, yaw, kind, tint, lean] relative to the cell centre (cx, cy, cz),
// and the count. Placement uses the true ground (the finest mesh follows it within centimetres).
export const FLORA_STRIDE = 8;
const FL = [0, 0, 0], FL2 = [0, 0, 0];
export function scatterFlora(S, f, level, ix, iy, density, out, cx, cy, cz) {
  if (!S.flora.length) return 0;
  const n = 1 << level, size = nodeSize(S, level), area = size * size;
  const r = rng(vhash(S.seeds[7], f, level, VDOM.FLORA, (ix * 4099 + iy) & 0xffff) ^ (ix * 0x9E3779B1) ^ (iy * 0x85EBCA77));
  const ex = { h: 0, mo: 0, m1: 0, m2: 0 }, cap = out.length / FLORA_STRIDE;
  let k = 0;
  for (const fk of S.flora) {
    let want = fk.dens * area * density;
    let cnt = Math.floor(want); if (r() < want - cnt) cnt++;
    for (let j = 0; j < cnt && k < cap; j++) {
      const u = -1 + 2 * (ix + r()) / n, v = -1 + 2 * (iy + r()) / n;
      cubeDir(f, u, v, FL);
      const e = elevation(S, FL[0], FL[1], FL[2], ex);
      if (!floraOk(S, fk.kind, e, ex, r)) continue;
      // slope check with a second sample ~2 m away
      cubeDir(f, u + 4 / (S.R * Math.PI / 2) * (r() < 0.5 ? 1 : -1), v, FL2);
      const e2 = elevation(S, FL2[0], FL2[1], FL2[2]);
      const slope = Math.abs(e2 - e) / 2;
      if (slope > (fk.kind === 'rock' || fk.kind === 'basalt' || fk.kind === 'shard' ? 1.4 : fk.kind === 'hoodoo' || fk.kind === 'spire' || fk.kind === 'icespire' ? 0.6 : 0.7)) continue;
      const rr = S.R + e - (fk.kind === 'rib' ? 3 : 0.25);
      const o = k * FLORA_STRIDE;
      out[o] = FL[0] * rr - cx; out[o + 1] = FL[1] * rr - cy; out[o + 2] = FL[2] * rr - cz;
      const t = Math.pow(r(), 1.8);
      out[o + 3] = fk.s0 + (fk.s1 - fk.s0) * t;
      out[o + 4] = r() * Math.PI * 2; out[o + 5] = fk.id; out[o + 6] = r(); out[o + 7] = (r() - 0.5) * 0.3 + slope * 0.2;
      k++;
    }
  }
  return k;
}
function floraOk(S, kind, e, ex, r) {
  const wet = S.sea > 0;
  if (wet && e < (kind === 'coral' ? -1.2 : 0.8)) return false;
  if (kind === 'coral') return e < 3.5 && r() < 0.9;
  const hi = S.A ? e / S.A : 0;
  switch (kind) {
    case 'palm': return e > 1.5 && e < S.A * 0.35 && ex.mo > 0.35;
    case 'spiral': return ex.mo > 0.42 + hi * 0.3 && hi < 0.7;
    case 'fern': return ex.mo > 0.3 && hi < 0.8;
    case 'glowpod': return ex.mo > 0.5;
    case 'shrub': return ex.mo > 0.38 && r() < 0.8;
    case 'rib': case 'spire': return ex.m1 > 0.4 || kind === 'spire';
    case 'icespire': return hi > 0.08;
    case 'frost': return hi < 0.6;
    case 'ember': return ex.m1 > 0.2 || hi < 0.15;
    case 'deadtree': return hi < 0.5;
    case 'lattice': return ex.m1 < 0.5;
    default: return true;
  }
}

// ---- GLSL: the macro field and the world colour ramp (the orbital bake and the terrain shader both use them) ---------
// Uniforms the includer provides: uvec4 uSeedA (seeds 0..3); float uF, uMntK, uSea, uType; vec3 r0..r4, uOcean
export const MACRO_GLSL = NOISE_GLSL + /* glsl */`
// returns vec4(h01, moist, mountain ridge, continent)
vec4 czMacro(vec3 n) {
  vec3 p = n * uF;
  vec3 q = vec3(czFbm(p * 0.8, 3, uSeedA.x, 0.5), czFbm(p * 0.8 + vec3(5.2, 1.3, 2.8), 3, uSeedA.x ^ 0x51u, 0.5), czFbm(p * 0.8 + vec3(9.7, 4.1, 6.3), 3, uSeedA.x ^ 0xA3u, 0.5));
  float c = czFbm(p + q * 0.9, 6, uSeedA.y, 0.5);
  float mn = czRidged(p * 1.9 + q * 0.6, 5, uSeedA.z, 0.5);
  float h = 0.5 + c * 0.75;
  h += mn * uMntK * smoothstep(0.48, 0.78, h) * 0.32;
  float mo = czFbm(p * 1.3 + vec3(11.0, 3.0, 7.0), 4, uSeedA.w, 0.5) * 0.5 + 0.5;
  return vec4(clamp(h, 0.0, 1.0), mo, mn, c);
}
vec3 czRamp(float t) {
  t = clamp(t, 0.0, 1.0) * 4.0;
  if (t < 1.0) return mix(r0, r1, t); if (t < 2.0) return mix(r1, r2, t - 1.0); if (t < 3.0) return mix(r2, r3, t - 2.0); return mix(r3, r4, t - 3.0);
}
// the colour of the ground from far away (latitude = n.y of the planet frame); w = 1 where there is open liquid
vec3 czAlbedo(vec4 M, float lat, out float wet) {
  float h = M.x, mo = M.y; wet = 0.0;
  float polar = smoothstep(0.62, 0.92, abs(lat) + (mo - 0.5) * 0.35);
  vec3 alb;
  if (uSea > 0.0 && h < uSea) {
    float dep = (uSea - h) / max(uSea, 0.01);
    alb = mix(uOcean * 1.6, uOcean * 0.55, smoothstep(0.0, 0.6, dep)); wet = 1.0;
    if (uType > 2.5 && uType < 3.5) { alb = mix(alb, vec3(0.78, 0.86, 0.95), 0.75); wet = 0.3; }
    if (uType > 4.5 && uType < 5.5) { alb = vec3(0.06, 0.03, 0.02); wet = 0.0; }
  } else {
    float t = uSea > 0.0 ? (h - uSea) / (1.0 - uSea) : h;
    alb = czRamp(t * 0.85 + mo * 0.25 - 0.05);
    if (uType > 4.5 && uType < 5.5) alb = mix(alb, vec3(0.05), 0.4);
  }
  if (uType < 4.5 || uType > 6.5) alb = mix(alb, vec3(0.92, 0.95, 1.0), polar * (uType > 0.5 && uType < 1.5 ? 0.25 : 0.9));
  return alb;
}
`;
