#!/usr/bin/env node
// Grok-as-question-teacher: enrich source-verified cards with QUALITY bilingual paraphrase QUESTIONS.
// The LLM NEVER writes answers (those stay the Wikipedia/Wikidata source -> zero hallucination): it only
// produces more ways a human would ASK, so ANIMA's shallow encoder recalls each fact from many phrasings
// (better comprehension). Validated, deduped, capped. Reads the Groq key read-only from the device SD.
//
// Usage: node tools/anima/enrich_grok.mjs [--file wiki-electronics.jsonl] [--limit N] [--cap 10] [--dry]
import { readFileSync, writeFileSync, readdirSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { setTimeout as sleep } from 'node:timers/promises';

const here = dirname(fileURLToPath(import.meta.url));
const KDIR = join(here, 'knowledge');
const arg = (k, d) => { const a = process.argv.find(x => x.startsWith(`--${k}=`)); return a ? a.split('=')[1] : d; };
const ONLY = process.argv.find(x => x.startsWith('--file=') )?.split('=')[1];
const LIMIT = parseInt(arg('limit', '1000000'), 10);
const CAP = parseInt(arg('cap', '10'), 10);          // max asks per language per card (templated + grok)
const DRY = process.argv.includes('--dry');
const BATCH = 6;                                       // topics per Groq call (economical)

// --- Groq config, read-only from the device SD (never modified/deleted) ---
const TCFG = ['H:/data/anima/teacher.json', join(here, '..', '..', 'tools', 'sd-sim', 'data', 'anima', 'teacher.json')]
  .find(p => existsSync(p));
if (!TCFG) { console.error('teacher.json not found (connect SD at H: or place it in sd-sim)'); process.exit(2); }
const cfg = JSON.parse(readFileSync(TCFG, 'utf8'));
const BASE = cfg.base || 'https://api.groq.com/openai/v1';
const MODEL = cfg.model || 'llama-3.1-8b-instant';
const KEY = cfg.key;
if (!KEY) { console.error('no key in teacher.json'); process.exit(2); }

async function groq(system, user) {
  for (let a = 0; a < 4; a++) {
    try {
      const r = await fetch(`${BASE}/chat/completions`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'Authorization': `Bearer ${KEY}` },
        body: JSON.stringify({ model: MODEL, temperature: 0.2, response_format: { type: 'json_object' },
          messages: [{ role: 'system', content: system }, { role: 'user', content: user }] }),
      });
      if (r.status === 429 || r.status >= 500) { await sleep(1500 * (a + 1)); continue; }
      if (!r.ok) { console.error('groq', r.status, (await r.text()).slice(0, 120)); return null; }
      const j = await r.json();
      const c = j?.choices?.[0]?.message?.content;
      return c ? JSON.parse(c) : null;
    } catch (e) { await sleep(1000 * (a + 1)); }
  }
  return null;
}

const norm = s => s.toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/\s+/g, ' ').trim();
// A def-cue: the paraphrase must be a way of asking for the DEFINITION/explanation of the topic — this
// is what filters out facets/sub-topics ("electronics for cars") and any stray fact-statement. So only
// genuine "what is X / how does X work" rephrasings survive -> no uncertain mappings, no false data.
const CUE = /\b(cos|cosa|cos'|significa|significato|definizione|spiega|spiegami|spiegare|come|che cos|chi |chi e|chi era|what|what's|whats|define|meaning|explain|how|who |describe|tell me what)\b/i;
function validQ(s, topicNorm) {
  if (typeof s !== 'string') return false;
  s = s.trim();
  if (s.length < 4 || s.length > 60) return false;
  if (s.includes('. ') || s.split('.').length > 2) return false;   // a sentence/statement, not a query
  if (/\b(19|20)\d\d\b/.test(s) || /\d{2,}/.test(s)) return false; // numbers -> smells like an asserted fact
  if (!CUE.test(s)) return false;                                  // must be a definition-question, not a facet
  const n = norm(s);
  if (n === topicNorm) return false;                               // the bare title is already a templated ask
  return true;
}

const SYS = 'Generi PARAFRASI della domanda "cos\'è X" per un motore di ricerca offline. Per ogni argomento dato '
  + 'produci SOLO modi diversi di chiedere la DEFINIZIONE/spiegazione di quell\'argomento, in italiano e inglese. '
  + 'REGOLE FERREE: (1) solo riformulazioni di "cos\'è X / che cos\'è / come funziona / spiega X"; '
  + '(2) VIETATO sotto-argomenti, applicazioni, esempi o domande più specifiche (NO "X per auto", "X in casa", "tipi di X"); '
  + '(3) MAI risposte, fatti, date o numeri; brevi (2-6 parole). '
  + 'Esempio per "Fotosintesi": it=["cosa significa fotosintesi","definizione di fotosintesi","spiegami la fotosintesi","come funziona la fotosintesi"], '
  + 'en=["what is photosynthesis","define photosynthesis","explain photosynthesis","how does photosynthesis work"]. '
  + 'Rispondi SOLO JSON: {"results":[{"i":<indice>,"it":[...],"en":[...]}]}';

let totalAdded = 0, cardsTouched = 0;
const files = ONLY ? [ONLY] : readdirSync(KDIR).filter(f => f.startsWith('wiki-') && f.endsWith('.jsonl'));
for (const f of files) {
  const p = join(KDIR, f);
  if (!existsSync(p)) { console.error('skip (missing):', f); continue; }
  const lines = readFileSync(p, 'utf8').split(/\r?\n/);
  const cards = []; const idx = [];
  lines.forEach((l, i) => { const t = l.trim(); if (t && !t.startsWith('//')) { try { cards.push(JSON.parse(t)); idx.push(i); } catch {} } });
  const todo = cards.slice(0, LIMIT);

  for (let b = 0; b < todo.length; b += BATCH) {
    const group = todo.slice(b, b + BATCH);
    const topics = group.map((c, k) => `${k}) ${(c.ask?.it?.[0] || c.id)} [${c.category}]`).join('\n');
    const out = await groq(SYS, `Argomenti:\n${topics}`);
    const results = out?.results || [];
    for (const res of results) {
      const c = group[res.i]; if (!c || !c.ask) continue;
      const topicNorm = norm(c.ask?.it?.[0] || '');
      for (const lang of ['it', 'en']) {
        const have = new Set((c.ask[lang] || []).map(norm));
        for (const q of (res[lang] || [])) {
          if (!validQ(q, topicNorm)) continue;
          const n = norm(q);
          if (have.has(n)) continue;
          if ((c.ask[lang] || []).length >= CAP) break;
          c.ask[lang] = c.ask[lang] || []; c.ask[lang].push(q.trim()); have.add(n); totalAdded++;
        }
      }
      cardsTouched++;
    }
    process.stdout.write(`\r[${f}] ${Math.min(b + BATCH, todo.length)}/${todo.length}   `);
    await sleep(250);
  }
  if (!DRY) { for (let k = 0; k < cards.length; k++) lines[idx[k]] = JSON.stringify(cards[k]); writeFileSync(p, lines.join('\n')); }
  console.log(`\n[${f}] enriched ${cardsTouched} cards, +${totalAdded} asks${DRY ? ' [DRY]' : ''}`);
  cardsTouched = 0; totalAdded = 0;
}
console.log('done.');
