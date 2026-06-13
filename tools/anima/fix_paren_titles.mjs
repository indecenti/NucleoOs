#!/usr/bin/env node
// One-off migration: cards whose title carries a Wikipedia disambiguation parenthetical
// ("Rust (linguaggio di programmazione)") never matched the bare word the user types ("rust").
// Add the de-parenthesized clean name as an alias ask (+ "cos'è"/"what is" forms). Scalable data fix.
import { readFileSync, writeFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const KDIR = join(dirname(fileURLToPath(import.meta.url)), 'knowledge');
let touched = 0, added = 0;
for (const f of readdirSync(KDIR).filter(f => f.startsWith('wiki-') && f.endsWith('.jsonl'))) {
  const p = join(KDIR, f);
  const lines = readFileSync(p, 'utf8').split(/\r?\n/);
  for (let i = 0; i < lines.length; i++) {
    const t = lines[i].trim(); if (!t || t.startsWith('//')) continue;
    let o; try { o = JSON.parse(t); } catch { continue; }
    if (!o.ask || !o.ask.it) continue;
    const title = o.ask.it[0] || '';
    if (!title.includes('(')) continue;
    const clean = title.replace(/\s*\([^)]*\)\s*/g, ' ').trim();
    if (!clean || clean.toLowerCase() === title.toLowerCase()) continue;
    const addIt = [clean, `cos'è ${clean}`, `cosa sai di ${clean}`, `conosci ${clean}`];
    const addEn = [clean, `what is ${clean}`, `what do you know about ${clean}`];
    const haveIt = new Set((o.ask.it || []).map(s => s.toLowerCase()));
    const haveEn = new Set((o.ask.en || []).map(s => s.toLowerCase()));
    let chg = false;
    for (const a of addIt) if (!haveIt.has(a.toLowerCase())) { o.ask.it.push(a); haveIt.add(a.toLowerCase()); added++; chg = true; }
    o.ask.en = o.ask.en || [];
    for (const a of addEn) if (!haveEn.has(a.toLowerCase())) { o.ask.en.push(a); haveEn.add(a.toLowerCase()); added++; chg = true; }
    if (chg) { lines[i] = JSON.stringify(o); touched++; }
  }
  writeFileSync(p, lines.join('\n'));
}
console.log(`parenthetical migration: ${touched} cards, +${added} clean-name asks`);
