/* SPDX-License-Identifier: AGPL-3.0-or-later */
// server-models.js — Settings ▸ AI ▸ "Models on the server": what the hosting server runs for this person
// (capability ai.server, GET /api/ai/server): each model with the context ONE request really gets and the
// longest reply, today's use of the person's daily allowance, and which online providers the server holds a key
// for (the key itself never reaches the browser). "Use with ANIMA" makes that model ANIMA's brain, written into
// the one AI config file (/data/anima/teacher.json, ai.js AI_PATH) beside the other providers, nothing else touched.
// The server decides who may ask and how much: this panel only shows it and picks.

export const SERVER_PROVIDER = 'box';   // the id ANIMA and the setup already use for the server's model

// Short token count: 8192 -> "8k", 1500 -> "1.5k".
export const kTok = (n) => (!n ? '' : n >= 10000 ? Math.round(n / 1000) + 'k' : (Math.round(n / 100) / 10) + 'k');

// The AI config with `model` (from the server at `base`) as ANIMA's brain; every other entry kept.
export function useServerModel(cfg, base, model) {
  const c = cfg && typeof cfg === 'object' && !Array.isArray(cfg) ? { ...cfg } : {};
  const entry = { base, model, key: 'server' };
  c.keys = { ...(c.keys && typeof c.keys === 'object' ? c.keys : {}), [SERVER_PROVIDER]: entry };
  return Object.assign(c, { provider: SERVER_PROVIDER, exec: c.exec || 'browser' }, entry);
}

export function mountServerModels(host, { T, fs, AI_PATH, fetchFn = (...a) => fetch(...a) } = {}) {
  let state = null, cfg = {}, msg = '';
  const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };

  async function load() {
    try {
      const r = await fetchFn('/api/ai/server', { cache: 'no-store', credentials: 'same-origin' });
      state = r.ok ? await r.json() : { ok: false };
    } catch { state = { ok: false }; }
    try { cfg = JSON.parse((await fs.readText(AI_PATH)) || '{}') || {}; } catch { cfg = {}; }
    render();
  }

  function render() {
    host.replaceChildren();
    if (!state || !state.ok) { host.appendChild(el('span', 'hint', T('srvErr'))); return; }
    const models = Array.isArray(state.models) ? state.models : [];
    if (!models.length) host.appendChild(el('div', 'hint', T('srvNone', { why: state.unavailable || '—' })));
    for (const m of models) {
      const row = el('div', 'row srv-row');
      const info = el('div', 'srv-info');
      info.appendChild(el('div', 'srv-name', m.name || m.id));
      const num = (n) => Number(n).toLocaleString(document.documentElement.lang || undefined);   // exact: 8.192, not "8.2k"
      const facts = [m.context ? T('srvContext', { n: num(m.context) }) : '', m.reply_max ? T('srvReply', { n: num(m.reply_max) }) : '']
        .filter(Boolean).join(' · ');
      if (facts) info.appendChild(el('div', 'hint', facts));
      row.appendChild(info);
      const inUse = cfg.provider === SERVER_PROVIDER && cfg.model === m.id;
      if (inUse) row.appendChild(el('span', 'badge live', T('srvInUse')));
      else {
        const b = el('button', 'btn', T('srvUse'));
        b.type = 'button';
        b.addEventListener('click', async () => {
          b.disabled = true;
          try {
            let cur = {};
            try { cur = JSON.parse((await fs.readText(AI_PATH)) || '{}') || {}; } catch { cur = {}; }
            cfg = useServerModel(cur, state.base, m.id);
            await fs.write(AI_PATH, JSON.stringify(cfg, null, 2));
            msg = T('srvSaved', { name: m.name || m.id });
          } catch (e) { msg = T('srvErr') + ' (' + ((e && e.message) || e) + ')'; }
          render();
        });
        row.appendChild(b);
      }
      host.appendChild(row);
    }
    const lim = state.limit || {};
    if (typeof lim.today === 'number') {
      host.appendChild(el('div', 'hint srv-line', lim.day == null ? T('srvTodayNoCap', { n: lim.today })
        : T('srvToday', { n: lim.today, max: lim.day })));
    }
    const NAMES = { anthropic: 'Claude', groq: 'Groq', google: 'Gemini', xai: 'Grok', openai: 'OpenAI' };
    const keys = Object.entries(state.providers || {}).filter(([, v]) => v).map(([k]) => NAMES[k] || k);
    host.appendChild(el('div', 'hint srv-line', keys.length ? T('srvKeys', { list: keys.join(', ') }) : T('srvNoKeys')));
    if (msg) host.appendChild(el('div', 'hint srv-msg', msg));
  }

  load();
  return { render, reload: load };
}
