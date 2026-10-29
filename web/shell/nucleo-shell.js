/* SPDX-License-Identifier: AGPL-3.0-or-later */
// nucleo-shell.js — a system app's ONE link to the OS around it.
//
// Forty system apps each spoke to the shell in their own words: a hand-built 'open-app' query here, a native
// confirm() there (which a joypad or the device's keys cannot answer), their own /api/status poll every 1 to
// 15 seconds although the shell already polls and hands out the snapshot, and thirteen listeners for a theme
// message the shell never sent. This module is the shared answer:
//
//   import os from '/nucleo-shell.js';
//   os.openApp('mail', { to: 'ada@example.org' });       // the query is built here, encoded once
//   if (await os.confirm(t('del_body'), { danger: true })) …   // the OS dialog: keys, joypad, touch
//   os.toast(t('saved'), { kind: 'success' });
//   os.onStatus((s) => paintBattery(s.battery));          // the shell's own snapshot, no poll of yours
//   os.onTheme(({ mode }) => redraw());                    // the shell sets data-theme itself
//
// Opened on its own (outside the shell, in development) every call still works: native dialogs, a console line
// for a toast, one /api/status read. Store apps without an origin use NucleoApp (nucleo-app.js) instead.
// The shell side of every message is in shell.js (search for the type names).

// createOs(env): the same module against any window — the page's own (the default export), or a test's fakes.
export function createOs(env = globalThis) {
  const window = env.window, location = env.location, document = env.document;
  const getComputedStyle = (el) => (env.getComputedStyle || window.getComputedStyle)(el);
  const requestAnimationFrame = (fn) => (env.requestAnimationFrame || ((f) => env.setTimeout(f, 0)))(fn);
  const matchMedia = (q) => (env.matchMedia || window.matchMedia)(q);
  const setTimeout = (fn, ms) => env.setTimeout(fn, ms);
  const fetch = (...a) => env.fetch(...a);
  const navigator = env.navigator || {};
  // The shell is the TOP window: a page nested inside an app (a player inside Console) still talks to it directly,
  // and the shell recognises it by walking up to the app window that holds it (senderApp in shell.js).
  let top = null;
  try {
    const t = window.top && window.top !== window ? window.top : (window.parent && window.parent !== window ? window.parent : null);
    let same = !!t;
    try { same = !!t && (!t.location || !t.location.origin || !location.origin || t.location.origin === location.origin); } catch { same = false; }
    top = same ? t : null;
  } catch { top = null; }
  const inShell = !!top;
  const origin = location.origin || '*';
  const toParent = (msg) => { try { top.postMessage(msg, origin); } catch {} };
  let seq = 0;
  const pending = new Map();
  const listeners = { theme: new Set(), status: new Set(), event: new Map() };
  let lastStatus = null;

  window.addEventListener('message', (e) => {
    if (e.source && e.source !== window.top && e.source !== window.parent) return;   // replies come to us; events to our app window
    const d = e.data;
    if (!d || typeof d !== 'object') return;
    if (d.type === 'os-ask-result' && pending.has(d.id)) { const r = pending.get(d.id); pending.delete(d.id); r(d.value); return; }
    if (d.type === 'clipboard-data' && pending.has('clip')) { const r = pending.get('clip'); pending.delete('clip'); r(d.item || null); return; }
    if (d.type === 'nucleo.broker.event' && d.event === 'theme' && d.theme) { emitTheme(); return; }
    if (d.type === 'set-theme') { emitTheme(); return; }
    if (typeof d.t === 'string') {                       // the shell's event stream: {t, d}
      if (d.t === 'status.snapshot' && d.d) { lastStatus = d.d; for (const fn of listeners.status) { try { fn(d.d); } catch {} } }
      const set = listeners.event.get(d.t);
      if (set) for (const fn of set) { try { fn(d.d); } catch {} }
    }
  });

  // ── theme: the shell sets data-theme, --os-accent and --os-font-size on this document (injectGlobalTheme) ──
  function theme() {
    const root = document.documentElement;
    const cs = getComputedStyle(root);
    return {
      mode: root.dataset.theme === 'light' ? 'light' : 'dark',
      accent: (cs.getPropertyValue('--os-accent') || cs.getPropertyValue('--accent') || '').trim() || '#4ea1ff',
      fontSize: (cs.getPropertyValue('--os-font-size') || '').trim() || '14px',
    };
  }
  // The shell writes the attribute just before it posts the event; read it on the next frame so it is there.
  const emitTheme = () => requestAnimationFrame(() => { const th = theme(); for (const fn of listeners.theme) { try { fn(th); } catch {} } });
  if (!inShell) {
    try {
      const mq = matchMedia('(prefers-color-scheme: light)');
      if (!document.documentElement.dataset.theme) document.documentElement.dataset.theme = mq.matches ? 'light' : 'dark';
    } catch {}
  }

  // ── device status: the snapshot the shell already polls and hands to every system app ──
  async function status() {
    if (lastStatus) return lastStatus;
    if (inShell) {
      // The shell sends one as the frame loads; a frame opened before the first poll waits a moment for it.
      const got = await new Promise((resolve) => {
        const fn = (s) => { listeners.status.delete(fn); resolve(s); };
        listeners.status.add(fn);
        setTimeout(() => { listeners.status.delete(fn); resolve(null); }, 2500);
      });
      if (got) return got;
    }
    try {
      const r = await fetch('/api/status', { cache: 'no-store' });
      if (r.ok) lastStatus = await r.json();
    } catch {}
    return lastStatus;
  }

  // ── launch arguments: what the shell put in this window's URL (openFile's path, openApp's args) ──
  function args() {
    const out = {};
    for (const [k, v] of new URLSearchParams(location.search)) out[k] = v;
    return out;
  }
  function query(obj) {
    if (!obj || typeof obj !== 'object') return '';
    const q = new URLSearchParams();
    for (const [k, v] of Object.entries(obj)) if (v != null) q.set(k, String(v));
    return q.toString();
  }

  // ── dialogs: drawn by the shell (osModal), so keys, joypad and touch work the same in every app ──
  function ask(kind, text, opts = {}) {
    if (!inShell) {
      if (kind === 'confirm') return Promise.resolve(window.confirm(text));
      if (kind === 'prompt') { const v = window.prompt(text, opts.value || ''); return Promise.resolve(v == null ? null : v); }
      window.alert(text); return Promise.resolve(true);
    }
    const id = 'ask' + (++seq) + '.' + Date.now().toString(36);
    return new Promise((resolve) => {
      pending.set(id, resolve);
      toParent({ type: 'os-ask', id, kind, text: String(text == null ? '' : text), title: opts.title || '', value: opts.value || '',
        ok: opts.ok || '', cancel: opts.cancel || '', danger: !!opts.danger });
    });
  }

  const os = {
    get inShell() { return inShell; },
    theme,
    onTheme(fn) { if (typeof fn === 'function') listeners.theme.add(fn); return () => listeners.theme.delete(fn); },
    status,
    onStatus(fn) { if (typeof fn === 'function') { listeners.status.add(fn); if (lastStatus) { try { fn(lastStatus); } catch {} } } return () => listeners.status.delete(fn); },
    // Any event of the shell's stream: 'fs.changed', 'link.state', 'settings.tab', 'status.snapshot'.
    on(name, fn) {
      if (!listeners.event.has(name)) listeners.event.set(name, new Set());
      listeners.event.get(name).add(fn);
      return () => listeners.event.get(name).delete(fn);
    },
    args,
    openApp(id, a, opts = {}) {
      if (!inShell) { location.href = '/apps/' + encodeURIComponent(id) + '/' + (a ? '?' + query(a) : ''); return; }
      toParent({ type: 'open-app', id, query: query(a), ...(opts.reload ? { reload: true } : {}) });
    },
    openFile(path) { if (inShell) toParent({ type: 'open-file', path }); },
    setTitle(title) { if (inShell) toParent({ type: 'set-window-title', title: String(title || '') }); else document.title = String(title || ''); },
    close() { if (inShell) toParent({ type: 'close-window' }); else window.close(); },
    toast(text, opts = {}) {
      if (!inShell) { console.info('[toast]', text); return; }
      toParent({ type: 'os-toast', text: String(text == null ? '' : text), kind: opts.kind || 'info', ms: opts.ms || 0 });
    },
    confirm: (text, opts) => ask('confirm', text, opts),
    prompt: (text, opts) => ask('prompt', text, opts),
    alert: (text, opts) => ask('alert', text, opts),
    // The OS clipboard history (and the system clipboard where the page may use it).
    copy(data, kind = 'text') {
      if (inShell) toParent({ type: 'clipboard-write', kind, data });
      try { if (kind === 'text' && navigator.clipboard && window.isSecureContext) navigator.clipboard.writeText(String(data)).catch(() => {}); } catch {}
    },
    paste() {
      if (!inShell) return (navigator.clipboard && navigator.clipboard.readText ? navigator.clipboard.readText() : Promise.reject(new Error('no clipboard')))
        .then((t) => ({ kind: 'text', data: t }), () => null);
      return new Promise((resolve) => { pending.set('clip', resolve); toParent({ type: 'clipboard-read' }); setTimeout(() => { if (pending.get('clip') === resolve) { pending.delete('clip'); resolve(null); } }, 3000); });
    },
  };
  return os;
}

const os = typeof window !== 'undefined' ? createOs(globalThis) : null;
export default os;
