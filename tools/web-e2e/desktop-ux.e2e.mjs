// Browser E2E — desktop defects a user actually met: icons that did nothing when double-clicked
// (shortcuts to disabled/uninstalled apps), the whole page scrolling to reveal a black band once many
// windows were open, new windows cascading off-screen, and labels split mid-letter.
import test from 'node:test';
import assert from 'node:assert/strict';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell, closeAllWindows } from './shell.mjs';

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

// Found on a real Cardputer (tools/web-e2e/device-smoke.mjs): the device remembered ANIMA at x=1841 and
// Settings at x=1385 from a wide monitor, and on a laptop they opened OFF-SCREEN — the window existed, the
// user never saw it. A window opening with a remembered rectangle must land fully on the screen it opens on,
// and that correction must not be saved: back on the wide monitor the user's own layout is still there.
test('a window remembered on a wider screen opens fully visible, and its saved place is kept', { skip }, async () => {
  const far = { x: 1841, y: 8, w: 520, h: 360, max: false, snap: null };
  const low = { x: 150, y: 611, w: 520, h: 360, max: false, snap: null };
  const sim = await startSim({ seed: { '/system/config/session.json': { windows: [], geom: { calculator: far, notepad: low } } } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  try {
    const page = await browser.newPage();
    await page.setViewport(1024, 720);
    assert.ok(await bootShell(page, sim, { lang: 'it' }));
    for (const name of ['Calcolatrice', 'Blocco note']) {
      const box = await page.eval(`(async () => {
        const row = [...document.querySelectorAll('#sm-all .sm-row')].find((r) => r.title.replace(/\u00ad/g, '') === ${JSON.stringify(name)});
        row.click(); await new Promise((r) => setTimeout(r, 600));
        const b = [...document.querySelectorAll('.win')].pop().getBoundingClientRect();
        const tb = document.getElementById('taskbar').offsetHeight;
        return { l: b.left, t: b.top, r: b.right, b: b.bottom, W: innerWidth, H: innerHeight - tb };
      })()`);
      assert.ok(box.l >= 0 && box.r <= box.W, `${name}: horizontally on screen ${JSON.stringify(box)}`);
      assert.ok(box.t >= 0 && box.b <= box.H, `${name}: vertically above the taskbar ${JSON.stringify(box)}`);
    }
    await closeAllWindows(page);
    const saved = await page.waitFor(`fetch('/api/fs/read?path=' + encodeURIComponent('/system/config/session.json')).then((r) => r.json())
      .then((s) => (s.geom && s.geom.calculator && s.geom.notepad && s.windows.length === 0) ? s.geom : null)`, { timeout: 8000 });
    assert.equal(saved.calculator.x, far.x, 'the wide-screen place is not overwritten by a display correction');
    assert.equal(saved.notepad.y, low.y);
  } finally {
    await browser.close();
    await sim.stop();
  }
});

// Found on a real Cardputer: an older shell seeded desktop labels from the app id ("Ir Remote", "Dosbox",
// "Games"), so the Italian desktop mixed them with "Telecomando IR"/"Giochi" in the Start menu. Such a
// label is a default, not a rename: it follows the OS language. A name the user typed always wins.
test('desktop labels seeded from the app id follow the OS language; user renames stay', { skip }, async () => {
  const uiState = {
    pins: [], startPins: [], recent: [], wallpaper: null, iconSize: 'md', autoArrange: false,
    desktop: [
      { uid: 'app-ir-remote', type: 'app', target: 'ir-remote', label: 'Ir Remote', x: 14, y: 14 },
      { uid: 'app-games', type: 'app', target: 'games', label: 'Games', x: 112, y: 14 },
      { uid: 'app-recorder', type: 'app', target: 'recorder', label: 'Recorder', x: 210, y: 14 },
      { uid: 'app-calculator', type: 'app', target: 'calculator', label: 'Conti di casa', x: 308, y: 14 },
    ],
  };
  const sim = await startSim({ seed: { '/system/config/ui-state.json': uiState, '/system/config/session.json': { windows: [], geom: {} } } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  try {
    const page = await browser.newPage();
    assert.ok(await bootShell(page, sim, { lang: 'it' }));
    const label = (uid) => page.eval(`document.querySelector('#desktop .icon[data-uid="${uid}"]').getAttribute('aria-label').replace(/\u00ad/g, '')`);
    assert.equal(await label('app-ir-remote'), 'Telecomando IR');
    assert.equal(await label('app-games'), 'Giochi');
    assert.equal(await label('app-recorder'), 'Registratore');
    assert.equal(await label('app-calculator'), 'Conti di casa', 'a name the user chose is kept');
  } finally {
    await browser.close();
    await sim.stop();
  }
});
