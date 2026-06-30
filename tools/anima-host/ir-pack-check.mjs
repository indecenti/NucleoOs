// Host gate for the IR preset pack: prove the packer (tools/ir-pack.mjs) and the firmware reader
// (nucleo_ir_pack.c) agree byte-for-byte, on RAM that stays O(one record). Strategy:
//   1. pack a synthetic catalog (with truncation + empty-button edge cases) -> .bin
//   2. compile + run the C dumper (ir-pack-ctest.c) over it
//   3. diff the C reader's records against the JS unpack() reference
//   4. repeat against the REAL apps/ir-remote/www/presets.json so the shipped DB is validated too
// Wired as `npm run irpack:test`.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, writeFileSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { pack, unpack, PROTOS, REGIONS } from '../ir-pack.mjs';

const ROOT = process.cwd();
const BUILD = join(ROOT, 'build');
mkdirSync(BUILD, { recursive: true });
const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };

const exe = join(BUILD, 'irpackctest.exe');
const cc = spawnSync(GCC, [
  '-std=gnu11', '-O0', '-Wall', '-Wextra',
  '-I', 'firmware/components/nucleo_ir/include',
  'tools/anima-host/ir-pack-ctest.c',
  'firmware/components/nucleo_ir/nucleo_ir_pack.c',
  '-o', exe,
], { cwd: ROOT, env, encoding: 'utf8' });
if (cc.status !== 0) {
  console.error('IR pack: COMPILE FAILED');
  process.stdout.write(cc.stdout || ''); process.stderr.write(cc.stderr || '');
  process.exit(1);
}

let fails = 0;
const check = (cond, msg) => { if (!cond) { console.log('  FAIL:', msg); fails++; } };
const eqArr = (a, b) => Array.isArray(a) && Array.isArray(b) && a.length === b.length && a.every((v, k) => v === b[k]);

// Parse the C dumper output into the same shape unpack() returns.
function dumpToModel(out) {
  const remotes = [], tvpower = []; let hdr = null, oob = null;
  for (const line of out.split('\n')) {
    const f = line.split('\t');
    if (f[0] === 'H') hdr = { n_remotes: +f[1], n_tvpower: +f[2] };
    else if (f[0] === 'R') remotes[+f[1]] = { name: f[2], proto: +f[3], region: +f[4], nbtn: +f[5], addr: +f[6], btns: [] };
    else if (f[0] === 'B') remotes[+f[1]].btns[+f[2]] = { key: f[3], cmd: +f[4] };
    else if (f[0] === 'RB') remotes[+f[1]].btns[+f[2]].raw = { carrier: +f[3], dur: f.slice(5).map(Number) };
    else if (f[0] === 'T') tvpower[+f[1]] = { brand: f[2], region: +f[3], proto: +f[4], addr: +f[5], cmd: +f[6] };
    else if (f[0] === 'TR') tvpower[+f[1]].raw = { carrier: +f[2], dur: f.slice(4).map(Number) };
    else if (f[0] === 'OOB') oob = { rem: +f[1], tvp: +f[2] };
  }
  return { hdr, remotes, tvpower, oob };
}

function runCReader(bin) {
  const r = spawnSync(exe, [bin], { cwd: ROOT, env, encoding: 'utf8' });
  if (r.status !== 0) { console.log('  FAIL: C reader exited', r.status, r.stderr); fails++; return null; }
  return r.stdout || '';
}

// Compare the JS unpack() reference to the C reader's view of the same .bin.
function crossCheck(label, catalog) {
  const buf = pack(catalog);
  const bin = join(BUILD, 'irpack-' + label + '.bin');
  writeFileSync(bin, buf);
  const ref = unpack(buf);
  const out = runCReader(bin);
  if (out == null) return;
  const c = dumpToModel(out);

  check(c.hdr && c.hdr.n_remotes === ref.remotes.length, `${label}: remote count ${c.hdr?.n_remotes} != ${ref.remotes.length}`);
  check(c.hdr && c.hdr.n_tvpower === ref.tvpower.length, `${label}: tvpower count ${c.hdr?.n_tvpower} != ${ref.tvpower.length}`);
  check(c.oob && c.oob.rem === 0 && c.oob.tvp === 0, `${label}: out-of-range read not rejected`);

  ref.remotes.forEach((r, i) => {
    const x = c.remotes[i];
    check(x && x.name === r.name && x.proto === r.proto && x.region === r.region && x.addr === r.addr && x.nbtn === r.btns.length,
      `${label}: remote[${i}] mismatch C=${JSON.stringify(x && { ...x, btns: undefined })} JS=${JSON.stringify({ ...r, btns: undefined })}`);
    r.btns.forEach((b, j) => {
      const y = x && x.btns[j];
      check(y && y.key === b.key && y.cmd === b.cmd, `${label}: remote[${i}].btn[${j}] C=${JSON.stringify(y)} JS=${JSON.stringify(b)}`);
      if (b.raw) check(y && y.raw && y.raw.carrier === b.raw.carrier && eqArr(y.raw.dur, b.raw.dur),
        `${label}: remote[${i}].btn[${j}] RAW C=${JSON.stringify(y && y.raw)} JS=${JSON.stringify(b.raw)}`);
    });
  });
  ref.tvpower.forEach((t, i) => {
    const x = c.tvpower[i];
    check(x && x.brand === t.brand && x.proto === t.proto && x.region === t.region && x.addr === t.addr && x.cmd === t.cmd,
      `${label}: tvpower[${i}] C=${JSON.stringify(x)} JS=${JSON.stringify(t)}`);
    if (t.raw) check(x && x.raw && x.raw.carrier === t.raw.carrier && eqArr(x.raw.dur, t.raw.dur),
      `${label}: tvpower[${i}] RAW C=${JSON.stringify(x && x.raw)} JS=${JSON.stringify(t.raw)}`);
  });
  console.log(`  ${label}: ${ref.remotes.length} remotes / ${ref.tvpower.length} codes round-tripped (${buf.length} B)`);
}

// 1) synthetic catalog with edge cases: an over-long name (truncated to 27), an empty-button remote.
const synthetic = {
  remotes: [
    { name: 'A Remote Whose Name Is Way Too Long To Fit', protocol: 'nec', region: 'us', address: 4,
      buttons: { power: 8, vol_up: 2, vol_down: 3 } },
    { name: 'Empty', protocol: 'rc5', region: 'eu', address: 0, buttons: {} },
    { name: 'Sony', protocol: 'sony12', region: 'all', address: 1, buttons: { power: 21, '0': 9 } },
    // RAW remote (the universal path): durations + an explicit carrier, dedup across two identical buttons.
    { name: 'RAW Box', protocol: 'raw', region: 'all',
      buttons: { power: { carrier: 37000, raw: [3456, 1728, 432, 432, 432, 1296, 432, 432] },
                 again: { carrier: 37000, raw: [3456, 1728, 432, 432, 432, 1296, 432, 432] } } },
  ],
  tvpower: [
    { brand: 'Philips', region: 'eu', protocol: 'rc5', address: 0, command: 12 },
    { brand: 'A Brand With A Very Long Name', region: 'asia', protocol: 'nec', address: 65535, command: 65535 },
    // RAW tv-power row, with a long-ish unique sequence to stress the (addr<<16|cmd) offset packing.
    { brand: 'RAW TV', region: 'us', protocol: 'raw', carrier: 36000,
      raw: [2666, 889, 444, 444, 888, 888, 444, 444, 444, 444, 888, 444, 444, 888] },
  ],
};
crossCheck('synthetic', synthetic);

// 2) the real shipped catalog
const real = JSON.parse(readFileSync(join(ROOT, 'apps/ir-remote/www/presets.json'), 'utf8'));
crossCheck('presets', real);

// 3) sanity: every protocol/region in the real catalog is a known code (no silent 0 fallbacks)
for (const r of real.remotes) {
  check(PROTOS.includes(String(r.protocol).toLowerCase()), `unknown protocol "${r.protocol}" in remote ${r.id || r.name}`);
  check(!r.region || REGIONS.includes(String(r.region).toLowerCase()), `unknown region "${r.region}" in remote ${r.id || r.name}`);
}
for (const t of real.tvpower) {
  check(PROTOS.includes(String(t.protocol).toLowerCase()), `unknown protocol "${t.protocol}" in tvpower ${t.brand}`);
  check(REGIONS.includes(String(t.region).toLowerCase()), `unknown region "${t.region}" in tvpower ${t.brand}`);
}

// 4) open-fail path: a non-pack file must be rejected, not crash.
const junk = join(BUILD, 'irpack-junk.bin');
writeFileSync(junk, Buffer.from('not a pack file at all'));
check((runCReader(junk) || '').includes('OPEN_FAIL'), 'junk file must fail ir_pack_open');

console.log(fails === 0 ? '\nIR pack: ALL PASS' : `\nIR pack: ${fails} FAILURE(S)`);
process.exit(fails ? 1 : 0);
