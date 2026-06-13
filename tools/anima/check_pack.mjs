#!/usr/bin/env node
// ANIMA pack-coherence guard — catches the class of "shipped a broken brain" bugs that the
// build pipeline (build_akb2.py via ANIMA_ENC → augment_akb4.py → deploy.ps1) cannot, because
// the encoder and the index are built/synced as SEPARATE files. The two live bugs this guards:
//
//   1) DIM MISMATCH — encoder ANE2 dim != index AKB3 D. The firmware load_index() rejects on
//      d != s_D (nucleo_anima_l1.c) and L1 is SILENTLY DISABLED. This happens when the encoder
//      is re-distilled at a new dim but a stale index from the old dim is left in a tree.
//   2) MISSING ASIG — a device index without the AKB4 sign-signature trailer falls back to the
//      slow brute-force exact path (the popcount prefilter is OFF), ~290 KB scattered SD reads
//      per query instead of a few KB. On the host packs it also makes l1-parity/l1-recall vacuous
//      (exact-vs-exact), so a real prefilter regression could pass unnoticed.
//
// For every SD tree we ship, assert: encoder.dim == index.D, the encoder.bin dim matches its
// .json sidecar, and (for AKB4-expecting trees) the index carries a valid ASIG footer whose
// sig_bits == D. Exit non-zero on any violation. Wire into `npm run anima:gate` and flash.ps1.
//
//   3) STALE FIXTURE (anti-drift) — the index is a deterministic function of (corpus, encoder),
//      but it is a SEPARATE committed file. The live failure mode that flips borderline gate
//      cases is NOT a non-reproducible build (the build is deterministic: random_state=0,
//      thread-invariant, sorted corpus load) — it is a SILENT stale fixture: someone edits the
//      knowledge corpus but does not rebuild + re-sync the index, so the gate keeps scoring an
//      index that no longer matches the corpus. Green now; a surprise k-means reshuffle the
//      moment `anima:packs` is finally run. build_akb2 stamps every index with a `.prov` sidecar
//      (corpus+encoder sha); here we recompute both and FAIL if the committed index is stale,
//      turning that silent drift into a deterministic, actionable red.
import { readFileSync, existsSync, statSync, readdirSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..');
const KDIR = join(here, 'knowledge');           // tools/anima/knowledge — the single corpus source

// Corpus content hash — MUST stay byte-identical to build_akb2.py:_corpus_sha() (sorted by
// basename; per file: name + NUL + bytes + LF). Knowledge filenames are ASCII, so JS's default
// (UTF-16 code-unit) sort and Python's (code-point) sort agree.
function corpusSha() {
  const files = existsSync(KDIR) ? readdirSync(KDIR).filter((f) => f.endsWith('.jsonl')).sort() : [];
  const h = createHash('sha256');
  for (const name of files) {
    h.update(name, 'utf8'); h.update(Buffer.from([0]));
    h.update(readFileSync(join(KDIR, name))); h.update(Buffer.from([0x0a]));
  }
  return { sha: h.digest('hex'), n: files.length };
}
const fileSha = (p) => createHash('sha256').update(readFileSync(p)).digest('hex');
const C = { g: '\x1b[32m', r: '\x1b[31m', y: '\x1b[33m', d: '\x1b[2m', b: '\x1b[1m', x: '\x1b[0m' };

// Trees that ship to a DEVICE must carry ASIG (the prefilter). The host harness tree should too,
// so the l1-parity / l1-recall prefilter gates actually exercise the fast path instead of passing
// vacuously. expectDim is advisory (warn-only): device encoder is D=192, host harness is D=256.
const TREES = [
  { name: 'deploy/sd',           dir: 'deploy/sd/data/anima',            asig: true, expectDim: 192 },
  { name: 'deploy/sd-safe',      dir: 'deploy/sd-safe/data/anima',       asig: true, expectDim: 192 },
  { name: 'tools/sd-sim',        dir: 'tools/sd-sim/data/anima',         asig: true, expectDim: 192 },
  { name: 'tools/anima-host/sd', dir: 'tools/anima-host/sd/data/anima',  asig: true, expectDim: 256 },
];

function readEncoder(p) {
  // ANE2 header: [4 magic][u32 rows][u32 dim][...]
  const fd = readFileSync(p);
  if (fd.subarray(0, 4).toString('latin1') !== 'ANE2') throw new Error(`bad encoder magic in ${p}`);
  return { rows: fd.readUInt32LE(4), dim: fd.readUInt32LE(8), bytes: fd.length };
}
function readIndex(p) {
  // AKB3 header: [4 magic 'AKB3'][u32 D][u32 K][u32 N]; optional AKB4 trailer at EOF-16:
  // ['ASIG'][u32 sig_off][u32 sig_bits=D][u32 version]
  const fd = readFileSync(p);
  if (fd.subarray(0, 4).toString('latin1') !== 'AKB3') throw new Error(`bad index magic in ${p}`);
  const D = fd.readUInt32LE(4), K = fd.readUInt32LE(8), N = fd.readUInt32LE(12);
  let asig = null;
  if (fd.length >= 16 && fd.subarray(fd.length - 16, fd.length - 12).toString('latin1') === 'ASIG') {
    asig = { sig_off: fd.readUInt32LE(fd.length - 12), sig_bits: fd.readUInt32LE(fd.length - 8),
             version: fd.readUInt32LE(fd.length - 4) };
  }
  return { D, K, N, asig, bytes: fd.length };
}

let violations = 0, warnings = 0;
console.log(`${C.b}=== ANIMA pack-coherence guard ===${C.x}`);
const corpus = corpusSha();                       // computed ONCE — the same corpus feeds every tree
console.log(`  ${C.d}corpus: ${corpus.n} knowledge file(s), sha ${corpus.sha.slice(0, 12)}…${C.x}`);
for (const t of TREES) {
  const encP = join(repo, t.dir, 'anima-it-encoder.bin');
  const idxP = join(repo, t.dir, 'anima-it-index.bin');
  const jsonP = join(repo, t.dir, 'anima-it-encoder.json');
  if (!existsSync(encP) || !existsSync(idxP)) {
    console.log(`  ${C.y}SKIP${C.x} ${t.name.padEnd(20)} ${C.d}(no encoder/index present)${C.x}`);
    continue;
  }
  let enc, idx, jdim = null;
  try { enc = readEncoder(encP); idx = readIndex(idxP); } catch (e) {
    console.log(`  ${C.r}FAIL${C.x} ${t.name.padEnd(20)} ${e.message}`); violations++; continue;
  }
  if (existsSync(jsonP)) { try { jdim = JSON.parse(readFileSync(jsonP, 'utf8')).dim ?? null; } catch {} }

  const problems = [];
  if (enc.dim !== idx.D) problems.push(`DIM MISMATCH enc=${enc.dim} idx=${idx.D} → load_index() rejects, L1 DISABLED`);
  if (jdim !== null && jdim !== enc.dim) problems.push(`encoder.json dim=${jdim} != encoder.bin dim=${enc.dim}`);
  if (t.asig) {
    if (!idx.asig) problems.push(`NO ASIG footer → AKB4 prefilter OFF (brute-force exact path)`);
    else if (idx.asig.sig_bits !== idx.D) problems.push(`ASIG sig_bits=${idx.asig.sig_bits} != D=${idx.D}`);
  }
  // ANTI-DRIFT: is this committed index a fresh build of the CURRENT corpus + this tree's encoder?
  let provTag = '';
  const provP = idxP + '.prov';
  if (!existsSync(provP)) {
    provTag = ` ${C.y}(no .prov — rebuild via npm run anima:packs to stamp provenance)${C.x}`; warnings++;
  } else {
    let prov = null; try { prov = JSON.parse(readFileSync(provP, 'utf8')); } catch {}
    if (!prov || !prov.corpus_sha) {
      problems.push(`unreadable .prov sidecar → cannot verify freshness; rebuild via npm run anima:packs`);
    } else {
      if (prov.corpus_sha !== corpus.sha)
        problems.push(`STALE INDEX: built from corpus ${prov.corpus_sha.slice(0,12)}… but current corpus is ` +
                      `${corpus.sha.slice(0,12)}… → rebuild + re-sync (npm run anima:packs${t.expectDim===256?' --host':''}) and re-validate goldens`);
      if (prov.encoder_sha !== fileSha(encP))
        problems.push(`STALE INDEX: built from a different encoder than ${t.dir}/anima-it-encoder.bin → rebuild`);
      if (prov.corpus_sha === corpus.sha) provTag = ` ${C.d}prov✓${C.x}`;
    }
  }
  const dimWarn = t.expectDim && enc.dim !== t.expectDim
    ? ` ${C.y}(expected dim ${t.expectDim})${C.x}` : '';
  if (dimWarn) warnings++;

  const desc = `enc ${enc.dim}d/${(enc.bytes/1e6).toFixed(1)}MB  idx D=${idx.D} K=${idx.K} N=${idx.N}` +
               `  ${idx.asig ? 'ASIG✓' : 'no-ASIG'}`;
  if (problems.length) {
    console.log(`  ${C.r}FAIL${C.x} ${t.name.padEnd(20)} ${desc}`);
    for (const p of problems) console.log(`         ${C.r}↳ ${p}${C.x}`);
    violations += problems.length;
  } else {
    console.log(`  ${C.g}PASS${C.x} ${t.name.padEnd(20)} ${desc}${dimWarn}${provTag}`);
  }
}

console.log('');
if (violations) {
  console.log(`${C.r}${C.b}PACK-COHERENCE FAILED:${C.x} ${violations} violation(s). ` +
              `Rebuild the offending index at the encoder's dim and run augment_akb4.py.`);
  process.exit(1);
}
console.log(`${C.g}${C.b}PACK COHERENT${C.x}${warnings ? ` ${C.y}(${warnings} dim warning(s))${C.x}` : ''} — every tree: encoder.dim == index.D, ASIG present.`);
process.exit(0);
