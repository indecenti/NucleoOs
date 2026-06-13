#!/usr/bin/env node
// ANIMA Agent Loop — correctness gate for the deterministic micro-agent.
//
// Drives the REAL compiled cascade (anima.exe) over the agent-loop cases below and asserts the
// multi-step behaviors that make ANIMA "Claude-Code-like": compose-then-act (compute/literal a
// payload THEN propose a write), the content channel, the device-settings tools, the naming guard,
// and the visible reasoning trace. Pure offline — no network, no flash.
//
//   npm run anima:build            # make sure anima.exe is current
//   node tools/anima-host/agent-check.mjs
//
// Exit != 0 on any wrong intent / arg / content / trace, so the agent loop can't silently regress.
import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const exe = join(here, 'build', 'anima.exe');
if (!existsSync(exe)) { console.error('anima.exe missing — run `npm run anima:build` first.'); process.exit(2); }

// Each case asserts the fields it cares about. Strings are substring matches (case-insensitive);
// `trace` is an array of fragments that must all appear; `content` of "" means none expected.
const CASES = [
  // --- compose-then-act: COMPUTE the payload, then write it -------------------
  { q: 'crea una nota con 12*8',                       intent: 'create_file', arg: '/data/Documents/nota.txt', content: '96',
    trace: ['piano', 'calcolo 12*8=96', 'verifica'] },
  { q: 'crea un file conti.txt con 100-37',            intent: 'create_file', arg: 'conti.txt', content: '63',
    trace: ['calcolo', '=63'] },
  // --- compose-then-act: LITERAL payload via several connectors ---------------
  { q: 'crea un file spesa.txt con scritto latte e pane', intent: 'create_file', arg: 'spesa.txt', content: 'latte e pane' },
  { q: 'crea un file todo.txt con il testo comprare il latte', intent: 'create_file', arg: 'todo.txt', content: 'comprare il latte' },
  { q: 'crea una nota: comprare il latte',             intent: 'create_file', content: 'comprare il latte' },
  { q: 'crea una nota con la lista della spesa',       intent: 'create_file', content: 'la lista della spesa' },
  { q: 'make a note saying hello world', lang: 'en',   intent: 'create_file', content: 'hello world' },
  { q: 'create a file todo.txt with the text buy milk', lang: 'en', intent: 'create_file', arg: 'todo.txt', content: 'buy milk' },
  // --- compose-FROM-KNOWLEDGE: ANIMA answers from its own brain and writes the answer ----------
  { q: 'crea una nota con cos\'e una variabile',       intent: 'create_file', content: 'contenitore', trace: ['cerco'] },
  { q: 'crea un file def.txt con la definizione di variabile', intent: 'create_file', arg: 'def.txt', content: 'contenitore' },
  { q: 'crea una nota con la capitale della francia',  intent: 'create_file', content: 'Parigi' },
  // ANTI-HALLUCINATION: an UNKNOWN entity must NOT fabricate a fact into the file — the self-verify
  // detects uncertainty and writes the LITERAL clause instead (never a made-up capital for "Floonkia").
  { q: 'crea un file con la capitale di Floonkia',     intent: 'create_file', content: 'Floonkia', trace: ['letterale'] },
  // explicit "scritto" stays LITERAL — the question text is copied verbatim, NOT answered
  { q: 'crea una nota con scritto cos\'e una variabile', intent: 'create_file', content: 'cos\'e una variabile' },
  // zero-false-write guard: literal text that merely CONTAINS a question word later stays literal
  { q: 'crea una nota con la lista delle cose da fare', intent: 'create_file', content: 'la lista delle cose da fare' },
  // --- naming guard: "con nome X" is a filename, NOT content ------------------
  { q: 'crea un file con nome pippo',                  intent: 'create_file', arg: 'pippo.txt', content: '' },
  // --- legacy empty-file create (no content clause) stays as-is ---------------
  { q: 'crea un file note.txt',                        intent: 'create_file', arg: 'note.txt', content: '' },
  // --- device-settings tool: absolute + relative -----------------------------
  { q: 'imposta il volume a 55',                       intent: 'set_volume', arg: '55' },
  { q: 'alza il volume a 70',                          intent: 'set_volume', arg: '70' },
  { q: 'abbassa il volume',                            intent: 'set_volume', arg: '-10' },
  { q: 'imposta la luminosita a 30',                   intent: 'set_brightness', arg: '30' },
  { q: 'raise the volume', lang: 'en',                 intent: 'set_volume', arg: '+10' },
  // widened NLU (natural-language robustness): spelled amounts, verbless direction, colloquial verbs
  { q: 'porta il volume a zero',                       intent: 'set_volume', arg: '0' },
  { q: 'metti il volume al massimo',                   intent: 'set_volume', arg: '100' },
  { q: 'volume piu alto',                              intent: 'set_volume', arg: '+10' },
  { q: 'volume piu basso',                             intent: 'set_volume', arg: '-10' },
  { q: 'turn the volume up', lang: 'en',               intent: 'set_volume', arg: '+10' },
  { q: 'fai piu chiaro',                               intent: 'set_brightness', arg: '+10' },
  { q: 'metti buio',                                   intent: 'set_brightness', arg: '-10' },
  { q: 'imposta la luminosita a meta',                 intent: 'set_brightness', arg: '50' },
  // --- add_event (calendar reminder): WHEN + TIME + TEXT parsing -------------
  { q: 'ricordami di chiamare mamma domani alle 15',   intent: 'add_event', content: 'off=1;time=15:00;text=chiamare mamma',
    trace: ['pianifica', 'domani', 'alle 15:00'] },
  { q: 'aggiungi un evento dentista dopodomani alle 9:30', intent: 'add_event', content: 'off=2;time=09:30;text=dentista' },
  { q: 'ricordami di comprare il latte',               intent: 'add_event', content: 'off=0;time=;text=comprare il latte' },
  { q: 'promemoria riunione tra 3 giorni alle 14',     intent: 'add_event', content: 'off=3;time=14:00;text=riunione' },
  { q: 'remind me to call john tomorrow at 9', lang: 'en', intent: 'add_event', content: 'off=1;time=09:00;text=call john' },
  // --- false-positive guards: a how-to QUESTION is knowledge, not a tool ------
  { q: 'ricordami come si crea un file',               intent: 'l1' },     // not add_event, not create_file
  { q: 'come si crea un file',                          intent: 'l1' },     // not create_file
  { q: 'posso creare un file gia scritto',             notIntent: 'create_file' },   // capability question, not a create command
  { q: 'posso aggiungere un evento',                   notIntent: 'add_event' },      // capability question, not a schedule command
  // --- widened NLU: play verbs launch the right app; new connectors ----------
  { q: 'metti la musica',                              intent: 'open_app', arg: 'media-player' },
  { q: 'metti la radio',                               intent: 'open_app', arg: 'radio' },
  { q: 'crea una nota dicendo ciao',                   intent: 'create_file', content: 'ciao' },
  { q: 'annota un file lista.txt con il testo pane',   intent: 'create_file', arg: 'lista.txt', content: 'pane' },
  // --- the trace exists even for a single-tier answer (synthesized) -----------
  { q: 'numeri primi',                                 intent: 'l1', trace: ['L1', 'knowledge'] },
  // --- MOSAICO (L2 span-stitch): a describe query is enriched with grounded spans; tier becomes L2 -
  { q: 'cos\'e nucleoos',                              intent: 'mosaico', trace: ['stitch'] },
  { q: '12*8',                                         intent: 'calc', trace: ['calc'] },
];

// Drive the firmware in one REPL stream (/reset + /it|/en per case, like math-check/route-check).
let lang = 'it';
const lines = [];
for (const c of CASES) {
  lines.push('/reset');
  const want = c.lang === 'en' ? 'en' : 'it';
  if (want !== lang) { lines.push('/' + want); lang = want; }
  lines.push(c.q);
}
const run = spawnSync(exe, [], { input: Buffer.from(lines.join('\n') + '\n', 'utf8'), maxBuffer: 32 * 1024 * 1024 });
const blocks = run.stdout.toString('utf8').split(/^Q: /m).slice(1);
const rows = blocks.map(b => ({
  intent:  (b.match(/intent=(\S*)/) || [])[1] || '',
  arg:     (b.match(/^\s*arg=(.*)$/m) || [])[1]?.trim() || '',
  content: (b.match(/^\s*content: (.*)$/m) || [])[1]?.trim() || '',
  trace:   (b.match(/^\s*trace: (.*)$/m) || [])[1]?.trim() || '',
  reply:   (b.match(/reply: (.*)/) || [])[1]?.trim() || '',
}));

const ci = s => String(s).toLowerCase();
const fails = [];
for (let i = 0; i < CASES.length; i++) {
  const c = CASES[i], r = rows[i] || {};
  const errs = [];
  if (c.intent && r.intent !== c.intent) errs.push(`intent=${r.intent} want ${c.intent}`);
  if (c.notIntent && r.intent === c.notIntent) errs.push(`intent=${r.intent} must NOT be ${c.notIntent}`);
  if (c.arg && !ci(r.arg).includes(ci(c.arg))) errs.push(`arg="${r.arg}" want ~"${c.arg}"`);
  if (c.content !== undefined) {
    if (c.content === '' && r.content !== '') errs.push(`content="${r.content}" want NONE`);
    if (c.content !== '' && !ci(r.content).includes(ci(c.content))) errs.push(`content="${r.content}" want ~"${c.content}"`);
  }
  if (c.trace) for (const frag of c.trace) if (!ci(r.trace).includes(ci(frag))) errs.push(`trace="${r.trace}" missing "${frag}"`);
  if (errs.length) fails.push({ q: c.q, errs });
}

const pass = CASES.length - fails.length;
console.log(`[agent] ${pass}/${CASES.length} agent-loop cases pass`);
if (fails.length) {
  console.log('\nFAILURES:');
  for (const f of fails) { console.log(`  ✗ "${f.q}"`); for (const e of f.errs) console.log(`      - ${e}`); }
  process.exit(1);
}
console.log('ALL GREEN — compose-then-act, content channel, settings, naming guard, and trace all correct.');
