#!/usr/bin/env node
// ANIMA generic entity-question grammar — the host-runnable MIRROR of the C extractor in
// nucleo_anima_online.c (nucleo_anima_online_entity). It proves the ONE thing that matters here:
// recognition is GENERIC — ANIMA pulls the entity out of ANY "asking about a named thing" phrasing,
// for ANY name (even invented ones), so the device can then look it up / fetch / learn / persist.
//
// The PREFIXES/SUFFIXES/ARTICLES lists below are the canonical source; keep TRIG_IT/TRIG_EN/ARTICLES
// in nucleo_anima_online.c in sync with them (same strings). Run:  node tools/anima/entity.mjs
//
// Design: a question is "<wrapper> <ENTITY>" (Italian) or "<wrapper> <ENTITY> [<tail>]" (English).
// Strip the LONGEST matching wrapper prefix, a leading article, an English tail ("famous for"/"do"),
// and trailing punctuation. What remains is the entity — works for one person or a hundred or none
// of a fixed list, because nothing here enumerates names.

// Italian wrappers (the entity follows). Accented and bare spellings both, since users type either.
export const PREFIX_IT = [
  // identity
  "chi e ","chi è ","chi era ","chi erano ","chi sono ","sai chi e ","sai chi è ","sai chi era ",
  "cos'e ","cos'è ","cosa e ","cosa è ","che cos'e ","che cos'è ","che cosa e ","che cosa è ","cosa significa ",
  // familiarity / recall
  "conosci ","conoscete ","conosce ","hai mai sentito parlare di ","hai mai sentito ","hai sentito parlare di ","hai sentito ",
  "ti e familiare ","ti è familiare ","ti suona ","hai presente ","ti viene in mente ","ti ricordi ","ricordi ",
  "eri a conoscenza di ","sei a conoscenza di ","hai mai incontrato il nome ","hai incontrato il nome ",
  "ti e mai capitato di leggere su ","ti è mai capitato di leggere su ","ti e capitato di leggere su ","ti è capitato di leggere su ","riesci a riconoscere ","riconosci ",
  // tell-me
  "parlami di ","potresti parlarmi di ","puoi parlarmi di ","raccontami di ","raccontami qualcosa su ","dimmi di ",
  "dimmi qualcosa su ","potresti dirmi qualcosa su ","puoi dirmi qualcosa su ","dimmi chi e ","dimmi chi è ",
  "cosa sai di ","cosa sai dire di ","che sai di ","che cosa sai di ","sai qualcosa su ","sai qualcosa riguardo a ","sai qualcosa di ","sai dirmi di ",
  "hai qualche informazione su ","hai informazioni su ","hai qualche nozione su ","hai notizie su ","qual e la tua conoscenza di ","qual è la tua conoscenza di ",
  // fame / role / field
  "per cosa e famoso ","per cosa è famoso ","per cosa e famosa ","per cosa è famosa ","per cosa e noto ","per cosa è noto ","per cosa e nota ","per cosa è nota ",
  "che cosa ha fatto ","cosa ha fatto ","che ha fatto ","in che ambito e noto ","in che ambito è noto ","in che ambito e nota ","in che ambito è nota ","in che campo e noto ","in che campo è noto ",
  "qual e il ruolo di ","qual è il ruolo di ","dove e conosciuto ","dove è conosciuto ","dove e conosciuta ","dove è conosciuta ",
  "per quale motivo e importante ","per quale motivo è importante ","perche e importante ","perché è importante ",
  "di quale campo e esperto ","di quale campo è esperto ","di quale campo e esperta ","di quale campo è esperta ","qual e la specialita di ","qual è la specialità di ",
  "chi rappresenta ","di che si occupa ","di cosa si occupa ","che lavoro fa ","che mestiere fa ",
  // occupation in the IMPERFECT (past) and ORIGIN — natural phrasings the present-tense triggers missed
  "di cosa si occupava ","di che si occupava ","che lavoro faceva ","che mestiere faceva ","che lavoro svolgeva ",
  "di dove e ","di dov'e ","di dov'è ","di dove è ","da dove viene ","da dove proviene ","di che nazionalita e ","di che nazionalità è ",
  "di che attore e ","di che attore è ","di che sportivo e ","di che sportivo è ","di che politico e ","di che politico è ",
  "di che cantante e ","di che cantante è ","di che scrittore e ","di che scrittore è ","di che artista e ","di che artista è ","di che musicista e ","di che musicista è ",
];

// English wrappers. Some put the entity in the MIDDLE ("what is X famous for") -> see SUFFIX_EN.
export const PREFIX_EN = [
  "do you know ","have you ever heard of ","have you heard of ","have you heard about ","are you familiar with ",
  "what can you tell me about ","what do you know about ","tell me about ","tell me who ","were you aware of ","ever heard of ","do you recognize ","do you recall ",
  "who is ","who was ","who are ","who's ","what is ","what was ","what are ","what's ","what did ",
];
// English tails that follow the entity; stripped from the residue so "what is X famous for" -> X.
export const SUFFIX_EN = [   // longest-first so "do for a living" wins over "do"
  " do for a living"," best known for"," famous for"," known for"," does"," about"," do",
];
// Leading articles to strip from the extracted entity.
export const ARTICLES = ["the ","a ","an ","il ","lo ","la ","i ","gli ","le ","l'","un ","uno ","una "];

const stripPunct = s => s.replace(/[\s?.!,;:]+$/,"").replace(/^[\s]+/,"");

export function extractEntity(input) {
  const low = input.toLowerCase().trim();
  // longest matching wrapper across BOTH languages (ANIMA understands IT+EN regardless of reply lang)
  let best = -1;
  for (const p of [...PREFIX_IT, ...PREFIX_EN]) if (low.startsWith(p) && p.length > best) best = p.length;
  if (best < 0) return null;
  let e = stripPunct(low.slice(best).trimStart());                 // drop trailing ?/. first…
  for (const a of ARTICLES) if (e.startsWith(a)) { e = e.slice(a.length).trimStart(); break; }
  for (const s of SUFFIX_EN) if (e.endsWith(s)) { e = stripPunct(e.slice(0, -s.length)); break; }  // …so the EN tail strips
  const slug = e.normalize("NFD").replace(/[̀-ͯ]/g,"").replace(/[^a-z0-9]+/g,"-").replace(/^-|-$/g,"");
  if (slug.length < 2) return null;
  return { entity: e, slug };
}

// ---- self-test: the user's real phrasing patterns, each with ARBITRARY names (incl. invented) ----
if (import.meta.url === `file://${process.argv[1]}`.replace(/\\/g,"/") || process.argv[1]?.endsWith("entity.mjs")) {
  const NAMES = ["Madonna","Michael Jackson","Freddie Mercury","Leonardo DiCaprio","Martin Luther King",
                 "Gabriel García Márquez","John F. Kennedy","Cristiano Ronaldo","Pinco Pallino","Zxcv Qwerty"];
  // template -> expected slug fragment (the name slug). {n} is the name.
  const TEMPLATES = [
    "Conosci {n}?","Sai chi è {n}?","Chi è {n}?","Hai mai sentito {n}?","Ti è mai capitato di leggere su {n}?",
    "Riesci a riconoscere {n}?","Eri a conoscenza di {n}?","Ti è familiare {n}?","Hai qualche informazione su {n}?",
    "Potresti dirmi qualcosa su {n}?","Cosa sai di {n}?","Qual è la tua conoscenza di {n}?","Ti ricordi {n}?",
    "Hai mai incontrato il nome {n}?","Sai qualcosa riguardo a {n}?","Cosa sai dire di {n}?","Potresti parlarmi di {n}?",
    "Hai qualche nozione su {n}?","Ti viene in mente {n}?","Sai chi era {n}?","Chi rappresenta {n}?","Di che attore è {n}?",
    "Qual è il ruolo di {n}?","Per cosa è famoso {n}?","Che cosa ha fatto {n}?","In che ambito è noto {n}?",
    "Dove è conosciuto {n}?","Per quale motivo è importante {n}?","Di quale campo è esperto {n}?","Qual è la specialità di {n}?",
    "Di cosa si occupava {n}?","Che lavoro faceva {n}?","Che mestiere faceva {n}?","Di dov'è {n}?","Da dove viene {n}?","Di che nazionalità è {n}?",
    "Do you know {n}?","Who is {n}?","Have you heard of {n}?","What do you know about {n}?","Tell me about {n}?",
    "What is {n} famous for?","What did {n} do?","Are you familiar with {n}?",
  ];
  const nameSlug = n => n.normalize("NFD").replace(/[̀-ͯ]/g,"").toLowerCase().replace(/[^a-z0-9]+/g,"-").replace(/^-|-$/g,"");
  let pass = 0, fail = 0; const misses = [];
  for (const t of TEMPLATES) for (const n of NAMES) {
    const q = t.replace("{n}", n);
    const r = extractEntity(q);
    const want = nameSlug(n);
    if (r && r.slug === want) pass++;
    else { fail++; misses.push(`${q}  ->  ${r ? r.slug : "(not recognized)"}  (want ${want})`); }
  }
  const total = pass + fail;
  console.log(`[entity] generic extraction: ${pass}/${total} (${(100*pass/total).toFixed(1)}%) over ${TEMPLATES.length} patterns × ${NAMES.length} names`);
  // Out-of-scope: a non-entity question must NOT be mistaken for one (these have no name to extract).
  const oos = ["quanto fa 12 per 8","che ore sono","apri le foto","crea un file note.txt","raccontami una barzelletta"];
  let fp = 0; for (const q of oos) { const r = extractEntity(q); if (r) { fp++; console.log(`  [FALSE+] "${q}" -> ${r.slug}`); } }
  console.log(`  out-of-scope false-positives: ${fp}/${oos.length}`);
  if (misses.length) { console.log("  misses (first 15):"); misses.slice(0,15).forEach(m => console.log("    "+m)); }
  process.exit(fail || fp ? 1 : 0);
}
