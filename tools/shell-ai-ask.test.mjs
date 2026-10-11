// web/shell/ai-ask.js — the ONE "who answers" decision for plain AI requests (Games coach, Paint prompts, Recorder
// summaries, the copilot's order). Private never reaches the cloud; "PC first" and "no key" put the PC first; a cloud
// failure falls back to the PC; a pinned engine never switches. Run: node --test tools/shell-ai-ask.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import * as AI from '../web/shell/ai.js';
import * as Engines from '../web/shell/ai-engines.js';
import { planEngines, planLabel, askAI } from '../web/shell/ai-ask.js';

const CLAUDE = { provider: 'anthropic', key: 'sk-ant-test', model: 'claude-sonnet-5-5', keys: { anthropic: { key: 'sk-ant-test' } } };

// deps: a vault (teacher), the OS mode, the PC config, and recorders for every engine call
function world({ teacher = null, mode = 'auto', local = {}, pcReply = null, cloudReply = 'from the cloud', cloudThrows = null, live = null } = {}) {
  const calls = { pc: [], cloud: [], tool: [] };
  const config = { enabled: true, servers: Engines.DEFAULT_SERVERS.map((s) => ({ ...s, enabled: true })), models: {}, ...local };
  const ai = {
    ...AI,
    readTeacher: async () => teacher,
    cloudComplete: async (cfg, system, user, maxTokens, o) => { calls.cloud.push({ cfg, system, user }); if (cloudThrows) throw cloudThrows; if (o && o.meta) o.meta.model = 'served-model'; return cloudReply; },
    cloudToolCall: async (cfg, o) => { calls.tool.push(o); if (o.meta) o.meta.model = 'served-model'; return { text: cloudReply }; },
  };
  const engines = {
    ...Engines,
    loadLocalConfig: () => config,
    loadPerf: () => ({ models: {} }),
    liveServers: async () => live || [],
    localComplete: async (task, o) => { calls.pc.push({ task, ...o }); return pcReply ? { text: pcReply, engine: { server: 'Ollama', model: 'qwen3.5:9b' } } : null; },
  };
  return { deps: { ai, engines, readMode: () => mode }, calls };
}

test('Private: only the PC, never the cloud — even with a key; nothing running = private_no_pc', async () => {
  const w = world({ teacher: CLAUDE, mode: 'private', pcReply: 'local answer' });
  const plan = await planEngines({}, w.deps);
  assert.deepEqual(plan.order, ['pc']); assert.equal(plan.cloud, null);
  const r = await askAI({ system: 's', user: 'u' }, w.deps);
  assert.equal(r.text, 'local answer'); assert.equal(r.engine.kind, 'pc'); assert.equal(w.calls.cloud.length, 0);
  assert.equal(w.calls.pc[0].loopbackOnly, true, 'Private keeps to this computer');
  const off = world({ teacher: CLAUDE, mode: 'private' });
  await assert.rejects(askAI({ user: 'u' }, off.deps), (e) => e.kind === 'private_no_pc');
  assert.equal(off.calls.cloud.length, 0);
});

test('Auto with a key: cloud first, the PC as its fallback; the label names the model that answered', async () => {
  const w = world({ teacher: CLAUDE, pcReply: 'pc' });
  assert.deepEqual((await planEngines({}, w.deps)).order, ['cloud', 'pc']);
  const r = await askAI({ system: 'sys', user: 'hi' }, w.deps);
  assert.equal(r.engine.kind, 'cloud'); assert.equal(r.engine.label, 'Claude · served-model');
  assert.equal(w.calls.pc.length, 0);
});

test('"PC first" or no usable key puts the PC first; no key and no PC = no_engine', async () => {
  const pcFirst = world({ teacher: CLAUDE, local: { prefer: 'local' }, pcReply: 'pc answer' });
  assert.deepEqual((await planEngines({}, pcFirst.deps)).order, ['pc', 'cloud']);
  assert.equal((await askAI({ user: 'x' }, pcFirst.deps)).engine.label, 'Ollama · qwen3.5:9b');
  const noKey = world({ teacher: { provider: 'anthropic', key: '', keys: {} }, pcReply: 'pc' });
  assert.deepEqual((await planEngines({}, noKey.deps)).order, ['pc']);
  const nothing = world({ teacher: null });
  await assert.rejects(askAI({ user: 'x' }, nothing.deps), (e) => e.kind === 'no_engine');
});

test('a failing cloud falls back to the PC; with no PC the cloud\'s own error is what the user reads', async () => {
  const rate = new AI.AiError('rate', 'Rate limit reached', { provider: 'anthropic', status: 429 });
  const w = world({ teacher: CLAUDE, cloudThrows: rate, pcReply: 'rescued by the PC' });
  const r = await askAI({ user: 'x' }, w.deps);
  assert.equal(r.text, 'rescued by the PC'); assert.equal(w.calls.cloud.length, 1);
  const alone = world({ teacher: CLAUDE, cloudThrows: rate });
  await assert.rejects(askAI({ user: 'x' }, alone.deps), (e) => e.kind === 'rate');
  assert.ok(AI.explainAiError(rate, 'en').length > 10);
});

test('a pinned engine never switches; the PC switched off is never asked; a device-exec vault is not a browser cloud', async () => {
  const w = world({ teacher: CLAUDE, pcReply: 'pc' });
  assert.equal((await askAI({ user: 'x', pin: 'pc' }, w.deps)).engine.kind, 'pc');
  assert.equal(w.calls.cloud.length, 0);
  const off = world({ teacher: CLAUDE, local: { enabled: false }, pcReply: 'pc' });
  assert.deepEqual((await planEngines({}, off.deps)).order, ['cloud']);
  const dev = world({ teacher: { ...CLAUDE, exec: 'device' }, pcReply: 'pc' });
  assert.deepEqual((await planEngines({}, dev.deps)).order, ['pc']);
});

test('a conversation goes through the tool-capable call with no tools; the tier reaches the router', async () => {
  const w = world({ teacher: CLAUDE });
  const r = await askAI({ system: 's', messages: [{ role: 'user', content: 'a' }, { role: 'assistant', content: 'b' }, { role: 'user', content: 'c' }], tier: 'max' }, w.deps);
  assert.equal(r.text, 'from the cloud');
  assert.equal(w.calls.tool.length, 1); assert.deepEqual(w.calls.tool[0].tools, []);
  assert.equal(w.calls.tool[0].messages.length, 3);
  const plan = await planEngines({ tier: 'max' }, w.deps);
  assert.equal(plan.cloud.model, 'claude-opus-5-5', 'max tier → the strongest model of the provider');
});

test('probe names the PC server and model that would answer (Private: localhost only); planLabel is honest', async () => {
  const live = [
    { id: 'lan', name: 'LAN box', base: 'http://192.168.1.20:11434', status: 'ok', models: [{ id: 'big' }] },
    { id: 'ollama', name: 'Ollama', base: 'http://localhost:11434', status: 'ok', models: [{ id: 'qwen3.5:9b', sizeGB: 6.6 }] },
  ];
  const auto = world({ teacher: null, live });
  const p1 = await planEngines({ probe: true }, auto.deps);
  assert.equal(p1.pc.server, 'LAN box'); assert.equal(planLabel(p1), 'LAN box · big');
  const priv = world({ teacher: null, live, mode: 'private' });
  const p2 = await planEngines({ probe: true }, priv.deps);
  assert.equal(p2.pc.server, 'Ollama', 'Private never uses another machine');
  const none = world({ teacher: null, live: [] });
  assert.deepEqual((await planEngines({ probe: true }, none.deps)).order, [], 'probe: no server running = no PC engine');
  const cloud = world({ teacher: CLAUDE });
  assert.equal(planLabel(await planEngines({}, cloud.deps)), 'Claude · claude-sonnet-5-5');
});

test('the two new error kinds read as one plain sentence in all five languages', () => {
  for (const kind of ['no_engine', 'private_no_pc']) for (const l of ['it', 'en', 'es', 'fr', 'de']) {
    const s = AI.explainAiError(new AI.AiError(kind, 'x'), l);
    assert.ok(s.length > 30 && !/\{[A-Z]+\}/.test(s), `${kind}/${l}: ${s}`);
  }
});

test('tools: the PC answers with a call (tool-capable "agent" model), the cloud through cloudToolCall', async () => {
  const TOOLS = [{ type: 'function', function: { name: 'press', description: 'press a key', parameters: { type: 'object', properties: { key: { type: 'string' } } } } }];
  const w = world({ teacher: null });
  w.deps.engines.localComplete = async (task, o) => { w.calls.pc.push({ task, ...o }); return { text: '', toolCalls: [{ name: 'press', arguments: { key: 'power' } }], engine: { server: 'Ollama', model: 'qwen3.5:9b' } }; };
  const r = await askAI({ system: 's', user: 'turn on the TV', tools: TOOLS }, w.deps);
  assert.equal(r.tool, 'press'); assert.deepEqual(r.args, { key: 'power' }); assert.equal(r.engine.kind, 'pc');
  assert.equal(w.calls.pc[0].task, 'agent'); assert.equal(w.calls.pc[0].tools, TOOLS);
  const c = world({ teacher: CLAUDE });
  c.deps.ai.cloudToolCall = async (cfg, o) => { c.calls.tool.push(o); return { tool: 'press', args: { key: 'mute' } }; };
  const rc = await askAI({ user: 'mute', tools: TOOLS }, c.deps);
  assert.equal(rc.tool, 'press'); assert.equal(rc.engine.kind, 'cloud'); assert.equal(c.calls.tool[0].tools, TOOLS);
  const priv = world({ teacher: CLAUDE, mode: 'private' });
  await assert.rejects(askAI({ user: 'mute', tools: TOOLS }, priv.deps), (e) => e.kind === 'private_no_pc');
  assert.equal(priv.calls.tool.length, 0, 'Private: no cloud tool call');
});
