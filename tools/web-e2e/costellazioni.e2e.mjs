// Browser E2E — Costellazioni (apps/games: games/constellations*.js + games/stelle/*) against the simulator.
//   - boot: the Game Center starts the game, a card with no save offers a new run, the hub paints in all five
//     languages from the games catalog (no raw cz_* keys), with no console errors;
//   - fly + fight: a mission launches into the 6DOF flight (WebGL canvas, HUD, sim stepping at 60 Hz), the
//     sortie runs to the debrief (autopilot + test time-scale), and the run on the SD is updated the way the
//     Cardputer expects: credits = old + reward + kill_cr × kills, kills added, hull ≥ 1, contract unchanged;
//   - pause and retreat from the in-flight menu;
//   - illustrations and music are stored once: a reload reads them from IndexedDB, not the network;
//   - M2: free flight from the bridge ends docked at the station; the 3D galaxy map plots a jump that plays as a
//     cinematic and lands in the new system; the relight is flown and the lit beacon persists in the shared save
//     (beacon_lit) while visited systems / relit history / codex go to the web save next to it; the codex pages;
//   - M3: a world visited end to end (the dive, the entry, the helm, low flight to a signal, landing = a discovery
//     paid into the run and kept in the web save, take-off, the climb back to orbit); a phone viewport with touch
//     emulation (touch controls on screen, SCN pings, L lands and takes off); a gamepad opens and closes the jump map;
//   - (E2E_GPU=1) frame-time budget on the real GPU at the auto quality tier, in combat, in free flight and low over
//     a world (plus frame times per tier there), and no non-finite (NaN/Inf) pixels in the HDR frame.
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
  // a choice (continue / new run) may come up first — but not the "syncing your run" card: an Enter pressed on that
  // one carries into the hub a moment later and launches a flight
  await page.waitFor(`(() => { const m = document.querySelector('[data-modal]'), h = document.querySelector('[data-hub]'); return (m && m.style.display === 'flex' && m.querySelector('.cz-btn')) || (h && h.style.display === 'block'); })()`, { timeout: 30000 });
  if (await page.eval(`(() => { const m = document.querySelector('[data-modal]'); return m.style.display === 'flex' && !!m.querySelector('.cz-btn'); })()`)) await key(page, 'Enter');
  assert.ok(await page.waitFor(`document.querySelector('[data-hub]').style.display === 'block' && document.querySelectorAll('.cz-tab').length === 6`, { timeout: 30000 }), 'hub painted');
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
    const tabs = await page.eval(`[...document.querySelectorAll('.cz-tab')].map((x) => x.textContent.replace('●', '').trim())`);
    assert.deepEqual(tabs, [c.cz_tab_bridge, c.cz_tab_map, c.cz_tab_market, c.cz_tab_shipyard, c.cz_tab_missions, c.cz_tab_codex], lang + ': tabs');
    for (const scr of ['1', '2', '3', '4', '5', '6']) {
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
  // compare by <name>.<hash>: a file over the device's large-file gate is fetched as <name>.<hash>.partN slices and
  // stored joined under its logical <name>.<hash>.<ext> (assets.js)
  const stem = (f) => f.replace(/\.(part\d+|[a-z0-9]+)$/, '');
  const fetched = async () => [...new Set((await page.eval(`performance.getEntriesByType('resource').filter((e) => /\\/stelle\\/assets\\/(img|music)\\//.test(e.name)).map((e) => e.name.split('/').pop())`)).map(stem))];
  assert.ok(await page.waitFor(`performance.getEntriesByType('resource').some((e) => /\\/stelle\\/assets\\/img\\//.test(e.name))`, { timeout: 30000 }), 'the hub streamed its illustrations');
  // wait until what the first visit downloaded is in the store (the hub plays a track, paints its art)
  const stored = async () => (await page.eval(`new Promise((res) => { const rq = indexedDB.open('stelle-assets', 1); rq.onsuccess = () => { const t = rq.result.transaction('files', 'readonly').objectStore('files').getAllKeys(); t.onsuccess = () => res(t.result.map((k) => k.split('/').pop())); t.onerror = () => res([]); }; rq.onerror = () => res([]); })`)).map(stem);
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

const WEB = '/data/costellazioni/web.json';
const readJson = async (sim, p) => { try { return JSON.parse(await sim.readSd(p)); } catch { return null; } };

test('costellazioni: free flight from the bridge, docking, and the web save next to the shared one', { skip, timeout: 6 * 60 * 1000 }, async (t) => {
  const sim = await startSim({ seed: { [SAVE]: { ...SAVE0, sys: 6 } } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await openGame(browser, sim, { flags: { __czAutopilot: true, __czAutoDock: true, __czEncounter: false, __czGod: true, __czTimeScale: 4 } });
  await toHub(page);
  await key(page, '1'); await sleep(200); await key(page, 'Enter');
  assert.ok(await page.waitFor(`!!(window.__cz.game.flight && window.__cz.game.flight.mission.kind === 'explore')`, { timeout: 20000 }), 'launch = free flight');
  const pois = await page.eval(`window.__cz.game.flight.pois.map((p) => p.kind)`);
  assert.ok(pois.includes('station') && pois.includes('planet'), 'destinations: ' + pois.join(','));
  assert.ok(await page.waitFor(`!!(window.__cz.game.flight && window.__cz.game.flight.dock && window.__cz.game.flight.dock.ph >= 1)`, { timeout: 120000, interval: 300 }), 'clearance and the docking run');
  await frames(page); await page.screenshot(join(OUT, 'docking.png'));
  assert.ok(await page.waitFor(`document.querySelector('[data-hub]').style.display === 'block' && !window.__cz.game.flight`, { timeout: 120000, interval: 300 }) || await page.waitFor(`document.querySelector('[data-hub]').style.display === 'block'`, { timeout: 5000 }), 'docked: back at the hub');
  let web = null; for (let i = 0; i < 30 && !web; i++) { web = await readJson(sim, WEB); if (!web) await sleep(300); }
  assert.ok(web && web.seed === SAVE0.seed && ((web.visited[SAVE0.sector] >>> 0) & (1 << 6)), 'the web save on the card records the visited system');
  assert.ok(web.codex && web.codex.costellatori != null && web.codex.gilda != null, 'and the codex entries');
  const disk = await readJson(sim, SAVE);
  assert.deepEqual(Object.keys(disk).sort(), Object.keys(SAVE0).sort(), 'the shared save keeps its contract');
  assert.deepEqual(gameErrors(page), [], 'no errors');
});

test('costellazioni: the 3D galaxy map plots a jump that plays out and lands in the new system', { skip, timeout: 5 * 60 * 1000 }, async (t) => {
  const sim = await startSim({ seed: { [SAVE]: { ...SAVE0, sys: 6 } } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await openGame(browser, sim);
  await toHub(page);
  await key(page, '2'); await sleep(1500);
  assert.ok(await page.waitFor(`[...document.querySelectorAll('div')].some((d) => d.style.zIndex === '28' && d.style.display === 'block' && d.children.length >= 10)`, { timeout: 10000 }), 'the 3D map shows its system labels');
  await key(page, 'Tgt:5'); await sleep(400);
  assert.equal(await page.eval(`document.querySelector('.cz-mapinfo') && document.querySelector('.cz-mapinfo').innerText.includes(window.__cz.game.model.sector[5].it)`), true, 'the panel shows the picked system');
  await frames(page); await page.screenshot(join(OUT, 'galaxy-map.png'));
  const fuel0 = (await readJson(sim, SAVE)).fuel;
  await key(page, 'Enter');
  assert.ok(await page.waitFor(`document.querySelector('[data-cz]').classList.contains('cine')`, { timeout: 5000 }), 'the hub steps aside for the jump');
  assert.ok(await page.waitFor(`window.__cz.game.run.sys === 5 && !document.querySelector('[data-cz]').classList.contains('cine')`, { timeout: 20000 }), 'and comes back in the new system');
  let disk = null; for (let i = 0; i < 30; i++) { disk = await readJson(sim, SAVE); if (disk.sys === 5) break; await sleep(250); }
  assert.ok(disk.sys === 5 && disk.fuel < fuel0, 'the jump is in the shared save (system, fuel)');
  assert.deepEqual(gameErrors(page), [], 'no errors');
});

test('costellazioni: the relight is flown and the beacon stays lit (shared beacon_lit + web history)', { skip, timeout: 8 * 60 * 1000 }, async (t) => {
  const sim = await startSim({ seed: { [SAVE]: { ...SAVE0, sys: 7, cargo: [0, 0, 0, 0, 0, 0, 1, 0], credits: 2000 } } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await openGame(browser, sim, { flags: { __czAutopilot: true, __czGod: true, __czTimeScale: 6 } });
  await toHub(page);
  await key(page, '2'); await sleep(600); await key(page, 'ArrowDown'); await sleep(200); await key(page, 'Enter');
  assert.ok(await page.waitFor(`!!(window.__cz.game.flight && window.__cz.game.flight.mission.relightWanted)`, { timeout: 20000 }), 'the relight flight starts');
  assert.ok(await page.waitFor(`!!(window.__cz.game.flight && window.__cz.game.flight.mission.relight)`, { timeout: 90000, interval: 300 }), 'the relic is seated, the charge begins');
  assert.ok(await page.waitFor(`!!(window.__cz.game.flight && window.__cz.game.flight.relit)`, { timeout: 240000, interval: 500 }), 'the beacon bursts into light');
  await frames(page); await page.screenshot(join(OUT, 'relight.png'));
  assert.ok(await page.waitFor(`document.querySelector('[data-deb]').style.display === 'flex'`, { timeout: 180000, interval: 500 }), 'the debrief');
  const it = cat('en');
  assert.equal(await page.eval(`document.querySelector('.cz-deb .t').textContent`), it.cz_deb_relit);
  let disk = null; for (let i = 0; i < 40; i++) { disk = await readJson(sim, SAVE); if ((disk.beacon_lit >>> 0) & (1 << 7)) break; await sleep(250); }
  assert.ok((disk.beacon_lit >>> 0) & (1 << 7), 'beacon_lit bit 7 is set on the card');
  assert.equal(disk.cargo[6], 0, 'the relic is spent');
  assert.deepEqual(Object.keys(disk).sort(), Object.keys(SAVE0).sort(), 'the shared save keeps its contract');
  let web = null; for (let i = 0; i < 30; i++) { web = await readJson(sim, WEB); if (web && web.relights) break; await sleep(300); }
  assert.ok(web && web.relights >= 1 && ((web.relit[SAVE0.sector] >>> 0) & (1 << 7)), 'the web save keeps the relit beacon');
  assert.ok(web.codex.relight != null, 'and unlocks its codex entry');
  assert.deepEqual(gameErrors(page), [], 'no errors');
});

test('costellazioni: the codex pages through its categories in the OS language', { skip, timeout: 3 * 60 * 1000 }, async (t) => {
  const sim = await startSim({ seed: { [SAVE]: SAVE0 } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await openGame(browser, sim, { lang: 'de' });
  await toHub(page);
  const de = cat('de');
  await key(page, '6'); await sleep(500);
  assert.equal(await page.eval(`document.querySelector('.cz-cxdet h2').textContent`), de.cz_cx_costellatori_t, 'opens on the first lore entry, in German');
  for (let i = 0; i < 4; i++) await key(page, 'ArrowRight');
  await sleep(1200);
  assert.ok(await page.eval(`!!document.querySelector('.cz-hangar')`), 'ship dossiers open the hangar');
  assert.ok(await page.waitFor(`window.__cz.r3d && document.querySelector('.cz-cxtxt h2') && document.querySelector('.cz-cxtxt h2').textContent === ${JSON.stringify(de.cz_cx_s_courier_t)}`, { timeout: 5000 }), 'the courier dossier is open');
  const txt = await page.eval(`document.querySelector('[data-cz]').innerText`);
  assert.ok(!/\bcz_[a-z_0-9]+/.test(txt), 'no raw keys');
  assert.deepEqual(gameErrors(page), [], 'no errors');
});

test('costellazioni: free-flight frame time on the real GPU, and a clean HDR frame', { skip: skip || (process.env.E2E_GPU !== '1' && 'set E2E_GPU=1 (needs a GPU)'), timeout: 5 * 60 * 1000 }, async (t) => {
  const sim = await startSim({ seed: { [SAVE]: { ...SAVE0, sys: 6 } } });
  const browser = await launchBrowser({ gpu: true, args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await openGame(browser, sim, { flags: { __czAutopilot: true, __czEncounter: false, __czGod: true }, viewport: [1920, 1080] });
  await toHub(page);
  await key(page, '1'); await sleep(200); await key(page, 'Enter');
  assert.ok(await page.waitFor(`!!(window.__cz.game.flight && window.__cz.game.flight.t > 6)`, { timeout: 60000 }));
  const p = await page.eval(`new Promise((res) => { const P = window.__cz.perf; const h0 = P.hi; setTimeout(() => { const n = Math.min(P.hi - h0, P.hist.length), a = []; for (let i = 0; i < n; i++) a.push(P.hist[(P.hi - 1 - i) % P.hist.length]); a.sort((x, y) => x - y); res({ tier: P.tier, scale: P.scale, n, avg: a.reduce((s, x) => s + x, 0) / n, p95: a[Math.floor(n * 0.95)] }); }, 4000); })`);
  console.log('  frame time (free flight)', JSON.stringify(p));
  assert.ok(p.avg < 17.5 && p.p95 < 25, `60 fps budget in free flight: avg ${p.avg.toFixed(1)} ms, p95 ${p.p95.toFixed(1)} ms`);
  assert.equal(await page.eval(`JSON.stringify(window.__cz.nanScan(480, 300).filter((x) => x.nan))`), '[]', 'no non-finite pixels in the HDR frame (free flight)');
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
  console.log('  frame time (combat)', JSON.stringify(p));
  assert.ok(p.avg < 17.5 && p.p95 < 25, `60 fps budget: avg ${p.avg.toFixed(1)} ms, p95 ${p.p95.toFixed(1)} ms on ${p.renderer}`);
  const nan = await page.eval(`JSON.stringify(window.__cz.nanScan(480, 300).filter((x) => x.nan))`);
  assert.equal(nan, '[]', 'no non-finite pixels in the HDR frame (combat)');
});

// ---- M3: worlds ---------------------------------------------------------------------------------------------------------
const surfState = (page) => page.eval(`(() => { const F = window.__cz.game.flight; if (!F || !F.surf) return null; const U = F.surf; return { st: U.st, alt: Math.round(U.alt), agl: Math.round(U.agl), heat: U.heat, found: U.found, w: U.w, left: !!U.left, world: window.__cz.r3d ? window.__cz.r3d.worldIdx : -2 }; })()`);
const frameStats = (page, ms = 4000) => page.eval(`new Promise((res) => { const P = window.__cz.perf; const h0 = P.hi, w0 = P.wi; setTimeout(() => { const st = (H, i1, i0) => { const n = Math.min(i1 - i0, H.length), a = []; for (let i = 0; i < n; i++) a.push(H[(i1 - 1 - i) % H.length]); a.sort((x, y) => x - y); return n ? { n, avg: +(a.reduce((s, x) => s + x, 0) / n).toFixed(2), p95: +a[Math.floor(n * 0.95)].toFixed(2) } : { n: 0, avg: 0, p95: 0 }; }; const f = st(P.hist, P.hi, h0), w = st(P.work, P.wi, w0), W = window.__cz.r3d.world; res({ tier: P.tier, scale: +(+P.scale).toFixed(2), n: f.n, avg: f.avg, p95: f.p95, work: w.avg, workP95: w.p95, chunks: W ? W.stats.visible : 0, flora: W ? W.stats.flora : 0, gpu: (() => { try { const gl = document.createElement('canvas').getContext('webgl2'), e = gl.getExtension('WEBGL_debug_renderer_info'); return String(gl.getParameter(e.UNMASKED_RENDERER_WEBGL)).split('(0x')[0].replace('ANGLE (', '').trim(); } catch { return ''; } })() }); }, ${ms}); })`);
const SURF = { SPACE: 0, ENTRY: 1, FLIGHT: 2, LANDING: 3, LANDED: 4, TAKEOFF: 5, ASCENT: 6, DESCENT: 7 };
async function launchFlight(page) {
  await toHub(page);
  await key(page, '1'); await sleep(200); await key(page, 'Enter');
  assert.ok(await page.waitFor(`!!(window.__cz.game.flight && window.__cz.game.flight.t > 1)`, { timeout: 30000 }), 'launched');
}

// the rocky home world of system 9: launch, nav to it, the test autopilot flies the whole visit — the dive, the entry
// (heat, plasma), the helm, low flight to a signal, landing next to it (a discovery), take-off, the climb back to orbit
test('costellazioni: a world visited — descent, entry, surface flight, a discovery, take-off to orbit', { skip, timeout: 12 * 60 * 1000 }, async (t) => {
  const sim = await startSim({ seed: { [SAVE]: { ...SAVE0, sys: 9 } } });
  const browser = await launchBrowser({ gpu: process.env.E2E_GPU === '1', args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await openGame(browser, sim, { flags: { __czAutopilot: true, __czAutoLand: true, __czEncounter: false, __czGod: true, __czTimeScale: 4, __czClock: 1000 } });
  await launchFlight(page);
  const credits0 = await page.eval('window.__cz.game.run.credits');
  await page.eval(`(() => { const F = window.__cz.game.flight; F.undock = null; F.nav = F.pois.findIndex((p) => p.kind === 'planet' && p.i === 0); return F.nav; })()`);
  const seen = new Set(); let heat = 0, shot = 0, worldDrawn = false;
  const t0 = Date.now();
  while (Date.now() - t0 < 9 * 60 * 1000) {
    const s = await surfState(page);
    if (s) { seen.add(s.st); heat = Math.max(heat, s.heat); if (s.world === 0) worldDrawn = true; }
    if (s && s.st === SURF.ENTRY && shot === 0) { shot++; await frames(page); await page.screenshot(join(OUT, 'world-entry.png')); }
    if (s && s.st === SURF.FLIGHT && s.agl < 400 && shot === 1) { shot++; await frames(page); await page.screenshot(join(OUT, 'world-surface.png')); }
    if (s && s.st === SURF.LANDED && shot === 2) { shot++; await sleep(500); await frames(page); await page.screenshot(join(OUT, 'world-landed.png')); }
    if (s && s.left) break;
    await sleep(150);
  }
  for (const [k, v] of Object.entries(SURF)) assert.ok(seen.has(v), `the visit passed through ${k} (seen ${[...seen].sort().join(',')})`);
  assert.ok(heat > 0.3, `the entry heated the hull (${(heat * 100).toFixed(0)}%)`);
  assert.ok(worldDrawn, 'the renderer drew the world terrain');
  const fin = await surfState(page);
  assert.ok(fin && fin.st === SURF.SPACE && fin.found >= 1, 'back in space with a site found: ' + JSON.stringify(fin));
  // the discovery: paid into the shared run, remembered in the web save next to it (never in the shared struct)
  let web = null; for (let i = 0; i < 40; i++) { web = await readJson(sim, WEB); if (web && web.found && Object.values(web.found).some((m) => m)) break; await sleep(300); }
  assert.ok(web && web.found && Object.values(web.found).some((m) => m), 'the web save on the card holds the found site: ' + JSON.stringify(web && web.found));
  const credits = await page.eval('window.__cz.game.run.credits');
  assert.ok(credits > credits0, `the discovery paid (${credits - credits0} cr)`);
  const disk = await readJson(sim, SAVE);
  assert.deepEqual(Object.keys(disk).sort(), Object.keys(SAVE0).sort(), 'the shared save keeps its contract');
  assert.deepEqual(gameErrors(page), [], 'no errors');
});

// a phone: the touch controls are there in flight, and over a world SCN pings the scanner and L lands the ship
test('costellazioni: phone viewport — touch controls on a world (scanner, landing)', { skip, timeout: 5 * 60 * 1000 }, async (t) => {
  const sim = await startSim({ seed: { [SAVE]: { ...SAVE0, sys: 9 } } });
  const browser = await launchBrowser({ gpu: process.env.E2E_GPU === '1', args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await browser.newPage();
  await page.setViewport(390, 844, true);
  await page.send('Emulation.setTouchEmulationEnabled', { enabled: true, maxTouchPoints: 5 });
  await page.initScript(`Object.assign(window, ${JSON.stringify({ __czEncounter: false, __czGod: true, __czClock: 1000 })});`);
  await bootShell(page, sim, { lang: 'en', wait: false });
  await page.goto(`${sim.origin}/apps/games/?host=constellations`);
  assert.ok(await page.waitFor(`document.getElementById('startBtn') && !document.getElementById('startBtn').disabled && document.getElementById('startBtn').offsetWidth > 0`, { timeout: 45000 }), 'waiting room');
  await page.eval(`document.getElementById('startBtn').click(), true`);
  assert.ok(await page.waitFor(`(() => { const m = document.querySelector('[data-modal]'), h = document.querySelector('[data-hub]'); return (m && m.style.display === 'flex') || (h && h.style.display === 'block'); })()`, { timeout: 45000 }), 'the game opened');
  await launchFlight(page);
  assert.ok(await page.eval(`document.querySelector('.sh-touch').classList.contains('on') && document.querySelector('.sh-stick').getBoundingClientRect().width > 60`), 'the touch stick is shown');
  const btns = await page.eval(`[...document.querySelectorAll('.sh-tb')].map((b) => b.textContent)`);
  for (const b of ['FIRE', 'L', 'SCN', 'MAP', 'NAV']) assert.ok(btns.includes(b), `touch button ${b} (${btns.join(',')})`);
  const inside = await page.eval(`[...document.querySelectorAll('.sh-tb')].every((b) => { const r = b.getBoundingClientRect(); return r.left >= 0 && r.top >= 0 && r.right <= innerWidth && r.bottom <= innerHeight; })`);
  assert.ok(inside, 'every touch button is on the screen');
  await page.eval(`window.__cz.dev.overWorld(0, 150, 45, 0, 60), true`);
  assert.ok(await page.waitFor(`window.__cz.game.flight.surf.st === 2 && window.__cz.r3d.worldIdx === 0`, { timeout: 15000 }), 'low over the world');
  await sleep(1500);
  const tap = (label) => page.eval(`(() => { const b = [...document.querySelectorAll('.sh-tb')].find((x) => x.textContent === ${JSON.stringify(label)}); const r = b.getBoundingClientRect(); for (const ty of ['pointerdown', 'pointerup']) b.dispatchEvent(new PointerEvent(ty, { bubbles: true, cancelable: true, pointerType: 'touch', pointerId: 7, clientX: r.x + r.width / 2, clientY: r.y + r.height / 2 })); return true; })()`);
  // (the passive 2.6 km ping runs every 8 s; the scanner's own 7 km ping answers once the last one has faded)
  assert.ok(await page.waitFor(`(() => { const F = window.__cz.game.flight; return F.t - F.surf.pingT > 4.5 && F.t - F.surf.pingT < 7; })()`, { timeout: 12000, interval: 100 }));
  await tap('SCN'); await sleep(300);
  assert.ok(await page.eval(`(() => { const F = window.__cz.game.flight; return F.surf.pingR === 7000 && F.t - F.surf.pingT < 1; })()`), 'SCN pings the scanner');
  await tap('L');
  assert.ok(await page.waitFor(`window.__cz.game.flight.surf.st === 3 || window.__cz.game.flight.surf.st === 4`, { timeout: 20000 }), 'L lands the ship');
  assert.ok(await page.waitFor(`window.__cz.game.flight.surf.st === 4`, { timeout: 40000 }), 'touchdown');
  await sleep(800); await frames(page); await page.screenshot(join(OUT, 'world-phone.png'));
  await tap('L');
  assert.ok(await page.waitFor(`window.__cz.game.flight.surf.st === 5 || window.__cz.game.flight.surf.st === 2`, { timeout: 10000 }), 'L takes off again');
  assert.deepEqual(gameErrors(page), [], 'no errors');
});

// M2 leftover: the pad that opens the jump map (Back) closes it too (B, Back, Start) — the held button must not bounce it
test('costellazioni: a gamepad opens and closes the jump map', { skip, timeout: 3 * 60 * 1000 }, async (t) => {
  const sim = await startSim({ seed: { [SAVE]: { ...SAVE0, sys: 6 } } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const pad = `window.__pad = new Array(17).fill(false); navigator.getGamepads = () => [{ id: 'test pad', index: 0, connected: true, mapping: 'standard', axes: [0, 0, 0, 0], buttons: window.__pad.map((p) => ({ pressed: p, touched: p, value: p ? 1 : 0 })) }];`;
  const page = await openGame(browser, sim, { flags: { __czEncounter: false, __czGod: true } });
  await page.eval(`${pad} true`);
  await launchFlight(page);
  const press = async (i) => { await page.eval(`window.__pad[${i}] = true, true`); await sleep(300); await page.eval(`window.__pad[${i}] = false, true`); await sleep(300); };
  const mapOn = `document.querySelector('.sh-map').classList.contains('on')`;
  await press(8);
  assert.ok(await page.waitFor(mapOn, { timeout: 4000 }), 'Back opens the jump map');
  await sleep(500); assert.ok(await page.eval(mapOn), 'and it stays open after the button is let go');
  await press(1);
  assert.ok(await page.waitFor(`!${mapOn}`, { timeout: 4000 }), 'B closes it');
  await press(8); assert.ok(await page.waitFor(mapOn, { timeout: 4000 }), 'Back opens it again');
  await press(8); assert.ok(await page.waitFor(`!${mapOn}`, { timeout: 4000 }), 'Back closes it too');
  await sleep(500); assert.ok(!(await page.eval(mapOn)), 'and it stays shut');
  assert.deepEqual(gameErrors(page), [], 'no errors');
});

// frame time low over a world (the jungle home world of system 5: forest, rain, clouds) at 1080p, per quality tier, on
// the discrete GPU and on the integrated one of a dual-GPU laptop (Chrome's low-power adapter). `avg`/`p95` are frame
// intervals (vsync-bound); `work` is the frame's cost with the GPU waited for (__czSync), what a slower GPU would show.
test('costellazioni: frame time over a world on the real GPU, per quality tier', { skip: skip || (process.env.E2E_GPU !== '1' && 'set E2E_GPU=1 (needs a GPU)'), timeout: 20 * 60 * 1000 }, async (t) => {
  const sim = await startSim({ seed: { [SAVE]: { ...SAVE0, sys: 5 } } });
  t.after(async () => { await sim.stop(); });
  for (const gpu of (process.env.CZ_GPUS || 'high-performance,low-power').split(',')) for (const q of (process.env.CZ_TIERS || 'auto,low,medium,high').split(',')) {
    const browser = await launchBrowser({ gpu: gpu === 'low-power' ? 'low-power' : true, args: [HOST_RULES] });
    try {
      const page = await browser.newPage();
      await page.setViewport(1920, 1080);
      await page.initScript(`Object.assign(window, ${JSON.stringify({ __czEncounter: false, __czGod: true, __czClock: 1000, __czPowerPref: gpu })}); try { ${q === 'auto' ? "localStorage.removeItem('stelle.quality')" : `localStorage.setItem('stelle.quality', '${q}')`} } catch {}`);
      await bootShell(page, sim, { lang: 'en', wait: false });
      await page.goto(`${sim.origin}/apps/games/?host=constellations`);
      assert.ok(await page.waitFor(`document.getElementById('startBtn') && !document.getElementById('startBtn').disabled && document.getElementById('startBtn').offsetWidth > 0`, { timeout: 45000 }));
      await page.eval(`document.getElementById('startBtn').click(), true`);
      assert.ok(await page.waitFor(`(() => { const m = document.querySelector('[data-modal]'), h = document.querySelector('[data-hub]'); return (m && m.style.display === 'flex' && m.querySelector('.cz-btn')) || (h && h.style.display === 'block'); })()`, { timeout: 45000 }));
      await launchFlight(page);
      await page.eval(`window.__cz.dev.overWorld(0, 100, 40, 300, 150), true`);
      assert.ok(await page.waitFor(`window.__cz.r3d.worldIdx === 0 && window.__cz.r3d.world.coverage() > 0.95`, { timeout: 30000 }), 'the terrain streamed in');
      await sleep(5000);   // flora cells in, the dynamic resolution settled
      const p = await frameStats(page);
      // then the same flight with the GPU waited for each frame: the cost under the vsync cap
      await page.eval('window.__czSync = true'); const w = await frameStats(page, 2500); await page.eval('window.__czSync = false');
      p.work = w.work; p.workP95 = w.workP95;
      console.log(`  frame time over a world [${gpu} / ${q}]`, JSON.stringify(p));
      if (q === 'auto') {
        assert.ok(p.avg < 17.5 && p.p95 < 25, `60 fps over a world on ${p.gpu} (auto = ${p.tier}): avg ${p.avg} ms, p95 ${p.p95} ms`);
        if (gpu !== 'low-power') assert.equal(await page.eval(`JSON.stringify(window.__cz.nanScan(480, 300).filter((x) => x.nan))`), '[]', 'no non-finite pixels in the HDR frame over a world');
      }
      assert.deepEqual(gameErrors(page), [], 'no errors');
    } finally { await browser.close(); }
  }
});
