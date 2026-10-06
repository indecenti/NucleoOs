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

// Settings ▸ AI ▸ AI profile used to be it-or-English only: a German owner read English in the preset
// cards, the four rungs, the capability chips, the app list, the scope legend and the confirm modal.
test('settings: the AI profile panel speaks German, no Italian/English leftovers', { skip }, async (t) => {
  const teacher = { provider: 'anthropic', model: 'claude-test', key: 'sk-ant-test', keys: { anthropic: { key: 'sk-ant-test', model: 'claude-test' } } };
  const sim = await startSim({ seed: { '/data/anima/teacher.json': teacher } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const { STR } = await import('../../apps/settings/www/i18n.js');
  const P = await import('../../apps/settings/www/preset-engine.js');
  const page = await browser.newPage();
  await bootShell(page, sim, { lang: 'de', wait: false });
  await page.goto(`${sim.origin}/apps/settings/`);
  await page.networkIdle({ quiet: 500, timeout: 20000 });
  await page.waitFor(`document.querySelectorAll('.nav-item').length > 4`, { timeout: 45000 });
  await page.eval(`document.querySelectorAll('.nav-item')[4].click(), true`);          // TABS: …, network, ai
  assert.ok(await page.waitFor(`document.querySelectorAll('#preset-strip .pcard').length === ${P.PRESETS.length}
    && document.querySelectorAll('#apps-list .arow').length === ${P.APP_MAP.length} && document.querySelectorAll('#scope-legend span').length === 3`, { timeout: 45000 }), 'the panel rendered');
  await page.eval(`document.querySelector('#apps-det').open = true, document.querySelector('#preset-strip .pcard[data-id="balanced"]').click(), true`);
  assert.ok(await page.waitFor(`document.getElementById('preset-modal').classList.contains('show')`, { timeout: 10000 }), 'the confirm modal opened');
  const text = await page.eval(`document.getElementById('ai-presets-sect').innerText + ' | ' + document.getElementById('preset-modal').innerText`);

  // Must read: German strings of every part of the panel.
  for (const s of [STR.de.apScopeSd, STR.de.apRungGpu, STR.de.apCapImages, STR.de.apTestNow, STR.de.apWillUse.trim(), P.APP_MAP[3].de, P.PRESETS[2].de])
    assert.ok(text.includes(s), `missing German text: ${s}`);
  // Must not read: any Italian or English string of the panel that differs from the German one.
  const foreign = [];
  const check = (o) => { for (const l of ['it', 'en']) { const s = String(o[l] || '').trim(); if (s.length >= 5 && s !== String(o.de).trim() && text.includes(s)) foreign.push(`${l}: ${s}`); } };
  for (const k of Object.keys(STR.en).filter((x) => x.startsWith('ap'))) check({ it: STR.it[k], en: STR.en[k], de: STR.de[k] });
  for (const p of P.PRESETS) { check(p); check(p.intent); }
  for (const a of P.APP_MAP) check(a);
  assert.deepEqual(foreign, [], 'Italian/English leftovers in the German AI profile panel');
});
