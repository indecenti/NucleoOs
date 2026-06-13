// Tiny static server for visually verifying games/pong3d.js outside NucleoOS. It mirrors the device's
// URL space so the renderer's absolute imports (/apps/games/...) resolve to apps/games/www/...
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const HERE = path.dirname(fileURLToPath(import.meta.url));
const WWW = path.resolve(HERE, '../../apps/games/www');
const MIME = { '.js': 'text/javascript', '.mjs': 'text/javascript', '.json': 'application/json', '.html': 'text/html', '.css': 'text/css', '.svg': 'image/svg+xml', '.png': 'image/png', '.wasm': 'application/wasm' };
function send(res, file) {
  fs.readFile(file, (e, buf) => {
    if (e) { res.writeHead(404); res.end('404 ' + file); return; }
    res.writeHead(200, { 'content-type': MIME[path.extname(file).toLowerCase()] || 'application/octet-stream', 'cache-control': 'no-store' });
    res.end(buf);
  });
}
http.createServer((req, res) => {
  const url = decodeURIComponent(req.url.split('?')[0]);
  if (url === '/' || url === '') return send(res, path.join(HERE, 'index.html'));
  if (url.startsWith('/apps/games/')) return send(res, path.join(WWW, url.slice('/apps/games/'.length)));
  return send(res, path.join(HERE, url.replace(/^\//, '')));
}).listen(process.env.PORT || 8099, '127.0.0.1', () => console.log('pong3d verify → http://127.0.0.1:' + (process.env.PORT || 8099)));
