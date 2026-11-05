#!/usr/bin/env node
// CODE-TASK gate: when ANIMA hands a request to Nucleo Code (docs/nucleo-code.md 4.6), when it asks first, and when
// it keeps the request. A wrong "yes" is the expensive mistake here (a reboot into Code for "fammi un timer di 5
// minuti"), so the negatives outnumber the positives, and every offer is checked in both directions.
//   code_task  -> "Code writes it" (an explicit program: un gioco, un'app, un programma, uno script)
//   code_offer -> "Code, or the chat?" (a classic by name, a tool word, the word codice/esempio)
//   code_run   -> "esegui snake.js"
//   (other)    -> not Code's
//   node tools/anima-host/code-task-check.mjs [--show]
import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const exe = join(here, 'build', 'anima.exe');
if (!existsSync(exe)) { console.error('anima.exe missing - run `npm run anima:build`.'); process.exit(2); }
const show = process.argv.includes('--show');

const TASK = [
  'scrivimi un gioco del serpente', 'fammi un gioco con le frecce', "fammi un'app per segnare le spese",
  'crea un gioco di carte', 'programma un gioco di dadi', 'sviluppa un videogioco spaziale', 'scrivi uno script che rinomina i file',
  'puoi scrivermi una app per il meteo', 'vorrei un gioco per due giocatori', 'costruiscimi un programma che conta le parole',
  'realizza una simulazione del sistema solare', "creami un'animazione di stelle", 'write a game with the arrow keys',
  'make an app that tracks my expenses', 'build a small game for the cardputer', 'scrivi un programma in javascript che ordina una lista',
];
const OFFER = [
  'fammi pong', 'scrivimi pong', 'voglio tetris', 'puoi programmare pong per il cardputer', 'fammi un sudoku', 'scrivimi tetris',
  'creami breakout', 'fammi una calcolatrice', 'scrivi il codice di pong', 'dammi un esempio di codice javascript',
  'scrivimi il codice di un gioco', 'fammi un timer', 'make me pong', 'write the code of snake',
];
const RUN = ['esegui snake.js', 'avvia pong.js', 'apri tetris.js', 'run snake.js'];
const NOT = [
  "che cos'e' un programma", 'come si scrive un gioco', 'cosa sai di pong', 'chi ha inventato tetris', 'quanto costa un videogioco',
  'scrivi un programma in python che ordina una lista', 'scrivimi uno script bash', 'dammi un esempio di codice python',
  'fammi un timer di 5 minuti', 'imposta un timer di 10 minuti', 'crea un promemoria domani alle 9', 'scrivi una poesia sulla neve',
  'scrivimi una storia su un gioco', 'scrivi una lettera per la mia app', 'apri la calcolatrice', 'apri il timer', 'gioca a pong',
  'calcola il codice fiscale di mario rossi', 'che tempo fa domani', 'fai una ricerca su internet', 'fammi ridere', 'apri le impostazioni',
  'raccontami una barzelletta', 'qual e il miglior gioco del 2024', 'fammi una domanda', 'scrivi una mail a luca',
  'cosa fa questa app', 'what is a program', 'how do I write a game', 'write a poem about the sea',
  'vorrei aprire la calcolatrice', 'quante righe di codice ha il mio cervello', 'mi puoi dire quante righe di codice ha linux',
  'dimmi un gioco da fare in famiglia', 'sai programmare?', 'voglio giocare a tetris', 'usa la calcolatrice', 'il codice di questo gioco e lento',
];
// two turns: the offer, then the answer
const FLOWS = [
  { name: 'offer, then yes = the job for Code', turns: [{ q: 'fammi pong', intent: 'code_offer' }, { q: 'si', intent: 'code_task' }] },
  { name: 'offer, then Enter-like "ok"', turns: [{ q: 'scrivimi tetris', intent: 'code_offer' }, { q: 'ok vai', intent: 'code_task' }] },
  { name: 'offer, then "qui" = an answer in the chat', turns: [{ q: 'scrivi il codice di pong', intent: 'code_offer' }, { q: 'qui', notIntent: /^code_/ }] },
  { name: 'offer, then "no" = the chat', turns: [{ q: 'fammi un sudoku', intent: 'code_offer' }, { q: 'no', notIntent: /^code_/ }] },
  { name: 'offer, then a new question drops it', turns: [{ q: 'fammi pong', intent: 'code_offer' }, { q: 'che ore sono', notIntent: /^code_/ },
                                                     { q: 'si', notIntent: /^code_/ }] },
  { name: 'a cold "si" is not a yes to an old offer', turns: [{ q: 'si', notIntent: /^code_/ }] },
];

function run(turns) {
  const lines = ['/reset', '/it', ...turns];
  const r = spawnSync(exe, [], { input: Buffer.from(lines.join('\n') + '\n', 'utf8'), maxBuffer: 64 << 20 });
  return r.stdout.toString('utf8').split(/^Q: /m).slice(1).map((b) => (b.match(/intent=(\S*)/) || [])[1] || '');
}

let fail = 0, total = 0;
const check = (ok, what) => { total++; if (!ok) { fail++; console.log('  x ' + what); } else if (show) console.log('  ok ' + what); };
for (const [list, want] of [[TASK, 'code_task'], [OFFER, 'code_offer'], [RUN, 'code_run']]) {
  const got = run(list);
  list.forEach((q, i) => check(got[i] === want, `"${q}" -> ${got[i] || '(none)'} (want ${want})`));
}
{
  const got = run(NOT);
  NOT.forEach((q, i) => check(!/^code_/.test(got[i] || ''), `"${q}" -> ${got[i] || '(none)'} (want not Code)`));
}
for (const f of FLOWS) {
  const got = run(f.turns.map((t) => t.q));
  f.turns.forEach((t, i) => {
    const g = got[i] || '';
    check(t.intent ? g === t.intent : !t.notIntent.test(g), `${f.name}: "${t.q}" -> ${g || '(none)'}`);
  });
}
console.log(`[code-task] ${total - fail}/${total} ${fail ? 'FAIL' : 'OK'}`);
process.exit(fail ? 1 : 0);
