// Gate: ANIMA Forge — the SILENT LEARNING FLYWHEEL. A completed Forge turn becomes a STAGED
// knowledge card ONLY when CERTAIN (pass + approved + ran + provenance) and USEFUL (not a duplicate,
// not a collision, not empty). Deterministic gates only — no probabilistic acceptance. Staged cards
// carry their provenance hash and round-trip through JSONL.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { distill, stagePatch, trigramJaccard, slug, derivePhrasings, summarize } from '../../apps/anima/www/forge/learn.js';

const PROV = 'a'.repeat(64);

// A fully-certain, useful turn.
const goodTurn = (over = {}) => ({
  spec: 'scrivimi una funzione che inverte una stringa in javascript',
  code: 'function reverse(s){ return s.split("").reverse().join(""); }\nconsole.log(reverse("abc"));',
  verdict: { verdict: 'pass', reasons: [] },
  approved: true,
  ranOk: true,
  substrate: 'M4-local',
  provenanceHash: PROV,
  lang: 'it',
  ...over,
});
const emptyCtx = () => ({ existingCards: [], stagedCards: [] });

test('pass + approved + ran + provenance → STAGED card with provenance + bilingual asks', () => {
  const { staged, reason } = distill(goodTurn(), emptyCtx());
  assert.ok(staged, 'expected a staged card, reason=' + reason);
  assert.equal(reason, 'staged');
  assert.equal(staged.category, 'code-recipe');
  assert.equal(staged.source, 'forge-distill');
  assert.equal(staged.provenance, PROV, 'card must link to its provenance hash');
  assert.match(staged.id, /^[a-z0-9]+([._-][a-z0-9]+)*$/, 'id matches card-schema pattern');
  // bilingual asks present
  assert.ok(Array.isArray(staged.ask.it) && staged.ask.it.length >= 2, 'IT asks');
  assert.ok(Array.isArray(staged.ask.en) && staged.ask.en.length >= 2, 'EN asks');
  // reply is a frozen summary (both langs), detail is the VERBATIM code
  assert.ok(staged.reply.it && staged.reply.en);
  assert.match(staged.detail.it, /split\(""\)/, 'detail keeps verbatim code');
});

test('id is deterministic — same spec yields the same id', () => {
  const a = distill(goodTurn(), emptyCtx()).staged;
  const b = distill(goodTurn(), emptyCtx()).staged;
  assert.equal(a.id, b.id);
});

test('WARN verdict → null, not-certain:verdict-warn', () => {
  const r = distill(goodTurn({ verdict: { verdict: 'warn' } }), emptyCtx());
  assert.equal(r.staged, null);
  assert.match(r.reason, /not-certain:verdict-warn/);
});

test('VETO verdict → null, not-certain:verdict-veto', () => {
  const r = distill(goodTurn({ verdict: { verdict: 'veto' } }), emptyCtx());
  assert.equal(r.staged, null);
  assert.match(r.reason, /not-certain:verdict-veto/);
});

test('not approved → null, not-certain:not-approved', () => {
  const r = distill(goodTurn({ approved: false }), emptyCtx());
  assert.equal(r.staged, null);
  assert.match(r.reason, /not-approved/);
});

test('did not run → null, not-certain:not-run', () => {
  const r = distill(goodTurn({ ranOk: false }), emptyCtx());
  assert.equal(r.staged, null);
  assert.match(r.reason, /not-run/);
});

test('missing provenance → null, not-certain:no-provenance', () => {
  const r = distill(goodTurn({ provenanceHash: '' }), emptyCtx());
  assert.equal(r.staged, null);
  assert.match(r.reason, /no-provenance/);
});

test('empty spec (no content words) → null, not-useful:empty', () => {
  const r = distill(goodTurn({ spec: '   come faccio a   ' }), emptyCtx());
  assert.equal(r.staged, null);
  assert.match(r.reason, /not-useful:empty/);
});

test('duplicate of an existing same-topic card → rejected', () => {
  // First, learn the card; then feed it back as an existing card with the SAME id+asks.
  const first = distill(goodTurn(), emptyCtx()).staged;
  const ctx = { existingCards: [{ id: first.id, ask: first.ask, detail: first.detail }], stagedCards: [] };
  const r = distill(goodTurn(), ctx);
  assert.equal(r.staged, null);
  assert.equal(r.reason, 'duplicate');
});

test('collision — asks near-identical to a card about a DIFFERENT topic → rejected as collision', () => {
  // The candidate is a JS code recipe. A foreign card about quantum physics has (by accident or via
  // a copied phrasing) one ask textually IDENTICAL to a candidate ask. Same-topic is judged by the
  // foreign card's IDENTITY (id slug + reply), which is disjoint from the spec → COLLISION, not a
  // duplicate. This guards against the new card stealing retrieval from an unrelated card.
  const turn = goodTurn({ spec: 'read a file in javascript', code: 'const fs = 0;', lang: 'it' });
  const foreign = {
    id: 'physics.quantum-entanglement',
    reply: { it: "L'entanglement quantistico collega due particelle.", en: 'Quantum entanglement links two particles.' },
    ask: { it: ['read a file in javascript'], en: ['read a file in javascript'] }, // verbatim collide → sim=1
    detail: { it: 'x', en: 'x' },
  };
  const r = distill(turn, { existingCards: [foreign], stagedCards: [] });
  assert.equal(r.staged, null);
  assert.equal(r.reason, 'collision');
});

test('clean — high-similarity SAME-topic card is a duplicate, not a collision', () => {
  // Identity words overlap the spec (read/file/javascript) → same topic → DUPLICATE, never collision.
  const turn = goodTurn({ spec: 'read a file in javascript', code: 'const fs = 0;', lang: 'it' });
  const sameTopic = {
    id: 'code-recipe.read-a-file-javascript',
    reply: { it: 'Ricetta di codice (javascript): read a file.', en: 'Code recipe (javascript): read a file.' },
    ask: { it: ['come faccio read a file in javascript', 'read a file in javascript'], en: ['how do i read a file in javascript'] },
    detail: { it: 'x', en: 'x' },
  };
  const r = distill(turn, { existingCards: [sameTopic], stagedCards: [] });
  assert.equal(r.staged, null);
  assert.equal(r.reason, 'duplicate');
});

test('no false-positive — a DIFFERENT, dissimilar card does not block staging', () => {
  // The common "how do i … in javascript" frame must NOT trigger a collision against an unrelated
  // recipe — zero false positives (ANIMA never rejects useful knowledge by accident).
  const turn = goodTurn({ spec: 'read a file in javascript', code: 'const fs = 0;', lang: 'it' });
  const far = {
    id: 'geo.capital-france',
    reply: { it: 'Parigi', en: 'Paris' },
    ask: { it: ['qual è la capitale della francia'], en: ['capital of france'] },
    detail: { it: 'x', en: 'x' },
  };
  const r = distill(turn, { existingCards: [far], stagedCards: [] });
  assert.ok(r.staged, 'should stage despite an unrelated card present');
  assert.equal(r.reason, 'staged');
});

test('staged-pool dedup — a card already staged this session blocks a re-stage', () => {
  const first = distill(goodTurn(), emptyCtx()).staged;
  const r = distill(goodTurn(), { existingCards: [], stagedCards: [first] });
  assert.equal(r.staged, null);
  assert.equal(r.reason, 'duplicate');
});

test('stagePatch round-trips to valid JSON with all card fields', () => {
  const card = distill(goodTurn(), emptyCtx()).staged;
  const line = stagePatch(card);
  assert.equal(typeof line, 'string');
  assert.ok(!line.includes('\n'), 'one JSONL line, no embedded newline from the serializer wrapper');
  const back = JSON.parse(line);
  assert.deepEqual(back, card);
  assert.equal(back.provenance, PROV);
  assert.equal(back.category, 'code-recipe');
});

test('stagePatch rejects non-object input', () => {
  assert.throws(() => stagePatch(null));
  assert.throws(() => stagePatch('nope'));
});

// ---- pure helpers ----
test('trigramJaccard: identical=1, disjoint≈0, symmetric', () => {
  assert.equal(trigramJaccard('hello', 'hello'), 1);
  assert.ok(trigramJaccard('reverse a string', 'parse a json') < 0.5);
  assert.equal(trigramJaccard('abcdef', 'defabc'), trigramJaccard('defabc', 'abcdef'));
  assert.equal(trigramJaccard('', ''), 1);
  assert.equal(trigramJaccard('x', ''), 0);
});

test('slug: kebab, accent-stripped, deterministic, capped', () => {
  assert.equal(slug('Inverti una Stringà!'), 'inverti-una-stringa');
  assert.equal(slug(''), 'recipe');
  assert.match(slug('a'.repeat(200)), /^a{60}$/);
});

test('derivePhrasings: bilingual, language label inferred, deduped, ≤4', () => {
  const p = derivePhrasings('scrivimi una funzione che inverte una stringa in javascript', 'it');
  assert.ok(p.it.length >= 2 && p.it.length <= 4);
  assert.ok(p.en.length >= 2 && p.en.length <= 4);
  assert.ok(p.it.some((s) => /javascript/.test(s)));
  assert.ok(p.en.some((s) => /how do i/i.test(s)));
  // the language must not be doubled when the spec already ends in "... in javascript"
  for (const s of [...p.it, ...p.en]) assert.ok(!/in javascript in javascript/.test(s), 'no doubled lang: ' + s);
});

test('summarize: bilingual one-liners under the card reply cap (250)', () => {
  const s = summarize('write a function that reverses a string in javascript');
  assert.ok(s.it.length <= 250 && s.en.length <= 250);
  assert.match(s.en, /Code recipe/);
  assert.match(s.it, /Ricetta di codice/);
});
