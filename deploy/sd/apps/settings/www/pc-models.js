/* SPDX-License-Identifier: AGPL-3.0-or-later */
// pc-models.js — Settings ▸ AI ▸ "Models on your PC": the full view of the AI servers on THIS computer (Ollama,
// LM Studio, llama.cpp, Jan …) that ANIMA, the copilot and Agenti use through /ai-engines.js — no key, no Internet.
//
// It edits only ai.local.engines, which is per browser ("localhost" means this computer): on/off, the server list
// (the four defaults + any server the user adds), who answers first when a cloud key exists (prefer) and the model
// per task (none = automatic: ranked from what was measured on this computer). ANIMA's engines panel is the quick
// view of the same settings and links here. A server that runs but refuses NucleoOS's origin (CORS) gets the
// step-by-step fix from /local-ai-help.js, the same one ANIMA shows.
//
// The pure pieces (chatModels, serverRows, addServer, removeServer, setServerEnabled, setTaskModel, setPrefer)
// are host-tested in tools/settings-pc-models.test.mjs; mountPcModels is the DOM half.

export const TASKS = ['chat', 'code', 'agent'];

// A server's models minus the embedding-only ones (they cannot chat) — the same filter as ANIMA's panel.
export function chatModels(models) {
  return (models || []).filter((m) => m && m.id && !(m.caps && m.caps.embedding && !m.caps.chat) && !/embed|bge|minilm|e5-/i.test(m.id));
}

// One row per configured server, joined with what the last probe found ('unknown' until a probe ran).
export function serverRows(cfg, live) {
  const byId = new Map((live || []).map((s) => [s.id, s]));
  return ((cfg && cfg.servers) || []).map((s) => {
    const l = byId.get(s.id), on = s.enabled !== false;
    return {
      id: s.id, name: s.name, base: s.base, kind: s.kind, enabled: on, custom: !!s.custom,
      status: !on ? 'off' : l ? l.status : 'unknown',
      models: on && l && l.status === 'ok' ? chatModels(l.models) : [],
    };
  });
}

// Add a server by its address. → { cfg, error: '' | 'bad' | 'dup', server }. Never mutates the input config.
// Ollama's API lives at the server root (/api/tags); an OpenAI-compatible server given without a path gets /v1,
// where LM Studio, llama.cpp, Jan, vLLM and LocalAI all serve /models and /chat/completions.
export function addServer(cfg, { kind, url } = {}) {
  let u;
  try { u = new URL(String(url || '').trim()); } catch { return { cfg, error: 'bad' }; }
  if (!/^https?:$/.test(u.protocol) || !u.hostname || u.username || u.password) return { cfg, error: 'bad' };
  const k = kind === 'openai' ? 'openai' : 'ollama';
  const path = u.pathname.replace(/\/+$/, '');
  const base = k === 'ollama' ? u.origin : u.origin + (path || '/v1');
  const norm = (b) => String(b || '').replace(/\/+$/, '').toLowerCase();
  const servers = (cfg && cfg.servers) || [];
  if (servers.some((s) => norm(s.base) === norm(base))) return { cfg, error: 'dup' };
  const stem = 'custom-' + (k === 'openai' ? 'oai-' : '') + u.host.toLowerCase().replace(/[^a-z0-9]+/g, '-');
  let id = stem;
  for (let n = 2; servers.some((s) => s.id === id); n++) id = stem + '-' + n;
  const server = { id, kind: k, name: u.host, base, enabled: true, custom: true };
  return { cfg: { ...cfg, servers: [...servers, server] }, error: '', server };
}

// Only a server the user added can be removed (the four defaults are switched off instead); its model picks go too.
export function removeServer(cfg, id) {
  const servers = ((cfg && cfg.servers) || []).filter((s) => !(s.id === id && s.custom));
  const models = { ...((cfg && cfg.models) || {}) };
  if (servers.length !== ((cfg && cfg.servers) || []).length) delete models[id];
  return { ...cfg, servers, models };
}

export function setServerEnabled(cfg, id, on) {
  return { ...cfg, servers: ((cfg && cfg.servers) || []).map((s) => (s.id === id ? { ...s, enabled: !!on } : s)) };
}

// The model one task uses on one server; '' = automatic. A server with every task automatic has no entry at all,
// which is what ai-engines.localComplete reads as "rank the models for the task".
export function setTaskModel(cfg, serverId, task, model) {
  const models = { ...((cfg && cfg.models) || {}) };
  const cur = { ...(models[serverId] || {}) };
  if (model) cur[task] = String(model); else delete cur[task];
  if (Object.keys(cur).length) models[serverId] = cur; else delete models[serverId];
  return { ...cfg, models };
}

// 'local' = the PC answers before the cloud; anything else = the default order (cloud first when a key exists).
export function setPrefer(cfg, p) {
  const c = { ...cfg };
  if (p === 'local') c.prefer = 'local'; else delete c.prefer;
  return c;
}

const esc = (s) => String(s == null ? '' : s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const fill = (s, vars) => String(s).replace(/\{(\w+)\}/g, (m, k) => (vars && k in vars ? String(vars[k]) : m));

// host: an element to render into. T(key) → the Settings string; CT(key, vars) → the core catalog (lai_*, cap_*).
// engines / help: the /ai-engines.js and /local-ai-help.js modules (injected so the caller owns the imports).
// onChange(): called after every saved change and every finished probe (Settings re-runs its AI profiles).
// → { render(), detect() } — render() re-labels after a language change; detect() probes the servers again.
export function mountPcModels(host, { T, CT, engines: E, help, onChange = () => {}, origin = globalThis.location && globalThis.location.origin } = {}) {
  let cfg = E.loadLocalConfig();
  let live = null, probing = false, msg = '', msgKind = '';
  const openHelp = new Set();
  const save = (next, { reprobe = false } = {}) => {
    cfg = next; E.saveLocalConfig(cfg);
    if (reprobe) { E.forgetServers(); detect(); } else { render(); onChange(); }
  };
  const ST = {
    ok: ['ok', (r) => fill(T('pcStOk'), { n: r.models.length })], cors: ['warn', () => T('pcStCors')],
    blocked: ['bad', () => T('pcStBlocked')], down: ['dim', () => T('pcStDown')], error: ['bad', () => T('pcStError')],
    off: ['dim', () => T('pcStOff')], unknown: ['dim', () => (probing ? T('pcDetecting') : '—')],
  };

  function modelSelect(row, task) {
    const perf = E.loadPerf();
    const auto = E.pickModel(row.models, task, { perf, base: row.base }) || '';
    const pin = (cfg.models && cfg.models[row.id] && cfg.models[row.id][task]) || '';
    const opt = (id, label, sel) => `<option value="${esc(id)}"${sel ? ' selected' : ''}>${esc(label)}</option>`;
    const rows = row.models.map((m) => {
      const tps = ((perf.models || {})[row.base + '|' + m.id] || {}).tps || 0;
      return opt(m.id, m.id + (m.sizeGB ? ' · ' + m.sizeGB + ' GB' : '') + (tps ? ' · ' + Math.round(tps) + ' tok/s' : ''), m.id === pin);
    });
    // a pinned model the server no longer lists stays visible (and selected) instead of silently reading "automatic"
    if (pin && !row.models.some((m) => m.id === pin)) rows.unshift(opt(pin, pin + ' · ' + T('pcGone'), true));
    return `<label class="pc-task"><span>${esc(T('pcTask_' + task))}</span><select data-task="${task}" data-srv="${esc(row.id)}">`
      + opt('', fill(T('pcModelAuto'), { m: auto || '—' }), !pin) + rows.join('') + '</select></label>';
  }

  function render() {
    const rows = serverRows(cfg, live);
    const okRows = rows.filter((r) => r.status === 'ok' && r.models.length);
    const anyUp = rows.some((r) => r.status === 'ok' || r.status === 'cors');
    const off = cfg.enabled === false;
    const pcFirst = cfg.prefer === 'local';
    host.innerHTML = ''
      + `<div class="row"><label>${esc(T('pcUse'))}</label><div class="ctl" style="flex-wrap:wrap"><span class="sw"><input type="checkbox" id="pc-enabled"${off ? '' : ' checked'} aria-label="${esc(T('pcUse'))}"><span></span></span>`
      + `<span class="hint" style="flex:1">${esc(off ? T('pcOffHint') : probing ? T('pcDetecting') : live && !anyUp ? T('pcNone') : '')}</span>`
      + `<button type="button" id="pc-detect"${probing || off ? ' disabled' : ''}>↻ ${esc(T('pcDetect'))}</button></div></div>`
      + `<div class="pc-list${off ? ' pc-off' : ''}">`
      + rows.map((r) => {
        const [cls, txt] = ST[r.status] || ST.unknown;
        const fix = (r.status === 'cors' || r.status === 'blocked')
          ? ` <button type="button" class="pc-fix" data-fix="${esc(r.id)}" aria-expanded="${openHelp.has(r.id)}">${esc(T('pcFix'))}</button>` : '';
        const rm = r.custom ? ` <button type="button" class="pc-rm" data-rm="${esc(r.id)}" title="${esc(T('pcRemove'))}" aria-label="${esc(T('pcRemove') + ' ' + r.name)}">✕</button>` : '';
        return `<div class="row pc-srv"><label><span class="sw"><input type="checkbox" data-srv-on="${esc(r.id)}"${r.enabled ? ' checked' : ''} aria-label="${esc(r.name)}"><span></span></span> ${esc(r.name)}</label>`
          + `<div class="ctl" style="flex-wrap:wrap;gap:8px"><code class="pc-base">${esc(r.base)}</code><span class="pc-st ${cls}" role="status">${esc(txt(r))}</span>${fix}${rm}</div></div>`
          + (openHelp.has(r.id) ? `<div class="pc-help" data-help="${esc(r.id)}"></div>` : '');
      }).join('')
      + '</div>'
      + okRows.map((r) => `<div class="row pc-models"><label>${esc(fill(T('pcModelsFor'), { s: r.name }))}</label><div class="ctl pc-tasks">${TASKS.map((tk) => modelSelect(r, tk)).join('')}</div></div>`).join('')
      + (okRows.length ? `<div class="hint pc-note">${esc(T('pcAutoHint'))}</div>` : '')
      + `<div class="row"><label>${esc(T('pcOrder'))}</label><div class="ctl" style="flex-wrap:wrap"><div class="seg-row" id="pc-order" role="group" aria-label="${esc(T('pcOrder'))}">`
      + `<div class="seg-item${pcFirst ? '' : ' active'}" data-p="cloud" role="button" tabindex="0" aria-pressed="${!pcFirst}">${esc(T('pcCloudFirst'))}</div>`
      + `<div class="seg-item${pcFirst ? ' active' : ''}" data-p="local" role="button" tabindex="0" aria-pressed="${pcFirst}">${esc(T('pcPcFirst'))}</div></div>`
      + `<span class="hint" style="flex:1;min-width:200px">${esc(T('pcOrderHint'))}</span></div></div>`
      + `<div class="row" style="border-bottom:none"><label>${esc(T('pcAdd'))}</label><div class="ctl" style="flex-wrap:wrap;gap:8px">`
      + `<select id="pc-add-kind" aria-label="${esc(T('pcAddKind'))}"><option value="ollama">Ollama</option><option value="openai">${esc(T('pcKindOpenai'))}</option></select>`
      + `<input type="text" id="pc-add-url" placeholder="http://192.168.1.20:11434" style="flex:1;min-width:190px" aria-label="${esc(T('pcAdd'))}" spellcheck="false" autocomplete="off">`
      + `<button type="button" id="pc-add-btn">${esc(T('pcAddBtn'))}</button></div></div>`
      + `<div class="hint pc-msg ${msgKind}" role="status">${esc(msg)}</div>`;
    wire();
  }

  function wire() {
    const $ = (s) => host.querySelector(s);
    $('#pc-enabled').addEventListener('change', (e) => { const on = e.target.checked; save({ ...cfg, enabled: on }, { reprobe: on }); });
    $('#pc-detect').addEventListener('click', () => detect());
    host.querySelectorAll('[data-srv-on]').forEach((el) => el.addEventListener('change', () => save(setServerEnabled(cfg, el.dataset.srvOn, el.checked), { reprobe: el.checked })));
    host.querySelectorAll('[data-rm]').forEach((el) => el.addEventListener('click', () => { openHelp.delete(el.dataset.rm); save(removeServer(cfg, el.dataset.rm)); }));
    host.querySelectorAll('[data-fix]').forEach((el) => el.addEventListener('click', () => { const id = el.dataset.fix; if (openHelp.has(id)) openHelp.delete(id); else openHelp.add(id); render(); }));
    host.querySelectorAll('select[data-task]').forEach((el) => el.addEventListener('change', () => save(setTaskModel(cfg, el.dataset.srv, el.dataset.task, el.value))));
    host.querySelectorAll('#pc-order .seg-item').forEach((el) => {
      const pick = () => save(setPrefer(cfg, el.dataset.p));
      el.addEventListener('click', pick);
      el.addEventListener('keydown', (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); pick(); } });
    });
    const add = () => {
      const r = addServer(cfg, { kind: $('#pc-add-kind').value, url: $('#pc-add-url').value });
      if (r.error) { msg = T(r.error === 'dup' ? 'pcAddDup' : 'pcAddBad'); msgKind = 'bad'; render(); return; }
      msg = fill(T('pcAddOk'), { s: r.server.name }); msgKind = 'ok';
      save(r.cfg, { reprobe: true });
    };
    $('#pc-add-btn').addEventListener('click', add);
    $('#pc-add-url').addEventListener('keydown', (e) => { if (e.key === 'Enter') { e.preventDefault(); add(); } });
    // the step-by-step fix for a server that runs but refuses this page (CORS) or that the browser blocks
    for (const el of host.querySelectorAll('[data-help]')) {
      const r = serverRows(cfg, live).find((x) => x.id === el.dataset.help);
      if (!r) continue;
      if (r.status === 'blocked') { el.innerHTML = `<p class="hint">${esc(T('pcBlockedHint'))}</p>`; continue; }
      try {
        help.renderLocalAiHelp(el, { server: r, origin }, CT, { onVerified: () => { openHelp.delete(r.id); E.forgetServers(); detect(); } });
      } catch { el.textContent = T('pcStCors'); }
    }
  }

  async function detect() {
    if (cfg.enabled === false) { live = null; render(); return; }
    probing = true; render();
    try { E.forgetServers(); live = await E.liveServers({ config: cfg, fresh: true }); }
    catch { live = []; }
    probing = false; render(); onChange();
  }

  render();
  detect();
  return { render, detect };
}
