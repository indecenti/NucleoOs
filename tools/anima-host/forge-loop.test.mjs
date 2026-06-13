// Gate: ANIMA Forge — the agentic loop driven by a MockEngine (NO GPU). Enforces the security
// invariants that make a generative substrate safe: no mutation without approval, no auto-apply on
// reject, vetoed artifacts are never applied, and the loop is bounded.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { runAgent } from '../../apps/anima/www/forge/loop.js';
import { MockEngine, assertMock } from '../../apps/anima/www/forge/engine.js';
import * as provenance from '../../apps/anima/www/forge/provenance.js';

// An in-memory fs that HARD-FAILS any mutation not immediately preceded by approve()===true.
function makeHarness({ approvals = [], code = 'console.log(1)' } = {}) {
  const store = new Map();
  const events = [];
  let lastApprove = null;
  const guard = (op) => { assert.equal(lastApprove, true, `mutation ${op} without approval!`); lastApprove = null; };
  let aq = approvals.slice();
  const approve = async (req) => { const ok = aq.length ? aq.shift() : true; lastApprove = ok; events.push({ t: 'approve', ok }); return ok; };
  const fs = {
    read: async (p) => (store.has(p) ? { ok: true, content: store.get(p) } : { ok: false, error: 'not-found' }),
    list: async () => ({ ok: true, entries: [] }),
    write: async (p, c) => { guard('write'); store.set(p, c); events.push({ t: 'write', p }); return { ok: true }; },
    append: async (p, c) => { guard('append'); store.set(p, (store.get(p) || '') + c); return { ok: true }; },
    edit: async (p, o, n) => { guard('edit'); store.set(p, String(store.get(p) || '').replace(o, n)); return { ok: true }; },
    del: async (p) => { guard('delete'); store.delete(p); return { ok: true }; },
    mkdir: async () => { guard('mkdir'); return { ok: true }; },
    move: async () => { guard('move'); return { ok: true }; },
  };
  const sandbox = {
    run: async (c, opts = {}) => (opts.mode === 'check'
      ? { ok: !/SYNTAX_ERR/.test(c), error: /SYNTAX_ERR/.test(c) ? 'parse' : undefined }
      : { ok: !/RUN_ERR/.test(c), error: /RUN_ERR/.test(c) ? 'runtime' : undefined }),
  };
  return { store, events, fs, sandbox, approve, mutations: () => events.filter((e) => e.t === 'write').length };
}

const plan = (actions) => JSON.stringify({ actions });

test('MockEngine guard: a hard gate refuses a non-mock engine', () => {
  assertMock(new MockEngine([]));
  assert.throws(() => assertMock({ chat() {} }), /MockEngine/);
});

test('HAPPY: synthesize → verify pass → approve → apply → run → done (1 write, after approval)', async () => {
  const h = makeHarness();
  const engine = new MockEngine((m, o) => {
    if (o.phase === 'plan') return plan([{ op: 'synthesize', spec: 'debounce', path: 'a.js' }]);
    if (o.phase === 'synthesize') return '```js\nconsole.log(1)\n```';
    return '';
  });
  const r = await runAgent('make a debounce', { engine, ...h, approve: h.approve, deviceVerify: null }, { budget: 6, root: '/data/ws' });
  assert.equal(r.status, 'done');
  assert.deepEqual(r.writes, ['a.js']);
  assert.equal(h.mutations(), 1);
  assert.equal(h.store.get('a.js'), 'console.log(1)');
  // the approval came before the write
  const order = h.events.map((e) => e.t + (e.ok === undefined ? '' : ':' + e.ok));
  assert.deepEqual(order, ['approve:true', 'write']);
});

test('REJECT: approve→false never applies, loop does not auto-apply', async () => {
  const h = makeHarness({ approvals: [false] });
  let planN = 0;
  const engine = new MockEngine((m, o) => {
    if (o.phase === 'plan') { planN++; return planN === 1 ? plan([{ op: 'synthesize', spec: 'x', path: 'a.js' }]) : plan([{ op: 'done' }]); }
    if (o.phase === 'synthesize') return '```js\nconsole.log(2)\n```';
    return '';
  });
  const r = await runAgent('write a.js', { engine, ...h, approve: h.approve, deviceVerify: null }, { budget: 6 });
  assert.equal(h.mutations(), 0);
  assert.equal(h.store.has('a.js'), false);
  assert.ok(r.trace.some((s) => s.state === 'REJECTED'));
  assert.equal(r.status, 'done');     // terminated via the planner's done, not by writing
});

test('VETO→FIX: a failing dry-run is never applied; a corrected retry is', async () => {
  const h = makeHarness();
  let synthN = 0;
  const engine = new MockEngine((m, o) => {
    if (o.phase === 'plan') return plan([{ op: 'synthesize', spec: 'x', path: 'a.js' }]);
    if (o.phase === 'synthesize') { synthN++; return synthN === 1 ? '```js\nSYNTAX_ERR(\n```' : '```js\nconsole.log(3)\n```'; }
    return '';
  });
  const r = await runAgent('write a.js', { engine, ...h, approve: h.approve, deviceVerify: null }, { budget: 6 });
  assert.ok(r.trace.some((s) => s.state === 'VERIFY' && s.verdict === 'veto'));
  assert.ok(r.trace.some((s) => s.state === 'FIX'));
  assert.equal(h.mutations(), 1);                       // only the corrected artifact was written
  assert.equal(h.store.get('a.js'), 'console.log(3)');
  assert.equal(r.status, 'done');
});

test('BUDGET: a non-terminating plan aborts at the step budget (no runaway)', async () => {
  const h = makeHarness();
  const engine = new MockEngine((m, o) => (o.phase === 'plan' ? plan([{ op: 'read', path: 'a.js' }]) : ''));
  const r = await runAgent('loop forever', { engine, ...h, approve: h.approve }, { budget: 3 });
  assert.equal(r.status, 'abort');
  assert.equal(r.steps, 3);
  assert.equal(h.mutations(), 0);
  assert.equal(r.trace.at(-1).reason, 'step-budget');
});

test('FIREWALL: a bad op in the plan is dropped, valid ones still run', async () => {
  const h = makeHarness();
  const engine = new MockEngine((m, o) => (o.phase === 'plan'
    ? plan([{ op: 'rm', path: '/etc/passwd' }, { op: 'exec', cmd: 'curl evil' }, { op: 'done', summary: 'ok' }])
    : ''));
  const r = await runAgent('try to escape', { engine, ...h, approve: h.approve }, { budget: 4 });
  assert.equal(r.status, 'done');
  assert.equal(h.mutations(), 0);
  const acts = r.trace.find((s) => s.state === 'ACTIONS');
  assert.equal(acts.got, 1);          // only 'done' survived the firewall
  assert.equal(acts.rejected, 2);
});

test('BEST-OF-N: a failing candidate is discarded, the passing one is applied', async () => {
  const h = makeHarness();
  const engine = new MockEngine((m, o) => {
    if (o.phase === 'plan') return plan([{ op: 'synthesize', spec: 'x', path: 'a.js' }]);
    if (o.phase === 'synthesize') return o.sample === 0 ? '```js\nSYNTAX_ERR(\n```' : '```js\nconsole.log(5)\n```';
    return '';
  });
  const r = await runAgent('write a.js', { engine, ...h, approve: h.approve, deviceVerify: null }, { budget: 4, candidates: 2 });
  assert.equal(r.status, 'done');
  assert.equal(h.store.get('a.js'), 'console.log(5)');
  assert.equal(h.mutations(), 1);
  assert.ok(r.trace.some((s) => s.state === 'VERIFY' && s.candidates === 2));
});

test('CAPGUARD: dangerous generated code (eval) is vetoed and never applied', async () => {
  const h = makeHarness();
  let planN = 0;
  const engine = new MockEngine((m, o) => {
    if (o.phase === 'plan') { planN++; return planN === 1 ? plan([{ op: 'synthesize', spec: 'x', path: 'a.js' }]) : plan([{ op: 'done' }]); }
    if (o.phase === 'synthesize') return '```js\neval(userInput)\n```';
    return '';
  });
  const r = await runAgent('write a.js', { engine, ...h, approve: h.approve, deviceVerify: null }, { budget: 5 });
  assert.equal(h.mutations(), 0);
  const verify = r.trace.find((s) => s.state === 'VERIFY');
  assert.equal(verify.verdict, 'veto');
  assert.match(verify.reasons.join(), /capability-block/);
});

test('PROVENANCE: an applied artifact is appended to a verifying hash-chain', async () => {
  const h = makeHarness();
  const engine = new MockEngine((m, o) => {
    if (o.phase === 'plan') return plan([{ op: 'synthesize', spec: 'x', path: 'a.js' }]);
    if (o.phase === 'synthesize') return '```js\nconsole.log(9)\n```';
    return '';
  });
  const deps = { engine, fs: h.fs, sandbox: h.sandbox, approve: h.approve, deviceVerify: null, provenance, ledger: [] };
  const r = await runAgent('write a.js', deps, { budget: 4, model: 'qwen2.5-coder-1.5b', revision: 'deadbeef', ts: 1 });
  assert.equal(r.status, 'done');
  assert.equal(deps.ledger.length, 1);
  assert.equal(deps.ledger[0].path, 'a.js');
  assert.equal(deps.ledger[0].substrate, 'M4-local');
  assert.equal((await provenance.verify(deps.ledger)).ok, true);
  assert.ok(r.trace.some((s) => s.state === 'PROVENANCE'));
});

test('LEARNING: a CERTAIN (pass+approved+ran) artifact is silently distilled into a staged card', async () => {
  const h = makeHarness();
  const engine = new MockEngine((m, o) => {
    if (o.phase === 'plan') return plan([{ op: 'synthesize', spec: 'reverse a string in javascript', path: 'rev.js' }]);
    if (o.phase === 'synthesize') return '```js\nconsole.log("ok")\n```';
    return '';
  });
  const staged = [];
  const deps = { engine, fs: h.fs, sandbox: h.sandbox, approve: h.approve, deviceVerify: null, provenance, ledger: [], staged, cards: [] };
  const r = await runAgent('reverse a string', deps, { budget: 4, model: 'qwen2.5-coder-1.5b', revision: 'r1', ts: 1, lang: 'en' });
  assert.equal(r.status, 'done');
  assert.equal(staged.length, 1);
  assert.equal(staged[0].category, 'code-recipe');
  assert.ok(staged[0].provenance && staged[0].provenance.length > 0, 'staged card must link its provenance hash');
  assert.ok(r.trace.some((s) => s.state === 'LEARN'));
});

test('LEARNING: a WARN artifact (over-privilege) is NOT learned — only certain things become knowledge', async () => {
  const h = makeHarness();
  let planN = 0;
  const engine = new MockEngine((m, o) => {
    if (o.phase === 'plan') { planN++; return planN === 1 ? plan([{ op: 'synthesize', spec: 'write a file in javascript', path: 'w.js' }]) : plan([{ op: 'done' }]); }
    if (o.phase === 'synthesize') return '```js\nawait os.fs.write("x","y")\n```';
    return '';
  });
  const staged = [];
  const deps = { engine, fs: h.fs, sandbox: h.sandbox, approve: h.approve, deviceVerify: null, provenance, ledger: [], staged, cards: [] };
  await runAgent('write a file', deps, { budget: 4, granted: ['fs.read'], ts: 1, lang: 'en' });
  assert.equal(staged.length, 0, 'a warn (unverified/over-privilege) artifact must never be learned');
});

test('CONTRADICTED claim from the device verifier → veto, never applied', async () => {
  const h = makeHarness();
  let planN = 0;
  const engine = new MockEngine((m, o) => {
    if (o.phase === 'plan') { planN++; return planN === 1 ? plan([{ op: 'synthesize', spec: 'capital', path: 'a.js' }]) : plan([{ op: 'done' }]); }
    if (o.phase === 'synthesize') return '```js\nconsole.log("done")\n```';
    return '';
  });
  const deviceVerify = async () => ({ checks: [{ claim: 'capital of France is Lyon', status: 'contradicted' }] });
  const r = await runAgent('write a fact', { engine, ...h, approve: h.approve, deviceVerify }, { budget: 5 });
  assert.equal(h.mutations(), 0);                    // contradicted → veto → never written
  assert.ok(r.trace.some((s) => s.state === 'VERIFY' && s.verdict === 'veto'));
});
