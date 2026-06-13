#!/usr/bin/env node
// quality-report.mjs — close the loop: turn the device's REAL offline telemetry into a prioritized
// improvement map. Fetches /data/anima/telemetry.ndjson from the device (read-only), then cross-references
// the LOCAL corpus (tools/anima/knowledge + knowledge.staged) to label each MISS as:
//   COVERAGE  — the topic/entity isn't in the corpus at all   -> ingest knowledge
//   ROUTING   — it IS in the corpus but the query missed it   -> NLU / phrasing / detector fix
//   RELATIONAL— "chi ha inventato/scritto/dipinto X" class     -> needs KG data + a detector
// Safe: no build, no deploy, no edits. Usage: node tools/anima-host/quality-report.mjs [host] [pin]
import { readFileSync, readdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const host = process.argv[2] || '192.168.0.166';
const pin = process.argv[3] || '689614';
const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..');

// --- 1. corpus token set (every alnum token >=4 chars across all card asks + replies, accent-folded) ---
const fold = s => s.normalize('NFD').replace(/\p{Diacritic}/gu, '').toLowerCase();
const corpus = new Set();
for (const dir of ['tools/anima/knowledge', 'tools/anima/knowledge.staged']) {
  let files = []; try { files = readdirSync(join(ROOT, dir)).filter(f => f.endsWith('.jsonl')); } catch {}
  for (const f of files) for (const ln of readFileSync(join(ROOT, dir, f), 'utf8').split('\n')) {
    if (!ln.trim() || ln.startsWith('//')) continue;
    let c; try { c = JSON.parse(ln); } catch { continue; }
    const blob = fold(JSON.stringify(c.ask || '') + ' ' + JSON.stringify(c.reply || '') + ' ' + (c.id || ''));
    for (const w of blob.match(/[a-z0-9]{4,}/g) || []) corpus.add(w);
  }
}

// --- 2. fetch telemetry ---
async function dev() {
  const ws = { cookie: '' };
  const pr = await fetch(`http://${host}/api/pair`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ pin }), signal: AbortSignal.timeout(12000) });
  ws.cookie = (pr.headers.get('set-cookie') || '').split(';')[0];
  const r = await fetch(`http://${host}/api/fs/read?path=/data/anima/telemetry.ndjson`, { headers: { cookie: ws.cookie }, signal: AbortSignal.timeout(12000) });
  return await r.text();
}

// --- 3. classify ---
const LEAD = /^(chi (e|era|sono)|cos(a sai( dir?mi)?( di| su| della| del)?| ?e|'e| ?è)|cosa mi sai dire di|come funziona|a cosa serve|quando (e|è|iniziò|fu|nacque|nato|nata)|qual ?(e|è)|da dove viene|di che (nazionalita|paese|nazione)|chi ha (scritto|inventato|dipinto|composto|fondato|diretto)|dove (si trova|e|è)|perche|come)\b/i;
const topicOf = q => fold(q).replace(LEAD, '').replace(/[?!.]/g, '').trim();
const pat = q => {
  const f = fold(q);
  if (/chi ha (scritto|inventato|dipinto|composto|diretto|fondato)/.test(f)) return 'relational';
  if (/^chi (e|era|sono)\b/.test(f) || /di che (nazionalita|paese|nazione)|da dove viene/.test(f)) return 'person/entity';
  if (/quando\b/.test(f)) return 'date/event';
  if (/\bpiu\b|\bpiù\b|più (grande|lungo|alto|profondo|piccolo)/.test(f)) return 'superlative';
  if (/^cos|come funziona|a cosa serve|cosa sai/.test(f)) return 'describe';
  return 'other';
};

(async () => {
  const fileArg = process.argv.find(a => a.startsWith('--file='));   // offline analysis on a saved telemetry dump
  let raw;
  if (fileArg) { raw = readFileSync(fileArg.slice(7), 'utf8'); }
  else { try { raw = await dev(); } catch (e) { console.error('device fetch failed (' + e.message + '); pass --file=<telemetry.ndjson> to analyze a saved dump'); process.exit(2); } }
  const rows = raw.split('\n').map(l => { try { return JSON.parse(l); } catch { return null; } }).filter(Boolean);
  const seen = new Set(); const miss = []; let answered = 0;
  for (const r of rows) {
    const q = (r.q || '').trim(); if (!q || seen.has(q)) continue; seen.add(q);
    if (r.tier && r.tier !== 'none' || (r.conf | 0) > 0 && r.intent) { answered++; continue; }
    const topic = topicOf(q);
    const toks = (topic.match(/[a-z0-9]{4,}/g) || []);
    const inCorpus = toks.length && toks.some(t => corpus.has(t));
    miss.push({ q, topic, pat: pat(q), kind: pat(q) === 'relational' ? 'RELATIONAL' : (inCorpus ? 'ROUTING' : 'COVERAGE') });
  }
  console.log(`\n=== ANIMA offline quality (device ${host}) — ${seen.size} distinct queries, ${answered} answered, ${miss.length} missed ===\n`);
  const byKind = {}; const byPat = {};
  for (const m of miss) { (byKind[m.kind] ??= []).push(m); (byPat[m.pat] ??= []).push(m); }
  console.log('MISSES BY ROOT CAUSE (where to invest):');
  for (const k of ['COVERAGE', 'RELATIONAL', 'ROUTING']) {
    const list = byKind[k] || []; if (!list.length) continue;
    console.log(`  ${k.padEnd(10)} ${String(list.length).padStart(3)}  e.g. ${list.slice(0, 4).map(m => `"${m.q}"`).join(', ')}`);
  }
  console.log('\nMISSES BY QUERY SHAPE:');
  for (const [p, list] of Object.entries(byPat).sort((a, b) => b[1].length - a[1].length))
    console.log(`  ${p.padEnd(14)} ${String(list.length).padStart(3)}`);
  console.log('\nINTERPRETATION: COVERAGE = ingest knowledge (the big lever). ROUTING = NLU/detector fix on data');
  console.log('that EXISTS (cheap win). RELATIONAL = add KG edges + a "chi ha <verb> X" detector.\n');
})();
