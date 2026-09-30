// Browser E2E — the shell's trust boundary, attacked the way a real adversary would: a hostile app
// manifest (the agent can publish one), a hostile .lnk on the SD (any app with shared storage can
// write one), and a sandboxed/foreign frame posting OS commands. Each case must be INERT — and the
// legitimate path next to it must keep working, so the fence is not simply "everything is off".
import test from 'node:test';
import assert from 'node:assert/strict';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const EVIL_NAME = '<img src=x onerror="window.top.__pwned=1">Evil';

test('shell trust boundary', { skip: !findChrome() && 'no Chrome/Edge installed' }, async (t) => {
  const sim = await startSim({ seed: {
    '/data/Desktop/evil.lnk': { schema: 1, type: 'url', target: 'javascript:window.top.__pwned=3', label: 'EvilLink' },
    '/data/Desktop/data.lnk': { schema: 1, type: 'url', target: 'data:text/html,<script>top.__pwned=4</script>', label: 'DataLink' },
    '/data/Desktop/good.lnk': { schema: 1, type: 'url', target: '/apps/notepad/', label: 'GoodLink' },
  } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  await sim.control('/api/_sim/apps', { add: { id: 'evil', name: EVIL_NAME, route: '/apps/notepad/', icon: 'x" onerror="window.top.__pwned=2', enabled: true } });
  const page = await browser.newPage();
  assert.ok(await bootShell(page, sim, { lang: 'en' }), 'desktop never came up');

  await t.test('a hostile manifest name/icon renders as text, never as markup', async () => {
    const opened = await page.eval(`(() => {
      const row = [...document.querySelectorAll('#sm-all .sm-row')].find((r) => r.title === ${JSON.stringify(EVIL_NAME)});
      if (!row) return 'no row';
      row.click(); return 'ok';
    })()`);
    assert.equal(opened, 'ok', 'the injected app is listed in Start');
    await sleep(1500);
    assert.equal(await page.eval('window.__pwned === undefined'), true, 'markup from the manifest executed');
    const label = await page.eval(`[...document.querySelectorAll('.task-btn .label')].map((l) => l.textContent).find((x) => x.includes('Evil')) || null`);
    assert.equal(label, EVIL_NAME, 'the taskbar shows the literal name');
    assert.equal(await page.eval(`document.querySelectorAll('img[onerror]').length`), 0, 'no injected <img onerror> in the DOM');
    await page.eval(`document.querySelectorAll('.win button.close').forEach((b) => b.click())`);
  });

  await t.test('a .lnk pointing at javascript:/data: is refused; a same-origin link still opens', async () => {
    const dbl = (label) => page.eval(`(() => {
      const el = [...document.querySelectorAll('#desktop .icon')].find((e) => e.getAttribute('aria-label') === ${JSON.stringify(label)});
      if (!el) return false;
      el.dispatchEvent(new MouseEvent('dblclick', { bubbles: true })); return true;
    })()`);
    await page.waitFor(`[...document.querySelectorAll('#desktop .icon')].some((e) => e.getAttribute('aria-label') === 'good')`, { timeout: 10000 });
    assert.ok(await dbl('evil'), 'evil.lnk icon on the desktop');
    assert.ok(await dbl('data'), 'data.lnk icon on the desktop');
    await sleep(1500);
    assert.equal(await page.eval('window.__pwned === undefined'), true, 'a script URL ran in the shell origin');
    assert.equal(await page.eval(`document.querySelectorAll('.win').length`), 0, 'no window was opened for a script URL');
    assert.ok(await dbl('good'));
    assert.ok(await page.waitFor(`[...document.querySelectorAll('.win iframe')].some((f) => (f.getAttribute('src') || '').startsWith('/apps/notepad/'))`, { timeout: 8000 }),
      'a same-origin /apps/ link opens in a window');
    await page.eval(`document.querySelectorAll('.win button.close').forEach((b) => b.click())`);
  });

  await t.test('a sandboxed frame cannot read the clipboard or drive the OS; the shell itself still can', async () => {
    const r = await page.eval(`(async () => {
      window.postMessage({ type: 'clipboard-write', kind: 'text', data: 'top-secret' }, location.origin);   // trusted: the shell
      await new Promise((r) => setTimeout(r, 200));
      const probes = [];
      const onMsg = (e) => { if (e.data && e.data.__probe) probes.push(e.data.__probe); };
      window.addEventListener('message', onMsg);
      const f = document.createElement('iframe');
      f.setAttribute('sandbox', 'allow-scripts');       // opaque origin, like an agent-written app
      f.srcdoc = '<script>addEventListener("message",(e)=>{if(e.data&&e.data.type==="clipboard-data")parent.postMessage({__probe:"leak:"+JSON.stringify(e.data.item)},"*")});' +
        'parent.postMessage({type:"clipboard-read"},"*");parent.postMessage({type:"open-app",id:"settings"},"*");' +
        'parent.postMessage({type:"set-language",lang:"de"},"*");setTimeout(()=>parent.postMessage({__probe:"done"},"*"),900);<\\/script>';
      document.body.appendChild(f);
      await new Promise((r) => setTimeout(r, 1500));
      window.removeEventListener('message', onMsg); f.remove();
      const settingsOpen = [...document.querySelectorAll('.win iframe')].some((x) => (x.getAttribute('src') || '').startsWith('/apps/settings/'));
      window.postMessage({ type: 'open-app', id: 'notepad' }, location.origin);   // trusted control
      await new Promise((r) => setTimeout(r, 800));
      const notepadOpen = [...document.querySelectorAll('.win iframe')].some((x) => (x.getAttribute('src') || '').startsWith('/apps/notepad/'));
      return { probes, settingsOpen, notepadOpen, lang: localStorage.getItem('anima.lang') };
    })()`);
    assert.ok(r.probes.includes('done'), 'the probe frame ran');
    assert.ok(!r.probes.some((p) => p.startsWith('leak')), 'the clipboard leaked to a sandboxed frame: ' + r.probes.join(','));
    assert.equal(r.settingsOpen, false, 'a sandboxed frame launched an app');
    assert.equal(r.lang, 'en', 'a sandboxed frame changed the OS language');
    assert.equal(r.notepadOpen, true, 'the shell itself can still launch apps');
  });
});
