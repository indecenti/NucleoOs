// Browser E2E — session restore must never become a boot storm. Restoring N open windows used to
// create N app iframes at once: on the Cardputer (one httpd task, 4 sockets incl. the live /ws) that is
// dozens of simultaneous asset requests at the worst possible moment. Contract:
//   • the window that was on top loads first, immediately;
//   • every other visible window comes back in place and loads ONE AT A TIME in the background;
//   • minimised windows stay unloaded until the user opens them;
//   • nothing is lost: every window is back, in the same stacking order, with its content URL.
import test from 'node:test';
import assert from 'node:assert/strict';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const VISIBLE = ['notepad', 'calculator', 'clock', 'tasks', 'contacts', 'unit-converter', 'metronome'];
const MINIMISED = ['paint', 'weather'];
const session = {
  windows: [
    ...VISIBLE.map((id, i) => ({ id, x: 40 + i * 30, y: 30 + i * 20, w: 520, h: 360, min: false, max: i === VISIBLE.length - 1, snap: null, z: 100 + i, url: `/apps/${id}/` })),
    ...MINIMISED.map((id, i) => ({ id, x: 80, y: 60, w: 520, h: 360, min: true, max: false, snap: null, z: 90 + i, url: `/apps/${id}/` })),
  ],
  // Per-app memory says the top app was ALSO maximised last time: applying both must not toggle it back.
  geom: { [VISIBLE[VISIBLE.length - 1]]: { x: 10, y: 10, w: 600, h: 400, max: true, snap: null } },
};

test('session restore is staggered, complete and ordered', { skip: !findChrome() && 'no Chrome/Edge installed' }, async (t) => {
  const sim = await startSim({ seed: { '/system/config/session.json': session } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await browser.newPage();
  // Timeline of every app frame from the first millisecond of the shell (top document only): when it was
  // created and when it finished loading. Sampling after the desktop appeared missed the start on a busy
  // machine — the top window had already loaded and the second (correctly) begun.
  await page.initScript(`if (window.top === window && location.pathname === '/') {
    window.__frames = [];
    new MutationObserver((ms) => { for (const m of ms) for (const n of m.addedNodes) {
      const fs = n.tagName === 'IFRAME' ? [n] : (n.querySelectorAll ? [...n.querySelectorAll('iframe')] : []);
      for (const f of fs) { if (f.__t) continue; const rec = { at: performance.now(), done: 0 }; f.__t = rec; window.__frames.push(rec);
        f.addEventListener('load', () => { if (f.getAttribute('src') && !rec.done) rec.done = performance.now(); }); }
    } }).observe(document, { childList: true, subtree: true });
  }`);
  assert.ok(await bootShell(page, sim, { lang: 'en', wait: false }) !== false);

  // Sample the page every 50 ms from the moment the desktop is up: how many app documents are
  // loading AT THE SAME TIME, and how many exist at all.
  await page.waitFor(`document.querySelectorAll('#desktop .icon').length > 0`, { timeout: 30000 });
  const trace = await page.eval(`(async () => {
    const out = { maxLoading: 0, firstFrames: -1, samples: 0 };
    const t0 = performance.now();
    while (performance.now() - t0 < 20000) {
      const frames = [...document.querySelectorAll('.win iframe')];
      const loading = frames.filter((f) => { try { const d = f.contentDocument; return !d || d.readyState !== 'complete' || d.location.href === 'about:blank'; } catch { return false; } }).length;
      if (out.firstFrames < 0 && document.querySelectorAll('.win').length >= ${VISIBLE.length + MINIMISED.length}) out.firstFrames = frames.length;
      out.maxLoading = Math.max(out.maxLoading, loading);
      out.samples++;
      if (frames.length >= ${VISIBLE.length} && loading === 0) break;
      await new Promise((r) => setTimeout(r, 50));
    }
    return out;
  })()`, 30000);

  const state = await page.eval(`(() => {
    const wins = [...document.querySelectorAll('.win')].map((w) => ({
      z: +w.style.zIndex || 0, hidden: w.classList.contains('hidden'),
      src: (w.querySelector('iframe') || { getAttribute: () => null }).getAttribute('src'),
      title: (w.querySelector('.bar .t') || {}).textContent }));
    const active = document.querySelector('.win.active iframe');
    const top = document.querySelector('.win.active');
    return { wins, active: active && active.getAttribute('src'), topMax: !!(top && top.classList.contains('max')) };
  })()`);

  assert.equal(state.wins.length, VISIBLE.length + MINIMISED.length, 'every saved window is back');
  // Up front = created before the FIRST app frame finished loading — or before staggerRestore's 15 s safety
  // net (a top app that never loads must not block the rest forever). From the boot timeline, not a late sample.
  const tl = await page.eval(`(window.__frames || []).map((f) => ({ at: Math.round(f.at), done: Math.round(f.done) }))`);
  assert.ok(tl.length > 0, 'the frame timeline was recorded');
  const gate = Math.min(Math.min(...tl.filter((f) => f.done).map((f) => f.done)), tl[0].at + 15000);
  const upFront = tl.filter((f) => f.at < gate).length;
  assert.ok(upFront <= 1, `restore created ${upFront} app frames before the first one loaded (expected only the top window): ${JSON.stringify(tl)}`);
  assert.ok(trace.maxLoading <= 1, `${trace.maxLoading} apps were loading at the same time (expected one at a time)`);
  const loaded = state.wins.filter((w) => w.src).map((w) => w.src.split('/')[2]);
  for (const id of VISIBLE) assert.ok(loaded.includes(id), `visible window ${id} was never loaded`);
  for (const id of MINIMISED) assert.ok(!loaded.includes(id), `minimised window ${id} was loaded at boot`);
  assert.equal(state.active, `/apps/${VISIBLE[VISIBLE.length - 1]}/`, 'the top window is the active one');
  assert.equal(state.topMax, true, 'a window saved maximised comes back maximised');
  const visibleZ = state.wins.filter((w) => !w.hidden).sort((a, b) => a.z - b.z).map((w) => w.src && w.src.split('/')[2]);
  assert.deepEqual(visibleZ, VISIBLE, 'stacking order preserved');
});
