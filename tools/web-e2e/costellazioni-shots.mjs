// Visual review for Costellazioni (web): boots the game in headless Chrome against the simulator and saves
// named scenes as PNGs, so a change to the 3D game can be LOOKED AT without a human driving it.
//   node tools/web-e2e/costellazioni-shots.mjs                       → every scene, build/web-e2e/costellazioni/review/
//   node tools/web-e2e/costellazioni-shots.mjs --scenes kit,stations --gpu --size 1440x900
//   --lang it   --out <dir>   --gpu (real GPU, like E2E_GPU=1)
//   node tools/web-e2e/costellazioni-shots.mjs --scenes worlds --gpu --biomes ocean,ice   → M3 worlds, some biomes
//   --suffix -v2   appends to every file name (keep a before / after pair side by side)
//   node tools/web-e2e/costellazioni-shots.mjs --scenes worlds,underwater,clouds,shadows,dunes --gpu --suffix -v3
// Scenes are plain async functions below; each gets a fresh page with a seeded run (sector 1, credits, a relic).
// M3 scenes (worlds, entry, ruin, groundfight, worldhud) place the ship with the __cz.dev hooks (overWorld,
// nearSite) and pin the universe clock (__czClock) so the same light comes back on every run.
import { join } from 'node:path';
import { mkdirSync, existsSync, rmSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim, REPO } from './sim.mjs';
import { bootShell } from './shell.mjs';

const arg = (k, d) => { const i = process.argv.indexOf('--' + k); return i > 0 ? process.argv[i + 1] : d; };
const flag = (k) => process.argv.includes('--' + k);
const [W, H] = arg('size', '1440x900').split('x').map(Number);
const OUT = arg('out', join(REPO, 'build', 'web-e2e', 'costellazioni', 'review'));
const LANG = arg('lang', 'en');
const SUFFIX = arg('suffix', '');
const GPU = flag('gpu') || process.env.E2E_GPU === '1';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
export const SAVE_PATH = '/data/games/costellazioni.json';
export const SAVE_SEED = { ver: 3, credits: 4200, fuel: 8, fuel_max: 8, hull: 100, hull_max: 100, cargo_max: 20, jump_range: 64, sensors: 1, weapon: 1, shield_max: 40, sys: 0,
  cargo: [0, 0, 0, 0, 0, 0, 2, 0], rep: [10, 5, 0, 0], flags: 0, beacon_lit: 0, epoch: 3, missions_done: 0, kills: 12, seed: 987654, sector: 1 };

const key = (page, k) => page.eval(`window.dispatchEvent(new KeyboardEvent('keydown', { key: ${JSON.stringify(k)}, bubbles: true })), true`);
const frames = (page, n = 2) => page.eval(`new Promise(r => { let k = ${n}; const f = () => (--k > 0 ? requestAnimationFrame(f) : setTimeout(r, 30)); requestAnimationFrame(f); })`);
async function shot(page, name) { await page.eval(`(() => { const h = document.getElementById('lockHint'); if (h) h.classList.remove('on'); return 1; })()`).catch(() => {}); await frames(page, 3); const f = join(OUT, name + SUFFIX + '.png'); const ok = await page.screenshot(f); console.log((ok ? '  ✓ ' : '  ✗ ') + f); return f; }

export async function openGame(browser, sim, { lang = LANG, flags = {}, viewport = [W, H], mobile = false } = {}) {
  const page = await browser.newPage();
  await page.setViewport(viewport[0], viewport[1], mobile);
  await page.initScript(`Object.assign(window, ${JSON.stringify(flags)});`);
  await bootShell(page, sim, { lang, wait: false });
  await page.goto(`${sim.origin}/apps/games/?host=constellations`);
  if (!await page.waitFor(`document.getElementById('startBtn') && !document.getElementById('startBtn').disabled && document.getElementById('startBtn').offsetWidth > 0`, { timeout: 45000 })) throw new Error('waiting room');
  await page.eval(`document.getElementById('startBtn').click(), true`);
  if (!await page.waitFor(`(() => { const m = document.querySelector('[data-modal]'), h = document.querySelector('[data-hub]'); return (m && m.style.display === 'flex' && m.querySelector('.cz-btn')) || (h && h.style.display === 'block'); })()`, { timeout: 45000 })) throw new Error('game did not open');
  return page;
}
export async function toHub(page) {
  // (only a modal that offers a choice takes the Enter: one pressed on the "syncing your run" card launches a flight)
  await page.waitFor(`(() => { const m = document.querySelector('[data-modal]'), h = document.querySelector('[data-hub]'); return (m && m.style.display === 'flex' && m.querySelector('.cz-btn')) || (h && h.style.display === 'block'); })()`, { timeout: 30000 });
  if (await page.eval(`(() => { const m = document.querySelector('[data-modal]'); return m.style.display === 'flex' && !!m.querySelector('.cz-btn'); })()`)) await key(page, 'Enter');
  if (!await page.waitFor(`document.querySelector('[data-hub]').style.display === 'block' && document.querySelectorAll('.cz-tab').length >= 5`, { timeout: 30000 })) throw new Error('hub');
  await page.waitFor(`!!(window.__cz && window.__cz.perf && window.__cz.perf.frames > 20)`, { timeout: 30000 });
}
// wait for the illustrations the hub asked for (IndexedDB / network) to land
const artSettled = (page) => page.waitFor(`![...document.querySelectorAll('[data-cz] *')].some((n) => /linear-gradient\\(#/.test(n.style.backgroundImage || ''))`, { timeout: 15000 });

const KIT_ROWS = {
  'kit-courier': { list: [['lucciola', 4, false]], o: { yaw: 2.45, elev: 0.22 } },
  'kit-courier-rear': { list: [['lucciola', 4, false]], o: { yaw: -0.5, elev: 0.3 } },
  'kit-courier-side': { list: [['lucciola', 4, false]], o: { yaw: 1.57, elev: 0.12 } },
  'kit-gilda': { list: [['lancer', 0, false], ['bastion', 0, false], ['lancer', 0, 0]], o: {} },
  'kit-relitti': { list: [['scrapwing', 2, false], ['harpoon', 2, false], ['gutter', 2, false], ['scrapwing', 2, 2]], o: {} },
  'kit-custodi': { list: [['votive', 1, false], ['censer', 1, false], ['votive', 1, 1]], o: {} },
  'kit-eco': { list: [['shard', 3, false], ['lattice', 3, false], ['choir', 3, false]], o: {} },
  'kit-capitals-a': { list: [['warden', 0, false], ['hauler', 0, false]], o: {} },
  'kit-capitals-b': { list: [['hulk', 2, false], ['reliquary', 1, false]], o: {} },
};

// M3: a world of each type in sector 1 of the seed — [system, planet, azimuth round the sub-solar point where the
// review flies: picked (at the pinned clock) so its signature flora stands ahead]
export const BIOMES = { rocky: [9, 0, 30], ocean: [2, 0, 40], jungle: [5, 0, 300], ice: [6, 0, 240], desert: [6, 3, 20], volcanic: [2, 1, 100], crystal: [1, 0, 160], gas: [3, 1, 20] };
// the light the low shots are taken in: [sun elevation (deg), heading from straight down-sun (deg): ~100 rakes the light
// across the view so every crest and ridge shows a lit side and a shaded one (the sun behind the camera flattens them)]
export const LIGHT = { rocky: [26, 100], ocean: [38, 75], jungle: [34, 95], ice: [24, 100], desert: [27, 100], volcanic: [30, 95], crystal: [30, 100], gas: [35, 0] };
const CLOCK = 1000;
async function launch(page) {
  await toHub(page); await key(page, '1'); await sleep(200); await key(page, 'Enter');
  if (!await page.waitFor(`!!(window.__cz.game.flight && window.__cz.game.flight.t > 1)`, { timeout: 30000 })) throw new Error('launch');
}
const surf = (page) => page.eval(`(() => { const F = window.__cz.game.flight; if (!F) return null; const U = F.surf; return { st: U.st, alt: U.alt | 0, agl: U.agl | 0, found: U.found, heat: U.heat, d: U.nav >= 0 && U.sites[U.nav] ? U.sites[U.nav].d | 0 : -1 }; })()`);
async function until(page, fn, timeout = 120000) { const end = Date.now() + timeout; for (;;) { const v = await surf(page); if (v && fn(v)) return v; if (Date.now() > end) throw new Error('timed out: ' + JSON.stringify(v)); await sleep(120); } }
// nose down by `k` (0..1 of a right angle) along the current heading, or at the world's limb from orbit
const lookDown = (page, k) => page.eval(`import('/apps/games/games/stelle/sim.js').then((S) => { const F = window.__cz.game.flight, U = F.surf, p = F.player, f = { x: 0, y: 0, z: 0 }; S.shipFwd(p, f); const c = Math.cos(${k} * Math.PI / 2), s = Math.sin(${k} * Math.PI / 2); S.qlook(p.q, f.x * c - U.up.x * s, f.y * c - U.up.y * s, f.z * c - U.up.z * s, U.up.x, U.up.y, U.up.z); F.input.aimReset = true; U.assist = false; return 1; })`);
const lookAtWorld = (page, pi) => page.eval(`import('/apps/games/games/stelle/sim.js').then((S) => { const F = window.__cz.game.flight, W = F.worlds[${pi}], p = F.player; S.qlook(p.q, W.c.x - p.pos.x + W.R * 0.4, W.c.y - p.pos.y + W.R * 0.2, W.c.z - p.pos.z); F.input.aimReset = true; return 1; })`);
const blank = (page) => page.goto('about:blank').catch(() => {});

const SCENES = {
  async title(browser) {
    const sim = await startSim();
    try { const page = await openGame(browser, sim); await sleep(5200); await artSettled(page); await shot(page, 'title'); }
    finally { await sim.stop(); }
  },
  async hub(browser) {
    const sim = await startSim({ seed: { [SAVE_PATH]: SAVE_SEED } });
    try {
      const page = await openGame(browser, sim); await toHub(page); await sleep(1500); await artSettled(page); await shot(page, 'hub-bridge');
      for (const [sys, name] of [[2, 'hub-bridge-b'], [5, 'hub-bridge-c']]) { await page.eval(`window.__cz.dev && window.__cz.dev.goto(${sys}), true`); await sleep(2500); await artSettled(page); await shot(page, name); }
    } finally { await sim.stop(); }
  },
  async hubsizes(browser) {
    const sim = await startSim({ seed: { [SAVE_PATH]: SAVE_SEED } });
    try {
      for (const [w, h, mob] of [[1280, 720], [1920, 1080], [390, 844, true]]) {
        const page = await openGame(browser, sim, { viewport: [w, h], mobile: !!mob }); await toHub(page); await sleep(1500); await artSettled(page); await shot(page, `hub-${w}x${h}`);
      }
    } finally { await sim.stop(); }
  },
  async map(browser) {
    const sim = await startSim({ seed: { [SAVE_PATH]: SAVE_SEED } });
    try { const page = await openGame(browser, sim); await toHub(page); await key(page, '2'); await sleep(2500); await shot(page, 'galaxy-map'); await key(page, 'ArrowRight'); await sleep(1500); await shot(page, 'galaxy-map-target'); }
    finally { await sim.stop(); }
  },
  async flight(browser) {
    const sim = await startSim({ seed: { [SAVE_PATH]: SAVE_SEED } });
    try {
      for (const [w, h, mob, tag] of [[W, H, false, ''], [1280, 720, false, '-1280'], [1920, 1080, false, '-1920'], [390, 844, true, '-phone']]) {
        const page = await openGame(browser, sim, { flags: { __czAutopilot: true, __czGod: true }, viewport: [w, h], mobile: mob });
        await toHub(page); await key(page, '5'); await sleep(300); await key(page, 'Enter');
        await page.waitFor(`!!(window.__cz.game.flight && window.__cz.game.flight.wave >= 1 && window.__cz.game.flight.ships.filter((s) => s.alive && s.team === 1).length >= 2)`, { timeout: 90000 });
        await sleep(2500); await shot(page, 'combat' + tag);
        if (!tag) { await sleep(4000); await shot(page, 'combat-b'); await page.eval(`window.__cz.r3d && window.__cz.r3d.setCamera && window.__cz.r3d.setCamera('cockpit'), true`); await sleep(1500); await shot(page, 'combat-cockpit'); }
        await page.send('Target.closeTarget', {}).catch(() => {});
      }
    } finally { await sim.stop(); }
  },
  // the jump from the hub map: drive, tunnel, arrival at the new station
  async jump(browser) {
    const sim = await startSim({ seed: { [SAVE_PATH]: { ...SAVE_SEED, sys: 6 } } });
    try {
      const page = await openGame(browser, sim); await toHub(page); await key(page, '2'); await sleep(1500);
      await page.eval(`window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Tgt:5' })), true`); await sleep(600);
      await key(page, 'Enter'); await sleep(700); await shot(page, 'jump-depart');
      await sleep(900); await shot(page, 'jump-tunnel');
      await sleep(1700); await shot(page, 'jump-arrival');
      await sleep(3000); await shot(page, 'jump-hub');
    } finally { await sim.stop(); }
  },
  // free flight: undock, cruise, request docking, the docking run
  async dock(browser) {
    const sim = await startSim({ seed: { [SAVE_PATH]: { ...SAVE_SEED, sys: 6 } } });
    try {
      const page = await openGame(browser, sim, { flags: { __czAutopilot: true, __czAutoDock: true, __czEncounter: false, __czGod: true } });
      await toHub(page); await key(page, '1'); await sleep(200); await key(page, 'Enter');
      await page.waitFor(`!!(window.__cz.game.flight && window.__cz.game.flight.t > 1.2)`, { timeout: 30000 }); await shot(page, 'undock');
      await page.waitFor(`!!(window.__cz.game.flight && window.__cz.game.flight.t > 6)`, { timeout: 30000 }); await shot(page, 'free-flight');
      await page.waitFor(`!!(window.__cz.game.flight && window.__cz.game.flight.dock && window.__cz.game.flight.dock.ph >= 1)`, { timeout: 120000 }); await sleep(1500); await shot(page, 'docking');
      await page.waitFor(`!!(window.__cz.game.flight && window.__cz.game.flight.dock && window.__cz.game.flight.dock.ph >= 2)`, { timeout: 60000 }); await sleep(2500); await shot(page, 'docking-b');
    } finally { await sim.stop(); }
  },
  // in flight: plot on the galaxy map, spool the drive, the tunnel, the arrival fly-in
  async arrival(browser) {
    const sim = await startSim({ seed: { [SAVE_PATH]: { ...SAVE_SEED, sys: 6 } } });
    try {
      const page = await openGame(browser, sim, { flags: { __czEncounter: false, __czGod: true } });
      await toHub(page); await key(page, '1'); await sleep(200); await key(page, 'Enter');
      await page.waitFor(`!!(window.__cz.game.flight && window.__cz.game.flight.t > 5)`, { timeout: 30000 });
      await key(page, 'p'); await sleep(1800); await shot(page, 'flight-map');
      await page.eval(`window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Tgt:9' })), true`); await sleep(300);
      await key(page, 'k'); await sleep(2600); await shot(page, 'jump-spool');
      await sleep(2300); await shot(page, 'jump-tunnel-flight');
      await page.waitFor(`!!(window.__cz.game.flight && window.__cz.game.flight.arriveT > 0)`, { timeout: 30000 }); await sleep(900); await shot(page, 'arrival');
      await sleep(2600); await shot(page, 'arrival-b');
    } finally { await sim.stop(); }
  },
  // cruise to a world and look at it from orbit (atmosphere, clouds, rings, moons)
  async planet(browser) {
    const sim = await startSim({ seed: { [SAVE_PATH]: { ...SAVE_SEED, sys: 6 } } });
    try {
      const page = await openGame(browser, sim, { flags: { __czAutopilot: true, __czEncounter: false, __czGod: true } });
      await toHub(page); await key(page, '1'); await sleep(200); await key(page, 'Enter');
      await page.waitFor(`!!(window.__cz.game.flight && window.__cz.game.flight.t > 2)`, { timeout: 30000 });
      await page.eval(`(() => { const F = window.__cz.game.flight; const i = F.pois.findIndex((p) => p.kind === 'planet' && p.i > 0) >= 0 ? F.pois.findIndex((p) => p.kind === 'planet' && p.i > 0) : F.pois.findIndex((p) => p.kind === 'planet'); F.undock = null; F.nav = i; return i; })()`);
      await page.waitFor(`!!(window.__cz.game.flight && window.__cz.game.flight.cruise === 2)`, { timeout: 60000 }); await sleep(1500); await shot(page, 'cruise');
      await page.waitFor(`(() => { const F = window.__cz.game.flight; return F && F.pois[F.nav].scanned; })()`, { timeout: 120000, interval: 300 });
      // hands off, nose on the world, a little off-centre so the limb and the moons show
      await page.eval(`import('/apps/games/games/stelle/sim.js').then((S) => { const F = window.__cz.game.flight, P = F.pois[F.nav], p = F.player; F.autopilot = false; F.input.thr = 0.15; F.input.aimMode = 0; F.input.pitch = F.input.yaw = F.input.roll = 0; S.qlook(p.q, P.pos[0] - p.pos.x + P.r * 0.55, P.pos[1] - p.pos.y + P.r * 0.25, P.pos[2] - p.pos.z); p.w.x = p.w.y = p.w.z = 0; return 1; })`);
      await sleep(2500); await shot(page, 'planet-orbit');
    } finally { await sim.stop(); }
  },
  // the relight: charge with the Keepers' watch, the burst, the Echo answers (calm here; hostile in 'echo')
  async relight(browser) {
    const sim = await startSim({ seed: { [SAVE_PATH]: { ...SAVE_SEED, sys: 7 } } });
    try {
      const page = await openGame(browser, sim, { flags: { __czAutopilot: true, __czGod: true, __czTimeScale: 3 } });
      await toHub(page); await key(page, '2'); await sleep(800); await key(page, 'ArrowDown'); await sleep(200); await key(page, 'Enter');
      await page.waitFor(`!!(window.__cz.game.flight && window.__cz.game.flight.mission.relight && window.__cz.game.flight.mission.relight.k > 0.35)`, { timeout: 180000 });
      await page.eval(`window.__cz.game.flight.timeScale = 1, true`); await sleep(1500); await shot(page, 'relight-charge');
      await page.eval(`window.__cz.game.flight.timeScale = 3, true`);
      await page.waitFor(`!!(window.__cz.game.flight && window.__cz.game.flight.relit)`, { timeout: 180000 });
      await page.eval(`window.__cz.game.flight.timeScale = 1, true`); await sleep(700); await shot(page, 'relight-burst');
      await page.waitFor(`!!(window.__cz.game.flight && window.__cz.game.flight.mission.relight.ph === 'answer')`, { timeout: 60000 }); await sleep(3500); await shot(page, 'relight-answer');
    } finally { await sim.stop(); }
  },
  async echo(browser) {
    const sim = await startSim({ seed: { [SAVE_PATH]: { ...SAVE_SEED, sys: 7, beacon_lit: 1 } } });
    try {
      const page = await openGame(browser, sim, { flags: { __czAutopilot: true, __czGod: true, __czTimeScale: 3 } });
      await toHub(page); await key(page, '2'); await sleep(800); await key(page, 'ArrowDown'); await sleep(200); await key(page, 'Enter');
      await page.waitFor(`!!(window.__cz.game.flight && window.__cz.game.flight.mission.relight && window.__cz.game.flight.mission.relight.ph === 'answer')`, { timeout: 300000 });
      await page.eval(`window.__cz.game.flight.timeScale = 1, true`);
      await page.waitFor(`window.__cz.game.flight.ships.filter((s) => s.alive && s.team === 1 && s.fac === 3).length >= 3`, { timeout: 30000 });
      await sleep(3500); await shot(page, 'echo-fight'); await sleep(3000); await shot(page, 'echo-fight-b');
    } finally { await sim.stop(); }
  },
  async codex(browser) {
    const sim = await startSim({ seed: { [SAVE_PATH]: SAVE_SEED } });
    try {
      const page = await openGame(browser, sim); await toHub(page); await key(page, '6'); await sleep(1500); await artSettled(page); await shot(page, 'codex');
      for (let i = 0; i < 5; i++) await key(page, 'ArrowRight');
      await sleep(1200); await artSettled(page); await shot(page, 'codex-contacts');
      await key(page, 'ArrowLeft'); await sleep(2200); await shot(page, 'codex-hangar');
    } finally { await sim.stop(); }
  },
  // M3 — every biome from orbit, mid-descent (2.6 km, nose down), at 300 m and low over the ground (70 m), in daylight;
  // the low shots in a raking light (LIGHT) with the nose a little down, so the ground — not the sky — fills the frame
  async worlds(browser) {
    for (const b of arg('biomes', Object.keys(BIOMES).join(',')).split(',')) {
      const [sys, pi, az] = BIOMES[b], [el, hdg] = LIGHT[b];
      const sim = await startSim({ seed: { [SAVE_PATH]: { ...SAVE_SEED, sys } } });
      try {
        const page = await openGame(browser, sim, { flags: { __czEncounter: false, __czGod: true, __czClock: CLOCK } });
        await launch(page);
        await page.eval(`window.__cz.dev.overWorld(${pi}, 16000, 35, ${az}, 0), true`); await lookAtWorld(page, pi); await sleep(3500); await shot(page, `world-${b}-orbit`);
        await page.eval(`window.__cz.dev.overWorld(${pi}, 2600, 35, ${az}, 140), true`); await sleep(300); await lookDown(page, 0.38); await sleep(4500); await shot(page, `world-${b}-descent`);
        if (b !== 'gas') { await page.eval(`window.__cz.dev.overWorld(${pi}, 300, ${el}, ${az}, 110, ${hdg}), window.__cz.game.flight.surf.assist = true, true`); await sleep(6000); await lookDown(page, 0.2); await sleep(1100); await shot(page, `world-${b}-300m`); }
        await page.eval(`window.__cz.dev.overWorld(${pi}, ${b === 'gas' ? 900 : 70}, ${el}, ${az}, 110, ${hdg}), window.__cz.game.flight.surf.assist = true, true`); await sleep(6000);
        if (b !== 'gas') { await lookDown(page, 0.08); await sleep(900); }
        await shot(page, `world-${b}-surface`);
        await blank(page);
      } finally { await sim.stop(); }
    }
  },
  // M3 — under the sea of an ocean world: over a shallow reef from above, then down among the coral and kelp (the
  // seabed, caustics, the water's own blue), and looking up at the surface (Snell's window)
  async underwater(browser) {
    const [sys, pi] = BIOMES.ocean;
    const sim = await startSim({ seed: { [SAVE_PATH]: { ...SAVE_SEED, sys } } });
    try {
      const page = await openGame(browser, sim, { flags: { __czEncounter: false, __czGod: true, __czClock: CLOCK } });
      await launch(page);
      // a reef 7 .. 16 m down, found by walking round the sub-solar point
      const az = await page.eval(`(() => { const F = window.__cz.game.flight; for (let a = 0; a < 360; a += 3) { window.__cz.dev.overWorld(${pi}, 30, 55, a, 30, 40); if (F.surf.water && F.surf.bed < -7 && F.surf.bed > -16) return a; } return 40; })()`);
      await page.eval(`window.__cz.dev.overWorld(${pi}, 26, 55, ${az}, 30, 40), true`); await sleep(5500); await lookDown(page, 0.32); await sleep(1100); await shot(page, 'world-ocean-shallows');
      // under: 4 m below the surface, slow, nose a little down at the reef
      const dive = (k) => page.eval(`import('/apps/games/games/stelle/sim.js').then((S) => { const F = window.__cz.game.flight, U = F.surf, p = F.player; window.__cz.dev.overWorld(${pi}, 1, 55, ${az}, 8, 40);
        const d = U.alt + ${k < 0 ? 7 : 4.5}; p.pos.x -= U.up.x * d; p.pos.y -= U.up.y * d; p.pos.z -= U.up.z * d; p.ppos.x = p.pos.x; p.ppos.y = p.pos.y; p.ppos.z = p.pos.z;
        const f = { x: 0, y: 0, z: 0 }; S.shipFwd(p, f); const c = Math.cos(${k} * Math.PI / 2), s = Math.sin(${k} * Math.PI / 2);
        S.qlook(p.q, f.x * c - U.up.x * s, f.y * c - U.up.y * s, f.z * c - U.up.z * s, U.up.x, U.up.y, U.up.z); p.pq.x = p.q.x; p.pq.y = p.q.y; p.pq.z = p.q.z; p.pq.w = p.q.w;
        p.vel.x = p.vel.y = p.vel.z = 0; p.spd = 0; F.input.aimReset = true; U.assist = false; F.input.thr = 0.02; return 1; })`);
      await dive(0.12); await sleep(3500); await shot(page, 'world-ocean-underwater');
      await dive(-0.42); await sleep(2000); await shot(page, 'world-ocean-surface-below');
      await blank(page);
    } finally { await sim.stop(); }
  },
  // M3 — the volumetric clouds: over their tops, then inside the layer on the way down
  async clouds(browser) {
    for (const b of arg('biomes', 'jungle,ice').split(',')) {
      const [sys, pi, az] = BIOMES[b];
      const sim = await startSim({ seed: { [SAVE_PATH]: { ...SAVE_SEED, sys } } });
      try {
        const page = await openGame(browser, sim, { flags: { __czEncounter: false, __czGod: true, __czClock: CLOCK } });
        await launch(page);
        await page.eval(`window.__cz.dev.overWorld(${pi}, 3000, 30, ${az}, 140, 95), true`); await sleep(400);
        const top = await page.eval(`(() => { const W = window.__cz.r3d.world, S = W.S; return S ? S.cloud.alt + Math.min(1500, Math.max(650, S.cloud.thick * 5.5)) * 0.7 : 2000; })()`);
        await page.eval(`window.__cz.dev.overWorld(${pi}, ${Math.round(top + 250)}, 30, ${az}, 140, 95), true`); await sleep(5500); await lookDown(page, 0.14); await sleep(1000); await shot(page, `world-${b}-cloudtops`);
        const mid = await page.eval(`(() => { const S = window.__cz.r3d.world.S; return S ? S.cloud.alt : 1500; })()`);
        await page.eval(`window.__cz.dev.overWorld(${pi}, ${Math.round(mid)}, 30, ${az}, 120, 95), true`); await sleep(5000); await shot(page, `world-${b}-incloud`);
        await blank(page);
      } finally { await sim.stop(); }
    }
  },
  // M3 — long shadows: a low sun behind the shoulder over the rock world's mesas (the wide cascade: buttes and ridges
  // shade the basins out to a kilometre and more), and the same view without the wide cascade for comparison
  async shadows(browser) {
    const [sys, pi, az] = BIOMES.rocky;
    const sim = await startSim({ seed: { [SAVE_PATH]: { ...SAVE_SEED, sys } } });
    try {
      const page = await openGame(browser, sim, { flags: { __czEncounter: false, __czGod: true, __czClock: CLOCK } });
      await launch(page);
      // one placement, two frames ~150 ms apart (the toggle lands on the next frame), so the pair differs only in the cascade
      await page.eval(`window.__cz.r3d.world.shadow2Off = false, window.__cz.dev.overWorld(${pi}, 600, 17, ${az}, 60, 150), true`); await sleep(6500); await lookDown(page, 0.3); await sleep(1100);
      await shot(page, 'world-rocky-longshadows');
      await page.eval(`window.__cz.r3d.world.shadow2Off = true, true`); await sleep(150); await shot(page, 'world-rocky-longshadows-off');
      await page.eval(`window.__cz.r3d.world.shadow2Off = false, true`);
      await blank(page);
    } finally { await sim.stop(); }
  },
  // M3 — the dune sea low and slow in a raking light; then a dust devil, the nose turned at it
  async dunes(browser) {
    const [sys, pi, az] = BIOMES.desert;
    const sim = await startSim({ seed: { [SAVE_PATH]: { ...SAVE_SEED, sys } } });
    try {
      const page = await openGame(browser, sim, { flags: { __czEncounter: false, __czGod: true, __czClock: CLOCK } });
      await launch(page);
      await page.eval(`window.__cz.dev.overWorld(${pi}, 110, 25, ${az}, 50, 100), window.__cz.game.flight.surf.assist = true, true`); await sleep(6000); await lookDown(page, 0.16); await sleep(1100); await shot(page, 'world-desert-dunes');
      // a dust devil raised 700 m ahead (the world's review hook), seen side-lit from 90 m
      await page.eval(`window.__cz.dev.overWorld(${pi}, 90, 22, ${az}, 15, 100), window.__cz.game.flight.surf.assist = true, true`); await sleep(5000);
      await page.eval(`window.__cz.r3d.world.devilAhead(600, 24), true`); await sleep(3500); await shot(page, 'world-desert-devil');
      await blank(page);
    } finally { await sim.stop(); }
  },
  // M3 — the dive from orbit: the autopilot descends, the hull heats, plasma, the air takes the speed
  async entry(browser) {
    const sim = await startSim({ seed: { [SAVE_PATH]: { ...SAVE_SEED, sys: 2 } } });
    try {
      const page = await openGame(browser, sim, { flags: { __czEncounter: false, __czGod: true, __czClock: CLOCK, __czAutopilot: true, __czAutoLand: true } });
      await launch(page);
      await page.eval(`window.__cz.dev.overWorld(0, 30000, 80, 20, 0), true`); await sleep(500);
      await page.eval(`import('/apps/games/games/stelle/sim.js').then((S) => S.cmd(window.__cz.game.flight, 'descend'))`);
      await until(page, (v) => v.st === 1 && v.heat > 0.45, 180000); await shot(page, 'world-entry');
      await sleep(2500); await shot(page, 'world-entry-b');
      await until(page, (v) => v.st === 2, 180000); await sleep(1500); await shot(page, 'world-helm');
      await blank(page);
    } finally { await sim.stop(); }
  },
  // M3 — a Costellatori ruin: the approach on its signal, landing beside it (the discovery), the take-off
  async ruin(browser) {
    const sim = await startSim({ seed: { [SAVE_PATH]: { ...SAVE_SEED, sys: 9 } } });
    try {
      const page = await openGame(browser, sim, { flags: { __czEncounter: false, __czGod: true, __czClock: CLOCK, __czAutopilot: true, __czAutoLand: true } });
      await launch(page);
      // a short final: 280 m out, slow and low, then land — the discovery is made on the ground, the camera shows the ruin
      let si = await page.eval(`window.__cz.dev.nearSite(0, ['gate'], 280, 90, 25, 30)`); if (si < 0) si = await page.eval(`window.__cz.dev.nearSite(0, ['archive', 'observatory'], 280, 90, 25, 30)`);
      if (si < 0) throw new Error('no ruin on this world');
      await sleep(800); await page.eval(`import('/apps/games/games/stelle/sim.js').then((S) => S.cmd(window.__cz.game.flight, 'land'))`);
      await until(page, (v) => v.st === 3, 20000); await sleep(2500); await shot(page, 'world-ruin-approach');
      await until(page, (v) => v.found > 0); await sleep(900); await shot(page, 'world-ruin-discovery');
      await until(page, (v) => v.st === 5); await sleep(1300); await shot(page, 'world-takeoff');
      await blank(page);
    } finally { await sim.stop(); }
  },
  // M3 — a relic recovered from a jungle shrine: the Echo's sentinels rise and the fight is low over the canopy
  async groundfight(browser) {
    const sim = await startSim({ seed: { [SAVE_PATH]: { ...SAVE_SEED, sys: 5 } } });
    try {
      const page = await openGame(browser, sim, { flags: { __czEncounter: false, __czGod: true, __czClock: CLOCK, __czAutopilot: true, __czAutoLand: true } });
      await launch(page);
      let si = -1;
      for (let pi = 0; pi < 6 && si < 0; pi++) si = await page.eval(`window.__cz.dev.nearSite(${pi}, ['relic'], 280, 90, 25, 40)`);
      if (si < 0) throw new Error('no relic in this system');
      await sleep(800); await page.eval(`import('/apps/games/games/stelle/sim.js').then((S) => S.cmd(window.__cz.game.flight, 'land'))`);
      await page.waitFor(`window.__cz.game.flight.ships.some((s) => s.alive && s.nameKey === 'cz_ship_sentinel')`, { timeout: 120000, interval: 200 });
      await until(page, (v) => v.st === 2, 60000);
      // hands to the combat autopilot: it turns on the drones instead of flying on to the next signal
      await page.eval(`window.__cz.game.flight.autoLand = false, true`);
      await page.waitFor(`(() => { const F = window.__cz.game.flight, p = F.player; return F.ships.some((s) => s.alive && s.nameKey === 'cz_ship_sentinel' && Math.hypot(s.pos.x - p.pos.x, s.pos.y - p.pos.y, s.pos.z - p.pos.z) < 900); })()`, { timeout: 30000, interval: 200 });
      await sleep(2500); await shot(page, 'world-combat');
      await sleep(2500); await shot(page, 'world-combat-b');
      await blank(page);
    } finally { await sim.stop(); }
  },
  // M3 — the planet HUD flying at a signal: 1440x900 (or --size) and a phone with touch controls
  async worldhud(browser) {
    const sim = await startSim({ seed: { [SAVE_PATH]: { ...SAVE_SEED, sys: 9 } } });
    try {
      for (const [name, vp, mobile] of [['world-hud', [W, H], false], ['world-hud-phone', [390, 844], true]]) {
        const page = await browser.newPage();
        await page.setViewport(vp[0], vp[1], mobile);
        if (mobile) await page.send('Emulation.setTouchEmulationEnabled', { enabled: true, maxTouchPoints: 5 });
        await page.initScript(`Object.assign(window, ${JSON.stringify({ __czEncounter: false, __czGod: true, __czClock: CLOCK })});`);
        await bootShell(page, sim, { lang: LANG, wait: false });
        await page.goto(`${sim.origin}/apps/games/?host=constellations`);
        if (!await page.waitFor(`document.getElementById('startBtn') && !document.getElementById('startBtn').disabled && document.getElementById('startBtn').offsetWidth > 0`, { timeout: 45000 })) throw new Error('waiting room');
        await page.eval(`document.getElementById('startBtn').click(), true`);
        await page.waitFor(`(() => { const m = document.querySelector('[data-modal]'), h = document.querySelector('[data-hub]'); return (m && m.style.display === 'flex' && m.querySelector('.cz-btn')) || (h && h.style.display === 'block'); })()`, { timeout: 45000 });
        await launch(page);
        await page.eval(`window.__cz.dev.nearSite(0, ['gate', 'observatory', 'archive'], 2600, 180, 120, 30)`);
        await sleep(2500); await page.eval(`import('/apps/games/games/stelle/sim.js').then((S) => S.cmd(window.__cz.game.flight, 'scan'))`);
        await sleep(1400); await shot(page, name);
        await blank(page);
      }
    } finally { await sim.stop(); }
  },
  async kit(browser) {
    const sim = await startSim({ seed: { [SAVE_PATH]: SAVE_SEED } });
    try {
      const page = await openGame(browser, sim); await toHub(page); await sleep(800);
      await page.eval(`document.querySelector('[data-cz]').style.visibility = 'hidden', true`);
      const files = [];
      for (const [name, r] of Object.entries(KIT_ROWS)) { await page.eval(`window.__cz.showcase(${JSON.stringify(r.list)}, ${JSON.stringify(r.o)}), true`); await sleep(1600); files.push(await shot(page, name)); }
      await page.eval(`window.__cz.showcase(null), true`);
    } finally { await sim.stop(); }
  },
  async stations(browser) {
    const sim = await startSim({ seed: { [SAVE_PATH]: SAVE_SEED } });
    try {
      const page = await openGame(browser, sim); await toHub(page); await sleep(800);
      await page.eval(`document.querySelector('[data-cz]').style.visibility = 'hidden', true`);
      for (const [fac, name] of [[0, 'station-gilda'], [1, 'station-custodi'], [2, 'station-relitti']]) {
        const info = await page.eval(`JSON.stringify(window.__cz.showStation(${fac}, { yaw: 0.6 }))`); console.log('   ', name, info);
        await sleep(1800); await shot(page, name);
        await page.eval(`window.__cz.showStation(${fac}, { yaw: 2.4, elev: 0.08 }), true`); await sleep(1200); await shot(page, name + '-b');
      }
      await page.eval(`window.__cz.showStation(null), true`);
    } finally { await sim.stop(); }
  },
};

if (process.argv[1] && process.argv[1].endsWith('costellazioni-shots.mjs')) {
  if (!findChrome()) { console.error('no Chrome/Edge found (set CHROME_PATH)'); process.exit(2); }
  mkdirSync(OUT, { recursive: true });
  const want = arg('scenes', Object.keys(SCENES).join(',')).split(',').filter(Boolean);
  const browser = await launchBrowser({ gpu: GPU, args: ['--host-resolver-rules=MAP nucleo.test 127.0.0.1'] });
  let failed = 0;
  try {
    for (const s of want) {
      if (!SCENES[s]) { console.error('unknown scene', s); failed++; continue; }
      console.log('scene', s);
      try { await SCENES[s](browser); } catch (e) { failed++; console.error('  scene', s, 'failed:', e.message); }
    }
  } finally { await browser.close(); }
  process.exit(failed ? 1 : 0);
}
export { SCENES, KIT_ROWS };
