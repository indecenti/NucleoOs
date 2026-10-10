// Visual review for Costellazioni (web): boots the game in headless Chrome against the simulator and saves
// named scenes as PNGs, so a change to the 3D game can be LOOKED AT without a human driving it.
//   node tools/web-e2e/costellazioni-shots.mjs                       → every scene, build/web-e2e/costellazioni/review/
//   node tools/web-e2e/costellazioni-shots.mjs --scenes kit,stations --gpu --size 1440x900
//   --lang it   --out <dir>   --gpu (real GPU, like E2E_GPU=1)
// Scenes are plain async functions below; each gets a fresh page with a seeded run (sector 1, credits, a relic).
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
const GPU = flag('gpu') || process.env.E2E_GPU === '1';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
export const SAVE_PATH = '/data/games/costellazioni.json';
export const SAVE_SEED = { ver: 3, credits: 4200, fuel: 8, fuel_max: 8, hull: 100, hull_max: 100, cargo_max: 20, jump_range: 64, sensors: 1, weapon: 1, shield_max: 40, sys: 0,
  cargo: [0, 0, 0, 0, 0, 0, 2, 0], rep: [10, 5, 0, 0], flags: 0, beacon_lit: 0, epoch: 3, missions_done: 0, kills: 12, seed: 987654, sector: 1 };

const key = (page, k) => page.eval(`window.dispatchEvent(new KeyboardEvent('keydown', { key: ${JSON.stringify(k)}, bubbles: true })), true`);
const frames = (page, n = 2) => page.eval(`new Promise(r => { let k = ${n}; const f = () => (--k > 0 ? requestAnimationFrame(f) : setTimeout(r, 30)); requestAnimationFrame(f); })`);
async function shot(page, name) { await frames(page, 3); const f = join(OUT, name + '.png'); const ok = await page.screenshot(f); console.log((ok ? '  ✓ ' : '  ✗ ') + f); return f; }

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
  if (await page.eval(`document.querySelector('[data-modal]').style.display === 'flex'`)) await key(page, 'Enter');
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
