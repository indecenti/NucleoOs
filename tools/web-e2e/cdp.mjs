// Minimal Chrome DevTools Protocol driver for the browser E2E suite — zero npm dependencies.
// Node >= 22 ships a global WebSocket, so this is all it takes to drive a real headless Chrome/Edge:
// launch it, attach to a page (and to any out-of-process iframe), evaluate JS, and collect every
// uncaught exception, console error and failed request the shell or an app produces.
import { spawn } from 'node:child_process';
import { mkdtemp, rm, writeFile, mkdir } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';

export function findChrome() {
  if (process.env.CHROME_PATH && existsSync(process.env.CHROME_PATH)) return process.env.CHROME_PATH;
  const env = process.env;
  const cands = process.platform === 'win32' ? [
    `${env.PROGRAMFILES}\\Google\\Chrome\\Application\\chrome.exe`,
    `${env['PROGRAMFILES(X86)']}\\Google\\Chrome\\Application\\chrome.exe`,
    `${env.LOCALAPPDATA}\\Google\\Chrome\\Application\\chrome.exe`,
    `${env['PROGRAMFILES(X86)']}\\Microsoft\\Edge\\Application\\msedge.exe`,
    `${env.PROGRAMFILES}\\Microsoft\\Edge\\Application\\msedge.exe`,
  ] : process.platform === 'darwin' ? [
    '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
    '/Applications/Chromium.app/Contents/MacOS/Chromium',
  ] : ['/usr/bin/google-chrome', '/usr/bin/google-chrome-stable', '/usr/bin/chromium', '/usr/bin/chromium-browser', '/snap/bin/chromium'];
  return cands.find((p) => p && !p.includes('undefined') && existsSync(p)) || null;
}

class Connection {
  constructor(ws) {
    this.ws = ws; this.id = 0; this.pending = new Map(); this.listeners = new Set();
    ws.addEventListener('message', (m) => {
      const msg = JSON.parse(typeof m.data === 'string' ? m.data : Buffer.from(m.data).toString('utf8'));
      if (msg.id !== undefined) {
        const p = this.pending.get(msg.id); if (!p) return;
        this.pending.delete(msg.id);
        if (msg.error) p.reject(new Error(`${p.method}: ${msg.error.message}`)); else p.resolve(msg.result);
        return;
      }
      for (const fn of this.listeners) { try { fn(msg.method, msg.params || {}, msg.sessionId); } catch {} }
    });
    ws.addEventListener('close', () => { for (const p of this.pending.values()) p.reject(new Error('CDP connection closed')); this.pending.clear(); });
  }
  static connect(url) {
    return new Promise((resolve, reject) => {
      const ws = new WebSocket(url);
      ws.addEventListener('open', () => resolve(new Connection(ws)), { once: true });
      ws.addEventListener('error', () => reject(new Error('CDP connect failed: ' + url)), { once: true });
    });
  }
  send(method, params = {}, sessionId) {
    const id = ++this.id;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject, method });
      this.ws.send(JSON.stringify(sessionId ? { id, method, params, sessionId } : { id, method, params }));
    });
  }
  on(fn) { this.listeners.add(fn); return () => this.listeners.delete(fn); }
}

// Launch headless Chrome. Returns null when no Chrome/Edge is installed (callers SKIP, not fail).
// gpu:true keeps the real GPU (WebGPU / WebLLM tests); the default headless run disables it for stability.
// profileDir: a PERSISTENT profile (e.g. to keep a downloaded model between runs); default = throwaway.
export async function launchBrowser({ args = [], gpu = false, profileDir = null } = {}) {
  const exe = findChrome();
  if (!exe) return null;
  const profile = profileDir || await mkdtemp(join(tmpdir(), 'nucleo-e2e-chrome-'));
  if (profileDir) await mkdir(profileDir, { recursive: true });
  const proc = spawn(exe, [
    '--headless=new', '--remote-debugging-port=0', `--user-data-dir=${profile}`,
    '--no-first-run', '--no-default-browser-check', '--disable-extensions', '--mute-audio',
    ...(gpu ? ['--enable-unsafe-webgpu', '--ignore-gpu-blocklist', '--force_high_performance_gpu'] : ['--disable-gpu']), '--disable-background-timer-throttling', '--disable-renderer-backgrounding',
    '--disable-backgrounding-occluded-windows', '--disable-features=Translate,MediaRouter',
    '--autoplay-policy=no-user-gesture-required', '--window-size=1440,900',
    // CI only: Ubuntu 24.04 runners restrict the unprivileged user namespaces Chrome's sandbox needs.
    ...(process.env.CI ? ['--no-sandbox'] : []),
    ...args, 'about:blank',
  ], { stdio: ['ignore', 'ignore', 'pipe'] });
  const wsUrl = await new Promise((resolve, reject) => {
    let buf = '';
    const t = setTimeout(() => reject(new Error('Chrome did not expose DevTools within 30s')), 30000);
    proc.stderr.on('data', (d) => {
      buf += d;
      const m = buf.match(/DevTools listening on (ws:\/\/\S+)/);
      if (m) { clearTimeout(t); resolve(m[1]); }
    });
    proc.on('exit', (code) => { clearTimeout(t); reject(new Error('Chrome exited early, code ' + code)); });
  });
  const conn = await Connection.connect(wsUrl);
  return {
    conn,
    async newPage() { return newPage(conn); },
    async close() {
      try { await Promise.race([conn.send('Browser.close'), new Promise((r) => setTimeout(r, 3000))]); } catch {}
      try { proc.kill(); } catch {}
      await new Promise((r) => setTimeout(r, 300));
      if (!profileDir) await rm(profile, { recursive: true, force: true }).catch(() => {});
    },
  };
}

// One isolated page (own browser context = own cookies/localStorage) with an event collector.
async function newPage(conn) {
  const { browserContextId } = await conn.send('Target.createBrowserContext', { disposeOnDetach: true });
  const { targetId } = await conn.send('Target.createTarget', { url: 'about:blank', browserContextId });
  const { sessionId } = await conn.send('Target.attachToTarget', { targetId, flatten: true });
  const sessions = new Set([sessionId]);
  const log = [];                    // { kind: 'exception'|'console'|'response'|'failed', ... , t }
  const inflight = new Map();        // requestId -> url, until the response ARRIVES (an app that never reads
                                     // a fire-and-forget body would otherwise look like a request stuck forever)
  const urls = new Map();            // requestId -> url, for failure reports
  let lastNetActivity = Date.now();
  const consoleInfo = [];            // console.info/log lines (boot milestones)

  const enable = async (sid) => {
    await conn.send('Runtime.enable', {}, sid);
    await conn.send('Network.enable', {}, sid);
    await conn.send('Page.enable', {}, sid).catch(() => {});
    await conn.send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: false, flatten: true }, sid).catch(() => {});
  };
  conn.on((method, p, sid) => {
    if (!sessions.has(sid) && method !== 'Target.attachedToTarget') return;
    const t = Date.now();
    switch (method) {
      case 'Target.attachedToTarget':
        if (!sessions.has(sid)) return;
        sessions.add(p.sessionId); enable(p.sessionId).catch(() => {});
        break;
      case 'Runtime.exceptionThrown': {
        const d = p.exceptionDetails || {};
        log.push({ kind: 'exception', t, text: (d.exception && (d.exception.description || d.exception.value)) || d.text, url: d.url, line: d.lineNumber });
        break;
      }
      case 'Runtime.consoleAPICalled': {
        const text = (p.args || []).map((a) => a.value !== undefined ? String(a.value) : (a.description || a.type)).join(' ');
        if (p.type === 'error') log.push({ kind: 'console', t, text });
        else consoleInfo.push({ t, type: p.type, text });
        break;
      }
      case 'Network.requestWillBeSent': inflight.set(p.requestId, p.request.url); urls.set(p.requestId, p.request.url); lastNetActivity = t; break;
      case 'Network.responseReceived':
        if (p.response.status >= 400) log.push({ kind: 'response', t, status: p.response.status, url: p.response.url });
        if (p.type !== 'EventSource') inflight.delete(p.requestId);
        lastNetActivity = t;
        break;
      case 'Network.loadingFinished': inflight.delete(p.requestId); urls.delete(p.requestId); lastNetActivity = t; break;
      case 'Network.loadingFailed':
        if (!p.canceled && p.errorText !== 'net::ERR_ABORTED') log.push({ kind: 'failed', t, text: p.errorText, url: urls.get(p.requestId) });
        inflight.delete(p.requestId); urls.delete(p.requestId); lastNetActivity = t;
        break;
      case 'Page.javascriptDialogOpening':        // never let an app's alert()/confirm() hang the suite
        conn.send('Page.handleJavaScriptDialog', { accept: true }, sid).catch(() => {});
        log.push({ kind: 'dialog', t, text: p.message });
        break;
    }
  });
  await enable(sessionId);

  const send = (m, params) => conn.send(m, params, sessionId);
  const page = {
    log, consoleInfo, sessionId,
    send,                            // raw CDP on this page's session (e.g. Fetch interception)
    pending: () => [...inflight.values()],
    mark: () => log.length,
    since: (m) => log.slice(m),
    // Run `source` in every document of this page before its own scripts (instrumentation that must not miss
    // the first milliseconds of a boot, whatever the machine's load).
    initScript: (source) => send('Page.addScriptToEvaluateOnNewDocument', { source }),
    async eval(expression, timeout = 30000) {
      const r = await send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true, timeout });
      if (r.exceptionDetails) throw new Error('eval failed: ' + ((r.exceptionDetails.exception && r.exceptionDetails.exception.description) || r.exceptionDetails.text));
      return r.result.value;
    },
    async goto(url) {
      const loaded = new Promise((resolve) => {
        const off = conn.on((m, _p, sid) => { if (sid === sessionId && m === 'Page.loadEventFired') { off(); resolve(); } });
        setTimeout(() => { off(); resolve(); }, 30000);
      });
      inflight.clear(); urls.clear(); // requests of the page we are leaving never report loadingFinished
      await send('Page.navigate', { url });
      await loaded;
    },
    async waitFor(expression, { timeout = 15000, interval = 100 } = {}) {
      const end = Date.now() + timeout;
      for (;;) {
        let v = null; try { v = await page.eval(expression); } catch {}
        if (v) return v;
        if (Date.now() > end) return null;
        await new Promise((r) => setTimeout(r, interval));
      }
    },
    // Network idle: nothing in flight (WebSockets are not counted) for `quiet` ms.
    async networkIdle({ quiet = 500, timeout = 15000 } = {}) {
      const end = Date.now() + timeout;
      while (Date.now() < end) {
        if (inflight.size === 0 && Date.now() - lastNetActivity >= quiet) return true;
        await new Promise((r) => setTimeout(r, 50));
      }
      return false;
    },
    async screenshot(file) {
      try {
        const { data } = await send('Page.captureScreenshot', { format: 'png' });
        await mkdir(dirname(file), { recursive: true });
        await writeFile(file, Buffer.from(data, 'base64'));
        return true;
      } catch (e) { if (process.env.E2E_DEBUG) console.error('[screenshot]', file, e && e.message); return false; }
    },
    async setViewport(width, height, mobile = false) {
      await send('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: 1, mobile });
    },
  };
  return page;
}
