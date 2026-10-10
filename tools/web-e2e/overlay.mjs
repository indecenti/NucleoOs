// Working-tree overlay for the REAL-device browser tests: every web file (the shell, the apps) is served to the page
// from this repo's working tree, while every /api/* call still goes to the Cardputer. A fix is then proven on the real
// hardware — its SD, live status, executors, the 4-socket httpd — BEFORE anything is copied to the card.
// Used by anima-device.mjs and device-smoke.mjs (--overlay).
import { readFileSync, existsSync, statSync } from 'node:fs';
import { join, extname } from 'node:path';
import { REPO } from './sim.mjs';

const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8', '.json': 'application/json; charset=utf-8', '.svg': 'image/svg+xml', '.png': 'image/png',
  '.jpg': 'image/jpeg', '.ico': 'image/x-icon', '.wasm': 'application/wasm', '.webmanifest': 'application/manifest+json', '.txt': 'text/plain; charset=utf-8' };

// The repo file a device URL maps to (the firmware's webfs: /apps/<id>/<rest> → apps/<id>/www/<rest>, else the shell),
// or null for what only the device has: /api, the user's data, a user-installed app.
export function overlayFile(pathname) {
  if (/^\/(api|data|system|sd)\//.test(pathname)) return null;
  const p = decodeURIComponent(pathname);
  const m = /^\/apps\/([^/]+)\/(.*)$/.exec(p);
  const abs = join(REPO, m ? join('apps', m[1], 'www', m[2] || 'index.html') : join('web', 'shell', p === '/' ? 'index.html' : p.slice(1)));
  if (!abs.startsWith(REPO) || !existsSync(abs) || statSync(abs).isDirectory()) return null;
  return abs;
}

// Intercept the page's requests (Fetch domain on its session: same-origin iframes included). → () => files served.
export async function enableOverlay(browser, page, origin) {
  let served = 0;
  browser.conn.on(async (method, p, sid) => {
    if (method !== 'Fetch.requestPaused' || sid !== page.sessionId) return;
    const cont = () => browser.conn.send('Fetch.continueRequest', { requestId: p.requestId }, sid).catch(() => {});
    try {
      const u = new URL(p.request.url);
      const abs = u.origin === origin && p.request.method === 'GET' ? overlayFile(u.pathname) : null;
      if (!abs) return cont();
      served++;
      await browser.conn.send('Fetch.fulfillRequest', { requestId: p.requestId, responseCode: 200, body: readFileSync(abs).toString('base64'),
        responseHeaders: [{ name: 'Content-Type', value: MIME[extname(abs).toLowerCase()] || 'application/octet-stream' }, { name: 'Cache-Control', value: 'no-store' }] }, sid);
    } catch { cont(); }
  });
  await page.send('Fetch.enable', { patterns: [{ urlPattern: origin + '/*', requestStage: 'Request' }] });
  return () => served;
}
