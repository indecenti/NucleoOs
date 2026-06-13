// refix-dos — rebuild the DOS bundles on a device so they match the one that works.
//
// Some bundles were authored with a minimal dosbox.conf (no `mount c .`) and later patched
// in place; to be safe we now REBUILD each one from scratch: same files, a clean store-method
// ZIP (identical encoding to the working digger.jsdos), and a dosbox.conf whose non-autoexec
// sections are copied verbatim from digger.jsdos (the known-good reference) with only the
// [autoexec] swapped to mount C: and run that game's executable.
//
// Usage: node tools/refix-dos.mjs --host http://192.168.0.166 --pin 689614 [--ref digger.jsdos]
import zlib from 'node:zlib';
import { writeFileSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { packStoreToFile } from './make-jsdos.mjs';

const args = process.argv.slice(2);
const opt = (k) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : null; };
const host = (opt('--host') || 'http://192.168.0.166').replace(/\/$/, '');
const pin = opt('--pin');
const ref = opt('--ref') || 'digger.jsdos';
const DIR = '/data/DOS';
const RUNNABLE = /\.(exe|com|bat)$/i;
if (!pin) { console.error('need --pin <code>'); process.exit(1); }

// ---- minimal ZIP reader (store + deflate) ----
function unzip(u8) {
  const dv = (o) => u8[o] | (u8[o + 1] << 8);
  const dw = (o) => (u8[o] | (u8[o + 1] << 8) | (u8[o + 2] << 16) | (u8[o + 3] << 24)) >>> 0;
  let p = u8.length - 22;
  while (p >= 0 && dw(p) !== 0x06054b50) p--;
  if (p < 0) throw new Error('no EOCD (not a zip)');
  const count = dv(p + 10); let cd = dw(p + 16);
  const out = [];
  for (let i = 0; i < count; i++) {
    if (dw(cd) !== 0x02014b50) break;
    const method = dv(cd + 10), csize = dw(cd + 20), nlen = dv(cd + 28), elen = dv(cd + 30), clen = dv(cd + 32), lho = dw(cd + 42);
    const name = Buffer.from(u8.subarray(cd + 46, cd + 46 + nlen)).toString('latin1');
    const lnlen = dv(lho + 26), lelen = dv(lho + 28);
    const start = lho + 30 + lnlen + lelen;
    const comp = Buffer.from(u8.subarray(start, start + csize));
    if (!name.endsWith('/')) {
      const data = method === 0 ? comp : method === 8 ? zlib.inflateRawSync(comp) : null;
      if (!data) throw new Error('unsupported method ' + method + ' in ' + name);
      out.push({ name, data });
    }
    cd += 46 + nlen + elen + clen;
  }
  return out;
}

let cookie = '';
async function pair() {
  const r = await fetch(host + '/api/pair', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ pin }) });
  if (!r.ok) throw new Error('pair failed: ' + r.status);
  cookie = (r.headers.get('set-cookie') || '').match(/nucleo_session=[^;]+/)[0];
}
const get = async (p) => new Uint8Array(await (await fetch(host + '/api/fs/read?path=' + encodeURIComponent(p), { headers: { cookie } })).arrayBuffer());
const list = async (p) => (await (await fetch(host + '/api/fs/list?path=' + encodeURIComponent(p), { headers: { cookie } })).json()).entries || [];
async function put(p, buf) {
  const r = await fetch(host + '/api/fs/write?path=' + encodeURIComponent(p), { method: 'POST', headers: { cookie }, body: buf });
  if (!r.ok) throw new Error('write failed ' + r.status); return r.text();
}

const confOf = (entries) => { const c = entries.find((e) => e.name === '.jsdos/dosbox.conf'); return c ? c.data.toString('latin1') : ''; };
// The run command a bundle intends: prefer the last real command in its current autoexec,
// else the lone root executable.
function runCmd(entries) {
  const conf = confOf(entries);
  const tail = conf.replace(/[\s\S]*?\[autoexec\]/, '');
  const cmds = tail.split(/\r?\n/).map((s) => s.trim()).filter((l) =>
    l && !/^[#;]/.test(l) && !/^@?echo\b/i.test(l) && !/^exit\b/i.test(l) && !/^mount\b/i.test(l) && !/^cd\b/i.test(l) && !/^[a-z]:$/i.test(l));
  if (cmds.length) return cmds[cmds.length - 1];
  const exe = entries.map((e) => e.name).filter((n) => RUNNABLE.test(n)).sort((a, b) => a.split('/').length - b.split('/').length)[0];
  if (!exe) throw new Error('no executable found');
  return /\.bat$/i.test(exe) ? 'call ' + exe : exe;
}

async function main() {
  await pair();
  console.log('paired with', host);
  const refEntries = unzip(await get(DIR + '/' + ref));
  const refConf = confOf(refEntries);
  // everything in the reference conf before [autoexec] is the proven-good environment
  const template = refConf.replace(/\[autoexec\][\s\S]*$/, '').replace(/\s+$/, '');
  if (!template) throw new Error('reference ' + ref + ' has no usable conf');

  const names = (await list(DIR)).map((e) => e.name).filter((n) => n.endsWith('.jsdos') && n !== ref);
  for (const name of names) {
    try {
      const entries = unzip(await get(DIR + '/' + name)).filter((e) => !e.name.toLowerCase().startsWith('.jsdos/'));
      const run = runCmd(unzip(await get(DIR + '/' + name)));   // detect from the existing conf/files
      const conf = template + '\n[autoexec]\necho off\nmount c .\nc:\n' + run + '\n';
      entries.push({ name: '.jsdos/dosbox.conf', data: Buffer.from(conf, 'latin1') });
      const out = join(tmpdir(), name);
      const res = packStoreToFile(out, entries.map((e) => ({ name: e.name, data: e.data })));
      await put(DIR + '/' + name, readFileSync(out));
      console.log(`rebuilt ${name}: ${res.files} files, ${res.bytes} bytes, runs ${run}`);
    } catch (e) { console.error(`FAILED ${name}: ${e.message}`); }
  }
  console.log('done — hard-refresh DOS Box and try each game.');
}
main().catch((e) => { console.error(e.message); process.exit(1); });
