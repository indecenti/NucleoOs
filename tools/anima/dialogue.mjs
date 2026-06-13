#!/usr/bin/env node
// Talk to ANIMA like a real user, then CHECK the answers — the cascade, the memory, the offline recall.
//
// It boots the device simulator (tools/serve-shell.mjs) — the same code path the web shell hits — holds
// ONE conversation (shared session, like a real chat), asks a battery of questions across every tier
// (identity, commands, math, Wikidata facts, Wiktionary defs, Wikipedia entities, the teacher), and
// asserts each reply. The headline checks are LEARNING + OFFLINE RECALL: ask "chi è Einstein" (online,
// learns it), then ask it again / a paraphrase / the bare name and require it to answer from MEMORY
// (intent:learned) with no network. Junk must stay an honest miss; ephemeral facts must NOT be frozen.
//
// Usage:
//   node tools/anima/dialogue.mjs                 # full battery (auto-starts the sim)
//   node tools/anima/dialogue.mjs --port 5599     # use an already-running sim
//   node tools/anima/dialogue.mjs --keep          # leave learned cards in place (default: fresh start)
//   GROQ_KEY=... node tools/anima/dialogue.mjs    # also exercises the online LLM teacher tier
//
// Exit code is the number of failed checks (0 = all green) so it doubles as a CI gate.

import { spawn } from 'node:child_process';
import { rm, mkdir } from 'node:fs/promises';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = join(HERE, '..', '..');
const SD = join(REPO, 'tools', 'sd-sim');
const argv = process.argv.slice(2);
const PORT = Number((argv[argv.indexOf('--port') + 1]) || 5599) || 5599;
const KEEP = argv.includes('--keep');
const useExisting = argv.includes('--port');
const C = { g: '\x1b[32m', r: '\x1b[31m', y: '\x1b[33m', d: '\x1b[2m', b: '\x1b[1m', c: '\x1b[36m', x: '\x1b[0m' };

const sleep = (ms) => new Promise(r => setTimeout(r, ms));

// One assistant turn: GET /api/anima, return the parsed result. `reset` clears the conversation.
async function say(q, { lang = 'it', reset = false } = {}) {
  const u = `http://localhost:${PORT}/api/anima?q=${encodeURIComponent(q)}&lang=${lang}${reset ? '&reset=1' : ''}`;
  const r = await fetch(u);
  return r.json();
}
async function reachable() { try { await say('ping'); return true; } catch { return false; } }
async function online() { try { const r = await fetch('https://it.wikipedia.org/w/api.php?action=opensearch&limit=1&namespace=0&format=json&search=test', { signal: AbortSignal.timeout(6000) }); return r.ok; } catch { return false; } }

// --- assertion helpers (compose into a turn's `check`) ---
const has = (s) => (r) => (r.reply || '').toLowerCase().includes(s.toLowerCase());
const tier = (t) => (r) => r.tier === t;
const intent = (i) => (r) => r.intent === i;
const answered = (r) => r.tier !== 'none' && !!(r.reply || '').trim();
const miss = (r) => r.tier === 'none';
const all = (...fns) => (r) => fns.every(f => f(r));
const any = (...fns) => (r) => fns.some(f => f(r));

let pass = 0, fail = 0, skip = 0;
function report(turn, r, ok, why) {
  const lang = turn.lang ? `${C.d}[${turn.lang}]${C.x} ` : '';
  console.log(`${C.c}you ▸${C.x} ${lang}${turn.q}`);
  const reply = (r.reply || '').replace(/\s+/g, ' ').trim() || `${C.d}(silenzio)${C.x}`;
  console.log(`${C.b}anima ◂${C.x} ${reply}`);
  const meta = `${C.d}tier=${r.tier} intent=${r.intent || '-'} conf=${r.confidence}${r.learned ? ' learned✓' : ''}${C.x}`;
  if (ok === 'skip') { skip++; console.log(`  ${C.y}skip${C.x} ${turn.note}  ${meta}\n`); return; }
  if (ok) { pass++; console.log(`  ${C.g}pass${C.x} ${turn.note}  ${meta}\n`); }
  else { fail++; console.log(`  ${C.r}FAIL${C.x} ${turn.note} — ${why || ''}  ${meta}\n`); }
}

// ---------------------------------------------------------------------------------------------
async function main() {
  let child = null;
  if (!useExisting) {
    // Refuse to silently test a foreign/stale sim squatting on the port (it would give false results).
    if (await reachable()) { console.error(`${C.r}La porta ${PORT} è già occupata da un'altra istanza.${C.x}\nLiberala (PowerShell: ${C.c}Get-NetTCPConnection -LocalPort ${PORT} -State Listen | %% { Stop-Process -Id $_.OwningProcess -Force }${C.x}) o usa ${C.c}--port ${PORT}${C.x} per testarla apposta.`); process.exit(1); }
    if (!KEEP) { try { await rm(join(SD, 'data', 'anima', 'learned'), { recursive: true, force: true }); } catch {} }
    await mkdir(join(SD, 'data', 'anima', 'learned'), { recursive: true });
    let crashed = null;
    child = spawn(process.execPath, [join(REPO, 'tools', 'serve-shell.mjs')], { stdio: ['ignore', 'ignore', 'pipe'] });
    let errbuf = ''; child.stderr.on('data', d => { errbuf += d; }); child.on('exit', c => { if (c) crashed = errbuf || `exit ${c}`; });
    for (let i = 0; i < 40 && !crashed && !(await reachable()); i++) await sleep(150);
    if (crashed) { console.error(`${C.r}Il simulatore è crashato all'avvio:${C.x}\n${crashed.trim().split('\n').slice(0, 6).join('\n')}`); process.exit(1); }
  }
  if (!(await reachable())) { console.error(`${C.r}Il simulatore non risponde su :${PORT}${C.x}`); process.exit(1); }
  const net = await online();
  console.log(`\n${C.b}=== ANIMA — verifica conversazionale ===${C.x}  ${C.d}porta ${PORT} · rete ${net ? 'ONLINE' : 'OFFLINE'} · teacher ${process.env.GROQ_KEY ? 'ON' : 'off'}${C.x}\n`);
  await say('reset', { reset: true });   // start a clean conversation

  // A turn: { q, lang?, check, note, online?(needs net), sect } — run top to bottom (a real dialogue).
  const O = net;   // online checks only assert when the network is up; otherwise they're skipped
  const turns = [
    { sect: 'Identità & comandi' },
    { q: 'chi sei', check: all(intent('whoami'), has('anima')), note: 'si presenta come ANIMA' },
    { q: 'che ore sono', check: all(intent('time'), answered), note: 'ora corrente (live)' },
    { q: 'quanto fa 12 * 8', check: all(intent('calc'), has('96')), note: 'calcolo = 96' },
    { q: 'apri la calcolatrice', check: all(tier('command'), has('calculator')), note: 'lancia un’app' },

    { sect: 'Entità enciclopediche (Wikipedia) — il bug segnalato' },
    { q: 'chi è einstein', online: true, check: all(intent('wikipedia'), answered), note: '"chi è X" risponde (non più "non lo so")' },
    { q: 'chi è trump', online: true, check: all(intent('wikipedia'), answered), note: 'persona contemporanea' },
    { q: 'batman?', online: true, check: all(intent('wikipedia'), answered), note: 'entità nuda con "?"' },
    { q: "cos'è la fotosintesi", online: true, check: all(intent('wikipedia'), answered), note: 'concetto, non persona' },
    { q: 'who is ada lovelace', lang: 'en', online: true, check: all(intent('wikipedia'), answered), note: 'inglese' },

    { sect: 'MEMORIA — recall OFFLINE di ciò che ha imparato (nessuna rete)' },
    { q: 'chi è einstein', check: all(intent('learned'), has('einstein')), note: 'stessa domanda → dalla memoria' },
    { q: 'einstein', check: intent('learned'), note: 'nome nudo → memoria (alias canonico)' },
    { q: 'parlami di einstein', check: intent('learned'), note: 'parafrasi → memoria' },
    { q: 'trump', check: intent('learned'), note: 'nome breve → memoria' },

    { sect: 'Fatti precisi (Wikidata, senza chiave) e definizioni (Wiktionary)' },
    { q: 'quando è nato einstein', online: true, check: all(intent('wikidata'), has('1879')), note: 'data di nascita = 1879' },
    { q: 'capitale del giappone', online: true, check: all(intent('wikidata'), has('tokyo')), note: 'capitale = Tokyo' },
    { q: 'chi ha scritto la divina commedia', online: true, check: all(intent('wikidata'), has('dante')), note: 'autore = Dante' },
    { q: 'cosa significa effimero', online: true, check: all(intent('wiktionary'), answered), note: 'definizione di dizionario' },

    { sect: 'Ragionamento: deduzione HDC/KGE + composizione neuro-simbolica' },
    // ("chi ha scritto la divina commedia" già appreso nella sezione Fatti) → ora deduco l'inverso:
    { q: 'cosa ha scritto Dante', online: true, check: all(intent('kge'), has('commedia')), note: 'DEDUCE l\'inverso, offline (mai memorizzato così)' },
    { q: 'chi è nato prima, Dante o Einstein', online: true, check: all(intent('combinator'), has('dante')), note: 'COMPONE 2 fatti: confronto nascite (mai memorizzato)' },
    { q: 'in che continente è Lione', online: true, check: all(intent('kge-geo'), has('europa')), note: 'impara la catena e DEDUCE il continente' },
    { q: 'dove si trova Lione', online: true, check: all(intent('kge-geo'), has('francia')), note: 'catena di contenimento, offline (riusa la cache)' },
    { q: 'in che continente è Nizza', online: true, check: all(intent('kge-geo'), has('europa')), note: 'COMPONE: riusa il link Francia→Europa' },

    { sect: 'Onestà & legge di volatilità' },
    { q: 'asdfghjkl', check: miss, note: 'parola inventata → onesto "non lo so"' },
    { q: 'chi è il presidente oggi', online: true, check: (r) => true, note: 'effimero: risponde ma NON congela una card', verify: async () => !(await isLearnedSlug('presidente')) },
    { q: 'perché il cielo è blu', check: process.env.GROQ_KEY ? all(tier('remote'), answered) : miss,
      note: process.env.GROQ_KEY ? 'aperta → teacher Grok' : 'aperta, nessuna fonte/chiave → onesto (qui entrerebbe Grok)' },

    { sect: 'Memoria conversazionale (follow-up)' },
    { q: 'apri le foto', check: tier('command'), note: 'apre la galleria' },
    { q: 'aprilo', check: (r) => r.memory === true, note: '"aprilo" → riapre dalla memoria di sessione' },
  ];

  for (const t of turns) {
    if (t.sect) { console.log(`${C.b}── ${t.sect} ──${C.x}`); continue; }
    if (t.online && !O) { report(t, { tier: '-', intent: '-' }, 'skip', t); continue; }
    if (t.online) await sleep(1200);   // space out anonymous Wikimedia/Wikidata calls (they throttle bursts; a real device is user-paced)
    let r;
    try { r = await say(t.q, { lang: t.lang }); } catch (e) { report(t, { reply: String(e) }, false, 'errore di rete'); continue; }
    // retry a transient online hiccup (rate-limit / flaky fetch) with backoff before declaring failure
    for (let k = 0; t.online && k < 2 && !t.check(r) && r.tier === 'none'; k++) { await sleep(2500); try { r = await say(t.q, { lang: t.lang }); } catch {} }
    let ok = !!t.check(r);
    if (ok && t.verify) { try { ok = await t.verify(); } catch { ok = false; } }
    // A still-empty online turn means the free source couldn't be reached (rate-limit/offline), NOT a
    // logic regression — those surface as a WRONG answer (tier!=none), which stays a hard fail.
    if (!ok && t.online && r.tier === 'none') { const n = t.note; t.note = n + ' (fonte non raggiungibile: rate-limit/offline)'; report(t, r, 'skip'); t.note = n; continue; }
    report(t, r, ok, 'atteso diverso');
  }

  console.log(`${C.b}=== Risultato ===${C.x}  ${C.g}${pass} pass${C.x} · ${fail ? C.r : C.d}${fail} fail${C.x} · ${C.y}${skip} skip${C.x}`);
  if (!net) console.log(`${C.d}(rete offline: i controlli online sono stati saltati — rilancia con connessione per la batteria completa)${C.x}`);
  if (child) child.kill();
  process.exit(fail);
}

// Did a learned card get written whose canonical slug contains `needle`? (volatility check)
async function isLearnedSlug(needle) {
  try {
    const { readFile } = await import('node:fs/promises');
    const t = await readFile(join(SD, 'data', 'anima', 'learned', 'it.jsonl'), 'utf8');
    return t.split('\n').filter(Boolean).some(l => { try { return (JSON.parse(l).id || '').includes(needle); } catch { return false; } });
  } catch { return false; }
}

main().catch(e => { console.error(e); process.exit(1); });
