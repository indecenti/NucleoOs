// ANIMA offline translator — JS TWIN of firmware nucleo_anima_translate.c, for the browser simulator
// (tools/serve-shell.mjs) and host parity tests. Same grounded IT<->EN dictionary lookup: exact key
// match -> translation, miss -> honest decline, never a fabricated translation. Keys are normalized
// EXACTLY like the firmware a_tokenize() / gen_dicts.py, so the same dict-*.tsv drive device and sim.
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

// Italian accented vowels the device folds (a_tokenize switch); everything else non-ASCII is a separator.
const FOLD = { 'à':'a','á':'a','â':'a','è':'e','é':'e','ê':'e','ì':'i','í':'i','î':'i',
               'ò':'o','ó':'o','ô':'o','ù':'u','ú':'u','û':'u' };

// Port of a_tokenize(): fold IT vowels, keep lowercased ASCII alnum, split on everything else.
// Caps mirror the firmware (token <= 23 chars, <= 24 tokens).
export function tokenize(raw) {
  const toks = []; let cur = '';
  for (const ch of String(raw || '')) {
    let out = FOLD[ch];
    if (out === undefined && ch.charCodeAt(0) < 128 && /[0-9a-z]/i.test(ch)) out = ch.toLowerCase();
    if (out) { if (cur.length < 23) cur += out; }
    else if (cur) { toks.push(cur); cur = ''; if (toks.length >= 24) return toks; }
  }
  if (cur && toks.length < 24) toks.push(cur);
  return toks;
}

const starts = (s, p) => s.startsWith(p);
const isVerb = (w) => starts(w, 'traduc') || starts(w, 'tradur') || starts(w, 'translat');
const isNoun = (w) => starts(w, 'traduzion') || w === 'translation' || w === 'translations';
const langOf = (w) => ((starts(w, 'ingles') || w === 'english') ? 'e'
                     : (starts(w, 'italian') || w === 'italiano') ? 'i' : 0);
const BORDER = new Set(['di','del','dello','della','dell','la','il','lo','le','l','un','una','uno',
  'parola','parole','frase','word','words','phrase','the','a','an','mi','me','per','in','into','to',
  'verso','nel','nella','that','this']);
const FRAMES = [['come','si','dice'], ['come','si','dicono'], ['how','do','you','say'],
                ['how','do','i','say'], ['how','to','say']];

function findPhrase(tok, pat) {
  for (let s = 0; s + pat.length <= tok.length; s++) {
    let k = 0; for (; k < pat.length; k++) if (tok[s + k] !== pat[k]) break;
    if (k === pat.length) return s;
  }
  return -1;
}

// Cache the parsed dictionary per file (small TSVs, read once). Map: key -> readable translations.
const cache = new Map();
function loadDict(path) {
  if (cache.has(path)) return cache.get(path);
  const m = new Map();
  try {
    for (const line of readFileSync(path, 'utf8').split(/\r?\n/)) {
      const tab = line.indexOf('\t');
      if (tab < 0) continue;
      m.set(line.slice(0, tab), line.slice(tab + 1).trim());
    }
  } catch { /* missing dict -> empty (every lookup misses, skill declines honestly) */ }
  cache.set(path, m);
  return m;
}

// Detect-ONLY: is `raw` a translation request? (no dictionary lookup). Mirror of the firmware
// nucleo_anima_translate_is_request — lets serve-shell route translation to the online teacher (Grok) in
// hybrid/online mode, with the dictionary as the offline floor.
export function isTranslateRequest(raw) {
  const tok = tokenize(raw);
  if (tok.length < 1) return false;
  let verb = false, noun = false, lang = 0;
  for (const t of tok) {
    if (isVerb(t)) verb = true;
    else if (isNoun(t)) noun = true;
    const l = langOf(t); if (l && !lang) lang = l;
  }
  let phrase = false;
  for (const f of FRAMES) if (findPhrase(tok, f) >= 0) phrase = true;
  return !!(verb || phrase || (noun && lang));   // real boolean (mirror the C bool; `noun && lang` could yield 0)
}

// Detect + answer a translation request. Returns a result object (tier/action/intent/reply/...) when the
// input IS a translation request (hit, honest decline, or ask), or null to let the cascade continue.
export function translateAnswer(raw, en, dictDir) {
  const tok = tokenize(raw);
  if (tok.length < 1) return null;

  const excl = new Array(tok.length).fill(false);
  let verb = false, noun = false, lang = 0;
  for (let i = 0; i < tok.length; i++) {
    if (isVerb(tok[i])) { excl[i] = true; verb = true; continue; }
    if (isNoun(tok[i])) { excl[i] = true; noun = true; continue; }
    const l = langOf(tok[i]);
    if (l) { excl[i] = true; if (!lang) lang = l; }
  }
  let phrase = false;
  for (const f of FRAMES) { const at = findPhrase(tok, f); if (at >= 0) { phrase = true; for (let k = 0; k < f.length; k++) excl[at + k] = true; } }
  if (!(verb || phrase || (noun && lang))) return null;     // not a translation request

  // Longest run of non-excluded tokens, border-trimmed = the phrase to translate.
  let bs = -1, bl = 0, cs = -1, cl = 0;
  for (let i = 0; i <= tok.length; i++) {
    if (i < tok.length && !excl[i]) { if (cs < 0) { cs = i; cl = 1; } else cl++; }
    else { if (cl > bl) { bl = cl; bs = cs; } cs = -1; cl = 0; }
  }
  let s = bs, e = bs + bl;
  while (s < e && BORDER.has(tok[s])) s++;
  while (e > s && BORDER.has(tok[e - 1])) e--;

  const base = { tier: 'L0', action: 'answer', intent: 'translate', state: 'tool' };
  if (s >= e) {
    return { ...base, confidence: 60, trace: 'L0 translate',
      reply: en ? 'What should I translate? e.g. "translate dog to Italian".'
                : 'Cosa traduco? es. "traduci cane in inglese".' };
  }
  const key = tok.slice(s, e).join(' ');

  const itEn = join(dictDir, 'dict-it-en.tsv');
  const enIt = join(dictDir, 'dict-en-it.tsv');
  let val = null, toEn = false;
  if (lang === 'e') { val = loadDict(itEn).get(key) || null; toEn = true; }
  else if (lang === 'i') { val = loadDict(enIt).get(key) || null; toEn = false; }
  else {                                                    // auto: Italian source first, then English
    const v1 = loadDict(itEn).get(key);
    if (v1) { val = v1; toEn = true; } else { const v2 = loadDict(enIt).get(key); if (v2) { val = v2; toEn = false; } }
  }

  const ln = toEn ? (en ? 'English' : 'inglese') : (en ? 'Italian' : 'italiano');
  if (val) {
    return { ...base, confidence: 95, trace: `L0 translate · ${ln}`, reply: `"${key}" in ${ln}: ${val}.` };
  }
  return { ...base, confidence: 55, trace: 'L0 translate · miss',
    reply: en ? `I don't have "${key}" in the offline IT<->EN dictionary.`
              : `Non ho "${key}" nel dizionario offline IT<->EN.` };
}
