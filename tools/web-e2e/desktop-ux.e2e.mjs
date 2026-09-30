// Browser E2E — desktop defects a user actually met: icons that did nothing when double-clicked
// (shortcuts to disabled/uninstalled apps), the whole page scrolling to reveal a black band once many
// windows were open, new windows cascading off-screen, and labels split mid-letter.
import test from 'node:test';
import assert from 'node:assert/strict';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';

test('desktop: no dead icons, no page scroll, windows stay on screen', { skip }, async (t) => {
  const uiState = {
    pins: [], startPins: [], recent: [], wallpaper: null, iconSize: 'md', autoArrange: false,
    desktop: [
      { uid: 'app-notepad', type: 'app', target: 'notepad', label: 'Notepad', x: 14, y: 14 },
      { uid: 'app-swarm', type: 'app', target: 'swarm', label: 'Swarm', x: 112, y: 14 },               // disabled service
      { uid: 'app-ghost', type: 'app', target: 'ghost-app', label: 'Ghost', x: 210, y: 14 },            // not installed
      { uid: 'app-calculator', type: 'app', target: 'calculator', label: 'Calculator', x: 308, y: 14 },
    ],
  };
  const sim = await startSim({ seed: { '/system/config/ui-state.json': uiState, '/system/config/session.json': { windows: [], geom: {} } } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await browser.newPage();
  await page.setViewport(1024, 720);
  assert.ok(await bootShell(page, sim, { lang: 'it' }));

  await t.test('shortcuts to apps that are not installed or enabled are not shown', async () => {
    // By uid, not by label: a factory label ('Notepad') follows the OS language now ("Blocco note" in it).
    const uids = await page.eval(`[...document.querySelectorAll('#desktop .icon')].map((e) => e.dataset.uid)`);
    assert.ok(uids.includes('app-notepad') && uids.includes('app-calculator'), 'installed apps are shown: ' + uids.join(', '));
    assert.ok(!uids.includes('app-swarm'), 'a disabled service shows a dead icon');
    assert.ok(!uids.includes('app-ghost'), 'an uninstalled app shows a dead icon');
    const np = await page.eval(`document.querySelector('#desktop .icon[data-uid="app-notepad"]').getAttribute('aria-label')`);
    assert.equal(np, 'Blocco note', 'the seeded English factory label is shown in the OS language');
    const saved = JSON.parse(await sim.readSd('/system/config/ui-state.json'));
    assert.ok(saved.desktop.some((i) => i.target === 'swarm'), 'hidden, not deleted: re-enabling the app brings the icon back');
  });

  await t.test('labels break at syllables, never mid-letter', async () => {
    const cs = await page.eval(`(() => { const l = document.querySelector('#desktop .icon .label'); const s = getComputedStyle(l); return { wrap: s.overflowWrap, hyph: s.hyphens || s.webkitHyphens }; })()`);
    assert.notEqual(cs.wrap, 'anywhere');
    assert.equal(cs.hyph, 'auto');
  });

  await t.test('opening many windows never scrolls the page or opens one off-screen', async () => {
    const ids = await page.eval(`[...document.querySelectorAll('#sm-all .sm-row')].length`);
    assert.ok(ids > 20);
    const r = await page.eval(`(async () => {
      const rows = [...document.querySelectorAll('#sm-all .sm-row')].slice(0, 24);
      for (const row of rows) { row.click(); await new Promise((r) => setTimeout(r, 120)); }
      await new Promise((r) => setTimeout(r, 1500));
      const off = [...document.querySelectorAll('.win:not(.hidden)')].filter((w) => { const b = w.getBoundingClientRect(); return b.top > innerHeight - 60 || b.left > innerWidth - 60; }).length;
      // Focus something far down, the way an app autofocusing would.
      const probe = document.createElement('input'); probe.style.cssText = 'position:absolute;top:' + (innerHeight + 600) + 'px;left:0';
      document.body.appendChild(probe); probe.focus(); await new Promise((r) => setTimeout(r, 200)); probe.remove();
      return { n: document.querySelectorAll('.win').length, off, scroll: document.scrollingElement.scrollTop };
    })()`, 60000);
    assert.ok(r.n >= 20, `only ${r.n} windows opened`);
    assert.equal(r.off, 0, `${r.off} window(s) opened off-screen`);
    assert.equal(r.scroll, 0, 'the desktop document scrolled');
  });
});
