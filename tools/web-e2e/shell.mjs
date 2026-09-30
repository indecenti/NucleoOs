// Shell-level helpers for the browser E2E suite: pair + boot the desktop, open every app through the
// real Start menu, and classify what went wrong. Shared by every *.test.mjs in this folder.

export const LANGS = ['it', 'en', 'es', 'fr', 'de'];

// Pair (the simulator exposes the on-screen PIN at /api/_dev/pin), pick the language, skip the
// first-run wizard, then load the desktop and wait until it is interactive and quiet.
// origin: override the page origin (e.g. sim.local = http://localhost:<port>, a SECURE context).
export async function bootShell(page, sim, { lang = 'en', onboarded = true, wait = true, origin = sim.origin } = {}) {
  await page.goto(origin + '/api/status');          // any same-origin page: prepares cookie + storage
  const L = JSON.stringify(lang);
  await page.eval(`(async () => {
    localStorage.setItem('anima.lang', ${L});
    ${onboarded ? "localStorage.setItem('nucleo.onboarded', '1');" : "localStorage.removeItem('nucleo.onboarded');"}
    const { pin } = await fetch('/api/_dev/pin').then((r) => r.json());
    const r = await fetch('/api/pair', { method: 'POST', body: JSON.stringify({ pin }) });
    await fetch('/api/lang', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ lang: ${L} }) });
    return r.status;
  })()`);
  await sim.control('/api/_sim/stats', { reset: true });
  await page.goto(origin + '/');
  if (!wait) return true;
  const up = await waitDesktop(page);
  await page.networkIdle({ quiet: 800, timeout: 20000 });
  return up;
}

export const waitDesktop = (page, timeout = 30000) =>
  page.waitFor(`document.querySelectorAll('#desktop .icon').length > 0 && document.querySelectorAll('#sm-all .sm-row').length > 0`, { timeout });

const LAST_WIN_READY = `(() => {
  const w = [...document.querySelectorAll('.win')].pop();
  if (!w) return false;
  const f = w.querySelector('iframe');
  if (!f) return true;
  try { const d = f.contentDocument; return d ? (d.readyState === 'complete' && d.location.href !== 'about:blank') : true; } catch { return true; }
})()`;

const LAST_WIN_INFO = `(() => {
  const w = [...document.querySelectorAll('.win')].pop();
  if (!w) return { opened: false };
  const f = w.querySelector('iframe');
  let bodyLen = null, sandboxed = false;
  try { const d = f && f.contentDocument; if (d && d.body) bodyLen = d.body.innerText.trim().length; else sandboxed = !!f; } catch { sandboxed = true; }
  const r = w.getBoundingClientRect();
  return { opened: true, bodyLen, sandboxed, w: Math.round(r.width), h: Math.round(r.height),
    offscreen: r.right < 40 || r.bottom < 40 || r.left > innerWidth - 40 || r.top > innerHeight - 40 };
})()`;

export async function appRows(page) {
  return page.eval(`[...document.querySelectorAll('#sm-all .sm-row')].map((r) => r.title)`);
}

export async function closeAllWindows(page) {
  await page.eval(`document.querySelectorAll('.win button.close').forEach((b) => b.click())`);
  await page.waitFor(`document.querySelectorAll('.win').length === 0`, { timeout: 4000 });
}

// Open app #i of the Start menu's "All apps" list the way a user does (click the row), wait until
// its window has loaded and the network is quiet, and return what happened meanwhile.
export async function openAppAt(page, i) {
  const m = page.mark();
  const name = await page.eval(`(() => { const r = document.querySelectorAll('#sm-all .sm-row')[${i}]; r.click(); return r.title; })()`);
  await page.waitFor(LAST_WIN_READY, { timeout: 12000 });
  await page.networkIdle({ quiet: 400, timeout: 8000 });
  const info = await page.eval(LAST_WIN_INFO);
  return { name, info, events: page.since(m) };
}

// What counts as a defect. A 404 from the file API is a normal answer ("that file does not exist
// yet"), and third-party hosts (weather, LLM providers) are outside the device's control.
export function defects(events, origin) {
  const own = (u) => typeof u === 'string' && u.startsWith(origin);
  return events.filter((e) => {
    if (e.kind === 'exception') return true;
    if (e.kind === 'console') return /\[fault\]/.test(e.text);
    if (e.kind === 'response') {
      if (!own(e.url)) return false;
      const p = new URL(e.url).pathname;
      if (e.status === 404 && /^\/api\/fs\/(read|list)$/.test(p)) return false;
      if (p === '/favicon.ico') return false;          // the suite's own JSON prep page, not the shell (it declares icon.png)
      // Agent/Forge presence probe: a model whose manifest is not on the SD resolves from the embedded
      // registry (apps/agent/www/engine-picker.js getManifest → null). Intentional, not a defect.
      if (e.status === 404 && /^\/apps\/anima\/forge\/models\/[^/]+\/manifest\.json$/.test(p)) return false;
      return true;
    }
    if (e.kind === 'failed') return own(e.url);
    return false;
  }).map((e) => e.kind === 'response' ? `HTTP ${e.status} ${new URL(e.url).pathname}${new URL(e.url).search}`
    : e.kind === 'failed' ? `FAILED ${e.text} ${e.url}` : `${e.kind.toUpperCase()}: ${String(e.text).split('\n').slice(0, 3).join(' | ')}`);
}
