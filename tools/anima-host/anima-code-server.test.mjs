// anima-code-server.test.mjs — the LOCAL-SERVER transport of ANIMA Code: the tool-using agent on a model
// that runs on the user's own PC (Ollama / LM Studio), the "OpenCode on a Cardputer" path. A scripted chat()
// stands in for the model; the real Ollama run is the browser E2E (tools/web-e2e/anima-code.e2e.mjs).
// What is pinned here:
//   1. the loop threads native tool calls → execTool → tool results until the model answers in text;
//   2. what small local models really do — a call written as TEXT, the same call repeated forever, an
//      unknown tool — is recovered / nudged, never executed blindly or shown as the answer;
//   3. the transcript stays inside a local window (old tool results trimmed, recent ones whole);
//   4. out of steps → a final tool-less summary, not silence;
//   5. the tool surface drops what needs a cloud key (and the network tools in Private);
//   6. OpenAI-compatible servers get string arguments + tool_call_id (ai-engines.toOpenAIMessages), and a
//      pinned model is the only one tried (no model switch mid-task);
//   7. ANIMA sends a TASK to the agent and a chat turn to plain chat, in all five languages (wantsAgent).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { runLocalToolLoop, parseTextToolCalls, trimOldToolResults, localToolDefs, CLIENT_TOOLS, toOpenAITools } from '../../apps/agent/www/agent-tools.js';
import { toOpenAIMessages, localComplete } from '../../web/shell/ai-engines.js';
import { wantsAgent, asksAboutContent } from '../../apps/anima/www/contextkit.js';

const TOOLS = toOpenAITools(localToolDefs(CLIENT_TOOLS));
function scripted(replies) {
  const seen = [];
  const chat = async (messages, o = {}) => { seen.push({ messages: messages.map((m) => ({ ...m })), o }); return replies.length > 1 ? replies.shift() : replies[0]; };
  return { chat, seen };
}

test('a native tool loop runs the tools and returns the final text', async () => {
  const files = { 'app.js': 'export const somma = (a, b) => a - b;\n' };
  const { chat, seen } = scripted([
    { text: '', toolCalls: [{ name: 'read_file', arguments: { path: 'app.js' } }] },
    { text: '', toolCalls: [{ name: 'edit_file', arguments: { path: 'app.js', old: 'a - b', new: 'a + b' } }] },
    { text: 'Fatto: somma ora somma.', toolCalls: [] },
  ]);
  const ran = [];
  const execTool = async (name, a) => {
    ran.push(name);
    if (name === 'read_file') return { content: files[a.path] };
    if (name === 'edit_file') { files[a.path] = files[a.path].replace(a.old, a.new); return { content: 'edited' }; }
    return { content: '?', is_error: true };
  };
  const messages = [{ role: 'system', content: 'sys' }, { role: 'user', content: 'correggi somma' }];
  const out = await runLocalToolLoop({ chat, execTool, messages, tools: TOOLS });
  assert.equal(out, 'Fatto: somma ora somma.');
  assert.deepEqual(ran, ['read_file', 'edit_file']);
  assert.equal(files['app.js'], 'export const somma = (a, b) => a + b;\n');
  // the wire shape: assistant tool_calls carry an id + OBJECT arguments; results are threaded by id AND name
  const asst = messages.find((m) => m.role === 'assistant' && m.tool_calls);
  assert.equal(typeof asst.tool_calls[0].function.arguments, 'object');
  const tool = messages.find((m) => m.role === 'tool');
  assert.equal(tool.tool_call_id, asst.tool_calls[0].id);
  assert.equal(tool.tool_name, 'read_file');
  assert.equal(seen.length, 3);
});

test('a tool call written as text is recovered; prose and unknown names are not executed', async () => {
  const known = new Set(['read_file', 'write_file']);
  const a = parseTextToolCalls('Leggo il file.\n<tool_call>{"name":"read_file","arguments":{"path":"a.js"}}</tool_call>', known);
  assert.deepEqual(a.calls, [{ name: 'read_file', arguments: { path: 'a.js' } }]);
  assert.equal(a.text, 'Leggo il file.');
  const b = parseTextToolCalls('```json\n{"name":"write_file","arguments":"{\\"path\\":\\"x\\",\\"content\\":\\"y\\"}"}\n```', known);
  assert.deepEqual(b.calls, [{ name: 'write_file', arguments: { path: 'x', content: 'y' } }]);
  assert.equal(parseTextToolCalls('Ecco un esempio: {"name":"rm_rf","arguments":{}}', known).calls.length, 0);
  assert.equal(parseTextToolCalls('{"name":"rm_rf","arguments":{}}', known).calls.length, 0, 'an unknown tool is never recovered');
  assert.equal(parseTextToolCalls('La risposta è 42.', known).calls.length, 0);

  const { chat } = scripted([{ text: '<tool_call>{"name":"sh","arguments":{"cmd":"ls"}}</tool_call>' }, { text: 'Ci sono 2 file.' }]);
  const ran = [];
  const out = await runLocalToolLoop({ chat, execTool: async (n) => { ran.push(n); return { content: 'a.js\nb.js' }; }, messages: [{ role: 'user', content: 'quali file?' }], tools: TOOLS });
  assert.deepEqual(ran, ['sh']);
  assert.equal(out, 'Ci sono 2 file.');
});

test('the same call repeated is nudged instead of re-run; an unknown tool gets the list', async () => {
  const same = { text: '', toolCalls: [{ name: 'sh', arguments: { cmd: 'ls' } }] };
  const { chat } = scripted([same, same, same, same, { text: '', toolCalls: [{ name: 'format_disk', arguments: {} }] }, { text: 'ok' }]);
  let runs = 0;
  const messages = [{ role: 'user', content: 'x' }];
  const out = await runLocalToolLoop({ chat, execTool: async () => { runs++; return { content: 'a.js' }; }, messages, tools: TOOLS });
  assert.equal(out, 'ok');
  assert.equal(runs, 2, 'a repeated identical call runs at most twice');
  const tools = messages.filter((m) => m.role === 'tool').map((m) => m.content);
  assert.match(tools[2], /already called sh/);
  assert.match(tools[4], /Unknown tool "format_disk".*read_file/);
});

test('old tool results are trimmed to fit a local window; the recent ones stay whole', () => {
  const big = 'x'.repeat(9000);
  const messages = [{ role: 'system', content: 's' }];
  for (let i = 0; i < 8; i++) { messages.push({ role: 'assistant', content: '', tool_calls: [{ id: 'c' + i, function: { name: 'read_file', arguments: {} } }] }); messages.push({ role: 'tool', tool_call_id: 'c' + i, content: big }); }
  const cut = trimOldToolResults(messages, { budget: 40000, keep: 3 });
  assert.ok(cut > 0);
  const tools = messages.filter((m) => m.role === 'tool');
  assert.ok(tools.slice(-3).every((m) => m.content.length === 9000), 'the 3 most recent results are untouched');
  assert.match(tools[0].content, /older result trimmed/);
  assert.equal(trimOldToolResults([{ role: 'tool', content: 'short' }], { budget: 40000 }), 0, 'under budget: nothing changes');
});

test('out of steps: one last tool-less call summarizes instead of going silent', async () => {
  let n = 0;
  const loopChat = async (m, o) => (o && o.noTools ? { text: 'Ho elencato i file; resta da scrivere il test.' } : { text: '', toolCalls: [{ name: 'sh', arguments: { cmd: 'ls p' + (n++) } }] });
  const out = await runLocalToolLoop({ chat: loopChat, execTool: async () => ({ content: 'ok' }), messages: [{ role: 'user', content: 'x' }], tools: TOOLS, maxSteps: 3 });
  assert.match(out, /^Ho elencato i file/);
  assert.match(out, /step budget exhausted/);
});

test('a Stop aborts the loop', async () => {
  const ac = new AbortController(); ac.abort();
  await assert.rejects(runLocalToolLoop({ chat: async () => ({ text: 'x' }), execTool: async () => ({}), messages: [], abort: ac.signal }), /stopped/);
});

test('the local tool surface drops cloud-only tools, and network tools in Private', () => {
  const names = localToolDefs(CLIENT_TOOLS).map((t) => t.name);
  assert.ok(names.includes('read_file') && names.includes('edit_file') && names.includes('run_js') && names.includes('weather'));
  assert.ok(!names.includes('generate_image') && !names.includes('transcribe'));
  assert.ok(!localToolDefs(CLIENT_TOOLS, { private: true }).some((t) => t.name === 'weather'));
  // one shell instead of six look-alike tools: a small model chooses better among fewer
  assert.equal(names[0], 'sh', 'the shell comes first');
  for (const dup of ['list_files', 'search_files', 'make_dir', 'move_file', 'delete_file', 'append_file', 'list_apps']) assert.ok(!names.includes(dup), dup + ' is covered by sh for local models');
  assert.ok(CLIENT_TOOLS.some((t) => t.name === 'list_files'), 'cloud models keep the full set');
});

test('OpenAI-compatible servers get string arguments and tool_call_id', () => {
  const out = toOpenAIMessages([
    { role: 'system', content: 's' },
    { role: 'assistant', content: '', tool_calls: [{ id: 'call_0_0', type: 'function', function: { name: 'read_file', arguments: { path: 'a.js' } } }] },
    { role: 'tool', tool_call_id: 'call_0_0', tool_name: 'read_file', content: 'x' },
  ]);
  assert.equal(out[1].tool_calls[0].function.arguments, '{"path":"a.js"}');
  assert.deepEqual(out[2], { role: 'tool', tool_call_id: 'call_0_0', content: 'x' });
  assert.deepEqual(out[0], { role: 'system', content: 's' });
});

test('a pinned model is the only one tried, with the agent context window', async () => {
  const bodies = [];
  const f = async (url, init) => {
    bodies.push(JSON.parse(init.body));
    const line = JSON.stringify({ message: { content: 'ciao' }, done: true, eval_count: 3, eval_duration: 1e8 });
    return { ok: true, status: 200, body: null, text: async () => line };
  };
  const servers = [{ id: 'ollama', kind: 'ollama', name: 'Ollama', base: 'http://localhost:11434', status: 'ok', models: [{ id: 'big:9b', params: '9B', caps: { tools: true } }, { id: 'small:2b', params: '2B', caps: { tools: true } }] }];
  const r = await localComplete('agent', { messages: [{ role: 'user', content: 'x' }], servers, model: 'small:2b', numCtx: 16384, storage: null, fetch: f, config: { enabled: true, servers: [], models: {} } });
  assert.equal(r.engine.model, 'small:2b');
  assert.equal(bodies.length, 1);
  assert.equal(bodies[0].model, 'small:2b');
  assert.equal(bodies[0].options.num_ctx, 16384);
});

test('ANIMA: a task goes to the agent, a chat turn does not (5 languages)', () => {
  const tasks = [
    'Correggi la funzione somma in app.js', 'crea una pagina html con un orologio', 'leggi README.md',
    'Create a todo app', 'refactor the code in utils.js', 'Escribe un archivo con la lista de la compra',
    'Écris un fichier avec la liste', 'Crée une page avec une horloge', 'Erstelle eine Datei mit einer Liste', 'Ändere die Datei im Ordner',
    // questions ABOUT the files need the tools to look (live miss 2026-10-01: a tool-less chat said it had no access)
    'Quante righe ha ogni file .md nello spazio di lavoro? Dammi una tabella.', 'How many .js files are in the workspace?',
    'elenca i file della cartella demo', '¿Cuántos archivos hay en la carpeta demo?', 'Combien de fichiers dans le dossier demo ?', 'Wie viele Dateien sind im Ordner demo?',
    // which/where questions too (live miss: a chat invented a README's content and answered "0 files")
    'In quali file dello spazio di lavoro compare la parola NucleoOS?', 'Which files in the workspace mention ESP32?',
    'Dove si trova il file README nella cartella demo?', '¿En qué archivos aparece la palabra demo?', 'Quels fichiers contiennent le mot demo ?', 'Welche Dateien enthalten das Wort demo?',
  ];
  const chats = [
    'ciao come stai', 'che ore sono', 'apri la calcolatrice', 'aggiungi un evento domani alle 9', "cos'è nucleoos",
    'what is the weather in Rome', 'raccontami una storia', 'wie spät ist es', 'quanto fa 2+2', 'traduci ciao in inglese',
    '¿qué hora es?', 'quelle heure est-il', 'erzähl mir einen Witz',
    'dammi una ricetta per la carbonara', 'quanta batteria ho', 'mostrami una barzelletta', 'how many planets are there',
  ];
  for (const q of tasks) assert.ok(wantsAgent(q), 'task: ' + q);
  for (const q of chats) assert.ok(!wantsAgent(q), 'chat: ' + q);
  assert.ok(wantsAgent('scrivi uno script python', { workspace: true }), 'with a workspace open, a coding request is a task');
});

test('ANIMA: "read X and tell me…" is a question for the agent, "read X" just shows the file', () => {
  for (const q of ['Read notes.md and tell me how many items are on the list.', 'leggi notes.md e dimmi quanti elementi ci sono', 'cosa fa app.js?',
    'Lee notes.md y dime cuántos elementos hay', 'Lis notes.md et résume-le', 'Lies notes.md und sag mir, wie viele Einträge es gibt']) assert.ok(asksAboutContent(q), q);
  for (const q of ['leggi notes.md', 'read notes.md', 'apri README.md', 'mostra app.js', 'elenca i file']) assert.ok(!asksAboutContent(q), q);
});

test('search_files leniency: a phrase with no hit is retried on its most distinctive term', async () => {
  const { searchFallbackTerms } = await import('../../apps/agent/www/agent-tools.js');
  assert.deepEqual(searchFallbackTerms('function euro'), ['euro']);
  assert.deepEqual(searchFallbackTerms('const formatPrice ='), ['formatPrice']);
  assert.deepEqual(searchFallbackTerms('euro'), [], 'a single term has nothing to fall back to');
  assert.deepEqual(searchFallbackTerms('def parse_line'), ['parse_line']);
});
