/* SPDX-License-Identifier: AGPL-3.0-or-later */
// fm-path.js — device paths for the file manager: joining, parents, breadcrumbs, what the address bar accepts,
// which names a file may have. Pure (no DOM, no fs): tests/suite/file-commander-path.test.mjs.

// '/data//a/./b/../c/' → '/data/a/c'; never climbs above '/'.
export function norm(p) {
  const out = [];
  for (const seg of String(p == null ? '' : p).split('/')) {
    if (seg === '' || seg === '.') continue;
    if (seg === '..') { out.pop(); continue; }
    out.push(seg);
  }
  return '/' + out.join('/');
}
export const join = (dir, name) => norm(String(dir) + '/' + String(name));
export const dirOf = (p) => norm(p).replace(/\/[^/]*$/, '') || '/';
export const nameOf = (p) => norm(p).split('/').pop();
// true when `p` is `dir` itself or anything below it.
export const isInside = (p, dir) => { const a = norm(p), b = norm(dir); return a === b || a.startsWith(b === '/' ? '/' : b + '/'); };

// "/data/Music/Live" → [{ name:'/', path:'/' }, { name:'data', path:'/data' }, { name:'Music', … }, { name:'Live', … }]
export function crumbs(p) {
  const out = [{ name: '/', path: '/' }];
  let acc = '';
  for (const seg of norm(p).split('/').filter(Boolean)) { acc += '/' + seg; out.push({ name: seg, path: acc }); }
  return out;
}

// Characters no edition's file system accepts in a name (FAT on the ESP32 cards is the strictest).
const BAD_CHARS = /[<>:"|?*\\\u0000-\u001f\u007f]/;

// What the user typed in the address bar → { ok: true, path } or { ok: false, reason }.
//   absolute  "/data/Music"           as is (normalised)
//   relative  "Music", "../Pictures"  from the folder shown
//   home      "~", "~/Documents"      under /data, the user's own space
//   a location label the app shows ("SD:/data/x", "sd:/data/x") is accepted too
// reason: 'empty' | 'chars' (a character no path may hold) | 'foreign' (a PC path such as "C:\Users").
export function parseAddress(input, cwd = '/data') {
  let s = String(input == null ? '' : input).trim();
  if ((s.startsWith('"') && s.endsWith('"')) || (s.startsWith("'") && s.endsWith("'"))) s = s.slice(1, -1).trim();
  if (!s) return { ok: false, reason: 'empty' };
  if (/^[A-Za-z]:([\\/]|$)/.test(s) || s.startsWith('\\\\')) return { ok: false, reason: 'foreign' };
  s = s.replace(/^sd:/i, '');
  if (/[\u0000-\u001f\u007f<>"|?*\\]/.test(s)) return { ok: false, reason: 'chars' };
  if (s === '~') return { ok: true, path: '/data' };
  if (s.startsWith('~/')) return { ok: true, path: norm('/data/' + s.slice(2)) };
  if (s.startsWith('/')) return { ok: true, path: norm(s) };
  return { ok: true, path: norm(norm(cwd) + '/' + s) };
}

// A name the user typed for a new or renamed item: null when fine, else why not
// ('empty' | 'dots' | 'slash' | 'chars' | 'long').
export function nameProblem(name) {
  const s = String(name == null ? '' : name);
  if (!s.trim()) return 'empty';
  if (s === '.' || s === '..') return 'dots';
  if (s.includes('/')) return 'slash';
  if (BAD_CHARS.test(s)) return 'chars';
  if (new TextEncoder().encode(s).length > 200) return 'long';
  if (/^\.~nfs[a-z0-9]{5}$/.test(s)) return 'chars';                 // the core's temporary names: hidden from every list
  return null;
}

// "report.txt" → ["report", ".txt"]; ".bashrc" → [".bashrc", ""]; "a.tar.gz" → ["a.tar", ".gz"].
export function splitExt(name) {
  const s = String(name);
  const i = s.lastIndexOf('.');
  return i > 0 ? [s.slice(0, i), s.slice(i)] : [s, ''];
}

// A free name among `taken` (a Set, updated): "report.txt" → "report (1).txt". Used where a batch must not
// collide with itself (a drop of several files); fs.uniquePath asks the device for one name.
export function uniqueName(name, taken) {
  if (!taken.has(name)) { taken.add(name); return name; }
  const [base, ext] = splitExt(name);
  for (let i = 1; ; i++) { const c = `${base} (${i})${ext}`; if (!taken.has(c)) { taken.add(c); return c; } }
}
