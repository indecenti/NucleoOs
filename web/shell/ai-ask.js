/* SPDX-License-Identifier: AGPL-3.0-or-later */
// ai-ask.js — "ask the best AI this OS may use right now". ONE decision of WHO answers a plain text request
// (a game coach line, a prompt rewrite, a summary, a copilot answer), shared by every surface that does not need
// a specialised pipeline. ANIMA's own ladder (device commands, WebGPU, WASM, streaming) and the agent's tool loop
// keep theirs; they follow the same policy (NucleoOs docs/anima-engines.md).
//
// It honours the three choices the user makes ONCE for the whole OS:
//   • the mode (anima-mode.js) — Private = nothing leaves this machine: only a model on THIS computer (loopback);
//   • the PC models (ai-engines.js, Settings ▸ AI / Models ▸ Models on your PC): on/off, servers, model per task,
//     and prefer = 'local' ("PC first");
//   • the cloud keys (ai.js vault, /data/anima/teacher.json): the best configured key for the tier (routeFor).
// Order: Private → the PC only. Otherwise the PC first when the user put it first or no usable cloud key exists;
// else the cloud first, with the PC as its fallback.
//
// Before this, Games and Paint each copied a key resolver with its own fixed provider order and hard-coded
// models, never used a PC model, and called the cloud in Private mode too.
//
// DOM-free; every dependency can be injected (deps) — host-tested in tests/suite/shell-ai-ask.test.mjs.
import * as AI from './ai.js';
import * as Engines from './ai-engines.js';
import * as Mode from './anima-mode.js';

const DIFFICULTY = { fast: 'fast', mid: 'mid', max: 'hard' };

// The engines this request may use, in order. No network unless probe (then the PC servers are probed — cached by
// ai-engines — and `pc` names the server + model that would answer).
// → { private, order: ('pc'|'cloud')[], cloud: cfg|null, local: config, pc: { server, model, base }|null }
export async function planEngines({ tier = 'mid', task = 'chat', pin = null, probe = false } = {}, deps = {}) {
  const ai = deps.ai || AI, E = deps.engines || Engines;
  const priv = (deps.readMode ? deps.readMode() : Mode.readMode()) === 'private';
  let cloud = null;
  if (!priv) {
    const t = await Promise.resolve(ai.readTeacher()).catch(() => null);
    // exec 'device' = the firmware itself calls the provider (the device's online tier): not a browser-direct cloud.
    if (t && !t.unpaired && (t.exec || 'browser') !== 'device') {
      const r = ai.routeFor({ difficulty: DIFFICULTY[tier] || 'mid' }, t.keys || {}, t);
      if (r && r.key) cloud = { ...r, tier };
    }
  }
  const local = E.loadLocalConfig();
  const pcAllowed = local.enabled !== false;
  let pc = null;
  if (probe && pcAllowed) {
    try {
      const live = (await E.liveServers({ config: local })).filter((s) => s.status === 'ok' && s.models && s.models.length && (!priv || E.isLoopback(s.base)));
      const s = live[0];
      if (s) {
        const pinned = local.models && local.models[s.id] && local.models[s.id][task];
        pc = { server: s.name, base: s.base, model: pinned || E.pickModel(s.models, task, { perf: E.loadPerf(), base: s.base }) || s.models[0].id };
      }
    } catch {}
  }
  const pcFirst = priv || !cloud || local.prefer === 'local';
  let order = (pcFirst ? ['pc', 'cloud'] : ['cloud', 'pc'])
    .filter((k) => (k === 'cloud' ? !!cloud : pcAllowed && (!probe || !!pc)));
  if (pin) order = order.filter((k) => k === pin);
  return { private: priv, order, cloud, local, pc };
}

// A short, honest label of who would answer first ("Ollama · qwen3.5:9b", "Claude · claude-sonnet-5-5"), or null.
export function planLabel(plan, ai = AI) {
  const k = plan && plan.order[0];
  if (k === 'pc') return plan.pc ? `${plan.pc.server} · ${plan.pc.model}` : 'PC';
  if (k === 'cloud') return `${ai.providerOf(plan.cloud.provider).label} · ${plan.cloud.model}`;
  return null;
}

// Ask. system + user (one turn) or messages ([{role:'user'|'assistant', content}], a conversation).
// tools: an OpenAI-style function list — the answer may then be a call { tool, args } (the PC model is picked for
// the 'agent' task, i.e. tool-capable; the cloud through cloudToolCall). Used by IR Remote and Nearby commands.
// pin: 'pc' | 'cloud' keeps one engine (a game shows who plays and must not switch brains mid-match).
// → { text, tool?, args?, engine: { kind: 'pc'|'cloud', label, model, server?, provider? } }. Throws an AiError:
// the cloud's own (explainAiError says what to do), 'stopped', or 'no_engine' / 'private_no_pc' when nothing may answer.
export async function askAI(opts = {}, deps = {}) {
  const { system = '', user = '', messages = null, tier = 'mid', maxTokens = 1024, temperature = 0.4, signal, pin = null } = opts;
  const tools = opts.tools && opts.tools.length ? opts.tools : null;
  const task = opts.task || (tools ? 'agent' : 'chat');
  const ai = deps.ai || AI, E = deps.engines || Engines;
  const plan = deps.plan || await planEngines({ tier, task, pin }, deps);
  const turns = messages && messages.length ? messages : [{ role: 'user', content: String(user || '') }];
  const stopped = () => new ai.AiError('stopped', 'stopped');
  let cloudErr = null;
  for (const kind of plan.order) {
    if (signal && signal.aborted) throw stopped();
    if (kind === 'pc') {
      try {
        const out = await E.localComplete(task, { messages: [...(system ? [{ role: 'system', content: system }] : []), ...turns],
          ...(tools ? { tools } : {}), temperature, maxTokens, loopbackOnly: plan.private, signal, config: plan.local });
        const call = out && out.toolCalls && out.toolCalls[0];
        if (out && (out.text || call)) {
          const g = out.engine || {};
          return { text: out.text || '', ...(call ? { tool: call.name, args: call.arguments || {} } : {}),
            engine: { kind: 'pc', server: g.server, model: g.model, label: [g.server, g.model].filter(Boolean).join(' · ') || 'PC' } };
        }
      } catch (e) { if (signal && signal.aborted) throw stopped(); }   // a PC failure falls through to the next engine
    } else {
      const meta = {};
      try {
        const r = (tools || (messages && messages.length))
          ? ((await ai.cloudToolCall(plan.cloud, { system, messages: turns, tools: tools || [], signal, maxTokens, temperature, tier, meta })) || {})
          : { text: await ai.cloudComplete(plan.cloud, system, user, maxTokens, { tier, signal, meta }) };
        if (r.text || r.tool) {
          const model = meta.model || plan.cloud.model;
          return { text: r.text || '', ...(r.tool ? { tool: r.tool, args: r.args || {} } : {}),
            engine: { kind: 'cloud', provider: plan.cloud.provider, model, label: `${ai.providerOf(plan.cloud.provider).label} · ${model}` } };
        }
      } catch (e) {
        const err = ai.toAiError(e, plan.cloud);
        if (err.kind === 'stopped') throw err;
        cloudErr = err;
      }
    }
  }
  if (cloudErr) throw cloudErr;
  throw plan.private ? new ai.AiError('private_no_pc', 'Private mode: no model on this computer answered')
    : new ai.AiError('no_engine', 'no AI engine answered');
}
