/* SPDX-License-Identifier: AGPL-3.0-or-later */
// fm-local.js — folders and drives of THIS computer, beside the device's own storage.
// Two ways in: the native bridge of the desktop companion (window.chrome.webview.hostObjects.nucleo, which lists
// the real drives), or the browser's File System Access API (the user grants one folder at a time).
// loc = { id, kind:'local', src:'bridge'|'fsa', label, icon, root?, path?, stack?, removable, writable }

export const bridge = (() => {
  try {
    return typeof window !== 'undefined' && window.chrome && window.chrome.webview && window.chrome.webview.hostObjects
      ? window.chrome.webview.hostObjects.nucleo : null;
  } catch { return null; }
})();
export const fsaSupported = typeof window !== 'undefined' && typeof window.showDirectoryPicker === 'function';

// Windows paths (the bridge): backslashes, "C:\" is a drive root.
// A name from the device must stay one name in the folder chosen on this computer: no separators, no "..", no drive
// colon or stream, no trailing dot or space, no reserved device name. Otherwise it is refused (bad-name).
export function winSafe(name) {
  const s = String(name == null ? '' : name);
  return !!s && s !== '.' && s !== '..' && !/[\\/:*?"<>|\u0000-\u001f]/.test(s) && !/[. ]$/.test(s)
    && !/^(con|prn|aux|nul|com[0-9]|lpt[0-9])(\.|$)/i.test(s);
}
export const winJoin = (a, b) => {
  if (!winSafe(b)) throw Object.assign(new Error('bad-name'), { code: 'bad-name' });
  return a.replace(/\\+$/, '') + '\\' + b;
};
export const winParent = (p) => { const t = p.replace(/\\+$/, ''); const i = t.lastIndexOf('\\'); return i <= 0 ? t.slice(0, 2) + '\\' : (t.slice(0, i) || t.slice(0, 2) + '\\'); };

const parseJ = (raw) => (typeof raw === 'string' ? JSON.parse(raw) : raw);
const b64ToBlob = (b64) => { const bin = atob(b64); const u8 = new Uint8Array(bin.length); for (let i = 0; i < bin.length; i++) u8[i] = bin.charCodeAt(i); return new Blob([u8]); };
const blobToB64 = (blob) => new Promise((res, rej) => { const fr = new FileReader(); fr.onerror = () => rej(new Error('read')); fr.onload = () => { const s = String(fr.result); res(s.slice(s.indexOf(',') + 1)); }; fr.readAsDataURL(blob); });
const bridgeCall = async (p) => { const r = JSON.parse((await p) || '{}'); if (r && r.error) throw new Error(r.error); return r; };

export async function listLocal(loc) {
  if (loc.src === 'bridge') {
    const j = parseJ(await bridge.ListDir(loc.path));
    if (j && j.error) throw new Error(j.error);
    return (j.entries || []).map((e) => ({ name: e.name, type: e.type === 'dir' ? 'dir' : 'file', size: e.size || 0, ...(e.mtime ? { mtime: e.mtime } : {}) }));
  }
  const dir = loc.stack[loc.stack.length - 1];
  const out = [];
  for await (const [name, h] of dir.entries()) {
    if (h.kind === 'directory') out.push({ name, type: 'dir', size: 0 });
    else { let size = 0, mtime; try { const f = await h.getFile(); size = f.size; mtime = f.lastModified; } catch {} out.push({ name, type: 'file', size, ...(mtime ? { mtime } : {}) }); }
  }
  return out;
}
// Only the sub-folders of a tree node ({ path } on the bridge, { handle } with FSA): [{ name, path?, handle?, leaf }]
export async function listLocalFolders(node) {
  if (node.loc.src === 'bridge') {
    const j = parseJ(await bridge.ListDir(node.path));
    if (j && j.error) throw new Error(j.error);
    return (j.entries || []).filter((e) => e.type === 'dir').map((e) => ({ name: e.name, path: winJoin(node.path, e.name), leaf: e.has_subdirs !== undefined ? !e.has_subdirs : false }));
  }
  const out = [];
  for await (const [name, h] of node.handle.entries()) {
    if (h.kind !== 'directory') continue;
    let sub = false;
    try { for await (const [, s] of h.entries()) { if (s.kind === 'directory') { sub = true; break; } } } catch {}
    out.push({ name, handle: h, leaf: !sub });
  }
  return out;
}
export async function localEnter(loc, name) {
  if (loc.src === 'bridge') { loc.path = winJoin(loc.path, name); return; }
  loc.stack.push(await loc.stack[loc.stack.length - 1].getDirectoryHandle(name));
}
export function localUp(loc) {
  if (loc.src === 'bridge') { if (loc.path.replace(/\\+$/, '').length > 2) loc.path = winParent(loc.path); }
  else if (loc.stack.length > 1) loc.stack.pop();
}
export const localAtRoot = (loc) => (loc.src === 'bridge' ? loc.path.replace(/\\+$/, '').length <= 2 : loc.stack.length <= 1);
export function localGoRoot(loc) { if (loc.src === 'bridge') loc.path = loc.root; else loc.stack = [loc.stack[0]]; }
// The part of a local path past its root ("Users\me\Pictures").
export function localTail(loc) {
  if (loc.src === 'bridge') { const r = loc.root || ''; return loc.path.length > r.length ? loc.path.slice(r.length).replace(/^\\+/, '') : ''; }
  return loc.stack.slice(1).map((h) => h.name).join('\\');
}
// The folder names from the root of the location to the folder shown.
export function localSegments(loc) {
  if (loc.src === 'bridge') { const r = loc.root.replace(/\\+$/, ''); const p = loc.path.replace(/\\+$/, ''); return p.length > r.length ? p.slice(r.length).split('\\').filter(Boolean) : []; }
  return loc.stack.slice(1).map((h) => h.name);
}
export async function localGoSegments(loc, segs) {
  if (loc.src === 'bridge') { loc.path = segs.length ? loc.root.replace(/\\+$/, '') + '\\' + segs.join('\\') : loc.root; return; }
  let h = loc.stack[0]; const stack = [h];
  for (const s of segs) { h = await h.getDirectoryHandle(s); stack.push(h); }
  loc.stack = stack;
}
export const localFullPath = (loc, name) => (loc.src === 'bridge' ? winJoin(loc.path, name) : (localTail(loc) ? localTail(loc) + '\\' : '') + name);

async function bridgeReadBlob(abs) {
  const b64 = await bridge.ReadFileB64(abs);
  if (b64 == null || b64 === '') throw new Error('read');
  return b64ToBlob(b64);
}
// One file of the folder shown, as a Blob; reader/deleter pairs captured now survive later navigation.
export async function localReader(loc, name) {
  if (loc.src === 'bridge') { const p = winJoin(loc.path, name); return { read: () => bridgeReadBlob(p), del: () => bridgeCall(bridge.DeleteEntry(p)) }; }
  const dir = loc.stack[loc.stack.length - 1];
  const fh = await dir.getFileHandle(name);
  return { read: () => fh.getFile(), del: () => dir.removeEntry(name) };
}
export async function localReadFile(loc, name) { return (await localReader(loc, name)).read(); }
export async function localWriteFile(loc, name, blob) {
  if (loc.src === 'bridge') return bridgeCall(bridge.WriteFileB64(winJoin(loc.path, name), await blobToB64(blob)));
  const fh = await loc.stack[loc.stack.length - 1].getFileHandle(name, { create: true });
  const w = await fh.createWritable(); await w.write(blob); await w.close();
}
export async function localMkdir(loc, name) {
  if (loc.src === 'bridge') return bridgeCall(bridge.MakeDir(winJoin(loc.path, name)));
  await loc.stack[loc.stack.length - 1].getDirectoryHandle(name, { create: true });
}
export async function localDelete(loc, e) {
  if (loc.src === 'bridge') return bridgeCall(bridge.DeleteEntry(winJoin(loc.path, e.name)));
  await loc.stack[loc.stack.length - 1].removeEntry(e.name, { recursive: e.type === 'dir' });
}
// FSA has no rename: a file is copied and the old name dropped; a folder cannot be renamed there.
export const canRenameLocal = (loc, e) => loc.src === 'bridge' || e.type !== 'dir';
export async function localRename(loc, e, newName) {
  if (loc.src === 'bridge') return bridgeCall(bridge.Rename(winJoin(loc.path, e.name), newName));
  if (e.type === 'dir') throw Object.assign(new Error('unsupported'), { code: 'unsupported' });
  const blob = await localReadFile(loc, e.name);
  await localWriteFile(loc, newName, blob);
  await localDelete(loc, e);
}

// The drives the bridge reports, as locations.
export async function bridgeDrives() {
  if (!bridge) return [];
  const arr = parseJ(await bridge.Drives()) || [];
  return arr.map((d) => {
    const root = d.path || (d.name + ':\\');
    return { id: 'drv:' + root, kind: 'local', src: 'bridge', icon: 'drive', label: d.label || d.name || root, root, path: root, removable: false, writable: true };
  });
}
// One folder the user grants through the browser's picker, as a location (null when cancelled).
export async function pickFolder() {
  if (!fsaSupported) return null;
  let handle; try { handle = await window.showDirectoryPicker({ mode: 'readwrite' }); } catch { return null; }
  let perm = 'granted';
  try { perm = await handle.queryPermission({ mode: 'readwrite' }); if (perm !== 'granted') perm = await handle.requestPermission({ mode: 'readwrite' }); } catch { perm = 'denied'; }
  return fsaLocation(handle, handle.name, perm === 'granted');
}
export const fsaLocation = (handle, label, writable, id) => ({ id: id || 'fsa:' + handle.name + ':' + Math.random().toString(36).slice(2),
  kind: 'local', src: 'fsa', icon: 'folder', label: label || handle.name, stack: [handle], path: handle.name, removable: true, writable: !!writable });
