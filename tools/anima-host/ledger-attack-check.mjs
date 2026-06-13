#!/usr/bin/env node
// LEDGER ATTACK gate — replays the hostile attacks an adversarial audit found against VKL v1 and proves
// VKL v2 DEFEATS each one. Every assertion is "the attack is REJECTED/DETECTED". If any passes, the
// immutability/certainty claim is broken and the gate fails. Offline, deterministic.
//   forgery     forge+reseal is detected by the off-SD anchor; a wrong key breaks the chain
//   provenance  prefix-spoofed / fabricated / malformed src is rejected; wd: must resolve in the cache
//   certainty   conf type-coercion, empty fields, control chars, length bombs are rejected
//   canonical   the encoding is injective (100 ≠ "100"; no 0x01 delimiter injection)
//   soundness   abstract nodes are blocklisted; a vandalized CONCRETE target is not a certified type
import { readFileSync, writeFileSync, rmSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { Ledger, isCertain, canonical, linkHash, genesis, validSrc, wdIndexFrom, KEY } from '../anima/ledger.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const REPO = join(here, '..', '..');
const SD = join(here, 'sd', 'data', 'anima', 'learned');
const cache = JSON.parse(readFileSync(join(REPO, 'tools/anima/refdata/wd_cache.json'), 'utf8'));
const policy = JSON.parse(readFileSync(join(REPO, 'tools/anima/refdata/closure_policy.json'), 'utf8'));
const roots = new Set(policy.certified_roots), block = new Set(policy.blocklist);
const wdIndex = wdIndexFrom(cache);
const anchor = readFileSync(join(REPO, 'tools/anima/refdata/knowledge.head'), 'utf8').trim();
const shipped = readFileSync(join(SD, 'knowledge.ledger.jsonl'), 'utf8').split(/\r?\n/).filter(Boolean).map((l) => JSON.parse(l));
const C = { g: '\x1b[32m', r: '\x1b[31m', d: '\x1b[2m', b: '\x1b[1m', x: '\x1b[0m' };
const fails = [];
const ck = (cond, msg) => { if (!cond) fails.push(msg); console.log(`   ${cond ? C.g + '✓' : C.r + '✗'}${C.x} ${msg}`); };

const TMP = join(SD, 'evo', '.attack.ledger.jsonl');
const fresh = () => { writeFileSync(TMP, '', 'utf8'); return new Ledger(TMP, { wdIndex }); };
const real = shipped.find((e) => e.src.startsWith('wd:'));   // a genuine, cache-resolvable statement

console.log(`${C.b}=== ANIMA ledger-attack gate (VKL v2 must defeat every v1 break) ===${C.x}\n`);

// ---------- provenance: forged / malformed / unresolvable src must be REJECTED ----------
console.log(`${C.b}A. Provenienza (src forgiata/malformata/non risolvibile → RIFIUTATA)${C.x}`);
{
  const T = fresh();
  for (const src of ['wd:lol', 'wd:', 'wd:Q937', 'curated:i made this up', 'user:Bad Name', 'http://evil', '', 'wd:../../etc'])
    ck(T.append({ s: 'einstein', rel: 'occupation', o: 'astrologer', src, conf: 100 }) === null, `src '${src || '(vuoto)'}' rifiutata`);
  // shaped like Wikidata but the statement is NOT in the immutable cache → rejected (referential check)
  ck(T.append({ s: 'einstein', rel: 'occupation', o: 'astronaut', src: 'wd:Q937#P106:Q9999999', conf: 100 }) === null,
     "wd: ben formata ma FABBRICATA (QID non nel cache) rifiutata");
  // a genuine cache-backed statement IS accepted (control: the gate isn't rejecting everything)
  const fT = fresh();
  ck(!!fT.append({ s: real.s, rel: real.rel, o: real.o, src: real.src, conf: 100 })?.h, `fonte reale del cache '${real.src}' accettata`);
}

// ---------- certainty: type-coercion / empty / control / length must be REJECTED ----------
console.log(`\n${C.b}B. Certezza (coercizione conf / campi vuoti / control char / bomba lunghezza → RIFIUTATI)${C.x}`);
{
  const T = fresh(); const ok = (e) => T.append(e) === null;
  for (const conf of ['100', [100], '1e3', '0x46', 70.5, true, 200, 69, -1])
    ck(ok({ s: 'x', rel: 'isa', o: 'y', src: real.src, conf }), `conf=${JSON.stringify(conf)} rifiutata`);
  ck(ok({ s: '', rel: 'isa', o: 'y', src: real.src, conf: 100 }), 'soggetto vuoto rifiutato');
  ck(ok({ s: '  ', rel: '\t', o: '\n', src: real.src, conf: 100 }), 'campi whitespace/tab/newline rifiutati');
  ck(ok({ s: 'x', rel: 'isa', o: 'ab', src: real.src, conf: 100 }), 'control char (0x01) nel valore rifiutato');
  ck(ok({ s: 'x', rel: 'isa', o: 'z'.repeat(200), src: real.src, conf: 100 }), 'bomba di lunghezza (o>95B) rifiutata');
}

// ---------- canonical: injective encoding (100 ≠ "100"; no in-band delimiter) ----------
console.log(`\n${C.b}C. Canonicalizzazione iniettiva (tipi distinti, nessuna iniezione di delimitatore)${C.x}`);
{
  const base = { s: 'a', rel: 'r', o: 'o', src: 'wd:Q1#P1:Q2' };
  ck(canonical({ ...base, conf: 100 }) !== canonical({ ...base, conf: '100' }), 'conf numero 100 ≠ stringa "100" (niente collisione di tipo)');
  // two DIFFERENT field tuples must never produce the same canonical string (JSON escapes, no 0x01 slide)
  const a = canonical({ s: 'a', rel: 'r', o: 'b', src: 'c', conf: 1 });
  const b = canonical({ s: 'a', rel: 'r', o: 'bc', src: '', conf: 1 });
  ck(a !== b, 'iniezione di 0x01 non collassa due triple distinte sullo stesso hash');
  ck(linkHash('p', { ...base, conf: 100 }) !== linkHash('p', { ...base, conf: 99 }), 'link-hash cambia col contenuto');
}

// ---------- forgery: forge+reseal is DETECTED by the off-SD anchor; wrong key breaks the chain ----------
console.log(`\n${C.b}D. Forgiatura (forge+reseal rilevato dall'ancora; chiave sbagliata rompe la catena)${C.x}`);
{
  // copy the shipped ledger, tamper a PAST fact, then run the (shipped) reseal — the v1 laundering attack
  writeFileSync(TMP, shipped.map((e, i) => i === 2 ? { ...e, o: 'plumber' } : e).map((e) => JSON.stringify(e)).join('\n') + '\n', 'utf8');
  const forged = new Ledger(TMP, { wdIndex });
  await forged.reseal();                                   // internally-consistent chain over the lie...
  const v = await forged.verify({ anchor });               // ...but its head ≠ the off-SD pinned head
  ck(!v.ok && v.head !== anchor, `forge+reseal RILEVATO: head ${v.head.slice(0, 12)}… ≠ ancora ${anchor.slice(0, 12)}…`);

  // reseal must DROP an uncertain row, not bless it
  writeFileSync(TMP, [...shipped.map((e) => JSON.stringify(e)),
    JSON.stringify({ i: 999, s: 'vaccines', rel: 'cause', o: 'autism', src: 'wd:lol', conf: 100, h: 'deadbeef' })].join('\n') + '\n', 'utf8');
  const r2 = new Ledger(TMP, { wdIndex }); const rs = await r2.reseal();
  const after = readFileSync(TMP, 'utf8');
  ck(rs.dropped >= 1 && !after.includes('vaccines'), `reseal SCARTA la riga non-certa (dropped=${rs.dropped}), non la "benedice"`);

  // a wrong key cannot produce a valid chain (the attacker doesn't hold the device/build secret)
  const vWrong = await new Ledger(join(SD, 'knowledge.ledger.jsonl'), { key: 'attacker-key', wdIndex }).verify({ anchor });
  ck(!vWrong.ok, 'verifica con CHIAVE SBAGLIATA fallisce (la catena è autenticata, non solo hashata)');
  ck(genesis('attacker-key') !== genesis(KEY), 'anche il genesis dipende dalla chiave (niente radice pubblica condivisa)');
}

// ---------- soundness: abstract blocklisted; a vandalized CONCRETE target is not certified ----------
console.log(`\n${C.b}E. Soundness (nodi astratti in blocklist; bersaglio concreto vandalizzato non certificato)${C.x}`);
{
  for (const j of ['being', 'subject', 'proto-agent', 'individual', 'worker']) ck(block.has(j), `«${j}» è in blocklist (potato all'ingest)`);
  // even if a vandalized P279 edge 'scientist ⊂ robot' were ingested, "X is a robot" is not a CERTIFIED
  // type, so the output gate refuses to assert it — the allowlist defeats single-edge poisoning.
  for (const w of ['robot', 'vampire', 'criminal', 'deity', 'flat']) ck(!roots.has(w), `bersaglio vandalizzato «${w}» NON è un tipo certificato → non asseribile`);
  ck(roots.has('scientist') && roots.has('person') && roots.has('artist'), 'i tipi legittimi (scientist/person/artist) restano certificati');
}

try { rmSync(TMP); } catch { /* ignore */ }
console.log('');
if (fails.length) { console.log(`${C.r}${C.b}[ledger-attack] ${fails.length} ATTACK(S) NOT DEFEATED${C.x}`); process.exit(1); }
console.log(`${C.g}${C.b}[ledger-attack] tutti gli attacchi (forgery/provenance/certainty/canonical/soundness) RESPINTI${C.x}`);
