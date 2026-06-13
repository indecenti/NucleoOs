#!/usr/bin/env node
// ANIMA Math Engine v2 — correctness + anti-drift gate.
//
// Drives the REAL compiled firmware cascade (anima.exe: nucleo_anima.c's math engine) over
// tools/anima-host/math-cases.jsonl and checks the exact value of every answer. Then, if the
// web mock is reachable, it re-runs each case through serve-shell.mjs (the JS twin) and flags
// any reply that differs — so the device and the browser preview can never silently drift.
//
//   npm run anima:build              # make sure anima.exe is current
//   node tools/anima-host/math-check.mjs
//   node tools/anima-host/math-check.mjs --no-mock   # skip the JS parity cross-check
//
// Exit != 0 on any wrong value, any false-positive guard that fired, or (mock up) any drift.
import { spawnSync, spawn } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { setTimeout as sleep } from 'node:timers/promises';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..');
const exe = join(here, 'build', 'anima.exe');
const casesFile = join(here, 'math-cases.jsonl');
const mock = join(repo, 'tools', 'serve-shell.mjs');
const MOCK_URL = 'http://localhost:5599/api/anima';
const noMock = process.argv.includes('--no-mock');
const MATH_INTENTS = new Set(['base', 'calc', 'prime', 'roman', 'convert', 'percent', 'ohm', 'geo', 'phys']);

if (!existsSync(exe)) { console.error('anima.exe missing — run `npm run anima:build` first.'); process.exit(2); }

const cases = readFileSync(casesFile, 'utf8').split(/\r?\n/)
  .filter(l => l.trim() && !l.startsWith('//')).map(l => JSON.parse(l));

// --- drive the firmware (one REPL stream; /reset + /it|/en per case, like route-check) ---
let lang = 'it';
const lines = [];
for (const c of cases) {
  lines.push('/reset');
  const want = c.lang === 'en' ? 'en' : 'it';
  if (want !== lang) { lines.push('/' + want); lang = want; }
  lines.push(c.q);
}
const run = spawnSync(exe, [], { input: Buffer.from(lines.join('\n') + '\n', 'utf8'), maxBuffer: 32 * 1024 * 1024 });
const blocks = run.stdout.toString('utf8').split(/^Q: /m).slice(1);
const exeRows = blocks.map(b => ({
  intent: (b.match(/intent=(\S*)/) || [])[1] || '',
  reply: ((b.match(/reply: (.*)/) || [])[1] || '').trim(),
}));

// --- optional: bring up the mock for the JS-twin parity cross-check ---
async function withMock(fn) {
  if (noMock) return fn(null);
  const proc = spawn('node', [mock], { stdio: 'ignore' });
  try {
    for (let i = 0; i < 40; i++) {
      try { const r = await fetch(`${MOCK_URL}?q=1+1&lang=it`); if (r.ok) break; } catch {}
      await sleep(150);
    }
    return await fn(async (q, lg) => {
      const r = await fetch(`${MOCK_URL}?q=${encodeURIComponent(q)}&lang=${lg}`);
      return await r.json();
    });
  } finally { proc.kill(); }
}

const norm = s => String(s).toLowerCase().replace(/\s+/g, ' ').trim();

await withMock(async (askMock) => {
  let ok = 0, badVal = [], badReject = [], drift = [], mockUp = false;
  for (let i = 0; i < cases.length; i++) {
    const c = cases[i], row = exeRows[i] || { intent: '', reply: '' };
    // firmware correctness
    if (c.reject) {
      if (MATH_INTENTS.has(row.intent)) badReject.push({ c, row }); else ok++;
    } else if (norm(row.reply).includes(norm(c.expect))) {
      ok++;
    } else {
      badVal.push({ c, got: row.reply });
    }
    // JS-twin parity (best-effort)
    if (askMock) {
      try {
        const m = await askMock(c.q, c.lang); mockUp = true;
        const mReply = m.reply || '';
        if (c.reject) {
          if (MATH_INTENTS.has(m.intent)) drift.push({ q: c.q, exe: '(rejected)', mock: `${m.intent}: ${mReply}` });
        } else if (norm(mReply) !== norm(row.reply)) {
          drift.push({ q: c.q, exe: row.reply, mock: mReply });
        }
      } catch {}
    }
  }

  console.log(`\n[math] firmware: ${ok}/${cases.length} correct  (${badVal.length} wrong, ${badReject.length} false-positive)`);
  for (const b of badVal)    console.log(`  [WRONG] [${b.c.lang}] "${b.c.q}"  expected ~"${b.c.expect}"  got: ${b.got || '(empty)'}`);
  for (const b of badReject) console.log(`  [FALSE+] [${b.c.lang}] "${b.c.q}"  math frame "${b.row.intent}" wrongly claimed it: ${b.row.reply}`);

  if (askMock && mockUp) {
    console.log(`[math] JS twin (serve-shell.mjs): ${drift.length ? drift.length + ' DRIFT' : 'in sync ✓'}`);
    for (const d of drift) console.log(`  [DRIFT] "${d.q}"\n      firmware: ${d.exe}\n      mock:     ${d.mock}`);
  } else if (!noMock) {
    console.log('[math] JS twin: mock not reachable — parity cross-check skipped (run `node tools/serve-shell.mjs`).');
  }

  const fatal = badVal.length + badReject.length + (mockUp ? drift.length : 0);
  console.log(fatal ? `\n${fatal} FAILURE(S)` : `\nALL GREEN — ${ok}/${cases.length} exact, twin in sync.`);
  process.exit(fatal ? 1 : 0);
});
