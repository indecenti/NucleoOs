// Browser E2E — System Monitor against the simulator: Overview renders a status snapshot (uptime, heap,
// battery, storage, network), survives a malformed or partial snapshot without "undefined"/"NaN" or a
// thrown handler, RAM/CPU tabs render /api/heap and /api/cpu, an offline device degrades to "n/a" without
// exceptions, and the live event feed shows event payloads as TEXT (they carry file names, SSIDs, …).
import test from 'node:test';
import assert from 'node:assert/strict';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const EN = JSON.parse((await import('node:fs')).readFileSync(new URL('../../apps/system-monitor/www/i18n.en.json', import.meta.url), 'utf8'));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Standalone (not embedded): Overview fetches /api/status itself. To feed it EXACT snapshots, the device's
// /api/status is faulted and the snapshot arrives the way the shell delivers it (a status.snapshot message).
async function open(browser, sim) {
  const page = await browser.newPage();
  await bootShell(page, sim, { lang: 'en', wait: false });
  await page.goto(`${sim.origin}/apps/system-monitor/`);
  // ready = the module ran (it paints the CPU placeholder rows only after I18N.init resolved)
  assert.ok(await page.waitFor(`document.querySelector('[data-tab="overview"]').textContent === ${JSON.stringify(EN.tab_overview)}`, { timeout: 45000 }), 'app started');
  return page;
}
const OVERVIEW = `Object.fromEntries(['o_os','o_up','o_heap','o_bat','o_apps','o_sd','o_sd_sub','o_net','o_net_sub'].map((id) => [id, document.getElementById(id).textContent]))`;
const post = (msg) => `window.postMessage(${JSON.stringify(msg)}, '*')`;
const exceptions = (page, m) => page.since(m).filter((e) => e.kind === 'exception').map((e) => String(e.text).split('\n')[0]);

const SNAP = {
  os: 'NucleoOS', version: '1.2.3', uptime_s: 3725, free_heap: 74000,
  battery: { pct: 87, mv: 4012 }, apps: { installed: 12 },
  storage: { mounted: true, fs: 'FAT32', total_bytes: 8e9, free_bytes: 6e9 },
  network: { ip: '192.168.4.7', mode: 'sta', ssid: 'Casa' },
};

test('system-monitor: Overview renders a status snapshot; partial or malformed snapshots never show garbage or throw', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  await sim.control('/api/_sim/fault', { route: '/api/status', status: 503 });   // only OUR snapshots reach the page
  const page = await open(browser, sim);

  await t.test('a full snapshot', async () => {
    const m = page.mark();
    const v = await page.waitFor(`(${post({ t: 'status.snapshot', d: SNAP })}, document.getElementById('o_up').textContent === '1h 2m' && ${OVERVIEW})`, { timeout: 10000 });
    assert.ok(v, 'the snapshot was rendered');
    assert.deepEqual(v, {
      o_os: 'NucleoOS v1.2.3', o_up: '1h 2m', o_heap: '74 KB', o_bat: '87% · 4.01V', o_apps: '12',
      o_sd: EN.sd_free.replace('{size}', '6.00 GB'), o_sd_sub: 'FAT32 · 2.00 GB / 8.00 GB', o_net: '192.168.4.7', o_net_sub: 'STA · Casa',
    });
    assert.equal(await page.eval(`document.getElementById('o_sdbar').style.width`), '25%', 'storage bar = used share');
    assert.deepEqual(exceptions(page, m), []);
  });

  await t.test('a partial / malformed snapshot: no "undefined", no "NaN", no fake "0 B" heap, no exception', async () => {
    const bad = [
      {},                                                              // nothing at all
      { os: 'NucleoOS' },                                              // no version
      { battery: null, storage: null, network: null, apps: null },     // explicit nulls
      { battery: { pct: -1 }, storage: { mounted: false, error_name: 'ESP_ERR_NOT_FOUND' }, uptime_s: 'abc', free_heap: 'lots' },
      { storage: { mounted: true } },                                  // mounted, but no sizes / fs name
      null,                                                            // a broken broadcast
    ];
    for (const d of bad) {
      const m = page.mark();
      // reset to a known-good state first, so each case proves its own rendering
      assert.ok(await page.waitFor(`(${post({ t: 'status.snapshot', d: SNAP })}, document.getElementById('o_up').textContent === '1h 2m')`, { timeout: 10000 }));
      await page.eval(`${post({ t: 'status.snapshot', d })}, true`);
      await sleep(300);
      const v = await page.eval(OVERVIEW);
      const text = Object.values(v).join(' | ');
      assert.doesNotMatch(text, /undefined|NaN|null|\[object/, `snapshot ${JSON.stringify(d)} rendered garbage: ${text}`);
      assert.deepEqual(exceptions(page, m), [], `snapshot ${JSON.stringify(d)} threw`);
      if (d && !('free_heap' in d && typeof d.free_heap === 'number')) assert.notEqual(v.o_heap, '0 B', `an unknown heap must not read as "0 B free" (${JSON.stringify(d)})`);
    }
    // the partial-but-valid fields that ARE present still render
    await page.eval(`${post({ t: 'status.snapshot', d: { battery: { pct: -1 }, storage: { mounted: false, error_name: 'ESP_ERR_NOT_FOUND' } } })}, true`);
    await sleep(300);
    const v = await page.eval(OVERVIEW);
    assert.equal(v.o_bat, '—', 'battery pct -1 = no battery reading');
    assert.equal(v.o_sd, EN.not_mounted);
    assert.equal(v.o_sd_sub, 'ESP_ERR_NOT_FOUND', 'the mount error is shown');
  });
  await sim.control('/api/_sim/fault', { clear: true });
});

test('system-monitor: RAM and CPU tabs render the device numbers; an offline device degrades without exceptions', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);
  const m = page.mark();
  const tab = (k) => `document.dispatchEvent(new KeyboardEvent('keydown', { key: ${JSON.stringify(k)}, bubbles: true })), true`;
  const visible = `[...document.querySelectorAll('.page')].filter((p) => !p.hidden).map((p) => p.dataset.page).join()`;

  await page.eval(tab('Tab'));
  assert.equal(await page.eval(visible), 'ram', 'Tab → RAM');
  const ram = await page.waitFor(`document.getElementById('r_total').textContent !== '—' && ['r_total','r_free','r_used','r_pct','r_frag'].map((id) => document.getElementById(id).textContent)`, { timeout: 15000 });
  assert.ok(ram, 'RAM rendered from /api/heap');
  assert.equal(ram[0], '328 KB', 'the internal SRAM total (327680 B)');
  assert.match(ram[1], /^\d+ KB$/); assert.match(ram[2], /^\d+ KB used$/); assert.match(ram[3], /^\d+\.\d%$/); assert.match(ram[4], /^\d+%$/);

  await page.eval(tab('Tab'));
  assert.equal(await page.eval(visible), 'cpu', 'Tab → CPU');
  const cpu = await page.waitFor(`document.querySelectorAll('#c_cores .core').length === 2 && document.getElementById('c_freq').textContent !== '—' && [document.getElementById('c_freq').textContent, document.getElementById('c_avg').textContent, [...document.querySelectorAll('#c_cores .core .lbl')].map((e) => e.textContent)]`, { timeout: 15000 });
  assert.ok(cpu, 'CPU rendered from /api/cpu');
  assert.equal(cpu[0], '240 MHz');
  assert.match(cpu[1], /^\d+\.\d%$/);
  assert.deepEqual(cpu[2], [EN.core_n.replace('{n}', '0'), EN.core_n.replace('{n}', '1')], 'one row per core');
  await page.eval(tab('Tab'));
  assert.equal(await page.eval(visible), 'overview', 'Tab wraps to Overview');
  await page.eval(`${tab('Tab')}; ${tab('Tab')}`);                     // back to CPU

  await sim.control('/api/_sim/offline', { on: true });
  const na = await page.waitFor(`document.getElementById('c_avg').textContent === ${JSON.stringify(EN.na_short)}`, { timeout: 15000 });
  await page.eval(tab('Tab'));                                         // Overview, then RAM, while offline
  await page.eval(tab('Tab'));
  await sleep(2500);
  await sim.control('/api/_sim/offline', { on: false });
  assert.ok(na, 'CPU says n/a when the device does not answer');
  assert.equal(await page.eval(`document.getElementById('r_total').textContent`), '328 KB', 'RAM keeps the last good reading');
  assert.deepEqual(exceptions(page, m), [], 'no uncaught exception while the device was gone');
});

test('system-monitor: the live event feed shows event names and payloads as text', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);
  // What the shell forwards from the device bus: payloads carry file names, Wi-Fi SSIDs, app names…
  const evil = { t: 'wifi.scan', seq: 7, d: { ssid: '<img src=x onerror="window.__xss=1">' } };
  const evilType = { t: '<b id="bold">fs.changed</b>', d: { path: '/data/a&amp;b.txt' } };
  assert.ok(await page.waitFor(`(${post(evil)}, document.querySelectorAll('#feed > div').length > 0)`, { timeout: 10000 }), 'the event reached the feed');
  await page.eval(`${post(evilType)}, true`);
  await page.waitFor(`document.querySelectorAll('#feed > div').length >= 2`, { timeout: 5000 });
  await sleep(500);                                                    // give an injected <img onerror> time to fire
  assert.equal(await page.eval(`document.querySelectorAll('#feed img, #feed b').length`), 0, 'event data must never become markup');
  assert.equal(await page.eval(`window.__xss`), undefined, 'an event payload ran script');
  const lines = await page.eval(`[...document.querySelectorAll('#feed > div')].map((d) => d.textContent)`);
  // Lines are deduplicated by count only: the feed may hold more events the page itself raised meanwhile.
  assert.ok(lines.some((l) => l.includes('#7') && l.includes('wifi.scan') && l.includes(JSON.stringify(evil.d))), 'the payload is shown literally: ' + JSON.stringify(lines));
  assert.ok(lines.some((l) => l.includes('<b id="bold">fs.changed</b>') && l.includes('a&amp;b.txt')), 'the event name and an &amp; in a path stay literal: ' + JSON.stringify(lines));
  // The feed is bounded: 100 more events keep at most 80 lines.
  await page.eval(`for (let i = 0; i < 100; i++) window.postMessage({ t: 'tick', seq: i, d: { i } }, '*'); true`);
  await page.waitFor(`[...document.querySelectorAll('#feed > div')][0].textContent.includes('#99')`, { timeout: 10000 });
  assert.equal(await page.eval(`document.querySelectorAll('#feed > div').length`), 80);
});
