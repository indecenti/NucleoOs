// stream.js — browser-direct cloud chat with REAL streaming for the ANIMA app.
//
// Before this, the app waited for the whole completion and then faked a typewriter over it (a 3 KB
// answer added ~20 s AFTER it had already arrived). Here the provider streams tokens over SSE and the
// caller renders them as they land (index.html batches the repaint per animation frame).
//
//   Anthropic   POST /v1/messages          {stream:true}  -> event: content_block_delta {delta:{type:'text_delta',text}}
//   OpenAI-compatible (Groq, OpenAI, xAI)  {stream:true}  -> data: {choices:[{delta:{content}}]} ... data: [DONE]
//
// A provider relayed through the device (proxy, e.g. Gemini via /api/llm) is NOT streamed: the relay
// holds a TLS session on a no-PSRAM chip, so we keep that call short and single-shot.
//
// Every call takes an AbortSignal (Stop/Esc, step timeout) and an idle timeout: a stream that goes
// silent for idleMs is aborted instead of hanging the turn. DOM-free; importable in Node for tests.

// Incremental SSE line splitter: feed() it decoded text, it calls onData(payload) for every `data:` line.
// Comments (':'), `event:`/`id:`/`retry:` fields and blank separators are ignored — both providers put
// everything we need in the JSON payload itself (Anthropic repeats the event name in payload.type).
export function sseParser(onData) {
  let buf = '';
  const line = (l) => {
    if (!l || l[0] === ':') return;
    if (l.startsWith('data:')) onData(l.slice(5).replace(/^ /, ''));
  };
  return {
    feed(chunk) {
      buf += chunk;
      let i;
      while ((i = buf.search(/\r\n|\n|\r/)) >= 0) {
        const l = buf.slice(0, i);
        buf = buf.slice(i + (buf[i] === '\r' && buf[i + 1] === '\n' ? 2 : 1));
        line(l);
      }
    },
    end() { if (buf) { line(buf); buf = ''; } },
  };
}

// Pull the text delta (or a terminal/error signal) out of one SSE payload.
//   returns { text } | { done:true } | { error } | null (nothing for us in this event)
export function deltaOf(provider, payload) {
  if (payload === '[DONE]') return { done: true };
  let j; try { j = JSON.parse(payload); } catch { return null; }
  if (!j || typeof j !== 'object') return null;
  if (provider === 'anthropic') {
    if (j.type === 'content_block_delta' && j.delta && j.delta.type === 'text_delta') return { text: j.delta.text || '' };
    if (j.type === 'message_stop') return { done: true };
    if (j.type === 'error') return { error: (j.error && j.error.message) || 'stream error' };
    return null;
  }
  if (j.error) return { error: j.error.message || String(j.error) };
  const c = j.choices && j.choices[0];
  if (!c) return null;
  const t = c.delta && typeof c.delta.content === 'string' ? c.delta.content : '';
  return t ? { text: t } : null;
}

const errorOf = async (resp) => {
  const j = await resp.json().catch(() => null);
  return new Error((j && j.error && (j.error.message || (typeof j.error === 'string' ? j.error : ''))) || ('HTTP ' + resp.status));
};

// Read an SSE body to the end, calling onDelta(text) per token batch. Returns the full text.
async function readStream(resp, provider, { onDelta, signal, idleMs }) {
  const reader = resp.body.getReader();
  const dec = new TextDecoder();
  let full = '', done = false, failure = null, idle = 0;
  const kill = (why) => { try { reader.cancel(why); } catch {} };
  const arm = () => { if (!idleMs) return; clearTimeout(idle); idle = setTimeout(() => { failure = Object.assign(new Error('stream stalled'), { name: 'TimeoutError' }); kill(failure); }, idleMs); };
  const onAbort = () => { failure = (signal && signal.reason) || Object.assign(new Error('Stopped'), { name: 'AbortError' }); kill(failure); };
  if (signal) { if (signal.aborted) onAbort(); else signal.addEventListener('abort', onAbort, { once: true }); }
  const p = sseParser((payload) => {
    const d = deltaOf(provider, payload);
    if (!d) return;
    if (d.error) { failure = new Error(d.error); kill(failure); return; }
    if (d.done) { done = true; return; }
    if (d.text) { full += d.text; try { onDelta && onDelta(d.text); } catch {} }
  });
  try {
    arm();
    while (!done && !failure) {
      let r;
      try { r = await reader.read(); } catch (e) { if (!failure) failure = e; break; }
      if (r.done) break;
      arm();
      p.feed(dec.decode(r.value, { stream: true }));
    }
    p.feed(dec.decode()); p.end();
  } finally {
    clearTimeout(idle);
    if (signal) signal.removeEventListener('abort', onAbort);
    if (done) kill('done');
  }
  if (failure) { failure.partial = full; throw failure; }
  return full;
}

// One chat completion. cfg: {provider, base, model, key, version}; req: {system, messages, maxTokens,
// temperature}; opts: {signal, onDelta, idleMs, proxyUrl(url)->url|null}. With onDelta and a direct
// (non-proxy) provider the call streams; otherwise it is a single JSON round-trip. Returns the text.
// On a mid-stream failure the thrown error carries .partial (what already arrived).
export async function chatComplete(cfg, req, opts = {}) {
  const { signal, onDelta, idleMs = 30000 } = opts;
  const msgs = Array.isArray(req.messages) ? req.messages : [{ role: 'user', content: String(req.user || '') }];
  const maxTokens = req.maxTokens || 1024;
  const temperature = typeof req.temperature === 'number' ? req.temperature : 0.4;
  const proxied = typeof opts.proxyUrl === 'function' ? opts.proxyUrl : null;

  if (cfg.provider === 'anthropic') {
    const stream = !!onDelta;
    const resp = await fetch((cfg.base || 'https://api.anthropic.com').replace(/\/+$/, '') + '/v1/messages', {
      method: 'POST', signal,
      headers: { 'content-type': 'application/json', 'x-api-key': cfg.key, 'anthropic-version': cfg.version || '2023-06-01',
                 'anthropic-dangerous-direct-browser-access': 'true' },
      body: JSON.stringify({ model: cfg.model || 'claude-sonnet-4-6', max_tokens: maxTokens,
                             ...(req.system ? { system: req.system } : {}), messages: msgs, ...(stream ? { stream: true } : {}) }),
    });
    if (!resp.ok) throw await errorOf(resp);
    if (stream && resp.body && /event-stream/i.test(resp.headers.get('content-type') || '')) return readStream(resp, 'anthropic', { onDelta, signal, idleMs });
    const j = await resp.json().catch(() => null);
    if (!j || j.type === 'error') throw new Error((j && j.error && j.error.message) || 'bad response');
    const txt = Array.isArray(j.content) ? j.content.filter((b) => b && b.type === 'text').map((b) => b.text).join('') : '';
    if (onDelta && txt) { try { onDelta(txt); } catch {} }
    return txt;
  }

  // OpenAI-compatible. A proxied provider (the device relays it) is never streamed.
  const direct = (cfg.base || 'https://api.groq.com/openai/v1').replace(/\/+$/, '') + '/chat/completions';
  const viaProxy = proxied ? proxied(direct) : null;
  const stream = !!onDelta && !viaProxy;
  const resp = await fetch(viaProxy || direct, {
    method: 'POST', signal,
    headers: { 'content-type': 'application/json', 'authorization': 'Bearer ' + cfg.key },
    body: JSON.stringify({ model: cfg.model || 'llama-3.1-8b-instant', max_tokens: maxTokens, temperature,
                           messages: [...(req.system ? [{ role: 'system', content: req.system }] : []), ...msgs],
                           ...(stream ? { stream: true } : {}) }),
  });
  if (!resp.ok) throw await errorOf(resp);
  if (stream && resp.body && /event-stream/i.test(resp.headers.get('content-type') || '')) return readStream(resp, 'openai', { onDelta, signal, idleMs });
  const j = await resp.json().catch(() => null);
  if (!j || j.error) throw new Error((j && j.error && (j.error.message || j.error)) || 'bad response');
  const txt = (j.choices && j.choices[0] && j.choices[0].message && j.choices[0].message.content) || '';
  if (onDelta && txt) { try { onDelta(txt); } catch {} }
  return txt;
}
