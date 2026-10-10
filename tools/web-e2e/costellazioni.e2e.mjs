// Browser E2E — Costellazioni (apps/games: games/constellations*.js + games/stelle/*) against the simulator.
//   - boot: the Game Center starts the game, a card with no save offers a new run, the hub paints in all five
//     languages from the games catalog (no raw cz_* keys), with no console errors;
//   - fly + fight: a mission launches into the 6DOF flight (WebGL canvas, HUD, sim stepping at 60 Hz), the
//     sortie runs to the debrief (autopilot + test time-scale), and the run on the SD is updated the way the
//     Cardputer expects: credits = old + reward + kill_cr × kills, kills added, hull ≥ 1, contract unchanged;
//   - pause and retreat from the in-flight menu;
//   - illustrations and music are stored once: a reload reads them from IndexedDB, not the network;
//   - (E2E_GPU=1) frame-time budget on the real GPU at the auto quality tier.
// Screenshots for review land in build/web-e2e/costellazioni/.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim, REPO } from './sim.mjs';
import { bootShell, LANGS } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const rd = (p) => JSON.parse(readFileSync(new URL(p, import.meta.url), 'utf8'));
const cat = (lang) => ({ ...rd(`../../web/shell/i18n/core.${lang}.json`), ...rd(`../../apps/games/www/i18n.${lang}.json`) });
const OUT = join(REPO, 'build', 'web-e2e', 'costellazioni');
const SAVE = '/data/games/costellazioni.json';
const SAVE0 = { ver: 3, credits: 1500, fuel: 8, fuel_max: 8, hull: 100, hull_max: 100, cargo_max: 20, jump_range: 64, sensors: 1, weapon: 1, shield_max: 40, sys: 0,
  cargo: [0, 0, 0, 0, 0, 0, 0, 0], rep: [0, 0, 0, 0], flags: 0, beacon_lit: 0, epoch: 1, missions_done: 0, kills: 3, seed: 987654, sector: 1 };

// errors that are not the game's: the save endpoint answers 404 when the card has no save (that is the
// "new run" path), and the favicon of the suite's own JSON prep page
function gameErrors(page) {
  return page.log.filter((e) => {
    if (e.kind === 'exception') return true;
    if (e.kind === 'console') return !/favicon/.test(e.text || '');
    if (e.kind === 'response') { const p = new URL(e.url).pathname; return !(p === '/favicon.ico' || p === '/api/game/costellazioni/save' || (p === '/api/fs/read' && e.status === 404)); }
    if (e.kind === 'failed') return true;
    return false;
  }).map((e) => `${e.kind}: ${e.text || ''} ${e.status || ''} ${e.url || ''}`.trim());
}
const key = (page, k) => page.eval(`window.dispatchEvent(new KeyboardEvent('keydown', { key: ${JSON.stringify(k)}, bubbles: true })), true`);
const frames = (page) => page.eval('new Promise(r => requestAnimationFrame(() => requestAnimationFrame(() => setTimeout(r, 30))))');

async function openGame(browser, sim, { lang = 'en', flags = {}, viewport = [1280, 800] } = {}) {
  const page = await browser.newPage();
  await page.setViewport(viewport[0], viewport[1]);
  await page.initScript(`Object.assign(window, ${JSON.stringify(flags)});`);
  await bootShell(page, sim, { lang, wait: false });
  await page.goto(`${sim.origin}/apps/games/?host=constellations`);
  assert.ok(await page.waitFor(`document.getElementById('startBtn') && !document.getElementById('startBtn').disabled && document.getElementById('startBtn').offsetWidth > 0`, { timeout: 45000 }), 'waiting room');
  await page.eval(`document.getElementById('startBtn').click(), true`);
  assert.ok(await page.waitFor(`(() => { const m = document.querySelector('[data-modal]'), h = document.querySelector('[data-hub]'); return (m && m.style.display === 'flex' && m.querySelector('.cz-btn')) || (h && h.style.display === 'block'); })()`, { timeout: 45000 }), 'the game opened');
  return page;
}
async function toHub(page) {
  if (await page.eval(`document.querySelector('[data-modal]').style.display === 'flex'`)) await key(page, 'Enter');
  assert.ok(await page.waitFor(`document.querySelector('[data-hub]').style.display === 'block' && document.querySelectorAll('.cz-tab').length === 5`, { timeout: 30000 }), 'hub painted');
}

test('costellazioni: the hub paints in all five languages, from the catalog, with no errors', { skip, timeout: 8 * 60 * 1000 }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  for (const lang of LANGS) {
    const c = cat(lang);
    const page = await openGame(browser, sim, { lang });
    // the first language starts the run (it is saved to the card); the others continue it
    if (lang === LANGS[0]) assert.equal(await page.eval(`document.querySelector('[data-modal] h3') && document.querySelector('[data-modal] h3').textContent`), c.cz_ui_no_run, lang + ': new-run offer');
    await toHub(page);
    const tabs = await page.eval(`[...document.querySelectorAll('.cz-tab')].map((x) => x.textContent)`);
    assert.deepEqual(tabs, [c.cz_tab_bridge, c.cz_tab_map, c.cz_tab_market, c.cz_tab_shipyard, c.cz_tab_missions], lang + ': tabs');
    for (const scr of ['1', '2', '3', '4', '5']) {
      await key(page, scr); await sleep(250);
      const txt = await page.eval(`document.querySelector('[data-cz]').innerText`);
      assert.ok(!/\bcz_[a-z_0-9]+/.test(txt), `${lang}: no raw catalog keys on screen ${scr}: ${(txt.match(/\bcz_[a-z_0-9]+/) || [])[0]}`);
    }
    await key(page, '1'); await sleep(400); await frames(page);
    await page.screenshot(join(OUT, `hub-${lang}.png`));
    assert.deepEqual(gameErrors(page), [], lang + ': no errors');
    assert.equal(await page.eval(`!!(window.__cz && window.__cz.perf)`), true, lang + ': the 3D renderer is up');
  }
});

test('costellazioni: a mission flies, fights and writes the shared save', { skip, timeout: 10 * 60 * 1000 }, async (t) => {
  const sim = await startSim({ seed: { [SAVE]: SAVE0 } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await openGame(browser, sim, { flags: { __czAutopilot: true, __czGod: true, __czTimeScale: 8 } });
  await toHub(page);
  assert.equal(await page.eval(`window.__cz.game.run.credits`), SAVE0.credits, 'continues the run on the card');
  const missions = await page.eval(`(() => { const r = window.__cz.game.run; return r ? 1 : 0; })()`);
  assert.equal(missions, 1);
  await key(page, '5'); await sleep(300);
  await key(page, 'Enter');
  assert.ok(await page.waitFor(`!!(window.__cz.game.flight && window.__cz.game.flight.t > 1)`, { timeout: 30000 }), 'the flight runs');
  const live = await page.eval(`(() => { const F = window.__cz.game.flight; return { ships: F.ships.filter((s) => s.alive).length, hud: getComputedStyle(document.querySelector('.sh-root')).display, gl: !!document.getElementById('board').getContext && window.__cz.perf.frames > 10, kind: F.mission.kind }; })()`);
  assert.ok(live.ships >= 1 && live.hud === 'block' && live.gl, 'flight + HUD: ' + JSON.stringify(live));
  assert.ok(await page.waitFor(`window.__cz.game.flight.wave >= 1 && window.__cz.game.flight.ships.filter((s) => s.alive).length >= 3`, { timeout: 60000 }), 'hostiles arrive');
  await sleep(1500); await frames(page);
  await page.screenshot(join(OUT, 'flight.png'));
  const cc = await page.eval(`(() => { const F = window.__cz.game.flight; return { reward: F.cc.rewardCr, killCr: F.cc.killCr, mission: F.cc.mission }; })()`);
  assert.ok(await page.waitFor(`document.querySelector('[data-deb]') && document.querySelector('[data-deb]').style.display === 'flex'`, { timeout: 6 * 60 * 1000, interval: 500 }), 'reached the debrief');
  const F = await page.eval(`(() => { const F = window.__cz.game.flight; return { outcome: F.outcome, kills: F.kills, hull: F.player.hull }; })()`);
  await frames(page); await page.screenshot(join(OUT, 'debrief.png'));
  assert.equal(F.outcome, 1, 'the sortie was won');
  let disk = null;
  for (let i = 0; i < 40; i++) { disk = JSON.parse(await sim.readSd(SAVE)); if (disk.kills !== SAVE0.kills) break; await sleep(250); }
  assert.equal(disk.kills, SAVE0.kills + F.kills, 'kills added to the run');
  assert.equal(disk.credits, SAVE0.credits + cc.reward + cc.killCr * F.kills, 'credits: reward + kill bounty');
  assert.ok(disk.hull >= 1 && disk.hull <= disk.hull_max, 'hull written back, never 0');
  assert.deepEqual(Object.keys(disk).sort(), Object.keys(SAVE0).sort(), 'the save keeps the shared contract');
  await key(page, 'Enter');
  assert.ok(await page.waitFor(`document.querySelector('[data-hub]').style.display === 'block'`, { timeout: 10000 }), 'back at the hub');
  assert.deepEqual(gameErrors(page), [], 'no errors');
});

test('costellazioni: pause and retreat from the flight menu', { skip, timeout: 5 * 60 * 1000 }, async (t) => {
  const sim = await startSim({ seed: { [SAVE]: SAVE0 } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await openGame(browser, sim, { lang: 'it' });
  await toHub(page);
  await key(page, '5'); await sleep(300); await key(page, 'Enter');
  assert.ok(await page.waitFor(`!!(window.__cz.game.flight && window.__cz.game.flight.t > 0.5)`, { timeout: 30000 }), 'flying');
  await key(page, 'Escape');
  assert.ok(await page.waitFor(`document.querySelector('.sh-pause').classList.contains('on')`, { timeout: 5000 }), 'Esc opens the pause menu');
  const t0 = await page.eval('window.__cz.game.flight.t'); await sleep(700);
  assert.equal(await page.eval('window.__cz.game.flight.t'), t0, 'the sim is frozen while paused');
  const it = cat('it');
  assert.ok((await page.eval(`document.querySelector('.sh-pbox').innerText`)).includes(it.cz_pause_retreat), 'menu in Italian');
  await page.eval(`document.querySelector('.sh-pbox [data-a="retreat"]').click(), true`);
  assert.ok(await page.waitFor(`document.querySelector('[data-deb]').style.display === 'flex'`, { timeout: 20000 }), 'retreat ends in the debrief');
  assert.equal(await page.eval(`document.querySelector('.cz-deb .t').textContent`), it.cz_deb_retreat);
  assert.deepEqual(gameErrors(page), [], 'no errors');
});

test('costellazioni: illustrations and music download once, then come from IndexedDB', { skip, timeout: 5 * 60 * 1000 }, async (t) => {
  const sim = await startSim({ seed: { [SAVE]: SAVE0 } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await openGame(browser, sim);
  await toHub(page);
  const fetched = () => page.eval(`performance.getEntriesByType('resource').filter((e) => /\\/stelle\\/assets\\/(img|music)\\//.test(e.name)).map((e) => e.name.split('/').pop())`);
  assert.ok(await page.waitFor(`performance.getEntriesByType('resource').some((e) => /\\/stelle\\/assets\\/img\\//.test(e.name))`, { timeout: 30000 }), 'the hub streamed its illustrations');
  // wait until what the first visit downloaded is in the store (the hub plays a track, paints its art)
  const stored = () => page.eval(`new Promise((res) => { const rq = indexedDB.open('stelle-assets', 1); rq.onsuccess = () => { const t = rq.result.transaction('files', 'readonly').objectStore('files').getAllKeys(); t.onsuccess = () => res(t.result.map((k) => k.split('/').pop())); t.onerror = () => res([]); }; rq.onerror = () => res([]); })`);
  let first = [];
  for (let i = 0; i < 60; i++) { first = await fetched(); const s = await stored(); if (first.length && first.every((f) => s.includes(f))) break; await sleep(500); }
  assert.deepEqual((await stored()).filter((f) => first.includes(f)).sort(), [...new Set(first)].sort(), 'everything downloaded is stored in IndexedDB');
  assert.ok(first.length > 0, 'first visit downloads: ' + first.join(', '));
  // reload: the same art must come from IndexedDB
  await page.goto(`${sim.origin}/apps/games/?host=constellations`);
  assert.ok(await page.waitFor(`document.getElementById('startBtn') && !document.getElementById('startBtn').disabled && document.getElementById('startBtn').offsetWidth > 0`, { timeout: 45000 }));
  await page.eval(`document.getElementById('startBtn').click(), true`);
  await toHub(page);
  assert.ok(await page.waitFor(`!!document.querySelector('.cz-hero') && getComputedStyle(document.querySelector('.cz-hero')).backgroundImage.startsWith('url("blob:')`, { timeout: 20000 }), 'the bridge art is painted from the stored blob');
  await sleep(2000);
  const again = (await fetched()).filter((f) => first.includes(f));
  assert.deepEqual(again, [], 'a reload downloads nothing it already stored: ' + again.join(', '));
});

test('costellazioni: frame-time budget on the real GPU (auto tier)', { skip: skip || (process.env.E2E_GPU !== '1' && 'set E2E_GPU=1 (needs a GPU)'), timeout: 5 * 60 * 1000 }, async (t) => {
  const sim = await startSim({ seed: { [SAVE]: SAVE0 } });
  const browser = await launchBrowser({ gpu: true, args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await openGame(browser, sim, { flags: { __czAutopilot: true, __czGod: true }, viewport: [1920, 1080] });
  await toHub(page);
  await key(page, '5'); await sleep(300); await key(page, 'Enter');
  assert.ok(await page.waitFor(`!!(window.__cz.game.flight && window.__cz.game.flight.t > 8)`, { timeout: 60000 }));
  const p = await page.eval(`new Promise((res) => { const P = window.__cz.perf; const h0 = P.hi; setTimeout(() => { const n = Math.min(P.hi - h0, P.hist.length), a = []; for (let i = 0; i < n; i++) a.push(P.hist[(P.hi - 1 - i) % P.hist.length]); a.sort((x, y) => x - y); res({ tier: P.tier, scale: P.scale, renderer: P.renderer, n, avg: a.reduce((s, x) => s + x, 0) / n, p95: a[Math.floor(n * 0.95)] }); }, 4000); })`);
  console.log('  frame time', JSON.stringify(p));
  assert.ok(p.avg < 17.5 && p.p95 < 25, `60 fps budget: avg ${p.avg.toFixed(1)} ms, p95 ${p.p95.toFixed(1)} ms on ${p.renderer}`);
});
