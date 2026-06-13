// Offline shell cache. Keeps the desktop usable when the device is unreachable;
// API calls always go to the network (never cached) so live data stays fresh.
const CACHE = 'nucleo-shell-v56';   // v56: OS-wide single-download gate (dlgate.js); boot pre-copy now opt-in/off — no automatic downloads
const ASSETS = ['./', 'index.html', 'style.css', 'copilot.css', 'shell.js', 'copilot.js', 'shortcuts.js', 'wm.js', 'fsindex.js', 'busy.js', 'dlgate.js', 'manifest.webmanifest', 'wallpaper.png'];

self.addEventListener('install', (e) => {
  // Resilient precache: add each asset INDEPENDENTLY (not addAll, which is atomic — a single 404
  // would abort the whole install, leaving the shell with NO offline cache and spamming the console,
  // exactly the failure we hit when copilot.*/shortcuts.js were missing). Everything present is still
  // cached; a stray miss is tolerated, so a future renamed asset can't take the desktop offline.
  e.waitUntil((async () => {
    const c = await caches.open(CACHE);
    await Promise.allSettled(ASSETS.map((a) => c.add(a)));
    await self.skipWaiting();
  })());
});

self.addEventListener('activate', (e) => {
  e.waitUntil(caches.keys().then((ks) =>
    Promise.all(ks.filter((k) => k !== CACHE).map((k) => caches.delete(k)))).then(() => self.clients.claim()));
});

self.addEventListener('fetch', (e) => {
  const url = new URL(e.request.url);
  if (url.pathname.startsWith('/api/')) {
    // Caching dinamico (lato client) per le immagini via API (come lo sfondo)
    if (url.pathname === '/api/fs/read' && url.searchParams.get('path')?.match(/\.(png|jpe?g|gif|svg)$/i)) {
      e.respondWith(caches.match(e.request).then((hit) => {
        if (hit) return hit; // Ritorna l'immagine dalla cache istantaneamente
        return fetch(e.request).then((res) => {
          if (res.ok) {
            const copy = res.clone();
            caches.open(CACHE).then((c) => c.put(e.request, copy));
          }
          return res;
        });
      }));
      return;
    }
    return; // Dati vivi (JSON, file testuali): non fare cache, vai in rete.
  }
  e.respondWith(caches.match(e.request).then((hit) => hit || fetch(e.request)));
});





