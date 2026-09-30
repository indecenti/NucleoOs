// Browser E2E — the shell against a device that misbehaves the way a real Cardputer does: writes
// refused under heap pressure (500 "oom"), the arbiter answering 503 at boot, the device dropping off
// the network. Driven through the simulator's fault switches (POST /api/_sim/fault | offline).
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim, REPO } from './sim.mjs';
import { bootShell, waitDesktop } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const cat = (lang) => JSON.parse(readFileSync(join(REPO, 'web', 'shell', 'i18n', `shell.${lang}.json`), 'utf8'));
const toasts = `[...document.querySelectorAll('#toast-container > *')].map((x) => x.textContent)`;
const skip = !findChrome() && 'no Chrome/Edge installed';

test('a refused config write is announced, kept, and saved once the device recovers', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await browser.newPage();
  const lang = 'de';                                  // non-default language on purpose: the message must be localised
  assert.ok(await bootShell(page, sim, { lang }));
  await sim.control('/api/_sim/fault', { route: '/api/fs/write', status: 500 });   // until cleared
  await page.eval(`window.postMessage({ type: 'set-wallpaper', path: '/data/Pictures/e2e-wall.png' }, location.origin)`);
  const failed = await page.waitFor(`${toasts}.some((x) => x.includes(${JSON.stringify(cat(lang).toast_save_failed)}))`, { timeout: 12000 });
  assert.ok(failed, 'the user is told the save failed (in German)');
  const before = await sim.readSd('/system/config/ui-state.json').catch(() => '');
  assert.ok(!before.includes('e2e-wall.png'), 'nothing was written while the device refused');
  await sim.control('/api/_sim/fault', { clear: true });
  const recovered = await page.waitFor(`${toasts}.some((x) => x.includes(${JSON.stringify(cat(lang).toast_save_recovered)}))`, { timeout: 25000 });
  assert.ok(recovered, 'the user is told the change was saved after all');
  const after = await sim.readSd('/system/config/ui-state.json');
  assert.ok(after.includes('e2e-wall.png'), 'the kept change reached the SD once the device recovered');
});

test('boot survives /api/apps answering 503 (arbiter busy) and still shows the REAL app list', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await browser.newPage();
  await sim.control('/api/_sim/fault', { route: '/api/apps', status: 503, times: 2 });
  await bootShell(page, sim, { lang: 'en', wait: false });
  assert.ok(await waitDesktop(page, 40000), 'desktop never came up');
  const n = await page.eval(`document.querySelectorAll('#sm-all .sm-row').length`);
  assert.ok(n >= 40, `Start lists ${n} apps — the shell fell back to its offline mock instead of retrying`);
});

test('the device dropping off the network is shown at once, clearly, and recovers by itself', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await browser.newPage();
  const lang = 'fr';
  assert.ok(await bootShell(page, sim, { lang }));
  await page.waitFor(`(document.querySelector('#tray-ws .ws-dot') || {}).className === 'ws-dot connected'`, { timeout: 10000 });
  const t0 = Date.now();
  await sim.control('/api/_sim/offline', { on: true });
  const shown = await page.waitFor(`(() => { const b = document.getElementById('link-down'); return b && b.classList.contains('show') ? b.textContent : null; })()`, { timeout: 15000 });
  const detectMs = Date.now() - t0;
  assert.ok(shown, 'no "device unreachable" banner');
  assert.ok(shown.includes(cat(lang).link_down_title) && shown.includes(cat(lang).link_retry), `banner not in French: ${shown}`);
  assert.ok(detectMs < 10000, `took ${detectMs} ms to tell the user the device is gone`);
  t.diagnostic(`offline detected in ${detectMs} ms`);
  await sim.control('/api/_sim/offline', { on: false });
  await page.eval(`document.querySelector('#link-down .ld-btn').click()`);
  const back = await page.waitFor(`!document.getElementById('link-down').classList.contains('show') && ${toasts}.some((x) => x.includes(${JSON.stringify(cat(lang).link_back)}))`, { timeout: 10000 });
  assert.ok(back, '"Retry now" did not bring the link back');
  assert.ok(await page.waitFor(`(document.querySelector('#tray-ws .ws-dot') || {}).className === 'ws-dot connected'`, { timeout: 15000 }), 'the live channel did not re-attach');
});
