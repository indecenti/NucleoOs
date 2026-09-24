// Unit test for the pure offline-resolution policy (apps/anima/www/local/cascade.js).
//
// No WASM, no network, no browser — the policy is exercised with mocked tier runners so the
// guarantee the user asked for ("the in-browser WASM brain is tried BEFORE we scale onto the
// Cardputer") is pinned and can't regress. Run:  node --test apps/anima/local/cascade.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const mod = join(here, '..', 'www', 'local', 'cascade.js');
const { answered, resolveOffline, classifyCommand, deviceToolOutcome, commandHint, memoryHint, DEVICE_TOOLS, actRequest } = await import(pathToFileURL(mod).href);

// A runner factory that records call order into `log` and returns a fixed result.
const rec = (log, name, result) => async () => { log.push(name); return result; };
const ans = (extra = {}) => ({ reply: 'x', tier: 'fact', ...extra });   // a real answer

test('answered(): tier-agnostic predicate', () => {
  assert.equal(answered(null), false);
  assert.equal(answered({ reply: '' }), false, 'empty reply -> abstention');
  assert.equal(answered({ reply: 'x', tier: 'none', action: 'none', domain: 'none' }), false, 'no signal -> abstention');
  assert.equal(answered({ reply: 'x', tier: 'fact' }), true);
  assert.equal(answered({ reply: 'x', tier: 'none', action: 'launch' }), true, 'action counts');
  // STITCH/L2: shaper drops tier to 'none' but the reply + domain are real -> must still be "answered".
  assert.equal(answered({ reply: 'la fotosintesi è…', tier: 'none', domain: 'knowledge' }), true, 'STITCH-as-answer');
});

test('ordering: browser is tried before the device', async () => {
  const log = [];
  const r = await resolveOffline('q', 'it', { prefer: 'browser', silent: false }, {
    browser: rec(log, 'browser', ans({ local: true })),
    device: rec(log, 'device', ans()),
  });
  assert.deepEqual(log, ['browser'], 'device must not be called once the browser answered');
  assert.equal(r.local, true, 'the browser result is returned');
});

test('abstain passthrough: browser abstains -> device answers', async () => {
  const log = [];
  const r = await resolveOffline('q', 'it', { prefer: 'browser', silent: false }, {
    browser: rec(log, 'browser', { reply: '' }),            // abstention
    device: rec(log, 'device', ans({ source: 'device' })),
  });
  assert.deepEqual(log, ['browser', 'device']);
  assert.equal(r.source, 'device');
});

test('STITCH-as-answer is returned, not skipped', async () => {
  const r = await resolveOffline('q', 'it', { prefer: 'browser', silent: false }, {
    browser: async () => ({ reply: 'la fotosintesi è…', tier: 'none', domain: 'knowledge', local: true }),
    device: async () => ans(),
  });
  assert.equal(r.local, true);
  assert.match(r.reply, /fotosintesi/);
});

test('silent skip: an unprovisioned browser brain is never called', async () => {
  const log = [];
  const r = await resolveOffline('q', 'it', { prefer: 'browser', silent: true }, {
    browser: rec(log, 'browser', ans({ local: true })),     // would answer, but...
    device: rec(log, 'device', ans({ source: 'device' })),
    browserProvisioned: async () => false,                  // ...not provisioned
  });
  assert.deepEqual(log, ['device'], 'browser runner must not run when unprovisioned + silent');
  assert.equal(r.source, 'device');
});

test('silent + provisioned: the browser IS used', async () => {
  const log = [];
  const r = await resolveOffline('q', 'it', { prefer: 'browser', silent: true }, {
    browser: rec(log, 'browser', ans({ local: true })),
    device: rec(log, 'device', ans({ source: 'device' })),
    browserProvisioned: async () => true,
  });
  assert.deepEqual(log, ['browser']);
  assert.equal(r.local, true);
});

test('device-first: prefer device, browser as fallback', async () => {
  const log = [];
  const r = await resolveOffline('q', 'it', { prefer: 'device', silent: true }, {
    browser: rec(log, 'browser', ans({ local: true })),
    device: rec(log, 'device', { reply: '' }),              // device abstains
    browserProvisioned: async () => true,
  });
  assert.deepEqual(log, ['device', 'browser'], 'device tried first, then browser');
  assert.equal(r.local, true);
});

test('null when nothing answers', async () => {
  const r = await resolveOffline('q', 'it', { prefer: 'browser', silent: false }, {
    browser: async () => ({ reply: '' }),
    device: async () => null,
  });
  assert.equal(r, null);
});

test('a throwing runner is treated as an abstention (never throws)', async () => {
  const log = [];
  const r = await resolveOffline('q', 'it', { prefer: 'browser', silent: false }, {
    browser: async () => { log.push('browser'); throw new Error('boom'); },
    device: rec(log, 'device', ans({ source: 'device' })),
  });
  assert.deepEqual(log, ['browser', 'device']);
  assert.equal(r.source, 'device');
});

test('a throwing browserProvisioned() is treated as not-provisioned', async () => {
  const log = [];
  const r = await resolveOffline('q', 'it', { prefer: 'browser', silent: true }, {
    browser: rec(log, 'browser', ans({ local: true })),
    device: rec(log, 'device', ans({ source: 'device' })),
    browserProvisioned: async () => { throw new Error('boom'); },
  });
  assert.deepEqual(log, ['device']);
  assert.equal(r.source, 'device');
});

// ---- web-index tier: browser WASM brain -> web index -> device ------------------------------------
test('webindex sits between the browser brain and the device', async () => {
  const log = [];
  const r = await resolveOffline('chi è X', 'it', { prefer: 'browser', silent: false }, {
    browser: rec(log, 'browser', { reply: '' }),                 // WASM brain abstains
    webindex: rec(log, 'webindex', ans({ web: true, local: true, intent: 'wikipedia' })),
    device: rec(log, 'device', ans({ source: 'device' })),
  });
  assert.deepEqual(log, ['browser', 'webindex'], 'web index tried after the brain, before the device');
  assert.equal(r.web, true);
});

test('webindex is NOT silent-gated (runs even when the WASM brain is unprovisioned)', async () => {
  const log = [];
  const r = await resolveOffline('chi è X', 'it', { prefer: 'browser', silent: true }, {
    browser: rec(log, 'browser', ans({ local: true })),         // would answer, but unprovisioned -> skipped
    webindex: rec(log, 'webindex', ans({ web: true, local: true })),
    device: rec(log, 'device', ans({ source: 'device' })),
    browserProvisioned: async () => false,
  });
  assert.deepEqual(log, ['webindex'], 'unprovisioned brain skipped; web index still runs (lightweight)');
  assert.equal(r.web, true);
});

test('webindex abstains -> device answers', async () => {
  const log = [];
  const r = await resolveOffline('chi è X', 'it', { prefer: 'browser', silent: false }, {
    browser: rec(log, 'browser', { reply: '' }),
    webindex: rec(log, 'webindex', null),                        // no web answer (offline / no match)
    device: rec(log, 'device', ans({ source: 'device' })),
  });
  assert.deepEqual(log, ['browser', 'webindex', 'device']);
  assert.equal(r.source, 'device');
});

test('device-first order: device, then browser, then webindex', async () => {
  const log = [];
  const r = await resolveOffline('chi è X', 'it', { prefer: 'device', silent: true }, {
    browser: rec(log, 'browser', { reply: '' }),
    webindex: rec(log, 'webindex', ans({ web: true })),
    device: rec(log, 'device', { reply: '' }),                  // device abstains
    browserProvisioned: async () => true,
  });
  assert.deepEqual(log, ['device', 'browser', 'webindex']);
  assert.equal(r.web, true);
});

test('missing webindex runner is simply skipped (back-compat)', async () => {
  const log = [];
  const r = await resolveOffline('q', 'it', { prefer: 'browser', silent: false }, {
    browser: rec(log, 'browser', { reply: '' }),
    device: rec(log, 'device', ans({ source: 'device' })),       // no webindex runner provided
  });
  assert.deepEqual(log, ['browser', 'device']);
  assert.equal(r.source, 'device');
});

// ---- commands first + honest device actions ----------------------------------------------------

test('classifyCommand(): device tools, live state, client hand-offs, everything else', () => {
  for (const tool of ['add_event', 'set_volume', 'set_brightness', 'create_file'])
    assert.equal(classifyCommand({ action: 'tool', tool, reply: 'x' }), 'device', tool);
  assert.ok(DEVICE_TOOLS.has('set_volume'));
  assert.equal(classifyCommand({ action: 'tool', tool: '', intent: 'add_event' }), 'device', 'intent fallback');
  assert.equal(classifyCommand({ action: 'system', arg: 'time', reply: '{value}.' }), 'live');
  assert.equal(classifyCommand({ action: 'launch', arg: 'calculator' }), 'client');
  assert.equal(classifyCommand({ action: 'tool', tool: 'open_file', arg: '/data/a.txt' }), 'client');
  assert.equal(classifyCommand({ action: 'tool', tool: 'translate' }), null, 'a pure tool is not a device action');
  assert.equal(classifyCommand({ action: 'answer', tier: 'fact', reply: 'Parigi' }), null);
  assert.equal(classifyCommand(null), null);
});

test('deviceToolOutcome(): only the device can make an action "done"', () => {
  assert.equal(deviceToolOutcome({ local: true, action: 'tool', tool: 'add_event', reply: 'Aggiunto' }), 'proposed', 'browser engine never executes');
  assert.equal(deviceToolOutcome({ action: 'tool', tool: 'set_volume', done: true }), 'done');
  assert.equal(deviceToolOutcome({ action: 'tool', tool: 'add_event', done: false, reply: 'Per aggiungere eventi devo essere associato' }), 'failed');
  // firmware older than the "done" flag: set_* were proposed but never executed by /api/anima
  assert.equal(deviceToolOutcome({ action: 'tool', tool: 'set_volume', reply: 'Imposto il volume al 50%.' }), 'unsupported');
  assert.equal(deviceToolOutcome({ action: 'tool', tool: 'set_brightness', reply: 'Alzo la luminosita.' }), 'unsupported');
  // ...while create_file/add_event were executed server-side
  assert.equal(deviceToolOutcome({ action: 'tool', tool: 'add_event', reply: 'Aggiunto "dentista" il 2026-09-24.' }), 'done');
  assert.equal(deviceToolOutcome({ action: 'tool', tool: 'add_event', reply: "Non sono riuscito ad aggiungere l'evento." }), 'failed');
  assert.equal(deviceToolOutcome({ action: 'tool', tool: 'create_file', arg: '/data/Note/a.txt', path: '/data/Note/a.txt', reply: 'Creo a.txt' }), 'done');
  assert.equal(deviceToolOutcome({ action: 'tool', tool: 'create_file', arg: '/data/Note/a.txt', reply: 'a.txt esiste gia: non lo sovrascrivo.' }), 'failed');
  assert.equal(deviceToolOutcome({ action: 'tool', tool: 'create_file', arg: '/data/Note/a.txt', reply: 'Per creare file devo essere associato (inserisci il PIN).' }), 'failed');
});

test('commandHint(): device commands and live state are caught, ordinary questions are not', () => {
  const act = ['alza il volume', 'volume al 50', 'metti la luminosità al massimo', 'fai più chiaro lo schermo', 'turn up the volume',
    'set brightness to 30', 'ricordami di chiamare Marco domani alle 10', 'aggiungi un evento dentista venerdì', 'remind me to call mum',
    'metti una sveglia alle 7', 'fammi un timer di 5 minuti'];
  const live = ['che ore sono', 'Che ora è?', 'what time is it', 'Anima, che ore sono adesso?', 'mi dici che ore sono per favore', 'che giorno è oggi', "what's the date", 'quanta batteria ho',
    'quanto spazio libero ho', 'quanta ram libera', 'a che rete sono connesso', 'che versione è', 'cosa ho oggi', 'my schedule'];
  const launch = ['apri la calcolatrice', 'open calculator', 'avvia il player'];
  const none = ['chi è Einstein', 'capitale della Francia', 'volume del cubo lato 3', 'come funziona il timer 555',
    'come funziona una batteria al litio', 'rete neurale', 'open source software is great', "l'audio del film era basso",
    'il volume di vendite è alto', 'crea un file note.txt', 'scrivi una funzione debounce in js', 'quanto fa 2+2', '',
    // look-alikes that belong to a brain, not the RTC / a device action (live state is matched whole-utterance)
    'che versione di python devo usare', "che giorno è natale quest'anno", 'come alzo il volume del mio pc?',
    'come creo un evento su google calendar?', 'start a new project in python'];
  for (const q of act) assert.equal(commandHint(q), 'act', q);
  for (const q of live) assert.equal(commandHint(q), 'live', q);
  for (const q of launch) assert.equal(commandHint(q), 'launch', q);
  for (const q of none) assert.equal(commandHint(q), null, q);
});

test('memoryHint(): teach / profile / recall go to the device (the one owner of personal memory)', () => {
  const teach = ['ricorda che la sala riunioni è al terzo piano', 'Ricordati che il mio colore preferito e il blu',
    'memorizza che il wifi di casa si chiama Pippo e la password è segreta', 'remember that my wife is Anna',
    'remember that the meeting room is on the third floor', 'tieni a mente che le chiavi sono nel cassetto'];
  const profile = ['mi chiamo Marco', 'Il mio nome è Giulia', 'my name is John', 'call me Ada', 'vivo a Brescia',
    'I live in London', 'ho 30 anni', "I'm 42 years old", 'lavoro come ingegnere', 'il mio compleanno è il 3 maggio'];
  const recall = ['come mi chiamo', 'come mi chiamo?', 'qual è il mio colore preferito', 'qual e il mio nome',
    "what's my name", 'what is my favourite colour', 'cosa sai di me', 'quanti anni ho', 'dove vivo', 'when is my birthday'];
  const none = ['ricordami di comprare il latte', 'remind me to call mum', 'ricorda che domani piove e fa freddo',
    'chi è Einstein', 'come si chiama il fiume più lungo', 'apri la calcolatrice', 'che ore sono', 'marco e luigi',
    'come vivo meglio?', ''];
  for (const q of teach) assert.equal(memoryHint(q), 'teach', q);
  for (const q of profile) assert.equal(memoryHint(q), 'profile', q);
  for (const q of recall) assert.equal(memoryHint(q), 'recall', q);
  for (const q of none) assert.equal(memoryHint(q), null, q);
});

test('actRequest(): a device action the browser decided, for /api/anima/act', () => {
  const vol = actRequest({ local: true, action: 'tool', tool: 'set_volume', arg: '30', reply: 'Volume al 30%.' }, 'it');
  assert.deepEqual(vol, { tool: 'set_volume', arg: '30', content: '', reply: 'Volume al 30%.', lang: 'it' });
  const note = actRequest({ local: true, action: 'tool', intent: 'create_file', arg: '/data/Documents/spesa.txt', content: 'latte, pane' }, 'en');
  assert.equal(note.tool, 'create_file', 'the tool falls back to the intent, like classifyCommand');
  assert.equal(note.content, 'latte, pane', 'the composed payload travels with it');
  assert.equal(note.lang, 'en');
  assert.equal(actRequest({ action: 'tool', tool: 'add_event', content: 'off=1;time=16:00;text=dentista' }, 'es').lang, 'it', 'the device speaks it/en');
  // not a device action: nothing to carry out on the Cardputer
  assert.equal(actRequest({ action: 'launch', arg: 'notepad' }, 'it'), null, 'a launch is the browser\'s own hand-off');
  assert.equal(actRequest({ action: 'tool', tool: 'open_file', arg: '/data/x.txt' }, 'it'), null);
  assert.equal(actRequest({ action: 'answer', reply: 'Parigi' }, 'it'), null);
  assert.equal(actRequest(null, 'it'), null);
});
