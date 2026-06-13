// Spreadsheet NLU corpus generator. Uses TWO backends in parallel:
//   • Groq (online teacher) — fast, runs many jobs concurrently
//   • Ollama qwen3-coder:30b (local, RTX 3070 Ti) — quality/adversarial pass
// Output: labeled JSONL {text, lang, intent} for scoring/refining localIntent().
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const DIR = path.dirname(fileURLToPath(import.meta.url));
const key = JSON.parse(fs.readFileSync(path.join(DIR, '..', 'anima', '.teacher.bak.json'), 'utf8').replace(/^﻿/, ''));
const GROQ_URL = (key.base || 'https://api.groq.com/openai/v1') + '/chat/completions';
const GROQ_MODEL = key.model || 'llama-3.1-8b-instant';
const OLLAMA_MODEL = process.env.OLLAMA_MODEL || 'qwen3-coder:30b';

// intent label -> meaning shown to the generator (what the user wants to DO)
const INTENTS = {
  agg_sum: 'SUM / total a column or selection of numbers',
  agg_avg: 'AVERAGE / mean of a column or selection',
  agg_min: 'the MINIMUM value of a column',
  agg_max: 'the MAXIMUM value of a column',
  agg_count: 'COUNT how many cells/values/rows there are',
  agg_product: 'multiply all values (PRODUCT)',
  agg_median: 'the MEDIAN of a column',
  describe: 'describe / give summary statistics of the selected data',
  insights: 'find automatic insights: anomalies, outliers, trends, correlations',
  chart: 'make a chart / bar or line graph of the data',
  total: 'add a TOTAL ROW with sums under the data block',
  dedupe: 'remove duplicate rows',
  rmempty: 'remove empty rows',
  clean: 'clean the text: trim spaces / normalize whitespace',
  transform_upper: 'convert the selected text to UPPERCASE',
  transform_lower: 'convert the selected text to lowercase',
  numfmt_currency: 'format the numbers as currency (euro)',
  numfmt_percent: 'format the numbers as a percentage',
  numfmt_comma: 'format numbers with a thousands separator',
  format_bold: 'make the cells bold',
  sort: 'sort the data by a column (ascending or descending)',
  highlight: 'highlight cells matching a condition (e.g. greater than 100, duplicates, empty, the max)',
  fill: 'fill a range with a number series (1,2,3 / from 1 to 10)',
  formula: 'a CONDITIONAL aggregate, e.g. "sum if column A > 10", "count if greater than 5"',
  enrich: 'enrich a column with world knowledge, e.g. "add the capital of each country in column A", "add the birth year of each person"',
  explain: 'explain what the formula in the current cell does',
  find: 'find / search for a specific value or text in the sheet',
  help: 'ask what the assistant can do (capabilities / help)',
  knowledge: 'a GENERAL world-knowledge or math question, NOT a spreadsheet command (e.g. "what is the capital of France", "who is Einstein", "how much is 12*8", "what time is it")',
  chitchat: 'off-topic small talk or an impossible/unrelated request (e.g. "hi", "how are you", "tell me a joke", "make me a coffee")',
};

const LANGS = { it: 'Italian', en: 'English' };
const LANGS_FULL = ['it', 'en'];      // the only two the device engine + app actually support

function buildJobs(perJob) {
  const jobs = [];
  // two diverse passes per (intent, lang) so we get more phrasing variety
  for (const intent of Object.keys(INTENTS)) for (const lang of LANGS_FULL) { jobs.push({ intent, lang, k: perJob }); jobs.push({ intent, lang, k: perJob }); }
  return jobs;
}

function extractArray(text) {
  const i = text.indexOf('['); if (i < 0) return null;
  let depth = 0, inStr = false, esc = false;
  for (let j = i; j < text.length; j++) {
    const c = text[j];
    if (inStr) { if (esc) esc = false; else if (c === '\\') esc = true; else if (c === '"') inStr = false; }
    else if (c === '"') inStr = true; else if (c === '[') depth++; else if (c === ']') { depth--; if (depth === 0) { try { return JSON.parse(text.slice(i, j + 1)); } catch { return null; } } }
  }
  return null;
}

const promptFor = (intent, lang, k) =>
  `You create TEST DATA for a spreadsheet assistant's intent classifier.\n` +
  `Write ${k} SHORT, natural, DIVERSE messages a real user would type to a spreadsheet AI, ALL meaning: "${INTENTS[intent]}".\n` +
  `Language: ${LANGS[lang]}. Vary phrasing, verbosity, politeness, slang and common typos; some with an explicit column letter (A/B/C), some without.\n` +
  `Each item = one realistic user message. No numbering, no quotes-of-explanation, no markdown.\n` +
  `Output ONLY a JSON array of exactly ${k} strings.`;

async function groq(prompt) {
  const r = await fetch(GROQ_URL, {
    method: 'POST', headers: { 'Content-Type': 'application/json', Authorization: 'Bearer ' + key.key },
    body: JSON.stringify({ model: GROQ_MODEL, temperature: 0.95, max_tokens: 700, messages: [{ role: 'user', content: prompt }] }),
  });
  if (!r.ok) throw new Error('groq ' + r.status + ' ' + (await r.text()).slice(0, 120));
  return (await r.json()).choices[0].message.content;
}
async function ollama(prompt) {
  const r = await fetch('http://localhost:11434/api/generate', {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ model: OLLAMA_MODEL, stream: false, options: { temperature: 0.9, num_predict: 700 }, prompt }),
  });
  if (!r.ok) throw new Error('ollama ' + r.status);
  return (await r.json()).response;
}

async function runWorker(name, backend, jobs, outFile, onCount) {
  const ws = fs.createWriteStream(outFile, { flags: 'a' });
  let n = 0;
  for (const job of jobs) {
    try {
      const out = await backend(promptFor(job.intent, job.lang, job.k));
      const arr = extractArray(out);
      if (arr) for (const t of arr) {
        if (typeof t === 'string' && t.trim().length > 1 && t.length < 160) { ws.write(JSON.stringify({ text: t.trim(), lang: job.lang, intent: job.intent, src: name }) + '\n'); n++; onCount(); }
      }
    } catch (e) { process.stderr.write(`[${name}] ${job.intent}/${job.lang} FAIL ${e.message}\n`); }
  }
  ws.end();
  return n;
}

// ---- distribute: Groq gets the bulk (fast, parallel); Ollama runs a quality slice concurrently ----
const perJob = parseInt(process.env.PER_JOB || '10');
const allJobs = buildJobs(perJob);
// Ollama (local 30b): a typo/slang-heavy pass over the HARD, easily-confused intents
// (where extra phrasing variety most helps the parser). Sequential on the single GPU.
const HARD = ['agg_count', 'agg_sum', 'total', 'dedupe', 'rmempty', 'clean', 'enrich', 'highlight', 'knowledge', 'chitchat', 'find', 'formula'];
const ollamaJobs = HARD.flatMap(intent => LANGS_FULL.map(lang => ({ intent, lang, k: 8 })));
// Groq: everything (it/en/es/fr/de) — run with concurrency.
const groqJobs = allJobs;

let total = 0; const tick = () => { total++; if (total % 25 === 0) process.stderr.write(`… ${total} examples\n`); };

// Groq concurrency pool
async function groqPool(jobs, conc) {
  const out = path.join(DIR, 'corpus.groq.jsonl'); fs.writeFileSync(out, '');
  let i = 0;
  const workers = Array.from({ length: conc }, async () => {
    const ws = fs.createWriteStream(out, { flags: 'a' });
    while (i < jobs.length) {
      const job = jobs[i++];
      try {
        const arr = extractArray(await groq(promptFor(job.intent, job.lang, job.k)));
        if (arr) for (const t of arr) if (typeof t === 'string' && t.trim().length > 1 && t.length < 160) { ws.write(JSON.stringify({ text: t.trim(), lang: job.lang, intent: job.intent, src: 'groq' }) + '\n'); tick(); }
      } catch (e) { process.stderr.write(`[groq] ${job.intent}/${job.lang} ${e.message}\n`); }
    }
    ws.end();
  });
  await Promise.all(workers);
}

const t0 = Date.now();
console.error(`Generating: Groq(${groqJobs.length} jobs ×~${perJob}) ∥ Ollama ${OLLAMA_MODEL}(${ollamaJobs.length} jobs ×8)…`);
await Promise.all([
  groqPool(groqJobs, 6),
  runWorker('ollama', ollama, ollamaJobs, path.join(DIR, 'corpus.ollama.jsonl'), tick),
]);

// ---- merge + dedup ----
function read(f) { try { return fs.readFileSync(f, 'utf8').trim().split('\n').filter(Boolean).map(l => JSON.parse(l)); } catch { return []; } }
const all = [...read(path.join(DIR, 'corpus.groq.jsonl')), ...read(path.join(DIR, 'corpus.ollama.jsonl'))];
const seen = new Set(); const merged = [];
for (const e of all) { const kk = e.lang + '|' + e.text.toLowerCase().replace(/\s+/g, ' ').trim(); if (seen.has(kk)) continue; seen.add(kk); merged.push(e); }
fs.writeFileSync(path.join(DIR, 'corpus.jsonl'), merged.map(e => JSON.stringify(e)).join('\n') + '\n');
const byIntent = {}; const byLang = {};
for (const e of merged) { byIntent[e.intent] = (byIntent[e.intent] || 0) + 1; byLang[e.lang] = (byLang[e.lang] || 0) + 1; }
console.error(`\nDONE in ${((Date.now() - t0) / 1000).toFixed(0)}s — ${merged.length} unique examples`);
console.error('by lang:', byLang);
console.error('by intent:', Object.entries(byIntent).sort((a, b) => a[1] - b[1]).map(([k, v]) => k + ':' + v).join('  '));
