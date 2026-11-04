/* SPDX-License-Identifier: AGPL-3.0-or-later */
// fm-menus.js — what the right click offers, as item lists for the OS menu (/nucleo-menu.js).
// Pure: every builder takes the context `m` and returns items; `m.act` holds the actions (fm-app.js).
// What does not apply here (a read-only location, a protected file) is disabled, never hidden, so a menu
// keeps its shape and the user learns where things are. tests/suite/file-commander-menus.test.mjs.
//
//   m = { t, act, sd, writable, hasClip, inSearch, prefs }
import { isRunnable, isZip, isShortcut } from './fm-types.js';

export const KEYS = {
  open: 'Enter', cut: 'Ctrl+X', copy: 'Ctrl+C', paste: 'Ctrl+V', dup: 'Ctrl+D', rename: 'F2', del: 'Del',
  delPerm: 'Shift+Del', props: 'Alt+Enter', newFolder: 'Ctrl+Shift+N', refresh: 'F5', all: 'Ctrl+A', copyPath: '',
};

// The "Open with" ▸ submenu. Built at once with what is known; `fillOpenWith` puts the apps in later, in
// place (the OS menu reads a submenu's items when it opens, so the answer of os.handlers lands in time).
// "Compress ▸": the formats the OS writes (m.packFormats, from nucleo-pack.js), and a ZIP with a password.
export function compressSub(m, sel) {
  const { t, act } = m;
  const names = { zip: 'ZIP (.zip)', '7z': '7-Zip (.7z)', 'tar.gz': 'tar.gz', 'tar.xz': 'tar.xz', 'tar.bz2': 'tar.bz2', 'tar.zst': 'tar.zst', tar: 'tar' };
  const formats = (m.packFormats || ['zip']).filter((f) => f in names);
  const items = formats.map((f) => ({ label: names[f], icon: 'archive', fn: () => act.compress(sel, f) }));
  items.push({ sep: true }, { label: t('ctx_compress_password'), icon: 'lock', fn: () => act.compress(sel, 'zip', { password: true }) });
  return items;
}
const isArchiveIn = (m, name) => (m.isArchive ? m.isArchive(name) : isZip(name));

export function openWithSub(m, e, handlers) {
  const sub = [];
  fillOpenWith(sub, m, e, handlers);
  return sub;
}
export function fillOpenWith(sub, m, e, handlers) {
  const { t, act } = m;
  const items = [];
  if (handlers == null) items.push({ label: t('ow_loading'), disabled: true });
  else if (!handlers.length) items.push({ label: t('ow_none'), disabled: true });
  else for (const h of handlers) {
    items.push({ label: h.isDefault ? t('ow_default', { app: h.name || h.id }) : (h.name || h.id), icon: 'open-with', fn: () => act.openWithApp(e, h.id) });
  }
  items.push({ sep: true }, { label: t('ow_choose'), icon: 'open-with', fn: () => act.chooseApp(e) });
  sub.splice(0, sub.length, ...items);
  return sub;
}

// One item (file or folder) of the list or of search results.
// extra: { handlers (array or null while loading, filled in place later), defaultName, isFav }
export function itemMenu(m, e, extra = {}) {
  const { t, act } = m;
  const isDir = e.type === 'dir';
  const w = !!m.writable;
  const prot = !!e.protected;
  if (!m.sd) {                                     // a folder or drive of this computer
    return [
      { label: t('open'), icon: 'open', shortcut: KEYS.open, disabled: !isDir, fn: () => act.open(e) },
      { label: t('ctx_copy_to_device'), icon: 'upload', disabled: isDir, fn: () => act.copyToDevice() },
      { sep: true },
      { label: t('cut'), icon: 'cut', shortcut: KEYS.cut, disabled: isDir || !w, fn: () => act.cut() },
      { label: t('copy'), icon: 'copy', shortcut: KEYS.copy, disabled: isDir, fn: () => act.copy() },
      { label: t('paste'), icon: 'paste', shortcut: KEYS.paste, disabled: !w || !m.hasClip, fn: () => act.paste() },
      { sep: true },
      { label: t('ctx_rename'), icon: 'rename', shortcut: KEYS.rename, disabled: !w || !act.canRenameLocal(e), fn: () => act.rename(e) },
      { label: t('ctx_delete_perm'), icon: 'delete', shortcut: KEYS.del, danger: true, disabled: !w, fn: () => act.del() },
      { sep: true },
      { label: t('ctx_copy_path'), icon: 'path', fn: () => act.copyPath([e]) },
      { label: t('prop_title'), icon: 'info', shortcut: KEYS.props, fn: () => act.properties([e]) },
    ];
  }
  const items = [];
  if (prot) items.push({ header: t('ctx_protected') });
  const openLabel = extra.defaultName && !isDir ? t('open_with_hint', { app: extra.defaultName }) : t('open');
  items.push({ label: openLabel, icon: isDir ? 'open' : 'file', shortcut: KEYS.open, fn: () => act.open(e) });
  if (!isDir && !isShortcut(e.name)) items.push({ label: t('openWith'), icon: 'open-with', sub: extra.sub || openWithSub(m, e, extra.handlers) });
  if (!isDir && isRunnable(e.name)) items.push({ label: t('ctx_run'), icon: 'run', fn: () => act.run(e) });
  if (isDir) items.push({ label: t('ctx_new_window'), icon: 'open', fn: () => act.openNewWindow(e.path) });
  if (m.inSearch) items.push({ label: t('ctx_open_location'), icon: 'folder', fn: () => act.openLocation(e) });
  items.push(
    { sep: true },
    { label: t('cut'), icon: 'cut', shortcut: KEYS.cut, disabled: !w || prot || isDir, fn: () => act.cut() },
    { label: t('copy'), icon: 'copy', shortcut: KEYS.copy, disabled: isDir, fn: () => act.copy() },
    isDir
      ? { label: t('ctx_paste_into'), icon: 'paste', disabled: !w || !m.hasClip, fn: () => act.paste(e.path) }
      : { label: t('paste'), icon: 'paste', shortcut: KEYS.paste, disabled: !w || !m.hasClip || m.inSearch, fn: () => act.paste() },
    { label: t('duplicate'), icon: 'duplicate', shortcut: KEYS.dup, disabled: !w, fn: () => act.duplicate([e]) },
    { sep: true },
    { label: t('ctx_rename'), icon: 'rename', shortcut: KEYS.rename, disabled: !w || prot, fn: () => act.rename(e) },
    { label: t('delete'), icon: 'delete', shortcut: KEYS.del, disabled: !w || prot, fn: () => act.del() },
    { label: t('ctx_delete_perm'), icon: 'delete', shortcut: KEYS.delPerm, danger: true, disabled: !w || prot, fn: () => act.delPermanent() },
    { sep: true },
    { label: t('ctx_compress'), icon: 'archive', disabled: !w, sub: compressSub(m, [e]) },
  );
  if (!isDir && isArchiveIn(m, e.name)) items.push({ label: t('ctx_extract'), icon: 'extract', disabled: !w, fn: () => act.extract(e) });
  items.push(
    { sep: true },
    { label: t('ctx_copy_path'), icon: 'path', fn: () => act.copyPath([e]) },
    { label: t('ctx_make_shortcut'), icon: 'link', disabled: !w, fn: () => act.makeShortcut(e) },
    { label: t('ctx_send_desktop'), icon: 'desktop', fn: () => act.sendToDesktop(e) },
    { label: t('ctx_download'), icon: 'download', disabled: isDir, fn: () => act.download([e]) },
  );
  if (isDir) {
    items.push(
      extra.isFav
        ? { label: t('fav_remove'), icon: 'star', fn: () => act.toggleFavorite(e.path) }
        : { label: t('fav_add'), icon: 'star', fn: () => act.toggleFavorite(e.path) },
      { label: t('ctx_terminal'), icon: 'terminal', fn: () => act.openTerminal(e.path) },
    );
  }
  items.push({ sep: true }, { label: t('prop_title'), icon: 'info', shortcut: KEYS.props, fn: () => act.properties([e]) });
  return items;
}

// Several items selected.
export function multiMenu(m, sel) {
  const { t, act } = m;
  const w = !!m.writable;
  const files = sel.filter((e) => e.type !== 'dir');
  const free = sel.filter((e) => !e.protected);
  if (!m.sd) {
    return [
      { header: t('n_items', { count: sel.length }) },
      { label: t('ctx_copy_to_device'), icon: 'upload', disabled: !files.length, fn: () => act.copyToDevice() },
      { sep: true },
      { label: t('cut'), icon: 'cut', shortcut: KEYS.cut, disabled: !w || !files.length, fn: () => act.cut() },
      { label: t('copy'), icon: 'copy', shortcut: KEYS.copy, disabled: !files.length, fn: () => act.copy() },
      { sep: true },
      { label: t('ctx_delete_perm'), icon: 'delete', shortcut: KEYS.del, danger: true, disabled: !w, fn: () => act.del() },
      { sep: true },
      { label: t('prop_title'), icon: 'info', shortcut: KEYS.props, fn: () => act.properties(sel) },
    ];
  }
  return [
    { header: t('n_items', { count: sel.length }) },
    { label: t('ctx_open_all'), icon: 'open', fn: () => act.openAll(sel) },
    { sep: true },
    { label: t('cut'), icon: 'cut', shortcut: KEYS.cut, disabled: !w || !files.some((e) => !e.protected), fn: () => act.cut() },
    { label: t('copy'), icon: 'copy', shortcut: KEYS.copy, disabled: !files.length, fn: () => act.copy() },
    { label: t('duplicate'), icon: 'duplicate', shortcut: KEYS.dup, disabled: !w, fn: () => act.duplicate(sel) },
    { sep: true },
    { label: t('ctx_compress'), icon: 'archive', disabled: !w, sub: compressSub(m, sel) },
    { label: t('ctx_download'), icon: 'download', disabled: !files.length, fn: () => act.download(sel) },
    { label: t('ctx_copy_path'), icon: 'path', fn: () => act.copyPath(sel) },
    { sep: true },
    { label: t('delete'), icon: 'delete', shortcut: KEYS.del, disabled: !w || !free.length, fn: () => act.del() },
    { label: t('ctx_delete_perm'), icon: 'delete', shortcut: KEYS.delPerm, danger: true, disabled: !w || !free.length, fn: () => act.delPermanent() },
    { sep: true },
    { label: t('prop_title'), icon: 'info', shortcut: KEYS.props, fn: () => act.properties(sel) },
  ];
}

// The empty part of the list: the folder itself.
export function backgroundMenu(m) {
  const { t, act, prefs } = m;
  const w = !!m.writable;
  const view = (v, key, icon) => ({ label: t(key), icon, checked: prefs.view === v, fn: () => act.setView(v) });
  const sort = (k, key) => ({ label: t(key), checked: prefs.sortKey === k, fn: () => act.setSort(k, prefs.sortDir) });
  const items = [
    { label: t('new'), icon: 'file-new', disabled: !w || m.inSearch, sub: [
      { label: t('new_folder'), icon: 'folder-new', shortcut: KEYS.newFolder, fn: () => act.newFolder() },
      { sep: true },
      { label: t('new_text'), icon: 'file-new', disabled: !m.sd, fn: () => act.newFile('txt') },
      { label: t('new_markdown'), icon: 'file-new', disabled: !m.sd, fn: () => act.newFile('md') },
    ] },
    { label: t('paste'), icon: 'paste', shortcut: KEYS.paste, disabled: !w || !m.hasClip || m.inSearch, fn: () => act.paste() },
    { sep: true },
    { label: t('view'), icon: 'view', sub: [
      view('icons', 'view_icons', 'view'), view('list', 'view_list', 'list'), view('details', 'view_details', 'details'),
    ] },
    { label: t('sort_by'), icon: 'sort', sub: [
      sort('name', 'col_name'), sort('mtime', 'col_mtime'), sort('type', 'col_type'), sort('size', 'col_size'),
      { sep: true },
      { label: t('sort_asc'), checked: prefs.sortDir === 1, fn: () => act.setSort(prefs.sortKey, 1) },
      { label: t('sort_desc'), checked: prefs.sortDir === -1, fn: () => act.setSort(prefs.sortKey, -1) },
    ] },
    { label: t('show_hidden'), checked: !!prefs.showHidden, fn: () => act.toggleHidden() },
    { sep: true },
    { label: t('selectAll'), icon: 'select-all', shortcut: KEYS.all, fn: () => act.selectAll() },
    { label: t('refresh'), icon: 'refresh', shortcut: KEYS.refresh, fn: () => act.refresh() },
  ];
  if (m.sd) items.push({ label: t('ctx_terminal'), icon: 'terminal', disabled: m.inSearch, fn: () => act.openTerminal(m.cwd) });
  items.push({ sep: true }, { label: t('prop_folder_title'), icon: 'info', disabled: m.inSearch, fn: () => act.folderProperties() });
  return items;
}

// A folder in the sidebar (tree node, quick access, favourite) of the device.
export function placeMenu(m, path, { isFav = false, removable = false } = {}) {
  const { t, act } = m;
  return [
    { label: t('open'), icon: 'open', fn: () => act.goTo(path) },
    { label: t('ctx_new_window'), icon: 'open', fn: () => act.openNewWindow(path) },
    { sep: true },
    isFav || removable
      ? { label: t('fav_remove'), icon: 'star', fn: () => act.toggleFavorite(path) }
      : { label: t('fav_add'), icon: 'star', fn: () => act.toggleFavorite(path) },
    { label: t('ctx_paste_into'), icon: 'paste', disabled: !m.hasClip, fn: () => act.paste(path) },
    { label: t('ctx_terminal'), icon: 'terminal', fn: () => act.openTerminal(path) },
    { label: t('ctx_copy_path'), icon: 'path', fn: () => act.copyPath([{ path, name: path.split('/').pop() || '/', type: 'dir' }]) },
    { sep: true },
    { label: t('prop_title'), icon: 'info', fn: () => act.placeProperties(path) },
  ];
}
