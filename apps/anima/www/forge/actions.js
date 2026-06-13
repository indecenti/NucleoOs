// actions.js — the CLOSED typed-action vocabulary the M4 orchestrator may emit, plus the schema
// FIREWALL that turns any model output (incl. adversarial / garbled JSON) into a validated,
// path-confined action list. This is what makes a tiny grammar-constrained model trustworthy as a
// router: it physically cannot produce an unknown op, a malformed arg, an out-of-root path, or
// stray prose that reaches the executor. The vocabulary mirrors nlfs.js {op:…} so the existing
// runFileIntent executor switch is reused verbatim. Pure & DOM-free → host-testable.

import { normPath } from '../fsclient.js';

// field spec: 'path' | 'str' (required) | '?path' | '?str' | '?bool' | 'enum:a,b,c'
export const ACTION_SCHEMA = {
  // file ops (mirror nlfs.js)
  read:   { path: 'path' },
  write:  { path: 'path', content: 'str' },
  append: { path: 'path', content: 'str' },
  edit:   { path: 'path', old: 'str', new: 'str', all: '?bool' },
  move:   { from: 'path', to: 'path' },
  delete: { path: 'path' },
  mkdir:  { path: 'path' },
  list:   { path: '?path' },
  tree:   { path: '?path' },
  search: { query: 'str', glob: '?str' },
  glob:   { pattern: 'str' },
  cd:     { path: 'path' },
  run:    { path: 'path' },
  // agent control
  synthesize: { spec: 'str', path: '?path', lang: '?str' },   // ask the coder LLM to write code
  answer:     { text: 'str' },                                // plain text reply
  ask:        { question: 'str' },                            // clarify (human)
  verify:     { path: '?path', code: '?str' },                // request the M1 grounded check
  route:      { substrate: 'enum:M1,M2,M3,M4-local' },        // router decision
  done:       { summary: '?str' },                            // stop the loop
};

const RUNNABLE = /\.(m?js|cjs)$/i;
const MUTATING = new Set(['write', 'append', 'edit', 'move', 'delete', 'mkdir']);
export function isMutating(op) { return MUTATING.has(op); }

// Path confinement: a relative path joined to root, or an absolute path, must resolve INSIDE root.
// This is the same guarantee fsclient.resolve() enforces at exec — duplicated here so a bad path is
// rejected at parse time, before any side effect.
export function confined(path, root = '/data/ws') {
  if (typeof path !== 'string' || !path) return false;
  if (/^[a-z][a-z0-9+.-]*:\/\//i.test(path)) return false;          // no URLs
  const base = normPath(root);
  const joined = path[0] === '/' ? normPath(path) : normPath(base + '/' + path);
  return joined === base || joined.startsWith(base + '/');
}

function coerce(spec, v) {
  if (spec === 'path' || spec === 'str') return (typeof v === 'string' && v.length) ? v : undefined;
  if (spec === '?path' || spec === '?str') return v == null ? '' : (typeof v === 'string' ? v : undefined);
  if (spec === '?bool') return v == null ? false : (typeof v === 'boolean' ? v : undefined);
  if (spec.startsWith('enum:')) { const opts = spec.slice(5).split(','); return opts.includes(v) ? v : undefined; }
  return undefined;
}

// Validate ONE action object → { ok, action } | { ok:false, reason }.
export function validateAction(a, { root = '/data/ws' } = {}) {
  if (!a || typeof a !== 'object' || Array.isArray(a)) return { ok: false, reason: 'not-object' };
  const op = a.op || a.tool || a.action;
  if (typeof op !== 'string' || !ACTION_SCHEMA[op]) return { ok: false, reason: 'unknown-op:' + op };
  const schema = ACTION_SCHEMA[op];
  const out = { op };
  for (const [k, spec] of Object.entries(schema)) {
    const val = coerce(spec, a[k]);
    if (val === undefined && spec[0] !== '?') return { ok: false, reason: 'bad-field:' + op + '.' + k };
    if (val !== undefined) out[k] = val;
  }
  for (const [k, spec] of Object.entries(schema))
    if ((spec === 'path' || spec === '?path') && out[k] && !confined(out[k], root)) return { ok: false, reason: 'path-escape:' + k };
  if (op === 'move' && (!confined(out.from, root) || !confined(out.to, root))) return { ok: false, reason: 'path-escape:move' };
  if (op === 'run' && !RUNNABLE.test(out.path)) return { ok: false, reason: 'not-runnable' };
  return { ok: true, action: out };
}

// Pull a JSON value out of possibly fenced / prose-wrapped model text. Returns null if none.
export function extractJson(text) {
  if (text && typeof text === 'object') return text;
  const s = String(text || '');
  const fence = /```(?:json)?\s*([\s\S]*?)```/i.exec(s);
  const cands = [];
  if (fence) cands.push(fence[1]);
  cands.push(s);
  const span = s.match(/[\[{][\s\S]*[\]}]/);
  if (span) cands.push(span[0]);
  for (const c of cands) { try { return JSON.parse(c); } catch { /* next */ } }
  return null;
}

// THE FIREWALL: any model output → { actions:[validated], rejected:[{raw,reason}] }.
// Guarantee: every action in `actions` is in the closed set, well-typed, and path-confined.
export function parseActions(modelOutput, { root = '/data/ws' } = {}) {
  const json = extractJson(modelOutput);
  let list = [];
  if (Array.isArray(json)) list = json;
  else if (json && Array.isArray(json.actions)) list = json.actions;
  else if (json && (json.op || json.tool || json.action)) list = [json];
  const actions = [], rejected = [];
  for (const a of list) {
    const v = validateAction(a, { root });
    if (v.ok) actions.push(v.action); else rejected.push({ raw: a, reason: v.reason });
  }
  return { actions, rejected };
}
