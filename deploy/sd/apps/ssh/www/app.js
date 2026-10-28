/* SPDX-License-Identifier: AGPL-3.0-or-later */
// NucleoOS SSH — app controller: bridge setup, saved hosts, connect form, terminal session.
// Online via the self-hosted bridge only; the Cardputer just serves these files. Agent seam is in
// ssh-client.js (window.NucleoSSH + exec); not wired to the agent app yet.
//
// DEVICE MODE: where the device itself runs an OpenSSH client in a pty (/api/pty/caps answers
// available + ssh — the Handheld's server.py), the session goes browser → device → host with no
// bridge (/api/pty?kind=ssh, see /nucleo-pty.js). There the password is not a form field: ssh asks
// for it on the terminal, so it reaches the remote host and nothing else, and host-key prompts and
// 2FA work the same way. A pasted private key is written 0600 on the device's tmpfs for that session
// only. Everywhere else (no endpoint, 404, available:false) the app is the bridge client below.

import I18N from '/nucleo-i18n.js';
import { createSSHClient, probeBridge } from './ssh-client.js';
import { createTerminal } from './term.js';
import { pairDialog, pairedFetch } from '/nucleo-pair.js';
import Caps from '/nucleo-caps.js';   // what this device can do (pty)

const t = await I18N.init('ssh');
const $ = (id) => document.getElementById(id);
const HOSTS_PATH = '/data/ssh/hosts.json';
const LS_URL = 'nucleo.ssh.url', LS_TOKEN = 'nucleo.ssh.token';

let hosts = [];
let sel = null;          // selected profile
let client = null, term = null, ro = null;
let caps = null, devMode = false;   // pty caps; devMode = the device runs ssh itself (no bridge)

const bridge = () => ({ url: (localStorage.getItem(LS_URL) || 'ws://localhost:8022').trim(), token: localStorage.getItem(LS_TOKEN) || '' });
const uid = () => 'h' + Math.random().toString(36).slice(2, 8);
const esc = (s) => String(s == null ? '' : s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

// ── persistence (SD, paired) ──
// Typed read. A FAILED read (offline, 401, 503, corrupt JSON) is NOT "no hosts": the old code then showed
// an empty list and the next "Save host" wrote a one-entry hosts.json over the saved list. 404 = none yet.
async function readHosts() {
  try {
    const r = await fetch('/api/fs/read?path=' + encodeURIComponent(HOSTS_PATH), { cache: 'no-store' });
    if (r.status === 404) return { ok: true, list: [] };
    if (!r.ok) return { ok: false, status: r.status };
    const s = (await r.text()).trim();
    if (!s) return { ok: true, list: [] };
    const j = JSON.parse(s);
    if (j && Array.isArray(j.hosts)) return { ok: true, list: j.hosts };
    if (j && typeof j === 'object' && !Array.isArray(j) && !Object.keys(j).length) return { ok: true, list: [] };   // the firmware's "{}"
    return { ok: false, status: 'corrupt' };
  } catch (e) { return { ok: false, status: e instanceof SyntaxError ? 'corrupt' : 0 }; }
}
const failText = (st) => st === 401 || st === 403 ? t('not_paired') : st === 'corrupt' ? t('read_corrupt') : t('read_failed');
let readOk = false, readStatus = 0;      // saving is allowed only once the REAL list was read (or is known not to exist)
async function loadHosts() {
  const r = await readHosts();
  readOk = r.ok; readStatus = r.ok ? 0 : r.status;
  if (r.ok) hosts = r.list;
}
async function saveHosts() {
  if (!readOk) return false;                                  // never write over a list we could not read
  // pairedFetch: a 401 `pairing_required` asks for the PIN and retries once, instead of a silent
  // failure that read back as "saved" and lost the host on the next reload.
  try { await pairedFetch('/api/fs/mkdir?path=' + encodeURIComponent('/data/ssh'), { method: 'POST' }); } catch {}
  try { const r = await pairedFetch('/api/fs/write?path=' + encodeURIComponent(HOSTS_PATH), { method: 'POST', body: JSON.stringify({ hosts }) }); return r.ok; } catch { return false; }
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

// ── device status (device mode): is there an ssh client on this device at all? ──
// The capability list says whether the device has a pty bridge; only then /api/pty/caps is read, for its state
// (available, ssh, paired).
async function probeCaps() {
  if (!(await Caps.has('pty'))) return null;
  try { const r = await fetch('/api/pty/caps', { cache: 'no-store' }); return r.ok ? await r.json() : null; } catch { return null; }
}
function paintDevice() {
  const line = $('bridge-line'), dot = $('bridge-dot');
  if (!caps) { line.textContent = t('dev_down'); dot.className = 'dot err'; return; }
  if (!caps.available) { line.textContent = t('dev_nopty'); dot.className = 'dot err'; return; }
  if (!caps.ssh) { line.textContent = t('dev_nossh'); dot.className = 'dot err'; return; }
  if (caps.paired === false) { line.textContent = t('dev_unpaired'); dot.className = 'dot busy'; return; }
  line.textContent = t('dev_ready'); dot.className = 'dot ok';
}
async function refreshDevice() { caps = await probeCaps(); paintDevice(); return caps; }
I18N.onChange(() => (devMode ? paintDevice() : paintBridgeLine()));

// ── sidebar ──
function renderHosts() {
  const box = $('hosts'); box.textContent = '';
  if (!readOk) { const d = document.createElement('div'); d.className = 'hint'; d.style.padding = '8px'; d.textContent = failText(readStatus); box.appendChild(d); return; }
  if (!hosts.length) { box.innerHTML = '<div class="hint" style="padding:8px" data-i18n="no_hosts">Nessun host. Crea il primo →</div>'; return; }
  for (const h of hosts) {
    const d = document.createElement('div'); d.className = 'host' + (sel && sel.id === h.id ? ' sel' : '');
    d.innerHTML = '<button class="del" data-i18n-attr="title:delete">✕</button><div class="hn">' + esc(h.name || h.host) + '</div><div class="hh">' + esc((h.user || '') + '@' + h.host + ':' + (h.port || 22)) + '</div>';
    d.addEventListener('click', (e) => { if (e.target.classList.contains('del')) { delHost(h); e.stopPropagation(); return; } sel = h; renderHosts(); showForm(h); });
    box.appendChild(d);
  }
}
async function delHost(h) {
  if (!confirm(t('confirm_delete', { name: h.name || h.host }))) return;
  hosts = hosts.filter((x) => x.id !== h.id); if (sel && sel.id === h.id) sel = null;
  const ok = await saveHosts();
  if (!ok) await loadHosts();   // persistence refused → re-sync the view with reality
  renderHosts(); showForm(sel);
  if (!ok) { const st = $('f-stat'); if (st) st.textContent = t('save_failed'); }
}

// ── connect form ──
function showForm(h) {
  if (client) return;   // a session is live → leave the terminal up
  const p = h || { name: '', host: '', port: 22, user: '', auth: 'password' };
  $('area').innerHTML =
    '<div class="form">' +
    '<h2 data-i18n="' + (h ? 'form_title_connect' : 'form_title_new') + '">' + (h ? 'Connetti' : 'Nuovo host') + '</h2>' +
    (devMode ? '<p data-i18n="form_intro_device"></p>'
      : '<p data-i18n="form_intro">Inserisci i dati. La password / chiave NON viene salvata: la digiti al momento.</p>') +
    '<div class="row"><div class="f"><label data-i18n="name">Nome</label><input id="f-name" value="' + esc(p.name) + '" data-i18n-attr="placeholder:name_ph" placeholder="il-mio-server"></div></div>' +
    '<div class="row"><div class="f" style="flex:3"><label data-i18n="host_label">Host</label><input id="f-host" value="' + esc(p.host) + '" placeholder="192.168.1.10 / example.com"></div><div class="f"><label data-i18n="port_label">Porta</label><input id="f-port" value="' + esc(p.port || 22) + '"></div></div>' +
    '<div class="row"><div class="f"><label data-i18n="user_label">Utente</label><input id="f-user" value="' + esc(p.user) + '" placeholder="root"></div>' +
    '<div class="f"><label data-i18n="auth_label">Autenticazione</label><div class="seg" id="f-auth"><button data-a="password" class="' + (p.auth !== 'key' ? 'on' : '') + '" data-i18n="auth_password">Password</button><button data-a="key" class="' + (p.auth === 'key' ? 'on' : '') + '" data-i18n="auth_key">Chiave</button></div></div></div>' +
    (devMode ? '<div class="row" id="row-pw"><div class="hint" data-i18n="password_in_term"></div></div>'
      : '<div class="row" id="row-pw"><div class="f"><label data-i18n="password_label">Password</label><input id="f-pw" type="password" autocomplete="off" data-i18n-attr="placeholder:password_ph" placeholder="(non salvata)"></div></div>') +
    '<div class="row" id="row-key" style="display:none;flex-direction:column"><div class="f"><label data-i18n="privkey_label">Chiave privata (incolla)</label><textarea id="f-key" rows="4" style="width:100%;background:var(--bg);border:1px solid var(--line);border-radius:8px;color:var(--ink);padding:9px;font:11px var(--mono)" placeholder="-----BEGIN OPENSSH PRIVATE KEY-----"></textarea></div>' +
    (devMode ? '<div class="hint" data-i18n="privkey_note"></div>'
      : '<div class="f"><label data-i18n="passphrase_label">Passphrase (se serve)</label><input id="f-pass" type="password" autocomplete="off"></div>') + '</div>' +
    '<div class="hint" id="f-stat"></div>' +
    '<div class="row" style="margin-top:14px"><button id="f-save" data-i18n="save_host">Salva host</button><div class="grow"></div><button class="pri" id="f-connect" data-i18n="connect_btn">Connetti ▸</button></div>' +
    '</div>';
  let auth = p.auth === 'key' ? 'key' : 'password';
  const applyAuth = () => { $('row-pw').style.display = auth === 'password' ? '' : 'none'; $('row-key').style.display = auth === 'key' ? 'flex' : 'none'; for (const b of $('f-auth').children) b.classList.toggle('on', b.dataset.a === auth); };
  applyAuth();
  for (const b of $('f-auth').children) b.addEventListener('click', () => { auth = b.dataset.a; applyAuth(); });
  const collect = () => ({ id: (h && h.id) || uid(), name: $('f-name').value.trim() || $('f-host').value.trim(), host: $('f-host').value.trim(), port: parseInt($('f-port').value, 10) || 22, user: $('f-user').value.trim(), auth, hostkey: (h && h.hostkey) || undefined });
  $('f-save').addEventListener('click', async () => {
    const prof = collect(); if (!prof.host || !prof.user) { $('f-stat').textContent = t('err_host_user_required'); return; }
    // The saved list is unknown (read failed): re-read it first; while it still fails, nothing is written
    // and the form keeps what was typed.
    if (!readOk) { await loadHosts(); renderHosts(); if (!readOk) { $('f-stat').textContent = failText(readStatus); return; } }
    const i = hosts.findIndex((x) => x.id === prof.id); if (i >= 0) hosts[i] = prof; else hosts.push(prof);
    sel = prof; const ok = await saveHosts(); renderHosts(); $('f-stat').textContent = ok ? t('saved') : t('st_error', { msg: HOSTS_PATH });
  });
  $('f-connect').addEventListener('click', () => {
    const prof = collect(); if (!prof.host || !prof.user) { $('f-stat').textContent = t('err_host_user_required'); return; }
    if (devMode) {
      // The private key is sent once, for this session only; the password never is.
      startDeviceSession(prof, auth === 'key' && $('f-key').value.trim() ? { privateKey: $('f-key').value } : {});
      return;
    }
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
  // TOFU host-key pinning. The bridge completes the SSH handshake itself, so the browser cannot
  // withhold credentials until the key is judged — what it CAN do is remember the key per profile,
  // refuse the session the moment a remembered key changes, and require an explicit human accept
  // before ever talking to the new key again (the banner says exactly that, MITM warning included).
  client.on('hostkey', (fp, hash) => {
    const fpFull = hash + ':' + fp;
    const bn = $('hk-banner'); if (!bn) return;
    const known = prof.hostkey;
    if (known && known !== fpFull) {
      stat(t('st_error', { msg: t('hostkey_mismatch_short') }));
      endSession(false);
      bn.textContent = '⛔ ' + t('hostkey_mismatch', { old: known, fp: fpFull });
      const btn = document.createElement('button');
      btn.textContent = t('hostkey_accept');
      btn.addEventListener('click', () => {
        prof.hostkey = fpFull;
        const i = hosts.findIndex((x) => x.id === prof.id); if (i >= 0) { hosts[i] = prof; saveHosts(); }
        endSession(true); startSession(prof, secrets);
      });
      bn.appendChild(btn);
      bn.classList.add('show', 'alarm');
      return;
    }
    if (!known) {
      prof.hostkey = fpFull;
      const i = hosts.findIndex((x) => x.id === prof.id); if (i >= 0) { hosts[i] = prof; saveHosts(); }   // pin only saved profiles
      bn.textContent = '🔑 ' + t('hostkey_tofu', { fp: fpFull });
    } else {
      bn.textContent = '🔑 ' + t('hostkey_ok', { fp: fpFull });
    }
    bn.classList.remove('alarm');
    bn.classList.add('show');
  });
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
  startSession._cur = { prof, session, reg: 'NucleoSSH' };
}

// ── live terminal session, device mode: the device's own ssh client in a pty ──
async function startDeviceSession(prof, extra) {
  const { createPtyClient, PTY_UNPAIRED } = await import('/nucleo-pty.js');
  $('area').innerHTML =
    '<div class="term-wrap">' +
    '<div class="banner" id="hk-banner"></div>' +
    '<div class="tbar"><span class="who">' + esc(prof.user + '@' + prof.host) + '</span><span class="st" id="t-stat" data-i18n="st_connecting">connessione…</span><button id="t-reconnect" style="display:none;padding:3px 9px" data-i18n="reconnect">Riconnetti</button><button class="danger" id="t-close" style="padding:3px 9px" data-i18n="disconnect">Disconnetti</button></div>' +
    '<div class="nt-host" id="nt-host"></div></div>';
  const stat = (s) => { const e = $('t-stat'); if (e) e.textContent = s; };
  term = createTerminal($('nt-host'), { onInput: (b) => client && client.send(b), onResize: (c, r) => client && client.resize(c, r) });
  term.fit(); term.focus();

  // ssh prints the host-key fingerprint into the terminal and waits for yes/no there. That prompt is
  // the security check, so the banner just points at it instead of pretending to be it.
  const bn = $('hk-banner');
  if (bn) { bn.textContent = '🔑 ' + t('hostkey_hint'); bn.classList.add('show'); }

  client = createPtyClient({ kind: 'ssh' });
  client.on('data', (u8) => term.write(u8));
  client.on('error', (m) => stat(t('st_error', { msg: m })));
  client.on('status', (state) => {
    if (state === 'connecting') stat(t('st_connecting'));
    else if (state === 'connected') { stat(t('st_connected')); $('bridge-dot').className = 'dot ok'; term.focus(); }
    else if (state === 'closed') { stat(t('st_closed')); endSession(false); }
  });
  $('t-close').addEventListener('click', () => endSession(true));
  $('t-reconnect').addEventListener('click', () => { endSession(true); startDeviceSession(prof, extra); });

  ro = new ResizeObserver(() => { if (term) term.fit(); });
  ro.observe($('nt-host'));

  const session = { id: prof.id, name: prof.name, host: prof.host, user: prof.user, kind: 'ssh',
                    exec: (cmd) => client.exec(cmd), disconnect: () => endSession(true),
                    get connected() { return client && client.connected; } };
  startSession._cur = { prof, session, reg: 'NucleoPty' };
  try {
    await client.connect({ host: prof.host, port: prof.port, username: prof.user, cols: term.cols, rows: term.rows, ...extra });
  } catch (e) {
    const msg = e && e.message;
    if (msg === PTY_UNPAIRED) {
      stat(t('st_unpaired'));
      if (await pairDialog()) { endSession(true); startDeviceSession(prof, extra); return; }
    } else stat(t('st_error', { msg: msg || '' }));
    endSession(false);
    await refreshDevice();
    return;
  }

  // Agent seam: a live session an in-shell agent can find and run exec() on
  // (server-side, exec needs key auth — a password prompt has no one to answer it).
  if (window.NucleoPty) window.NucleoPty.sessions.push(session);
  try { window.parent && window.parent.postMessage({ type: 'ssh.session', action: 'open', id: prof.id, name: prof.name, host: prof.host }, '*'); } catch {}
}

function endSession(showReconnect) {
  try { client && client.disconnect(); } catch {}
  try { ro && ro.disconnect(); } catch {}
  const reg = startSession._cur && window[startSession._cur.reg];
  if (reg) reg.sessions = reg.sessions.filter((s) => s !== startSession._cur.session);
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
  caps = await probeCaps();
  if (caps && caps.available && caps.ssh) {
    devMode = true;
    $('settings-btn').hidden = true;
    const sub = $('sub'); sub.setAttribute('data-i18n', 'subtitle_device'); sub.textContent = t('subtitle_device');
    paintDevice();
    showForm(sel);
    return;
  }
  if (!bridge().token) {
    $('area').innerHTML = '<div class="gate"><div><div class="gi">⌘</div><h2 data-i18n="gate_title">Terminale SSH reale</h2><p data-i18n-html="gate_body">Si collega ai tuoi host SSH tramite un piccolo <b>bridge</b> che avvii su una tua macchina (PC/Pi/NAS) — nessun servizio terzo. Avvia <code>tools/nucleo-ssh-bridge</code> (<code>npm i &amp;&amp; node index.js</code>), poi imposta URL e token qui.</p><button class="pri" id="gate-set" data-i18n="gate_set_btn">Imposta bridge</button></div></div>';
    const gs = $('gate-set'); if (gs) gs.addEventListener('click', () => openSettings());
  } else showForm(sel);
  refreshBridge();
})();
