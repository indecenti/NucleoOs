/* SPDX-License-Identifier: AGPL-3.0-or-later */
// fm-ops.js — what the file manager does to the device's files, without any DOM: the recycle bin and its
// undo, paste from the OS clipboard, duplicate, shortcuts, permanent delete, folder sizes, drops between folders.
// Every disk operation goes through the core file layer (`fs`, /nucleo-fs.js). The tests bind it to a fake
// device (tests/suite/file-commander-ops.test.mjs, with tests/suite/lib/core-sandbox.mjs).
import { join, dirOf, nameOf, splitExt, isInside } from './fm-path.js';

export const TRASH = '/system/.trash';
// Never the source of a paste: the same folders the shell refuses to link (shell.js PROTECTED_TARGET).
const PROTECTED_SRC = /^\/(system|www|apps)(\/|$)|^\/data\/anima(\/|$)|(^|\/)\.\.(\/|$)/i;
export const TRASH_DB = '/system/config/trash.json';
const TRASH_CAP = 20, UNDO_CAP = 25;
const TREE_CAP = 5000;                           // files a folder size / copy / permanent delete will walk at most

// createOps({ fs, Caps, t, notify(text, kind), onUndo() })
export function createOps({ fs, Caps, t, notify = () => {}, onUndo = () => {} }) {
  const undoStack = [];
  // The device answers 403 for protected system files: said once, here, for every destructive call.
  const guard = (p) => p.catch((err) => { if (err && err.code === 'denied') notify(t('lock_kept_safe'), 'warn'); throw err; });
  const userError = (msg) => Object.assign(new Error(msg), { user: true });

  // ── recycle bin ──
  // The trash index, or null when it could not be read. A failed read is NEVER an empty bin: saving that
  // would wipe the record of every other trashed file. A 404, or "{}", is a genuinely empty bin.
  async function loadTrashDb() {
    if (typeof fs.trashIndex === 'function') {
      try { const d = await fs.trashIndex(); return d && Array.isArray(d.items) ? d : null; } catch { return null; }
    }
    let txt; try { txt = await guard(fs.readText(TRASH_DB)); } catch { return null; }
    if (txt == null || !txt.trim()) return { items: [] };
    try {
      const d = JSON.parse(txt);
      if (d && typeof d === 'object' && !Array.isArray(d)) {
        if (d.items === undefined) return { ...d, items: [] };
        if (Array.isArray(d.items)) return d;
      }
    } catch {}
    // A readable but CORRUPT index: kept aside byte for byte, a fresh one started; if even that copy cannot be
    // written, nothing changes.
    try { await guard(fs.writeText(TRASH_DB.replace(/.json$/, '.corrupt.json'), txt)); return { items: [] }; } catch {}
    return null;
  }
  const saveTrashDb = (db) => guard(fs.writeJSON(TRASH_DB, db));
  // Does a delete of this path go to the SERVER's bin? Only the device can say, on the resolved path
  // (/api/fs/stat → trash:true, capability fs.stat). Without it the app keeps its own checked trash — never a
  // guess from the path: on a backend that hard-deletes, a wrong guess would destroy the file.
  async function serverTrashes(path) {
    if (!Caps || !(await Caps.has('fs.stat'))) return false;
    try { const st = await fs.stat(path); return !!(st && st.trash === true); } catch { return false; }
  }
  // One file into the Recycle Bin (app-side path). Every step is checked and the original is deleted LAST: index readable ->
  // copy written -> index saved -> original deleted. Any failure throws (a message for the user) and leaves the
  // original where it was.
  // THE one way into the Recycle Bin: the core's fs.trash where this OS has it (same index format), else the
  // app's own checked steps below. The undo record is kept here either way.
  async function toTrash(path, record = true) {
    const name = nameOf(path);
    const notTrashed = () => userError(t('err_trash_failed', { name }));
    if (typeof fs.trash === 'function') {
      let entry;
      try { entry = await guard(fs.trash(path)); } catch { throw notTrashed(); }
      if (record && entry && entry.trash) pushUndo({ type: 'delete', orig: entry.orig || path, trash: entry.trash });
      return entry;
    }
    if (await serverTrashes(path)) {
      let d = false; try { d = await guard(fs.remove(path)); } catch {}
      if (!d) throw notTrashed();
      if (record) {
        const db = await loadTrashDb();
        const it = ((db && db.items) || []).find((i) => i.orig === path);
        if (it) pushUndo({ type: 'delete', orig: path, trash: it.trash });
      }
      return;
    }
    const db = await loadTrashDb();
    if (!db) throw userError(t('err_trash_index'));
    const tpath = join(TRASH, Date.now() + '_' + Math.floor(Math.random() * 9999) + '_' + name);
    try { await guard(fs.copy(path, tpath)); } catch { fs.remove(tpath).catch(() => {}); throw notTrashed(); }
    const entry = { orig: path, trash: tpath, name, ts: Date.now() };
    db.items.unshift(entry);
    let saved = false; try { await saveTrashDb(db); saved = true; } catch {}
    if (!saved) { fs.remove(tpath).catch(() => {}); throw notTrashed(); }
    let d = false; try { d = await guard(fs.remove(path)); } catch {}
    if (!d) {
      db.items = db.items.filter((i) => i !== entry);
      await saveTrashDb(db).catch(() => {});
      fs.remove(tpath).catch(() => {});
      throw notTrashed();
    }
    if (db.items.length > TRASH_CAP) {           // the oldest go only once this one is safely in
      while (db.items.length > TRASH_CAP) { const old = db.items.pop(); try { await guard(fs.remove(old.trash)); } catch {} }
      await saveTrashDb(db).catch(() => {});
    }
    if (record) pushUndo({ type: 'delete', orig: path, trash: tpath });
  }
  // A trashed file back where it was; the trash copy goes only once the restored file is there.
  async function restoreFromTrash(trash, orig) {
    const name = nameOf(orig);
    try {
      const db = await loadTrashDb();
      if (!db) { notify(t('err_trash_index'), 'error'); return false; }
      // back where it was, unless something new took the name meanwhile: then next to it ("x (2).txt"), never over it
      const to = (await fs.exists(orig)) ? await fs.uniquePath(dirOf(orig), name) : orig;
      try { await fs.move(trash, to); }
      catch {
        await guard(fs.copy(trash, to));
        await guard(fs.remove(trash)).catch(() => {});
      }
      db.items = db.items.filter((i) => i.trash !== trash); await saveTrashDb(db).catch(() => {});
      return true;
    } catch { notify(t('err_restore_failed', { name }), 'error'); return false; }
  }
  // The device removes only an EMPTY folder (a full one answers "no entry"): said, not silently ignored.
  async function deleteFolder(path) {
    let gone = false;
    try { gone = await guard(fs.remove(path)); } catch (err) { if (err && err.code === 'denied') return false; }
    if (gone) return true;
    notify(t('err_folder_not_empty', { name: nameOf(path) }), 'warn');
    return false;
  }

  // ── walking a folder ──
  // Every file and folder below `root` (folders listed after their contents: the order a delete needs).
  async function walk(root, { signal, onProgress } = {}) {
    const files = [], dirs = [];
    const stack = [root];
    const order = [];
    while (stack.length) {
      if (signal && signal.aborted) return { files, dirs, cancelled: true, truncated: false };
      const d = stack.pop();
      order.push(d);
      const list = await fs.list(d);
      for (const e of list) {
        const p = join(d, e.name);
        if (e.type === 'dir') stack.push(p);
        else files.push({ path: p, size: Number(e.size) || 0, protected: !!e.protected });
        if (files.length >= TREE_CAP) return { files, dirs: order.reverse(), cancelled: false, truncated: true };
      }
      if (onProgress) onProgress({ files: files.length, dirs: order.length, bytes: files.reduce((a, f) => a + f.size, 0) });
    }
    dirs.push(...order.reverse());
    return { files, dirs, cancelled: false, truncated: false };
  }
  // { bytes, files, dirs, truncated, cancelled } of a folder, counted file by file.
  async function folderSize(path, opts = {}) {
    let bytes = 0, files = 0, dirs = 0;
    const stack = [path];
    while (stack.length) {
      if (opts.signal && opts.signal.aborted) return { bytes, files, dirs, cancelled: true, truncated: false };
      const d = stack.pop();
      let list = [];
      try { list = await fs.list(d); } catch { continue; }
      for (const e of list) {
        if (e.type === 'dir') { dirs++; stack.push(join(d, e.name)); }
        else { files++; bytes += Number(e.size) || 0; }
      }
      if (opts.onProgress) opts.onProgress({ bytes, files, dirs });
      if (files >= TREE_CAP * 4) return { bytes, files, dirs, cancelled: false, truncated: true };
    }
    return { bytes, files, dirs, cancelled: false, truncated: false };
  }
  // Delete permanently: a file, or a folder with everything in it (bottom-up). Protected files are kept, and
  // with them the folders that hold them.
  async function removeForever(path, isDir, opts = {}) {
    if (!isDir) return guard(fs.remove(path));
    const w = await walk(path, opts);
    if (w.cancelled) return false;
    if (w.truncated) throw userError(t('err_tree_too_big', { count: TREE_CAP }));
    for (const f of w.files) {
      if (opts.signal && opts.signal.aborted) return false;
      if (f.protected) continue;
      await guard(fs.remove(f.path));
    }
    let ok = true;
    for (const d of w.dirs) { try { if (!(await fs.remove(d))) ok = false; } catch { ok = false; } }
    return ok;
  }
  // A folder copied file by file (the device has no folder copy).
  async function copyTree(from, to, opts = {}) {
    if (isInside(to, from)) throw userError(t('err_into_itself'));
    const w = await walk(from, opts);
    if (w.truncated) throw userError(t('err_tree_too_big', { count: TREE_CAP }));
    await fs.mkdirp(to);
    for (const d of w.dirs) if (d !== from) await fs.mkdirp(join(to, d.slice(from.length)));
    for (const f of w.files) {
      if (opts.signal && opts.signal.aborted) return false;
      await guard(fs.copy(f.path, join(to, f.path.slice(from.length))));
    }
    return true;
  }

  // ── duplicate, paste, shortcuts ──
  // Ctrl+D: "report.txt" → "report (2).txt" beside it; folders too. → the paths created.
  async function duplicate(entries) {
    const created = [];
    let failed = 0;
    for (const e of entries) {
      try {
        const dst = await fs.uniquePath(dirOf(e.path), e.name);
        if (e.type === 'dir') await copyTree(e.path, dst); else await guard(fs.copy(e.path, dst));
        created.push(dst);
      } catch { failed++; }
    }
    if (created.length) pushUndo({ type: 'paste', created });
    if (failed) notify(t('err_copy_failed', { count: failed }), 'error');
    return created;
  }
  // A paste of the OS clipboard's { kind:'files', data:{ op, paths } } (another window, the device) into dir.
  async function pasteFiles(clip, dir) {
    if (!clip || clip.kind !== 'files' || !clip.data || !Array.isArray(clip.data.paths)) return { created: [], moved: [], failed: 0 };
    const { op, paths } = clip.data;
    const created = [], moved = [];
    let failed = 0;
    for (const src of paths) {
      if (typeof src !== 'string' || !src.startsWith('/')) continue;
      if (PROTECTED_SRC.test(src)) { failed++; continue; }      // the system, apps' code, ANIMA: never copied or moved
      let dst = join(dir, nameOf(src));
      if (dst === src) {
        if (op === 'cut') continue;
        const [b, x] = splitExt(nameOf(src));
        dst = await fs.uniquePath(dir, b + ' - ' + t('copy_word') + x).catch(() => join(dir, b + ' copy' + x));
      }
      if (dst !== src && await fs.exists(dst)) dst = await fs.uniquePath(dir, nameOf(src));   // never over a file that is there
      try { await guard(fs.copy(src, dst)); }
      catch { failed++; continue; }                          // the source is untouched
      if (op === 'cut') {
        // The copy landed; the source goes only into the Recycle Bin. If that fails it stays put, and the copy is
        // kept as a plain paste (undoable) so nothing is lost.
        try { await toTrash(src, false); moved.push({ src, dst }); }
        catch { created.push(dst); failed++; }
      } else created.push(dst);
    }
    if (moved.length) pushUndo({ type: 'move', moved });
    if (created.length) pushUndo({ type: 'paste', created });
    if (failed) notify(t(op === 'cut' ? 'err_move_kept' : 'err_copy_failed', { count: failed }), 'error');
    return { created, moved, failed };
  }
  const lnkJson = (target, label) => JSON.stringify({ schema: 1, type: 'file', target, label: label || undefined, createdAt: Date.now() });
  async function makeShortcut(target, destDir, label) {
    const dst = await fs.uniquePath(destDir, label + '.lnk').catch(() => join(destDir, label + '.lnk'));
    await guard(fs.writeText(dst, lnkJson(target, label)));
    return dst;
  }
  // Drag and drop between folders of the device: 'move' | 'copy' | 'shortcut'. → number done.
  async function dropInto(destDir, items, op) {
    let ok = 0, failed = 0;
    for (const it of items) {
      if (!it || !it.path) continue;
      if (isInside(destDir, it.path)) continue;                 // never a folder into itself
      if (op !== 'shortcut' && dirOf(it.path) === destDir) continue;
      if (op === 'shortcut') {
        try { await makeShortcut(it.path, destDir, it.name.replace(/\.lnk$/i, '')); ok++; } catch {}
        continue;
      }
      const dst = await fs.uniquePath(destDir, it.name).catch(() => join(destDir, it.name));
      try {
        if (op === 'move') await guard(fs.move(it.path, dst));
        else if (it.isDir) await copyTree(it.path, dst);
        else await guard(fs.copy(it.path, dst));
        ok++;
      } catch { failed++; }
    }
    if (failed) notify(t(op === 'move' ? 'err_move_kept' : 'err_copy_failed', { count: failed }), 'error');
    return ok;
  }

  // ── undo (this session; the Recycle Bin is the lasting net) ──
  function pushUndo(rec) { undoStack.push(rec); if (undoStack.length > UNDO_CAP) undoStack.shift(); onUndo(); }
  async function undo() {
    const rec = undoStack.pop();
    if (!rec) return false;
    if (rec.type === 'delete') { if (!(await restoreFromTrash(rec.trash, rec.orig))) undoStack.push(rec); }
    else if (rec.type === 'paste') {
      for (const p of rec.created) {
        try { if (!(await guard(fs.remove(p)))) { const st = await fs.stat(p).catch(() => null); if (st && st.type === 'dir') await removeForever(p, true); } } catch {}
      }
    } else if (rec.type === 'move') { for (const m of rec.moved) { try { await fs.move(m.dst, m.src); } catch {} } }
    onUndo();
    return true;
  }

  return { guard, loadTrashDb, saveTrashDb, toTrash, trashFile: toTrash, restoreFromTrash, deleteFolder, folderSize, removeForever, copyTree,
    duplicate, pasteFiles, makeShortcut, dropInto, pushUndo, undo, lnkJson,
    get canUndo() { return undoStack.length > 0; }, get undoSize() { return undoStack.length; } };
}
