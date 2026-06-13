// sheet-copilot.js — the ONE orchestrator the spreadsheet UI calls for an escalated turn. It unifies
// "one ANIMA, four substrates" behind a single handle(): the deterministic parser is the FLOOR (the
// caller runs it first and only calls here on a miss / forced dial), then an offline learned recipe
// replays, then — only for a genuinely new request — the best AVAILABLE generative substrate runs
// under the closed-action grammar, its output is RE-DERIVED by the deterministic engine, and a
// verified+applied plan is frozen into the offline recipe store so it never needs a model again.
//
// Honesty contract (load-bearing): a generative substrate only PROPOSES; nothing mutates the grid on
// a VETO; every applied turn carries a verdict the UI renders as icon+text. The existing, already-
// verified Grok (M3) and device (M1/M2) paths are NOT reimplemented here — handle() returns a
// 'delegate' decision for them so the proven code in index.html keeps running unchanged.
//
// Every dependency is INJECTED → pure & host-testable with a MockEngine + a mock sheet.
//
//   deps.engine    : { chat(messages,opts) -> {text} }                              (Mock or WebLLM)
//   deps.sheet     : { dims(), context(), dryRunAction(a)->{ok,error,expr,value},
//                      applyAction(a)->{ok}, snapshot()->{rows,headers,r1,c1,nrows,ncols},
//                      applyTransform(grid2d,loc)->{ok,changed} }
//   deps.sandbox   : { run(code, env, opts)->{ok,error,value,timeout} }             (nucleo-run)
//   deps.store     : { get(k), set(k,v) }                                           (localStorage)
//   deps.approve   : async ({kind,plan?,code?,diff?,verdict?}) -> bool              (human gate)
//   deps.caps      : { webgpu, coderReady, orchReady, online, vramMB }
//   deps.log       : step -> void
//   deps.ts        : number                                                         (no Date.now in core)

import { parseSheetActions } from './sheet-actions.js';
import { orchestratorMessages, coderMessages } from './sheet-engine.js';
import { planCoverage, combineVerdict } from './sheet-verify.js';
import { route, isGenerative } from './sheet-router.js';
import { recall, learn } from './sheet-learn.js';

const stripFences = (t) => { const m = /```(?:[a-z0-9]+)?\s*([\s\S]*?)```/i.exec(String(t || '')); return (m ? m[1] : String(t || '')).trim(); };

const NONEXEC = new Set(['done', 'answer', 'ask', 'transform_code']);

// Dry-run a plan into a verdict WITHOUT mutating the grid: each executable action is evaluated by the
// deterministic engine (an evaluation error ⇒ contradicted; a clean transform ⇒ confirmed), and the
// non-recompute-checkable ops (answer/ask/transform_code) count as uncovered.
function verifyPlan(sheet, actions) {
  const coverage = planCoverage(actions);
  const checks = [];
  for (const a of actions) {
    if (NONEXEC.has(a.op)) continue;                       // uncovered/control — handled by coverage
    let dry; try { dry = sheet.dryRunAction(a); } catch (e) { dry = { ok: false, error: String(e && e.message || e) }; }
    checks.push(dry && dry.ok === false ? { status: 'contradicted', evidence: { op: a.op, error: dry.error, expr: dry.expr } }
                                        : { status: 'confirmed', evidence: { op: a.op, expr: dry && dry.expr, value: dry && dry.value } });
  }
  const verdict = combineVerdict({ parsed: { ok: actions.length > 0, rejected: [] }, checks, coverage });
  return verdict;
}

// Canonical EXECUTION order: spreadsheet ops have dependencies, so a plan must run by dependency, not
// by phrasing order. "fai i totali poi ordina" literally would total-then-sort, which sorts the new
// totals row INTO the data (→ circular refs / scrambled). Structure & in-place ops first, then sort,
// then row-appending/derived ops — so totals/aggregates land on the settled, sorted data.
const EXEC_ORDER = { clean: 1, transform: 2, numfmt: 3, format: 3, rmempty: 4, dedupe: 5, sort: 6, fill: 7, highlight: 8, formula: 9, setcell: 9, aggregate: 10, total: 11, enrich: 12, chart: 13, describe: 13, insights: 13, find: 13, explain: 13, refresh: 13 };
export function orderForExec(actions) {
  return actions.map((a, i) => ({ a, i })).sort((x, y) => ((EXEC_ORDER[x.a.op] || 50) - (EXEC_ORDER[y.a.op] || 50)) || (x.i - y.i)).map((o) => o.a);
}

// Apply the executable actions of a verified plan (the firewall already bounded them) in dependency
// order, over a region PINNED for the whole plan (so each step sees the same data block). Returns the
// list actually applied and any answer/ask text + a deferred transform_code spec.
function applyPlan(sheet, actions, log) {
  const applied = [];
  let answer = '', clarify = '', codeSpec = '';
  if (sheet.pinRegion) { try { sheet.pinRegion(); } catch { /* best-effort */ } }
  for (const a of orderForExec(actions)) {
    if (a.op === 'done') break;
    if (a.op === 'answer') { answer = a.text || ''; continue; }
    if (a.op === 'ask') { clarify = a.question || ''; continue; }
    if (a.op === 'transform_code') { codeSpec = codeSpec || a.spec || ''; continue; }   // first one → coder follow-up
    let r; try { r = sheet.applyAction(a); } catch (e) { r = { ok: false, error: String(e && e.message || e) }; }
    log({ state: 'APPLY', op: a.op, ok: !!(r && r.ok) });
    if (r && r.ok) applied.push(a);
  }
  return { applied, answer, clarify, codeSpec };
}

// Verify → apply → learn a plan of ALREADY-VALIDATED actions. Shared by the LLM orchestrator
// (M4-plan) AND the deterministic offline planner (plan-det) so both go through the identical
// honesty gate: nothing mutates on a veto, only a fully-grounded (pass) applied plan is learned.
export function executeValidatedPlan(query, actions, deps, substrate) {
  const { sheet, store, log = () => {}, ts = 0 } = deps;
  if (!actions || !actions.length) {                      // nothing valid → don't fabricate; let caller fall back
    return { kind: 'failed', substrate, verdict: { verdict: 'veto', reasons: ['no-valid-actions'] }, actions: [], applied: [] };
  }
  const verdict = verifyPlan(sheet, actions);
  log({ state: 'VERIFY', verdict: verdict.verdict, reasons: verdict.reasons });
  if (verdict.verdict === 'veto') return { kind: 'failed', substrate, verdict, actions, applied: [] };

  const { applied, answer, clarify, codeSpec } = applyPlan(sheet, actions, log);
  // Learn a recipe only from a substrate that NEEDED a model (M4-plan): the deterministic planner is
  // already instant+offline, so caching it adds nothing. Keeps the flywheel meaningful.
  let learned = null;
  if (verdict.verdict === 'pass' && applied.length && substrate !== 'plan-det') {
    const res = learn(store, { query, plan: applied, verdict, substrate, applied: true, ts });
    learned = res.learned; log({ state: 'LEARN', reason: res.reason });
  }
  return { kind: 'executed', substrate, verdict, actions, applied, answer, clarify, codeSpec, learned };
}

// ---- the M4-plan substrate: orchestrator emits a grammar-constrained plan → verify → apply → learn ----
async function runPlanSubstrate(query, deps, opts, substrate) {
  const { engine, sheet, log = () => {} } = deps;
  const ctx = sheet.context ? sheet.context() : {};
  const { messages, options } = orchestratorMessages(query, ctx);
  const out = await engine.chat(messages, options);
  const { actions, rejected } = parseSheetActions(out.text, sheet.dims ? sheet.dims() : {});
  log({ state: 'PLAN', got: actions.length, rejected: rejected.length });
  return executeValidatedPlan(query, actions, deps, substrate);
}

// ---- the M4-code substrate: coder writes a pure JS transform → sandbox dry-run → run → staged diff ----
async function runCodeSubstrate(query, deps, opts) {
  const { engine, sheet, sandbox, store, approve, log, ts = 0 } = deps;
  const snap = sheet.snapshot ? sheet.snapshot() : { rows: [], headers: [], nrows: 0, ncols: 0 };
  if (!snap.nrows) return { kind: 'failed', substrate: 'M4-code', verdict: { verdict: 'veto', reasons: ['empty-selection'] } };

  const { messages, options } = coderMessages(query, { nrows: snap.nrows, ncols: snap.ncols, headers: snap.headers });
  const gen = await engine.chat(messages, options);
  const body = stripFences(gen.text);
  // wrap so the transform body sees rows/headers and its returned 2D array is what the run resolves to
  const code = `const rows = ${JSON.stringify(snap.rows)};\nconst headers = ${JSON.stringify(snap.headers)};\n` +
               `const __out = await (async () => {\n${body}\n})();\nreturn __out;`;

  // 1) parse-only gate (host-safe), 2) hermetic run with ALL capabilities denied (pure compute)
  const chk = await sandbox.run(code, {}, { mode: 'check' });
  if (!(chk && chk.ok)) { log({ state: 'CHECK', ok: false, error: chk && chk.error }); return { kind: 'failed', substrate: 'M4-code', verdict: { verdict: 'veto', reasons: ['syntax:' + (chk && chk.error || '')] }, code }; }
  const run = await sandbox.run(code, {}, { caps: { fs: false, http: false, anima: false } });
  log({ state: 'RUN', ok: !!(run && run.ok), error: run && run.error });
  if (!(run && run.ok)) return { kind: 'failed', substrate: 'M4-code', verdict: { verdict: 'veto', reasons: ['run:' + (run && (run.error || run.timeout && 'timeout') || '')] }, code };

  // parse the returned grid; require same shape (a custom transform can't be auto-verified for
  // CORRECTNESS, only that it ran hermetically and preserved shape → honest WARN, human approves).
  let grid = null; try { grid = typeof run.value === 'string' ? JSON.parse(run.value) : run.value; } catch { grid = null; }
  const shapeOk = Array.isArray(grid) && grid.length === snap.nrows && grid.every((r) => Array.isArray(r) && r.length === snap.ncols);
  if (!shapeOk) return { kind: 'failed', substrate: 'M4-code', verdict: { verdict: 'veto', reasons: ['shape-mismatch'] }, code };

  const verdict = { verdict: 'warn', reasons: ['custom-transform-unverified'], checks: [], coverage: { found: 1, checkable: 0, uncovered: 1 } };
  const ok = approve ? await approve({ kind: 'transform', code, diff: { before: snap.rows, after: grid }, verdict }) : false;
  log({ state: 'AWAIT_APPROVAL', approved: !!ok });
  if (!ok) return { kind: 'rejected', substrate: 'M4-code', verdict, code, grid };

  const ap = sheet.applyTransform ? sheet.applyTransform(grid, { r1: snap.r1, c1: snap.c1 }) : { ok: false };
  log({ state: 'APPLY', op: 'transform_code', ok: !!(ap && ap.ok) });
  // a warn transform is NOT learned (honesty: only fully-grounded plans become offline recipes)
  return { kind: 'executed', substrate: 'M4-code', verdict, code, grid, applied: ap && ap.ok ? [{ op: 'transform_code' }] : [] };
}

// ---- replay a previously-verified offline recipe (no model, no network) ----
function replayRecipe(recipe, deps) {
  const { sheet, log } = deps;
  const { applied } = applyPlan(sheet, recipe.plan || [], log);
  log({ state: 'REPLAY', id: recipe.id, applied: applied.length });
  return { kind: 'executed', substrate: 'recipe', verdict: { verdict: 'pass', reasons: ['learned-recipe'] }, actions: recipe.plan, applied, recipe };
}

// handle(query, ctx) → a decision object the caller acts on.
//   ctx: { dial:'auto'|'off'|'on'|'only'|'local', lang, localHit:bool }
// Returns:
//   { kind:'executed'|'failed'|'rejected', substrate, verdict, ... }   — the orchestrator did the work
//   { kind:'delegate', substrate:'M3'|'M2'|'M1', to:'grok'|'remote', mode }  — caller runs its proven path
export async function handle(query, ctx = {}, deps = {}) {
  const { dial = 'auto', localHit = false } = ctx;
  const caps = deps.caps || {};
  const store = deps.store;
  const log = deps.log || (() => {});
  const ts = deps.ts || 0;

  // offline recipe lookup (skip when the deterministic parser already understood, or dial is cloud-only)
  let recipeHit = null;
  if (!localHit && dial !== 'only' && dial !== 'on' && store) {
    const r = recall(store, query, ts);
    if (r) recipeHit = r.recipe;
  }

  const decision = route({ text: query, dial, caps, localHit, recipeHit: !!recipeHit });
  log({ state: 'ROUTE', substrate: decision.substrate, reason: decision.reason });

  switch (decision.substrate) {
    case 'det':    return { kind: 'delegate', substrate: 'det', to: 'deterministic' };      // caller runs localIntent
    case 'recipe': return replayRecipe(recipeHit, deps);
    case 'M4-plan': {
      const r = await runPlanSubstrate(query, deps, ctx, 'M4-plan');
      if (r.kind === 'failed') {                                                            // graceful cascade down the ladder
        if (caps.online) return { kind: 'delegate', substrate: 'M3', to: 'grok', fellBack: 'M4-plan' };
        return { kind: 'delegate', substrate: caps.online ? 'M2' : 'M1', to: 'remote', mode: caps.online ? 'on' : 'off', fellBack: 'M4-plan' };
      }
      return r;
    }
    case 'M4-code': {
      const r = await runCodeSubstrate(query, deps, ctx);
      if (r.kind === 'failed' && caps.online) return { kind: 'delegate', substrate: 'M3', to: 'grok', fellBack: 'M4-code' };
      return r;
    }
    case 'M3': return { kind: 'delegate', substrate: 'M3', to: 'grok' };
    case 'M2': return { kind: 'delegate', substrate: 'M2', to: 'remote', mode: 'on' };
    case 'M1':
    default:   return { kind: 'delegate', substrate: 'M1', to: 'remote', mode: 'off' };
  }
}

// re-exports for the UI layer
export { route, isGenerative } from './sheet-router.js';
export { provenance, verdictChip } from './sheet-verify.js';
export { recall, learn, forget, normQuery } from './sheet-learn.js';
