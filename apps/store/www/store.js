/* SPDX-License-Identifier: AGPL-3.0-or-later */
// The Store app: tabs = the signed sources this device can see, cards = the apps it can run. The checks live
// in /store-client.js; this file shows them and asks before anything touches the device.
import I18N from '/nucleo-i18n.js';
import Caps from '/nucleo-caps.js';
import fs from '/nucleo-fs.js';
import os from '/nucleo-shell.js';   // open-app and the OS dialogs
import { makeStore, StoreError } from '/store-client.js';

const t = await I18N.init('store');
const $ = (id) => document.getElementById(id);
const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };
const size = (n) => (n >= 1048576 ? (n / 1048576).toFixed(1) + ' MB' : Math.max(1, Math.round(n / 1024)) + ' KB');

// The device, through the core file layer (/nucleo-fs.js). Any failure is a 'device' error for the store.
const device = (e) => { throw e instanceof StoreError ? e : new StoreError('device', (e && e.message) || String(e)); };
const dev = {
  readText: (p) => fs.readText(p).catch(device),
  // The package lands in its own .tmp folder and takes its place in one move: those files need no atomic
  // replace of their own. The store creates every folder itself (mkdir), in order.
  writeBytes: (p, b) => fs.write(p, b, { mkdir: false, atomic: !p.includes('/.tmp/') }).catch(device),
  // fs.mkdirp forgets a folder once it is removed, so .tmp/<id>, re-made on every install, is really made again.
  mkdir: (p) => fs.mkdirp(p).catch(device),
  move: (from, to) => fs.move(from, to).catch(device),
  remove: (p) => fs.remove(p).then((had) => { if (!had) throw new StoreError('device', '404 ' + p); }, device),
};

const info = await Caps.info();
const paths = info.paths || {};
const store = makeStore({ dev, caps: info.caps, appsDir: paths.apps || '', storeBase: paths.store || '' });
const hasSystem = info.caps.includes('store.native');
const tabs = store.sources().map((s) => ({ id: s.id, src: s }));
if (hasSystem) tabs.push({ id: 'system' });

let current = tabs[0] ? tabs[0].id : null, filter = 'all', busy = false;
const catalogs = new Map();          // source id -> { apps, serial } | { error }

const lang = () => (I18N.lang || 'it').slice(0, 2);
function msg(text, kind) { const i = $('info'); i.textContent = text || ''; i.className = 'info grow' + (kind ? ' ' + kind : ''); }
function why(e) {
  const code = e && e.code;
  const k = { bad_signature: 'e_signature', unknown_key: 'e_signature', bad_package: 'e_signature', bad_file: 'e_file',
    rollback: 'e_rollback', expired: 'e_expired', network: 'e_net', device: 'e_device', install_unsupported: 'not_installable' }[code];
  return k ? t(k) : t('failed') + (e && e.message ? ' — ' + e.message : '');
}

function renderTabs() {
  const box = $('tabs'); box.textContent = '';
  for (const tab of tabs) {
    const b = el('button', tab.id === current ? 'on' : '', t('tab_' + tab.id));
    b.setAttribute('role', 'tab'); b.setAttribute('aria-selected', String(tab.id === current));
    b.addEventListener('click', () => { current = tab.id; show(); });
    box.appendChild(b);
  }
  box.hidden = tabs.length < 2;
}

async function show() {
  renderTabs();
  const sys = current === 'system';
  $('system').hidden = !sys;
  $('grid').hidden = sys; $('bar').hidden = sys;
  for (const id of ['q', 'refresh']) $(id).hidden = sys;
  if (sys) { if (!$('system').src) $('system').src = 'system.html'; $('sub').textContent = t('sub_system'); msg(''); return; }
  if (!current) { $('grid').textContent = ''; $('grid').appendChild(el('div', 'empty', t('empty'))); return; }
  if (!catalogs.has(current)) await load(current); else status(current);
  render();
}

async function load(id) {
  msg(t('loading'));
  try { catalogs.set(id, await store.catalog(id, { lang: lang() })); }
  catch (e) { catalogs.set(id, { error: e }); }
  status(id);
}

// The line under the filter: what the source's catalog said. Set on load and tab change, never by render(),
// so "installed"/"removed" stay visible after the grid redraws.
function status(id) {
  const cat = catalogs.get(id);
  if (!cat) msg('');
  else if (cat.error) msg(why(cat.error), 'err');
  else msg(cat.serial != null ? t('catalog_ok', { n: cat.apps.length, serial: cat.serial }) : '', 'ok');
}

function iconFor(src, app) {
  const box = el('div', 'ph', (app.names[lang()] || app.id).slice(0, 1).toUpperCase());
  const base = src.base + 'apps/' + app.id + '/' + app.version_code + '/';
  if (src.id === 'web') {
    // through fetch, not <img src>: a strict CSP may allow the store only as a connection (same-origin: the
    // cloud's store proxy sits behind its lock; GitHub never gets a cookie)
    fetch(base + 'www/icon.svg', { credentials: 'same-origin' }).then((r) => (r.ok ? r.blob() : null)).then((b) => {
      if (!b) return;
      const img = el('img'); img.alt = ''; img.src = URL.createObjectURL(new Blob([b], { type: 'image/svg+xml' }));
      box.replaceWith(img);
    }).catch(() => {});
  } else if (src.id === 'nap') {
    // the Cardputer launcher's 32x32 icon: '#' on, '.' off
    fetch(base + 'device-icon.txt', { credentials: 'same-origin' }).then((r) => (r.ok ? r.text() : null)).then((txt) => {
      const rows = (txt || '').split('\n').filter((l) => /^[#.]{32}$/.test(l));
      if (rows.length !== 32) return;
      const c = el('canvas'); c.width = 32; c.height = 32;
      const g = c.getContext('2d'); g.fillStyle = getComputedStyle(document.documentElement).getPropertyValue('--accent') || '#6aa8ff';
      rows.forEach((row, y) => [...row].forEach((ch, x) => { if (ch === '#') g.fillRect(x, y, 1, 1); }));
      box.replaceWith(c);
    }).catch(() => {});
  }
  return box;
}

function render() {
  const grid = $('grid'); grid.textContent = '';
  const cat = catalogs.get(current);
  const src = tabs.find((x) => x.id === current).src;
  $('sub').textContent = t('sub_' + current);
  $('filter').hidden = src.install !== 'browser';
  if (!cat || cat.error) { grid.appendChild(el('div', 'empty', cat ? why(cat.error) : t('loading'))); return; }
  const needle = $('q').value.trim().toLowerCase();
  let apps = cat.apps.filter((a) => !needle || (a.names[lang()] + ' ' + a.descriptions[lang()] + ' ' + a.id).toLowerCase().includes(needle));
  if (filter === 'installed') apps = apps.filter((a) => a.installed > 0);
  if (filter === 'updates') apps = apps.filter((a) => a.update);
  if (!apps.length) { grid.appendChild(el('div', 'empty', t('empty'))); return; }
  for (const a of apps) grid.appendChild(card(src, a));
}

function card(src, a) {
  const c = el('div', 'card');
  const hd = el('div', 'hd');
  hd.appendChild(iconFor(src, a));
  const nm = el('div', 'grow'); nm.appendChild(el('div', 'nm', a.names[lang()] || a.id));
  nm.appendChild(el('div', 'mt', [a.version && 'v' + a.version, a.size ? size(a.size) : '', a.author].filter(Boolean).join(' · ')));
  hd.appendChild(nm); c.appendChild(hd);
  c.appendChild(el('div', 'ds', a.descriptions[lang()] || ''));
  if (a.requires && a.requires.length) {
    const chips = el('div', 'chips');
    for (const r of a.requires) if (r !== 'apps.nap') chips.appendChild(el('span', 'chip', r));
    if (chips.children.length) c.appendChild(chips);
  }
  const act = el('div', 'act');
  if (src.install !== 'browser') {
    act.appendChild(el('span', 'where', t('install_on_device')));
  } else if (!src.installable) {
    act.appendChild(el('span', 'where', t('not_installable')));
  } else {
    if (a.installed > 0) act.appendChild(el('span', 'inst', t('installed_v', { v: a.version })));
    if (!a.installed || a.update) {
      const b = el('button', 'pri', a.installed ? t('update_to', { v: a.version }) : t('install'));
      b.addEventListener('click', () => doInstall(src, a));
      act.appendChild(b);
    }
    if (a.installed > 0) {
      const o = el('button', '', t('open'));
      o.addEventListener('click', () => os.openApp(a.id));
      const r = el('button', 'danger', t('remove'));
      r.addEventListener('click', () => doRemove(a));
      act.append(o, r);
    }
  }
  c.appendChild(act);
  return c;
}

function lockUI(on) { busy = on; for (const b of document.querySelectorAll('main button, #refresh')) b.disabled = on; }

async function doInstall(src, a) {
  if (busy) return;
  lockUI(true);
  const name = a.names[lang()] || a.id;
  try {
    await store.install(src.id, a, (s) => {
      const k = { package: 'step_package', download: 'step_download', write: 'step_write', swap: 'step_swap' }[s.step];
      msg(t(k, { done: s.done || 0, total: s.total || 0 }));
    });
    parent.postMessage({ type: 'apps-changed' }, location.origin);
    catalogs.delete(src.id); await load(src.id);
    msg(t('installed_ok', { n: name }), 'ok');
  } catch (e) { msg(why(e), 'err'); }
  lockUI(false); render();
}

async function doRemove(a) {
  if (busy) return;
  const name = a.names[lang()] || a.id;
  if (!(await os.confirm(t('confirm_remove', { n: name }), { danger: true }))) return;
  lockUI(true);
  try {
    await store.uninstall(a.id);
    parent.postMessage({ type: 'apps-changed' }, location.origin);
    catalogs.delete(current); await load(current);
    msg(t('removed_ok', { n: name }), 'ok');
  } catch (e) { msg(why(e), 'err'); }
  lockUI(false); render();
}

$('refresh').addEventListener('click', async () => { if (busy || !current || current === 'system') return; catalogs.delete(current); await load(current); render(); });
let qt = null;
$('q').addEventListener('input', () => { clearTimeout(qt); qt = setTimeout(render, 150); });
$('filter').addEventListener('click', (e) => {
  const b = e.target.closest('button[data-f]'); if (!b) return;
  filter = b.dataset.f;
  for (const x of $('filter').children) x.classList.toggle('on', x === b);
  render();
});
I18N.onChange(() => { catalogs.clear(); show(); });
await show();
