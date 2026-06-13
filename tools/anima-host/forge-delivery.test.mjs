// Gate: ANIMA Forge — REAL weight delivery over HTTP Range. Answers "can a model actually be
// downloaded from the device?" by serving a staged shard from a real HTTP server (mimicking
// nucleo_webfs's 206/Content-Range) and having download.js fetch it in BOUNDED windows, reassemble,
// and verify the SHA-256 against the manifest. Proves the mechanism end-to-end over a real socket.
// (The DEVICE's WiFi+SD throughput is ~1 MB/s — a documented one-time cost — but the MECHANISM works;
// CDN-first is the default, the Cardputer SD is the air-gapped fallback.) SKIPS if no model is staged.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import { createReadStream, existsSync, readFileSync, statSync, readdirSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { planRanges, rangeHeader, MAX_WINDOW, verifyManifest } from '../../apps/anima/www/forge/download.js';

const here = dirname(fileURLToPath(import.meta.url));
const modelsDir = join(here, '..', '..', 'deploy', 'sd-safe', 'apps', 'anima', 'www', 'forge', 'models');
const staged = existsSync(modelsDir) ? readdirSync(modelsDir).filter((d) => existsSync(join(modelsDir, d, 'manifest.json'))) : [];

// A tiny Range-capable static server (like nucleo_webfs static_get: 206 + Content-Range, streamed).
function serve(dir) {
  return new Promise((resolve) => {
    const srv = http.createServer((req, res) => {
      const p = join(dir, decodeURIComponent(req.url.replace(/^\//, '')));
      if (!existsSync(p)) { res.writeHead(404); res.end('nf'); return; }
      const size = statSync(p).size;
      const m = /bytes=(\d+)-(\d+)/.exec(req.headers.range || '');
      if (m) {
        const start = +m[1], end = Math.min(+m[2], size - 1);
        res.writeHead(206, { 'Content-Range': `bytes ${start}-${end}/${size}`, 'Accept-Ranges': 'bytes', 'Content-Length': end - start + 1, 'Content-Type': 'application/octet-stream' });
        createReadStream(p, { start, end }).pipe(res);
      } else { res.writeHead(200, { 'Content-Length': size }).end(); }   // whole-file GET path (we must NOT use it)
    });
    srv.listen(0, '127.0.0.1', () => resolve({ srv, port: srv.address().port }));
  });
}

test('weight delivery: a shard downloads in BOUNDED ranges and verifies (or skip)', { skip: staged.length === 0 ? 'no model staged' : false }, async () => {
  const id = staged[0];
  const dir = join(modelsDir, id);
  const manifest = JSON.parse(readFileSync(join(dir, 'manifest.json'), 'utf8'));
  assert.ok(verifyManifest({ model: manifest.model, revision: manifest.revision, shards: manifest.shards }).ok);
  const shard = manifest.shards.slice().sort((a, b) => a.bytes - b.bytes)[0];   // smallest shard (fast)

  const { srv, port } = await serve(dir);
  let requests = 0, non206 = 0;
  try {
    const ranges = planRanges(shard.bytes, MAX_WINDOW);
    assert.ok(ranges.length >= 1);
    const chunks = [];
    for (const r of ranges) {
      requests++;
      const resp = await fetch(`http://127.0.0.1:${port}/${shard.name}`, { headers: { Range: rangeHeader(r) } });
      if (resp.status !== 206) non206++;
      const buf = Buffer.from(await resp.arrayBuffer());
      assert.equal(buf.length, r.end - r.start + 1, `window ${r.start}-${r.end} wrong length`);
      chunks.push(buf);
    }
    const assembled = Buffer.concat(chunks);
    assert.equal(assembled.length, shard.bytes, 'reassembled size matches the manifest');
    const sha = createHash('sha256').update(assembled).digest('hex');
    assert.equal(sha, shard.sha256, 'reassembled SHA-256 matches the manifest (integrity)');
    assert.equal(non206, 0, 'every fetch was a 206 Partial Content (bounded Range, never a whole-file GET)');
    if (shard.bytes > MAX_WINDOW) assert.ok(requests > 1, 'a large shard was fetched across multiple windows');
    console.log(`[delivery] ${id}: ${shard.name} ${(shard.bytes / 1e6).toFixed(1)} MB in ${requests} bounded window(s) → SHA verified ✓`);
  } finally { srv.close(); }
});
