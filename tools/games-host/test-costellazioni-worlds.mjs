// Host-side tests for Costellazioni M3 (worlds) — no browser.
//   1. the shared generator stays byte-identical; the web hash domains stay distinct;
//   2. the noise and the surfaces are deterministic (golden digests) and every world type is shaped;
//   3. terrain chunks: bit-exact seams between neighbours (same face and across cube faces), the geomorph lands every
//      odd edge vertex on its coarse neighbour's edge, skirts are deep enough, normals agree, winding faces out;
//   4. the sites (ruins, shrines, wrecks, outposts): deterministic, on dry land, apart, on levelled pads; flora sits on
//      the ground; the orbits and spins are pure functions of time;
//   5. the descent state machine in the sim: dive, entry (heat), the helm, landing, discovery, recovery, take-off, the
//      climb back to orbit; a manual hot entry; the ground is solid; a spinning world carries a landed ship; gas giants
//      refuse landing and hold you above the pressure floor; the surface threats rise when a relic is taken;
//   6. through constellations.js: a discovery pays once and is persisted in the WEB save (never in the shared one), the
//      ruin's codex entry unlocks, a recovered relic goes into the run's cargo; the next flight knows what was found;
//   7. every surface UI string exists in five languages.
// Run: node tools/games-host/test-costellazioni-worlds.mjs (part of `node tools/games-host/all.mjs`).
import { readFileSync, writeFileSync, mkdirSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { pathToFileURL, fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { createHash } from 'node:crypto';

const here = dirname(fileURLToPath(import.meta.url)), root = join(here, '..', '..');
const GW = join(root, 'apps/games/www/games');
const url = (p) => pathToFileURL(join(GW, p)).href;
let pass = 0, fail = 0; const ok = (c, m) => { if (c) pass++; else { fail++; console.log('  FAIL:', m); } };
// regression digests of the generated worlds (update deliberately, with the docs, when a surface rule changes)
const GOLDEN = { surfaces: '6c4542fe4ccc1e038a8e4b320ad05b6349dd1171a0e470918e833ae98b8ad709' };

const G = await import(url('constellations-gen.js'));
const W = await import(url('stelle/world.js'));
const N = await import(url('stelle/noise.js'));
const PL = await import(url('stelle/planet.js'));
const CH = await import(url('stelle/chunk.js'));
const AT = await import(url('stelle/atmo.js'));
const S = await import(url('stelle/sim.js'));
const SF = await import(url('stelle/surface.js'));

// ---- 1. the shared layer -----------------------------------------------------------------------------------------------
{
  const h = createHash('sha256');
  for (const seed of [1, 0xC057E11A, 123456789, 4000000000]) for (const sector of [0, 1, 5, 13]) {
    const SEC = G.genSector(seed, sector); h.update(JSON.stringify(SEC));
    for (let i = 0; i < SEC.length; i++) for (const m of G.genMissions(seed, sector, i, SEC[i].faction)) {
      const { flavor, ...num } = m; h.update(JSON.stringify(num));
      h.update(JSON.stringify([flavor.arch, flavor.title, flavor.brief, flavor.name, flavor.gang, flavor.rarity, flavor.mods]));
    }
  }
  ok(h.digest('hex') === '1c895c8a6f348137da53aa022238ec9dbabc000d22a80891bae567231fa3a7ed', 'the shared generator output is unchanged');
  const vd = Object.values(W.VDOM); ok(new Set(vd).size === vd.length && W.VDOM.SURF && W.VDOM.SITE && W.VDOM.FLORA, 'web hash domains are distinct (M3 ones included)');
}

// worlds of every type from one seed
const SEED = 987654, SECTOR = 1, SEC = G.genSector(SEED, SECTOR);
const BPS = SEC.map((s, i) => W.systemBlueprint(SEED, SECTOR, i, s));
const byType = {};
BPS.forEach((bp, i) => bp.planets.forEach((p, k) => { if (!byType[p.type]) byType[p.type] = { bp, i, k, p }; }));
const surf = (t) => PL.surfaceFor(byType[t].bp, byType[t].k);

// ---- 2. noise + surfaces: deterministic, varied ------------------------------------------------------------------------
{
  // the integer hash is exact 32-bit arithmetic (the GLSL twin computes the same bits)
  ok(N.hash3i(0, 0, 0, 0) === N.hash3i(0, 0, 0, 0) && N.hash3i(-5, 7, 11, 0xdeadbeef) !== N.hash3i(-5, 7, 12, 0xdeadbeef), 'lattice hash: stable and sensitive');
  ok(Number.isInteger(N.hash3i(-70000 + 65536, 123, -9, 4294967295)) && N.hash3i(1, 2, 3, 4) >= 0, 'lattice hash is an unsigned 32-bit integer');
  let mn = 9, mx = -9; for (let i = 0; i < 4000; i++) { const v = N.noise3(i * 0.37, i * 0.11 - 3, i * 0.071 + 9, 77); mn = Math.min(mn, v); mx = Math.max(mx, v); }
  ok(mn > -1.2 && mx < 1.2 && mx - mn > 1, `gradient noise stays in range (${mn.toFixed(2)} .. ${mx.toFixed(2)})`);
  const h = createHash('sha256');
  for (const t of Object.keys(byType)) {
    const Sf = surf(t), d = [0, 0, 0];
    for (let j = 0; j < 160; j++) {
      const z = -1 + 2 * ((j * 0.618034) % 1), a = j * 2.39996, q = Math.sqrt(1 - z * z);
      d[0] = q * Math.cos(a); d[1] = z; d[2] = q * Math.sin(a);
      h.update(PL.elevation(Sf, d[0], d[1], d[2]).toFixed(4));
    }
    h.update(JSON.stringify(Sf.sites.map((s) => [s.kind, s.dir.map((x) => x.toFixed(6)), s.e.toFixed(3)])));
  }
  const dig = h.digest('hex');
  ok(dig === GOLDEN.surfaces || GOLDEN.surfaces === '', 'surfaces are deterministic (golden digest ' + dig.slice(0, 12) + ')');
  if (GOLDEN.surfaces === '') console.log('  surfaces digest:', dig);
  ok(Object.keys(byType).length === 8, 'the sector has every world type: ' + Object.keys(byType).join(','));
  for (const t of Object.keys(byType)) {
    const Sf = surf(t), d = [0, 0, 0]; let lo = 1e9, hi = -1e9, wet = 0;
    for (let j = 0; j < 600; j++) { const z = -1 + 2 * ((j * 0.618034) % 1), a = j * 2.39996, q = Math.sqrt(1 - z * z); const e = PL.elevation(Sf, q * Math.cos(a), z, q * Math.sin(a)); lo = Math.min(lo, e); hi = Math.max(hi, e); if (e < 0) wet++; }
    if (t === 'gas') { ok(Sf.A === 0 && Sf.sites.length === 0, 'gas giant: no ground, no sites'); continue; }
    ok(hi - lo > Sf.A * 0.4 && hi < Sf.A * 2.2, `${t}: relief ${Math.round(lo)}..${Math.round(hi)} m on a ${Math.round(Sf.R)} m world`);
    if (Sf.sea > 0) ok(wet > 0, `${t}: has ${Sf.liquid === 3 ? 'lava' : Sf.liquid === 2 ? 'frozen sea' : 'water'} (${(wet / 6).toFixed(0)}%)`);
    ok(Sf.atmo.top > 1500 && Sf.atmo.HR > 0 && Sf.atmo.betaR.every((b) => b > 0), `${t}: has an atmosphere`);
  }
  // a world bigger than the minimum keeps its size; small ones grow to the landable minimum
  ok(BPS.every((bp) => bp.planets.every((p) => p.type === 'gas' || p.radius >= 4500)), 'every landable world is at least 4.5 km in radius');
  // the macro field behind the orbital bake agrees with the terrain's (same function, the bake is its GLSL twin)
  const Sr = surf('rocky'), M = PL.macro(Sr, 0.6, 0.64, -0.48, {});
  ok(M.h >= 0 && M.h <= 1 && M.mo >= 0 && M.mo <= 1, 'macro field: height and moisture in 0..1');
  ok(/czMacro/.test(PL.MACRO_GLSL) && /czAlbedo/.test(PL.MACRO_GLSL) && /uint czHash/.test(PL.MACRO_GLSL), 'the GLSL twin of the macro field is exported for the bake and the terrain');
}

// ---- 3. chunks ------------------------------------------------------------------------------------------------------------
{
  const Sf = surf('rocky'), V = CH.SIDE;
  const build = (f, L, x, y) => { const b = CH.newChunkBufs(); return { b, info: CH.buildChunk(Sf, f, L, x, y, b) }; };
  const wp = (c, o) => [c.b.pos[o * 3] + c.info.cx, c.b.pos[o * 3 + 1] + c.info.cy, c.b.pos[o * 3 + 2] + c.info.cz];
  const dist = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
  // determinism
  const a1 = build(2, 6, 20, 33), a2 = build(2, 6, 20, 33);
  ok(Buffer.from(a1.b.pos.buffer).equals(Buffer.from(a2.b.pos.buffer)) && Buffer.from(a1.b.srf.buffer).equals(Buffer.from(a2.b.srf.buffer)), 'a chunk builds bit-identically twice');
  // same-face neighbours: positions and normals along the shared edge
  const A = build(2, 6, 20, 33), B = build(2, 6, 21, 33);
  let gap = 0, ndev = 0;
  for (let j = 0; j < V; j++) { const oa = j * V + V - 1, ob = j * V; gap = Math.max(gap, dist(wp(A, oa), wp(B, ob))); ndev = Math.max(ndev, Math.abs(A.b.nrm[oa * 3] - B.b.nrm[ob * 3]) + Math.abs(A.b.nrm[oa * 3 + 1] - B.b.nrm[ob * 3 + 1]) + Math.abs(A.b.nrm[oa * 3 + 2] - B.b.nrm[ob * 3 + 2])); }
  ok(gap < 0.005, `same-face seam: edges meet within ${gap.toExponential(1)} m`);
  ok(ndev < 1e-3, `same-face seam: normals agree (${ndev.toExponential(1)})`);
  // across cube faces: every edge vertex of a face's border chunks has a twin on the neighbouring face
  const L = 3, n = 1 << L, pts = [];
  for (let f = 0; f < 6; f++) for (let x = 0; x < n; x++) for (let y = 0; y < n; y++) {
    if (x !== 0 && y !== 0 && x !== n - 1 && y !== n - 1) continue;
    const c = build(f, L, x, y);
    for (let o = 0; o < V * V; o++) { const i = o % V, j = (o / V) | 0; if (((x === 0 && i === 0) || (x === n - 1 && i === V - 1) || (y === 0 && j === 0) || (y === n - 1 && j === V - 1))) pts.push({ p: wp(c, o), f }); }
  }
  pts.sort((u, v) => u.p[0] - v.p[0]);
  let pairs = 0, worst = 0;
  for (let i = 0; i < pts.length; i++) for (let k = i + 1; k < pts.length && pts[k].p[0] - pts[i].p[0] < 0.05; k++) { if (pts[k].f === pts[i].f) continue; const d = dist(pts[i].p, pts[k].p); if (d < 0.05) { pairs++; worst = Math.max(worst, d); } }
  ok(pairs > 1000 && worst < 0.005, `cube-face seams: ${pairs} edge vertices meet their twins on the next face (worst ${worst.toExponential(1)} m)`);
  // LOD boundary: fine (level 7, x 23) meets coarse (level 6, x 12) along u; every fine edge vertex, fully morphed,
  // lies on the coarse edge (even ones coincide with coarse vertices, odd ones on the segment between two)
  const Fc = build(2, 7, 23, 32), Cr = build(2, 6, 12, 16); let mDev = 0, rDev = 0;
  for (let j = 0; j < V; j++) {
    const of = j * V + V - 1, oc0 = Math.floor(j / 2) * V, oc1 = Math.ceil(j / 2) * V;
    const C0 = wp(Cr, oc0), C1 = wp(Cr, oc1), cm = [(C0[0] + C1[0]) / 2, (C0[1] + C1[1]) / 2, (C0[2] + C1[2]) / 2];
    const p = wp(Fc, of), fm = [p[0] + Fc.b.mor[of * 4], p[1] + Fc.b.mor[of * 4 + 1], p[2] + Fc.b.mor[of * 4 + 2]];
    mDev = Math.max(mDev, dist(fm, cm)); rDev = Math.max(rDev, dist(p, cm));
  }
  ok(mDev < 0.02, `LOD seam: a fully morphed fine edge lies on the coarse edge (${mDev.toFixed(3)} m)`);
  const drop = PL.nodeSize(Sf, 7) / PL.GRID * 2.5 + 3 + Sf.A * 0.01;
  ok(rDev < drop, `LOD seam: the skirt (${drop.toFixed(1)} m) is deeper than any unmorphed gap (${rDev.toFixed(2)} m)`);
  // winding faces outward; the bounding sphere holds every vertex; skirts hang below their edge
  const idx = CH.chunkIndex(); let outward = 0;
  for (let t = 0; t < 32 * 32 * 2; t++) {
    const p0 = wp(A, idx[t * 3]), p1 = wp(A, idx[t * 3 + 1]), p2 = wp(A, idx[t * 3 + 2]);
    const e1 = [p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]], e2 = [p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]];
    const nx = e1[1] * e2[2] - e1[2] * e2[1], ny = e1[2] * e2[0] - e1[0] * e2[2], nz = e1[0] * e2[1] - e1[1] * e2[0];
    if (nx * p0[0] + ny * p0[1] + nz * p0[2] > 0) outward++;
  }
  ok(outward === 2048, `every grid triangle faces out (${outward}/2048)`);
  let inside = true; for (let o = 0; o < CH.VERTS; o++) if (Math.hypot(A.b.pos[o * 3], A.b.pos[o * 3 + 1], A.b.pos[o * 3 + 2]) > A.info.rad + 1e-3) inside = false;
  ok(inside, 'the chunk bounding sphere holds every vertex');
  const r0 = Math.hypot(...wp(A, 0)), rs = Math.hypot(...wp(A, V * V));
  ok(rs < r0 - 2, 'skirts hang below their edge');
  // liquids are flat at sea level; their elevation attribute keeps the depth
  const So = surf('ocean'); let flat = true, deep = 0;
  for (let f = 0; f < 6 && deep < 50; f++) {
    const c = { b: CH.newChunkBufs() }; c.info = CH.buildChunk(So, f, 2, 1, 1, c.b);
    for (let o = 0; o < V * V; o++) if (c.b.srf[o * 4] < 0) { deep++; if (Math.abs(Math.hypot(...wp(c, o)) - So.R) > 0.01) flat = false; }
  }
  ok(deep > 0 && flat, `ocean: water vertices sit on the sea-level sphere (${deep} checked)`);
}

// ---- 4. sites, flora, orbits ----------------------------------------------------------------------------------------------
{
  let all = 0, ruins = 0, relics = 0;
  for (const bp of BPS) bp.planets.forEach((p, k) => {
    const Sf = PL.surfaceFor(bp, k); if (!Sf || !Sf.A) return;
    const again = PL.surfaceOf(p, SEED, SECTOR, Number(bp.key.split(':')[2]), k, bp.faction);
    ok(JSON.stringify(again.sites) === JSON.stringify(Sf.sites), `${p.name}: sites are deterministic`);
    ok(Sf.sites.length >= 3 && Sf.sites.length <= 9, `${p.name}: ${Sf.sites.length} sites`);
    for (const s of Sf.sites) {
      all++; if (['gate', 'archive', 'observatory'].includes(s.kind)) ruins++; if (s.loot.relic) relics++;
      ok(PL.SITE_KINDS.includes(s.kind) && s.loot.cr > 0, `${p.name}/${s.kind}: a known kind with a reward`);
      const e = PL.elevation(Sf, s.dir[0], s.dir[1], s.dir[2]);
      ok(Math.abs(e - s.e) < 0.6, `${p.name}/${s.kind}: stands on its levelled pad (${(e - s.e).toFixed(2)} m)`);
      if (Sf.sea > 0) ok(s.e > 2, `${p.name}/${s.kind}: on dry land (${s.e.toFixed(1)} m)`);
      for (const o of Sf.sites) if (o !== s) ok(Math.acos(Math.min(1, s.dir[0] * o.dir[0] + s.dir[1] * o.dir[1] + s.dir[2] * o.dir[2])) * Sf.R > 1200, `${p.name}: sites keep apart`);
    }
  });
  ok(all > 40 && ruins > 10 && relics > 3, `the sector holds ${all} sites, ${ruins} Costellatori ruins, ${relics} relics`);
  // flora: deterministic per cell, standing on the ground
  const Sj = surf('jungle'), buf = new Float32Array(4000 * PL.FLORA_STRIDE), buf2 = new Float32Array(4000 * PL.FLORA_STRIDE);
  const L = Math.round(Math.log2(PL.nodeSize(Sj, 0) / 300)); let n = 0, cell = null;
  for (let f = 0; f < 6 && !n; f++) for (let x = 0; x < 6 && !n; x++) {
    const c = [0, 0, 0]; PL.cubeDir(f, -1 + 2 * (x + 0.5) / (1 << L), -1 + 2 * 0.5 / (1 << L), c);
    n = PL.scatterFlora(Sj, f, L, x, 0, 1, buf, c[0] * Sj.R, c[1] * Sj.R, c[2] * Sj.R); cell = { f, x, c };
  }
  const n2 = PL.scatterFlora(Sj, cell.f, L, cell.x, 0, 1, buf2, cell.c[0] * Sj.R, cell.c[1] * Sj.R, cell.c[2] * Sj.R);
  ok(n > 10 && n === n2 && Buffer.from(buf.buffer, 0, n * 32).equals(Buffer.from(buf2.buffer, 0, n * 32)), `flora: ${n} plants in a cell, the same every time`);
  let off = 0; for (let i = 0; i < n; i++) { const o = i * PL.FLORA_STRIDE, x = buf[o] + cell.c[0] * Sj.R, y = buf[o + 1] + cell.c[1] * Sj.R, z = buf[o + 2] + cell.c[2] * Sj.R, r = Math.hypot(x, y, z); off = Math.max(off, Math.abs(r - Sj.R - PL.elevation(Sj, x / r, y / r, z / r))); }
  ok(off < 3.5, `flora stands on the ground (worst ${off.toFixed(2)} m, ribs sink 3 m)`);
  // the far LOD of the big kinds: the same silhouette for far fewer triangles; tiny parts use a plain icosahedron
  {
    const THREE = await import(url('../vendor/three.module.min.js')), PR = await import(url('stelle/props.js'));
    const tris = (g) => g.index.count / 3;
    let worst = 0;
    for (const k of PR.FLORA_BIG) {
      const a = PR.floraGeometry(THREE, k), b = PR.floraGeometry(THREE, k, true);
      worst = Math.max(worst, tris(b) / tris(a));
      ok(tris(b) <= tris(a) && b.attributes.aMat && b.boundingSphere.radius > a.boundingSphere.radius * 0.8, `flora LOD ${k}: ${tris(a)} -> ${tris(b)} triangles, same size`);
    }
    const sp = PR.floraGeometry(THREE, 'spiral'), spl = PR.floraGeometry(THREE, 'spiral', true);
    ok(tris(sp) < 1200 && tris(spl) * 3 < tris(sp), `the spiral tree: ${tris(sp)} triangles near, ${tris(spl)} far`);
    ok(PR.floraGeometry(THREE, 'spiral').index.count === sp.index.count, 'building a far LOD leaves the near builder as it was');
  }
  // orbits: pure functions of time; the home world stays, the others drift slowly, moons circle
  const bp = BPS.find((b) => b.planets.length > 2 && b.planets.some((p) => p.moons.length)), T0 = 12345678;
  const home0 = W.planetAt(bp.planets[0], 0, T0), home1 = W.planetAt(bp.planets[0], 0, T0 + 3600);
  ok(home0.join() === home1.join(), 'the home world (the station frame) stays put');
  const i2 = bp.planets.findIndex((p, i) => i > 0), p0 = W.planetAt(bp.planets[i2], i2, T0), p1 = W.planetAt(bp.planets[i2], i2, T0 + 60);
  const v = Math.hypot(p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]) / 60;
  ok(v > 0.05 && v < 6 && W.planetAt(bp.planets[i2], i2, T0).join() === p0.join(), `other worlds orbit slowly and deterministically (${v.toFixed(2)} m/s)`);
  ok(Math.abs(Math.hypot(p0[0], p0[2]) - Math.hypot(bp.planets[i2].pos[0], bp.planets[i2].pos[2])) < 1, 'an orbit keeps its radius');
  const pm = bp.planets.findIndex((p) => p.moons.length), m = bp.planets[pm].moons[0];
  const m0 = W.moonAt(bp.planets[pm], pm, m, T0), m1 = W.moonAt(bp.planets[pm], pm, m, T0 + W.moonPeriod(m) / 4), c0 = W.planetAt(bp.planets[pm], pm, T0);
  ok(Math.abs(Math.hypot(m0[0] - c0[0], m0[1] - c0[1], m0[2] - c0[2]) - bp.planets[pm].radius * m.orbit) < 1 && Math.hypot(m1[0] - m0[0], m1[1] - m0[1], m1[2] - m0[2]) > bp.planets[pm].radius, 'moons circle their world in minutes');
  const q = W.planetQuat(bp.planets[0], T0), q2 = W.planetQuat(bp.planets[0], T0 + 100);
  ok(Math.abs(Math.hypot(...q) - 1) < 1e-9 && q.join() !== q2.join(), 'worlds spin (a unit quaternion from time)');
  // the atmosphere's JS twin: blue sky at noon on an ocean world, red sun at sunset, nothing in space
  const So = surf('ocean'), A = AT.atmoParams(So), I = [3, 3, 3], res = AT.ambientRes();
  AT.ambientAt(A, 0, So.R + 50, 0, [0.6, 0.8, 0], I, res);
  ok(res.zen[2] > res.zen[0] * 1.8, 'ocean world: the sky is blue');
  const r2 = AT.ambientRes(); AT.ambientAt(A, 0, So.R + 50, 0, [0.999, 0.04, 0], I, r2);
  ok(r2.sun[0] > r2.sun[2] * 1.4, 'sunset: the sun goes red through the air');
  const r3 = AT.ambientRes(); AT.ambientAt(A, 0, So.R + So.atmo.top * 3, 0, [0, 1, 0], I, r3);
  ok(r3.zen[0] + r3.zen[1] + r3.zen[2] < 1e-6 && r3.sun.every((x) => Math.abs(x - 3) < 0.01), 'above the air: black sky, the full sun');
}

// ---- 5. the descent state machine in the sim --------------------------------------------------------------------------------
const RUN = { hull: 100, hull_max: 100, shield_max: 40, weapon: 1, sensors: 1 };
const xcc = { kind: 'explore', type: 0, foeFac: 2, waves: 2, perWave: 3, foeHp: 50, foeDmg: 10, foeSpeed: 85, ace: 0, rewardCr: 0, killCr: 20, repFac: -1, mission: false, slot: 13 };
const flightAt = (t, o = {}) => { const { bp, i } = byType[t]; const F = S.createFlight({ bp, cc: xcc, run: { ...RUN }, seed: SEED, sector: SECTOR, sys: i, slot: 0, brief: false, clock: 1000, ...o }); F.mission.waves.length = 0; return F; };
{
  const t = 'rocky', F = flightAt(t, { autopilot: true, god: true, autoLand: true }), k = byType[t].k;
  S.cmd(F, 'nav', F.pois.findIndex((p) => p.kind === 'planet' && p.i === k));
  const seq = [], evs = []; let heat = 0, below = 0, cur = F.evHead, landedLocal = null, drift = 0, landedT = 0;
  while (F.t < 420 && !F.surf.left) {
    S.stepFlight(F);
    const U = F.surf;
    if (seq[seq.length - 1] !== U.st) seq.push(U.st);
    heat = Math.max(heat, U.heat);
    if (U.w >= 0 && U.st !== SF.SURF.LANDED && U.agl < -0.5) below++;
    for (; cur < F.evHead; cur++) { const e = F.ev[cur % F.ev.length]; if (e.n === S.EV.DISCOVER || e.n === S.EV.LOOT || (e.n === S.EV.GROUND && e.b)) evs.push(e.n); }
    if (U.st === SF.SURF.LANDED) {   // the world spins under the ship: in its frame the ship must not move
      landedT += S.DT; if (landedT < 0.25) continue;
      const W0 = F.worlds[U.w], l = { x: 0, y: 0, z: 0 }; S.qinv(W0.q, F.player.pos.x - W0.c.x, F.player.pos.y - W0.c.y, F.player.pos.z - W0.c.z, l);
      if (!landedLocal) landedLocal = l; else drift = Math.max(drift, Math.hypot(l.x - landedLocal.x, l.y - landedLocal.y, l.z - landedLocal.z));
    }
  }
  const N_ = SF.SURF, want = [N_.SPACE, N_.DESCENT, N_.ENTRY, N_.FLIGHT, N_.LANDING, N_.LANDED, N_.TAKEOFF, N_.FLIGHT, N_.ASCENT, N_.SPACE];
  const name = (x) => Object.keys(N_).find((kk) => N_[kk] === x);
  ok(want.every((s, i) => seq[i] === s), `descent to take-off in order: ${seq.map(name).join(' > ')}`);
  ok(heat > 0.5, `the entry heats the hull (peak ${(heat * 100).toFixed(0)}%)`);
  ok(below === 0, 'never under the ground');
  ok(evs.includes(S.EV.GROUND) && evs.includes(S.EV.DISCOVER), 'touchdown and a discovery');
  ok(landedT > 1 && drift < 0.05, `a landed ship rides with the spinning world (${drift.toFixed(3)} m over ${landedT.toFixed(1)} s)`);
  ok(F.surf.found >= 1 && F.t < 420, `a site found and back in orbit in ${F.t.toFixed(0)} s`);
}
{ // a hot manual entry, the air bleeds the speed; the ground is solid
  const t = 'ocean', F = flightAt(t, { god: false }), k = byType[t].k, p = F.player;
  SF.placeOver(F, k, PL.surfaceFor(byType[t].bp, k).atmo.top * 1.15, 50, 0, 0);
  const U = F.surf, up = U.up, f = { x: 0, y: 0, z: 0 }; S.shipFwd(p, f);
  S.qlook(p.q, f.x - up.x * 0.8, f.y - up.y * 0.8, f.z - up.z * 0.8, up.x, up.y, up.z);
  p.spd = 1400; F.cruise = 2; F.cruiseV = 1400; S.shipFwd(p, f); p.vel.x = f.x * 1400; p.vel.y = f.y * 1400; p.vel.z = f.z * 1400;
  F.nav = F.pois.findIndex((x) => x.kind !== 'planet');   // cruising past the world, not to it: no drop-out at its approach point
  F.input.aimMode = 0; F.input.pitch = 0; F.input.thr = 0.6; F.input.touched = true;
  let entry = false, minSp = 1e9;
  for (let i = 0; i < 60 * 25 && F.surf.st !== SF.SURF.FLIGHT; i++) { S.stepFlight(F); if (F.surf.st === SF.SURF.ENTRY) entry = true; }
  ok(entry && F.surf.st === SF.SURF.FLIGHT && p.spd < 420, `a hot manual dive is an entry; the air slows it to ${p.spd.toFixed(0)} m/s`);
  ok(!F.cruise, 'the cruise drive drops in thick air');
  // straight into the ground, assist off: a bounce that hurts, never through
  U.assist = false; S.shipFwd(p, f); S.qlook(p.q, f.x - up.x * 3, f.y - up.y * 3, f.z - up.z * 3, up.x, up.y, up.z);
  const h0 = p.hull; let hit = 0, cur = F.evHead, low = 1e9;
  for (let i = 0; i < 60 * 30 && !hit; i++) { S.stepFlight(F); F.input.thr = 1; low = Math.min(low, F.surf.agl); for (; cur < F.evHead; cur++) if (F.ev[cur % F.ev.length].n === S.EV.GROUND) hit++; }
  ok(hit > 0 && low > -1 && p.hull < h0, `the ground is solid: impact, damage ${(h0 - p.hull).toFixed(0)}, lowest ${low.toFixed(1)} m`);
  // no landing on open water
  if (F.surf.water || true) {
    SF.placeOver(F, k, 120, 50, 0, 40);
    let tries = 0; while (!F.surf.water && tries++ < 40) SF.placeOver(F, k, 120, 50, tries * 9, 40);
    if (F.surf.water) ok(S.cmd(F, 'land') === false && F.surf.st === SF.SURF.FLIGHT, 'no landing on open water');
  }
}
{ // gas giants: no ground to land on, a pressure floor that pushes back
  const F = flightAt('gas', { god: true }), k = byType.gas.k;
  SF.placeOver(F, k, 800, 50, 0, 120);
  ok(S.cmd(F, 'land') === false, 'a gas giant refuses a landing');
  const p = F.player, up = F.surf.up, f = { x: 0, y: 0, z: 0 }; S.shipFwd(p, f);
  S.qlook(p.q, f.x - up.x * 2, f.y - up.y * 2, f.z - up.z * 2, up.x, up.y, up.z);
  F.input.aimMode = 0; F.input.thr = 1; F.surf.assist = false; let low = 1e9, warn = 0, cur = F.evHead;
  for (let i = 0; i < 60 * 20; i++) { S.stepFlight(F); low = Math.min(low, F.surf.alt); for (; cur < F.evHead; cur++) { const e = F.ev[cur % F.ev.length]; if (e.n === S.EV.SURF && e.k === 'cz_hud_pressure') warn++; } }
  ok(low > 60, `the pressure floor holds (lowest ${low.toFixed(0)} m above the cloud tops)`);
  ok(warn > 0, 'and warns');
}
{ // the review hook puts a ruin in daylight ahead; a landing never picks a ruin's own ground; a gas giant has no chart
  const t = 'rocky', F = flightAt(t, { god: true }), k = byType[t].k, Sf = surf(t), p = F.player;
  const si = SF.placeNearSite(F, k, ['gate', 'archive', 'observatory'], 600, 120, 0, 35);
  ok(si >= 0, 'nearSite finds a Costellatori ruin');
  const U = F.surf, s = U.sites[si], W0 = F.worlds[k], sd = F.bp.star.dir;
  const sup = { x: 0, y: 0, z: 0 }; S.qrot(W0.q, s.st.dir[0], s.st.dir[1], s.st.dir[2], sup);
  const el = Math.asin(sup.x * sd[0] + sup.y * sd[1] + sup.z * sd[2]) * 180 / Math.PI;
  ok(Math.abs(el - 35) < 4, `the clock is moved to put the sun ${el.toFixed(1)} deg over it`);
  const d = Math.hypot(s.pos.x - p.pos.x, s.pos.y - p.pos.y, s.pos.z - p.pos.z), f = { x: 0, y: 0, z: 0 }; S.shipFwd(p, f);
  const to = (s.pos.x - p.pos.x) * f.x + (s.pos.y - p.pos.y) * f.y + (s.pos.z - p.pos.z) * f.z;
  ok(d > 500 && d < 760 && to / d > 0.9 && U.st === SF.SURF.FLIGHT && U.nav === si, `the ship flies at it from ${d.toFixed(0)} m`);
  // land right over the ruin: the spot is off its pad
  const r = Sf.R + s.st.e + 60, v = { x: 0, y: 0, z: 0 }; S.qrot(W0.q, s.st.dir[0] * r, s.st.dir[1] * r, s.st.dir[2] * r, v);
  p.pos.x = W0.c.x + v.x; p.pos.y = W0.c.y + v.y; p.pos.z = W0.c.z + v.z; p.vel.x = p.vel.y = p.vel.z = 0; p.spd = 0;
  S.stepFlight(F);
  ok(S.cmd(F, 'land') !== false && U.land, 'landing over a ruin is allowed');
  const off = Math.acos(Math.min(1, U.land.x * s.st.dir[0] + U.land.y * s.st.dir[1] + U.land.z * s.st.dir[2])) * Sf.R;
  ok(off >= s.st.r0 * 0.75 - 0.5, `but the ship sets down beside it, not on it (${off.toFixed(0)} m from the centre, pad ${s.st.r0} m)`);
  const G = flightAt('gas', { god: true }); SF.placeOver(G, byType.gas.k, 800, 50, 0, 120);
  const O = { key: '', a: {}, marker: null };
  ok(SF.surfaceObjective(G, O) === false && SF.placeNearSite(G, byType.gas.k, ['gate']) === -1, 'a gas giant has no sites to chart (the flight keeps its own objective)');
}
{ // a relic recovered from a shrine wakes the sentinels
  let done = false;
  for (const t of ['rocky', 'jungle', 'ice', 'desert', 'crystal', 'volcanic', 'ocean']) {
    const Sf = surf(t), si = Sf.sites.findIndex((s) => s.loot.relic && s.guard === 'sentinel'); if (si < 0) continue;
    const F = flightAt(t, { god: true, autopilot: true, autoLand: true }), k = byType[t].k, s = Sf.sites[si];
    SF.placeOver(F, k, 200, 50, 0, 0);
    // drop the ship right over the shrine
    const W0 = F.worlds[k], r = Sf.R + s.e + 160, v = { x: 0, y: 0, z: 0 }; S.qrot(W0.q, s.dir[0] * r, s.dir[1] * r, s.dir[2] * r, v);
    F.player.pos.x = W0.c.x + v.x; F.player.pos.y = W0.c.y + v.y; F.player.pos.z = W0.c.z + v.z; F.player.vel.x = F.player.vel.y = F.player.vel.z = 0; F.player.spd = 0;
    F.surf.nav = si; F.surf.sites[si].seen = true; F.surf.picked = true;
    let loot = 0, sentinels = 0, cur = F.evHead;
    for (let i = 0; i < 60 * 60 && !loot; i++) { S.stepFlight(F); for (; cur < F.evHead; cur++) if (F.ev[cur % F.ev.length].n === S.EV.LOOT) loot++; }
    for (let i = 0; i < 30; i++) S.stepFlight(F);
    for (const sh of F.ships) if (sh.alive && sh.nameKey === 'cz_ship_sentinel') sentinels++;
    ok(loot === 1, `${t}: the relic is recovered from the ${s.kind}`);
    ok(sentinels >= 2, `${t}: the Echo's sentinels rise (${sentinels})`);
    const air = F.ships.filter((sh) => sh.alive && sh.nameKey === 'cz_ship_sentinel');
    for (let i = 0; i < 60 * 10; i++) S.stepFlight(F);
    ok(air.every((sh) => !sh.alive || SF.groundAt(F, sh.pos.x, sh.pos.y, sh.pos.z) > -1), 'the sentinels fly above the ground');
    done = true; break;
  }
  ok(done, 'a guarded relic exists somewhere in the sector');
}

// ---- 6. through the game module: discoveries in the web save ----------------------------------------------------------------
{
  const T = join(tmpdir(), 'cz-worlds-' + process.pid); mkdirSync(T, { recursive: true });
  writeFileSync(join(T, 'shim.mjs'), 'export const defineGame = d => d;\nexport default defineGame;\n');
  writeFileSync(join(T, 'cz-3d-stub.mjs'), 'export async function createRenderer() { return { frame() {}, resize() {}, dispose() {} }; }\n');
  let src = readFileSync(join(GW, 'constellations.js'), 'utf8');
  const rel = (p) => pathToFileURL(join(GW, p)).href;
  src = src.split("'/apps/games/nucleo-game.js'").join("'./shim.mjs'").split("'/apps/games/games/constellations-3d.js'").join("'./cz-3d-stub.mjs'")
    .replace(/'\/apps\/games\/games\/([^']+)'/g, (_, p) => `'${rel(p)}'`);
  writeFileSync(join(T, 'constellations.mjs'), src);
  let disk = null, webDisk = null; const posts = [];
  globalThis.window = { __czAutopilot: true, __czAutoLand: true, __czGod: true, __czNoBrief: true, __czEncounter: false, __czClock: 1000 };
  globalThis.fetch = async (u, o) => {
    const s = String(u);
    if (s.startsWith('/api/fs/')) {
      if (s.startsWith('/api/fs/mkdir')) return { ok: true, status: 200, json: async () => ({ ok: true }) };
      if (s.startsWith('/api/fs/write')) { webDisk = JSON.parse(o.body); return { ok: true, status: 200, json: async () => ({ ok: true }) }; }
      if (s.startsWith('/api/fs/read')) return webDisk ? { ok: true, status: 200, text: async () => JSON.stringify(webDisk) } : { ok: false, status: 404, text: async () => '' };
    }
    if (o && o.method === 'POST') { disk = JSON.parse(o.body); posts.push(disk); return { ok: true, status: 200, json: async () => ({ ok: true }) }; }
    if (!disk) return { ok: false, status: 404, json: async () => ({}) };
    return { ok: true, status: 200, json: async () => JSON.parse(JSON.stringify(disk)) };
  };
  const realRandom = Math.random; let rs = 0x2545F491; Math.random = () => { rs ^= rs << 13; rs ^= rs >>> 17; rs ^= rs << 5; return (rs >>> 0) / 4294967296; };
  const mod = await import(pathToFileURL(join(T, 'constellations.mjs')).href);
  const g = mod.default, cz = mod.__cz;
  let st = g.setup(); g.mount({ parentElement: null }, {});
  for (let i = 0; i < 50 && st.phase !== 'new_run'; i++) { await new Promise((r) => setTimeout(r, 5)); st = g.tick(st, 50); }
  st = g.reduce(st, { type: 'confirm' });
  const shape = Object.keys(cz.run).sort().join();
  // a system whose home world is dry ground
  const si = cz.model.sector.findIndex((s, i) => s.faction !== 3);
  globalThis.window.__cz.dev.goto(si);
  const launch = () => { st = g.reduce(st, { type: 'screen', to: 0 }); st = { ...st, focus: { ...st.focus, bridge: 0 } }; st = g.reduce(st, { type: 'confirm' }); return cz.flight; };
  const fly = (F, max) => { while (!F.done && F.t < max && !(F.surf && F.surf.left)) { S.stepFlight(F); if ((F.step % 4) === 0) st = g.tick(st, 50); } };
  let F = launch();
  ok(st.phase === 'combat' && F.mission.kind === 'explore', 'a free flight from the bridge');
  const wi = F.bp.planets.findIndex((p) => p.type !== 'gas');
  const before = { cr: cz.run.credits, rep: cz.run.rep.slice(), relic: cz.run.cargo[6] };
  globalThis.window.__cz.dev.overWorld(wi, 300, 50, 0, 120);
  let found = null, cur = F.evHead;
  const tStart = F.t;
  while (!found && F.t < tStart + 300) { S.stepFlight(F); if ((F.step % 4) === 0) st = g.tick(st, 50); for (; cur < F.evHead; cur++) { const e = F.ev[cur % F.ev.length]; if (e.n === S.EV.DISCOVER) found = e.v; } }
  ok(!!found, `a site is discovered on the surface (${found && found.kind}, ${(F.t - tStart).toFixed(0)} s)`);
  for (let i = 0; i < 8; i++) st = g.tick(st, 50);
  await new Promise((r) => setTimeout(r, 900));   // the web save is debounced
  const key = cz.run.sector + ':' + cz.run.sys + ':' + wi;
  ok(webDisk && webDisk.found && ((webDisk.found[key] >>> 0) & (1 << found.i)), 'the discovery is in the WEB save (/data/costellazioni/web.json)');
  ok(cz.run.credits >= before.cr + found.loot.cr, `it pays its reward once (+${cz.run.credits - before.cr} cr)`);
  if (found.loot.rep >= 0 && found.loot.rep < 4) ok(cz.run.rep[found.loot.rep] >= Math.min(100, before.rep[found.loot.rep] + found.loot.repN), 'and the reputation it is worth');
  if (['gate', 'archive', 'observatory'].includes(found.kind)) ok(webDisk.codex[found.kind] != null, 'a ruin opens its codex entry');
  ok(posts.length > 0 && Object.keys(posts[posts.length - 1]).sort().join() === shape, 'the shared save keeps its contract');
  ok(!posts.some((p) => 'found' in p || 'looted' in p || 'disc' in p), 'nothing web-only reaches the shared save');
  // the next flight knows: the same site is found already and pays nothing again
  F.done = true; F.outcome = 3; st = g.tick(st, 50);
  const cr1 = cz.run.credits; F = launch();
  globalThis.window.__cz.dev.overWorld(wi, 300, 50, 0, 0);
  const U = F.surf;
  ok(U.w === wi && U.sites[found.i].found && U.sites[found.i].seen, 'a new flight remembers the site as found');
  let again = 0; cur = F.evHead;
  for (let i = 0; i < 60 * 20; i++) { S.stepFlight(F); for (; cur < F.evHead; cur++) { const e = F.ev[cur % F.ev.length]; if (e.n === S.EV.DISCOVER && e.v.i === found.i && e.b === wi) again++; } }
  ok(again === 0 && cz.run.credits >= cr1, 'a found site never pays twice');
  // the web save module itself
  const WS = await import(url('constellations-web.js'));
  const w = WS.freshWeb(5);
  ok(WS.markFound(w, 2, 3, 1, 4) && !WS.markFound(w, 2, 3, 1, 4) && WS.foundMask(w, 2, 3, 1) === 16 && WS.foundCount(w) === 1, 'found sites: a bit per site per world');
  ok(WS.markLooted(w, 2, 3, 1, 4) && WS.wasLooted(w, 2, 3, 1, 4) && !WS.wasLooted(w, 2, 3, 0, 4), 'recovered relics are remembered per world');
  delete globalThis.window; Math.random = realRandom;
  try { rmSync(T, { recursive: true, force: true }); } catch {}
}

// ---- 7. every surface string in five languages ---------------------------------------------------------------------------------
{
  const cats = Object.fromEntries(['it', 'en', 'es', 'fr', 'de'].map((l) => [l, JSON.parse(readFileSync(join(root, 'apps/games/www/i18n.' + l + '.json'), 'utf8'))]));
  const used = new Set();
  for (const f of ['stelle/surface.js', 'stelle/hud.js', 'constellations-3d.js', 'stelle/sim.js']) for (const m of readFileSync(join(GW, f), 'utf8').matchAll(/'(cz_[a-z0-9_]+)'/g)) used.add(m[1]);
  for (const k of PL.SITE_KINDS) { used.add('cz_site_' + k); used.add('cz_c_found_' + k); }
  const missing = [];
  for (const k of used) { if (/_$/.test(k)) continue; for (const l of Object.keys(cats)) if (!cats[l][k]) missing.push(l + ':' + k); }
  ok(missing.length === 0, `every HUD / sim string exists in five languages (${used.size} keys)` + (missing.length ? ': ' + missing.slice(0, 8).join(', ') : ''));
}

console.log(`\ncostellazioni worlds: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
