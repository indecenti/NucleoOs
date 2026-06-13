#!/usr/bin/env node
// ANIMA importer #3 — GROUNDED bilingual definitions from Wikipedia (the pocket-encyclopedia engine).
// For a CURATED list of high-value, non-niche concepts it fetches the REAL first-paragraph from BOTH the
// Italian AND English Wikipedia (native each language -> zero translation risk; verbatim -> zero
// hallucination). Only "standard" pages are accepted (disambiguation/missing are skipped, never guessed).
// Good-citizen: descriptive User-Agent, SERIAL requests with a small delay. Output (staged):
//   tools/anima/knowledge.staged/wiki-defs.jsonl   (lands via AKB5 / a gated rebuild)
// Run: node tools/anima/import_wiki_defs.mjs   (network needed; re-run is incremental via cache)
import { readFileSync, writeFileSync, existsSync, readdirSync, mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const CACHE = join(here, '.cache', 'wiki');
mkdirSync(CACHE, { recursive: true });
const OUT = join(here, 'knowledge.staged', 'wiki-defs.jsonl');
const UA = 'NucleoOS-ANIMA/1.0 (offline pocket encyclopedia; grounded, no-hallucination)';

// CURATED high-value, non-niche concepts: {cat, it: <IT wiki title>, en: <EN wiki title>}. Chosen for a
// pocket encyclopedia — informatica, elettronica/ESP, scienza, storia essenziale, cultura generale.
const T = [
  // --- informatica / CS ---
  ['computer-science','Algoritmo','Algorithm'],['computer-science','Struttura dati','Data structure'],
  ['computer-science','Database','Database'],['computer-science','Sistema operativo','Operating system'],
  ['computer-science','Compilatore','Compiler'],['computer-science','Linguaggio di programmazione','Programming language'],
  ['computer-science','Intelligenza artificiale','Artificial intelligence'],['computer-science','Rete neurale artificiale','Artificial neural network'],
  ['computer-science','Apprendimento automatico','Machine learning'],['computer-science','Crittografia','Cryptography'],
  ['computer-science','Compressione dei dati','Data compression'],['computer-science','Codice sorgente','Source code'],
  ['computer-science','Programmazione orientata agli oggetti','Object-oriented programming'],['computer-science','Ricorsione','Recursion'],
  ['computer-science','Complessità computazionale','Computational complexity'],['computer-science','Sistema numerico binario','Binary number'],
  // --- reti / web ---
  ['networking','Internet','Internet'],['networking','World Wide Web','World Wide Web'],['networking','Indirizzo IP','IP address'],
  ['networking','Hypertext Transfer Protocol','HTTP'],['networking','Domain Name System','Domain Name System'],
  ['networking','Suite di protocolli Internet','Internet protocol suite'],['networking','Wi-Fi','Wi-Fi'],['networking','Bluetooth','Bluetooth'],
  // --- elettronica / embedded / ESP ---
  ['electronics','Transistor','Transistor'],['electronics','Resistore','Resistor'],['electronics','Condensatore (elettrotecnica)','Capacitor'],
  ['electronics','Diodo','Diode'],['electronics','Diodo a emissione di luce','Light-emitting diode'],['electronics','Circuito integrato','Integrated circuit'],
  ['electronics','Microcontrollore','Microcontroller'],['electronics','Microprocessore','Microprocessor'],['electronics','Semiconduttore','Semiconductor'],
  ['electronics','Corrente elettrica','Electric current'],['electronics','Tensione elettrica','Voltage'],['electronics','Legge di Ohm','Ohm\'s law'],
  ['esp32','Arduino (hardware)','Arduino'],['esp32','Sensore','Sensor'],['esp32','Sistema embedded','Embedded system'],
  // --- scienza ---
  ['science','Atomo','Atom'],['science','Elettrone','Electron'],['science','Molecola','Molecule'],['science','Energia','Energy'],
  ['science','Gravità','Gravity'],['science','Fotosintesi','Photosynthesis'],['science','DNA','DNA'],['science','Cellula','Cell (biology)'],
  ['science','Evoluzione','Evolution'],['science','Teoria della relatività','Theory of relativity'],['science','Meccanica quantistica','Quantum mechanics'],
  ['science','Sistema solare','Solar System'],['science','Buco nero','Black hole'],['science','Tavola periodica','Periodic table'],
  ['science','Vulcano','Volcano'],['science','Terremoto','Earthquake'],['science','Clima','Climate'],['science','Virus (biologia)','Virus'],
  // --- matematica ---
  ['math','Numero primo','Prime number'],['math','Pi greco','Pi'],['math','Teorema di Pitagora','Pythagorean theorem'],
  ['math','Derivata','Derivative'],['math','Probabilità','Probability'],['math','Geometria','Geometry'],
  // --- storia essenziale ---
  ['world-history','Impero romano','Roman Empire'],['world-history','Antico Egitto','Ancient Egypt'],['world-history','Medioevo','Middle Ages'],
  ['world-history','Rinascimento','Renaissance'],['world-history','Rivoluzione francese','French Revolution'],['world-history','Rivoluzione industriale','Industrial Revolution'],
  ['world-history','Prima guerra mondiale','World War I'],['world-history','Seconda guerra mondiale','World War II'],['world-history','Guerra fredda','Cold War'],
  // --- cultura generale ---
  ['disciplines','Filosofia','Philosophy'],['disciplines','Economia','Economics'],['disciplines','Democrazia','Democracy'],
  ['disciplines','Lingua (linguistica)','Language'],['disciplines','Musica','Music'],['disciplines','Fotografia','Photography'],
  // ===== EXPANSION (curated, high-value, NON-niche) =====
  // --- informatica everyday ---
  ['computer-science','Unità di elaborazione centrale','Central processing unit'],['computer-science','Memoria ad accesso casuale','Random-access memory'],
  ['computer-science','Disco rigido','Hard disk drive'],['computer-science','Unità a stato solido','Solid-state drive'],
  ['computer-science','Universal Serial Bus','USB'],['computer-science','Bit','Bit'],['computer-science','Byte','Byte'],
  ['computer-science','File system','File system'],['computer-science','Software','Software'],['computer-science','Hardware','Computer hardware'],
  ['computer-science','Cloud computing','Cloud computing'],['computer-science','Software libero','Free software'],
  ['computer-science','Kernel','Kernel (operating system)'],['computer-science','Browser web','Web browser'],
  ['computer-science','Server','Server (computing)'],['computer-science','Password','Password'],['computer-science','Firewall','Firewall (computing)'],
  ['computer-science','Backup','Backup'],['computer-science','Codice QR','QR code'],['computer-science','Posta elettronica','Email'],
  ['computer-science','Sistema di posizionamento globale','Global Positioning System'],['computer-science','Pixel','Pixel'],
  // --- elettronica / embedded ---
  ['electronics','Corrente alternata','Alternating current'],['electronics','Corrente continua','Direct current'],
  ['electronics','Batteria (elettrotecnica)','Electric battery'],['electronics','Modulazione di larghezza di impulso','Pulse-width modulation'],
  ['electronics','Antenna','Antenna (radio)'],['electronics','Frequenza','Frequency'],['electronics','Onda radio','Radio wave'],
  ['electronics','Relè','Relay'],['electronics','Motore elettrico','Electric motor'],['electronics','Potenza elettrica','Electric power'],
  // --- fisica ---
  ['science','Forza','Force'],['science','Massa (fisica)','Mass'],['science','Velocità','Velocity'],['science','Accelerazione','Acceleration'],
  ['science','Temperatura','Temperature'],['science','Pressione','Pressure'],['science','Elettricità','Electricity'],['science','Magnetismo','Magnetism'],
  ['science','Luce','Light'],['science','Suono','Sound'],['science','Calore','Heat'],['science','Onda','Wave'],['science','Radioattività','Radioactive decay'],
  // --- chimica ---
  ['chemistry','Elemento chimico','Chemical element'],['chemistry','Ossigeno','Oxygen'],['chemistry','Idrogeno','Hydrogen'],['chemistry','Carbonio','Carbon'],
  ['chemistry','Acqua','Water'],['chemistry','Cloruro di sodio','Sodium chloride'],['chemistry','Acido','Acid'],['chemistry','Metallo','Metal'],
  // --- biologia / salute ---
  ['biology','Gene','Gene'],['biology','Proteina','Protein'],['biology','Batteri','Bacteria'],['biology','Vaccino','Vaccine'],
  ['health','Cuore','Heart'],['health','Cervello','Brain'],['health','Sangue','Blood'],['health','Polmone','Lung'],
  ['health','Osso','Bone'],['health','Muscolo','Muscle'],['health','Sistema immunitario','Immune system'],['biology','Neurone','Neuron'],
  // --- astronomia / terra ---
  ['astronomy','Sole','Sun'],['astronomy','Luna','Moon'],['astronomy','Terra','Earth'],['astronomy','Pianeta','Planet'],
  ['astronomy','Stella','Star'],['astronomy','Galassia','Galaxy'],['astronomy','Universo','Universe'],['astronomy','Big Bang','Big Bang'],
  ['geography','Oceano','Ocean'],['geography','Montagna','Mountain'],['geography','Fiume','River'],['geography','Deserto','Desert'],
  ['geography','Continente','Continent'],['geography','Atmosfera terrestre','Atmosphere of Earth'],
  // --- matematica ---
  ['math','Equazione','Equation'],['math','Frazione (matematica)','Fraction'],['math','Percentuale','Percentage'],['math','Logaritmo','Logarithm'],
  ['math','Funzione (matematica)','Function (mathematics)'],['math','Statistica','Statistics'],['math','Numero','Number'],['math','Insieme','Set (mathematics)'],
  // --- storia essenziale ---
  ['world-history','Antica Grecia','Ancient Greece'],['world-history','Antica Roma','Ancient Rome'],['world-history','Cristianesimo','Christianity'],
  ['world-history','Islam','Islam'],['world-history','Illuminismo','Age of Enlightenment'],['world-history','Risorgimento','Italian unification'],
  // --- società / economia ---
  ['disciplines','Stato (diritto)','Sovereign state'],['disciplines','Governo','Government'],['disciplines','Legge','Law'],
  ['disciplines','Valuta','Currency'],['disciplines','Mercato (economia)','Market (economics)'],['disciplines','Religione','Religion'],
  ['disciplines','Riscaldamento globale','Global warming'],['disciplines','Inquinamento','Pollution'],['disciplines','Energia solare','Solar energy'],
];

const slug = (s) => s.toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g,'').replace(/[^a-z0-9]+/g,'-').replace(/^-|-$/g,'');
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
async function fetchSummary(lang, title) {
  const cf = join(CACHE, `${lang}.${slug(title)}.json`);
  if (existsSync(cf)) { try { return JSON.parse(readFileSync(cf, 'utf8')); } catch {} }
  const url = `https://${lang}.wikipedia.org/api/rest_v1/page/summary/${encodeURIComponent(title)}`;
  for (let attempt = 0; attempt < 3; attempt++) {
    try {
      const res = await fetch(url, { headers: { 'User-Agent': UA, 'Accept': 'application/json' } });
      if (res.status === 429) { await sleep(5000 * (attempt + 1)); continue; }   // rate-limited -> back off
      if (!res.ok) return null;
      const j = await res.json();
      writeFileSync(cf, JSON.stringify({ type: j.type, title: j.title, extract: j.extract || '' }));
      await sleep(450);                                                          // good-citizen pacing (~2/s)
      return j;
    } catch { await sleep(1000); }
  }
  return null;
}
// strip a leading IPA/pronunciation parenthetical; clip to a clean sentence under cap bytes.
function clean(ex, cap = 250) {
  let s = (ex || '').replace(/\s+/g, ' ').trim();
  if (s.length <= cap) return s;
  s = s.slice(0, cap);
  const cut = Math.max(s.lastIndexOf('. '), s.lastIndexOf('! '), s.lastIndexOf('? '));
  return (cut > cap / 2 ? s.slice(0, cut + 1) : s.replace(/\s+\S*$/, '') + '…');
}
// cross-corpus ask dedup
const norm = (s) => s.toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g,'').replace(/[^a-z0-9 ]/g,'').replace(/\s+/g,' ').trim();
const claimed = new Set();
const kdir = join(here, 'knowledge');
for (const f of readdirSync(kdir)) { if (!f.endsWith('.jsonl')) continue;
  for (const l of readFileSync(join(kdir,f),'utf8').split('\n')) { if (!l.trim()||l.startsWith('//')) continue;
    let j; try { j=JSON.parse(l); } catch { continue; } for (const lg of ['it','en']) for (const a of (j.ask?.[lg]||[])) claimed.add(norm(a)); } }
const dedup = (arr) => arr.filter(a => { const n = norm(a); if (!n || claimed.has(n)) return false; claimed.add(n); return true; });

const cards = []; const skipped = [];
for (const [cat, itT, enT] of T) {
  const [si, se] = [await fetchSummary('it', itT), await fetchSummary('en', enT)];
  const okIt = si && si.type === 'standard' && (si.extract||'').length > 40;
  const okEn = se && se.type === 'standard' && (se.extract||'').length > 40;
  if (!okIt && !okEn) { skipped.push(`${itT}/${enT}`); continue; }
  const nameIt = okIt ? si.title : itT, nameEn = okEn ? se.title : enT;
  const askIt = dedup([`cos'è ${nameIt}`, `che cos'è ${nameIt}`, `cosa è ${nameIt}`, `definizione di ${nameIt}`, `spiegami ${nameIt}`, nameIt]);
  const askEn = dedup([`what is ${nameEn}`, `what is a ${nameEn}`, `define ${nameEn}`, `definition of ${nameEn}`, nameEn]);
  cards.push({ id: `wiki.${cat}.${slug(enT)}`, category: cat, action: 'answer', arg: '',
    reply: { it: okIt ? clean(si.extract) : clean(se.extract), en: okEn ? clean(se.extract) : clean(si.extract) },
    ask: { it: askIt.length ? askIt : [`cos'è ${nameIt}`], en: askEn.length ? askEn : [`what is ${nameEn}`] },
    lang_primary: (okIt && okEn) ? 'bi' : (okIt ? 'it' : 'en'),
    source: `wikipedia:${okIt?'it@'+nameIt:''}${okIt&&okEn?'+':''}${okEn?'en@'+nameEn:''}` });
}
writeFileSync(OUT, cards.map(c => JSON.stringify(c)).join('\n') + '\n');
console.log(`[wiki-defs] ${cards.length}/${T.length} grounded bilingual definition cards -> ${OUT}`);
if (skipped.length) console.log(`[wiki-defs] skipped (no standard page): ${skipped.join(', ')}`);
console.log(`[wiki-defs] sample: ${cards[0]?.reply.it?.slice(0,90)}`);
