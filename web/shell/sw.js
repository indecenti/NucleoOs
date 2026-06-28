// Offline shell cache. Keeps the desktop usable when the device is unreachable;
// API calls always go to the network (never cached) so live data stays fresh.
const CACHE = 'nucleo-shell-v106';  // v105: Voice Recorder gains BROWSER-side CHUNKED transcription for long takes (1-2h) — pulls the WAV in byte ranges (/api/fs/read 206), transcribes ≤8-min segments directly against Whisper (browser Groq key) + map-reduce summary, bypassing the device's 25MB/single-shot-TLS limit; new longtranscribe.js, device path kept for MP3/no-key. v104: ANIMA chat gains the same 🌐 Web mode — a compose-box toggle routes the turn to Groq's compound (built-in web search) for current, CITED answers (Sources list, deduped), Groq-only + browser-direct, gated behind a Groq key, overriding the selected mode when armed; reuses ANIMA's markdown renderer + engine-map (🌐 Online · Web). v103: AI Chat (groq-chat) gains a 🌐 Web mode — routes the turn to Groq's agentic "compound" system (built-in web search) for current, CITED answers; non-streamed so executed_tools search_results render as a clean Sources list, with a firm system prompt suppressing compound's raw process/code dumps. v102: AI Chat (groq-chat) now RENDERS Markdown — replies were shown raw (## headings, |tables|, **bold**, lists, links all literal). Added a compact, XSS-safe block+inline renderer (code fences already worked). v101: AI Chat (groq-chat) dark-mode fix — the model dropdown rendered its native <option> popup on a white OS background (unreadable in dark): added color-scheme:dark + explicit dark <option> colors. v100: roll APP_CACHE so the fixed AI Chat (groq-chat) loads — it now calls Groq BROWSER-DIRECT (api.groq.com) instead of the device /api/llm proxy, which OOMed mid-stream (503 low-mem-for-TLS / errno 113) on this PSRAM-less chip; the stale v99 copy kept hammering the dying proxy. v99: Start-menu Android button now serves the REAL built NucleoMind APK (/downloads/NucleoMind.apk, com.nucleoos.mind — the on-device LLM companion); the never-built NucleoOS Capacitor wrapper (android-app, needs Android Studio/JDK17) stays a build-machine TODO. v98: File Commander hides .gz shadow dupes (each was a dead click → stray /api/fs/list 404). v97: app icons loading="lazy" (cut the cold-load icon burst in every browser; the SW governor is inert over http LAN IP so this is the only client-side throttle that runs). v96: anti-overload pass — /api/fs/list now goes through the SW MAX_INFLIGHT=2 gate (was bypassing it: crawl/reconcile/File-Commander list traffic stacked on the device); + adaptive /api/status polling (pause-when-hidden + error backoff). v95: roll APP_CACHE so the updated ANIMA app loads — web offline ANIMA now runs in-browser (WASM) and NEVER scales onto the Cardputer (skipDevice); the device keeps L1 unloaded while a web client is connected. v94: (a) shell connects to /ws?shell=1 so ONLY the real web OS triggers the device's remote handoff (screen-off + RAM reclaim) — a standalone app page opening /ws no longer blanks the device; (b) close /ws on pagehide so the device reclaims it in ~4s on tab close instead of waiting the ~20s keep-alive reaper. v93: web-OS boot resilience — fetchJSON retries 503/timeout w/ backoff (boot flooded the single-task PSRAM-less device → /api/apps timed out → boot hung → /ws never attached) + SW concurrency 3->2 + boot logs. v92: Costellazioni — longer/harder missions (7-12 waves, tougher foes), missiles 3 + pickups (missile/shield/repair drops), richer SFX + dynamic music bed. v91: 8 background biomes per sector w/ smooth cross-fade + whoosh, enemy threat-colour ramp, more waves (4-9), punchier SFX. v90: right-mouse fires missiles + render-side interpolation of enemy ships (silky, no 30Hz stutter). v89: bump to roll APP_CACHE so the rebuilt Costellazioni 3D web game (real glTF ship models + GLTFLoader, arcade HUD, missiles, dynamic warp/nebula, engage-and-hold combat, varied missions) actually loads instead of the stale cached copy. v88: /ws reconnect discipline — single socket (never parallel), exponential backoff (3s→30s) instead of a fixed 3s storm, and pause while the tab is hidden; kills the endless failed-handshake requests at the PSRAM-less device when unpaired/offline. v87: boot loading screen = full-screen animated atom (canvas, web twin of the firmware splash) replacing the spinner; stops itself on .hidden. v86: ANIMA online-only fallback ladder — cloud chat ALWAYS browser-direct (even exec=device; firmware mbedTLS OOMs on this PSRAM-less chip), deleted the firmware online rung, browser-only labelled offline degrade (skipDevice). v85: drop the 535KB wallpaper.png from precache (it's a JPEG-misnamed-.png never displayed — the live wallpaper is /data/Pictures/wallpaper.png; it only tripped the webfs low-heap defer). v84: agentic loop browser-direct independent of exec. v83: workspace invite-before-write + WS4/WS5 fixes. v82: loop as default online engine. v81: multi-model router + workspace-as-context
// Per-version cache for app assets (/apps/<id>/...). Tied to the shell version so a deploy (which
// bumps CACHE) drops it; the shell also flushes it on apps.changed (OTA app update) via postMessage.
const APP_CACHE = CACHE + '-apps';
const ASSETS = ['./', 'index.html', 'style.css', 'copilot.css', 'notify.css', 'onboarding.css', 'shell.js', 'boot-fetch.js', 'copilot.js', 'notify.js', 'onboarding.js', 'ai.js', 'ai-keys.js', 'shortcuts.js', 'wm.js', 'fsindex.js', 'busy.js', 'dlgate.js', 'micgate.js', 'nucleo-i18n.js', 'i18n/core.it.json', 'i18n/core.en.json', 'i18n/shell.it.json', 'i18n/shell.en.json', 'manifest.webmanifest', 'icon.png'];   // NB: wallpaper.png removed — it's a 535KB JPEG-misnamed-.png never displayed (live wallpaper = /data/Pictures/wallpaper.png) that only tripped the webfs low-heap defer

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
  const keep = new Set([CACHE, APP_CACHE, MODEL_CACHE]);
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
    // Caching dinamico (lato client) per le immagini via API (come lo sfondo)
    if (p === '/api/fs/read' && url.searchParams.get('path')?.match(/\.(png|jpe?g|gif|svg)$/i)) {
      e.respondWith(caches.match(e.request).then((hit) => {
        if (hit) return hit; // Ritorna l'immagine dalla cache istantaneamente
        return gatedFetch(e.request).then((res) => {
          if (res.ok) {
            const copy = res.clone();
            caches.open(CACHE).then((c) => c.put(e.request, copy));
          }
          return res;
        }).catch(() => new Response('', { status: 504, statusText: 'device busy' }));
      }));
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





