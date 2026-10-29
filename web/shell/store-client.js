/* SPDX-License-Identifier: AGPL-3.0-or-later */
// store-client.js — the NucleoOS store, one for every edition: catalogs, signatures, what this device can run,
// install. The Store app is its face; this module is the part that must be right.
//
// Sources (SOURCES below) are separate signed catalogs; a source is shown only on a device that has the
// capabilities it needs, and every app inside it only where the device has the capabilities the app needs
// (`requires`, from its caps.json). So a web app that needs the Cardputer's microphone is shown on a Cardputer
// and nowhere else, a P4 native app only on a P4, and an app that needs nothing on every edition.
//
// Web apps install from the browser: the catalog and the package are verified here (ECDSA P-256 over the
// exact bytes, the same keys the Cardputer firmware has compiled in), every file is checked against its
// SHA-256 and size, written next to the installed version and swapped in only when all of it matched. The
// device then serves the app at /apps/<id>/ (capability apps.install, folder paths.apps). Native apps are
// installed by the device's own store: here they are shown, with where to install them, until the firmware
// offers an install route.
//
// Pages served over plain http on the LAN are not a secure context: crypto.subtle does not exist there. The
// signature check then runs in plain JavaScript (p256Verify below), the SHA-256 in ./sha256.js.

import { CORE } from './nucleo-version.js';
import { sha256Hex } from './sha256.js';

// Public keys of the store (DER SubjectPublicKeyInfo, base64), by key id = first 16 hex of SHA-256(DER).
// Main and backup: either signs. Same values as nucleoos-cardputer-store/keys/ and the Cardputer firmware.
export const STORE_KEYS = {
  '7144a7c1dde55968': 'MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE/gmWlgMb9bXRwmsx4BEOSeHAnJgq6HkX9eGXLwwrv4lzYsfEATeAha2J8PIpLIjZJhJw4QSWZwOX74jOL5I+kw==',
  'f1b47dac0ff05dd4': 'MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAErepWzkSvWX1wxa6wsthmlsb+Y77GfMyibYPeb3JH14tZTgkrKE81ZuQZ2JMlhypTMNM+z2pB+I2IAV/W5ArtRg==',
};

const CARDPUTER_STORE = 'https://indecenti.github.io/nucleoos-cardputer-store/';
// needs: capabilities to SHOW the source; install: 'browser' (this module, needs apps.install) or 'device'
// (the device's own store; `where` names it for the user).
export const SOURCES = [
  { id: 'web', kind: 'cardputer-v2', channel: 'web', base: CARDPUTER_STORE + 'web/', needs: [], install: 'browser' },
  { id: 'nap', kind: 'cardputer-v2', channel: 'native', base: CARDPUTER_STORE + 'native/', needs: ['apps.nap'], install: 'device', where: 'cardputer' },
  { id: 'p4', kind: 'p4-json', base: 'https://indecenti.github.io/nucleoos-p4-store/', needs: ['apps.p4'], install: 'device', where: 'p4' },
];

export const LANGS = ['it', 'en', 'es', 'fr', 'de'];
const INDEX_MAGIC = 'nucleoos-cardputer-index-v2';
const SIG_MAGIC = 'nucleoos-cardputer-sig-v1';
const PKG_MAGIC = 'nucleoos-cardputer-pkg-v1';
const ID_RE = /^[a-z][a-z0-9-]{1,31}$/;
const PATH_SEG = /^[A-Za-z0-9_-][A-Za-z0-9._-]*$/;
export const LIMITS = { files: 64, file: 4 * 1024 * 1024, package: 8 * 1024 * 1024 };

export class StoreError extends Error {
  constructor(code, detail) { super(code + (detail ? ': ' + detail : '')); this.code = code; this.detail = detail || ''; }
}

const enc = new TextEncoder();
const dec = new TextDecoder();
const b64 = (s) => Uint8Array.from(atob(s), (c) => c.charCodeAt(0));

// ---------------------------------------------------------------- catalogs

// The Cardputer store's text catalog (nucleoos-cardputer-store docs/format.md). Unknown kinds and extra fields
// are skipped, as the format requires; `requires` is field 16 ('-' or absent = nothing).
export function parseIndex(text) {
  const rows = String(text).replace(/\n$/, '').split('\n');
  if (rows[0] !== INDEX_MAGIC) throw new StoreError('bad_catalog', 'magic');
  const head = {};
  for (const [i, k] of [[1, 'channel'], [2, 'serial'], [3, 'expires'], [4, 'apps']]) {
    const m = (rows[i] || '').match(new RegExp('^' + k + ' (\\S+)$'));
    if (!m) throw new StoreError('bad_catalog', k);
    head[k] = k === 'channel' ? m[1] : Number(m[1]);
  }
  const apps = [];
  for (const row of rows.slice(5)) {
    const f = row.split('\t');
    if (f[0] !== 'app' || f.length < 15) throw new StoreError('bad_catalog', row.slice(0, 40));
    if (!ID_RE.test(f[1])) throw new StoreError('bad_catalog', 'id ' + f[1]);
    const five = (v) => { const p = v.split('|'); return Object.fromEntries(LANGS.map((l, i) => [l, p[i] || p[1] || p[0] || ''])); };
    apps.push({
      id: f[1], kind: f[2], version_code: Number(f[3]), version: f[4], min_os: f[5], category: f[6],
      size: Number(f[7]), sig_sha256: f[8], permissions: f[9] === '-' ? [] : f[9].split(','),
      names: five(f[10]), descriptions: five(f[11]), updated: Number(f[12]), author: f[13],
      abi: f[14] === '-' ? null : Number(f[14]),
      requires: !f[15] || f[15] === '-' ? [] : f[15].split(','),
    });
  }
  if (apps.length !== head.apps) throw new StoreError('bad_catalog', 'count');
  return { ...head, apps };
}

// A P4 store catalog (store2-<lang>.json): shown, not installed from here. Its packages are signed for the
// P4 firmware, which verifies them itself when it installs.
export function parseP4(json) {
  const apps = Array.isArray(json && json.apps) ? json.apps : [];
  return apps.filter((a) => a && typeof a.id === 'string').map((a) => ({
    id: a.id, kind: 'p4', version: String(a.version || ''), category: a.category_name || a.category || '',
    names: Object.fromEntries(LANGS.map((l) => [l, a.name || a.id])),
    descriptions: Object.fromEntries(LANGS.map((l) => [l, a.description || ''])),
    author: a.author || '', size: Number(a.size) || 0, requires: [], engine: a.engine || '',
  }));
}

// `index.sig` / the tail of `package.sig`: "key <id>\nsig <base64 DER>".
export function parseSigBlock(text) {
  const m = String(text).match(/(?:^|\n)key ([0-9a-f]{16})\nsig ([A-Za-z0-9+/=]+)\n?$/);
  if (!m) throw new StoreError('bad_signature', 'format');
  return { key: m[1], sig: b64(m[2]) };
}

// Does this device have everything the app (or source) needs?
export function fits(needs, caps) {
  const have = caps instanceof Set ? caps : new Set(caps || []);
  return (needs || []).every((c) => have.has(c));
}

// ---------------------------------------------------------------- signatures

// ECDSA P-256 over SHA-256 of `msg` (bytes), `der` the DER signature, `spki` the public key (DER bytes).
export async function verifySignature(spki, msg, der, { subtle = globalThis.crypto && globalThis.crypto.subtle } = {}) {
  const { r, s } = derToRS(der);
  if (subtle) {
    try {
      const key = await subtle.importKey('spki', spki, { name: 'ECDSA', namedCurve: 'P-256' }, false, ['verify']);
      const raw = new Uint8Array(64);
      raw.set(toBytes32(r), 0); raw.set(toBytes32(s), 32);
      return await subtle.verify({ name: 'ECDSA', hash: 'SHA-256' }, key, raw, msg);
    } catch { /* fall through to the plain-JavaScript check */ }
  }
  const pub = spki.slice(-65);
  if (pub[0] !== 4) return false;
  const qx = bytesToBig(pub.slice(1, 33)), qy = bytesToBig(pub.slice(33, 65));
  return p256Verify(qx, qy, BigInt('0x' + sha256Hex(msg)), r, s);
}

function derToRS(der) {
  // SEQUENCE { INTEGER r, INTEGER s }, short lengths only (a P-256 signature is at most 72 bytes)
  if (der[0] !== 0x30 || der[1] !== der.length - 2) throw new StoreError('bad_signature', 'der');
  let i = 2;
  const int = () => {
    if (der[i] !== 0x02) throw new StoreError('bad_signature', 'der');
    const len = der[i + 1]; const v = der.slice(i + 2, i + 2 + len); i += 2 + len;
    return bytesToBig(v);
  };
  const r = int(), s = int();
  if (i !== der.length) throw new StoreError('bad_signature', 'der');
  return { r, s };
}
const bytesToBig = (b) => b.reduce((acc, x) => (acc << 8n) | BigInt(x), 0n);
function toBytes32(v) { const out = new Uint8Array(32); for (let i = 31; i >= 0; i--) { out[i] = Number(v & 0xffn); v >>= 8n; } return out; }

// ---- P-256 (FIPS 186-4, curve P-256), verification only: public data, no secret ever passes through here.
const P = 0xffffffff00000001000000000000000000000000ffffffffffffffffffffffffn;
const N = 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551n;
const A = P - 3n;
const B = 0x5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604bn;
const G = [0x6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296n,
  0x4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5n];
const mod = (a, m) => { const r = a % m; return r < 0n ? r + m : r; };
function inv(a, m) {
  let [r0, r1, t0, t1] = [mod(a, m), m, 1n, 0n];
  while (r1) { const q = r0 / r1; [r0, r1] = [r1, r0 - q * r1]; [t0, t1] = [t1, t0 - q * t1]; }
  if (r0 !== 1n) throw new StoreError('bad_signature', 'not invertible');
  return mod(t0, m);
}
// Jacobian points [X, Y, Z]; Z = 0 is the point at infinity.
function dbl([X, Y, Z]) {
  if (!Y || !Z) return [0n, 1n, 0n];
  const YY = mod(Y * Y, P), S = mod(4n * X * YY, P), ZZ = mod(Z * Z, P);
  const M = mod(3n * X * X + A * ZZ * ZZ, P);
  const X3 = mod(M * M - 2n * S, P);
  return [X3, mod(M * (S - X3) - 8n * YY * YY, P), mod(2n * Y * Z, P)];
}
function add(p1, p2) {
  if (!p1[2]) return p2;
  if (!p2[2]) return p1;
  const [X1, Y1, Z1] = p1, [X2, Y2, Z2] = p2;
  const Z1Z1 = mod(Z1 * Z1, P), Z2Z2 = mod(Z2 * Z2, P);
  const U1 = mod(X1 * Z2Z2, P), U2 = mod(X2 * Z1Z1, P);
  const S1 = mod(Y1 * Z2 * Z2Z2, P), S2 = mod(Y2 * Z1 * Z1Z1, P);
  if (U1 === U2) return S1 === S2 ? dbl(p1) : [0n, 1n, 0n];
  const H = mod(U2 - U1, P), R = mod(S2 - S1, P), HH = mod(H * H, P), HHH = mod(H * HH, P);
  const X3 = mod(R * R - HHH - 2n * U1 * HH, P);
  return [X3, mod(R * (U1 * HH - X3) - S1 * HHH, P), mod(H * Z1 * Z2, P)];
}
function onCurve(x, y) { return x >= 0n && x < P && y >= 0n && y < P && mod(y * y - (x * x * x + A * x + B), P) === 0n; }

export function p256Verify(qx, qy, e, r, s) {
  if (r <= 0n || r >= N || s <= 0n || s >= N || !onCurve(qx, qy)) return false;
  const w = inv(s, N), u1 = mod(e * w, N), u2 = mod(r * w, N);
  // Shamir's trick: u1*G + u2*Q in one pass
  const g = [G[0], G[1], 1n], q = [qx, qy, 1n], gq = add(g, q);
  let acc = [0n, 1n, 0n];
  for (let bit = 255n; bit >= 0n; bit--) {
    acc = dbl(acc);
    const b1 = (u1 >> bit) & 1n, b2 = (u2 >> bit) & 1n;
    if (b1 && b2) acc = add(acc, gq); else if (b1) acc = add(acc, g); else if (b2) acc = add(acc, q);
  }
  if (!acc[2]) return false;
  const zi = inv(acc[2], P);
  return mod(mod(acc[0] * zi * zi, P), N) === r;
}

async function checkSigned(text, block, what, keys = STORE_KEYS) {
  const { key, sig } = parseSigBlock(block);
  const spki = keys[key];
  if (!spki) throw new StoreError('unknown_key', what + ' ' + key);
  if (!(await verifySignature(b64(spki), enc.encode(text), sig))) throw new StoreError('bad_signature', what);
}

// ---------------------------------------------------------------- packages

export function parsePackage(text) {
  const [body, block] = String(text).split('\n--\n');
  if (block === undefined) throw new StoreError('bad_package', 'format');
  const lines = body.split('\n');
  if (lines[0] !== PKG_MAGIC) throw new StoreError('bad_package', 'magic');
  const files = [];
  let total = 0, prev = '';
  for (const line of lines.slice(4)) {
    if (!line) continue;
    const m = line.match(/^([0-9a-f]{64}) (\d+) (\S+)$/);
    if (!m) throw new StoreError('bad_package', 'line');
    const path = m[3], segs = path.split('/');
    if (segs.length > 4 || !segs.every((s) => PATH_SEG.test(s) && s !== '..') || path <= prev) throw new StoreError('bad_package', 'path ' + path);
    prev = path;
    const size = Number(m[2]);
    if (size > LIMITS.file) throw new StoreError('bad_package', 'file too big');
    total += size;
    files.push({ sha256: m[1], size, path });
  }
  if (files.length > LIMITS.files || total > LIMITS.package) throw new StoreError('bad_package', 'too big');
  return { id: lines[1], version_code: Number(lines[2]), kind: lines[3], files, body: body + '\n', block };
}

// ---------------------------------------------------------------- the store

// The Cardputer store seen through a same-origin proxy (caps paths.store, the cloud's <base>store/): same
// catalogs, same signatures, only the address changes. Anything but an absolute local path is ignored.
export function viaStore(list, storeBase) {
  if (typeof storeBase !== 'string' || !/^\/(?!\/)[A-Za-z0-9._~\/-]*$/.test(storeBase) || /(^|\/)\.\.(\/|$)/.test(storeBase)) return list;   // never //host
  const root = storeBase.replace(/\/*$/, '/');             // nucleo-caps hands paths over without the trailing slash
  return list.map((s) => (s.base.startsWith(CARDPUTER_STORE) ? { ...s, base: root + s.base.slice(CARDPUTER_STORE.length) } : s));
}

// deps: fetch (network, for the store), dev (the device: readText(path) -> string|null, writeBytes(path, bytes),
// mkdir(path), move(from, to), remove(path)), caps (array), appsDir (paths.apps), storeBase (paths.store), now (ms).
export function makeStore({ fetch = globalThis.fetch, dev, caps = [], appsDir = '', storeBase = '', now = () => Date.now(), keys = STORE_KEYS, sourceList = SOURCES } = {}) {
  sourceList = viaStore(sourceList, storeBase);
  // the device's capabilities, and the web OS's own core level (core.1 … core.N): a store app that needs a newer
  // core than this one is not offered. A device cannot claim a core level.
  const have = new Set([...caps.filter((c) => !/^core\.\d+$/.test(c)), ...Array.from({ length: CORE }, (_, i) => 'core.' + (i + 1))]);
  const canInstall = have.has('apps.install') && !!appsDir;
  const statePath = (src) => '/system/store/' + src + '.json';

  async function getText(url) {
    const r = await fetch(url, { cache: 'no-store', credentials: 'same-origin' });
    if (!r.ok) throw new StoreError('network', r.status + ' ' + url);
    return r.text();
  }
  async function getBytes(url) {
    const r = await fetch(url, { cache: 'no-store', credentials: 'same-origin' });
    if (!r.ok) throw new StoreError('network', r.status + ' ' + url);
    return new Uint8Array(await r.arrayBuffer());
  }

  // The sources this device can see.
  function sources() {
    return sourceList.filter((s) => fits(s.needs, have)).map((s) => ({ ...s, installable: s.install === 'browser' ? canInstall : false }));
  }

  async function installedCode(id) {
    if (!appsDir) return 0;
    const t = await dev.readText(appsDir + '/' + id + '/.package.sig').catch(() => null);
    if (!t) return 0;
    try { return parsePackage(t).version_code; } catch { return 0; }
  }

  // A source's catalog: verified, rollback-checked, and only the apps this device can run.
  async function catalog(sourceId, { lang = 'it' } = {}) {
    const src = sources().find((s) => s.id === sourceId);
    if (!src) throw new StoreError('not_on_this_device', sourceId);
    if (src.kind === 'p4-json') {
      const l = LANGS.includes(lang) ? lang : 'en';
      const apps = parseP4(JSON.parse(await getText(src.base + 'store2-' + l + '.json')));
      return { source: src, apps: apps.map((a) => ({ ...a, installed: 0, update: false })) };
    }
    const [text, sig] = await Promise.all([getText(src.base + 'index.txt'), getText(src.base + 'index.sig')]);
    if (!sig.startsWith(SIG_MAGIC + '\n')) throw new StoreError('bad_signature', 'catalog magic');
    await checkSigned(text, sig.slice(SIG_MAGIC.length + 1), 'catalog', keys);
    const idx = parseIndex(text);
    if (idx.channel !== src.channel) throw new StoreError('bad_catalog', 'channel');      // one store's catalog served as the other
    if (idx.expires * 1000 < now() && now() > Date.UTC(2026, 0, 1)) throw new StoreError('expired');   // only with a believable clock
    // Rollback: never accept a lower serial than the highest this device has seen (kept ON the device).
    const st = await dev.readText(statePath(src.id)).then((t) => (t ? JSON.parse(t) : {})).catch(() => ({}));
    if (Number(st.serial || 0) > idx.serial) throw new StoreError('rollback', st.serial + ' > ' + idx.serial);
    if (Number(st.serial || 0) < idx.serial) {
      // The folder may not exist yet, and a file API that does not create parents (the Cardputer's) would
      // refuse the write: the rollback floor would then never be kept.
      await dev.mkdir('/system/store').catch(() => {});
      await dev.writeBytes(statePath(src.id), enc.encode(JSON.stringify({ serial: idx.serial }))).catch(() => {});
    }
    const kinds = src.channel === 'web' ? ['web'] : ['native'];
    const apps = [];
    for (const a of idx.apps) {
      if (!kinds.includes(a.kind) || !fits(a.requires, have)) continue;
      const installed = src.install === 'browser' ? await installedCode(a.id) : 0;
      apps.push({ ...a, installed, update: installed > 0 && a.version_code > installed });
    }
    return { source: src, serial: idx.serial, apps };
  }

  // Install (or update) one web app from a verified catalog line. onStep({ step, done, total }).
  async function install(sourceId, app, onStep = () => {}) {
    const src = sources().find((s) => s.id === sourceId);
    if (!src || src.install !== 'browser') throw new StoreError('install_on_device', src ? src.where : sourceId);
    if (!canInstall) throw new StoreError('install_unsupported');
    if (!fits(app.requires, have)) throw new StoreError('not_on_this_device', app.id);
    const pkgBase = src.base + 'apps/' + app.id + '/' + app.version_code + '/';
    onStep({ step: 'package' });
    const pkgBytes = await getBytes(pkgBase + 'package.sig');
    if (sha256Hex(pkgBytes) !== app.sig_sha256) throw new StoreError('bad_package', 'not the one the catalog names');
    const pkgText = dec.decode(pkgBytes);
    const pkg = parsePackage(pkgText);
    await checkSigned(pkg.body, pkg.block, 'package', keys);
    if (pkg.id !== app.id || pkg.version_code !== app.version_code || pkg.kind !== app.kind) throw new StoreError('bad_package', 'id / version / kind');
    const tmp = appsDir + '/.tmp/' + app.id, dest = appsDir + '/' + app.id, old = appsDir + '/.old/' + app.id;
    const files = [];
    let done = 0;
    onStep({ step: 'download', done: 0, total: pkg.files.length });
    for (const f of pkg.files) {
      const bytes = await getBytes(pkgBase + f.path);
      if (bytes.length !== f.size || sha256Hex(bytes) !== f.sha256) throw new StoreError('bad_file', f.path);
      files.push([f.path, bytes]);
      onStep({ step: 'download', done: ++done, total: pkg.files.length, path: f.path });
    }
    // Every byte checked: only now does anything reach the device, beside the installed version.
    onStep({ step: 'write', done: 0, total: files.length });
    const dirs = new Set();
    for (const [path] of files) { const parts = path.split('/'); for (let i = 1; i < parts.length; i++) dirs.add(parts.slice(0, i).join('/')); }
    await dev.mkdir(appsDir).catch(() => {});                // first install: the folder may not exist yet
    await dev.mkdir(appsDir + '/.tmp').catch(() => {});
    await removeTree(tmp);
    await dev.mkdir(tmp);
    for (const d of [...dirs].sort()) await dev.mkdir(tmp + '/' + d);
    let w = 0;
    for (const [path, bytes] of files) { await dev.writeBytes(tmp + '/' + path, bytes); onStep({ step: 'write', done: ++w, total: files.length }); }
    await dev.writeBytes(tmp + '/.package.sig', pkgBytes);
    // The store's category, from the signed catalog line, beside the app: Start files it under the same category the
    // store shows (shell/app-categories.js), even when the app's own manifest names none.
    await dev.writeBytes(tmp + '/.store.json', enc.encode(JSON.stringify({ category: String(app.category || '') })));
    onStep({ step: 'swap' });
    const had = (await installedCode(app.id)) > 0;
    if (had) { await dev.mkdir(appsDir + '/.old').catch(() => {}); await removeTree(old); await dev.move(dest, old); }
    await dev.move(tmp, dest);
    if (had) await removeTree(old);
    return { id: app.id, version_code: app.version_code, files: files.length };
  }

  // Remove an app this store installed: its signed file list says exactly what is there.
  async function removeTree(dir) {
    const t = await dev.readText(dir + '/.package.sig').catch(() => null);
    let pkg = null;
    try { pkg = t ? parsePackage(t) : null; } catch { pkg = null; }
    if (pkg) {
      for (const f of pkg.files) await dev.remove(dir + '/' + f.path).catch(() => {});
      await dev.remove(dir + '/.package.sig').catch(() => {});
      await dev.remove(dir + '/.store.json').catch(() => {});
      const dirs = new Set();
      for (const f of pkg.files) { const parts = f.path.split('/'); for (let i = parts.length - 1; i > 0; i--) dirs.add(parts.slice(0, i).join('/')); }
      for (const d of [...dirs].sort((a, b) => b.length - a.length)) await dev.remove(dir + '/' + d).catch(() => {});
    } else if (dev.removeTree) {
      await dev.removeTree(dir).catch(() => {});                   // a half-written .tmp has no package yet
    }
    await dev.remove(dir).catch(() => {});
  }

  async function uninstall(id) {
    if (!canInstall || !ID_RE.test(id)) throw new StoreError('install_unsupported');
    await removeTree(appsDir + '/' + id);
  }

  return { sources, catalog, install, uninstall, canInstall };
}

// Before each open, an installed app served from elsewhere (the cloud's store proxy: the device keeps its
// copy, the frame loads the network one) is checked against the package signed at install. The device's
// .package.sig is the reference: the served package.sig must be byte-identical and every file must match its
// size and SHA-256. Throws StoreError; resolves to the number of files checked.
export async function verifyServed({ fetch = globalThis.fetch, sigText, base }) {
  const pkg = parsePackage(sigText);
  const get = async (url) => {
    const r = await fetch(url, { credentials: 'same-origin' });
    if (!r.ok) throw new StoreError('network', r.status + ' ' + url);
    return new Uint8Array(await r.arrayBuffer());
  };
  if (dec.decode(await get(base + 'package.sig')) !== sigText) throw new StoreError('bad_package', 'served package differs from the installed one');
  for (const f of pkg.files) {
    const bytes = await get(base + f.path);
    if (bytes.length !== f.size || sha256Hex(bytes) !== f.sha256) throw new StoreError('bad_file', f.path);
  }
  return pkg.files.length;
}
