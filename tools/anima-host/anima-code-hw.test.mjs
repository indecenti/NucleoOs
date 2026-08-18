// anima-code-hw.test.mjs — F2 (gated hardware) + F3 (grounded veto in the local loop). What is pinned:
//
//   F2  A hardware tool is offered to the model ONLY when the app's manifest declares its permission;
//       every ACT capability is mutating (always-confirm); args are validated before the request
//       leaves the browser; and capguard turns UNGRANTED hardware actuation into a hard VETO, not a
//       warning — firing the IR blaster is not undoable like a workspace file.
//   F3  The local agent loop routes a code write through the injected verify(): a veto does NOT
//       execute and its reason re-enters the loop (self-repair); a pass runs normally. And the wllama
//       adapter renders chat correctly so the CPU rung is genuinely runnable.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { HW_CAPABILITIES, toAgentTools, capabilityForTool, HW_MUTATING, validateArgs, toolName } from '../../apps/code-runner/www/nucleo-hw.js';
import { assess } from '../../apps/anima/www/forge/capguard.js';
import { runWorkerLocal } from '../../apps/agent/www/local-worker.js';
import { toGBNF, grammarAccepts } from '../../apps/anima/www/forge/grammar.js';
import { formatChatML, applyTemplate, toWllamaOpts, makeWllamaCreateEngine } from '../../apps/agent/www/wllama-adapter.js';

const grammar = { toGBNF, grammarAccepts };

// ---- F2: the gate is the manifest permission --------------------------------------------------
test('every hardware capability declares a permission and a kind', () => {
  for (const c of HW_CAPABILITIES) {
    assert.ok(typeof c.permission === 'string' && c.permission, c.id + ' has no permission');
    assert.ok(c.kind === 'act' || c.kind === 'read', c.id + ' bad kind');
  }
});

test('the permissions a hardware app needs are all in the manifest schema enum', async () => {
  const { readFileSync } = await import('node:fs');
  const schema = JSON.parse(readFileSync(new URL('../../schemas/manifest.schema.json', import.meta.url), 'utf8'));
  const enumSet = new Set(schema.$defs.capability.enum);
  for (const c of HW_CAPABILITIES) assert.ok(enumSet.has(c.permission), c.permission + ' (used by ' + c.id + ') missing from the enum');
});

test('gating: only capabilities whose permission is granted become tools', () => {
  // mirror runtime.js: HW_TOOLS = capabilities filtered by the granted permission set
  const grant = (perms) => HW_CAPABILITIES.filter((c) => perms.includes(c.permission));
  assert.deepEqual(grant([]).length, 0, 'no permission → no hardware tools at all');
  const irOnly = grant(['device.ir']).map((c) => c.id);
  assert.ok(irOnly.includes('ir.send') && irOnly.includes('ir.tvbgone'));
  assert.ok(!irOnly.includes('gpio.write'), 'a granted IR app must not get GPIO');
  assert.ok(grant(['device.gpio']).some((c) => c.id === 'gpio.write'));
});

test('every ACT capability is mutating (always-confirm) and read capabilities are not', () => {
  for (const c of HW_CAPABILITIES) {
    const isMut = HW_MUTATING.has(toolName(c.id));
    assert.equal(isMut, c.kind === 'act', c.id + ' mutating/kind mismatch');
  }
});

test('args are validated before a request could leave the browser', () => {
  assert.equal(validateArgs('gpio.write', { pin: 1, value: 1 }).ok, true);
  assert.equal(validateArgs('gpio.write', { pin: 'x' }).ok, false, 'a string pin is rejected');
  assert.equal(validateArgs('ir.tvbgone', { action: 'nope' }).ok, false, 'an off-enum action is rejected');
});

test('capguard: hardware actuation the app was NOT granted is a hard veto', () => {
  const code = 'await os.hw.ir.send({protocol:"nec",address:1,command:2});';
  const denied = assess(code, { granted: [] });
  assert.equal(denied.severity, 'block');
  assert.equal(denied.hwOverreach, true);
  const allowed = assess(code, { granted: ['hw'] });
  assert.notEqual(allowed.severity, 'block', 'granted → not a block on the hw axis');
});

// ---- F3: grounded veto in the local loop -------------------------------------------------------
function scripted(replies) { return { chat: async () => ({ text: replies.length > 1 ? replies.shift() : replies[0] }) }; }
function recorder() { const log = []; const et = async (name, input) => { log.push({ name, input }); return { content: 'ok' }; }; et.log = log; return et; }

test('a vetoed write does not execute; the reason re-enters the loop', async () => {
  const engine = scripted([
    '[{"op":"write","path":"a.js","content":"eval(\\"danger\\")"}]',   // capguard blocks eval
    '[{"op":"write","path":"a.js","content":"const x = 1;"}]',          // repaired
    '[{"op":"answer","text":"done"}]',
  ]);
  const execTool = recorder();
  const verify = async ({ code }) => { const a = assess(code, { granted: [] }); return { verdict: a.severity === 'block' ? 'veto' : 'pass', reasons: a.dangers.map((d) => d.kind) }; };
  const vetoes = [];
  const r = await runWorkerLocal({ engine, execTool, grammar, verify }, { task: 'x', onEvent: (e) => e.type === 'veto' && vetoes.push(e) });
  assert.equal(r.text, 'done');
  assert.equal(vetoes.length, 1, 'the eval write was vetoed once');
  assert.equal(execTool.log.filter((c) => c.name === 'write_file').length, 1, 'only the repaired write executed');
  assert.equal(execTool.log[0].input.content, 'const x = 1;');
});

test('with no verify injected, writes execute unchanged (opt-in veto)', async () => {
  const engine = scripted(['[{"op":"write","path":"a.js","content":"const x=1;"}]', '[{"op":"done"}]']);
  const execTool = recorder();
  await runWorkerLocal({ engine, execTool, grammar }, { task: 'x' });
  assert.equal(execTool.log.filter((c) => c.name === 'write_file').length, 1);
});

// ---- F3: the wllama chat adapter ---------------------------------------------------------------
test('formatChatML renders a transcript into ChatML the GGUF was trained on', () => {
  const p = formatChatML([{ role: 'system', content: 'S' }, { role: 'user', content: 'U' }]);
  assert.match(p, /<\|im_start\|>system\nS<\|im_end\|>/);
  assert.match(p, /<\|im_start\|>user\nU<\|im_end\|>/);
  assert.match(p, /<\|im_start\|>assistant\n$/, 'ends primed for the assistant turn');
});

test('applyTemplate falls back to ChatML for a non-ChatML template, never guesses', () => {
  assert.equal(applyTemplate('{{ some jinja }}', [{ role: 'user', content: 'x' }]), null);
  assert.ok(applyTemplate('...<|im_start|>...', [{ role: 'user', content: 'x' }]));
});

test('toWllamaOpts maps our opts and defaults sanely', () => {
  assert.equal(toWllamaOpts({ maxTokens: 128, temperature: 0.5 }).nPredict, 128);
  assert.equal(toWllamaOpts({ temperature: 0.5 }).sampling.temp, 0.5);
  assert.equal(toWllamaOpts({}).nPredict, 512);
});

test('the adapter wraps createCompletion in the createChatCompletion shape wasm-engine expects', async () => {
  const fakeInst = { getChatTemplate: () => null, createCompletion: async (prompt) => 'REPLY for ' + prompt.length + ' chars', exit: async () => {} };
  const createEngine = makeWllamaCreateEngine(async () => fakeInst);
  const eng = await createEngine('m');
  const res = await eng.createChatCompletion([{ role: 'user', content: 'hi' }], { maxTokens: 10 });
  assert.match(res.choices[0].message.content, /^REPLY/);
});
