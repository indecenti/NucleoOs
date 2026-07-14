// ir-pack — compile the human-editable IR catalog (apps/ir-remote/www/presets.json) into the
// fixed-width "IRPK" binary the firmware reads with fseek/fread (see nucleo_ir_pack.h). The browser
// keeps loading the JSON directly; only the RAM-starved device needs the pack. Run as part of the
// SD build so /sd/system/ir/presets.bin is always fresh.
//
//   node tools/ir-pack.mjs            # pack the canonical source into every SD target + sim
//   node tools/ir-pack.mjs in.json out.bin
//
// Exports pack()/unpack() so the host gate (irpack:test) can round-trip without a device.
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

// proto codes — KEEP IN SYNC with nir_proto_t (nucleo_ir_proto.h).
export const PROTOS = ['raw', 'nec', 'necext', 'samsung', 'sony12', 'sony15', 'sony20', 'rc5', 'jvc', 'panasonic'];
// region codes — KEEP IN SYNC with REGIONS in nucleo_ir_pack.c.
export const REGIONS = ['all', 'us', 'eu', 'asia'];

const HDR = 32, REM = 52, BTN = 16, TVP = 28;   // REM v3: name[28]+proto+region+nbtn[2]+addr[4]+data_off[4]+category[12]
const NAME_LEN = 28, KEY_LEN = 12, BRAND_LEN = 20, CAT_LEN = 12;

const protoCode = (n) => { const i = PROTOS.indexOf(String(n || '').toLowerCase()); return i < 0 ? 0 : i; };
const regionCode = (n) => { const i = REGIONS.indexOf(String(n || 'all').toLowerCase()); return i < 0 ? 0 : i; };

// write a NUL-padded, NUL-terminated ASCII field of `len` bytes
function writeStr(buf, off, s, len) {
  const b = Buffer.from(String(s ?? ''), 'utf8').subarray(0, len - 1);
  buf.fill(0, off, off + len);
  b.copy(buf, off);
}

const isRaw = (proto) => String(proto || '').toLowerCase() === 'raw';
const clampDur = (a) => (Array.isArray(a) ? a : []).map((x) => Math.max(0, Math.min(0xffff, x | 0)));

// presets.json object -> Buffer in IRPK v2 layout (RAW pool included). Throws on a structurally
// invalid catalog. RAW entries (any captured µs sequence) make the pack universal — any protocol.
export function pack(catalog) {
  const remotes = Array.isArray(catalog?.remotes) ? catalog.remotes : [];
  const tvpower = Array.isArray(catalog?.tvpower) ? catalog.tvpower : [];

  // RAW pool with dedup: identical (carrier, durations) share one entry.
  const rawPool = [], rawMap = new Map();
  const addRaw = (dur, carrier) => {
    const d = clampDur(dur), car = (carrier | 0) || 38000;
    const key = car + '|' + d.join(',');
    if (rawMap.has(key)) return rawMap.get(key);
    const e = { carrier: car, dur: d, off: 0 };
    rawPool.push(e); rawMap.set(key, e); return e;
  };

  // normalise remotes (a remote is uniformly RAW or protocol), then SORT by category so each category
  // occupies a contiguous index range — the device browses category -> remotes with plain seek arithmetic.
  const R = remotes.map((r) => {
    const cat = String(r.category || '').toLowerCase();
    if (isRaw(r.protocol)) {
      const btns = Object.entries(r.buttons || {}).map(([key, v]) => {
        const arr = Array.isArray(v) ? v : (v && v.raw) || [];
        return { key, rawEntry: addRaw(arr, (v && v.carrier) || r.carrier) };
      });
      return { name: r.name || r.id || '?', proto: 0, region: regionCode(r.region), addr: 0, btns, raw: true, cat };
    }
    const btns = Object.entries(r.buttons || {}).map(([key, cmd]) => ({ key, cmd: cmd >>> 0 }));
    return { name: r.name || r.id || '?', proto: protoCode(r.protocol), region: regionCode(r.region),
             addr: (r.address >>> 0) || 0, btns, raw: false, cat };
  });
  R.sort((a, b) => (a.cat < b.cat ? -1 : a.cat > b.cat ? 1 : 0));   // stable (ES2019+): keeps in-category order
  const T = tvpower.map((t) => {
    if (isRaw(t.protocol)) return { brand: t.brand || '?', region: regionCode(t.region), proto: 0,
             rawEntry: addRaw(t.raw, t.carrier), raw: true };
    return { brand: t.brand || '?', region: regionCode(t.region), proto: protoCode(t.protocol),
             addr: (t.address >>> 0) & 0xffff, cmd: (t.command >>> 0) & 0xffff, raw: false };
  });

  const remotesOff = HDR;
  const btnBase = remotesOff + R.length * REM;
  let totalBtn = 0;
  for (const r of R) { r.dataOff = btnBase + totalBtn * BTN; totalBtn += r.btns.length; }
  const tvpowerOff = btnBase + totalBtn * BTN;
  const rawBase = tvpowerOff + T.length * TVP;
  let cur = rawBase;
  for (const e of rawPool) { e.off = cur; cur += 4 + e.dur.length * 2; }   // u16 carrier + u16 count + dur
  const rawSize = cur - rawBase;
  const size = cur;

  const buf = Buffer.alloc(size);
  buf.write('IRPK', 0, 'ascii');
  buf.writeUInt16LE(3, 4);                 // version 3: remote record carries a category[12] tail
  buf.writeUInt16LE(0, 6);                 // flags
  buf.writeUInt32LE(R.length, 8);
  buf.writeUInt32LE(remotesOff, 12);
  buf.writeUInt32LE(T.length, 16);
  buf.writeUInt32LE(tvpowerOff, 20);
  buf.writeUInt32LE(rawPool.length ? rawBase : 0, 24);
  buf.writeUInt32LE(rawSize, 28);

  R.forEach((r, i) => {
    const o = remotesOff + i * REM;
    writeStr(buf, o, r.name, NAME_LEN);
    buf.writeUInt8(r.proto, o + 28);
    buf.writeUInt8(r.region, o + 29);
    buf.writeUInt16LE(r.btns.length, o + 30);
    buf.writeUInt32LE(r.addr, o + 32);
    buf.writeUInt32LE(r.dataOff, o + 36);
    writeStr(buf, o + 40, r.cat, CAT_LEN);        // v3 category tail
  });
  R.forEach((r) => r.btns.forEach((b, j) => {
    const o = r.dataOff + j * BTN;
    writeStr(buf, o, b.key, KEY_LEN);
    buf.writeUInt32LE(r.raw ? b.rawEntry.off : b.cmd, o + 12);   // RAW button: cmd field = raw offset
  }));
  T.forEach((t, i) => {
    const o = tvpowerOff + i * TVP;
    writeStr(buf, o, t.brand, BRAND_LEN);
    buf.writeUInt8(t.region, o + 20);
    buf.writeUInt8(t.proto, o + 21);
    if (t.raw) { buf.writeUInt16LE((t.rawEntry.off >>> 16) & 0xffff, o + 22); buf.writeUInt16LE(t.rawEntry.off & 0xffff, o + 24); }
    else       { buf.writeUInt16LE(t.addr, o + 22); buf.writeUInt16LE(t.cmd, o + 24); }
  });
  // RAW pool
  for (const e of rawPool) {
    buf.writeUInt16LE(e.carrier, e.off);
    buf.writeUInt16LE(e.dur.length, e.off + 2);
    e.dur.forEach((v, k) => buf.writeUInt16LE(v, e.off + 4 + k * 2));
  }
  return buf;
}

const cstr = (buf, off, len) => { const z = buf.indexOf(0, off); const e = z < 0 || z > off + len ? off + len : z; return buf.toString('utf8', off, e); };

// inverse of pack() — used by the round-trip test to assert byte-for-byte fidelity in JS.
export function unpack(buf) {
  if (buf.toString('ascii', 0, 4) !== 'IRPK') throw new Error('bad magic');
  const ver = buf.readUInt16LE(4);
  const nRem = buf.readUInt32LE(8), remOff = buf.readUInt32LE(12);
  const nTvp = buf.readUInt32LE(16), tvpOff = buf.readUInt32LE(20);
  const rawOff = ver >= 2 ? buf.readUInt32LE(24) : 0;
  const readRaw = (off) => {
    if (!off) return null;
    const carrier = buf.readUInt16LE(off), n = buf.readUInt16LE(off + 2), dur = [];
    for (let k = 0; k < n; k++) dur.push(buf.readUInt16LE(off + 4 + k * 2));
    return { carrier, dur };
  };
  const remotes = [];
  for (let i = 0; i < nRem; i++) {
    const o = remOff + i * REM;
    const proto = buf.readUInt8(o + 28), nbtn = buf.readUInt16LE(o + 30), dataOff = buf.readUInt32LE(o + 36);
    const btns = [];
    for (let j = 0; j < nbtn; j++) {
      const b = dataOff + j * BTN, v = buf.readUInt32LE(b + 12);
      const e = { key: cstr(buf, b, KEY_LEN), cmd: v };
      if (proto === 0) e.raw = readRaw(v);    // RAW remote: cmd field is the raw-pool offset
      btns.push(e);
    }
    remotes.push({ name: cstr(buf, o, NAME_LEN), proto, region: buf.readUInt8(o + 29),
                   addr: buf.readUInt32LE(o + 32), btns, category: ver >= 3 ? cstr(buf, o + 40, CAT_LEN) : '' });
  }
  const tvpower = [];
  for (let i = 0; i < nTvp; i++) {
    const o = tvpOff + i * TVP;
    const proto = buf.readUInt8(o + 21), addr = buf.readUInt16LE(o + 22), cmd = buf.readUInt16LE(o + 24);
    const e = { brand: cstr(buf, o, BRAND_LEN), region: buf.readUInt8(o + 20), proto, addr, cmd };
    if (proto === 0) e.raw = readRaw((addr << 16) | cmd);   // RAW row: offset = (addr<<16 | cmd)
    tvpower.push(e);
  }
  return { remotes, tvpower, rawOff };
}

// ---- CLI ------------------------------------------------------------------------------------
function main(argv) {
  const root = process.cwd();
  const src = argv[0] || join(root, 'apps/ir-remote/www/presets.json');
  const catalog = JSON.parse(readFileSync(src, 'utf8'));
  const buf = pack(catalog);

  let targets;
  if (argv[1]) {
    targets = [argv[1]];
  } else {
    // every SD tree the release/sim can serve from /sd/system/ir/presets.bin
    targets = [
      'deploy/sd/system/ir/presets.bin',
      'deploy/sd-master/system/ir/presets.bin',
      'tools/sd-sim/system/ir/presets.bin',
    ].map((p) => join(root, p));
  }
  for (const out of targets) {
    mkdirSync(dirname(out), { recursive: true });
    writeFileSync(out, buf);
  }
  const { remotes, tvpower } = unpack(buf);
  console.log(`ir-pack: ${remotes.length} remotes, ${tvpower.length} tv-power codes -> ${buf.length} bytes`);
  for (const out of targets) console.log('  wrote', out.replace(root + '\\', '').replace(root + '/', ''));
}

if (process.argv[1] && resolve(fileURLToPath(import.meta.url)) === resolve(process.argv[1])) {
  main(process.argv.slice(2));
}
