// Browser E2E — Updates app: the firmware OTA upload against the simulator's POST /api/ota (mirror of
// firmware nucleo_httpd.c ota_post). What a user must be able to trust:
//   • only a plausible OTA image is ever sent — wrong extension, empty file, not an ESP image (a renamed
//     download error page), the merged FULL-flash image (bootloader + partition table: docs/update-check.md
//     says it must never reach /api/ota) or a file larger than the OTA slot are refused BEFORE uploading
//     (every upload first switches the device into its OTA RAM posture — Remote Control, voice suspended);
//   • a valid image goes raw, POST, to /api/ota, with progress, and "complete" only after a 2xx;
//   • a refusal shows the device's own reason (readable, not raw JSON), never success;
//   • a connection lost mid-upload is NOT reported as an installed update unless the device really rebooted.
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
const fmt = (s, a) => String(s).replace(/\{(\w+)\}/g, (m, k) => (k in a ? String(a[k]) : m));
const SLOT = 0x380000;                                   // ota_0 / ota_1 size, firmware/partitions.csv

// Record every XHR (method, url, body size, response) and every status message the page shows.
const RECORD = `(() => {
  if (!location.pathname.startsWith('/apps/updates')) return;
  window.__xhr = []; window.__msgs = [];
  const open = XMLHttpRequest.prototype.open, send = XMLHttpRequest.prototype.send;
  XMLHttpRequest.prototype.open = function (m, u) { this.__rec = { method: m, url: String(u) }; return open.apply(this, arguments); };
  XMLHttpRequest.prototype.send = function (body) {
    const rec = { ...this.__rec, size: body && body.size != null ? body.size : (body ? String(body).length : 0), status: null, resp: null };
    window.__xhr.push(rec);
    this.addEventListener('loadend', () => { rec.status = this.status; rec.resp = this.responseText; });
    return send.apply(this, arguments);
  };
  addEventListener('DOMContentLoaded', () => {
    const m = document.getElementById('msg');
    new MutationObserver(() => { const last = window.__msgs[window.__msgs.length - 1];
      const cur = { text: m.textContent, cls: m.className, installDisabled: document.getElementById('install').disabled,
        binDisabled: document.getElementById('bin').disabled, bar: document.getElementById('bar').style.display };
      if (!last || last.text !== cur.text) window.__msgs.push(cur); }).observe(m, { childList: true, characterData: true, subtree: true });
  });
})()`;

async function openApp(browser, sim, lang = 'en') {
  const page = await browser.newPage();
  await page.initScript(RECORD);
  await bootShell(page, sim, { lang, wait: false });
  await page.goto(`${sim.origin}/apps/updates/`);
  await page.networkIdle({ quiet: 500, timeout: 20000 });
  await page.waitFor(`document.getElementById('slot').textContent !== '…'`, { timeout: 30000 });
  return page;
}
// Put a synthetic file into the <input type=file> the way a picker does, then click Install.
// kind: 'ok' (0xE9 app image) | 'zero' | 'html' (a saved error page) | 'merged' (full-flash image).
const pick = (page, name, size, kind = 'ok') => page.eval(`(() => {
  const b = new Uint8Array(${size});
  if (${JSON.stringify(kind)} === 'ok' || ${JSON.stringify(kind)} === 'merged') b[0] = 0xE9;
  if (${JSON.stringify(kind)} === 'html') b.set(new TextEncoder().encode('<!doctype html><title>404</title>').slice(0, b.length));
  if (${JSON.stringify(kind)} === 'merged') { b[0x8000] = 0xAA; b[0x8001] = 0x50; b[0x8020] = 0xAA; b[0x8021] = 0x50; }
  const dt = new DataTransfer(); dt.items.add(new File([b], ${JSON.stringify(name)}, { type: 'application/octet-stream' }));
  const inp = document.getElementById('bin'); inp.files = dt.files; inp.dispatchEvent(new Event('change'));
  return true; })()`);
const install = (page) => page.eval(`document.getElementById('install').click(), true`);
const msg = (page) => page.eval(`({ text: document.getElementById('msg').textContent, cls: document.getElementById('msg').className })`);
const otaHits = async (sim) => (await sim.control('/api/_sim/stats')).byPath['/api/ota'] || 0;

test('updates: only a plausible OTA image is ever uploaded — refused before any byte is sent', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const en = cat('updates', 'en');
  const page = await openApp(browser, sim, 'en');
  const cases = [
    ['not a .bin (firmware.zip)', 'firmware.zip', 4096, 'ok', 'err_pick_bin'],
    ['an empty .bin', 'nucleoos.bin', 0, 'zero', 'err_empty'],
    ['a saved error page renamed .bin (no 0xE9 ESP image magic)', 'nucleoos.bin', 4096, 'html', 'err_not_image'],
    ['the merged full-flash image (bootloader + partition table)', 'nucleoos-latest.bin', 0x30000, 'merged', 'err_merged'],
    ['larger than the OTA slot', 'nucleoos.bin', SLOT + 1, 'ok', 'err_too_big'],
  ];
  await sim.control('/api/_sim/stats', { reset: true });
  for (const [what, name, size, kind, key] of cases) {
    await t.test(what, async () => {
      await pick(page, name, size, kind);
      await install(page);
      await sleep(400);
      const m = await msg(page);
      assert.ok(en[key], 'catalog key ' + key);
      assert.deepEqual(m, { text: fmt(en[key], { max: '3.5 MB' }), cls: 'status-msg err' });
      assert.equal(await page.eval(`window.__xhr.length`), 0, 'no upload was started');
      assert.equal(await page.eval(`document.getElementById('bin').disabled || document.getElementById('bar').style.display === 'block'`), false, 'the form is not stuck in "uploading"');
    });
  }
  assert.equal(await otaHits(sim), 0, 'the device never saw an OTA request');
  assert.equal(await page.eval(`document.getElementById('slot').textContent`), 'factory', 'still running the old image');
});

test('updates: a valid image is POSTed raw to /api/ota, with progress; "complete" only after the 2xx and the device is back', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const it = cat('updates', 'it');
  const page = await openApp(browser, sim, 'it');
  const N = 300 * 1024;
  await pick(page, 'nucleoos.bin', N);
  assert.equal(await page.eval(`document.getElementById('install').disabled`), false, 'picking a file enables Install');
  await install(page);
  assert.ok(await page.waitFor(`document.getElementById('msg').textContent === ${JSON.stringify(it.update_complete)}`, { timeout: 30000 }), 'the update completes');
  const x = await page.eval(`window.__xhr`);
  assert.equal(x.length, 1, 'exactly one upload');
  assert.deepEqual({ method: x[0].method, url: x[0].url, size: x[0].size, status: x[0].status }, { method: 'POST', url: '/api/ota', size: N, status: 200 });
  assert.equal(JSON.parse(x[0].resp).bytes, N, 'the device received every byte of the file, raw (no multipart wrapper)');
  const msgs = await page.eval(`window.__msgs`);
  const texts = msgs.map((m) => m.text);
  const iw = texts.indexOf(it.image_written);
  assert.ok(iw >= 0 && iw < texts.indexOf(it.update_complete), 'written (2xx) is reported before complete: ' + texts.join(' / '));
  const during = msgs.slice(0, iw + 1).filter((m) => m.text !== 'nucleoos.bin · 307 KB');
  assert.ok(during.length && during.every((m) => m.installDisabled && m.binDisabled && m.bar === 'block'), 'controls locked and the progress bar shown while uploading');
  const end = await page.eval(`({ slot: document.getElementById('slot').textContent, install: document.getElementById('install').disabled,
    bin: document.getElementById('bin').disabled, file: document.getElementById('bin').value, bar: document.getElementById('bar').style.display })`);
  assert.deepEqual(end, { slot: 'ota_0', install: true, bin: false, file: '', bar: 'none' }, 'status refreshed to the new slot; the form is reset');
});

test('updates: a refused upload shows the device\'s reason and is never reported as success', { skip }, async (t) => {
  const en = cat('updates', 'en');

  await t.test('device error (500): its reason is shown, the form is usable again, no "complete" ever', async () => {
    const sim = await startSim();
    const browser = await launchBrowser({ args: [HOST_RULES] });
    t.after(async () => { await browser.close(); await sim.stop(); });
    const page = await openApp(browser, sim, 'en');
    await sim.control('/api/_sim/fault', { route: '/api/ota', status: 500 });
    await pick(page, 'nucleoos.bin', 8192);
    await install(page);
    assert.ok(await page.waitFor(`document.getElementById('msg').className === 'status-msg err'`, { timeout: 15000 }));
    await sleep(2600);                                          // past the first reboot poll of a success
    assert.deepEqual(await msg(page), { text: fmt(en.update_rejected, { reason: 'sim fault' }), cls: 'status-msg err' }, 'the device\'s error text, not raw JSON');
    const texts = (await page.eval(`window.__msgs`)).map((m) => m.text);
    for (const k of ['image_written', 'update_complete', 'upload_done_rebooting', 'rebooting_short']) assert.ok(!texts.includes(en[k]), 'never claimed: ' + k);
    assert.deepEqual(await page.eval(`[document.getElementById('install').disabled, document.getElementById('bin').disabled, document.getElementById('bar').style.display]`),
      [false, false, 'none'], 'ready for a retry with the same file');
    assert.equal(await page.eval(`document.getElementById('slot').textContent`), 'factory');
  });

  await t.test('device busy (503 single-flight): a readable "busy" reason', async () => {
    const sim = await startSim();
    const browser = await launchBrowser({ args: [HOST_RULES] });
    t.after(async () => { await browser.close(); await sim.stop(); });
    const page = await openApp(browser, sim, 'en');
    await sim.control('/api/_sim/fault', { route: '/api/ota', status: 503 });
    await pick(page, 'nucleoos.bin', 8192);
    await install(page);
    assert.ok(await page.waitFor(`document.getElementById('msg').className === 'status-msg err'`, { timeout: 15000 }));
    assert.equal((await msg(page)).text, fmt(en.update_rejected, { reason: en.err_busy || '(no err_busy string)' }));
  });

  await t.test('installed by M5Launcher (409): the device\'s own explanation, not {"error":"hosted",…}', async () => {
    process.env.SIM_GUEST = '1';
    const sim = await startSim().finally(() => { delete process.env.SIM_GUEST; });
    const browser = await launchBrowser({ args: [HOST_RULES] });
    t.after(async () => { await browser.close(); await sim.stop(); });
    const page = await openApp(browser, sim, 'en');
    await pick(page, 'nucleoos.bin', 8192);
    await install(page);
    assert.ok(await page.waitFor(`document.getElementById('msg').className === 'status-msg err'`, { timeout: 15000 }));
    assert.equal((await msg(page)).text, fmt(en.update_rejected, { reason: 'Installed by M5Launcher: update NucleoOS from the Launcher (OTA)' }));
  });
});

test('updates: a connection lost mid-upload is never reported as an installed update when the device did not reboot', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const en = cat('updates', 'en');
  const page = await openApp(browser, sim, 'en');
  // Every /api/ota connection is cut (what an early device-side abort or a WiFi blip looks like to XHR).
  await sim.control('/api/_sim/fault', { route: '/api/ota', drop: true });
  await pick(page, 'nucleoos.bin', 64 * 1024);
  await install(page);
  assert.ok(await page.waitFor(`window.__xhr.length && window.__xhr[0].status !== null`, { timeout: 30000 }), 'the upload ended');
  await sim.control('/api/_sim/fault', { clear: true });
  // The device is still up — it never rebooted (uptime keeps growing): the update was NOT applied.
  const settled = await page.waitFor(`(() => { const m = document.getElementById('msg');
    return /status-msg (ok|err)$/.test(m.className) && !${JSON.stringify([en.upload_done_rebooting, en.rebooting_short])}.includes(m.textContent) && m.textContent; })()`, { timeout: 20000 });
  assert.notEqual(settled, en.update_complete, 'reported "Update complete" although nothing was installed');
  assert.equal(settled, en.not_applied || '(no not_applied string)');
  assert.equal(await page.eval(`document.getElementById('msg').className`), 'status-msg err');
  assert.equal(await page.eval(`document.getElementById('slot').textContent`), 'factory', 'still the old image');
  assert.equal(await page.eval(`document.getElementById('install').disabled`), false, 'the same file can be retried');
});
