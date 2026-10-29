/* SPDX-License-Identifier: AGPL-3.0-or-later */
// filedialog.js — the OS "Open" and "Save" sheets, shared by every app and every edition.
//
// An app asks the broker (file.open / file.save); the SHELL shows this sheet in its own origin, and the user
// decides where the bytes come from or go to:
//   • the device's storage — browsed through /api/fs (the same contract on every edition), under /data;
//   • the computer or phone the OS is being used from — a file input / a download, made here because a
//     frame without an origin may do neither.
// The app never sees a path it was not given by the user, never lists a folder, never touches /api.
// Edition differences live behind /api/fs and the core file layer (nucleo-fs.js), so no app is written twice.

import coreFs from './nucleo-fs.js';
import { OS_PRIVATE } from './appbroker.js';

const ROOT = '/data';
// Folders of the OS itself, never offered (the same list the broker refuses to store apps).
const HIDDEN = new Set(OS_PRIVATE);
export const OPEN_MAX_BYTES = 64 * 1024 * 1024;

const esc = (s) => String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const extOf = (n) => { const m = /\.([A-Za-z0-9]{1,8})$/.exec(n || ''); return m ? m[1].toLowerCase() : ''; };
export const joinPath = (dir, name) => (dir === '/' ? '' : dir.replace(/\/+$/, '')) + '/' + name;
const parentOf = (p) => p.replace(/\/[^/]+\/?$/, '') || '/';
// A folder the sheet may show: under /data, not hidden, no '..'.
export function browsable(p) {
  if (typeof p !== 'string' || !p.startsWith(ROOT) || /(^|\/)\.\.(\/|$)/.test(p)) return false;
  if (p !== ROOT && !p.startsWith(ROOT + '/')) return false;
  for (const h of HIDDEN) if (p === h || p.startsWith(h + '/')) return false;
  return true;
}
// A name the user typed for a new file or folder: one segment, nothing a file system would refuse.
export function cleanName(n) {
  const s = String(n == null ? '' : n).replace(/[\u0000-\u001f\u007f<>:"|?*\\/]/g, '').replace(/^[.\s]+|[.\s]+$/g, '').slice(0, 120);
  return s;
}
const human = (n) => (n < 1024 ? n + ' B' : n < 1048576 ? (n / 1024).toFixed(0) + ' KB' : (n / 1048576).toFixed(1) + ' MB');

export function createFileDialog({ t, fs = coreFs, remember, recall } = {}) {
  const tr = (k, v) => (t ? t(k, v) : k);
  let current = null;                       // the one sheet on screen: { close }

  async function list(dir) {
    return (await fs.list(dir)).filter((e) => !e.name.startsWith('.'))
      .filter((e) => e.type !== 'dir' || browsable(joinPath(dir, e.name)));
  }

  // The sheet. mode 'open' | 'save'. Resolves what the user chose, or null.
  function sheet({ mode, appName, types, multiple, name, bytes, mime, dirKey, altKey, pathOnly, title, dir: startDir }) {
    if (current) current.close(null);
    const want = (types || []).map((x) => String(x).toLowerCase());
    const fits = (n) => !want.length || want.includes(extOf(n));
    // Where the app last saved, else where it last opened from (a signed copy lands next to the original).
    let dir = browsable(startDir) ? startDir : recall && recall(dirKey);
    if (!browsable(dir) && altKey) dir = recall && recall(altKey);
    if (!browsable(dir)) dir = ROOT;
    return new Promise((resolve) => {
      const scrim = document.createElement('div');
      scrim.className = 'os-files-scrim';
      const box = document.createElement('div');
      box.className = 'os-files'; box.setAttribute('role', 'dialog'); box.setAttribute('aria-modal', 'true');
      if (!title) title = mode === 'open' ? tr('fd_open_title') : tr('fd_save_title');
      box.innerHTML =
        `<div class="fd-head"><h3>${esc(title)}</h3>${appName ? `<span class="fd-app">${esc(appName)}</span>` : ''}</div>` +
        `<div class="fd-where"><button class="fd-up" type="button" aria-label="${esc(tr('fd_up'))}" title="${esc(tr('fd_up'))}">` +
          `<svg viewBox="0 0 16 16" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M8 13V3M3.5 7.5 8 3l4.5 4.5"/></svg></button>` +
          `<nav class="fd-crumbs"></nav>` +
          (mode === 'save' ? `<button class="fd-mk" type="button">${esc(tr('fd_new_folder'))}</button>` : '') +
        `</div>` +
        `<div class="fd-list" role="listbox" tabindex="0" aria-label="${esc(tr('fd_device'))}"></div>` +
        (mode === 'save'
          ? `<label class="fd-name"><span>${esc(tr('fd_name'))}</span><input type="text" autocomplete="off" spellcheck="false"></label>`
          : (want.length ? `<div class="fd-types">${esc(tr('fd_types', { types: want.map((x) => '.' + x).join(' ') }))}</div>` : '')) +
        `<div class="fd-msg" role="status" hidden></div>` +
        `<div class="fd-row">` +
          (pathOnly ? '' : `<button class="os-dialog-btn fd-alt" type="button">${esc(mode === 'open' ? tr('fd_from_here') : tr('fd_download'))}</button>`) +
          `<span class="fd-sp"></span>` +
          `<button class="os-dialog-btn" type="button" data-act="cancel">${esc(tr('cancel'))}</button>` +
          `<button class="os-dialog-btn primary" type="button" data-act="ok" disabled>${esc(mode === 'open' ? tr('fd_open') : tr('fd_save'))}</button>` +
        `</div>` +
        (mode === 'open' && !pathOnly ? `<input type="file" class="fd-file" hidden ${multiple ? 'multiple' : ''} ${want.length ? `accept="${esc(want.map((x) => '.' + x).join(','))}"` : ''}>` : '');
      scrim.appendChild(box);
      document.body.appendChild(scrim);
      requestAnimationFrame(() => scrim.classList.add('show'));

      const $ = (s) => box.querySelector(s);
      const listEl = $('.fd-list'), crumbs = $('.fd-crumbs'), msg = $('.fd-msg'), ok = $('[data-act="ok"]');
      const nameIn = $('.fd-name input');
      let entries = [], picked = new Set(), focusIdx = -1, busy = false, closed = false, deviceOk = true;
      if (nameIn) nameIn.value = name || '';

      const say = (text, kind) => { msg.hidden = !text; msg.textContent = text || ''; msg.dataset.kind = kind || ''; };
      const close = (result) => {
        if (closed) return; closed = true;
        if (current && current.close === close) current = null;
        document.removeEventListener('keydown', onKey, true);
        scrim.classList.remove('show');
        setTimeout(() => scrim.remove(), 160);
        resolve(result);
      };
      current = { close };

      function refreshOk() {
        if (mode === 'open') ok.disabled = busy || !deviceOk || ![...picked].some((n) => entries.find((e) => e.name === n && e.type === 'file'));
        else ok.disabled = busy || !deviceOk || !cleanName(nameIn.value);
      }
      function paintCrumbs() {
        const parts = dir.slice(ROOT.length).split('/').filter(Boolean);
        let acc = ROOT;
        const items = [`<button type="button" data-p="${esc(ROOT)}">${esc(tr('fd_device'))}</button>`];
        for (const p of parts) { acc += '/' + p; items.push(`<span>›</span><button type="button" data-p="${esc(acc)}">${esc(p)}</button>`); }
        crumbs.innerHTML = items.join('');
        crumbs.scrollLeft = crumbs.scrollWidth;             // a deep folder shows its own name, not the root
        $('.fd-up').disabled = dir === ROOT;
      }
      function paintList() {
        if (!deviceOk) { listEl.innerHTML = `<div class="fd-empty">${esc(tr('fd_no_device'))}</div>`; return; }
        const rows = entries.map((e, i) => {
          const isDir = e.type === 'dir';
          const dim = !isDir && (mode === 'save' || !fits(e.name));
          const sel = picked.has(e.name) ? ' sel' : '';
          return `<div class="fd-item${isDir ? ' dir' : ''}${dim ? ' dim' : ''}${sel}${i === focusIdx ? ' focus' : ''}" role="option" data-i="${i}" aria-selected="${!!sel}">` +
            `<span class="fd-ic">${isDir ? '📁' : '📄'}</span><span class="fd-n">${esc(e.name)}</span>` +
            `<span class="fd-s">${isDir ? '' : esc(human(e.size | 0))}</span></div>`;
        });
        listEl.innerHTML = rows.length ? rows.join('') : `<div class="fd-empty">${esc(tr('fd_empty'))}</div>`;
        const f = listEl.querySelector('.fd-item.focus');
        if (f) f.scrollIntoView({ block: 'nearest' });
      }
      async function go(next) {
        if (!browsable(next)) return;
        busy = true; refreshOk();
        listEl.setAttribute('aria-busy', 'true');
        try {
          const got = await list(next);
          got.sort((a, b) => (a.type === b.type ? a.name.localeCompare(b.name, undefined, { numeric: true, sensitivity: 'base' }) : a.type === 'dir' ? -1 : 1));
          entries = got; dir = next; deviceOk = true; picked.clear(); focusIdx = -1; say('');
          if (remember) remember(dirKey, dir);
        } catch {
          if (next !== ROOT) { busy = false; return go(ROOT); }
          entries = []; deviceOk = false;
        }
        busy = false;
        listEl.removeAttribute('aria-busy');
        paintCrumbs(); paintList(); refreshOk();
      }

      async function readDevice(names) {
        const out = [];
        for (const n of names) {
          const path = joinPath(dir, n);
          const b = await fs.read(path);
          if (b == null) throw new Error('read');
          if (b.byteLength > OPEN_MAX_BYTES) throw new Error('large');
          out.push({ name: n, bytes: b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength), path });
        }
        return out;
      }
      async function confirmOk() {
        if (ok.disabled) return;
        if (mode === 'open') {
          const names = [...picked].filter((n) => entries.find((e) => e.name === n && e.type === 'file'));
          if (!names.length) return;
          if (pathOnly) { close({ paths: names.map((n) => joinPath(dir, n)) }); return; }
          busy = true; refreshOk(); say(tr('fd_reading'));
          try { close({ files: await readDevice(multiple ? names : names.slice(0, 1)) }); }
          catch (e) { busy = false; refreshOk(); say(tr(e && e.message === 'large' ? 'fd_too_large' : 'fd_read_fail'), 'err'); }
          return;
        }
        let n = cleanName(nameIn.value);
        const ext = extOf(name);
        if (ext && extOf(n) !== ext && !pathOnly) n += '.' + ext;   // the type stays what the app made
        if (!n) return;
        const hit = entries.find((e) => e.name === n);
        if (hit && hit.type === 'dir') { say(tr('fd_is_folder'), 'err'); return; }
        if (hit && !box.dataset.replace) {
          box.dataset.replace = n;
          say(tr('fd_replace', { name: n }), 'warn');
          ok.textContent = tr('fd_replace_ok');
          return;
        }
        const path = joinPath(dir, n);
        if (pathOnly) { close({ where: 'device', path, name: n }); return; }
        busy = true; refreshOk(); say(tr('fd_saving'));
        try {
          // Atomic and space-checked (nucleo-fs.js): a failure leaves any previous file as it was.
          await fs.write(path, bytes, { mkdir: false, onProgress: (done, total) => {
            if (total > 512 * 1024) say(tr('fd_saving_pct', { pct: Math.floor((done / total) * 100) }));
          } });
          close({ where: 'device', path, name: n });
        } catch (e) {
          busy = false; refreshOk(); say(tr(e && e.code === 'no-space' ? 'fd_no_space' : 'fd_save_fail'), 'err');
        }
      }

      function pick(i, additive) {
        const e = entries[i];
        if (!e) return;
        focusIdx = i;
        if (e.type === 'dir') { picked.clear(); }
        else if (mode === 'save') { nameIn.value = e.name; delete box.dataset.replace; ok.textContent = tr('fd_save'); say(''); }
        else if (fits(e.name)) {
          if (!(multiple && additive)) picked.clear();
          if (picked.has(e.name) && additive) picked.delete(e.name); else picked.add(e.name);
        }
        paintList(); refreshOk();
      }
      function activate(i) {
        const e = entries[i];
        if (!e) return;
        if (e.type === 'dir') { go(joinPath(dir, e.name)); return; }
        pick(i, false);
        confirmOk();
      }

      listEl.addEventListener('click', (ev) => {
        const it = ev.target.closest('.fd-item'); if (!it) return;
        const i = +it.dataset.i;
        // One tap opens a folder (touch screens have no double tap to spare); files are picked, then confirmed.
        if (entries[i] && entries[i].type === 'dir') { go(joinPath(dir, entries[i].name)); return; }
        pick(i, ev.ctrlKey || ev.metaKey || ev.shiftKey);
      });
      listEl.addEventListener('dblclick', (ev) => {
        const it = ev.target.closest('.fd-item'); if (it && entries[+it.dataset.i] && entries[+it.dataset.i].type === 'file') activate(+it.dataset.i);
      });
      crumbs.addEventListener('click', (ev) => { const b = ev.target.closest('button[data-p]'); if (b) go(b.dataset.p); });
      $('.fd-up').addEventListener('click', () => { if (dir !== ROOT) go(parentOf(dir)); });
      $('[data-act="cancel"]').addEventListener('click', () => close(null));
      ok.addEventListener('click', confirmOk);
      scrim.addEventListener('pointerdown', (ev) => { if (ev.target === scrim) close(null); });
      if (nameIn) nameIn.addEventListener('input', () => { delete box.dataset.replace; ok.textContent = tr('fd_save'); say(''); refreshOk(); });

      const mk = $('.fd-mk');
      if (mk) mk.addEventListener('click', async () => {
        if (!deviceOk || busy) return;
        const row = document.createElement('div');
        row.className = 'fd-item dir fd-newdir';
        row.innerHTML = `<span class="fd-ic">📁</span><input type="text" aria-label="${esc(tr('fd_folder_name'))}" placeholder="${esc(tr('fd_folder_name'))}">`;
        listEl.prepend(row);
        const inp = row.querySelector('input');
        inp.focus();
        let done = false;
        const finish = async (commit) => {
          if (done) return; done = true;
          const n = cleanName(inp.value);
          if (!commit || !n) { row.remove(); return; }
          try {
            await fs.mkdirp(joinPath(dir, n));
            await go(joinPath(dir, n));
          } catch { row.remove(); say(tr('fd_mkdir_fail'), 'err'); }
        };
        inp.addEventListener('keydown', (ev) => {
          if (ev.key === 'Enter') { ev.preventDefault(); ev.stopPropagation(); finish(true); }
          if (ev.key === 'Escape') { ev.preventDefault(); ev.stopPropagation(); finish(false); }
        });
        inp.addEventListener('blur', () => finish(true));
      });

      // The other side: the computer or phone in front of the user. The click lands HERE, in the shell, so the
      // browser lets the file input open and the download start.
      if ($('.fd-alt')) $('.fd-alt').addEventListener('click', () => {
        if (mode === 'save') {
          const url = URL.createObjectURL(new Blob([bytes], { type: mime || 'application/octet-stream' }));
          const a = document.createElement('a');
          a.href = url; a.download = name; a.rel = 'noopener'; a.style.display = 'none';
          document.body.appendChild(a); a.click(); a.remove();
          setTimeout(() => URL.revokeObjectURL(url), 60000);
          close({ where: 'download', name });
          return;
        }
        const input = $('.fd-file');
        input.onchange = async () => {
          const fl = [...(input.files || [])].filter((f) => fits(f.name));
          if (!fl.length) return;
          if (fl.some((f) => f.size > OPEN_MAX_BYTES)) { say(tr('fd_too_large'), 'err'); return; }
          busy = true; refreshOk(); say(tr('fd_reading'));
          try { close({ files: await Promise.all((multiple ? fl : fl.slice(0, 1)).map(async (f) => ({ name: f.name, bytes: await f.arrayBuffer() }))) }); }
          catch { busy = false; refreshOk(); say(tr('fd_read_fail'), 'err'); }
        };
        input.click();
      });

      function onKey(ev) {
        if (closed) return;
        const inField = ev.target && ev.target.tagName === 'INPUT';
        if (ev.key === 'Escape') { ev.preventDefault(); ev.stopPropagation(); close(null); return; }
        if (ev.key === 'Tab') {                                   // keep focus inside the sheet
          const f = [...box.querySelectorAll('button:not([disabled]),input:not([hidden]),[tabindex="0"]')];
          if (!f.length) return;
          const i = f.indexOf(document.activeElement);
          if (ev.shiftKey && i <= 0) { ev.preventDefault(); f[f.length - 1].focus(); }
          else if (!ev.shiftKey && i === f.length - 1) { ev.preventDefault(); f[0].focus(); }
          return;
        }
        if (ev.key === 'Enter' && !(inField && ev.target.closest('.fd-newdir'))) {
          if (ev.target && ev.target.tagName === 'BUTTON') return;   // the button's own click
          ev.preventDefault(); ev.stopPropagation();
          if (!inField && focusIdx >= 0 && entries[focusIdx] && entries[focusIdx].type === 'dir') { activate(focusIdx); return; }
          confirmOk();
          return;
        }
        if (inField) return;
        if (ev.key === 'ArrowDown' || ev.key === 'ArrowUp') {
          ev.preventDefault();
          if (!entries.length) return;
          focusIdx = Math.max(0, Math.min(entries.length - 1, focusIdx + (ev.key === 'ArrowDown' ? 1 : -1)));
          const e = entries[focusIdx];
          if (e.type === 'file') pick(focusIdx, false); else { paintList(); refreshOk(); }
          return;
        }
        if (ev.key === 'Backspace' || (ev.key === 'ArrowLeft' && ev.altKey)) { ev.preventDefault(); if (dir !== ROOT) go(parentOf(dir)); }
      }
      document.addEventListener('keydown', onKey, true);

      go(dir).then(() => {
        if (nameIn) {
          nameIn.focus();
          const dot = nameIn.value.lastIndexOf('.');
          nameIn.setSelectionRange(0, dot > 0 ? dot : nameIn.value.length);
        } else listEl.focus();
      });
    });
  }

  return {
    // → [{ name, bytes, path? }] or null (cancelled)
    async open({ appId, appName, types, multiple } = {}) {
      const r = await sheet({ mode: 'open', appName, types, multiple, dirKey: 'open.' + (appId || '') });
      return r ? r.files : null;
    },
    // → { where: 'device', path, name } | { where: 'download', name } | null (cancelled)
    save({ appId, appName, name, bytes, mime } = {}) {
      return sheet({ mode: 'save', appName, name, bytes, mime, dirKey: 'save.' + (appId || ''), altKey: 'open.' + (appId || '') });
    },
    // For system apps (same origin) that read and write themselves: the sheet only answers WHERE.
    // mode 'open' → [path…] | null; mode 'save' → path | null (a replacement already confirmed).
    async pickPath({ appId, appName, mode = 'open', types, multiple, name, dir, title } = {}) {
      const r = await sheet({ mode, appName, types, multiple, name, pathOnly: true, dir, title,
        dirKey: (mode === 'save' ? 'save.' : 'open.') + (appId || ''), altKey: mode === 'save' ? 'open.' + (appId || '') : undefined });
      if (!r) return null;
      return mode === 'save' ? r.path : r.paths;
    },
    get isOpen() { return !!current; },
    cancel() { if (current) current.close(null); },
  };
}
