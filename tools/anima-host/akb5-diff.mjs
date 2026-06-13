#!/usr/bin/env node
// AKB5 ↔ FLAT divergence audit. Drives the REAL exe twice over the SAME large query set — once flat, once
// with ANIMA_AKB5=1 — and classifies every query where the two modes disagree, so a fix targets the REAL
// root cause instead of a guess. Query set = every eval_*.jsonl `q` + one ask per corpus/staged card.
// Output: a JSON report (build/akb5-diff.json) + a console summary. Exit 0 always (this is a measurement).
import { spawnSync } from 'node:child_process';
import { existsSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..');
const exe = join(here, 'build', 'anima.exe');
if (!existsSync(exe)) { console.error('anima.exe missing — build first.'); process.exit(2); }

const norm = (s) => (s || '').normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase().replace(/\s+/g, ' ').trim();
const isRefusal = (s) => /not a place|not a person|non (è|e) (un|una) (luogo|persona|posto)|did you mean|non sono sicuro|^intendi |non ho (informazioni|dettagli|altri)|i (don'?t|do not) (have|know)|più (di )?dettagli|niente corpo/i.test(s || '');

// ---- assemble the query set -----------------------------------------------------------------------
const ADV_FILES = /halluc|traps|bigjunk|ood|typegate|paint_decline|paint_stress|xskill|skills[345]/;  // must-abstain-ish
const queries = [];                 // {q, lang, src, kind: 'adversarial'|'recall'|'mixed', expectReply?}
const seen = new Set();
const add = (q, lang, src, kind, expectReply) => {
  const k = norm(q) + '|' + (lang || 'it');
  if (!q || q.length < 2 || seen.has(k)) return; seen.add(k);
  queries.push({ q, lang: lang || 'it', src, kind, expectReply });
};
// eval files
for (const dir of [join(repo, 'tools', 'anima'), here]) {
  for (const f of readdirSync(dir)) {
    if (!/^eval_.*\.jsonl$/.test(f)) continue;
    const kind = ADV_FILES.test(f) ? 'adversarial' : 'mixed';
    for (const l of readFileSync(join(dir, f), 'utf8').split('\n')) {
      if (!l.trim() || l.trim().startsWith('//')) continue;
      try { const o = JSON.parse(l); if (o.q) add(o.q, o.lang || (/_en|_swe/.test(f) ? 'en' : 'it'), f, kind); } catch {}
    }
  }
}
// corpus + staged asks (one IT + one EN per card), tagged recall with the card's own reply as ground truth
for (const dir of [join(repo, 'tools', 'anima', 'knowledge'), join(repo, 'tools', 'anima', 'knowledge.staged')]) {
  if (!existsSync(dir)) continue;
  for (const f of readdirSync(dir)) {
    if (!f.endsWith('.jsonl')) continue;
    for (const l of readFileSync(join(dir, f), 'utf8').split('\n')) {
      if (!l.trim() || l.trim().startsWith('//')) continue;
      let o; try { o = JSON.parse(l); } catch { continue; }
      if ((o.action ?? 'answer') !== 'answer' || !o.ask) continue;
      for (const lang of ['it', 'en']) {
        const a = o.ask?.[lang]; if (Array.isArray(a) && a[0]) add(a[0], lang, 'corpus:' + f, 'recall', o.reply?.[lang] || o.reply?.it || o.reply?.en);
      }
    }
  }
}

// ---- run the exe over the whole set in one batch (flat, then AKB5) --------------------------------
function run(akb5) {
  const lines = []; let lang = 'it';
  for (const it of queries) { lines.push('/reset'); if (it.lang !== lang) { lines.push('/' + it.lang); lang = it.lang; } lines.push(it.q); }
  const r = spawnSync(exe, [], {
    input: Buffer.from(lines.join('\n') + '\n', 'utf8'),
    maxBuffer: 256 * 1024 * 1024,
    env: akb5 ? { ...process.env, ANIMA_AKB5: '1' } : { ...process.env, ANIMA_AKB5: '' },
  });
  const map = new Map();
  for (const b of r.stdout.toString('utf8').split(/^Q: /m).slice(1)) {
    const q = norm(b.split('\n')[0]);
    const tier = (b.match(/tier=(\S+)/) || [])[1] || 'none';
    let reply = (b.match(/reply:\s*([\s\S]*?)\s*$/m) || [])[1] || '';
    reply = reply.replace(/\s+/g, ' ').trim();
    if (reply === '(vuoto)' || reply === '(empty)') reply = '';
    map.set(q, { tier, reply, answered: tier !== 'none' && reply !== '' });
  }
  return map;
}
console.error(`[akb5-diff] running ${queries.length} queries × 2 modes ...`);
const flat = run(false);
const akb5 = run(true);

// ---- classify -------------------------------------------------------------------------------------
const sameReply = (a, b) => { const na = norm(a), nb = norm(b); return na && (na.startsWith(nb.slice(0, 40)) || nb.startsWith(na.slice(0, 40))); };
const report = [];
const counts = {};
const bump = (k) => counts[k] = (counts[k] || 0) + 1;
for (const it of queries) {
  const k = norm(it.q);
  const F = flat.get(k) || { answered: false, reply: '' };
  const A = akb5.get(k) || { answered: false, reply: '' };
  let cls;
  if (!F.answered && !A.answered) cls = 'both_abstain';
  else if (F.answered && A.answered) cls = sameReply(F.reply, A.reply) ? 'both_same' : 'both_diff';
  else if (F.answered && !A.answered) cls = 'akb5_lost';     // recall regression
  else cls = 'akb5_gained';                                  // AKB5 answers where flat abstained
  bump(cls);
  if (cls === 'both_same' || cls === 'both_abstain') continue;   // only record divergences
  // SAFETY flag: AKB5 gives a confident (non-refusal) answer where flat did not (gained), or a DIFFERENT
  // answer (diff). On adversarial queries this is a hallucination candidate; on recall queries a 'diff'
  // that doesn't match the expected card is a misattribution candidate.
  const akb5Confident = A.answered && !isRefusal(A.reply);
  let unsafe = false, why = '';
  if (cls === 'akb5_gained' && akb5Confident && it.kind === 'adversarial') { unsafe = true; why = 'gained-on-adversarial'; }
  if (cls === 'both_diff' && akb5Confident && it.kind === 'adversarial') { unsafe = true; why = 'diff-on-adversarial'; }
  if (cls === 'both_diff' && it.kind === 'recall' && it.expectReply && !sameReply(A.reply, it.expectReply) && akb5Confident) { unsafe = true; why = 'misattribution-vs-expected'; }
  report.push({ q: it.q, lang: it.lang, src: it.src, kind: it.kind, cls, unsafe, why,
    flat: F.reply.slice(0, 80), akb5: A.reply.slice(0, 80), akb5Refusal: A.answered && isRefusal(A.reply) });
}

const unsafe = report.filter(r => r.unsafe);
const lost = report.filter(r => r.cls === 'akb5_lost');
const gained = report.filter(r => r.cls === 'akb5_gained');
const diff = report.filter(r => r.cls === 'both_diff');
writeFileSync(join(here, 'build', 'akb5-diff.json'), JSON.stringify({ counts, total: queries.length, report }, null, 1));

console.log(`\n[akb5-diff] ${queries.length} queries  ` + JSON.stringify(counts));
console.log(`[akb5-diff] divergences: akb5_lost=${lost.length} (recall ↓, safe)  akb5_gained=${gained.length}  both_diff=${diff.length}  UNSAFE-candidates=${unsafe.length}`);
if (unsafe.length) {
  console.log('\nUNSAFE CANDIDATES (AKB5 confidently answers where flat abstained / differently, on a trap):');
  for (const u of unsafe.slice(0, 40)) console.log(`  [${u.why}] (${u.src}) "${u.q}" -> "${u.akb5}"`);
}
console.log(`\n[akb5-diff] full report -> tools/anima-host/build/akb5-diff.json  (lost/gained/diff lists inside)`);
