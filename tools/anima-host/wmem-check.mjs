#!/usr/bin/env node
// WORKING-MEMORY gate — the conversational state that lets a follow-up mean something, and the
// guards that stop it from meaning too much.
//
// ANIMA already carried entity slots (last app / last file / last topic) and an 8-turn ring, so
// "aprilo" and "chiudilo" resolved. What it could NOT do was continue a KNOWLEDGE thread: the
// structured focus shift only fires when the KGE reasoner declared a (subject, relation), and an L1
// card answer declares neither — so "chi era einstein" / "e newton?" abstained on a question whose
// meaning was obvious. The topic-FRAME carry-over closes that, and this gate holds both halves.
//
// The interesting half is the guards, not the feature. A follow-up mechanism that fires too eagerly
// is a hallucination engine: it turns an unrelated fragment into a confident answer about whatever
// the thread happened to be. So every positive case here is paired with a negative one —
//   * a COLD session may never resolve a follow-up (nothing to continue),
//   * a STALE frame (past the recency window) may never resolve one,
//   * /reset must forget,
//   * an unknown entity must still ABSTAIN rather than borrow the previous answer,
//   * a self-contained question must never be re-aimed by the thread.
//
//   node tools/anima-host/wmem-check.mjs [--show]
import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const exe  = join(here, 'build', 'anima.exe');
if (!existsSync(exe)) { console.error('anima.exe missing — run `npm run anima:build`.'); process.exit(2); }
const show = process.argv.includes('--show');

// A scenario is a named list of turns; each turn may assert on the result. Turns share one session
// unless a turn says `reset: true` — that is the point, so the assertions can be about carry-over.
function runScenario(sc) {
  const lines = ['/reset'];
  let lang = 'it';
  for (const t of sc.turns) {
    if (t.reset) lines.push('/reset');
    const want = t.lang || sc.lang || 'it';
    if (want !== lang) { lines.push('/' + want); lang = want; }
    lines.push(t.q);
  }
  const r = spawnSync(exe, [], { input: Buffer.from(lines.join('\n') + '\n', 'utf8'), maxBuffer: 64 * 1024 * 1024 });
  const blocks = r.stdout.toString('utf8').split(/^Q: /m).slice(1);
  return sc.turns.map((t, i) => {
    const b = blocks[i] || '';
    return {
      ...t,
      tier:      (b.match(/tier=(\S+)/) || [])[1] || 'none',
      intent:    (b.match(/intent=(\S*)/) || [])[1] || '',
      corrected: ((b.match(/\(interpretato: "(.*)"\)/) || [])[1] || ''),
      reply:     ((b.match(/reply: (.*)/) || [])[1] || '').trim(),
    };
  });
}

// Filler turns used to age the frame past its 8-turn recency window. They must not themselves answer
// with the thread's subject, so they are plain unanswerable noise.
const FILL = (n) => Array.from({ length: n }, (_, i) => ({ q: `zzqq filler ${i} wubba` }));

const SCENARIOS = [
  // ---- entity slots: the behaviour that already worked, now pinned -----------------------------
  { name: 'app slot resolves "aprilo"', turns: [
      { q: 'apri il blocco note', want: /notepad|blocco/i },
      { q: 'aprilo',              want: /notepad|blocco/i, wantTier: /L0/ } ] },
  { name: 'app slot resolves "chiudilo"', turns: [
      { q: 'apri il blocco note' },
      { q: 'chiudilo', wantTier: /L0/ } ] },

  // ---- topic-frame carry-over: the gap this closes ---------------------------------------------
  { name: 'IT knowledge thread continues with a new entity', turns: [
      { q: 'chi era einstein', want: /einstein/i },
      { q: 'e newton?',        want: /newton/i, wantCorrected: /newton/i } ] },
  { name: 'EN knowledge thread continues with a new entity', lang: 'en', turns: [
      { q: 'who was einstein', want: /einstein/i },
      { q: 'and newton?',      want: /newton/i } ] },
  { name: 'structured (relation) shift still wins where it applies', turns: [
      { q: 'capitale della francia', want: /parigi/i },
      { q: 'e la germania?',         want: /berlino/i } ] },
  // Three turns deep: the frame must survive its own re-use, not just the first hop. (Entities are
  // picked from cards the host fixture actually holds — a corpus gap here would look like a carry-over
  // bug, which is exactly the confusion the "unknown entity abstains" scenario below pins down.)
  { name: 'the thread chains across three turns', turns: [
      { q: 'chi era einstein', want: /einstein/i },
      { q: 'e newton?',        want: /newton/i },
      { q: 'e darwin?',        want: /darwin/i } ] },

  // ---- the roadmap's own step-6 acceptance cases (docs/anima-roadmap.md) -------------------------
  // Both need the frame to survive a NON-answer turn (a launch, a system query) and the entity to be
  // punched out of the MIDDLE of the turn ("che |batteria| ho"), not just off its end.
  { name: 'roadmap: "apri le foto" -> "no, la musica"', turns: [
      { q: 'apri le foto',  want: /photo/i },
      { q: 'no, la musica', want: /media|music/i, wantTier: /L0/ } ] },
  { name: 'roadmap: "che batteria ho?" -> "e lo spazio?"', turns: [
      { q: 'che batteria ho?', want: /batteria/i },
      { q: 'e lo spazio?',     want: /spazio|SD/i, wantTier: /L0/ } ] },

  // ---- the guards -------------------------------------------------------------------------------
  { name: 'a COLD session cannot resolve a follow-up', turns: [
      { q: 'e newton?', wantTier: /none/ } ] },
  { name: '/reset forgets the thread', turns: [
      { q: 'chi era einstein', want: /einstein/i },
      { q: 'e newton?', reset: true, wantTier: /none/ } ] },
  { name: 'a STALE frame (past the recency window) does not fire', turns: [
      { q: 'chi era einstein', want: /einstein/i },
      ...FILL(9),
      { q: 'e newton?', wantTier: /none/ } ] },
  { name: 'an unknown entity ABSTAINS instead of borrowing the answer', turns: [
      { q: 'chi era einstein', want: /einstein/i },
      { q: 'e zzqqwubba?', wantTier: /none/, notWant: /einstein/i } ] },
  { name: 'a self-contained question is never re-aimed by the thread', turns: [
      { q: 'chi era einstein', want: /einstein/i },
      { q: 'capitale della francia', want: /parigi/i, notWant: /einstein/i } ] },
  { name: 'a follow-up never invents when the thread had no answer', turns: [
      { q: 'chi era zzqqwubba', wantTier: /none/ },
      { q: 'e newton?', notWant: /zzqqwubba/i } ] },
  // A COMMAND frame must not launch something at random when the follow-up doesn't fit it. The
  // rebuilt turn goes through the real router, so "apri le newton" finds no app and abstains.
  { name: 'a command frame does not launch a bogus app', turns: [
      { q: 'apri le foto', want: /photo/i },
      { q: 'e newton?', wantTier: /none/ } ] },
];

const fails = [];
for (const sc of SCENARIOS) {
  const res = runScenario(sc);
  for (let i = 0; i < res.length; i++) {
    const t = res[i];
    if (show) console.log(`  [${sc.name}] "${t.q}" -> ${t.tier}${t.corrected ? ` (as "${t.corrected}")` : ''} "${t.reply.slice(0, 70)}"`);
    if (t.want && !t.want.test(t.reply))
      fails.push(`${sc.name} — turn ${i + 1} "${t.q}": reply does not match ${t.want} (got "${t.reply.slice(0, 70)}")`);
    if (t.notWant && t.notWant.test(t.reply))
      fails.push(`${sc.name} — turn ${i + 1} "${t.q}": reply LEAKED ${t.notWant} (got "${t.reply.slice(0, 70)}")`);
    if (t.wantTier && !t.wantTier.test(t.tier))
      fails.push(`${sc.name} — turn ${i + 1} "${t.q}": tier ${t.tier} does not match ${t.wantTier}`);
    if (t.wantCorrected && !t.wantCorrected.test(t.corrected))
      fails.push(`${sc.name} — turn ${i + 1} "${t.q}": re-asked as "${t.corrected}", expected ${t.wantCorrected}`);
  }
}

const turnCount = SCENARIOS.reduce((s, sc) => s + sc.turns.length, 0);
console.log(`[wmem] ${SCENARIOS.length} scenarios, ${turnCount} turns`);
if (fails.length) {
  console.log(`WORKING-MEMORY DEFECTS (${fails.length}):`);
  for (const f of fails) console.log(`  ✗ ${f}`);
  process.exit(1);
}
console.log('✓ follow-ups resolve when there is a thread, and never when there is not.');
