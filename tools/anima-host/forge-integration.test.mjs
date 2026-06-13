// Gate: ANIMA Forge — END-TO-END INTEGRATION. Wires the REAL modules (loop + capguard + extract +
// verify + selfcheck + provenance + learn) with a real parse/exec sandbox and a scripted MockEngine
// (the GPU model is the only stub), then runs the BUILD-TIME promotion gate over what was learned.
// Proves the 18-module spine composes into one coherent offline agentic session — and that the
// safety invariants hold across the whole pipeline, not just per unit.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { runAgent } from '../../apps/anima/www/forge/loop.js';
import { MockEngine } from '../../apps/anima/www/forge/engine.js';
import * as provenance from '../../apps/anima/www/forge/provenance.js';
import { checkSyntax } from '../../apps/code-runner/www/nucleo-run.js';
import { promote } from '../anima/promote-learned.mjs';

// in-memory workspace fs (fsclient-shaped subset the loop uses)
function mkFs() {
  const store = new Map(); const log = [];
  return {
    store, log,
    read: async (p) => (store.has(p) ? { ok: true, content: store.get(p) } : { ok: false, error: 'not-found' }),
    list: async () => ({ ok: true, entries: [] }),
    write: async (p, c) => { store.set(p, c); log.push('write:' + p); return { ok: true }; },
    append: async (p, c) => { store.set(p, (store.get(p) || '') + c); return { ok: true }; },
    edit: async (p, o, n) => { store.set(p, String(store.get(p) || '').replace(o, n)); return { ok: true }; },
    del: async (p) => { store.delete(p); return { ok: true }; },
    mkdir: async () => ({ ok: true }), move: async () => ({ ok: true }),
  };
}

// REAL sandbox: mode:'check' = parse-only (checkSyntax, no exec); otherwise actually EXECUTE the
// generated code on the host with I/O denied and console captured — a genuine edit→run→observe.
function mkSandbox() {
  const AsyncFn = Object.getPrototypeOf(async function () {}).constructor;
  return {
    run: async (code, opts = {}) => {
      if (opts.mode === 'check') return checkSyntax(code);
      const logs = [];
      const con = { log: (...a) => logs.push(a.join(' ')), info() {}, warn() {}, error() {}, debug() {} };
      const denied = () => { throw new Error('capability denied'); };
      const os = { fs: { read: denied, write: denied, append: denied, list: denied, remove: denied, mkdir: denied, exists: denied }, http: { get: denied, json: denied }, anima: denied, notify: denied };
      try { const fn = new AsyncFn('os', 'console', 'print', 'args', 'env', '"use strict";\n' + code); await fn(os, con, (...a) => logs.push(a.join(' ')), [], {}); return { ok: true, logs }; }
      catch (e) { return { ok: false, error: String((e && e.message) || e), logs }; }
    },
  };
}

const plan = (actions) => JSON.stringify({ actions });

test('FULL SESSION: plan → best-of-N synth → verify → approve → apply → run → provenance → learn → promote', async () => {
  const fs = mkFs(), sandbox = mkSandbox();
  const ledger = [], staged = [];
  const engine = new MockEngine((m, o) => {
    if (o.phase === 'plan') return plan([{ op: 'synthesize', spec: 'reverse a string in javascript', path: 'rev.js' }]);
    if (o.phase === 'synthesize') {
      return o.sample === 0
        ? '```js\nfunction reverse(s){ return BROKEN(\n```'                                   // candidate 0: won't parse
        : '```js\nfunction reverse(s){ return [...s].reverse().join(""); }\nconsole.log(reverse("abc"));\n```'; // candidate 1: good
    }
    return '';
  });
  const deps = { engine, fs, sandbox, approve: async () => true, deviceVerify: null, provenance, ledger, staged, cards: [] };
  const r = await runAgent('reverse a string', deps, { budget: 5, candidates: 2, model: 'qwen2.5-coder-1.5b', revision: 'rev1', ts: 1, lang: 'en' });

  assert.equal(r.status, 'done');
  assert.equal(deps.ledger.length, 1, 'an applied artifact gets a provenance record');
  assert.equal((await provenance.verify(deps.ledger)).ok, true, 'provenance chain verifies');
  assert.ok(fs.store.has('rev.js'), 'the verified candidate was applied to the workspace');
  assert.match(fs.store.get('rev.js'), /\.reverse\(\)/);
  assert.equal(staged.length, 1, 'a certain (pass+approved+ran) recipe is silently learned');
  assert.equal(staged[0].provenance, deps.ledger[0].hash, 'the learned card links its provenance hash');

  // BUILD-TIME promotion against a different-topic corpus → the learned recipe ships clean.
  const corpus = [{ id: 'geo.france', category: 'knowledge', ask: { en: ['what is the capital of france'] }, reply: { en: 'Paris' } }];
  const { promoted, rejected } = promote(staged, corpus);
  assert.equal(promoted.length, 1, 'the learned recipe passes the conservative cross-corpus gate');
  assert.equal(rejected.length, 0);
});

test('SAFETY across the pipeline: a dangerous artifact is vetoed, never applied, never learned', async () => {
  const fs = mkFs(), sandbox = mkSandbox();
  const ledger = [], staged = [];
  let planN = 0;
  const engine = new MockEngine((m, o) => {
    if (o.phase === 'plan') { planN++; return planN === 1 ? plan([{ op: 'synthesize', spec: 'do something in javascript', path: 'x.js' }]) : plan([{ op: 'done' }]); }
    if (o.phase === 'synthesize') return '```js\neval(userInput)\n```';   // capguard BLOCK → veto
    return '';
  });
  const deps = { engine, fs, sandbox, approve: async () => true, deviceVerify: null, provenance, ledger, staged, cards: [] };
  const r = await runAgent('do something', deps, { budget: 4, candidates: 1, ts: 1, lang: 'en' });

  assert.equal(fs.store.size, 0, 'nothing written');
  assert.equal(ledger.length, 0, 'no provenance for a vetoed artifact');
  assert.equal(staged.length, 0, 'a vetoed artifact is NEVER learned — only certain things become knowledge');
  assert.ok(r.trace.some((s) => s.state === 'VERIFY' && s.verdict === 'veto'));
});

test('LEARNED-then-rejected: re-staging the same recipe is idempotent (no double-learning)', async () => {
  // Two identical certain sessions; the second is a duplicate of the first within the session corpus.
  const staged = [];
  const ledger = [];
  async function once() {
    const fs = mkFs(), sandbox = mkSandbox();
    const engine = new MockEngine((m, o) => {
      if (o.phase === 'plan') return plan([{ op: 'synthesize', spec: 'clamp a number between min and max in javascript', path: 'clamp.js' }]);
      if (o.phase === 'synthesize') return '```js\nconst clamp=(n,a,b)=>Math.min(b,Math.max(a,n));\nconsole.log(clamp(5,0,3));\n```';
      return '';
    });
    const cards = staged.map((c) => ({ id: c.id, ask: c.ask, reply: c.reply, detail: c.detail, category: c.category }));
    const deps = { engine, fs, sandbox, approve: async () => true, deviceVerify: null, provenance, ledger, staged, cards };
    await runAgent('clamp', deps, { budget: 4, candidates: 1, model: 'm', revision: 'r', ts: 1, lang: 'en' });
  }
  await once();
  assert.equal(staged.length, 1, 'first session learns the recipe');
  await once();
  assert.equal(staged.length, 1, 'second identical session does NOT double-learn (dedup vs session corpus)');
});
