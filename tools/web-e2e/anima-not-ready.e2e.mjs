// Browser E2E — the device assistant is off (or only partly on) and the web OS must not mistake that for an
// answer. On the Cardputer's web profile the firmware runs ANIMA memory-only: personal memory (profile, "remember
// that X is Y", recall) is answered, everything else is 503 {"error":"not_ready"}. Until firmware 0.7.5 every
// question got 200 "I don't know" and My Facts said "Profile saved and taught" while ANIMA had learned nothing
// (Discord report, 2026-10-27). Covers:
//   * My Facts: "taught" only when ANIMA confirmed every profile field; otherwise it says it did not learn it,
//     and why when the device is unreachable. The profile is saved to the SD in every case.
//   * ANIMA app: a 503 "not_ready" is not retried like a 503 "busy" (it can't change by retrying).
//   * Terminal `anima`: says the assistant is off, instead of retrying and reporting "busy".
//   * Settings' AI check: a 503 body is not "answered by device".
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
const cat = (app, lang) => ({ ...rd(`../../web/shell/i18n/core.${lang}.json`), ...rd(`../../apps/${app}/www/i18n.${lang}.json`) });
const NOT_READY = { route: '/api/anima', status: 503, body: { error: 'not_ready', memory_only: true } };

async function openApp(browser, sim, id, lang = 'en') {
  const page = await browser.newPage();
  await bootShell(page, sim, { lang, wait: false });
  await page.goto(`${sim.origin}/apps/${id}/`);
  await page.networkIdle({ quiet: 500, timeout: 20000 });
  return page;
}

test('miei-fatti: the profile is "taught" only when ANIMA confirms it', { skip }, async (t) => {
  const sim = await startSim({ seed: { '/data/miei-fatti.json': { schema: 2, profile: {}, facts: [], rels: [] } } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const en = cat('miei-fatti', 'en');
  const page = await openApp(browser, sim, 'miei-fatti', 'en');
  assert.ok(await page.waitFor(`!!document.getElementById('p-name') && !!document.getElementById('saveprof')`, { timeout: 20000 }));
  const save = (name) => page.eval(`(() => { document.getElementById('profmsg').textContent = '';
    document.getElementById('p-name').value = ${JSON.stringify(name)}; document.getElementById('saveprof').click(); return true; })()`);
  const msgIs = (s) => page.waitFor(`document.getElementById('profmsg').textContent === ${JSON.stringify(s)}`, { timeout: 15000 });
  const msg = () => page.eval(`document.getElementById('profmsg').textContent`);
  const savedName = async () => JSON.parse(await sim.readSd('/data/miei-fatti.json')).profile.name;

  await t.test('ANIMA answers without learning ("I don\'t know"): not reported as taught, still saved', async () => {
    await save('Ada');
    assert.ok(await msgIs(en.profNotTaught), 'got: ' + await msg());
    await sleep(800);
    assert.equal(await savedName(), 'Ada', 'the profile is saved to the SD anyway');
  });

  await t.test('device unreachable (503): says so', async () => {
    await sim.control('/api/_sim/fault', { route: '/api/anima', status: 503 });
    await save('Ada2');
    assert.ok(await msgIs(en.profNotTaught + ' — ' + en.noconn), 'got: ' + await msg());
    await sim.control('/api/_sim/fault', { clear: true });
  });

  await t.test('ANIMA confirms every field: "saved and taught"', async () => {
    await page.eval(`(() => { const f = window.fetch; window.fetch = (u, o) => String(u).startsWith('/api/anima?')
      ? Promise.resolve(new Response(JSON.stringify({ tier: 'command', action: 'answer', intent: 'profile', reply: 'Nice to meet you!' }),
          { status: 200, headers: { 'content-type': 'application/json' } }))
      : f(u, o); return true; })()`);
    await save('Ada3');
    assert.ok(await page.waitFor(`document.getElementById('profmsg').textContent === ${JSON.stringify(en.profSaved)}`, { timeout: 15000 }), 'got: ' + await msg());
  });
});

test('anima app: a 503 "not_ready" from the device is not retried', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await openApp(browser, sim, 'anima', 'en');
  assert.ok(await page.waitFor(`!!document.getElementById('q') && !!document.getElementById('send')`, { timeout: 30000 }));
  await sleep(1500);                                   // let the boot probes settle before counting

  // Two identical turns: one against a "busy" device (retried by design), one against a "not_ready" one.
  const turn = async (fault) => {
    await sim.control('/api/_sim/fault', fault);
    const before = (await sim.control('/api/_sim/stats')).byPath['/api/anima'] || 0;
    await page.eval(`(() => { const q = document.getElementById('q'); q.value = 'qwzx plorf'; q.dispatchEvent(new Event('input', { bubbles: true }));
      document.getElementById('send').click(); return true; })()`);
    await sleep(6000);
    await sim.control('/api/_sim/fault', { clear: true });
    return ((await sim.control('/api/_sim/stats')).byPath['/api/anima'] || 0) - before;
  };
  const busy = await turn({ route: '/api/anima', status: 503 });
  const notReady = await turn(NOT_READY);
  assert.ok(busy > 1, `a busy device is retried (saw ${busy} request(s))`);
  assert.ok(notReady < busy, `not_ready must not be retried like busy: ${notReady} request(s) vs ${busy} for busy`);
});

test('terminal: `anima` on a not_ready device says the assistant is off', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await openApp(browser, sim, 'terminal', 'en');
  assert.ok(await page.waitFor(`!!document.getElementById('in') && !!document.getElementById('cmdform')`, { timeout: 20000 }));
  await sim.control('/api/_sim/fault', NOT_READY);
  await page.eval(`(() => { document.getElementById('in').value = 'anima what time is it';
    document.getElementById('cmdform').requestSubmit(); return true; })()`);
  assert.ok(await page.waitFor(`document.body.innerText.includes('the device assistant is off while the web desktop is open')`, { timeout: 15000 }),
    'got: ' + (await page.eval(`document.body.innerText`)).slice(-300));
  assert.ok(!(await page.eval(`document.body.innerText.includes('brain busy')`)), 'not reported as busy');
});

test('settings: the AI check does not count a 503 body as an answer', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const en = cat('settings', 'en');
  const page = await openApp(browser, sim, 'settings', 'en');
  assert.ok(await page.waitFor(`!!document.getElementById('preset-verify') && !!document.getElementById('preset-verify-out')`, { timeout: 20000 }));
  await sim.control('/api/_sim/fault', NOT_READY);
  await page.eval(`(document.getElementById('preset-verify').click(), true)`);
  assert.ok(await page.waitFor(`document.getElementById('preset-verify-out').textContent === ${JSON.stringify(en.apNoAnswer)}`, { timeout: 15000 }),
    'got: ' + await page.eval(`document.getElementById('preset-verify-out').textContent`));
});
