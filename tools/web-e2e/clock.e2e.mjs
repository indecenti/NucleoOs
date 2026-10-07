// Browser E2E — Clock against the simulator: the 12/24-hour, seconds and analog preferences apply at once,
// persist to /system/config/clock.json and survive a reload; a FAILED read of that file never lets the next
// toggle overwrite the saved preferences with defaults; a malformed locale in settings.json never freezes the
// clock; the timer counts down, pauses without losing time and fires exactly once; stopwatch laps keep their
// real numbers past the 10 rows shown.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const rd = (p) => JSON.parse(readFileSync(new URL(p, import.meta.url), 'utf8'));
const cat = (lang) => ({ ...rd(`../../web/shell/i18n/core.${lang}.json`), ...rd(`../../apps/clock/www/i18n.${lang}.json`) });
const EN = cat('en');
const CFG = '/system/config/clock.json';
const SETTINGS = rd('../../tools/sd-sim/system/config/settings.json');

async function open(browser, sim, { init } = {}) {
  const page = await browser.newPage();
  if (init) await page.initScript(init);
  await bootShell(page, sim, { lang: 'en', wait: false });
  await page.goto(`${sim.origin}/apps/clock/`);
  return page;
}
const TIME = `document.getElementById('time').textContent`;
const SAVE_MSG = `document.getElementById('cfgsave').textContent`;
const click = (id) => `document.getElementById(${JSON.stringify(id)}).click(), true`;
const ready = (page) => page.waitFor(`/^\\d{1,2}:\\d{2}/.test(${TIME}) && document.getElementById('cfgsave') !== null`, { timeout: 45000 });

test('clock: 12/24-hour, seconds and analog prefs apply at once, persist to clock.json and survive a reload', { skip }, async (t) => {
  const sim = await startSim({ seed: { [CFG]: { format24: false, showSeconds: false, analog: false } } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);
  assert.ok(await ready(page), 'the clock is ticking');
  await page.networkIdle({ quiet: 400, timeout: 10000 });

  assert.match(await page.eval(TIME), /^\d{1,2}:\d{2} (AM|PM)$/, 'saved 12-hour, no seconds: "h:mm AM/PM"');
  assert.equal(await page.eval(`document.querySelectorAll('#time *').length`), 1, 'only the AM/PM suffix is markup');
  assert.equal(await page.eval(`document.getElementById('analog-wrap').style.display`), 'none', 'saved analog:false hides the dial');
  assert.deepEqual(await page.eval(`[o24.checked, osec.checked, oanal.checked]`), [false, false, false], 'the settings card mirrors the saved file');

  await page.eval(click('o24'));
  assert.ok(await page.waitFor(`${SAVE_MSG} === ${JSON.stringify(EN.save_ok)}`, { timeout: 10000 }), 'the change is saved');
  assert.match(await page.eval(TIME), /^\d{2}:\d{2}$/, '24-hour applies at once: "HH:mm"');
  await page.eval(click('osec'));
  assert.ok(await page.waitFor(`/^\\d{2}:\\d{2}:\\d{2}$/.test(${TIME})`, { timeout: 5000 }), 'seconds appear at once');
  await page.waitFor(`${SAVE_MSG} === ${JSON.stringify(EN.save_ok)}`, { timeout: 10000 });
  await page.networkIdle({ quiet: 400, timeout: 10000 });
  assert.deepEqual(JSON.parse(await sim.readSd(CFG)), { format24: true, showSeconds: true, analog: false }, 'the file holds exactly what is on screen');

  await page.goto(`${sim.origin}/apps/clock/`);
  assert.ok(await page.waitFor(`/^\\d{2}:\\d{2}:\\d{2}$/.test(${TIME})`, { timeout: 45000 }), 'after a reload the clock is still 24-hour with seconds');
  assert.equal(await page.eval(`document.getElementById('analog-wrap').style.display`), 'none');
});

test('clock: a failed read of clock.json never lets the next toggle overwrite the saved prefs', { skip }, async (t) => {
  const saved = { format24: false, showSeconds: false, analog: false };
  const sim = await startSim({ seed: { [CFG]: saved } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const before = await sim.readSd(CFG);

  await sim.control('/api/_sim/fault', { route: '/api/fs/read', status: 503 });
  const page = await open(browser, sim);
  assert.ok(await ready(page), 'the clock still runs with the device unreadable');
  await page.networkIdle({ quiet: 400, timeout: 10000 });

  await t.test('read still failing: the toggle is not written and the card says why', async () => {
    await page.eval(click('osec'));
    assert.ok(await page.waitFor(`${SAVE_MSG} === ${JSON.stringify(EN.save_unread)}`, { timeout: 10000 }), 'the user is told the change was not saved');
    await sleep(500);
    assert.equal(await sim.readSd(CFG), before, 'clock.json was overwritten with defaults + one toggle');
  });

  await t.test('device back: the next toggle re-reads and changes ONLY that field', async () => {
    await sim.control('/api/_sim/fault', { clear: true });
    await page.eval(click('oanal'));
    assert.ok(await page.waitFor(`${SAVE_MSG} === ${JSON.stringify(EN.save_ok)}`, { timeout: 10000 }), 'saved');
    await page.networkIdle({ quiet: 400, timeout: 10000 });
    const now = JSON.parse(await sim.readSd(CFG));
    assert.equal(now.format24, false, 'the saved 12-hour choice survived');
    assert.equal(now.showSeconds, false, 'the toggle that was never saved did not sneak in');
    assert.equal(typeof now.analog, 'boolean');
    assert.deepEqual(await page.eval(`[o24.checked, osec.checked, oanal.checked]`), [now.format24, now.showSeconds, now.analog], 'the card shows what the file holds');
  });
});

test('clock: a malformed locale in settings.json never freezes the clock; a valid one formats the date', { skip }, async (t) => {
  const bad = structuredClone(SETTINGS); bad.device.locale = 'it_IT';   // Settings takes free text: an underscore typo
  const sim = await startSim({ seed: { '/system/config/settings.json': bad } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);
  assert.ok(await ready(page), 'time shown');
  await page.networkIdle({ quiet: 400, timeout: 10000 });
  await sleep(600);
  const date = await page.eval(`document.getElementById('date').textContent`);
  assert.ok(date && date !== '—' && /\d/.test(date), `the date line is painted despite the bad locale (got "${date}")`);
  const s1 = await page.eval(`document.getElementById('analog').innerHTML`);
  await sleep(1300);
  assert.notEqual(await page.eval(`document.getElementById('analog').innerHTML`), s1, 'the analog second hand keeps moving');

  const good = structuredClone(SETTINGS); good.device.locale = 'de-DE';
  await sim.control('/api/_sim/fault', { clear: true });
  const sim2 = await startSim({ seed: { '/system/config/settings.json': good } });
  t.after(() => sim2.stop());
  const p2 = await open(browser, sim2);
  assert.ok(await ready(p2));
  await p2.networkIdle({ quiet: 400, timeout: 10000 });
  const de = new Date().toLocaleDateString('de-DE', { weekday: 'long' });
  assert.ok(await p2.waitFor(`document.getElementById('date').textContent.includes(${JSON.stringify(de)})`, { timeout: 5000 }), 'the configured locale names the weekday (' + de + ')');
});

test('clock: the timer counts down, pauses without losing time and fires exactly once', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  // Count the beeps (each one is a new AudioContext).
  const page = await open(browser, sim, { init: `window.__beeps = 0; (() => { const A = window.AudioContext; if (A) window.AudioContext = function () { window.__beeps++; return new A(); }; })();` });
  assert.ok(await ready(page));
  await page.eval(`document.querySelector('.tab[data-tab="timer"]').click(), true`);
  const DISP = `document.getElementById('timer-disp').textContent`;
  assert.equal(await page.eval(DISP), '05:00', 'default 5 minutes');
  for (let i = 0; i < 4; i++) await page.eval(click('t-m1'));
  for (let i = 0; i < 6; i++) await page.eval(click('t-s10'));
  assert.equal(await page.eval(DISP), '00:10', 'never below the 10 s floor');

  await page.eval(click('te'));
  await sleep(2300);
  await page.eval(click('te'));                                   // pause
  const paused = await page.eval(DISP);
  assert.ok(['00:07', '00:08'].includes(paused), 'about 2 s elapsed: ' + paused);
  await sleep(1500);
  assert.equal(await page.eval(DISP), paused, 'a paused timer does not move');
  assert.equal(await page.eval(`document.getElementById('te').textContent`), EN.start, 'the button offers Start again');

  await page.eval(click('te'));                                   // resume
  assert.ok(await page.waitFor(`document.getElementById('timer-done').textContent === ${JSON.stringify(EN.timer_done)}`, { timeout: 15000 }), 'the timer fires');
  assert.equal(await page.eval(DISP), '00:00');
  await sleep(1500);
  assert.equal(await page.eval(`window.__beeps`), 1, 'one alarm, not one per completion path');
  assert.equal(await page.eval(`document.getElementById('te').textContent`), EN.start);
});

test('clock: stopwatch laps keep their real numbers past the 10 rows shown', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);
  assert.ok(await ready(page));
  await page.eval(`document.querySelector('.tab[data-tab="sw"]').click(), true`);
  await page.eval(click('se'));
  await sleep(250);
  for (let i = 0; i < 12; i++) { await page.eval(click('sl')); await sleep(20); }
  const rows = await page.eval(`[...document.querySelectorAll('#laps .lap-num')].map((e) => e.textContent)`);
  assert.equal(rows.length, 10, 'the list is bounded to the last 10 laps');
  assert.equal(rows[0], `${EN.lap} 12`, 'the newest row is lap 12, not "lap 10"');
  assert.equal(rows[9], `${EN.lap} 3`, 'the oldest row shown is lap 3');
  await page.eval(click('sr'));
  assert.equal(await page.eval(`document.querySelectorAll('#laps .lap-row').length`), 0, 'reset clears the laps');
  await page.eval(click('se')); await sleep(250); await page.eval(click('sl'));
  assert.equal(await page.eval(`document.querySelector('#laps .lap-num').textContent`), `${EN.lap} 1`, 'after a reset numbering starts again at 1');
});
