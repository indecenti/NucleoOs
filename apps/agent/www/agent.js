// NucleoOS "Agenti" — UI controller. Reads the Anthropic key from the device vault, drives the
// online multi-agent runtime, renders the conversation + a live activity log, and gates destructive
// tool calls behind explicit approval. Online-only: if there's no key or no network it does NOT fall
// back to the offline brain — it cleanly hands off to the separate offline ANIMA app.

import { createRuntime } from './runtime.js';
import I18N from '/nucleo-i18n.js';

// Centralized i18n: loads core + this app's catalog, paints [data-i18n] DOM, live-switches on OS
// language change, and returns a namespace-bound t(). Imperatively-built strings use t() below.
const t = await I18N.init('agent');

const $ = (id) => document.getElementById(id);
const lang = () => (localStorage.getItem('anima.lang') === 'en' ? 'en' : 'it');
const KEY_PATH = '/data/anima/teacher.json';

let cfg = null;            // active provider config for the runtime
let rt = null;             // runtime instance
let history = [];          // [{role:'user'|'bot', text}]
let busy = false;

// ---- tiny safe markdown (escape → fenced code → inline) ----
const esc = (s) => String(s == null ? '' : s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
function md(text) {
  const parts = String(text || '').split(/```/);
  let out = '';
  for (let i = 0; i < parts.length; i++) {
    if (i % 2 === 1) { out += '<pre>' + esc(parts[i].replace(/^[a-z]*\n/i, '')) + '</pre>'; continue; }
    out += esc(parts[i])
      .replace(/`([^`]+)`/g, '<code>$1</code>')
      .replace(/\*\*([^*]+)\*\*/g, '<b>$1</b>')
      .replace(/\bhttps?:\/\/[^\s<]+/g, (m) => '<a href="' + m + '" target="_blank" rel="noopener">' + m + '</a>')
      .replace(/\n/g, '<br>');
  }
  return out;
}

const logEl = () => $('log');
function scroll() { const m = logEl(); m.scrollTop = m.scrollHeight; }
function addMsg(role, text, asMd) {
  const d = document.createElement('div'); d.className = 'msg ' + role;
  if (asMd) d.innerHTML = md(text); else d.textContent = text;
  logEl().appendChild(d); scroll(); return d;
}
function setStatus(s, kind) { $('status').textContent = s; const dot = $('dot'); dot.className = 'dot' + (kind ? ' ' + kind : ''); }

// ---- activity log: one card per tool call ----
const TOOL_ICON = { list_files: '📁', read_file: '📄', search_files: '🔎', make_dir: '📁', write_file: '✍️', edit_file: '✏️', append_file: '➕', delete_file: '🗑️', move_file: '🔀', run_js: '⚙️', open_in_os: '🪟', device_status: '📊', list_apps: '🧩', weather: '🌤️', web_search: '🌐' };
function toolStart(ev) {
  const d = document.createElement('div'); d.className = 'tool run';
  const p = ev.input.path || ev.input.from || ev.input.app || '';
  d.innerHTML = '<div class="th"><span class="ti">' + (TOOL_ICON[ev.name] || '🔧') + '</span><span class="tn">' + esc(ev.name) + '</span><span class="tp">' + esc(p) + '</span></div><div class="tr">…</div>';
  logEl().appendChild(d); ev._el = d; scroll();
}
function toolEnd(ev, out, isErr) {
  const d = ev._el; if (!d) return;
  d.className = 'tool' + (isErr ? ' err' : '');
  const tr = d.querySelector('.tr'); if (tr) tr.textContent = String(out || '').slice(0, 1200);
  scroll();
}

// ---- approval overlay (returns a Promise<bool>) ----
function diffHtml(oldS, newS) {
  return '<pre>' + (oldS ? '<span class="del">- ' + esc(oldS).replace(/\n/g, '\n- ') + '</span>\n' : '') +
    (newS ? '<span class="add">+ ' + esc(newS).replace(/\n/g, '\n+ ') + '</span>' : '') + '</pre>';
}
function confirmTool(req) {
  return new Promise((resolve) => {
    const labels = { write_file: t('op_write_file'), edit_file: t('op_edit_file'), append_file: t('op_append_file'), delete_file: t('op_delete_file'), move_file: t('op_move_file'), run_js: t('op_run_js') };
    let bodyHtml = '';
    if (req.op === 'edit_file') bodyHtml = diffHtml(req.old, req.new);
    else if (req.op === 'write_file' || req.op === 'append_file') bodyHtml = '<pre>' + esc(String(req.content || '').slice(0, 4000)) + (String(req.content || '').length > 4000 ? '\n…' : '') + '</pre>';
    else if (req.op === 'run_js') bodyHtml = '<pre>' + esc(String(req.code || '').slice(0, 4000)) + '</pre>';
    else if (req.op === 'move_file') bodyHtml = '<div class="pth">' + esc(req.from) + ' → ' + esc(req.to) + '</div>';
    const pth = req.path || req.from || '';
    $('appr').innerHTML =
      '<h3>' + esc(t('appr_title')) + ' <span class="op">' + esc(labels[req.op] || req.op) + '</span></h3>' +
      (pth ? '<div class="pth">' + esc(pth) + '</div>' : '') + bodyHtml +
      '<div class="btns"><button class="deny" id="ap-deny">' + esc(t('appr_deny')) + '</button><button class="allow" id="ap-allow">' + esc(t('appr_allow')) + '</button></div>';
    $('ovl').classList.add('show');
    const fin = (v) => { $('ovl').classList.remove('show'); resolve(v); };
    $('ap-allow').onclick = () => fin(true);
    $('ap-deny').onclick = () => fin(false);
  });
}

const ui = {
  status: (s) => setStatus(s, 'busy'),
  note: (t) => addMsg('sys', t),
  toolStart, toolEnd,
  sandboxLog: (lvl, txt) => { if (txt) addMsg('sys', '⚙️ ' + txt); },
  confirm: confirmTool,
  autoApprove: () => $('auto').checked,
  webSearchEnabled: () => $('web').checked,
};

// ---- provider config from the device vault ----
function pickCfg(j) {
  j = j || {};
  const keys = (j.keys && typeof j.keys === 'object') ? j.keys : {};
  // Active Google Gemini wins (OpenAI-compat tool-use, but CORS-blocked → routed via the device /api/llm proxy).
  if (j.provider === 'google' && j.key) return { provider: 'google', base: j.base || 'https://generativelanguage.googleapis.com/v1beta/openai', model: j.model || 'gemini-2.5-flash', key: j.key, proxy: true };
  const anth = (keys.anthropic && keys.anthropic.key) ? keys.anthropic
    : (j.provider === 'anthropic' && j.key ? { base: j.base, model: j.model, key: j.key, version: j.version } : null);
  if (anth && anth.key) return { provider: 'anthropic', base: anth.base || 'https://api.anthropic.com', model: anth.model || 'claude-sonnet-4-6', key: anth.key, version: anth.version || '2023-06-01' };
  // degraded: OpenAI-compatible (Groq) — plain chat only
  const oa = (keys.groq && keys.groq.key) ? keys.groq : (keys.openai && keys.openai.key) ? keys.openai
    : (j.key && (!j.provider || j.provider === 'openai') ? { base: j.base, model: j.model, key: j.key } : null);
  if (oa && oa.key) return { provider: 'openai', base: oa.base || 'https://api.groq.com/openai/v1', model: oa.model || 'llama-3.1-8b-instant', key: oa.key };
  // Saved Gemini key (no stronger browser-direct key) — via the device proxy.
  const goog = (keys.google && keys.google.key) ? keys.google : (j.key && /generativelanguage/i.test(j.base || '') ? { base: j.base, model: j.model, key: j.key } : null);
  if (goog && goog.key) return { provider: 'google', base: goog.base || 'https://generativelanguage.googleapis.com/v1beta/openai', model: goog.model || 'gemini-2.5-flash', key: goog.key, proxy: true };
  return null;
}

function showGate(kind) {
  const c = $('gate-card');
  const openApp = (id) => window.parent && window.parent.postMessage({ type: 'open-app', id }, '*');
  if (kind === 'offline') {
    c.innerHTML = '<div class="gi">📡</div><h2>' + esc(t('gate_offline_title')) + '</h2><p>' + esc(t('gate_offline_body')) + '</p><div><button id="g-retry">' + esc(t('retry')) + '</button><button class="ghost" id="g-anima">' + esc(t('gate_anima')) + '</button></div>';
  } else {
    c.innerHTML = '<div class="gi">🔑</div><h2>' + esc(t('gate_key_title')) + '</h2><p>' + esc(t('gate_key_body')) + '</p><div><button id="g-set">' + esc(t('gate_open_settings')) + '</button><button class="ghost" id="g-anima">' + esc(t('gate_anima')) + '</button></div><div><button class="ghost" id="g-retry" style="margin-top:10px">' + esc(t('retry')) + '</button></div>';
  }
  $('gate').classList.add('show');
  const r = $('g-retry'); if (r) r.onclick = boot;
  const s = $('g-set'); if (s) s.onclick = () => openApp('settings');
  const a = $('g-anima'); if (a) a.onclick = () => openApp('anima');
}
function hideGate() { $('gate').classList.remove('show'); }

function rebuildRuntime() {
  const root = ($('ws').value || '/data/agent').trim() || '/data/agent';
  rt = createRuntime({ cfg, root, lang: lang(), ui });
  $('model-line').textContent = cfg.provider === 'anthropic' ? t('model_anthropic') : t('model_groq');
}

async function send() {
  if (busy || !rt) return;
  const q = $('q').value.trim(); if (!q) return;
  if (!navigator.onLine) { showGate('offline'); return; }
  $('q').value = ''; autosize();
  addMsg('user', q);
  history.push({ role: 'user', text: q });
  busy = true; $('send').disabled = true; $('stop').style.display = '';
  setStatus(t('st_thinking'), 'busy');
  try {
    const reply = await rt.run(q, history.slice(0, -1));
    addMsg('bot', reply, true);
    history.push({ role: 'bot', text: reply });
    setStatus(t('st_ready'), 'ok');
  } catch (e) {
    const msg = String(e && e.message || e);
    if (msg === 'stopped') { addMsg('sys', t('msg_stopped')); setStatus(t('st_stopped')); }
    else { addMsg('sys', '⚠️ ' + msg); setStatus(t('st_error'), 'err'); }
  } finally {
    busy = false; $('send').disabled = false; $('stop').style.display = 'none'; $('q').focus();
  }
}

function autosize() { const t = $('q'); t.style.height = 'auto'; t.style.height = Math.min(160, t.scrollHeight) + 'px'; }

async function boot() {
  hideGate();
  if (!navigator.onLine) { showGate('offline'); return; }
  setStatus(t('st_checking_key'));
  try {
    const r = await fetch('/api/fs/read?path=' + encodeURIComponent(KEY_PATH), { cache: 'no-store' });
    if (r.status === 401 || r.status === 403) { setStatus(t('st_pair_first'), 'err'); showGate('key'); return; }
    const j = r.ok ? (JSON.parse(await r.text()) || {}) : {};
    cfg = pickCfg(j);
  } catch { cfg = null; }
  if (!cfg) { showGate('key'); return; }
  rebuildRuntime();
  setStatus(t('st_ready'), 'ok');
  if (!history.length) addMsg('sys', cfg.provider === 'anthropic' ? t('ready_anthropic') : t('ready_groq'));
}

// ---- wiring ----
$('send').addEventListener('click', send);
$('stop').addEventListener('click', () => { if (rt) rt.stop(); });
$('q').addEventListener('input', autosize);
$('q').addEventListener('keydown', (e) => { if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); send(); } });
$('ws').addEventListener('change', () => { if (cfg) rebuildRuntime(); });
window.addEventListener('online', () => { if (!cfg) boot(); else hideGate(); });
window.addEventListener('offline', () => { setStatus(t('st_offline_reconnect'), 'err'); });

// Live language switch (no reload). The observer repaints all [data-i18n] DOM; re-do the bits we
// build imperatively: the model-line (the observer would reset it to the generic "subtitle"), the
// rebuilt runtime (so the agent's own prompt language follows), and any open gate card.
I18N.onChange(() => {
  if (cfg) rebuildRuntime();   // also re-sets #model-line via t()
  if ($('gate').classList.contains('show')) showGate($('g-set') ? 'key' : 'offline');
});

boot();
