// The REAL agent runtime (apps/agent/www/runtime.js) on the host: a scripted local model (the injected
// localServer engine — no network) drives the actual tool loop against an in-memory device, so the parts
// that only exist inside runtime.js are proven, not assumed:
//   • the open plan rides on every tool result (OpenCode re-injects its todo list each step);
//   • read_file pages through a file far past the model's read budget, by the offset the tail names;
//   • out of steps, the turn ends with the model's own summary.
import { register } from 'node:module';
import { test } from 'node:test';
import assert from 'node:assert/strict';

register('../lib/web-paths-loader.mjs', import.meta.url);
const { createRuntime } = await import('../../apps/agent/www/runtime.js');

// An in-memory device that answers like nucleo_fsapi + /api/status + /api/apps.
function fakeDevice(files = {}) {
  const store = new Map(Object.entries(files));
  const dirs = new Set(['/', '/data', '/data/agent']);
  for (const p of store.keys()) for (let d = p.slice(0, p.lastIndexOf('/')); d; d = d.slice(0, d.lastIndexOf('/'))) dirs.add(d);
  const res = (status, body = '') => ({ ok: status < 300, status, headers: new Map(), text: async () => String(body), json: async () => JSON.parse(body), arrayBuffer: async () => new TextEncoder().encode(String(body)).buffer });
  globalThis.fetch = async (url, { method = 'GET', body } = {}) => {
    const u = new URL(String(url), 'http://dev');
    if (u.pathname === '/api/status') return res(200, JSON.stringify({ version: 'test', free_heap: 80000, storage: { mounted: true }, network: { time: 1790808000 } }));
    if (u.pathname === '/api/apps') return res(200, JSON.stringify({ apps: [] }));
    const op = u.pathname.startsWith('/api/fs/') ? u.pathname.split('/').pop() : '', p = u.searchParams.get('path');
    if (op === 'read') return store.has(p) ? res(200, store.get(p)) : res(404, 'no file');
    if (op === 'mkdir') { dirs.add(p); return res(200, '{"ok":true}'); }
    if (op === 'write') { store.set(p, String(body)); return res(200, '{"ok":true}'); }
    if (op === 'list') {
      if (!dirs.has(p)) return res(404, 'no dir');
      const kids = [...store.keys()].filter((f) => f.slice(0, f.lastIndexOf('/')) === p).map((f) => ({ name: f.slice(p.length + 1), type: 'file', size: store.get(f).length }));
      return res(200, JSON.stringify({ entries: kids }));
    }
    return res(404, 'not here');
  };
  return store;
}

// A local "model" that plays a script: each entry gets the messages so far and returns a reply.
function scriptedEngine(script) {
  const calls = [];
  const E = {
    loadLocalConfig: () => ({ enabled: true }),
    liveServers: async () => [{ status: 'ok', models: ['scripted'], base: 'http://127.0.0.1:11434', name: 'test' }],
    isLoopback: () => true,
    localComplete: async (role, o) => {
      const msgs = o.messages.map((m) => ({ ...m }));
      calls.push({ msgs, noTools: !o.tools });
      const step = script[Math.min(calls.length - 1, script.length - 1)];
      const r = step(msgs, { noTools: !o.tools });
      return { text: r.text || '', toolCalls: r.tools || [], engine: { model: 'scripted', kind: 'ollama' } };
    },
  };
  return { E, calls };
}

const runtimeWith = (E, extra = {}) => createRuntime({ cfg: {}, lang: 'en', ui: {}, localServer: { engines: async () => E, first: () => true }, ...extra });
const lastTool = (msgs) => [...msgs].reverse().find((m) => m.role === 'tool');
const call = (name, args) => ({ name, arguments: args });

test('runtime: the open plan rides on every tool result, and read_file pages to any line', async () => {
  const big = Array.from({ length: 4000 }, (_, i) => 'record ' + (i + 1) + ' ' + 'v'.repeat(20)).join('\n') + '\n';   // ~120 KB
  fakeDevice({ '/data/agent/big.txt': big });
  let resumeAt = 0;
  const { E, calls } = scriptedEngine([
    () => ({ tools: [call('update_plan', { steps: [{ title: 'read the file', status: 'doing' }, { title: 'report', status: 'todo' }] })] }),
    () => ({ tools: [call('read_file', { path: 'big.txt' })] }),
    (msgs) => {
      const out = lastTool(msgs).content;
      resumeAt = +(/offset=(\d+)/.exec(out) || [])[1];
      return { tools: [call('read_file', { path: 'big.txt', offset: resumeAt })] };
    },
    () => ({ tools: [call('read_file', { path: 'big.txt', offset: 3999 })] }),
    () => ({ text: 'done' }),
  ]);
  const out = await runtimeWith(E).run('read big.txt and report');
  assert.equal(out, 'done');

  const planResult = lastTool(calls[1].msgs).content;
  assert.match(planResult, /▸ read the file/, 'update_plan returns the checklist');
  assert.ok(!planResult.includes('[plan'), 'update_plan is not followed by a duplicate reminder');

  const firstRead = lastTool(calls[2].msgs).content;
  assert.match(firstRead, /1→record 1 v/);
  assert.match(firstRead, /\[plan 0\/2 done · now: read the file · next: report\]$/, 'the plan rides on the result');
  assert.ok(firstRead.length < 9000 + 600, 'a local model gets about LOCAL_READ_CAP of text, not the whole file');
  assert.ok(resumeAt > 100 && resumeAt < 4000, 'the tail names where to resume');

  const secondRead = lastTool(calls[3].msgs).content;
  assert.match(secondRead, new RegExp(resumeAt + '→record ' + resumeAt + ' '), 'the offset lands exactly on the next line');
  assert.ok(!secondRead.includes('→record ' + (resumeAt - 1) + ' '), 'no overlap with the first page');

  const tailRead = lastTool(calls[4].msgs).content;
  assert.match(tailRead, /4000→record 4000 v+/, 'the last line of a 120 KB file is reachable');
});

test('runtime: out of steps, the turn ends with the model\'s own summary', async () => {
  fakeDevice({ '/data/agent/a.txt': 'alpha\n' });
  const { E, calls } = scriptedEngine([
    (msgs, o) => (o.noTools ? { text: 'Listed the folder twice; the report is not written yet.' } : { tools: [call('sh', { cmd: 'ls' })] }),
  ]);
  const out = await runtimeWith(E, { maxSteps: 2 }).run('make a report');
  assert.equal(calls.length, 3, 'two tool steps, then one call without tools');
  assert.equal(calls[2].noTools, true);
  assert.match(out, /^Listed the folder twice; the report is not written yet\./);
  assert.match(out, /step budget exhausted/);
});

// The Anthropic worker, against a fake Messages API. Out of steps, the summary request must be a VALID call:
// it rides in the last user turn (which holds the tool_results — two user turns in a row would break the
// alternation), keeps the tools declared (the history has tool_use blocks) and sets tool_choice "none".
test('runtime (Anthropic): out of steps → a valid tool_choice "none" call, summary returned', async () => {
  fakeDevice({ '/data/agent/a.txt': 'alpha\n' });
  const device = globalThis.fetch;
  const bodies = [];
  globalThis.fetch = async (url, o = {}) => {
    const u = new URL(String(url), 'http://dev');
    if (u.hostname !== 'api.anthropic.com') return device(url, o);
    const json = (status, j) => ({ ok: status < 300, status, headers: new Map(), json: async () => j, text: async () => JSON.stringify(j) });
    if (u.pathname !== '/v1/messages') return json(404, { error: { type: 'not_found_error', message: 'no' } });
    const b = JSON.parse(o.body); bodies.push(b);
    if (!b.tools) return json(200, { content: [{ type: 'text', text: '{"mode":"task","plan":"list"}' }], stop_reason: 'end_turn' });   // orchestrator triage
    if (b.tool_choice && b.tool_choice.type === 'none') return json(200, { content: [{ type: 'text', text: 'Listed the folder; nothing written yet.' }], stop_reason: 'end_turn' });
    return json(200, { content: [{ type: 'tool_use', id: 'tu' + bodies.length, name: 'sh', input: { cmd: 'ls' } }], stop_reason: 'tool_use' });
  };
  try {
    const rt = createRuntime({ cfg: { provider: 'anthropic', key: 'test-key', model: 'claude-haiku-4-5-20251001' }, lang: 'en', ui: {}, maxSteps: 2 });
    const out = await rt.run('make a report from the files');
    assert.match(out, /^Listed the folder; nothing written yet\./);
    assert.match(out, /step budget exhausted/);
    const last = bodies[bodies.length - 1];
    assert.deepEqual(last.tool_choice, { type: 'none' });
    assert.ok(Array.isArray(last.tools) && last.tools.length, 'tools stay declared');
    const roles = last.messages.map((m) => m.role);
    for (let i = 1; i < roles.length; i++) assert.notEqual(roles[i], roles[i - 1], 'roles alternate: ' + roles.join(','));
    const tail = last.messages[last.messages.length - 1];
    assert.equal(tail.role, 'user');
    assert.equal(tail.content[0].type, 'tool_result');
    assert.equal(tail.content[tail.content.length - 1].type, 'text');
    assert.match(tail.content[tail.content.length - 1].text, /Step budget reached/);
    const worker = bodies.filter((b) => b.tools && !b.tool_choice);
    assert.equal(worker.length, 2, 'exactly maxSteps tool rounds before the summary');
  } finally { globalThis.fetch = device; }
});

// hostAnswers: ANIMA answers plain questions with its own grounded chat. The triage (cheapest model, no NucleoOS
// facts) answered "what is NucleoOS?" with "a Linux IoT OS for STM32 Nucleo boards" on the real Cardputer
// (2026-10-10) — for such a caller its "answer" is a classification only.
test('runtime: hostAnswers hands a plain question back instead of the triage model\'s own answer', async () => {
  const device = fakeDevice({});
  globalThis.fetch = async (url, o = {}) => {
    const u = new URL(String(url), 'http://dev');
    if (u.hostname !== 'api.anthropic.com') return device(url, o);
    const json = (status, j) => ({ ok: status < 300, status, headers: new Map(), json: async () => j, text: async () => JSON.stringify(j) });
    return json(200, { content: [{ type: 'text', text: '{"mode":"answer","answer":"NucleoOS is a Linux IoT OS for STM32 Nucleo boards."}' }], stop_reason: 'end_turn' });
  };
  try {
    const rt = createRuntime({ cfg: { provider: 'anthropic', key: 'test-key', model: 'claude-haiku-4-5-20251001' }, lang: 'it', ui: {} });
    assert.equal(await rt.run('che cos\'è NucleoOS?', [], { hostAnswers: true }), null, 'the host answers it');
    assert.match(await rt.run('che cos\'è NucleoOS?'), /STM32/, 'the Agenti app keeps the direct answer');
  } finally { globalThis.fetch = device; }
});

// replyLang + the PC model's app list: the prompt NAMES the user's language when it is not the OS one (a 9B model
// summed up a Spanish task in Italian), and points a local model at `sh apps` — it has no list_apps.
test('runtime: the reply language is named, and a PC model is told to list apps with sh', async () => {
  fakeDevice({});
  const { E, calls } = scriptedEngine([() => ({ text: 'Hecho.' })]);
  const rt = createRuntime({ cfg: {}, lang: 'it', ui: {}, localServer: { engines: async () => E, first: () => true } });
  assert.equal(await rt.run('¿qué apps de música hay?', [], { replyLang: 'es' }), 'Hecho.');
  const sys = calls[0].msgs[0].content;
  assert.match(sys, /LINGUA DELLA RISPOSTA: l'utente ha scritto in español/);
  assert.match(sys, /comando apps le elenca TUTTE/);
  assert.doesNotMatch(sys, /list_apps: elenca/, 'no tool the local model does not have');
  await rt.run('ciao', [], { replyLang: 'it' });
  assert.doesNotMatch(calls[1].msgs[0].content, /LINGUA DELLA RISPOSTA/, 'nothing to name when it is the OS language');
});

// A small model that ENDS on a present-tense announcement ("Ich füge … hinzu.") had changed nothing: one nudge, then it acts.
test('runtime: a present-tense announcement with no tool call is nudged once, and the work happens', async () => {
  const store = fakeDevice({ '/data/agent/saluti.txt': 'Ciao\n' });
  const { E, calls } = scriptedEngine([
    () => ({ text: 'Ich füge die Zeile „Hola“ am Ende der Datei saluti.txt hinzu.' }),
    () => ({ tools: [call('write_file', { path: 'saluti.txt', content: 'Ciao\nHola\n' })] }),
    () => ({ text: 'Erledigt.' }),
  ]);
  const rt = createRuntime({ cfg: {}, lang: 'de', ui: { autoApprove: () => true, confirm: async () => true }, localServer: { engines: async () => E, first: () => true } });   // as ANIMA with auto-approve
  assert.equal(await rt.run('Füge am Ende dieser Datei noch eine Zeile mit Hola hinzu.'), 'Erledigt.');
  assert.match(calls[1].msgs[calls[1].msgs.length - 1].content, /did not call any tool/, 'the nudge');
  assert.equal(store.get('/data/agent/saluti.txt'), 'Ciao\nHola\n');
  const { ANNOUNCES_WORK } = await import('../../apps/agent/www/agent-tools.js');
  for (const t of ['Aggiungo la riga in fondo.', 'Añado la línea al final.', "J'ajoute la ligne à la fin.", "I'm adding the line now."]) assert.match(t, ANNOUNCES_WORK, t);
  for (const t of ['Ho aggiunto la riga.', 'He añadido la línea.', 'Done: the line is there.', 'Die Zeile wurde hinzugefügt.']) assert.doesNotMatch(t, ANNOUNCES_WORK, t);
});

// The user named a file, not an app: scaffold/publish is refused with a pointer to write_file (live, the 9B went for
// publish_app /apps/timer/ on "crea la pagina timer.html: …" and wrote no file). An app request still passes.
test('runtime: a named file is written, not turned into an app to publish', async () => {
  const store = fakeDevice({});
  const { E, calls } = scriptedEngine([
    () => ({ tools: [call('scaffold_app', { name: 'Timer', description: 'countdown', category: 'tools', kind: 'timer' })] }),
    (msgs) => ({ tools: [call('write_file', { path: 'timer.html', content: '<!doctype html><button>Avvia</button><script>setInterval(()=>{},1000)</script>' })] }),
    () => ({ text: 'Fatto.' }),
  ]);
  const rt = createRuntime({ cfg: {}, lang: 'it', ui: { autoApprove: () => true, confirm: async () => true }, localServer: { engines: async () => E, first: () => true } });
  assert.equal(await rt.run('Crea la pagina timer.html: un conto alla rovescia di 10 secondi che parte quando premi Avvia.'), 'Fatto.');
  const toolMsg = calls[1].msgs.find((m) => m.role === 'tool');
  assert.match(toolMsg.content, /asked for the file timer\.html/);
  assert.match(store.get('/data/agent/timer.html'), /Avvia/);
});
