// Browser E2E — Nearby (Vicino, ESP-NOW device-to-device) against the simulator.
//   - the real simulated link: scan → peer list → pick a file → send → progress reaches "completed",
//     and the device is asked to send THAT file to THAT peer;
//   - peer names (and offer / command text) come from OTHER devices over the air: always text;
//   - the chosen recipient is a DEVICE, not a row number: when the peer list reorders or the chosen
//     device disappears, a send must never silently go to someone else;
//   - a command being typed survives the background peer refresh;
//   - a failed peer query says the device can't be reached (not "no devices"); failed sends say so.
// The device's /api/link/* is faked IN THE PAGE for the hostile / reorder cases (no radio, no P2P).
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
const EN = { ...rd('../../web/shell/i18n/core.en.json'), ...rd('../../apps/nearby/www/i18n.en.json') };
const EVIL = '<img src=x onerror="window.__pwned=1">Bob & <b>co</b>';

// In-page stand-in for the device's link API (only on the Nearby page). window.__link is the "radio":
// tests mutate its peers/offer/cmd and read back every call the app made.
const FAKE_LINK = `(() => {
  if (!location.pathname.startsWith('/apps/nearby/')) return;
  const L = window.__link = { name: 'me', channel: 6, peers: [], status: { active: false, state: 0 },
    offer: { pending: false }, cmd: { pending: false }, calls: [], fail: {}, ok: true };
  const real = window.fetch.bind(window);
  window.fetch = async (u, o = {}) => {
    const url = new URL(String(u), location.href);
    if (!url.pathname.startsWith('/api/link/')) return real(u, o);
    const sub = url.pathname.slice('/api/link/'.length), method = (o.method || 'GET').toUpperCase();
    L.calls.push({ sub, method, body: o.body ? JSON.parse(o.body) : null });
    const json = (x, status = 200) => new Response(JSON.stringify(x), { status, headers: { 'content-type': 'application/json' } });
    const f = L.fail[sub];
    if (f === 'network') throw new TypeError('Failed to fetch');
    if (f) return json({ ok: false, err: 'busy' }, f);
    if (sub === 'peers') return json({ name: L.name, channel: L.channel, inbox: '/data/Vicino', peers: L.peers.map((p, i) => ({ i, ...p })) });
    if (sub === 'status') return json(L.status);
    if (sub === 'offer' && method === 'GET') return json(L.offer);
    if (sub === 'cmd' && method === 'GET') return json(L.cmd);
    if (sub === 'send' || sub === 'cmd') return json({ ok: L.ok });
    return json({ ok: true });
  };
})();`;

async function open(browser, sim, { fake = false } = {}) {
  const page = await browser.newPage();
  if (fake) await page.initScript(FAKE_LINK);
  await bootShell(page, sim, { lang: 'en', wait: false });
  await page.goto(`${sim.origin}/apps/nearby/`);
  await page.waitFor(`document.querySelector('#view .card') !== null`, { timeout: 45000 });
  return page;
}
const tab = (name) => `document.querySelector('#tabs [data-t="${name}"]').click(), true`;
const ROWS = `[...document.querySelectorAll('#plist .row .nm')].map((n) => n.textContent)`;
const BADGE = `document.querySelector('#view .card .badge').textContent`;
const TOAST = `document.getElementById('toast').textContent`;
const peersRefresh = `window.postMessage({ t: 'os-visibility', d: { visible: true } }, '*'), true`;   // what the shell sends on focus
const calls = (sub) => `window.__link.calls.filter((c) => c.sub === ${JSON.stringify(sub)} && c.method === 'POST').map((c) => c.body)`;

test('nearby: scan → pick a peer → pick a file → send: the device gets that file for that peer, progress completes', { skip }, async (t) => {
  const sim = await startSim({ seed: { '/data/hello.txt': 'hi there', '/data/Docs/inner.txt': 'x' } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);

  assert.equal(await page.eval(`document.querySelector('#plist .empty').textContent`), EN.peers_empty, 'nothing found before a scan');
  await page.eval(`document.getElementById('scan').click(), true`);
  assert.ok(await page.waitFor(`document.querySelectorAll('#plist .row').length === 2`, { timeout: 10000 }), 'the scan found the two simulated peers');
  assert.deepEqual(await page.eval(ROWS), ['Nucleo-Bruno', 'Bruce-Box']);
  assert.deepEqual(await page.eval(`[...document.querySelectorAll('#plist .pill')].map((p) => p.textContent)`), ['NUCLEO', 'BRUCE']);
  assert.equal(await page.eval(`document.getElementById('dev').textContent`), 'nucleo-sim');

  await page.eval(`document.querySelectorAll('#plist .row')[1].click(), true`);
  assert.equal(await page.eval(TOAST), EN.recipient_x.replace('{name}', 'Bruce-Box'));
  await page.eval(tab('send'));
  assert.ok(await page.waitFor(`[...document.querySelectorAll('#flist .row .nm')].some((n) => n.textContent.includes('hello.txt'))`, { timeout: 10000 }), 'the /data listing is shown');
  assert.equal(await page.eval(BADGE), '→ Bruce-Box');
  assert.equal(await page.eval(`document.getElementById('dosend').disabled`), true, 'no file chosen yet → Send is disabled');
  await page.eval(`[...document.querySelectorAll('#flist .row')].find((r) => r.textContent.includes('hello.txt')).click(), true`);
  assert.equal(await page.eval(`document.getElementById('dosend').disabled`), false);
  await page.eval(`document.getElementById('dosend').click(), true`);
  assert.ok(await page.waitFor(`${TOAST} === ${JSON.stringify(EN.send_started)}`, { timeout: 5000 }));

  const st = await sim.control('/api/link/status');
  assert.equal(st.name, 'hello.txt'); assert.equal(st.peer, 'Bruce-Box', 'the device sends to the peer that was picked');
  assert.ok(await page.waitFor(`document.getElementById('pverb').textContent === ${JSON.stringify(EN.sending + ' — ' + EN.state_done)}`, { timeout: 15000 }), 'progress reaches "completed"');
  assert.equal(await page.eval(`document.getElementById('pbar').style.width`), '100%');
});

test('nearby: names, offers and commands from other devices are shown as text', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim, { fake: true });
  await page.eval(`window.__link.peers = [{ name: ${JSON.stringify(EVIL)}, mac: '<i>AA</i>:BB', proto: 'nucleo' }]; window.__link.offer = { pending: true, from: ${JSON.stringify(EVIL)}, name: '<svg onload="window.__pwned=2">.txt', size: 2048 };
    window.__link.cmd = { pending: true, from: ${JSON.stringify(EVIL)}, cmd: '<img src=y onerror="window.__pwned=3">' };
    window.__link.status = { active: true, sending: false, state: 2, done: 1024, total: 2048, rate: 0, name: '<b>f</b>.bin', peer: ${JSON.stringify(EVIL)} }; true`);
  await page.eval(peersRefresh);
  assert.ok(await page.waitFor(`document.querySelectorAll('#plist .row').length === 1`, { timeout: 5000 }));
  assert.deepEqual(await page.eval(ROWS), [EVIL]);
  assert.equal(await page.eval(`document.querySelector('#plist .row .dim').textContent`), '<i>AA</i>:BB');

  await page.eval(tab('send'));
  assert.ok(await page.waitFor(`document.querySelector('#view .card .badge') && ${BADGE} === ${JSON.stringify('→ ' + EVIL)}`, { timeout: 5000 }), 'send badge');
  assert.ok(await page.waitFor(`document.getElementById('pname').textContent === ${JSON.stringify('<b>f</b>.bin  ·  ' + EVIL)}`, { timeout: 8000 }), 'progress card');
  await page.eval(tab('recv'));
  assert.ok(await page.waitFor(`document.querySelector('#offer .amb') !== null`, { timeout: 8000 }), 'the offer is shown');
  assert.equal(await page.eval(`document.querySelector('#offer b').textContent`), EVIL);
  assert.equal(await page.eval(`document.querySelector('#offer .amb').textContent`), '<svg onload="window.__pwned=2">.txt · 2 KB');
  await page.eval(`document.getElementById('acc').click(), true`);
  assert.deepEqual(await page.waitFor(`(${calls('offer')}).length && ${calls('offer')}`, { timeout: 5000 }), [{ accept: true }], 'Accept answers the offer');
  await page.eval(tab('cmd'));
  assert.ok(await page.waitFor(`document.querySelector('#incoming .amb') !== null`, { timeout: 8000 }), 'the incoming command is shown');
  assert.equal(await page.eval(`document.querySelector('#incoming .amb').textContent`), '<img src=y onerror="window.__pwned=3">');
  assert.equal(await page.eval(`document.querySelector('#incoming b').textContent`), EVIL);
  await page.eval(`document.getElementById('crej').click(), true`);
  assert.deepEqual(await page.waitFor(`(${calls('cmd/confirm')}).length && ${calls('cmd/confirm')}`, { timeout: 5000 }), [{ ok: false }], 'Discard declines it');

  assert.equal(await page.eval(`document.querySelectorAll('#view img, #view svg, #view i, #view .row b').length`), 0, 'nothing from the air became markup');
  assert.equal(await page.eval(`window.__pwned === undefined`), true);
});

test('nearby: the recipient is a device, not a row — a reordered or shrunk peer list never retargets a send', { skip }, async (t) => {
  const sim = await startSim({ seed: { '/data/secret.txt': 'for bob only' } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim, { fake: true });
  const A = { name: 'Alice', mac: 'AA:AA:AA:AA:AA:01', proto: 'nucleo' }, B = { name: 'Bob', mac: 'BB:BB:BB:BB:BB:02', proto: 'nucleo' };
  await page.eval(`window.__link.peers = ${JSON.stringify([A, B])}; true`);
  await page.eval(peersRefresh);
  assert.ok(await page.waitFor(`document.querySelectorAll('#plist .row').length === 2`, { timeout: 5000 }));
  await page.eval(`document.querySelectorAll('#plist .row')[1].click(), true`);          // Bob
  await page.eval(tab('send'));
  assert.ok(await page.waitFor(`[...document.querySelectorAll('#flist .row')].some((r) => r.textContent.includes('secret.txt'))`, { timeout: 10000 }));
  await page.eval(`[...document.querySelectorAll('#flist .row')].find((r) => r.textContent.includes('secret.txt')).click(), true`);
  assert.equal(await page.eval(BADGE), '→ Bob');

  // The device restarted its radio and re-discovered the peers in the other order.
  await page.eval(`window.__link.peers = ${JSON.stringify([B, A])}; true`);
  await page.eval(peersRefresh);
  await sleep(400);
  assert.equal(await page.eval(BADGE), '→ Bob', 'still Bob after the reorder');
  await page.eval(`document.getElementById('dosend').click(), true`);
  const sends = await page.waitFor(`(${calls('send')}).length && ${calls('send')}`, { timeout: 5000 });
  assert.deepEqual(sends, [{ peer: 0, path: '/data/secret.txt', proto: 'nucleo' }], 'the send goes to Bob\'s CURRENT index');

  // Bob walks away: the send must not fall through to Alice.
  await page.eval(`window.__link.peers = ${JSON.stringify([A])}; true`);
  await page.eval(peersRefresh);
  await sleep(400);
  assert.equal(await page.eval(BADGE), EN.no_recipient, 'the chosen device is gone → no recipient');
  assert.equal(await page.eval(`document.getElementById('dosend').disabled`), true, 'and Send is disabled');
  await page.eval(`document.getElementById('dosend').click(), true`);
  await sleep(300);
  assert.equal((await page.eval(calls('send'))).length, 1, 'nothing was sent to Alice');

  // Bob comes back (at another index): he is the recipient again.
  await page.eval(`window.__link.peers = ${JSON.stringify([A, { ...B, name: 'Bob' }])}; true`);
  await page.eval(peersRefresh);
  await sleep(400);
  assert.equal(await page.eval(BADGE), '→ Bob');
});

test('nearby: a command being typed survives the peer refresh; send failures and an unreachable radio are reported', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim, { fake: true });
  await page.eval(`window.__link.peers = [{ name: 'Bruno', mac: '01:02:03:04:05:06', proto: 'nucleo' }]; true`);
  await page.eval(peersRefresh);
  assert.ok(await page.waitFor(`document.querySelectorAll('#plist .row').length === 1`, { timeout: 5000 }));
  await page.eval(tab('cmd'));
  await page.eval(`(() => { const i = document.getElementById('cmdin'); i.focus(); i.value = 'open mu'; return true; })()`);
  await page.eval(peersRefresh);                     // the 5 s peer poll fires while the user is typing
  await sleep(400);
  assert.equal(await page.eval(`document.getElementById('cmdin').value`), 'open mu', 'what was typed is kept');
  assert.equal(await page.eval(`document.activeElement && document.activeElement.id`), 'cmdin', 'and the caret stays in the box');

  await page.eval(`(() => { document.getElementById('cmdin').value = 'open music'; document.getElementById('cmdsend').click(); return true; })()`);
  assert.deepEqual(await page.waitFor(`(${calls('cmd')}).length && ${calls('cmd')}`, { timeout: 5000 }), [{ peer: 0, command: 'open music', proto: 'nucleo' }]);
  assert.ok(await page.waitFor(`${TOAST} === ${JSON.stringify(EN.cmd_sent)}`, { timeout: 5000 }));

  await page.eval(`window.__link.ok = false; document.getElementById('cmdin').value = 'again'; document.getElementById('cmdsend').click(), true`);
  assert.ok(await page.waitFor(`${TOAST} === ${JSON.stringify(EN.send_failed)}`, { timeout: 5000 }), 'a refused command says so');

  // The radio / device stops answering: that is not "no devices".
  await page.eval(tab('peers'));
  await page.eval(`window.__link.peers = []; window.__link.fail.peers = 503; true`);
  await page.eval(peersRefresh);
  assert.ok(await page.waitFor(`document.querySelector('#plist .empty, #plist .red') !== null`, { timeout: 5000 }));
  await sleep(300);
  assert.equal(await page.eval(`(document.querySelector('#plist .red') || document.querySelector('#plist .empty')).textContent`), EN.peers_failed, 'an unreachable link is reported as such');
  await page.eval(`window.__link.fail.peers = 'network'; true`);
  await page.eval(peersRefresh);
  await sleep(300);
  assert.equal(await page.eval(`(document.querySelector('#plist .red') || document.querySelector('#plist .empty')).textContent`), EN.peers_failed);
  await page.eval(`delete window.__link.fail.peers; true`);
  await page.eval(peersRefresh);
  assert.ok(await page.waitFor(`document.querySelector('#plist .empty') && document.querySelector('#plist .empty').textContent === ${JSON.stringify(EN.peers_empty)} && !document.querySelector('#plist .red')`, { timeout: 5000 }), 'back to normal once it answers');
});
