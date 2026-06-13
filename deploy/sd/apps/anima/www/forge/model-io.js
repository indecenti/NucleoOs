// model-io.js — the concrete I/O adapters that make model-fetch.js / sd-model-loader.js run for real against
// the network (CDN) and the Cardputer device (/api/fs, /api/status). Everything heavy is parameterised
// (fetchImpl, crypto) so these same factories run under Node for the host gate. The pure controllers never
// import this file directly — the UI wires these in — which is what keeps the controllers DOM-free.
//
// Persistence is deliberately two-tier (the resume contract):
//   • browser cache  (localStorage) {name->sha256}: fast, survives a reload, may be wiped by the user.
//   • device index   (SD JSON file): the ground truth for what actually sits on disk.
// On resume the device index wins; localStorage is only a hint. A shard counts as "have it" only if BOTH
// agree, so a cleared cache or a half-synced SD never silently skips a missing shard.

const HEX = Array.from({ length: 256 }, (_, b) => b.toString(16).padStart(2, '0'));
export function hexFromBuffer(buf) {
  const u = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
  let s = ''; for (let i = 0; i < u.length; i++) s += HEX[u[i]];
  return s;
}

// SHA-256 over real bytes via Web Crypto (works in browsers and Node ≥ 18 globalThis.crypto.subtle).
export function makeSha256({ crypto = globalThis.crypto } = {}) {
  if (!crypto || !crypto.subtle) throw new Error('WebCrypto unavailable');
  return async function sha256(bytes) {
    const u = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    const d = await crypto.subtle.digest('SHA-256', u);
    return hexFromBuffer(d);
  };
}

// CDN range fetcher: one bounded HTTP Range GET per call → real bytes. baseUrl is the model's shard root.
export function makeCdnRangeFetcher(baseUrl, { fetchImpl = globalThis.fetch } = {}) {
  const root = String(baseUrl).replace(/\/$/, '');
  return async function fetchRange(shardName, { start, end, header } = {}) {
    try {
      const r = await fetchImpl(`${root}/${shardName}`, { headers: { Range: header || `bytes=${start}-${end}` } });
      if (!r.ok && r.status !== 206 && r.status !== 200) return { ok: false, status: r.status };
      const buf = await r.arrayBuffer();
      return { ok: true, status: r.status, chunk: new Uint8Array(buf) };
    } catch (e) { return { ok: false, status: 0, error: String(e && e.message || e) }; }
  };
}

// SD part reader: GET /api/fs/read?path=… → real bytes (used by sd-model-loader and the SD resume source).
export function makeSdPartReader({ fetchImpl = globalThis.fetch } = {}) {
  return async function readPart(path) {
    try {
      const r = await fetchImpl('/api/fs/read?path=' + encodeURIComponent(path) + '&t=' + Date.now(), { cache: 'no-store' });
      if (!r.ok) return { ok: false, status: r.status };
      const buf = await r.arrayBuffer();
      return { ok: true, status: r.status, bytes: new Uint8Array(buf) };
    } catch (e) { return { ok: false, status: 0, error: String(e && e.message || e) }; }
  };
}

// SD sink: persist one verified ≤7 MB shard as a part file under destDir. The shard name already carries the
// .NNN suffix, so this is a single binary POST (raw body, never FormData — the PNG-corruption lesson). The
// 7 MB shard size keeps each write under the httpd reset ceiling. Idempotent: a re-run overwrites in place.
export function makeSdSink(destDir, { fetchImpl = globalThis.fetch } = {}) {
  const dir = String(destDir).replace(/\/$/, '');
  return async function writeShard(name, bytes) {
    try {
      const body = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
      const r = await fetchImpl('/api/fs/write?path=' + encodeURIComponent(`${dir}/${name}`), { method: 'POST', body });
      return r.ok ? { ok: true } : { ok: false, error: 'http-' + r.status };
    } catch (e) { return { ok: false, error: String(e && e.message || e) }; }
  };
}

// Device telemetry for the coexistence scheduler. Polls /api/status at most once per ttl (avoid hammering
// the single-task httpd). Returns { freeHeap, largestBlock, verifyInFlight }. The verifyInFlight flag is fed
// from app state via getVerifyInFlight (an in-flight /api/anima request must pause SD pulls).
export function makeTelemetry({ fetchImpl = globalThis.fetch, statusUrl = '/api/status', ttlMs = 5000, getVerifyInFlight, now = () => Date.now() } = {}) {
  let cached = { freeHeap: Infinity, largestBlock: Infinity, verifyInFlight: false };
  let at = -Infinity;
  return async function telemetry() {
    const t = now();
    if (t - at < ttlMs) return { ...cached, verifyInFlight: !!(getVerifyInFlight && getVerifyInFlight()) };
    try {
      const r = await fetchImpl(statusUrl, { cache: 'no-store' });
      if (r.ok) { const j = await r.json();
        cached = { freeHeap: j.freeHeap ?? j.heap_free ?? Infinity, largestBlock: j.largestBlock ?? j.heap_largest ?? Infinity, verifyInFlight: false };
        at = t;
      }
    } catch { /* keep last good telemetry */ }
    return { ...cached, verifyInFlight: !!(getVerifyInFlight && getVerifyInFlight()) };
  };
}

// ---- resume persistence ----------------------------------------------------------------------------------

// localStorage-backed cache {name->sha256}, keyed per model@revision so two models never collide.
export function makeLocalCacheStore(key, { storage = (typeof localStorage !== 'undefined' ? localStorage : null) } = {}) {
  const k = 'dl-cache:' + key;
  return {
    loadCache() { try { return JSON.parse(storage.getItem(k) || '{}'); } catch { return {}; } },
    saveCache(cache) { try { storage.setItem(k, JSON.stringify(cache)); } catch { /* quota: resume just slower */ } },
    removeCache() { try { storage.removeItem(k); } catch {} },
  };
}

// Device-side index (SD JSON ground truth). load() returns {name->sha256} from /data/cache/models/<key>.json;
// save() writes it back. Tolerates a missing/corrupt file (returns {}). Used to verify the browser cache
// against what is really on disk (a half-synced SD must re-fetch its missing shards).
export function makeDeviceIndexStore(key, { fetchImpl = globalThis.fetch, dir = '/data/cache/models' } = {}) {
  const path = `${dir}/${String(key).replace(/[^a-z0-9_.@-]/gi, '_')}.json`;
  return {
    path,
    async load() {
      try { const r = await fetchImpl('/api/fs/read?path=' + encodeURIComponent(path) + '&t=' + Date.now(), { cache: 'no-store' });
        if (!r.ok) return {}; const j = await r.json(); return (j && j.shards) || {}; } catch { return {}; }
    },
    async save(cache) {
      try { await fetchImpl('/api/fs/write?path=' + encodeURIComponent(path), { method: 'POST', body: JSON.stringify({ key, ts: Date.now(), shards: cache }) }); } catch {}
    },
  };
}

// Merge the two tiers into the { loadCache, saveCache } shape model-fetch wants. A shard is considered DONE
// only if the device index lists it (disk truth); the browser cache is used to avoid an /api/fs round-trip
// for the common warm-reload case but is intersected with the device index so it can never over-skip.
export function makeResumeStore(key, opts = {}) {
  const local = makeLocalCacheStore(key, opts);
  const device = opts.device || makeDeviceIndexStore(key, opts);
  return {
    async loadCache() {
      const l = local.loadCache();
      let d = {}; try { d = await device.load(); } catch { d = {}; }
      if (!d || !Object.keys(d).length) return l;          // no device index yet → trust local (cold first run)
      const merged = {};                                    // intersect: keep only shards both agree on
      for (const [n, sha] of Object.entries(d)) if (l[n] === undefined || l[n] === sha) merged[n] = sha;
      return merged;
    },
    async saveCache(cache) { local.saveCache(cache); try { await device.save(cache); } catch {} },
    removeCache() { local.removeCache(); },
  };
}
