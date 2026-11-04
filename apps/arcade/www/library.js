// SPDX-License-Identifier: AGPL-3.0-or-later
// The game library the device and the browser share: favourites and recently played, the games' screenshots on the
// card, the screenshots fetched for games that have none (a scraper, as EmulationStation's), and new ROMs put in the
// folder of their console. Pure functions first (tests/arcade-library.test.mjs), then the ones that touch the disk.

// ── favourites and recently played: one text file on the card ───────────────────────────────────────────────
// The device's Games gallery reads and writes the same file, so a star set on either side shows on both, and
// "continue playing" is whatever was played last, here or there.
//   # comment                     kept as is
//   F <card path>                 a favourite
//   R <n> <card path>             played recently; the highest n is the latest (at most RECENT_MAX lines)
// Lines this version does not know are kept, so a newer device or page can add kinds without losing them here.
export const LIBRARY = '/data/GameShots/library.txt';
export const RECENT_MAX = 30;

export function parseLibrary(text) {
  const lib = { favs: [], recent: [], other: [] };
  for (const raw of String(text || '').split(/\r?\n/)) {
    const line = raw.replace(/\s+$/, '');
    if (!line) continue;
    let m;
    if ((m = /^F (\/.+)$/.exec(line))) { if (!lib.favs.includes(m[1])) lib.favs.push(m[1]); }
    else if ((m = /^R (\d+) (\/.+)$/.exec(line))) lib.recent.push({ n: Number(m[1]), path: m[2] });
    else lib.other.push(line);
  }
  // newest first, one entry per game
  lib.recent.sort((a, b) => b.n - a.n);
  const seen = new Set();
  lib.recent = lib.recent.filter((r) => !seen.has(r.path) && seen.add(r.path)).slice(0, RECENT_MAX);
  return lib;
}

export function formatLibrary(lib) {
  const head = lib.other.length ? lib.other : ['# NucleoOS game library: the Games gallery and the web Arcade share it'];
  return head.concat(lib.favs.map((p) => 'F ' + p), lib.recent.map((r) => 'R ' + r.n + ' ' + r.path)).join('\n') + '\n';
}

export function toggleFav(lib, path) {
  const i = lib.favs.indexOf(path);
  if (i >= 0) lib.favs.splice(i, 1); else lib.favs.push(path);
  return i < 0;
}

export function notePlayed(lib, path) {
  const n = lib.recent.reduce((a, r) => Math.max(a, r.n), 0) + 1;
  lib.recent = [{ n, path }].concat(lib.recent.filter((r) => r.path !== path)).slice(0, RECENT_MAX);
}

// The browser's old shelf (localStorage "arcade.favs" = ["sys/file"], "arcade.recent" = [{s, f, ts}]) folded into the
// shared file once: nothing the user starred before this version is lost.
export function mergeLegacy(lib, favs, recent, romDir) {
  let changed = false;
  for (const k of favs || []) {
    const p = romDir + k;
    if (!lib.favs.includes(p)) { lib.favs.push(p); changed = true; }
  }
  const old = (recent || []).slice().sort((a, b) => (a.ts || 0) - (b.ts || 0));
  for (const r of old) {
    const p = romDir + r.s + '/' + r.f;
    if (!lib.recent.some((x) => x.path === p)) { lib.recent.push({ n: 0, path: p }); changed = true; }
  }
  if (changed) {
    // the old entries go below everything the file already ordered, in their own order
    const known = lib.recent.filter((r) => r.n > 0), added = lib.recent.filter((r) => r.n === 0);
    let n = 1;
    added.forEach((r) => { r.n = n++; });
    known.forEach((r) => { r.n += added.length; });
    lib.recent = known.concat(added.reverse()).slice(0, RECENT_MAX);
  }
  return changed;
}

// ── the games' pictures on the card ─────────────────────────────────────────────────────────────────────────
// <rom folder>/.snaps/<rom file>.snp — "SNP1", u16 w, u16 h, u32 deflated size (LE), raw-deflated RGB565 (LE): the
// device's emulator shelves. <rom file>.pic — "PIC1", u16 w, u16 h, u32 0, u32 0, RGB565 high byte first: the Games
// gallery streams it to the panel as is. Both fit 112 x 101 (tools/fetch-rom-snaps.py in the firmware repository).
export const SNAP_BOX = [112, 101];
export const snapPath = (dir, file, ext) => dir.replace(/\/$/, '') + '/.snaps/' + file + '.' + ext;

async function inflateRaw(bytes) {
  const s = new Blob([bytes]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
  return new Uint8Array(await new Response(s).arrayBuffer());
}
async function deflateRaw(bytes) {
  const s = new Blob([bytes]).stream().pipeThrough(new CompressionStream('deflate-raw'));
  return new Uint8Array(await new Response(s).arrayBuffer());
}

// RGBA (w x h) from a .snp, or null when it is not one.
export async function decodeSnp(buf) {
  const b = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
  if (b.length < 12 || b[0] !== 0x53 || b[1] !== 0x4e || b[2] !== 0x50 || b[3] !== 0x31) return null;
  const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
  const w = dv.getUint16(4, true), h = dv.getUint16(6, true), n = dv.getUint32(8, true);
  if (!w || !h || w > 512 || h > 512 || 12 + n > b.length) return null;
  let raw;
  try { raw = await inflateRaw(b.subarray(12, 12 + n)); } catch { return null; }
  if (raw.length < w * h * 2) return null;
  const rgba = new Uint8ClampedArray(w * h * 4);
  for (let i = 0; i < w * h; i++) {
    const v = raw[i * 2] | raw[i * 2 + 1] << 8;
    rgba[i * 4] = (v >> 11 & 31) * 255 / 31; rgba[i * 4 + 1] = (v >> 5 & 63) * 255 / 63; rgba[i * 4 + 2] = (v & 31) * 255 / 31;
    rgba[i * 4 + 3] = 255;
  }
  return { w, h, rgba };
}

// The .snp and .pic of an RGBA picture already fitted to SNAP_BOX.
export async function encodeSnap(w, h, rgba) {
  const le = new Uint8Array(w * h * 2), be = new Uint8Array(w * h * 2);
  for (let i = 0; i < w * h; i++) {
    const v = (rgba[i * 4] & 0xf8) << 8 | (rgba[i * 4 + 1] & 0xfc) << 3 | rgba[i * 4 + 2] >> 3;
    le[i * 2] = v & 255; le[i * 2 + 1] = v >> 8;
    be[i * 2] = v >> 8; be[i * 2 + 1] = v & 255;
  }
  const z = await deflateRaw(le);
  const snp = new Uint8Array(12 + z.length), pic = new Uint8Array(16 + be.length);
  const ds = new DataView(snp.buffer), dp = new DataView(pic.buffer);
  snp.set([0x53, 0x4e, 0x50, 0x31]); ds.setUint16(4, w, true); ds.setUint16(6, h, true); ds.setUint32(8, z.length, true);
  snp.set(z, 12);
  pic.set([0x50, 0x49, 0x43, 0x31]); dp.setUint16(4, w, true); dp.setUint16(6, h, true);
  pic.set(be, 16);
  return { snp, pic };
}

// Where a screenshot of (iw x ih) lands: an exact integer upscale of the console's screen is undone first (the
// pixels stay sharp), then it shrinks into SNAP_BOX keeping its shape (never grows).
export function fitSnap(iw, ih, native) {
  let w = iw, h = ih;
  if (native) {
    const k = Math.min(Math.floor(w / native[0]), Math.floor(h / native[1]));
    if (k >= 2 && w % native[0] === 0 && h % native[1] === 0) { w /= k; h /= k; }
  }
  const s = Math.min(1, SNAP_BOX[0] / w, SNAP_BOX[1] / h);
  return { w: Math.max(1, Math.round(w * s)), h: Math.max(1, Math.round(h * s)), pre: [w, h] };
}

// ── the scraper: libretro-thumbnails, by the cartridge's name ───────────────────────────────────────────────
// Same matching as tools/fetch-rom-snaps.py: the very same No-Intro name first, then the name without its tags,
// then squashed (no spaces, 2 = II), then the title without its subtitle, then a prefix; among equals the
// international release, never a beta or a hack.
const ROMAN = { 1: 'i', 2: 'ii', 3: 'iii', 4: 'iv', 5: 'v', 6: 'vi' };
const stemOf = (s) => { const i = s.lastIndexOf('.'); return i > 0 ? s.slice(0, i) : s; };
export function norm(s) {
  s = stemOf(s).toLowerCase().replace(/[([].*?[)\]]/g, ' ').replace(/&/g, ' and ');
  return s.replace(/[^a-z0-9]+/g, ' ').trim().split(/\s+/).filter(Boolean).join(' ');
}
export const squash = (s) => norm(s).split(' ').map((w) => ROMAN[w] || w).join('');

export function snapCandidates(names, rom) {
  const want = norm(rom), stem = stemOf(rom).replace(/&/g, '_'), sq = squash(rom);
  const head = rom.includes(' - ') ? squash(rom.split(' - ')[0]) : null;
  const hits = [];
  const add = (f) => names.forEach((n) => { if (!hits.includes(n) && f(n)) hits.push(n); });
  // a snap's name has no extension: norm() must not take "Dr. Mario"'s ". Mario" for one
  const nn = (n) => norm(n + '.png'), sn = (n) => squash(n + '.png');
  add((n) => n === stem);
  add((n) => nn(n) === want);
  add((n) => sn(n) === sq);
  add((n) => head && sn(n) === head);
  add((n) => nn(n).startsWith(want + ' '));
  const rank = (n) => [/\((USA|World|USA, Europe)\)/.test(n) ? 0 : 1, /Beta|Hack|Unl|Proto/.test(n) ? 1 : 0];
  const exact = hits.filter((n) => n === stem);
  const rest = hits.filter((n) => n !== stem).sort((a, b) => {
    const ra = rank(a), rb = rank(b);
    return ra[0] - rb[0] || ra[1] - rb[1];
  });
  return exact.concat(rest).slice(0, 3);
}

// The Named_Snaps of one libretro-thumbnails repository (one GitHub request a console, kept for the session).
const indexCache = new Map();
export async function snapIndex(repo, fetchFn = fetch) {
  if (indexCache.has(repo)) return indexCache.get(repo);
  const key = 'arcade.snapindex.' + repo;
  try { const c = JSON.parse(sessionStorage.getItem(key)); if (Array.isArray(c)) { indexCache.set(repo, c); return c; } } catch {}
  const r = await fetchFn(`https://api.github.com/repos/libretro-thumbnails/${repo}/git/trees/master?recursive=1`);
  if (!r.ok) throw new Error('index ' + r.status);
  const j = await r.json();
  const names = (j.tree || []).map((t) => t.path).filter((p) => p.startsWith('Named_Snaps/') && p.endsWith('.png'))
    .map((p) => p.slice('Named_Snaps/'.length, -4));
  indexCache.set(repo, names);
  try { sessionStorage.setItem(key, JSON.stringify(names)); } catch {}
  return names;
}
export const snapUrl = (repo, name) =>
  `https://raw.githubusercontent.com/libretro-thumbnails/${repo}/master/Named_Snaps/${encodeURIComponent(name)}.png`;

// ── new ROMs: which console's folder ────────────────────────────────────────────────────────────────────────
// By the extension, when exactly one console with a folder on the card takes it (.pce, .nes, .gba...). Shared ones
// (.gb on Game Boy and Color, .bin, .zip) go to the console being shown, or are left for the user to place.
export function systemFor(name, systems, current) {
  const i = name.lastIndexOf('.');
  const ext = i < 0 ? '' : name.slice(i + 1).toLowerCase();
  if (!ext) return null;
  if (current && current.exts.includes(ext)) return current;
  const takers = systems.filter((s) => s.exts.includes(ext) && ext !== 'zip');
  if (takers.length === 1) return takers[0];
  // .gb: the Game Boy's folder (the Color folder also takes it, but a .gb is a Game Boy cartridge first)
  if (ext === 'gb') return takers.find((s) => s.id === 'gb') || null;
  return null;
}
