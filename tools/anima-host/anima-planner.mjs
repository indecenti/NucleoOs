#!/usr/bin/env node
// anima-planner.mjs — PROTOTYPE of compositional understanding for ANIMA. Today the cascade answers ONE
// fact per query. A real assistant must understand a MULTI-PART request: decompose it into a deterministic
// PLAN of atomic sub-queries the cascade CAN answer, run them on the REAL compiled cascade (anima.exe),
// and COMPOSE the result — no LLM, fully offline, deterministic, and honest (it states what it couldn't find).
//
// This host prototype proves the capability end-to-end before it's integrated into the firmware orchestrator
// (nucleo_anima.c) — which is currently being edited by another session, so we don't touch it.
//   Patterns: CONJUNCTION ("di X dimmi nascita e nazionalità"), COMPARISON ("chi è nato prima, X o Y").
// Usage: node tools/anima-host/anima-planner.mjs "di Einstein dimmi quando è nato e la nazionalità"
import { spawnSync } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const EXE = join(dirname(fileURLToPath(import.meta.url)), 'build', 'anima.exe');
const fold = s => s.normalize('NFD').replace(/\p{Diacritic}/gu, '').toLowerCase();

// Run a BATCH of sub-queries through the one real cascade process; return replies in order ('' = abstained).
function ask(queries) {
  const lines = ['/it'];
  for (const q of queries) { lines.push('/reset'); lines.push(q); }
  const r = spawnSync(EXE, [], { input: lines.join('\n') + '\n', encoding: 'utf8', maxBuffer: 1 << 24 });
  const out = (r.stdout || '');
  const byQ = new Map();
  const re = /Q: (.*?)\r?\n([\s\S]*?)reply: (.*?)\r?\n/g; let m;
  while ((m = re.exec(out))) {
    const rep = m[3].trim();
    byQ.set(m[1].trim(), (rep === '(vuoto)' || rep === '(empty)') ? '' : rep);
  }
  return queries.map(q => byQ.get(q) ?? '');
}

// --- value extractors from a cascade reply ---
const yearOf = r => { const m = r.match(/\b(1[0-9]{3}|20[0-2][0-9])\b/); return m ? +m[1] : null; };
const bornText = r => { const m = r.match(/nato\/a il (.+?)\.?$/i) || r.match(/born on (.+?)\.?$/i); return m ? m[1].replace(/\.$/, '') : null; };
const countryOf = r => { const m = r.match(/\b(?:e|è) di (.+?)\.?$/i) || r.match(/is from (.+?)\.?$/i); return m ? m[1].replace(/\.$/, '') : null; };
const nameOf = r => { const m = r.match(/^([A-ZÀ-Ý][\wÀ-ý'’]*(?: [A-ZÀ-Ý][\wÀ-ý'’.]*){0,3})/); return m ? m[1] : null; };

// --- extract the salient proper-noun spans (candidate entities) ---
function entities(req) {
  const lead = /\b(di|su|sulla|del|della|tra|fra|o|e|chi|cosa|dimmi|nato|nata|nascita|nazionalita|nazionalità|paese|quando|prima|dopo|piu|più|vecchio|giovane|la|il|lo|dov|dove|che|del|raccontami)\b/gi;
  // grab capitalized runs
  const caps = req.match(/[A-ZÀ-Ý][\wÀ-ý'’.]*(?:\s+[A-ZÀ-Ý][\wÀ-ý'’.]*)*/g) || [];
  return caps.map(s => s.trim()).filter(s => s.length >= 3 && !/^(Di|Su|Chi|Cosa|Dimmi|Quando|La|Il)$/i.test(s));
}

function plan(req) {
  const f = fold(req);
  const wantBorn = /\b(nato|nata|nascita|quando .* nat|data di nascita)\b/.test(f);
  const wantCountry = /\b(nazionalita|nazione|paese|da dove viene|di dov|di che paese)\b/.test(f);
  const isCompare = /\b(piu (vecchio|giovane|grande)|nato (prima|dopo)|chi e (piu|nato))\b/.test(f);
  const ents = entities(req);

  // --- COMPARISON: "chi è nato prima/più vecchio, X o Y" -> decompose to born(X), born(Y), compare ---
  if (isCompare && ents.length >= 2) {
    const [X, Y] = ents;
    const olderWanted = /\b(piu vecchio|nato prima)\b/.test(f);   // else younger / born-after
    const [rx, ry] = ask([`quando è nato ${X}`, `quando è nato ${Y}`]);
    const yx = yearOf(rx), yy = yearOf(ry);
    if (yx == null || yy == null) {
      const miss = yx == null ? X : Y;
      return { plan: ['born ' + X, 'born ' + Y], reply: `Non ho la data di nascita di ${miss}, quindi non posso confrontarli.` };
    }
    const older = yx < yy ? X : Y, younger = yx < yy ? Y : X;
    const pick = olderWanted ? older : younger;
    return {
      plan: [`born(${X})=${yx}`, `born(${Y})=${yy}`, olderWanted ? 'min' : 'max'],
      reply: `${pick}. ${X} è nato nel ${yx}, ${Y} nel ${yy}${olderWanted ? ` — ${older} è il più vecchio.` : ` — ${younger} è il più giovane.`}`,
    };
  }

  // --- CONJUNCTION: multi-attribute about ONE entity -> run each attribute sub-query, compose ---
  if ((wantBorn || wantCountry) && ents.length >= 1) {
    const E = ents[0];
    const subs = [], kinds = [];
    if (wantBorn) { subs.push(`quando è nato ${E}`); kinds.push('born'); }
    if (wantCountry) { subs.push(`di che nazionalità è ${E}`); kinds.push('country'); }
    const reps = ask(subs);
    const parts = [];
    let canon = E;
    reps.forEach((r, i) => {
      if (kinds[i] === 'born') { const b = bornText(r); if (r) canon = nameOf(r) || canon; if (b) parts.push(`è nato/a ${b}`); }
      if (kinds[i] === 'country') { const c = countryOf(r); if (r) canon = nameOf(r) || canon; if (c) parts.push(`è di ${c}`); }
    });
    if (!parts.length) return { plan: subs, reply: `Non ho informazioni su ${E}.` };
    // honest: name the attributes we DID find; note any missing
    const missing = [];
    if (wantBorn && !parts.some(p => p.includes('nato'))) missing.push('la data di nascita');
    if (wantCountry && !parts.some(p => p.includes('di '))) missing.push('la nazionalità');
    let reply = `${canon} ${parts.join(' ed ')}.`;
    if (missing.length) reply += ` (Non ho ${missing.join(' né ')}.)`;
    return { plan: subs, reply };
  }

  return { plan: [], reply: null };   // not a compositional request -> defer to the normal cascade
}

const req = process.argv.slice(2).join(' ');
const demo = req ? [req] : [
  'di Einstein dimmi quando è nato e la nazionalità',
  'quando è nato e di dov\'è Dante?',
  'chi è nato prima, Einstein o Dante?',
  'chi è più vecchio tra Newton e Einstein?',
  'di Einstein dimmi nascita e nazionalità e chi era suo padre',   // partial-knowledge -> honest
];
for (const r of demo) {
  const p = plan(r);
  console.log(`\nREQ: ${r}`);
  console.log(`  PLAN: [${p.plan.join('  +  ')}]`);
  console.log(`  ANIMA: ${p.reply ?? '(non compositiva — passa alla cascata normale)'}`);
}
