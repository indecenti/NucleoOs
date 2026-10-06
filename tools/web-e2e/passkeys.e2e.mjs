// Browser E2E — Passkeys (the FIDO2 security-key console) against the simulator's /api/fido/* mirror of
// firmware nucleo_fido/port/fido_api.c. A resident passkey is the ONLY way into some accounts, and deleting
// one cannot be undone, so: delete and reset must ask first, a declined prompt must send nothing, a stale
// list must never delete a DIFFERENT passkey (the device deletes by list index), failures are reported and
// never look like "no passkeys", and the PIN is validated before it is sent and never left on screen.
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
const fmt = (s, a) => s.replace(/\{(\w+)\}/g, (m, k) => (k in a ? String(a[k]) : m));

async function openApp(browser, sim, lang = 'en', init = null) {
  const page = await browser.newPage();
  if (init) await page.initScript(init);
  await bootShell(page, sim, { lang, wait: false });
  await page.goto(`${sim.origin}/apps/passkeys/`);
  await page.networkIdle({ quiet: 500, timeout: 20000 });
  return page;
}
const stubConfirm = (answer) => `(() => { window.__asked = []; window.confirm = (m) => { window.__asked.push(String(m)); return ${answer}; }; return true; })()`;
const rows = (page) => page.eval(`[...document.querySelectorAll('#creds .cred')].map((r) => r.querySelector('.rp').textContent + '|' + r.querySelector('.u').textContent)`);
const deviceCreds = (page) => page.eval(`fetch('/api/fido/creds', { cache: 'no-store' }).then((r) => r.json()).then((a) => a.map((c) => c.rp + '/' + c.user))`);
const hits = async (sim, p) => (await sim.control('/api/_sim/stats')).byPath[p] || 0;
const msg = (page) => page.eval(`document.getElementById('msg').textContent`);

test('passkeys: deleting a passkey asks first (localized, names the site); declining sends nothing', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const de = cat('passkeys', 'de');
  const page = await openApp(browser, sim, 'de');
  assert.ok(await page.waitFor(`document.querySelectorAll('#creds .cred').length === 1`, { timeout: 30000 }), 'the simulator\'s demo passkey is listed');
  assert.deepEqual(await rows(page), ['github.com|demo  ·  #12'], 'site, user and sign counter shown');

  await sim.control('/api/_sim/stats', { reset: true });
  await page.eval(stubConfirm(false));
  await page.eval(`document.querySelector('#creds .cred .del').click(), true`);
  await sleep(800);
  assert.equal(await hits(sim, '/api/fido/cred/delete'), 0, 'declined (or never asked): no delete request may be sent');
  assert.deepEqual(await deviceCreds(page), ['github.com/demo'], 'the passkey is still on the device');
  assert.deepEqual(await page.eval(`window.__asked`), [fmt(de.askdel || '(no askdel string)', { rp: 'github.com', user: 'demo' })], 'asked once, in German, naming site and user');

  await page.eval(stubConfirm(true));
  await page.eval(`document.querySelector('#creds .cred .del').click(), true`);
  assert.ok(await page.waitFor(`document.getElementById('msg').textContent === ${JSON.stringify(de.delok)}`, { timeout: 10000 }), 'confirmed: deleted and reported');
  assert.deepEqual(await deviceCreds(page), []);
  assert.ok(await page.waitFor(`document.querySelector('#creds .empty') && document.querySelector('#creds .empty').textContent === ${JSON.stringify(de.empty)}`, { timeout: 10000 }));
});

// A stateful in-page stand-in for the device (two passkeys) so the test can change the list "elsewhere"
// between render and click — exactly what the native FIDO UI or a second browser tab does.
const FAKE_FIDO = `(() => {
  if (!location.pathname.startsWith('/apps/passkeys')) return;
  const st = window.__fido = { creds: [{ rp: 'github.com', user: 'alice', signCount: 3 }, { rp: 'bank.example', user: 'alice', signCount: 9 }], deletes: [] };
  const real = window.fetch.bind(window);
  const json = (o) => new Response(JSON.stringify(o), { status: 200, headers: { 'content-type': 'application/json' } });
  window.fetch = async (input, opts = {}) => {
    const p = new URL(typeof input === 'string' ? input : input.url, location.href).pathname;
    if (p === '/api/fido/status') return json({ keyHardware: true, pinSet: true, pinRetries: 8, credCount: st.creds.length, connected: true });
    if (p === '/api/fido/creds') return json(st.creds.map((c, index) => ({ index, ...c })));
    if (p === '/api/fido/cred/delete') {
      const i = JSON.parse(opts.body).index; st.deletes.push(i);
      const ok = i >= 0 && i < st.creds.length; if (ok) st.creds.splice(i, 1);
      return json({ ok });
    }
    return real(input, opts);
  };
})()`;

test('passkeys: a stale list never deletes a DIFFERENT passkey (the device deletes by index)', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const en = cat('passkeys', 'en');
  const page = await openApp(browser, sim, 'en', FAKE_FIDO);
  assert.ok(await page.waitFor(`document.querySelectorAll('#creds .cred').length === 2`, { timeout: 30000 }));
  // github.com (index 0) is removed elsewhere; bank.example slides to index 0. The page still shows both.
  await page.eval(`window.__fido.creds.shift(), true`);
  await page.eval(stubConfirm(true));
  await page.eval(`[...document.querySelectorAll('#creds .cred')].find((r) => r.querySelector('.rp').textContent === 'github.com').querySelector('.del').click(), true`);
  await sleep(1200);
  assert.deepEqual(await page.eval(`window.__fido.creds.map((c) => c.rp)`), ['bank.example'], 'the bank passkey was deleted in place of the stale github row');
  assert.equal(await msg(page), en.stale, 'the user is told the list changed');
  assert.deepEqual(await rows(page), ['bank.example|alice  ·  #9'], 'and sees the current list');
});

test('passkeys: the PIN is validated before it is sent and never left on screen', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const en = cat('passkeys', 'en');
  const page = await openApp(browser, sim, 'en');
  assert.ok(await page.waitFor(`document.getElementById('v-pin').textContent === ${JSON.stringify(en.notset)}`, { timeout: 30000 }));
  const submit = (a, b) => page.eval(`(async () => {
    if (document.getElementById('pinform').hidden) document.getElementById('b-pin').click();
    document.getElementById('pin1').value = ${JSON.stringify(a)}; document.getElementById('pin2').value = ${JSON.stringify(b)};
    document.getElementById('pin-ok').click();
    await new Promise((r) => setTimeout(r, 600));
    return document.getElementById('msg').textContent;
  })()`);
  await sim.control('/api/_sim/stats', { reset: true });
  assert.equal(await submit('123', '123'), en.pinbad, 'too short');
  assert.equal(await submit('x'.repeat(64), 'x'.repeat(64)), en.pinbad, 'too long (the input maxlength can be bypassed by paste/scripts)');
  assert.equal(await submit('s3cret-pin', 's3cret-pjn'), en.pinmm, 'mismatch');
  assert.equal(await hits(sim, '/api/fido/pin'), 0, 'nothing invalid reached the device');

  const PIN = 's3cret-pin-42';
  assert.equal(await submit(PIN, PIN), en.pinok);
  assert.equal(await hits(sim, '/api/fido/pin'), 1);
  assert.ok(await page.waitFor(`document.getElementById('v-pin').textContent === ${JSON.stringify(en.set + ' · 8 ' + en.tries)}`, { timeout: 10000 }), 'status shows the PIN is set');
  const leak = await page.eval(`({ hidden: document.getElementById('pinform').hidden,
    values: document.getElementById('pin1').value + document.getElementById('pin2').value,
    inDom: document.documentElement.outerHTML.includes(${JSON.stringify(PIN)}) || document.body.innerText.includes(${JSON.stringify(PIN)}) })`);
  assert.deepEqual(leak, { hidden: true, values: '', inDom: false }, 'the PIN form closed, cleared, and the PIN appears nowhere in the page');
});

test('passkeys: reset asks first; declining keeps every passkey and the PIN', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const it = cat('passkeys', 'it');
  const page = await openApp(browser, sim, 'it');
  assert.ok(await page.waitFor(`document.querySelectorAll('#creds .cred').length === 1`, { timeout: 30000 }));
  await sim.control('/api/_sim/stats', { reset: true });
  await page.eval(stubConfirm(false));
  await page.eval(`document.getElementById('b-reset').click(), true`);
  await sleep(800);
  assert.deepEqual(await page.eval(`window.__asked`), [it.askreset]);
  assert.equal(await hits(sim, '/api/fido/reset'), 0, 'declined: no reset request');
  assert.deepEqual(await deviceCreds(page), ['github.com/demo']);

  await page.eval(stubConfirm(true));
  await page.eval(`document.getElementById('b-reset').click(), true`);
  assert.ok(await page.waitFor(`document.getElementById('msg').textContent === ${JSON.stringify(it.resetok)}`, { timeout: 10000 }));
  assert.deepEqual(await deviceCreds(page), []);
});

test('passkeys: failed reads and a failed delete are reported, never shown as "no passkeys" or as deleted', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const en = cat('passkeys', 'en');

  await t.test('the credential list cannot be read (503): an error, not an empty list', async () => {
    await sim.control('/api/_sim/fault', { route: '/api/fido/creds', status: 503 });
    const page = await openApp(browser, sim, 'en');
    assert.ok(await page.waitFor(`document.getElementById('msg').textContent === ${JSON.stringify(en.err)}`, { timeout: 30000 }), 'the failure is reported');
    assert.notEqual(await page.eval(`document.getElementById('creds').textContent`), en.empty, '"No passkeys yet" would tell the user their keys are gone');
    await sim.control('/api/_sim/fault', { clear: true });
  });

  await t.test('the device refuses the delete: error shown, the passkey stays listed', async () => {
    const page = await openApp(browser, sim, 'en');
    assert.ok(await page.waitFor(`document.querySelectorAll('#creds .cred').length === 1`, { timeout: 30000 }));
    await sim.control('/api/_sim/fault', { route: '/api/fido/cred/delete', status: 503, times: 1 });
    await page.eval(stubConfirm(true));
    await page.eval(`document.querySelector('#creds .cred .del').click(), true`);
    assert.ok(await page.waitFor(`document.getElementById('msg').textContent === ${JSON.stringify(en.err)}`, { timeout: 10000 }));
    await sleep(500);
    assert.deepEqual(await rows(page), ['github.com|demo  ·  #12']);
    assert.deepEqual(await deviceCreds(page), ['github.com/demo']);
  });
});
