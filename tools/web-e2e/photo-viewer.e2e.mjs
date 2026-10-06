// Browser E2E — Photo Viewer against the simulator: the gallery lists ONLY the pictures in /data/Pictures
// (names as text), ?path= opens a picture and the arrows walk the list, "Set as wallpaper" hands the shell
// the picture on screen, and a card without the folder reads "no pictures" — not "device unreachable".
import test from 'node:test';
import assert from 'node:assert/strict';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const PNG = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==', 'base64').toString('latin1');
const EN = JSON.parse((await import('node:fs')).readFileSync(new URL('../../apps/photo-viewer/www/i18n.en.json', import.meta.url), 'utf8'));

async function open(browser, sim, query = '') {
  const page = await browser.newPage();
  await bootShell(page, sim, { lang: 'en', wait: false });
  await page.goto(`${sim.origin}/apps/photo-viewer/${query}`);
  return page;
}
// The path the <img> is showing (decoded from /api/fs/read?path=…), '' when none.
const SHOWN = `(() => { const s = document.getElementById('img').getAttribute('src'); return s ? new URL(s, location.href).searchParams.get('path') : ''; })()`;
const CHIPS = `[...document.querySelectorAll('#bar .chip')].map((c) => c.textContent)`;
const ACTIVE = `[...document.querySelectorAll('#bar .chip.active')].map((c) => c.textContent)`;
const key = (k) => `document.dispatchEvent(new KeyboardEvent('keydown', { key: ${JSON.stringify(k)}, bubbles: true })), true`;

test('photo-viewer: lists only the pictures in /data/Pictures, file names as text, first one shown', { skip }, async (t) => {
  const sim = await startSim({ seed: {
    '/data/Pictures/a.png': PNG, '/data/Pictures/B.JPG': PNG, '/data/Pictures/b&amp;w.gif': PNG, '/data/Pictures/c.webp': PNG,
    '/data/Pictures/notes.txt': 'not a picture', '/data/Pictures/album.png/inner.png': PNG,   // a FOLDER named like a picture
    '/data/elsewhere.png': PNG,
  } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);
  assert.ok(await page.waitFor(`document.querySelectorAll('#bar .chip').length >= 4`, { timeout: 45000 }), 'the gallery loaded');
  await page.networkIdle({ quiet: 400, timeout: 10000 });
  const chips = await page.eval(CHIPS);
  assert.deepEqual([...chips].sort(), ['B.JPG', 'a.png', 'b&amp;w.gif', 'c.webp'], 'pictures only (any case, .webp too — Settings offers those as wallpapers), no .txt, no folder, nothing outside the folder; &amp; stays literal text');
  assert.equal(await page.eval(`document.querySelectorAll('#bar .chip *').length`), 0, 'a name is never parsed as markup');
  assert.equal(await page.eval(SHOWN), '/data/Pictures/' + chips[0], 'with no ?path= the first picture is shown');
  assert.deepEqual(await page.eval(ACTIVE), [chips[0]], 'and its chip is the active one');
  assert.equal(await page.eval(`document.getElementById('name').textContent`), '/data/Pictures/' + chips[0], 'the header shows the picture path');
});

test('photo-viewer: ?path= opens that picture, arrows walk the list, wallpaper gets the picture on screen', { skip }, async (t) => {
  const sim = await startSim({ seed: { '/data/Pictures/one.png': PNG, '/data/Pictures/two.png': PNG, '/data/Pictures/three.png': PNG } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim, '?path=' + encodeURIComponent('/data/Pictures/two.png'));
  assert.ok(await page.waitFor(`document.querySelectorAll('#bar .chip').length === 3`, { timeout: 45000 }), 'the gallery loaded');
  await page.networkIdle({ quiet: 400, timeout: 10000 });
  assert.equal(await page.eval(SHOWN), '/data/Pictures/two.png', '?path= wins over "first picture"');
  assert.deepEqual(await page.eval(ACTIVE), ['two.png'], 'the opened picture is highlighted in the list');
  const order = await page.eval(CHIPS);
  const at = order.indexOf('two.png');

  await page.eval(key('ArrowRight'));
  const right = order[Math.min(order.length - 1, at + 1)];
  assert.equal(await page.eval(SHOWN), '/data/Pictures/' + right, 'ArrowRight shows the next picture');
  assert.deepEqual(await page.eval(ACTIVE), [right]);
  await page.eval(key('Home'));
  assert.equal(await page.eval(SHOWN), '/data/Pictures/' + order[0], 'Home → first');
  await page.eval(key('ArrowLeft'));
  assert.equal(await page.eval(SHOWN), '/data/Pictures/' + order[0], 'ArrowLeft on the first picture stays there (no wrap, no blank)');
  await page.eval(key('End'));
  assert.equal(await page.eval(SHOWN), '/data/Pictures/' + order[2], 'End → last');

  // Clicking a chip shows it; "Set as wallpaper" posts THAT path to the shell (parent === window here).
  await page.eval(`window.__wall = []; addEventListener('message', (e) => { if (e.data && e.data.type === 'set-wallpaper') window.__wall.push(e.data); }); true`);
  await page.eval(`[...document.querySelectorAll('#bar .chip')].find((c) => c.textContent === 'one.png').click(), true`);
  assert.equal(await page.eval(SHOWN), '/data/Pictures/one.png');
  await page.eval(`document.getElementById('wall').click(), true`);
  const msgs = await page.waitFor(`window.__wall.length && window.__wall`, { timeout: 5000 });
  assert.deepEqual(msgs, [{ type: 'set-wallpaper', path: '/data/Pictures/one.png' }], 'exactly one wallpaper request, for the picture on screen');
});

test('photo-viewer: no /data/Pictures = "no pictures"; an unreachable device = "unreachable"; a broken file names itself', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });

  const page = await open(browser, sim);
  const msg = `(() => { const m = document.getElementById('msg'); return !m.hidden && m.textContent; })()`;
  const empty = await page.waitFor(msg, { timeout: 45000 });
  assert.equal(empty, EN.dir_empty.replace('{dir}', '/data/Pictures'), 'a 404 folder is an empty gallery, not an offline device');
  assert.equal(await page.eval(`document.querySelectorAll('#bar .chip').length`), 0);
  await page.eval(`window.__wall = 0; addEventListener('message', (e) => { if (e.data && e.data.type === 'set-wallpaper') window.__wall++; }); document.getElementById('wall').click(), true`);
  await new Promise((r) => setTimeout(r, 300));
  assert.equal(await page.eval(`window.__wall`), 0, 'nothing on screen → no wallpaper request');

  await sim.control('/api/_sim/fault', { route: '/api/fs/list', status: 503 });
  const p2 = await open(browser, sim);
  const down = await p2.waitFor(msg, { timeout: 45000 });
  await sim.control('/api/_sim/fault', { clear: true });
  assert.equal(down, EN.dir_failed.replace('{dir}', '/data/Pictures'), 'a failed listing IS reported as a reachability problem');

  const p3 = await open(browser, sim, '?path=' + encodeURIComponent('/data/Pictures/gone.png'));
  const broken = await p3.waitFor(msg, { timeout: 45000 });
  assert.equal(broken, EN.img_failed.replace('{path}', '/data/Pictures/gone.png'), 'a picture that fails to load says which one');
  assert.equal(await p3.eval(`document.getElementById('img').hidden`), true, 'and no broken-image icon is left on screen');
});
