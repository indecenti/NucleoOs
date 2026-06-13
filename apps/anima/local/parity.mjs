// ANIMA Local — fidelity certification.
//
// Proves the in-browser WASM cascade is an EXACT copy of the device offline layer by running
// the SAME knowledge pack through BOTH the WASM module and the reference host build (anima.exe,
// which is the firmware cascade compiled natively with MinGW) and asserting identical replies.
// Both are built with -DANIMA_HOST and no env overrides, so they use the device's default gates
// and the flat index — the WASM is literally the same C, just compiled to wasm32 instead of x86.
//
//   node apps/anima/local/parity.mjs
//
// Exit 0 = every query matched (certified). Exit 1 = at least one divergence (printed).
import { readFileSync, readdirSync, statSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..', '..');
const sdRoot = join(repo, 'tools', 'anima-host', 'sd');               // the brain anima.exe reads
const exe    = join(repo, 'tools', 'anima-host', 'build', 'anima.exe');
const wasmMjs = join(here, '..', 'www', 'local', 'anima-local.mjs');

if (!existsSync(exe))     { console.error('anima.exe not found — run tools/anima-host/build.ps1'); process.exit(2); }
if (!existsSync(wasmMjs)) { console.error('anima-local.mjs not found — run apps/anima/local/build.ps1'); process.exit(2); }

// The browser WASM runs PC-grade (full rerank pool M=64, set via setenv in wasm_main.c). Match it on the
// native side so this certifies "WASM PC-grade == native PC-grade" — anima.exe reads L1_PFM from the env.
process.env.L1_PFM = '64';

// ---- load the WASM module (ES6 factory) and mount the brain into MEMFS ----
const AnimaLocal = (await import(pathToFileURL(wasmMjs).href)).default;
const M = await AnimaLocal();

function mkdirp(vpath) {
    let cur = '';
    for (const p of vpath.split('/').filter(Boolean)) { cur += '/' + p; try { M.FS.mkdir(cur); } catch {} }
}
function mount(localDir, vdir) {
    mkdirp(vdir);
    for (const name of readdirSync(localDir)) {
        const lp = join(localDir, name), vp = vdir + '/' + name;
        if (statSync(lp).isDirectory()) mount(lp, vp);
        else M.FS.writeFile(vp, new Uint8Array(readFileSync(lp)));
    }
}
mount(sdRoot, '/sd');

const init   = M.cwrap('anima_init', 'number', ['string']);
const reset  = M.cwrap('anima_reset', null, []);
const queryJ = M.cwrap('anima_query_json', 'string', ['string', 'string']);
init('it');
init('en');   // load both packs once; the cascade picks per-query by the lang arg

// ---- reference: drive anima.exe in interactive mode, one reset per query ----
function exeReply(q, lang) {
    const input = (lang === 'en' ? '/en\n' : '/it\n') + '/reset\n' + q + '\n';
    let out = '';
    try { out = execFileSync(exe, [], { input, encoding: 'utf8', stdio: ['pipe', 'pipe', 'ignore'] }); }
    catch (e) { out = (e.stdout || '').toString(); }
    const m = out.match(/^ {3}reply: (.*)$/m);
    let r = m ? m[1] : '';
    if (r === '(vuoto)') r = '';
    return r.trim();
}
function wasmReply(q, lang) {
    reset();
    const r = JSON.parse(queryJ(q, lang || 'it'));
    return (r.reply || '').trim();
}

// ---- representative, session-independent queries across the tiers (no live-state / follow-ups) ----
const Q = [
    ['it', 'apri la fotocamera'],            // L0 launch
    ['it', 'apri le impostazioni'],          // L0 launch
    ['it', 'apri la calcolatrice'],          // L0 launch via command-pack (commands.it.json)
    ['it', 'apri il calendario'],            // L0 launch
    ['it', 'apri il browser'],               // L0 launch
    ['it', 'apri la radio'],                 // L0 launch
    ['it', 'apri il terminale'],             // L0 launch
    ['en', 'open the calculator'],           // L0 launch (en)
    ['it', 'cos’è la fotosintesi'],// L1 fact
    ['en', 'what is photosynthesis'],        // L1 fact (en)
    ['it', 'cos’è python'],        // L1 fact (tech)
    ['it', 'capitale della francia'],        // KGE / facet
    ['it', 'chi ha scritto la divina commedia'], // KGE inverse
    ['it', 'quando è nato einstein'],   // KGE forward
    ['it', 'che lavoro faceva einstein'],    // facet occupation
    ['it', 'in che continente è la francia'], // KGE transitive
    ['it', 'quanto fa 12 per 8'],            // solver
    ['it', 'radice quadrata di 144'],        // solver
    ['en', 'what is the capital of japan'],  // KGE (en)
    ['it', 'traduci cane in inglese'],       // translate
    ['it', 'cos’è un buco nero'],  // L1 fact (astronomy)
    ['it', 'asdkfj qwerty zzz'],             // OOD -> both must abstain identically
];

let pass = 0, fail = 0;
const fails = [];
for (const [lang, q] of Q) {
    const e = exeReply(q, lang);
    const w = wasmReply(q, lang);
    const ok = e === w;
    if (ok) pass++; else { fail++; fails.push({ q, lang, exe: e, wasm: w }); }
    const tag = ok ? 'OK ' : 'XX ';
    console.log(`${tag} [${lang}] ${q}`);
    if (!ok) {
        console.log(`      exe : ${JSON.stringify(e)}`);
        console.log(`      wasm: ${JSON.stringify(w)}`);
    }
}
console.log(`\n=== parity: ${pass}/${Q.length} identical, ${fail} divergent ===`);
process.exit(fail === 0 ? 0 : 1);
