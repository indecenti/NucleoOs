// push-files.mjs — surgical, immediate file push to a NucleoOS device over the web API.
//
// Unlike push-ota.mjs --sync (which LISTS every directory and content-READs each staged file to
// compute a delta — heavy on a PSRAM-less, battery-marginal device), this writes EXACTLY the files
// you name and nothing else: pair once, then one POST /api/fs/write per file. No list, no read,
// no manifest scan. Precise + immediate.
//
// Paths land on whatever SD is currently in the device, at the device-relative path you give —
// which is exactly what nucleo_webfs reads from (URL /sw.js -> SD www/shell/sw.js, etc.), so it's
// immune to which physical card is inserted.
//
//   node tools/push-files.mjs --host http://192.168.0.166 --pin 318134 \
//        <local>:<devicePath> [<local>:<devicePath> ...]
//
// Example device paths: www/shell/sw.js , apps/groq-chat/www/index.html
import { readFile } from 'node:fs/promises';

const argv = process.argv.slice(2);
let host = null, pin = null;
const pairs = [];
for (let i = 0; i < argv.length; i++) {
  const v = argv[i];
  if (v === '--host' || v === '-h') host = String(argv[++i] || '').replace(/\/+$/, '');
  else if (v === '--pin' || v === '-p') pin = String(argv[++i] || '').trim();
  else if (v.includes(':')) {
    const idx = v.lastIndexOf(':');                 // lastIndexOf so a Windows "C:\.." local path still splits on the trailing :dev
    pairs.push({ local: v.slice(0, idx), dev: v.slice(idx + 1).replace(/^\/+/, '') });
  }
}
if (!host || !pairs.length) { console.error('usage: --host <url> --pin <code> <local>:<devicePath> ...'); process.exit(2); }

const timeout = (ms) => { const c = new AbortController(); const t = setTimeout(() => c.abort(), ms); return { signal: c.signal, done: () => clearTimeout(t) }; };
async function f(url, opt, ms = 15000) { const g = timeout(ms); try { return await fetch(url, { ...opt, signal: g.signal }); } finally { g.done(); } }

// Pair with the screen PIN -> nucleo_session cookie reused on every write (same flow as push-ota.mjs).
let cookie = null;
if (pin) {
  const r = await f(host + '/api/pair', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ pin }) });
  if (!r.ok) { console.error(`✗ pairing rejected (HTTP ${r.status}) — check the PIN on the Cardputer`); process.exit(1); }
  const m = /(?:^|,\s*)(nucleo_session=[^;]+)/.exec(r.headers.get('set-cookie') || '');
  cookie = m ? m[1] : null;
  if (!cookie) { console.error('✗ paired but no session cookie returned'); process.exit(1); }
}
const auth = cookie ? { cookie } : {};

let ok = 0, fail = 0;
for (const { local, dev } of pairs) {
  let buf;
  try { buf = await readFile(local); } catch (e) { console.error(`✗ ${local}: cannot read (${e.message})`); fail++; continue; }
  let wrote = false;
  for (let attempt = 1; attempt <= 3 && !wrote; attempt++) {
    try {
      const r = await f(host + '/api/fs/write?path=' + encodeURIComponent(dev), { method: 'POST', body: buf, headers: auth }, 20000);
      if (r.ok) { console.log(`✓ ${dev}  (${buf.length} B)`); ok++; wrote = true; }
      else { console.error(`  …${dev} HTTP ${r.status} (attempt ${attempt})`); await new Promise(s => setTimeout(s, 800 * attempt)); }
    } catch (e) { console.error(`  …${dev} ${e.message} (attempt ${attempt})`); await new Promise(s => setTimeout(s, 800 * attempt)); }
  }
  if (!wrote) { console.error(`✗ ${dev}: failed after 3 attempts`); fail++; }
}
console.log(`\nPush: ${ok} written, ${fail} failed.`);
process.exit(fail ? 1 : 0);
