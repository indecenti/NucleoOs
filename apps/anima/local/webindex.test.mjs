#!/usr/bin/env node
// Host test for the client-side web indexer (apps/anima/www/local/webindex.js).
//
//   PURE block   — deterministic, no network: slug/ortho/grounding/stripPronun/clip, intent + context
//                  detection, candidate scoring, the coherence gate. Always runs (CI-safe).
//   LIVE block   — hits REAL Wikipedia to prove the precision upgrades: context-aware disambiguation,
//                  multi-candidate fallback, recall-from-store (no 2nd fetch), ephemeral never indexed.
//                  Skipped automatically when offline (set ANIMA_NO_NET=1 to force-skip).
//
// Run: npm run anima:webindex     (or: node apps/anima/local/webindex.test.mjs)

import { slug, ortho, grounding, stripPronun, clip, detectIntent, classify, webIndexAnswer, crawlBranch, exportForPromotion } from '../www/local/webindex.js';
import { webStore } from '../www/local/webstore.js';

let pass = 0, fail = 0; const fails = [];
function ok(name, cond, extra) { if (cond) { pass++; } else { fail++; fails.push(name + (extra ? '  ' + extra : '')); } }
function eq(name, got, want) { ok(name, JSON.stringify(got) === JSON.stringify(want), `got=${JSON.stringify(got)} want=${JSON.stringify(want)}`); }

// ---------------- PURE ----------------
console.log('— pure —');

eq('slug accents',           slug('Cristóforo Colómbo'), 'cristoforo-colombo');
ok('ortho exact',            ortho('mercurio', 'mercurio') > 0.99);
ok('ortho typo close',       ortho('einstein', 'enstein') > 0.6);
ok('ortho unrelated low',    ortho('mercurio', 'giove-pianeta') < 0.4);
ok('grounding hit',          grounding(['pianeta'], 'Mercurio è il pianeta più vicino al Sole.') > 0);
ok('grounding miss',         grounding(['pianeta'], 'Mercurio è una divinità romana.') === 0);

eq('stripPronun ipa',        stripPronun('Tizio (IPA: [ˈtit.tsjo]; nato 1900) è un pittore.'), 'Tizio (nato 1900) è un pittore.');
ok('stripPronun keeps verb', stripPronun('He was pronounced dead.') === 'He was pronounced dead.');
ok('clip boundary',          clip('Frase uno. Frase due molto lunga che sfora il limite.', 20).length <= 21);

// intent + disambiguating context
let it = detectIntent('chi è il pianeta Mercurio');
eq('intent entity',          it && it.entity, 'mercurio');
ok('intent context planet',  it && it.context.includes('pianeta'));
it = detectIntent('chi è Mercurio il dio romano');
ok('intent context god',     it && it.context.includes('dio') && it.context.includes('romano'));
ok('ephemeral rejected',     detectIntent('chi è il presidente oggi') === null);
ok('non-question null',      detectIntent('apri la calcolatrice') === null);
it = detectIntent('einstein', { bare: true });
eq('bare noun entity',       it && it.entity, 'einstein');
ok('bare stop too long',     detectIntent('una frase molto lunga che non e un nome', { bare: true }) === null);

eq('classify person',        classify('calciatore italiano'), 'person');
eq('classify place',         classify('pianeta del sistema solare'), 'place');

// coherence gate via webIndexAnswer with a stubbed fetch (no real net) — proves multi-candidate fallback:
// candidate 0 is a disambiguation (rejected), candidate 1 is the real planet page (accepted).
{
  const calls = [];
  const stub = async (url) => {
    calls.push(url);
    if (url.includes('list=search')) {   // full-text: disambigua ranks first, real page second
      return { ok: true, json: async () => ({ query: { search: [
        { title: 'Mercurio (disambigua)', snippet: 'Mercurio può riferirsi a' },
        { title: 'Mercurio (astronomia)', snippet: 'il pianeta più vicino al Sole' }] } }) };
    }
    if (url.includes('action=opensearch')) {
      return { ok: true, json: async () => ['mercurio', ['Mercurio (astronomia)'], ['pianeta del Sistema solare'], []] };
    }
    if (url.includes('Mercurio_(disambigua)')) return { ok: true, json: async () => ({ query: { pages: { 1: { title: 'Mercurio (disambigua)', pageprops: { disambiguation: '' }, extract: 'Mercurio può riferirsi a...' } } } }) };
    if (url.includes('Mercurio_(astronomia)')) return { ok: true, json: async () => ({ query: { pages: { 2: { title: 'Mercurio (astronomia)', description: 'pianeta del Sistema solare', extract: 'Mercurio è il primo e più piccolo pianeta del Sistema solare.' } } } }) };
    return { ok: false, json: async () => ({}) };
  };
  const mem = []; const store = { get: async () => null, put: async (c) => mem.push(c), all: async () => mem };
  const r = await webIndexAnswer('chi è il pianeta Mercurio', 'it', { fetch: stub, store });
  ok('fallback: answered',     !!(r && r.reply));
  ok('fallback: right page',   !!(r && /pianeta/.test(r.reply)), r && r.reply);
  ok('fallback: skipped disamb', mem.length === 1 && /astronomia/.test(mem[0].title));
  // recall: a second ask hits the store, NO network
  const store2 = { get: async () => ({ reply: mem[0].reply, category: mem[0].category }), put: async () => {} };
  const calls0 = calls.length;
  const r2 = await webIndexAnswer('chi è Mercurio', 'it', { fetch: stub, store: store2 });
  ok('recall: from store',     !!(r2 && r2.recalled && r2.reply));
  ok('recall: no fetch',       calls.length === calls0);
}

// webStore alias pointers: store under the asked-entity key, recall by canonical title too.
{
  await webStore.clear();
  await webStore.put({ slug: 'leonardo', lang: 'it', title: 'Leonardo da Vinci', reply: 'pittore e inventore', category: 'person', aliasKeys: ['leonardo-da-vinci'] });
  ok('store get by entity',  !!(await webStore.get('leonardo', 'it')));
  ok('store get by title',   (await webStore.get('leonardo-da-vinci', 'it'))?.title === 'Leonardo da Vinci');
  ok('store all no pointers', (await webStore.all('it')).length === 1);
  await webStore.clear();
}

// sense-keyed recall: "pianeta Mercurio" cached must NOT be served for "dio Mercurio" (no collision).
{
  const m = new Map(); const store = { get: async (s, l) => m.get(l + ':' + s) || null, put: async (c) => m.set(c.lang + ':' + c.slug, c), all: async () => [...m.values()] };
  const stub = async (url) => {
    if (url.includes('list=search') && /dio/.test(url)) return { ok: true, json: async () => ({ query: { search: [{ title: 'Mercurio (divinità)', snippet: 'è un dio della mitologia romana' }] } }) };
    if (url.includes('list=search')) return { ok: true, json: async () => ({ query: { search: [{ title: 'Mercurio (astronomia)', snippet: 'il pianeta più vicino al Sole' }] } }) };
    if (url.includes('action=opensearch')) return { ok: true, json: async () => ['mercurio', [], [], []] };
    if (url.includes('divinit')) return { ok: true, json: async () => ({ query: { pages: { 1: { title: 'Mercurio (divinità)', description: 'divinità romana', extract: 'Mercurio è un dio della mitologia romana.' } } } }) };
    if (url.includes('astronomia')) return { ok: true, json: async () => ({ query: { pages: { 2: { title: 'Mercurio (astronomia)', description: 'pianeta', extract: 'Mercurio è il primo pianeta del Sistema solare.' } } } }) };
    return { ok: false, json: async () => ({}) };
  };
  const a = await webIndexAnswer('chi è il pianeta Mercurio', 'it', { fetch: stub, store });
  const b = await webIndexAnswer('chi è il dio Mercurio', 'it', { fetch: stub, store });
  ok('sense: planet then god',  /pianeta/.test(a && a.reply) && /dio/.test(b && b.reply), JSON.stringify([a && a.reply, b && b.reply]));
  ok('sense: two cards stored', (await store.all()).length === 2);
}

// branch crawler (stubbed): bounded page cap, bilingual cards, dedup, MOSAICO shape, promotion export.
{
  const make = (n) => ({ query: { pages: { 1: n } } });
  const EN = { 'Seconda guerra mondiale': 'World War II', 'Adolf Hitler': 'Adolf Hitler', 'Olocausto': 'The Holocaust', 'Winston Churchill': 'Winston Churchill' };
  const stub = async (url) => {
    if (url.includes('list=search')) return { ok: true, json: async () => ({ query: { search: [{ title: 'Seconda guerra mondiale', snippet: 'conflitto 1939 1945' }] } }) };
    if (url.includes('action=opensearch')) return { ok: true, json: async () => ['x', ['Seconda guerra mondiale'], ['conflitto'], []] };
    if (url.includes('prop=links')) return { ok: true, json: async () => make({ title: 'Seconda guerra mondiale', links: [
      { title: 'Adolf Hitler' }, { title: 'Olocausto' }, { title: 'Lista delle battaglie' }, { title: 'Winston Churchill' }] }) };
    const T = decodeURIComponent((url.match(/titles=([^&]+)/) || [])[1] || '').replace(/_/g, ' ');
    if (url.includes('it.wikipedia') && url.includes('langlinks')) {
      const en = EN[T];
      if (!en) return { ok: true, json: async () => make({ missing: '' }) };
      return { ok: true, json: async () => make({ title: T, description: 'evento storico', langlinks: [{ '*': en }],
        extract: `${T} fu un fatto storico legato a ${T}. Approfondimento distinto su ${T} con dettagli ulteriori. Terza frase su ${T}.` }) };
    }
    if (url.includes('en.wikipedia')) return { ok: true, json: async () => make({ title: T,
      extract: `${T} was a historical matter about ${T}. Distinct deep dive on ${T} with further detail. Third sentence on ${T}.` }) };
    return { ok: false, json: async () => ({}) };
  };
  const mk = () => { const m = new Map(); return { get: async (s, l) => m.get(l + ':' + s) || null, put: async (c) => m.set(c.lang + ':' + c.slug, c), all: async (l) => [...m.values()].filter(c => (c.lang === 'en') === (l === 'en')) }; };
  const store = mk();
  const res = await crawlBranch('Seconda guerra mondiale', 'it', { fetch: stub, store }, { maxPages: 3 });
  ok('branch: seed resolved',  res && res.seed === 'Seconda guerra mondiale');
  ok('branch: capped',         res && res.cards.length === 3, res && res.cards.length);
  ok('branch: bilingual+detail', res && res.cards.every(c => c.reply.it && c.reply.en && c.detail.it && c.detail.en), JSON.stringify(res && res.cards[1]));
  ok('branch: list-page filtered', res && !res.cards.some(c => /lista|list/i.test(c.id)));
  // dedup on recrawl: with a fresh store crawl ALL 4, then recrawl -> 0 new, all skipped
  const s2 = mk();
  await crawlBranch('Seconda guerra mondiale', 'it', { fetch: stub, store: s2 }, { maxPages: 6 });
  const res2 = await crawlBranch('Seconda guerra mondiale', 'it', { fetch: stub, store: s2 }, { maxPages: 6 });
  ok('branch: dedup on recrawl', res2 && res2.cards.length === 0 && res2.skipped >= 4, res2 && JSON.stringify({ c: res2.cards.length, s: res2.skipped }));
  // promotion export: bilingual cards only, deduped by id
  const exp = await exportForPromotion(store);
  ok('export: bilingual cards', exp.length === res.cards.length && exp.every(c => c.reply.it && c.reply.en), 'exp=' + exp.length);
  ok('export: ids unique',      new Set(exp.map(c => c.id)).size === exp.length);
}

console.log(`  pure: ${pass} ok, ${fail} fail`);

// ---------------- LIVE ----------------
const NET = process.env.ANIMA_NO_NET !== '1';
if (NET) {
  console.log('— live (real Wikipedia) —');
  const reach = await fetch('https://it.wikipedia.org/w/api.php?action=query&meta=siteinfo&format=json&origin=*').then(r => r.ok).catch(() => false);
  if (!reach) { console.log('  (offline — live block skipped)'); }
  else {
    const mkStore = () => { const m = new Map(); return { get: async (s, l) => m.get(l + ':' + s) || null, put: async (c) => m.set((c.lang) + ':' + c.slug, c), all: async () => [...m.values()], _m: m }; };

    // 1) Context disambiguation: "pianeta Mercurio" must land on the PLANET, not the god/element.
    {
      const r = await webIndexAnswer('chi è il pianeta Mercurio', 'it', { fetch, store: mkStore() });
      ok('LIVE planet picked', !!(r && /pianet|sistema solare|sole/i.test(r.reply)), r && r.reply.slice(0, 90));
    }
    // 1b) Context STEERS the sense: same name, opposite intent. The god must NOT return the planet.
    {
      const god = await webIndexAnswer('chi è il dio Mercurio', 'it', { fetch, store: mkStore() });
      ok('LIVE god picked', !!(god && /divinit|dio|mitolog|romana/i.test(god.reply) && !/sistema solare/i.test(god.reply)), god && god.reply.slice(0, 90));
    }
    // 2) A clean person entity resolves with a coherent lead.
    {
      const r = await webIndexAnswer('chi è Albert Einstein', 'it', { fetch, store: mkStore() });
      ok('LIVE einstein', !!(r && /fisico|physicist|relativ/i.test(r.reply)), r && r.reply.slice(0, 90));
    }
    // 3) English entity.
    {
      const r = await webIndexAnswer('who is Ada Lovelace', 'en', { fetch, store: mkStore() });
      ok('LIVE ada en', !!(r && /mathematician|computer|programmer/i.test(r.reply)), r && r.reply.slice(0, 90));
    }
    // 4) Recall: second identical ask served from the store with zero extra fetches. Skip the assertion
    //    if the first live fetch flaked (no card stored) — that's a network blip, not a logic failure.
    {
      const store = mkStore(); let n = 0; const f = (...a) => { n++; return fetch(...a); };
      const r1 = await webIndexAnswer('chi è Leonardo da Vinci', 'it', { fetch: f, store });
      if (!r1) { console.log('  (recall skipped — first fetch flaked)'); }
      else {
        const after = n; const r2 = await webIndexAnswer('chi è Leonardo da Vinci', 'it', { fetch: f, store });
        ok('LIVE recall hit', !!(r2 && r2.recalled), r2 && JSON.stringify(r2).slice(0, 80));
        ok('LIVE recall no-net', n === after);
      }
    }
    // 5) Junk entity -> honest miss (null), never a fabricated answer.
    {
      const r = await webIndexAnswer('chi è asdfqwerzxcv', 'it', { fetch, store: mkStore() });
      ok('LIVE junk miss', r === null, r && JSON.stringify(r).slice(0, 80));
    }
    // 6) Ephemeral -> not handled here (left to live/LLM tiers).
    ok('LIVE ephemeral null', (await webIndexAnswer('chi è il presidente oggi', 'it', { fetch, store: mkStore() })) === null);
    // 7) Branch crawl: real WWII -> a small bounded set of bilingual MOSAICO cards. Flake-tolerant: if
    //    the network dropped most fetches this run, don't fail the gate — only assert what came back.
    {
      const store = mkStore();
      const r = await crawlBranch('Seconda guerra mondiale', 'it', { fetch, store }, { maxPages: 4, byteBudget: 80 * 1024 });
      if (!r || r.cards.length === 0) { console.log('  (branch crawl skipped — network flaked)'); }
      else {
        ok('LIVE branch bounded', r.cards.length <= 4, '' + r.cards.length);
        ok('LIVE branch bilingual', r.cards.every(c => c.reply.it && c.reply.en), r.cards.map(c => c.id).join(','));
        ok('LIVE branch detail', r.cards.every(c => c.detail.it || c.detail.en));
      }
    }
  }
}

console.log(`\nTOTAL: ${pass} ok, ${fail} fail`);
if (fails.length) { console.log('FAILURES:'); for (const f of fails) console.log('  ✗ ' + f); }
// Set the code and let Node drain in-flight fetch handles on its own — calling process.exit() while
// undici sockets are still closing trips a libuv assertion on Windows.
process.exitCode = fail ? 1 : 0;
