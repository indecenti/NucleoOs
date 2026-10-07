// Browser E2E — Game Center hub (apps/games) against the simulator.
//   - the catalog renders in all 5 languages from the games catalog keys (names / descriptions are text);
//   - rooms other players created (room.json on the SD: name, avatar, host) are listed as TEXT;
//   - a real two-browser Tris match over the hub's own transport: the winner's name — another player's
//     profile, i.e. untrusted — is shown as text in the result banner, and the result is recorded;
//   - a result whose stats cannot be read is NOT recorded (the record on the card is kept) and the player
//     is told (the read-modify-write itself is covered in tools/games-save-safety.test.mjs).
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell, LANGS } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const rd = (p) => JSON.parse(readFileSync(new URL(p, import.meta.url), 'utf8'));
const cat = (lang) => ({ ...rd(`../../web/shell/i18n/core.${lang}.json`), ...rd(`../../apps/games/www/i18n.${lang}.json`) });
const GAMES = rd('../../registry/games.json').games;
const EVIL = '<img src=x onerror="window.__pwned=1">Ada';

// Fail ONLY the stats.json read when window.__failStats is set (everything else — signaling — must work).
const STATS_FAULT = `(() => {
  if (!location.pathname.startsWith('/apps/games/')) return;
  const real = window.fetch.bind(window);
  window.fetch = (u, o) => {
    const s = String(u);
    if (window.__failStats && s.includes('/api/fs/read') && decodeURIComponent(s).includes('/data/play/stats.json'))
      return Promise.resolve(new Response('{"error":"busy"}', { status: 503 }));
    return real(u, o);
  };
})();`;

async function open(browser, sim, { lang = 'en', profile = null, query = '' } = {}) {
  const page = await browser.newPage();
  await page.initScript(STATS_FAULT);
  await bootShell(page, sim, { lang, wait: false });
  if (profile) await page.eval(`localStorage.setItem('play.profile', ${JSON.stringify(JSON.stringify(profile))}), true`);
  await page.goto(`${sim.origin}/apps/games/${query}`);
  await page.waitFor(`document.querySelectorAll('#catalog .card').length > 0 || document.querySelector('#catalog .empty')`, { timeout: 45000 });
  return page;
}

test('games: the catalog renders in all 5 languages, from the catalog keys, as text', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  for (const lang of LANGS) {
    const c = cat(lang);
    const page = await open(browser, sim, { lang });
    const cards = await page.eval(`[...document.querySelectorAll('#catalog .card')].map((k) => ({ name: k.querySelector('h3').textContent, desc: k.querySelector('p').textContent,
      buttons: [...k.querySelectorAll('button')].map((b) => b.textContent) }))`);
    assert.deepEqual(cards.map((k) => k.name), GAMES.map((g) => c[`game_${g.id}_name`]), lang + ': game names');
    assert.deepEqual(cards.map((k) => k.desc), GAMES.map((g) => c[`game_${g.id}_desc`]), lang + ': descriptions');
    cards.forEach((k, i) => {
      assert.equal(k.buttons[0], '▶ ' + c.create, `${lang}: create button of ${GAMES[i].id}`);
      if (GAMES[i].ai) assert.equal(k.buttons[1], '🤖 ' + c.vs_anima, `${lang}: vs-ANIMA button of ${GAMES[i].id}`);
    });
    assert.equal(await page.eval(`document.querySelectorAll('#catalog h3 *, #catalog p *').length`), 0, lang + ': no markup inside names / descriptions');
  }
});

test('games: rooms created by other players are listed as text', { skip }, async (t) => {
  const future = Date.now() + 10 * 60 * 1000;                    // "seen" in the future: never stale during the test
  const room = { id: 'r_evil22', name: EVIL + "'s room", gameId: 'tris', hostPeer: 'p_x', hostName: EVIL, hostAvatar: '<b>A</b>',
    maxSeats: 2, seats: [{ seat: 0, name: EVIL, avatar: '<b>A</b>', ai: false }], spectators: 0, seen: future };
  const sim = await startSim({ seed: { '/data/play/rooms/r_evil22/room.json': room } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const en = cat('en');
  const page = await open(browser, sim);
  assert.ok(await page.waitFor(`document.querySelectorAll('#rooms .room').length === 1`, { timeout: 20000 }), 'the room is listed');
  const row = await page.eval(`(() => { const r = document.querySelector('#rooms .room'); const d = r.querySelectorAll(':scope > div');
    return { avatar: d[0].textContent, name: r.querySelector('.grow > div').textContent, sub: r.querySelector('.grow > div:nth-child(2)').textContent, btn: r.querySelector('button').textContent }; })()`);
  assert.deepEqual(row, { avatar: '<b>A</b>', name: EVIL + "'s room", sub: `${en.game_tris_name} · 1/2`, btn: en.join });
  assert.equal(await page.eval(`document.querySelectorAll('#rooms .room img, #rooms .room b').length`), 0, 'nothing from room.json became markup');
  assert.equal(await page.eval(`window.__pwned === undefined`), true);
});

test('games: a real Tris match between two browsers — the winner\'s name is text, results are recorded, a failed stats read is not', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const en = cat('en');

  // The HOST is the other player, with a hostile profile name; the GUEST is the user under test.
  const host = await open(browser, sim, { profile: { name: EVIL, avatar: '🦊', color: '#22d3ee' }, query: '?host=tris' });
  assert.ok(await host.waitFor(`document.getElementById('roomCode') && document.getElementById('roomCode').textContent.length > 2`, { timeout: 20000 }), 'host is in its room');
  const code = (await host.eval(`document.getElementById('roomCode').textContent`)).replace('#', '');
  const guest = await open(browser, sim, { profile: { name: 'Bea', avatar: '🐼', color: '#e879f9' }, query: '?join=r_' + code });

  const seated = `document.querySelectorAll('#seatCards .seat, #seatCards > div').length >= 2 && !document.getElementById('startBtn').disabled`;
  assert.ok(await host.waitFor(seated, { timeout: 60000 }), 'the guest took the second seat (host can start)');
  assert.ok(await guest.waitFor(`document.getElementById('waitTitle') && document.getElementById('waitTitle').textContent.length > 0`, { timeout: 30000 }), 'guest is in the waiting room');
  // The guest sees the host's name in the roster / seat cards as text.
  assert.equal(await guest.eval(`document.querySelectorAll('#seatCards img, #seats img').length`), 0, 'roster: no markup from a profile name');

  await host.eval(`document.getElementById('startBtn').click(), true`);
  const playing = `document.getElementById('board') && document.getElementById('board').offsetWidth > 0`;
  assert.ok(await host.waitFor(playing, { timeout: 20000 }) && await guest.waitFor(playing, { timeout: 20000 }), 'both are playing');
  await sleep(800);

  // Host (seat 0) takes the top row: keys 7 8 9; the guest answers with 4 and 5. The guest's stats read fails.
  const key = (k) => `window.dispatchEvent(new KeyboardEvent('keydown', { key: '${k}', bubbles: true })), true`;
  await guest.eval(`window.__failStats = true; true`);
  const before = await sim.readSd('/data/play/stats.json').catch(() => null);
  for (const [who, k] of [[host, '7'], [guest, '4'], [host, '8'], [guest, '5'], [host, '9']]) { await who.eval(key(k)); await sleep(700); }

  assert.ok(await guest.waitFor(`document.getElementById('gbResult').textContent.length > 0`, { timeout: 20000 }), 'the guest sees the result');
  const banner = await guest.eval(`document.getElementById('status').textContent`);
  assert.equal(banner, en.seat_wins.replace('{name}', EVIL), 'the winner banner shows the other player\'s name verbatim');
  assert.equal(await guest.eval(`document.querySelectorAll('#status img, #status *:not(span)').length`), 0, 'the winner\'s name is never parsed as markup');
  assert.equal(await guest.eval(`window.__pwned === undefined`), true);
  assert.ok(await host.waitFor(`document.getElementById('status').textContent === ${JSON.stringify(en.you_won)}`, { timeout: 10000 }), 'host: you won');

  // The host's result is recorded; the guest's stats read failed → nothing recorded for it, and it says so.
  assert.ok(await guest.waitFor(`[...document.querySelectorAll('#msgs .msg.sys')].some((m) => m.textContent === ${JSON.stringify(en.stats_not_saved)})`, { timeout: 10000 }), 'the guest is told the result was not saved');
  let stats = null;
  for (let i = 0; i < 30; i++) { try { stats = JSON.parse(await sim.readSd('/data/play/stats.json')); if (stats.tris) break; } catch {} await sleep(200); }
  assert.ok(stats && stats.tris, 'stats recorded: ' + JSON.stringify(stats) + ' (before: ' + before + ')');
  assert.deepEqual({ w: stats.tris.w, l: stats.tris.l, games: stats.tris.games }, { w: 1, l: 0, games: 1 }, 'only the host\'s win was recorded (the guest could not read the record, so it wrote nothing)');
});
