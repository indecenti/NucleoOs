// Offline shell cache. Keeps the desktop usable when the device is unreachable;
// API calls always go to the network (never cached) so live data stays fresh.
// Bump this on every shell change that must reach already-installed clients. The reason for each
// roll goes in docs/shell-cache-log.md — NOT here: it used to be one 10.5 KB comment on this line,
// half the whole service worker, re-shipped to every browser on every update check.
const CACHE = 'nucleo-shell-v132';   // v132 — settings quick-toggle label/value stacking fix
// Per-version cache for app assets (/apps/<id>/...). Tied to the shell version so a deploy (which
// bumps CACHE) drops it; the shell also flushes it on apps.changed (OTA app update) via postMessage.
const APP_CACHE = CACHE + '-apps';
const ASSETS = ['./', 'index.html', 'style.css', 'copilot.css', 'notify.css', 'onboarding.css', 'shell.js', 'boot-fetch.js', 'copilot.js', 'notify.js', 'onboarding.js', 'ambient.js', 'ai.js', 'ai-keys.js', 'shortcuts.js', 'search-rank.js', 'appbroker.js', 'wm.js', 'fsindex.js', 'busy.js', 'dlgate.js', 'micgate.js', 'system-ui.js', 'nucleo-i18n.js', 'i18n/core.it.json', 'i18n/core.en.json', 'i18n/core.es.json', 'i18n/core.fr.json', 'i18n/core.de.json', 'i18n/shell.it.json', 'i18n/shell.en.json', 'i18n/shell.es.json', 'i18n/shell.fr.json', 'i18n/shell.de.json', 'manifest.webmanifest', 'icon.png'];   // NB: wallpaper.png removed — it's a 535KB JPEG-misnamed-.png never displayed (live wallpaper = /data/Pictures/wallpaper.png) that only tripped the webfs low-heap defer

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
async function netFetch(req, signal) {
  try { return await fetch(req, signal ? { signal } : undefined); }
  catch (err) {
    // A transient lru_purge reset / momentary OOM. Replaying a body is unsafe, so
    // only retry idempotent GETs (no body) after a short breath.
    if (req.method !== 'GET') throw err;
    await new Promise((r) => setTimeout(r, 250));
    return await fetch(req);
  }
}
async function gatedFetch(req, exclusive) {
  const need = exclusive ? MAX_INFLIGHT : 1;
  await acquire(need);
  try {
    // A write holds the WHOLE pool, so cap it with a timeout: a hung exclusive lock would
    // otherwise freeze the desktop. A tiny JSON save to SD is sub-second; 15s is generous.
    return await netFetch(req, exclusive ? AbortSignal.timeout(15000) : null);
  } finally { release(need); }
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
        const network = gatedFetch(e.request).then((res) => {
          if (res && res.ok) cache.put(e.request, res.clone());
          return res;
        }).catch(() => null);
        if (hit) { e.waitUntil(network); return hit; }          // instant; refresh silently
        const res = await network;
        return res || new Response('', { status: 504, statusText: 'device busy' });
      })());
      return;
    }
    // Letture/scritture file: richiesta/risposta corte, mai in cache (dati vivi). La WRITE
    // gira ESCLUSIVA (prende tutti i permessi): il device la serve da sola, con tutto l'heap
    // libero, così trova un blocco contiguo grande invece di andare in OOM a metà burst.
    if (p === '/api/fs/read' || p === '/api/fs/list' || p === '/api/fs/write') {
      const exclusive = (p === '/api/fs/write');   // list = shared read (need=1), like read
      e.respondWith(gatedFetch(e.request, exclusive).catch(() => new Response('', { status: 504, statusText: 'device busy' })));
      return;
    }
    // /api/anima: la domanda all'assistente. Dodici superfici la chiamano — copilot, ricerca della
    // shell, onboarding, ai.js, e le app anima/agent/settings/spreadsheet/games/miei-fatti/recorder/
    // code-runner — e nessuna di loro sa delle altre: la regola "mai chiamate concorrenti" era affidata
    // alla buona educazione di dodici file. Su un chip senza PSRAM, con 4-6 socket condivisi da tutti
    // gli iframe, due domande insieme bastano a far scadere le letture file dell'OS.
    //
    // Slot CONDIVISO (need=1), non esclusivo: una query 'mode=on' apre una TLS sul device e puo' durare
    // secondi — dandole i permessi esclusivi congelerebbe ogni /api/fs/read della shell.
    //
    // E COALESCENZA, che qui e' il guadagno vero: la stessa domanda posta insieme da piu' superfici
    // (la ricerca fa da ponte verso il copilot, un'app chiede lo stesso fatto) diventa UNA richiesta
    // sola, e tutti leggono la stessa risposta. Zero byte in piu', meno concorrenza: costo negativo.
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
    return; // Endpoint live/streaming (chat, logs, llm): dritti in rete, niente gate, niente cache.
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
  // Asset statici dello shell: prima la cache, poi rete con gate + retry, e un fallback
  // pulito così un reset transitorio non diventa un "Uncaught Failed to fetch" in console.
  e.respondWith((async () => {
    const hit = await caches.match(e.request);
    if (hit) return hit;
    try { return await gatedFetch(e.request); }
    catch { return new Response('', { status: 504, statusText: 'device unreachable' }); }
  })());
});





