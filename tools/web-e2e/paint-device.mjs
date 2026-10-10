// Paint on a REAL Cardputer, in the web OS: the natural-language command bar in the five OS languages — judged on the
// CANVAS (the pixel the command must change), not on the reply text — and the Atelier (AI generation): which engines it
// offers on the device's plain-http origin and why, and that a generation really lands on the canvas.
//
// Read-only towards the user's files: nothing is saved (no "save" command), the window session and the UI language are
// restored. --overlay serves the web files from the working tree (tools/web-e2e/overlay.mjs).
//
//   node tools/web-e2e/paint-device.mjs [--host 192.168.0.166] [--overlay]
import { readFileSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';
import { launchBrowser, findChrome } from './cdp.mjs';
import { waitDesktop } from './shell.mjs';
import { REPO } from './sim.mjs';
import { enableOverlay } from './overlay.mjs';

const arg = (k, d) => { const i = process.argv.indexOf('--' + k); return i > 0 ? process.argv[i + 1] : d; };
const overlay = process.argv.includes('--overlay');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const OUT = join(REPO, 'build', 'web-e2e', 'paint-device');
mkdirSync(OUT, { recursive: true });
const cfg = JSON.parse(readFileSync(join(REPO, 'tools', 'release.local.json'), 'utf8').replace(/^﻿/, ''));
const host = String(arg('host', cfg.host)).replace(/^https?:\/\//, '');
const unit = (cfg.devices || []).find((d) => d.host === host) || (cfg.host === host ? cfg : null);
if (!unit || !unit.pin) { console.error(`no PIN for ${host}`); process.exit(2); }
if (!findChrome()) { console.error('no Chrome/Edge installed'); process.exit(2); }
const origin = 'http://' + host, pin = String(unit.pin);
const SESSION = '/system/config/session.json';

let cookie = '';
const pairNode = async () => { const r = await fetch(origin + '/api/pair', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ pin }) }); cookie = (r.headers.get('set-cookie') || '').split(';')[0]; };
const sdRead = async (p) => { const r = await fetch(origin + '/api/fs/read?path=' + encodeURIComponent(p), { headers: { cookie } }); return r.ok ? r.text() : null; };
const sdWrite = (p, body) => fetch(origin + '/api/fs/write?path=' + encodeURIComponent(p), { method: 'POST', headers: { cookie }, body }).then((r) => r.ok);

const PW = `[...document.querySelectorAll('.win iframe')].find((f) => /\\/apps\\/paint\\//.test(f.src))`;
const IN_PAINT = (body) => `(async () => { const f = ${PW}; if (!f) throw new Error('no Paint window'); const w = f.contentWindow, d = f.contentDocument; ${body} })()`;
// the colour at the canvas centre, and the number of layers
// (Paint's global "c" is the ACTIVE layer; undo/redo rebuild the layer canvases, so the id "c" does not survive them)
const PIXEL = IN_PAINT(`const c = w.eval('c'); const p = c.getContext('2d').getImageData(c.width >> 1, c.height >> 1, 1, 1).data; return [p[0], p[1], p[2]];`);

// [lang, command, check(pixelBefore, pixelAfter) → true | message, reply regex]
const near = (a, b, tol = 40) => a.every((v, i) => Math.abs(v - b[i]) <= tol);
const gray = (p) => Math.abs(p[0] - p[1]) < 12 && Math.abs(p[1] - p[2]) < 12;
const CASES = [
  ['it', 'riempi di rosso', (b, a) => near(a, [237, 28, 36]) || `not red: ${a}`, /riempio/i],
  ['it', 'inverti i colori', (b, a) => near(a, [255 - b[0], 255 - b[1], 255 - b[2]]) || `not inverted: ${b} → ${a}`, /invert/i],
  ['it', 'annulla', (b, a) => near(a, [237, 28, 36]) || `undo did not bring red back: ${a}`, /annull/i],
  ['en', 'fill with blue', (b, a) => near(a, [0, 162, 232]) || `not blue: ${a}`, /fill/i],
  ['en', 'grayscale', (b, a) => gray(a) || `not gray: ${a}`, /gray/i],
  ['es', 'rellena de verde', (b, a) => near(a, [34, 177, 76]) || `not green: ${a}`, /relleno/i],
  ['es', 'invierte los colores', (b, a) => near(a, [255 - b[0], 255 - b[1], 255 - b[2]]) || `not inverted: ${b} → ${a}`, /invierto/i],
  ['fr', 'remplis de jaune', (b, a) => near(a, [255, 242, 0]) || `not yellow: ${a}`, /remplis/i],
  ['fr', 'noir et blanc', (b, a) => gray(a) || `not gray: ${a}`, /gris/i],
  ['de', 'fülle mit rot', (b, a) => near(a, [237, 28, 36]) || `not red: ${a}`, /fülle/i],
  ['de', 'Graustufen', (b, a) => gray(a) || `not gray: ${a}`, /graustufen/i],
  ['de', 'rückgängig', (b, a) => near(a, [237, 28, 36]) || `undo did not bring red back: ${a}`, /rückgängig/i],
];

const browser = await launchBrowser({ args: ['--disable-features=LocalNetworkAccessChecks'] });
let exitCode = 0, sessionBefore = null;
const fails = [];
try {
  await pairNode();
  sessionBefore = await sdRead(SESSION);
  const page = await browser.newPage();
  await page.setViewport(1440, 900);
  await page.goto(origin + '/api/status');
  await page.eval(`(async () => { localStorage.setItem('anima.lang', 'it'); localStorage.setItem('nucleo.onboarded', '1'); localStorage.setItem('paint.tourDone', '1');
    await fetch('/api/pair', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ pin: ${JSON.stringify(pin)} }) }); })()`);
  await page.goto(origin + '/');
  if (!(await waitDesktop(page, 120000))) throw new Error('desktop never came up');
  await page.networkIdle({ quiet: 1500, timeout: 45000 }).catch(() => {});
  if (overlay) { await enableOverlay(browser, page, origin); await page.goto(origin + '/'); await waitDesktop(page, 120000); await page.networkIdle({ quiet: 1500, timeout: 45000 }).catch(() => {}); console.log('overlay on'); }
  await page.eval(`document.querySelectorAll('.win button.close').forEach((b) => b.click())`).catch(() => {});
  const idx = await page.eval(`[...document.querySelectorAll('#sm-all .sm-row')].findIndex((r) => /paint/i.test(r.title))`);
  if (idx < 0) throw new Error('Paint is not in the Start menu');
  await page.eval(`document.querySelectorAll('#sm-all .sm-row')[${idx}].click()`);
  if (!(await page.waitFor(IN_PAINT(`return !!(d && d.readyState === 'complete' && d.getElementById('cmdInput') && d.getElementById('c'));`), { timeout: 90000, interval: 500 }))) throw new Error('Paint did not load');
  await sleep(2500);
  // a tour or dialog left open would swallow the commands
  await page.eval(IN_PAINT(`d.querySelectorAll('.tour-skip, .tour-close, [data-tour-close]').forEach((b) => b.click()); return true;`)).catch(() => {});
  console.log('Paint open\n');

  for (const [lang, cmd, check, replyRe] of CASES) {
    const before = await page.eval(PIXEL);
    await page.eval(IN_PAINT(`localStorage.setItem('anima.lang', ${JSON.stringify(lang)}); const i = d.getElementById('cmdInput'); i.value = ${JSON.stringify(cmd)};
      i.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })); return true;`));
    await sleep(900);
    const after = await page.eval(PIXEL);
    const reply = await page.eval(IN_PAINT(`return (d.getElementById('cmdResp') || {}).textContent || '';`));
    const v = check(before, after), ok = v === true && replyRe.test(reply);
    if (!ok) { fails.push(`${lang} "${cmd}": ${v === true ? 'reply "' + reply + '" not in ' + lang : v} (reply: ${reply})`); exitCode = 1; }
    console.log(`${ok ? '✓' : '✗'} ${lang} ${cmd.padEnd(22)} ${String(before).padEnd(12)} → ${String(after).padEnd(12)} « ${reply} »${v === true ? '' : '  ✗ ' + v}`);
  }
  await page.eval(IN_PAINT(`localStorage.setItem('anima.lang', 'it'); return true;`));

  // Atelier: open it, list the engines it offers here and why
  await page.eval(IN_PAINT(`d.getElementById('atelierBtn').click(); return true;`));
  await sleep(4000);
  const at = await page.eval(IN_PAINT(`const s = d.getElementById('atlProvider');
    return { open: !!(d.getElementById('atlStudio') && d.getElementById('atlStudio').offsetParent) || !!(d.getElementById('atlManager') && d.getElementById('atlManager').offsetParent),
      providers: s ? [...s.options].map((o) => (o.disabled ? '✗ ' : '✓ ') + o.textContent.trim()) : [], note: (d.getElementById('atlProvNote') || {}).textContent || '',
      banner: (d.getElementById('atlBanner') || {}).textContent || '', manager: (d.getElementById('atlManager') || {}).textContent ? d.getElementById('atlManager').textContent.replace(/\\s+/g, ' ').slice(0, 400) : '' };`));
  console.log('\nAtelier:', JSON.stringify(at, null, 1));
  await page.screenshot(join(OUT, 'atelier.png'));
  if (!at.open) { fails.push('the Atelier did not open'); exitCode = 1; }
  // a generation with an engine that is available here (preview when nothing else is)
  const gen = await page.eval(IN_PAINT(`const s = d.getElementById('atlProvider'); if (!s) return { skipped: 'no provider select' };
    const avail = [...s.options].filter((o) => !o.disabled).map((o) => o.value); const pick = avail.includes('online') ? 'online' : avail.includes('local') ? 'local' : avail[0];
    if (!pick) return { skipped: 'no engine available' };
    s.value = pick; s.dispatchEvent(new Event('change', { bubbles: true }));
    const p = d.getElementById('atlPrompt'); if (p) { p.value = 'a red apple on a white table'; p.dispatchEvent(new Event('input', { bubbles: true })); }
    const sv = d.getElementById('atlSave'); if (sv && sv.checked) { sv.checked = false; sv.dispatchEvent(new Event('change', { bubbles: true })); }   // never write into the user's Pictures
    const c = w.eval('c'); w.__genBefore = c.toDataURL();
    d.getElementById('atlGo').click(); return { pick };`));
  console.log('generation:', JSON.stringify(gen));
  if (gen && !gen.skipped) {
    let done = null;
    for (let i = 0; i < 90 && !done; i++) {
      await sleep(2000);
      done = await page.eval(IN_PAINT(`const st = (d.getElementById('atlStatus') || {}).textContent || ''; const b = (d.getElementById('atlBanner') || {}).textContent || '';
        const v = w.eval('c').toDataURL() !== w.__genBefore;   // the generation landed on the canvas
        return (v || /errore|error|fallit|failed|non disponibile|unavailable/i.test(st + ' ' + b)) ? { st, v, b } : null;`)).catch(() => null);
    }
    console.log('generation result:', JSON.stringify(done));
    await page.screenshot(join(OUT, 'atelier-result.png'));
    if (!done || !done.v) { fails.push('the Atelier generation produced no image: ' + JSON.stringify(done)); exitCode = 1; }
  }
  console.log(`\n${fails.length ? fails.length + ' problem(s):\n  ' + fails.join('\n  ') : 'all good'}`);
} catch (e) {
  console.error('✗ ' + (e && e.stack || e)); exitCode = 1;
} finally {
  await browser.close();
  if (sessionBefore != null) { await pairNode().catch(() => {}); const ok = await sdWrite(SESSION, sessionBefore).catch(() => false); console.log(ok ? 'window session restored' : '⚠ could not restore the window session'); }
}
process.exit(exitCode);
