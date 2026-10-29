/* SPDX-License-Identifier: AGPL-3.0-or-later */
// fm-preview.js — the preview pane (Alt+P): a picture, the first 8 KB of a text file, or the facts of anything
// else. It never loads a big file whole: text is a Range read of 8 KB, pictures stream from the device.
import { iconFor, typeLabel, isImage, isTextLike } from './fm-types.js';
import { normMtime, formatSize, formatDate } from './fm-state.js';

export const PREVIEW_TEXT_BYTES = 8192;
const IMG_MAX = 24 * 1024 * 1024;

// ctrl: { t, locale(), imageUrl(e) → string | Promise<string> | null, readHead(e, n) → Promise<string | null> }
export function createPreview(el, ctrl) {
  const { t } = ctrl;
  let seq = 0, timer = 0, blobUrl = null;
  const drop = () => { if (blobUrl) { URL.revokeObjectURL(blobUrl); blobUrl = null; } };

  function facts(e) {
    const dl = document.createElement('dl');
    dl.className = 'pv-facts';
    const add = (k, v) => { if (v == null || v === '') return; const dt = document.createElement('dt'); dt.textContent = k; const dd = document.createElement('dd'); dd.textContent = v; dl.append(dt, dd); };
    add(t('prop_type'), typeLabel(e.name, e.type === 'dir', t));
    if (e.type !== 'dir') add(t('prop_size'), formatSize(e.size, ctrl.locale()));
    const ms = normMtime(e.mtime);
    if (ms != null) add(t('col_mtime'), formatDate(ms, ctrl.locale()));
    if (e.protected) add(t('prop_attrs'), t('prop_protected'));
    return dl;
  }
  function frame(e) {
    el.textContent = '';
    const box = document.createElement('div'); box.className = 'pv-body';
    const name = document.createElement('div'); name.className = 'pv-name'; name.textContent = e.name;
    el.append(box, name, facts(e));
    return box;
  }
  function big(box, e) { box.innerHTML = `<div class="pv-icon">${iconFor(e.name, e.type === 'dir')}</div>`; }

  async function paint(entries) {
    const my = ++seq;
    drop();
    if (!entries.length) { el.innerHTML = `<div class="pv-none">${escapeText(t('pv_none'))}</div>`; return; }
    if (entries.length > 1) {
      const bytes = entries.reduce((a, e) => a + (e.type === 'dir' ? 0 : Number(e.size) || 0), 0);
      el.innerHTML = `<div class="pv-none"><strong>${escapeText(t('n_items', { count: entries.length }))}</strong><br>${escapeText(formatSize(bytes, ctrl.locale()))}</div>`;
      return;
    }
    const e = entries[0];
    const box = frame(e);
    big(box, e);
    if (e.type === 'dir') return;
    if (isImage(e.name) && (Number(e.size) || 0) <= IMG_MAX) {
      let url = null;
      try { url = await ctrl.imageUrl(e); } catch {}
      if (my !== seq || !url) return;
      if (url.startsWith('blob:')) blobUrl = url;
      const img = document.createElement('img');
      img.alt = e.name; img.decoding = 'async';
      img.addEventListener('load', () => { if (my === seq) { box.textContent = ''; box.appendChild(img); } }, { once: true });
      img.src = url;
      return;
    }
    if (isTextLike(e.name) && (Number(e.size) || 0) > 0) {
      let text = null;
      try { text = await ctrl.readHead(e, PREVIEW_TEXT_BYTES); } catch {}
      if (my !== seq || text == null) return;
      const pre = document.createElement('pre');
      pre.className = 'pv-text';
      pre.textContent = text + ((Number(e.size) || 0) > PREVIEW_TEXT_BYTES ? '\n…' : '');
      box.textContent = '';
      box.appendChild(pre);
    }
  }
  // Selection changes come in bursts (arrow keys held down): the pane follows a moment later.
  function show(entries) { clearTimeout(timer); timer = setTimeout(() => paint(entries), 120); }
  function clear() { seq++; clearTimeout(timer); drop(); el.textContent = ''; }
  return { show, clear };
}
const escapeText = (s) => String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
