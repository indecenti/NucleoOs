// Browser E2E — Settings ▸ Personalization wallpaper gallery against the simulator. Guards two bugs found in
// review: a trailing comment that swallowed the .filter() callback (still valid JS by ASI, so `imgs` became
// the filter FUNCTION and the gallery fell into "device unreachable"), and a 404 for a card without
// /data/Pictures read as an unreachable device instead of "no wallpapers yet".
import test from 'node:test';
import assert from 'node:assert/strict';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const PNG = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==', 'base64').toString('latin1');

async function openPersonalization(browser, sim) {
  const page = await browser.newPage();
  await bootShell(page, sim, { lang: 'en', wait: false });
  await page.goto(`${sim.origin}/apps/settings/`);
  await page.networkIdle({ quiet: 500, timeout: 20000 });
  // TABS order in apps/settings/www/index.html: control, display, personalization, …
  await page.waitFor(`document.querySelectorAll('.nav-item').length > 2`, { timeout: 45000 });
  await page.eval(`document.querySelectorAll('.nav-item')[2].click(), true`);
  return page;
}

test('settings: the wallpaper gallery lists the pictures on the SD, names as text', { skip }, async (t) => {
  const sim = await startSim({ seed: { '/data/Pictures/beach.png': PNG, '/data/Pictures/b&amp;w.png': PNG, '/data/Pictures/notes.txt': 'x' } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await openPersonalization(browser, sim);
  const n = await page.waitFor(`document.querySelectorAll('#wp-container .wp-item').length`, { timeout: 45000 });
  assert.equal(n, 2, 'one tile per picture, the .txt is not a wallpaper');
  const names = await page.eval(`[...document.querySelectorAll('#wp-container .wp-name')].map((x) => x.textContent).sort()`);
  assert.deepEqual(names, ['b&amp;w.png', 'beach.png'], 'a file name is text: &amp; stays literal, never decoded as markup');
});

test('settings: a card without /data/Pictures shows "no wallpapers", not "device unreachable"', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await openPersonalization(browser, sim);
  // The placeholder ("Open this tab to load wallpapers…") ends in an ellipsis; the settled hint does not.
  const hint = await page.waitFor(`(() => { const h = document.querySelector('#wp-container .hint'); return h && !h.textContent.includes('…') && h.textContent; })()`, { timeout: 45000 });
  assert.ok(hint, 'the gallery settled');
  assert.doesNotMatch(hint, /unreachable|offline|raggiungibile/i, 'an empty folder is not an offline device: ' + hint);
});
