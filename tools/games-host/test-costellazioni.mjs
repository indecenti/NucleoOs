// Host-side tests for the web Costellazioni (apps/games/www/games/stelle + constellations.js) — no browser.
//   1. the shared generator layer (numbers + flavor text the firmware mirrors) is byte-for-byte unchanged;
//   2. the web-only visual blueprint (stelle/world.js) is deterministic and varies between systems;
//   3. the 6DOF flight model: throttle sweet spot, boost, drift, power pips, front/back shields, laser heat,
//      missile lock + flares, tether, capital subsystems;
//   4. every mission kind (patrol, hunt, duel, escort, defend, sweep, free ambush) runs to completion with
//      the Gilda and Relitti rosters, within a per-step time budget;
//   5. the save contract: a won sortie writes credits / kills / hull / reputation exactly like the rail shooter.
//   6. M2: the web-only world (planets you can reach, moons, the arrival point), free flight with points of
//      interest and the supercruise, docking, the jump drive, the beacon relight (the Echo's calm and hostile
//      answers), the Custodi / Echo rosters (torpedoes, phase blinks, shield links, chords), the codex catalog in
//      five languages, and the web save written NEXT TO the shared save, which keeps its contract.
// Run: node tools/games-host/test-costellazioni.mjs (part of `npm run games:gate`).
import { readFileSync, writeFileSync, mkdirSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { pathToFileURL, fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { createHash } from 'node:crypto';

const here = dirname(fileURLToPath(import.meta.url)), root = join(here, '..', '..');
const GW = join(root, 'apps/games/www/games');
const url = (p) => pathToFileURL(join(GW, p)).href;
let pass = 0, fail = 0; const ok = (c, m) => { if (c) pass++; else { fail++; console.log('  FAIL:', m); } };

const G = await import(url('constellations-gen.js'));
const W = await import(url('stelle/world.js'));
const S = await import(url('stelle/sim.js'));

// ---- 1. shared layer unchanged (hash of genSector + genMissions numbers + the Italian flavor fields) ----
{
  const h = createHash('sha256');
  for (const seed of [1, 0xC057E11A, 123456789, 4000000000]) for (const sector of [0, 1, 5, 13]) {
    const SEC = G.genSector(seed, sector); h.update(JSON.stringify(SEC));
    for (let i = 0; i < SEC.length; i++) for (const m of G.genMissions(seed, sector, i, SEC[i].faction)) {
      const { flavor, ...num } = m; h.update(JSON.stringify(num));
      h.update(JSON.stringify([flavor.arch, flavor.title, flavor.brief, flavor.name, flavor.gang, flavor.rarity, flavor.mods]));
    }
  }
  ok(h.digest('hex') === '1c895c8a6f348137da53aa022238ec9dbabc000d22a80891bae567231fa3a7ed', 'shared generator output is unchanged (firmware parity layer)');
  const fl = G.genMissions(123, 2, 1, 0).map((m) => m.flavor);
  ok(fl.every((f) => f.archK >= 0 && f.archK <= 5 && f.gangI >= 0 && f.gangI < 8 && Array.isArray(f.modI)), 'flavor also carries its index form (for the five-language UI)');
}

// ---- 2. world blueprint ------------------------------------------------------------------------------------
const SEC = G.genSector(987654, 3);
const bp = (i, lit = false) => W.systemBlueprint(987654, 3, i, SEC[i], lit);
{
  const a = bp(2), b = bp(2), c = bp(5);
  const strip = (x) => JSON.stringify(x, (k, v) => (v instanceof Float32Array || v instanceof Uint8Array ? Array.from(v) : v));
  ok(strip(a) === strip(b), 'blueprint is deterministic');
  ok(strip(a) !== strip(c), 'different systems look different');
  ok(a.planets.length >= 2 && a.planets.length <= 6 && a.field.n > 400 && a.star.size > 0, 'blueprint has a star, 2-6 planets and an asteroid field');
  let stations = 0, eco = 0;
  for (let i = 0; i < SEC.length; i++) { const x = bp(i); if (x.station) stations++; if (SEC[i].faction === 3) { eco++; ok(!x.station, 'Echo systems have no station'); } }
  ok(stations + eco === SEC.length, 'every inhabited system has a station');
  const st = a.station || bp(0).station;
  if (st) ok(st.collide.length > 0 && st.mods.length > 3, 'station blueprint has modules and collision volumes');
}

// ---- 3. flight model ---------------------------------------------------------------------------------------------
const RUN = { hull: 100, hull_max: 100, shield_max: 40, weapon: 1, sensors: 1 };
const ccOf = (type, arch, foeFac, extra = {}) => ({ type, arch, foeFac, waves: 7, perWave: 3, foeHp: 50, foeDmg: 10, foeSpeed: 85, ace: 1, rewardCr: 900, killCr: 22, repFac: foeFac === 0 ? 2 : 0, mission: true, targetName: 'Vexnor', gang: 1, ...extra });
function flight(cc, o = {}) { return S.createFlight({ bp: bp(o.sys ?? 1), cc, run: { ...RUN, ...(o.run || {}) }, seed: 987654, sector: 3, sys: o.sys ?? 1, slot: 0, brief: false, ...o }); }
function quiet(F) { for (const s of F.ships) if (s.alive && !s.isPlayer) s.alive = false; F.mission.waves = [{ trig: 'never', ships: [] }]; F.obs.n = 0; F.grid.clear(); }
{
  const F = flight(ccOf(0, 0, 2)); quiet(F);
  const p = F.player;
  ok(S._turnMul({ thr: 0.5, drift: false, boosting: false }) === 1 && S._turnMul({ thr: 1, drift: false, boosting: false }) < 0.75 && S._turnMul({ thr: 0.1, drift: false, boosting: false }) < 1, 'throttle sweet spot turns best in the middle');
  F.input.thr = 1; for (let i = 0; i < 600; i++) S.stepFlight(F);
  ok(Math.abs(p.spd - S.CLS.lucciola.maxSpd) < 3, 'full throttle reaches max speed (' + p.spd.toFixed(1) + ')');
  F.input.boost = true; for (let i = 0; i < 90; i++) S.stepFlight(F);
  ok(p.spd > S.CLS.lucciola.maxSpd + 40 && p.boost < 0.6, 'boost: faster and drains the meter');
  F.input.boost = false; for (let i = 0; i < 120; i++) S.stepFlight(F);
  // drift: velocity direction holds while the nose swings
  const v0 = { ...p.vel }; F.input.drift = true; F.input.aimMode = 0; F.input.yaw = 1;
  for (let i = 0; i < 40; i++) S.stepFlight(F);
  const f = { x: 0, y: 0, z: 0 }; S.shipFwd(p, f);
  const vl = Math.hypot(p.vel.x, p.vel.y, p.vel.z), cv = (p.vel.x * v0.x + p.vel.y * v0.y + p.vel.z * v0.z) / (vl * Math.hypot(v0.x, v0.y, v0.z)), cf = (f.x * v0.x + f.y * v0.y + f.z * v0.z) / Math.hypot(v0.x, v0.y, v0.z);
  ok(cv > 0.95 && cf < cv - 0.1, 'drift keeps momentum while the nose turns');
  F.input.drift = false; F.input.yaw = 0;
  // power pips
  const pp = [2, 2, 2];
  S._pipsAdd(pp, 0); S._pipsAdd(pp, 0); ok(pp[0] === 4 && pp[0] + pp[1] + pp[2] === 6, 'pips: engines to 4, total 6');
  ok(!S._pipsAdd(pp, 0) && pp[0] === 4, 'pips: capped at 4');
  S._pipsAdd(pp, 3); ok(pp.join() === '2,2,2', 'pips: 4 resets to 2/2/2');
  // shields: a hit in front drains only the front shield
  p.shF = p.shB = 20; const fw = { x: 0, y: 0, z: 0 }; S.shipFwd(p, fw);
  S.damage(F, p, 8, null, p.pos.x + fw.x * 5, p.pos.y + fw.y * 5, p.pos.z + fw.z * 5, 0);
  ok(p.shF === 12 && p.shB === 20 && p.hull === 100, 'front hit drains the front shield only');
  S.cmd(F, 'shields', 1); ok(p.shFocus === 1 && p.shF > p.shB, 'shield focus forward moves energy to the front');
  // laser heat -> overheat lockout
  F.input.fire = true; let over = false; for (let i = 0; i < 600 && !over; i++) { S.stepFlight(F); over = p.overT > 0; }
  ok(over && F.stats.shots > 10, 'sustained fire overheats the lasers (' + F.stats.shots + ' shots)');
  F.input.fire = false;
}
{ // missile lock + flares
  const F = flight(ccOf(0, 0, 2)); quiet(F);
  const p = F.player, f = { x: 0, y: 0, z: 0 }; S.shipFwd(p, f);
  const t = F.ships.find((s) => !s.alive);
  Object.assign(t, { alive: true }); // reuse a pooled slot as a static target ahead
  t.cls = S.CLS.scrapwing; t.ck = 'scrapwing'; t.team = S.TEAM_E; t.fac = 2; t.hull = t.hullMax = 500; t.shMax = 0; t.st = 13; t.gen++;
  t.pos.x = p.pos.x + f.x * 600; t.pos.y = p.pos.y + f.y * 600; t.pos.z = p.pos.z + f.z * 600; t.vel.x = t.vel.y = t.vel.z = 0; t.q = { ...p.q };
  S.cmd(F, 'target', 'ahead'); ok(p.target === t, 'target ahead picks the ship under the nose');
  F.input.aimMode = 1; F.input.aim = f; F.input.thr = 0;
  let steps = 0; while (p.lockT < p.lockTime && steps < 300) { S.stepFlight(F); steps++; }
  ok(steps >= Math.floor(p.lockTime * 60) - 2 && p.lockT >= p.lockTime, 'missile lock takes the lock time (' + (steps / 60).toFixed(2) + ' s)');
  const before = p.msl; S.cmd(F, 'missile'); S.stepFlight(F);
  ok(p.msl === before - 1 && F.msls.some((m) => m.alive && m.tgt === t), 'locked missile launches and tracks');
  let hit = false; for (let i = 0; i < 400 && !hit; i++) { S.stepFlight(F); hit = t.hull < 500; }
  ok(hit, 'the missile hits its target');
  // enemy missile at the player, decoyed by flares
  const m = F.msls.find((x) => !x.alive); Object.assign(m, { alive: true, tgt: p, tgtGen: p.gen, flare: null, life: 6, team: S.TEAM_E, own: t, dmg: 30, spd: 300, armT: 0 });
  m.pos.x = p.pos.x + 800; m.pos.y = p.pos.y; m.pos.z = p.pos.z; m.vel.x = -300; m.vel.y = m.vel.z = 0;
  let decoyed = 0; for (let k = 0; k < 20; k++) { p.flares = 3; p.flareCd = 0; m.flare = null; S.cmd(F, 'flare'); S.stepFlight(F); if (m.flare) decoyed++; }
  ok(decoyed >= 12, 'flares decoy inbound missiles most of the time (' + decoyed + '/20)');
}

{ // cruise drive: spool, fast travel, mass lock drops it
  const F = flight(ccOf(0, 0, 2)); quiet(F);
  const p = F.player; F.input.thr = 0.6;
  ok(S.cmd(F, 'cruise') && F.cruise === 1, 'cruise spools up');
  for (let i = 0; i < 100; i++) S.stepFlight(F);
  ok(F.cruise === 2, 'cruise engages after the spool');
  for (let i = 0; i < 240; i++) S.stepFlight(F);
  ok(p.spd > 1500, 'cruise reaches km/s speeds (' + p.spd.toFixed(0) + ' m/s)');
  const e = F.ships.find((s) => !s.alive); Object.assign(e, { alive: true }); e.cls = S.CLS.scrapwing; e.ck = 'scrapwing'; e.team = S.TEAM_E; e.st = 13; e.hull = e.hullMax = 50; e.gen++;
  e.pos.x = p.pos.x + 1500; e.pos.y = p.pos.y; e.pos.z = p.pos.z;
  S.stepFlight(F);
  ok(F.cruise === 0, 'a hostile inside 2.4 km mass-locks the drive');
  ok(!S.cmd(F, 'cruise') && F.cruise === 0, 'cruise refuses to start under mass lock');
  for (let i = 0; i < 120; i++) S.stepFlight(F);
  ok(p.spd < S.CLS.lucciola.boostSpd, 'dropping out sheds the cruise speed');
}

// ---- 4. missions end to end ---------------------------------------------------------------------------------------
const KINDS = [[0, 0, 'patrol'], [1, 1, 'hunt'], [1, 2, 'duel'], [2, 3, 'escort'], [3, 5, 'defend'], [3, 4, 'sweep']];
const seen = new Set();
let stepMs = 0, steps = 0;
for (const foe of [0, 2]) {
  for (const [type, arch, kind] of KINDS) {
    const F = flight(ccOf(type, arch, foe), { autopilot: true, god: true, sys: foe === 0 ? 4 : 1 });
    ok(F.mission.kind === kind, `mission kind ${kind}`);
    const t0 = performance.now();
    while (!F.done && F.t < 600) { S.stepFlight(F); steps++; for (const s of F.ships) if (s.alive && s.team === S.TEAM_E) seen.add(s.ck); }
    stepMs += performance.now() - t0;
    ok(F.outcome === 1, `${kind} vs ${foe === 0 ? 'Gilda' : 'Relitti'} completes (outcome ${F.outcome}, ${F.t.toFixed(0)} s, ${F.kills} kills)`);
    ok(F.kills >= (kind === 'escort' ? 0 : 1) && F.player.hull >= 1, `${kind}: kills and hull reported`);
  }
}
{ const F = flight({ type: 0, foeFac: 2, waves: 2, perWave: 3, foeHp: 40, foeDmg: 9, foeSpeed: 85, ace: 0, rewardCr: 0, killCr: 20, repFac: -1, mission: false }, { autopilot: true, god: true });
  ok(F.mission.kind === 'ambush', 'free patrol is an ambush');
  while (!F.done && F.t < 400) { S.stepFlight(F); steps++; for (const s of F.ships) if (s.alive && s.team === S.TEAM_E) seen.add(s.ck); }
  ok(F.outcome === 1, 'ambush completes'); }
for (const ck of ['lancer', 'bastion', 'warden', 'scrapwing', 'harpoon', 'gutter', 'hulk']) ok(seen.has(ck), `roster: ${ck} takes part`);
// mirrors the firmware: Echo-space ambushes are Echo drones, a hostile Keeper station sends Votives
for (const [ff, ck] of [[3, 'shard'], [1, 'votive'], [0, 'lancer']]) {
  const F = flight({ type: 0, foeFac: ff, waves: 2, perWave: 3, foeHp: 40, foeDmg: 9, foeSpeed: 85, ace: 0, rewardCr: 0, killCr: 20, repFac: -1, mission: false }, { autopilot: true, god: true });
  const kinds = new Set(); while (!F.done && F.t < 300) { S.stepFlight(F); for (const s of F.ships) if (s.alive && s.team === S.TEAM_E) kinds.add(s.ck); }
  ok(F.outcome === 1 && kinds.has(ck), `ambush by faction ${ff} flies ${ck} (${[...kinds].join(',')})`);
}
{ // the cast ace of a bounty follows the enemy faction (same rule as the firmware's ace_cast)
  let n = 0;
  for (const seed of [5, 77, 4242]) for (let sec = 0; sec < 6; sec++) { const SS = G.genSector(seed, sec); for (let i = 0; i < 10; i++) for (const m of G.genMissions(seed, sec, i, SS[i].faction)) {
    if (!m.ace) { ok(m.flavor.aceI === -1, 'no ace, no cast'); continue; }
    n++; const a = m.flavor.aceI; ok(m.foe_fac === 0 ? a === 0 : m.foe_fac === 2 ? (a === 2 || a === 4) : true, `cast ace ${S.ACE_NAMES[a]} for foe ${m.foe_fac}`);
  } }
  ok(n > 5, 'some contracts carry a cast ace');
}
const per = stepMs / steps;
ok(per < 0.25, `sim step within budget (${(per * 1000).toFixed(0)} µs avg)`);
console.log(`  sim: ${steps} steps, ${(per * 1000).toFixed(1)} µs/step`);

{ // capital subsystems: killing the shield generator drops the Warden's shields
  const F = flight(ccOf(3, 5, 0)); quiet(F);
  const wd = F.ships.find((s) => !s.alive);
  Object.assign(wd, { alive: true }); wd.cls = S.CLS.warden; wd.ck = 'warden'; wd.team = S.TEAM_E; wd.fac = 0; wd.cap = true; wd.hs = S.CLS.warden.hsph; wd.sub = S.CLS.warden.sub;
  wd.subHp = new Float32Array(wd.sub.length).fill(100); wd.subMax = new Float32Array(wd.sub.length).fill(100); wd.turAim = wd.sub.map(() => ({ x: 0, y: 1, z: 0 })); wd.turCd = new Float32Array(wd.sub.length);
  wd.hull = wd.hullMax = 5000; wd.shMax = 300; wd.shF = wd.shB = 0; wd.shGen = true; wd.st = 6; wd.armor = S.CLS.warden.armor; wd.pos.x = 0; wd.pos.y = 0; wd.pos.z = -2000; wd.q = { x: 0, y: 0, z: 0, w: 1 };
  const k = wd.sub.findIndex((s) => s.k === 'shield'), sp = wd.sub[k].p;
  for (let i = 0; i < 6 && wd.subHp[k] > 0; i++) S.damage(F, wd, 60, F.player, sp[0], sp[1], sp[2] - 2000, 4);
  ok(wd.subHp[k] <= 0 && !wd.shGen, 'destroying the shield generator disables the capital shields');
  const r = wd.sub.findIndex((s) => s.weak), h0 = wd.hull; S.damage(F, wd, 40, F.player, wd.sub[r].p[0], wd.sub[r].p[1], wd.sub[r].p[2] - 2000, 0);
  ok(h0 - wd.hull > 40, 'the bridge / reactor weak point amplifies damage');
}

// ---- 5. save contract through constellations.js -------------------------------------------------------------------------
{
  const T = join(tmpdir(), 'cz-host-' + process.pid); mkdirSync(T, { recursive: true });   // machine-specific file URLs: keep them out of the repo
  writeFileSync(join(T, 'shim.mjs'), 'export const defineGame = d => d;\nexport default defineGame;\n');
  writeFileSync(join(T, 'cz-3d-stub.mjs'), 'export async function createRenderer() { return { frame() {}, resize() {}, dispose() {} }; }\n');
  let src = readFileSync(join(GW, 'constellations.js'), 'utf8');
  const rel = (p) => pathToFileURL(join(GW, p)).href;
  src = src.split("'/apps/games/nucleo-game.js'").join("'./shim.mjs'")
    .split("'/apps/games/games/constellations-3d.js'").join("'./cz-3d-stub.mjs'")
    .replace(/'\/apps\/games\/games\/([^']+)'/g, (_, p) => `'${rel(p)}'`);
  writeFileSync(join(T, 'constellations.mjs'), src);
  let disk = null; const posts = [];
  globalThis.window = { __czAutopilot: true, __czGod: true, __czNoBrief: true };
  let webDisk = null; const webPaths = new Set();
  globalThis.fetch = async (u, o) => {
    const url = String(u);
    if (url.startsWith('/api/fs/')) {   // the web-only save lives next to the shared one, through the fs API
      const path = decodeURIComponent((url.split('path=')[1] || '').split('&')[0]);
      if (url.startsWith('/api/fs/mkdir')) return { ok: true, status: 200, json: async () => ({ ok: true }) };
      if (url.startsWith('/api/fs/write')) { webPaths.add(path); webDisk = JSON.parse(o.body); return { ok: true, status: 200, json: async () => ({ ok: true }) }; }
      if (url.startsWith('/api/fs/read')) return webDisk ? { ok: true, status: 200, text: async () => JSON.stringify(webDisk) } : { ok: false, status: 404, text: async () => '' };
    }
    if (o && o.method === 'POST') { disk = JSON.parse(o.body); posts.push(disk); return { ok: true, status: 200, json: async () => ({ ok: true }) }; }
    if (!disk) return { ok: false, status: 404, json: async () => ({}) };
    return { ok: true, status: 200, json: async () => JSON.parse(JSON.stringify(disk)) };
  };
  const realRandom = Math.random; let rs = Number(process.env.CZ_SEED) || 0x2545F491;   // a fixed new-run seed: the same universe every time
  Math.random = () => { rs ^= rs << 13; rs ^= rs >>> 17; rs ^= rs << 5; return (rs >>> 0) / 4294967296; };
  const mod = await import(pathToFileURL(join(T, 'constellations.mjs')).href);
  const g = mod.default, cz = mod.__cz;
  let st = g.setup();
  g.mount({ parentElement: null }, {});
  for (let i = 0; i < 50 && st.phase !== 'new_run'; i++) { await new Promise((r) => setTimeout(r, 5)); st = g.tick(st, 50); }
  ok(st.phase === 'new_run', 'no save on the card -> new run offer');
  st = g.reduce(st, { type: 'confirm' });
  ok(st.phase === 'hub' && cz.run && cz.run.credits === 600, 'new run starts at the hub');
  await new Promise((r) => setTimeout(r, 10));
  const run = cz.run;
  // find a system with missions (not Echo), jump-free: just rebuild by switching sys if needed
  st = g.reduce(st, { type: 'screen', to: 4 });
  const before = { credits: run.credits, kills: run.kills, rep: run.rep.slice() };
  st = g.reduce(st, { type: 'confirm' });
  ok(st.phase === 'combat' && cz.flight, 'launching a mission starts a flight');
  const F = cz.flight, cc = st.cc;
  while (!F.done && F.t < 600) S.stepFlight(F);
  st = g.tick(st, 50);
  ok(st.phase === 'debrief' && st.result === F.outcome, 'the flight ends in the debrief');
  await new Promise((r) => setTimeout(r, 20));
  const won = F.outcome === 1;
  const expect = won ? cc.rewardCr + cc.killCr * F.kills : (cc.mission ? 0 : cc.killCr * F.kills);
  ok(run.credits === before.credits + expect, `credits: +reward +kill_cr×kills (${run.credits - before.credits} = ${expect})`);
  ok(run.kills === before.kills + F.kills, 'kills written back');
  ok(run.hull >= 1 && run.hull <= run.hull_max, 'hull written back, never 0');
  if (won && cc.repFac >= 0) ok(run.rep[cc.repFac] === Math.min(100, before.rep[cc.repFac] + cc.repGain), 'reputation with the offering faction rises');
  const last = posts[posts.length - 1];
  ok(last && last.credits === run.credits && last.kills === run.kills && Object.keys(last).sort().join() === Object.keys(run).sort().join(), 'the POSTed save has the unchanged contract shape');
  // ---- M2 through the game module: launch -> dock, the jump drive, the relight; shared vs web save ------------------
  {
    const shape = Object.keys(run).sort().join();
    // fly a flight the way the page does: the sim at 60 Hz, the game's tick at ~15 Hz (it reads the flight's events)
    const step = (F, max = 900) => { while (!F.done && F.t < max) { S.stepFlight(F); if ((F.step % 4) === 0) { st = g.tick(st, 50); if (st.phase !== 'combat' || cz.flight !== F) return; } } };
    st = g.tick(st, 50); st = g.reduce(st, { type: 'confirm' });   // debrief -> hub
    ok(st.phase === 'hub', 'the debrief returns to the hub');
    // launch: an explore flight; the autopilot asks for a bay and docks
    globalThis.window.__czAutoDock = true; globalThis.window.__czEncounter = false;
    const sys0 = cz.run.sys;
    if (cz.model.sector[sys0].faction === 3) { cz.run.sys = cz.model.sector.findIndex((s) => s.faction !== 3); }
    globalThis.window.__cz.dev.goto(cz.run.sys);
    st = g.reduce(st, { type: 'screen', to: 0 }); st = { ...st, focus: { ...st.focus, bridge: 0 } }; st = g.reduce(st, { type: 'confirm' });
    ok(st.phase === 'combat' && cz.flight && cz.flight.mission.kind === 'explore', 'the bridge launches a free flight');
    ok(cz.flight.pois.some((x) => x.kind === 'station') && cz.flight.pois.some((x) => x.kind === 'planet'), 'free flight lists the station and the worlds as destinations');
    let F = cz.flight; step(F, 400); if (st.phase === 'combat' && cz.flight === F) st = g.tick(st, 50);
    ok(F.outcome === 3 && st.phase === 'hub' && st.screen === 'bridge', `docking ends the flight at the hub (outcome ${F.outcome}, ${F.t.toFixed(0)} s)`);
    // the jump drive in flight: the shared jump (fuel, epoch) and a new flight that arrives in the target system
    globalThis.window.__czAutoDock = false;
    st = { ...st, focus: { ...st.focus, bridge: 0 } }; st = g.reduce(st, { type: 'confirm' });
    const before = { sys: cz.run.sys, fuel: cz.run.fuel, epoch: cz.run.epoch };
    const S0 = cz.model.sector, to = S0.findIndex((x, i) => i !== before.sys && Math.hypot(x.x - S0[before.sys].x, x.y - S0[before.sys].y) <= cz.run.jump_range);
    cz.model.actions.plot(to);
    for (const s of cz.flight.ships) if (s.alive && s.team === S.TEAM_E) s.alive = false;
    const why = cz.model.actions.jump();
    ok(why === '', 'the jump drive spools for a plotted system in range (' + why + ')');
    F = cz.flight; step(F, 60); if (cz.flight === F) st = g.tick(st, 50);
    ok(F.outcome === 4 && cz.run.sys === to && cz.run.fuel < before.fuel && cz.run.epoch === before.epoch + 1, 'the jump moves the shared run (system, fuel, epoch)');
    ok(st.phase === 'combat' && cz.flight !== F && cz.flight.arriveT > 0, 'and drops out into a free flight in the new system');
    ok(Object.keys(cz.run).sort().join() === shape, 'the shared save keeps its contract after travel');
    // the relight, flown: relic + 300 cr are taken when the light bursts, the bit is set, the Keepers are pleased
    cz.flight.done = true; cz.flight.outcome = 3; st = g.tick(st, 50);
    const bsys = cz.model.sector.findIndex((s, i) => s.beacon && !((cz.run.beacon_lit >>> i) & 1));
    globalThis.window.__cz.dev.goto(bsys);
    cz.run.cargo[6] = 1; cz.run.credits = Math.max(cz.run.credits, 1000);
    const pre = { cr: cz.run.credits, kp: cz.run.rep[1], lit: cz.run.beacon_lit >>> 0, sector: cz.run.sector };
    st = g.reduce(st, { type: 'screen', to: 1 }); st = { ...st, focus: { ...st.focus, map: 1 } }; st = g.reduce(st, { type: 'confirm' });
    ok(st.phase === 'combat' && cz.flight.mission.relightWanted, 'the map sends you to relight the beacon');
    F = cz.flight; step(F, 500); if (st.phase === 'combat' && cz.flight === F) st = g.tick(st, 50);
    ok(F.relit && F.outcome === 1 && st.phase === 'debrief' && st.dbStats && st.dbStats.relit, `the relight is flown to the end (outcome ${F.outcome}, ${F.t.toFixed(0)} s)`);
    const litNow = ((cz.run.beacon_lit >>> 0) & (1 << bsys)) !== 0 || cz.run.sector > pre.sector;
    ok(litNow && cz.run.cargo[6] === 0 && cz.run.rep[1] === Math.min(100, pre.kp + 12), 'the shared numbers of a relight: beacon bit, relic spent, Keepers +12');
    ok(cz.run.credits === pre.cr - 300 + F.kills * st.cc.killCr, 'the relight costs 300 cr (salvage added)');
    ok(Object.keys(cz.run).sort().join() === shape && Object.keys(posts[posts.length - 1]).sort().join() === shape, 'the shared save keeps its contract after a relight');
    await new Promise((r) => setTimeout(r, 900));   // the web save is debounced
    const web = webDisk;
    ok(webPaths.has('/data/costellazioni/web.json') && web && web.seed === cz.run.seed >>> 0, 'the web save is written next to the shared one (/data/costellazioni/web.json)');
    ok(web && web.relights >= 1 && Object.values(web.relit).some((m) => m), 'it remembers the relit beacon');
    ok(web && ((web.visited[pre.sector] >>> 0) & (1 << bsys)), 'and the systems visited');
    ok(web && web.codex.relight != null && web.codex.costellatori != null, 'and the codex entries unlocked');
    ok(!posts.some((p) => 'codex' in p || 'visited' in p), 'nothing web-only ever reaches the shared save');
  }

  delete globalThis.window; Math.random = realRandom;
  try { rmSync(T, { recursive: true, force: true }); } catch {}
}

// ---- 6. M2 at the sim level ---------------------------------------------------------------------------------------------
{
  // the web-only world: reachable planets, moons on their own hash domain, an arrival point; new domains do not collide
  const vd = Object.values(W.VDOM); ok(new Set(vd).size === vd.length, 'web visual hash domains are distinct');
  const b = bp(1);
  ok(b.planets.every((p) => p.pos && p.radius > 500 && p.dist >= 40000 && Array.isArray(p.moons)), 'every world has a place, a size and its moons');
  ok(b.planets[0].dist === 40000 && b.planets.slice(1).every((p) => p.dist > 60000), 'the home world hangs 40 km out, the others further');
  let moons = 0; for (let i = 0; i < 10; i++) for (const p of bp(i).planets) for (const m of p.moons) { moons++; const mp = W.moonPos(p, m), d = Math.hypot(mp[0] - p.pos[0], mp[1] - p.pos[1], mp[2] - p.pos[2]); ok(Math.abs(d - p.radius * m.orbit) < 1, 'a moon orbits at its radius'); }
  ok(moons > 2, 'the sector has moons (' + moons + ')');
  ok(Array.isArray(b.edge) && Math.hypot(...b.edge) > 8000, 'a system has an arrival point at its edge');
}
const xcc = (o = {}) => ({ kind: 'explore', type: 0, foeFac: 2, waves: 2, perWave: 3, foeHp: 50, foeDmg: 10, foeSpeed: 85, ace: 0, rewardCr: 0, killCr: 20, repFac: -1, mission: false, slot: 13, ...o });
const SECn = (fn) => { for (let i = 0; i < SEC.length; i++) if (fn(SEC[i], i)) return i; return -1; };
const stSys = SECn((s) => s.faction !== 3), bSys = SECn((s) => s.beacon);
const evs = (F, n) => { let k = 0; for (const e of F.ev) if (e.n === n) k++; return k; };
{ // free flight: destinations, supercruise to a world, the survey
  const F = flight(xcc({ arrive: true }), { sys: stSys, autopilot: true, god: true });
  ok(F.mission.kind === 'explore' && F.pois.length >= 4 && F.arriveT > 0, 'an arrival starts a free flight with destinations');
  ok(F.player.spd > 2000, 'the arrival drops out of the jump at speed');
  for (let i = 0; i < 200; i++) S.stepFlight(F);
  ok(F.player.spd < 400 && F.arriveT <= 0, 'and settles into normal flight');
  const pi = F.pois.findIndex((p) => p.kind === 'planet'); S.cmd(F, 'nav', pi);
  ok(F.nav === pi && F.obj.key === 'cz_obj_explore' && F.obj.marker, 'the nav computer points at a world');
  let vmax = 0; while (F.t < 120 && !F.pois[pi].scanned) { S.stepFlight(F); vmax = Math.max(vmax, F.player.spd); }
  ok(F.pois[pi].scanned && evs(F, S.EV.SCAN) >= 1, `the world is reached and surveyed (${F.t.toFixed(0)} s)`);
  ok(vmax > 2600, `the cruise speeds up with the distance to go (${Math.round(vmax)} m/s)`);
  const P0 = F.pois[pi]; ok(Math.hypot(P0.pos[0] - F.player.pos.x, P0.pos[1] - F.player.pos.y, P0.pos[2] - F.player.pos.z) > P0.r, 'worlds are solid: you stop at the atmosphere');
  S.cmd(F, 'nav'); ok(F.nav === (pi + 1) % F.pois.length, 'N cycles the destinations');
}
{ // docking: clearance, lane, slide in, docked; denied with hostiles around
  const F = flight(xcc({}), { sys: stSys, god: true });
  F.mission.waves.length = 0;
  ok(S.cmd(F, 'dock') && F.dock && F.dock.ph === 0, 'docking clearance from within 9 km');
  while (!F.done && F.t < 200) S.stepFlight(F);
  ok(F.outcome === S.OUT.DOCKED && evs(F, S.EV.DOCK) >= 4, `the docking run ends in the bay (${F.t.toFixed(0)} s)`);
  const G2 = flight(xcc({}), { sys: stSys, god: true });
  const e = G2.ships.find((s) => !s.alive); Object.assign(e, { alive: true }); e.cls = S.CLS.scrapwing; e.ck = 'scrapwing'; e.team = S.TEAM_E; e.st = 13; e.hull = e.hullMax = 50; e.gen++; e.pos.x = G2.player.pos.x + 900; e.pos.y = G2.player.pos.y; e.pos.z = G2.player.pos.z;
  ok(!S.cmd(G2, 'dock') && !G2.dock, 'no clearance with hostiles close');
  ok(!S.cmd(G2, 'jump', 2) && !G2.jump, 'the jump drive is mass-locked too');
}
{ // the jump drive
  const F = flight(xcc({}), { sys: stSys, god: true });
  F.mission.waves.length = 0;
  ok(S.cmd(F, 'jump', 3) && F.jump, 'the drive spools');
  while (!F.done && F.t < 30) S.stepFlight(F);
  ok(F.outcome === S.OUT.JUMPED && F.jumpedTo === 3 && F.t > 4, `and jumps after the spool (${F.t.toFixed(1)} s)`);
}
{ // the relight: calm (the Voice measures you) and hostile (the lattice ships come)
  for (const calm of [true, false]) {
    const F = flight(xcc({ relight: true, echoCalm: calm, litCount: calm ? 0 : 2 }), { sys: bSys, autopilot: true, god: true });
    const seen = new Set(); let neutral = 0, linked = 0, bursts = 0, cur = F.evHead;
    while (!F.done && F.t < 500) {
      S.stepFlight(F);
      for (; cur < F.evHead; cur++) if (F.ev[cur % F.ev.length].n === S.EV.RELIGHT) bursts++;
      for (const s of F.ships) if (s.alive && !s.isPlayer) { seen.add(s.ck + ':' + s.team); if (s.team === S.TEAM_N) neutral++; if (s.ck === 'choir' && s.links && s.links.length) linked++; }
    }
    const tag = calm ? 'calm' : 'hostile';
    ok(F.relit && F.outcome === 1, `relight (${tag}): the beacon is lit and the event won (outcome ${F.outcome}, ${F.t.toFixed(0)} s)`);
    ok(bursts === 1 && F.mission.relight.k >= 1, `relight (${tag}): one burst, full charge`);
    ok(seen.has('censer:0') && seen.has('votive:0'), `relight (${tag}): the Keepers' watch flies (Censer + Votives)`);
    ok(seen.has('reliquary:0'), `relight (${tag}): deep enough, the Keepers' Reliquary barge comes to witness`);
    if (calm) ok(neutral > 0 && !seen.has('lattice:1'), 'calm: the lattice ships come, measure, and leave without firing');
    else ok(seen.has('lattice:1') && seen.has('choir:1') && evs(F, S.EV.PHASE) > 0, 'hostile: Lattice interceptors (phasing) and a Choir attack');
    if (!calm) ok(linked > 0, 'hostile: the Choir links its kin');
  }
}
{ // rosters up close: torpedoes are slow and can be shot down; a phased Lattice is not there; links restore shields
  const F = flight(xcc({}), { sys: stSys, god: true }); quiet(F);
  const p = F.player, f = { x: 0, y: 0, z: 0 }; S.shipFwd(p, f);
  const mk = (ck, team, fac, d) => { const s = F.ships.find((x) => !x.alive); Object.assign(s, { alive: true }); s.cls = S.CLS[ck]; s.ck = ck; s.team = team; s.fac = fac; s.hull = s.hullMax = 400; s.shMax = 100; s.shF = s.shB = 10; s.st = 13; s.gen++; s.pos.x = p.pos.x + f.x * d; s.pos.y = p.pos.y + f.y * d; s.pos.z = p.pos.z + f.z * d; s.vel.x = s.vel.y = s.vel.z = 0; s.q = { ...p.q }; return s; };
  const c = mk('censer', S.TEAM_E, 1, 1500); c.target = p; c.st = 1;
  let torp = null; for (let i = 0; i < 1200 && !torp; i++) { S.stepFlight(F); torp = F.msls.find((m) => m.alive && m.torp); }
  ok(!!torp && torp.spd < 260, 'the Censer launches a slow torpedo');
  if (torp) {
    const bl = F.bolts.find((x) => !x.alive); Object.assign(bl, { alive: true, x: torp.pos.x + 30, y: torp.pos.y, z: torp.pos.z, px: torp.pos.x + 30, py: torp.pos.y, pz: torp.pos.z, vx: -1800, vy: 0, vz: 0, life: 1, team: S.TEAM_P, own: p, fac: 4, dmg: 10, kind: 0 });
    torp.vel.x = torp.vel.y = torp.vel.z = 0; torp.spd = 0;
    for (let i = 0; i < 4; i++) S.stepFlight(F);
    ok(torp.hp < 2 || !torp.alive, 'a bolt hits a torpedo (two hits bring it down)');
  }
  const L = mk('lattice', S.TEAM_E, 3, 700); L.st = 1; L.target = p; L.lastHitT = F.t; L.phaseCd = 0;
  for (let i = 0; i < 3; i++) S.stepFlight(F);
  ok(L.phaseT > 0, 'a hit Lattice phases out');
  const bl2 = F.bolts.find((x) => !x.alive); Object.assign(bl2, { alive: true, x: L.pos.x + 10, y: L.pos.y, z: L.pos.z, px: L.pos.x + 10, py: L.pos.y, pz: L.pos.z, vx: -1500, vy: 0, vz: 0, life: 1, team: S.TEAM_P, own: p, fac: 4, dmg: 50, kind: 0 });
  const h0 = L.hull; S.stepFlight(F); ok(L.hull === h0, 'and bolts pass through it while it is out of phase');
  const ch = mk('choir', S.TEAM_E, 3, 2500); ch.st = 1; ch.target = p; L.shF = L.shB = 0; L.phaseT = 0; L.pos.x = ch.pos.x + 200; L.pos.y = ch.pos.y; L.pos.z = ch.pos.z;
  for (let i = 0; i < 120; i++) S.stepFlight(F);
  ok(L.shF > 0 && ch.links.includes(L), 'the Choir re-shields a Lattice through its link');
}
{ // the codex: every entry has its title, text and hint in five languages; its art exists; its model is a ship class
  const CX = await import(url('constellations-codex.js'));
  const cats = Object.fromEntries(['it', 'en', 'es', 'fr', 'de'].map((l) => [l, JSON.parse(readFileSync(join(root, 'apps/games/www/i18n.' + l + '.json'), 'utf8'))]));
  const man = JSON.parse(readFileSync(join(GW, 'stelle/assets/manifest.json'), 'utf8'));
  let missing = [];
  for (const e of CX.ENTRIES) {
    for (const l of Object.keys(cats)) for (const k of ['_t', '_b', '_h']) if (!cats[l]['cz_cx_' + e.id + k]) missing.push(l + ':' + e.id + k);
    if (e.img && !man.images[e.img]) missing.push('img:' + e.img);
    if (e.emblem && !man.images[e.emblem]) missing.push('img:' + e.emblem);
    if (e.model && !S.CLS[e.model[0]]) missing.push('model:' + e.model[0]);
  }
  ok(missing.length === 0, 'codex entries complete in five languages with art / models' + (missing.length ? ': ' + missing.slice(0, 6).join(', ') : ''));
  ok(CX.ENTRIES.length >= 50 && CX.CATS.every((c, i) => CX.entriesOf(i).length > 0), 'the codex has ' + CX.ENTRIES.length + ' entries over ' + CX.CATS.length + ' categories');
  for (const ck of ['censer', 'lattice', 'choir', 'reliquary']) ok(CX.ENTRIES.some((e) => e.model && e.model[0] === ck), 'codex dossier for ' + ck);
}
{ // the web save module: per run, merge, helpers
  const WS = await import(url('constellations-web.js'));
  const w = WS.freshWeb(42);
  ok(WS.markVisited(w, 3, 5) && WS.visited(w, 3, 5) && !WS.markVisited(w, 3, 5), 'visited systems are a per-sector bitmask');
  ok(WS.unlock(w, 'relight') && !WS.unlock(w, 'relight') && WS.hasCodex(w, 'relight'), 'codex unlocks once');
  WS.markRelit(w, 3, 7); ok(WS.wasRelit(w, 3, 7) && w.relights === 1, 'relit beacons outlive the sector');
  ok(WS.markScanned(w, 3, 5, 2) && WS.scanned(w, 3, 5, 2), 'surveys are kept per system');
}

console.log(`\ncostellazioni: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
