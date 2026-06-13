// ANIMA NSPCG — Neuro-Symbolic Proof-Carrying Generation. The offline answer to "can a Cardputer
// GENERATE, not just retrieve?" without an LLM and without hallucinating. The leap over everything
// already in ANIMA:
//
//   • MOSAICO (l1_stitch) glues VERBATIM card spans — zero synthesis.
//   • combinator.mjs COMPOSES ≥2 facts into a new sentence — but only fixed 2-fact templates, and its
//     "proof" is a flat list of source ids.
//   • kge.mjs DISCOVERS multi-hop chains autonomously (reach) — but emits an ENTITY, not language.
//
// NSPCG is the missing synthesis the codebase was primed for: it lets ANIMA (1) DISCOVER a derivation
// chain by itself in hyperdimensional space (kge.reach — relations are bit-rotations), (2) VERIFY every
// hop against the symbolic triple store (HDC proposes, symbols dispose — soundness), (3) VERBALIZE the
// verified chain into a sentence that exists NOWHERE in the corpus, and (4) attach a machine-checkable
// PROOF TREE so any consumer can re-derive the claim without trusting the generator.
//
// The property no LLM has: every generated sentence is HALLUCINATION-IMPOSSIBLE BY CONSTRUCTION — a
// clause can only be emitted if it reduces to a stored edge that passes the coherence gate. When the
// chain breaks, NSPCG REFUSES (honest miss) instead of confabulating a bridge. The "thinking" is real
// (autonomous beam search), the output is novel, and the answer carries its own proof.
//
// Pure JS, builds only on hdc.mjs (XOR/popcount/rotate — the ops the S3 does natively) + kge.mjs.
// This is the reference twin; the firmware C port lands the reach() beam search into nucleo_anima_hdc.c.
import { GATE } from './kge.mjs';

// ---------- surface realization (a thin, honest grammar — NOT where the novelty lives) ----------
const deslug = (s) => String(s).replace(/[_-]+/g, ' ').trim();
const cap = (s) => deslug(s).split(' ').map((w) => w.charAt(0).toUpperCase() + w.slice(1)).join(' '); // title-case

// Minimal Italian "di + article" contraction for the object of a relation (del/della/dell'/di). Vowel-
// initial names elide; otherwise a tiny gender lexicon picks del/della; unknown → bare "di" (always
// grammatical). This is the only piece of language-specific morphology — kept small and graceful.
const GENDER = { giappone: 'm', regno: 'm', 'regno unito': 'm', portogallo: 'm', brasile: 'm', francia: 'f', spagna: 'f', germania: 'f', cina: 'f' };
function diOf(name) {
  const l = deslug(name).toLowerCase();
  if (/^[aeiou]/.test(l)) return `dell'${cap(name)}`;        // dell'Italia, dell'Europa
  const g = GENDER[l];
  if (g === 'm') return `del ${cap(name)}`;                  // del Giappone
  if (g === 'f') return `della ${cap(name)}`;                // della Francia
  return `di ${cap(name)}`;                                  // safe default
}

// Per-relation clause templates. Each renders ONE verified edge into a grounded clause. The load-bearing
// guarantee is that every clause = one stored edge; fluency (articles/prepositions) is this thin layer.
const CLAUSE = {
  it: {
    si_trova_in: (h, t) => `${cap(h)} si trova in ${cap(t)}`,
    capitale_di: (h, t) => `${cap(h)} è la capitale ${diOf(t)}`,
    parte_di:    (h, t) => `${cap(h)} fa parte ${diOf(t)}`,
    nato_in:     (h, t) => `${cap(h)} è nato a ${cap(t)}`,
    _default:    (h, r, t) => `${cap(h)} → ${deslug(r)} → ${cap(t)}`,
  },
  en: {
    si_trova_in: (h, t) => `${cap(h)} is located in ${cap(t)}`,
    capitale_di: (h, t) => `${cap(h)} is the capital of ${cap(t)}`,
    parte_di:    (h, t) => `${cap(h)} is part of ${cap(t)}`,
    nato_in:     (h, t) => `${cap(h)} was born in ${cap(t)}`,
    _default:    (h, r, t) => `${cap(h)} → ${deslug(r)} → ${cap(t)}`,
  },
};

// Conclusion templates per CLOSURE relation (the deduced, never-stored fact the chain entails).
const CONCLUDE = {
  it: {
    si_trova_in: (h, t) => `${cap(h)} si trova in ${cap(t)}`,
    nato_in:     (h, t) => `${cap(h)} è nato in un luogo che si trova in ${cap(t)}`,
    connesso:    (h, t) => `${cap(h)} è collegato a ${cap(t)}`,
  },
  en: {
    si_trova_in: (h, t) => `${cap(h)} is located in ${cap(t)}`,
    nato_in:     (h, t) => `${cap(h)} was born in a place located in ${cap(t)}`,
    connesso:    (h, t) => `${cap(h)} is connected to ${cap(t)}`,
  },
};

const clause = (lang, h, r, t) => (CLAUSE[lang][r] || ((a, b) => CLAUSE[lang]._default(a, r, b)))(h, t);

// ---------- relation algebra: what fact does a PATH of relations entail? ----------
// Containment relations are transitive and compose to "is located in". Birth-then-containment yields a
// (transitively located) birthplace. Anything else collapses to the weakest TRUE reading: "connected to".
// We never assert a specific relation we cannot justify from the path — soundness over fluency.
const CONTAINMENT = new Set(['si_trova_in', 'capitale_di', 'parte_di']);
function closureRel(rels) {
  if (rels.every((r) => CONTAINMENT.has(r))) return 'si_trova_in';
  if (rels[0] === 'nato_in' && rels.slice(1).every((r) => CONTAINMENT.has(r))) return 'nato_in';
  return 'connesso';
}

// ---------- walk a discovered path, materializing + GROUNDING every hop ----------
// reach() hands back only (final entity, relation path, min coherence). To verbalize AND to prove, we
// re-walk the path hop-by-hop: at each step rotate the current entity vector by the relation and clean up
// (exactly what reach did), capturing each intermediate entity + its coherence. Then — the soundness gate
// — we require every materialized (h,rel,t) to be an ACTUAL stored edge. If HDC's fuzzy match invented a
// hop that isn't a real triple, the chain is rejected. HDC discovers; the symbol table verifies.
function walkAndGround(kg, h0, rels) {
  const storedSet = kg._storedSet || (kg._storedSet = new Set(kg.triples.map((e) => `${e.h}|${e.r}|${e.t}`)));
  const steps = [];
  let cur = h0;
  for (const r of rels) {
    const c = kg.cb.cleanup(kg.rotate(kg.vec(cur), r));
    if (c.coherence < GATE) return null;                       // a hop that doesn't resolve crisply → refuse
    const stored = storedSet.has(`${cur}|${r}|${c.key}`);
    if (!stored) return null;                                  // HDC proposed an edge the symbols don't back → refuse
    steps.push({ h: cur, rel: r, t: c.key, coherence: +c.coherence.toFixed(2), stored: true, src: `kg:${cur}|${r}|${c.key}` });
    cur = c.key;
  }
  return steps;
}

const confOf = (minCoh) => Math.round((100 * minCoh) / (minCoh + GATE)); // anchored: coherence==GATE → 50%

// Build the proof-carrying envelope from a grounded, verified chain.
function envelope(lang, steps) {
  const h0 = steps[0].h, hN = steps[steps.length - 1].t, rels = steps.map((s) => s.rel);
  const rule = closureRel(rels);
  const minCoh = Math.min(...steps.map((s) => s.coherence));
  const becauseWord = lang === 'en' ? ', because ' : ', perché ';
  const andWord = lang === 'en' ? ' and ' : ' e ';
  const reply =
    CONCLUDE[lang][rule](h0, hN) +
    (steps.length > 1 || rule === 'connesso'
      ? becauseWord + steps.map((s) => clause(lang, s.h, s.rel, s.t)).join(andWord)
      : '') +
    '.';
  return {
    reply,
    confidence: confOf(minCoh),
    provenance: steps.map((s) => s.src),
    proof: { claim: { h: h0, rel: rule, t: hN }, rule: `closure(${rels.join('∘')})→${rule}`, derivation: steps },
    intent: 'pcg',
    tier: 'fact',
    action: 'answer',
  };
}

// ---------- the standalone proof checker: the "carrying" has teeth ----------
// Anyone (a verifier tier, the firmware's nucleo_anima_verify_claim, a skeptical user) can re-check a
// generated answer WITHOUT trusting NSPCG. Returns {ok, failures[]}. This is what an LLM can never give.
export function verifyProof(kg, proof) {
  const storedSet = new Set(kg.triples.map((e) => `${e.h}|${e.r}|${e.t}`));
  const fails = [];
  const d = proof && proof.derivation;
  if (!d || !d.length) return { ok: false, failures: ['empty derivation'] };
  // 1) every hop is a real stored edge
  for (const s of d) if (!storedSet.has(`${s.h}|${s.rel}|${s.t}`)) fails.push(`unstored hop ${s.h}|${s.rel}|${s.t}`);
  // 2) the chain is connected end-to-end
  for (let i = 1; i < d.length; i++) if (d[i].h !== d[i - 1].t) fails.push(`broken link at ${i}: ${d[i - 1].t} ≠ ${d[i].h}`);
  // 3) the claim's endpoints match the chain endpoints
  if (d[0].h !== proof.claim.h) fails.push(`claim head ${proof.claim.h} ≠ chain head ${d[0].h}`);
  if (d[d.length - 1].t !== proof.claim.t) fails.push(`claim tail ${proof.claim.t} ≠ chain tail ${d[d.length - 1].t}`);
  // 4) the asserted closure relation is the one the path actually entails
  if (closureRel(d.map((s) => s.rel)) !== proof.claim.rel) fails.push(`closure mismatch: path entails ${closureRel(d.map((s) => s.rel))}, claim says ${proof.claim.rel}`);
  return { ok: fails.length === 0, failures: fails };
}

// ---------- tight NL front-end (a guard, not the engine — firmware has the real NLU) ----------
const norm = (q) => String(q).toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '').trim();
export function parseQuery(q) {
  const s = norm(q);
  let m;
  // bridge: "come è collegato/connesso X a/con Y", "che lega X e/a Y"
  if ((m = s.match(/(?:come (?:e |è )?(?:collegat[oa]|conness[oa]|legat[oa])|che (?:lega|collega|unisce))\s+(.+?)\s+(?:a|con|e|ad|all[ao']?|al)\s+(.+?)\??$/)))
    return { mode: 'bridge', a: m[1].trim(), b: m[2].trim() };
  // why-located: "perché X è/si trova in Y"
  if ((m = s.match(/perch[eé]\s+(.+?)\s+(?:e |è |si trova )?(?:in|a|nell[ao']?)\s+(.+?)\??$/)))
    return { mode: 'why', a: m[1].trim(), b: m[2].trim() };
  // where/continent: "in che continente è X", "dove si trova X", "X in che continente"
  if ((m = s.match(/(?:in che (?:continente|emisfero|stato|paese)\s+(?:e |è |si trova )?|dove (?:si trova|e|è)\s+)(.+?)\??$/)))
    return { mode: 'where', a: m[1].trim() };
  if ((m = s.match(/^(.+?)\s+in che (?:continente|emisfero|stato|paese)\??$/)))
    return { mode: 'where', a: m[1].trim() };
  return null;
}

// ---------- the generator ----------
export class ProofGen {
  constructor(kg, { lang = 'it', maxDepth = 4, gate = GATE } = {}) {
    this.kg = kg; this.lang = lang; this.maxDepth = maxDepth; this.gate = gate;
  }
  _resolve(name) { const r = this.kg.resolve(name); return r.coherence >= this.gate ? r.key : (this.kg.ents.has(name) ? name : null); }

  // DERIVE: explain a target fact "h is (transitively) located in / connected to t".
  derive(head, target) {
    const h = this._resolve(head), t = this._resolve(target);
    if (!h || !t) return null;
    const reached = this.kg.reach(h, { maxDepth: this.maxDepth, gate: this.gate });
    // prefer the SHORTEST verified chain that lands on t (most direct explanation)
    const hits = reached.filter((x) => x.entity === t).sort((a, b) => a.path.length - b.path.length);
    for (const hit of hits) {
      const steps = walkAndGround(this.kg, h, hit.path);
      if (steps && steps[steps.length - 1].t === t) return envelope(this.lang, steps);
    }
    return null; // no grounded chain → honest refusal
  }

  // BRIDGE: abductive — discover IF and HOW a and b are connected, then verbalize the proof.
  bridge(a, b) { return this.derive(a, b); }

  // WHERE: deepest verified containment chain (→ continent / hemisphere), generated with its proof.
  where(head) {
    const h = this._resolve(head);
    if (!h) return null;
    const reached = this.kg.reach(h, { maxDepth: this.maxDepth, gate: this.gate })
      .filter((x) => x.path.every((r) => CONTAINMENT.has(r)))
      .sort((a, b) => b.path.length - a.path.length); // deepest first
    for (const hit of reached) {
      const steps = walkAndGround(this.kg, h, hit.path);
      if (steps) return envelope(this.lang, steps);
    }
    return null;
  }

  // EXPLAIN: NL entry. Parses, routes, generates a proof-carrying sentence — or refuses.
  explain(q) {
    const p = parseQuery(q);
    if (!p) return null;
    if (p.mode === 'why' || p.mode === 'bridge') return this.derive(p.a, p.b);
    if (p.mode === 'where') return this.where(p.a);
    return null;
  }
}

export default ProofGen;
