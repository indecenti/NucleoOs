// Costellazioni asset store: every image and track is downloaded once and kept in IndexedDB.
// The device serves the web OS over plain http://, where service workers and the Cache API are not
// available; IndexedDB is. File names carry a content hash (tools/costellazioni-assets/build-assets.py),
// so a stored file never goes stale: an update ships new names, only those are fetched, and the entries
// the manifest no longer lists are pruned. Without IndexedDB (private mode) it degrades to plain fetch,
// which still hits the device's one-week HTTP cache for images and audio.
const BASE = new URL('./assets/', import.meta.url);
const DB = 'stelle-assets', STORE = 'files';
const PARALLEL = 2;                 // the Cardputer's httpd is small: two streams at a time

let manifest = null, dbp = null;
const urls = new Map();             // file -> object URL (one per file per session)
const inflight = new Map();         // file -> Promise<Blob>

function openDb() {
  if (!dbp) dbp = new Promise((res) => {
    try {
      const rq = indexedDB.open(DB, 1);
      rq.onupgradeneeded = () => rq.result.createObjectStore(STORE);
      rq.onsuccess = () => res(rq.result);
      rq.onerror = rq.onblocked = () => res(null);
    } catch { res(null); }
  });
  return dbp;
}

function tx(mode, fn) {
  return openDb().then((db) => db && new Promise((res) => {
    try {
      const t = db.transaction(STORE, mode), s = t.objectStore(STORE);
      const rq = fn(s);
      t.oncomplete = () => res(rq ? rq.result : true);
      t.onerror = t.onabort = () => res(null);
    } catch { res(null); }
  }));
}

export async function loadManifest() {
  if (!manifest) {
    const r = await fetch(new URL('manifest.json', BASE), { cache: 'no-cache' });
    manifest = r.ok ? await r.json() : { images: {}, music: {} };
    prune();
  }
  return manifest;
}

async function prune() {
  const live = new Set([...Object.values(manifest.images), ...Object.values(manifest.music)].map((e) => e.f));
  const keys = await tx('readonly', (s) => s.getAllKeys());
  const dead = (keys || []).filter((k) => !live.has(k));
  if (dead.length) tx('readwrite', (s) => { for (const k of dead) s.delete(k); });
}

function blobOf(file) {
  if (inflight.has(file)) return inflight.get(file);
  const p = (async () => {
    const hit = await tx('readonly', (s) => s.get(file));
    if (hit) return hit;
    const r = await fetch(new URL(file, BASE));
    if (!r.ok) throw new Error(`${file}: ${r.status}`);
    const b = await r.blob();
    tx('readwrite', (s) => s.put(b, file));
    return b;
  })();
  inflight.set(file, p);
  p.catch(() => inflight.delete(file));
  return p;
}

function entry(group, id) {
  const e = manifest && manifest[group][id];
  if (!e) throw new Error(`no ${group} asset "${id}"`);
  return e;
}

/** Object URL of an image (for <img>, CSS backgrounds), ready to use. */
export async function imageUrl(id) {
  await loadManifest();
  const { f } = entry('images', id);
  if (!urls.has(f)) urls.set(f, URL.createObjectURL(await blobOf(f)));
  return urls.get(f);
}

/** Decoded image for a Three.js texture or a canvas. */
export async function imageBitmap(id) {
  await loadManifest();
  return createImageBitmap(await blobOf(entry('images', id).f));
}

/** Encoded track bytes for AudioContext.decodeAudioData. */
export async function musicBuffer(id) {
  await loadManifest();
  return (await blobOf(entry('music', id).f)).arrayBuffer();
}

/** CSS gradient painted while an image streams in. */
export function placeholder(id) {
  const e = manifest && manifest.images[id];
  return e ? `linear-gradient(${e.ph[0]}, ${e.ph[1]})` : '#05070d';
}

export function has(group, id) { return !!(manifest && manifest[group][id]); }

/** Download everything not stored yet, by priority, in the background. onProgress(doneBytes, totalBytes). */
export async function prefetch(onProgress) {
  await loadManifest();
  const all = [...Object.values(manifest.images), ...Object.values(manifest.music)].sort((a, b) => a.p - b.p);
  const total = all.reduce((n, e) => n + e.b, 0);
  let done = 0, i = 0;
  const worker = async () => {
    while (i < all.length) {
      const e = all[i++];
      try { await blobOf(e.f); } catch { /* retried on next use */ }
      done += e.b;
      if (onProgress) onProgress(done, total);
    }
  };
  await Promise.all(Array.from({ length: PARALLEL }, worker));
}
