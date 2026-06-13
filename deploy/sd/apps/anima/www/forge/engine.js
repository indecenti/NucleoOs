// engine.js — the LLM engine boundary. Every M4 module takes `engine` by INJECTION so the whole
// agent loop runs deterministically in CI behind a MockEngine (no GPU). The real engine wraps
// WebLLM; the mock replays a scripted queue. A hard gate must assert that only a MockEngine ever
// appears in an automated test (assertMock). Pure & DOM-free → host-testable.
//
//   interface Engine { chat(messages, { schema?, temperature?, seed?, phase? }) -> Promise<{text, usage}> }

export class MockEngine {
  // script: array of (string | {text, usage}) replayed in order, OR a function (messages,opts,i)→same.
  constructor(script = []) {
    this.script = typeof script === 'function' ? script : (Array.isArray(script) ? script.slice() : [script]);
    this.calls = [];
    this.i = 0;
  }
  async chat(messages, opts = {}) {
    this.calls.push({ messages, opts });
    let r;
    if (typeof this.script === 'function') r = this.script(messages, opts, this.i);
    else r = this.i < this.script.length ? this.script[this.i] : '';
    this.i++;
    const text = typeof r === 'string' ? r : ((r && r.text) || '');
    const usage = (r && typeof r === 'object' && r.usage) || { tokens: text.length };
    return { text, usage };
  }
  get isMock() { return true; }
}

// Guard: a hard automated gate must never run a real model. Call this at the top of loop gates.
export function assertMock(engine) {
  if (!engine || engine.isMock !== true) throw new Error('hard gate requires a MockEngine (no real model in CI)');
}
