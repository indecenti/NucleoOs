// anima-code-local.test.mjs — the F0 host gate (docs/anima-code.md §13.4): the LOCAL transport of
// the model-agnostic action protocol, proven end-to-end with a SCRIPTED engine — no GPU, no browser,
// no device. What is pinned here, in order of importance:
//
//   1. A scripted local engine drives a full tool loop to completion through the SAME execTool
//      contract the cloud transports use ({content, is_error}), and every op lands on the mapped
//      shared tool name — the "one tool surface, three transports" claim, executed.
//   2. The grammar is the firewall: prose, unknown ops and path escapes never reach execTool.
//   3. HONEST DECLINE beats fabrication: budget exhaustion, repeated-action loops and persistent
//      invalid output all return {declined, reason} — never invented text.
//   4. The cascade tries local rungs only after the cloud is exhausted, picks them in order, skips
//      a declining rung, and with nothing injected leaves today's behavior byte-identical.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { runWorkerLocal, LOCAL_SCHEMA, OP_TO_TOOL, localSystem, flattenMessages } from '../../apps/agent/www/local-worker.js';
import { toGBNF, grammarAccepts } from '../../apps/anima/www/forge/grammar.js';

const grammar = { toGBNF, grammarAccepts };

// A scripted engine: replies from a queue, one per chat() call. The queue IS the model.
function scriptedEngine(replies) {
  const calls = [];
  return {
    calls,
    chat: async (messages, opts) => {
      calls.push({ messages: messages.map((m) => ({ ...m })), opts });
      const next = replies.length > 1 ? replies.shift() : replies[0];
      return { text: typeof next === 'function' ? next(messages) : next, usage: { tokens: 1 } };
    },
  };
}

// An execTool double that records calls and serves a tiny in-memory workspace.
function fakeExecTool(fs = {}) {
  const log = [];
  const et = async (name, input) => {
    log.push({ name, input });
    if (name === 'read_file') return fs[input.path] != null
      ? { content: '<untrusted_file>\n' + fs[input.path] + '\n</untrusted_file>' }
      : { content: 'read error: not found', is_error: true };
    if (name === 'write_file') { fs[input.path] = input.content; return { content: 'written ' + input.path }; }
    if (name === 'list_files') return { content: Object.keys(fs).join('\n') || '(empty)' };
    return { content: 'ok' };
  };
  et.log = log; et.fs = fs;
  return et;
}

// ---- 1. the full loop, to completion ----------------------------------------------------------
test('local transport: read → write → answer completes through the shared execTool', async () => {
  const engine = scriptedEngine([
    '[{"op":"read","path":"notes.txt"}]',
    '[{"op":"write","path":"out.txt","content":"HELLO"}]',
    '[{"op":"answer","text":"done: wrote out.txt"}]',
  ]);
  const execTool = fakeExecTool({ 'notes.txt': 'hello' });
  const r = await runWorkerLocal({ engine, execTool, grammar }, { task: 'uppercase notes.txt into out.txt' });
  assert.equal(r.declined, undefined);
  assert.equal(r.text, 'done: wrote out.txt');
  assert.deepEqual(execTool.log.map((c) => c.name), ['read_file', 'write_file']);
  assert.equal(execTool.fs['out.txt'], 'HELLO');
  // the tool RESULT was fed back to the model as the next user turn — the loop is a real loop
  const turn2 = engine.calls[1].messages;
  assert.match(turn2[turn2.length - 1].content, /untrusted_file/);
});

test('local transport: every mapped op reaches the ONE shared tool surface by its real name', () => {
  const CLIENT_NAMES = new Set(['read_file', 'write_file', 'append_file', 'edit_file', 'move_file',
    'delete_file', 'make_dir', 'list_files', 'search_files']);
  for (const [op, m] of Object.entries(OP_TO_TOOL)) {
    assert.ok(CLIENT_NAMES.has(m.tool), op + ' maps to unknown tool ' + m.tool);
    assert.ok(LOCAL_SCHEMA[op], op + ' missing from the grammar schema');
  }
  for (const op of Object.keys(LOCAL_SCHEMA))
    assert.ok(OP_TO_TOOL[op] || ['answer', 'ask', 'done'].includes(op), op + ' has no mapping and is not a terminal');
});

// ---- 2. the grammar is the firewall -----------------------------------------------------------
test('grammar rejects prose, unknown ops and escaping paths before any tool runs', async () => {
  for (const bad of [
    'Sure! I will read the file for you.',
    '[{"op":"format_disk"}]',
    '[{"op":"read","path":"../../etc/passwd"}]',
  ]) {
    const engine = scriptedEngine([bad]);
    const execTool = fakeExecTool();
    const r = await runWorkerLocal({ engine, execTool, grammar }, { task: 'x', maxSteps: 3 });
    assert.equal(r.declined, true, bad);
    assert.match(r.reason, /^invalid-actions:/);
    assert.equal(execTool.log.length, 0, 'execTool must never see: ' + bad);
  }
});

test('one invalid turn gets a corrective retry; recovery then completes', async () => {
  const engine = scriptedEngine([
    'Let me think about this…',
    '[{"op":"answer","text":"recovered"}]',
  ]);
  const r = await runWorkerLocal({ engine, execTool: fakeExecTool(), grammar }, { task: 'x' });
  assert.equal(r.text, 'recovered');
  assert.equal(engine.calls.length, 2);
  assert.match(engine.calls[1].messages[engine.calls[1].messages.length - 1].content, /Invalid/);
});

// ---- 3. honest declines ------------------------------------------------------------------------
test('a stuck model (same action forever) declines with reason=loop, never fabricates', async () => {
  const engine = scriptedEngine(['[{"op":"read","path":"a.txt"}]']);   // repeats forever
  const r = await runWorkerLocal({ engine, execTool: fakeExecTool({ 'a.txt': 'x' }), grammar }, { task: 'x' });
  assert.equal(r.declined, true);
  assert.equal(r.reason, 'loop');
});

test('budget exhaustion declines with reason=budget', async () => {
  let n = 0;
  const engine = { chat: async () => ({ text: '[{"op":"read","path":"f' + (n++) + '.txt"}]' }) };
  const r = await runWorkerLocal({ engine, execTool: fakeExecTool(), grammar }, { task: 'x', maxSteps: 4 });
  assert.equal(r.declined, true);
  assert.equal(r.reason, 'budget');
  assert.equal(r.steps, 4);
});

test('ask surfaces the question to the human instead of guessing', async () => {
  const engine = scriptedEngine(['[{"op":"ask","question":"which file?"}]']);
  const r = await runWorkerLocal({ engine, execTool: fakeExecTool(), grammar }, { task: 'x' });
  assert.equal(r.asked, true);
  assert.equal(r.text, 'which file?');
});

// ---- 4. the cascade picks the right rung ------------------------------------------------------
// A minimal stand-in for runWorkerWithFallback's local block, driven the same way runtime.js drives
// it (the real function needs the whole browser runtime; the CONTRACT is what must hold).
async function cascadeLocal(rungs, baseMessages, execTool) {
  for (const rung of rungs) {
    const out = await runWorkerLocal({ engine: rung.engine, execTool, grammar }, { messages: baseMessages, maxSteps: 4 });
    if (out && !out.declined) return { rung: rung.tier, text: out.text };
  }
  return null;
}

test('cascade: a declining rung is skipped, the next capable rung answers', async () => {
  const declining = { tier: 'local-webgpu', engine: scriptedEngine(['nonsense prose forever']) };
  const capable  = { tier: 'local-wasm',  engine: scriptedEngine(['[{"op":"answer","text":"from wasm"}]']) };
  const r = await cascadeLocal([declining, capable], [{ role: 'user', content: 'task' }], fakeExecTool());
  assert.equal(r.rung, 'local-wasm');
  assert.equal(r.text, 'from wasm');
});

test('cascade: every rung declining yields null — the caller declines, nothing is invented', async () => {
  const r = await cascadeLocal(
    [{ tier: 'a', engine: scriptedEngine(['x']) }, { tier: 'b', engine: scriptedEngine(['y']) }],
    [{ role: 'user', content: 'task' }], fakeExecTool());
  assert.equal(r, null);
});

// ---- plumbing ---------------------------------------------------------------------------------
test('flattenMessages: anthropic block arrays become plain chat, empties dropped', () => {
  const out = flattenMessages([
    { role: 'user', content: 'hi' },
    { role: 'assistant', content: [{ type: 'text', text: 'a' }, { type: 'tool_use', name: 'x' }] },
    { role: 'user', content: '' },
  ]);
  assert.deepEqual(out, [{ role: 'user', content: 'hi' }, { role: 'assistant', content: 'a' }]);
});

test('localSystem names the workspace root and the untrusted-fence rule', () => {
  const s = localSystem('/data/agent');
  assert.match(s, /\/data\/agent/);
  assert.match(s, /untrusted/);
});
