// qgen.mjs — pure, testable core for ANIMA question generation (the `ask` phrasings the
// retriever matches). Teacher LLMs (ollama/grok) propose many paraphrases; this module is the
// QUALITY GATE that keeps the corpus clean:
//   • relevance  — the question must be on-topic for its card (anti-hallucination: no question
//                  whose answer the card can't actually support).
//   • dedup      — drop near-duplicates of the card's own asks / of already-accepted ones.
//   • collision  — drop a question too similar to a DIFFERENT card's asks (cross-card ambiguity
//                  is what makes a shallow encoder return the wrong fact — the real hazard).
// Similarity is trigram-Jaccard by default; pass a vector `sim` (ollama / sentence-transformers
// embeddings) for semantic collision detection. No I/O, no network — host-testable.

export const normQ = (s) => String(s || '').toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '')
  .replace(/[^a-z0-9\s]/g, ' ').replace(/\s+/g, ' ').trim();

export function trigrams(s) {
  const t = '  ' + (typeof s === 'string' ? normQ(s) : s) + '  ';
  const g = new Set();
  for (let i = 0; i < t.length - 2; i++) g.add(t.slice(i, i + 3));
  return g;
}
export function jaccard(a, b) {
  const A = a instanceof Set ? a : trigrams(a), B = b instanceof Set ? b : trigrams(b);
  let inter = 0; for (const x of A) if (B.has(x)) inter++;
  const uni = A.size + B.size - inter; return uni ? inter / uni : 0;
}
export function cosine(a, b) {
  let d = 0, na = 0, nb = 0;
  for (let i = 0; i < a.length; i++) { d += a[i] * b[i]; na += a[i] * a[i]; nb += b[i] * b[i]; }
  return na && nb ? d / (Math.sqrt(na) * Math.sqrt(nb)) : 0;
}

// Stopwords (IT+EN) so relevance compares content words, not glue.
const STOP = new Set(('il lo la i gli le un uno una di a da in con su per tra fra e o ma che chi cosa come quando dove '
  + 'perche quale quali del della dello dei degli delle al allo alla ai agli alle è e\' sono era '
  + 'the a an of to in on for and or is are was were be as at by it this that with what whats how who when where which '
  + 'do does did your you my me i s').split(/\s+/));
export function contentWords(s) { return normQ(s).split(' ').filter((w) => w.length > 2 && !STOP.has(w)); }

// A question is RELEVANT to a card when enough of its content words appear in the card's topic
// text (title + reply). Loose enough for paraphrases, tight enough to reject off-topic drift.
export function relevant(question, topicText, { min = 0.34 } = {}) {
  const qw = contentWords(question);
  if (!qw.length) return false;
  const t = new Set(contentWords(topicText));
  let hit = 0; for (const w of qw) if (t.has(w)) hit++;
  return hit / qw.length >= min;
}

// Bilingual interrogative cue — a real "ask", not an asserted fact.
const CUE = /\b(cos|cosa|cos'|che\s+cos|significa|significato|definizione|spiega|spiegami|spiegare|come|chi|quando|dove|quale|quali|quanti|quanto|perche|what|what's|whats|define|meaning|explain|how|who|when|where|which|describe|tell me)\b/i;
export function isQuestion(s) {
  s = String(s || '').trim();
  if (s.length < 4 || s.length > 80) return false;
  if (s.split('.').length > 2) return false;             // a sentence/statement, not a query
  if (/\b(19|20)\d\d\b/.test(s) || /\d{3,}/.test(s)) return false;   // year/number -> smells like an asserted fact
  return CUE.test(s);
}

// Default lexical similarity.
const lexSim = (a, b) => jaccard(a, b);

// Filter candidate questions for ONE card. Returns the accepted list (deduped, on-topic,
// non-colliding), capped at `max`. `foreign` is [{q, id}] across the WHOLE corpus.
export function acceptVariants(candidates, {
  selfId = '', topicText = '', existing = [], foreign = [],
  dupTh = 0.8, collideTh = 0.84, relMin = 0.34, max = 8,
  requireCue = true, sim = lexSim,
} = {}) {
  const accepted = [], acceptedNorm = [];
  const existNorm = existing.map(normQ);
  const foreignOther = foreign.filter((f) => f && f.id !== selfId && f.q);
  for (const raw of candidates) {
    const q = String(raw || '').trim();
    const nq = normQ(q);
    if (!nq) continue;
    if (requireCue && !isQuestion(q)) continue;                       // must be a genuine question
    if (topicText && !relevant(q, topicText, { min: relMin })) continue;   // anti-hallucination: on-topic
    let bad = false;
    for (const e of existNorm) if (jaccard(nq, e) >= dupTh) { bad = true; break; }   // dup vs card's own asks
    if (!bad) for (const a of acceptedNorm) if (jaccard(nq, a) >= dupTh) { bad = true; break; }  // dup vs accepted
    if (bad) continue;
    for (const f of foreignOther) if (sim(q, f.q) >= collideTh) { bad = true; break; }   // collision vs other cards
    if (bad) continue;
    accepted.push(q); acceptedNorm.push(nq);
    if (accepted.length >= max) break;
  }
  return accepted;
}

// Build the cross-card foreign index from a list of cards: every ask (both languages) tagged
// with its card id, so acceptVariants can reject cross-card collisions.
export function foreignIndex(cards, { lang } = {}) {
  const out = [];
  for (const c of cards) {
    const a = c.ask || {};
    const langs = lang ? [lang] : ['it', 'en'];
    for (const L of langs) for (const q of (a[L] || [])) out.push({ q, id: c.id });
  }
  return out;
}
