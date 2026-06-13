// learn.js — the SILENT LEARNING FLYWHEEL: distil a COMPLETED Forge interaction into a STAGED
// knowledge candidate for the existing offline corpus, but ONLY when it is CERTAIN and USEFUL.
// This is how the local generative substrate (M4) silently teaches the deterministic grounded
// brain (M1): a code recipe that PASSED grounded verification, was HUMAN-approved, and actually
// RAN becomes a frozen `code-recipe` card the device serves OFFLINE forever after — with zero
// hallucination by construction (the reply is a summary, the detail is the VERBATIM code that ran).
//
// Two deterministic gates, no probabilistic acceptance:
//   CERTAINTY — verdict==='pass' (NOT warn/veto), approved, ranOk, and a provenanceHash. Anything
//               less never becomes knowledge → { staged:null, reason:'not-certain:<why>' }.
//   USEFULNESS — reject near-duplicates of an existing/staged card (trigram-Jaccard of asks ≥ 0.8),
//               cross-card COLLISIONS (asks too similar to a card about a DIFFERENT topic), and
//               empty specs with no real content words.
// Every staged card carries its provenance hash, so learning is auditable and reversible (drop the
// JSONL line; the hash links back to the ledger record). Pure & DOM-free → host-testable.

const STOP = new Set([
  // IT
  'come', 'faccio', 'fare', 'fai', 'un', 'una', 'uno', 'il', 'lo', 'la', 'le', 'gli', 'i', 'di', 'da',
  'in', 'con', 'per', 'che', 'cosa', 'mi', 'puoi', 'voglio', 'vorrei', 'dammi', 'scrivi', 'scrivimi',
  'crea', 'creami', 'genera', 'generami', 'fammi', 'mostrami', 'funzione', 'codice', 'script', 'esempio',
  // EN
  'how', 'do', 'i', 'to', 'a', 'an', 'the', 'of', 'in', 'with', 'for', 'that', 'what', 'me', 'can', 'you',
  'want', 'give', 'write', 'create', 'make', 'generate', 'show', 'function', 'code', 'script', 'example', 'please',
]);

const LANGS = /\b(javascript|js|typescript|ts|jsx|tsx|react|node(?:js)?|html|css|python|py|c\+\+|cpp|c#|csharp|java|rust|go|bash|shell|sql)\b/i;

// Canonicalise a programming-language token to its full name.
function canonLang(t) {
  t = String(t || '').toLowerCase();
  if (t === 'js' || t === 'node' || t === 'nodejs') return 'javascript';
  if (t === 'ts') return 'typescript';
  if (t === 'py') return 'python';
  return t;
}

// Pick the PROGRAMMING-language label from the spec text. `hint` is the turn's natural language
// (it/en) and is only honoured if it itself names a code language — a spoken-language code never
// becomes the recipe label. Defaults to 'javascript'.
function langLabel(spec, hint) {
  const m = LANGS.exec(String(spec || ''));
  if (m) return canonLang(m[0]);
  if (hint && LANGS.test(hint)) return canonLang(hint);
  return 'javascript';
}

// kebab slug from arbitrary text — deterministic, derives ONLY from its argument.
export function slug(text) {
  return String(text || '')
    .toLowerCase()
    .normalize('NFKD').replace(/[̀-ͯ]/g, '')   // strip accents
    .replace(/[^a-z0-9]+/g, '-')
    .replace(/^-+|-+$/g, '')
    .replace(/-{2,}/g, '-')
    .slice(0, 60) || 'recipe';
}

// Content words of a spec (lowercased, stop-words and pure-punctuation dropped).
function contentWords(text) {
  return String(text || '')
    .toLowerCase()
    .normalize('NFKD').replace(/[̀-ͯ]/g, '')
    .split(/[^a-z0-9+#]+/)
    .filter((w) => w && w.length > 1 && !STOP.has(w));
}

// Character trigrams of a normalised string (whitespace collapsed).
function trigrams(text) {
  const s = String(text || '').toLowerCase().replace(/\s+/g, ' ').trim();
  const out = new Set();
  if (s.length < 3) { if (s) out.add(s); return out; }
  for (let i = 0; i <= s.length - 3; i++) out.add(s.slice(i, i + 3));
  return out;
}

// Jaccard over character trigrams of two strings (0..1). Deterministic, symmetric.
export function trigramJaccard(a, b) {
  const A = trigrams(a), B = trigrams(b);
  if (!A.size && !B.size) return 1;
  if (!A.size || !B.size) return 0;
  let inter = 0;
  for (const g of A) if (B.has(g)) inter++;
  return inter / (A.size + B.size - inter);
}

// Best trigram-Jaccard between any phrasing in `asks` and any phrasing in `otherAsks`.
function maxAskSimilarity(asks, otherAsks) {
  let best = 0;
  for (const x of asks) for (const y of otherAsks) {
    const s = trigramJaccard(x, y);
    if (s > best) best = s;
    if (best >= 1) return best;
  }
  return best;
}

// Flatten a card's bilingual ask object (or array) into one phrasing list.
function askPhrasings(card) {
  const ask = card && card.ask;
  if (Array.isArray(ask)) return ask.filter((s) => typeof s === 'string');
  if (ask && typeof ask === 'object') return [...(ask.it || []), ...(ask.en || [])].filter((s) => typeof s === 'string');
  return [];
}

// Leading filler tokens (generation verbs, articles, the generic code-nouns and relative pronouns)
// that carry no topic signal. Stripped iteratively so a chain like "scrivimi una funzione che …"
// peels down to the real topic "…".
const LEADIN = /^\s*(?:please|scrivi(?:mi)?|scriva|crea(?:mi)?|genera(?:mi)?|generami|fammi|mostrami|implementa(?:mi)?|write|create|make|generate|build|implement|show|un[ao']?|una|a|an|the|me|mi|funzione|function|codice|code|script|programma|program|snippet|che|that|to)\b\s*/i;

// Strip leading generation lead-ins AND a trailing language mention, so the bare TOPIC reads
// naturally inside "come faccio … in <lang>" without doubling the language ("… in js in js").
function bareTopic(spec) {
  let s = String(spec || '');
  for (let prev = null; prev !== s; ) { prev = s; s = s.replace(LEADIN, ''); }   // peel chained fillers
  return s
    .replace(new RegExp('\\s+(?:in|with|using|con)\\s+' + LANGS.source + '\\s*$', 'i'), '')
    .replace(/\s+/g, ' ')
    .trim();
}

// Bilingual request phrasings derived from the spec — deterministic, no model call. The "topic" is
// the spec with leading generation verbs/lead-ins stripped; we frame it as how-to questions.
export function derivePhrasings(spec, lang) {
  const label = langLabel(spec, lang);
  const topic = bareTopic(spec);
  const t = topic || String(spec || '').trim();
  const it = [
    `come faccio ${t} in ${label}`,
    `${t} in ${label}`,
    `scrivimi ${t} in ${label}`,
  ];
  const en = [
    `how do i ${t} in ${label}`,
    `${t} in ${label}`,
    `write ${t} in ${label}`,
  ];
  // de-dup while preserving order; cap at 4 per language
  const uniq = (arr) => [...new Set(arr.map((s) => s.replace(/\s+/g, ' ').trim()).filter(Boolean))].slice(0, 4);
  return { it: uniq(it), en: uniq(en) };
}

// One-line bilingual summary of the spec (the FROZEN reply). Never the generative prose — just a
// faithful restatement of the request, so the reply makes no factual claim the code didn't ground.
export function summarize(spec) {
  const label = langLabel(spec, null);
  const topic = bareTopic(spec) || String(spec || '').trim();
  return {
    it: `Ricetta di codice (${label}): ${topic}.`.slice(0, 250),
    en: `Code recipe (${label}): ${topic}.`.slice(0, 250),
  };
}

const DUP_THRESHOLD = 0.8;       // trigram-Jaccard of asks ≥ this against a SAME-topic card → duplicate
const COLLISION_THRESHOLD = 0.8; // asks this close to a DIFFERENT-topic card → collision (ambiguous retrieval)

// distill(turn, ctx) → { staged: card|null, reason }
// turn = { spec, code, verdict (combineVerdict obj), approved, ranOk, substrate, provenanceHash, lang }
// ctx  = { existingCards:[{id,ask,detail}], stagedCards:[] }
export function distill(turn = {}, ctx = {}) {
  const { spec, code, verdict, approved, ranOk, substrate, provenanceHash, lang } = turn;

  // ---- CERTAINTY GATE (only certain) ----
  const v = verdict && verdict.verdict;
  if (v !== 'pass') return { staged: null, reason: 'not-certain:verdict-' + (v || 'none') };
  if (approved !== true) return { staged: null, reason: 'not-certain:not-approved' };
  if (ranOk !== true) return { staged: null, reason: 'not-certain:not-run' };
  if (!provenanceHash || typeof provenanceHash !== 'string') return { staged: null, reason: 'not-certain:no-provenance' };

  // ---- USEFULNESS GATE (only useful) ----
  const words = contentWords(spec);
  if (!words.length) return { staged: null, reason: 'not-useful:empty' };
  const verbatim = String(code || '');
  if (!verbatim.trim()) return { staged: null, reason: 'not-useful:no-code' };

  const langPrimary = (lang === 'en' || lang === 'it') ? lang : 'bi';
  const label = langLabel(spec, lang);
  // slug derives ONLY from the spec text → same request always yields the same id (idempotent
  // staging). Append the language label only when the spec didn't already name it (no doubling).
  const idWords = words.includes(label) ? words : [...words, label];
  const id = 'code-recipe.' + slug(idWords.join(' '));
  const phrasings = derivePhrasings(spec, lang);
  const candidate = {
    id,
    category: 'code-recipe',
    action: 'answer',
    arg: '',
    reply: summarize(spec),
    ask: phrasings,
    detail: { it: verbatim.slice(0, 250), en: verbatim.slice(0, 250) },
    source: 'forge-distill',
    provenance: provenanceHash,
    lang_primary: langPrimary,
  };

  const candAsks = askPhrasings(candidate);
  const candWords = new Set(words);
  const pool = [...(ctx.existingCards || []), ...(ctx.stagedCards || [])];

  for (const other of pool) {
    if (!other) continue;
    const otherAsks = askPhrasings(other);
    if (!otherAsks.length) continue;
    const sim = maxAskSimilarity(candAsks, otherAsks);
    if (sim < COLLISION_THRESHOLD) continue;   // not close enough to be either → keep scanning

    // Same topic vs collision is decided by the other card's IDENTITY, not its asks: a single
    // copied/near phrasing would otherwise inject the candidate's own words into the comparison and
    // mask a true cross-topic collision. Identity = id slug words ∪ reply words (not the asks).
    const identityWords = new Set([
      ...contentWords(String(other.id || '').replace(/[._-]+/g, ' ')),
      ...contentWords(other.reply ? [other.reply.it, other.reply.en].filter(Boolean).join(' ') : ''),
    ]);
    let shared = 0;
    for (const w of candWords) if (identityWords.has(w)) shared++;
    const sameTopic = other.id === id || (identityWords.size > 0 && candWords.size && shared / candWords.size >= 0.5);

    return sameTopic ? { staged: null, reason: 'duplicate' } : { staged: null, reason: 'collision' };
  }

  return { staged: candidate, reason: 'staged' };
}

// stagePatch(card) → one JSONL line the caller appends to /data/anima/learned-forge.jsonl.
// Serialization ONLY (pure); the caller does the I/O. The card already links to its provenance hash.
export function stagePatch(card) {
  if (!card || typeof card !== 'object') throw new Error('stagePatch: card required');
  return JSON.stringify(card);
}
