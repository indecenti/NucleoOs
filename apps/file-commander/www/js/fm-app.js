/* SPDX-License-Identifier: AGPL-3.0-or-later */
// fm-app.js — File Commander, the OS's file manager: the controller that ties the modules together.
//   fm-types (what a file is) · fm-path (paths, address bar) · fm-state (sorting, history, prefs, words)
//   fm-search (search in subfolders) · fm-menus (right-click menus) · fm-ops (bin, undo, paste, duplicate)
//   fm-shortcuts (the shell's messages) · fm-local (this computer's folders) · fm-view (the list)
//   fm-sidebar (navigation pane) · fm-preview (preview pane) · fm-dialogs (Properties)
// The core (I18N, Caps, fs, os, menu, pack) comes from index.html, so nothing here imports from '/'.
import { extOf, isTextLike, isImage, iconFor, placeIcon } from './fm-types.js';
import { join, dirOf, nameOf, crumbs as crumbsOf, parseAddress, nameProblem, uniqueName } from './fm-path.js';
import { sortEntries, visibleEntries, createHistory, loadPrefs, savePrefs, formatSize, summarize, errorText,
  moveIndex, createTypeAhead, findByPrefix, hasMtime, isHidden } from './fm-state.js';
import { searchTree } from './fm-search.js';
import { itemMenu, multiMenu, backgroundMenu, placeMenu, fillOpenWith } from './fm-menus.js';
import { createOps } from './fm-ops.js';
import { handleShellMessage, FS_NOISE } from './fm-shortcuts.js';
import * as L from './fm-local.js';
import { createView } from './fm-view.js';
import { createSidebar } from './fm-sidebar.js';
import { createPreview } from './fm-preview.js';
import { showProperties } from './fm-dialogs.js';

const $ = (id) => document.getElementById(id);
const esc = (s) => String(s == null ? '' : s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const MAX_UPLOAD = 640 * 1024 * 1024;               // the devices' hard ceiling for one file
const TEXT_PREVIEW_MAX = 1024 * 1024;               // above this, a device without Range reads would send it whole
const store = (() => { try { return window.localStorage; } catch { return null; } })();
const memStore = { _m: new Map(), getItem(k) { return this._m.has(k) ? this._m.get(k) : null; }, setItem(k, v) { this._m.set(k, String(v)); } };

export async function boot({ I18N, Caps, fs, os, menu, ICONS, pack }) {
  const t = await I18N.init('file-commander');
  const locale = () => { try { return I18N.locale(); } catch { return undefined; } };
  const toast = (text, kind = 'info') => os.toast(text, { kind });
  for (const b of document.querySelectorAll('[data-icon]')) if (ICONS[b.dataset.icon]) b.insertAdjacentHTML('afterbegin', ICONS[b.dataset.icon]);

  // ── state ──
  const prefs = loadPrefs(store || memStore);
  const savePref = () => savePrefs(store || memStore, prefs);
  const SD = { id: 'sd', kind: 'sd', label: 'device' };
  const locals = [];
  const S = {
    loc: SD, cwd: '/data', entries: [], shown: [], sel: new Set(), cur: -1, anchor: -1,
    filter: '', search: null, loading: false, error: null, free: null,
  };
  const isSD = () => S.loc.kind === 'sd';
  const writable = () => (isSD() ? true : S.loc.writable === true);
  let lastSdPath = '/data';                         // the device folder a copy from this computer lands in
  let xfer = null;                                   // the app's own clipboard: { op, items:[{ name, key, read, del }] }
  const history = createHistory();
  const listCache = new Map();
  let loadSeq = 0;

  const ops = createOps({ fs, Caps, t, notify: toast, onUndo: () => syncCommands() });
  const list = $('list');
  const view = createView({ list, head: $('colhead'), t, locale, thumbUrl, onHeaderSort: (k) => act.setSort(k, prefs.sortKey === k ? -prefs.sortDir : 1) });
  const preview = createPreview($('preview'), { t, locale, imageUrl: thumbUrl, readHead });
  const sidebar = createSidebar($('tree'), {
    t,
    locations: () => [SD, ...locals],
    active: () => S.loc,
    segments: () => (isSD() ? S.cwd.split('/').filter(Boolean) : L.localSegments(S.loc)),
    cwd: () => S.cwd,
    favorites: () => prefs.favorites,
    listFolders: async (node) => {
      if (node.loc.kind === 'sd') return (await fs.list(node.path)).filter((e) => e.type === 'dir' && (prefs.showHidden || !isHidden(e.name)))
        .map((e) => ({ name: e.name, path: join(node.path, e.name), leaf: e.has_subdirs !== undefined ? !e.has_subdirs : false }));
      return L.listLocalFolders(node);
    },
    go: (node) => goNode(node),
    goPath: (p) => goTo(p),
    dropOn: (path, ev, probe) => dropOnFolder(path, ev, probe),
    removeLocation: (loc) => removeLocal(loc),
    localHint: () => (L.bridge ? t('side_scanning') : L.fsaSupported ? t('side_add_hint') : t('side_unsupported')),
    dataFolders: async () => new Set((await fs.list('/data')).filter((e) => e.type === 'dir').map((e) => e.name)),
    // The encrypted cloud archive is a drive of the core file layer (/r2) where the device has it: every action here
    // works on it through the same fs calls.
    drives: async () => ((await Caps.has('cloud.archive')) ? [{ path: '/r2', label: t('loc_cloud'), icon: 'cloud' }] : []),
  });

  // ── where we are ──
  const place = () => (isSD() ? { loc: 'sd', path: S.cwd } : { loc: S.loc.id, segs: L.localSegments(S.loc) });
  async function goPlace(p, { push = true, select = null } = {}) {
    if (!p) return;
    endSearch(false);
    if (p.loc === 'sd') { S.loc = SD; S.cwd = p.path; lastSdPath = p.path; }
    else {
      const loc = locals.find((l) => l.id === p.loc);
      if (!loc) return;
      S.loc = loc;
      try { await L.localGoSegments(loc, p.segs || []); } catch { L.localGoRoot(loc); }
    }
    if (push) history.push(place());
    S.sel.clear(); S.cur = -1; S.anchor = -1;
    if (S.filter) { S.filter = ''; $('find').value = ''; }
    closeDrawer();
    await load({ select });
  }
  const goTo = (path, opts) => goPlace({ loc: 'sd', path }, opts);
  async function goNode(node) {
    if (node.loc.kind === 'sd') return goTo('/' + node.segs.join('/'));
    return goPlace({ loc: node.loc.id, segs: node.segs });
  }
  async function up() {
    if (S.search) { endSearch(true); return; }
    if (isSD()) { if (S.cwd !== '/') goTo(dirOf(S.cwd), { select: nameOf(S.cwd) }); return; }
    if (L.localAtRoot(S.loc)) return;
    const segs = L.localSegments(S.loc);
    goPlace({ loc: S.loc.id, segs: segs.slice(0, -1) }, { select: segs[segs.length - 1] });
  }
  const back = () => { if (history.canBack) goPlace(history.back(), { push: false }); };
  const forward = () => { if (history.canForward) goPlace(history.forward(), { push: false }); };

  // ── listing ──
  const cacheKey = () => (isSD() ? 'sd:' + S.cwd : S.loc.id + ':' + L.localSegments(S.loc).join('/'));
  const sig = (es) => es.map((e) => (e.type === 'dir' ? 'd' : 'f') + e.name + ':' + (e.size || 0) + ':' + (e.mtime || '') + (e.protected ? 'p' : '')).join('|');
  async function listSD(dir) {
    const all = await fs.list(dir);
    // A ".gz" twin is how the device serves <name> compressed: an internal file, not the user's. A lone .gz stays.
    const names = new Set(all.map((e) => e.name));
    return all.filter((e) => !(e.type !== 'dir' && /\.gz$/i.test(e.name) && names.has(e.name.slice(0, -3))));
  }
  const decorate = (e) => (isSD() ? { ...e, path: join(S.cwd, e.name), key: join(S.cwd, e.name) } : { ...e, path: L.localFullPath(S.loc, e.name), key: 'L:' + e.name });
  // Stale-while-revalidate: the cached listing paints at once, the device is asked in the background, and the
  // list is reconciled only when the answer differs. A newer navigation wins over a slow older answer.
  async function load({ select = null } = {}) {
    renderCrumbs();
    syncCommands();
    const key = cacheKey();
    const cached = listCache.get(key);
    const seq = ++loadSeq;
    S.error = null;
    if (cached) { S.entries = cached.map(decorate); refresh({ select }); }
    else { S.loading = true; S.entries = []; S.shown = []; S.cur = -1; view.skeleton(); paintStatus(); if (prefs.preview) preview.show([]); }
    sidebar.reveal();
    os.setTitle(isSD() ? (S.cwd === '/' ? t('loc_device') : nameOf(S.cwd)) : S.loc.label);
    let raw;
    try { raw = isSD() ? await listSD(S.cwd) : await L.listLocal(S.loc); }
    catch (err) {
      if (seq !== loadSeq) return;
      S.loading = false;
      if (!cached) { S.error = err; S.entries = []; refresh(); }
      return;
    }
    if (seq !== loadSeq) return;
    S.loading = false;
    const changed = !cached || sig(raw) !== sig(cached);
    listCache.set(key, raw.slice());
    if (changed) { S.entries = raw.map(decorate); refresh({ select: cached ? null : select }); }
    else paintStatus();
    if (isSD()) refreshFree();
  }
  const reload = () => load();

  // The rows the user sees: the folder's entries (or search results), hidden ones and the filter applied, sorted.
  function computeShown() {
    const src = S.search ? S.search.results : S.entries;
    const vis = visibleEntries(src, { showHidden: prefs.showHidden, filter: S.search ? '' : S.filter });
    return sortEntries(vis, { key: prefs.sortKey, dir: prefs.sortDir });
  }
  function refresh({ select = null } = {}) {
    const prevKey = S.shown[S.cur] ? S.shown[S.cur].key : null;
    S.shown = computeShown();
    const keys = new Set(S.shown.map((e) => e.key));
    for (const k of [...S.sel]) if (!keys.has(k)) S.sel.delete(k);
    if (select) {
      const want = Array.isArray(select) ? select : [select];
      const hit = S.shown.filter((e) => want.includes(e.name) || want.includes(e.key));
      if (hit.length) { S.sel = new Set(hit.map((e) => e.key)); S.cur = S.shown.indexOf(hit[0]); S.anchor = S.cur; }
    } else S.cur = prevKey ? S.shown.findIndex((e) => e.key === prevKey) : Math.min(S.cur, S.shown.length - 1);
    view.setMode(prefs.view);
    if (!S.shown.length) showBlock();
    else view.render(S.shown, viewState({ cols: { folder: !!S.search, mtime: hasMtime(S.shown) } }));
    if (S.cur >= 0) view.reveal(S.cur);
    paintStatus();
    syncCommands();
    if (prefs.preview) preview.show(selectedEntries());
  }
  const viewState = (extra = {}) => ({ selected: S.sel, cursor: S.cur, sortKey: prefs.sortKey, sortDir: prefs.sortDir,
    cutKeys: new Set(xfer && xfer.op === 'cut' ? xfer.items.map((i) => i.key) : []), ...extra });
  function paintSel() {
    view.paint(viewState());
    paintStatus();
    syncCommands();
    if (prefs.preview) preview.show(selectedEntries());
    const one = selectedEntries();
    if (one.length === 1 && one[0].type !== 'dir' && isSD()) handlersFor(one[0]);   // the "Open with" list, ready for the menu
  }
  const selectedEntries = () => S.shown.filter((e) => S.sel.has(e.key));

  function showBlock() {
    const btn = (actName, label) => `<button type="button" data-act="${actName}">${esc(label)}</button>`;
    let icon = iconFor('', true), text, acts = '';
    if (S.error) {
      icon = ICONS.info;
      text = t('cannot_read') + ' ' + errorText(S.error, t);
      acts = btn('retry', t('retry'));
    } else if (S.search && !S.search.running) {
      icon = ICONS.search; text = t('search_none', { q: S.search.query }); acts = btn('closeSearch', t('search_close'));
    } else if (S.search) {
      text = t('searching_short');
    } else if (S.filter) {
      icon = ICONS.search; text = t('no_match', { q: S.filter }); acts = btn('clearFilter', t('filter_clear'));
    } else if (S.entries.length) {
      text = t('only_hidden', { count: S.entries.length }); acts = btn('showHidden', t('show_hidden'));
    } else {
      text = isSD() ? t('empty_folder') : t('empty_folder_local');
      if (writable()) acts = btn('newFolder', t('new_folder'));
    }
    view.showBlock(S.error ? 'error' : 'empty', `<div class="big">${icon}</div><p>${esc(text)}</p>${acts ? `<div class="acts">${acts}</div>` : ''}`);
  }

  // ── status bar and commands ──
  function paintStatus() {
    const n = S.shown.length;
    $('st-count').textContent = S.loading && !n ? t('loading') : t('count_items', { count: n }) + (S.filter && !S.search ? ' ' + t('count_filtered') : '');
    const sel = selectedEntries();
    if (sel.length) {
      const s = summarize(sel);
      $('st-sel').textContent = s.files ? t('count_selected_size', { count: sel.length, size: formatSize(s.bytes, locale()) }) : t('count_selected', { count: sel.length });
    } else $('st-sel').textContent = '';
    $('st-free').textContent = isSD() && S.free != null ? t('free_space', { size: formatSize(S.free, locale()) }) : '';
  }
  let freeTimer = 0;
  function refreshFree() { clearTimeout(freeTimer); freeTimer = setTimeout(async () => { const f = await fs.free().catch(() => null); if (f != null) { S.free = f; paintStatus(); } }, 300); }
  fs.onChange && fs.onChange(() => refreshFree());

  function syncCommands() {
    const sel = selectedEntries();
    const w = writable();
    const files = sel.filter((e) => e.type !== 'dir');
    const one = sel.length === 1 ? sel[0] : null;
    $('c-new').disabled = !w || !!S.search;
    $('c-cut').disabled = !w || !files.some((e) => !e.protected);
    $('c-copy').disabled = !files.length;
    $('c-paste').disabled = !w || (!xfer && !isSD()) || !!S.search;
    $('c-rename').disabled = !one || !w || one.protected || (!isSD() && !L.canRenameLocal(S.loc, one));
    $('c-delete').disabled = !w || !sel.length || (isSD() && sel.every((e) => e.protected));
    $('c-undo').hidden = !(isSD() && ops.canUndo);
    $('c-tosd').hidden = isSD();
    $('c-tosd').disabled = !files.length;
    $('c-preview').setAttribute('aria-pressed', String(!!prefs.preview));
    $('b-back').disabled = !history.canBack;
    $('b-fwd').disabled = !history.canForward;
    $('b-up').disabled = !S.search && (isSD() ? S.cwd === '/' : L.localAtRoot(S.loc));
    const deep = $('b-deep');
    deep.setAttribute('aria-pressed', String(!!prefs.searchDeep));
    deep.disabled = !isSD();
  }

  // ── breadcrumbs and the editable address ──
  const addr = $('addr'), addrIn = $('addr-in'), crumbsEl = $('crumbs');
  const SEP = '<span class="sepc" aria-hidden="true"><svg viewBox="0 0 10 10" width="10" height="10"><path d="M3.5 2l3 3-3 3" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"/></svg></span>';
  function renderCrumbs() {
    if (S.search) {
      crumbsEl.innerHTML = `<span class="static">${esc(t('search_crumb', { q: S.search.query, folder: S.search.root }))}</span><span class="fill"></span>`;
      return;
    }
    if (isSD()) {
      const cs = crumbsOf(S.cwd);
      const shown = cs.length > 5 ? [cs[0], null, ...cs.slice(-3)] : cs;
      crumbsEl.innerHTML = shown.map((c, i) => {
        if (!c) return SEP + '<span class="ell">…</span>';
        const label = c.path === '/' ? `${placeIcon('device')}<span>${esc(t('loc_device'))}</span>` : esc(c.name);
        return (i ? SEP : '') + `<button type="button" class="seg${i === shown.length - 1 ? ' last' : ''}" data-p="${esc(c.path)}" title="${esc(c.path)}">${label}</button>`;
      }).join('') + '<span class="fill"></span>';
      return;
    }
    const tail = L.localTail(S.loc);
    crumbsEl.innerHTML = `<button type="button" class="seg" data-root="1">${placeIcon(S.loc.icon || 'folder')}<span>${esc(S.loc.label)}</span></button>` +
      (tail ? SEP + `<span class="static">${esc(tail)}</span>` : '') + '<span class="fill"></span>';
  }
  crumbsEl.addEventListener('click', (ev) => {
    const seg = ev.target.closest('.seg');
    if (seg) { ev.stopPropagation(); if (seg.dataset.root) goPlace({ loc: S.loc.id, segs: [] }); else goTo(seg.dataset.p); return; }
  });
  addr.addEventListener('click', (ev) => { if (!ev.target.closest('.seg') && addrIn.hidden) editAddress(); });
  crumbsEl.addEventListener('dragover', (ev) => { const seg = ev.target.closest('.seg[data-p]'); if (seg && dropOnFolder(seg.dataset.p, ev, true)) ev.preventDefault(); });
  crumbsEl.addEventListener('drop', (ev) => { const seg = ev.target.closest('.seg[data-p]'); if (seg && dropOnFolder(seg.dataset.p, ev, false)) ev.preventDefault(); });
  function editAddress() {
    crumbsEl.hidden = true;
    addrIn.hidden = false;
    addr.classList.add('editing');
    addrIn.value = S.search ? S.search.root : isSD() ? S.cwd : S.loc.label + (L.localTail(S.loc) ? '\\' + L.localTail(S.loc) : '');
    addrIn.focus();
    addrIn.select();
  }
  function endAddress(focusList = true) {
    addrIn.hidden = true;
    crumbsEl.hidden = false;
    addr.classList.remove('editing', 'bad');
    if (focusList) list.focus({ preventScroll: true });
  }
  addrIn.addEventListener('keydown', async (ev) => {
    if (ev.key === 'Escape') { ev.preventDefault(); ev.stopPropagation(); endAddress(); return; }
    if (ev.key !== 'Enter') return;
    ev.preventDefault();
    const r = parseAddress(addrIn.value, isSD() ? S.cwd : '/data');
    if (!r.ok) { addr.classList.add('bad'); toast(t('addr_' + r.reason), 'warn'); return; }
    let st = null;
    try { st = await fs.stat(r.path); } catch {}
    if (!st) { addr.classList.add('bad'); toast(t('addr_missing', { path: r.path }), 'warn'); return; }
    endAddress();
    if (st.type === 'dir') goTo(r.path);
    else goTo(dirOf(r.path), { select: nameOf(r.path) });
  });
  addrIn.addEventListener('input', () => addr.classList.remove('bad'));
  addrIn.addEventListener('blur', () => { if (!addrIn.hidden) endAddress(false); });

  // ── selection with mouse and touch ──
  function selectOnly(i) { S.sel = new Set(S.shown[i] ? [S.shown[i].key] : []); S.cur = i; S.anchor = i; }
  function selectRange(i, add = false) {
    const a = S.anchor >= 0 ? S.anchor : i;
    if (!add) S.sel.clear();
    for (let k = Math.min(a, i); k <= Math.max(a, i); k++) S.sel.add(S.shown[k].key);
    S.cur = i;
  }
  list.addEventListener('click', (ev) => {
    const b = ev.target.closest('[data-act]');
    if (b) { blockAction(b.dataset.act); return; }
    const i = view.indexAt(ev.target);
    if (i < 0) { if (!ev.ctrlKey && !ev.shiftKey && S.sel.size) { S.sel.clear(); paintSel(); } return; }
    const e = S.shown[i];
    if (ev.shiftKey) selectRange(i, ev.ctrlKey || ev.metaKey);
    else if (ev.ctrlKey || ev.metaKey) { S.sel.has(e.key) ? S.sel.delete(e.key) : S.sel.add(e.key); S.cur = i; S.anchor = i; }
    else selectOnly(i);
    paintSel();
  });
  list.addEventListener('dblclick', (ev) => { const i = view.indexAt(ev.target); if (i >= 0) act.open(S.shown[i]); });
  function blockAction(a) {
    if (a === 'retry') reload();
    else if (a === 'newFolder') act.newFolder();
    else if (a === 'clearFilter') { S.filter = ''; $('find').value = ''; refresh(); }
    else if (a === 'closeSearch') endSearch(true);
    else if (a === 'showHidden') act.toggleHidden();
  }

  // ── keyboard ──
  const typeAhead = createTypeAhead();
  const inField = (el) => !!el && (el.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(el.tagName || ''));
  function moveTo(i, ev) {
    if (i < 0 || !S.shown.length) return;
    if (ev && ev.shiftKey) selectRange(i);
    else if (ev && (ev.ctrlKey || ev.metaKey)) S.cur = i;
    else selectOnly(i);
    paintSel();
    view.reveal(i);
  }
  document.addEventListener('keydown', (ev) => {
    if (ev.defaultPrevented || menu.isOpen || document.querySelector('.scrim')) return;
    const k = ev.key, ctrl = ev.ctrlKey || ev.metaKey;
    const take = () => { ev.preventDefault(); ev.stopPropagation(); };
    // anywhere in the window
    if (ev.altKey && !ctrl && k === 'ArrowLeft') { take(); back(); return; }
    if (ev.altKey && !ctrl && k === 'ArrowRight') { take(); forward(); return; }
    if (ev.altKey && !ctrl && k === 'ArrowUp') { take(); up(); return; }
    if (ev.altKey && !ctrl && (k === 'p' || k === 'P')) { take(); act.togglePreview(); return; }
    if ((ctrl && (k === 'l' || k === 'L')) || k === 'F4') { take(); editAddress(); return; }
    if ((ctrl && (k === 'f' || k === 'F')) || k === 'F3') { take(); $('find').focus(); $('find').select(); return; }
    if (ctrl && ev.shiftKey && (k === 'n' || k === 'N')) { take(); act.newFolder(); return; }
    if (inField(ev.target)) return;
    if (ev.target.closest && ev.target.closest('#tree')) return;          // the tree has its own keys
    const ae = document.activeElement;
    const onList = !ae || ae === list || ae === document.body || ae === document.documentElement;
    if (ev.altKey && k === 'Enter') { take(); act.properties(selectedEntries().length ? selectedEntries() : null); return; }
    if (k === 'Delete' && ev.shiftKey) { take(); act.delPermanent(); return; }   // before the shell sees a plain Delete
    if (ctrl && (k === 'd' || k === 'D')) { take(); act.duplicate(selectedEntries()); return; }
    if (ctrl && k === ' ') { take(); const e = S.shown[S.cur]; if (e) { S.sel.has(e.key) ? S.sel.delete(e.key) : S.sel.add(e.key); S.anchor = S.cur; paintSel(); } return; }
    if (!onList) return;
    if (k === 'Backspace') { take(); up(); return; }
    if (k === 'Escape') { if (S.search) { take(); endSearch(true); } else if (S.sel.size) { take(); S.sel.clear(); paintSel(); } return; }
    if (k === 'Enter') { take(); const sel = selectedEntries(); if (sel.length === 1) act.open(sel[0]); else if (sel.length > 1) act.openAll(sel); return; }
    if (['ArrowDown', 'ArrowUp', 'ArrowLeft', 'ArrowRight', 'Home', 'End', 'PageUp', 'PageDown'].includes(k)) {
      take();
      if (document.activeElement !== list) list.focus({ preventScroll: true });
      moveTo(moveIndex(S.cur, k, { count: S.shown.length, cols: view.perRow(), page: view.pageRows() }), ev);
      return;
    }
    if (k === ' ' && !ctrl) { take(); if (S.cur >= 0 && !S.sel.has(S.shown[S.cur].key)) { S.sel.add(S.shown[S.cur].key); paintSel(); } else if (S.cur < 0) moveTo(0); return; }
    // Outside the shell nobody turns these into os-shortcut messages: handle them here.
    if (!os.inShell) {
      const sc = ctrl && !ev.shiftKey ? { c: 'copy', x: 'cut', v: 'paste', a: 'selectAll', z: 'undo' }[k.toLowerCase()] : k === 'F2' ? 'rename' : k === 'Delete' ? 'delete' : k === 'F5' ? 'refresh' : null;
      if (sc) { take(); shellMessage({ type: 'os-shortcut', action: sc }); return; }
    }
    if (k.length === 1 && !ctrl && !ev.altKey && /\S/.test(k)) {
      const buf = typeAhead.feed(k);
      const i = findByPrefix(S.shown.map((e) => e.name), buf, S.cur);
      if (i >= 0) { ev.preventDefault(); moveTo(i); }
    }
  });
  // The mouse's back and forward buttons.
  document.addEventListener('mouseup', (ev) => { if (ev.button === 3) { ev.preventDefault(); back(); } else if (ev.button === 4) { ev.preventDefault(); forward(); } });
  document.addEventListener('auxclick', (ev) => { if (ev.button === 3 || ev.button === 4) ev.preventDefault(); });

  // ── "Open with": the OS's list for the type, asked once per extension and kept a little while ──
  const handlerCache = new Map();
  function handlersFor(e) {
    const k = extOf(e.name);
    const hit = handlerCache.get(k);
    if (hit && Date.now() - hit.at < 30000) return hit;
    const rec = { at: Date.now(), list: null, promise: null };
    rec.promise = Promise.resolve(os.handlers(e.path)).then((l) => { rec.list = Array.isArray(l) ? l : []; return rec.list; }, () => { rec.list = []; return []; });
    handlerCache.set(k, rec);
    return rec;
  }

  // ── menus ──
  const mctx = () => ({ t, act, isArchive: pack.isArchive, packFormats: pack.formats.write, sd: isSD(), writable: writable(), hasClip: !!xfer || isSD(), inSearch: !!S.search, prefs, cwd: S.cwd });
  function menuForList(target, kbd = false) {
    const i = view.indexAt(target);
    if (i < 0) {
      if (target && target.closest && target.closest('.row')) return null;
      if (kbd && S.sel.size) {                                     // the Menu key on the list: the selection's menu
        const sel = selectedEntries();
        return sel.length > 1 ? multiMenu(mctx(), sel) : entryMenu(sel[0]);
      }
      if (S.sel.size) { S.sel.clear(); paintSel(); }
      return backgroundMenu(mctx());
    }
    const e = S.shown[i];
    if (!S.sel.has(e.key)) { selectOnly(i); paintSel(); }
    const sel = selectedEntries();
    return sel.length > 1 ? multiMenu(mctx(), sel) : entryMenu(e);
  }
  function entryMenu(e) {
    const m = mctx();
    if (!m.sd || e.type === 'dir') return itemMenu(m, e, { isFav: prefs.favorites.includes(e.path) });
    const rec = handlersFor(e);
    const sub = [];
    fillOpenWith(sub, m, e, rec.list);
    if (!rec.list) rec.promise.then((l) => fillOpenWith(sub, m, e, l));
    const def = rec.list && rec.list.find((h) => h.isDefault);
    return itemMenu(m, e, { sub, defaultName: def ? def.name : '', isFav: false });
  }
  // Our own Menu key / Shift+F10 handler first: the menu opens at the focused row, not at the list's corner.
  list.addEventListener('keydown', (ev) => {
    if (ev.key !== 'ContextMenu' && !(ev.key === 'F10' && ev.shiftKey)) return;
    ev.preventDefault(); ev.stopImmediatePropagation();
    const e = S.shown[S.cur];
    const row = e && view.rowOf(e.key);
    const r = row ? row.getBoundingClientRect() : list.getBoundingClientRect();
    const items = menuForList(row || list, true);
    if (items) menu.open(r.left + 24, row ? r.bottom - 4 : r.top + 8, items, { focusFirst: true });
  });
  menu.attach(list, (ev) => menuForList(ev.target));
  menu.attach($('tree'), (ev) => {
    const n = ev.target.closest && ev.target.closest('.tnode');
    if (!n) return null;
    if (n.dataset.path) return placeMenu(mctx(), n.dataset.path, { isFav: n.dataset.kind === 'fav' || prefs.favorites.includes(n.dataset.path) });
    if (n._node && n._node.isRoot && n._node.loc.removable) return [{ label: t('open'), icon: 'open', fn: () => goNode(n._node) }, { sep: true }, { label: t('tx_remove'), icon: 'close', fn: () => removeLocal(n._node.loc) }];
    if (n._node) return [{ label: t('open'), icon: 'open', fn: () => goNode(n._node) }];
    return null;
  });
  const openAt = (btn, items) => { const r = btn.getBoundingClientRect(); menu.open(r.left, r.bottom + 2, items, { focusFirst: document.activeElement === btn }); };
  const bgSub = (label) => (backgroundMenu(mctx()).find((x) => x.label === label) || {}).sub || [];
  $('c-new').addEventListener('click', (ev) => openAt(ev.currentTarget, bgSub(t('new'))));
  $('c-sort').addEventListener('click', (ev) => openAt(ev.currentTarget, bgSub(t('sort_by'))));
  $('c-view').addEventListener('click', (ev) => openAt(ev.currentTarget, [...bgSub(t('view')), { sep: true },
    { label: t('show_hidden'), checked: !!prefs.showHidden, fn: () => act.toggleHidden() },
    { label: t('preview_pane'), checked: !!prefs.preview, shortcut: 'Alt+P', fn: () => act.togglePreview() }]));
  $('c-more').addEventListener('click', (ev) => { const sel = selectedEntries(); openAt(ev.currentTarget, sel.length > 1 ? multiMenu(mctx(), sel) : sel.length ? entryMenu(sel[0]) : backgroundMenu(mctx())); });
  $('c-cut').addEventListener('click', () => act.cut());
  $('c-copy').addEventListener('click', () => act.copy());
  $('c-paste').addEventListener('click', () => act.paste());
  $('c-rename').addEventListener('click', () => act.rename());
  $('c-delete').addEventListener('click', () => act.del());
  $('c-undo').addEventListener('click', () => act.undo());
  $('c-tosd').addEventListener('click', () => act.copyToDevice());
  $('c-preview').addEventListener('click', () => act.togglePreview());
  $('b-back').addEventListener('click', back);
  $('b-fwd').addEventListener('click', forward);
  $('b-up').addEventListener('click', up);
  $('b-refresh').addEventListener('click', () => act.refresh());

  // ── the app's clipboard (device ⇄ this computer), mirrored to the OS clipboard for device files ──
  async function copyCut(op) {
    let sel = selectedEntries().filter((e) => e.type !== 'dir');
    if (op === 'cut') {
      if (!writable()) return;
      const n = sel.length; sel = sel.filter((e) => !e.protected);
      if (sel.length < n) toast(t('lock_no_move'), 'warn');
    }
    if (!sel.length) { if (selectedEntries().some((e) => e.type === 'dir')) toast(t('copy_folder_hint')); return; }
    const items = [];
    if (isSD()) for (const e of sel) items.push({ name: e.name, key: e.key, path: e.path, read: () => readBlob(e.path), del: () => ops.toTrash(e.path, false) });
    else for (const e of sel) { try { const r = await L.localReader(S.loc, e.name); items.push({ name: e.name, key: e.key, ...r }); } catch {} }
    xfer = { op, items };
    if (isSD()) os.copy({ op, paths: items.map((i) => i.path) }, 'files');
    paintSel();
    toast(t(op === 'cut' ? 'toast_cut' : 'toast_copied', { count: items.length }));
  }
  async function readBlob(p) {
    const b = await ops.guard(fs.read(p));
    if (!b) throw Object.assign(new Error('not-found'), { code: 'not-found' });
    return new Blob([b]);
  }
  async function paste(targetDir = null, clip = undefined) {
    if (!writable()) return;
    if (xfer) return pasteXfer(targetDir);
    if (!isSD()) return;
    const c = clip !== undefined && clip !== null ? clip : await os.paste();
    if (!c || c.kind !== 'files') { toast(t('paste_empty')); return; }
    const dest = targetDir || S.cwd;
    const r = await ops.pasteFiles(c, dest);
    await reload();
    if (!targetDir) refresh({ select: [...r.created, ...r.moved.map((m) => m.dst)].map(nameOf) });
  }
  async function pasteXfer(targetDir) {
    const { op, items } = xfer;
    const landed = new Set();
    if (isSD()) {
      const dest = targetDir || S.cwd;
      let taken = new Set(); try { taken = new Set((await fs.list(dest)).map((e) => e.name)); } catch {}
      const jobs = items.map((it) => ({ label: it.name, dest: join(dest, uniqueName(it.name, taken)), read: it.read, item: it }));
      const done = await runUploadJobs(jobs);
      for (const j of done) landed.add(j.item);
      if (done.length) ops.pushUndo({ type: 'paste', created: done.map((j) => j.dest) });
    } else {
      let taken = new Set(); try { taken = new Set((await L.listLocal(S.loc)).map((e) => e.name)); } catch {}
      for (const it of items) { try { await L.localWriteFile(S.loc, uniqueName(it.name, taken), await it.read()); landed.add(it); } catch {} }
    }
    const failed = items.filter((it) => !landed.has(it));
    if (op === 'cut') {
      // A move deletes a source ONLY when its copy landed; a failed one keeps its source and stays on the clipboard.
      let kept = failed.length;
      for (const it of items) { if (!landed.has(it)) continue; try { await it.del(); } catch { kept++; } }
      xfer = failed.length ? { op, items: failed } : null;
      if (kept) toast(t('err_move_kept', { count: kept }), 'error');
    } else if (failed.length) toast(t('err_copy_failed', { count: failed.length }), 'error');
    await reload();
  }

  // ── uploads (files dropped from the computer, copies to the device) ──
  const upPanel = $('uploads'), upBody = $('up-body');
  let uploading = false;
  function upRow(label) {
    const row = document.createElement('div');
    row.className = 'uitem';
    row.innerHTML = `<div class="top"><span class="nm">${esc(label)}</span><span class="pc">0%</span></div><div class="bar"><i></i></div>`;
    upBody.appendChild(row); upBody.scrollTop = upBody.scrollHeight;
    const bar = row.querySelector('i'), pc = row.querySelector('.pc');
    return {
      progress: (l, tot) => { const r = tot ? Math.min(1, l / tot) : 0; bar.style.transform = 'scaleX(' + r.toFixed(3) + ')'; pc.textContent = Math.round(r * 100) + '%'; },
      done: () => { row.classList.add('done'); bar.style.transform = 'scaleX(1)'; pc.textContent = '100%'; },
      fail: (msg) => { row.classList.add('err'); pc.textContent = t('up_failed'); row.querySelector('.nm').title = msg; },
    };
  }
  // Jobs { file | read(), dest, label } one after the other, with progress; → the jobs that landed.
  async function runUploadJobs(jobs) {
    if (!jobs.length) return [];
    const landed = [];
    const title = $('up-title');
    upPanel.hidden = false;
    uploading = true;
    let ok = 0, failed = 0, done = 0;
    title.textContent = t('up_count', { done: 0, total: jobs.length });
    for (const job of jobs) {
      const ui = upRow(job.label);
      try {
        const file = job.file || (job.read && await job.read());
        if (file.size > MAX_UPLOAD) { ui.fail(t('up_too_large', { size: formatSize(file.size, locale()) })); failed++; }
        else { await fs.write(job.dest, file, { onProgress: ui.progress }); ui.done(); ok++; landed.push(job); }
      } catch (err) { ui.fail(errorText(err, t)); failed++; }
      title.textContent = t('up_count', { done: ++done, total: jobs.length });
    }
    uploading = false;
    title.textContent = failed ? t('up_done_failed', { ok, failed }) : t('up_done', { ok });
    if (isSD()) reload();
    return landed;
  }
  // One archive job in the transfers panel: run(onProgress) → the name to select afterwards (or `select`).
  // One archive job in the transfers panel: run(onProgress) → the name to select afterwards (or `select`).
  // → true when done; the error's code when it is one of opts.quietCodes (the caller handles it, e.g. a password).
  async function zipJob(label, run, select, { quietCodes = [] } = {}) {
    upPanel.hidden = false;
    const ui = upRow(label);
    $('up-title').textContent = label;
    uploading = true;
    try {
      const got = await run(ui.progress);
      ui.done();
      $('up-title').textContent = t('zip_done', { name: select || got || '' });
      await reload();
      refresh({ select: select || got || null });
      return true;
    } catch (err) {
      const k = pack.messageKey(err);
      const msg = k ? t(k) : errorText(err, t);
      ui.fail(msg);
      $('up-title').textContent = t('up_done_failed', { ok: 0, failed: 1 });
      if (quietCodes.includes(err && err.code)) return err.code;
      toast(msg, 'error');
      await reload();
      return false;
    } finally { uploading = false; }
  }
  $('up-clear').addEventListener('click', () => { if (uploading) return; upBody.textContent = ''; upPanel.hidden = true; });
  async function collect(dt) {
    const out = [];
    const items = dt.items ? [...dt.items] : [];
    if (items.length && items[0].webkitGetAsEntry) {
      const walk = (entry, prefix) => new Promise((res) => {
        if (entry.isFile) entry.file((f) => { out.push({ file: f, rel: prefix + entry.name }); res(); }, () => res());
        else if (entry.isDirectory) {
          const rd = entry.createReader(); const acc = [];
          const batch = () => rd.readEntries(async (es) => {
            if (!es.length) { for (const c of acc) await walk(c, prefix + entry.name + '/'); res(); }
            else { acc.push(...es); batch(); }
          }, () => res());
          batch();
        } else res();
      });
      for (const r of items.map((it) => it.webkitGetAsEntry && it.webkitGetAsEntry()).filter(Boolean)) await walk(r, '');
    }
    if (!out.length) for (const f of dt.files || []) out.push({ file: f, rel: f.name });
    return out;
  }
  async function uploadDrop(dt, destDir) {
    const dropped = await collect(dt);
    if (!dropped.length) return;
    let taken = new Set(); try { taken = new Set((await fs.list(destDir)).map((e) => e.name)); } catch {}
    const jobs = dropped.map(({ file, rel }) => {
      const segs = rel.split('/').filter(Boolean);
      segs[0] = uniqueName(segs[0], taken);
      return { file, dest: join(destDir, segs.join('/')), label: segs.join('/') };
    });
    await runUploadJobs(jobs);
  }
  async function copyToDevice() {
    if (isSD()) return;
    const files = selectedEntries().filter((e) => e.type !== 'dir');
    if (!files.length) return;
    const dest = lastSdPath;
    let taken = new Set(); try { taken = new Set((await fs.list(dest)).map((e) => e.name)); } catch {}
    const loc = S.loc;
    await runUploadJobs(files.map((e) => ({ label: t('to_device_label', { name: e.name, path: dest }), dest: join(dest, uniqueName(e.name, taken)), read: () => L.localReadFile(loc, e.name) })));
  }
  // ── drag and drop ──
  const DT_SD = 'application/x-nucleo-sd', DT_LOCAL = 'application/x-nucleo-local';
  const hasType = (ev, ty) => !!ev.dataTransfer && [...ev.dataTransfer.types].includes(ty);
  list.addEventListener('dragstart', (ev) => {
    const i = view.indexAt(ev.target);
    if (i < 0) return;
    const e = S.shown[i];
    if (!S.sel.has(e.key)) { selectOnly(i); paintSel(); }
    const sel = selectedEntries();
    if (isSD()) {
      const items = sel.filter((x) => !x.protected).map((x) => ({ path: x.path, name: x.name, isDir: x.type === 'dir' }));
      if (!items.length) { ev.preventDefault(); return; }
      ev.dataTransfer.setData(DT_SD, JSON.stringify({ items, srcDir: S.cwd }));
      ev.dataTransfer.effectAllowed = 'all';
      try { window.parent.postMessage({ type: 'fc-drag-start', items }, '*'); } catch {}
    } else {
      if (!sel.some((x) => x.type !== 'dir')) { ev.preventDefault(); return; }
      ev.dataTransfer.setData(DT_LOCAL, '1');
      ev.dataTransfer.effectAllowed = 'copy';
    }
    const row = view.rowOf(e.key); if (row) row.classList.add('dragging');
  });
  list.addEventListener('dragend', () => {
    for (const r of list.querySelectorAll('.dragging')) r.classList.remove('dragging');
    if (isSD()) { try { window.parent.postMessage({ type: 'fc-drag-end' }, '*'); } catch {} }
  });
  // A device folder as a drop target (rows, tree, breadcrumbs): device items move (Ctrl copies, Alt makes a
  // shortcut), this computer's files are copied to it. probe=true only asks whether it would accept.
  function dropOnFolder(path, ev, probe) {
    if (hasType(ev, DT_SD)) {
      if (probe) { ev.dataTransfer.dropEffect = ev.ctrlKey ? 'copy' : ev.altKey ? 'link' : 'move'; return true; }
      let payload; try { payload = JSON.parse(ev.dataTransfer.getData(DT_SD)); } catch { return true; }
      ops.dropInto(path, payload.items || [], ev.ctrlKey ? 'copy' : ev.altKey ? 'shortcut' : 'move').then((n) => { if (n) reload(); });
      return true;
    }
    if (hasType(ev, DT_LOCAL)) { if (!probe) { lastSdPath = path; copyToDevice(); } return true; }
    if (hasType(ev, 'Files') && !probe) { ev.stopPropagation(); hideDrop(); uploadDrop(ev.dataTransfer, path); return true; }
    return hasType(ev, 'Files');
  }
  list.addEventListener('dragover', (ev) => {
    const i = view.indexAt(ev.target);
    for (const r of list.querySelectorAll('.drop-target')) r.classList.remove('drop-target');
    const e = S.shown[i];
    if (!e || e.type !== 'dir' || !isSD() || S.sel.has(e.key) && hasType(ev, DT_SD)) return;
    if (dropOnFolder(e.path, ev, true)) { ev.preventDefault(); const r = view.rowOf(e.key); if (r) r.classList.add('drop-target'); }
  });
  list.addEventListener('dragleave', (ev) => { const r = ev.target.closest && ev.target.closest('.row'); if (r) r.classList.remove('drop-target'); });
  list.addEventListener('drop', (ev) => {
    for (const r of list.querySelectorAll('.drop-target')) r.classList.remove('drop-target');
    const e = S.shown[view.indexAt(ev.target)];
    if (e && e.type === 'dir' && isSD() && dropOnFolder(e.path, ev, false)) ev.preventDefault();
  });
  // Files from the computer anywhere on the window: uploaded into the folder shown.
  const dropEl = $('drop');
  let dragDepth = 0;
  const hideDrop = () => { dragDepth = 0; dropEl.hidden = true; };
  const filesDrag = (ev) => isSD() && !S.search && hasType(ev, 'Files');
  window.addEventListener('dragenter', (ev) => { if (!filesDrag(ev)) return; ev.preventDefault(); dragDepth++; $('dropdest').textContent = t('drop_dest', { dest: S.cwd }); dropEl.hidden = false; });
  window.addEventListener('dragover', (ev) => { if (filesDrag(ev)) { ev.preventDefault(); ev.dataTransfer.dropEffect = 'copy'; } });
  window.addEventListener('dragleave', () => { if (--dragDepth <= 0) hideDrop(); });
  window.addEventListener('drop', (ev) => { if (!filesDrag(ev)) return; ev.preventDefault(); hideDrop(); uploadDrop(ev.dataTransfer, S.cwd); });

  // ── search ──
  const findEl = $('find');
  findEl.addEventListener('input', () => {
    S.filter = findEl.value;
    if (!S.filter.trim() && S.search) { endSearch(false); }
    if (!S.search) refresh();
  });
  findEl.addEventListener('keydown', (ev) => {
    if (ev.key === 'Enter') { ev.preventDefault(); if (prefs.searchDeep && isSD() && findEl.value.trim()) startSearch(findEl.value); else { list.focus(); if (S.shown.length) moveTo(0); } }
    else if (ev.key === 'Escape') { ev.preventDefault(); ev.stopPropagation(); if (S.search) endSearch(true); else if (findEl.value) { findEl.value = ''; S.filter = ''; refresh(); } else list.focus(); }
    else if (ev.key === 'ArrowDown') { ev.preventDefault(); list.focus(); if (S.shown.length) moveTo(Math.max(0, S.cur)); }
  });
  $('b-deep').addEventListener('click', () => {
    prefs.searchDeep = !prefs.searchDeep; savePref(); syncCommands();
    if (prefs.searchDeep && findEl.value.trim()) startSearch(findEl.value);
    else if (!prefs.searchDeep && S.search) endSearch(false);
    findEl.focus();
  });
  const bar = $('searchbar');
  function paintSearchBar() {
    const s = S.search;
    if (!s) { bar.hidden = true; bar.textContent = ''; return; }
    bar.hidden = false;
    if (s.running) bar.innerHTML = `<span class="spin" aria-hidden="true"></span><span>${esc(t('searching', { folder: s.root, dirs: s.dirs, count: s.results.length }))}</span><span class="sp"></span><button type="button" class="small" data-s="stop">${esc(t('stop'))}</button>`;
    else bar.innerHTML = `<span>${esc(t('search_done', { count: s.results.length, q: s.query, folder: s.root }))}${s.truncated ? ' ' + esc(t(s.truncated === 'results' ? 'search_capped' : 'search_capped_dirs')) : ''}${s.cancelled ? ' ' + esc(t('search_stopped')) : ''}</span><span class="sp"></span><button type="button" class="small" data-s="close">${esc(t('search_close'))}</button>`;
  }
  bar.addEventListener('click', (ev) => { const b = ev.target.closest('[data-s]'); if (!b) return; if (b.dataset.s === 'stop' && S.search) S.search.ctl.aborted = true; else endSearch(true); });
  async function startSearch(q) {
    if (!isSD()) return;
    endSearch(false);
    const s = { query: q.trim(), root: S.cwd, results: [], running: true, ctl: { aborted: false }, dirs: 0, truncated: false, cancelled: false };
    S.search = s; S.sel.clear(); S.cur = -1;
    renderCrumbs(); paintSearchBar(); refresh();
    let pending = 0;
    const flush = () => { pending = 0; if (S.search === s) { refresh(); paintSearchBar(); } };
    const r = await searchTree({
      list: (d) => fs.list(d), root: s.root, query: s.query, showHidden: prefs.showHidden, signal: s.ctl,
      onResult: (hit) => { s.results.push({ ...hit, key: hit.path }); if (!pending) pending = setTimeout(flush, 250); },
      onProgress: (p) => { s.dirs = p.dirs; if (!pending) pending = setTimeout(flush, 250); },
    });
    clearTimeout(pending);
    if (S.search !== s) return;
    s.running = false; s.truncated = r.truncated; s.cancelled = r.cancelled; s.dirs = r.dirs;
    s.results = r.results.map((h) => ({ ...h, key: h.path }));
    refresh(); paintSearchBar();
  }
  function endSearch(repaint) {
    if (!S.search) return;
    S.search.ctl.aborted = true;
    S.search = null;
    paintSearchBar();
    if (repaint) { findEl.value = ''; S.filter = ''; renderCrumbs(); refresh(); list.focus({ preventScroll: true }); }
  }

  // ── the actions every menu, button and key ends in ──
  const ask = (text, opts) => os.confirm(text, opts);
  const failed = (err) => toast(errorText(err, t), 'error');
  const focusList = () => { try { list.focus({ preventScroll: true }); } catch {} };
  async function askName(title, value) {
    const v = await os.prompt(t('modal_new_name'), { title, value });
    focusList();
    if (v == null) return null;
    const name = String(v).trim();
    const bad = nameProblem(name);
    if (bad) { toast(t('name_' + bad), 'warn'); return null; }
    return name;
  }
  const act = {
    open(e) {
      if (!e) return;
      if (e.type === 'dir') {
        if (isSD()) return goTo(e.path);
        return (async () => { try { await L.localEnter(S.loc, e.name); } catch {} history.push(place()); S.sel.clear(); S.cur = -1; load(); })();
      }
      if (!isSD()) { toast(t('local_open_hint')); return; }
      os.openFile(e.path);                          // outside the shell the core decides: shown if inert, else downloaded
    },
    openAll(sel) { for (const e of sel) { if (e.type === 'dir') act.openNewWindow(e.path); else if (isSD()) os.openFile(e.path); } },
    openWithApp(e, id) { os.openFile(e.path, { with: id }); },
    chooseApp(e) { handlerCache.clear(); os.openWith(e.path); },
    run(e) { os.openApp('code-runner', { path: e.path, autorun: 1 }); },
    openNewWindow(path) { os.openApp('file-commander', { path }); },
    openTerminal(path) { os.openApp('terminal', { cwd: path || S.cwd }); },
    openLocation(e) { const sel = e.name; const dir = e.dir || dirOf(e.path); endSearch(false); findEl.value = ''; S.filter = ''; goTo(dir, { select: sel }); },
    goTo: (p) => goTo(p),
    cut: () => copyCut('cut'),
    copy: () => copyCut('copy'),
    paste: (dir, clip) => paste(dir || null, clip),
    async duplicate(sel) {
      if (!isSD() || !sel || !sel.length) return;
      const made = await ops.duplicate(sel);
      await reload();
      if (made.length && !S.search) refresh({ select: made.map(nameOf) });
    },
    async rename(e) {
      if (!e) { const s = selectedEntries(); if (s.length !== 1) return; e = s[0]; }
      if (!writable()) return;
      if (e.protected) { toast(t('lock_is_protected', { name: e.name }), 'warn'); return; }
      if (!isSD() && !L.canRenameLocal(S.loc, e)) { toast(t('err_unsupported'), 'warn'); return; }
      const name = await askName(t('modal_rename'), e.name);
      if (!name || name === e.name) return;
      try {
        if (isSD()) {
          const to = join(dirOf(e.path), name);
          await ops.guard(fs.move(e.path, to));
          if (prefs.favorites.includes(e.path)) { prefs.favorites = prefs.favorites.map((p) => (p === e.path ? to : p)); savePref(); }
          if (S.search) { e.path = to; e.key = to; e.name = name; refresh(); return; }
        } else await L.localRename(S.loc, e, name);
      } catch (err) { failed(err); return; }
      await reload();
      refresh({ select: name });
    },
    async del() {
      const sel = selectedEntries();
      if (!sel.length || !writable()) return;
      if (!isSD()) {
        const q = sel.length === 1 ? t('del_from', { name: sel[0].name, loc: S.loc.label }) : t('del_n_from', { count: sel.length, loc: S.loc.label });
        if (!(await ask(t('sub_local_perm'), { title: q, ok: t('delete'), danger: true }))) return focusList();
        for (const e of sel) { try { await L.localDelete(S.loc, e); } catch (err) { failed(err); } }
        S.sel.clear(); await reload(); focusList(); return;
      }
      const free = sel.filter((e) => !e.protected);
      if (free.length < sel.length) toast(t('lock_kept_count', { count: sel.length - free.length }), 'warn');
      if (!free.length) return;
      const dirs = free.filter((e) => e.type === 'dir');
      const q = free.length === 1 ? t(free[0].type === 'dir' ? 'del_folder' : 'del_file', { name: free[0].name }) : t('del_n', { count: free.length });
      const sub = dirs.length ? t('sub_folder_perm') : free.length === 1 ? t('sub_to_trash') : t('sub_files_to_trash');
      if (!(await ask(sub, { title: q, ok: t('delete'), danger: dirs.length > 0 }))) return focusList();
      for (const e of free) {
        if (e.type === 'dir') await ops.deleteFolder(e.path);
        else { try { await ops.toTrash(e.path); } catch (err) { failed(err); } }
      }
      dropFromSearch(free);
      S.sel.clear(); await reload(); focusList();
    },
    async delPermanent() {
      if (!isSD()) return act.del();
      const sel = selectedEntries().filter((e) => !e.protected);
      if (!sel.length || !writable()) return;
      const q = sel.length === 1 ? t('delp_one', { name: sel[0].name }) : t('delp_n', { count: sel.length });
      if (!(await ask(t('delp_sub'), { title: q, ok: t('ctx_delete_perm'), danger: true }))) return focusList();
      for (const e of sel) {
        try { if (!(await ops.removeForever(e.path, e.type === 'dir'))) toast(t('err_partly_kept', { name: e.name }), 'warn'); }
        catch (err) { failed(err); }
      }
      dropFromSearch(sel);
      S.sel.clear(); await reload(); focusList();
    },
    async undo() { if (await ops.undo()) reload(); },
    // Archives straight from here, through the OS's own archive layer (shell/nucleo-pack.js): no other app, the same
    // formats on every device. Progress shows in the transfers panel; the result is selected when it lands.
    async compress(sel, format = 'zip', { password = false } = {}) {
      sel = sel || selectedEntries();
      if (!sel.length || !isSD() || !writable()) return;
      let pw = '';
      if (password) {
        pw = await os.prompt(t('zip_password_new'), { title: t('ctx_compress_password'), password: true });
        if (!pw) return;
      }
      const dir = dirOf(sel[0].path);
      const stem = sel.length === 1 ? (sel[0].type === 'dir' ? sel[0].name : sel[0].name.replace(/\.[^.]+$/, '') || sel[0].name) : t('zip_archive_name');
      const dest = await fs.uniquePath(dir, stem + '.' + format);
      await zipJob(t('zip_compressing', { name: nameOf(dest) }), (onProgress) => pack.compressTo(fs, sel.map((e) => e.path), dest, { onProgress, password: pw }), nameOf(dest));
    },
    // "Extract here", the way desktop systems do it: an archive holding one top-level item puts just that item
    // here; anything else goes into a new folder named after the archive. Nothing existing is overwritten. A protected
    // archive asks for its password (again, when it was wrong).
    async extract(e) {
      if (!e || !isSD() || !writable()) return;
      const dir = dirOf(e.path);
      const bytes = await fs.read(e.path).catch((err) => { failed(err); return null; });
      if (!bytes) return;
      let password = '';
      for (let tries = 0; tries < 4; tries++) {
        const ok = await zipJob(t('zip_extracting', { name: e.name }), async (onProgress) => {
          const listed = await pack.list(bytes, { name: e.name, password });
          if (listed.encrypted && !password) throw Object.assign(new Error('password'), { code: 'password' });
          const tops = new Set(listed.entries.filter((x) => !x.unsafe).map((x) => x.name.replace(/^\/+/, '').split('/')[0]).filter(Boolean));
          const [only] = tops;
          const into = tops.size === 1 && !(await fs.exists(join(dir, only))) ? dir
            : await fs.uniquePath(dir, e.name.replace(/\.(tar\.[a-z0-9]+|[a-z0-9]+)$/i, '') || 'archive');
          const r = await pack.extractTo(fs, bytes, into, { onProgress, password, name: e.name });
          if (r.skipped.length) toast(t('zip_skipped', { count: r.skipped.length }), 'warn');
          return into === dir ? only : nameOf(into);
        }, null, { quietCodes: ['password'] });
        if (ok !== 'password') return;
        password = await os.prompt(tries ? t('zip_password_wrong') : t('zip_password_ask', { name: e.name }), { title: e.name, password: true });
        if (!password) return;
      }
    },
    copyPath(sel) { const paths = sel.map((e) => e.path); os.copy(paths.join('\n')); toast(t('toast_path_copied', { count: paths.length })); },
    async makeShortcut(e) {
      const base = e.name.replace(/\.lnk$/i, '');
      try { await ops.makeShortcut(e.path, dirOf(e.path), base + ' - ' + t('shortcut_word')); toast(t('toast_shortcut_made'), 'success'); } catch (err) { failed(err); }
      reload();
    },
    async sendToDesktop(e) {
      try { await ops.makeShortcut(e.path, '/data/Desktop', e.name.replace(/\.lnk$/i, '')); toast(t('toast_sent_desktop'), 'success'); } catch (err) { failed(err); }
    },
    download(sel) {
      for (const e of sel.filter((x) => x.type !== 'dir')) {
        const a = document.createElement('a');
        a.href = fs.url(e.path); a.download = e.name;
        document.body.appendChild(a); a.click(); a.remove();
      }
    },
    properties(sel) {
      if (!sel || !sel.length) return act.folderProperties();
      showProperties(sel, propsCtx());
    },
    folderProperties() { act.placeProperties(isSD() ? S.cwd : null); },
    placeProperties(path) {
      if (!path) { showProperties([{ name: S.loc.label, type: 'dir', path: '', key: 'root' }], { ...propsCtx(), sd: false }); return; }
      showProperties([{ name: nameOf(path) || t('loc_device'), type: 'dir', path, key: path }], propsCtx());
    },
    toggleFavorite(path) {
      const i = prefs.favorites.indexOf(path);
      if (i >= 0) prefs.favorites.splice(i, 1); else prefs.favorites.push(path);
      savePref(); sidebar.render();
      toast(t(i >= 0 ? 'fav_removed' : 'fav_added', { name: nameOf(path) || '/' }));
    },
    async newFolder() {
      if (!writable() || S.search) return;
      const name = await askName(t('modal_new_folder'), t('default_new_folder'));
      if (!name) return;
      if (S.entries.some((e) => e.name === name)) { toast(t('err_exists'), 'warn'); return; }
      try { if (isSD()) await ops.guard(fs.mkdirp(join(S.cwd, name))); else await L.localMkdir(S.loc, name); } catch (err) { failed(err); return; }
      await reload();
      refresh({ select: name });
    },
    async newFile(kind) {
      if (!isSD() || S.search) return;
      const ext = kind === 'md' ? '.md' : '.txt';
      let name = await askName(t(kind === 'md' ? 'modal_new_md' : 'modal_new_text'), t(kind === 'md' ? 'default_md_name' : 'default_text_name') + ext);
      if (!name) return;
      if (!/\.[a-z0-9]+$/i.test(name)) name += ext;
      let dest; try { dest = await fs.uniquePath(S.cwd, name); } catch (err) { failed(err); return; }
      try { await ops.guard(fs.write(dest, '', { overwrite: false })); } catch (err) { failed(err); return; }
      await reload();
      refresh({ select: nameOf(dest) });
    },
    setView(v) { prefs.view = v; savePref(); refresh(); },
    setSort(key, dir) { prefs.sortKey = key; prefs.sortDir = dir; savePref(); refresh(); },
    toggleHidden() { prefs.showHidden = !prefs.showHidden; savePref(); refresh(); sidebar.render(); },
    togglePreview() {
      prefs.preview = !prefs.preview; savePref();
      $('preview').hidden = !prefs.preview;
      if (prefs.preview) preview.show(selectedEntries()); else preview.clear();
      syncCommands();
    },
    selectAll() { S.sel = new Set(S.shown.map((e) => e.key)); if (S.cur < 0 && S.shown.length) S.cur = 0; paintSel(); },
    refresh() { if (S.search) return startSearch(S.search.query); return reload(); },
    copyToDevice: () => copyToDevice(),
    canRenameLocal: (e) => L.canRenameLocal(S.loc, e),
  };
  function dropFromSearch(gone) {
    if (!S.search) return;
    const keys = new Set(gone.map((e) => e.key));
    S.search.results = S.search.results.filter((e) => !keys.has(e.key));
  }
  const propsCtx = () => ({
    t, locale, sd: isSD(), readOnly: !writable(),
    where: (e) => (isSD() ? (e.dir || dirOf(e.path)) : S.loc.label + (L.localTail(S.loc) ? '\\' + L.localTail(S.loc) : '')),
    folderSize: (p, o) => ops.folderSize(p, o),
    handlers: (p) => os.handlers(p),
    changeApp: (e) => act.chooseApp(e),
    copyPath: (sel) => act.copyPath(sel.filter((e) => e.path)),
  });

  // ── thumbnails and preview reads ──
  async function thumbUrl(e) {
    if (!isImage(e.name)) return null;
    if (isSD() || e.path.startsWith('/')) return fs.url(e.path);
    if ((Number(e.size) || 0) > 8 * 1024 * 1024) return null;
    const blob = await L.localReadFile(S.loc, e.name);
    return URL.createObjectURL(blob);
  }
  async function readHead(e, n) {
    if (!isTextLike(e.name)) return null;
    if (isSD() || e.path.startsWith('/')) {
      if ((Number(e.size) || 0) > TEXT_PREVIEW_MAX) return null;
      const b = await fs.readRange(e.path, 0, n - 1);
      return b ? new TextDecoder().decode(b.subarray(0, n)) : null;
    }
    const blob = await L.localReadFile(S.loc, e.name);
    return blob.slice(0, n).text();
  }

  // ── the shell's messages ──
  let fsTimer = 0;
  const fsPaths = new Set();
  const ctrl = {
    local: () => !isSD(), writable,
    copy: () => act.copy(), cut: () => act.cut(), paste: (clip) => paste(null, clip),
    selectAll: () => act.selectAll(), del: () => act.del(), rename: () => act.rename(), undo: () => act.undo(),
    refresh: () => act.refresh(), newWindow: () => act.openNewWindow(isSD() ? S.cwd : '/data'),
    fsChanged(p) {
      if (!isSD()) return;
      const cp = p ? p.replace(/\\/g, '/') : null;
      if (cp) { if (FS_NOISE.has(cp)) return; fsPaths.add(cp); }
      clearTimeout(fsTimer);
      fsTimer = setTimeout(() => {
        const paths = [...fsPaths]; fsPaths.clear();
        if (!paths.length || paths.some((x) => dirOf(x) === S.cwd || x === S.cwd)) { if (!S.search) load(); }
        for (const d of new Set(paths.map(dirOf))) sidebar.refreshDevice(d);
        refreshFree();
      }, 400);
    },
  };
  const shellMessage = (d) => handleShellMessage(d, ctrl);
  // Commands come from the shell only: the window that holds this one, same origin. A store app framed next to this
  // one could post an "os-shortcut" too, and this app has the whole disk.
  const fromShell = (e) => e.origin === location.origin && (window.parent !== window ? e.source === window.parent : e.source === window);
  window.addEventListener('message', (e) => { if (fromShell(e)) shellMessage(e.data); });

  // ── locations of this computer ──
  function removeLocal(loc) {
    const i = locals.indexOf(loc); if (i >= 0) locals.splice(i, 1);
    history.drop((p) => p.loc === loc.id);
    if (S.loc === loc) goTo(lastSdPath); else sidebar.render();
  }
  async function addLocal() {
    const loc = await L.pickFolder();
    if (!loc) return;
    locals.push(loc);
    goPlace({ loc: loc.id, segs: [] });
  }
  const addBtn = $('add-loc');
  addBtn.addEventListener('click', addLocal);
  const syncAddTitle = () => { addBtn.title = L.bridge ? t('add_folder_only') : (L.fsaSupported ? t('add_folder_title') : t('add_unsupported')); };
  if (!L.bridge && !L.fsaSupported) addBtn.disabled = true;
  (async () => { try { const d = await L.bridgeDrives(); if (d.length) { locals.push(...d); sidebar.render(); } } catch {} })();

  // ── the navigation pane: resizable, a drawer on narrow windows ──
  const side = $('side'), resizer = $('resizer'), appEl = $('app'), scrim = $('side-scrim');
  try { const w = parseInt((store && store.getItem('fc.sidew')) || '', 10); if (w >= 160 && w <= 520) side.style.width = w + 'px'; } catch {}
  let rz = null;
  resizer.addEventListener('pointerdown', (e) => { rz = { x: e.clientX, w: side.getBoundingClientRect().width }; resizer.classList.add('drag'); resizer.setPointerCapture(e.pointerId); });
  resizer.addEventListener('pointermove', (e) => { if (rz) side.style.width = Math.max(160, Math.min(520, Math.round(rz.w + e.clientX - rz.x))) + 'px'; });
  resizer.addEventListener('pointerup', () => { if (!rz) return; rz = null; resizer.classList.remove('drag'); try { store && store.setItem('fc.sidew', String(parseInt(side.style.width, 10) || 240)); } catch {} });
  function closeDrawer() { appEl.classList.remove('drawer-open'); scrim.hidden = true; $('b-side').setAttribute('aria-expanded', 'false'); }
  $('b-side').addEventListener('click', () => {
    const open = !appEl.classList.contains('drawer-open');
    appEl.classList.toggle('drawer-open', open); scrim.hidden = !open; $('b-side').setAttribute('aria-expanded', String(open));
    if (open) { const a = side.querySelector('.tnode.active') || side.querySelector('.tnode'); if (a) a.focus(); }
  });
  scrim.addEventListener('click', closeDrawer);

  // Test and preview hook (as before): mount a real FileSystemDirectoryHandle without the picker.
  window._fc = {
    state: () => ({ active: S.loc.id, kind: S.loc.kind, locals: locals.map((l) => l.id), cwd: S.cwd, sdPath: lastSdPath, names: S.entries.map((e) => e.name),
      shown: S.shown.map((e) => e.name), selected: selectedEntries().map((e) => e.name), view: prefs.view, search: S.search ? { q: S.search.query, n: S.search.results.length, running: S.search.running } : null }),
    mountFsa: (handle, label, w = true) => { const loc = L.fsaLocation(handle, label, w, 'fsa:test'); locals.push(loc); goPlace({ loc: loc.id, segs: [] }); return loc.id; },
    goSD: () => goTo(lastSdPath),
    go: (id) => { if (id === 'sd') goTo(lastSdPath); else if (locals.some((l) => l.id === id)) goPlace({ loc: id, segs: [] }); },
  };

  I18N.onChange(() => { renderCrumbs(); refresh(); sidebar.render(); syncAddTitle(); paintSearchBar(); });

  // ── start ──
  syncAddTitle();
  $('preview').hidden = !prefs.preview;
  list.addEventListener('focus', () => { if (S.cur < 0 && S.shown.length && !S.sel.size) { S.cur = 0; view.paint(viewState()); } });
  const start = os.args().path || '/data';
  let startDir = start, startSel = null;
  try { const st = await fs.stat(start); if (st && st.type === 'file') { startDir = dirOf(start); startSel = nameOf(start); } } catch {}
  history.push({ loc: 'sd', path: startDir });
  S.cwd = startDir;
  lastSdPath = startDir;
  sidebar.loadQuick();
  await load({ select: startSel });
  focusList();
}
