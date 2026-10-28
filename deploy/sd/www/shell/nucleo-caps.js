/* SPDX-License-Identifier: AGPL-3.0-or-later */
// nucleo-caps.js — what THIS device can do, asked once, in one place.
//
// Before this, every app guessed on its own: a GET to /api/rec/status here, /api/pad/state there,
// "/api/status has ota.running" somewhere else — thirty probes, each with its own idea of what a 404 or a
// 401 means, and each one a request the 4-socket Cardputer had to serve. Now an app asks
//
//   import Caps from '/nucleo-caps.js';
//   if (await Caps.has('device.mic')) showDeviceMicButton();
//
// and the answer comes from, in order:
//   1. GET /api/system/caps — the device says it (contract/api.yaml). Cached for the browser session.
//   2. a device that predates that route: recognise the edition from /api/status, then read the row of
//      /caps-fallback.json for the version it reports. The table is generated from every released version of
//      the backends (tools/contract/versions.py): exact for any shipped firmware, however old.
//   3. neither: no capability at all. Fail closed — a hidden feature is a lesser evil than a broken one.
//
// Capabilities say what EXISTS, not its live state: "the console has a joypad" is a capability, "the native
// shell is running right now so the pad is free" is state, which an app still reads from its own endpoint.
// Sandboxed apps cannot import this module; they get the same answer through the broker (sys.caps).

const CACHE_KEY = 'nucleo.caps.v1';
const EDITIONS = ['cardputer', 'handheld', 'p4', 'web'];
let pending = null;

// Which edition answered /api/status, for devices without /api/system/caps. The fields are each backend's
// own (see contract/routes.json for where they come from); anything else is "unknown".
export function editionFromStatus(s) {
  if (!s || typeof s !== 'object' || s.os !== 'NucleoOS') return null;
  if (s.storage && s.storage.fs === 'IndexedDB') return 'web';
  if (typeof s.webos === 'string' && s.mem) return 'handheld';
  if (s.ota && typeof s.ota === 'object' && 'running' in s.ota) return 'cardputer';
  if (s.ota && typeof s.ota === 'object' && 'active' in s.ota) return 'p4';
  return null;
}

// Versions as the devices report them ("0.7.5", "v1.2.189", "0.7.6-dev+12"): the leading numbers decide.
export function versionKey(v) {
  const m = /^v?(\d+)\.(\d+)\.(\d+)(?:\.(\d+))?/.exec(String(v == null ? '' : v).trim());
  return m ? [m[1], m[2], m[3], m[4] || 0].map(Number) : null;
}
function cmpVersion(a, b) {
  for (let i = 0; i < 4; i++) if (a[i] !== b[i]) return a[i] < b[i] ? -1 : 1;
  return 0;
}
// The fallback row for a device (shell/caps-fallback.json, format 2): the last row whose `since` is not above
// its version, or the `after` row when it runs firmware newer than the last release. A version that cannot be
// read, or one older than every release, gets only what EVERY release had: never a feature it may lack.
export function capsForVersion(rows, version) {
  if (!Array.isArray(rows) || !rows.length) return [];
  if (typeof rows[0] === 'string') return rows;                               // format 1: one list per edition
  const all = rows.map((r) => (Array.isArray(r.caps) ? r.caps : []));
  const common = all.reduce((acc, c) => acc.filter((x) => c.includes(x)));
  const k = versionKey(version);
  if (!k) return common;
  let pick = null;
  for (const r of rows) {
    const s = r.since != null ? versionKey(r.since) : null, a = r.after != null ? versionKey(r.after) : null;
    if ((s && cmpVersion(k, s) >= 0) || (a && cmpVersion(k, a) > 0)) pick = r;
  }
  return pick && Array.isArray(pick.caps) ? pick.caps : common;
}

function normalise(d, source) {
  const caps = Array.isArray(d && d.caps) ? d.caps.filter((c) => typeof c === 'string' && /^[a-z][a-z0-9.-]{0,63}$/.test(c)) : [];
  const edition = EDITIONS.includes(d && d.edition) ? d.edition : null;
  const limits = {};
  if (d && d.limits && typeof d.limits === 'object') {
    for (const [k, v] of Object.entries(d.limits)) if (/^[a-z_]{1,32}$/.test(k) && Number.isInteger(v) && v >= 0) limits[k] = v;
  }
  // Where things live in the file API's namespace (paths.apps: the store's install folder). Absolute,
  // no '..': a path the device gives is used to write, so nothing odd gets through.
  const paths = {};
  if (d && d.paths && typeof d.paths === 'object') {
    for (const [k, v] of Object.entries(d.paths)) {
      if (/^[a-z_]{1,32}$/.test(k) && typeof v === 'string' && /^\/(?!\/)[A-Za-z0-9._/-]{0,200}$/.test(v) && !v.split('/').includes('..')) paths[k] = v.replace(/\/+$/, '');
    }
  }
  return { source, edition, version: typeof (d && d.version) === 'string' ? d.version : '', caps, limits, paths };
}

async function getJSON(fetchFn, url) {
  const opts = { cache: 'no-store', credentials: 'same-origin' };
  if (typeof AbortSignal !== 'undefined' && AbortSignal.timeout) opts.signal = AbortSignal.timeout(5000);
  const r = await fetchFn(url, opts);
  return { status: r.status, json: r.ok ? await r.json().catch(() => null) : null };
}

// The resolution itself, with every dependency injected so it is testable without a browser.
export async function resolveCaps({ fetchFn = (...a) => fetch(...a) } = {}) {
  try {
    const r = await getJSON(fetchFn, '/api/system/caps');
    if (r.status === 200 && r.json && Number.isInteger(r.json.api) && r.json.api >= 1 && Array.isArray(r.json.caps)) {
      return normalise(r.json, 'device');
    }
  } catch {}
  try {
    const st = await getJSON(fetchFn, '/api/status');
    const edition = editionFromStatus(st.json);
    if (edition) {
      const table = await getJSON(fetchFn, '/caps-fallback.json');
      const rows = table.json && table.json.editions && table.json.editions[edition];
      const version = (st.json && st.json.version) || '';
      if (Array.isArray(rows)) return normalise({ edition, version, caps: capsForVersion(rows, version) }, 'fallback');
    }
  } catch {}
  return normalise(null, 'none');
}

function readCache() {
  try {
    const c = JSON.parse(sessionStorage.getItem(CACHE_KEY) || 'null');
    return c && Array.isArray(c.caps) && c.source !== 'none' ? c : null;
  } catch { return null; }
}

function writeCache(c) {
  try { if (c.source !== 'none') sessionStorage.setItem(CACHE_KEY, JSON.stringify(c)); } catch {}
}

const Caps = {
  // { source: 'device'|'fallback'|'none', edition, version, caps: [...], limits: {...} }
  async info() {
    if (!pending) {
      const cached = readCache();
      // 'none' (nothing reachable yet: before pairing, a busy device) is not remembered: the next call asks again.
      pending = cached ? Promise.resolve(cached) : resolveCaps().then((c) => { if (c.source === 'none') pending = null; else writeCache(c); return c; });
    }
    return pending;
  },
  async has(name) { return (await Caps.info()).caps.includes(name); },
  async limit(name, fallback) { const v = (await Caps.info()).limits[name]; return Number.isInteger(v) ? v : fallback; },
  // After pairing or an OS update the answer may change: ask again.
  reset() { pending = null; try { sessionStorage.removeItem(CACHE_KEY); } catch {} },
};

export default Caps;
