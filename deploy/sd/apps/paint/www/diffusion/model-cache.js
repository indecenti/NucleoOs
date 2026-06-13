// model-cache.js — IndexedDB cache for SDXS model chunks.
// Uses IndexedDB (not Cache API) so it works on plain HTTP origins like http://192.168.x.x
// (Cache API requires a secure context — HTTPS or localhost — and would silently do nothing here).
//
// makeModelCache({ revision }) → { wrap, get, put, hasAny, clear }
//   revision: string from manifest.revision — part of the DB name so a new model provision
//             auto-migrates to a fresh DB with no manual cleanup needed.
//
// wrap(readPart, partShas) → cachedReadPart(path) → {ok, status, bytes:Uint8Array}
//   Checks IndexedDB first (key = SHA-256 of the chunk). On miss: calls readPart, writes result
//   to DB in the background, then returns the bytes. A DB that can't open degrades to no-cache.
//
// hasAny(partShas) → Promise<bool>
//   Returns true if at least one chunk from partShas is cached (used to detect first-download).
//
// Pure & DOM-free → host-testable.

const DB_NAME  = 'sdxs-cache';
const STORE    = 'chunks';

function openDb(revision) {
  return new Promise((res, rej) => {
    if (typeof indexedDB === 'undefined') { res(null); return; }
    const req = indexedDB.open(`${DB_NAME}-${revision}`, 1);
    req.onupgradeneeded = e => e.target.result.createObjectStore(STORE);
    req.onsuccess       = e => res(e.target.result);
    req.onerror         = ()  => rej(req.error);
  });
}

export function makeModelCache({ revision = 'v1' } = {}) {
  let _dbP = null;
  function db() {
    if (!_dbP) _dbP = openDb(revision).catch(() => null);
    return _dbP;
  }

  async function get(sha) {
    const d = await db(); if (!d) return null;
    return new Promise(res => {
      try {
        const tx  = d.transaction(STORE, 'readonly');
        const req = tx.objectStore(STORE).get(sha);
        req.onsuccess = () => res(req.result ? new Uint8Array(req.result) : null);
        req.onerror   = () => res(null);
      } catch { res(null); }
    });
  }

  function put(sha, bytes) {
    // fire-and-forget — never block inference for a write
    db().then(d => {
      if (!d) return;
      try {
        const buf = bytes instanceof ArrayBuffer ? bytes : bytes.buffer;
        const tx  = d.transaction(STORE, 'readwrite');
        tx.objectStore(STORE).put(buf, sha);
      } catch {}
    }).catch(() => {});
  }

  // wrap(readPart, partShas) — drop-in replacement for readPart that checks the DB first.
  function wrap(readPart, partShas) {
    return async function cachedRead(path) {
      // derive chunk index from the .NNN suffix (e.g. fused.onnx.007 → 7)
      const m   = path.match(/\.(\d{3})$/);
      const idx = m ? parseInt(m[1], 10) : -1;
      const sha = (partShas && idx >= 0) ? partShas[idx] : null;
      if (!sha) return readPart(path);                    // no sha → bypass cache

      const cached = await get(sha);
      if (cached) return { ok: true, status: 200, bytes: cached };  // local hit, no WiFi

      const res = await readPart(path);
      if (res && res.ok && res.bytes) put(sha, res.bytes);          // async write after good read
      return res;
    };
  }

  // hasAny: true if at least one chunk from partShas is already in the DB (first-download check).
  async function hasAny(partShas) {
    if (!partShas || !partShas.length) return false;
    const v = await get(partShas[0]);
    return !!v;
  }

  // clear: wipe the entire DB bucket (e.g. after a model is removed or re-provisioned).
  async function clear() {
    const d = await db(); if (!d) return;
    try {
      const tx = d.transaction(STORE, 'readwrite');
      tx.objectStore(STORE).clear();
    } catch {}
  }

  return { wrap, get, put, hasAny, clear };
}
