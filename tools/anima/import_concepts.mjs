#!/usr/bin/env node
// ANIMA importer — JavaScript / PROGRAMMING / CS concepts from WIKIPEDIA REST summaries (CC-BY-SA),
// grounded: the reply is the article's own lead extract (verbatim, clipped), never generated. Bilingual
// (it + en). Curated, high-signal title list (no gossip, only useful technical concepts).
// Output: tools/anima/knowledge.staged/concepts.jsonl  (lands via AKB5, certified by anima:gate-akb5).
//   node tools/anima/import_concepts.mjs
import { writeFileSync, readFileSync, readdirSync, mkdirSync, existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
const here = dirname(fileURLToPath(import.meta.url));
const OUT = join(here, 'knowledge.staged', 'concepts.jsonl');
const UA = 'NucleoOS-ANIMA-Import/1.0 (offline edge assistant; CS concepts; CC-BY-SA Wikipedia)';

// [it title, en title, short label for asks]. IT title drives the IT card; EN title the EN card.
const TOPICS = [
  ['JavaScript', 'JavaScript', 'JavaScript'], ['Variabile (informatica)', 'Variable (computer science)', 'variabile'],
  ['Funzione (informatica)', 'Subroutine', 'funzione'], ['Chiusura (informatica)', 'Closure (computer programming)', 'closure'],
  ['Ricorsione (informatica)', 'Recursion (computer science)', 'ricorsione'], ['Array', 'Array (data structure)', 'array'],
  ['JSON', 'JSON', 'JSON'], ['Document Object Model', 'Document Object Model', 'DOM'],
  ['Programmazione asincrona', 'Asynchrony (computer programming)', 'asincrono'], ['Callback (informatica)', 'Callback (computer programming)', 'callback'],
  ['Espressione regolare', 'Regular expression', 'regex'], ['Algoritmo', 'Algorithm', 'algoritmo'],
  ['Struttura dati', 'Data structure', 'struttura dati'], ['Complessità computazionale', 'Computational complexity', 'complessità'],
  ['Ricerca dicotomica', 'Binary search algorithm', 'ricerca binaria'], ['Algoritmo di ordinamento', 'Sorting algorithm', 'ordinamento'],
  ['Tabella hash', 'Hash table', 'hash table'], ['Albero (informatica)', 'Tree (data structure)', 'albero'],
  ['Grafo (teoria dei grafi)', 'Graph (abstract data type)', 'grafo'], ['Pila (informatica)', 'Stack (abstract data type)', 'stack'],
  ['Coda (informatica)', 'Queue (abstract data type)', 'coda'], ['Lista concatenata', 'Linked list', 'lista concatenata'],
  ['Programmazione orientata agli oggetti', 'Object-oriented programming', 'OOP'], ['Ereditarietà (informatica)', 'Inheritance (object-oriented programming)', 'ereditarietà'],
  ['Polimorfismo (informatica)', 'Polymorphism (computer science)', 'polimorfismo'], ['Classe (informatica)', 'Class (computer programming)', 'classe'],
  ['Compilatore', 'Compiler', 'compilatore'], ['Interprete (informatica)', 'Interpreter (computing)', 'interprete'],
  ['Macchina virtuale', 'Virtual machine', 'macchina virtuale'], ['Garbage collection', 'Garbage collection (computer science)', 'garbage collection'],
  ['Puntatore (programmazione)', 'Pointer (computer programming)', 'puntatore'], ['Sistema operativo', 'Operating system', 'sistema operativo'],
  ['Processo (informatica)', 'Process (computing)', 'processo'], ['Thread (informatica)', 'Thread (computing)', 'thread'],
  ['Concorrenza (informatica)', 'Concurrency (computer science)', 'concorrenza'], ['Base di dati', 'Database', 'database'],
  ['SQL', 'SQL', 'SQL'], ['Git', 'Git', 'Git'], ['Controllo versione', 'Version control', 'controllo versione'],
  ['Transmission Control Protocol', 'Transmission Control Protocol', 'TCP'], ['Internet Protocol', 'Internet Protocol', 'IP'],
  ['Domain Name System', 'Domain Name System', 'DNS'], ['Hypertext Transfer Protocol', 'HTTP', 'HTTP'],
  ['HTML', 'HTML', 'HTML'], ['CSS', 'CSS', 'CSS'], ['Representational State Transfer', 'REST', 'REST'],
  ['Crittografia', 'Cryptography', 'crittografia'], ['Funzione di hash', 'Hash function', 'funzione di hash'],
  ['API', 'API', 'API'], ['Bug', 'Software bug', 'bug'], ['Debugging', 'Debugging', 'debug'],
  ['Sistema binario', 'Binary number', 'binario'], ['Codifica dei caratteri', 'Character encoding', 'encoding'],
  ['Unicode', 'Unicode', 'Unicode'], ['Booleano', 'Boolean data type', 'booleano'], ['Bit', 'Bit', 'bit'],
  ['Byte', 'Byte', 'byte'], ['Programmazione funzionale', 'Functional programming', 'programmazione funzionale'],
  ['Intelligenza artificiale', 'Artificial intelligence', 'intelligenza artificiale'], ['Apprendimento automatico', 'Machine learning', 'machine learning'],
  ['Rete neurale artificiale', 'Artificial neural network', 'rete neurale'], ['WebAssembly', 'WebAssembly', 'WebAssembly'],
  ['Microcontrollore', 'Microcontroller', 'microcontrollore'], ['ESP32', 'ESP32', 'ESP32'],
  ['Sistema embedded', 'Embedded system', 'sistema embedded'], ['Open source', 'Open-source software', 'open source'],
];

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const slug = (s) => s.normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');
const clip = (t) => { t = (t || '').replace(/\s+/g, ' ').trim(); if (t.length <= 248) return t; const cut = t.slice(0, 248); const d = cut.lastIndexOf('. '); return (d > 120 ? cut.slice(0, d + 1) : cut.slice(0, 245) + '…'); };

async function summary(lang, title) {
  const u = `https://${lang}.wikipedia.org/api/rest_v1/page/summary/` + encodeURIComponent(title.replace(/ /g, '_'));
  for (let a = 0; a < 3; a++) {
    try {
      const r = await fetch(u, { headers: { 'User-Agent': UA, Accept: 'application/json' }, redirect: 'follow' });
      if (r.status === 404) return null;
      if (!r.ok) throw new Error('HTTP ' + r.status);
      const j = await r.json();
      if (j.type === 'disambiguation' || !j.extract) return null;
      return clip(j.extract);
    } catch (e) { if (a === 2) return null; await sleep(1200); }
  }
  return null;
}

// cross-corpus ask dedup
const norm = (s) => s.toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/[^a-z0-9 ]/g, '').replace(/\s+/g, ' ').trim();
const claimed = new Set();
const kdir = join(here, 'knowledge');
for (const f of readdirSync(kdir)) { if (!f.endsWith('.jsonl')) continue;
  for (const l of readFileSync(join(kdir, f), 'utf8').split('\n')) { if (!l.trim() || l.startsWith('//')) continue;
    try { const c = JSON.parse(l); for (const a of (c.ask?.it || [])) claimed.add(norm(a)); } catch {} } }

const cards = [];
for (let i = 0; i < TOPICS.length; i++) {
  const [itT, enT, lbl] = TOPICS[i];
  process.stderr.write(`[concepts] ${lbl} (${i + 1}/${TOPICS.length}) … `);
  const eIt = await summary('it', itT); const eEn = await summary('en', enT);
  if (!eIt && !eEn) { process.stderr.write('miss\n'); continue; }
  process.stderr.write('ok\n');
  const askIt = [`cos'è ${lbl}`, `che cos'è ${lbl}`, `cosa significa ${lbl}`, `spiegami ${lbl}`, `definizione di ${lbl}`, `${lbl}`];
  const askEn = [`what is ${lbl}`, `what does ${lbl} mean`, `explain ${lbl}`, `${lbl}`];
  if (askIt.some((a) => claimed.has(norm(a)))) { continue; }
  cards.push({ id: `wiki.concept.${slug(lbl)}`, category: 'concept', action: 'answer', arg: '',
    reply: { it: eIt || eEn, en: eEn || eIt }, ask: { it: askIt, en: askEn },
    source: `wikipedia:${itT}`, lang_primary: 'bi', tags: ['concept', 'programming', 'wikipedia'] });
  await sleep(150);
}
if (!existsSync(dirname(OUT))) mkdirSync(dirname(OUT), { recursive: true });
writeFileSync(OUT, cards.map((c) => JSON.stringify(c)).join('\n') + '\n');
console.log(`[concepts] ${cards.length} grounded concept cards -> ${OUT}`);
