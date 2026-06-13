#!/usr/bin/env node
// Add common ABBREVIATION/spelling aliases to their canonical concept card so the everyday word the user
// types ("wifi","ram","ip") matches the formal-titled card ("Wi-Fi","Memoria ad accesso casuale",
// "Indirizzo IP"). Curated, bounded list -> scalable, big recall win. Run AFTER dedup (these aliases are
// intentional; do NOT re-run dedup after, or it strips the ambiguous ones again).
import { readFileSync, writeFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const KDIR = join(dirname(fileURLToPath(import.meta.url)), 'knowledge');
// alias -> needle identifying the canonical card by its title (ask.it[0]), case-insensitive substring.
const MAP = [
  ['wifi', 'wi-fi'], ['ram', 'accesso casuale'], ['http', 'hypertext transfer protocol'],
  ['dns', 'domain name system'], ['ip', 'indirizzo ip'], ['tcp', 'transmission control protocol'],
  ['udp', 'user datagram protocol'], ['os', 'sistema operativo'], ['ai', 'intelligenza artificiale'],
  ['llm', 'modello linguistico di grandi dimensioni'], ['led', 'diodo a emissione di luce'],
  ['vpn', 'virtual private network'], ['nat', 'network address translation'], ['url', 'uniform resource locator'],
  ['dom', 'document object model'], ['oop', 'programmazione orientata agli oggetti'], ['rest', 'representational state transfer'],
];
const variants = a => [a, `cos'è ${a}`, `cos'è la ${a}`, `cos'è il ${a}`, `cosa sai di ${a}`, `conosci ${a}`];

const files = readdirSync(KDIR).filter(f => f.endsWith('.jsonl'));
const raw = {}; for (const f of files) raw[f] = readFileSync(join(KDIR, f), 'utf8').split(/\r?\n/);

let hit = 0, added = 0;
for (const [alias, needle] of MAP) {
  let o = null, bestFile = null, bestLine = -1;
  for (const f of files) for (let i = 0; i < raw[f].length; i++) {
    const t = raw[f][i].trim(); if (!t || t.startsWith('//')) continue;
    let c; try { c = JSON.parse(t); } catch { continue; }
    const title = (c.ask?.it?.[0] || '').toLowerCase();
    if (title === needle || title.includes(needle)) {
      if (!o || (c.id.startsWith('wiki.') && !o.id.startsWith('wiki.')) || title.length < o.ask.it[0].length) {
        o = c; bestFile = f; bestLine = i;
      }
    }
  }
  if (!o) { console.log(`  (no card for "${alias}")`); continue; }
  hit++;
  const have = new Set((o.ask.it || []).map(s => s.toLowerCase()));
  for (const v of variants(alias)) if (!have.has(v.toLowerCase())) { o.ask.it.push(v); have.add(v.toLowerCase()); added++; }
  o.ask.en = o.ask.en || [];
  const haveEn = new Set(o.ask.en.map(s => s.toLowerCase()));
  for (const v of [alias, `what is ${alias}`]) if (!haveEn.has(v.toLowerCase())) { o.ask.en.push(v); haveEn.add(v.toLowerCase()); added++; }
  raw[bestFile][bestLine] = JSON.stringify(o);
  console.log(`  ${alias} -> ${o.id}`);
}
for (const f of files) writeFileSync(join(KDIR, f), raw[f].join('\n'));
console.log(`alias pass: ${hit}/${MAP.length} matched, +${added} asks`);
