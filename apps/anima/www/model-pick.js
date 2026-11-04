/* SPDX-License-Identifier: AGPL-3.0-or-later */
// model-pick.js — what ANIMA's model picker (the "◆ Auto ▾" button by the input, and /model) offers, as data.
//
// DOM-free and import-free (shell/ai.js is passed in), so the grouping is tested in Node. index.html draws the
// pop-over and applies the choice. A pick is a small JSON object kept per conversation (session.pick) and as the
// default for new ones (localStorage anima.pick):
//   {kind:'auto', effort?}                      the ladder: the model chosen in Settings ▸ AI first
//   {kind:'private'}                            nothing leaves this computer (AM 'private')
//   {kind:'cloud', provider, model, effort?}    one cloud model by name, with that provider's key from the device
//   {kind:'server', model, base}                a model the hosting server runs (capability ai.server)
//   {kind:'pc', server, model}                  a model on this PC (Ollama, LM Studio… through ai-engines.js)
// A model chosen by name answers every turn alone (no routing to another model behind the user's back).
// Every group is there only where its source exists: no server group without ai.server, no PC group with no
// server running, and the cloud groups are exactly the keys the device holds (the Cardputer's teacher.json keys{}).

export const PICK_KINDS = Object.freeze(['auto', 'private', 'cloud', 'server', 'pc']);
const ID = /^[\w.:/+@-]{1,200}$/;
const EFF = ['low', 'medium', 'high', 'xhigh', 'max'];

export function pickValid(p) {
  if (!p || typeof p !== 'object' || !PICK_KINDS.includes(p.kind)) return false;
  if (p.effort != null && !EFF.includes(p.effort)) return false;
  if (p.kind === 'cloud') return typeof p.provider === 'string' && /^[a-z]{2,20}$/.test(p.provider) && typeof p.model === 'string' && ID.test(p.model);
  if (p.kind === 'server') return typeof p.model === 'string' && ID.test(p.model) && typeof p.base === 'string' && /^\/[\w./-]{0,200}$/.test(p.base);
  if (p.kind === 'pc') return typeof p.model === 'string' && ID.test(p.model) && typeof p.server === 'string' && p.server.length < 80;
  return true;
}
// Identity of a pick (effort excluded: changing the effort keeps the same row ticked).
export function pickKey(p) {
  if (!p) return 'auto';
  if (p.kind === 'cloud') return 'cloud:' + p.provider + ':' + p.model;
  if (p.kind === 'server') return 'server:' + p.model;
  if (p.kind === 'pc') return 'pc:' + p.server + ':' + p.model;
  return p.kind;
}

// The cloud keys the device holds, one per provider: the active one (top level) and teacher.json keys{}. A sealed
// vault's saved key is the VAULT_KEY placeholder: still a key (the device relay adds the real one). A keyless LAN
// server counts when it has an address and a model. The server's own models ('box') are the server group, not here.
export function vaultKeys(AI, t) {
  const out = {};
  if (!t || t.unpaired) return out;
  const usable = (p, c) => p !== 'box' && AI.PROVIDERS[p] && c && (c.key || (AI.PROVIDERS[p].keyOptional && c.base && c.model));
  const entry = (p, c) => ({ provider: p, base: c.base || AI.PROVIDERS[p].base, key: c.key || '', keyHint: c.keyHint || '',
    version: c.version || AI.PROVIDERS[p].version || '', model: c.model || AI.PROVIDERS[p].def, effort: c.effort || '' });
  for (const [p, c] of Object.entries(t.keys || {})) if (usable(p, c)) out[p] = entry(p, c);
  if (usable(t.provider, t)) out[t.provider] = Object.assign(out[t.provider] || {}, entry(t.provider, t));
  return out;
}

// Every chat model of a provider: the known ones first (in their order, only those the key still lists), then any
// other one the provider lists, strongest first — a new model shows up without a release. No live list (offline,
// provider down): the built-in list, plus the saved model when it is not in it.
export function cloudModels(AI, provider, ids, saved) {
  const known = ((AI.PROVIDERS[provider] || {}).models || []).map((e) => e[0]);
  let all = known.slice();
  if (ids && ids.length) {
    const live = AI.rankModels(ids, 'max');
    all = known.filter((k) => ids.includes(k)).concat(live.filter((x) => !known.includes(x)));
    if (!all.length) all = known.slice();
  }
  if (saved && !all.includes(saved)) all.unshift(saved);
  return all;
}

// The groups the pop-over shows. data: { teacher (AI.readTeacher()), live: {provider: ids|null}, server
// ({base, models:[{id,name}], unavailable}) | null, pcs ([{id, label, models:[{id, name}]}]) | null, pick (current) }.
// T(key, vars) is the app's translator. Rows: {p, label, sub, badge, levels} (levels = the effort levels of that
// model, [] when it takes none) or {off:true, label, sub} for an honest "nothing here" line.
export function buildGroups(AI, T, data = {}) {
  const groups = [];
  const vk = vaultKeys(AI, data.teacher);
  const effortOf = (p, model) => {
    const cur = data.pick;
    if (cur && cur.effort && pickKey(cur) === pickKey(p)) return cur.effort;
    const e = p.kind === 'cloud' ? (vk[p.provider] || {}).effort : (p.kind === 'auto' ? (data.teacher && data.teacher.effort) : '');
    return AI.normEffort(e);
  };
  const act = data.teacher && !data.teacher.unpaired && vk[data.teacher.provider] ? vk[data.teacher.provider] : null;
  const autoSub = act ? T('mpAutoFrom', { model: AI.providerOf(act.provider).label + ' ' + AI.modelName(act.provider, act.model).name }) : T('mpAutoSub');
  const auto = { kind: 'auto' };
  groups.push({ g: '', items: [
    { p: auto, model: act ? act.model : '', label: T('modeAuto'), sub: autoSub, badge: T('mpRecommended'), levels: act ? AI.effortLevels(act.provider, act.model) : [], effort: effortOf(auto) },
    { p: { kind: 'private' }, label: T('modePrivate'), sub: T('engPrivDesc'), badge: T('mpPrivate'), levels: [] },
  ] });
  for (const [prov, c] of Object.entries(vk)) {
    const P = AI.providerOf(prov);
    const items = cloudModels(AI, prov, (data.live || {})[prov], c.model).map((id) => {
      const n = AI.modelName(prov, id), p = { kind: 'cloud', provider: prov, model: id };
      return { p, label: n.name, sub: n.desc || '', badge: id === c.model ? T('mpSaved') : '', levels: AI.effortLevels(prov, id), effort: effortOf(p) };
    });
    groups.push({ g: T('mpKeyDevice', { provider: P.label }), items });
  }
  if (data.server) {
    const ms = Array.isArray(data.server.models) ? data.server.models : [];
    groups.push({ g: T('mpServer'), items: ms.length
      ? ms.map((m) => ({ p: { kind: 'server', model: m.id, base: data.server.base }, label: m.name || m.id, sub: m.name && m.name !== m.id ? m.id : '', badge: '', levels: [] }))
      : [{ off: true, label: T('mpServerNone'), sub: data.server.unavailable || '' }] });
  }
  for (const s of (data.pcs || [])) {
    if (!s || !Array.isArray(s.models) || !s.models.length) continue;
    groups.push({ g: T('mpPc', { server: s.label || s.id }), items: s.models.map((m) => ({
      p: { kind: 'pc', server: s.id, model: m.id }, label: m.name || m.id, sub: '', badge: T('mpPrivate'), levels: [] })) });
  }
  const any = Object.keys(vk).length > 0 || !!(data.server && (data.server.models || []).length) || (data.pcs || []).some((s) => s && s.models && s.models.length);
  return { groups, any };
}

// Rows of `groups` that match the search text (accents and case folded, group name included).
export const fold = (x) => String(x || '').toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '');
export function filterGroups(groups, q) {
  const f = fold(q).trim();
  if (!f) return groups;
  return groups.map((g) => ({ g: g.g, items: g.items.filter((it) => !it.off && fold(it.label + ' ' + (it.sub || '') + ' ' + g.g + ' ' + ((it.p && it.p.model) || '')).includes(f)) }))
    .filter((g) => g.items.length);
}

// The next effort level of `levels` from `cur`, dir = +1 / -1 (clamped at both ends).
export function stepEffort(levels, cur, dir) {
  if (!levels || !levels.length) return '';
  let i = levels.indexOf(cur);
  if (i < 0) i = Math.max(0, levels.indexOf('medium'));
  return levels[Math.max(0, Math.min(levels.length - 1, i + (dir < 0 ? -1 : 1)))];
}
