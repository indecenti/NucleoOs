// Browser E2E — an app the agent wrote runs SANDBOXED (no origin, out of process) and must still be usable:
// real mouse clicks and key presses reach it, and its window is a normal window. The Chrome extension's
// synthetic clicks never reached such a frame, so "the contatore window reacts to clicks" had only ever been
// checked by hand; here the input goes through the browser's own hit-testing (CDP Input.*, page level), as a
// user's does. The app ("contatore", like the one the agent built in the live tests) lives on the card, as an
// agent-published app does, and reports each count to its parent window — the only channel it has.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim, REPO } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const J = JSON.stringify;

const SD_REG = JSON.parse(readFileSync(join(REPO, 'tools', 'sd-sim', 'system', 'registry', 'apps.json'), 'utf8'));
const REG = { ...SD_REG, installed: [...SD_REG.installed, { id: 'contatore', version: '0.1.0', path: '/apps/contatore', enabled: true, autostart: false, permissions: [], created_by: 'agent' }] };
const COUNTER = `<!doctype html><html><head><meta charset="utf-8"><title>Contatore</title>
<style>html,body{margin:0;height:100%}button{all:unset;display:grid;place-items:center;width:100vw;height:100vh;font:48px system-ui;cursor:pointer}</style></head>
<body><button id="plus" type="button">0</button><script>
let n = 0; const b = document.getElementById('plus');
const bump = (how) => { n++; b.textContent = String(n); parent.postMessage({ t: 'contatore', n, how, origin: String(self.origin) }, '*'); };
b.addEventListener('click', () => bump('click'));
addEventListener('keydown', (e) => { if (e.key === '+') bump('key'); });
parent.postMessage({ t: 'contatore-ready' }, '*');
</script></body></html>`;

async function startSdRegistrySim(opts) {
  process.env.SIM_REGISTRY_FROM_SD = '1';
  try { return await startSim(opts); } finally { delete process.env.SIM_REGISTRY_FROM_SD; }
}

test('a sandboxed agent app takes real clicks and keys', { skip }, async (t) => {
  const sim = await startSdRegistrySim({ seed: {
    '/system/registry/apps.json': REG,
    '/apps/contatore/www/index.html': COUNTER,
    '/system/config/session.json': { windows: [], geom: {} },
  } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  await sim.control('/api/_sim/apps', { add: { id: 'contatore', name: 'Contatore', route: '/apps/contatore/', icon: '', enabled: true } });
  const page = await browser.newPage();
  await page.setViewport(1280, 800);
  assert.ok(await bootShell(page, sim, { lang: 'en' }), 'desktop never came up');
  await page.eval(`(window.__cnt = [], window.__ready = false, addEventListener('message', (e) => { if (e.data && e.data.t === 'contatore') window.__cnt.push(e.data); if (e.data && e.data.t === 'contatore-ready') window.__ready = true; }), true)`);

  await page.eval(`window.postMessage({ type: 'open-app', id: 'contatore' }, location.origin)`);
  const FRAME = `[...document.querySelectorAll('.win iframe')].find((f) => (f.getAttribute('src') || '').startsWith('/apps/contatore/'))`;
  assert.ok(await page.waitFor(`!!${FRAME}`, { timeout: 10000 }), 'the window did not open');
  assert.equal(await page.eval(`${FRAME}.getAttribute('sandbox')`), 'allow-scripts allow-forms allow-modals', 'an agent app runs without an origin');
  // the app's document is out of process: it says when its handlers are in place
  assert.ok(await page.waitFor('window.__ready', { timeout: 10000 }), 'the app never loaded');

  // The window animates in (winIn) and an out-of-process frame becomes hit-testable a moment after its document
  // loads: on a busy machine (three suites at once) the first click landed before that. Wait for a still, unobstructed
  // frame (elementFromPoint is the frame itself), retry only the FIRST click until it lands — every later input must
  // land at once, and be counted once.
  const box = await page.waitFor(`(() => { const f = ${FRAME}; if (!f) return null; const r = f.getBoundingClientRect();
    const x = r.left + r.width / 2, y = r.top + r.height / 2, now = [r.left, r.top, r.width, r.height].join(',');
    const still = window.__lastRect === now; window.__lastRect = now;
    return still && document.elementFromPoint(x, y) === f ? { x, y, w: r.width, h: r.height } : null; })()`, { timeout: 15000, interval: 200 });
  assert.ok(box && box.w > 100 && box.h > 60, 'the window has a usable, unobstructed size: ' + J(box));
  const click = async () => { for (const type of ['mousePressed', 'mouseReleased']) await page.send('Input.dispatchMouseEvent', { type, x: box.x, y: box.y, button: 'left', clickCount: 1 }); };
  let landed = false;
  for (let i = 0; i < 6 && !landed; i++) { await click(); landed = !!(await page.waitFor(`window.__cnt.length >= 1`, { timeout: 1500 })); }
  assert.ok(landed, 'clicks never reached the sandboxed app: ' + J(await page.eval('window.__cnt')));
  for (const n of [2, 3]) {
    await click();
    assert.ok(await page.waitFor(`window.__cnt.length >= ${n}`, { timeout: 5000 }), `click ${n} did not land: ` + J(await page.eval('window.__cnt')));
  }

  // the click focused the frame: a key press goes to the app, not the shell
  await page.send('Input.dispatchKeyEvent', { type: 'keyDown', key: '+', text: '+', unmodifiedText: '+' });
  await page.send('Input.dispatchKeyEvent', { type: 'keyUp', key: '+' });
  assert.ok(await page.waitFor(`window.__cnt.some((m) => m.how === 'key')`, { timeout: 5000 }), 'a key press did not reach the sandboxed app');

  const got = await page.eval('window.__cnt');
  assert.deepEqual(got.map((m) => m.n), [1, 2, 3, 4], 'every input counted once, in order');
  assert.equal(got[0].origin, 'null', 'the app really ran with an opaque origin');
  assert.deepEqual(page.log.filter((e) => e.kind === 'exception'), [], 'no script errors');
});
