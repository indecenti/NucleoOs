// ANIMA Local — fidelity certification (an entry of `npm run anima:gate`).
//
// The browser engine (apps/anima/www/local/anima-local.{mjs,wasm}) is compiled DIRECTLY from
// firmware/components/nucleo_anima, like the host harness's anima.exe (see build.ps1 / engine-src.mjs).
// This test certifies two things:
//
//   1. FRESH  — the module's compiled-in build id equals the fingerprint of the sources on disk. A firmware
//               engine edit (or a shared shim/stub edit) that was not rebuilt into the WASM fails HERE,
//               even if none of the queries below happens to notice. Fix: run apps/anima/local/build.ps1.
//   2. SAME   — the same knowledge pack through BOTH the WASM module and anima.exe gives identical replies.
//               anima.exe runs with the browser's own runtime knobs (read back from the module via
//               anima_knobs), so this is "browser engine == native engine" in the browser's configuration.
//
//   node apps/anima/local/parity.mjs
//
// Exit 0 = fresh and every query matched (certified). Exit 1 = stale module or a divergence (printed).
import { readFileSync, readdirSync, statSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { engineHash } from './engine-src.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..', '..');
const sdRoot = join(repo, 'tools', 'anima-host', 'sd');               // the brain anima.exe reads
const exe    = join(repo, 'tools', 'anima-host', 'build', 'anima.exe');
const wasmMjs = join(here, '..', 'www', 'local', 'anima-local.mjs');

if (!existsSync(exe))     { console.error('anima.exe not found — run tools/anima-host/build.ps1'); process.exit(2); }
if (!existsSync(wasmMjs)) { console.error('anima-local.mjs not found — run apps/anima/local/build.ps1'); process.exit(2); }

// ---- load the WASM module (ES6 factory) and mount the brain into MEMFS ----
const AnimaLocal = (await import(pathToFileURL(wasmMjs).href)).default;
const M = await AnimaLocal();

// ---- 1. FRESH: the module must be built from the sources on disk ----
const built = M.ccall('anima_build_id', 'string', [], []);
const want = engineHash();
if (built !== want) {
    console.error(`XX  STALE WASM ENGINE: anima-local was built from sources ${built}, the tree is now ${want}.`);
    console.error('    firmware/components/nucleo_anima (or a shared host shim/stub) changed since the last build.');
    console.error('    Rebuild:  powershell -NoProfile -ExecutionPolicy Bypass -File apps/anima/local/build.ps1');
    console.log('\n=== parity: STALE build (0 queries run) ===');
    process.exit(1);
}
console.log(`OK  build id ${built} matches firmware/components/nucleo_anima`);

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

// ---- 2. SAME: replay the browser's runtime knobs on the native side ----
const knobs = Object.fromEntries(M.ccall('anima_knobs', 'string', [], []).split('\n').filter(Boolean)
    .map((l) => [l.slice(0, l.indexOf('=')), l.slice(l.indexOf('=') + 1)]));
// Hermetic: drop any engine knob inherited from the caller's shell (ANIMA_*, L0_*, L1_* — e.g. a leftover
// ANIMA_SD_ROOT or ANIMA_L1_EXACT) so the native side sees exactly what the WASM sees, nothing more.
const exeEnv = Object.fromEntries(Object.entries(process.env).filter(([k]) => !/^(ANIMA_|L0_|L1_)/i.test(k)));
Object.assign(exeEnv, knobs);
console.log(`    knobs (browser == native): ${Object.entries(knobs).map(([k, v]) => k + '=' + v).join(' ')}`);

// ---- reference: drive anima.exe in interactive mode, one reset per query ----
function exeReply(q, lang) {
    const input = (lang === 'en' ? '/en\n' : '/it\n') + '/reset\n' + q + '\n';
    let out = '';
    try { out = execFileSync(exe, [], { input, encoding: 'utf8', env: exeEnv, stdio: ['pipe', 'pipe', 'ignore'] }); }
    catch (e) { out = (e.stdout || '').toString(); }
    // The reply runs to the blank line that closes the block — multi-line replies (formulas, lists) included.
    const text = out.replace(/\r/g, '');
    const at = text.search(/^ {3}reply: /m);
    let r = at < 0 ? '' : text.slice(at + '   reply: '.length).split('\n\n')[0];
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
