// grok-live-check.mjs — REAL Groq (xAI-branded "Grok" config) API verification of the ANIMA context
// engine + the Groq multi-agent CONTRACT layer. Hits the live endpoint with the device's own key.
//
//   node tools/anima-host/grok-live-check.mjs
//
// Key source (first hit wins): env GROQ_API_KEY, else deploy/sd/data/anima/teacher.json (the device key).
// Three suites:
//   A. CAPABILITY (10) — programming / story / reasoning via the SINGLE-CALL chat path (contextkit.assemble
//      → Groq), structural assertions a real answer must satisfy. Runs on the CONFIGURED chat model.
//   B. CONTEXT (10)    — multi-turn histories where the answer is ONLY derivable from prior turns
//      (made-up facts, chaining, recall). Proves the engine keeps the model contextual even on weak 8B.
//   C. CONTRACT (multi-agent) — real Groq JSON-mode orchestrator plan + the tool-use loop with MOCKED
//      OS tools, proving the agent↔OS contract (tool_call → tool_result threading) works on Grok.
//
// Bounded to ~28 live calls, spaced to respect rate limits. Exit non-zero on any hard failure.
import { readFileSync } from 'node:fs';
import { assemble } from '../../apps/anima/www/contextkit.js';
import { CLIENT_TOOLS, toOpenAITools, callOpenAIChat, runOpenAIToolLoop, extractJson, guardPlan, GROQ_MODELS } from '../../apps/agent/www/agent-tools.js';

/* ───────────── key + config ───────────── */
function loadKey() {
  if (process.env.GROQ_API_KEY) return { key: process.env.GROQ_API_KEY, base: 'https://api.groq.com/openai/v1', model: process.env.GROQ_MODEL || 'llama-3.1-8b-instant' };
  for (const p of ['deploy/sd/data/anima/teacher.json', 'tools/sd-sim/data/anima/teacher.json', 'deploy/sd-safe/data/anima/teacher.json']) {
    try { const o = JSON.parse(readFileSync(p, 'utf8')); if (o && o.key) return { key: o.key, base: o.base || 'https://api.groq.com/openai/v1', model: o.model || 'llama-3.1-8b-instant' }; } catch {}
  }
  return null;
}
const cfg = loadKey();
if (!cfg) { console.error('No Groq key found (set GROQ_API_KEY or deploy/sd/data/anima/teacher.json).'); process.exit(2); }
const CHAT_MODEL = cfg.model;                       // the configured chat model (8B by default)
const WORKER_MODEL = GROQ_MODELS.worker;            // the agent worker tier (70B)
const ORCH_MODEL = GROQ_MODELS.orchestrator;        // the orchestrator tier (8B)
console.log(`Groq live check — chat=${CHAT_MODEL}  worker=${WORKER_MODEL}  (key …${cfg.key.slice(-4)})\n`);

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let pass = 0, fail = 0; const fails = [];
function check(name, cond, detail) { if (cond) { pass++; console.log('  ✓ ' + name); } else { fail++; fails.push(name + (detail ? ' — ' + detail : '')); console.log('  ✗ ' + name + (detail ? ' — ' + detail : '')); } }

// One chat turn through the REAL ANIMA pipeline: contextkit assembles the budgeted, injection-safe
// transcript; we send it to Groq exactly as the browser cloudComplete does (system as a message).
async function chat(model, history, user, lang = 'it') {
  const asm = assemble({ history, user, mode: 'only', provider: 'openai', model, lang });
  const msg = await callOpenAIChat(fetch, cfg, {
    model, messages: [{ role: 'system', content: asm.system }, ...asm.messages],
    maxTokens: asm.maxTokens, temperature: asm.temperature,
  });
  await sleep(350);
  return (msg && msg.content || '').trim();
}

/* ───────────── A. CAPABILITY (single-call) ───────────── */
async function suiteA() {
  console.log('A · CAPABILITY (programming / writing / reasoning) — model ' + CHAT_MODEL);
  const t = async (name, user, assertFn, lang) => { try { const out = await chat(CHAT_MODEL, [], user, lang); check(name, assertFn(out), out.slice(0, 70).replace(/\n/g, '⏎')); } catch (e) { check(name, false, 'ERR ' + e.message); } };

  await t('js-function', 'Scrivimi una funzione JavaScript isPrime(n) che dica se n è primo.', (o) => /```/.test(o) && /function|=>/.test(o) && /return/.test(o));
  await t('js-game', 'Scrivimi un piccolo gioco snake in JavaScript per la console (console.log), tieni il codice in un blocco.', (o) => /```/.test(o) && o.length > 200 && /(snake|serpente|food|cibo|grid|board|griglia|for|while)/i.test(o));
  await t('regex', 'Dammi una regex JavaScript per validare un indirizzo email, dentro un blocco di codice.', (o) => /```/.test(o) && /@|\\w|\[a-z/i.test(o));
  await t('sql', 'Scrivi una query SQL che selezioni i 5 clienti con più ordini dalla tabella orders.', (o) => /select/i.test(o) && /(count|group by|order by|limit|top)/i.test(o));
  await t('racconto', 'Scrivimi un racconto breve di circa 5 frasi su un robot che scopre la musica.', (o) => o.length > 180 && /robot/i.test(o) && /(musica|suono|melod|canzo|nota)/i.test(o));
  await t('poesia', 'Scrivi una breve poesia di 4 versi sul mare.', (o) => o.length > 40 && o.split(/\n/).filter((l) => l.trim()).length >= 3 && /mar|ond|acqua|sale|blu/i.test(o));
  await t('spiegazione', 'Spiega in modo semplice cos\'è la ricorsione, con un piccolo esempio di codice.', (o) => /ricorsi/i.test(o) && /```|function|def |return/i.test(o));
  await t('math-word', 'Un treno percorre 240 km in 3 ore. Qual è la velocità media in km/h? Rispondi col numero.', (o) => /\b80\b/.test(o));
  await t('translate', 'Traduci in inglese: "buongiorno mondo". Dammi solo la traduzione.', (o) => /good\s*morning/i.test(o) && /world/i.test(o));
  await t('structured-json', 'Elenca 3 pianeti del sistema solare come array JSON di stringhe. Solo il JSON.', (o) => { const j = extractArray(o); return Array.isArray(j) && j.length >= 3; });
}
function extractArray(s) { const a = s.indexOf('['), b = s.lastIndexOf(']'); if (a < 0 || b <= a) return null; try { return JSON.parse(s.slice(a, b + 1)); } catch { return null; } }

/* ───────────── B. CONTEXT RETENTION (multi-turn) ───────────── */
// Each history embeds a fact the model CANNOT know otherwise; the follow-up forces it to use context.
async function suiteB() {
  console.log('\nB · CONTEXT RETENTION (answer derivable ONLY from prior turns) — model ' + CHAT_MODEL);
  const H = (pairs) => pairs.flatMap(([u, a]) => [{ role: 'user', text: u }, { role: 'bot', text: a }]);
  const t = async (name, hist, user, assertFn, lang) => { try { const out = await chat(CHAT_MODEL, hist, user, lang); check(name, assertFn(out), out.slice(0, 70).replace(/\n/g, '⏎')); } catch (e) { check(name, false, 'ERR ' + e.message); } };

  await t('name-recall', H([['Mi chiamo Marco e sto imparando a programmare.', 'Ciao Marco, ottimo!']]), 'Come mi chiamo?', (o) => /marco/i.test(o));
  await t('made-up-fact', H([['Il mio gatto si chiama Otto.', 'Che bel nome!']]), 'Come si chiama il mio gatto?', (o) => /otto/i.test(o));
  await t('calc-chain', H([['Quanto fa 45 per 54?', 'Fa 2430.']]), 'E quel risultato diviso 2 quanto fa?', (o) => /1215/.test(o));
  await t('fav-number', H([['Il mio numero preferito è 7.', 'Annotato.']]), 'Qual è il mio numero preferito?', (o) => /\b7\b/.test(o));
  await t('file-ref', H([['Ho creato un file che si chiama relazione.txt.', 'Ok.']]), 'Come si chiama il file che ho creato?', (o) => /relazione\.txt/i.test(o));
  await t('count-facts', H([['Ho 3 cani e 2 gatti.', 'Una bella famiglia!']]), 'Quanti animali ho in totale?', (o) => /\b5\b/.test(o));
  await t('job-recall', H([['Lavoro come medico in ospedale.', 'Capito.']]), 'Che lavoro faccio?', (o) => /medic/i.test(o));
  await t('project-name', H([['Il progetto a cui lavoro si chiama Nucleo.', 'Interessante.']]), 'Come si chiama il mio progetto?', (o) => /nucleo/i.test(o));
  await t('city-recall', H([['Vivo a Bologna da dieci anni.', 'Bella città!']]), 'In che città vivo?', (o) => /bologna/i.test(o));
  await t('lang-pref', H([['From now on, please always answer me in English.', 'Sure, I will answer in English.']]), 'Salutami e dimmi come stai.', (o) => /\b(hello|hi|how|i\s*am|i'm|fine|good|doing|are you|great)\b/i.test(o), 'en');
}

/* ───────────── C. MULTI-AGENT CONTRACT (real Groq) ───────────── */
const ORCH_SYS = `Sei l'ORCHESTRATORE di NucleoOS Agenti. Classifica la richiesta dell'utente e rispondi SOLO con JSON compatto:
{"mode":"answer"|"task"|"parallel","answer":"<se answer>","plan":"<1 frase>","hard":false,"subtasks":[{"title":"...","goal":"...","hard":false}]}
- "answer": SOLO chiacchiera/spiegazione a cui rispondi SUBITO senza strumenti.
- "task": un compito che usa strumenti (file, codice, device_status per ora/spazio/rete, weather, open_in_os per lanciare un'app).
- "parallel": 2-4 sottocompiti INDIPENDENTI.
In dubbio scegli "task".`;
async function orchestrate(user) {
  const msg = await callOpenAIChat(fetch, cfg, { model: ORCH_MODEL, messages: [{ role: 'system', content: ORCH_SYS }, { role: 'user', content: 'Richiesta: ' + user }], responseFormat: { type: 'json_object' }, maxTokens: 500, temperature: 0.1 });
  await sleep(350); return guardPlan(extractJson(msg.content), user);   // same deterministic guard the runtime applies — device/tool asks can't slip through as a fabricated "answer"
}
function mockOS() {
  const calls = []; const files = { 'a.txt': 'riga uno\nriga due\nriga tre' };
  const execTool = async (name, args) => {
    calls.push({ name, args: args || {} });
    switch (name) {
      case 'write_file': files[args.path] = String(args.content == null ? '' : args.content); return { content: '✔ scritto ' + args.path };
      case 'read_file': return files[args.path] != null ? { content: files[args.path] } : { content: 'Errore: not found', is_error: true };
      case 'list_files': return { content: Object.keys(files).join('\n') };
      case 'append_file': files[args.path] = (files[args.path] || '') + String(args.content || ''); return { content: '✔ aggiunto' };
      case 'device_status': return { content: JSON.stringify({ ora: '12:00', spazio_sd: '63 GB liberi', rete: 'STA · CasaWifi · 192.168.0.5', ram_libera_kb: 18 }) };
      case 'list_apps': return { content: 'calculator — Calcolatrice\nnotepad — Note\nmedia-player — Musica' };
      case 'open_in_os': return { content: '✔ avviato ' + (args.app || args.path) };
      case 'weather': return { content: JSON.stringify({ luogo: args.city, temperatura: '21°C', oggi_min_max: '14° / 23°' }) };
      default: return { content: 'ok' };
    }
  };
  return { execTool, calls, files };
}
async function worker(userMsg, sys) {
  const { execTool, calls, files } = mockOS();
  const oaTools = toOpenAITools(CLIENT_TOOLS);
  const messages = [{ role: 'system', content: sys }, { role: 'user', content: userMsg }];
  const callModel = (m) => callOpenAIChat(fetch, cfg, { model: WORKER_MODEL, messages: m, tools: oaTools, toolChoice: 'auto', maxTokens: 1024, temperature: 0.2 });
  const out = await runOpenAIToolLoop({ callModel, execTool, messages, maxSteps: 6 });
  await sleep(350); return { out, calls, files };
}
const WORKER_SYS = 'Sei un agente operativo di NucleoOS. Usa gli STRUMENTI per agire davvero (non descrivere a parole un\'azione che puoi compiere). Quando un tool restituisce un risultato, fidati di QUELLO e non inventare. Rispondi in italiano.';

async function suiteC() {
  console.log('\nC · MULTI-AGENT CONTRACT (Groq JSON orchestrator + tool-loop) — orch ' + ORCH_MODEL + ' / worker ' + WORKER_MODEL);
  // contract 1: orchestrator returns a valid typed plan
  try { const p = await orchestrate('Scrivimi una funzione fattoriale in python.'); check('orch-valid-json', !!(p && ['answer', 'task', 'parallel'].includes(p.mode)), JSON.stringify(p).slice(0, 70)); } catch (e) { check('orch-valid-json', false, 'ERR ' + e.message); }
  // contract 2: a device action must be classified as a TOOL task, not a direct answer
  try { const p = await orchestrate('Che ore sono sul dispositivo?'); check('orch-device-is-task', !!(p && p.mode === 'task'), JSON.stringify(p).slice(0, 70)); } catch (e) { check('orch-device-is-task', false, 'ERR ' + e.message); }
  // contract 3: small talk → direct answer
  try { const p = await orchestrate('Ciao, come stai oggi?'); check('orch-chitchat-is-answer', !!(p && p.mode === 'answer'), JSON.stringify(p).slice(0, 70)); } catch (e) { check('orch-chitchat-is-answer', false, 'ERR ' + e.message); }
  // contract 4: tool-loop actually CALLS write_file with the right args (agent↔OS contract)
  try { const r = await worker('Crea un file ciao.txt che contenga esattamente la parola: Hello', WORKER_SYS);
    check('toolloop-write', r.calls.some((c) => c.name === 'write_file' && /ciao\.txt/i.test(c.args.path || '') && /hello/i.test(c.args.content || '')), 'calls=' + r.calls.map((c) => c.name).join(',')); } catch (e) { check('toolloop-write', false, 'ERR ' + e.message); }
  // contract 5: tool-loop reads a file, threads the RESULT back, and answers from it
  try { const r = await worker('Leggi il file a.txt e dimmi quante righe contiene. Rispondi col numero.', WORKER_SYS);
    check('toolloop-read-then-answer', r.calls.some((c) => c.name === 'read_file') && /\b3\b/.test(r.out), 'out=' + r.out.slice(0, 50)); } catch (e) { check('toolloop-read-then-answer', false, 'ERR ' + e.message); }
  // contract 6: tool-loop uses device_status result instead of inventing
  try { const r = await worker('Quanta RAM libera ha il dispositivo adesso?', WORKER_SYS);
    check('toolloop-device-status', r.calls.some((c) => c.name === 'device_status') && /18/.test(r.out), 'out=' + r.out.slice(0, 50)); } catch (e) { check('toolloop-device-status', false, 'ERR ' + e.message); }
}

/* ───────────── run ───────────── */
(async () => {
  try { await suiteA(); await suiteB(); await suiteC(); }
  catch (e) { console.error('\nFATAL', e); process.exit(2); }
  console.log(`\nGrok live check: ${pass} passed, ${fail} failed (of ${pass + fail})`);
  if (fail) { console.log('FAILED:\n - ' + fails.join('\n - ')); process.exit(1); }
  console.log('all green ✓');
})();
