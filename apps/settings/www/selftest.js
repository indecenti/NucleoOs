/* SPDX-License-Identifier: AGPL-3.0-or-later */
// selftest.js — the "Battito" device self-test: a SEQUENTIAL health sweep over the device's cheap,
// public, same-origin diagnostic endpoints. One request at a time (the device is single-task; keeps the server load predictable), each
// validated for a sane response shape, producing a per-check pass/warn/fail + a copyable plaintext
// verdict. fetch is injected so the whole sweep is unit-testable offline.
//
// ENDPOINTS is also the single source of truth for the sim-coverage meta-test: every path here MUST
// be mocked by tools/serve-shell.mjs (and exist in the firmware), so the test surface can never claim
// "all green" while silently 404-ing.
//
// Shapes differ per backend; every check accepts all of them:
//   /api/status → {os,version,uptime_s,network:{...},...}            (Linux also carries mem:{...})
//   /api/heap   → ESP32 (Cardputer/P4): {internal:{free_bytes,...},...}
//                 Linux (Handheld):     {total,free,available,swap_total,swap_free,used_pct}   (bytes)
//   /api/cpu    → ESP32: {load:[core0,core1],...}   Linux: {mhz,governor,load,temp} (load = 1-min loadavg)
//
// `cap`: the capability (contract/capabilities.json) an endpoint belongs to. opts.has(cap) false ⇒ the
// device does not have it: the check is skipped, not failed (no request to a missing endpoint).

const SUPPORTED = ['it', 'en', 'es', 'fr', 'de'];

export const ENDPOINTS = [
  { key: 'status',  path: '/api/status',       kind: 'json',
    it: 'Stato dispositivo', en: 'Device status', es: 'Estado del dispositivo', fr: 'État de l’appareil', de: 'Gerätestatus',
    check: (d) => !!d && typeof d.os === 'string' && !!d.network },
  { key: 'heap',    path: '/api/heap',         kind: 'json', cap: 'diag',
    it: 'Memoria (RAM)', en: 'Memory (RAM)', es: 'Memoria (RAM)', fr: 'Mémoire (RAM)', de: 'Speicher (RAM)',
    check: (d) => !!d && ((!!d.internal && typeof d.internal.free_bytes === 'number')
                          || (typeof d.available === 'number' && d.available > 0)),
    // amber if free internal SRAM is tight (< ~12 KB) — a real near-OOM signal on the ESP32 boards —
    // or when Linux memory pressure is high (used_pct in the red band).
    warn:  (d) => (d.internal && typeof d.internal.free_bytes === 'number')
                    ? d.internal.free_bytes < 12000 : (d.used_pct || 0) >= 90 },
  { key: 'cpu',     path: '/api/cpu',          kind: 'json', cap: 'diag',
    it: 'Carico CPU', en: 'CPU load', es: 'Carga de CPU', fr: 'Charge CPU', de: 'CPU-Last',
    check: (d) => !!d && (Array.isArray(d.load) || typeof d.load === 'number') },
  { key: 'wifi',    path: '/api/wifi/scan',    kind: 'json', cap: 'wifi.manage',
    it: 'Scansione Wi-Fi', en: 'Wi-Fi scan', es: 'Escaneo Wi-Fi', fr: 'Analyse Wi-Fi', de: 'Wi-Fi-Scan',
    check: (d) => !!d && Array.isArray(d.networks) },
  { key: 'logs',    path: '/api/logs',         kind: 'text', cap: 'diag',
    it: 'Log di sistema', en: 'System logs', es: 'Registros del sistema', fr: 'Journaux système', de: 'Systemprotokolle',
    check: (s) => typeof s === 'string' },
  { key: 'caps',    path: '/api/anima/caps',   kind: 'json',
    it: 'Motore ANIMA', en: 'ANIMA engine', es: 'Motor ANIMA', fr: 'Moteur ANIMA', de: 'ANIMA-Engine',
    check: (d) => !!d && ('l1Mode' in d) },
  { key: 'apps',    path: '/api/apps',         kind: 'json',
    it: 'Registro app', en: 'App registry', es: 'Registro de apps', fr: 'Registre des apps', de: 'App-Register',
    check: (d) => !!d && Array.isArray(d.apps) },
  { key: 'auth',    path: '/api/auth/status',  kind: 'json',
    it: 'Accoppiamento', en: 'Pairing', es: 'Emparejamiento', fr: 'Appairage', de: 'Kopplung',
    check: (d) => !!d && ('paired' in d) },   // `auth` is Handheld/Web only; the ESP32 boards send {required,paired}
];

// Run the whole sweep, one request at a time. `now` is injected for timing (ms). Returns
// [{key,path,ok,warn,status,shapeOk,ms,label}]. `ok=false` ⇒ unreachable or bad shape; `warn=true`
// ⇒ reachable + valid but a soft threshold tripped (e.g. heap tight / memory pressure high).
export async function runSelfTest(fetchImpl, opts = {}) {
  const now = opts.now || (() => 0);
  // Labels exist in all five OS languages; anything else falls back to English (the OS base
  // language, CP's floor) — no opts.lang → en.
  const lang = SUPPORTED.includes(opts.lang) ? opts.lang : 'en';
  const has = typeof opts.has === 'function' ? opts.has : () => true;
  const results = [];
  for (const ep of ENDPOINTS) {
    if (ep.cap && !has(ep.cap)) continue;
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
const WORDS = {
  head:        { it: 'Self-test NucleoOS', en: 'NucleoOS self-test', es: 'Autodiagnóstico NucleoOS', fr: 'Autotest NucleoOS', de: 'NucleoOS-Selbsttest' },
  warn:        { it: 'attenzione', en: 'warn', es: 'aviso', fr: 'avertissement', de: 'Warnung' },
  unreachable: { it: 'irraggiungibile', en: 'unreachable', es: 'inaccesible', fr: 'injoignable', de: 'unerreichbar' },
};
export function formatReport(results, lang = 'en') {
  const L = SUPPORTED.includes(lang) ? lang : 'en';
  const okN = results.filter((r) => r.ok && !r.warn).length;
  const lines = results.map((r) => {
    const mark = !r.ok ? '✗' : r.warn ? '!' : '✓';
    const st = r.ok ? (r.warn ? WORDS.warn[L] : 'ok') : (r.status ? 'HTTP ' + r.status : WORDS.unreachable[L]);
    return `${mark} ${r.label.padEnd(20)} ${r.path.padEnd(20)} ${st}`;
  });
  return `${WORDS.head[L]} — ${okN}/${results.length} OK\n` + lines.join('\n');
}
