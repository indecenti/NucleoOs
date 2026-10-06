// QR encoder (apps/qr/www/qrcode.js) — round-trip regression test. The app draws whatever NucleoQR.generate
// returns; a wrong mask, format word, block interleave or Reed-Solomon table still draws a pretty square
// that NO phone can scan. So this test DECODES every symbol with an independent reader written from the
// QR spec (ISO/IEC 18004 / Nayuki's reference tables, not the encoder's own tables) and checks:
//   - the format + version words carry valid BCH codes and the ECC level that was asked for,
//   - every block passes the Reed-Solomon syndrome check,
//   - the payload decodes back to the exact text (numeric / alphanumeric / byte UTF-8 modes),
//   - known-answer data codewords for the two textbook symbols, and the spec's v40 capacity edge.
// Pure Node (no browser, no device) — runs on Linux CI.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

const SRC = readFileSync(new URL('../apps/qr/www/qrcode.js', import.meta.url), 'utf8');
function loadQR() {
  const sandbox = { TextEncoder };
  sandbox.window = sandbox;
  vm.runInNewContext(SRC, sandbox, { filename: 'qrcode.js' });
  return sandbox.NucleoQR;
}
const NucleoQR = loadQR();

// ---- independent decoder (versions 1..10) ----
// Error-correction codewords per block and number of blocks, indexed [ecl][version-1], ecl = L, M, Q, H.
const ECC_PER_BLOCK = [
  [7, 10, 15, 20, 26, 18, 20, 24, 30, 18],
  [10, 16, 26, 18, 24, 16, 18, 22, 22, 26],
  [13, 22, 18, 26, 18, 24, 18, 22, 20, 24],
  [17, 28, 22, 16, 22, 28, 26, 26, 24, 28],
];
const NUM_BLOCKS = [
  [1, 1, 1, 1, 1, 2, 2, 2, 2, 4],
  [1, 1, 1, 2, 2, 4, 4, 4, 5, 5],
  [1, 1, 2, 2, 4, 4, 6, 6, 8, 8],
  [1, 1, 2, 4, 4, 4, 5, 6, 8, 8],
];
const FORMAT_TO_ECL = { 1: 0, 0: 1, 3: 2, 2: 3 };   // the 2 format bits: L=01 M=00 Q=11 H=10
const ALNUM = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:';

const EXP = new Uint8Array(512), LOG = new Uint8Array(256);
for (let i = 0, x = 1; i < 255; i++) { EXP[i] = x; LOG[x] = i; x <<= 1; if (x & 0x100) x ^= 0x11d; }
for (let i = 255; i < 512; i++) EXP[i] = EXP[i - 255];
const gmul = (a, b) => (a && b ? EXP[LOG[a] + LOG[b]] : 0);

function alignmentPositions(ver) {
  if (ver === 1) return [];
  const n = Math.floor(ver / 7) + 2, size = ver * 4 + 17;
  const step = Math.floor((ver * 8 + n * 3 + 5) / (n * 4 - 4)) * 2;
  const out = [6];
  for (let pos = size - 7; out.length < n; pos -= step) out.splice(1, 0, pos);
  return out;
}
function rawDataModules(ver) {
  let r = (16 * ver + 128) * ver + 64;
  if (ver >= 2) { const n = Math.floor(ver / 7) + 2; r -= (25 * n - 10) * n - 55; if (ver >= 7) r -= 36; }
  return r;
}
const MASKS = [
  (x, y) => (x + y) % 2 === 0, (x, y) => y % 2 === 0, (x) => x % 3 === 0, (x, y) => (x + y) % 3 === 0,
  (x, y) => (Math.floor(x / 3) + Math.floor(y / 2)) % 2 === 0, (x, y) => (x * y) % 2 + (x * y) % 3 === 0,
  (x, y) => ((x * y) % 2 + (x * y) % 3) % 2 === 0, (x, y) => ((x + y) % 2 + (x * y) % 3) % 2 === 0,
];
function formatWord(fmt, mask) {
  const data = (fmt << 3) | mask; let rem = data;
  for (let i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >>> 9) * 0x537);
  return ((data << 10) | (rem & 0x3ff)) ^ 0x5412;
}
function versionWord(ver) {
  let rem = ver;
  for (let i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >>> 11) * 0x1f25);
  return (ver << 12) | (rem & 0xfff);
}

// get(x, y): x = column, y = row (the convention of the spec and of the app's drawQR()).
function decode(get, size) {
  const ver = (size - 17) / 4;
  assert.ok(Number.isInteger(ver) && ver >= 1 && ver <= 10, 'decoder handles versions 1..10, got size ' + size);
  const bit = (x, y) => (get(x, y) ? 1 : 0);

  // format word, both copies
  let f1 = 0, f2 = 0;
  for (let i = 0; i <= 5; i++) f1 |= bit(8, i) << i;
  f1 |= bit(8, 7) << 6; f1 |= bit(8, 8) << 7; f1 |= bit(7, 8) << 8;
  for (let i = 9; i < 15; i++) f1 |= bit(14 - i, 8) << i;
  for (let i = 0; i < 8; i++) f2 |= bit(size - 1 - i, 8) << i;
  for (let i = 8; i < 15; i++) f2 |= bit(8, size - 15 + i) << i;
  assert.equal(f1, f2, 'the two format-word copies disagree');
  let fmt = -1, mask = -1;
  for (let f = 0; f < 4; f++) for (let m = 0; m < 8; m++) if (formatWord(f, m) === f1) { fmt = f; mask = m; }
  assert.ok(fmt >= 0, 'format word is not a valid BCH codeword: ' + f1.toString(2));
  assert.equal(bit(8, size - 8), 1, 'dark module missing');
  const ecl = FORMAT_TO_ECL[fmt];

  if (ver >= 7) {
    let v1 = 0, v2 = 0;
    for (let i = 0; i < 18; i++) { v1 |= bit(size - 11 + (i % 3), Math.floor(i / 3)) << i; v2 |= bit(Math.floor(i / 3), size - 11 + (i % 3)) << i; }
    assert.equal(v1, versionWord(ver), 'version word (top-right) wrong');
    assert.equal(v2, versionWord(ver), 'version word (bottom-left) wrong');
  }

  // function-pattern map
  const fn = Array.from({ length: size }, () => new Uint8Array(size));
  const mark = (x0, y0, w, h) => { for (let y = y0; y < y0 + h; y++) for (let x = x0; x < x0 + w; x++) if (x >= 0 && y >= 0 && x < size && y < size) fn[y][x] = 1; };
  mark(0, 0, 9, 9); mark(size - 8, 0, 8, 9); mark(0, size - 8, 9, 8);
  mark(6, 0, 1, size); mark(0, 6, size, 1);
  const al = alignmentPositions(ver), last = al.length - 1;
  for (let i = 0; i < al.length; i++) for (let j = 0; j < al.length; j++) {
    if ((i === 0 && j === 0) || (i === 0 && j === last) || (i === last && j === 0)) continue;
    mark(al[i] - 2, al[j] - 2, 5, 5);
  }
  if (ver >= 7) { mark(size - 11, 0, 3, 6); mark(0, size - 11, 6, 3); }

  // zig-zag read, unmasked
  const bits = [];
  for (let right = size - 1; right >= 1; right -= 2) {
    if (right === 6) right = 5;
    for (let vert = 0; vert < size; vert++) for (let j = 0; j < 2; j++) {
      const x = right - j, up = ((right + 1) & 2) === 0, y = up ? size - 1 - vert : vert;
      if (!fn[y][x]) bits.push(bit(x, y) ^ (MASKS[mask](x, y) ? 1 : 0));
    }
  }
  assert.equal(bits.length, rawDataModules(ver), 'data-module count wrong (function patterns misplaced?)');
  const raw = [];
  for (let i = 0; i + 8 <= bits.length; i += 8) raw.push(bits.slice(i, i + 8).reduce((a, b) => (a << 1) | b, 0));

  // de-interleave + Reed-Solomon check
  const nb = NUM_BLOCKS[ecl][ver - 1], ecLen = ECC_PER_BLOCK[ecl][ver - 1];
  const numShort = nb - (raw.length % nb), shortLen = Math.floor(raw.length / nb);
  // Interleave order: every block is padded to shortLen + 1; a short block's pad sits at the end of its data part.
  const padded = Array.from({ length: nb }, () => new Array(shortLen + 1));
  let p = 0;
  for (let i = 0; i <= shortLen; i++) for (let k = 0; k < nb; k++) {
    if (k < numShort && i === shortLen - ecLen) continue;
    padded[k][i] = raw[p++];
  }
  const blocks = padded.map((b, k) => (k < numShort ? b.filter((_, i) => i !== shortLen - ecLen) : b));
  assert.equal(p, raw.length, 'interleave consumed the wrong number of codewords');
  const data = [];
  blocks.forEach((b, k) => {
    for (let r = 0; r < ecLen; r++) {
      let s = 0; for (const c of b) s = gmul(s, EXP[r]) ^ c;
      assert.equal(s, 0, `Reed-Solomon syndrome ${r} of block ${k} is not zero`);
    }
    data.push(...b.slice(0, b.length - ecLen));
  });

  // bit-stream → text
  let pos = 0;
  const dbits = data.flatMap((c) => [7, 6, 5, 4, 3, 2, 1, 0].map((s) => (c >> s) & 1));
  const read = (n) => { let v = 0; for (let i = 0; i < n; i++) v = (v << 1) | dbits[pos++]; return v; };
  const bytes = [], modes = [];             // character-count widths: versions 1-9 vs 10-26
  while (dbits.length - pos >= 4) {
    const mode = read(4);
    if (mode === 0) break;
    if (mode === 1) {
      modes.push('numeric'); let n = read(ver <= 9 ? 10 : 12);
      for (; n >= 3; n -= 3) bytes.push(...String(read(10)).padStart(3, '0').split('').map((c) => c.charCodeAt(0)));
      if (n === 2) bytes.push(...String(read(7)).padStart(2, '0').split('').map((c) => c.charCodeAt(0)));
      if (n === 1) bytes.push(String(read(4)).charCodeAt(0));
    } else if (mode === 2) {
      modes.push('alnum'); let n = read(ver <= 9 ? 9 : 11);
      for (; n >= 2; n -= 2) { const v = read(11); bytes.push(ALNUM.charCodeAt(Math.floor(v / 45)), ALNUM.charCodeAt(v % 45)); }
      if (n === 1) bytes.push(ALNUM.charCodeAt(read(6)));
    } else if (mode === 4) {
      modes.push('byte'); const n = read(ver <= 9 ? 8 : 16);
      for (let i = 0; i < n; i++) bytes.push(read(8));
    } else assert.fail('unexpected segment mode ' + mode);
  }
  return { text: new TextDecoder('utf-8', { fatal: true }).decode(new Uint8Array(bytes)), ecl, mask, version: ver, modes, data };
}
const decodeQR = (qr) => decode((x, y) => qr.get(x, y), qr.size);

test('qr: textbook symbols carry the exact data codewords of the spec', () => {
  // ISO/IEC 18004 Annex I: "01234567" at 1-M.
  const num = NucleoQR.generate('01234567', { ecc: NucleoQR.ECC.MEDIUM });
  const a = decodeQR(num);
  assert.equal(a.version, 1); assert.equal(a.ecl, 1);
  assert.deepEqual(a.modes, ['numeric']);
  assert.deepEqual(a.data, [16, 32, 12, 86, 97, 128, 236, 17, 236, 17, 236, 17, 236, 17, 236, 17]);
  // "HELLO WORLD" at 1-Q (the classic alphanumeric walk-through).
  const hw = decodeQR(NucleoQR.generate('HELLO WORLD', { ecc: NucleoQR.ECC.QUARTILE }));
  assert.equal(hw.version, 1); assert.equal(hw.ecl, 2);
  assert.deepEqual(hw.modes, ['alnum']);
  assert.deepEqual(hw.data, [32, 91, 11, 120, 209, 114, 220, 77, 67, 64, 236, 17, 236]);
  assert.equal(hw.text, 'HELLO WORLD');
});

test('qr: what the app encodes decodes back to the same text, at every ECC level', () => {
  const samples = [
    'https://nucleo.os',
    'WIFI:T:WPA;S:My\\;Net;P:p\\:w\\"d;;',
    'BEGIN:VCARD\r\nVERSION:3.0\r\nN:Rossi\\, Anna;;;;\r\nFN:Rossi\\, Anna\r\nEND:VCARD',
    'mailto:anna@example.org?subject=Ciao%20Anna',
    'Caffè ☕ — €5, grazie!',
    '3141592653589793238462643383279',
    'tel:+390612345678',
    'x',
  ];
  for (const s of samples) {
    let prev = 0;
    for (const ecc of [0, 1, 2, 3]) {
      const qr = NucleoQR.generate(s, { ecc });
      assert.ok(qr, `generate(${JSON.stringify(s)}, ecc ${ecc}) returned null`);
      assert.equal(qr.size, qr.version * 4 + 17, 'size must match the version');
      const d = decodeQR(qr);
      assert.equal(d.text, s, `round trip at ecc ${ecc}`);
      assert.equal(d.ecl, ecc, 'the symbol must carry the ECC level that was asked for');
      assert.equal(d.mask, qr.mask, 'reported mask must be the one in the format word');
      assert.ok(qr.version >= prev, 'stronger ECC never needs a SMALLER symbol');
      prev = qr.version;
    }
  }
  // Mode selection keeps symbols small: digits → numeric, upper-case → alphanumeric, else byte.
  assert.deepEqual(decodeQR(NucleoQR.generate('3141592653589793238462643383279')).modes, ['numeric']);
  assert.deepEqual(decodeQR(NucleoQR.generate('HTTPS://NUCLEO.OS/A')).modes, ['alnum']);
  assert.deepEqual(decodeQR(NucleoQR.generate('https://nucleo.os')).modes, ['byte']);
});

test('qr: version grows with the payload, versions 7+ carry a valid version word', () => {
  const s = 'NucleoOS on the Cardputer: '.repeat(4);               // 108 bytes
  const lo = NucleoQR.generate(s, { ecc: 0 }), hi = NucleoQR.generate(s, { ecc: 3 });
  assert.ok(hi.version > lo.version, `H (${hi.version}) must need a bigger symbol than L (${lo.version})`);
  assert.ok(hi.version >= 7, 'this payload at H exercises the version-info area');
  assert.equal(decodeQR(hi).text, s);
  assert.equal(decodeQR(lo).text, s);
});

test('qr: capacity edge — the spec maximum fits, one byte more is refused (null, never a broken symbol)', () => {
  const max = NucleoQR.generate('x'.repeat(2953), { ecc: NucleoQR.ECC.LOW });   // v40-L byte capacity
  assert.ok(max, '2953 bytes fit in a version-40 L symbol');
  assert.equal(max.version, 40);
  assert.equal(max.size, 177);
  assert.equal(NucleoQR.generate('x'.repeat(2954), { ecc: NucleoQR.ECC.LOW }), null);
  assert.equal(NucleoQR.generate('x'.repeat(1300), { ecc: NucleoQR.ECC.HIGH }), null, 'v40-H holds 1273 bytes');
});

test('qr: the encoder is deterministic and get() is bounded', () => {
  const a = NucleoQR.generate('determinism'), b = NucleoQR.generate('determinism');
  for (let y = 0; y < a.size; y++) for (let x = 0; x < a.size; x++) assert.equal(a.get(x, y), b.get(x, y));
  assert.equal(a.get(-1, 0), false); assert.equal(a.get(a.size, 0), false); assert.equal(a.get(0, a.size), false);
});

