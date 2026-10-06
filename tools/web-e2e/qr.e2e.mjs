// Browser E2E — QR app against the simulator. What matters is the code ON SCREEN: the canvas is sampled
// module by module and must equal NucleoQR.generate() of exactly the string the form should produce (URL
// scheme added, Wi-Fi special characters escaped, the ECC level picked). tools/qr-encoder.test.mjs proves
// that generate() output decodes back to its input, so together: what the user sees scans to what they typed.
// Also: a too-long text disables saving, and "Save to device" adds to the library on the SD (names as text).
import test from 'node:test';
import assert from 'node:assert/strict';
import vm from 'node:vm';
import { readFileSync } from 'node:fs';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const EN = JSON.parse(readFileSync(new URL('../../apps/qr/www/i18n.en.json', import.meta.url), 'utf8'));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const QR = (() => { const sb = { TextEncoder }; sb.window = sb; vm.runInNewContext(readFileSync(new URL('../../apps/qr/www/qrcode.js', import.meta.url), 'utf8'), sb); return sb.NucleoQR; })();

// The expected module grid as a string of 0/1, row by row.
function expected(text, ecc) {
  const q = QR.generate(text, { ecc });
  let s = ''; for (let y = 0; y < q.size; y++) for (let x = 0; x < q.size; x++) s += q.get(x, y) ? '1' : '0';
  return { size: q.size, bits: s };
}
// Sample the app's canvas at the centre of every module (same geometry as the app's drawQR: 4-module quiet zone).
const onScreen = (size) => `(() => {
  const cv = document.getElementById('qr'), px = cv.width, ctx = cv.getContext('2d');
  const n = ${size} + 8, scale = Math.floor(px / n) || 1, off = ((px - scale * n) / 2) | 0;
  const img = ctx.getImageData(0, 0, px, px).data; let s = '', quietDark = 0;
  const dark = (X, Y) => img[(Y * px + X) * 4] < 128;
  for (let y = 0; y < ${size}; y++) for (let x = 0; x < ${size}; x++) s += dark(off + (x + 4) * scale + (scale >> 1), off + (y + 4) * scale + (scale >> 1)) ? '1' : '0';
  for (let i = 0; i < n; i++) { if (dark(off + i * scale + 1, off + 1)) quietDark++; if (dark(off + 1, off + i * scale + 1)) quietDark++; }
  return { bits: s, quietDark };
})()`;
const type = (k, v) => `(() => { const el = document.querySelector('#form [data-k="${k}"]'); if (el.type === 'checkbox') { el.checked = ${JSON.stringify(!!v)}; el.dispatchEvent(new Event('change', { bubbles: true })); } else { el.value = ${JSON.stringify(v)}; el.dispatchEvent(new Event('input', { bubbles: true })); } return true; })()`;
const setEcc = (e) => `(() => { const s = document.getElementById('ecc'); s.value = '${e}'; s.dispatchEvent(new Event('change')); return true; })()`;

async function open(browser, sim) {
  const page = await browser.newPage();
  await bootShell(page, sim, { lang: 'en', wait: false });
  await page.goto(`${sim.origin}/apps/qr/`);
  assert.ok(await page.waitFor(`document.querySelectorAll('#types .chip').length === 8 && document.querySelector('#form [data-k]')`, { timeout: 45000 }), 'app started');
  await page.networkIdle({ quiet: 400, timeout: 10000 });
  return page;
}
async function assertScreen(page, text, ecc, what) {
  const e = expected(text, ecc);
  const got = await page.eval(onScreen(e.size));
  assert.equal(got.quietDark, 0, `${what}: the quiet zone must be white`);
  assert.equal(got.bits, e.bits, `${what}: the symbol on screen is not the QR for ${JSON.stringify(text)} at ECC ${ecc}`);
}

test('qr: the code on screen encodes exactly what the form says', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);

  await assertScreen(page, 'https://nucleo.os', 0, 'the default link');
  await page.eval(type('url', 'nucleo.test/x?a=1&b=2'));
  await assertScreen(page, 'https://nucleo.test/x?a=1&b=2', 0, 'a bare host gets https://');
  await page.eval(setEcc(2));
  await assertScreen(page, 'https://nucleo.test/x?a=1&b=2', 2, 'after switching to Quartile');
  assert.match(await page.eval(`document.getElementById('meta').textContent`), /· Q ·/, 'the meta line names the level');
  await page.eval(type('url', 'ftp://files.example.org'));
  await assertScreen(page, 'ftp://files.example.org', 2, 'an explicit scheme is kept');

  await page.eval(`document.querySelector('#types [data-t="wifi"]').click(), true`);
  await page.eval(type('ssid', 'My;Net,5G'));
  await page.eval(type('pass', 'p:w"d\\x'));
  await page.eval(type('hidden', true));
  await assertScreen(page, 'WIFI:T:WPA;S:My\\;Net\\,5G;P:p\\:w\\"d\\\\x;H:true;;', 2, 'Wi-Fi with special characters escaped');

  await page.eval(`document.querySelector('#types [data-t="tel"]').click(), true`);
  await page.eval(type('num', '+39 06 1234 5678'));
  await assertScreen(page, 'tel:+390612345678', 2, 'a phone number without spaces');

  await page.eval(`document.querySelector('#types [data-t="text"]').click(), true`);
  await page.eval(type('text', 'x'.repeat(3000)));
  assert.equal(await page.eval(`document.getElementById('meta').textContent`), EN.too_long, 'a text over the capacity says so');
  assert.deepEqual(await page.eval(`['save','png','svg'].map((id) => document.getElementById(id).disabled)`), [true, true, true], 'and nothing broken can be saved/exported');
  await page.eval(type('text', ''));
  assert.equal(await page.eval(`document.getElementById('meta').textContent`), '—', 'an empty form draws nothing');
  assert.equal(await page.eval(`document.getElementById('save').disabled`), true);
});

test('qr: "Save to device" adds the shown code to the SD library and it loads back identically', { skip }, async (t) => {
  const LIB = { schema: 1, items: [{ id: 'q0', label: '<img src=x onerror="window.__xss=1">', type: 'text', data: 'from the Cardputer', ts: 1 }] };
  const sim = await startSim({ seed: { '/data/QR/qrcodes.json': LIB } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);
  assert.ok(await page.waitFor(`document.querySelectorAll('#savedList .item').length === 1`, { timeout: 10000 }), 'the existing library is listed');
  await sleep(300);
  assert.equal(await page.eval(`document.querySelectorAll('#savedList img').length`), 0, 'a saved label is text, never markup');
  assert.equal(await page.eval(`window.__xss`), undefined);
  assert.equal(await page.eval(`document.querySelector('#savedList .item .n').textContent`), LIB.items[0].label);

  await page.eval(type('url', 'nucleo.test/saved'));
  await page.eval(setEcc(1));
  await page.eval(`document.getElementById('save').click(), true`);
  assert.ok(await page.waitFor(`document.querySelectorAll('#savedList .item').length === 2`, { timeout: 10000 }), 'the new code is listed');
  const saved = JSON.parse(await sim.readSd('/data/QR/qrcodes.json'));
  assert.equal(saved.items.length, 2, 'added, nothing overwritten');
  assert.deepEqual({ type: saved.items[0].type, data: saved.items[0].data, ecc: saved.items[0].ecc, label: saved.items[0].label },
    { type: 'link', data: 'https://nucleo.test/saved', ecc: 1, label: 'nucleo.test/saved' }, 'the newest first, with exactly the encoded data');
  assert.deepEqual(saved.items[1], LIB.items[0], 'the device-side item is untouched');

  // Switch away, then load the saved one back: same form value, same symbol on screen.
  await page.eval(`document.querySelector('#types [data-t="geo"]').click(), true`);
  await page.eval(`document.querySelector('#savedList [data-load="0"]').click(), true`);
  assert.equal(await page.eval(`document.querySelector('#form [data-k="url"]').value`), 'https://nucleo.test/saved');
  assert.equal(await page.eval(`document.querySelector('#types .chip.on').dataset.t`), 'link');
  await assertScreen(page, 'https://nucleo.test/saved', 1, 'a loaded code');
});
