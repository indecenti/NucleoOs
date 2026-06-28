#!/usr/bin/env node
// Generate tools/test-registry.json — the SINGLE central catalog of every test runnable OFFLINE in ANIMA
// (no web models: the WebLLM/forge tests run with MockEngine). The GUI (test_lab.py), the CLI (run-tests.mjs)
// and docs/testing.md all read this ONE file, so the test universe lives in one place.
//
// NEW TESTS ARE INCLUDED AUTOMATICALLY: this generator (1) parses every gate in gate.mjs, and (2) WALKS the
// repo for every *.test.mjs. So a new test added as a *.test.mjs, or wired into gate.mjs, appears here on the
// next `npm run test:registry` with zero manual edits. Only a brand-new standalone runner that is neither a
// *.test.mjs nor a gate needs a one-line entry in `extras` below.
// NB: pure app-packaging lint (tools/validate.mjs) is intentionally NOT listed — it checks app manifests,
// not ANIMA behaviour, and depends on app-config state. Run it separately with `npm run validate`.
import { readFileSync, writeFileSync, readdirSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join, relative } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..');

// ---- categories (NL first — the heart of the system) ---------------------------------------------
const categories = [
  { id: 'nl-hallucination', label: 'NL · Anti-allucinazione', nl: true,  desc: 'Trappole avversariali: ogni risposta confidente a una richiesta inananswerabile è un fallimento. Il cuore della sicurezza.' },
  { id: 'nl-skill-routing', label: 'NL · Routing skill',       nl: true,  desc: 'La richiesta giusta raggiunge lo strumento giusto; la skill non pesta mai i piedi alla conoscenza e viceversa.' },
  { id: 'nl-knowledge',     label: 'NL · Conoscenza & recupero', nl: true, desc: 'Retrieval L1/L2, definizioni, descrizioni, entità: risposte fondate o astensione onesta, mai fabbricate.' },
  { id: 'nl-reasoning',     label: 'NL · Ragionamento',        nl: true,  desc: 'Deduzione KGE, recall HDC, composizione: fatti dedotti, mai memorizzati né inventati.' },
  { id: 'nl-math',          label: 'NL · Matematica & calcolo', nl: true, desc: 'Ogni risposta matematica esatta + parità col gemello JS.' },
  { id: 'nl-memory',        label: 'NL · Memoria & profilo',   nl: true,  desc: 'Apprendimento utente e profilo personale tipato: recall per parafrasi, zero misattribuzioni.' },
  { id: 'nl-translate',     label: 'NL · Traduzione',          nl: true,  desc: 'Traduttore offline IT↔EN via dizionario: tradotti fondati, decline onesto, zero falsi positivi.' },
  { id: 'nl-weather',       label: 'NL · Meteo',               nl: true,  desc: 'NLU meteo: estrazione luogo/data, tier offline/ibrida/online.' },
  { id: 'cascade-infra',    label: 'Cascata · Infrastruttura', nl: false, desc: 'Orchestratore, loop agentico, coerenza dei pacchetti indice/encoder.' },
  { id: 'knowledge-graph',  label: 'Knowledge Graph & Ledger', nl: false, desc: 'Auto-evoluzione tassonomia (Wikidata) e libro mastro verificabile immutabile.' },
  { id: 'forge-webllm',     label: 'ANIMA Forge / WebLLM (M4)', nl: false, desc: 'Editor agentico nel browser: firewall azioni, verifica cross-substrato, download/engine, provenienza.' },
  { id: 'app-spreadsheet',  label: 'App · Foglio di calcolo',  nl: true,  desc: 'Motore formule Excel-class + NL→formula del copilot.' },
  { id: 'app-paint',        label: 'App · Paint',              nl: false, desc: 'Imaging e comandi NL della Paint Atelier.' },
  { id: 'app-shell',        label: 'App · Shell & tool',       nl: false, desc: 'Browser, scorciatoie, terminale, percorsi file, NFV video.' },
  { id: 'app-device',       label: 'App · Device & HW',        nl: false, desc: 'UI device e bridge USB-HID.' },
  { id: 'device-load',      label: 'Device · Carico & RAM',    nl: false, desc: "Stress concorrente del web server su device reale: l'arbitro heavy-work serializza il TLS, l'heap regge sopra il floor, zero crash. Richiede un device flashato (SKIP se assente)." },
  { id: 'connect-transfer', label: 'Connessione · Trasferimento (Vicino)', nl: false, desc: 'Scambio file/comandi device-to-device via ESP-NOW: protocollo affidabile host-testato (finestra+ACK+CRC32+resume sotto perdita/riordino/duplicazione, codec Bruce wire-compatibile) e skill ANIMA gated (schema chiuso, azioni mutanti fail-closed).' },
  { id: 'security',         label: 'Sicurezza · Attacchi (test autorizzati)', nl: false, desc: 'Strumenti offensivi per test autorizzati, host-testati (pura logica C, nessun device): il core dei frame dell\'engine Ethernet L2/L3 (W5500 MACRAW — ARP/DHCP/TCP build+parse, checksum IP/UDP/TCP, subnet, MAC random, OUI) e il core advertisement BLE (Apple Continuity / Swift Pair / Fast Pair / iBeacon framing + report map HID, invariante <=31 byte). Un byte sbagliato = un attacco morto.' },
  { id: 'build-lint',       label: 'Build & lint',             nl: false, desc: 'Validazione asset/manifest e drift dello spec API.' },
];

// ANIMA = la cascata DETERMINISTICA offline (M1), SENZA modelli generativi. Esclude forge-webllm (il
// substrato WebLLM/M4: anche se mockato riguarda i MODELLI), i test delle APP (foglio/paint/shell/device)
// e il build-lint. È "ANIMA offline, solo quella" → flag `anima` su categorie e test.
const ANIMA_CATS = new Set(['nl-hallucination', 'nl-skill-routing', 'nl-knowledge', 'nl-reasoning',
  'nl-math', 'nl-memory', 'nl-translate', 'nl-weather', 'cascade-infra', 'knowledge-graph']);
for (const c of categories) c.anima = ANIMA_CATS.has(c.id);

// ---- explicit category per ANIMA gate (by gate name) ---------------------------------------------
const gateCat = {
  'pack-coherence': 'cascade-infra', 'regress (corpus+L1)': 'nl-knowledge', 'route-check': 'cascade-infra',
  'l1-parity (prefilter)': 'nl-knowledge', 'l1-recall (asym)': 'nl-knowledge', 'agent-check': 'cascade-infra',
  'math-check': 'nl-math', 'ood-check (safety)': 'nl-hallucination', 'reliability (traps)': 'nl-hallucination',
  'halluc-stress': 'nl-hallucination', 'halluc-battery (NL)': 'nl-hallucination', 'realistic (NL)': 'nl-skill-routing',
  'skill<->knowledge boundary': 'nl-skill-routing', 'metamorph (NL)': 'nl-hallucination', 'nl-stress (offline)': 'nl-hallucination',
  'fluency-grounded (L2)': 'nl-knowledge', 'describe-stress (NL)': 'nl-knowledge', 'skill-routing': 'nl-skill-routing',
  'skill-routing-2': 'nl-skill-routing', 'false-positives (NL)': 'nl-hallucination', 'false-positives-2 (NL)': 'nl-hallucination',
  'cross-topic halluc (NL)': 'nl-hallucination', 'halluc-probe IT': 'nl-hallucination', 'halluc-probe EN': 'nl-hallucination',
  'skill-isolation (paint)': 'nl-skill-routing', 'image-gen stress (NL)': 'nl-skill-routing', 'action-tier (false-act)': 'nl-skill-routing',
  'cross-skill (xskill)': 'nl-skill-routing', 'teach-loop (learn)': 'nl-memory', 'profile (personal)': 'nl-memory',
  'skill-coverage IT (s6)': 'nl-skill-routing', 'skill-coverage EN (s6)': 'nl-skill-routing',
  'skill-coverage IT (s7)': 'nl-skill-routing', 'skill-coverage EN (s7)': 'nl-skill-routing',
  'skill-coverage IT (s8)': 'nl-skill-routing', 'skill-coverage EN (s8)': 'nl-skill-routing',
  'date/excel halluc IT': 'nl-hallucination', 'date/excel halluc EN': 'nl-hallucination',
  'combinator+excel IT': 'nl-reasoning', 'combinator+excel EN': 'nl-reasoning',
  'math-dialog IT': 'nl-math', 'math-dialog EN': 'nl-math',
  'dict-sync (translate)': 'nl-translate', 'translate (IT<->EN)': 'nl-translate', 'kge-eval': 'nl-reasoning',
  'hdc-eval': 'nl-reasoning', 'combinator-eval': 'nl-reasoning', 'typed-facets (KG)': 'nl-reasoning',
  'typed-nl (facet)': 'nl-knowledge', 'entity-detect (online)': 'nl-knowledge', 'clean-extract (wiki)': 'nl-knowledge',
  'auto-evolution (VKL)': 'knowledge-graph', 'ledger-attack (VKL)': 'knowledge-graph', 'akb5-content (sharded)': 'nl-knowledge',
  'arbiter (concurrency)': 'cascade-infra',
  'link-proto (espnow)': 'connect-transfer', 'nearby-skill (scoped)': 'connect-transfer',
  'eth-frames (wired)': 'security',
  'ble-adv (spam/beacon)': 'security',
  'ducky (payloads)': 'security',
  'weather (meteo)': 'app-shell',
};
const NL_GATES = new Set(Object.entries(gateCat).filter(([, c]) => categories.find(x => x.id === c)?.nl).map(([n]) => n));

// ---- file→category for *.test.mjs and extras -----------------------------------------------------
function fileCat(f) {
  if (/forge-/.test(f)) return 'forge-webllm';
  if (/paint-/.test(f)) return 'app-paint';
  if (/spread|spreadsheet/.test(f)) return 'app-spreadsheet';
  if (/weather\.test/.test(f)) return 'nl-weather';
  if (/translate\.test/.test(f)) return 'nl-translate';
  if (/calc-eval/.test(f)) return 'nl-math';
  if (/device-ui|usb-hid/.test(f)) return 'app-device';
  if (/browser-check|app-shortcuts|shortcuts|terminal-commands|files-path|nfv/.test(f)) return 'app-shell';
  if (/validate/.test(f)) return 'build-lint';
  return 'app-shell';
}

// inventory descriptions (file basename -> verifies/nl/lang)
let inv = [];
try { inv = JSON.parse(readFileSync('/tmp/inv.json', 'utf8')); } catch {}
const invBy = new Map();
for (const t of inv) { const b = (t.file || '').split(/[\\/]/).pop(); if (b) invBy.set(b, t); }

const tests = [];
const slug = (s) => s.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');

// 1) ANIMA gates (parse gate.mjs)
const gsrc = readFileSync(join(here, 'anima-host', 'gate.mjs'), 'utf8');
const re = /\{\s*name:\s*['"]([^'"]+)['"],\s*cmd:\s*['"]([^'"]+)['"],\s*args:\s*\[([^\]]*)\]/g;
let m;
while ((m = re.exec(gsrc))) {
  const name = m[1]; if (name === 'unit tests') continue;   // expanded individually below
  const args = m[2 + 1].split(',').map(s => s.trim().replace(/^['"]|['"]$/g, '')).filter(Boolean);
  const cat = gateCat[name] || 'cascade-infra';
  tests.push({ id: 'gate-' + slug(name), label: name, category: cat, kind: 'gate', cmd: m[2], args,
    verifies: '', nl: !!categories.find(x => x.id === cat)?.nl, lang: 'both', in_gate: true });
}

// 2) every *.test.mjs (run individually via node --test)
const testFiles = [];
(function walk(dir) {
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    if (e.name === 'node_modules' || e.name.startsWith('.')) continue;
    const p = join(dir, e.name);
    if (e.isDirectory()) walk(p);
    else if (e.name.endsWith('.test.mjs')) testFiles.push(p);
  }
})(join(repo, 'tools'));
for (const p of [join(repo, 'apps', 'spreadsheet', 'test.mjs')]) if (existsSync(p)) testFiles.push(p);
testFiles.sort();
for (const p of testFiles) {
  const rel = relative(repo, p).replace(/\\/g, '/');
  const base = rel.split('/').pop();
  const cat = fileCat(rel);
  const info = invBy.get(base);
  tests.push({ id: 'unit-' + slug(base.replace('.test.mjs', '').replace('.mjs', '')), label: base, category: cat, kind: 'unit',
    cmd: 'node', args: ['--test', rel], verifies: info?.verifies || '', nl: info?.nl ?? !!categories.find(x => x.id === cat)?.nl,
    lang: info?.lang || 'na', in_gate: true /* via the collective "unit tests" gate */ });
}

// 3) extra non-gate runners
const extras = [
  { label: 'spread-nlu (regress)', cat: 'app-spreadsheet', cmd: 'node', args: ['tools/spread-nlu/regress.mjs'], verifies: 'Regressione NLU del foglio di calcolo (NL→formula).' },
  { label: 'spreadsheet engine', cat: 'app-spreadsheet', cmd: 'node', args: ['apps/spreadsheet/test.mjs'], verifies: 'Motore formule Excel-class (SUMIFS/VLOOKUP/IF annidato…).' },
  { label: 'api-spec drift', cat: 'build-lint', cmd: 'node', args: ['tools/gen-api-spec.mjs', '--check'], verifies: 'Lo Swagger è rigenerato dalle route REALI del firmware (anti-drift).' },
  { label: 'workspace · nlfs', cat: 'nl-skill-routing', cmd: 'node', args: ['tools/anima-host/nlfs-check.mjs'], verifies: 'ANIMA Workspace: comandi NL sul filesystem (crea/leggi/sposta) della "Claude Code offline".' },
  { label: 'workspace · context', cat: 'nl-memory', cmd: 'node', args: ['tools/anima-host/context-check.mjs'], verifies: 'ANIMA Workspace: compattazione del contesto della sessione.' },
  { label: 'workspace · qgen', cat: 'nl-knowledge', cmd: 'node', args: ['tools/anima-host/qgen-check.mjs'], verifies: 'ANIMA Workspace: pipeline di generazione domande (qgen).' },
  { label: 'arbiter load (device)', cat: 'device-load', cmd: 'python', args: ['tools/test_arbiter_load.py'], verifies: 'Carico concorrente sul web server reale: liveness (mai irraggiungibile), heap floor, degrado grazioso (503 non crash). Richiede device flashato; SKIP se assente.' },
  { label: 'games · foundation+P2P+brain', cat: 'app-shell', cmd: 'node', args: ['tools/games-host/all.mjs'], verifies: 'Game Center: SDK host-authoritative, trasporto P2P/mailbox, logica Tris/Forza4/Pong, cervello LLM. (tools/games-host/)' },
  { label: 'dj · planner+npx', cat: 'app-shell', cmd: 'node', args: ['apps/dj/test/all.mjs'], verifies: 'DJ engine: planner beatmatch/armonico/energia + decode .npx (apps/dj/test/). App WIP, non installata.' },
  { label: 'gz freshness', cat: 'build-lint', cmd: 'node', args: ['tools/check-gz.mjs'], verifies: 'Ogni .gz servito (web/shell, apps/*/www) è in sync col sorgente — niente codice vecchio/orfano spedito al device (gotcha .gz shadowing).' },
];
for (const e of extras) tests.push({ id: 'extra-' + slug(e.label), label: e.label, category: e.cat, kind: 'extra',
  cmd: e.cmd, args: e.args, verifies: e.verifies, nl: !!categories.find(x => x.id === e.cat)?.nl, lang: 'na', in_gate: false });

// ---- METRICS: each test can emit numbers parsed from its OWN stdout, so the cockpit is a health monitor,
// not just pass/fail. Modular: add a row here and the dashboard + trends pick it up automatically.
//   k='cases'  → NL requests this test exercises (summed into "casi NL coperti")
//   k='halluc' → fabrications/false-positives found (summed into "Allucinazioni" — must stay 0)
//   k='diffs'  → routing drift vs the golden snapshot (must stay 0)
//   agg='sum'  → sum every capture group (a line with 3 numbers); default 'last' = group 1 of the last match
const METRICS = {
  'gate-halluc-stress':            [{ k: 'cases', re: 'HALLUCINATIONS:\\s*\\d+/(\\d+)' }, { k: 'halluc', re: 'HALLUCINATIONS:\\s*(\\d+)/' }],
  'gate-halluc-battery-nl':        [{ k: 'cases', re: '(\\d+)\\s*adversarial traps' }, { k: 'halluc', re: 'traps\\s*\\u00b7\\s*(\\d+)\\s*fabrication' }],
  'gate-metamorph-nl':             [{ k: 'cases', re: '->\\s*(\\d+)\\s*derived mutants' }, { k: 'halluc', re: '(\\d+)\\s*invariance break' }],
  'gate-realistic-nl':             [{ k: 'cases', re: '(\\d+)\\s*programmatic requests' }],
  'gate-skill-knowledge-boundary': [{ k: 'cases', re: '(\\d+) definition \\+ (\\d+) compute \\+ (\\d+) reverse', agg: 'sum' }],
  'gate-nl-stress-offline':        [{ k: 'halluc', re: 'TOTAL HALLUCINATIONS:\\s*(\\d+)' }],
  'gate-describe-stress-nl':       [{ k: 'cases', re: '(\\d+) cases' }, { k: 'halluc', re: 'fabrications (\\d+)' }],
  'gate-cross-topic-halluc-nl':    [{ k: 'cases', re: '\\u2014\\s*\\d+/(\\d+) pass' }],
  'gate-skill-routing':            [{ k: 'cases', re: '\\u2014\\s*\\d+/(\\d+) pass' }],
  'gate-skill-routing-2':          [{ k: 'cases', re: '\\u2014\\s*\\d+/(\\d+) pass' }],
  'gate-false-positives-nl':       [{ k: 'cases', re: '\\u2014\\s*\\d+/(\\d+) pass' }],
  'gate-false-positives-2-nl':     [{ k: 'cases', re: '\\u2014\\s*\\d+/(\\d+) pass' }],
  'gate-image-gen-stress-nl':      [{ k: 'cases', re: '\\u2014\\s*\\d+/(\\d+) pass' }],
  'gate-action-tier-false-act':    [{ k: 'cases', re: '\\u2014\\s*\\d+/(\\d+) pass' }],
  'gate-cross-skill-xskill':       [{ k: 'cases', re: '\\u2014\\s*\\d+/(\\d+) pass' }],
  'gate-skill-coverage-it-s6':     [{ k: 'cases', re: '\\u2014\\s*\\d+/(\\d+) pass' }],
  'gate-skill-coverage-en-s6':     [{ k: 'cases', re: '\\u2014\\s*\\d+/(\\d+) pass' }],
  'gate-skill-coverage-it-s7':     [{ k: 'cases', re: '\\u2014\\s*\\d+/(\\d+) pass' }],
  'gate-skill-coverage-en-s7':     [{ k: 'cases', re: '\\u2014\\s*\\d+/(\\d+) pass' }],
  'gate-skill-coverage-it-s8':     [{ k: 'cases', re: '\\u2014\\s*\\d+/(\\d+) pass' }],
  'gate-skill-coverage-en-s8':     [{ k: 'cases', re: '\\u2014\\s*\\d+/(\\d+) pass' }],
  'gate-date-excel-halluc-it':     [{ k: 'cases', re: '(\\d+) honestly abstained' }],
  'gate-date-excel-halluc-en':     [{ k: 'cases', re: '(\\d+) honestly abstained' }],
  'gate-combinator-excel-it':      [{ k: 'cases', re: '\\u2014\\s*\\d+/(\\d+) pass' }],
  'gate-combinator-excel-en':      [{ k: 'cases', re: '\\u2014\\s*\\d+/(\\d+) pass' }],
  'gate-math-dialog-it':           [{ k: 'cases', re: '\\d+/(\\d+) turns correct' }],
  'gate-math-dialog-en':           [{ k: 'cases', re: '\\d+/(\\d+) turns correct' }],
  'gate-typed-nl-facet':           [{ k: 'cases', re: '(\\d+)/(\\d+) pass' }],
  'gate-math-check':               [{ k: 'cases', re: '(\\d+)/\\d+ exact' }],
  'gate-route-check':              [{ k: 'diffs', re: '(\\d+) routing change' }],
  'gate-ood-check-safety':         [{ k: 'halluc', re: 'FALSE-POSITIVE\\(bad\\)\\s+(\\d+)' }],
  'gate-reliability-traps':        [{ k: 'halluc', re: 'FALSE-POSITIVE\\(bad\\)\\s+(\\d+)' }, { k: 'cases', re: 'out-of-scope \\((\\d+)\\)' }],
  'gate-akb5-content-sharded':     [{ k: 'cases', re: 'recall \\d+/(\\d+)' }, { k: 'halluc', re: 'HALLUCINATIONS (\\d+)' }],
  'gate-halluc-probe-it':          [{ k: 'cases', re: '(\\d+) honestly abstained' }],
  'gate-halluc-probe-en':          [{ k: 'cases', re: '(\\d+) honestly abstained' }],
};
for (const t of tests) if (METRICS[t.id]) t.metrics = METRICS[t.id];
for (const t of tests) t.anima = ANIMA_CATS.has(t.category);   // belongs to the deterministic ANIMA cascade (no models, no apps)

// ---- HEALTH: the headline aggregates the dashboard shows. Each rolls up a per-test metric across the suite,
// so a new test that emits 'cases'/'halluc' lifts these automatically. (special:'green' = green/total tests.)
const health = [
  { id: 'hallucinations', label: 'Allucinazioni', metric: 'halluc', agg: 'sum', goal: 0, critical: true,
    hint: 'Fabbricazioni + falsi-positivi su tutte le trappole avversariali. DEVE essere 0.' },
  { id: 'nl_cases', label: 'Casi NL coperti', metric: 'cases', agg: 'sum',
    hint: 'Quante richieste in linguaggio naturale ANIMA è verificata a gestire (somma di tutte le suite NL).' },
  { id: 'route_drift', label: 'Drift routing', metric: 'diffs', agg: 'sum', goal: 0,
    hint: 'Cambi di instradamento rispetto allo snapshot golden. 0 = nessuna regressione di routing.' },
  { id: 'green', label: 'Test verdi', special: 'green',
    hint: 'Test passati sul totale (escludendo gli skip intenzionali).' },
];

const out = { generated: 'tools/gen-test-registry.mjs', categories, tests, health };
writeFileSync(join(repo, 'tools', 'test-registry.json'), JSON.stringify(out, null, 2) + '\n');
const nlCount = tests.filter(t => t.nl).length;
console.log(`[test-registry] ${tests.length} tests across ${categories.length} categories (${nlCount} natural-language) -> tools/test-registry.json`);
for (const c of categories) { const n = tests.filter(t => t.category === c.id).length; if (n) console.log(`  ${c.label.padEnd(34)} ${n}`); }
