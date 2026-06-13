// Push NFV video clips (.nfv + sibling .mp3) to a NucleoOS device's /data/Videos.
//
// The standard push-ota.mjs deliberately skips /data/ (media is huge, not OS code), so
// media gets its own uploader. The device's /api/fs/write streams the body straight to a
// .tmp file on the SD card and renames it atomically — no full-file RAM buffering — so big
// clips are fine despite the 512 KB SRAM.
//
//   node tools/nfv/push-video.mjs --host http://192.168.0.166 --pin 689614 "tools/nfv/out/Clip.nfv"
//
// Pass the .nfv path(s); the sibling .mp3 (same basename) is uploaded automatically.
import { readFile, stat } from 'node:fs/promises';
import { basename, dirname, join } from 'node:path';

function parseArgs(argv) {
  const a = { host: null, pin: null, dir: '/data/Videos', files: [] };
  for (let i = 0; i < argv.length; i++) {
    const v = argv[i];
    if (v === '--host' || v === '-h') a.host = argv[++i];
    else if (v === '--pin' || v === '-p') a.pin = String(argv[++i] || '').trim();
    else if (v === '--dir') a.dir = argv[++i];
    else if (!a.host && /^https?:\/\//i.test(v)) a.host = v;
    else a.files.push(v);
  }
  return a;
}
const normHost = (h) => (h && !/^https?:\/\//i.test(h) ? 'http://' + h : h);

let cookie = null;
const auth = () => (cookie ? { cookie } : {});

async function pair(host, pin) {
  const r = await fetch(host + '/api/auth/status', { cache: 'no-store' }).then(r => r.json()).catch(() => null);
  if (r && r.required && !r.paired) {
    if (!pin) throw new Error('device requires pairing — pass --pin <code> from the Cardputer screen');
    const pr = await fetch(host + '/api/pair', {
      method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ pin }),
    });
    if (!pr.ok) throw new Error(`pairing rejected (HTTP ${pr.status}) — check the PIN`);
    const m = /(?:^|,\s*)(nucleo_session=[^;]+)/.exec(pr.headers.get('set-cookie') || '');
    cookie = m ? m[1] : null;
    if (!cookie) throw new Error('paired but no session cookie returned');
    console.log('✓ paired');
  }
}

async function mkdirp(host, dir) {
  const parts = dir.split('/').filter(Boolean);
  let cur = '';
  for (const p of parts) {
    cur += '/' + p;
    await fetch(host + '/api/fs/mkdir?path=' + encodeURIComponent(cur), { method: 'POST', headers: auth() }).catch(() => {});
  }
}

function human(n) { for (const u of ['B', 'KB', 'MB', 'GB']) { if (n < 1024 || u === 'GB') return `${n.toFixed(1)} ${u}`; n /= 1024; } }

async function upload(host, localPath, devPath) {
  const body = await readFile(localPath);
  const t0 = Date.now();
  const r = await fetch(host + '/api/fs/write?path=' + encodeURIComponent(devPath), {
    method: 'POST',
    headers: { ...auth(), 'content-type': 'application/octet-stream', 'content-length': String(body.length) },
    body,
  });
  if (!r.ok) throw new Error(`write ${devPath} failed: HTTP ${r.status} ${await r.text().catch(() => '')}`);
  const secs = (Date.now() - t0) / 1000;
  console.log(`✓ ${devPath}  (${human(body.length)} in ${secs.toFixed(1)}s, ${human(body.length / secs)}/s)`);
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const host = normHost(args.host);
  if (!host || args.files.length === 0) {
    console.error('Usage: node tools/nfv/push-video.mjs --host http://<ip> [--pin <code>] <clip.nfv> [...]');
    process.exit(1);
  }
  const status = await fetch(host + '/api/status').then(r => r.json()).catch(e => { throw new Error(`cannot reach ${host}: ${e.message}`); });
  console.log(`✓ device ${host} (v${status.version || '?'}, free ${human(status.storage?.free_bytes || 0)})`);
  await pair(host, args.pin);
  await mkdirp(host, args.dir);

  for (const nfv of args.files) {
    const mp3 = join(dirname(nfv), basename(nfv).replace(/\.nfv$/i, '.mp3'));
    await upload(host, nfv, args.dir + '/' + basename(nfv));
    try { await stat(mp3); await upload(host, mp3, args.dir + '/' + basename(mp3)); }
    catch { console.log(`  (no sibling mp3 for ${basename(nfv)} — video only)`); }
  }
  console.log('done.');
}
main().catch(e => { console.error('✗', e.message); process.exit(1); });
