// Exact arithmetic from a sentence in the five OS languages (apps/anima/www/local/nlmath.js).
// Regression (ADV web-OS battery, 2026-10-10): with the device brain paused, maths reached the PC's 9B model, which put
// a WRONG result in bold ("**78**" for 17×23÷5 = 78.2). Pure arithmetic is now computed in the browser; anything with a
// non-arithmetic word stays with the normal ladder.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseMath, mathReply } from '../apps/anima/www/local/nlmath.js';

const val = (q, o) => { const r = parseMath(q, o); return r && r.value; };

test('five languages, operator words and symbols', () => {
  assert.equal(val('Quanto fa 45 per 54?', { lang: 'it' }), 2430);
  assert.equal(val('What is 12 times 11 plus 3?', { lang: 'en' }), 135);
  assert.equal(val('¿Cuánto es 45 por 54?', { lang: 'es' }), 2430);
  assert.equal(val('Combien font 17 fois 23, divisé ensuite par 5 ?', { lang: 'fr' }), 78.2);
  assert.equal(val('Was ist 144 geteilt durch 12?', { lang: 'de' }), 12);
  assert.equal(val('wie viel ist 7 mal 8', { lang: 'de' }), 56);
  assert.equal(val('3 + 4 × 2', { lang: 'it' }), 11, 'precedence');
  assert.equal(val('(3 + 4) × 2', { lang: 'it' }), 14);
  assert.equal(val('3 più 4, poi per 2', { lang: 'it' }), 14, 'a "then" step applies to the result so far');
  assert.equal(val('calcola 2 elevato a 10', { lang: 'it' }), 1024);
  assert.equal(val('12 al quadrato', { lang: 'it' }), 144);
  assert.equal(val('20% di 150', { lang: 'it' }), 30);
  assert.equal(val('100 diviso 8', { lang: 'it' }), 12.5);
  assert.equal(val('0,1 + 0,2', { lang: 'it' }), 0.3, 'no floating-point noise');
});

test('the user\'s number notation', () => {
  assert.equal(val('2.430 diviso 3', { lang: 'it' }), 810, 'dot thousands in Italian');
  assert.equal(val('1,000 times 3', { lang: 'en' }), 3000, 'comma thousands in English');
  assert.equal(val('2.5 times 4', { lang: 'en' }), 10);
  assert.equal(val('2,5 per 4', { lang: 'it' }), 10, 'decimal comma');
});

test('a follow-up continues from the previous result', () => {
  assert.equal(val('y dividido entre 3', { lang: 'es', prev: 2430 }), 810);
  assert.equal(val('e poi per 2', { lang: 'it', prev: 21 }), 42);
  assert.equal(val('and then minus 5', { lang: 'en', prev: 10 }), 5);
  assert.equal(val('al quadrato', { lang: 'it', prev: 9 }), 81);
  assert.equal(val('y dividido entre 3', { lang: 'es' }), null, 'no previous result: not a calculation');
});

test('anything that is not pure arithmetic stays with the ladder', () => {
  for (const q of ['Quanti chilometri sono 26,2 miglia?', 'Ho 3 scatole con 12 matite ciascuna. Quante matite ho?', 'chi ha vinto i mondiali del 2006',
    'apri le note', 'imposta il volume al 50', 'what is 5', '42', 'che ore sono', 'Napoleone è nato nel 1769', 'la luminosità al 40%', 'abre 3 ventanas',
    'tra 10 minuti', 'quanto fa', 'x', 'per', '1/0']) assert.equal(parseMath(q, { lang: 'it' }), null, q);
});

test('the reply shows the working and ONE bold result', () => {
  const r = parseMath('Combien font 17 fois 23, divisé ensuite par 5 ?', { lang: 'fr' });
  assert.equal(mathReply(r), '17 × 23 ÷ 5 = **78,2**');
  assert.equal(mathReply(parseMath('3 più 4, poi per 2', { lang: 'it' })), '(3 + 4) × 2 = **14**');
  assert.equal(mathReply(parseMath('¿Cuánto es 45 por 54?', { lang: 'es' })), '45 × 54 = **2430**');
  assert.equal(mathReply(parseMath('What is 1250 times 3?', { lang: 'en' })), '1,250 × 3 = **3,750**');
});
