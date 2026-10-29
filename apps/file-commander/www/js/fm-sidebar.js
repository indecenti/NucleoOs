/* SPDX-License-Identifier: AGPL-3.0-or-later */
// fm-sidebar.js — the navigation pane: Quick access (the user's usual folders that exist), Favorites (pinned
// folders), then every location as a folder tree (the device, and folders or drives of this computer).
// The tree loads lazily, follows the folder shown, and is driven by mouse, touch and the keyboard.
import { placeIcon } from './fm-types.js';

const CHEV = '<svg viewBox="0 0 10 10" width="10" height="10" aria-hidden="true"><path d="M3.5 2l3 3-3 3" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"/></svg>';
export const QUICK = [
  { name: 'Desktop', key: 'qa_desktop', icon: 'desktop' },
  { name: 'Documents', key: 'qa_documents', icon: 'documents' },
  { name: 'Downloads', key: 'qa_downloads', icon: 'downloads' },
  { name: 'Pictures', key: 'qa_pictures', icon: 'pictures' },
  { name: 'Music', key: 'qa_music', icon: 'music' },
  { name: 'Videos', key: 'qa_videos', icon: 'videos' },
];

// ctrl: { t, locations() → [loc], active() → loc, segments() → [names], cwd(), favorites() → [path],
//         listFolders(node) → [{ name, path?, handle?, leaf }], go(node), goPath(path), menuFor(kind, data, ev),
//         dropOn(path, ev) → bool, removeLocation(loc), dataFolders() → Promise<Set<name>>,
//         drives() → Promise<[{ path, label, icon }]> (device drives outside the folder tree, such as the cloud archive) }
export function createSidebar(el, ctrl) {
  const { t } = ctrl;
  let roots = [];
  let quick = [];                               // the Quick access folders that exist
  let drives = [];                              // the device's extra drives (ctrl.drives)
  const byName = (a, b) => a.name.localeCompare(b.name, undefined, { numeric: true, sensitivity: 'base' });

  const makeRoot = (loc) => ({ loc, label: loc.label, depth: 0, isRoot: true, segs: [], expanded: false, loaded: false, leaf: false, children: [],
    path: loc.kind === 'sd' ? '/' : loc.src === 'bridge' ? loc.root : undefined, handle: loc.kind === 'local' && loc.src === 'fsa' ? loc.stack[0] : undefined });
  const child = (parent, f) => ({ loc: parent.loc, label: f.name, depth: parent.depth + 1, isRoot: false, segs: [...parent.segs, f.name],
    expanded: false, loaded: false, leaf: !!f.leaf, children: [], path: f.path, handle: f.handle });

  async function loadChildren(node) {
    let kids = [];
    try { kids = await ctrl.listFolders(node); } catch { kids = []; }
    node.children = kids.sort(byName).map((f) => child(node, f));
    node.loaded = true;
    node.leaf = node.children.length === 0;
  }
  async function expandTo(root, segs) {
    if (!root.loaded) await loadChildren(root);
    root.expanded = true;
    let n = root;
    for (const s of segs) { const c = n.children.find((x) => x.label === s); if (!c) break; if (!c.loaded) await loadChildren(c); c.expanded = true; n = c; }
  }
  function syncRoots() {
    const prev = new Map(roots.map((r) => [r.loc.id, r]));
    roots = ctrl.locations().map((loc) => prev.get(loc.id) || makeRoot(loc));
  }

  // ── drawing ──
  const sec = (label, extraCls = '') => { const h = document.createElement('div'); h.className = 'side-sec ' + extraCls; h.setAttribute('role', 'presentation'); h.textContent = label; return h; };
  function placeRow({ icon, label, path, kind, active, title }) {
    const r = document.createElement('div');
    r.className = 'tnode place' + (active ? ' active' : '');
    r.setAttribute('role', 'treeitem');
    r.tabIndex = -1;
    r.dataset.path = path;
    r.dataset.kind = kind;
    r.innerHTML = `<span class="tw leaf"></span><span class="tg">${placeIcon(icon)}</span><span class="tn"></span>`;
    r.querySelector('.tn').textContent = label;
    if (title) r.title = title;
    r.addEventListener('click', () => ctrl.goPath(path));
    wireDrop(r, path);
    return r;
  }
  function wireDrop(r, path) {
    r.addEventListener('dragover', (ev) => { if (ctrl.dropOn(path, ev, true)) { ev.preventDefault(); r.classList.add('drop-target'); } });
    r.addEventListener('dragleave', () => r.classList.remove('drop-target'));
    r.addEventListener('drop', (ev) => { r.classList.remove('drop-target'); if (ctrl.dropOn(path, ev, false)) ev.preventDefault(); });
  }
  function nodeRow(node, act) {
    const r = document.createElement('div');
    const active = node.loc === ctrl.active() && node.segs.length === act.length && node.segs.every((s, i) => s === act[i]);
    r.className = 'tnode' + (active ? ' active' : '');
    r.setAttribute('role', 'treeitem');
    r.tabIndex = -1;
    if (!node.leaf) r.setAttribute('aria-expanded', String(!!node.expanded));
    r.style.paddingLeft = (6 + node.depth * 14) + 'px';
    const icon = node.isRoot ? (node.loc.kind === 'sd' ? 'device' : node.loc.icon || 'folder') : 'folder';
    r.innerHTML = `<span class="tw${node.expanded ? ' open' : ''}${node.leaf ? ' leaf' : ''}">${CHEV}</span><span class="tg">${placeIcon(icon)}</span><span class="tn"></span>`;
    r.querySelector('.tn').textContent = node.isRoot && node.loc.kind === 'sd' ? t('loc_device') : node.label;
    if (node.isRoot && node.loc.removable) {
      const x = document.createElement('button');
      x.className = 'tx'; x.type = 'button'; x.title = t('tx_remove'); x.setAttribute('aria-label', t('tx_remove'));
      x.innerHTML = '<svg viewBox="0 0 16 16" width="12" height="12" aria-hidden="true"><path d="M4 4l8 8M12 4l-8 8" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/></svg>';
      x.addEventListener('click', (e) => { e.stopPropagation(); ctrl.removeLocation(node.loc); });
      r.appendChild(x);
    }
    r._node = node;
    if (node.loc.kind === 'sd') { const p = '/' + node.segs.join('/'); r.dataset.path = p; r.dataset.kind = 'tree'; wireDrop(r, p); }
    r.querySelector('.tw').addEventListener('click', (e) => { e.stopPropagation(); toggle(node); });
    r.addEventListener('click', () => ctrl.go(node));
    return r;
  }
  function render() {
    syncRoots();
    const act = ctrl.segments();
    const focusedPath = document.activeElement && el.contains(document.activeElement) ? keyOf(document.activeElement) : null;
    el.textContent = '';
    const cwd = ctrl.active().kind === 'sd' ? ctrl.cwd() : null;
    if (quick.length) {
      el.appendChild(sec(t('sec_quick')));
      for (const q of quick) el.appendChild(placeRow({ icon: q.icon, label: t(q.key), path: '/data/' + q.name, kind: 'quick', active: cwd === '/data/' + q.name }));
    }
    const favs = ctrl.favorites();
    el.appendChild(sec(t('sec_favorites')));
    if (!favs.length) { const m = document.createElement('div'); m.className = 'side-empty'; m.textContent = t('fav_empty'); el.appendChild(m); }
    for (const p of favs) el.appendChild(placeRow({ icon: 'star', label: p.split('/').pop() || '/', path: p, kind: 'fav', active: cwd === p, title: p }));
    el.appendChild(sec(t('sec_device')));
    const sdRoot = roots.find((r) => r.loc.kind === 'sd');
    if (sdRoot) drawTree(sdRoot, act);
    for (const d of drives) el.appendChild(placeRow({ icon: d.icon || 'drive', label: d.label, path: d.path, kind: 'drive', active: cwd === d.path || (cwd || '').startsWith(d.path + '/'), title: d.path }));
    el.appendChild(sec(t('sec_pc')));
    const locals = roots.filter((r) => r.loc.kind !== 'sd');
    if (!locals.length) { const m = document.createElement('div'); m.className = 'side-empty'; m.textContent = ctrl.localHint(); el.appendChild(m); }
    else for (const r of locals) drawTree(r, act);
    const a = el.querySelector('.tnode.active');
    if (a) { a.tabIndex = 0; if (!focusedPath) a.scrollIntoView({ block: 'nearest' }); }
    else { const f = el.querySelector('.tnode'); if (f) f.tabIndex = 0; }
    if (focusedPath) { const again = [...el.querySelectorAll('.tnode')].find((n) => keyOf(n) === focusedPath); if (again) { again.tabIndex = 0; again.focus({ preventScroll: true }); } }
  }
  const keyOf = (n) => (n._node ? n._node.loc.id + ':' + n._node.segs.join('/') : (n.dataset.kind || '') + ':' + (n.dataset.path || ''));
  function drawTree(node, act) {
    el.appendChild(nodeRow(node, act));
    if (node.expanded) for (const c of node.children) drawTree(c, act);
  }
  async function toggle(node) {
    if (node.leaf) return;
    if (!node.loaded) { await loadChildren(node); node.expanded = true; }
    else node.expanded = !node.expanded;
    render();
  }

  // ── keeping up with the list ──
  async function reveal() {
    syncRoots();
    const root = roots.find((r) => r.loc === ctrl.active());
    if (root) { try { await expandTo(root, ctrl.segments()); } catch {} }
    render();
  }
  // A folder appeared or vanished below `parentPath` (a device path): reload that level if it is open.
  async function refreshDevice(parentPath) {
    const root = roots.find((r) => r.loc.kind === 'sd');
    if (!root) return;
    let hit = null;
    (function walk(n) { if (hit) return; if (n.loaded && '/' + n.segs.join('/') === parentPath) hit = n; n.children.forEach(walk); })(root);
    if (!hit) return;
    const open = new Set(); (function walk(n) { if (n.expanded) open.add(n.segs.join('/')); n.children.forEach(walk); })(hit);
    await loadChildren(hit);
    for (const c of hit.children) if (open.has(c.segs.join('/'))) { await loadChildren(c); c.expanded = true; }
    render();
  }
  async function loadQuick() {
    try {
      const have = await ctrl.dataFolders();
      quick = QUICK.filter((q) => have.has(q.name));
    } catch { quick = []; }
    try { drives = ctrl.drives ? await ctrl.drives() : []; } catch { drives = []; }
    render();
  }

  // ── keyboard: up/down move, right opens, left closes or goes to the parent, Enter navigates ──
  el.addEventListener('keydown', (ev) => {
    const cur = ev.target.closest && ev.target.closest('.tnode');
    if (!cur) return;
    const all = [...el.querySelectorAll('.tnode')];
    const i = all.indexOf(cur);
    const focus = (n) => { if (!n) return; all.forEach((x) => { x.tabIndex = -1; }); n.tabIndex = 0; n.focus(); };
    const node = cur._node;
    switch (ev.key) {
      case 'ArrowDown': focus(all[i + 1]); break;
      case 'ArrowUp': focus(all[i - 1]); break;
      case 'Home': focus(all[0]); break;
      case 'End': focus(all[all.length - 1]); break;
      case 'ArrowRight': if (node && !node.leaf && !node.expanded) toggle(node); else focus(all[i + 1]); break;
      case 'ArrowLeft':
        if (node && node.expanded) toggle(node);
        else if (node && node.depth > 0) { const up = all.slice(0, i).reverse().find((x) => x._node && x._node.depth < node.depth); focus(up); }
        break;
      case 'Enter': case ' ': cur.click(); break;
      default: return;
    }
    ev.preventDefault();
  });

  return { render, reveal, refreshDevice, loadQuick, get roots() { return roots; } };
}
