// NucleoOS "Agenti" — UI controller. Reads the Anthropic key from the device vault, drives the
// online multi-agent runtime, renders the conversation + a live activity log, and gates destructive
// tool calls behind explicit approval. Online-only: if there's no key or no network it does NOT fall
// back to the offline brain — it cleanly hands off to the separate offline ANIMA app.

import { createRuntime } from './runtime.js';
import { providerOf, PROVIDERS, readTeacher } from '/ai.js';   // shared provider registry + cached vault read: Claude, Groq, Grok (xAI), Gemini
import I18N from '/nucleo-i18n.js';

// Centralized i18n: loads core + this app's catalog, paints [data-i18n] DOM, live-switches on OS
// language change, and returns a namespace-bound t(). Imperatively-built strings use t() below.
const t = await I18N.init('agent');

const $ = (id) => document.getElementById(id);
const lang = () => (localStorage.getItem('anima.lang') === 'en' ? 'en' : 'it');

let cfg = null;            // active/default provider config for the runtime
let keys = null;           // full keys{} map (all configured providers) → cross-provider routing + fallback
let active = null;         // the user's chosen active cfg (for geminiTier + routing tiebreak)
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
const TOOL_ICON = { list_files: '📁', read_file: '📄', search_files: '🔎', make_dir: '📁', write_file: '✍️', edit_file: '✏️', append_file: '➕', delete_file: '🗑️', move_file: '🔀', run_js: '⚙️', open_in_os: '🪟', device_status: '📊', list_apps: '🧩', weather: '🌤️', web_search: '🌐', scaffold_app: '🧱', publish_app: '🚀', manage_app: '🎛️', generate_image: '🎨', transcribe: '🎙️' };
function toolStart(ev) {
  const d = document.createElement('div'); d.className = 'tool run';
  const p = ev.input.path || ev.input.from || ev.input.app || ev.input.id || ev.input.name || '';
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
    const labels = { write_file: t('op_write_file'), edit_file: t('op_edit_file'), append_file: t('op_append_file'), delete_file: t('op_delete_file'), move_file: t('op_move_file'), run_js: t('op_run_js'), scaffold_app: t('op_scaffold_app'), publish_app: t('op_publish_app'), manage_app: t('op_manage_app'), generate_image: t('op_generate_image') };
    let bodyHtml = '';
    if (req.op === 'edit_file') bodyHtml = diffHtml(req.old, req.new);
    else if (req.op === 'write_file' || req.op === 'append_file') bodyHtml = '<pre>' + esc(String(req.content || '').slice(0, 4000)) + (String(req.content || '').length > 4000 ? '\n…' : '') + '</pre>';
    else if (req.op === 'run_js') bodyHtml = '<pre>' + esc(String(req.code || '').slice(0, 4000)) + '</pre>';
    else if (req.op === 'move_file') bodyHtml = '<div class="pth">' + esc(req.from) + ' → ' + esc(req.to) + '</div>';
    else if (req.op === 'scaffold_app') bodyHtml = '<div class="pth">' + esc(t('appr_new_app')) + ': ' + esc(req.name || req.id || '') + '</div>' + (req.description ? '<pre>' + esc(req.description) + '</pre>' : '');
    else if (req.op === 'publish_app') bodyHtml = '<div class="pth">' + esc(t('appr_install_app')) + ': <b>' + esc(req.id || '') + '</b> → /apps/' + esc(req.id || '') + '</div>';
    else if (req.op === 'manage_app') bodyHtml = '<div class="pth">' + esc(String(req.action || '') === 'enable' ? t('appr_enable_app') : t('appr_disable_app')) + ': <b>' + esc(req.id || '') + '</b></div>';
    else if (req.op === 'generate_image') bodyHtml = '<div class="pth">' + esc(req.path || '') + '</div><pre>' + esc(String(req.prompt || '').slice(0, 2000)) + '</pre>';
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
// Returns { cfg, keys, active } or null. `cfg`/`active` is the user's chosen provider (or the strongest
// configured one as a default); `keys` is the FULL keys{} map so the runtime routes a subtask to the best
// model across EVERY configured provider and falls back when one is down. Works with ANY combination:
// only one key → just that provider; all keys → all exploited. Recognises all 4 providers incl. Grok (xai).
function inferProvider(j) {
  if (j.provider && PROVIDERS[j.provider]) return j.provider;
  const b = String(j.base || '');
  if (/anthropic/.test(b)) return 'anthropic';
  if (/generativelanguage/.test(b)) return 'google';
  if (/x\.ai/.test(b)) return 'xai';
  if (j.key) return 'openai';   // bare key, Groq-style base assumed
  return null;
}
function cfgFromEntry(prov, e) {
  const p = providerOf(prov);
  return { provider: prov, base: e.base || p.base, model: e.model || p.def, key: e.key, version: e.version || p.version, geminiTier: e.geminiTier || '' };
}
function pickCfg(j) {
  j = j || {};
  const keys = (j.keys && typeof j.keys === 'object') ? j.keys : {};
  // 1) the active provider chosen by the user (top-level), if it actually carries a key
  let active = null;
  const tp = inferProvider(j);
  if (j.key && tp) active = cfgFromEntry(tp, j);
  // 2) otherwise the strongest configured key in keys{} (Claude → Grok → Groq → Gemini)
  if (!active) {
    for (const prov of ['anthropic', 'xai', 'openai', 'google']) {
      const e = keys[prov];
      if (e && e.key) { active = cfgFromEntry(prov, e); break; }
    }
  }
  if (!active || !active.key) return null;
  return { cfg: active, keys, active };
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

function configuredProviders() { return Object.keys(keys || {}).filter((p) => keys[p] && keys[p].key && PROVIDERS[p]); }
function modelLine() {
  if (cfg.provider === 'anthropic') return t('model_anthropic');
  if (configuredProviders().length > 1) return t('model_multi');
  return (providerOf(cfg.provider).label || cfg.provider) + ' · ' + t('model_tools_suffix');
}
function readyMsg() {
  if (cfg.provider === 'anthropic') return t('ready_anthropic');
  return t('ready_generic') + (configuredProviders().length > 1 ? t('ready_multi_suffix') : '');
}
function rebuildRuntime() {
  const root = ($('ws').value || '/data/agent').trim() || '/data/agent';
  rt = createRuntime({ cfg, root, lang: lang(), ui, keys, active, t });
  $('model-line').textContent = modelLine();
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
    // Shared cached vault read (memoised ~30s in ai.js → collapses repeated device reads). Returns the
    // normalised cfg incl. keys{} (all providers), {unpaired:true}, or null.
    const tc = await readTeacher({ fresh: true });
    if (tc && tc.unpaired) { setStatus(t('st_pair_first'), 'err'); showGate('key'); return; }
    const res = pickCfg(tc || {});
    cfg = res && res.cfg; keys = res && res.keys; active = res && res.active;
  } catch { cfg = null; keys = null; active = null; }
  if (!cfg) { showGate('key'); return; }
  rebuildRuntime();
  setStatus(t('st_ready'), 'ok');
  if (!history.length) addMsg('sys', readyMsg());
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
