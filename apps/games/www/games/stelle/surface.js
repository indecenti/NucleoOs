// stelle/surface.js — M3 in the flight sim: the worlds move, the air, the ground, landing, the sites, the threats.
//
// Pure logic like sim.js (no Three.js, no DOM), hooked into its fixed 60 Hz step:
//   · worldsStep   — every world's centre and spin from universe time (stelle/world.js); inside a world's sphere of
//                    influence everything (ships, bolts, missiles, flares, pickups) is carried with its frame, so a
//                    spinning world drags its air and what flies in it — a landed ship stays landed;
//   · the descent state machine of the player:  SPACE -> DESCENT (auto dive) -> ENTRY (heat, plasma, the air bleeds
//                    the speed) -> FLIGHT (yours: ground effect, terrain-following assist, collisions) -> LANDING ->
//                    LANDED -> TAKEOFF -> FLIGHT -> ASCENT -> SPACE; flying into the air fast by hand is an ENTRY too;
//   · sites        — scanner pings, discovery by flying low or landing near, relics to recover, salvage; sentinels
//                    (Echo drones) rise when a relic is taken, raiders come for a wreck;
//   · the AI keeps off the ground; bolts strike it.
// The web save remembers what was found (constellations.js + constellations-web.js); nothing here touches the run.
import { surfaceFor, surfaceElevation, elevation, landable } from './planet.js';
import { planetAt, planetQuat, moonAt, spinRate } from './world.js';
import { _int, EV, TEAM_E, F_ECO, F_RELITTI, DT, qrot, qinv, qlook } from './sim.js';

export const SURF = { SPACE: 0, ENTRY: 1, FLIGHT: 2, LANDING: 3, LANDED: 4, TAKEOFF: 5, ASCENT: 6, DESCENT: 7 };
const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
const sstep = (a, b, x) => { const t = clamp((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); };
const V = { x: 0, y: 0, z: 0 }, V2 = { x: 0, y: 0, z: 0 }, V3 = { x: 0, y: 0, z: 0 }, Q = { x: 0, y: 0, z: 0, w: 1 }, QD = { x: 0, y: 0, z: 0, w: 1 };
const A3 = [0, 0, 0], A4 = [0, 0, 0, 1];
export const REST = 2.6;          // a landed courier's centre above the ground (gear down)
const AIR_V = 330;                // what the air lets you keep (m/s) low down

// ---- setup ------------------------------------------------------------------------------------------------------
export function initSurface(F, opts) {
  const bp = F.bp;
  F.T0 = opts.clock != null ? +opts.clock : (typeof window !== 'undefined' ? Date.now() / 1000 - 1767225600 : 0);
  F.worlds = bp.planets.map((pl, i) => {
    const c = planetAt(pl, i, F.T0, [0, 0, 0]), q = planetQuat(pl, F.T0, [0, 0, 0, 1]);
    return { i, pl, S: null, c: { x: c[0], y: c[1], z: c[2] }, q: { x: q[0], y: q[1], z: q[2], w: q[3] }, R: pl.radius, gas: pl.type === 'gas', land: landable(pl.type) };
  });
  F.surf = { w: -1, S: null, st: SURF.SPACE, t: 0, auto: false, alt: 1e9, agl: 1e9, vs: 0, gs: 0, ground: 0, up: { x: 0, y: 1, z: 0 }, heat: 0, dens: 0, gear: 0, assist: true,
    pull: 0, press: 0, water: false, sites: [], nav: -1, scanI: -1, scanT: 0, ping: 0, pingT: 0, pingR: 0, lootT: 0, ticks: 0, found: 0, dive: null, why: '',
    disc: typeof opts.discovered === 'function' ? opts.discovered : () => 0, lootM: typeof opts.looted === 'function' ? opts.looted : () => 0, slope: 0, land: null, gT: 0, entryV: 0, lvl: 0 };
  // planet and moon destinations follow their worlds (copies: the blueprint stays as it was generated)
  for (const P of F.pois || []) if (P.kind === 'planet' || P.kind === 'moon') P.pos = P.pos.slice();
}
const top = (S) => S.atmo.top;
// sphere of influence: where the world's frame carries you and the ground matters
const soiAlt = (W) => Math.max(W.S ? top(W.S) * 1.9 : 3000, W.R * 0.32);

// ---- the worlds move; inside one, its frame carries everything --------------------------------------------------------
export function worldsStep(F) {
  const T = F.T0 + F.t, U = F.surf, p = F.player;
  for (const W of F.worlds) {
    planetAt(W.pl, W.i, T, A3); planetQuat(W.pl, T, A4);
    if (W.i === U.w) {   // drag what is inside: dq = q1 * q0^-1, p' = c1 + dq (p - c0)
      const q0 = W.q; QD.x = -q0.x; QD.y = -q0.y; QD.z = -q0.z; QD.w = q0.w;
      qmul(A4[0], A4[1], A4[2], A4[3], QD, Q);
      const R2 = (W.R + soiAlt(W)) * (W.R + soiAlt(W));
      const mv = (o) => { const dx = o.x - W.c.x, dy = o.y - W.c.y, dz = o.z - W.c.z; if (dx * dx + dy * dy + dz * dz > R2) return false; qrot(Q, dx, dy, dz, V); o.x = A3[0] + V.x; o.y = A3[1] + V.y; o.z = A3[2] + V.z; return true; };
      const rv = (v) => { qrot(Q, v.x, v.y, v.z, V); v.x = V.x; v.y = V.y; v.z = V.z; };
      for (const s of F.ships) if ((s.alive || s.dieT > 0) && mv(s.pos)) { rv(s.vel); qmul(Q.x, Q.y, Q.z, Q.w, s.q, s.q); }
      for (const m of F.msls) if (m.alive && mv(m.pos)) rv(m.vel);
      for (const f of F.flares) if (f.alive && mv(f.pos)) rv(f.vel);
      for (const k of F.picks) if (k.alive && mv(k.pos)) rv(k.vel);
      for (const b of F.bolts) if (b.alive) { V2.x = b.x; V2.y = b.y; V2.z = b.z; if (mv(V2)) { b.x = V2.x; b.y = V2.y; b.z = V2.z; V3.x = b.vx; V3.y = b.vy; V3.z = b.vz; rv(V3); b.vx = V3.x; b.vy = V3.y; b.vz = V3.z; } }
    }
    W.c.x = A3[0]; W.c.y = A3[1]; W.c.z = A3[2]; W.q.x = A4[0]; W.q.y = A4[1]; W.q.z = A4[2]; W.q.w = A4[3];
  }
  // destinations follow (planets every step is cheap; moons orbit their planet)
  if (F.pois) for (const P of F.pois) {
    if (P.kind === 'planet') { const W = F.worlds[P.i]; P.pos[0] = W.c.x; P.pos[1] = W.c.y; P.pos[2] = W.c.z; }
    else if (P.kind === 'moon' && (F.step % 6) === 0) { const pl = F.bp.planets[P.i]; moonAt(pl, P.i, pl.moons[P.k], T, P.pos); }
  }
  // which world are we in (the nearest whose sphere of influence holds the player)
  let best = -1, bd = Infinity;
  for (const W of F.worlds) {
    if (!W.S) W.S = surfaceFor(F.bp, W.i);
    const d = Math.hypot(p.pos.x - W.c.x, p.pos.y - W.c.y, p.pos.z - W.c.z) - W.R, lim = W.i === U.w ? (U.st === SURF.DESCENT ? Infinity : soiAlt(W) * 1.1) : soiAlt(W);
    if (d < lim && d < bd) { bd = d; best = W.i; }
  }
  if (best !== U.w) enterWorld(F, best);
  if (U.w >= 0) metrics(F);
}
function qmul(ax, ay, az, aw, b, out) {   // out = a * b
  const x = aw * b.x + ax * b.w + ay * b.z - az * b.y, y = aw * b.y - ax * b.z + ay * b.w + az * b.x;
  const z = aw * b.z + ax * b.y - ay * b.x + az * b.w, w = aw * b.w - ax * b.x - ay * b.y - az * b.z;
  out.x = x; out.y = y; out.z = z; out.w = w;
  const l = Math.hypot(out.x, out.y, out.z, out.w) || 1; out.x /= l; out.y /= l; out.z /= l; out.w /= l;
  return out;
}
function enterWorld(F, wi) {
  const U = F.surf;
  U.w = wi; U.S = wi >= 0 ? F.worlds[wi].S : null; U.sites = []; U.nav = -1; U.scanI = -1; U.scanT = 0; U.lootT = 0;
  if (wi < 0) { if (U.st !== SURF.SPACE) setState(F, SURF.SPACE); return; }
  const S = U.S, W = F.worlds[wi], mask = U.disc(wi) >>> 0, lm = U.lootM(wi) >>> 0;
  for (const st of S.sites) U.sites.push({ i: st.i, st, kind: st.kind, pos: { x: 0, y: 0, z: 0 }, seen: !!((mask >>> st.i) & 1), found: !!((mask >>> st.i) & 1), looted: !!((lm >>> st.i) & 1), scan: 0, guard: false, d: 1e9 });
  sitesPos(F, W);
  const e = _int.ev(F, EV.SURF, F.player.pos.x, F.player.pos.y, F.player.pos.z); e.a = -1; e.b = wi; e.k = 'cz_hud_soi'; e.v = { name: W.pl.name, type: W.pl.type };
}
function sitesPos(F, W) {
  const U = F.surf, S = U.S;
  for (const s of U.sites) { const d = s.st.dir, r = S.R + s.st.e + 4; qrot(W.q, d[0] * r, d[1] * r, d[2] * r, s.pos); s.pos.x += W.c.x; s.pos.y += W.c.y; s.pos.z += W.c.z; }
}
// the player's height, ground, climb, the air around it
const LOC = { x: 0, y: 0, z: 0 };
function localOf(W, x, y, z, out) { qinv(W.q, x - W.c.x, y - W.c.y, z - W.c.z, out); return out; }
export function groundAt(F, x, y, z) {   // -> agl at a world point in the active world (and fills LOC with the local position)
  const W = F.worlds[F.surf.w], S = F.surf.S;
  localOf(W, x, y, z, LOC); const r = Math.hypot(LOC.x, LOC.y, LOC.z) || 1;
  const g = S.A ? surfaceElevation(S, LOC.x / r, LOC.y / r, LOC.z / r) : (W.gas ? 120 : 0);
  return r - S.R - g;
}
function metrics(F) {
  const U = F.surf, p = F.player, W = F.worlds[U.w], S = U.S;
  const rx = p.pos.x - W.c.x, ry = p.pos.y - W.c.y, rz = p.pos.z - W.c.z, r = Math.hypot(rx, ry, rz) || 1;
  U.up.x = rx / r; U.up.y = ry / r; U.up.z = rz / r;
  localOf(W, p.pos.x, p.pos.y, p.pos.z, LOC);
  const nx = LOC.x / r, ny = LOC.y / r, nz = LOC.z / r;
  const e = S.A ? elevation(S, nx, ny, nz) : 0;
  U.water = S.sea > 0 && e < 0 && S.liquid !== 2;
  U.ground = W.gas ? 120 : (S.sea > 0 ? Math.max(e, 0) : e);
  U.alt = r - S.R; U.agl = U.alt - U.ground;
  U.vs = p.vel.x * U.up.x + p.vel.y * U.up.y + p.vel.z * U.up.z;
  U.gs = Math.sqrt(Math.max(0, p.vel.x * p.vel.x + p.vel.y * p.vel.y + p.vel.z * p.vel.z - U.vs * U.vs));
  U.dens = Math.exp(-Math.max(0, U.alt) / S.atmo.HR);
  U.inAir = U.alt < top(S);
  // pull-up warning: seconds to impact (straight-line along the velocity, checked at two look-ahead points)
  let tti = U.vs < -1 ? U.agl / -U.vs : 99;
  if ((F.step & 3) === 0 && U.st === SURF.FLIGHT && U.agl < 400) {
    for (const k of [1.0, 2.0, 3.2]) { const a = groundAt(F, p.pos.x + p.vel.x * k, p.pos.y + p.vel.y * k, p.pos.z + p.vel.z * k); if (a < 18) tti = Math.min(tti, k * Math.max(0.2, (a + 30) / 48)); }
    U.tti = tti;
  } else U.tti = tti;
  U.pull = U.st === SURF.FLIGHT && Math.min(tti, U.tti || 99) < 3 && U.agl < 300 ? 1 : 0;
  U.press = W.gas ? sstep(500, 140, U.alt) : 0;
}
function setState(F, st, k) {
  const U = F.surf, p = F.player;
  if ((st === SURF.FLIGHT || st === SURF.SPACE) && U.st !== SURF.FLIGHT) F.input.aimReset = true;   // the helm comes back: aim where the nose is
  U.st = st; U.t = 0;
  const e = _int.ev(F, EV.SURF, p.pos.x, p.pos.y, p.pos.z); e.a = st; e.k = k || ''; e.b = U.w;
}

// ---- commands (from the HUD: L / Y / N, the touch buttons, the autopilot) ------------------------------------------------
export function cmdSurface(F, name) {
  const U = F.surf, p = F.player;
  if (!U) return null;
  const no = (k) => { const e = _int.ev(F, EV.SURF, p.pos.x, p.pos.y, p.pos.z); e.a = -2; e.k = k; return false; };
  switch (name) {
    case 'descend': {   // from space: the computer flies the entry; you get the helm back below the clouds
      if (U.st !== SURF.SPACE) return false;
      const W = nearestLandable(F); if (!W) return no('cz_hud_no_world');
      if (F.dock || F.jump || F.outcome || !p.alive) return no('cz_cr_blocked');
      if (_int.hostileNear(F, p, 2400)) return no('cz_cr_masslock');
      if (p.spd > 3200) return no('cz_hud_land_fast');
      if (U.w !== W.i) enterWorld(F, W.i);
      if (F.cruise) _int.cruiseSet(F, false);
      // bring her down near a signal: the closest unfound site to where the dive would land
      if (U.nav < 0 && U.sites.length) {
        localOf(W, p.pos.x, p.pos.y, p.pos.z, LOC); const r = Math.hypot(LOC.x, LOC.y, LOC.z) || 1;
        let bi = -1, bc = -2; for (let i = 0; i < U.sites.length; i++) { const d = U.sites[i].st.dir, c = (d[0] * LOC.x + d[1] * LOC.y + d[2] * LOC.z) / r - (U.sites[i].found ? 1 : 0); if (c > bc) { bc = c; bi = i; } }
        if (bi >= 0) { U.nav = bi; U.sites[bi].seen = true; }
      }
      U.auto = true; U.dive = null; setState(F, SURF.DESCENT, W.gas ? 'cz_hud_gas_entry' : 'cz_hud_descent');
      return true;
    }
    case 'land': {
      if (U.st !== SURF.FLIGHT) return false;
      if (U.S && F.worlds[U.w].gas) return no('cz_hud_land_gas');
      if (U.agl > 450) return no('cz_hud_land_high');
      if (p.spd > 200) return no('cz_hud_land_fast');
      if (U.water) return no('cz_hud_land_water');
      if (_int.hostileNear(F, p, 1500)) return no('cz_hud_land_hostile');
      pickLandingSpot(F); setState(F, SURF.LANDING, 'cz_hud_landing'); return true;
    }
    case 'takeoff': if (U.st !== SURF.LANDED) return false; setState(F, SURF.TAKEOFF, 'cz_hud_takeoff'); return true;
    case 'ascend': {
      if (U.st !== SURF.FLIGHT && U.st !== SURF.ENTRY) return false;
      if (_int.hostileNear(F, p, 1200)) return no('cz_cr_masslock');
      U.auto = true; setState(F, SURF.ASCENT, 'cz_hud_ascent'); return true;
    }
    case 'scan': {
      if (U.w < 0 || U.st === SURF.SPACE) return no('cz_hud_scan_space');
      if (U.ping > 0 && F.t - U.pingT < 4) return false;
      ping(F, 7000); return true;
    }
    case 'assist': U.assist = !U.assist; { const e = _int.ev(F, EV.SURF, p.pos.x, p.pos.y, p.pos.z); e.a = -3; e.k = U.assist ? 'cz_hud_assist_on' : 'cz_hud_assist_off'; } return true;
    case 'snav': {   // next site (seen ones), then none
      const list = U.sites.filter((s) => s.seen); if (!list.length) return no('cz_hud_no_signal');
      const cur = U.sites[U.nav], i = cur ? list.indexOf(cur) : -1;
      U.nav = i + 1 < list.length ? U.sites.indexOf(list[i + 1]) : U.sites.indexOf(list[0]);
      _int.updateObjective(F); return true;
    }
  }
  return null;
}
export function nearestLandable(F, reach = 1.6) {
  const p = F.player; let best = null, bd = Infinity;
  for (const W of F.worlds) {
    if (!W.S) W.S = surfaceFor(F.bp, W.i);
    const d = Math.hypot(p.pos.x - W.c.x, p.pos.y - W.c.y, p.pos.z - W.c.z) - W.R;
    if (d < W.R * reach + 2500 && d < bd) { bd = d; best = W; }
  }
  return best;
}
function ping(F, R) {
  const U = F.surf, p = F.player;
  U.ping = 1; U.pingT = F.t; U.pingR = R; let n = 0;
  for (const s of U.sites) { s.d = Math.hypot(s.pos.x - p.pos.x, s.pos.y - p.pos.y, s.pos.z - p.pos.z); if (s.d < R && !s.seen) { s.seen = true; n++; } }
  const e = _int.ev(F, EV.SCANPING, p.pos.x, p.pos.y, p.pos.z); e.a = n; e.b = R;
  if (U.nav < 0) { let bi = -1, bd = Infinity; for (let i = 0; i < U.sites.length; i++) { const s = U.sites[i]; if (s.seen && !s.found && s.d < bd) { bd = s.d; bi = i; } } if (bi >= 0) U.nav = bi; }
}
// a flat, dry spot under (or next to) the ship
function pickLandingSpot(F) {
  const U = F.surf, p = F.player, W = F.worlds[U.w], S = U.S;
  localOf(W, p.pos.x, p.pos.y, p.pos.z, LOC);
  const r = Math.hypot(LOC.x, LOC.y, LOC.z), n = { x: LOC.x / r, y: LOC.y / r, z: LOC.z / r };
  let best = null, bs = Infinity;
  // a ring of candidates (tangent offsets) plus the point below; slope from 4 samples 12 m apart
  const ux = Math.abs(n.y) < 0.9 ? -n.z : 1, uz = Math.abs(n.y) < 0.9 ? n.x : 0, uy = 0, ul = Math.hypot(ux, uy, uz);
  const ax = ux / ul, ay = uy / ul, az = uz / ul, bx = n.y * az - n.z * ay, by = n.z * ax - n.x * az, bz = n.x * ay - n.y * ax;
  for (let k = -1; k < 12; k++) {
    const ang = k * 0.7, dd = k < 0 ? 0 : 30 + (k % 3) * 35;
    const ox = (ax * Math.cos(ang) + bx * Math.sin(ang)) * dd / S.R, oy = (ay * Math.cos(ang) + by * Math.sin(ang)) * dd / S.R, oz = (az * Math.cos(ang) + bz * Math.sin(ang)) * dd / S.R;
    let cx = n.x + ox, cy = n.y + oy, cz = n.z + oz; const cl = Math.hypot(cx, cy, cz); cx /= cl; cy /= cl; cz /= cl;
    // not on a site's own ground (the ruin stands there), not in the sea
    let onSite = false; for (const s of S.sites) { const sd = s.dir; if (sd[0] * cx + sd[1] * cy + sd[2] * cz > Math.cos(s.r0 * 0.75 / S.R)) { onSite = true; break; } }
    if (onSite) continue;
    const e0 = elevation(S, cx, cy, cz);
    if (S.sea > 0 && e0 < 0.5 && S.liquid !== 2) continue;
    let sl = 0; const k6 = 6 / S.R;
    for (const [sa, sb] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) { const qx = cx + (ax * sa + bx * sb) * k6, qy = cy + (ay * sa + by * sb) * k6, qz = cz + (az * sa + bz * sb) * k6, ql = Math.hypot(qx, qy, qz); sl = Math.max(sl, Math.abs(elevation(S, qx / ql, qy / ql, qz / ql) - e0) / 6); }
    const score = sl * 100 + dd * 0.05;
    if (score < bs) { bs = score; best = { x: cx, y: cy, z: cz, e: Math.max(e0, S.sea > 0 ? 0 : -1e9), sl }; }
    if (k < 0 && sl < 0.18) break;
  }
  U.land = best || { x: n.x, y: n.y, z: n.z, e: U.ground, sl: 1 }; U.slope = U.land.sl;
}

// ---- the player's control in the air (called from sim.js playerControl) --------------------------------------------------
// returns true when the computer flies (descent, entry, landing, take-off, ascent, landed)
export function surfaceControl(F, p) {
  const U = F.surf; if (!U) return false;
  U.t += DT;
  p.spdCmd = 0; p.hover = U.st === SURF.LANDING || U.st === SURF.LANDED || U.st === SURF.TAKEOFF;
  if (U.st === SURF.SPACE || U.w < 0) { p.hover = false; return false; }
  const W = F.worlds[U.w], S = U.S, up = U.up;
  switch (U.st) {
    case SURF.DESCENT: case SURF.ENTRY: {
      if (!U.auto) return false;   // a manual entry: yours, the air only bleeds the speed (surfacePost)
      // dive toward the signal (or straight ahead), steep in the thin air, shallowing as it thickens; level out
      // below the clouds and hand over the helm
      const tgt = U.sites[U.nav], tp = top(S);
      fwdOf(p, V);
      let fx = V.x - up.x * (V.x * up.x + V.y * up.y + V.z * up.z), fy = V.y - up.y * (V.x * up.x + V.y * up.y + V.z * up.z), fz = V.z - up.z * (V.x * up.x + V.y * up.y + V.z * up.z);
      const fl = Math.hypot(fx, fy, fz); if (fl < 1e-3) { fx = -up.z; fy = 0; fz = up.x; } else { fx /= fl; fy /= fl; fz /= fl; }
      let hx = fx, hy = fy, hz = fz, hl = U.alt * 6;
      if (tgt) {
        let tx = tgt.pos.x - p.pos.x, ty = tgt.pos.y - p.pos.y, tz = tgt.pos.z - p.pos.z; const vu = tx * up.x + ty * up.y + tz * up.z; tx -= up.x * vu; ty -= up.y * vu; tz -= up.z * vu;
        const tl = Math.hypot(tx, ty, tz);
        // head for the signal when it is ahead (never loop back for it: overfly it, the pilot turns after the entry)
        if (tl > 1 && (tx * fx + ty * fy + tz * fz) / tl > 0.15) { hx = tx / tl; hy = ty / tl; hz = tz / tl; hl = tl; }
      }
      // above the air: a steep dive (40 deg); in it, the glide path that arrives over the signal at the level-off
      // height, shallowing as the air thickens
      const k = sstep(tp * 0.9, tp * 0.32, U.alt);
      // still at cruise speed: hold a shallow line and brake before the dive
      const braking = U.st === SURF.DESCENT && p.spd > 2600;
      let gam = U.st === SURF.DESCENT ? (braking ? 0.12 : 0.7) : clamp(Math.atan2(Math.max(0, U.alt - tp * 0.28), Math.max(400, hl - 1200)), 0.18, 1.1) * (1 - 0.75 * k);
      if (U.st === SURF.ENTRY && U.agl < 1100 && p.spd > AIR_V + 120) gam = -0.15;   // too low, too fast: level and climb while the air brakes
      const pitch = -gam;
      steerTo(p, hx * Math.cos(pitch) + up.x * Math.sin(pitch), hy * Math.cos(pitch) + up.y * Math.sin(pitch), hz * Math.cos(pitch) + up.z * Math.sin(pitch), up);
      p.boosting = false; p.fire = false; p.thr = 1;
      if (U.st === SURF.DESCENT) { p.spdCmd = 1700 + 1100 * sstep(tp * 1.3, tp * 5, U.alt); p.accCmd = 900; if (U.alt < tp) { U.entryV = p.spd; setState(F, SURF.ENTRY, W.gas ? 'cz_hud_gas_entry' : 'cz_hud_entry'); } }
      else { p.spdCmd = AIR_V + (p.spd - AIR_V) * Math.exp(-DT * (0.22 + 0.9 * k)); p.accCmd = 1400; }
      if (U.st === SURF.ENTRY && (U.alt < tp * 0.3 || U.agl < 700 || U.t > 18) && p.spd < AIR_V + 90) { U.auto = false; F.input.thr = 0.6; p.thr = 0.6; U.lvl = 2.2; p.spdCmd = 0; setState(F, SURF.FLIGHT, 'cz_hud_helm'); }
      return true;
    }
    case SURF.LANDING: {
      const L = U.land, r = S.R + L.e + REST;
      qrot(W.q, L.x * r, L.y * r, L.z * r, V2); V2.x += W.c.x; V2.y += W.c.y; V2.z += W.c.z;
      const dx = V2.x - p.pos.x, dy = V2.y - p.pos.y, dz = V2.z - p.pos.z;
      const vert = dx * up.x + dy * up.y + dz * up.z, hx = dx - up.x * vert, hy = dy - up.y * vert, hz = dz - up.z * vert, hd = Math.hypot(hx, hy, hz);
      // keep the nose level, slide over the spot, settle: a hover, not a dive
      fwdOf(p, V); let fx = V.x - up.x * (V.x * up.x + V.y * up.y + V.z * up.z), fy = V.y - up.y * (V.x * up.x + V.y * up.y + V.z * up.z), fz = V.z - up.z * (V.x * up.x + V.y * up.y + V.z * up.z);
      const fl = Math.hypot(fx, fy, fz) || 1; levelTo(p, fx / fl, fy / fl, fz / fl, up, 2.5);
      const hs = clamp(hd * 0.6, 0, 40), vsWant = U.t < 0.6 ? 0 : clamp(vert * 0.55, -22, 6) * (hd < 30 ? 1 : 0.35);
      p.vel.x += ((hx / (hd || 1)) * hs + up.x * vsWant - p.vel.x) * 0.06; p.vel.y += ((hy / (hd || 1)) * hs + up.y * vsWant - p.vel.y) * 0.06; p.vel.z += ((hz / (hd || 1)) * hs + up.z * vsWant - p.vel.z) * 0.06;
      p.spd = Math.hypot(p.vel.x, p.vel.y, p.vel.z); p.hover = true; p.fire = false; p.boosting = false; p.thr = 0.2;
      U.gear = Math.min(1, U.gear + (U.agl < 90 ? DT * 0.8 : 0));
      if (Math.abs(vert) < 0.5 && hd < 6 && U.gear > 0.95) {
        p.vel.x = p.vel.y = p.vel.z = 0; p.spd = 0;
        localOf(W, p.pos.x, p.pos.y, p.pos.z, LOC); const lr = Math.hypot(LOC.x, LOC.y, LOC.z);
        U.land = { x: LOC.x / lr, y: LOC.y / lr, z: LOC.z / lr, e: lr - S.R - REST, sl: U.land.sl };
        const e = _int.ev(F, EV.GROUND, p.pos.x - up.x * REST, p.pos.y - up.y * REST, p.pos.z - up.z * REST); e.a = Math.abs(U.vs); e.b = 1; e.s = p;
        setState(F, SURF.LANDED, 'cz_hud_landed');
      }
      if (U.t > 40) { p.hover = false; setState(F, SURF.FLIGHT, 'cz_hud_land_abort'); }
      return true;
    }
    case SURF.LANDED: {
      const L = U.land, r = S.R + L.e + REST;
      qrot(W.q, L.x * r, L.y * r, L.z * r, V2); p.pos.x = V2.x + W.c.x; p.pos.y = V2.y + W.c.y; p.pos.z = V2.z + W.c.z;
      p.vel.x = p.vel.y = p.vel.z = 0; p.spd = 0; p.thr = 0; p.cp = p.cy = p.cr = 0; p.w.x = p.w.y = p.w.z = 0; p.fire = false; p.boosting = false; p.hover = true; U.gear = 1;
      return true;
    }
    case SURF.TAKEOFF: {
      const k = Math.min(1, U.t / 2.6);
      fwdOf(p, V); let fx = V.x - up.x * (V.x * up.x + V.y * up.y + V.z * up.z), fy = V.y - up.y * (V.x * up.x + V.y * up.y + V.z * up.z), fz = V.z - up.z * (V.x * up.x + V.y * up.y + V.z * up.z);
      const fl = Math.hypot(fx, fy, fz) || 1; levelTo(p, fx / fl, fy / fl, fz / fl, up, 2);
      // a cliff or a butte ahead: rise straight up until the way is clear (sampled 70 m and 150 m along the heading)
      const blocked = groundAt(F, p.pos.x + fx / fl * 70, p.pos.y + fy / fl * 70, p.pos.z + fz / fl * 70) < 28 || groundAt(F, p.pos.x + fx / fl * 150, p.pos.y + fy / fl * 150, p.pos.z + fz / fl * 150) < 22;
      const vsW = blocked ? 18 : U.agl < 45 ? 14 * (0.4 + k) : 2, fwdW = blocked ? 0 : k * 50;
      if (blocked) U.t = Math.min(U.t, 3.0);
      p.vel.x = up.x * vsW + fx / fl * fwdW; p.vel.y = up.y * vsW + fy / fl * fwdW; p.vel.z = up.z * vsW + fz / fl * fwdW; p.spd = Math.hypot(p.vel.x, p.vel.y, p.vel.z);
      p.hover = true; p.thr = 0.5; p.fire = false;
      if (U.agl > 22) U.gear = Math.max(0, U.gear - DT * 1.2);
      if (U.t > 3.2 && U.agl > 30) { p.hover = false; p.spd = Math.max(p.spd, 60); F.input.thr = 0.45; p.thr = 0.45; setState(F, SURF.FLIGHT, 'cz_hud_airborne'); }
      return true;
    }
    case SURF.ASCENT: {
      fwdOf(p, V); let hx = V.x - up.x * (V.x * up.x + V.y * up.y + V.z * up.z), hy = V.y - up.y * (V.x * up.x + V.y * up.y + V.z * up.z), hz = V.z - up.z * (V.x * up.x + V.y * up.y + V.z * up.z);
      const hl = Math.hypot(hx, hy, hz) || 1; hx /= hl; hy /= hl; hz /= hl;
      const pitch = 0.68;
      steerTo(p, hx * Math.cos(pitch) + up.x * Math.sin(pitch), hy * Math.cos(pitch) + up.y * Math.sin(pitch), hz * Math.cos(pitch) + up.z * Math.sin(pitch), up);
      const tp = top(S);
      p.spdCmd = 260 + 2200 * sstep(tp * 0.25, tp * 1.1, U.alt) + U.t * 30; p.accCmd = 260; p.fire = false; p.thr = 1; p.boosting = U.alt < tp * 0.3;
      U.gear = Math.max(0, U.gear - DT);
      if (U.alt > tp * 1.12) { U.auto = false; p.spdCmd = 0; F.input.thr = 0.7; p.thr = 0.7; setState(F, SURF.SPACE, 'cz_hud_orbit'); }
      return true;
    }
  }
  return false;
}
// after the pilot's input (FLIGHT): terrain-following assist, the level-off after an entry, gear up
export function surfaceAssist(F, p) {
  const U = F.surf; if (!U || U.w < 0 || U.st !== SURF.FLIGHT) return;
  if (U.lvl > 0) {   // the entry hands over level: hold the horizon for a moment
    U.lvl -= DT; fwdOf(p, V); const up = U.up, c = V.x * up.x + V.y * up.y + V.z * up.z;
    if (c < -0.05 && !F.input.touched) p.cp = Math.max(p.cp, clamp(-c * 3, 0, 1));
  }
  U.gear = Math.max(0, U.gear - DT * 0.8);
  if (!U.assist || p.hover) return;
  // auto-GCAS: when the ground ahead (or below a sinking ship) would come within ~25 m, pull up, wings level
  const lim = 26 + p.spd * 0.08;
  let need = 0;
  if (U.agl < lim * 3 || (U.tti || 99) < 4) {
    const a1 = U.tti != null && U.tti < 3.5 ? 1 - U.tti / 3.5 : 0;
    need = Math.max(a1, sstep(lim, lim * 0.35, U.agl) * (U.vs < 4 ? 1 : 0.3));
  }
  if (need > 0.02) {
    fwdOf(p, V); const up = U.up, c = V.x * up.x + V.y * up.y + V.z * up.z;
    if (c < 0.35) { p.cp = Math.max(p.cp, clamp(need * 1.4 + (0.35 - c), 0, 1)); p.cr *= 1 - need * 0.6; U.assistK = need; }
  } else U.assistK = 0;
}

// ---- physics after the step: the air, the ground, the pressure floor of a gas giant ---------------------------------------
export function surfacePost(F) {
  const U = F.surf; if (!U || U.w < 0) return;
  const p = F.player, W = F.worlds[U.w], S = U.S, tp = top(S);
  U.ticks++;
  if (p.alive) {
    // the air: a fast ship coming in hot is an entry (heat, plasma), the air bleeds the speed
    if (U.inAir && U.st === SURF.SPACE) setState(F, p.spd > 450 ? SURF.ENTRY : SURF.FLIGHT, p.spd > 450 ? 'cz_hud_entry' : 'cz_hud_atmo');
    if (!U.inAir && (U.st === SURF.FLIGHT || (U.st === SURF.ENTRY && !U.auto))) setState(F, SURF.SPACE, 'cz_hud_orbit');
    // heat: speed through air that is thick enough to burn (peaks just after the edge of the atmosphere)
    const heatV = U.st === SURF.ENTRY || U.st === SURF.DESCENT || U.st === SURF.ASCENT || U.st === SURF.FLIGHT ? p.spd : 0;
    const air = sstep(tp * 1.12, tp * 0.88, U.alt);
    U.heat += (clamp((heatV - 360) / 650, 0, 1) * air * (U.st === SURF.ASCENT ? 0.45 : 1) - U.heat) * (1 - Math.exp(-5 * DT));
    if (U.st === SURF.ENTRY && !U.auto) {
      const cap = AIR_V + (p.spd - AIR_V) * Math.exp(-0.7 * DT * (0.3 + U.dens));
      if (p.spd > cap) { const k = cap / p.spd; p.spd = cap; p.vel.x *= k; p.vel.y *= k; p.vel.z *= k; }
      if (p.spd < AIR_V + 40) setState(F, SURF.FLIGHT, 'cz_hud_helm');
    }
    if (U.st === SURF.FLIGHT) {
      // the cruise drive needs thin air
      if (F.cruise && U.alt < tp * 0.5) _int.cruiseSet(F, false, 'cz_cr_atmo');
      // ground effect: a cushion and a little extra pace in the last 15 m
      if (U.agl < 15 && !U.water) { const k = (1 - U.agl / 15) * 0.15; p.vel.x += U.up.x * k; p.vel.y += U.up.y * k; p.vel.z += U.up.z * k; }
    }
    if (U.st !== SURF.LANDED && U.st !== SURF.LANDING && U.st !== SURF.TAKEOFF) { metrics(F); groundCollide(F, p, true); }
    if (W.gas && U.alt < 380) {   // the pressure floor: refuse gracefully, push back up
      const k = sstep(380, 100, U.alt) * 30 * DT;
      p.vel.x += U.up.x * k; p.vel.y += U.up.y * k; p.vel.z += U.up.z * k;
      if (U.alt < 140 && U.vs < 0) { p.vel.x -= U.up.x * U.vs * 0.2; p.vel.y -= U.up.y * U.vs * 0.2; p.vel.z -= U.up.z * U.vs * 0.2; }
      if (U.ticks % 120 === 0) { const e = _int.ev(F, EV.SURF, p.pos.x, p.pos.y, p.pos.z); e.a = -4; e.k = 'cz_hud_pressure'; }
    }
  }
  // AI keeps off the ground; bolts strike it (sampled every other step, only the low ones)
  if ((F.step & 1) === 0) {
    const R2 = (S.R + soiAlt(W)) * (S.R + soiAlt(W)), hi = S.R + Math.max(S.A, 120) + 40;
    for (const s of F.ships) {
      if (!s.alive || s.isPlayer || s.cap) continue;
      const dx = s.pos.x - W.c.x, dy = s.pos.y - W.c.y, dz = s.pos.z - W.c.z, d2 = dx * dx + dy * dy + dz * dz;
      if (d2 > R2 || d2 > hi * hi * 1.2) continue;
      groundCollide(F, s, false);
    }
    for (const b of F.bolts) {
      if (!b.alive) continue;
      const dx = b.x - W.c.x, dy = b.y - W.c.y, dz = b.z - W.c.z, d2 = dx * dx + dy * dy + dz * dz;
      if (d2 > hi * hi) continue;
      if (groundAt(F, b.x, b.y, b.z) < 0) { b.alive = false; const e = _int.ev(F, EV.SPARK, b.x, b.y, b.z); const r = Math.sqrt(d2) || 1; e.a = dx / r; e.b = dy / r; e.c = dz / r; }
    }
  }
  if ((F.step % 6) === 0) sitesStep(F);
}
// keep a ship above the ground; the player bounces (and gets hurt) like off a rock, the AI is shoved up
function groundCollide(F, s, isP) {
  const U = F.surf, W = F.worlds[U.w];
  const agl = isP ? U.agl : groundAt(F, s.pos.x, s.pos.y, s.pos.z);
  const clr = isP ? REST * 0.9 : s.cls.rad * 0.6;
  if (agl >= clr) return;
  const rx = s.pos.x - W.c.x, ry = s.pos.y - W.c.y, rz = s.pos.z - W.c.z, r = Math.hypot(rx, ry, rz) || 1, ux = rx / r, uy = ry / r, uz = rz / r;
  const push = clr - agl;
  s.pos.x += ux * push; s.pos.y += uy * push; s.pos.z += uz * push;
  if (isP) { U.agl = clr; U.alt += push; }
  const vn = s.vel.x * ux + s.vel.y * uy + s.vel.z * uz;
  if (vn < 0) {
    s.vel.x -= 1.5 * vn * ux; s.vel.y -= 1.5 * vn * uy; s.vel.z -= 1.5 * vn * uz; s.spd *= isP ? 0.75 : 0.6;
    const hard = -vn;
    const dmg = Math.max(0, hard - 14) * (isP ? 0.9 : 0.6);
    if (dmg > 0 && !(isP && F.god)) _int.damage(F, s, dmg, null, s.pos.x - ux * clr, s.pos.y - uy * clr, s.pos.z - uz * clr, 2);
    const e = _int.ev(F, EV.GROUND, s.pos.x - ux * clr, s.pos.y - uy * clr, s.pos.z - uz * clr); e.a = hard; e.b = 0; e.s = s; e.c = U.water ? 1 : 0;
    if (isP) { const e2 = _int.ev(F, EV.BUMP, e.x, e.y, e.z); e2.a = hard * 4; e2.s = s; }
  }
}

// ---- sites: pings, discovery, relics, salvage, the guards ------------------------------------------------------------------
function sitesStep(F) {
  const U = F.surf, p = F.player, W = F.worlds[U.w];
  sitesPos(F, W);
  if (!U.sites.length || !p.alive) return;
  // a passive ping every 8 s in the air reveals what is close
  if (U.st !== SURF.SPACE && F.t - (U.pingT || -99) > 8) ping(F, 2600);
  U.ping = Math.max(0, U.ping - DT * 6 * 0.6);
  let near = -1, nd = Infinity;
  for (let i = 0; i < U.sites.length; i++) {
    const s = U.sites[i];
    s.d = Math.hypot(s.pos.x - p.pos.x, s.pos.y - p.pos.y, s.pos.z - p.pos.z);
    if (s.d < nd) { nd = s.d; near = i; }
  }
  const s = U.sites[near];
  // discovery: fly low over it, or land near it, and hold for a moment
  const close = (U.st === SURF.LANDED && s.d < 420) || (U.st === SURF.FLIGHT && U.agl < 240 && s.d < 320);
  if (s && !s.found && close) {
    if (U.scanI !== near) { U.scanI = near; U.scanT = 0; }
    s.seen = true; U.scanT += DT * 6;
    if (U.scanT >= 2.4) {
      s.found = true; U.scanI = -1; U.scanT = 0; U.found++;
      const e = _int.ev(F, EV.DISCOVER, s.pos.x, s.pos.y, s.pos.z); e.a = s.i; e.b = U.w; e.k = s.kind; e.v = { kind: s.kind, loot: s.st.loot, w: U.w, i: s.i, world: W.pl.name, type: W.pl.type };
      if (s.kind === 'wreck' || s.kind === 'outpost') _int.comms(F, 'cz_c_found_' + s.kind, p, { name: W.pl.name }, 3);
      else _int.comms(F, 'cz_c_found_' + s.kind, null, { name: W.pl.name, who: 'novice' }, 3);
      if (s.kind === 'wreck' && s.st.guard === 'raider' && !s.guard) { s.guard = true; raiders(F, s); }
      if (U.nav === near) U.nav = -1;
    }
  } else if (U.scanI === near && !close) { U.scanT = Math.max(0, U.scanT - DT * 6 * 2); if (U.scanT <= 0) U.scanI = -1; }
  // the relic: landed next to a found shrine or ruin that holds one, it comes aboard; the Echo's sentinels rise
  if (s && s.found && !s.looted && s.st.loot.relic && U.st === SURF.LANDED && s.d < 300) {
    U.lootT += DT * 6;
    if (U.lootT >= 3) {
      s.looted = true; U.lootT = 0;
      const e = _int.ev(F, EV.LOOT, s.pos.x, s.pos.y, s.pos.z); e.a = s.i; e.b = U.w; e.v = { w: U.w, i: s.i };
      if (s.st.guard === 'sentinel' && !s.guard) { s.guard = true; sentinels(F, s); }
    }
  } else U.lootT = Math.max(0, U.lootT - DT * 6);
}
function sentinels(F, s) {
  const U = F.surf, n = 2 + Math.min(3, F.tier >> 1), up = U.up;
  const sq = _int.squad(F, new Array(n).fill('shard'), TEAM_E, F_ECO, s.pos.x + up.x * 30, s.pos.y + up.y * 30, s.pos.z + up.z * 30, up.x, up.y, up.z, { speed: 70, form: 'W' });
  sq.members.forEach((m) => { m.nameKey = 'cz_ship_sentinel'; m.hullMax = m.hull = Math.round(m.hullMax * 1.3); const e = _int.ev(F, EV.WARPIN, m.pos.x, m.pos.y, m.pos.z); e.s = m; e.a = up.x; e.b = up.y; e.c = up.z; });
  _int.comms(F, 'cz_c_sentinels', null, { voice: 1 }, 3);
  F.mission.ef = F_ECO;
}
function raiders(F, s) {
  const U = F.surf, p = F.player, up = U.up, n = 2 + Math.min(3, F.tier >> 1);
  fwdOf(p, V); const x = p.pos.x - V.x * 1600 + up.x * 260, y = p.pos.y - V.y * 1600 + up.y * 260, z = p.pos.z - V.z * 1600 + up.z * 260;
  const list = new Array(n).fill('scrapwing'); if (F.tier >= 3) list[0] = 'harpoon';
  _int.squad(F, list, TEAM_E, F_RELITTI, x, y, z, V.x, V.y, V.z, { speed: 150 });
  _int.comms(F, 'cz_c_raiders_ground', null, { who: 'raider' }, 3);
}
// the AI (called from sim.js avoid()): keep at least ~60 m off the ground, more at speed
export function aiGround(F, s) {
  const U = F.surf; if (!U || U.w < 0 || s.cap) return;
  const W = F.worlds[U.w];
  const dx = s.pos.x - W.c.x, dy = s.pos.y - W.c.y, dz = s.pos.z - W.c.z, r = Math.hypot(dx, dy, dz);
  if (r - U.S.R > Math.max(U.S.A, 120) + 260) return;
  const look = 1.4 + s.spd * 0.004;
  const a0 = groundAt(F, s.pos.x, s.pos.y, s.pos.z), a1 = groundAt(F, s.pos.x + s.vel.x * look, s.pos.y + s.vel.y * look, s.pos.z + s.vel.z * look);
  const want = 55 + s.spd * 0.25, m = Math.min(a0, a1);
  if (m > want) return;
  const k = clamp((want - m) / want, 0, 1) * 2.5, ux = dx / r, uy = dy / r, uz = dz / r;
  fwdOf(s, V);
  _int.steerToward(s, V.x + ux * k, V.y + uy * k, V.z + uz * k, false);
  if (m < want * 0.4) s.fire = false;
}
// objective line while you are in a world's air
export function surfaceObjective(F, O) {
  const U = F.surf, p = F.player;
  if (!U.sites.length) return false;   // a gas giant: nothing to chart, the flight's own objective stays
  if (U.st === SURF.LANDED) { O.key = 'cz_obj_landed'; O.marker = null; return true; }
  const s = U.sites[U.nav];
  if (s) { O.key = s.found ? 'cz_obj_site_found' : 'cz_obj_site'; O.a.kind = ''; O.a.site = s.kind; O.a.d = Math.hypot(s.pos.x - p.pos.x, s.pos.y - p.pos.y, s.pos.z - p.pos.z); O.marker = [s.pos.x, s.pos.y, s.pos.z]; return true; }
  const left = U.sites.filter((x) => !x.found).length;
  O.key = left ? 'cz_obj_scan' : 'cz_obj_world_done'; O.a.n = left; O.marker = null;
  return true;
}
// autopilot (tests / attract): descend to the nav world, visit the nearest unfound site, land, recover, take off, leave
export function autoSurface(F, p) {
  const U = F.surf; if (!U || !F.autoLand || (F.step % 15) !== 0) return false;
  if (U.st === SURF.SPACE) {
    if (U.done) { U.left = true; return false; }
    const P = F.pois && F.pois[F.nav];
    if (P && P.kind === 'planet' && !F.worlds[P.i].gas && !U.left) {
      const W = F.worlds[P.i], d = Math.hypot(p.pos.x - W.c.x, p.pos.y - W.c.y, p.pos.z - W.c.z) - W.R;
      if (d < W.R * 1.5 + 2000 && !_int.hostileNear(F, p, 2400) && p.spd < 3000) { if (F.cruise) _int.cruiseSet(F, false); else cmdSurface(F, 'descend'); }
    }
    return false;
  }
  if (U.st === SURF.FLIGHT) {
    if (U.done) { if (!_int.hostileNear(F, p, 1500)) cmdSurface(F, 'ascend'); return false; }
    let t = U.sites[U.nav];
    // a site found by flying over it stays the target while it is close: the visit still lands beside it
    if (!t || (t.found && t.d > 450) || (U.t < 0.3 && !U.picked)) {
      U.picked = true; let bd = Infinity; for (let i = 0; i < U.sites.length; i++) { const s = U.sites[i]; if (!s.found && s.d < bd) { bd = s.d; U.nav = i; } } t = U.sites[U.nav]; }
    if (!t) { U.done = true; return false; }
    // over the ground, not through it: the horizontal distance (the ship flies 100-200 m up over tall ground)
    const up = U.up, dx = t.pos.x - p.pos.x, dy = t.pos.y - p.pos.y, dz = t.pos.z - p.pos.z, vu = dx * up.x + dy * up.y + dz * up.z;
    const hd = Math.hypot(dx - up.x * vu, dy - up.y * vu, dz - up.z * vu);
    if (hd < 170 && p.spd < 210) cmdSurface(F, 'land');
  } else if (U.st === SURF.LANDED) {
    let t = null; for (const s of U.sites) if (!t || s.d < t.d) t = s;   // the site we came down next to
    const ready = U.t > 4 && (!t || t.d > 420 || (t.found && (t.looted || !t.st.loot.relic || t.d > 300)));
    if (ready) { U.done = U.sites.every((s) => s.found) || U.found >= (F.autoSites || 1); cmdSurface(F, 'takeoff'); }
  }
  return false;
}
// autopilot steering in FLIGHT: toward the nav site at ~140 m above the ground (the assist keeps it off the hills)
export function autoFly(F, p) {
  const U = F.surf; if (!U || !F.autoLand || U.st !== SURF.FLIGHT) return false;
  const t = U.sites[U.nav]; if (!t || U.done) return false;
  const up = U.up, dx = t.pos.x - p.pos.x, dy = t.pos.y - p.pos.y, dz = t.pos.z - p.pos.z, vert = dx * up.x + dy * up.y + dz * up.z;
  const hx = dx - up.x * vert, hy = dy - up.y * vert, hz = dz - up.z * vert, hd = Math.hypot(hx, hy, hz) || 1;
  const want = 170 - Math.min(120, Math.max(0, 450 - hd) * 0.35);   // come down on the final approach (horizontal distance)
  flyLevel(p, hx / hd, hy / hd, hz / hd, clamp((want - U.agl) / 160, -0.3, 0.4), up);
  p.thr = hd < 600 ? 0.4 : 0.85; p.boosting = hd > 2500 && U.agl > 120 && p.boost > 0.3 && !p.boostLock; p.fire = false;
  return true;
}

// review / test hook: put the player over world wi at height alt, where the sun stands sunEl degrees up (az: degrees
// round the sub-solar point), flying level at speed v — the shots and the e2e use it to look at a world in daylight
export function placeOver(F, wi, alt = 300, sunEl = 40, az = 0, v = 140) {
  const W = F.worlds[wi]; if (!W) return false;
  if (!W.S) W.S = surfaceFor(F.bp, wi);
  const S = W.S, sd = F.bp.star.dir, th = (90 - sunEl) * Math.PI / 180, a = az * Math.PI / 180;
  let ux = -sd[2], uy = 0, uz = sd[0]; const ul = Math.hypot(ux, uy, uz) || 1; ux /= ul; uz /= ul;
  const wx = sd[1] * uz - sd[2] * uy, wy = sd[2] * ux - sd[0] * uz, wz = sd[0] * uy - sd[1] * ux;
  const px = ux * Math.cos(a) + wx * Math.sin(a), py = uy * Math.cos(a) + wy * Math.sin(a), pz = uz * Math.cos(a) + wz * Math.sin(a);
  const dx = sd[0] * Math.cos(th) + px * Math.sin(th), dy = sd[1] * Math.cos(th) + py * Math.sin(th), dz = sd[2] * Math.cos(th) + pz * Math.sin(th);
  qinv(W.q, dx, dy, dz, V); const g = S.A ? surfaceElevation(S, V.x, V.y, V.z) : 120, r = S.R + Math.max(g, 0) + alt;
  const p = F.player;
  p.pos.x = W.c.x + dx * r; p.pos.y = W.c.y + dy * r; p.pos.z = W.c.z + dz * r; p.ppos.x = p.pos.x; p.ppos.y = p.pos.y; p.ppos.z = p.pos.z;
  // fly away from the sun along the ground (it lights the view from behind the shoulder)
  let fx = px, fy = py, fz = pz; const fd = fx * dx + fy * dy + fz * dz; fx -= dx * fd; fy -= dy * fd; fz -= dz * fd;
  qlook(p.q, fx, fy, fz, dx, dy, dz); p.pq.x = p.q.x; p.pq.y = p.q.y; p.pq.z = p.q.z; p.pq.w = p.q.w;
  const fl = Math.hypot(fx, fy, fz) || 1; p.vel.x = fx / fl * v; p.vel.y = fy / fl * v; p.vel.z = fz / fl * v; p.spd = v; p.w.x = p.w.y = p.w.z = 0;
  F.undock = null; F.arriveT = 0; F.cruise = 0; F.input.thr = Math.min(1, v / 165);
  const P = F.pois.findIndex((x) => x.kind === 'planet' && x.i === wi); if (P >= 0) F.nav = P;
  if (F.surf.w !== wi) enterWorld(F, wi);
  metrics(F); F.surf.st = F.surf.inAir ? SURF.FLIGHT : SURF.SPACE; F.surf.t = 0; F.surf.auto = false; F.input.aimReset = true;
  return true;
}
// review / test hook: the first site of one of `kinds` on world wi, in daylight — the clock moves (within one turn of
// the world) to when the sun stands about sunEl° over it — and the ship `dist` m from it on the sun side, `alt` m over
// the ground, flying at it with the sun behind (the site is seen lit). Returns the site index or -1.
export function placeNearSite(F, wi, kinds, dist = 1500, alt = 160, v = 120, sunEl = 35) {
  const W = F.worlds[wi]; if (!W || W.gas) return -1;
  if (!W.S) W.S = surfaceFor(F.bp, wi);
  const S = W.S, st = S.sites.find((x) => kinds.includes(x.kind)); if (!st) return -1;
  const sd = F.bp.star.dir, per = Math.PI * 2 / Math.max(1e-6, spinRate(W.pl)), q = [0, 0, 0, 1];
  let bt = F.T0 + F.t, be = Infinity;
  for (let k = 0; k < 240; k++) {
    const T = F.T0 + F.t + per * k / 240; planetQuat(W.pl, T, q); qrot({ x: q[0], y: q[1], z: q[2], w: q[3] }, st.dir[0], st.dir[1], st.dir[2], V);
    const el = Math.asin(clamp(V.x * sd[0] + V.y * sd[1] + V.z * sd[2], -1, 1)) * 180 / Math.PI, d = Math.abs(el - sunEl);
    if (d < be) { be = d; bt = T; }
  }
  F.T0 = bt - F.t;
  for (const w of F.worlds) { planetAt(w.pl, w.i, bt, A3); planetQuat(w.pl, bt, A4); w.c.x = A3[0]; w.c.y = A3[1]; w.c.z = A3[2]; w.q.x = A4[0]; w.q.y = A4[1]; w.q.z = A4[2]; w.q.w = A4[3]; }
  // the site's up (world), the sun's direction along its ground, the start point back along it
  qrot(W.q, st.dir[0], st.dir[1], st.dir[2], V); const ux = V.x, uy = V.y, uz = V.z;
  let hx = sd[0] - ux * (sd[0] * ux + sd[1] * uy + sd[2] * uz), hy = sd[1] - uy * (sd[0] * ux + sd[1] * uy + sd[2] * uz), hz = sd[2] - uz * (sd[0] * ux + sd[1] * uy + sd[2] * uz);
  const hl = Math.hypot(hx, hy, hz) || 1; hx /= hl; hy /= hl; hz /= hl;
  const a = dist / S.R; let dx = ux * Math.cos(a) + hx * Math.sin(a), dy = uy * Math.cos(a) + hy * Math.sin(a), dz = uz * Math.cos(a) + hz * Math.sin(a);
  qinv(W.q, dx, dy, dz, V); const g = surfaceElevation(S, V.x, V.y, V.z), r = S.R + Math.max(g, 0) + alt;
  const p = F.player;
  p.pos.x = W.c.x + dx * r; p.pos.y = W.c.y + dy * r; p.pos.z = W.c.z + dz * r; p.ppos.x = p.pos.x; p.ppos.y = p.pos.y; p.ppos.z = p.pos.z;
  // toward the site, level: -(the sun-side tangent) at the start point
  let fx = ux - dx * (ux * dx + uy * dy + uz * dz), fy = uy - dy * (ux * dx + uy * dy + uz * dz), fz = uz - dz * (ux * dx + uy * dy + uz * dz);
  const fl = Math.hypot(fx, fy, fz) || 1; fx /= fl; fy /= fl; fz /= fl;
  qlook(p.q, fx, fy, fz, dx, dy, dz); p.pq.x = p.q.x; p.pq.y = p.q.y; p.pq.z = p.q.z; p.pq.w = p.q.w;
  p.vel.x = fx * v; p.vel.y = fy * v; p.vel.z = fz * v; p.spd = v; p.w.x = p.w.y = p.w.z = 0;
  F.undock = null; F.arriveT = 0; F.cruise = 0; F.input.thr = Math.min(1, v / 165);
  const P = F.pois.findIndex((x) => x.kind === 'planet' && x.i === wi); if (P >= 0) F.nav = P;
  enterWorld(F, wi);
  const U = F.surf; metrics(F); U.st = U.inAir ? SURF.FLIGHT : SURF.SPACE; U.t = 0; U.auto = false; F.input.aimReset = true;
  const si = U.sites.findIndex((x) => x.i === st.i); if (si >= 0) { U.sites[si].seen = true; U.nav = si; }
  return si;
}

// ---- small steering helpers --------------------------------------------------------------------------------------------
function fwdOf(s, out) { return qrot(s.q, 0, 0, -1, out); }
function steerTo(s, dx, dy, dz, up) {   // nose toward d, wings level with the local horizon
  _int.steerToward(s, dx, dy, dz, false);
  qrot(s.q, 1, 0, 0, V3); const bank = V3.x * up.x + V3.y * up.y + V3.z * up.z;   // right wing up -> roll right
  s.cr = clamp(s.cr * 0.4 + bank * 2.2, -1, 1);
}
// coordinated level flight toward a horizontal heading h (unit), climbing along a path slope `climb` (tan of the angle):
// bank into the turn, pull through it, rudder to help, wings level when on course
function flyLevel(s, hx, hy, hz, climb, up) {
  fwdOf(s, V); qrot(s.q, 1, 0, 0, V3);
  const fu = V.x * up.x + V.y * up.y + V.z * up.z;
  let fx = V.x - up.x * fu, fy = V.y - up.y * fu, fz = V.z - up.z * fu; const fl = Math.hypot(fx, fy, fz) || 1; fx /= fl; fy /= fl; fz /= fl;
  // signed heading error about the local up (positive: the target is to the right)
  const crs = (fy * hz - fz * hy) * up.x + (fz * hx - fx * hz) * up.y + (fx * hy - fy * hx) * up.z, dot = fx * hx + fy * hy + fz * hz;
  const err = -Math.atan2(crs, dot);
  const bankCur = V3.x * up.x + V3.y * up.y + V3.z * up.z;          // > 0: right wing up (banked left)
  const bankWant = -clamp(err * 1.1, -0.85, 0.85);
  s.cr = clamp((bankCur - bankWant) * 2.6 - s.w.z * 0.2, -1, 1);
  const elev = Math.asin(clamp(fu, -1, 1)), gam = Math.atan(climb);
  s.cp = clamp((gam - elev) * 3.2 + Math.abs(bankWant) * 0.55, -1, 1);
  s.cy = clamp(err * 1.4, -1, 1) * 0.6;
}
// ease the orientation toward nose f (level), up = local up
function levelTo(s, fx, fy, fz, up, rate) {
  qlook(Q, fx, fy, fz, up.x, up.y, up.z);
  const k = 1 - Math.exp(-rate * DT);
  let dot = s.q.x * Q.x + s.q.y * Q.y + s.q.z * Q.z + s.q.w * Q.w; const sg = dot < 0 ? -1 : 1;
  s.q.x += (Q.x * sg - s.q.x) * k; s.q.y += (Q.y * sg - s.q.y) * k; s.q.z += (Q.z * sg - s.q.z) * k; s.q.w += (Q.w * sg - s.q.w) * k;
  const l = Math.hypot(s.q.x, s.q.y, s.q.z, s.q.w) || 1; s.q.x /= l; s.q.y /= l; s.q.z /= l; s.q.w /= l;
  s.w.x *= 0.85; s.w.y *= 0.85; s.w.z *= 0.85; s.cp = s.cy = s.cr = 0;
}
