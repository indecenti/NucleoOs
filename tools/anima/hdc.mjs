// ANIMA HDC/VSA reasoning core — a hyperdimensional algebra ANIMA can THINK in, offline, using only
// the operations an ESP32-S3 does natively at ~zero cost: XOR (bind), popcount (distance), bitwise
// majority (bundle). Three things make this more than textbook HDC:
//
//  1. SEMANTIC ATOMS. Concept hypervectors are SimHash of the concept's text features, not random —
//     so semantically/lexically near concepts stay CORRELATED in Hamming space. The algebra then
//     tolerates paraphrase (the documented weakness of the shallow n-gram encoder), while distinct
//     concepts stay quasi-orthogonal (concentration of measure / JL).
//  2. RESONANCE COHERENCE = INTRINSIC CONFIDENCE. When we factor a question into an answer, the
//     stability of the fixed point and the Hamming margin of the winning codeword ARE the confidence.
//     No tuned threshold: an answerable query resolves coherently; an unknown one doesn't converge
//     -> honest "non lo so". Honesty becomes a convergence property of the computation.
//  3. ONE-SHOT, GRADIENT-FREE LEARNING. A new fact is one more XOR-bundle into a memory hypervector.
//     Learning = addition. Perfect for an MCU (no backprop), instant, O(D/32) words.
//
// Pure JS, no deps. Every op maps 1:1 to C on the device (Uint32 words = the S3's SIMD lanes).

export const D = 8192;                 // hypervector width in bits (1 KB each). N facts safely bundled while N << D.
const W = D >>> 5;                      // 32-bit words per hypervector
export const STD = Math.sqrt(D) / 2;   // std of Hamming between two random HVs (Binomial(D,1/2)) ~ 45 bits

// --- deterministic PRNG (so atoms are reproducible from their name; on-device: regenerate, don't store) ---
function hash32(s) { let h = 2166136261 >>> 0; for (let i = 0; i < s.length; i++) { h ^= s.charCodeAt(i); h = Math.imul(h, 16777619); } return h >>> 0; }
function mulberry32(a) { return function () { a |= 0; a = (a + 0x6D2B79F5) | 0; let t = Math.imul(a ^ (a >>> 15), 1 | a); t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }; }

export function newHV() { return new Uint32Array(W); }
// A purely random hypervector (for STRUCTURAL atoms: roles like NATO/CAPITALE that must be orthogonal).
export function randomHV(seed) { const r = mulberry32(hash32(seed)); const v = newHV(); for (let i = 0; i < W; i++) v[i] = (r() * 4294967296) >>> 0; return v; }

function popcount(x) { x = x - ((x >>> 1) & 0x55555555); x = (x & 0x33333333) + ((x >>> 2) & 0x33333333); x = (x + (x >>> 4)) & 0x0f0f0f0f; return (Math.imul(x, 0x01010101) >>> 24); }
export function hamming(a, b) { let d = 0; for (let i = 0; i < W; i++) d += popcount((a[i] ^ b[i]) >>> 0); return d; }
export const bind = (a, b) => { const o = newHV(); for (let i = 0; i < W; i++) o[i] = (a[i] ^ b[i]) >>> 0; return o; };   // self-inverse: A⊗A=0
export const permute = (a, k = 1) => { const o = newHV(); for (let i = 0; i < D; i++) { if ((a[i >>> 5] >>> (i & 31)) & 1) { const j = (i + k) % D; o[j >>> 5] |= (1 << (j & 31)); } } return o; };
export const sim = (a, b) => 1 - 2 * hamming(a, b) / D;          // cosine-like: +1 identical, 0 orthogonal, -1 opposite

// SEMANTIC atom: SimHash of the text's features (word tokens + boundary char-3-grams). Shared features
// -> shared random contributions -> small Hamming. This is locality-sensitive hashing: near text, near HV.
function features(text) {
  const t = String(text).toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '');
  const out = []; for (const w of t.split(/[^a-z0-9]+/).filter(Boolean)) { out.push('w:' + w); const p = '^' + w + '$'; for (let i = 0; i + 3 <= p.length; i++) out.push('g:' + p.slice(i, i + 3)); }
  return out.length ? out : ['w:' + t];
}
export function semanticHV(text) {
  const cnt = new Int32Array(D); const fs = features(text);
  for (const f of fs) { const r = mulberry32(hash32('atom:' + f)); for (let i = 0; i < D; i++) cnt[i] += (r() < 0.5 ? -1 : 1); }
  const v = newHV(); for (let i = 0; i < D; i++) if (cnt[i] > 0) v[i >>> 5] |= (1 << (i & 31)); return v;
}

// Majority bundle (superposition). Odd N -> no ties; for even N a fixed tie-vector breaks deterministically.
const TIE = randomHV('::tie::');
export function bundle(list) {
  if (!list.length) return newHV(); if (list.length === 1) return list[0].slice();
  const cnt = new Int32Array(D);
  for (const v of list) for (let i = 0; i < W; i++) { let w = v[i]; const base = i << 5; while (w) { const b = 31 - Math.clz32(w & -w); cnt[base + b]++; w &= w - 1; } }
  const half = list.length / 2, v = newHV();
  for (let i = 0; i < D; i++) { const c = cnt[i]; const on = c > half || (c === half && ((TIE[i >>> 5] >>> (i & 31)) & 1)); if (on) v[i >>> 5] |= (1 << (i & 31)); }
  return v;
}

// --- cleanup memory: snap a noisy HV back to the nearest stored item; the MARGIN is the confidence ---
export class Codebook {
  constructor() { this.items = []; }                                   // {key, hv}
  add(key, hv) { this.items.push({ key, hv }); return this; }
  addText(key, text) { return this.add(key, semanticHV(text || key)); }
  get(key) { const e = this.items.find(x => x.key === key); return e ? e.hv : null; }
  // Nearest neighbour + how decisively it won. coherence = margin in units of random-Hamming std.
  cleanup(hv) {
    let best = null, bd = Infinity, second = Infinity;
    for (const e of this.items) { const d = hamming(hv, e.hv); if (d < bd) { second = bd; bd = d; best = e; } else if (d < second) second = d; }
    const margin = (second - bd);
    return { key: best && best.key, dist: bd, margin, coherence: margin / STD, sim: best ? 1 - 2 * bd / D : 0 };
  }
}

// --- relational records (the "mind"): each subject = bundle of (ROLE ⊗ value) over its facts -------
// Lets ANIMA recall a relation by UNBINDING (works for phrasings it never literally learned), do
// Kanerva-style analogy, and gate every answer by resonance coherence. Roles are structural (random),
// values/subjects are semantic (SimHash).
const roleCache = new Map();
export const role = (name) => { let h = roleCache.get(name); if (!h) { h = randomHV('role:' + name); roleCache.set(name, h); } return h; };

export class Mind {
  constructor() { this.records = new Map(); this.values = new Codebook(); this.subjects = new Codebook(); this._raw = new Map(); }
  // Learn a triple subject --rel--> value. One-shot: re-bundle the subject's record. Gradient-free.
  learn(subject, rel, value) {
    if (!this.values.get(value)) this.values.addText(value, value);
    if (!this.subjects.get(subject)) this.subjects.addText(subject, subject);
    const facts = this._raw.get(subject) || []; facts.push({ rel, value }); this._raw.set(subject, facts);
    this.records.set(subject, bundle(facts.map(f => bind(role(f.rel), this.values.get(f.value)))));
    return this;
  }
  record(subject) { return this.records.get(subject) || null; }
  // Answer "rel of subject" by unbinding the record, then cleaning up against the value codebook.
  ask(subject, rel) {
    const rec = this.records.get(subject); if (!rec) return { key: null, coherence: 0, reason: 'unknown-subject' };
    const noisy = bind(rec, role(rel));               // record ⊗ ROLE ≈ value (+ crosstalk noise)
    const r = this.values.cleanup(noisy); r.reason = 'unbind'; return r;
  }
  // Analogy "valueA is to A as ? is to B". Pure XOR, but ROBUST: the algebraic recA⊗recB⊗valueA trick is
  // unclean here because bind (XOR) does not distribute over bundle (bitwise MAJORITY is non-linear), so it
  // only survives for tiny records. Instead we DISCOVER the shared relation by unbinding A (the primitive
  // that works), then TRANSFER it to B. Two unbinds; coherence = the weaker of the two recalls.
  analogy(subjectA, subjectB, valueA) {
    const facts = this._raw.get(subjectA);
    if (!facts || !this.records.get(subjectB)) return { key: null, coherence: 0, reason: 'unknown' };
    let rel = null, relCoh = -1;
    for (const f of facts) { const r = this.ask(subjectA, f.rel); if (r.key === valueA && r.coherence > relCoh) { rel = f.rel; relCoh = r.coherence; } }
    if (!rel) return { key: null, coherence: 0, reason: 'no-shared-relation' };
    const out = this.ask(subjectB, rel); out.reason = 'analogy'; out.relation = rel;
    out.coherence = Math.min(relCoh, out.coherence); return out;
  }
}

// --- resonator: factor an unknown product P = a⊗b (a∈CB_A, b∈CB_B) by iterative resonance --------
// The "thinking" primitive: alternately estimate each factor by unbinding with the current estimate of
// the other and cleaning up. CONVERGENCE (a stable fixed point reached fast, with margin) = coherence;
// non-convergence / churn = "this question has no answer in my mind" -> honest refusal.
export function resonate(P, cbA, cbB, maxIter = 12) {
  let a = cbA.items[0].hv, b = cbB.items[0].hv;     // any start
  let prevA = null, prevB = null, iters = 0;
  let ra, rb;
  for (; iters < maxIter; iters++) {
    ra = cbA.cleanup(bind(P, b)); a = cbA.get(ra.key);
    rb = cbB.cleanup(bind(P, a)); b = cbB.get(rb.key);
    if (ra.key === prevA && rb.key === prevB) { iters++; break; }     // fixed point
    prevA = ra.key; prevB = rb.key;
  }
  const coherence = Math.min(ra.coherence, rb.coherence);
  return { a: ra.key, b: rb.key, iters, converged: iters < maxIter, coherence };
}

export const meta = { D, W, STD };
