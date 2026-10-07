// Browser E2E — Arcade against the simulator.
// Library (index.html):
//   - the shelf renders in all 5 languages: section titles, per-system game counts (plural forms), the
//     romset warning of arcade systems; ROM file names are TEXT; the BIOS is never listed as a game;
//     a game with a save on the card carries the save badge;
//   - a library the device cannot read says so — it is not "add ROMs to the SD card";
//   - favorites / "continue playing" persist; launching hands the player the right ROM and save identity.
// Player (player.html), battery saves on the SD:
//   - the SD save is loaded into the emulator at game start and later changes are written back;
//   - when that read FAILS (device busy right after streaming the ROM + core), the emulator runs on its
//     own blank/stale SRAM — and the auto-sync must NOT write that over the save on the card.
// EmulatorJS itself is replaced IN THE PAGE by a stand-in exposing the surface the player uses
// (gameManager FS / getSaveFile / saveSaveFiles / loadSaveFiles, on(), EJS_onGameStart). Any request
// that would leave the simulator is refused.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { writeFile, mkdir, readFile, rm } from 'node:fs/promises';
import { join } from 'node:path';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell, LANGS } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const rd = (p) => JSON.parse(readFileSync(new URL(p, import.meta.url), 'utf8'));
const cat = (lang) => ({ ...rd(`../../web/shell/i18n/core.${lang}.json`), ...rd(`../../apps/arcade/www/i18n.${lang}.json`) });
const plural = (c, key, n, lang) => { const v = c[key]; const f = v[new Intl.PluralRules(lang).select(n)] || v.other; return f.replace('{count}', n); };
const SYSTEMS = rd('../../apps/arcade/www/systems.json').systems;
const EVIL = '&lt;img src=x onerror=window.__pwned=1&gt; Quest';     // valid on every file system
const EVIL_TITLE = EVIL.replace(/[._]+/g, ' ').trim();                   // titles turn . and _ into spaces

const STUB_EJS = (cfg = {}) => `(() => {
  if (!location.pathname.startsWith('/apps/arcade/')) return;
  const CFG = ${JSON.stringify(cfg)};
  const realFetch = window.fetch.bind(window);
  window.__srmReads = 0;
  window.fetch = async (u, o) => {
    const url = new URL(String(u), location.href);
    if (url.host !== location.host) throw new TypeError('blocked: ' + url.host);                  // never leave the simulator
    const path = url.searchParams.get('path') || '';
    if (url.pathname === '/api/fs/read' && path.endsWith('.srm') && (!o || !o.method || o.method === 'GET')) {
      window.__srmReads++;
      if (CFG.failSrm && (CFG.failSrm === 'always' ? !window.__srmHealed : window.__srmReads <= CFG.failSrm))
        return new Response('{"error":"busy"}', { status: 503, headers: { 'Retry-After': '0' } });
    }
    return realFetch(u, o);
  };
  const app = Element.prototype.appendChild;
  Element.prototype.appendChild = function (node) {
    if (node && node.tagName === 'SCRIPT' && String(node.src).endsWith('/emulatorjs/loader.js')) {
      setTimeout(() => {
        const listeners = {}, files = new Map(), dirs = new Set();
        let sram = new TextEncoder().encode(CFG.sram || 'BLANK-SRAM');
        const gm = {
          FS: { analyzePath: (p) => ({ exists: files.has(p) || dirs.has(p) }), mkdir: (p) => dirs.add(p), unlink: (p) => files.delete(p), writeFile: (p, b) => files.set(p, new Uint8Array(b)) },
          getSaveFilePath: () => '/data/saves/game.srm',
          loadSaveFiles: () => { const b = files.get('/data/saves/game.srm'); if (b) { sram = b; window.__emu.loaded.push(new TextDecoder().decode(b)); } },
          saveSaveFiles: () => { (listeners.saveSaveFiles || []).forEach((f) => f()); },     // EmulatorJS fires this synchronously
          getSaveFile: () => sram,
          loadState: () => {},
        };
        window.__emu = { loaded: [], setSram: (s) => { sram = new TextEncoder().encode(s); }, fire: (ev) => (listeners[ev] || []).forEach((f) => f()) };
        window.EJS_emulator = { gameManager: gm, on: (ev, fn) => (listeners[ev] ||= []).push(fn), failedToStart: false };
        document.getElementById('game').appendChild(document.createElement('canvas'));
        if (window.EJS_onGameStart) window.EJS_onGameStart();
      }, 0);
      return node;
    }
    return app.call(this, node);
  };
})();`;

async function card(sim) {
  const w = async (rel, body) => { const p = join(sim.sd, rel); await mkdir(join(p, '..'), { recursive: true }); await writeFile(p, body); };
  await w('data/ROMs/nes/Super_Mario.Bros (USA).nes', Buffer.alloc(64, 1));
  await w(`data/ROMs/nes/${EVIL}.nes`, Buffer.alloc(32, 2));
  await w('data/ROMs/nes/Zelda.nes', Buffer.alloc(48, 3));
  await w('data/ROMs/nes/readme.txt', 'not a rom');
  await w('data/ROMs/nes/saves/Zelda.srm', 'SD-SAVE-ZELDA');
  await w('data/ROMs/neogeo/neogeo.zip', Buffer.alloc(16, 4));                   // the BIOS
  await w('data/ROMs/neogeo/mslug.zip', Buffer.alloc(16, 5));
  await mkdir(join(sim.sd, 'data', 'ROMs', 'gb'), { recursive: true });         // configured but empty
}
async function openLib(browser, sim, lang = 'en') {
  const page = await browser.newPage();
  await page.initScript(STUB_EJS());
  await bootShell(page, sim, { lang, wait: false });
  await page.goto(`${sim.origin}/apps/arcade/`);
  await page.waitFor(`document.querySelector('#main .grid .sys') || document.querySelector('#main .msg')`, { timeout: 45000 });
  return page;
}
const sysCard = (name) => `[...document.querySelectorAll('#main .sys')].find((s) => s.querySelector('.nm').textContent === ${JSON.stringify(name)})`;
const GAMES = `[...document.querySelectorAll('#main .game .gn')].map((g) => g.textContent)`;

test('arcade: the shelf in 5 languages — counts, ROM names as text, BIOS hidden, save badge, romset warning', { skip }, async (t) => {
  const sim = await startSim();
  await card(sim);
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });

  for (const lang of LANGS) {
    const c = cat(lang);
    const page = await openLib(browser, sim, lang);
    assert.deepEqual(await page.eval(`[...document.querySelectorAll('#main .sect')].map((s) => s.textContent)`), [c.systems], lang + ': section title');
    assert.equal(await page.eval(`document.getElementById('sub').textContent`), plural(c, 'games_count', 4, lang), lang + ': total games');
    assert.equal(await page.eval(`${sysCard('NES')}.querySelector('.ct').textContent`), plural(c, 'games_count', 3, lang), lang + ': NES count (no .txt)');
    assert.equal(await page.eval(`${sysCard('Neo Geo')}.querySelector('.ct').textContent`), plural(c, 'games_count', 1, lang), lang + ': the BIOS is not a game');
    assert.equal(await page.eval(`${sysCard('Game Boy')}.querySelector('.ct').textContent`), c.empty, lang + ': empty system');
    assert.equal(await page.eval(`${sysCard('Game Boy')}.classList.contains('empty')`), true);
    assert.equal(await page.eval(`document.querySelectorAll('#main .sys').length`), SYSTEMS.length, lang + ': every configured system is on the shelf');

    // Neo Geo: the romset warning must be in this language (not the English fallback).
    await page.eval(`${sysCard('Neo Geo')}.click(), true`);
    assert.ok(await page.waitFor(`document.querySelector('#main .warnbox b')`, { timeout: 10000 }), lang + ': romset warning shown');
    const warn = await page.eval(`document.querySelector('#main .warnbox b').textContent`);
    const neo = SYSTEMS.find((s) => s.id === 'neogeo').warn;
    assert.ok(warn && (lang === 'en' || warn !== neo.en), `${lang}: romset warning is translated (got "${warn}")`);
    assert.equal(await page.eval(`document.querySelector('#main .warnbox .wn').textContent`), c.romset_note);
    assert.deepEqual(await page.eval(GAMES), ['mslug'], lang + ': Neo Geo lists the game, not the BIOS');
    await page.eval(`document.getElementById('back').click(), true`);
  }

  const page = await openLib(browser, sim, 'en');
  await page.eval(`${sysCard('NES')}.click(), true`);
  assert.ok(await page.waitFor(`document.querySelectorAll('#main .game').length === 3`, { timeout: 10000 }));
  assert.deepEqual([...await page.eval(GAMES)].sort(), [EVIL_TITLE, 'Super Mario Bros (USA)', 'Zelda'].sort(), 'titles from file names');
  assert.equal(await page.eval(`document.querySelectorAll('#main .game .gn *').length`), 0, 'a ROM name is never markup');
  assert.equal(await page.eval(`window.__pwned === undefined`), true);
  assert.deepEqual(await page.eval(`[...document.querySelectorAll('#main .game')].filter((g) => g.querySelector('.sv')).map((g) => g.querySelector('.gn').textContent)`), ['Zelda'], 'only the game with a .srm on the card has the save badge');
});

test('arcade: a library the device cannot read says so — not "add ROMs to the SD card"', { skip }, async (t) => {
  const sim = await startSim();
  await card(sim);
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const en = cat('en');

  await sim.control('/api/_sim/fault', { route: '/api/fs/list', status: 503 });
  const page = await openLib(browser, sim);
  const msg = await page.eval(`(document.querySelector('#main .msg') || {}).textContent || ''`);
  assert.ok(!msg.startsWith(en.add_roms), 'an unreachable device is not an empty card: ' + msg);
  assert.equal(msg, en.list_failed);
  assert.equal(await page.eval(`${sysCard('NES')}.classList.contains('empty')`), false, 'NES is not shown as an empty system');
  assert.equal(await page.eval(`${sysCard('NES')}.querySelector('.ct').textContent`), en.unreadable);

  // Opening a system while the device is still busy says so too; once it answers, the games are there.
  await page.eval(`${sysCard('NES')}.click(), true`);
  assert.ok(await page.waitFor(`document.querySelector('#main .msg') && document.querySelector('#main .msg').textContent === ${JSON.stringify(en.list_failed)}`, { timeout: 10000 }), 'system view: read failure reported');
  await sim.control('/api/_sim/fault', { clear: true });
  await page.eval(`document.getElementById('back').click(), true`);
  assert.ok(await page.waitFor(`${sysCard('NES')} && ${sysCard('NES')}.querySelector('.ct').textContent === ${JSON.stringify(plural(en, 'games_count', 3, 'en'))}`, { timeout: 10000 }), 'back to normal once the device answers');
  assert.equal(await page.eval(`document.querySelector('#main .msg')`), null);
});

test('arcade: favorites and "continue playing" persist; launching hands the player the ROM and its save identity', { skip }, async (t) => {
  const sim = await startSim();
  await card(sim);
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const en = cat('en');
  const page = await openLib(browser, sim);
  await page.eval(`${sysCard('NES')}.click(), true`);
  assert.ok(await page.waitFor(`document.querySelectorAll('#main .game').length === 3`, { timeout: 10000 }));
  await page.eval(`[...document.querySelectorAll('#main .game')].find((g) => g.querySelector('.gn').textContent === 'Zelda').querySelector('.star').click(), true`);
  await page.eval(`[...document.querySelectorAll('#main .game')].find((g) => g.querySelector('.gn').textContent === 'Zelda').click(), true`);
  const src = await page.eval(`document.getElementById('pframe').getAttribute('src')`);
  const q = new URL(src, 'http://x/').searchParams;
  assert.equal(q.get('core'), 'nes'); assert.equal(q.get('corefile'), 'fceumm');
  assert.equal(q.get('rom'), '/api/fs/read?path=' + encodeURIComponent('data/ROMs/nes/Zelda.nes'));
  assert.equal(q.get('sys'), 'nes'); assert.equal(q.get('file'), 'Zelda.nes'); assert.equal(q.get('name'), 'Zelda');
  assert.equal(await page.eval(`document.getElementById('play').style.display`), 'block');
  await page.eval(`window.postMessage({ type: 'arcade-exit' }, '*'), true`);
  assert.ok(await page.waitFor(`document.getElementById('play').style.display === 'none'`, { timeout: 5000 }), 'Esc in the player returns to the shelf');

  const p2 = page;                                             // same browser profile: reload the shelf
  await p2.goto(`${sim.origin}/apps/arcade/`);
  await p2.waitFor(`document.querySelector('#main .grid .sys')`, { timeout: 45000 });
  assert.deepEqual(await p2.eval(`[...document.querySelectorAll('#main .sect')].map((s) => s.textContent)`), [en.continue_playing, en.favorites, en.systems]);
  assert.equal(await p2.eval(`document.querySelector('#main .hero .t').textContent`), 'Zelda');
  assert.deepEqual(await p2.eval(`[...document.querySelectorAll('#main .fav .t')].map((f) => f.textContent)`), ['Zelda']);
});

// ── the player's battery saves ───────────────────────────────────────────────────────────────────
const SRM = 'data/ROMs/nes/saves/Zelda.srm';
async function openPlayer(browser, sim, cfg) {
  const page = await browser.newPage();
  await page.initScript(STUB_EJS(cfg));
  await bootShell(page, sim, { lang: 'en', wait: false });
  const rom = '/api/fs/read?path=' + encodeURIComponent('data/ROMs/nes/Zelda.nes');
  await page.goto(`${sim.origin}/apps/arcade/player.html?core=nes&rom=${encodeURIComponent(rom)}&name=Zelda&corefile=fceumm&sys=nes&file=Zelda.nes`);
  assert.ok(await page.waitFor(`window.__emu && document.querySelector('#game canvas')`, { timeout: 45000 }), 'the (stand-in) emulator started');
  return page;
}
const sdSrm = async (sim) => { try { return await readFile(join(sim.sd, SRM), 'utf8'); } catch { return null; } };

test('arcade player: the SD save is loaded at start, and only real changes are written back', { skip }, async (t) => {
  const sim = await startSim();
  await card(sim);
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await openPlayer(browser, sim, { sram: 'BROWSER-OLD-SRAM' });

  assert.ok(await page.waitFor(`window.__emu.loaded.length > 0`, { timeout: 15000 }), 'the SD save was pushed into the emulator');
  assert.deepEqual(await page.eval(`window.__emu.loaded`), ['SD-SAVE-ZELDA'], 'the card wins over the browser copy');
  await page.eval(`window.__emu.fire('saveSaveFiles'), true`);
  await sleep(1200);
  assert.equal(await sdSrm(sim), 'SD-SAVE-ZELDA', 'unchanged SRAM → no write');
  await page.eval(`window.__emu.setSram('PROGRESS-AFTER-BOSS'); window.__emu.fire('saveSaveFiles'), true`);
  for (let i = 0; i < 40 && (await sdSrm(sim)) !== 'PROGRESS-AFTER-BOSS'; i++) await sleep(150);
  assert.equal(await sdSrm(sim), 'PROGRESS-AFTER-BOSS', 'new progress reaches the card');
});

test('arcade player: no save on the card yet → the first in-game save creates it', { skip }, async (t) => {
  const sim = await startSim();
  await card(sim);
  await rm(join(sim.sd, SRM));                                 // a card with the game but no save yet
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await openPlayer(browser, sim, { sram: 'FIRST-SAVE' });
  await sleep(500);
  await page.eval(`window.__emu.fire('saveSaveFiles'), true`);
  for (let i = 0; i < 40 && (await sdSrm(sim)) !== 'FIRST-SAVE'; i++) await sleep(150);
  assert.equal(await sdSrm(sim), 'FIRST-SAVE');
});

test('arcade player: when the SD save cannot be read at start, auto-save never overwrites it', { skip }, async (t) => {
  const sim = await startSim();
  await card(sim);
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const en = cat('en');
  const page = await openPlayer(browser, sim, { sram: 'BLANK-SRAM', failSrm: 'always' });

  // The game runs on the emulator's own (blank) SRAM; the player plays a bit, the core dumps its save…
  await sleep(500);
  await page.eval(`window.__emu.setSram('BLANK-SRAM-PLUS-2-MINUTES'); window.__emu.fire('saveSaveFiles'), true`);
  await sleep(6000);                                           // past any retry window
  assert.equal(await sdSrm(sim), 'SD-SAVE-ZELDA', 'the save on the card was overwritten by an SRAM that never saw it');
  assert.deepEqual(await page.eval(`window.__emu.loaded`), [], 'nothing was loaded (the read failed)');
  assert.ok(await page.eval(`window.__srmReads`) >= 2, 'the read was retried before giving up');
  assert.ok(await page.waitFor(`document.getElementById('toast').textContent === ${JSON.stringify(en.p_sram_unread)}`, { timeout: 8000 }), 'the player is told');

  // The device answers again, but the emulator is still NOT running the card's save: still no write…
  await page.eval(`window.__srmHealed = true; window.__emu.fire('saveSaveFiles'), true`);
  await sleep(1500);
  assert.equal(await sdSrm(sim), 'SD-SAVE-ZELDA');
  // …and leaving the page (the pagehide beacon) does not write it either.
  await page.goto('about:blank');
  await sleep(1000);
  assert.equal(await sdSrm(sim), 'SD-SAVE-ZELDA', 'the last-chance beacon respects the guard');
});

test('arcade player: a transient busy read at start is retried, then the card save loads normally', { skip }, async (t) => {
  const sim = await startSim();
  await card(sim);
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await openPlayer(browser, sim, { sram: 'BLANK-SRAM', failSrm: 1 });
  assert.ok(await page.waitFor(`window.__emu.loaded.length > 0`, { timeout: 15000 }), 'loaded after a retry');
  assert.deepEqual(await page.eval(`window.__emu.loaded`), ['SD-SAVE-ZELDA']);
  await page.eval(`window.__emu.setSram('NEW-PROGRESS'); window.__emu.fire('saveSaveFiles'), true`);
  for (let i = 0; i < 40 && (await sdSrm(sim)) !== 'NEW-PROGRESS'; i++) await sleep(150);
  assert.equal(await sdSrm(sim), 'NEW-PROGRESS', 'and auto-save works for the rest of the session');
});
