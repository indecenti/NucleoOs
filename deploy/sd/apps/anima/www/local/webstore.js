// ANIMA — learned WEB store. The browser-resident index of everything webIndexAnswer() has pulled
// from the web, so an entity fetched once is recalled INSTANTLY and offline thereafter. This is where
// the client-side hybrid brain "evolves": separate from the read-only WASM knowledge pack, separate
// from the device's /sd learned cards, owned entirely by this browser.
//
// Backed by IndexedDB (its own DB, never collides with the engine pack DB). In a headless host test
// (no indexedDB) it degrades to an in-memory Map so the indexer is still fully testable. Keyed by
// `${lang}:${slug}` so the same entity asked any way maps to ONE card (merge, no duplicates).

const DB_NAME = 'anima-webindex', STORE = 'cards', VER = 1;
const _mem = new Map();                 // host fallback (no IndexedDB)

function hasIDB() { return typeof indexedDB !== 'undefined'; }
function key(slug, lang) { return (lang === 'en' ? 'en' : 'it') + ':' + slug; }

function openDB() {
  return new Promise((res, rej) => {
    const o = indexedDB.open(DB_NAME, VER);
    o.onupgradeneeded = () => { if (!o.result.objectStoreNames.contains(STORE)) o.result.createObjectStore(STORE); };
    o.onsuccess = () => res(o.result);
    o.onerror = () => rej(o.error);
  });
}

// get(slug, lang) -> card | null. Follows one level of alias pointer ({ ref }) so a lookup by the
// canonical title or full phrasing resolves to the same card stored under the asked-entity key.
export async function get(slug, lang) {
  const rec = await _raw(key(slug, lang));
  if (rec && rec.ref) return await _raw(rec.ref);
  return rec;
}
async function _raw(k) {
  if (!hasIDB()) return _mem.get(k) || null;
  try {
    const db = await openDB();
    return await new Promise((res) => {
      const r = db.transaction(STORE).objectStore(STORE).get(k);
      r.onsuccess = () => res(r.result || null);
      r.onerror = () => res(null);
    });
  } catch { return null; }
}
async function _putRaw(k, rec) {
  if (!hasIDB()) { _mem.set(k, rec); return true; }
  try {
    const db = await openDB();
    return await new Promise((res) => {
      const r = db.transaction(STORE, 'readwrite').objectStore(STORE).put(rec, k);
      r.onsuccess = () => res(true); r.onerror = () => res(false);
    });
  } catch { return false; }
}

// put(card) — card carries { slug, lang, title, reply, desc, category, source, aliases, ts }.
// Merge-on-key: a re-fetch of the same entity overwrites (fresher extract), preserving accumulated
// aliases so cross-phrasing recall keeps working.
export async function put(card) {
  if (!card || !card.slug || !card.reply) return false;
  const k = key(card.slug, card.lang);
  const prior = await get(card.slug, card.lang);
  const aliases = [...new Set([...(prior?.aliases || []), ...(card.aliases || [])])].filter(Boolean).slice(0, 16);
  const aliasKeys = card.aliasKeys || [];
  const rec = { ...card, aliases }; delete rec.aliasKeys;
  const okMain = await _putRaw(k, rec);
  for (const a of aliasKeys) { const ak = key(a, card.lang); if (ak !== k) await _putRaw(ak, { ref: k }); }   // pointer
  return okMain;
}

// all(lang) -> card[] (for a "what have I learned" view / export; bounded scan).
export async function all(lang) {
  if (!hasIDB()) return [...(_mem.values())].filter(c => !c.ref && (c.lang === 'en') === (lang === 'en'));
  try {
    const db = await openDB();
    return await new Promise((res) => {
      const out = []; const pfx = (lang === 'en' ? 'en' : 'it') + ':';
      const cur = db.transaction(STORE).objectStore(STORE).openCursor();
      cur.onsuccess = (e) => { const c = e.target.result; if (!c) return res(out); if (String(c.key).startsWith(pfx) && c.value && !c.value.ref) out.push(c.value); c.continue(); };
      cur.onerror = () => res(out);
    });
  } catch { return []; }
}

export async function count() {
  if (!hasIDB()) return _mem.size;
  try {
    const db = await openDB();
    return await new Promise((res) => { const r = db.transaction(STORE).objectStore(STORE).count(); r.onsuccess = () => res(r.result | 0); r.onerror = () => res(0); });
  } catch { return 0; }
}

export async function clear() {
  _mem.clear();
  if (!hasIDB()) return true;
  try {
    const db = await openDB();
    return await new Promise((res) => { const r = db.transaction(STORE, 'readwrite').objectStore(STORE).clear(); r.onsuccess = () => res(true); r.onerror = () => res(false); });
  } catch { return false; }
}

// A ready-to-inject store object for webIndexAnswer({ store }).
export const webStore = { get, put, all, count, clear };
