// Offline shell cache. Keeps the desktop usable when the device is unreachable;
// API calls always go to the network (never cached) so live data stays fresh.
const CACHE = 'nucleo-shell-v22';
const ASSETS = ['./', 'index.html', 'style.css', 'shell.js', 'wm.js', 'manifest.webmanifest'];

self.addEventListener('install', (e) => {
  e.waitUntil(caches.open(CACHE).then((c) => c.addAll(ASSETS)).then(() => self.skipWaiting()));
});

self.addEventListener('activate', (e) => {
  e.waitUntil(caches.keys().then((ks) =>
    Promise.all(ks.filter((k) => k !== CACHE).map((k) => caches.delete(k)))).then(() => self.clients.claim()));
});

self.addEventListener('fetch', (e) => {
  const url = new URL(e.request.url);
  if (url.pathname.startsWith('/api/')) return; // live data: go to network
  e.respondWith(caches.match(e.request).then((hit) => hit || fetch(e.request)));
});
