// Browser E2E — WiFi Scanner / manager against the simulator's /api/wifi/{scan,known,join} (mirror of
// firmware nucleo_httpd.c wifi_scan_get + the nucleo_setup multi-network API). Covers what a user relies on:
//   • an SSID is attacker-controlled (anyone nearby can broadcast one): it renders as TEXT, never markup;
//   • the list is sorted strongest-first with the right counts, badges (connected / saved / open) and a
//     hidden network shown as "(hidden)" and not joinable;
//   • joining a secured network sends {ssid, pass} once and the password never lands in the page text;
//   • an empty scan says "no networks"; a FAILED scan (offline, busy, not paired) never fabricates networks
//     or a "connected" badge — it keeps the last real scan, or says why there is none.
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
const fmt = (s, a, lang = 'en') => {
  if (s && typeof s === 'object') s = s[new Intl.PluralRules(lang).select(Number(a.count))] ?? s.other;
  return String(s).replace(/\{(\w+)\}/g, (m, k) => (k in a ? String(a[k]) : m));
};
// The demo set the app used to invent when a scan failed — none of these exist around the simulator.
const DEMO = ['NucleoOS-Mesh', 'HomeFiber-5G', 'Cardputer-AP', 'GuestZone', 'IoT-Lab', 'eduroam'];

// Optionally replace the scan answer (plan = the JSON body) and record /api/wifi/join bodies.
const STUB = (plan) => `(() => {
  if (!location.pathname.startsWith('/apps/wifi-scanner')) return;
  window.__joins = []; window.__scanPlan = ${JSON.stringify(plan ?? null)};
  const real = window.fetch.bind(window);
  window.fetch = async (input, opts = {}) => {
    const p = new URL(typeof input === 'string' ? input : input.url, location.href).pathname;
    if (p === '/api/wifi/join') { try { window.__joins.push(JSON.parse(opts.body)); } catch {} }
    if (p === '/api/wifi/scan' && window.__scanPlan) return new Response(JSON.stringify(window.__scanPlan), { status: 200, headers: { 'content-type': 'application/json' } });
    return real(input, opts);
  };
})()`;

async function openApp(browser, sim, { lang = 'en', plan = null, faults = [] } = {}) {
  const page = await browser.newPage();
  await page.initScript(STUB(plan));
  await bootShell(page, sim, { lang, wait: false });
  for (const f of faults) await sim.control('/api/_sim/fault', f);
  await page.goto(`${sim.origin}/apps/wifi-scanner/`);
  await page.networkIdle({ quiet: 500, timeout: 20000 });
  return page;
}
const settled = (page, timeout = 30000) => page.waitFor(`document.getElementById('src').textContent !== '' &&
  !document.getElementById('hd').classList.contains('scanning') && document.getElementById('list').children.length > 0`, { timeout });
const rows = (page) => page.eval(`[...document.querySelectorAll('#list li')].filter((li) => li.querySelector('.name')).map((li) => ({
  name: li.querySelector('.name').textContent, me: !!li.querySelector('.me'), saved: !!li.querySelector('.saved'),
  open: !!li.querySelector('.lock.open'), dbm: li.querySelector('.dbm').textContent }))`);
const src = (page) => page.eval(`document.getElementById('src').textContent`);

test('wifi-scanner: the live scan is listed strongest first, with counts and connected/saved/open badges', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const en = cat('wifi-scanner', 'en');
  const page = await openApp(browser, sim);
  assert.ok(await settled(page), 'the first scan finished');
  assert.deepEqual(await rows(page), [
    { name: 'home-wifi', me: true, saved: true, open: false, dbm: '-42 dBm' },
    { name: 'FRITZ!Box 7530', me: false, saved: true, open: false, dbm: '-67 dBm' },
    { name: 'CoffeeShop_Free', me: false, saved: false, open: true, dbm: '-78 dBm' },
    { name: 'Vodafone-2261', me: false, saved: false, open: false, dbm: '-83 dBm' },
  ]);
  assert.equal(await page.eval(`document.getElementById('count').textContent`), fmt(en.count, { count: 4 }) + ' · ' + fmt(en.count_open, { count: 1 }));
  assert.ok((await src(page)).startsWith(en.src_live), 'labelled as a live scan');
});

test('wifi-scanner: SSIDs are text, never markup; a hidden network is "(hidden)" and not joinable', { skip }, async (t) => {
  const evil = '<img src=x onerror="window.__pwned=1">';
  const plan = { networks: [
    { ssid: evil, rssi: -50, channel: 6, auth: 'WPA2' },
    { ssid: '<b>Free</b>&amp;WiFi', rssi: -60, channel: 1, auth: 'Open' },
    { ssid: '', rssi: -70, channel: 11, auth: 'WPA2' },
  ] };
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const en = cat('wifi-scanner', 'en');
  const page = await openApp(browser, sim, { plan });
  assert.ok(await settled(page));
  await sleep(300);
  const r = await rows(page);
  assert.deepEqual(r.map((x) => x.name), [evil, '<b>Free</b>&amp;WiFi', en.hidden_ssid], 'names rendered literally');
  assert.deepEqual(await page.eval(`({ injected: document.querySelectorAll('#list img, #list .name b').length, pwned: !!window.__pwned })`),
    { injected: 0, pwned: false }, 'no element was created from an SSID');
  // Open the hostile one's drawer and join: the SSID goes to the device as the exact string.
  await page.eval(`(() => { const li = [...document.querySelectorAll('#list li')][0]; li.click();
    li.querySelector('.pw').value = 'pw-123456'; li.querySelector('.join').click(); return true; })()`);
  assert.ok(await page.waitFor(`window.__joins.length === 1`, { timeout: 10000 }));
  assert.deepEqual(await page.eval(`window.__joins[0]`), { ssid: evil, pass: 'pw-123456' });
  assert.equal(await page.eval(`document.querySelectorAll('#list img').length`), 0, 'the toast/list stayed text after the join');
  await page.eval(`[...document.querySelectorAll('#list li')][2].click(), true`);
  assert.equal(await page.eval(`document.querySelectorAll('.act').length`), 0, 'a hidden SSID offers no Connect drawer');
});

test('wifi-scanner: joining a secured network sends {ssid, pass} once; the password never appears in the page', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const en = cat('wifi-scanner', 'en');
  const page = await openApp(browser, sim);
  assert.ok(await settled(page));
  const PASS = 'c0rrect-h0rse-battery';
  const open = (name) => page.eval(`(() => { const li = [...document.querySelectorAll('#list li')].find((l) => l.querySelector('.name') && l.querySelector('.name').textContent === ${JSON.stringify(name)});
    li.click(); const a = li.querySelector('.act'); return a ? { pw: !!a.querySelector('.pw'), forget: !!a.querySelector('.forget'), join: a.querySelector('.join').textContent } : null; })()`);
  assert.deepEqual(await open('CoffeeShop_Free'), { pw: false, forget: false, join: en.connect }, 'an open network needs no password');
  await page.eval(`document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })), true`);
  assert.deepEqual(await open('Vodafone-2261'), { pw: true, forget: false, join: en.connect }, 'a new secured network asks for its password');
  await page.eval(`(() => { const pw = document.querySelector('.act .pw'); pw.value = ${JSON.stringify(PASS)};
    pw.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })); return true; })()`);
  assert.ok(await page.waitFor(`document.getElementById('toast').textContent === ${JSON.stringify(fmt(en.connected_to, { ssid: 'Vodafone-2261' }))}`, { timeout: 15000 }), 'joined and confirmed');
  assert.deepEqual(await page.eval(`window.__joins`), [{ ssid: 'Vodafone-2261', pass: PASS }], 'exactly one join request with the typed password');
  assert.ok(await page.waitFor(`(() => { const me = [...document.querySelectorAll('#list li.cur .name')].map((n) => n.textContent);
    return me.length === 1 && me[0] === 'Vodafone-2261' && !document.getElementById('hd').classList.contains('scanning'); })()`, { timeout: 15000 }),
    'the connected badge moved to the joined network after the re-scan');
  const r = await rows(page);
  assert.deepEqual(r.filter((x) => x.me).map((x) => x.name), ['Vodafone-2261']);
  assert.ok(r.find((x) => x.name === 'Vodafone-2261').saved, 'and it is now saved');
  assert.equal(await page.eval(`document.documentElement.outerHTML.includes(${JSON.stringify(PASS)}) || document.body.innerText.includes(${JSON.stringify(PASS)})`), false, 'the password is nowhere in the page');
});

test('wifi-scanner: an empty scan says "no networks" (and invents none)', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const en = cat('wifi-scanner', 'en');
  const page = await openApp(browser, sim, { plan: { networks: [] } });
  assert.ok(await settled(page));
  assert.deepEqual(await rows(page), []);
  assert.equal(await page.eval(`document.getElementById('count').textContent`), fmt(en.count, { count: 0 }));
  assert.ok(await page.waitFor(`document.querySelector('#list .empty').textContent.includes('Scan')`, { timeout: 10000 }), 'the empty-state message is shown');
  assert.ok((await src(page)).startsWith(en.src_live), 'an empty result is still a real, live scan');
});

test('wifi-scanner: a failed scan never fabricates networks or a "connected" badge', { skip }, async (t) => {
  const en = cat('wifi-scanner', 'en');

  await t.test('device unreachable from the start: no demo networks, the reason is shown', async () => {
    const sim = await startSim();
    const browser = await launchBrowser({ args: [HOST_RULES] });
    t.after(async () => { await browser.close(); await sim.stop(); });
    const page = await openApp(browser, sim, { faults: [{ route: '/api/wifi', drop: true }, { route: '/api/status', drop: true }] });
    assert.ok(await settled(page));
    const r = await rows(page);
    assert.deepEqual(r.filter((x) => DEMO.includes(x.name)).map((x) => x.name), [], 'invented networks shown as real');
    assert.deepEqual(r.filter((x) => x.me).map((x) => x.name), [], 'a "connected" badge while the device cannot be reached');
    assert.equal(await src(page), en.src_failed || '(no src_failed string)');
    assert.ok(await page.eval(`document.getElementById('list').textContent.includes(${JSON.stringify(en.src_failed || '(no src_failed string)')})`), 'the list explains why it is empty');
  });

  await t.test('a later scan fails (device busy): the last REAL scan stays, labelled as failed', async () => {
    const sim = await startSim();
    const browser = await launchBrowser({ args: [HOST_RULES] });
    t.after(async () => { await browser.close(); await sim.stop(); });
    const page = await openApp(browser, sim);
    assert.ok(await settled(page));
    const before = await rows(page);
    assert.equal(before.length, 4);
    await sim.control('/api/_sim/fault', { route: '/api/wifi/scan', status: 503 });
    await page.eval(`document.getElementById('scan').click(), true`);
    assert.ok(await page.waitFor(`!document.getElementById('src').textContent.startsWith(${JSON.stringify(en.src_live)})`, { timeout: 15000 }), 'the source label changed');
    assert.ok(await settled(page));
    assert.equal(await src(page), en.src_failed || '(no src_failed string)');
    assert.deepEqual((await rows(page)).map((x) => x.name), before.map((x) => x.name), 'the real list was replaced');
  });

  await t.test('not paired (401, as the firmware answers): "pair to scan", no invented networks', async () => {
    const sim = await startSim();
    const browser = await launchBrowser({ args: [HOST_RULES] });
    t.after(async () => { await browser.close(); await sim.stop(); });
    const page = await openApp(browser, sim, { faults: [{ route: '/api/wifi', status: 401 }, { route: '/api/status', drop: true }] });
    assert.ok(await settled(page));
    assert.deepEqual(await rows(page), [], 'no networks are invented');
    assert.equal(await src(page), en.src_need_pair || '(no src_need_pair string)');
  });
});
