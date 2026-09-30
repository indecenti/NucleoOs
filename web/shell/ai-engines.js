// ai-engines.js — LOCAL AI servers on the user's own PC (Ollama, LM Studio, llama.cpp, Jan, LocalAI,
// NucleoMind, any OpenAI-compatible endpoint): detect them, read what each installed model can really do,
// and talk to them — straight from the browser. The Cardputer is never involved: not a single request of
// this module goes to the device (its /api/llm proxy can't reach the PC's localhost anyway, and routing
// tokens through a no-PSRAM ESP32 would be absurd).
//
// Browser realities this handles:
//   • CORS: Ollama only admits localhost-ish origins by default; the OS served from http://<device-ip> is
//     refused (403, no CORS headers → an opaque TypeError). We tell "server down" from "server up but this
//     origin not allowed" with a no-cors probe, so the UI can show the exact OLLAMA_ORIGINS fix.
//   • Chrome Local Network Access: private-IP → loopback is not gated today; `targetAddressSpace:'loopback'`
//     is passed anyway (ignored by browsers that don't know it) so a future gate prompts instead of failing.
//
// Pure / I/O-injected (fetch, AbortSignal) → host-tested in tools/shell-ai-engines.test.mjs.

export const DEFAULT_SERVERS = [
  { id: 'ollama', kind: 'ollama', name: 'Ollama', base: 'http://localhost:11434' },
  { id: 'lmstudio', kind: 'openai', name: 'LM Studio', base: 'http://localhost:1234/v1' },
  { id: 'llamacpp', kind: 'openai', name: 'llama.cpp', base: 'http://localhost:8080/v1' },
  { id: 'jan', kind: 'openai', name: 'Jan', base: 'http://localhost:1337/v1' },
];

export const isLoopback = (url) => { try { const h = new URL(url).hostname; return h === 'localhost' || h === '127.0.0.1' || h === '[::1]' || h.endsWith('.localhost'); } catch { return false; } };
function withTimeout(ms, outer) {
  const ac = new AbortController();
  const t = setTimeout(() => ac.abort(new Error('timeout')), ms);
  if (outer) { if (outer.aborted) ac.abort(outer.reason); else outer.addEventListener('abort', () => ac.abort(outer.reason), { once: true }); }
  return { signal: ac.signal, done: () => clearTimeout(t) };
}
function reqInit(url, init = {}) {
  const o = { ...init };
  if (isLoopback(url)) o.targetAddressSpace = 'loopback';
  return o;
}

// ── probing ──────────────────────────────────────────────────────────────────────────────────────
// → { ...server, status: 'ok'|'cors'|'down'|'error', models: [ {id, sizeGB, params, family, caps:{…}} ], error? }
export async function probeServer(server, { fetch: f = globalThis.fetch, timeoutMs = 1500, details = true } = {}) {
  const out = { ...server, status: 'down', models: [] };
  const listUrl = server.kind === 'ollama' ? server.base.replace(/\/+$/, '') + '/api/tags' : server.base.replace(/\/+$/, '') + '/models';
  const tm = withTimeout(timeoutMs);
  let r;
  try { r = await f(listUrl, reqInit(listUrl, { signal: tm.signal, headers: server.key ? { authorization: 'Bearer ' + server.key } : undefined })); }
  catch (e) {
    tm.done();
    // Up but refusing this origin (CORS) resolves an opaque no-cors request; a dead server rejects it too.
    const t2 = withTimeout(timeoutMs);
    try { await f(listUrl, reqInit(listUrl, { mode: 'no-cors', signal: t2.signal })); out.status = 'cors'; }
    catch { out.status = 'down'; }
    finally { t2.done(); }
    out.error = String(e && e.message || e);
    return out;
  }
  tm.done();
  if (r.status === 403) { out.status = 'cors'; return out; }           // Ollama's answer to a disallowed Origin
  if (r.status === 401) { out.status = 'error'; out.error = 'unauthorized'; return out; }
  if (!r.ok) { out.status = 'error'; out.error = 'HTTP ' + r.status; return out; }
  let j = null; try { j = await r.json(); } catch { out.status = 'error'; out.error = 'bad json'; return out; }
  out.status = 'ok';
  if (server.kind === 'ollama') {
    out.models = (j.models || []).map((m) => ({
      id: m.name || m.model, sizeGB: m.size ? Math.round(m.size / 1e8) / 10 : null,
      params: (m.details && m.details.parameter_size) || '', family: (m.details && m.details.family) || '',
      quant: (m.details && m.details.quantization_level) || '', caps: null,
    }));
    if (details) await Promise.all(out.models.slice(0, 16).map(async (m) => { m.caps = await ollamaCaps(server.base, m.id, { fetch: f }); }));
  } else {
    out.models = (j.data || j.models || []).map((m) => ({ id: m.id || m.name, sizeGB: null, params: '', family: '', caps: null }));
  }
  return out;
}

// Ollama /api/show → the model's declared capabilities (completion, tools, vision, thinking, embedding, audio).
export async function ollamaCaps(base, model, { fetch: f = globalThis.fetch, timeoutMs = 2500 } = {}) {
  const url = base.replace(/\/+$/, '') + '/api/show';
  const tm = withTimeout(timeoutMs);
  try {
    const r = await f(url, reqInit(url, { method: 'POST', body: JSON.stringify({ model }), headers: { 'content-type': 'application/json' }, signal: tm.signal }));
    if (!r.ok) return null;
    const j = await r.json();
    const c = new Set(j.capabilities || []);
    let ctx = 0;
    for (const [k, v] of Object.entries(j.model_info || {})) if (/context_length$/.test(k)) ctx = v;
    return { chat: c.has('completion'), tools: c.has('tools'), vision: c.has('vision'), thinking: c.has('thinking'), embedding: c.has('embedding'), audio: c.has('audio'), ctx };
  } catch { return null; } finally { tm.done(); }
}

export async function detectServers({ servers = DEFAULT_SERVERS, fetch: f = globalThis.fetch, timeoutMs = 1500 } = {}) {
  return Promise.all(servers.map((s) => probeServer(s, { fetch: f, timeoutMs })));
}
// Cached for chat turns: probing four localhost ports on EVERY question would add up to a second of
// latency when no server runs. The result is re-used for ttl — only for the SAME server list and fetch
// (a config change re-probes) — and a failed chat drops it at once (see localComplete).
// "Nothing runs here" is kept longer (downTtlMs): on Windows a refused localhost port can take ~1 s to
// fail, and most machines have no local server at all. Settings ▸ Detect passes fresh:true.
let _live = null, _liveAt = 0, _liveKey = '', _liveFetch = null;
export async function liveServers({ config = loadLocalConfig(), ttlMs = 60000, downTtlMs = 300000, fresh = false, fetch: f = globalThis.fetch } = {}) {
  const servers = config.servers.filter((s) => s.enabled !== false);
  const key = JSON.stringify(servers.map((s) => [s.id, s.kind, s.base]));
  const ttl = _live && _live.some((s) => s.status === 'ok') ? ttlMs : Math.max(ttlMs, downTtlMs);
  if (!fresh && _live && _liveKey === key && _liveFetch === f && Date.now() - _liveAt < ttl) return _live;
  const live = await detectServers({ servers, fetch: f, timeoutMs: 1200 });
  _live = live; _liveAt = Date.now(); _liveKey = key; _liveFetch = f;
  return live;
}
export function forgetServers() { _live = null; _liveAt = 0; _liveKey = ''; _liveFetch = null; }

// ── choosing a model for a task (from what the user actually installed) ─────────────────────────────
const paramsB = (m) => { const x = /([\d.]+)\s*B/i.exec(m.params || '') || /(\d+(?:\.\d+)?)b\b/i.exec(m.id || ''); return x ? parseFloat(x[1]) : 0; };
const isCoder = (m) => /coder|code/i.test(m.id);
const isEmbed = (m) => (m.caps && m.caps.embedding && !m.caps.chat) || /embed|bge|minilm|e5-/i.test(m.id);
// Before anything was measured, assume the common consumer GPU: 8 GB, ~1 GB of it held by the desktop. A
// model FILE of that size already spills once loaded (gemma4:12b: 7.56 GB file, 8.9 GB loaded with its
// projector and an 8k KV cache), so the prior compares the file size to it with no slack.
export const DEFAULT_VRAM_GB = 7;
// task: 'chat' | 'agent' | 'code' | 'vision' | 'embed'. Prefers capable models of a size that stays
// responsive on a consumer GPU (≤ ~14B, unless it is a sparse MoE "a3b"), bigger within that range.
// opts.perf / opts.base: what was MEASURED on this computer (see notePerf) beats every size estimate.
export function pickModel(models, task = 'chat', { perf = null, base = '' } = {}) {
  const list = (models || []).filter((m) => task === 'embed' ? isEmbed(m) : !isEmbed(m));
  if (!list.length) return null;
  const need = (m) => task === 'agent' ? (!m.caps || m.caps.tools) : task === 'vision' ? !!(m.caps && m.caps.vision) : true;
  const moe = (m) => /a\d+b|moe/i.test(m.id + ' ' + m.family);
  const vram = (perf && perf.vramGB) || DEFAULT_VRAM_GB;
  const score = (m) => {
    const p = paramsB(m);
    let s = 0;
    if (!need(m)) return null;
    if (task === 'code') s += isCoder(m) ? 50 : 0; else s -= isCoder(m) ? 15 : 0;
    if (m.caps && m.caps.tools) s += 5;
    const fits = p <= 14 || moe(m);
    s += fits ? Math.min(p, 14) * 2 : 2;
    // What must sit in (V)RAM is the FILE, not the active parameters: a 22 GB MoE "a3b" still loads 22 GB.
    // Past ~10 GB it rarely fits a consumer GPU and spills to CPU (slow) or fails — keep it as a fallback.
    if (m.sizeGB && m.sizeGB > 10) s -= 30 + m.sizeGB;
    const seen = perf && perf.models && perf.models[base + '|' + m.id];
    if (seen && seen.tps) {
      // Measured here. Measured 2026-09-30 on an 8 GB laptop RTX 5070: gemma4:12b (8.9 GB) ran a third on
      // the CPU at 2.1 tok/s — a minute per answer — while qwen3.5:9b gave 20.5 tok/s. Speed is the truth.
      if (seen.tps < 6) s -= 40; else if (seen.tps >= 15) s += 4;
    } else if (m.sizeGB && m.sizeGB > vram) {
      s -= 15 + 2 * (m.sizeGB - vram);             // not measured yet, and larger than the GPU looks: likely to spill
    }
    return s;
  };
  const ranked = list.map((m) => ({ m, s: score(m) })).filter((x) => x.s !== null).sort((a, b) => b.s - a.s);
  return ranked.length ? ranked[0].m.id : null;
}
// Every suitable model for a task, best first — the fallback ladder when one does not fit in memory.
export function rankModels(models, task = 'chat', opts = {}) {
  const out = [];
  let rest = [...(models || [])];
  for (;;) { const id = pickModel(rest, task, opts); if (!id) break; out.push(id); rest = rest.filter((m) => m.id !== id); }
  return out;
}

// ── learning what really runs well HERE ───────────────────────────────────────────────────────────────
// After a chat, Ollama's /api/ps says how much of the model sits in VRAM (size_vram / size) and the chat
// itself gave tokens/s. Kept per browser (localhost = this computer), small, and never sent anywhere.
const PERF = 'ai.local.perf';
export function loadPerf(storage = globalThis.localStorage) {
  try { const j = JSON.parse(storage.getItem(PERF) || 'null'); if (j && j.models) return j; } catch {}
  return { models: {} };
}
export function savePerf(perf, storage = globalThis.localStorage) { try { storage.setItem(PERF, JSON.stringify(perf)); } catch {} }
// obs: { tps, sizeGB?, vramGB? } → the updated record. The VRAM estimate follows the latest evidence: a model
// that fit entirely raises it to at least its size; one that spilled pins it to what the GPU actually held.
export function notePerf(perf, base, model, { tps, sizeGB, vramGB } = {}) {
  const p = perf && perf.models ? perf : { models: {} };
  const key = base + '|' + model;
  const prev = p.models[key] || {};
  const rec = { ...prev, at: Date.now() };
  if (tps > 0) rec.tps = prev.tps ? Math.round((prev.tps * 0.4 + tps * 0.6) * 10) / 10 : tps;   // smooth one noisy turn
  if (sizeGB && vramGB != null) {
    rec.fit = Math.round(Math.min(1, vramGB / sizeGB) * 100) / 100;
    // fit entirely → the GPU holds AT LEAST this much (never lowers the estimate); spilled → it holds what it held.
    p.vramGB = rec.fit >= 0.98 ? Math.max(p.vramGB || DEFAULT_VRAM_GB, Math.round(sizeGB * 10) / 10) : Math.round(vramGB * 10) / 10;
  }
  p.models[key] = rec;
  const keys = Object.keys(p.models);                                    // stay tiny: the 24 most recent
  if (keys.length > 24) for (const k of keys.sort((a, b) => p.models[a].at - p.models[b].at).slice(0, keys.length - 24)) delete p.models[k];
  return p;
}
// Ollama /api/ps → [{ id, sizeGB, vramGB }] for the models loaded right now.
export async function ollamaLoaded(base, { fetch: f = globalThis.fetch, timeoutMs = 1500 } = {}) {
  const url = base.replace(/\/+$/, '') + '/api/ps';
  const tm = withTimeout(timeoutMs);
  try {
    const r = await f(url, reqInit(url, { signal: tm.signal }));
    if (!r.ok) return [];
    const j = await r.json();
    return (j.models || []).map((m) => ({ id: m.name || m.model, sizeGB: m.size / 1e9, vramGB: (m.size_vram || 0) / 1e9 }));
  } catch { return []; } finally { tm.done(); }
}
export const isOutOfMemory = (e) => /out of memory|\bOOM\b|CUDA error|cudaMalloc|insufficient memory|requires more system memory/i.test(String(e && e.message || e));

// ── persisted choice (per browser: "localhost" means THIS computer) ────────────────────────────────────
const LS = 'ai.local.engines';
export function loadLocalConfig(storage = globalThis.localStorage) {
  try { const j = JSON.parse(storage.getItem(LS) || 'null'); if (j && Array.isArray(j.servers)) return j; } catch {}
  return { enabled: true, servers: DEFAULT_SERVERS.map((s) => ({ ...s, enabled: true })), models: {} };
}
export function saveLocalConfig(cfg, storage = globalThis.localStorage) { try { storage.setItem(LS, JSON.stringify(cfg)); } catch {} }

// ── chat ─────────────────────────────────────────────────────────────────────────────────────────────
// Ollama native /api/chat (NDJSON stream). think:false keeps thinking models quick and clean for chat.
// → { text, toolCalls: [{name, arguments}], usage: { promptTokens, outputTokens, tokPerSec } }
// num_ctx defaults to 8k: Ollama otherwise sizes the context from VRAM (up to 256k for some models), which
// on an 8 GB laptop GPU plus a vision projector ran out of CUDA memory before the first token. 8k covers
// an assistant turn with tools comfortably; callers can raise it.
export async function ollamaChat({ base, model, messages, tools, format, think = false, signal, onDelta, keepAlive = '10m', options, fetch: f = globalThis.fetch }) {
  const url = base.replace(/\/+$/, '') + '/api/chat';
  const body = { model, messages, stream: true, think, keep_alive: keepAlive, options: { num_ctx: 8192, ...(options || {}) } };
  if (tools && tools.length) body.tools = tools;
  if (format) body.format = format;
  const r = await f(url, reqInit(url, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(body), signal }));
  if (!r.ok) { const e = new Error('Ollama HTTP ' + r.status + ' ' + (await r.text().catch(() => '')).slice(0, 200)); e.status = r.status; throw e; }
  let text = '', toolCalls = [], usage = {};
  await readLines(r, (line) => {
    let j; try { j = JSON.parse(line); } catch { return; }
    if (j.error) throw new Error('Ollama: ' + j.error);
    const m = j.message || {};
    if (m.content) { text += m.content; if (onDelta) try { onDelta(m.content); } catch {} }
    if (Array.isArray(m.tool_calls)) for (const tc of m.tool_calls) toolCalls.push({ name: tc.function && tc.function.name, arguments: (tc.function && tc.function.arguments) || {} });
    if (j.done) usage = { promptTokens: j.prompt_eval_count || 0, outputTokens: j.eval_count || 0, tokPerSec: j.eval_duration ? Math.round((j.eval_count || 0) / (j.eval_duration / 1e9) * 10) / 10 : 0 };
  });
  return { text: stripThink(text), toolCalls, usage };
}

// OpenAI-compatible /chat/completions (SSE stream) — LM Studio, llama.cpp, Jan, LocalAI, NucleoMind.
export async function openaiChat({ base, model, key, messages, tools, temperature, maxTokens, signal, onDelta, fetch: f = globalThis.fetch }) {
  const url = base.replace(/\/+$/, '') + '/chat/completions';
  const body = { model, messages: toOpenAIMessages(messages), stream: true };
  if (temperature != null) body.temperature = temperature;
  if (maxTokens) body.max_tokens = maxTokens;
  if (tools && tools.length) body.tools = tools;
  const headers = { 'content-type': 'application/json' };
  if (key) headers.authorization = 'Bearer ' + key;
  const r = await f(url, reqInit(url, { method: 'POST', headers, body: JSON.stringify(body), signal }));
  if (!r.ok) { const e = new Error('HTTP ' + r.status + ' ' + (await r.text().catch(() => '')).slice(0, 200)); e.status = r.status; throw e; }
  let text = ''; const calls = new Map();
  await readLines(r, (line) => {
    if (!line.startsWith('data:')) return;
    const data = line.slice(5).trim();
    if (!data || data === '[DONE]') return;
    let j; try { j = JSON.parse(data); } catch { return; }
    const d = j.choices && j.choices[0] && j.choices[0].delta || {};
    if (d.content) { text += d.content; if (onDelta) try { onDelta(d.content); } catch {} }
    for (const tc of d.tool_calls || []) {
      const c = calls.get(tc.index) || { name: '', args: '' };
      if (tc.function && tc.function.name) c.name = tc.function.name;
      if (tc.function && tc.function.arguments) c.args += tc.function.arguments;
      calls.set(tc.index, c);
    }
  });
  const toolCalls = [...calls.values()].map((c) => { let a = {}; try { a = JSON.parse(c.args || '{}'); } catch {} return { name: c.name, arguments: a }; });
  return { text: stripThink(text), toolCalls, usage: {} };
}

// The agent loop keeps tool calls in Ollama's native shape (object arguments). OpenAI-compatible servers want
// the arguments as a JSON STRING and every tool result threaded by tool_call_id.
export function toOpenAIMessages(messages) {
  return (messages || []).map((m) => {
    if (m.role === 'assistant' && Array.isArray(m.tool_calls)) {
      return { role: 'assistant', content: m.content || '', tool_calls: m.tool_calls.map((c, i) => ({ id: c.id || 'call_' + i, type: 'function',
        function: { name: c.function && c.function.name, arguments: typeof (c.function && c.function.arguments) === 'string' ? c.function.arguments : JSON.stringify((c.function && c.function.arguments) || {}) } })) };
    }
    if (m.role === 'tool') return { role: 'tool', tool_call_id: m.tool_call_id || '', content: String(m.content || '') };
    return m;
  });
}

// Stream a Response body line by line (works for NDJSON and SSE; tolerant of chunk boundaries).
export async function readLines(resp, onLine) {
  if (!resp.body || !resp.body.getReader) { for (const l of String(await resp.text()).split('\n')) if (l.trim()) onLine(l.trim()); return; }
  const rd = resp.body.getReader(), dec = new TextDecoder();
  let buf = '';
  for (;;) {
    const { value, done } = await rd.read();
    if (done) break;
    buf += dec.decode(value, { stream: true });
    let i;
    while ((i = buf.indexOf('\n')) >= 0) { const l = buf.slice(0, i).trim(); buf = buf.slice(i + 1); if (l) onLine(l); }
  }
  if (buf.trim()) onLine(buf.trim());
}
const stripThink = (s) => String(s || '').replace(/<think>[\s\S]*?<\/think>\s*/g, '').trim();

// One call for "use whatever local engine is set up for this task" → { text, toolCalls, usage, engine }
// or null when no local server is enabled + reachable (caller falls back to cloud / WebGPU / device).
// loopbackOnly: Private mode — only a server on THIS computer counts (a LAN box is still "the network").
// perf: what was measured on this computer (loadPerf); a fresh measurement is recorded after each Ollama
// answer (storage: where it is kept; null = don't persist, e.g. tests that pass their own perf).
// model: pin one model (an agent keeps the model it started a task with — no switch mid-task); numCtx: the
// context window to ask Ollama for (8k default; the agent asks for more — see AGENT_CTX).
export async function localComplete(task, { messages, tools, temperature, maxTokens, loopbackOnly = false, signal, onDelta, model: pinned, numCtx, config = loadLocalConfig(), servers, perf, storage = globalThis.localStorage, fetch: f = globalThis.fetch } = {}) {
  if (!config.enabled) return null;
  if (!perf) perf = storage ? loadPerf(storage) : { models: {} };
  const live = servers || await liveServers({ config, fetch: f });
  for (const s of live) {
    if (s.status !== 'ok' || !s.models.length) continue;
    if (loopbackOnly && !isLoopback(s.base)) continue;
    const chosen = config.models && config.models[s.id] && config.models[s.id][task];
    // The user's pick first, then the rest of the ladder: a model that does not fit in memory right now
    // (another app holds the GPU, the context is too big) falls through to the next lighter one.
    const ladder = pinned ? [pinned] : [...new Set([chosen, ...rankModels(s.models, task, { perf, base: s.base })].filter(Boolean))];
    const skipped = [];
    for (const model of ladder) {
      try {
        const options = {};
        if (temperature != null) options.temperature = temperature;
        if (maxTokens) options.num_predict = maxTokens;
        if (numCtx) options.num_ctx = numCtx;
        const res = s.kind === 'ollama'
          ? await ollamaChat({ base: s.base, model, messages, tools, options, signal, onDelta, fetch: f })
          : await openaiChat({ base: s.base, model, key: s.key, messages, tools, temperature, maxTokens, signal, onDelta, fetch: f });
        if (s.kind === 'ollama' && res.usage && res.usage.outputTokens >= 16) learnPerf(perf, s.base, model, res.usage.tokPerSec, { storage, fetch: f });
        return { ...res, engine: { server: s.name, kind: s.kind, model, base: s.base, ...(skipped.length ? { skipped } : {}) } };
      } catch (e) {
        if (signal && signal.aborted) throw e;
        if (!isOutOfMemory(e)) { forgetServers(); throw e; }   // server gone / model removed: re-probe next turn
        skipped.push(model);
      }
    }
  }
  return null;
}
// Fire-and-forget: read where the model sits (VRAM vs CPU) and remember how fast it answered. Never blocks
// or fails the turn; a server without /api/ps just records the speed.
async function learnPerf(perf, base, model, tps, { storage, fetch: f }) {
  try {
    const m = (await ollamaLoaded(base, { fetch: f })).find((x) => x.id === model);
    notePerf(perf, base, model, { tps, sizeGB: m && m.sizeGB, vramGB: m ? m.vramGB : undefined });
    if (storage) savePerf(perf, storage);
  } catch {}
}
