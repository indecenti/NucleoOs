// agent-contract-check.mjs — DETERMINISTIC (no API) verification of the multi-agent CONTRACT layer in
// apps/agent/www/agent-tools.js: the tool schema mapping, the deterministic plan guard, and the
// OpenAI tool-use loop (tool_call → tool_result threading). Complements the live grok-live-check.mjs.
//   node tools/anima-host/agent-contract-check.mjs
import { CLIENT_TOOLS, MUTATING, ALWAYS_CONFIRM, toOpenAITools, guardPlan, extractJson, runOpenAIToolLoop } from '../../apps/agent/www/agent-tools.js';

let pass = 0, fail = 0; const fails = [];
const ok = (name, cond) => { if (cond) pass++; else { fail++; fails.push(name); } };

/* ---- tool schema mapping (worker↔OS contract for Groq/OpenAI) ---- */
const oa = toOpenAITools(CLIENT_TOOLS);
ok('maps all tools', oa.length === CLIENT_TOOLS.length);
ok('openai function shape', oa.every((t) => t.type === 'function' && t.function && t.function.name && t.function.parameters));
ok('write_file params preserved', oa.find((t) => t.function.name === 'write_file').function.parameters.required.includes('content'));

/* ---- app-creation tools are in the contract for EVERY provider, and gated ---- */
ok('scaffold_app present', CLIENT_TOOLS.some((t) => t.name === 'scaffold_app'));
ok('publish_app present', CLIENT_TOOLS.some((t) => t.name === 'publish_app'));
ok('publish_app requires id', oa.find((t) => t.function.name === 'publish_app').function.parameters.required.includes('id'));
ok('scaffold_app is mutating (gated)', MUTATING.has('scaffold_app'));
ok('publish_app always confirms', MUTATING.has('publish_app') && ALWAYS_CONFIRM.has('publish_app'));
ok('manage_app present + requires id & action', CLIENT_TOOLS.some((t) => t.name === 'manage_app') && oa.find((t) => t.function.name === 'manage_app').function.parameters.required.includes('action'));
ok('manage_app always confirms', MUTATING.has('manage_app') && ALWAYS_CONFIRM.has('manage_app'));
ok('generate_image present + requires prompt & path', CLIENT_TOOLS.some((t) => t.name === 'generate_image') && ['prompt', 'path'].every((k) => oa.find((t) => t.function.name === 'generate_image').function.parameters.required.includes(k)));
ok('generate_image is mutating (gated, writes a file)', MUTATING.has('generate_image'));
ok('transcribe present + requires path + NOT mutating (read-only)', CLIENT_TOOLS.some((t) => t.name === 'transcribe') && oa.find((t) => t.function.name === 'transcribe').function.parameters.required.includes('path') && !MUTATING.has('transcribe'));

/* ---- gating set invariants (catch a typo in MUTATING / ALWAYS_CONFIRM) ---- */
ok('ALWAYS_CONFIRM ⊆ MUTATING (so the gate fires)', [...ALWAYS_CONFIRM].every((n) => MUTATING.has(n)));
ok('every MUTATING name is a real tool', [...MUTATING].every((n) => CLIENT_TOOLS.some((t) => t.name === n)));
ok('every ALWAYS_CONFIRM name is a real tool', [...ALWAYS_CONFIRM].every((n) => CLIENT_TOOLS.some((t) => t.name === n)));
ok('tool names are unique', new Set(CLIENT_TOOLS.map((t) => t.name)).size === CLIENT_TOOLS.length);

/* ---- deterministic plan guard (device/tool requests can't slip through as a fabricated answer) ---- */
ok('guard forces task: time', guardPlan({ mode: 'answer', answer: 'Sono le 12' }, 'che ore sono?').mode === 'task');
ok('guard forces task: weather', guardPlan({ mode: 'answer', answer: 'Sole' }, 'che tempo fa a Roma?').mode === 'task');
ok('guard forces task: open app', guardPlan({ mode: 'answer', answer: 'ok' }, 'apri la calcolatrice').mode === 'task');
ok('guard forces task: space', guardPlan({ mode: 'answer', answer: '10GB' }, 'quanto spazio libero ho?').mode === 'task');
ok('guard forces task: write file', guardPlan({ mode: 'answer' }, 'scrivi un file note.txt').mode === 'task');
ok('guard forces task EN', guardPlan({ mode: 'answer' }, 'what time is it?').mode === 'task');
ok('guard keeps chitchat as answer', guardPlan({ mode: 'answer', answer: 'Bene!' }, 'ciao come stai?').mode === 'answer');
ok('guard keeps story as answer', guardPlan({ mode: 'answer', answer: '...' }, 'scrivimi una poesia sul mare').mode === 'answer');
ok('guard null → task', guardPlan(null, 'qualsiasi cosa').mode === 'task');
ok('guard passes through a real task', guardPlan({ mode: 'task', hard: true }, 'refactor il file').mode === 'task');

/* ---- tolerant JSON extraction ---- */
ok('extractJson plain', extractJson('{"mode":"task"}').mode === 'task');
ok('extractJson wrapped', extractJson('Ecco il piano: {"mode":"answer","answer":"ok"} fine').mode === 'answer');
ok('extractJson junk → null', extractJson('niente json qui') === null);

/* ---- the tool-use loop threads tool_call → tool_result and answers from it (FAKE model, no API) ---- */
(async () => {
  const calls = [];
  const execTool = async (name, args) => { calls.push({ name, args }); return { content: name === 'read_file' ? 'riga1\nriga2' : '✔ ' + name + ' ' + (args.path || '') }; };
  let step = 0; let threadedToolContent = null;
  const callModel = async (messages) => {
    if (step++ === 0) return { role: 'assistant', content: '', tool_calls: [{ id: 'c1', function: { name: 'write_file', arguments: JSON.stringify({ path: 'x.txt', content: 'hi' }) } }] };
    const lastTool = messages.filter((m) => m.role === 'tool').pop();   // the loop must have appended the tool result
    threadedToolContent = lastTool ? lastTool.content : null;
    return { role: 'assistant', content: 'Fatto.' };
  };
  const messages = [{ role: 'system', content: 'sys' }, { role: 'user', content: 'crea x.txt' }];
  const out = await runOpenAIToolLoop({ callModel, execTool, messages, maxSteps: 5 });
  ok('loop executed the tool', calls.length === 1 && calls[0].name === 'write_file' && calls[0].args.path === 'x.txt');
  ok('loop threaded tool_result by id', /✔ write_file x\.txt/.test(threadedToolContent || ''));
  ok('loop returns final text', out === 'Fatto.');
  ok('loop appended assistant+tool messages', messages.some((m) => m.role === 'tool' && m.tool_call_id === 'c1') && messages.some((m) => m.role === 'assistant' && m.tool_calls));

  // a model that never calls a tool just returns its text
  let s2 = 0; const out2 = await runOpenAIToolLoop({ callModel: async () => (s2++, { role: 'assistant', content: 'Risposta diretta.' }), execTool, messages: [{ role: 'user', content: 'ciao' }], maxSteps: 5 });
  ok('loop no-tool path returns text', out2 === 'Risposta diretta.' && s2 === 1);

  console.log(`\nagent-contract-check: ${pass} passed, ${fail} failed`);
  if (fail) { console.log('FAILED:\n - ' + fails.join('\n - ')); process.exit(1); }
  console.log('all green ✓');
})();
