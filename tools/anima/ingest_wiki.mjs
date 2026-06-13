#!/usr/bin/env node
// Bulk knowledge ingest for ANIMA — "cos'è X" cards from Wikipedia SHORT abstracts. Deterministic
// (no LLM tokens): harvests authoritative IT categories, pulls a 2-sentence intro per page via the
// action API (small, clean, source-anchored), pairs the EN abstract via langlinks, generates
// templated bilingual ask-phrasings, DEDUPES against the existing corpus, categorizes by domain, and
// writes tools/anima/knowledge/wiki-<domain>.jsonl. Then: build_akb2.py -> eval -> deploy to SD.
//
// Usage: node tools/anima/ingest_wiki.mjs [--cap N] [--dry]
import { readFileSync, writeFileSync, readdirSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { setTimeout as sleep } from 'node:timers/promises';

const here = dirname(fileURLToPath(import.meta.url));
const KDIR = join(here, 'knowledge');
const UA = 'NucleoOS-ANIMA-ingest/1.0 (offline educational device; contact: local)';
const CAP = parseInt((process.argv.find(a => a.startsWith('--cap=')) || '').split('=')[1] || '40', 10);
const DRY = process.argv.includes('--dry');

// --- the only "intelligence" we author: curated high-value TITLES (reliable, exist for sure) + a few
// categories that genuinely hold articles. domain -> {titles?, cats?}. Deterministic, broad, low-noise.
const SEED = {
  'computer-science': { cats: ['Categoria:Algoritmi', 'Categoria:Strutture_dati', 'Categoria:Linguaggi_di_programmazione', 'Categoria:Protocolli_di_rete'],
    titles: ['Informatica', 'Algoritmo', 'Compilatore', 'Sistema operativo', 'Memoria ad accesso casuale', 'CPU', 'Bit', 'Byte',
      'Codice sorgente', 'Linguaggio di programmazione', 'Programmazione orientata agli oggetti', 'Ricorsione (informatica)',
      'Complessità computazionale', 'Crittografia', 'Funzione di hash', 'Base di dati', 'Intelligenza artificiale', 'Rete di computer',
      'Internet', 'Hypertext Transfer Protocol', 'TCP/IP', 'Indirizzo IP', 'Domain Name System', 'Sistema embedded', 'Microcontrollore',
      'Codice binario', 'Variabile (informatica)', 'Puntatore (programmazione)', 'Apprendimento automatico', 'Cloud computing'] },
  'electronics': { titles: ['Elettronica', 'Transistor', 'Resistore', 'Condensatore (elettrotecnica)', 'Induttore', 'Diodo',
      'Circuito integrato', 'Amplificatore operazionale', 'Legge di Ohm', 'Corrente elettrica', 'Tensione elettrica', 'Potenza elettrica',
      'Circuito elettrico', 'Semiconduttore', 'Diodo a emissione di luce', 'Microprocessore', 'Segnale analogico', 'Segnale digitale',
      'Convertitore analogico-digitale', 'Relè', 'Trasformatore', 'Oscillatore', 'Frequenza', 'Campo magnetico'] },
  'science': { titles: ['Atomo', 'Molecola', 'Elettrone', 'Protone', 'Neutrone', 'Energia', 'Forza', 'Gravità', 'Termodinamica',
      'Entropia', 'Velocità della luce', 'Relatività ristretta', 'Meccanica quantistica', 'Fotosintesi', 'DNA', 'Cellula', 'Evoluzione',
      'Sistema solare', 'Buco nero', 'Big Bang', 'Tavola periodica', 'Reazione chimica', 'Onda', 'Elettromagnetismo', 'Pressione', 'Temperatura'] },
  'mathematics': { titles: ['Numero primo', 'Frazione', 'Percentuale', 'Equazione', 'Funzione (matematica)', 'Derivata', 'Integrale',
      'Teorema di Pitagora', 'Pi greco', 'Logaritmo', 'Insieme', 'Probabilità', 'Matrice (matematica)', 'Geometria', 'Algebra',
      'Numero reale', 'Numero complesso', 'Radice quadrata', 'Fattoriale', 'Statistica'] },
  'world-history': { cats: ['Categoria:Antica_Roma', 'Categoria:Antica_Grecia'],
    titles: ['Impero romano', 'Antica Grecia', 'Rivoluzione francese', 'Rivoluzione industriale', 'Prima guerra mondiale',
      'Seconda guerra mondiale', 'Guerra fredda', 'Rinascimento', 'Medioevo', 'Antico Egitto', 'Impero bizantino', 'Illuminismo',
      'Scoperta dell\'America', 'Unità d\'Italia', 'Rivoluzione russa'] },
  'geography': { titles: ['Oceano', 'Continente', 'Montagna', 'Fiume', 'Lago', 'Deserto', 'Vulcano', 'Terremoto', 'Clima', 'Equatore',
      'Europa', 'Asia', 'Africa', 'America', 'Oceania', 'Mar Mediterraneo', 'Alpi', 'Himalaya', 'Nilo', 'Rio delle Amazzoni', 'Sahara'] },
  'philosophy': { titles: ['Filosofia', 'Etica', 'Logica', 'Metafisica', 'Epistemologia', 'Ontologia', 'Stoicismo', 'Empirismo',
      'Razionalismo', 'Esistenzialismo', 'Idealismo', 'Materialismo', 'Libero arbitrio', 'Coscienza (filosofia)', 'Dialettica'] },
  // --- maker / dev / security: the highest-value domains for a Cardputer (ESP32) assistant ---
  'embedded': { titles: ['ESP32', 'ESP8266', 'Arduino', 'Raspberry Pi', 'GPIO', 'I²C', 'Serial Peripheral Interface', 'UART',
      'Modulazione di larghezza di impulso', 'Convertitore analogico-digitale', 'Sensore', 'FreeRTOS', 'Sistema operativo real-time',
      'Datasheet', 'Memoria flash', 'EEPROM', 'Bus (informatica)', 'Interrupt', 'Firmware', 'Scheda a microcontrollore', 'Attuatore'] },
  'networking': { titles: ['Wi-Fi', 'Bluetooth', 'Bluetooth Low Energy', 'Indirizzo MAC', 'Ethernet', 'Router', 'Modem',
      'Network address translation', 'Virtual Private Network', 'Proxy', 'Firewall', 'Modello OSI', 'User Datagram Protocol',
      'Transmission Control Protocol', 'MQTT', 'Pacchetto (reti)', 'Fibra ottica', 'Latenza', 'Gateway (informatica)', 'Commutatore (informatica)'] },
  'security': { titles: ['Malware', 'Virus (informatica)', 'Phishing', 'Ransomware', 'Autenticazione', 'Autenticazione a due fattori',
      'Password', 'Exploit', 'Vulnerabilità informatica', 'Hacker', 'Penetration test', 'Cifrario', 'Chiave crittografica',
      'Certificato digitale', 'Transport Layer Security', 'Attacco denial of service', 'Botnet', 'Backdoor', 'Ingegneria sociale', 'Sniffing'] },
  'web-dev': { titles: ['HTML', 'CSS', 'JavaScript', 'JSON', 'Representational State Transfer', 'World Wide Web', 'Browser',
      'HTTP cookie', 'Uniform Resource Locator', 'Server web', 'Document Object Model', 'WebSocket', 'Single-page application', 'XML'] },
  'linux': { titles: ['Linux', 'Kernel Linux', 'Bash', 'Shell (informatica)', 'Interfaccia a riga di comando', 'File system',
      'Distribuzione Linux', 'GNU', 'Open source', 'Software libero', 'Processo (informatica)', 'Demone (informatica)', 'Variabile d\'ambiente', 'Unix'] },
  // --- essential SOFTWARE-ENGINEERING concepts (the core vocabulary of a programmatic assistant) ---
  'cs-advanced': { titles: ['API', 'Libreria (software)', 'Framework', 'Debugging', 'Interprete (informatica)', 'Garbage collection',
      'Thread (informatica)', 'Concorrenza (informatica)', 'Mutua esclusione', 'Stallo (informatica)', 'Pila (informatica)',
      'Coda (informatica)', 'Lista concatenata', 'Albero (informatica)', 'Grafo (informatica)', 'O-grande', 'Design pattern',
      'Refactoring', 'Controllo versione', 'Git (software)', 'Espressione regolare', 'Programmazione funzionale',
      'Gestione delle eccezioni', 'Polimorfismo (informatica)', 'Ereditarietà (informatica)', 'Incapsulamento (informatica)',
      'Programmazione concorrente', 'Programmazione asincrona', 'Callback (programmazione)', 'Test del software'] },
  'databases': { titles: ['SQL', 'NoSQL', 'Sistema di gestione di basi di dati', 'Modello relazionale', 'Chiave primaria',
      'Chiave esterna', 'Indice (basi di dati)', 'Transazione (basi di dati)', 'ACID', 'Join (SQL)', 'Normalizzazione (basi di dati)',
      'Query', 'MySQL', 'PostgreSQL', 'MongoDB', 'Redis', 'SQLite', 'Database distribuito'] },
  'devops': { titles: ['Docker (software)', 'Container (virtualizzazione)', 'Kubernetes', 'Virtualizzazione', 'Macchina virtuale',
      'DevOps', 'Integrazione continua', 'Microservizi', 'Bilanciamento del carico', 'Cache', 'Scalabilità',
      'Tolleranza ai guasti', 'Sistema distribuito', 'Webhook', 'Log (informatica)'] },
  'ai-ml': { titles: ['Rete neurale artificiale', 'Apprendimento profondo', 'Apprendimento supervisionato',
      'Apprendimento non supervisionato', 'Apprendimento per rinforzo', 'Rete neurale convoluzionale',
      'Trasformatore (modello di apprendimento automatico)', 'Modello linguistico di grandi dimensioni', 'Discesa del gradiente',
      'Overfitting', 'Funzione di attivazione', 'Retropropagazione dell\'errore', 'Insieme di dati', 'Tensore',
      'Visione artificiale', 'Elaborazione del linguaggio naturale', 'Regressione lineare', 'Albero di decisione'] },
  // --- programming languages (core for a programmatic assistant) ---
  'programming-languages': { titles: ['Python', 'C (linguaggio)', 'C++', 'Rust (linguaggio di programmazione)', 'Go (linguaggio)',
      'Java (linguaggio di programmazione)', 'JavaScript', 'TypeScript', 'Kotlin (linguaggio di programmazione)', 'Swift (linguaggio di programmazione)',
      'PHP', 'Ruby (linguaggio di programmazione)', 'Assembly', 'Haskell (linguaggio)', 'Linguaggio di programmazione C Sharp',
      'Lua (linguaggio di programmazione)', 'Perl', 'MATLAB', 'R (linguaggio di programmazione)', 'Fortran'] },
  'cs-theory': { titles: ['Macchina di Turing', 'Automa a stati finiti', 'Problema P contro NP', 'Algebra di Boole',
      'Logica proposizionale', 'Teoria della calcolabilità', 'Grammatica formale', 'Linguaggio formale', 'Lambda calcolo',
      'Tesi di Church-Turing', 'Funzione ricorsiva', 'Teoria della complessità computazionale', 'Automa a pila',
      'Espressione booleana', 'Codifica (informatica)', 'Teoria dell\'informazione', 'Entropia (teoria dell\'informazione)'] },
  // --- general "real AI" breadth: nature & mind ---
  'astronomy': { titles: ['Stella', 'Pianeta', 'Galassia', 'Via Lattea', 'Sole', 'Luna', 'Terra', 'Marte (astronomia)',
      'Giove (astronomia)', 'Saturno (astronomia)', 'Supernova', 'Nebulosa', 'Cometa', 'Asteroide', 'Telescopio',
      'Anno luce', 'Costellazione', 'Universo', 'Gravità', 'Materia oscura', 'Stazione spaziale'] },
  'biology': { titles: ['Batterio', 'Enzima', 'Mitocondrio', 'Cromosoma', 'Gene', 'RNA', 'Ecosistema', 'Neurone',
      'Ormone', 'Riproduzione', 'Respirazione cellulare', 'Tessuto (biologia)', 'Organo (anatomia)', 'Specie',
      'Biodiversità', 'Catena alimentare', 'Genetica', 'Microbiologia', 'Virus'] },
  'chemistry': { titles: ['Elemento chimico', 'Composto chimico', 'Legame chimico', 'Acido', 'Base (chimica)', 'pH',
      'Ossidazione', 'Riduzione (chimica)', 'Catalizzatore', 'Soluzione (chimica)', 'Mole (unità di misura)', 'Isotopo',
      'Ione', 'Polimero', 'Idrocarburo', 'Carbonio', 'Ossigeno', 'Idrogeno', 'Acqua', 'Sale (chimica)'] },
  'psychology': { titles: ['Psicologia', 'Memoria (psicologia)', 'Apprendimento', 'Emozione', 'Motivazione', 'Percezione',
      'Attenzione', 'Intelligenza', 'Personalità', 'Cognizione', 'Inconscio', 'Comportamento', 'Empatia',
      'Bias cognitivo', 'Condizionamento classico', 'Psicoanalisi'] },
  // --- top-level DISCIPLINES: the bare subject names people ask about ("cos'è la fisica"). Without these
  // a single word like "fisica" n-gram-collides with "musica". Bounded set (the subjects), not combinatorial.
  'disciplines': { titles: ['Fisica', 'Chimica', 'Matematica', 'Biologia', 'Geografia', 'Storia', 'Filosofia',
      'Astronomia', 'Psicologia', 'Economia', 'Informatica', 'Elettronica', 'Statistica', 'Letteratura', 'Medicina',
      'Geologia', 'Sociologia', 'Linguistica', 'Architettura', 'Diritto', 'Ingegneria', 'Biochimica', 'Astrofisica',
      'Genetica', 'Ecologia', 'Anatomia', 'Fisiologia', 'Meteorologia', 'Robotica', 'Logica matematica'] },
  // --- general life knowledge (useful, broad) ---
  'health': { titles: ['Salute', 'Medicina', 'Vaccino', 'Antibiotico', 'Sistema immunitario', 'Diabete', 'Ipertensione arteriosa',
      'Primo soccorso', 'Allergia', 'Influenza', 'Stress', 'Anatomia umana', 'Sistema nervoso', 'Apparato digerente',
      'Apparato respiratorio', 'Colesterolo', 'Metabolismo', 'Antinfiammatorio'] },
  'food': { titles: ['Alimentazione', 'Carboidrato', 'Grasso (alimento)', 'Caloria', 'Dieta mediterranea', 'Fermentazione',
      'Glutine', 'Lattosio', 'Vitamina', 'Proteina', 'Fibra alimentare', 'Zucchero', 'Caffè', 'Olio di oliva'] },
  'economy': { titles: ['Economia', 'Inflazione', 'Prodotto interno lordo', 'Moneta', 'Banca', 'Borsa valori', 'Tasso di interesse',
      'Debito pubblico', 'Capitalismo', 'Imposta', 'Criptovaluta', 'Mercato finanziario', 'Domanda e offerta', 'Interesse (economia)'] },
  'art-music': { titles: ['Arte', 'Pittura', 'Scultura', 'Impressionismo', 'Prospettiva', 'Musica', 'Nota musicale',
      'Strumento musicale', 'Jazz', 'Sinfonia', 'Melodia', 'Armonia (musica)', 'Ritmo', 'Opera (musica)'] },
  // --- offline emergency / survival domains ---
  'survival-firstaid': { titles: ['Primo soccorso', 'Rianimazione cardiopolmonare', 'Ustione', 'Nodo', 'Bussola', 'Ipotermia', 'Colpo di calore', 'Disidratazione', 'Purificazione dell\'acqua', 'Bivacco'] },
  'electronics-reference': { titles: ['Saldatura', 'Multimetro', 'Oscilloscopio', 'Circuito stampato'] },
  'cooking-safety': { titles: ['Sicurezza alimentare', 'Pastorizzazione', 'Fermentazione', 'Lievitazione', 'Conservazione degli alimenti', 'Intossicazione alimentare'] },
  'automotive-diy': { titles: ['Pneumatico', 'Alternatore', 'Fusibile', 'Batteria al piombo', 'Liquido di raffreddamento', 'Freno a disco', 'Motore a combustione interna'] },
};

async function wp(lang, params) {
  const u = new URL(`https://${lang}.wikipedia.org/w/api.php`);
  u.search = new URLSearchParams({ format: 'json', formatversion: '2', maxlag: '5', ...params }).toString();
  for (let attempt = 0; attempt < 5; attempt++) {
    try {
      const r = await fetch(u, { headers: { 'User-Agent': UA } });
      if (r.status === 429 || r.status === 503) { await sleep(2000 * (attempt + 1)); continue; }   // throttled
      if (r.ok) { const j = await r.json(); if (!j.error || j.error.code !== 'maxlag') return j; }
    } catch {}
    await sleep(800 * (attempt + 1));
  }
  return null;
}

async function catList(lang, cat, type, cap) {     // type: 'page' | 'subcat'
  const out = [];
  let cont;
  do {
    const j = await wp(lang, { action: 'query', list: 'categorymembers', cmtitle: cat, cmtype: type,
                               cmlimit: '100', ...(cont ? { cmcontinue: cont } : {}) });
    for (const p of (j?.query?.categorymembers || [])) out.push(p.title);
    cont = j?.continue?.cmcontinue;
    await sleep(100);
  } while (cont && out.length < cap);
  return out.slice(0, cap);
}

// Direct article pages; if a high-level category holds mostly SUBCATEGORIES, recurse one level into
// them so broad seeds ("Elettronica", "Chimica") still yield real articles.
async function members(lang, cat, cap) {
  const pages = await catList(lang, cat, 'page', cap);
  if (pages.length >= cap) return pages.slice(0, cap);
  for (const sub of await catList(lang, cat, 'subcat', 14)) {
    if (pages.length >= cap) break;
    for (const t of await catList(lang, sub, 'page', cap - pages.length)) if (!pages.includes(t)) pages.push(t);
  }
  return pages.slice(0, cap);
}

// Batched intro extracts (exsentences=2) + short description. extracts API caps at 20 titles/req.
async function extracts(lang, titles) {
  const map = {};
  for (let i = 0; i < titles.length; i += 20) {
    const batch = titles.slice(i, i + 20);
    const j = await wp(lang, { action: 'query', prop: 'extracts|description', exintro: '1', explaintext: '1',
                               exsentences: '2', exlimit: 'max', redirects: '1', titles: batch.join('|') });
    for (const p of (j?.query?.pages || [])) {
      if (p.missing || !p.extract) continue;
      map[p.title] = { extract: p.extract.replace(/\s+/g, ' ').trim(), desc: (p.description || '').trim() };
    }
    await sleep(150);
  }
  return map;
}

const slug = s => s.toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '')
  .replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 60);

const BAD = /\bdisambigua|\b(lista|elenco|glossario)\b|may refer to|può riferirsi|puo riferirsi/i;
const isPerson = (extract, desc) => /\b(è stato|è stata|è un[ao]? (politico|scrittore|scrittrice|fisico|matematico|filosofo|artista|pittore|attore|attrice|musicista|imperatore|re |regina)|was an? |is an? .*born|\(\d{3,4}\s*[–-])/i.test((desc || '') + ' ' + (extract || ''));

function asks(title, person) {
  const it = [title, `cos'è ${title}`, `che cos'è ${title}`, `spiegami ${title}`, `parlami di ${title}`,
              `cosa sai di ${title}`, `conosci ${title}`, `dimmi di ${title}`];
  const en = [title, `what is ${title}`, `what's ${title}`, `explain ${title}`,
              `what do you know about ${title}`, `do you know ${title}`];   // no "tell me about X": its
                                                                           // "tell me a" n-gram snags "tell me a joke"
  if (person) { it.push(`chi è ${title}`, `chi era ${title}`); en.push(`who is ${title}`, `who was ${title}`); }
  return { it: [...new Set(it)].slice(0, 9), en: [...new Set(en)].slice(0, 8) };
}

// --- load existing corpus -> slugs to skip (no overlap) ---
const seen = new Set();
for (const f of readdirSync(KDIR).filter(f => f.endsWith('.jsonl'))) {
  for (const line of readFileSync(join(KDIR, f), 'utf8').split(/\r?\n/)) {
    const t = line.trim(); if (!t || t.startsWith('//')) continue;
    try {
      const o = JSON.parse(t);
      if (o.id) seen.add(o.id.split('.').pop());                    // last id segment = entity slug
      for (const lang of ['it', 'en']) for (const a of (o.ask?.[lang] || [])) seen.add(slug(a));
    } catch {}
  }
}
console.log(`[ingest] existing corpus: ${seen.size} slugs to avoid`);

let total = 0, skippedDup = 0, skippedBad = 0;
const FORCE = process.argv.includes('--force');
for (const [category, spec] of Object.entries(SEED)) {
  // Don't clobber a domain already ingested+enriched (re-run only adds NEW domains). --force to rebuild.
  if (!FORCE && !DRY && existsSync(join(KDIR, `wiki-${category}.jsonl`))) { console.log(`[${category}] exists, skip (use --force)`); continue; }
  const titles = [];
  for (const t of (spec.titles || [])) if (!titles.includes(t)) titles.push(t);
  for (const c of (spec.cats || [])) { const m = await members('it', c, CAP); for (const t of m) if (!titles.includes(t)) titles.push(t); }
  const itEx = await extracts('it', titles);

  const cards = [];
  for (const title of titles) {
    if (BAD.test(title)) { skippedBad++; continue; }
    const it = itEx[title];
    if (!it || it.extract.length < 50 || BAD.test(it.extract)) { skippedBad++; continue; }
    const s = slug(title);
    if (!s || seen.has(s)) { skippedDup++; continue; }
    seen.add(s);
    const a = asks(title, isPerson(it.extract, it.desc));            // bilingual ASKS via templates (free)
    cards.push({ id: `wiki.it.${s}`, category, action: 'answer', reply: { it: it.extract }, ask: a,
                 source: `wikipedia:it:${title}`, lang_primary: 'it' });
  }
  total += cards.length;
  console.log(`[${category}] ${cards.length} cards from ${titles.length} titles`);
  if (DRY && cards.length) { const c = cards[0]; console.log(`    e.g. ${c.id}: ${c.reply.it.slice(0, 110)}...`); }
  if (!DRY && cards.length) {
    const out = cards.map(c => JSON.stringify(c)).join('\n') + '\n';
    writeFileSync(join(KDIR, `wiki-${category}.jsonl`),
      `// Bulk Wikipedia short-abstract cards (ingest_wiki.mjs). Source-anchored, deterministic, deduped.\n` + out);
  }
}
console.log(`\n[ingest] wrote ${total} new cards  (skipped ${skippedDup} dup, ${skippedBad} low-quality)${DRY ? ' [DRY]' : ''}`);
