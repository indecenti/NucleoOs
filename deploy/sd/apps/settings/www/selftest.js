// selftest.js — the "Battito" device self-test: a SEQUENTIAL health sweep over the device's cheap,
// public, same-origin diagnostic endpoints. One request at a time (the device is single-task), each
// validated for a sane response shape, producing a per-check pass/warn/fail + a copyable plaintext
// verdict. fetch is injected so the whole sweep is unit-testable offline.
//
// ENDPOINTS is also the single source of truth for the sim-coverage meta-test: every path here MUST
// be mocked by tools/serve-shell.mjs (and exist in the firmware), so the test surface can never claim
// "all green" while silently 404-ing.

export const ENDPOINTS = [
  { key: 'status',  path: '/api/status',       kind: 'json',
    it: 'Stato dispositivo', en: 'Device status',
    check: (d) => !!d && typeof d.os === 'string' && !!d.network },
  { key: 'heap',    path: '/api/heap',         kind: 'json',
    it: 'Memoria (heap)', en: 'Memory (heap)',
    check: (d) => !!d && !!d.internal && typeof d.internal.free_bytes === 'number',
    // amber if free internal SRAM is tight (< ~12 KB) — a real near-OOM signal on this board.
    warn:  (d) => d.internal.free_bytes < 12000 },
  { key: 'cpu',     path: '/api/cpu',          kind: 'json',
    it: 'Carico CPU', en: 'CPU load',
    check: (d) => !!d && Array.isArray(d.load) },
  { key: 'wifi',    path: '/api/wifi/scan',    kind: 'json',
    it: 'Scansione Wi-Fi', en: 'Wi-Fi scan',
    check: (d) => !!d && Array.isArray(d.networks) },
  { key: 'logs',    path: '/api/logs',         kind: 'text',
    it: 'Log di sistema', en: 'System logs',
    check: (s) => typeof s === 'string' },
  { key: 'caps',    path: '/api/anima/caps',   kind: 'json',
    it: 'Motore ANIMA', en: 'ANIMA engine',
    check: (d) => !!d && ('l1Mode' in d) },
  { key: 'apps',    path: '/api/apps',         kind: 'json',
    it: 'Registro app', en: 'App registry',
    check: (d) => !!d && Array.isArray(d.apps) },
  { key: 'auth',    path: '/api/auth/status',  kind: 'json',
    it: 'Sessione', en: 'Session',
    check: (d) => !!d && ('paired' in d) },
];

// Run the whole sweep, one request at a time. `now` is injected for timing (ms). Returns
// [{key,path,ok,warn,status,shapeOk,ms,label}]. `ok=false` ⇒ unreachable or bad shape; `warn=true`
// ⇒ reachable + valid but a soft threshold tripped (e.g. heap tight).
export async function runSelfTest(fetchImpl, opts = {}) {
  const now = opts.now || (() => 0);
  const lang = opts.lang === 'en' ? 'en' : 'it';
  const results = [];
  for (const ep of ENDPOINTS) {
    const t0 = now();
    let ok = false, warn = false, status = 0, shapeOk = false;
    try {
      const r = await fetchImpl(ep.path, { method: 'GET', cache: 'no-store' });
      status = r.status || 0;
      if (r.ok) {
        const data = ep.kind === 'text' ? await r.text() : await r.json();
        shapeOk = ep.check ? !!ep.check(data) : true;
        ok = shapeOk;
        if (ok && ep.warn) { try { warn = !!ep.warn(data); } catch { warn = false; } }
      }
    } catch { ok = false; }
    results.push({ key: ep.key, path: ep.path, ok, warn, status, shapeOk, ms: now() - t0, label: ep[lang] });
  }
  return results;
}

// A human-readable, copyable verdict block from a results array.
export function formatReport(results, lang = 'it') {
  const head = lang === 'en' ? 'NucleoOS self-test' : 'Self-test NucleoOS';
  const okN = results.filter((r) => r.ok && !r.warn).length;
  const lines = results.map((r) => {
    const mark = !r.ok ? '✗' : r.warn ? '!' : '✓';
    const st = r.ok ? (r.warn ? (lang === 'en' ? 'warn' : 'attenzione') : 'ok') : (r.status ? 'HTTP ' + r.status : (lang === 'en' ? 'unreachable' : 'irraggiungibile'));
    return `${mark} ${r.label.padEnd(20)} ${r.path.padEnd(20)} ${st}`;
  });
  return `${head} — ${okN}/${results.length} OK\n` + lines.join('\n');
}
