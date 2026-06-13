#!/usr/bin/env node
// ANIMA BULK importer — useful Wikipedia BRANCHES at scale, grounded & bilingual. Walks curated category
// trees (IT Wikipedia) breadth-first to a depth, pulls each article's LEAD extract in BATCHES (prop=extracts,
// exintro — up to 20/call), pairs an EN extract via langlinks, and emits a grounded card (reply = the
// article's own lead, clipped; never generated -> cannot hallucinate). Sub-sharded by branch so AKB5 stays
// fast. RESUMABLE: appends to <branch>.jsonl, skips QIDs already on disk -> safe to run repeatedly / in bg.
//
//   node tools/anima/import_wiki_bulk.mjs [--depth 2] [--cap 4000] [--only informatica,fisica]
import { appendFileSync, readFileSync, readdirSync, existsSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
const here = dirname(fileURLToPath(import.meta.url));
const OUTDIR = join(here, 'knowledge.staged', 'wiki');
const UA = 'NucleoOS-ANIMA-Import/1.0 (offline edge assistant; useful knowledge branches; CC-BY-SA Wikipedia)';
const arg = (k, d) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : d; };
const DEPTH = +arg('--depth', 2);          // category recursion depth (0 = direct members only)
const CAP = +arg('--cap', 4000);           // max NEW articles fetched per branch this run
const ONLY = (arg('--only', '') || '').split(',').filter(Boolean);

// Curated USEFUL branches: [branch shard name, IT root category]. Tech-heavy (NucleoOS/ANIMA) + science +
// the human-useful (medicine/first-aid/geography/history). Deliberately NO gossip/entertainment trees.
const BRANCHES = [
  ['informatica', 'Informatica'], ['programmazione', 'Linguaggi di programmazione'],
  ['algoritmi', 'Algoritmi'], ['strutture-dati', 'Strutture dati'], ['crittografia', 'Crittografia'],
  ['reti', 'Reti di computer'], ['web', 'World Wide Web'], ['elettronica', 'Elettronica'],
  ['embedded', 'Sistemi embedded'], ['sicurezza', 'Sicurezza informatica'],
  ['fisica', 'Fisica'], ['chimica', 'Chimica'], ['biologia', 'Biologia'], ['matematica', 'Matematica'],
  ['astronomia', 'Astronomia'], ['medicina', 'Medicina'], ['anatomia', 'Anatomia umana'],
  ['primo-soccorso', 'Primo soccorso'], ['nutrizione', 'Alimentazione'],
  ['geografia', 'Geografia'], ['storia', 'Storia'], ['filosofia', 'Filosofia'], ['tecnologia', 'Tecnologia'],
];

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const slug = (s) => s.normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');
const clip = (t) => { t = (t || '').replace(/\s+/g, ' ').trim(); if (t.length <= 248) return t; const cut = t.slice(0, 248); const d = cut.lastIndexOf('. '); return d > 120 ? cut.slice(0, d + 1) : cut.slice(0, 245) + '…'; };
// reject junk titles (lists, meta, dates, disambiguation, stubs by name)
const JUNK = /^(Lista |Elenco |Categoria:|Template:|Aiuto:|Wikipedia:|Portale:|File:|Progetto:)|\(disambigua\)|^\d{1,4}$|^\d{1,2} (gennaio|febbraio|marzo|aprile|maggio|giugno|luglio|agosto|settembre|ottobre|novembre|dicembre)/i;

async function api(lang, params) {
  const u = `https://${lang}.wikipedia.org/w/api.php?` + new URLSearchParams({ format: 'json', formatversion: '2', ...params });
  for (let a = 0; a < 4; a++) {
    try {
      const r = await fetch(u, { headers: { 'User-Agent': UA } });
      if (r.status === 429) { await sleep(2000 * (a + 1)); continue; }
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return await r.json();
    } catch (e) { if (a === 3) throw e; await sleep(1000 * (a + 1)); }
  }
  return null;
}

// (streaming importer below fuses the category walk with the extract fetch — no separate walk pass)

// Batched IT extracts (+ pageprops wikibase id + langlink EN title) for up to 20 titles/call.
async function itBatch(titles) {
  const j = await api('it', { action: 'query', prop: 'extracts|pageprops|langlinks', titles: titles.join('|'),
    exintro: '1', explaintext: '1', exlimit: '20', exchars: '500', ppprop: 'wikibase_item', lllang: 'en', lllimit: '1' });
  const out = [];
  for (const p of (j?.query?.pages || [])) {
    if (!p.extract || p.missing) continue;
    out.push({ title: p.title, it: clip(p.extract), qid: p.pageprops?.wikibase_item || null, enTitle: p.langlinks?.[0]?.title || null });
  }
  return out;
}
async function enBatch(titles) {
  if (!titles.length) return {};
  const j = await api('en', { action: 'query', prop: 'extracts', titles: titles.join('|'), exintro: '1', explaintext: '1', exlimit: '20', exchars: '500' });
  const m = {};
  for (const p of (j?.query?.pages || [])) if (p.extract && !p.missing) m[p.title] = clip(p.extract);
  return m;
}

// cross-corpus ask dedup (curated + already-imported) + per-branch QID resume
const norm = (s) => s.toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/[^a-z0-9 ]/g, '').replace(/\s+/g, ' ').trim();
const claimed = new Set(), seenQid = new Set();
const kdir = join(here, 'knowledge');
for (const f of readdirSync(kdir)) { if (!f.endsWith('.jsonl')) continue;
  for (const l of readFileSync(join(kdir, f), 'utf8').split('\n')) { if (!l.trim() || l.startsWith('//')) continue;
    try { const c = JSON.parse(l); for (const a of (c.ask?.it || [])) claimed.add(norm(a)); } catch {} } }
if (!existsSync(OUTDIR)) mkdirSync(OUTDIR, { recursive: true });
for (const f of readdirSync(OUTDIR)) { if (!f.endsWith('.jsonl')) continue;
  for (const l of readFileSync(join(OUTDIR, f), 'utf8').split('\n')) { if (!l.trim()) continue;
    try { const c = JSON.parse(l); const q = (c.source || '').replace('wikipedia-wd:', ''); if (q) seenQid.add(q);
      for (const a of (c.ask?.it || [])) claimed.add(norm(a)); } catch {} } }

// Emit one batch of <=20 article titles: fetch IT extracts + EN via langlink, write grounded cards. Returns #added.
async function emitChunk(branch, out, chunk) {
  let rows; try { rows = await itBatch(chunk); } catch { return 0; }
  rows = rows.filter((r) => r.qid && !seenQid.has(r.qid) && r.it && r.it.length > 60);
  if (!rows.length) return 0;
  const enMap = await enBatch(rows.map((r) => r.enTitle).filter(Boolean));
  const lines = [];
  for (const r of rows) {
    seenQid.add(r.qid);
    const lbl = r.title;
    const askIt = [`cos'è ${lbl}`, `che cos'è ${lbl}`, `cosa significa ${lbl}`, `spiegami ${lbl}`, `definizione di ${lbl}`, `${lbl}`];
    if (askIt.some((a) => claimed.has(norm(a)))) continue;
    askIt.forEach((a) => claimed.add(norm(a)));
    const en = (r.enTitle && enMap[r.enTitle]) || r.it;
    const askEn = r.enTitle ? [`what is ${r.enTitle}`, `explain ${r.enTitle}`, `${r.enTitle}`] : [];
    lines.push(JSON.stringify({ id: `wiki.${branch}.${slug(lbl)}`, category: 'wiki-' + branch, action: 'answer', arg: '',
      reply: { it: r.it, en }, ask: { it: askIt, en: askEn }, source: `wikipedia-wd:${r.qid}`, lang_primary: 'bi', tags: ['wiki', branch] }));
  }
  if (lines.length) appendFileSync(out, lines.join('\n') + '\n');
  return lines.length;
}

// STREAMING per-branch: BFS the category tree and fetch+emit extracts AS titles are discovered (cards from
// the first API call), stopping at CAP. Far faster than walk-all-then-fetch on huge trees.
async function importBranch(branch, root) {
  const out = join(OUTDIR, branch + '.jsonl');
  const seenTitle = new Set(), seenCat = new Set([root]);
  let frontier = [root], buf = [], added = 0;
  for (let d = 0; d <= DEPTH && frontier.length && added < CAP; d++) {
    const next = [];
    for (const cat of frontier) {
      if (added >= CAP) break;
      let cont;
      do {
        let j; try { j = await api('it', { action: 'query', list: 'categorymembers', cmtitle: 'Categoria:' + cat,
          cmlimit: '500', cmtype: 'page|subcat', ...(cont ? { cmcontinue: cont } : {}) }); } catch { break; }
        if (!j) break;
        for (const m of (j.query?.categorymembers || [])) {
          if (m.ns === 14) { const c = m.title.replace(/^Categoria:/, ''); if (!seenCat.has(c)) { seenCat.add(c); next.push(c); } }
          else if (m.ns === 0 && !JUNK.test(m.title) && !seenTitle.has(m.title)) {
            seenTitle.add(m.title); buf.push(m.title);
            if (buf.length >= 20) { added += await emitChunk(branch, out, buf.splice(0, 20)); process.stderr.write(`  ${branch}: +${added}\r`); }
          }
        }
        cont = j.continue?.cmcontinue;
        await sleep(60);
      } while (cont && added < CAP);
    }
    frontier = next;
  }
  while (buf.length && added < CAP) added += await emitChunk(branch, out, buf.splice(0, 20));
  return added;
}

let grand = 0;
for (const [branch, root] of BRANCHES) {
  if (ONLY.length && !ONLY.includes(branch)) continue;
  process.stderr.write(`\n[wiki:${branch}] «${root}» (depth ${DEPTH}, cap ${CAP}) …\n`);
  let added = 0; try { added = await importBranch(branch, root); } catch (e) { process.stderr.write(`  ERR ${e.message}\n`); }
  grand += added;
  process.stderr.write(`  [wiki:${branch}] +${added} cards\n`);
}
console.log(`[wiki-bulk] +${grand} new grounded cards across ${BRANCHES.length} branches -> ${OUTDIR}`);
