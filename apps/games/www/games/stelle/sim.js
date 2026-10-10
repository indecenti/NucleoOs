// stelle/sim.js — Costellazioni space combat: a fixed-step (60 Hz) 6DOF arcade-sim.
//
// Pure logic, no Three.js, no DOM: the renderer reads the pools below and interpolates between the
// previous and current step (ppos/pq -> pos/q); the HUD reads F.player / F.obj / the event ring.
// Nothing is allocated per step: ships, bolts, missiles, flares, pickups and events live in fixed
// pools created once per flight. Conventions: metres, seconds, Y up, a ship's nose is local -Z.
//
// One flight model for everyone (player, wingmen, enemies, capital ships): pitch/yaw/roll rates
// with a throttle sweet spot, boost, drift. The AI drives ships through the same controls the
// player uses, so what it can do is what you can do — readable, fair, and it looks like flying.
import { rng, vhash, VDOM } from './world.js';

export const DT = 1 / 60;
export const TEAM_P = 0, TEAM_E = 1, TEAM_N = 2;
export const F_GILDA = 0, F_CUSTODI = 1, F_RELITTI = 2, F_ECO = 3, F_PLAYER = 4;

// event ring types (renderer / HUD / audio consume them by cursor)
export const EV = {
  SHOT: 1, HIT: 2, SHIELD: 3, SPARK: 4, KILL: 5, MFIRE: 6, MHIT: 7, FLARE: 8, LOCK: 9, WARPIN: 10,
  WARPOUT: 11, TETHER: 12, UNTETHER: 13, PICKUP: 14, COMMS: 15, OBJ: 16, SUBSYS: 17, PHURT: 18,
  BOOST: 19, OVERHEAT: 20, CAPKILL: 21, BOARD: 22, PIPS: 23, TARGET: 24, WAVE: 25, END: 26, BUMP: 27,
  CHAIN: 28, LOCKING: 29, NOAMMO: 30, CRUISE: 31,
};

// ---- ship classes ------------------------------------------------------------------------------
// guns/eng/tether/hsph/sub positions are in local metres (nose = -Z); the procedural kit builds the
// meshes around exactly these points so muzzle flashes, plumes and turrets line up.
//   eng: [x, y, z, plume size]   hsph: hit spheres [x, y, z, r]   sub: subsystems (capitals)
export const CLS = {
  lucciola: { name: 'Lucciola', role: 'fighter', len: 13, rad: 7.5, maxSpd: 165, acc: 78, boostSpd: 292, pr: 1.8, yr: 1.25, rr: 3.1, resp: 7.5, grip: 2.8,
    hull: 1, shield: 1, armor: 0, guns: [[-1.5, -0.95, -6.5], [1.5, -0.95, -6.5]], eng: [[-4.35, 0.9, 4.6, 1.15], [4.35, 0.9, 4.6, 1.15]], rate: 8.5, bolt: 1150, heat: 0.085, msl: 4, flares: 3 },
  lancer: { name: 'Lancer', fac: F_GILDA, role: 'fighter', len: 16, rad: 7, maxSpd: 176, acc: 86, boostSpd: 298, pr: 1.9, yr: 1.3, rr: 3.4, resp: 8, grip: 3,
    hull: 0.66, shield: 0.42, armor: 0, guns: [[-1.25, -0.55, -6.9], [1.25, -0.55, -6.9]], eng: [[0, 0.05, 7.9, 1.55]], rate: 6.2, bolt: 1060, dmg: 0.8, skill: 0.55 },
  bastion: { name: 'Bastion', fac: F_GILDA, role: 'heavy', len: 15, rad: 9.5, maxSpd: 134, acc: 56, boostSpd: 228, pr: 1.25, yr: 0.92, rr: 2.2, resp: 5.6, grip: 2.4,
    hull: 1.35, shield: 0.75, armor: 0.1, guns: [[-3.3, -0.75, -6.2], [3.3, -0.75, -6.2]], eng: [[-4.7, 0, 6.6, 1.3], [4.7, 0, 6.6, 1.3]], rate: 4, bolt: 980, dmg: 1.3, heavyBolt: true, skill: 0.6, msl: 2, flares: 2 },
  scrapwing: { name: 'Scrapwing', fac: F_RELITTI, role: 'fighter', len: 10, rad: 5.5, maxSpd: 168, acc: 92, boostSpd: 262, pr: 2.05, yr: 1.4, rr: 3.8, resp: 8.5, grip: 3.2,
    hull: 0.55, shield: 0, armor: 0, guns: [[0.95, -0.5, -4.7]], eng: [[-0.45, 0.1, 5, 1.1]], rate: 5, bolt: 1000, dmg: 0.65, skill: 0.42 },
  harpoon: { name: 'Harpoon', fac: F_RELITTI, role: 'tether', len: 14, rad: 7.5, maxSpd: 154, acc: 72, boostSpd: 250, pr: 1.6, yr: 1.15, rr: 2.8, resp: 7, grip: 2.8,
    hull: 1.15, shield: 0, armor: 0.15, guns: [[0, -1.2, -6.3]], eng: [[-1.9, 0, 6.5, 1.1], [1.9, 0, 6.5, 1.1]], rate: 3.2, bolt: 980, dmg: 0.8, skill: 0.5, tether: [0, 0, -8.4] },
  gutter: { name: 'Gutter', fac: F_RELITTI, role: 'board', len: 12, rad: 6.5, maxSpd: 150, acc: 76, boostSpd: 240, pr: 1.5, yr: 1.1, rr: 2.8, resp: 7, grip: 2.8,
    hull: 0.95, shield: 0, armor: 0.2, guns: [], eng: [[-1.6, 0, 5.9, 1.0], [1.6, 0, 5.9, 1.0]], rate: 0, skill: 0.4 },
  votive: { name: 'Votive', fac: F_CUSTODI, role: 'fighter', len: 12, rad: 6.5, maxSpd: 172, acc: 84, boostSpd: 290, pr: 1.9, yr: 1.3, rr: 3.4, resp: 8, grip: 3,
    hull: 0.75, shield: 0.7, armor: 0, guns: [[-1.45, 0, -5.2], [1.45, 0, -5.2]], eng: [[0, 0, 6.1, 1.35]], rate: 6.2, bolt: 1060, dmg: 0.8, skill: 0.55 },
  shard: { name: 'Shard', fac: F_ECO, role: 'fighter', len: 8, rad: 4.8, maxSpd: 186, acc: 105, boostSpd: 300, pr: 2.3, yr: 1.6, rr: 4, resp: 9, grip: 3.6,
    hull: 0.45, shield: 0.35, armor: 0, guns: [[0, 0, -4.2]], eng: [[0, 0, 3.6, 0.9]], rate: 4.5, bolt: 1150, dmg: 0.6, skill: 0.5 },
  hauler: { name: 'Hauler', role: 'hauler', len: 92, rad: 48, maxSpd: 58, acc: 10, boostSpd: 58, pr: 0.2, yr: 0.16, rr: 0.3, resp: 1.4, grip: 1.4, cap: true,
    hull: 9, shield: 3, armor: 0.25, guns: [], eng: [[-7, 0, 47, 4.5], [7, 0, 47, 4.5]],
    hsph: [[0, 1, -36, 12], [0, 0, -14, 14], [0, 0, 8, 14], [0, 0, 30, 14], [0, 0, 44, 10]], sub: [{ k: 'engine', p: [0, 0, 45], r: 12, hp: 3 }] },
  warden: { name: 'Warden', fac: F_GILDA, role: 'capital', len: 132, rad: 68, maxSpd: 46, acc: 7, boostSpd: 46, pr: 0.16, yr: 0.13, rr: 0.25, resp: 1.2, grip: 1.2, cap: true,
    hull: 17, shield: 6, armor: 0.2, guns: [], eng: [[-9, 0, 66, 6], [9, 0, 66, 6], [0, 7, 66, 4.5]],
    hsph: [[0, 0, -52, 11], [0, 0, -30, 15], [0, 1, -6, 17], [0, 2, 20, 17], [0, 3, 44, 15], [0, 6, 60, 11]],
    sub: [{ k: 'turret', p: [0, 11, -36], r: 6, hp: 2.2 }, { k: 'turret', p: [0, 13, 4], r: 6, hp: 2.2 }, { k: 'turret', p: [0, -11, -22], r: 6, hp: 2.2, down: true },
      { k: 'turret', p: [0, -12, 26], r: 6, hp: 2.2, down: true }, { k: 'shield', p: [0, 16, 32], r: 7, hp: 3 }, { k: 'engine', p: [0, 2, 64], r: 11, hp: 3 }, { k: 'bridge', p: [0, 17, 48], r: 7, hp: 3.5, weak: true }] },
  hulk: { name: 'Hulk', fac: F_RELITTI, role: 'capital', len: 190, rad: 96, maxSpd: 34, acc: 6, boostSpd: 34, pr: 0.12, yr: 0.1, rr: 0.2, resp: 1, grip: 1, cap: true,
    hull: 22, shield: 0, armor: 0.25, guns: [], eng: [[-14, -6, 94, 7], [14, -6, 94, 7], [0, 10, 94, 6]], carrier: true,
    hsph: [[0, 0, -78, 16], [0, 0, -50, 22], [0, 0, -18, 24], [6, 0, 14, 24], [0, 0, 46, 24], [0, 0, 76, 20]],
    sub: [{ k: 'turret', p: [-16, 22, -60], r: 7, hp: 2.2 }, { k: 'turret', p: [18, 24, -10], r: 7, hp: 2.2 }, { k: 'turret', p: [-20, 24, 30], r: 7, hp: 2.2 },
      { k: 'turret', p: [0, -24, -30], r: 7, hp: 2.2, down: true }, { k: 'turret', p: [14, -22, 50], r: 7, hp: 2.2, down: true },
      { k: 'engine', p: [0, 0, 90], r: 14, hp: 3 }, { k: 'reactor', p: [0, 0, 46], r: 10, hp: 3.5, weak: true }] },
  station: { name: 'Station', role: 'static', len: 400, rad: 300, maxSpd: 0, acc: 0, boostSpd: 0, pr: 0, yr: 0, rr: 0, resp: 1, grip: 1, cap: true, hull: 60, shield: 0, armor: 0.4, guns: [], eng: [] },
  beacon: { name: 'Beacon', role: 'static', len: 260, rad: 140, maxSpd: 0, acc: 0, boostSpd: 0, pr: 0, yr: 0, rr: 0, resp: 1, grip: 1, cap: true, hull: 34, shield: 0, armor: 0.3, guns: [], eng: [] },
};
export const SUB_KINDS = ['turret', 'shield', 'engine', 'reactor', 'bridge'];
// the named aces of the cast (lore.md; same callsigns as the firmware): 0 Dax Oren (Gilda), 1 Sister Vigil
// (Custodi), 2 Scarlet Gutter, 3 Warden of the Dark (Echo), 4 One-Eye Bram (Relitti). Signature trick each.
export const ACE_NAMES = ['Lancer Prime Dax Oren', 'Sister Vigil', 'Scarlet Gutter', 'Warden of the Dark', 'One-Eye Bram'];
const ACE_TRICK = [1, 3, 0, 2, 2];   // 0 afterburner jink, 1 missile volley, 2 holo decoys, 3 shield surge

// ---- tiny vector / quaternion helpers (allocation-free; operate on {x,y,z[,w]}) -----------------
const v3 = (x = 0, y = 0, z = 0) => ({ x, y, z });
const q4 = () => ({ x: 0, y: 0, z: 0, w: 1 });
const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
const sstep = (a, b, x) => { const t = clamp((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); };
const T = { x: 0, y: 0, z: 0 }, T2 = { x: 0, y: 0, z: 0 }, T3 = { x: 0, y: 0, z: 0 }, LD = { x: 0, y: 0, z: 0 };
// rotate v by quaternion q -> out
export function qrot(q, x, y, z, out) {
  const ix = q.w * x + q.y * z - q.z * y, iy = q.w * y + q.z * x - q.x * z, iz = q.w * z + q.x * y - q.y * x, iw = -q.x * x - q.y * y - q.z * z;
  out.x = ix * q.w + iw * -q.x + iy * -q.z - iz * -q.y;
  out.y = iy * q.w + iw * -q.y + iz * -q.x - ix * -q.z;
  out.z = iz * q.w + iw * -q.z + ix * -q.y - iy * -q.x;
  return out;
}
// inverse-rotate (world -> local)
export function qinv(q, x, y, z, out) {
  const qx = -q.x, qy = -q.y, qz = -q.z, qw = q.w;
  const ix = qw * x + qy * z - qz * y, iy = qw * y + qz * x - qx * z, iz = qw * z + qx * y - qy * x, iw = -qx * x - qy * y - qz * z;
  out.x = ix * qw + iw * -qx + iy * -qz - iz * -qy;
  out.y = iy * qw + iw * -qy + iz * -qx - ix * -qz;
  out.z = iz * qw + iw * -qz + ix * -qy - iy * -qx;
  return out;
}
function qnorm(q) { const l = Math.hypot(q.x, q.y, q.z, q.w) || 1; q.x /= l; q.y /= l; q.z /= l; q.w /= l; }
// body-frame angular integration: q = q * dq(w*dt)
function qintegrate(q, wx, wy, wz, dt) {
  const hx = wx * dt * 0.5, hy = wy * dt * 0.5, hz = wz * dt * 0.5;
  const x = q.x, y = q.y, z = q.z, w = q.w;
  q.x = w * hx + x + y * hz - z * hy;
  q.y = w * hy + y + z * hx - x * hz;
  q.z = w * hz + z + x * hy - y * hx;
  q.w = w - x * hx - y * hy - z * hz;
  qnorm(q);
}
// orientation looking along dir (world) with an up hint
export function qlook(q, dx, dy, dz, ux = 0, uy = 1, uz = 0) {
  let l = Math.hypot(dx, dy, dz) || 1; const fx = -dx / l, fy = -dy / l, fz = -dz / l;   // local +Z = -forward
  let rx = uy * fz - uz * fy, ry = uz * fx - ux * fz, rz = ux * fy - uy * fx; l = Math.hypot(rx, ry, rz);
  if (l < 1e-6) { rx = 1; ry = 0; rz = 0; l = 1; }
  rx /= l; ry /= l; rz /= l;
  const ax = fy * rz - fz * ry, ay = fz * rx - fx * rz, az = fx * ry - fy * rx;   // true up = z × x
  const m00 = rx, m01 = ax, m02 = fx, m10 = ry, m11 = ay, m12 = fy, m20 = rz, m21 = az, m22 = fz, tr = m00 + m11 + m22;
  if (tr > 0) { const s = 0.5 / Math.sqrt(tr + 1); q.w = 0.25 / s; q.x = (m21 - m12) * s; q.y = (m02 - m20) * s; q.z = (m10 - m01) * s; }
  else if (m00 > m11 && m00 > m22) { const s = 2 * Math.sqrt(1 + m00 - m11 - m22); q.w = (m21 - m12) / s; q.x = 0.25 * s; q.y = (m01 + m10) / s; q.z = (m02 + m20) / s; }
  else if (m11 > m22) { const s = 2 * Math.sqrt(1 + m11 - m00 - m22); q.w = (m02 - m20) / s; q.x = (m01 + m10) / s; q.y = 0.25 * s; q.z = (m12 + m21) / s; }
  else { const s = 2 * Math.sqrt(1 + m22 - m00 - m11); q.w = (m10 - m01) / s; q.x = (m02 + m20) / s; q.y = (m12 + m21) / s; q.z = 0.25 * s; }
  qnorm(q); return q;
}
function firstAlive(list) { for (let i = 0; i < list.length; i++) if (list[i].alive) return list[i]; return null; }
function countAlive(list) { let n = 0; for (let i = 0; i < list.length; i++) if (list[i].alive) n++; return n; }
const cpy = (a, b) => { a.x = b.x; a.y = b.y; a.z = b.z; return a; };
const dist2 = (a, b) => { const x = a.x - b.x, y = a.y - b.y, z = a.z - b.z; return x * x + y * y + z * z; };
// forward vector (local -Z) of a ship
const fwd = (s, out) => qrot(s.q, 0, 0, -1, out);

// ---- power distribution ------------------------------------------------------------------------
// 6 pips over engines / weapons / shields, 0..4 each; keys 1/2/3 pull a pip, 4 resets to 2/2/2.
const ENG_SPD = [0.8, 0.9, 1.0, 1.1, 1.2], ENG_BOOST = [0.45, 0.75, 1, 1.4, 1.85];
const WPN_COOL = [0.5, 0.78, 1, 1.32, 1.65], WPN_DMG = [0.86, 0.94, 1, 1.08, 1.18];
const SHD_REGEN = [0, 0.5, 1, 1.55, 2.1], SHD_CAP = [1, 1, 1, 1.15, 1.35];   // 4 pips overcharge shields
export function pipsAdd(p, sys) {
  if (sys === 3) { p[0] = 2; p[1] = 2; p[2] = 2; return true; }
  if (p[sys] >= 4) return false;
  const a = (sys + 1) % 3, b = (sys + 2) % 3, from = p[a] > p[b] ? a : p[b] > p[a] ? b : (p[a] > 0 ? a : b);
  if (p[from] <= 0) return false;
  p[from]--; p[sys]++; return true;
}

// ---- the flight ----------------------------------------------------------------------------------
const NSHIP = 72, NBOLT = 640, NMSL = 40, NFLARE = 32, NPICK = 12, NEV = 512;
const ST = { FORM: 0, ATTACK: 1, EVADE: 2, EXTEND: 3, TETHER: 4, BOARD: 5, CAP: 6, FLEE: 7, DARK: 8, ESCORT: 9, PATH: 10, WARP: 11, STATIC: 12, HOLD: 13 };
export { ST };

function mkShip(i) {
  return {
    i, gen: 0, alive: false, cls: null, ck: '', team: 0, fac: 0, isPlayer: false, cap: false,
    pos: v3(), vel: v3(), q: q4(), w: v3(), ppos: v3(), pq: q4(),
    spd: 0, thr: 0.5, boost: 1, boosting: false, boostLock: false, drift: false, spdCap: 1,
    hull: 1, hullMax: 1, shF: 0, shB: 0, shMax: 0, shRegT: 0, armor: 0, shFocus: 0,
    heat: 0, overT: 0, fireCd: 0, gunI: 0, burst: 0, burstCd: 0, msl: 0, mslCd: 0, lockT: 0, lockTgt: null, flares: 0, flareCd: 0,
    cp: 0, cy: 0, cr: 0, fire: false,
    target: null, lastHitBy: null, lastHitT: -99, hitT: -99, hullHitT: -99, dmgShown: 0,
    st: 0, stT: 0, thinkT: 0, aimJx: 0, aimJy: 0, evDir: 1, wantSpd: 1, home: v3(),
    squad: null, slot: 0, ace: 0, trick: 0, trickT: 0, nameKey: '', name: '', skill: 0.5, dmgMul: 1, value: 0,
    hs: null, sub: null, subHp: null, subMax: null, turAim: null, turCd: null, shGen: true, engOk: true,
    tether: null, tetherT: 0, breakT: 0, tetheredBy: null, board: null, boardT: 0, carryT: 0,
    warpT: 0, darkWake: 0, decoy: false, fleeT: 0, kills: 0, path: null, pathI: 0, spawnT: 0, objective: false, dieT: 0,
    blink: 0, evadeCd: 0,
  };
}

export function createFlight(opts) {
  const { bp, cc, run } = opts;
  const seedKey = vhash(opts.seed >>> 0, opts.sector >>> 0, opts.sys >>> 0, VDOM.MISSION, (opts.slot | 0) & 0xffff);
  const R = rng(seedKey);
  const F = {
    t: 0, step: 0, outcome: 0, done: false, endT: 0, hitStop: 0, slow: 0, slowK: 1, acc: 0, alpha: 0,
    ships: [], bolts: [], msls: [], flares: [], picks: [], ev: [], evHead: 0,
    player: null, kills: 0, earned: 0, obj: { key: '', a: {}, marker: null, bars: [] },
    stats: { shots: 0, hits: 0, dmgOut: 0, dmgIn: 0, aceKill: 0, capKill: 0, t0: 0 },
    input: { pitch: 0, yaw: 0, roll: 0, thr: 0.55, thrRate: 0, boost: false, drift: false, fire: false, missile: false, aim: null, aimMode: 0 },
    bp, cc, run, R, mission: null, squads: [], wave: 0, waves: 0, enemyLeft: 0, comms: 0, autopilot: !!opts.autopilot, god: !!opts.god, timeScale: Math.max(1, Math.min(16, opts.timeScale | 0 || 1)),
    grid: null, obs: null, retreat: 0, boltI: 0, cruise: 0, cruiseT: 0, cruiseV: 0, briefT: opts.brief === false ? 0 : 3.2, paused: false, tier: clamp(1 + (opts.sector | 0), 1, 12),
  };
  for (let i = 0; i < NSHIP; i++) F.ships.push(mkShip(i));
  for (let i = 0; i < NBOLT; i++) F.bolts.push({ alive: false, x: 0, y: 0, z: 0, vx: 0, vy: 0, vz: 0, px: 0, py: 0, pz: 0, life: 0, dmg: 0, team: 0, own: null, fac: 0, kind: 0, len: 18 });
  for (let i = 0; i < NMSL; i++) F.msls.push({ alive: false, gen: 0, pos: v3(), ppos: v3(), vel: v3(), tgt: null, tgtGen: 0, flare: null, life: 0, team: 0, own: null, dmg: 0, spd: 0, armT: 0 });
  for (let i = 0; i < NFLARE; i++) F.flares.push({ alive: false, pos: v3(), vel: v3(), life: 0, team: 0 });
  for (let i = 0; i < NPICK; i++) F.picks.push({ alive: false, pos: v3(), vel: v3(), kind: 0, life: 0, spin: 0 });
  for (let i = 0; i < NEV; i++) F.ev.push({ n: 0, t: 0, x: 0, y: 0, z: 0, a: 0, b: 0, c: 0, s: null, s2: null, k: '', v: null });
  buildObstacles(F);
  // enemy scaling from the shared mission numbers (constellations-gen.js), unchanged contract
  F.E = { hp: cc.foeHp, dmg: cc.foeDmg, spd: clamp(0.93 + ((cc.foeSpeed || 82) - 82) / 45 * 0.012, 0.9, 1.1) };
  // player ship
  const p = spawn(F, 'lucciola', TEAM_P, F_PLAYER, 0, 0, 0, 0, 0, -1);
  p.isPlayer = true; F.player = p;
  p.hullMax = Math.max(1, run.hull_max); p.hull = clamp(run.hull, 1, p.hullMax);
  p.shMax = Math.max(10, run.shield_max); p.shF = p.shB = p.shMax / 2;
  p.msl = CLS.lucciola.msl; p.flares = CLS.lucciola.flares; p.spd = 70; p.thr = 0.55; F.input.thr = 0.55;
  qlook(p.q, 0, 0, -1); vset(p.vel, 0, 0, -70);
  p.pips = [2, 2, 2]; p.wpnDmg = 7.5 + (run.weapon | 0) * 2.4; p.lockTime = clamp(1.6 - (run.sensors | 0) * 0.18, 0.75, 1.6);
  p.dmgMul = 1; p.skill = 0.8; p.nameKey = 'cz_ship_player';
  setupMission(F);
  return F;
}
const vset = (v, x, y, z) => { v.x = x; v.y = y; v.z = z; return v; };

// static obstacles: asteroids + station (unless it is a defended entity) + beacon, in a hashed grid
function buildObstacles(F) {
  const bp = F.bp, f = bp.field, list = [];
  for (let i = 0; i < f.n; i++) list.push(f.x[i], f.y[i], f.z[i], f.r[i] * 0.86, 0);
  const defStation = F.cc.type === 3 && !(bp.beacon && F.cc.arch === 5) && bp.station;
  if (bp.station && !defStation) for (const c of bp.station.collide) list.push(bp.station.pos[0] + c[0], bp.station.pos[1] + c[1], bp.station.pos[2] + c[2], c[3], 1);
  if (bp.beacon && !(F.cc.type === 3 && F.cc.arch === 5)) { const b = bp.beacon; list.push(b.pos[0], b.pos[1] + b.height * 0.5, b.pos[2], b.radius, 2); }
  const n = list.length / 5, x = new Float32Array(n), y = new Float32Array(n), z = new Float32Array(n), r = new Float32Array(n), kind = new Uint8Array(n);
  for (let i = 0; i < n; i++) { x[i] = list[i * 5]; y[i] = list[i * 5 + 1]; z[i] = list[i * 5 + 2]; r[i] = list[i * 5 + 3]; kind[i] = list[i * 5 + 4]; }
  F.obs = { n, x, y, z, r, kind };
  const CELL = 200, grid = new Map();
  const key = (a, b, c) => ((a + 512) * 1024 + (b + 512)) * 1024 + (c + 512);
  for (let i = 0; i < n; i++) {
    const rr = r[i] + 30;
    const x0 = Math.floor((x[i] - rr) / CELL), x1 = Math.floor((x[i] + rr) / CELL), y0 = Math.floor((y[i] - rr) / CELL), y1 = Math.floor((y[i] + rr) / CELL), z0 = Math.floor((z[i] - rr) / CELL), z1 = Math.floor((z[i] + rr) / CELL);
    for (let a = x0; a <= x1; a++) for (let b = y0; b <= y1; b++) for (let c = z0; c <= z1; c++) { const k = key(a, b, c); let l = grid.get(k); if (!l) grid.set(k, l = []); l.push(i); }
  }
  F.grid = grid; F.cell = CELL; F.gkey = key;
}
const EMPTY = [];
function cellList(F, x, y, z) { const C = F.cell; return F.grid.get(F.gkey(Math.floor(x / C), Math.floor(y / C), Math.floor(z / C))) || EMPTY; }

// ---- events ------------------------------------------------------------------------------------
function ev(F, n, x = 0, y = 0, z = 0) {
  const e = F.ev[F.evHead % NEV]; F.evHead++;
  e.n = n; e.t = F.t; e.x = x; e.y = y; e.z = z; e.a = 0; e.b = 0; e.c = 0; e.s = null; e.s2 = null; e.k = ''; e.v = null;
  return e;
}
function comms(F, key, who, vars, prio = 1) {
  const e = ev(F, EV.COMMS); e.k = key; e.s = who || null; e.v = vars || null; e.a = prio;
}

// ---- spawning --------------------------------------------------------------------------------------
function spawn(F, ck, team, fac, x, y, z, dx, dy, dz) {
  let s = null;
  for (const c of F.ships) if (!c.alive && c.dieT <= 0) { s = c; break; }
  if (!s) for (const c of F.ships) if (!c.alive) { s = c; break; }
  if (!s) return null;
  const C = CLS[ck];
  Object.assign(s, mkShip(s.i), { gen: s.gen + 1 });
  s.alive = true; s.cls = C; s.ck = ck; s.team = team; s.fac = fac; s.cap = !!C.cap;
  vset(s.pos, x, y, z); cpy(s.ppos, s.pos); qlook(s.q, dx, dy, dz); s.pq.x = s.q.x; s.pq.y = s.q.y; s.pq.z = s.q.z; s.pq.w = s.q.w;
  const E = F.E || { hp: 50, dmg: 10, spd: 1 };
  s.hullMax = s.hull = Math.max(1, Math.round(E.hp * C.hull));
  s.shMax = Math.round(E.hp * C.shield); s.shF = s.shB = s.shMax / 2; s.armor = C.armor || 0;
  s.msl = C.msl || 0; s.flares = C.flares || 0; s.skill = C.skill || 0.5; s.dmgMul = C.dmg || 1;
  s.spd = C.maxSpd * 0.7; s.thr = 0.7; vset(s.vel, 0, 0, 0);
  s.hs = C.hsph || null; s.nameKey = ''; s.value = Math.round((F.cc.killCr || 20) * (C.cap ? 6 : C.hull >= 1.5 ? 1.6 : 1));
  if (C.sub) {
    s.sub = C.sub; s.subHp = new Float32Array(C.sub.length); s.subMax = new Float32Array(C.sub.length);
    s.turAim = []; s.turCd = new Float32Array(C.sub.length);
    for (let k = 0; k < C.sub.length; k++) { s.subMax[k] = s.subHp[k] = Math.round(E.hp * C.sub[k].hp); s.turAim.push(v3(0, C.sub[k].down ? -1 : 1, 0)); s.turCd[k] = 1 + k * 0.37; }
  }
  if (s.ppos) fwd(s, T);
  return s;
}
// nudge a freshly spawned ship out of any rock it landed in
function clearRocks(F, s) {
  const O = F.obs;
  for (let it = 0; it < 4; it++) {
    const list = cellList(F, s.pos.x, s.pos.y, s.pos.z); let moved = false;
    for (let j = 0; j < list.length; j++) {
      const i = list[j], dx = s.pos.x - O.x[i], dy = s.pos.y - O.y[i], dz = s.pos.z - O.z[i], R = O.r[i] + s.cls.rad + 12, d = Math.hypot(dx, dy, dz) || 1;
      if (d < R) { s.pos.x = O.x[i] + dx / d * R; s.pos.y = O.y[i] + dy / d * R; s.pos.z = O.z[i] + dz / d * R; moved = true; }
    }
    if (!moved) break;
  }
  cpy(s.ppos, s.pos);
}
function launchShip(s, speed) { fwd(s, T); s.spd = speed; vset(s.vel, T.x * speed, T.y * speed, T.z * speed); }

// a squad: leader + wingmen in formation
function squad(F, list, team, fac, x, y, z, dx, dy, dz, opt = {}) {
  const sq = { id: F.squads.length, team, fac, members: [], leader: null, form: opt.form || 'V', called: false, lost: 0 };
  const l = Math.hypot(dx, dy, dz) || 1; dx /= l; dy /= l; dz /= l;
  qlook(TQ, dx, dy, dz);
  list.forEach((ck, k) => {
    const o = FORM[sq.form][k % FORM.V.length];
    qrot(TQ, o[0], o[1], o[2], T3);
    const s = spawn(F, ck, team, fac, x + T3.x, y + T3.y, z + T3.z, dx, dy, dz);
    if (!s) return;
    s.squad = sq; s.slot = k; sq.members.push(s);
    s.st = opt.state != null ? opt.state : ST.ATTACK; s.stT = 0;
    launchShip(s, opt.speed != null ? opt.speed : CLS[ck].maxSpd * 0.8);
    if (opt.warp) { s.warpT = 1.1 + k * 0.12; s.st = ST.WARP; const e = ev(F, EV.WARPIN, s.pos.x, s.pos.y, s.pos.z); e.s = s; e.a = dx; e.b = dy; e.c = dz; }
    if (opt.dark) { s.st = ST.DARK; s.spd = 0; vset(s.vel, 0, 0, 0); s.thr = 0; clearRocks(F, s); }
    if (CLS[ck].cap) s.st = ST.CAP;
    if (opt.hp) { s.hullMax = s.hull = Math.round(s.hullMax * opt.hp); }
  });
  sq.leader = sq.members[0] || null;
  F.squads.push(sq);
  return sq;
}
const TQ = q4();
const FORM = {
  V: [[0, 0, 0], [-26, 2, 20], [26, -2, 20], [-52, 4, 40], [52, -4, 40], [0, 8, 46]],
  W: [[0, 0, 0], [-30, 0, 14], [30, 0, 14], [-60, 0, 28], [60, 0, 28], [0, -12, 34]],
  C: [[0, 0, 0], [-60, 10, 30], [60, -10, 30], [-20, 30, 80], [20, -30, 80], [0, 0, 120]],
};

// ---- mission staging ---------------------------------------------------------------------------------
// The shared generator gives waves × per_wave hostiles (built for the Cardputer's rail shooter); in
// 6DOF a sortie is fewer, smarter ships, so the totals are condensed into 2–5 engagements. Rewards
// and kill credits stay exactly the generator's numbers (constellations.js applies them).
function setupMission(F) {
  const cc = F.cc, bp = F.bp, R = F.R, tier = F.tier;
  // M1 rosters: Gilda and Relitti in full; Custodi patrols (reputation <= -25) fly Votives, Echo space wakes Shards
  const ef = cc.foeFac === F_GILDA || cc.foeFac === F_CUSTODI || cc.foeFac === F_ECO ? cc.foeFac : F_RELITTI;
  const offer = cc.repFac >= 0 ? cc.repFac : F_GILDA;
  const kind = cc.mission === false ? 'ambush' : cc.type === 1 ? (cc.arch === 2 ? 'duel' : 'hunt') : cc.type === 2 ? 'escort' : cc.type === 3 ? (cc.arch === 4 ? 'sweep' : 'defend') : 'patrol';
  const total = clamp(Math.round((cc.waves || 3) * (cc.perWave || 3) * 0.42), 6, 22);
  let nW = clamp(Math.round((cc.waves || 3) / 3), 2, 4);
  const M = F.mission = { kind, ef, offer, total, nW, waves: [], waveI: 0, nav: [], navI: 0, protect: [], ace: null, aceName: cc.targetName || '', gangKey: cc.gang != null ? 'cz_gang_' + cc.gang : '',
    route: null, jump: null, capital: null, startT: 0, timer: 0, ended: false, defended: null, swarm: kind === 'sweep' };
  const fighter = ef === F_GILDA ? 'lancer' : ef === F_CUSTODI ? 'votive' : ef === F_ECO ? 'shard' : 'scrapwing';
  const heavy = ef === F_GILDA ? 'bastion' : ef === F_CUSTODI ? 'votive' : ef === F_ECO ? 'shard' : 'harpoon';
  const capCls = ef === F_GILDA ? 'warden' : ef === F_RELITTI ? 'hulk' : null;
  const wingCls = offer === F_GILDA ? 'lancer' : offer === F_CUSTODI ? 'votive' : 'scrapwing';
  const p = F.player;
  // composition helper: n ships for wave w
  const comp = (n, w, extra = []) => {
    const out = extra.slice();
    const heavies = (tier >= 2 || w >= 1) ? Math.min(2, Math.floor(n / 3) + (w >= 2 ? 1 : 0)) : 0;
    for (let i = 0; i < heavies && out.length < n; i++) out.push(heavy);
    while (out.length < n) out.push(fighter);
    return out;
  };
  const perW = (k) => Math.max(2, Math.round(total / k));
  const fieldK = bp.field.knots;
  if (kind === 'patrol' || kind === 'ambush') {
    const navs = kind === 'ambush' ? 1 : 3;
    for (let i = 0; i < navs; i++) {
      const k = fieldK[(i + R.int(4)) % fieldK.length];
      const a = R.range(0, Math.PI * 2), d = R.range(300, 700);
      M.nav.push([k[0] + Math.cos(a) * d, k[1] + R.range(-120, 120), k[2] + Math.sin(a) * d]);
    }
    if (kind === 'ambush') { M.nav[0] = [p.pos.x + R.range(-400, 400), p.pos.y + R.range(-150, 150), p.pos.z - R.range(1300, 1700)]; nW = clamp(nW, 2, 3); }
    const per = perW(nW);
    for (let w = 0; w < nW; w++) {
      const navI = kind === 'ambush' ? 0 : Math.min(navs - 1, Math.floor(w * navs / nW));
      M.waves.push({ trig: w === 0 || (kind === 'patrol' && navI !== Math.min(navs - 1, Math.floor((w - 1) * navs / nW))) ? 'nav' : 'clear', nav: navI,
        ships: comp(per, w), how: (kind === 'ambush' && w === 0) || (ef === F_RELITTI && R() < 0.5) ? 'dark' : 'warp' });
    }
  } else if (kind === 'hunt' || kind === 'duel') {
    if (kind === 'duel') { nW = 1; M.waves.push({ trig: 'time', at: 2, ships: [], ace: fighter, how: 'warp' }); }
    else {
      const per = perW(nW);
      for (let w = 0; w < nW - 1; w++) M.waves.push({ trig: w === 0 ? 'time' : 'clear', at: 3, ships: comp(per, w), how: ef === F_RELITTI && w === 0 ? 'dark' : 'warp' });
      const last = comp(Math.max(2, per - 1), nW - 1); last.length = Math.min(last.length, 3);
      M.waves.push({ trig: 'clear', ships: last, ace: fighter, how: 'warp', cap: tier >= 5 ? capCls : null });
    }
  } else if (kind === 'escort') {
    // convoy: haulers of the offering faction run from near the start to a jump point ~6 km out
    const dir = [R.range(-0.4, 0.4), R.range(-0.1, 0.1), -1], dl = Math.hypot(...dir);
    const start = [p.pos.x + R.range(-150, 150), p.pos.y - 60, p.pos.z - 500];
    const end = [start[0] + dir[0] / dl * 6200, start[1] + dir[1] / dl * 6200, start[2] + dir[2] / dl * 6200];
    M.route = [start, [(start[0] + end[0]) / 2 + R.range(-900, 900), (start[1] + end[1]) / 2 + R.range(-200, 200), (start[2] + end[2]) / 2], end];
    M.jump = end;
    const nh = 2 + (tier >= 6 ? 1 : 0);
    for (let h = 0; h < nh; h++) {
      const s = spawn(F, 'hauler', TEAM_P, offer, start[0] + (h - (nh - 1) / 2) * 140, start[1] + h * 20, start[2] + h * 60, dir[0], dir[1], dir[2]);
      s.hullMax = s.hull = Math.round((400 + tier * 60) * (1 + h * 0.1)); s.shMax = Math.round(120 + tier * 20); s.shF = s.shB = s.shMax / 2;
      s.st = ST.PATH; s.path = M.route; s.pathI = 1; s.objective = true; launchShip(s, 40); s.nameKey = 'cz_ship_hauler'; s.name = String(h + 1);
      M.protect.push(s);
    }
    const per = perW(nW);
    for (let w = 0; w < nW; w++) M.waves.push({ trig: w === 0 ? 'time' : 'route', at: w === 0 ? 7 : 0, frac: (w + 0.4) / (nW + 0.4), ships: comp(per, w, ef === F_RELITTI ? (w === 0 ? ['harpoon'] : ['gutter', 'harpoon']) : []), how: 'warp' });
    if (tier >= 6) M.waves[nW - 1].cap = capCls;
  } else if (kind === 'defend' || kind === 'sweep') {
    let obj = null;
    if (kind === 'defend') {
      const useBeacon = cc.arch === 5 && bp.beacon;
      if (useBeacon) { const b = bp.beacon; obj = spawn(F, 'beacon', TEAM_P, offer, b.pos[0], b.pos[1] + b.height * 0.5, b.pos[2], 0, 0, -1); obj.hs = [[0, -b.height * 0.3, 0, b.radius * 0.8], [0, 0, 0, b.radius], [0, b.height * 0.3, 0, b.radius * 0.8]]; obj.nameKey = 'cz_ship_beacon'; }
      else if (bp.station) { const st = bp.station; obj = spawn(F, 'station', TEAM_P, offer, st.pos[0], st.pos[1], st.pos[2], 0, 0, -1); obj.hs = st.collide; obj.nameKey = 'cz_ship_station'; }
      if (obj) {
        obj.hullMax = obj.hull = Math.round((obj.ck === 'beacon' ? 2400 : 3200) + tier * 240); obj.st = ST.STATIC; obj.objective = true; obj.spd = 0; vset(obj.vel, 0, 0, 0);
        M.protect.push(obj); M.defended = obj;
      }
    }
    if (kind === 'sweep') nW = clamp(nW + 1, 3, 4);
    const per = perW(nW);
    for (let w = 0; w < nW; w++) {
      const ships = kind === 'sweep' ? new Array(Math.round(per * (ef === F_RELITTI ? 1.5 : 1.1))).fill(fighter) : comp(per, w, ef === F_RELITTI && w >= 1 ? ['gutter'] : []);
      M.waves.push({ trig: w === 0 ? 'time' : 'clear', at: 3, ships, how: 'warp', cap: kind === 'defend' && w === nW - 1 ? capCls : null });
    }
  }
  // wingmen from the offering faction (not in a duel, not on a free ambush)
  if (kind !== 'duel' && kind !== 'ambush') {
    const nWing = kind === 'escort' || kind === 'defend' ? 2 : 1;
    qrot(p.q, 0, 0, 1, T);
    const sq = squad(F, new Array(nWing).fill(wingCls), TEAM_P, offer, p.pos.x + 40, p.pos.y + 6, p.pos.z + 40, 0, 0, -1, { state: ST.FORM, form: 'V', speed: 70 });
    sq.leader = p; sq.wing = true;
    sq.members.forEach((s, k) => { s.slot = k + 1; s.nameKey = 'cz_cs_' + offer; s.name = String(k + 2); s.hullMax = s.hull = Math.round(s.hullMax * 1.6); });
    M.wing = sq;
  }
  F.waves = M.waves.length;
  M.objective = kind;
  updateObjective(F);
}

function spawnWave(F, w) {
  const M = F.mission, p = F.player, R = F.R, tier = F.tier;
  // anchor: around the objective (nav / convoy / defended object) or the player
  let ax = p.pos.x, ay = p.pos.y, az = p.pos.z;
  if (w.nav != null && M.nav[w.nav]) [ax, ay, az] = M.nav[w.nav];
  else if (M.protect.length) { const o = firstAlive(M.protect) || p; ax = o.pos.x; ay = o.pos.y; az = o.pos.z; }
  // direction biased into the player's forward hemisphere so arrivals read on screen
  fwd(p, T);
  const yaw = R.range(-1.1, 1.1), cy = Math.cos(yaw), sy = Math.sin(yaw);
  let dx = T.x * cy - T.z * sy, dz = T.x * sy + T.z * cy, dy = R.range(-0.25, 0.25);
  const dl = Math.hypot(dx, dy, dz) || 1; dx /= dl; dy /= dl; dz /= dl;
  const D = w.how === 'dark' ? 0 : R.range(1900, 2600);
  let cx = ax + dx * D, cyy = ay + dy * D, cz = az + dz * D;
  if (w.nav != null && w.how !== 'dark') { cx = ax + dx * 900; cyy = ay + dy * 300; cz = az + dz * 900; }
  const list = w.ships.slice();
  const groups = [];
  const sz = M.swarm ? 5 : 3;
  for (let i = 0; i < list.length; i += sz) groups.push(list.slice(i, i + sz));
  let made = 0;
  groups.forEach((g, gi) => {
    let gx = cx, gy = cyy, gz = cz;
    if (w.how === 'dark') {
      const k = w.nav != null && M.nav[w.nav] ? M.nav[w.nav] : F.bp.field.knots[(gi + R.int(4)) % 4];
      const a = R.range(0, Math.PI * 2), d = R.range(450, 850);
      gx = k[0] + Math.cos(a) * d; gy = k[1] + R.range(-90, 90); gz = k[2] + Math.sin(a) * d;
    }
    else { gx += R.range(-260, 260) * gi; gy += R.range(-120, 120); gz += R.range(-260, 260) * gi; }
    const sq = squad(F, g, TEAM_E, M.ef, gx, gy, gz, -dx, -dy, -dz, { warp: w.how !== 'dark', dark: w.how === 'dark', form: M.swarm ? 'W' : 'V' });
    made += sq.members.length;
    if (w.how === 'dark') sq.members.forEach((s) => { s.darkWake = 1300 + R.range(-200, 300); });
  });
  if (w.ace) {
    const s = spawn(F, w.ace, TEAM_E, M.ef, cx + dx * 120, cyy + 40, cz + dz * 120, -dx, -dy, -dz);
    if (s) {
      s.ace = 1; s.hullMax = s.hull = Math.round(F.E.hp * CLS[w.ace].hull * 2.4 + 40); s.shMax = Math.round(s.shMax * 1.6 + 20); s.shF = s.shB = s.shMax / 2;
      s.skill = 0.9; s.dmgMul *= 1.1; s.name = M.aceName; s.nameKey = 'cz_ship_ace'; s.value = (F.cc.killCr || 20) * 4; s.msl = 2;
      s.aceId = F.cc.aceId != null && F.cc.aceId >= 0 ? F.cc.aceId : -1; s.trick = s.aceId >= 0 ? ACE_TRICK[s.aceId] : R.int(4);
      s.st = ST.WARP; s.warpT = 1.4; launchShip(s, CLS[w.ace].boostSpd);
      const e = ev(F, EV.WARPIN, s.pos.x, s.pos.y, s.pos.z); e.s = s; e.a = -dx; e.b = -dy; e.c = -dz;
      M.ace = s; made++;
      comms(F, s.aceId >= 0 ? 'cz_c_acequip_' + s.aceId : 'cz_c_ace_' + (s.trick % 3), s, { name: s.name }, 3);
    }
  }
  if (w.cap) {
    const s = spawn(F, w.cap, TEAM_E, M.ef, cx + dx * 700, cyy + 120, cz + dz * 700, -dx, -dy, -dz);
    if (s) {
      s.st = ST.CAP; s.warpT = 2.2; launchShip(s, 140); s.nameKey = 'cz_ship_' + w.cap; s.value = (F.cc.killCr || 20) * 8;
      const e = ev(F, EV.WARPIN, s.pos.x, s.pos.y, s.pos.z); e.s = s; e.a = -dx; e.b = -dy; e.c = -dz; e.c2 = 1;
      M.capital = s; made++;
      comms(F, 'cz_c_capital', null, { name: CLS[w.cap].name }, 3);
    }
  }
  F.wave++;
  const e = ev(F, EV.WAVE); e.a = F.wave; e.b = F.waves;
  if (M.ef === F_ECO && F.wave === 1) comms(F, 'cz_c_echo_wake', null, { voice: 1 }, 3);   // the Voice speaks first
  else if (w.how === 'dark') comms(F, 'cz_c_ambush', null, null, 2);
  else comms(F, 'cz_c_contacts', null, { n: made }, 2);
  if (M.wing && firstAlive(M.wing.members) && F.R() < 0.7) comms(F, 'cz_c_wing_engage', firstAlive(M.wing.members));
}

function updateObjective(F) {
  const M = F.mission, O = F.obj, p = F.player;
  let left = 0; for (const s of F.ships) if (s.alive && s.team === TEAM_E && !s.decoy) left++;
  F.enemyLeft = left;
  O.bars.length = 0;
  for (const s of M.protect) O.bars.push(s);
  O.marker = null; O.a = O.a || {};
  if (F.outcome === 1) { O.key = 'cz_obj_complete'; return; }
  if (M.kind === 'patrol' || M.kind === 'ambush') {
    const pending = M.waves[M.waveI];
    if (pending && pending.trig === 'nav' && left === 0) { const n = M.nav[pending.nav]; O.key = 'cz_obj_nav'; O.a.n = pending.nav + 1; O.marker = n; O.a.d = n ? Math.hypot(n[0] - p.pos.x, n[1] - p.pos.y, n[2] - p.pos.z) : 0; }
    else { O.key = M.kind === 'ambush' ? 'cz_obj_survive' : 'cz_obj_clear'; O.a.n = left; }
  } else if (M.kind === 'hunt' || M.kind === 'duel') {
    if (M.ace && M.ace.alive) { O.key = 'cz_obj_bounty'; O.a.name = M.aceName; }
    else { O.key = 'cz_obj_clear'; O.a.n = left; }
  } else if (M.kind === 'escort') {
    const h = firstAlive(M.protect);
    O.key = 'cz_obj_escort'; O.marker = M.jump;
    O.a.d = h && M.jump ? Math.hypot(M.jump[0] - h.pos.x, M.jump[1] - h.pos.y, M.jump[2] - h.pos.z) : 0;
  } else if (M.kind === 'defend') {
    O.key = M.defended && M.defended.ck === 'beacon' ? 'cz_obj_defend_beacon' : 'cz_obj_defend_station'; O.a.w = Math.min(F.wave, F.waves); O.a.m = F.waves; O.a.n = left;
  } else if (M.kind === 'sweep') { O.key = 'cz_obj_swarm'; O.a.n = left; }
}

// ---- per-step update ------------------------------------------------------------------------------
export function frame(F, realDt) {
  if (F.paused || F.done) { F.alpha = 1; return 0; }
  let scale = 1;
  if (F.hitStop > 0) { F.hitStop -= realDt; scale = 0.06; }
  else if (F.slow > 0) { F.slow -= realDt; scale = F.slowK; }
  const ts = F.timeScale || 1;   // tests fast-forward (window.__czTimeScale); 1 in play
  F.acc += Math.min(0.1, realDt) * scale * ts;
  let n = 0;
  const maxN = 5 * ts;
  while (F.acc >= DT && n < maxN) { stepFlight(F); F.acc -= DT; n++; }
  if (n >= maxN) F.acc = 0;
  F.alpha = F.acc / DT;
  return n;
}

export function stepFlight(F) {
  F.t += DT; F.step++;
  const ships = F.ships;
  for (let i = 0; i < NSHIP; i++) { const s = ships[i]; if (s.alive || s.dieT > 0) { cpy(s.ppos, s.pos); s.pq.x = s.q.x; s.pq.y = s.q.y; s.pq.z = s.q.z; s.pq.w = s.q.w; } }
  for (const b of F.bolts) if (b.alive) { b.px = b.x; b.py = b.y; b.pz = b.z; }
  for (const m of F.msls) if (m.alive) cpy(m.ppos, m.pos);
  if (F.briefT > 0) F.briefT -= DT;
  // player controls
  const p = F.player;
  if (p.alive) playerControl(F, p);
  // AI
  for (let i = 0; i < NSHIP; i++) {
    const s = ships[i];
    if (!s.alive) { if (s.dieT > 0) s.dieT -= DT; continue; }
    if (s.isPlayer) continue;
    aiStep(F, s);
  }
  // physics
  for (let i = 0; i < NSHIP; i++) { const s = ships[i]; if (s.alive) flyShip(F, s); }
  weaponsStep(F);
  boltsStep(F);
  missilesStep(F);
  flaresStep(F);
  picksStep(F);
  directorStep(F);
}

// ---- flight model ------------------------------------------------------------------------------------
function turnMul(s) {
  if (s.drift) return 1.4;
  if (s.boosting) return 0.62;
  const t = s.thr;
  if (t < 0.35) return 0.86 + (t / 0.35) * 0.14;
  if (t <= 0.65) return 1;
  return 1 - (t - 0.65) / 0.35 * 0.32;
}
function flyShip(F, s) {
  const C = s.cls;
  if (s.st === ST.STATIC) { s.blink += DT; return; }
  if (s.board) { boardFollow(F, s); return; }
  const pips = s.pips;
  const engM = pips ? ENG_SPD[pips[0]] : 1, engB = pips ? ENG_BOOST[pips[0]] : 1;
  const tm = turnMul(s) * (s.engOk ? 1 : 0.6) * (s.isPlayer && F.cruise === 2 ? 0.32 : 1);
  // angular velocity toward the control target (first-order response)
  const k = 1 - Math.exp(-C.resp * DT);
  const twx = s.cp * C.pr * tm, twy = -s.cy * C.yr * tm, twz = -s.cr * C.rr * (s.drift ? 1.2 : 1);
  s.w.x += (twx - s.w.x) * k; s.w.y += (twy - s.w.y) * k; s.w.z += (twz - s.w.z) * k;
  qintegrate(s.q, s.w.x, s.w.y, s.w.z, DT);
  // boost meter
  if (s.boosting) {
    s.boost -= DT * (s.tetheredBy ? 0.55 : 0.36);
    if (s.boost <= 0) { s.boost = 0; s.boosting = false; s.boostLock = true; }
  } else if (s.boost < 1) s.boost = Math.min(1, s.boost + DT * 0.11 * engB);
  if (s.boostLock && s.boost > 0.25) s.boostLock = false;
  // speed along the nose
  let want = s.boosting ? C.boostSpd * (0.9 + engM * 0.1) : s.thr * C.maxSpd * engM;
  const cruising = s.isPlayer && F.cruise === 2 && F.cruiseV > 0;
  if (cruising) { want = F.cruiseV; s.boosting = false; }
  if (!s.engOk) want *= 0.35;
  if (s.tetheredBy) want = Math.min(want, C.maxSpd * 0.5 * (s.boosting ? 1.5 : 1));
  if (!s.isPlayer && F.E && s.team === TEAM_E && !s.cap) want *= F.E.spd;
  const acc = cruising ? 900 : s.boosting ? C.acc * 2.2 : C.acc;
  s.spd += clamp(want - s.spd, -(s.spd > C.boostSpd * 1.1 ? 1400 : acc * 1.4) * DT, acc * DT);   // a cruise drop-out sheds speed fast
  fwd(s, T);
  // velocity aligns to the nose with grip; drifting keeps momentum while the nose swings
  const g = s.drift ? 0.12 : C.grip;
  const kv = 1 - Math.exp(-g * DT);
  s.vel.x += (T.x * s.spd - s.vel.x) * kv; s.vel.y += (T.y * s.spd - s.vel.y) * kv; s.vel.z += (T.z * s.spd - s.vel.z) * kv;
  // tether pull
  if (s.tetheredBy) {
    const h = s.tetheredBy;
    if (!h.alive || h.tether !== s) s.tetheredBy = null;
    else {
      const dx = h.pos.x - s.pos.x, dy = h.pos.y - s.pos.y, dz = h.pos.z - s.pos.z, d = Math.hypot(dx, dy, dz) || 1;
      if (d > 260) { const a = 26 * DT / d; s.vel.x += dx * a; s.vel.y += dy * a; s.vel.z += dz * a; }
      if (s.boosting && d > 380) s.breakT += DT; else s.breakT = Math.max(0, s.breakT - DT * 0.5);
      if (s.breakT > 1.5 || d > 760) { breakTether(F, h); }
    }
  }
  s.pos.x += s.vel.x * DT; s.pos.y += s.vel.y * DT; s.pos.z += s.vel.z * DT;
  // shields regen + laser cooling
  if (s.shMax > 0 && s.shGen) {
    const cap = s.shMax * (pips ? SHD_CAP[pips[2]] : 1);
    const capF = cap * (s.shFocus === 1 ? 0.7 : s.shFocus === -1 ? 0.3 : 0.5), capB = cap - capF;
    if (F.t - s.shRegT > 2.2) {
      const r = s.shMax * (s.cap ? 0.025 : 0.11) * (pips ? SHD_REGEN[pips[2]] : 1) * DT;
      if (s.shF < capF) s.shF = Math.min(capF, s.shF + r);
      if (s.shB < capB) s.shB = Math.min(capB, s.shB + r);
    }
    if (s.shF > capF) s.shF = Math.max(capF, s.shF - s.shMax * 0.3 * DT);
    if (s.shB > capB) s.shB = Math.max(capB, s.shB - s.shMax * 0.3 * DT);
  }
  if (s.heat > 0) s.heat = Math.max(0, s.heat - DT * 0.42 * (pips ? WPN_COOL[pips[1]] : 1));
  if (s.overT > 0) s.overT -= DT;
  // static obstacle collision
  if (!s.cap) collideStatic(F, s);
  else if (s.ck !== 'station' && s.ck !== 'beacon') collideCap(F, s);
}
function collideStatic(F, s) {
  const list = cellList(F, s.pos.x, s.pos.y, s.pos.z), O = F.obs, rr = s.cls.rad * 0.8;
  for (let j = 0; j < list.length; j++) {
    const i = list[j], dx = s.pos.x - O.x[i], dy = s.pos.y - O.y[i], dz = s.pos.z - O.z[i], R = O.r[i] + rr, d2 = dx * dx + dy * dy + dz * dz;
    if (d2 < R * R) {
      const d = Math.sqrt(d2) || 1, nx = dx / d, ny = dy / d, nz = dz / d;
      s.pos.x = O.x[i] + nx * R; s.pos.y = O.y[i] + ny * R; s.pos.z = O.z[i] + nz * R;
      const vn = s.vel.x * nx + s.vel.y * ny + s.vel.z * nz;
      if (vn < 0) {
        s.vel.x -= 1.6 * vn * nx; s.vel.y -= 1.6 * vn * ny; s.vel.z -= 1.6 * vn * nz; s.spd *= 0.55;
        const dmg = Math.max(0, -vn - 40) * (s.isPlayer ? 0.22 : 0.1);
        if (dmg > 0) damage(F, s, dmg, null, O.x[i] + nx * O.r[i], O.y[i] + ny * O.r[i], O.z[i] + nz * O.r[i], 2);
        const e = ev(F, EV.BUMP, s.pos.x - nx * rr, s.pos.y - ny * rr, s.pos.z - nz * rr); e.a = -vn; e.s = s;
      }
    }
  }
}
// fighters bounce off capital hulls
function collideCap(F, c) {
  for (const s of F.ships) {
    if (!s.alive || s.cap || s.board) continue;
    if (dist2(s.pos, c.pos) > (c.cls.rad + 20) * (c.cls.rad + 20)) continue;
    for (const h of c.hs) {
      qrot(c.q, h[0], h[1], h[2], T2);
      const hx = c.pos.x + T2.x, hy = c.pos.y + T2.y, hz = c.pos.z + T2.z, R = h[3] + s.cls.rad * 0.7;
      const dx = s.pos.x - hx, dy = s.pos.y - hy, dz = s.pos.z - hz, d2 = dx * dx + dy * dy + dz * dz;
      if (d2 < R * R) {
        const d = Math.sqrt(d2) || 1, nx = dx / d, ny = dy / d, nz = dz / d;
        s.pos.x = hx + nx * R; s.pos.y = hy + ny * R; s.pos.z = hz + nz * R;
        const vn = (s.vel.x - c.vel.x) * nx + (s.vel.y - c.vel.y) * ny + (s.vel.z - c.vel.z) * nz;
        if (vn < 0) { s.vel.x -= 1.6 * vn * nx; s.vel.y -= 1.6 * vn * ny; s.vel.z -= 1.6 * vn * nz; s.spd *= 0.5; const dmg = Math.max(0, -vn - 25) * 0.4; if (dmg > 0) damage(F, s, dmg, null, s.pos.x, s.pos.y, s.pos.z, 2); const e = ev(F, EV.BUMP, s.pos.x, s.pos.y, s.pos.z); e.a = -vn; e.s = s; }
      }
    }
  }
}
function boardFollow(F, s) {
  const b = s.board;
  if (!b.tgt.alive) { s.board = null; s.st = ST.ATTACK; launchShip(s, 60); return; }
  qrot(b.tgt.q, b.lx, b.ly, b.lz, T);
  s.pos.x = b.tgt.pos.x + T.x; s.pos.y = b.tgt.pos.y + T.y; s.pos.z = b.tgt.pos.z + T.z;
  cpy(s.vel, b.tgt.vel);
  s.boardT += DT;
  if (s.boardT > 1) { s.boardT = 0; damage(F, b.tgt, b.tgt.hullMax * 0.012 + 4, s, s.pos.x, s.pos.y, s.pos.z, 3); }
}

// ---- player control (input -> controls) ----------------------------------------------------------------
function playerControl(F, p) {
  const I = F.input;
  if (F.retreat > 0) {   // jump-out: line up and go
    p.cp = 0; p.cy = 0; p.cr = 0; p.boosting = true; p.boost = Math.max(p.boost, 0.5); p.fire = false;
    F.retreat -= DT;
    if (F.retreat <= 0 && F.outcome === 0) { const e = ev(F, EV.WARPOUT, p.pos.x, p.pos.y, p.pos.z); e.s = p; p.alive = false; p.dieT = 0.5; finish(F, -1); }
    return;
  }
  if (F.cruise) cruiseStep(F, p);
  if (F.autopilot) { aiStep(F, p); autoCruise(F, p); return; }
  if (F.briefT > 0.6 && !I.touched) { p.cp = 0; p.cy = 0; p.cr = 0; p.fire = false; return; }
  // throttle
  if (I.thrRate) I.thr = clamp(I.thr + I.thrRate * DT * 0.75, 0, 1);
  p.thr = I.thr;
  if (I.boost && !p.boostLock && p.boost > 0.05) { if (!p.boosting) { const e = ev(F, EV.BOOST, p.pos.x, p.pos.y, p.pos.z); e.s = p; } p.boosting = true; }
  else p.boosting = false;
  p.drift = !!I.drift && p.spd > 60;
  if (I.aimMode === 1 && I.aim) {
    steerToward(p, I.aim.x, I.aim.y, I.aim.z, true);
    p.cr = clamp(p.cr + I.roll, -1, 1);
  } else { p.cp = clamp(I.pitch, -1, 1); p.cy = clamp(I.yaw, -1, 1); p.cr = clamp(I.roll, -1, 1); }
  p.fire = !!I.fire && F.cruise !== 2;
}

// ---- cruise drive: in-system travel at up to 2.6 km/s ------------------------------------------------------
// Spools for 1.4 s, then the ship runs on a cruise bubble: turn rates drop, the speed follows an approach ramp
// to the objective marker and the drive drops out by itself near the marker, on hostile contact (mass lock)
// or before an obstacle. Toggle again to drop out by hand.
const CRUISE_MAX = 2600, MASS_LOCK = 2400;
function hostileNear(F, p, r) {
  for (const s of F.ships) if (s.alive && s.team === TEAM_E && s.st !== ST.DARK && !s.decoy && dist2(s.pos, p.pos) < r * r) return true;
  return false;
}
function cruiseBlock(F, p) {
  if (F.outcome || F.retreat > 0 || !p.alive) return 'cz_cr_blocked';
  if (p.tetheredBy) return 'cz_cr_tethered';
  if (hostileNear(F, p, MASS_LOCK)) return 'cz_cr_masslock';
  return '';
}
function cruiseSet(F, on, why) {
  const p = F.player;
  if (on) {
    const why2 = cruiseBlock(F, p);
    if (why2) { const e = ev(F, EV.CRUISE, p.pos.x, p.pos.y, p.pos.z); e.a = -1; e.k = why2; return false; }
    F.cruise = 1; F.cruiseT = 1.4; const e = ev(F, EV.CRUISE, p.pos.x, p.pos.y, p.pos.z); e.a = 1; return true;
  }
  if (!F.cruise) return false;
  const was = F.cruise; F.cruise = 0; F.cruiseV = 0;
  const e = ev(F, EV.CRUISE, p.pos.x, p.pos.y, p.pos.z); e.a = 0; e.b = was; e.k = why || '';
  return true;
}
function cruiseStep(F, p) {
  if (F.cruise === 1) {
    F.cruiseT -= DT;
    const why = cruiseBlock(F, p); if (why) { cruiseSet(F, false, why); return; }
    if (F.cruiseT <= 0) { F.cruise = 2; const e = ev(F, EV.CRUISE, p.pos.x, p.pos.y, p.pos.z); e.a = 2; }
    return;
  }
  const why = cruiseBlock(F, p); if (why) { cruiseSet(F, false, why); return; }
  let v = CRUISE_MAX;
  const mk = F.obj.marker;
  if (mk) {
    const d = Math.hypot(mk[0] - p.pos.x, mk[1] - p.pos.y, mk[2] - p.pos.z);
    fwd(p, T);
    const toward = ((mk[0] - p.pos.x) * T.x + (mk[1] - p.pos.y) * T.y + (mk[2] - p.pos.z) * T.z) / (d || 1);
    if (toward > 0.5) v = clamp((d - 700) * 0.85, 260, CRUISE_MAX);
    if (d < 900 && toward > 0) { cruiseSet(F, false, 'cz_cr_arrived'); return; }
  }
  // obstacle ahead within ~1.2 s of travel -> drop out
  const look = Math.min(3000, Math.max(300, p.spd * 1.2));
  for (let k = 1; k <= 4; k++) {
    const t = look * k / 4, x = p.pos.x + T.x * t, y = p.pos.y + T.y * t, z = p.pos.z + T.z * t;
    const list = cellList(F, x, y, z), O = F.obs;
    for (let j = 0; j < list.length; j++) { const i = list[j], dx = x - O.x[i], dy = y - O.y[i], dz = z - O.z[i], R = O.r[i] + 60; if (dx * dx + dy * dy + dz * dz < R * R) { cruiseSet(F, false, 'cz_cr_obstacle'); return; } }
  }
  F.cruiseV = v;
}
function autoCruise(F, p) {
  // autopilot (tests / attract): cruise to far objectives when nothing hostile is around
  if (F.cruise || F.briefT > 0 || F.outcome) return;
  const mk = F.obj.marker; if (!mk || (F.step % 30) !== 0) return;
  const d = Math.hypot(mk[0] - p.pos.x, mk[1] - p.pos.y, mk[2] - p.pos.z);
  if (d > 3200 && !cruiseBlock(F, p)) { fwd(p, T); if (((mk[0] - p.pos.x) * T.x + (mk[1] - p.pos.y) * T.y + (mk[2] - p.pos.z) * T.z) / d > 0.9) cruiseSet(F, true); }
}

// Steer a ship's nose toward a world direction (dx,dy,dz). Bank-to-turn for large angles (roll the
// target "above", then pull), direct pitch/yaw for fine aim. mouse=true: precise, no banking flourish.
function steerToward(s, dx, dy, dz, mouse) {
  const C = s.cls;
  qinv(s.q, dx, dy, dz, LD);
  const l = Math.hypot(LD.x, LD.y, LD.z) || 1, lx = LD.x / l, ly = LD.y / l, lz = LD.z / l;
  const fw = -lz, off = Math.acos(clamp(fw, -1, 1));
  const pErr = Math.atan2(ly, Math.max(fw, 0.02) + (fw < 0.02 ? Math.hypot(lx, ly) * 0 : 0)), yErr = Math.atan2(lx, Math.max(fw, 0.02));
  const pRate = C.pr ? s.w.x / C.pr : 0, yRate = C.yr ? -s.w.y / C.yr : 0, rRate = C.rr ? -s.w.z / C.rr : 0;
  const kp = mouse ? 4.2 : 3.2, kd = mouse ? 0.7 : 0.55;
  let pitch = clamp(pErr * kp - pRate * kd, -1, 1), yaw = clamp(yErr * kp - yRate * kd, -1, 1), roll = 0;
  const bank = mouse ? sstep(0.5, 1.4, off) : sstep(0.22, 0.85, off);
  if (bank > 0) {
    const push = ly < 0 && off < 1.35;
    const re = push ? Math.atan2(-lx, -ly) : Math.atan2(lx, ly);
    roll = clamp(re * 1.9 - rRate * 0.45, -1, 1) * bank;
    yaw *= 1 - 0.75 * bank;
    if (off > 1.4) pitch = push ? -1 : (ly > -0.25 * Math.hypot(lx, ly) ? 1 : 0.15);
  }
  s.cp = pitch; s.cy = yaw; s.cr = roll;
  return off;
}

// ---- AI ------------------------------------------------------------------------------------------------
function aiStep(F, s) {
  s.stT += DT; if (s.evadeCd > 0) s.evadeCd -= DT;
  if (s.decoy) { s.decoyT -= DT; if (s.decoyT <= 0) { const e = ev(F, EV.WARPOUT, s.pos.x, s.pos.y, s.pos.z); e.s = s; e.a = 2; s.alive = false; s.dieT = 0.4; return; } }
  if (s.st === ST.STATIC) return;
  if (s.st === ST.WARP) {
    s.warpT -= DT; s.cp = s.cy = s.cr = 0; s.boosting = false; s.thr = 0.8;
    if (s.warpT <= 0) { s.st = s.cap ? ST.CAP : ST.ATTACK; s.stT = 0; }
    return;
  }
  if (s.st === ST.DARK) {
    s.cp = s.cy = s.cr = 0; s.thr = 0; s.spd = 0;
    const p = F.player;
    if ((p.alive && dist2(p.pos, s.pos) < s.darkWake * s.darkWake) || F.t - s.lastHitT < 0.2 || s.stT > 40) wakeSquad(F, s);
    return;
  }
  if (s.board) return;
  s.thinkT -= DT;
  if (s.thinkT <= 0) { s.thinkT = 0.12 + (s.i % 5) * 0.02; aiThink(F, s); }
  switch (s.st) {
    case ST.FORM: aiForm(F, s); break;
    case ST.ATTACK: aiAttack(F, s); break;
    case ST.EVADE: aiEvade(F, s); break;
    case ST.EXTEND: aiExtend(F, s); break;
    case ST.TETHER: aiAttack(F, s); break;
    case ST.BOARD: aiBoard(F, s); break;
    case ST.CAP: aiCap(F, s); break;
    case ST.FLEE: aiFlee(F, s); break;
    case ST.PATH: aiPath(F, s); break;
    case ST.HOLD: s.cp = s.cy = s.cr = 0; s.thr = 0.3; break;
  }
  avoid(F, s);
  if (s.trick) aceTrick(F, s);
}
function wakeSquad(F, s) {
  const sq = s.squad;
  const list = sq ? sq.members : [s];
  for (const m of list) if (m.alive && m.st === ST.DARK) { m.st = ST.ATTACK; m.stT = 0; m.thr = 1; m.boosting = true; m.boost = 1; launchShip(m, 60); }
}
function hostile(a, b) { return a.team !== b.team && a.team !== TEAM_N && b.team !== TEAM_N; }
function aiThink(F, s) {
  const C = s.cls, role = C.role, p = F.player;
  // target selection with hysteresis
  let best = null, bs = Infinity;
  for (const o of F.ships) {
    if (!o.alive || !hostile(s, o) || o.st === ST.DARK || o.decoy && s.team === TEAM_P && F.R() < 0.5) continue;
    if (o.st === ST.WARP && o.warpT > 0.5) continue;
    let d = Math.sqrt(dist2(s.pos, o.pos));
    if (o === s.target) d *= 0.6;
    if (o.isPlayer) d *= 0.8;
    if (s.isPlayer && ((o.cap && F.mission.waveI >= F.mission.waves.length) || o === F.mission.ace)) d *= 0.4;   // autopilot: go for the objective
    if (role === 'tether' || role === 'board') { if (o.ck === 'hauler' || o.ck === 'station' || o.ck === 'beacon') d *= role === 'board' ? 0.2 : 0.45; else if (role === 'board') d *= 3; }
    else if (o.cap && !s.cap) d *= (o.ck === 'station' || o.ck === 'beacon') ? (F.mission.kind === 'defend' && s.cls.role === 'heavy' ? 0.5 : 2.2) : 1.8;
    if (s.team === TEAM_P && s.squad && s.squad.wing && p.target && o === p.target) d *= 0.7;
    if (d < bs) { bs = d; best = o; }
  }
  if (best !== s.target) { s.target = best; if (best && role === 'tether' && s.tether && s.tether !== best) breakTether(F, s); }
  if (!best) { if (s.st !== ST.CAP && s.st !== ST.PATH) s.st = s.squad && s.squad.wing ? ST.FORM : ST.ATTACK; return; }
  if (s.st === ST.CAP || s.st === ST.PATH) return;
  // wingmen stay in formation until something is close
  if (s.squad && s.squad.wing && s.st === ST.FORM) { if (bs < 1700 || (p.target && p.target.alive && dist2(p.target.pos, s.pos) < 2200 * 2200)) setSt(s, ST.ATTACK); return; }
  if (role === 'board') { if (s.st !== ST.BOARD && (best.ck === 'hauler' || best.ck === 'station' || best.ck === 'beacon')) setSt(s, ST.BOARD); return; }
  // evade checks: being lined up, recently hit, missile inbound
  if (s.st === ST.ATTACK || s.st === ST.TETHER) {
    const threat = s.evadeCd > 0 ? 0 : linedUpBy(F, s);
    const hitRecently = s.evadeCd <= 0 && F.t - s.lastHitT < 0.5;
    const mslIn = incomingMissile(F, s, 900);
    const ek = s.isPlayer ? 0.3 : 1;
    if ((threat > 0.8 && F.R() < (0.04 + s.skill * 0.08) * ek) || (hitRecently && F.R() < (0.06 + s.skill * 0.12) * ek) || (mslIn && s.evadeCd <= 0)) {
      s.evadeCd = 3.5 + (1 - s.skill) * 4 + F.R() * 2;
      setSt(s, ST.EVADE); s.evDir = F.R() < 0.5 ? -1 : 1; s.aimJx = F.R() * 2 - 1; s.aimJy = F.R() * 2 - 1;
      if (mslIn && s.flares > 0 && s.flareCd <= 0) dropFlares(F, s);
      return;
    }
    // relitti hit-and-run: low hull -> flee
    if (s.fac === F_RELITTI && !s.ace && s.hull < s.hullMax * 0.28 && F.R() < 0.04) { setSt(s, ST.FLEE); return; }
  }
  if (s.st === ST.ATTACK) {
    const d2 = dist2(s.pos, best.pos);
    // overshoot / merge: too close and closing -> extend away
    if (d2 < 160 * 160 && !best.cap) { setSt(s, ST.EXTEND); return; }
    if (role === 'tether' && !s.tether && d2 < 480 * 480 && s.tetherCd !== undefined && s.tetherCd <= 0) tryTether(F, s, best);
  }
}
function setSt(s, st) { s.st = st; s.stT = 0; }
// how precisely the player (or any hostile) has this ship lined up (0..1)
function linedUpBy(F, s) {
  let m = 0;
  for (const o of F.ships) {
    if (!o.alive || !hostile(s, o) || o.cap || o.target !== s) continue;
    const dx = s.pos.x - o.pos.x, dy = s.pos.y - o.pos.y, dz = s.pos.z - o.pos.z, d = Math.hypot(dx, dy, dz) || 1;
    if (d > 1000) continue;
    fwd(o, T2);
    const c = (T2.x * dx + T2.y * dy + T2.z * dz) / d;
    if (c > 0.985) m = Math.max(m, (c - 0.985) / 0.015);
  }
  return m;
}
function incomingMissile(F, s, range) {
  for (const m of F.msls) if (m.alive && m.tgt === s && m.tgtGen === s.gen && !m.flare && dist2(m.pos, s.pos) < range * range) return true;
  return false;
}
// lead pursuit point for a projectile of speed v
export function leadPoint(sx, sy, sz, svx, svy, svz, t, v, out) {
  const rx = t.pos.x - sx, ry = t.pos.y - sy, rz = t.pos.z - sz;
  const vx = t.vel.x - svx * 0.0, vy = t.vel.y - svy * 0.0, vz = t.vel.z - svz * 0.0;
  const a = vx * vx + vy * vy + vz * vz - v * v, b = 2 * (rx * vx + ry * vy + rz * vz), c = rx * rx + ry * ry + rz * rz;
  let tt;
  if (Math.abs(a) < 1e-6) tt = -c / (b || -1);
  else { const disc = b * b - 4 * a * c; if (disc < 0) tt = Math.sqrt(c) / v; else { const sq = Math.sqrt(disc), t1 = (-b - sq) / (2 * a), t2 = (-b + sq) / (2 * a); tt = t1 > 0 && t2 > 0 ? Math.min(t1, t2) : Math.max(t1, t2); } }
  if (!(tt > 0)) tt = Math.sqrt(c) / v;
  tt = Math.min(tt, 3);
  out.x = t.pos.x + vx * tt; out.y = t.pos.y + vy * tt; out.z = t.pos.z + vz * tt;
  return tt;
}
function aiAttack(F, s) {
  const t = s.target;
  if (!t || !t.alive) {
    s.fire = false; s.thr = 0.6; s.cp = s.cy = s.cr = 0;
    const mk = s.isPlayer || (s.squad && s.squad.wing) ? F.obj.marker : null;   // autopilot / wingmen: head for the objective
    if (mk) { steerToward(s, mk[0] - s.pos.x, mk[1] - s.pos.y, mk[2] - s.pos.z, false); s.thr = 1; s.boosting = s.boost > 0.5 && !s.boostLock; }
    return;
  }
  const C = s.cls, bv = C.bolt || 1000;
  leadPoint(s.pos.x, s.pos.y, s.pos.z, s.vel.x, s.vel.y, s.vel.z, t, bv, T3);
  // aim error shrinks with skill; wobble so they are hittable but dangerous
  const d = Math.sqrt(dist2(s.pos, t.pos)), err = (1 - s.skill) * 0.045 + 0.006;
  const wob = Math.sin(F.t * 1.7 + s.i) * err * d, wob2 = Math.cos(F.t * 1.3 + s.i * 2) * err * d;
  qrot(s.q, 1, 0, 0, T); qrot(s.q, 0, 1, 0, T2);
  const ax = T3.x + T.x * wob + T2.x * wob2 - s.pos.x, ay = T3.y + T.y * wob + T2.y * wob2 - s.pos.y, az = T3.z + T.z * wob + T2.z * wob2 - s.pos.z;
  let tx = ax, ty = ay, tz = az;
  // heavy attack runs on capitals/stations: come in from a standoff angle
  const off = steerToward(s, tx, ty, tz, false);
  // throttle: close fast when far, sit on their six at the sweet spot when near
  const rel = t.vel ? (t.vel.x * (s.pos.x - t.pos.x) + t.vel.y * (s.pos.y - t.pos.y) + t.vel.z * (s.pos.z - t.pos.z)) : 0;
  if (d > 1600) { s.thr = 1; s.boosting = s.boost > 0.4 && d > 2300 && !s.boostLock; }
  else if (d > 600) { s.thr = 0.85; s.boosting = false; }
  else { s.thr = rel < 0 ? 0.5 : 0.62; s.boosting = false; }
  s.drift = false;
  // fire in bursts when the lead point is under the nose
  const cone = 0.035 + (1 - s.skill) * 0.05 + (t.cap ? 0.08 : 0);
  if (C.rate > 0 && off < cone && d < (t.cap ? 1500 : 1250)) {
    if (s.burst > 0) s.fire = true; else if (s.burstCd <= 0) { s.burst = 0.5 + F.R() * 0.6; s.fire = true; }
  } else s.fire = false;
  if (s.burst > 0) { s.burst -= DT; if (s.burst <= 0) { s.burstCd = 0.35 + F.R() * 0.7 * (1.2 - s.skill); s.fire = false; } }
  if (s.burstCd > 0) s.burstCd -= DT;
  // missiles (heavies / aces): lock when held in the cone
  if (s.msl > 0 && !t.cap && off < 0.22 && d < 1800 && d > 300) {
    if (s.lockTgt !== t) { s.lockTgt = t; s.lockT = 0; }
    s.lockT += DT;
    if (s.lockT > 2.2 && s.mslCd <= 0) { fireMissile(F, s, t); s.lockT = 0; s.mslCd = 9 + F.R() * 5; }
  } else if (s.lockT > 0) s.lockT = Math.max(0, s.lockT - DT * 2);
  if (s.mslCd > 0) s.mslCd -= DT;
  if (s.tetherCd > 0) s.tetherCd -= DT; else if (s.tetherCd === undefined && C.tether) s.tetherCd = 0;
  // harpoons: hold station behind a tethered target
  if (s.tether) {
    const h = s.tether, dd = Math.sqrt(dist2(s.pos, h.pos));
    s.thr = dd > 420 ? 0.9 : dd < 300 ? 0.15 : 0.45; s.boosting = false; s.tetherT += DT;
    if (s.tetherT > 7.5 || !h.alive) breakTether(F, s);
  }
}
function tryTether(F, s, t) {
  if (t.tetheredBy || t.cap && t.ck !== 'hauler') return;
  fwd(s, T); const dx = t.pos.x - s.pos.x, dy = t.pos.y - s.pos.y, dz = t.pos.z - s.pos.z, d = Math.hypot(dx, dy, dz) || 1;
  if ((T.x * dx + T.y * dy + T.z * dz) / d < 0.9) return;
  s.tether = t; s.tetherT = 0; t.tetheredBy = s; t.breakT = 0;
  const e = ev(F, EV.TETHER, s.pos.x, s.pos.y, s.pos.z); e.s = s; e.s2 = t;
  if (t.isPlayer) comms(F, 'cz_c_tether', null, null, 3);
}
function breakTether(F, s) {
  if (!s.tether) return;
  const t = s.tether; if (t.tetheredBy === s) t.tetheredBy = null;
  s.tether = null; s.tetherCd = 6 + F.R() * 3;
  const e = ev(F, EV.UNTETHER, s.pos.x, s.pos.y, s.pos.z); e.s = s; e.s2 = t;
}
function aiEvade(F, s) {
  // break turn + jinks + boost, then come back around
  fwd(s, T);
  const ph = s.stT * (3 + s.skill * 3);
  s.cp = clamp(0.85 * s.evDir + Math.sin(ph) * 0.4 * s.aimJy, -1, 1);
  s.cy = Math.sin(ph * 0.7) * 0.6 * s.aimJx;
  s.cr = s.evDir * (0.6 + 0.4 * Math.sin(ph * 0.5));
  s.thr = 0.55; s.boosting = s.boost > 0.3 && s.stT < 1.2 && !s.boostLock; s.fire = false;
  if (s.stT > 1.3 + s.skill * 1.2) setSt(s, F.R() < 0.4 ? ST.EXTEND : ST.ATTACK);
}
function aiExtend(F, s) {
  const t = s.target;
  if (!t || !t.alive) { setSt(s, ST.ATTACK); return; }
  const dx = s.pos.x - t.pos.x, dy = s.pos.y - t.pos.y, dz = s.pos.z - t.pos.z, d = Math.hypot(dx, dy, dz) || 1;
  qrot(s.q, 0, 1, 0, T2);
  steerToward(s, dx / d + T2.x * 0.3 * s.evDir, dy / d + T2.y * 0.3, dz / d + T2.z * 0.3, false);
  s.thr = 1; s.boosting = s.boost > 0.25 && !s.boostLock; s.fire = false;
  if (d > 750 + s.skill * 300 || s.stT > 4) setSt(s, ST.ATTACK);
}
function aiForm(F, s) {
  const L = s.squad && s.squad.leader;
  if (!L || !L.alive) { setSt(s, ST.ATTACK); return; }
  const o = FORM.V[s.slot % FORM.V.length];
  qrot(L.q, o[0], o[1], o[2], T);
  const gx = L.pos.x + T.x + L.vel.x * 0.6, gy = L.pos.y + T.y + L.vel.y * 0.6, gz = L.pos.z + T.z + L.vel.z * 0.6;
  const dx = gx - s.pos.x, dy = gy - s.pos.y, dz = gz - s.pos.z, d = Math.hypot(dx, dy, dz);
  if (d > 60) steerToward(s, dx, dy, dz, false);
  else { fwd(L, T2); steerToward(s, T2.x * 40 + dx, T2.y * 40 + dy, T2.z * 40 + dz, true); }
  const ls = Math.hypot(L.vel.x, L.vel.y, L.vel.z);
  s.thr = clamp((ls + d * 0.6) / s.cls.maxSpd, 0.2, 1); s.boosting = d > 450 && s.boost > 0.3; s.fire = false;
}
function aiBoard(F, s) {
  const t = s.target;
  if (!t || !t.alive || !(t.ck === 'hauler' || t.ck === 'station' || t.ck === 'beacon')) { setSt(s, ST.ATTACK); return; }
  const d = Math.sqrt(dist2(s.pos, t.pos));
  steerToward(s, t.pos.x - s.pos.x, t.pos.y - s.pos.y, t.pos.z - s.pos.z, false);
  s.thr = 1; s.boosting = d > 700 && s.boost > 0.3; s.fire = false;
  // latch onto the nearest hull sphere surface
  const hs = t.hs || [[0, 0, 0, t.cls.rad]];
  for (const h of hs) {
    qrot(t.q, h[0], h[1], h[2], T);
    const hx = t.pos.x + T.x, hy = t.pos.y + T.y, hz = t.pos.z + T.z, dd = Math.hypot(s.pos.x - hx, s.pos.y - hy, s.pos.z - hz);
    if (dd < h[3] + 14) {
      const nx = (s.pos.x - hx) / (dd || 1), ny = (s.pos.y - hy) / (dd || 1), nz = (s.pos.z - hz) / (dd || 1);
      qinv(t.q, hx + nx * (h[3] + 3) - t.pos.x, hy + ny * (h[3] + 3) - t.pos.y, hz + nz * (h[3] + 3) - t.pos.z, T2);
      s.board = { tgt: t, lx: T2.x, ly: T2.y, lz: T2.z }; s.boardT = 0;
      qlook(s.q, -nx, -ny, -nz);
      const e = ev(F, EV.BOARD, s.pos.x, s.pos.y, s.pos.z); e.s = s; e.s2 = t;
      if (F.R() < 0.6) comms(F, 'cz_c_board', null, { t: t.nameKey }, 2);
      return;
    }
  }
}
function aiFlee(F, s) {
  const p = F.player;
  const dx = s.pos.x - p.pos.x, dy = s.pos.y - p.pos.y, dz = s.pos.z - p.pos.z;
  steerToward(s, dx, dy + 200, dz, false); s.thr = 1; s.boosting = s.boost > 0.2; s.fire = false;
  s.fleeT += DT;
  if (s.fleeT > 6 && dx * dx + dy * dy + dz * dz > 1800 * 1800) {   // jump away: gone, no kill credit
    const e = ev(F, EV.WARPOUT, s.pos.x, s.pos.y, s.pos.z); e.s = s; s.alive = false; s.dieT = 0.6;
  }
  if (s.fleeT > 14) setSt(s, ST.ATTACK);
}
function aiPath(F, s) {
  // haulers follow the convoy route; tethered or boarded haulers crawl
  const P = s.path; if (!P) return;
  const wp = P[Math.min(s.pathI, P.length - 1)];
  const dx = wp[0] - s.pos.x, dy = wp[1] - s.pos.y, dz = wp[2] - s.pos.z, d = Math.hypot(dx, dy, dz);
  steerToward(s, dx, dy, dz, false);
  let boarded = 0; for (const g of F.ships) if (g.alive && g.board && g.board.tgt === s) boarded++;
  s.thr = boarded ? 0.35 : 0.85;
  if (d < 260) { if (s.pathI < P.length - 1) s.pathI++; else { const e = ev(F, EV.WARPOUT, s.pos.x, s.pos.y, s.pos.z); e.s = s; e.a = 1; s.alive = false; s.dieT = 0.6; s.jumped = true; F.mission.jumped = (F.mission.jumped || 0) + 1; comms(F, 'cz_c_convoy_jump', null, null, 3); } }
}
function aiCap(F, s) {
  // capitals: close on the objective to a standoff, then broadside-crawl; turrets do the fighting
  const M = F.mission;
  let goal = M.defended && M.defended.alive ? M.defended : (firstAlive(M.protect) || F.player);
  const dx = goal.pos.x - s.pos.x, dy = goal.pos.y - s.pos.y, dz = goal.pos.z - s.pos.z, d = Math.hypot(dx, dy, dz) || 1;
  const stand = goal.cap ? 1100 : 900;
  if (d > stand) steerToward(s, dx, dy, dz, false);
  else { qrot(s.q, 1, 0, 0, T); steerToward(s, -dz / d + T.x * 0.2, 0, dx / d + T.z * 0.2, false); }
  s.thr = d > stand ? 0.9 : 0.35; s.boosting = false; s.fire = false;
  if (!s.engOk) s.thr *= 0.3;
  if (s.cls.carrier && s.carryT <= 0) {   // the Hulk launches scrapwings from its belly
    let out = 0; for (const o of F.ships) if (o.alive && o.carrier === s) out++;
    if (out < 4) {
      qrot(s.q, 0, -30, -20, T); fwd(s, T2);
      const sq = squad(F, ['scrapwing', 'scrapwing'], s.team, s.fac, s.pos.x + T.x, s.pos.y + T.y, s.pos.z + T.z, T2.x, T2.y - 0.4, T2.z, { speed: 120 });
      sq.members.forEach((m) => { m.carrier = s; });
      if (F.R() < 0.6) comms(F, 'cz_c_launch', null, null, 1);
    }
    s.carryT = 22 + F.R() * 8;
  } else s.carryT -= DT;
}
function avoid(F, s) {
  if (s.cap) return;
  // look ahead along the velocity for rocks / hulls; steer away from the nearest threat
  const look = 1.1 + s.spd * 0.004;
  const px = s.pos.x + s.vel.x * look, py = s.pos.y + s.vel.y * look, pz = s.pos.z + s.vel.z * look;
  const list = cellList(F, px, py, pz), O = F.obs;
  let ax = 0, ay = 0, az = 0, hit = 0;
  for (let j = 0; j < list.length; j++) {
    const i = list[j], dx = px - O.x[i], dy = py - O.y[i], dz = pz - O.z[i], R = O.r[i] + s.cls.rad + 35, d2 = dx * dx + dy * dy + dz * dz;
    if (d2 < R * R) { const d = Math.sqrt(d2) || 1, w = (R - d) / R; ax += dx / d * w; ay += dy / d * w; az += dz / d * w; hit++; }
  }
  for (const c of F.ships) {
    if (!c.alive || c === s || (!c.cap && !(c.squad && c.squad === s.squad))) continue;
    const R = c.cap ? c.cls.rad * 0.9 + 40 : 22;
    const dx = px - c.pos.x, dy = py - c.pos.y, dz = pz - c.pos.z, d2 = dx * dx + dy * dy + dz * dz;
    if (d2 < R * R) { const d = Math.sqrt(d2) || 1, w = (R - d) / R; ax += dx / d * w; ay += dy / d * w; az += dz / d * w; hit++; }
  }
  if (!hit) return;
  fwd(s, T);
  steerToward(s, T.x + ax * 2.5, T.y + ay * 2.5, T.z + az * 2.5, false);
  s.fire = s.fire && hit < 2;
}
function aceTrick(F, s) {
  s.trickT -= DT;
  if (s.trickT > 0) return;
  const t = s.target;
  if (s.trick === 1 && t && t.alive && s.msl < 3) {   // volley: three missiles
    for (let k = 0; k < 3; k++) fireMissile(F, s, t);
    s.trickT = 16; comms(F, 'cz_c_ace_volley', s, { name: s.name }, 2);
  } else if (s.trick === 2 && s.hull < s.hullMax * 0.7) {   // decoys: two holo-twins
    for (let k = 0; k < 2; k++) {
      const d = spawn(F, s.ck, s.team, s.fac, s.pos.x + (k ? 30 : -30), s.pos.y + 10, s.pos.z, 0, 0, -1);
      if (!d) break;
      d.q.x = s.q.x; d.q.y = s.q.y; d.q.z = s.q.z; d.q.w = s.q.w; launchShip(d, s.spd);
      d.decoy = true; d.hullMax = d.hull = 1; d.shMax = 0; d.shF = d.shB = 0; d.ace = 1; d.name = s.name; d.nameKey = 'cz_ship_ace'; d.st = ST.EVADE; d.dmgMul = 0; d.value = 0;
      d.squad = s.squad; d.evDir = k ? 1 : -1; d.aimJx = 1; d.aimJy = -1; d.decoyT = 11;
    }
    s.trickT = 20; comms(F, 'cz_c_ace_decoy', s, { name: s.name }, 2);
  } else if (s.trick === 3 && s.hull < s.hullMax * 0.85) {   // shield surge
    s.shF = s.shB = s.shMax * 0.8; s.trickT = 18; const e = ev(F, EV.SHIELD, s.pos.x, s.pos.y, s.pos.z); e.s = s; e.a = 2;
  } else if (s.trick === 0 && s.st === ST.ATTACK && F.t - s.lastHitT < 1) {   // afterburner jink
    setSt(s, ST.EVADE); s.boost = 1; s.trickT = 6;
  } else s.trickT = 2;
}

// ---- weapons ----------------------------------------------------------------------------------------------
function weaponsStep(F) {
  for (const s of F.ships) {
    if (!s.alive) continue;
    if (s.fireCd > 0) s.fireCd -= DT;
    if (s.flareCd > 0) s.flareCd -= DT;
    if (s.isPlayer) playerWeapons(F, s);
    if (s.fire && s.cls.rate > 0 && s.fireCd <= 0 && s.overT <= 0 && !s.decoy) fireLaser(F, s);
    if (s.sub) turretsStep(F, s);
  }
}
function fireLaser(F, s) {
  const C = s.cls, g = C.guns[s.gunI % C.guns.length]; s.gunI++;
  s.fireCd = 1 / C.rate;
  qrot(s.q, g[0], g[1], g[2], T);
  const ox = s.pos.x + T.x, oy = s.pos.y + T.y, oz = s.pos.z + T.z;
  // convergence: aim at the point under the reticle at the target's range (or 500 m), with a gentle
  // magnet toward the lead point when it is already within ~2.5 degrees (player only)
  fwd(s, T2);
  let conv = 500;
  const t = s.target;
  if (t && t.alive) { const d = Math.sqrt(dist2(s.pos, t.pos)); if (d < 1600) conv = clamp(d, 120, 1600); }
  let ax = s.pos.x + T2.x * conv, ay = s.pos.y + T2.y * conv, az = s.pos.z + T2.z * conv;
  if (s.isPlayer && t && t.alive) {
    leadPoint(s.pos.x, s.pos.y, s.pos.z, s.vel.x, s.vel.y, s.vel.z, t, C.bolt, T3);
    const lx = T3.x - s.pos.x, ly = T3.y - s.pos.y, lz = T3.z - s.pos.z, ld = Math.hypot(lx, ly, lz) || 1;
    const c = (lx * T2.x + ly * T2.y + lz * T2.z) / ld;
    if (c > 0.9990 && ld < 1500) { const k = sstep(0.9990, 0.99985, c) * 0.85 + 0.15; ax += (T3.x - ax) * k; ay += (T3.y - ay) * k; az += (T3.z - az) * k; }
  }
  let dx = ax - ox, dy = ay - oy, dz = az - oz; const dl = Math.hypot(dx, dy, dz) || 1; dx /= dl; dy /= dl; dz /= dl;
  const b = allocBolt(F); if (!b) return;
  const v = C.bolt;
  b.alive = true; b.x = b.px = ox; b.y = b.py = oy; b.z = b.pz = oz;
  b.vx = dx * v + s.vel.x * 0.5; b.vy = dy * v + s.vel.y * 0.5; b.vz = dz * v + s.vel.z * 0.5;
  b.life = 1.55; b.team = s.team; b.own = s; b.fac = s.isPlayer ? F_PLAYER : s.fac; b.kind = C.heavyBolt ? 1 : 0; b.len = C.heavyBolt ? 24 : 18;
  const pip = s.pips ? WPN_DMG[s.pips[1]] : 1;
  b.dmg = s.isPlayer ? s.wpnDmg * pip * (F.god ? 4 : 1) : F.E.dmg * s.dmgMul * (s.team === TEAM_P ? 0.5 : 0.24);
  if (s.isPlayer) { s.heat += C.heat; F.stats.shots++; if (s.heat >= 1) { s.heat = 1; s.overT = 1.7; const e = ev(F, EV.OVERHEAT, ox, oy, oz); e.s = s; } }
  const e = ev(F, EV.SHOT, ox, oy, oz); e.s = s; e.a = b.kind; e.b = s.gunI % C.guns.length;
}
function allocBolt(F) {
  const B = F.bolts;
  for (let k = 0; k < B.length; k++) { const i = (F.boltI + k) % B.length; if (!B[i].alive) { F.boltI = i + 1; return B[i]; } }
  return null;
}
function playerWeapons(F, p) {
  const I = F.input, t = p.target;
  // missile lock: hold the target in a 12 degree cone within 2.2 km
  if (t && t.alive && !t.decoy || t && t.alive && t.decoy) {
    const dx = t.pos.x - p.pos.x, dy = t.pos.y - p.pos.y, dz = t.pos.z - p.pos.z, d = Math.hypot(dx, dy, dz) || 1;
    fwd(p, T);
    const c = (T.x * dx + T.y * dy + T.z * dz) / d;
    if (c > 0.978 && d < 2200 && p.msl > 0) {
      const was = p.lockT >= p.lockTime;
      if (p.lockTgt !== t) { p.lockTgt = t; p.lockT = 0; }
      p.lockT = Math.min(p.lockTime, p.lockT + DT);
      if (!was && p.lockT >= p.lockTime) { const e = ev(F, EV.LOCK, t.pos.x, t.pos.y, t.pos.z); e.s = t; }
      else if (!was && (F.step % 12) === 0) { const e = ev(F, EV.LOCKING); e.a = p.lockT / p.lockTime; }
    } else p.lockT = Math.max(0, p.lockT - DT * 2.5);
  } else p.lockT = 0;
  if (I.missile) {
    I.missile = false;
    if (p.msl <= 0) ev(F, EV.NOAMMO);
    else if (t && t.alive && p.lockT >= p.lockTime && p.mslCd <= 0) { fireMissile(F, p, t); p.lockT = 0; p.mslCd = 0.6; }
    else if (t && t.alive && p.mslCd <= 0 && t.cap) { fireMissile(F, p, t); p.mslCd = 0.6; }   // capitals: dumb-fire torpedo run
  }
  if (p.mslCd > 0) p.mslCd -= DT;
  if (I.flare) { I.flare = false; if (p.flares > 0 && p.flareCd <= 0) dropFlares(F, p); }
  if (p.flares < CLS.lucciola.flares) { p.flareRe = (p.flareRe || 0) + DT; if (p.flareRe > 12) { p.flareRe = 0; p.flares++; } }
}
function fireMissile(F, s, t) {
  const m = F.msls.find((x) => !x.alive); if (!m) return;
  if (!s.isPlayer || true) s.msl = Math.max(0, s.msl - 1);
  qrot(s.q, 0, -1.5, -3, T); fwd(s, T2);
  m.alive = true; m.gen++; vset(m.pos, s.pos.x + T.x, s.pos.y + T.y, s.pos.z + T.z); cpy(m.ppos, m.pos);
  m.spd = Math.max(160, s.spd + 40); vset(m.vel, T2.x * m.spd, T2.y * m.spd, T2.z * m.spd);
  m.tgt = t; m.tgtGen = t.gen; m.flare = null; m.life = 6.5; m.team = s.team; m.own = s; m.armT = 0.25;
  m.dmg = s.isPlayer ? 95 + (F.run.weapon | 0) * 12 : F.E.dmg * 2.0 * (s.ace ? 1.2 : 1);
  const e = ev(F, EV.MFIRE, m.pos.x, m.pos.y, m.pos.z); e.s = s; e.s2 = t;
}
function dropFlares(F, s) {
  s.flares--; s.flareCd = 1.2;
  qrot(s.q, 0, 0, 1, T);
  for (let k = 0; k < 4; k++) {
    const f = F.flares.find((x) => !x.alive); if (!f) break;
    f.alive = true; f.life = 2.6; f.team = s.team; cpy(f.pos, s.pos);
    const a = k * 1.57 + F.R(), up = 30 + F.R() * 20;
    qrot(s.q, Math.cos(a) * 45, Math.sin(a) * 45 + up * 0.3, 60, T2);
    vset(f.vel, s.vel.x * 0.4 + T2.x, s.vel.y * 0.4 + T2.y, s.vel.z * 0.4 + T2.z);
  }
  const e = ev(F, EV.FLARE, s.pos.x, s.pos.y, s.pos.z); e.s = s;
  // decoy any missile tracking s within 1100 m
  for (const m of F.msls) if (m.alive && m.tgt === s && !m.flare && dist2(m.pos, s.pos) < 1100 * 1100 && F.R() < 0.85) m.flare = F.flares.find((x) => x.alive && x.team === s.team) || null;
}
function turretsStep(F, s) {
  const C = s.cls;
  for (let k = 0; k < C.sub.length; k++) {
    const sb = C.sub[k];
    if (sb.k !== 'turret' || s.subHp[k] <= 0) continue;
    s.turCd[k] -= DT;
    qrot(s.q, sb.p[0], sb.p[1], sb.p[2], T);
    const tx = s.pos.x + T.x, ty = s.pos.y + T.y, tz = s.pos.z + T.z;
    // nearest hostile within 1700 m above the turret's horizon
    let best = null, bd = 1700 * 1700;
    qrot(s.q, 0, sb.down ? -1 : 1, 0, T2);
    for (const o of F.ships) {
      if (!o.alive || !hostile(s, o) || o.st === ST.DARK || (o.cap && s.ck !== 'station')) continue;
      const dx = o.pos.x - tx, dy = o.pos.y - ty, dz = o.pos.z - tz, d2 = dx * dx + dy * dy + dz * dz;
      if (d2 > bd) continue;
      if ((dx * T2.x + dy * T2.y + dz * T2.z) < -0.15 * Math.sqrt(d2)) continue;
      const w = o.isPlayer ? 0.75 : 1;
      if (d2 * w < bd) { bd = d2 * w; best = o; }
    }
    const aim = s.turAim[k];
    if (best) {
      leadPoint(tx, ty, tz, 0, 0, 0, best, 900, T3);
      let ax = T3.x - tx, ay = T3.y - ty, az = T3.z - tz; const l = Math.hypot(ax, ay, az) || 1; ax /= l; ay /= l; az /= l;
      const kk = 1 - Math.exp(-2.4 * DT);
      aim.x += (ax - aim.x) * kk; aim.y += (ay - aim.y) * kk; aim.z += (az - aim.z) * kk;
      const al = Math.hypot(aim.x, aim.y, aim.z) || 1; aim.x /= al; aim.y /= al; aim.z /= al;
      if (s.turCd[k] <= 0 && ax * aim.x + ay * aim.y + az * aim.z > 0.985 && l < 1650) {
        const burst = (s.turBurst = (s.turBurst || 0) + 1) % 4;
        s.turCd[k] = burst === 0 ? 1.1 + F.R() * 0.8 : 0.16;
        const b = allocBolt(F); if (!b) continue;
        const spread = 0.012 + (s.subHp.length && !bridgeOk(s) ? 0.03 : 0);
        b.alive = true; b.x = b.px = tx + aim.x * 6; b.y = b.py = ty + aim.y * 6; b.z = b.pz = tz + aim.z * 6;
        b.vx = (aim.x + (F.R() - 0.5) * spread) * 900; b.vy = (aim.y + (F.R() - 0.5) * spread) * 900; b.vz = (aim.z + (F.R() - 0.5) * spread) * 900;
        b.life = 2.0; b.team = s.team; b.own = s; b.fac = s.fac; b.kind = 2; b.len = 30;
        b.dmg = s.ck === 'station' ? 10 + F.tier * 1.5 : F.E.dmg * 0.45;
        const e = ev(F, EV.SHOT, b.x, b.y, b.z); e.s = s; e.a = 2; e.b = k;
      }
    }
  }
}
function bridgeOk(s) { const C = s.cls; for (let k = 0; k < C.sub.length; k++) if (C.sub[k].k === 'bridge') return s.subHp[k] > 0; return true; }

// ---- projectiles --------------------------------------------------------------------------------------------
function boltsStep(F) {
  const ships = F.ships, O = F.obs;
  for (const b of F.bolts) {
    if (!b.alive) continue;
    b.life -= DT;
    if (b.life <= 0) { b.alive = false; continue; }
    const nx = b.x + b.vx * DT, ny = b.y + b.vy * DT, nz = b.z + b.vz * DT;
    // ships: segment vs sphere (relative motion ignored at 60 Hz; bolts are 10x faster than ships)
    let hit = null, hitT = 2;
    const sx = nx - b.x, sy = ny - b.y, sz = nz - b.z, sl2 = sx * sx + sy * sy + sz * sz;
    for (let i = 0; i < ships.length; i++) {
      const s = ships[i];
      if (!s.alive || s.team === b.team || s === b.own || s.st === ST.WARP && s.warpT > 0.8) continue;
      const R = s.cls.rad + (s.cap ? 4 : 1.5);
      const cx = s.pos.x - b.x, cy = s.pos.y - b.y, cz = s.pos.z - b.z;
      if (cx * cx + cy * cy + cz * cz > (R + 40) * (R + 40) && Math.abs(cx) > 60 + R) continue;
      let tt = (cx * sx + cy * sy + cz * sz) / (sl2 || 1); tt = clamp(tt, 0, 1);
      const dx = cx - sx * tt, dy = cy - sy * tt, dz = cz - sz * tt;
      if (dx * dx + dy * dy + dz * dz > R * R) continue;
      if (s.hs) { const ht = hullHit(s, b.x, b.y, b.z, sx, sy, sz, sl2); if (ht < 0) continue; tt = ht; }
      if (tt < hitT) { hitT = tt; hit = s; }
    }
    if (hit) {
      const hx = b.x + sx * hitT, hy = b.y + sy * hitT, hz = b.z + sz * hitT;
      b.alive = false;
      if (b.own && b.own.isPlayer) { F.stats.hits++; }
      damage(F, hit, b.dmg, b.own, hx, hy, hz, b.kind === 2 ? 1 : 0);
      continue;
    }
    // static obstacles
    const list = cellList(F, nx, ny, nz);
    for (let j = 0; j < list.length; j++) {
      const i = list[j], dx = nx - O.x[i], dy = ny - O.y[i], dz = nz - O.z[i];
      if (dx * dx + dy * dy + dz * dz < O.r[i] * O.r[i]) {
        b.alive = false; const d = Math.hypot(dx, dy, dz) || 1;
        const e = ev(F, EV.SPARK, O.x[i] + dx / d * O.r[i], O.y[i] + dy / d * O.r[i], O.z[i] + dz / d * O.r[i]); e.a = dx / d; e.b = dy / d; e.c = dz / d; e.v = b.fac;
        break;
      }
    }
    if (!b.alive) continue;
    b.x = nx; b.y = ny; b.z = nz;
  }
}
// segment vs a capital's hull spheres -> earliest t (0..1) or -1
function hullHit(s, bx, by, bz, sx, sy, sz, sl2) {
  let best = -1;
  for (const h of s.hs) {
    qrot(s.q, h[0], h[1], h[2], T2);
    const cx = s.pos.x + T2.x - bx, cy = s.pos.y + T2.y - by, cz = s.pos.z + T2.z - bz;
    let tt = (cx * sx + cy * sy + cz * sz) / (sl2 || 1); tt = clamp(tt, 0, 1);
    const dx = cx - sx * tt, dy = cy - sy * tt, dz = cz - sz * tt;
    if (dx * dx + dy * dy + dz * dz <= h[3] * h[3] && (best < 0 || tt < best)) best = tt;
  }
  return best;
}
function missilesStep(F) {
  for (const m of F.msls) {
    if (!m.alive) continue;
    m.life -= DT; m.armT -= DT;
    let tx, ty, tz, tgt = null;
    if (m.flare) { if (!m.flare.alive) { m.flare = null; m.life = Math.min(m.life, 0.8); } else { tx = m.flare.pos.x; ty = m.flare.pos.y; tz = m.flare.pos.z; } }
    if (!m.flare && m.tgt && m.tgt.alive && m.tgt.gen === m.tgtGen) {
      tgt = m.tgt; leadPoint(m.pos.x, m.pos.y, m.pos.z, 0, 0, 0, tgt, m.spd, T3); tx = T3.x; ty = T3.y; tz = T3.z;
    }
    m.spd = Math.min(430, m.spd + 260 * DT);
    const vl = Math.hypot(m.vel.x, m.vel.y, m.vel.z) || 1;
    let fx = m.vel.x / vl, fy = m.vel.y / vl, fz = m.vel.z / vl;
    if (tx !== undefined) {
      let dx = tx - m.pos.x, dy = ty - m.pos.y, dz = tz - m.pos.z; const d = Math.hypot(dx, dy, dz) || 1; dx /= d; dy /= d; dz /= d;
      // turn toward with a max rate (missiles can be out-turned at the sweet spot)
      const c = clamp(fx * dx + fy * dy + fz * dz, -1, 1), ang = Math.acos(c), maxA = 2.1 * DT;
      const k = ang > maxA ? maxA / ang : 1;
      fx += (dx - fx) * k; fy += (dy - fy) * k; fz += (dz - fz) * k; const fl = Math.hypot(fx, fy, fz) || 1; fx /= fl; fy /= fl; fz /= fl;
      // proximity fuse
      const fuse = tgt ? (tgt.cap ? 12 : tgt.cls.rad + 7) : 10;
      if (m.armT <= 0) {
        if (tgt && tgt.cap && tgt.hs) { const sl2 = m.spd * m.spd * DT * DT; if (hullHit(tgt, m.pos.x, m.pos.y, m.pos.z, fx * m.spd * DT, fy * m.spd * DT, fz * m.spd * DT, sl2) >= 0 || hullHit(tgt, m.pos.x, m.pos.y, m.pos.z, 0, 0, 0, 1) >= 0) { missileHit(F, m, tgt); continue; } }
        else if (d < fuse) { if (m.flare) { m.alive = false; const e = ev(F, EV.MHIT, m.pos.x, m.pos.y, m.pos.z); e.a = 0; continue; } missileHit(F, m, tgt); continue; }
      }
    }
    vset(m.vel, fx * m.spd, fy * m.spd, fz * m.spd);
    m.pos.x += m.vel.x * DT; m.pos.y += m.vel.y * DT; m.pos.z += m.vel.z * DT;
    if (m.life <= 0) { m.alive = false; const e = ev(F, EV.MHIT, m.pos.x, m.pos.y, m.pos.z); e.a = 0; }
  }
}
function missileHit(F, m, t) {
  m.alive = false;
  const e = ev(F, EV.MHIT, m.pos.x, m.pos.y, m.pos.z); e.a = 1; e.s = t;
  if (m.own && m.own.isPlayer) F.stats.hits++;
  damage(F, t, m.dmg, m.own, m.pos.x, m.pos.y, m.pos.z, 4);
}
function flaresStep(F) {
  for (const f of F.flares) {
    if (!f.alive) continue;
    f.life -= DT; if (f.life <= 0) { f.alive = false; continue; }
    f.vel.x *= 0.985; f.vel.y *= 0.985; f.vel.z *= 0.985;
    f.pos.x += f.vel.x * DT; f.pos.y += f.vel.y * DT; f.pos.z += f.vel.z * DT;
  }
}
function picksStep(F) {
  const p = F.player;
  for (const k of F.picks) {
    if (!k.alive) continue;
    k.life -= DT; k.spin += DT;
    if (k.life <= 0) { k.alive = false; continue; }
    k.pos.x += k.vel.x * DT; k.pos.y += k.vel.y * DT; k.pos.z += k.vel.z * DT;
    k.vel.x *= 0.99; k.vel.y *= 0.99; k.vel.z *= 0.99;
    if (p.alive) {
      const d2 = dist2(p.pos, k.pos);
      if (d2 < 140 * 140) { const d = Math.sqrt(d2) || 1, a = 60 * DT / d; k.vel.x += (p.pos.x - k.pos.x) * a; k.vel.y += (p.pos.y - k.pos.y) * a; k.vel.z += (p.pos.z - k.pos.z) * a; }   // magnet
      if (d2 < 30 * 30) {
        k.alive = false;
        if (k.kind === 0) p.msl = Math.min(CLS.lucciola.msl + 2, p.msl + 2);
        else if (k.kind === 1) { p.shF = p.shB = p.shMax * 0.5 * SHD_CAP[p.pips[2]]; }
        else p.hull = Math.min(p.hullMax, p.hull + p.hullMax * 0.25);
        const e = ev(F, EV.PICKUP, k.pos.x, k.pos.y, k.pos.z); e.a = k.kind;
      }
    }
  }
}

// ---- damage ---------------------------------------------------------------------------------------------------
// kind: 0 laser, 1 turret, 2 collision, 3 boarding, 4 missile
export function damage(F, s, dmg, src, hx, hy, hz, kind = 0) {
  if (!s.alive || dmg <= 0) return;
  if (s.st === ST.DARK) wakeSquad(F, s);
  if (src && hostile(s, src)) { s.lastHitBy = src; s.lastHitT = F.t; }
  s.hitT = F.t;
  if (s.decoy) { kill(F, s, src, hx, hy, hz); return; }
  if (s.isPlayer) { F.stats.dmgIn += dmg; if (F.god) dmg *= 0.02; if (F.cruise) cruiseSet(F, false, 'cz_cr_hit'); }
  if (src && src.isPlayer) F.stats.dmgOut += dmg;
  // shields first: front or back hemisphere by where it hit
  qinv(s.q, hx - s.pos.x, hy - s.pos.y, hz - s.pos.z, T);
  const front = T.z < 0;
  let left = dmg;
  const missilePierce = kind === 4 && s.cap;
  if (s.shMax > 0 && !missilePierce) {
    if (front && s.shF > 0) { const a = Math.min(s.shF, left); s.shF -= a; left -= a; }
    else if (!front && s.shB > 0) { const a = Math.min(s.shB, left); s.shB -= a; left -= a; }
    if (left < dmg) {
      s.shRegT = F.t;
      const e = ev(F, EV.SHIELD, hx, hy, hz); e.s = s; e.a = (dmg - left) / Math.max(1, s.shMax * 0.25); e.b = T.x; e.c = T.z; e.v = [T.x, T.y, T.z];
    }
  } else if (s.shMax > 0) s.shRegT = F.t;
  if (left <= 0) { if (s.isPlayer) { const e = ev(F, EV.PHURT, hx, hy, hz); e.a = dmg / s.hullMax; e.b = 0; e.s = src; } return; }
  left *= 1 - (s.armor || 0) * (kind === 4 ? 0.3 : 1);
  if (s.st === ST.STATIC && kind <= 1) left *= 0.6;   // stations/beacons shrug off small arms
  if (missilePierce) left *= 0.8;
  // capital subsystems near the impact take the hit (weak points amplify)
  if (s.sub) {
    let bk = -1, bd = Infinity;
    for (let k = 0; k < s.sub.length; k++) {
      if (s.subHp[k] <= 0) continue;
      const sb = s.sub[k], dx = T.x - sb.p[0], dy = T.y - sb.p[1], dz = T.z - sb.p[2], d2 = dx * dx + dy * dy + dz * dz, rr = sb.r + (kind === 4 ? 14 : 3);
      if (d2 < rr * rr && d2 < bd) { bd = d2; bk = k; }
    }
    if (bk >= 0) {
      const sb = s.sub[bk];
      s.subHp[bk] -= left * (kind === 4 ? 1.6 : 1);
      if (sb.weak) left *= 2.5;
      if (s.subHp[bk] <= 0) subsystemDown(F, s, bk, src);
      else if (!sb.weak) left *= 0.35;   // armoured hardpoints soak most of the hit; weak points do not
    }
  }
  s.hull -= left; s.hullHitT = F.t;
  const e = ev(F, EV.HIT, hx, hy, hz); e.s = s; e.a = left; e.b = kind; e.s2 = src;
  if (s.isPlayer) { const e2 = ev(F, EV.PHURT, hx, hy, hz); e2.a = left / s.hullMax; e2.b = 1; e2.s = src; }
  if (s.objective && F.R() < 0.06) comms(F, s.ck === 'hauler' ? 'cz_c_convoy_hit' : 'cz_c_station_hit', null, null, 1);
  if (s.squad && s.squad.wing && s.hull < s.hullMax * 0.5 && !s.saidHit) { s.saidHit = true; comms(F, 'cz_c_wing_hit', s); }
  if (s.hull <= 0) kill(F, s, src, hx, hy, hz);
}
function subsystemDown(F, s, k, src) {
  const sb = s.sub[k];
  s.subHp[k] = 0;
  qrot(s.q, sb.p[0], sb.p[1], sb.p[2], T);
  const e = ev(F, EV.SUBSYS, s.pos.x + T.x, s.pos.y + T.y, s.pos.z + T.z); e.s = s; e.a = k; e.k = sb.k;
  if (sb.k === 'shield') { s.shGen = false; s.shF = s.shB = 0; }
  if (sb.k === 'engine') s.engOk = false;
  if (src && src.isPlayer) comms(F, 'cz_c_subsys_' + sb.k, null, null, 2);
  if (sb.k === 'reactor' || sb.k === 'bridge') s.hull -= s.hullMax * 0.18;
}
function kill(F, s, src, hx, hy, hz) {
  if (!s.alive) return;
  s.alive = false; s.dieT = s.cap ? 3.2 : 0.8;
  if (s.tether) breakTether(F, s);
  if (s.tetheredBy) { const h = s.tetheredBy; if (h.tether === s) breakTether(F, h); }
  for (const o of F.ships) if (o.alive && o.board && o.board.tgt === s) { o.board = null; setSt(o, ST.ATTACK); launchShip(o, 50); }
  const e = ev(F, s.cap ? EV.CAPKILL : EV.KILL, s.pos.x, s.pos.y, s.pos.z); e.s = s; e.s2 = src; e.a = s.cls.rad; e.b = s.value; e.c = s.decoy ? 1 : 0;
  if (s.isPlayer) { finish(F, 2); F.slow = 1.4; F.slowK = 0.3; return; }
  const sq = s.squad;
  if (sq) {
    sq.lost++;
    if (sq.leader === s) sq.leader = sq.wing ? F.player : firstAlive(sq.members);
    // Gilda discipline: a mauled wing calls for help once
    if (sq.team === TEAM_E && sq.fac === F_GILDA && !sq.called && sq.lost * 2 >= sq.members.length && sq.members.length >= 3 && !F.mission.reinf && F.outcome === 0) {
      sq.called = true; F.mission.reinf = true; F.mission.reinfT = 8;
      comms(F, 'cz_c_reinf', null, null, 2);
    }
  }
  if (s.decoy) return;
  if (src && src.isPlayer && s.team === TEAM_E) {
    F.kills++; F.earned += s.value;
    F.hitStop = s.cap ? 0.1 : s.ace ? 0.12 : 0.055;
    if (s.ace || s.cap) { F.slow = s.cap ? 1.3 : 0.7; F.slowK = s.cap ? 0.25 : 0.35; }
    if (s.ace) F.stats.aceKill++;
    if (s.cap) F.stats.capKill++;
    // salvage drop
    if (!s.cap && F.R() < 0.14) {
      const k = F.picks.find((x) => !x.alive);
      if (k) { k.alive = true; cpy(k.pos, s.pos); vset(k.vel, s.vel.x * 0.3, s.vel.y * 0.3, s.vel.z * 0.3); k.kind = F.R() < 0.45 ? 0 : F.R() < 0.6 ? 1 : 2; k.life = 22; k.spin = 0; }
    }
  } else if (src && src.squad && src.squad.wing && s.team === TEAM_E && F.R() < 0.5) comms(F, 'cz_c_wing_kill', src);
  if (s.ace) comms(F, 'cz_c_ace_down', null, { name: s.name }, 3);
  if (s.team === TEAM_P && s.squad && s.squad.wing) comms(F, 'cz_c_wing_down', s, null, 2);
  if (s.ck === 'hauler') comms(F, 'cz_c_convoy_lost', null, null, 3);
  if (s.cap) { F.hitStop = Math.max(F.hitStop, 0.08); }
  if (s.target === F.player) { /* noop */ }
  if (F.player.target === s) F.player.target = null;
}

// ---- director: waves, objectives, the end ------------------------------------------------------------
function directorStep(F) {
  const M = F.mission, p = F.player;
  M.timer += DT;
  if ((F.step % 6) === 0) updateObjective(F);
  // threat feeds for the HUD
  p.lockWarn = 0; p.incoming = 0;
  for (const s of F.ships) if (s.alive && s.lockTgt === p && s.lockT > 0) p.lockWarn = Math.max(p.lockWarn, s.lockT / 2.2);
  for (const m of F.msls) if (m.alive && m.tgt === p && !m.flare) { const d = Math.sqrt(dist2(m.pos, p.pos)); p.incoming = p.incoming ? Math.min(p.incoming, d) : d; }
  if (M.reinf === true && M.reinfT > 0) {
    M.reinfT -= DT;
    if (M.reinfT <= 0) { M.reinf = 'done'; spawnWave(F, { ships: ['lancer', 'lancer'].concat(F.tier >= 4 ? ['bastion'] : []), how: 'warp' }); F.wave--; F.waves += 0; }
  }
  if (F.outcome !== 0) {
    F.endT += DT;
    if (F.endT > (F.outcome === 1 ? 4.2 : F.outcome === 2 ? 3.4 : 1.2)) F.done = true;
    return;
  }
  if (F.briefT > 0) return;
  // the combat zone: 9 km around the action (the convoy, or where the flight began)
  if (p.alive && F.retreat <= 0) {
    const h = firstAlive(M.protect);
    const d0 = Math.hypot(p.pos.x, p.pos.y, p.pos.z), d1 = h ? Math.sqrt(dist2(p.pos, h.pos)) : d0;
    if (Math.min(d0, d1) > 9000) { F.outT = (F.outT || 0) + DT; if (F.outT > 12) cmd(F, 'retreat'); } else F.outT = 0;
  }
  // trigger pending waves
  const w = M.waves[M.waveI];
  let left = 0; for (const s of F.ships) if (s.alive && s.team === TEAM_E && !s.decoy) left++;
  if (w) {
    let go = false;
    if (w.trig === 'time') go = M.timer >= (w.at || 0) + 3.2;
    else if (w.trig === 'clear') go = left <= (M.swarm ? 2 : 1) && M.timer - (M.lastWaveT || 0) > 6 || M.timer - (M.lastWaveT || 0) > 70;
    else if (w.trig === 'nav') { const n = M.nav[w.nav]; go = n && Math.hypot(n[0] - p.pos.x, n[1] - p.pos.y, n[2] - p.pos.z) < (left === 0 ? 650 : 0); if (!go && left === 0 && M.timer - (M.lastWaveT || 0) > 110) go = true; }
    else if (w.trig === 'route') { const h = firstAlive(M.protect); go = (h && routeFrac(M, h) >= w.frac) || (left === 0 && M.timer - (M.lastWaveT || 0) > 14); }
    if (go) { M.waveI++; M.lastWaveT = M.timer; spawnWave(F, w); left = F.enemyLeft = left + 1; }
  }
  // outcome
  const protAlive = countAlive(M.protect);
  if (M.kind === 'escort') {
    if ((M.jumped || 0) > 0 && protAlive === 0) win(F);
    else if (protAlive === 0 && !(M.jumped > 0)) lose(F);
  } else if (M.kind === 'defend') {
    if (M.defended && !M.defended.alive) lose(F);
    else if (M.waveI >= M.waves.length && left === 0) win(F);
  } else if (M.kind === 'hunt' || M.kind === 'duel') {
    if (M.waveI >= M.waves.length && M.ace && !M.ace.alive) win(F);
  } else if (M.waveI >= M.waves.length && left === 0) win(F);
}
function routeFrac(M, h) {
  const a = M.route[0], b = M.jump;
  const tx = b[0] - a[0], ty = b[1] - a[1], tz = b[2] - a[2], L2 = tx * tx + ty * ty + tz * tz || 1;
  return clamp(((h.pos.x - a[0]) * tx + (h.pos.y - a[1]) * ty + (h.pos.z - a[2]) * tz) / L2, 0, 1);
}
function win(F) {
  finish(F, 1);
  comms(F, 'cz_c_victory_' + F.mission.kind, null, null, 3);
  // the rest of the hostiles bug out
  for (const s of F.ships) if (s.alive && s.team === TEAM_E) { s.st = ST.FLEE; s.fleeT = 5; }
}
function lose(F) { finish(F, 2); }
function finish(F, r) {
  if (F.outcome !== 0) return;
  F.outcome = r; F.endT = 0;
  const e = ev(F, EV.END); e.a = r;
  updateObjective(F);
}

// ---- commands from the HUD / input layer ------------------------------------------------------------------------
export function cmd(F, name, arg) {
  const p = F.player;
  if (!p.alive && name !== 'retreat') return false;
  switch (name) {
    case 'pips': { const ok = pipsAdd(p.pips, arg); if (ok) { const e = ev(F, EV.PIPS); e.a = arg; } return ok; }
    case 'shields': { p.shFocus = p.shFocus === arg ? 0 : arg; const tot = p.shF + p.shB, f = p.shFocus === 1 ? 0.7 : p.shFocus === -1 ? 0.3 : 0.5; p.shF = tot * f; p.shB = tot - p.shF; const e = ev(F, EV.PIPS); e.a = 4 + p.shFocus; return true; }
    case 'target': return setTarget(F, arg);
    case 'flare': F.input.flare = true; return true;
    case 'missile': F.input.missile = true; return true;
    case 'retreat': if (F.outcome === 0 && F.retreat <= 0) { F.retreat = 1.6; p.target = null; cruiseSet(F, false); } return true;
    case 'cruise': return F.cruise ? cruiseSet(F, false, 'cz_cr_manual') : cruiseSet(F, true);
  }
  return false;
}
function setTarget(F, mode) {
  const p = F.player; let pick = null;
  const cands = [];
  for (const s of F.ships) if (s.alive && s.team === TEAM_E && s.st !== ST.DARK) cands.push(s);
  if (!cands.length) { p.target = null; return false; }
  fwd(p, T);
  if (mode === 'ahead') {
    let best = -2;
    for (const s of cands) {
      const dx = s.pos.x - p.pos.x, dy = s.pos.y - p.pos.y, dz = s.pos.z - p.pos.z, d = Math.hypot(dx, dy, dz) || 1;
      const c = (T.x * dx + T.y * dy + T.z * dz) / d - d / 40000;
      if (c > best) { best = c; pick = s; }
    }
  } else if (mode === 'attacker') {
    let bt = -99;
    for (const s of cands) { const t = s === p.lastHitBy ? F.t - p.lastHitT : s.target === p ? 3 + dist2(s.pos, p.pos) / 1e7 : 99; if (t < 99 && -t > bt) { bt = -t; pick = s; } }
    if (!pick) return setTarget(F, 'nearest');
  } else if (mode === 'nearest') {
    let bd = Infinity; for (const s of cands) { const d = dist2(s.pos, p.pos); if (d < bd) { bd = d; pick = s; } }
  } else {   // cycle +1 / -1 by distance
    cands.sort((a, b) => dist2(a.pos, p.pos) - dist2(b.pos, p.pos));
    const i = cands.indexOf(p.target);
    pick = cands[((i < 0 ? -1 : i) + (mode === 'prev' ? -1 : 1) + cands.length) % cands.length];
  }
  if (pick && pick !== p.target) { p.target = pick; p.subSel = -1; p.lockT = 0; const e = ev(F, EV.TARGET); e.s = pick; }
  return !!pick;
}
export function cycleSub(F) {
  const p = F.player, t = p.target; if (!t || !t.sub) return -1;
  for (let k = 1; k <= t.sub.length; k++) { const i = ((p.subSel == null ? -1 : p.subSel) + k) % t.sub.length; if (t.subHp[i] > 0) { p.subSel = i; return i; } }
  p.subSel = -1; return -1;
}
// world position of a ship-local point (for HUD/renderer)
export function localToWorld(s, x, y, z, out) { qrot(s.q, x, y, z, out); out.x += s.pos.x; out.y += s.pos.y; out.z += s.pos.z; return out; }
export function shipFwd(s, out) { return fwd(s, out); }
export { pipsAdd as _pipsAdd, turnMul as _turnMul, steerToward as _steer, NSHIP, NBOLT };
