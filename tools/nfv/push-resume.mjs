// Push a big file to a NucleoOS device in resumable parts (/api/fs/write?append=1&at=<bytes>).
//
// A single-request upload of a film (tens of MB, minutes) dies with the first Wi-Fi drop. This one sends
// 512 KB parts; the device appends a part only when its copy is exactly `at` bytes long, and answers with its
// real size otherwise — so after a drop (or a reboot) the upload carries on from what actually landed. The
// file is written as <name>.part and renamed into place once complete.
//
//   node tools/nfv/push-resume.mjs --host http://<device-ip> --pin <pin> "tools/nfv/out/Clip.nfv" [--dir /data/Videos]
import { open, stat } from 'node:fs/promises';
import { basename } from 'node:path';

const args = process.argv.slice(2);
const opt = { host: null, pin: null, dir: '/data/Videos', files: [] };
for (let i = 0; i < args.length; i++) {
  const v = args[i];
  if (v === '--host') opt.host = args[++i];
  else if (v === '--pin') opt.pin = String(args[++i] || '').trim();
  else if (v === '--dir') opt.dir = args[++i];
  else opt.files.push(v);
}
if (!opt.host || !opt.files.length) { console.error('usage: --host <url> [--pin <pin>] <file> [--dir /data/Videos]'); process.exit(2); }
const PART = 512 * 1024;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let cookie = null;

async function req(path, init = {}, tries = 1) {
  init.headers = { ...(init.headers || {}), ...(cookie ? { cookie } : {}) };
  init.signal = AbortSignal.timeout(init.timeout || 60000);
  return fetch(opt.host + path, init);
}
async function pair() {
  const st = await req('/api/auth/status').then((r) => r.json()).catch(() => null);
  if (!st || !st.required || st.paired) return;
  const r = await req('/api/pair', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ pin: opt.pin }) });
  const m = /(?:^|,\s*)(nucleo_session=[^;]+)/.exec(r.headers.get('set-cookie') || '');
  if (m) cookie = m[1];
}
async function waitDevice() {
  for (let i = 0; i < 120; i++) {
    try { const r = await req('/api/status', { timeout: 5000 }); if (r.ok) return; } catch {}
    await sleep(3000);
  }
  throw new Error('device unreachable');
}

for (const file of opt.files) {
  const size = (await stat(file)).size;
  const name = basename(file);
  const dst = `${opt.dir}/${name}`, part = `${dst}.part`;
  const fh = await open(file, 'r');
  await waitDevice(); await pair();
  let at = 0, fails = 0, t0 = Date.now();
  // Where does the device's .part stand? (409 tells us; a fresh start writes from 0.)
  {
    const r = await req(`/api/fs/write?path=${encodeURIComponent(part)}&append=1&at=-2`, { method: 'POST', body: new Uint8Array(0) }).catch(() => null);
    if (r && (r.ok || r.status === 409)) { at = (await r.json()).size || 0; if (at > size) at = 0; }
  }
  if (at === 0) await req(`/api/fs/delete?path=${encodeURIComponent(part)}`, { method: 'POST' }).catch(() => {});
  while (at < size) {
    const n = Math.min(PART, size - at);
    const buf = Buffer.alloc(n);
    await fh.read(buf, 0, n, at);
    try {
      const r = await req(`/api/fs/write?path=${encodeURIComponent(part)}&append=1&at=${at}`,
                          { method: 'POST', body: buf, timeout: 120000 });
      const j = await r.json().catch(() => ({}));
      if (r.status === 401) { await pair(); continue; }
      if (r.ok || r.status === 409) { at = j.size ?? at; fails = 0; }
      else throw new Error('HTTP ' + r.status);
      const kbs = at / 1024 / ((Date.now() - t0) / 1000);
      process.stdout.write(`\r  ${name}: ${(at / 1048576).toFixed(1)} / ${(size / 1048576).toFixed(1)} MB  ${kbs.toFixed(0)} KB/s   `);
    } catch (e) {
      if (++fails > 30) throw new Error('too many failures: ' + e.message);
      process.stdout.write(`\n  drop (${e.message}) — waiting for the device, resuming at ${at}\n`);
      await waitDevice(); await pair();
      const r = await req(`/api/fs/write?path=${encodeURIComponent(part)}&append=1&at=-2`, { method: 'POST', body: new Uint8Array(0) }).catch(() => null);
      if (r && (r.ok || r.status === 409)) at = (await r.json()).size || 0;
    }
  }
  await fh.close();
  await req(`/api/fs/delete?path=${encodeURIComponent(dst)}`, { method: 'POST' }).catch(() => {});
  const mv = await req(`/api/fs/move?from=${encodeURIComponent(part)}&to=${encodeURIComponent(dst)}`, { method: 'POST' });
  console.log(`\n✓ ${dst} (${(size / 1048576).toFixed(1)} MB) ${mv.ok ? '' : '— rename failed: ' + mv.status}`);
}
