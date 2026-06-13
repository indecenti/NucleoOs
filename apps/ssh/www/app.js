// NucleoOS SSH — app controller: bridge setup, saved hosts, connect form, terminal session.
// Online via the self-hosted bridge only; the Cardputer just serves these files. Agent seam is in
// ssh-client.js (window.NucleoSSH + exec); not wired to the agent app yet.

import I18N from '/nucleo-i18n.js';
import { createSSHClient, probeBridge } from './ssh-client.js';
import { createTerminal } from './term.js';

const t = await I18N.init('ssh');
const $ = (id) => document.getElementById(id);
const HOSTS_PATH = '/data/ssh/hosts.json';
const LS_URL = 'nucleo.ssh.url', LS_TOKEN = 'nucleo.ssh.token';

let hosts = [];
let sel = null;          // selected profile
let client = null, term = null, ro = null;

const bridge = () => ({ url: (localStorage.getItem(LS_URL) || 'ws://localhost:8022').trim(), token: localStorage.getItem(LS_TOKEN) || '' });
const uid = () => 'h' + Math.random().toString(36).slice(2, 8);
const esc = (s) => String(s == null ? '' : s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

// ── persistence (SD, paired) ──
async function loadHosts() {
  try {
    const r = await fetch('/api/fs/read?path=' + encodeURIComponent(HOSTS_PATH), { cache: 'no-store' });
    if (r.ok) { const j = JSON.parse(await r.text()); hosts = Array.isArray(j.hosts) ? j.hosts : []; }
    else hosts = [];
  } catch { hosts = []; }
}
async function saveHosts() {
  try { await fetch('/api/fs/mkdir?path=' + encodeURIComponent('/data/ssh'), { method: 'POST' }); } catch {}
  try { await fetch('/api/fs/write?path=' + encodeURIComponent(HOSTS_PATH), { method: 'POST', body: JSON.stringify({ hosts }) }); } catch {}
}

// ── bridge status ──
let lastBridge = { host: '', key: 'bridge_line' };   // remembered so a live language switch can repaint the line
function paintBridgeLine() { $('bridge-line').textContent = t(lastBridge.key, { host: lastBridge.host }); }
async function refreshBridge() {
  const b = bridge();
  lastBridge = { host: b.url.replace(/^wss?:\/\//, ''), key: 'bridge_line' };
  paintBridgeLine();
  $('bridge-dot').className = 'dot busy';
  const up = await probeBridge(b.url);
  $('bridge-dot').className = 'dot ' + (up ? 'ok' : 'err');
  lastBridge.key = up ? 'bridge_up' : 'bridge_down';
  paintBridgeLine();
  return up;
}
I18N.onChange(() => paintBridgeLine());

// ── sidebar ──
function renderHosts() {
  const box = $('hosts'); box.textContent = '';
  if (!hosts.length) { box.innerHTML = '<div class="hint" style="padding:8px" data-i18n="no_hosts">Nessun host. Crea il primo →</div>'; return; }
  for (const h of hosts) {
    const d = document.createElement('div'); d.className = 'host' + (sel && sel.id === h.id ? ' sel' : '');
    d.innerHTML = '<button class="del" data-i18n-attr="title:delete">✕</button><div class="hn">' + esc(h.name || h.host) + '</div><div class="hh">' + esc((h.user || '') + '@' + h.host + ':' + (h.port || 22)) + '</div>';
    d.addEventListener('click', (e) => { if (e.target.classList.contains('del')) { delHost(h); e.stopPropagation(); return; } sel = h; renderHosts(); showForm(h); });
    box.appendChild(d);
  }
}
async function delHost(h) { if (!confirm(t('confirm_delete', { name: h.name || h.host }))) return; hosts = hosts.filter((x) => x.id !== h.id); if (sel && sel.id === h.id) sel = null; await saveHosts(); renderHosts(); showForm(sel); }

// ── connect form ──
function showForm(h) {
  if (client) return;   // a session is live → leave the terminal up
  const p = h || { name: '', host: '', port: 22, user: '', auth: 'password' };
  $('area').innerHTML =
    '<div class="form">' +
    '<h2 data-i18n="' + (h ? 'form_title_connect' : 'form_title_new') + '">' + (h ? 'Connetti' : 'Nuovo host') + '</h2>' +
    '<p data-i18n="form_intro">Inserisci i dati. La password / chiave NON viene salvata: la digiti al momento.</p>' +
    '<div class="row"><div class="f"><label data-i18n="name">Nome</label><input id="f-name" value="' + esc(p.name) + '" data-i18n-attr="placeholder:name_ph" placeholder="il-mio-server"></div></div>' +
    '<div class="row"><div class="f" style="flex:3"><label data-i18n="host_label">Host</label><input id="f-host" value="' + esc(p.host) + '" placeholder="192.168.0.10 / example.com"></div><div class="f"><label data-i18n="port_label">Porta</label><input id="f-port" value="' + esc(p.port || 22) + '"></div></div>' +
    '<div class="row"><div class="f"><label data-i18n="user_label">Utente</label><input id="f-user" value="' + esc(p.user) + '" placeholder="root"></div>' +
    '<div class="f"><label data-i18n="auth_label">Autenticazione</label><div class="seg" id="f-auth"><button data-a="password" class="' + (p.auth !== 'key' ? 'on' : '') + '" data-i18n="auth_password">Password</button><button data-a="key" class="' + (p.auth === 'key' ? 'on' : '') + '" data-i18n="auth_key">Chiave</button></div></div></div>' +
    '<div class="row" id="row-pw"><div class="f"><label data-i18n="password_label">Password</label><input id="f-pw" type="password" autocomplete="off" data-i18n-attr="placeholder:password_ph" placeholder="(non salvata)"></div></div>' +
    '<div class="row" id="row-key" style="display:none;flex-direction:column"><div class="f"><label data-i18n="privkey_label">Chiave privata (incolla)</label><textarea id="f-key" rows="4" style="width:100%;background:var(--bg);border:1px solid var(--line);border-radius:8px;color:var(--ink);padding:9px;font:11px var(--mono)" placeholder="-----BEGIN OPENSSH PRIVATE KEY-----"></textarea></div><div class="f"><label data-i18n="passphrase_label">Passphrase (se serve)</label><input id="f-pass" type="password" autocomplete="off"></div></div>' +
    '<div class="hint" id="f-stat"></div>' +
    '<div class="row" style="margin-top:14px"><button id="f-save" data-i18n="save_host">Salva host</button><div class="grow"></div><button class="pri" id="f-connect" data-i18n="connect_btn">Connetti ▸</button></div>' +
    '</div>';
  let auth = p.auth === 'key' ? 'key' : 'password';
  const applyAuth = () => { $('row-pw').style.display = auth === 'password' ? '' : 'none'; $('row-key').style.display = auth === 'key' ? 'flex' : 'none'; for (const b of $('f-auth').children) b.classList.toggle('on', b.dataset.a === auth); };
  applyAuth();
  for (const b of $('f-auth').children) b.addEventListener('click', () => { auth = b.dataset.a; applyAuth(); });
  const collect = () => ({ id: (h && h.id) || uid(), name: $('f-name').value.trim() || $('f-host').value.trim(), host: $('f-host').value.trim(), port: parseInt($('f-port').value, 10) || 22, user: $('f-user').value.trim(), auth });
  $('f-save').addEventListener('click', async () => {
    const prof = collect(); if (!prof.host || !prof.user) { $('f-stat').textContent = t('err_host_user_required'); return; }
    const i = hosts.findIndex((x) => x.id === prof.id); if (i >= 0) hosts[i] = prof; else hosts.push(prof);
    sel = prof; await saveHosts(); renderHosts(); $('f-stat').textContent = t('saved');
  });
  $('f-connect').addEventListener('click', () => {
    const prof = collect(); if (!prof.host || !prof.user) { $('f-stat').textContent = t('err_host_user_required'); return; }
    const secrets = auth === 'key' ? { privateKey: $('f-key').value, passphrase: $('f-pass').value || undefined } : { password: $('f-pw').value };
    startSession(prof, secrets);
  });
}

// ── live terminal session ──
function startSession(prof, secrets) {
  const b = bridge();
  if (!b.token) { openSettings(t('need_bridge_first')); return; }
  $('area').innerHTML =
    '<div class="term-wrap">' +
    '<div class="banner" id="hk-banner"></div>' +
    '<div class="tbar"><span class="who">' + esc(prof.user + '@' + prof.host) + '</span><span class="st" id="t-stat" data-i18n="st_connecting">connessione…</span><button id="t-reconnect" style="display:none;padding:3px 9px" data-i18n="reconnect">Riconnetti</button><button class="danger" id="t-close" style="padding:3px 9px" data-i18n="disconnect">Disconnetti</button></div>' +
    '<div class="nt-host" id="nt-host"></div></div>';
  const stat = (s) => { const e = $('t-stat'); if (e) e.textContent = s; };
  term = createTerminal($('nt-host'), { onInput: (b2) => client && client.send(b2), onResize: (c, r) => client && client.resize(c, r) });
  term.fit(); term.focus();

  client = createSSHClient({ bridgeUrl: b.url, token: b.token });
  client.on('data', (u8) => term.write(u8));
  client.on('hostkey', (fp, hash) => { const bn = $('hk-banner'); if (bn) { bn.textContent = '🔑 ' + t('hostkey_banner', { fp: hash + ':' + fp }); bn.classList.add('show'); } });
  client.on('status', (state, msg) => {
    if (state === 'connecting') stat(t('st_connecting'));
    else if (state === 'authenticated') stat(t('st_authenticated'));
    else if (state === 'connected') { stat(t('st_connected')); $('bridge-dot').className = 'dot ok'; term.focus(); }
    else if (state === 'closed') { stat(t('st_closed')); endSession(false); }
    else if (state === 'error') { stat(t('st_error', { msg: msg || '' })); endSession(false); }
  });
  client.on('error', (m) => stat(t('st_error', { msg: m })));
  $('t-close').addEventListener('click', () => endSession(true));
  $('t-reconnect').addEventListener('click', () => { endSession(true); startSession(prof, secrets); });

  ro = new ResizeObserver(() => { if (term) term.fit(); });
  ro.observe($('nt-host'));

  client.connect({ host: prof.host, port: prof.port, username: prof.user, cols: term.cols, rows: term.rows, ...secrets })
    .catch((e) => stat(t('st_error', { msg: e.message || e })));

  // AGENT SEAM: register the live session so a future in-shell agent can find it and run exec().
  const session = { id: prof.id, name: prof.name, host: prof.host, user: prof.user, exec: (cmd) => client.exec(cmd), disconnect: () => endSession(true), get connected() { return client && client.connected; } };
  if (window.NucleoSSH) window.NucleoSSH.sessions.push(session);
  try { window.parent && window.parent.postMessage({ type: 'ssh.session', action: 'open', id: prof.id, name: prof.name, host: prof.host }, '*'); } catch {}
  startSession._cur = { prof, session };
}

function endSession(showReconnect) {
  try { client && client.disconnect(); } catch {}
  try { ro && ro.disconnect(); } catch {}
  if (window.NucleoSSH && startSession._cur) window.NucleoSSH.sessions = window.NucleoSSH.sessions.filter((s) => s !== startSession._cur.session);
  try { window.parent && startSession._cur && window.parent.postMessage({ type: 'ssh.session', action: 'close', id: startSession._cur.prof.id }, '*'); } catch {}
  const cur = startSession._cur; client = null; ro = null; startSession._cur = null;
  if (showReconnect) { if (term) { term.dispose(); term = null; } showForm(cur ? cur.prof : sel); }
  else { const rb = $('t-reconnect'); if (rb) rb.style.display = ''; }   // closed by remote → offer reconnect, keep buffer
}

// ── settings ──
function openSettings(msg) {
  const b = bridge(); $('set-url').value = b.url; $('set-token').value = b.token;
  $('set-stat').textContent = msg || ''; $('set-ovl').classList.add('show');
}
$('settings-btn').addEventListener('click', () => openSettings());
$('set-cancel').addEventListener('click', () => $('set-ovl').classList.remove('show'));
$('set-test').addEventListener('click', async () => { localStorage.setItem(LS_URL, $('set-url').value.trim()); $('set-stat').textContent = t('verifying'); const up = await refreshBridge(); $('set-stat').textContent = up ? t('bridge_reachable') : t('bridge_unreachable'); });
$('set-save').addEventListener('click', () => { localStorage.setItem(LS_URL, $('set-url').value.trim() || 'ws://localhost:8022'); localStorage.setItem(LS_TOKEN, $('set-token').value); $('set-ovl').classList.remove('show'); refreshBridge(); });

$('new-btn').addEventListener('click', () => { if (client) return; sel = null; renderHosts(); showForm(null); });

// ── boot ──
(async function boot() {
  await loadHosts(); renderHosts();
  if (!bridge().token) {
    $('area').innerHTML = '<div class="gate"><div><div class="gi">⌘</div><h2 data-i18n="gate_title">Terminale SSH reale</h2><p data-i18n-html="gate_body">Si collega ai tuoi host SSH tramite un piccolo <b>bridge</b> che avvii su una tua macchina (PC/Pi/NAS) — nessun servizio terzo. Avvia <code>tools/nucleo-ssh-bridge</code> (<code>npm i &amp;&amp; node index.js</code>), poi imposta URL e token qui.</p><button class="pri" id="gate-set" data-i18n="gate_set_btn">Imposta bridge</button></div></div>';
    const gs = $('gate-set'); if (gs) gs.addEventListener('click', () => openSettings());
  } else showForm(sel);
  refreshBridge();
})();
