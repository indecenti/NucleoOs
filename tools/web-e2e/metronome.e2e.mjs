// Browser E2E — Metronome UI against the simulator (the DSP / scheduler math is in
// tools/metronome-engine.test.mjs). What the user drives: the BPM buttons, slider and arrow keys (clamped
// to 30–260), tap tempo on the T key, the beats-per-bar dots, Space = start/stop with the label following
// the OS language, and the settings surviving a reload.
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
const cat = (lang) => ({ ...rd(`../../web/shell/i18n/core.${lang}.json`), ...rd(`../../apps/metronome/www/i18n.${lang}.json`) });

const BPM = `+document.getElementById('bpm').textContent`;
const click = (id) => `document.getElementById(${JSON.stringify(id)}).click(), true`;
const key = (k, code) => `document.dispatchEvent(new KeyboardEvent('keydown', { key: ${JSON.stringify(k)}, code: ${JSON.stringify(code || '')}, bubbles: true })), true`;

async function open(page, sim) {
  await page.goto(`${sim.origin}/apps/metronome/`);
  await page.waitFor(`document.getElementById('dots') && document.getElementById('dots').children.length > 0`, { timeout: 45000 });
}

test('metronome: BPM controls clamp to 30–260, tap tempo, meter dots, and settings survive a reload', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await browser.newPage();
  await bootShell(page, sim, { lang: 'en', wait: false });
  await open(page, sim);

  assert.equal(await page.eval(BPM), 120, 'default 120 BPM');
  await page.eval(click('plus5')); await page.eval(click('plus1'));
  assert.equal(await page.eval(BPM), 126);
  await page.eval(key('ArrowDown')); await page.eval(key('ArrowLeft'));
  assert.equal(await page.eval(BPM), 120, 'keys: ↓ −5, ← −1');
  for (let i = 0; i < 40; i++) await page.eval(click('plus5'));
  assert.equal(await page.eval(BPM), 260, 'clamped at 260');
  for (let i = 0; i < 60; i++) await page.eval(key('ArrowDown'));
  assert.equal(await page.eval(BPM), 30, 'clamped at 30');
  await page.eval(`(() => { const s = document.getElementById('slider'); s.value = 97; s.dispatchEvent(new Event('input')); return true; })()`);
  assert.equal(await page.eval(BPM), 97, 'slider');
  assert.equal(await page.eval(`document.getElementById('slider').value`), '97');

  // Tap tempo: four taps 500 ms apart → 120 (± the event-loop jitter of a test machine).
  for (let i = 0; i < 4; i++) { await page.eval(key('t')); if (i < 3) await sleep(500); }
  const tapped = await page.eval(BPM);
  assert.ok(Math.abs(tapped - 120) <= 4, 'tap tempo ≈ 120: ' + tapped);

  await page.eval(`(() => { const b = document.getElementById('beats'); b.value = '7'; b.dispatchEvent(new Event('change')); return true; })()`);
  assert.equal(await page.eval(`document.getElementById('dots').children.length`), 7, '7 beats → 7 dots');
  await page.eval(`(() => { const s = document.getElementById('subdiv'); s.value = '3'; s.dispatchEvent(new Event('change')); return true; })()`);

  const en = cat('en');
  await page.eval(key(' ', 'Space'));
  assert.equal(await page.eval(`document.getElementById('startstop').textContent`), en.stop, 'Space starts');
  await sleep(600);
  assert.ok(await page.eval(`document.querySelectorAll('#dots .dot').length === 7`), 'still 7 dots while running');
  await page.eval(key(' ', 'Space'));
  assert.equal(await page.eval(`document.getElementById('startstop').textContent`), en.start, 'Space stops');

  // Reload: tempo, meter and subdivision are remembered.
  await open(page, sim);
  assert.equal(await page.eval(BPM), tapped, 'tempo remembered');
  assert.equal(await page.eval(`document.getElementById('beats').value`), '7');
  assert.equal(await page.eval(`document.getElementById('subdiv').value`), '3');
  assert.equal(await page.eval(`document.getElementById('dots').children.length`), 7);
});

test('metronome: the labels follow the OS language', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  for (const lang of LANGS) {
    const c = cat(lang);
    const page = await browser.newPage();
    await bootShell(page, sim, { lang, wait: false });
    await open(page, sim);
    assert.ok(await page.waitFor(`document.getElementById('startstop').textContent === ${JSON.stringify(c.start)}`, { timeout: 10000 }), lang + ': start');
    assert.equal(await page.eval(`document.getElementById('tap').textContent`), c.tap, lang + ': tap');
    assert.equal(await page.eval(`document.getElementById('tabTun').textContent`), c['tab.tuner'], lang + ': tuner tab');
    await page.eval(key(' ', 'Space'));
    assert.equal(await page.eval(`document.getElementById('startstop').textContent`), c.stop, lang + ': stop');
    await page.eval(key(' ', 'Space'));
  }
});
