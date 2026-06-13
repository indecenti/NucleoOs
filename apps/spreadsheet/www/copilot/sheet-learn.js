// sheet-learn.js — the OFFLINE RECIPE FLYWHEEL: the genuinely-new offline trick of this copilot.
// When a generative substrate (browser GPU orchestrator / coder, or cloud Grok) produces a PLAN that
// the deterministic engine VERIFIES (verdict 'pass') and the user APPLIES, we freeze the
// (normalised query → typed plan) mapping into an on-device recipe store. The NEXT time the same (or
// a near-identical) request arrives, it is answered by REPLAYING the verified plan — instantly, with
// NO model and NO network. So a model is needed at most ONCE per kind of request; forever after the
// $30 microcontroller serves a verified macro offline. The plan is a list of CLOSED, firewalled
// actions (sheet-actions), so replay is grounded-by-construction — it cannot drift into a new op.
//
// Two deterministic gates (no probabilistic acceptance), mirroring forge/learn.distill:
//   CERTAINTY  — verdict==='pass' AND applied===true. warn/veto/non-applied are NEVER learned.
//   USEFULNESS — non-empty content words; reject near-duplicates of an existing recipe
//                (trigram-Jaccard ≥ 0.8). Store is bounded (LRU) so it can't grow without limit.
// Storage is INJECTED ({get,set}) so the module is pure & host-testable; the browser passes a thin
// localStorage adapter. Pure & DOM-free.

const STOP = new Set([
  'la', 'il', 'lo', 'le', 'gli', 'i', 'di', 'da', 'in', 'con', 'per', 'che', 'del', 'della', 'dei',
  'delle', 'un', 'una', 'uno', 'mi', 'la', 'su', 'a', 'e', 'the', 'of', 'in', 'with', 'for', 'to',
  'a', 'an', 'me', 'my', 'this', 'that', 'and', 'on', 'colonna', 'colonne', 'column', 'columns',
]);

const KEY = 'anima.sheet.recipes.v1';
const MAX_RECIPES = 200;
const DUP_THRESHOLD = 0.8;

// Normalise a query for keying: lowercase, de-accent, collapse whitespace, strip a leading column
// letter token so "somma colonna A" and "somma colonna B" don't collide (the plan carries the col).
export function normQuery(q) {
  return String(q || '')
    .toLowerCase()
    .normalize('NFD').replace(/[̀-ͯ]/g, '')
    .replace(/[‘’“”`]/g, "'")
    .replace(/\bcolonn[ae]?\s+[a-z]\b/g, 'colonna')
    .replace(/\b[a-z]\b/g, ' ')           // drop bare single letters (column refs)
    .replace(/\s+/g, ' ')
    .trim();
}

function contentWords(q) {
  return normQuery(q).split(/[^a-z0-9]+/).filter((w) => w && w.length > 1 && !STOP.has(w));
}

function trigrams(s) {
  s = String(s || '').toLowerCase().replace(/\s+/g, ' ').trim();
  const out = new Set();
  if (s.length < 3) { if (s) out.add(s); return out; }
  for (let i = 0; i <= s.length - 3; i++) out.add(s.slice(i, i + 3));
  return out;
}

export function trigramJaccard(a, b) {
  const A = trigrams(a), B = trigrams(b);
  if (!A.size && !B.size) return 1;
  if (!A.size || !B.size) return 0;
  let inter = 0; for (const g of A) if (B.has(g)) inter++;
  return inter / (A.size + B.size - inter);
}

// ---- the store (storage injected: { get(key)->string|null, set(key,string) }) ----
export function loadRecipes(store) {
  try { const raw = store && store.get(KEY); const arr = raw ? JSON.parse(raw) : []; return Array.isArray(arr) ? arr : []; }
  catch { return []; }
}
function saveRecipes(store, recipes) {
  try { store && store.set(KEY, JSON.stringify(recipes.slice(0, MAX_RECIPES))); } catch { /* quota — best-effort */ }
}

// recall(store, query) → { recipe, score } | null. Returns the best recipe whose normalised query is
// trigram-close enough to be a confident match (≥ DUP_THRESHOLD). Touches its `used`/`ts` for LRU.
export function recall(store, query, ts = 0) {
  const nq = normQuery(query);
  if (!nq) return null;
  const recipes = loadRecipes(store);
  let best = null, bestScore = 0;
  for (const r of recipes) {
    const s = r.nq === nq ? 1 : trigramJaccard(nq, r.nq);
    if (s > bestScore) { bestScore = s; best = r; }
  }
  if (!best || bestScore < DUP_THRESHOLD) return null;
  best.used = (best.used || 0) + 1; best.ts = ts || best.ts;
  saveRecipes(store, recipes);
  return { recipe: best, score: bestScore };
}

// learn(store, turn) → { learned:recipe|null, reason }.
// turn: { query, plan:[validated actions], verdict, substrate, applied:bool, ts }
export function learn(store, turn = {}) {
  const { query, plan, verdict, substrate = '', applied, ts = 0 } = turn;

  // CERTAINTY GATE
  const v = verdict && verdict.verdict;
  if (v !== 'pass') return { learned: null, reason: 'not-certain:verdict-' + (v || 'none') };
  if (applied !== true) return { learned: null, reason: 'not-certain:not-applied' };
  if (!Array.isArray(plan) || !plan.length) return { learned: null, reason: 'not-certain:no-plan' };

  // USEFULNESS GATE
  const nq = normQuery(query);
  const words = contentWords(query);
  if (!nq || !words.length) return { learned: null, reason: 'not-useful:empty' };

  const recipes = loadRecipes(store);
  for (const r of recipes) {
    if (r.nq === nq || trigramJaccard(nq, r.nq) >= DUP_THRESHOLD) {
      // refresh the existing recipe with the freshly-verified plan (idempotent, no growth)
      r.plan = plan; r.verdict = v; r.substrate = substrate; r.ts = ts; r.used = (r.used || 0) + 1;
      saveRecipes(store, recipes);
      return { learned: r, reason: 'refreshed' };
    }
  }

  const recipe = { id: 'recipe.' + words.slice(0, 6).join('-'), nq, query: String(query || ''), plan, verdict: v, substrate, ts, used: 0 };
  recipes.unshift(recipe);
  // LRU eviction: keep the most-recently-used / most-recent (sort by used desc, then ts desc)
  recipes.sort((a, b) => (b.used || 0) - (a.used || 0) || (b.ts || 0) - (a.ts || 0));
  saveRecipes(store, recipes.slice(0, MAX_RECIPES));
  return { learned: recipe, reason: 'learned' };
}

// forget(store) — clear the recipe cache (a Settings action).
export function forget(store) { try { store && store.set(KEY, '[]'); } catch { /* */ } }

// In-memory store factory for host tests (and a graceful no-storage fallback in the browser).
export function memStore() {
  const m = new Map();
  return { get: (k) => (m.has(k) ? m.get(k) : null), set: (k, v) => { m.set(k, v); } };
}
