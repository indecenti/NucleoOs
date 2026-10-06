// Browser E2E — the shell against a device that misbehaves the way a real Cardputer does: writes
// refused under heap pressure (500 "oom"), the arbiter answering 503 at boot, the device dropping off
// the network. Driven through the simulator's fault switches (POST /api/_sim/fault | offline).
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim, REPO } from './sim.mjs';
import { bootShell, waitDesktop } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const cat = (lang) => JSON.parse(readFileSync(join(REPO, 'web', 'shell', 'i18n', `shell.${lang}.json`), 'utf8'));
const toasts = `[...document.querySelectorAll('#toast-container > *')].map((x) => x.textContent)`;
const skip = !findChrome() && 'no Chrome/Edge installed';

test('a refused config write is announced, kept, and saved once the device recovers', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await browser.newPage();
  const lang = 'de';                                  // non-default language on purpose: the message must be localised
  assert.ok(await bootShell(page, sim, { lang }));
  await sim.control('/api/_sim/fault', { route: '/api/fs/write', status: 500 });   // until cleared
  await page.eval(`window.postMessage({ type: 'set-wallpaper', path: '/data/Pictures/e2e-wall.png' }, location.origin)`);
  const failed = await page.waitFor(`${toasts}.some((x) => x.includes(${JSON.stringify(cat(lang).toast_save_failed)}))`, { timeout: 12000 });
  assert.ok(failed, 'the user is told the save failed (in German)');
  const before = await sim.readSd('/system/config/ui-state.json').catch(() => '');
  assert.ok(!before.includes('e2e-wall.png'), 'nothing was written while the device refused');
  await sim.control('/api/_sim/fault', { clear: true });
  const recovered = await page.waitFor(`${toasts}.some((x) => x.includes(${JSON.stringify(cat(lang).toast_save_recovered)}))`, { timeout: 25000 });
  assert.ok(recovered, 'the user is told the change was saved after all');
  const after = await sim.readSd('/system/config/ui-state.json');
  assert.ok(after.includes('e2e-wall.png'), 'the kept change reached the SD once the device recovered');
});

test('boot survives /api/apps answering 503 (arbiter busy) and still shows the REAL app list', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await browser.newPage();
  await sim.control('/api/_sim/fault', { route: '/api/apps', status: 503, times: 2 });
  await bootShell(page, sim, { lang: 'en', wait: false });
  assert.ok(await waitDesktop(page, 40000), 'desktop never came up');
  const n = await page.eval(`document.querySelectorAll('#sm-all .sm-row').length`);
  assert.ok(n >= 40, `Start lists ${n} apps — the shell fell back to its offline mock instead of retrying`);
});

test('the device dropping off the network is shown at once, clearly, and recovers by itself', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await browser.newPage();
  const lang = 'fr';
  assert.ok(await bootShell(page, sim, { lang }));
  await page.waitFor(`(document.querySelector('#tray-ws .ws-dot') || {}).className === 'ws-dot connected'`, { timeout: 10000 });
  const t0 = Date.now();
  await sim.control('/api/_sim/offline', { on: true });
  const shown = await page.waitFor(`(() => { const b = document.getElementById('link-down'); return b && b.classList.contains('show') ? b.textContent : null; })()`, { timeout: 15000 });
  const detectMs = Date.now() - t0;
  assert.ok(shown, 'no "device unreachable" banner');
  assert.ok(shown.includes(cat(lang).link_down_title) && shown.includes(cat(lang).link_retry), `banner not in French: ${shown}`);
  assert.ok(detectMs < 10000, `took ${detectMs} ms to tell the user the device is gone`);
  t.diagnostic(`offline detected in ${detectMs} ms`);
  await sim.control('/api/_sim/offline', { on: false });
  await page.eval(`document.querySelector('#link-down .ld-btn').click()`);
  const back = await page.waitFor(`!document.getElementById('link-down').classList.contains('show') && ${toasts}.some((x) => x.includes(${JSON.stringify(cat(lang).link_back)}))`, { timeout: 10000 });
  assert.ok(back, '"Retry now" did not bring the link back');
  assert.ok(await page.waitFor(`(document.querySelector('#tray-ws .ws-dot') || {}).className === 'ws-dot connected'`, { timeout: 15000 }), 'the live channel did not re-attach');
});

// Found on a real Cardputer (tools/web-e2e/device-smoke.mjs): the single-task httpd (4 sockets) now and then
// resets a connection while an app window loads. A lost module (Recorder's `import … from '/micgate.js'`)
// left the app dead until the user reopened it — no service worker on http:// to retry. Every app reports a
// failed <script>/<link> to the shell, which reloads that window ONCE; a resource that keeps failing is not
// retried in a loop.
test('an app that loses a module while loading recovers by itself, and never reloads in a loop', { skip }, async (t) => {
  const sim = await startSim({ seed: { '/system/config/session.json': { windows: [], geom: {} } } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await browser.newPage();
  await page.setViewport(1280, 800);
  assert.ok(await bootShell(page, sim, { lang: 'it' }));
  const openRecorder = () => page.eval(`(() => {
    const row = [...document.querySelectorAll('#sm-all .sm-row')].find((r) => r.title.replace(/\u00ad/g, '') === 'Registratore');
    row.click(); return true; })()`);
  // Frame loads, counted on the "device": every (re)load of the window fetches the app page again.
  const pageLoads = async () => { const st = await sim.control('/api/_sim/stats');
    return Object.entries(st.byPath).filter(([k]) => k === '/apps/recorder/' || k === '/apps/recorder/index.html').reduce((n, [, v]) => n + v, 0); };
  const micgateLoaded = `(() => { const f = [...document.querySelectorAll('.win iframe')].pop(); try {
    return f.contentWindow.performance.getEntriesByType('resource').some((e) => e.name.endsWith('/micgate.js') && e.responseStatus === 200); } catch { return false; } })()`;

  // The device RESETS the connection; Chrome then retries on its own a varying number of times (socket
  // reuse), so a dropped socket does not reproduce deterministically. A 503 — the firmware's own "busy" —
  // is never retried by the browser and fails the module exactly like the lost one does.
  await t.test('one lost module: the window reloads once and the app works', async () => {
    await sim.control('/api/_sim/fault', { route: '/micgate.js', status: 503, times: 1 });
    await sim.control('/api/_sim/stats', { reset: true });
    assert.ok(await openRecorder());
    const ok = await page.waitFor(micgateLoaded, { timeout: 15000 }).catch(() => false);
    if (!ok) { await sim.control('/api/_sim/fault', { clear: true }); await closeAll(page); }
    assert.ok(ok, 'the app ended up with its module loaded');
    try { assert.equal(await pageLoads(), 2, 'exactly one reload'); }
    finally { await sim.control('/api/_sim/fault', { clear: true }); await closeAll(page); }
  });

  await t.test('a module that keeps failing: one retry, then the app is left alone', async () => {
    await sim.control('/api/_sim/fault', { route: '/micgate.js', status: 503, times: 0 });   // fails until cleared
    await sim.control('/api/_sim/stats', { reset: true });
    assert.ok(await openRecorder());
    await new Promise((r) => setTimeout(r, 6000));
    try { assert.equal(await pageLoads(), 2, 'one retry, no loop'); }
    finally { await sim.control('/api/_sim/fault', { clear: true }); await closeAll(page); }
  });
});

async function closeAll(page) {
  await page.eval(`document.querySelectorAll('.win button.close').forEach((b) => b.click())`);
  await page.waitFor(`document.querySelectorAll('.win').length === 0`, { timeout: 4000 });
}

// v150's guard reported ANY failed same-origin <script>/<link>, at any time: an app that lazy-loads a script
// after it has loaded (Dictation's local vosk.js with its CDN fallback, ANIMA voice, Video Studio's ffmpeg)
// was reloaded 800 ms later — the dictation, the chat, the project in progress, gone. Only a failure while
// the window is still loading may reload it (the two cases above keep that half honest).
test('a script an app lazy-loads AFTER it has loaded may fail without the window being reloaded', { skip }, async (t) => {
  const sim = await startSim({ seed: { '/system/config/session.json': { windows: [], geom: {} } } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await browser.newPage();
  await page.setViewport(1280, 800);
  assert.ok(await bootShell(page, sim, { lang: 'en' }));
  await page.eval(`window.postMessage({ type: 'open-app', id: 'notepad' }, location.origin)`);
  const frame = `[...document.querySelectorAll('.win iframe')].find((f) => (f.getAttribute('src') || '').startsWith('/apps/notepad/'))`;
  assert.ok(await page.waitFor(`(() => { const f = ${frame}; try { return !!f && f.contentDocument.readyState === 'complete' && f.contentWindow.location.href !== 'about:blank'; } catch { return false; } })()`, { timeout: 12000 }));
  await new Promise((r) => setTimeout(r, 500));
  await sim.control('/api/_sim/fault', { route: '/missing-lazy.js', status: 503 });
  await page.eval(`(() => { const f = ${frame}; f.contentWindow.__work = 'unsaved'; const s = f.contentDocument.createElement('script'); s.src = '/missing-lazy.js'; f.contentDocument.head.appendChild(s); return true; })()`);
  await new Promise((r) => setTimeout(r, 3000));
  try {
    assert.equal(await page.eval(`(() => { try { return ${frame}.contentWindow.__work || null; } catch { return 'gone'; } })()`), 'unsaved',
      'a lazy script that failed after load reloaded the window: the work in it is gone');
  } finally { await sim.control('/api/_sim/fault', { clear: true }); }
});
