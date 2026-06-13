// ANIMA deductive core — from RECALL to INFERENCE. A knowledge graph embedded in hyperdimensional
// space where RELATIONS ARE PERMUTATIONS (cyclic bit-rotations). The point: permutations COMPOSE, so
// logical deduction becomes algebra the ESP32 does natively:
//
//   fact  h --r--> t      is embedded as   E_t ≈ ρ_r(E_h)        (ρ_r = rotate bits by k_r)
//   transitivity (r∘r)    is              ρ_r(ρ_r(E_h)) = ρ_r²(E_h)   ← a fact NEVER stored, computed
//   composition (r1∘r2)   is              ρ_r2(ρ_r1(E_h))
//   inverse (r⁻¹)         is              ρ_r⁻¹ = rotate by D−k_r   ← bidirectional for free
//
// So ANIMA can answer "in che continente è Parigi?" though it only ever learned Parigi→Francia and
// Francia→Europa: it DEDUCES Parigi→Europa by composing two rotations. Confidence = cleanup coherence;
// if a chain doesn't resolve to a crisp entity it refuses. Gradient-free: entity vectors are propagated
// from semantic roots through the relation-rotations (one bundle per node), not trained.
//
// Why rotation and not XOR for relations: XOR is its own inverse (x⊕m⊕m=x) so it CANNOT do forward
// transitivity. Rotation is a proper cyclic group: ρ_k applied twice = ρ_{2k} ≠ identity → real chains.
// Reuses the binary primitives in hdc.mjs (permute/bundle/Codebook) — all XOR/popcount/shift.
import { D, semanticHV, permute, bundle, Codebook, hamming, STD } from './hdc.mjs';

function hash32(s) { let h = 2166136261 >>> 0; for (let i = 0; i < s.length; i++) { h ^= s.charCodeAt(i); h = Math.imul(h, 16777619); } return h >>> 0; }

export class KG {
  constructor() { this.triples = []; this.rels = new Set(); this.ents = new Set(); this.E = new Map(); this.cb = new Codebook(); this.label = new Map(); }
  // Rotation amount for a relation: deterministic, non-zero, well inside [1, D-1].
  shift(r) { return 1 + (hash32('rel:' + r) % (D - 2)); }
  rotate(v, r) { return permute(v, this.shift(r)); }                  // forward ρ_r
  unrotate(v, r) { return permute(v, (D - this.shift(r)) % D); }      // inverse ρ_r⁻¹
  add(h, r, t, label) { this.triples.push({ h, r, t }); this.rels.add(r); this.ents.add(h); this.ents.add(t); if (label) this.label.set(t, label); return this; }

  // Build the embedding: roots (no incoming edge) anchor to their SEMANTIC vector (grounding + identity);
  // every other node = the bundle (majority) of its incoming constraints ρ_r(E_head). Sweep enough times
  // for the deepest chain to propagate. Pure binary, O(triples·D) per sweep.
  build(sweeps) {
    const incoming = new Map();                                       // t -> [{h, r}]
    for (const e of this.ents) incoming.set(e, []);
    for (const { h, r, t } of this.triples) incoming.get(t).push({ h, r });
    for (const e of this.ents) this.E.set(e, semanticHV(e));          // init: semantic anchor
    const depth = this.longestChain();
    const K = sweeps || (depth + 2);
    for (let s = 0; s < K; s++) {
      const next = new Map();
      for (const e of this.ents) {
        const inc = incoming.get(e);
        if (!inc.length) { next.set(e, semanticHV(e)); continue; }    // root stays its semantic identity
        // bundle the relational evidence; a light semantic anchor keeps distinct tails from collapsing.
        next.set(e, bundle([semanticHV(e), ...inc.map(({ h, r }) => this.rotate(this.E.get(h), r))]));
      }
      this.E = next;
    }
    this.cb = new Codebook(); for (const e of this.ents) this.cb.add(e, this.E.get(e));
    return this;
  }
  longestChain() { // crude DAG depth (caps cycles) — sets the sweep count
    const seen = new Map(); const out = new Map(); for (const e of this.ents) out.set(e, []);
    for (const { h, t } of this.triples) out.get(h).push(t);
    const dfs = (e, d) => { if (d > 16) return d; if (seen.has(e)) return seen.get(e); let m = 0; for (const t of out.get(e) || []) m = Math.max(m, 1 + dfs(t, d + 1)); seen.set(e, m); return m; };
    let mx = 1; for (const e of this.ents) mx = Math.max(mx, dfs(e, 0)); return Math.min(mx, 16);
  }

  vec(e) { return this.E.get(e) || semanticHV(e); }
  // Resolve a (possibly partial) name to the nearest known entity by semantic similarity — so "Manzoni"
  // finds "Alessandro Manzoni". Returns the cleanup {key, coherence}.
  resolve(name) { return this.cb.cleanup(semanticHV(name)); }
  // Predict the tail of a relation PATH from head h (e.g. [located_in, located_in] = 2 hops). The whole
  // chain is one sequence of rotations, then a single cleanup. coherence = how crisply it lands.
  inferPath(h, rels) {
    let v = this.vec(h); for (const r of rels) v = this.rotate(v, r);
    const c = this.cb.cleanup(v); c.path = rels; return c;
  }
  infer(h, r) { return this.inferPath(h, [r]); }
  inverse(t, r) { const c = this.cb.cleanup(this.unrotate(this.vec(t), r)); c.path = [r + '⁻¹']; return c; }

  // AUTONOMOUS multi-hop: the device DISCOVERS the reasoning chain itself. Beam search in vector space:
  // from h, try every relation, cleanup, keep the high-coherence expansions, snap to the clean entity
  // vector (denoise between hops — the resonator idea) and recurse. Returns reachable {entity, path, coh}.
  reach(h, { maxDepth = 3, gate = 4.0, beam = 4 } = {}) {
    const out = []; const seen = new Set([h]);
    let frontier = [{ e: h, v: this.vec(h), path: [], coh: Infinity }];
    for (let d = 0; d < maxDepth; d++) {
      const cand = [];
      for (const node of frontier) for (const r of this.rels) {
        const c = this.cb.cleanup(this.rotate(node.v, r));
        if (c.coherence >= gate && !seen.has(c.key)) cand.push({ e: c.key, v: this.vec(c.key), path: [...node.path, r], coh: Math.min(node.coh, c.coherence) });
      }
      cand.sort((a, b) => b.coh - a.coh);
      frontier = cand.slice(0, beam);
      for (const n of frontier) { if (!seen.has(n.e)) { seen.add(n.e); out.push({ entity: n.e, path: n.path, coherence: n.coh }); } }
      if (!frontier.length) break;
    }
    return out;
  }
}

export const GATE = 4.0;   // coherence units (margin/σ). Bench-measured separation: entailed ≫ this ≫ noise.
