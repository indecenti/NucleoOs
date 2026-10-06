// Browser E2E — regressions for the shell review of 2026-10 (sw cache v149). Each case reproduces the
// defect the way a user (or a hostile agent-written app) meets it, on the real shell + the device simulator:
//   • an agent app reaching an unsandboxed window through a .lnk, or because its provenance was not read yet
//   • re-opening an app reloading it (unsaved text lost); status polling dead after the tab was hidden once
//   • a stale modal's Enter confirming a destructive action; failed copies/links reported as success
//   • title-bar / resize-handle geometry bugs; a slow file-dialog listing overwriting a newer one
//   • hard-coded chrome strings; a sandboxed frame switching the shell language
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim, REPO } from './sim.mjs';
import { bootShell, closeAllWindows } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const J = JSON.stringify;

const LANG = 'de';   // not en/it: a hard-coded English or Italian string cannot pass for a translation
const cat = (l) => JSON.parse(readFileSync(join(REPO, 'web', 'shell', 'i18n', `shell.${l}.json`), 'utf8'));
const DE = cat('de'), EN = cat('en');
const L = (k) => (DE[k] != null ? DE[k] : EN[k]);

// The SD registry the shell reads (/system/registry/apps.json), plus one agent-written app.
const SD_REG = JSON.parse(readFileSync(join(REPO, 'tools', 'sd-sim', 'system', 'registry', 'apps.json'), 'utf8'));
const withAgent = (...ids) => ({ ...SD_REG, installed: [...SD_REG.installed, ...ids.map((id) => ({ id, version: '0.1.0', path: '/apps/' + id, enabled: true, autostart: false, permissions: [], created_by: 'agent' }))] });
// The simulator answers /system/registry/apps.json from the REPO registry unless SIM_REGISTRY_FROM_SD=1;
// these cases are about what the card's own registry says (or fails to say), so serve the card's copy.
async function startSdRegistrySim(opts) {
  process.env.SIM_REGISTRY_FROM_SD = '1';
  try { return await startSim(opts); } finally { delete process.env.SIM_REGISTRY_FROM_SD; }
}
// Hosted on an existing route so the frame loads; identity is the record id, which is what decides trust.
const simApp = (id, name) => ({ id, name, route: '/apps/notepad/', icon: '', enabled: true });

// The window whose title bar reads `name`, or whose iframe src starts with `src`.
const WIN = (by) => `([...document.querySelectorAll('.win')].find((w) => ${by.name ? `w.querySelector('.bar .t').textContent === ${J(by.name)}` : `(w.querySelector('iframe') && (w.querySelector('iframe').getAttribute('src') || '').startsWith(${J(by.src)}))`}) || null)`;
const frameState = (by) => `(() => { const w = ${WIN(by)}; if (!w) return 'no-window'; const f = w.querySelector('iframe'); if (!f) return 'held'; return f.hasAttribute('sandbox') ? 'sandboxed' : 'trusted'; })()`;
const openApp = (id, query) => `window.postMessage({ type: 'open-app', id: ${J(id)}${query ? `, query: ${J(query)}` : ''} }, location.origin)`;
const dblIcon = (label) => `(() => { const el = [...document.querySelectorAll('#desktop .icon')].find((e) => e.getAttribute('aria-label') === ${J(label)}); if (!el) return false; el.dispatchEvent(new MouseEvent('dblclick', { bubbles: true })); return true; })()`;

test('shell review fixes (v149)', { skip }, async (t) => {
  const uiState = { pins: [], startPins: [], recent: [], wallpaper: null, iconSize: 'md', autoArrange: false,
    desktop: [{ uid: 'app-notepad', type: 'app', target: 'notepad', label: 'Notepad', x: 14, y: 14 }] };
  const sim = await startSdRegistrySim({ seed: {
    '/system/registry/apps.json': withAgent('agentapp'),
    '/system/config/ui-state.json': uiState,
    '/system/config/session.json': { windows: [], geom: {} },
    '/data/Desktop/agent.lnk': { schema: 1, type: 'url', target: '/apps/agentapp/', label: 'AgentLink' },
    '/data/Desktop/ghost.lnk': { schema: 1, type: 'url', target: '/apps/no-such-app/index.html', label: 'GhostLink' },
    '/data/Desktop/good.lnk': { schema: 1, type: 'url', target: '/apps/notepad/', label: 'GoodLink' },
    '/data/Documents/keep.txt': 'hello',
  } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  await sim.control('/api/_sim/apps', { add: simApp('agentapp', 'Agent App') });
  await sim.control('/api/_sim/apps', { add: simApp('stranger', 'Stranger App') });   // neither in the registry nor built in
  const page = await browser.newPage();
  await page.setViewport(1280, 800);
  assert.ok(await bootShell(page, sim, { lang: LANG }), 'desktop never came up');
  const fresh = () => closeAllWindows(page).catch(() => {});   // a failed case must not leave windows for the next

  await t.test('bug 1: a .lnk cannot open an agent app (or an unknown /apps path) outside its sandbox', async () => {
    await page.waitFor(`[...document.querySelectorAll('#desktop .icon')].some((e) => e.getAttribute('aria-label') === 'agent')`, { timeout: 10000 });
    assert.ok(await page.eval(dblIcon('agent')));
    assert.ok(await page.waitFor(`${frameState({ src: '/apps/agentapp/' })} !== 'no-window'`, { timeout: 8000 }));
    assert.equal(await page.eval(frameState({ src: '/apps/agentapp/' })), 'sandboxed', 'an agent app opened through a .lnk got the shell origin');
    assert.ok(await page.eval(dblIcon('ghost')));
    assert.ok(await page.waitFor(`${frameState({ src: '/apps/no-such-app/' })} !== 'no-window'`, { timeout: 8000 }));
    assert.equal(await page.eval(frameState({ src: '/apps/no-such-app/' })), 'sandboxed', 'a link to an unknown app is not vouched for');
    assert.ok(await page.eval(dblIcon('good')));
    assert.ok(await page.waitFor(`${frameState({ src: '/apps/notepad/' })} === 'trusted'`, { timeout: 8000 }), 'a link to a built-in app still opens normally');
    await closeAllWindows(page);
  });

  await t.test('bug 2: an app the registry does not vouch for runs sandboxed; built-ins do not', async () => {
    await fresh();
    await page.eval(openApp('stranger'));
    assert.ok(await page.waitFor(`${frameState({ name: 'Stranger App' })} === 'sandboxed' || ${frameState({ name: 'Stranger App' })} === 'trusted'`, { timeout: 8000 }));
    assert.equal(await page.eval(frameState({ name: 'Stranger App' })), 'sandboxed', 'unknown provenance must fail closed');
    await page.eval(openApp('agentapp'));
    assert.ok(await page.waitFor(`${frameState({ name: 'Agent App' })} === 'sandboxed'`, { timeout: 8000 }), 'created_by:agent is sandboxed');
    await page.eval(openApp('calculator'));
    assert.ok(await page.waitFor(`${frameState({ src: '/apps/calculator/' })} === 'trusted'`, { timeout: 8000 }), 'a built-in app is trusted');
    await closeAllWindows(page);
  });

  await t.test('bug 2: a just-published app opened before the registry re-read is held, then sandboxed', async () => {
    await fresh();
    const reg = withAgent('agentapp', 'fresh-agent');
    const { writeFile } = await import('node:fs/promises');
    await writeFile(join(sim.sd, 'system', 'registry', 'apps.json'), J(reg));
    await sim.control('/api/_sim/apps', { add: simApp('fresh-agent', 'Fresh Agent') });
    await sim.control('/api/_sim/fault', { route: '/api/fs/read', delay_ms: 3000, times: 1 });   // the registry re-read is slow
    await sim.control('/api/_sim/publish', { t: 'apps.changed', d: {} });
    assert.ok(await page.waitFor(`[...document.querySelectorAll('#sm-all .sm-row')].some((r) => r.title === 'Fresh Agent')`, { timeout: 10000 }), 'the new app never appeared');
    await page.eval(openApp('fresh-agent'));
    assert.ok(await page.waitFor(`${frameState({ name: 'Fresh Agent' })} !== 'no-window'`, { timeout: 5000 }), 'the window did not open');
    const early = await page.eval(frameState({ name: 'Fresh Agent' }));
    assert.notEqual(early, 'trusted', 'the new app got the shell origin before its provenance was read');
    assert.ok(await page.waitFor(`${frameState({ name: 'Fresh Agent' })} === 'sandboxed'`, { timeout: 15000 }), 'the held window never loaded, sandboxed');
    await sim.control('/api/_sim/fault', { clear: true });
    await closeAllWindows(page);
  });

  await t.test('bug 3: re-opening an open app focuses it without reloading it', async () => {
    await fresh();
    await page.eval(openApp('notepad'));
    assert.ok(await page.waitFor(`(() => { const f = ${WIN({ src: '/apps/notepad/' })}?.querySelector('iframe'); return f && f.contentDocument && f.contentDocument.readyState === 'complete' && f.contentWindow.location.href !== 'about:blank'; })()`, { timeout: 10000 }));
    await page.eval(`${WIN({ src: '/apps/notepad/' })}.querySelector('iframe').contentWindow.__unsaved = 'typed text'`);
    await page.eval(openApp('notepad'));
    await sleep(1500);
    assert.equal(await page.eval(`${WIN({ src: '/apps/notepad/' })}.querySelector('iframe').contentWindow.__unsaved || null`), 'typed text', 'the app was reloaded: unsaved work lost');
    // ...while an open WITH a target (a file double-clicked) still navigates the window to it.
    await page.eval(openApp('notepad', 'path=' + encodeURIComponent('/data/Documents/keep.txt')));
    assert.ok(await page.waitFor(`(${WIN({ src: '/apps/notepad/' })}.querySelector('iframe').getAttribute('src') || '').includes('keep.txt')`, { timeout: 5000 }));
  });

  await t.test('bug 15: window controls are localised', async () => {
    const titles = await page.eval(`(() => { const w = ${WIN({ src: '/apps/notepad/' })}; return { min: w.querySelector('.min').title, max: w.querySelector('.max').title, close: w.querySelector('.close').title }; })()`);
    assert.ok(L('win_minimize') && L('win_maximize') && L('win_close') && L('win_restore'), 'the window-control keys are missing from the shell catalog');
    assert.deepEqual(titles, { min: L('win_minimize'), max: L('win_maximize'), close: L('win_close') });
    await page.eval(`${WIN({ src: '/apps/notepad/' })}.querySelector('.max').click()`);
    assert.equal(await page.eval(`${WIN({ src: '/apps/notepad/' })}.querySelector('.max').title`), L('win_restore'));
    await page.eval(`${WIN({ src: '/apps/notepad/' })}.querySelector('.max').click()`);
  });

  // Synthetic pointer gestures on the notepad window (pointerId 7 throughout, like one mouse).
  const gesture = (sel, steps) => page.eval(`(() => {
    const w = ${WIN({ src: '/apps/notepad/' })}; const el = w.querySelector(${J(sel)}); const r = el.getBoundingClientRect();
    let x = r.left + r.width / 2, y = r.top + r.height / 2;
    const fire = (type, target) => target.dispatchEvent(new PointerEvent(type, { bubbles: true, cancelable: true, button: 0, buttons: type === 'pointerup' ? 0 : 1, pointerId: 7, isPrimary: true, clientX: x, clientY: y }));
    for (const s of ${J(steps)}) {
      if (s.move) { x += s.move[0]; y += s.move[1]; fire('pointermove', window); }
      else if (s.dbl) el.dispatchEvent(new MouseEvent('dblclick', { bubbles: true, clientX: x, clientY: y }));
      else fire(s.t, s.t === 'pointerdown' ? el : window);
    }
    return { max: w.classList.contains('max'), left: parseInt(w.style.left), top: parseInt(w.style.top), width: w.offsetWidth, height: w.offsetHeight };
  })()`);

  await t.test('bug 12: a click on a maximized title bar keeps it maximized; double-click restores', async () => {
    await page.eval(`${WIN({ src: '/apps/notepad/' })}.querySelector('.max').click()`);
    assert.equal((await gesture('.bar .t', [])).max, true);
    assert.equal((await gesture('.bar .t', [{ t: 'pointerdown' }, { t: 'pointerup' }])).max, true, 'a single click un-maximized the window');
    const dbl = await gesture('.bar .t', [{ t: 'pointerdown' }, { t: 'pointerup' }, { t: 'pointerdown' }, { t: 'pointerup' }, { dbl: true }]);
    assert.equal(dbl.max, false, 'double-click on a maximized title bar did not restore it');
    await page.eval(`${WIN({ src: '/apps/notepad/' })}.querySelector('.max').click()`);
    const torn = await gesture('.bar .t', [{ t: 'pointerdown' }, { move: [40, 30] }, { t: 'pointerup' }]);
    assert.equal(torn.max, false, 'dragging a maximized window still tears it off');
  });

  await t.test('bug 13: resizing past the minimum never moves the opposite edge', async () => {
    await page.eval(`(() => { const w = ${WIN({ src: '/apps/notepad/' })}; Object.assign(w.style, { left: '300px', top: '200px', width: '520px', height: '400px' }); })()`);
    const sw = await gesture('.win-resizer.sw', [{ t: 'pointerdown' }, { move: [0, -700] }, { t: 'pointerup' }]);
    assert.equal(sw.top, 200, 'dragging the sw corner up past the min height moved the TOP edge');
    assert.equal(sw.height, 160);
    await page.eval(`(() => { const w = ${WIN({ src: '/apps/notepad/' })}; Object.assign(w.style, { left: '300px', top: '200px', width: '520px', height: '400px' }); })()`);
    const ne = await gesture('.win-resizer.ne', [{ t: 'pointerdown' }, { move: [-900, 0] }, { t: 'pointerup' }]);
    assert.equal(ne.left, 300, 'dragging the ne corner left past the min width moved the LEFT edge');
    assert.equal(ne.width, 240);
    await page.eval(`(() => { const w = ${WIN({ src: '/apps/notepad/' })}; Object.assign(w.style, { left: '300px', top: '200px', width: '520px', height: '400px' }); })()`);
    const nw = await gesture('.win-resizer.nw', [{ t: 'pointerdown' }, { move: [600, 600] }, { t: 'pointerup' }]);
    assert.deepEqual([nw.left, nw.top, nw.width, nw.height], [580, 440, 240, 160], 'the nw corner still pins the far (se) corner');
    await closeAllWindows(page);
  });

  await t.test('bug 9: a modal opened over another cancels it — a later Enter cannot confirm the stale one', async () => {
    await fresh();
    const before = await page.eval(`document.querySelectorAll('#desktop .icon').length`);
    const r = await page.eval(`(async () => {
      const wait = (ms) => new Promise((r) => setTimeout(r, ms));
      const desk = document.getElementById('desktop');
      desk.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true, clientX: 700, clientY: 500 }));
      await wait(100);
      const reset = [...document.querySelectorAll('#ctxmenu .ctx-item')].find((i) => i.textContent.includes(${J(L('ctx_reset_apps'))}));
      if (!reset) return 'no reset item';
      reset.click(); await wait(200);
      const icon = document.querySelector('#desktop .icon');
      icon.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true, clientX: 40, clientY: 40 }));
      await wait(100);
      const props = [...document.querySelectorAll('#ctxmenu .ctx-item')].find((i) => i.textContent.includes(${J(L('ctx_properties'))}));
      if (!props) return 'no properties item';
      props.click(); await wait(200);
      document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true, cancelable: true }));
      await wait(1200);
      return 'ok';
    })()`);
    assert.equal(r, 'ok');
    assert.equal(await page.eval(`document.querySelectorAll('#desktop .icon').length`), before, 'Enter on the properties box confirmed the hidden "reset desktop" dialog');
    assert.equal(await page.eval(`document.getElementById('os-modal-scrim').classList.contains('show')`), false, 'a modal is still up');
  });

  const fcDrop = (items, mods = {}) => page.eval(`(async () => {
    document.getElementById('toast-container').innerHTML = '';
    window.postMessage({ type: 'fc-drag-start', items: ${J(items)} }, location.origin);
    await new Promise((r) => setTimeout(r, 150));
    document.getElementById('desktop').dispatchEvent(new DragEvent('drop', { bubbles: true, cancelable: true, clientX: 600, clientY: 420, ...${J(mods)} }));
    await new Promise((r) => setTimeout(r, 2500));
    return [...document.querySelectorAll('#toast-container .toast')].map((t) => t.className + ' :: ' + t.textContent);
  })()`);
  const sdHas = async (p) => { try { await sim.readSd(p); return true; } catch { return false; } };

  await t.test('bug 11: a shortcut whose write failed is not shown or announced as created', async () => {
    await sim.control('/api/_sim/fault', { route: '/api/fs/write', status: 500, times: 1 });
    const toasts = await fcDrop([{ path: '/data/Documents/keep.txt', name: 'keep.txt' }]);
    await sim.control('/api/_sim/fault', { clear: true });
    assert.ok(!toasts.some((x) => x.includes(L('drop_linked'))), 'a failed shortcut was announced as created: ' + toasts.join(' | '));
    assert.ok(toasts.some((x) => x.includes('error')), 'the failure was not reported: ' + toasts.join(' | '));
    assert.equal(await page.eval(`[...document.querySelectorAll('#desktop .icon')].some((e) => e.getAttribute('aria-label') === 'keep')`), false, 'an icon for a file that was never written');
  });

  await t.test('bug 10: copying an unreadable file writes nothing and reports the failure', async () => {
    const toasts = await fcDrop([{ path: '/data/Documents/missing.txt', name: 'missing.txt' }], { ctrlKey: true });
    assert.equal(await sdHas('/data/Desktop/missing.txt'), false, 'the read error body was written as the "copied" file');
    assert.ok(!toasts.some((x) => x.includes(L('drop_copied'))), 'a failed copy was announced as done: ' + toasts.join(' | '));
    // The happy path still copies.
    const ok = await fcDrop([{ path: '/data/Documents/keep.txt', name: 'keep.txt' }], { ctrlKey: true });
    assert.equal(await sim.readSd('/data/Desktop/keep.txt'), 'hello');
    assert.ok(ok.some((x) => x.includes(L('drop_copied'))));
  });

  await t.test('bug 14 + 15: the file dialog shows the folder last navigated to, with a fresh filter', async () => {
    const dlg = (js) => page.eval(`(() => { ${js} })()`);
    await dlg(`window.postMessage({ type: 'os-file-dialog', id: 't1', mode: 'open', defaultPath: '/data/nope-missing' }, location.origin);`);
    assert.ok(await page.waitFor(`!!document.querySelector('#os-dialog-list .os-dialog-msg') && !document.querySelector('#os-dialog-list .os-dialog-msg').textContent.includes('…')`, { timeout: 8000 }));
    assert.equal(await page.eval(`document.querySelector('#os-dialog-list .os-dialog-msg').textContent`), L('osdlg_read_error'), 'the read error is not localised');
    await dlg(`document.getElementById('os-dialog-btn-cancel').click();`);
    await dlg(`window.postMessage({ type: 'os-file-dialog', id: 't2', mode: 'open', defaultPath: '/data/Documents' }, location.origin);`);
    assert.ok(await page.waitFor(`[...document.querySelectorAll('#os-dialog-list .os-dialog-name')].some((n) => n.textContent === 'keep.txt')`, { timeout: 8000 }));
    await dlg(`const li = [...document.querySelectorAll('#os-dialog-list .os-dialog-item')].find((l) => l.textContent.includes('keep.txt')); li.click();
      const s = document.getElementById('os-dialog-search'); s.value = 'kee'; s.dispatchEvent(new Event('input'));`);
    assert.equal(await page.eval(`document.getElementById('os-dialog-filename').value`), 'keep.txt');
    // An older, SLOW listing (Desktop) must not land over the newer one (the SD root).
    await sim.control('/api/_sim/fault', { route: '/api/fs/list', delay_ms: 2500, times: 1 });
    await dlg(`const up = [...document.querySelectorAll('#os-dialog-list .os-dialog-item')].find((l) => l.querySelector('.os-dialog-name').textContent === '..'); up.dispatchEvent(new MouseEvent('dblclick'));`);
    await sleep(200);
    await dlg(`document.querySelector('#os-dialog-crumbs .os-crumbs-seg').click();`);
    await sleep(3800);
    await sim.control('/api/_sim/fault', { clear: true });
    const st = await page.eval(`({ crumbs: document.getElementById('os-dialog-crumbs').textContent.trim(),
      names: [...document.querySelectorAll('#os-dialog-list .os-dialog-name')].map((n) => n.textContent),
      search: document.getElementById('os-dialog-search').value, file: document.getElementById('os-dialog-filename').value })`);
    assert.equal(st.crumbs, 'SD');
    assert.ok(st.names.includes('system') && !st.names.includes('Documents'), 'a slow older listing overwrote the newer folder: ' + st.names.join(','));
    assert.equal(st.search, '', 'the search filter survived navigation');
    assert.equal(st.file, '', 'a file picked in another folder survived navigation (Open would pick the wrong path)');
    await dlg(`document.getElementById('os-dialog-btn-cancel').click();`);
  });

  await t.test('bug 16: a sandboxed frame cannot switch the shell language', async () => {
    const r = await page.eval(`(async () => {
      const f = document.createElement('iframe');
      f.setAttribute('sandbox', 'allow-scripts');
      f.srcdoc = '<script>parent.postMessage({type:"set-language",lang:"fr"},"*");<\\/script>';
      document.body.appendChild(f);
      await new Promise((r) => setTimeout(r, 1200));
      f.remove();
      return window.NucleoI18N.lang;
    })()`);
    assert.equal(r, LANG, 'a sandboxed (opaque-origin) frame switched the OS language');
  });

  await t.test('bug 4: status polling resumes after the tab was hidden while the socket stayed open', async () => {
    const status = async () => ((await sim.control('/api/_sim/stats')).byPath['/api/status'] || 0);
    await page.eval(`(() => { Object.defineProperty(document, 'hidden', { configurable: true, get: () => true }); document.dispatchEvent(new Event('visibilitychange')); })()`);
    // Let the poll's pending tick fire while hidden (base interval 15 s): it then stops rescheduling.
    await sleep(17000);
    await sim.control('/api/_sim/stats', { reset: true });
    await page.eval(`(() => { Object.defineProperty(document, 'hidden', { configurable: true, get: () => false }); document.dispatchEvent(new Event('visibilitychange')); })()`);
    await sleep(2500);
    assert.ok(await status() >= 1, 'coming back to the tab did not revive the /api/status poll');
  });
});

test('registry unreadable: built-in apps stay trusted, everything else is sandboxed (bug 2)', { skip }, async (t) => {
  const sim = await startSdRegistrySim({ seed: { '/system/registry/apps.json': '{ this is not json', '/system/config/session.json': { windows: [], geom: {} } } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  await sim.control('/api/_sim/apps', { add: simApp('agentapp', 'Agent App') });
  const page = await browser.newPage();
  assert.ok(await bootShell(page, sim, { lang: 'en' }), 'desktop never came up');
  await page.eval(openApp('agentapp'));
  assert.ok(await page.waitFor(`${frameState({ name: 'Agent App' })} === 'sandboxed' || ${frameState({ name: 'Agent App' })} === 'trusted'`, { timeout: 20000 }));
  assert.equal(await page.eval(frameState({ name: 'Agent App' })), 'sandboxed', 'an app of unconfirmed provenance got the shell origin');
  await page.eval(openApp('calculator'));   // (not notepad: the agent app above is hosted on notepad's route)
  assert.ok(await page.waitFor(`${frameState({ src: '/apps/calculator/' })} === 'trusted'`, { timeout: 20000 }), 'a built-in app was broken by a failed registry read');
});

// ---- v151: regressions of the v149/v150 fixes ----------------------------------------------------------

// Runs `code` INSIDE the window's iframe (so e.source is the app's own window, as for a real app message).
const inFrame = (by, code) => `(() => { const f = ${WIN(by)}?.querySelector('iframe'); if (!f) return 'no-frame'; f.contentWindow.eval(${J(code)}); return 'ok'; })()`;
const frameReady = (by) => `(() => { const f = ${WIN(by)}?.querySelector('iframe'); try { return !!f && f.contentDocument.readyState === 'complete' && f.contentWindow.location.href !== 'about:blank'; } catch { return false; } })()`;

test('registry AND built-in list unreadable at boot: apps wait, are never sandboxed for it, and load trusted once the reads recover', { skip }, async (t) => {
  const sim = await startSdRegistrySim({ seed: { '/system/registry/apps.json': '{ this is not json', '/system/config/session.json': { windows: [], geom: {} } } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  await sim.control('/api/_sim/fault', { route: '/app-catalog.json', status: 503 });   // until cleared
  const page = await browser.newPage();
  await page.setViewport(1280, 800);
  assert.ok(await bootShell(page, sim, { lang: 'en' }), 'desktop never came up');
  await page.eval(openApp('settings'));
  assert.ok(await page.waitFor(`${frameState({ name: 'Settings' })} !== 'no-window'`, { timeout: 8000 }), 'the Settings window did not open');
  await sleep(4000);
  assert.equal(await page.eval(frameState({ name: 'Settings' })), 'held', 'a failed provenance read settled a built-in app (Settings) as sandboxed for the whole session');
  // The device recovers: the registry is readable again and the built-in list is served.
  const { writeFile } = await import('node:fs/promises');
  await writeFile(join(sim.sd, 'system', 'registry', 'apps.json'), J(SD_REG));
  await sim.control('/api/_sim/fault', { clear: true });
  assert.ok(await page.waitFor(`${frameState({ name: 'Settings' })} === 'trusted'`, { timeout: 45000 }), 'the held built-in window never loaded once the reads recovered');
});

test('shell review fixes (v151)', { skip }, async (t) => {
  // A user-installed (not agent-written, not built-in) app: only the registry vouches for it.
  const reg = { ...SD_REG, installed: [...SD_REG.installed, { id: 'userapp', version: '0.1.0', path: '/apps/userapp', enabled: true, autostart: false, permissions: [] }] };
  const sim = await startSdRegistrySim({ seed: { '/system/registry/apps.json': reg, '/system/config/session.json': { windows: [], geom: {} },
    '/data/Documents/keep.txt': 'hello' } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  await sim.control('/api/_sim/apps', { add: { ...simApp('userapp', 'User App'), route: '/apps/calculator/' } });   // calculator never retitles its window
  const page = await browser.newPage();
  await page.setViewport(1280, 800);
  assert.ok(await bootShell(page, sim, { lang: 'en' }), 'desktop never came up');

  await t.test('an app refresh with the registry unreadable keeps an open trusted window trusted, and its messages accepted', async () => {
    await page.eval(openApp('userapp'));
    assert.ok(await page.waitFor(`${frameState({ name: 'User App' })} === 'trusted'`, { timeout: 10000 }), 'the registry-vouched app did not open trusted');
    assert.ok(await page.waitFor(frameReady({ name: 'User App' }), { timeout: 10000 }));
    const { writeFile } = await import('node:fs/promises');
    await writeFile(join(sim.sd, 'system', 'registry', 'apps.json'), '{ this is not json');
    await sim.control('/api/_sim/publish', { t: 'apps.changed', d: {} });   // → refreshApps → the registry re-read fails
    await sleep(4000);
    assert.equal(await page.eval(inFrame({ name: 'User App' }, `parent.postMessage({ type: 'set-window-title', title: 'Still mine' }, location.origin)`)), 'ok');
    const accepted = await page.waitFor(`[...document.querySelectorAll('.win .bar .t')].some((x) => x.textContent.includes('Still mine'))`, { timeout: 4000 });
    await writeFile(join(sim.sd, 'system', 'registry', 'apps.json'), J(reg));
    assert.ok(accepted, 'after a failed registry re-read the open, trusted window was demoted: the shell now drops its messages');
    await closeAllWindows(page);
  });

  await t.test('open-app with reload:true reloads an open window; without it, it only focuses it', async () => {
    await closeAllWindows(page).catch(() => {});
    await page.eval(openApp('notepad'));
    assert.ok(await page.waitFor(frameReady({ src: '/apps/notepad/' }), { timeout: 10000 }));
    await page.eval(`${WIN({ src: '/apps/notepad/' })}.querySelector('iframe').contentWindow.__mark = 'before'`);
    await page.eval(openApp('notepad'));
    await sleep(1500);
    assert.equal(await page.eval(`${WIN({ src: '/apps/notepad/' })}.querySelector('iframe').contentWindow.__mark || null`), 'before', 'a bare re-open reloaded the app');
    await page.eval(`window.postMessage({ type: 'open-app', id: 'notepad', reload: true }, location.origin)`);
    assert.ok(await page.waitFor(`(() => { const f = ${WIN({ src: '/apps/notepad/' })}?.querySelector('iframe'); try { return !!f && f.contentDocument.readyState === 'complete' && f.contentWindow.location.href !== 'about:blank' && f.contentWindow.__mark === undefined; } catch { return false; } })()`, { timeout: 8000 }),
      'open-app {reload:true} did not reload the open window (an updated app / a newly installed game stays stale)');
    // reload together with a query: the query wins (one navigation to it).
    await page.eval(`window.postMessage({ type: 'open-app', id: 'notepad', reload: true, query: ${J('path=' + encodeURIComponent('/data/Documents/keep.txt'))} }, location.origin)`);
    assert.ok(await page.waitFor(`(${WIN({ src: '/apps/notepad/' })}.querySelector('iframe').getAttribute('src') || '').includes('keep.txt')`, { timeout: 5000 }));
    await closeAllWindows(page);
  });
});
