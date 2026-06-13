// Gate: the offline translator JS TWIN (translate.mjs) must match the C skill on the SAME eval cases —
// content + routing + zero false positives. This is the device/sim parity guard (like weather's twin).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { translateAnswer, tokenize, isTranslateRequest } from './translate.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const dictDir = join(here, '..', 'sd-sim', 'data', 'anima');
const cases = readFileSync(join(here, 'eval_translate.jsonl'), 'utf8')
  .split(/\r?\n/).filter((l) => l.trim() && !l.startsWith('//')).map((l) => JSON.parse(l));
const arr = (v) => (v == null ? [] : Array.isArray(v) ? v : [v]);

test('translate twin: content + routing + zero false positives (parity with the C skill)', () => {
  for (const it of cases) {
    const r = translateAnswer(it.q, it.lang === 'en', dictDir);
    if (it.not_translate) { assert.ok(!r || r.intent !== 'translate', `false positive: "${it.q}"`); continue; }
    assert.ok(r, `no answer: "${it.q}"`);
    assert.equal(r.intent, it.intent || 'translate', `"${it.q}": wrong intent`);
    for (const sub of arr(it.has)) assert.ok(r.reply.toLowerCase().includes(String(sub).toLowerCase()), `"${it.q}": missing "${sub}" in "${r.reply}"`);
    for (const sub of arr(it.nothas)) assert.ok(!r.reply.toLowerCase().includes(String(sub).toLowerCase()), `"${it.q}": contains "${sub}"`);
  }
});

// ── building-block properties (JS twin internals; these complement the content cases above) ─────────
test('tokenize: folds IT accents, lowercases, splits on non-alnum, keeps digits', () => {
  assert.deepEqual(tokenize('Perché città'), ['perche', 'citta']);   // è→e, à→a, lowercased
  assert.deepEqual(tokenize('CANE, gatto!'), ['cane', 'gatto']);     // caps + punctuation as separators
  assert.deepEqual(tokenize('traduci 42 cose'), ['traduci', '42', 'cose']);  // digits are kept
  assert.deepEqual(tokenize(''), []);
  assert.deepEqual(tokenize(null), []);
  assert.deepEqual(tokenize('  ...---  '), []);                      // punctuation-only → no tokens
});

test('tokenize: honours the firmware caps (≤24 tokens, ≤23 chars/token)', () => {
  const many = tokenize(Array.from({ length: 40 }, (_, i) => 'w' + i).join(' '));
  assert.ok(many.length <= 24, '≤24 tokens (firmware a_tokenize cap)');
  const long = tokenize('a'.repeat(50));
  assert.ok(long[0].length <= 23, '≤23 chars/token');
});

test('isTranslateRequest: detects requests, ignores look-alikes, never crashes', () => {
  assert.equal(isTranslateRequest('traduci cane in inglese'), true);
  assert.equal(isTranslateRequest('come si dice grazie in inglese'), true);
  assert.equal(isTranslateRequest('how do you say hello in italian'), true);
  assert.equal(isTranslateRequest('che ore sono'), false);
  assert.equal(isTranslateRequest('mi piace il calcio'), false);
  assert.equal(isTranslateRequest('traduttore inglese italiano'), false);  // a noun phrase, NOT a request
  assert.equal(isTranslateRequest('parlami della traduzione'), false);     // noun without a target language
  assert.equal(isTranslateRequest(''), false);
  assert.equal(isTranslateRequest(null), false);
});

test('translateAnswer: robust on degenerate input (no throw, honest routing)', () => {
  const noDict = '/__no_such_dict_dir__';   // missing dict → every lookup misses, but must never throw
  assert.equal(translateAnswer('', false, noDict), null);            // empty → not a request
  assert.equal(translateAnswer('!!! ...', false, noDict), null);     // punctuation-only → not a request
  assert.equal(translateAnswer(null, false, noDict), null);          // null → no crash
  const longReq = translateAnswer('traduci ' + 'parola '.repeat(60) + 'in inglese', false, noDict);
  assert.ok(longReq && longReq.intent === 'translate', 'a very long request still routes without crashing');
  const ask = translateAnswer('traduci', false, noDict);            // a verb but nothing to translate
  assert.ok(ask && /cosa traduco/i.test(ask.reply), 'asks what to translate');
});

test('translateAnswer: an unknown word is an HONEST decline, never a fabricated translation', () => {
  const r = translateAnswer('traduci zxqwvbn in inglese', false, dictDir);   // real dict, junk token
  assert.equal(r.intent, 'translate');
  assert.match(r.reply.toLowerCase(), /dizionario/);                 // says it doesn't have it
  assert.match(r.reply, /zxqwvbn/);                                  // echoes the extracted key (not invented)
});
