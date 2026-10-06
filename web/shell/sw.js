// Offline shell cache. Keeps the desktop usable when the device is unreachable;
// API calls always go to the network (never cached) so live data stays fresh.
// Bump this on every shell change that must reach already-installed clients. The reason for each
// roll goes in docs/shell-cache-log.md — NOT here: it used to be one 10.5 KB comment on this line,
// half the whole service worker, re-shipped to every browser on every update check.
const CACHE = 'nucleo-shell-v149';   // v149 — review fixes (see docs/shell-cache-log.md)
// Per-version cache for app assets (/apps/<id>/...). Tied to the shell version so a deploy (which
// bumps CACHE) drops it; the shell also flushes it on apps.changed (OTA app update) via postMessage.
const APP_CACHE = CACHE + '-apps';
const ASSETS = ['./', 'index.html', 'style.css', 'copilot.css', 'notify.css', 'onboarding.css', 'shell.js', 'boot-fetch.js', 'copilot.js', 'anima-mode.js', 'notify.js', 'onboarding.js', 'ambient.js', 'ai.js', 'ai-keys.js', 'shortcuts.js', 'search-rank.js', 'appbroker.js', 'wm.js', 'fsindex.js', 'busy.js', 'dlgate.js', 'micgate.js', 'system-ui.js', 'nucleo-i18n.js', 'update-check.js', 'update-core.js', 'sha256.js', 'ai-engines.js', 'capabilities.js', 'local-ai-help.js', 'seq-import.js', 'i18n/core.it.json', 'i18n/core.en.json', 'i18n/core.es.json', 'i18n/core.fr.json', 'i18n/core.de.json', 'i18n/shell.it.json', 'i18n/shell.en.json', 'i18n/shell.es.json', 'i18n/shell.fr.json', 'i18n/shell.de.json', 'manifest.webmanifest', 'icon.png', 'icons.json'];   // NB: wallpaper.png removed — it's a 535KB JPEG-misnamed-.png never displayed (live wallpaper = /data/Pictures/wallpaper.png) that only tripped the webfs low-heap defer

// --- Device request gate (shared reads, exclusive writes) ----------------------
// The firmware httpd has max_open_sockets=4 + lru_purge_enable (it deliberately RESETS
// the oldest connection when a 5th arrives) on ~18KB of heap, no PSRAM. Live the heap is
// ~80% fragmented — the largest contiguous block is only ~7.5KB and has historically
// grazed 16 bytes free. A burst of parallel requests starves the heap; a write that lands
// mid-burst can't even malloc its 2KB floor and returns "500 oom".
//
// Counting semaphore with permits = MAX_INFLIGHT. A normal request (asset / fs read) takes
// ONE permit (so up to MAX_INFLIGHT run together). A WRITE takes ALL permits: it can only
// start once everything else has drained, and while it holds them no other gated request
// proceeds — the device serves the write ALONE, with the whole heap free, then the queue
// resumes. FIFO drain (we only ever grant the head of the queue) so a write can't be
// starved by a steady trickle of reads. Streaming endpoints (chat/logs/llm) are NOT gated.
const MAX_INFLIGHT = 2;   // 3->2: serialise harder so the PSRAM-less single-task device is never flooded at boot (v93)
let active = 0;            // permits currently held
const queue = [];         // FIFO of { need, resolve }
// In-flight /api/anima GETs, keyed by path+query: identical questions asked at the same moment share
// ONE request instead of racing each other at the device. Entries are removed when the job settles.
const animaInflight = new Map();
function pump() {
  // Strictly head-of-line: never grant a later waiter past a blocked one (prevents the
  // exclusive write from being starved by reads that keep slipping into freed slots).
  while (queue.length && active + queue[0].need <= MAX_INFLIGHT) {
    const w = queue.shift();
    active += w.need;
    w.resolve();
  }
}
function acquire(need) { return new Promise((resolve) => { queue.push({ need, resolve }); pump(); }); }
function release(need) { active -= need; pump(); }
async function netFetch(req) {
  try { return await fetch(req); }
  catch (err) {
    // A transient lru_purge reset / momentary OOM. Replaying a body is unsafe, so
    // only retry idempotent GETs (no body) after a short breath.
    if (req.method !== 'GET') throw err;
    // Three tries with backoff: one lost module of a dynamic import() fails the WHOLE graph, and the browser
    // remembers it until a reload (the ANIMA agent runtime stayed dead after one reset in a busy moment).
    for (const ms of [250, 800]) {
      await new Promise((r) => setTimeout(r, ms));
      try { return await fetch(req); } catch (e) { err = e; }
    }
    throw err;
  }
}
// A write holds the WHOLE pool — but only for so long. It used to be ABORTED after 15 s, which killed
// every multi-MB upload (desktop drop, File Commander) mid-transfer: 504, then retried three times. The
// SW cannot see a body's size (Content-Length is not exposed on a FetchEvent request), so it never aborts
// a write; instead, past EXCLUSIVE_MAX_MS it hands back all but one permit and the upload carries on as
// an ordinary shared request. A hung write therefore can no longer freeze the desktop either. Callers
// that want a deadline (the shell's small config saves) set their own — that abort reaches us through
// req.signal.
const EXCLUSIVE_MAX_MS = 15000;
async function gatedFetch(req, exclusive) {
  const need = exclusive ? MAX_INFLIGHT : 1;
  await acquire(need);
  let held = need;
  const downgrade = exclusive ? setTimeout(() => { if (held > 1) { const give = held - 1; held = 1; release(give); } }, EXCLUSIVE_MAX_MS) : null;
  try {
    return await netFetch(req);
  } finally { clearTimeout(downgrade); release(held); }
}

// --- ANIMA Forge: serve installed model weights from the verified install cache --------------------
// The Forge installer downloads each SHA-verified shard/aux into caches['anima-forge-models'] at
// '/fc/<id>/<file>'. When an offline model loads, WebLLM/wllama request those files from the device SD
// path or the HF CDN. Serving them from that cache means a loaded model needs NEITHER the network NOR a
// heavy whole-file read off the single-task device — closing the "install → runs offline" loop and
// avoiding the very read-storm the bounded-range installer exists to prevent.
// KEEP forgeModelKey byte-identical to apps/anima/www/forge/model-url-map.js (pinned by
// tools/anima-host/forge-model-url-map.test.mjs). Returns null for non-model URLs → SW leaves them alone.
const MODEL_CACHE = 'anima-forge-models';
// Durable wallpaper/image cache. Unlike the version-scoped shell CACHE (wiped on every deploy), this
// is preserved across shell version bumps (see the activate `keep` set) — so a wallpaper the user
// picked loads ONCE and stays available offline / in WASM forever, exactly like the content-addressed
// model cache. Served stale-while-revalidate so a changed file still refreshes in the background.
const WALLPAPER_CACHE = 'nucleo-wallpaper';
const isImagePath = (p) => !!p && /\.(png|jpe?g|gif|svg|webp)$/i.test(p);
// The durable store must stay SMALL: every image read lands here (gallery browsing included), it
// survives every deploy, and an unbounded cache eventually trips origin-quota eviction — which can
// take the SHA-verified Forge model cache down with it. Keep the last N images, drop the oldest.
const WALLPAPER_CACHE_MAX = 24;
async function trimWallpaperCache(cache) {
  try {
    const keys = await cache.keys();
    for (let i = 0; i < keys.length - WALLPAPER_CACHE_MAX; i++) await cache.delete(keys[i]);
  } catch {}
}
// A file changed under a cached image: drop every cached read of that path (or of anything below it, for
// a folder move/delete). Without this a Paint save reopened with the OLD pixels and a deleted image kept
// being served, from a cache that even survives deploys.
async function dropCachedImages(paths) {
  const ps = paths.filter(Boolean).map((p) => p.replace(/\/+$/, ''));
  if (!ps.length) return;
  try {
    const cache = await caches.open(WALLPAPER_CACHE);
    for (const k of await cache.keys()) {
      let kp = null; try { kp = new URL(k.url).searchParams.get('path'); } catch {}
      if (kp && ps.some((p) => kp === p || kp.startsWith(p + '/'))) await cache.delete(k);
    }
  } catch {}
}
const changedPaths = (url) => [url.searchParams.get('path'), url.searchParams.get('from'), url.searchParams.get('to')];
function forgeModelKey(url) {
  const u = String(url).split('?')[0].split('#')[0];
  let m = /\/forge\/models\/([^/]+)\/(.+)$/.exec(u);
  if (m) return '/fc/' + m[1] + '/' + m[2];
  m = /huggingface\.co\/[^/]+\/([^/]+)\/resolve\/[^/]+\/(.+)$/.exec(u);
  if (m) return '/fc/' + m[1] + '/' + m[2];
  return null;
}

self.addEventListener('install', (e) => {
  // Resilient precache: add each asset INDEPENDENTLY (not addAll, which is atomic — a single 404
  // would abort the whole install, leaving the shell with NO offline cache and spamming the console,
  // exactly the failure we hit when copilot.*/shortcuts.js were missing). Everything present is still
  // cached; a stray miss is tolerated, so a future renamed asset can't take the desktop offline.
  e.waitUntil((async () => {
    const c = await caches.open(CACHE);
    // Windowed (3-at-a-time): the SW's own install fetches bypass the fetch-handler gate above,
    // so a flat ~24-parallel burst would hit the no-PSRAM httpd with exactly the storm
    // MAX_INFLIGHT exists to prevent. Misses are still tolerated per-asset.
    for (let i = 0; i < ASSETS.length; i += MAX_INFLIGHT) {
      await Promise.allSettled(ASSETS.slice(i, i + MAX_INFLIGHT).map((a) => c.add(a)));
    }
    await self.skipWaiting();
  })());
});

self.addEventListener('activate', (e) => {
  // Drop only STALE versions. Preserve the live shell cache, the live app cache, AND the Forge model
  // cache — the latter is content-addressed (/fc/<id>/<file>), NOT version-scoped, so wiping it on a
  // shell bump used to throw away GBs of SHA-verified model weights and force a full re-download.
  const keep = new Set([CACHE, APP_CACHE, MODEL_CACHE, WALLPAPER_CACHE]);
  e.waitUntil(caches.keys().then((ks) =>
    Promise.all(ks.filter((k) => !keep.has(k)).map((k) => caches.delete(k)))).then(() => self.clients.claim()));
});

// Let the shell force-refresh the app cache without a full SW version bump — wired to the
// apps.changed bus event (an app was installed/updated/removed over the air).
self.addEventListener('message', (e) => {
  if (e.data && e.data.type === 'flush-app-cache') e.waitUntil(caches.delete(APP_CACHE));
});

self.addEventListener('fetch', (e) => {
  const url = new URL(e.request.url);

  // Installed Forge model weights → serve from the verified install cache (offline, no device read-storm).
  const mkey = forgeModelKey(e.request.url);
  if (mkey) {
    e.respondWith((async () => {
      try { const hit = await (await caches.open(MODEL_CACHE)).match(mkey); if (hit) return hit; } catch {}
      // Not installed yet → preserve normal behaviour: gate same-origin (device) reads, fetch the CDN direct.
      if (url.origin === self.location.origin) return gatedFetch(e.request).catch(() => new Response('', { status: 504, statusText: 'model not installed' }));
      try { return await fetch(e.request); } catch { return new Response('', { status: 504, statusText: 'model unavailable offline' }); }
    })());
    return;
  }

  // Everything else that is not the device (LLM providers, GitHub, CDNs) goes straight to the network.
  // Routed through gatedFetch it held one of the DEVICE's permits for a request the device never sees,
  // and a CORS/network failure came back as a fake 504 "device unreachable".
  if (url.origin !== self.location.origin) return;

  const p = url.pathname;
  if (p.startsWith('/api/')) {
    // Images read via the file API (wallpapers, gallery thumbnails) → DURABLE cache with
    // stale-while-revalidate. A cached copy is returned instantly (offline / WASM-safe) AND the
    // network is queried in the background to refresh it if the file changed. The store is the
    // deploy-surviving WALLPAPER_CACHE, so the wallpaper truly loads ONCE — a shell version bump no
    // longer discards it and forces a re-download off the single-task device.
    if (p === '/api/fs/read' && isImagePath(url.searchParams.get('path'))) {
      e.respondWith((async () => {
        const cache = await caches.open(WALLPAPER_CACHE);
        const hit = await cache.match(e.request);
        const network = gatedFetch(e.request).then(async (res) => {
          // A cache failure (quota, a 206 partial) must never cost the caller a good network answer.
          if (res && res.ok && res.status === 200) { try { await cache.put(e.request, res.clone()); await trimWallpaperCache(cache); } catch {} }
          return res;
        }).catch(() => null);
        if (hit) { e.waitUntil(network); return hit; }          // instant; refresh silently
        const res = await network;
        return res || new Response('', { status: 504, statusText: 'device busy' });
      })());
      return;
    }
    // File reads/writes: short request/response, never cached (live data). The WRITE
    // runs EXCLUSIVE (takes all permits): the device serves it alone, with the whole heap
    // free, so it finds a large contiguous block instead of running OOM mid-burst.
    if (p === '/api/fs/read' || p === '/api/fs/list' || p === '/api/fs/write') {
      const exclusive = (p === '/api/fs/write');   // list = shared read (need=1), like read
      e.respondWith((async () => {
        if (exclusive) await dropCachedImages(changedPaths(url));
        const res = await gatedFetch(e.request, exclusive).catch(() => new Response('', { status: 504, statusText: 'device busy' }));
        if (exclusive && res.ok) await dropCachedImages(changedPaths(url));   // a read racing the write may have re-cached the old bytes
        return res;
      })());
      return;
    }
    // Move / delete: not gated (as before), but they change what a cached image path holds.
    if ((p === '/api/fs/move' || p === '/api/fs/delete') && e.request.method !== 'GET') {
      e.respondWith((async () => {
        const res = await fetch(e.request);
        if (res.ok) await dropCachedImages(changedPaths(url));
        return res;
      })());
      return;
    }
    // /api/anima: the question to the assistant. Twelve surfaces call it — copilot, shell
    // search, onboarding, ai.js, and the anima/agent/settings/spreadsheet/games/my-facts/recorder/
    // code-runner apps — and none of them knows about the others: the "never concurrent calls" rule was left
    // to the good manners of twelve files. On a chip without PSRAM, with 4-6 sockets shared by all
    // the iframes, two questions at once are enough to time out the OS file reads.
    //
    // SHARED slot (need=1), not exclusive: a 'mode=on' query opens a TLS session on the device and can last
    // seconds — giving it the exclusive permits would freeze every /api/fs/read of the shell.
    //
    // And COALESCING, which is the real gain here: the same question asked together by several surfaces
    // (search bridges to the copilot, an app asks for the same fact) becomes ONE request
    // only, and everyone reads the same answer. Zero extra bytes, less concurrency: negative cost.
    if (p === '/api/anima' && e.request.method === 'GET') {
      const key = url.pathname + url.search;
      const inflight = animaInflight.get(key);
      if (inflight) { e.respondWith(inflight.then((r) => r.clone())); return; }
      const job = gatedFetch(e.request, false)
        .catch(() => new Response('', { status: 504, statusText: 'device busy' }))
        .finally(() => animaInflight.delete(key));
      animaInflight.set(key, job);
      e.respondWith(job.then((r) => r.clone()));
      return;
    }
    return; // Live/streaming endpoints (chat, logs, llm): straight to the network, no gate, no cache.
  }
  // App assets (/apps/<id>/...): the device serves them no-cache, so without this EVERY cold open
  // re-downloaded the whole app (~25-440 KB) from the single-task httpd. Cache-first, version-keyed:
  // a repeat open hits ZERO device reads. The /apps tree is read-only at runtime (apps persist to
  // /data via /api/fs, never to /apps), so a cached copy can't go stale within a deploy. A deploy
  // bumps CACHE → APP_CACHE rolls; apps.changed flushes it. Model weights are handled above.
  if (url.origin === self.location.origin && p.startsWith('/apps/') && e.request.method === 'GET') {
    e.respondWith((async () => {
      const cache = await caches.open(APP_CACHE);
      const hit = await cache.match(e.request);
      if (hit) return hit;
      try {
        const res = await gatedFetch(e.request);
        if (res && res.ok && res.status === 200) e.waitUntil(cache.put(e.request, res.clone()));
        return res;
      } catch { return new Response('', { status: 504, statusText: 'app asset unavailable' }); }
    })());
    return;
  }
  // Static shell assets: cache first, then network with gate + retry, and a clean
  // fallback so a transient reset does not become an "Uncaught Failed to fetch" in the console.
  e.respondWith((async () => {
    const hit = await caches.match(e.request);
    if (hit) return hit;
    try { return await gatedFetch(e.request); }
    catch { return new Response('', { status: 504, statusText: 'device unreachable' }); }
  })());
});





