// provenance.js — tamper-evident provenance ledger for APPLIED artifacts, in the spirit of the
// project's Verifiable Knowledge Ledger. Every agentic write appends an immutable, hash-chained
// record: who/what generated it (substrate + model + pinned revision), the verdict it passed, the
// SHA-256 of the content, the human approver, and the previous hash. `verify` recomputes the chain
// and detects any mutation; an off-chain `anchor` (the last known head hash) defeats forge+reseal.
// Uses Web Crypto (globalThis.crypto.subtle) → works in the browser AND in Node → host-testable.
// Deterministic: timestamps are PASSED IN (no Date.now), so the chain is reproducible in CI.

const GENESIS = 'GENESIS';

// Stable, key-sorted JSON so the hash is canonical regardless of property order.
export function canonical(obj) {
  if (obj === null || typeof obj !== 'object') return JSON.stringify(obj);
  if (Array.isArray(obj)) return '[' + obj.map(canonical).join(',') + ']';
  return '{' + Object.keys(obj).sort().map((k) => JSON.stringify(k) + ':' + canonical(obj[k])).join(',') + '}';
}

export async function sha256hex(str) {
  const data = new TextEncoder().encode(String(str == null ? '' : str));
  const buf = await crypto.subtle.digest('SHA-256', data);
  return [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, '0')).join('');
}

// entry: { path, substrate, model, revision, verdict, contentSha, approver, ts }
export async function append(chain, entry) {
  const prevHash = chain.length ? chain[chain.length - 1].hash : GENESIS;
  const rec = {
    path: String(entry.path || ''),
    substrate: String(entry.substrate || ''),
    model: String(entry.model || ''),
    revision: String(entry.revision || ''),
    verdict: String(entry.verdict || ''),
    contentSha: String(entry.contentSha || ''),
    approver: String(entry.approver || ''),
    ts: entry.ts | 0,
    prevHash,
  };
  rec.hash = await sha256hex(canonical(rec) + prevHash);
  return [...chain, rec];
}

// verify(chain, { anchor }) → { ok, brokenAt? }. Recompute each link; if `anchor` is given the head
// hash must equal it (off-chain anchor defeats a full forge+reseal of the chain).
export async function verify(chain, { anchor = null } = {}) {
  let prev = GENESIS;
  for (let i = 0; i < chain.length; i++) {
    const r = chain[i];
    if (r.prevHash !== prev) return { ok: false, brokenAt: i, reason: 'broken-link' };
    const { hash, ...body } = r;
    const expect = await sha256hex(canonical(body) + prev);
    if (hash !== expect) return { ok: false, brokenAt: i, reason: 'bad-hash' };
    prev = hash;
  }
  if (anchor != null && chain.length && chain[chain.length - 1].hash !== anchor) return { ok: false, brokenAt: chain.length - 1, reason: 'anchor-mismatch' };
  return { ok: true };
}

export function head(chain) { return chain.length ? chain[chain.length - 1].hash : GENESIS; }
