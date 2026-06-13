// Anti-regression gate: a CURATED, hand-verified golden set of NL requests (IT+EN) run
// against the LIVE parser extracted from the app. Exits non-zero on any regression.
// Run: node tools/spread-nlu/regress.mjs   (add `-v` to list every case)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const DIR = path.dirname(fileURLToPath(import.meta.url));
const HTML = fs.readFileSync(path.join(DIR, '..', '..', 'apps', 'spreadsheet', 'www', 'index.html'), 'utf8');
function grab(name) { const a = HTML.indexOf('function ' + name + '('); if (a < 0) return ''; let i = HTML.indexOf('{', a), d = 0; for (let j = i; j < HTML.length; j++) { if (HTML[j] === '{') d++; else if (HTML[j] === '}') { d--; if (d === 0) return HTML.slice(a, j + 1); } } }
const localIntent = new Function([grab('extractCol'), grab('localIntent')].join('\n') + '\nreturn localIntent;')();
const lab = r => { if (!r) return 'remote'; switch (r.type) { case 'agg': return 'agg_' + ({ SUM: 'sum', AVERAGE: 'avg', MIN: 'min', MAX: 'max', COUNT: 'count', PRODUCT: 'product', MEDIAN: 'median' }[r.fn] || r.fn.toLowerCase()); case 'transform': return 'transform_' + r.mode; case 'numfmt': return 'numfmt_' + r.kind; case 'format': return 'format_' + r.fmt; default: return r.type; } };

// ---- GOLDEN SET (label: [utterances...]) — every case hand-verified ----
const G = {
  agg_sum: ['somma la colonna B', 'sum column A', "qual e il totale di A", "what's the total of column B", 'fai la somma dei valori', 'add up the numbers in C', 'calcola somma di colonna A', 'sommami i valori selezionati'],
  agg_avg: ['media della colonna C', 'average of column D', "qual e la media di B", 'mean of column B', 'fammi la media di A'],
  agg_max: ['il massimo di A', 'the highest value in column A', 'valore piu alto di B', 'max of column C', 'qual e il numero piu grande in D'],
  agg_min: ['il minimo di B', 'lowest value in column A', 'valore piu basso di C', 'min of column D', 'the smallest number in B'],
  agg_count: ['conta le righe', 'how many rows are there', 'quante celle ci sono', 'count the values in B', 'numero di righe', 'how many values in column B', 'conta quante celle piene'],
  agg_product: ['moltiplica i valori della colonna A', 'product of column B'],
  agg_median: ['la mediana di B', 'median of column C'],
  describe: ['analizza la selezione', 'describe this data', 'dammi le statistiche', 'summarize this data', 'fammi un riepilogo'],
  insights: ['trova le anomalie', 'give me insights', 'any correlations in this data', 'scopri pattern nei dati', 'cosa noti in questi numeri'],
  chart: ['fai un grafico della colonna B', 'make a bar chart of column A', 'grafico a linee della colonna C', 'plot column D', 'visualizza i dati con un istogramma'],
  total: ['aggiungi una riga totali', 'add a total row', 'totali per colonna', 'metti una riga con i totali sotto i dati', 'put a sum row under the data for all columns', 'somma di ogni colonna in fondo'],
  dedupe: ['rimuovi i duplicati', 'remove duplicate rows', 'elimina le righe doppie', 'cancella le righe che si ripetono', 'get rid of the repeated rows', 'togli le copie'],
  rmempty: ['rimuovi le righe vuote', 'remove empty rows', 'cancella le righe senza dati', 'delete all blank rows', 'togli le righe che non contengono nulla'],
  clean: ['pulisci i dati', 'trim the spaces', 'rimuovi gli spazi in piu', 'normalize whitespace in column B', 'togli i doppi spazi da A'],
  transform_upper: ['metti la colonna A in maiuscolo', 'make this uppercase', 'tutto in maiuscolo'],
  transform_lower: ['metti in minuscolo', 'lowercase column B', 'rendi tutto minuscolo'],
  numfmt_currency: ['formatta come valuta', 'format as currency', 'metti in euro la colonna B'],
  numfmt_percent: ['formatta come percentuale', 'format these as percent', 'mostra come percentuali'],
  format_bold: ['metti in grassetto', 'make the cells bold', 'rendi grassetto'],
  sort: ['ordina per colonna B', 'sort descending', 'ordina alfabeticamente', 'sort column C ascending', 'metti in ordine decrescente'],
  highlight: ['evidenzia i valori sopra 100', 'highlight values below 50', 'evidenzia i duplicati', 'highlight the empty cells', 'evidenzia il massimo', 'highlight the max value', 'colora le celle maggiori di 10'],
  fill: ['riempi con 1,2,3', 'fill from 1 to 10', 'compila la serie', 'genera una serie di numeri'],
  formula: ['somma se la colonna A e maggiore di 10', 'count if greater than 5', 'media se > 100', 'quanti valori in B superano 5', 'somma i valori di A che superano 30'],
  enrich: ['aggiungi la capitale di ogni paese in colonna A', 'add the capital of each country in A', "metti l'anno di nascita di ogni persona", 'arricchisci con il continente di colonna B', 'popola la colonna con la valuta di ogni nazione'],
  explain: ['spiega questa formula', 'explain this formula', 'cosa fa questa cella'],
  find: ["trova 'pera'", 'cerca il valore 500', 'find orange', "dove c'e 2023", 'search for cliente X'],
  help: ['cosa sai fare', 'what can you do', 'aiuto', 'help', 'come funzioni'],
  knowledge: ['quanto fa 12 per 8', 'chi e Einstein', 'capitale della Francia', 'che ore sono', 'how much is 15% of 240', 'what is the capital of Spain'],
  chitchat: ['ciao', 'raccontami una barzelletta', 'come stai', 'grazie mille'],
};
const correct = (exp, pred) => (exp === 'knowledge' || exp === 'chitchat') ? pred === 'remote' : pred === exp;

let ok = 0, n = 0; const miss = [];
for (const [label, list] of Object.entries(G)) for (const u of list) { n++; const p = lab(localIntent(u)); if (correct(label, p)) { ok++; if (process.argv.includes('-v')) console.log('  ok  ', label.padEnd(16), u); } else miss.push({ label, u, p }); }
console.log(`\nGOLDEN REGRESSION: ${ok}/${n} = ${(ok / n * 100).toFixed(1)}%`);
if (miss.length) { console.log('\nREGRESSIONS:'); for (const m of miss) console.log(`  ✗ [${m.label} -> ${m.p}] ${m.u}`); process.exitCode = 1; }
else console.log('✓ all golden cases pass');
