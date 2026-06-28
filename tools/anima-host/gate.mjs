#!/usr/bin/env node
// ANIMA unified gate — ONE command that runs every offline ANIMA check and reports a single
// pass/fail. Closes the "no single runner" gap: a contributor (or CI) runs `npm run anima:gate`
// instead of remembering eight separate commands. Builds the real cascade exe first, then runs:
//
//   regress.py        corpus hygiene + L1 retrieval at the DEVICE gate (dedup, in-dist recall, OOS FP)
//   route-check       orchestrator routing vs the golden snapshot (real exe)
//   agent-check       agentic loop: compose-then-act, content channel, settings, guards
//   math-check        every math answer exact + JS-twin parity
//   ood-check         end-to-end OOS safety on the real exe (0 false-positives) — parsed from output
//   kge / hdc / combinator-eval   the offline reasoning tiers (deduction, recall, composition)
//   node --test       the *.test.mjs suite (weather NLU, etc.)
//
// Exits non-zero if ANY hard gate fails. Run from the repo root: `npm run anima:gate`.
import { spawnSync } from 'node:child_process';
import { existsSync, rmSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..');
const exe = join(here, 'build', 'anima.exe');
const C = { g: '\x1b[32m', r: '\x1b[31m', y: '\x1b[33m', b: '\x1b[1m', d: '\x1b[2m', x: '\x1b[0m' };

function run(cmd, args, { cwd = repo, env = {} } = {}) {
  const res = spawnSync(cmd, args, {
    cwd, encoding: 'utf8', env: { ...process.env, PYTHONUTF8: '1', PYTHONIOENCODING: 'utf-8', ...env },
  });
  const out = (res.stdout || '') + (res.stderr || '');
  return { code: res.status ?? 1, out };
}

// Ensure the exe is CURRENT — rebuild if any firmware/shim source changed, not merely if it's missing.
// Without this the gate could test a STALE exe after a firmware edit and report a false green.
{
  process.stdout.write(`${C.d}ensuring anima.exe is up to date ...${C.x}\r`);
  const b = run('node', [join(here, 'anima.mjs'), '--ensure']);
  if (b.code !== 0 || !existsSync(exe)) {
    console.error(`${C.r}anima.exe build FAILED — cannot run the gate:${C.x}\n${b.out.slice(-2000)}`);
    process.exit(1);
  }
}

// HERMETIC: clear the volatile per-run SD state so one gate can't pollute the next. math-check teaches
// custom units ("1 spanna = 22 cm"), profile-check sets a name, teach-check learns facts, and realistic
// executes profile/learn commands — all persist to SD across the cascade's /reset. Clearing only ONCE at
// the start let an earlier gate's writes leak into a later one (e.g. realistic's "mi chiamo Stefano" /
// "ricorda che…" changed skill-routing-2's retrieval). So reset before EVERY gate, not just at startup.
const clearVolatile = () => {
  for (const f of ['units.txt', 'session.txt', 'telemetry.ndjson',
                   'user.tsv', 'user.vec', 'user.tsv.tmp', 'user.vec.tmp',   // user-taught store
                   'profile.tsv', 'profile.tsv.tmp',                          // personal-profile store
                   'events.tsv', 'events.txt', 'calendar.tsv']) {            // agenda / add_event store
    try { rmSync(join(here, 'sd', 'data', 'anima', f)); } catch { /* absent is fine */ }
  }
};
clearVolatile();

const lastLine = (out) => out.trim().split(/\r?\n/).filter((l) => l.trim()).pop() || '';

// Each gate: {name, cmd, args, ok(code,out)->bool, summary(out)->string}
const gates = [
  { name: 'pack-coherence', cmd: 'node', args: ['tools/anima/check_pack.mjs'],
    // encoder.dim == index.D across EVERY shipped SD tree + a valid ASIG trailer (prefilter ON).
    // Catches the "stale index at the old dim" (load_index rejects → L1 silently disabled) and the
    // "forgot augment_akb4" (prefilter off / parity gates vacuous) bugs the separate-file build can't.
    ok: (code) => code === 0, summary: (o) => (o.match(/PACK [A-Z][^\n]*/) || [lastLine(o)])[0].replace(/\x1b\[[0-9;]*m/g, '').trim() },
  { name: 'regress (corpus+L1)', cmd: 'python', args: ['tools/anima/regress.py'],
    ok: (code) => code === 0, summary: (o) => lastLine(o) },
  { name: 'route-check', cmd: 'node', args: ['tools/anima-host/route-check.mjs', '--snapshot'],
    ok: (code) => code === 0, summary: (o) => lastLine(o) },
  { name: 'l1-parity (prefilter)', cmd: 'node', args: ['tools/anima-host/l1-parity.mjs'],
    // 130 bilingual EN/IT requests through BOTH the AKB4 popcount prefilter and the forced-exact path
    // (ANIMA_L1_EXACT=1): the fast path must be answer-identical to exhaustive search. Guards the L1
    // retrieval optimisation against any future drift.
    ok: (code) => code === 0, summary: (o) => (o.match(/parity diffs[^\n]*/) || [lastLine(o)])[0].trim() },
  { name: 'l1-recall (asym)', cmd: 'node', args: ['tools/anima-host/l1-recall.mjs'],
    // device-path recall on HARD out-of-distribution NL phrasings: the AKB4 asymmetric prefilter
    // (full-precision int8 query x 1-bit DB sign) must retrieve the right card AS OFTEN as exhaustive
    // search and drop none the exact path found — the regression guard for the holographic/asymmetric
    // prefilter that let M shrink 64->16 (4x fewer scattered SD reads). Derivation: tools/anima/holo_probe.py.
    ok: (code) => code === 0, summary: (o) => (o.match(/device recall[^\n]*/) || [lastLine(o)])[0].trim() },
  { name: 'agent-check', cmd: 'node', args: ['tools/anima-host/agent-check.mjs'],
    ok: (code) => code === 0, summary: (o) => lastLine(o) },
  { name: 'math-check', cmd: 'node', args: ['tools/anima-host/math-check.mjs'],
    ok: (code) => code === 0, summary: (o) => lastLine(o) },
  { name: 'reason-check', cmd: 'node', args: ['tools/anima-host/reason-check.mjs'],
    // the conversational reasoning layer (anima_reason): equation solver (1st/2nd degree + discriminant +
    // complex + self-verification), multi-step chains threaded through context, cross-turn named registers,
    // and the metacognitive conscience (impossible/underdetermined answered honestly). 0 false-positives on
    // ordinary sentences carrying a connective ("vado al cinema poi a cena") or a set register letter.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[reason\][^\n]*$/m) || [lastLine(o)])[0] },
  { name: 'ood-check (safety)', cmd: 'node', args: ['tools/anima-host/ood-check.mjs'],
    // ood-check always exits 0; the hard property is 0 out-of-scope FALSE-POSITIVES on the real exe.
    ok: (_code, o) => { const m = o.match(/FALSE-POSITIVE\(bad\)\s+(\d+)/); return m ? Number(m[1]) === 0 : false; },
    summary: (o) => (o.match(/out-of-scope[^\n]*/) || ['?'])[0].trim() },
  { name: 'reliability (traps)', cmd: 'node', args: ['tools/anima-host/ood-check.mjs', 'tools/anima/eval_traps.jsonl'],
    // adversarial scope/relation traps: a confident fact here = hallucination. The evidence-coverage
    // guards (L1 superlative scope + KGE specificity) must keep this at 0 FALSE-POSITIVES.
    ok: (_code, o) => { const m = o.match(/FALSE-POSITIVE\(bad\)\s+(\d+)/); return m ? Number(m[1]) === 0 : false; },
    summary: (o) => (o.match(/out-of-scope[^\n]*/) || ['?'])[0].trim() },
  { name: 'halluc-stress', cmd: 'node', args: ['tools/anima-host/halluc-stress.mjs'],
    // 441 combinatorially-generated unanswerable phrases (scoped superlatives, sub-national capitals,
    // fictional/false-premise) over the real pipeline — must stay at 0 hallucinations.
    ok: (code) => code === 0, summary: (o) => (o.match(/TOTAL HALLUCINATIONS[^\n]*/) || [lastLine(o)])[0].trim() },
  { name: 'halluc-battery (NL)', cmd: 'node', args: ['tools/anima-host/halluc-suite.mjs'],
    // The curated adversarial NL battery (eval_halluc_it/en + eval_halluc2_it/en, 485 deliberately-
    // unanswerable questions: impossible dates, fabricated entities/attributes, false premises, cross-skill
    // nonsense) run over the REAL exe via halluc-probe. HARD: 0 fabrications. Was previously UNGATED — a
    // date/system/geo skill over-firing on a false premise slipped through silently until added here.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[halluc-suite\][^\n]*/) || [lastLine(o)])[0].trim() },
  { name: 'realistic (NL)', cmd: 'node', args: ['tools/anima-host/realistic.mjs'],
    // ~310 REAL user requests (workflow-generated, exe-validated, many with QWERTY fast-typing typos) over
    // the PROGRAMMATIC cascade ONLY (no LLM): under-tested skills (gcd/lcm, base/roman conversion, scale,
    // unit variety, profile/learn/translate/calendar/settings) must FIRE the right intent; knowledge
    // questions (incl. typo'd or offline-unknown ones) must stay grounded or HONESTLY abstain — never a
    // compute skill, never a fabrication. Locks programmatic routing + safe degradation on garbled input.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[realistic\][^\n]*$/m) || [lastLine(o)])[0].replace(/\x1b\[[0-9;]*m/g, '').trim() },
  { name: 'skill<->knowledge boundary', cmd: 'node', args: ['tools/anima-host/boundary.mjs'],
    // The deterministic SKILL<->KNOWLEDGE boundary, both directions, over 35 homographs (forza/energia/
    // media/vettore/area/resistenza/logaritmo/seno…) that are BOTH a skill trigger AND a knowledge concept:
    // (A) a DEFINITION ("cos'è la forza", "what is the average") must be EXPLAINED/abstain and NEVER compute
    // a fabricated value (a skill stepping on knowledge); (B) a COMPUTE ("forza con massa 10 e accelerazione
    // 3") must STILL fire its skill (knowledge must not swallow a command). 455 cases, no RNG, no clock.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[boundary\][^\n]*$/m) || [lastLine(o)])[0].replace(/\x1b\[[0-9;]*m/g, '').trim() },
  { name: 'metamorph (NL)', cmd: 'node', args: ['tools/anima-host/metamorph.mjs'],
    // METAMORPHIC + cross-substrate: takes the ~485 curated unanswerable seeds and derives ~2300 mutants
    // via SEMANTICS-PRESERVING perturbation (typo on wrappers / CAPS / dropped-accents / punctuation /
    // politeness lead-in / filler / wrapper code-switch / commutative reorder), then asserts the abstain
    // verdict is INVARIANT under every perturbation (a flip = a fabrication the static suite missed, e.g.
    // accent "morira"->spellfix "mostra"->launch, typo "chi"->"cho"->capabilities {value}). Also host-proves
    // M3 entity-detection invariance (no perturbation re-aims the network) and M4 verifier monotonicity
    // (anima.exe --verify never CONFIRMS a fabricated answer). HARD: 0 invariance breaks. More coverage, no new phrases.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[metamorph\][^\n]*$/m) || [lastLine(o)])[0].replace(/\x1b\[[0-9;]*m/g, '').trim() },
  { name: 'nl-stress (offline)', cmd: 'node', args: ['tools/anima-host/nl-stress.mjs'],
    // 250+ generated offline NL cases over the REAL exe: corpus-derived paraphrase+typo recall (no
    // misattribution) + adversarial false-premise/fictional/nonsense (must abstain/refuse/deflect-to-
    // grounded-fact). HARD: 0 hallucinations. Catches cross-skill false-positives like "cos'è …42…"→cos(42).
    ok: (code) => code === 0, summary: (o) => (o.match(/TOTAL HALLUCINATIONS[^\n]*/) || [lastLine(o)])[0].trim() },
  { name: 'fluency-grounded (L2)', cmd: 'node', args: ['tools/anima-host/fluency-grounded.mjs'],
    // MOSAICO span-stitch (docs/anima.md "L2 … deferred behind a measurement gate"): describe/explain
    // answers must get FULLER (uplift) by appending ONLY verbatim card spans (grounded, additive) and
    // never answer a describe-shaped question about a fabricated entity (safety). The gate that un-defers L2.
    ok: (code) => code === 0, summary: (o) => (o.match(/stitched[^\n]*/) || [lastLine(o)])[0].trim() },
  { name: 'describe-stress (NL)', cmd: 'node', args: ['tools/anima-host/describe-stress.mjs'],
    // 170 IT+EN describe/MOSAICO cases (correct / misleading / deliberately-trap): the HARD property is
    // ZERO fabricated knowledge on fake/adversarial/lookalike (never invents); recall is coverage-floored.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[describe-stress\] \d+ cases[^\n]*/) || [lastLine(o)])[0].trim() },
  { name: 'skill-routing', cmd: 'node', args: ['tools/anima-host/skill-probe.mjs', 'tools/anima/eval_skills.jsonl'],
    // curated cross-skill routing (calendar/people/excel/weather/file/settings) — locks the skill-audit
    // fixes: real requests reach the right tool, wrong/unanswerable ones abstain.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[skill-probe\][^\n]*/) || [lastLine(o)])[0].trim() },
  { name: 'skill-routing-2', cmd: 'node', args: ['tools/anima-host/skill-probe.mjs', 'tools/anima/eval_skills2.jsonl'],
    // 300+ verified IT+EN cases (calendar/file/settings/math/excel/knowledge/weather/apps/adversarial),
    // authored by driving the real exe — broad NL coverage anchor.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[skill-probe\][^\n]*/) || [lastLine(o)])[0].trim() },
  { name: 'false-positives (NL)', cmd: 'node', args: ['tools/anima-host/skill-probe.mjs', 'tools/anima/eval_skills3.jsonl'],
    // 100 IT+EN probes: deliberately OUT-OF-CONTEXT phrases (nonsense, gibberish, false-premise
    // geography, impossible actions) that MUST abstain — the system must never fabricate — plus correct
    // Excel/spreadsheet and major-geography requests that must route/answer. Guards against false positives.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[skill-probe\][^\n]*/) || [lastLine(o)])[0].trim() },
  { name: 'false-positives-2 (NL)', cmd: 'node', args: ['tools/anima-host/skill-probe.mjs', 'tools/anima/eval_skills4.jsonl'],
    // 100 MORE fresh IT+EN probes across math/settings/apps/calendar/file/date-time/knowledge (correct
    // routing) + new out-of-context nonsense, impossible actions, predictions and gibberish (must abstain).
    ok: (code) => code === 0, summary: (o) => (o.match(/\[skill-probe\][^\n]*/) || [lastLine(o)])[0].trim() },
  { name: 'cross-topic halluc (NL)', cmd: 'node', args: ['tools/anima-host/skill-probe.mjs', 'tools/anima/eval_skills5.jsonl'],
    // 100 ADVERSARIAL IT+EN traps: cross-topic false premises (one entity's property asked of a wrong-
    // domain entity), category errors, leading/presupposition questions — must NEVER fabricate (abstain,
    // or answer with the correct grounded bio, never the false fact) + deliberate typos (spellfix or safe
    // abstain). The strongest anti-hallucination regression guard.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[skill-probe\][^\n]*/) || [lastLine(o)])[0].trim() },
  { name: 'halluc-probe IT', cmd: 'node', args: ['tools/anima-host/halluc-probe.mjs', 'tools/anima/eval_halluc_it.jsonl'],
    // ~200 NOVEL IT adversarial requests crossing a real skill (calc/convert/date/weather/translate/file/
    // excel/profile/people-facet) with an impossible premise or unknowable fact. STRICTER than skill-probe:
    // a confident reply via ANY tier (incl. L0/command — a leaked {value}, a fabricated weekday) is a
    // fabrication. Must stay at 0. Guards the system-intent frame guards / date-solver / spreadsheet target.
    ok: (code) => code === 0, summary: (o) => (o.match(/honestly abstained[^\n]*/) || [lastLine(o)])[0].trim() },
  { name: 'halluc-probe EN', cmd: 'node', args: ['tools/anima-host/halluc-probe.mjs', 'tools/anima/eval_halluc_en.jsonl'],
    // ~200 NOVEL EN counterparts of halluc-probe IT — same strict 0-fabrication property over the real exe.
    ok: (code) => code === 0, summary: (o) => (o.match(/honestly abstained[^\n]*/) || [lastLine(o)])[0].trim() },
  { name: 'skill-isolation (paint)', cmd: 'node', args: ['tools/anima-host/skill-probe.mjs', 'tools/anima/eval_paint_decline.jsonl'],
    // image-generation requests DECLINE with a grounded redirect to Paint's Atelier (intent=image_gen),
    // and must NOT collide with open_app(paint)/create_file/translate/how-to. The isolation guard that
    // keeps the generative skill from clobbering the rest of the cascade (nucleo_anima.c a_is_image_gen).
    ok: (code) => code === 0, summary: (o) => (o.match(/\[skill-probe\][^\n]*/) || [lastLine(o)])[0].trim() },
  { name: 'image-gen stress (NL)', cmd: 'node', args: ['tools/anima-host/skill-probe.mjs', 'tools/anima/eval_paint_stress.jsonl'],
    // 115 REAL adversarial IT+EN cases stressing the image-gen isolation: genuine asks (verb+noun, polite/
    // question forms, "disegnami/draw me X" request forms, weather-word objects, CAPS/punctuation) MUST
    // decline to image_gen; look-alikes (open-app, create-file/note/event, how-to, art knowledge, verb-without-
    // image-noun, image-noun-without-verb, translate, ambiguous) MUST route elsewhere. 0 collisions.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[skill-probe\][^\n]*/) || [lastLine(o)])[0].trim() },
  { name: 'action-tier (false-act)', cmd: 'node', args: ['tools/anima-host/skill-probe.mjs', 'tools/anima/eval_action.jsonl'],
    // the action-side of 0-hallucination: real commands act; PAST-TENSE statements ("ho creato un file")
    // and NEGATIONS ("non voglio creare un file") must NEVER execute a fabricated action (tier:none).
    // Locks the participle/negation guard against the spellfix that "corrected" creato->crea.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[skill-probe\][^\n]*/) || [lastLine(o)])[0] },
  { name: 'cross-skill (xskill)', cmd: 'node', args: ['tools/anima-host/skill-probe.mjs', 'tools/anima/eval_xskill.jsonl'],
    // 40 curated cross-skill confusion cases: function-name/number false-friends ("cos'è il seno",
    // "log in al sistema") must NOT compute; "ricordami CHI/COSA…" must NOT make an event; "vorrei/voglio
    // sapere…" must NOT launch; physical questions about ANIMA hit the identity card not a fact card.
    // tier:none locks the must-fully-abstain cases so a calc/calendar/launch regression is caught.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[skill-probe\][^\n]*/) || [lastLine(o)])[0] },
  { name: 'skill-coverage IT (s6)', cmd: 'node', args: ['tools/anima-host/skill-probe.mjs', 'tools/anima/eval_skills6_it.jsonl'],
    // GENUINE offline-skill coverage (IT): every offline intent (calc/percent/convert/base/roman/prime/geo/
    // phys/spreadsheet/create_file/add_event/set_volume/set_brightness/open_app/close_app/translate/profile/
    // teach/time/date/capabilities/whoami/agenda + L1 knowledge) must FIRE on real phrasings + typos. The
    // determinism anchor: a skill that stops triggering offline is caught here. Twin trap set: eval_halluc6_it.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[skill-probe\][^\n]*/) || [lastLine(o)])[0] },
  { name: 'skill-coverage EN (s6)', cmd: 'node', args: ['tools/anima-host/skill-probe.mjs', 'tools/anima/eval_skills6_en.jsonl'],
    // GENUINE offline-skill coverage (EN): English counterpart of skill-coverage IT — same offline intents fire.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[skill-probe\][^\n]*/) || [lastLine(o)])[0] },
  { name: 'skill-coverage IT (s7)', cmd: 'node', args: ['tools/anima-host/skill-probe.mjs', 'tools/anima/eval_skills7_it.jsonl'],
    // GENUINE offline-skill coverage round 2 (IT): fresh never-before-tested phrasings (parentheses/powers,
    // speed/time/volume conversions, new app aliases, profile fields, new knowledge) — broadens the determinism net.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[skill-probe\][^\n]*/) || [lastLine(o)])[0] },
  { name: 'skill-coverage EN (s7)', cmd: 'node', args: ['tools/anima-host/skill-probe.mjs', 'tools/anima/eval_skills7_en.jsonl'],
    // GENUINE offline-skill coverage round 2 (EN): English counterpart of s7.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[skill-probe\][^\n]*/) || [lastLine(o)])[0] },
  { name: 'skill-coverage IT (s8)', cmd: 'node', args: ['tools/anima-host/skill-probe.mjs', 'tools/anima/eval_skills8_it.jsonl'],
    // GENUINE offline-skill coverage round 3 (IT): 104 fresh phrasings weighted to APP-LAUNCH + CALENDAR/agenda
    // + settings (apri/lancia/avvia/mostrami/vai/portami a N app; ricordami/segna/aggiungi/metti in agenda;
    // luminosità/volume). Locks the action-skill surface that users hit most. All validated on the real exe.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[skill-probe\][^\n]*/) || [lastLine(o)])[0] },
  { name: 'skill-coverage EN (s8)', cmd: 'node', args: ['tools/anima-host/skill-probe.mjs', 'tools/anima/eval_skills8_en.jsonl'],
    // GENUINE offline-skill coverage round 3 (EN): English counterpart of s8 — app-launch + calendar + settings.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[skill-probe\][^\n]*/) || [lastLine(o)])[0] },
  { name: 'date/excel halluc IT', cmd: 'node', args: ['tools/anima-host/halluc-probe.mjs', 'tools/anima/eval_halluc_date_it.jsonl'],
    // anti-hallucination DATE/BIO + EXCEL traps (IT): "quando è morto <vivo/fittizio>", "quanti anni ha oggi
    // <defunto>", "chi è morto prima tra X e Y", "fai la media dei miei sogni" — must abstain, never fabricate a
    // year/age/formula. The combinator computes a birth-year compare only when BOTH years are known, else abstains.
    ok: (code) => code === 0, summary: (o) => (o.match(/honestly abstained[^\n]*/) || [lastLine(o)])[0].trim() },
  { name: 'date/excel halluc EN', cmd: 'node', args: ['tools/anima-host/halluc-probe.mjs', 'tools/anima/eval_halluc_date_en.jsonl'],
    // English counterpart: died-when on the living/fictional, age-at-death, who-died-first, excel-over-nonsense.
    ok: (code) => code === 0, summary: (o) => (o.match(/honestly abstained[^\n]*/) || [lastLine(o)])[0].trim() },
  { name: 'combinator+excel IT', cmd: 'node', args: ['tools/anima-host/skill-probe.mjs', 'tools/anima/eval_combo_date_it.jsonl'],
    // GENUINE twin (IT): the date-reasoning combinator ("chi è nato prima/dopo …") and the Excel engine
    // (SUM/AVERAGE/MAX/MIN/COUNT/COUNTIF/SUMIF/VLOOKUP/CONCAT/IF over real cell refs) must FIRE. Locks the
    // skill side so the anti-hallucination guard above doesn't over-abstain on real requests.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[skill-probe\][^\n]*/) || [lastLine(o)])[0] },
  { name: 'combinator+excel EN', cmd: 'node', args: ['tools/anima-host/skill-probe.mjs', 'tools/anima/eval_combo_date_en.jsonl'],
    // English counterpart of combinator+excel IT.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[skill-probe\][^\n]*/) || [lastLine(o)])[0] },
  { name: 'math-dialog IT', cmd: 'node', args: ['tools/anima-host/math-dialog.mjs', 'tools/anima/eval_mathdlg_it.jsonl'],
    // 10 CHAINED-calculation dialogues (IT), 10 turns each, via cross-turn named registers ("chiamalo a" ->
    // "a più 10" -> "il doppio di a"). No reset between turns: the register context must survive and every
    // chained result stay exact (100 calcs). Guards the conversational calculator's multi-turn memory.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[math-dialog\][^\n]*/) || [lastLine(o)])[0] },
  { name: 'math-dialog EN', cmd: 'node', args: ['tools/anima-host/math-dialog.mjs', 'tools/anima/eval_mathdlg_en.jsonl'],
    // English counterpart: 10 dialogues x 10 chained turns via "call it a" / "a plus 10" / "double of a".
    ok: (code) => code === 0, summary: (o) => (o.match(/\[math-dialog\][^\n]*/) || [lastLine(o)])[0] },
  { name: 'teach-loop (learn)', cmd: 'node', args: ['tools/anima-host/teach-check.mjs'],
    // the offline user-teach tier (nucleo_anima_learn.c): teach a fact -> recall it by paraphrase offline
    // (IT+EN), while same-shape-different-subject and volatile statements abstain. 0 misattributions.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[teach-check\][^\n]*/) || [lastLine(o)])[0] },
  { name: 'profile (personal)', cmd: 'node', args: ['tools/anima-host/profile-check.mjs'],
    // the typed personal-profile tier (nucleo_anima_profile.c): 100+ IT+EN cases — set/recall self-facts
    // cross-session deterministically, honest "don't know yet" on unset, ZERO third-person/teach hijack.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[profile-check\][^\n]*/) || [lastLine(o)])[0] },
  { name: 'dict-sync (translate)', cmd: 'python', args: ['tools/anima/gen_dicts.py', '--check'],
    // the offline IT<->EN dictionaries (dict-*.tsv on SD + host) must be regenerated from the seed
    // (tools/anima/dict/seed.it-en.tsv). Fails if someone edited the seed but forgot to run gen_dicts.py.
    ok: (code) => code === 0, summary: (o) => lastLine(o) },
  { name: 'factory-games (sd)', cmd: 'python', args: ['tools/gen-factory-manifests.py', '--check'],
    // the bundled DOS/ROMs games are pinned against deletion by a per-folder .factory manifest the
    // firmware reads (nucleo_fsfactory.h). Fails if someone added/removed a bundled game but forgot to
    // regenerate the manifests — which would leave a new game unprotected, or a removed one falsely listed.
    ok: (code) => code === 0, summary: (o) => lastLine(o) },
  { name: 'translate (IT<->EN)', cmd: 'node', args: ['tools/anima-host/translate-check.mjs'],
    // the offline dictionary translator (nucleo_anima_translate.c): word/phrase IT<->EN via EXACT SD
    // lookup — grounded translations, honest decline on a miss, and ZERO false positives (a non-translation
    // query never routes here). Content + routing asserted over the real cascade.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[translate-check\][^\n]*/) || [lastLine(o)])[0] },
  { name: 'dict-stress (bulk IT<->EN)', cmd: 'node', args: ['tools/anima-host/dict-stress.mjs'],
    // the BULK FreeDict dictionary (~60k entries/direction) binary-searched on SD: grounded recall sampled
    // from the dict itself (both directions), junk words abstain, and weather-words as a translate OBJECT
    // ("traduci sole in inglese") translate instead of firing the forecast. 0 hallucination by construction.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[dict-stress\] recall[^\n]*/) || [lastLine(o)])[0].replace(/\x1b\[[0-9;]*m/g, '').trim() },
  { name: 'kge-eval', cmd: 'node', args: ['tools/anima/kge-eval.mjs'], ok: (code) => code === 0,
    summary: (o) => (o.match(/fatti entailed[^\n]*/) || [lastLine(o)])[0].replace(/\x1b\[[0-9;]*m/g, '').trim() },
  { name: 'hdc-eval', cmd: 'node', args: ['tools/anima/hdc-eval.mjs'], ok: (code) => code === 0,
    summary: (o) => (o.match(/recall offline[^\n]*/) || [lastLine(o)])[0].replace(/\x1b\[[0-9;]*m/g, '').trim() },
  { name: 'combinator-eval', cmd: 'node', args: ['tools/anima/combinator-eval.mjs'], ok: (code) => code === 0,
    summary: (o) => (o.match(/risposte composte[^\n]*/) || [lastLine(o)])[0].replace(/\x1b\[[0-9;]*m/g, '').trim() },
  // NSPCG: proof-carrying GENERATION — autonomously discovers a derivation chain (kge.reach), verifies
  // every hop against the symbol store, verbalizes a sentence that is nowhere in the corpus, and attaches
  // a re-checkable proof tree. Refuses when no grounded chain exists → 0 hallucination by construction.
  { name: 'pcg-eval', cmd: 'node', args: ['tools/anima/pcg-eval.mjs'], ok: (code) => code === 0,
    summary: (o) => (o.match(/frasi generate[^\n]*/) || [lastLine(o)])[0].replace(/\x1b\[[0-9;]*m/g, '').trim() },
  { name: 'typed-facets (KG)', cmd: 'node', args: ['tools/anima-host/typed-check.mjs'],
    // the typed knowledge layer (docs/anima-knowledge-graph.md): isa/occupation/gender facets +
    // died — present, curated entities resolve correctly, it/en person-facet parity, coverage floor,
    // and PLACES carry no person facets. Guards extract_triples.py against silent drift.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[typed-check\][^\n]*/) || [lastLine(o)])[0] },
  { name: 'typed-nl (facet)', cmd: 'node', args: ['tools/anima-host/typed-nl-check.mjs'],
    // 171 NL cases through the REAL cascade: the facet tier (nucleo_anima_facet.c) answers occupation/
    // gender correctly + died via the KGE, and FABRICATES NOTHING on adversarial unknown-entity /
    // wrong-type / false-premise / tier-overlap phrasings. The 0-hallucination guard for typed answers.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[typed-nl\][^\n]*/) || [lastLine(o)])[0] },
  { name: 'entity-detect (online)', cmd: 'node', args: ['tools/anima/entity.mjs'],
    // generic entity-question grammar (host mirror of nucleo_anima_online.c TRIG_IT/EN): pulls the entity
    // out of ANY "asking about a named thing" phrasing — incl. imperfect occupation / origin forms — for
    // any name, 0 out-of-scope false-positives. Guards what reaches the online certain-source lookup.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[entity\][^\n]*/) || [lastLine(o)])[0] },
  { name: 'clean-extract (wiki)', cmd: 'node', args: ['tools/anima/clean-extract.mjs'],
    // host mirror of strip_pronun() in nucleo_anima_online.c: the Wikipedia lead is relayed verbatim
    // (0-hallucination) but the IPA/AFI phonetic pronunciation + reference markers are stripped, WITHOUT
    // cutting real prose (a verb "pronounced dead" with no phonetic marker is kept).
    ok: (code) => code === 0, summary: (o) => (o.match(/\[clean-extract\][^\n]*/) || [lastLine(o)])[0] },
  { name: 'auto-evolution (VKL)', cmd: 'node', args: ['tools/anima-host/evolution-check.mjs'],
    // docs/anima-knowledge-graph.md §self-evolution: the taxonomy GROWS by itself from Wikidata (CC0)
    // subclass-of edges — generalizations ("X is a scientist / a person") are DEDUCED by composing
    // relation-rotations (never stored); long/noisy chains ABSTAIN (0 fabrication); the hash-chained
    // knowledge ledger verifies + detects tampering; uncertain/duplicate facts are rejected.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[evolution\][^\n]*/) || [lastLine(o)])[0].replace(/\x1b\[[0-9;]*m/g, '').trim() },
  { name: 'ledger-attack (VKL)', cmd: 'node', args: ['tools/anima-host/ledger-attack-check.mjs'],
    // replays the adversarial-audit attacks against the Verifiable Knowledge Ledger and asserts v2 DEFEATS
    // each: forged/unresolvable provenance rejected, conf/field/length/control-char abuse rejected, canonical
    // injective, forge+reseal detected by the off-SD anchor, wrong key breaks the chain, abstract/vandalized
    // generalizations refused. The immutability + certainty backbone's regression guard.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[ledger-attack\][^\n]*/) || [lastLine(o)])[0].replace(/\x1b\[[0-9;]*m/g, '').trim() },
  { name: 'contradiction (facts)', cmd: 'node', args: ['tools/anima-host/contradiction-check.mjs'],
    // The certainty backbone the flywheel lacked: over the accumulated (subject,rel,value) triples
    // (extract_triples.py output + the online learned cache that grows from Wikipedia at runtime), assert
    // no FUNCTIONAL fact contradicts itself — born/died (by year), gender (M/F normalized), capital (one
    // country, compared per-language so "Spagna"!="Spain" is not a false flag). Multi-valued relations
    // (occupation/isa/located_in/country) are excluded by design. Caught the live world.json typo
    // "Sudan capital = Il Cairo" the extractor's own self-checks could not see. SKIPs cleanly if no store.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[contradiction\][^\n]*/) || [lastLine(o)])[0].replace(/\x1b\[[0-9;]*m/g, '').trim() },
  { name: 'akb5-content (sharded)', cmd: 'node', args: ['tools/anima-host/akb5-content.mjs'],
    // Forces the AKB5 sharded router (ANIMA_AKB5=1) and proves the SCALABLE encyclopedia on the real exe:
    // capitals / physical constants / grounded definitions answer under varied NL, while false premises
    // ("capitale di Marte", anachronisms) ABSTAIN — 0 hallucinations. SKIPs cleanly if no manifest is built.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[akb5-content\][^\n]*(recall|SKIP)[^\n]*/) || [lastLine(o)])[0].replace(/\x1b\[[0-9;]*m/g, '').trim() },
  { name: 'arbiter (concurrency)', cmd: 'node', args: ['tools/anima-host/arb-check.mjs'],
    // The device-authoritative heavy-work arbiter (firmware/components/nucleo_arb): the SAME core
    // that serializes TLS/SD/heap jobs on the PSRAM-less device, host-compiled and run under REAL
    // Win32 thread contention. HARD: mutual exclusion (two threads never hold the token at once),
    // FG-preempts-BG yielding, the never-block try-acquire (the httpd task must never stall), and
    // the teardown heap-floor sentinel. Guards the OOM-race fix against any future drift.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[arb-check\] \d+\/\d+[^\n]*/) || [lastLine(o)])[0] },
  { name: 'online-stability (TWDT)', cmd: 'node', args: ['tools/anima-host/online-stability-check.mjs'],
    // docs/anima-native.md §stability: locks the 2026-06-24 anti-reboot fix in nucleo_anima_online.c so it
    // can't silently regress — per-attempt TLS timeout stays < the 8 s Task-WDT, the chat paths bind the
    // HTTP_TIMEOUT symbol (no raw literal dodge), the wall-clock turn budget + WDT-pet + heap-state failure
    // logging are present, and nucleo_exclusive_enter returns per-call ownership. Source-invariant, no exe.
    ok: (code) => code === 0, summary: (o) => (o.match(/\[online-stability\][^\n]*/) || [lastLine(o)])[0] },
  { name: 'offline-installer', cmd: 'node', args: ['--test', 'tools/anima-host/forge-model-store.test.mjs', 'tools/anima-host/forge-install-flow.test.mjs', 'tools/anima-host/forge-install-integration.test.mjs', 'tools/anima-host/forge-model-url-map.test.mjs', 'tools/anima-host/forge-local-models.test.mjs'],
    // First-class regression guard for the OFFLINE-MODEL INSTALLER (apps/anima/www/forge — the "scarica dal
    // Cardputer → gira offline nel browser" core path). Two suites: the download CONTROLLER's hard invariants
    // (never auto-download · one at a time · online-CDN→Cardputer-SD order · SHA-256 integrity · idempotent
    // resume · typed errors) and the resilient install FLOW (AUTO-RESUME across a Cardputer disconnect
    // without losing verified shards · cancel · engine-prereq block · clear per-error messages · OS-wide
    // one-at-a-time lock). Pure & I/O-injected, so it runs with no exe/device. (Also swept by 'unit tests'
    // below; named here so a regression is attributed, not buried.)
    ok: (code) => code === 0, summary: (o) => (o.match(/[#ℹ] pass \d+/) || ['tests']).concat(o.match(/[#ℹ] fail \d+/) || []).join('  ') },
  { name: 'tts (voice plan+index)', cmd: 'node', args: ['tools/anima-host/tts-check.mjs'],
    // The OFFLINE concatenative VOICE (firmware/components/nucleo_tts): the IT/EN text->clip planner
    // (numbers/dates/decimals, greedy phrase match, content guard vs code/too-long, whole-utterance
    // fixed-reply lookup) AND the packed-index binary search (RAM-light clip retrieval on SD). Pure C,
    // host-compiled with MinGW — no device. Guards the voice logic against drift.
    ok: (code) => code === 0, summary: (o) => (o.match(/\d+ ok, \d+ fail/g) || [lastLine(o)]).join('  ') },
  { name: 'hw-manifest (NEXUS)', cmd: 'node', args: ['--test', 'tools/anima-host/hw-check.mjs'],
    // The hardware capability manifest (apps/code-runner/nucleo-hw.js): protocol→/api mapping, validateArgs
    // grounding (GPIO safe-pin allowlist + enums), and the offline NL resolver — including its hard
    // invariant that it NEVER fires online (so it can't shadow the real LLM). Pure, no exe/device.
    ok: (code) => code === 0, summary: (o) => (o.match(/[#ℹ] pass \d+/) || ['tests']).concat(o.match(/[#ℹ] fail \d+/) || []).join('  ') },
  { name: 'link-proto (espnow)', cmd: 'node', args: ['tools/anima-host/link-check.mjs'],
    // The "Vicino" device-to-device transfer core (firmware/components/nucleo_link), host-compiled with
    // MinGW and driven through a packet-dropping/reordering/duplicating channel: the EVOLVED protocol must
    // deliver every byte LOSSLESSLY (sliding window + selective ACK + retransmit + whole-file CRC32) and
    // RESUME from the contiguous prefix, where Bruce's naive share would corrupt the file. Also asserts the
    // Bruce-codec stays wire-compatible (248-byte Message layout). Pure C, no device.
    ok: (code) => code === 0, summary: (o) => (o.match(/RESULT:[^\n]*/) || [lastLine(o)])[0].trim() },
  { name: 'mesh-seam (swarm)', cmd: 'node', args: ['tools/anima-host/mesh-check.mjs'],
    // The MESH gossip seam (firmware/components/nucleo_mesh), host-compiled with MinGW: extends the
    // event bus across ESP-NOW peers. Asserts the ADR invariants (docs/swarm-architecture.md) — only
    // mesh.*/chorus.* topics gossip, foreign events are NEVER re-forwarded (loop prevention), and
    // de-dup by (origin id, seq) never injects any event twice through a lossy/reordering channel.
    ok: (code) => code === 0, summary: (o) => (o.match(/RESULT:[^\n]*/) || [lastLine(o)])[0].trim() },
  { name: 'chorus-dir (swarm)', cmd: 'node', args: ['tools/anima-host/chorus-check.mjs'],
    // The CHORUS capability directory (firmware/components/nucleo_mesh/nucleo_chorus.c) running over
    // the MESH seam: a 3-node swarm gossips self-manifests through a lossy channel and must CONVERGE,
    // then content-route by capability+domain (tie-broken by advertised free_kb), reroute around a
    // busy peer, and drop peers past the TTL. The "bulletin board, not a boss" model — no leases.
    ok: (code) => code === 0, summary: (o) => (o.match(/RESULT:[^\n]*/) || [lastLine(o)])[0].trim() },
  { name: 'swarm-sec (gate)', cmd: 'node', args: ['tools/anima-host/swarm-sec-check.mjs'],
    // The swarm security gate (firmware/components/nucleo_mesh/nucleo_swarm_sec.c): demux by magic,
    // HMAC seal/open (hash fn injected; constant-time verify) and MAC trust-pin. The MANDATORY auth
    // the open ESP-NOW transport lacks — a tampered/forged/wrong-key/truncated frame must NOT open.
    ok: (code) => code === 0, summary: (o) => (o.match(/RESULT:[^\n]*/) || [lastLine(o)])[0].trim() },
  { name: 'eth-frames (wired)', cmd: 'node', args: ['tools/anima-host/eth-check.mjs'],
    // The wired-attack frame core (firmware/components/nucleo_eth/eth_frames.c), host-compiled with
    // MinGW: ARP/DHCP/TCP build+parse, IP/UDP/TCP checksums, subnet math, random-MAC properties (LAA/
    // unicast), OUI vendor lookup, host-table dedup — 47 assertions. Pure C, no device. The regression
    // guard for the W5500 L2/L3 engine's frame layout/byte-order/checksums (a wrong byte = a dead attack).
    ok: (code) => code === 0, summary: (o) => (o.match(/== \d+ passed[^\n]*==/) || [lastLine(o)])[0].trim() },
  { name: 'ble-adv (spam/beacon)', cmd: 'node', args: ['tools/anima-host/ble-check.mjs'],
    // The BLE advertisement payload core (firmware/components/nucleo_ble/nucleo_ble_adv.c), host-compiled
    // with MinGW: Apple Continuity / Microsoft Swift Pair / Google Fast Pair / iBeacon AD framing — company
    // IDs, type bytes, rotating model placement — and the HARD <=31-byte adv invariant. Pure C, no device,
    // no NimBLE. A wrong byte = a payload the controller rejects or an OS ignores. FOR AUTHORIZED TESTING.
    ok: (code) => code === 0, summary: (o) => (o.match(/\d+ passed[^\n]*/) || [lastLine(o)])[0].trim() },
  { name: 'probe-ssid (KARMA)', cmd: 'node', args: ['tools/anima-host/probe-check.mjs'],
    // The KARMA probe-request SSID parser (firmware/components/nucleo_wifiatk/nucleo_wifiatk_probe.c),
    // host-compiled with MinGW: 802.11 subtype gate, IE walk, SSID extraction, and the broadcast /
    // non-printable / malformed / tiny-buffer rejections. Pure C, no radio. A wrong byte = the lure
    // mislists or crashes on a crafted frame. FOR AUTHORIZED TESTING.
    ok: (code) => code === 0, summary: (o) => (o.match(/\d+ passed[^\n]*/) || [lastLine(o)])[0].trim() },
  { name: 'portal-clone (F2)', cmd: 'node', args: ['tools/anima-host/portalclone-check.mjs'],
    // The Evil Portal page-cloner asset rewriter (firmware/components/nucleo_evilportal/
    // nucleo_evilportal_clone.c), host-compiled with MinGW: same-origin detection, absolute/root/doc-
    // relative URL resolution, the extension filter, the in-place shrink-only rewrite, and the cross-
    // origin / data: / unknown-ext rejections. Pure C, no networking. FOR AUTHORIZED TESTING.
    ok: (code) => code === 0, summary: (o) => (o.match(/\d+ passed[^\n]*/) || [lastLine(o)])[0].trim() },
  { name: 'ducky (payloads)', cmd: 'node', args: ['tools/anima-host/ducky-check.mjs'],
    // The DuckyScript engine (firmware/components/nucleo_ducky/nucleo_ducky.c), host-compiled with MinGW:
    // command parsing (STRING/STRINGLN/DELAY/DEFAULT_DELAY/REPEAT/combos), US+IT keyboard layout maps, and
    // the dry-run analysis. Pure C, no USB/BLE/device. A wrong keycode = a payload that mistypes on the host.
    ok: (code) => code === 0, summary: (o) => (o.match(/\d+ passed[^\n]*/) || [lastLine(o)])[0].trim() },
  { name: 'weather (meteo)', cmd: 'node', args: ['tools/anima-host/weather-check.mjs'],
    // The pure weather core (firmware/components/nucleo_weather/weather_wmo.c): WMO code -> icon class +
    // Italian label, and date -> weekday. Pure C, no network. Guards the app's icon/label/forecast mapping.
    ok: (code) => code === 0, summary: (o) => (o.match(/\d+ passed[^\n]*/) || [lastLine(o)])[0].trim() },
  { name: 'nearby-skill (scoped)', cmd: 'node', args: ['--test', 'tools/anima-host/nearby-check.mjs'],
    // The Vicino ANIMA skill (apps/nearby/www/nearby-skill.js): the IT/EN floor parses transfer commands,
    // the CLOSED action schema rejects off-app verbs, and the device-touching verbs (send file/command,
    // accept) are MUTATING → fail-closed without explicit confirm (no silent exfiltration — the gate
    // Bruce's arbitrary remote-exec lacks). Pure, transport+ops injected.
    ok: (code) => code === 0, summary: (o) => (o.match(/[#ℹ] pass \d+/) || ['tests']).concat(o.match(/[#ℹ] fail \d+/) || []).join('  ') },
  { name: 'app-skill (scoped)', cmd: 'node', args: ['--test', 'tools/anima-host/skill-check.mjs'],
    // The reusable per-app scoped ANIMA skill kit (apps/anima/anima-skill.js): closed action schema,
    // runtime validation, the deterministic-floor fast-path, and the prompt-injection defense (GUARD
    // preamble + untrusted fencing + mutating fail-closed). Locks each app's skill against drift/jailbreak.
    ok: (code) => code === 0, summary: (o) => (o.match(/[#ℹ] pass \d+/) || ['tests']).concat(o.match(/[#ℹ] fail \d+/) || []).join('  ') },
  { name: 'agent-helpers (ANIMA Code)', cmd: 'node', args: ['--test', 'tools/anima-host/agent-helpers.test.mjs'],
    // ANIMA Code / Claude-Code helpers (apps/agent/agent-tools.js): line-numbered reads, the write→lint
    // verifyCode (module-aware so it never false-alarms), fenceUntrusted injection defense (forged closing
    // tag neutralised), and VALID Gemini model ids (guards the dead 'gemini-3.5-flash' regression).
    // (Also swept by 'unit tests'; named here so a regression is attributed, not buried.)
    ok: (code) => code === 0, summary: (o) => (o.match(/[#ℹ] pass \d+/) || ['tests']).concat(o.match(/[#ℹ] fail \d+/) || []).join('  ') },
  { name: 'agent-contract (multi-provider)', cmd: 'node', args: ['tools/anima-host/agent-contract-check.mjs'],
    // The online multi-agent's tool contract (apps/agent): tool schema ↔ OpenAI mapping (so Groq/Grok/Gemini
    // get the SAME OS tools as Claude), the tool-use loop threading, the deterministic plan guard, and the
    // app-creation tools (scaffold/publish/manage) present + gated. Locks the cross-provider parity surface.
    ok: (code) => code === 0, summary: (o) => lastLine(o) },
  { name: 'app-publish (create-app safety)', cmd: 'node', args: ['tools/anima-host/app-publish-check.mjs'],
    // The "agent builds a NucleoOS app" core: id/manifest/scaffold + the ANTI-DESTRUCTIVE registry planner
    // (a null/unreadable registry REFUSES — never wipes the other apps), the anti-traversal path guard, the
    // size cap, and the enable/disable lifecycle. The safety net for everything written to /apps + the registry.
    ok: (code) => code === 0, summary: (o) => lastLine(o) },
  { name: 'app-ops (publish integration)', cmd: 'node', args: ['tools/anima-host/app-ops-check.mjs'],
    // End-to-end publish/scaffold/manage against an IN-MEMORY device: proves the anti-destructive ORDERING
    // (lint before any write, registry written LAST, zero writes on any refusal — no-wipe / traversal / size).
    ok: (code) => code === 0, summary: (o) => lastLine(o) },
  { name: 'app-review (advisory)', cmd: 'node', args: ['tools/anima-host/app-review-check.mjs'],
    // The cross-provider review core: ADVISORY-safety (a non-parsable/empty reply degrades to {ok,issues:[]}
    // so a reviewer hiccup can never falsely block a valid app) + the prompt builder (manifest+html, truncated).
    ok: (code) => code === 0, summary: (o) => lastLine(o) },
  { name: 'device-queue (cardputer-safe)', cmd: 'node', args: ['tools/anima-host/device-queue-check.mjs'],
    // The single device queue: light reads pooled, heavy ops (writes + Gemini /api/llm proxy) EXCLUSIVE, FIFO
    // no-starvation. Locks the discipline that keeps the PSRAM-less board from being hit by concurrent requests.
    ok: (code) => code === 0, summary: (o) => lastLine(o) },
  { name: 'unit tests', cmd: 'node', args: ['--test', 'tools/**/*.test.mjs'], ok: (code) => code === 0,
    summary: (o) => (o.match(/[#ℹ] pass \d+/) || ['tests']).concat(o.match(/[#ℹ] fail \d+/) || []).join('  ') },
];

console.log(`${C.b}=== ANIMA unified gate ===${C.x}\n`);
const results = [];
for (const g of gates) {
  process.stdout.write(`  ${C.d}running ${g.name} ...${C.x}\r`);
  clearVolatile();                       // each gate starts from clean SD state — no cross-gate pollution
  const { code, out } = run(g.cmd, g.args);
  const pass = g.ok(code, out);
  results.push({ name: g.name, pass, summary: g.summary(out) });
  console.log(`  ${pass ? C.g + 'PASS' : C.r + 'FAIL'}${C.x}  ${g.name.padEnd(22)} ${C.d}${g.summary(out).slice(0, 76)}${C.x}`);
}

const failed = results.filter((r) => !r.pass).map((r) => r.name);
console.log('\n' + (failed.length ? `${C.r}${C.b}GATE FAILED:${C.x} ${failed.join(', ')}`
                                  : `${C.g}${C.b}ALL ${results.length} GATES PASS${C.x}`));
process.exit(failed.length ? 1 : 0);
