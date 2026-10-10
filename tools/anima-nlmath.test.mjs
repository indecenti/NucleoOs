// Exact arithmetic from a sentence in the five OS languages (apps/anima/www/local/nlmath.js).
// Regression (ADV web-OS battery, 2026-10-10): with the device brain paused, maths reached the PC's 9B model, which put
// a WRONG result in bold ("**78**" for 17×23÷5 = 78.2). Pure arithmetic is now computed in the browser; anything with a
// non-arithmetic word stays with the normal ladder.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseMath, mathReply, parseConvert, parseCalendar } from '../apps/anima/www/local/nlmath.js';

// Calendar questions, five languages, from the clock (now = Saturday 10 Oct 2026, before the 25 Oct DST change).
test('calendar questions: weekday of a date, days until, the date in N days — DST-safe', () => {
  const now = new Date(2026, 9, 10);
  const cal = (q, lang) => { const r = parseCalendar(q, { lang, now }); return r && r.reply; };
  assert.equal(cal('Che giorno della settimana sarà il 25 dicembre 2026?', 'it'), 'Il 25 dicembre 2026 è **venerdì**.');
  assert.equal(cal('What day is December 25, 2026?', 'en'), '25 December 2026 is a **Friday**.');
  assert.equal(cal('¿Qué día de la semana es el 25 de diciembre de 2026?', 'es'), 'El 25 de diciembre de 2026 es **viernes**.');
  assert.equal(cal('Welcher Wochentag ist der 25. Dezember 2026?', 'de'), 'Der 25. Dezember 2026 ist ein **Freitag**.');
  assert.equal(cal('che giorno era il 4 luglio 1776', 'it'), 'Il 4 luglio 1776 era **giovedì**.');
  assert.equal(cal('Quanti giorni mancano a Natale?', 'it'), 'Mancano **76** giorni a 25 dicembre 2026.');
  assert.equal(cal('Combien de jours avant Noël ?', 'fr'), 'Il reste **76** jours avant le 25 décembre 2026.');
  assert.equal(cal('quanti giorni mancano al 1 gennaio', 'it'), 'Mancano **83** giorni a 1 gennaio 2027.');
  // across the 25 October DST change: calendar days, not N×24 h (gave 8 November / 17 January)
  assert.equal(cal('Che giorno sarà tra 30 giorni?', 'it'), 'Tra 30 giorni sarà **lunedì 9 novembre 2026**.');
  assert.equal(cal('Quel jour serons-nous dans 100 jours ?', 'fr'), 'Dans 100 jours, nous serons le **lundi 18 janvier 2027**.');
  assert.equal(cal('Welcher Tag ist in 3 Wochen?', 'de'), 'In 21 Tagen ist **Samstag, 31. Oktober 2026**.');
  for (const q of ['che giorno è oggi', 'quanti giorni ha febbraio', 'chi è nato il 25 dicembre', 'che giorno è il 31 febbraio 2027'])
    assert.equal(parseCalendar(q, { lang: 'it', now }), null, q);
});

// Unit conversions, five languages: "Quanti chilometri sono 26,2 miglia?" had reached a model.
test('unit conversions in five languages, exact, and nothing else', () => {
  const cv = (q, lang) => { const r = parseConvert(q, { lang }); return r && mathReply(r); };
  assert.equal(cv('Quanti chilometri sono 26,2 miglia?', 'it'), '26,2 mi = **42,16 km**');
  assert.equal(cv('convert 70 kg to pounds', 'en'), '70 kg = **154.3 lb**');
  assert.equal(cv('¿Cuántos grados Fahrenheit son 30 grados Celsius?', 'es'), '30°C = **86°F**');
  assert.equal(cv('combien de litres font 2 gallons', 'fr'), '2 gal = **7,57 l**');
  assert.equal(cv('Wie viele Meilen sind 10 km?', 'de'), '10 km = **6,21 mi**');
  assert.equal(cv('100 km/h in mph', 'it'), '100 km/h = **62,14 mph**');
  assert.equal(cv('32 °F in °C', 'en'), '32°F = **0°C**');
  assert.equal(cv('quanti chili sono 10 libbre', 'it'), '10 lb = **4,54 kg**');
  for (const q of ['5 km in kg', 'quanti anni ha Mario', 'what is 5 miles', 'converti 3 metri in metri', 'quanti gradi ci sono a Roma', 'apri le note'])
    assert.equal(parseConvert(q, { lang: 'it' }), null, q);
});

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
  assert.equal(val('Quanto fa il 20% di 150?', { lang: 'it' }), 30, 'a leading article');
  assert.equal(val('What is 20 percent of 150?', { lang: 'en' }), 30, 'spelled-out percent');
  assert.equal(val('calcola il 15 per cento di 80', { lang: 'it' }), 12);
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
