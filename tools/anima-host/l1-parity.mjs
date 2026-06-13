#!/usr/bin/env node
// ANIMA L1 fast-path PARITY gate — the strongest possible test of the AKB4 popcount prefilter:
// for a large bilingual (EN/IT) request set, drive the REAL compiled cascade (anima.exe) TWICE on
// the same binary & AKB4 index — once with the prefilter (default) and once forced onto the
// brute-force exact path (ANIMA_L1_EXACT=1) — and assert byte-identical routing + reply for every
// query. If the prefilter ever changed an answer vs exhaustive search, this fails. Plus an optional
// `expect` substring (accent/case-insensitive) per item pins high-confidence answers as a sanity net.
//
//   node tools/anima-host/l1-parity.mjs            # parity + expect (the gate)
//   node tools/anima-host/l1-parity.mjs --verbose  # also print every parsed answer
import { spawnSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..');
const exe  = join(here, 'build', 'anima.exe');
const qfile = join(repo, 'tools', 'anima', 'eval_l1_requests.jsonl');
const verbose = process.argv.includes('--verbose');

if (!existsSync(exe)) { console.error('anima.exe missing — run `npm run anima:build` first.'); process.exit(2); }

const items = readFileSync(qfile, 'utf8').split(/\r?\n/)
  .filter(l => l.trim() && !l.startsWith('//')).map(l => JSON.parse(l));

// One REPL stream: /reset (clean session) + /it|/en (language) before each query.
let lang = 'it';
const lines = [];
for (const it of items) {
  lines.push('/reset');
  const want = (it.lang === 'en') ? 'en' : 'it';
  if (want !== lang) { lines.push('/' + want); lang = want; }
  lines.push(it.q);
}
const input = Buffer.from(lines.join('\n') + '\n', 'utf8');

function drive(extraEnv) {
  const r = spawnSync(exe, [], { input, maxBuffer: 32 * 1024 * 1024, env: { ...process.env, ...extraEnv } });
  const out = r.stdout.toString('utf8');
  const blocks = out.split(/^Q: /m).slice(1);
  return blocks.map(b => {
    const tier   = (b.match(/tier=(\S+)/) || [])[1] || 'none';
    const action = (b.match(/action=(\S+)/) || [])[1] || 'none';
    const intent = (b.match(/intent=(\S*)/) || [])[1] || '';
    let reply = (b.match(/reply:\s*([\s\S]*?)\s*$/) || [])[1] || '';
    reply = reply.replace(/\s+/g, ' ').trim();
    if (reply === '(vuoto)' || reply === '(empty)') reply = '';   // harness marker for no reply
    return { tier, action, intent, reply, sig: `${tier}|${action}|${intent}|${reply}` };
  });
}

const norm = s => s.normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase();

console.log(`[l1-parity] driving ${items.length} bilingual requests x2 (prefilter vs forced-exact) ...`);
const fast  = drive({});                       // AKB4 popcount prefilter (default)
const exact = drive({ ANIMA_L1_EXACT: '1' });  // brute-force exact path, same binary & index

if (fast.length !== items.length || exact.length !== items.length) {
  console.error(`[l1-parity] block count mismatch: items=${items.length} fast=${fast.length} exact=${exact.length}`);
  process.exit(1);
}

let parityDiffs = 0, expectMiss = 0, expectSkip = 0, answered = 0, refused = 0;
const byLang = { it: 0, en: 0 };
for (let i = 0; i < items.length; i++) {
  const it = items[i], f = fast[i], e = exact[i];
  byLang[it.lang === 'en' ? 'en' : 'it']++;
  const isAnswered = f.tier !== 'none' && f.reply !== '';
  if (isAnswered) answered++; else refused++;
  if (f.sig !== e.sig) {
    parityDiffs++;
    console.log(`  [PARITY] "${it.q}"\n      fast : ${f.sig}\n      exact: ${e.sig}`);
  }
  if (it.expect) {
    if (!isAnswered) expectSkip++;                                // escalated to the stubbed online tier (no Wi-Fi on host)
    else if (!norm(f.reply).includes(norm(it.expect))) {
      expectMiss++;
      console.log(`  [EXPECT] "${it.q}" — want ~"${it.expect}" got "${f.reply}"`);
    }
  }
  if (verbose) console.log(`  ${it.lang} | ${it.q}  ->  ${f.tier}/${f.action}/${f.intent}  ${f.reply ? '“'+f.reply+'”' : '(refused/online)'}`);
}

console.log(`[l1-parity] ${items.length} requests (IT ${byLang.it}, EN ${byLang.en}) — answered offline ${answered}, escalated ${refused}`);
console.log(`[l1-parity] prefilter-vs-exact parity diffs: ${parityDiffs}   expect misses: ${expectMiss}   (expect skipped, online-only: ${expectSkip})`);
if (parityDiffs === 0 && expectMiss === 0) { console.log('[l1-parity] ✓ prefilter is answer-identical to exhaustive search on all bilingual requests'); process.exit(0); }
process.exit(1);
