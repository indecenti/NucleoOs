// ANIMA Local — the device's offline cascade, compiled to WebAssembly, running in THIS browser.
//
// This is a DROP-IN substitute for the Cardputer's offline tier: the knowledge pack is downloaded
// from the device ONCE over /api/fs/read (then cached in IndexedDB, so it works with the device
// off), mounted into the WASM in-memory filesystem, and queries run the EXACT same cascade as the
// firmware — L0 intents + L1 retrieval + HDC/KGE deduction + facet/profile/learn — just on the
// client's far larger CPU+RAM instead of the MCU. query() returns the same JSON shape as the device
// /api/anima endpoint (this module plays the role nucleo_httpd.c plays on the device: it shapes the
// raw anima_result_t struct into the web contract), so the web app consumes it with zero changes.
//
// Fidelity is certified by apps/anima/local/parity.mjs (WASM == native anima.exe, 16/16).
// The engine source is a decoupled snapshot in apps/anima/local/ — it evolves independently of the
// firmware (full-recall flat index in RAM, room to grow far past the MCU's limits).

// Flat-index brain: full-corpus recall, RAM-resident. `req` files must be present (L1 core); the
// rest (learned facts, dictionaries) are best-effort — the cascade abstains gracefully if absent.
const PACK = [
  { p: 'data/anima/anima-it-encoder.bin',     req: true  },
  { p: 'data/anima/anima-it-index.bin',       req: true  },
  { p: 'data/anima/commands.it.json',         req: true  },
  { p: 'data/anima/dict-it-en.tsv',           req: false },
  { p: 'data/anima/dict-en-it.tsv',           req: false },
  { p: 'data/anima/learned/mind.it.jsonl',    req: false },
  { p: 'data/anima/learned/mind.en.jsonl',    req: false },
  { p: 'data/anima/learned/facets.it.jsonl',  req: false },
  { p: 'data/anima/learned/facets.en.jsonl',  req: false },
  // UNIFIED brain (AKB5 sharded): the SAME knowledge the device now ships at /data/anima/ (base + extended,
  // RAM-flat one-shard-at-a-time). The WASM (ANIMA_AKB5=1) loads the manifest + shards; the shards are
  // enumerated dynamically at load. Absent -> the flat index above is used. Single source of truth.
  { p: 'data/anima/anima-it-akb5.bin', req: false },
];

const CACHE_VER = 2;                       // bump to invalidate the IndexedDB pack cache (2: unified AKB5)
const DB_NAME = 'anima-local', STORE = 'pack';

// ---- tiny IndexedDB blob cache (key = ver:path -> Uint8Array) -------------------------------------
function openDB() {
  return new Promise((res, rej) => {
    const o = indexedDB.open(DB_NAME, 1);
    o.onupgradeneeded = () => { if (!o.result.objectStoreNames.contains(STORE)) o.result.createObjectStore(STORE); };
    o.onsuccess = () => res(o.result);
    o.onerror = () => rej(o.error);
  });
}
async function cacheGet(path) {
  try {
    const db = await openDB();
    return await new Promise((res) => {
      const r = db.transaction(STORE).objectStore(STORE).get(CACHE_VER + ':' + path);
      r.onsuccess = () => res(r.result ? new Uint8Array(r.result) : null);
      r.onerror = () => res(null);
    });
  } catch { return null; }
}
async function cachePut(path, bytes) {
  try {
    const db = await openDB();
    await new Promise((res) => {
      const r = db.transaction(STORE, 'readwrite').objectStore(STORE).put(bytes, CACHE_VER + ':' + path);
      r.onsuccess = () => res(true); r.onerror = () => res(false);
    });
  } catch {}
}
async function cacheClear() {
  try {
    const db = await openDB();
    await new Promise((res) => {
      const r = db.transaction(STORE, 'readwrite').objectStore(STORE).clear();
      r.onsuccess = () => res(true); r.onerror = () => res(false);
    });
  } catch {}
}

async function cacheDel(path) {
  try {
    const db = await openDB();
    await new Promise((res) => {
      const r = db.transaction(STORE, 'readwrite').objectStore(STORE).delete(CACHE_VER + ':' + path);
      r.onsuccess = () => res(true); r.onerror = () => res(false);
    });
  } catch {}
}
async function cacheGetText(key) { const b = await cacheGet(key); return b ? new TextDecoder().decode(b) : null; }
async function cachePutText(key, s) { await cachePut(key, new TextEncoder().encode(s)); }
// djb2 over bytes -> short base36 string (provenance fingerprint; not security, just change-detection).
function hashBytes(b) { let h = 5381; for (let i = 0; i < b.length; i++) h = ((h << 5) + h + b[i]) >>> 0; return h.toString(36); }

// ---- streaming fetch with byte-progress (smooth bar for the ~8 MB index) --------------------------
async function fetchBytes(url, onChunk, signal) {
  let resp;
  try { resp = await fetch(url, { cache: 'no-store', signal }); } catch { return null; }
  if (!resp.ok) return null;
  if (resp.body && resp.body.getReader) {
    const reader = resp.body.getReader();
    const chunks = []; let got = 0;
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      chunks.push(value); got += value.length; onChunk && onChunk(got);
    }
    const out = new Uint8Array(got); let off = 0;
    for (const c of chunks) { out.set(c, off); off += c.length; }
    return out;
  }
  const ab = await resp.arrayBuffer();
  const u = new Uint8Array(ab); onChunk && onChunk(u.length); return u;
}

// ---- OS-wide download gate -----------------------------------------------------------------------
// Never let the brain pull race another NucleoOS download — the device is a single-task server (two
// concurrent multi-MB reads trip the webfs 503 breaker). Lazy + optional: if the shell module isn't
// reachable (e.g. host parity tests), the download runs ungated rather than failing to import.
let _gate;
async function downloadGate() {
  if (_gate) return _gate;
  try { _gate = (await import('/dlgate.js')).withDownloadLock; }
  catch { _gate = (label, fn) => fn(); }
  return _gate;
}
const PACK_LABEL = 'ANIMA · cervello locale';

// ---- shape the raw struct dump into the /api/anima web contract (mirrors nucleo_httpd.c) ----------
// BUG (tracked follow-up): TIER has no key 3 -> a STITCH/L2 reply maps to tier:'none' even when the
// reply is correct and non-empty. Consumers MUST NOT gate "did we get an answer?" on tier !== 'none'
// (the cascade uses local/cascade.js `answered()`, which gates on reply+action+domain instead). Adding
// 3:'stitch' here is a one-liner but needs the UI's tier-string switches (e.g. index.html subTier) and
// nucleo_httpd.c shaping checked first, so it is intentionally left for a separate, isolated change.
const TIER = { 1: 'command', 2: 'fact', 4: 'remote' };          // else 'none' (NONE/STITCH — see BUG above)
const ACT  = { 1: 'launch', 2: 'system', 3: 'answer', 4: 'tool' }; // else 'none'
export function shape(raw, q, lang) {
  const intent = raw.intent || '';
  const isTool = raw.action === 4;
  let domain;
  if (intent === 'clarify') domain = 'clarify';
  else if (intent === 'weather') domain = 'meteo';
  else if (intent === 'fx') domain = 'cambio';
  else if (intent === 'grok') domain = 'online';
  else if (raw.tier === 4) domain = 'online';
  else if (raw.tier === 2) domain = 'knowledge';
  else if (intent === 'capabilities') domain = 'faq';
  else if (intent === 'agenda') domain = 'agenda';
  else if (['calc', 'percent', 'convert', 'ohm'].includes(intent)) domain = 'calc';
  else if (raw.action === 4) domain = 'tool';
  else if (raw.action === 2) domain = 'system';
  else if (raw.action === 1) domain = 'app';
  else if (raw.action === 3) domain = 'faq';
  else domain = 'none';
  const out = {
    query: q, lang,
    tier: TIER[raw.tier] || 'none',
    action: ACT[raw.action] || 'none',
    arg: raw.arg || '',
    reply: raw.reply || '',
    tool: isTool ? intent : '',
    confidence: raw.confidence | 0,
    domain, intent,
    budget: raw.budget | 0,
    memory: !!raw.from_memory,
    state: raw.state || '',
    awaiting: !!raw.awaiting,
    corrected: raw.corrected || '',
    trace: raw.trace || '',
    content: raw.content || '',          // tool_content channel: the web writes this for create_file
    subject: raw.subject || '', relation: raw.relation || '',
    local: true,                         // marker: answered by the in-browser engine, not the device
  };
  if (isTool && out.arg && out.arg.includes('/')) out.path = out.arg;  // best-effort tool path
  return out;
}

// ---- public engine --------------------------------------------------------------------------------
export function createAnimaLocal(opts = {}) {
  const here = new URL('.', import.meta.url);
  const fsReadUrl = opts.fsReadUrl || ((p) => '/api/fs/read?path=' + encodeURIComponent(p));
  const fsListUrl = opts.fsListUrl || ((p) => '/api/fs/list?path=' + encodeURIComponent(p));
  let M = null, _q = null, _reset = null, loaded = false, loading = null, totalBytes = 0, _idbfs = false, _flushTimer = null;

  function mkdirp(M, dir) {
    let cur = '';
    for (const seg of dir.split('/').filter(Boolean)) { cur += '/' + seg; try { M.FS.mkdir(cur); } catch {} }
  }
  // Persist the IDBFS-backed writable subtree (user-taught facts + profile) to IndexedDB. Debounced
  // after each query; the cascade only writes there on a learn/profile turn, so this is usually a no-op.
  function flush() { if (_idbfs && M) { try { M.FS.syncfs(false, () => {}); } catch {} } }
  function scheduleFlush() { if (!_idbfs) return; clearTimeout(_flushTimer); _flushTimer = setTimeout(flush, 600); }

  async function _load(onProgress, opts = {}) {
    const { signal, cachedOnly } = opts;
    const aborted = () => !!(signal && signal.aborted);
    const ckAbort = () => { if (aborted()) throw Object.assign(new Error('cancelled'), { code: 'CANCELLED' }); };
    const note = (o) => { try { onProgress && onProgress(o); } catch {} };
    ckAbort();
    note({ phase: 'wasm' });
    const { default: AnimaLocal } = await import(new URL('./anima-local.mjs', here).href);
    M = await AnimaLocal();

    // Mount IDBFS on the small writable subtree (/sd/data/anima/rw) so user-taught facts (learn tier)
    // and the personal profile survive a page reload. The read-only pack stays in MEMFS. syncfs(true)
    // restores anything persisted in a previous session (no-op on first run).
    // Headless (Node host tests) has no IndexedDB -> skip the mount cleanly instead of throwing into the
    // catch on every load. In the browser this is a no-op and the mount proceeds as before.
    if (typeof indexedDB === 'undefined') { _idbfs = false; }
    else try {
      mkdirp(M, '/sd/data/anima/rw');
      M.FS.mount(M.FS.filesystems.IDBFS, {}, '/sd/data/anima/rw');
      await new Promise((res) => M.FS.syncfs(true, () => res()));
      _idbfs = true;
    } catch { _idbfs = false; }

    // CACHED-ONLY mount (silent path): touch NO network. Restore exactly the set the last full download
    // saved in `__packlist__` (so even dynamically-enumerated AKB5 shards come back from IndexedDB) and
    // skip the auto-sync entirely. This is what GUARANTEES a cached mount can never start a download.
    let items;
    if (cachedOnly) {
      let list = null;
      try { const t = await cacheGetText('__packlist__'); if (t) list = JSON.parse(t); } catch {}
      items = (Array.isArray(list) && list.length)
        ? list.map((p) => ({ p, req: PACK.some((x) => x.p === p && x.req) }))
        : PACK.slice();
    } else {
      // ---- AUTO-SYNC: keep the local brain in lockstep with the Cardputer. A cheap signature (every brain
      // file's size from /api/fs/list + a hash of the index provenance sidecar) is compared to the cached one;
      // if the device's brain changed (rebuilt index, edited cards, facts taught ON the device) the stale pack
      // is dropped so the loop below re-downloads it. Offline (device unreachable) -> sig null -> cache kept.
      // The browser's OWN taught facts live in IDBFS (/sd/.../rw, a SEPARATE DB) and are never touched here.
      // Only runs on a CONSENTED full download — NEVER on the silent cached mount above.
      note({ phase: 'sync' });
      let sig = null;
      try {
        const parts = [];
        for (const dir of ['/data/anima', '/data/anima/learned', '/data/anima/akb5']) {
          ckAbort();
          const r = await fetch(fsListUrl(dir), { cache: 'no-store', signal });
          if (r.ok) { const j = await r.json(); for (const e of (j.entries || [])) if (e.type !== 'dir') parts.push(dir + '/' + e.name + ':' + e.size); }
        }
        if (parts.length) {
          const prov = await fetchBytes(fsReadUrl('/data/anima/anima-it-index.bin.prov'), null, signal);
          sig = parts.sort().join('|') + (prov ? '#' + hashBytes(prov) : '');
        }
      } catch (e) { if (e && e.code === 'CANCELLED') throw e; }
      if (sig) {
        const stored = await cacheGetText('__brainsig__');
        if (stored && stored !== sig) { note({ phase: 'update' }); await cacheClear(); }   // device/extended brain changed -> full refresh
        await cachePutText('__brainsig__', sig);
      }

      // Build the download list: the static PACK + every AKB5 shard (enumerated dynamically from the unified
      // device path /data/anima/akb5). Each item downloads from `from` (or `p`) and mounts at `p`.
      items = PACK.slice();
      try {
        ckAbort();
        const r = await fetch(fsListUrl('/data/anima/akb5'), { cache: 'no-store', signal });
        if (r.ok) for (const e of ((await r.json()).entries || []))
          if (e.type !== 'dir' && e.name.endsWith('.bin'))
            items.push({ p: 'data/anima/akb5/' + e.name, req: false });
      } catch (e) { if (e && e.code === 'CANCELLED') throw e; }
    }

    const files = [];
    totalBytes = 0;
    for (let i = 0; i < items.length; i++) {
      ckAbort();                                      // honour a cancel between files
      const f = items[i];
      const name = f.p.split('/').pop();
      note({ phase: 'fetch', name, idx: i, count: items.length, bytes: totalBytes });
      let bytes = await cacheGet(f.p);
      const fromCache = !!bytes;
      if (!bytes && !cachedOnly) {
        bytes = await fetchBytes(fsReadUrl('/' + (f.from || f.p)), (n) =>
          note({ phase: 'fetch', name, idx: i, count: items.length, bytes: totalBytes + n, downloading: true }), signal);
        if (!bytes) {
          ckAbort();                                  // a null from an aborted fetch is a cancel, not a missing file
          if (f.req) throw Object.assign(new Error('ANIMA Local: required pack file missing: ' + (f.from || f.p)), { code: 'PACK_REQUIRED_MISSING' });
          continue;                                   // optional file absent on this device -> skip
        }
        await cachePut(f.p, bytes);
      }
      if (!bytes) {                                   // cachedOnly: not in the cache -> skip (a missing req file is fatal)
        if (f.req) throw Object.assign(new Error('ANIMA Local: required pack file missing: ' + (f.from || f.p)), { code: 'PACK_REQUIRED_MISSING' });
        continue;
      }
      totalBytes += bytes.length;
      files.push({ p: f.p, bytes, fromCache });
      note({ phase: 'fetch', name, idx: i + 1, count: items.length, bytes: totalBytes });
    }

    // Remember EXACTLY what we mounted so a later cached-only mount restores the same set with no network.
    if (!cachedOnly) { try { await cachePutText('__packlist__', JSON.stringify(files.map((f) => f.p))); } catch {} }

    // mount the pack into the WASM in-memory filesystem at /sd (the firmware's NUCLEO_SD_MOUNT)
    for (const { p, bytes } of files) {
      const vp = '/sd/' + p;
      mkdirp(M, vp.slice(0, vp.lastIndexOf('/')));
      M.FS.writeFile(vp, bytes);
    }

    note({ phase: 'init' });
    const init = M.cwrap('anima_init', 'number', ['string']);
    _q = M.cwrap('anima_query_json', 'string', ['string', 'string']);
    _reset = M.cwrap('anima_reset', null, []);
    init('it'); init('en');                           // load both command packs once
    loaded = true;
    note({ phase: 'ready', bytes: totalBytes });
  }

  return {
    // Idempotent; concurrent callers share the one in-flight load. onProgress gets {phase,name,bytes,...}.
    // The whole load (WASM module + pack files) holds the OS-wide download lock so it can never run
    // alongside another NucleoOS download (voice models, Forge weights, a second tab).
    // Full (consented) download. Pass an AbortSignal to make it CANCELLABLE: aborting rejects the
    // returned promise with {code:'CANCELLED'} and clears the memo so a later attempt can retry cleanly.
    load(onProgress, signal) {
      if (loaded) return Promise.resolve();
      if (!loading) {
        loading = downloadGate().then((gate) => gate(PACK_LABEL, () => _load(onProgress, { signal })));
        loading.catch(() => { loading = null; });     // a failed/cancelled load must not poison the next attempt
      }
      return loading;
    },
    isLoaded() { return loaded; },
    bytes() { return totalBytes; },
    // True if the engine can answer WITHOUT a network pull — already loaded, or the required pack is
    // cached in IndexedDB. Lets the cascade decide "browser brain or device?" without triggering a
    // download (the mid-chat silent-fallback probe). Never pulls; safe to call often.
    async ready() { return loaded || await packCached(); },
    // Load ONLY from the cache (cachedOnly: NO network at all), and only if the required pack is present;
    // otherwise resolve false so the caller falls back to the device instead of pulling ~14 MB. This is
    // the silent boot/mid-chat mount — it can never start a download.
    async loadIfCached(onProgress, signal) {
      if (loaded) return true;
      if (!(await packCached())) return false;
      if (!loading) {
        loading = downloadGate().then((gate) => gate(PACK_LABEL, () => _load(onProgress, { signal, cachedOnly: true })));
        loading.catch(() => { loading = null; });
      }
      try { await loading; } catch {}
      return loaded;
    },
    // Run the offline cascade. Returns the /api/anima-shaped result object.
    query(text, lang) {
      if (!loaded) throw Object.assign(new Error('ANIMA Local: engine not loaded'), { code: 'ENGINE_NOT_LOADED' });
      const out = shape(JSON.parse(_q(text || '', lang === 'en' ? 'en' : 'it')), text || '', lang === 'en' ? 'en' : 'it');
      scheduleFlush();   // persist any learn/profile write this turn (debounced; usually a no-op)
      return out;
    },
    // Cheap post-load sanity probe: run one deterministic offline query and report whether the cascade
    // is alive. No I/O, no learn write. The shell can gate "brain ready" on this; the host tests assert it.
    selfTest() {
      if (!loaded) return { ok: false, reason: 'not-loaded' };
      try {
        const r = shape(JSON.parse(_q('apri la calcolatrice', 'it')), 'apri la calcolatrice', 'it');
        return { ok: !!(r && (r.action === 'launch' || r.reply)), tier: r.tier, action: r.action };
      } catch (e) { return { ok: false, reason: String(e && e.message || e) }; }
    },
    reset() { if (_reset) _reset(); },
    flush,               // force-persist now (e.g. on pagehide)
    async refresh() { await cacheClear(); loaded = false; loading = null; M = null; },  // force a fresh download next load
  };
}

// Background pre-copy: download + cache the knowledge pack WITHOUT instantiating the WASM (lighter).
// Called by the OS shell on first boot so that, by the time the user opens ANIMA, the brain is already
// in IndexedDB and the engine loads instantly (no blocking gate). Idempotent and cache-first; safe to
// call repeatedly. Returns the total bytes ensured. Throws only if a REQUIRED file can't be fetched.
async function _prefetchPack(onProgress, fsReadUrl) {
  const url = fsReadUrl || ((p) => '/api/fs/read?path=' + encodeURIComponent(p));
  let total = 0;
  for (let i = 0; i < PACK.length; i++) {
    const f = PACK[i];
    let bytes = await cacheGet(f.p);
    if (!bytes) {
      bytes = await fetchBytes(url('/' + f.p), null);
      if (!bytes) { if (f.req) throw new Error('ANIMA prefetch: required file missing: ' + f.p); else continue; }
      await cachePut(f.p, bytes);
    }
    total += bytes.length;
    try { onProgress && onProgress({ idx: i + 1, count: PACK.length, bytes: total, name: f.p.split('/').pop() }); } catch {}
  }
  return total;
}

// Public: same as _prefetchPack but funnelled through the OS-wide download gate (one transfer at a
// time across all of NucleoOS). Pass {ifAvailable:true} for background pre-copy that must yield to a
// user-initiated download instead of queueing ahead of it.
export async function prefetchPack(onProgress, fsReadUrl, opts = {}) {
  const gate = await downloadGate();
  return gate(PACK_LABEL, () => _prefetchPack(onProgress, fsReadUrl), opts);
}

// Cheap probe: is the full required pack already cached? (Lets the shell skip a network probe.)
export async function packCached() {
  for (const f of PACK) if (f.req && !(await cacheGet(f.p))) return false;
  return true;
}
