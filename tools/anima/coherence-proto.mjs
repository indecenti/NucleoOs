#!/usr/bin/env node
// SC3 — Self-Calibrated Coherence Cross : math prototype / proof.
//
// Goal: decide ACCEPT (learn) vs REJECT (don't poison the ODD) for a fetched resolution
// query -> (title, extract), WITHOUT a hand-tuned similarity threshold, with ASYMMETRIC risk
// (a false learn is far worse than a false reject), covering BOTH orthographic (typo/name) and
// semantic (description) legitimacy. This file proves the kernel on real cases incl. the live bug.
//
// Run: node tools/anima/coherence-proto.mjs

const C = { g:'\x1b[32m', r:'\x1b[31m', y:'\x1b[33m', d:'\x1b[2m', b:'\x1b[1m', c:'\x1b[36m', x:'\x1b[0m' };

// ---- normalization ---------------------------------------------------------
const norm = (s) => String(s||'').toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g,'')
  .replace(/[^a-z0-9]+/g,' ').trim();

// ---- Damerau-Levenshtein (bounded use) -------------------------------------
function damlev(a, b) {
  const m=a.length, n=b.length;
  if (!m) return n; if (!n) return m;
  const d = Array.from({length:m+1},(_,i)=>{const row=new Array(n+1).fill(0);row[0]=i;return row;});
  for (let j=0;j<=n;j++) d[0][j]=j;
  for (let i=1;i<=m;i++) for (let j=1;j<=n;j++){
    const cost = a[i-1]===b[j-1]?0:1;
    d[i][j]=Math.min(d[i-1][j]+1, d[i][j-1]+1, d[i-1][j-1]+cost);
    if (i>1&&j>1&&a[i-1]===b[j-2]&&a[i-2]===b[j-1]) d[i][j]=Math.min(d[i][j], d[i-2][j-2]+1);
  }
  return d[m][n];
}
const editSim = (a,b) => { const L=Math.max(a.length,b.length); return L?1-damlev(a,b)/L:1; };

// ---- character-trigram cosine (orthographic shape) -------------------------
function trigrams(s){ const t=' '+norm(s).replace(/ /g,' ')+' '; const m=new Map();
  for (let i=0;i<t.length-2;i++){ const g=t.slice(i,i+3); m.set(g,(m.get(g)||0)+1);} return m; }
function cosMap(A,B){ let dot=0,na=0,nb=0; for(const v of A.values())na+=v*v; for(const v of B.values())nb+=v*v;
  for(const[k,v]of A){ const w=B.get(k); if(w)dot+=v*w; } return (na&&nb)?dot/Math.sqrt(na*nb):0; }

// ---- LENS A: orthographic coherence c_o(query, title) ----------------------
// max( char-trigram cosine , best token-pair edit-similarity for tokens len>=3 )
function lensOrtho(query, title){
  const co = cosMap(trigrams(query), trigrams(title));
  const qt = norm(query).split(' ').filter(t=>t.length>=3);
  const tt = norm(title).split(' ').filter(t=>t.length>=3);
  let best=0; for(const a of qt) for(const b of tt) best=Math.max(best, editSim(a,b));
  return Math.max(co, best);
}

// ---- LENS B: lexical grounding g(query, extract) ---------------------------
// Count of query content-tokens (len>=4) that appear EXACTLY in the extract's defining (first)
// sentence — encoder-free evidence the article is about what was asked. Conservative: counts only if
// >=2 hits OR one long word (>=7). This is the on-device semantic arm (the real e5 encoder is unloaded
// during the online fetch to free heap for TLS), identical in firmware (coh_grounding) and sim.
function lensGround(query, extract){
  const fs = String(extract||'').split(/(?<=[.!?])\s/)[0] || extract || '';
  const qt = [...new Set(norm(query).split(' ').filter(w=>w.length>=4))];
  const et = new Set(norm(fs).split(' ').filter(Boolean));
  let hits=0, lng=false;
  for(const w of qt) if(et.has(w)){ hits++; if(w.length>=7) lng=true; }
  return (hits>=2||lng)?hits:0;
}

// ---- SC3 fused coherence + conformal self-calibrated gate ------------------
// kappa = orthographic coherence (the calibrated axis); grounding is an independent OR-rescue.
const kappa = (q,t,e) => lensOrtho(q,t);

// Build the calibration distribution from the device's OWN accepted cards: each card contributes
// kappa(alias, its-own-title, its-own-extract) for the user-phrasing aliases (what a real recall
// looks like). These are, by construction, coherent -> they define "what coherent means HERE".
function calibrate(cards){
  const ks=[];
  for(const c of cards) for(const a of (c.aliases||[])) ks.push(kappa(a, c.title, c.extract));
  return ks.sort((x,y)=>x-y);
}

// Empirical-Bayes shrinkage of the conformal quantile to a conservative prior at cold start, so a
// near-empty ODD can't accept junk. N0 = trust mass of the prior; alpha = NP risk knob (fraction of
// known-good we're willing to fall below). Higher alpha = stricter learning (asymmetric, by design).
function gate(q, t, e, calib, { alpha=0.30, priorFloor=0.42, N0=8, autoHi=0.86 } = {}){
  const k = kappa(q,t,e);
  if (k >= autoHi) return { accept:true, k, p:1, thr:autoHi, why:'near-exact (auto)' };
  const N = calib.length;
  // empirical alpha-quantile of known-good coherence
  const idx = Math.min(N-1, Math.max(0, Math.floor(alpha*(N-1))));
  const emp = N ? calib[idx] : priorFloor;
  // shrink: blend prior floor and empirical quantile by evidence mass
  const thr = (N0*priorFloor + N*emp) / (N0 + N);
  // conformal p-value (one-sided): is k at least as coherent as known-good?
  const p = (1 + calib.filter(x=>x<=k).length) / (1 + N);
  const g = lensGround(q, e);                                  // LENS B: independent grounding rescue
  return { accept: (k >= thr) || (g > 0), k, g, p, thr, why: `co=${k.toFixed(2)} thr=${thr.toFixed(2)} g=${g}` };
}

// ---- realistic calibration set (mirrors the live ODD) ----------------------
const CARDS = [
  { title:'Albert Einstein', extract:'Albert Einstein è stato un fisico tedesco naturalizzato svizzero e statunitense.', aliases:['einstein','albert einstein','chi è einstein'] },
  { title:'Donald Trump', extract:'Donald John Trump è un politico e imprenditore statunitense, 47º presidente degli Stati Uniti.', aliases:['trump','donald trump'] },
  { title:'Silvio Berlusconi', extract:'Silvio Berlusconi è stato un imprenditore e politico italiano, fondatore di Forza Italia.', aliases:['berlusconi','silvio berlusconi'] },
  { title:'Batman', extract:'Batman è un personaggio dei fumetti creato da Bob Kane e Bill Finger, pubblicato dalla DC Comics.', aliases:['batman'] },
  { title:'Fotosintesi clorofilliana', extract:'La fotosintesi clorofilliana è un processo bioenergetico con cui alcuni organismi producono sostanze organiche dalla luce.', aliases:['fotosintesi','cos\'è la fotosintesi'] },
  { title:'Napoleone Bonaparte', extract:'Napoleone Bonaparte è stato un politico e generale francese, fondatore del Primo Impero francese.', aliases:['napoleone'] },
  { title:'Ada Lovelace', extract:'Ada Lovelace è stata una matematica inglese, nota per il lavoro sulla macchina analitica di Babbage.', aliases:['ada lovelace'] },
];

// ---- test set: (query, title, extract, expected) ---------------------------
const TESTS = [
  // POSITIVES (should ACCEPT)
  ['einstein','Albert Einstein','Albert Einstein è stato un fisico tedesco...', true],
  ['enstein','Albert Einstein','Albert Einstein è stato un fisico tedesco...', true],      // typo
  ['berluscono','Silvio Berlusconi','Silvio Berlusconi è stato un imprenditore e politico italiano...', true], // typo
  ['trump','Donald Trump','Donald John Trump è un politico statunitense...', true],
  ['torre eiffel','Torre Eiffel','La torre Eiffel è una torre di Parigi, simbolo della città.', true],
  ['fotosintesi','Fotosintesi clorofilliana','La fotosintesi clorofilliana è un processo bioenergetico...', true],
  ['presidente americano','Presidente degli Stati Uniti d\'America','Il presidente degli Stati Uniti d\'America è il capo di Stato e di governo.', true], // descriptive, ortho fires
  ['chi ha inventato la relativita','Albert Einstein','Albert Einstein è il fisico che sviluppò la teoria della relatività.', true], // descriptive, semantic fires
  // NEGATIVES (should REJECT)
  ['berluscono','Politica italiana','La politica italiana si svolge nel quadro di una repubblica parlamentare.', false], // THE LIVE BUG
  ['asdfgh','Associazione Sportiva Dilettantistica','Una associazione sportiva dilettantistica è un ente senza scopo di lucro.', false],
  ['xyzqw','Xilofono','Lo xilofono è uno strumento musicale a percussione.', false],
  ['presidente americano','Politica italiana','La politica italiana si svolge nel quadro di una repubblica parlamentare.', false],
  ['berluscono','Bernardo Provenzano','Bernardo Provenzano è stato un criminale italiano, capo di Cosa Nostra.', false],
];

const calib = calibrate(CARDS);
console.log(`${C.b}SC3 — Self-Calibrated Coherence Cross${C.x}`);
console.log(`${C.d}calibration: ${calib.length} known-good pairs, kappa range [${calib[0].toFixed(2)}..${calib[calib.length-1].toFixed(2)}], median ${calib[Math.floor(calib.length/2)].toFixed(2)}${C.x}\n`);

let tp=0,tn=0,fp=0,fn=0;
console.log(`${C.b}query                 -> title                              exp  got     c_o   thr   ground  via${C.x}`);
for (const [q,t,e,exp] of TESTS){
  const r=gate(q,t,e,calib);
  const ok = r.accept===exp;
  if(exp&&r.accept)tp++; if(!exp&&!r.accept)tn++; if(!exp&&r.accept)fp++; if(exp&&!r.accept)fn++;
  const mark = ok?`${C.g}✓${C.x}`:`${C.r}✗${C.x}`;
  const decid = r.accept?`${C.g}LEARN ${C.x}`:`${C.y}reject${C.x}`;
  const via = r.accept ? (r.k>=0.86?'auto':(r.k>=r.thr?'ortho':'ground')) : '-';
  console.log(`${mark} ${q.padEnd(20).slice(0,20)} -> ${t.padEnd(34).slice(0,34)} ${exp?'Y':'N'}   ${decid} ${r.k.toFixed(2)}  ${r.thr.toFixed(2)}  ${String(r.g).padStart(4)}   ${via}`);
}
console.log(`\n${C.b}TP=${tp} TN=${tn} ${fp?C.r:C.g}FP(false-learn)=${fp}${C.x} ${fn?C.y:C.g}FN(false-reject)=${fn}${C.x}`);
console.log(`${C.d}FP is the poison case (must be 0). FN is tolerable (re-fetches next time).${C.x}`);
process.exit(fp>0?1:0);
