// Browser E2E — Spreadsheet in Private mode (anima-mode.js: nothing leaves this computer). With a cloud key in the
// vault, the device's assistant is asked with mode=off (offline only) and =ANIMA() never escalates to the online
// tier; in Auto the same calls use mode=on — the control that proves the test sees the difference.
import test from 'node:test';
import assert from 'node:assert/strict';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1, MAP * ~NOTFOUND, EXCLUDE localhost, EXCLUDE 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const VAULT = { provider: 'openai', key: 'gsk_test_key_never_sent', base: 'https://api.groq.com/openai/v1', keys: { openai: { key: 'gsk_test_key_never_sent' } } };
// every /api/anima call the page made, as its mode parameter
const ANIMA_MODES = `performance.getEntriesByType('resource').map((r) => r.name).filter((u) => u.includes('/api/anima?')).map((u) => new URL(u).searchParams.get('mode'))`;

async function sheet(browser, sim, mode) {
  const page = await browser.newPage();
  await bootShell(page, sim, { lang: 'it', wait: false });
  await page.eval(`localStorage.setItem('anima.mode', ${JSON.stringify(mode)}), true`);
  await page.goto(`${sim.origin}/apps/spreadsheet/`);
  assert.ok(await page.waitFor(`typeof remoteAnima === 'function' && window.sheetAPI && window.sheetAPI.hasGrokKey()`, { timeout: 30000 }), 'the vault key is seen');
  await page.eval(`performance.clearResourceTimings(), true`);
  return page;
}

test('spreadsheet: Private mode keeps ANIMA offline — mode=off, no online escalation; Auto uses mode=on', { skip }, async (t) => {
  const sim = await startSim({ seed: { '/data/anima/teacher.json': VAULT } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });

  const priv = await sheet(browser, sim, 'private');
  await priv.eval(`remoteAnima('quanto fa due per tre', 'it').catch(() => {}), true`);
  await priv.eval(`(setCellRaw(0, 0, '=ANIMA("chi ha scritto il codice da vinci che non esiste")'), typeof recalcAll === 'function' && recalcAll(), true)`);
  await sleep(2500);
  const pm = await priv.eval(ANIMA_MODES);
  assert.ok(pm.length >= 1, 'the device assistant was asked: ' + JSON.stringify(pm));
  assert.deepEqual(pm.filter((m) => m !== 'off'), [], 'Private: every device call is offline-only (mode=off): ' + JSON.stringify(pm));
  const groq = await priv.eval(`performance.getEntriesByType('resource').map((r) => r.name).filter((u) => u.includes('groq.com'))`);
  assert.deepEqual(groq, [], 'nothing went to the cloud');

  const auto = await sheet(browser, sim, 'auto');
  await auto.eval(`remoteAnima('quanto fa due per tre', 'it').catch(() => {}), true`);
  await sleep(1500);
  assert.ok((await auto.eval(ANIMA_MODES)).includes('on'), 'Auto: the device may use its online tier (mode=on)');
});
