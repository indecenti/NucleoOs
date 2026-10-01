// web/shell/ai-engines.js — local AI servers on the user's PC, straight from the browser (never via the
// Cardputer). Pins: CORS-refused vs down detection (the whole reason the OLLAMA_ORIGINS wizard can be
// specific), Ollama's real per-model capabilities, streaming parsers (NDJSON + SSE, chunk-boundary safe),
// tool calls, and the task → model choice over what the user actually installed.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { probeServer, pickModel, rankModels as rankModelsImpl, ollamaChat, openaiChat, readLines, localComplete, DEFAULT_SERVERS } from '../web/shell/ai-engines.js';

const json = (o, status = 200) => new Response(JSON.stringify(o), { status, headers: { 'content-type': 'application/json' } });
const stream = (chunks) => new Response(new ReadableStream({ start(c) { for (const s of chunks) c.enqueue(new TextEncoder().encode(s)); c.close(); } }));
const OLLAMA_MODELS = [
  { name: 'qwen3.5:9b', size: 6594474711, details: { parameter_size: '9.7B', family: 'qwen35', quantization_level: 'Q4_K_M' } },
  { name: 'qwen3.6-coder:16k', size: 22600000000, details: { parameter_size: '35.5B', family: 'qwen35moe' } },
  { name: 'minicpm5-2b:latest', size: 1600000000, details: { parameter_size: '2.5B', family: 'llama' } },
  { name: 'embeddinggemma:300m', size: 300000000, details: { parameter_size: '300M', family: 'gemma3' } },
];
const CAPS = { 'qwen3.5:9b': ['completion', 'vision', 'tools', 'thinking'], 'qwen3.6-coder:16k': ['completion', 'tools', 'thinking'], 'minicpm5-2b:latest': ['completion', 'tools'], 'embeddinggemma:300m': ['embedding'] };
function ollamaFetch({ origin = 'ok' } = {}) {
  return async (url, init = {}) => {
    if (origin === 'down') throw new TypeError('Failed to fetch');
    if (origin === 'cors') { if (init.mode === 'no-cors') return { type: 'opaque', status: 0, ok: false }; throw new TypeError('Failed to fetch'); }   // what a browser's opaque no-cors answer looks like
    if (url.endsWith('/api/tags')) return json({ models: OLLAMA_MODELS });
    if (url.endsWith('/api/show')) { const m = JSON.parse(init.body).model; return json({ capabilities: CAPS[m] || [], model_info: { 'x.context_length': 32768 } }); }
    throw new Error('unexpected ' + url);
  };
}

test('probeServer: Ollama up → models with their REAL capabilities', async () => {
  const s = await probeServer(DEFAULT_SERVERS[0], { fetch: ollamaFetch() });
  assert.equal(s.status, 'ok');
  assert.equal(s.models.length, 4);
  const q = s.models.find((m) => m.id === 'qwen3.5:9b');
  assert.deepEqual({ tools: q.caps.tools, vision: q.caps.vision, thinking: q.caps.thinking, ctx: q.caps.ctx }, { tools: true, vision: true, thinking: true, ctx: 32768 });
  assert.equal(q.sizeGB, 6.6);
  assert.equal(s.models.find((m) => m.id.startsWith('embeddinggemma')).caps.embedding, true);
});

test('probeServer tells "up but this origin is not allowed" (→ OLLAMA_ORIGINS) from "not running"', async () => {
  assert.equal((await probeServer(DEFAULT_SERVERS[0], { fetch: ollamaFetch({ origin: 'cors' }) })).status, 'cors');
  assert.equal((await probeServer(DEFAULT_SERVERS[0], { fetch: ollamaFetch({ origin: 'down' }) })).status, 'down');
  assert.equal((await probeServer(DEFAULT_SERVERS[0], { fetch: async () => new Response('forbidden', { status: 403 }) })).status, 'cors', 'Ollama answers 403 to a disallowed Origin');
});

test('probeServer: OpenAI-compatible servers list /models', async () => {
  const f = async (url) => (url.endsWith('/v1/models') ? json({ data: [{ id: 'qwen2.5-7b-instruct' }, { id: 'llama-3.2-3b' }] }) : json({}, 404));
  const s = await probeServer(DEFAULT_SERVERS[1], { fetch: f });
  assert.equal(s.status, 'ok');
  assert.deepEqual(s.models.map((m) => m.id), ['qwen2.5-7b-instruct', 'llama-3.2-3b']);
});

test('pickModel chooses per task from what is installed', async () => {
  const { models } = await probeServer(DEFAULT_SERVERS[0], { fetch: ollamaFetch() });
  // Measured 2026-09-30, 8 GB laptop RTX 5070: the 22.6 GB coder does not even start (CUDA out of memory).
  assert.equal(pickModel(models, 'code'), 'qwen3.5:9b', 'unknown GPU (≈8 GB assumed): a 22 GB model would not fit — the general model codes');
  assert.equal(pickModel(models, 'code', { perf: { vramGB: 24, models: {} } }), 'qwen3.6-coder:16k', 'a GPU known to hold it gets the coder (a sparse MoE is fine)');
  assert.equal(pickModel(models, 'chat'), 'qwen3.5:9b', 'the strongest responsive general model for chat');
  assert.equal(pickModel(models, 'agent'), 'qwen3.5:9b', 'agent needs tools');
  assert.equal(pickModel(models, 'vision'), 'qwen3.5:9b');
  assert.equal(pickModel(models, 'embed'), 'embeddinggemma:300m');
  assert.equal(pickModel([], 'chat'), null);
});

test('ollamaChat streams NDJSON across chunk boundaries, collects tool calls and speed', async () => {
  const lines = [
    '{"message":{"role":"assistant","content":"Cia"}}\n{"message":{"content":"o!"}}\n{"mess',
    'age":{"content":"","tool_calls":[{"function":{"name":"add_event","arguments":{"title":"Chiamare Marco","when":"2026-09-30T09:00"}}}]}}\n',
    '{"done":true,"prompt_eval_count":40,"eval_count":20,"eval_duration":500000000}\n',
  ];
  let sent = null; const deltas = [];
  const f = async (url, init) => { sent = JSON.parse(init.body); return stream(lines); };
  const r = await ollamaChat({ base: 'http://localhost:11434', model: 'qwen3.5:9b', messages: [{ role: 'user', content: 'x' }], tools: [{ type: 'function', function: { name: 'add_event' } }], onDelta: (d) => deltas.push(d), fetch: f });
  assert.equal(r.text, 'Ciao!');
  assert.deepEqual(deltas, ['Cia', 'o!']);
  assert.deepEqual(r.toolCalls, [{ name: 'add_event', arguments: { title: 'Chiamare Marco', when: '2026-09-30T09:00' } }]);
  assert.equal(r.usage.tokPerSec, 40);
  assert.equal(sent.think, false, 'thinking off by default (fast, clean chat)');
  assert.equal(sent.stream, true);
});

test('openaiChat parses SSE deltas and assembles streamed tool-call arguments', async () => {
  const sse = [
    'data: {"choices":[{"delta":{"content":"Hola"}}]}\n\n',
    'data: {"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"name":"open_app","arguments":"{\\"id\\":"}}]}}]}\n\n',
    'data: {"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"\\"notepad\\"}"}}]}}]}\n\ndata: [DONE]\n\n',
  ];
  const r = await openaiChat({ base: 'http://localhost:1234/v1', model: 'm', messages: [], fetch: async () => stream(sse) });
  assert.equal(r.text, 'Hola');
  assert.deepEqual(r.toolCalls, [{ name: 'open_app', arguments: { id: 'notepad' } }]);
});

test('readLines strips a thinking block from the final text', async () => {
  const r = await ollamaChat({ base: 'http://x', model: 'm', messages: [], fetch: async () => stream(['{"message":{"content":"<think>hmm</think>Risposta"}}\n{"done":true}\n']) });
  assert.equal(r.text, 'Risposta');
  const got = []; await readLines(stream(['a\nb', '\n\nc']), (l) => got.push(l)); assert.deepEqual(got, ['a', 'b', 'c']);
});

test('localComplete: null when nothing is reachable, else answers and names the engine', async () => {
  assert.equal(await localComplete('chat', { messages: [], fetch: ollamaFetch({ origin: 'down' }), config: { enabled: true, servers: [DEFAULT_SERVERS[0]], models: {} } }), null);
  const base = ollamaFetch();
  const f = async (url, init) => (url.endsWith('/api/chat') ? stream(['{"message":{"content":"ok"}}\n{"done":true}\n']) : base(url, init));
  const r = await localComplete('chat', { messages: [{ role: 'user', content: 'x' }], fetch: f, config: { enabled: true, servers: [DEFAULT_SERVERS[0]], models: {} } });
  assert.equal(r.text, 'ok');
  assert.deepEqual(r.engine, { server: 'Ollama', kind: 'ollama', model: 'qwen3.5:9b', base: 'http://localhost:11434' });
  assert.equal(await localComplete('chat', { messages: [], fetch: f, config: { enabled: false, servers: [], models: {} } }), null, 'disabled → never used');
});

test('adapts to resources: a model that does not fit in memory falls through to the next lighter one', async () => {
  const base = ollamaFetch();
  const tried = [];
  const f = async (url, init) => {
    if (!url.endsWith('/api/chat')) return base(url, init);
    const body = JSON.parse(init.body);
    tried.push(body.model);
    assert.equal(body.options.num_ctx, 8192, 'bounded context by default (Ollama would size it from VRAM)');
    if (body.model === 'qwen3.5:9b') return new Response('{"error":"llama-server reported out-of-memory during startup: CUDA error"}', { status: 500 });
    return stream(['{"message":{"content":"fatto"}}\n{"done":true}\n']);
  };
  const r = await localComplete('agent', { messages: [{ role: 'user', content: 'x' }], fetch: f, config: { enabled: true, servers: [DEFAULT_SERVERS[0]], models: {} } });
  assert.equal(r.text, 'fatto');
  assert.equal(tried[0], 'qwen3.5:9b');
  assert.notEqual(r.engine.model, 'qwen3.5:9b');
  assert.deepEqual(r.engine.skipped, ['qwen3.5:9b'], 'the answer says which model was skipped and why it fell back');
  const other = async (url, init) => (url.endsWith('/api/chat') ? new Response('{"error":"model not found"}', { status: 404 }) : base(url, init));
  await assert.rejects(() => localComplete('agent', { messages: [], fetch: other, config: { enabled: true, servers: [DEFAULT_SERVERS[0]], models: {} } }), /404/, 'non-memory errors are not hidden by the ladder');
});

test('liveServers: one probe per turn burst, re-probed on a config change or after a failed chat', async () => {
  const { liveServers, forgetServers } = await import('../web/shell/ai-engines.js');
  forgetServers();
  const base = ollamaFetch(); let probes = 0;
  const f = async (url, init) => { if (url.endsWith('/api/tags')) probes++; return url.endsWith('/api/chat') ? new Response('{"error":"model not found"}', { status: 404 }) : base(url, init); };
  const config = { enabled: true, servers: [DEFAULT_SERVERS[0]], models: {} };
  await liveServers({ config, fetch: f }); await liveServers({ config, fetch: f });
  assert.equal(probes, 1, 'second turn re-uses the detection');
  await liveServers({ config: { ...config, servers: [{ ...DEFAULT_SERVERS[0], base: 'http://127.0.0.1:11435' }] }, fetch: f });
  assert.equal(probes, 2, 'a different server list is probed, not served from the cache');
  await liveServers({ config, fetch: f });
  await assert.rejects(() => localComplete('chat', { messages: [], fetch: f, config }), /404/);
  await liveServers({ config, fetch: f });
  assert.equal(probes, 4, 'a failed chat drops the cache: the next turn re-probes');
  forgetServers();
});

test('localComplete: Private keeps to THIS computer; the ANIMA budget reaches the server', async () => {
  const { forgetServers } = await import('../web/shell/ai-engines.js');
  const base = ollamaFetch(); const bodies = [];
  const f = async (url, init) => { if (url.endsWith('/api/chat')) { bodies.push(JSON.parse(init.body)); return stream(['{"message":{"content":"ok"}}\n{"done":true}\n']); } return base(url, init); };
  const lan = { ...DEFAULT_SERVERS[0], id: 'lan', base: 'http://192.168.1.50:11434' };
  forgetServers();
  assert.equal(await localComplete('chat', { messages: [], loopbackOnly: true, fetch: f, config: { enabled: true, servers: [lan], models: {} } }), null, 'a LAN server is the network: not used in Private');
  forgetServers();
  const r = await localComplete('chat', { messages: [], temperature: 0.4, maxTokens: 1024, loopbackOnly: true, fetch: f, config: { enabled: true, servers: [DEFAULT_SERVERS[0]], models: {} } });
  assert.equal(r.text, 'ok');
  assert.equal(r.engine.base, 'http://localhost:11434', 'the answer names where it ran');
  assert.deepEqual(bodies[0].options, { num_ctx: 8192, temperature: 0.4, num_predict: 1024 });
  const oa = []; const g = async (url, init) => { oa.push(JSON.parse(init.body)); return stream(['data: {"choices":[{"delta":{"content":"y"}}]}\n', 'data: [DONE]\n']); };
  await openaiChat({ base: 'http://localhost:1234/v1', model: 'm', messages: [], temperature: 0.2, maxTokens: 512, fetch: g });
  assert.equal(oa[0].temperature, 0.2); assert.equal(oa[0].max_tokens, 512);
  forgetServers();
});

test('adapts to THIS computer: measured speed and VRAM beat the size estimate', async () => {
  const { notePerf, loadPerf, savePerf, ollamaLoaded, DEFAULT_VRAM_GB, forgetServers } = await import('../web/shell/ai-engines.js');
  const models = [
    { id: 'gemma4:12b', sizeGB: 7.56, params: '11.9B', family: 'gemma4', caps: { chat: true, tools: true } },   // the real /api/tags sizes
    { id: 'qwen3.5:9b', sizeGB: 6.59, params: '9.7B', family: 'qwen35', caps: { chat: true, tools: true } },
  ];
  const base = 'http://localhost:11434';
  assert.equal(pickModel(models, 'chat'), 'qwen3.5:9b', 'nothing measured: a 7.56 GB file does not fit the ≈7 GB an 8 GB GPU leaves');
  assert.equal(pickModel(models, 'chat', { perf: { vramGB: 12, models: {} } }), 'gemma4:12b', 'a GPU known to be bigger gets the bigger model');
  // what the real laptop showed: gemma4 spilled a third to the CPU at 2.1 tok/s, qwen3.5 answered at 20.5 tok/s
  let perf = notePerf({ vramGB: 12, models: {} }, base, 'gemma4:12b', { tps: 2.1, sizeGB: 8.9, vramGB: 5.96 });
  assert.equal(perf.vramGB, 6, 'a model that spilled pins the VRAM estimate to what the GPU held');
  assert.equal(pickModel(models, 'chat', { perf, base }), 'qwen3.5:9b', 'too slow here → the next model answers from now on');
  perf = notePerf(perf, base, 'qwen3.5:9b', { tps: 20.5, sizeGB: 6.25, vramGB: 6.25 });
  assert.equal(perf.models[base + '|qwen3.5:9b'].fit, 1);
  assert.equal(perf.vramGB, 6.3, 'a model that fit raises the measured estimate to at least its size');
  assert.equal(notePerf({ models: {} }, base, 'small', { tps: 30, sizeGB: 2, vramGB: 2 }).vramGB, DEFAULT_VRAM_GB, 'a small model that fit never LOWERS the default estimate');
  assert.deepEqual(rankModelsOf(models, perf, base), ['qwen3.5:9b', 'gemma4:12b'], 'the slow one stays a fallback, not removed');
  // persisted per browser, tiny
  const mem = new Map(); const storage = { getItem: (k) => mem.get(k) ?? null, setItem: (k, v) => mem.set(k, v) };
  savePerf(perf, storage); assert.equal(loadPerf(storage).models[base + '|gemma4:12b'].tps, 2.1);
  let p2 = { models: {} }; for (let i = 0; i < 40; i++) p2 = notePerf(p2, base, 'm' + i, { tps: 10 });
  assert.equal(Object.keys(p2.models).length, 24, 'bounded memo');
  // /api/ps parsing + localComplete records the measurement after an answer
  const ps = async (url) => new Response(JSON.stringify({ models: [{ name: 'qwen3.5:9b', size: 6.25e9, size_vram: 4.8e9 }] }));
  assert.deepEqual((await ollamaLoaded(base, { fetch: ps }))[0], { id: 'qwen3.5:9b', sizeGB: 6.25, vramGB: 4.8 });
  const bse = ollamaFetch();
  const f = async (url, init) => url.endsWith('/api/chat') ? stream(['{"message":{"content":"ok"}}\n{"done":true,"eval_count":40,"eval_duration":2000000000}\n'])
    : url.endsWith('/api/ps') ? ps(url) : bse(url, init);
  forgetServers();
  const store = new Map(); const st = { getItem: (k) => store.get(k) ?? null, setItem: (k, v) => store.set(k, v) };
  const r = await localComplete('chat', { messages: [], fetch: f, storage: st, config: { enabled: true, servers: [DEFAULT_SERVERS[0]], models: {} } });
  assert.equal(r.text, 'ok');
  for (let i = 0; i < 20 && !store.get('ai.local.perf'); i++) await new Promise((res) => setTimeout(res, 5));
  const saved = JSON.parse(store.get('ai.local.perf'));
  assert.equal(saved.models[base + '|' + r.engine.model].tps, 20, 'speed learned from the real answer (40 tokens / 2 s)');
  forgetServers();
});
function rankModelsOf(models, perf, base) { return rankModelsImpl(models, 'chat', { perf, base }); }

test('a model bigger than the GPU loads split GPU/CPU instead of being skipped, and the split is remembered', async () => {
  // Measured 2026-10-01, qwen3.6 35B-A3B (22.6 GB, 41 layers) on an 8 GB RTX 5070 laptop: Ollama's own split
  // died "CUDA error: out of memory"; 12 layers on the GPU → 22.4 tok/s; 20 layers → 3.8 tok/s (spilled).
  const { gpuLayersFor, forgetServers, notePerf } = await import('../web/shell/ai-engines.js');
  assert.equal(gpuLayersFor({ sizeGB: 22.6, blockCount: 41, vramGB: 8 }), 11, 'the layers that fit, with room for the KV cache');
  assert.equal(gpuLayersFor({ sizeGB: 22.6, blockCount: 41 }), 9, 'default VRAM estimate: a little more on the CPU, still fast');
  assert.equal(gpuLayersFor({ sizeGB: 4, blockCount: 30, vramGB: 8 }), 30, 'never more layers than the model has');
  assert.equal(gpuLayersFor({ sizeGB: 22.6, blockCount: 0 }), null, 'unknown layer count: no guess');

  const base = ollamaFetch();
  const sent = [];
  const f = async (url, init) => {
    if (url.endsWith('/api/show')) { const m = JSON.parse(init.body).model; return json({ capabilities: CAPS[m] || [], model_info: { 'qwen35moe.block_count': 41, 'x.context_length': 32768 } }); }
    if (url.endsWith('/api/ps')) return json({ models: [{ name: 'qwen3.6-coder:16k', size: 22.6e9, size_vram: 6.4e9 }] });
    if (!url.endsWith('/api/chat')) return base(url, init);
    const body = JSON.parse(init.body); sent.push({ model: body.model, num_gpu: body.options.num_gpu });
    if (body.model === 'qwen3.6-coder:16k' && body.options.num_gpu == null) return new Response('{"error":"llama-server reported out-of-memory during startup: CUDA error\\nCUDA error: out of memory"}', { status: 500 });
    return stream(['{"message":{"content":"fatto"}}\n{"done":true,"eval_count":44,"eval_duration":2000000000}\n']);
  };
  forgetServers();
  const store = new Map(); const st = { getItem: (k) => store.get(k) ?? null, setItem: (k, v) => store.set(k, v) };
  const cfg = { enabled: true, servers: [DEFAULT_SERVERS[0]], models: { ollama: { code: 'qwen3.6-coder:16k' } } };
  const r = await localComplete('code', { messages: [{ role: 'user', content: 'x' }], fetch: f, storage: st, config: cfg });
  assert.equal(r.text, 'fatto');
  assert.equal(r.engine.model, 'qwen3.6-coder:16k', 'the big model answered — not skipped for the 9B');
  assert.deepEqual(sent.slice(0, 2), [{ model: 'qwen3.6-coder:16k', num_gpu: undefined }, { model: 'qwen3.6-coder:16k', num_gpu: 9 }], 'one retry with the split that fits');
  assert.equal(r.engine.gpuLayers, 9);
  for (let i = 0; i < 20 && !store.get('ai.local.perf'); i++) await new Promise((res) => setTimeout(res, 5));
  const saved = JSON.parse(store.get('ai.local.perf'));
  const rec = saved.models['http://localhost:11434|qwen3.6-coder:16k'];
  assert.equal(rec.numGpu, 9, 'the working split is remembered');
  assert.equal(saved.vramGB, undefined, 'a deliberate split is not read as the GPU size (6.4 GB held is our choice)');
  // next turn: straight to the remembered split, no failed load first
  sent.length = 0; forgetServers();
  await localComplete('code', { messages: [{ role: 'user', content: 'y' }], fetch: f, storage: st, config: cfg, perf: saved });
  assert.deepEqual(sent, [{ model: 'qwen3.6-coder:16k', num_gpu: 9 }]);
  // measured fast here → the big MoE wins the automatic choice too, not only when pinned
  const models = [
    { id: 'qwen3.5:9b', sizeGB: 6.59, params: '9.7B', family: 'qwen35', caps: { chat: true, tools: true } },
    { id: 'qwen3.6:35b-a3b', sizeGB: 22.6, params: '35.5B', family: 'qwen35moe', caps: { chat: true, tools: true } },
  ];
  const b = 'http://localhost:11434';
  assert.equal(pickModel(models, 'chat', { perf: { models: {} }, base: b }), 'qwen3.5:9b', 'never measured: the 22 GB file stays the fallback');
  const fast = notePerf({ models: {} }, b, 'qwen3.6:35b-a3b', { tps: 22.4, sizeGB: 22.6, vramGB: 6.4, numGpu: 12 });
  assert.equal(pickModel(models, 'chat', { perf: fast, base: b }), 'qwen3.6:35b-a3b', 'measured 22 tok/s: speed is the truth, the capable model answers');
  const slow = notePerf({ models: {} }, b, 'qwen3.6:35b-a3b', { tps: 3.8, sizeGB: 22.6, vramGB: 10.5, numGpu: 20 });
  assert.equal(pickModel(models, 'chat', { perf: slow, base: b }), 'qwen3.5:9b', 'measured slow: back to the small one');
  forgetServers();
});

test('a browser that forbids localhost (Local Network Access denied) is "blocked", not a stopped server', async () => {
  const { probeServer, localNetworkPermission } = await import('../web/shell/ai-engines.js');
  const fail = async () => { throw new TypeError('Failed to fetch'); };
  const perm = (state) => ({ query: async ({ name }) => { if (name !== 'loopback-network') throw new TypeError('unknown'); return { state }; } });
  const srv = { id: 'ollama', kind: 'ollama', name: 'Ollama', base: 'http://localhost:11434' };
  assert.equal((await probeServer(srv, { fetch: fail, permissions: perm('denied') })).status, 'blocked');
  assert.equal((await probeServer(srv, { fetch: fail, permissions: perm('granted') })).status, 'down');
  assert.equal((await probeServer({ ...srv, base: 'http://192.168.0.9:11434' }, { fetch: fail, permissions: perm('denied') })).status, 'down', 'only loopback is gated');
  assert.equal(await localNetworkPermission({ permissions: undefined }), null);
});

test('one loopback server that answers proves the browser is not blocking: the stopped ones are just "down"', async () => {
  const { detectServers } = await import('../web/shell/ai-engines.js');
  const perm = { query: async () => ({ state: 'denied' }) };    // what Chrome reports inside the ANIMA iframe
  const f = async (url) => { if (String(url).includes(':11434')) return new Response(JSON.stringify({ models: [] }), { status: 200 }); throw new TypeError('Failed to fetch'); };
  const out = await detectServers({ fetch: f, permissions: perm, servers: [
    { id: 'ollama', kind: 'ollama', name: 'Ollama', base: 'http://localhost:11434' },
    { id: 'lmstudio', kind: 'openai', name: 'LM Studio', base: 'http://localhost:1234/v1' }] });
  assert.deepEqual(out.map((s) => s.status), ['ok', 'down']);
  const none = await detectServers({ fetch: async () => { throw new TypeError('x'); }, permissions: perm, servers: [{ id: 'ollama', kind: 'ollama', name: 'Ollama', base: 'http://localhost:11434' }] });
  assert.equal(none[0].status, 'blocked');
});

test('a model that would win if fast is measured first — the 35B-A3B lost to the 9B forever, unmeasured', async () => {
  const { benchCandidates, pickModel } = await import('../web/shell/ai-engines.js');
  const caps = { chat: true, tools: true };
  const models = [   // the dev PC's real list (sizes in GB)
    { id: 'qwen3.5:9b', sizeGB: 6.6, params: '9.7B', family: 'qwen35', caps },
    { id: 'minicpm5-2b:latest', sizeGB: 1.6, params: '2.5B', family: 'llama', caps: { chat: true, tools: false } },
    { id: 'qwen3.6-coder:16k', sizeGB: 22.6, params: '35.5B', family: 'qwen35moe', caps },
    { id: 'qwen3.6:35b-a3b-mtp-q4_K_M', sizeGB: 22.6, params: '35.5B', family: 'qwen35moe', caps },
    { id: 'gemma4:12b', sizeGB: 7.6, params: '11.9B', family: 'gemma4', caps },
  ];
  const base = 'http://localhost:11434';
  assert.equal(pickModel(models, 'agent', { base }), 'qwen3.5:9b', 'unmeasured: the big file stays a fallback');
  assert.deepEqual(benchCandidates(models, 'agent', { base }), ['qwen3.6:35b-a3b-mtp-q4_K_M']);
  assert.deepEqual(benchCandidates(models, 'code', { base }), ['qwen3.6-coder:16k']);
  const perf = { models: { [base + '|qwen3.6:35b-a3b-mtp-q4_K_M']: { tps: 22.4 } } };
  assert.equal(pickModel(models, 'agent', { perf, base }), 'qwen3.6:35b-a3b-mtp-q4_K_M', 'measured fast: it wins');
  assert.deepEqual(benchCandidates(models, 'agent', { perf, base }), [], 'measured: never again');
  assert.deepEqual(benchCandidates(models, 'agent', { base, tried: { [base + '|qwen3.6:35b-a3b-mtp-q4_K_M']: Date.now() } }), [], 'a failed try waits a week');
});

test('too FEW GPU layers fail too: the retry climbs the ladder until a split loads (7 OOM, 9 ok on the ADV PC)', async () => {
  const { gpuLayerLadder, forgetServers } = await import('../web/shell/ai-engines.js');
  assert.deepEqual(gpuLayerLadder(7, 41), [7, 10, 13]);
  assert.deepEqual(gpuLayerLadder(40, 41), [40, 41], 'capped at the layer count, no repeats');
  const base = ollamaFetch();
  const sent = [];
  const oom = () => new Response('{"error":"llama-server reported out-of-memory during startup: CUDA error\nCUDA error: out of memory"}', { status: 500 });
  const f = async (url, init) => {
    if (url.endsWith('/api/show')) { const m = JSON.parse(init.body).model; return json({ capabilities: CAPS[m] || [], model_info: { 'qwen35moe.block_count': 41 } }); }
    if (url.endsWith('/api/ps')) return json({ models: [] });
    if (!url.endsWith('/api/chat')) return base(url, init);
    const n = JSON.parse(init.body).options.num_gpu; sent.push(n);
    if (n == null || n < 9) return oom();                       // Ollama's own split and the low estimate both die
    return stream(['{"message":{"content":"ok"}}\n{"done":true,"eval_count":40,"eval_duration":1000000000}\n']);
  };
  forgetServers();
  const store = new Map(); const st = { getItem: (k) => store.get(k) ?? null, setItem: (k, v) => store.set(k, v) };
  const perf = { models: {}, vramGB: 5.5 };                     // learned from the 9B: an under-estimate
  const cfg = { enabled: true, servers: [DEFAULT_SERVERS[0]] };
  const r = await localComplete('code', { model: 'qwen3.6-coder:16k', messages: [{ role: 'user', content: 'x' }], fetch: f, storage: st, config: cfg, perf });
  assert.equal(r && r.text, 'ok');
  assert.deepEqual(sent, [undefined, 7, 10]);
  assert.equal(r.engine.gpuLayers, 10);
});
