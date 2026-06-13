#!/usr/bin/env node
// enrich_ollama.mjs — LOCAL ollama as a question-teacher. For each knowledge card it asks ollama
// for many bilingual paraphrase QUESTIONS (never answers -> zero hallucination), then runs the
// qgen quality gate: on-topic only, deduped, and — the new part vs enrich_grok — rejected if they
// COLLIDE with another card's asks (the thing that makes a shallow encoder return the wrong fact).
// Lexical collision by default; --embed uses ollama embeddings for semantic collision.
//
// Usage: node tools/anima/enrich_ollama.mjs [--file=wiki-electronics.jsonl] [--limit=N] [--cap=10]
//                                           [--model=llama3.1:8b] [--embed] [--collide=0.84] [--dry]
import { readFileSync, writeFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { available, genJSON, makeEmbedder, listModels } from './ollama.mjs';
import { acceptVariants, foreignIndex, cosine, jaccard, normQ } from './qgen.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const KDIR = join(here, 'knowledge');
const arg = (k, d) => { const a = process.argv.find((x) => x.startsWith(`--${k}=`)); return a ? a.split('=')[1] : d; };
const ONLY = arg('file', '');
const LIMIT = parseInt(arg('limit', '1000000'), 10);
const CAP = parseInt(arg('cap', '10'), 10);
const MODEL = arg('model', 'llama3.1:8b');
const EMB_MODEL = arg('embModel', 'nomic-embed-text');
const COLLIDE = parseFloat(arg('collide', '0.84'));
const USE_EMB = process.argv.includes('--embed');
const DRY = process.argv.includes('--dry');

const SYS = 'You generate PARAPHRASE QUESTIONS for an offline search engine, in Italian and English. '
  + 'Given a topic and its short answer, output ONLY different ways a human would ASK for that topic. '
  + 'HARD RULES: (1) questions only, never answers, facts, dates or numbers; (2) stay strictly ON the topic '
  + '(no sub-topics, applications, or more specific questions); (3) short (2-8 words); (4) 6-10 per language. '
  + 'Reply ONLY JSON: {"it":[...],"en":[...]}.';

function loadAll() {
  const files = ONLY ? [ONLY] : readdirSync(KDIR).filter((f) => f.endsWith('.jsonl'));
  const byFile = {}, all = [];
  for (const f of files) {
    const cards = [];
    for (const ln of readFileSync(join(KDIR, f), 'utf8').split('\n')) {
      const t = ln.trim(); if (!t) continue;
      try { const c = JSON.parse(t); cards.push(c); all.push(c); } catch {}
    }
    byFile[f] = cards;
  }
  return { files, byFile, all };
}
const save = (f, cards) => writeFileSync(join(KDIR, f), cards.map((c) => JSON.stringify(c)).join('\n') + '\n');
const topicOf = (c) => [c.reply?.it, c.reply?.en, (c.ask?.it || [])[0], (c.ask?.en || [])[0]].filter(Boolean).join(' — ');
const capUniq = (arr, n) => { const seen = new Set(), out = []; for (const q of arr) { const k = normQ(q); if (k && !seen.has(k)) { seen.add(k); out.push(q); } if (out.length >= n) break; } return out; };

async function main() {
  if (!(await available())) {
    console.error(`ollama not reachable at the configured host. Start it (\`ollama serve\`) and pull a model (\`ollama pull ${MODEL}\`).`);
    process.exit(2);
  }
  const have = await listModels();
  if (have.length && !have.some((m) => m === MODEL || m.startsWith(MODEL.split(':')[0]))) {
    console.error(`model "${MODEL}" not installed. Available: ${have.join(', ') || '(none)'} — pull it with \`ollama pull ${MODEL}\`.`);
    process.exit(2);
  }

  const { files, byFile, all } = loadAll();
  const fidxIt = foreignIndex(all, { lang: 'it' });
  const fidxEn = foreignIndex(all, { lang: 'en' });

  // optional semantic collision: memoised embedder + a sync sim that reads a per-call vector map.
  const embedder = USE_EMB ? makeEmbedder({ model: EMB_MODEL }) : null;
  async function simFor(cands, foreign) {
    if (!USE_EMB) return jaccard;
    const vmap = new Map();
    const strs = [...new Set([...cands, ...foreign.map((f) => f.q)])];
    await Promise.all(strs.map(async (s) => vmap.set(s, await embedder(s))));
    return (a, b) => { const va = vmap.get(a), vb = vmap.get(b); return va && vb ? cosine(va, vb) : jaccard(a, b); };
  }

  let processed = 0, addedIt = 0, addedEn = 0;
  for (const f of files) {
    let dirty = false;
    for (const c of byFile[f]) {
      if (processed >= LIMIT) break;
      processed++;
      const raw = await genJSON(SYS, `Topic: ${topicOf(c)}\nCard id: ${c.id}`, { model: MODEL });
      if (!raw) continue;
      c.ask = c.ask || {};
      for (const [lang, fidx, addRef] of [['it', fidxIt, 'it'], ['en', fidxEn, 'en']]) {
        const cands = Array.isArray(raw[lang]) ? raw[lang] : [];
        if (!cands.length) continue;
        const existing = c.ask[lang] || [];
        const sim = await simFor(cands, fidx);
        const accepted = acceptVariants(cands, { selfId: c.id, topicText: topicOf(c), existing, foreign: fidx, collideTh: COLLIDE, sim, max: CAP });
        if (accepted.length) {
          c.ask[lang] = capUniq([...existing, ...accepted], CAP);
          // keep the foreign index live so later cards in this run also avoid the freshly-added asks
          for (const q of accepted) (addRef === 'it' ? fidxIt : fidxEn).push({ q, id: c.id });
          if (addRef === 'it') addedIt += accepted.length; else addedEn += accepted.length;
          dirty = true;
        }
      }
      process.stdout.write(`\r  ${processed} cards · +${addedIt} it · +${addedEn} en   `);
    }
    if (dirty && !DRY) save(f, byFile[f]);
  }
  console.log(`\n${DRY ? '[dry] ' : ''}done: ${processed} cards processed, +${addedIt} IT asks, +${addedEn} EN asks${USE_EMB ? ' (semantic collision)' : ''}.`);
}

main().catch((e) => { console.error(e); process.exit(1); });
