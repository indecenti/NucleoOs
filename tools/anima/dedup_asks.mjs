// Remove cross-card duplicate asks from the bulk wiki-* cards: any ask phrasing that appears on >1 card
// is AMBIGUOUS (can't route reliably), so drop it from the wiki cards (curated cards keep theirs; the
// per-card title + cos'è family always survive). Run after enrich_grok, before build_akb2. No duplicates.
import { readFileSync, writeFileSync, readdirSync } from 'node:fs';
import { join } from 'node:path';
const dir = 'tools/anima/knowledge';
const norm = s => s.toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/\s+/g, ' ').trim();
const count = new Map();
for (const f of readdirSync(dir).filter(f => f.endsWith('.jsonl'))) {
  for (const l of readFileSync(join(dir, f), 'utf8').split(/\r?\n/)) {
    const t = l.trim(); if (!t || t.startsWith('//')) continue;
    try { const o = JSON.parse(t); for (const lang of ['it', 'en']) for (const a of (o.ask?.[lang] || [])) count.set(norm(a), (count.get(norm(a)) || 0) + 1); } catch {}
  }
}
let removed = 0;
for (const f of readdirSync(dir).filter(f => f.startsWith('wiki-') && f.endsWith('.jsonl'))) {
  const p = join(dir, f);
  const out = readFileSync(p, 'utf8').split(/\r?\n/).map(l => {
    const t = l.trim(); if (!t || t.startsWith('//')) return l;
    const o = JSON.parse(t);
    for (const lang of ['it', 'en']) {
      if (!o.ask?.[lang]?.length) continue;
      const title = o.ask[lang][0];
      const kept = o.ask[lang].filter((a, i) => i === 0 || a === title || count.get(norm(a)) <= 1);
      removed += o.ask[lang].length - kept.length;
      o.ask[lang] = kept;
    }
    return JSON.stringify(o);
  });
  writeFileSync(p, out.join('\n'));
}
console.log('removed', removed, 'duplicate/ambiguous asks');
