#!/usr/bin/env node
// ENC A/B — misura recall (encoder-sensitive) sulla fixture host CORRENTE. Forza AKB5 on. Stampa recall per
// gruppo (curate/verbose/knowledge) + halluc sui false-premise. Si lancia DUE volte (fixture old vs aug) e
// si confrontano i numeri. Uso: node tools/anima-host/enc-ab.mjs [tag]
import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
const here = dirname(fileURLToPath(import.meta.url));
const exe = join(here, 'build', 'anima.exe');
const tag = process.argv[2] || '';
if (!existsSync(exe)) { console.error('anima.exe missing'); process.exit(2); }
if (!existsSync(join(here, 'sd', 'data', 'anima', 'anima-it-akb5.bin'))) { console.error('no AKB5 manifest in fixture'); process.exit(2); }

// CURATE (canoniche) — encoder-sensitive
const CURATE = [
  { q: 'capitale del Giappone', lang: 'it', want: ['tokyo'] },
  { q: "cos'è la fotosintesi", lang: 'it', want: ['piant'] },
  { q: 'what is a transistor', lang: 'en', want: ['transistor'] },
  { q: "cos'è il DNS", lang: 'it', want: ['domini'] },
  { q: 'costante di Planck', lang: 'it', want: ['6.626'] },
];
// VERBOSE (parafrasi col filler — il caso che instrada male)
const VERBOSE = [
  { q: 'spiegami come funziona la fotosintesi', lang: 'it', want: ['piant', 'luce', 'cloro'] },
  { q: 'puoi spiegarmi cos è un transistor e a cosa serve', lang: 'it', want: ['transistor', 'interrutt', 'amplific'] },
  { q: 'raccontami della teoria della relatività di einstein', lang: 'it', want: ['einstein', 'spazio', 'tempo', 'relativ'] },
  { q: 'mi spieghi come funziona il dns', lang: 'it', want: ['domini', 'nome', 'indirizz'] },
  { q: 'can you explain how photosynthesis works', lang: 'en', want: ['light', 'plant', 'chloro'] },
  { q: 'tell me about the theory of relativity', lang: 'en', want: ['einstein', 'space', 'time', 'relativ'] },
];
// KNOWLEDGE-20 reali (escludo skill/math non encoder-dipendenti)
const KNOW = [
  { q: 'chi ha scritto la divina commedia', lang: 'it', want: ['dante', 'alighieri'] },
  { q: "cos'e il dna", lang: 'it', want: ['acido', 'genetic', 'nucleic'] },
  { q: "qual e la capitale dell'australia", lang: 'it', want: ['canberra'] },
  { q: 'spiegami cos e un buco nero', lang: 'it', want: ['gravit', 'celeste', 'spaziotempo', 'luce'] },
  { q: "quando e caduto l'impero romano", lang: 'it', want: ['476', '395', 'occident', 'odoacre'] },
  { q: "che cos'e l'intelligenza artificiale", lang: 'it', want: ['macchina', 'intellig', 'compiti'] },
  { q: 'come funziona un motore a scoppio', lang: 'it', want: ['combust', 'piston', 'scoppio', 'cilindr'] },
  { q: "qual e la formula dell'acqua", lang: 'it', want: ['h2o', 'h₂o', 'idrogeno', 'ossigeno'] },
  { q: 'what is the speed of light', lang: 'en', want: ['300', '299', 'kilomet'] },
  { q: 'who painted the mona lisa', lang: 'en', want: ['leonardo', 'vinci'] },
  { q: 'how does a computer cpu work', lang: 'en', want: ['cpu', 'processor', 'instruct', 'arithmet'] },
  { q: 'what is the capital of japan', lang: 'en', want: ['tokyo'] },
  { q: 'explain what a black hole is', lang: 'en', want: ['gravit', 'celestial', 'light', 'spacetime'] },
  { q: 'what causes earthquakes', lang: 'en', want: ['tectonic', 'plate', 'ground'] },
  { q: 'what is machine learning', lang: 'en', want: ['learn', 'data', 'algorithm', 'apprendi'] },
  { q: 'how far is the sun from earth', lang: 'en', want: ['150', '149', 'million', 'kilomet'] },
  { q: 'what is the boiling point of water', lang: 'en', want: ['100', 'celsius', 'degree'] },
];
const ABSTAIN = [
  { q: 'qual è la capitale di Marte', lang: 'it' },
  { q: "qual è l'indirizzo email di Napoleone", lang: 'it' },
  { q: 'what is the capital of the Moon', lang: 'en' },
  { q: 'quanti followers ha il Medioevo', lang: 'it' },
];
const groups = { curate: CURATE, verbose: VERBOSE, knowledge: KNOW };
const all = [...CURATE, ...VERBOSE, ...KNOW, ...ABSTAIN.map(x => ({ ...x, abstain: true }))];
const lines = []; let lang = 'it';
for (const it of all) { lines.push('/reset'); if (it.lang !== lang) { lines.push('/' + it.lang); lang = it.lang; } lines.push(it.q); }
const norm = s => (s || '').normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase().replace(/\s+/g, ' ').trim();
const isRefusal = s => /not a place|not a person|non (è|e) (un|una) (luogo|persona|posto)|did you mean|non sono sicuro|intendi\b|non ho (informazioni|dettagli)|i (don'?t|do not) (have|know)/i.test(s);
const r = spawnSync(exe, [], { input: Buffer.from(lines.join('\n') + '\n', 'utf8'), maxBuffer: 64 * 1024 * 1024, env: { ...process.env, ANIMA_AKB5: '1' } });
const byQ = new Map();
for (const b of r.stdout.toString('utf8').split(/^Q: /m).slice(1)) {
  const q = norm(b.split('\n')[0]);
  const tier = (b.match(/tier=(\S+)/) || [])[1] || 'none';
  let rep = (b.match(/reply:\s*([\s\S]*?)\s*$/m) || [])[1] || ''; rep = rep.replace(/\s+/g, ' ').trim();
  if (rep === '(vuoto)' || rep === '(empty)') rep = '';
  byQ.set(q, { tier, rep, answered: rep !== '' });
}
function score(set) {
  let ok = 0; const miss = [];
  for (const it of set) { const b = byQ.get(norm(it.q)) || { answered: false, rep: '' };
    if (b.answered && it.want.some(w => norm(b.rep).includes(norm(w)))) ok++; else miss.push(it.q.slice(0, 34)); }
  return { ok, n: set.length, miss };
}
let halluc = 0;
for (const it of ABSTAIN) { const b = byQ.get(norm(it.q)) || { answered: false, rep: '' }; if (b.answered && !isRefusal(b.rep)) halluc++; }
const cur = score(CURATE), ver = score(VERBOSE), kn = score(KNOW);
const tot = cur.ok + ver.ok + kn.ok, totN = cur.n + ver.n + kn.n;
console.log(`[enc-ab ${tag}] TOT ${tot}/${totN} | curate ${cur.ok}/${cur.n} | verbose ${ver.ok}/${ver.n} | knowledge ${kn.ok}/${kn.n} | HALLUC ${halluc}`);
console.log(`   miss verbose:   ${ver.miss.join(' ; ')}`);
console.log(`   miss knowledge: ${kn.miss.join(' ; ')}`);
