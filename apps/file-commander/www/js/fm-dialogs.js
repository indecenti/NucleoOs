/* SPDX-License-Identifier: AGPL-3.0-or-later */
// fm-dialogs.js — the Properties dialog: drawn in the app's window like the OS dialogs, closed with Esc, Enter
// or a click outside, focus kept inside while open and given back to the list after.
// Questions (rename, delete, new folder) are the OS's own dialogs (os.prompt / os.confirm), not here.
import { iconFor, typeLabel } from './fm-types.js';
import { normMtime, formatSize, formatDate, summarize } from './fm-state.js';
import { dirOf } from './fm-path.js';

let openOne = null;

// A modal frame: returns { box, close }. onClose runs once, whichever way it closes.
export function modal({ title, labelledIcon = '', onClose = () => {} }) {
  if (openOne) openOne.close();
  const back = document.activeElement;
  const scrim = document.createElement('div');
  scrim.className = 'scrim';
  const box = document.createElement('div');
  box.className = 'modal';
  box.setAttribute('role', 'dialog');
  box.setAttribute('aria-modal', 'true');
  box.setAttribute('aria-labelledby', 'm-title');
  const h = document.createElement('h2');
  h.id = 'm-title';
  h.innerHTML = labelledIcon;
  const span = document.createElement('span'); span.textContent = title; h.appendChild(span);
  box.appendChild(h);
  scrim.appendChild(box);
  document.body.appendChild(scrim);
  let closed = false;
  const close = () => {
    if (closed) return;
    closed = true;
    scrim.remove();
    document.removeEventListener('keydown', onKey, true);
    if (openOne && openOne.close === close) openOne = null;
    try { onClose(); } catch {}
    if (back && back.focus) { try { back.focus({ preventScroll: true }); } catch {} }
  };
  function onKey(e) {
    if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); close(); return; }
    if (e.key === 'Tab') {                                   // keep focus inside
      const f = [...box.querySelectorAll('button, [href], input, [tabindex]:not([tabindex="-1"])')].filter((x) => !x.disabled);
      if (!f.length) return;
      const i = f.indexOf(document.activeElement);
      if (e.shiftKey && i <= 0) { e.preventDefault(); f[f.length - 1].focus(); }
      else if (!e.shiftKey && i === f.length - 1) { e.preventDefault(); f[0].focus(); }
    }
  }
  document.addEventListener('keydown', onKey, true);
  scrim.addEventListener('pointerdown', (e) => { if (e.target === scrim) close(); });
  openOne = { close };
  return { box, close };
}

const row = (tbl, k, v, cls = '') => {
  const tr = document.createElement('tr');
  const th = document.createElement('th'); th.scope = 'row'; th.textContent = k;
  const td = document.createElement('td'); if (cls) td.className = cls;
  if (v instanceof Node) td.appendChild(v); else td.textContent = v == null ? '' : String(v);
  tr.append(th, td); tbl.appendChild(tr);
  return td;
};
const btn = (label, cls, fn) => { const b = document.createElement('button'); b.type = 'button'; b.className = cls || ''; b.textContent = label; b.addEventListener('click', fn); return b; };

// ctx: { t, locale(), where(entry) → location words, folderSize(path, { signal, onProgress }), handlers(path) → Promise<[{name,isDefault}]>,
//        changeApp(entry), copyPath(entries), sd: bool }
export function showProperties(entries, ctx) {
  const { t } = ctx;
  const one = entries.length === 1 ? entries[0] : null;
  const ctl = { aborted: false };
  const icon = one ? iconFor(one.name, one.type === 'dir') : iconFor('', true);
  const m = modal({ title: one ? one.name : t('n_items', { count: entries.length }), labelledIcon: icon, onClose: () => { ctl.aborted = true; } });
  const tbl = document.createElement('table');
  tbl.className = 'props';
  m.box.appendChild(tbl);
  const bytes = (n) => t('prop_bytes', { size: formatSize(n, ctx.locale()), bytes: Number(n).toLocaleString(ctx.locale()) });

  let sizeCell;
  if (one) {
    row(tbl, t('prop_name'), one.name, 'wrap');
    row(tbl, t('prop_type'), typeLabel(one.name, one.type === 'dir', t));
    if (one.type !== 'dir' && ctx.sd) {
      const span = document.createElement('span');
      span.className = 'opens';
      const app = document.createElement('span'); app.textContent = '…';
      span.append(app, btn(t('prop_change'), 'small', () => { m.close(); ctx.changeApp(one); }));
      row(tbl, t('prop_opens_with'), span);
      Promise.resolve(ctx.handlers(one.path)).then((hs) => {
        const d = (hs || []).find((h) => h.isDefault) || (hs || [])[0];
        app.textContent = d ? (d.name || d.id) : t('prop_no_app');
      }).catch(() => { app.textContent = t('prop_no_app'); });
    }
    row(tbl, t('prop_location'), ctx.where(one), 'wrap');
    sizeCell = row(tbl, t('prop_size'), one.type === 'dir' ? t('prop_counting') : bytes(Number(one.size) || 0));
    const ms = normMtime(one.mtime);
    if (ms != null) row(tbl, t('col_mtime'), formatDate(ms, ctx.locale()));
    const attrs = [];
    if (one.protected) attrs.push(t('prop_protected'));
    if (one.read_only || ctx.readOnly) attrs.push(t('prop_read_only'));
    if (one.name.startsWith('.')) attrs.push(t('prop_hidden'));
    row(tbl, t('prop_attrs'), attrs.length ? attrs.join(', ') : t('prop_none'));
  } else {
    const s = summarize(entries);
    row(tbl, t('prop_contains'), t('prop_files_folders', { files: s.files, folders: s.dirs }));
    const dirs = [...new Set(entries.map((e) => (e.dir || (e.path ? dirOf(e.path) : ''))))];
    row(tbl, t('prop_location'), dirs.length === 1 ? (ctx.where(entries[0])) : t('prop_various'), 'wrap');
    sizeCell = row(tbl, t('prop_size'), s.dirs ? t('prop_counting') : bytes(s.bytes));
  }

  // Folder sizes are counted file by file, in the background; closing the dialog stops the count.
  const folders = entries.filter((e) => e.type === 'dir' && e.path && ctx.sd);
  if (folders.length) {
    const base = entries.reduce((a, e) => a + (e.type === 'dir' ? 0 : Number(e.size) || 0), 0);
    const filesBase = entries.filter((e) => e.type !== 'dir').length;
    const stop = btn(t('stop'), 'small', () => { ctl.aborted = true; });
    sizeCell.appendChild(document.createTextNode(' '));
    sizeCell.appendChild(stop);
    (async () => {
      let b = base, f = filesBase, d = 0, truncated = false;
      const done = [];
      for (const dir of folders) {
        const r = await ctx.folderSize(dir.path, { signal: ctl, onProgress: (p) => {
          if (ctl.aborted) return;
          sizeCell.firstChild.textContent = t('prop_counting_n', { size: formatSize(b + p.bytes, ctx.locale()), files: f + p.files }) + ' ';
        } }).catch(() => null);
        if (!r || ctl.aborted) break;
        b += r.bytes; f += r.files; d += r.dirs + (one ? 0 : 1); truncated = truncated || r.truncated;
        done.push(dir);
      }
      stop.remove();
      if (!sizeCell.isConnected) return;
      const text = (ctl.aborted ? t('prop_counted_partial') + ' ' : '') + bytes(b) + ' · ' + t('prop_files_folders', { files: f, folders: d }) + (truncated ? ' +' : '');
      sizeCell.textContent = text;
    })();
  }

  const foot = document.createElement('div');
  foot.className = 'm-foot';
  const ok = btn(t('close'), 'primary', () => m.close());
  foot.append(btn(t('ctx_copy_path'), '', () => ctx.copyPath(entries)), ok);
  m.box.appendChild(foot);
  ok.focus();
  return m;
}
