// device-session.mjs — ONE paired session per device for every dev tool (push-files, push-ota, sd-net-sync).
//
// Each POST /api/pair mints a new session token, and the device keeps 32 of them (LRU, nucleo_auth/auth_slots.c).
// Tools that paired on every run evicted the user's own browser session after a day of pushes — Chrome landed
// on "Associa questo dispositivo" again. The cookie is cached per host in tools/.device-sessions.json
// (gitignored) and re-validated with a tiny paired-only GET (/api/cpu) before use; it is re-paired only when
// the device refuses it.
import { readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const CACHE = join(dirname(fileURLToPath(import.meta.url)), '..', '.device-sessions.json');
const load = () => { try { return JSON.parse(readFileSync(CACHE, 'utf8')); } catch { return {}; } };
const save = (o) => { try { writeFileSync(CACHE, JSON.stringify(o, null, 2) + '\n'); } catch {} };

const withTimeout = async (f, url, opt, ms) => {
  const c = new AbortController(); const t = setTimeout(() => c.abort(), ms);
  try { return await f(url, { ...opt, signal: c.signal }); } finally { clearTimeout(t); }
};

// → 'nucleo_session=…' cookie string. Throws Error with .status on a refused PIN.
export async function deviceSession(host, pin, { fetch: f = globalThis.fetch, timeoutMs = 15000, fresh = false } = {}) {
  host = String(host).replace(/\/+$/, '');
  const cache = load();
  const cached = !fresh && cache[host];
  if (cached) {
    try {
      const r = await withTimeout(f, host + '/api/cpu', { headers: { cookie: cached } }, timeoutMs);
      if (r.ok) return cached;
      if (r.status !== 401 && r.status !== 403) return cached;   // busy / 5xx: the session itself is not the problem
    } catch { return cached; }                                     // unreachable: let the caller's own request report it
  }
  if (!pin) { const e = new Error('no cached session for ' + host + ' and no PIN given'); e.status = 401; throw e; }
  const r = await withTimeout(f, host + '/api/pair', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ pin }) }, timeoutMs);
  if (!r.ok) { const e = new Error('pairing rejected (HTTP ' + r.status + ')'); e.status = r.status; throw e; }
  const m = /(?:^|,\s*)(nucleo_session=[^;]+)/.exec(r.headers.get('set-cookie') || '');
  if (!m) throw new Error('paired but no session cookie returned');
  cache[host] = m[1]; save(cache);
  return m[1];
}
