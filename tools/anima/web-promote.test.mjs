#!/usr/bin/env node
// Host test for web-promote.mjs — the AKB collision/dedup gate for browser-indexed knowledge.
// Pure, deterministic, no network. Run: node tools/anima/web-promote.test.mjs

import { normalizeCard, promoteWeb, mergeWeb, DUP, COLLIDE } from './web-promote.mjs';

let pass = 0, fail = 0; const fails = [];
function ok(name, cond, extra) { if (cond) pass++; else { fail++; fails.push(name + (extra ? '  ' + extra : '')); } }

const bil = (id, it, en, cat = 'concept') => ({ id, category: cat, reply: { it, en }, detail: { it: it + ' dettaglio.', en: en + ' detail.' }, ask: { it: [it.split(' ')[0]], en: [en.split(' ')[0]] }, source: 'wikipedia:it:X|en:X' });

// ---- normalizeCard ----
ok('norm: valid bilingual', !!normalizeCard(bil('web.roma', 'Roma è la capitale', 'Rome is the capital'), '2026-06-10'));
ok('norm: rejects monolingual', normalizeCard({ id: 'web.x', reply: { it: 'solo it' }, ask: { it: ['x'], en: ['x'] }, source: 'wikipedia:it:X' }) === null);
ok('norm: rejects no-source', normalizeCard({ id: 'web.x', reply: { it: 'a', en: 'b' }, ask: { it: ['x'], en: ['x'] } }) === null);
ok('norm: rejects bad-id', normalizeCard({ id: 'Web X!', reply: { it: 'a', en: 'b' }, ask: { it: ['x'], en: ['x'] }, source: 'wikipedia:it:X' }) === null);
{
  const c = normalizeCard(bil('web.roma', 'Roma è la capitale dItalia', 'Rome is the capital of Italy'), '2026-06-10');
  ok('norm: stamps date', c.last_updated === '2026-06-10');
  ok('norm: action answer', c.action === 'answer' && c.ttl_days === 3650);
  ok('norm: keeps detail', !!(c.detail && c.detail.it && c.detail.en));
}

// ---- promoteWeb: dedup + collision ----
{
  const corpus = [{ id: 'einstein', category: 'person', ask: { it: ['einstein'], en: ['einstein'] }, reply: { it: 'fisico', en: 'physicist' } }];
  // duplicate-id
  let r = promoteWeb([normalizeCard(bil('einstein', 'Einstein fisico', 'Einstein physicist', 'person'), 't')], corpus);
  ok('promote: duplicate-id rejected', r.promoted.length === 0 && r.rejected[0].reason === 'duplicate-id');
  // collision: same entity name, different id, different category -> recall hazard
  r = promoteWeb([normalizeCard(bil('web.einstein', 'Einstein einstein einstein', 'Einstein einstein einstein', 'work'), 't')], corpus);
  ok('promote: collision rejected', r.promoted.length === 0 && r.rejected[0].reason === 'collision', JSON.stringify(r.rejected[0]));
  // clean new entity -> promoted
  r = promoteWeb([normalizeCard(bil('web.newton', 'Newton matematico', 'Newton mathematician', 'person'), 't')], corpus);
  ok('promote: clean promoted', r.promoted.length === 1 && r.promoted[0].id === 'web.newton');
  // within-batch dedup: two near-identical cards, only first survives
  r = promoteWeb([
    normalizeCard(bil('web.darwin', 'Darwin naturalista evoluzione', 'Darwin naturalist evolution', 'person'), 't'),
    normalizeCard(bil('web.darwin2', 'Darwin naturalista evoluzione', 'Darwin naturalist evolution', 'person'), 't'),
  ], corpus);
  ok('promote: within-batch dedup', r.promoted.length === 1, 'p=' + r.promoted.length);
  // hard cap (distinct entities so nothing collides)
  const NAMES = [['Roma', 'Rome'], ['Parigi', 'Paris'], ['Berlino', 'Berlin'], ['Madrid', 'Madrid'], ['Vienna', 'Vienna']];
  const many = NAMES.map(([it, en], i) => normalizeCard({ id: 'web.c' + i, category: 'place', reply: { it: it + ' è una citta', en: en + ' is a city' }, ask: { it: [it], en: [en] }, source: 'wikipedia:it:' + it }, 't'));
  r = promoteWeb(many, [], { max: 3 });
  ok('promote: hard cap', r.promoted.length === 3 && r.rejected.filter(x => x.reason === 'over-cap').length === 2);
}

// ---- mergeWeb: idempotent by id ----
{
  const a = normalizeCard(bil('web.a', 'Alfa uno', 'Alpha one'), 't');
  const b = normalizeCard(bil('web.b', 'Beta due', 'Beta two'), 't');
  const t1 = mergeWeb('', [a, b]);
  const lines = t1.split('\n').filter(l => l && !l.startsWith('//'));
  ok('merge: two cards', lines.length === 2);
  const a2 = normalizeCard(bil('web.a', 'Alfa aggiornato', 'Alpha updated'), 't');
  const t2 = mergeWeb(t1, [a2]);
  const lines2 = t2.split('\n').filter(l => l && !l.startsWith('//'));
  ok('merge: idempotent by id', lines2.length === 2 && t2.includes('aggiornato') && !t2.includes('Alfa uno'));
}

ok('thresholds sane', DUP > COLLIDE && COLLIDE > 0.5);

console.log(`\n[web-promote] TOTAL: ${pass} ok, ${fail} fail`);
if (fails.length) { console.log('FAILURES:'); for (const f of fails) console.log('  ✗ ' + f); }
process.exitCode = fail ? 1 : 0;
