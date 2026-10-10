// Host-side tests for the Costellazioni procedural model kit (apps/games/www/games/stelle/kit.js) — no browser.
// Builds every ship class in every faction palette (+ the ace liveries), the courier's swivelling nacelles, every
// turret, stations of each faction over several seeds, both beacon states, rocks, debris and the cockpit, and
// checks the geometry contract the renderer relies on:
//   1. attributes: finite positions / normals / colours / glow, unit normals, no zero-area triangle, winding that
//      agrees with the normal (a zero or NaN normal becomes NaN pixels in the hull shader);
//   2. budgets: fighters <= 12k triangles, the Choir <= 20k, capitals <= 60k, stations <= 200k, beacon <= 40k;
//   3. fit: models within 1.7x CLS rad, guns / engines / subsystems within 1.5 m of the hull box, turret mounts on
//      geometry; the courier's nacelle parts pivot on their hinges and their nozzle exits sit on CLS.lucciola.eng;
//   4. stations: blueprint shape, <= 60 collision spheres inside the radius, a docking lane (mouth .. 600 m out and
//      the bay behind the mouth) clear of spheres and geometry; deterministic builds;
//   5. the hull material: one shared program, uEngine uniform, every shader injection point found.
// Run: node tools/games-host/test-costellazioni-kit.mjs
import { pathToFileURL, fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url)), root = join(here, '..', '..');
const GW = join(root, 'apps/games/www/games');
const url = (p) => pathToFileURL(join(GW, p)).href;
let pass = 0, fail = 0; const ok = (c, m) => { if (c) pass++; else { fail++; console.log('  FAIL:', m); } };

const THREE = await import(pathToFileURL(join(root, 'apps/games/www/vendor/three.module.min.js')).href);
const K = await import(url('stelle/kit.js'));
const W = await import(url('stelle/world.js'));
const S = await import(url('stelle/sim.js'));
const { CLS, F_GILDA, F_CUSTODI, F_RELITTI, F_ECO, F_PLAYER } = S;

// ---- geometry contract ----------------------------------------------------------------------------------------
function inspect(name, g, budget) {
  const P = g.attributes.position.array, N = g.attributes.normal.array, C = g.attributes.color && g.attributes.color.array, G = g.attributes.aGlow && g.attributes.aGlow.array;
  const n = P.length / 3, tris = n / 3;
  ok(!g.index && P.length % 9 === 0 && N.length === P.length && C && C.length === P.length && G && G.length === n, `${name}: non-indexed triangles with position/normal/color/aGlow`);
  let bad = 0, unit = 0, zero = 0, wind = 0, glow = 0;
  const box = [Infinity, Infinity, Infinity, -Infinity, -Infinity, -Infinity];
  for (let i = 0; i < P.length; i++) if (!Number.isFinite(P[i]) || !Number.isFinite(N[i]) || !Number.isFinite(C[i])) bad++;
  for (let v = 0; v < n; v++) {
    const l = Math.hypot(N[v * 3], N[v * 3 + 1], N[v * 3 + 2]);
    if (!(Math.abs(l - 1) <= 1e-3)) unit++;
    const a = G[v]; if (!Number.isFinite(a) || a < -3 || a > 13 || (a > 9.5 && a < 10)) glow++;
    for (let k = 0; k < 3; k++) { box[k] = Math.min(box[k], P[v * 3 + k]); box[k + 3] = Math.max(box[k + 3], P[v * 3 + k]); }
  }
  for (let t = 0; t < tris; t++) {
    const i = t * 9, ux = P[i + 3] - P[i], uy = P[i + 4] - P[i + 1], uz = P[i + 5] - P[i + 2], vx = P[i + 6] - P[i], vy = P[i + 7] - P[i + 1], vz = P[i + 8] - P[i + 2];
    const cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
    if (!(Math.hypot(cx, cy, cz) > 1e-10)) { zero++; continue; }
    const sx = N[i] + N[i + 3] + N[i + 6], sy = N[i + 1] + N[i + 4] + N[i + 7], sz = N[i + 2] + N[i + 5] + N[i + 8];
    if (cx * sx + cy * sy + cz * sz <= 0) wind++;
  }
  ok(bad === 0, `${name}: ${bad} non-finite attribute values`);
  ok(unit === 0, `${name}: ${unit} normals are not unit length`);
  ok(zero === 0, `${name}: ${zero} zero-area triangles`);
  ok(wind === 0, `${name}: ${wind} triangles wound against their normal`);
  ok(glow === 0, `${name}: ${glow} aGlow values outside the encoding (-3..9.5 surfaces and lights, 10..13 engine roles)`);
  if (budget) ok(tris <= budget, `${name}: ${tris} triangles over the ${budget} budget`);
  let rmax = 0; for (let v = 0; v < n; v++) rmax = Math.max(rmax, Math.hypot(P[v * 3], P[v * 3 + 1], P[v * 3 + 2]));
  return { tris, box, rmax, P, N, G };
}
const boxDist = (p, b) => Math.hypot(Math.max(b[0] - p[0], 0, p[0] - b[3]), Math.max(b[1] - p[1], 0, p[1] - b[4]), Math.max(b[2] - p[2], 0, p[2] - b[5]));
const nearest = (P, p) => { let d = Infinity; for (let i = 0; i < P.length; i += 3) d = Math.min(d, Math.hypot(P[i] - p[0], P[i + 1] - p[1], P[i + 2] - p[2])); return d; };

// ---- 1-3. every ship class, every palette, the aces ------------------------------------------------------------
const SHIPS = Object.keys(CLS).filter((ck) => ck !== 'station' && ck !== 'beacon');
const counts = {};
for (const ck of SHIPS) {
  const C = CLS[ck], budget = C.cap ? 60000 : ck === 'choir' ? 20000 : 12000;
  const facs = ck === 'lucciola' ? [F_PLAYER] : [F_GILDA, F_CUSTODI, F_RELITTI, F_ECO];
  const home = ck === 'lucciola' ? F_PLAYER : C.fac != null ? C.fac : F_GILDA;
  for (const fac of facs) {
    const g = K.shipGeometry(THREE, ck, fac, false), r = inspect(`${ck}/${fac}`, g, budget);
    ok(K.shipGeometry(THREE, ck, fac, false) === g, `${ck}/${fac}: geometry is cached`);
    if (fac !== home) continue;
    counts[ck] = r.tris;
    ok(r.rmax <= C.rad * 1.7, `${ck}: model reaches ${r.rmax.toFixed(1)} m, over 1.7 x rad ${C.rad}`);
    for (const p of C.guns || []) ok(boxDist(p, r.box) <= 1.5, `${ck}: gun ${p} is ${boxDist(p, r.box).toFixed(2)} m off the hull box`);
    for (const e of C.eng || []) ok(boxDist(e, r.box) <= 1.5, `${ck}: engine ${e.slice(0, 3)} is ${boxDist(e, r.box).toFixed(2)} m off the hull box`);
    for (const e of C.eng || []) ok(nearest(r.P, [e[0], e[1], e[2]]) <= Math.max(1.5, e[3] * 0.9), `${ck}: engine ${e.slice(0, 3)} has no nozzle geometry around it`);
    if (C.torpAt) ok(boxDist(C.torpAt, r.box) <= 1.5 && nearest(r.P, C.torpAt) <= 1.0, `${ck}: torpedo launcher ${C.torpAt} not on the hull`);
    if (C.tether) ok(nearest(r.P, C.tether) <= 1.0, `${ck}: tether emitter ${C.tether} not on the hull`);
    for (const s of C.sub || []) {
      ok(boxDist(s.p, r.box) <= 1.5, `${ck}: subsystem ${s.k} ${s.p} is off the hull box`);
      if (s.k === 'turret') ok(nearest(r.P, s.p) <= 2.5, `${ck}: turret mount ${s.p} floats (no barbette under it)`);
    }
    // the engine-role glow is a small hot core, never the whole nozzle disc
    const E = r.G; let lit = 0; for (let v = 0; v < E.length; v++) if (E[v] > 9.5) lit++;
    ok(lit > 0, `${ck}: has engine-role (throttle-driven) glow`);
    if (C.fac != null && ck !== 'lucciola') for (const ace of [true, 0, 1, 2, 3, 4]) inspect(`${ck}/ace ${ace}`, K.shipGeometry(THREE, ck, C.fac, ace), budget);
  }
}
for (const ck of ['censer', 'lattice', 'choir', 'reliquary']) ok(counts[ck] > 0 && K.shipGeometry(THREE, ck, CLS[ck].fac, false) !== K.shipGeometry(THREE, 'lancer', CLS[ck].fac, false), `${ck} has its own design`);
// the engine-glow cores stay small: no engine-role vertex farther than 45% of the plume radius from its eng axis
{
  for (const ck of SHIPS) {
    const C = CLS[ck], home = ck === 'lucciola' ? F_PLAYER : C.fac != null ? C.fac : F_GILDA, g = K.shipGeometry(THREE, ck, home, false);
    if (!(C.eng || []).length || ck === 'shard' || ck === 'lattice' || ck === 'choir') continue;
    const P = g.attributes.position.array, G = g.attributes.aGlow.array; let wide = 0;
    for (let v = 0; v < G.length; v++) if (G[v] > 10.6) {   // engine + engineHot (the dim intake / rim role is 10.4)
      const x = P[v * 3], y = P[v * 3 + 1], z = P[v * 3 + 2];
      const d = Math.min(...C.eng.map((e) => (Math.abs(z - e[2]) < e[3] * 2 ? Math.hypot(x - e[0], y - e[1]) / e[3] : 9)));
      if (d > 0.45) wide++;
    }
    ok(wide === 0, `${ck}: ${wide} engine-glow vertices outside the hot core (> 45% of the nozzle)`);
  }
}

// ---- the courier's swivelling nacelles -----------------------------------------------------------------------
{
  const full = K.shipGeometry(THREE, 'lucciola', F_PLAYER, false), sp = K.shipParts(THREE, 'lucciola', F_PLAYER, false);
  ok(sp && sp.body && Array.isArray(sp.parts) && sp.parts.length === 2, 'shipParts(lucciola) gives a body and two parts');
  const body = inspect('lucciola body', sp.body, 12000);
  ok(body.box[0] > -3.4 && body.box[3] < 3.4, `the courier body excludes the nacelles (body |x| <= ${Math.max(-body.box[0], body.box[3]).toFixed(2)})`);
  let total = sp.body.attributes.position.count;
  for (const part of sp.parts) {
    total += part.geo.attributes.position.count;
    const e = CLS.lucciola.eng[part.side < 0 ? 0 : 1], pv = part.pivot, r = inspect(`lucciola part ${part.side}`, part.geo, 6000);
    ok(part.kind === 'pod' && (part.side === -1 || part.side === 1) && Math.sign(pv[0]) === part.side, `part ${part.side}: kind pod, side matches its pivot`);
    ok(Math.abs(pv[1] - e[1]) <= 0.3, `part ${part.side}: the swivel axis passes through the nacelle axis (pivot y ${pv[1]} vs eng y ${e[1]})`);
    ok(Math.abs(pv[0]) > 2.5 && Math.abs(pv[0]) < Math.abs(e[0]), `part ${part.side}: the pivot is the pylon hinge between hull and nacelle`);
    ok(Math.abs(r.box[5] + pv[2] - e[2]) <= 0.3, `part ${part.side}: nozzle exit z ${(r.box[5] + pv[2]).toFixed(2)} on eng z ${e[2]}`);
    const P = r.P; let sx = 0, sy = 0, c = 0;
    for (let i = 0; i < P.length; i += 3) if (P[i + 2] + pv[2] > e[2] - 0.15) { sx += P[i] + pv[0]; sy += P[i + 1] + pv[1]; c++; }
    ok(c > 0 && Math.hypot(sx / c - e[0], sy / c - e[1]) <= 0.3, `part ${part.side}: nozzle exit centred on eng (${(sx / c).toFixed(2)}, ${(sy / c).toFixed(2)})`);
    // the part sits around its own pivot: the hinge knuckle touches the origin plane
    ok(nearest(P, [0, 0, 0]) <= 0.6, `part ${part.side}: geometry is built around its pivot`);
  }
  ok(total === full.attributes.position.count, 'body + parts = the full static model');
  const lone = K.shipParts(THREE, 'lancer', F_GILDA, false);
  ok(lone.parts.length === 0 && lone.body === K.shipGeometry(THREE, 'lancer', F_GILDA, false), 'classes without parts: body is the full model, parts empty');
}

// ---- turrets ----------------------------------------------------------------------------------------------------
for (const fac of [F_GILDA, F_CUSTODI, F_RELITTI, F_ECO]) {
  const t = K.turretGeometry(THREE, fac);
  const base = inspect(`turret ${fac} base`, t.base, 4000), head = inspect(`turret ${fac} head`, t.head, 4000), gun = inspect(`turret ${fac} gun`, t.gun, 4000);
  ok(base.box[1] >= -0.05 && base.box[4] <= 2.2 && head.box[1] >= -0.05, `turret ${fac}: base and head stand on their mount`);
  ok(Math.abs(gun.box[2] + 7.5) <= 0.3 && gun.box[5] <= 1.2, `turret ${fac}: barrels reach the muzzle at z -7.5 (${gun.box[2].toFixed(2)})`);
  ok(K.turretGeometry(THREE, fac) === t, `turret ${fac}: cached`);
}

// ---- stations ---------------------------------------------------------------------------------------------------
const segDist = (c, a, b) => { const ab = [b[0] - a[0], b[1] - a[1], b[2] - a[2]], ac = [c[0] - a[0], c[1] - a[1], c[2] - a[2]], L2 = ab[0] ** 2 + ab[1] ** 2 + ab[2] ** 2, t = Math.max(0, Math.min(1, (ac[0] * ab[0] + ac[1] * ab[1] + ac[2] * ab[2]) / L2)); return Math.hypot(c[0] - a[0] - ab[0] * t, c[1] - a[1] - ab[1] * t, c[2] - a[2] - ab[2] * t); };
for (const fac of [F_GILDA, F_CUSTODI, F_RELITTI]) for (const seed of [987654, 1, 42, 7, 123456789]) {
  const tag = `station ${fac}/${seed}`, bp = W.stationBlueprint(seed, 1, 3, fac, null);
  ok(Array.isArray(bp.pos) && bp.faction === fac && Array.isArray(bp.mods) && bp.mods.length > 3 && Array.isArray(bp.collide) && bp.radius > 0 && Number.isFinite(bp.seed) && Number.isFinite(bp.lights), `${tag}: blueprint shape`);
  ok(bp.collide.length > 0 && bp.collide.length <= 60, `${tag}: ${bp.collide.length} collision spheres (1..60)`);
  ok(bp.collide.every((c) => c.length === 4 && c.every(Number.isFinite) && c[3] > 0 && Math.hypot(c[0], c[1], c[2]) + c[3] <= bp.radius + 1), `${tag}: collision spheres inside the radius ${bp.radius.toFixed(0)}`);
  const d = bp.dock;
  ok(d && d.p.length === 3 && d.n.length === 3 && Math.abs(Math.hypot(...d.n) - 1) < 1e-6 && Math.hypot(...d.p) <= bp.radius, `${tag}: dock mouth inside the radius with a unit normal`);
  if (!d) continue;
  const a = [d.p[0] - d.n[0] * 26, d.p[1] - d.n[1] * 26, d.p[2] - d.n[2] * 26], b = [d.p[0] + d.n[0] * 600, d.p[1] + d.n[1] * 600, d.p[2] + d.n[2] * 600];
  const blocked = bp.collide.filter((c) => segDist(c, a, b) < c[3] + 7);
  ok(blocked.length === 0, `${tag}: ${blocked.length} collision spheres in the docking lane ${JSON.stringify(blocked.slice(0, 2))}`);
  const st = K.stationGeometry(THREE, bp), r = inspect(tag, st.geo, 200000);
  ok(st.lights.length > 0 && st.lights.every((l) => l.length === 7 && l.every(Number.isFinite)), `${tag}: running / guide lights`);
  let inLane = 0; const P = r.P;
  for (let i = 0; i < P.length; i += 9) for (const w of [[1, 0, 0], [0, 1, 0], [0, 0, 1], [1 / 3, 1 / 3, 1 / 3]]) {
    const q = [0, 1, 2].map((k) => P[i + k] * w[0] + P[i + 3 + k] * w[1] + P[i + 6 + k] * w[2]);
    if (segDist(q, a, b) < 8.5) { inLane++; break; }
  }
  ok(inLane === 0, `${tag}: ${inLane} triangles reach into the docking lane`);
  ok(r.rmax <= bp.radius * 1.15, `${tag}: geometry within the station radius (${r.rmax.toFixed(0)} vs ${bp.radius.toFixed(0)})`);
  if (seed === 987654) { const again = K.stationGeometry(THREE, W.stationBlueprint(seed, 1, 3, fac, null)).geo.attributes.position.array; ok(again.length === P.length && again.every((v, i) => v === P[i]), `${tag}: deterministic`); }
}
// systems built by the generator: every inhabited one has a dock
for (let i = 0; i < 10; i++) { const st = W.stationBlueprint(77, 2, i, i % 3, null); ok(!!st.dock, `generated station ${i} has a dock`); }

// ---- beacon -----------------------------------------------------------------------------------------------------
{
  const emissive = (g) => { const C = g.attributes.color.array, G = g.attributes.aGlow.array; let r = 0, b = 0; for (let v = 0; v < G.length; v++) if (G[v] > 0.5 && G[v] < 9.5) { r += C[v * 3] * G[v]; b += C[v * 3 + 2] * G[v]; } return [r, b]; };
  const res = {};
  for (const lit of [true, false]) {
    const bc = { lit, height: 260, radius: 70 }, out = K.beaconGeometry(THREE, bc), r = inspect(`beacon lit=${lit}`, out.geo, 40000);
    let horiz = 0; for (let i = 0; i < r.P.length; i += 3) horiz = Math.max(horiz, Math.hypot(r.P[i], r.P[i + 2]));
    ok(horiz <= bc.radius && r.box[1] >= -0.5 && r.box[4] <= bc.height + 0.5, `beacon lit=${lit}: inside the sim volume (r ${horiz.toFixed(1)}, y ${r.box[1].toFixed(1)}..${r.box[4].toFixed(1)})`);
    ok(Array.isArray(out.core) && out.core[1] > bc.height * 0.6 && out.core[1] < bc.height && nearest(r.P, out.core) < 20, `beacon lit=${lit}: crystal heart at core ${out.core}`);
    res[lit] = emissive(out.geo);
  }
  ok(res[true][0] > res[true][1] && res[false][1] > res[false][0] && res[true][0] > res[false][0] * 2, 'beacon: lit burns gold, dead is cold cyan and dimmer');
}

// ---- rocks, debris, cockpit -------------------------------------------------------------------------------------
for (let shape = 0; shape < 4; shape++) for (const seed of [1, 2, 3, 17]) {
  const g = K.rockGeometry(THREE, shape, seed), N = g.attributes.normal.array, P = g.attributes.position.array;
  let bad = 0; for (let i = 0; i < N.length; i += 3) if (!(Math.abs(Math.hypot(N[i], N[i + 1], N[i + 2]) - 1) <= 1e-3) || !Number.isFinite(P[i])) bad++;
  ok(bad === 0, `rock ${shape}/${seed}: ${bad} bad normals`);
}
inspect('debris', K.debrisGeometry(THREE), 2000);
inspect('cockpit', K.cockpitGeometry(THREE), 4000);

// ---- the hull material ------------------------------------------------------------------------------------------
{
  const m = K.hullMaterial(THREE, {}), m2 = K.hullMaterial(THREE, { metal: 0.7, rock: true }), U = m.userData.U;
  ok(U && U.uEngine && U.uEngine.value === 1 && U.uGlowMul && U.uFlash && U.uScorch, 'hullMaterial: uniforms incl. uEngine (default 1)');
  ok(m.customProgramCacheKey() === 'stelle-hull' && m2.customProgramCacheKey() === m.customProgramCacheKey(), 'hullMaterial: one shared program for every hull');
  const sh = { uniforms: {}, vertexShader: THREE.ShaderLib.standard.vertexShader, fragmentShader: THREE.ShaderLib.standard.fragmentShader };
  m.onBeforeCompile(sh);
  ok(sh.uniforms.uEngine === U.uEngine, 'hullMaterial: uEngine bound to the program');
  for (const mark of ['attribute float aGlow', 'vGlow = aGlow']) ok(sh.vertexShader.includes(mark), `vertex shader injection: ${mark}`);
  for (const mark of ['uniform float uEngine', 'hullWear = ', 'roughnessFactor = mix', 'metalnessFactor = mix', 'normal = nb / nl', '(vGlow - 10.0) * uEngine', 'totalEmissiveRadiance +=']) ok(sh.fragmentShader.includes(mark), `fragment shader injection: ${mark}`);
}

console.log('  triangles:', Object.entries(counts).map(([k, v]) => `${k} ${v}`).join(', '));
console.log(`\ncostellazioni-kit: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
