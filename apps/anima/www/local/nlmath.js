// nlmath.js — exact arithmetic from a sentence, in the browser, in the five OS languages.
//
// Why: in the web profile the Cardputer's offline brain (and its calculator) is paused, so "Combien font 17 fois 23,
// divisé ensuite par 5 ?" reached the PC's model, which wrote "**78**" in bold and 78.2 in the working; and "Ho 3
// scatole da 12…" came back as "**45**" with 53 further down (ADV web-OS battery, 2026-10-10). Arithmetic is not a
// language task: this module computes it exactly, instantly, with no model and no device round-trip.
//
// High precision by construction: the sentence is accepted ONLY when every word is a number, an operator word or a
// known filler ("quanto fa", "what is", "combien font"…). Any other word ("miglia", "matite", "Napoleone") → null and
// the normal ladder answers. A follow-up that starts with an operator ("y dividido entre 3", "and then times 2")
// continues from the previous result. No DOM — host-testable (tools/anima-nlmath.test.mjs).

const fold = (s) => String(s || '').toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/[‘’`]/g, "'");

// Operator words → symbol, longest first so "divided by" wins over "by" (which is not an operator on its own).
const OPS = [
  ['multiplicado por', '*'], ['multiplied by', '*'], ['multiplie par', '*'], ['multipliziert mit', '*'], ['moltiplicato per', '*'],
  ['dividido entre', '/'], ['dividido por', '/'], ['divided by', '/'], ['divise par', '/'], ['diviso per', '/'], ['geteilt durch', '/'],
  ['to the power of', '^'], ['elevato alla', '^'], ['elevato a', '^'], ['elevado a', '^'], ['puissance', '^'], ['hoch', '^'],
  ['diviso', '/'], ['fratto', '/'], ['over', '/'], ['dividido', '/'], ['entre', '/'], ['durch', '/'],
  ['per', '*'], ['volte', '*'], ['times', '*'], ['por', '*'], ['fois', '*'], ['mal', '*'], ['x', '*'],
  ['piu', '+'], ['plus', '+'], ['mas', '+'], ['add', '+'],
  ['meno', '-'], ['minus', '-'], ['menos', '-'], ['moins', '-'],
];
// "al quadrato" / "squared" / "au carré" / "al cuadrado" / "hoch 2" spelled as a postfix
const POSTFIX = [['al quadrato', '^2'], ['al cubo', '^3'], ['squared', '^2'], ['cubed', '^3'], ['al cuadrado', '^2'], ['al cubo', '^3'],
  ['au carre', '^2'], ['au cube', '^3'], ['zum quadrat', '^2'], ['quadrat', '^2'], ['hoch zwei', '^2'], ['hoch drei', '^3']];
// Fillers: question frames and connectors that carry no arithmetic. "then" words split the expression into steps.
const LEAD = /^(?:(?:ehi |hey |hola |salut |hallo )?anima,? )?(?:(?:mi )?(?:dici|sai dirmi|puoi dirmi) |dimmi |calcola(?:mi)? |quanto (?:fa|fanno|e|vale) |quant'e |what(?:'s| is) |how much is |calculate |compute |cuanto (?:es|son|da) |calcula |combien (?:font|fait|ca fait|cela fait) |calcule |was (?:ist|ergibt|macht) |wie ?viel (?:ist|sind|ergibt|macht) |rechne |berechne )*/;
const TAIL = /\s*(?:per favore|please|por favor|s'il te plait|s'il vous plait|bitte|grazie|thanks|gracias|merci|danke)?\s*$/;
// A "then" word or ", " ends a step ("3 più 4, poi per 2" = (3 + 4) × 2); one INSIDE an operator ("divisé ensuite par
// 5") is a filler. "26,2" / "1,000" (no space after the comma) are numbers, not steps.
const THEN = /\b(?:e poi|poi|quindi|and then|then|y luego|luego|despues|et puis|puis|ensuite|und dann|dann|danach)\b/g;
const OP_HEAD = /\b(divise|multiplie|diviso|moltiplicato|divided|multiplied|dividido|multiplicado|geteilt|multipliziert)\s+(?:e poi|poi|quindi|then|luego|despues|puis|ensuite|dann|danach)\s+/g;
const JOIN = /^(?:e|ed|and|y|et|und|also|anche)\s+/;

// One number in the user's notation. "2.430" / "1.000.000" are thousands in it/es/de/fr; "1,000" in English; a lone
// comma or a dot with not exactly three digits after it is a decimal point.
function num(tok, lang) {
  let t = tok.replace(/[\s  ']/g, '');
  if (!/^\d[\d.,]*$/.test(t)) return NaN;
  if (lang === 'en') { if (/^\d{1,3}(,\d{3})+(\.\d+)?$/.test(t)) t = t.replace(/,/g, ''); else t = t.replace(',', '.'); }
  else if (/^\d{1,3}(\.\d{3})+(,\d+)?$/.test(t)) t = t.replace(/\./g, '').replace(',', '.');
  else t = t.replace(',', '.');
  if ((t.match(/\./g) || []).length > 1) return NaN;
  return Number(t);
}

// Tokens of one step: numbers and operator symbols, or null when a word is not arithmetic.
function tokens(step, lang) {
  let s = ' ' + step.replace(/×/g, ' * ').replace(/÷/g, ' / ').replace(/:/g, ' / ').replace(/\*\*/g, ' ^ ') + ' ';
  for (const [w, sym] of POSTFIX) s = s.split(' ' + w + ' ').join(' ' + sym.replace('^', '^ ') + ' ');
  for (const [w, sym] of OPS) s = s.split(' ' + w + ' ').join(' ' + sym + ' ');
  s = s.replace(/(\d)\s*%\s*(?:di|of|de|del|von|du)\s+/g, '$1 % ');    // "20% di 150"
  s = s.replace(/([+\-*/^()%])/g, ' $1 ').replace(/\s+/g, ' ').trim();
  if (!s) return null;
  const out = [];
  for (const tk of s.split(' ')) {
    if (/^[+\-*/^()%]$/.test(tk)) out.push(tk);
    else { const v = num(tk, lang); if (!Number.isFinite(v)) return null; out.push(v); }
  }
  return out;
}

// Shunting-yard over + - * / ^ ( ) and "a % b" (a percent of b). → number | null
function evaluate(tk) {
  const prec = { '+': 1, '-': 1, '*': 2, '/': 2, '%': 2, '^': 3 }, right = { '^': true };
  const out = [], ops = []; let prevVal = false;
  for (let i = 0; i < tk.length; i++) {
    const t = tk[i];
    if (typeof t === 'number') { if (prevVal) return null; out.push(t); prevVal = true; continue; }
    if (t === '(') { if (prevVal) return null; ops.push(t); continue; }
    if (t === ')') { while (ops.length && ops[ops.length - 1] !== '(') out.push(ops.pop()); if (!ops.length) return null; ops.pop(); prevVal = true; continue; }
    if (!prevVal) { if (t === '-' && typeof tk[i + 1] === 'number') { tk[i + 1] = -tk[i + 1]; continue; } return null; }
    while (ops.length && ops[ops.length - 1] !== '(' && (prec[ops[ops.length - 1]] > prec[t] || (prec[ops[ops.length - 1]] === prec[t] && !right[t]))) out.push(ops.pop());
    ops.push(t); prevVal = false;
  }
  if (!prevVal) return null;
  while (ops.length) { const o = ops.pop(); if (o === '(') return null; out.push(o); }
  const st = [];
  for (const t of out) {
    if (typeof t === 'number') { st.push(t); continue; }
    const b = st.pop(), a = st.pop(); if (a === undefined || b === undefined) return null;
    if (t === '+') st.push(a + b); else if (t === '-') st.push(a - b); else if (t === '*') st.push(a * b);
    else if (t === '/') { if (b === 0) return null; st.push(a / b); }
    else if (t === '%') st.push(a * b / 100);
    else if (t === '^') { if (Math.abs(b) > 64) return null; st.push(Math.pow(a, b)); }
  }
  return st.length === 1 && Number.isFinite(st[0]) ? Number(st[0].toPrecision(12)) : null;
}

const SYM = { '*': '×', '/': '÷', '^': '^', '%': '% ×' };
const show = (tk, fmt) => tk.map((t) => typeof t === 'number' ? fmt(t) : (SYM[t] || t)).join(' ').replace(/\( /g, '(').replace(/ \)/g, ')').replace(/\^ /g, '^');

// parseMath(q, { lang, prev }) → { value, shown } | null.
//   lang: the language the numbers are written in (decides "2.430" vs "2.43"); prev: the previous result, for a
//   follow-up that starts with an operator.
export function parseMath(q, { lang = 'it', prev = null } = {}) {
  let t = fold(q).replace(/[?!¿¡=]+/g, ' ').replace(/\s+/g, ' ').trim();
  t = t.replace(LEAD, '').replace(TAIL, '').trim();
  if (!t || !/\d/.test(t) && prev == null) return null;
  const fmt = (v) => { try { return v.toLocaleString(LOCALE[lang] || 'en-GB', { maximumFractionDigits: 10 }); } catch { return String(v); } };
  const steps = t.replace(OP_HEAD, '$1 ').replace(THEN, ' | ').replace(/,\s+/g, ' | ').split('|').map((x) => x.trim().replace(JOIN, '').trim()).filter(Boolean);
  if (!steps.length) return null;
  let acc = null, shown = '', ops = 0;
  for (let i = 0; i < steps.length; i++) {
    let tk = tokens(steps[i], lang);
    if (!tk || !tk.length) return null;
    const startsWithOp = typeof tk[0] === 'string' && tk[0] !== '(' && !(tk[0] === '-' && typeof tk[1] === 'number' && i === 0 && acc == null && prev == null);
    if (startsWithOp) {
      const base = acc != null ? acc : (i === 0 ? prev : null);
      if (base == null) return null;
      if (tk.length === 1 && tk[0] !== '^') return null;
      tk = [base, ...tk];
      shown = acc != null ? (/[+\-]/.test(shown.replace(/^-/, '')) && /[×÷^%]/.test(SYM[tk[1]] || tk[1]) ? '(' + shown + ')' : shown) + ' ' + show(tk.slice(1), fmt) : show(tk, fmt);
    } else {
      if (acc != null) return null;                     // a second step must continue the first
      shown = show(tk, fmt);
    }
    ops += tk.filter((x) => typeof x === 'string' && x !== '(' && x !== ')').length;
    const v = evaluate(tk.slice());
    if (v == null) return null;
    acc = v;
  }
  if (!ops) return null;                                 // a lone number is not a calculation
  return { value: acc, shown: shown + ' = ' + fmt(acc) };
}
const LOCALE = { it: 'it-IT', en: 'en-GB', es: 'es-ES', fr: 'fr-FR', de: 'de-DE' };

// The reply: the working, then the result in bold — the only number in bold, and the right one.
export function mathReply(res) {
  const i = res.shown.lastIndexOf(' = ');
  return res.shown.slice(0, i) + ' = **' + res.shown.slice(i + 3) + '**';
}
