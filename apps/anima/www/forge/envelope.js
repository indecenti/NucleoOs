// envelope.js — ANIMA Envelope v1: the single canonical shape EVERY substrate (M1/M2/M3/M4)
// emits, so the chat renderer, context compaction, message actions and export never branch on
// who answered. It is a strict SUPERSET of the device /api/anima JSON (nucleo_httpd.c:566-601)
// plus the four new fields {substrate, grounding, verdict, actions, usage}. M1/M2/M3 keep their
// wire format byte-identical; the new fields are CLIENT-synthesised (and null/empty for the
// device substrates today). Pure & DOM-free → host-testable.

export const SUBSTRATES = ['M1', 'M2', 'M3', 'M4-local'];
// dial segment → which device substrate answers (M4 is the new browser-local one).
export const MODE_TO_SUBSTRATE = { off: 'M1', on: 'M2', only: 'M3', local: 'M4-local' };

export function deviceSubstrate(mode) { return MODE_TO_SUBSTRATE[mode] || 'M2'; }

const asStr = (x, d = '') => (typeof x === 'string' ? x : (x == null ? d : String(x)));
const asNum = (x, d = 0) => (Number.isFinite(x) ? x : (Number.isFinite(+x) ? +x : d));
const asBool = (x) => x === true || x === 1 || x === '1' || x === 'true';

// Normalise a raw device JSON OR a client-built M4 result into the canonical envelope.
// opts: { substrate?, mode?, lang? }. substrate wins, else derived from mode, else M2.
export function normalize(raw, opts = {}) {
  raw = raw || {};
  const substrate = opts.substrate || raw.substrate || deviceSubstrate(opts.mode);
  return {
    v: 1,
    substrate,
    query: asStr(raw.query),
    tier: asStr(raw.tier),
    action: asStr(raw.action, 'answer'),
    arg: asStr(raw.arg),
    reply: asStr(raw.reply),
    path: asStr(raw.path),
    tool: asStr(raw.tool),
    confidence: asNum(raw.confidence, 0),
    domain: asStr(raw.domain),
    intent: asStr(raw.intent),
    lang: asStr(raw.lang, opts.lang || 'it'),
    budget: asNum(raw.budget, 0),
    memory: asBool(raw.memory),
    state: asStr(raw.state),
    awaiting: asBool(raw.awaiting),
    corrected: asStr(raw.corrected),
    trace: asStr(raw.trace),
    // ---- v1 additions (client-synthesised) ----
    grounding: Array.isArray(raw.grounding) ? raw.grounding : [],
    verdict: (raw.verdict && typeof raw.verdict === 'object') ? raw.verdict : null,
    actions: Array.isArray(raw.actions) ? raw.actions : [],
    usage: (raw.usage && typeof raw.usage === 'object') ? raw.usage : {},
  };
}

// Structural validation — the renderer must be safe over any normalised envelope, and a substrate
// must never emit an unknown shape. Returns { ok, errors }.
export function validate(env) {
  if (!env || typeof env !== 'object') return { ok: false, errors: ['not-object'] };
  const errors = [];
  if (env.v !== 1) errors.push('bad-version');
  if (!SUBSTRATES.includes(env.substrate)) errors.push('bad-substrate:' + env.substrate);
  for (const k of ['query', 'tier', 'action', 'reply', 'domain', 'intent', 'lang', 'trace', 'arg', 'path', 'tool', 'state', 'corrected'])
    if (typeof env[k] !== 'string') errors.push('non-string:' + k);
  if (typeof env.confidence !== 'number') errors.push('confidence-not-number');
  if (typeof env.awaiting !== 'boolean') errors.push('awaiting-not-bool');
  if (!Array.isArray(env.actions)) errors.push('actions-not-array');
  if (!Array.isArray(env.grounding)) errors.push('grounding-not-array');
  if (env.verdict && !['pass', 'warn', 'veto'].includes(env.verdict.verdict)) errors.push('bad-verdict');
  return { ok: errors.length === 0, errors };
}

// Provenance badge metadata for the UI — M4 must NEVER borrow the on-device/grounded look.
export function provenance(env) {
  switch (env.substrate) {
    case 'M1': return { kind: 'on-device', grounded: true, label: 'on-device' };
    case 'M2': return { kind: 'hybrid', grounded: true, label: 'grounded' };
    case 'M3': return { kind: 'cloud', grounded: false, label: 'cloud' };
    case 'M4-local': return { kind: 'local-gpu', grounded: false, label: 'local-GPU' };
    default: return { kind: 'unknown', grounded: false, label: '?' };
  }
}
