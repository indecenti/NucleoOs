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

const TYPES = { avif: 'image/avif', webp: 'image/webp', ogg: 'audio/ogg' };
const wait = (ms) => new Promise((res) => setTimeout(res, ms));

// The device answers 503 + Retry-After when its heap is short for a moment: back off and try again.
async function fetchBytes(path) {
  for (let i = 0; ; i++) {
    let r = null;
    try { r = await fetch(new URL(path, BASE)); } catch { /* network blip */ }
    if (r && r.ok) return r.blob();
    if (i >= 5 || (r && r.status !== 503 && r.status !== 429)) throw new Error(`${path}: ${r ? r.status : 'network'}`);
    const after = r && +r.headers.get('Retry-After');
    await wait((after > 0 ? after * 1000 : 800) * (1 + i * 0.5));
  }
}

// A manifest entry -> its Blob. Files over the server's large-file gate ship as byte slices ("parts")
// that are joined back into the original bytes; the store keeps the joined file under its logical name.
function blobOf(e) {
  const file = e.f;
  if (inflight.has(file)) return inflight.get(file);
  const p = (async () => {
    const hit = await tx('readonly', (s) => s.get(file));
    if (hit) return hit;
    let b;
    if (e.parts) {
      const parts = [];
      for (const part of e.parts) parts.push(await fetchBytes(part));   // one at a time: gentle on the device
      b = new Blob(parts, { type: TYPES[file.split('.').pop()] || '' });
    } else b = await fetchBytes(file);
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
  const e = entry('images', id);
  if (!urls.has(e.f)) urls.set(e.f, URL.createObjectURL(await blobOf(e)));
  return urls.get(e.f);
}

/** Decoded image for a Three.js texture or a canvas. */
export async function imageBitmap(id) {
  await loadManifest();
  return createImageBitmap(await blobOf(entry('images', id)));
}

/** Object URL of a stored track, for an <audio> element (streams; never decoded whole). */
export async function musicUrl(id) {
  await loadManifest();
  const e = entry('music', id);
  if (!urls.has(e.f)) urls.set(e.f, URL.createObjectURL(await blobOf(e)));
  return urls.get(e.f);
}

/** Encoded track bytes for AudioContext.decodeAudioData. */
export async function musicBuffer(id) {
  await loadManifest();
  return (await blobOf(entry('music', id))).arrayBuffer();
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
      try { await blobOf(e); } catch { /* retried on next use */ }
      done += e.b;
      if (onProgress) onProgress(done, total);
    }
  };
  await Promise.all(Array.from({ length: PARALLEL }, worker));
}
