// Stream a single local file onto the device's SD, gently. push-ota uses a buffered fetch()
// body, which drops the TCP connection on a big (~30 MB) upload over Wi-Fi. This mirrors the
// proven path in studio.mjs devicePush(): an http.request with a piped ReadStream so the OS
// applies backpressure and never blooms a huge body — the same reason .nfv pushes succeed.
//
//   node tools/nfv/push-file.mjs <localPath> <devicePath> [--host http://ip] [--pin 123456]
//
// Host/pin default to tools/release.local.json. Use it to vendor the ffmpeg.wasm core when the
// SD lives inside the device (Wi-Fi only). Idempotent: re-run if a transfer drops.
import { request as httpRequest } from 'node:http';
import { createReadStream, promises as fsp, statSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const args = process.argv.slice(2);
const flag = (n) => { const i = args.indexOf(n); return i >= 0 ? args[i + 1] : null; };
const positional = args.filter((a, i) => !a.startsWith('--') && !(i > 0 && args[i - 1].startsWith('--')));
const [localPath, devPath] = positional;
if (!localPath || !devPath) { console.error('usage: push-file.mjs <localPath> <devicePath> [--host ..] [--pin ..]'); process.exit(2); }

async function cfg() {
  let host = flag('--host'), pin = flag('--pin');
  if (!host || !pin) {
    try { const j = JSON.parse((await fsp.readFile(join(REPO, 'tools', 'release.local.json'), 'utf8')).replace(/^﻿/, '')); host = host || j.host; pin = pin || String(j.pin || ''); } catch {}
  }
  if (!host) throw new Error('no --host and no tools/release.local.json');
  if (!/^https?:\/\//.test(host)) host = 'http://' + host;
  return { host, pin: pin || '' };
}

function devReq(host, path, { method = 'GET', headers = {}, body = null, timeout = 0 } = {}) {
  return new Promise((resolve, reject) => {
    const u = new URL(host + path);
    const req = httpRequest({ hostname: u.hostname, port: u.port || 80, path: u.pathname + u.search, method, headers, timeout },
      (res) => { let d = ''; res.on('data', (c) => (d += c)); res.on('end', () => resolve({ status: res.statusCode, body: d, headers: res.headers })); });
    if (timeout) req.on('timeout', () => req.destroy(new Error('timeout')));
    req.on('error', reject);
    if (body && body.pipe) body.pipe(req); else { if (body) req.write(body); req.end(); }
  });
}

async function main() {
  const { host, pin } = await cfg();
  const total = statSync(localPath).size;
  const human = (n) => { for (const u of ['B', 'KB', 'MB']) { if (n < 1024 || u === 'MB') return `${n.toFixed(1)} ${u}`; n /= 1024; } };

  // Pair if required (mirrors studio.mjs).
  let cookie = null;
  const st = await devReq(host, '/api/auth/status').then((r) => JSON.parse(r.body)).catch(() => null);
  if (st && st.required && !st.paired) {
    if (!pin) throw new Error('device needs pairing — pass --pin or set it in release.local.json');
    const pr = await devReq(host, '/api/pair', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ pin }) });
    if (pr.status !== 200) throw new Error('pairing rejected (check PIN)');
    cookie = (/(nucleo_session=[^;]+)/.exec(pr.headers['set-cookie']?.[0] || '') || [])[1] || null;
    if (!cookie) throw new Error('no session cookie from device');
  }

  console.log(`↑ ${devPath}  (${human(total)})  streaming to ${host} …`);
  let sent = 0, lastPct = -1;
  const rs = createReadStream(localPath);
  rs.on('data', (c) => { sent += c.length; const p = Math.round((sent / total) * 100); if (p !== lastPct && p % 5 === 0) { lastPct = p; process.stdout.write(`\r  ${p}%   `); } });
  const headers = { 'content-type': 'application/octet-stream', 'content-length': total };
  if (cookie) headers.cookie = cookie;
  const r = await devReq(host, '/api/fs/write?path=' + encodeURIComponent(devPath), { method: 'POST', headers, body: rs, timeout: 0 });
  process.stdout.write('\r');
  if (r.status !== 200) throw new Error(`device write failed (HTTP ${r.status}): ${r.body.slice(0, 200)}`);
  console.log(`✓ ${devPath}  ${human(total)} written. ${r.body.slice(0, 120)}`);
}
main().catch((e) => { console.error('\nFAILED:', e.message); process.exit(1); });
