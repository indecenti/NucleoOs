// loop.js — the Antigravity-style agentic loop, as an explicit state machine with every dependency
// INJECTED (engine, fs, sandbox, deviceVerify, approve) so it is fully deterministic in CI.
// Security invariants enforced HERE (must-fix #4), not by trust in the model:
//   • NO file mutation happens without a preceding approve() that returned true (staged diff).
//   • generated code is dry-run HERMETICALLY (fs/http/anima denied) before it can be applied.
//   • on reject, the loop never auto-applies; it offers a FIX turn instead.
//   • a hard step budget bounds the loop.
// Pure logic (no DOM, no real I/O — all via injected deps) → host-testable with a MockEngine.

import { parseActions, isMutating } from './actions.js';
import { extract } from './extract.js';
import { combineVerdict } from './verify.js';
import { assess as capAssess } from './capguard.js';
import { pickBest } from './selfcheck.js';
import { distill } from './learn.js';

const stripFences = (t) => { const m = /```(?:[a-z0-9]+)?\s*([\s\S]*?)```/i.exec(String(t || '')); return (m ? m[1] : String(t || '')).trim(); };

// deps:
//   engine.chat(messages, opts) -> {text}
//   fs: { read, write, append, edit, del|delete, mkdir, move, list }  (fsclient-shaped, async)
//   sandbox.run(code, {caps, mode}) -> {ok, error, value, timeout?}    (mode:'check' = parse-only)
//   deviceVerify(artifact) -> {checks:[{claim,status}]}                (async; null when offline)
//   approve({path?, op?, before?, after?, action?, verdict?}) -> bool  (human-in-the-loop)
//   log(step) -> void
// opts: { budget=8, root='/data/ws', lang='it' }
export async function runAgent(goal0, deps, opts = {}) {
  const { engine, fs, sandbox, deviceVerify = null, approve, log = () => {} } = deps;
  const budget = opts.budget || 8;
  const root = opts.root || '/data/ws';
  const trace = [], writes = [];
  let goal = String(goal0 || ''), status = 'running', steps = 0, lastFollowups = [];
  const step = (s) => { trace.push(s); log(s); };
  const del = (p) => (fs.del ? fs.del(p) : fs.delete(p));

  step({ state: 'PLAN', goal });
  while (steps < budget && status === 'running') {
    steps++;
    const plan = await engine.chat([{ role: 'system', content: 'plan' }, { role: 'user', content: goal }], { schema: 'actions', phase: 'plan' });
    const { actions, rejected } = parseActions(plan.text, { root });
    step({ state: 'ACTIONS', got: actions.length, rejected: rejected.length });
    if (!actions.length) { status = 'abort'; step({ state: 'ABORT', reason: 'no-valid-actions' }); break; }

    for (const a of actions) {
      if (status !== 'running') break;

      if (a.op === 'done') { status = 'done'; step({ state: 'DONE', summary: a.summary || '' }); break; }
      if (a.op === 'answer') { step({ state: 'ANSWER', text: a.text }); continue; }
      if (a.op === 'read') { const r = await fs.read(a.path); step({ state: 'READ', path: a.path, ok: !!(r && r.ok) }); continue; }
      if (a.op === 'list') { const r = await fs.list(a.path || '.'); step({ state: 'LIST', ok: !!(r && r.ok) }); continue; }

      if (a.op === 'synthesize') {
        // BEST-OF-N grounded synthesis (test-time compute): generate N candidates, evaluate each
        // hermetically (dry-run check + capability/danger guard + coverage + device grounded checks),
        // and pick the best ADMISSIBLE one by grounded signal — never the model's self-report.
        const N = Math.max(1, opts.candidates || 1);
        const granted = opts.granted || null, allowNetwork = !!opts.allowNetwork;
        const evald = [];
        for (let k = 0; k < N; k++) {
          const out = await engine.chat([{ role: 'system', content: 'coder' }, { role: 'user', content: a.spec }], { phase: 'synthesize', sample: k });
          const code = stripFences(out.text);
          const run = await sandbox.run(code, { caps: { fs: false, http: false, anima: false }, mode: 'check' });
          const cg = (opts.capguard === false) ? null : capAssess(code, { granted, allowNetwork });
          const ex = extract({ code });
          const checks = deviceVerify ? ((await deviceVerify({ code })).checks || []) : [];
          const verdict = combineVerdict({ run, checks, coverage: ex.coverage, capguard: cg });
          evald.push({ code, run, verdict, caps: cg ? cg.capabilities.length : 0, bytes: code.length });
        }
        const candidates = evald.map((e) => ({ code: e.code, check: { ok: !!(e.run && e.run.ok) }, verdict: e.verdict.verdict, caps: e.caps, bytes: e.bytes, tests: { passed: e.run && e.run.ok ? 1 : 0, total: 1 } }));
        const best = pickBest(candidates);
        const chosen = best ? evald.find((e) => e.code === best.code) : evald[0];
        const verdict = chosen ? chosen.verdict : { verdict: 'veto', reasons: ['no-candidate'] };
        step({ state: 'VERIFY', verdict: verdict.verdict, reasons: verdict.reasons, candidates: N });

        if (!best) {                                            // FIX turn — never apply a non-admissible artifact
          step({ state: 'FIX', reason: (verdict.reasons || []).join('; ') });
          goal = goal + `\n\n[verifier ${verdict.verdict}: ${(verdict.reasons || []).join('; ')}] correct the code.`;
          continue;
        }
        const code = best.code;

        // SELF-TEST (Claude-Code-style "is it actually CORRECT?"): when enabled, the agent WRITES
        // assertions for its own code, RUNS code+tests, and FIXES on failure — catching WRONG output
        // that merely "ran". A passing static/parse check is never trusted as behavioural correctness.
        if (opts.test) {
          const tg = await engine.chat([{ role: 'system', content: 'tests' }, { role: 'user', content: `Write runnable assertions (throw on failure) for: ${a.spec}\n\nCODE:\n${code}` }], { phase: 'test' });
          const testCode = stripFences(tg.text);
          if (testCode) {
            const tr = await sandbox.run(code + '\n;\n' + testCode, { caps: { fs: false, http: false, anima: false } });
            step({ state: 'TEST', ok: !!(tr && tr.ok), error: tr && tr.error, output: tr && (Array.isArray(tr.logs) ? tr.logs.join('\n') : undefined) });
            if (!(tr && tr.ok)) {
              step({ state: 'FIX', reason: 'tests-failed: ' + ((tr && tr.error) || '') });
              goal = goal + `\n\n[self-test FAILED: ${(tr && tr.error) || ''}] the code ran but produced WRONG results — correct it so the assertions pass.`;
              continue;
            }
          }
        }

        // STAGED DIFF → human approval. The ONLY path to a write.
        const target = a.path || 'out.js';
        const cur = await fs.read(target).then((r) => r).catch(() => ({ ok: false }));
        const before = (cur && cur.ok) ? cur.content : '';
        const approved = await approve({ path: target, before, after: code, verdict });
        step({ state: 'AWAIT_APPROVAL', path: target, approved: !!approved });
        if (!approved) { step({ state: 'REJECTED', path: target }); continue; }

        const w = await fs.write(target, code);
        if (w && w.ok) writes.push(target);
        step({ state: 'APPLY', path: target, ok: !!(w && w.ok) });

        // PROVENANCE — append a tamper-evident record of who/what wrote this (if a ledger is wired).
        if (deps.provenance && Array.isArray(deps.ledger)) {
          deps.ledger = await deps.provenance.append(deps.ledger, {
            path: target, substrate: 'M4-local', model: opts.model || '', revision: opts.revision || '',
            verdict: verdict.verdict, contentSha: await deps.provenance.sha256hex(code), approver: 'human', ts: opts.ts || 0,
          });
          step({ state: 'PROVENANCE', entries: deps.ledger.length });
        }

        // OBSERVE — run the applied code (mutation denied), CAPTURING the real console output / return
        // value and feeding it back on failure (Claude-Code-style read-the-output-then-decide).
        const obs = await sandbox.run(code, { caps: { fs: false, http: false, anima: false } });
        const out = obs ? (Array.isArray(obs.logs) ? obs.logs.join('\n') : (obs.value !== undefined ? String(obs.value) : '')) : '';
        step({ state: 'RUN', ok: !!(obs && obs.ok), error: obs && obs.error, output: out });
        if (!(obs && obs.ok)) { step({ state: 'FIX', reason: 'run-error: ' + ((obs && obs.error) || '') }); goal = goal + `\n\n[run error: ${(obs && obs.error) || ''} | output so far: ${out}] fix it.`; continue; }

        // SILENT LEARNING FLYWHEEL — distil a CERTAIN+USEFUL recipe into the offline corpus.
        // Fires only on verdict==='pass' + approved + ran + provenance present; the caller persists
        // deps.staged (one JSONL line per card) for build_akb2.py to compile into the device index.
        // M4's verified generative output silently, auditably teaches M1's grounded brain.
        if (Array.isArray(deps.staged)) {
          const provHash = (deps.provenance && Array.isArray(deps.ledger) && deps.ledger.length) ? deps.provenance.head(deps.ledger) : '';
          const d = distill({ spec: a.spec, code, verdict, approved: true, ranOk: true, substrate: 'M4-local', provenanceHash: provHash, lang: opts.lang || 'it' },
            { existingCards: deps.cards || [], stagedCards: deps.staged });
          if (d.staged) { deps.staged.push(d.staged); step({ state: 'LEARN', id: d.staged.id }); }
          else step({ state: 'LEARN-SKIP', reason: d.reason });
        }

        // DONE — propose the next agentic steps, the way Claude Code offers follow-ups.
        lastFollowups = ['run', 'improve', 'add-tests', 'explain', 'edit'];
        step({ state: 'SUGGEST', followups: lastFollowups });
        status = 'done'; step({ state: 'DONE', path: target, output: out, followups: lastFollowups });
        break;
      }

      // Generic mutating ops proposed directly by the planner also go through approval.
      if (isMutating(a.op)) {
        const approved = await approve({ op: a.op, action: a });
        step({ state: 'AWAIT_APPROVAL', op: a.op, approved: !!approved });
        if (!approved) { step({ state: 'REJECTED', op: a.op }); continue; }
        const r = await applyOp(fs, a, del);
        if (r && r.ok) writes.push(a.path || a.to);
        step({ state: 'APPLY', op: a.op, ok: !!(r && r.ok) });
        continue;
      }
      // read-only ops (tree/search/glob/cd/run/ask/verify/route) are observational here.
      step({ state: 'OP', op: a.op });
    }
  }
  if (status === 'running') { status = 'abort'; step({ state: 'ABORT', reason: 'step-budget' }); }
  return { status, steps, trace, writes, followups: lastFollowups };
}

async function applyOp(fs, a, del) {
  switch (a.op) {
    case 'write':  return fs.write(a.path, a.content || '');
    case 'append': return fs.append(a.path, a.content || '');
    case 'edit':   return fs.edit(a.path, a.old, a.new, { all: a.all });
    case 'delete': return del(a.path);
    case 'mkdir':  return fs.mkdir(a.path);
    case 'move':   return fs.move(a.from, a.to);
    default:       return { ok: false };
  }
}
