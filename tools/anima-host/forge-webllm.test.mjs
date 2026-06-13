// Gate: ANIMA Forge — the real WebLLM engine adapter (M4 inference), pure parts host-tested with NO
// GPU and NO '@mlc-ai/web-llm' import. probeWebGPU degrades cleanly under Node; chooseModels is
// VRAM-gated; the orchestrator request carries the GBNF grammar; makeWebLLMEngine runs entirely on
// an injected fake createEngine and is NOT a mock (isMock===false). A real WebLLM run is GPU-only.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  probeWebGPU, chooseModels, orchestratorMessages, coderMessages, makeWebLLMEngine,
  CODER_MODEL, ORCH_DEFAULT, ORCH_SMALL,
} from '../../apps/anima/www/forge/webllm-engine.js';
import { toGBNF } from '../../apps/anima/www/forge/grammar.js';

test('probeWebGPU degrades cleanly with no navigator/gpu (Node)', async () => {
  assert.deepEqual((await probeWebGPU(null)).supported, false);
  assert.equal((await probeWebGPU({}).then((r) => r.supported)), false);   // navigator without .gpu
  const r = await probeWebGPU(undefined);                                   // default → no global navigator in Node
  assert.equal(r.supported, false);
});

test('probeWebGPU reports supported + vramMB from a fake adapter', async () => {
  const fakeNav = {
    gpu: {
      async requestAdapter() {
        return { limits: { maxBufferSize: 2 * 1024 * 1024 * 1024 } };   // 2 GiB proxy
      },
    },
  };
  const r = await probeWebGPU(fakeNav);
  assert.equal(r.supported, true);
  assert.equal(r.vramMB, 2048);
});

test('probeWebGPU: supported but no usable vram when adapter is null', async () => {
  const fakeNav = { gpu: { async requestAdapter() { return null; } } };
  const r = await probeWebGPU(fakeNav);
  assert.equal(r.supported, false);
  assert.equal(r.reason, 'no-adapter');
});

test('chooseModels: single-model (orchestrator null) on a ~2GB cap', () => {
  const c = chooseModels({ webgpu: true, vramMB: 2000 });
  assert.equal(c.plan.mode, 'single-model');
  assert.equal(c.orchestrator, null);
  assert.equal(c.coder.id, CODER_MODEL);
});

test('chooseModels: two-model on ~8GB → small orchestrator + resident coder', () => {
  const c = chooseModels({ webgpu: true, vramMB: 8000 });
  assert.equal(c.plan.mode, 'two-model');
  assert.equal(c.orchestrator.id, ORCH_SMALL);
  assert.equal(c.coder.id, CODER_MODEL);
});

test('chooseModels: paged plan (~4GB) keeps the default-safe orchestrator', () => {
  const c = chooseModels({ webgpu: true, vramMB: 4000 });
  assert.equal(c.plan.mode, 'two-model-paged');
  assert.equal(c.orchestrator.id, ORCH_DEFAULT);
});

test('chooseModels: no webgpu → none plan, orchestrator null', () => {
  const c = chooseModels({ webgpu: false });
  assert.equal(c.plan.mode, 'none');
  assert.equal(c.orchestrator, null);
});

test('orchestratorMessages embeds the GBNF grammar string and constrains decoding', () => {
  const g = toGBNF();
  const { messages, options } = orchestratorMessages('leggi note.txt e riassumi', g);
  assert.equal(messages[0].role, 'system');
  assert.match(messages[0].content, /JSON array of actions/);
  assert.equal(messages[1].content, 'leggi note.txt e riassumi');
  assert.equal(options.grammar, g);
  assert.equal(options.response_format.grammar, g);
  assert.equal(options.response_format.type, 'grammar');
  assert.match(options.grammar, /^root ::=/m);          // it really is the GBNF
  assert.equal(options.temperature, 0);
});

test('orchestratorMessages defaults to the live grammar when none is passed', () => {
  const { options } = orchestratorMessages('do something');
  assert.equal(options.grammar, toGBNF());
});

test('coderMessages asks for ONE fenced js block at temp 0.2', () => {
  const { messages, options } = coderMessages('a debounce function');
  assert.match(messages[0].content, /EXACTLY ONE fenced code block/);
  assert.equal(messages[1].content, 'a debounce function');
  assert.equal(options.temperature, 0.2);
  assert.equal(options.phase, 'code');
});

test('makeWebLLMEngine: injected fake createEngine, scripted chat(), isMock===false', async () => {
  const calls = [];
  let created = 0;
  const fakeCreateEngine = async (modelId, opts) => {
    created++;
    return {
      chat: {
        completions: {
          async create(req) {
            calls.push(req);
            return {
              choices: [{ message: { content: '```js\nexport const x = 1;\n```' } }],
              usage: { total_tokens: 7 },
            };
          },
        },
      },
      async unload() { this.unloaded = true; },
    };
  };

  const engine = makeWebLLMEngine({ createEngine: fakeCreateEngine, modelId: CODER_MODEL });
  assert.equal(engine.isMock, false);

  const out = await engine.chat([{ role: 'user', content: 'hi' }], { temperature: 0.2 });
  assert.match(out.text, /export const x = 1/);
  assert.equal(out.usage.total_tokens, 7);
  assert.equal(created, 1);                       // lazily created once
  await engine.chat([{ role: 'user', content: 'again' }]);
  assert.equal(created, 1);                       // reused, not re-created
  assert.equal(calls[0].temperature, 0.2);
});

test('makeWebLLMEngine: opts.grammar/schema flows into response_format', async () => {
  let seen = null;
  const fakeCreateEngine = async () => ({
    chat: { completions: { async create(req) { seen = req; return { choices: [{ message: { content: '[]' } }] }; } } },
  });
  const engine = makeWebLLMEngine({ createEngine: fakeCreateEngine });
  await engine.chat([{ role: 'user', content: 'x' }], { grammar: 'root ::= "[]"' });
  assert.equal(seen.response_format.type, 'grammar');
  assert.equal(seen.response_format.grammar, 'root ::= "[]"');

  await engine.chat([{ role: 'user', content: 'y' }], { schema: 'root ::= "{}"' });
  assert.equal(seen.response_format.grammar, 'root ::= "{}"');   // schema is accepted as the grammar too
});

test('makeWebLLMEngine: load()/unload() lifecycle is best-effort and re-creatable', async () => {
  let created = 0; let unloaded = 0;
  const fakeCreateEngine = async () => ({
    chat: { completions: { async create() { return { choices: [{ message: { content: 'ok' } }] }; } } },
    async unload() { unloaded++; },
  });
  const engine = makeWebLLMEngine({ createEngine: () => { created++; return fakeCreateEngine(); } });
  await engine.load();
  assert.equal(created, 1);
  await engine.unload();
  assert.equal(unloaded, 1);
  await engine.chat([{ role: 'user', content: 'z' }]);   // re-creates after unload
  assert.equal(created, 2);
});

test('makeWebLLMEngine: missing createEngine throws (never imports web-llm itself)', () => {
  assert.throws(() => makeWebLLMEngine({}), /injected createEngine/);
});
