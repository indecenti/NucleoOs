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
const ART = /^(?:il|lo|la|l'|the|el|le|les|der|die|das|un|una|uno|a|an)\s+/;   // "quanto fa IL 20% di 150"
const PCT = /\s*\b(?:percento|per cento|percent|por ciento|pour cent|prozent)\b/g;   // "20 percent of 150"

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
  t = t.replace(PCT, '%').replace(/(\d)\s+%/g, '$1%');
  const steps = t.replace(OP_HEAD, '$1 ').replace(THEN, ' | ').replace(/,\s+/g, ' | ').split('|').map((x) => x.trim().replace(JOIN, '').replace(ART, '').trim()).filter(Boolean);
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

// ---- unit conversion, five languages ----------------------------------------------------------------
// "Quanti chilometri sono 26,2 miglia?", "convert 70 kg to pounds", "¿cuántos grados Fahrenheit son 30 grados?",
// "combien de litres font 2 gallons", "wie viele Meilen sind 10 km" — a number, a unit, a target unit of the same
// kind, and nothing else (the same whole-sentence discipline as parseMath). Factors to the SI base of each kind.
const U = [
  // [kind, symbol, factor to base (or 'temp'), aliases (folded)]
  ['len', 'km', 1000, ['km', 'chilometri', 'chilometro', 'kilometri', 'kilometers', 'kilometres', 'kilometer', 'kilometre', 'kilometros', 'kilometro']],
  ['len', 'm', 1, ['m', 'metri', 'metro', 'meters', 'metres', 'meter', 'metre', 'metros']],
  ['len', 'cm', 0.01, ['cm', 'centimetri', 'centimetro', 'centimeters', 'centimetres', 'centimeter', 'centimetros', 'zentimeter']],
  ['len', 'mm', 0.001, ['mm', 'millimetri', 'millimetro', 'millimeters', 'millimetres', 'milimetros', 'millimeter']],
  ['len', 'mi', 1609.344, ['mi', 'miglia', 'miglio', 'miles', 'mile', 'millas', 'milla', 'milles', 'meilen', 'meile']],
  ['len', 'ft', 0.3048, ['ft', 'piedi', 'piede', 'feet', 'foot', 'pies', 'pieds', 'pied', 'fuss']],
  ['len', 'in', 0.0254, ['pollici', 'pollice', 'inches', 'inch', 'pulgadas', 'pulgada', 'pouces', 'pouce', 'zoll']],
  ['len', 'yd', 0.9144, ['yd', 'iarde', 'iarda', 'yards', 'yard', 'yardas']],
  ['mass', 'kg', 1, ['kg', 'chili', 'chilo', 'chilogrammi', 'chilogrammo', 'kilogrammi', 'kilograms', 'kilogram', 'kilos', 'kilo', 'kilogramos', 'kilogrammes', 'kilogramm']],
  ['mass', 'g', 0.001, ['g', 'grammi', 'grammo', 'grams', 'gram', 'gramos', 'grammes', 'gramm']],
  ['mass', 'lb', 0.45359237, ['lb', 'lbs', 'libbre', 'libbra', 'pounds', 'pound', 'libras', 'libra', 'livres', 'livre', 'pfund']],
  ['mass', 'oz', 0.028349523125, ['oz', 'once', 'oncia', 'ounces', 'ounce', 'onzas', 'onza', 'onces', 'unzen', 'unze']],
  ['vol', 'l', 1, ['l', 'litri', 'litro', 'liters', 'litres', 'liter', 'litre', 'litros']],
  ['vol', 'ml', 0.001, ['ml', 'millilitri', 'millilitro', 'milliliters', 'millilitres', 'mililitros', 'milliliter']],
  ['vol', 'gal', 3.785411784, ['gal', 'galloni', 'gallone', 'gallons', 'gallon', 'galones', 'galon', 'gallonen']],
  ['speed', 'km/h', 1 / 3.6, ['km/h', 'kmh', 'chilometri orari', 'chilometri all ora', 'kilometers per hour', 'kilometros por hora', 'kilometres par heure', 'stundenkilometer']],
  ['speed', 'mph', 0.44704, ['mph', 'miglia orarie', 'miglia all ora', 'miles per hour', 'millas por hora']],
  ['speed', 'm/s', 1, ['m/s', 'metri al secondo', 'meters per second', 'metros por segundo', 'metres par seconde']],
  ['temp', '°C', 'temp', ['°c', 'c', 'celsius', 'gradi celsius', 'gradi centigradi', 'grados celsius', 'degres celsius', 'grad celsius', 'gradi', 'grados', 'degres', 'grad']],
  ['temp', '°F', 'temp', ['°f', 'f', 'fahrenheit', 'gradi fahrenheit', 'grados fahrenheit', 'degres fahrenheit', 'grad fahrenheit']],
  ['temp', 'K', 'temp', ['kelvin']],
];
const UNIT = new Map(); for (const [kind, sym, f, al] of U) for (const a of al) if (!UNIT.has(a)) UNIT.set(a, { kind, sym, f });
const UNIT_RE = [...UNIT.keys()].sort((a, b) => b.length - a.length).map((a) => a.replace(/[/°]/g, (c) => '\\' + c)).join('|');
const NUM_RE = String.raw`(-?\d[\d.,  ']*)`;
const CONV_LEAD = String.raw`(?:converti(?:mi)?|convert|convierte|convertis|konvertiere|rechne|quanto (?:fa|fanno|sono|e)|quant'e|how much is|what is|what's|cuanto (?:es|son)|combien (?:font|fait)|wie viel (?:sind|ist))?\s*`;
const TO = String.raw`(?:in|to|into|a|en|nach)`;
const HOW_MANY = String.raw`(?:quanti|quante|how many|cuantos|cuantas|combien de|combien d'|wie viele|wieviel|wie viel)`;
const CONV_A = new RegExp(String.raw`^${CONV_LEAD}${NUM_RE}\s*(${UNIT_RE})\s+${TO}\s+(${UNIT_RE})$`);
const CONV_B = new RegExp(String.raw`^${HOW_MANY}\s*(${UNIT_RE})\s+(?:sono|ci sono in|fa|fanno|are(?: there)? in|is|are|son|hay en|font|y a-t-il dans|sind|hat|ergeben|in|en)\s+${NUM_RE}\s*(${UNIT_RE})$`);
function toBase(v, u) { if (u.f !== 'temp') return v * u.f; return u.sym === '°C' ? v + 273.15 : u.sym === '°F' ? (v - 32) * 5 / 9 + 273.15 : v; }
function fromBase(v, u) { if (u.f !== 'temp') return v / u.f; return u.sym === '°C' ? v - 273.15 : u.sym === '°F' ? (v - 273.15) * 9 / 5 + 32 : v; }
export function parseConvert(q, { lang = 'it' } = {}) {
  const t = fold(q).replace(/[?!¿¡=]+/g, ' ').replace(/\s+/g, ' ').trim().replace(TAIL, '').trim();
  let m = CONV_A.exec(t), n, a, b;
  if (m) { n = m[1]; a = m[2]; b = m[3]; }
  else if ((m = CONV_B.exec(t))) { b = m[1]; n = m[2]; a = m[3]; }
  else return null;
  const ua = UNIT.get(a), ub = UNIT.get(b), v = num(n.replace(/[.,]$/, ''), lang);
  if (!ua || !ub || ua.kind !== ub.kind || ua.sym === ub.sym || !Number.isFinite(v)) return null;
  // bare "gradi/grados/degrés/Grad" is a temperature only next to an explicit °C/°F on the other side
  const out = Number(fromBase(toBase(v, ua), ub).toPrecision(10));
  const fmt = (x, d) => { try { return x.toLocaleString(LOCALE[lang] || 'en-GB', { maximumFractionDigits: d }); } catch { return String(x); } };
  const d = Math.abs(out) >= 100 ? 1 : 2;
  const sp = (s) => (s.startsWith('°') ? '' : ' ') + s;
  return { value: Number(out.toFixed(d)), shown: fmt(v, 6) + sp(ua.sym) + ' = ' + fmt(out, d) + sp(ub.sym) };
}

// The reply: the working, then the result in bold — the only number in bold, and the right one.
export function mathReply(res) {
  const i = res.shown.lastIndexOf(' = ');
  return res.shown.slice(0, i) + ' = **' + res.shown.slice(i + 3) + '**';
}
