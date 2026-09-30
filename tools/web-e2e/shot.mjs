// Visual review helper: boot the real shell against the simulator in headless Chrome and save a PNG,
// per language — so a UI change can be LOOKED AT in all five languages without flashing anything.
//   node tools/web-e2e/shot.mjs                          → build/web-e2e/shots/desktop-<lang>.png ×5
//   node tools/web-e2e/shot.mjs --lang de --size 1024x720
//   node tools/web-e2e/shot.mjs --open settings          (open an app first)   --offline (device gone)
import { join } from 'node:path';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim, REPO } from './sim.mjs';
import { LANGS, bootShell } from './shell.mjs';

const arg = (k, d) => { const i = process.argv.indexOf('--' + k); return i > 0 ? process.argv[i + 1] : d; };
const flag = (k) => process.argv.includes('--' + k);
const langs = arg('lang') ? arg('lang').split(',') : LANGS;
const [W, H] = arg('size', '1440x900').split('x').map(Number);
const outDir = arg('out', join(REPO, 'build', 'web-e2e', 'shots'));
const openApp = arg('open');

if (!findChrome()) { console.error('no Chrome/Edge found (set CHROME_PATH)'); process.exit(2); }
const browser = await launchBrowser({ args: ['--host-resolver-rules=MAP nucleo.test 127.0.0.1'] });
try {
  for (const lang of langs) {
    const sim = await startSim({ seed: { '/system/config/session.json': { windows: [], geom: {} } } });
    try {
      const page = await browser.newPage();
      await page.setViewport(W, H);
      await bootShell(page, sim, { lang });
      if (openApp) {
        await page.eval(`window.postMessage({ type: 'open-app', id: ${JSON.stringify(openApp)} }, location.origin)`);
        await new Promise((r) => setTimeout(r, 2500));
      }
      if (flag('offline')) {
        await sim.control('/api/_sim/offline', { on: true });
        await page.waitFor(`document.getElementById('link-down') && document.getElementById('link-down').classList.contains('show')`, { timeout: 15000 });
      }
      await new Promise((r) => setTimeout(r, 500));
      const file = join(outDir, `${openApp ? openApp + '-' : 'desktop-'}${flag('offline') ? 'offline-' : ''}${lang}.png`);
      console.log((await page.screenshot(file)) ? file : 'FAILED ' + file);
    } finally { await sim.stop(); }
  }
} finally { await browser.close(); }
