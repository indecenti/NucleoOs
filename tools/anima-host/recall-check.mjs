#!/usr/bin/env node
// RECALL-CHECK — the network-free learned-card recall (firmware/components/nucleo_anima/
// nucleo_anima_recall.c) on the REAL cascade. It used to live in the online tier, which the host and
// the browser WASM don't build (they linked a stub that always missed), so what the device learned was
// invisible to the browser. Now the same code runs everywhere; this gate pins its behaviour:
//   * a paraphrase of a learned card is recalled (intent=recall), offline;
//   * an unrelated question never borrows a learned card;
//   * a generic word shared with a card's title (the "lago" of "Lago Quarzino") is not enough;
//   * naming the WHOLE title recalls even in the lower cosine band (RECALL_NAMED).
// The fixture entities are invented so no corpus/L1 card can answer first. Stateful: it writes a
// temporary ./sd/data/anima/learned/it.{jsonl,vec} and removes it (restoring any prior store) after.
//
//   node tools/anima-host/recall-check.mjs [--show]
import { spawnSync } from 'node:child_process';
import { existsSync, readFileSync, writeFileSync, rmSync, renameSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const exe = join(here, 'build', 'anima.exe');
const dir = join(here, 'sd', 'data', 'anima', 'learned');
if (!existsSync(exe)) { console.error('anima.exe missing — run `npm run anima:build`.'); process.exit(2); }
const show = process.argv.includes('--show');

const CARDS = [
  { id: 'wiki.it.zorblax-industries', reply: "Zorblax Industries e un'azienda immaginaria dei test di ANIMA.",
    ask: ['Zorblax Industries', 'zorblax', "cos'e zorblax industries", 'che cosa fa la zorblax'] },
  { id: 'wiki.it.lago-quarzino', reply: 'Il lago Quarzino e un lago immaginario dei test di ANIMA.',
    ask: ['Lago Quarzino', 'quarzino', 'dove si trova il lago quarzino'] },
];
const cases = [
  // names the whole title -> recalled even well below the vector-only band (measured ~0.70-0.74)
  { q: "cos'e zorblax industries",              want: { intent: 'recall', reply: /zorblax/i } },
  { q: 'dimmi cosa è la zorblax industries',    want: { intent: 'recall', reply: /zorblax/i } },
  { q: 'parlami di zorblax industries',         want: { intent: 'recall', reply: /zorblax/i } },
  { q: 'zorblax industries?',                   want: { intent: 'recall', reply: /zorblax/i } },
  { q: 'dove si trova il lago quarzino',        want: { intent: 'recall', reply: /quarzino/i } },
  { q: 'il lago quarzino dove si trova',        want: { intent: 'recall', reply: /quarzino/i } },
  { q: 'che lago è il quarzino',                want: { intent: 'recall', reply: /quarzino/i } },
  // unrelated, or only a generic/partial title word -> never borrow the card
  { q: 'dove si trova il lago di garda',        want: { notReply: /quarzino/i } },
  { q: 'il lago di como',                       want: { notReply: /quarzino/i } },
  { q: 'cosa sono le industrie',                want: { notReply: /zorblax/i } },
  { q: "cos'e la microsoft",                    want: { notReply: /zorblax/i } },
  { q: 'che cosa fa la ferrari',                want: { notReply: /zorblax/i } },
  { q: 'zorro',                                 want: { notReply: /zorblax/i } },
];

const files = ['it.jsonl', 'it.vec'];
const bak = (f) => join(dir, f + '.recallcheck.bak');
for (const f of files) if (existsSync(join(dir, f))) renameSync(join(dir, f), bak(f));
let fails = [];
try {
  writeFileSync(join(dir, 'it.jsonl'), CARDS.map((c) => JSON.stringify({
    id: c.id, category: 'test', action: 'answer', reply: { it: c.reply }, ask: { it: c.ask },
    source: 'test', last_updated: '2026-09-24', ttl_days: 3650 })).join('\n') + '\n');
  const lines = ['/it', '/learnvec it'];
  for (const c of cases) { lines.push('/reset'); lines.push(c.q); }
  const r = spawnSync(exe, [], { input: Buffer.from(lines.join('\n') + '\n', 'utf8'), maxBuffer: 64 * 1024 * 1024 });
  const out = r.stdout.toString('utf8');
  const learned = (r.stderr.toString('utf8').match(/learnvec it: (-?\d+)/) || [])[1];
  if (learned !== String(CARDS.length)) fails.push(['/learnvec', `wrote ${learned} vectors, expected ${CARDS.length}`]);
  const blocks = out.split(/^Q: /m).slice(1);
  cases.forEach((c, i) => {
    const b = blocks[i] || '';
    const p = { intent: (b.match(/intent=(\S*)/) || [])[1] || '', tier: (b.match(/tier=(\S+)/) || [])[1] || 'none',
                reply: ((b.match(/reply: (.*)/) || [])[1] || '').trim() };
    let why = '';
    if (c.want.intent && p.intent !== c.want.intent) why = `intent ${p.intent || '(none)'} != ${c.want.intent}`;
    if (c.want.reply && !c.want.reply.test(p.reply)) why = `reply "${p.reply.slice(0, 40)}" !~ ${c.want.reply}`;
    if (c.want.notReply && c.want.notReply.test(p.reply)) why = `borrowed a learned card: "${p.reply.slice(0, 40)}"`;
    if (why) fails.push([c.q, why]);
    if (show) console.log(`${why ? 'FAIL' : '  ok'}  "${c.q}" -> ${p.intent || '(none)'}/${p.tier} "${p.reply.slice(0, 50)}"`);
  });
} finally {
  for (const f of files) { try { rmSync(join(dir, f)); } catch { /* absent */ } if (existsSync(bak(f))) renameSync(bak(f), join(dir, f)); }
}

const n = cases.length + 1;
console.log(`[recall-check] learned-card recall — ${n - fails.length}/${n} pass`);
if (fails.length) { console.log('FAILURES:'); for (const f of fails) console.log(`  ✗ "${f[0]}" — ${f[1]}`); process.exit(1); }
console.log('✓ paraphrases of learned cards are recalled offline; unrelated or generic-word queries never borrow one');
