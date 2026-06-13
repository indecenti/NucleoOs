// Host check for tools/anima/qgen.mjs — the question-generation quality gate. Pure, no network.
// Run: node tools/anima-host/qgen-check.mjs
import { normQ, jaccard, cosine, contentWords, relevant, isQuestion, acceptVariants, foreignIndex } from '../../tools/anima/qgen.mjs';

let pass = 0, fail = 0; const fails = [];
const ok = (n, c) => { if (c) pass++; else { fail++; fails.push(n); } };

// primitives
ok('normQ accents', normQ("Cos'è la Fotosintesi?") === 'cos e la fotosintesi');
ok('jaccard self=1', jaccard('photosynthesis', 'photosynthesis') === 1);
ok('jaccard diff<1', jaccard('what is photosynthesis', 'capital of france') < 0.3);
ok('cosine', Math.abs(cosine([1, 0], [1, 0]) - 1) < 1e-9 && Math.abs(cosine([1, 0], [0, 1])) < 1e-9);

// isQuestion: real asks vs asserted facts
ok('q: what is', isQuestion('what is photosynthesis'));
ok('q: cosa', isQuestion("cos'è la fotosintesi"));
ok('q reject fact', !isQuestion('Einstein was born in 1879'));     // year -> asserted fact
ok('q reject statement', !isQuestion('The capital of France is Paris.'));
ok('q reject long num', !isQuestion('temperature is 1234 degrees'));

// relevance (anti-hallucination): on-topic kept, off-topic dropped
const topic = 'Fotosintesi — processo con cui le piante convertono luce solare in energia chimica';
ok('relevant on-topic', relevant('come funziona la fotosintesi nelle piante', topic));
ok('relevant off-topic', !relevant('qual è la capitale della Francia', topic));

// acceptVariants: dedup + cue + relevance + cap
const cands = [
  'spiegami la fotosintesi',            // good
  'che cos\'è la fotosintesi',          // good
  'spiegami la fotosintesi',            // dup of #1
  'la fotosintesi avviene nelle foglie',// statement (no cue / fact) -> reject
  'come funziona la fotosintesi',       // good
  'qual è la capitale della Francia',   // off-topic -> reject (relevance)
];
const acc = acceptVariants(cands, { selfId: 'concept.fotosintesi', topicText: topic, existing: ['fotosintesi'], max: 8 });
ok('accept keeps good', acc.includes('spiegami la fotosintesi') && acc.includes('come funziona la fotosintesi'));
ok('accept dedups', acc.filter((x) => x === 'spiegami la fotosintesi').length === 1);
ok('accept drops statement', !acc.some((x) => /avviene nelle foglie/.test(x)));
ok('accept drops off-topic', !acc.some((x) => /capitale/.test(x)));

// collision across cards: a paraphrase that matches a DIFFERENT card's ask is rejected
const cards = [
  { id: 'concept.fotosintesi', ask: { it: ['cos\'è la fotosintesi', 'spiega la fotosintesi'] } },
  { id: 'concept.respirazione', ask: { it: ['cos\'è la respirazione cellulare', 'spiega la respirazione cellulare'] } },
];
const fidx = foreignIndex(cards, { lang: 'it' });
ok('foreignIndex size', fidx.length === 4 && fidx.every((f) => f.q && f.id));
// try to add to fotosintesi a near-clone of respirazione's ask -> must be rejected as collision
const acc2 = acceptVariants(['spiega la respirazione cellulare', 'come funziona la fotosintesi'], {
  selfId: 'concept.fotosintesi', topicText: topic + ' respirazione cellulare luce', existing: [], foreign: fidx, collideTh: 0.7,
});
ok('collision rejected', !acc2.some((x) => /respirazione/.test(x)));
ok('non-colliding kept', acc2.some((x) => /fotosintesi/.test(x)));

// vector sim path: a mock embedder makes two lexically-different strings "collide"
const vmap = new Map([
  ['fammi capire il processo verde delle piante', [1, 0, 0]],
  ['cos\'è la fotosintesi', [0.99, 0.01, 0]],
]);
const vsim = (a, b) => { const va = vmap.get(a), vb = vmap.get(b); return va && vb ? cosine(va, vb) : jaccard(a, b); };
const acc3 = acceptVariants(['fammi capire il processo verde delle piante'], {
  selfId: 'x', topicText: topic, existing: [], foreign: [{ q: "cos'è la fotosintesi", id: 'y' }],
  collideTh: 0.9, sim: vsim, requireCue: false, relMin: 0,
});
ok('vector collision rejects semantically-equal', acc3.length === 0);

console.log(`\nqgen-check: ${pass} passed, ${fail} failed (of ${pass + fail})`);
if (fails.length) { console.log('FAILURES:\n  ' + fails.join('\n  ')); process.exit(1); }
console.log('all green ✓');
