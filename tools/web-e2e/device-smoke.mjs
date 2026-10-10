// Smoke the web OS on a REAL Cardputer: boot the shell from the device's own address in headless Chrome,
// open EVERY app from the Start menu the way a user does, and report each one that throws, gets a failed
// HTTP answer from the device, opens blank or off-screen — with a screenshot under build/web-e2e/device/.
// The simulator suites (shell-smoke.e2e.mjs …) prove the code; this proves the CARD: what is really on the
// SD, the real httpd under its real heap, gzip, latency.
//
// Read-only towards the device's settings: it pairs (the PIN comes from tools/release.local.json, never
// from the command line) and opens apps; it does not change the device language or any config. Apps are
// opened one at a time with a pause, so the single-task httpd is never flooded.
//
//   node tools/web-e2e/device-smoke.mjs [--host 192.168.0.166] [--lang it] [--only settings,notes] [--pause 600] [--overlay]
// --overlay: the web payload from this repo's working tree, /api/* from the device (tools/web-e2e/overlay.mjs) — the
// reload pass runs with it, the first visit stays the device's own (its hand-off into the web profile).
import { readFileSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';
import { launchBrowser, findChrome } from './cdp.mjs';
import { waitDesktop, appRows, openAppAt, closeAllWindows, defects } from './shell.mjs';
import { REPO } from './sim.mjs';
import { enableOverlay, OVERLAY_ARGS } from './overlay.mjs';

const arg = (k, d) => { const i = process.argv.indexOf('--' + k); return i > 0 ? process.argv[i + 1] : d; };
const cfg = JSON.parse(readFileSync(join(REPO, 'tools', 'release.local.json'), 'utf8').replace(/^﻿/, ''));
const host = String(arg('host', cfg.host)).replace(/^https?:\/\//, '');
const unit = (cfg.devices || []).find((d) => d.host === host) || (cfg.host === host ? cfg : null);
if (!unit || !unit.pin) { console.error(`no PIN for ${host} in tools/release.local.json`); process.exit(2); }
const origin = 'http://' + host;
const lang = arg('lang', 'it');
const only = arg('only', '') ? arg('only').split(',').map((s) => s.trim().toLowerCase()) : null;
const pause = +arg('pause', 600);
const overlay = process.argv.includes('--overlay');
const SHOTS = join(REPO, 'build', 'web-e2e', 'device');
mkdirSync(SHOTS, { recursive: true });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

if (!findChrome()) { console.error('no Chrome/Edge installed'); process.exit(2); }
const status = await fetch(origin + '/api/status', { signal: AbortSignal.timeout(8000) }).then((r) => r.json()).catch(() => null);
if (!status) { console.error(`${origin} does not answer /api/status`); process.exit(2); }
console.log(`device ${origin}: v${status.version}, heap ${status.free_heap} B free (min ${status.min_free_heap}, block ${status.largest_free_block}), ${status.apps && status.apps.installed} apps`);

const SESSION = '/system/config/session.json';
// The newest window, now: how much text its app shows and which of its own files it holds (status 200).
const WIN_NOW = `(() => { const w = [...document.querySelectorAll('.win')].pop(); const f = w && w.querySelector('iframe');
  try { const d = f.contentDocument; return { bodyLen: d && d.body ? d.body.innerText.trim().length : 0,
    loaded: f.contentWindow.performance.getEntriesByType('resource').filter((e) => e.responseStatus === 200 && e.name.startsWith(location.origin)).map((e) => new URL(e.name).pathname) };
  } catch { return null; } })()`;
// With --overlay the document is served by Chrome itself, so Local Network Access would treat it as public and block
// its /api/* calls to the device's private address: switched off, as for a user who allowed it.
const browser = await launchBrowser(overlay ? { args: OVERLAY_ARGS } : {});
const report = [];
let exitCode = 0, sessionBefore = null;
try {
  const page = await browser.newPage();
  await page.setViewport(1366, 820);
  await page.goto(origin + '/api/status');                       // same-origin page: storage + the pairing cookie
  const paired = await page.eval(`(async () => {
    localStorage.setItem('anima.lang', ${JSON.stringify(lang)});
    localStorage.setItem('nucleo.onboarded', '1');
    const r = await fetch('/api/pair', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ pin: ${JSON.stringify(String(unit.pin))} }) });
    return r.status;
  })()`);
  if (paired !== 200) throw new Error('pairing refused (HTTP ' + paired + ') — check the PIN on the device');
  // The window session is the user's state: whatever the shell saves while apps open and close here is
  // put back exactly as it was once the run ends.
  sessionBefore = await page.eval(`fetch('/api/fs/read?path=' + encodeURIComponent(${JSON.stringify(SESSION)})).then((r) => r.ok ? r.text() : null)`);
  // Two loads: the FIRST visit (on the full OS the device hands off: a flash page, a warm reboot into the
  // "web" profile, then the shell) and a RELOAD (web profile, service worker warm) — what a user sees next.
  // Counted in the page (Resource Timing): the service worker fetches on the page's behalf, so CDP
  // network events miss most of them.
  const loads = [];
  for (const kind of ['first visit', 'reload']) {
    if (kind === 'reload' && overlay) { await enableOverlay(browser, page, origin); console.log('overlay on: web files from the working tree'); }
    const mark = page.mark();
    const t0 = Date.now();
    await page.goto(origin + '/');
    const up = await waitDesktop(page, 90000).catch(() => false);
    const ms = Date.now() - t0;
    await page.networkIdle({ quiet: 1200, timeout: 45000 }).catch(() => {});
    const res = await page.eval(`(() => { const r = performance.getEntriesByType('resource').filter((e) => e.name.startsWith(location.origin));
      return { n: r.length, kb: Math.round(r.reduce((s, e) => s + (e.transferSize || 0), 0) / 1024) }; })()`).catch(() => ({ n: '?', kb: '?' }));
    const bad = defects(page.since(mark), origin);
    loads.push({ kind, up, ms });
    console.log(`${kind}: ${up ? 'desktop up' : 'DESKTOP NEVER CAME UP'} in ${(ms / 1000).toFixed(1)} s · ${res.n} requests, ${res.kb} KB over the wire · ${bad.length} defect(s)`);
    for (const d of bad) console.log('   ✗ ' + d);
    if (!up) { await page.screenshot(join(SHOTS, `${lang}-boot.png`)); throw new Error('desktop never came up'); }
    if (bad.length) { exitCode = 1; await page.screenshot(join(SHOTS, `${lang}-${kind.replace(' ', '-')}.png`)); }
  }

  const names = await appRows(page);
  console.log(`Start menu: ${names.length} apps`);
  for (let i = 0; i < names.length; i++) {
    const name = String(names[i]).replace(/­/g, '');
    if (only && !only.some((o) => name.toLowerCase().includes(o))) continue;
    const t1 = Date.now();
    let r, bad = [];
    let recovered = [];
    try {
      r = await openAppAt(page, i);
      bad = defects(r.events, origin);
      if (!r.info.opened) bad.push('window did not open');
      else if (r.info.bodyLen === 0) bad.push('window is blank');
      if (r.info.offscreen) bad.push('window opened off-screen');
      // The shell reloads a window that lost one of its own resources while loading (wm.js retryFrame): give it
      // time, then re-measure. A lost file that the window now holds, or a blank window that filled in, is a
      // RECOVERY — reported apart, so the resets stay visible without being counted as broken apps.
      if (r.info.opened && bad.length) {
        for (let k = 0; k < 5 && bad.length; k++) {
          await sleep(2000);
          const now = await page.eval(WIN_NOW).catch(() => null);
          if (!now) break;
          bad = bad.filter((d) => {
            const lost = /^FAILED \S+ (\S+)/.exec(d) || /^HTTP 5\d\d (\S+)/.exec(d);
            const path = lost && lost[1].replace(/^https?:\/\/[^/]+/, '').split('?')[0];
            const ok = (d === 'window is blank' && now.bodyLen > 0) || (path && now.loaded.includes(path));
            if (ok) recovered.push(d);
            return !ok;
          });
        }
      }
    } catch (e) { bad.push('did not finish loading: ' + String(e && e.message || e).split('\n')[0]); }
    const ms = Date.now() - t1;
    const row = { name, ms, defects: [...new Set(bad)], recovered: [...new Set(recovered)] };
    report.push(row);
    const mark = row.defects.length ? '✗' : row.recovered.length ? '↻' : '✓';
    const notes = [...row.defects, ...row.recovered.map((d) => 'recovered: ' + d)];
    console.log(`${mark} ${name.padEnd(24)} ${(ms / 1000).toFixed(1).padStart(5)} s${notes.length ? '\n     ' + notes.join('\n     ') : ''}`);
    if (row.defects.length) { exitCode = 1; await page.screenshot(join(SHOTS, `${lang}-${name.replace(/[^\w-]+/g, '_')}.png`)); }
    await closeAllWindows(page).catch(() => page.eval(`document.querySelectorAll('.win button.close').forEach((b) => b.click())`).catch(() => {}));
    await sleep(pause);
  }
  const after = await fetch(origin + '/api/status').then((x) => x.json()).catch(() => null);
  const failed = report.filter((r) => r.defects.length);
  const slow = [...report].sort((a, b) => b.ms - a.ms).slice(0, 5).map((r) => `${r.name} ${(r.ms / 1000).toFixed(1)} s`);
  const rec = report.filter((r) => !r.defects.length && r.recovered.length);
  console.log(`\n${report.length - failed.length}/${report.length} apps working (${rec.length} recovered after a lost file${rec.length ? ': ' + rec.map((r) => r.name).join(', ') : ''}); slowest: ${slow.join(', ')}`);
  if (after) console.log(`device after: heap ${after.free_heap} B free (min ${after.min_free_heap}, block ${after.largest_free_block}), uptime ${after.uptime_s} s`);
} catch (e) {
  console.error('✗ ' + (e && e.message || e)); exitCode = 1;
} finally {
  await browser.close();
  if (sessionBefore != null) {                    // after the browser is gone: its last debounced save cannot land on top
    const ok = await restoreSession(sessionBefore).catch(() => false);
    console.log(ok ? 'window session restored as it was' : '⚠ could not restore ' + SESSION + ' — check it on the device');
  }
}
process.exit(exitCode);

async function restoreSession(text) {
  const pair = await fetch(origin + '/api/pair', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ pin: String(unit.pin) }) });
  const cookie = (pair.headers.get('set-cookie') || '').split(';')[0];
  const w = await fetch(origin + '/api/fs/write?path=' + encodeURIComponent(SESSION), { method: 'POST', headers: { cookie }, body: text });
  if (!w.ok) return false;
  const back = await fetch(origin + '/api/fs/read?path=' + encodeURIComponent(SESSION), { headers: { cookie } }).then((r) => r.text());
  return back === text;
}
